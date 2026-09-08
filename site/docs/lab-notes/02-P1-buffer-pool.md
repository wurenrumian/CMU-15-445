# P1 · 缓冲池管理器（Buffer Pool Manager）

**涉及文件**
- `src/buffer/arc_replacer.cpp` + `src/include/buffer/arc_replacer.h`
- `src/buffer/lru_k_replacer.cpp` + `src/include/buffer/lru_k_replacer.h`
- `src/storage/disk/disk_scheduler.cpp`
- `src/storage/page/page_guard.cpp` + `src/include/storage/page/page_guard.h`
- `src/buffer/buffer_pool_manager.cpp` + `src/include/buffer/buffer_pool_manager.h`

**测试**：`arc_replacer_test`、`arc_replacer_performance_test`、`lru_k_replacer_test`、
`disk_scheduler_test`、`page_guard_test`、`buffer_pool_manager_test`（共 14 个用例，明细见第 7 节）

---

## 1. 缓冲池要解决什么问题

数据库的数据总量远大于内存。缓冲池是**磁盘与内存之间唯一的中转站**，它要同时做到：

1. **透明**：上层只说"我要 page 42"，不关心它在内存还是磁盘。
2. **缓存**：热点页留在内存，冷页换出去 → 需要**替换策略**。
3. **正确**：脏页换出前必须落盘，否则丢数据 → 需要**脏位**。
4. **并发**：多线程同时访问不同页应当互不阻塞 → 需要**分层加锁**。
5. **安全**：正在被使用的页绝不能被换出 → 需要**引用计数（pin）**。

### 整体结构

```
       CheckedReadPage(42)                      CheckedWritePage(42)
              │                                          │
              ▼                                          ▼
      ┌───────────────────────────────────────────────────────────┐
      │              BufferPoolManager                            │
      │  page_table_ : page_id → frame_id     (哈希表)            │
      │  free_frames_: 空闲帧链表                                  │
      │  replacer_   : ArcReplacer（决定淘汰谁）                    │
      │  bpm_latch_  : 保护上面这些「元数据」                        │
      └───────────────────────────┬───────────────────────────────┘
                                  │
      frames_[0]   frames_[1]   frames_[2]  ...   frames_[n-1]
      ┌────────┐   ┌────────┐   ┌────────┐        ┌────────┐
      │FrameHdr│   │FrameHdr│   │FrameHdr│        │FrameHdr│
      │ rwlatch│   │ rwlatch│   │ rwlatch│        │ rwlatch│  ← 每帧一把读写锁
      │ pin=1  │   │ pin=0  │   │ pin=2  │        │ pin=0  │  ← 引用计数
      │ dirty  │   │        │   │        │        │ dirty  │
      │ 4KB    │   │ 4KB    │   │ 4KB    │        │ 4KB    │  ← 页数据
      └────────┘   └────────┘   └────────┘        └────────┘
                                  │
                          DiskScheduler（请求队列 + 后台线程）
                                  │
                             DiskManager（真正读写文件）
```

---

## 2. Task 1a：ARC 替换器

BusTub F2025 把默认替换策略从 LRU-K 换成了 **ARC (Adaptive Replacement Cache)**，
这是 IBM 在 2003 年提出、后来被 ZFS 采用的算法。它比 LRU / LFU 都好在**能自适应**。

### 2.1 为什么需要 ARC

- **纯 LRU 的死穴**：一次全表扫描会把整个缓冲池冲刷一遍（cache pollution），
  把真正的热点页全挤出去，而那些扫描页之后再也不会被访问。
- **纯 LFU 的死穴**：早期积累了高频次的页会赖着不走，工作集变化后无法适应。

ARC 的答案：**同时维护"最近性"和"频繁性"两条队列，并让二者的配额自动浮动。**

### 2.2 四条链表

```
   [<──── mru_ghost_ ────][<──── mru_ ────] ! [──── mfu_ ────>][──── mfu_ghost_ ────>]
        (B1, 只有 page_id)   (T1, 占内存帧)      (T2, 占内存帧)     (B2, 只有 page_id)
                                             ↑
                                        缓存的"中心"，越靠近越新
```

| 链表 | 论文记号 | 含义 | 占内存帧？ |
|---|---|---|---|
| `mru_` | T1 | 只被访问过 **1 次** 的页 | ✅ |
| `mfu_` | T2 | 被访问过 **≥2 次** 的页 | ✅ |
| `mru_ghost_` | B1 | 刚从 T1 淘汰的页，只记 `page_id` | ❌ |
| `mfu_ghost_` | B2 | 刚从 T2 淘汰的页，只记 `page_id` | ❌ |

**幽灵链表是 ARC 的灵魂。** 它们不占任何内存帧，只是"记忆"。
它们的唯一作用是提供反馈信号：

- 命中 `mru_ghost_` → "刚被我淘汰的最近页马上又要了" → 最近性这一侧空间不够 → **调大 p**
- 命中 `mfu_ghost_` → "刚被我淘汰的频繁页马上又要了" → 频繁性这一侧空间不够 → **调小 p**

其中 `p = mru_target_size_` 是我们希望 `mru_` 占据的帧数目标。

不变量：`|T1| + |B1| ≤ c`，四表总元素数 `≤ 2c`（`c = replacer_size_`）。

### 2.3 `RecordAccess` 的四种情况（`STEP 3`）

| 情况 | 条件 | 动作 |
|---|---|---|
| **1** | 命中 `mru_` 或 `mfu_` | 无条件晋升到 `mfu_` 头部（第二次访问 ⇒ 有复用价值）。`evictable_` 保持不变 |
| **2** | 命中 `mru_ghost_` | `p = min(p + max(\|B2\|/\|B1\|, 1), c)`；从 B1 摘除；**放进 `mfu_`** |
| **3** | 命中 `mfu_ghost_` | `p = max(p − max(\|B1\|/\|B2\|, 1), 0)`；从 B2 摘除；放进 `mfu_` |
| **4** | 全都没命中 | 先维护不变量（见下），再放进 `mru_` 头部 |

情况 2/3 里"幽灵复活"直接进 `mfu_` 而不是 `mru_`：它已经被访问过至少两次
（一次进 T1，被淘汰后又访问一次），按定义就属于频繁页。

调整量里的除法 `|B2|/|B1|` 是按两侧幽灵表的相对长度成比例调整。
整数除法结果可能是 0，所以要 `max(..., 1)` 保证每次至少动 1。

情况 4 的不变量维护（论文的 Case IV A / IV B）：

```cpp
if (|T1| + |B1| == c) {                       // 4A：最近性一侧满了
    if (|T1| < c) DropOldestGhost(B1);        //   丢最旧的幽灵腾名额
    else          /* 直接丢 T1 尾部实页 */;    //   极端情况，缓冲池用法下几乎不触发
} else if (|T1| + |B1| < c && 总数 >= c) {     // 4B
    if (总数 == 2c) DropOldestGhost(B2);      //   顶到上限才丢
}
```

### 2.4 `Evict`：从哪一侧淘汰（`STEP 2`）

论文的 REPLACE 判据是
`|T1| ≥ 1 && (|T1| > p || (x ∈ B2 && |T1| == p))`。
BusTub 的 writeup 要求去掉"上一次访问是否来自 B2"这个检查（论文本身也说这个边界是任意的），
于是 `|T1| == p` 归到哪边就自由了。**本实现把它归给 `mru_`**：

```cpp
bool prefer_mru = !mru_.empty() && mru_.size() >= mru_target_size_;
```

> 这个 `>=` 而不是 `>` 是从测试反推出来的：`ArcReplacerTest.SampleTest2` 中
> `|T1| = 1, p = 1` 时期望淘汰 T1 的元素（frame 2）。写成 `>` 会淘汰 T2，直接挂掉。

第二条改动：**跳过被 pin 住的帧**。若首选一侧全部被 pin，就去另一侧找。
关键细节——淘汰出去的页进入的是**它实际所属那条链表**对应的幽灵表，
而不是首选一侧的幽灵表。否则会把频繁页的历史记到最近性账上，污染 p 的自适应。

### 2.5 性能：为什么必须用侵入式迭代器（`STEP 1`）

`arc_replacer_performance_test` 用 **26 万个帧**反复 `RecordAccess`。
如果每次命中都用 `std::find` 在链表里线性查找，单轮就是 O(n²)。

解法：把条目在 `std::list` 中的位置（迭代器）直接存进 `FrameStatus`：

```cpp
struct FrameStatus {
  ...
  std::list<frame_id_t>::iterator alive_it_;   // 在 mru_/mfu_ 中的位置
  std::list<page_id_t>::iterator  ghost_it_;   // 在 mru_ghost_/mfu_ghost_ 中的位置
};
```

`std::list` 的迭代器在其它元素增删时不会失效，这正是它适合做侵入式索引的原因。
于是"把某帧移到 MFU 头部" = `erase(it)` + `push_front()`，两次 O(1)。

**实测结果：平均 0.071 秒 / 轮，阈值是 3 秒，有 42 倍余量。**

### 2.6 `Remove` 与 `Evict` 的区别

| | `Evict` | `Remove` |
|---|---|---|
| 谁调用 | 缓冲池需要空闲帧时 | `DeletePage` |
| 选谁 | 由 ARC 算法决定 | 调用方指定 |
| 留幽灵 | ✅ 留（要用来调 p） | ❌ 不留 |

`Remove` 不留幽灵是因为：那个页在磁盘上都要被删了，保留"它曾在缓存中"的历史
毫无意义，反而会污染 p 的自适应。

---

## 3. Task 1b：LRU-K 替换器

虽然 BPM 用的是 ARC，LRU-K 仍是必做项（也是经典考点）。

**Backward k-distance** = 当前时刻 − 倒数第 K 次访问的时刻。淘汰它最大的那一帧。
访问次数不足 K 次的帧，其 k-distance 定义为 **+∞**，优先淘汰；
多个 +∞ 之间用经典 LRU（最早那次访问最久远者先走）打平局。

### 实现要点

**① 只保留最近 K 个时间戳**（`STEP 4`）

计算只需要"倒数第 K 次"，更早的历史毫无用处。队列长度裁剪到 K，
内存占用是 O(K) 而不是 O(访问次数)。

**② 不用真的做减法**

"k-distance 最大" ⟺ "倒数第 K 次访问的时间戳最小"（因为"当前时刻"对所有候选相同）。
所以 `Evict` 里只比较 `history_.front()`，省掉减法也避免了溢出问题。

**③ 两级比较**（`STEP 5`）

```cpp
std::optional<frame_id_t> inf_victim;      // 历史不满 K 次的候选
std::optional<frame_id_t> finite_victim;   // 历史满 K 次的候选
// ... 各自记录 EarliestTimestamp() 最小者 ...
auto victim = inf_victim.has_value() ? inf_victim : finite_victim;   // +∞ 组优先
```

**④ `curr_size_` 只在 `evictable_` 真正翻转时才 ±1**

`SetEvictable` 被同值重复调用是很常见的（缓冲池每次访问都会调），
不加 `if (当前状态 == 目标状态) return;` 会导致计数错乱。

---

## 4. Task 2：磁盘调度器

```
   调用方 ──Schedule(requests)──▶ Channel<optional<DiskRequest>> ──▶ 后台线程 ──▶ DiskManager
      │                                                                  │
      └───────────────── future.get() 阻塞等待 ◀── promise.set_value(true)┘
```

### 为什么要这一层

1. **DiskManager 不是线程安全的**（内部共享一个文件流的读写偏移）。
   交给**单个**后台线程独占访问，天然消除竞争，调用方也不必自己加锁。
2. **解耦提交与等待**：调用方可以先提交一批请求再统一等待。

### 实现要点

**① `DiskRequest` 是 move-only 的**（内含 `std::promise`），入队必须 `std::move`。

**② 毒丸（poison pill）退出**：析构函数往队列放一个 `std::nullopt`，
工作线程收到就退出循环，`join()` 才能返回。因为毒丸排在队尾，
它之前的所有请求都会被正常处理完。

**③ 骨架的陷阱**：原始骨架里 `UNIMPLEMENTED(...)` 写在
`background_thread_.emplace(...)` **之前**。必须删掉它，
否则线程根本没启动，任何 `Schedule` 都会永远卡在 `future.get()` 上。

---

## 5. Task 3：页守卫（Page Guard）

`ReadPageGuard` / `WritePageGuard` 是 RAII 对象，是**上层访问页数据的唯一途径**。
它同时管两样东西：一把页锁 + 一个引用计数。

### 5.1 生命周期

| 阶段 | 动作 |
|---|---|
| 构造（`STEP 12`、`15`） | `rwlatch_.lock_shared()` / `lock()`；写守卫额外置脏 |
| 移动（`STEP 13`） | **只转移所有权，不碰锁**；源对象置 `is_valid_ = false` |
| `Flush()` | 提交写请求并等待；清脏位 |
| `Drop()`（`STEP 14`） | 解页锁 → 在 bpm 锁内 `pin--`（归零则设为可驱逐）→ 断开 shared_ptr |
| 析构 | 调 `Drop()` |

### 5.2 移动语义的两个必答题

**① 移动构造为什么不能加锁也不能解锁？**

那把页锁已经被 `that` 锁上了，移动只是**换一个对象来负责解锁**。
再 `lock()` 会自死锁；`unlock()` 则会提前放锁。`pin_count_` 同理不变——
页的使用者数量没变，只是"谁拿着凭证"变了。

**必须把 `that.is_valid_` 置 false**，否则 `that` 析构时会再 `Drop()` 一次：
同一把锁解两次、`pin_count_` 减两次，直接崩溃或计数错乱。

**② 自我移动赋值（`guard = std::move(guard)`）**

```cpp
if (this == &that) return *this;
```

少了这一行，`Drop()` 会先把自己彻底释放，然后再从"已被清空的自己"拷贝字段，
`pin_count_` 就凭空少了 1。`PageGuardTest.MoveTest` 专门测这一点。

### 5.3 `Drop()` 的释放顺序（`STEP 14`）

```cpp
is_valid_ = false;            // 先置无效，保证幂等
frame_->rwlatch_.unlock();    // ① 先放页锁
{                             // ② 再在 bpm 锁内改 pin
  std::scoped_lock lock(*bpm_latch_);
  if (frame_->pin_count_.fetch_sub(1) == 1) {
    replacer_->SetEvictable(frame_->frame_id_, true);
  }
}
frame_.reset(); replacer_.reset(); disk_scheduler_.reset(); bpm_latch_.reset();  // ③
```

- **① 在 ② 之前**：此刻 `pin_count_` 还 > 0，替换器不会选中这一帧，
  别人不可能把它淘汰并改写内容。反过来若先把 pin 降到 0 再解锁，
  中间那个窗口里别的线程可以淘汰这一帧、装入新页，而我们还锁着它。
- **② 必须整体在 bpm 锁内**：否则出现这个交错——
  我们 `pin 1→0`，还没 `SetEvictable(true)`；另一线程 `FetchFrame` 同一页
  （`pin 0→1`、`SetEvictable(false)`）；然后我们的 `SetEvictable(true)` 执行，
  把一个**正在被使用**的帧标成了可驱逐。
- **③ `bpm_latch_.reset()` 必须在 `scoped_lock` 作用域之外**，
  否则会先销毁自己正持有的那把锁的最后一个引用。

`fetch_sub` 返回的是**减之前**的值，等于 1 说明自己是最后一个使用者。

### 5.4 脏位的时机（`STEP 15`）

采取保守策略：**拿到写守卫就置脏**，并且 `GetDataMut()` 里**再置一次**。

- 构造时置脏：`GetDataMut()` 只是交出一个裸指针，之后的写入发生在守卫之外，
  我们观察不到。要么保守置脏，要么对 4KB 做校验和比对——后者代价远大于偶尔多写一次盘。
- `GetDataMut()` 里再置一次：因为 `Flush()` 会清脏位。守卫还活着时若继续修改，
  必然要再调一次 `GetDataMut()`/`AsMut()` 拿可写指针，脏位随即被重新置上。
  **少了这一处，`Flush()` 之后的修改会在淘汰时被当成干净页直接丢弃。**

代价是只读却用了写守卫的场景会多写一次盘。**正确性优先于性能。**

---

## 6. Task 4：缓冲池管理器

### 6.1 加锁约定（全篇最重要的一节）

系统里有**两层完全不同**的锁：

| 锁 | 保护什么 | 持有时长 |
|---|---|---|
| `bpm_latch_`（一把 `std::mutex`） | `page_table_`、`free_frames_`、replacer 状态、`pin_count_`、`page_id_` | 极短 |
| `frame->rwlatch_`（每帧一把 `shared_mutex`） | 该帧的 4KB **页数据** | 任意长 |

> **铁律：绝不能持有 `bpm_latch_` 去申请 `frame->rwlatch_`。**

否则：线程 A 长期持有页 0 的写锁 → 线程 B 拿着 `bpm_latch_` 阻塞在页 0 的锁上 →
整个缓冲池对**所有其它页**也全部冻结。这正是 `DeadlockTest` 检测的场景：

```cpp
auto guard0 = bpm->WritePage(pid0);          // 主线程握住 page 0 的写锁
子线程: bpm->WritePage(pid0);                 // 阻塞
sleep(1000ms);
const auto guard1 = bpm->WritePage(pid1);    // ← 若加锁顺序错，这行永远返回不了
```

**解法**：`FetchFrame()` 在返回前就释放 `bpm_latch_`，页锁留到 `PageGuard` 的
构造函数里再取。

**那释放 `bpm_latch_` 之后帧为什么不会被别人淘汰？**
因为 `FetchFrame()` 在锁内已经 `pin_count_++` 并 `SetEvictable(false)`，
替换器不会再选中它。**"先 pin 住，再放锁"是整个设计的安全基础。**

### 6.2 `FetchFrame`：读写两条路径的公共部分（`STEP 8`）

```cpp
auto BufferPoolManager::FetchFrame(page_id, access_type) -> std::shared_ptr<FrameHeader> {
  std::scoped_lock lock(*bpm_latch_);
  if (page_table_ 命中) {
      frame_id = 命中值;                        // 情况 1：零 I/O
  } else {
      auto allocated = AllocateFrame();         // 情况 2：缺页
      if (!allocated) return nullptr;           //   缓冲池耗尽
      frame_id = *allocated;
      SchedulePageIo(读, page_id, frame->GetDataMut());
      frame->page_id_ = page_id;
      page_table_[page_id] = frame_id;
  }
  frame->pin_count_.fetch_add(1);                // ┐
  replacer_->RecordAccess(frame_id, page_id);    // ├ 三件事必须原子
  replacer_->SetEvictable(frame_id, false);      // ┘
  return frame;
}
```

**为什么这三步顺序不能反？**
`RecordAccess` 在幽灵命中时会新建条目，默认 `evictable_ = false`。
若先 `SetEvictable(false)` 再 `RecordAccess`，前者会因为找不到条目而静默返回，
`curr_size_` 就记错了。

**为什么磁盘读放在锁内？**
若在读之前放锁，另一个线程会在页表里看到这个 `page_id`，以为数据已就绪，
从而读到半截数据。要既不阻塞又正确，需要引入"加载中"状态并让其他线程在其上等待——
那是 P1 之外的优化。

### 6.3 `AllocateFrame`：找一个空帧

```
free_frames_ 非空？──是──▶ 直接取
       │否
       ▼
replacer_->Evict() ──失败──▶ 返回 nullopt（缓冲池耗尽，诚实失败，不阻塞）
       │成功
       ▼
脏页？──是──▶ 回写磁盘        ← 干净页可直接丢弃，磁盘那份仍有效
       ▼
page_table_.erase(旧 page_id)
frame->Reset()                 ← 清零很重要：新页若在磁盘上不存在，
frame->page_id_.reset()           读操作不会填满缓冲区，残留旧数据会被当成新页内容
```

**为什么给 `FrameHeader` 加 `std::optional<page_id_t> page_id_`**（`STEP 7`）：
淘汰时需要知道"往哪个 page_id 回写"和"从页表删哪个键"。
不存的话只能反向遍历整个 `page_table_`，每次缺页都要 O(帧数) 线性扫描。
用 `optional` 而非 `INVALID_PAGE_ID` 哨兵，让"这帧是空的"在类型层面就无法被误当成合法页号。

### 6.4 `FlushPage` 的三阶段

不能简单地"拿着 bpm 锁去锁页"（违反铁律），也不能"不加锁直接写"（可能写出半截数据）。
解法是**借用**：

1. bpm 锁内定位并 `pin++`、`SetEvictable(false)` → 放锁
2. 不持 bpm 锁，取页**读锁**，落盘，清脏位
3. bpm 锁内 `pin--`，归零则 `SetEvictable(true)`

`FlushPageUnsafe` 则跳过第 2 步的页锁，因此可能写出"改了一半"的页——
这就是它名字里 Unsafe 的含义。它的脏位必须在**提交写请求之后**才清零，
否则并发写者在我们发起 I/O 前刚改完并置脏，会被紧接着的清零抹掉，那次修改就永远丢了。

`FlushAllPages` 必须先在锁内拷一份页号快照再逐个调 `FlushPage`——
`std::mutex` 不可重入，边持锁边调用会立刻自锁死。

### 6.5 `GetPinCount` 返回 `optional` 的用意

区分"页在内存里但没人用"（返回 0）和"页根本不在内存里"（返回 `nullopt`）。
测试正是靠这个区分来验证淘汰确实发生了：

```cpp
ASSERT_FALSE(bpm->GetPinCount(pageid0).has_value());   // 断言 pageid0 已被换出
```

---

## 7. 测试结果

```
arc_replacer_test                 2 tests   PASSED
arc_replacer_performance_test     1 test    PASSED   平均 0.071s（阈值 3s）
lru_k_replacer_test               1 test    PASSED
disk_scheduler_test               1 test    PASSED
page_guard_test                   2 tests   PASSED   DropTest / MoveTest
buffer_pool_manager_test          7 tests   PASSED
  ├─ VeryBasicTest        (4 ms)
  ├─ PagePinEasyTest      (0 ms)
  ├─ PagePinMediumTest    (1 ms)
  ├─ PageAccessTest    (1198 ms)   ← 读写并发下数据不被篡改
  ├─ ContentionTest     (967 ms)   ← 4 线程 × 10 万次写同一页
  ├─ DeadlockTest      (1001 ms)   ← 验证不持 bpm 锁去抢页锁
  └─ EvictableTest      (274 ms)   ← 1000 轮 × 8 读者，验证 pin 状态始终正确
```

`make format`、`make check-lint` 通过。

---

## 8. 一句话小结

缓冲池的全部难点可以浓缩成一句话：
**用两层粒度不同的锁，把"元数据一致性"和"页数据一致性"彻底分开，
并且永远按"先 pin 住、再放元数据锁、最后拿页锁"的顺序推进。**

ARC 则展示了另一件事：好的缓存策略不是更聪明的固定规则，
而是**能从自己的失误中学习**——幽灵链表就是缓存的"错题本"。
