#ifndef ANYPS5_PS1_CHECKPOINT_H_
#define ANYPS5_PS1_CHECKPOINT_H_

#include "ps1_netplay_timeline.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace anyps5::retro::ps1_checkpoint {

constexpr size_t kMaximumBytes = 12 * 1024 * 1024;
constexpr size_t kPrefixBytes = 40;
using Digest = std::array<uint8_t, 32>;
using Bytes = std::vector<uint8_t>;
using Save = size_t (*)(void*, size_t, const uint8_t*);
using Load = bool (*)(const void*, size_t, const uint8_t*);

struct Snapshot { uint32_t frame; Bytes state; };
struct Recovery { int64_t source = 0; Digest digest{}; Bytes state; };
struct Capture { Snapshot snapshot; Bytes private_state; };
struct Hooks { Save blank = nullptr, private_save = nullptr; Load load = nullptr; size_t capacity = 0; };

inline std::optional<int64_t> NextIdentity() {
  static std::atomic<int64_t> serial{0};
  auto previous = serial.load();
  do {
    if (previous == std::numeric_limits<int64_t>::max()) return std::nullopt;
  } while (!serial.compare_exchange_weak(previous, previous + 1));
  return previous + 1;
}

// Invoked only at a core-owned frame boundary. The public exporter strips
// memory cards; a parked ordinary game additionally retains a private recovery.
inline std::optional<Capture> CaptureState(Save save, Save private_save, size_t capacity,
                                          const Digest& digest, uint64_t frame, bool park) {
  if (!save || (park && !private_save) || capacity < kPrefixBytes || capacity > kMaximumBytes ||
      frame > ps1_netplay::Timeline::kMaxFrame) return std::nullopt;
  Bytes bytes(capacity);
  const auto used = save(bytes.data(), bytes.size(), digest.data());
  if (!used || used > bytes.size()) return std::nullopt;
  bytes.resize(used);
  Bytes recovery;
  if (park) {
    recovery.resize(capacity);
    const auto private_size = private_save(recovery.data(), recovery.size(), digest.data());
    if (!private_size || private_size > recovery.size()) return std::nullopt;
    recovery.resize(private_size);
  }
  return Capture{Snapshot{static_cast<uint32_t>(frame), std::move(bytes)}, std::move(recovery)};
}

// App-thread requests and polling never execute the core. Service is called by
// its emulation worker; there is one outstanding request, including its result.
class Mailbox {
 public:
  using Reply = std::variant<std::monostate, bool, Snapshot>;
  struct Effects { bool refresh_memory = false, flush_audio = false; };

  void Configure(Hooks hooks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || configured_) return;
    configured_ = true;
    hooks_ = hooks;
  }
  bool ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Ready();
  }
  int64_t source() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Ready() ? source_ : 0;
  }
  bool parked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return parked_;
  }
  bool busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return job_.has_value() || parked_;
  }
  std::optional<int64_t> RequestCapture(const Digest& digest, bool park) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Ready() || job_ || parked_ || (park && (!hooks_.private_save || !hooks_.load))) return std::nullopt;
    return Queue(park ? Kind::kPark : Kind::kCapture, digest);
  }
  std::optional<int64_t> RequestResume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Ready() || !parked_ || job_ || recovery_.empty() || !hooks_.load) return std::nullopt;
    return Queue(Kind::kResume, recovery_digest_);
  }
  std::optional<int64_t> RequestImport(const Digest& digest, Bytes state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Ready() || job_ || parked_ || !hooks_.load || state.empty() || state.size() > kMaximumBytes)
      return std::nullopt;
    const auto ticket = Queue(Kind::kImport, digest);
    if (ticket) job_->import = std::move(state);
    return ticket;
  }
  Reply Take(int64_t ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || !job_ || job_->id != ticket || job_->cancelled) return false;
    if (!job_->reply) return std::monostate{};
    auto result = std::move(*job_->reply);
    job_.reset();
    return result;
  }
  void Cancel(int64_t ticket) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!job_ || job_->id != ticket) return;
    if (job_->running && !job_->reply) { job_->cancelled = true; return; }
    if (job_->kind == Kind::kPark) { parked_ = false; recovery_.clear(); }
    job_.reset();
  }
  Effects Service(uint64_t frame, const std::function<void()>& validate_restore = {}) {
    return Service([frame] { return frame; }, validate_restore);
  }
  Effects Service(const std::function<uint64_t()>& frame, const std::function<void()>& validate_restore = {}) {
    Hooks hooks;
    Kind kind;
    int64_t id;
    Digest digest;
    Bytes recovery;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!Ready() || !job_ || job_->running) return {};
      job_->running = true;
      hooks = hooks_; kind = job_->kind; id = job_->id; digest = job_->digest;
      if (kind == Kind::kResume) recovery = recovery_;
      if (kind == Kind::kImport) recovery = std::move(job_->import);
    }
    const bool loading = kind == Kind::kResume || kind == Kind::kImport;
    auto captured = loading ? std::nullopt :
        CaptureState(hooks.blank, hooks.private_save, hooks.capacity, digest, frame(), kind == Kind::kPark);
    const bool restored = loading && hooks.load(recovery.data(), recovery.size(), digest.data());
    if (restored && validate_restore) validate_restore();
    Effects effects{loading, loading};
    std::lock_guard<std::mutex> lock(mutex_);
    if (kind == Kind::kResume && restored) { parked_ = false; recovery_.clear(); }
    if (stopped_ || !job_ || job_->id != id) return effects;
    if (job_->cancelled) { job_.reset(); return effects; }
    if (captured) {
      if (kind == Kind::kPark) {
        recovery_ = std::move(captured->private_state);
        recovery_digest_ = digest;
        parked_ = true;
        effects.flush_audio = true;
      }
      job_->reply = std::move(captured->snapshot);
    } else {
      job_->reply = restored;
    }
    return effects;
  }
  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true; parked_ = false; job_.reset(); recovery_.clear();
  }

  bool Detach(int64_t source, Recovery& recovery) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Ready() || source != source_ || !parked_ || job_ || recovery_.empty()) return false;
    recovery = {source_, recovery_digest_, std::move(recovery_)};
    stopped_ = true;
    return true;
  }

 private:
  enum class Kind { kCapture, kPark, kResume, kImport };
  struct Job {
    int64_t id;
    Kind kind;
    Digest digest;
    bool running = false, cancelled = false;
    std::optional<Reply> reply;
    Bytes import;
  };
  bool Ready() const {
    return source_ > 0 && !stopped_ && hooks_.blank && hooks_.capacity >= kPrefixBytes && hooks_.capacity <= kMaximumBytes;
  }
  std::optional<int64_t> Queue(Kind kind, const Digest& digest) {
    const auto id = NextIdentity();
    if (!id) return std::nullopt;
    job_ = Job{*id, kind, digest, false, false, std::nullopt, {}};
    return id;
  }
  mutable std::mutex mutex_;
  const int64_t source_ = NextIdentity().value_or(0);
  Hooks hooks_;
  bool configured_ = false, stopped_ = false, parked_ = false;
  std::optional<Job> job_;
  Bytes recovery_;
  Digest recovery_digest_{};
};

}  // namespace anyps5::retro::ps1_checkpoint

#endif  // ANYPS5_PS1_CHECKPOINT_H_
