//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// skiplist.h
//
// Identification: src/include/primer/skiplist.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <shared_mutex>
#include <utility>
#include <vector>
#include "common/macros.h"

namespace bustub {

/** @brief Common template arguments used for the `SkipList` class. */
#define SKIPLIST_TEMPLATE_ARGUMENTS template <typename K, typename Compare, size_t MaxHeight, uint32_t Seed>

/**
 * @brief A probabilistic data structure that implements the ordered set abstract data type.
 *
 * The skip list is implemented as a linked list of nodes. Each node has a list of forward links. The number of
 * forward links is determined by a geometric distribution. The skip list maintains a header node that is always at the
 * maximum height of the skip list. The skip list is always sorted by the key using the comparison function provided.
 *
 * @tparam K the type of key.
 * @tparam Compare the comparison function for the key.
 * @tparam MaxHeight the maximum height of the skip list.
 * @tparam Seed the seed for the random number generator.
 */
template <typename K, typename Compare = std::less<K>, size_t MaxHeight = 14, uint32_t Seed = 15445>
class SkipList {
 protected:
  struct SkipNode;

 public:
  /**  @brief Constructs an empty skip list with an optional custom comparison function. */
  explicit SkipList(const Compare &compare = Compare{})
      : compare_(compare),
        // ==== P0 STEP 1: 哨兵头节点 ====
        // header_ 是一个不存放有效 key 的「哨兵节点」(sentinel)，它的存在让插入/删除
        // 不必对「链表头」做特殊处理——任何真实节点都一定有前驱。
        //
        // 关键：头节点必须一次性分配 MaxHeight 条前向链接，而不是当前高度 height_ 条。
        // 原因有二：
        //   1. 后续插入的节点可能随机到任意 <= MaxHeight 的高度，此时直接写
        //      header_->links_[level] 即可，无需扩容 vector（扩容在并发下很危险）。
        //   2. Drop() 的实现固定遍历 `i < MaxHeight` 次 header_->links_[i]，
        //      若头节点链接数不足会直接越界。
        // 高于 height_ 的那些链接保持 nullptr，语义上表示「该层为空」。
        header_(std::make_shared<SkipNode>(MaxHeight)) {}

  /**
   * @brief Destructs the skip list.
   *
   * See `Drop()` for how we avoid blowing up the stack.
   */
  ~SkipList() { Drop(); }

  /**
   * We disable both copy & move operators/constructors as the skip list contains a mutex
   * that cannot be safely moved/copied while other threads may be using it.
   */
  SkipList(const SkipList &) = delete;
  auto operator=(const SkipList &) -> SkipList & = delete;
  SkipList(SkipList &&) = delete;
  auto operator=(SkipList &&) -> SkipList & = delete;

  auto Empty() -> bool;
  auto Size() -> size_t;

  void Clear();
  auto Insert(const K &key) -> bool;
  auto Erase(const K &key) -> bool;
  auto Contains(const K &key) -> bool;

  void Print();

 protected:
  auto Header() -> std::shared_ptr<SkipNode> { return header_; }

 private:
  auto RandomHeight() -> size_t;

  void Drop();

  // ==== P0 STEP 0: 自定义私有辅助函数 ====

  /**
   * @brief 判断两个 key 是否「等价」。
   *
   * 跟标准库容器保持一致：只依赖一个「小于」比较器 `compare_`，
   * 当 a 不小于 b 且 b 也不小于 a 时，认为二者等价。
   * 这样即使 K 没有定义 operator==（或定义得与 compare_ 不一致）也能正确工作。
   */
  auto Equals(const K &a, const K &b) -> bool { return !compare_(a, b) && !compare_(b, a); }

  /**
   * @brief 自顶向下搜索，找出每一层中「最后一个 key < 目标 key」的节点。
   *
   * 这是跳表所有操作（查找/插入/删除）共用的核心步骤。返回的 `prevs[level]`
   * 就是在第 level 层上目标 key 的前驱节点：
   *   - 查找：`prevs[0]->Next(0)` 要么是目标节点，要么就说明目标不存在。
   *   - 插入：新节点在第 level 层要挂在 `prevs[level]` 后面。
   *   - 删除：要把 `prevs[level]` 的链接改指向被删节点的后继。
   *
   * @param key   目标 key。
   * @param prevs 出参，长度必须 >= MaxHeight；只有 [0, height_) 会被写入有效值，
   *              更高层由调用方按需填 header_。
   *
   * @note 本函数不加锁，加锁由调用方负责（读操作用共享锁，写操作用独占锁）。
   */
  void FindPredecessors(const K &key, std::vector<std::shared_ptr<SkipNode>> &prevs) { FindLastLessThan(key, &prevs); }

  /**
   * @brief 跳表的核心下降搜索：返回第 0 层上「最后一个 key < 目标 key」的节点。
   *
   * @param key   目标 key。
   * @param prevs 若非 nullptr，则顺带记录每一层的前驱（写操作需要）；
   *              纯查找传 nullptr 可以省掉一次 vector 堆分配和 2*MaxHeight 次
   *              shared_ptr 原子引用计数操作——ConcurrentReadTest 有约 640 万次
   *              查找，这个开销并非可以忽略。
   * @return 第 0 层的前驱节点（表为空时即 header_ 本身）。
   *
   * @note 本函数不加锁，加锁由调用方负责。
   */
  auto FindLastLessThan(const K &key, std::vector<std::shared_ptr<SkipNode>> *prevs) -> std::shared_ptr<SkipNode> {
    auto curr = header_;
    // 从当前最高层开始向下走。高层链接跨度大，可以快速「跳过」大量节点，
    // 这正是跳表把平均查找复杂度从 O(n) 降到 O(log n) 的原因。
    for (auto level = static_cast<int64_t>(height_) - 1; level >= 0; --level) {
      auto lvl = static_cast<size_t>(level);
      // 在本层一直向右走，直到下一个节点的 key >= 目标 key（或到达链表尾）。
      while (true) {
        auto next = curr->Next(lvl);
        if (next == nullptr || !compare_(next->Key(), key)) {
          break;
        }
        curr = next;
      }
      // 此时 curr 是本层最后一个 key < 目标 key 的节点，记录为该层前驱，
      // 然后「下沉」一层继续搜索（curr 不变，因为下一层的起点就是它）。
      if (prevs != nullptr) {
        (*prevs)[lvl] = curr;
      }
    }
    return curr;
  }

  /** @brief Lowest level index for the skip list. */
  static constexpr size_t LOWEST_LEVEL = 0;

  /** @brief Comparison function used to check key orders and keep the skip list sorted. */
  Compare compare_;

  /**
   * @brief Header consists of a list of forward links for level 0 to `MaxHeight - 1`. The forward links at
   * level higher than current `height_` are `nullptr`.
   */
  std::shared_ptr<SkipNode> header_;

  /**
   * @brief Current height of the skip list.
   *
   * Invariant: `height_` should never be greater than `MaxHeight`.
   */
  uint32_t height_{1};

  /** @brief Number of elements in the skip list. */
  size_t size_{0};

  /** @brief Random number generator. */
  std::mt19937 rng_{Seed};

  /** @brief A reader-writer latch protecting the skip list. */
  std::shared_mutex rwlock_{};
};

/**
 * Node type for `SkipList`.
 */
SKIPLIST_TEMPLATE_ARGUMENTS struct SkipList<K, Compare, MaxHeight, Seed>::SkipNode {
  /**
   * @brief Constructs a skip list node with height number of links and given key.
   *
   * This constructor is used both for creating regular nodes (with key) and the header node (without key).
   *
   * @param height The number of links the node will have
   * @param key The key to store in the node (default empty for header)
   *
   * ==== P0 STEP 2: 节点的「高度」就是它的链接数量 ====
   * `links_(height)` 会构造出 height 个值初始化（即 nullptr）的 shared_ptr。
   * 节点一旦创建，links_ 的长度就固定不变——这一点很重要：
   * 后续所有修改都只是改写已有槽位，不会触发 vector 扩容/搬迁，
   * 因此其它线程持有的节点指针永远不会失效。
   */
  explicit SkipNode(size_t height, K key = K{}) : links_(height), key_(std::move(key)) {}

  auto Height() const -> size_t;
  auto Next(size_t level) const -> std::shared_ptr<SkipNode>;
  void SetNext(size_t level, const std::shared_ptr<SkipNode> &node);
  auto Key() const -> const K &;

  /**
   * @brief A list of forward links.
   *
   * Note: `links_[0]` is the lowest level link, and `links_[links_.size()-1]` is the highest level link.
   *
   * We use `std::shared_ptr` to manage the memory of the next node. This is because the next node can be shared by
   * multiple links. We also use `std::vector` instead of a flexible array member for simplicity instead of performance.
   */
  std::vector<std::shared_ptr<SkipNode>> links_;
  K key_;
};

}  // namespace bustub
