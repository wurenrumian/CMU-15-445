//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// insert_executor.cpp
//
// Identification: src/execution/insert_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <utility>
#include <vector>
#include "common/exception.h"
#include "common/macros.h"
#include "concurrency/transaction_manager.h"
#include "execution/execution_common.h"

#include "execution/executors/insert_executor.h"

namespace bustub {

/**
 * Construct a new InsertExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The insert plan to be executed
 * @param child_executor The child executor from which inserted tuples are pulled
 */
InsertExecutor::InsertExecutor(ExecutorContext *exec_ctx, const InsertPlanNode *plan,
                               std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {
  table_info_ = exec_ctx_->GetCatalog()->GetTable(plan_->GetTableOid());
  indexes_ = exec_ctx_->GetCatalog()->GetTableIndexes(table_info_->name_);
}

/** Initialize the insert */
void InsertExecutor::Init() {
  child_executor_->Init();
  done_ = false;
}

/**
 * Yield the number of rows inserted into the table.
 * @param[out] tuple_batch The tuple batch with one integer indicating the number of rows inserted into the table
 * @param[out] rid_batch The next tuple RID batch produced by the insert (ignore, not used)
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 *
 * NOTE: InsertExecutor::Next() does not use the `rid_batch` out-parameter.
 * NOTE: InsertExecutor::Next() returns true with the number of inserted rows produced only once.
 */
auto InsertExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                          size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  // ==== P3 STEP 4: 写算子是「阻塞式」的 ====
  // 和 SeqScan 这种流式算子不同，INSERT 必须先把子算子**彻底消费完**，
  // 才知道自己一共插了几行。中途返回会导致后半截数据根本没被插入。
  // done_ 保证这件事只做一次——否则第二次 Next() 会把所有数据再插一遍。
  if (done_) {
    return false;
  }
  done_ = true;

  auto *txn = exec_ctx_->GetTransaction();

  int32_t inserted = 0;
  std::vector<Tuple> child_tuples;
  std::vector<RID> child_rids;

  auto *txn_mgr = exec_ctx_->GetTransactionManager();
  const auto &schema = table_info_->schema_;

  // 找出主键索引（如果有）。MVCC 下它是唯一性约束的执行者。
  std::shared_ptr<IndexInfo> primary_index = nullptr;
  for (const auto &index : indexes_) {
    if (index->is_primary_key_) {
      primary_index = index;
      break;
    }
  }

  while (child_executor_->Next(&child_tuples, &child_rids, batch_size)) {
    for (auto &tuple : child_tuples) {
      // ==== P4 STEP 17: 主键唯一性 —— 插入前必须先查索引 ====
      // 没有主键索引时，插入就是纯追加。有主键索引时，同一个键可能已经存在，
      // 而且它有两种截然不同的状态：
      //
      //   ① 那一行还活着 → 违反唯一性约束 → 本事务失败（TAINTED）。
      //   ② 那一行已被删除（墓碑）→ 允许「复活」：
      //      在**原来的 RID 上**原地写入新值，而不是插一条新记录。
      //
      // 为什么必须复活而不是新插？因为索引项 (键 → RID) 还指向旧的 RID。
      // 若新插一条，就会出现同一个键对应两个 RID 的局面，唯一性彻底失效。
      // 这也是 MVCC + 主键索引组合起来最微妙的地方。
      if (primary_index != nullptr) {
        auto key = tuple.KeyFromTuple(schema, primary_index->key_schema_, primary_index->index_->GetKeyAttrs());
        std::vector<RID> existing;
        primary_index->index_->ScanKey(key, &existing, txn);

        if (!existing.empty()) {
          const auto rid = existing.front();
          auto [meta, old_tuple, undo_link] = GetTupleAndUndoLink(txn_mgr, table_info_->table_.get(), rid);

          CheckWriteConflict(meta, txn);
          if (!meta.is_deleted_) {
            // 情况 ①：键已被一条存活的记录占用。
            txn->SetTainted();
            throw ExecutionException("duplicate primary key");
          }

          // 情况 ②：复活这一行。
          PrepareUndoLog(txn_mgr, txn, &schema, rid, meta, old_tuple, &tuple, undo_link);
          table_info_->table_->UpdateTupleInPlace(TupleMeta{txn->GetTransactionTempTs(), false}, tuple, rid);
          txn->AppendWriteSet(plan_->GetTableOid(), rid);
          // 索引项本来就指向这个 RID，无需改动。
          ++inserted;
          continue;
        }
      }

      // ==== P4 STEP 12: 新插入的行先带「临时时间戳」====
      // 事务运行期间写入的元组用 txn_id_（>= TXN_START_ID 的大数）作时间戳，
      // 它对任何别的事务都不可见（可见性判断会走"太新"分支并回溯版本链，
      // 而这一行根本没有更老的版本，于是判定为"当时还不存在"）。
      // 提交时 TransactionManager::Commit 会把它统一换成 commit_ts，
      // 那一刻这一行才对外界出现 —— 这就是插入的原子性。
      const auto rid = table_info_->table_->InsertTuple(TupleMeta{txn->GetTransactionTempTs(), false}, tuple,
                                                        exec_ctx_->GetLockManager(), txn, plan_->GetTableOid());
      if (!rid.has_value()) {
        continue;  // 表页写满且无法扩展（正常情况下不会发生）。
      }

      // 记入写集合。Commit 时要靠它找到本事务改过的所有行，
      // 把临时时间戳换成 commit_ts。漏记 = 这一行永远不会对外可见。
      txn->AppendWriteSet(plan_->GetTableOid(), *rid);

      // ---- 索引必须与表同步更新 ----
      // 索引里存的是 (键, RID)。键要用 KeyFromTuple 从完整元组里"投影"出来：
      // 索引只关心被索引的那几列，key_attrs_ 记录的正是这个映射。
      for (auto &index : indexes_) {
        auto key = tuple.KeyFromTuple(schema, index->key_schema_, index->index_->GetKeyAttrs());
        if (!index->index_->InsertEntry(key, *rid, txn) && index->is_primary_key_) {
          // 并发场景：另一个事务在我们查完索引之后、插入之前抢先占了这个键。
          // 索引插入失败是唯一性冲突的最后一道防线。
          txn->SetTainted();
          throw ExecutionException("duplicate primary key (lost the race)");
        }
      }
      ++inserted;
    }
  }

  // 产出唯一的一行结果：插入的行数。
  // 这一行的模式由 plan_->OutputSchema() 定义（单个 INTEGER 列）。
  std::vector<Value> values{Value(TypeId::INTEGER, inserted)};
  tuple_batch->emplace_back(values, &GetOutputSchema());
  // rid_batch 与 tuple_batch 必须等长（上层算子按同一下标访问两者）。
  rid_batch->emplace_back();
  return true;
}

}  // namespace bustub
