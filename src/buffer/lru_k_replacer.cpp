//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_k_replacer.cpp
//
// Identification: src/buffer/lru_k_replacer.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/lru_k_replacer.h"
#include <limits>
#include <mutex>  // NOLINT
#include <optional>
#include <utility>
#include "common/exception.h"
#include "common/macros.h"

namespace bustub {

/**
 * @brief a new LRUKReplacer.
 * @param num_frames the maximum number of frames the LRUReplacer will be required to store
 */
LRUKReplacer::LRUKReplacer(size_t num_frames, size_t k) : replacer_size_(num_frames), k_(k) {}

/**
 * @brief Find the frame with largest backward k-distance and evict that frame. Only frames
 * that are marked as 'evictable' are candidates for eviction.
 *
 * A frame with less than k historical references is given +inf as its backward k-distance.
 * If multiple frames have inf backward k-distance, then evict frame whose oldest timestamp
 * is furthest in the past.
 *
 * Successful eviction of a frame should decrement the size of replacer and remove the frame's
 * access history.
 *
 * @return the frame ID if a frame is successfully evicted, or `std::nullopt` if no frames can be evicted.
 */
auto LRUKReplacer::Evict() -> std::optional<frame_id_t> {
  std::scoped_lock lock(latch_);

  // ==== P1 STEP 5: 在「+inf 组」和「有限组」之间做两级比较 ====
  // 淘汰规则按优先级：
  //   1) 优先淘汰历史不足 K 次的帧（backward k-distance = +inf）；
  //      这类帧之间用经典 LRU 打平局，即最早那次访问时间戳最小的先走。
  //   2) 若所有可驱逐帧都攒满了 K 次历史，则淘汰倒数第 K 次访问最久远的那个，
  //      同样等价于「EarliestTimestamp() 最小」。
  // 两组各自维护一个当前最优，最后 +inf 组优先胜出。
  std::optional<frame_id_t> inf_victim;     // 历史不满 K 次的候选
  std::optional<frame_id_t> finite_victim;  // 历史满 K 次的候选
  size_t inf_oldest = std::numeric_limits<size_t>::max();
  size_t finite_oldest = std::numeric_limits<size_t>::max();

  for (const auto &[fid, node] : node_store_) {
    if (!node.IsEvictable()) {
      continue;  // 还有 page guard 持有这一帧，不能动。
    }
    const size_t ts = node.EarliestTimestamp();
    if (!node.HasFullHistory()) {
      if (ts < inf_oldest) {
        inf_oldest = ts;
        inf_victim = fid;
      }
    } else {
      if (ts < finite_oldest) {
        finite_oldest = ts;
        finite_victim = fid;
      }
    }
  }

  auto victim = inf_victim.has_value() ? inf_victim : finite_victim;
  if (!victim.has_value()) {
    return std::nullopt;  // 没有任何可驱逐的帧。
  }

  // 驱逐成功：连同访问历史一起清掉。下次这个帧再被装入新页时，
  // 历史必须从零开始，否则会把上一个页的访问记录算到新页头上。
  node_store_.erase(*victim);
  --curr_size_;
  return victim;
}

/**
 * @brief Record the event that the given frame id is accessed at current timestamp.
 * Create a new entry for access history if frame id has not been seen before.
 *
 * If frame id is invalid (ie. larger than replacer_size_), throw an exception. You can
 * also use BUSTUB_ASSERT to abort the process if frame id is invalid.
 *
 * @param frame_id id of frame that received a new access.
 * @param access_type type of access that was received. This parameter is only needed for
 * leaderboard tests.
 */
void LRUKReplacer::RecordAccess(frame_id_t frame_id, [[maybe_unused]] AccessType access_type) {
  std::scoped_lock lock(latch_);
  BUSTUB_ASSERT(static_cast<size_t>(frame_id) <= replacer_size_, "RecordAccess: frame id out of range");

  auto it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    // 第一次见到这个帧。注意新建的节点默认 is_evictable_ = false：
    // 缓冲池刚把页装进来时它是被 pin 住的，随后才会调 SetEvictable。
    it = node_store_.emplace(frame_id, LRUKNode(frame_id, k_)).first;
  }
  it->second.RecordAccess(current_timestamp_++);
}

/**
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
void LRUKReplacer::SetEvictable(frame_id_t frame_id, bool set_evictable) {
  std::scoped_lock lock(latch_);
  BUSTUB_ASSERT(static_cast<size_t>(frame_id) <= replacer_size_, "SetEvictable: frame id out of range");

  auto it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    return;  // 帧不存在 → 什么都不做（测试末尾专门验证了这一点）。
  }
  if (it->second.IsEvictable() == set_evictable) {
    return;  // 状态未变，不能重复计数。
  }
  it->second.SetEvictable(set_evictable);
  if (set_evictable) {
    ++curr_size_;
  } else {
    --curr_size_;
  }
}

/**
 * @brief Remove an evictable frame from replacer, along with its access history.
 * This function should also decrement replacer's size if removal is successful.
 *
 * Note that this is different from evicting a frame, which always remove the frame
 * with largest backward k-distance. This function removes specified frame id,
 * no matter what its backward k-distance is.
 *
 * If Remove is called on a non-evictable frame, throw an exception or abort the
 * process.
 *
 * If specified frame is not found, directly return from this function.
 *
 * @param frame_id id of frame to be removed
 */
void LRUKReplacer::Remove(frame_id_t frame_id) {
  std::scoped_lock lock(latch_);

  auto it = node_store_.find(frame_id);
  if (it == node_store_.end()) {
    return;  // 找不到直接返回。
  }
  BUSTUB_ASSERT(it->second.IsEvictable(), "LRUKReplacer::Remove called on a non-evictable frame");

  node_store_.erase(it);
  --curr_size_;
}

/**
 * @brief Return replacer's size, which tracks the number of evictable frames.
 *
 * @return size_t
 */
auto LRUKReplacer::Size() -> size_t {
  std::scoped_lock lock(latch_);
  return curr_size_;
}

}  // namespace bustub
