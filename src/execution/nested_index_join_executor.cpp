//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// nested_index_join_executor.cpp
//
// Identification: src/execution/nested_index_join_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/nested_index_join_executor.h"
#include <memory>
#include <utility>
#include <vector>
#include "common/macros.h"
#include "type/value_factory.h"

namespace bustub {

/**
 * Creates a new nested index join executor.
 * @param exec_ctx the context that the nested index join should be performed in
 * @param plan the nested index join plan to be executed
 * @param child_executor the outer table
 */
NestedIndexJoinExecutor::NestedIndexJoinExecutor(ExecutorContext *exec_ctx, const NestedIndexJoinPlanNode *plan,
                                                 std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {
  if (plan->GetJoinType() != JoinType::LEFT && plan->GetJoinType() != JoinType::INNER) {
    // Note for Spring 2025: You ONLY need to implement left join and inner join.
    throw bustub::NotImplementedException(fmt::format("join type {} not supported", plan->GetJoinType()));
  }
  inner_table_info_ = exec_ctx_->GetCatalog()->GetTable(plan_->GetInnerTableOid());
  index_info_ = exec_ctx_->GetCatalog()->GetIndex(plan_->GetIndexOid());
}

void NestedIndexJoinExecutor::Init() {
  child_executor_->Init();
  outer_tuples_.clear();
  outer_rids_.clear();
  outer_idx_ = 0;
  outer_done_ = false;
}

auto NestedIndexJoinExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                                   size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  const auto &outer_schema = child_executor_->GetOutputSchema();
  const auto &inner_schema = inner_table_info_->schema_;

  auto emit = [&](const Tuple &outer, const Tuple *inner) {
    std::vector<Value> values;
    values.reserve(outer_schema.GetColumnCount() + inner_schema.GetColumnCount());
    for (uint32_t i = 0; i < outer_schema.GetColumnCount(); i++) {
      values.push_back(outer.GetValue(&outer_schema, i));
    }
    for (uint32_t i = 0; i < inner_schema.GetColumnCount(); i++) {
      values.push_back(inner == nullptr ? ValueFactory::GetNullValueByType(inner_schema.GetColumn(i).GetType())
                                        : inner->GetValue(&inner_schema, i));
    }
    tuple_batch->emplace_back(values, &GetOutputSchema());
    rid_batch->emplace_back();
  };

  while (tuple_batch->size() < batch_size) {
    if (outer_idx_ >= outer_tuples_.size()) {
      if (outer_done_ || !child_executor_->Next(&outer_tuples_, &outer_rids_, batch_size)) {
        outer_done_ = true;
        break;
      }
      outer_idx_ = 0;
      continue;
    }

    const auto &outer_tuple = outer_tuples_[outer_idx_++];

    // ==== P3 STEP 14: 索引嵌套循环连接 ====
    // 与普通嵌套循环连接的唯一区别：内表不做全扫描，而是拿外表元组算出连接键，
    // 直接去内表的索引上点查。
    //
    //   普通 NLJ  ：每条外表元组 × 扫描整个内表   → O(N × M)
    //   索引 NLJ  ：每条外表元组 × 一次索引查找   → O(N × log M)
    //
    // 前提是内表的连接列上恰好有索引 —— 这正是优化器规则
    // OptimizeNLJAsIndexJoin 要检查的条件。
    auto key_value = plan_->KeyPredicate()->Evaluate(&outer_tuple, outer_schema);
    std::vector<Value> key_values{key_value};
    Tuple probe_key{key_values, &index_info_->key_schema_};

    std::vector<RID> matches;
    index_info_->index_->ScanKey(probe_key, &matches, exec_ctx_->GetTransaction());

    bool matched = false;
    for (const auto &rid : matches) {
      // 索引只给出 RID，仍需回表取完整的内表元组。
      auto [meta, inner_tuple] = inner_table_info_->table_->GetTuple(rid);
      if (meta.is_deleted_) {
        continue;
      }
      matched = true;
      emit(outer_tuple, &inner_tuple);
    }

    if (!matched && plan_->GetJoinType() == JoinType::LEFT) {
      emit(outer_tuple, nullptr);
    }
  }

  return !tuple_batch->empty();
}

}  // namespace bustub
