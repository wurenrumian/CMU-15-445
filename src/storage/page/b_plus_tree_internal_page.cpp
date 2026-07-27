//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree_internal_page.cpp
//
// Identification: src/storage/page/b_plus_tree_internal_page.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <iostream>
#include <sstream>

#include "common/exception.h"
#include "storage/page/b_plus_tree_internal_page.h"

namespace bustub {
/*****************************************************************************
 * HELPER METHODS AND UTILITIES
 *****************************************************************************/

/**
 * @brief Init method after creating a new internal page.
 *
 * Writes the necessary header information to a newly created page,
 * including set page type, set current size, set page id, set parent id and set max page size,
 * must be called after the creation of a new page to make a valid BPlusTreeInternalPage.
 *
 * @param max_size Maximal size of the page
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::Init(int max_size) {
  SetPageType(IndexPageType::INTERNAL_PAGE);
  SetSize(0);
  SetMaxSize(max_size);
}

/**
 * @brief Helper method to get/set the key associated with input "index"(a.k.a
 * array offset).
 *
 * @param index The index of the key to get. Index must be non-zero.
 * @return Key at index
 */
INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::KeyAt(int index) const -> KeyType {
  // ==== P2 STEP 8: 内部页的第 0 个键永远是无效的 ====
  // n 个键只能分隔出 n+1 个区间，但本实现让键和孩子一一对齐存放，
  // 于是下标 0 的键槽位被浪费掉（存什么都不影响查找，因为搜索只从下标 1 开始比较）。
  // 这个「浪费一格」的设计换来的是：孩子下标 i 与键下标 i 完全对齐，
  // 分裂、合并、借用时不需要在两套下标之间做偏移换算，极大减少了 off-by-one。
  return key_array_[index];
}

/**
 * @brief Set key at the specified index.
 *
 * @param index The index of the key to set. Index must be non-zero.
 * @param key The new value for key
 */
INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::SetKeyAt(int index, const KeyType &key) { key_array_[index] = key; }

/**
 * @brief 线性查找某个孩子指针的下标。
 *
 * 只在「从父节点找到自己是第几个孩子」这一个场景用到，而内部页的扇出不大
 *（通常几十到几百），所以线性扫描完全够用，也不需要孩子指针有序。
 */
INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueIndex(const ValueType &value) const -> int {
  for (int i = 0; i < GetSize(); i++) {
    if (page_id_array_[i] == value) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief Helper method to get the value associated with input "index"(a.k.a array
 * offset)
 *
 * @param index The index of the value to get.
 * @return Value at index
 */
INDEX_TEMPLATE_ARGUMENTS
auto B_PLUS_TREE_INTERNAL_PAGE_TYPE::ValueAt(int index) const -> ValueType { return page_id_array_[index]; }

/*****************************************************************************
 * 条目的物理增删
 *****************************************************************************/

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::InsertAt(int index, const KeyType &key, const ValueType &value) {
  for (int i = GetSize(); i > index; i--) {
    key_array_[i] = key_array_[i - 1];
    page_id_array_[i] = page_id_array_[i - 1];
  }
  key_array_[index] = key;
  page_id_array_[index] = value;
  ChangeSizeBy(1);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::RemoveAt(int index) {
  for (int i = index; i + 1 < GetSize(); i++) {
    key_array_[i] = key_array_[i + 1];
    page_id_array_[i] = page_id_array_[i + 1];
  }
  ChangeSizeBy(-1);
}

/*****************************************************************************
 * 结构调整：分裂 / 合并 / 借用
 *****************************************************************************/

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveHalfTo(BPlusTreeInternalPage *recipient, int start) {
  const int move_count = GetSize() - start;
  for (int i = 0; i < move_count; i++) {
    recipient->key_array_[i] = key_array_[start + i];
    recipient->page_id_array_[i] = page_id_array_[start + i];
  }
  recipient->SetSize(move_count);
  SetSize(start);
  // 注意：recipient 的 key_array_[0] 此时是原本的 key_array_[start]，它会被调用方
  // 「上推」到父节点当分隔键，而 recipient 自己的第 0 个键随即变成无效槽位。
  // 这正是 B+ 树内部节点分裂与叶子分裂的根本区别：叶子分裂是「复制」中间键，
  // 内部节点分裂是「上移」中间键。
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveAllTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key) {
  const int base = recipient->GetSize();
  for (int i = 0; i < GetSize(); i++) {
    recipient->key_array_[base + i] = key_array_[i];
    recipient->page_id_array_[base + i] = page_id_array_[i];
  }
  // 本页的第 0 个键是无效槽位。它在合并后的页里不再位于开头，必须用父节点中
  // 分隔这两页的 middle_key 填上，否则从 recipient 下降时会漏掉一整棵子树。
  recipient->key_array_[base] = middle_key;
  recipient->SetSize(base + GetSize());
  SetSize(0);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key) {
  // 从右兄弟借第一个孩子。同样地，被借走的孩子在 recipient 里需要一个真实的分隔键，
  // 而这个键正是父节点当前的 middle_key。
  recipient->InsertAt(recipient->GetSize(), middle_key, page_id_array_[0]);
  RemoveAt(0);
}

INDEX_TEMPLATE_ARGUMENTS
void B_PLUS_TREE_INTERNAL_PAGE_TYPE::MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key) {
  // 从左兄弟借最后一个孩子插到 recipient 开头。
  // recipient 原来的第 0 个键是无效槽位，现在它前面多了一个孩子，
  // 于是它必须变成真实的分隔键 —— 用父节点的 middle_key 填上。
  recipient->SetKeyAt(0, middle_key);
  recipient->InsertAt(0, key_array_[GetSize() - 1], page_id_array_[GetSize() - 1]);
  RemoveAt(GetSize() - 1);
}

// valuetype for internalNode should be page id_t
template class BPlusTreeInternalPage<GenericKey<4>, page_id_t, GenericComparator<4>>;
template class BPlusTreeInternalPage<GenericKey<8>, page_id_t, GenericComparator<8>>;
template class BPlusTreeInternalPage<GenericKey<16>, page_id_t, GenericComparator<16>>;
template class BPlusTreeInternalPage<GenericKey<32>, page_id_t, GenericComparator<32>>;
template class BPlusTreeInternalPage<GenericKey<64>, page_id_t, GenericComparator<64>>;
}  // namespace bustub
