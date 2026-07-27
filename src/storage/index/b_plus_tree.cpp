//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// b_plus_tree.cpp
//
// Identification: src/storage/index/b_plus_tree.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "storage/index/b_plus_tree.h"
#include <optional>
#include <utility>
#include <vector>
#include "buffer/traced_buffer_pool_manager.h"
#include "storage/index/b_plus_tree_debug.h"

namespace bustub {

/*
 * ======================== B+ 树的并发协议：螃蟹锁 ========================
 *
 * 所有操作都从「头页」出发。头页里只有一个字段 root_page_id_，它的存在是为了
 * 给「根是谁」这件事本身加锁——否则两个线程同时让根分裂就会互相覆盖。
 *
 * 下降时按「螃蟹走路」的方式换手：
 *
 *      查询（读锁）           插入/删除（写锁）
 *      ┌───┐                  ┌───┐  ← 若子节点「安全」，这一层连同其上
 *      │根 │                  │根 │    全部立刻释放
 *      └─┬─┘                  └─┬─┘
 *        │ 先锁住子节点          │ 先锁住子节点
 *      ┌─▼─┐  再放开父节点     ┌─▼─┐  安全 ⇒ 放开全部祖先
 *      │中 │                  │中 │  不安全 ⇒ 继续攥着（分裂/合并可能上传）
 *      └─┬─┘                  └─┬─┘
 *      ┌─▼─┐                  ┌─▼─┐
 *      │叶 │                  │叶 │
 *      └───┘                  └───┘
 *
 * 「安全」的定义（IsSafe）：
 *   - 插入：GetSize() <  GetMaxSize()  ⇒ 插入后不会满，不会分裂。
 *   - 删除：GetSize() >  GetMinSize()  ⇒ 即便真的物理删掉一条也不会下溢。
 *
 * 这两个条件都是**保守**的：满足就一定安全，不满足未必真会传播。
 * 保守是必须的——判断错一次就会在没有父节点锁的情况下需要修改父节点。
 *
 * 为什么读操作不需要 Context？因为读锁下降时永远只需要同时持有两层，
 * 用一个局部变量的移动赋值就能表达；而写操作可能要回头修改任意多层祖先，
 * 所以要用 ctx.write_set_ 把整条路径的守卫攒起来。
 * ========================================================================
 */

FULL_INDEX_TEMPLATE_ARGUMENTS
BPLUSTREE_TYPE::BPlusTree(std::string name, page_id_t header_page_id, BufferPoolManager *buffer_pool_manager,
                          const KeyComparator &comparator, int leaf_max_size, int internal_max_size)
    : bpm_(std::make_shared<TracedBufferPoolManager>(buffer_pool_manager)),
      index_name_(std::move(name)),
      comparator_(std::move(comparator)),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size),
      header_page_id_(header_page_id) {
  WritePageGuard guard = bpm_->WritePage(header_page_id_);
  auto root_page = guard.AsMut<BPlusTreeHeaderPage>();
  root_page->root_page_id_ = INVALID_PAGE_ID;
}

/**
 * @brief Helper function to decide whether current b+tree is empty
 * @return Returns true if this B+ tree has no keys and values.
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::IsEmpty() const -> bool {
  // 「空」的判据是还没有根页。注意这与「没有任何存活的键」不是一回事：
  // 墓碑机制下，所有键都被删光之后树仍然保有若干页（里面全是墓碑），
  // 此时 IsEmpty() 返回 false，但 Begin().IsEnd() 为 true。
  ReadPageGuard guard = bpm_->ReadPage(header_page_id_);
  return guard.template As<BPlusTreeHeaderPage>()->root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * 内部辅助
 *****************************************************************************/

FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::FindLeafForWrite(const KeyType &key, Context &ctx, Operation op) -> bool {
  // 头页的写锁必须最先拿到并一直攥着，直到确认根不会变。
  ctx.header_page_ = bpm_->WritePage(header_page_id_);
  auto *header = ctx.header_page_->template AsMut<BPlusTreeHeaderPage>();
  ctx.root_page_id_ = header->root_page_id_;

  if (ctx.root_page_id_ == INVALID_PAGE_ID) {
    return false;  // 空树，调用方自行决定是建根还是直接返回。
  }

  auto guard = bpm_->WritePage(ctx.root_page_id_);

  // ==== P2 STEP 18: 根节点的「安全」判据和别人不一样 ====
  // 头页写锁只需要在「根本身会被换掉」时保留。而根不受最小容量约束，
  // 它换掉的条件与普通节点不同：
  //   - 插入：根满了会分裂并长出新根 ⇒ 判据同普通节点（size < max）。
  //   - 删除：叶子根在最后一条记录被物理删掉时，整棵树要清空（root_page_id_ 置无效），
  //           所以条目数 > 1 才安全；
  //           内部根只在孩子数掉到 1 时才塌缩。一次合并最多让它少一个孩子，
  //           所以孩子数 > 2 就绝对安全。
  //
  // 这里如果偷懒复用 IsSafe（内部根的下限是 max/2，通常远小于 2），
  // 就会在 max_size 较小时提前放掉头页锁，等到真要塌缩时手上已经没有它了 —— 直接崩溃。
  const auto *root_page = guard.template As<BPlusTreePage>();
  const bool root_safe = (op == Operation::INSERT)
                             ? root_page->GetSize() < root_page->GetMaxSize()
                             : (root_page->IsLeafPage() ? root_page->GetSize() > 1 : root_page->GetSize() > 2);
  if (root_safe) {
    // 根一定不会变 ⇒ 头页的锁可以立刻放掉，
    // 别的线程就能同时对树的其它部分做写操作了。
    ctx.header_page_ = std::nullopt;
  }

  while (!guard.template As<BPlusTreePage>()->IsLeafPage()) {
    const auto *internal = guard.template As<InternalPage>();
    const page_id_t child_id = internal->ValueAt(internal->ChildIndexFor(key, comparator_));

    ctx.write_set_.push_back(std::move(guard));
    // 先锁住孩子，再决定要不要放开祖先——顺序反了就会出现「谁都没锁住」的空窗，
    // 期间另一个线程可能把这棵子树整个改掉。
    guard = bpm_->WritePage(child_id);

    if (IsSafe(guard.template As<BPlusTreePage>(), op)) {
      ctx.write_set_.clear();           // 一次性释放全部祖先的写锁
      ctx.header_page_ = std::nullopt;  // 根也不会变了
    }
  }

  ctx.write_set_.push_back(std::move(guard));  // 队尾恒为目标叶子
  return true;
}

FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::TryOptimisticLeaf(const KeyType &key, Operation op) -> std::optional<WritePageGuard> {
  // ==== P2 STEP 17: 乐观加锁 ====
  // `parent_guard` 始终持有「当前节点的父亲」的读锁。它一开始是头页——
  // 因为根节点的"父亲"就是记录着 root_page_id_ 的那一页。
  ReadPageGuard parent_guard = bpm_->ReadPage(header_page_id_);
  page_id_t child_id = parent_guard.template As<BPlusTreeHeaderPage>()->root_page_id_;
  if (child_id == INVALID_PAGE_ID) {
    return std::nullopt;  // 空树，只能走悲观路径去建根。
  }

  auto child_guard = bpm_->ReadPage(child_id);
  while (!child_guard.template As<BPlusTreePage>()->IsLeafPage()) {
    parent_guard = std::move(child_guard);  // 下沉一层，旧的父守卫在此释放
    const auto *internal = parent_guard.template As<InternalPage>();
    child_id = internal->ValueAt(internal->ChildIndexFor(key, comparator_));
    child_guard = bpm_->ReadPage(child_id);
  }

  // 放掉叶子的读锁，改申请写锁。
  //
  // 这中间存在一个窗口，但它是**安全**的：任何会让这一页消失或改变归属的操作
  // （分裂、合并、删页）都必须先拿到其父节点的写锁，而父节点的读锁还在我们手里，
  // 所以它们全都被挡住了。窗口内只可能有别的线程修改页内容，而我们拿到写锁后
  // 会重新读取，不会用到过期数据。
  child_guard.Drop();
  auto leaf_guard = bpm_->WritePage(child_id);
  const auto *leaf = leaf_guard.template As<LeafPage>();

  bool safe;
  if (op == Operation::INSERT) {
    safe = leaf->GetSize() < leaf->GetMaxSize();  // 还有空位就不会分裂
  } else {
    // 删除有两条安全理由，满足其一即可：
    //   a) 墓碑队列没满 ⇒ 这次只会记一笔墓碑，size_ 根本不变，绝不可能下溢；
    //   b) 条目数高于下限 ⇒ 即便真的兑现一笔物理删除也还够。
    safe = leaf->NumTombstones() < LeafPage::TombCapacity() || leaf->GetSize() > leaf->GetMinSize();
  }
  if (!safe) {
    return std::nullopt;  // 赌输了，交给悲观路径重来一遍。
  }
  return leaf_guard;
}

FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::FindLeftmostLeaf() -> std::optional<ReadPageGuard> {
  ReadPageGuard header_guard = bpm_->ReadPage(header_page_id_);
  const page_id_t root_id = header_guard.template As<BPlusTreeHeaderPage>()->root_page_id_;
  if (root_id == INVALID_PAGE_ID) {
    return std::nullopt;
  }

  auto guard = bpm_->ReadPage(root_id);
  header_guard.Drop();
  while (!guard.template As<BPlusTreePage>()->IsLeafPage()) {
    // 一路取第 0 个孩子就是最左路径。
    guard = bpm_->ReadPage(guard.template As<InternalPage>()->ValueAt(0));
  }
  return guard;
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
/**
 * @brief Return the only value that associated with input key
 *
 * This method is used for point query
 *
 * @param key input key
 * @param[out] result vector that stores the only value that associated with input key, if the value exists
 * @return : true means key exists
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetValue(const KeyType &key, std::vector<ValueType> *result) -> bool {
  // ==== P2 STEP 11: 读路径的螃蟹锁 ====
  // 读操作只需同时持有两层：先拿到孩子的读锁，再放开父亲的。
  // 这里用移动赋值 `guard = bpm_->ReadPage(child)` 就天然实现了正确顺序——
  // 新守卫在临时对象里构造（已加锁），赋值时才把旧守卫 Drop 掉。
  ReadPageGuard header_guard = bpm_->ReadPage(header_page_id_);
  const page_id_t root_id = header_guard.template As<BPlusTreeHeaderPage>()->root_page_id_;
  if (root_id == INVALID_PAGE_ID) {
    return false;
  }

  auto guard = bpm_->ReadPage(root_id);
  header_guard.Drop();

  while (!guard.template As<BPlusTreePage>()->IsLeafPage()) {
    const auto *internal = guard.template As<InternalPage>();
    guard = bpm_->ReadPage(internal->ValueAt(internal->ChildIndexFor(key, comparator_)));
  }

  const auto *leaf = guard.template As<LeafPage>();
  const int index = LeafLowerBound(leaf, key);
  if (index >= leaf->GetSize() || comparator_(leaf->KeyAt(index), key) != 0) {
    return false;  // 键根本不存在。
  }
  if (leaf->IsTombstoned(index)) {
    // 条目物理上还在页里，但已被逻辑删除。对上层而言它就是不存在。
    return false;
  }

  result->push_back(leaf->ValueAt(index));
  return true;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
/**
 * @brief Insert constant key & value pair into b+ tree
 *
 * if current tree is empty, start new tree, update root page id and insert
 * entry; otherwise, insert into leaf page.
 *
 * @param key the key to insert
 * @param value the value associated with key
 * @return: since we only support unique key, if user try to insert duplicate
 * keys return false; otherwise, return true.
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Insert(const KeyType &key, const ValueType &value) -> bool {
  // ---- STEP 12.0: 先赌一把「只影响叶子」----
  // OptimisticInsertTest 会精确统计：往一个还有空位的叶子里插一条，
  // 必须只发生 **1 次** WritePage。若沿途全部加写锁，这个数字会是路径长度。
  if (auto leaf_guard = TryOptimisticLeaf(key, Operation::INSERT); leaf_guard.has_value()) {
    auto *leaf = leaf_guard->template AsMut<LeafPage>();
    const int index = LeafLowerBound(leaf, key);
    if (index < leaf->GetSize() && comparator_(leaf->KeyAt(index), key) == 0) {
      if (leaf->IsTombstoned(index)) {
        leaf->SetValueAt(index, value);
        leaf->ClearTombstone(index);
        return true;
      }
      return false;
    }
    leaf->InsertAt(index, key, value);
    return true;
  }

  Context ctx;

  // ---- STEP 12.1: 空树 → 直接建一个叶子当根 ----
  if (!FindLeafForWrite(key, ctx, Operation::INSERT)) {
    const page_id_t root_id = bpm_->NewPage();
    auto root_guard = bpm_->WritePage(root_id);
    auto *root = root_guard.template AsMut<LeafPage>();
    root->Init(leaf_max_size_);
    root->InsertAt(0, key, value);
    // 只有在持有头页写锁的前提下改 root_page_id_ 才是安全的，
    // FindLeafForWrite 在空树分支上刻意没有释放它。
    ctx.header_page_->template AsMut<BPlusTreeHeaderPage>()->root_page_id_ = root_id;
    return true;
  }

  auto *leaf = ctx.write_set_.back().template AsMut<LeafPage>();
  const int index = LeafLowerBound(leaf, key);

  // ---- STEP 12.2: 键已存在的两种情况 ----
  if (index < leaf->GetSize() && comparator_(leaf->KeyAt(index), key) == 0) {
    if (leaf->IsTombstoned(index)) {
      // 「复活」：这个键之前被逻辑删除了，现在原地改值并撤销墓碑。
      // 这是墓碑机制最大的红利——删后重插退化成 O(1) 的原地更新，
      // 既不用挪动数组，也完全不触碰树结构。
      leaf->SetValueAt(index, value);
      leaf->ClearTombstone(index);
      return true;
    }
    return false;  // 本实现只支持唯一键。
  }

  // ---- STEP 12.3: 有空位，直接插 ----
  if (leaf->GetSize() < leaf->GetMaxSize()) {
    leaf->InsertAt(index, key, value);
    return true;
  }

  // ---- STEP 12.4: 叶子已满 → 先对半切开，再把新键插进该去的那一半 ----
  // 顺序很重要：**先分裂、后插入**，而不是「先插入撑到 max+1 再切」。
  // 后者会临时写到 key_array_[max_size]，当 max_size 等于页容量上限时直接越界。
  // 这个顺序也被测试锁死了（见 docs/lab-notes/03-P2-index.md 规格 F）。
  const page_id_t new_leaf_id = bpm_->NewPage();
  auto new_guard = bpm_->WritePage(new_leaf_id);
  auto *new_leaf = new_guard.template AsMut<LeafPage>();
  new_leaf->Init(leaf_max_size_);

  const int split_at = leaf->GetSize() / 2;
  leaf->MoveHalfTo(new_leaf, split_at);

  // 维护叶子链表：新页插到当前页之后。
  new_leaf->SetNextPageId(leaf->GetNextPageId());
  leaf->SetNextPageId(new_leaf_id);

  if (index < split_at) {
    leaf->InsertAt(index, key, value);
  } else {
    new_leaf->InsertAt(index - split_at, key, value);
  }

  // 分隔键取右半页的第一个键。叶子分裂是**复制**中间键上去（不是移走），
  // 因为叶子层必须保有全部数据——这正是 B+ 树与 B 树的分野。
  const KeyType separator = new_leaf->KeyAt(0);
  const page_id_t left_id = ctx.write_set_.back().GetPageId();
  new_guard.Drop();  // 父节点只需要 page id，尽早放锁

  InsertIntoParent(ctx, left_id, separator, new_leaf_id);
  return true;
}

FULL_INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::InsertIntoParent(Context &ctx, page_id_t left_id, const KeyType &key, page_id_t right_id) {
  ctx.write_set_.pop_back();  // 当前层已处理完，弹出让队尾变成父节点

  // ---- 情况 A：刚分裂的是根 → 树长高一层 ----
  if (ctx.write_set_.empty()) {
    // ==== P2 STEP 13: B+ 树是唯一「从叶子往上长」的树 ====
    // 树高只在根分裂时增加，而且是所有叶子同时深一层，
    // 这就是 B+ 树永远完美平衡、不需要任何旋转操作的原因。
    const page_id_t new_root_id = bpm_->NewPage();
    auto root_guard = bpm_->WritePage(new_root_id);
    auto *root = root_guard.template AsMut<InternalPage>();
    root->Init(internal_max_size_);
    root->SetSize(2);
    root->SetValueAt(0, left_id);  // 下标 0 的键是无效槽位，不用写
    root->SetKeyAt(1, key);
    root->SetValueAt(1, right_id);
    ctx.header_page_->template AsMut<BPlusTreeHeaderPage>()->root_page_id_ = new_root_id;
    return;
  }

  auto *parent = ctx.write_set_.back().template AsMut<InternalPage>();
  const int left_index = parent->ValueIndex(left_id);

  // ---- 情况 B：父节点有空位 ----
  if (parent->GetSize() < parent->GetMaxSize()) {
    parent->InsertAt(left_index + 1, key, right_id);
    return;
  }

  // ---- 情况 C：父节点也满了 → 同样先分裂再插入，然后继续向上递归 ----
  const page_id_t new_internal_id = bpm_->NewPage();
  auto new_guard = bpm_->WritePage(new_internal_id);
  auto *new_internal = new_guard.template AsMut<InternalPage>();
  new_internal->Init(internal_max_size_);

  const int split_at = (parent->GetSize() + 1) / 2;
  parent->MoveHalfTo(new_internal, split_at);

  const int insert_pos = left_index + 1;
  if (insert_pos < split_at) {
    parent->InsertAt(insert_pos, key, right_id);
  } else {
    new_internal->InsertAt(insert_pos - split_at, key, right_id);
  }

  // 内部节点分裂是把中间键**上移**（而非复制）：右半页的第 0 个键升到父节点后，
  // 它在右半页里就退化成那个无效槽位，不再参与比较。
  const KeyType up_key = new_internal->KeyAt(0);
  const page_id_t parent_id = ctx.write_set_.back().GetPageId();
  new_guard.Drop();

  InsertIntoParent(ctx, parent_id, up_key, new_internal_id);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
/**
 * @brief Delete key & value pair associated with input key
 * If current tree is empty, return immediately.
 * If not, User needs to first find the right leaf page as deletion target, then
 * delete entry from leaf page. Remember to deal with redistribute or merge if
 * necessary.
 *
 * @param key input key
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::Remove(const KeyType &key) {
  // 同样先走乐观路径。有了墓碑机制，"安全"的判定还多了一条捷径：
  // 只要墓碑队列没满，这次删除根本不改变 size_，天然安全。
  if (auto leaf_guard = TryOptimisticLeaf(key, Operation::REMOVE); leaf_guard.has_value()) {
    auto *leaf = leaf_guard->template AsMut<LeafPage>();
    const int index = LeafLowerBound(leaf, key);
    if (index >= leaf->GetSize() || comparator_(leaf->KeyAt(index), key) != 0 || leaf->IsTombstoned(index)) {
      return;
    }
    leaf->MarkDeleted(index);
    return;
  }

  // Declaration of context instance.
  Context ctx;

  if (!FindLeafForWrite(key, ctx, Operation::REMOVE)) {
    return;  // 空树。
  }

  auto *leaf = ctx.write_set_.back().template AsMut<LeafPage>();
  const int index = LeafLowerBound(leaf, key);

  // 键不存在，或者已经被逻辑删除过一次 —— 两种情况都视为无事发生（幂等）。
  if (index >= leaf->GetSize() || comparator_(leaf->KeyAt(index), key) != 0 || leaf->IsTombstoned(index)) {
    return;
  }

  // ==== P2 STEP 14: 删除的两级代价 ====
  // MarkDeleted 返回本次真正物理删掉的条目数：
  //   - 返回 0：只是在墓碑队列里记了一笔，size_ 没变 ⇒ 绝不可能下溢，直接结束。
  //     这是绝大多数删除走的路径，代价是 O(1)，完全不碰树结构。
  //   - 返回 1：墓碑队列满了，最老的那笔待删除被兑现，size_ 减一 ⇒ 可能下溢。
  // 换句话说，昂贵的借用/合并被摊薄到了每 NumTombs 次删除才发生一次。
  if (leaf->MarkDeleted(index) == 0) {
    return;
  }

  // 根节点不受最小容量约束：整棵树只有一页时，它爱剩几条剩几条 ——
  // 但一条不剩时要把整棵树清空。
  if (ctx.IsRootPage(ctx.write_set_.back().GetPageId())) {
    if (leaf->GetSize() == 0) {
      // ==== P2 STEP 19: 树彻底变空 ====
      // 注意这只在墓碑容量为 0（经典 B+ 树语义）时才可能发生：
      // 有墓碑时，被删的条目仍以墓碑形式留在页里，size_ 不会归零，
      // 树因此保持「非空但可迭代为空」的状态（TombstoneBasicTest 依赖这一点）。
      const page_id_t root_id = ctx.write_set_.back().GetPageId();
      ctx.header_page_->template AsMut<BPlusTreeHeaderPage>()->root_page_id_ = INVALID_PAGE_ID;
      ctx.write_set_.pop_back();  // DeletePage 要求 pin 归零
      bpm_->DeletePage(root_id);
    }
    return;
  }
  if (leaf->GetSize() >= leaf->GetMinSize()) {
    return;
  }

  HandleUnderflow(ctx);
}

FULL_INDEX_TEMPLATE_ARGUMENTS
void BPLUSTREE_TYPE::HandleUnderflow(Context &ctx) {
  // 循环而非递归：下溢可能一层层往上传播，每轮处理一层。
  // 进入循环时，ctx.write_set_.back() 是**已经下溢**的那个节点，
  // 它的前一个元素是其父节点（螃蟹锁保证了父节点的写锁一定还握在手里）。
  while (ctx.write_set_.size() >= 2) {
    auto node_guard = std::move(ctx.write_set_.back());
    ctx.write_set_.pop_back();

    auto *parent = ctx.write_set_.back().template AsMut<InternalPage>();
    const page_id_t node_id = node_guard.GetPageId();
    const int node_index = parent->ValueIndex(node_id);
    auto *node = node_guard.template AsMut<BPlusTreePage>();

    // ---- STEP 15.1: 选一个兄弟 ----
    // 优先取左兄弟；自己就是最左孩子时才取右兄弟。
    // 只考虑**亲兄弟**（同一个父节点下的相邻孩子）——跨父节点的"堂兄弟"虽然
    // 在叶子链表上相邻，但借用/合并它需要修改两个父节点，代价和复杂度都不划算。
    const bool sibling_is_left = node_index > 0;
    const int sibling_index = sibling_is_left ? node_index - 1 : node_index + 1;
    auto sibling_guard = bpm_->WritePage(parent->ValueAt(sibling_index));
    auto *sibling = sibling_guard.template AsMut<BPlusTreePage>();

    // 父节点中分隔这两个孩子的键，永远位于「右边那个孩子」的下标处。
    const int separator_index = sibling_is_left ? node_index : sibling_index;

    // ---- STEP 15.2: 合并还是借用 ----
    // 判据：两页装得下就合并，装不下才借用。
    // 「优先合并」而非「优先借用」是被 TombstoneBorrowTest 锁死的选择
    // （见 docs/lab-notes/03-P2-index.md 规格 F）。它也更符合直觉：
    // 合并能减少一个页、降低树的稀疏度，而借用只是把问题挪个位置。
    if (node->GetSize() + sibling->GetSize() <= node->GetMaxSize()) {
      // ========== 合并 ==========
      // 统一约定：永远把右边那页并入左边那页，然后删掉右页。
      page_id_t victim_id;
      if (sibling_is_left) {
        if (node->IsLeafPage()) {
          node_guard.template AsMut<LeafPage>()->MoveAllTo(sibling_guard.template AsMut<LeafPage>());
        } else {
          node_guard.template AsMut<InternalPage>()->MoveAllTo(sibling_guard.template AsMut<InternalPage>(),
                                                               parent->KeyAt(separator_index));
        }
        victim_id = node_id;
      } else {
        if (node->IsLeafPage()) {
          sibling_guard.template AsMut<LeafPage>()->MoveAllTo(node_guard.template AsMut<LeafPage>());
        } else {
          sibling_guard.template AsMut<InternalPage>()->MoveAllTo(node_guard.template AsMut<InternalPage>(),
                                                                  parent->KeyAt(separator_index));
        }
        victim_id = sibling_guard.GetPageId();
      }
      // 父节点里指向被吞并页的那一项（连同分隔键）要一起去掉。
      parent->RemoveAt(separator_index);

      // DeletePage 要求 pin_count 归零，所以必须先放掉两把守卫。
      node_guard.Drop();
      sibling_guard.Drop();
      bpm_->DeletePage(victim_id);
    } else {
      // ========== 借用 ==========
      // 借用只是在两个兄弟之间搬一条数据、顺带订正父节点的分隔键，
      // 父节点的**条目数不变**，因此不可能继续向上传播，处理完直接收工。
      if (node->IsLeafPage()) {
        auto *node_leaf = node_guard.template AsMut<LeafPage>();
        auto *sibling_leaf = sibling_guard.template AsMut<LeafPage>();
        if (sibling_is_left) {
          sibling_leaf->MoveLastToFrontOf(node_leaf);
          parent->SetKeyAt(separator_index, node_leaf->KeyAt(0));
        } else {
          sibling_leaf->MoveFirstToEndOf(node_leaf);
          parent->SetKeyAt(separator_index, sibling_leaf->KeyAt(0));
        }
      } else {
        auto *node_internal = node_guard.template AsMut<InternalPage>();
        auto *sibling_internal = sibling_guard.template AsMut<InternalPage>();
        if (sibling_is_left) {
          // 被搬走的那个孩子原本的分隔键，正是它到了新家之后要顶上去的父分隔键。
          const KeyType moved_key = sibling_internal->KeyAt(sibling_internal->GetSize() - 1);
          sibling_internal->MoveLastToFrontOf(node_internal, parent->KeyAt(separator_index));
          parent->SetKeyAt(separator_index, moved_key);
        } else {
          const KeyType moved_key = sibling_internal->KeyAt(1);
          sibling_internal->MoveFirstToEndOf(node_internal, parent->KeyAt(separator_index));
          parent->SetKeyAt(separator_index, moved_key);
        }
      }
      return;
    }

    // ---- STEP 15.3: 合并之后回头看父节点 ----
    //
    // 这里必须用 IsRootPage 判断，**不能**用 `write_set_.size() == 1`。
    // 螃蟹锁会在遇到安全节点时清空 write_set_，所以队首往往只是
    // 「我们还攥着的最高一层」，而不是真正的树根。用长度判断会把一个普通的
    // 中间节点误当成根，进而去访问早已释放的头页守卫 —— 直接崩溃。
    // Context::root_page_id_ 正是为这个判断而存在的。
    if (ctx.IsRootPage(ctx.write_set_.back().GetPageId())) {
      // 父节点就是根。根的规则不同：它不看 GetMinSize()，
      // 只要求「内部根至少有 2 个孩子」。只剩 1 个孩子说明树该矮一层了。
      if (parent->GetSize() == 1) {
        // ==== P2 STEP 16: 根塌缩 ====
        // 唯一的孩子直接升格为新根。这是「树长高」的逆操作，
        // 同样保证所有叶子的深度整齐地减一。
        const page_id_t old_root_id = ctx.write_set_.back().GetPageId();
        ctx.header_page_->template AsMut<BPlusTreeHeaderPage>()->root_page_id_ = parent->ValueAt(0);
        ctx.write_set_.pop_back();  // 放掉旧根的守卫，才能删它
        bpm_->DeletePage(old_root_id);
      }
      return;
    }

    if (parent->GetSize() >= parent->GetMinSize()) {
      return;  // 父节点还够肥，传播到此为止。
    }
    // 父节点也下溢了，下一轮把它当作新的「当前节点」继续处理。
    //
    // 循环条件 `write_set_.size() >= 2` 一定还能满足：能走到这里说明父节点下溢，
    // 而下降时判定为「安全」的节点绝不会下溢（安全 ⇒ size > minsize ⇒ 减一后仍 >= minsize），
    // 所以队首那个被保留下来的节点之上必然还有未释放的祖先。
  }
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
/**
 * @brief Input parameter is void, find the leftmost leaf page first, then construct
 * index iterator
 *
 * You may want to implement this while implementing Task #3.
 *
 * @return : index iterator
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin() -> INDEXITERATOR_TYPE {
  auto guard = FindLeftmostLeaf();
  if (!guard.has_value()) {
    return End();
  }
  // 从下标 0 开始；构造函数内部的 SkipDeleted() 会自动跳过开头的墓碑，
  // 甚至在「所有键都被删光、只剩墓碑」时一路走到末尾变成 End()。
  return INDEXITERATOR_TYPE(bpm_, std::move(*guard), 0);
}

/**
 * @brief Input parameter is low key, find the leaf page that contains the input key
 * first, then construct index iterator
 * @return : index iterator
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::Begin(const KeyType &key) -> INDEXITERATOR_TYPE {
  // 定位到 key 应当所在的叶子，起点取 lower_bound —— 即「第一个 >= key 的条目」。
  // 这样即使 key 本身不存在，返回的也是范围扫描应有的起点。
  ReadPageGuard header_guard = bpm_->ReadPage(header_page_id_);
  const page_id_t root_id = header_guard.template As<BPlusTreeHeaderPage>()->root_page_id_;
  if (root_id == INVALID_PAGE_ID) {
    return End();
  }

  auto guard = bpm_->ReadPage(root_id);
  header_guard.Drop();
  while (!guard.template As<BPlusTreePage>()->IsLeafPage()) {
    const auto *internal = guard.template As<InternalPage>();
    guard = bpm_->ReadPage(internal->ValueAt(internal->ChildIndexFor(key, comparator_)));
  }

  const int index = LeafLowerBound(guard.template As<LeafPage>(), key);
  return INDEXITERATOR_TYPE(bpm_, std::move(guard), index);
}

/**
 * @brief Input parameter is void, construct an index iterator representing the end
 * of the key/value pair in the leaf node
 * @return : index iterator
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::End() -> INDEXITERATOR_TYPE {
  // 尾后迭代器不持有任何页守卫，因此不占帧、也不加锁。
  return INDEXITERATOR_TYPE();
}

/**
 * @return Page id of the root of this tree
 *
 * You may want to implement this while implementing Task #3.
 */
FULL_INDEX_TEMPLATE_ARGUMENTS
auto BPLUSTREE_TYPE::GetRootPageId() -> page_id_t {
  // 必须通过头页读，而不能缓存在树对象里：并发环境下根随时可能因分裂或塌缩而更换，
  // 头页的读锁保证我们看到的是一个一致的值。这正是「为什么要有头页」的答案。
  ReadPageGuard guard = bpm_->ReadPage(header_page_id_);
  return guard.template As<BPlusTreeHeaderPage>()->root_page_id_;
}

template class BPlusTree<GenericKey<4>, RID, GenericComparator<4>>;

template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>, 3>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>, 2>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>, 1>;
template class BPlusTree<GenericKey<8>, RID, GenericComparator<8>, -1>;

template class BPlusTree<GenericKey<16>, RID, GenericComparator<16>>;

template class BPlusTree<GenericKey<32>, RID, GenericComparator<32>>;

template class BPlusTree<GenericKey<64>, RID, GenericComparator<64>>;

}  // namespace bustub
