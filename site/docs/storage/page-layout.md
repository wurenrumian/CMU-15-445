# Page & Tuple Layout

数据库不会把 C++ 对象直接写进磁盘。它把记录编码成字节，放进固定大小的 Page；Page 再由 `DiskManager` 负责读写。

## Slotted Page

```text
┌──────────────┬──────────────────────┬─────────────────────────┐
│ Page Header  │ Slot 0 │ Slot 1 ...  │       Tuple Data        │
│ free_space   │ offset │ length      │  ← 从页尾向前生长       │
└──────────────┴──────────────────────┴─────────────────────────┘
```

Slot 保存偏移量和长度，所以一条记录移动后，RID 仍然可以通过 Slot 定位。删除时通常先标记元数据，而不是立刻搬动所有记录；这正是 P2 Tombstone 和 P3 `is_deleted_` 的物理基础。

## RID 如何工作

```cpp
RID rid(page_id, slot_num);
tuple = table_heap.GetTuple(rid);
```

RID 不包含内存地址，只包含“哪一页、哪一个槽位”。页可以被淘汰、重新加载，RID 仍然有效。代价是每次访问都要经过 Buffer Pool。

## 阅读源码时找什么

- `src/storage/page/`：页的生命周期与守卫
- `src/storage/table/`：Tuple、TableHeap、TableIterator
- `src/include/storage/rid.h`：RID 的稳定定位语义

关键直觉：**对象会消失，页 ID 才是持久身份。**
