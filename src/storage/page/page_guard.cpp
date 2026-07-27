//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// page_guard.cpp
//
// Identification: src/storage/page/page_guard.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "storage/page/page_guard.h"
#include <memory>
#include <mutex>  // NOLINT
#include <utility>
#include <vector>
#include "buffer/arc_replacer.h"
#include "common/macros.h"

namespace bustub {

/**
 * @brief The only constructor for an RAII `ReadPageGuard` that creates a valid guard.
 *
 * Note that only the buffer pool manager is allowed to call this constructor.
 *
 * TODO(P1): Add implementation.
 *
 * @param page_id The page ID of the page we want to read.
 * @param frame A shared pointer to the frame that holds the page we want to protect.
 * @param replacer A shared pointer to the buffer pool manager's replacer.
 * @param bpm_latch A shared pointer to the buffer pool manager's latch.
 * @param disk_scheduler A shared pointer to the buffer pool manager's disk scheduler.
 */
ReadPageGuard::ReadPageGuard(page_id_t page_id, std::shared_ptr<FrameHeader> frame,
                             std::shared_ptr<ArcReplacer> replacer, std::shared_ptr<std::mutex> bpm_latch,
                             std::shared_ptr<DiskScheduler> disk_scheduler)
    : page_id_(page_id),
      frame_(std::move(frame)),
      replacer_(std::move(replacer)),
      bpm_latch_(std::move(bpm_latch)),
      disk_scheduler_(std::move(disk_scheduler)) {
  // ==== P1 STEP 12: guard 构造 = 获取页锁 ====
  // 到这一步为止，BufferPoolManager::FetchFrame 已经做完了所有簿记
  // （pin_count_ +1、SetEvictable(false)）并且释放了 bpm_latch_。
  // 这里只剩下一件事：拿到这一页的共享读锁。
  //
  // 共享锁意味着多个 ReadPageGuard 可以同时存在于同一页上，但只要有任何一个
  // ReadPageGuard 活着，就不可能有 WritePageGuard 在改这页数据。
  frame_->rwlatch_.lock_shared();
  is_valid_ = true;
}

/**
 * @brief The move constructor for `ReadPageGuard`.
 *
 * ### Implementation
 *
 * If you are unfamiliar with move semantics, please familiarize yourself with learning materials online. There are many
 * great resources (including articles, Microsoft tutorials, YouTube videos) that explain this in depth.
 *
 * Make sure you invalidate the other guard; otherwise, you might run into double free problems! For both objects, you
 * need to update _at least_ 5 fields each.
 *
 * TODO(P1): Add implementation.
 *
 * @param that The other page guard.
 */
ReadPageGuard::ReadPageGuard(ReadPageGuard &&that) noexcept
    : page_id_(that.page_id_),
      frame_(std::move(that.frame_)),
      replacer_(std::move(that.replacer_)),
      bpm_latch_(std::move(that.bpm_latch_)),
      disk_scheduler_(std::move(that.disk_scheduler_)),
      is_valid_(that.is_valid_) {
  // ==== P1 STEP 13: 移动 = 所有权转移，不做任何加解锁 ====
  // 关键认识：那把页锁已经被 `that` 锁上了，现在只是换一个对象来负责解锁。
  // 因此这里既不该再 lock（会死锁），也不该 unlock（锁还要继续持有）。
  // pin_count_ 同理保持不变——页的使用者数量没有变化，只是"谁拿着凭证"变了。
  //
  // 必须把 that 置为 invalid：否则 that 析构时会调 Drop()，
  // 于是同一把锁被解两次、pin_count_ 被减两次，直接崩溃或计数错乱。
  that.is_valid_ = false;
}

/**
 * @brief The move assignment operator for `ReadPageGuard`.
 *
 * ### Implementation
 *
 * If you are unfamiliar with move semantics, please familiarize yourself with learning materials online. There are many
 * great resources (including articles, Microsoft tutorials, YouTube videos) that explain this in depth.
 *
 * Make sure you invalidate the other guard; otherwise, you might run into double free problems! For both objects, you
 * need to update _at least_ 5 fields each, and for the current object, make sure you release any resources it might be
 * holding on to.
 *
 * TODO(P1): Add implementation.
 *
 * @param that The other page guard.
 * @return ReadPageGuard& The newly valid `ReadPageGuard`.
 */
auto ReadPageGuard::operator=(ReadPageGuard &&that) noexcept -> ReadPageGuard & {
  // 自我移动赋值（`guard = std::move(guard)`）必须是无操作。
  // 少了这个判断的话，下面的 Drop() 会先把自己释放掉，
  // 接着再从「已经被清空的自己」拷贝字段，pin_count_ 就凭空少了 1。
  // 测试里 `guard0 = std::move(guard0_r);` 专门验证这一点。
  if (this == &that) {
    return *this;
  }

  // 本对象可能已经守着另一个页，先把它彻底释放（解锁 + 减 pin），再接管新的。
  Drop();

  page_id_ = that.page_id_;
  frame_ = std::move(that.frame_);
  replacer_ = std::move(that.replacer_);
  bpm_latch_ = std::move(that.bpm_latch_);
  disk_scheduler_ = std::move(that.disk_scheduler_);
  is_valid_ = that.is_valid_;
  that.is_valid_ = false;

  return *this;
}

/**
 * @brief Gets the page ID of the page this guard is protecting.
 */
auto ReadPageGuard::GetPageId() const -> page_id_t {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid read guard");
  return page_id_;
}

/**
 * @brief Gets a `const` pointer to the page of data this guard is protecting.
 */
auto ReadPageGuard::GetData() const -> const char * {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid read guard");
  return frame_->GetData();
}

/**
 * @brief Returns whether the page is dirty (modified but not flushed to the disk).
 */
auto ReadPageGuard::IsDirty() const -> bool {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid read guard");
  return frame_->is_dirty_;
}

/**
 * @brief Flushes this page's data safely to disk.
 *
 * TODO(P1): Add implementation.
 */
void ReadPageGuard::Flush() {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid read guard");

  // 这里之所以「安全」，是因为我们此刻正持有该页的共享锁：
  // 不可能有写者在改这 4KB，写到磁盘的一定是一个自洽的快照。
  auto promise = disk_scheduler_->CreatePromise();
  auto future = promise.get_future();
  std::vector<DiskRequest> requests;
  requests.emplace_back(DiskRequest{/*is_write=*/true, frame_->GetDataMut(), page_id_, std::move(promise)});
  disk_scheduler_->Schedule(requests);
  future.get();

  frame_->is_dirty_ = false;
}

/**
 * @brief Manually drops a valid `ReadPageGuard`'s data. If this guard is invalid, this function does nothing.
 *
 * ### Implementation
 *
 * Make sure you don't double free! Also, think **very** **VERY** carefully about what resources you own and the order
 * in which you release those resources. If you get the ordering wrong, you will very likely fail one of the later
 * Gradescope tests. You may also want to take the buffer pool manager's latch in a very specific scenario...
 *
 * TODO(P1): Add implementation.
 */
void ReadPageGuard::Drop() {
  // 幂等：对无效 guard（默认构造的、已被移走的、已经 Drop 过的）什么都不做。
  // 测试会连续调用两次 Drop()，第二次必须没有副作用。
  if (!is_valid_) {
    return;
  }
  // 先置无效，保证后面即便发生异常也不会被重复释放。
  is_valid_ = false;

  // ==== P1 STEP 14: 释放顺序 ====
  // 第 1 步：放掉页锁。放在最前面，是因为此刻 pin_count_ 还大于 0，
  //          替换器不会选中这一帧，别人不可能把它淘汰掉并改写内容。
  //          反过来若先把 pin 降到 0 再解锁，中间那个窗口里别的线程可以淘汰
  //          这一帧、装入新页，而我们还锁着它 —— 逻辑上就乱了。
  frame_->rwlatch_.unlock_shared();

  // 第 2 步：在 bpm 锁的保护下更新使用计数。
  //          这两步必须原子：否则我们把 pin 减到 0 之后、SetEvictable(true) 之前，
  //          另一个线程可能刚好 FetchFrame 同一页（pin 0→1，SetEvictable(false)），
  //          随后我们的 SetEvictable(true) 会把一个正在被使用的帧标成可驱逐。
  {
    std::scoped_lock lock(*bpm_latch_);
    if (frame_->pin_count_.fetch_sub(1) == 1) {
      // fetch_sub 返回减之前的值；等于 1 表示我们是最后一个使用者，
      // 这一帧现在可以被替换器回收了。
      replacer_->SetEvictable(frame_->frame_id_, true);
    }
  }

  // 第 3 步：断开对缓冲池各组件的 shared_ptr 引用。
  //          注意必须在 scoped_lock 作用域之外重置 bpm_latch_ ——
  //          否则会先销毁自己正持有的那把锁的最后一个引用。
  frame_.reset();
  replacer_.reset();
  disk_scheduler_.reset();
  bpm_latch_.reset();
}

/** @brief The destructor for `ReadPageGuard`. This destructor simply calls `Drop()`. */
ReadPageGuard::~ReadPageGuard() { Drop(); }

/**********************************************************************************************************************/
/**********************************************************************************************************************/
/**********************************************************************************************************************/

/**
 * @brief The only constructor for an RAII `WritePageGuard` that creates a valid guard.
 *
 * Note that only the buffer pool manager is allowed to call this constructor.
 *
 * TODO(P1): Add implementation.
 *
 * @param page_id The page ID of the page we want to write to.
 * @param frame A shared pointer to the frame that holds the page we want to protect.
 * @param replacer A shared pointer to the buffer pool manager's replacer.
 * @param bpm_latch A shared pointer to the buffer pool manager's latch.
 * @param disk_scheduler A shared pointer to the buffer pool manager's disk scheduler.
 */
WritePageGuard::WritePageGuard(page_id_t page_id, std::shared_ptr<FrameHeader> frame,
                               std::shared_ptr<ArcReplacer> replacer, std::shared_ptr<std::mutex> bpm_latch,
                               std::shared_ptr<DiskScheduler> disk_scheduler)
    : page_id_(page_id),
      frame_(std::move(frame)),
      replacer_(std::move(replacer)),
      bpm_latch_(std::move(bpm_latch)),
      disk_scheduler_(std::move(disk_scheduler)) {
  // 独占锁：同一时刻这一页上只能存在一个 WritePageGuard，且不能有任何 ReadPageGuard。
  frame_->rwlatch_.lock();
  is_valid_ = true;

  // ==== P1 STEP 15: 何时置脏位 ====
  // 采取保守策略：只要有人拿到了写 guard，就认为这一页会被改，直接置脏。
  //
  // 为什么不等到调用 GetDataMut() 时才置脏？因为 GetDataMut() 只是交出一个裸指针，
  // 之后的写入发生在 guard 之外，我们根本观察不到。要么在这里保守置脏，
  // 要么把 4KB 数据做校验和比对——后者的代价远大于偶尔多写一次磁盘。
  //
  // 代价：只读却用了 WritePageGuard 的场景会产生多余的回写 I/O。
  // 收益：绝不会漏掉真实修改导致数据丢失。正确性优先于性能。
  //
  // 这行赋值在持有独占锁之后执行，因此与其它写者之间没有竞争。
  frame_->is_dirty_ = true;
}

/**
 * @brief The move constructor for `WritePageGuard`.
 *
 * ### Implementation
 *
 * If you are unfamiliar with move semantics, please familiarize yourself with learning materials online. There are many
 * great resources (including articles, Microsoft tutorials, YouTube videos) that explain this in depth.
 *
 * Make sure you invalidate the other guard; otherwise, you might run into double free problems! For both objects, you
 * need to update _at least_ 5 fields each.
 *
 * TODO(P1): Add implementation.
 *
 * @param that The other page guard.
 */
WritePageGuard::WritePageGuard(WritePageGuard &&that) noexcept
    : page_id_(that.page_id_),
      frame_(std::move(that.frame_)),
      replacer_(std::move(that.replacer_)),
      bpm_latch_(std::move(that.bpm_latch_)),
      disk_scheduler_(std::move(that.disk_scheduler_)),
      is_valid_(that.is_valid_) {
  // 与 ReadPageGuard 完全同理：只转移所有权，不碰锁，并让源对象失效。
  that.is_valid_ = false;
}

/**
 * @brief The move assignment operator for `WritePageGuard`.
 *
 * ### Implementation
 *
 * If you are unfamiliar with move semantics, please familiarize yourself with learning materials online. There are many
 * great resources (including articles, Microsoft tutorials, YouTube videos) that explain this in depth.
 *
 * Make sure you invalidate the other guard; otherwise, you might run into double free problems! For both objects, you
 * need to update _at least_ 5 fields each, and for the current object, make sure you release any resources it might be
 * holding on to.
 *
 * TODO(P1): Add implementation.
 *
 * @param that The other page guard.
 * @return WritePageGuard& The newly valid `WritePageGuard`.
 */
auto WritePageGuard::operator=(WritePageGuard &&that) noexcept -> WritePageGuard & {
  if (this == &that) {
    return *this;  // 自我移动赋值必须无副作用。
  }

  Drop();  // 先释放本对象当前守着的页。

  page_id_ = that.page_id_;
  frame_ = std::move(that.frame_);
  replacer_ = std::move(that.replacer_);
  bpm_latch_ = std::move(that.bpm_latch_);
  disk_scheduler_ = std::move(that.disk_scheduler_);
  is_valid_ = that.is_valid_;
  that.is_valid_ = false;

  return *this;
}

/**
 * @brief Gets the page ID of the page this guard is protecting.
 */
auto WritePageGuard::GetPageId() const -> page_id_t {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid write guard");
  return page_id_;
}

/**
 * @brief Gets a `const` pointer to the page of data this guard is protecting.
 */
auto WritePageGuard::GetData() const -> const char * {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid write guard");
  return frame_->GetData();
}

/**
 * @brief Gets a mutable pointer to the page of data this guard is protecting.
 */
auto WritePageGuard::GetDataMut() -> char * {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid write guard");
  // 交出可写指针 == 调用方即将修改这一页，置脏。
  // 构造函数里也置了一次，这里看似冗余，实则不可省：Flush() 会把脏位清掉，
  // 之后的修改只能靠这一行重新标记，否则淘汰时会被当成干净页直接丢弃。
  frame_->is_dirty_ = true;
  return frame_->GetDataMut();
}

/**
 * @brief Returns whether the page is dirty (modified but not flushed to the disk).
 */
auto WritePageGuard::IsDirty() const -> bool {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid write guard");
  return frame_->is_dirty_;
}

/**
 * @brief Flushes this page's data safely to disk.
 *
 * TODO(P1): Add implementation.
 */
void WritePageGuard::Flush() {
  BUSTUB_ENSURE(is_valid_, "tried to use an invalid write guard");

  // 我们持有独占锁，所以写出去的一定是完整、一致的一页。
  auto promise = disk_scheduler_->CreatePromise();
  auto future = promise.get_future();
  std::vector<DiskRequest> requests;
  requests.emplace_back(DiskRequest{/*is_write=*/true, frame_->GetDataMut(), page_id_, std::move(promise)});
  disk_scheduler_->Schedule(requests);
  future.get();

  // 刚落盘，内存与磁盘一致，清掉脏位可以让后续淘汰省掉一次写。
  //
  // 这里能安全清零，靠的是 GetDataMut() 也会置脏（见其实现）：
  // guard 还活着，调用方 Flush 之后若继续修改，必然要再调一次 GetDataMut()/AsMut()
  // 拿可写指针，脏位随即被重新置上。少了那一处，这里的清零就会吞掉后续修改。
  frame_->is_dirty_ = false;
}

/**
 * @brief Manually drops a valid `WritePageGuard`'s data. If this guard is invalid, this function does nothing.
 *
 * ### Implementation
 *
 * Make sure you don't double free! Also, think **very** **VERY** carefully about what resources you own and the order
 * in which you release those resources. If you get the ordering wrong, you will very likely fail one of the later
 * Gradescope tests. You may also want to take the buffer pool manager's latch in a very specific scenario...
 *
 * TODO(P1): Add implementation.
 */
void WritePageGuard::Drop() {
  if (!is_valid_) {
    return;  // 幂等。
  }
  is_valid_ = false;

  // 顺序与 ReadPageGuard::Drop 完全一致，只是解的是独占锁。
  // 详细的理由见 ReadPageGuard::Drop 里的分步说明。
  frame_->rwlatch_.unlock();

  {
    std::scoped_lock lock(*bpm_latch_);
    if (frame_->pin_count_.fetch_sub(1) == 1) {
      replacer_->SetEvictable(frame_->frame_id_, true);
    }
  }

  frame_.reset();
  replacer_.reset();
  disk_scheduler_.reset();
  bpm_latch_.reset();
}

/** @brief The destructor for `WritePageGuard`. This destructor simply calls `Drop()`. */
WritePageGuard::~WritePageGuard() { Drop(); }

}  // namespace bustub
