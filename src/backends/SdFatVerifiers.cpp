// SPDX-License-Identifier: MIT
// Enable this optional SdFat-specific translation unit only when its dependency is available.
#include "SdFatVerifiers.h"
#include "../Digest.h"
#include "../Names.h"
#include "../storage/SectorJournal.h"
#include <common/upcase.h>
#include <new>
#include <string.h>
namespace esp32_nfs {
namespace {
const char *Database = "/.esp32-nfs/exclusive.bin";
void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = n >> (24 - 8 * i);
}
void put64(uint8_t *p, uint64_t n) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = n >> (56 - 8 * i);
}
uint32_t get32(const uint8_t *p) {
  uint32_t n = 0;
  for (unsigned i = 0; i < 4; ++i)
    n = n << 8 | p[i];
  return n;
}
uint64_t get64(const uint8_t *p) {
  uint64_t n = 0;
  for (unsigned i = 0; i < 8; ++i)
    n = n << 8 | p[i];
  return n;
}
bool valid(uint8_t *p) {
  uint8_t d[32];
  sha256(p, 480, d);
  return !memcmp(d, p + 480, 32);
}
struct Close {
  fat::FsBaseFile &f;
  ~Close() {
    f.close();
  }
};
} // namespace
bool SdFatVerifiers::equal(const char *a, const char *b) {
  while (*a && *b)
    if (name_detail::asciiLower((unsigned char)*a++) !=
        name_detail::asciiLower((unsigned char)*b++))
      return false;
  return *a == *b;
}
bool SdFatVerifiers::below(const char *p, const char *base) {
  size_t n = strlen(base);
  if (strlen(p) <= n || p[n] != '/')
    return false;
  for (size_t i = 0; i < n; ++i)
    if (name_detail::asciiLower((unsigned char)p[i]) !=
        name_detail::asciiLower((unsigned char)base[i]))
      return false;
  return true;
}
bool SdFatVerifiers::reserved(const char *p) {
  if (!p)
    return false;
  if (*p == '/')
    ++p;
  const char *end = p + strlen(p);
  for (const char *q = ".ESP32-NFS"; *q; ++q) {
    uint32_t cp;
    if (!name_detail::next(p, end, cp) || cp > 0xffff || toUpcase(uint16_t(cp)) != uint8_t(*q))
      return false;
  }
  return p == end || *p == '/';
}
uint64_t SdFatVerifiers::key(const char *p) {
  uint64_t key = 14695981039346656037ULL;
  for (; *p; ++p) {
    key ^= name_detail::asciiLower((unsigned char)*p);
    key *= 1099511628211ULL;
  }
  return key ? key : 1; // full path comparison still resolves hash collisions
}
bool SdFatVerifiers::header() {
  uint8_t b[512] = {};
  memcpy(b, "ENFSVR01", 8);
  put32(b + 8, count_);
  sha256(b, 480, b + 480);
  fat::FsBaseFile f = volume_.open(Database, O_RDWR);
  Close close{f};
  return f && f.seekSet(0) && f.write(b, 512) == 512 && f.sync();
}
Status SdFatVerifiers::initialize() {
  if (!index_)
    index_ = new (std::nothrow) Index[MaxRecords];
  if (!index_)
    return Jukebox;
  if (volume_.exists(Database))
    return load() ? Ok : Io;
  if (!volume_.exists("/.esp32-nfs") && !volume_.mkdir("/.esp32-nfs", false))
    return Io;
  fat::FsBaseFile f = volume_.open(Database, O_CREAT | O_EXCL | O_RDWR);
  if (!f)
    return Io;
  f.close();
  count_ = 0;
  memset(index_, 0, sizeof(Index) * MaxRecords);
  loaded_ = header();
  return loaded_ ? Ok : Io;
}
bool SdFatVerifiers::load() {
  if (loaded_)
    return true;
  if (!volume_.exists(Database)) {
    count_ = 0;
    if (index_)
      memset(index_, 0, sizeof(Index) * MaxRecords);
    loaded_ = true;
    return true;
  }
  if (!index_)
    index_ = new (std::nothrow) Index[MaxRecords];
  if (!index_)
    return false;
  uint8_t b[512];
  fat::FsBaseFile f = volume_.open(Database, O_RDONLY);
  Close close{f};
  if (!f || f.read(b, 512) != 512 || memcmp(b, "ENFSVR01", 8) || !valid(b))
    return false;
  count_ = get32(b + 8);
  if (count_ > MaxRecords)
    return false;
  memset(index_, 0, sizeof(Index) * MaxRecords);
  for (uint32_t i = 0; i < count_; ++i) {
    Record r;
    if (!readSlot(i, r))
      return false;
    index_[i].key = r.flags ? key(r.path) : 0;
    index_[i].slot = i;
  }
  loaded_ = true;
  return true;
}
bool SdFatVerifiers::readSlot(uint32_t slot, Record &r) {
  uint8_t b[512];
  fat::FsBaseFile f = volume_.open(Database, O_RDONLY);
  Close close{f};
  if (!f || !f.seekSet(uint64_t(slot + 1) * 512) || f.read(b, 512) != 512 ||
      memcmp(b, "ENFSVE01", 8) || !valid(b))
    return false;
  r = Record();
  r.flags = get32(b + 8);
  r.verifier = get64(b + 24);
  uint32_t n = get32(b + 32);
  if (n >= PathSize)
    return false;
  memcpy(r.path, b + 40, n);
  r.path[n] = 0;
  return true;
}
bool SdFatVerifiers::writeSlot(uint32_t slot, const Record &r) {
  uint8_t b[512] = {};
  memcpy(b, "ENFSVE01", 8);
  put32(b + 8, r.flags);
  put64(b + 24, r.verifier);
  put32(b + 32, strlen(r.path));
  memcpy(b + 40, r.path, strlen(r.path));
  sha256(b, 480, b + 480);
  fat::FsBaseFile f = volume_.open(Database, O_RDWR);
  Close close{f};
  return f && f.seekSet(uint64_t(slot + 1) * 512) && f.write(b, 512) == 512 && f.sync();
}
Status SdFatVerifiers::get(const char *p, Record &r) {
  if (strlen(p) >= PathSize)
    return NameTooLong;
  if (!load())
    return Io;
  uint64_t k = key(p);
  for (uint32_t i = 0; i < count_; ++i)
    if (index_[i].key == k) {
      if (!readSlot(i, r))
        return Io;
      if (equal(r.path, p) && r.flags)
        return Ok;
    }
  return NoEnt;
}
Status SdFatVerifiers::set(Record &r) {
  if (strlen(r.path) >= PathSize)
    return NameTooLong;
  Status s = initialize();
  if (s != Ok)
    return s;
  uint32_t slot = count_;
  uint64_t k = key(r.path);
  for (uint32_t i = 0; i < count_; ++i) {
    if (!index_[i].key && slot == count_)
      slot = i;
    if (index_[i].key == k) {
      Record old;
      if (!readSlot(i, old))
        return Io;
      if (equal(old.path, r.path)) {
        slot = i;
        break;
      }
    }
  }
  if (slot >= MaxRecords)
    return Jukebox;
  if (!writeSlot(slot, r))
    return Io;
  bool grew = slot == count_;
  if (grew)
    ++count_;
  index_[slot].key = r.flags ? k : 0;
  index_[slot].slot = slot;
  return !grew || header() ? Ok : Io;
}
Status SdFatVerifiers::erase(const char *p) {
  Record r;
  Status s = get(p, r);
  if (s == NoEnt)
    return Ok;
  if (s != Ok)
    return s;
  r.flags = 0;
  return set(r);
}
Status SdFatVerifiers::moveTree(const char *from, const char *to) {
  if (!load())
    return Io;
  uint32_t matches = 0;
  size_t a = strlen(from), b = strlen(to);
  for (uint32_t i = 0; i < count_; ++i)
    if (index_[i].key) {
      Record r;
      if (!readSlot(i, r))
        return Io;
      if (equal(r.path, from) || below(r.path, from)) {
        if (b + strlen(r.path) - a >= PathSize)
          return NameTooLong;
        ++matches;
      }
    }
  // Keep filesystem directory/FAT updates within the same bounded transaction.
  if (matches > ESP32_NFS_TX_SECTORS - 12)
    return Jukebox;
  if (!count_)
    return Ok;
  Status e = equal(from, to) ? Ok : erase(to);
  if (e != Ok)
    return e;
  for (uint32_t i = 0; i < count_; ++i)
    if (index_[i].key) {
      Record r;
      if (!readSlot(i, r))
        return Io;
      if (equal(r.path, from) || below(r.path, from)) {
        char path[PathSize];
        strcpy(path, to);
        strcat(path, r.path + a);
        strcpy(r.path, path);
        if (!writeSlot(i, r))
          return Io;
        index_[i].key = key(path);
      }
    }
  return Ok;
}
} // namespace esp32_nfs
