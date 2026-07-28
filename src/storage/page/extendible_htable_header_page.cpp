//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// extendible_htable_header_page.cpp
//
// Identification: src/storage/page/extendible_htable_header_page.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "storage/page/extendible_htable_header_page.h"

#include "common/exception.h"

namespace bustub {

/**
 * After creating a new header page from buffer pool, must call initialize
 * method to set default values
 * @param max_depth Max depth in the header page
 *
 * ==== P2 STEP 20: 三层结构的最顶层 ====
 * 可扩展哈希在 BusTub 里是三层：header → directory → bucket。
 * 为什么不能只有两层？因为**目录本身也放不下一页**。
 * 目录数组要按 2 的幂增长，一个 4KB 页最多装 512 个 page_id
 * （2048 字节的 bucket_page_ids_ + 512 字节的 local_depths_ 已经用掉大半页），
 * 所以单个目录页最多管 2^9 个桶。header 再往上加一层，把键空间先切成
 * 最多 2^9 个彼此独立的目录，总容量提到 2^18 个桶。
 *
 * header 是**静态**的：max_depth_ 在 Init 时定死，目录槽位数此后再不变化，
 * 增长全部发生在 directory 那一层。这也是它在查询路径上只需要读锁的原因。
 */
void ExtendibleHTableHeaderPage::Init(uint32_t max_depth) {
  max_depth_ = max_depth;
  // 全部置为 INVALID_PAGE_ID：目录页是**懒创建**的，第一次有键落到某个槽位
  // 时才去 NewPage。否则一张空表开局就要分配 512 个目录页。
  for (uint32_t i = 0; i < HTABLE_HEADER_ARRAY_SIZE; i++) {
    directory_page_ids_[i] = INVALID_PAGE_ID;
  }
}

/**
 * Get the directory index that the key is hashed to
 *
 * @param hash the hash of the key
 * @return directory index the key is hashed to
 *
 * ==== P2 STEP 21: header 取**高**位，directory 取**低**位 ====
 * 这是整个可扩展哈希最容易记反的一条，而且记反之后表面上还能跑一阵子。
 *
 * header 用高 max_depth_ 位，directory 用低 global_depth_ 位——两者取的是
 * 同一个 32 位哈希值的**两端**，因此互不干扰：某个目录内部无论怎么分裂增长，
 * 都不会把已有的键甩到别的目录去。
 *
 * 如果两层都取低位，目录分裂时低位的含义会变，键就会"跑到隔壁目录"，
 * 而 header 层根本没有迁移机制，那些键就永久找不回来了。
 */
auto ExtendibleHTableHeaderPage::HashToDirectoryIndex(uint32_t hash) const -> uint32_t {
  // max_depth_ == 0 是合法配置（整张表只有一个目录，InsertTest1 就是这么建的）。
  // 这里必须提前返回：`hash >> 32` 在 C++ 里是**未定义行为**，
  // x86 会把移位量按 32 取模，于是原样返回 hash，测试以完全无关的方式挂掉。
  if (max_depth_ == 0) {
    return 0;
  }
  return hash >> (32 - max_depth_);
}

/**
 * Get the directory page id at an index
 *
 * @param directory_idx index in the directory page id array
 * @return directory page_id at index
 */
auto ExtendibleHTableHeaderPage::GetDirectoryPageId(uint32_t directory_idx) const -> page_id_t {
  if (directory_idx >= MaxSize()) {
    return INVALID_PAGE_ID;
  }
  return directory_page_ids_[directory_idx];
}

/**
 * @brief Set the directory page id at an index
 *
 * @param directory_idx index in the directory page id array
 * @param directory_page_id page id of the directory
 */
void ExtendibleHTableHeaderPage::SetDirectoryPageId(uint32_t directory_idx, page_id_t directory_page_id) {
  if (directory_idx >= MaxSize()) {
    return;
  }
  directory_page_ids_[directory_idx] = directory_page_id;
}

/**
 * @brief Get the maximum number of directory page ids the header page could handle
 */
auto ExtendibleHTableHeaderPage::MaxSize() const -> uint32_t { return 1U << max_depth_; }

}  // namespace bustub
