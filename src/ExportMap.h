// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifndef ESP32_NFS_EXPORT_COUNT
#define ESP32_NFS_EXPORT_COUNT 4
#endif
namespace esp32_nfs {
class Vfs;
class Lease;
enum Status : uint32_t;
// Maps already-mounted filesystem instances, not raw device addresses or filesystem formats.
class ExportMap {
public:
  static constexpr size_t PathCapacity = 64;
  struct Mapping {
    Vfs *vfs = nullptr;
    Lease *lease = nullptr;
    uint64_t token = 0, mountPeer = 0, generation = 0;
    bool readOnly = false, mounted = false;
    char path[PathCapacity] = {};
  };
  static bool validPath(const char *);
  bool add(const char *exportPath, Vfs &, bool readOnly = false, Lease *lease = nullptr);
  bool remove(const char *exportPath, Lease *fallback = nullptr);
  Mapping *find(const char *exportPath);
  Mapping *find(uint64_t token);
  Mapping *at(size_t index);
  Status clear(Lease *fallback = nullptr);
  Status endSessions(Lease *fallback = nullptr);
  Status refreshGenerations(Lease *fallback = nullptr);
  uint64_t revision() const {
    return revision_;
  }

private:
  Status release(Mapping &, Lease *fallback);
  Mapping entries_[ESP32_NFS_EXPORT_COUNT];
  uint64_t nextToken_ = 1, revision_ = 1;
};
} // namespace esp32_nfs
