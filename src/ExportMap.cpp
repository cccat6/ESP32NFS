// SPDX-License-Identifier: MIT
#include "ExportMap.h"
#include "ESP32NFS.h"
#include <string.h>
namespace esp32_nfs {
bool ExportMap::add(const char *p, Vfs &v, bool readOnly, Lease *lease) {
  if (!p || p[0] != '/' || strlen(p) >= PathCapacity || find(p) || nextToken_ == UINT64_MAX)
    return false;
  if (strlen(p) > 1 && p[strlen(p) - 1] == '/')
    return false;
  for (const char *c = p + 1; *c;) {
    const char *e = strchr(c, '/');
    size_t n = e ? size_t(e - c) : strlen(c);
    if (!n || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.'))
      return false;
    if (!e)
      break;
    c = e + 1;
  }
  for (auto &e : entries_)
    if (e.vfs == &v)
      return false;
  for (auto &e : entries_)
    if (!e.vfs) {
      e = Mapping();
      e.vfs = &v;
      e.lease = lease;
      e.readOnly = readOnly;
      e.token = nextToken_++;
      e.generation = v.generation();
      strcpy(e.path, p);
      ++revision_;
      return true;
    }
  return false;
}
ExportMap::Mapping *ExportMap::find(const char *p) {
  if (!p)
    return nullptr;
  for (auto &e : entries_)
    if (e.vfs && !strcmp(e.path, p))
      return &e;
  return nullptr;
}
ExportMap::Mapping *ExportMap::find(uint64_t t) {
  for (auto &e : entries_)
    if (e.vfs && e.token == t)
      return &e;
  return nullptr;
}
ExportMap::Mapping *ExportMap::at(size_t i) {
  return i < ESP32_NFS_EXPORT_COUNT && entries_[i].vfs ? entries_ + i : nullptr;
}
bool ExportMap::remove(const char *p) {
  auto *e = find(p);
  if (!e)
    return false;
  e->vfs->endSession();
  *e = Mapping();
  ++revision_;
  return true;
}
void ExportMap::clear() {
  for (auto &e : entries_)
    if (e.vfs) {
      e.vfs->endSession();
      e = Mapping();
    }
  ++revision_;
}
void ExportMap::endSessions() {
  for (auto &e : entries_)
    if (e.vfs) {
      e.vfs->endSession();
      e.mounted = false;
    }
}
void ExportMap::refreshGenerations() {
  for (auto &e : entries_)
    if (e.vfs && e.generation != e.vfs->generation()) {
      e.vfs->endSession();
      e.generation = e.vfs->generation();
      e.token = nextToken_ == UINT64_MAX ? 0 : nextToken_++;
      e.mounted = false;
      ++revision_;
    }
}
} // namespace esp32_nfs
