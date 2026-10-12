// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
namespace esp32_nfs {
class XdrIn {
public:
  const uint8_t *data;
  size_t size, pos = 0;
  bool valid = true;
  XdrIn(const uint8_t *p, size_t n) : data(p), size(n) {}
  bool take(size_t n) {
    if (!valid || n > size - pos) {
      valid = false;
      return false;
    }
    return true;
  }
  uint32_t u32() {
    if (!take(4))
      return 0;
    const uint8_t *p = data + pos;
    pos += 4;
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  }
  uint64_t u64() {
    uint64_t h = u32();
    return h << 32 | u32();
  }
  bool boolean() {
    uint32_t b = u32();
    if (b > 1)
      valid = false;
    return b != 0;
  }
  const uint8_t *fixed(size_t n) {
    if (n > SIZE_MAX - 3) {
      valid = false;
      return nullptr;
    }
    size_t padded = (n + 3) & ~size_t(3);
    if (!take(padded))
      return nullptr;
    const uint8_t *p = data + pos;
    for (size_t i = n; i < padded; ++i)
      if (p[i]) {
        valid = false;
        return nullptr;
      }
    pos += padded;
    return p;
  }
  const uint8_t *opaque(size_t &n, size_t max) {
    n = u32();
    if (n > max) {
      valid = false;
      return nullptr;
    }
    return fixed(n);
  }
  bool string(char *s, size_t cap) {
    size_t n;
    const uint8_t *p = opaque(n, cap - 1);
    if (!p)
      return false;
    if (memchr(p, 0, n)) {
      valid = false;
      return false;
    }
    memcpy(s, p, n);
    s[n] = 0;
    return true;
  }
  bool done() const {
    return valid && pos == size;
  }
};
class XdrOut {
public:
  uint8_t *data;
  size_t size, pos = 0;
  bool valid = true;
  XdrOut(uint8_t *p, size_t n) : data(p), size(n) {}
  void u32(uint32_t v) {
    if (!valid || size - pos < 4) {
      valid = false;
      return;
    }
    uint8_t *p = data + pos;
    pos += 4;
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
  }
  void u64(uint64_t v) {
    u32(v >> 32);
    u32(v);
  }
  void fixed(const void *src, size_t n) {
    if (n > SIZE_MAX - 3) {
      valid = false;
      return;
    }
    size_t p = (n + 3) & ~size_t(3);
    if (!valid || p > size - pos) {
      valid = false;
      return;
    }
    if (n)
      memcpy(data + pos, src, n);
    if (p > n)
      memset(data + pos + n, 0, p - n);
    pos += p;
  }
  void opaque(const void *p, size_t n) {
    u32(n);
    fixed(p, n);
  }
  void string(const char *s) {
    opaque(s, strlen(s));
  }
};
} // namespace esp32_nfs
