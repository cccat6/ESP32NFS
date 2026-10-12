// SPDX-License-Identifier: MIT
#pragma once
#include "../storage/SectorJournal.h"
#include "PathVfs.h"
#include "SdFatVerifiers.h"
#include <SdFat.h>
#include <algorithm>
#include <common/upcase.h>
#include <string.h>
#include <time.h>
#if SD_FAT_VERSION != 20300
#error "ESP32NFS SdFat adapter is validated against SdFat 2.3.0; pin that version."
#endif
#if SDFAT_FILE_TYPE != 3
#error "Use SDFAT_FILE_TYPE=3 for one FAT32/exFAT stack."
#endif
#if !USE_LONG_FILE_NAMES || !USE_UTF8_LONG_NAMES
#error                                                                                             \
    "Build sketch and SdFat with USE_LONG_FILE_NAMES=1 and USE_UTF8_LONG_NAMES=1. See docs/storage.md."
#endif
namespace esp32_nfs {
class SdFatStorage : public PathStorage {
public:
  explicit SdFatStorage(FsVolume &sd) : sd_(sd), verifiers_(sd) {}
  using Remount = bool (*)(void *);
  void attachTransactions(storage::SectorJournal &journal, Remount remount, void *context) {
    journal_ = &journal;
    remount_ = remount;
    context_ = context;
    verifiers_.invalidate();
  }
  bool recoverTransactions() {
    invalidateCaches();
    if (!journal_ || !remount_)
      return false;
    if (journal_->recover() != storage::Error::Ok)
      return false;
    verifiers_.invalidate();
    poison_ = !remount_(context_);
    return !poison_;
  }

  bool sameName(const char *a, const char *b) const override {
    const char *ae = a + strlen(a), *be = b + strlen(b);
    while (a < ae && b < be) {
      uint32_t x, y;
      if (!name_detail::next(a, ae, x) || !name_detail::next(b, be, y))
        return false;
      if (x <= 0xffff)
        x = toUpcase(uint16_t(x));
      if (y <= 0xffff)
        y = toUpcase(uint16_t(y));
      if (x != y)
        return false;
    }
    return a == ae && b == be;
  }
  Status validateName(const char *p) const override {
    return name_detail::component(p, true);
  }
  Status resolve(const char *p, char *canonical, size_t cap, Attr &a) override {
    Status s = canonicalize(p, canonical, cap);
    if (s == Ok)
      a = readAttr_;
    return s == Access ? NoEnt : s;
  }
  Status setattr(const char *p, const SetAttr &a) override {
    if (SdFatVerifiers::reserved(p))
      return Access;
    // Ownership is a fixed export mapping; reject unsupported changes before writing.
    if ((a.uidSet && a.uid) || (a.gidSet && a.gid) || a.atimeHow || a.mtimeHow)
      return NotSupported;
    Attr old;
    char canonical[PathSize];
    Status s = resolve(p, canonical, sizeof(canonical), old);
    if (s != Ok)
      return s;
    SetAttr update = a;
    if (update.sizeSet && update.size == old.size)
      update.sizeSet = false;
    if (update.modeSet && bool(update.mode & 0222) == bool(old.mode & 0222))
      update.modeSet = false;
    if (!update.sizeSet && !update.modeSet)
      return Ok;
    return transact([&]() { return setattrImpl(canonical, update); });
  }
  Status write(const char *p, uint64_t off, const uint8_t *data, size_t &n) override {
    char canonical[PathSize];
    Status s = canonicalize(p, canonical, sizeof(canonical));
    if (s != Ok)
      return s;
    return transact([&]() { return writeImpl(canonical, off, data, n); });
  }
  Status create(const char *p, bool dir) override {
    char canonical[PathSize];
    Status s = canonicalize(p, canonical, sizeof(canonical), true);
    if (s != Ok)
      return s;
    return transact([&]() { return createImpl(canonical, dir); });
  }
  Status remove(const char *p, bool dir) override {
    char canonical[PathSize];
    Status resolved = canonicalize(p, canonical, sizeof(canonical));
    if (resolved != Ok)
      return resolved;
    return transact([&]() {
      Status s = removeImpl(canonical, dir);
      if (s == Ok && journal_)
        s = verifiers_.erase(canonical);
      return s;
    });
  }
  Status rename(const char *a, const char *b) override {
    char from[PathSize], to[PathSize];
    Status s = canonicalize(a, from, sizeof(from));
    if (s != Ok)
      return s;
    uint32_t sourceType = readAttr_.type;
    Status t = canonicalize(b, to, sizeof(to));
    if (t != Ok && t != NoEnt)
      return t;
    uint32_t targetType = readAttr_.type;
    bool same = t == Ok && !strcmp(from, to);
    if (t == NoEnt || same) {
      s = canonicalize(b, to, sizeof(to), true, true);
      if (s != Ok)
        return s;
    }
    if (!strcmp(from, to) || (same && !sameName(from, to)))
      return Ok;
    if (name_detail::below(to, from))
      return Invalid;
    return transact([&]() {
      if (!journal_)
        return renameImpl(from, to);
      if (same) {
        Status moved = verifiers_.moveTree(from, to);
        if (moved != Ok)
          return moved;
        if (!sd_.exists("/.esp32-nfs") && !sd_.mkdir("/.esp32-nfs", false))
          return Io;
        if (sd_.exists("/.esp32-nfs/case.tmp"))
          return Io;
        return sd_.rename(from, "/.esp32-nfs/case.tmp") && sd_.rename("/.esp32-nfs/case.tmp", to)
                   ? Ok
                   : Io;
      }
      if (t == Ok && sourceType != targetType)
        return targetType == 2 ? IsDir : NotDir;
      Status moved = verifiers_.moveTree(from, to);
      if (moved != Ok)
        return moved;
      if (t == Ok) {
        Status removed = removeImpl(to, targetType == 2);
        if (removed != Ok)
          return removed;
      }
      return sd_.rename(from, to) ? Ok : Io;
    });
  }
  Status createExclusive(const char *input, uint64_t verifier) override {
    char canonical[PathSize];
    Status resolved = canonicalize(input, canonical, sizeof(canonical), true);
    if (resolved != Ok)
      return resolved;
    const char *p = canonical;
    if (!journal_)
      return NotSupported;
    return transact([&]() {
      SdFatVerifiers::Record r;
      if (sd_.exists(p)) {
        Status s = verifiers_.get(p, r);
        return s == Ok && (r.flags & SdFatVerifiers::HasVerifier) && r.verifier == verifier ? Ok
               : s == Io                                                                    ? Io
                                                                                            : Exist;
      }
      Status s = createImpl(p, false);
      if (s != Ok)
        return s;
      strcpy(r.path, p);
      r.verifier = verifier;
      r.flags |= SdFatVerifiers::HasVerifier;
      s = verifiers_.set(r);
      return s;
    });
  }
  uint64_t generation() const override {
    return poison_ ? 0 : journal_ ? journal_->geometry().generation : 1;
  }
  bool caseInsensitive() const override {
    return true;
  }
  uint64_t maxFileSize() const override {
    return sd_.fatType() == 64 ? uint64_t(INT64_MAX) : uint64_t(UINT32_MAX);
  }
  Status stat(const char *p, Attr &a) override {
    if (poison_)
      return Io;
    if (readFile_ && p && !strcmp(readPath_, p)) {
      a = readAttr_;
      return Ok;
    }
    char canonical[PathSize];
    return resolve(p, canonical, sizeof(canonical), a);
  }

private:
  Status setattrImpl(const char *p, const SetAttr &a) {
    // FAT has a read-only bit, not arbitrary Unix permissions or ownership.
    if ((a.uidSet && a.uid) || (a.gidSet && a.gid) || a.atimeHow || a.mtimeHow)
      return NotSupported;
    if (a.sizeSet) {
      FsFile f = sd_.open(p, O_RDWR);
      CloseFile closeFile{f};
      if (!f)
        return Access;
      if (f.isDir())
        return IsDir;
      if (a.size > maxFileSize())
        return FBig;
      // SdFat truncate only shrinks. Extend explicitly with zeroes, one bounded sector at a time.
      // Large extension is rejected to avoid unbounded storage work within one cooperative poll.
      uint64_t old = f.fileSize();
      if (a.size > old) {
        if (a.size - old > BlockSize)
          return NotSupported;
        uint8_t zero[512] = {};
        if (!f.seekSet(old))
          return Io;
        while (old < a.size) {
          size_t n = std::min<uint64_t>(sizeof(zero), a.size - old);
          if (f.write(zero, n) != n)
            return Io;
          old += n;
        }
      } else if (!f.truncate(a.size))
        return Io;
      if (!f.sync())
        return Io;
    }
    if (a.modeSet) {
      int bits = sd_.attrib(p);
      if (bits < 0)
        return Io;
      int oldBits = bits;
      if (a.mode & 0222)
        bits &= ~FS_ATTRIB_READ_ONLY;
      else
        bits |= FS_ATTRIB_READ_ONLY;
      if (bits != oldBits && !sd_.attrib(p, bits))
        return Io;
    }
    return Ok;
  }

public:
  Status read(const char *p, uint64_t off, uint8_t *data, size_t &n) override {
    if (poison_)
      return Io;
    if (SdFatVerifiers::reserved(p))
      return Access;
    Attr a;
    Status status = stat(p, a);
    if (status != Ok)
      return status;
    if (a.type != 1)
      return IsDir;
    FsFile &f = readFile_;
    if (off >= f.fileSize()) {
      n = 0;
      return Ok;
    }
    if (f.curPosition() != off && !f.seekSet(off))
      return Io;
    int got = f.read(data, n);
    if (got < 0)
      return Io;
    n = got;
    return Ok;
  }

private:
  Status writeImpl(const char *p, uint64_t off, const uint8_t *data, size_t &n) {
    FsFile f = sd_.open(p, O_RDWR);
    CloseFile closeFile{f};
    if (!f)
      return Access;
    uint64_t old = f.fileSize();
    if (!n)
      return f.sync() ? Ok : Io;
    // FAT/exFAT allocation/zero fill is not sparse. Bound hole extension to one block.
    if (off > old) {
      if (off - old > BlockSize)
        return NotSupported;
      uint8_t zero[512] = {};
      if (!f.seekSet(old))
        return Io;
      while (old < off) {
        size_t part = std::min<uint64_t>(sizeof(zero), off - old);
        if (f.write(zero, part) != part)
          return Io;
        old += part;
      }
    }
    if (!f.seekSet(off))
      return Io;
    size_t got = f.write(data, n);
    n = got;
    if (!got)
      return NoSpace;
    return f.sync() ? Ok : Io;
  }
  Status createImpl(const char *p, bool directory) {
    if (SdFatVerifiers::reserved(p))
      return Access;
    if (sd_.exists(p))
      return Exist;
    if (directory)
      return sd_.mkdir(p, false) ? Ok : Io;
    FsFile f = sd_.open(p, O_RDWR | O_CREAT | O_EXCL);
    CloseFile closeFile{f};
    return f && f.sync() ? Ok : Io;
  }
  Status removeImpl(const char *p, bool directory) {
    if (SdFatVerifiers::reserved(p))
      return Access;
    Attr a;
    Status s = stat(p, a);
    if (s != Ok)
      return s;
    if (directory != (a.type == 2))
      return directory ? NotDir : IsDir;
    if (directory) {
      bool eof = false;
      char name[PathSize];
      s = entry(p, 0, name, sizeof(name), eof);
      if (s != Ok)
        return s;
      if (!eof)
        return NotEmpty;
    }
    return (directory ? sd_.rmdir(p) : sd_.remove(p)) ? Ok : Io;
  }
  Status renameImpl(const char *a, const char *b) {
    if (!sd_.exists(a))
      return NoEnt;
    // SdFat explicitly forbids destination replacement. Never unlink the destination first.
    if (sd_.exists(b))
      return NotSupported;
    return sd_.rename(a, b) ? Ok : Io;
  }

public:
  void invalidateCaches() override {
    readFile_.close();
    readPath_[0] = 0;
    directory_.close();
    directoryPath_[0] = 0;
    nextIndex_ = 0;
    lastValid_ = false;
  }
  Status entry(const char *input, uint64_t index, char *name, size_t cap, bool &eof) override {
    char canonical[PathSize];
    Status resolved = canonicalize(input, canonical, sizeof(canonical));
    if (resolved != Ok)
      return resolved;
    const char *p = canonical;
    if (!directory_ || strcmp(p, directoryPath_)) {
      invalidateCaches();
      directory_ = sd_.open(p, O_RDONLY);
      if (!directory_)
        return NoEnt;
      if (!directory_.isDir()) {
        invalidateCaches();
        return NotDir;
      }
      strcpy(directoryPath_, p);
    }
    if (lastValid_ && index == lastIndex_)
      return copyEntry(name, cap, eof);
    if (index < nextIndex_) {
      directory_.rewind();
      nextIndex_ = 0;
      lastValid_ = false;
    }
    unsigned budget = scanBudget_;
    while (nextIndex_ <= index && budget--) {
      FsFile f;
      CloseFile close{f};
      if (!f.openNext(&directory_, O_RDONLY)) {
        eof = true;
        return directory_.getError() ? Io : Ok;
      }
      if (!f.isFileOrSubDir())
        continue;
      char found[PathSize];
      size_t n = f.getName(found, sizeof(found));
      if (!n || n >= sizeof(found) - 1)
        return NameTooLong;
      if (!strcmp(p, "/") && SdFatVerifiers::reserved(found))
        continue;
      Status valid = validateName(found);
      if (valid != Ok)
        return valid;
      if (nextIndex_++ != index)
        continue;
      strcpy(lastName_, found);
      lastIndex_ = index;
      lastValid_ = true;
      lastAttr_ = Attr();
      lastAttr_.type = f.isDir() ? 2 : 1;
      lastAttr_.mode = f.isDir() ? 0777 : 0666;
      if (f.attrib() & FS_ATTRIB_READ_ONLY)
        lastAttr_.mode &= ~0222;
      lastAttr_.size = lastAttr_.used = f.isDir() ? 0 : f.fileSize();
      uint16_t date = 0, t = 0;
      if (f.getModifyDateTime(&date, &t))
        lastAttr_.atime.seconds = lastAttr_.ctime.seconds = lastAttr_.mtime.seconds =
            dosTime(date, t);
      return copyEntry(name, cap, eof);
    }
    return Jukebox;
  }
  Status entryInfo(const char *p, uint64_t index, char *name, size_t cap, Attr &a,
                   bool &eof) override {
    Status s = entry(p, index, name, cap, eof);
    if (s == Ok && !eof)
      a = lastAttr_;
    return s;
  }
  Status sync(const char *p) override {
    char canonical[PathSize];
    Status s = canonicalize(p, canonical, sizeof(canonical));
    return s == Ok ? (readFile_.sync() ? Ok : Io) : s;
  }
  Status space(Space &s) override {
    s = cachedSpace_;
    return spaceValid_ ? Ok : Jukebox;
  }
  // Explicit integration operation under a bus lease: FAT free-cluster scans may have long latency.
  bool refreshSpace() {
    int32_t free = sd_.freeClusterCount();
    if (free < 0)
      return false;
    cachedSpace_.totalBytes = uint64_t(sd_.clusterCount()) * sd_.bytesPerCluster();
    cachedSpace_.freeBytes = uint64_t(free) * sd_.bytesPerCluster();
    cachedSpace_.totalFiles = ESP32_NFS_HANDLE_COUNT;
    cachedSpace_.freeFiles = 0;
    spaceValid_ = true;
    return true;
  }
  void setDirectoryScanBudget(uint32_t count) {
    scanBudget_ = count;
  }

private:
  // Indexed reopen recovers LFN state omitted when a FAT object is opened by its SFN.
  Status canonicalize(const char *input, char *out, size_t cap, bool allowMissing = false,
                      bool preserveLeaf = false) {
    if (poison_)
      return Io;
    Status valid = name_detail::path(input, true);
    if (valid != Ok)
      return valid;
    if (!out || cap < 2)
      return NameTooLong;
    cap = std::min(cap, PathSize);
    if (SdFatVerifiers::reserved(input))
      return Access;
    if (!allowMissing && !preserveLeaf && readFile_ && !strcmp(input, readPath_)) {
      if (strlen(readPath_) >= cap)
        return NameTooLong;
      memmove(out, readPath_, strlen(readPath_) + 1);
      return Ok;
    }
    char request[PathSize];
    strcpy(request, input);
    FsFile dir = sd_.open("/", O_RDONLY);
    CloseFile closeDir{dir};
    if (!dir)
      return Io;
    strcpy(out, "/");
    size_t used = 1;
    for (char *part = request + 1; *part;) {
      char *next = strchr(part, '/');
      if (next)
        *next = 0;
      FsFile file;
      CloseFile closeFile{file};
      if (!file.open(&dir, part, O_RDONLY)) {
        if (dir.getError())
          return Io;
        if (!next && allowMissing) {
          size_t n = strlen(part), sep = used > 1;
          if (used + sep + n >= cap)
            return NameTooLong;
          if (sep)
            out[used++] = '/';
          memcpy(out + used, part, n + 1);
          return SdFatVerifiers::reserved(out) ? Access : Ok;
        }
        return NoEnt;
      }
      uint32_t index = file.dirIndex();
      file.close();
      if (!dir.seekSet(uint64_t(index) * 32) || !file.open(&dir, index, O_RDONLY))
        return Io;
      if (!file.isFileOrSubDir())
        return NotSupported;
      char stored[PathSize];
      size_t n = file.getName(stored, sizeof(stored));
      if (!n || n >= sizeof(stored) - 1)
        return file.getError() ? Io : NameTooLong;
      valid = validateName(stored);
      if (valid != Ok)
        return valid;
      size_t sep = used > 1;
      if (used + sep + n >= cap)
        return NameTooLong;
      if (sep)
        out[used++] = '/';
      size_t leafAt = used;
      memcpy(out + used, stored, n + 1);
      used += n;
      if (SdFatVerifiers::reserved(out))
        return Access;
      if (!next) {
        if (preserveLeaf) {
          n = strlen(part);
          if (leafAt + n >= cap)
            return NameTooLong;
          memcpy(out + leafAt, part, n + 1);
          return SdFatVerifiers::reserved(out) ? Access : Ok;
        }
        if (!allowMissing) {
          readFile_.close();
          readFile_.move(&file);
          cacheAttributes(out);
        }
        return Ok;
      }
      if (!file.isDir())
        return NotDir;
      dir.close();
      dir.move(&file);
      part = next + 1;
    }
    if (!allowMissing) {
      readFile_.close();
      readFile_.move(&dir);
      cacheAttributes(out);
    }
    return Ok;
  }
  void cacheAttributes(const char *path) {
    FsFile &f = readFile_;
    readAttr_ = Attr();
    readAttr_.type = f.isDir() ? 2 : 1;
    readAttr_.mode = f.isDir() ? 0777 : 0666;
    if (f.attrib() & FS_ATTRIB_READ_ONLY)
      readAttr_.mode &= ~0222;
    readAttr_.size = readAttr_.used = f.isDir() ? 0 : f.fileSize();
    uint16_t date = 0, time = 0;
    if (f.getModifyDateTime(&date, &time))
      readAttr_.atime.seconds = readAttr_.ctime.seconds = readAttr_.mtime.seconds =
          dosTime(date, time);
    strcpy(readPath_, path);
  }
  storage::SectorJournal *journal_ = nullptr;
  Remount remount_ = nullptr;
  void *context_ = nullptr;
  bool poison_ = false;
  SdFatVerifiers verifiers_;
  template <class F> Status transact(F operation) {
    if (poison_)
      return Io;
    invalidateCaches();
    if (!journal_) {
      Status s = operation();
      invalidateCaches();
      return s;
    }
    if (!remount_ || !journal_->healthy())
      return Io;
    storage::Error e = journal_->begin();
    if (e != storage::Error::Ok)
      return Io;
    Status status = operation();
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
  struct CloseFile {
    FsFile &file;
    ~CloseFile() {
      file.close();
    }
  };
  FsVolume &sd_;
  Space cachedSpace_;
  bool spaceValid_ = false;
  uint32_t scanBudget_ = 64;
  FsFile directory_, readFile_;
  char readPath_[PathSize] = {};
  Attr readAttr_;
  char directoryPath_[PathSize] = {}, lastName_[PathSize] = {};
  uint64_t nextIndex_ = 0, lastIndex_ = 0;
  Attr lastAttr_;
  bool lastValid_ = false;
  Status copyEntry(char *name, size_t cap, bool &eof) {
    if (strlen(lastName_) >= cap)
      return NameTooLong;
    strcpy(name, lastName_);
    eof = false;
    return Ok;
  }
  static uint32_t dosTime(uint16_t date, uint16_t time) {
    if (!date)
      return 0;
    int y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    y -= m <= 2;
    int era = y / 400;
    unsigned yo = y - era * 400, day = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = era * 146097 + yo * 365 + yo / 4 - yo / 100 + day - 719468;
    return uint32_t(days * 86400 + (time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2);
  }
};
} // namespace esp32_nfs
