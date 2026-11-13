// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#ifndef ANYPS5_RETRO_HOST_GPU_H_
#define ANYPS5_RETRO_HOST_GPU_H_

#include <cstdint>
#include <memory>

namespace retro_host {

// An offscreen OpenGL context, current on the game's thread, with the framebuffer a libretro core
// draws a hardware-rendered frame into (RETRO_ENVIRONMENT_SET_HW_RENDER). Each frame is read back
// into the frame ring, so the launcher draws it like any other.
class Gpu {
 public:
  virtual ~Gpu() = default;

  // A context of OpenGL major.minor, core profile when core is true; nullptr when the system has
  // none. The context is current on the calling thread.
  static std::unique_ptr<Gpu> Create(unsigned major, unsigned minor, bool core);

  // An OpenGL function for the core (retro_hw_render_callback::get_proc_address).
  virtual void* Proc(const char* name) = 0;

  // The largest framebuffer this context can draw into: GL_MAX_RENDERBUFFER_SIZE, and
  // GL_MAX_VIEWPORT_DIMS for each side.
  uint32_t MaxWidth() const { return max_width_; }
  uint32_t MaxHeight() const { return max_height_; }

  // Makes the framebuffer at least width x height; false when OpenGL refuses.
  bool Reserve(uint32_t width, uint32_t height, bool depth, bool stencil);
  uintptr_t Framebuffer() const { return framebuffer_; }

  // Reads the frame of width x height into out as rows of width x 4 bytes, top row first, in RGBA
  // or BGRA order; bottom_left says the core draws with OpenGL's bottom-left origin.
  bool Read(uint32_t width, uint32_t height, bool bgra, bool bottom_left, uint8_t* out);

  // Releases the framebuffer; called before the context goes away.
  void ReleaseFramebuffer();

  // Answers the messages the system sends to the context's window, where it needs one; called on
  // the game's thread between frames, so the window never looks hung to other programs.
  virtual void Pump() {}

 protected:
  bool LoadFunctions();

 private:
  struct Functions;
  std::shared_ptr<Functions> gl_;
  uint32_t framebuffer_ = 0;
  uint32_t color_ = 0;
  uint32_t depth_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t max_width_ = 0;
  uint32_t max_height_ = 0;
  bool with_depth_ = false;
  bool with_stencil_ = false;
};

}  // namespace retro_host

#endif  // ANYPS5_RETRO_HOST_GPU_H_
