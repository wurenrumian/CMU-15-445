//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree_leaf_page.h
//
// Identification: src/include/storage/page/b_plus_tree_leaf_page.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "storage/page/b_plus_tree_page.h"

namespace bustub {

#define B_PLUS_TREE_LEAF_PAGE_TYPE BPlusTreeLeafPage<KeyType, ValueType, KeyComparator, NumTombs>
#define LEAF_PAGE_HEADER_SIZE 16
#define LEAF_PAGE_DEFAULT_TOMB_CNT 0
#define LEAF_PAGE_TOMB_CNT ((NumTombs < 0) ? LEAF_PAGE_DEFAULT_TOMB_CNT : NumTombs)
#define LEAF_PAGE_SLOT_CNT                                                                               \
  ((BUSTUB_PAGE_SIZE - LEAF_PAGE_HEADER_SIZE - sizeof(size_t) - (LEAF_PAGE_TOMB_CNT * sizeof(size_t))) / \
   (sizeof(KeyType) + sizeof(ValueType)))  // NOLINT

/**
 * Store indexed key and record id(record id = page id combined with slot id,
 * see include/common/rid.h for detailed implementation) together within leaf
 * page. Only support unique key.
 *
 * Leaf pages also contain a fixed buffer of "tombstone" indexes for entries
 * that have been deleted.
 *
 * Leaf page format (keys are stored in order, tomb order is up to you):
 *  --------------------
 * | HEADER | TOMB_SIZE | (where TOMB_SIZE is num_tombstones_)
 *  --------------------
 *  -----------------------------------
 * | TOMB(0) | TOMB(1) | ... | TOMB(k) |
 *  -----------------------------------
 *  ---------------------------------
 * | KEY(1) | KEY(2) | ... | KEY(n) |
 *  ---------------------------------
 *  ---------------------------------
 * | RID(1) | RID(2) | ... | RID(n) |
 *  ---------------------------------
 *
 *  Header format (size in byte, 16 bytes in total):
 *  -----------------------------------------------
 * | PageType (4) | CurrentSize (4) | MaxSize (4) |
 *  -----------------------------------------------
 *  -----------------
 * | NextPageId (4) |
 *  -----------------
 */
FULL_INDEX_TEMPLATE_ARGUMENTS_DEFN
class BPlusTreeLeafPage : public BPlusTreePage {
 public:
  // Delete all constructor / destructor to ensure memory safety
  BPlusTreeLeafPage() = delete;
  BPlusTreeLeafPage(const BPlusTreeLeafPage &other) = delete;

  void Init(int max_size = LEAF_PAGE_SLOT_CNT);

  auto GetTombstones() const -> std::vector<KeyType>;

  // Helper methods
  auto GetNextPageId() const -> page_id_t;
  void SetNextPageId(page_id_t next_page_id);
  auto KeyAt(int index) const -> KeyType;

  /* ==================== 以下为本实现新增的辅助接口 ==================== */

  /** @brief 取第 index 个条目的值（RID）。 */
  auto ValueAt(int index) const -> ValueType { return rid_array_[index]; }

  /**
   * @brief 直接返回页内数据的常量引用（不拷贝）。
   *
   * IndexIterator 的 `operator*` 要求返回 `pair<const K&, const V&>`，
   * 因此必须能拿到页内实际存储位置的引用。只要迭代器还持有该页的
   * ReadPageGuard，这些引用就一直有效。
   */
  auto KeyRefAt(int index) const -> const KeyType & { return key_array_[index]; }
  auto ValueRefAt(int index) const -> const ValueType & { return rid_array_[index]; }

  /** @brief 覆盖第 index 个条目的值。用于「复活墓碑」时更新 RID。 */
  void SetValueAt(int index, const ValueType &value) { rid_array_[index] = value; }

  /** @brief 本页墓碑缓冲区的容量（编译期常量，等于模板参数 NumTombs，负数视为 0）。 */
  static constexpr auto TombCapacity() -> size_t { return LEAF_PAGE_TOMB_CNT; }

  /** @brief 当前待生效的删除条数。 */
  auto NumTombstones() const -> size_t { return num_tombstones_; }

  /** @brief 第 index 个条目是否已被逻辑删除。 */
  auto IsTombstoned(int index) const -> bool { return FindTombstoneSlot(index) < num_tombstones_; }

  /** @brief 本页当前存活（未被墓碑标记）的条目数。 */
  auto NumLiveEntries() const -> int { return GetSize() - static_cast<int>(num_tombstones_); }

  /**
   * @brief 在下标 index 处插入一个条目，其后的条目整体右移。
   *
   * 会同步把 >= index 的墓碑下标加一，保证墓碑始终指向它原本标记的那个条目。
   */
  void InsertAt(int index, const KeyType &key, const ValueType &value);

  /**
   * @brief 物理删除下标 index 处的条目，其后的条目整体左移，size_ 减一。
   *
   * 若该条目本身带墓碑，会先把它从墓碑队列中摘掉；
   * 其余 > index 的墓碑下标同步减一。
   */
  void RemoveAt(int index);

  /**
   * @brief 逻辑删除下标 index 处的条目（打墓碑）。
   *
   * 这是 P2 墓碑机制的核心。行为：
   *   - 墓碑容量为 0（NumTombs <= 0）：退化成经典 B+ 树，直接物理删除。
   *   - 队列未满：仅入队，size_ 不变，因此**不会造成下溢**。
   *   - 队列已满：先把队首（最老的待删条目）真正物理删除，再把本条目入队。
   *
   * @return 本次物理删除的条目数（0 或 1）。调用方据此决定是否需要检查下溢。
   */
  auto MarkDeleted(int index) -> int;

  /** @brief 把下标 index 处的墓碑撤销（键被重新插入时「复活」它）。 */
  void ClearTombstone(int index);

  /**
   * @brief 分裂：把 [start, GetSize()) 区间的条目搬到 recipient。
   *
   * 墓碑随条目一起迁移，并保持原有的先后（recency）顺序——
   * TombstoneSplitTest 断言了这一点。
   */
  void MoveHalfTo(BPlusTreeLeafPage *recipient, int start);

  /**
   * @brief 合并：把本页所有条目追加到 recipient 末尾，本页清空。
   *
   * 墓碑队列串接为 `recipient 的 ++ 本页的`（左边的更老）；
   * 若超出容量，则从队首开始逐个真正物理删除，直到长度合法。
   */
  void MoveAllTo(BPlusTreeLeafPage *recipient);

  /** @brief 借用：把本页第一个条目（连同墓碑身份）搬到 recipient 末尾。 */
  void MoveFirstToEndOf(BPlusTreeLeafPage *recipient);

  /** @brief 借用：把本页最后一个条目（连同墓碑身份）搬到 recipient 开头。 */
  void MoveLastToFrontOf(BPlusTreeLeafPage *recipient);

  /**
   * @brief for test only return a string representing all keys in
   * this leaf page formatted as "(tombkey1, tombkey2, ...|key1,key2,key3,...)"
   *
   * @return std::string
   */
  auto ToString() const -> std::string {
    std::string kstr = "(";
    bool first = true;

    auto tombs = GetTombstones();
    for (size_t i = 0; i < tombs.size(); i++) {
      kstr.append(std::to_string(tombs[i].ToString()));
      if ((i + 1) < tombs.size()) {
        kstr.append(",");
      }
    }

    kstr.append("|");

    for (int i = 0; i < GetSize(); i++) {
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
  /**
   * @brief 在墓碑队列中找到指向 entry_index 的槽位；找不到返回 num_tombstones_。
   *
   * 线性扫描没问题：队列长度是编译期常量 NumTombs，实测最大只有个位数。
   */
  auto FindTombstoneSlot(int entry_index) const -> size_t {
    for (size_t i = 0; i < num_tombstones_; i++) {
      if (tombstones_[i] == static_cast<size_t>(entry_index)) {
        return i;
      }
    }
    return num_tombstones_;
  }

  /** @brief 从墓碑队列的第 slot 个位置摘除一条，后面的整体前移（保持 FIFO 顺序）。 */
  void EraseTombstoneSlot(size_t slot) {
    for (size_t i = slot + 1; i < num_tombstones_; i++) {
      tombstones_[i - 1] = tombstones_[i];
    }
    num_tombstones_--;
  }

  page_id_t next_page_id_;
  size_t num_tombstones_;
  // Fixed-size tombstone buffer (indexes into key_array_ / rid_array_).
  size_t tombstones_[LEAF_PAGE_TOMB_CNT];
  // Array members for page data.
  KeyType key_array_[LEAF_PAGE_SLOT_CNT];
  ValueType rid_array_[LEAF_PAGE_SLOT_CNT];
  // (Spring 2025) Feel free to add more fields and helper functions below if needed
};

}  // namespace bustub
