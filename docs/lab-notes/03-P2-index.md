# P2 · 索引：带墓碑的 B+ 树与可扩展哈希

> **状态：B+ 树 18/18 通过；可扩展哈希 10/11 通过（剩下那一个是坏测试，见 7.4）。**
> 第 1–4 节是从测试用例反推出的**精确行为规格**（骨架文档未给出），
> 第 5 节起是 B+ 树的实现记录，第 7 节是可扩展哈希。

**涉及文件**
- `src/storage/page/b_plus_tree_page.cpp`（公共页头）
- `src/storage/page/b_plus_tree_leaf_page.cpp` / `b_plus_tree_internal_page.cpp`
- `src/storage/index/b_plus_tree.cpp`（增删查主逻辑）
- `src/storage/index/index_iterator.cpp` + `src/include/storage/index/index_iterator.h`
- `src/storage/page/extendible_htable_{header,directory,bucket}_page.cpp`
- `src/container/disk/hash/disk_extendible_hash_table.cpp`

**测试**：`b_plus_tree_insert_test`、`b_plus_tree_delete_test`、`b_plus_tree_tombstone_test`、
`b_plus_tree_concurrent_test`、`b_plus_tree_sequential_scale_test`、
`extendible_htable_page_test`、`extendible_htable_test`、`extendible_htable_concurrent_test`

---

## 1. F2025 版的特殊之处：墓碑（Tombstone）

经典 B+ 树的 `Remove` 会立刻把条目从叶子里搬走，一旦触发下溢就要借用或合并，
而借用/合并要改父节点的分隔键，可能层层向上传播。**这是 B+ 树最贵的操作。**

F2025 版引入了**墓碑**来推迟这个代价：删除时不真的搬走条目，只把它标记为"已逻辑删除"。
只有当墓碑缓冲区放不下了，才真正物理删除最老的那一条。

```
叶子页布局（NumTombs = 2 的例子）
 ┌────────┬───────────┬─────────┬─────────┬───────────────────┬───────────────────┐
 │ HEADER │ TOMB_SIZE │ TOMB(0) │ TOMB(1) │ KEY(0)…KEY(n-1)   │ RID(0)…RID(n-1)   │
 └────────┴───────────┴─────────┴─────────┴───────────────────┴───────────────────┘
              ↑            ↑         ↑
        num_tombstones_   存的是 key_array_ 的下标（不是键本身）
```

骨架头文件里的一句注释确定了语义：

> *"The last `NumTombs` keys with pending deletes in this page **in order of recency (oldest at front)**."*

即：墓碑缓冲区是一个**容量为 `NumTombs` 的 FIFO 队列**，保存"最近 N 次尚未生效的删除"。

---

## 2. 从测试反推出的精确规格

以下每一条都由 `b_plus_tree_tombstone_test.cpp` 的具体断言确定。

### 规格 A：`GetSize()` **包含**墓碑条目

依据：`b_plus_tree_debug.h:168` 写着 `leaf->GetSize() > tombs.size()`，
并据此计算空白格数量。所以墓碑条目物理上仍占一个槽位、仍计入 `size_`。

**推论**：打墓碑**不改变** `size_`，因此**不会触发下溢**。
只有"应用"墓碑（物理删除）才会让 `size_` 减一。

### 规格 B：`Remove` 的算法

```
Remove(key):
  1. 定位叶子，二分查找 key
  2. 未找到，或该条目已是墓碑  → 直接返回（幂等）
  3. 若墓碑缓冲区已满：
       取出队首（最老的待删条目），物理删除它 → size_ -= 1
  4. 把当前条目的下标压入墓碑队尾
  5. 若第 3 步造成 size_ < GetMinSize() → 处理下溢（借用 / 合并）
```

**验证 1**（`TombstoneBasicTest` 第三段）：在一个 `size > minsize` 的叶子里，
NumTombs=2，连删 3 个键 `k0,k1,k2`。
结果断言 `tombstones == [k1, k2]`（数量 = 3−1 = 2），且 `GetValue(k0)` 返回空。
→ 证明第 3 步淘汰的是**最老的**（k0 被物理删除），且顺序是 FIFO。

**验证 2**（`TombstoneBasicTest` 第一段）：删 1、5、9 后，
三个叶子各留一个墓碑，跨叶子按叶子顺序收集得到 `[1,5,9]`。
→ 证明未满时只打墓碑，不做任何物理删除。

### 规格 C：`Insert` 命中墓碑 = 复活

```
Insert(key, value):
  若 key 已存在且**是墓碑** → 更新其 value，把它从墓碑队列中摘除，返回 true
  若 key 已存在且是活条目   → 返回 false（不支持重复键）
  否则                      → 正常插入
```

**验证**（`TombstoneBasicTest` 第二段）：把删掉的 1、5、9 重新插入后，
所有叶子的 `GetTombstones().size() == 0`，且 `GetValue` 返回**新的** RID。

### 规格 D：分裂时墓碑按相对顺序切分

**验证**（`TombstoneSplitTest`）：一个叶子的墓碑队列是 `[3,2,0]`，分裂后
每个叶子的墓碑必须等于"原队列中属于本叶子的那些，保持原相对顺序"。
测试用 `std::sort(expected.rbegin(), expected.rend())` 构造期望值——
因为该例的删除顺序恰好是 3→2→0（降序），过滤后自然保持降序。

→ 实现要求：分裂时逐条搬运条目，同步重建两侧的墓碑下标数组，**保持相对先后顺序**。

### 规格 E：合并时墓碑串接，超容量则从**队首**应用

```
merge(left, right):
  条目：left.entries ++ right.entries
  墓碑：left.tombs   ++ right.tombs      (左边的在前，即更老)
  while (墓碑数 > NumTombs):
      物理删除队首那条 → size_ -= 1
```

**验证**（`TombstoneCoalesceTest`，这是全套测试里最精妙的一个）：

| | leaf_max=6, NumTombs=2, minsize=3 |
|---|---|
| 初始 | 插入 0..6 → 分裂成 `左=[0,1,2]`(smaller) 和 `右=[3,4,5,6]`(larger) |
| 删除序列 | `[5, 0, 6, 1, 3, 2]`（交替作用于两个叶子） |

逐步模拟：

| 步骤 | 左叶子 (smaller) | 右叶子 (larger) |
|---|---|---|
| 初始 | `[0,1,2]` tombs=`[]` | `[3,4,5,6]` tombs=`[]` |
| Remove 5 | — | `[3,4,5,6]` tombs=`[5]` |
| Remove 0 | `[0,1,2]` tombs=`[0]` | — |
| Remove 6 | — | `[3,4,5,6]` tombs=`[5,6]` |
| Remove 1 | `[0,1,2]` tombs=`[0,1]` | — |
| Remove 3 | — | 满 → 应用 5 → `[3,4,6]` size 3；tombs=`[6,3]` |
| Remove 2 | 满 → 应用 0 → `[1,2]` **size 2 < 3 下溢**；tombs=`[1,2]` | |
| 下溢处理 | 2 + 3 = 5 ≤ max 6 → **合并** | |
| 合并结果 | 条目 `[1,2,3,4,6]`，墓碑 `[1,2]++[6,3]=[1,2,6,3]` 超容量 2 → 应用 1、2 → **`[3,4,6]` tombs=`[6,3]`** | 被删除 |

测试断言：`remaining_pid == smaller_pid` 时 `tombstones == [to_delete[2], to_delete[4]] == [6, 3]`。**完全吻合。**

### 规格 F：下溢时"能合并就合并"，否则借用

```
if (left->GetSize() + right->GetSize() <= max_size)  → 合并
else                                                  → 借用一条
```

**验证**（`TombstoneBorrowTest`）：leaf_max=4、NumTombs=1、插入 0..4。
分裂后 `左=[0,1]`(size 2 == minsize)、`右=[2,3,4]`。删除序列 `[2, 1, 0]`：

| 步骤 | 结果 |
|---|---|
| Remove 2 | 右 `[2,3,4]` tombs=`[2]` |
| Remove 1 | 左 `[0,1]` tombs=`[1]` |
| Remove 0 | 左满 → 应用 1 → `[0]` size 1 **下溢**；tombs=`[0]` |
| 下溢处理 | 1 + 3 = 4 ≤ max 4 → **合并**（注意：右有 3 > minsize 2，若策略是"优先借用"就会走借用，结果不符） |
| 合并结果 | 条目 `[0,2,3,4]`，墓碑 `[0]++[2]=[0,2]` 超容量 1 → 应用 0 → **`[2,3,4]` tombs=`[2]`** |

测试断言：`tombstones.size() == 1 && tombstones[0] == to_remove[0] == 2`。**吻合。**

→ 这条测试同时锁死了**分裂策略**：必须得到 `左=[0,1]`（size 2），
也就是"**先把满页对半切开，再把新键插入到该去的那一半**"，
而不是"先插入形成 5 个条目再切"（那样会得到左 2 右 3 或左 3 右 2，
测试的 else 分支模拟下来结果不符）。

### 规格 G：借用要携带墓碑状态

从兄弟页借一条条目时，若该条目是墓碑，墓碑身份要一起搬过去；
接收方墓碑队列若因此超容量，同样从队首应用。

### 规格 H：迭代器与查询跳过墓碑

- `GetValue(key)`：命中墓碑 → 返回 false / 空结果。
- `IndexIterator`：`Begin()` 和 `operator++` 都必须跳过墓碑条目。
- **全部键被删光后**（叶子里只剩墓碑），`tree.Begin().IsEnd()` 必须为 `true`，
  但树本身不能被物理清空（`TombstoneBasicTest` 末尾断言仍有 8 < tot_tombs < 17 个墓碑）。

---

## 3. 这个设计的取舍

| | 经典即时删除 | 墓碑延迟删除 |
|---|---|---|
| 删除的平均代价 | 每次都可能触发借用/合并，进而改父节点 | 每 `NumTombs` 次才触发一次 |
| 读放大 | 无 | 查询要额外判断墓碑 |
| 空间 | 紧凑 | 最多浪费 `NumTombs` 个槽位/页 |
| 删后重插 | 完整的删除 + 插入 | O(1) 复活，连结构都不用动 |

这本质上是**把随机的、昂贵的结构调整批量化**——和 LSM-Tree 的 delete marker、
PostgreSQL 的 dead tuple + VACUUM 是同一个思想：
**先记账，攒够了再一次性清算。**

---

## 4. 与 P1 的接口

B+ 树的每个节点就是缓冲池的一个页，所有访问都通过 P1 的页守卫：

```cpp
ReadPageGuard  guard = bpm_->ReadPage(page_id);
auto page = guard.As<LeafPage>();          // reinterpret_cast 到页结构

WritePageGuard guard = bpm_->WritePage(page_id);
auto page = guard.AsMut<InternalPage>();   // 会自动置脏位
```

这解释了为什么 P2 的每一个类都写着
`BPlusTreePage() = delete; ~BPlusTreePage() = delete;`——
这些"对象"从来不被构造，它们只是**对缓冲池里那 4KB 字节的一种解释**。
`Init()` 取代了构造函数的角色。

### 并发：螃蟹锁（Latch Crabbing）

沿树下降时的加锁规则：

1. 先锁住父节点，再锁住子节点；
2. 一旦确认子节点"安全"（插入时未满 / 删除时大于最小容量），
   立刻释放**所有**祖先的锁；
3. 否则保留祖先的锁，因为分裂/合并可能向上传播。

`Context` 类（`b_plus_tree.h`）就是为此设计的：`write_set_` 存着一路持有的写守卫，
安全时 `write_set_.clear()` 一次性释放。

---

## 5. 实现记录：三个把我卡住的坑

### 坑 1：骨架自己漏了模板参数（一定要先修）

`b_plus_tree.h` 原本写的是：

```cpp
using LeafPage = BPlusTreeLeafPage<KeyType, ValueType, KeyComparator>;   // ← 少了 NumTombs！
```

于是树内部用的叶子类型永远是「零墓碑」版本，而测试实例化的是
`BPlusTree<..., 2>` 并按 `BPlusTreeLeafPage<..., 2>` 去检查页内容——**两者根本不是同一个类**。
不改这一行，所有墓碑测试都会以匪夷所思的方式失败。

### 坑 2：`write_set_.size() == 1` ≠「父节点是根」

下溢向上传播时，我最初这样判断是否到了树根：

```cpp
if (ctx.write_set_.size() == 1) { /* 当作根处理，访问 ctx.header_page_ */ }
```

**这是错的。** 螃蟹锁一旦遇到安全节点就会 `write_set_.clear()`，
所以队首往往只是「我们还攥着的最高一层」，而不是真正的树根。
把一个普通中间节点误当成根，就会去访问早已释放的头页守卫，直接 crash。

正确写法是用 `Context::IsRootPage(page_id)`——`Context` 里那个
`root_page_id_` 字段的存在意义正在于此。

### 坑 3：内部节点的最小容量必须是 `ceil(max/2)` 且至少 2

`GetMinSize()` 若对叶子和内部页一视同仁地取 `max/2`，当 `internal_max_size = 3` 时
内部节点的下限会算成 1，于是**允许存在只有一个孩子的内部节点**。这种节点：

- 没有任何分隔键可用；
- 下溢处理时 `parent->ValueAt(sibling_index)` 直接下标越界。

所以要分开处理（`b_plus_tree_page.cpp` STEP 2）：

| 页类型 | `size_` 的含义 | 下限 |
|---|---|---|
| 叶子 | 记录条数 | `max / 2`（floor，被测试锁死） |
| 内部 | **孩子个数** | `(max + 1) / 2`（ceil，且天然 ≥ 2） |

### 另外三个关键决策

**① 根节点的「安全」判据要单独写**（STEP 18）

头页写锁只在「根本身会被换掉」时需要保留，而根不受最小容量约束：

```cpp
root_safe = (op == INSERT) ? size < max
          : (是叶子 ? size > 1     // 删空了要把整棵树置为 INVALID_PAGE_ID
                    : size > 2);   // 内部根掉到 1 个孩子才塌缩
```

偷懒复用 `IsSafe` 会在 `max_size` 较小时提前放掉头页锁，等真要塌缩时手上已经没有它了。

**② 乐观加锁**（STEP 17）

`OptimisticInsertTest` 精确断言：往一个还有空位的叶子里插一条，
只能发生 **1 次 `WritePage`**。所以必须先赌一把：

1. 全程读锁下降到叶子的父节点；
2. 只给叶子加写锁；
3. 叶子安全就直接干活，不安全就整个放弃、回退到悲观路径重来。

放掉叶子读锁、再申请写锁之间有个窗口，但它是**安全**的——
任何会让这一页消失或改变归属的操作都必须先拿到其父节点的写锁，
而父节点的读锁还在我们手里。

墓碑还额外送了一条安全捷径：**墓碑队列没满 ⇒ 这次删除根本不改 `size_` ⇒ 必然安全**。

**③ 先分裂、后插入**

不能「先插入撑到 `max+1` 再切」——当 `max_size` 等于页的物理容量上限时，
那一下会直接写到数组外面。而且这个顺序也被测试锁死（规格 F）。

---

## 6. 测试结果

```
b_plus_tree_insert_test              4 tests  PASSED
  ├─ BasicInsertTest
  ├─ OptimisticInsertTest      ← 断言插入只产生 1 次 WritePage
  ├─ InsertTest1NoIterator
  └─ InsertTest2

b_plus_tree_delete_test              3 tests  PASSED
  ├─ DeleteTestNoIterator      ← 删光后 root_page_id 必须变回 INVALID_PAGE_ID
  ├─ OptimisticDeleteTest
  └─ SequentialEdgeMixTest

b_plus_tree_tombstone_test           4 tests  PASSED
  ├─ TombstoneBasicTest        ← FIFO 语义 + 复活 + 迭代器跳过
  ├─ TombstoneSplitTest        ← 分裂保持墓碑相对顺序
  ├─ TombstoneBorrowTest       ← 借用携带墓碑身份 + 优先合并策略
  └─ TombstoneCoalesceTest     ← 合并时墓碑串接并从队首兑现

b_plus_tree_sequential_scale_test    1 test   PASSED  (3682 ms)

b_plus_tree_concurrent_test          6 tests  PASSED
  ├─ InsertTest1  ( 2549 ms)
  ├─ InsertTest2  (20468 ms)
  ├─ DeleteTest1  (  408 ms)
  ├─ DeleteTest2  (  431 ms)
  ├─ MixTest1     (18504 ms)
  └─ MixTest2     ( 1709 ms)
```

**B+ 树合计 18 个测试全部通过。**

可扩展哈希（第 7 节，评分范围外）：

```
extendible_htable_page_test          2 tests  PASSED
  ├─ BucketPageSampleTest            ← 桶页的增删查 + 满/空判定
  └─ HeaderDirectoryPageSampleTest   ← 目录翻倍/收缩，锁死了 IncrGlobalDepth 必须复制高半区

extendible_htable_test               2 / 3
  ├─ InsertTest1               ✗  ← 测试本身坏了，见 7.4
  ├─ InsertTest2               PASSED
  └─ RemoveTest1               PASSED

extendible_htable_concurrent_test    6 tests  PASSED
```

`make format`、`make check-lint` 通过。

---

## 7. 可扩展哈希表

**已实现**（虽然它是 F2023 遗留、不在 F2025 评分范围内）。
10 / 11 个测试通过；**剩下那一个测试本身有问题**，证据见 7.4。

涉及文件：`extendible_htable_{header,directory,bucket}_page.cpp`、
`disk_extendible_hash_table.cpp`，对应 STEP 20–29。

### 7.1 三层结构与两个深度

```
   header (静态, 取哈希高 max_depth 位)
      │  最多 2^9 个槽位
      ▼
   directory (动态, 取哈希低 global_depth 位)
      │  2^GD 个槽位，可翻倍 / 减半
      ▼
   bucket (无序紧凑数组)
```

**为什么要三层**：目录数组按 2 的幂增长，而一个 4KB 页最多装 512 个 `page_id`
（2048 字节的 `bucket_page_ids_` 加 512 字节的 `local_depths_` 已占去大半页）。
单个目录页只能管 2^9 个桶，header 再往上分一层，总容量提到 2^18。

**核心概念只有一个**——全局深度 GD 与局部深度 LD 的差：

> LD 比 GD 小的桶，会被 **2^(GD−LD) 个目录槽位同时指向**。

这个"多对一"就是可扩展哈希优于静态哈希的全部理由：
**桶满了不必重建整张表，只分裂那一个桶。** 分裂时该桶 LD 加一，
原本指向它的那批槽位一分为二，其它桶完全不受影响。
只有当 LD 已经等于 GD（没有富余槽位可分）时，才需要把目录整体翻倍。

### 7.2 三个关键实现细节

**① header 取高位、directory 取低位**（STEP 21）

两层取同一个 32 位哈希的**两端**，因此互不干扰。若两层都取低位，
目录分裂会改变低位的含义，键就"跑到隔壁目录"去了——而 header 层没有迁移机制，
那些键永久找不回来。

`max_depth == 0` 必须提前返回 0：`hash >> 32` 是**未定义行为**，
x86 会把移位量按 32 取模从而原样返回 `hash`，随后越界访问。

**② 目录翻倍是"原样复制一份接在后面"**（STEP 24）

新槽位 `i + 2^GD` 与旧槽位 `i` 指向同一个桶，局部深度照抄。
所以**翻倍本身不搬任何数据**，它只是把分裂的余地腾出来。

这也解释了为什么目录取**低**位而非高位：翻倍后一个键落到的槽位要么不变、
要么变成 `idx + 2^GD`，而这两个槽位指向同一个桶。

**③ 迁移条目必须倒序遍历**（STEP 28）

桶页无序，所以 `RemoveAt` 用**末项填空**做到 O(1) 删除。代价是正序遍历时
删掉下标 `i` 会把最后一项挪到 `i`，而循环紧接着 `i++` 跳过了它——
那一条永远不被检查，本该迁走的条目留在老桶里，之后按新低位再也查不到。

### 7.3 与 B+ 树的对照

| | B+ 树 | 可扩展哈希 |
|---|---|---|
| 树高 | O(log n)，随数据量变 | **恒为 3** |
| 等值查找 | O(log n) 次页访问 | O(1) 次页访问 |
| 范围查询 / ORDER BY | ✅ 叶子链表天然有序 | ❌ **完全做不到** |
| 结构调整 | 分裂/合并**向上传播**，可能改到根 | 只在同一目录内横向传播，碰不到 header |
| 最坏情况 | 有保证 | 哈希倾斜时退化 |

"索引即物化的排序结果"这条在 P3 优化器里用到的捷径（`ORDER BY` 改写成索引扫描），
对哈希索引**完全不成立**。这也是为什么真实数据库的默认索引几乎总是 B+ 树，
哈希索引只在明确知道"只做等值查找"时才用。

### 7.4 `InsertTest1` 是个坏测试（不是实现的问题）

```cpp
// header_max_depth=0, directory_max_depth=2, bucket_max_size=2
DiskExtendibleHashTable<int, int, IntComparator> ht(..., 0, 2, 2);
for (int i = 0; i < 8; i++) { ASSERT_TRUE(ht.Insert(i, i)); }   // 断言 8 个全部插入成功
ASSERT_FALSE(ht.Insert(8, 8));                                  // 第 9 个才该失败
```

配置给出的总容量恰好是 4 个桶 × 2 = 8，所以这个断言隐含要求
**键 0–7 在低 2 位上恰好 2/2/2/2 均匀分布**。实测并非如此：

| 低 2 位 | 落进来的键 | 数量 |
|---|---|---|
| `00` | 0, 2, 4, 6 | **4** ← 超过桶容量 2 |
| `01` | — | 0 |
| `10` | 1 | 1 |
| `11` | 3, 5, 7 | 3 |

于是插入到 **key=4**（低 2 位为 `00` 的第 3 个键）时必然失败——实测失败点正是这里。

这不是实现能绕过的：目录深度上限 2 ⇒ 寻址只能用低 2 位 ⇒ 这 4 个键必须挤进同一个桶。
**任何正确实现都过不了这个断言。** 该测试大概是当年在某个不同的
`HashFunction<int>` 下写死的，随骨架演进而失效，又因为一直带着 `DISABLED_`
前缀无人运行，就这么留下来了。

保留原样、不去改测试来"凑通过"——记下证据比刷一个绿勾有意义。

---

## 8. 一句话小结

B+ 树的复杂度全部来自一个约束：**节点必须是定长磁盘页**。
于是有了分裂/合并/借用（维持半满）、有了叶子链表（范围扫描不回根）、
有了螃蟹锁（页级并发）。

而 F2025 的墓碑机制则回答了另一个问题：
**既然结构调整这么贵，能不能少做几次？** 答案是「先记账，攒够了再清算」——
和 LSM-Tree 的 delete marker、PostgreSQL 的 dead tuple + VACUUM 是同一个思想。

把可扩展哈希也写一遍之后，最大的收获是看清了一件事：
**这两种索引的差别，全部源于"有没有保留顺序"这一个选择。**

哈希放弃了顺序，于是换来层数恒定（3 次页访问，与数据量无关）、
结构调整不向上传播；代价是范围查询和 `ORDER BY` **完全做不了**——
P3 优化器里"索引即物化的排序结果"那条捷径对它不成立。

B+ 树保留了顺序，于是必须维持半满、必须分裂合并向上传播、必须螃蟹锁，
换来的是范围扫描、有序遍历，以及最坏情况的复杂度保证。

真实数据库的默认索引几乎总是 B+ 树，不是因为它更快，
而是因为**它没有"某类查询彻底做不了"这种断崖**。
