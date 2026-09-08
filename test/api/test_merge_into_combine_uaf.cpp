// Threaded reproducer for the SIGSEGV / heap-use-after-free observed in
//
//   PhysicalMergeInto::Combine
//   -> MergeIntoGlobalState::Combine
//   -> PhysicalInsert::Combine
//   -> DataTable::LocalAppend
//   -> LocalStorage::Append
//   -> RowGroupCollection::Append
//   -> RowGroup::Append
//   -> ColumnData::Append   <-- crash
//
// !!! ASAN-ONLY TEST !!!
//   Without -DENABLE_SANITIZER=1 this test passing tells you nothing.
//   The bug is a heap-use-after-free where the freed slab is recycled
//   into a live allocation; without ASAN the read silently returns
//   garbage and the test "passes".
//
//   This test is also marked [!hide] so the default `unittest` run
//   doesn't pick it up. Invoke explicitly with:
//     ./build/debug/test/unittest "[merge_into]"
//
// In an internal fork (commit f8c4bc0e) gdb at the crash showed:
//   * ColumnData's vtable pointer pointed at
//     StandardBufferManager::BufferAllocatorAllocate (NOT a vtable),
//   * `type.id_` reset to INVALID,
//   * `parent`/`compression` fields contained ASCII 'memory structs o'
//     (= freed slab recycled into a std::string allocation),
//   * RowGroupCollection::Append running with
//     `last_row_group != current_row_group` at insert_count = 765003,
//   * PhysicalInsert::Combine's local insert_chunk had garbage
//     Vector::size and capacity.
//
// Trigger profile:
//   * INSERT OR IGNORE desugars into MergeInto via
//     Binder::Bind(InsertStatement) -> GenerateMergeInto.
//   * The corrupted ColumnData lived in an OptimisticWriteCollection
//     row group, suggesting the lifetime bug is around how
//     OptimisticWriteCollection / its ColumnData are kept alive
//     across PhysicalMergeInto::Combine -> PhysicalInsert::Combine
//     -> LocalMerge.
//
// Strategy:
//   * 2 connections doing INSERT OR IGNORE on overlapping ranges into a
//     table with a VARCHAR column and PRIMARY KEY.
//   * 1 thread CHECKPOINTing every 20ms to force buffer reuse.
//   * Wall-clock-bounded at 15s — the test should EITHER abort under
//     ASAN within seconds, OR run to time-budget cleanly.

#include "catch.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace duckdb;

namespace {

constexpr idx_t ROWS_PER_ITERATION = 200000;       // > 122880 row-group size
constexpr idx_t INSERTER_THREADS = 2;
constexpr int   WALL_CLOCK_BUDGET_SECONDS = 15;

void Inserter(DuckDB *db, idx_t worker_id, std::atomic<bool> *stop) {
	Connection con(*db);
	(void)con.Query("PRAGMA threads=4");
	idx_t iteration = 0;
	while (!stop->load()) {
		// Overlapping ranges across workers + iterations, so IGNORE
		// path is exercised every loop.
		idx_t start = ((worker_id + iteration) * 100000) % ROWS_PER_ITERATION;
		idx_t end = start + ROWS_PER_ITERATION;
		auto sql = "INSERT OR IGNORE INTO t SELECT i, "
		           "i || ' payload string for slab reuse pattern', "
		           "repeat(CHR((((i + " +
		           std::to_string(worker_id) +
		           ") % 26) + 65)::INTEGER), 100) "
		           "FROM range(" +
		           std::to_string(start) + "," + std::to_string(end) + ") tbl(i)";
		(void)con.Query(sql);
		iteration++;
	}
}

void Checkpointer(DuckDB *db, std::atomic<bool> *stop) {
	Connection con(*db);
	while (!stop->load()) {
		(void)con.Query("CHECKPOINT");
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
}

} // namespace

TEST_CASE("Concurrent INSERT OR IGNORE + CHECKPOINT (UAF reproducer)",
          "[storage][merge_into][!hide]") {
	// [!hide] — does not run by default. Run explicitly:
	//   ./build/debug/test/unittest "[merge_into]"
	// MUST be built with ENABLE_SANITIZER=1 to catch the bug.
	DuckDB db(nullptr);
	{
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("PRAGMA threads=4"));
		REQUIRE_NO_FAIL(con.Query(
		    "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR, payload VARCHAR)"));
	}

	std::atomic<bool> stop {false};
	std::vector<std::thread> workers;
	workers.reserve(INSERTER_THREADS + 1);
	for (idx_t i = 0; i < INSERTER_THREADS; i++) {
		workers.emplace_back(Inserter, &db, i, &stop);
	}
	workers.emplace_back(Checkpointer, &db, &stop);

	// Wall-clock budget: under ASAN the bug should fire within seconds.
	// On a clean codebase the test runs to budget then exits cleanly.
	std::this_thread::sleep_for(std::chrono::seconds(WALL_CLOCK_BUDGET_SECONDS));
	stop.store(true);
	for (auto &w : workers) {
		w.join();
	}

	// Under ASAN, any heap-use-after-free above already aborted the
	// process. If we got here, the workload completed without ASAN
	// firing — this is the success path.
	Connection con(db);
	auto result = con.Query("SELECT COUNT(*) FROM t");
	REQUIRE(result->HasError() == false);
}
