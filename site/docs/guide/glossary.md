# 术语词典

**Page**：磁盘读写的固定大小单位，BusTub 默认 4KB。

**Frame**：Buffer Pool 中承载一个 Page 的内存槽位。

**RID**：由 Page ID 和 Slot Number 组成的稳定记录定位符。

**Pin Count**：正在使用某页的引用数量；非零页面不能被替换。

**Tombstone**：延迟物理删除的标记，先记录删除，再批量清理。

**Undo Log**：描述如何恢复到上一版本的增量日志。

**Watermark**：所有活跃事务中最小的读取时间戳，用来判断旧版本何时可回收。

**WAL**：Write-Ahead Logging，数据页落盘前先保证对应日志持久化。

**LSN**：Log Sequence Number，用于比较日志与数据页的新旧关系。

**Page Table**：把 Page ID 映射到 Buffer Pool 中 Frame ID 的内存表。

**Dirty Page**：已经在内存中修改、但尚未写回磁盘的页，换出前必须先 flush。

**Latch**：保护短临界区内存结构的轻量同步原语；它和事务语义上的 Lock 不是一回事。

**Snapshot**：事务读取时所依据的逻辑时间点，MVCC 用它判断版本是否可见。

**Compaction**：LSM-Tree 后台合并 SSTable、清理旧版本和删除标记的过程。

## 最容易混淆的三组词

| 词组 | 区别 |
|---|---|
| Page / Frame | Page 是持久化对象，Frame 是承载它的内存槽位。 |
| Lock / Latch | Lock 保护事务读写，Latch 保护实现数据结构的短时一致性。 |
| WAL / Undo Log | WAL 面向崩溃恢复，Undo Log 面向快照读取与事务回滚。 |
