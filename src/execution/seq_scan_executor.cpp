//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// seq_scan_executor.cpp
//
// Identification: src/execution/seq_scan_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/seq_scan_executor.h"
#include <utility>
#include <vector>
#include "common/macros.h"
#include "concurrency/transaction_manager.h"
#include "execution/execution_common.h"

namespace bustub {

/**
 * Construct a new SeqScanExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The sequential scan plan to be executed
 */
SeqScanExecutor::SeqScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan)
    : AbstractExecutor(exec_ctx), plan_(plan) {
  // ==== P3 STEP 1: 构造函数只做「登记」，不做任何真实工作 ====
  // 火山模型的算子树是自顶向下一次性构造出来的，此时谁也不知道这棵树会不会
  // 真的被执行（可能被优化器裁掉，也可能上层 Limit 提前结束）。
  // 因此昂贵的初始化——建游标、建哈希表、排序——全部推迟到 Init()。
  table_info_ = exec_ctx_->GetCatalog()->GetTable(plan_->GetTableOid());
}

/** Initialize the sequential scan */
void SeqScanExecutor::Init() {
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
    exec_ctx_->GetTransaction()->AppendScanPredicate(plan_->GetTableOid(), plan_->filter_predicate_);
  }

  // Init() 可能被调用**多次**：作为嵌套循环连接的内表时，外层每产出一条元组
  // 就会把内表重置一遍。所以这里必须是「重新开始」而不是「第一次开始」。
  iter_.emplace(table_info_->table_->MakeIterator());
}

/**
 * Yield the next tuple batch from the seq scan.
 * @param[out] tuple_batch The next tuple batch produced by the scan
 * @param[out] rid_batch The next tuple RID batch produced by the scan
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 */
auto SeqScanExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                           size_t batch_size) -> bool {
  // ==== P3 STEP 2: 向量化执行 —— 一次产出一整批 ====
  // 经典火山模型每次 Next() 只吐一条元组，每条都要经过整棵算子树的虚函数调用链。
  // 批量模型把这个开销摊薄了 batch_size 倍，同时对 CPU 缓存和分支预测都更友好。
  //
  // 约定：Next() 必须先清空出参。返回 true 表示「这一批有数据」，
  // 返回 false 表示「彻底扫完了」。注意二者不是互斥的反面——
  // 扫到最后一批时可能既产出了数据又已到末尾，此时应返回 true，
  // 下一次调用再返回 false（空批）。
  tuple_batch->clear();
  rid_batch->clear();

  const auto &filter = plan_->filter_predicate_;

  auto *txn = exec_ctx_->GetTransaction();
  auto *txn_mgr = exec_ctx_->GetTransactionManager();

  while (!iter_->IsEnd() && tuple_batch->size() < batch_size) {
    auto [meta, tuple] = iter_->GetTuple();
    const auto rid = iter_->GetRID();
    ++(*iter_);

    // ==== P4 STEP 8: 扫描要读的是「本事务快照下的版本」，不是表堆里最新的那份 ====
    // 表堆存的永远是最新版本，它可能比本事务的快照更新（别的事务刚改过），
    // 也可能是别的事务尚未提交的中间状态。所以不能直接用 iter_ 读出来的那份，
    // 必须先做可见性判断，必要时沿 undo 版本链回溯。
    //
    // 这一步让「读」彻底不需要加锁：读者永远回溯到属于自己快照的那个版本，
    // 与并发的写者互不阻塞 —— 这正是 MVCC 相对两阶段封锁的最大优势。
    if (txn != nullptr && txn_mgr != nullptr) {
      auto undo_logs = CollectUndoLogs(rid, meta, tuple, txn_mgr->GetUndoLink(rid), txn, txn_mgr);
      if (!undo_logs.has_value()) {
        continue;  // 该记录在本事务的快照时刻还不存在。
      }
      auto reconstructed = ReconstructTuple(&GetOutputSchema(), tuple, meta, *undo_logs);
      if (!reconstructed.has_value()) {
        continue;  // 在本事务看来它已被删除。
      }
      tuple = std::move(*reconstructed);
    } else if (meta.is_deleted_) {
      // 无事务上下文时退化成 P3 的语义：被删除的元组在表堆里并不会被立刻抹掉，
      // 只是打了 is_deleted_ 标记（和 P2 叶子页的墓碑是同一个思路：
      // 删除只记账，回收交给后续的清理流程），扫描时必须跳过它们。
      continue;
    }

    // 谓词下推：优化器可能把 WHERE 条件直接挂在 SeqScan 上。
    // 在这里过滤而不是交给上层 Filter 算子，能少产生一批中间元组。
    if (filter != nullptr) {
      auto value = filter->Evaluate(&tuple, GetOutputSchema());
      if (value.IsNull() || !value.GetAs<bool>()) {
        continue;
      }
    }

    tuple_batch->emplace_back(std::move(tuple));
    rid_batch->emplace_back(rid);
  }

  return !tuple_batch->empty();
}

}  // namespace bustub
