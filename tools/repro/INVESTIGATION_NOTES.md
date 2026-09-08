# Investigation notes — MergeInto/Insert Combine UAF

## Build environment status: BLOCKED

The harness for this run lacks a usable C++ toolchain:

```
$ which g++ clang++ ninja
(none)
$ apt-get install ...
E: Could not open lock file /var/lib/dpkg/lock-frontend ... Permission denied
$ which sudo
sudo: not found
```

Only `cmake`, `make`, and `python3` are present. The ASAN build attempt:

```
BUILD_UNITTEST=1 ENABLE_SANITIZER=1 CRASH_ON_ASSERT=1 GEN=ninja make debug -j4
```

fails immediately at the cmake configure step with:

```
CMake Error: CMake was unable to find a build program corresponding to "Ninja".
CMake Error: CMAKE_C_COMPILER not set, after EnableLanguage
CMake Error: CMAKE_CXX_COMPILER not set, after EnableLanguage
```

(captured in `tools/repro/build.log`).

Per the task's fallback policy, this PR consists of:

1. The reproducer test files
   (`test/sql/storage/merge_into_combine_uaf.test`,
   `test/api/test_merge_into_combine_uaf.cpp`).
2. `tools/repro/REPRODUCER.md` — how to build, how to run, expected
   ASAN signature, observed gdb evidence, lifetime hypothesis from
   code reading.
3. This file — environment limitations and code-reading-only
   analysis.

The hypothesis below was derived from reading the suspect files only.

## Sanity check of git history

Commits in the last 6 months touching the suspect file set
(`physical_merge_into.cpp`, `physical_insert.cpp`, `local_storage.cpp`,
`row_group_collection.cpp`, `optimistic_data_writer.cpp`,
`row_group.cpp`, `column_data.cpp`) were searched for keywords
`use-after-free`, `UAF`, `lifetime`, `merge.into`,
`optimistic.*combine`, `combine.*race`, `race`, `thread`. No commit
clearly fixes the SIGSEGV stack reported by the internal fork. The
closest matches are:

| commit | file | relevance |
| --- | --- | --- |
| `0085be43d5` Keep exact track of which row groups have not yet been flushed in the optimistic writer | `optimistic_data_writer.cpp` | reshapes the `unflushed_row_groups` set in the same Combine path. Could be related but does not explicitly target a UAF. |
| `e79a6b6c07` WAL corruption when reusing optimistically written blocks too optimistically | `optimistic_data_writer.cpp` | block reuse around optimistic writes. Different symptom (WAL corruption, not UAF). |
| `17e2a0e6c5` Only push append into the transaction after the append has succeeded | `local_storage.cpp` | rearranges Append + transaction registration ordering. Adjacent code, not a fix for this UAF. |
| `0f5ef6bfd1` Fix DELETE RETURNING for rows inserted in the same transaction | `local_storage.cpp` | unrelated symptom. |

The investigation already noted upstream `RowGroupReorderer` (PRs
22884 / 22911) is on the SCAN path only and is not on this code path.
That stands.

Conclusion: no commit obviously fixes this scenario on
`v1.5-variegata`. The bug is most likely still present.

## Hypothesis (one paragraph)

`PhysicalInsert` on the parallel path stores per-thread state in
`InsertLocalState`: a `TableAppendState local_append_state` whose
`row_group_append_state.row_group` is a raw `RowGroup *` pointing into
the per-thread `OptimisticWriteCollection`'s `RowGroupCollection`. In
`PhysicalInsert::Combine`'s "large" branch (`>= row_group_size`,
which fires once the optimistic collection has produced full row
groups — at ~6 row groups, i.e. ~737280 rows, matching the observed
`insert_count = 765003`), the per-thread collection's row-group
segments are moved-out into the shared `LocalTableStorage` via
`LocalStorage::LocalMerge` →
`OptimisticWriteCollection::MergeStorage` →
`RowGroupCollection::MergeStorage` (`MoveSegments()` +
`AppendSegment(std::move(row_group))`). After the move, any raw
pointer in `local_append_state` into the per-thread collection's
former segments is dangling — and `MergeStorage` even toggles
`SetRowGroupAppendMode(SUGGEST_NEW)` so the next `Append` goes
elsewhere. Within `PhysicalMergeInto`, multiple inner `Combine`s run
sequentially per local state, and the inner `PhysicalInsert::Sink` is
invoked again on the next pipeline chunk — that next `Append` walks
the now-stale `RowGroup *` and segfaults inside
`ColumnData::Append`. The string-slab reuse evidence
(`'memory structs o'`, `LogicalType::id_ = INVALID` consistent with a
default-constructed `LogicalType`, vptr replaced with a heap pointer
into `BufferAllocatorAllocate`) is consistent with the
`BufferAllocator` reissuing the freed `ColumnData` slab to a
`std::string` shortly after the destructor fires.

## Suggested next steps for whoever picks this up

1. Reproduce locally: build with `ENABLE_SANITIZER=1` and run the
   loop in `tools/repro/REPRODUCER.md`. Capture the freed-by stack —
   that is the load-bearing piece of evidence.
2. The freed-by stack should land in
   `~OptimisticWriteCollection` / `~RowGroupCollection` /
   `~RowGroup` / `~ColumnData`, dispatched from one of:
   - `OptimisticWriteCollection::MergeStorage`
     (`optimistic_data_writer.cpp:142`)
   - `LocalStorage::LocalMerge` (`local_storage.cpp:509`)
   - `LocalTableStorage::Rollback` (less likely — this is a success
     path crash).
3. If the freed-by stack lands somewhere else entirely (for example,
   a checkpoint thread freeing block buffers), the hypothesis above
   is wrong and the investigation should pivot to
   `BlockManager` / `StandardBufferManager` lifetime instead — note
   that the vptr was overwritten with a `BufferAllocatorAllocate`
   *function pointer*, which could also be coincidence (the
   `BufferAllocator` slab issuance code being adjacent in memory) or
   evidence that the slab is one the buffer manager itself is
   actively recycling.
4. The minimal fix candidates are likely:
   a. Reset `lstate.local_append_state.row_group_append_state.row_group`
      to `nullptr` after `LocalMerge` in
      `PhysicalInsert::Combine`'s large branch, so any subsequent
      mis-sequenced `Sink` call cannot dereference a stale pointer.
   b. Re-initialize `local_append_state` (call
      `collection.InitializeAppend(...)` again) on the new
      destination collection if MergeInto's Combine ordering can lead
      to another Sink after Combine on the same local state.
   c. Have `RowGroupCollection::MergeStorage` invalidate any
      `RowGroupAppendState`s that reference moved-out segments
      (probably overkill).

The code-reading evidence does not let us choose between (a) and (b)
without ASAN output. The freed-by stack will.
