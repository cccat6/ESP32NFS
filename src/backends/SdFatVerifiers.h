// SPDX-License-Identifier: MIT
#pragma once
#include "../ESP32NFS.h"
#include "SdFatVolume.h"
namespace esp32_nfs {
// Transaction-protected EXCLUSIVE CREATE verifiers. No permissions, owners or timestamp database.
class SdFatVerifiers {
public:
  explicit SdFatVerifiers(SdFatVolume &volume, Memory &memory = defaultMemory())
      : volume_(volume), memory_(memory) {}
  ~SdFatVerifiers() {
    release();
  }
  SdFatVerifiers(const SdFatVerifiers &) = delete;
  SdFatVerifiers &operator=(const SdFatVerifiers &) = delete;
  struct Record {
    char path[PathSize] = {};
    uint64_t verifier = 0;
    uint32_t flags = 0;
  };
  Status load();
  void release();
  void invalidate() {
    loaded_ = false;
  }
  Status get(const char *, Record &);
  Status set(Record &);
  Status erase(const char *);
  Status moveTree(const char *, const char *);
  Status initialize();
  static constexpr uint32_t HasVerifier = 1;
  static bool reserved(const char *);

private:
  SdFatVolume &volume_;
  Memory &memory_;
  bool loaded_ = false;
  uint32_t count_ = 0, capacity_ = 0;
  struct Page {
    Page *next;
    uint64_t keys[64];
  };
  Page *pages_ = nullptr, *tail_ = nullptr, *cursor_ = nullptr;
  uint32_t cursorFirst_ = 0;
  uint64_t &slotKey(uint32_t slot);
  Status reserve(uint32_t count);
  bool readSlot(uint32_t, Record &);
  bool writeSlot(uint32_t, const Record &);
  bool header();
  static uint64_t key(const char *);
  static bool equal(const char *, const char *);
  static bool below(const char *, const char *);
};
} // namespace esp32_nfs

#include "SdFatVerifiers.ipp"
