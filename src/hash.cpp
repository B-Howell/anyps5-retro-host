// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "hash.h"

#include <cstring>

namespace retro_host {

uint64_t FnvZeros(uint64_t count, uint64_t hash) {
  uint64_t factor = 1;
  uint64_t base = kFnvPrime;
  for (; count; count >>= 1) {
    if (count & 1) factor *= base;
    base *= base;
  }
  return hash * factor;
}

size_t UsedLength(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  size_t end = size;
  while (end % 8 && end > 0 && bytes[end - 1] == 0) --end;
  if (end % 8 == 0) {
    while (end >= 8) {
      uint64_t word;
      std::memcpy(&word, bytes + end - 8, 8);
      if (word) break;
      end -= 8;
    }
  }
  while (end > 0 && bytes[end - 1] == 0) --end;
  return end;
}

}  // namespace retro_host
