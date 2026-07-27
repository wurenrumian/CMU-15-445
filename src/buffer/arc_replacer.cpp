// :bustub-keep-private:
//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// arc_replacer.cpp
//
// Identification: src/buffer/arc_replacer.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/arc_replacer.h"
#include <algorithm>
#include <list>
#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <utility>
#include "common/config.h"
#include "common/macros.h"

namespace bustub {

/*
 * ============================ ARC 算法速览 ============================
 *
 * ARC (Adaptive Replacement Cache, Megiddo & Modha 2003) 用四条链表同时
 * 追踪「最近性 (recency)」和「频繁性 (frequency)」，并且能根据负载自动调整
 * 二者的配比——这正是它相对纯 LRU / 纯 LFU 的优势。
 *
 *   [<---- mru_ghost_ ----][<---- mru_ ----] ! [---- mfu_ ---->][---- mfu_ghost_ ---->]
 *                                            ^
 *                                      "!" 表示缓存的中心
 *
 *   - mru_        (论文中的 T1)：只被访问过 1 次的页，真实占用内存帧。
 *   - mfu_        (论文中的 T2)：被访问过 >= 2 次的页，真实占用内存帧。
 *   - mru_ghost_  (论文中的 B1)：刚从 mru_ 淘汰出去的页，只保留 page_id。
 *   - mfu_ghost_  (论文中的 B2)：刚从 mfu_ 淘汰出去的页，只保留 page_id。
 *
 * 幽灵链表不占用任何内存帧，它们只是「记忆」。它们的作用是给出反馈信号：
 *   命中 mru_ghost_  → 说明缓存对「最近性」给的空间太小了 → 调大 p
 *   命中 mfu_ghost_  → 说明缓存对「频繁性」给的空间太小了 → 调小 p
 * 其中 p = mru_target_size_，表示我们希望 mru_ 占据的帧数目标值。
 *
 * 不变量：|mru_| + |mru_ghost_| <= c，四条链表元素总数 <= 2c（c = replacer_size_）。
 *
 * 本实现相对论文的两点改动（见 Evict 的文档注释）：
 *   1. |mru_| == p 时不再检查「本次访问是否来自 mfu_ghost_」，直接从 mru_ 淘汰。
 *   2. 被 pin 住的帧要跳过；若目标一侧全被 pin 住，则改从另一侧淘汰。
 *
 * 另外，本实现把论文的 REPLACE 子过程拆到了 Evict() 里：RecordAccess() 只负责
 * 调整 p 和裁剪幽灵链表，真正的「淘汰哪一帧」由缓冲池在需要空闲帧时调 Evict() 决定。
 * ======================================================================
 */

/**
 *
 * TODO(P1): Add implementation
 *
 * @brief a new ArcReplacer, with lists initialized to be empty and target size to 0
 * @param num_frames the maximum number of frames the ArcReplacer will be required to cache
 */
ArcReplacer::ArcReplacer(size_t num_frames) : replacer_size_(num_frames) {}

/**
 * TODO(P1): Add implementation
 *
 * @brief Performs the Replace operation as described by the writeup
 * that evicts from either mfu_ or mru_ into its corresponding ghost list
 * according to balancing policy.
 *
 * If you wish to refer to the original ARC paper, please note that there are
 * two changes in our implementation:
 * 1. When the size of mru_ equals the target size, we don't check
 * the last access as the paper did when deciding which list to evict from.
 * This is fine since the original decision is stated to be arbitrary.
 * 2. Entries that are not evictable are skipped. If all entries from the desired side
 * (mru_ / mfu_) are pinned, we instead try victimize the other side (mfu_ / mru_),
 * and move it to its corresponding ghost list (mfu_ghost_ / mru_ghost_).
 *
 * @return frame id of the evicted frame, or std::nullopt if cannot evict
 */
auto ArcReplacer::Evict() -> std::optional<frame_id_t> {
  std::scoped_lock lock(latch_);

  // ==== P1 STEP 2: 决定从哪一侧淘汰 ====
  // 论文的 REPLACE 判据是：|T1| >= 1 && (|T1| > p || (x ∈ B2 && |T1| == p)) → 淘汰 T1。
  // 按 writeup 的要求去掉「x ∈ B2」这个对上一次访问的检查后，|T1| == p 这个边界
  // 归到哪边就是自由的了。本实现选择归给 mru_，即判据简化为 |mru_| >= p。
  //
  // 直觉：p 是我们希望 mru_ 占的目标大小。mru_ 超标（或刚好达标）就砍 mru_，
  // 否则说明 mru_ 已经偏小，该砍 mfu_ 了。
  bool prefer_mru = !mru_.empty() && mru_.size() >= mru_target_size_;

  auto &first_list = prefer_mru ? mru_ : mfu_;
  auto &first_ghosts = prefer_mru ? mru_ghost_ : mfu_ghost_;
  auto first_status = prefer_mru ? ArcStatus::MRU_GHOST : ArcStatus::MFU_GHOST;

  if (auto victim = EvictFromList(first_list, first_ghosts, first_status); victim.has_value()) {
    return victim;
  }

  // 首选一侧要么为空，要么所有条目都被 pin 住了。退而求其次去另一侧找。
  // 注意：淘汰出去的页进入的是「它实际所属那条链表」对应的幽灵表，
  // 而不是首选一侧的幽灵表——否则会把频繁页的历史记到最近性账上，污染 p 的自适应。
  auto &second_list = prefer_mru ? mfu_ : mru_;
  auto &second_ghosts = prefer_mru ? mfu_ghost_ : mru_ghost_;
  auto second_status = prefer_mru ? ArcStatus::MFU_GHOST : ArcStatus::MRU_GHOST;

  return EvictFromList(second_list, second_ghosts, second_status);
}

auto ArcReplacer::EvictFromList(std::list<frame_id_t> &victims, std::list<page_id_t> &ghosts, ArcStatus ghost_status)
    -> std::optional<frame_id_t> {
  // 约定：链表头 = 最近使用 (MRU)，链表尾 = 最久未用 (LRU)。
  // 从尾巴往前扫，找到第一个可驱逐的条目。
  for (auto it = victims.rbegin(); it != victims.rend(); ++it) {
    auto status = alive_map_[*it];
    if (!status->evictable_) {
      continue;  // 被 pin 住（还有 page guard 在用），跳过。
    }

    frame_id_t victim = *it;

    // 1) 从「活」链表和活映射中摘除。
    //    reverse_iterator 转 forward iterator 要用 std::next(it).base()，
    //    因为 rbegin() 对应的正向迭代器其实是 end()。
    victims.erase(std::next(it).base());
    alive_map_.erase(victim);

    // 2) 降级为幽灵：帧被回收了，但我们仍然记住这个 page_id 曾经在缓存里，
    //    以便它很快又被访问时能识别出「这是一次本可避免的 miss」并调整 p。
    status->arc_status_ = ghost_status;
    status->evictable_ = false;
    ghosts.push_front(status->page_id_);
    status->ghost_it_ = ghosts.begin();
    ghost_map_[status->page_id_] = status;

    // 3) 可驱逐条目少了一个。
    --curr_size_;
    return victim;
  }
  return std::nullopt;
}

void ArcReplacer::DropOldestGhost(std::list<page_id_t> &ghosts) {
  if (ghosts.empty()) {
    return;
  }
  ghost_map_.erase(ghosts.back());
  ghosts.pop_back();
}

void ArcReplacer::PromoteToMfu(const std::shared_ptr<FrameStatus> &status) {
  // 先从当前所在的活链表里摘掉（O(1)，靠存好的迭代器）。
  if (status->arc_status_ == ArcStatus::MRU) {
    mru_.erase(status->alive_it_);
  } else {
    mfu_.erase(status->alive_it_);
  }
  // 再挂到 mfu_ 头部，并把新位置记回条目里。
  status->arc_status_ = ArcStatus::MFU;
  mfu_.push_front(status->frame_id_);
  status->alive_it_ = mfu_.begin();
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Record access to a frame, adjusting ARC bookkeeping accordingly
 * by bring the accessed page to the front of mfu_ if it exists in any of the lists
 * or the front of mru_ if it does not.
 *
 * Performs the operations EXCEPT REPLACE described in original paper, which is
 * handled by `Evict()`.
 *
 * Consider the following four cases, handle accordingly:
 * 1. Access hits mru_ or mfu_
 * 2/3. Access hits mru_ghost_ / mfu_ghost_
 * 4. Access misses all the lists
 *
 * This routine performs all changes to the four lists as preperation
 * for `Evict()` to simply find and evict a victim into ghost lists.
 *
 * Note that frame_id is used as identifier for alive pages and
 * page_id is used as identifier for the ghost pages, since page_id is
 * the unique identifier to the page after it's dead.
 * Using page_id for alive pages should be the same since it's one to one mapping,
 * but using frame_id is slightly more intuitive.
 *
 * @param frame_id id of frame that received a new access.
 * @param page_id id of page that is mapped to the frame.
 * @param access_type type of access that was received. This parameter is only needed for
 * leaderboard tests.
 */
void ArcReplacer::RecordAccess(frame_id_t frame_id, page_id_t page_id, [[maybe_unused]] AccessType access_type) {
  std::scoped_lock lock(latch_);
  BUSTUB_ASSERT(frame_id >= 0, "RecordAccess: invalid frame id");

  // ==== P1 STEP 3: 四种情况分别处理 ====

  // ---- 情况 1：命中 mru_ 或 mfu_（页就在内存里）----
  // 「被访问了第二次」就说明它有复用价值，无条件晋升到 mfu_ 头部。
  // 注意 evictable_ 保持不变：一次访问不改变 pin 状态。
  if (auto it = alive_map_.find(frame_id); it != alive_map_.end()) {
    auto status = it->second;
    status->page_id_ = page_id;
    PromoteToMfu(status);
    return;
  }

  // ---- 情况 2/3：命中幽灵链表（页刚被淘汰不久，是一次"本可避免的 miss"）----
  if (auto it = ghost_map_.find(page_id); it != ghost_map_.end()) {
    auto status = it->second;
    // 调整 p 之前先记录两条幽灵链表的长度——下面马上要从其中一条里删元素。
    const size_t b1 = mru_ghost_.size();
    const size_t b2 = mfu_ghost_.size();

    if (status->arc_status_ == ArcStatus::MRU_GHOST) {
      // 命中 B1：说明「最近访问过一次」的页被淘汰得太早了，mru_ 需要更多空间。
      // 论文公式：p = min(p + max(|B2| / |B1|, 1), c)
      // 除法的含义是「按两侧幽灵表的相对长度按比例调整」：B2 越长说明另一侧
      // 的证据越弱，这一侧就该多加一点。整数除法结果可能为 0，故与 1 取大。
      const size_t delta = std::max<size_t>(b1 == 0 ? 1 : b2 / b1, 1);
      mru_target_size_ = std::min(mru_target_size_ + delta, replacer_size_);
      mru_ghost_.erase(status->ghost_it_);
    } else {
      // 命中 B2：说明「频繁访问」的页被淘汰得太早了，mfu_ 需要更多空间 → 调小 p。
      // 论文公式：p = max(p - max(|B1| / |B2|, 1), 0)
      const size_t delta = std::max<size_t>(b2 == 0 ? 1 : b1 / b2, 1);
      mru_target_size_ = mru_target_size_ > delta ? mru_target_size_ - delta : 0;
      mfu_ghost_.erase(status->ghost_it_);
    }
    ghost_map_.erase(it);

    // 幽灵复活：它至少被访问过两次（一次进 mru_，被淘汰后又访问一次），
    // 因此直接进 mfu_，而不是 mru_。
    status->frame_id_ = frame_id;
    status->evictable_ = false;  // 新装入的帧默认被 pin 住，等 BPM 调 SetEvictable。
    status->arc_status_ = ArcStatus::MFU;
    mfu_.push_front(frame_id);
    status->alive_it_ = mfu_.begin();
    alive_map_[frame_id] = status;
    return;
  }

  // ---- 情况 4：彻底的 miss（四条链表里都没有）----
  // 插入前要先维护两条不变量：|mru_| + |mru_ghost_| <= c，四表总数 <= 2c。
  const size_t l1 = mru_.size() + mru_ghost_.size();
  const size_t total = mru_.size() + mfu_.size() + mru_ghost_.size() + mfu_ghost_.size();

  if (l1 == replacer_size_) {
    // 情况 4A：最近性这一侧（实页 + 幽灵）已经满了。
    if (mru_.size() < replacer_size_) {
      // 有幽灵可以丢，丢最旧的那个，给新页腾出「名额」。
      DropOldestGhost(mru_ghost_);
    } else {
      // 极端情况：mru_ 自己就占满了 c 个帧、一个幽灵都没有。
      // 论文规定此时直接丢弃 mru_ 尾部的实页且不留幽灵。
      // 在缓冲池的实际用法里几乎不会走到这里（RecordAccess 前必然已有空闲帧）。
      if (!mru_.empty()) {
        auto victim = mru_.back();
        auto status = alive_map_[victim];
        if (status->evictable_) {
          --curr_size_;
        }
        alive_map_.erase(victim);
        mru_.pop_back();
      }
    }
  } else if (l1 < replacer_size_ && total >= replacer_size_) {
    // 情况 4B：最近性一侧没满，但四表总量已经到达 c。
    // 只有当总量顶到上限 2c 时才需要丢一个 mfu_ 幽灵。
    if (total == 2 * replacer_size_) {
      DropOldestGhost(mfu_ghost_);
    }
  }

  // 第一次见到的页放进 mru_ 头部——它目前只有「最近性」证据，还谈不上「频繁性」。
  auto status = std::make_shared<FrameStatus>(page_id, frame_id, false, ArcStatus::MRU);
  mru_.push_front(frame_id);
  status->alive_it_ = mru_.begin();
  alive_map_[frame_id] = status;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Toggle whether a frame is evictable or non-evictable. This function also
 * controls replacer's size. Note that size is equal to number of evictable entries.
 *
 * If a frame was previously evictable and is to be set to non-evictable, then size should
 * decrement. If a frame was previously non-evictable and is to be set to evictable,
 * then size should increment.
 *
 * If frame id is invalid, throw an exception or abort the process.
 *
 * For other scenarios, this function should terminate without modifying anything.
 *
 * @param frame_id id of frame whose 'evictable' status will be modified
 * @param set_evictable whether the given frame is evictable or not
 */
void ArcReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock lock(latch_);
  BUSTUB_ASSERT(frame_id >= 0, "SetEvictable: invalid frame id");

  auto it = alive_map_.find(frame_id);
  if (it == alive_map_.end()) {
    // 帧不在 replacer 里（从没记录过，或已被淘汰/移除）→ 按文档要求静默返回。
    return;
  }

  auto status = it->second;
  if (status->evictable_ == set_evictable) {
    return;  // 状态没变化，curr_size_ 也不能动，否则会重复计数。
  }

  status->evictable_ = set_evictable;
  // curr_size_ 的定义是「可驱逐条目数」，也就是 Size() 的返回值，
  // 它只在 evictable_ 真正翻转时才 ±1。
  if (set_evictable) {
    ++curr_size_;
  } else {
    --curr_size_;
  }
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Remove an evictable frame from replacer.
 * This function should also decrement replacer's size if removal is successful.
 *
 * Note that this is different from evicting a frame, which always remove the frame
 * decided by the ARC algorithm.
 *
 * If Remove is called on a non-evictable frame, throw an exception or abort the
 * process.
 *
 * If specified frame is not found, directly return from this function.
 *
 * @param frame_id id of frame to be removed
 */
void ArcReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock lock(latch_);

  auto it = alive_map_.find(frame_id);
  if (it == alive_map_.end()) {
    return;  // 找不到就直接返回，不是错误。
  }

  auto status = it->second;
  BUSTUB_ASSERT(status->evictable_, "ArcReplacer::Remove called on a non-evictable frame");

  // 与 Evict 的关键区别：Remove 不留幽灵。
  // 它服务于 BufferPoolManager::DeletePage —— 这个页在磁盘上都被删了，
  // 保留「它曾在缓存中」的历史毫无意义，反而会污染 p 的自适应。
  if (status->arc_status_ == ArcStatus::MRU) {
    mru_.erase(status->alive_it_);
  } else {
    mfu_.erase(status->alive_it_);
  }
  alive_map_.erase(it);
  --curr_size_;
}

/**
 * TODO(P1): Add implementation
 *
 * @brief Return replacer's size, which tracks the number of evictable frames.
 *
 * @return size_t
 */
auto ArcReplacer::Size() -> size_t {
  std::scoped_lock lock(latch_);
  return curr_size_;
}

}  // namespace bustub
