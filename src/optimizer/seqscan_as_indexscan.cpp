//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// seqscan_as_indexscan.cpp
//
// Identification: src/optimizer/seqscan_as_indexscan.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <memory>
#include <utility>
#include <vector>

#include "execution/expressions/column_value_expression.h"
#include "execution/expressions/comparison_expression.h"
#include "execution/expressions/constant_value_expression.h"
#include "execution/expressions/logic_expression.h"
#include "execution/plans/index_scan_plan.h"
#include "execution/plans/seq_scan_plan.h"
#include "optimizer/optimizer.h"

namespace bustub {

namespace {

/**
 * @brief 递归拆解形如 `a = 1 OR a = 2 OR a = 3` 的谓词。
 *
 * ==== P3 STEP 8: 什么样的谓词才能用索引 ====
 * 只有「同一列 = 常量」的等值条件（以及它们的 OR 并集）能被 B+ 树索引直接定位。
 * 判定条件：
 *   - 比较类型必须是 `=`。范围条件（`>`、`<`）需要区间扫描，本实现不支持。
 *   - 一侧是列引用、另一侧是常量。两侧都是列（`a = b`）无法用作查找键。
 *   - OR 的每一支都必须指向**同一列**，否则一次索引扫描无法同时覆盖。
 *
 * @param expr         待分析的谓词。
 * @param column_idx   出参/累积参数：所有分支共同引用的列下标。首次匹配时写入。
 * @param pred_keys    出参：抽取出的常量查找键，按出现顺序追加。
 * @return 整个表达式是否可以完全转换为索引点查。
 */
auto ExtractEqualityKeys(const AbstractExpressionRef &expr, std::optional<uint32_t> *column_idx,
                         std::vector<AbstractExpressionRef> *pred_keys) -> bool {
  // ---- 情况 1：OR ——两边都必须可转换 ----
  if (const auto *logic = dynamic_cast<const LogicExpression *>(expr.get()); logic != nullptr) {
    if (logic->logic_type_ != LogicType::Or) {
      return false;  // AND 的语义是取交集，不能简单地并起来做多次点查。
    }
    return ExtractEqualityKeys(logic->GetChildAt(0), column_idx, pred_keys) &&
           ExtractEqualityKeys(logic->GetChildAt(1), column_idx, pred_keys);
  }

  // ---- 情况 2：单个比较 ----
  const auto *cmp = dynamic_cast<const ComparisonExpression *>(expr.get());
  if (cmp == nullptr || cmp->comp_type_ != ComparisonType::Equal) {
    return false;
  }

  // 允许 `col = const` 和 `const = col` 两种写法。
  const auto *left_col = dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(0).get());
  const auto *right_col = dynamic_cast<const ColumnValueExpression *>(cmp->GetChildAt(1).get());
  const auto *left_const = dynamic_cast<const ConstantValueExpression *>(cmp->GetChildAt(0).get());
  const auto *right_const = dynamic_cast<const ConstantValueExpression *>(cmp->GetChildAt(1).get());

  const ColumnValueExpression *column = nullptr;
  AbstractExpressionRef constant = nullptr;
  if (left_col != nullptr && right_const != nullptr) {
    column = left_col;
    constant = cmp->GetChildAt(1);
  } else if (right_col != nullptr && left_const != nullptr) {
    column = right_col;
    constant = cmp->GetChildAt(0);
  } else {
    return false;  // `a = b` 或 `1 = 2`，都用不上索引。
  }

  // 所有分支必须指向同一列。
  if (column_idx->has_value() && **column_idx != column->GetColIdx()) {
    return false;
  }
  *column_idx = column->GetColIdx();
  pred_keys->push_back(std::move(constant));
  return true;
}

}  // namespace

/**
 * @brief Optimizes seq scan as index scan if there's an index on a table
 */
auto Optimizer::OptimizeSeqScanAsIndexScan(const bustub::AbstractPlanNodeRef &plan) -> AbstractPlanNodeRef {
  // 优化规则统一是「先递归优化孩子，再看自己能不能改写」的自底向上模式。
  std::vector<AbstractPlanNodeRef> children;
  children.reserve(plan->GetChildren().size());
  for (const auto &child : plan->GetChildren()) {
    children.emplace_back(OptimizeSeqScanAsIndexScan(child));
  }
  auto optimized_plan = plan->CloneWithChildren(std::move(children));

  if (optimized_plan->GetType() != PlanType::SeqScan) {
    return optimized_plan;
  }

  const auto &seq_scan = dynamic_cast<const SeqScanPlanNode &>(*optimized_plan);
  // 谓词下推规则已经先跑过了，所以 WHERE 条件此时挂在 SeqScan 的
  // filter_predicate_ 上，而不是一个独立的 Filter 算子里。
  if (seq_scan.filter_predicate_ == nullptr) {
    return optimized_plan;
  }

  std::optional<uint32_t> column_idx;
  std::vector<AbstractExpressionRef> pred_keys;
  if (!ExtractEqualityKeys(seq_scan.filter_predicate_, &column_idx, &pred_keys) || !column_idx.has_value()) {
    return optimized_plan;
  }

  // 该列上必须真的存在索引，否则改写没有意义（反而更慢）。
  auto index = MatchIndex(seq_scan.table_name_, *column_idx);
  if (!index.has_value()) {
    return optimized_plan;
  }
  auto [index_oid, index_name] = *index;

  // 改写完成：全表扫描 O(n) → 每个键一次 B+ 树查找 O(log n)。
  // 注意 filter_predicate_ 仍然传下去：索引只能保证「键相等」，
  // 若原谓词还有别的部分（本实现里没有，但保留更稳妥），仍需在回表后复核。
  return std::make_shared<IndexScanPlanNode>(seq_scan.output_schema_, seq_scan.GetTableOid(), index_oid,
                                             seq_scan.filter_predicate_, std::move(pred_keys));
}

}  // namespace bustub
