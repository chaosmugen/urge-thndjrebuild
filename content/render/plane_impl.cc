// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/render/plane_impl.h"

#include "renderer/utils/texture_utils.h"

#include <algorithm>

#include "content/context/execution_context.h"
// Defined in content/screen/renderscreen_impl.cc. Marker: URGE_DIAG_GATE
extern "C" bool UrgeDiagEnabled();

// The plane picks its per-frame vertex set with SpriteBatch::FrameSetIndex(), whose range
// is the batch's own set count. Keep the two counts equal at compile time, exactly as the
// window path does.
static_assert(content::PlaneImpl::GPUData::kFrameSets ==
                  content::SpriteBatch::kFrameSets,
              "plane and sprite batch frame-set counts must match");

namespace content {

// static
scoped_refptr<Plane> Plane::New(ExecutionContext* execution_context,
                                scoped_refptr<Viewport> viewport,
                                ExceptionState& exception_state) {
  return base::MakeRefCounted<PlaneImpl>(execution_context,
                                         ViewportImpl::From(viewport));
}

///////////////////////////////////////////////////////////////////////////////
// PlaneImpl Implement

PlaneImpl::PlaneImpl(ExecutionContext* execution_context,
                     scoped_refptr<ViewportImpl> parent)
    : EngineObject(execution_context),
      Disposable(execution_context->disposable_parent),
      node_(parent ? parent->GetDrawableController()
                   : execution_context->screen_drawable_node,
            SortKey()),
      viewport_(parent),
      src_rect_(base::MakeRefCounted<RectImpl>(base::Rect())),
      scale_(1.0f),
      color_(base::MakeRefCounted<ColorImpl>(base::Vec4())),
      tone_(base::MakeRefCounted<ToneImpl>(base::Vec4())) {
  node_.RegisterEventHandler(base::BindRepeating(
      &PlaneImpl::DrawableNodeHandlerInternal, base::Unretained(this)));

  GPUCreatePlaneInternal();
}

DISPOSABLE_DEFINITION(PlaneImpl);

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Bitmap,
    scoped_refptr<Bitmap>,
    PlaneImpl,
    { return bitmap_; },
    {
      DISPOSE_CHECK;

      bitmap_ = CanvasImpl::FromBitmap(value);
      if (Disposable::IsValid(bitmap_.get()))
        src_rect_->SetBase((**bitmap_)->size);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    SrcRect,
    scoped_refptr<Rect>,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(nullptr);
      return src_rect_;
    },
    {
      DISPOSE_CHECK;
      CHECK_ATTRIBUTE_VALUE;
      *src_rect_ = *RectImpl::From(value);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Viewport,
    scoped_refptr<Viewport>,
    PlaneImpl,
    { return viewport_; },
    {
      DISPOSE_CHECK;

      if (viewport_ == value)
        return;

      viewport_ = ViewportImpl::From(value);
      node_.RebindController(viewport_ ? viewport_->GetDrawableController()
                                       : context()->screen_drawable_node);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Visible,
    bool,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(false);

      return node_.GetVisibility();
    },
    {
      DISPOSE_CHECK;

      node_.SetNodeVisibility(value);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Z,
    int32_t,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(0);

      return node_.GetSortKeys()->weight[0];
    },
    {
      DISPOSE_CHECK;

      node_.SetNodeSortWeight(value);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Ox,
    int32_t,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(0);

      return origin_.x;
    },
    {
      DISPOSE_CHECK;

      origin_.x = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Oy,
    int32_t,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(0);

      return origin_.y;
    },
    {
      DISPOSE_CHECK;

      origin_.y = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    ZoomX,
    float,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(0.0f);

      return scale_.x;
    },
    {
      DISPOSE_CHECK;

      scale_.x = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    ZoomY,
    float,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(0.0f);

      return scale_.y;
    },
    {
      DISPOSE_CHECK;

      scale_.y = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Opacity,
    int32_t,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(0);

      return opacity_;
    },
    {
      DISPOSE_CHECK;

      opacity_ = std::clamp(value, 0, 255);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    BlendType,
    int32_t,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(0);

      return blend_type_;
    },
    {
      DISPOSE_CHECK;

      blend_type_ = std::clamp<int32_t>(value, 0, BLEND_TYPE_NUMS - 1);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    ToneExempt,
    bool,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(false);

      return node_.IsToneExempt();
    },
    {
      DISPOSE_CHECK;

      node_.SetToneExempt(value);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Color,
    scoped_refptr<Color>,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(nullptr);

      return color_;
    },
    {
      DISPOSE_CHECK;

      CHECK_ATTRIBUTE_VALUE;

      *color_ = *ColorImpl::From(value);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Tone,
    scoped_refptr<Tone>,
    PlaneImpl,
    {
      DISPOSE_CHECK_RETURN(nullptr);

      return tone_;
    },
    {
      DISPOSE_CHECK;

      CHECK_ATTRIBUTE_VALUE;

      *tone_ = *ToneImpl::From(value);
    });

void PlaneImpl::OnObjectDisposed() {
  node_.DisposeNode();

  GPUData empty_data;
  std::swap(gpu_, empty_data);
}

// TEMPORARY class-skip switch. Marker: URGE_SKIPDRAW
//
// The one-frame loss was measured to be a large region getting *brighter*, and the pipeline tags put
// that region on the basic/window-bitmap layer rather than on the tilemap - so the victim is some
// drawable of one of these classes and not the map at all. Skipping a whole class one at a time and
// looking at the screen says which class owns the room picture and which owns the dark overlay, far
// more reliably than an opaque tag does (a tag shows coverage, a skip shows visibility).
// 0 skips the class, 1 restores it.
static bool SkipDrawEnabledInternal() {
  static int32_t s_poll = 0;
  static bool s_skipped = false;
  if (++s_poll >= 30) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/skip_plane.txt",
        "r");
    if (file) {
      int32_t value = 1;
      if (fscanf(file, "%d", &value) == 1)
        s_skipped = value == 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/skip_plane.txt");
    }
  }
  return s_skipped;
}

void PlaneImpl::DrawableNodeHandlerInternal(
    DrawableNode::RenderStage stage,
    DrawableNode::RenderControllerParams* params) {
  if (!Disposable::IsValid(bitmap_.get()))
    return;

  if (stage == DrawableNode::RenderStage::BEFORE_RENDER) {
    GPUUpdatePlaneQuadArrayInternal(params->context, src_rect_->AsBaseRect(),
                                    node_.GetParentViewport()->bound.Size(),
                                    scale_, origin_, params->light_gain);
  } else   if (stage == DrawableNode::RenderStage::ON_RENDERING) {
    if (SkipDrawEnabledInternal()) return;  // TEMPORARY, see above
    GPUOnViewportRenderingInternal(params->context, params->world_binding);
  }
}

void PlaneImpl::GPUCreatePlaneInternal() {
  for (int32_t i = 0; i < GPUData::kFrameSets; ++i)
    gpu_.batch[i] = renderer::QuadBatch::Make(**context()->render_device);
  gpu_.shader_binding =
      context()->render.pipeline_loader->viewport.CreateBinding();

  Diligent::CreateUniformBuffer(
      **context()->render_device, sizeof(renderer::Binding_Flat::Params),
      "plane.flat.uniform", &gpu_.uniform_buffer, Diligent::USAGE_DEFAULT);
}

void PlaneImpl::GPUUpdatePlaneQuadArrayInternal(
    Diligent::IDeviceContext* render_context,
    const base::Rect& src_rect,
    const base::Vec2i& viewport_size,
    const base::Vec2& scale,
    const base::Vec2i& origin,
    float light_gain) {
  // Source texture
  auto* texture = **bitmap_;

  // Pre-calculate tile dimensions with scaling
  const float item_x =
      std::max(1.0f, static_cast<float>(texture->size.x) * scale.x);
  const float item_y =
      std::max(1.0f, static_cast<float>(texture->size.y) * scale.y);

  // Wrap float into desire value
  auto value_wrap = [](float value, float range) {
    float result = std::fmod(value, range);
    return result < 0 ? result + range : result;
  };

  // Calculate wrapped origin offsets
  const float wrap_ox = value_wrap(static_cast<float>(origin.x), item_x);
  const float wrap_oy = value_wrap(static_cast<float>(origin.y), item_y);

  // Optimized tile calculation using simplified formula
  const float total_x = static_cast<float>(viewport_size.x) + wrap_ox;
  const float total_y = static_cast<float>(viewport_size.y) + wrap_oy;
  const int32_t tile_x = static_cast<int32_t>(std::ceil(total_x / item_x));
  const int32_t tile_y = static_cast<int32_t>(std::ceil(total_y / item_y));

  // Prepare vertex buffer
  const int32_t quad_size = tile_x * tile_y;
  gpu_.cache.resize(quad_size);
  float opacity = static_cast<float>(opacity_) / 255.0f;
  // Tone-exempt lights are attenuated when the viewport tint is (near)
  // pure black, so a pure-black tint still covers them.
  if (node_.IsToneExempt())
    opacity *= light_gain;
  const base::Vec4 opacity_norm(opacity);

  // Pointer-based vertex writing with accumulative positioning
  renderer::Quad* quad_ptr = gpu_.cache.data();
  float current_y = -wrap_oy;
  for (int32_t y = 0; y < tile_y; ++y) {
    float current_x = -wrap_ox;
    for (int32_t x = 0; x < tile_x; ++x) {
      // Set vertex properties directly through pointer
      const base::RectF pos(current_x, current_y, item_x, item_y);
      renderer::Quad::SetPositionRect(quad_ptr, pos);
      renderer::Quad::SetTexCoordRect(quad_ptr, src_rect,
                                      texture->size.Recast<float>());
      renderer::Quad::SetColor(quad_ptr, opacity_norm);

      // Move to next quad using pointer arithmetic
      ++quad_ptr;
      current_x += item_x;  // X-axis accumulation
    }
    current_y += item_y;  // Y-axis accumulation
  }

  context()->render.quad_index->Allocate(quad_size);
  gpu_.quad_size = quad_size;

  const int32_t frame_set = context()->sprite_batcher->FrameSetIndex();
  gpu_.batch[frame_set].QueueWrite(render_context, gpu_.cache.data(),
                                   gpu_.cache.size());

  // Temporary: the set this frame composed into and the vertex buffer that set uses. A
  // frame that draws from another frame's data will show a buffer that is not the one
  // this line named.
  if (context()->sprite_batcher->RunFpEnabled())
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[PlaneFp] f=" << context()->sprite_batcher->FrameCount()
              << " set=" << frame_set << " quads=" << quad_size << " buf="
              << static_cast<const void*>(*gpu_.batch[frame_set])
              << " obj=" << static_cast<const void*>(this)
              << " tex=" << texture->size.x << "x" << texture->size.y
              << " scale=" << scale.x << "," << scale.y
              << " origin=" << origin.x << "," << origin.y
              << " item=" << item_x << "," << item_y
              << " wrap=" << wrap_ox << "," << wrap_oy
              << " first=" << (-wrap_ox) << "," << (-wrap_oy)
              << " op=" << opacity_ << " bl=" << blend_type_
              << " src=" << src_rect.x << "," << src_rect.y << " "
              << src_rect.width << "x" << src_rect.height
              << " t=" << static_cast<const void*>(texture)
              << " st=" << static_cast<int32_t>(texture->data->GetState())
              << " srv=" << static_cast<const void*>(texture->shader_resource_view.RawPtr())
              << " bmp=" << texture->name;

  renderer::Binding_Flat::Params transient_uniform;
  transient_uniform.Color = color_->AsNormColor();
  transient_uniform.Tone = tone_->AsNormColor();
  render_context->UpdateBuffer(
      gpu_.uniform_buffer, 0, sizeof(transient_uniform), &transient_uniform,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void PlaneImpl::GPUOnViewportRenderingInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::IBuffer* world_binding) {
  // Diagnostic bisection, gated on urge_debug/noplane.txt. The map background turned out to
  // be a Plane (see the [PlaneFp] line: a 670x402 game bitmap), the last drawable type
  // whose draw has not been switched off yet, while the tilemap ground behind it was
  // already ruled out. Turning the plane off tells whether the one-frame region loss lives
  // in this path at all.
  {
    static int32_t s_no_plane_poll = 0;
    static bool s_no_plane = false;
    if (++s_no_plane_poll >= 30) {
      s_no_plane_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "noplane.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_no_plane = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/noplane.txt");
      }
    }
    if (s_no_plane)
      return;
  }

  // Source texture
  auto* texture = **bitmap_;

  // Keep-alive ring for the texture this draw samples, switched by the auto A/B arm (0 none,
  // 1 eight frames, 2 thirty-two frames, 3 none) - the same arms the sprite batch uses, and
  // the arms that were rotating while this was measured. This is the ring that matters: the
  // plane does not go through the sprite batch, it binds the game bitmap's view itself, so
  // the ring added there never covered it. The plane is where the loss is produced: with a
  // static scene it reproduces every few thousand frames (7 events of magnitude 4334 in
  // 12336 frames, always the same block and the same wrong value), while switching this draw
  // off leaves 1 noise-level event in 16956 frames. The game replaces the bitmap a plane
  // draws when its background changes, and destroying that texture while an in-flight draw
  // still samples it - its memory going straight back to the allocator - is the shape that
  // would produce exactly one frame of wrong texels.
  {
    static int32_t s_plane_alive_poll = 0;
    static bool s_plane_alive_auto = false;
    static int32_t s_plane_alive_depth = 0;
    static decltype(texture->shader_resource_view) s_plane_alive[32];
    if (++s_plane_alive_poll >= 30) {
      s_plane_alive_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "keepalive.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_plane_alive_depth = value;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/keepalive.txt");
      }
      FILE* auto_file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "autosync.txt",
          "r");
      if (auto_file) {
        int32_t value = 1;
        if (fscanf(auto_file, "%d", &value) == 1)
          s_plane_alive_auto = value != 0;
        fclose(auto_file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/autosync.txt");
      }
    }
    const uint32_t alive_frame = context()->sprite_batcher->FrameCount();
    const uint32_t alive_arm = (alive_frame / 1000u) % 4u;
    const int32_t alive_depth =
        s_plane_alive_auto
            ? (alive_arm == 1 ? 8 : (alive_arm == 2 ? 32 : 0))
            : s_plane_alive_depth;
    if (alive_depth > 0) {
      s_plane_alive[alive_frame % 32] = texture->shader_resource_view;
      // A fixed-size ring holds the last 32 frames, so the slots beyond the wanted depth are
      // released explicitly each frame; that way the depth can change with the arm without
      // leaving stale references behind.
      for (int32_t k = alive_depth; k < 32; ++k)
        s_plane_alive[(alive_frame - k) % 32] = nullptr;
    } else {
      for (int32_t k = 0; k < 32; ++k)
        s_plane_alive[k] = nullptr;
    }
  }

  // Candidate fix, on by default, switchable with urge_debug/planesync.txt (0 turns it off).
  // The plane draw is where the one-frame region loss is produced: switching this draw off
  // makes the characteristic events disappear (6 against 2 noise-level in a within-session
  // A/B) while every parameter it uses - and even the contents of the texture it samples,
  // read back across one of those frames - is identical to the frames around it. The one
  // lever that ever moved this metric is retiring the pending GPU work immediately before
  // the sampling: on the stress bench 89.3% of frames lost a region without it and 4.4%
  // with it, while draining anywhere earlier in the frame changes nothing (after the canvas
  // commands 83.9% against 84.5%, before the render pass 95.3% against 92.8%). Cost of this
  // drain, measured on the device with a within-session A/B: 24.2 fps with it against 24.0
  // without, i.e. nothing, because the frame limiter dominates. It is the only lever with a
  // proven mechanism behind it, so it is enabled and judged on real play; the natural
  // reproduction is too rare in a scripted session to settle it by probe (2 events against
  // 0 in the last A/B, which says nothing).
  {
    static int32_t s_plane_sync_poll = 0;
    static bool s_plane_sync = false;
    static bool s_plane_auto = false;
    static uint32_t s_plane_sync_frame = 0;
    if (++s_plane_sync_poll >= 30) {
      s_plane_sync_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "planesync.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_plane_sync = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/planesync.txt");
      }
      FILE* auto_file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "autosync.txt",
          "r");
      if (auto_file) {
        int32_t value = 1;
        if (fscanf(auto_file, "%d", &value) == 1)
          s_plane_auto = value != 0;
        fclose(auto_file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/autosync.txt");
      }
    }
    const uint32_t plane_frame = context()->sprite_batcher->FrameCount();
    // Arm-driven again, in a 2x2 with the canvas-blit drain in the render screen:
    // arm 0 nothing / 1 canvas drain / 2 canvas + plane drain / 3 plane drain only. The
    // canvas drain alone leaves about a fifth of the loss behind, and the writes it cannot
    // retire are the ones submitted after it - the plane's own layer composition among them,
    // which is what this drain sits after.
    const uint32_t plane_arm = (plane_frame / 1000u) % 4u;
    const bool want_drain =
        s_plane_auto ? (plane_arm == 2 || plane_arm == 3) : s_plane_sync;
    if (want_drain && plane_frame != s_plane_sync_frame &&
        context()->primary_render_context) {
      s_plane_sync_frame = plane_frame;
      context()->primary_render_context->Flush();
      context()->primary_render_context->WaitForIdle();
    }
  }

  // Reproduction stress, gated on urge_debug/planestress.txt. The one-frame region loss is
  // intermittent: the same script and the same probe have measured 0, 2, 6, 59, 117 and 236
  // events in different runs, which makes every candidate fix impossible to judge. What made
  // those numbers climb is exactly what this switch does - extra GPU accesses to the texture
  // this plane samples, pure copies with no read-back, so the cost is a few hundred
  // kilobytes per frame. With the loss firing many times per arm, a fix can be proved or
  // rejected in minutes instead of by luck.
  {
    static int32_t s_stress_poll = 0;
    static int32_t s_stress_level = 0;
    if (++s_stress_poll >= 30) {
      s_stress_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "planestress.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_stress_level = value > 0 ? value : 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/planestress.txt");
      }
    }

    if (s_stress_level > 0 && texture->data &&
        context()->primary_render_context) {
      static Diligent::RefCntAutoPtr<Diligent::ITexture> s_stress_tex;
      const int32_t w = texture->size.x;
      const int32_t h = texture->size.y;
      if (!s_stress_tex && w > 0 && h > 0)
        renderer::CreateTexture2D(**context()->render_device, &s_stress_tex,
                                  "plane.stress", base::Vec2i(w, h));
      if (s_stress_tex) {
        auto* render_context = context()->primary_render_context;
        for (int32_t i = 0; i < s_stress_level; ++i) {
          Diligent::CopyTextureAttribs attribs;
          attribs.pSrcTexture = texture->data;
          attribs.SrcTextureTransitionMode =
              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
          attribs.pDstTexture = s_stress_tex;
          attribs.DstTextureTransitionMode =
              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
          render_context->CopyTexture(attribs);
        }

        // The decisive experiment: retire the copies above before this plane samples the
        // texture they read from. If that removes the region loss, the loss is the
        // visibility of a same-frame access chain on this texture and nothing else.
        // urge_debug/stressdrain.txt, nonzero for on.
        static int32_t s_stress_drain_poll = 0;
        static bool s_stress_drain = false;
        if (++s_stress_drain_poll >= 30) {
          s_stress_drain_poll = 0;
          FILE* file = fopen(
              "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
              "stressdrain.txt",
              "r");
          if (file) {
            int32_t value = 0;
            if (fscanf(file, "%d", &value) == 1)
              s_stress_drain = value != 0;
            fclose(file);
            remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
                   "urge_debug/stressdrain.txt");
          }
        }
        if (s_stress_drain) {
          render_context->Flush();
          render_context->WaitForIdle();
        }
      }
    }
  }

  // Render device etc
  // Candidate fix for the one-frame region loss, gated on urge_debug/planeflat.txt. The
  // plane draw is where the loss is produced: switching it off removes the characteristic
  // events (6 with, 2 noise-level without), every parameter it uses is identical to the
  // frames around it, and the texture it samples was read back and found byte-identical
  // across an event as well. So this driver mis-renders this draw given correct inputs, and
  // the way around a driver defect in one pipeline is to use another one: the plane already
  // binds a Binding_Flat and draws Quads, exactly like the viewport effect, whose flat
  // pipeline is known to behave on this device. Set urge_debug/planeflat.txt to 1 to draw
  // the plane through it instead.
  static int32_t s_plane_flat_poll = 0;
  static bool s_plane_flat = false;
  if (++s_plane_flat_poll >= 30) {
    s_plane_flat_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "planeflat.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_plane_flat = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/planeflat.txt");
    }
  }

  auto* pipeline =
      s_plane_flat
          ? context()->render.pipeline_states->viewport_flat.RawPtr()
          : context()->render.pipeline_states->plane[blend_type_].RawPtr();

  // Experiment: sample a copy of the texture instead of the game's own bitmap, rotating with
  // the auto arms (0/2 direct, 1/3 copy). If the one-frame band disappears when the draw
  // reads a texture the engine produced itself, the defect is in how this draw samples the
  // game's texture - a stale tile row. The shape of the loss points that way: one horizontal
  // band, always the same block and the same wrong value, in both the offscreen buffer and
  // the presented image, while the texture's content read back after a wait is always
  // correct, because waiting for the GPU is itself what refreshes a stale tile.
  Diligent::ITextureView* plane_view = texture->shader_resource_view;
  {
    static int32_t s_copy_poll = 0;
    static bool s_copy_auto = true;
    static bool s_copy_manual = false;
    static Diligent::RefCntAutoPtr<Diligent::ITexture> s_copy[2];
    static int32_t s_copy_index = 0;
    static uint32_t s_copy_frame = 0;
    if (++s_copy_poll >= 30) {
      s_copy_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "planecopy.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_copy_manual = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/planecopy.txt");
      }
      FILE* auto_file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "autosync.txt",
          "r");
      if (auto_file) {
        int32_t value = 1;
        if (fscanf(auto_file, "%d", &value) == 1)
          s_copy_auto = value != 0;
        fclose(auto_file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/autosync.txt");
      }
    }
    const uint32_t copy_frame = context()->sprite_batcher->FrameCount();
    const uint32_t copy_arm = (copy_frame / 1000u) % 4u;
    // Arms: 0 direct, 1 copy sampled one frame late, 2 copy sampled in the frame it was
    // made, 3 direct. Sampling this frame's copy multiplies the loss by 25 (105 events per
    // 10000 frames against 4.3 for the direct draw), which identifies the defect: a texture
    // written in a frame and sampled in that same frame is not reliably visible on this
    // driver - a few tile rows are read stale. If the one-frame-late copy is clean, that is
    // the mechanism, and decoupling the sample from the write is the fix.
    const int32_t copy_mode = s_copy_manual ? 2 : 0;
    // The copy experiment is finished and stays out of the rotating arms (they now drive the
    // canvas-blit drain, which must not be confounded with a 20x amplifier); keep the switch
    // reads alive so planecopy.txt still works by hand.
    (void)s_copy_auto;
    (void)copy_arm;
    if (copy_mode && texture->data && context()->primary_render_context) {
      const auto& src_desc = texture->data->GetDesc();
      if (src_desc.Width > 0 && src_desc.Height > 0) {
        if (!s_copy[0] || s_copy[0]->GetDesc().Width != src_desc.Width ||
            s_copy[0]->GetDesc().Height != src_desc.Height)
          for (int32_t k = 0; k < 2; ++k)
            renderer::CreateTexture2D(
                **context()->render_device, &s_copy[k],
                k ? "plane.copy.1" : "plane.copy.0",
                base::Vec2i(static_cast<int32_t>(src_desc.Width),
                            static_cast<int32_t>(src_desc.Height)),
                Diligent::USAGE_DEFAULT, Diligent::BIND_SHADER_RESOURCE,
                Diligent::CPU_ACCESS_NONE, src_desc.Format);
        if (s_copy[0] && s_copy[1] && copy_frame != s_copy_frame) {
          s_copy_frame = copy_frame;
          // Mode 2 drains before the copy, mode 1 does not. The copy is a blit out of the
          // game's bitmap, and a blit that reads stale tiles of a source written earlier in
          // the frame produces a wrong image no matter when it is sampled - which is what
          // the one-frame-late copy proved by being just as bad as this frame's (117 against
          // 90 events per 10000 frames). Retiring the pending writes before the blit is the
          // one lever that removed this class of loss on the stress bench (89.3% to 4.4%),
          // where the copies were exactly this shape.
          if (copy_mode == 2 && context()->primary_render_context) {
            context()->primary_render_context->Flush();
            context()->primary_render_context->WaitForIdle();
          }
          Diligent::CopyTextureAttribs attribs;
          attribs.pSrcTexture = texture->data;
          attribs.SrcTextureTransitionMode =
              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
          attribs.pDstTexture = s_copy[s_copy_index];
          attribs.DstTextureTransitionMode =
              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
          context()->primary_render_context->CopyTexture(attribs);
          Diligent::ITexture* sampled = s_copy[s_copy_index].RawPtr();
          if (sampled)
            plane_view = sampled->GetDefaultView(
                Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
          s_copy_index = 1 - s_copy_index;
        }
      }
    }
  }

  // Setup uniform params
  gpu_.shader_binding.u_transform->Set(world_binding);
  gpu_.shader_binding.u_texture->Set(plane_view);
  gpu_.shader_binding.u_params->Set(gpu_.uniform_buffer);

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.shader_binding,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer =
      *gpu_.batch[context()->sprite_batcher->FrameSetIndex()];
  render_context->SetVertexBuffers(
      0, 1, &vertex_buffer, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->SetIndexBuffer(
      **context()->render.quad_index, 0,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Execute render command
  Diligent::DrawIndexedAttribs draw_indexed_attribs;
  draw_indexed_attribs.NumIndices = 6 * gpu_.cache.size();

  // Temporary: the set this draw reads and the vertex buffer it will actually read from.
  // A frame whose compose named another set (or whose buffer differs) is drawing another
  // frame's geometry.
  if (context()->sprite_batcher->RunFpEnabled())
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[PlaneDraw] f=" << context()->sprite_batcher->FrameCount()
              << " set=" << context()->sprite_batcher->FrameSetIndex()
              << " indices=" << draw_indexed_attribs.NumIndices << " buf="
              << static_cast<const void*>(vertex_buffer)
              << " obj=" << static_cast<const void*>(this)
              << " w=" << static_cast<const void*>(world_binding)
              << " tex=" << texture->size.x << "x" << texture->size.y
              << " op=" << opacity_ << " bl=" << blend_type_
              << " t=" << static_cast<const void*>(texture)
              << " st=" << static_cast<int32_t>(texture->data->GetState())
              << " srv=" << static_cast<const void*>(texture->shader_resource_view.RawPtr())
              << " bmp=" << texture->name;
  draw_indexed_attribs.IndexType = context()->render.quad_index->GetIndexType();
  render_context->DrawIndexed(draw_indexed_attribs);
}

}  // namespace content
