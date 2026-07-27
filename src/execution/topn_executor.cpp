//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// topn_executor.cpp
//
// Identification: src/execution/topn_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/topn_executor.h"
#include <algorithm>
#include <memory>
#include <queue>
#include <utility>
#include <vector>
#include "execution/execution_common.h"

namespace bustub {

/**
 * Construct a new TopNExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The TopN plan to be executed
 */
TopNExecutor::TopNExecutor(ExecutorContext *exec_ctx, const TopNPlanNode *plan,
                           std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

/** Initialize the TopN */
void TopNExecutor::Init() {
  child_executor_->Init();
  top_.clear();
  cursor_ = 0;
  heap_size_ = 0;

  const auto &schema = child_executor_->GetOutputSchema();
  const auto &order_bys = plan_->GetOrderBy();
  TupleComparator cmp{order_bys};
  const size_t n = plan_->GetN();
  if (n == 0) {
    return;
  }

  // ==== P3 STEP 24: 为什么 TopN 不是「排序 + 取前 N」====
  // 直接排序是 O(M log M) 时间、O(M) 内存（M = 输入行数）。
  // 但 `ORDER BY x LIMIT 10` 只要前 10 条，完全没必要把剩下的也排好。
  //
  // 用一个**大小恒为 N 的最大堆**：堆顶是"当前前 N 名里最差的那个"。
  // 每来一条元组，只和堆顶比一次：
  //   - 比堆顶差 → 它连前 N 都进不去，直接丢弃；
  //   - 比堆顶好 → 弹出堆顶，把它放进去。
  // 时间降到 O(M log N)，**内存降到 O(N)** —— 后者才是关键：
  // 一亿行里取前 10 条，内存占用与那一亿行无关。
  //
  // 堆的比较器要和期望顺序**相反**：我们要的是升序结果，
  // 就得用最大堆才能在堆顶拿到"最该被淘汰的那个"。
  std::priority_queue<SortEntry, std::vector<SortEntry>, TupleComparator> heap{cmp};

  std::vector<Tuple> tuples;
  std::vector<RID> rids;
  while (child_executor_->Next(&tuples, &rids, BUSTUB_BATCH_SIZE)) {
    for (const auto &tuple : tuples) {
      SortEntry entry{GenerateSortKey(tuple, order_bys, schema), tuple};
      if (heap.size() < n) {
        heap.push(std::move(entry));
      } else if (cmp(entry, heap.top())) {
        // entry 排在堆顶之前 ⇒ 它比当前第 N 名更好，替换掉。
        heap.pop();
        heap.push(std::move(entry));
      }
    }
  }

  heap_size_ = heap.size();

  // 堆是逐个弹出「最差的」，所以倒着填就得到升序结果。
  top_.resize(heap.size());
  for (size_t i = heap.size(); i > 0; --i) {
    top_[i - 1] = heap.top().second;
    heap.pop();
  }
}

/**
 * Yield the next tuple batch from the TopN.
 * @param[out] tuple_batch The next tuple batch produced by the TopN
 * @param[out] rid_batch The next tuple RID batch produced by the TopN
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 */
auto TopNExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
    -> bool {
  tuple_batch->clear();
  rid_batch->clear();
  while (cursor_ < top_.size() && tuple_batch->size() < batch_size) {
    tuple_batch->push_back(top_[cursor_++]);
    rid_batch->emplace_back();
  }
  return !tuple_batch->empty();
}

auto TopNExecutor::GetNumInHeap() -> size_t { return heap_size_; };

}  // namespace bustub
