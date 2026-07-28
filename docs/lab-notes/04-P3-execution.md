# P3 · 查询执行引擎

**涉及文件**
- `src/execution/*.cpp`（各类算子）、`src/execution/execution_common.cpp`（排序基础设施）
- `src/include/storage/page/intermediate_result_page.h`（外排的中间结果页）
- `src/optimizer/seqscan_as_indexscan.cpp`、`nlj_as_hash_join.cpp`、`sort_limit_as_topn.cpp`（优化器规则）

**测试**：`test/sql/p3.00-*.slt` ~ `p3.19-*.slt`（20 个评分用例），全部通过。

```bash
# 注意：不要偷懒写成 p3.*.slt —— 那个通配符会多匹配到 6 个文件：
#   p3.20-window-function.slt   窗口函数，非评分范围（见第 6 节）
#   p3.leaderboard-q{1,2,3}*    可选的性能排行榜基准，数据量极大，要跑很久
# 我就因为这个以为"测试卡死了"，实际只是在跑 leaderboard。
for f in ../test/sql/p3.[01]?-*.slt; do ./bin/bustub-sqllogictest "$f" || echo "FAIL $f"; done
```

---

## 1. 一条 SQL 是怎么变成结果的

```
  SQL 文本
     │  Parser（libpg_query）
     ▼
  抽象语法树
     │  Binder      —— 把名字解析成 catalog 里的实际对象
     ▼
  Bound AST
     │  Planner     —— 生成「怎么算」的算子树
     ▼
  逻辑计划                 ← P3 的输入
     │  Optimizer   —— 等价改写成更快的算子树     ← P3 要写 3 条规则
     ▼
  物理计划
     │  ExecutionEngine —— 自顶向下 Init()，再反复 Next()
     ▼
  结果元组
```

P3 要做的就是最后两步：**把每个算子实现出来**，以及**写几条让计划变快的改写规则**。

---

## 2. F2025 的关键改动：向量化执行

经典火山模型（Volcano）的接口是 `Next(Tuple *out)`，一次一条元组。
F2025 改成了**批量**接口：

```cpp
auto Next(std::vector<Tuple> *tuple_batch, std::vector<RID> *rid_batch, size_t batch_size) -> bool;
```

一次产出一整批。好处：
- 每条元组穿过整棵算子树的虚函数调用开销被摊薄 `batch_size` 倍；
- 对 CPU 缓存和分支预测友好（同类型数据连续处理）。

这是现代分析型数据库（DuckDB、ClickHouse）的标准做法。

### 三条必须遵守的约定

1. **`Next()` 开头必须清空两个出参**。
2. **`rid_batch` 必须和 `tuple_batch` 等长**。
   上层算子（如 `ProjectionExecutor`）会用同一个下标同时访问两个数组。
   聚合、连接、排序的结果不对应任何物理行，也**必须**填哑 RID 占位。
   > 这是我踩的第一个坑：`AggregationExecutor` 只填了 tuple 没填 rid，
   > 结果在 `ProjectionExecutor` 里 `child_rids_[i]` 越界，
   > ASAN 报的却是 `_platform_memmove` 里的 SEGV，与真正的错误点隔了两层。
3. **`Init()` 可能被调用多次**。作为嵌套循环连接的内表时，
   外层每推进一条元组就会把内表整个重置一遍。所以 `Init()` 的语义是
   「重新开始」而不是「第一次开始」，所有游标/标志位都要复位。

---

## 3. 算子分类：流式 vs 阻塞

这是理解执行引擎最有用的一把尺子。

| 类型 | 算子 | 特征 |
|---|---|---|
| **流式**（pipelined） | SeqScan、IndexScan、Filter、Projection、Limit、NestedLoopJoin | 拿到一条就能吐一条，内存 O(1) |
| **阻塞**（pipeline breaker） | Aggregation、Sort、TopN、HashJoin 的建表侧、Insert/Delete/Update | 必须先看完**全部**输入才能产出第一条 |

阻塞算子的共同实现套路：**在 `Init()` 里把子算子消费干净并物化结果，
`Next()` 只负责把结果搬出去**。因为：

- 聚合：下一条元组随时可能属于某个已有分组并改变它的 SUM/MAX；
- 排序：最小的那条可能是最后读到的；
- 写算子：必须先全部写完才知道「影响了几行」。

流式算子里，`Limit` 最能体现火山模型的优雅：够了就直接 `return false`，
根本不去问子算子，于是下面的全表扫描自然停在半路（STEP 3）。

---

## 4. 各算子实现要点

### 4.1 SeqScan（STEP 1-2）

- 构造函数只做登记，游标建在 `Init()` 里 —— 算子树是一次性构造的，
  此时还不知道这棵树会不会真的被执行。
- 用 `std::optional<TableIterator>` 持有游标，因为 `TableIterator` 没有默认构造函数，
  而 `Init()` 要能重置它。
- **跳过 `meta.is_deleted_` 的元组**：删除在表堆里只是打标记，
  和 P2 叶子页的墓碑是同一个思想。
- 支持谓词下推（`plan_->filter_predicate_`）。

### 4.2 Insert / Delete / Update（STEP 4-5）

三者共享同一套骨架：阻塞式消费子算子 → 改表堆 → **同步维护所有索引** → 输出一行行数。

**索引维护是写操作的固有成本**，漏掉就会让索引和表脱节：

| 操作 | 表堆 | 索引 |
|---|---|---|
| INSERT | `InsertTuple` | 每个索引 `InsertEntry` |
| DELETE | `UpdateTupleMeta(is_deleted=true)` | 每个索引 `DeleteEntry`（**必须真删**，索引没有墓碑机制） |
| UPDATE | 删旧 + 插新 | 先 `DeleteEntry(旧键)` 再 `InsertEntry(新键)` |

**UPDATE 为什么是「删除+插入」而不是原地改**：
- 新元组长度可能变（变长字段），原地覆盖会破坏页内布局；
- P4 的 MVCC 要求旧版本留在原地供老事务读取。

顺序必须是**先删后插**：反过来的话，新插入的元组可能落在子算子（SeqScan）
尚未扫过的位置，同一行会被更新第二次，形成死循环。
`TableIterator::stop_at_rid_` 也是为防这个而存在的。

> ⚠️ **P4 推翻了这一节的两条结论**，读到这里请一并参照
> [05-P4-并发控制](./05-P4-concurrency.md) 第 6.3 / 7 节：
>
> | | P3 的做法 | P4 改成 |
> |---|---|---|
> | UPDATE | 删除 + 插入（RID 改变） | **原地覆盖**（版本链必须锚在同一 RID）；仅当更新主键时才退化回删除+插入 |
> | DELETE 对**主键**索引 | 摘除索引项 | **保留**（索引项承担唯一性约束，并支撑"复活"路径） |
> | DELETE 对**二级**索引 | 摘除索引项 | 不变，仍然摘除 |
>
> 本节保留 P3 版本的推理过程，因为那些理由本身是对的 ——
> 只是 MVCC 引入了更强的约束，把结论压向了另一边。这种"同一个问题在
> 不同约束下有不同最优解"的对照，本身就是这套实验最值得体会的地方。

### 4.3 IndexScan（STEP 7）

两种模式：

- **点查**：`plan_->pred_keys_` 非空（来自 `WHERE a = 1 OR a = 5`），
  对每个键做一次 B+ 树查找，O(log n) 而不是 O(n)。
- **有序遍历**：优化器把 `ORDER BY a` 改写成索引扫描时，
  顺着 B+ 树的叶子链表走一遍就等于排好序了 —— **索引即物化的排序结果**。

两种模式最后都要**回表**：索引只给出 RID，其余列必须去表堆取。
这就是「覆盖索引」在真实数据库里那么重要的原因。

### 4.4 Aggregation（STEP 9-11）

**增量合并**：每来一条元组就折叠进中间结果，内存 O(分组数) 而非 O(行数)。

NULL 语义是全部难点：

| 聚合 | 初始值 | NULL 输入 |
|---|---|---|
| `COUNT(*)` | 整数 0 | 也算（数的是**行**） |
| `COUNT(col)` | NULL | 跳过 |
| `SUM` / `MIN` / `MAX` | NULL | 跳过 |

初始值是 NULL 而非 0，正是 `SELECT SUM(x) FROM empty` 返回 NULL、
而 `SELECT COUNT(*) FROM empty` 返回 0 的原因。

还有两处专门的特判：

**① 空表 + 无 GROUP BY 必须输出一行**
没有 GROUP BY 时，整张表就是"唯一的那一个分组"，哪怕它是空的；
有 GROUP BY 则应返回零行。

**② `AggregateKey::operator==` 必须显式处理 NULL**（STEP 11，最隐蔽的坑）

SQL 三值逻辑里 `NULL = NULL` 是 UNKNOWN，而 GROUP BY 却要求所有 NULL 归为**同一组**。
直接用 `CompareEquals` 的后果非常曲折：

```
InsertCombine: ht_.count(k) == 0  → 判定不存在 → insert(k, 初始值)
               ht_[k]             → 又找不到刚插进去的（operator== 说 k != k）
                                  → operator[] 默认构造一个 AggregateValue
                                  → 它的 aggregates_ 是**空 vector**
CombineAggregateValues: aggregates_[0] 越界 → 拿到 type_id_ 非法的 Value
Value::Add: Type::GetInstance(非法 id) 返回空指针 → 段错误
```

崩溃点（`Value::Add`）和真正的错误点（`operator==`）隔了三层。

### 4.5 三种 Join（STEP 12-15）

| 算子 | 复杂度 | 适用条件 |
|---|---|---|
| NestedLoopJoin | O(N × M) | 任意条件，兜底方案 |
| NestedIndexJoin | O(N × log M) | 内表连接列上有索引 |
| HashJoin | O(N + M) | **等值**连接，且右表装得下内存 |

**NLJ 必须逐条重启内表**（STEP 12）。我最初把内表一次性物化到内存，
想把 I/O 从 O(N×M) 降到 O(N+M)，但 `p3.10` 有一条断言：

```cpp
right.GetInitCount() + 1 >= left.GetNextCount()
```

它要求教科书式的实现：外表每推进一条，就把内表 `Init()` 一次重新拉取。
这不是刁难——物化内表在真实系统里不成立（内表可能远大于内存；
相关子查询每次的结果本就不同）。**想要更快就该换算子，而不是偷偷改变算子语义。**

**LEFT JOIN 的补空行**（STEP 13）：内表扫完一圈没匹配上，
仍要输出外表元组、右侧填 NULL。所以需要一个 `matched_` 标志记录"这一圈有没有匹配过"。

**批量模型下所有内层游标都必须是成员变量**：一批输出可能在内层循环中途填满，
下次 `Next()` 必须从**同一条外表元组、同一个内表位置**继续。

### 4.6 排序（STEP 17-24）

**先算排序键再排序**（decorate-sort-undecorate）：
排序过程中每条元组要被比较 O(log n) 次，若每次都现场求值 `ORDER BY a + b * 2`，
代价会乘以 log n。`GenerateSortKey` 一次性把键算出来，之后只比较 `Value`。

**NULL 的位置必须显式规定**：`CompareLessThan(NULL, x)` 返回 `CmpNull`，
只按 `CmpTrue` 判断的话含 NULL 的元组会被当作"相等"，相对顺序变成不确定的。
BusTub 的默认约定是 **NULL 视为最小值**（升序时排最前），
`NULLS FIRST` / `NULLS LAST` 可显式覆盖。

**三个排序算子的分工**：

| 算子 | 时间 | 内存 | 何时用 |
|---|---|---|---|
| `SortExecutor` | O(M log M) | O(M) | 数据装得进内存 |
| `ExternalMergeSortExecutor` | O(M log M) | **O(缓冲池)** | 装不进内存 |
| `TopNExecutor` | O(M log N) | **O(N)** | `ORDER BY ... LIMIT N` |

**外部归并排序**（STEP 18-22）的核心是把中间结果放进**缓冲池管理的页**里，
而不是 `std::vector<Tuple>`——否则和内存排序毫无区别。

页内布局用最简单的顺序格式（每条元组前缀 4 字节长度），
因为归并对中间结果**只做顺序扫描**，不需要支持随机访问的槽位目录。

第一趟：每读一批就在内存里排好序、写成一个有序段。
后续每趟：两两归并，段数减半。归并时**任意时刻只需在内存持有两条元组**，
与段的长度完全无关——这正是外排能处理远大于内存的数据的根本原因。

> 迭代器的坑：`operator*` 必须无副作用（连续两次解引用要得到同一条元组），
> 所以「读」和「推进」被拆成 `ReadAt` / `SkipAt` 两个方法。
> 我最初把偏移推进写在了 `ReadAt` 里、`operator++` 只加计数，
> 结果 `offset_` 永远停在页首，查询返回了 10 条一模一样的记录。

**TopN**（STEP 24）用一个大小恒为 N 的**最大堆**：堆顶是"当前前 N 名里最差的"。
每来一条只和堆顶比一次。关键收益不是时间而是**内存 O(N)**——
一亿行里取前 10 条，内存占用与那一亿行无关。

---

## 5. 三条优化器规则

优化规则的统一形状是「先递归优化孩子，再看自己能不能改写」的自底向上模式。

### 5.1 `SeqScan → IndexScan`（STEP 8）

识别 `WHERE col = const`（以及它们的 OR 并集），且该列上有索引。

能用索引的条件很严格：
- 比较类型必须是 `=`（范围条件需要区间扫描，本实现不支持）；
- 一侧是列、另一侧是常量（`a = b` 无法用作查找键）；
- OR 的每一支必须指向**同一列**。

### 5.2 `NestedLoopJoin → HashJoin`（STEP 16）

识别**等值条件的合取**：`l.a = r.x AND l.b = r.y`。
- `OR` 不行：一条元组可能因不同分支匹配，无法归约成单一的键；
- `<`、`>` 不行：哈希表只支持等值查找；
- `l.a = l.b`（同一侧两列）不行：那是过滤而非连接。

表达式用 `tuple_idx` 区分来源（0=左，1=右）；写成 `r.x = l.a` 时要对调。

### 5.3 `Limit(Sort(...)) → TopN`（STEP 25）

**算子合并（operator fusion）** 的典型例子：两个算子各自都没问题，
但组合在一起时存在明显更优的单一实现。
时间 O(M log M) → O(M log N)，内存 O(M) → O(N)。

---

## 6. 测试结果

```
p3.00-primer               PASS      p3.10-simple-join          PASS
p3.01-seqscan              PASS      p3.11-multi-way-join       PASS
p3.02-insert               PASS      p3.12-repeat-execute       PASS
p3.03-update               PASS      p3.13-nested-index-join    PASS
p3.04-delete               PASS      p3.14-hash-join            PASS
p3.05-index-scan-btree     PASS      p3.15-multi-way-hash-join  PASS
p3.06-empty-table          PASS      p3.16-sort-limit           PASS
p3.07-simple-agg           PASS      p3.17-topn                 PASS
p3.08-group-agg-1          PASS      p3.18-integration-1        PASS
p3.09-group-agg-2          PASS      p3.19-integration-2        PASS
```

**20 / 20 通过。** 仓库内 `TODO(P3)` 标记已全部清零。
`make format`、`make check-lint` 通过。

### 未实现：窗口函数（非评分范围）

`p3.20-window-function.slt` 依赖 `WindowFunctionExecutor`，
该文件用的是 `throw NotImplementedException(...)` 而**没有 `TODO(P3)` 标记**
（与可扩展哈希表、`TopNPerGroupExecutor` 情况相同）。
其余带 `TODO(P3)` 标记的任务已全部完成。若需要补做，
可在 `src/execution/window_function_executor.cpp` 实现
`RANK / ROW_NUMBER / 聚合窗口` 三类语义。

---

## 7. 一句话小结

执行引擎的全部设计都围绕一个问题展开：
**数据比内存大，那就一次只碰一小块。**

流式算子让数据"流过"而不驻留；阻塞算子被迫物化时就把中间结果也放进缓冲池的页里；
优化器则负责把"逻辑上正确但物理上很傻"的计划，改写成同样正确但少读几个数量级数据的形式。

真正的性能差距不来自把某个循环写得更紧凑，而来自**换一个算法**——
`O(N×M)` 的嵌套循环换成 `O(N+M)` 的哈希连接，
`O(M)` 内存的全排序换成 `O(N)` 内存的堆。
这也是为什么数据库的"优化器"比"执行器"更值钱。
