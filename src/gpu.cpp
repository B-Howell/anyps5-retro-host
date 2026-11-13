// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include "gpu.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace retro_host {
namespace {

#ifdef _WIN32
#define GL_CALL __stdcall
#else
#define GL_CALL
#endif

// Values from the Khronos OpenGL registry, api/GL/glcorearb.h.
constexpr unsigned kFramebuffer = 0x8D40;
constexpr unsigned kReadFramebuffer = 0x8CA8;
constexpr unsigned kRenderbuffer = 0x8D41;
constexpr unsigned kColorAttachment0 = 0x8CE0;
constexpr unsigned kDepthAttachment = 0x8D00;
constexpr unsigned kDepthStencilAttachment = 0x821A;
constexpr unsigned kRgba8 = 0x8058;
constexpr unsigned kDepthComponent24 = 0x81A6;
constexpr unsigned kDepth24Stencil8 = 0x88F0;
constexpr unsigned kFramebufferComplete = 0x8CD5;
constexpr unsigned kRgba = 0x1908;
constexpr unsigned kBgra = 0x80E1;
constexpr unsigned kUnsignedByte = 0x1401;
constexpr unsigned kPackAlignment = 0x0D05;
constexpr unsigned kPackRowLength = 0x0D02;
constexpr unsigned kPackSkipRows = 0x0D03;
constexpr unsigned kPackSkipPixels = 0x0D04;
constexpr unsigned kNoError = 0;
constexpr unsigned kPixelPackBuffer = 0x88EB;

}  // namespace

struct Gpu::Functions {
  void(GL_CALL* GenFramebuffers)(int, unsigned*);
  void(GL_CALL* DeleteFramebuffers)(int, const unsigned*);
  void(GL_CALL* BindFramebuffer)(unsigned, unsigned);
  void(GL_CALL* GenRenderbuffers)(int, unsigned*);
  void(GL_CALL* DeleteRenderbuffers)(int, const unsigned*);
  void(GL_CALL* BindRenderbuffer)(unsigned, unsigned);
  void(GL_CALL* RenderbufferStorage)(unsigned, unsigned, int, int);
  void(GL_CALL* FramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
  unsigned(GL_CALL* CheckFramebufferStatus)(unsigned);
  void(GL_CALL* ReadPixels)(int, int, int, int, unsigned, unsigned, void*);
  void(GL_CALL* PixelStorei)(unsigned, int);
  void(GL_CALL* BindBuffer)(unsigned, unsigned);
  unsigned(GL_CALL* GetError)();
};

bool Gpu::LoadFunctions() {
  auto gl = std::make_shared<Functions>();
  const auto load = [this](auto& function, const char* name) {
    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(Proc(name));
    return function != nullptr;
  };
  if (!load(gl->GenFramebuffers, "glGenFramebuffers") || !load(gl->DeleteFramebuffers, "glDeleteFramebuffers") ||
      !load(gl->BindFramebuffer, "glBindFramebuffer") || !load(gl->GenRenderbuffers, "glGenRenderbuffers") ||
      !load(gl->DeleteRenderbuffers, "glDeleteRenderbuffers") || !load(gl->BindRenderbuffer, "glBindRenderbuffer") ||
      !load(gl->RenderbufferStorage, "glRenderbufferStorage") ||
      !load(gl->FramebufferRenderbuffer, "glFramebufferRenderbuffer") ||
      !load(gl->CheckFramebufferStatus, "glCheckFramebufferStatus") || !load(gl->ReadPixels, "glReadPixels") ||
      !load(gl->PixelStorei, "glPixelStorei") || !load(gl->BindBuffer, "glBindBuffer") || !load(gl->GetError, "glGetError")) {
    return false;
  }
  gl_ = std::move(gl);
  return true;
}

bool Gpu::Reserve(uint32_t width, uint32_t height, bool depth, bool stencil) {
  if (!gl_ || width == 0 || height == 0) return false;
  if (framebuffer_ && width <= width_ && height <= height_ && depth == with_depth_ && stencil == with_stencil_) {
    return true;
  }
  width = std::max(width, width_);
  height = std::max(height, height_);
  ReleaseFramebuffer();
  gl_->GenFramebuffers(1, &framebuffer_);
  gl_->BindFramebuffer(kFramebuffer, framebuffer_);
  gl_->GenRenderbuffers(1, &color_);
  gl_->BindRenderbuffer(kRenderbuffer, color_);
  gl_->RenderbufferStorage(kRenderbuffer, kRgba8, static_cast<int>(width), static_cast<int>(height));
  gl_->FramebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, color_);
  if (depth || stencil) {
    gl_->GenRenderbuffers(1, &depth_);
    gl_->BindRenderbuffer(kRenderbuffer, depth_);
    gl_->RenderbufferStorage(kRenderbuffer, stencil ? kDepth24Stencil8 : kDepthComponent24, static_cast<int>(width),
                             static_cast<int>(height));
    gl_->FramebufferRenderbuffer(kFramebuffer, stencil ? kDepthStencilAttachment : kDepthAttachment, kRenderbuffer,
                                 depth_);
  }
  gl_->BindRenderbuffer(kRenderbuffer, 0);
  const bool complete = gl_->CheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
  gl_->BindFramebuffer(kFramebuffer, 0);
  if (!complete) {
    ReleaseFramebuffer();
    return false;
  }
  width_ = width;
  height_ = height;
  with_depth_ = depth;
  with_stencil_ = stencil;
  return true;
}

bool Gpu::Read(uint32_t width, uint32_t height, bool bgra, bool bottom_left, uint8_t* out) {
  if (!gl_ || !framebuffer_ || width > width_ || height > height_) return false;
  const size_t row = static_cast<size_t>(width) * 4;
  for (int pending = 0; pending < 16 && gl_->GetError() != kNoError; ++pending) {
  }
  gl_->BindBuffer(kPixelPackBuffer, 0);
  gl_->BindFramebuffer(kReadFramebuffer, framebuffer_);
  gl_->PixelStorei(kPackAlignment, 1);
  gl_->PixelStorei(kPackRowLength, 0);
  gl_->PixelStorei(kPackSkipRows, 0);
  gl_->PixelStorei(kPackSkipPixels, 0);
  gl_->ReadPixels(0, 0, static_cast<int>(width), static_cast<int>(height), bgra ? kBgra : kRgba, kUnsignedByte, out);
  gl_->BindFramebuffer(kReadFramebuffer, 0);
  if (gl_->GetError() != kNoError) return false;
  if (bottom_left) {
    for (uint32_t y = 0; y < height / 2; ++y) {
      std::swap_ranges(out + y * row, out + (y + 1) * row, out + (height - 1 - y) * row);
    }
  }
  for (size_t at = 3; at < row * height; at += 4) out[at] = 0xFF;
  return true;
}

void Gpu::ReleaseFramebuffer() {
  if (!gl_) return;
  if (framebuffer_) gl_->DeleteFramebuffers(1, &framebuffer_);
  if (color_) gl_->DeleteRenderbuffers(1, &color_);
  if (depth_) gl_->DeleteRenderbuffers(1, &depth_);
  framebuffer_ = color_ = depth_ = 0;
  width_ = height_ = 0;
}

}  // namespace retro_host
