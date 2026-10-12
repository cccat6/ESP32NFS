// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace esp32_nfs {
namespace storage {
constexpr uint32_t InterfaceVersion = 1;
// Serialized volume lifecycle only; unique within one firmware lifetime. Zero means exhausted.
uint64_t nextMountGeneration();
enum class Error : uint8_t {
  Ok,
  Pending,
  Busy,
  Invalid,
  NoMedia,
  Changed,
  ReadOnly,
  Unsupported,
  Timeout,
  Io,
  RecoveryRequired,
  NoSpace
};
enum class Operation : uint8_t { Read, Write, Flush };
struct Geometry {
  uint64_t blocks = 0, generation = 0, identity = 0;
  uint32_t blockBytes = 512, maxTransferBlocks = 8, bufferAlignment = 1;
  bool writable = false, durableFlush = false;
  // After successful flush, acknowledged blocks survive; failed/torn writes affect only requested
  // blocks. Journal recovery requires this contract from the controller/translation layer.
  bool isolatedWrites = false;
};
struct Request {
  Operation operation = Operation::Read;
  uint64_t lba = 0, generation = 0;
  uint32_t blocks = 0;
  uint8_t *readBuffer = nullptr;
  const uint8_t *writeBuffer = nullptr;
  size_t bufferBytes = 0;
};
struct Result {
  Error error;
  uint32_t blocks;
  bool durable;
  constexpr Result(Error e = Error::Ok, uint32_t n = 0, bool d = false)
      : error(e), blocks(n), durable(d) {}
};
class BlockDevice {
public:
  virtual ~BlockDevice() {}
  // Cached metadata only. Physical probing/preparation is an explicit driver operation.
  virtual Geometry geometry() const = 0;
  // Busy means no admission. Pending means admitted exactly once; poll/cancel only until terminal.
  // Buffers are borrowed through terminal completion/cancel. cancel synchronously detaches
  // DMA/buffers.
  virtual Result start(const Request &) = 0;
  virtual Result poll() = 0;
  virtual void cancel() = 0;
};
struct Waiter {
  void *context = nullptr;
  uint32_t (*now)(void *) = nullptr;
  void (*wait)(void *,
               uint32_t) = nullptr; // Yield/event wait, never a spin loop or recursive NFS poll.
  uint32_t timeoutMs = 5000;
};
class BlockIo {
public:
  BlockIo(BlockDevice &device, const Waiter &waiter = Waiter())
      : device_(device), waiter_(waiter) {}
  void cooperate() {
    if (waiter_.wait)
      waiter_.wait(waiter_.context, 0);
  }
  Geometry geometry() const {
    return device_.geometry();
  }
  BlockDevice &device() {
    return device_;
  }
  Result execute(Request);
  Error read(uint64_t lba, uint32_t blocks, uint8_t *data, size_t bytes);
  Error write(uint64_t lba, uint32_t blocks, const uint8_t *data, size_t bytes);
  Error flush();

private:
  BlockDevice &device_;
  Waiter waiter_;
};
class Partition : public BlockDevice {
public:
  Partition(BlockDevice &parent, uint64_t start, uint64_t count, bool readOnly = false)
      : parent_(parent), start_(start), count_(count), readOnly_(readOnly) {}
  Geometry geometry() const override;
  Result start(const Request &) override;
  Result poll() override {
    return parent_.poll();
  }
  void cancel() override {
    parent_.cancel();
  }

private:
  BlockDevice &parent_;
  uint64_t start_, count_;
  bool readOnly_;
};
// Native driver callback ABI: USB MSC, SPI storage, simulation and other controllers use the same
// contract.
struct Driver {
  void *context;
  Geometry (*geometry)(void *);
  Result (*start)(void *, const Request &);
  Result (*poll)(void *);
  void (*cancel)(void *);
};
class CallbackDevice : public BlockDevice {
public:
  explicit CallbackDevice(const Driver &driver) : driver_(driver) {}
  Geometry geometry() const override {
    return driver_.geometry ? driver_.geometry(driver_.context) : Geometry();
  }
  Result start(const Request &r) override {
    return driver_.geometry && driver_.start && driver_.poll && driver_.cancel
               ? driver_.start(driver_.context, r)
               : Result(Error::Unsupported);
  }
  Result poll() override {
    return driver_.poll ? driver_.poll(driver_.context) : Result(Error::Invalid);
  }
  void cancel() override {
    if (driver_.cancel)
      driver_.cancel(driver_.context);
  }

private:
  Driver driver_;
};
// Borrowed memory blocks. Volatile RAM never claims durable flush; ROM rejects writes.
class MemoryDevice : public BlockDevice {
public:
  MemoryDevice(uint8_t *bytes, size_t size, uint32_t blockBytes = 512, bool readOnly = false)
      : bytes_(bytes), size_(size), blockBytes_(blockBytes), readOnly_(readOnly) {}
  Geometry geometry() const override;
  Result start(const Request &) override;
  Result poll() override {
    return {Error::Invalid, 0, false};
  }
  void cancel() override {}
  void mediaChanged() {
    ++generation_;
  }

private:
  uint8_t *bytes_;
  size_t size_;
  uint32_t blockBytes_;
  bool readOnly_;
  uint64_t generation_ = 1;
};
} // namespace storage
} // namespace esp32_nfs

#include "BlockDevice.ipp"
