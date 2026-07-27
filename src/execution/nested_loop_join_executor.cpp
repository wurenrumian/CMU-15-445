//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// nested_loop_join_executor.cpp
//
// Identification: src/execution/nested_loop_join_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/nested_loop_join_executor.h"
#include <memory>
#include <utility>
#include <vector>
#include "binder/table_ref/bound_join_ref.h"
#include "common/exception.h"
#include "common/macros.h"
#include "type/value_factory.h"

namespace bustub {

/**
 * Construct a new NestedLoopJoinExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The nested loop join plan to be executed
 * @param left_executor The child executor that produces tuple for the left side of join
 * @param right_executor The child executor that produces tuple for the right side of join
 */
NestedLoopJoinExecutor::NestedLoopJoinExecutor(ExecutorContext *exec_ctx, const NestedLoopJoinPlanNode *plan,
                                               std::unique_ptr<AbstractExecutor> &&left_executor,
                                               std::unique_ptr<AbstractExecutor> &&right_executor)
    : AbstractExecutor(exec_ctx),
      plan_(plan),
      left_executor_(std::move(left_executor)),
      right_executor_(std::move(right_executor)) {
  if (plan->GetJoinType() != JoinType::LEFT && plan->GetJoinType() != JoinType::INNER) {
    // Note for Spring 2025: You ONLY need to implement left join and inner join.
    throw bustub::NotImplementedException(fmt::format("join type {} not supported", plan->GetJoinType()));
  }
}

/** Initialize the join */
void NestedLoopJoinExecutor::Init() {
  left_executor_->Init();

  left_tuples_.clear();
  left_rids_.clear();
  left_idx_ = 0;
  left_done_ = false;

  // 内表的 Init 推迟到「拿到第一条外表元组」时再做，
  // 由 ResetRightScan() 统一负责。
  right_tuples_.clear();
  right_rids_.clear();
  right_idx_ = 0;
  right_done_ = true;  // 还没有外表元组，视为"已扫完"
  matched_ = false;
}

/**
 * @brief 为一条新的外表元组重启内表扫描。
 *
 * ==== P3 STEP 12: 外层每推进一条，内层就重来一遍 ====
 * 这就是嵌套循环连接的定义。Init() 让内表执行器回到初始状态，
 * 之后的 Next() 会从头产出全部内表元组。
 */
void NestedLoopJoinExecutor::ResetRightScan() {
  right_executor_->Init();
  right_tuples_.clear();
  right_rids_.clear();
  right_idx_ = 0;
  right_done_ = false;
  matched_ = false;
}

/**
 * Yield the next tuple batch from the join.
 * @param[out] tuple_batch The next tuple batch produced by the join
 * @param[out] rid_batch The next tuple RID batch produced by the join
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 */
auto NestedLoopJoinExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                                  size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  const auto &left_schema = left_executor_->GetOutputSchema();
  const auto &right_schema = right_executor_->GetOutputSchema();
  const auto &predicate = plan_->Predicate();

  // 把「左元组 + 右元组」拼成一条输出元组。右侧传 nullptr 表示补 NULL（LEFT JOIN 用）。
  auto emit = [&](const Tuple &left, const Tuple *right) {
    std::vector<Value> values;
    values.reserve(left_schema.GetColumnCount() + right_schema.GetColumnCount());
    for (uint32_t i = 0; i < left_schema.GetColumnCount(); i++) {
      values.push_back(left.GetValue(&left_schema, i));
    }
    for (uint32_t i = 0; i < right_schema.GetColumnCount(); i++) {
      values.push_back(right == nullptr ? ValueFactory::GetNullValueByType(right_schema.GetColumn(i).GetType())
                                        : right->GetValue(&right_schema, i));
    }
    tuple_batch->emplace_back(values, &GetOutputSchema());
    rid_batch->emplace_back();  // 连接结果不对应任何物理行。
  };

  while (tuple_batch->size() < batch_size) {
    // ---- 情况 A：需要新的一批外表元组 ----
    if (left_idx_ >= left_tuples_.size()) {
      if (left_done_) {
        break;
      }
      if (!left_executor_->Next(&left_tuples_, &left_rids_, batch_size)) {
        left_done_ = true;
        break;
      }
      left_idx_ = 0;
      ResetRightScan();  // 为这批的第一条外表元组重启内表
      continue;
    }

    const auto &left_tuple = left_tuples_[left_idx_];

    // ---- 情况 B：内表缓冲里还有没处理的元组 ----
    // right_idx_ / right_done_ 都是成员变量：一批输出可能在内层循环中途就填满，
    // 下次 Next() 必须从**同一条外表元组、同一个内表位置**继续，不能从头再来。
    if (right_idx_ < right_tuples_.size()) {
      const auto &right_tuple = right_tuples_[right_idx_++];
      bool ok = true;
      if (predicate != nullptr) {
        auto value = predicate->EvaluateJoin(&left_tuple, left_schema, &right_tuple, right_schema);
        ok = !value.IsNull() && value.GetAs<bool>();
      }
      if (ok) {
        matched_ = true;
        emit(left_tuple, &right_tuple);
      }
      continue;
    }

    // ---- 情况 C：内表缓冲空了，再向内表要一批 ----
    if (!right_done_) {
      if (right_executor_->Next(&right_tuples_, &right_rids_, batch_size)) {
        right_idx_ = 0;
      } else {
        right_done_ = true;
      }
      continue;
    }

    // ---- 情况 D：这条外表元组已经和整个内表比对完了 ----
    // ==== P3 STEP 13: LEFT JOIN 的补空行 ====
    // 内表扫完一圈都没匹配上，LEFT JOIN 仍必须输出这条外表元组，
    // 右侧各列填 NULL。matched_ 就是为记录"这一圈有没有匹配过"而存在的。
    // INNER JOIN 则直接丢弃。
    if (!matched_ && plan_->GetJoinType() == JoinType::LEFT) {
      emit(left_tuple, nullptr);
    }
    ++left_idx_;
    if (left_idx_ < left_tuples_.size()) {
      ResetRightScan();  // 换下一条外表元组，内表从头再扫一遍
    }
  }

  return !tuple_batch->empty();
}

}  // namespace bustub
