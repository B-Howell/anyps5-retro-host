#ifndef RUNNER_PS2_VIRTUAL_LAN_H_
#define RUNNER_PS2_VIRTUAL_LAN_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

// Frontend side of retro_anyps5_dev9_bind_v1. Frames carry no FCS.
class Ps2VirtualLan {
 public:
  using Mac = std::array<uint8_t, 6>;
  struct Descriptor {
    void* opaque;
    uint8_t mac[6];
    bool (*send)(void*, const void*, size_t);
    size_t (*recv)(void*, void*, size_t);
  };
  using Bind = bool (*)(const Descriptor*);
  static constexpr size_t kMinFrame = 14;
  static constexpr size_t kMaxFrame = 1514;
  static constexpr size_t kQueueSize = 64;
  static constexpr size_t kBatchSize = 16;

  explicit Ps2VirtualLan(Mac mac) : mac_(mac) {}

  static bool ValidMac(const Mac& mac) { return !(mac[0] & 1) && mac != Mac{}; }

  // Called on the emulator worker before loading and after unloading content.
  bool Attach(Bind bind) {
    if (!bind || bound_ || closed_ || !ValidMac(mac_)) return false;
    Descriptor descriptor{
        this, {},
        [](void* opaque, const void* bytes, size_t size) {
          return static_cast<Ps2VirtualLan*>(opaque)->Send(bytes, size);
        },
        [](void* opaque, void* bytes, size_t capacity) {
          return static_cast<Ps2VirtualLan*>(opaque)->Receive(bytes, capacity);
        },
    };
    std::copy(mac_.begin(), mac_.end(), descriptor.mac);
    if (!bind(&descriptor)) return false;
    bound_ = bind;
    return true;
  }

  bool Detach() {
    if (!bound_) return true;
    if (!bound_(nullptr)) return false;
    bound_ = nullptr;
    ClearFrames();
    return true;
  }

  bool Accepts(const void* bytes, size_t size) const {
    if (!bytes || size < kMinFrame || size > kMaxFrame) return false;
    const auto* frame = static_cast<const uint8_t*>(bytes);
    return (frame[0] & 1) || std::equal(mac_.begin(), mac_.end(), frame);
  }

  bool Inject(const void* bytes, size_t size) {
    return !closed_ && Accepts(bytes, size) && incoming_.Push(bytes, size, closed_);
  }

  size_t Drain(void* bytes, size_t capacity) {
    return outgoing_.Pop(bytes, capacity, closed_);
  }

  void Close() {
    closed_ = true;
    ClearFrames();
  }

  void ClearFrames() {
    incoming_.Clear();
    outgoing_.Clear();
  }

 private:
  class Queue {
   public:
    bool Push(const void* bytes, size_t size, const std::atomic<bool>& closed) {
      std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
      if (!lock || closed || count_ == slots_.size()) return false;
      auto& entry = slots_[(head_ + count_) % slots_.size()];
      std::memcpy(entry.bytes.data(), bytes, size);
      entry.size = size;
      ++count_;
      return true;
    }

    size_t Pop(void* bytes, size_t capacity, const std::atomic<bool>& closed) {
      std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
      if (!lock || closed || !bytes || count_ == 0 || capacity < slots_[head_].size) return 0;
      const auto& entry = slots_[head_];
      std::memcpy(bytes, entry.bytes.data(), entry.size);
      head_ = (head_ + 1) % slots_.size();
      --count_;
      return entry.size;
    }

    void Clear() {
      std::lock_guard<std::mutex> lock(mutex_);
      head_ = count_ = 0;
    }

   private:
    struct Frame {
      std::array<uint8_t, kMaxFrame> bytes;
      size_t size;
    };
    std::mutex mutex_;
    std::array<Frame, kQueueSize> slots_{};
    size_t head_ = 0, count_ = 0;
  };

  bool Send(const void* bytes, size_t size) {
    if (closed_ || !bytes || size < kMinFrame || size > kMaxFrame) return false;
    const auto* source = static_cast<const uint8_t*>(bytes) + 6;
    return std::equal(mac_.begin(), mac_.end(), source) && outgoing_.Push(bytes, size, closed_);
  }

  size_t Receive(void* bytes, size_t capacity) {
    return incoming_.Pop(bytes, capacity, closed_);
  }

  const Mac mac_;
  std::atomic<bool> closed_{false};
  Queue incoming_;
  Queue outgoing_;
  Bind bound_ = nullptr;
};

#endif  // RUNNER_PS2_VIRTUAL_LAN_H_
