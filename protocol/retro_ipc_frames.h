#ifndef ANYPS5_RETRO_IPC_FRAMES_H_
#define ANYPS5_RETRO_IPC_FRAMES_H_

#include "retro_ipc_transport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace anyps5::retro_ipc {

// A mapping of an anonymous shared memory object. The host creates it; the
// launcher maps the same object from a duplicated handle (Windows) or a
// descriptor received over the socket (POSIX).
class SharedMemory {
 public:
  ~SharedMemory();
  SharedMemory(const SharedMemory&) = delete;
  SharedMemory& operator=(const SharedMemory&) = delete;

  static std::unique_ptr<SharedMemory> Create(size_t size);
  static std::unique_ptr<SharedMemory> Map(NativeHandle handle, size_t size);

  uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  NativeHandle handle() const { return handle_; }

 private:
  SharedMemory() = default;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
#if defined(_WIN32)
  NativeHandle handle_ = nullptr;
#else
  NativeHandle handle_ = -1;
#endif
};

constexpr uint32_t kFrameRingMagic = 0x46523541;
constexpr uint32_t kFrameRingSlots = 3;
constexpr uint32_t kNoSlot = UINT32_MAX;
constexpr size_t kFrameRingHeaderBytes = 4096;

struct FrameSlot {
  uint32_t width;
  uint32_t height;
  uint32_t visible;
  uint32_t reserved;
  uint64_t sequence;
};

// Lives at the start of the shared memory. One writer (the host) and one
// reader (the launcher's raster thread). The writer fills a slot that is
// neither the newest nor the one the reader has pinned, then publishes it as
// the newest, so neither side ever waits for the other.
struct FrameRingHeader {
  uint32_t magic;
  uint32_t version;
  uint32_t slot_count;
  uint32_t header_bytes;
  uint64_t slot_bytes;
  std::atomic<uint32_t> latest;
  std::atomic<uint32_t> reading;
  std::atomic<uint32_t> epoch;
  uint32_t reserved;
  FrameSlot slots[kFrameRingSlots];
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(FrameRingHeader) <= kFrameRingHeaderBytes);

size_t FrameRingBytes(size_t slot_bytes);

class FrameRingWriter {
 public:
  explicit FrameRingWriter(std::unique_ptr<SharedMemory> memory, uint32_t epoch);
  static std::unique_ptr<FrameRingWriter> Create(size_t slot_bytes, uint32_t epoch);

  size_t slot_bytes() const;
  uint32_t epoch() const;
  const SharedMemory& memory() const { return *memory_; }
  bool Write(const uint8_t* rgba, size_t width, size_t height, bool visible);
  uint32_t Clear();

 private:
  std::unique_ptr<SharedMemory> memory_;
  FrameRingHeader* header_;
  uint64_t sequence_ = 0;
};

struct FrameView {
  const uint8_t* pixels;
  size_t width, height;
  uint32_t epoch;
  uint64_t sequence;
  bool visible;
};

class FrameRingReader {
 public:
  static std::unique_ptr<FrameRingReader> Attach(std::unique_ptr<SharedMemory> memory);
  bool Acquire(FrameView& view);

 private:
  explicit FrameRingReader(std::unique_ptr<SharedMemory> memory);
  std::unique_ptr<SharedMemory> memory_;
  FrameRingHeader* header_;
};

}  // namespace anyps5::retro_ipc

#endif  // ANYPS5_RETRO_IPC_FRAMES_H_
