#ifndef RUNNER_PS1_NETPLAY_TIMELINE_H_
#define RUNNER_PS1_NETPLAY_TIMELINE_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace ps1_netplay {

struct PadInput {
  uint16_t buttons = 0;
  std::array<int16_t, 4> axes{};

  bool operator==(const PadInput& other) const {
    return buttons == other.buttons && axes == other.axes;
  }
};

struct FrameInput {
  uint32_t frame;
  PadInput input;

  bool operator==(const FrameInput& other) const {
    return frame == other.frame && input == other.input;
  }
};

// The emulator worker owns the cursor; callers serialize exchanges with it.
// Prediction is opt-in for workers that can checkpoint and replay. The
// default waits for confirmed inputs before advancing the emulator.
class Timeline {
 public:
  static constexpr uint32_t kDelay = 2;
  static constexpr uint32_t kHistory = 64;
  static constexpr uint32_t kMaxFrame = 0xfffffffe;
  static constexpr uint32_t kMaxPacket = 8;
  static constexpr uint32_t kStandardPrediction = 8;
  static constexpr uint32_t kMaxPrediction = 16;

  explicit Timeline(unsigned seat, uint32_t prediction_window = 0)
      : seat_(seat), prediction_window_(prediction_window) {
    if (seat > 1 || prediction_window > kMaxPrediction)
      throw std::invalid_argument("Invalid PS1 netplay seat or prediction window");
    for (uint32_t frame = 0; frame < kDelay; ++frame) local_.emplace(frame, PadInput{});
  }

  void Start() { started_ = true; }
  bool Hold() {
    if (prepared_) return false;
    started_ = false;
    return true;
  }
  void Reset() { *this = Timeline(seat_, prediction_window_); }
  bool SetPredictionWindow(uint32_t window) {
    if (started_ || frame_ || window > kMaxPrediction) return false;
    prediction_window_ = window;
    return true;
  }
  uint32_t prediction_window() const { return prediction_window_; }
  uint32_t prediction_depth() const { return cursor_ - NextUnconfirmed(); }
  // The transport reports a monotonic frame. The emulator cursor can rewind.
  uint32_t frame() const { return frame_; }
  uint32_t cursor() const { return cursor_; }
  std::optional<uint32_t> confirmed() const {
    const auto next = NextUnconfirmed();
    return next ? std::optional<uint32_t>(next - 1) : std::nullopt;
  }
  std::optional<uint32_t> received() const { return received_; }
  bool waiting() const { return !started_ || (!prepared_ && !CanAdvance()); }

  bool needs_checkpoint() const { return prepared_ && predictions_.count(cursor_) != 0; }
  std::optional<uint32_t> correction() const { return prepared_ ? std::nullopt : correction_; }

  // After restoring the checkpoint immediately before this frame, retry
  // through the same local inputs and the newly received remote inputs.
  bool Rewind(uint32_t frame) {
    if (correction() != frame) return false;
    cursor_ = frame;
    predictions_.erase(predictions_.lower_bound(frame), predictions_.end());
    correction_.reset();
    return true;
  }

  bool Receive(const std::vector<FrameInput>& samples, std::optional<uint32_t> acknowledged) {
    if (acknowledged && *acknowledged > local_.rbegin()->first) return false;
    const auto oldest = frame_ > kHistory ? frame_ - kHistory : 0;
    std::map<uint32_t, PadInput> incoming;
    for (const auto& sample : samples) {
      if (sample.frame > kMaxFrame || (sample.frame > frame_ && sample.frame - frame_ > kHistory)) return false;
      if (sample.frame < oldest) continue;
      const auto existing = remote_.find(sample.frame);
      if (existing != remote_.end()) {
        if (!(existing->second == sample.input)) return false;
        continue;
      }
      const auto inserted = incoming.emplace(sample.frame, sample.input);
      if (!(inserted.first->second == sample.input)) return false;
    }
    if (acknowledged && (!acknowledged_ || *acknowledged > *acknowledged_)) acknowledged_ = acknowledged;
    for (const auto& [frame, input] : incoming) {
      const auto predicted = predictions_.find(frame);
      if (predicted != predictions_.end() && !(predicted->second == input) &&
          (!correction_ || frame < *correction_)) {
        correction_ = frame;
      }
    }
    remote_.merge(incoming);
    auto next = received_ ? *received_ + 1 : 0;
    while (remote_.count(next)) received_ = next++;
    Prune();
    return true;
  }

  bool Prepare(const PadInput& local, std::array<PadInput, 2>& ports) {
    if (!started_ || prepared_ || !CanSample()) return false;
    local_.emplace(cursor_ + kDelay, local);
    if (!CanAdvance()) return false;
    const auto other = remote_.lower_bound(cursor_);
    PadInput remote;
    if (other != remote_.end() && other->first == cursor_) {
      remote = other->second;
    } else {
      if (other != remote_.begin()) remote = std::prev(other)->second;
      predictions_.emplace(cursor_, remote);
    }
    ports[seat_] = local_.at(cursor_);
    ports[1 - seat_] = remote;
    prepared_ = true;
    return true;
  }

  // Presentation can sample ahead of replay. The authoritative Prepare call
  // keeps the same delayed local sample and resolves the remote pad again.
  bool Preview(uint32_t frame, const PadInput& local, std::array<PadInput, 2>& ports) {
    const auto next = NextUnconfirmed();
    if (!started_ || frame < frame_ || frame < next || frame - next >= prediction_window_ ||
        !CanSampleAt(frame)) return false;
    const auto current = local_.find(frame);
    if (current == local_.end()) return false;
    local_.emplace(frame + kDelay, local);
    ports[seat_] = current->second;
    const auto remote = remote_.upper_bound(frame);
    ports[1 - seat_] = remote == remote_.begin() ? PadInput{} : std::prev(remote)->second;
    return true;
  }

  void Commit() {
    if (!prepared_) throw std::logic_error("PS1 netplay frame was not prepared");
    frame_ = std::max(frame_, ++cursor_);
    prepared_ = false;
    Prune();
  }

  std::vector<FrameInput> LocalPacket() {
    const auto latest = local_.rbegin()->first;
    const auto unacknowledged = acknowledged_ ? *acknowledged_ + 1 : 0;
    const auto overlap = latest >= kMaxPacket - 1 ? latest - (kMaxPacket - 1) : 0;
    auto first = std::min(unacknowledged, overlap);
    // A high RTT can leave more than one packet unacknowledged even without
    // loss. Keep new inputs flowing while reserving every other packet for
    // the oldest gap. Both packet types remain contiguous.
    if (prediction_window_ && unacknowledged < overlap) {
      if (send_fresh_next_) first = overlap;
      send_fresh_next_ = !send_fresh_next_;
    } else {
      send_fresh_next_ = true;
    }
    std::vector<FrameInput> samples;
    samples.reserve(kMaxPacket);
    for (auto it = local_.lower_bound(first); it != local_.end() && samples.size() < kMaxPacket; ++it)
      samples.push_back({it->first, it->second});
    return samples;
  }

 private:
  uint32_t NextReceived() const { return received_ ? *received_ + 1 : 0; }

  uint32_t NextUnconfirmed() const {
    return std::min({cursor_, NextReceived(), correction_.value_or(cursor_)});
  }

  bool CanAdvance() const {
    if (correction_ || !CanSample()) return false;
    const auto next = NextReceived();
    return cursor_ < next || cursor_ - next < prediction_window_;
  }

  bool CanSample() const {
    return CanSampleAt(cursor_);
  }

  bool CanSampleAt(uint32_t frame) const {
    if (frame > kMaxFrame - kDelay) return false;
    const auto unacknowledged = acknowledged_ ? *acknowledged_ + 1 : 0;
    const auto target = frame + kDelay;
    return target < unacknowledged || target - unacknowledged < kHistory;
  }

  void Prune() {
    const auto next_unconfirmed = NextUnconfirmed();
    predictions_.erase(predictions_.begin(), predictions_.lower_bound(next_unconfirmed));
    const auto oldest_remote = frame_ > kHistory ? frame_ - kHistory : 0;
    remote_.erase(remote_.begin(), remote_.lower_bound(oldest_remote));
    if (acknowledged_) {
      const auto overlap = *acknowledged_ >= kMaxPacket - 1 ? *acknowledged_ - (kMaxPacket - 1) : 0;
      const auto oldest_local = std::min(next_unconfirmed, overlap);
      local_.erase(local_.begin(), local_.lower_bound(oldest_local));
    }
  }

  unsigned seat_;
  uint32_t prediction_window_;
  bool started_ = false;
  bool prepared_ = false;
  bool send_fresh_next_ = true;
  uint32_t frame_ = 0;
  uint32_t cursor_ = 0;
  std::optional<uint32_t> correction_;
  std::optional<uint32_t> acknowledged_;
  std::optional<uint32_t> received_;
  std::map<uint32_t, PadInput> local_;
  std::map<uint32_t, PadInput> remote_;
  std::map<uint32_t, PadInput> predictions_;
};

}  // namespace ps1_netplay

#endif  // RUNNER_PS1_NETPLAY_TIMELINE_H_
