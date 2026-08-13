# UAF reproducer for `PhysicalMergeInto::Combine`

**Status: NOT YET REPRODUCED on `motherduckdb/public-duckdb` `v1.5-variegata`.**

This directory contains a hypothesis-shaped reproducer for a SIGSEGV
observed on an internal duckdb fork (commit `f8c4bc0e`). The crash was
diagnosed as a heap-use-after-free in `ColumnData::Append`, reached via
`PhysicalMergeInto::Combine` → `PhysicalInsert::Combine` →
`RowGroupCollection::Append`. See gdb backtrace + locals dump in the
originating Slack thread for the full evidence.

## What's in this PR

- `test/sql/storage/merge_into_combine_uaf.test` — sqllogic-driven
  reproducer that runs the smallest workload large enough to cross the
  row-group boundary the original crash hit. ~1–2s on debug build.
- `test/api/test_merge_into_combine_uaf.cpp` — multi-threaded C++
  reproducer with concurrent `INSERT OR IGNORE` + `CHECKPOINT`,
  wall-clock-bounded at 15s. Tagged `[!hide]` so default `unittest`
  runs skip it; invoke explicitly with `"[merge_into]"`.

## Mandatory: build with ASAN

```bash
BUILD_UNITTEST=1 ENABLE_SANITIZER=1 CRASH_ON_ASSERT=1 make debug -j
```

Without ASAN, **these tests prove nothing**. The bug is a heap-use-after-free
where the freed slab is immediately recycled into a different live
allocation (gdb showed the slab's bytes had been overwritten with an
ASCII string at crash time). Without ASAN, the read just returns
garbage and execution continues.

## Run

```bash
# Single-threaded sqllogic — fastest, run first:
./build/debug/test/unittest test/sql/storage/merge_into_combine_uaf.test

# Threaded stress — only useful under ASAN:
./build/debug/test/unittest "[merge_into]"

# Tight loop variant, in case the trigger is timing-sensitive:
for i in $(seq 1 30); do
    ./build/debug/test/unittest test/sql/storage/merge_into_combine_uaf.test || break
done
```

## How to interpret a clean run

A clean run on `v1.5-variegata` does NOT mean the bug is fixed. It
means one of:

1. The bug is fixed on `v1.5-variegata` but exists on the internal fork.
2. The bug is present on both, but this test's row counts / thread
   counts / scheduling don't trigger it deterministically.
3. ASAN was not actually enabled in the build.

For (3), check that the binary was built with `-fsanitize=address`:

```bash
nm build/debug/test/unittest 2>/dev/null | grep -c '__asan' || echo "NOT ASAN"
```

A non-zero count means ASAN is linked. Zero means rebuild with
`ENABLE_SANITIZER=1`.

## How to interpret an ASAN abort

If ASAN fires, the abort report will include both:

- The **read** stack — should bottom out in `ColumnData::Append` /
  `RowGroup::Append`.
- The **freed-by** stack — this is the gold. It tells us where the
  `ColumnData` (or its containing `RowGroup` / `RowGroupCollection`)
  was destroyed prematurely. The bug fix lives there.

File the freed-by stack alongside this PR.

## What to try next if these tests don't trigger

1. Larger row counts (back to 1M+ per wave) — the original crash hit
   at 765k rows; the size cut here is for fast iteration, not coverage.
2. More inserter threads (4, 8). The internal-fork crash was on a
   parallel pipeline.
3. Add UPDATE / DELETE workload alongside INSERT OR IGNORE — the
   merge-into Combine path also handles those actions.
4. Run under TSAN (`THREAD_SANITIZER`) instead of ASAN — if the bug is
   a data race rather than a pure UAF, TSAN will catch it where ASAN
   won't.

## Bisect targets if/when reproduced

If reproduced on `v1.5-variegata` (or some earlier point), bisect range
should be commits touching:

- `src/execution/operator/persistent/physical_merge_into.cpp`
- `src/execution/operator/persistent/physical_insert.cpp`
- `src/storage/local_storage.cpp`
- `src/storage/optimistic_data_writer.cpp`
- `src/storage/table/row_group_collection.cpp`

The `RowGroupReorderer` patches (commits `27575c3e`, `e6971ad4`) were
*not* this bug — they fix the read/scan path, not the append/Combine
path. See `INVESTIGATION_NOTES.md` for the elimination logic.
