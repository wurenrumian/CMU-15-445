//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// watermark.cpp
//
// Identification: src/concurrency/watermark.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "concurrency/watermark.h"
#include <exception>
#include "common/exception.h"

namespace bustub {

/*
 * ==== P4 STEP 1: 水位线是什么，为什么需要它 ====
 *
 * MVCC 下每次更新都会留下旧版本（undo log），版本链会一直增长。
 * 什么时候才能安全地回收一个旧版本？答案是：**当没有任何活跃事务可能再读到它时。**
 *
 * 快照隔离下，事务只会读到 ts <= 自己 read_ts 的版本。所以只要一个版本
 * 比「所有活跃事务中最小的 read_ts」还要旧，且它上面已经有更新的版本可用，
 * 它就永远不会再被访问 —— 可以回收。
 *
 * 这个「最小的活跃 read_ts」就是水位线（watermark）。
 *
 *      已提交的历史版本 ──────────────►  时间
 *              ▲                ▲
 *         watermark         last_commit_ts
 *              │                │
 *      <── 可回收 ──┼── 必须保留 ──►
 *
 * 没有活跃事务时，水位线等于最新提交时间戳（GetWatermark() 里的特判），
 * 此时除最新版本外的一切都可以回收。
 */

auto Watermark::AddTxn(timestamp_t read_ts) -> void {
  if (read_ts < commit_ts_) {
    throw Exception("read ts < commit ts");
  }

  // 多个事务可能持有同一个 read_ts（它们在同一时刻开始），所以用计数而非集合。
  ++current_reads_[read_ts];
  active_ts_.insert(read_ts);
  watermark_ = *active_ts_.begin();
}

auto Watermark::RemoveTxn(timestamp_t read_ts) -> void {
  auto it = current_reads_.find(read_ts);
  if (it == current_reads_.end()) {
    return;  // 防御：重复移除时静默返回。
  }

  // 只有当这个时间戳上最后一个事务也退出时，它才真正不再活跃。
  if (--(it->second) <= 0) {
    current_reads_.erase(it);
    active_ts_.erase(read_ts);
  }

  // 水位线随最小活跃时间戳前移。全部事务都结束时，
  // GetWatermark() 会走 current_reads_.empty() 的分支直接返回 commit_ts_，
  // 所以这里不需要为空集合特意赋值。
  if (!active_ts_.empty()) {
    watermark_ = *active_ts_.begin();
  }
}

}  // namespace bustub
