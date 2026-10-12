// SPDX-License-Identifier: MIT
#pragma once
#include "PathVfs.h"
#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_littlefs.h>
#else
#include <sys/statvfs.h>
#endif
namespace esp32_nfs {
// Native ESP-IDF LittleFS VFS adapter. The caller mounts a LittleFS volume first.
// Unlike Arduino File, POSIX write/fsync/ftruncate report errors. No permission sidecars.
// One cached descriptor coalesces UNSTABLE writes; eviction/COMMIT/stable WRITE flushes it.
class LittleFsStorage : public PathStorage {
public:
  struct Metrics {
    uint32_t opens = 0, stats = 0, reads = 0, writes = 0, syncs = 0, entries = 0, cachedPatches = 0,
             cacheFlushes = 0;
    uint64_t mergedBytes = 0;
  } metrics;
  explicit LittleFsStorage(const char *mountPath, const char *partitionLabel = nullptr,
                           size_t smallWriteCache = 8192) {
    if (smallWriteCache >= 512 && smallWriteCache <= 16384 &&
        !(smallWriteCache & (smallWriteCache - 1)))
      cacheCapacity_ = smallWriteCache;
    if (partitionLabel && strlen(partitionLabel) < sizeof(label_))
      strcpy(label_, partitionLabel);
    if (mountPath && mountPath[0] == '/' && strlen(mountPath) < sizeof(base_)) {
      strcpy(base_, mountPath);
      size_t n = strlen(base_);
      while (n > 1 && base_[n - 1] == '/')
        base_[--n] = 0;
    }
  }
  ~LittleFsStorage() override {
    invalidateCaches();
  }
  Status setSmallWriteCache(size_t bytes) {
    if (bytes && (bytes < 512 || bytes > 16384 || (bytes & (bytes - 1))))
      return Invalid;
    Status s = closeFile();
    if (s == Ok)
      cacheCapacity_ = bytes;
    return s;
  }
  bool bufferedWrites() const override {
    return true;
  }
  uint64_t generation() const override {
    return generation_;
  }
  uint64_t maxFileSize() const override {
    return INT32_MAX;
  }
  void invalidateCaches() override {
    closeFile();
    closeDirectory();
    rootValid_ = statValid_ = false;
  }
  Status stat(const char *p, Attr &a) override {
    if (poison_)
      return Io;
    if (fd_ >= 0 && !strcmp(filePath_, p)) {
      a = fileAttr_;
      return Ok;
    }
    if (!strcmp(p, "/") && rootValid_) {
      a = rootAttr_;
      return Ok;
    }
    if (directory_ && directoryAttrValid_ && !strcmp(p, directoryPath_)) {
      a = directoryAttr_;
      return Ok;
    }
    if (statValid_ && !strcmp(p, statPath_)) {
      a = statAttr_;
      return Ok;
    }
    char full[sizeof(base_) + PathSize];
    Status s = path(p, full);
    if (s != Ok)
      return s;
    struct ::stat st;
    ++metrics.stats;
    if (::stat(full, &st))
      return error();
    if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))
      return NotSupported;
    attributes(st, a);
    strcpy(statPath_, p);
    statAttr_ = a;
    statValid_ = true;
    if (!strcmp(p, "/")) {
      rootAttr_ = a;
      rootValid_ = true;
    }
    return Ok;
  }
  Status read(const char *p, uint64_t off, uint8_t *data, size_t &n) override {
    Status s = cache(p);
    if (s != Ok)
      return s;
    if (off >= fileAttr_.size) {
      n = 0;
      return Ok;
    }
    if (off > maxFileSize())
      return FBig;
    n = std::min<uint64_t>(n, fileAttr_.size - off);
    // Fully cached reads do not seek/read the writable LittleFS descriptor: a read
    // there would end its sequential write and copy the remaining file tail.
    uint64_t end = off + n, covered = off;
    while (covered < end) {
      uint64_t next = covered;
      for (size_t i = 0; i < patchCount_; ++i)
        if (patches_[i].offset <= covered)
          next = std::max(next, patches_[i].offset + patches_[i].length);
      if (next == covered)
        break;
      covered = next;
    }
    if (covered < end) {
      size_t disk = off < storedSize_ ? size_t(std::min<uint64_t>(n, storedSize_ - off)) : 0;
      if (disk) {
        if (position_ != off && ::lseek(fd_, off, SEEK_SET) < 0)
          return error();
        ++metrics.reads;
        ssize_t got = ::read(fd_, data, disk);
        if (got < 0)
          return error();
        position_ = off + got;
        if (size_t(got) != disk)
          return Io;
      }
      if (n > disk)
        memset(data + disk, 0, n - disk);
    }
    // Arrival order implements last-write-wins for overlapping patches.
    for (size_t i = 0; i < patchCount_; ++i) {
      const Patch &p = patches_[i];
      uint64_t begin = std::max(off, p.offset), stop = std::min(end, p.offset + p.length);
      if (begin < stop)
        memcpy(data + size_t(begin - off), writeCache_ + p.data + size_t(begin - p.offset),
               size_t(stop - begin));
    }
    return Ok;
  }
  Status write(const char *p, uint64_t off, const uint8_t *data, size_t &n) override {
    Status s = writeData(p, off, data, n, false);
    return s == Ok ? sync(p) : s;
  }
  Status writeBuffered(const char *p, uint64_t off, const uint8_t *data, size_t &n) override {
    return writeData(p, off, data, n, true);
  }

private:
  Status writeData(const char *p, uint64_t off, const uint8_t *data, size_t &n, bool coalesce) {
    Status s = cache(p, true);
    if (s != Ok)
      return s;
    if (off > maxFileSize() || n > maxFileSize() - off)
      return FBig;
    if (off > fileAttr_.size && off - fileAttr_.size > BlockSize)
      return NotSupported;
    if (!n)
      return Ok;
    statValid_ = false;
    if (coalesce && cacheCapacity_ && n < 4096 && n <= cacheCapacity_) {
      // Bound both memory and the amount copied through one flush. The span cap
      // prevents two tiny distant updates from streaming an arbitrarily large file.
      uint64_t first = patchCount_ ? std::min(patchFirst_, off) : off;
      uint64_t end = patchCount_ ? std::max(patchEnd_, off + n) : off + n;
      if (patchCount_ == MaxPatches || n > cacheCapacity_ - cacheUsed_ ||
          end - first > MaxPatchSpan) {
        s = flushPatches();
        if (s != Ok)
          return s;
        first = off;
        end = off + n;
      }
      if (!writeCache_)
        writeCache_ = new (std::nothrow) uint8_t[cacheCapacity_];
      if (!writeCache_)
        return Jukebox;
      memcpy(writeCache_ + cacheUsed_, data, n);
      patches_[patchCount_++] = Patch{off, uint16_t(cacheUsed_), uint16_t(n)};
      cacheUsed_ += n;
      patchFirst_ = first;
      patchEnd_ = end;
      dirty_ = true;
      ++metrics.cachedPatches;
      fileAttr_.size = std::max(fileAttr_.size, off + n);
      fileAttr_.used = fileAttr_.size;
      return Ok;
    }
    s = flushPatches();
    if (s != Ok)
      return s;
    if (position_ != off && ::lseek(fd_, off, SEEK_SET) < 0)
      return error();
    position_ = off;
    ++metrics.writes;
    ssize_t got = ::write(fd_, data, n);
    if (got < 0)
      return error();
    if (n && !got)
      return NoSpace;
    n = got;
    position_ = off + n;
    storedSize_ = std::max(storedSize_, position_);
    fileAttr_.size = std::max(fileAttr_.size, position_);
    fileAttr_.used = fileAttr_.size;
    backingDirty_ = dirty_ = true;
    return Ok;
  }

public:
  Status sync(const char *p) override {
    if (poison_)
      return Io;
    if (fd_ >= 0 && dirty_ && (!p || !strcmp(p, filePath_))) {
      Status pending = flushPatches();
      if (pending != Ok)
        return pending;
      ++metrics.syncs;
      if (::fsync(fd_)) {
        poison_ = true;
        ++generation_;
        return Io;
      }
      backingDirty_ = dirty_ = false;
    }
    return Ok;
  }
  Status setattr(const char *p, const SetAttr &a) override {
    Attr old;
    Status s = stat(p, old);
    if (s != Ok)
      return s;
    if ((a.uidSet && a.uid) || (a.gidSet && a.gid) || a.atimeHow || a.mtimeHow ||
        (a.modeSet && a.mode != old.mode))
      return NotSupported;
    if (!a.sizeSet)
      return Ok;
    if (old.type == 2)
      return IsDir;
    if (a.size > maxFileSize())
      return FBig;
    if (a.size > old.size && a.size - old.size > BlockSize)
      return NotSupported;
    s = cache(p, true);
    if (s != Ok)
      return s;
    s = flushPatches();
    if (s != Ok)
      return s;
    patchCount_ = 0;
    statValid_ = false;
    if (::ftruncate(fd_, a.size))
      return error();
    storedSize_ = fileAttr_.size = fileAttr_.used = a.size;
    backingDirty_ = dirty_ = true;
    return sync(p);
  }
  Status create(const char *p, bool directory) override {
    Status s = namespaceChange();
    if (s != Ok)
      return s;
    char full[sizeof(base_) + PathSize];
    s = path(p, full);
    if (s != Ok)
      return s;
    if (directory)
      return ::mkdir(full, 0777) ? error() : Ok; // LittleFS commits namespace operations
    int fd = ::open(full, O_CREAT | O_EXCL | O_RDWR, 0666);
    ++metrics.opens;
    if (fd < 0)
      return error();
    ++metrics.syncs;
    int result = ::fsync(fd);
    int saved = errno;
    ::close(fd);
    errno = saved;
    return result ? error() : Ok;
  }
  Status remove(const char *p, bool directory) override {
    Status s = namespaceChange();
    if (s != Ok)
      return s;
    char full[sizeof(base_) + PathSize];
    s = path(p, full);
    if (s != Ok)
      return s;
    return (directory ? ::rmdir(full) : ::unlink(full)) ? error() : Ok;
  }
  Status rename(const char *a, const char *b) override {
    Status s = namespaceChange();
    if (s != Ok)
      return s;
    char from[sizeof(base_) + PathSize], to[sizeof(base_) + PathSize];
    s = path(a, from);
    if (s != Ok)
      return s;
    s = path(b, to);
    if (s != Ok)
      return s;
    return ::rename(from, to) ? error()
                              : Ok; // LittleFS's atomic native rename, including replacement
  }
  Status entry(const char *p, uint64_t index, char *name, size_t cap, bool &eof) override {
    if (poison_)
      return Io;
    if (!directory_ || strcmp(directoryPath_, p)) {
      closeDirectory();
      char full[sizeof(base_) + PathSize];
      Status s = path(p, full);
      if (s != Ok)
        return s;
      directory_ = ::opendir(full);
      if (!directory_)
        return error();
      strcpy(directoryPath_, p);
      directoryAttrValid_ = stat(p, directoryAttr_) == Ok;
    }
    if (lastValid_ && index == lastIndex_)
      return copyEntry(name, cap, eof);
    if (index < nextIndex_) {
      ::rewinddir(directory_);
      nextIndex_ = 0;
      lastValid_ = false;
    }
    // Bound one call's forward scan, not the volume's number of directory entries.
    unsigned budget = 64;
    while (nextIndex_ <= index && budget--) {
      errno = 0;
      ++metrics.entries;
      struct dirent *e = ::readdir(directory_);
      if (!e) {
        eof = true;
        return errno ? error() : Ok;
      }
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
        continue;
      if (nextIndex_++ != index)
        continue;
      if (strlen(e->d_name) >= sizeof(lastName_))
        return NameTooLong;
      strcpy(lastName_, e->d_name);
      lastIndex_ = index;
      lastValid_ = true;
      return copyEntry(name, cap, eof);
    }
    return Jukebox; // retry the same cookie to continue a bounded seek
  }
  Status space(Space &s) override {
    s = Space();
#ifdef ARDUINO_ARCH_ESP32
    size_t total = 0, used = 0;
    if (esp_littlefs_info(label_[0] ? label_ : nullptr, &total, &used) != ESP_OK)
      return Io;
    s.totalBytes = total;
    s.freeBytes = total - used;
#else
    struct statvfs st;
    if (::statvfs(base_, &st))
      return error();
    s.totalBytes = uint64_t(st.f_blocks) * st.f_frsize;
    s.freeBytes = uint64_t(st.f_bavail) * st.f_frsize;
#endif
    return Ok;
  }

private:
  char label_[17] = {};
  char base_[128] = {}, filePath_[PathSize] = {}, directoryPath_[PathSize] = {},
       lastName_[PathSize] = {};
  int fd_ = -1;
  bool fileWritable_ = false;
  DIR *directory_ = nullptr;
  uint64_t position_ = 0, nextIndex_ = 0, lastIndex_ = 0, generation_ = 1;
  bool dirty_ = false, poison_ = false, lastValid_ = false, rootValid_ = false;
  Attr fileAttr_, rootAttr_, statAttr_, directoryAttr_;
  bool directoryAttrValid_ = false;
  char statPath_[PathSize] = {};
  bool statValid_ = false;
  uint8_t *writeCache_ = nullptr;
  struct Patch {
    uint64_t offset;
    uint16_t data, length;
  };
  static constexpr size_t MaxPatches = 32;
  static constexpr uint64_t MaxPatchSpan = 65536;
  Patch patches_[MaxPatches];
  size_t cacheCapacity_ = 0, cacheUsed_ = 0, patchCount_ = 0;
  uint64_t patchFirst_ = 0, patchEnd_ = 0, storedSize_ = 0;
  bool backingDirty_ = false;
  Status flushPatches() {
    if (!patchCount_)
      return Ok;
    // A separate read-only descriptor supplies unchanged gaps while the writer
    // advances monotonically. Publish any earlier backend writes before opening
    // it; an independent LittleFS descriptor cannot see uncommitted file state.
    if (backingDirty_) {
      ++metrics.syncs;
      if (::fsync(fd_))
        return poison(error());
      backingDirty_ = false;
    }
    char full[sizeof(base_) + PathSize];
    Status s = path(filePath_, full);
    if (s != Ok)
      return poison(s);
    ++metrics.opens;
    int original = ::open(full, O_RDONLY);
    if (original < 0)
      return poison(error());
    uint8_t chunk[512];
    uint64_t readerPosition = 0;
    if (::lseek(fd_, patchFirst_, SEEK_SET) < 0)
      s = error();
    for (uint64_t off = patchFirst_; s == Ok && off < patchEnd_;) {
      size_t n = size_t(std::min<uint64_t>(sizeof(chunk), patchEnd_ - off));
      size_t disk = off < storedSize_ ? size_t(std::min<uint64_t>(n, storedSize_ - off)) : 0;
      // Reading unchanged bytes is cheap compared with repeatedly copying the
      // LittleFS tail. It also preserves holes and partially overlapping writes.
      if (disk) {
        if (readerPosition != off && ::lseek(original, off, SEEK_SET) < 0) {
          s = error();
          break;
        }
        ++metrics.reads;
        ssize_t got = ::read(original, chunk, disk);
        if (got < 0 || size_t(got) != disk) {
          s = got < 0 ? error() : Io;
          break;
        }
        readerPosition = off + disk;
      }
      if (n > disk)
        memset(chunk + disk, 0, n - disk);
      for (size_t i = 0; i < patchCount_; ++i) {
        const Patch &p = patches_[i];
        uint64_t begin = std::max(off, p.offset), end = std::min(off + n, p.offset + p.length);
        if (begin < end)
          memcpy(chunk + size_t(begin - off), writeCache_ + p.data + size_t(begin - p.offset),
                 size_t(end - begin));
      }
      for (size_t done = 0; done < n;) {
        ++metrics.writes;
        ssize_t got = ::write(fd_, chunk + done, n - done);
        if (got <= 0) {
          s = got < 0 ? error() : NoSpace;
          break;
        }
        done += got;
        metrics.mergedBytes += got;
      }
      off += n;
    }
    if (::close(original) && s == Ok)
      s = error();
    if (s != Ok)
      return poison(s);
    position_ = patchEnd_;
    storedSize_ = std::max(storedSize_, position_);
    patchCount_ = cacheUsed_ = 0;
    backingDirty_ = true;
    ++metrics.cacheFlushes;
    return Ok;
  }
  Status poison(Status error) {
    if (!poison_) {
      poison_ = true;
      ++generation_;
    }
    return error;
  }

  static void attributes(const struct ::stat &st, Attr &a) {
    a = Attr();
    a.type = S_ISDIR(st.st_mode) ? 2 : 1;
    a.mode = a.type == 2 ? 0777 : 0666;
    a.size = a.type == 2 ? 0 : st.st_size;
    a.used = a.size;
    a.atime.seconds = st.st_atime;
    a.mtime.seconds = st.st_mtime;
    a.ctime.seconds = st.st_ctime;
  }
  Status path(const char *p, char *out) const {
    if (!base_[0] || !p || p[0] != '/')
      return Invalid;
    if (strlen(p) >= PathSize)
      return NameTooLong;
    for (const char *s = p; *s;) {
      if (*s++ != '/')
        return Invalid;
      const char *end = strchr(s, '/');
      size_t n = end ? size_t(end - s) : strlen(s);
      if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.') || memchr(s, '\\', n))
        return Invalid;
      if (!end)
        break;
      s = end;
    }
    strcpy(out, base_);
    strcat(out, p);
    return Ok;
  }
  static Status error() {
    switch (errno) {
    case ENOENT:
      return NoEnt;
    case EEXIST:
      return Exist;
    case ENOSPC:
      return NoSpace;
    case EACCES:
    case EPERM:
      return Access;
    case EROFS:
      return ReadOnly;
    case ENOTDIR:
      return NotDir;
    case EISDIR:
      return IsDir;
    case ENOTEMPTY:
      return NotEmpty;
    case EINVAL:
      return Invalid;
    case ENAMETOOLONG:
      return NameTooLong;
    case EFBIG:
      return FBig;
    default:
      return Io;
    }
  }
  Status cache(const char *p, bool writing = false) {
    if (poison_)
      return Io;
    if (fd_ >= 0 && !strcmp(p, filePath_) && (!writing || fileWritable_))
      return Ok;
    Status s = closeFile();
    if (s != Ok)
      return s;
    char full[sizeof(base_) + PathSize];
    s = path(p, full);
    if (s != Ok)
      return s;
    ++metrics.opens;
    fd_ = ::open(full, writing ? O_RDWR : O_RDONLY);
    fileWritable_ = writing;
    if (fd_ < 0)
      return error();
    struct ::stat st;
    ++metrics.stats;
    if (::fstat(fd_, &st)) {
      s = error();
      ::close(fd_);
      fd_ = -1;
      return s;
    }
    if (!S_ISREG(st.st_mode)) {
      ::close(fd_);
      fd_ = -1;
      return IsDir;
    }
    strcpy(filePath_, p);
    attributes(st, fileAttr_);
    storedSize_ = fileAttr_.size;
    position_ = 0;
    return Ok;
  }
  Status closeFile() {
    Status s = sync(filePath_);
    if (fd_ >= 0) {
      if (::close(fd_) && s == Ok)
        s = error();
      fd_ = -1;
    }
    filePath_[0] = 0;
    delete[] writeCache_;
    writeCache_ = nullptr;
    patchCount_ = cacheUsed_ = 0;
    backingDirty_ = dirty_ = false;
    return s;
  }
  void closeDirectory() {
    if (directory_)
      ::closedir(directory_);
    directory_ = nullptr;
    directoryAttrValid_ = false;
    directoryPath_[0] = 0;
    nextIndex_ = 0;
    lastValid_ = false;
  }
  Status namespaceChange() {
    Status s = closeFile();
    closeDirectory();
    rootValid_ = statValid_ = false;
    return s;
  }
  Status copyEntry(char *name, size_t cap, bool &eof) {
    if (strlen(lastName_) >= cap)
      return NameTooLong;
    strcpy(name, lastName_);
    eof = false;
    return Ok;
  }
};
} // namespace esp32_nfs
