//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// sort_executor.cpp
//
// Identification: src/execution/sort_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/sort_executor.h"
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>
#include "execution/execution_common.h"

namespace bustub {

/**
 * Construct a new SortExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The sort plan to be executed
 */
SortExecutor::SortExecutor(ExecutorContext *exec_ctx, const SortPlanNode *plan,
                           std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

/** Initialize the sort */
void SortExecutor::Init() {
  child_executor_->Init();
  sorted_.clear();
  cursor_ = 0;

  // ==== P3 STEP 23: 内存排序 ====
  // 与 ExternalMergeSortExecutor 的分工：数据能全部装进内存时用这个算子，
  // 装不下才需要外部归并排序把中间结果落到页上。
  // 两者的比较逻辑完全共用 TupleComparator + GenerateSortKey。
  const auto &schema = child_executor_->GetOutputSchema();
  const auto &order_bys = plan_->GetOrderBy();
  TupleComparator cmp{order_bys};

  std::vector<SortEntry> entries;
  std::vector<Tuple> tuples;
  std::vector<RID> rids;
  while (child_executor_->Next(&tuples, &rids, BUSTUB_BATCH_SIZE)) {
    for (const auto &tuple : tuples) {
      entries.emplace_back(GenerateSortKey(tuple, order_bys, schema), tuple);
    }
  }

  std::sort(entries.begin(), entries.end(), cmp);

  sorted_.reserve(entries.size());
  for (auto &entry : entries) {
    sorted_.push_back(std::move(entry.second));
  }
}

/**
 * Yield the next tuple batch from the sort.
 * @param[out] tuple_batch The next tuple batch produced by the sort
 * @param[out] rid_batch The next tuple RID batch produced by the sort
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 */
auto SortExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
    -> bool {
  tuple_batch->clear();
  rid_batch->clear();
  while (cursor_ < sorted_.size() && tuple_batch->size() < batch_size) {
    tuple_batch->push_back(sorted_[cursor_++]);
    rid_batch->emplace_back();
  }
  return !tuple_batch->empty();
}

}  // namespace bustub
