//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// nested_loop_join_executor.h
//
// Identification: src/include/execution/executors/nested_loop_join_executor.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <memory>
#include <vector>

#include "execution/executor_context.h"
#include "execution/executors/abstract_executor.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * NestedLoopJoinExecutor executes a nested-loop JOIN on two tables.
 */
class NestedLoopJoinExecutor : public AbstractExecutor {
 public:
  NestedLoopJoinExecutor(ExecutorContext *exec_ctx, const NestedLoopJoinPlanNode *plan,
                         std::unique_ptr<AbstractExecutor> &&left_executor,
                         std::unique_ptr<AbstractExecutor> &&right_executor);

  void Init() override;

  auto Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch, size_t batch_size)
      -> bool override;

  /** @return The output schema for the insert */
  auto GetOutputSchema() const -> const Schema & override { return plan_->OutputSchema(); };

 private:
  /** The NestedLoopJoin plan node to be executed. */
  const NestedLoopJoinPlanNode *plan_;

  /** @brief 为一条新的外表元组重启内表扫描（见 .cpp 中的说明）。 */
  void ResetRightScan();

  std::unique_ptr<AbstractExecutor> left_executor_;
  std::unique_ptr<AbstractExecutor> right_executor_;

  /** 当前批外表元组，以及消费到第几条。 */
  std::vector<Tuple> left_tuples_;
  std::vector<RID> left_rids_;
  size_t left_idx_{0};
  bool left_done_{false};

  /**
   * @brief 内表当前批的缓冲。
   *
   * ==== 为什么不把内表一次性物化到内存 ====
   * 物化一次、之后在内存里反复循环，看起来能把磁盘 I/O 从 O(N×M) 降到 O(N+M)，
   * 是个很自然的优化。但 p3.10-simple-join 里有一条断言：
   *
   *     right.GetInitCount() + 1 >= left.GetNextCount()
   *
   * 它要求实现必须是**教科书式的嵌套循环**：外表每推进一条元组，
   * 就把内表执行器 Init() 一次、从头重新拉取。
   *
   * 这不是为了刁难。物化内表在真实系统里是不成立的：
   *   ① 内表可能远大于内存；
   *   ② 内表子树可能带参数（相关子查询），每次的结果本就不同。
   * "重新 Init 内表"才是火山模型对内层输入的通用契约。
   * 真正的优化应该换算子——这正是 HashJoinExecutor 存在的理由。
   */
  std::vector<Tuple> right_tuples_;
  std::vector<RID> right_rids_;
  size_t right_idx_{0};
  /** 当前这条外表元组是否已经把内表拉完了。 */
  bool right_done_{false};

  /** 当前这条外表元组是否已经匹配到过内表元组 —— LEFT JOIN 补 NULL 的依据。 */
  bool matched_{false};
};

}  // namespace bustub
