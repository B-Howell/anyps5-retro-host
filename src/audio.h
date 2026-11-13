// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_AUDIO_H_
#define ANYPS5_RETRO_HOST_AUDIO_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

struct ma_device;

namespace retro_host {

// The game's sound, played by the host itself (protocol.md "Processes"). Volume and mute last
// for the host's whole run; a paused game is silent.
class AudioOutput {
 public:
  AudioOutput();
  ~AudioOutput();

  // Opens the system's default output for interleaved stereo 16-bit samples at sample_rate. A
  // machine with no output device plays nothing, and the game still runs.
  void Open(double sample_rate);
  void Close();

  // Queues frames of stereo samples from the core; what does not fit is dropped.
  void Write(const int16_t* samples, size_t frames);

  void SetVolume(float level, bool muted);
  void SetPaused(bool paused);

  struct Device;

 private:
  static void Fill(::ma_device* device, void* out, const void* in, uint32_t frames);
  std::mutex device_mutex_;
  std::unique_ptr<Device> device_;
  std::atomic<float> level_{1.0f};
  std::atomic<bool> muted_{false};
  std::atomic<bool> paused_{false};
};

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_AUDIO_H_
