//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// buffer_pool_manager.h
//
// Identification: src/include/buffer/buffer_pool_manager.h
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#pragma once

#include <list>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "buffer/arc_replacer.h"
#include "common/config.h"
#include "recovery/log_manager.h"
#include "storage/disk/disk_scheduler.h"
#include "storage/page/page.h"
#include "storage/page/page_guard.h"

namespace bustub {

class BufferPoolManager;
class ReadPageGuard;
class WritePageGuard;

/**
 * @brief A helper class for `BufferPoolManager` that manages a frame of memory and related metadata.
 *
 * This class represents headers for frames of memory that the `BufferPoolManager` stores pages of data into. Note that
 * the actual frames of memory are not stored directly inside a `FrameHeader`, rather the `FrameHeader`s store pointer
 * to the frames and are stored separately them.
 *
 * ---
 *
 * Something that may (or may not) be of interest to you is why the field `data_` is stored as a vector that is
 * allocated on the fly instead of as a direct pointer to some pre-allocated chunk of memory.
 *
 * In a traditional production buffer pool manager, all memory that the buffer pool is intended to manage is allocated
 * in one large contiguous array (think of a very large `malloc` call that allocates several gigabytes of memory up
 * front). This large contiguous block of memory is then divided into contiguous frames. In other words, frames are
 * defined by an offset from the base of the array in page-sized (4 KB) intervals.
 *
 * In BusTub, we instead allocate each frame on its own (via a `std::vector<char>`) in order to easily detect buffer
 * overflow with address sanitizer. Since C++ has no notion of memory safety, it would be very easy to cast a page's
 * data pointer into some large data type and start overwriting other pages of data if they were all contiguous.
 *
 * If you would like to attempt to use more efficient data structures for your buffer pool manager, you are free to do
 * so. However, you will likely benefit significantly from detecting buffer overflow in future projects (especially
 * project 2).
 */
class FrameHeader {
  friend class BufferPoolManager;
  friend class ReadPageGuard;
  friend class WritePageGuard;

 public:
  explicit FrameHeader(frame_id_t frame_id);

 private:
  auto GetData() const -> const char *;
  auto GetDataMut() -> char *;
  void Reset();

  /** @brief The frame ID / index of the frame this header represents. */
  const frame_id_t frame_id_;

  /** @brief The readers / writer latch for this frame. */
  std::shared_mutex rwlatch_;

  /** @brief The number of pins on this frame keeping the page in memory. */
  std::atomic<size_t> pin_count_;

  /** @brief The dirty flag. */
  bool is_dirty_;

  /**
   * @brief A pointer to the data of the page that this frame holds.
   *
   * If the frame does not hold any page data, the frame contains all null bytes.
   */
  std::vector<char> data_;

  /**
   * TODO(P1): You may add any fields or helper functions under here that you think are necessary.
   *
   * ==== P1 STEP 7: 让帧自己记住它装的是哪个页 ====
   * 淘汰一个帧时我们需要知道两件事：往磁盘的哪个 page_id 回写脏数据，
   * 以及该从 page_table_ 里删掉哪个键。若不在帧里存 page_id，就只能反向
   * 遍历整个 page_table_ 去找 frame_id —— 那是 O(帧数) 的线性扫描，
   * 而且每次缺页都要做一次。
   *
   * 用 std::optional 而不是「INVALID_PAGE_ID 哨兵值」，是为了让「这个帧是空的」
   * 这一状态在类型层面就无法被误当成一个合法页号。
   */
  std::optional<page_id_t> page_id_{std::nullopt};
};

/**
 * @brief The declaration of the `BufferPoolManager` class.
 *
 * As stated in the writeup, the buffer pool is responsible for moving physical pages of data back and forth from
 * buffers in main memory to persistent storage. It also behaves as a cache, keeping frequently used pages in memory for
 * faster access, and evicting unused or cold pages back out to storage.
 *
 * Make sure you read the writeup in its entirety before attempting to implement the buffer pool manager. You also need
 * to have completed the implementation of both the `ArcReplacer` and `DiskManager` classes.
 */
class BufferPoolManager {
 public:
  BufferPoolManager(size_t num_frames, DiskManager *disk_manager, LogManager *log_manager = nullptr);
  ~BufferPoolManager();

  auto Size() const -> size_t;
  auto NewPage() -> page_id_t;
  auto DeletePage(page_id_t page_id) -> bool;
  auto CheckedWritePage(page_id_t page_id, AccessType access_type = AccessType::Unknown)
      -> std::optional<WritePageGuard>;
  auto CheckedReadPage(page_id_t page_id, AccessType access_type = AccessType::Unknown) -> std::optional<ReadPageGuard>;
  auto WritePage(page_id_t page_id, AccessType access_type = AccessType::Unknown) -> WritePageGuard;
  auto ReadPage(page_id_t page_id, AccessType access_type = AccessType::Unknown) -> ReadPageGuard;
  auto FlushPageUnsafe(page_id_t page_id) -> bool;
  auto FlushPage(page_id_t page_id) -> bool;
  void FlushAllPagesUnsafe();
  void FlushAllPages();
  auto GetPinCount(page_id_t page_id) -> std::optional<size_t>;

 private:
  /** @brief The number of frames in the buffer pool. */
  const size_t num_frames_;

  /** @brief The next page ID to be allocated.  */
  std::atomic<page_id_t> next_page_id_;

  /**
   * @brief The latch protecting the buffer pool's inner data structures.
   *
   * 保护的是**元数据**，不是页数据。具体覆盖：
   *   - `page_table_`（page_id → frame_id 的映射）
   *   - `free_frames_`（空闲帧链表）
   *   - `next_page_id_` 之外的所有分配状态
   *   - `replacer_` 的全部内部状态
   *   - 每个 `FrameHeader` 的 `page_id_` / `pin_count_` / `is_dirty_`
   *
   * **不**保护 `FrameHeader::data_` —— 那是每帧自己的 `rwlatch_` 的职责。
   * 这就是"两层粒度"的分工：元数据一把全局锁（临界区极短），页数据一帧一把锁
   * （临界区可以很长，甚至包含磁盘 I/O）。
   *
   * 铁律：**绝不能持有 bpm_latch_ 去申请任何一把 rwlatch_**。
   * 页锁可能被另一个线程长时间持有，而那个线程接下来可能要拿 bpm_latch_ ——
   * 立刻死锁。`DeadlockTest` 专门压这一点。正确顺序永远是
   * 「bpm 锁内 pin 住 → 放掉 bpm 锁 → 再取页锁」。
   */
  std::shared_ptr<std::mutex> bpm_latch_;

  /** @brief The frame headers of the frames that this buffer pool manages. */
  std::vector<std::shared_ptr<FrameHeader>> frames_;

  /** @brief The page table that keeps track of the mapping between pages and buffer pool frames. */
  std::unordered_map<page_id_t, frame_id_t> page_table_;

  /** @brief A list of free frames that do not hold any page's data. */
  std::list<frame_id_t> free_frames_;

  /** @brief The replacer to find unpinned / candidate pages for eviction. */
  std::shared_ptr<ArcReplacer> replacer_;

  /** @brief A pointer to the disk scheduler. Shared with the page guards for flushing. */
  std::shared_ptr<DiskScheduler> disk_scheduler_;

  /**
   * @brief A pointer to the log manager.
   *
   * Note: Please ignore this for P1.
   */
  LogManager *log_manager_ __attribute__((__unused__));

  /**
   * TODO(P1): You may add additional private members and helper functions if you find them necessary.
   *
   * ==== P1 STEP 8: 三个私有辅助函数 ====
   */

  /**
   * @brief 找一个「空的、可以拿来装新页」的帧。
   *
   * 优先从 free_frames_ 取；取不到就让 replacer_ 挑一个受害者淘汰：
   * 脏页回写磁盘 → 从 page_table_ 摘除 → 清零帧内容。
   *
   * @return 可用的 frame id；若所有帧都被 pin 住（缓冲池耗尽）则返回 std::nullopt。
   * @note 调用方必须已持有 *bpm_latch_。
   */
  auto AllocateFrame() -> std::optional<frame_id_t>;

  /**
   * @brief 把 page_id 对应的页装入内存并 pin 住，返回承载它的帧。
   *
   * 这是 CheckedReadPage / CheckedWritePage 共用的全部簿记逻辑。两者的唯一区别
   * 只是最后包装成读锁还是写锁的 guard，所以这里把公共部分抽出来。
   *
   * @return 帧指针；缓冲池耗尽时返回 nullptr。
   * @note 本函数自己取 *bpm_latch_，返回时已释放——绝不能在持有 bpm 锁的状态下
   *       再去抢页面的读写锁，否则就是 DeadlockTest 要抓的那种死锁。
   */
  auto FetchFrame(page_id_t page_id, AccessType access_type) -> std::shared_ptr<FrameHeader>;

  /**
   * @brief 向磁盘调度器提交一次同步 I/O，并阻塞等待其完成。
   *
   * @param is_write true 表示写出，false 表示读入。
   * @param page_id  目标页号。
   * @param data     内存缓冲区（长度必须是 BUSTUB_PAGE_SIZE）。
   */
  void SchedulePageIo(bool is_write, page_id_t page_id, char *data);
};
}  // namespace bustub
