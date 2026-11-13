// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "ring.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace retro_host {
namespace {

constexpr size_t kLatest = 24;
constexpr size_t kReading = 28;

std::atomic<uint32_t>& Atomic(uint8_t* ring, size_t offset) {
  static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);
  return *reinterpret_cast<std::atomic<uint32_t>*>(ring + offset);
}

void Put32(uint8_t* at, uint32_t value) { std::memcpy(at, &value, 4); }
void Put64(uint8_t* at, uint64_t value) { std::memcpy(at, &value, 8); }

}  // namespace

FrameRings::Growth FrameRings::Prepare(uint32_t width, uint32_t height) {
  if (width == 0 || height == 0 || width > kMaxFrameSide || height > kMaxFrameSide) return {std::nullopt, false};
  const uint64_t need = static_cast<uint64_t>(width) * height * 4;
  if (need <= slot_bytes_) return {};
  const uint64_t slot_bytes = slot_bytes_ == 0 ? need : std::min(std::max(2 * slot_bytes_, need), kMaxSlotBytes);
  auto memory = CreateSharedMemory(kHeaderBytes + kSlotCount * slot_bytes);
  if (!memory) return {std::nullopt, true, true};
  uint8_t* ring = memory->data;
  std::memset(ring, 0, kHeaderBytes);
  Put32(ring + 0, kRingMagic);
  Put32(ring + 4, kRingVersion);
  Put32(ring + 8, kSlotCount);
  Put32(ring + 12, kHeaderBytes);
  Put64(ring + 16, slot_bytes);
  Put32(ring + 32, static_cast<uint32_t>(format_));
  Atomic(ring, kLatest).store(kNoSlot);
  Atomic(ring, kReading).store(kNoSlot);
  rings_.push_back(*memory);
  slot_bytes_ = slot_bytes;
  return {memory, true};
}

void FrameRings::Publish(const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t epoch) {
  uint8_t* ring = Current();
  if (!ring) return;
  const uint32_t latest = Atomic(ring, kLatest).load();
  const uint32_t reading = Atomic(ring, kReading).load();
  uint32_t slot = 0;
  while (slot == latest || slot == reading) ++slot;
  const uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
  std::memcpy(ring + kHeaderBytes + slot * slot_bytes_, pixels, bytes);
  bool visible = false;
  for (uint64_t at = 0; at < bytes && !visible; at += 4) {
    const uint8_t* pixel = pixels + at;
    visible = pixel[0] | pixel[1] | pixel[2];
  }
  uint8_t* fields = ring + 40 + 24 * slot;
  Put32(fields + 0, width);
  Put32(fields + 4, height);
  Put32(fields + 8, visible ? 1 : 0);
  Put32(fields + 12, epoch);
  Put64(fields + 16, ++sequence_);
  Atomic(ring, kLatest).store(slot);
}

void FrameRings::Clear() {
  if (uint8_t* ring = Current()) Atomic(ring, kLatest).store(kNoSlot);
}

}  // namespace retro_host
