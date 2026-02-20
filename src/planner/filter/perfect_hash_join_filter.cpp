#include "duckdb/planner/filter/perfect_hash_join_filter.hpp"

#include "duckdb/execution/operator/join/perfect_hash_join_executor.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

PerfectHashJoinFilter::PerfectHashJoinFilter(optional_ptr<const PerfectHashJoinExecutor> perfect_join_executor_p,
                                             const string &key_column_name_p)
    : TableFilter(TYPE), perfect_join_executor(perfect_join_executor_p), key_column_name(key_column_name_p) {
}

FilterPropagateResult PerfectHashJoinFilter::CheckStatistics(BaseStatistics &stats) const {
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

string PerfectHashJoinFilter::ToString(const string &column_name) const {
	return column_name + " IN PHJ(" + key_column_name + ")";
}

idx_t PerfectHashJoinFilter::Filter(Vector &keys, SelectionVector &sel, idx_t &approved_tuple_count) const {
	if (perfect_join_executor) {
		if (!sel.IsSet()) {
			sel.Initialize(approved_tuple_count);
		}
		const idx_t approved_before = approved_tuple_count;
		approved_tuple_count = 0;
		perfect_join_executor->FillSelectionVectorSwitchProbe(keys, approved_before, sel, approved_tuple_count,
		                                                      nullptr);
	}
	return approved_tuple_count;
}

bool PerfectHashJoinFilter::FilterValue(const Value &value) const {
	Vector keys(value);
	SelectionVector sel;
	idx_t approved_tuple_count = 1;
	Filter(keys, sel, approved_tuple_count);
	return approved_tuple_count == 1;
}

bool PerfectHashJoinFilter::Equals(const TableFilter &other_p) const {
	if (!TableFilter::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<PerfectHashJoinFilter>();
	return perfect_join_executor.get() == other.perfect_join_executor.get() && key_column_name == other.key_column_name;
}
unique_ptr<TableFilter> PerfectHashJoinFilter::Copy() const {
	return make_uniq<PerfectHashJoinFilter>(perfect_join_executor, key_column_name);
}

unique_ptr<Expression> PerfectHashJoinFilter::ToExpression(const Expression &column) const {
	auto bound_constant = make_uniq<BoundConstantExpression>(Value(true));
	return std::move(bound_constant);
}

void PerfectHashJoinFilter::Serialize(Serializer &serializer) const {
	TableFilter::Serialize(serializer);
	serializer.WriteProperty<string>(200, "key_column_name", key_column_name);
}

unique_ptr<TableFilter> PerfectHashJoinFilter::Deserialize(Deserializer &deserializer) {
	auto key_column_name = deserializer.ReadProperty<string>(200, "key_column_name");
	return make_uniq<PerfectHashJoinFilter>(nullptr, key_column_name);
}

} // namespace duckdb
