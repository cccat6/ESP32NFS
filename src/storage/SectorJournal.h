// SPDX-License-Identifier: MIT
#pragma once
#include "BlockDevice.h"
#ifndef ESP32_NFS_TX_SECTORS
#define ESP32_NFS_TX_SECTORS 32
#endif
namespace esp32_nfs {
namespace storage {
// 512-byte logical device, reserved log OUTSIDE the data range. Never autoformats a journal.
// Transactions isolate all filesystem writes; sync while active is staging, not durable commit.
class SectorJournal : public BlockDevice {
public:
  static constexpr uint32_t MaxSectors = ESP32_NFS_TX_SECTORS;
  static constexpr uint32_t RequiredJournalBlocks = 2 + 2 * MaxSectors;
  SectorJournal(BlockIo &parent, uint64_t dataStart, uint64_t dataBlocks, uint64_t journalStart)
      : parent_(parent), dataStart_(dataStart), dataBlocks_(dataBlocks),
        journalStart_(journalStart) {}
  Error formatJournal(); // Explicit provision operation; refuses overlap/unsupported durability.
  Error recover(); // Replay committed log before mounting filesystem. Prepared logs are discarded.
  Error begin();
  Error commit();
  Error abort();
  Error stagingBarrier();
  Error lastError() const {
    return lastError_;
  }
  uint32_t usedRecords() const {
    return count_;
  }
  bool active() const {
    return active_;
  }
  bool healthy() const {
    return ready_ && !fault_;
  }
  Geometry geometry() const override;
  Result start(const Request &) override;
  Result poll() override {
    return {Error::Invalid};
  } // Parent Io handles asynchronous/event waits.
  void cancel() override {}

private:
  struct Index {
    uint64_t lba;
    uint32_t slot, blocks;
    bool zero, dirty;
  };
  Index index_[MaxSectors];
  uint32_t count_ = 0;
  BlockIo &parent_;
  uint64_t dataStart_, dataBlocks_, journalStart_, sequence_ = 0, transaction_ = 0,
                                                   parentGeneration_ = 0, mountGeneration_ = 1;
  Error lastError_ = Error::Ok;
  bool ready_ = false, active_ = false, fault_ = false;
  alignas(64) uint8_t scratch_[512];
  Error validate() const;
  Error superblock(uint32_t state, uint32_t count);
  bool decodeSuper(const uint8_t *, uint64_t &sequence, uint32_t &state, uint32_t &count) const;
  Error writeRecord(uint32_t slot, uint64_t lba, const uint8_t *);
  Error readRecord(uint32_t slot, uint64_t &lba, uint8_t *data, uint32_t &blocks);
  Error writeZero(uint32_t slot);
  Error fail(Error e) {
    lastError_ = e;
    fault_ = true;
    ready_ = false;
    return e;
  }
};
} // namespace storage
} // namespace esp32_nfs

#include "SectorJournal.ipp"
