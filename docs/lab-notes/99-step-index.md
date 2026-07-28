# 附录 · STEP 标记总索引

源码里每一处 `// ==== Pn STEP k: 标题 ====` 都是一个可独立理解的实现步骤，
`// ---- STEP k.m: ---- ` 是它内部的子阶段。标记散落在 30 多个文件里，
这份索引让你可以从「我想看某个概念」直接跳到对应代码。

**阅读建议**：按 STEP 编号顺序读，它本身就是一条从零搭出数据库的路径。

标记约定见 [总览](./00-overview.md)。

---

## P0 · 并发跳表

配套教学报告：[01-P0-skiplist.md](./01-P0-skiplist.md)

| STEP | 主题 | 位置 |
|---|---|---|
| **0** | 自定义私有辅助函数 | `src/include/primer/skiplist.h` |
| **1** | 哨兵头节点 | `src/include/primer/skiplist.h` |
| **2** | 节点的「高度」就是它的链接数量 | `src/include/primer/skiplist.h` |
| **3** | 只读操作一律用「共享锁」 | `src/primer/skiplist.cpp` |
| **4** | 写操作用「独占锁」 | `src/primer/skiplist.cpp` |
| **5** | 插入的四个阶段 | `src/primer/skiplist.cpp` |
| ⤷ 5.1 | 找到每一层的前驱节点 | |
| ⤷ 5.2 | 去重检查 | |
| ⤷ 5.3 | 掷骰子决定新节点高度 | |
| ⤷ 5.4 | 逐层把新节点接进链表 | |
| **6** | 删除的四个阶段 | `src/primer/skiplist.cpp` |
| ⤷ 6.1 | 同样先定位每层前驱 | |
| ⤷ 6.2 | 确认目标存在 | |
| ⤷ 6.3 | 把目标节点从它出现的每一层上摘下来 | |
| ⤷ 6.4 | 收缩空的高层 | |
| **7** | 查找必须用共享锁 | `src/primer/skiplist.cpp` |
| **8** | 高度 = 链接数量 | `src/primer/skiplist.cpp` |

## P1 · 缓冲池管理器

配套教学报告：[02-P1-buffer-pool.md](./02-P1-buffer-pool.md)

| STEP | 主题 | 位置 |
|---|---|---|
| **1** | 用「侵入式迭代器」把链表操作降到 O(1) | `src/include/buffer/arc_replacer.h` |
| **2** | 决定从哪一侧淘汰 | `src/buffer/arc_replacer.cpp` |
| **3** | 四种情况分别处理 | `src/buffer/arc_replacer.cpp` |
| **4** | 只保留最近 K 个时间戳 | `src/include/buffer/lru_k_replacer.h` |
| **5** | 在「+inf 组」和「有限组」之间做两级比较 | `src/buffer/lru_k_replacer.cpp` |
| **6** | 磁盘调度器 = 请求队列 + 一个后台工作线程 | `src/storage/disk/disk_scheduler.cpp` |
| **7** | 让帧自己记住它装的是哪个页 | `src/include/buffer/buffer_pool_manager.h` |
| **8** | 三个私有辅助函数 | `src/include/buffer/buffer_pool_manager.h` |
| **9** | 分配页号 | `src/buffer/buffer_pool_manager.cpp` |
| **10** | 清理一个页要覆盖它所有可能的藏身之处 | `src/buffer/buffer_pool_manager.cpp` |
| **11** | 「Unsafe」到底不安全在哪 | `src/buffer/buffer_pool_manager.cpp` |
| **12** | guard 构造 = 获取页锁 | `src/storage/page/page_guard.cpp` |
| **13** | 移动 = 所有权转移，不做任何加解锁 | `src/storage/page/page_guard.cpp` |
| **14** | 释放顺序 | `src/storage/page/page_guard.cpp` |
| **15** | 何时置脏位 | `src/storage/page/page_guard.cpp` |

## P2 · B+ 树索引与可扩展哈希

配套教学报告：[03-P2-index.md](./03-P2-index.md)

| STEP | 主题 | 位置 |
|---|---|---|
| **0** | 这 12 个字节就是每一页的公共页头 | `src/include/storage/page/b_plus_tree_page.h` |
| **1** | 页头就是对那 4KB 前 12 个字节的解释 | `src/storage/page/b_plus_tree_page.cpp` |
| **2** | 最小容量取 floor(max/2) | `src/storage/page/b_plus_tree_page.cpp` |
| **3** | Init 取代构造函数 | `src/storage/page/b_plus_tree_leaf_page.cpp` |
| **4** | 有序数组的插入 | `src/storage/page/b_plus_tree_leaf_page.cpp` |
| **5** | 延迟删除的核心 | `src/storage/page/b_plus_tree_leaf_page.cpp` |
| **6** | 分裂时墓碑要按相对顺序切分 | `src/storage/page/b_plus_tree_leaf_page.cpp` |
| **7** | 合并时墓碑串接，超容量则从队首兑现 | `src/storage/page/b_plus_tree_leaf_page.cpp` |
| **8** | 内部页的第 0 个键永远是无效的 | `src/storage/page/b_plus_tree_internal_page.cpp` |
| **9** | 迭代器为什么要一直握着 ReadPageGuard | `src/include/storage/index/index_iterator.h` |
| **10** | 为什么这里只剩下模板实例化 | `src/storage/index/index_iterator.cpp` |
| **11** | 读路径的螃蟹锁 | `src/storage/index/b_plus_tree.cpp` |
| **12** | 插入的五条分支 | `src/storage/index/b_plus_tree.cpp` |
| ⤷ 12.0 | 先赌一把「只影响叶子」 | |
| ⤷ 12.1 | 空树 → 直接建一个叶子当根 | |
| ⤷ 12.2 | 键已存在的两种情况 | |
| ⤷ 12.3 | 有空位，直接插 | |
| ⤷ 12.4 | 叶子已满 → 先对半切开，再把新键插进该去的那一半 | |
| **13** | B+ 树是唯一「从叶子往上长」的树 | `src/storage/index/b_plus_tree.cpp` |
| **14** | 删除的两级代价 | `src/storage/index/b_plus_tree.cpp` |
| **15** | 下溢的修复 —— 借用还是合并 | `src/storage/index/b_plus_tree.cpp` |
| ⤷ 15.1 | 选一个兄弟 | |
| ⤷ 15.2 | 合并还是借用 | |
| ⤷ 15.3 | 合并之后回头看父节点 | |
| **16** | 根塌缩 | `src/storage/index/b_plus_tree.cpp` |
| **17** | 乐观加锁 | `src/storage/index/b_plus_tree.cpp` |
| **18** | 根节点的「安全」判据和别人不一样 | `src/storage/index/b_plus_tree.cpp` |
| **19** | 树彻底变空 | `src/storage/index/b_plus_tree.cpp` |
| **20** | 三层结构的最顶层 | `src/storage/page/extendible_htable_header_page.cpp` |
| **21** | header 取**高**位，directory 取**低**位 | `src/storage/page/extendible_htable_header_page.cpp` |
| **22** | 全局深度与局部深度 | `src/storage/page/extendible_htable_directory_page.cpp` |
| **23** | 「分裂镜像」是唯一有资格与我合并的伙伴 | `src/storage/page/extendible_htable_directory_page.cpp` |
| **24** | 目录翻倍 = 把整个数组原样复制一份接在后面 | `src/storage/page/extendible_htable_directory_page.cpp` |
| **25** | 桶页是「无序紧凑数组」，和 B+ 树叶子完全相反 | `src/storage/page/extendible_htable_bucket_page.cpp` |
| **26** | 建表只造一个 header 页 | `src/container/disk/hash/disk_extendible_hash_table.cpp` |
| **27** | 查询路径 —— 三次页访问，全部只用读锁 | `src/container/disk/hash/disk_extendible_hash_table.cpp` |
| **28** | 插入 —— 桶满了就分裂，而不是重建整张表 | `src/container/disk/hash/disk_extendible_hash_table.cpp` |
| **29** | 删除 —— 合并空桶，必要时收缩目录 | `src/container/disk/hash/disk_extendible_hash_table.cpp` |

> STEP 0–19 是 B+ 树，STEP 20–29 是**可扩展哈希表**（F2023 遗留，额外补做）。

## P3 · 查询执行引擎

配套教学报告：[04-P3-execution.md](./04-P3-execution.md)

| STEP | 主题 | 位置 |
|---|---|---|
| **1** | 构造函数只做「登记」，不做任何真实工作 | `src/execution/seq_scan_executor.cpp` |
| **2** | 向量化执行 —— 一次产出一整批 | `src/execution/seq_scan_executor.cpp` |
| **3** | Limit 的价值在于「提前停手」 | `src/execution/limit_executor.cpp` |
| **4** | 写算子是「阻塞式」的 | `src/execution/insert_executor.cpp` |
| **5** | 删除是「打标记」，不是「抹掉」 | `src/execution/delete_executor.cpp` |
| **7** | 点查 —— 索引扫描相对顺序扫描的全部意义 | `src/execution/index_scan_executor.cpp` |
| **8** | 什么样的谓词才能用索引 | `src/optimizer/seqscan_as_indexscan.cpp` |
| **9** | 聚合的增量合并规则 | `src/include/execution/executors/aggregation_executor.h` |
| **10** | 聚合是彻底的「阻塞算子」 | `src/execution/aggregation_executor.cpp` |
| **11** | GROUP BY 里的 NULL 必须特殊处理 | `src/include/execution/plans/aggregation_plan.h` |
| **12** | 外层每推进一条，内层就重来一遍 | `src/execution/nested_loop_join_executor.cpp` |
| **13** | LEFT JOIN 的补空行 | `src/execution/nested_loop_join_executor.cpp` |
| **14** | 索引嵌套循环连接 | `src/execution/nested_index_join_executor.cpp` |
| **15** | 哈希连接的「建表 / 探测」两阶段 | `src/execution/hash_join_executor.cpp` |
| **16** | 什么样的连接条件能用哈希连接 | `src/optimizer/nlj_as_hash_join.cpp` |
| **17** | 为什么要先算出 SortKey | `src/execution/execution_common.cpp` |
| **18** | 为什么中间结果也要落在「页」上 | `src/include/storage/page/intermediate_result_page.h` |
| **19** | 迭代器要同时记住「哪一页」和「页内哪个字节」 | `src/include/execution/executors/external_merge_sort_executor.h` |
| **20** | 二路归并 | `src/execution/external_merge_sort_executor.cpp` |
| **21** | 第一趟 —— 生成初始有序段 | `src/execution/external_merge_sort_executor.cpp` |
| **22** | 后续每一趟 —— 两两归并，段数减半 | `src/execution/external_merge_sort_executor.cpp` |
| **23** | 内存排序 | `src/execution/sort_executor.cpp` |
| **24** | 为什么 TopN 不是「排序 + 取前 N」 | `src/execution/topn_executor.cpp` |
| **25** | 识别 Limit(Sort(...)) 这个模式 | `src/optimizer/sort_limit_as_topn.cpp` |
| **26** | 窗口函数为什么必须是「阻塞算子」中最彻底的那一类 | `src/include/execution/executors/window_function_executor.h` |
| **27** | 窗口函数和聚合的本质区别 | `src/execution/window_function_executor.cpp` |
| **28** | 分区 + 排序 + 累计，以及「同伴行」这个坑 | `src/execution/window_function_executor.cpp` |

> STEP 1–25 是 F2025 的评分范围，STEP 26–28 是**窗口函数**（额外补做）。
>
> **编号里为什么缺 STEP 6**：原本是 `UpdateExecutor` 的「删除 + 插入」写法，
> 被 P4 的 MVCC 整个推翻了（版本链必须锚在同一个 RID 上），代码换成了
> P4 STEP 15 / 20 / 21。缺口是**故意保留**的——它标记着一处"P3 的正确答案
> 在 P4 变成了错误答案"的位置，对照见 [04-P3-执行引擎](./04-P3-execution.md) 第 4.2 节。

## P4 · MVCC 并发控制

配套教学报告：[05-P4-concurrency.md](./05-P4-concurrency.md)

| STEP | 主题 | 位置 |
|---|---|---|
| **1** | 水位线是什么，为什么需要它 | `src/concurrency/watermark.cpp` |
| **2** | 事务开始时确定「读时间戳」 | `src/concurrency/transaction_manager.cpp` |
| **3** | 提交时间戳必须在 commit_mutex_ 保护下分配 | `src/concurrency/transaction_manager.cpp` |
| **4** | 提交 = 把「临时时间戳」换成真正的提交时间戳 | `src/concurrency/transaction_manager.cpp` |
| **5** | 中止 —— 必须把堆上的修改真正撤销 | `src/concurrency/transaction_manager.cpp` |
| **6** | 版本链是「最新值 + 一串反向增量」 | `src/execution/execution_common.cpp` |
| **7** | 可见性判断 —— 快照隔离的全部规则 | `src/execution/execution_common.cpp` |
| **8** | 扫描要读的是「本事务快照下的版本」，不是表堆里最新的那份 | `src/execution/seq_scan_executor.cpp` |
| **9** | 本事务第一次修改这条记录 —— 生成一条新的 undo log | `src/execution/execution_common.cpp` |
| **10** | 本事务**再次**修改同一条记录 —— 就地扩充已有的 undo log | `src/execution/execution_common.cpp` |
| **11** | 把整张表的版本链打印出来 | `src/execution/execution_common.cpp` |
| **12** | 新插入的行先带「临时时间戳」 | `src/execution/insert_executor.cpp` |
| **13** | 写写冲突检测 | `src/execution/execution_common.cpp` |
| **14** | 一个事务对同一行只留一条 undo log | `src/execution/execution_common.cpp` |
| **15** | 普通更新是「原地覆盖」，不是删除+插入 | `src/execution/update_executor.cpp` |
| **16** | 垃圾回收 —— MVCC 必须付的税 | `src/concurrency/transaction_manager.cpp` |
| **17** | 主键唯一性 —— 插入前必须先查索引 | `src/execution/insert_executor.cpp` |
| **18** | 索引扫描同样要做可见性判断 | `src/execution/index_scan_executor.cpp` |
| **19** | MVCC 下删除**不摘**索引项 | `src/execution/delete_executor.cpp` |
| **20** | 先收集，再统一修改 | `src/execution/update_executor.cpp` |
| **21** | 改主键时只能「删除 + 插入」 | `src/execution/update_executor.cpp` |
| **22** | 可串行化需要记住「我读过什么」 | `src/execution/index_scan_executor.cpp` + `src/execution/seq_scan_executor.cpp` |
| **23** | 可串行化验证 —— 抓「写偏斜」 | `src/concurrency/transaction_manager.cpp` |

