# 日志与恢复深入

WAL 让数据库在断电后重建一致状态：先把描述修改的日志持久化，再允许脏数据页落盘。

## ARIES 三阶段

- **Analysis**：从检查点和日志重建活跃事务表、脏页表。
- **Redo**：从最早可能缺失的 LSN 开始重放历史修改；PageLSN 可避免重复应用。
- **Undo**：从未提交事务的末尾反向撤销，并写 Compensation Log Record。

Steal / No-Steal 决定未提交页能否提前写盘；Force / No-Force 决定提交时是否必须把所有数据页写盘。WAL 通常选择 Steal + No-Force，以换取更高吞吐，再依赖 Undo / Redo 保证正确性。

BusTub 的 `LogManager`、`CheckpointManager` 和 `LogRecovery` 是阅读这些概念的入口。
