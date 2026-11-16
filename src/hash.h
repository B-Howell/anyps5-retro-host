// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#pragma once

#include <cstddef>
#include <cstdint>

namespace retro_host {

// The hashes of a sync (protocol.md "sync", capability syncXxh3): XXH3-64 from the xxHash submodule,
// which is the same on every system and byte order and reads memory many bytes at a time.

// XXH3-64 of size bytes with seed 0: the console's main RAM.
uint64_t Hash(const void* data, size_t size);

// A serialized state in a buffer of size bytes that started zeroed, whose first used bytes hold
// every non-zero byte: XXH3-64 of those used bytes, seeded with size. Two such buffers of the same
// size are equal exactly when their used bytes are, so this stands for the whole buffer without
// reading its zero tail.
uint64_t StateHash(const void* data, size_t used, size_t size);

// A picture of width by height pixels of 4 bytes each, red, green and blue at offsets red, 1 and
// blue (the fourth byte is not hashed): XXH3-64 of the width and height as two 32-bit
// little-endian integers, then the red, green and blue bytes of each pixel, top row first. The
// same for an rgba8 and a bgra8 copy of one picture.
uint64_t PictureHash(const uint8_t* pixels, uint32_t width, uint32_t height, size_t red, size_t blue);

// How many bytes from the start of data hold every non-zero byte (0 when all are zero).
size_t UsedLength(const void* data, size_t size);

}  // namespace retro_host
