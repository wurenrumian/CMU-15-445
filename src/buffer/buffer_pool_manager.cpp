//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// buffer_pool_manager.cpp
//
// Identification: src/buffer/buffer_pool_manager.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "buffer/buffer_pool_manager.h"
#include <algorithm>
#include <memory>
#include <mutex>  // NOLINT
#include <optional>
#include <shared_mutex>  // NOLINT
#include <utility>
#include <vector>
#include "buffer/arc_replacer.h"
#include "common/config.h"
#include "common/macros.h"

namespace bustub {

/*
 * ======================= 缓冲池管理器的加锁约定 =======================
 *
 * 系统里有两层完全不同的锁，务必分清：
 *
 *   1. bpm_latch_（一把 std::mutex）保护缓冲池的「元数据」：
 *        page_table_、free_frames_、replacer_ 内部状态、frame->pin_count_、
 *        frame->page_id_。
 *      它只在极短的临界区内持有。
 *
 *   2. frame->rwlatch_（每帧一把 shared_mutex）保护该帧里那 4KB「页数据」：
 *        ReadPageGuard 持共享锁，WritePageGuard 持独占锁。
 *      它可以被持有任意长的时间——上层可能拿着一个 WritePageGuard 干很久的活。
 *
 * 【最关键的规则】绝不能持有 bpm_latch_ 去申请 frame->rwlatch_。
 * 否则一个线程长期持有某页的写锁时，另一个线程会拿着 bpm_latch_ 阻塞在该页锁上，
 * 于是整个缓冲池对所有其它页也全部冻结——这正是 DeadlockTest 检测的场景。
 * 所以 FetchFrame() 在返回前就释放 bpm_latch_，页锁留到 PageGuard 的构造函数里再取。
 *
 * 那为什么释放 bpm_latch_ 之后帧不会被别人淘汰掉？因为 FetchFrame() 在锁内
 * 已经把 pin_count_ 加一并调用了 SetEvictable(fid, false)，替换器不会再选中它。
 * 「先 pin 住，再放锁」是这里的安全基础。
 * ====================================================================
 */

/**
 * @brief The constructor for a `FrameHeader` that initializes all fields to default values.
 *
 * See the documentation for `FrameHeader` in "buffer/buffer_pool_manager.h" for more information.
 *
 * @param frame_id The frame ID / index of the frame we are creating a header for.
 */
FrameHeader::FrameHeader(frame_id_t frame_id) : frame_id_(frame_id), data_(BUSTUB_PAGE_SIZE, 0) { Reset(); }

/**
 * @brief Get a raw const pointer to the frame's data.
 *
 * @return const char* A pointer to immutable data that the frame stores.
 */
auto FrameHeader::GetData() const -> const char * { return data_.data(); }

/**
 * @brief Get a raw mutable pointer to the frame's data.
 *
 * @return char* A pointer to mutable data that the frame stores.
 */
auto FrameHeader::GetDataMut() -> char * { return data_.data(); }

/**
 * @brief Resets a `FrameHeader`'s member fields.
 */
void FrameHeader::Reset() {
  std::fill(data_.begin(), data_.end(), 0);
  pin_count_.store(0);
  is_dirty_ = false;
}

/**
 * @brief Creates a new `BufferPoolManager` instance and initializes all fields.
 *
 * See the documentation for `BufferPoolManager` in "buffer/buffer_pool_manager.h" for more information.
 *
 * ### Implementation
 *
 * We have implemented the constructor for you in a way that makes sense with our reference solution. You are free to
 * change anything you would like here if it doesn't fit with you implementation.
 *
 * Be warned, though! If you stray too far away from our guidance, it will be much harder for us to help you. Our
 * recommendation would be to first implement the buffer pool manager using the stepping stones we have provided.
 *
 * Once you have a fully working solution (all Gradescope test cases pass), then you can try more interesting things!
 *
 * @param num_frames The size of the buffer pool.
 * @param disk_manager The disk manager.
 * @param log_manager The log manager. Please ignore this for P1.
 */
BufferPoolManager::BufferPoolManager(size_t num_frames, DiskManager *disk_manager, LogManager *log_manager)
    : num_frames_(num_frames),
      next_page_id_(0),
      bpm_latch_(std::make_shared<std::mutex>()),
      replacer_(std::make_shared<ArcReplacer>(num_frames)),
      disk_scheduler_(std::make_shared<DiskScheduler>(disk_manager)),
      log_manager_(log_manager) {
  // Not strictly necessary...
  std::scoped_lock latch(*bpm_latch_);

  // Initialize the monotonically increasing counter at 0.
  next_page_id_.store(0);

  // Allocate all of the in-memory frames up front.
  frames_.reserve(num_frames_);

  // The page table should have exactly `num_frames_` slots, corresponding to exactly `num_frames_` frames.
  page_table_.reserve(num_frames_);

  // Initialize all of the frame headers, and fill the free frame list with all possible frame IDs (since all frames are
  // initially free).
  for (size_t i = 0; i < num_frames_; i++) {
    frames_.push_back(std::make_shared<FrameHeader>(i));
    free_frames_.push_back(static_cast<int>(i));
  }
}

/**
 * @brief Destroys the `BufferPoolManager`, freeing up all memory that the buffer pool was using.
 */
BufferPoolManager::~BufferPoolManager() = default;

/**
 * @brief Returns the number of frames that this buffer pool manages.
 */
auto BufferPoolManager::Size() const -> size_t { return num_frames_; }

/**
 * @brief Allocates a new page on disk.
 *
 * ### Implementation
 *
 * You will maintain a thread-safe, monotonically increasing counter in the form of a `std::atomic<page_id_t>`.
 * See the documentation on [atomics](https://en.cppreference.com/w/cpp/atomic/atomic) for more information.
 *
 * TODO(P1): Add implementation.
 *
 * @return The page ID of the newly allocated page.
 */
auto BufferPoolManager::NewPage() -> page_id_t {
  // ==== P1 STEP 9: 分配页号 ====
  // fetch_add 是原子的「读取并自增」，多个线程同时调用也不会拿到相同页号，
  // 而且完全不需要加 bpm_latch_。
  //
  // 注意 NewPage 只分配一个逻辑页号，并不会立刻占用缓冲池的帧，也不写磁盘。
  // 真正的物理分配推迟到第一次 WritePage/ReadPage 时由 DiskManager 完成
  // （它在写入未知页号时会自动分配磁盘槽位）。
  return next_page_id_.fetch_add(1);
}

void BufferPoolManager::SchedulePageIo(bool is_write, page_id_t page_id, char *data) {
  auto promise = disk_scheduler_->CreatePromise();
  auto future = promise.get_future();

  std::vector<DiskRequest> requests;
  requests.emplace_back(DiskRequest{is_write, data, page_id, std::move(promise)});
  disk_scheduler_->Schedule(requests);

  // 阻塞直到后台线程真正完成这次读/写。
  // 缓冲池的语义是同步的：CheckedReadPage 返回时页数据必须已经在内存里了。
  future.get();
}

auto BufferPoolManager::AllocateFrame() -> std::optional<frame_id_t> {
  // ---- 情况 A：还有从未使用过（或刚被 DeletePage 释放）的空闲帧 ----
  if (!free_frames_.empty()) {
    auto frame_id = free_frames_.front();
    free_frames_.pop_front();
    return frame_id;
  }

  // ---- 情况 B：所有帧都装着页，必须淘汰一个 ----
  auto victim = replacer_->Evict();
  if (!victim.has_value()) {
    // 替换器里一个可驱逐的帧都没有 —— 所有帧都被 page guard pin 住了。
    // 这是「缓冲池耗尽」，只能诚实地失败，不能阻塞（否则可能永远等不到）。
    return std::nullopt;
  }

  auto frame = frames_[*victim];
  if (frame->page_id_.has_value()) {
    // 脏页必须先落盘，否则修改就丢了。干净页可以直接丢弃——磁盘上那份仍然有效，
    // 这也是为什么维护 is_dirty_ 值得：它能省掉大量无谓的写 I/O。
    if (frame->is_dirty_) {
      SchedulePageIo(/*is_write=*/true, *frame->page_id_, frame->GetDataMut());
    }
    // 页不再驻留内存，从页表里摘掉，后续访问才会重新触发一次磁盘读。
    page_table_.erase(*frame->page_id_);
  }

  // 清空帧：pin_count_ 归零、is_dirty_ 归 false、4KB 数据清零。
  // 清零很重要——如果新页在磁盘上不存在，读操作不会填满缓冲区，
  // 残留的旧页数据就会被当成新页的内容读出去。
  frame->Reset();
  frame->page_id_.reset();
  return victim;
}

auto BufferPoolManager::FetchFrame(page_id_t page_id, AccessType access_type) -> std::shared_ptr<FrameHeader> {
  std::scoped_lock lock(*bpm_latch_);

  frame_id_t frame_id;
  if (auto it = page_table_.find(page_id); it != page_table_.end()) {
    // ---- 情况 1：缓存命中，页已经在内存里，零 I/O ----
    frame_id = it->second;
  } else {
    // ---- 情况 2：缺页，需要一个帧并从磁盘读入 ----
    auto allocated = AllocateFrame();
    if (!allocated.has_value()) {
      return nullptr;  // 缓冲池耗尽。
    }
    frame_id = *allocated;
    auto frame = frames_[frame_id];

    // 这次磁盘读是在持有 bpm_latch_ 的情况下做的，会短暂阻塞其它线程的元数据操作。
    // 之所以可以接受：若在读之前就放锁，另一个线程会在页表里看到这个 page_id，
    // 以为数据已就绪，从而读到半截数据。要既不阻塞又正确，得引入「加载中」状态
    // 并让其他线程在其上等待——那是 P1 之外的优化了。
    SchedulePageIo(/*is_write=*/false, page_id, frame->GetDataMut());

    frame->page_id_ = page_id;
    page_table_[page_id] = frame_id;
  }

  auto frame = frames_[frame_id];

  // 三件事必须在同一个临界区里原子完成，否则会和并发的 Drop() 打架：
  //   1) pin 住这一帧，表示「有人正在用」；
  //   2) 告诉替换器记录这次访问（ARC 据此调整 mru_/mfu_ 与目标大小 p）；
  //   3) 把它标成不可驱逐。
  // 如果 2)、3) 顺序反了也不行：RecordAccess 在幽灵命中时会新建条目并默认
  // evictable_=false，若先 SetEvictable(true) 会被随后的 RecordAccess 覆盖掉，
  // 导致 curr_size_ 记错。
  frame->pin_count_.fetch_add(1);
  replacer_->RecordAccess(frame_id, page_id, access_type);
  replacer_->SetEvictable(frame_id, false);

  return frame;
}

/**
 * @brief Removes a page from the database, both on disk and in memory.
 *
 * If the page is pinned in the buffer pool, this function does nothing and returns `false`. Otherwise, this function
 * removes the page from both disk and memory (if it is still in the buffer pool), returning `true`.
 *
 * ### Implementation
 *
 * Think about all of the places that a page or a page's metadata could be, and use that to guide you on implementing
 * this function. You will probably want to implement this function _after_ you have implemented `CheckedReadPage` and
 * `CheckedWritePage`.
 *
 * You should call `DeallocatePage` in the disk scheduler to make the space available for new pages.
 *
 * TODO(P1): Add implementation.
 *
 * @param page_id The page ID of the page we want to delete.
 * @return `false` if the page exists but could not be deleted, `true` if the page didn't exist or deletion succeeded.
 */
auto BufferPoolManager::DeletePage(page_id_t page_id) -> bool {
  std::scoped_lock lock(*bpm_latch_);

  if (auto it = page_table_.find(page_id); it != page_table_.end()) {
    auto frame_id = it->second;
    auto frame = frames_[frame_id];

    // 还有 page guard 在用这个页，删不得。
    if (frame->pin_count_.load() > 0) {
      return false;
    }

    // ==== P1 STEP 10: 清理一个页要覆盖它所有可能的藏身之处 ====
    //   1) 替换器：用 Remove 而不是 Evict —— 页在磁盘上都要没了，
    //      不该在 ARC 的幽灵链表里留下「它曾在缓存中」的历史。
    //   2) 页表：删掉 page_id → frame_id 的映射。
    //   3) 帧本身：清零内容。注意这里绝不能回写脏数据，页马上就要被释放了。
    //   4) 空闲链表：把帧还回去供后续复用。
    replacer_->Remove(frame_id);
    page_table_.erase(it);
    frame->Reset();
    frame->page_id_.reset();
    free_frames_.push_back(frame_id);
  }

  // 页不在内存里也要走这一步：磁盘上的空间需要归还。
  disk_scheduler_->DeallocatePage(page_id);
  return true;
}

/**
 * @brief Acquires an optional write-locked guard over a page of data. The user can specify an `AccessType` if needed.
 *
 * If it is not possible to bring the page of data into memory, this function will return a `std::nullopt`.
 *
 * Page data can _only_ be accessed via page guards. Users of this `BufferPoolManager` are expected to acquire either a
 * `ReadPageGuard` or a `WritePageGuard` depending on the mode in which they would like to access the data, which
 * ensures that any access of data is thread-safe.
 *
 * There can only be 1 `WritePageGuard` reading/writing a page at a time. This allows data access to be both immutable
 * and mutable, meaning the thread that owns the `WritePageGuard` is allowed to manipulate the page's data however they
 * want. If a user wants to have multiple threads reading the page at the same time, they must acquire a `ReadPageGuard`
 * with `CheckedReadPage` instead.
 *
 * ### Implementation
 *
 * There are three main cases that you will have to implement. The first two are relatively simple: one is when there is
 * plenty of available memory, and the other is when we don't actually need to perform any additional I/O. Think about
 * what exactly these two cases entail.
 *
 * The third case is the trickiest, and it is when we do not have any _easily_ available memory at our disposal. The
 * buffer pool is tasked with finding memory that it can use to bring in a page of memory, using the replacement
 * algorithm you implemented previously to find candidate frames for eviction.
 *
 * Once the buffer pool has identified a frame for eviction, several I/O operations may be necessary to bring in the
 * page of data we want into the frame.
 *
 * There is likely going to be a lot of shared code with `CheckedReadPage`, so you may find creating helper functions
 * useful.
 *
 * These two functions are the crux of this project, so we won't give you more hints than this. Good luck!
 *
 * TODO(P1): Add implementation.
 *
 * @param page_id The ID of the page we want to write to.
 * @param access_type The type of page access.
 * @return std::optional<WritePageGuard> An optional latch guard where if there are no more free frames (out of memory)
 * returns `std::nullopt`; otherwise, returns a `WritePageGuard` ensuring exclusive and mutable access to a page's data.
 */
auto BufferPoolManager::CheckedWritePage(page_id_t page_id, AccessType access_type) -> std::optional<WritePageGuard> {
  // 全部簿记都在 FetchFrame 里完成，它返回时 bpm_latch_ 已经释放。
  auto frame = FetchFrame(page_id, access_type);
  if (frame == nullptr) {
    return std::nullopt;  // 缓冲池耗尽（所有帧都被 pin 住）。
  }
  // WritePageGuard 的构造函数在这里才去抢该页的独占锁——此时我们没有持有 bpm_latch_，
  // 所以即使这一步要等很久，也只会阻塞想访问「这一页」的线程，不影响整个缓冲池。
  return WritePageGuard(page_id, frame, replacer_, bpm_latch_, disk_scheduler_);
}

/**
 * @brief Acquires an optional read-locked guard over a page of data. The user can specify an `AccessType` if needed.
 *
 * If it is not possible to bring the page of data into memory, this function will return a `std::nullopt`.
 *
 * Page data can _only_ be accessed via page guards. Users of this `BufferPoolManager` are expected to acquire either a
 * `ReadPageGuard` or a `WritePageGuard` depending on the mode in which they would like to access the data, which
 * ensures that any access of data is thread-safe.
 *
 * There can be any number of `ReadPageGuard`s reading the same page of data at a time across different threads.
 * However, all data access must be immutable. If a user wants to mutate the page's data, they must acquire a
 * `WritePageGuard` with `CheckedWritePage` instead.
 *
 * ### Implementation
 *
 * See the implementation details of `CheckedWritePage`.
 *
 * TODO(P1): Add implementation.
 *
 * @param page_id The ID of the page we want to read.
 * @param access_type The type of page access.
 * @return std::optional<ReadPageGuard> An optional latch guard where if there are no more free frames (out of memory)
 * returns `std::nullopt`; otherwise, returns a `ReadPageGuard` ensuring shared and read-only access to a page's data.
 */
auto BufferPoolManager::CheckedReadPage(page_id_t page_id, AccessType access_type) -> std::optional<ReadPageGuard> {
  // 与 CheckedWritePage 唯一的区别：最后包装成持共享锁的 ReadPageGuard，
  // 因而允许多个线程同时读同一页。
  auto frame = FetchFrame(page_id, access_type);
  if (frame == nullptr) {
    return std::nullopt;
  }
  return ReadPageGuard(page_id, frame, replacer_, bpm_latch_, disk_scheduler_);
}

/**
 * @brief A wrapper around `CheckedWritePage` that unwraps the inner value if it exists.
 *
 * If `CheckedWritePage` returns a `std::nullopt`, **this function aborts the entire process.**
 *
 * This function should **only** be used for testing and ergonomic's sake. If it is at all possible that the buffer pool
 * manager might run out of memory, then use `CheckedPageWrite` to allow you to handle that case.
 *
 * See the documentation for `CheckedPageWrite` for more information about implementation.
 *
 * @param page_id The ID of the page we want to read.
 * @param access_type The type of page access.
 * @return WritePageGuard A page guard ensuring exclusive and mutable access to a page's data.
 */
auto BufferPoolManager::WritePage(page_id_t page_id, AccessType access_type) -> WritePageGuard {
  auto guard_opt = CheckedWritePage(page_id, access_type);

  if (!guard_opt.has_value()) {
    fmt::println(stderr, "\n`CheckedWritePage` failed to bring in page {}\n", page_id);
    std::abort();
  }

  return std::move(guard_opt).value();
}

/**
 * @brief A wrapper around `CheckedReadPage` that unwraps the inner value if it exists.
 *
 * If `CheckedReadPage` returns a `std::nullopt`, **this function aborts the entire process.**
 *
 * This function should **only** be used for testing and ergonomic's sake. If it is at all possible that the buffer pool
 * manager might run out of memory, then use `CheckedPageWrite` to allow you to handle that case.
 *
 * See the documentation for `CheckedPageRead` for more information about implementation.
 *
 * @param page_id The ID of the page we want to read.
 * @param access_type The type of page access.
 * @return ReadPageGuard A page guard ensuring shared and read-only access to a page's data.
 */
auto BufferPoolManager::ReadPage(page_id_t page_id, AccessType access_type) -> ReadPageGuard {
  auto guard_opt = CheckedReadPage(page_id, access_type);

  if (!guard_opt.has_value()) {
    fmt::println(stderr, "\n`CheckedReadPage` failed to bring in page {}\n", page_id);
    std::abort();
  }

  return std::move(guard_opt).value();
}

/**
 * @brief Flushes a page's data out to disk unsafely.
 *
 * This function will write out a page's data to disk if it has been modified. If the given page is not in memory, this
 * function will return `false`.
 *
 * You should not take a lock on the page in this function.
 * This means that you should carefully consider when to toggle the `is_dirty_` bit.
 *
 * ### Implementation
 *
 * You should probably leave implementing this function until after you have completed `CheckedReadPage` and
 * `CheckedWritePage`, as it will likely be much easier to understand what to do.
 *
 * TODO(P1): Add implementation
 *
 * @param page_id The page ID of the page to be flushed.
 * @return `false` if the page could not be found in the page table; otherwise, `true`.
 */
auto BufferPoolManager::FlushPageUnsafe(page_id_t page_id) -> bool {
  std::scoped_lock lock(*bpm_latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    return false;  // 页不在内存里，没什么可刷的。
  }

  auto frame = frames_[it->second];
  // ==== P1 STEP 11: 「Unsafe」到底不安全在哪 ====
  // 这里不取该帧的读锁，所以另一个持有 WritePageGuard 的线程可能正在改这 4KB。
  // 结果就是可能把「改了一半」的页写到磁盘上。
  //
  // is_dirty_ 的清零时机同理很微妙：必须在提交写请求「之后」清，否则并发的写者
  // 在我们发起 I/O 前刚改完并置脏，会被我们紧接着的清零抹掉，那次修改就永远
  // 不会再被回写了。放在后面清，最坏也只是多写一次，不会丢数据。
  SchedulePageIo(/*is_write=*/true, page_id, frame->GetDataMut());
  frame->is_dirty_ = false;
  return true;
}

/**
 * @brief Flushes a page's data out to disk safely.
 *
 * This function will write out a page's data to disk if it has been modified. If the given page is not in memory, this
 * function will return `false`.
 *
 * You should take a lock on the page in this function to ensure that a consistent state is flushed to disk.
 *
 * ### Implementation
 *
 * You should probably leave implementing this function until after you have completed `CheckedReadPage`,
 * `CheckedWritePage`, and `Flush` in the page guards, as it will likely be much easier to understand what to do.
 *
 * TODO(P1): Add implementation
 *
 * @param page_id The page ID of the page to be flushed.
 * @return `false` if the page could not be found in the page table; otherwise, `true`.
 */
auto BufferPoolManager::FlushPage(page_id_t page_id) -> bool {
  std::shared_ptr<FrameHeader> frame;
  frame_id_t frame_id;

  // ---- 阶段 1：在 bpm 锁内定位并「借用」这一帧 ----
  {
    std::scoped_lock lock(*bpm_latch_);
    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
      return false;
    }
    frame_id = it->second;
    frame = frames_[frame_id];
    // 先 pin 住再放锁：否则放锁之后、拿页锁之前，这一帧可能被别人淘汰掉，
    // 我们就会把一个已经装了别的页的帧当成 page_id 写出去。
    frame->pin_count_.fetch_add(1);
    replacer_->SetEvictable(frame_id, false);
  }

  // ---- 阶段 2：不持 bpm 锁，安全地取页读锁再落盘 ----
  {
    std::shared_lock page_lock(frame->rwlatch_);
    SchedulePageIo(/*is_write=*/true, page_id, frame->GetDataMut());
    frame->is_dirty_ = false;
  }

  // ---- 阶段 3：归还借用 ----
  {
    std::scoped_lock lock(*bpm_latch_);
    if (frame->pin_count_.fetch_sub(1) == 1) {
      // fetch_sub 返回的是「减之前」的值，等于 1 说明我们是最后一个使用者。
      replacer_->SetEvictable(frame_id, true);
    }
  }
  return true;
}

/**
 * @brief Flushes all page data that is in memory to disk unsafely.
 *
 * You should not take locks on the pages in this function.
 * This means that you should carefully consider when to toggle the `is_dirty_` bit.
 *
 * ### Implementation
 *
 * You should probably leave implementing this function until after you have completed `CheckedReadPage`,
 * `CheckedWritePage`, and `FlushPage`, as it will likely be much easier to understand what to do.
 *
 * TODO(P1): Add implementation
 */
void BufferPoolManager::FlushAllPagesUnsafe() {
  std::scoped_lock lock(*bpm_latch_);
  for (const auto &[page_id, frame_id] : page_table_) {
    auto frame = frames_[frame_id];
    SchedulePageIo(/*is_write=*/true, page_id, frame->GetDataMut());
    frame->is_dirty_ = false;
  }
}

/**
 * @brief Flushes all page data that is in memory to disk safely.
 *
 * You should take locks on the pages in this function to ensure that a consistent state is flushed to disk.
 *
 * ### Implementation
 *
 * You should probably leave implementing this function until after you have completed `CheckedReadPage`,
 * `CheckedWritePage`, and `FlushPage`, as it will likely be much easier to understand what to do.
 *
 * TODO(P1): Add implementation
 */
void BufferPoolManager::FlushAllPages() {
  // 先在锁内拷一份页号快照，再逐个调 FlushPage。
  // 不能一边持有 bpm_latch_ 一边循环调用 FlushPage —— 后者自己还要取这把锁
  // （std::mutex 不可重入，会立刻自锁死）。
  std::vector<page_id_t> page_ids;
  {
    std::scoped_lock lock(*bpm_latch_);
    page_ids.reserve(page_table_.size());
    for (const auto &[page_id, frame_id] : page_table_) {
      page_ids.push_back(page_id);
    }
  }

  for (const auto page_id : page_ids) {
    // 快照拍完之后某个页可能已被淘汰，FlushPage 返回 false，忽略即可。
    FlushPage(page_id);
  }
}

/**
 * @brief Retrieves the pin count of a page. If the page does not exist in memory, return `std::nullopt`.
 *
 * This function is thread safe. Callers may invoke this function in a multi-threaded environment where multiple threads
 * access the same page.
 *
 * This function is intended for testing purposes. If this function is implemented incorrectly, it will definitely cause
 * problems with the test suite and autograder.
 *
 * # Implementation
 *
 * We will use this function to test if your buffer pool manager is managing pin counts correctly. Since the
 * `pin_count_` field in `FrameHeader` is an atomic type, you do not need to take the latch on the frame that holds the
 * page we want to look at. Instead, you can simply use an atomic `load` to safely load the value stored. You will still
 * need to take the buffer pool latch, however.
 *
 * Again, if you are unfamiliar with atomic types, see the official C++ docs
 * [here](https://en.cppreference.com/w/cpp/atomic/atomic).
 *
 * TODO(P1): Add implementation
 *
 * @param page_id The page ID of the page we want to get the pin count of.
 * @return std::optional<size_t> The pin count if the page exists; otherwise, `std::nullopt`.
 */
auto BufferPoolManager::GetPinCount(page_id_t page_id) -> std::optional<size_t> {
  std::scoped_lock lock(*bpm_latch_);

  auto it = page_table_.find(page_id);
  if (it == page_table_.end()) {
    // 返回 nullopt 而不是 0，是为了区分「页在内存里但没人用」和「页根本不在内存里」。
    // 测试正是靠这个区分来验证淘汰确实发生了。
    return std::nullopt;
  }
  return frames_[it->second]->pin_count_.load();
}

}  // namespace bustub
