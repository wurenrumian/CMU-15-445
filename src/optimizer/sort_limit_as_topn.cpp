//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// sort_limit_as_topn.cpp
//
// Identification: src/optimizer/sort_limit_as_topn.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <utility>
#include <vector>

#include "execution/plans/limit_plan.h"
#include "execution/plans/sort_plan.h"
#include "execution/plans/topn_plan.h"
#include "optimizer/optimizer.h"

namespace bustub {

/**
 * @brief optimize sort + limit as top N
 */
auto Optimizer::OptimizeSortLimitAsTopN(const AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeSortLimitAsTopN(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  // ==== P3 STEP 25: 识别 Limit(Sort(...)) 这个模式 ====
  // `ORDER BY x LIMIT 10` 会被规划成 Limit 套在 Sort 上。
  // 照字面执行的话，Sort 会把**全部**数据排好序，Limit 再扔掉 99.99%。
  // 合并成一个 TopN 算子后：
  //   时间 O(M log M) → O(M log N)，内存 O(M) → O(N)。
  //
  // 这是「算子合并（operator fusion）」类优化规则的典型例子：
  // 两个算子各自都没问题，但组合在一起时存在明显更优的单一实现。
  if (optimized_plan->GetType() != PlanType::Limit) {
    return optimized_plan;
  }
  const auto &limit = dynamic_cast<const LimitPlanNode &>(*optimized_plan);

  BUSTUB_ENSURE(optimized_plan->children_.size() == 1, "Limit should have exactly one child");
  const auto &child = optimized_plan->children_[0];
  if (child->GetType() != PlanType::Sort) {
    return optimized_plan;
  }
  const auto &sort = dynamic_cast<const SortPlanNode &>(*child);

  BUSTUB_ENSURE(child->children_.size() == 1, "Sort should have exactly one child");
  return std::make_shared<TopNPlanNode>(limit.output_schema_, child->children_[0], sort.GetOrderBy(), limit.GetLimit());
}

}  // namespace bustub
