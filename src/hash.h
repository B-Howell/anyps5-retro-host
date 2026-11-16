// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#pragma once

#include <cstddef>
#include <cstdint>

namespace retro_host {

inline constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
inline constexpr uint64_t kFnvPrime = 1099511628211ULL;

// 64-bit FNV-1a over size bytes, continuing from hash.
inline uint64_t Fnv(const void* data, size_t size, uint64_t hash = kFnvOffset) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * kFnvPrime;
  return hash;
}

// The same as Fnv over count zero bytes: each one only multiplies by the prime, so the run costs a
// power of the prime (mod 2^64) instead of a byte at a time.
uint64_t FnvZeros(uint64_t count, uint64_t hash = kFnvOffset);

// How many bytes from the start of data hold every non-zero byte (0 when all are zero). Fnv over
// those, then FnvZeros over the rest, is Fnv over all of data.
size_t UsedLength(const void* data, size_t size);

}  // namespace retro_host
