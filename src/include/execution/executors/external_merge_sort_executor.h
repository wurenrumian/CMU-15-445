//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// external_merge_sort_executor.h
//
// Identification: src/include/execution/executors/external_merge_sort_executor.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <optional>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>
#include "common/config.h"
#include "common/macros.h"
#include "execution/execution_common.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/sort_plan.h"
#include "storage/page/intermediate_result_page.h"
#include "storage/page/page_guard.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * A data structure that holds the sorted tuples as a run during external merge sort.
 * Tuples might be stored in multiple pages, and tuples are ordered both within one page
 * and across pages.
 */
class MergeSortRun {
 public:
  MergeSortRun() = default;
  MergeSortRun(std::vector<page_id_t> pages, BufferPoolManager *bpm) : pages_(std::move(pages)), bpm_(bpm) {}

  auto GetPageCount() -> size_t { return pages_.size(); }

  /**
   * Iterator for iterating on the sorted tuples in one run.
   *
   * ==== P3 STEP 19: 迭代器要同时记住「哪一页」和「页内哪个字节」====
   * 一个有序段横跨多页，且页内元组是变长的，所以位置由 (页下标, 页内字节偏移)
   * 二元组唯一确定。此外还要缓存当前页的读守卫：
   *   - 守卫 pin 住页，防止扫描途中被缓冲池换出；
   *   - 只在跨页时才重新取一次页，页内推进是纯指针运算。
   */
  class Iterator {
    friend class MergeSortRun;

   public:
    Iterator() = default;

    /**
     * Advance the iterator to the next tuple. If the current sort page is exhausted, move to the
     * next sort page.
     */
    auto operator++() -> Iterator & {
      // 先把页内字节偏移推过当前元组，再更新计数。
      // 少了这一步，offset_ 会永远停在页首，解引用总是返回同一条元组。
      guard_->template As<IntermediateResultPage>()->SkipAt(&offset_);
      ++tuple_idx_;
      if (tuple_idx_ >= tuples_in_page_) {
        // 本页读完了，翻到下一页并重置页内偏移。
        ++page_idx_;
        LoadPage();
      }
      return *this;
    }

    /**
     * Dereference the iterator to get the current tuple in the sorted run that the iterator is
     * pointing to.
     */
    auto operator*() -> Tuple {
      // ReadAt 只读不推进 —— 解引用必须无副作用，否则连续两次 *it 会读到不同元组。
      return guard_->template As<IntermediateResultPage>()->ReadAt(offset_);
    }

    /**
     * Checks whether two iterators are pointing to the same tuple in the same sorted run.
     */
    auto operator==(const Iterator &other) const -> bool {
      return run_ == other.run_ && page_idx_ == other.page_idx_ && tuple_idx_ == other.tuple_idx_;
    }

    /**
     * Checks whether two iterators are pointing to different tuples in a sorted run or iterating
     * on different sorted runs.
     */
    auto operator!=(const Iterator &other) const -> bool { return !(*this == other); }

   private:
    explicit Iterator(const MergeSortRun *run) : run_(run) {}

    Iterator(const MergeSortRun *run, size_t page_idx) : run_(run), page_idx_(page_idx) { LoadPage(); }

    /**
     * @brief 取 page_idx_ 指向的页并把游标重置到页首。
     *
     * 跳过空页（正常不会有，但让实现更健壮）；越过最后一页时把守卫置空，
     * 此时迭代器即等于 End()。
     */
    void LoadPage() {
      offset_ = 0;
      tuple_idx_ = 0;
      tuples_in_page_ = 0;
      guard_.reset();
      while (page_idx_ < run_->pages_.size()) {
        auto guard = run_->bpm_->ReadPage(run_->pages_[page_idx_]);
        const auto count = guard.template As<IntermediateResultPage>()->GetTupleCount();
        if (count > 0) {
          tuples_in_page_ = count;
          guard_ = std::make_shared<ReadPageGuard>(std::move(guard));
          return;
        }
        ++page_idx_;
      }
    }

    /** The sorted run that the iterator is iterating on. */
    const MergeSortRun *run_{nullptr};

    /** 当前页在 run_->pages_ 里的下标。等于 pages_.size() 时表示已到末尾。 */
    size_t page_idx_{0};
    /** 当前元组是本页的第几条（用于和 tuples_in_page_ 比较判断翻页）。 */
    uint32_t tuple_idx_{0};
    /** 本页共有多少条元组。 */
    uint32_t tuples_in_page_{0};
    /** 当前元组在本页 data_ 中的字节偏移。 */
    uint32_t offset_{0};

    /**
     * 当前页的读守卫。用 shared_ptr 包一层是因为迭代器需要可拷贝
     * （归并时会在容器里传来传去），而 ReadPageGuard 本身只可移动。
     */
    std::shared_ptr<ReadPageGuard> guard_{nullptr};
  };

  /**
   * Get an iterator pointing to the beginning of the sorted run, i.e. the first tuple.
   */
  auto Begin() -> Iterator { return Iterator{this, 0}; }

  /**
   * Get an iterator pointing to the end of the sorted run, i.e. the position after the last tuple.
   */
  auto End() -> Iterator {
    Iterator it{this};
    it.page_idx_ = pages_.size();
    it.tuple_idx_ = 0;
    return it;
  }

  /** @return 本段涉及的所有页号（归并完成后用来释放它们）。 */
  auto GetPages() const -> const std::vector<page_id_t> & { return pages_; }

 private:
  /** The page IDs of the sort pages that store the sorted tuples. */
  std::vector<page_id_t> pages_;
  /**
   * The buffer pool manager used to read sort pages. The buffer pool manager is responsible for
   * deleting the sort pages when they are no longer needed.
   */
  BufferPoolManager *bpm_{nullptr};
};

/**
 * ExternalMergeSortExecutor executes an external merge sort.
 *
 * In Spring 2025, only 2-way external merge sort is required.
 */
template <size_t K>
class ExternalMergeSortExecutor : public AbstractExecutor {
 public:
  ExternalMergeSortExecutor(ExecutorContext *exec_ctx, const SortPlanNode *plan,
                            std::unique_ptr<AbstractExecutor> &&child_executor);

  void Init() override;

  auto Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
      -> bool override;

  /** @return The output schema for the external merge sort */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); }

 private:
  /** The sort plan node to be executed */
  const SortPlanNode *plan_;

  /** Compares tuples based on the order-bys */
  TupleComparator cmp_;

  /** 提供待排序输入的子算子。 */
  std::unique_ptr<AbstractExecutor> child_executor_;

  /** 归并到最后只剩的那一个有序段，以及它的读游标。 */
  std::optional<MergeSortRun> result_run_{std::nullopt};
  std::optional<MergeSortRun::Iterator> cursor_{std::nullopt};

  /**
   * @brief 把一批已在内存中排好序的元组写成一个有序段（可能跨多页）。
   */
  auto MakeRun(const std::vector<Tuple> &sorted) -> MergeSortRun;

  /**
   * @brief 二路归并两个有序段，产出一个新的有序段，并释放两个输入段的页。
   */
  auto MergeTwoRuns(MergeSortRun &left, MergeSortRun &right) -> MergeSortRun;
};

}  // namespace bustub
