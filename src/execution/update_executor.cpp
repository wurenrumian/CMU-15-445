//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// update_executor.cpp
//
// Identification: src/execution/update_executor.cpp
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

#include "execution/executors/update_executor.h"

namespace bustub {

/**
 * Construct a new UpdateExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The update plan to be executed
 * @param child_executor The child executor that feeds the update
 */
UpdateExecutor::UpdateExecutor(ExecutorContext *exec_ctx, const UpdatePlanNode *plan,
                               std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {
  table_info_ = exec_ctx_->GetCatalog()->GetTable(plan_->GetTableOid());
  indexes_ = exec_ctx_->GetCatalog()->GetTableIndexes(table_info_->name_);
}

/** Initialize the update */
void UpdateExecutor::Init() {
  child_executor_->Init();
  done_ = false;
}

/**
 * Yield the number of rows updated in the table.
 * @param[out] tuple_batch The tuple batch with one integer indicating the number of rows updated in the table
 * @param[out] rid_batch The next tuple RID batch produced by the update (ignore, not used)
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 *
 * NOTE: UpdateExecutor::Next() does not use the `rid_batch` out-parameter.
 * NOTE: UpdateExecutor::Next() returns true with the number of updated rows produced only once.
 */
auto UpdateExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                          size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  if (done_) {
    return false;
  }
  done_ = true;

  int32_t updated = 0;
  std::vector<Tuple> child_tuples;
  std::vector<RID> child_rids;
  const auto &schema = table_info_->schema_;
  auto *txn = exec_ctx_->GetTransaction();
  auto *txn_mgr = exec_ctx_->GetTransactionManager();

  // 找出主键索引（如果有）。
  std::shared_ptr<IndexInfo> primary_index = nullptr;
  for (const auto &index : indexes_) {
    if (index->is_primary_key_) {
      primary_index = index;
      break;
    }
  }

  // ==== P4 STEP 20: 先收集，再统一修改 ====
  // 更新主键时必须走「删除旧行 + 插入新行」（见下），而插入会往表堆追加新记录。
  // 如果边扫边改，新插入的行可能落在 SeqScan 尚未扫过的位置，
  // 于是同一条逻辑记录被更新两次，甚至无限循环。
  // 把「读取」和「写入」两个阶段彻底分开，问题从根上消失。
  std::vector<RID> target_rids;
  std::vector<Tuple> new_tuples;
  while (child_executor_->Next(&child_tuples, &child_rids, batch_size)) {
    for (size_t i = 0; i < child_tuples.size(); i++) {
      // ---- 用 target_expressions 算出新元组 ----
      // UPDATE t SET a = a + 1, b = 'x' 被规划成「每个输出列一个表达式」的形式：
      // 没被修改的列对应一个 ColumnValueExpression（原样取出），
      // 被修改的列对应实际的计算表达式。这样执行器完全不必关心 SQL 语法细节。
      std::vector<Value> row;
      row.reserve(plan_->target_expressions_.size());
      for (const auto &expr : plan_->target_expressions_) {
        row.push_back(expr->Evaluate(&child_tuples[i], child_executor_->GetOutputSchema()));
      }
      target_rids.push_back(child_rids[i]);
      new_tuples.emplace_back(row, &schema);
    }
  }

  // ---- 判断这次更新会不会改动主键 ----
  bool primary_key_changed = false;
  if (primary_index != nullptr) {
    const auto &key_attrs = primary_index->index_->GetKeyAttrs();
    for (size_t i = 0; i < target_rids.size(); i++) {
      auto [meta, old_tuple, undo_link] = GetTupleAndUndoLink(txn_mgr, table_info_->table_.get(), target_rids[i]);
      auto old_key = old_tuple.KeyFromTuple(schema, primary_index->key_schema_, key_attrs);
      auto new_key = new_tuples[i].KeyFromTuple(schema, primary_index->key_schema_, key_attrs);
      if (!IsTupleContentEqual(old_key, new_key)) {
        primary_key_changed = true;
        break;
      }
    }
  }

  if (!primary_key_changed) {
    // ==== P4 STEP 15: 普通更新是「原地覆盖」，不是删除+插入 ====
    // 这与 P3 的做法正好相反。原因是版本链以 RID 为锚：
    // 同一条逻辑记录的所有历史版本必须挂在**同一个 RID** 上，
    // 老事务才能顺着 undo 链找回自己那个版本。换 RID 就等于换了一条记录，
    // 版本链会断掉。
    // 索引项指向的 RID 也没变，因此索引完全不用动。
    for (size_t i = 0; i < target_rids.size(); i++) {
      const auto rid = target_rids[i];
      auto [meta, old_tuple, undo_link] = GetTupleAndUndoLink(txn_mgr, table_info_->table_.get(), rid);
      CheckWriteConflict(meta, txn);
      PrepareUndoLog(txn_mgr, txn, &schema, rid, meta, old_tuple, &new_tuples[i], undo_link);
      table_info_->table_->UpdateTupleInPlace(TupleMeta{txn->GetTransactionTempTs(), false}, new_tuples[i], rid);
      txn->AppendWriteSet(plan_->GetTableOid(), rid);

      // ---- 二级索引仍需维护 ----
      // 主键索引不用动（键没变，RID 也没变）。但**非主键**索引的键可能变了，
      // 例如 `CREATE INDEX ON t(v1)` 之后执行 `UPDATE t SET v1 = -v1`。
      // 留着旧键会让有序索引扫描按错误的顺序返回结果 —— 这正是
      // p3.05-index-scan-btree 抓到的回归。
      //
      // 注意这里对老事务是不友好的（二级索引项被立刻改掉，不像主键索引那样
      // 版本无关）。BusTub 的 P4 只要求主键索引支持 MVCC，二级索引沿用 P3 语义。
      for (auto &index : indexes_) {
        if (index->is_primary_key_) {
          continue;
        }
        auto old_key = old_tuple.KeyFromTuple(schema, index->key_schema_, index->index_->GetKeyAttrs());
        auto new_key = new_tuples[i].KeyFromTuple(schema, index->key_schema_, index->index_->GetKeyAttrs());
        if (IsTupleContentEqual(old_key, new_key)) {
          continue;  // 键没变，索引项原样有效。
        }
        index->index_->DeleteEntry(old_key, rid, txn);
        index->index_->InsertEntry(new_key, rid, txn);
      }
      ++updated;
    }
  } else {
    // ==== P4 STEP 21: 改主键时只能「删除 + 插入」====
    // 主键索引的项是 (键 → RID)。键变了，索引项就必须换位置；
    // 而 MVCC 又要求同一 RID 上的版本链保持连续。两个约束无法同时满足，
    // 所以只能把它当成「删掉一条旧记录、插入一条新记录」两件独立的事：
    //   - 旧 RID：打删除标记 + 生成 undo log（老事务仍能读到旧值）；
    //   - 新键：若已存在墓碑就复活它，否则追加新行并插索引项。
    //
    // 必须**先全部删除、再全部插入**：否则新插入的键可能与尚未删除的旧键撞车，
    // 被误判成唯一性冲突（比如 `UPDATE t SET a = a + 1` 会让 1→2 撞上原来的 2）。
    for (const auto rid : target_rids) {
      auto [meta, old_tuple, undo_link] = GetTupleAndUndoLink(txn_mgr, table_info_->table_.get(), rid);
      CheckWriteConflict(meta, txn);
      PrepareUndoLog(txn_mgr, txn, &schema, rid, meta, old_tuple, nullptr, undo_link);
      table_info_->table_->UpdateTupleInPlace(TupleMeta{txn->GetTransactionTempTs(), true}, old_tuple, rid);
      txn->AppendWriteSet(plan_->GetTableOid(), rid);
    }

    for (auto &new_tuple : new_tuples) {
      auto key = new_tuple.KeyFromTuple(schema, primary_index->key_schema_, primary_index->index_->GetKeyAttrs());
      std::vector<RID> existing;
      primary_index->index_->ScanKey(key, &existing, txn);

      if (!existing.empty()) {
        // 这个键已经有对应的行（可能是刚被我们删掉的那条，也可能是历史墓碑）→ 复活它。
        const auto rid = existing.front();
        auto [meta, old_tuple, undo_link] = GetTupleAndUndoLink(txn_mgr, table_info_->table_.get(), rid);
        CheckWriteConflict(meta, txn);
        if (!meta.is_deleted_) {
          txn->SetTainted();
          throw ExecutionException("duplicate primary key after update");
        }
        PrepareUndoLog(txn_mgr, txn, &schema, rid, meta, old_tuple, &new_tuple, undo_link);
        table_info_->table_->UpdateTupleInPlace(TupleMeta{txn->GetTransactionTempTs(), false}, new_tuple, rid);
        txn->AppendWriteSet(plan_->GetTableOid(), rid);
      } else {
        const auto rid = table_info_->table_->InsertTuple(TupleMeta{txn->GetTransactionTempTs(), false}, new_tuple,
                                                          exec_ctx_->GetLockManager(), txn, plan_->GetTableOid());
        if (!rid.has_value()) {
          continue;
        }
        txn->AppendWriteSet(plan_->GetTableOid(), *rid);
        for (auto &index : indexes_) {
          auto index_key = new_tuple.KeyFromTuple(schema, index->key_schema_, index->index_->GetKeyAttrs());
          if (!index->index_->InsertEntry(index_key, *rid, txn) && index->is_primary_key_) {
            txn->SetTainted();
            throw ExecutionException("duplicate primary key after update (lost the race)");
          }
        }
      }
      ++updated;
    }
  }

  std::vector<Value> values{Value(TypeId::INTEGER, updated)};
  tuple_batch->emplace_back(values, &GetOutputSchema());
  // rid_batch 与 tuple_batch 必须等长（上层算子按同一下标访问两者）。
  rid_batch->emplace_back();
  return true;
}

}  // namespace bustub
