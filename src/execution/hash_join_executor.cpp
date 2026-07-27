//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// hash_join_executor.cpp
//
// Identification: src/execution/hash_join_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/hash_join_executor.h"
#include <memory>
#include <utility>
#include <vector>
#include "common/macros.h"
#include "type/value_factory.h"

namespace bustub {

/**
 * Construct a new HashJoinExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The HashJoin join plan to be executed
 * @param left_child The child executor that produces tuples for the left side of join
 * @param right_child The child executor that produces tuples for the right side of join
 */
HashJoinExecutor::HashJoinExecutor(ExecutorContext *exec_ctx, const HashJoinPlanNode *plan,
                                   std::unique_ptr<AbstractExecutor> &&left_child,
                                   std::unique_ptr<AbstractExecutor> &&right_child)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      left_executor_(std::move(left_child)),
      right_executor_(std::move(right_child)) {
  if (plan->GetJoinType() != JoinType::LEFT && plan->GetJoinType() != JoinType::INNER) {
    // Note for Spring 2025: You ONLY need to implement left join and inner join.
    throw bustub::NotImplementedException(fmt::format("join type {} not supported", plan->GetJoinType()));
  }
}

/** Initialize the join */
void HashJoinExecutor::Init() {
  left_executor_->Init();
  right_executor_->Init();

  // ==== P3 STEP 15: 哈希连接的「建表 / 探测」两阶段 ====
  //
  //   建表 (build)  ：把**右**表全部读入，按连接键建一张哈希表。
  //   探测 (probe)  ：流式读左表，每条元组算出键，去哈希表里 O(1) 查匹配。
  //
  // 复杂度对比（N = 左表行数，M = 右表行数）：
  //   嵌套循环连接：O(N × M)      —— 每条左元组都要扫一遍右表
  //   哈希连接    ：O(N + M)      —— 每条元组只被处理一次
  //
  // 代价：① 右表必须能装进内存（真实系统用 Grace Hash Join 分区落盘解决）；
  //       ② **只能处理等值连接**——哈希表天然只支持"键相等"的查找，
  //          `a < b` 这种条件无法转成哈希探测。这正是优化器的
  //          OptimizeNLJAsHashJoin 规则要先检查连接条件形状的原因。
  hash_table_.clear();
  std::vector<Tuple> tuples;
  std::vector<RID> rids;
  const auto &right_schema = right_executor_->GetOutputSchema();
  while (right_executor_->Next(&tuples, &rids, BUSTUB_BATCH_SIZE)) {
    for (auto &tuple : tuples) {
      auto key = MakeJoinKey(&tuple, right_schema, plan_->RightJoinKeyExpressions());
      hash_table_[key].push_back(tuple);
    }
  }

  left_tuples_.clear();
  left_rids_.clear();
  left_idx_ = 0;
  match_idx_ = 0;
  left_done_ = false;
}

/**
 * Yield the next tuple batch from the hash join.
 * @param[out] tuple_batch The next tuple batch produced by the hash join
 * @param[out] rid_batch The next tuple RID batch produced by the hash join
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 */
auto HashJoinExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                            size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  const auto &left_schema = left_executor_->GetOutputSchema();
  const auto &right_schema = right_executor_->GetOutputSchema();

  auto emit = [&](const Tuple &left, const Tuple *right) {
    std::vector<Value> values;
    values.reserve(left_schema.GetColumnCount() + right_schema.GetColumnCount());
    for (uint32_t i = 0; i < left_schema.GetColumnCount(); i++) {
      values.push_back(left.GetValue(&left_schema, i));
    }
    for (uint32_t i = 0; i < right_schema.GetColumnCount(); i++) {
      values.push_back(right == nullptr ? ValueFactory::GetNullValueByType(right_schema.GetColumn(i).GetType())
                                        : right->GetValue(&right_schema, i));
    }
    tuple_batch->emplace_back(values, &GetOutputSchema());
    rid_batch->emplace_back();
  };

  while (tuple_batch->size() < batch_size) {
    if (left_idx_ >= left_tuples_.size()) {
      if (left_done_ || !left_executor_->Next(&left_tuples_, &left_rids_, batch_size)) {
        left_done_ = true;
        break;
      }
      left_idx_ = 0;
      match_idx_ = 0;
      continue;
    }

    const auto &left_tuple = left_tuples_[left_idx_];
    auto key = MakeJoinKey(&left_tuple, left_schema, plan_->LeftJoinKeyExpressions());
    auto it = hash_table_.find(key);

    if (it == hash_table_.end()) {
      // 一条都没匹配上：INNER 丢弃，LEFT 补一行 NULL。
      if (plan_->GetJoinType() == JoinType::LEFT) {
        emit(left_tuple, nullptr);
      }
      ++left_idx_;
      match_idx_ = 0;
      continue;
    }

    // 同一个键可能对应多条右表元组，逐条输出。
    // match_idx_ 是成员变量：一批填满后要能从桶内的同一位置继续。
    const auto &bucket = it->second;
    while (match_idx_ < bucket.size() && tuple_batch->size() < batch_size) {
      emit(left_tuple, &bucket[match_idx_]);
      ++match_idx_;
    }
    if (match_idx_ >= bucket.size()) {
      ++left_idx_;
      match_idx_ = 0;
    }
  }

  return !tuple_batch->empty();
}

}  // namespace bustub
