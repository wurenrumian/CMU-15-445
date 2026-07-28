//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree_page.h
//
// Identification: src/include/storage/page/b_plus_tree_page.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cassert>
#include <climits>
#include <cstdlib>
#include <string>

#include "buffer/buffer_pool_manager.h"
#include "storage/index/generic_key.h"

namespace bustub {

#define MappingType std::pair<KeyType, ValueType>

#define FULL_INDEX_TEMPLATE_ARGUMENTS_DEFN \
  template <typename KeyType, typename ValueType, typename KeyComparator, ssize_t NumTombs = 0>
#define FULL_INDEX_TEMPLATE_ARGUMENTS \
  template <typename KeyType, typename ValueType, typename KeyComparator, ssize_t NumTombs>
#define INDEX_TEMPLATE_ARGUMENTS template <typename KeyType, typename ValueType, typename KeyComparator>

// define page type enum
enum class IndexPageType { INVALID_INDEX_PAGE = 0, LEAF_PAGE, INTERNAL_PAGE };

/**
 * Both internal and leaf page are inherited from this page.
 *
 * It actually serves as a header part for each B+ tree page and
 * contains information shared by both leaf page and internal page.
 *
 * Header format (size in byte, 12 bytes in total):
 * ---------------------------------------------------------
 * | PageType (4) | CurrentSize (4) | MaxSize (4) |  ...   |
 * ---------------------------------------------------------
 */
class BPlusTreePage {
 public:
  // Delete all constructor / destructor to ensure memory safety
  BPlusTreePage() = delete;
  BPlusTreePage(const BPlusTreePage &other) = delete;
  ~BPlusTreePage() = delete;

  auto IsLeafPage() const -> bool;
  void SetPageType(IndexPageType page_type);

  auto GetSize() const -> int;
  void SetSize(int size);
  void ChangeSizeBy(int amount);

  auto GetMaxSize() const -> int;
  void SetMaxSize(int max_size);
  auto GetMinSize() const -> int;

 private:
  // ==== P2 STEP 0: 这 12 个字节就是每一页的公共页头 ====
  // 三个字段都被实现用到了，所以骨架里的 __attribute__((__unused__)) 已按其
  // 自带的提示移除。注意字段顺序和类型不能改：叶子页/内部页是靠
  // `reinterpret_cast` 直接解释缓冲池里那 4KB 字节的，页头布局一变，
  // 已经落盘的页就再也读不回来了。
  //
  // Member variables, attributes that both internal and leaf page share
  IndexPageType page_type_;
  // Number of key & value pairs in a page（**内部页存的是孩子个数**，见 STEP 2）
  int size_;
  // Max number of key & value pairs in a page
  int max_size_;
};

}  // namespace bustub
