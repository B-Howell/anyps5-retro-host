// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 AnyPS5 Desktop contributors

#include <windows.h>

#include "gpu.h"

namespace retro_host {
namespace {

// Values from the Khronos OpenGL registry, api/GL/wglext.h.
constexpr int kContextMajorVersion = 0x2091;
constexpr int kContextMinorVersion = 0x2092;
constexpr int kContextProfileMask = 0x9126;
constexpr int kContextCoreProfileBit = 0x00000001;

using CreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

// OpenGL on Windows needs a window's device context; this one is never shown.
class WglGpu final : public Gpu {
 public:
  ~WglGpu() override {
    ReleaseFramebuffer();
    wglMakeCurrent(nullptr, nullptr);
    if (context_) wglDeleteContext(context_);
    if (dc_) ReleaseDC(window_, dc_);
    if (window_) DestroyWindow(window_);
    if (opengl_) FreeLibrary(opengl_);
  }

  bool Open(unsigned major, unsigned minor, bool core) {
    opengl_ = LoadLibraryW(L"opengl32.dll");
    if (!opengl_) return false;
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"anyps5-retro-host-gl";
    RegisterClassW(&type);
    window_ = CreateWindowExW(0, type.lpszClassName, L"", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, type.hInstance,
                              nullptr);
    if (!window_) return false;
    dc_ = GetDC(window_);
    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof format;
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    format.cStencilBits = 8;
    const int chosen = ChoosePixelFormat(dc_, &format);
    if (!chosen || !SetPixelFormat(dc_, chosen, &format)) return false;
    HGLRC legacy = wglCreateContext(dc_);
    if (!legacy || !wglMakeCurrent(dc_, legacy)) {
      if (legacy) wglDeleteContext(legacy);
      return false;
    }
    const auto create = reinterpret_cast<CreateContextAttribs>(
        reinterpret_cast<void*>(wglGetProcAddress("wglCreateContextAttribsARB")));
    if (create && major) {
      const int attributes[] = {kContextMajorVersion, static_cast<int>(major), kContextMinorVersion,
                                static_cast<int>(minor), kContextProfileMask, core ? kContextCoreProfileBit : 0, 0};
      context_ = create(dc_, nullptr, core ? attributes : attributes + 6);
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(legacy);
    } else if (!core) {
      context_ = legacy;
    } else {
      wglMakeCurrent(nullptr, nullptr);
      wglDeleteContext(legacy);
    }
    return context_ && wglMakeCurrent(dc_, context_) && LoadFunctions();
  }

  void* Proc(const char* name) override {
    void* function = reinterpret_cast<void*>(wglGetProcAddress(name));
    const auto value = reinterpret_cast<intptr_t>(function);
    if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1) {
      function = reinterpret_cast<void*>(GetProcAddress(opengl_, name));
    }
    return function;
  }

 private:
  HMODULE opengl_ = nullptr;
  HWND window_ = nullptr;
  HDC dc_ = nullptr;
  HGLRC context_ = nullptr;
};

}  // namespace

std::unique_ptr<Gpu> Gpu::Create(unsigned major, unsigned minor, bool core) {
  auto gpu = std::make_unique<WglGpu>();
  if (!gpu->Open(major, minor, core)) return nullptr;
  return gpu;
}

}  // namespace retro_host
