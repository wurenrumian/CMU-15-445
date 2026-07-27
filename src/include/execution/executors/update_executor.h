//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// update_executor.h
//
// Identification: src/include/execution/executors/update_executor.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/update_plan.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * UpdateExecutor executes an update on a table.
 * Updated values are always pulled from a child.
 */
class UpdateExecutor : public AbstractExecutor {
  friend class UpdatePlanNode;

 public:
  UpdateExecutor(ExecutorContext *exec_ctx, const UpdatePlanNode *plan,
                 std::unique_ptr<AbstractExecutor> &&child_executor);

  void Init() override;

  auto Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
      -> bool override;

  /** @return The output schema for the update */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); }

 private:
  /** The update plan node to be executed */
  const UpdatePlanNode *plan_;

  /** Metadata identifying the table that should be updated */
  std::shared_ptr<TableInfo> table_info_;

  /** The child executor to obtain value from */
  std::unique_ptr<AbstractExecutor> child_executor_;

  /** 该表上的所有索引。更新会改变键值，索引项必须「先删旧、再插新」。 */
  std::vector<std::shared_ptr<IndexInfo>> indexes_;

  /** 与 Insert/Delete 同理：更新是阻塞式算子，结果行只产出一次。 */
  bool done_{false};
};
}  // namespace bustub
