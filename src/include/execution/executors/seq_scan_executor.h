//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// seq_scan_executor.h
//
// Identification: src/include/execution/executors/seq_scan_executor.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/seq_scan_plan.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * The SeqScanExecutor executor executes a sequential table scan.
 */
class SeqScanExecutor : public AbstractExecutor {
 public:
  SeqScanExecutor(ExecutorContext *exec_ctx, const SeqScanPlanNode *plan);

  void Init() override;

  auto Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
      -> bool override;

  /** @return The output schema for the sequential scan */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); }

 private:
  /** The sequential scan plan node to be executed */
  const SeqScanPlanNode *plan_;

  /**
   * @brief 表堆的游标。
   *
   * 用 optional 而不是直接持有，是因为 TableIterator 没有默认构造函数
   *（它必须在构造时就绑定表和起止 RID），而 Init() 可能被调用多次
   *（例如作为嵌套循环连接的内表，每次外层元组都要重新扫一遍）。
   * optional 让「重新开始扫描」变成简单的 emplace 覆盖。
   */
  std::optional<TableIterator> iter_{std::nullopt};

  /** 缓存 catalog 里的表信息，避免每批都查一次。 */
  std::shared_ptr<TableInfo> table_info_{nullptr};
};
}  // namespace bustub
