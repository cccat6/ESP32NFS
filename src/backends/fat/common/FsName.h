/**
 * Copyright (c) 2011-2022 Bill Greiman
 * This file is part of the SdFat library for SD memory cards.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// SdFat 2.3.0 filesystem core, isolated UTF-8 variant; see README.md.
#pragma once
namespace esp32_nfs { namespace fat {

#ifndef ESP32NFS_PRIVATE_FsName_h
#define ESP32NFS_PRIVATE_FsName_h
} } // namespace esp32_nfs::fat
#include <stdint.h>

#include <common/SysCall.h>
namespace esp32_nfs { namespace fat {
/**
 * \file
 * \brief FsName class.
 */
/**
 * \class FsName
 * \brief Handle UTF-8 file names.
 */
class FsName {
 public:
  /** Beginning of LFN. */
  const char* begin;
  /** Next LFN character of end. */
  const char* next;
  /** Position one beyond last LFN character. */
  const char* end;
  uint16_t ls = 0;
  bool atEnd() { return !ls && next == end; }
  void reset() {
    next = begin;
    ls = 0;  // lowSurrogate
  }
  uint16_t get16();
};
#endif  // ESP32NFS_PRIVATE_FsName_h
} } // namespace esp32_nfs::fat
