#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "common/config.h"
#include "storage/table/tuple.h"

namespace bustub {

/**
 * Page to hold the intermediate data for external merge sort and hash join.
 * Supports variable-length tuples.
 *
 * ==== P3 STEP 18: 为什么中间结果也要落在「页」上 ====
 * 外部归并排序存在的全部理由就是「数据装不下内存」。如果中间结果用
 * std::vector<Tuple> 存在堆上，那和内存排序没有任何区别。
 * 把每一趟的有序段写进由缓冲池管理的页里，才真正做到：
 *   - 内存占用受缓冲池大小约束，与数据总量无关；
 *   - 冷的段被自动换出到磁盘，需要时再换回来 —— P1 的工作在这里被直接复用。
 *
 * 页内布局（变长元组）：
 *
 *   +--------+--------+---------------------------------------------+
 *   | count_ | used_  |  [len][tuple bytes][len][tuple bytes] ...   |
 *   +--------+--------+---------------------------------------------+
 *      4B       4B                    data_
 *
 * 每条元组前缀一个 4 字节长度，正好是 Tuple::SerializeTo /
 * Tuple::DeserializeFrom 约定的格式，可以直接复用。
 *
 * 之所以不用「页尾槽位数组」那种支持随机访问的经典布局，是因为
 * 归并排序对中间结果**只做顺序扫描**，顺序格式更紧凑也更简单。
 */
class IntermediateResultPage {
 public:
  /** 页内可用于数据的字节数。 */
  static constexpr size_t DATA_CAPACITY = BUSTUB_PAGE_SIZE - 2 * sizeof(uint32_t);

  /** @brief 初始化一个空页。必须在从缓冲池拿到新页后立刻调用。 */
  void Init() {
    count_ = 0;
    used_ = 0;
  }

  /** @return 本页存了多少条元组。 */
  auto GetTupleCount() const -> uint32_t { return count_; }

  /** @return 本页是否一条元组都没有。 */
  auto IsEmpty() const -> bool { return count_ == 0; }

  /**
   * @brief 在页尾追加一条元组。
   * @return 空间不足时返回 false，调用方据此换一页新的。
   */
  auto Append(const Tuple &tuple) -> bool {
    const uint32_t len = tuple.GetLength();
    if (used_ + sizeof(uint32_t) + len > DATA_CAPACITY) {
      return false;
    }
    // SerializeTo 写的正是 [4 字节长度][内容]，与本页的存储格式一致。
    tuple.SerializeTo(data_ + used_);
    used_ += sizeof(uint32_t) + len;
    ++count_;
    return true;
  }

  /**
   * @brief 从字节偏移 `*offset` 处读出一条元组，并把 offset 推进到下一条。
   *
   * 顺序游标式的接口：调用方只需维护一个 uint32_t 偏移量，
   * 不需要页内的槽位目录。
   */
  auto ReadAt(uint32_t offset) const -> Tuple {
    Tuple tuple;
    tuple.DeserializeFrom(data_ + offset);
    return tuple;
  }

  /**
   * @brief 把偏移量推进到下一条元组，不构造 Tuple。
   *
   * 迭代器的 `operator*` 必须是无副作用的（连续两次解引用要得到同一条元组），
   * 所以「读」和「推进」被拆成两个方法：ReadAt 只读不动，SkipAt 只动不读。
   * 推进时只需要读那 4 字节的长度前缀，不必反序列化整条元组。
   */
  void SkipAt(uint32_t *offset) const {
    uint32_t len = 0;
    std::memcpy(&len, data_ + *offset, sizeof(uint32_t));
    *offset += sizeof(uint32_t) + len;
  }

 private:
  /** 本页的元组条数。 */
  uint32_t count_;
  /** data_ 中已使用的字节数。 */
  uint32_t used_;
  /** 紧凑存放的 [长度][内容] 序列。 */
  char data_[DATA_CAPACITY];
};

}  // namespace bustub
