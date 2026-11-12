#include "retro_ipc_frames.h"

#include <cstring>
#include <new>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace anyps5::retro_ipc {

#if defined(_WIN32)

SharedMemory::~SharedMemory() {
  if (data_) UnmapViewOfFile(data_);
  if (handle_) CloseHandle(handle_);
}

std::unique_ptr<SharedMemory> SharedMemory::Create(size_t size) {
  const auto high = static_cast<DWORD>(static_cast<uint64_t>(size) >> 32);
  const auto low = static_cast<DWORD>(size & 0xFFFFFFFFu);
  const HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, high, low, nullptr);
  if (!handle) return nullptr;
  auto memory = Map(handle, size);
  if (!memory) CloseHandle(handle);
  return memory;
}

std::unique_ptr<SharedMemory> SharedMemory::Map(NativeHandle handle, size_t size) {
  if (!handle || size < kFrameRingHeaderBytes) return nullptr;
  void* data = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
  if (!data) return nullptr;
  MEMORY_BASIC_INFORMATION region{};
  if (!VirtualQuery(data, &region, sizeof(region)) || region.RegionSize < size) {
    UnmapViewOfFile(data);
    return nullptr;
  }
  auto memory = std::unique_ptr<SharedMemory>(new SharedMemory());
  memory->data_ = static_cast<uint8_t*>(data);
  memory->size_ = size;
  memory->handle_ = handle;
  return memory;
}

#else

SharedMemory::~SharedMemory() {
  if (data_) munmap(data_, size_);
  if (handle_ >= 0) close(handle_);
}

std::unique_ptr<SharedMemory> SharedMemory::Create(size_t size) {
#if defined(__linux__) && defined(SYS_memfd_create)
  const int fd = static_cast<int>(syscall(SYS_memfd_create, "anyps5-retro-frames", 1u));
#else
  char name[64];
  std::snprintf(name, sizeof(name), "/anyps5-retro-%d-%p", static_cast<int>(getpid()), static_cast<void*>(&name));
  const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) shm_unlink(name);
#endif
  if (fd < 0) return nullptr;
  if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
    close(fd);
    return nullptr;
  }
  auto memory = Map(fd, size);
  if (!memory) close(fd);
  return memory;
}

std::unique_ptr<SharedMemory> SharedMemory::Map(NativeHandle handle, size_t size) {
  if (handle < 0 || size < kFrameRingHeaderBytes) return nullptr;
  const auto end = lseek(handle, 0, SEEK_END);
  if (end < 0 || static_cast<size_t>(end) < size) return nullptr;
  void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
  if (data == MAP_FAILED) return nullptr;
  auto memory = std::unique_ptr<SharedMemory>(new SharedMemory());
  memory->data_ = static_cast<uint8_t*>(data);
  memory->size_ = size;
  memory->handle_ = handle;
  return memory;
}

#endif

size_t FrameRingBytes(size_t slot_bytes) { return kFrameRingHeaderBytes + slot_bytes * kFrameRingSlots; }

FrameRingWriter::FrameRingWriter(std::unique_ptr<SharedMemory> memory, uint32_t epoch)
    : memory_(std::move(memory)), header_(new (memory_->data()) FrameRingHeader{}) {
  header_->magic = kFrameRingMagic;
  header_->version = kProtocolVersion;
  header_->slot_count = kFrameRingSlots;
  header_->header_bytes = static_cast<uint32_t>(kFrameRingHeaderBytes);
  header_->slot_bytes = (memory_->size() - kFrameRingHeaderBytes) / kFrameRingSlots;
  header_->latest.store(kNoSlot);
  header_->reading.store(kNoSlot);
  header_->epoch.store(epoch);
}

std::unique_ptr<FrameRingWriter> FrameRingWriter::Create(size_t slot_bytes, uint32_t epoch) {
  auto memory = SharedMemory::Create(FrameRingBytes(slot_bytes));
  if (!memory) return nullptr;
  return std::make_unique<FrameRingWriter>(std::move(memory), epoch);
}

size_t FrameRingWriter::slot_bytes() const { return static_cast<size_t>(header_->slot_bytes); }
uint32_t FrameRingWriter::epoch() const { return header_->epoch.load(); }

bool FrameRingWriter::Write(const uint8_t* rgba, size_t width, size_t height, bool visible) {
  const auto bytes = width * height * 4;
  if (!rgba || !width || !height || bytes > header_->slot_bytes) return false;
  const auto latest = header_->latest.load();
  const auto reading = header_->reading.load();
  uint32_t slot = 0;
  while (slot == latest || slot == reading) ++slot;
  std::memcpy(memory_->data() + kFrameRingHeaderBytes + slot * header_->slot_bytes, rgba, bytes);
  header_->slots[slot] = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), visible ? 1u : 0u, 0,
                          ++sequence_};
  header_->latest.store(slot, std::memory_order_release);
  return true;
}

uint32_t FrameRingWriter::Clear() {
  header_->latest.store(kNoSlot);
  return header_->epoch.fetch_add(1) + 1;
}

FrameRingReader::FrameRingReader(std::unique_ptr<SharedMemory> memory)
    : memory_(std::move(memory)), header_(reinterpret_cast<FrameRingHeader*>(memory_->data())) {}

std::unique_ptr<FrameRingReader> FrameRingReader::Attach(std::unique_ptr<SharedMemory> memory) {
  if (!memory) return nullptr;
  const auto* header = reinterpret_cast<const FrameRingHeader*>(memory->data());
  if (header->magic != kFrameRingMagic || header->version != kProtocolVersion ||
      header->slot_count != kFrameRingSlots || header->header_bytes != kFrameRingHeaderBytes ||
      FrameRingBytes(static_cast<size_t>(header->slot_bytes)) > memory->size())
    return nullptr;
  return std::unique_ptr<FrameRingReader>(new FrameRingReader(std::move(memory)));
}

bool FrameRingReader::Acquire(FrameView& view) {
  uint32_t slot;
  while (true) {
    slot = header_->latest.load(std::memory_order_acquire);
    if (slot >= kFrameRingSlots) return false;
    header_->reading.store(slot);
    if (header_->latest.load() == slot) break;
  }
  const auto meta = header_->slots[slot];
  if (!meta.width || !meta.height ||
      static_cast<uint64_t>(meta.width) * meta.height * 4 > header_->slot_bytes)
    return false;
  view = {memory_->data() + kFrameRingHeaderBytes + slot * header_->slot_bytes, meta.width, meta.height,
          header_->epoch.load(), meta.sequence, meta.visible != 0};
  return true;
}

}  // namespace anyps5::retro_ipc
