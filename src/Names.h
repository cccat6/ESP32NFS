// SPDX-License-Identifier: MIT
#pragma once
#include "ESP32NFS.h"
#include <string.h>
namespace esp32_nfs {
namespace name_detail {
// Strict Unicode scalar decoding; byte spelling and normalization form are preserved.
inline bool next(const char *&p, const char *end, uint32_t &cp) {
  if (p == end)
    return false;
  uint8_t lead = uint8_t(*p++);
  if (lead < 0x80) {
    cp = lead;
    return true;
  }
  unsigned count;
  uint32_t minimum;
  if (lead >= 0xc2 && lead <= 0xdf) {
    cp = lead & 31;
    count = 1;
    minimum = 0x80;
  } else if (lead >= 0xe0 && lead <= 0xef) {
    cp = lead & 15;
    count = 2;
    minimum = 0x800;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
    cp = lead & 7;
    count = 3;
    minimum = 0x10000;
  } else
    return false;
  if (size_t(end - p) < count)
    return false;
  while (count--) {
    uint8_t c = uint8_t(*p++);
    if ((c & 0xc0) != 0x80)
      return false;
    cp = (cp << 6) | (c & 63);
  }
  return cp >= minimum && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}
inline Status component(const char *p, bool fat = false) {
  if (!p || !*p)
    return Invalid;
  size_t n = 0;
  while (n < PathSize && p[n])
    ++n;
  if (n == PathSize)
    return NameTooLong;
  if (fat && (*p == ' ' || p[n - 1] == ' ' || p[n - 1] == '.'))
    return Invalid;
  const char *end = p + n;
  while (p < end) {
    uint32_t cp;
    if (!next(p, end, cp) || cp == '/' || cp == '\\')
      return Invalid;
    if (fat && (cp < 32 || cp == 0xffff || (cp < 128 && strchr("\"*:<>?|", char(cp)))))
      return Invalid;
  }
  return Ok;
}
// Absolute paths supplied to storage adapters have explicit, nonempty components.
inline Status path(const char *p, bool fat = false) {
  if (!p || *p != '/')
    return Invalid;
  size_t n = 0;
  while (n < PathSize && p[n])
    ++n;
  if (n == PathSize)
    return NameTooLong;
  if (n == 1)
    return Ok;
  char part[PathSize];
  for (++p; *p;) {
    const char *end = strchr(p, '/');
    size_t size = end ? size_t(end - p) : strlen(p);
    memcpy(part, p, size);
    part[size] = 0;
    if (!strcmp(part, ".") || !strcmp(part, ".."))
      return Invalid;
    Status s = component(part, fat);
    if (s != Ok)
      return s;
    if (!end)
      return Ok;
    p = end + 1;
    if (!*p)
      return Invalid;
  }
  return Invalid;
}
inline bool below(const char *p, const char *base) {
  size_t n = strlen(base);
  return strlen(p) > n && !strncmp(p, base, n) && p[n] == '/';
}
inline unsigned char asciiLower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
} // namespace name_detail
} // namespace esp32_nfs
