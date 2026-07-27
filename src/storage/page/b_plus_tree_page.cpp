//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree_page.cpp
//
// Identification: src/storage/page/b_plus_tree_page.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "storage/page/b_plus_tree_page.h"

namespace bustub {

/*
 * Helper methods to get/set page type
 * Page type enum class is defined in b_plus_tree_page.h
 */
// ==== P2 STEP 1: 页头就是对那 4KB 前 12 个字节的解释 ====
// BPlusTreePage 的构造/析构函数全被 delete 了，它从来不会被 new 出来。
// 上层永远是这样用的：
//     auto *page = guard.AsMut<BPlusTreeLeafPage<...>>();   // reinterpret_cast
// 也就是说，这些成员函数操作的是「缓冲池某一帧里的原始字节」，
// 页在磁盘和内存中的二进制布局必须严格一致——这也是为什么这里不能有虚函数
// （虚表指针会混进页数据，写到磁盘上毫无意义，重新读入后还是野指针）。
auto BPlusTreePage::IsLeafPage() const -> bool { return page_type_ == IndexPageType::LEAF_PAGE; }
void BPlusTreePage::SetPageType(IndexPageType page_type) { page_type_ = page_type; }

/*
 * Helper methods to get/set size (number of key/value pairs stored in that
 * page)
 */
auto BPlusTreePage::GetSize() const -> int { return size_; }
void BPlusTreePage::SetSize(int size) { size_ = size; }
void BPlusTreePage::ChangeSizeBy(int amount) { size_ += amount; }

/*
 * Helper methods to get/set max size (capacity) of the page
 */
auto BPlusTreePage::GetMaxSize() const -> int { return max_size_; }
void BPlusTreePage::SetMaxSize(int size) { max_size_ = size; }

/*
 * Helper method to get min page size
 * Generally, min page size == max page size / 2
 * But whether you will take ceil() or floor() depends on your implementation
 */
auto BPlusTreePage::GetMinSize() const -> int {
  // ==== P2 STEP 2: 最小容量取 floor(max/2) ====
  // 这个选择被测试锁死了：TombstoneBasicTest 里 leaf_max_size = 4 时
  // 断言 GetMinSize() == 2，TombstoneCoalesceTest 里 max = 6 时行为对应 minsize = 3。
  //
  // 语义：一个非根节点的条目数低于这个值就算「下溢」，必须向兄弟借用或与之合并。
  // 保证每个节点至少半满，是 B+ 树高度为 O(log n)、空间利用率不低于 50% 的根据。
  //
  // 注意根节点不受此约束：
  //   - 根是叶子时（整棵树只有一页），可以只剩 0 条；
  //   - 根是内部页时，至少要有 2 个孩子，否则应当降低树高（root collapse）。
  // 这两个例外由 b_plus_tree.cpp 里的调用方判断，不在这里处理。
  //
  // ---- 叶子取 floor，内部页取 ceil，二者不能统一 ----
  // 叶子的 size 是「记录条数」，取 floor(max/2)。测试锁死了这个值：
  // TombstoneBasicTest 里 leaf_max_size = 4 时断言 GetMinSize() == 2。
  //
  // 内部页的 size 是「孩子个数」，必须取 ceil(max/2) 且**至少为 2**。
  // 原因：只有一个孩子的内部节点是退化的——它既没有任何分隔键可用，
  // 在下溢处理里也根本找不到兄弟可借可并（parent->ValueAt(sibling_index) 直接越界）。
  // 若这里对内部页也用 floor，internal_max_size = 3 时下限会算成 1，
  // 于是允许单孩子内部节点存在，删除路径立刻踩空。
  if (IsLeafPage()) {
    return max_size_ / 2;
  }
  return (max_size_ + 1) / 2;
}

}  // namespace bustub
