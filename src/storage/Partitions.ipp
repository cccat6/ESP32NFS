#pragma once
// SPDX-License-Identifier: MIT
#include "Partitions.h"
#include <string.h>
namespace esp32_nfs {
namespace storage {
namespace partition_detail {
inline uint32_t u32(const uint8_t *p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint64_t u64(const uint8_t *p) {
  return uint64_t(u32(p)) | uint64_t(u32(p + 4)) << 32;
}
inline uint32_t crc(const uint8_t *p, size_t n, uint32_t state = 0xffffffff) {
  while (n--) {
    state ^= *p++;
    for (unsigned i = 0; i < 8; ++i)
      state = (state >> 1) ^ (0xedb88320u & uint32_t(-int32_t(state & 1)));
  }
  return state;
}
inline bool overlap(const PartitionInfo *out, size_t n, uint64_t start, uint64_t count) {
  for (size_t i = 0; i < n; ++i)
    if (start < out[i].first + out[i].blocks && out[i].first < start + count)
      return true;
  return false;
}
} // namespace
inline Error discoverPartitions(BlockIo &io, PartitionInfo *out, size_t cap, size_t &count, uint8_t *b,
                         size_t bytes, uint32_t maxEntries) {
  count = 0;
  auto g = io.geometry();
  if (!out || !cap || !b || bytes < g.blockBytes || g.blockBytes < 512 || g.blockBytes > 4096)
    return Error::Invalid;
  Error e = io.read(0, 1, b, bytes);
  if (e != Error::Ok)
    return e;
  if (b[510] != 0x55 || b[511] != 0xaa)
    return Error::Unsupported;
  bool gpt = false;
  for (unsigned i = 0; i < 4; ++i) {
    const uint8_t *entry = b + 446 + i * 16;
    uint8_t type = entry[4];
    if (type == 0xee) {
      gpt = true;
      break;
    }
  }
  if (!gpt) {
    for (unsigned i = 0; i < 4; ++i) {
      const uint8_t *p = b + 446 + i * 16;
      uint8_t type = p[4];
      uint64_t first = partition_detail::u32(p + 8), n = partition_detail::u32(p + 12);
      if (!type || !n)
        continue;
      if (type == 5 || type == 15 || type == 0x85)
        return Error::Unsupported; // EBR chains require a separate bounded walker.
      if (!first || first >= g.blocks || n > g.blocks - first || partition_detail::overlap(out, count, first, n))
        return Error::Invalid;
      if (count == cap)
        return Error::NoSpace;
      out[count] = PartitionInfo();
      out[count].first = first;
      out[count].blocks = n;
      out[count].type = type;
      ++count;
    }
    return Error::Ok;
  }
  e = io.read(1, 1, b, bytes);
  if (e != Error::Ok)
    return e;
  if (memcmp(b, "EFI PART", 8))
    return Error::Invalid;
  uint32_t length = partition_detail::u32(b + 12), expected = partition_detail::u32(b + 16);
  if (length < 92 || length > g.blockBytes || partition_detail::u64(b + 24) != 1 || partition_detail::u64(b + 32) >= g.blocks)
    return Error::Invalid;
  memset(b + 16, 0, 4);
  if ((partition_detail::crc(b, length) ^ 0xffffffff) != expected)
    return Error::Invalid;
  uint64_t firstUsable = partition_detail::u64(b + 40), lastUsable = partition_detail::u64(b + 48), table = partition_detail::u64(b + 72);
  uint32_t entries = partition_detail::u32(b + 80), stride = partition_detail::u32(b + 84), tableCrc = partition_detail::u32(b + 88);
  if (!entries || entries > maxEntries || stride < 128 || stride > 512 || (stride & 7) ||
      firstUsable > lastUsable || lastUsable >= g.blocks || table >= g.blocks)
    return Error::Unsupported;
  uint64_t tableBytes = uint64_t(entries) * stride,
           tableBlocks = (tableBytes + g.blockBytes - 1) / g.blockBytes;
  if (tableBlocks > g.blocks - table || table + tableBlocks > firstUsable)
    return Error::Invalid;
  uint32_t checksum = 0xffffffff;
  for (uint64_t sector = 0; sector < tableBlocks; ++sector) {
    e = io.read(table + sector, 1, b, bytes);
    if (e != Error::Ok)
      return e;
    size_t n = size_t(tableBytes - sector * g.blockBytes);
    if (n > g.blockBytes)
      n = g.blockBytes;
    checksum = partition_detail::crc(b, n, checksum);
  }
  if ((checksum ^ 0xffffffff) != tableCrc)
    return Error::Invalid;
  // GPT records may cross sector boundaries; a fixed 512-byte entry buffer handles it without
  // allocation.
  uint8_t record[512];
  for (uint32_t i = 0; i < entries; ++i) {
    uint64_t offset = uint64_t(i) * stride;
    size_t copied = 0;
    while (copied < stride) {
      uint64_t sector = offset / g.blockBytes;
      size_t at = offset % g.blockBytes;
      e = io.read(table + sector, 1, b, bytes);
      if (e != Error::Ok)
        return e;
      size_t n = stride - copied;
      if (n > g.blockBytes - at)
        n = g.blockBytes - at;
      memcpy(record + copied, b + at, n);
      copied += n;
      offset += n;
    }
    bool empty = true;
    for (unsigned j = 0; j < 16; ++j)
      empty &= record[j] == 0;
    if (empty)
      continue;
    uint64_t start = partition_detail::u64(record + 32), last = partition_detail::u64(record + 40);
    if (start < firstUsable || last > lastUsable || start > last ||
        partition_detail::overlap(out, count, start, last - start + 1))
      return Error::Invalid;
    if (count == cap)
      return Error::NoSpace;
    out[count] = PartitionInfo();
    out[count].first = start;
    out[count].blocks = last - start + 1;
    memcpy(out[count].guid, record, 16);
    out[count].readOnly = (partition_detail::u64(record + 48) & (uint64_t(1) << 60)) != 0;
    ++count;
  }
  return Error::Ok;
}
} // namespace storage
} // namespace esp32_nfs
