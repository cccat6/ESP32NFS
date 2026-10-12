// SPDX-License-Identifier: MIT
#include "Names.h"
namespace esp32_nfs {
namespace name_detail {
// Strict Unicode scalar decoding; byte spelling and normalization form are preserved.
bool next(const char *&p, const char *end, uint32_t &cp) {
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
static Status range(const char *p, const char *end, bool fat) {
  if (p == end)
    return Invalid;
  if (fat && (*p == ' ' || end[-1] == ' ' || end[-1] == '.'))
    return Invalid;
  while (p < end) {
    uint32_t cp;
    if (!next(p, end, cp) || cp == '/' || cp == '\\')
      return Invalid;
    if (fat && (cp < 32 || cp == 0xffff || (cp < 128 && strchr("\"*:<>?|", char(cp)))))
      return Invalid;
  }
  return Ok;
}
Status component(const char *p, bool fat) {
  if (!p)
    return Invalid;
  size_t n = strnlen(p, PathSize);
  return n == PathSize ? NameTooLong : range(p, p + n, fat);
}
Status path(const char *p, bool fat) {
  if (!p || *p != '/')
    return Invalid;
  size_t n = strnlen(p, PathSize);
  if (n == PathSize)
    return NameTooLong;
  if (n == 1)
    return Ok;
  const char *limit = p + n;
  for (++p;;) {
    const char *end = strchr(p, '/');
    if (!end)
      end = limit;
    size_t size = end - p;
    if ((size == 1 && *p == '.') || (size == 2 && p[0] == '.' && p[1] == '.'))
      return Invalid;
    Status s = range(p, end, fat);
    if (s != Ok || end == limit)
      return s;
    p = end + 1;
  }
}
bool below(const char *p, const char *base) {
  size_t n = strlen(base);
  return !strncmp(p, base, n) && p[n] == '/';
}
} // namespace name_detail
} // namespace esp32_nfs
