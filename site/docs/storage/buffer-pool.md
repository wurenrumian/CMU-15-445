# Buffer Pool 深入

缓冲池是磁盘和执行器之间唯一的中转站。它把昂贵的 I/O 变成可替换的内存缓存，同时保证正在使用的页不会被淘汰。

## 一次读取发生了什么

```text
CheckedReadPage(42)
       ↓
page_table 命中？ ── 是 ──→ pin++ → ReadPageGuard
       │ 否
free frame / replacer 选择 victim
       ↓
脏页先 Flush → DiskManager 读取 42 → pin=1
```

### 三个必须同时成立的不变量

| 不变量 | 保护机制 |
|---|---|
| 正在使用的页不能被换出 | Pin count / PageGuard |
| 脏页换出前必须落盘 | Dirty bit |
| 页表与帧内容必须一致 | BPM latch |

## 替换算法的取舍

LRU 只看最近一次访问；LRU-K 看第 K 次历史访问；ARC 把“最近访问”和“频繁访问”拆成两条自适应队列。顺序扫描会污染缓存，好的替换器要识别这种访问模式。

## 为什么 PageGuard 很重要

`ReadPageGuard` / `WritePageGuard` 是 RAII：离开作用域时自动 Unpin，写守卫还会标记脏页。它把“记得释放 pin”从调用者的习惯，变成编译器可以检查的生命周期。
