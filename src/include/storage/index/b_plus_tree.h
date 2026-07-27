//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree.h
//
// Identification: src/include/storage/index/b_plus_tree.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

/**
 * b_plus_tree.h
 *
 * Implementation of simple b+ tree data structure where internal pages direct
 * the search and leaf pages contain actual data.
 * (1) We only support unique key
 * (2) support insert & remove
 * (3) The structure should shrink and grow dynamically
 * (4) Implement index iterator for range scan
 */
#pragma once

#include <algorithm>
#include <deque>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/macros.h"
#include "storage/index/index_iterator.h"
#include "storage/page/b_plus_tree_header_page.h"
#include "storage/page/b_plus_tree_internal_page.h"
#include "storage/page/b_plus_tree_leaf_page.h"
#include "storage/page/page_guard.h"

namespace bustub {

struct PrintableBPlusTree;

/**
 * @brief Definition of the Context class.
 *
 * Hint: This class is designed to help you keep track of the pages
 * that you're modifying or accessing.
 */
class Context {
 public:
  // When you insert into / remove from the B+ tree, store the write guard of header page here.
  // Remember to drop the header page guard and set it to nullopt when you want to unlock all.
  std::optional<WritePageGuard> header_page_{std::nullopt};

  // Save the root page id here so that it's easier to know if the current page is the root page.
  page_id_t root_page_id_{INVALID_PAGE_ID};

  // Store the write guards of the pages that you're modifying here.
  std::deque<WritePageGuard> write_set_;

  // You may want to use this when getting value, but not necessary.
  std::deque<ReadPageGuard> read_set_;

  auto IsRootPage(page_id_t page_id) -> bool { return page_id == root_page_id_; }
};

#define BPLUSTREE_TYPE BPlusTree<KeyType, ValueType, KeyComparator, NumTombs>

// Main class providing the API for the Interactive B+ Tree.
FULL_INDEX_TEMPLATE_ARGUMENTS_DEFN
class BPlusTree {
  using InternalPage = BPlusTreeInternalPage<KeyType, page_id_t, KeyComparator>;
  // 注意：骨架这里漏掉了 NumTombs，导致树内部用的叶子类型永远是「零墓碑」版本，
  // 与测试实例化的 BPlusTree<..., 2> 所检查的叶子类型不是同一个类。
  // 必须把模板参数透传下去，墓碑功能才真正生效。
  using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator, NumTombs>;

 public:
  explicit BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager *buffer_pool_manager,
                     const KeyComparator &comparator, int leaf_max_size = LEAF_PAGE_SLOT_CNT,
                     int internal_max_size = INTERNAL_PAGE_SLOT_CNT);

  // Returns true if this B+ tree has no keys and values.
  auto IsEmpty() const -> bool;

  // Insert a key-value pair into this B+ tree.
  auto Insert(const KeyType &key, const ValueType &value) -> bool;

  // Remove a key and its value from this B+ tree.
  void Remove(const KeyType &key);

  // Return the value associated with a given key
  auto GetValue(const KeyType &key, std::vector<ValueType> *result) -> bool;

  // Return the page id of the root node
  auto GetRootPageId() -> page_id_t;

  // Index iterator
  auto Begin() -> INDEXITERATOR_TYPE;

  auto End() -> INDEXITERATOR_TYPE;

  auto Begin(const KeyType &key) -> INDEXITERATOR_TYPE;

  void Print(BufferPoolManager *bpm);

  void Draw(BufferPoolManager *bpm, const std::filesystem::path &outf);

  auto DrawBPlusTree() -> std::string;

  // read data from file and insert one by one
  void InsertFromFile(const std::filesystem::path &file_name);

  // read data from file and remove one by one
  void RemoveFromFile(const std::filesystem::path &file_name);

  void BatchOpsFromFile(const std::filesystem::path &file_name);

  // Do not change this type to a BufferPoolManager!
  std::shared_ptr<TracedBufferPoolManager> bpm_;

 private:
  /** @brief 下降时要执行的操作类型，决定「什么样的子节点算安全」。 */
  enum class Operation { SEARCH, INSERT, REMOVE };

  /**
   * @brief 判断一个节点在给定操作下是否「安全」。
   *
   * 安全 = 这次操作绝不会让该节点的条目数越界，因而不会向父节点传播结构变化。
   * 一旦确认子节点安全，就可以立刻释放它所有祖先的写锁（螃蟹锁的精髓）。
   *   - 插入：条目数 < 上限 ⇒ 不会分裂。
   *   - 删除：条目数 > 下限 ⇒ 即便真的物理删掉一条也不会下溢。
   */
  static auto IsSafe(const BPlusTreePage *page, Operation op) -> bool {
    if (op == Operation::INSERT) {
      return page->GetSize() < page->GetMaxSize();
    }
    return page->GetSize() > page->GetMinSize();
  }

  /** @brief 在叶子里二分查找第一个 >= key 的条目下标（C++ lower_bound 语义）。 */
  auto LeafLowerBound(const LeafPage *leaf, const KeyType &key) const -> int {
    int lo = 0;
    int hi = leaf->GetSize();
    while (lo < hi) {
      int mid = lo + (hi - lo) / 2;
      if (comparator_(leaf->KeyAt(mid), key) < 0) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    return lo;
  }

  /**
   * @brief 加写锁从根下降到 key 所在的叶子，路径上的守卫存入 ctx.write_set_。
   *
   * 返回时 `ctx.write_set_.back()` 一定是目标叶子；若沿途遇到安全节点，
   * 其祖先的守卫（以及 ctx.header_page_）会被提前释放。
   *
   * @return 树为空（还没有根）时返回 false。
   */
  auto FindLeafForWrite(const KeyType &key, Context &ctx, Operation op) -> bool;

  /**
   * @brief 乐观路径：全程只加读锁下降，仅对目标叶子加一次写锁。
   *
   * 绝大多数插入/删除并不会让叶子分裂或下溢，为它们在整条路径上加写锁是巨大的浪费——
   * 那会让整棵树的一条竖线被独占，并发度骤降。乐观加锁先赌「这次操作只影响叶子」：
   *
   *   1. 一路读锁下降到叶子的父节点；
   *   2. 只给叶子加写锁；
   *   3. 检查叶子是否安全——安全就直接干活，不安全就整个放弃，回退到悲观路径重来。
   *
   * 赌输的代价是白跑一趟，但概率很低（只有恰好触发分裂/合并时才发生）。
   *
   * @return 叶子的写守卫；树为空或叶子不安全时返回 std::nullopt（调用方需回退）。
   */
  auto TryOptimisticLeaf(const KeyType &key, Operation op) -> std::optional<WritePageGuard>;

  /** @brief 把 (key, right_id) 插入到 left_id 的父节点中，必要时递归分裂直至新建根。 */
  void InsertIntoParent(Context &ctx, page_id_t left_id, const KeyType &key, page_id_t right_id);

  /** @brief 自底向上处理下溢：借用或合并，必要时塌缩树根。 */
  void HandleUnderflow(Context &ctx);

  /** @brief 定位最左侧的叶子页（Begin() 用）。 */
  auto FindLeftmostLeaf() -> std::optional<ReadPageGuard>;

  void ToGraph(page_id_t page_id, const BPlusTreePage *page, std::ofstream &out);

  void PrintTree(page_id_t page_id, const BPlusTreePage *page);

  auto ToPrintableBPlusTree(page_id_t root_id) -> PrintableBPlusTree;

  // member variable
  std::string index_name_;
  KeyComparator comparator_;
  std::vector<std::string> log;  // NOLINT
  int leaf_max_size_;
  int internal_max_size_;
  page_id_t header_page_id_;
};

/**
 * @brief for test only. PrintableBPlusTree is a printable B+ tree.
 * We first convert B+ tree into a printable B+ tree and the print it.
 */
struct PrintableBPlusTree {
  int size_;
  std::string keys_;
  std::vector<PrintableBPlusTree> children_;

  /**
   * @brief BFS traverse a printable B+ tree and print it into
   * into out_buf
   *
   * @param out_buf
   */
  void Print(std::ostream &out_buf) {
    std::vector<PrintableBPlusTree *> que = {this};
    while (!que.empty()) {
      std::vector<PrintableBPlusTree *> new_que;

      for (auto &t : que) {
        int padding = (t->size_ - t->keys_.size()) / 2;
        out_buf << std::string(padding, ' ');
        out_buf << t->keys_;
        out_buf << std::string(padding, ' ');

        for (auto &c : t->children_) {
          new_que.push_back(&c);
        }
      }
      out_buf << "\n";
      que = new_que;
    }
  }
};

}  // namespace bustub
