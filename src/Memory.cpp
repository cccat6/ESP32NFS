// SPDX-License-Identifier: MIT
#include "Memory.h"
#include <stdlib.h>
#include <string.h>
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#elif defined(ARDUINO_ARCH_RP2040)
#include <Arduino.h>
#endif
namespace esp32_nfs {
namespace {
void *systemAllocate(void *, size_t bytes) {
  return malloc(bytes);
}
void systemRelease(void *, void *block) {
  free(block);
}
size_t systemAvailable(void *) {
#if defined(ARDUINO_ARCH_ESP32) || defined(ESP_PLATFORM)
  return heap_caps_get_free_size(MALLOC_CAP_8BIT);
#elif defined(ARDUINO_ARCH_RP2040)
  return rp2040.getFreeHeap();
#else
  return SIZE_MAX;
#endif
}
Memory::Allocator systemAllocator() {
  Memory::Allocator a;
  a.allocate = systemAllocate;
  a.release = systemRelease;
  a.available = systemAvailable;
  return a;
}
} // namespace
Memory::Memory() : Memory(MemoryLimits()) {}
Memory::Memory(const MemoryLimits &limits) : Memory(limits, systemAllocator()) {}
Memory::Memory(const MemoryLimits &limits, const Allocator &allocator)
    : limits_(limits), allocator_(allocator) {}
bool Memory::getLimits(MemoryLimits &out) const {
  if (control_.test_and_set(std::memory_order_acquire))
    return false;
  out = limits_;
  control_.clear(std::memory_order_release);
  return true;
}
MemoryStats Memory::stats() const {
  MemoryStats s;
  s.usedBytes = used_.load();
  s.peakBytes = peak_.load();
  s.allocations = allocations_.load();
  s.failures = failures_.load();
  s.lastError = error_.load();
  return s;
}
bool Memory::configure(const MemoryLimits &limits) {
  if (control_.test_and_set(std::memory_order_acquire)) {
    error_ = MemoryError::Busy;
    return false;
  }
  bool valid = limits.maxBytes && limits.maxBytes >= used_.load();
  if (valid)
    limits_ = limits;
  control_.clear(std::memory_order_release);
  error_ = valid ? MemoryError::None : MemoryError::Invalid;
  return valid;
}
void *Memory::failure(MemoryError error) {
  error_ = error;
  ++failures_;
  return nullptr;
}
void *Memory::allocate(size_t bytes, bool clear) {
  if (!bytes || !allocator_.allocate || !allocator_.release)
    return failure(MemoryError::Invalid);
  if (control_.test_and_set(std::memory_order_acquire))
    return failure(MemoryError::Busy);
  const size_t cost = charge(bytes), used = used_.load(), reserve = limits_.reserveBytes;
  bool fits = cost <= limits_.maxBytes && used <= limits_.maxBytes - cost;
  if (fits)
    used_.fetch_add(cost);
  control_.clear(std::memory_order_release);
  if (!fits)
    return failure(MemoryError::Budget);
  if (allocator_.available) {
    size_t free = allocator_.available(allocator_.context);
    if (free < cost || free - cost < reserve) {
      used_.fetch_sub(cost);
      return failure(MemoryError::Reserve);
    }
  }
  void *block = allocator_.allocate(allocator_.context, bytes);
  if (!block) {
    used_.fetch_sub(cost);
    return failure(MemoryError::Allocation);
  }
  if (clear)
    memset(block, 0, bytes);
  size_t peak = peak_.load(), now = used_.load();
  while (peak < now && !peak_.compare_exchange_weak(peak, now)) {
  }
  ++allocations_;
  error_ = MemoryError::None;
  return block;
}
void Memory::release(void *block, size_t bytes) {
  if (!block)
    return;
  allocator_.release(allocator_.context, block);
  used_.fetch_sub(charge(bytes));
  --allocations_;
}
Memory &defaultMemory() {
  static Memory memory;
  return memory;
}
} // namespace esp32_nfs
