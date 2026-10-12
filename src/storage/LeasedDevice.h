// SPDX-License-Identifier: MIT
#pragma once
#include "BlockDevice.h"
namespace esp32_nfs {
namespace storage {
struct BusLease {
  void *context;
  bool (*acquire)(void *);
  void (*release)(void *);
};
// Bus ownership covers one admitted transfer, not an entire filesystem operation.
class LeasedDevice : public BlockDevice {
public:
  LeasedDevice(BlockDevice &parent, const BusLease &lease) : parent_(parent), lease_(lease) {}
  ~LeasedDevice() {
    cancel();
  }
  LeasedDevice(const LeasedDevice &) = delete;
  LeasedDevice &operator=(const LeasedDevice &) = delete;
  Geometry geometry() const override {
    return parent_.geometry();
  }
  Result start(const Request &r) override {
    if (!lease_.acquire || !lease_.release)
      return {Error::Unsupported};
    if (held_)
      return {Error::Busy};
    if (!lease_.acquire(lease_.context))
      return {Error::Busy};
    held_ = true;
    Result result = parent_.start(r);
    if (result.error != Error::Pending)
      release();
    return result;
  }
  Result poll() override {
    if (!held_)
      return {Error::Invalid};
    Result result = parent_.poll();
    if (result.error == Error::Busy) {
      parent_.cancel();
      result = Result(Error::Io);
    }
    if (result.error != Error::Pending)
      release();
    return result;
  }
  void cancel() override {
    if (held_) {
      parent_.cancel();
      release();
    }
  }

private:
  BlockDevice &parent_;
  BusLease lease_;
  bool held_ = false;
  void release() {
    lease_.release(lease_.context);
    held_ = false;
  }
};
} // namespace storage
} // namespace esp32_nfs
