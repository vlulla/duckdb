#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/tableref/bound_subqueryref.hpp"
#include "duckdb/planner/tableref/bound_pivotref.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

static void ConstructPivots(PivotRef &ref, vector<PivotValueElement> &pivot_values, idx_t pivot_idx = 0,
                            const PivotValueElement &current_value = PivotValueElement()) {
	auto &pivot = ref.pivots[pivot_idx];
	bool last_pivot = pivot_idx + 1 == ref.pivots.size();
	for (auto &entry : pivot.entries) {
		PivotValueElement new_value = current_value;
		string name = entry.alias;
		D_ASSERT(entry.values.size() == pivot.pivot_expressions.size());
		for (idx_t v = 0; v < entry.values.size(); v++) {
			auto &value = entry.values[v];
			new_value.values.push_back(value);
			if (entry.alias.empty()) {
				if (name.empty()) {
					name = value.ToString();
				} else {
					name += "_" + value.ToString();
				}
			}
		}
		if (!current_value.name.empty()) {
			new_value.name = current_value.name + "_" + name;
		} else {
			new_value.name = std::move(name);
		}
		if (last_pivot) {
			pivot_values.push_back(std::move(new_value));
		} else {
			// need to recurse
			ConstructPivots(ref, pivot_values, pivot_idx + 1, new_value);
		}
	}
}

static void ExtractPivotExpressions(ParsedExpression &root_expr, case_insensitive_set_t &handled_columns) {
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    root_expr, [&](const ColumnRefExpression &child_colref) {
		    if (child_colref.IsQualified()) {
			    throw BinderException(child_colref, "PIVOT expression cannot contain qualified columns");
		    }
		    handled_columns.insert(child_colref.GetColumnName());
	    });
}

struct ExtractPivotAggregateOperator {
	using TYPE = vector<reference<FunctionExpression>>;

	static void HandleAggregate(unique_ptr<ParsedExpression> &expr, FunctionExpression &aggr_function,
	                            TYPE &aggregates) {
		aggregates.push_back(aggr_function);
	}
};

struct ReplacePivotAggregateOperator {
	using TYPE = unique_ptr<ParsedExpression>;

	static void HandleAggregate(unique_ptr<ParsedExpression> &expr, FunctionExpression &aggr_function,
	                            TYPE &replacement_expr) {
		if (replacement_expr->type != ExpressionType::COLUMN_REF) {
			throw BinderException(*expr, "Pivot expression can only have one aggregate");
		}
		auto aggr = std::move(expr);
		expr = std::move(replacement_expr);
		replacement_expr = std::move(aggr);
	}
};

template <class OP>
void TemplatedHandlePivotAggregate(ClientContext &context, unique_ptr<ParsedExpression> &expr,
                                   typename OP::TYPE &aggregates) {
	if (expr->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &aggr_function = expr->Cast<FunctionExpression>();

		// check if this is an aggregate to ensure it is an aggregate and not a scalar function
		EntryLookupInfo lookup_info(CatalogType::AGGREGATE_FUNCTION_ENTRY, aggr_function.function_name, *expr);
		auto &entry = Catalog::GetEntry(context, aggr_function.catalog, aggr_function.schema, lookup_info);
		if (entry.type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
			// aggregate
			OP::HandleAggregate(expr, aggr_function, aggregates);
			return;
		}
	}
	if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
		throw BinderException(*expr, "Columns can only be referenced within the aggregate of a PIVOT expression");
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		TemplatedHandlePivotAggregate<OP>(context, child, aggregates);
	});
}

void ExtractPivotAggregateExpression(ClientContext &context, unique_ptr<ParsedExpression> &expr,
                                     vector<reference<FunctionExpression>> &aggregates) {
	TemplatedHandlePivotAggregate<ExtractPivotAggregateOperator>(context, expr, aggregates);
}

void ReplacePivotAggregateExpression(ClientContext &context, unique_ptr<ParsedExpression> &expr,
                                     unique_ptr<ParsedExpression> &replacement_expr) {
	TemplatedHandlePivotAggregate<ReplacePivotAggregateOperator>(context, expr, replacement_expr);
}

static unique_ptr<SelectNode> ConstructInitialGrouping(PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns,
                                                       const case_insensitive_set_t &handled_columns) {
	auto subquery = make_uniq<SelectNode>();
	subquery->from_table = std::move(ref.source);
	if (ref.groups.empty()) {
		// if rows are not specified any columns that are not pivoted/aggregated on are added to the GROUP BY clause
		for (auto &entry : all_columns) {
			auto column_entry = Binder::GetResolvedColumnExpression(*entry);
			if (!column_entry) {
				throw InternalException("Unexpected child of pivot source - not a ColumnRef");
			}
			auto &columnref = column_entry->Cast<ColumnRefExpression>();
			if (handled_columns.find(columnref.GetColumnName()) == handled_columns.end()) {
				// not handled - add to grouping set
				subquery->groups.group_expressions.push_back(make_uniq<ConstantExpression>(
				    Value::INTEGER(UnsafeNumericCast<int32_t>(subquery->select_list.size() + 1))));
				subquery->select_list.push_back(make_uniq<ColumnRefExpression>(columnref.GetColumnName()));
			}
		}
	} else {
		// if rows are specified only the columns mentioned in rows are added as groups
		for (auto &row : ref.groups) {
			subquery->groups.group_expressions.push_back(make_uniq<ConstantExpression>(
			    Value::INTEGER(UnsafeNumericCast<int32_t>(subquery->select_list.size() + 1))));
			subquery->select_list.push_back(make_uniq<ColumnRefExpression>(row));
		}
	}
	return subquery;
}

static unique_ptr<SelectNode> PivotFilteredAggregate(ClientContext &context, PivotRef &ref,
                                                     vector<unique_ptr<ParsedExpression>> all_columns,
                                                     const case_insensitive_set_t &handled_columns,
                                                     vector<PivotValueElement> pivot_values) {
	auto subquery = ConstructInitialGrouping(ref, std::move(all_columns), handled_columns);

	// push the filtered aggregates
	for (auto &pivot_value : pivot_values) {
		unique_ptr<ParsedExpression> filter;
		idx_t pivot_value_idx = 0;
		for (auto &pivot_column : ref.pivots) {
			for (auto &pivot_expr : pivot_column.pivot_expressions) {
				auto column_ref = make_uniq<CastExpression>(LogicalType::VARCHAR, pivot_expr->Copy());
				auto constant_value = make_uniq<ConstantExpression>(
				    pivot_value.values[pivot_value_idx++].DefaultCastAs(LogicalType::VARCHAR));
				auto comp_expr = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOT_DISTINCT_FROM,
				                                                 std::move(column_ref), std::move(constant_value));
				if (filter) {
					filter = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(filter),
					                                          std::move(comp_expr));
				} else {
					filter = std::move(comp_expr);
				}
			}
		}
		for (auto &aggregate : ref.aggregates) {
			auto copied_aggr = aggregate->Copy();

			vector<reference<FunctionExpression>> aggregates;
			ExtractPivotAggregateExpression(context, copied_aggr, aggregates);
			D_ASSERT(aggregates.size() == 1);

			auto &aggr = aggregates[0].get().Cast<FunctionExpression>();
			aggr.filter = filter->Copy();
			auto &aggr_name = aggregate->GetAlias();
			auto name = pivot_value.name;
			if (ref.aggregates.size() > 1 || !aggr_name.empty()) {
				// if there are multiple aggregates specified we add the name of the aggregate as well
				name += "_" + (aggr_name.empty() ? aggregate->GetName() : aggr_name);
			}
			copied_aggr->SetAlias(name);
			subquery->select_list.push_back(std::move(copied_aggr));
		}
	}
	return subquery;
}

struct PivotBindState {
	vector<string> internal_group_names;
	vector<string> group_names;
	vector<string> aggregate_names;
	vector<string> internal_aggregate_names;
};

static unique_ptr<SelectNode> PivotInitialAggregate(ClientContext &context, PivotBindState &bind_state, PivotRef &ref,
                                                    vector<unique_ptr<ParsedExpression>> all_columns,
                                                    const case_insensitive_set_t &handled_columns) {
	auto subquery_stage1 = ConstructInitialGrouping(ref, std::move(all_columns), handled_columns);

	idx_t group_count = 0;
	for (auto &expr : subquery_stage1->select_list) {
		bind_state.group_names.push_back(expr->GetName());
		if (expr->GetAlias().empty()) {
			expr->SetAlias("__internal_pivot_group" + std::to_string(++group_count));
		}
		bind_state.internal_group_names.push_back(expr->GetAlias());
	}
	// group by all of the pivot values
	idx_t pivot_count = 0;
	for (auto &pivot_column : ref.pivots) {
		for (auto &pivot_expr : pivot_column.pivot_expressions) {
			if (pivot_expr->GetAlias().empty()) {
				pivot_expr->SetAlias("__internal_pivot_ref" + std::to_string(++pivot_count));
			}
			auto pivot_alias = pivot_expr->GetAlias();
			subquery_stage1->groups.group_expressions.push_back(make_uniq<ConstantExpression>(
			    Value::INTEGER(UnsafeNumericCast<int32_t>(subquery_stage1->select_list.size() + 1))));
			subquery_stage1->select_list.push_back(std::move(pivot_expr));
			pivot_expr = make_uniq<ColumnRefExpression>(std::move(pivot_alias));
		}
	}
	idx_t aggregate_count = 0;
	// finally add the aggregates
	for (auto &aggregate : ref.aggregates) {
		auto aggregate_alias = "__internal_pivot_aggregate" + std::to_string(++aggregate_count);
		auto aggr_name = aggregate->GetAlias();
		if (aggr_name.empty() && ref.aggregates.size() > 1) {
			aggr_name = aggregate->ToString();
		}
		if (!aggr_name.empty()) {
			aggr_name = "_" + aggr_name;
		}

		unique_ptr<ParsedExpression> aggregate_ref;
		aggregate_ref = make_uniq<ColumnRefExpression>(aggregate_alias);
		ReplacePivotAggregateExpression(context, aggregate, aggregate_ref);

		bind_state.aggregate_names.push_back(std::move(aggr_name));
		bind_state.internal_aggregate_names.push_back(aggregate_alias);
		aggregate_ref->SetAlias(std::move(aggregate_alias));
		subquery_stage1->select_list.push_back(std::move(aggregate_ref));
	}
	return subquery_stage1;
}

unique_ptr<ParsedExpression> ConstructPivotExpression(unique_ptr<ParsedExpression> pivot_expr) {
	auto cast = make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(pivot_expr));
	vector<unique_ptr<ParsedExpression>> coalesce_children;
	coalesce_children.push_back(std::move(cast));
	coalesce_children.push_back(make_uniq<ConstantExpression>(Value("NULL")));
	auto coalesce = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(coalesce_children));
	return std::move(coalesce);
}

static unique_ptr<SelectNode> PivotListAggregate(PivotBindState &bind_state, PivotRef &ref,
                                                 unique_ptr<SelectNode> subquery_stage1) {
	auto subquery_stage2 = make_uniq<SelectNode>();
	// wrap the subquery of stage 1
	auto subquery_select = make_uniq<SelectStatement>();
	subquery_select->node = std::move(subquery_stage1);
	auto subquery_ref = make_uniq<SubqueryRef>(std::move(subquery_select));

	// add all of the groups
	for (idx_t gr = 0; gr < bind_state.internal_group_names.size(); gr++) {
		subquery_stage2->groups.group_expressions.push_back(make_uniq<ConstantExpression>(
		    Value::INTEGER(UnsafeNumericCast<int32_t>(subquery_stage2->select_list.size() + 1))));
		auto group_reference = make_uniq<ColumnRefExpression>(bind_state.internal_group_names[gr]);
		group_reference->SetAlias(bind_state.internal_group_names[gr]);
		subquery_stage2->select_list.push_back(std::move(group_reference));
	}

	// construct the list aggregates
	for (idx_t aggr = 0; aggr < bind_state.internal_aggregate_names.size(); aggr++) {
		auto colref = make_uniq<ColumnRefExpression>(bind_state.internal_aggregate_names[aggr]);
		vector<unique_ptr<ParsedExpression>> list_children;
		list_children.push_back(std::move(colref));
		auto aggregate = make_uniq<FunctionExpression>("list", std::move(list_children));
		aggregate->SetAlias(bind_state.internal_aggregate_names[aggr]);
		subquery_stage2->select_list.push_back(std::move(aggregate));
	}
	// construct the pivot list
	auto pivot_name = "__internal_pivot_name";
	unique_ptr<ParsedExpression> expr;
	for (auto &pivot : ref.pivots) {
		for (auto &pivot_expr : pivot.pivot_expressions) {
			// coalesce(pivot::VARCHAR, 'NULL')
			auto coalesce = ConstructPivotExpression(std::move(pivot_expr));
			if (!expr) {
				expr = std::move(coalesce);
			} else {
				// string concat
				vector<unique_ptr<ParsedExpression>> concat_children;
				concat_children.push_back(std::move(expr));
				concat_children.push_back(make_uniq<ConstantExpression>(Value("_")));
				concat_children.push_back(std::move(coalesce));
				auto concat = make_uniq<FunctionExpression>("concat", std::move(concat_children));
				expr = std::move(concat);
			}
		}
	}
	// list(coalesce)
	vector<unique_ptr<ParsedExpression>> list_children;
	list_children.push_back(std::move(expr));
	auto aggregate = make_uniq<FunctionExpression>("list", std::move(list_children));

	aggregate->SetAlias(pivot_name);
	subquery_stage2->select_list.push_back(std::move(aggregate));

	subquery_stage2->from_table = std::move(subquery_ref);
	return subquery_stage2;
}

void ReplacePivotColumnRef(ParsedExpression &root_expr, const string &name) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
	    root_expr, [&](ColumnRefExpression &colref) { colref.column_names[0] = name; });
}

static unique_ptr<SelectNode> PivotFinalOperator(PivotBindState &bind_state, PivotRef &ref,
                                                 unique_ptr<SelectNode> subquery,
                                                 vector<PivotValueElement> pivot_values) {
	auto final_pivot_operator = make_uniq<SelectNode>();
	// wrap the subquery of stage 1
	auto subquery_select = make_uniq<SelectStatement>();
	subquery_select->node = std::move(subquery);
	auto subquery_ref = make_uniq<SubqueryRef>(std::move(subquery_select));

	auto bound_pivot = make_uniq<PivotRef>();
	bound_pivot->bound_pivot_values = std::move(pivot_values);
	bound_pivot->bound_group_names = std::move(bind_state.group_names);
	bound_pivot->bound_aggregate_names = std::move(bind_state.aggregate_names);
	bound_pivot->source = std::move(subquery_ref);

	for (auto &group_name : bound_pivot->bound_group_names) {
		final_pivot_operator->select_list.push_back(make_uniq<ColumnRefExpression>(group_name));
	}
	// gather aggregate names
	vector<string> aggregate_names;
	for (auto &pivot_value : bound_pivot->bound_pivot_values) {
		for (idx_t aggr_idx = 0; aggr_idx < ref.aggregates.size(); aggr_idx++) {
			auto aggr = ref.aggregates[aggr_idx]->Copy();
			auto &aggr_name = bound_pivot->bound_aggregate_names[aggr_idx];
			auto pivot_aggr_name = pivot_value.name + aggr_name;
			aggregate_names.push_back(std::move(pivot_aggr_name));
		}
	}
	QueryResult::DeduplicateColumns(aggregate_names);

	idx_t aggr_name_idx = 0;
	for (idx_t pivot_idx = 0; pivot_idx < bound_pivot->bound_pivot_values.size(); pivot_idx++) {
		for (idx_t aggr_idx = 0; aggr_idx < ref.aggregates.size(); aggr_idx++) {
			auto aggr = ref.aggregates[aggr_idx]->Copy();
			auto &pivot_aggr_name = aggregate_names[aggr_name_idx++];
			// replace column ref with name
			ReplacePivotColumnRef(*aggr, pivot_aggr_name);
			aggr->alias = pivot_aggr_name;

			final_pivot_operator->select_list.push_back(std::move(aggr));
		}
	}

	final_pivot_operator->from_table = std::move(bound_pivot);
	return final_pivot_operator;
}

void ExtractPivotAggregates(BoundTableRef &node, vector<unique_ptr<Expression>> &aggregates) {
	if (node.type != TableReferenceType::SUBQUERY) {
		throw InternalException("Pivot - Expected a subquery");
	}
	auto &subq = node.Cast<BoundSubqueryRef>();
	if (subq.subquery->type != QueryNodeType::SELECT_NODE) {
		throw InternalException("Pivot - Expected a select node");
	}
	auto &select = subq.subquery->Cast<BoundSelectNode>();
	if (select.from_table->type != TableReferenceType::SUBQUERY) {
		throw InternalException("Pivot - Expected another subquery");
	}
	auto &subq2 = select.from_table->Cast<BoundSubqueryRef>();
	if (subq2.subquery->type != QueryNodeType::SELECT_NODE) {
		throw InternalException("Pivot - Expected another select node");
	}
	auto &select2 = subq2.subquery->Cast<BoundSelectNode>();
	for (auto &aggr : select2.aggregates) {
		if (aggr->GetAlias() == "__collated_group") {
			continue;
		}
		aggregates.push_back(aggr->Copy());
	}
}

string GetPivotAggregateName(const PivotValueElement &pivot_value, const string &aggr_name, idx_t aggregate_count) {
	auto name = pivot_value.name;
	if (aggregate_count > 1 || !aggr_name.empty()) {
		// if there are multiple aggregates specified we add the name of the aggregate as well
		name += "_" + aggr_name;
	}
	return name;
}

unique_ptr<BoundTableRef> Binder::BindBoundPivot(PivotRef &ref) {
	// bind the child table in a child binder
	auto result = make_uniq<BoundPivotRef>();
	result->bind_index = GenerateTableIndex();
	result->child_binder = Binder::CreateBinder(context, this);
	result->child = result->child_binder->Bind(*ref.source);

	auto &aggregates = result->bound_pivot.aggregates;
	ExtractPivotAggregates(*result->child, aggregates);
	if (aggregates.size() != ref.bound_aggregate_names.size()) {
		throw InternalException("Pivot aggregate count mismatch (expected %llu, found %llu)",
		                        ref.bound_aggregate_names.size(), aggregates.size());
	}

	vector<string> child_names;
	vector<LogicalType> child_types;
	result->child_binder->bind_context.GetTypesAndNames(child_names, child_types);

	vector<string> names;
	vector<LogicalType> types;
	// emit the groups
	for (idx_t i = 0; i < ref.bound_group_names.size(); i++) {
		names.push_back(ref.bound_group_names[i]);
		types.push_back(child_types[i]);
	}
	// emit the pivot columns
	for (auto &pivot_value : ref.bound_pivot_values) {
		for (idx_t aggr_idx = 0; aggr_idx < ref.bound_aggregate_names.size(); aggr_idx++) {
			auto &aggr = aggregates[aggr_idx];
			auto &aggr_name = ref.bound_aggregate_names[aggr_idx];
			auto name = pivot_value.name + aggr_name;

			string pivot_str;
			for (auto &value : pivot_value.values) {
				auto str = value.ToString();
				if (pivot_str.empty()) {
					pivot_str = std::move(str);
				} else {
					pivot_str += "_" + str;
				}
			}
			result->bound_pivot.pivot_values.push_back(std::move(pivot_str));
			names.push_back(std::move(name));
			types.push_back(aggr->return_type);
		}
	}
	result->bound_pivot.group_count = ref.bound_group_names.size();
	result->bound_pivot.types = types;
	auto subquery_alias = ref.alias.empty() ? "__unnamed_pivot" : ref.alias;
	QueryResult::DeduplicateColumns(names);
	bind_context.AddGenericBinding(result->bind_index, subquery_alias, names, types);

	MoveCorrelatedExpressions(*result->child_binder);
	return std::move(result);
}

unique_ptr<SelectNode> Binder::BindPivot(PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns) {
	// keep track of the columns by which we pivot/aggregate
	// any columns which are not pivoted/aggregated on are added to the GROUP BY clause
	case_insensitive_set_t handled_columns;

	vector<reference<FunctionExpression>> pivot_aggregates;
	// parse the aggregate, and extract the referenced columns from the aggregate
	for (auto &aggr : ref.aggregates) {
		if (aggr->HasSubquery()) {
			throw BinderException(*aggr, "Pivot expression cannot contain subqueries");
		}
		if (aggr->IsWindow()) {
			throw BinderException(*aggr, "Pivot expression cannot contain window functions");
		}
		idx_t aggregate_count = pivot_aggregates.size();
		ExtractPivotAggregateExpression(context, aggr, pivot_aggregates);
		if (pivot_aggregates.size() != aggregate_count + 1) {
			string error_str = pivot_aggregates.size() == aggregate_count
			                       ? "but no aggregates were found"
			                       : "but " + to_string(pivot_aggregates.size() - aggregate_count) + " were found";
			throw BinderException(*aggr, "Pivot expression must contain exactly one aggregate, %s", error_str);
		}
	}
	for (auto &aggr : pivot_aggregates) {
		ExtractPivotExpressions(aggr.get(), handled_columns);
	}

	// first add all pivots to the set of handled columns, and check for duplicates
	idx_t total_pivots = 1;
	for (auto &pivot : ref.pivots) {
		if (!pivot.pivot_enum.empty()) {
			auto &type_entry =
			    Catalog::GetEntry<TypeCatalogEntry>(context, INVALID_CATALOG, INVALID_SCHEMA, pivot.pivot_enum);
			auto type = type_entry.user_type;
			if (type.id() != LogicalTypeId::ENUM) {
				throw BinderException(ref, "Pivot must reference an ENUM type: \"%s\" is of type \"%s\"",
				                      pivot.pivot_enum, type.ToString());
			}
			if (!type.IsComplete()) {
				throw BinderException("ENUM type is incomplete");
			}
			auto enum_size = EnumType::GetSize(type);
			for (idx_t i = 0; i < enum_size; i++) {
				auto enum_value = EnumType::GetValue(Value::ENUM(i, type));
				PivotColumnEntry entry;
				entry.values.emplace_back(enum_value);
				entry.alias = std::move(enum_value);
				pivot.entries.push_back(std::move(entry));
			}
		}
		total_pivots *= pivot.entries.size();
		// add the pivoted column to the columns that have been handled
		for (auto &pivot_name : pivot.pivot_expressions) {
			ExtractPivotExpressions(*pivot_name, handled_columns);
		}
		value_set_t pivots;
		for (auto &entry : pivot.entries) {
			D_ASSERT(!entry.expr);
			Value val;
			if (entry.values.size() == 1) {
				val = entry.values[0];
			} else {
				val = Value::LIST(LogicalType::VARCHAR, entry.values);
			}
			if (pivots.find(val) != pivots.end()) {
				throw BinderException(ref, "The value \"%s\" was specified multiple times in the IN clause",
				                      val.ToString());
			}
			if (entry.values.size() != pivot.pivot_expressions.size()) {
				throw BinderException(ref, "PIVOT IN list - inconsistent amount of rows - expected %d but got %d",
				                      pivot.pivot_expressions.size(), entry.values.size());
			}
			pivots.insert(val);
		}
	}
	auto pivot_limit = DBConfig::GetSetting<PivotLimitSetting>(context);
	if (total_pivots >= pivot_limit) {
		throw BinderException(ref, "Pivot column limit of %llu exceeded. Use SET pivot_limit=X to increase the limit.",
		                      pivot_limit);
	}

	// construct the required pivot values recursively
	vector<PivotValueElement> pivot_values;
	ConstructPivots(ref, pivot_values);

	unique_ptr<SelectNode> pivot_node;
	// pivots have three components
	// - the pivots (i.e. future column names)
	// - the groups (i.e. the future row names
	// - the aggregates (i.e. the values of the pivot columns)

	// we have two ways of executing a pivot statement
	// (1) the straightforward manner of filtered aggregates SUM(..) FILTER (pivot_value=X)
	// (2) computing the aggregates once, then using LIST to group the aggregates together with the PIVOT operator
	// -> filtered aggregates are faster when there are FEW pivot values
	// -> LIST is faster when there are MANY pivot values
	// we switch dynamically based on the number of pivots to compute
	auto pivot_filter_threshold = DBConfig::GetSetting<PivotFilterThresholdSetting>(context);
	if (pivot_values.size() <= pivot_filter_threshold) {
		// use a set of filtered aggregates
		pivot_node =
		    PivotFilteredAggregate(context, ref, std::move(all_columns), handled_columns, std::move(pivot_values));
	} else {
		// executing a pivot statement happens in three stages
		// 1) execute the query "SELECT {groups}, {pivots}, {aggregates} FROM {from_clause} GROUP BY {groups}, {pivots}
		// this computes all values that are required in the final result, but not yet in the correct orientation
		// 2) execute the query "SELECT {groups}, LIST({pivots}), LIST({aggregates}) FROM [Q1] GROUP BY {groups}
		// this pushes all pivots and aggregates that belong to a specific group together in an aligned manner
		// 3) push a PIVOT operator, that performs the actual pivoting of the values into the different columns

		PivotBindState bind_state;
		// Pivot Stage 1
		// SELECT {groups}, {pivots}, {aggregates} FROM {from_clause} GROUP BY {groups}, {pivots}
		auto subquery_stage1 = PivotInitialAggregate(context, bind_state, ref, std::move(all_columns), handled_columns);

		// Pivot stage 2
		// SELECT {groups}, LIST({pivots}), LIST({aggregates}) FROM [Q1] GROUP BY {groups}
		auto subquery_stage2 = PivotListAggregate(bind_state, ref, std::move(subquery_stage1));

		// Pivot stage 3
		// construct the final pivot operator
		pivot_node = PivotFinalOperator(bind_state, ref, std::move(subquery_stage2), std::move(pivot_values));
	}
	return pivot_node;
}

struct UnpivotEntry {
	string alias;
	vector<string> column_names;
	vector<unique_ptr<ParsedExpression>> expressions;
};

void Binder::ExtractUnpivotEntries(Binder &child_binder, PivotColumnEntry &entry,
                                   vector<UnpivotEntry> &unpivot_entries) {
	if (!entry.expr) {
		// pivot entry without an expression - generate one
		UnpivotEntry unpivot_entry;
		unpivot_entry.alias = entry.alias;
		for (auto &val : entry.values) {
			auto column_name = val.ToString();
			if (column_name.empty()) {
				throw BinderException("UNPIVOT - empty column name not supported");
			}
			unpivot_entry.expressions.push_back(make_uniq<ColumnRefExpression>(column_name));
		}
		unpivot_entries.push_back(std::move(unpivot_entry));
		return;
	}
	D_ASSERT(entry.values.empty());
	// expand star expressions (if any)
	vector<unique_ptr<ParsedExpression>> star_columns;
	child_binder.ExpandStarExpression(std::move(entry.expr), star_columns);

	for (auto &expr : star_columns) {
		// create one pivot entry per result column
		UnpivotEntry unpivot_entry;
		if (!expr->GetAlias().empty()) {
			unpivot_entry.alias = expr->GetAlias();
		}
		unpivot_entry.expressions.push_back(std::move(expr));
		unpivot_entries.push_back(std::move(unpivot_entry));
	}
}

void Binder::ExtractUnpivotColumnName(ParsedExpression &expr, vector<string> &result) {
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		result.push_back(colref.GetColumnName());
		return;
	}
	if (expr.GetExpressionType() == ExpressionType::SUBQUERY) {
		throw BinderException(expr, "UNPIVOT list cannot contain subqueries");
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { ExtractUnpivotColumnName(child, result); });
}

unique_ptr<SelectNode> Binder::BindUnpivot(Binder &child_binder, PivotRef &ref,
                                           vector<unique_ptr<ParsedExpression>> all_columns,
                                           unique_ptr<ParsedExpression> &where_clause) {
	D_ASSERT(ref.groups.empty());
	D_ASSERT(ref.pivots.size() == 1);

	auto select_node = make_uniq<SelectNode>();
	select_node->from_table = std::move(ref.source);

	// handle the pivot
	auto &unpivot = ref.pivots[0];

	// handle star expressions in any entries
	vector<UnpivotEntry> unpivot_entries;
	for (auto &entry : unpivot.entries) {
		ExtractUnpivotEntries(child_binder, entry, unpivot_entries);
	}
	if (unpivot_entries.empty()) {
		throw BinderException(ref, "UNPIVOT clause must unpivot on at least one column - zero were provided");
	}

	case_insensitive_set_t handled_columns;
	case_insensitive_map_t<string> name_map;
	for (auto &entry : unpivot_entries) {
		for (auto &unpivot_expr : entry.expressions) {
			vector<string> result;
			ExtractUnpivotColumnName(*unpivot_expr, result);
			if (result.empty()) {

				throw BinderException(
				    *unpivot_expr,
				    "UNPIVOT clause must contain exactly one column - expression \"%s\" does not contain any",
				    unpivot_expr->ToString());
			}
			if (result.size() > 1) {
				throw BinderException(
				    *unpivot_expr,
				    "UNPIVOT clause must contain exactly one column - expression \"%s\" contains multiple (%s)",
				    unpivot_expr->ToString(), StringUtil::Join(result, ", "));
			}
			handled_columns.insert(result[0]);
			entry.column_names.push_back(std::move(result[0]));
		}
	}

	vector<string> select_names;
	for (auto &col_expr : all_columns) {
		if (col_expr->GetExpressionType() != ExpressionType::COLUMN_REF) {
			throw InternalException("Unexpected child of pivot source - not a ColumnRef");
		}
		auto &columnref = col_expr->Cast<ColumnRefExpression>();
		auto &column_name = columnref.GetColumnName();
		auto entry = handled_columns.find(column_name);
		if (entry == handled_columns.end()) {
			// not handled - add to the set of regularly selected columns
			select_names.push_back(col_expr->GetName());
			select_node->select_list.push_back(std::move(col_expr));
		} else {
			name_map[column_name] = column_name;
			handled_columns.erase(entry);
		}
	}
	if (!handled_columns.empty()) {
		for (auto &entry : handled_columns) {
			throw BinderException(
			    ref, "Column \"%s\" referenced in UNPIVOT but no matching entry was found in the table", entry);
		}
	}
	vector<Value> unpivot_names;
	for (auto &entry : unpivot_entries) {
		string generated_name;
		D_ASSERT(entry.expressions.size() == entry.column_names.size());
		for (idx_t c = 0; c < entry.expressions.size(); c++) {
			auto name_entry = name_map.find(entry.column_names[c]);
			if (name_entry == name_map.end()) {
				throw InternalException("Unpivot - could not find column name in name map");
			}
			if (!generated_name.empty()) {
				generated_name += "_";
			}
			generated_name += name_entry->second;
		}
		unpivot_names.emplace_back(!entry.alias.empty() ? entry.alias : generated_name);
	}
	vector<vector<unique_ptr<ParsedExpression>>> unpivot_expressions;
	for (idx_t v_idx = 1; v_idx < unpivot_entries.size(); v_idx++) {
		if (unpivot_entries[v_idx].expressions.size() != unpivot_entries[0].expressions.size()) {
			throw BinderException(
			    ref,
			    "UNPIVOT value count mismatch - entry has %llu values, but expected all entries to have %llu values",
			    unpivot_entries[v_idx].expressions.size(), unpivot_entries[0].expressions.size());
		}
	}

	for (idx_t v_idx = 0; v_idx < unpivot_entries[0].expressions.size(); v_idx++) {
		vector<unique_ptr<ParsedExpression>> expressions;
		expressions.reserve(unpivot_entries.size());
		for (auto &entry : unpivot_entries) {
			expressions.push_back(std::move(entry.expressions[v_idx]));
		}
		unpivot_expressions.push_back(std::move(expressions));
	}

	idx_t column_count = select_names.size();
	idx_t unnest_count = unpivot_expressions.size();
	// add the names for the generated unpivot lists
	select_names.push_back("unpivot_names");
	for (idx_t i = 0; i < unpivot_expressions.size(); i++) {
		if (i > 0) {
			select_names.push_back("unpivot_list_" + std::to_string(i + 1));
		} else {
			select_names.push_back("unpivot_list");
		}
	}

	// now de-duplicate the names
	QueryResult::DeduplicateColumns(select_names);

	// construct the UNNEST expression for the set of names (constant)
	auto unpivot_list = Value::LIST(LogicalType::VARCHAR, std::move(unpivot_names));
	auto unpivot_name_expr = make_uniq<ConstantExpression>(std::move(unpivot_list));
	unpivot_name_expr->alias = select_names[column_count];
	select_node->select_list.push_back(std::move(unpivot_name_expr));

	// construct the unpivot lists for the set of unpivoted columns
	if (ref.unpivot_names.size() != unpivot_expressions.size()) {
		throw BinderException(ref, "UNPIVOT name count mismatch - got %d names but %d expressions",
		                      ref.unpivot_names.size(), unpivot_expressions.size());
	}
	for (idx_t i = 0; i < unpivot_expressions.size(); i++) {
		auto list_expr = make_uniq<FunctionExpression>("unpivot_list", std::move(unpivot_expressions[i]));
		list_expr->alias = select_names[column_count + 1 + i];
		select_node->select_list.push_back(std::move(list_expr));
	}

	// move the unpivot lists into a subquery
	auto result_node = make_uniq<SelectNode>();
	auto sub_select = make_uniq<SelectStatement>();
	sub_select->node = std::move(select_node);
	auto subquery = make_uniq<SubqueryRef>(std::move(sub_select));
	subquery->alias = "unpivot";

	result_node->from_table = std::move(subquery);

	// construct the final UNNEST calls which generate the final unpivot result
	for (idx_t i = 0; i < column_count; i++) {
		auto select_col = make_uniq<ColumnRefExpression>(std::move(select_names[i]));
		result_node->select_list.push_back(std::move(select_col));
	}

	auto unpivot_name_list = make_uniq<ColumnRefExpression>(std::move(select_names[column_count]));
	vector<unique_ptr<ParsedExpression>> unnest_name_children;
	unnest_name_children.push_back(std::move(unpivot_name_list));
	auto unnest_name_expr = make_uniq<FunctionExpression>("unnest", std::move(unnest_name_children));
	unnest_name_expr->SetAlias(unpivot.unpivot_names[0]);
	result_node->select_list.push_back(std::move(unnest_name_expr));

	for (idx_t i = 0; i < unnest_count; i++) {
		auto unpivot_internal_name = std::move(select_names[column_count + 1 + i]);

		auto unpivot_list_ref = make_uniq<ColumnRefExpression>(std::move(unpivot_internal_name));
		vector<unique_ptr<ParsedExpression>> unnest_val_children;
		unnest_val_children.push_back(std::move(unpivot_list_ref));
		auto unnest_val_expr = make_uniq<FunctionExpression>("unnest", std::move(unnest_val_children));
		auto unnest_name = i < ref.column_name_alias.size() ? ref.column_name_alias[i] : ref.unpivot_names[i];
		unnest_val_expr->SetAlias(unnest_name);
		result_node->select_list.push_back(std::move(unnest_val_expr));
		if (!ref.include_nulls) {
			// if we are running with EXCLUDE NULLS we need to add an IS NOT NULL filter
			auto colref = make_uniq<ColumnRefExpression>(unnest_name);
			auto filter = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(colref));
			if (where_clause) {
				where_clause = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND,
				                                                std::move(where_clause), std::move(filter));
			} else {
				where_clause = std::move(filter);
			}
		}
	}
	return result_node;
}

unique_ptr<BoundTableRef> Binder::Bind(PivotRef &ref) {
	if (!ref.source) {
		throw InternalException("Pivot without a source!?");
	}
	if (!ref.bound_pivot_values.empty() || !ref.bound_group_names.empty() || !ref.bound_aggregate_names.empty()) {
		// bound pivot
		return BindBoundPivot(ref);
	}

	// bind the source of the pivot
	// we need to do this to be able to expand star expressions
	if (ref.source->type == TableReferenceType::SUBQUERY && ref.source->alias.empty()) {
		ref.source->alias = "__internal_pivot_alias_" + to_string(GenerateTableIndex());
	}
	auto copied_source = ref.source->Copy();
	auto star_binder = Binder::CreateBinder(context, this);
	star_binder->Bind(*copied_source);

	// figure out the set of column names that are in the source of the pivot
	vector<unique_ptr<ParsedExpression>> all_columns;
	star_binder->ExpandStarExpression(make_uniq<StarExpression>(), all_columns);

	unique_ptr<SelectNode> select_node;
	unique_ptr<ParsedExpression> where_clause;
	if (!ref.aggregates.empty()) {
		select_node = BindPivot(ref, std::move(all_columns));
	} else {
		select_node = BindUnpivot(*star_binder, ref, std::move(all_columns), where_clause);
	}
	// bind the generated select node
	auto child_binder = Binder::CreateBinder(context, this);
	auto bound_select_node = child_binder->BindNode(*select_node);
	auto root_index = bound_select_node->GetRootIndex();
	BoundQueryNode *bound_select_ptr = bound_select_node.get();

	unique_ptr<BoundTableRef> result;
	MoveCorrelatedExpressions(*child_binder);
	result = make_uniq<BoundSubqueryRef>(std::move(child_binder), std::move(bound_select_node));
	auto subquery_alias = ref.alias.empty() ? "__unnamed_pivot" : ref.alias;
	SubqueryRef subquery_ref(nullptr, subquery_alias);
	subquery_ref.column_name_alias = std::move(ref.column_name_alias);
	if (where_clause) {
		// if a WHERE clause was provided - bind a subquery holding the WHERE clause
		// we need to bind a new subquery here because the WHERE clause has to be applied AFTER the unnest
		child_binder = Binder::CreateBinder(context, this);
		child_binder->bind_context.AddSubquery(root_index, subquery_ref.alias, subquery_ref, *bound_select_ptr);
		auto where_query = make_uniq<SelectNode>();
		where_query->select_list.push_back(make_uniq<StarExpression>());
		where_query->where_clause = std::move(where_clause);
		bound_select_node = child_binder->BindSelectNode(*where_query, std::move(result));
		bound_select_ptr = bound_select_node.get();
		root_index = bound_select_node->GetRootIndex();
		result = make_uniq<BoundSubqueryRef>(std::move(child_binder), std::move(bound_select_node));
	}
	bind_context.AddSubquery(root_index, subquery_ref.alias, subquery_ref, *bound_select_ptr);
	return result;
}

} // namespace duckdb
