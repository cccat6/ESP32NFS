// SPDX-License-Identifier: MIT
#pragma once
#include "ESP32NFS.h"
#include <string.h>
namespace esp32_nfs {
namespace name_detail {
bool next(const char *&p, const char *end, uint32_t &cp);
Status component(const char *p, bool fat = false);
Status path(const char *p, bool fat = false);
bool below(const char *p, const char *base);
inline unsigned char asciiLower(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}
} // namespace name_detail
} // namespace esp32_nfs
