// SPDX-License-Identifier: MIT
#pragma once
#include "../ESP32NFS.h"
#include "../Names.h"
#include <algorithm>
#include <string.h>
namespace esp32_nfs {
// Caller owns both slots and data buffers; no dynamic allocation. RAM is explicitly volatile.
class RamVfs : public Vfs {
public:
  struct Slot {
    uint8_t *data = nullptr;
    size_t capacity = 0, size = 0;
    uint64_t id = 0, revision = 1;
    bool directory = false;
    char path[PathSize] = {};
  };
  RamVfs(Slot *slots, size_t count) : slots_(slots), count_(count) {}
  uint64_t root() const override {
    return 1;
  }
  bool stableStorage() const override {
    return false;
  }
  uint64_t maxFileSize() const override {
    size_t max = 0;
    for (size_t i = 0; i < count_; ++i)
      max = std::max(max, slots_[i].capacity);
    return max;
  }
  Status getattr(uint64_t id, Attr &a) override {
    a = Attr();
    a.id = id;
    if (id == 1) {
      a.type = 2;
      a.mode = 0777;
      a.ctime.nanos = a.mtime.nanos = rootRevision_ % 1000000000;
      return Ok;
    }
    Slot *s = node(id);
    if (!s)
      return Stale;
    a.type = s->directory ? 2 : 1;
    a.mode = s->directory ? 0777 : 0666;
    a.size = s->directory ? 0 : s->size;
    a.used = a.size;
    a.ctime.nanos = a.mtime.nanos = s->revision % 1000000000;
    return Ok;
  }
  Status lookup(uint64_t d, const char *name, uint64_t &id) override {
    char p[PathSize];
    Status status = child(d, name, p);
    if (status != Ok)
      return status;
    if (!strcmp(p, "/")) {
      id = 1;
      return Ok;
    }
    Slot *s = find(p);
    if (!s)
      return NoEnt;
    id = s->id;
    return Ok;
  }
  Status setattr(uint64_t id, const SetAttr &a) override {
    if (a.atimeHow || a.mtimeHow)
      return NotSupported;
    Slot *s = node(id);
    if (!s)
      return id == 1 ? NotSupported : Stale;
    if ((a.uidSet && a.uid) || (a.gidSet && a.gid) ||
        (a.modeSet && a.mode != (s->directory ? 0777u : 0666u)))
      return NotSupported;
    if (a.sizeSet) {
      if (s->directory)
        return IsDir;
      if (a.size > s->capacity)
        return NoSpace;
      if (a.size > s->size)
        memset(s->data + s->size, 0, size_t(a.size) - s->size);
      s->size = a.size;
    }
    s->revision = ++revision_;
    return Ok;
  }
  Status read(uint64_t id, uint64_t off, uint8_t *p, size_t &n) override {
    Slot *s = node(id);
    if (!s)
      return id == 1 ? IsDir : Stale;
    if (s->directory)
      return IsDir;
    if (off >= s->size) {
      n = 0;
      return Ok;
    }
    n = std::min<uint64_t>(n, s->size - off);
    if (n)
      memcpy(p, s->data + size_t(off), n);
    return Ok;
  }
  Status write(uint64_t id, uint64_t off, const uint8_t *p, size_t &n) override {
    Slot *s = node(id);
    if (!s)
      return id == 1 ? IsDir : Stale;
    if (s->directory)
      return IsDir;
    if (off > s->capacity)
      return NoSpace;
    size_t size = std::min<uint64_t>(n, s->capacity - off);
    if (n && !size)
      return NoSpace;
    if (off > s->size)
      memset(s->data + s->size, 0, size_t(off) - s->size);
    if (size)
      memcpy(s->data + size_t(off), p, size);
    n = size;
    s->size = std::max(s->size, size_t(off) + size);
    s->revision = ++revision_;
    return Ok;
  }
  Status create(uint64_t d, const char *name, bool directory, uint32_t how, uint64_t,
                const SetAttr &a, uint64_t &id) override {
    if (how == 2)
      return NotSupported;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
      return Exist;
    if (a.atimeHow || a.mtimeHow)
      return NotSupported;
    char path[PathSize];
    Status s = child(d, name, path);
    if (s != Ok)
      return s;
    Slot *old = find(path);
    if (old) {
      if (directory || how)
        return Exist;
      if (old->directory)
        return IsDir;
      id = old->id;
      SetAttr initial = a;
      initial.modeSet = initial.uidSet = initial.gidSet = false;
      return setattr(id, initial);
    }
    if (nextId_ == UINT64_MAX)
      return Jukebox;
    for (size_t i = 0; i < count_; ++i)
      if (!slots_[i].id) {
        Slot &slot = slots_[i];
        if (a.sizeSet && a.size > slot.capacity)
          continue;
        if (slot.capacity && !slot.data)
          return Invalid;
        slot.id = nextId_++;
        slot.directory = directory;
        strcpy(slot.path, path);
        slot.size = 0;
        slot.revision = ++revision_;
        id = slot.id;
        changed(d);
        SetAttr initial = a;
        initial.modeSet = initial.uidSet = initial.gidSet = false;
        return setattr(id, initial);
      }
    return NoSpace;
  }
  Status remove(uint64_t d, const char *name, bool directory) override {
    if (!strcmp(name, ".") || !strcmp(name, ".."))
      return Invalid;
    char p[PathSize];
    Status s = child(d, name, p);
    if (s != Ok)
      return s;
    Slot *slot = find(p);
    if (!slot)
      return NoEnt;
    if (directory != slot->directory)
      return directory ? NotDir : IsDir;
    if (directory)
      for (size_t i = 0; i < count_; ++i)
        if (slots_[i].id && below(slots_[i].path, p))
          return NotEmpty;
    slot->id = 0;
    slot->size = 0;
    changed(d);
    return Ok;
  }
  Status rename(uint64_t from, const char *old, uint64_t to, const char *name) override {
    if (!strcmp(old, ".") || !strcmp(old, "..") || !strcmp(name, ".") || !strcmp(name, ".."))
      return Invalid;
    char a[PathSize], b[PathSize];
    Status s = child(from, old, a);
    if (s != Ok)
      return s;
    s = child(to, name, b);
    if (s != Ok)
      return s;
    Slot *source = find(a);
    if (!source)
      return NoEnt;
    if (!strcmp(a, b))
      return Ok;
    if (below(b, a))
      return Invalid;
    Slot *target = find(b);
    if (target) {
      if (source->directory != target->directory)
        return target->directory ? IsDir : NotDir;
      if (target->directory)
        for (size_t i = 0; i < count_; ++i)
          if (slots_[i].id && below(slots_[i].path, b))
            return NotEmpty;
    }
    size_t an = strlen(a), bn = strlen(b);
    for (size_t i = 0; i < count_; ++i)
      if (slots_[i].id && below(slots_[i].path, a) && bn + strlen(slots_[i].path) - an >= PathSize)
        return NameTooLong;
    if (target)
      target->id = 0;
    for (size_t i = 0; i < count_; ++i)
      if (slots_[i].id && (!strcmp(slots_[i].path, a) || below(slots_[i].path, a))) {
        char p[PathSize];
        strcpy(p, b);
        strcat(p, slots_[i].path + an);
        strcpy(slots_[i].path, p);
        slots_[i].revision = ++revision_;
      }
    changed(from);
    changed(to);
    return Ok;
  }
  Status directoryVerifier(uint64_t d, uint64_t &ver) override {
    Attr a;
    Status s = getattr(d, a);
    if (s != Ok)
      return s;
    if (a.type != 2)
      return NotDir;
    ver = d == 1 ? rootRevision_ : node(d)->revision;
    return Ok;
  }
  Status readdir(uint64_t d, uint64_t cookie, uint64_t ver, Entry &e, bool &eof) override {
    uint64_t actual;
    Status s = directoryVerifier(d, actual);
    if (s != Ok)
      return s;
    if (cookie && ver != actual)
      return BadCookie;
    if (cookie > count_)
      return BadCookie;
    const char *path = d == 1 ? "/" : node(d)->path;
    size_t len = strlen(path);
    for (size_t i = size_t(cookie); i < count_; ++i)
      if (slots_[i].id) {
        const char *p = slots_[i].path;
        if (strncmp(p, path, len))
          continue;
        if (d != 1 && p[len] != '/')
          continue;
        const char *tail = p + len + (d == 1 ? 0 : 1);
        if (!*tail || strchr(tail, '/'))
          continue;
        strcpy(e.name, tail);
        e.id = slots_[i].id;
        e.cookie = i + 1;
        getattr(e.id, e.attr);
        eof = false;
        return Ok;
      }
    eof = true;
    return Ok;
  }
  Status sync(uint64_t id) override {
    Attr a;
    return getattr(id, a);
  } // RAM visibility only. Server refuses durable COMMIT.
  Status space(Space &s) override {
    s = Space();
    s.totalFiles = count_ + 1;
    for (size_t i = 0; i < count_; ++i) {
      s.totalBytes += slots_[i].capacity;
      s.freeBytes += slots_[i].id && slots_[i].directory ? 0 : slots_[i].capacity - slots_[i].size;
      s.freeFiles += !slots_[i].id;
    }
    return Ok;
  }

private:
  Slot *slots_;
  size_t count_;
  uint64_t nextId_ = 2, revision_ = 1, rootRevision_ = 1;
  Slot *node(uint64_t id) {
    for (size_t i = 0; i < count_; ++i)
      if (id && slots_[i].id == id)
        return slots_ + i;
    return nullptr;
  }
  Slot *find(const char *p) {
    for (size_t i = 0; i < count_; ++i)
      if (slots_[i].id && !strcmp(slots_[i].path, p))
        return slots_ + i;
    return nullptr;
  }
  static bool below(const char *p, const char *base) {
    size_t n = strlen(base);
    return strlen(p) > n && !strncmp(p, base, n) && p[n] == '/';
  }
  void changed(uint64_t d) {
    if (d == 1)
      rootRevision_ = ++revision_;
    else if (auto *s = node(d))
      s->revision = ++revision_;
  }
  Status child(uint64_t d, const char *name, char *p) {
    Attr a;
    Status s = getattr(d, a);
    if (s != Ok)
      return s;
    if (a.type != 2)
      return NotDir;
    Status valid = name_detail::component(name);
    if (valid != Ok)
      return valid;
    const char *base = d == 1 ? "/" : node(d)->path;
    if (!strcmp(name, ".")) {
      strcpy(p, base);
      return Ok;
    }
    if (!strcmp(name, "..")) {
      strcpy(p, base);
      char *slash = strrchr(p, '/');
      if (slash == p)
        p[1] = 0;
      else
        *slash = 0;
      return Ok;
    }
    size_t len = strlen(base);
    if (len + (len > 1) + strlen(name) >= PathSize)
      return NameTooLong;
    strcpy(p, base);
    if (len > 1)
      strcat(p, "/");
    strcat(p, name);
    return Ok;
  }
};
} // namespace esp32_nfs
