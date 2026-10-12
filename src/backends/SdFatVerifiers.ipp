#pragma once
// SPDX-License-Identifier: MIT
// Enable this optional SdFat-specific translation unit only when its dependency is available.
#include "../Digest.h"
#include "../Names.h"
#include "../storage/SectorJournal.h"
#include "SdFatVerifiers.h"
#include <common/upcase.h>
#include <new>
#include <string.h>
namespace esp32_nfs {
namespace verifier_detail {
inline const char *database() {
  return "/.esp32-nfs/exclusive.bin";
}
inline void put32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; ++i)
    p[i] = n >> (24 - 8 * i);
}
inline void put64(uint8_t *p, uint64_t n) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = n >> (56 - 8 * i);
}
inline uint32_t get32(const uint8_t *p) {
  uint32_t n = 0;
  for (unsigned i = 0; i < 4; ++i)
    n = n << 8 | p[i];
  return n;
}
inline uint64_t get64(const uint8_t *p) {
  uint64_t n = 0;
  for (unsigned i = 0; i < 8; ++i)
    n = n << 8 | p[i];
  return n;
}
inline bool valid(uint8_t *p) {
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
} // namespace verifier_detail
inline bool SdFatVerifiers::equal(const char *a, const char *b) {
  while (*a && *b)
    if (name_detail::asciiLower((unsigned char)*a++) !=
        name_detail::asciiLower((unsigned char)*b++))
      return false;
  return *a == *b;
}
inline bool SdFatVerifiers::below(const char *p, const char *base) {
  size_t n = strlen(base);
  if (strlen(p) <= n || p[n] != '/')
    return false;
  for (size_t i = 0; i < n; ++i)
    if (name_detail::asciiLower((unsigned char)p[i]) !=
        name_detail::asciiLower((unsigned char)base[i]))
      return false;
  return true;
}
inline bool SdFatVerifiers::reserved(const char *p) {
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
inline uint64_t SdFatVerifiers::key(const char *p) {
  uint64_t key = 14695981039346656037ULL;
  for (; *p; ++p) {
    key ^= name_detail::asciiLower((unsigned char)*p);
    key *= 1099511628211ULL;
  }
  return key ? key : 1; // full path comparison still resolves hash collisions
}
inline bool SdFatVerifiers::header() {
  uint8_t b[512] = {};
  memcpy(b, "ENFSVR01", 8);
  verifier_detail::put32(b + 8, count_);
  sha256(b, 480, b + 480);
  fat::FsBaseFile f = volume_.open(verifier_detail::database(), O_RDWR);
  verifier_detail::Close close{f};
  return f && f.seekSet(0) && f.write(b, 512) == 512 && f.sync();
}
inline void SdFatVerifiers::release() {
  while (pages_) {
    Page *next = pages_->next;
    memory_.release(pages_, sizeof(Page));
    pages_ = next;
  }
  tail_ = cursor_ = nullptr;
  count_ = capacity_ = cursorFirst_ = 0;
  loaded_ = false;
}
inline uint64_t &SdFatVerifiers::slotKey(uint32_t slot) {
  if (!cursor_ || slot < cursorFirst_) {
    cursor_ = pages_;
    cursorFirst_ = 0;
  }
  while (slot - cursorFirst_ >= 64) {
    cursor_ = cursor_->next;
    cursorFirst_ += 64;
  }
  return cursor_->keys[slot - cursorFirst_];
}
inline Status SdFatVerifiers::reserve(uint32_t count) {
  if (count <= capacity_)
    return Ok;
  if (count > UINT32_MAX - 63)
    return Jukebox;
  Page *first = nullptr, *last = nullptr;
  uint32_t capacity = capacity_;
  while (capacity < count) {
    auto *page = static_cast<Page *>(memory_.allocate(sizeof(Page), true));
    if (!page) {
      while (first) {
        Page *next = first->next;
        memory_.release(first, sizeof(Page));
        first = next;
      }
      return Jukebox;
    }
    if (last)
      last->next = page;
    else
      first = page;
    last = page;
    capacity += 64;
  }
  if (tail_)
    tail_->next = first;
  else
    pages_ = first;
  tail_ = last;
  capacity_ = capacity;
  return Ok;
}
inline Status SdFatVerifiers::initialize() {
  if (volume_.exists(verifier_detail::database()))
    return load();
  Status s = reserve(1);
  if (s != Ok)
    return s;
  if (!volume_.exists("/.esp32-nfs") && !volume_.mkdir("/.esp32-nfs", false))
    return Io;
  fat::FsBaseFile f = volume_.open(verifier_detail::database(), O_CREAT | O_EXCL | O_RDWR);
  if (!f)
    return Io;
  f.close();
  count_ = 0;
  for (Page *p = pages_; p; p = p->next)
    memset(p->keys, 0, sizeof(p->keys));
  loaded_ = header();
  return loaded_ ? Ok : Io;
}
inline Status SdFatVerifiers::load() {
  if (loaded_)
    return Ok;
  if (!volume_.exists(verifier_detail::database())) {
    count_ = 0;
    for (Page *p = pages_; p; p = p->next)
      memset(p->keys, 0, sizeof(p->keys));
    loaded_ = true;
    return Ok;
  }
  uint8_t b[512];
  fat::FsBaseFile f = volume_.open(verifier_detail::database(), O_RDONLY);
  verifier_detail::Close close{f};
  if (!f || f.read(b, 512) != 512 || memcmp(b, "ENFSVR01", 8) || !verifier_detail::valid(b))
    return Io;
  uint32_t count = verifier_detail::get32(b + 8);
  if ((uint64_t(count) + 1) * 512 > f.fileSize())
    return Io;
  Status s = reserve(count);
  if (s != Ok)
    return s;
  count_ = count;
  for (Page *p = pages_; p; p = p->next)
    memset(p->keys, 0, sizeof(p->keys));
  for (uint32_t i = 0; i < count_; ++i) {
    Record r;
    if (!readSlot(i, r))
      return Io;
    slotKey(i) = r.flags ? key(r.path) : 0;
  }
  loaded_ = true;
  return Ok;
}
inline bool SdFatVerifiers::readSlot(uint32_t slot, Record &r) {
  uint8_t b[512];
  fat::FsBaseFile f = volume_.open(verifier_detail::database(), O_RDONLY);
  verifier_detail::Close close{f};
  if (!f || !f.seekSet(uint64_t(slot + 1) * 512) || f.read(b, 512) != 512 ||
      memcmp(b, "ENFSVE01", 8) || !verifier_detail::valid(b))
    return false;
  r = Record();
  r.flags = verifier_detail::get32(b + 8);
  r.verifier = verifier_detail::get64(b + 24);
  uint32_t n = verifier_detail::get32(b + 32);
  if (n >= PathSize)
    return false;
  memcpy(r.path, b + 40, n);
  r.path[n] = 0;
  return true;
}
inline bool SdFatVerifiers::writeSlot(uint32_t slot, const Record &r) {
  uint8_t b[512] = {};
  memcpy(b, "ENFSVE01", 8);
  verifier_detail::put32(b + 8, r.flags);
  verifier_detail::put64(b + 24, r.verifier);
  verifier_detail::put32(b + 32, strlen(r.path));
  memcpy(b + 40, r.path, strlen(r.path));
  sha256(b, 480, b + 480);
  fat::FsBaseFile f = volume_.open(verifier_detail::database(), O_RDWR);
  verifier_detail::Close close{f};
  return f && f.seekSet(uint64_t(slot + 1) * 512) && f.write(b, 512) == 512 && f.sync();
}
inline Status SdFatVerifiers::get(const char *p, Record &r) {
  if (strlen(p) >= PathSize)
    return NameTooLong;
  Status loaded = load();
  if (loaded != Ok)
    return loaded;
  uint64_t k = key(p);
  for (uint32_t i = 0; i < count_; ++i)
    if (slotKey(i) == k) {
      if (!readSlot(i, r))
        return Io;
      if (equal(r.path, p) && r.flags)
        return Ok;
    }
  return NoEnt;
}
inline Status SdFatVerifiers::set(Record &r) {
  if (strlen(r.path) >= PathSize)
    return NameTooLong;
  Status s = initialize();
  if (s != Ok)
    return s;
  uint32_t slot = count_;
  uint64_t k = key(r.path);
  for (uint32_t i = 0; i < count_; ++i) {
    if (!slotKey(i) && slot == count_)
      slot = i;
    if (slotKey(i) == k) {
      Record old;
      if (!readSlot(i, old))
        return Io;
      if (equal(old.path, r.path)) {
        slot = i;
        break;
      }
    }
  }
  if (slot == UINT32_MAX)
    return Jukebox;
  s = reserve(slot + 1);
  if (s != Ok)
    return s;
  if (!writeSlot(slot, r))
    return Io;
  bool grew = slot == count_;
  if (grew)
    ++count_;
  slotKey(slot) = r.flags ? k : 0;
  return !grew || header() ? Ok : Io;
}
inline Status SdFatVerifiers::erase(const char *p) {
  Record r;
  Status s = get(p, r);
  if (s == NoEnt)
    return Ok;
  if (s != Ok)
    return s;
  r.flags = 0;
  return set(r);
}
inline Status SdFatVerifiers::moveTree(const char *from, const char *to) {
  Status loaded = load();
  if (loaded != Ok)
    return loaded;
  uint32_t matches = 0;
  size_t a = strlen(from), b = strlen(to);
  for (uint32_t i = 0; i < count_; ++i)
    if (slotKey(i)) {
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
    if (slotKey(i)) {
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
        slotKey(i) = key(path);
      }
    }
  return Ok;
}
} // namespace esp32_nfs
