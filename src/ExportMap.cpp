// SPDX-License-Identifier: MIT
#include "ExportMap.h"
#include "ESP32NFS.h"
#include <string.h>
namespace esp32_nfs {
bool ExportMap::validPath(const char *p) {
  if (!p || p[0] != '/' || strlen(p) >= PathCapacity)
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
  return true;
}
bool ExportMap::add(const char *p, Vfs &v, bool readOnly, Lease *lease) {
  if (!validPath(p) || find(p) || nextToken_ == UINT64_MAX)
    return false;
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
Status ExportMap::release(Mapping &e, Lease *fallback) {
  Lease *bus = e.lease ? e.lease : fallback;
  if (bus && !bus->acquire())
    return Jukebox;
  Status s = e.vfs->endSession();
  if (bus)
    bus->release();
  e.mounted = false;
  return s;
}
bool ExportMap::remove(const char *p, Lease *fallback) {
  auto *e = find(p);
  if (!e || release(*e, fallback) != Ok)
    return false;
  *e = Mapping();
  ++revision_;
  return true;
}
Status ExportMap::clear(Lease *fallback) {
  Status result = Ok;
  for (auto &e : entries_)
    if (e.vfs) {
      Status s = release(e, fallback);
      if (s == Ok) {
        e = Mapping();
        ++revision_;
      } else
        result = s;
    }
  return result;
}
Status ExportMap::endSessions(Lease *fallback) {
  Status result = Ok;
  for (auto &e : entries_)
    if (e.vfs) {
      Status s = release(e, fallback);
      if (s != Ok)
        result = s;
    }
  return result;
}
Status ExportMap::refreshGenerations(Lease *fallback) {
  for (auto &e : entries_)
    if (e.vfs && e.generation != e.vfs->generation()) {
      Status s = release(e, fallback);
      if (s != Ok)
        return s;
      e.generation = e.vfs->generation();
      e.token = nextToken_ == UINT64_MAX ? 0 : nextToken_++;
      ++revision_;
    }
  return Ok;
}
} // namespace esp32_nfs
