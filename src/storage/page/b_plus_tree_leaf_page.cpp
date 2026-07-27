//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree_leaf_page.cpp
//
// Identification: src/storage/page/b_plus_tree_leaf_page.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <sstream>

#include "common/exception.h"
#include "common/rid.h"
#include "storage/page/b_plus_tree_leaf_page.h"

namespace bustub {

/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/

/**
 * @brief Init method after creating a new leaf page
 *
 * After creating a new leaf page from buffer pool, must call initialize method to set default values,
 * including set page type, set current size to zero, set page id/parent id, set
 * next page id and set max size.
 *
 * @param max_size Max size of the leaf node
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::Init(int max_size) {
  // ==== P2 STEP 3: Init 取代构造函数 ====
  // 页对象是「对缓冲池里 4KB 字节的一种解释」，从来不会被 new 出来，
  // 因此没有构造函数可用。刚从 NewPage() 拿到的帧内容是全零（P1 的 Reset 保证），
  // 但我们不能依赖这一点：帧也可能是刚被淘汰后复用的。所以每个字段都要显式赋值。
  SetPageType(IndexPageType::LEAF_PAGE);
  SetSize(0);
  SetMaxSize(max_size);
  // 叶子层是一条单向有序链表，这条链是范围扫描（IndexIterator）的基础：
  // 扫完一页直接顺着 next_page_id_ 走，完全不必回到根节点重新下降。
  next_page_id_ = INVALID_PAGE_ID;
  num_tombstones_ = 0;
}

/**
 * @brief Helper function for fetching tombstones of a page.
 * @return The last `NumTombs` keys with pending deletes in this page in order of recency (oldest at front).
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::GetTombstones() const -> std::vector<KeyType> {
  // 墓碑数组存的是**下标**而不是键本身：键可能很大（GenericKey<64>），
  // 而下标只要 8 字节，还能在条目移动时被精确修正。
  // 这里按存储顺序（队首 = 最老）翻译成键返回。
  std::vector<KeyType> keys;
  keys.reserve(num_tombstones_);
  for (size_t i = 0; i < num_tombstones_; i++) {
    keys.push_back(key_array_[tombstones_[i]]);
  }
  return keys;
}

/**
 * Helper methods to set/get next page id
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::GetNextPageId() const -> page_id_t { return next_page_id_; }

FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::SetNextPageId(page_id_t next_page_id) { next_page_id_ = next_page_id; }

/*
 * Helper method to find and return the key associated with input "index" (a.k.a
 * array offset)
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::KeyAt(int index) const -> KeyType { return key_array_[index]; }

/*****************************************************************************
 * 条目的物理增删（含墓碑下标维护）
 *****************************************************************************/

FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::InsertAt(int index, const KeyType &key, const ValueType &value) {
  // ==== P2 STEP 4: 有序数组的插入 ====
  // 从后往前搬，避免自我覆盖。
  for (int i = GetSize(); i > index; i--) {
    key_array_[i] = key_array_[i - 1];
    rid_array_[i] = rid_array_[i - 1];
  }
  key_array_[index] = key;
  rid_array_[index] = value;
  ChangeSizeBy(1);

  // 所有位于插入点及其之后的条目都右移了一格，指向它们的墓碑下标必须同步加一。
  // 漏掉这一步，墓碑就会「指错人」——原本被删的条目复活，而一个活条目凭空消失。
  for (size_t i = 0; i < num_tombstones_; i++) {
    if (tombstones_[i] >= static_cast<size_t>(index)) {
      tombstones_[i]++;
    }
  }
}

FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::RemoveAt(int index) {
  // 若被删的条目自己带着墓碑，先把这条墓碑记录摘掉，否则它会变成悬空下标。
  if (auto slot = FindTombstoneSlot(index); slot < num_tombstones_) {
    EraseTombstoneSlot(slot);
  }

  for (int i = index; i + 1 < GetSize(); i++) {
    key_array_[i] = key_array_[i + 1];
    rid_array_[i] = rid_array_[i + 1];
  }
  ChangeSizeBy(-1);

  // 与 InsertAt 对称：删除点之后的条目左移了一格。
  for (size_t i = 0; i < num_tombstones_; i++) {
    if (tombstones_[i] > static_cast<size_t>(index)) {
      tombstones_[i]--;
    }
  }
}

/*****************************************************************************
 * 墓碑机制
 *****************************************************************************/

FULL_INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_LEAF_PAGE_TYPE::MarkDeleted(int index) -> int {
  // ==== P2 STEP 5: 延迟删除的核心 ====
  //
  // 容量为 0 时退化成经典 B+ 树：删除立刻生效。
  // 默认模板参数 NumTombs = 0，所以 insert/delete 那几套经典测试走的就是这条路径。
  if constexpr (TombCapacity() == 0) {
    RemoveAt(index);
    return 1;
  } else {
    int physically_removed = 0;

    // 队列已满：必须先「兑现」最老的那笔待删除，才能腾出位置记新的。
    // 这是唯一会真正改变 size_ 的时刻，也因此是唯一可能触发下溢的时刻。
    if (num_tombstones_ == TombCapacity()) {
      const auto oldest = static_cast<int>(tombstones_[0]);
      EraseTombstoneSlot(0);  // 先出队，避免 RemoveAt 又去队列里找它
      RemoveAt(oldest);       // 真正物理删除，并修正其余墓碑下标
      physically_removed = 1;

      // 我们自己要标记的那个条目，如果排在被删条目后面，也跟着左移了一格。
      if (index > oldest) {
        index--;
      }
    }

    // 入队尾 —— 队首最老、队尾最新，与 GetTombstones() 的文档约定一致。
    tombstones_[num_tombstones_++] = static_cast<size_t>(index);
    return physically_removed;
  }
}

FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::ClearTombstone(int index) {
  // 「复活」：这个键被重新插入了，撤销它的待删除标记。
  // 条目本身原地不动，连 size_ 都不变——这正是墓碑设计的最大红利：
  // 删后重插退化成 O(1) 的原地更新，完全不需要动树结构。
  if (auto slot = FindTombstoneSlot(index); slot < num_tombstones_) {
    EraseTombstoneSlot(slot);
  }
}

/*****************************************************************************
 * 结构调整：分裂 / 合并 / 借用
 *****************************************************************************/

FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveHalfTo(BPlusTreeLeafPage *recipient, int start) {
  // ==== P2 STEP 6: 分裂时墓碑要按相对顺序切分 ====
  const int move_count = GetSize() - start;

  for (int i = 0; i < move_count; i++) {
    recipient->key_array_[i] = key_array_[start + i];
    recipient->rid_array_[i] = rid_array_[start + i];
  }
  recipient->SetSize(move_count);

  if constexpr (TombCapacity() > 0) {
    // 按原队列顺序逐条分流：< start 的留在本页，>= start 的迁到 recipient 并重新编号。
    // 「按原顺序」是硬性要求 —— TombstoneSplitTest 断言分裂后两侧的墓碑
    // 仍保持它们在原队列中的先后关系。
    size_t kept = 0;
    recipient->num_tombstones_ = 0;
    for (size_t i = 0; i < num_tombstones_; i++) {
      if (tombstones_[i] < static_cast<size_t>(start)) {
        tombstones_[kept++] = tombstones_[i];
      } else {
        recipient->tombstones_[recipient->num_tombstones_++] = tombstones_[i] - start;
      }
    }
    num_tombstones_ = kept;
  }

  SetSize(start);
}

FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveAllTo(BPlusTreeLeafPage *recipient) {
  // ==== P2 STEP 7: 合并时墓碑串接，超容量则从队首兑现 ====
  const int base = recipient->GetSize();

  for (int i = 0; i < GetSize(); i++) {
    recipient->key_array_[base + i] = key_array_[i];
    recipient->rid_array_[base + i] = rid_array_[i];
  }
  recipient->SetSize(base + GetSize());

  if constexpr (TombCapacity() > 0) {
    // 串接顺序：recipient 原有的在前（更老），本页的在后（更新）。
    // 本页永远是「右兄弟」，被并入左边的 recipient，所以这个顺序天然成立。
    for (size_t i = 0; i < num_tombstones_; i++) {
      recipient->tombstones_[recipient->num_tombstones_ + i] = tombstones_[i] + base;
    }
    recipient->num_tombstones_ += num_tombstones_;

    // 串接后可能超出容量。按 FIFO 语义，超出的部分是最老的那些待删除，
    // 把它们真正兑现掉（物理删除）。
    // 这一步会让 recipient 的 size_ 继续下降，但合并后的页原则上不会再下溢
    // （调用方保证 left.size + right.size 是合法的）。
    while (recipient->num_tombstones_ > TombCapacity()) {
      const auto oldest = static_cast<int>(recipient->tombstones_[0]);
      recipient->EraseTombstoneSlot(0);
      recipient->RemoveAt(oldest);
    }
  }

  SetSize(0);
  num_tombstones_ = 0;
  // 叶子链表要接上：被合并掉的页从链上摘除。
  recipient->SetNextPageId(GetNextPageId());
}

FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveFirstToEndOf(BPlusTreeLeafPage *recipient) {
  // 从右兄弟借第一条给左边的 recipient。
  const bool tombed = IsTombstoned(0);
  recipient->InsertAt(recipient->GetSize(), key_array_[0], rid_array_[0]);
  if (tombed) {
    // 墓碑身份必须跟着条目一起走，否则一条已被逻辑删除的记录会「诈尸」。
    recipient->MarkDeleted(recipient->GetSize() - 1);
  }
  RemoveAt(0);
}

FULL_INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_LEAF_PAGE_TYPE::MoveLastToFrontOf(BPlusTreeLeafPage *recipient) {
  // 从左兄弟借最后一条给右边的 recipient。
  const int last = GetSize() - 1;
  const bool tombed = IsTombstoned(last);
  recipient->InsertAt(0, key_array_[last], rid_array_[last]);
  if (tombed) {
    recipient->MarkDeleted(0);
  }
  RemoveAt(last);
}

template class BPlusTreeLeafPage<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTreeLeafPage<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTreeLeafPage<GenericKey<8>, RID, GenericComparator<8>, 3>;
template class BPlusTreeLeafPage<GenericKey<8>, RID, GenericComparator<8>, 2>;
template class BPlusTreeLeafPage<GenericKey<8>, RID, GenericComparator<8>, 1>;
template class BPlusTreeLeafPage<GenericKey<8>, RID, GenericComparator<8>, -1>;

template class BPlusTreeLeafPage<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTreeLeafPage<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTreeLeafPage<GenericKey<64>, RID, GenericComparator<64>>;
}  // namespace bustub
