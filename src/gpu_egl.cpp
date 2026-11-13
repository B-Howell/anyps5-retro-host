// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <dlfcn.h>

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <type_traits>

#include "gpu.h"

namespace retro_host {
namespace {

// Values from the Khronos EGL registry, api/EGL/egl.h and api/EGL/eglext.h.
constexpr int32_t kNone = 0x3038;
constexpr int32_t kSurfaceType = 0x3033;
constexpr int32_t kPbufferBit = 0x0001;
constexpr int32_t kRenderableType = 0x3040;
constexpr int32_t kOpenGlBit = 0x0008;
constexpr int32_t kRedSize = 0x3024;
constexpr int32_t kGreenSize = 0x3023;
constexpr int32_t kBlueSize = 0x3022;
constexpr int32_t kAlphaSize = 0x3021;
constexpr int32_t kWidth = 0x3057;
constexpr int32_t kHeight = 0x3056;
constexpr int32_t kExtensions = 0x3055;
constexpr uint32_t kOpenGlApi = 0x30A2;
constexpr int32_t kContextMajorVersion = 0x3098;
constexpr int32_t kContextMinorVersion = 0x30FB;
constexpr int32_t kContextOpenGlProfileMask = 0x30FD;
constexpr int32_t kContextOpenGlCoreProfileBit = 0x00000001;
constexpr uint32_t kPlatformSurfacelessMesa = 0x31DD;

using Display = void*;
using Config = void*;
using Context = void*;
using Surface = void*;

struct Egl {
  void* library = nullptr;
  void* (*GetProcAddress)(const char*) = nullptr;
  Display (*GetDisplay)(void*) = nullptr;
  unsigned (*Initialize)(Display, int32_t*, int32_t*) = nullptr;
  unsigned (*Terminate)(Display) = nullptr;
  unsigned (*BindApi)(uint32_t) = nullptr;
  unsigned (*ChooseConfig)(Display, const int32_t*, Config*, int32_t, int32_t*) = nullptr;
  Context (*CreateContext)(Display, Config, Context, const int32_t*) = nullptr;
  unsigned (*DestroyContext)(Display, Context) = nullptr;
  Surface (*CreatePbufferSurface)(Display, Config, const int32_t*) = nullptr;
  unsigned (*DestroySurface)(Display, Surface) = nullptr;
  unsigned (*MakeCurrent)(Display, Surface, Surface, Context) = nullptr;
  const char* (*QueryString)(Display, int32_t) = nullptr;
  Display (*GetPlatformDisplay)(uint32_t, void*, const intptr_t*) = nullptr;
  unsigned (*ReleaseThread)() = nullptr;
};

class EglGpu final : public Gpu {
 public:
  ~EglGpu() override {
    ReleaseFramebuffer();
    if (display_) {
      egl_.MakeCurrent(display_, nullptr, nullptr, nullptr);
      if (context_) egl_.DestroyContext(display_, context_);
      if (surface_) egl_.DestroySurface(display_, surface_);
      egl_.Terminate(display_);
    }
    // The last EGL call on this thread: libglvnd before 1.5 (Ubuntu 22.04 has 1.4) leaves this
    // thread's state under a pthread key whose destructor is in libEGL, so unloading libEGL with
    // that state still set crashes the thread when it exits.
    if (egl_.ReleaseThread) egl_.ReleaseThread();
    if (opengl_) dlclose(opengl_);
    if (egl_.library) dlclose(egl_.library);
  }

  bool Open(unsigned major, unsigned minor, bool core) {
    egl_.library = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl_.library) return false;
    const auto load = [this](auto& function, const char* name) {
      function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(dlsym(egl_.library, name));
      return function != nullptr;
    };
    if (!load(egl_.GetProcAddress, "eglGetProcAddress") || !load(egl_.GetDisplay, "eglGetDisplay") ||
        !load(egl_.Initialize, "eglInitialize") || !load(egl_.Terminate, "eglTerminate") ||
        !load(egl_.BindApi, "eglBindAPI") || !load(egl_.ChooseConfig, "eglChooseConfig") ||
        !load(egl_.CreateContext, "eglCreateContext") || !load(egl_.DestroyContext, "eglDestroyContext") ||
        !load(egl_.CreatePbufferSurface, "eglCreatePbufferSurface") ||
        !load(egl_.DestroySurface, "eglDestroySurface") || !load(egl_.MakeCurrent, "eglMakeCurrent") ||
        !load(egl_.QueryString, "eglQueryString") || !load(egl_.ReleaseThread, "eglReleaseThread")) {
      return false;
    }
    egl_.GetPlatformDisplay = reinterpret_cast<decltype(egl_.GetPlatformDisplay)>(dlsym(egl_.library, "eglGetPlatformDisplay"));
    const char* client = egl_.QueryString(nullptr, kExtensions);
    if (egl_.GetPlatformDisplay && client && std::strstr(client, "EGL_MESA_platform_surfaceless")) {
      display_ = egl_.GetPlatformDisplay(kPlatformSurfacelessMesa, nullptr, nullptr);
    }
    if (!display_) display_ = egl_.GetDisplay(nullptr);
    if (!display_ || !egl_.Initialize(display_, nullptr, nullptr)) {
      display_ = nullptr;
      return false;
    }
    if (!egl_.BindApi(kOpenGlApi)) return false;
    const int32_t wanted[] = {kSurfaceType, kPbufferBit, kRenderableType, kOpenGlBit, kRedSize, 8, kGreenSize, 8,
                              kBlueSize, 8, kAlphaSize, 8, kNone};
    Config config = nullptr;
    int32_t found = 0;
    if (!egl_.ChooseConfig(display_, wanted, &config, 1, &found) || found < 1) return false;
    const int32_t size[] = {kWidth, 16, kHeight, 16, kNone};
    surface_ = egl_.CreatePbufferSurface(display_, config, size);
    const int32_t versioned[] = {kContextMajorVersion, static_cast<int32_t>(major), kContextMinorVersion,
                                 static_cast<int32_t>(minor), kContextOpenGlProfileMask, kContextOpenGlCoreProfileBit,
                                 kNone};
    const int32_t plain[] = {kNone};
    context_ = egl_.CreateContext(display_, config, nullptr, core && major ? versioned : plain);
    if (!context_ || !egl_.MakeCurrent(display_, surface_, surface_, context_)) return false;
    opengl_ = dlopen("libOpenGL.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!opengl_) opengl_ = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    return LoadFunctions();
  }

  void* Proc(const char* name) override {
    void* function = egl_.GetProcAddress(name);
    if (!function && opengl_) function = dlsym(opengl_, name);
    return function;
  }

 private:
  Egl egl_;
  void* opengl_ = nullptr;
  Display display_ = nullptr;
  Surface surface_ = nullptr;
  Context context_ = nullptr;
};

}  // namespace

std::unique_ptr<Gpu> Gpu::Create(unsigned major, unsigned minor, bool core) {
  auto gpu = std::make_unique<EglGpu>();
  if (!gpu->Open(major, minor, core)) return nullptr;
  return gpu;
}

}  // namespace retro_host
