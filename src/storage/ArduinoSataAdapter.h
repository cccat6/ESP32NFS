// SPDX-License-Identifier: MIT
#pragma once
#include "BlockDevice.h"
#include <ArduinoSATA.h>
namespace esp32_nfs {
namespace storage {
class ArduinoSataAdapter : public BlockDevice {
public:
  explicit ArduinoSataAdapter(arduino_sata::BlockDevice &drive, uint64_t identity,
                              uint32_t (*clock)(void *), void *context = nullptr,
                              uint32_t timeout = 5000, bool isolationVerified = false)
      : drive_(drive), identity_(identity), clock_(clock), context_(context), timeout_(timeout),
        isolationVerified_(isolationVerified) {}
  Geometry geometry() const override {
    Geometry g;
    auto &info = drive_.info();
    g.blocks = info.sectors;
    g.blockBytes = info.sectorBytes;
    g.generation = drive_.ready() ? drive_.generation() : 0;
    g.identity = identity_;
    g.writable = drive_.writable();
    g.durableFlush = drive_.canFlush();
    g.isolatedWrites = isolationVerified_;
    g.maxTransferBlocks = 1;
    return g;
  }
  Result start(const Request &r) override {
    if (!clock_)
      return {Error::Invalid};
    uint32_t now = clock_(context_);
    arduino_sata::Status status;
    if (r.operation == Operation::Read)
      status =
          drive_.read(r.lba, r.blocks, r.readBuffer, r.bufferBytes, r.generation, now, timeout_);
    else if (r.operation == Operation::Write)
      status = drive_.write(r.lba, r.blocks, r.writeBuffer, r.bufferBytes, r.generation,
                            arduino_sata::Durability::Unstable, now, timeout_);
    else
      status = drive_.flush(r.generation, now, timeout_);
    return status == arduino_sata::Status::Ok ? poll() : Result{convert(status)};
  }
  Result poll() override {
    auto done = drive_.poll(clock_(context_));
    return {convert(done.status), done.sectors,
            done.durability == arduino_sata::Durability::Flushed};
  }
  void cancel() override {
    drive_.cancel();
  }

private:
  arduino_sata::BlockDevice &drive_;
  uint64_t identity_;
  uint32_t (*clock_)(void *);
  void *context_;
  uint32_t timeout_;
  bool isolationVerified_;
  static Error convert(arduino_sata::Status s) {
    using S = arduino_sata::Status;
    switch (s) {
    case S::Ok:
      return Error::Ok;
    case S::Pending:
      return Error::Pending;
    case S::Busy:
      return Error::Busy;
    case S::ReadOnly:
      return Error::ReadOnly;
    case S::Unsupported:
      return Error::Unsupported;
    case S::NoMedia:
      return Error::NoMedia;
    case S::MediaChanged:
      return Error::Changed;
    case S::Timeout:
      return Error::Timeout;
    case S::InvalidArgument:
      return Error::Invalid;
    default:
      return Error::Io;
    }
  }
};
} // namespace storage
} // namespace esp32_nfs
