# P4 · MVCC 并发控制

> **状态：Task 1–5 全部完成，25 个测试全部通过。**

**涉及文件**
- `src/concurrency/watermark.cpp`、`src/concurrency/transaction_manager.cpp`
- `src/execution/execution_common.cpp`（版本链重建、可见性、undo log 生成、冲突检测）
- `src/execution/{seq_scan,index_scan,insert,delete,update}_executor.cpp`（MVCC 读写路径）

**测试**：`txn_timestamp_test`(2)、`txn_scan_test`(3)、`txn_executor_test`(11)、
`txn_index_test`(5)、`txn_abort_serializable_test`(4) —— **25 个全部通过**。

---

## 1. 为什么需要 MVCC

传统的两阶段封锁（2PL）下，读要加共享锁、写要加排他锁，于是：

> **读会阻塞写，写也会阻塞读。**

一个跑十分钟的分析查询会把整张表锁住十分钟。这在 OLTP 系统里是灾难。

MVCC（多版本并发控制）的答案：**不要覆盖旧数据，而是不断追加新版本。**
每个事务读它自己"那个时刻"的版本快照，于是：

> **读永远不阻塞写，写也永远不阻塞读。** 只有写写之间才需要协调。

代价是空间（要存多个版本）和一个新问题：**旧版本什么时候能删？**
——这就是水位线要回答的。

---

## 2. 时间戳：整个系统的骨架

BusTub 用两种时间戳，靠数值范围区分：

```
   0 ─────────────── 已提交事务的 commit_ts ──────────► ... ◄── TXN_START_ID ──► 临时 ts
   └──────────── 真实时间线（单调递增） ─────────────┘     └── 事务运行期间的占位 ──┘
```

| 字段 | 何时确定 | 含义 |
|---|---|---|
| `read_ts` | 事务 `Begin()` 时 | 我的快照点 = 当时的 `last_commit_ts` |
| `commit_ts` | 事务 `Commit()` 时 | `last_commit_ts + 1`，我的修改从此刻起对外可见 |
| 临时 ts（`txn_id_`） | 事务创建时 | 运行期间写入元组的占位标记，值 ≥ `TXN_START_ID` |

### Begin：确定快照（STEP 2）

```cpp
txn_ref->read_ts_.store(last_commit_ts_.load());
```

快照隔离的核心承诺：**事务只看得见「它开始那一刻」已经提交的数据**。
之后无论别人提交了什么，本事务都视而不见 —— 所以读永远不会被写阻塞。

### Commit：把临时标记换成真时间戳（STEP 3-4）

事务运行期间写入的元组带的是 `txn_id_`。别的事务看到这种时间戳就知道"尚未可见"。
提交要做的，就是把 `write_set_` 里所有元组的时间戳**统一替换成 `commit_ts`**：

```cpp
for (const auto &[table_oid, rids] : txn->write_set_) {
  for (const auto &rid : rids) {
    auto meta = table->GetTupleMeta(rid);
    meta.ts_ = commit_ts;          // ← 这一刻起，全部修改同时可见
    table->UpdateTupleMeta(meta, rid);
  }
}
```

这一步保证了**提交的原子性**：不会出现"改了一半可见"的中间状态。

时间戳的分配必须在 `commit_mutex_` 保护下完成。否则两个并发提交可能拿到同一个号，
或者 A 拿到较大的号却先发布，让别的事务看到"时间倒流"的版本序列。

同理，`commit_ts_` 必须先于 `last_commit_ts_` 写入：顺序反了的话，
新开始的事务会拿到一个包含本事务的 `read_ts`，却看到尚未改完的时间戳。

### Abort：直觉是错的（STEP 5）

一开始我按直觉写成"什么都不用做"：本事务的修改一直带着临时时间戳，
从来没对任何人可见过，不换成 `commit_ts` 就等于没发生。

**这个直觉是错的。** 带临时时间戳的行会被写写冲突检测当成
「正被另一个未提交事务持有」，于是后来的事务永远碰不了它 ——
**一个已经死掉的事务把那些行永久锁死了。**

`txn_abort_serializable_test` 的 `SimpleAbortTest` 正是抓这个：
先 `UPDATE ... ; ABORT`，再让另一个事务 `DELETE`，会直接撞上
`write-write conflict detected`。

所以中止必须逐行真正回滚：

```cpp
for (const auto &[table_oid, rids] : txn->GetWriteSets()) {
  for (const auto &rid : rids) {
    if (meta.ts_ != txn->GetTransactionTempTs()) continue;   // 已不归我
    if (有我自己的 undo log) {
      用它 ReconstructTuple 还原内容，时间戳恢复成 log.ts_
      UpdateUndoLink(rid, log.prev_version_);                // 丢弃我这条日志
    } else {
      // 我自己插入的行：撤销后不该存在
      UpdateTupleInPlace(TupleMeta{0, true}, tuple, rid);
    }
  }
}
```

关键在于**恢复时间戳**，让这些行重新变成"无主"状态。

---

## 3. 水位线：旧版本什么时候能删（STEP 1）

```
      已提交的历史版本 ──────────────────────►  时间
              ▲                        ▲
         watermark                last_commit_ts
              │                        │
      <── 可回收 ──┼──── 必须保留 ────►
```

**水位线 = 所有活跃事务中最小的 `read_ts`。**

一个版本只要比水位线还旧，且它上面已经有更新的版本可用，
就再也不可能被任何事务读到 —— 可以安全回收。

### 实现要点

骨架给的 `current_reads_` 是 `unordered_map<timestamp_t, int>`（时间戳 → 引用计数），
求最小值要 O(n) 遍历。但 `AddTxn` / `RemoveTxn` 在每个事务的开始和结束都会被调用，
是热路径 —— `WatermarkPerformance` 测试专门压这一点。

解法：额外维护一个 `std::set<timestamp_t> active_ts_`，取最小值降到 O(1)：

| 容器 | 记录什么 |
|---|---|
| `current_reads_` | 这个时间戳**有几个**事务在用（多个事务可能同时开始） |
| `active_ts_` | **有哪些**时间戳还在被用（计数归零才移除） |

---

## 4. 版本链：最新值 + 一串反向增量（STEP 6）

```
   表堆 (ts=5)          undo log (ts=3)        undo log (ts=1)
   (a=9, b=9, c=9) ──►  「把 a 改回 3」  ──►   「把 b 改回 1」
     最新版本                增量                  增量
```

**表堆里存的永远是最新版本**；历史版本不完整保存，而是记录
「要变回上一版，需要把哪几列改成什么」。

想读 `ts=1` 的快照，就从最新值出发依次套用两条日志。

好处是**未修改的列不重复存储** —— 100 列的宽表只 UPDATE 一列时，
旧版本只占 1 列的空间。代价是读越老的版本要套用越多日志。

`UndoLog::modified_fields_` 是按列的位图，标记这条日志覆盖了哪些列。
`UndoLog::tuple_` 只**紧凑地**存放被标记的那几列，所以解析时必须先按位图
构造出一个"部分模式"，才能正确地把值一个个取出来 —— 这是本任务最容易写错的地方。

---

## 5. 可见性判断：快照隔离的全部规则（STEP 7-8）

给定读事务（`read_ts`）和一条记录，`base_meta.ts_` 有三种情况：

| 情况 | 条件 | 处理 |
|---|---|---|
| ① | `ts_ == txn->GetTransactionTempTs()` | **本事务自己**未提交的修改 → 可见（read-your-own-writes） |
| ② | `ts_ <= read_ts` | 本事务开始前已提交 → 直接可见，不用回溯 |
| ③ | 其它 | 表堆那份对本事务"太新" → 沿版本链回溯，找第一个 `ts <= read_ts` 的日志 |

情况 ③ 走到链尾仍没找到，说明记录在快照时刻**还不存在** → 返回 `nullopt`（不可见）。

### 扫描算子的集成

`SeqScanExecutor` 不能直接用迭代器读出的那份数据 —— 它可能是别的事务尚未提交的
中间状态，或者比本事务的快照更新。必须先 `CollectUndoLogs` 判可见性，
再 `ReconstructTuple` 还原。

> **调试记录**：这一步我一度以为实现有 bug（扫描返回了本该不可见的两行），
> 排查半天才发现是 `cmake --build .` 没有重新链接 `txn_scan_test` 这个目标，
> 跑的是旧二进制。改完 MVCC 相关代码后，务必显式重建对应的测试目标。

集成后 **P3 的 20 个 SQL 测试全部无回归**：P3 场景下元组的 `ts_` 都是 0、
事务的 `read_ts` 也是 0，走情况 ②，行为与原来完全一致。

---

## 6. MVCC 写路径（Task 3）

### 6.1 写写冲突检测（STEP 13）

快照隔离允许「读旧版本」，但**绝不允许两个事务同时改同一行**，
否则后提交者会静默覆盖先提交者，即经典的「丢失更新」(lost update)。

表堆里那一行的时间戳就说明了它归谁：

| 条件 | 含义 | 规则 |
|---|---|---|
| `ts >= TXN_START_ID` 且不是我 | 另一个**未提交**事务正持有 | first-updater-wins：先动手的赢 |
| `ts < TXN_START_ID` 且 `ts > read_ts` | 有人在我开始**之后**提交过 | first-committer-wins：先提交的赢 |

冲突时把事务置为 **TAINTED**（受污染）而不是直接 ABORTED —— 前者表示
"逻辑上已失败、资源还没清理"，由上层决定何时真正 `Abort`。

注意这里**不等待**。等待就变成了阻塞，也就重新引入了 2PL 的问题。

### 6.2 一个事务对同一行只留一条 undo log（STEP 14）

对外界而言，本事务的所有修改是原子生效的，中间态从来不存在，
因此没有必要为它保留可回溯的版本：

| 场景 | 处理 |
|---|---|
| 第一次改这一行 | `GenerateNewUndoLog`，挂到版本链头部 |
| 再次改这一行 | `GenerateUpdatedUndoLog` 就地扩充（把新变化的列**并**进去） |
| 本事务自己插入的行 | 完全不需要 undo log（撤销结果是"不存在"，空版本链正好表达） |

扩充时，**已记录过的列必须保持原值**——那才是真正的旧值，
不能被第二次修改覆盖；只有新变化的列才从当前堆值取。

还有一个隐蔽的坑（`PrepareUndoLog` 里的 `base` 变量）：
如果当前行本身是**删除标记**，那"修改前的样子"就是"不存在"，
必须生成 `is_deleted_ = true` 的日志。漏掉这一步，INSERT 复活一条已删除记录时
会生成普通更新日志，于是老事务回溯时把这一行"还原"成删除前的旧值——
**一条本该不可见的记录复活了。**

### 6.3 更新为什么变回「原地覆盖」（STEP 15、21）

P3 里 UPDATE 是「删除 + 插入」，P4 反过来要求**原地覆盖**。原因是
版本链以 RID 为锚：同一条逻辑记录的所有历史版本必须挂在同一个 RID 上，
老事务才能顺着 undo 链找回自己那个版本。换 RID 就等于换了一条记录，链会断掉。

`UpdateTest1` 用 `TableHeapEntryNoMoreThan(bustub, table_info, 1)` 锁死了这一点。

**唯一的例外是更新主键**：主键索引项是 (键 → RID)，键变了索引项就得换位置，
与"版本链锚在同一 RID"直接冲突。两个约束无法同时满足，只能退化为删除 + 插入。
此时必须**先全部删除、再全部插入**，否则 `UPDATE t SET a = a + 1` 会让
新键 2 撞上尚未删除的旧键 2，被误判成唯一性冲突。

---

## 7. 索引与主键约束（Task 4）

### 7.1 删除**不摘**主键索引项（STEP 19）

这与 P3 的做法完全相反，两个理由：

1. 老事务的快照里这一行还活着，它必须仍能通过索引找到这个 RID；
2. 索引项是**唯一性约束的持有者**。摘掉之后，另一个事务就能插入同一个主键、
   拿到一个新的 RID，于是同一个键对应两条记录，唯一性彻底失效。

保留索引项，插入方才会发现"键已存在但那行是墓碑"，从而走**复活**路径（STEP 17）：
在原来的 RID 上原地写入新值。

索引项因此变成了**版本无关的指针**，可见性完全交给版本链判断 ——
所以 `IndexScanExecutor` 也必须和顺序扫描一样做 `CollectUndoLogs` + `ReconstructTuple`（STEP 18）。

### 7.2 二级索引是例外

BusTub 的 P4 只要求**主键索引**支持 MVCC。二级索引不承担唯一性约束、
也不参与复活逻辑，因此沿用 P3 的即时维护语义（删除时摘除、更新时重建）。

> **踩坑记录**：我一度把「删除不摘索引项」推广到了所有索引，
> 结果 `p3.05-index-scan-btree` 回归。因为 BusTub 的 B+ 树索引**只支持唯一键**，
> 陈旧的键项会让后续插入同一个键**静默失败**（`InsertEntry` 返回 false 而我没检查），
> 于是新行进不了索引，走索引的查询直接看不到它。
> 表现是「有序索引扫描少了几行」，与索引维护的关系并不直观。

---

## 8. 垃圾回收与可串行化（Task 5）

### 8.1 垃圾回收（STEP 16）

一条 undo log 还有用，当且仅当**某个可能出现的读者**还需要它。
水位线之下的历史谁都读不到了。

但不能简单地"删掉所有 `ts < watermark` 的日志"：
一个 `read_ts` 恰好等于水位线的事务，仍然需要**第一条 `ts <= watermark` 的日志**
来重建它那个版本。所以每条版本链要保留到"第一个足够旧的版本"为止。

```
版本链：  base(ts=9) → log(ts=7) → log(ts=4) → log(ts=2) → log(ts=1)
watermark = 5                          ▲
                     ├─── 必须保留 ────┤ └──── 可以回收 ────┘
```

BusTub 的回收粒度是**整个事务**：只有当一个事务的所有 undo log
都不再被任何版本链引用时，才能把它从 `txn_map_` 里移除。

### 8.2 可串行化验证（STEP 22-23）

快照隔离挡不住**写偏斜**(write skew)：

```
初始：行 A.x = 1，行 B.x = 0
txn2: UPDATE t SET x = 0 WHERE x = 1   （读到 A，改 A）
txn3: UPDATE t SET x = 1 WHERE x = 0   （读到 B，改 B）
```

两者的写集合**完全不重叠**，没有写写冲突，都能提交。
但结果 `(A.x=0, B.x=1)` 在任何串行顺序下都得不到。

根因是 txn3 读的时候 A.x 还是 1（不匹配它的 `x = 0`），
但 txn2 提交后 A.x 变成了 0 —— 晚一点读就会读到 A。这是幻读的一般形式。

**检测办法**：记住每个事务的读集合，提交时回头检查
「有没有并发事务改出了一行满足我的扫描谓词的数据」。

逐行记录读集合太贵，BusTub 用更粗的粒度：**记下扫描时用的谓词**
（`Transaction::AppendScanPredicate`，仅 SERIALIZABLE 隔离级别下记录）。
这是谓词级检测，会有误报但不会漏报。真正精确的做法（SSI）需要跟踪读写依赖图。

> **最关键的一条判断**：验证时必须**跳过仍带临时时间戳的行**。
> 提交被 `commit_mutex_` 串行化，所以走到验证时，凡是已提交的事务都已经把
> 临时时间戳换成了真实的 `commit_ts`。还带着临时时间戳的就是**尚未提交**的
> 并发事务，它们可能最终中止，拿它们的中间状态判我的冲突毫无道理。
>
> 漏掉这个判断的后果非常反直觉：两个互相冲突的事务里，**先提交的那个**
> 会因为看到后者未提交的修改而误判失败，后者反倒提交成功 —— 完全颠倒了。

---

## 9. 测试结果

```
txn_timestamp_test              2 tests  PASSED
  ├─ TimestampTracking                  ← Begin/Commit/Abort 的时间戳语义
  └─ WatermarkPerformance               ← 水位线必须是 O(log n) 而非 O(n)

txn_scan_test                   3 tests  PASSED
  ├─ TupleReconstructTest               ← 版本链重建（删除标记/全列更新/空更新）
  ├─ CollectUndoLogTest                 ← 可见性判断
  └─ ScanTest                           ← 扫描算子的快照读集成

txn_executor_test              11 tests  PASSED
  ├─ Insert / InsertCommit / InsertDelete / InsertDeleteConflict
  ├─ GenerateUndoLog
  ├─ UpdateTest1 / UpdateTest2 / UpdateTestWithUndoLog / UpdateConflict
  └─ GarbageCollection / GarbageCollectionWithTainted

txn_index_test                  5 tests  PASSED
  ├─ IndexInsert                        ← 重复主键 → TAINTED
  ├─ InsertDelete                       ← 删除后索引项保留 + 复活
  ├─ Update / UpdatePrimaryKey          ← 改主键退化为删除+插入
  └─ IndexUpdateConflict

txn_abort_serializable_test     4 tests  PASSED
  ├─ SerializableTest / ConcurrentSerializableTest
  └─ SimpleAbortTest / ...              ← 中止必须真正回滚堆
```

**合计 25 个测试全部通过。** `make format`、`make check-lint` 通过；
**P0–P3 全部无回归**（P3 的 20 个 SQL 测试重跑仍 20/20）。

---

## 10. 一句话小结

MVCC 用**空间换并发**：不覆盖旧数据，让每个事务读自己那一刻的快照，
于是读写彻底解耦。

整套机制其实只靠两个数字撑起来：
- **`read_ts`** 决定"我能看见什么"；
- **`commit_ts`** 决定"别人什么时候能看见我"。

而"旧版本何时能删"这个 MVCC 特有的新问题，答案是第三个数字——
**水位线**，即所有活跃事务中最小的 `read_ts`。

有意思的是，这个"删除只记账、回收交给后台"的模式在这套实验里出现了四次：
P1 缓冲池的脏页、P2 叶子页的墓碑、P3 表堆的 `is_deleted_` 标记、P4 的版本链 + 水位线。
**推迟回收、批量清算**是数据库系统里反复出现的一等公民思想。

而 P4 本身还教了另一课：**很多"显然不用做"的事其实必须做。**
中止看似可以什么都不干（修改从未可见），实际却会把行永久锁死；
删除看似应该摘掉索引项，实际会让唯一性约束失效。
在并发系统里，直觉的可靠性远低于把每条不变量写下来逐一检查。
