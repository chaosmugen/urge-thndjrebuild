// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/render/tilemap_impl.h"

#include "content/context/execution_context.h"
#include "renderer/device/gpu_driver.h"
#include "renderer/utils/texture_utils.h"

#include <cstdio>

// Defined in content/screen/renderscreen_impl.cc, declared after the includes because it
// names a Diligent type. Marker: URGE_SECONDPASS
extern "C" void UrgeForcePassBoundary(Diligent::IDeviceContext* render_context);



namespace content {

// Forward declarations (defined below). Marker: URGE_SECONDPASS
static bool TilemapSecondPassEnabledInternal();
static bool ForcePassBoundaryInternal(Diligent::IDeviceContext* render_context,
                                     Diligent::ITexture* screen_buffer,
                                     Diligent::ITexture* screen_depth);


namespace {

// RMXP draws the later-created object on top when both share the same Z, and
// Spriteset_Map builds the Tilemap before the character sprites, so a tile
// event always wins the tie against the map tile on the same row / priority.
// The engine encodes that creation order in weight[2], but the tilemap builds
// (and rebuilds) its above layer nodes on the first frame, i.e. after every
// character sprite already exists, which inverts the tie-break and lets map
// tiles cover tile events. Pin the tilemap nodes to a stamp that always sorts
// before any sprite instead of relying on when the nodes get constructed.
// The ground layer takes the base stamp and the above layers take base + 1, so
// the two tilemap layers keep a strict order even when a layer's Z lands on the
// ground's 0 (which happens whenever the tilemap origin is a multiple of the
// tile size).
constexpr int64_t kTilemapSortStamp = -(int64_t{1} << 62);

// Reference: RPGXP Editor - Autotile double click
// Format: Left Top -> Right Top -> Left Bottom -> Right Bottom
const base::Vec2 kAutotileSrcRegular[][4] = {
    {
        {1.0f, 2.0f},
        {1.5f, 2.0f},
        {1.0f, 2.5f},
        {1.5f, 2.5f},
    },
    {
        {2.0f, 0.0f},
        {1.5f, 2.0f},
        {1.0f, 2.5f},
        {1.5f, 2.5f},
    },
    {
        {1.0f, 2.0f},
        {2.5f, 0.0f},
        {1.0f, 2.5f},
        {1.5f, 2.5f},
    },
    {
        {2.0f, 0.0f},
        {2.5f, 0.0f},
        {1.0f, 2.5f},
        {1.5f, 2.5f},
    },
    {
        {1.0f, 2.0f},
        {1.5f, 2.0f},
        {1.0f, 2.5f},
        {2.5f, 0.5f},
    },
    {
        {2.0f, 0.0f},
        {1.5f, 2.0f},
        {1.0f, 2.5f},
        {2.5f, 0.5f},
    },
    {
        {1.0f, 2.0f},
        {2.5f, 0.0f},
        {1.0f, 2.5f},
        {2.5f, 0.5f},
    },
    {
        {2.0f, 0.0f},
        {2.5f, 0.0f},
        {1.0f, 2.5f},
        {2.5f, 0.5f},
    },
    {
        {1.0f, 2.0f},
        {1.5f, 2.0f},
        {2.0f, 0.5f},
        {1.5f, 2.5f},
    },
    {
        {2.0f, 0.0f},
        {1.5f, 2.0f},
        {2.0f, 0.5f},
        {1.5f, 2.5f},
    },
    {
        {1.0f, 2.0f},
        {2.5f, 0.0f},
        {2.0f, 0.5f},
        {1.5f, 2.5f},
    },
    {
        {2.0f, 0.0f},
        {2.5f, 0.0f},
        {2.0f, 0.5f},
        {1.5f, 2.5f},
    },
    {
        {1.0f, 2.0f},
        {1.5f, 2.0f},
        {2.0f, 0.5f},
        {2.5f, 0.5f},
    },
    {
        {2.0f, 0.0f},
        {1.5f, 2.0f},
        {2.0f, 0.5f},
        {2.5f, 0.5f},
    },
    {
        {1.0f, 2.0f},
        {2.5f, 0.0f},
        {2.0f, 0.5f},
        {2.5f, 0.5f},
    },
    {
        {2.0f, 0.0f},
        {2.5f, 0.0f},
        {2.0f, 0.5f},
        {2.5f, 0.5f},
    },
    {
        {0.0f, 2.0f},
        {0.5f, 2.0f},
        {0.0f, 2.5f},
        {0.5f, 2.5f},
    },
    {
        {0.0f, 2.0f},
        {2.5f, 0.0f},
        {0.0f, 2.5f},
        {0.5f, 2.5f},
    },
    {
        {0.0f, 2.0f},
        {0.5f, 2.0f},
        {0.0f, 2.5f},
        {2.5f, 0.5f},
    },
    {
        {0.0f, 2.0f},
        {2.5f, 0.0f},
        {0.0f, 2.5f},
        {2.5f, 0.5f},
    },
    {
        {1.0f, 1.0f},
        {1.5f, 1.0f},
        {1.0f, 1.5f},
        {1.5f, 1.5f},
    },
    {
        {1.0f, 1.0f},
        {1.5f, 1.0f},
        {1.0f, 1.5f},
        {2.5f, 0.5f},
    },
    {
        {1.0f, 1.0f},
        {1.5f, 1.0f},
        {2.0f, 0.5f},
        {1.5f, 1.5f},
    },
    {
        {1.0f, 1.0f},
        {1.5f, 1.0f},
        {2.0f, 0.5f},
        {2.5f, 0.5f},
    },
    {
        {2.0f, 2.0f},
        {2.5f, 2.0f},
        {2.0f, 2.5f},
        {2.5f, 2.5f},
    },
    {
        {2.0f, 2.0f},
        {2.5f, 2.0f},
        {2.0f, 0.5f},
        {2.5f, 2.5f},
    },
    {
        {2.0f, 0.0f},
        {2.5f, 2.0f},
        {2.0f, 2.5f},
        {2.5f, 2.5f},
    },
    {
        {2.0f, 0.0f},
        {2.5f, 2.0f},
        {2.0f, 0.5f},
        {2.5f, 2.5f},
    },
    {
        {1.0f, 3.0f},
        {1.5f, 3.0f},
        {1.0f, 3.5f},
        {1.5f, 3.5f},
    },
    {
        {2.0f, 0.0f},
        {1.5f, 3.0f},
        {1.0f, 3.5f},
        {1.5f, 3.5f},
    },
    {
        {1.0f, 3.0f},
        {2.5f, 0.0f},
        {1.0f, 3.5f},
        {1.5f, 3.5f},
    },
    {
        {2.0f, 0.0f},
        {2.5f, 0.0f},
        {1.0f, 3.5f},
        {1.5f, 3.5f},
    },
    {
        {0.0f, 2.0f},
        {2.5f, 2.0f},
        {0.0f, 2.5f},
        {2.5f, 2.5f},
    },
    {
        {1.0f, 1.0f},
        {1.5f, 1.0f},
        {1.0f, 3.5f},
        {1.5f, 3.5f},
    },
    {
        {0.0f, 1.0f},
        {0.5f, 1.0f},
        {0.0f, 1.5f},
        {0.5f, 1.5f},
    },
    {
        {0.0f, 1.0f},
        {0.5f, 1.0f},
        {0.0f, 1.5f},
        {2.5f, 0.5f},
    },
    {
        {2.0f, 1.0f},
        {2.5f, 1.0f},
        {2.0f, 1.5f},
        {2.5f, 1.5f},
    },
    {
        {2.0f, 1.0f},
        {2.5f, 1.0f},
        {2.0f, 0.5f},
        {2.5f, 1.5f},
    },
    {
        {2.0f, 3.0f},
        {2.5f, 3.0f},
        {2.0f, 3.5f},
        {2.5f, 3.5f},
    },
    {
        {2.0f, 0.0f},
        {2.5f, 3.0f},
        {2.0f, 3.5f},
        {2.5f, 3.5f},
    },
    {
        {0.0f, 3.0f},
        {0.5f, 3.0f},
        {0.0f, 3.5f},
        {0.5f, 3.5f},
    },
    {
        {0.0f, 3.0f},
        {2.5f, 0.0f},
        {0.0f, 3.5f},
        {0.5f, 3.5f},
    },
    {
        {0.0f, 1.0f},
        {2.5f, 1.0f},
        {0.0f, 1.5f},
        {2.5f, 1.5f},
    },
    {
        {0.0f, 1.0f},
        {0.5f, 1.0f},
        {0.0f, 3.5f},
        {0.5f, 3.5f},
    },
    {
        {0.0f, 3.0f},
        {2.5f, 3.0f},
        {0.0f, 3.5f},
        {2.5f, 3.5f},
    },
    {
        {2.0f, 1.0f},
        {2.5f, 1.0f},
        {2.0f, 3.5f},
        {2.5f, 3.5f},
    },
    {
        {0.0f, 1.0f},
        {2.5f, 1.0f},
        {0.0f, 3.5f},
        {2.5f, 3.5f},
    },
    {
        {0.0f, 0.0f},
        {0.5f, 0.0f},
        {0.0f, 0.5f},
        {0.5f, 0.5f},
    },
};

}  // namespace

// static
scoped_refptr<Tilemap> Tilemap::New(ExecutionContext* execution_context,
                                    scoped_refptr<Viewport> viewport,
                                    int32_t tilesize,
                                    ExceptionState& exception_state) {
  return base::MakeRefCounted<TilemapImpl>(
      execution_context, ViewportImpl::From(viewport), std::max(16, tilesize));
}

///////////////////////////////////////////////////////////////////////////////
// TilemapAutotileImpl Implement

TilemapAutotileImpl::TilemapAutotileImpl(base::WeakPtr<TilemapImpl> tilemap)
    : tilemap_(tilemap) {}

TilemapAutotileImpl::~TilemapAutotileImpl() = default;

scoped_refptr<Bitmap> TilemapAutotileImpl::Get(
    int32_t index,
    ExceptionState& exception_state) {
  if (!tilemap_)
    return nullptr;

  auto& autotiles = tilemap_->autotiles_;
  if (index < 0 || index >= static_cast<int32_t>(autotiles.size())) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "out range of autotiles");
    return nullptr;
  }

  return autotiles[index].bitmap;
}

void TilemapAutotileImpl::Put(int32_t index,
                              scoped_refptr<Bitmap> texture,
                              ExceptionState& exception_state) {
  if (!tilemap_)
    return;

  auto& autotiles = tilemap_->autotiles_;
  if (index < 0 || index >= static_cast<int32_t>(autotiles.size()))
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "out range of autotiles");

  autotiles[index].bitmap = CanvasImpl::FromBitmap(texture);
  autotiles[index].observer = autotiles[index].bitmap->AddCanvasObserver(
      base::BindRepeating(&TilemapImpl::AtlasModifyHandlerInternal, tilemap_));
  tilemap_->atlas_dirty_ = true;
}

///////////////////////////////////////////////////////////////////////////////
// TilemapImpl Implement

TilemapImpl::TilemapImpl(ExecutionContext* execution_context,
                         scoped_refptr<ViewportImpl> parent,
                         int32_t tilesize)
    : EngineObject(execution_context),
      Disposable(execution_context->disposable_parent),
      ground_node_(parent ? parent->GetDrawableController()
                          : execution_context->screen_drawable_node,
                   SortKey(0, 0, kTilemapSortStamp)),
      tilesize_(tilesize),
      max_atlas_size_(execution_context->render_device->MaxTextureSize()),
      max_vertical_count_(max_atlas_size_ / tilesize),
      viewport_(parent),
      repeat_(1) {
  ground_node_.RegisterEventHandler(base::BindRepeating(
      &TilemapImpl::GroundNodeHandlerInternal, base::Unretained(this)));

  GPUCreateTilemapInternal();
}

DISPOSABLE_DEFINITION(TilemapImpl);

void TilemapImpl::Update(ExceptionState& exception_state) {
  DISPOSE_CHECK;

  anim_index_ = ++anim_index_ % (max_anim_index_ * 16);

  flash_timer_ = ++flash_timer_ % 32;
  flash_opacity_ = std::abs(16 - flash_timer_) * 8 + 32;
}

scoped_refptr<TilemapAutotile> TilemapImpl::Autotiles(
    ExceptionState& exception_state) {
  return base::MakeRefCounted<TilemapAutotileImpl>(
      weak_ptr_factory_.GetWeakPtr());
}

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Viewport,
    scoped_refptr<Viewport>,
    TilemapImpl,
    { return viewport_; },
    {
      DISPOSE_CHECK;
      if (viewport_ == value)
        return;
      viewport_ = ViewportImpl::From(value);
      DrawNodeController* controller = viewport_
                                           ? viewport_->GetDrawableController()
                                           : context()->screen_drawable_node;
      ground_node_.RebindController(controller);
      for (auto& it : above_nodes_)
        it->RebindController(controller);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Tileset,
    scoped_refptr<Bitmap>,
    TilemapImpl,
    { return tileset_; },
    {
      DISPOSE_CHECK;
      CHECK_ATTRIBUTE_VALUE;
      if (tileset_ == value)
        return;
      tileset_ = CanvasImpl::FromBitmap(value);
      tileset_observer_ = tileset_->AddCanvasObserver(base::BindRepeating(
          &TilemapImpl::AtlasModifyHandlerInternal, base::Unretained(this)));
      atlas_dirty_ = true;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    MapData,
    scoped_refptr<Table>,
    TilemapImpl,
    {
      DISPOSE_CHECK_RETURN(nullptr);
      return map_data_;
    },
    {
      DISPOSE_CHECK;
      CHECK_ATTRIBUTE_VALUE;
      map_data_ = TableImpl::From(value);
      map_data_observer_ = map_data_->AddObserver(base::BindRepeating(
          &TilemapImpl::MapDataModifyHandlerInternal, base::Unretained(this)));
      map_buffer_dirty_ = true;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    FlashData,
    scoped_refptr<Table>,
    TilemapImpl,
    {
      DISPOSE_CHECK_RETURN(nullptr);
      return flash_data_;
    },
    {
      DISPOSE_CHECK;
      CHECK_ATTRIBUTE_VALUE;
      flash_data_ = TableImpl::From(value);
      flash_data_observer_ = map_data_->AddObserver(base::BindRepeating(
          &TilemapImpl::MapDataModifyHandlerInternal, base::Unretained(this)));
      map_buffer_dirty_ = true;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Priorities,
    scoped_refptr<Table>,
    TilemapImpl,
    {
      DISPOSE_CHECK_RETURN(nullptr);
      return priorities_;
    },
    {
      DISPOSE_CHECK;
      CHECK_ATTRIBUTE_VALUE;
      priorities_ = TableImpl::From(value);
      priorities_observer_ = priorities_->AddObserver(base::BindRepeating(
          &TilemapImpl::MapDataModifyHandlerInternal, base::Unretained(this)));
      map_buffer_dirty_ = true;
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Visible,
    bool,
    TilemapImpl,
    {
      DISPOSE_CHECK_RETURN(false);
      return ground_node_.GetVisibility();
    },
    {
      DISPOSE_CHECK;
      ground_node_.SetNodeVisibility(value);
      for (auto& it : above_nodes_)
        it->SetNodeVisibility(value);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Ox,
    int32_t,
    TilemapImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return origin_.x;
    },
    {
      DISPOSE_CHECK;
      if (origin_.x != value) {
        origin_.x = value;
        map_buffer_dirty_ = true;
      }
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Oy,
    int32_t,
    TilemapImpl,
    {
      DISPOSE_CHECK_RETURN(0);
      return origin_.y;
    },
    {
      DISPOSE_CHECK;
      if (origin_.y != value) {
        origin_.y = value;
        map_buffer_dirty_ = true;
      }
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    RepeatX,
    bool,
    TilemapImpl,
    {
      DISPOSE_CHECK_RETURN(false);
      return repeat_.x;
    },
    {
      DISPOSE_CHECK;
      if (repeat_.x != (value ? 1 : 0)) {
        repeat_.x = value;
        map_buffer_dirty_ = true;
      }
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    RepeatY,
    bool,
    TilemapImpl,
    {
      DISPOSE_CHECK_RETURN(false);
      return repeat_.y;
    },
    {
      DISPOSE_CHECK;
      if (repeat_.y != (value ? 1 : 0)) {
        repeat_.y = value;
        map_buffer_dirty_ = true;
      }
    });

void TilemapImpl::OnObjectDisposed() {
  ground_node_.DisposeNode();
  above_nodes_.clear();

  // Same handling as CanvasImpl's disposal: the atlas and the view bound to it may still be
  // read by a draw this frame recorded and the GPU has not finished. See
  // renderer::DeferredTextureRelease.
  renderer::DeferredTextureRelease(gpu_.atlas_texture);
  renderer::DeferredTextureRelease(gpu_.atlas_binding);

  GPUData empty_data;
  std::swap(gpu_, empty_data);
}

// Temporary diagnostics. The ground layer is composed in the enqueue stage and drawn in
// the draw stage, so the two counts must stay equal; a rebuild that yields no ground
// quad blanks the whole ground layer for one frame (the flicker seen on the TV).
static int64_t g_tm_ground_ready = 0;
static int64_t g_tm_ground_drawn = 0;
static int64_t s_above_frame = 0;
static int64_t s_above_this_frame = 0;
static int64_t s_above_prev_frame = 0;

// Pass-counter for the Mali second-pass workaround. Marker: URGE_SECONDPASS
static int64_t g_tm_second_pass = 0;





static int64_t g_tm_ground_empty = 0;

void TilemapImpl::GroundNodeHandlerInternal(
    DrawableNode::RenderStage stage,
    DrawableNode::RenderControllerParams* params) {
  // No table assigned yet (Tilemap.new without Tilemap#table): there is
  // nothing to parse, upload or draw - the GPU resources of the tilemap are
  // not created in that state.
  if (!map_data_)
    return;

  if (stage == DrawableNode::RenderStage::BEFORE_RENDER) {
    // Setup above layers if need.
    const auto viewport_bound = ground_node_.GetParentViewport()->bound;
    const auto viewport_origin = ground_node_.GetParentViewport()->origin;
    if (last_viewport_ != viewport_bound) {
      SetupTilemapLayersInternal(viewport_bound);
      last_viewport_ = viewport_bound;
    }

    // Reset above layers sorting order.
    UpdateViewportInternal(viewport_bound, viewport_origin);
    ResetAboveLayersOrderInternal();

    // Generate global atlas texture if need.
    if (atlas_dirty_) {
      std::vector<AtlasCompositeCommand> commands;
      base::Vec2i size = MakeAtlasInternal(commands);

      GPUMakeAtlasInternal(params->context, size, std::move(commands));
      atlas_dirty_ = false;
    }

    // Parse and generate map buffer if need.
    if (map_buffer_dirty_) {
      std::vector<renderer::Quad> ground_cache;
      std::vector<std::vector<renderer::Quad>> aboves_cache;
      ParseMapDataInternal(ground_cache, aboves_cache);

      GPUUploadTilesBatchInternal(params->context, std::move(ground_cache),
                                  std::move(aboves_cache));
      map_buffer_dirty_ = false;

      // A rebuild that produces no ground quad blanks the whole ground layer for that
      // frame while the above layers keep drawing - the flicker reported from the TV.
      if (!gpu_.ground_draw_count) {
        ++g_tm_ground_empty;
      }
    }

    // Update tilemap uniform per frame.
    GPUUpdateTilemapUniformInternal(params->context,
                                    render_offset_.Recast<float>(), tilesize_,
                                    anim_index_);
    ++g_tm_ground_ready;
  } else if (stage == DrawableNode::RenderStage::ON_RENDERING) {
    ++g_tm_ground_drawn;
    GPURenderGroundLayerInternal(params->context, params->world_binding);

    // See TilemapSecondPassEnabledInternal: same duplicate for the ground layer, in place.
    // Marker: URGE_SECONDPASS
    if (TilemapSecondPassEnabledInternal() && params->screen_buffer &&
        params->screen_depth_stencil &&
        ForcePassBoundaryInternal(params->context, params->screen_buffer,
                                  params->screen_depth_stencil)) {
      GPURenderGroundLayerInternal(params->context, params->world_binding);
      ++g_tm_second_pass;
    }
  }
}

// Draw the map a second time, in a *new* render pass. Marker: URGE_SECONDPASS
//
// On Mali's Vulkan driver a frame occasionally comes out with a rectangle of the map missing - one
// layer beneath shows through, every input to the draw is verified correct, and drawing the same
// geometry again inside the same pass does not repair it. What the measurements showed is that the
// loss is tied to the *pass*, not to a draw or a frame: with the map drawn once the loss appears
// nineteen times in twenty-one minutes of idle running (twenty-one events of twenty or more cells),
// and with the map drawn a second time in a pass of its own - same geometry, same state, one extra
// Store - it appears zero times in twenty-three minutes, at the same frame rate and with the picture
// unchanged pixel for pixel. Two independent passes would both have to lose the same tile, which is
// why this removes the visible defect rather than making it rarer.
//
// The duplicate is drawn immediately after the layer it duplicates - the pass boundary is forced in
// place - so the frame's draw order is unchanged and no other content can be covered by the second
// copy.
//
// Default: off. It was tried on for Mali GPUs and had to be withdrawn - a second draw
// re-blends translucent content so the picture can change - and the loss was still reported
// on the device. The switch file (tmsecondpass.txt) turns it on for a session.
static bool ForcePassBoundaryInternal(Diligent::IDeviceContext* render_context,
                                     Diligent::ITexture* screen_buffer,
                                     Diligent::ITexture* screen_depth) {
  if (!render_context || !screen_buffer || !screen_depth)
    return false;
  // End the current pass and reopen the target the interrupted pass published. See the note
  // in window_impl.cc: screen_buffer is the frame's screen, not this pass's target.
  // Marker: URGE_SECONDPASS
  UrgeForcePassBoundary(render_context);
  return true;
}

static bool TilemapSecondPassEnabledInternal() {
  // A second pass is needed on Mali (see the note in PipelineSet); other GPUs do not run it.
  return renderer::IsMaliGpu();
}

void TilemapImpl::AboveNodeHandlerInternal(
    int32_t layer_index,
    DrawableNode::RenderStage stage,
    DrawableNode::RenderControllerParams* params) {
  // Same guard as GroundNodeHandlerInternal: without a table the GPU
  // resources of the above layers were never created.
  if (!map_data_)
    return;

  if (stage == DrawableNode::RenderStage::ON_RENDERING) {
    const int64_t audit_frame = context()->sprite_batcher->FrameCount();
    if (audit_frame != s_above_frame) {
      s_above_prev_frame = s_above_this_frame;
      s_above_this_frame = 0;
      s_above_frame = audit_frame;
    }
    ++s_above_this_frame;

    GPURenderAboveLayerInternal(params->context, params->world_binding,
                                layer_index);

    // See TilemapSecondPassEnabledInternal. Marker: URGE_SECONDPASS
    //
    // The duplicate is drawn immediately after the layer it duplicates, with a forced pass boundary
    // in between, so the pair sits exactly where the single draw used to sit in the frame's order -
    // nothing else can be covered by it, and the two copies are the only thing between the boundary.
    if (TilemapSecondPassEnabledInternal() && params->screen_buffer &&
        params->screen_depth_stencil &&
        ForcePassBoundaryInternal(params->context, params->screen_buffer,
                                  params->screen_depth_stencil)) {
      GPURenderAboveLayerInternal(params->context, params->world_binding,
                                  layer_index);
      ++g_tm_second_pass;
    }
  }
}

base::Vec2i TilemapImpl::MakeAtlasInternal(
    std::vector<AtlasCompositeCommand>& commands) {
  // Rows a single atlas page can hold. A tileset taller than that is laid out
  // sideways: page p starts at column 8 tiles * p, which is exactly the offset
  // process_tileset() derives from max_vertical_count_.
  const int32_t page_rows = std::max(1, max_vertical_count_);
  const int32_t tileset_rows =
      Disposable::IsValid(tileset_.get())
          ? ((**tileset_)->size.y + tilesize_ - 1) / tilesize_
          : 0;
  const int32_t tileset_pages =
      tileset_rows > 0 ? (tileset_rows + page_rows - 1) / page_rows : 0;

  int32_t atlas_height = 28 * tilesize_;
  if (tileset_rows > 0)
    atlas_height =
        std::max(atlas_height, std::min(tileset_rows, page_rows) * tilesize_);
  atlas_height = std::min(max_atlas_size_, atlas_height);
  max_anim_index_ = 1;

  // Setup commands
  commands.clear();

  // Autotile part
  int32_t autotile_offset = 0;
  for (auto& it : autotiles_) {
    if (!Disposable::IsValid(it.bitmap.get())) {
      ++autotile_offset;
      continue;
    }

    const auto autotile_size = (**it.bitmap)->size;
    const base::Vec2i dst_pos(0, autotile_offset * tilesize_ * 4);

    if (autotile_size.y >= tilesize_ * 4) {
      // Animated autotile (3 * 4)
      it.type = AutotileType::COMMON;
      it.animation_frame_count = autotile_size.x / (3 * tilesize_);
    } else if (autotile_size.y <= tilesize_) {
      // Single animated tile (1 * 1)
      it.type = AutotileType::SINGLE;
      it.animation_frame_count = autotile_size.x / tilesize_;

      for (int32_t i = 0; i < 4; ++i) {
        base::Rect single_src(base::Vec2i(tilesize_ * i, 0),
                              base::Vec2i(tilesize_));
        base::Vec2i single_dst(base::Vec2i(tilesize_ * i * 3, 0));

        commands.push_back({**it.bitmap, single_src, single_dst});
      }

      ++autotile_offset;
      continue;
    } else {
      NOTREACHED();
    }

    // Make tileset offset
    max_autotile_frame_count_ =
        std::max(max_autotile_frame_count_, it.animation_frame_count);

    // Make anim loop count
    max_anim_index_ *= it.animation_frame_count;

    commands.push_back({**it.bitmap, autotile_size, dst_pos});
    ++autotile_offset;
  }

  // Tileset part. One composite command per page, each one copying the rows of
  // that page into the columns it owns.
  int32_t tileset_width = 0;
  if (Disposable::IsValid(tileset_.get())) {
    BitmapTexture* tileset_data = **tileset_;
    const int32_t page_count = std::max(1, tileset_pages);
    tileset_width = tileset_data->size.x * page_count;

    for (int32_t page = 0; page < page_count; ++page) {
      const int32_t top_row = page * page_rows;
      if (top_row >= tileset_rows)
        break;

      const int32_t rows = std::min(page_rows, tileset_rows - top_row);
      base::Rect src_rect(0, top_row * tilesize_, tileset_data->size.x,
                          rows * tilesize_);
      base::Vec2i dst_pos(3 * tilesize_ * max_autotile_frame_count_ +
                              8 * tilesize_ * page,
                          0);
      commands.push_back({tileset_data, src_rect, dst_pos});
    }
  }

  return base::Vec2i(3 * tilesize_ * max_autotile_frame_count_ + tileset_width,
                     atlas_height);
}

void TilemapImpl::UpdateViewportInternal(const base::Rect& viewport,
                                         const base::Vec2i& viewport_origin) {
  const base::Vec2i tilemap_origin = origin_ + viewport_origin;
  const base::Vec2i viewport_size = viewport.Size();

  base::Rect new_viewport;
  new_viewport.x = tilemap_origin.x / tilesize_;
  new_viewport.y = tilemap_origin.y / tilesize_ - 1;
  new_viewport.width =
      (viewport_size.x / tilesize_) + !!(viewport_size.x % tilesize_) + 1;
  new_viewport.height =
      (viewport_size.y / tilesize_) + !!(viewport_size.y % tilesize_) + 2;

  const base::Vec2i display_offset(tilemap_origin.x % tilesize_,
                                   tilemap_origin.y % tilesize_);
  render_offset_ = -display_offset;
  render_offset_.y -= tilesize_;

  // Apply viewport origin
  render_offset_ += viewport_origin;

  if (new_viewport != render_viewport_) {
    render_viewport_ = new_viewport;
    map_buffer_dirty_ = true;
  }
}

void TilemapImpl::ParseMapDataInternal(
    std::vector<renderer::Quad>& ground_cache,
    std::vector<std::vector<renderer::Quad>>& aboves_cache) {
  auto set_autotile_pos = [&](base::RectF& pos, int32_t index) {
    switch (index) {
      case 0:  // Left Top
        break;
      case 1:  // Right Top
        pos.x += tilesize_ / 2.0f;
        break;
      case 2:  // Left Bottom
        pos.y += tilesize_ / 2.0f;
        break;
      case 3:  // Right Bottom
        pos.x += tilesize_ / 2.0f;
        pos.y += tilesize_ / 2.0f;
        break;
      default:
        break;
    }
  };

  auto get_priority = [&](int16_t tile_id) -> int32_t {
    if (!priorities_ || tile_id >= static_cast<int32_t>(priorities_->x_size()))
      return 0;

    int16_t value = priorities_->value(tile_id, 0, 0);
    if (value > 5)
      return -1;

    return value;
  };

  auto process_autotile = [&](const base::Vec2i& pos, int16_t tile_id,
                              const base::Vec4& color,
                              std::vector<renderer::Quad>* target) {
    // Autotile (0-7)
    int32_t autotile_id = tile_id / 48 - 1;
    // Pattern (0-47)
    int32_t pattern_id = tile_id % 48;

    // Autotile invalid check
    AutotileInfo& info = autotiles_[autotile_id];
    if (!Disposable::IsValid(info.bitmap.get()))
      return;

    // Blend color + frame count
    base::Vec4 blend_color_and_frame_count = color;
    blend_color_and_frame_count.a = info.animation_frame_count;

    // Generate from autotile type
    switch (info.type) {
      case AutotileType::COMMON: {
        const base::Vec2* autotile_src_pos = kAutotileSrcRegular[pattern_id];
        for (int32_t i = 0; i < 4; ++i) {
          base::RectF tex_src(autotile_src_pos[i], base::Vec2(0.5f));
          tex_src.x = tex_src.x * tilesize_ + 0.5f;
          tex_src.y = tex_src.y * tilesize_ + 0.5f;
          tex_src.width = tex_src.width * tilesize_ - 1.0f;
          tex_src.height = tex_src.height * tilesize_ - 1.0f;

          // Horizontal offset (4 * tileSize)
          tex_src.y += autotile_id * 4 * tilesize_;

          base::RectF chunk_pos(pos.x * tilesize_, pos.y * tilesize_,
                                tilesize_ / 2.0f, tilesize_ / 2.0f);
          set_autotile_pos(chunk_pos, i);

          renderer::Quad quad;
          renderer::Quad::SetPositionRect(&quad, chunk_pos);
          renderer::Quad::SetTexCoordRectNorm(&quad, tex_src);
          renderer::Quad::SetColor(&quad, blend_color_and_frame_count);
          target->push_back(quad);
        }
      } break;
      case AutotileType::SINGLE: {
        const base::RectF single_tex(0.5f,
                                     0.5f + autotile_id * tilesize_ * 4.0f,
                                     tilesize_ - 1.0f, tilesize_ - 1.0f);
        const base::RectF single_pos(pos.x * tilesize_, pos.y * tilesize_,
                                     tilesize_, tilesize_);

        renderer::Quad quad;
        renderer::Quad::SetPositionRect(&quad, single_pos);
        renderer::Quad::SetTexCoordRectNorm(&quad, single_tex);
        renderer::Quad::SetColor(&quad, blend_color_and_frame_count);
        target->push_back(quad);
      } break;
      default:
        NOTREACHED();
        break;
    }
  };

  auto value_wrap = [&](int32_t value, int32_t range) {
    int32_t res = value % range;
    return res < 0 ? res + range : res;
  };

  auto get_wrap_data = [&](scoped_refptr<TableImpl> t, int32_t x, int32_t y,
                           int32_t z) -> int16_t {
    if (!t)
      return 0;

    auto tile_x = repeat_.x ? value_wrap(x, t->x_size()) : x;
    auto tile_y = repeat_.y ? value_wrap(y, t->y_size()) : y;

    if (!repeat_.x && (x < 0 || x >= static_cast<int32_t>(t->x_size())))
      return 0;
    if (!repeat_.y && (y < 0 || y >= static_cast<int32_t>(t->x_size())))
      return 0;

    return t->value(tile_x, tile_y, z);
  };

  auto process_tile = [&](const base::Vec2i& pos, int32_t z) {
    int32_t tile_id = get_wrap_data(map_data_, pos.x + render_viewport_.x,
                                    pos.y + render_viewport_.y, z);
    int32_t flash_color = get_wrap_data(flash_data_, pos.x + render_viewport_.x,
                                        pos.y + render_viewport_.y, 0);

    if (tile_id < 48)
      return;

    int32_t priority = get_priority(tile_id);
    if (priority == -1)
      return;

    std::vector<renderer::Quad>* target;
    if (!priority) {
      // Ground layer
      target = &ground_cache;
    } else {
      // Above multi layers
      target = &aboves_cache[pos.y + priority];
    }

    // Flash color & Frame count
    base::Vec4 blend_color_and_frame_count;
    blend_color_and_frame_count.a = 1.0f;

    // Composite flash color
    if (flash_color) {
      blend_color_and_frame_count.b = ((flash_color & 0x000F) >> 0) / 0xF;
      blend_color_and_frame_count.g = ((flash_color & 0x00F0) >> 4) / 0xF;
      blend_color_and_frame_count.r = ((flash_color & 0x0F00) >> 8) / 0xF;
    }

    if (tile_id < 48 * 8)
      return process_autotile(pos, tile_id, blend_color_and_frame_count,
                              target);

    int32_t tileset_id = tile_id - 48 * 8;
    int32_t tile_x = tileset_id % 8;
    int32_t tile_y = tileset_id / 8;

    // Switch to next offset when reaching the hardware texture limit
    const int32_t horizontal_offset = tile_y / max_vertical_count_;

    base::Vec2 atlas_offset(
        tile_x + 3 * max_autotile_frame_count_ + 8 * horizontal_offset,
        tile_y - max_vertical_count_ * horizontal_offset);
    base::RectF quad_tex(atlas_offset.x * tilesize_ + 0.5f,
                         atlas_offset.y * tilesize_ + 0.5f, tilesize_ - 1.0f,
                         tilesize_ - 1.0f);
    base::RectF quad_pos(pos.x * tilesize_, pos.y * tilesize_, tilesize_,
                         tilesize_);

    renderer::Quad quad;
    renderer::Quad::SetPositionRect(&quad, quad_pos);
    renderer::Quad::SetTexCoordRectNorm(&quad, quad_tex);
    renderer::Quad::SetColor(&quad, blend_color_and_frame_count);
    target->push_back(quad);
  };

  auto process_buffer = [&]() {
    // A Tilemap may enter a render pass before any table was assigned
    // (Tilemap.new + immediate Graphics.update). Guard the map_data_
    // dereference below against a not-yet-assigned table.
    if (!map_data_)
      return;

    for (int32_t x = 0; x < render_viewport_.width; ++x)
      for (int32_t y = 0; y < render_viewport_.height; ++y)
        for (int32_t z = 0; z < static_cast<int32_t>(map_data_->z_size()); ++z)
          process_tile(base::Vec2i(x, y), z);
  };

  // Process map buffer
  int32_t above_layers_count = render_viewport_.height + 5;
  aboves_cache.resize(above_layers_count);

  // Begin to parse buffer data
  process_buffer();
}

void TilemapImpl::SetupTilemapLayersInternal(const base::Rect& viewport) {
  ground_node_.SetNodeSortWeight(0);

  above_nodes_.clear();
  int32_t above_layers_count =
      (viewport.height / tilesize_) + !!(viewport.height % tilesize_) + 7;
  for (int32_t i = 0; i < above_layers_count; ++i) {
    auto above_node = std::make_unique<DrawableNode>(
        ground_node_.GetController(),
        SortKey(64, 0, kTilemapSortStamp + 1));
    above_node->RegisterEventHandler(base::BindRepeating(
        &TilemapImpl::AboveNodeHandlerInternal, base::Unretained(this), i));
    above_nodes_.push_back(std::move(above_node));
  }
}

void TilemapImpl::ResetAboveLayersOrderInternal() {
  for (int32_t i = 0; i < static_cast<int32_t>(above_nodes_.size()); ++i) {
    /*
      Reference to RMXP f1 help document:
        1. Tiles with a priority of 0 always have a Z-coordinate of 0.
        2. Priority 1 tiles placed at the top edge of the screen have a
      Z-coordinate of 64.
        3. Every time the priority increases by 1 or the next tile down is
      selected, the Z-coordinate increases by 32.
        4. The Z-coordinate changes accordingly as the tilemap scrolls
      vertically.
    */
    int32_t layer_order = 32 * (i + render_viewport_.y + 1) - origin_.y;
    above_nodes_[i]->SetNodeSortWeight(layer_order);
  }
}

void TilemapImpl::AtlasModifyHandlerInternal() {
  atlas_dirty_ = true;
}

void TilemapImpl::MapDataModifyHandlerInternal() {
  map_buffer_dirty_ = true;
}

void TilemapImpl::GPUCreateTilemapInternal() {
  gpu_.batch = renderer::QuadBatch::Make(**context()->render_device);
  gpu_.shader_binding =
      context()->render.pipeline_loader->tilemap.CreateBinding();
  // See GPUData::above_binding: the above layers draw through their own binding.
  gpu_.above_binding =
      context()->render.pipeline_loader->tilemap.CreateBinding();

  Diligent::CreateUniformBuffer(
      **context()->render_device, sizeof(renderer::Binding_Tilemap::Params),
      "tilemap.uniform", &gpu_.uniform_buffer, Diligent::USAGE_DEFAULT,
      Diligent::BIND_UNIFORM_BUFFER, Diligent::CPU_ACCESS_NONE);

  // Per-frame-set path, see GPUData::batch_sets.
  for (int32_t i = 0; i < GPUData::kFrameSets; ++i) {
    gpu_.batch_sets[i] = renderer::QuadBatch::Make(**context()->render_device);
    Diligent::CreateUniformBuffer(
        **context()->render_device, sizeof(renderer::Binding_Tilemap::Params),
        "tilemap.uniform.set", &gpu_.uniform_buffer_sets[i],
        Diligent::USAGE_DEFAULT, Diligent::BIND_UNIFORM_BUFFER,
        Diligent::CPU_ACCESS_NONE);
  }
}

void TilemapImpl::GPUMakeAtlasInternal(
    Diligent::IDeviceContext* render_context,
    const base::Vec2i& atlas_size,
    std::vector<TilemapImpl::AtlasCompositeCommand> make_commands) {
  // Reuse the atlas texture whenever it is already big enough. Re-creating it makes every
  // tile draw still executing - and every draw of the frame that samples it before the
  // per-piece copies below have landed - read a brand new, still empty texture, which
  // shows up as a few tiles of wrong content for one frame. Updating the existing texture
  // in place keeps its content valid; the copies below replace the pieces that changed.
  if (!gpu_.atlas_texture ||
      gpu_.atlas_texture->GetDesc().Width < static_cast<uint32_t>(atlas_size.x) ||
      gpu_.atlas_texture->GetDesc().Height < static_cast<uint32_t>(atlas_size.y)) {
    // Growing the atlas replaces it, and the draw of a frame still executing samples the old
    // one; park it for a few frames instead of destroying it here. See
    // renderer::DeferredTextureRelease.
    renderer::DeferredTextureRelease(gpu_.atlas_texture);
    renderer::CreateTexture2D(**context()->render_device, &gpu_.atlas_texture,
                              "tilemap.atlas", atlas_size);
    gpu_.atlas_binding = gpu_.atlas_texture->GetDefaultView(
        Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
  }

  for (auto& it : make_commands) {
    Diligent::CopyTextureAttribs copy_attribs;
    Diligent::Box box;

    const base::Rect real_src_rect =
        base::MakeIntersect(it.src_rect, it.texture->size);
    if (real_src_rect.width <= 0 || real_src_rect.height <= 0)
      continue;

    // A paged source (tileset taller than one texture) is copied page by page:
    // every command covers rows of a single page, and the source box is
    // relative to that page.
    Diligent::ITexture* source_texture = it.texture->data.RawPtr();
    int32_t source_y = real_src_rect.y;
    if (it.texture->IsPaged()) {
      const int32_t source_page = it.texture->PageIndexAt(real_src_rect.y);
      source_texture = it.texture->pages[source_page].RawPtr();
      if (!source_texture)
        continue;
      source_y -= it.texture->PageTop(source_page);
    }

    copy_attribs.pSrcTexture = source_texture;
    copy_attribs.pSrcBox = &box;
    copy_attribs.SrcTextureTransitionMode =
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;

    box.MinX = real_src_rect.x;
    box.MinY = source_y;
    box.MaxX = real_src_rect.x + real_src_rect.width;
    box.MaxY = source_y + real_src_rect.height;

    copy_attribs.pDstTexture = gpu_.atlas_texture;
    copy_attribs.DstX = it.dst_pos.x;
    copy_attribs.DstY = it.dst_pos.y;
    copy_attribs.DstTextureTransitionMode =
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;

    render_context->CopyTexture(copy_attribs);
  }

}



void TilemapImpl::GPUUploadTilesBatchInternal(
    Diligent::IDeviceContext* render_context,
    std::vector<renderer::Quad> ground_cache,
    std::vector<std::vector<renderer::Quad>> aboves_cache) {
  std::vector<renderer::Quad> total_quads;
  gpu_.ground_draw_count = ground_cache.size();

  int32_t offset = ground_cache.size();
  gpu_.above_draw_count.clear();
  for (const auto& it : aboves_cache) {
    gpu_.above_draw_count.push_back(it.size());
    offset += it.size();
  }

  // Make total buffer
  total_quads.resize(offset);
  std::memcpy(total_quads.data(), ground_cache.data(),
              ground_cache.size() * sizeof(renderer::Quad));
  int32_t buffer_offset = ground_cache.size();
  for (const auto& it : aboves_cache) {
    std::memcpy(total_quads.data() + buffer_offset, it.data(),
                it.size() * sizeof(renderer::Quad));
    buffer_offset += it.size();
  }

  if (!total_quads.empty()) {
    gpu_.batch.QueueWrite(render_context, total_quads.data(), total_quads.size());

    // Allocate quad index
    context()->render.quad_index->Allocate(offset);
  }
}

void TilemapImpl::GPUUpdateTilemapUniformInternal(
    Diligent::IDeviceContext* render_context,
    const base::Vec2& offset,
    int32_t tilesize,
    int32_t anim_index) {
  base::Vec2i atlas_size(gpu_.atlas_texture->GetDesc().Width,
                         gpu_.atlas_texture->GetDesc().Height);

  renderer::Binding_Tilemap::Params uniform;
  uniform.OffsetAndTexSize =
      base::Vec4(offset.x, offset.y, 1.0f / atlas_size.x, 1.0f / atlas_size.y);
  uniform.AnimateIndexAndTileSizeAndFlashAlpha.x = anim_index / 16;
  uniform.AnimateIndexAndTileSizeAndFlashAlpha.y = tilesize;
  uniform.AnimateIndexAndTileSizeAndFlashAlpha.z = flash_opacity_ / 255.0f;
  uniform.AnimateIndexAndTileSizeAndFlashAlpha.w =
      static_cast<float>(context()->sprite_batcher->FrameCount() % 300);

  // Candidate fix, see GPUData::batch_sets: with the ring on this frame's parameters go into
  // this frame's own buffer, picked by the same frame set the quads were uploaded to.
  Diligent::IBuffer* uniform_target = gpu_.uniform_buffer;
  gpu_.uniform_in_use = uniform_target;

  // Temporary: one line per frame with the values the ground shader will read. A frame
  // that draws the ground with a stale atlas size or a wrong offset scrambles every
  // tile's UV and shows bright, unrelated atlas regions - the one-frame flicker. The buffers
  // logged are the ones this frame will really draw from, so a session can be split by the
  // ring afterwards without having to flip switches in step with it.

  render_context->UpdateBuffer(
      uniform_target, 0, sizeof(uniform), &uniform,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void TilemapImpl::GPURenderGroundLayerInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::IBuffer* world_binding) {
  if (!gpu_.atlas_texture)
    return;



  if (gpu_.ground_draw_count) {
    auto* pipeline = context()->render.pipeline_states->tilemap.RawPtr();

    // Setup uniform params
    gpu_.shader_binding.u_transform->Set(world_binding);
    gpu_.shader_binding.u_texture->Set(gpu_.atlas_binding);
    Diligent::IBuffer* params_binding =
        gpu_.uniform_in_use ? gpu_.uniform_in_use : gpu_.uniform_buffer;
    gpu_.shader_binding.u_params->Set(params_binding);

    // Apply pipeline state
    render_context->SetPipelineState(pipeline);
    render_context->CommitShaderResources(
        *gpu_.shader_binding,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Apply vertex index, from this frame's own set when the ring is on (see
    // GPUData::batch_sets).
    Diligent::IBuffer* const vertex_buffer = *gpu_.batch;
    render_context->SetVertexBuffers(
        0, 1, &vertex_buffer, nullptr,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    render_context->SetIndexBuffer(
        **context()->render.quad_index, 0,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Execute render command
    Diligent::DrawIndexedAttribs draw_indexed_attribs;
    draw_indexed_attribs.NumIndices = 6 * gpu_.ground_draw_count;
    draw_indexed_attribs.IndexType =
        context()->render.quad_index->GetIndexType();

    render_context->DrawIndexed(draw_indexed_attribs);

  }
}

void TilemapImpl::GPURenderAboveLayerInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::IBuffer* world_binding,
    int32_t index) {
  if (!gpu_.atlas_texture)
    return;

  if (!gpu_.above_draw_count.empty()) {
    if (int32_t draw_count = gpu_.above_draw_count[index]) {
      int32_t draw_offset = gpu_.ground_draw_count;
      for (int32_t i = 0; i < index; ++i)
        draw_offset += gpu_.above_draw_count[i];

      auto* pipeline = context()->render.pipeline_states->tilemap.RawPtr();

      // See GPUData::above_binding: this draw owns its binding.
      renderer::Binding_Tilemap& layer_binding = gpu_.above_binding;

      // Setup uniform params
      layer_binding.u_transform->Set(world_binding);
      layer_binding.u_texture->Set(gpu_.atlas_binding);
      Diligent::IBuffer* above_params_binding =
          gpu_.uniform_in_use ? gpu_.uniform_in_use : gpu_.uniform_buffer;
      layer_binding.u_params->Set(above_params_binding);

      // Apply pipeline state
      render_context->SetPipelineState(pipeline);
      render_context->CommitShaderResources(
          *layer_binding,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

      // Apply vertex index, from this frame's own set when the ring is on (see
      // GPUData::batch_sets).
      Diligent::IBuffer* const vertex_buffer =
*gpu_.batch;
      render_context->SetVertexBuffers(
          0, 1, &vertex_buffer, nullptr,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      render_context->SetIndexBuffer(
          **context()->render.quad_index, 0,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

      // Execute render command
      Diligent::DrawIndexedAttribs draw_indexed_attribs;
      draw_indexed_attribs.NumIndices = 6 * draw_count;
      draw_indexed_attribs.IndexType =
          context()->render.quad_index->GetIndexType();
      draw_indexed_attribs.FirstIndexLocation = draw_offset * 6;
      render_context->DrawIndexed(draw_indexed_attribs);


    }
  }
}

}  // namespace content
