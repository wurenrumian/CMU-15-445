//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_htable_directory_page.cpp
//
// Identification: src/storage/page/extendible_htable_directory_page.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "storage/page/extendible_htable_directory_page.h"

#include <algorithm>
#include <unordered_map>

#include "common/config.h"
#include "common/logger.h"

namespace bustub {

/**
 * After creating a new directory page from buffer pool, must call initialize
 * method to set default values
 * @param max_depth Max depth in the directory page
 *
 * ==== P2 STEP 22: 全局深度与局部深度 ====
 * 这是可扩展哈希唯一真正需要理解的概念，其余全是它的推论。
 *
 *   全局深度 GD —— 目录用哈希的低几位来寻址。目录大小恒为 2^GD。
 *   局部深度 LD —— **某一个桶**实际用到了低几位。LD ≤ GD。
 *
 * 关键推论：一个 LD 比 GD 小的桶，会被 **2^(GD−LD) 个目录槽位同时指向**。
 * 例如 GD=3、某桶 LD=1，那么低 1 位为 1 的四个槽位 001/011/101/111 全指向它。
 *
 * 这个"多对一"正是可扩展哈希优于普通哈希的地方：
 * **桶满了不必重建整张表，只需分裂那一个桶。**
 * 分裂时把该桶的 LD 加一，原来指向它的那批槽位一分为二，
 * 一半继续指向老桶、一半指向新桶。其它桶完全不受影响。
 *
 * 只有当 LD 已经等于 GD（没有多余槽位可分）时，才需要把目录整体翻倍（GD+1）。
 * 目录翻倍是 O(2^GD) 的拷贝，但它发生的频率远低于桶分裂。
 */
void ExtendibleHTableDirectoryPage::Init(uint32_t max_depth) {
  max_depth_ = max_depth;
  global_depth_ = 0;
  // 只有 [0, Size()) 是"活"的，但整个数组都清干净：DecrGlobalDepth 之后
  // 高半区会变成不可见的垃圾，将来 IncrGlobalDepth 又会把它当有效数据拷回来。
  // 与其依赖"每次都记得覆盖"，不如一开始就保证全数组有确定值。
  for (uint32_t i = 0; i < HTABLE_DIRECTORY_ARRAY_SIZE; i++) {
    local_depths_[i] = 0;
    bucket_page_ids_[i] = INVALID_PAGE_ID;
  }
}

/**
 * Get the bucket index that the key is hashed to
 *
 * @param hash the hash of the key
 * @return bucket index current key is hashed to
 */
auto ExtendibleHTableDirectoryPage::HashToBucketIndex(uint32_t hash) const -> uint32_t {
  // 取**低** global_depth_ 位。取低位而非高位是有讲究的：
  // 目录翻倍时，新的高半区是旧半区的逐项拷贝，于是一个键在翻倍前后落到的
  // 槽位要么不变、要么变成 idx + 2^GD —— 而这两个槽位指向同一个桶。
  // 也就是说**目录翻倍本身不需要搬动任何数据**，只有随后的桶分裂才搬。
  return hash & GetGlobalDepthMask();
}

/**
 * Lookup a bucket page using a directory index
 *
 * @param bucket_idx the index in the directory to lookup
 * @return bucket page_id corresponding to bucket_idx
 */
auto ExtendibleHTableDirectoryPage::GetBucketPageId(uint32_t bucket_idx) const -> page_id_t {
  if (bucket_idx >= HTABLE_DIRECTORY_ARRAY_SIZE) {
    return INVALID_PAGE_ID;
  }
  return bucket_page_ids_[bucket_idx];
}

/**
 * Updates the directory index using a bucket index and page_id
 *
 * @param bucket_idx directory index at which to insert page_id
 * @param bucket_page_id page_id to insert
 */
void ExtendibleHTableDirectoryPage::SetBucketPageId(uint32_t bucket_idx, page_id_t bucket_page_id) {
  if (bucket_idx >= HTABLE_DIRECTORY_ARRAY_SIZE) {
    return;
  }
  bucket_page_ids_[bucket_idx] = bucket_page_id;
}

/**
 * Gets the split image of an index
 *
 * @param bucket_idx the directory index for which to find the split image
 * @return the directory index of the split image
 *
 * ==== P2 STEP 23: 「分裂镜像」是唯一有资格与我合并的伙伴 ====
 * 把 LD 的最高位（第 LD-1 位）翻转，得到的就是分裂镜像：
 * 它是当初和我从同一个桶里分出来的那一半，低 LD-1 位与我完全相同。
 *
 * 合并只能在镜像之间进行。随便挑一个空桶去合并是错的——
 * 它们的低位模式不同，合并后目录就无法用"低 LD 位"这一条规则寻址了。
 */
auto ExtendibleHTableDirectoryPage::GetSplitImageIndex(uint32_t bucket_idx) const -> uint32_t {
  const uint32_t local_depth = GetLocalDepth(bucket_idx);
  if (local_depth == 0) {
    // LD=0 的桶被整个目录指向，它没有镜像，也永远不需要合并。
    return bucket_idx;
  }
  return bucket_idx ^ (1U << (local_depth - 1));
}

/** @return mask of global_depth 1's and the rest 0's (with 1's from LSB upwards) */
auto ExtendibleHTableDirectoryPage::GetGlobalDepthMask() const -> uint32_t { return (1U << global_depth_) - 1; }

/** @return mask of local 1's and the rest 0's (with 1's from LSB upwards) */
auto ExtendibleHTableDirectoryPage::GetLocalDepthMask(uint32_t bucket_idx) const -> uint32_t {
  return (1U << GetLocalDepth(bucket_idx)) - 1;
}

/**
 * Get the global depth of the hash table directory
 *
 * @return the global depth of the directory
 */
auto ExtendibleHTableDirectoryPage::GetGlobalDepth() const -> uint32_t { return global_depth_; }

/** @return the max depth of the directory */
auto ExtendibleHTableDirectoryPage::GetMaxDepth() const -> uint32_t { return max_depth_; }

/**
 * Increment the global depth of the directory
 *
 * ==== P2 STEP 24: 目录翻倍 = 把整个数组原样复制一份接在后面 ====
 * 新槽位 i + 2^GD 与旧槽位 i 指向**同一个桶**，局部深度也照抄。
 * 这一步之后目录变大了，但数据一条都没动、桶一个都没多 ——
 * 它只是把"分裂的余地"腾出来，真正的分裂由调用方紧接着完成。
 *
 * 漏掉这次拷贝是最常见的写法错误：目录高半区会留着 INVALID_PAGE_ID，
 * 于是一半的键突然查不到了，而 VerifyIntegrity 直到下一次断言才报出来。
 */
void ExtendibleHTableDirectoryPage::IncrGlobalDepth() {
  if (global_depth_ >= max_depth_) {
    return;
  }
  const uint32_t old_size = Size();
  for (uint32_t i = 0; i < old_size; i++) {
    bucket_page_ids_[old_size + i] = bucket_page_ids_[i];
    local_depths_[old_size + i] = local_depths_[i];
  }
  global_depth_++;
}

/**
 * Decrement the global depth of the directory
 */
void ExtendibleHTableDirectoryPage::DecrGlobalDepth() {
  if (global_depth_ == 0) {
    return;
  }
  global_depth_--;
  // 高半区不必清理：CanShrink() 已经保证了它与低半区完全一致
  // （所有 LD < GD ⇒ 每个桶至少被两个槽位指向，且 i 与 i+2^(GD-1) 必然成对）。
}

/**
 * @return true if the directory can be shrunk
 *
 * 收缩的条件是**所有**桶的 LD 都严格小于 GD。
 * 只要还有一个桶的 LD == GD，它就独占一个槽位，砍掉高半区会把它弄丢。
 */
auto ExtendibleHTableDirectoryPage::CanShrink() -> bool {
  if (global_depth_ == 0) {
    return false;
  }
  const uint32_t size = Size();
  for (uint32_t i = 0; i < size; i++) {
    if (local_depths_[i] >= global_depth_) {
      return false;
    }
  }
  return true;
}

/**
 * @return the current directory size
 */
auto ExtendibleHTableDirectoryPage::Size() const -> uint32_t { return 1U << global_depth_; }

/**
 * @return the max directory size
 */
auto ExtendibleHTableDirectoryPage::MaxSize() const -> uint32_t { return 1U << max_depth_; }

/**
 * Gets the local depth of the bucket at bucket_idx
 *
 * @param bucket_idx the bucket index to lookup
 * @return the local depth of the bucket at bucket_idx
 */
auto ExtendibleHTableDirectoryPage::GetLocalDepth(uint32_t bucket_idx) const -> uint32_t {
  if (bucket_idx >= HTABLE_DIRECTORY_ARRAY_SIZE) {
    return 0;
  }
  return local_depths_[bucket_idx];
}

/**
 * Set the local depth of the bucket at bucket_idx to local_depth
 *
 * @param bucket_idx bucket index to update
 * @param local_depth new local depth
 */
void ExtendibleHTableDirectoryPage::SetLocalDepth(uint32_t bucket_idx, uint8_t local_depth) {
  if (bucket_idx >= HTABLE_DIRECTORY_ARRAY_SIZE) {
    return;
  }
  local_depths_[bucket_idx] = local_depth;
}

/**
 * Increment the local depth of the bucket at bucket_idx
 * @param bucket_idx bucket index to increment
 */
void ExtendibleHTableDirectoryPage::IncrLocalDepth(uint32_t bucket_idx) {
  if (bucket_idx >= HTABLE_DIRECTORY_ARRAY_SIZE) {
    return;
  }
  local_depths_[bucket_idx]++;
}

/**
 * Decrement the local depth of the bucket at bucket_idx
 * @param bucket_idx bucket index to decrement
 */
void ExtendibleHTableDirectoryPage::DecrLocalDepth(uint32_t bucket_idx) {
  if (bucket_idx >= HTABLE_DIRECTORY_ARRAY_SIZE || local_depths_[bucket_idx] == 0) {
    return;
  }
  local_depths_[bucket_idx]--;
}

}  // namespace bustub
