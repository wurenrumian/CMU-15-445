//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree_internal_page.h
//
// Identification: src/include/storage/page/b_plus_tree_internal_page.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <queue>
#include <string>

#include "storage/page/b_plus_tree_page.h"

namespace bustub {

#define B_PLUS_TREE_INTERNAL_PAGE_TYPE BPlusTreeInternalPage<KeyType, ValueType, KeyComparator>
#define INTERNAL_PAGE_HEADER_SIZE 12
#define INTERNAL_PAGE_SLOT_CNT \
  ((BUSTUB_PAGE_SIZE - INTERNAL_PAGE_HEADER_SIZE) / ((int)(sizeof(KeyType) + sizeof(ValueType))))  // NOLINT

/**
 * Store `n` indexed keys and `n + 1` child pointers (page_id) within internal page.
 * Pointer PAGE_ID(i) points to a subtree in which all keys K satisfy:
 * K(i) <= K < K(i+1).
 * NOTE: Since the number of keys does not equal to number of child pointers,
 * the first key in key_array_ always remains invalid. That is to say, any search / lookup
 * should ignore the first key.
 *
 * Internal page format (keys are stored in increasing order):
 *  ---------
 * | HEADER |
 *  ---------
 *  ------------------------------------------
 * | KEY(1)(INVALID) | KEY(2) | ... | KEY(n) |
 *  ------------------------------------------
 *  ---------------------------------------------
 * | PAGE_ID(1) | PAGE_ID(2) | ... | PAGE_ID(n) |
 *  ---------------------------------------------
 */
INDEX_TEMPLATE_ARGUMENTS
class BPlusTreeInternalPage : public BPlusTreePage {
 public:
  // Delete all constructor / destructor to ensure memory safety
  BPlusTreeInternalPage() = delete;
  BPlusTreeInternalPage(const BPlusTreeInternalPage &other) = delete;

  void Init(int max_size = INTERNAL_PAGE_SLOT_CNT);

  auto KeyAt(int index) const -> KeyType;

  void SetKeyAt(int index, const KeyType &key);

  /**
   * @param value The value to search for
   * @return The index that corresponds to the specified value
   */
  auto ValueIndex(const ValueType &value) const -> int;

  auto ValueAt(int index) const -> ValueType;

  /* ==================== 以下为本实现新增的辅助接口 ==================== */

  /** @brief 覆盖第 index 个孩子指针。 */
  void SetValueAt(int index, const ValueType &value) { page_id_array_[index] = value; }

  /**
   * @brief 二分查找：给定 key，返回应当下降到的孩子的下标。
   *
   * 内部页的语义是 `PAGE_ID(i)` 指向的子树中所有键 K 满足 `KEY(i) <= K < KEY(i+1)`，
   * 且 `KEY(0)` 恒为无效值（因为 n 个键要分隔 n+1 个孩子，最左边那个没有下界）。
   * 所以要找的是「最后一个满足 KeyAt(i) <= key 的 i」，下标 0 是兜底答案。
   */
  auto ChildIndexFor(const KeyType &key, const KeyComparator &comparator) const -> int {
    // 在 [1, size) 上二分找第一个 KeyAt(mid) > key，其前一个就是答案。
    int lo = 1;
    int hi = GetSize();
    while (lo < hi) {
      int mid = lo + (hi - lo) / 2;
      if (comparator(KeyAt(mid), key) <= 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo - 1;
  }

  /** @brief 在下标 index 处插入一对 (key, child)，其后整体右移。 */
  void InsertAt(int index, const KeyType &key, const ValueType &value);

  /** @brief 删除下标 index 处的 (key, child)，其后整体左移。 */
  void RemoveAt(int index);

  /** @brief 分裂：把 [start, GetSize()) 的条目搬到 recipient。 */
  void MoveHalfTo(BPlusTreeInternalPage *recipient, int start);

  /**
   * @brief 合并：把本页所有条目追加到 recipient 末尾。
   *
   * @param middle_key 父节点中分隔这两页的键。内部页的第 0 个键是无效的，
   *        合并时必须用父节点的分隔键把它「补齐」，否则搜索路径会断掉。
   */
  void MoveAllTo(BPlusTreeInternalPage *recipient, const KeyType &middle_key);

  /** @brief 借用：把本页第一个条目搬到 recipient 末尾（middle_key 同上，用于补齐）。 */
  void MoveFirstToEndOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key);

  /** @brief 借用：把本页最后一个条目搬到 recipient 开头。 */
  void MoveLastToFrontOf(BPlusTreeInternalPage *recipient, const KeyType &middle_key);

  /**
   * @brief For test only, return a string representing all keys in
   * this internal page, formatted as "(key1,key2,key3,...)"
   *
   * @return The string representation of all keys in the current internal page
   */
  auto ToString() const -> std::string {
    std::string kstr = "(";
    bool first = true;

    // First key of internal page is always invalid
    for (int i = 1; i < GetSize(); i++) {
      KeyType key = KeyAt(i);
      if (first) {
        first = false;
      } else {
        kstr.append(",");
      }

      kstr.append(std::to_string(key.ToString()));
    }
    kstr.append(")");

    return kstr;
  }

 private:
  // Array members for page data.
  KeyType key_array_[INTERNAL_PAGE_SLOT_CNT];
  ValueType page_id_array_[INTERNAL_PAGE_SLOT_CNT];
  // (Spring 2025) Feel free to add more fields and helper functions below if needed
};

}  // namespace bustub
