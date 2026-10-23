// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/render/tilemap_impl.h"

#include "content/context/execution_context.h"
#include "renderer/device/gpu_driver.h"
#include "renderer/utils/texture_utils.h"

// Defined in content/screen/renderscreen_impl.cc, declared after the includes because it
// names a Diligent type. Marker: URGE_SECONDPASS
extern "C" void UrgeForcePassBoundary(Diligent::IDeviceContext* render_context);
// Defined in content/screen/renderscreen_impl.cc. Marker: URGE_DIAG_GATE
extern "C" bool UrgeDiagEnabled();

// TEMPORARY A/B switch, see the Ox/Oy setters. Marker: URGE_OXDIRTY
//
// Ox and Oy used to dirty the tile batch unconditionally, and a game that assigns tilemap.ox/.oy
// every frame - the usual way a map view follows the player - then re-parsed the whole map and
// re-uploaded the entire vertex batch every frame: about 24 uploads a second in a completely idle
// scene, as [UploadAudit] showed. That rewrite races the draws the GPU is still reading, which is
// the shape of the one-frame region loss this file has been chasing. The setters now dirty only when
// the value actually changes, matching RepeatX/RepeatY right below them. Setting
// urge_debug/oxalwaysdirty.txt to 1 restores the old behaviour so the two can be compared in one
// session, which is the only way this metric can be trusted.
// TEMPORARY: which code path marked the tile batch dirty, so the upload audit line can say why the
// buffer is being rewritten. The map is being re-uploaded about 24 times a second in an idle scene,
// and the difference between "the game changes the map every frame" and "the engine re-dirties it on
// its own" is the difference between a design consequence and a bug. Marker: URGE_UPLOAD_AUDIT
static const char* g_tm_dirty_source = "?";

static bool TilemapAlwaysDirtyInternal() {
  static int32_t s_poll = 0;
  static bool s_enabled = false;
  if (++s_poll >= 30) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "oxalwaysdirty.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_enabled = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/oxalwaysdirty.txt");
    }
  }
  return s_enabled;
}

namespace content {

// TEMPORARY, see their definitions further down. Marker: URGE_SECONDPASS
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
  ground_node_.SetDebugTag(0x1000);  // TEMPORARY, see the order audit. Marker: URGE_ORDER_AUDIT
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
      // See TilemapAlwaysDirtyInternal. Only a real change may dirty the batch: rewriting it every
      // frame races the GPU's read of the previous frame. Marker: URGE_OXDIRTY
      if (origin_.x != value || TilemapAlwaysDirtyInternal()) {
        origin_.x = value;
        map_buffer_dirty_ = true;
        g_tm_dirty_source = "ox";
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
      // See TilemapAlwaysDirtyInternal. Marker: URGE_OXDIRTY
      if (origin_.y != value || TilemapAlwaysDirtyInternal()) {
        origin_.y = value;
        map_buffer_dirty_ = true;
        g_tm_dirty_source = "oy";
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
// TEMPORARY audit state, see AboveNodeHandlerInternal. Marker: URGE_ABOVE_AUDIT
static int64_t s_above_frame = 0;
static int64_t s_above_this_frame = 0;
static int64_t s_above_prev_frame = 0;

// TEMPORARY, see TilemapSecondPassEnabledInternal. Marker: URGE_SECONDPASS
static int64_t g_tm_second_pass = 0;


// TEMPORARY binding audit, see the draw sites. Marker: URGE_BIND_AUDIT
//
// The probe has narrowed the defect to "in one frame the tilemap's draw does not cover part of the
// map": the tinted pixels do not come out of this pipeline at all in that frame, the layer below
// shows through, and neither the number of draws nor the submitted ranges change. What is left is
// what that one draw was bound to. Every value below is constant while the map is idle (the vertex
// ring and the params ring are off by default), so this prints only when one of them changes - and
// mode 11, which binds the world matrix into the params slot, is on record as reproducing the same
// picture, which is the shape of failure this looks for.
struct TilemapBindingSample {
  const void* pipeline = nullptr;
  const void* srb = nullptr;
  const void* vertex_buffer = nullptr;
  const void* index_buffer = nullptr;
  const void* params_binding = nullptr;
  int32_t num_indices = 0;
  int32_t first_index = 0;
};

static void AuditTilemapBindingInternal(const char* what, int32_t index,
                                        const TilemapBindingSample& sample) {
  // 0 = ground, 1..63 = above layers, 95 = anything else.
  static TilemapBindingSample slots[96];
  static bool used[96] = {};
  const int32_t slot = (index >= 0 && index < 95) ? index : 95;
  const TilemapBindingSample& prev = slots[slot];
  const bool changed = used[slot] && (prev.pipeline != sample.pipeline ||
                                      prev.srb != sample.srb ||
                                      prev.vertex_buffer != sample.vertex_buffer ||
                                      prev.index_buffer != sample.index_buffer ||
                                      prev.params_binding != sample.params_binding ||
                                      prev.num_indices != sample.num_indices ||
                                      prev.first_index != sample.first_index);
  if (changed)
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[BindAudit] " << what << " idx=" << index << " pipeline "
              << prev.pipeline << "->" << sample.pipeline << " srb " << prev.srb << "->"
              << sample.srb << " vbuf " << prev.vertex_buffer << "->"
              << sample.vertex_buffer << " ibuf " << prev.index_buffer << "->"
              << sample.index_buffer << " params " << prev.params_binding << "->"
              << sample.params_binding << " num " << prev.num_indices << "->"
              << sample.num_indices << " first " << prev.first_index << "->"
              << sample.first_index;
  slots[slot] = sample;
  used[slot] = true;
}

// TEMPORARY binding audit, see GPURenderGroundLayerInternal / GPURenderAboveLayerInternal.
// Marker: URGE_BIND_AUDIT
//
// The probe has narrowed the defect to "in one frame the tilemap's draw does not cover part of the
// map": the tinted pixels do not come out of this pipeline at all in that frame, the layer below
// shows through, the draw counts and submitted ranges never change, and the vertex data is written
// once at map load. What is left is what the draw was bound to. Every one of these pointers is
// constant while the map is idle (the vertex ring and the params ring are off by default), so this
// prints only when one of them actually changes - and mode 11, which binds the world matrix into
// the params slot, is on record as reproducing this same picture.
// TEMPORARY mitigation switch: draw each tilemap layer twice per frame. Marker: URGE_DRAW2
//
// The one-frame region loss has survived every test that can be made from inside the application:
// with the parameters, atlas, tint branch, draw counts, submitted ranges, bindings, vertex buffer
// ownership, scissor stack, both matrices and the node walk order all verified unchanged, a frame
// still comes out with a rectangle of the map missing and the layer below showing through. What is
// left is that a single execution of the draw does not cover everything it should. The pipeline is
// opaque, so drawing the same geometry a second time overwrites the first pixel for pixel and
// costs only the rasterisation: if the loss is one execution going wrong, the second execution
// fills it in. If it survives the duplicate, the draw's *state* is what is wrong and this will not
// help. Shared by the ground and the above layers, because the affected region is the one the
// above layers paint.
// TEMPORARY mitigation switch: split each above-layer draw into four sub-draws. Marker:
// URGE_SPLIT4
//
// Drawing the same geometry twice with an identical command did not fill the lost region, which
// says the loss is not tied to one execution of one command. Splitting the range into four
// commands with different offsets is the cheap next step: if the loss is per-command, the other
// three commands still cover the region; if it is a property of the frame's rendering context,
// this will not help either and the mitigation has to move above the draw call entirely.
// TEMPORARY: pick the tilemap pipeline that does not write depth. Marker: URGE_NOWRITE
//
// The pipeline is built as "no depth test" but Diligent's default leaves DepthWriteEnable true, so
// it asks to write a depth buffer it does not test against - a mismatch every driver implements
// somewhere different. Everything else about this draw is verified constant, and the offscreen arm
// showed this driver loses a region far more readily when the pass is configured differently (about
// 50 losses a minute against 1.5), which puts the depth/stencil state of the pass in scope. The
// scissor variant this slot used to hold was A/B'd and changed nothing (21 against 16 events over
// 13.7 minutes each), so the slot is reused for this question.
// TEMPORARY A/B. Marker: URGE_FLUSH
//
// Flush after every above-layer draw, so the command submission is split into one command buffer
// per layer instead of one per frame. If the driver's one-frame coverage loss comes from how a
// submission is built or retired, moving the boundary should show it; if the loss follows the layer
// regardless of where the boundaries fall, it does not. Costs a submit per layer, so this is an
// experiment rather than something to ship.
// TEMPORARY: pick the tilemap pipeline that does not blend. Marker: URGE_NOBLEND
//
// See PipelineSet::tilemap_noblend. If the region loss is the blend's write-back going missing, the
// frame's destination content - the base layer - is what stays behind, which is what the probe sees.
// Turning blending off turns the draw into a plain write, so the loss should have nowhere to hide.
// TEMPORARY: what rectangle the draw ran under. Marker: URGE_SCISSOR_AUDIT
// See g_scissor_current in drawable_controller.h. The map viewport's rectangle is constant for a
// session, so a frame in which the value seen here differs is the stale-scissor case - which would
// clip one region for one frame and survive a duplicate draw of the same geometry.
static void AuditScissorAtDrawInternal(const char* what) {
  static int32_t s_last[4] = {-1, -1, -1, -1};
  static bool s_any = false;
  if (!s_any || s_last[0] != g_scissor_current[0] || s_last[1] != g_scissor_current[1] ||
      s_last[2] != g_scissor_current[2] || s_last[3] != g_scissor_current[3]) {
    if (s_any)
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[ScissorDraw] " << what << " rect (" << s_last[0] << "," << s_last[1] << ","
                << s_last[2] << "x" << s_last[3] << ") -> (" << g_scissor_current[0] << ","
                << g_scissor_current[1] << "," << g_scissor_current[2] << "x"
                << g_scissor_current[3] << ")";
    for (int32_t i = 0; i < 4; ++i) s_last[i] = g_scissor_current[i];
    s_any = true;
  }
}

static void AuditBindingChangeInternal(const char* what, int32_t index,
                                       const void* params,
                                       const void* vertices,
                                       const void* srb) {
  static const void* s_params[64] = {};
  static const void* s_vertices[64] = {};
  static const void* s_srb[64] = {};
  const int32_t slot = index & 63;
  const bool first = !s_params[slot] && !s_vertices[slot] && !s_srb[slot];
  if (s_params[slot] != params || s_vertices[slot] != vertices ||
      s_srb[slot] != srb) {
    if (!first)
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[BindAudit] " << what << " idx=" << index
                << " params " << s_params[slot] << "->" << params
                << " vertices " << s_vertices[slot] << "->" << vertices
                << " srb " << s_srb[slot] << "->" << srb;
    s_params[slot] = params;
    s_vertices[slot] = vertices;
    s_srb[slot] = srb;
  }
}
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
      // TEMPORARY correlation instrument. Marker: URGE_UPLOAD_AUDIT
      //
      // A rebuild replaces the whole atlas texture and re-points the binding at it, which is the
      // only remaining per-frame event that changes what the tilemap samples - and unlike every
      // input measured so far, it has never appeared in a log next to a defect frame. The earlier
      // work only A/B'd the deferred-release switch, which is a different question. It is rare (a
      // scene change, an animation frame), so volume is not a concern and timing is untouched.
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[UploadAudit] f=" << context()->sprite_batcher->FrameCount()
                << " ATLAS rebuild " << size.x << "x" << size.y << " tex="
                << gpu_.atlas_texture.RawPtr();
    }

    // Parse and generate map buffer if need.
    if (map_buffer_dirty_) {
      std::vector<renderer::Quad> ground_cache;
      std::vector<std::vector<renderer::Quad>> aboves_cache;
      ParseMapDataInternal(ground_cache, aboves_cache);

      GPUUploadTilesBatchInternal(params->context, std::move(ground_cache),
                                  std::move(aboves_cache));
      map_buffer_dirty_ = false;
      // See the note above the atlas rebuild. Marker: URGE_UPLOAD_AUDIT
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[UploadAudit] f=" << context()->sprite_batcher->FrameCount()
                << " BATCH reupload ground=" << gpu_.ground_draw_count
                << " src=" << g_tm_dirty_source;
      // The marker is deliberately cleared here: it is sticky, so without this every later upload
      // would keep reporting whichever path dirtied the buffer first. A "?" on the next line means
      // the dirty came from one of the property setters rather than the viewport or the game's
      // map-data call. Marker: URGE_UPLOAD_AUDIT
      g_tm_dirty_source = "?";

      // A rebuild that produces no ground quad blanks the whole ground layer for that
      // frame while the above layers keep drawing - the flicker reported from the TV.
      if (!gpu_.ground_draw_count) {
        ++g_tm_ground_empty;
        if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
        LOG(INFO) << "[LayerAudit] ground empty after rebuild: tile_viewport="
                  << render_viewport_.x << "," << render_viewport_.y << " "
                  << render_viewport_.width << "x" << render_viewport_.height
                  << " origin=" << origin_.x << "," << origin_.y
                  << " tilesize=" << tilesize_;
      }
    }

    // Update tilemap uniform per frame.
    GPUUpdateTilemapUniformInternal(params->context,
                                    render_offset_.Recast<float>(), tilesize_,
                                    anim_index_);
    ++g_tm_ground_ready;
  } else if (stage == DrawableNode::RenderStage::ON_RENDERING) {
    ++g_tm_ground_drawn;
    if (g_tm_ground_drawn % 300 == 0)
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[LayerAudit] ground ready=" << g_tm_ground_ready
                << " drawn=" << g_tm_ground_drawn
                << " empty=" << g_tm_ground_empty;
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
    // TEMPORARY audit. Marker: URGE_ABOVE_AUDIT
    //
    // The probe has shown that in a defect frame the pixels of the darkened region do not come out
    // of this pipeline at all (an unconditional paint over the whole layer does not land there),
    // while the ground layer's ready/drawn counters stay equal. Those counters are cumulative, so
    // they cannot see a single frame in which the above layers were drawn fewer times or not at
    // all. This counts the above draws per frame and prints only when that count differs from the
    // previous frame, so a steady frame stays silent and the log volume - which has changed the
    // timing of this defect before - is untouched until something actually happens.
    const int64_t audit_frame = context()->sprite_batcher->FrameCount();
    if (audit_frame != s_above_frame) {
      if (s_above_frame != 0 && s_above_this_frame != s_above_prev_frame)
        if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
        LOG(INFO) << "[AboveAudit] f=" << audit_frame
                  << " prev_frame_draws=" << s_above_prev_frame
                  << " last_frame_draws=" << s_above_this_frame;
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
  int32_t atlas_height = 28 * tilesize_;
  if (Disposable::IsValid(tileset_.get()))
    atlas_height = std::max(atlas_height, (**tileset_)->size.y);
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

  // Tileset part
  int32_t tileset_width = 0;
  if (Disposable::IsValid(tileset_.get())) {
    base::Vec2i tileset_size = (**tileset_)->size;
    base::Vec2i dst_pos(3 * tilesize_ * max_autotile_frame_count_, 0);
    tileset_width = tileset_size.x;

    commands.push_back({**tileset_, tileset_size, dst_pos});
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
    // TEMPORARY: read the previous value *before* it is overwritten - logging it afterwards printed
    // the new value twice and made the two look identical, which hid what actually moved.
    // Marker: URGE_UPLOAD_AUDIT
    const base::Rect previous_viewport = render_viewport_;
    render_viewport_ = new_viewport;
    map_buffer_dirty_ = true;
    g_tm_dirty_source = "viewport";  // TEMPORARY, see g_tm_dirty_source
    // TEMPORARY: the batch is re-uploaded every single frame because this test keeps firing, so
    // print what actually moved. Rate-limited: the first thirty occurrences, then every three
    // hundredth, which is enough to see whether the viewport is oscillating between two values
    // (a computation that flips) or drifting (a sub-tile offset that never settles).
    // Marker: URGE_UPLOAD_AUDIT
    static int32_t s_vp_logs = 0;
    ++s_vp_logs;
    if (s_vp_logs <= 30 || s_vp_logs % 300 == 0)
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[VpDirty] n=" << s_vp_logs << " new=" << new_viewport.x << ","
                << new_viewport.y << " " << new_viewport.width << "x"
                << new_viewport.height << " old=" << previous_viewport.x << ","
                << previous_viewport.y << " " << previous_viewport.width << "x"
                << previous_viewport.height << " origin=" << origin_.x << ","
                << origin_.y << " vp_origin=" << viewport_origin.x << ","
                << viewport_origin.y << " size=" << viewport_size.x << "x"
                << viewport_size.y;
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
    // TEMPORARY stable tag, see the order audit. Marker: URGE_ORDER_AUDIT
    above_node->SetDebugTag(0x1001 + i);
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
  g_tm_dirty_source = "map-data-modify";  // TEMPORARY, see g_tm_dirty_source
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

    copy_attribs.pSrcTexture = it.texture->data;
    copy_attribs.pSrcBox = &box;
    copy_attribs.SrcTextureTransitionMode =
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;

    const base::Rect real_src_rect =
        base::MakeIntersect(it.src_rect, it.texture->size);
    if (real_src_rect.width <= 0 || real_src_rect.height <= 0)
      continue;

    box.MinX = real_src_rect.x;
    box.MinY = real_src_rect.y;
    box.MaxX = real_src_rect.x + real_src_rect.width;
    box.MaxY = real_src_rect.y + real_src_rect.height;

    copy_attribs.pDstTexture = gpu_.atlas_texture;
    copy_attribs.DstX = it.dst_pos.x;
    copy_attribs.DstY = it.dst_pos.y;
    copy_attribs.DstTextureTransitionMode =
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;

    render_context->CopyTexture(copy_attribs);
  }

  // Candidate fix for the one-frame region loss, gated on urge_debug/atlasdrain.txt for
  // A/B testing. Everything above runs in the enqueue stage; the ground and the above
  // layers sample the atlas in the draw stage of the same frame. The comment at the top of
  // this function already worries about draws reading the atlas before the copies land -
  // the ground then shows unrelated, bright parts of the atlas for exactly one frame,
  // which is what the movement-only reproduction records. Draining here retires the copies
  // before anything samples them. Only dirty frames reach this point, so the sync cost is
  // paid when the map scrolls onto new tiles, not every frame.
  {
    static int32_t s_atlas_drain_poll = 0;
    static bool s_atlas_drain = false;
    if (++s_atlas_drain_poll >= 30) {
      s_atlas_drain_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "atlasdrain.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_atlas_drain = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/atlasdrain.txt");
      }
    }
    if (s_atlas_drain) {
      render_context->Flush();
      render_context->WaitForIdle();
    }
  }
}

// urge_debug/tmuniring.txt, nonzero for on. See GPUData::batch_sets: with the ring on this
// frame writes and reads its own batch and its own params buffer, so a draw can never read an
// upload that belongs to another frame. Read about twice a second from every user below.
// urge_debug/tmforce.txt, 0 = off, see the use in GPUUpdateTilemapUniformInternal. The
// one-frame ground loss draws an image that is byte-identical in every session, so the bad
// input that frame reads instead of the right one is a constant; forcing one candidate
// constant at a time turns the loss's image into the steady-state image, whose 256 block
// values can be compared with the recorded anomaly's.
static int32_t TilemapForcedInputInternal() {
  static int32_t s_force_poll = 0;
  static int32_t s_force = 0;
  if (++s_force_poll >= 30) {
    s_force_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "tmforce.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_force = value;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/tmforce.txt");
    }
  }
  return s_force;
}

// Whether the above layers draw through their own shader binding. On by default - it is the fix
// for the params slot holding the world matrix buffer for one frame (see GPUData::above_binding);
// urge_debug/tmsrbsplit.txt = 0 restores the shared binding, so the two can be compared inside
// one session.
static bool TilemapSrbSplitInternal() {
  static int32_t s_split_poll = 0;
  static bool s_split = true;
  if (++s_split_poll >= 30) {
    s_split_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "tmsrbsplit.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_split = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/tmsrbsplit.txt");
    }
  }
  return s_split;
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
  // TEMPORARY: frame counter for the shader's self-test. Marker: URGE_PARAM_READOUT
  //
  // The other self-test keyed on the flash alpha reaching a particular byte value. That only works
  // in a map whose darkening sweeps a range; where the alpha is constant it never fires, the probe
  // cannot prove its own path is live, and its negative results mean nothing. This counter is
  // value-free: the v1 shader paints the layer blue on two frames out of every 300, whatever the
  // parameters hold. The v1 shader never reads .w, so nothing else changes.
  uniform.AnimateIndexAndTileSizeAndFlashAlpha.w =
      static_cast<float>(context()->sprite_batcher->FrameCount() % 300);

  // See TilemapForcedInputInternal(). The value written here is what the ground shader reads;
  // forcing one candidate bad input at a time and comparing the resulting block values with the
  // recorded anomaly's says which one constant the loss is drawn from.
  switch (TilemapForcedInputInternal()) {
    case 1:  // next atlas animation row
      uniform.AnimateIndexAndTileSizeAndFlashAlpha.x += 16.0f;
      break;
    case 2:  // atlas texel size zeroed
      uniform.OffsetAndTexSize.z = 0.0f;
      uniform.OffsetAndTexSize.w = 0.0f;
      break;
    case 4:  // map scroll dropped
      uniform.OffsetAndTexSize.x = 0.0f;
      uniform.OffsetAndTexSize.y = 0.0f;
      break;
    case 5:  // tile size zeroed
      uniform.AnimateIndexAndTileSizeAndFlashAlpha.y = 0.0f;
      break;
    case 6:  // the whole offset-and-texel-size vector zeroed
      uniform.OffsetAndTexSize = base::Vec4(0.0f);
      break;
    case 7:  // every field zeroed - what a params buffer reads as before it is ever written
      uniform.OffsetAndTexSize = base::Vec4(0.0f);
      uniform.AnimateIndexAndTileSizeAndFlashAlpha = base::Vec4(0.0f);
      break;
    case 8:  // only the horizontal texel size collapsed
      uniform.OffsetAndTexSize.z = 0.0f;
      break;
    case 9:  // only the vertical texel size collapsed
      uniform.OffsetAndTexSize.w = 0.0f;
      break;
    default:
      break;
  }

  // Candidate fix, see GPUData::batch_sets: with the ring on this frame's parameters go into
  // this frame's own buffer, picked by the same frame set the quads were uploaded to.
  Diligent::IBuffer* uniform_target = gpu_.uniform_buffer;
  gpu_.uniform_in_use = uniform_target;

  // Temporary: one line per frame with the values the ground shader will read. A frame
  // that draws the ground with a stale atlas size or a wrong offset scrambles every
  // tile's UV and shows bright, unrelated atlas regions - the one-frame flicker. The buffers
  // logged are the ones this frame will really draw from, so a session can be split by the
  // ring afterwards without having to flip switches in step with it.
  if (context()->sprite_batcher->RunFpEnabled())
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[TmFp] f=" << context()->sprite_batcher->FrameCount()
              << " atlas=" << static_cast<const void*>(gpu_.atlas_texture.RawPtr())
              << " ast="
              << static_cast<int32_t>(
                     gpu_.atlas_texture ? gpu_.atlas_texture->GetState()
                                        : Diligent::RESOURCE_STATE_UNKNOWN)
              << " texsize=" << 1.0f / atlas_size.x << "," << 1.0f / atlas_size.y
              << " off=" << offset.x << "," << offset.y
              << " anim=" << anim_index << " tile=" << tilesize
              << " flash=" << flash_opacity_ << " tiles_n="
              << gpu_.ground_draw_count << " set="
              << context()->sprite_batcher->FrameSetIndex() << " vbuf="
              << static_cast<const void*>(
*gpu_.batch)
              << " ubuf=" << static_cast<const void*>(uniform_target);

  render_context->UpdateBuffer(
      uniform_target, 0, sizeof(uniform), &uniform,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void TilemapImpl::GPURenderGroundLayerInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::IBuffer* world_binding) {
  if (!gpu_.atlas_texture)
    return;

  // See TilemapForcedInputInternal(): mode 12 drops the ground draw. It answers which of the
  // two tilemap draws the floor on screen comes from - they share one params buffer, so a bad
  // read of it shows through either, and that decides where the fix belongs.
  if (TilemapForcedInputInternal() == 12)
    return;

  // Reproduction stress, gated on urge_debug/tmstress.txt (level = extra accesses per frame).
  // The one-frame ground loss is intermittent: the same build has measured 0.4 and 2.5 events
  // per minute in two nearby windows of one session, so a fix that halves it cannot be told
  // apart from that drift. What raised the plane's numbers was exactly this shape - extra GPU
  // accesses to the texture the draw samples, pure copies with no read-back and no waiting.
  // Same here, on the atlas the ground samples. It is an amplifier for comparing candidates
  // inside one session, not evidence about the mechanism.
  {
    static int32_t s_stress_poll = 0;
    static int32_t s_stress_level = 0;
    if (++s_stress_poll >= 30) {
      s_stress_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "tmstress.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_stress_level = value > 0 ? value : 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/tmstress.txt");
      }
    }

    if (s_stress_level > 0) {
      static RRefPtr<Diligent::ITexture> s_stress_tex;
      const auto& atlas_desc = gpu_.atlas_texture->GetDesc();
      if (!s_stress_tex ||
          s_stress_tex->GetDesc().Width != atlas_desc.Width ||
          s_stress_tex->GetDesc().Height != atlas_desc.Height)
        renderer::CreateTexture2D(
            **context()->render_device, &s_stress_tex, "tilemap.stress",
            base::Vec2i(static_cast<int32_t>(atlas_desc.Width),
                        static_cast<int32_t>(atlas_desc.Height)),
            Diligent::USAGE_DEFAULT, Diligent::BIND_SHADER_RESOURCE,
            Diligent::CPU_ACCESS_NONE, atlas_desc.Format);

      if (s_stress_tex) {
        for (int32_t i = 0; i < s_stress_level; ++i) {
          Diligent::CopyTextureAttribs attribs;
          attribs.pSrcTexture = gpu_.atlas_texture;
          attribs.SrcTextureTransitionMode =
              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
          attribs.pDstTexture = s_stress_tex;
          attribs.DstTextureTransitionMode =
              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
          render_context->CopyTexture(attribs);
        }
      }
    }
  }

  // Diagnostic bisection, gated on urge_debug/notilemap.txt. The one-frame region loss
  // covers the map view while the UI panels on other pipelines stay pixel-identical, so
  // the ground layer is a suspect. Skipping its draw (the tiles go black) and measuring
  // whether the map area still deviates once per frame tells whether the corruption is
  // inside this draw or behind it.
  {
    static int32_t s_no_tm_poll = 0;
    static bool s_no_tm = false;
    if (++s_no_tm_poll >= 30) {
      s_no_tm_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "notilemap.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_no_tm = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/notilemap.txt");
      }
    }
    if (s_no_tm)
      return;
  }

  if (gpu_.ground_draw_count) {
    auto* pipeline = context()->render.pipeline_states->tilemap.RawPtr();

    // Setup uniform params
    gpu_.shader_binding.u_transform->Set(world_binding);
    gpu_.shader_binding.u_texture->Set(gpu_.atlas_binding);
    // See TilemapForcedInputInternal(): mode 10 hands the world matrix buffer to the params
    // slot - what a draw whose params binding landed on the other buffer of its signature
    // reads. The matrix's leading floats are a zero texel size (the UV collapses to a constant)
    // and a zero flash (no tint lerp), which is what the recorded anomaly's image shows.
    Diligent::IBuffer* params_binding =
        gpu_.uniform_in_use ? gpu_.uniform_in_use : gpu_.uniform_buffer;
    if (TilemapForcedInputInternal() == 10)
      params_binding = world_binding;
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
    AuditBindingChangeInternal("ground", 0, params_binding, vertex_buffer,
                               pipeline);
    AuditScissorAtDrawInternal("ground");
    render_context->SetIndexBuffer(
        **context()->render.quad_index, 0,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Execute render command
    Diligent::DrawIndexedAttribs draw_indexed_attribs;
    draw_indexed_attribs.NumIndices = 6 * gpu_.ground_draw_count;
    draw_indexed_attribs.IndexType =
        context()->render.quad_index->GetIndexType();

    // See TilemapForcedInputInternal(): mode 3 reads the ground's quads from the tail of the
    // batch instead of its head, which is what a draw whose read landed one upload away sees.
    if (TilemapForcedInputInternal() == 3) {
      int32_t batch_quads = gpu_.ground_draw_count;
      for (int32_t count : gpu_.above_draw_count)
        batch_quads += count;
      if (batch_quads > gpu_.ground_draw_count)
        draw_indexed_attribs.FirstIndexLocation =
            6 * (batch_quads - gpu_.ground_draw_count);
    }

    {
      // See AuditTilemapBindingInternal. Marker: URGE_BIND_AUDIT
      TilemapBindingSample sample;
      sample.pipeline = pipeline;
      sample.srb = *gpu_.shader_binding;
      sample.vertex_buffer = vertex_buffer;
      sample.index_buffer = **context()->render.quad_index;
      sample.params_binding = params_binding;
      sample.num_indices = draw_indexed_attribs.NumIndices;
      sample.first_index = draw_indexed_attribs.FirstIndexLocation;
      AuditTilemapBindingInternal("ground", 0, sample);
    }
    render_context->DrawIndexed(draw_indexed_attribs);

  }
}

void TilemapImpl::GPURenderAboveLayerInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::IBuffer* world_binding,
    int32_t index) {
  if (!gpu_.atlas_texture)
    return;

  // See TilemapForcedInputInternal(): mode 13 drops the above layers.
  if (TilemapForcedInputInternal() == 13)
    return;

  if (!gpu_.above_draw_count.empty()) {
    if (int32_t draw_count = gpu_.above_draw_count[index]) {
      int32_t draw_offset = gpu_.ground_draw_count;
      for (int32_t i = 0; i < index; ++i)
        draw_offset += gpu_.above_draw_count[i];

      auto* pipeline = context()->render.pipeline_states->tilemap.RawPtr();

      // See GPUData::above_binding: this draw owns its binding, and only falls back to the
      // one the ground draws through when urge_debug/tmsrbsplit.txt = 0.
      renderer::Binding_Tilemap& layer_binding =
          TilemapSrbSplitInternal() ? gpu_.above_binding : gpu_.shader_binding;

      // Setup uniform params
      layer_binding.u_transform->Set(world_binding);
      layer_binding.u_texture->Set(gpu_.atlas_binding);
      // See TilemapForcedInputInternal(): mode 11 is mode 10 for this draw.
      Diligent::IBuffer* above_params_binding =
          gpu_.uniform_in_use ? gpu_.uniform_in_use : gpu_.uniform_buffer;
      if (TilemapForcedInputInternal() == 11)
        above_params_binding = world_binding;
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
      AuditBindingChangeInternal("above", index, above_params_binding,
                                 vertex_buffer, pipeline);
      AuditScissorAtDrawInternal("above");
      render_context->SetIndexBuffer(
          **context()->render.quad_index, 0,
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

      // Execute render command
      Diligent::DrawIndexedAttribs draw_indexed_attribs;
      draw_indexed_attribs.NumIndices = 6 * draw_count;
      draw_indexed_attribs.IndexType =
          context()->render.quad_index->GetIndexType();
      draw_indexed_attribs.FirstIndexLocation = draw_offset * 6;
      {
        // See AuditTilemapBindingInternal. Marker: URGE_BIND_AUDIT
        TilemapBindingSample sample;
        sample.pipeline = pipeline;
        sample.srb = &layer_binding;
        sample.vertex_buffer = vertex_buffer;
        sample.index_buffer = **context()->render.quad_index;
        sample.params_binding = above_params_binding;
        sample.num_indices = draw_indexed_attribs.NumIndices;
        sample.first_index = draw_indexed_attribs.FirstIndexLocation;
        AuditTilemapBindingInternal("above", index + 1, sample);
      }
      render_context->DrawIndexed(draw_indexed_attribs);


    }
  }
}

}  // namespace content
