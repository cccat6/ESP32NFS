// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace esp32_nfs {
void sha256(const uint8_t *, size_t, uint8_t out[32]);
}
