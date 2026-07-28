//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_htable_bucket_page.cpp
//
// Identification: src/storage/page/extendible_htable_bucket_page.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <optional>
#include <utility>

#include "common/exception.h"
#include "storage/page/extendible_htable_bucket_page.h"

namespace bustub {

/**
 * After creating a new bucket page from buffer pool, must call initialize
 * method to set default values
 * @param max_size Max size of the bucket array
 *
 * ==== P2 STEP 25: 桶页是「无序紧凑数组」，和 B+ 树叶子完全相反 ====
 * B+ 树叶子必须**有序**，因为它要支持范围扫描和二分查找。
 * 哈希桶只需要支持等值查找，顺序毫无意义 —— 于是可以选最简单的布局：
 * 一个从 0 到 size_-1 连续填充的数组，查找就是线性扫描。
 *
 * 这个取舍是哈希索引的本质：**放弃有序性，换来 O(1) 的期望查找**。
 * 也正因如此，哈希索引做不了 `WHERE a > 5`，更做不了 `ORDER BY a`
 * ——P3 里"索引即物化的排序结果"那条捷径，对哈希索引完全不成立。
 *
 * 线性扫描看着很慢，但桶的容量被页大小卡死（4KB / 每项字节数），
 * 顶多几百项且全在同一页里，缓存友好，比多一层间接寻址还快。
 */
template <typename K, typename V, typename KC>
void ExtendibleHTableBucketPage<K, V, KC>::Init(uint32_t max_size) {
  size_ = 0;
  max_size_ = max_size;
}

/**
 * Lookup a key
 *
 * @param key key to lookup
 * @param[out] value value to set
 * @param cmp the comparator
 * @return true if the key and value are present, false if not found.
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::Lookup(const K &key, V &value, const KC &cmp) const -> bool {
  for (uint32_t i = 0; i < size_; i++) {
    if (cmp(array_[i].first, key) == 0) {
      value = array_[i].second;
      return true;
    }
  }
  return false;
}

/**
 * Attempts to insert a key and value in the bucket.
 *
 * @param key key to insert
 * @param value value to insert
 * @param cmp the comparator to use
 * @return true if inserted, false if bucket is full or the same key is already present
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::Insert(const K &key, const V &value, const KC &cmp) -> bool {
  // 先查重再判满，顺序不能反。
  // 反过来的话，往一个已满的桶里插入**已存在的键**会返回 false，
  // 上层就会误以为"桶满了需要分裂"，于是分裂一个根本不需要分裂的桶；
  // 而分裂后那个键还是重复的，还是插不进去 —— 目录白白翻倍，甚至一路
  // 涨到 max_depth 才罢休。
  for (uint32_t i = 0; i < size_; i++) {
    if (cmp(array_[i].first, key) == 0) {
      return false;
    }
  }
  if (IsFull()) {
    return false;
  }
  array_[size_] = std::make_pair(key, value);
  size_++;
  return true;
}

/**
 * Removes a key and value.
 *
 * @return true if removed, false if not found
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::Remove(const K &key, const KC &cmp) -> bool {
  for (uint32_t i = 0; i < size_; i++) {
    if (cmp(array_[i].first, key) == 0) {
      RemoveAt(i);
      return true;
    }
  }
  return false;
}

/**
 * @brief 删除下标为 bucket_idx 的条目，并保持数组紧凑。
 *
 * 用**末项填空**而不是整体左移：桶内无序，谁在哪个下标毫无语义，
 * 于是删除可以是 O(1) 而不是 O(n)。
 *
 * 代价是条目顺序会被打乱 —— 这正是 MigrateEntries 必须**倒序**遍历的原因
 * （见 disk_extendible_hash_table.cpp STEP 28）。
 */
template <typename K, typename V, typename KC>
void ExtendibleHTableBucketPage<K, V, KC>::RemoveAt(uint32_t bucket_idx) {
  if (bucket_idx >= size_) {
    return;
  }
  array_[bucket_idx] = array_[size_ - 1];
  size_--;
}

/**
 * @brief Gets the key at an index in the bucket.
 *
 * @param bucket_idx the index in the bucket to get the key at
 * @return key at index bucket_idx of the bucket
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::KeyAt(uint32_t bucket_idx) const -> K {
  return array_[bucket_idx].first;
}

/**
 * Gets the value at an index in the bucket.
 *
 * @param bucket_idx the index in the bucket to get the value at
 * @return value at index bucket_idx of the bucket
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::ValueAt(uint32_t bucket_idx) const -> V {
  return array_[bucket_idx].second;
}

/**
 * Gets the entry at an index in the bucket.
 *
 * @param bucket_idx the index in the bucket to get the entry at
 * @return entry at index bucket_idx of the bucket
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::EntryAt(uint32_t bucket_idx) const -> const std::pair<K, V> & {
  return array_[bucket_idx];
}

/**
 * @return number of entries in the bucket
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::Size() const -> uint32_t {
  return size_;
}

/**
 * @return whether the bucket is full
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::IsFull() const -> bool {
  return size_ >= max_size_;
}

/**
 * @return whether the bucket is empty
 */
template <typename K, typename V, typename KC>
auto ExtendibleHTableBucketPage<K, V, KC>::IsEmpty() const -> bool {
  return size_ == 0;
}

template class ExtendibleHTableBucketPage<int, int, IntComparator>;
template class ExtendibleHTableBucketPage<GenericKey<4>, RID, GenericComparator<4>>;
template class ExtendibleHTableBucketPage<GenericKey<8>, RID, GenericComparator<8>>;
template class ExtendibleHTableBucketPage<GenericKey<16>, RID, GenericComparator<16>>;
template class ExtendibleHTableBucketPage<GenericKey<32>, RID, GenericComparator<32>>;
template class ExtendibleHTableBucketPage<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
