# 查询处理

查询处理把“用户想要什么”逐步变成“机器具体怎么做”。

## 四个阶段

1. **Parser**：把 SQL 文本变成语法树。
2. **Binder**：将表名、列名解析到 Catalog 对象。
3. **Planner / Optimizer**：生成逻辑计划，并用等价规则寻找更快的物理计划。
4. **Executor**：通过 `Init()` 与批量 `Next()` 产出 Tuple。

P3 的执行器包含 SeqScan、IndexScan、Join、Aggregation、Sort、Limit 以及写算子。流式算子边读边产出；排序、聚合和哈希连接等阻塞算子必须先物化部分输入。

## 优化的本质

优化器不改变结果，只改变访问路径：把过滤条件下推、用索引替代全表扫描、把 Nested Loop Join 改成 Hash Join。成本来自页访问、CPU 运算和中间结果大小。

## 流式与阻塞：调试时的分界线

SeqScan、Filter、Projection 通常是流式算子；Sort、Aggregation 和 Hash Join 则是阻塞算子，往往要先读完输入、建立内存结构，才产生第一行结果。遇到“查询迟迟没有输出”时，先确认是否正处于阻塞算子的初始化阶段。

```text
Projection(name)
└─ Filter(age > 18)
   └─ IndexScan(users, idx_age)
```

谓词下推会把条件尽量交给 IndexScan；若索引不匹配，退化为 SeqScan 仍应返回完全相同的结果。优化改变访问路径，不改变正确性。
