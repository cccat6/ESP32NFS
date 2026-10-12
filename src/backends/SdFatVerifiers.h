// SPDX-License-Identifier: MIT
#pragma once
#include "../ESP32NFS.h"
#include "SdFatVolume.h"
namespace esp32_nfs {
// Transaction-protected EXCLUSIVE CREATE verifiers. No permissions, owners or timestamp database.
class SdFatVerifiers {
public:
  static constexpr uint32_t MaxRecords = 64;
  explicit SdFatVerifiers(SdFatVolume &volume) : volume_(volume) {}
  ~SdFatVerifiers() {
    delete[] index_;
  }
  SdFatVerifiers(const SdFatVerifiers &) = delete;
  SdFatVerifiers &operator=(const SdFatVerifiers &) = delete;
  struct Record {
    char path[PathSize] = {};
    uint64_t verifier = 0;
    uint32_t flags = 0;
  };
  bool load();
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
  bool loaded_ = false;
  uint32_t count_ = 0;
  struct Index {
    uint64_t key = 0;
    uint32_t slot = 0;
  };
  Index *index_ = nullptr;
  bool readSlot(uint32_t, Record &);
  bool writeSlot(uint32_t, const Record &);
  bool header();
  static uint64_t key(const char *);
  static bool equal(const char *, const char *);
  static bool below(const char *, const char *);
};
} // namespace esp32_nfs
