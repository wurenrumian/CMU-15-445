//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// window_function_executor.cpp
//
// Identification: src/execution/window_function_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/window_function_executor.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/macros.h"
#include "execution/execution_common.h"
#include "execution/plans/aggregation_plan.h"
#include "execution/plans/window_plan.h"
#include "storage/table/tuple.h"
#include "type/value_factory.h"

namespace bustub {

namespace {

/**
 * @brief 把一个新值折叠进累加器，实现窗口聚合的增量语义。
 *
 * NULL 的处理和普通聚合完全一致（见 P3 STEP 9）：
 *   COUNT(*)                数的是**行**，NULL 也算，初值 0；
 *   COUNT(col)              只数非 NULL，初值 0；
 *   SUM / MIN / MAX         跳过 NULL，初值 NULL —— 全是 NULL 时结果就是 NULL。
 *
 * 初值取 NULL 而不是 0，正是 `SUM(x) OVER ()` 在空窗口上返回 NULL、
 * 而 `COUNT(*) OVER ()` 返回 0 的原因。
 */
void Accumulate(WindowFunctionType type, Value *acc, const Value &input) {
  switch (type) {
    case WindowFunctionType::CountStarAggregate:
      *acc = acc->Add(Value(TypeId::INTEGER, 1));
      return;
    case WindowFunctionType::CountAggregate:
      if (input.IsNull()) {
        return;
      }
      *acc = acc->IsNull() ? Value(TypeId::INTEGER, 1) : acc->Add(Value(TypeId::INTEGER, 1));
      return;
    case WindowFunctionType::SumAggregate:
      if (input.IsNull()) {
        return;
      }
      *acc = acc->IsNull() ? input : acc->Add(input);
      return;
    case WindowFunctionType::MinAggregate:
      if (input.IsNull()) {
        return;
      }
      *acc = acc->IsNull() ? input : acc->Min(input);
      return;
    case WindowFunctionType::MaxAggregate:
      if (input.IsNull()) {
        return;
      }
      *acc = acc->IsNull() ? input : acc->Max(input);
      return;
    case WindowFunctionType::Rank:
      // rank 不是聚合，它由行的序号决定，见 ComputeWindow。
      return;
  }
}

/** @return 该窗口函数类型的累加器初值。 */
auto InitialAccumulator(WindowFunctionType type) -> Value {
  if (type == WindowFunctionType::CountStarAggregate) {
    return {TypeId::INTEGER, 0};
  }
  return ValueFactory::GetNullValueByType(TypeId::INTEGER);
}

}  // namespace

/**
 * Construct a new WindowFunctionExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The window aggregation plan to be executed
 */
WindowFunctionExecutor::WindowFunctionExecutor(ExecutorContext *exec_ctx, const WindowFunctionPlanNode *plan,
                                               std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

/**
 * ==== P3 STEP 27: 窗口函数和聚合的本质区别 ====
 * 聚合把 N 行**压缩**成 M 行（M = 分组数）；
 * 窗口函数保持 N 行不变，只是**多加几列**。
 *
 *   SELECT v1, SUM(v1) OVER () FROM t        -- 6 行进，6 行出
 *   SELECT SUM(v1) FROM t                    -- 6 行进，1 行出
 *
 * 这个差别决定了实现方式：不能边扫边折叠成一个累加器就完事，
 * 必须把每一行的窗口值单独记下来。
 *
 * 两种取窗口范围的方式（本实现按 SQL 标准的默认帧）：
 *   无 ORDER BY  → 整个分区都是窗口，分区内每一行拿到**同一个值**；
 *   有 ORDER BY  → 帧是 RANGE UNBOUNDED PRECEDING AND CURRENT ROW，
 *                  即"从分区开头到当前行"的**累计值**。
 */
void WindowFunctionExecutor::Init() {
  child_executor_->Init();
  tuples_.clear();
  results_.clear();
  cursor_ = 0;

  // ---- 物化全部输入 ----
  std::vector<Tuple> batch;
  std::vector<RID> rids;
  while (child_executor_->Next(&batch, &rids, BUSTUB_BATCH_SIZE)) {
    tuples_.insert(tuples_.end(), batch.begin(), batch.end());
  }

  const auto &child_schema = child_executor_->GetOutputSchema();

  // ---- 输出行的顺序 ----
  // 只要有任何一个窗口函数带 ORDER BY，输出就按它排序 —— 这是 SQL 对窗口函数
  // 的既定行为（`... OVER (ORDER BY v1)` 的结果天然按 v1 递增呈现），
  // p3.20 里那条不带 rowsort 的断言正是靠这个才对得上。
  //
  // 多个窗口函数各有各的 ORDER BY 时，用第一个非空的那个定输出顺序；
  // 其余窗口函数在计算时各自再按自己的 ORDER BY 排一遍（见 ComputeWindow），
  // 所以取哪个都不影响每一列的正确性。
  for (const auto &[idx, wf] : plan_->window_functions_) {
    if (!wf.order_by_.empty()) {
      std::vector<SortEntry> entries;
      entries.reserve(tuples_.size());
      for (const auto &tuple : tuples_) {
        entries.emplace_back(GenerateSortKey(tuple, wf.order_by_, child_schema), tuple);
      }
      std::stable_sort(entries.begin(), entries.end(), TupleComparator{wf.order_by_});
      for (size_t i = 0; i < entries.size(); i++) {
        tuples_[i] = entries[i].second;
      }
      break;
    }
  }

  // ---- 逐个窗口函数算出它那一列 ----
  // window_functions_ 是 `列下标 → 窗口定义` 的映射，下标对应输出模式里的位置。
  std::vector<std::vector<Value>> window_columns(plan_->columns_.size());
  for (const auto &[idx, wf] : plan_->window_functions_) {
    ComputeWindow(wf, &window_columns[idx]);
  }

  // ---- 拼出输出元组 ----
  // columns_ 里窗口函数那几列是**占位符**（列下标 -1），直接求值会越界，
  // 必须换成上面算好的值；其余列正常求值。
  results_.reserve(tuples_.size());
  for (size_t row = 0; row < tuples_.size(); row++) {
    std::vector<Value> values;
    values.reserve(plan_->columns_.size());
    for (size_t col = 0; col < plan_->columns_.size(); col++) {
      if (plan_->window_functions_.count(col) > 0) {
        values.push_back(window_columns[col][row]);
      } else {
        values.push_back(plan_->columns_[col]->Evaluate(&tuples_[row], child_schema));
      }
    }
    results_.emplace_back(values, &GetOutputSchema());
  }
}

/**
 * @brief 算出一个窗口函数在每一行上的取值，结果按 tuples_ 的下标对齐。
 *
 * ==== P3 STEP 28: 分区 + 排序 + 累计，以及「同伴行」这个坑 ====
 * 三步：
 *   1. 按 PARTITION BY 的键把行分组 —— 分区之间完全独立，互不影响；
 *   2. 分区内按该函数自己的 ORDER BY 排序；
 *   3. 顺序扫一遍，累计出每一行的值。
 *
 * 第 3 步有个容易忽略的语义：**排序键相同的行（peer / 同伴行）必须拿到同一个值**。
 * SQL 标准的默认帧是 RANGE（按值）而不是 ROWS（按物理行），所以
 * `SUM(v) OVER (ORDER BY v)` 遇到两个相同的 v 时，两行都应包含彼此。
 * 逐行累加而不管同伴，在有重复排序键的数据上就会给出两个不同的值。
 *
 * RANK 是同一个道理的另一种表现：同伴行共享名次，下一个名次**跳号**
 * （1,1,3 而不是 1,1,2）。所以这两件事在实现上是同一段代码。
 */
void WindowFunctionExecutor::ComputeWindow(const WindowFunctionPlanNode::WindowFunction &wf, std::vector<Value> *out) {
  const auto &child_schema = child_executor_->GetOutputSchema();
  const size_t n = tuples_.size();
  out->assign(n, ValueFactory::GetNullValueByType(TypeId::INTEGER));

  // ---- 1. 按分区键分组 ----
  // 复用聚合算子的 AggregateKey：它的 operator== 已经正确处理了 NULL
  // （SQL 里 NULL = NULL 是 UNKNOWN，但分组时所有 NULL 必须归为同一组，
  // 见 P3 STEP 11 那个隐蔽的坑）。
  std::unordered_map<AggregateKey, std::vector<size_t>> partitions;
  std::vector<AggregateKey> partition_order;  // 保持首次出现的顺序，便于调试复现
  for (size_t i = 0; i < n; i++) {
    AggregateKey key;
    key.group_bys_.reserve(wf.partition_by_.size());
    for (const auto &expr : wf.partition_by_) {
      key.group_bys_.push_back(expr->Evaluate(&tuples_[i], child_schema));
    }
    if (partitions.find(key) == partitions.end()) {
      partition_order.push_back(key);
    }
    partitions[key].push_back(i);
  }

  for (const auto &key : partition_order) {
    std::vector<size_t> rows = partitions[key];

    // ---- 2. 分区内按本函数自己的 ORDER BY 排序 ----
    // 注意用的是 wf.order_by_ 而不是 Init() 里定输出顺序的那个 ——
    // 不同窗口函数可以有不同的 ORDER BY（p3.20 最后一条查询正是如此）。
    if (!wf.order_by_.empty()) {
      // 比较器只构造一次。放进 lambda 里每比较一次就重建一个，
      // 在 O(n log n) 次比较上是纯浪费。
      const TupleComparator comparator{wf.order_by_};
      std::stable_sort(rows.begin(), rows.end(), [&](size_t a, size_t b) {
        SortEntry ea{GenerateSortKey(tuples_[a], wf.order_by_, child_schema), tuples_[a]};
        SortEntry eb{GenerateSortKey(tuples_[b], wf.order_by_, child_schema), tuples_[b]};
        return comparator(ea, eb);
      });
    }

    // ---- 3. 累计 ----
    if (wf.order_by_.empty()) {
      // 无 ORDER BY：整个分区是一个窗口，先算总值，再回填给分区内每一行。
      Value acc = InitialAccumulator(wf.type_);
      for (const size_t row : rows) {
        Accumulate(wf.type_, &acc,
                   wf.function_ == nullptr ? ValueFactory::GetNullValueByType(TypeId::INTEGER)
                                           : wf.function_->Evaluate(&tuples_[row], child_schema));
      }
      for (const size_t row : rows) {
        (*out)[row] = acc;
      }
      continue;
    }

    // 有 ORDER BY：从分区头累计到当前行，同伴行取相同的值。
    Value acc = InitialAccumulator(wf.type_);
    size_t i = 0;
    while (i < rows.size()) {
      // 找出与 rows[i] 排序键相同的一整段同伴行 [i, j)。
      const auto key_i = GenerateSortKey(tuples_[rows[i]], wf.order_by_, child_schema);
      size_t j = i + 1;
      while (j < rows.size()) {
        const auto key_j = GenerateSortKey(tuples_[rows[j]], wf.order_by_, child_schema);
        bool same = true;
        for (size_t k = 0; k < key_i.size(); k++) {
          const bool a_null = key_i[k].IsNull();
          const bool b_null = key_j[k].IsNull();
          if (a_null || b_null) {
            if (a_null != b_null) {
              same = false;
            }
          } else if (key_i[k].CompareEquals(key_j[k]) != CmpBool::CmpTrue) {
            same = false;
          }
          if (!same) {
            break;
          }
        }
        if (!same) {
          break;
        }
        j++;
      }

      if (wf.type_ == WindowFunctionType::Rank) {
        // RANK 的定义：比我小的行数 + 1。同伴行共享名次，因此下一段的名次
        // 直接跳到 j+1 —— 这正是 1,1,3 而不是 1,1,2 的由来。
        const auto rank = static_cast<int32_t>(i + 1);
        for (size_t t = i; t < j; t++) {
          (*out)[rows[t]] = Value(TypeId::INTEGER, rank);
        }
      } else {
        // 先把这一整段同伴行**全部**折进累加器，再统一回填 ——
        // 顺序反了就退化成逐行累加，同伴行会拿到不同的值。
        for (size_t t = i; t < j; t++) {
          Accumulate(wf.type_, &acc,
                     wf.function_ == nullptr ? ValueFactory::GetNullValueByType(TypeId::INTEGER)
                                             : wf.function_->Evaluate(&tuples_[rows[t]], child_schema));
        }
        for (size_t t = i; t < j; t++) {
          (*out)[rows[t]] = acc;
        }
      }
      i = j;
    }
  }
}

/**
 * Yield the next tuple batch from the window aggregation.
 * @param[out] tuple_batch The next tuple batch produced by the window aggregation
 * @param[out] rid_batch The next tuple RID batch produced by the window aggregation
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 */
auto WindowFunctionExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                                  size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  while (cursor_ < results_.size() && tuple_batch->size() < batch_size) {
    tuple_batch->push_back(results_[cursor_++]);
    // 窗口函数的输出不对应任何物理行，填哑 RID 占位 ——
    // 上层算子按同一下标访问两个数组，长度必须一致。
    rid_batch->emplace_back();
  }

  return !tuple_batch->empty();
}
}  // namespace bustub
