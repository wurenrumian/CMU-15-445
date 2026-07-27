//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// lru_k_replacer.h
//
// Identification: src/include/buffer/lru_k_replacer.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <limits>
#include <list>
#include <mutex>  // NOLINT
#include <optional>
#include <unordered_map>
#include <vector>

#include "buffer/arc_replacer.h"
#include "common/config.h"
#include "common/macros.h"

namespace bustub {

class LRUKNode {
 public:
  LRUKNode() = default;
  LRUKNode(frame_id_t fid, size_t k) : k_(k), fid_(fid) {}

  /**
   * @brief 记录一次访问。
   *
   * ==== P1 STEP 4: 只保留最近 K 个时间戳 ====
   * 计算 backward k-distance 只需要「倒数第 K 次访问」的时间戳，更早的历史毫无用处。
   * 因此队列长度恒定裁剪到 K，内存占用是 O(K) 而不是 O(访问次数)。
   */
  void RecordAccess(size_t timestamp) {
    history_.push_back(timestamp);
    if (history_.size() > k_) {
      history_.pop_front();
    }
  }

  /** @brief 是否已经攒够 K 次访问历史。不够的话 backward k-distance 视为 +inf。 */
  auto HasFullHistory() const -> bool { return history_.size() >= k_; }

  /**
   * @brief 返回队列中最旧的时间戳。
   *
   * 当历史满 K 条时，它就是「倒数第 K 次访问」的时刻，
   * backward k-distance = 当前时刻 - 该值。因为「当前时刻」对所有候选帧都一样，
   * 所以「k-distance 最大」等价于「这个时间戳最小」，比较时无需真的做减法。
   *
   * 当历史不足 K 条时，它是「最早的一次访问」的时刻，正好用来在多个 +inf
   * 候选之间按经典 LRU 打破平局。
   */
  auto EarliestTimestamp() const -> size_t { return history_.front(); }

  auto IsEvictable() const -> bool { return is_evictable_; }
  void SetEvictable(bool evictable) { is_evictable_ = evictable; }
  auto Fid() const -> frame_id_t { return fid_; }

 private:
  /** History of last seen K timestamps of this page. Least recent timestamp stored in front. */
  std::list<size_t> history_;
  size_t k_{0};
  frame_id_t fid_{0};
  bool is_evictable_{false};
};

/**
 * LRUKReplacer implements the LRU-k replacement policy.
 *
 * The LRU-k algorithm evicts a frame whose backward k-distance is maximum
 * of all frames. Backward k-distance is computed as the difference in time between
 * current timestamp and the timestamp of kth previous access.
 *
 * A frame with less than k historical references is given
 * +inf as its backward k-distance. When multiple frames have +inf backward k-distance,
 * classical LRU algorithm is used to choose victim.
 */
class LRUKReplacer {
 public:
  explicit LRUKReplacer(size_t num_frames, size_t k);

  DISALLOW_COPY_AND_MOVE(LRUKReplacer);

  /**
   * TODO(P1): Add implementation
   *
   * @brief Destroys the LRUReplacer.
   */
  ~LRUKReplacer() = default;

  auto Evict() -> std::optional<frame_id_t>;

  void RecordAccess(frame_id_t frame_id, AccessType access_type = AccessType::Unknown);

  void SetEvictable(frame_id_t frame_id, bool set_evictable);

  void Remove(frame_id_t frame_id);

  auto Size() -> size_t;

 private:
  // TODO(student): implement me! You can replace these member variables as you like.
  std::unordered_map<frame_id_t, LRUKNode> node_store_;
  /** 逻辑时钟：每次 RecordAccess 自增，避免依赖真实时间（更快，也让测试可复现）。 */
  size_t current_timestamp_{0};
  /** 当前可驱逐的帧数，即 Size() 的返回值。 */
  size_t curr_size_{0};
  size_t replacer_size_;
  size_t k_;
  std::mutex latch_;
};

}  // namespace bustub
