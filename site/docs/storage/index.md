# 存储系统专题

BusTub 的核心问题可以浓缩成一句话：**如何把不可靠的内存，变成可靠、可并发、可恢复的数据系统？**

P1 的缓冲池、P2 的索引和 P4 的版本链，分别回答了缓存、组织和时间这三个问题。本专题沿着同一条线继续向下，连接到真实数据库里常见的页布局、WAL、LSM-Tree 和列式存储。

## 四个视角

<div class="storage-cards">
<a href="/storage/page-layout"><b>01</b><strong>Page & Tuple Layout</strong><span>一行记录如何变成 4KB 页中的字节？</span></a>
<a href="/storage/buffer-pool"><b>02</b><strong>Buffer Pool</strong><span>内存满了，数据库决定把谁送回磁盘。</span></a>
<a href="/storage/recovery"><b>03</b><strong>WAL & Recovery</strong><span>机器突然断电，提交过的数据如何回来？</span></a>
<a href="/storage/index-structures"><b>04</b><strong>B+ Tree vs LSM</strong><span>随机更新与顺序写入，两条存储路线。</span></a>
</div>

## 一条存储链

```text
Tuple → Slotted Page → Buffer Pool → Disk File
   ↓          ↓              ↓            ↓
  RID      Page ID       Replacement     WAL
   └────────────── 查询执行与事务 ──────────────┘
```

这也是阅读源码的推荐顺序：先理解物理布局，再看页如何进出内存，最后看系统如何在故障后恢复一致性。
