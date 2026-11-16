// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <cstdint>
#include <cstdio>
#include <string>

#include "hash.h"

namespace {

int failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

// xxHash's own sanity buffer (tests/sanity_test.c, fillTestBuffer), which its published vectors hash.
std::string SanityBuffer(size_t size) {
  std::string buffer(size, '\0');
  uint64_t generator = 2654435761U;
  for (size_t i = 0; i < size; ++i) {
    buffer[i] = static_cast<char>(generator >> 56);
    generator *= 11400714785074694797ULL;
  }
  return buffer;
}

}  // namespace

int main() {
  using retro_host::Hash;
  using retro_host::PictureHash;
  using retro_host::StateHash;
  using retro_host::UsedLength;

  // xxHash 0.8.3's tests/sanity_test_vectors.h, XSUM_XXH3_testdata: lengths 0, 1 and 1025, seed 0, and
  // 1025 with seed 0x9E3779B185EBCA8D.
  const std::string sanity = SanityBuffer(1025);
  Expect(Hash(sanity.data(), 0) == 0x2D06800538D394C2ULL, "XXH3-64 of no bytes is the published value");
  Expect(Hash(sanity.data(), 1) == 0xC44BDFF4074EECDBULL, "XXH3-64 of 1 byte is the published value");
  Expect(Hash(sanity.data(), 1025) == 0xD870C0FA13211C6AULL, "XXH3-64 of 1025 bytes is the published value");
  if (sizeof(size_t) == 8) {
    Expect(StateHash(sanity.data(), 1025, static_cast<size_t>(0x9E3779B185EBCA8DULL)) == 0x96792BCF9AF88519ULL,
           "a state's hash is XXH3-64 seeded with the buffer's size");
  }

  for (size_t size = 0; size < 200; ++size) {
    for (size_t last = 0; last <= size; ++last) {
      std::string buffer(size, '\0');
      for (size_t at = 0; at < last; at += 3) buffer[at] = static_cast<char>(at + 1);
      if (last) buffer[last - 1] = 'x';
      if (UsedLength(buffer.data(), buffer.size()) != last) {
        std::fprintf(stderr, "FAIL: used length of %zu bytes ending at %zu\n", size, last);
        ++failures;
      }
    }
  }
  std::string state(11 * 1024 * 1024, '\0');
  for (size_t at = 0; at < 3 * 1024 * 1024; at += 4093) state[at] = static_cast<char>(at | 1);
  state[3 * 1024 * 1024 + 5] = 1;
  Expect(UsedLength(state.data(), state.size()) == 3 * 1024 * 1024 + 6, "a state-sized buffer's used length");
  const size_t used = UsedLength(state.data(), state.size());
  Expect(StateHash(state.data(), used, state.size()) != StateHash(state.data(), used, state.size() + 1),
         "the buffer's size is part of a state's hash");

  // A picture hashes the same from either byte order, its fourth bytes are not hashed, and the size
  // and every colour byte count; checked against the definition's bytes hashed in one go, over more
  // pixels than one chunk.
  const uint32_t width = 101;
  const uint32_t height = 57;
  std::string rgba(static_cast<size_t>(width) * height * 4, '\0');
  std::string bgra(rgba.size(), '\0');
  std::string defined(8 + static_cast<size_t>(width) * height * 3, '\0');
  for (int i = 0; i < 4; ++i) {
    defined[i] = static_cast<char>(width >> (8 * i));
    defined[4 + i] = static_cast<char>(height >> (8 * i));
  }
  for (size_t pixel = 0; pixel < static_cast<size_t>(width) * height; ++pixel) {
    const char r = static_cast<char>(pixel * 7), g = static_cast<char>(pixel * 13 + 1), b = static_cast<char>(pixel >> 3);
    rgba[pixel * 4] = r, rgba[pixel * 4 + 1] = g, rgba[pixel * 4 + 2] = b, rgba[pixel * 4 + 3] = static_cast<char>(pixel);
    bgra[pixel * 4] = b, bgra[pixel * 4 + 1] = g, bgra[pixel * 4 + 2] = r, bgra[pixel * 4 + 3] = 0;
    defined[8 + pixel * 3] = r, defined[8 + pixel * 3 + 1] = g, defined[8 + pixel * 3 + 2] = b;
  }
  const auto* rgba_bytes = reinterpret_cast<const uint8_t*>(rgba.data());
  const auto* bgra_bytes = reinterpret_cast<const uint8_t*>(bgra.data());
  const uint64_t picture = PictureHash(rgba_bytes, width, height, 0, 2);
  Expect(picture == Hash(defined.data(), defined.size()), "a picture hashes its size and RGB bytes");
  Expect(picture == PictureHash(bgra_bytes, width, height, 2, 0), "rgba8 and bgra8 copies hash alike");
  Expect(picture != PictureHash(rgba_bytes, height, width, 0, 2), "the width and height are hashed");
  rgba[4 * 5000 + 1] ^= 1;
  Expect(picture != PictureHash(rgba_bytes, width, height, 0, 2), "a changed green byte changes the hash");

  if (failures) return 1;
  std::puts("All hash checks passed.");
  return 0;
}
