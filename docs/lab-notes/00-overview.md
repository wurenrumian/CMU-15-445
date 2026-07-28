# BusTub 实验教学报告 · 总览

> 课程：CMU 15-445/645 *Introduction to Database Systems*（Fall 2025 版本）
> 仓库：本仓库 `lab` 分支
> 目标：从零实现一个可运行的关系型数据库存储与执行引擎

## 这套实验在造什么

BusTub 是一个"教学用但五脏俱全"的磁盘型关系数据库。五个 Project 自底向上拼出完整的一条数据通路：

```
                    ┌──────────────────────────────────────┐
   SQL 查询  ───▶   │  Parser → Binder → Planner → Optimizer│
                    └───────────────────┬──────────────────┘
                                        │  查询计划树
                    ┌───────────────────▼──────────────────┐
        P3          │  执行引擎（火山模型 Executor 树）      │
                    │  SeqScan / Join / Agg / Sort / ...    │
                    └───────────────────┬──────────────────┘
                                        │  Tuple / RID
                    ┌───────────────────▼──────────────────┐
        P4          │  事务与并发控制（MVCC 快照隔离）       │
                    └───────────────────┬──────────────────┘
                    ┌───────────────────▼──────────────────┐
        P2          │  索引（B+ 树 / 可扩展哈希）            │
                    └───────────────────┬──────────────────┘
                    ┌───────────────────▼──────────────────┐
        P1          │  缓冲池（页表 / ARC 替换 / 页守卫）     │
                    └───────────────────┬──────────────────┘
                    ┌───────────────────▼──────────────────┐
                    │  DiskManager（4KB 页读写）            │
                    └──────────────────────────────────────┘

        P0          C++ 并发编程热身（跳表）
```

**一个贯穿全程的核心思想：数据库不信任内存。**
所有数据的权威副本在磁盘上，以 4KB 定长页为单位。内存只是一层可以随时丢弃的缓存。
上层的每一个模块——索引、执行器、事务——拿到的都不是"对象"，而是"某个页的一段字节 +
一把保护它的锁"。这决定了 BusTub 里几乎所有 API 的形状。

## 各 Project 清单

| Project | 主题 | 关键产出 | 状态 | 教学报告 |
|---|---|---|---|---|
| P0 | C++ / 并发热身 | 读写锁保护的并发跳表 | ✅ 9/9 | [01-P0-跳表](./01-P0-skiplist.md) |
| P1 | 缓冲池管理器 | ARC & LRU-K 替换器、磁盘调度器、RAII 页守卫、缓冲池 | ✅ 14/14 | [02-P1-缓冲池](./02-P1-buffer-pool.md) |
| P2 | 索引 | 带墓碑的 B+ 树（含并发） | ✅ 18/18 | [03-P2-索引](./03-P2-index.md) |
| P3 | 查询执行 | 12 个算子 + 3 条优化器规则 | ✅ 20/20 | [04-P3-执行引擎](./04-P3-execution.md) |
| P4 | 并发控制 | MVCC 全套：时间戳、水位线、版本链、写冲突、主键约束、GC、可串行化 | ✅ 25/25 | [05-P4-并发控制](./05-P4-concurrency.md) |

**合计 86 个测试全部通过**（P0–P2 的 41 个单元测试 + P3 的 20 个 SQL 测试 + P4 的 25 个事务测试）。

P1 的 14 个用例包含 `arc_replacer_performance_test`（1 个），它测的是吞吐而非正确性，
但同样是评分项——朴素实现会因 `Evict` 的 O(n) 扫描超时。

上表是 **F2025 版 P0–P4 的评分范围**，已 100% 完成。

### 仓库里还有哪些没做的

BusTub 是逐年演进的，骨架里堆积了往届的实验代码。它们**不属于 F2025 的评分范围**，
但确实还是空桩。为免误导，把全部清单和证据强度列在这里：

| 未实现项 | 归属 | 测试状态 | 位置 |
|---|---|---|---|
| **Trie / TrieStore** | F2023 P0 | ❌ 17 个用例失败（**未** DISABLED） | `src/primer/trie.cpp`、`trie_store.cpp` |
| **ORSet**（CRDT） | F2023 P0 | ❌ 9 个用例失败（**未** DISABLED） | `src/primer/orset.cpp` |
| **CountMinSketch** | F2024 P0 | ❌ 13 个用例失败（**未** DISABLED） | `src/primer/count_min_sketch.cpp` |
| **HyperLogLog** | F2024 P0 | ❌ 失败（带 DISABLED 前缀） | `src/primer/hyperloglog*.cpp` |
| **可扩展哈希表** | F2023 P2 | ❌ 3 个用例失败（带 DISABLED） | `src/container/disk/hash/`、`storage/page/extendible_htable_*` |
| **窗口函数 / TopNPerGroup** | P3 选做 | ❌ `p3.20-window-function.slt` | `src/execution/window_function_executor.cpp` 等 |

**一处需要更正的判断**：我最初用"有没有 `TODO(Pn)` 标记"来区分是否评分任务，
并据此断言可扩展哈希表"没有 TODO 标记"。复查发现**这不准确**——
`disk_extendible_hash_table.cpp` 里有 3 处 `TODO(P2): Add implementation`，
只有三个页类文件确实没标记。详见 [03-P2 第 7 节](./03-P2-index.md)。

同样地，"测试带 `DISABLED_` 前缀 ⇒ 非评分范围"这条推理也**不成立**——
P1、P2 的正式评分测试同样带 `DISABLED_` 前缀，这是 BusTub 的惯例，
目的是防止学生把测试直接跑通就当作完成。

真正可靠的依据只有一条：**F2025 各 Project 的讲义与 Gradescope 提交项**。
按那个口径，P0 是跳表、P1 缓冲池、P2 B+ 树、P3 执行引擎、P4 MVCC —— 均已完成。

> ⚠️ 因此 `make check-tests` 或直接跑 `ctest` **会有失败项**，
> 那些全部来自上表的往届遗留代码，与本仓库实现的 86 个测试无关。

### 一条贯穿全实验的主线：延迟回收

同一个思想在四个 Project 里以不同面貌出现了四次，值得单独记住：

| 位置 | 表现 | 何时真正回收 |
|---|---|---|
| P1 缓冲池 | 脏页不立即回写，只置 `is_dirty_` | 帧被淘汰时 |
| P2 B+ 树叶子 | 删除只入**墓碑队列**，条目留在页里 | 队列满时兑现最老的一笔 |
| P3 表堆 | 删除只置 `is_deleted_` 标记 | 交给后续清理流程 |
| P4 MVCC | 旧版本保留在 undo 链上 | 落到**水位线**以下时，由 `GarbageCollection` 回收 |

**先记账、攒够了再清算** —— 这是数据库系统里反复出现的一等公民思想。

## 代码中的标记约定

为了让实现与讲解对应得上，源码里统一使用这样的标记：

```cpp
// ==== P1 STEP 7: 让帧自己记住它装的是哪个页 ====
// <为什么这样写，而不是别的写法>
// <这里的坑 / 与讲义概念的对应>
```

- `==== Pn STEP k: 标题 ====` —— 一个可独立理解的实现步骤，报告里按同样的编号讲解。
- `---- STEP k.m: 标题 ----` —— 步骤内部的子阶段。
- 普通注释只解释**为什么**，不复述代码在做什么。

全部 91 个主标记（另有 16 个子标记）的清单见
**[附录 · STEP 标记总索引](./99-step-index.md)**，
可以从"我想看某个概念"直接跳到对应文件。按编号顺序读下来，
本身就是一条从零搭出数据库的路径。

## 如何构建与测试

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..

# 并行度：Linux 用 nproc，macOS 没有这个命令
JOBS=$( (nproc 2>/dev/null) || sysctl -n hw.ncpu )

# 构建并运行某个测试
make skiplist_test -j"$JOBS"
./test/skiplist_test

# 课程测试默认带 DISABLED_ 前缀，本地验证需要显式打开
./test/buffer_pool_manager_test --gtest_also_run_disabled_tests

# 代码风格（课程评分项）
make format        # 自动格式化
make check-lint    # cpplint
make check-clang-tidy-p1
```

> **踩坑提醒（重要）**：`cmake --build .` **不会**可靠地重新链接单个 gtest 目标。
> 改完代码后必须显式指定目标：`cmake --build . --target txn_scan_test`
> （或 `make txn_scan_test`）。我因为这个问题两次追查"根本不存在的 bug"——
> 跑的一直是旧二进制。**任何"改了代码但行为没变"的现象，先怀疑没重新构建。**

> **注意**：本仓库在 macOS 上运行 `check-clang-tidy-*` 会报
> `bugprone-forward-declaration-namespace` 错误，位置在 `catalog/column.h`、
> `catalog/schema.h`、`type/value.h`。这是 macOS SDK 里 `typedef union {...}`
> 触发的 clang-tidy 误报，在**未修改的干净仓库上同样复现**，与实验代码无关。

## 关于公开发布

BusTub 的 README 里有一段明确的要求：

> DO NOT PUSH PROJECT SOLUTIONS PUBLICLY. THIS IS AN ACADEMIC INTEGRITY VIOLATION...
> IF YOU ARE A STUDENT OUTSIDE CMU, DO NOT MAKE YOUR SOLUTION PUBLICLY AVAILABLE,
> OTHERWISE YOU WILL BE BANNED FROM USING THE AUTOGRADER.

把事实说清楚，读到这里的人自己判断：

- **法律上**：BusTub 采用 MIT 许可证，公开发布派生作品是许可证明确允许的。这一条没有争议。
- **规则上**：上面那段话是课程组的**请求**，不是合同。本仓库的作者不是 CMU 在读学生，
  没有签署任何课程协议，也从未使用 Gradescope 提交，因此不存在"违反学术诚信"或
  "被封禁"的适用对象。
- **实际影响**：真正的代价落在**别人**身上——公开答案会让后来的选课学生更容易抄，
  也会增加课程组每年重写测试的负担。这是唯一实质性的理由，且它是道义层面的，不是法律层面的。

本仓库选择公开，主要是把它当作**教学材料**而非"答案"：报告里大量篇幅在讲
"为什么这样写"、"我踩了哪些坑"、"测试是怎么把规格锁死的"，这些内容抄不出分数，
但对想学明白的人有用。

如果你是**正在修这门课的学生**：请自己写。这套实验最有价值的部分恰恰是调试过程本身——
你在 `TombstoneCoalesceTest` 上卡的那两个小时，比读完这整份报告学到的都多。
