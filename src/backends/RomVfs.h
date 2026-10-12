// SPDX-License-Identifier: MIT
#pragma once
#include "../ESP32NFS.h"
#include "../Names.h"
#include <algorithm>
#include <string.h>
namespace esp32_nfs {
struct RomEntry {
  const char *path;
  const uint8_t *data;
  uint64_t size;
  bool directory;
  constexpr RomEntry(const char *p, const uint8_t *d, uint64_t n, bool dir = false)
      : path(p), data(d), size(n), directory(dir) {}
};
// Immutable manifest and data can reside in ROM, memory-mapped flash, or caller-owned RAM.
// No allocation, path registry, filesystem formatting, or flash writes occur.
class RomVfs : public Vfs {
public:
  using Reader = bool (*)(const RomEntry &, uint64_t, uint8_t *, size_t);
  RomVfs(const RomEntry *entries, size_t count, Reader reader = nullptr)
      : entries_(entries), count_(count), reader_(reader) {}
  uint64_t root() const override {
    return 1;
  }
  bool writable() const override {
    return false;
  }
  uint64_t maxFileSize() const override {
    return UINT64_MAX;
  }
  Status getattr(uint64_t id, Attr &a) override {
    if (!id || id > count_ + 1)
      return Stale;
    a = Attr();
    a.id = id;
    if (id == 1) {
      a.type = 2;
      a.mode = 0555;
      return Ok;
    }
    auto &e = entries_[id - 2];
    if (!e.path || e.path[0] != '/')
      return Invalid;
    if (strlen(e.path) >= PathSize)
      return NameTooLong;
    a.type = e.directory ? 2 : 1;
    a.mode = e.directory ? 0555 : 0444;
    a.size = e.size;
    a.used = e.size;
    return Ok;
  }
  Status lookup(uint64_t dir, const char *name, uint64_t &id) override {
    Attr a;
    Status s = getattr(dir, a);
    if (s != Ok)
      return s;
    if (a.type != 2)
      return NotDir;
    Status valid = name_detail::component(name);
    if (valid != Ok)
      return valid;
    if (!strcmp(name, ".")) {
      id = dir;
      return Ok;
    }
    char p[PathSize];
    const char *base = dir == 1 ? "/" : entries_[dir - 2].path;
    if (!strcmp(name, "..")) {
      strcpy(p, base);
      char *slash = strrchr(p, '/');
      if (slash == p)
        p[1] = 0;
      else
        *slash = 0;
      return find(p, id);
    }
    size_t len = strlen(base), n = strlen(name);
    if (len + (len > 1) + n >= sizeof(p))
      return NameTooLong;
    strcpy(p, base);
    if (len > 1)
      strcat(p, "/");
    strcat(p, name);
    return find(p, id);
  }
  Status read(uint64_t id, uint64_t off, uint8_t *p, size_t &n) override {
    Attr a;
    Status s = getattr(id, a);
    if (s != Ok)
      return s;
    if (a.type == 2)
      return IsDir;
    const RomEntry &e = entries_[id - 2];
    if (off >= e.size) {
      n = 0;
      return Ok;
    }
    n = std::min<uint64_t>(n, e.size - off);
    if (reader_)
      return reader_(e, off, p, n) ? Ok : Io;
    if (off > SIZE_MAX || n > SIZE_MAX - size_t(off) || (!e.data && n))
      return Io;
    if (n)
      memcpy(p, e.data + size_t(off), n);
    return Ok;
  }
  Status directoryVerifier(uint64_t dir, uint64_t &ver) override {
    Attr a;
    Status s = getattr(dir, a);
    if (s == Ok && a.type != 2)
      return NotDir;
    ver = 1;
    return s;
  }
  Status readdir(uint64_t dir, uint64_t cookie, uint64_t ver, Entry &e, bool &eof) override {
    uint64_t actual;
    Status s = directoryVerifier(dir, actual);
    if (s != Ok)
      return s;
    if (cookie && ver != actual)
      return BadCookie;
    const char *base = dir == 1 ? "/" : entries_[dir - 2].path;
    size_t len = strlen(base);
    for (size_t i = size_t(std::min<uint64_t>(cookie, count_)); i < count_; ++i) {
      const char *p = entries_[i].path;
      if (strncmp(p, base, len) || p[len ? len - 1 : 0] == 0)
        continue;
      const char *tail = dir == 1 ? p + 1 : p + len + 1;
      if (dir != 1 && p[len] != '/')
        continue;
      if (!*tail || strchr(tail, '/'))
        continue;
      if (strlen(tail) >= sizeof(e.name))
        return NameTooLong;
      strcpy(e.name, tail);
      e.id = i + 2;
      e.cookie = i + 1;
      getattr(e.id, e.attr);
      eof = false;
      return Ok;
    }
    eof = true;
    return cookie > count_ ? BadCookie : Ok;
  }
  Status sync(uint64_t id) override {
    Attr a;
    return getattr(id, a);
  }
  Status space(Space &s) override {
    s = Space();
    for (size_t i = 0; i < count_; ++i) {
      if (entries_[i].size > UINT64_MAX - s.totalBytes)
        return FBig;
      s.totalBytes += entries_[i].size;
    }
    s.totalFiles = count_ + 1;
    return Ok;
  }
  Status setattr(uint64_t, const SetAttr &) override {
    return ReadOnly;
  }
  Status write(uint64_t, uint64_t, const uint8_t *, size_t &) override {
    return ReadOnly;
  }
  Status create(uint64_t, const char *, bool, uint32_t, uint64_t, const SetAttr &,
                uint64_t &) override {
    return ReadOnly;
  }
  Status remove(uint64_t, const char *, bool) override {
    return ReadOnly;
  }
  Status rename(uint64_t, const char *, uint64_t, const char *) override {
    return ReadOnly;
  }

private:
  const RomEntry *entries_;
  size_t count_;
  Reader reader_;
  Status find(const char *p, uint64_t &id) {
    if (!strcmp(p, "/")) {
      id = 1;
      return Ok;
    }
    for (size_t i = 0; i < count_; ++i)
      if (!strcmp(entries_[i].path, p)) {
        id = i + 2;
        return Ok;
      }
    return NoEnt;
  }
};
} // namespace esp32_nfs
