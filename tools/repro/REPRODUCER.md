# MergeInto/Insert Combine UAF — reproducer

This directory contains a reproducer for a SIGSEGV / heap-use-after-free
observed at runtime inside `PhysicalMergeInto::Combine` while running

```sql
INSERT OR IGNORE INTO db.t SELECT ... FROM range(1_000_000);
CHECKPOINT;
```

The crash was originally observed in an internal fork at commit
`f8c4bc0e`. The corresponding upstream codebase is DuckDB
`v1.5-variegata`.

## Files

| File | Purpose |
| --- | --- |
| `test/sql/storage/merge_into_combine_uaf.test` | sqllogic reproducer (single-threaded harness, multi-thread executor via `PRAGMA threads=4`). |
| `test/api/test_merge_into_combine_uaf.cpp` | catch test that spawns 4 inserter threads + 1 checkpoint thread to exercise the parallel `OptimisticDataWriter` path more directly. |
| `tools/repro/asan_output.txt` | raw (untruncated) ASAN dump from a triggering run, OR a "no trigger observed" stub. |
| `tools/repro/INVESTIGATION_NOTES.md` | code-reading analysis of where the lifetime bug most likely lives. |
| `tools/repro/build.log` | build attempt log (kept for the record). |

## How to build and run

The reproducer needs the project's standard ASAN debug build:

```bash
BUILD_UNITTEST=1 ENABLE_SANITIZER=1 CRASH_ON_ASSERT=1 make debug -j
```

Run the sqllogic test:

```bash
./build/debug/test/unittest test/sql/storage/merge_into_combine_uaf.test
```

To stress-loop the sqllogic test:

```bash
for i in $(seq 1 30); do
    ./build/debug/test/unittest test/sql/storage/merge_into_combine_uaf.test \
        || { echo "ASAN trigger on iteration $i"; break; }
done
```

Run the threaded catch reproducer (it is tagged `[!hide]` so it does not
run by default):

```bash
./build/debug/test/unittest "[merge_into]"
```

## Expected ASAN signature

```
==NNNN==ERROR: AddressSanitizer: heap-use-after-free
    READ of size 8 at 0x... thread T?
    #0  ColumnData::Append
    #1  RowGroup::Append
    #2  RowGroupCollection::Append
    #3  LocalStorage::Append              (or DataTable::LocalAppend)
    #4  PhysicalInsert::Combine
    #5  MergeIntoGlobalState::Combine
    #6  PhysicalMergeInto::Combine
```

Freed-by stack is the load-bearing one — see hypothesis below.

## Observed evidence (from the original gdb session)

Inside `RowGroupCollection::Append` at the moment of crash:
- `last_row_group != current_row_group` (we had just hopped row groups).
- `gstate.insert_count = 765003` (~6 row groups @ 122880 rows each).

Inside the offending `ColumnData`:
- vptr pointed at `StandardBufferManager::BufferAllocatorAllocate`
  (i.e. not a vtable at all — the vptr slot's bytes had been reused as
  an arbitrary pointer-shaped value).
- `type.id_ = INVALID`
- `parent` and `compression` slots contained the ASCII bytes
  `'memory structs o'` (~16 bytes of a `std::string`).
- `allocation_size`, `count` were garbage.

Inside `PhysicalInsert::Combine`'s local `insert_chunk`:
- `Vector::size` and capacity garbage.

Diagnosis: classic heap-use-after-free where the freed slab was
recycled by the buffer allocator into a string allocation between the
free and the read. The garbage that ended up in the freed slab is just
"whatever happened to be allocated next". The interesting question is
**who freed it**, not **who reused the slab**.

## Hypothesis (from code reading on `v1.5-variegata`)

The MergeInto pipeline runs `PhysicalInsert` as a child operator (one
of the actions in `MergeIntoGlobalState`). On the parallel path,
`PhysicalInsert::Sink` (`physical_insert.cpp` ~line 645–670) creates a
per-thread `OptimisticDataWriter` and an
`OptimisticWriteCollection` whose `RowGroupCollection` (and therefore
its `RowGroup`s and their `ColumnData`s) is owned by
`LocalTableStorage::optimistic_collections` — a `vector<unique_ptr<...>>`
indexed by `lstate.collection_index`.

`PhysicalInsert::Combine` (`physical_insert.cpp` ~line 673–718) splits
on `append_count < row_group_size`:

- **Small branch (`< row_group_size`):** drain the local collection
  via `Chunks(transaction)` and re-append into the shared local
  storage.
- **Large branch (`>= row_group_size`, which is the path that hits
  ~765003 rows):**

  ```cpp
  lstate.optimistic_writer->WriteUnflushedRowGroups(optimistic_collection);
  lstate.optimistic_writer->FinalFlush();
  gstate.table.GetStorage().LocalMerge(context.client, optimistic_collection);
  auto &optimistic_writer = gstate.table.GetStorage().GetOptimisticWriter(context.client);
  optimistic_writer.Merge(*lstate.optimistic_writer);
  ```

  `LocalStorage::LocalMerge` (`local_storage.cpp:509`) calls
  `OptimisticWriteCollection::MergeStorage`
  (`optimistic_data_writer.cpp:142`), which calls
  `RowGroupCollection::MergeStorage` (`row_group_collection.cpp:696`)
  with `MoveSegments()` — i.e. `unique_ptr<RowGroup>`s **are moved**
  out of the per-thread `OptimisticWriteCollection`'s
  `RowGroupCollection` and into the shared `LocalTableStorage`.

  After `MergeStorage` returns, the per-thread
  `OptimisticWriteCollection` (and its `RowGroupCollection`) still
  exists but its `RowGroup` segment tree has been emptied. Critically,
  **`lstate.local_append_state.row_group_append_state.row_group`**
  (a raw `RowGroup *` — see `append_state.hpp:50`) and its child
  `ColumnAppendState`s still point at the moved-out
  `RowGroup` / `ColumnData`. When `MergeStorage` calls
  `row_group->MoveToCollection(*this)`, the `RowGroup` is reparented
  but the storage is not yet freed (so far so good).

  However, `RowGroupCollection::MergeStorage` does NOT itself free the
  old `ColumnData`. The free path is via
  `optimistic_collection.collection->CommitDropTable()` /
  destruction of the now-empty `OptimisticWriteCollection`. The owning
  slot in `LocalTableStorage::optimistic_collections` is **never
  reset** by the success path of `Combine` (only `Rollback()` calls
  `optimistic_collections.clear()`, see `local_storage.cpp:283-294`).
  So the lifetime question reduces to: are the moved-out `RowGroup`
  segments (now in `LocalTableStorage::row_groups`) still alive when
  the next `Sink` chunk lands and walks
  `lstate.local_append_state.row_group_append_state.row_group`?

  The bug almost certainly sits in this corner: a per-thread
  `local_append_state.row_group` raw pointer survives across an
  operation that re-anchors row groups elsewhere. If `MergeStorage`
  triggers a `SetRowGroupAppendMode(SUGGEST_NEW)` (see
  `optimistic_data_writer.cpp:171`), or if a concurrent thread's
  `LocalMerge` causes `LocalTableStorage::row_groups` to be
  reorganized, the raw `RowGroup *` in `local_append_state` becomes
  stale. The next `Append` call walks that stale pointer's vtable and
  data, and we land exactly on the `ColumnData::Append` SIGSEGV
  observed.

  The string-slab reuse signature ("memory structs o") is consistent
  with the `BufferAllocator` reissuing the freed `ColumnData` slab to
  a `std::string` allocation in another thread — most likely a
  metadata string. The cleanly-reset `type.id_ = INVALID` is the
  `LogicalType` default constructor — i.e. a freshly-default-constructed
  `LogicalType` was placed at exactly the freed object's offset.
  That's the smoking gun for "this slab was freed and re-issued for a
  different object" rather than "this object was double-freed".

### Concretely, the suspect lines

- `physical_insert.cpp:710-714` — the order
  `WriteUnflushedRowGroups` → `FinalFlush` → `LocalMerge` →
  `optimistic_writer.Merge` — and then the Combine returns
  `FINISHED` without calling `ResetOptimisticCollection`. The local
  state still references chunks that have been moved away.
- `optimistic_data_writer.cpp:142-173` (`MergeStorage`) — note the
  `SetRowGroupAppendMode(RowGroupAppendMode::SUGGEST_NEW)` once
  `complete_row_groups == GetRowGroupCount()`. This silently changes
  what the next `Append` will do.
- `row_group_collection.cpp:696-745` — `MergeStorage` moves segments
  out from `data` into `*this`. After the call, any raw pointer into
  `data`'s segments is now ambiguously parented.
- `local_storage.cpp:262-294` — the success path NEVER clears
  `optimistic_collections[collection_index.index]`. Only `Rollback()`
  does. So the per-thread collection is kept alive for the duration of
  the transaction, but its row-group segments have been moved away.

### Why MergeInto specifically?

Plain `PhysicalInsert` runs `Combine` once at end-of-pipeline. In
`PhysicalMergeInto`, `MergeIntoGlobalState::Combine`
(`physical_merge_into.cpp:223-239`) iterates all child actions and
calls `Combine` on each — and then `PhysicalMergeInto::Combine` itself
runs (line 379-388). The combine ordering and the fact that
`MergeIntoLocalState::states[i].local_state` (a
`unique_ptr<LocalSinkState>`) is destroyed after all the inner
`Combine` calls means that any data that the inner `PhysicalInsert`'s
`local_append_state` still references must outlive
`MergeIntoLocalState::states[i]`. If `LocalMerge` has already
moved-out the row groups, the inner local state is referencing freed
memory the moment a later combine tries to access it.

## What we did NOT verify

- We were unable to actually run the test under ASAN in this
  environment (no compiler available — see `INVESTIGATION_NOTES.md`),
  so the test's trigger rate against `v1.5-variegata` is **unknown**.
- We did not bisect to confirm the bug exists on `v1.5-variegata` HEAD
  vs. the internal fork commit `f8c4bc0e`. There has been no obvious
  fix in the relevant files since (no commit matched any of
  `use-after-free`, `lifetime`, `merge.into`, `optimistic.*combine`,
  `combine.*race`, `UAF` in the suspect file set).

## If `git push` fails

If pushing this branch returns 403 or similar, do not retry. Capture
the exact error in a comment at the top of this file and exit. The
test files alone are useful artifacts even without a PR.
