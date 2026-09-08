# B+ Tree vs LSM-Tree

BusTub 的 P2 让你实现了磁盘友好的 B+Tree。真实系统里，另一条主流路线是 LSM-Tree：把随机更新改造成顺序写，再用后台 Compaction 合并数据。

| 维度 | B+ Tree | LSM-Tree |
|---|---|---|
| 写入 | 原地更新，可能随机 I/O | 追加写 MemTable / SSTable |
| 点查 | 稳定的 O(log n) | 可能查多个层级 |
| 范围查 | 叶子链天然有序 | 需要归并多个 SSTable |
| 空间代价 | 页内碎片与分裂 | Compaction / 写放大 |
| 典型场景 | OLTP、需要低延迟更新 | 写入密集、日志与时序数据 |

```text
写入 → MemTable → Flush → SSTable L0
                         ↓ Compaction
                 更大的有序 SSTable
```

## 什么时候选哪一个？

如果工作负载需要频繁按主键更新、并且希望单次读取延迟稳定，B+ Tree 往往更直接；如果写入远多于读取，且可以接受后台合并，LSM-Tree 能把磁盘写入变成更连续的吞吐。

BusTub 的可扩展哈希扩展也提供了第三个参照：它用空间换点查速度，但没有 B+ Tree 的范围顺序。
