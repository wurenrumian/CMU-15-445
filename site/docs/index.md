---
layout: home
hero:
  name: BusTub / Atlas
  text: 一条 SQL，如何穿过整座数据库
  tagline: 从并发跳表到 MVCC，把一个教学数据库拆成可走进去的五层结构。
  actions:
    - theme: brand
      text: 开始阅读实验报告
      link: /lab-notes/00-overview
    - theme: alt
      text: 先看系统地图
      link: '#system-map'
features:
  - icon: 01
    title: 4KB 是起点
    details: 数据库不信任内存。所有模块最终都围绕磁盘页、缓存与回写展开。
  - icon: 02
    title: 五个 Project，一条通路
    details: P0 → P4 逐层叠加，从指针结构走到事务可见性。
  - icon: 03
    title: 98 个测试，97 个通过
    details: 86 个评分测试全部通过，另完成可扩展哈希与窗口函数扩展。
---

<div id="system-map" class="atlas-map">
  <div class="map-kicker">THE DATA PATH</div>
  <div class="map-row"><span class="map-node query">SQL 查询</span><span class="map-arrow">→</span><span class="map-node">Parser · Binder · Planner</span><span class="map-arrow">→</span><span class="map-node accent">执行器树</span></div>
  <div class="map-row"><span class="map-node">Tuple / RID</span><span class="map-arrow">↓</span><span class="map-node">MVCC 快照</span><span class="map-arrow">↓</span><span class="map-node">B+ 树 / Hash</span></div>
  <div class="map-row"><span class="map-node">页守卫</span><span class="map-arrow">↓</span><span class="map-node">ARC / LRU-K</span><span class="map-arrow">↓</span><span class="map-node accent">DiskManager · 4KB page</span></div>
</div>

## 这不是 API 清单，是一次拆机

每份报告都保留了实现时真正影响正确性的推理：为什么读锁必须共享、墓碑为什么延迟兑现、聚合为什么要填哑 RID，以及中止事务为什么必须回滚。按 Project 阅读，或从 [STEP 标记总索引](/lab-notes/99-step-index) 反向检索代码。

<div class="project-grid">
  <a href="/lab-notes/01-P0-skiplist" class="project-card"><b>P0</b><strong>跳表</strong><span>有序结构 · 读写锁 · O(log n)</span></a>
  <a href="/lab-notes/02-P1-buffer-pool" class="project-card"><b>P1</b><strong>缓冲池</strong><span>页缓存 · 替换 · RAII 守卫</span></a>
  <a href="/lab-notes/03-P2-index" class="project-card"><b>P2</b><strong>B+ 树索引</strong><span>分裂合并 · Tombstone · 并发</span></a>
  <a href="/lab-notes/04-P3-execution" class="project-card"><b>P3</b><strong>执行引擎</strong><span>向量化 · 火山模型 · 优化器</span></a>
  <a href="/lab-notes/05-P4-concurrency" class="project-card"><b>P4</b><strong>MVCC</strong><span>版本链 · 水位线 · 可串行化</span></a>
</div>

<div class="atlas-note"><span>一句话小结</span><p>先把数据放在页里，再让页安全地进出内存；然后在页之上组织索引、执行与版本。</p></div>
