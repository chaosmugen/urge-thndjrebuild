// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RENDERER_DEVICE_RENDER_DEVICE_H_
#define RENDERER_DEVICE_RENDER_DEVICE_H_

#include <atomic>
#include <mutex>
#include <tuple>

#include "renderer/pipeline/render_pipeline.h"
#include "renderer/resource/render_buffer.h"
#include "ui/widget/widget.h"

#if defined(OS_ANDROID)
#include <android/native_window.h>
#endif  //! OS_ANDROID

namespace renderer {

enum class DriverType {
  UNDEFINED = 0,
  OPENGL,
  VULKAN,
  D3D11,
  D3D12,
  WEBGPU,
  kNums,
};

class RenderDevice {
 public:
  using CreateDeviceResult = std::tuple<std::unique_ptr<RenderDevice>,
                                        RRefPtr<Diligent::IDeviceContext>>;
  static CreateDeviceResult Create(base::WeakPtr<ui::Widget> window_target,
                                   DriverType driver_type,
                                   bool validation);

  ~RenderDevice();

  RenderDevice(const RenderDevice&) = delete;
  RenderDevice& operator=(const RenderDevice&) = delete;

  // Device access
  Diligent::IRenderDevice* operator->() { return device_; }
  Diligent::IRenderDevice* operator*() { return device_; }

  // Device Attribute interface
  base::WeakPtr<ui::Widget> GetWindow() { return window_; }
  Diligent::ISwapChain* GetSwapChain() const { return swapchain_; }

  // Max texture size
  int32_t MaxTextureSize() const { return max_texture_size_; }

  // Managed mobile rendering context
  void SuspendContext();
  int32_t ResumeContext(Diligent::IDeviceContext* immediate_context);

  // Returns true once right after the swap chain was rebuilt, asking the
  // caller to skip Present() for that single frame. On Android the freshly
  // published ANativeWindow needs one frame before the driver can safely
  // present it (vkQueuePresentKHR NULL-dereferences otherwise).
  bool ConsumeSkipPresentOnce();

  // Suppresses the Resize() that would otherwise follow a rebuild for a few
  // frames: resizing a swap chain that was just rebuilt crashes inside the
  // Adreno driver, and the window size can still be stale right after a resume.
  bool ConsumeResizeSuppression();

  // Whether the rendering surface is usable right now.
  //
  // On Android the ANativeWindow is released by SDL on surfaceDestroyed()
  // without waiting for the render thread. While it is gone, every Vulkan
  // entry point that touches the surface (Resize/Present/Recreate) must be
  // skipped, otherwise the driver dereferences a dead ANativeWindow and the
  // process dies.
  bool IsSurfaceValid() const;

  // Called once per frame, before anything is rendered.
  // Rebuilds the swap chain when a fresh native window became available and
  // reports whether rendering is allowed for this frame.
  bool UpdateSwapChainState(Diligent::IDeviceContext* immediate_context);

  // Marks the surface as unusable from the render thread, e.g. after a Vulkan
  // call failed because the ANativeWindow is gone. Rendering is skipped until
  // UpdateSwapChainState() rebuilds the swap chain.
  void MarkSurfaceLost();

  // Called from the Java UI thread (Activity.onPause) before SDL releases the
  // ANativeWindow. Only flips the surface state, never dereferences objects
  // owned by the render thread.
  static void NotifySurfaceLosing();

 private:
  RenderDevice(int32_t max_texture_size,
               base::WeakPtr<ui::Widget> window,
               const Diligent::SwapChainDesc& swapchain_desc,
               Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device,
               Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapchain,
               SDL_GLContext gl_context);

  base::WeakPtr<ui::Widget> window_;
  Diligent::SwapChainDesc swapchain_desc_;
  int32_t max_texture_size_;

  Diligent::RefCntAutoPtr<Diligent::IRenderDevice> device_;
  Diligent::RefCntAutoPtr<Diligent::ISwapChain> swapchain_;

  Diligent::RENDER_DEVICE_TYPE device_type_;
  SDL_GLContext gl_context_;

  // Set for a single frame when the swap chain was rebuilt, consumed by
  // ConsumeSkipPresentOnce(). Never set outside the Android Vulkan path.
  std::atomic<bool> skip_present_once_{false};
  std::atomic<int32_t> resize_suppress_frames_{0};

#if defined(OS_ANDROID)
  // Native window the current swap chain is bound to. Only compared against
  // the value published by SDL, never dereferenced.
  void* bound_window_ = nullptr;

  // Strong reference on the bound ANativeWindow. SDL calls
  // ANativeWindow_release() from the UI thread while the Vulkan driver may
  // still hold a pointer to it; keeping our own reference turns a wild
  // pointer dereference into a recoverable Vulkan error.
  ANativeWindow* acquired_window_ = nullptr;

  std::atomic<bool> surface_valid_{true};
  std::atomic<bool> pending_recreate_{false};
  std::atomic<int32_t> pending_frame_count_{0};
  std::mutex swapchain_lock_;

  // Descriptor of the swap chain that was live before the surface was lost.
  // swapchain_desc_ is only the creation-time one (3 buffers / FIFO) and does
  // not match what the renderer later resized to (4 buffers / MAILBOX);
  // rebuilding from it forces Diligent to recreate the swap chain a second
  // time right after every resume.
  Diligent::SwapChainDesc live_swapchain_desc_;
  bool has_live_swapchain_desc_ = false;

  void* GetAndroidNativeWindow() const;
  void ReleaseAcquiredWindow();
  void RecreateSwapChainInternal(Diligent::IDeviceContext* immediate_context,
                                 void* native_window);

  static std::atomic<RenderDevice*> current_device_;
#endif  //! OS_ANDROID
};

}  // namespace renderer

#endif  //! RENDERER_DEVICE_RENDER_DEVICE_H_
