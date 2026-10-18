// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RENDERER_UTILS_TEXTURE_UTILS_H_
#define RENDERER_UTILS_TEXTURE_UTILS_H_

#include <type_traits>

#include "SDL3/SDL_surface.h"

#include "Graphics/GraphicsEngine/interface/DeviceObject.h"
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
// A load is one enormous script frame, so nothing is presented for its whole
// duration: the drain only engages once the current frame has been running for a
// while, which keeps it out of play entirely - including battle animations,
// which blit heavily but still present every frame.
void ConsiderLoadDrain();

// Deferred release, and the leading explanation for the one-frame region loss.
//
// A texture that the game drops while draws recorded in earlier frames may still be
// sampling it must not be destroyed on the spot. On Vulkan the device hands a destroyed
// texture's memory to the next allocation, so one frame paints a piece of some other
// image over a region - which is exactly the recorded shape of the defect: one frame, one
// region, byte-identical again on the next, only on Vulkan, with every CPU-side value and
// every texture readback correct. GL defers the deletion itself until the object can no
// longer be in use, which is why the same build never loses a region on GLES. It is the
// release-side counterpart of the frame sets (SpriteBatch::FrameSetIndex) that keep the CPU
// from overwriting buffers still in flight, and like them it is not a synchronisation: a
// drain gives the CPU a moment of rest and the next frame races ahead again, which is why
// every drain this project tried - six positions, on both device contexts - measured
// nothing at all in real play.
//
// DeferredTextureRelease takes the caller's reference into a graveyard that is emptied
// kDeferredReleaseFrames frames later (see AdvanceDeferredRelease), so the object outlives
// every draw that could still be reading it. urge_debug/texring.txt = 0 turns it off.
//
// The graveyard holds IDeviceObject, so the same ring covers every resource whose memory the
// device recycles on the spot: textures, the views hanging off them, and buffers. Views and
// buffers matter for the same reason the textures do - a descriptor still in flight that names
// a destroyed image view, or a draw still reading a vertex buffer that was released and
// replaced, is the same "somebody else's object" hazard one level up.
constexpr int32_t kDeferredReleaseFrames = 8;
void DeferredReleaseObject(Diligent::IDeviceObject* object);
void AdvanceDeferredRelease();
bool DeferredReleaseEnabled();

// Transfers the caller's reference into the graveyard - the caller's pointer ends up empty -
// or releases it at once when the ring is switched off. Takes any Diligent GPU object.
template <typename ObjectType>
void DeferredTextureRelease(Diligent::RefCntAutoPtr<ObjectType>& object) {
  static_assert(std::is_base_of<Diligent::IDeviceObject, ObjectType>::value,
                "the deferred release ring only takes Diligent GPU objects");
  if (!object)
    return;
  if (!DeferredReleaseEnabled()) {
    object.Release();
    return;
  }
  DeferredReleaseObject(object.Detach());
}

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
