// SPDX-License-Identifier: MIT
#pragma once
#include "BlockDevice.h"
#include "SectorJournal.h"
#include <SdFat.h>
#include <string.h>

namespace esp32_nfs {
namespace storage {
class SdFatBlockAdapter : public FsBlockDeviceInterface {
public:
  explicit SdFatBlockAdapter(BlockIo &io, SectorJournal *journal = nullptr)
      : io_(io), journal_(journal) {}
  bool isBusy() override {
    return false;
  }
  uint32_t sectorCount() override {
    auto g = io_.geometry();
    return g.blockBytes == 512 && g.blocks <= UINT32_MAX ? uint32_t(g.blocks) : 0;
  }
  bool readSector(uint32_t lba, uint8_t *data) override {
    return readSectors(lba, data, 1);
  }
  bool writeSector(uint32_t lba, const uint8_t *data) override {
    return writeSectors(lba, data, 1);
  }
  bool readSectors(uint32_t lba, uint8_t *data, size_t count) override {
    if (count > UINT32_MAX || uint64_t(lba) + count > sectorCount())
      return false;
    auto g = io_.geometry();
    if (g.bufferAlignment && reinterpret_cast<uintptr_t>(data) % g.bufferAlignment == 0 &&
        count <= g.maxTransferBlocks)
      return io_.read(lba, uint32_t(count), data, count * 512) == Error::Ok;
    for (size_t i = 0; i < count; ++i) {
      if (io_.read(lba + i, 1, bounce_, 512) != Error::Ok)
        return false;
      memcpy(data + i * 512, bounce_, 512);
      if (i % 8 == 7)
        io_.cooperate();
    }
    return true;
  }
  bool writeSectors(uint32_t lba, const uint8_t *data, size_t count) override {
    if (count > UINT32_MAX || uint64_t(lba) + count > sectorCount())
      return false;
    auto g = io_.geometry();
    if (g.bufferAlignment && reinterpret_cast<uintptr_t>(data) % g.bufferAlignment == 0 &&
        count <= g.maxTransferBlocks)
      return io_.write(lba, uint32_t(count), data, count * 512) == Error::Ok;
    for (size_t i = 0; i < count; ++i) {
      memcpy(bounce_, data + i * 512, 512);
      if (io_.write(lba + i, 1, bounce_, 512) != Error::Ok)
        return false;
      if (i % 8 == 7)
        io_.cooperate();
    }
    return true;
  }
  bool syncDevice() override {
    return journal_ && journal_->active() ? journal_->stagingBarrier() == Error::Ok
                                          : io_.flush() == Error::Ok;
  }

private:
  BlockIo &io_;
  SectorJournal *journal_;
  alignas(64) uint8_t bounce_[512];
};
// Stock SdFat card driver adapter: no initialization/pins owned here. Caller supplies stable CID
// identity.
class SdCardDevice : public BlockDevice {
public:
  SdCardDevice(FsBlockDeviceInterface &card, uint64_t blocks, uint64_t identity,
               bool writable = true, bool flushVerified = false, bool isolationVerified = false)
      : card_(card), blocks_(blocks), identity_(identity), writable_(writable),
        flush_(flushVerified), isolation_(isolationVerified) {}
  Geometry geometry() const override {
    Geometry g;
    g.blocks = blocks_;
    g.identity = identity_;
    g.generation = generation_;
    g.writable = writable_;
    g.durableFlush = flush_;
    g.isolatedWrites = isolation_;
    return g;
  }
  Result start(const Request &r) override {
    if (r.generation != generation_)
      return {Error::Changed};
    if (r.operation == Operation::Flush)
      return {!flush_ ? Error::Unsupported : card_.syncDevice() ? Error::Ok : Error::Io, 0, flush_};
    if (r.lba > UINT32_MAX || r.lba > blocks_ || r.blocks > blocks_ - r.lba ||
        r.bufferBytes < size_t(r.blocks) * 512)
      return {Error::Invalid};
    if (r.operation == Operation::Write && !writable_)
      return {Error::ReadOnly};
    bool ok = r.operation == Operation::Read
                  ? card_.readSectors(uint32_t(r.lba), r.readBuffer, r.blocks)
                  : card_.writeSectors(uint32_t(r.lba), r.writeBuffer, r.blocks);
    return {ok ? Error::Ok : Error::Io, ok ? r.blocks : 0};
  }
  Result poll() override {
    return {Error::Invalid};
  }
  void cancel() override {}
  void mediaChanged(uint64_t identity, uint64_t blocks) {
    identity_ = identity;
    blocks_ = blocks;
    ++generation_;
  }

private:
  FsBlockDeviceInterface &card_;
  uint64_t blocks_, identity_, generation_ = 1;
  bool writable_, flush_, isolation_;
};
} // namespace storage
} // namespace esp32_nfs
