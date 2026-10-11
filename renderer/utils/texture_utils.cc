// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "renderer/utils/texture_utils.h"

#include "SDL3/SDL_timer.h"

#include "Graphics/GraphicsAccessories/interface/GraphicsAccessories.hpp"

namespace renderer {

namespace {

// See SetLoadDrainContext()/SetVulkanBackend()/ConsiderLoadDrain().
Diligent::IDeviceContext* g_drain_context = nullptr;
bool g_vulkan_backend = false;
uint64_t g_last_present_ms = 0;

}  // namespace

void SetLoadDrainContext(Diligent::IDeviceContext* context) {
  g_drain_context = context;
}

void SetVulkanBackend(bool vulkan) {
  g_vulkan_backend = vulkan;
}

void NotifyFramePresented() {
  // A frame reached the screen: it re-arms the quiet period below. Play presents
  // constantly (battle animations included), so the drain never engages there.
  g_last_present_ms = SDL_GetTicks();
}

void ConsiderLoadDrain() {
  if (!g_drain_context || !g_vulkan_backend)
    return;

  // Only once the current script frame has been running for a while without
  // presenting anything. That is what a load looks like - one enormous frame
  // submitting thousands of texture operations - while every gameplay frame,
  // however heavy on blits, presents long before this elapses.
  if (SDL_GetTicks() - g_last_present_ms < 200)
    return;

  g_drain_context->Flush();
  g_drain_context->WaitForIdle();
}

void CreateTexture2D(Diligent::IRenderDevice* device,
                     Diligent::ITexture** texture,
                     const std::string& name,
                     const base::Vec2i& size,
                     Diligent::USAGE usage,
                     Diligent::BIND_FLAGS bind_flags,
                     Diligent::CPU_ACCESS_FLAGS access,
                     Diligent::TEXTURE_FORMAT format) {
  Diligent::TextureDesc texture_desc;
  texture_desc.Name = name.c_str();
  texture_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
  texture_desc.Width = size.x;
  texture_desc.Height = size.y;
  texture_desc.Format = format;
  texture_desc.BindFlags = bind_flags;
  texture_desc.Usage = usage;
  texture_desc.CPUAccessFlags = access;
  texture_desc.ClearValue.Format = texture_desc.Format;
  std::memset(texture_desc.ClearValue.Color, 0,
              sizeof(texture_desc.ClearValue.Color));

  // A zero-sized texture is invalid; reject it early instead of allocating a
  // zero-length buffer and letting CreateTexture fail on a 0x0 dimension.
  if (size.x <= 0 || size.y <= 0)
    return;

  // Depth-stencil textures must NOT be created with initial content: the
  // Vulkan backend has no implementation for that path and asserts
  // ("Initializing depth-stencil texture is currently not supported",
  // TextureVkImpl.cpp). In release the assertion is compiled out and the
  // unsupported path is taken silently, which is exactly what we do not want
  // on a backend this strict. GL tolerates it, which is why this only ever
  // showed up on Vulkan.
  const bool is_depth_stencil =
      (bind_flags & Diligent::BIND_DEPTH_STENCIL) != 0;
  // Readback destinations must not be created with initial content either: passing a zero
  // fill makes the driver allocate and copy it, which on this path aborts the process on the
  // first presented frame. The fill below is for render targets and for textures that are
  // sampled - not for a texture that only ever receives copies.
  const bool is_readback =
      (access & Diligent::CPU_ACCESS_READ) != 0 ||
      usage == Diligent::USAGE_STAGING;
  if (is_depth_stencil || is_readback) {
    device->CreateTexture(texture_desc, nullptr, texture);
    return;
  }

  // Vulkan 不会自动清零新分配的显存（OpenGL 多数驱动会），未初始化的
  // SRV 纹理（如 tilemap atlas）会呈现随机内容导致花屏。这里显式传入
  // 全 0 初始数据，确保所有平台创建时显存都被初始化为透明。
  const uint32_t bytes_per_element =
      Diligent::GetTextureFormatAttribs(texture_desc.Format).GetElementSize();

  // Reuse a per-thread zero buffer instead of allocating w*h*4 bytes on every
  // texture creation (screen buffers / atlases can be multi-MB). thread_local
  // keeps calls from different threads independent.
  static thread_local std::vector<uint8_t> zeros;
  zeros.assign(static_cast<size_t>(size.x) * size.y * bytes_per_element, 0);

  Diligent::TextureSubResData sub_res_data;
  sub_res_data.pData = zeros.data();
  sub_res_data.Stride = static_cast<uint32_t>(size.x) * bytes_per_element;
  Diligent::TextureData texture_data;
  texture_data.pSubResources = &sub_res_data;
  texture_data.NumSubresources = 1;

  device->CreateTexture(texture_desc, &texture_data, texture);
  ConsiderLoadDrain();
}

void CreateTexture2D(Diligent::IRenderDevice* device,
                     Diligent::ITexture** texture,
                     const std::string& name,
                     SDL_Surface* data,
                     Diligent::USAGE usage,
                     Diligent::BIND_FLAGS bind_flags,
                     Diligent::CPU_ACCESS_FLAGS access,
                     Diligent::TEXTURE_FORMAT format) {
  Diligent::TextureDesc texture_desc;
  texture_desc.Name = name.c_str();
  texture_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
  texture_desc.Width = data->w;
  texture_desc.Height = data->h;
  texture_desc.Format = format;
  texture_desc.BindFlags = bind_flags;
  texture_desc.Usage = usage;
  texture_desc.CPUAccessFlags = access;
  texture_desc.ClearValue.Format = texture_desc.Format;
  std::memset(texture_desc.ClearValue.Color, 0,
              sizeof(texture_desc.ClearValue.Color));

  Diligent::TextureSubResData texture_sub_res_data;
  texture_sub_res_data.pData = data->pixels;
  texture_sub_res_data.Stride = data->pitch;

  Diligent::TextureData texture_data;
  texture_data.pSubResources = &texture_sub_res_data;
  texture_data.NumSubresources = 1;

  device->CreateTexture(texture_desc, &texture_data, texture);
  ConsiderLoadDrain();
}

}  // namespace renderer