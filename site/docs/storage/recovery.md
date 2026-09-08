# WAL 与崩溃恢复

写入磁盘的顺序决定了数据库能否在断电后保持一致。WAL（Write-Ahead Logging）的规则只有一句：**数据页落盘之前，对应的日志必须先落盘。**

## 提交与恢复

```text
UPDATE tuple
   ↓
append log (LSN=105)
   ↓ fsync log
COMMIT acknowledged
   ↓（稍后）
flush dirty page

断电 → Analysis → Redo committed changes → Undo incomplete changes
```

BusTub 的 `LogManager`、`CheckpointManager` 与 `LogRecovery` 对应这条链路。Checkpoint 不要求暂停所有事务，它记录一个可以缩短恢复扫描范围的安全点。

## Redo / Undo 直觉

- **Redo**：日志比页新，说明页的修改还没落盘，重放它。
- **Undo**：事务没有提交，沿着它的日志反向撤销。
- **LSN**：页头保存 PageLSN，用来判断某条日志是否已经体现在页上。

WAL 和 P4 的 Undo Log 解决的是两个相邻但不同的问题：前者面对“机器死了”，后者面对“事务要回到自己的快照”。
