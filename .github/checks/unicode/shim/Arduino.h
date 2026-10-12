// SPDX-License-Identifier: MIT
#pragma once
// Minimal host-only Print/Stream compatibility for executing upstream SdFat on image disks.
#include <chrono>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ARDUINO 10819
#define SS 0
#define HEX 16
#define DEC 10
#define OCT 8
#define BIN 2
class __FlashStringHelper;
#define F(s) reinterpret_cast<const __FlashStringHelper *>(s)
inline uint32_t millis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline void yield() {}
class Print {
  int error_ = 0;

public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *p, size_t n) {
    size_t done = 0;
    while (done < n && write(p[done]))
      ++done;
    return done;
  }
  size_t write(const char *s) {
    return write(reinterpret_cast<const uint8_t *>(s), strlen(s));
  }
  virtual void flush() {}
  size_t print(const char *s) {
    return write(reinterpret_cast<const uint8_t *>(s), strlen(s));
  }
  size_t print(char *s) {
    return print(static_cast<const char *>(s));
  }
  size_t print(const __FlashStringHelper *s) {
    return print(reinterpret_cast<const char *>(s));
  }
  size_t print(char c) {
    return write(uint8_t(c));
  }
  template <typename T> size_t print(T value, int base = 10) {
    char b[40];
    if (base == 16)
      snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(value));
    else
      snprintf(b, sizeof(b), "%lld", static_cast<long long>(value));
    return print(b);
  }
  size_t println() {
    return print("\r\n");
  }
  template <typename T> size_t println(T value) {
    return print(value) + println();
  }
  template <typename T> size_t println(T value, int base) {
    return print(value, base) + println();
  }
  void setWriteError(int e = 1) {
    error_ = e;
  }
  int getWriteError() const {
    return error_;
  }
  void clearWriteError() {
    error_ = 0;
  }
};
class Stream : public Print {
public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
};
