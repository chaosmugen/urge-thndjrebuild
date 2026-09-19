// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RENDERER_UTILS_TEXTURE_UTILS_H_
#define RENDERER_UTILS_TEXTURE_UTILS_H_

#include "SDL3/SDL_surface.h"

#include "Graphics/GraphicsEngine/interface/RenderDevice.h"
#include "Graphics/GraphicsEngine/interface/Texture.h"

#include "base/math/rectangle.h"

namespace renderer {

// The context the load drain below submits through. Set once during startup.
void SetLoadDrainContext(Diligent::IDeviceContext* context);

// Whether the active backend is Vulkan. The drain works around a Vulkan driver
// defect, so it stays off on GLES where it would only add sync cost.
void SetVulkanBackend(bool vulkan);

// Retires pending GPU work while a map load streams through texture operations.
// A load submits thousands of texture creations and bitmap blits inside a single
// script frame and never retires them; once enough of that work is left pending,
// this Adreno Vulkan driver parks the render thread forever in
// adreno_drawctxt_wait (no error, no recovery - the process has to be killed).
// Operations arriving back to back are treated as a load: from the third one on,
// each does Flush + WaitForIdle, keeping the pending set at a couple of
// operations. Normal play issues its blits far apart, so it never engages.
void ConsiderLoadDrain();

// Called after a frame reaches the screen. A load is a single very long script
// frame, so nothing is presented for its whole duration, while normal play
// presents constantly - which is what tells ConsiderLoadDrain() the difference.
void NotifyFramePresented();

void CreateTexture2D(
    Diligent::IRenderDevice* device,
    Diligent::ITexture** texture,
    const std::string& name,
    const base::Vec2i& size,
    Diligent::USAGE usage = Diligent::USAGE_DEFAULT,
    Diligent::BIND_FLAGS bind_flags = Diligent::BIND_SHADER_RESOURCE,
    Diligent::CPU_ACCESS_FLAGS access = Diligent::CPU_ACCESS_NONE,
    Diligent::TEXTURE_FORMAT format =
        Diligent::TEXTURE_FORMAT::TEX_FORMAT_RGBA8_UNORM);

void CreateTexture2D(
    Diligent::IRenderDevice* device,
    Diligent::ITexture** texture,
    const std::string& name,
    SDL_Surface* data,
    Diligent::USAGE usage = Diligent::USAGE_DEFAULT,
    Diligent::BIND_FLAGS bind_flags = Diligent::BIND_SHADER_RESOURCE,
    Diligent::CPU_ACCESS_FLAGS access = Diligent::CPU_ACCESS_NONE,
    Diligent::TEXTURE_FORMAT format =
        Diligent::TEXTURE_FORMAT::TEX_FORMAT_RGBA8_UNORM);

}  // namespace renderer

#endif  //! RENDERER_UTILS_TEXTURE_UTILS_H_
