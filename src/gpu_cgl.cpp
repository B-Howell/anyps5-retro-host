// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#define GL_SILENCE_DEPRECATION

#include <OpenGL/OpenGL.h>
#include <dlfcn.h>

#include "gpu.h"

namespace retro_host {
namespace {

// macOS gives OpenGL 4.1 for a core profile request and 2.1 otherwise; IOSurface stays undecided
// (protocol.md "Frame ring"), so frames are read back into the ring as on the other systems.
class CglGpu final : public Gpu {
 public:
  ~CglGpu() override {
    ReleaseFramebuffer();
    CGLSetCurrentContext(nullptr);
    if (context_) CGLDestroyContext(context_);
    if (opengl_) dlclose(opengl_);
  }

  bool Open(unsigned major, unsigned minor, bool core) {
    if (core && (major > 4 || (major == 4 && minor > 1))) return false;
    const CGLPixelFormatAttribute attributes[] = {
        kCGLPFAOpenGLProfile,
        static_cast<CGLPixelFormatAttribute>(core ? kCGLOGLPVersion_3_2_Core : kCGLOGLPVersion_Legacy),
        kCGLPFAAccelerated,
        kCGLPFAColorSize,
        static_cast<CGLPixelFormatAttribute>(24),
        kCGLPFAAlphaSize,
        static_cast<CGLPixelFormatAttribute>(8),
        static_cast<CGLPixelFormatAttribute>(0)};
    CGLPixelFormatObj format = nullptr;
    GLint count = 0;
    if (CGLChoosePixelFormat(attributes, &format, &count) != kCGLNoError || !format) return false;
    const CGLError created = CGLCreateContext(format, nullptr, &context_);
    CGLDestroyPixelFormat(format);
    if (created != kCGLNoError || CGLSetCurrentContext(context_) != kCGLNoError) return false;
    opengl_ = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL);
    return opengl_ && LoadFunctions();
  }

  void* Proc(const char* name) override { return dlsym(opengl_, name); }

 private:
  CGLContextObj context_ = nullptr;
  void* opengl_ = nullptr;
};

}  // namespace

std::unique_ptr<Gpu> Gpu::Create(unsigned major, unsigned minor, bool core) {
  auto gpu = std::make_unique<CglGpu>();
  if (!gpu->Open(major, minor, core)) return nullptr;
  return gpu;
}

}  // namespace retro_host
