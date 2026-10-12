#pragma once
// SPDX-License-Identifier: MIT
#include "BlockDevice.h"
#include <string.h>
namespace esp32_nfs {
namespace storage {
inline uint64_t nextMountGeneration() {
  static uint64_t value = 0;
  return value == UINT64_MAX ? 0 : ++value;
}
inline Result BlockIo::execute(Request r) {
  Geometry g = device_.geometry();
  if (!g.generation || !g.blocks || !g.blockBytes || g.blockBytes > 65536 ||
      (g.blockBytes & (g.blockBytes - 1)))
    return {Error::NoMedia};
  if (r.generation != g.generation)
    return {Error::Changed};
  if (r.operation != Operation::Read && r.operation != Operation::Write &&
      r.operation != Operation::Flush)
    return {Error::Invalid};
  if (r.operation == Operation::Flush) {
    if (!g.durableFlush)
      return {Error::Unsupported};
  } else {
    if (!r.blocks || r.blocks > g.maxTransferBlocks || r.lba > g.blocks ||
        r.blocks > g.blocks - r.lba || r.blocks > SIZE_MAX / g.blockBytes)
      return {Error::Invalid};
    size_t bytes = size_t(r.blocks) * g.blockBytes;
    const void *p =
        r.operation == Operation::Read ? static_cast<const void *>(r.readBuffer) : r.writeBuffer;
    if (!p || r.bufferBytes < bytes || !g.bufferAlignment ||
        (reinterpret_cast<uintptr_t>(p) % g.bufferAlignment))
      return {Error::Invalid};
    if (r.operation == Operation::Write && !g.writable)
      return {Error::ReadOnly};
  }
  bool admitted = false;
  uint32_t began = waiter_.now ? waiter_.now(waiter_.context) : 0;
  Result result = device_.start(r);
  while (result.error == Error::Busy || result.error == Error::Pending) {
    admitted |= result.error == Error::Pending;
    if (!waiter_.now || !waiter_.wait || !waiter_.timeoutMs || waiter_.timeoutMs > INT32_MAX) {
      if (admitted)
        device_.cancel();
      return {Error::Busy};
    }
    if (device_.geometry().generation != g.generation) {
      if (admitted)
        device_.cancel();
      return {Error::Changed};
    }
    if (uint32_t(waiter_.now(waiter_.context) - began) >= waiter_.timeoutMs) {
      if (admitted)
        device_.cancel();
      return {Error::Timeout};
    }
    waiter_.wait(waiter_.context, 1);
    result = admitted ? device_.poll() : device_.start(r);
    // Busy is an admission failure, never a valid completion of our admitted request.
    if (admitted && result.error == Error::Busy) {
      device_.cancel();
      return {Error::Io};
    }
  }
  if (device_.geometry().generation != g.generation)
    return {Error::Changed};
  if (result.error == Error::Ok && r.operation != Operation::Flush && result.blocks != r.blocks)
    return {Error::Io, result.blocks};
  if (result.error == Error::Ok && r.operation == Operation::Flush && !result.durable)
    return {Error::Unsupported};
  return result;
}
inline Error BlockIo::read(uint64_t l, uint32_t n, uint8_t *p, size_t b) {
  Request r;
  r.lba = l;
  r.blocks = n;
  r.readBuffer = p;
  r.bufferBytes = b;
  r.generation = geometry().generation;
  return execute(r).error;
}
inline Error BlockIo::write(uint64_t l, uint32_t n, const uint8_t *p, size_t b) {
  Request r;
  r.operation = Operation::Write;
  r.lba = l;
  r.blocks = n;
  r.writeBuffer = p;
  r.bufferBytes = b;
  r.generation = geometry().generation;
  return execute(r).error;
}
inline Error BlockIo::flush() {
  Request r;
  r.operation = Operation::Flush;
  r.generation = geometry().generation;
  return execute(r).error;
}
inline Geometry Partition::geometry() const {
  Geometry g = parent_.geometry();
  if (start_ > g.blocks || count_ > g.blocks - start_) {
    g.blocks = 0;
    g.generation = 0;
    return g;
  }
  g.blocks = count_;
  g.writable &= !readOnly_;
  return g;
}
inline Result Partition::start(const Request &request) {
  Geometry g = geometry();
  if (!g.generation)
    return {Error::Invalid};
  if (request.generation != g.generation)
    return {Error::Changed};
  if (request.operation == Operation::Write && readOnly_)
    return {Error::ReadOnly};
  Request r = request;
  if (r.operation != Operation::Flush) {
    if (r.lba > count_ || r.blocks > count_ - r.lba)
      return {Error::Invalid};
    r.lba += start_;
  }
  return parent_.start(r);
}
inline Geometry MemoryDevice::geometry() const {
  Geometry g;
  g.blocks = bytes_ && blockBytes_ ? size_ / blockBytes_ : 0;
  g.blockBytes = blockBytes_;
  g.generation = generation_;
  g.identity = reinterpret_cast<uintptr_t>(bytes_);
  g.writable = !readOnly_;
  g.maxTransferBlocks = 8;
  return g;
}
inline Result MemoryDevice::start(const Request &r) {
  if (r.operation != Operation::Read && r.operation != Operation::Write && r.operation != Operation::Flush)
    return {Error::Invalid};
  if (!bytes_) return {Error::NoMedia};
  Geometry g = geometry();
  if (r.generation != g.generation)
    return {Error::Changed};
  if (r.operation == Operation::Flush)
    return {Error::Unsupported};
  if (!r.blocks || r.lba > g.blocks || r.blocks > g.blocks - r.lba ||
      r.blocks > SIZE_MAX / blockBytes_)
    return {Error::Invalid};
  size_t n = size_t(r.blocks) * blockBytes_;
  if (r.bufferBytes < n)
    return {Error::Invalid};
  if (r.operation == Operation::Read) {
    if (!r.readBuffer)
      return {Error::Invalid};
    memcpy(r.readBuffer, bytes_ + size_t(r.lba) * blockBytes_, n);
  } else {
    if (readOnly_)
      return {Error::ReadOnly};
    if (!r.writeBuffer)
      return {Error::Invalid};
    memcpy(bytes_ + size_t(r.lba) * blockBytes_, r.writeBuffer, n);
  }
  return {Error::Ok, r.blocks};
}
} // namespace storage
} // namespace esp32_nfs
