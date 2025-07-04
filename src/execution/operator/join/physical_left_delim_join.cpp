#include "duckdb/execution/operator/join/physical_left_delim_join.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/operator/aggregate/physical_hash_aggregate.hpp"
#include "duckdb/execution/operator/join/physical_join.hpp"
#include "duckdb/execution/operator/scan/physical_column_data_scan.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"

namespace duckdb {

PhysicalLeftDelimJoin::PhysicalLeftDelimJoin(PhysicalPlan &physical_plan, PhysicalPlanGenerator &planner,
                                             vector<LogicalType> types, PhysicalOperator &original_join,
                                             PhysicalOperator &distinct,
                                             const vector<const_reference<PhysicalOperator>> &delim_scans,
                                             idx_t estimated_cardinality, optional_idx delim_idx)
    : PhysicalDelimJoin(physical_plan, PhysicalOperatorType::LEFT_DELIM_JOIN, std::move(types), original_join, distinct,
                        delim_scans, estimated_cardinality, delim_idx) {
	D_ASSERT(join.children.size() == 2);
	// now for the original join
	// we take its left child, this is the side that we will duplicate eliminate
	children.push_back(join.children[0]);

	// we replace it with a PhysicalColumnDataScan, that scans the ColumnDataCollection that we keep cached
	// the actual chunk collection to scan will be created in the LeftDelimJoinGlobalState
	auto &cached_scan = planner.Make<PhysicalColumnDataScan>(
	    children[0].get().GetTypes(), PhysicalOperatorType::COLUMN_DATA_SCAN, estimated_cardinality, nullptr);
	if (delim_idx.IsValid()) {
		auto &cast_cached_scan = cached_scan.Cast<PhysicalColumnDataScan>();
		cast_cached_scan.cte_index = delim_idx.GetIndex();
	}
	join.children[0] = cached_scan;
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class LeftDelimJoinGlobalState : public GlobalSinkState {
public:
	explicit LeftDelimJoinGlobalState(ClientContext &context, const PhysicalLeftDelimJoin &delim_join)
	    : lhs_data(context, delim_join.children[0].get().GetTypes()) {
		D_ASSERT(!delim_join.delim_scans.empty());
		// set up the delim join chunk to scan in the original join
		auto &cast_cached_scan = delim_join.join.children[0].get().Cast<PhysicalColumnDataScan>();
		cast_cached_scan.collection = &lhs_data;
	}

	ColumnDataCollection lhs_data;
	mutex lhs_lock;

	void Merge(ColumnDataCollection &input) {
		lock_guard<mutex> guard(lhs_lock);
		lhs_data.Combine(input);
	}
};

class LeftDelimJoinLocalState : public LocalSinkState {
public:
	explicit LeftDelimJoinLocalState(ClientContext &context, const PhysicalLeftDelimJoin &delim_join)
	    : lhs_data(context, delim_join.children[0].get().GetTypes()) {
		lhs_data.InitializeAppend(append_state);
	}

	unique_ptr<LocalSinkState> distinct_state;
	ColumnDataCollection lhs_data;
	ColumnDataAppendState append_state;

	void Append(DataChunk &input) {
		lhs_data.Append(input);
	}
};

unique_ptr<GlobalSinkState> PhysicalLeftDelimJoin::GetGlobalSinkState(ClientContext &context) const {
	auto state = make_uniq<LeftDelimJoinGlobalState>(context, *this);
	distinct.sink_state = distinct.GetGlobalSinkState(context);
	if (delim_scans.size() > 1) {
		PhysicalHashAggregate::SetMultiScan(*distinct.sink_state);
	}
	return std::move(state);
}

unique_ptr<LocalSinkState> PhysicalLeftDelimJoin::GetLocalSinkState(ExecutionContext &context) const {
	auto state = make_uniq<LeftDelimJoinLocalState>(context.client, *this);
	state->distinct_state = distinct.GetLocalSinkState(context);
	return std::move(state);
}

SinkResultType PhysicalLeftDelimJoin::Sink(ExecutionContext &context, DataChunk &chunk,
                                           OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<LeftDelimJoinLocalState>();
	lstate.lhs_data.Append(lstate.append_state, chunk);
	OperatorSinkInput distinct_sink_input {*distinct.sink_state, *lstate.distinct_state, input.interrupt_state};
	distinct.Sink(context, chunk, distinct_sink_input);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalLeftDelimJoin::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &lstate = input.local_state.Cast<LeftDelimJoinLocalState>();
	auto &gstate = input.global_state.Cast<LeftDelimJoinGlobalState>();
	gstate.Merge(lstate.lhs_data);

	OperatorSinkCombineInput distinct_combine_input {*distinct.sink_state, *lstate.distinct_state,
	                                                 input.interrupt_state};
	distinct.Combine(context, distinct_combine_input);

	return SinkCombineResultType::FINISHED;
}

void PhysicalLeftDelimJoin::PrepareFinalize(ClientContext &context, GlobalSinkState &sink_state) const {
	distinct.PrepareFinalize(context, *distinct.sink_state);
}

SinkFinalizeType PhysicalLeftDelimJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &client,
                                                 OperatorSinkFinalizeInput &input) const {
	// Finalize the distinct hash table.
	OperatorSinkFinalizeInput finalize_input {*distinct.sink_state, input.interrupt_state};
	distinct.Finalize(pipeline, event, client, finalize_input);
	return SinkFinalizeType::READY;
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalLeftDelimJoin::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();

	auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
	child_meta_pipeline.Build(children[0]);

	D_ASSERT(type == PhysicalOperatorType::LEFT_DELIM_JOIN);
	// recurse into the actual join
	// any pipelines in there depend on the main pipeline
	// any scan of the duplicate eliminated data on the RHS depends on this pipeline
	// we add an entry to the mapping of (PhysicalOperator*) -> (Pipeline*)
	auto &state = meta_pipeline.GetState();
	for (auto &delim_scan : delim_scans) {
		state.delim_join_dependencies.insert(
		    make_pair(delim_scan, reference<Pipeline>(*child_meta_pipeline.GetBasePipeline())));
	}
	join.BuildPipelines(current, meta_pipeline);
}

} // namespace duckdb
