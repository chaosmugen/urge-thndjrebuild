// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/canvas/canvas_scheduler.h"

#include "renderer/utils/texture_utils.h"

namespace content {

CanvasScheduler::CanvasScheduler(renderer::RenderDevice* render_device,
                                 Diligent::IDeviceContext* primary_context,
                                 renderer::PipelineSet* loader)
    : device_(render_device),
      context_(primary_context),
      common_quad_batch_(renderer::QuadBatch::Make(**device_)) {
  // Create initial blt cache
  renderer::CreateTexture2D(**device_, &generic_blt_texture_,
                            "canvas.generic.blt.cache", base::Vec2i(2 << 8));
}

CanvasScheduler::~CanvasScheduler() = default;

void CanvasScheduler::SubmitPendingPaintCommands() {
  for (auto it = children_.head(); it != children_.end(); it = it->next()) {
    // Submit all pending commands (except Blt, StretchBlt)
    it->value()->SubmitQueuedCommands(/*at_frame_boundary=*/true);
  }
}

// TEMPORARY A/B switch, see SetupRenderTarget. Marker: URGE_RTKEEP
static bool CanvasKeepRenderTargetInternal() {
  static int32_t s_poll = 0;
  static bool s_enabled = false;
  if (++s_poll >= 30) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "rtkeep.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_enabled = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/rtkeep.txt");
    }
  }
  return s_enabled;
}

// TEMPORARY canvas-target audit. Marker: URGE_CANVAS_RT
//
// The frame ends and reopens the Vulkan render scope 19 to 34 times (measured with [PassAudit]),
// every one of them a render-target change, and on this tile-based GPU each change stores and
// reloads every tile - the only mechanism found so far that can lose one rectangle of a frame while
// all application inputs are correct. This counts how the canvas contributes: distinct targets per
// frame, and how many of its bindings repeat a target the frame has already bound, which is the part
// that could in principle be merged away. Reset and printed once per frame by the [PassAudit] block.
extern "C" int g_urge_canvas_rt_calls = 0;
extern "C" int g_urge_canvas_rt_resets = 0;
extern "C" int g_urge_canvas_rt_repeat = 0;
extern "C" void* g_urge_canvas_rt_seen[64] = {};
extern "C" int g_urge_canvas_rt_seen_count = 0;
// The order in which the frame binds them, so the pattern - alternating A/B/A/B or grouped - can be
// told apart. Marker: URGE_CANVAS_RT
extern "C" void* g_urge_canvas_rt_seq[16] = {};
extern "C" int g_urge_canvas_rt_seq_count = 0;

// TEMPORARY A/B switch, see SetupRenderTarget. Marker: URGE_RTNORM
static bool CanvasNormalizeTargetInternal() {
  static int32_t s_poll = 0;
  static bool s_enabled = false;
  if (++s_poll >= 30) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "rtnorm.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_enabled = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/rtnorm.txt");
    }
  }
  return s_enabled;
}

void CanvasScheduler::SetupRenderTarget(Diligent::ITextureView* render_target,
                                        Diligent::ITextureView* depth_stencil,
                                        bool clear_target) {
  // See g_urge_canvas_rt_calls. Marker: URGE_CANVAS_RT
  if (render_target) {
    ++g_urge_canvas_rt_calls;
    if (g_urge_canvas_rt_seq_count < 16)
      g_urge_canvas_rt_seq[g_urge_canvas_rt_seq_count++] = render_target;
    bool repeat = false;
    for (int32_t i = 0; i < g_urge_canvas_rt_seen_count; ++i) {
      if (g_urge_canvas_rt_seen[i] == render_target) {
        repeat = true;
        break;
      }
    }
    if (repeat)
      ++g_urge_canvas_rt_repeat;
    else if (g_urge_canvas_rt_seen_count < 64)
      g_urge_canvas_rt_seen[g_urge_canvas_rt_seen_count++] = render_target;
  } else {
    ++g_urge_canvas_rt_resets;
  }

  // Setup new render target
  if (render_target) {
    // TEMPORARY normalisation arm. Marker: URGE_RTNORM
    //
    // [PassAudit] shows 19 to 32 render scopes ended per frame while only ever leaving *one*
    // distinct framebuffer and never leaving a null one - so the frame repeatedly reopens a scope on
    // the same target with a different dynamic-rendering configuration. The configuration that
    // varies here is the depth attachment (and, on a few call sites, the clear flag); the same
    // colour target bound once with a depth-stencil and once without is two different framebuffers
    // to Diligent, so every alternation costs a scope. This arm always binds it the same way, so a
    // frame can reuse one scope; it is deliberately blunt (no depth, no clear) because the point is
    // to see whether the boundary count collapses at all.
    if (CanvasNormalizeTargetInternal()) {
      context_->SetRenderTargets(
          1, &render_target, nullptr,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      return;
    }

    context_->SetRenderTargets(
        1, &render_target, depth_stencil,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Apply clear buffer if need
    if (clear_target) {
      float clear_color[] = {0, 0, 0, 0};
      context_->ClearRenderTarget(
          render_target, clear_color,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

      if (depth_stencil) {
        // Clear depth buffer if need
        context_->ClearDepthStencil(
            depth_stencil, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      }
    }

    return;
  }

  // Reset render target state
  //
  // TEMPORARY A/B switch. Marker: URGE_RTKEEP
  //
  // This reset ends the Vulkan render scope outright, and the canvas issues it after *every* draw
  // command: twelve of those a frame, each followed by a command that immediately binds the very
  // same target again. The engine therefore ends and reopens a render scope roughly twenty-five
  // times a frame (measured with [PassAudit]), and on this tile-based GPU every one of those
  // boundaries stores and reloads every tile of the screen - the one mechanism we have found that
  // can lose a rectangle of a frame while every input the application supplies is correct, and the
  // one that GLES, which has no render scopes at all, cannot exhibit. Skipping the reset leaves the
  // target bound; the next SetupRenderTarget, or any draw that binds its own, still switches it
  // correctly. If the loss rate falls with this on, the boundaries are the mechanism and reducing
  // them is the evasion.
  if (!CanvasKeepRenderTargetInternal())
    context_->SetRenderTargets(
        0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

Diligent::ITexture* CanvasScheduler::RequireBltCacheTexture(
    const base::Vec2i& size) {
  if (static_cast<int32_t>(generic_blt_texture_->GetDesc().Width) < size.x ||
      static_cast<int32_t>(generic_blt_texture_->GetDesc().Height) < size.y) {
    base::Vec2i new_size;
    new_size.x =
        std::max<int32_t>(size.x, generic_blt_texture_->GetDesc().Width);
    new_size.y =
        std::max<int32_t>(size.y, generic_blt_texture_->GetDesc().Height);

    renderer::DeferredTextureRelease(generic_blt_texture_);
    renderer::CreateTexture2D(**device_, &generic_blt_texture_,
                              "canvas.generic.blt.cache", new_size);
  }

  return generic_blt_texture_;
}

renderer::RenderDevice* CanvasScheduler::GetRenderDevice() {
  return device_;
}

Diligent::IDeviceContext* CanvasScheduler::GetDiscreteRenderContext() {
  return context_;
}

}  // namespace content
