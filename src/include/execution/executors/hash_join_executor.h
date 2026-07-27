//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// hash_join_executor.h
//
// Identification: src/include/execution/executors/hash_join_executor.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * HashJoinExecutor executes a nested-loop JOIN on two tables.
 */
class HashJoinExecutor : public AbstractExecutor {
 public:
  HashJoinExecutor(ExecutorContext *exec_ctx, const HashJoinPlanNode *plan,
                   std::unique_ptr<AbstractExecutor> &&left_child, std::unique_ptr<AbstractExecutor> &&right_child);

  void Init() override;

  auto Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
      -> bool override;

  /** @return The output schema for the join */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); };

 private:
  /**
   * @brief 连接键。语义与 GROUP BY 键完全一致，NULL 的处理也一样棘手。
   *
   * 复用 AggregateKey 而不是重新定义一个类型，是因为二者需要的能力完全相同：
   * 可哈希 + 可判等，且 NULL 必须和 NULL 相等（见 aggregation_plan.h STEP 11）。
   */
  using JoinKey = AggregateKey;

  /** 从元组中按给定表达式提取连接键。 */
  static auto MakeJoinKey(const Tuple *tuple, const Schema &schema, const std::vector<AbstractExpressionRef> &exprs)
      -> JoinKey {
    std::vector<Value> keys;
    keys.reserve(exprs.size());
    for (const auto &expr : exprs) {
      keys.emplace_back(expr->Evaluate(tuple, schema));
    }
    return {keys};
  }

  /** The HashJoin plan node to be executed. */
  const HashJoinPlanNode *plan_;

  std::unique_ptr<AbstractExecutor> left_executor_;
  std::unique_ptr<AbstractExecutor> right_executor_;

  /**
   * @brief 建表阶段的产物：右表按连接键分好的桶。
   *
   * 一个键可能对应多条右表元组（连接键不必唯一），所以值是 vector。
   */
  std::unordered_map<JoinKey, std::vector<Tuple>> hash_table_;

  /** 探测阶段：当前批左表元组与进度。 */
  std::vector<Tuple> left_tuples_;
  std::vector<RID> left_rids_;
  size_t left_idx_{0};
  /** 当前左表元组匹配到的那个桶里，已经输出到第几条。 */
  size_t match_idx_{0};
  bool left_done_{false};
};

}  // namespace bustub
