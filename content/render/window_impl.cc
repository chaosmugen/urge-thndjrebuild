// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/render/window_impl.h"


#include "content/context/execution_context.h"
#include "content/render/tilequad.h"
#include "renderer/device/gpu_driver.h"
#include "renderer/utils/texture_utils.h"

extern "C" void UrgeForcePassBoundary(Diligent::IDeviceContext* render_context);

// The window path picks its per-frame vertex set with SpriteBatch::FrameSetIndex(), whose
// range is the batch's own set count, and indexes background_batch/controls_batch with it.
// Nothing else ties the two counts together, and a batch count larger than the window's
// would index past those arrays. Keep them equal at compile time.
static_assert(content::WindowImpl::GPUData::kFrameSets ==
                  content::SpriteBatch::kFrameSets,
              "window and sprite batch frame-set counts must match");

namespace content {

// static
scoped_refptr<Window> Window::New(ExecutionContext* execution_context,
                                  scoped_refptr<Viewport> viewport,
                                  int32_t scale_,
                                  ExceptionState& exception_state) {
  return base::MakeRefCounted<WindowImpl>(
      execution_context, ViewportImpl::From(viewport), std::max(1, scale_));
}

///////////////////////////////////////////////////////////////////////////////
// WindowImpl Implement

WindowImpl::WindowImpl(ExecutionContext* execution_context,
                       scoped_refptr<ViewportImpl> parent,
                       int32_t scale_)
    : EngineObject(execution_context),
      Disposable(execution_context->disposable_parent),
      background_node_(parent ? parent->GetDrawableController()
                              : execution_context->screen_drawable_node,
                       SortKey()),
      control_node_(parent ? parent->GetDrawableController()
                           : execution_context->screen_drawable_node,
                    SortKey(2)),
      scale_(scale_),
      viewport_(parent),
      cursor_rect_(base::MakeRefCounted<RectImpl>(base::Rect())) {
  background_node_.RegisterEventHandler(base::BindRepeating(
      &WindowImpl::BackgroundNodeHandlerInternal, base::Unretained(this)));
  control_node_.RegisterEventHandler(base::BindRepeating(
      &WindowImpl::ControlNodeHandlerInternal, base::Unretained(this)));

  GPUCreateWindowInternal();
}

DISPOSABLE_DEFINITION(WindowImpl);

void WindowImpl::Update(ExceptionState& exception_state) {
  DISPOSE_CHECK;


  ++pause_index_;
  if (pause_index_ >= 32)
    pause_index_ = 0;

  if (active_) {
    cursor_opacity_ += cursor_fade_ ? -8 : 8;
    if (cursor_opacity_ > 255) {
      cursor_opacity_ = 255;
      cursor_fade_ = true;
    } else if (cursor_opacity_ < 128) {
      cursor_opacity_ = 128;
      cursor_fade_ = false;
    }
  } else {
    cursor_opacity_ = 128;
  }
}

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Viewport,
    scoped_refptr<Viewport>,
    WindowImpl,
    { return viewport_; },
    {
      DISPOSE_CHECK;
      if (viewport_ == value)
        return;
      viewport_ = ViewportImpl::From(value);
      DrawNodeController* controller = viewport_
                                           ? viewport_->GetDrawableController()
                                           : context()->screen_drawable_node;
      background_node_.RebindController(controller);
      control_node_.RebindController(controller);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Windowskin,
    scoped_refptr<Bitmap>,
    WindowImpl,
    { return windowskin_; },
    {
      DISPOSE_CHECK;
      // A bitmap that cannot be converted (already disposed, for instance) must not wipe
      // the skin the window is drawing with: without a skin the border layer emits no
      // quad at all, so the whole window goes missing while the background shows through.
      scoped_refptr<CanvasImpl> converted = CanvasImpl::FromBitmap(value);
      if (converted || !value)
        windowskin_ = converted;
      // Probe kept for verification: object=0 means the window now has no skin at all.
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Contents,
    scoped_refptr<Bitmap>,
    WindowImpl,
    { return contents_; },
    {
      DISPOSE_CHECK;
      // Same reason as Windowskin: a contents bitmap that cannot be converted must not
      // clear what the window is drawing with.
      scoped_refptr<CanvasImpl> converted = CanvasImpl::FromBitmap(value);
      if (converted || !value)
        contents_ = converted;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Stretch,
    bool,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(false);
      return stretch_;
    },
    {
      DISPOSE_CHECK;
      stretch_ = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    CursorRect,
    scoped_refptr<Rect>,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(nullptr);
      return cursor_rect_;
    },
    {
      DISPOSE_CHECK;
      CHECK_ATTRIBUTE_VALUE;
      *cursor_rect_ = *RectImpl::From(value);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Active,
    bool,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(false);
      return active_;
    },
    {
      DISPOSE_CHECK;
      active_ = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Visible,
    bool,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(false);
      return background_node_.GetVisibility();
    },
    {
      DISPOSE_CHECK;
      background_node_.SetNodeVisibility(value);
      control_node_.SetNodeVisibility(value);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Pause,
    bool,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(false);
      return pause_;
    },
    {
      DISPOSE_CHECK;
      pause_ = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    X,
    int32_t,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return bound_.x;
    },
    {
      DISPOSE_CHECK;
      bound_.x = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Y,
    int32_t,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return bound_.y;
    },
    {
      DISPOSE_CHECK;
      bound_.y = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Width,
    int32_t,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return bound_.width;
    },
    {
      DISPOSE_CHECK;
      bound_.width = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Height,
    int32_t,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return bound_.height;
    },
    {
      DISPOSE_CHECK;
      bound_.height = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Z,
    int32_t,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return background_node_.GetSortKeys()->weight[0];
    },
    {
      DISPOSE_CHECK;
      background_node_.SetNodeSortWeight(value);
      control_node_.SetNodeSortWeight(value + 2);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Ox,
    int32_t,
    WindowImpl,
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
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return origin_.y;
    },
    {
      DISPOSE_CHECK;
      origin_.y = value;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Opacity,
    int32_t,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return opacity_;
    },
    {
      DISPOSE_CHECK;
      opacity_ = std::clamp(value, 0, 255);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    BackOpacity,
    int32_t,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return back_opacity_;
    },
    {
      DISPOSE_CHECK;
      back_opacity_ = std::clamp(value, 0, 255);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    ContentsOpacity,
    int32_t,
    WindowImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return contents_opacity_;
    },
    {
      DISPOSE_CHECK;
      contents_opacity_ = std::clamp(value, 0, 255);
    });

static void ForgetLayersInternal(WindowImpl* window);

void WindowImpl::OnObjectDisposed() {
  // Recorded so that "composed and then not drawn" can be told apart from "the window
  // was torn down between the two stages".

  // The fallback must never emit a window that is being torn down.
  ForgetLayersInternal(this);

  background_node_.DisposeNode();
  control_node_.DisposeNode();

  GPUData empty_data;
  std::swap(gpu_, empty_data);
}

static void RegisterLayerInternal(WindowImpl* window,
                                  Diligent::IBuffer* world,
                                  bool background);
static void MarkLayerDrawnInternal(WindowImpl* window, bool background);
static void ForgetLayersInternal(WindowImpl* window);

// A window must keep drawing with its own skin. Borrowing another window's skin was
// tried and reverted: the game uses several skins, so it put the wrong border on
// windows that had none of their own. Only this window's own last usable skin is
// reused.
BitmapTexture* WindowImpl::ResolveWindowskinInternal() {
  if (Disposable::IsValid(windowskin_.get())) {
    BitmapTexture* texture = **windowskin_;
    if (texture && texture->shader_resource_view) {
      last_windowskin_ = windowskin_;
      return texture;
    }
  }

  // The skin is cleared or its texture is gone: keep drawing with the last usable one
  // rather than emitting no border at all.
  if (Disposable::IsValid(last_windowskin_.get())) {
    BitmapTexture* texture = **last_windowskin_;
    if (texture && texture->shader_resource_view)
      return texture;
  }

  return nullptr;
}

BitmapTexture* WindowImpl::ResolveContentsInternal() {
  if (Disposable::IsValid(contents_.get())) {
    BitmapTexture* texture = **contents_;
    if (texture && texture->shader_resource_view) {
      last_contents_ = contents_;
      return texture;
    }
  }

  if (Disposable::IsValid(last_contents_.get())) {
    BitmapTexture* texture = **last_contents_;
    if (texture && texture->shader_resource_view)
      return texture;
  }

  return nullptr;
}



static bool WindowForcePassBoundaryInternal(
    Diligent::IDeviceContext* render_context, Diligent::ITexture* screen_buffer,
    Diligent::ITexture* screen_depth) {
  if (!render_context || !screen_buffer || !screen_depth)
    return false;
  UrgeForcePassBoundary(render_context);
  return true;
}

void WindowImpl::BackgroundNodeHandlerInternal(
    DrawableNode::RenderStage stage,
    DrawableNode::RenderControllerParams* params) {
  if (bound_.width <= 4 || bound_.height <= 4) {
    return;
  }

  // Temporary, and deliberately narrow: a window draws nothing when neither layer has
  // anything to queue - no skin means the border layer builds zero quads, no contents
  // means the control layer does. Rather than guessing which input flips, report every
  // change of that whole state (skin / contents / opacity / bound) per window. A state
  // change to "all empty" is exactly the frame that disappears and shows the background.
  // The per-window state trace that used to be here is gone: it fired on every opacity
  // step of every fade and cost real frame time on the TV. What it established is kept:
  // skin, contents, opacity handling and bounds stay constant per window across a session.

  BitmapTexture* windowskin_agent = ResolveWindowskinInternal();

  if (stage == DrawableNode::RenderStage::BEFORE_RENDER) {
    GPUCompositeBackgroundLayerInternal(params->context, windowskin_agent);
    RegisterLayerInternal(this, params->world_binding, /*background=*/true);
  } else if (stage == DrawableNode::RenderStage::ON_RENDERING) {
    GPURenderBackgroundLayerInternal(params->context, params->world_binding,
                                     windowskin_agent);
    if (renderer::IsMaliGpu() && params->screen_buffer &&
        params->screen_depth_stencil &&
        WindowForcePassBoundaryInternal(params->context, params->screen_buffer,
                                        params->screen_depth_stencil))
      GPURenderBackgroundLayerInternal(params->context, params->world_binding,
                                       windowskin_agent);
    MarkLayerDrawnInternal(this, /*background=*/true);

    // Logged at the moment the border layer is sent to the GPU, with the values that
    // draw actually uses. A visible window whose layer line is missing here is a layer
    // that was skipped for that frame; a line with count=0 is a layer with nothing to
    // draw. Hidden windows (opacity 0) are left out of the trace.
    // The per-frame [WinDraw] trace that used to be here wrote two lines per window per
    // frame straight to the log (24+ synchronous writes a frame with a dozen windows) and
    // was itself a large part of the stutter on the TV. Its conclusions are recorded: the
    // layers are drawn every frame with constant bound/opacity/texture/world/scissor.
  }
}

void WindowImpl::ControlNodeHandlerInternal(
    DrawableNode::RenderStage stage,
    DrawableNode::RenderControllerParams* params) {
  if (bound_.width <= 4 || bound_.height <= 4)
    return;  // logged by the background node's guard above

  BitmapTexture* windowskin_agent = ResolveWindowskinInternal();
  BitmapTexture* contents_agent = ResolveContentsInternal();

  // Temporary: with the window's own state (skin/contents/opacity/bound) proven constant,
  // the textures it draws with are the only inputs left that can blank a whole window for
  // a frame - the game redraws its content bitmap continuously, so it is replaced while
  // the window keeps drawing. Tracked per window, so several windows cannot be confused
  // with each other the way the earlier global probe was.
  {
    static std::vector<std::pair<const void*, std::pair<const void*, const void*>>>
        s_window_textures;
    std::pair<const void*, const void*>* slot = nullptr;
    for (auto& it : s_window_textures) {
      if (it.first == this) {
        slot = &it.second;
        break;
      }
    }
    if (!slot) {
      s_window_textures.push_back(
          {this, {static_cast<const void*>(windowskin_agent),
                  static_cast<const void*>(contents_agent)}});
      slot = &s_window_textures.back().second;
    } else if (slot->first != windowskin_agent ||
               slot->second != contents_agent) {
      slot->first = windowskin_agent;
      slot->second = contents_agent;
      }
      }

      // Temporary: the last input left. The window is drawn through params->world_binding;
      // a frame that draws it with another level's transform puts it off screen, which looks
      // exactly like the window vanishing while the background keeps rendering.
      {
      static std::vector<std::pair<const void*, const void*>> s_window_worlds;
      const void* world = static_cast<const void*>(params->world_binding);
      const void** slot = nullptr;
      for (auto& it : s_window_worlds) {
      if (it.first == this) {
       slot = &it.second;
       break;
      }
      }
      if (!slot) {
      s_window_worlds.push_back({this, world});
      } else if (*slot != world) {
      *slot = world;
      }
      }

  if (stage == DrawableNode::RenderStage::BEFORE_RENDER) {

    GPUCompositeControlLayerInternal(params->context, windowskin_agent,
                                     contents_agent);
    RegisterLayerInternal(this, params->world_binding, /*background=*/false);

  } else if (stage == DrawableNode::RenderStage::ON_RENDERING) {
    GPURenderControlLayerInternal(params->context, params->world_binding,
                                  windowskin_agent, contents_agent,
                                  params->scissors);
    MarkLayerDrawnInternal(this, /*background=*/false);

  }
}

void WindowImpl::GPUCreateWindowInternal() {
  for (int32_t i = 0; i < GPUData::kFrameSets; ++i) {
    gpu_.background_batch[i] = renderer::QuadBatch::Make(**context()->render_device);
    gpu_.controls_batch[i] = renderer::QuadBatch::Make(**context()->render_device);
  }

  gpu_.base_binding = context()->render.pipeline_loader->base.CreateBinding();
  gpu_.content_binding =
      context()->render.pipeline_loader->base.CreateBinding();
}

// A window layer composed in the enqueue stage that never reaches the draw stage would
// lose the window for a frame. Such layers are registered here and emitted by
// FlushPendingLayers before the pass that composed them closes. |composites| counts
// every layer queued, |draws| every layer emitted (by its node or by that fallback):
// the two must end up equal.
struct PendingWindowLayer {
  WindowImpl* window = nullptr;
  Diligent::IBuffer* world = nullptr;
  bool background = false;
  bool control = false;
};

static std::vector<PendingWindowLayer> g_pending_layers;

static void RegisterLayerInternal(WindowImpl* window,
                                  Diligent::IBuffer* world,
                                  bool background) {

  for (auto& it : g_pending_layers) {
    if (it.window == window) {
      (background ? it.background : it.control) = true;
      return;
    }
  }

  PendingWindowLayer entry;
  entry.window = window;
  entry.world = world;
  entry.background = background;
  entry.control = !background;
  g_pending_layers.push_back(entry);
}

static void MarkLayerDrawnInternal(WindowImpl* window, bool background) {
  for (auto it = g_pending_layers.begin(); it != g_pending_layers.end(); ++it) {
    if (it->window != window)
      continue;
    (background ? it->background : it->control) = false;
    if (!it->background && !it->control)
      g_pending_layers.erase(it);
    return;
  }
}

static void ForgetLayersInternal(WindowImpl* window) {
  for (auto it = g_pending_layers.begin(); it != g_pending_layers.end(); ++it) {
    if (it->window == window) {
      g_pending_layers.erase(it);
      return;
    }
  }
}

void WindowImpl::FlushPendingLayers(Diligent::IDeviceContext* context,
                                    Diligent::IBuffer* world,
                                    const base::Rect& scissor) {
  if (g_pending_layers.empty())
    return;

  // Only this pass's level: a layer composed under another world belongs to that
  // level's pass, whose draw stage has not run yet at this point.
  std::vector<PendingWindowLayer> pending;
  for (const auto& it : g_pending_layers) {
    if (it.world == world)
      pending.push_back(it);
  }

  // Report what this pass actually saw: entries left over (the fallback about to run)
  // or entries skipped because they belong to another level.

  if (pending.empty())
    return;

  ScissorStack scissor_stack(context, scissor);

  for (const PendingWindowLayer& it : pending) {
    WindowImpl* window = it.window;
    BitmapTexture* windowskin_agent =
        Disposable::IsValid(window->windowskin_.get()) ? **window->windowskin_
                                                       : nullptr;
    BitmapTexture* contents_agent =
        Disposable::IsValid(window->contents_.get()) ? **window->contents_ : nullptr;

    if (it.background) {
      window->GPURenderBackgroundLayerInternal(context, world, windowskin_agent);
    }
    if (it.control)
      window->GPURenderControlLayerInternal(context, world, windowskin_agent,
                                            contents_agent, &scissor_stack);
  }

  for (const PendingWindowLayer& it : pending)
    ForgetLayersInternal(it.window);
}


void WindowImpl::GPUCompositeBackgroundLayerInternal(
    Diligent::IDeviceContext* render_context,
    BitmapTexture* windowskin) {

  // Temporary probe: the background layer builds no quad at all without a windowskin
  // (see the "if (windowskin)" below), so a frame where the skin bitmap is invalid
  // loses the entire frame border while the control layer keeps drawing the contents -
  // exactly "only the border disappears for a frame".

  // Temporary probe: the border layer is composed and drawn every frame (the layer
  // counters are paired), so a border that still disappears for a frame can only be
  // drawn with the wrong content - the skin texture being replaced or re-uploaded.
  // The game creates ~40 bitmaps per second, so report every change of skin texture.
  const base::Vec2i& draw_offset = bound_.Position();

  // Generate background quads
  int32_t quad_index = 0;
  if (windowskin) {
    // Corners
    int32_t quad_count = 4;

    // Background
    if (stretch_) {
      quad_count += 1;
    } else {
      int32_t horizon_count =
          CalculateQuadTileCount(64 * scale_, bound_.width - 2 * scale_);
      int32_t vertical_count =
          CalculateQuadTileCount(64 * scale_, bound_.height - 2 * scale_);
      quad_count += horizon_count * vertical_count;
    }

    // Frame tiles
    const int32_t horizon_frame_size = bound_.width - scale_ * 16;
    const int32_t vertical_frame_size = bound_.height - scale_ * 16;
    quad_count += CalculateQuadTileCount(16 * scale_, horizon_frame_size) * 2;
    quad_count += CalculateQuadTileCount(16 * scale_, vertical_frame_size) * 2;

    // Resize cache
    auto& quads = gpu_.background_cache;
    quads.resize(quad_count);

    // Draw attributes
    // Alpha was forced to 1 here to test the transparency path: the window kept
    // vanishing, so its alpha is not the cause and the real opacity maths is restored.
    const float opacity_norm = static_cast<float>(opacity_) / 255.0f;
    const float back_opacity_norm = static_cast<float>(back_opacity_) / 255.0f;

    // Stretch / Tile layer
    const base::Rect dest_rect(draw_offset.x + scale_, draw_offset.y + scale_,
                               bound_.width - 2 * scale_,
                               bound_.height - 2 * scale_);
    const base::Rect background_src(0, 0, 64 * scale_, 64 * scale_);

    if (stretch_) {
      renderer::Quad::SetPositionRect(&quads[quad_index], dest_rect);
      renderer::Quad::SetTexCoordRect(&quads[quad_index], background_src,
                                      windowskin->size.Recast<float>());
      renderer::Quad::SetColor(&quads[quad_index],
                               base::Vec4(opacity_norm * back_opacity_norm));
      quad_index++;
    } else {
      // Build tiled background quads
      quad_index += BuildTiles(background_src, dest_rect,
                               base::Vec4(opacity_norm * back_opacity_norm),
                               windowskin->size, &quads[quad_index]);
    }

    // Frame Corners
    base::Rect corner_left_top(64 * scale_, 0, 8 * scale_, 8 * scale_);
    base::Rect corner_right_top(88 * scale_, 0, 8 * scale_, 8 * scale_);
    base::Rect corner_right_bottom(88 * scale_, 24 * scale_, 8 * scale_,
                                   8 * scale_);
    base::Rect corner_left_bottom(64 * scale_, 24 * scale_, 8 * scale_,
                                  8 * scale_);

    renderer::Quad::SetPositionRect(
        &quads[quad_index], base::Rect{draw_offset, base::Vec2i(8 * scale_)});
    renderer::Quad::SetTexCoordRect(&quads[quad_index], corner_left_top,
                                    windowskin->size.Recast<float>());
    renderer::Quad::SetColor(&quads[quad_index], base::Vec4(opacity_norm));
    quad_index++;
    renderer::Quad::SetPositionRect(
        &quads[quad_index],
        base::Rect{{draw_offset.x + bound_.width - 8 * scale_, draw_offset.y},
                   base::Vec2i(8 * scale_)});
    renderer::Quad::SetTexCoordRect(&quads[quad_index], corner_right_top,
                                    windowskin->size.Recast<float>());
    renderer::Quad::SetColor(&quads[quad_index], base::Vec4(opacity_norm));
    quad_index++;
    renderer::Quad::SetPositionRect(
        &quads[quad_index],
        base::Rect{{draw_offset.x + bound_.width - 8 * scale_,
                    draw_offset.y + bound_.height - 8 * scale_},
                   base::Vec2i(8 * scale_)});
    renderer::Quad::SetTexCoordRect(&quads[quad_index], corner_right_bottom,
                                    windowskin->size.Recast<float>());
    renderer::Quad::SetColor(&quads[quad_index], base::Vec4(opacity_norm));
    quad_index++;
    renderer::Quad::SetPositionRect(
        &quads[quad_index],
        base::Rect{{draw_offset.x, draw_offset.y + bound_.height - 8 * scale_},
                   base::Vec2i(8 * scale_)});
    renderer::Quad::SetTexCoordRect(&quads[quad_index], corner_left_bottom,
                                    windowskin->size.Recast<float>());
    renderer::Quad::SetColor(&quads[quad_index], base::Vec4(opacity_norm));
    quad_index++;

    // Frame tiles
    base::Rect frame_up(72 * scale_, 0, 16 * scale_, 8 * scale_);
    base::Rect frame_down(72 * scale_, 24 * scale_, 16 * scale_, 8 * scale_);
    base::Rect frame_left(64 * scale_, 8 * scale_, 8 * scale_, 16 * scale_);
    base::Rect frame_right(88 * scale_, 8 * scale_, 8 * scale_, 16 * scale_);

    base::Vec2i frame_up_pos(draw_offset.x + 8 * scale_, draw_offset.y);
    base::Vec2i frame_down_pos(draw_offset.x + 8 * scale_,
                               draw_offset.y + bound_.height - 8 * scale_);
    base::Vec2i frame_left_pos(draw_offset.x, draw_offset.y + 8 * scale_);
    base::Vec2i frame_right_pos(draw_offset.x + bound_.width - 8 * scale_,
                                draw_offset.y + 8 * scale_);

    quad_index += BuildTilesAlongAxis(
        TileAxis::HORIZONTAL, frame_up, frame_up_pos, base::Vec4(opacity_norm),
        bound_.width - 16 * scale_, windowskin->size, &quads[quad_index]);
    quad_index += BuildTilesAlongAxis(TileAxis::HORIZONTAL, frame_down,
                                      frame_down_pos, base::Vec4(opacity_norm),
                                      bound_.width - 16 * scale_,
                                      windowskin->size, &quads[quad_index]);
    quad_index += BuildTilesAlongAxis(TileAxis::VERTICAL, frame_left,
                                      frame_left_pos, base::Vec4(opacity_norm),
                                      bound_.height - 16 * scale_,
                                      windowskin->size, &quads[quad_index]);
    quad_index += BuildTilesAlongAxis(TileAxis::VERTICAL, frame_right,
                                      frame_right_pos, base::Vec4(opacity_norm),
                                      bound_.height - 16 * scale_,
                                      windowskin->size, &quads[quad_index]);
  }

  // Make sure quads drawcall count
  gpu_.background_draw_count = quad_index;

  if (!gpu_.background_cache.empty())
    gpu_.background_batch[context()->sprite_batcher->FrameSetIndex()].QueueWrite(
        render_context, gpu_.background_cache.data(),
        gpu_.background_cache.size());
}

void WindowImpl::GPUCompositeControlLayerInternal(
    Diligent::IDeviceContext* render_context,
    BitmapTexture* windowskin,
    BitmapTexture* contents) {

  const float contents_opacity_norm = contents_opacity_ / 255.0f;
  const float cursor_opacity_norm = cursor_opacity_ / 255.0f;

  // Cursor render
  auto build_cursor_internal = [&](const base::Rect& rect, int32_t unit,
                                   base::Rect quad_rects[9]) {
    const int32_t w = rect.width;
    const int32_t h = rect.height;
    const int32_t l = rect.x;
    const int32_t r = l + w;
    const int32_t t = rect.y;
    const int32_t b = t + h;

    int32_t i = 0;
    quad_rects[i++] = {{l, t}, base::Vec2i(unit)};         // Left-Top
    quad_rects[i++] = {{r - unit, t}, base::Vec2i(unit)};  // Right-Top
    quad_rects[i++] = {{r - unit, b - unit},
                       base::Vec2i(unit)};                 // Right-Bottom
    quad_rects[i++] = {{l, b - unit}, base::Vec2i(unit)};  // Left-Bottom

    quad_rects[i++] = {l, t + unit, unit, h - unit * 2};         // Left
    quad_rects[i++] = {r - unit, t + unit, unit, h - unit * 2};  // Right
    quad_rects[i++] = {l + unit, t, w - unit * 2, unit};         // Top
    quad_rects[i++] = {l + unit, b - unit, w - unit * 2, unit};  // Bottom
    quad_rects[i++] = {l + unit, t + unit, w - unit * 2,
                       h - unit * 2};  // Center
  };

  auto build_cursor_quads = [&](const base::Rect& src, const base::Rect& dst,
                                renderer::Quad vert[9]) {
    base::Rect texcoords[9], positions[9];
    const base::Vec4 draw_color(cursor_opacity_norm * contents_opacity_norm);
    build_cursor_internal(src, scale_, texcoords);
    build_cursor_internal(dst, scale_, positions);

    for (int32_t i = 0; i < 9; ++i) {
      renderer::Quad::SetPositionRect(&vert[i], positions[i]);
      renderer::Quad::SetTexCoordRect(&vert[i], texcoords[i],
                                      windowskin->size.Recast<float>());
      renderer::Quad::SetColor(&vert[i], draw_color);
    }

    return 9;
  };

  // Generate quads
  const base::Vec2i draw_offset = bound_.Position();
  auto& quads = gpu_.controls_cache;
  quads.resize(9 + 4 + 1 + 1);
  int32_t quad_index = 0;

  if (windowskin) {
    // Cursor render (9)
    const auto cursor_rect = cursor_rect_->AsBaseRect();
    if (cursor_rect.width > 0 && cursor_rect.height > 0) {
      const base::Rect cursor_dest_rect(
          draw_offset.x + cursor_rect.x + 8 * scale_,
          draw_offset.y + cursor_rect.y + 8 * scale_, cursor_rect.width,
          cursor_rect.height);
      const base::Rect cursor_src(64 * scale_, 32 * scale_, 16 * scale_,
                                  16 * scale_);
      quad_index +=
          build_cursor_quads(cursor_src, cursor_dest_rect, &quads[quad_index]);
    }

    // Arrows render (0-4)
    const base::Vec2i scroll =
        draw_offset + ((bound_.Size() - base::Vec2i(8 * scale_)) / 2);
    base::Rect scroll_arrow_up_pos = base::Rect(
        scroll.x, draw_offset.y + 2 * scale_, 8 * scale_, 4 * scale_);
    base::Rect scroll_arrow_down_pos =
        base::Rect(scroll.x, draw_offset.y + bound_.height - 6 * scale_,
                   8 * scale_, 4 * scale_);
    base::Rect scroll_arrow_left_pos = base::Rect(
        draw_offset.x + 2 * scale_, scroll.y, 4 * scale_, 8 * scale_);
    base::Rect scroll_arrow_right_pos =
        base::Rect(draw_offset.x + bound_.width - 6 * scale_, scroll.y,
                   4 * scale_, 8 * scale_);

    if (contents) {
      base::Rect scroll_arrow_up_src = {76 * scale_, 8 * scale_, 8 * scale_,
                                        4 * scale_};
      base::Rect scroll_arrow_down_src = {76 * scale_, 20 * scale_, 8 * scale_,
                                          4 * scale_};
      base::Rect scroll_arrow_left_src = {72 * scale_, 12 * scale_, 4 * scale_,
                                          8 * scale_};
      base::Rect scroll_arrow_right_src = {84 * scale_, 12 * scale_, 4 * scale_,
                                           8 * scale_};

      if (origin_.x > 0) {
        renderer::Quad::SetPositionRect(&quads[quad_index],
                                        scroll_arrow_left_pos);
        renderer::Quad::SetTexCoordRect(&quads[quad_index],
                                        scroll_arrow_left_src,
                                        windowskin->size.Recast<float>());
        renderer::Quad::SetColor(&quads[quad_index],
                                 base::Vec4(contents_opacity_norm));
        quad_index++;
      }

      if (origin_.y > 0) {
        renderer::Quad::SetPositionRect(&quads[quad_index],
                                        scroll_arrow_up_pos);
        renderer::Quad::SetTexCoordRect(&quads[quad_index], scroll_arrow_up_src,
                                        windowskin->size.Recast<float>());
        renderer::Quad::SetColor(&quads[quad_index],
                                 base::Vec4(contents_opacity_norm));
        quad_index++;
      }

      if ((bound_.width - 16 * scale_) < (contents->size.x - origin_.x)) {
        renderer::Quad::SetPositionRect(&quads[quad_index],
                                        scroll_arrow_right_pos);
        renderer::Quad::SetTexCoordRect(&quads[quad_index],
                                        scroll_arrow_right_src,
                                        windowskin->size.Recast<float>());
        renderer::Quad::SetColor(&quads[quad_index],
                                 base::Vec4(contents_opacity_norm));
        quad_index++;
      }

      if ((bound_.height - 16 * scale_) < (contents->size.y - origin_.y)) {
        renderer::Quad::SetPositionRect(&quads[quad_index],
                                        scroll_arrow_down_pos);
        renderer::Quad::SetTexCoordRect(&quads[quad_index],
                                        scroll_arrow_down_src,
                                        windowskin->size.Recast<float>());
        renderer::Quad::SetColor(&quads[quad_index],
                                 base::Vec4(contents_opacity_norm));
        quad_index++;
      }
    }

    // Pause render (0-1)
    base::Rect pause_animation[] = {
        {80 * scale_, 32 * scale_, 8 * scale_, 8 * scale_},
        {88 * scale_, 32 * scale_, 8 * scale_, 8 * scale_},
        {80 * scale_, 40 * scale_, 8 * scale_, 8 * scale_},
        {88 * scale_, 40 * scale_, 8 * scale_, 8 * scale_},
    };

    if (pause_) {
      renderer::Quad::SetPositionRect(
          &quads[quad_index],
          base::Rect(draw_offset.x + (bound_.width - 8 * scale_) / 2,
                     draw_offset.y + bound_.height - 8 * scale_, 8 * scale_,
                     8 * scale_));
      renderer::Quad::SetTexCoordRect(&quads[quad_index],
                                      pause_animation[pause_index_ / 8],
                                      windowskin->size.Recast<float>());
      renderer::Quad::SetColor(&quads[quad_index],
                               base::Vec4(contents_opacity_norm));
      quad_index++;
    }

    // Make sure controls quads count
    gpu_.controls_draw_count = quad_index;
  }

  // Contents (0-1)
  if (contents) {
    const base::Vec2i content_offset(draw_offset.x + 8 * scale_ - origin_.x,
                                     draw_offset.y + 8 * scale_ - origin_.y);

    renderer::Quad::SetPositionRect(&quads[quad_index],
                                    base::Rect(content_offset, contents->size));
    renderer::Quad::SetTexCoordRectNorm(
        &quads[quad_index], base::RectF(base::Vec2(0), base::Vec2(1)));
    renderer::Quad::SetColor(&quads[quad_index],
                             base::Vec4(contents_opacity_norm));

    // Make sure contents quad offset
    gpu_.contents_quad_offset = quad_index;
  }

  // Make sure index buffer count
  context()->render.quad_index->Allocate(quad_index);

  if (!quads.empty())
    gpu_.controls_batch[context()->sprite_batcher->FrameSetIndex()].QueueWrite(
        render_context, quads.data(), quads.size());
}


void WindowImpl::GPURenderBackgroundLayerInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::IBuffer* world_binding,
    BitmapTexture* windowskin) {

  // The window draws with the shared quad index buffer, whose size is otherwise decided
  // by this frame's sprite count (SubmitBatchDataAndResetCache allocates for the sprite
  // cache). A window needs far more indices than a sprite batch usually does - four
  // corners plus dozens of tiles - so NumIndices below could exceed the buffer, and
  // reading indices past its end is what makes whole windows vanish on this driver while
  // others tolerate it. Allocate() only ever grows, so the sprite path is unaffected.
  if (!gpu_.background_cache.empty())
    context()->render.quad_index->Allocate(gpu_.background_cache.size());

  if (windowskin) {
    auto* pipeline = context()->render.pipeline_states->window.RawPtr();

    // Setup shader resource
    gpu_.base_binding.u_transform->Set(world_binding);
    gpu_.base_binding.u_texture->Set(windowskin->shader_resource_view);

    // Apply vertex index
    Diligent::IBuffer* const vertex_buffer =
        *gpu_.background_batch[context()->sprite_batcher->FrameSetIndex()];
    render_context->SetVertexBuffers(
        0, 1, &vertex_buffer, nullptr,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    render_context->SetIndexBuffer(
        **context()->render.quad_index, 0,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Apply pipeline state
    render_context->SetPipelineState(pipeline);
    render_context->CommitShaderResources(
        *gpu_.base_binding,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Execute render command
    Diligent::DrawIndexedAttribs draw_indexed_attribs;
    draw_indexed_attribs.NumIndices = gpu_.background_draw_count * 6;
    draw_indexed_attribs.IndexType =
        context()->render.quad_index->GetIndexType();
    render_context->DrawIndexed(draw_indexed_attribs);
  }
}

void WindowImpl::GPURenderControlLayerInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::IBuffer* world_binding,
    BitmapTexture* windowskin,
    BitmapTexture* contents,
    ScissorStack* scissor_stack) {

  // Same index-buffer reservation as the background layer above: this layer queues the
  // frame, the cursor and the contents into one buffer, so its index needs are the
  // largest of the two.
  if (!gpu_.controls_cache.empty())
    context()->render.quad_index->Allocate(gpu_.controls_cache.size());



  const auto current_viewport =
      background_node_.GetParentViewport()->bound.Position() -
      background_node_.GetParentViewport()->origin;

  if (windowskin) {
    const base::Rect clipping_region(current_viewport.x + bound_.x,
                                     current_viewport.y + bound_.y,
                                     bound_.width, bound_.height);

    const bool frame_pushed = scissor_stack->Push(clipping_region);
    if (frame_pushed) {
      auto* pipeline = context()->render.pipeline_states->window.RawPtr();

      // Setup shader resource
      gpu_.base_binding.u_transform->Set(world_binding);
      gpu_.base_binding.u_texture->Set(windowskin->shader_resource_view);

      // Apply vertex index
      Diligent::IBuffer* const vertex_buffer =
          *gpu_.controls_batch[context()->sprite_batcher->FrameSetIndex()];
      render_context->SetVertexBuffers(
          0, 1, &vertex_buffer, nullptr,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      render_context->SetIndexBuffer(
          **context()->render.quad_index, 0,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

      // Apply pipeline state
      render_context->SetPipelineState(pipeline);
      render_context->CommitShaderResources(
          *gpu_.base_binding,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

      // Execute render command
      Diligent::DrawIndexedAttribs draw_indexed_attribs;
      draw_indexed_attribs.NumIndices = gpu_.controls_draw_count * 6;
      draw_indexed_attribs.IndexType =
          context()->render.quad_index->GetIndexType();
      render_context->DrawIndexed(draw_indexed_attribs);

      // Restore
      scissor_stack->Pop();
    }
  }

  if (contents) {
    const base::Rect clipping_region(current_viewport.x + bound_.x + 8 * scale_,
                                     current_viewport.y + bound_.y + 8 * scale_,
                                     bound_.width - 16 * scale_,
                                     bound_.height - 16 * scale_);

    const bool contents_pushed = scissor_stack->Push(clipping_region);

    if (contents_pushed) {
      auto* pipeline = context()->render.pipeline_states->window.RawPtr();

      // Setup shader resource
      gpu_.content_binding.u_transform->Set(world_binding);
      gpu_.content_binding.u_texture->Set(contents->shader_resource_view);

      // Apply vertex index
      Diligent::IBuffer* const vertex_buffer =
          *gpu_.controls_batch[context()->sprite_batcher->FrameSetIndex()];
      render_context->SetVertexBuffers(
          0, 1, &vertex_buffer, nullptr,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      render_context->SetIndexBuffer(
          **context()->render.quad_index, 0,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

      // Apply pipeline state
      render_context->SetPipelineState(pipeline);
      render_context->CommitShaderResources(
          *gpu_.content_binding,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);


      Diligent::DrawIndexedAttribs draw_indexed_attribs;
      draw_indexed_attribs.NumIndices = 6;
      draw_indexed_attribs.IndexType =
          context()->render.quad_index->GetIndexType();
      draw_indexed_attribs.FirstIndexLocation = gpu_.contents_quad_offset * 6;
      render_context->DrawIndexed(draw_indexed_attribs);

      // Restore
      scissor_stack->Pop();
    }
  }
}

}  // namespace content
