//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// delete_executor.cpp
//
// Identification: src/execution/delete_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <utility>
#include <vector>
#include "common/macros.h"
#include "concurrency/transaction_manager.h"
#include "execution/execution_common.h"

#include "execution/executors/delete_executor.h"

namespace bustub {

/**
 * Construct a new DeleteExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The delete plan to be executed
 * @param child_executor The child executor that feeds the delete
 */
DeleteExecutor::DeleteExecutor(ExecutorContext *exec_ctx, const DeletePlanNode *plan,
                               std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {
  table_info_ = exec_ctx_->GetCatalog()->GetTable(plan_->GetTableOid());
  indexes_ = exec_ctx_->GetCatalog()->GetTableIndexes(table_info_->name_);
}

/** Initialize the delete */
void DeleteExecutor::Init() {
  child_executor_->Init();
  done_ = false;
}

/**
 * Yield the number of rows deleted from the table.
 * @param[out] tuple_batch The tuple batch with one integer indicating the number of rows deleted from the table
 * @param[out] rid_batch The next tuple RID batch produced by the delete (ignore, not used)
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 *
 * NOTE: DeleteExecutor::Next() does not use the `rid_batch` out-parameter.
 * NOTE: DeleteExecutor::Next() returns true with the number of deleted rows produced only once.
 */
auto DeleteExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                          size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  if (done_) {
    return false;
  }
  done_ = true;

  int32_t deleted = 0;
  std::vector<Tuple> child_tuples;
  std::vector<RID> child_rids;

  auto *txn = exec_ctx_->GetTransaction();
  auto *txn_mgr = exec_ctx_->GetTransactionManager();
  const auto &schema = table_info_->schema_;

  while (child_executor_->Next(&child_tuples, &child_rids, batch_size)) {
    for (size_t i = 0; i < child_tuples.size(); i++) {
      const auto rid = child_rids[i];
      auto [meta, old_tuple, undo_link] = GetTupleAndUndoLink(txn_mgr, table_info_->table_.get(), rid);

      // 先判冲突，再动手。冲突时会抛 ExecutionException，
      // 已经改过的那些行保持临时时间戳、对外不可见，由 Abort 兜底。
      CheckWriteConflict(meta, txn);

      // target_tuple 传 nullptr 表示这是一次删除：undo log 要留下**整行**旧值，
      // 否则将来无法为老事务恢复出完整的记录。
      PrepareUndoLog(txn_mgr, txn, &schema, rid, meta, old_tuple, nullptr, undo_link);

      // ==== P3 STEP 5 / P4: 删除是「打标记」，不是「抹掉」====
      // 只把 TupleMeta.is_deleted_ 置为 true，元组的字节仍然留在页里。
      // 三个理由：
      //   1. 真正抹掉需要挪动页内数据，而其它元组的 RID（页号+槽号）会因此失效；
      //   2. MVCC 需要旧版本还在原地，才能让老事务读到它；
      //   3. 和 P2 叶子页的墓碑完全是同一个思想——删除只记账，回收交给后续流程。
      // 时间戳换成本事务的临时值：删除对别人暂时不可见，提交时才生效。
      table_info_->table_->UpdateTupleInPlace(TupleMeta{txn->GetTransactionTempTs(), true}, old_tuple, rid);
      txn->AppendWriteSet(plan_->GetTableOid(), rid);

      // ==== P4 STEP 19: MVCC 下删除**不摘**索引项 ====
      // 这与 P3 的做法相反，原因有二：
      //   ① 老事务的快照里这一行还活着，它必须仍能通过索引找到这个 RID；
      //   ② 索引项是唯一性约束的持有者。摘掉之后，另一个事务就能插入同一个主键、
      //      拿到一个**新的 RID**，于是同一个键对应两条记录，唯一性彻底失效。
      //      保留索引项，插入方就会发现"键已存在但那行是墓碑"，从而走「复活」路径。
      //
      // 索引项因此变成了版本无关的指针，可见性完全交给版本链判断
      // （见 index_scan_executor.cpp 的 P4 STEP 18）。
      //
      // 但**非主键**索引例外：它不承担唯一性约束，也不参与「复活」逻辑。
      // 留着陈旧的键项有两个坏处：
      //   ① 有序索引扫描会按错误的顺序返回结果；
      //   ② B+ 树索引只支持唯一键，陈旧项会让后续插入同一个键**静默失败**，
      //      于是新行进不了索引，走索引的查询直接看不到它。
      // BusTub 的 P4 只要求主键索引支持 MVCC，二级索引沿用 P3 的即时维护语义。
      for (auto &index : indexes_) {
        if (index->is_primary_key_) {
          continue;
        }
        auto key = child_tuples[i].KeyFromTuple(schema, index->key_schema_, index->index_->GetKeyAttrs());
        index->index_->DeleteEntry(key, rid, txn);
      }
      ++deleted;
    }
  }

  std::vector<Value> values{Value(TypeId::INTEGER, deleted)};
  tuple_batch->emplace_back(values, &GetOutputSchema());
  // rid_batch 与 tuple_batch 必须等长（上层算子按同一下标访问两者）。
  rid_batch->emplace_back();
  return true;
}

}  // namespace bustub
