//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// transaction_manager.cpp
//
// Identification: src/concurrency/transaction_manager.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "concurrency/transaction_manager.h"

#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

#include "catalog/catalog.h"
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/config.h"
#include "common/exception.h"
#include "common/macros.h"
#include "concurrency/transaction.h"
#include "execution/execution_common.h"
#include "storage/table/table_heap.h"
#include "storage/table/tuple.h"
#include "type/type_id.h"
#include "type/value.h"
#include "type/value_factory.h"

namespace bustub {

/**
 * Begins a new transaction.
 * @param isolation_level an optional isolation level of the transaction.
 * @return an initialized transaction
 */
auto TransactionManager::Begin(IsolationLevel isolation_level) -> Transaction * {
  std::unique_lock<std::shared_mutex> l(txn_map_mutex_);
  auto txn_id = next_txn_id_++;
  auto txn = std::make_unique<Transaction>(txn_id, isolation_level);
  auto *txn_ref = txn.get();
  txn_map_.insert(std::make_pair(txn_id, std::move(txn)));

  // ==== P4 STEP 2: 事务开始时确定「读时间戳」====
  // 快照隔离的核心承诺：事务只看得见「它开始那一刻」已经提交的数据。
  // read_ts 就是这个快照点 —— 取当前最新的提交时间戳。
  // 之后无论别人提交了什么，本事务都视而不见，因此读永远不会被写阻塞。
  txn_ref->read_ts_.store(last_commit_ts_.load());

  running_txns_.AddTxn(txn_ref->read_ts_);
  return txn_ref;
}

/** @brief Verify if a txn satisfies serializability. We will not test this function and you can change / remove it as
 * you want. */
auto TransactionManager::VerifyTxn(Transaction *txn) -> bool {
  /*
   * ==== P4 STEP 23: 可串行化验证 —— 抓「写偏斜」====
   *
   * 快照隔离能挡住丢失更新（写写冲突），却挡不住写偏斜 (write skew)：
   *
   *   初始：行 A.x = 1，行 B.x = 0
   *   txn2: UPDATE t SET x = 0 WHERE x = 1   （读到 A，改 A）
   *   txn3: UPDATE t SET x = 1 WHERE x = 0   （读到 B，改 B）
   *
   * 两者的**写集合完全不重叠**，所以没有写写冲突，两个都能提交。
   * 但结果 (A.x=0, B.x=1) 在任何串行顺序下都得不到 —— 不可串行化。
   *
   * 根因是：txn3 读的时候 A.x 还是 1（不匹配它的 `x = 0`），
   * 但 txn2 提交后 A.x 变成了 0 —— 如果 txn3 晚一点读，就会读到 A。
   * 这正是「幻读」的一般形式。
   *
   * 检测办法：提交前回头看一遍。
   *   对本事务扫描过的每张表，逐行检查那些「在我运行期间被别人提交修改过」的行，
   *   看它的**新值或旧值**是否满足我的扫描谓词。满足就说明我本该看到它却没看到
   *   （或本不该看到却看到了），串行化被破坏，只能中止。
   *
   * 这是谓词级的检测：粒度粗，会有误报（把某些其实可串行化的情况判为冲突），
   * 但绝不会漏报。真正精确的做法（SSI）需要跟踪读写依赖图，代价高得多。
   */
  const auto &scan_predicates = txn->GetScanPredicates();
  if (scan_predicates.empty()) {
    return true;  // 什么都没扫过（比如纯插入），无从冲突。
  }
  if (txn->GetWriteSets().empty()) {
    return true;  // 只读事务永远可串行化 —— 它不会让任何人看到不一致的状态。
  }

  for (const auto &[table_oid, predicates] : scan_predicates) {
    auto table_info = catalog_->GetTable(table_oid);
    if (table_info == nullptr) {
      continue;
    }
    const auto &schema = table_info->schema_;

    for (auto iter = table_info->table_->MakeIterator(); !iter.IsEnd(); ++iter) {
      const auto rid = iter.GetRID();
      auto [meta, tuple] = iter.GetTuple();

      // 本事务自己改的行不算冲突。
      if (meta.ts_ == txn->GetTransactionTempTs()) {
        continue;
      }
      // 别的事务尚未提交的修改也不算。
      //
      // 这一条至关重要：提交是被 commit_mutex_ 串行化的，所以走到这里时，
      // 凡是已经提交的事务都已经把临时时间戳换成了真实的 commit_ts。
      // 还带着临时时间戳的，就是**尚未提交**的并发事务 —— 它们可能最终中止，
      // 拿它们的中间状态来判我的冲突是没有道理的。
      // 真要冲突，也该由它们自己在提交时的 VerifyTxn 里发现。
      //
      // 漏掉这个判断的后果：两个互相冲突的事务里，**先提交的那个**会因为
      // 看到后者未提交的修改而误判失败，而后者反倒提交成功 —— 完全颠倒了。
      if (meta.ts_ >= TXN_START_ID) {
        continue;
      }
      // 在我开始之前就定型的行也不算 —— 我读到的就是它的最终值。
      if (meta.ts_ <= txn->GetReadTs()) {
        continue;
      }

      // 剩下的就是「我运行期间被别人动过」的行。
      // 收集它在我这段时间里出现过的所有版本：最新值，以及沿版本链回溯到
      // 我的 read_ts 为止的每一个中间版本。只要**任何一个**匹配我的谓词，
      // 就说明我的读结果本可以不同 —— 冲突。
      std::vector<Tuple> candidate_versions;
      candidate_versions.push_back(tuple);

      std::vector<UndoLog> logs;
      auto link = GetUndoLink(rid);
      while (link.has_value() && link->IsValid()) {
        auto log = GetUndoLogOptional(*link);
        if (!log.has_value()) {
          break;
        }
        logs.push_back(*log);
        auto rebuilt = ReconstructTuple(&schema, tuple, meta, logs);
        if (rebuilt.has_value()) {
          candidate_versions.push_back(*rebuilt);
        }
        if (log->ts_ <= txn->GetReadTs()) {
          break;  // 已经回溯到我的快照点，再往前的版本我根本看不到。
        }
        link = log->prev_version_;
      }

      for (const auto &version : candidate_versions) {
        for (const auto &predicate : predicates) {
          auto value = predicate->Evaluate(&version, schema);
          if (!value.IsNull() && value.GetAs<bool>()) {
            return false;  // 串行化被破坏。
          }
        }
      }
    }
  }
  return true;
}

/**
 * Commits a transaction.
 * @param txn the transaction to commit, the txn will be managed by the txn manager so no need to delete it by
 * yourself
 */
auto TransactionManager::Commit(Transaction *txn) -> bool {
  std::unique_lock<std::mutex> commit_lck(commit_mutex_);

  // ==== P4 STEP 3: 提交时间戳必须在 commit_mutex_ 保护下分配 ====
  // 提交时间戳决定了「其它事务什么时候能看到我的修改」，它必须严格递增且唯一。
  // commit_mutex_ 让「取号 → 写回时间戳 → 发布」这一串成为原子操作：
  // 否则两个并发提交可能拿到同一个号，或者 A 拿到较大的号却先发布，
  // 导致别的事务看到「时间倒流」的版本序列。
  const timestamp_t commit_ts = last_commit_ts_.load() + 1;

  if (txn->state_ != TransactionState::RUNNING) {
    throw Exception("txn not in running state");
  }

  if (txn->GetIsolationLevel() == IsolationLevel::SERIALIZABLE) {
    if (!VerifyTxn(txn)) {
      commit_lck.unlock();
      Abort(txn);
      return false;
    }
  }

  // ==== P4 STEP 4: 提交 = 把「临时时间戳」换成真正的提交时间戳 ====
  // 事务运行期间，它写入的元组带的是 txn_id_（一个 >= TXN_START_ID 的大数），
  // 表示「这行正被某个未提交的事务占用」。别的事务看到这种时间戳就知道
  // 该版本尚未可见。
  //
  // 提交要做的就是把这些临时标记**统一替换成 commit_ts**，
  // 这一刻起本事务的所有修改同时对外可见 —— 保证了提交的原子性
  // （不会出现"改了一半可见"的中间状态）。
  for (const auto &[table_oid, rids] : txn->write_set_) {
    auto table_info = catalog_->GetTable(table_oid);
    for (const auto &rid : rids) {
      auto meta = table_info->table_->GetTupleMeta(rid);
      meta.ts_ = commit_ts;
      table_info->table_->UpdateTupleMeta(meta, rid);
    }
  }

  std::unique_lock<std::shared_mutex> lck(txn_map_mutex_);

  txn->commit_ts_.store(commit_ts);
  // 先写 commit_ts_ 再发布 last_commit_ts_：顺序反了的话，
  // 新开始的事务会拿到一个包含本事务的 read_ts，却看到尚未改完的时间戳。
  last_commit_ts_.store(commit_ts);

  txn->state_ = TransactionState::COMMITTED;
  running_txns_.UpdateCommitTs(txn->commit_ts_);
  running_txns_.RemoveTxn(txn->read_ts_);

  return true;
}

/**
 * Aborts a transaction
 * @param txn the transaction to abort, the txn will be managed by the txn manager so no need to delete it by yourself
 */
void TransactionManager::Abort(Transaction *txn) {
  if (txn->state_ != TransactionState::RUNNING && txn->state_ != TransactionState::TAINTED) {
    throw Exception("txn not in running / tainted state");
  }

  // ==== P4 STEP 5: 中止 —— 必须把堆上的修改真正撤销 ====
  //
  // 直觉上，MVCC 的中止应该"什么都不用做"：本事务的修改一直带着临时时间戳，
  // 从来没对任何人可见过，不换成 commit_ts 就等于没发生。
  //
  // 但这个直觉是**错的**。带临时时间戳的行会被写写冲突检测当成
  // 「正被另一个未提交事务持有」，于是后来的事务永远碰不了它 ——
  // 一个已经死掉的事务把那些行**永久锁死**了。
  //
  // 所以必须逐行回滚：用自己那条 undo log 把内容还原，
  // 并把时间戳恢复成修改之前的值，让这些行重新变成"无主"状态。
  for (const auto &[table_oid, rids] : txn->GetWriteSets()) {
    auto table_info = catalog_->GetTable(table_oid);
    if (table_info == nullptr) {
      continue;
    }
    const auto &schema = table_info->schema_;

    for (const auto &rid : rids) {
      auto [meta, tuple, link] = GetTupleAndUndoLink(this, table_info->table_.get(), rid);
      if (meta.ts_ != txn->GetTransactionTempTs()) {
        continue;  // 这一行已经不归我了（理论上不该发生），别动。
      }

      if (link.has_value() && link->IsValid() && link->prev_txn_ == txn->GetTransactionTempTs()) {
        // 有我自己的 undo log ⇒ 这一行在我之前就存在，还原到那个版本。
        auto log = GetUndoLog(*link);
        auto restored = ReconstructTuple(&schema, tuple, meta, {log});
        if (restored.has_value()) {
          table_info->table_->UpdateTupleInPlace(TupleMeta{log.ts_, false}, *restored, rid);
        } else {
          // 还原的结果是"当时不存在"（我把一条已删除的行复活了，现在撤销）。
          table_info->table_->UpdateTupleInPlace(TupleMeta{log.ts_, true}, tuple, rid);
        }
        // 把版本链头接到更早的版本上，丢弃我这条日志。
        UpdateUndoLink(rid, log.prev_version_);
      } else {
        // 没有我的 undo log ⇒ 这一行是我自己插入的，撤销后它就不该存在。
        // 时间戳置 0 让它对所有人都"早已定型"，配合删除标记即为不可见。
        // 行本身留在堆里等 GC 处理 —— 物理回收从来不在关键路径上做。
        table_info->table_->UpdateTupleInPlace(TupleMeta{0, true}, tuple, rid);
      }
    }
  }

  std::unique_lock<std::shared_mutex> lck(txn_map_mutex_);
  txn->state_ = TransactionState::ABORTED;
  running_txns_.RemoveTxn(txn->read_ts_);
}

/** @brief Stop-the-world garbage collection. Will be called only when all transactions are not accessing the table
 * heap. */
void TransactionManager::GarbageCollection() {
  /*
   * ==== P4 STEP 16: 垃圾回收 —— MVCC 必须付的税 ====
   *
   * 版本链会无限增长，总得有人清理。什么可以清理？
   *
   *   一条 undo log 还有用，当且仅当**某个可能出现的读者**还需要它。
   *   水位线（= 所有活跃事务中最小的 read_ts）之下的历史，谁都读不到了。
   *
   * 但注意不能简单地"删掉所有 ts < watermark 的日志"：
   * 一个 read_ts 恰好等于水位线的事务，仍然需要**第一条 ts <= watermark 的日志**
   * 来重建它那个版本。所以每条版本链上要保留到"第一个足够旧的版本"为止。
   *
   *   版本链：  base(ts=9) → log(ts=7) → log(ts=4) → log(ts=2) → log(ts=1)
   *   watermark = 5                          ▲
   *                        ├─── 必须保留 ────┤ └──── 可以回收 ────┘
   *
   * BusTub 的回收粒度是**整个事务**：一个事务的所有 undo log 存在它自己的
   * undo_logs_ 数组里，只有当它的日志全都不再被任何版本链引用时，
   * 才能把这个事务对象从 txn_map_ 里移除（连带释放它的全部日志）。
   *
   * 这是一次「停止世界」(stop-the-world) 的回收：调用时保证没有事务在访问表堆。
   * 真实系统会做增量/后台回收，但原理相同。
   */
  const auto watermark = GetWatermark();

  // 第一步：扫描所有表的所有版本链，标记出「仍被引用」的事务。
  std::unordered_set<txn_id_t> still_referenced;

  for (const auto &table_name : catalog_->GetTableNames()) {
    auto table_info = catalog_->GetTable(table_name);
    if (table_info == nullptr) {
      continue;
    }
    for (auto iter = table_info->table_->MakeIterator(); !iter.IsEnd(); ++iter) {
      auto [meta, tuple] = iter.GetTuple();

      // 表堆里那份已经足够旧 ⇒ 任何活跃事务都能直接读它，整条历史链都可以扔。
      bool need_more = meta.ts_ > watermark;

      auto link = GetUndoLink(iter.GetRID());
      while (need_more && link.has_value() && link->IsValid()) {
        auto log = GetUndoLogOptional(*link);
        if (!log.has_value()) {
          break;  // 已经被回收过了。
        }
        // 这条日志还可能被读到，于是它所属的事务不能被清理。
        still_referenced.insert(link->prev_txn_);
        if (log->ts_ <= watermark) {
          // 找到了第一个「足够旧」的版本，再往前的历史谁也读不到了。
          need_more = false;
        }
        link = log->prev_version_;
      }
    }
  }

  // 第二步：把已经结束（提交或中止）且再无人引用的事务整个移除。
  // 仍在运行的事务当然不能碰 —— 它随时可能再写新的 undo log。
  std::unique_lock<std::shared_mutex> lck(txn_map_mutex_);
  for (auto it = txn_map_.begin(); it != txn_map_.end();) {
    const auto state = it->second->GetTransactionState();
    const bool finished = state == TransactionState::COMMITTED || state == TransactionState::ABORTED;
    if (finished && still_referenced.find(it->first) == still_referenced.end()) {
      it = txn_map_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace bustub
