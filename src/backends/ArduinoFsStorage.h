// SPDX-License-Identifier: MIT
#pragma once
#include "PathVfs.h"
#include <FS.h>
#include <string.h>
namespace esp32_nfs {
// Read-only Arduino FS export. One read handle and one directory cursor are cached.
// External writers MUST call PathVfs::invalidateAll before access resumes. This is a
// serialized adapter; callers sharing it must share the same storage lease.
class ArduinoFsStorage : public PathStorage {
public:
  explicit ArduinoFsStorage(fs::FS &fs) : fs_(fs) {}
  ~ArduinoFsStorage() override {
    invalidateCaches();
  }
  bool writable() const override {
    return false;
  }
  uint64_t maxFileSize() const override {
    return UINT32_MAX;
  }
  void invalidateCaches() override {
    file_.close();
    directory_.close();
    filePath_[0] = directoryPath_[0] = 0;
    nextIndex_ = 0;
    lastValid_ = false;
  }
  Status stat(const char *p, Attr &a) override {
    if (directory_ && !strcmp(p, directoryPath_)) {
      a = directoryAttr_;
      return Ok;
    }
    if (file_ && !strcmp(p, filePath_)) {
      a = fileAttr_;
      return Ok;
    }
    if (strlen(p) >= PathSize)
      return NameTooLong;
    fs::File candidate = fs_.open(p, "r");
    if (!candidate)
      return NoEnt;
    // Parent-directory attribute checks must not evict the current read handle.
    attributes(candidate, a);
    if (candidate.isDirectory()) {
      directory_.close();
      directory_ = candidate;
      strcpy(directoryPath_, p);
      directoryAttr_ = a;
      nextIndex_ = 0;
      lastValid_ = false;
    } else {
      file_.close();
      file_ = candidate;
      strcpy(filePath_, p);
      fileAttr_ = a;
    }
    return Ok;
  }
  Status read(const char *p, uint64_t off, uint8_t *data, size_t &n) override {
    Status s = openRead(p);
    if (s != Ok)
      return s;
    if (file_.isDirectory())
      return IsDir;
    if (off >= file_.size()) {
      n = 0;
      return Ok;
    }
    if (off > UINT32_MAX)
      return FBig;
    if (file_.position() != off && !file_.seek(uint32_t(off)))
      return Io;
    size_t got = file_.read(data, n);
    if (got > n)
      return Io;
    n = got;
    return Ok;
  }
  Status entry(const char *p, uint64_t index, char *name, size_t cap, bool &eof) override {
    Attr unused;
    return entryInfo(p, index, name, cap, unused, eof);
  }
  Status entryInfo(const char *p, uint64_t index, char *name, size_t cap, Attr &a,
                   bool &eof) override {
    if (strlen(p) >= PathSize)
      return NameTooLong;
    if (!directory_ || strcmp(p, directoryPath_)) {
      directory_.close();
      lastValid_ = false;
      nextIndex_ = 0;
      directory_ = fs_.open(p, "r");
      if (!directory_)
        return NoEnt;
      if (!directory_.isDirectory()) {
        directory_.close();
        return NotDir;
      }
      strcpy(directoryPath_, p);
      attributes(directory_, directoryAttr_);
    }
    // READDIR may inspect an entry that did not fit the last reply. Replay it without
    // rewinding, and keep no client-specific cursor assumptions beyond the supplied index.
    if (lastValid_ && index == lastIndex_)
      return lastEntry(name, cap, a, eof);
    if (index < nextIndex_) {
      directory_.rewindDirectory();
      nextIndex_ = 0;
      lastValid_ = false;
    }
    unsigned budget = scanBudget_;
    while (nextIndex_ <= index) {
      if (!budget--)
        return Jukebox;
      fs::File f = directory_.openNextFile();
      if (!f) {
        eof = true;
        return Ok;
      }
      if (nextIndex_++ != index)
        continue;
      const char *base = strrchr(f.name(), '/');
      base = base ? base + 1 : f.name();
      if (strlen(base) >= sizeof(lastName_))
        return NameTooLong;
      strcpy(lastName_, base);
      attributes(f, lastAttr_);
      lastIndex_ = index;
      lastValid_ = true;
      return lastEntry(name, cap, a, eof);
    }
    return Io;
  }
  Status sync(const char *) override {
    return Ok;
  }
  Status space(Space &s) override {
    s = Space();
    return Ok;
  }
  Status setattr(const char *, const SetAttr &) override {
    return ReadOnly;
  }
  Status write(const char *, uint64_t, const uint8_t *, size_t &) override {
    return ReadOnly;
  }
  Status create(const char *, bool) override {
    return ReadOnly;
  }
  Status remove(const char *, bool) override {
    return ReadOnly;
  }
  Status rename(const char *, const char *) override {
    return ReadOnly;
  }
  void setDirectoryScanBudget(uint32_t count) {
    scanBudget_ = count;
  }

private:
  fs::FS &fs_;
  fs::File file_, directory_;
  char filePath_[PathSize] = {}, directoryPath_[PathSize] = {}, lastName_[PathSize] = {};
  Attr lastAttr_, fileAttr_, directoryAttr_;
  uint64_t nextIndex_ = 0, lastIndex_ = 0;
  uint32_t scanBudget_ = 64;
  bool lastValid_ = false;
  static void attributes(fs::File &f, Attr &a) {
    a = Attr();
    a.type = f.isDirectory() ? 2 : 1;
    a.mode = a.type == 2 ? 0555 : 0444;
    a.size = a.type == 2 ? 0 : f.size();
    a.used = a.size;
    a.mtime.seconds = f.getLastWrite();
    a.ctime = a.atime = a.mtime;
  }
  Status openRead(const char *p) {
    if (strlen(p) >= PathSize)
      return NameTooLong;
    if (file_ && !strcmp(p, filePath_))
      return Ok;
    file_.close();
    filePath_[0] = 0;
    file_ = fs_.open(p, "r");
    if (!file_)
      return NoEnt;
    strcpy(filePath_, p);
    attributes(file_, fileAttr_);
    return Ok;
  }
  Status lastEntry(char *name, size_t cap, Attr &a, bool &eof) {
    if (strlen(lastName_) >= cap)
      return NameTooLong;
    strcpy(name, lastName_);
    a = lastAttr_;
    eof = false;
    return Ok;
  }
};
} // namespace esp32_nfs
