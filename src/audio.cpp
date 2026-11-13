// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "audio.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace retro_host {

struct AudioOutput::Device {
  ma_device device;
  ma_pcm_rb buffer;
  AudioOutput* owner = nullptr;
};

void AudioOutput::Fill(ma_device* device, void* out, const void*, ma_uint32 frames) {
  auto* state = static_cast<AudioOutput::Device*>(device->pUserData);
  auto* samples = static_cast<int16_t*>(out);
  const bool silent = state->owner->paused_;
  ma_uint32 done = 0;
  while (done < frames) {
    ma_uint32 count = frames - done;
    void* source = nullptr;
    if (ma_pcm_rb_acquire_read(&state->buffer, &count, &source) != MA_SUCCESS || count == 0) break;
    if (!silent) std::memcpy(samples + done * 2, source, count * 2 * sizeof(int16_t));
    ma_pcm_rb_commit_read(&state->buffer, count);
    done += count;
  }
  if (silent) done = 0;
  std::memset(samples + done * 2, 0, (frames - done) * 2 * sizeof(int16_t));
  const float gain = state->owner->muted_ ? 0.0f : state->owner->level_.load();
  if (gain >= 1.0f) return;
  for (ma_uint32 i = 0; i < done * 2; ++i) samples[i] = static_cast<int16_t>(samples[i] * gain);
}

AudioOutput::AudioOutput() = default;

AudioOutput::~AudioOutput() { Close(); }

void AudioOutput::Open(double sample_rate) {
  Close();
  std::lock_guard<std::mutex> lock(device_mutex_);
  if (!(sample_rate >= 8000 && sample_rate <= 192000)) return;
  auto device = std::make_unique<Device>();
  device->owner = this;
  const auto rate = static_cast<ma_uint32>(std::lround(sample_rate));
  if (ma_pcm_rb_init(ma_format_s16, 2, rate / 5, nullptr, nullptr, &device->buffer) != MA_SUCCESS) return;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_s16;
  config.playback.channels = 2;
  config.sampleRate = rate;
  config.dataCallback = &AudioOutput::Fill;
  config.pUserData = device.get();
  if (ma_device_init(nullptr, &config, &device->device) != MA_SUCCESS) {
    ma_pcm_rb_uninit(&device->buffer);
    return;
  }
  if (ma_device_start(&device->device) != MA_SUCCESS) {
    ma_device_uninit(&device->device);
    ma_pcm_rb_uninit(&device->buffer);
    return;
  }
  device_ = std::move(device);
}

void AudioOutput::Close() {
  std::lock_guard<std::mutex> lock(device_mutex_);
  if (!device_) return;
  ma_device_uninit(&device_->device);
  ma_pcm_rb_uninit(&device_->buffer);
  device_.reset();
}

void AudioOutput::Write(const int16_t* samples, size_t frames) {
  std::lock_guard<std::mutex> lock(device_mutex_);
  if (!device_ || paused_) return;
  while (frames > 0) {
    ma_uint32 count = static_cast<ma_uint32>(std::min<size_t>(frames, 1u << 16));
    void* target = nullptr;
    if (ma_pcm_rb_acquire_write(&device_->buffer, &count, &target) != MA_SUCCESS || count == 0) return;
    std::memcpy(target, samples, count * 2 * sizeof(int16_t));
    ma_pcm_rb_commit_write(&device_->buffer, count);
    samples += count * 2;
    frames -= count;
  }
}

void AudioOutput::SetVolume(float level, bool muted) {
  level_ = level;
  muted_ = muted;
}

void AudioOutput::SetPaused(bool paused) { paused_ = paused; }

}  // namespace retro_host
