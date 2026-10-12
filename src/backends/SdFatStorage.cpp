// SPDX-License-Identifier: MIT
#include "SdFatStorage.h"
namespace esp32_nfs {
Status SdFatStorage::transactImpl(Status (*operation)(void *), void *argument) {
  if (poison_)
    return Io;
  invalidateCaches();
  if (!journal_) {
    Status s = operation(argument);
    invalidateCaches();
    return s;
  }
  if (!remount_ || !journal_->healthy())
    return Io;
  storage::Error e = journal_->begin();
  if (e != storage::Error::Ok)
    return Io;
  Status status = operation(argument);
  invalidateCaches();
  if (status == Ok) {
    e = journal_->commit();
    if (e == storage::Error::Ok)
      return Ok;
    poison_ = true;
    return Io;
  }
  e = journal_->abort();
  if (e != storage::Error::Ok)
    e = journal_->recover();
  verifiers_.invalidate();
  if (e != storage::Error::Ok || !remount_(context_)) {
    poison_ = true;
    return Io;
  }
  return status;
}
} // namespace esp32_nfs
