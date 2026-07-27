//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// execution_common.h
//
// Identification: src/include/execution/execution_common.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "binder/bound_order_by.h"
#include "catalog/catalog.h"
#include "catalog/schema.h"
#include "concurrency/transaction.h"
#include "storage/table/tuple.h"

namespace bustub {

/** The SortKey defines a list of values that sort is based on */
using SortKey = std::vector<Value>;
/** The SortEntry defines a key-value pairs for sorting tuples and corresponding RIDs */
using SortEntry = std::pair<SortKey, Tuple>;

/** The Tuple Comparator provides a comparison function for SortEntry */
class TupleComparator {
 public:
  explicit TupleComparator(std::vector<OrderBy> order_bys);

  auto operator()(const SortEntry &entry_a, const SortEntry &entry_b) const -> bool;

 private:
  std::vector<OrderBy> order_bys_;
};

auto GenerateSortKey(const Tuple &tuple, const std::vector<OrderBy> &order_bys, const Schema &schema) -> SortKey;

/**
 * Above are all you need for P3.
 * You can ignore the remaining part of this file until P4.
 */

auto ReconstructTuple(const Schema *schema, const Tuple &base_tuple, const TupleMeta &base_meta,
                      const std::vector<UndoLog> &undo_logs) -> std::optional<Tuple>;

auto CollectUndoLogs(RID rid, const TupleMeta &base_meta, const Tuple &base_tuple, std::optional<UndoLink> undo_link,
                     Transaction *txn, TransactionManager *txn_mgr) -> std::optional<std::vector<UndoLog>>;

auto GenerateNewUndoLog(const Schema *schema, const Tuple *base_tuple, const Tuple *target_tuple, timestamp_t ts,
                        UndoLink prev_version) -> UndoLog;

auto GenerateUpdatedUndoLog(const Schema *schema, const Tuple *base_tuple, const Tuple *target_tuple,
                            const UndoLog &log) -> UndoLog;

/**
 * @brief 检查写写冲突；一旦冲突就把事务标记为 TAINTED 并抛出 ExecutionException。
 *
 * 快照隔离允许「读旧版本」，但**不允许两个事务同时改同一行**。
 * 详见 execution_common.cpp 中的 P4 STEP 13。
 */
void CheckWriteConflict(const TupleMeta &meta, Transaction *txn);

/**
 * @brief 为一次修改准备好 undo log 并挂上版本链。
 *
 * 会自动区分「本事务第一次改这一行」和「本事务再次改这一行」两种情况。
 *
 * @param target_tuple 修改后的元组；传 nullptr 表示这是一次删除。
 */
void PrepareUndoLog(TransactionManager *txn_mgr, Transaction *txn, const Schema *schema, RID rid, const TupleMeta &meta,
                    const Tuple &base_tuple, const Tuple *target_tuple, std::optional<UndoLink> undo_link);

void TxnMgrDbg(const std::string &info, TransactionManager *txn_mgr, const TableInfo *table_info,
               TableHeap *table_heap);

// TODO(P4): Add new functions as needed... You are likely need to define some more functions.
//
// To give you a sense of what can be shared across executors / transaction manager, here are the
// list of helper function names that we defined in the reference solution. You should come up with
// your own when you go through the process.
// * WalkUndoLogs
// * Modify
// * IsWriteWriteConflict
// * GenerateNullTupleForSchema
// * GetUndoLogSchema
//
// We do not provide the signatures for these functions because it depends on the your implementation
// of other parts of the system. You do not need to define the same set of helper functions in
// your implementation. Please add your own ones as necessary so that you do not need to write
// the same code everywhere.

}  // namespace bustub
