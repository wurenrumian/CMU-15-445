# 源码导览

| 想理解什么 | 从哪里开始 |
|---|---|
| 并发有序结构 | `src/primer/skiplist.cpp` |
| 页缓存与淘汰 | `src/buffer/buffer_pool_manager.cpp`、`arc_replacer.cpp` |
| 页生命周期 | `src/storage/page/page_guard.cpp` |
| B+Tree 索引 | `src/storage/index/b_plus_tree.cpp` |
| SQL 执行 | `src/execution/*_executor.cpp` |
| MVCC 版本链 | `src/execution/execution_common.cpp` |
| 时间戳与提交 | `src/concurrency/transaction_manager.cpp` |
| 日志与检查点 | `src/recovery/log_manager.cpp`、`checkpoint_manager.cpp` |

## 推荐的追踪顺序

```text
Execute(sql) → Planner → Executor::Next → TableHeap / Index
             → BufferPoolManager → DiskManager（4KB page）
```

从一个入口函数向下走，再回头补数据结构；并发问题则从 `TransactionManager` 的提交/中止入口开始，检查锁表、Undo Log、版本链和索引是否都更新。阅读时同时打开对应测试文件，测试名称通常就是模块必须保持的不变量。
