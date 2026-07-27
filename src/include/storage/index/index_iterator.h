//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// index_iterator.h
//
// Identification: src/include/storage/index/index_iterator.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

/**
 * index_iterator.h
 * For range scan of b+ tree
 */
#pragma once
#include <memory>
#include <optional>
#include <utility>
#include "buffer/traced_buffer_pool_manager.h"
#include "common/config.h"
#include "common/macros.h"
#include "storage/page/b_plus_tree_leaf_page.h"

namespace bustub {

#define INDEXITERATOR_TYPE IndexIterator<KeyType, ValueType, KeyComparator, NumTombs>
#define SHORT_INDEXITERATOR_TYPE IndexIterator<KeyType, ValueType, KeyComparator>

/**
 * @brief B+ 树的正向迭代器，用于范围扫描。
 *
 * ==== P2 STEP 9: 迭代器为什么要一直握着 ReadPageGuard ====
 * `operator*` 返回的是**指向页内数据的引用**（`std::pair<const K&, const V&>`）。
 * 只要这个引用还可能被使用，承载它的那一帧就绝不能被缓冲池淘汰——
 * 否则引用会指向一个已经装了别的页的帧，读出完全无关的字节。
 *
 * 持有 `ReadPageGuard` 同时解决了两件事：
 *   1. pin 住该帧，杜绝淘汰；
 *   2. 持共享锁，保证扫描期间没人在改这一页。
 *
 * 代价是：迭代器活着期间会一直占用一个帧。缓冲池很小（测试里常设 5~50 帧）时，
 * 同时打开太多迭代器会耗尽缓冲池。这是索引扫描的固有代价，不是实现缺陷。
 */
FULL_INDEX_TEMPLATE_ARGUMENTS_DEFN
class IndexIterator {
  using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator, NumTombs>;

 public:
  /** @brief 默认构造出的是「尾后迭代器」，等价于 End()。 */
  IndexIterator() = default;

  /**
   * @brief 从某个叶子页的某个下标开始迭代。
   *
   * 构造时会立刻跳过墓碑条目，因此迭代器一旦构造完成，
   * 要么指向一个真实存活的条目，要么已经等于 End()。
   */
  IndexIterator(std::shared_ptr<TracedBufferPoolManager> bpm, ReadPageGuard guard, int index)
      : bpm_(std::move(bpm)), guard_(std::move(guard)), index_(index) {
    SkipDeleted();
  }

  ~IndexIterator() = default;

  IndexIterator(const IndexIterator &) = delete;
  auto operator=(const IndexIterator &) -> IndexIterator & = delete;
  IndexIterator(IndexIterator &&) noexcept = default;
  auto operator=(IndexIterator &&) noexcept -> IndexIterator & = default;

  auto IsEnd() -> bool { return !guard_.has_value(); }

  auto operator*() -> std::pair<const KeyType &, const ValueType &> {
    BUSTUB_ASSERT(guard_.has_value(), "dereferencing an end iterator");
    const auto *leaf = guard_->template As<LeafPage>();
    return {leaf->KeyRefAt(index_), leaf->ValueRefAt(index_)};
  }

  auto operator++() -> IndexIterator & {
    BUSTUB_ASSERT(guard_.has_value(), "incrementing an end iterator");
    ++index_;
    SkipDeleted();
    return *this;
  }

  auto operator==(const IndexIterator &itr) const -> bool {
    // 两个尾后迭代器相等；一个是尾后、一个不是则不等。
    if (!guard_.has_value() || !itr.guard_.has_value()) {
      return guard_.has_value() == itr.guard_.has_value();
    }
    return guard_->GetPageId() == itr.guard_->GetPageId() && index_ == itr.index_;
  }

  auto operator!=(const IndexIterator &itr) const -> bool { return !(*this == itr); }

 private:
  /**
   * @brief 向前推进，直到落在一个存活条目上，或走到整棵树的末尾。
   *
   * 两层跳过：
   *   1. 页内：跳过被墓碑标记的条目（它们物理存在但逻辑上已删除）。
   *   2. 跨页：本页扫完就顺着叶子链表 next_page_id_ 走到下一页。
   *      叶子链表的存在使范围扫描不必回到根节点重新下降，这正是 B+ 树
   *      （相对 B 树）为范围查询做的关键优化。
   */
  void SkipDeleted() {
    while (guard_.has_value()) {
      const auto *leaf = guard_->template As<LeafPage>();
      while (index_ < leaf->GetSize() && leaf->IsTombstoned(index_)) {
        ++index_;
      }
      if (index_ < leaf->GetSize()) {
        return;  // 找到一个存活条目。
      }
      const auto next = leaf->GetNextPageId();
      if (next == INVALID_PAGE_ID) {
        guard_ = std::nullopt;  // 整棵树扫完了。
        return;
      }
      // 先取到下一页的守卫再覆盖当前守卫：移动赋值会在接管新守卫时释放旧的，
      // 顺序天然正确，不会出现「两页都没锁住」的空窗。
      guard_ = bpm_->ReadPage(next);
      index_ = 0;
    }
  }

  std::shared_ptr<TracedBufferPoolManager> bpm_{nullptr};
  std::optional<ReadPageGuard> guard_{std::nullopt};
  int index_{0};
};

}  // namespace bustub
