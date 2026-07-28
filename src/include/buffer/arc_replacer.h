
//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// arc_replacer.h
//
// Identification: src/include/buffer/arc_replacer.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <list>
#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <unordered_map>

#include "common/config.h"
#include "common/macros.h"

namespace bustub {

enum class AccessType { Unknown = 0, Lookup, Scan, Index };

enum class ArcStatus { MRU, MFU, MRU_GHOST, MFU_GHOST };

// TODO(student): You can modify or remove this struct as you like.
struct FrameStatus {
  page_id_t page_id_;
  frame_id_t frame_id_;
  bool evictable_;
  ArcStatus arc_status_;

  // ==== P1 STEP 1: 用「侵入式迭代器」把链表操作降到 O(1) ====
  // 一个条目在任意时刻只会待在四条链表中的某一条里。我们把它在链表中的位置
  // （std::list 的迭代器）直接存进条目本身，这样「把某个 frame 移到 MFU 头部」
  // 就是 erase(it) + push_front() 两次 O(1) 操作，而不需要 std::find 线性扫描。
  //
  // 为什么必须这么做：arc_replacer_performance_test 用 26 万个 frame 反复
  // RecordAccess。如果每次命中都线性查找链表，单轮就是 O(n^2)，测试会跑到天荒地老。
  // std::list 的迭代器在其它元素增删时不会失效，这正是它适合做侵入式索引的原因。
  std::list<frame_id_t>::iterator alive_it_{};
  std::list<page_id_t>::iterator ghost_it_{};

  FrameStatus(page_id_t pid, frame_id_t fid, bool ev, ArcStatus st)
      : page_id_(pid), frame_id_(fid), evictable_(ev), arc_status_(st) {}

  /** @brief 该条目当前是否是「幽灵」（只留 page_id 的历史记录，不占用内存帧）。 */
  auto IsGhost() const -> bool { return arc_status_ == ArcStatus::MRU_GHOST || arc_status_ == ArcStatus::MFU_GHOST; }
};

/**
 * ArcReplacer implements the ARC replacement policy.
 */
class ArcReplacer {
 public:
  explicit ArcReplacer(size_t num_frames);

  DISALLOW_COPY_AND_MOVE(ArcReplacer);

  /**
   * @brief Destroys the LRUReplacer.
   */
  ~ArcReplacer() = default;

  auto Evict() -> std::optional<frame_id_t>;
  void RecordAccess(frame_id_t frame_id, page_id_t page_id, AccessType access_type = AccessType::Unknown);
  void SetEvictable(frame_id_t frame_id, bool set_evictable);
  void Remove(frame_id_t frame_id);
  auto Size() -> size_t;

 private:
  // TODO(student): implement me! You can replace or remove these member variables as you like.
  std::list<frame_id_t> mru_;
  std::list<frame_id_t> mfu_;
  std::list<page_id_t> mru_ghost_;
  std::list<page_id_t> mfu_ghost_;

  /* record entries in mru_ and mfu_
   * this uses frame_id_t to guarantee no duplicate records for the same
   * frame when they are alive */
  std::unordered_map<frame_id_t, std::shared_ptr<FrameStatus>> alive_map_;
  /* record entries in mru_ghost_ and mfu_ghost_
   * this uses page_id_t but not frame_id_t because page_id is the unique
   * identifier in ghost lists */
  std::unordered_map<page_id_t, std::shared_ptr<FrameStatus>> ghost_map_;

  /* alive, evictable entries count */
  size_t curr_size_{0};
  /* p as in original paper */
  size_t mru_target_size_{0};
  /* c as in original paper */
  size_t replacer_size_;
  std::mutex latch_;

  // TODO(student): You can add member variables / functions as you like.

  /**
   * @brief 从指定的「活」链表里挑一个可驱逐的受害者，并把它降级成幽灵。
   *
   * 从链表尾部（最久未用）向头部扫描，跳过被 pin 住（不可驱逐）的条目。
   *
   * @param victims      要驱逐的链表（mru_ 或 mfu_）。
   * @param ghosts       对应的幽灵链表（mru_ghost_ 或 mfu_ghost_）。
   * @param ghost_status 降级后应写入的状态（MRU_GHOST 或 MFU_GHOST）。
   * @return 被驱逐的 frame id；若该链表中所有条目都被 pin 住则返回 std::nullopt。
   *
   * @note 调用方必须已持有 latch_。
   */
  auto EvictFromList(std::list<frame_id_t> &victims, std::list<page_id_t> &ghosts, ArcStatus ghost_status)
      -> std::optional<frame_id_t>;

  /** @brief 丢弃幽灵链表中最旧的一条记录（链表尾）。调用方必须已持有 latch_。 */
  void DropOldestGhost(std::list<page_id_t> &ghosts);

  /** @brief 把某个已在 alive_map_ 中的条目从其所在链表摘出，重新挂到 mfu_ 头部。调用方持锁。 */
  void PromoteToMfu(const std::shared_ptr<FrameStatus> &status);
};

}  // namespace bustub
