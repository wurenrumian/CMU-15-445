//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// insert_executor.h
//
// Identification: src/include/execution/executors/insert_executor.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/insert_plan.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * InsertExecutor executes an insert on a table.
 * Inserted values are always pulled from a child executor.
 */
class InsertExecutor : public AbstractExecutor {
 public:
  InsertExecutor(ExecutorContext *exec_ctx, const InsertPlanNode *plan,
                 std::unique_ptr<AbstractExecutor> &&child_executor);

  void Init() override;

  auto Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
      -> bool override;

  /** @return The output schema for the insert */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); };

 private:
  /** The insert plan node to be executed*/
  const InsertPlanNode *plan_;

  /** 提供待插入元组的子算子（通常是 Values 或另一个查询）。 */
  std::unique_ptr<AbstractExecutor> child_executor_;

  /** 目标表。 */
  std::shared_ptr<TableInfo> table_info_;

  /**
   * @brief 该表上的所有索引。
   *
   * 每插入一条元组，**必须**同步往每个索引里插一条键项，否则索引会和表脱节，
   * 之后走索引的查询就会查不到刚插入的数据。这就是「索引维护是写操作的固有成本」。
   */
  std::vector<std::shared_ptr<IndexInfo>> indexes_;

  /**
   * @brief 是否已经把「插入了几行」这个结果吐出去了。
   *
   * INSERT 是一个「一次性产出单行结果」的算子：它必须先把子算子**全部**消费完
   * （否则只插了一半），然后产出一行 `(插入行数)`。所以需要这个标志来保证
   * 第二次调用 Next() 时返回 false，而不是把所有数据再插一遍。
   */
  bool done_{false};
};

}  // namespace bustub
