//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// index_scan_executor.cpp
//
// Identification: src/execution/index_scan_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/index_scan_executor.h"
#include <memory>
#include <utility>
#include <vector>
#include "common/macros.h"
#include "concurrency/transaction_manager.h"
#include "execution/execution_common.h"

namespace bustub {

/**
 * Creates a new index scan executor.
 * @param exec_ctx the executor context
 * @param plan the index scan plan to be executed
 */
IndexScanExecutor::IndexScanExecutor(ExecutorContext *exec_ctx, const IndexScanPlanNode *plan)
    : AbstractExecutor(exec_ctx), plan_(plan) {
  index_info_ = exec_ctx_->GetCatalog()->GetIndex(plan_->GetIndexOid());
  table_info_ = exec_ctx_->GetCatalog()->GetTable(index_info_->table_name_);
  // 计划里带了点查键，就走「等值查找」；否则走「按键序全表有序遍历」。
  point_lookup_ = !plan_->pred_keys_.empty();
}

void IndexScanExecutor::Init() {
  // ==== P4 STEP 22: 可串行化需要记住「我读过什么」====
  // 快照隔离只管写写冲突，挡不住「写偏斜」(write skew)：
  // 两个事务各自读一批行、再各自改另一批行，写集合完全不重叠，
  // 却可能产生任何串行顺序都得不到的结果。
  //
  // 要检测它，就必须知道每个事务的**读集合**。逐行记录太贵，
  // BusTub 采用更粗的粒度：记下扫描时用的谓词。提交时再回头检查
  // 「有没有并发事务改出了一行满足我这个谓词的数据」。
  // 这是谓词级（predicate-based）的冲突检测，会有误报但不会漏报。
  if (exec_ctx_->GetTransaction() != nullptr &&
      exec_ctx_->GetTransaction()->GetIsolationLevel() == IsolationLevel::SERIALIZABLE &&
      plan_->filter_predicate_ != nullptr) {
    exec_ctx_->GetTransaction()->AppendScanPredicate(table_info_->oid_, plan_->filter_predicate_);
  }

  rids_.clear();
  cursor_ = 0;

  auto *tree = dynamic_cast<BPlusTreeIndexForTwoIntegerColumn *>(index_info_->index_.get());

  if (point_lookup_) {
    // ==== P3 STEP 7: 点查 —— 索引扫描相对顺序扫描的全部意义 ====
    // `WHERE a = 1 OR a = 5` 会被优化器改写成带两个 pred_key 的 IndexScan。
    // 对每个键做一次 B+ 树查找（O(log n)），而不是把整张表读一遍（O(n)）。
    for (const auto &pred_key : plan_->pred_keys_) {
      // pred_key 是一个常量表达式，Evaluate(nullptr, ...) 直接求出它的值。
      auto value = pred_key->Evaluate(nullptr, index_info_->key_schema_);
      std::vector<Value> key_values{value};
      Tuple key{key_values, &index_info_->key_schema_};

      std::vector<RID> found;
      index_info_->index_->ScanKey(key, &found, exec_ctx_->GetTransaction());
      rids_.insert(rids_.end(), found.begin(), found.end());
    }
    return;
  }

  // 有序遍历：优化器把 `ORDER BY a` 改写成索引扫描时走这条路。
  // 索引的叶子层本来就是按键有序的单向链表，顺着走一遍就等于排好序了，
  // 完全不需要额外的排序算子——这是「索引即物化的排序结果」这一思想的体现。
  if (tree != nullptr) {
    iter_.emplace(tree->GetBeginIterator());
  }
}

auto IndexScanExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                             size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  const auto &filter = plan_->filter_predicate_;

  // 把一个 RID「回表」取出完整元组，并过滤掉已删除的 / 不满足谓词的。
  // 索引里只有被索引的列，其余列必须回表堆取——这就是回表的代价，
  // 也是为什么覆盖索引（索引本身就包含所需全部列）在真实数据库里那么重要。
  auto *txn = exec_ctx_->GetTransaction();
  auto *txn_mgr = exec_ctx_->GetTransactionManager();

  auto emit = [&](RID rid) {
    auto [meta, tuple] = table_info_->table_->GetTuple(rid);

    // ==== P4 STEP 18: 索引扫描同样要做可见性判断 ====
    // 索引项是「版本无关」的：它只记录 (键 → RID)，不区分版本。
    // MVCC 下删除**不会**摘除索引项（否则老事务就找不到自己那个版本了），
    // 所以走索引拿到 RID 之后，必须和顺序扫描一样回溯版本链，
    // 由版本链而不是索引来决定这一行对本事务是否存在。
    if (txn != nullptr && txn_mgr != nullptr) {
      auto undo_logs = CollectUndoLogs(rid, meta, tuple, txn_mgr->GetUndoLink(rid), txn, txn_mgr);
      if (!undo_logs.has_value()) {
        return;
      }
      auto reconstructed = ReconstructTuple(&GetOutputSchema(), tuple, meta, *undo_logs);
      if (!reconstructed.has_value()) {
        return;
      }
      tuple = std::move(*reconstructed);
    } else if (meta.is_deleted_) {
      return;
    }
    if (filter != nullptr) {
      auto value = filter->Evaluate(&tuple, GetOutputSchema());
      if (value.IsNull() || !value.GetAs<bool>()) {
        return;
      }
    }
    tuple_batch->emplace_back(std::move(tuple));
    rid_batch->emplace_back(rid);
  };

  if (point_lookup_) {
    while (cursor_ < rids_.size() && tuple_batch->size() < batch_size) {
      emit(rids_[cursor_++]);
    }
  } else if (iter_.has_value()) {
    while (!iter_->IsEnd() && tuple_batch->size() < batch_size) {
      emit((**iter_).second);
      ++(*iter_);
    }
  }

  return !tuple_batch->empty();
}

}  // namespace bustub
