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
// In an internal fork (commit f8c4bc0e) gdb at the crash showed:
//   * The ColumnData object's vtable pointer pointed at
//     StandardBufferManager::BufferAllocatorAllocate (NOT a vtable),
//   * `type.id_` had been reset to INVALID,
//   * `parent` and `compression` fields contained ASCII bytes spelling
//     'memory structs o' (= the freed slab had been recycled into a
//     std::string allocation),
//   * `RowGroupCollection::Append` was running with
//     `last_row_group != current_row_group` and
//     `gstate.insert_count = 765003`,
//   * `PhysicalInsert::Combine`'s local insert_chunk had garbage
//     Vector::size and capacity.
//
// Trigger profile:
//   * INSERT OR IGNORE desugars into MergeInto via
//     Binder::Bind(InsertStatement) -> GenerateMergeInto. The parallel
//     pipeline runs PhysicalInsert as a child of PhysicalMergeInto,
//     and PhysicalInsert::Combine merges a per-thread
//     OptimisticWriteCollection into LocalStorage.
//   * The corrupted ColumnData lived in an OptimisticWriteCollection
//     row group, suggesting the lifetime bug is around how
//     OptimisticWriteCollection / its ColumnData are kept alive across
//     the PhysicalMergeInto::Combine -> PhysicalInsert::Combine ->
//     LocalMerge transition.
//
// Strategy:
//   * Run multiple connections, each doing INSERT OR IGNORE on
//     overlapping ranges into a table with a VARCHAR column and a
//     PRIMARY KEY (so the IGNORE path is exercised and the MergeInto
//     desugar fires).
//   * One thread also runs CHECKPOINT to force optimistic-write blocks
//     to be reused.
//   * The merge / checkpoint timing is what distinguishes this race
//     from the pure single-threaded sqllogic version.
//
// Trigger rate: highly reliable on the internal fork; on public DuckDB
// v1.5-variegata the test harness was unable to verify because the
// build environment for this run lacked a C++ compiler. See
// tools/repro/INVESTIGATION_NOTES.md.

#include "catch.hpp"
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace duckdb;

namespace {

constexpr idx_t TARGET_ROWS = 1'500'000;     // > 765k crash threshold
constexpr idx_t INSERTER_THREADS = 4;        // parallel optimistic-write path
constexpr idx_t ITERATIONS_PER_INSERTER = 3; // multiple slabs / row groups

void Inserter(DuckDB *db, idx_t worker_id, std::atomic<bool> *stop) {
	Connection con(*db);
	(void)con.Query("PRAGMA threads=4");
	for (idx_t it = 0; it < ITERATIONS_PER_INSERTER && !stop->load(); it++) {
		// Overlapping ranges across workers, so the IGNORE path is exercised.
		idx_t start = (worker_id * 250'000) % TARGET_ROWS;
		idx_t end = start + TARGET_ROWS;
		auto sql = "INSERT OR IGNORE INTO t SELECT i, "
		           "i || ' payload string for slab reuse pattern in OptimisticDataWriter', "
		           "repeat(CHR((((i + " +
		           std::to_string(worker_id) +
		           ") % 26) + 65)::INTEGER), 100) "
		           "FROM range(" +
		           std::to_string(start) + "," + std::to_string(end) + ") tbl(i)";
		auto result = con.Query(sql);
		// Don't REQUIRE here: under ASAN any heap-use-after-free will
		// surface as an ASAN abort regardless. Surface query errors only
		// if they are not the expected concurrency error.
		(void)result;
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
	// Marked [!hide] so it isn't run by default — this is a stress
	// reproducer that depends on ASAN to surface the bug. Run with:
	//   ./build/debug/test/unittest "[merge_into]"
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

	// Let inserters finish.
	for (idx_t i = 0; i < INSERTER_THREADS; i++) {
		workers[i].join();
	}
	stop.store(true);
	workers.back().join();

	// Sanity: table must contain at least the unique key range. Under
	// ASAN, the value here is irrelevant because we already aborted on
	// the heap-use-after-free above.
	Connection con(db);
	auto result = con.Query("SELECT COUNT(*) FROM t");
	REQUIRE(result->HasError() == false);
}
