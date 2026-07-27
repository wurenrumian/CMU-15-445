//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// index_scan_executor.h
//
// Identification: src/include/execution/executors/index_scan_executor.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "common/rid.h"
#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/index_scan_plan.h"
#include "storage/index/b_plus_tree_index.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * IndexScanExecutor executes an index scan over a table.
 */

class IndexScanExecutor : public AbstractExecutor {
 public:
  IndexScanExecutor(ExecutorContext *exec_ctx, const IndexScanPlanNode *plan);

  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); }

  void Init() override;

  auto Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
      -> bool override;

 private:
  /** The index scan plan node to be executed. */
  const IndexScanPlanNode *plan_;

  /** 被扫描的索引及其所属的表。 */
  std::shared_ptr<IndexInfo> index_info_;
  std::shared_ptr<TableInfo> table_info_;

  /**
   * @brief 点查模式下，一次性解析出来的候选 RID 列表。
   *
   * 索引查出来的是 RID（行的物理地址），还要再回表堆取一次完整元组
   * ——这就是所谓「回表」。索引本身只存被索引的那几列，其余列必须从表里拿。
   */
  std::vector<RID> rids_;

  /** 已经消费到 rids_ 的第几个（支持分批产出）。 */
  size_t cursor_{0};

  /**
   * @brief 全序扫描模式下的 B+ 树迭代器。
   *
   * 当计划里没有点查键时（例如优化器把 `ORDER BY a` 改写成索引扫描），
   * 就顺着 B+ 树的叶子链表按键序遍历整个索引。
   */
  std::optional<BPlusTreeIndexIteratorForTwoIntegerColumn> iter_{std::nullopt};

  /** true 表示走点查路径（rids_），false 表示走有序遍历路径（iter_）。 */
  bool point_lookup_{false};
};
}  // namespace bustub
