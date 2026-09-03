// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "renderer/device/render_device.h"

#include <exception>

#include "SDL3/SDL_hints.h"
#include "SDL3/SDL_loadso.h"
#include "SDL3/SDL_video.h"
#include "magic_enum/magic_enum.hpp"

#include "Graphics/GraphicsAccessories/interface/GraphicsAccessories.hpp"
#if GL_SUPPORTED || GLES_SUPPORTED
#include "Graphics/GraphicsEngineOpenGL/interface/EngineFactoryOpenGL.h"
#endif  //! GL_SUPPORTED || GLES_SUPPORTED
#if VULKAN_SUPPORTED
#include "Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h"
#endif  // !VULKAN_SUPPORTED
#if D3D11_SUPPORTED
#include "Graphics/GraphicsEngineD3D11/interface/EngineFactoryD3D11.h"
#endif  //! D3D11_SUPPORTED
#if D3D12_SUPPORTED
#include "Graphics/GraphicsEngineD3D12/interface/EngineFactoryD3D12.h"
#endif  // !D3D12_SUPPORTED
#if WEBGPU_SUPPORTED
#include "Graphics/GraphicsEngineWebGPU/interface/EngineFactoryWebGPU.h"
#endif  //! WEBGPU_SUPPORTED
#include "Primitives/interface/DebugOutput.h"

#if PLATFORM_WEB
#include <emscripten/html5_webgpu.h>
#endif

#include "base/debug/logging.h"
#include "ui/widget/widget.h"

#if defined(OS_ANDROID)
#include "Graphics/GraphicsEngineOpenGL/interface/RenderDeviceGLES.h"
#endif

namespace renderer {

#if defined(OS_ANDROID)
std::atomic<RenderDevice*> RenderDevice::current_device_{nullptr};

namespace {
// Frames to wait for SDL to publish a brand new native window after a resume
// before rebuilding the swap chain on the window we already have. Some
// background transitions (power button, notification shade) never destroy the
// surface, so SDL never publishes a new window in those cases.
constexpr int32_t kMaxSurfaceWaitFrames = 30;
}  // namespace
#endif  //! OS_ANDROID

//--------------------------------------------------------------------------------------
// Internal Helper Functions
//--------------------------------------------------------------------------------------

namespace {

void DILIGENT_CALL_TYPE
DebugMessageOutputFunc(Diligent::DEBUG_MESSAGE_SEVERITY Severity,
                       const Diligent::Char* Message,
                       const Diligent::Char* Function,
                       const Diligent::Char* File,
                       int Line) {
  if (Function)
    LOG(INFO) << "[Renderer] Function " << Function << ":";

  switch (Severity) {
    default:
    case Diligent::DEBUG_MESSAGE_SEVERITY_INFO:
      LOG(INFO) << "[Renderer] " << Message;
      break;
    case Diligent::DEBUG_MESSAGE_SEVERITY_WARNING:
      LOG(WARNING) << "[Renderer] " << Message;
      break;
    case Diligent::DEBUG_MESSAGE_SEVERITY_ERROR:
      LOG(ERROR) << "[Renderer] " << Message;
      break;
    case Diligent::DEBUG_MESSAGE_SEVERITY_FATAL_ERROR:
      LOG(FATAL) << "[Renderer] " << Message;
      break;
  }
}

}  // namespace

RenderDevice::CreateDeviceResult RenderDevice::Create(
    base::WeakPtr<ui::Widget> window_target,
    DriverType driver_type,
    bool validation) {
  // Setup debugging output
  Diligent::SetDebugMessageCallback(DebugMessageOutputFunc);

  // Setup native window
  Diligent::NativeWindow native_window;
  SDL_PropertiesID window_properties =
      SDL_GetWindowProperties(window_target->AsSDLWindow());

  // Setup specific platform window handle
  SDL_GLContext glcontext = nullptr;
#if defined(OS_WIN)
  native_window.hWnd = SDL_GetPointerProperty(
      window_properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(OS_LINUX)
  // Xlib Display Port
  void* xdisplay = SDL_GetPointerProperty(
      window_properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  int64_t xwindow = SDL_GetNumberProperty(window_properties,
                                          SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);

  // OpenGL context
  glcontext = SDL_GL_CreateContext(window_target->AsSDLWindow());
  SDL_GL_MakeCurrent(window_target->AsSDLWindow(), glcontext);

  // Get XCBConnect from Xlib for Vulkan
  SDL_SharedObject* xlib_xcb_library = SDL_LoadObject("libX11-xcb.so");
  if (!xlib_xcb_library)
    xlib_xcb_library = SDL_LoadObject("libX11-xcb.so.1");

  // Get proc address
  using XGetXCBConnection = void* (*)(void*);
  XGetXCBConnection xgetxcb_func = nullptr;
  if (xlib_xcb_library)
    xgetxcb_func = (XGetXCBConnection)SDL_LoadFunction(xlib_xcb_library,
                                                       "XGetXCBConnection");

  // Setup native window
  native_window.WindowId = static_cast<uint32_t>(xwindow);
  native_window.pDisplay = xdisplay;
  native_window.pXCBConnection =
      xgetxcb_func ? xgetxcb_func(xdisplay) : nullptr;
#elif defined(OS_ANDROID)
  native_window.pAWindow = SDL_GetPointerProperty(
      window_properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
#elif defined(OS_EMSCRIPTEN)
  native_window.pCanvasId = SDL_GetStringProperty(
      window_properties, SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING,
      "#canvas");
#else
#error "Unsupport Platform"
#endif

// Correction backend settings
#if defined(OS_WIN)
  switch (driver_type) {
    case DriverType::OPENGL:
    case DriverType::VULKAN:
    case DriverType::D3D11:
    case DriverType::D3D12:
      break;
    default:
      driver_type = DriverType::D3D11;
      break;
  }
#elif defined(OS_LINUX)
  switch (driver_type) {
    case DriverType::OPENGL:
    case DriverType::VULKAN:
      break;
    default:
      driver_type = DriverType::OPENGL;
      break;
  }
#elif defined(OS_ANDROID)
  switch (driver_type) {
    case DriverType::OPENGL:
    case DriverType::VULKAN:
      break;
    default:
      driver_type = DriverType::OPENGL;
      break;
  }
#elif defined(OS_EMSCRIPTEN)
  switch (driver_type) {
    case DriverType::OPENGL:
    case DriverType::WEBGPU:
      break;
    default:
      driver_type = DriverType::OPENGL;
      break;
  }
#else
#error "Unsupport Platform"
#endif

  Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device;
  Diligent::RefCntAutoPtr<Diligent::IDeviceContext> context;
  Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapchain;

  // Initialize driver descriptor
  Diligent::EngineCreateInfo engine_create_info;
  Diligent::SwapChainDesc swap_chain_desc;
#if D3D11_SUPPORTED || D3D12_SUPPORTED
  Diligent::FullScreenModeDesc fullscreen_mode_desc;
#endif

  // Setup renderer create info
  engine_create_info.EnableValidation = validation;
  if (engine_create_info.EnableValidation)
    LOG(INFO) << "[Renderer] Enable renderer validation.";

  // Requested features
  if (driver_type != DriverType::OPENGL)
    engine_create_info.Features.SeparablePrograms =
        Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
  engine_create_info.Features.ComputeShaders =
      Diligent::DEVICE_FEATURE_STATE_OPTIONAL;

  // Setup primary swapchain
  swap_chain_desc.ColorBufferFormat = Diligent::TEX_FORMAT_RGBA8_UNORM;
  swap_chain_desc.PreTransform = Diligent::SURFACE_TRANSFORM_OPTIMAL;
  swap_chain_desc.IsPrimary = Diligent::True;

  // Create device and fallback when error
  size_t creating_retry_count = 0;
  do {
#if GL_SUPPORTED || GLES_SUPPORTED
    if (driver_type == DriverType::OPENGL) {
#if ENGINE_DLL
      auto GetEngineFactoryOpenGL = Diligent::LoadGraphicsEngineOpenGL();
#else
      using Diligent::GetEngineFactoryOpenGL;
#endif
      auto* factory = GetEngineFactoryOpenGL();

      Diligent::EngineGLCreateInfo gl_create_info(engine_create_info);
      gl_create_info.Window = native_window;

      factory->CreateDeviceAndSwapChainGL(gl_create_info, &device, &context,
                                          swap_chain_desc, &swapchain);
    }
#endif  // OPENGL_SUPPORT
#if VULKAN_SUPPORTED
    if (driver_type == DriverType::VULKAN) {
#if ENGINE_DLL
      auto GetEngineFactoryVk = Diligent::LoadGraphicsEngineVk();
#else
      using Diligent::GetEngineFactoryVk;
#endif
      auto* factory = GetEngineFactoryVk();

      Diligent::EngineVkCreateInfo vk_create_info(engine_create_info);
      vk_create_info.FeaturesVk.DynamicRendering =
          Diligent::DEVICE_FEATURE_STATE_OPTIONAL;

      factory->CreateDeviceAndContextsVk(vk_create_info, &device, &context);
      factory->CreateSwapChainVk(device, context, swap_chain_desc,
                                 native_window, &swapchain);
    }
#endif  // VULKAN_SUPPORT
#if D3D11_SUPPORTED
    if (driver_type == DriverType::D3D11) {
#if ENGINE_DLL
      auto GetEngineFactoryD3D11 = Diligent::LoadGraphicsEngineD3D11();
#else
      using Diligent::GetEngineFactoryD3D11;
#endif
      auto* pFactory = GetEngineFactoryD3D11();

      Diligent::EngineD3D11CreateInfo d3d11_create_info(engine_create_info);
      pFactory->CreateDeviceAndContextsD3D11(d3d11_create_info, &device,
                                             &context);
      pFactory->CreateSwapChainD3D11(device, context, swap_chain_desc,
                                     fullscreen_mode_desc, native_window,
                                     &swapchain);
    }
#endif  // D3D11_SUPPORT
#if D3D12_SUPPORTED
    if (driver_type == DriverType::D3D12) {
#if ENGINE_DLL
      auto GetEngineFactoryD3D12 = Diligent::LoadGraphicsEngineD3D12();
#else
      using Diligent::GetEngineFactoryD3D12;
#endif
      auto* pFactoryD3D12 = GetEngineFactoryD3D12();

      Diligent::EngineD3D12CreateInfo d3d12_create_info(engine_create_info);
      pFactoryD3D12->CreateDeviceAndContextsD3D12(d3d12_create_info, &device,
                                                  &context);
      pFactoryD3D12->CreateSwapChainD3D12(device, context, swap_chain_desc,
                                          fullscreen_mode_desc, native_window,
                                          &swapchain);
    }
#endif  // D3D12_SUPPORT
#if WEBGPU_SUPPORTED
    if (driver_type == DriverType::WEBGPU) {
#if ENGINE_DLL
      auto GetEngineFactoryWebGPU = Diligent::LoadGraphicsEngineWebGPU();
#else
      using Diligent::GetEngineFactoryWebGPU;
#endif
      auto* pFactoryWebGPU = GetEngineFactoryWebGPU();

      Diligent::EngineWebGPUCreateInfo webgpu_create_info(engine_create_info);
      webgpu_create_info.Features.AsyncShaderCompilation =
          Diligent::DEVICE_FEATURE_STATE_DISABLED;

#if PLATFORM_WEB
      WGPUInstance wgpuInstance = wgpuCreateInstance(nullptr);
      WGPUDevice wgpuDevice = emscripten_webgpu_get_device();
      pFactoryWebGPU->AttachToWebGPUDevice(wgpuInstance, nullptr, wgpuDevice,
                                           webgpu_create_info, &device,
                                           &context);
#else
      pFactoryWebGPU->CreateDeviceAndContextsWebGPU(webgpu_create_info, &device,
                                                    &context);
#endif

      pFactoryWebGPU->CreateSwapChainWebGPU(device, context, swap_chain_desc,
                                            native_window, &swapchain);
    }
#endif  // WEBGPU_SUPPORTED

    if (device && context && swapchain) {
      // Success
      break;
    }

    // Fallback
    driver_type = static_cast<DriverType>(creating_retry_count++);
  } while (creating_retry_count < static_cast<size_t>(DriverType::kNums));

  if (!device || !context || !swapchain) {
    LOG(ERROR) << "[Renderer] Failed to create renderer.";
    return CreateDeviceResult(nullptr, nullptr);
  }

  // etc
  const auto& device_info = device->GetDeviceInfo();
  const auto& adapter_info = device->GetAdapterInfo();
  const int32_t max_texture_size =
      static_cast<int32_t>(adapter_info.Texture.MaxTexture2DDimension);

  LOG(INFO) << "[Renderer] DeviceType: " +
                   std::string(GetRenderDeviceTypeString(device_info.Type)) +
                   " (version "
            << device_info.APIVersion.Major << "."
            << device_info.APIVersion.Minor << ")";
  LOG(INFO) << "[Renderer] Adapter: " << adapter_info.Description;
  LOG(INFO) << "[Renderer] MaxTexture Size: " << max_texture_size;

  // Global render device
  std::unique_ptr<RenderDevice> render_device(
      new RenderDevice(max_texture_size, window_target, swap_chain_desc, device,
                       swapchain, glcontext));

#if defined(OS_ANDROID)
  // Track the native window the swap chain was built on and keep a strong
  // reference on it, see RenderDevice::acquired_window_.
  render_device->bound_window_ = native_window.pAWindow;
  if (render_device->bound_window_) {
    ANativeWindow_acquire(
        static_cast<ANativeWindow*>(render_device->bound_window_));
    render_device->acquired_window_ =
        static_cast<ANativeWindow*>(render_device->bound_window_);
  }
  current_device_.store(render_device.get());
#endif  //! OS_ANDROID

  return std::make_tuple(std::move(render_device), std::move(context));
}

RenderDevice::RenderDevice(
    int32_t max_texture_size,
    base::WeakPtr<ui::Widget> window,
    const Diligent::SwapChainDesc& swapchain_desc,
    Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device,
    Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapchain,
    SDL_GLContext gl_context)
    : window_(std::move(window)),
      swapchain_desc_(swapchain_desc),
      max_texture_size_(max_texture_size),
      device_(device),
      swapchain_(swapchain),
      device_type_(device_->GetDeviceInfo().Type),
      gl_context_(gl_context) {}

RenderDevice::~RenderDevice() {
#if defined(OS_ANDROID)
  {
    RenderDevice* self = this;
    current_device_.compare_exchange_strong(self, nullptr);
  }
  ReleaseAcquiredWindow();
#endif  //! OS_ANDROID

  if (gl_context_)
    SDL_GL_DestroyContext(gl_context_);
}

bool RenderDevice::IsSurfaceValid() const {
#if defined(OS_ANDROID)
  return surface_valid_.load(std::memory_order_acquire);
#else
  return true;
#endif  //! OS_ANDROID
}

void RenderDevice::SuspendContext() {
#if defined(OS_ANDROID)
  // The native window is gone, or about to be released by SDL: block every
  // rendering step before it gets a chance to touch it again.
  pending_frame_count_.store(0, std::memory_order_relaxed);
  surface_valid_.store(false, std::memory_order_release);

  switch (device_type_) {
    case Diligent::RENDER_DEVICE_TYPE_GLES: {
      Diligent::RefCntAutoPtr<Diligent::IRenderDeviceGLES> es_device(
          device_, Diligent::IID_RenderDeviceGLES);
      es_device->Suspend();
    } break;
#if VULKAN_SUPPORTED
    case Diligent::RENDER_DEVICE_TYPE_VULKAN:
      /* The swap chain is intentionally kept alive here.
       *
       * Destroying or recreating a Vulkan surface dereferences the
       * ANativeWindow, and SDL releases it from surfaceDestroyed() without
       * waiting for the render thread (SDL only synchronizes windows created
       * with the OpenGL flag, which would make the window incompatible with
       * vkCreateAndroidSurfaceKHR). The rebuild is deferred to
       * UpdateSwapChainState(), which runs once SDL publishes a brand new
       * native window. */
      break;
#endif  // VULKAN_SUPPORTED
    default:
      break;
  }
#endif  // OS_ANDROID
}

int32_t RenderDevice::ResumeContext(
    Diligent::IDeviceContext* immediate_context) {
#if defined(OS_ANDROID)
  switch (device_type_) {
    case Diligent::RENDER_DEVICE_TYPE_GLES: {
      void* android_native_window = GetAndroidNativeWindow();
      Diligent::RefCntAutoPtr<Diligent::IRenderDeviceGLES> es_device(
          device_, Diligent::IID_RenderDeviceGLES);
      int32_t resume_result =
          es_device->Resume(static_cast<ANativeWindow*>(android_native_window));

      if (resume_result == EGL_SUCCESS)
        surface_valid_.store(true, std::memory_order_release);
      else
        LOG(ERROR) << "[Renderer] Failed to resume GLES context: "
                   << resume_result;

      return resume_result;
    }
#if VULKAN_SUPPORTED
    case Diligent::RENDER_DEVICE_TYPE_VULKAN:
      /* A usable ANativeWindow is usually not published yet when
       * DID_ENTER_FOREGROUND is dispatched: SDL writes it from
       * surfaceCreated(), which comes later. Arm the deferred rebuild and let
       * UpdateSwapChainState() perform it on one of the next frames. */
      pending_recreate_.store(true, std::memory_order_release);
      return EGL_SUCCESS;
#endif  // VULKAN_SUPPORTED
    default:
      break;
  }

  return EGL_NOT_INITIALIZED;
#else
  (void)immediate_context;
  return 0;
#endif  // OS_ANDROID
}

bool RenderDevice::UpdateSwapChainState(
    Diligent::IDeviceContext* immediate_context) {
#if defined(OS_ANDROID)
  // GLES keeps its swap chain object alive across suspend/resume, the surface
  // state alone decides whether rendering is allowed.
  if (device_type_ != Diligent::RENDER_DEVICE_TYPE_VULKAN)
    return IsSurfaceValid() && static_cast<bool>(swapchain_);

  if (IsSurfaceValid())
    return static_cast<bool>(swapchain_);

  if (!pending_recreate_.load(std::memory_order_acquire))
    return false;

  void* android_native_window = GetAndroidNativeWindow();
  if (!android_native_window)
    return false;

  // Prefer a brand new window: SDL publishes it from surfaceCreated(), where
  // the window is guaranteed to be usable. Comparing pointers never
  // dereferences the window itself.
  if (android_native_window == bound_window_ &&
      pending_frame_count_.load(std::memory_order_relaxed) <
          kMaxSurfaceWaitFrames) {
    pending_frame_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  std::lock_guard<std::mutex> lock(swapchain_lock_);
  RecreateSwapChainInternal(immediate_context, android_native_window);

  return IsSurfaceValid() && static_cast<bool>(swapchain_);
#else
  (void)immediate_context;
  return true;
#endif  // OS_ANDROID
}

void RenderDevice::MarkSurfaceLost() {
#if defined(OS_ANDROID)
  pending_frame_count_.store(0, std::memory_order_relaxed);
  surface_valid_.store(false, std::memory_order_release);
#endif  //! OS_ANDROID
}

void RenderDevice::NotifySurfaceLosing() {
#if defined(OS_ANDROID)
  // Called on the Java UI thread: only the atomic state is touched here, the
  // render thread owns every other member.
  RenderDevice* device = current_device_.load(std::memory_order_acquire);
  if (device)
    device->MarkSurfaceLost();
#endif  //! OS_ANDROID
}

#if defined(OS_ANDROID)
void* RenderDevice::GetAndroidNativeWindow() const {
  if (!window_.get())
    return nullptr;

  SDL_PropertiesID window_properties =
      SDL_GetWindowProperties(window_->AsSDLWindow());
  return SDL_GetPointerProperty(
      window_properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
}

void RenderDevice::ReleaseAcquiredWindow() {
  if (acquired_window_) {
    ANativeWindow_release(acquired_window_);
    acquired_window_ = nullptr;
  }
}

void RenderDevice::RecreateSwapChainInternal(
    Diligent::IDeviceContext* immediate_context,
    void* native_window) {
  // Retire pending GPU work before dropping the old swap chain.
  try {
    device_->IdleGPU();
  } catch (const std::exception& error) {
    LOG(ERROR) << "[Renderer] IdleGPU failed: " << error.what();
  }

  swapchain_.Release();
  ReleaseAcquiredWindow();

  Diligent::NativeWindow window;
  window.pAWindow = native_window;

  bool recreated = false;
  try {
#if ENGINE_DLL
    auto GetEngineFactoryVk = Diligent::LoadGraphicsEngineVk();
#else
    using Diligent::GetEngineFactoryVk;
#endif
    auto* factory = GetEngineFactoryVk();
    factory->CreateSwapChainVk(device_, immediate_context, swapchain_desc_,
                               window, &swapchain_);
    recreated = static_cast<bool>(swapchain_);
  } catch (const std::exception& error) {
    LOG(ERROR) << "[Renderer] Failed to recreate swap chain: " << error.what();
    recreated = false;
  }

  if (recreated) {
    // Keep the window alive for the driver: SDL calls ANativeWindow_release()
    // from the UI thread as soon as the surface is destroyed, while the Vulkan
    // driver keeps a pointer to it until the surface is destroyed.
    ANativeWindow_acquire(static_cast<ANativeWindow*>(native_window));
    acquired_window_ = static_cast<ANativeWindow*>(native_window);
    bound_window_ = native_window;

    pending_recreate_.store(false, std::memory_order_release);
    surface_valid_.store(true, std::memory_order_release);
    LOG(INFO) << "[Renderer] Swap chain rebuilt on the new native window.";
  } else {
    surface_valid_.store(false, std::memory_order_release);
    LOG(ERROR) << "[Renderer] Swap chain rebuild failed, retry next frame.";
  }
}
#endif  //! OS_ANDROID

}  // namespace renderer
