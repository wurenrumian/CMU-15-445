# P0 · 并发跳表（SkipList）

**涉及文件**：`src/include/primer/skiplist.h`、`src/primer/skiplist.cpp`
**测试**：`test/primer/skiplist_test.cpp`（9 个用例）

---

## 1. 为什么数据库课要先写跳表

P0 名义上是 C++ 热身，但选跳表不是随意的。跳表是**有序索引**的一种，和 P2 的 B+ 树解决同一类问题：

| | 跳表 | B+ 树 |
|---|---|---|
| 有序性 | 底层是有序链表 | 叶子层是有序链表 |
| 加速手段 | 概率性的多级"快车道" | 确定性的多级索引页 |
| 复杂度 | 期望 O(log n) | 最坏 O(log n) |
| 平衡方式 | 掷骰子，无需再平衡 | 分裂 / 合并 / 借用 |
| 适用介质 | 内存（指针跳转随机访问） | 磁盘（节点=页，减少 I/O 次数） |

写完跳表再写 B+ 树，会很明显地感受到：**B+ 树的全部复杂度都来自"要放在磁盘上"这一个约束**。
跳表用随机数逃避了再平衡，代价是不能保证最坏复杂度，而且指针跳来跳去对磁盘极不友好。

同时 P0 建立了后续所有 Project 都要用的两个直觉：
1. **读写锁（`std::shared_mutex`）**：读多写少的结构必须让读操作并发。
2. **迭代式释放**：链式数据结构的析构不能靠编译器递归，否则栈会爆。

---

## 2. 数据结构

```
 level 3           header ───────────────────────────────▶ 19 ──▶ nil
 level 2           header ──▶ 0 ──▶ 4 ──▶ ... ──▶ 12 ────▶ 19 ──▶ nil
 level 1           header ──▶ 0 ──▶ 1 ──▶ 2 ──▶ 3 ──▶ ... ──▶ 19 ──▶ nil
                    (第 0 层是完整的有序链表)
```

- 每个节点有 `links_` 数组，长度 = 该节点的**高度**，由 `RandomHeight()` 掷骰决定。
- 高度服从几何分布：以 1/4 的概率再升一层（Pugh 论文的 branching factor = 4）。
  因此第 k 层的节点数期望是总数的 $4^{-k}$，查找期望走 O(log₄ n) 步。
- `header_` 是**哨兵节点**，高度固定为 `MaxHeight`。

### 关键设计点

**① 哨兵头节点必须一次性开满 `MaxHeight` 条链接**（`STEP 1`）

```cpp
header_(std::make_shared<SkipNode>(MaxHeight))
```

两个理由：
- 新节点的随机高度可能超过当前 `height_`，届时直接写 `header_->links_[level]` 即可，
  不需要给 vector 扩容。扩容会让其它线程持有的引用失效。
- 骨架给的 `Drop()` 固定循环 `i < MaxHeight` 次访问 `header_->links_[i]`，
  链接数不足会直接越界。

**② 节点高度 = `links_.size()`**（`STEP 2`、`STEP 8`）

不额外存 `height_` 字段，消除了"两处状态可能不一致"的整类 bug。
更重要的是：**`links_` 长度创建后永不改变**，所以任何修改都只是改写已有槽位，
不会触发 vector 搬迁，其它线程持有的节点指针永远有效。

---

## 3. 三个操作的共同骨架

查找、插入、删除的第一步完全相同：**自顶向下，逐层右行，记录每层的前驱**。

```cpp
auto FindLastLessThan(const K &key, std::vector<std::shared_ptr<SkipNode>> *prevs)
    -> std::shared_ptr<SkipNode> {
  auto curr = header_;
  for (level = height_ - 1; level >= 0; --level) {
    while (curr->Next(level) != nullptr && compare_(curr->Next(level)->Key(), key)) {
      curr = curr->Next(level);          // 本层继续右行
    }
    if (prevs) (*prevs)[level] = curr;   // 记录本层前驱
  }                                       // 下沉一层，起点仍是 curr
  return curr;
}
```

结束时 `curr` 是第 0 层最后一个 `key < 目标` 的节点，于是：

- **查找**：`curr->Next(0)` 要么等价于目标，要么目标不存在。
- **插入**：新节点在第 `level` 层挂到 `prevs[level]` 后面。
- **删除**：把 `prevs[level]` 的链接改指向被删节点的后继。

### 为什么给 `Contains` 留了不填 `prevs` 的快速路径

`ConcurrentReadTest` 有 8 线程 × 80 万次查找 ≈ **640 万次调用**。
每次调用若都构造一个 `std::vector<shared_ptr>(MaxHeight)`，就是 640 万次堆分配
外加 2×14×640万 次 `shared_ptr` 原子引用计数操作。传 `nullptr` 走标量路径可以完全省掉。

---

## 4. `Insert` 的三个坑

### 坑 1：`RandomHeight()` 的调用时机（`STEP 5.3`）

`IntegrityCheckTest` 用固定随机种子写死了 20 个节点各自的期望高度。这意味着：

> `RandomHeight()` 在每次 `Insert` 中必须**恰好被调用一次**，而且只在"确定要真正插入"之后调用。

多调一次或少调一次，随机数流就整体错位，之后所有节点的高度全错。
比如"先掷高度、再检查重复"的写法，在重复插入时会白白消耗一个随机数。

### 坑 2：链接的赋值顺序（`STEP 5.4`）

```cpp
node->SetNext(level, prevs[level]->Next(level));   // ① 新节点先指向后继
prevs[level]->SetNext(level, node);                // ② 前驱再指向新节点
```

顺序反了的话，中间会出现一个瞬间：前驱指向了新节点，而新节点还没指向后继——
此时从 `header_` 出发遍历会**丢失后半条链表**。虽然本实现用全局写锁保护，
外部观察不到这个中间态，但保持这个顺序是无锁跳表能成立的根本前提，值得养成习惯。

### 坑 3：`prevs` 预填 `header_`

```cpp
std::vector<std::shared_ptr<SkipNode>> prevs(MaxHeight, header_);
```

`FindLastLessThan` 只会写入 `[0, height_)` 这些层。若新节点的高度超过 `height_`，
更高层的前驱天然就是 `header_`（那些层此前从未有过节点），预填之后就不必再补写。

---

## 5. `Erase` 的两个细节

**① 析构不会连锁**（`STEP 6.3`）

摘除目标节点后，`target->links_` 里仍然存着指向后继的 `shared_ptr`。
但那些后继同时被它们各自的前驱持有，引用计数不为 0，所以 `target` 析构时不会
连锁析构整条链表。

**② 收缩空的高层**（`STEP 6.4`）

```cpp
while (height_ > 1 && header_->Next(height_ - 1) == nullptr) { --height_; }
```

删掉最高的节点后顶层会变空。不收缩虽然不影响正确性（查找会在空层白走一遍），
但会让每次查找多下沉几层。维持"表非空时 `header_->links_[height_-1]` 非空"这个不变量。

---

## 6. 并发：这一题真正想教的东西

### 锁的选择直接决定测试通过与否

| 函数 | 锁 | 理由 |
|---|---|---|
| `Insert` / `Erase` / `Clear` | `std::unique_lock`（独占） | 修改链表结构 |
| `Contains` / `Size` / `Empty` | `std::shared_lock`（共享） | 只读，必须允许读读并发 |

`Contains` 若误用 `unique_lock`，**功能完全正确但测试会超时**：
`ConcurrentReadTest` 的 8 个线程会被完全串行化。这正是该测试的注释所警告的
"You will see a timeout if your reads cannot share access"。

`Size()` 只读一个 `size_t`，看起来"原子"，但仍然必须加锁——
与并发的 `Insert` 之间构成 data race，是标准定义的未定义行为，ASAN/TSAN 会直接报错。

### 本实现的并发粒度

这是一把**全局读写锁**保护整个跳表。工业级实现（如 LevelDB / RocksDB 的 MemTable）
会用无锁跳表：`links_` 用 `std::atomic<Node*>`，插入靠 CAS，删除靠标记。
本实现的写操作完全串行，是刻意的简化——P0 的目标是理解"读写分离"这一层，
真正的细粒度并发留到 P2 的 B+ 树螃蟹锁（latch crabbing）。

---

## 7. 迭代式析构：为什么不能交给编译器

骨架提供的 `Drop()` 值得单独一讲：

```cpp
for (size_t i = 0; i < MaxHeight; i++) {
  auto curr = std::move(header_->links_[i]);
  while (curr != nullptr) {
    curr = std::move(curr->links_[i]);   // 把下一个"接管"过来，同时把源置空
  }
}
```

如果直接写 `header_->links_[0] = nullptr`，会发生：
第 1 个节点析构 → 析构其 `links_[0]` → 第 2 个节点析构 → …
这是一条**深度等于链表长度的递归**。`ConcurrentReadTest` 有 80 万个节点，
栈必然溢出。`Drop()` 用循环把每个节点先从链上摘下来（`std::move` 后源指针置空，
引用计数归零，节点独立析构），栈深度恒为 O(1)。

`Clear()` 直接复用 `Drop()`：它结束后 `header_->links_` 全为 `nullptr`，
头节点本身可以继续复用，只需把 `size_` 归零、`height_` 退回 1。

---

## 8. 测试结果

```
[==========] Running 9 tests from 1 test suite.
[  PASSED  ] 9 tests.

IntegrityCheckTest            (0 ms)   ← 验证每层链接与节点高度完全正确
InsertContainsTest1/2         (0 ms)
InsertAndEraseTest            (0 ms)
EraseNonExistingElement       (0 ms)
ConcurrentInsertTest          (9 ms)   ← 10 线程 × 100 次插入
ConcurrentEraseTest           (0 ms)
ConcurrentInsertAndEraseTest  (2 ms)
ConcurrentReadTest         (7533 ms)   ← 8 线程 × 80 万次查找，481% CPU 利用率
```

`ConcurrentReadTest` 的 481% CPU 利用率（约 4.8 核并行）直接证明了共享锁生效——
若是独占锁，利用率会贴近 100%（完全串行）并触发超时。

`make format` 与 `make check-lint` 均通过。

---

## 9. 一句话小结

跳表用**随机化**换掉了平衡树的再平衡逻辑；用**哨兵节点**消掉了链表头的特判；
用**读写锁**换来了读的并发。这三个套路在后面的 Project 里会反复出现——
只不过 P2 的 B+ 树需要在磁盘约束下把它们全部重做一遍。
