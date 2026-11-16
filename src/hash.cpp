// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "hash.h"

#include <cstring>

#define XXH_INLINE_ALL
#include "xxhash.h"

namespace retro_host {

uint64_t Hash(const void* data, size_t size) { return XXH3_64bits(data, size); }

uint64_t StateHash(const void* data, size_t used, size_t size) {
  return XXH3_64bits_withSeed(data, used, static_cast<XXH64_hash_t>(size));
}

uint64_t PictureHash(const uint8_t* pixels, uint32_t width, uint32_t height, size_t red, size_t blue) {
  XXH3_state_t state;
  XXH3_INITSTATE(&state);
  XXH3_64bits_reset(&state);
  uint8_t size[8];
  for (int i = 0; i < 4; ++i) {
    size[i] = static_cast<uint8_t>(width >> (8 * i));
    size[4 + i] = static_cast<uint8_t>(height >> (8 * i));
  }
  XXH3_64bits_update(&state, size, sizeof size);
  // The pixels' colours in RGB order, a few thousand at a time, so the hash reads whole blocks.
  constexpr size_t kChunk = 4096;
  uint8_t rgb[kChunk * 3];
  const size_t count = static_cast<size_t>(width) * height;
  for (size_t done = 0; done < count;) {
    const size_t take = count - done < kChunk ? count - done : kChunk;
    const uint8_t* from = pixels + done * 4;
    for (size_t i = 0; i < take; ++i, from += 4) {
      rgb[i * 3] = from[red];
      rgb[i * 3 + 1] = from[1];
      rgb[i * 3 + 2] = from[blue];
    }
    XXH3_64bits_update(&state, rgb, take * 3);
    done += take;
  }
  return XXH3_64bits_digest(&state);
}

size_t UsedLength(const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  size_t end = size;
  // Whole 64-byte blocks of zeros first, each tested as one OR of its words, then byte by byte.
  constexpr size_t kBlock = 64;
  while (end % kBlock && bytes[end - 1] == 0) --end;
  if (end % kBlock == 0) {
    while (end >= kBlock) {
      uint64_t words[kBlock / 8];
      std::memcpy(words, bytes + end - kBlock, kBlock);
      uint64_t any = 0;
      for (const uint64_t word : words) any |= word;
      if (any) break;
      end -= kBlock;
    }
  }
  while (end > 0 && bytes[end - 1] == 0) --end;
  return end;
}

}  // namespace retro_host
