//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// disk_scheduler.cpp
//
// Identification: src/storage/disk/disk_scheduler.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "storage/disk/disk_scheduler.h"
#include <utility>
#include <vector>
#include "common/macros.h"
#include "storage/disk/disk_manager.h"

namespace bustub {

DiskScheduler::DiskScheduler(DiskManager *disk_manager) : disk_manager_(disk_manager) {
  // ==== P1 STEP 6: 磁盘调度器 = 请求队列 + 一个后台工作线程 ====
  // 把「谁发起 I/O」和「谁执行 I/O」解耦：调用方只管把请求塞进队列然后拿 future，
  // 真正的读写全部由这一个后台线程串行执行。
  //
  // 这样做的好处：DiskManager 的读写并不是线程安全的（内部共享一个文件流的偏移量），
  // 由单线程独占访问就天然避免了竞争，调用方也不必自己加锁。
  // 同时调用方可以「先提交、后等待」，在等待期间干别的事情。
  //
  // 注意：原骨架里 UNIMPLEMENTED 写在 emplace 之前，直接删掉即可——线程必须在
  // 构造函数结束前启动，否则任何 Schedule 都会永远卡在 future 上。
  background_thread_.emplace([&] { StartWorkerThread(); });
}

DiskScheduler::~DiskScheduler() {
  // Put a `std::nullopt` in the queue to signal to exit the loop
  request_queue_.Put(std::nullopt);
  if (background_thread_.has_value()) {
    background_thread_->join();
  }
}

/**
 * @brief Schedules a request for the DiskManager to execute.
 *
 * @param requests The requests to be scheduled.
 */
void DiskScheduler::Schedule(std::vector<DiskRequest> &requests) {
  // DiskRequest 内含 std::promise，是只可移动 (move-only) 类型，
  // 所以这里必须 std::move 进队列，不能拷贝。
  for (auto &request : requests) {
    request_queue_.Put(std::move(request));
  }
}

/**
 * @brief Background worker thread function that processes scheduled requests.
 *
 * The background thread needs to process requests while the DiskScheduler exists, i.e., this function should not
 * return until ~DiskScheduler() is called. At that point you need to make sure that the function does return.
 */
void DiskScheduler::StartWorkerThread() {
  while (true) {
    // Channel::Get() 在队列为空时会阻塞在条件变量上，不会空转烧 CPU。
    auto request = request_queue_.Get();

    // 析构函数会 Put(std::nullopt) 作为「毒丸 (poison pill)」，
    // 收到它就退出循环，让 background_thread_->join() 得以返回。
    if (!request.has_value()) {
      return;
    }

    if (request->is_write_) {
      disk_manager_->WritePage(request->page_id_, request->data_);
    } else {
      disk_manager_->ReadPage(request->page_id_, request->data_);
    }

    // 设置 promise 的值，唤醒等在对应 future 上的调用方。
    // 这一步必须在 I/O 真正完成之后做——调用方 future.get() 返回就意味着
    // data_ 缓冲区已经可以安全读取/复用了。
    request->callback_.set_value(true);
  }
}

}  // namespace bustub
