//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// external_merge_sort_executor.cpp
//
// Identification: src/execution/external_merge_sort_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/external_merge_sort_executor.h"
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>
#include "common/macros.h"
#include "execution/execution_common.h"
#include "execution/plans/sort_plan.h"

namespace bustub {

template <size_t K>
ExternalMergeSortExecutor<K>::ExternalMergeSortExecutor(ExecutorContext *exec_ctx, const SortPlanNode *plan,
                                                        std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), cmp_(plan->GetOrderBy()), child_executor_(std::move(child_executor)) {}

template <size_t K>
auto ExternalMergeSortExecutor<K>::MakeRun(const std::vector<Tuple> &sorted) -> MergeSortRun {
  auto *bpm = exec_ctx_->GetBufferPoolManager();
  std::vector<page_id_t> pages;

  page_id_t page_id = bpm->NewPage();
  auto guard = bpm->WritePage(page_id);
  auto *page = guard.template AsMut<IntermediateResultPage>();
  page->Init();
  pages.push_back(page_id);

  for (const auto &tuple : sorted) {
    if (!page->Append(tuple)) {
      // 当前页写满了，换一页继续。段内的顺序由页的先后 + 页内的先后共同决定。
      guard.Drop();
      page_id = bpm->NewPage();
      guard = bpm->WritePage(page_id);
      page = guard.template AsMut<IntermediateResultPage>();
      page->Init();
      pages.push_back(page_id);
      // 换到空页后仍然失败，只可能是这条元组比整页还大。此时静默跳过会
      // **无声地丢数据**（排序结果少几行，且没有任何报错），必须让它响亮地崩掉。
      //
      // 注意副作用调用必须写在宏**外面**：BUSTUB_ASSERT 展开成裸 assert()，
      // 在 NDEBUG（Release）下整个表达式会被删掉，元组就再也不会被写进去了。
      const bool appended = page->Append(tuple);
      BUSTUB_ENSURE(appended, "tuple larger than one page cannot be spilled");
    }
  }
  guard.Drop();
  return MergeSortRun{std::move(pages), bpm};
}

template <size_t K>
auto ExternalMergeSortExecutor<K>::MergeTwoRuns(MergeSortRun &left, MergeSortRun &right) -> MergeSortRun {
  auto *bpm = exec_ctx_->GetBufferPoolManager();
  const auto &schema = child_executor_->GetOutputSchema();
  const auto &order_bys = plan_->GetOrderBy();

  std::vector<page_id_t> pages;
  page_id_t page_id = bpm->NewPage();
  auto guard = bpm->WritePage(page_id);
  auto *page = guard.template AsMut<IntermediateResultPage>();
  page->Init();
  pages.push_back(page_id);

  auto emit = [&](const Tuple &tuple) {
    if (!page->Append(tuple)) {
      guard.Drop();
      page_id = bpm->NewPage();
      guard = bpm->WritePage(page_id);
      page = guard.template AsMut<IntermediateResultPage>();
      page->Init();
      pages.push_back(page_id);
      // 同上：副作用不能放进 assert 类宏里。
      const bool appended = page->Append(tuple);
      BUSTUB_ENSURE(appended, "tuple larger than one page cannot be spilled");
    }
  };

  // ==== P3 STEP 20: 二路归并 ====
  // 两个游标各指向一个有序段的当前最小元素，每次取较小的那个输出并推进。
  // 关键性质：**任意时刻只需要在内存里持有两条元组**，与段的长度完全无关。
  // 这正是外部排序能处理远大于内存的数据的根本原因。
  auto lit = left.Begin();
  auto rit = right.Begin();
  auto lend = left.End();
  auto rend = right.End();

  while (lit != lend && rit != rend) {
    auto lt = *lit;
    auto rt = *rit;
    // 比较器吃的是 (排序键, 元组) 二元组，所以现场算一下排序键。
    if (cmp_({GenerateSortKey(rt, order_bys, schema), rt}, {GenerateSortKey(lt, order_bys, schema), lt})) {
      emit(rt);
      ++rit;
    } else {
      // 相等时优先取左段 —— 保证归并是**稳定**的（相等元素维持原有先后）。
      emit(lt);
      ++lit;
    }
  }
  while (lit != lend) {
    emit(*lit);
    ++lit;
  }
  while (rit != rend) {
    emit(*rit);
    ++rit;
  }
  guard.Drop();

  // 两个输入段已经没用了，把它们的页还给缓冲池。
  // 不释放的话，排序过程中占用的页数会随归并趟数线性膨胀。
  for (const auto pid : left.GetPages()) {
    bpm->DeletePage(pid);
  }
  for (const auto pid : right.GetPages()) {
    bpm->DeletePage(pid);
  }

  return MergeSortRun{std::move(pages), bpm};
}

/** Initialize the external merge sort */
template <size_t K>
void ExternalMergeSortExecutor<K>::Init() {
  child_executor_->Init();

  const auto &schema = child_executor_->GetOutputSchema();
  const auto &order_bys = plan_->GetOrderBy();

  // ==== P3 STEP 21: 第一趟 —— 生成初始有序段 ====
  // 每次读进「一批」元组，在内存里排好序，写成一个有序段。
  // 批的大小就是排序能用的内存上限；批越大，初始段越少，后面要归并的趟数越少。
  // 教科书里这一步叫「置换选择」或「内存排序生成初始归并段」。
  std::vector<MergeSortRun> runs;
  std::vector<Tuple> tuples;
  std::vector<RID> rids;

  while (child_executor_->Next(&tuples, &rids, BUSTUB_BATCH_SIZE)) {
    std::vector<SortEntry> entries;
    entries.reserve(tuples.size());
    for (const auto &tuple : tuples) {
      entries.emplace_back(GenerateSortKey(tuple, order_bys, schema), tuple);
    }
    // 先算键再排序：避免在 O(n log n) 次比较里反复求值 ORDER BY 表达式。
    std::sort(entries.begin(), entries.end(), cmp_);

    std::vector<Tuple> sorted;
    sorted.reserve(entries.size());
    for (auto &entry : entries) {
      sorted.push_back(entry.second);
    }
    runs.push_back(MakeRun(sorted));
  }

  if (runs.empty()) {
    // 输入为空：造一个空段，让 Next() 的逻辑不必特判。
    result_run_ = MakeRun({});
    cursor_ = result_run_->Begin();
    return;
  }

  // ==== P3 STEP 22: 后续每一趟 —— 两两归并，段数减半 ====
  // 段数每趟折半，所以总共需要 ceil(log2(初始段数)) 趟，
  // 总 I/O 是 O(N · log_K(N/M))（K = 归并路数，这里是 2；M = 内存能装的页数）。
  // 提高 K 能显著减少趟数，这也是真实系统用多路归并而非二路的原因。
  while (runs.size() > 1) {
    std::vector<MergeSortRun> next_runs;
    for (size_t i = 0; i + 1 < runs.size(); i += 2) {
      next_runs.push_back(MergeTwoRuns(runs[i], runs[i + 1]));
    }
    if (runs.size() % 2 == 1) {
      // 落单的那个段直接带到下一趟，不做无谓的拷贝。
      next_runs.push_back(runs.back());
    }
    runs = std::move(next_runs);
  }

  result_run_ = runs.front();
  cursor_ = result_run_->Begin();
}

/**
 * Yield the next tuple batch from the external merge sort.
 * @param[out] tuple_batch The next tuple batch produced by the external merge sort.
 * @param[out] rid_batch The next tuple RID batch produced by the external merge sort.
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 */
template <size_t K>
auto ExternalMergeSortExecutor<K>::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                                        size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  if (!cursor_.has_value()) {
    return false;
  }

  // 全部排序工作在 Init() 里就做完了，这里只是顺着最终那个有序段往下读。
  auto end = result_run_->End();
  while (*cursor_ != end && tuple_batch->size() < batch_size) {
    tuple_batch->push_back(**cursor_);
    rid_batch->emplace_back();  // 排序结果不对应任何物理行。
    ++(*cursor_);
  }

  return !tuple_batch->empty();
}

template class ExternalMergeSortExecutor<2>;

}  // namespace bustub
