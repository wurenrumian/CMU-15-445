//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// watermark.h
//
// Identification: src/include/concurrency/watermark.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <set>
#include <unordered_map>

#include "concurrency/transaction.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * @brief tracks all the read timestamps.
 *
 */
class Watermark {
 public:
  explicit Watermark(timestamp_t commit_ts) : commit_ts_(commit_ts), watermark_(commit_ts) {}

  auto AddTxn(timestamp_t read_ts) -> void;

  auto RemoveTxn(timestamp_t read_ts) -> void;

  /** The caller should update commit ts before removing the txn from the watermark so that we can track watermark
   * correctly. */
  auto UpdateCommitTs(timestamp_t commit_ts) { commit_ts_ = commit_ts; }

  auto GetWatermark() -> timestamp_t {
    if (current_reads_.empty()) {
      return commit_ts_;
    }
    return watermark_;
  }

  timestamp_t commit_ts_;

  timestamp_t watermark_;

  std::unordered_map<timestamp_t, int> current_reads_;

  /**
   * @brief 所有仍在活跃的 read_ts，有序。
   *
   * 水位线的定义是「所有活跃事务中最小的 read_ts」，也就是这个集合的最小值。
   * current_reads_ 是无序哈希表，求最小值要 O(n) 遍历；而 AddTxn/RemoveTxn
   * 在事务开始/结束时都会被调用，是热路径。
   * 额外维护一个有序集合，取最小值就变成 O(1)（*begin()），增删是 O(log n)。
   *
   * 两个容器共同维护同一份信息：current_reads_ 记「这个时间戳有几个事务在用」，
   * active_ts_ 只记「有哪些时间戳还在被用」，计数归零时才从后者移除。
   */
  std::set<timestamp_t> active_ts_;
};

};  // namespace bustub
