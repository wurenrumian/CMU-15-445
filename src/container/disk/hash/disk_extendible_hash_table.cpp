//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// disk_extendible_hash_table.cpp
//
// Identification: src/container/disk/hash/disk_extendible_hash_table.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "common/config.h"
#include "common/exception.h"
#include "common/logger.h"
#include "common/macros.h"
#include "common/rid.h"
#include "common/util/hash_util.h"
#include "container/disk/hash/disk_extendible_hash_table.h"
#include "storage/index/hash_comparator.h"
#include "storage/page/extendible_htable_bucket_page.h"
#include "storage/page/extendible_htable_directory_page.h"
#include "storage/page/extendible_htable_header_page.h"
#include "storage/page/page_guard.h"

namespace bustub {

/**
 * @brief Creates a new DiskExtendibleHashTable.
 *
 * @param name
 * @param bpm buffer pool manager to be used
 * @param cmp comparator for keys
 * @param hash_fn the hash function
 * @param header_max_depth the max depth allowed for the header page
 * @param directory_max_depth the max depth allowed for the directory page
 * @param bucket_max_size the max size allowed for the bucket page array
 *
 * ==== P2 STEP 26: 建表只造一个 header 页 ====
 * 目录页和桶页全部**懒创建**。一张空表因此只占一页，
 * 而不是上来就分配 2^header_max_depth 个目录 + 一堆空桶。
 *
 * 与 B+ 树的对照很有意思：B+ 树空树时连根都没有（root_page_id 是
 * INVALID_PAGE_ID），哈希表则必须有一个 header —— 因为 header 的大小
 * 是静态的，它就是这张表的身份。
 */
template <typename K, typename V, typename KC>
DiskExtendibleHashTable<K, V, KC>::DiskExtendibleHashTable(const std::string &name, BufferPoolManager *bpm,
                                                           const KC &cmp, const HashFunction<K> &hash_fn,
                                                           uint32_t header_max_depth, uint32_t directory_max_depth,
                                                           uint32_t bucket_max_size)
    : index_name_(name),
      bpm_(bpm),
      cmp_(cmp),
      hash_fn_(std::move(hash_fn)),
      header_max_depth_(header_max_depth),
      directory_max_depth_(directory_max_depth),
      bucket_max_size_(bucket_max_size) {
  header_page_id_ = bpm_->NewPage();
  auto guard = bpm_->WritePage(header_page_id_);
  auto *header = guard.template AsMut<ExtendibleHTableHeaderPage>();
  header->Init(header_max_depth_);
}

template <typename K, typename V, typename KC>
auto DiskExtendibleHashTable<K, V, KC>::Hash(K key) const -> uint32_t {
  // HashFunction 返回 64 位，截成 32 位。高低位在这里都会用到
  // （header 取高位、directory 取低位），所以不能只保留一半。
  return static_cast<uint32_t>(hash_fn_.GetHash(key));
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/

/**
 * Get the value associated with a given key in the hash table.
 *
 * Note(fall2023): This semester you will only need to support unique key-value pairs.
 *
 * @param key the key to look up
 * @param[out] result the value(s) associated with a given key
 * @param transaction the current transaction
 * @return the value(s) associated with the given key
 *
 * ==== P2 STEP 27: 查询路径 —— 三次页访问，全部只用读锁 ====
 * header → directory → bucket，每层拿到下一层的 page_id 后**立刻放掉上一层的锁**。
 * 这就是 P2 B+ 树里那套螃蟹锁（latch crabbing），只不过哈希表的树高恒为 3，
 * 不像 B+ 树那样随数据量变化。
 *
 * 三次页访问听起来不比 B+ 树少（B+ 树 3 层也是 3 次），但关键差别在于：
 * **哈希表的层数是常数，B+ 树的层数是 O(log n)**。数据涨一百倍，
 * 哈希表还是 3 次，B+ 树可能变成 4~5 次。
 */
template <typename K, typename V, typename KC>
auto DiskExtendibleHashTable<K, V, KC>::GetValue(const K &key, std::vector<V> *result, Transaction *transaction) const
    -> bool {
  const uint32_t hash = Hash(key);

  auto header_guard = bpm_->ReadPage(header_page_id_);
  const auto *header = header_guard.template As<ExtendibleHTableHeaderPage>();
  const page_id_t directory_page_id = header->GetDirectoryPageId(header->HashToDirectoryIndex(hash));
  if (directory_page_id == INVALID_PAGE_ID) {
    return false;  // 这个目录还没被创建过 —— 键必然不存在。
  }

  auto directory_guard = bpm_->ReadPage(directory_page_id);
  header_guard.Drop();  // 拿到目录页的锁之后，header 就不需要了。
  const auto *directory = directory_guard.template As<ExtendibleHTableDirectoryPage>();
  const page_id_t bucket_page_id = directory->GetBucketPageId(directory->HashToBucketIndex(hash));
  if (bucket_page_id == INVALID_PAGE_ID) {
    return false;
  }

  auto bucket_guard = bpm_->ReadPage(bucket_page_id);
  directory_guard.Drop();
  const auto *bucket = bucket_guard.template As<ExtendibleHTableBucketPage<K, V, KC>>();

  V value{};
  if (!bucket->Lookup(key, value, cmp_)) {
    return false;
  }
  result->push_back(value);
  return true;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/

/**
 * Inserts a key-value pair into the hash table.
 *
 * @param key the key to create
 * @param value the value to be associated with the key
 * @param transaction the current transaction
 * @return true if insert succeeded, false otherwise
 *
 * ==== P2 STEP 28: 插入 —— 桶满了就分裂，而不是重建整张表 ====
 * 这是可扩展哈希存在的全部理由。静态哈希表满了要 rehash 全部数据（O(n) 停顿），
 * 可扩展哈希只动**那一个**桶：
 *
 *   1. 桶没满            → 直接插入，结束。
 *   2. 桶满且 LD < GD    → 只分裂这个桶。目录不动，其它桶不动。
 *   3. 桶满且 LD == GD   → 目录先翻倍（GD+1），再回到情况 2。
 *   4. GD 已达 max_depth → 真的满了，返回 false。
 *
 * 情况 2 是常态，情况 3 少见，摊还下来插入是 O(1)。
 *
 * 注意这里用 while 循环而不是"分裂一次就插入"：
 * 分裂后所有条目**可能仍然落在同一侧**（哈希值的那一位恰好都相同），
 * 于是桶还是满的，需要接着分裂。只有 max_depth 能终止这个循环。
 */
template <typename K, typename V, typename KC>
auto DiskExtendibleHashTable<K, V, KC>::Insert(const K &key, const V &value, Transaction *transaction) -> bool {
  const uint32_t hash = Hash(key);

  auto header_guard = bpm_->WritePage(header_page_id_);
  auto *header = header_guard.template AsMut<ExtendibleHTableHeaderPage>();
  const uint32_t directory_idx = header->HashToDirectoryIndex(hash);
  const page_id_t directory_page_id = header->GetDirectoryPageId(directory_idx);

  if (directory_page_id == INVALID_PAGE_ID) {
    return InsertToNewDirectory(header, directory_idx, hash, key, value);
  }

  auto directory_guard = bpm_->WritePage(directory_page_id);
  // 目录写锁到手，header 可以放了。整个插入过程中我们独占这个目录，
  // 因此下面即使同时持有两把桶锁也不会死锁 —— 目录锁是唯一的入口。
  header_guard.Drop();
  auto *directory = directory_guard.template AsMut<ExtendibleHTableDirectoryPage>();

  uint32_t bucket_idx = directory->HashToBucketIndex(hash);
  page_id_t bucket_page_id = directory->GetBucketPageId(bucket_idx);
  if (bucket_page_id == INVALID_PAGE_ID) {
    return InsertToNewBucket(directory, bucket_idx, key, value);
  }

  while (true) {
    auto bucket_guard = bpm_->WritePage(bucket_page_id);
    auto *bucket = bucket_guard.template AsMut<ExtendibleHTableBucketPage<K, V, KC>>();

    if (bucket->Insert(key, value, cmp_)) {
      return true;
    }
    // Insert 失败有两种原因：键重复，或桶满。只有后者才该分裂。
    V existing{};
    if (bucket->Lookup(key, existing, cmp_)) {
      return false;  // 重复键，本学期只支持唯一键。
    }

    // ---- 桶确实满了，准备分裂 ----
    const uint32_t local_depth = directory->GetLocalDepth(bucket_idx);
    if (local_depth == directory->GetGlobalDepth()) {
      if (directory->GetGlobalDepth() >= directory->GetMaxDepth()) {
        return false;  // 目录到顶，这张表真的装不下了。
      }
      directory->IncrGlobalDepth();
      // 目录翻倍后 bucket_idx 依然有效（低位没变），但 Size() 变了，
      // 下面按新的 Size() 重新铺设指针。
    }

    // 分裂：把这个桶的一半条目搬到新桶。
    //   old_mask 下相同的那批槽位 = 当前指向本桶的全部槽位；
    //   加一位之后，该批槽位一分为二：新增位为 0 的留在老桶，为 1 的指向新桶。
    const uint32_t new_local_depth = local_depth + 1;
    const uint32_t old_mask = (1U << local_depth) - 1;
    const uint32_t new_mask = (1U << new_local_depth) - 1;
    const uint32_t base = bucket_idx & old_mask;
    const uint32_t new_bucket_idx = base | (1U << local_depth);

    const page_id_t new_bucket_page_id = bpm_->NewPage();
    if (new_bucket_page_id == INVALID_PAGE_ID) {
      return false;
    }
    {
      auto new_guard = bpm_->WritePage(new_bucket_page_id);
      auto *new_bucket = new_guard.template AsMut<ExtendibleHTableBucketPage<K, V, KC>>();
      new_bucket->Init(bucket_max_size_);

      // 先把这一组槽位的局部深度统一加一（两侧都要），再改指向。
      // 顺序反了的话 UpdateDirectoryMapping 用新深度去匹配，
      // 会漏掉本该更新的槽位。
      const uint32_t dir_size = directory->Size();
      for (uint32_t i = 0; i < dir_size; i++) {
        if ((i & old_mask) == base) {
          directory->SetLocalDepth(i, new_local_depth);
        }
      }
      UpdateDirectoryMapping(directory, new_bucket_idx, new_bucket_page_id, new_local_depth, new_mask);
      MigrateEntries(bucket, new_bucket, new_bucket_idx, new_mask);
    }

    // 目录变了，本键该去哪个桶要重算 —— 可能还是老桶，也可能是新桶。
    bucket_idx = directory->HashToBucketIndex(hash);
    bucket_page_id = directory->GetBucketPageId(bucket_idx);
  }
}

/**
 * @brief 某个 header 槽位还没有目录时，造一个目录 + 一个桶，然后插入。
 */
template <typename K, typename V, typename KC>
auto DiskExtendibleHashTable<K, V, KC>::InsertToNewDirectory(ExtendibleHTableHeaderPage *header, uint32_t directory_idx,
                                                             uint32_t hash, const K &key, const V &value) -> bool {
  const page_id_t directory_page_id = bpm_->NewPage();
  if (directory_page_id == INVALID_PAGE_ID) {
    return false;
  }
  header->SetDirectoryPageId(directory_idx, directory_page_id);

  auto directory_guard = bpm_->WritePage(directory_page_id);
  auto *directory = directory_guard.template AsMut<ExtendibleHTableDirectoryPage>();
  directory->Init(directory_max_depth_);

  // 新目录 GD=0，只有 1 个槽位，所以 bucket_idx 必然是 0。
  return InsertToNewBucket(directory, directory->HashToBucketIndex(hash), key, value);
}

/**
 * @brief 某个目录槽位还没有桶时，造一个桶然后插入。
 */
template <typename K, typename V, typename KC>
auto DiskExtendibleHashTable<K, V, KC>::InsertToNewBucket(ExtendibleHTableDirectoryPage *directory, uint32_t bucket_idx,
                                                          const K &key, const V &value) -> bool {
  const page_id_t bucket_page_id = bpm_->NewPage();
  if (bucket_page_id == INVALID_PAGE_ID) {
    return false;
  }
  auto bucket_guard = bpm_->WritePage(bucket_page_id);
  auto *bucket = bucket_guard.template AsMut<ExtendibleHTableBucketPage<K, V, KC>>();
  bucket->Init(bucket_max_size_);

  directory->SetBucketPageId(bucket_idx, bucket_page_id);
  directory->SetLocalDepth(bucket_idx, directory->GetGlobalDepth());
  return bucket->Insert(key, value, cmp_);
}

/**
 * @brief 把所有"低 new_local_depth 位等于 new_bucket_idx"的目录槽位指向新桶。
 *
 * 注意不是只改 new_bucket_idx 一个槽位。GD 可能远大于新的局部深度，
 * 此时有 2^(GD − LD) 个槽位共同指向新桶，必须全部更新 ——
 * 漏掉任何一个，VerifyIntegrity 的"每个桶恰好被 2^(GD−LD) 个槽位指向"就会失败。
 */
template <typename K, typename V, typename KC>
void DiskExtendibleHashTable<K, V, KC>::UpdateDirectoryMapping(ExtendibleHTableDirectoryPage *directory,
                                                               uint32_t new_bucket_idx, page_id_t new_bucket_page_id,
                                                               uint32_t new_local_depth, uint32_t local_depth_mask) {
  const uint32_t dir_size = directory->Size();
  const uint32_t target = new_bucket_idx & local_depth_mask;
  for (uint32_t i = 0; i < dir_size; i++) {
    if ((i & local_depth_mask) == target) {
      directory->SetBucketPageId(i, new_bucket_page_id);
      directory->SetLocalDepth(i, new_local_depth);
    }
  }
}

/**
 * @brief 把老桶里"该归新桶"的条目搬过去。
 *
 * **必须倒序遍历。** 桶页的 RemoveAt 用末项填空（O(1) 删除），
 * 正序遍历时删掉下标 i 会把最后一项挪到 i，而循环紧接着 i++ 跳过了它 ——
 * 那一条永远不会被检查，于是本该迁走的条目留在了老桶里，
 * 之后按新的低位去查就再也找不到。倒序则天然避开这个问题。
 */
template <typename K, typename V, typename KC>
void DiskExtendibleHashTable<K, V, KC>::MigrateEntries(ExtendibleHTableBucketPage<K, V, KC> *old_bucket,
                                                       ExtendibleHTableBucketPage<K, V, KC> *new_bucket,
                                                       uint32_t new_bucket_idx, uint32_t local_depth_mask) {
  const uint32_t target = new_bucket_idx & local_depth_mask;
  for (uint32_t i = old_bucket->Size(); i > 0; i--) {
    const uint32_t idx = i - 1;
    const auto &entry = old_bucket->EntryAt(idx);
    if ((Hash(entry.first) & local_depth_mask) == target) {
      new_bucket->Insert(entry.first, entry.second, cmp_);
      old_bucket->RemoveAt(idx);
    }
  }
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/

/**
 * Removes a key-value pair from the hash table.
 *
 * @param key the key to delete
 * @param value the value to delete
 * @param transaction the current transaction
 * @return true if remove succeeded, false otherwise
 *
 * ==== P2 STEP 29: 删除 —— 合并空桶，必要时收缩目录 ====
 * 分裂的逆操作，但条件更苛刻，因为合并错了会直接破坏寻址规则：
 *
 *   1. 只能和**分裂镜像**合并（低 LD−1 位相同的那个兄弟，见 STEP 23）；
 *   2. 双方的局部深度必须**相等**。镜像 LD 更小说明它自己还带着一堆槽位，
 *      不是当初和我一起分出来的那个；
 *   3. 两者至少有一个是空的。
 *
 * 合并后 LD 减一，两批槽位重新指向同一个桶。这可能让更外层的桶也满足合并条件，
 * 所以要循环往下走。全部合并完，若**所有** LD 都小于 GD，目录就能整体减半。
 *
 * 与 P2 B+ 树的对照：B+ 树的合并会向上传播到父节点、可能一路改到根；
 * 哈希表的合并只在同一个目录内横向传播，永远碰不到 header。
 * 层数固定的结构，维护起来简单得多 —— 这是哈希索引相对 B+ 树的一个真实优势。
 */
template <typename K, typename V, typename KC>
auto DiskExtendibleHashTable<K, V, KC>::Remove(const K &key, Transaction *transaction) -> bool {
  const uint32_t hash = Hash(key);

  auto header_guard = bpm_->ReadPage(header_page_id_);
  const auto *header = header_guard.template As<ExtendibleHTableHeaderPage>();
  const page_id_t directory_page_id = header->GetDirectoryPageId(header->HashToDirectoryIndex(hash));
  if (directory_page_id == INVALID_PAGE_ID) {
    return false;
  }

  auto directory_guard = bpm_->WritePage(directory_page_id);
  header_guard.Drop();
  auto *directory = directory_guard.template AsMut<ExtendibleHTableDirectoryPage>();

  uint32_t bucket_idx = directory->HashToBucketIndex(hash);
  const page_id_t bucket_page_id = directory->GetBucketPageId(bucket_idx);
  if (bucket_page_id == INVALID_PAGE_ID) {
    return false;
  }

  {
    auto bucket_guard = bpm_->WritePage(bucket_page_id);
    auto *bucket = bucket_guard.template AsMut<ExtendibleHTableBucketPage<K, V, KC>>();
    if (!bucket->Remove(key, cmp_)) {
      return false;
    }
    // 桶锁必须在这里放掉：下面的合并循环要重新读这一页判空，
    // 而页锁是不可重入的，攥着不放会当场自锁死。
  }

  // ---- 合并 ----
  while (true) {
    const uint32_t local_depth = directory->GetLocalDepth(bucket_idx);
    if (local_depth == 0) {
      break;  // LD=0 的桶被整个目录指向，没有镜像可合。
    }
    const uint32_t image_idx = directory->GetSplitImageIndex(bucket_idx);
    if (directory->GetLocalDepth(image_idx) != local_depth) {
      break;  // 深度不等 ⇒ 不是当初和我一起分出来的那一半。
    }
    const page_id_t cur_page_id = directory->GetBucketPageId(bucket_idx);
    const page_id_t img_page_id = directory->GetBucketPageId(image_idx);
    if (cur_page_id == img_page_id || cur_page_id == INVALID_PAGE_ID || img_page_id == INVALID_PAGE_ID) {
      break;
    }

    bool cur_empty = false;
    bool img_empty = false;
    {
      auto g = bpm_->ReadPage(cur_page_id);
      cur_empty = g.template As<ExtendibleHTableBucketPage<K, V, KC>>()->IsEmpty();
    }
    {
      auto g = bpm_->ReadPage(img_page_id);
      img_empty = g.template As<ExtendibleHTableBucketPage<K, V, KC>>()->IsEmpty();
    }
    if (!cur_empty && !img_empty) {
      break;  // 两边都有数据，合并会溢出。
    }

    // 留下非空的那一个（都空则留镜像，随便挑一个即可）。
    const page_id_t keep_page_id = cur_empty ? img_page_id : cur_page_id;
    const page_id_t drop_page_id = cur_empty ? cur_page_id : img_page_id;

    const uint32_t new_local_depth = local_depth - 1;
    const uint32_t new_mask = (1U << new_local_depth) - 1;
    const uint32_t base = bucket_idx & new_mask;
    const uint32_t dir_size = directory->Size();
    for (uint32_t i = 0; i < dir_size; i++) {
      if ((i & new_mask) == base) {
        directory->SetBucketPageId(i, keep_page_id);
        directory->SetLocalDepth(i, new_local_depth);
      }
    }
    bpm_->DeletePage(drop_page_id);

    // 合并后可能满足更外一层的合并条件，继续往下走。
    bucket_idx = base;
  }

  // ---- 收缩目录 ----
  // 用 while 而不是 if：一次删除可能连续触发多轮合并，
  // 于是目录也可能连续减半好几次。
  while (directory->CanShrink()) {
    directory->DecrGlobalDepth();
  }
  return true;
}

template <typename K, typename V, typename KC>
auto DiskExtendibleHashTable<K, V, KC>::GetHeaderPageId() const -> page_id_t {
  return header_page_id_;
}

template <typename K, typename V, typename KC>
void DiskExtendibleHashTable<K, V, KC>::VerifyIntegrity() const {
  auto header_guard = bpm_->ReadPage(header_page_id_);
  const auto *header = header_guard.template As<ExtendibleHTableHeaderPage>();
  for (uint32_t idx = 0; idx < header->MaxSize(); idx++) {
    const page_id_t directory_page_id = header->GetDirectoryPageId(idx);
    if (directory_page_id == INVALID_PAGE_ID) {
      continue;
    }
    auto directory_guard = bpm_->ReadPage(directory_page_id);
    directory_guard.template As<ExtendibleHTableDirectoryPage>()->VerifyIntegrity();
  }
}

template <typename K, typename V, typename KC>
void DiskExtendibleHashTable<K, V, KC>::PrintHT() const {
  auto header_guard = bpm_->ReadPage(header_page_id_);
  const auto *header = header_guard.template As<ExtendibleHTableHeaderPage>();
  header->PrintHeader();
  for (uint32_t idx = 0; idx < header->MaxSize(); idx++) {
    const page_id_t directory_page_id = header->GetDirectoryPageId(idx);
    if (directory_page_id == INVALID_PAGE_ID) {
      continue;
    }
    auto directory_guard = bpm_->ReadPage(directory_page_id);
    const auto *directory = directory_guard.template As<ExtendibleHTableDirectoryPage>();
    directory->PrintDirectory();
    for (uint32_t bucket_idx = 0; bucket_idx < directory->Size(); bucket_idx++) {
      const page_id_t bucket_page_id = directory->GetBucketPageId(bucket_idx);
      if (bucket_page_id == INVALID_PAGE_ID) {
        continue;
      }
      auto bucket_guard = bpm_->ReadPage(bucket_page_id);
      bucket_guard.template As<ExtendibleHTableBucketPage<K, V, KC>>()->PrintBucket();
    }
  }
}

template class DiskExtendibleHashTable<int, int, IntComparator>;
template class DiskExtendibleHashTable<GenericKey<4>, RID, GenericComparator<4>>;
template class DiskExtendibleHashTable<GenericKey<8>, RID, GenericComparator<8>>;
template class DiskExtendibleHashTable<GenericKey<16>, RID, GenericComparator<16>>;
template class DiskExtendibleHashTable<GenericKey<32>, RID, GenericComparator<32>>;
template class DiskExtendibleHashTable<GenericKey<64>, RID, GenericComparator<64>>;
}  // namespace bustub
