//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// skiplist.cpp
//
// Identification: src/primer/skiplist.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "primer/skiplist.h"
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/macros.h"
#include "fmt/core.h"

namespace bustub {

/** @brief Checks whether the container is empty. */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::Empty() -> bool {
  // ==== P0 STEP 3: 只读操作一律用「共享锁」 ====
  // std::shared_lock 允许多个读线程同时进入，只与写线程互斥。
  // 即便这里只是读一个 size_t，也必须加锁：没有锁的话，与并发的 Insert/Erase
  // 之间构成 data race（未定义行为），ASAN/TSAN 会直接报错。
  std::shared_lock<std::shared_mutex> lock(rwlock_);
  return size_ == 0;
}

/** @brief Returns the number of elements in the skip list. */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::Size() -> size_t {
  std::shared_lock<std::shared_mutex> lock(rwlock_);
  return size_;
}

/**
 * @brief Iteratively deallocate all the nodes.
 *
 * We do this to avoid stack overflow when the skip list is large.
 *
 * If we let the compiler handle the deallocation, it will recursively call the destructor of each node,
 * which could block up the stack.
 */
SKIPLIST_TEMPLATE_ARGUMENTS void SkipList<K, Compare, MaxHeight, Seed>::Drop() {
  for (size_t i = 0; i < MaxHeight; i++) {
    auto curr = std::move(header_->links_[i]);
    while (curr != nullptr) {
      // std::move sets `curr` to the old value of `curr->links_[i]`,
      // and then resets `curr->links_[i]` to `nullptr`.
      curr = std::move(curr->links_[i]);
    }
  }
}

/**
 * @brief Removes all elements from the skip list.
 *
 * Note: You might want to use the provided `Drop` helper function.
 */
SKIPLIST_TEMPLATE_ARGUMENTS void SkipList<K, Compare, MaxHeight, Seed>::Clear() {
  // ==== P0 STEP 4: 写操作用「独占锁」 ====
  std::unique_lock<std::shared_mutex> lock(rwlock_);

  // Drop() 会逐层把 header_ 的链接 std::move 出来再迭代释放。
  // 注意它是「迭代」而非「递归」释放：如果直接令 header_->links_[0] = nullptr，
  // 会触发第 1 个节点析构 → 析构其 links_[0] → 第 2 个节点析构 …… 这条递归链
  // 在几十万节点时会把调用栈撑爆。Drop() 用循环把每个节点从链上摘下来再释放，
  // 栈深度恒定为 O(1)。
  //
  // Drop() 结束后 header_->links_[i] 全部为 nullptr（std::move 后源指针置空），
  // 所以头节点本身可以继续复用，不需要重新分配。
  Drop();

  // 恢复到「空表」的初始状态：元素数归零，有效层数退回 1。
  size_ = 0;
  height_ = 1;
}

/**
 * @brief Inserts a key into the skip list.
 *
 * Note: `Insert` will not insert the key if it already exists in the skip list.
 *
 * @param key key to insert.
 * @return true if the insertion is successful, false if the key already exists.
 */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::Insert(const K &key) -> bool {
  std::unique_lock<std::shared_mutex> lock(rwlock_);

  // ---- STEP 5.1: 找到每一层的前驱节点 ----
  // prevs 开满 MaxHeight 个槽位并预填 header_：这样即使随机高度超过当前 height_，
  // 那些「以前是空层」的槽位也已经是合法的 header_，无需再补写。
  std::vector<std::shared_ptr<SkipNode>> prevs(MaxHeight, header_);
  FindPredecessors(key, prevs);

  // ---- STEP 5.2: 去重检查 ----
  // 第 0 层是完整的有序链表，prevs[0] 的后继就是「>= key 的第一个节点」。
  // 若它与 key 等价，说明 key 已存在，插入失败。
  auto candidate = prevs[LOWEST_LEVEL]->Next(LOWEST_LEVEL);
  if (candidate != nullptr && Equals(candidate->Key(), key)) {
    return false;
  }

  // ---- STEP 5.3: 掷骰子决定新节点高度 ----
  // 注意调用时机：RandomHeight() 必须、且只能在「确认要真正插入」之后调用一次。
  // 测试用固定随机种子写死了每个节点的期望高度，多调用或少调用一次都会让
  // 后续所有节点的高度整体错位，IntegrityCheckTest 会失败。
  auto new_height = RandomHeight();

  // 如果新节点比当前跳表还高，就抬高跳表的有效层数。
  // 这些新层此前从未被使用过，前驱自然是 header_（prevs 初值已经是 header_）。
  if (new_height > height_) {
    height_ = static_cast<uint32_t>(new_height);
  }

  // ---- STEP 5.4: 逐层把新节点接进链表 ----
  auto node = std::make_shared<SkipNode>(new_height, key);
  for (size_t level = 0; level < new_height; ++level) {
    // 经典的单链表插入两步走，顺序不能反：
    //   先让新节点指向后继，再让前驱指向新节点。
    // 这样在任意时刻从 header_ 出发遍历，看到的链表都是完整合法的
    // （这也是无锁跳表能工作的基础，虽然本实现用的是全局读写锁）。
    node->SetNext(level, prevs[level]->Next(level));
    prevs[level]->SetNext(level, node);
  }

  ++size_;
  return true;
}

/**
 * @brief Erases the key from the skip list.
 *
 * @param key key to erase.
 * @return bool true if the element got erased, false otherwise.
 */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::Erase(const K &key) -> bool {
  std::unique_lock<std::shared_mutex> lock(rwlock_);

  // ---- STEP 6.1: 同样先定位每层前驱 ----
  std::vector<std::shared_ptr<SkipNode>> prevs(MaxHeight, header_);
  FindPredecessors(key, prevs);

  // ---- STEP 6.2: 确认目标存在 ----
  auto target = prevs[LOWEST_LEVEL]->Next(LOWEST_LEVEL);
  if (target == nullptr || !Equals(target->Key(), key)) {
    return false;
  }

  // ---- STEP 6.3: 把目标节点从它出现的每一层上摘下来 ----
  // 只需要遍历到 target->Height()，因为更高层根本没有它的身影。
  for (size_t level = 0; level < target->Height(); ++level) {
    // 这个 if 是防御性的：正常情况下 prevs[level] 的后继必然就是 target。
    // 但如果 target 的高度超过了 height_（不该发生），这里能避免误改无关链接。
    if (prevs[level]->Next(level) == target) {
      prevs[level]->SetNext(level, target->Next(level));
    }
  }
  // 说明：这里不需要担心 target 析构时递归释放后继节点——target->links_ 里的
  // 后继仍被前驱节点（或 header_）持有，引用计数不为 0，不会发生连锁析构。

  --size_;

  // ---- STEP 6.4: 收缩空的高层 ----
  // 删掉最高的节点后，顶层可能变空。把 height_ 降回去可以让后续查找少走几层，
  // 同时维持「header_->links_[height_ - 1] 非空（表非空时）」这一不变量。
  while (height_ > 1 && header_->Next(height_ - 1) == nullptr) {
    --height_;
  }

  return true;
}

/**
 * @brief Checks whether a key exists in the skip list.
 *
 * @param key key to look up.
 * @return bool true if the element exists, false otherwise.
 */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::Contains(const K &key) -> bool {
  // Following the standard library: Key `a` and `b` are considered equivalent if neither compares less
  // than the other: `!compare_(a, b) && !compare_(b, a)`.

  // ==== P0 STEP 7: 查找必须用共享锁 ====
  // 这里若误用 std::unique_lock，功能仍然正确，但 ConcurrentReadTest 里
  // 8 个线程 × 80 万次查找会被串行化，测试直接超时。
  // 「读读并发、读写互斥」正是 shared_mutex 的意义所在。
  std::shared_lock<std::shared_mutex> lock(rwlock_);

  // 纯查找不需要记录每层前驱，传 nullptr 走快速路径。
  auto prev = FindLastLessThan(key, nullptr);
  auto candidate = prev->Next(LOWEST_LEVEL);
  return candidate != nullptr && Equals(candidate->Key(), key);
}

/**
 * @brief Prints the skip list for debugging purposes.
 *
 * Note: You may modify the functions in any way and the output is not tested.
 */
SKIPLIST_TEMPLATE_ARGUMENTS void SkipList<K, Compare, MaxHeight, Seed>::Print() {
  auto node = header_->Next(LOWEST_LEVEL);
  while (node != nullptr) {
    fmt::println("Node {{ key: {}, height: {} }}", node->Key(), node->Height());
    node = node->Next(LOWEST_LEVEL);
  }
}

/**
 * @brief Generate a random height. The height should be capped at `MaxHeight`.
 * Note: we implement/simulate the geometric process to ensure platform independence.
 */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::RandomHeight() -> size_t {
  // Branching factor (1 in 4 chance), see Pugh's paper.
  static constexpr unsigned int branching_factor = 4;
  // Start with the minimum height
  size_t height = 1;
  while (height < MaxHeight && (rng_() % branching_factor == 0)) {
    height++;
  }
  return height;
}

/**
 * @brief Gets the current node height.
 */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::SkipNode::Height() const -> size_t {
  // ==== P0 STEP 8: 高度 = 链接数量 ====
  // 不需要额外存一个 height_ 字段，links_.size() 就是权威来源，
  // 也就不存在两者不一致的可能。
  return links_.size();
}

/**
 * @brief Gets the next node by following the link at `level`.
 *
 * @param level index to the link.
 * @return std::shared_ptr<SkipNode> the next node, or `nullptr` if such node does not exist.
 */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::SkipNode::Next(size_t level) const
    -> std::shared_ptr<SkipNode> {
  // 越界访问返回 nullptr 而不是 UB：调用方（如测试里的 CheckIntegrity、
  // 或 Erase 的收缩逻辑）可能会探测超出本节点高度的层，统一按「无后继」处理。
  if (level >= links_.size()) {
    return nullptr;
  }
  return links_[level];
}

/**
 * @brief Set the `node` to be linked at `level`.
 *
 * @param level index to the link.
 */
SKIPLIST_TEMPLATE_ARGUMENTS void SkipList<K, Compare, MaxHeight, Seed>::SkipNode::SetNext(
    size_t level, const std::shared_ptr<SkipNode> &node) {
  // 这里用 assert 而非静默返回：写越界一定是调用方的逻辑错误
  // （例如把节点挂到了超过自身高度的层上），应当尽早暴露。
  BUSTUB_ASSERT(level < links_.size(), "SetNext: level out of range");
  links_[level] = node;
}

/** @brief Returns a reference to the key stored in the node. */
SKIPLIST_TEMPLATE_ARGUMENTS auto SkipList<K, Compare, MaxHeight, Seed>::SkipNode::Key() const -> const K & {
  // 返回 const 引用而非拷贝：K 可能是 std::string 这类重量级类型，
  // 而查找路径上每层都要取一次 key 做比较。
  return key_;
}

// Below are explicit instantiation of template classes.
template class SkipList<int>;
template class SkipList<std::string>;
template class SkipList<int, std::greater<>>;
template class SkipList<int, std::less<>, 8>;

}  // namespace bustub
