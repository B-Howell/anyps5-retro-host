// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_RING_H_
#define ANYPS5_RETRO_HOST_RING_H_

#include <cstdint>
#include <optional>
#include <vector>

#include "platform.h"

namespace retro_host {

constexpr uint32_t kRingMagic = 0x46523541;
constexpr uint32_t kRingVersion = 1;
constexpr uint32_t kSlotCount = 3;
constexpr uint32_t kHeaderBytes = 4096;
constexpr uint64_t kMaxSlotBytes = 268435456;
constexpr uint32_t kMaxFrameSide = 8192;
constexpr uint32_t kNoSlot = 0xFFFFFFFF;

enum class PixelFormat : uint32_t { kRgba8 = 1, kBgra8 = 2 };

// The frame rings of protocol.md "Frame ring", written by one thread.
class FrameRings {
 public:
  explicit FrameRings(PixelFormat format) : format_(format) {}

  // A ring to announce before the frame of width x height can be published, or nullopt when
  // the current ring has room. fits is false when no ring can hold the frame, and no_memory when
  // the system would not give the memory for one.
  struct Growth {
    std::optional<SharedMemory> ring;
    bool fits = true;
    bool no_memory = false;
  };
  Growth Prepare(uint32_t width, uint32_t height);

  // Copies a frame of rows of width x 4 bytes in the agreed format into a free slot and
  // publishes it. Prepare must have returned no ring, or the ring it returned must have been
  // announced.
  void Publish(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t epoch);

  // Sets latest to none (starting or releasing a game).
  void Clear();

  PixelFormat format() const { return format_; }

 private:
  uint8_t* Current() const { return rings_.empty() ? nullptr : rings_.back().data; }

  PixelFormat format_;
  std::vector<SharedMemory> rings_;
  uint64_t slot_bytes_ = 0;
  uint64_t sequence_ = 0;
};

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_RING_H_
