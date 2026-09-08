# 数据库系统总览

一个数据库系统同时解决四件事：把数据可靠地放在存储介质上、用索引找到它、把 SQL 变成执行计划，并让并发事务看到一致的结果。

```text
SQL → Parser / Binder → Planner / Optimizer → Executor
                                      ↓
                              Tuple / RID / Index
                                      ↓
                            Transaction + MVCC
                                      ↓
                          Buffer Pool → Disk / WAL
```

BusTub 把这条路径拆成 P0–P4。它不是一个“玩具 SQL 解析器”，而是一台缩小的关系数据库：内存可以丢失，页可以被替换，事务可能冲突，机器可能崩溃。

## 两类工作负载

OLTP 追求短事务和低延迟，偏好行存储、B+Tree 和精确更新；OLAP 追求高吞吐扫描，偏好列存储、向量化执行和批量聚合。现代系统往往把两类技术组合起来。

## 一个查询的完整旅程

以 `SELECT name FROM users WHERE id = 42` 为例：Parser 识别语法，Binder 确认表和列存在；Optimizer 比较 SeqScan 与 IndexScan 的代价；Executor 调用索引找到 RID，再通过 Buffer Pool 读取对应页。若该行有多个版本，事务层还要用读取时间戳过滤不可见版本，最后只返回投影后的 `name`。

每一层都有自己的失败模式：列名不存在在 Binder 失败，页不在缓存会触发 I/O，索引损坏会破坏查找不变量，事务冲突则可能导致回滚。把错误放回所属层，是定位问题最快的方法。

## 读懂系统的三个不变量

- **身份稳定**：RID 通过 Page ID + Slot 定位，而不是暴露内存地址。
- **资源有界**：Pin count 非零的页不能被替换，所有 Guard 都必须最终释放。
- **结果可解释**：执行器输出必须遵守计划的 schema、顺序和可见性规则。
