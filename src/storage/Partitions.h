// SPDX-License-Identifier: MIT
#pragma once
#include "BlockDevice.h"
namespace esp32_nfs {
namespace storage {
struct PartitionInfo {
  uint64_t first = 0, blocks = 0;
  uint8_t type = 0, guid[16] = {};
  bool readOnly = false;
};
// Strict bounded parser. Caller supplies an aligned physical-sector scratch buffer and output
// array.
Error discoverPartitions(BlockIo &, PartitionInfo *out, size_t capacity, size_t &count,
                         uint8_t *scratch, size_t bytes, uint32_t maxGptEntries = 128);
} // namespace storage
} // namespace esp32_nfs

#include "Partitions.ipp"
