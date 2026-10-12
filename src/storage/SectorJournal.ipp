#pragma once
// SPDX-License-Identifier: MIT
#include "SectorJournal.h"
#include "../Digest.h"
#include <string.h>
namespace esp32_nfs {
namespace storage {
namespace journal_detail {
inline void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = n >> (24 - 8 * i);
}
inline void put64(uint8_t *p, uint64_t n) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = n >> (56 - 8 * i);
}
inline uint32_t get32(const uint8_t *p) {
  uint32_t n = 0;
  for (unsigned i = 0; i < 4; ++i)
    n = n << 8 | p[i];
  return n;
}
inline uint64_t get64(const uint8_t *p) {
  uint64_t n = 0;
  for (unsigned i = 0; i < 8; ++i)
    n = n << 8 | p[i];
  return n;
}
inline void seal(uint8_t *p) {
  sha256(p, 480, p + 480);
}
inline bool valid(const uint8_t *p) {
  uint8_t d[32];
  sha256(p, 480, d);
  return !memcmp(d, p + 480, 32);
}
} // namespace
inline Error SectorJournal::validate() const {
  auto g = parent_.geometry();
  if (g.blockBytes != 512 || !g.identity || !g.durableFlush || !g.isolatedWrites || !g.writable)
    return Error::Unsupported;
  if (!dataBlocks_ || dataStart_ > g.blocks || dataBlocks_ > g.blocks - dataStart_ ||
      journalStart_ > g.blocks || RequiredJournalBlocks > g.blocks - journalStart_)
    return Error::Invalid;
  if (dataStart_ < journalStart_ + RequiredJournalBlocks &&
      journalStart_ < dataStart_ + dataBlocks_)
    return Error::Invalid;
  return Error::Ok;
}
inline Geometry SectorJournal::geometry() const {
  auto g = parent_.geometry();
  g.blocks = dataBlocks_;
  g.maxTransferBlocks = 1;
  g.generation = healthy() && g.generation == parentGeneration_ ? mountGeneration_ : 0;
  g.writable &= healthy();
  return g;
}
inline Error SectorJournal::superblock(uint32_t state, uint32_t count) {
  if (sequence_ == UINT64_MAX)
    return fail(Error::RecoveryRequired);
  memset(scratch_, 0, sizeof(scratch_));
  memcpy(scratch_, "ENFSTX01", 8);
  journal_detail::put64(scratch_ + 8, ++sequence_);
  journal_detail::put32(scratch_ + 16, state);
  journal_detail::put32(scratch_ + 20, count);
  journal_detail::put64(scratch_ + 24, dataStart_);
  journal_detail::put64(scratch_ + 32, dataBlocks_);
  journal_detail::put64(scratch_ + 40, parent_.geometry().identity);
  journal_detail::put64(scratch_ + 48, transaction_);
  journal_detail::put32(scratch_ + 56, MaxSectors);
  journal_detail::seal(scratch_);
  Error e = parent_.write(journalStart_ + (sequence_ & 1), 1, scratch_, 512);
  if (e == Error::Ok)
    e = parent_.flush();
  return e == Error::Ok ? e : fail(e);
}
inline bool SectorJournal::decodeSuper(const uint8_t *p, uint64_t &seq, uint32_t &state,
                                uint32_t &count) const {
  if (memcmp(p, "ENFSTX01", 8) || !journal_detail::valid(p) || journal_detail::get64(p + 24) != dataStart_ ||
      journal_detail::get64(p + 32) != dataBlocks_ || journal_detail::get64(p + 40) != parent_.geometry().identity ||
      journal_detail::get32(p + 56) != MaxSectors)
    return false;
  seq = journal_detail::get64(p + 8);
  state = journal_detail::get32(p + 16);
  count = journal_detail::get32(p + 20);
  return seq && state <= 2 && count <= MaxSectors;
}
inline Error SectorJournal::formatJournal() {
  Error e = validate();
  if (e != Error::Ok)
    return e;
  sequence_ = transaction_ = 0;
  fault_ = false;
  active_ = false;
  ready_ = false;
  e = superblock(0, 0);
  if (e == Error::Ok)
    e = superblock(0, 0);
  if (e != Error::Ok)
    return e;
  parentGeneration_ = parent_.geometry().generation;
  mountGeneration_ = nextMountGeneration();
  if (!mountGeneration_)
    return fail(Error::RecoveryRequired);
  ready_ = true;
  count_ = 0;
  return Error::Ok;
}
inline Error SectorJournal::readRecord(uint32_t slot, uint64_t &lba, uint8_t *data, uint32_t &blocks) {
  Error e = parent_.read(journalStart_ + 2 + 2 * slot, 1, scratch_, 512);
  if (e != Error::Ok)
    return e;
  if (memcmp(scratch_, "ENFSSEC1", 8) || !journal_detail::valid(scratch_) || journal_detail::get64(scratch_ + 8) != transaction_)
    return Error::RecoveryRequired;
  lba = journal_detail::get64(scratch_ + 16);
  blocks = journal_detail::get32(scratch_ + 56);
  bool zero = journal_detail::get32(scratch_ + 60) == 1;
  if (!blocks || lba >= dataBlocks_ || blocks > dataBlocks_ - lba)
    return Error::RecoveryRequired;
  uint8_t expected[32];
  memcpy(expected, scratch_ + 24, 32);
  if (zero)
    memset(data, 0, 512);
  else
    e = parent_.read(journalStart_ + 3 + 2 * slot, 1, data, 512);
  if (e != Error::Ok)
    return e;
  uint8_t digest[32];
  sha256(data, 512, digest);
  return memcmp(expected, digest, 32) ? Error::RecoveryRequired : Error::Ok;
}
inline Error SectorJournal::writeRecord(uint32_t slot, uint64_t lba, const uint8_t *data) {
  Error e = parent_.write(journalStart_ + 3 + 2 * slot, 1, data, 512);
  if (e != Error::Ok)
    return fail(e);
  memset(scratch_, 0, 512);
  memcpy(scratch_, "ENFSSEC1", 8);
  journal_detail::put64(scratch_ + 8, transaction_);
  journal_detail::put64(scratch_ + 16, lba);
  sha256(data, 512, scratch_ + 24);
  journal_detail::put32(scratch_ + 56, 1);
  journal_detail::seal(scratch_);
  e = parent_.write(journalStart_ + 2 + 2 * slot, 1, scratch_, 512);
  return e == Error::Ok ? e : fail(e);
}
inline Error SectorJournal::writeZero(uint32_t slot) {
  uint8_t digest[32];
  memset(scratch_, 0, 512);
  sha256(scratch_, 512, digest);
  memset(scratch_, 0, 512);
  memcpy(scratch_, "ENFSSEC1", 8);
  journal_detail::put64(scratch_ + 8, transaction_);
  journal_detail::put64(scratch_ + 16, index_[slot].lba);
  memcpy(scratch_ + 24, digest, 32);
  journal_detail::put32(scratch_ + 56, index_[slot].blocks);
  journal_detail::put32(scratch_ + 60, 1);
  journal_detail::seal(scratch_);
  Error e = parent_.write(journalStart_ + 2 + 2 * slot, 1, scratch_, 512);
  if (e == Error::Ok)
    index_[slot].dirty = false;
  return e == Error::Ok ? e : fail(e);
}
inline Error SectorJournal::recover() {
  Error e = validate();
  if (e != Error::Ok)
    return e;
  fault_ = false;
  ready_ = false;
  active_ = false;
  count_ = 0;
  uint64_t best = 0;
  uint32_t bestState = 0, bestCount = 0;
  uint64_t bestTx = 0;
  for (unsigned slot = 0; slot < 2; ++slot) {
    e = parent_.read(journalStart_ + slot, 1, scratch_, 512);
    if (e != Error::Ok)
      return fail(e);
    uint64_t seq;
    uint32_t state, count;
    if (decodeSuper(scratch_, seq, state, count) && seq > best) {
      best = seq;
      bestState = state;
      bestCount = count;
      bestTx = journal_detail::get64(scratch_ + 48);
    }
  }
  if (!best)
    return fail(Error::RecoveryRequired);
  sequence_ = best;
  transaction_ = bestTx;
  if (bestState == 2) {
    alignas(64) uint8_t data[512];
    // Verify the entire committed set before modifying any base sector.
    for (uint32_t i = 0; i < bestCount; ++i) {
      uint64_t lba;
      uint32_t blocks;
      e = readRecord(i, lba, data, blocks);
      if (e != Error::Ok)
        return fail(e);
    }
    for (uint32_t i = 0; i < bestCount; ++i) {
      uint64_t lba;
      uint32_t blocks;
      e = readRecord(i, lba, data, blocks);
      if (e == Error::Ok)
        for (uint32_t block = 0; block < blocks && e == Error::Ok; ++block)
          e = parent_.write(dataStart_ + lba + block, 1, data, 512);
      if (e != Error::Ok)
        return fail(e);
    }
    e = parent_.flush();
    if (e != Error::Ok)
      return fail(e);
  }
  e = superblock(0, 0);
  if (e != Error::Ok)
    return e;
  parentGeneration_ = parent_.geometry().generation;
  ready_ = true;
  mountGeneration_ = nextMountGeneration();
  if (!mountGeneration_)
    return fail(Error::RecoveryRequired);
  return Error::Ok;
}
inline Error SectorJournal::begin() {
  if (!healthy() || active_ || parentGeneration_ != parent_.geometry().generation)
    return Error::RecoveryRequired;
  count_ = 0;
  transaction_ = sequence_ + 1;
  Error e = superblock(1, 0);
  if (e == Error::Ok)
    active_ = true;
  return e;
}
inline Error SectorJournal::stagingBarrier() {
  if (!healthy())
    return Error::RecoveryRequired;
  Error e = Error::Ok;
  for (uint32_t i = 0; i < count_; ++i)
    if (index_[i].zero && index_[i].dirty) {
      e = writeZero(i);
      if (e != Error::Ok)
        return e;
    }
  e = parent_.flush();
  return e == Error::Ok ? e : fail(e);
}
inline Error SectorJournal::commit() {
  if (!healthy() || !active_)
    return Error::RecoveryRequired;
  Error e = stagingBarrier();
  if (e != Error::Ok)
    return e;
  e = superblock(2, count_);
  if (e != Error::Ok)
    return e;
  active_ = false;
  // Recovery roll-forward is also the normal commit path; base writes occur only after durable
  // intent.
  uint64_t generation = mountGeneration_;
  Error result = recover();
  if (result == Error::Ok)
    mountGeneration_ = generation;
  return result;
}
inline Error SectorJournal::abort() {
  if (fault_)
    return Error::RecoveryRequired;
  if (!active_)
    return Error::Invalid;
  active_ = false;
  count_ = 0;
  Error e = superblock(0, 0);
  // A clean rollback leaves the base image identical. Preserve live NFS identities;
  // the filesystem adapter still remounts its discarded transaction caches.
  return e;
}
inline Result SectorJournal::start(const Request &r) {
  auto g = geometry();
  if (!g.generation || r.generation != g.generation)
    return {Error::Changed};
  if (r.operation == Operation::Flush) {
    if (active_)
      return {Error::Unsupported};
    return {parent_.flush(), 0, true};
  }
  if (r.blocks != 1 || r.lba >= dataBlocks_ || r.bufferBytes < 512)
    return {Error::Invalid};
  if (r.operation == Operation::Read) {
    if (!r.readBuffer)
      return {Error::Invalid};
    if (active_)
      for (uint32_t i = count_; i > 0; --i) {
        auto &record = index_[i - 1];
        if (r.lba >= record.lba && r.lba - record.lba < record.blocks) {
          if (record.zero) {
            memset(r.readBuffer, 0, 512);
            return {Error::Ok, 1};
          }
          uint64_t lba;
          uint32_t blocks;
          Error e = readRecord(record.slot, lba, r.readBuffer, blocks);
          return {e, e == Error::Ok ? 1u : 0u};
        }
      }
    Error e = parent_.read(dataStart_ + r.lba, 1, r.readBuffer, 512);
    return {e, e == Error::Ok ? 1u : 0u};
  }
  if (!r.writeBuffer)
    return {Error::Invalid};
  if (!active_) {
    Error e = parent_.write(dataStart_ + r.lba, 1, r.writeBuffer, 512);
    return {e, e == Error::Ok ? 1u : 0u};
  }
  bool zero = true;
  for (unsigned i = 0; i < 512; ++i)
    if (r.writeBuffer[i]) {
      zero = false;
      break;
    }
  uint32_t slot = count_;
  for (uint32_t i = count_; i > 0; --i) {
    auto &prior = index_[i - 1];
    if (r.lba >= prior.lba && r.lba - prior.lba < prior.blocks) {
      if (zero && prior.zero)
        return {Error::Ok, 1, false};
      if (!prior.zero && prior.blocks == 1)
        slot = i - 1;
      break;
    }
  }
  if (slot == count_ && zero && count_ && index_[count_ - 1].zero &&
      index_[count_ - 1].lba + index_[count_ - 1].blocks == r.lba) {
    auto &last = index_[count_ - 1];
    ++last.blocks;
    last.dirty = true;
    return {Error::Ok, 1, false};
  }
  if (slot == MaxSectors)
    return {fail(Error::NoSpace)};
  if (zero) {
    index_[slot].zero = true;
    index_[slot].dirty = true;
  } else {
    Error e = writeRecord(slot, r.lba, r.writeBuffer);
    if (e != Error::Ok)
      return {e};
    index_[slot].zero = false;
    index_[slot].dirty = false;
  }
  index_[slot].lba = r.lba;
  index_[slot].blocks = 1;
  index_[slot].slot = slot;
  if (slot == count_)
    ++count_;
  return {Error::Ok, 1, false};
}
} // namespace storage
} // namespace esp32_nfs
