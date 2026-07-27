//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// nlj_as_hash_join.cpp
//
// Identification: src/optimizer/nlj_as_hash_join.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>
#include "catalog/column.h"
#include "catalog/schema.h"
#include "common/exception.h"
#include "common/macros.h"
#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/plans/abstract_plan.h"
#include "execution/plans/filter_plan.h"
#include "execution/plans/hash_join_plan.h"
#include "execution/plans/nested_loop_join_plan.h"
#include "execution/plans/projection_plan.h"
#include "optimizer/optimizer.h"
#include "type/type_id.h"

namespace bustub {

/**
 * @brief optimize nested loop join into hash join.
 * In the starter code, we will check NLJs with exactly one equal condition. You can further support optimizing joins
 * with multiple eq conditions.
 */
namespace {

/**
 * @brief 递归拆解连接条件，抽出所有等值连接键。
 *
 * ==== P3 STEP 16: 什么样的连接条件能用哈希连接 ====
 * 哈希连接的原理是「两侧算出同一个键，再看键是否相等」，因此只能处理
 * **等值条件的合取（AND）**：`l.a = r.x AND l.b = r.y`。
 *   - `OR` 不行：一条元组可能因为不同分支匹配，无法归约成单一的键。
 *   - `<`、`>` 不行：哈希表只支持等值查找。
 *   - `l.a = l.b`（同一侧两列）不行：它是过滤条件，不是连接条件。
 *
 * 表达式里用 `tuple_idx` 区分来自哪一侧：0 = 左表，1 = 右表。
 * 条件写成 `r.x = l.a` 时要把两侧对调，保证左键始终来自左表。
 *
 * @param expr       待分析的连接条件。
 * @param left_keys  出参：左表侧的键表达式。
 * @param right_keys 出参：右表侧的键表达式。
 * @return 整个条件是否可以完全转换为等值连接键。
 */
auto ExtractJoinKeys(const AbstractExpressionRef &expr, std::vector<AbstractExpressionRef> *left_keys,
                     std::vector<AbstractExpressionRef> *right_keys) -> bool {
  // AND：两个子条件都必须能转换，键列表合并起来。
  if (const auto *logic = dynamic_cast<const LogicExpression *>(expr.get()); logic != nullptr) {
    if (logic->logic_type_ != LogicType::And) {
      return false;
    }
    return ExtractJoinKeys(logic->GetChildAt(0), left_keys, right_keys) &&
           ExtractJoinKeys(logic->GetChildAt(1), left_keys, right_keys);
  }

  const auto *cmp = dynamic_cast<const ComparisonExpression *>(expr.get());
  if (cmp == nullptr || cmp->comp_type_ != ComparisonType::Equal) {
    return false;
  }

  const auto *lhs = dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(0).get());
  const auto *rhs = dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(1).get());
  if (lhs == nullptr || rhs == nullptr) {
    return false;  // 至少有一侧不是列引用（例如 `l.a = 5`），那是过滤而非连接。
  }

  // 两侧必须分别来自不同的输入，否则不是连接条件。
  if (lhs->GetTupleIdx() == 0 && rhs->GetTupleIdx() == 1) {
    left_keys->push_back(cmp->GetChildAt(0));
    right_keys->push_back(cmp->GetChildAt(1));
    return true;
  }
  if (lhs->GetTupleIdx() == 1 && rhs->GetTupleIdx() == 0) {
    // 写成 `r.x = l.a`，对调回来。
    left_keys->push_back(cmp->GetChildAt(1));
    right_keys->push_back(cmp->GetChildAt(0));
    return true;
  }
  return false;
}

}  // namespace

auto Optimizer::OptimizeNLJAsHashJoin(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeNLJAsHashJoin(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() != PlanType::NestedLoopJoin) {
    return optimized_plan;
  }

  const auto &nlj = dynamic_cast<const NestedLoopJoinPlanNode &>(*optimized_plan);
  if (nlj.Predicate() == nullptr) {
    return optimized_plan;  // 笛卡尔积，没有键可哈希。
  }

  std::vector<AbstractExpressionRef> left_keys;
  std::vector<AbstractExpressionRef> right_keys;
  if (!ExtractJoinKeys(nlj.Predicate(), &left_keys, &right_keys) || left_keys.empty()) {
    return optimized_plan;
  }

  // 改写成功：O(N × M) 的嵌套循环 → O(N + M) 的哈希连接。
  return std::make_shared<HashJoinPlanNode>(nlj.output_schema_, nlj.GetLeftPlan(), nlj.GetRightPlan(),
                                            std::move(left_keys), std::move(right_keys), nlj.GetJoinType());
}

}  // namespace bustub
