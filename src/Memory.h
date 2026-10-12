// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <stddef.h>
#include <stdint.h>
namespace esp32_nfs {
enum class MemoryError : uint8_t { None, Budget, Reserve, Allocation, Invalid, Busy };
struct MemoryLimits {
  size_t maxBytes = 128 * 1024;
  size_t reserveBytes = 32 * 1024;
};
struct MemoryStats {
  size_t usedBytes = 0, peakBytes = 0, allocations = 0;
  uint32_t failures = 0;
  MemoryError lastError = MemoryError::None;
};
// Bounded resource owner. Share one instance across services/backends to share a budget.
// Custom callbacks let an application select a heap or supply deterministic failure injection.
class Memory {
public:
  struct Allocator {
    void *context = nullptr;
    void *(*allocate)(void *, size_t) = nullptr;
    void (*release)(void *, void *) = nullptr;
    size_t (*available)(void *) = nullptr;
  };
  Memory();
  explicit Memory(const MemoryLimits &limits);
  Memory(const MemoryLimits &limits, const Allocator &allocator);
  Memory(const Memory &) = delete;
  Memory &operator=(const Memory &) = delete;
  bool configure(const MemoryLimits &limits);
  bool getLimits(MemoryLimits &out) const;
  MemoryStats stats() const;
  void *allocate(size_t bytes, bool clear = false);
  void release(void *block, size_t bytes);
  // Includes a conservative allowance for the underlying allocator's bookkeeping.
  static size_t charge(size_t bytes) {
    return bytes <= SIZE_MAX - 16 ? bytes + 16 : SIZE_MAX;
  }

private:
  MemoryLimits limits_;
  Allocator allocator_;
  mutable std::atomic<uint32_t> control_{0};
  bool enter() const {
    uint32_t expected = 0;
    return control_.compare_exchange_strong(expected, 1, std::memory_order_acquire);
  }
  void leave() const {
    control_.store(0, std::memory_order_release);
  }
  std::atomic<size_t> used_{0}, peak_{0}, allocations_{0};
  std::atomic<uint32_t> failures_{0};
  std::atomic<MemoryError> error_{MemoryError::None};
  void *failure(MemoryError error);
};
Memory &defaultMemory();
} // namespace esp32_nfs
