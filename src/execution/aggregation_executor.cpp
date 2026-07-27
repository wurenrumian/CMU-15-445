//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// aggregation_executor.cpp
//
// Identification: src/execution/aggregation_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <utility>
#include <vector>
#include "common/macros.h"

#include "execution/executors/aggregation_executor.h"

namespace bustub {

/**
 * Construct a new AggregationExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The insert plan to be executed
 * @param child_executor The child executor from which inserted tuples are pulled (may be `nullptr`)
 */
AggregationExecutor::AggregationExecutor(ExecutorContext *exec_ctx, const AggregationPlanNode *plan,
                                         std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      child_executor_(std::move(child_executor)),
      aht_(plan_->GetAggregates(), plan_->GetAggregateTypes()),
      aht_iterator_(aht_.Begin()) {}

/** Initialize the aggregation */
void AggregationExecutor::Init() {
  child_executor_->Init();
  aht_.Clear();
  finished_ = false;

  // ==== P3 STEP 10: 聚合是彻底的「阻塞算子」====
  // 在看完**最后一条**输入之前，任何一个分组的结果都不能确定
  // （下一条元组随时可能属于某个已有分组并改变它的 SUM/MAX）。
  // 所以 Init() 里就把子算子一次性消费干净，构建出完整的哈希表，
  // Next() 只负责把哈希表里的结果搬出去。
  //
  // 这也是「流水线断点（pipeline breaker）」的典型例子：
  // 聚合、排序、哈希连接的建表侧都必须先物化全部输入。
  std::vector<Tuple> tuples;
  std::vector<RID> rids;
  while (child_executor_->Next(&tuples, &rids, BUSTUB_BATCH_SIZE)) {
    for (auto &tuple : tuples) {
      aht_.InsertCombine(MakeAggregateKey(&tuple), MakeAggregateValue(&tuple));
    }
  }

  aht_iterator_ = aht_.Begin();
}

/**
 * Yield the next tuple batch from the aggregation.
 * @param[out] tuple_batch The next batch of tuples produced by the aggregation
 * @param[out] rid_batch The next batch of tuple RIDs produced by the aggregation
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if any tuples were produced, `false` if there are no more tuples
 */

auto AggregationExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                               size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  if (finished_) {
    return false;
  }

  // ---- 特殊情况：空输入 + 无 GROUP BY ----
  // `SELECT COUNT(*) FROM empty_table` 必须返回**一行** `0`，而不是零行；
  // 但 `SELECT COUNT(*) FROM empty_table GROUP BY x` 应该返回零行。
  // 区别在于：没有 GROUP BY 时，整张表就是"唯一的那一个分组"，
  // 哪怕它是空的，这个分组也存在。
  if (aht_iterator_ == aht_.End() && plan_->GetGroupBys().empty() && aht_.Begin() == aht_.End()) {
    finished_ = true;
    // 用初始聚合值构造这一行：COUNT(*) 是 0，SUM/MIN/MAX/COUNT(col) 是 NULL。
    tuple_batch->emplace_back(aht_.GenerateInitialAggregateValue().aggregates_, &GetOutputSchema());
    // rid_batch 必须与 tuple_batch 等长：上层算子（如 ProjectionExecutor）会用同一个
    // 下标同时访问两个数组。聚合结果是计算出来的、不对应任何物理行，填一个哑 RID 即可。
    rid_batch->emplace_back();
    return true;
  }

  while (aht_iterator_ != aht_.End() && tuple_batch->size() < batch_size) {
    // 输出模式是「分组键在前，聚合结果在后」，与 AggregationPlanNode 的约定一致。
    std::vector<Value> values(aht_iterator_.Key().group_bys_);
    for (const auto &agg : aht_iterator_.Val().aggregates_) {
      values.push_back(agg);
    }
    tuple_batch->emplace_back(values, &GetOutputSchema());
    rid_batch->emplace_back();  // 见上：两个数组必须等长
    ++aht_iterator_;
  }

  if (aht_iterator_ == aht_.End()) {
    finished_ = true;
  }
  return !tuple_batch->empty();
}

/** Do not use or remove this function; otherwise, you will get zero points. */
auto AggregationExecutor::GetChildExecutor() const -> const AbstractExecutor * { return child_executor_.get(); }

}  // namespace bustub
