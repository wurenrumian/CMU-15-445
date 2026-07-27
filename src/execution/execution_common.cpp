//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// execution_common.cpp
//
// Identification: src/execution/execution_common.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/execution_common.h"
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "catalog/catalog.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "common/macros.h"
#include "concurrency/transaction_manager.h"
#include "fmt/core.h"
#include "storage/table/table_heap.h"

namespace bustub {

TupleComparator::TupleComparator(std::vector<OrderBy> order_bys) : order_bys_(std::move(order_bys)) {}

/**
 * @brief 严格弱序比较：a 是否应当排在 b 之前。
 *
 * ==== P3 STEP 17: 为什么要先算出 SortKey ====
 * 排序过程中同一条元组会被比较 O(log n) 次。如果每次比较都现场求一遍
 * ORDER BY 表达式（可能是 `a + b * 2` 这种），代价会乘以 log n。
 * 所以先一次性把每条元组的排序键算出来（GenerateSortKey），
 * 之后的比较只在 Value 之间进行 —— 这就是数据库里常说的
 * 「decorate-sort-undecorate」（Schwartzian transform）。
 *
 * @return true 表示 a 严格排在 b 前面；相等或 a 在后都返回 false。
 *         std::sort 要求的是严格弱序，返回 true 表示"必须交换顺序"。
 */
auto TupleComparator::operator()(const SortEntry &entry_a, const SortEntry &entry_b) const -> bool {
  const auto &key_a = entry_a.first;
  const auto &key_b = entry_b.first;

  for (size_t i = 0; i < order_bys_.size(); i++) {
    // OrderBy 是三元组 (排序方向, NULL 排位, 表达式)。
    const auto order_type = std::get<0>(order_bys_[i]);
    const auto null_type = std::get<1>(order_bys_[i]);
    const bool desc = order_type == OrderByType::DESC;

    // ==== NULL 必须显式处理 ====
    // Value::CompareLessThan(NULL, x) 返回的是 CmpNull（三值逻辑里的 UNKNOWN），
    // 既不是真也不是假。如果只按 CmpTrue 判断，含 NULL 的两条元组会被当作"相等"，
    // 于是它们的相对位置完全由输入顺序决定 —— 结果不确定，测试必然失败。
    //
    // 排序不是逻辑判断，它必须给 NULL 一个确定的位置。BusTub 的默认约定是
    // **NULL 视为最小值**（升序时排在最前）。SQL 标准允许用
    // `NULLS FIRST` / `NULLS LAST` 显式覆盖，对应 null_type。
    const bool a_null = key_a[i].IsNull();
    const bool b_null = key_b[i].IsNull();
    if (a_null || b_null) {
      if (a_null && b_null) {
        continue;  // 都是 NULL，本列分不出先后，看下一列。
      }
      bool null_first;
      switch (null_type) {
        case OrderByNullType::NULLS_FIRST:
          null_first = true;
          break;
        case OrderByNullType::NULLS_LAST:
          null_first = false;
          break;
        default:
          // 默认：把 NULL 当最小值。因此降序时它自然跑到末尾。
          null_first = !desc;
          break;
      }
      return a_null == null_first;
    }

    // 多列排序：前面的列分出胜负就直接返回，相等才看下一列。
    if (key_a[i].CompareLessThan(key_b[i]) == CmpBool::CmpTrue) {
      return !desc;
    }
    if (key_a[i].CompareGreaterThan(key_b[i]) == CmpBool::CmpTrue) {
      return desc;
    }
    // 相等 → 看下一个排序列。
  }
  return false;  // 所有排序列都相等，视为等价，不交换。
}

/**
 * Generate sort key for a tuple based on the order by expressions.
 */
auto GenerateSortKey(const Tuple &tuple, const std::vector<OrderBy> &order_bys, const Schema &schema) -> SortKey {
  SortKey key;
  key.reserve(order_bys.size());
  for (const auto &order_by : order_bys) {
    key.emplace_back(std::get<2>(order_by)->Evaluate(&tuple, schema));
  }
  return key;
}

/**
 * Above are all you need for P3.
 * You can ignore the remaining part of this file until P4.
 */

/**
 * @brief Reconstruct a tuple by applying the provided undo logs from the base tuple. All logs in the undo_logs are
 * applied regardless of the timestamp
 *
 * @param schema The schema of the base tuple and the returned tuple.
 * @param base_tuple The base tuple to start the reconstruction from.
 * @param base_meta The metadata of the base tuple.
 * @param undo_logs The list of undo logs to apply during the reconstruction, the front is applied first.
 * @return An optional tuple that represents the reconstructed tuple. If the tuple is deleted as the result, returns
 * std::nullopt.
 */
auto ReconstructTuple(const Schema *schema, const Tuple &base_tuple, const TupleMeta &base_meta,
                      const std::vector<UndoLog> &undo_logs) -> std::optional<Tuple> {
  /*
   * ==== P4 STEP 6: 版本链是「最新值 + 一串反向增量」====
   *
   * 表堆里存的永远是**最新**版本；历史版本不单独完整保存，而是以
   * undo log 的形式记录「要变回上一版，需要把哪几列改成什么」。
   *
   *   表堆 (ts=5)  ──prev──►  undo log (ts=3) ──prev──►  undo log (ts=1)
   *   (a=9,b=9,c=9)          改回 a=3           改回 b=1
   *
   * 想读 ts=1 的快照，就从最新值出发，依次套用 ts=3、ts=1 两条日志。
   * 这样做的好处是**未修改的列不重复存储** —— UPDATE 一列的表宽 100 列时，
   * 旧版本只占 1 列的空间。代价是读越老的版本要套用越多日志。
   *
   * modified_fields_ 是一个按列的位图，标记这条日志覆盖了哪些列。
   */
  std::vector<Value> values;
  values.reserve(schema->GetColumnCount());
  for (uint32_t i = 0; i < schema->GetColumnCount(); i++) {
    values.push_back(base_tuple.GetValue(schema, i));
  }
  bool deleted = base_meta.is_deleted_;

  for (const auto &log : undo_logs) {
    // 一条日志可能把元组还原成「当时还不存在」的状态（对应一次 INSERT 的撤销）。
    deleted = log.is_deleted_;
    if (deleted) {
      // 是删除标记时，tuple_ 无意义，不必也不能去解析它。
      continue;
    }

    // 日志里的 tuple_ 只包含被修改的那几列，按 modified_fields_ 的顺序紧凑排列。
    // 因此要先构造出「部分模式」，才能正确地把值一个个取出来。
    std::vector<Column> partial_columns;
    for (uint32_t i = 0; i < log.modified_fields_.size(); i++) {
      if (log.modified_fields_[i]) {
        partial_columns.push_back(schema->GetColumn(i));
      }
    }
    Schema partial_schema{partial_columns};

    uint32_t partial_idx = 0;
    for (uint32_t i = 0; i < log.modified_fields_.size(); i++) {
      if (log.modified_fields_[i]) {
        values[i] = log.tuple_.GetValue(&partial_schema, partial_idx++);
      }
    }
  }

  if (deleted) {
    // 在目标时刻这条记录并不存在，对读者而言就是「查无此行」。
    return std::nullopt;
  }
  return Tuple{values, schema};
}

/**
 * @brief Collects the undo logs sufficient to reconstruct the tuple w.r.t. the txn.
 *
 * @param rid The RID of the tuple.
 * @param base_meta The metadata of the base tuple.
 * @param base_tuple The base tuple.
 * @param undo_link The undo link to the latest undo log.
 * @param txn The transaction.
 * @param txn_mgr The transaction manager.
 * @return An optional vector of undo logs to pass to ReconstructTuple(). std::nullopt if the tuple did not exist at the
 * time.
 */
auto CollectUndoLogs(RID rid, const TupleMeta &base_meta, const Tuple &base_tuple, std::optional<UndoLink> undo_link,
                     Transaction *txn, TransactionManager *txn_mgr) -> std::optional<std::vector<UndoLog>> {
  /*
   * ==== P4 STEP 7: 可见性判断 —— 快照隔离的全部规则 ====
   *
   * 给定一个读事务（read_ts）和一条记录，要决定它该看到哪个版本。
   * 表堆里那份的时间戳 base_meta.ts_ 有三种情况：
   *
   *   ① ts_ == txn->GetTransactionTempTs()
   *      这是**本事务自己**尚未提交的修改 → 应当看到（read-your-own-writes）。
   *
   *   ② ts_ <= read_ts
   *      这是在本事务开始前就已提交的版本 → 直接可见，不用回溯。
   *
   *   ③ 其它（ts_ > read_ts，或是别的事务的临时时间戳）
   *      表堆里这份对本事务「太新」了 → 必须顺着版本链往回走，
   *      直到找到第一个 ts <= read_ts 的 undo log。
   *
   * 情况 ③ 里若走到链尾仍没找到，说明这条记录在本事务的快照时刻**还不存在**，
   * 返回 nullopt 表示不可见。
   */
  const auto read_ts = txn->GetReadTs();

  // 情况 ①②：表堆里那份就是我们要的版本，不需要任何 undo log。
  if (base_meta.ts_ == txn->GetTransactionTempTs() || base_meta.ts_ <= read_ts) {
    return std::vector<UndoLog>{};
  }

  // 情况 ③：沿版本链回溯。
  std::vector<UndoLog> logs;
  auto link = undo_link;
  while (link.has_value() && link->IsValid()) {
    auto log = txn_mgr->GetUndoLogOptional(*link);
    if (!log.has_value()) {
      // 日志已被 GC 回收 —— 说明它早已在水位线之下，
      // 正常情况下当前事务不应该还需要它。
      return std::nullopt;
    }
    logs.push_back(*log);
    if (log->ts_ <= read_ts) {
      // 找到了第一个在快照时刻之前的版本，回溯到此为止。
      // 注意这条日志本身也要包含进去（它记录的正是"变回那个版本"的增量）。
      return logs;
    }
    link = log->prev_version_;
  }

  // 走到链尾都没找到足够旧的版本 → 该记录在本事务的快照时刻尚不存在。
  return std::nullopt;
}

/**
 * @brief Generates a new undo log as the transaction tries to modify this tuple at the first time.
 *
 * @param schema The schema of the table.
 * @param base_tuple The base tuple before the update, the one retrieved from the table heap. nullptr if the tuple is
 * deleted.
 * @param target_tuple The target tuple after the update. nullptr if this is a deletion.
 * @param ts The timestamp of the base tuple.
 * @param prev_version The undo link to the latest undo log of this tuple.
 * @return The generated undo log.
 */
auto GenerateNewUndoLog(const Schema *schema, const Tuple *base_tuple, const Tuple *target_tuple, timestamp_t ts,
                        UndoLink prev_version) -> UndoLog {
  /*
   * ==== P4 STEP 9: 本事务第一次修改这条记录 —— 生成一条新的 undo log ====
   *
   * undo log 记录的是「怎么变回**修改前**的样子」，所以它保存的是 base_tuple
   * （旧值）里被改动的那几列。
   *
   * 三种情形：
   *   插入（base_tuple == nullptr）：修改前这行不存在 → is_deleted_ = true，
   *       不需要任何列数据。回滚时看到这条日志就知道"当时没有这一行"。
   *   删除（target_tuple == nullptr）：修改前这行存在且完整 → 必须保存**全部**列，
   *       否则将来恢复不出完整的旧值。
   *   更新：只保存真正变化了的列 —— 这正是版本链省空间的关键。
   */
  UndoLog log;
  log.ts_ = ts;
  log.prev_version_ = prev_version;

  if (base_tuple == nullptr) {
    // 插入的撤销：修改前不存在。
    log.is_deleted_ = true;
    log.modified_fields_.assign(schema->GetColumnCount(), false);
    return log;
  }

  log.is_deleted_ = false;
  const auto column_count = schema->GetColumnCount();
  log.modified_fields_.assign(column_count, false);

  std::vector<Column> partial_columns;
  std::vector<Value> partial_values;

  for (uint32_t i = 0; i < column_count; i++) {
    bool modified;
    if (target_tuple == nullptr) {
      // 删除的撤销：整行都要留底，否则恢复不出完整旧值。
      modified = true;
    } else {
      // 更新的撤销：只有真正变了的列才需要留底。
      // 注意 NULL 的比较：CompareExactlyEquals 把 NULL == NULL 视为真，
      // 而普通的 CompareEquals 会返回 CmpNull，导致「NULL 改成 NULL」被误判为变化。
      modified = !base_tuple->GetValue(schema, i).CompareExactlyEquals(target_tuple->GetValue(schema, i));
    }
    if (modified) {
      log.modified_fields_[i] = true;
      partial_columns.push_back(schema->GetColumn(i));
      partial_values.push_back(base_tuple->GetValue(schema, i));
    }
  }

  // 日志里的 tuple_ 只按位图紧凑存放被修改的那几列。
  Schema partial_schema{partial_columns};
  log.tuple_ = Tuple{partial_values, &partial_schema};
  return log;
}

/**
 * @brief Generate the updated undo log to replace the old one, whereas the tuple is already modified by this txn once.
 *
 * @param schema The schema of the table.
 * @param base_tuple The base tuple before the update, the one retrieved from the table heap. nullptr if the tuple is
 * deleted.
 * @param target_tuple The target tuple after the update. nullptr if this is a deletion.
 * @param log The original undo log.
 * @return The updated undo log.
 */
auto GenerateUpdatedUndoLog(const Schema *schema, const Tuple *base_tuple, const Tuple *target_tuple,
                            const UndoLog &log) -> UndoLog {
  /*
   * ==== P4 STEP 10: 本事务**再次**修改同一条记录 —— 就地扩充已有的 undo log ====
   *
   * 关键认识：一个事务对同一行改两次，版本链上**只应该有一条** undo log。
   * 因为对外界而言，本事务的所有修改是原子生效的，中间态从来不存在，
   * 不需要为它保留可回溯的版本。
   *
   * 那这条日志该记什么？记的永远是「变回**本事务开始修改之前**的样子」。
   * 所以：
   *   - 已经记过的列：保持原值不动（那才是真正的旧值，不能被第二次修改覆盖）；
   *   - 这次新变化的列：追加进来。
   * 也就是把两次修改的 modified_fields_ 做**并集**。
   *
   * 若这次是删除（target_tuple == nullptr），则整行都要留底 —— 全列置位。
   */
  UndoLog updated;
  updated.ts_ = log.ts_;
  updated.prev_version_ = log.prev_version_;
  updated.is_deleted_ = log.is_deleted_;

  if (log.is_deleted_) {
    // 原日志说"修改前这行不存在"（本事务自己插入的）。
    // 再怎么改，撤销的结果仍然是"不存在"，日志内容无需变化。
    updated.modified_fields_ = log.modified_fields_;
    return updated;
  }

  const auto column_count = schema->GetColumnCount();

  // 先把原日志里已记录的那些列的值还原出来，方便下面按列取用。
  std::vector<Column> old_partial_columns;
  for (uint32_t i = 0; i < column_count; i++) {
    if (log.modified_fields_[i]) {
      old_partial_columns.push_back(schema->GetColumn(i));
    }
  }
  Schema old_partial_schema{old_partial_columns};

  updated.modified_fields_.assign(column_count, false);
  std::vector<Column> partial_columns;
  std::vector<Value> partial_values;

  uint32_t old_idx = 0;
  for (uint32_t i = 0; i < column_count; i++) {
    const bool already_logged = log.modified_fields_[i];
    bool newly_modified = false;
    if (!already_logged) {
      newly_modified = target_tuple == nullptr ||
                       !base_tuple->GetValue(schema, i).CompareExactlyEquals(target_tuple->GetValue(schema, i));
    }

    if (already_logged || newly_modified) {
      updated.modified_fields_[i] = true;
      partial_columns.push_back(schema->GetColumn(i));
      // 已记录过的列取**原日志**里的值（最初的旧值），
      // 新增的列取 base_tuple（此刻表堆里的值，对本事务而言仍是"改之前"）。
      partial_values.push_back(already_logged ? log.tuple_.GetValue(&old_partial_schema, old_idx)
                                              : base_tuple->GetValue(schema, i));
    }
    if (already_logged) {
      ++old_idx;
    }
  }

  Schema partial_schema{partial_columns};
  updated.tuple_ = Tuple{partial_values, &partial_schema};
  return updated;
}

void CheckWriteConflict(const TupleMeta &meta, Transaction *txn) {
  /*
   * ==== P4 STEP 13: 写写冲突检测 ====
   *
   * 快照隔离允许「读旧版本」，但**绝不允许两个事务同时改同一行** ——
   * 否则后提交者会静默覆盖先提交者的修改，即经典的「丢失更新」(lost update)。
   *
   * 表堆里这一行的时间戳告诉我们它当前归谁：
   *
   *   ① ts >= TXN_START_ID 且不是我自己 → 另一个**未提交**事务正持有它。
   *      不能等（那就变成了阻塞），直接判本事务失败。
   *      这就是 first-updater-wins：先动手的赢。
   *
   *   ② ts < TXN_START_ID 但 ts > 我的 read_ts → 有人在我开始**之后**提交了对它的修改。
   *      我看到的是旧快照，若在此基础上写就会覆盖掉那次提交。
   *      这就是 first-committer-wins：先提交的赢。
   *
   * 冲突时把事务置为 TAINTED（受污染）而不是直接 ABORTED：
   * TAINTED 表示"逻辑上已失败，但资源还没清理"，由上层决定何时调用 Abort。
   */
  const bool held_by_other_txn = meta.ts_ >= TXN_START_ID && meta.ts_ != txn->GetTransactionTempTs();
  const bool committed_after_me = meta.ts_ < TXN_START_ID && meta.ts_ > txn->GetReadTs();

  if (held_by_other_txn || committed_after_me) {
    txn->SetTainted();
    throw ExecutionException("write-write conflict detected");
  }
}

void PrepareUndoLog(TransactionManager *txn_mgr, Transaction *txn, const Schema *schema, RID rid, const TupleMeta &meta,
                    const Tuple &base_tuple, const Tuple *target_tuple, std::optional<UndoLink> undo_link) {
  /*
   * ==== P4 STEP 14: 一个事务对同一行只留一条 undo log ====
   *
   * 对外界而言，本事务的所有修改是**原子生效**的，中间态从来不存在，
   * 因此没有必要为它保留可回溯的版本。所以：
   *
   *   第一次改这一行  → 新建一条 undo log，挂到版本链头部；
   *   再次改这一行    → 就地扩充已有的那条（把新变化的列并进去）；
   *   本事务自己插入的行 → 根本不需要 undo log
   *                        （撤销的结果是"不存在"，而版本链为空正好表达这一点）。
   */
  const bool modified_by_me = meta.ts_ == txn->GetTransactionTempTs();

  // 当前这一行如果本身就是**删除标记**，那么"修改前的样子"就是"不存在"。
  // 传 nullptr 让 GenerateNewUndoLog 生成 is_deleted_ = true 的日志。
  //
  // 漏掉这一步的后果非常隐蔽：INSERT 复活一条已删除的记录时，
  // 会生成一条 is_deleted_ = false 的普通更新日志，于是老事务顺着版本链
  // 回溯时会把这一行"还原"成删除前的旧值 —— 一条本该不可见的记录复活了。
  const Tuple *base = meta.is_deleted_ ? nullptr : &base_tuple;

  if (!modified_by_me) {
    // 第一次修改：新建 undo log，记录"变回当前值"所需的增量。
    auto log = GenerateNewUndoLog(schema, base, target_tuple, meta.ts_, undo_link.value_or(UndoLink{}));
    auto new_link = txn->AppendUndoLog(std::move(log));
    txn_mgr->UpdateUndoLink(rid, new_link);
    return;
  }

  // 本事务已经改过这一行。
  if (undo_link.has_value() && undo_link->IsValid() && undo_link->prev_txn_ == txn->GetTransactionTempTs()) {
    // 链头那条日志是本事务自己建的 → 就地扩充它。
    auto old_log = txn_mgr->GetUndoLog(*undo_link);
    txn->ModifyUndoLog(undo_link->prev_log_idx_, GenerateUpdatedUndoLog(schema, base, target_tuple, old_log));
  }
  // 否则说明这一行本来就是本事务插入的，没有历史版本可言，什么都不用做。
}

void TxnMgrDbg(const std::string &info, TransactionManager *txn_mgr, const TableInfo *table_info,
               TableHeap *table_heap) {
  // always use stderr for printing logs...
  fmt::println(stderr, "debug_hook: {}", info);

  // ==== P4 STEP 11: 把整张表的版本链打印出来 ====
  // MVCC 的 bug 几乎都表现为「读到了不该读的版本」，而版本链是运行时才成形的，
  // 光看代码根本推不出来。这个函数是整个 P4 最划算的投资：
  // 花二十行把版本链可视化，后面每个 bug 的定位时间都能少一个数量级。
  //
  // 输出格式：
  //   RID=0/0 ts=txn8 tuple=(1, <NULL>, <NULL>)
  //     txn8@0 (2, _, _) ts=1
  // 其中 `_` 表示这条 undo log 没有记录该列（未被修改），
  // `ts=txnN` 表示该版本仍属于未提交的事务 N。
  const auto &schema = table_info->schema_;

  auto ts_to_string = [](timestamp_t ts) -> std::string {
    if (ts >= TXN_START_ID) {
      // 临时时间戳：显示成人类可读的事务编号，而不是那个天文数字。
      return fmt::format("txn{}", ts - TXN_START_ID);
    }
    return fmt::format("{}", ts);
  };

  for (auto iter = table_heap->MakeIterator(); !iter.IsEnd(); ++iter) {
    const auto rid = iter.GetRID();
    auto [meta, tuple] = iter.GetTuple();

    fmt::println(stderr, "RID={}/{} ts={}{} tuple={}", rid.GetPageId(), rid.GetSlotNum(), ts_to_string(meta.ts_),
                 meta.is_deleted_ ? " <del marker>" : "", tuple.ToString(&schema));

    // 顺着版本链往回打印全部历史版本。
    auto link = txn_mgr->GetUndoLink(rid);
    while (link.has_value() && link->IsValid()) {
      auto log = txn_mgr->GetUndoLogOptional(*link);
      if (!log.has_value()) {
        fmt::println(stderr, "  (undo log already garbage-collected)");
        break;
      }
      if (log->is_deleted_) {
        fmt::println(stderr, "  txn{}@{} <del> ts={}", link->prev_txn_ - TXN_START_ID, link->prev_log_idx_,
                     ts_to_string(log->ts_));
      } else {
        // 只打印被该日志记录的列，其余显示为 `_`。
        std::vector<Column> partial_columns;
        for (uint32_t i = 0; i < log->modified_fields_.size(); i++) {
          if (log->modified_fields_[i]) {
            partial_columns.push_back(schema.GetColumn(i));
          }
        }
        Schema partial_schema{partial_columns};

        std::string fields;
        uint32_t partial_idx = 0;
        for (uint32_t i = 0; i < log->modified_fields_.size(); i++) {
          if (i > 0) {
            fields += ", ";
          }
          if (log->modified_fields_[i]) {
            fields += log->tuple_.GetValue(&partial_schema, partial_idx++).ToString();
          } else {
            fields += "_";
          }
        }
        fmt::println(stderr, "  txn{}@{} ({}) ts={}", link->prev_txn_ - TXN_START_ID, link->prev_log_idx_, fields,
                     ts_to_string(log->ts_));
      }
      link = log->prev_version_;
    }
  }
  return;

  // We recommend implementing this function as traversing the table heap and print the version chain. An example output
  // of our reference solution:
  //
  // debug_hook: before verify scan
  // RID=0/0 ts=txn8 tuple=(1, <NULL>, <NULL>)
  //   txn8@0 (2, _, _) ts=1
  // RID=0/1 ts=3 tuple=(3, <NULL>, <NULL>)
  //   txn5@0 <del> ts=2
  //   txn3@0 (4, <NULL>, <NULL>) ts=1
  // RID=0/2 ts=4 <del marker> tuple=(<NULL>, <NULL>, <NULL>)
  //   txn7@0 (5, <NULL>, <NULL>) ts=3
  // RID=0/3 ts=txn6 <del marker> tuple=(<NULL>, <NULL>, <NULL>)
  //   txn6@0 (6, <NULL>, <NULL>) ts=2
  //   txn3@1 (7, _, _) ts=1
}

}  // namespace bustub
