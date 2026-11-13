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
// A Mac's own GPU renderer is chosen first. A Mac with none, such as a virtual machine whose
// paravirtual GPU offers Metal but no OpenGL renderer, gets Apple's software renderer instead, as
// Linux gets Mesa's: the core keeps its OpenGL renderer and the internal resolution option.
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
    CGLPixelFormatObj format = Choose(core, true);
    if (!format) format = Choose(core, false);
    if (!format) return false;
    const CGLError created = CGLCreateContext(format, nullptr, &context_);
    CGLDestroyPixelFormat(format);
    if (created != kCGLNoError || CGLSetCurrentContext(context_) != kCGLNoError) return false;
    opengl_ = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW | RTLD_LOCAL);
    return opengl_ && LoadFunctions();
  }

  void* Proc(const char* name) override { return dlsym(opengl_, name); }

 private:
  static CGLPixelFormatObj Choose(bool core, bool accelerated) {
    const auto profile = static_cast<CGLPixelFormatAttribute>(core ? kCGLOGLPVersion_3_2_Core : kCGLOGLPVersion_Legacy);
    const auto software = static_cast<CGLPixelFormatAttribute>(kCGLRendererGenericFloatID);
    const auto color = static_cast<CGLPixelFormatAttribute>(24);
    const auto alpha = static_cast<CGLPixelFormatAttribute>(8);
    const auto end = static_cast<CGLPixelFormatAttribute>(0);
    const CGLPixelFormatAttribute gpu[] = {kCGLPFAOpenGLProfile, profile, kCGLPFAAccelerated,
                                           kCGLPFAColorSize, color, kCGLPFAAlphaSize, alpha, end};
    const CGLPixelFormatAttribute cpu[] = {kCGLPFAOpenGLProfile, profile, kCGLPFARendererID, software,
                                           kCGLPFAColorSize, color, kCGLPFAAlphaSize, alpha, end};
    CGLPixelFormatObj format = nullptr;
    GLint count = 0;
    if (CGLChoosePixelFormat(accelerated ? gpu : cpu, &format, &count) != kCGLNoError) return nullptr;
    return format;
  }

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
