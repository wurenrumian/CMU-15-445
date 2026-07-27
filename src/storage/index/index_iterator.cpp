//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// index_iterator.cpp
//
// Identification: src/storage/index/index_iterator.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

/**
 * index_iterator.cpp
 */
#include <cassert>

#include "storage/index/index_iterator.h"

namespace bustub {

// ==== P2 STEP 10: 为什么这里只剩下模板实例化 ====
// IndexIterator 的全部成员函数都定义在头文件里（见 index_iterator.h）。
// 这不是偷懒，而是必需的：`operator*` 返回页内数据的引用，`SkipDeleted()`
// 需要在页与页之间跳转，两者都强依赖 LeafPage 的完整定义；把它们放在头里
// 既让编译器有机会内联掉这些一行函数，也避免了在 .cpp 里重复一遍冗长的
// FULL_INDEX_TEMPLATE_ARGUMENTS 签名。
//
// 但显式实例化必须留在这里：B+ 树被 GenericKey<4/8/16/32/64> 等多种键类型
// 使用，在这个唯一的编译单元里集中实例化，可以避免每个 .cpp 都重新展开一遍模板。

template class IndexIterator<GenericKey<4>, RID, GenericComparator<4>>;

template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>>;
template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>, 3>;
template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>, 2>;
template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>, 1>;
template class IndexIterator<GenericKey<8>, RID, GenericComparator<8>, -1>;

template class IndexIterator<GenericKey<16>, RID, GenericComparator<16>>;

template class IndexIterator<GenericKey<32>, RID, GenericComparator<32>>;

template class IndexIterator<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
