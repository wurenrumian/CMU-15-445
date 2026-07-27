//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// aggregation_executor.h
//
// Identification: src/include/execution/executors/aggregation_executor.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/macros.h"
#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/expressions/abstract_expression.h"
#include "execution/plans/aggregation_plan.h"
#include "storage/table/tuple.h"
#include "type/value_factory.h"

namespace bustub {

/**
 * A simplified hash table that has all the necessary functionality for aggregations.
 */
class SimpleAggregationHashTable {
 public:
  /**
   * Construct a new SimpleAggregationHashTable instance.
   * @param agg_exprs the aggregation expressions
   * @param agg_types the types of aggregations
   */
  SimpleAggregationHashTable(const std::vector<AbstractExpressionRef> &agg_exprs,
                             const std::vector<AggregationType> &agg_types)
      : agg_exprs_{agg_exprs}, agg_types_{agg_types} {}

  /** @return The initial aggregate value for this aggregation executor */
  auto GenerateInitialAggregateValue() -> AggregateValue {
    std::vector<Value> values{};
    for (const auto &agg_type : agg_types_) {
      switch (agg_type) {
        case AggregationType::CountStarAggregate:
          // Count start starts at zero.
          values.emplace_back(ValueFactory::GetIntegerValue(0));
          break;
        case AggregationType::CountAggregate:
        case AggregationType::SumAggregate:
        case AggregationType::MinAggregate:
        case AggregationType::MaxAggregate:
          // Others starts at null.
          values.emplace_back(ValueFactory::GetNullValueByType(TypeId::INTEGER));
          break;
      }
    }
    return {values};
  }

  /**
   * Combines the input into the aggregation result.
   * @param[out] result The output aggregate value
   * @param input The input value
   */
  void CombineAggregateValues(AggregateValue *result, const AggregateValue &input) {
    // ==== P3 STEP 9: 聚合的增量合并规则 ====
    // 每来一条元组就把它「折叠」进已有的中间结果，全程只保留每个分组一份状态，
    // 内存占用是 O(分组数) 而不是 O(行数)。
    //
    // SQL 的 NULL 语义是这里的全部难点：
    //   - COUNT(*) 数的是**行**，NULL 也算，所以无条件 +1。
    //   - COUNT(col) / SUM / MIN / MAX 都**跳过** NULL 输入。
    //   - 这几个的初始值是 NULL（而非 0），表示「还没见过任何非空值」。
    //     所以合并时要先判断「当前结果是不是 NULL」：是的话直接取输入值作为起点，
    //     否则才做真正的累加/比较。
    //     这正是为什么 `SELECT SUM(x) FROM empty_table` 返回 NULL 而不是 0，
    //     而 `SELECT COUNT(*) FROM empty_table` 返回 0。
    for (uint32_t i = 0; i < agg_exprs_.size(); i++) {
      const auto &in = input.aggregates_[i];
      auto &out = result->aggregates_[i];

      switch (agg_types_[i]) {
        case AggregationType::CountStarAggregate:
          // 初始值是整数 0，且不跳过 NULL —— 数的是行数。
          out = out.Add(ValueFactory::GetIntegerValue(1));
          break;

        case AggregationType::CountAggregate:
          if (!in.IsNull()) {
            out = out.IsNull() ? ValueFactory::GetIntegerValue(1) : out.Add(ValueFactory::GetIntegerValue(1));
          }
          break;

        case AggregationType::SumAggregate:
          if (!in.IsNull()) {
            out = out.IsNull() ? in : out.Add(in);
          }
          break;

        case AggregationType::MinAggregate:
          if (!in.IsNull()) {
            out = out.IsNull() ? in : out.Min(in);
          }
          break;

        case AggregationType::MaxAggregate:
          if (!in.IsNull()) {
            out = out.IsNull() ? in : out.Max(in);
          }
          break;
      }
    }
  }

  /**
   * Inserts a value into the hash table and then combines it with the current aggregation.
   * @param agg_key the key to be inserted
   * @param agg_val the value to be inserted
   */
  void InsertCombine(const AggregateKey &agg_key, const AggregateValue &agg_val) {
    if (ht_.count(agg_key) == 0) {
      ht_.insert({agg_key, GenerateInitialAggregateValue()});
    }
    CombineAggregateValues(&ht_[agg_key], agg_val);
  }

  /**
   * Clear the hash table
   */
  void Clear() { ht_.clear(); }

  /** An iterator over the aggregation hash table */
  class Iterator {
   public:
    /** Creates an iterator for the aggregate map. */
    explicit Iterator(std::unordered_map<AggregateKey, AggregateValue>::const_iterator iter) : iter_{iter} {}

    /** @return The key of the iterator */
    auto Key() -> const AggregateKey & { return iter_->first; }

    /** @return The value of the iterator */
    auto Val() -> const AggregateValue & { return iter_->second; }

    /** @return The iterator before it is incremented */
    auto operator++() -> Iterator & {
      ++iter_;
      return *this;
    }

    /** @return `true` if both iterators are identical */
    auto operator==(const Iterator &other) -> bool { return this->iter_ == other.iter_; }

    /** @return `true` if both iterators are different */
    auto operator!=(const Iterator &other) -> bool { return this->iter_ != other.iter_; }

   private:
    /** Aggregates map */
    std::unordered_map<AggregateKey, AggregateValue>::const_iterator iter_;
  };

  /** @return Iterator to the start of the hash table */
  auto Begin() -> Iterator { return Iterator{ht_.cbegin()}; }

  /** @return Iterator to the end of the hash table */
  auto End() -> Iterator { return Iterator{ht_.cend()}; }

 private:
  /** The hash table is just a map from aggregate keys to aggregate values */
  std::unordered_map<AggregateKey, AggregateValue> ht_{};
  /** The aggregate expressions that we have */
  const std::vector<AbstractExpressionRef> &agg_exprs_;
  /** The types of aggregations that we have */
  const std::vector<AggregationType> &agg_types_;
};

/**
 * AggregationExecutor executes an aggregation operation (e.g. COUNT, SUM, MIN, MAX)
 * over the tuples produced by a child executor.
 */
class AggregationExecutor : public AbstractExecutor {
 public:
  AggregationExecutor(ExecutorContext *exec_ctx, const AggregationPlanNode *plan,
                      std::unique_ptr<AbstractExecutor> &&child_executor);

  void Init() override;

  auto Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
      -> bool override;

  /** @return The output schema for the aggregation */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); };

  auto GetChildExecutor() const -> const AbstractExecutor *;

 private:
  /** @return The tuple as an AggregateKey */
  auto MakeAggregateKey(const Tuple *tuple) -> AggregateKey {
    std::vector<Value> keys;
    for (const auto &expr : plan_->GetGroupBys()) {
      keys.emplace_back(expr->Evaluate(tuple, child_executor_->GetOutputSchema()));
    }
    return {keys};
  }

  /** @return The tuple as an AggregateValue */
  auto MakeAggregateValue(const Tuple *tuple) -> AggregateValue {
    std::vector<Value> vals;
    for (const auto &expr : plan_->GetAggregates()) {
      vals.emplace_back(expr->Evaluate(tuple, child_executor_->GetOutputSchema()));
    }
    return {vals};
  }

 private:
  /** The aggregation plan node */
  const AggregationPlanNode *plan_;

  /** The child executor that produces tuples over which the aggregation is computed */
  std::unique_ptr<AbstractExecutor> child_executor_;

  /** Simple aggregation hash table */
  SimpleAggregationHashTable aht_;

  /** Simple aggregation hash table iterator */
  SimpleAggregationHashTable::Iterator aht_iterator_;

  /**
   * @brief 是否已经把结果集全部产出完。
   *
   * 需要它是因为 `aht_iterator_ == aht_.End()` 这个条件在两种情况下都成立：
   * 「还没开始」和「已经结束」。而空表上的无分组聚合还要额外补一行默认值
   *（`SELECT COUNT(*) FROM empty` 必须返回 0，而不是零行），
   * 那一行是在哈希表之外单独产生的，也需要这个标志来保证只产出一次。
   */
  bool finished_{false};
};
}  // namespace bustub
