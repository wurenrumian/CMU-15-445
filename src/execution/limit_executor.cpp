//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// limit_executor.cpp
//
// Identification: src/execution/limit_executor.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/executors/limit_executor.h"
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>
#include "common/macros.h"

namespace bustub {

/**
 * Construct a new LimitExecutor instance.
 * @param exec_ctx The executor context
 * @param plan The limit plan to be executed
 * @param child_executor The child executor from which limited tuples are pulled
 */
LimitExecutor::LimitExecutor(ExecutorContext *exec_ctx, const LimitPlanNode *plan,
                             std::unique_ptr<AbstractExecutor> &&child_executor)
    : AbstractExecutor(exec_ctx), plan_(plan), child_executor_(std::move(child_executor)) {}

/** Initialize the limit */
void LimitExecutor::Init() {
  child_executor_->Init();
  // 已经吐出去的行数。Init 里必须重置，否则算子被复用时会立刻"以为自己满了"。
  emitted_ = 0;
}

/**
 * Yield the next tuple batch from the limit.
 * @param[out] tuple_batch The next tuple batch produced by the limit
 * @param[out] rid_batch The next tuple RID batch produced by the limit
 * @param batch_size The number of tuples to be included in the batch (default: BUSTUB_BATCH_SIZE)
 * @return `true` if a tuple was produced, `false` if there are no more tuples
 */
auto LimitExecutor::Next(std::vector<bustub::Tuple> *tuple_batch, std::vector<bustub::RID> *rid_batch,
                         size_t batch_size) -> bool {
  tuple_batch->clear();
  rid_batch->clear();

  const size_t limit = plan_->GetLimit();
  if (emitted_ >= limit) {
    // ==== P3 STEP 3: Limit 的价值在于「提前停手」====
    // 这里直接返回 false 而**不去问子算子**，是火山模型最优雅的地方：
    // 拉取是按需的，上层一旦够了，下层的全表扫描/排序就自然停在半路，
    // 根本不会去算那些永远用不上的元组。
    return false;
  }

  // 只向子算子要还差的那么多条，别多要。
  const size_t want = std::min(batch_size, limit - emitted_);
  if (!child_executor_->Next(tuple_batch, rid_batch, want)) {
    return false;
  }

  // 子算子可能给多了（它只把 batch_size 当作上限的建议值），这里做一次硬截断。
  if (tuple_batch->size() > limit - emitted_) {
    tuple_batch->resize(limit - emitted_);
    rid_batch->resize(tuple_batch->size());
  }

  emitted_ += tuple_batch->size();
  return !tuple_batch->empty();
}

}  // namespace bustub
