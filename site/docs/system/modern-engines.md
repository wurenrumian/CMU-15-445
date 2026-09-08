# 现代存储引擎

经典 B+Tree 之外，现代系统常用 LSM-Tree 处理高写入吞吐：先写内存中的 MemTable，再顺序刷成 SSTable，后台 Compaction 合并有序文件。

```text
Write → MemTable → WAL → SSTable → Compaction
                         ↑ Bloom Filter
```

Bloom Filter 用极小空间排除“不可能存在”的键；Compaction 重新组织文件，但会产生写放大。列式存储则通过只读取需要的列、压缩重复值和向量化处理来提升分析查询吞吐。

选择哪种引擎取决于读写比例、范围查询需求、延迟目标和可接受的后台 I/O 成本。
