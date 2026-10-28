// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDER_SPRITE_BATCH_H_
#define CONTENT_RENDER_SPRITE_BATCH_H_

#include <vector>

#include "content/canvas/canvas_impl.h"
#include "content/render/pipeline_collection.h"
#include "renderer/device/render_device.h"
#include "renderer/pipeline/render_binding.h"

namespace content {

// Upload target for the sprite parameters (see SubmitBatchDataAndResetCache).
// A DISCARD-mapped dynamic buffer makes everything written before the newest Map
// undefined, and the sprite batches are written more than once per frame (every
// render pass uploads the accumulated array). A pending draw of an earlier pass -
// a viewport inside the screen pass - then reads undefined data, which shows up as
// the picture flickering in game, while the single-pass title screen is stable.
// A USAGE_DEFAULT buffer written with UpdateBuffer keeps every write as its own
// copy in the command stream, so each draw keeps reading what it was recorded
// against.
using SpriteBatchStaticBuffer =
    renderer::BatchBuffer<renderer::Binding_Sprite::Params,
                          Diligent::BIND_SHADER_RESOURCE,
                          Diligent::BUFFER_MODE_STRUCTURED,
                          Diligent::CPU_ACCESS_NONE,
                          Diligent::USAGE_DEFAULT>;

class SpriteBatch {
 public:
  SpriteBatch(renderer::RenderDevice* device, renderer::PipelineSet* loader);
  ~SpriteBatch();

  SpriteBatch(const SpriteBatch&) = delete;
  SpriteBatch& operator=(const SpriteBatch&) = delete;

  using TextureID = void*;
  TextureID GetCurrentTexture() const { return current_texture_; }

  // Shader resource view the run being filled binds. A paged bitmap (taller
  // than one GPU texture) has one view per page, so the view - not just the
  // bitmap - is what tells two runs apart.
  Diligent::ITextureView* GetCurrentSRV() const { return current_srv_; }

  // One set of upload buffers is used per frame and they rotate, so the data a
  // frame still being executed reads is never written by the next frame: the GPU
  // runs one or two frames behind the CPU, and a single shared buffer made an
  // occasionally stale picture show up on screen (mixed parameter data).
  // The box is slow enough (and this build carries the renderer's development
  // validation) for the CPU to run several frames ahead of the GPU, so the sets
  // are plentiful: with too few, an early frame still being executed would read
  // the parameters and vertices of a later one - corners of the picture then miss
  // or show the wrong content for a single frame.
  static constexpr int32_t kFrameSets = 8;

  renderer::Binding_Sprite& GetShaderBinding() { return binding_; }
  Diligent::IBuffer* GetVertexBuffer() { return *vertex_batch_[frame_set_]; }
  Diligent::IBufferView* GetUniformBinding() { return uniform_binding_; }

  // Setup a sprite batch
  void BeginBatch(TextureID texture, Diligent::ITextureView* srv);

  // World transform the pending batch belongs to. A batch is drawn with a single
  // transform, so the queue must be broken when a node of another render level
  // (screen vs nested viewport) reaches it - SpriteImpl does that.
  void SetBatchWorld(Diligent::IBuffer* world) { batch_world_ = world; }
  Diligent::IBuffer* GetBatchWorld() const { return batch_world_; }

  // Blend mode the queued sprites were built with; a batch only ever contains one
  // blend mode, so the end-of-pass flush can pick the right pipeline for it.
  void SetBatchBlend(int32_t blend) { batch_blend_ = blend; }
  int32_t GetBatchBlend() const { return batch_blend_; }

  void PushSprite(const renderer::Quad& quad,
                  const renderer::Binding_Sprite::Params& uniform);

  void EndBatch(uint32_t* instance_offset, uint32_t* instance_count);

  // Summit pending batch data to rendering queue
  void SubmitBatchDataAndResetCache(renderer::QuadIndexCache* quad_cache,
                                    Diligent::IDeviceContext* render_context);

  // Emits every batch queued during the current render pass, using each batch's own
  // texture and blend mode and the pass's world transform, then empties the pass.
  //
  // Draws used to be issued by whichever node happened to close a batch, which made
  // a bitmap depend on that node still being visited during ON_RENDERING: the
  // drawable list is re-linked every frame (the tilemap rebuilds its above layers,
  // menus and battles create and destroy bitmaps), so a node unlinked between the
  // enqueue and the render pass silently dropped its bitmaps for that frame. The
  // batcher now owns the emission, and a pass is therefore drawn completely and
  // deterministically.
  void FlushPass(Diligent::IDeviceContext* context,
                 renderer::QuadIndexCache* quad_index,
                 PipelineCollection* pipelines,
                 const base::Rect& scissor,
                 Diligent::IBuffer* world);

  // Marks the run a node just drew as emitted. Nodes normally draw the runs they
  // close themselves (that keeps the scissor rect the node set); FlushPass only
  // picks up what no node got to draw.
  void MarkEmitted(uint32_t start, uint32_t count);

  // Which of the rotating frame sets this frame writes into. The window layers queue
  // their vertices into their own single buffer today, which a frame still in flight can
  // be reading while the next one overwrites it - the sprite batch avoids exactly that
  // with its sets, and the window path needs the same protection. Exposed so both paths
  // rotate together.
  int32_t FrameSetIndex() const {
    return static_cast<int32_t>(audit_frame_ % kFrameSets);
  }

  // Raw frame counter, so diagnostics in other modules can tag their lines with the same
  // frame number and their order within one frame becomes unambiguous.
  int64_t FrameCount() const { return audit_frame_; }

  // Rotates the per-frame buffer sets. Called once per presented frame, at the present
  // boundary: the sets exist to keep the CPU off buffers the GPU may still be executing
  // from, and the GPU is only done with a set once that frame has reached the screen.
  // Rotating per render call instead split a frame that renders the scene twice (screen
  // plus snapshot) across two sets - so a layer that latched the set
  // when it queued its vertices drew from the other one, putting an element of that frame
  // on screen from data it never wrote: the one-frame vanishing.
  void AdvanceFrameSet();

  // Drops everything accumulated so far. Called once per render sequence: on
  // Android TV the caches are deliberately kept across the submissions of one
  // sequence (see SubmitBatchDataAndResetCache), so the reset has to happen
  // here instead. The set rotation is not here, see AdvanceFrameSet.
  void ResetFrameCache();

  bool IsBatchEnabled() const { return support_storage_buffer_batch_; }

 private:
  // One contiguous run of sprites sharing a texture and a blend mode; the unit the
  // pass is finally drawn in.
  struct BatchRun {
    TextureID texture = TextureID();
    // View the run samples: page 0 for a normal bitmap, the page itself for a
    // paged one. Null falls back to the bitmap's first view.
    Diligent::ITextureView* srv = nullptr;
    // World transform this run was queued under: a run never spans render levels,
    // so the fallback draw can use the run's own transform instead of guessing.
    Diligent::IBuffer* world = nullptr;
    uint32_t start = 0;
    uint32_t count = 0;
    int32_t blend = 0;
    bool emitted = false;
  };

  // Moves the run currently being filled into pass_runs_.
  void CloseCurrentRunInternal();

  // Runs queued during the current frame. A run belongs to a render level through
  // its world transform, which is also what FlushPass filters on.
  std::vector<BatchRun> pass_runs_;

  // Temporary diagnostics, reported by ResetFrameCache: quads queued in the frame
  // versus quads a node really drew, plus the runs only the fallback had to emit and
  // the runs whose texture carried no shader resource view. A frame where these do
  // not line up is a frame that loses bitmaps.
  uint32_t audit_frame_ = 0;

  TextureID current_texture_;
  Diligent::ITextureView* current_srv_ = nullptr;
  int32_t last_batch_index_;
  const bool support_storage_buffer_batch_;

  std::vector<renderer::Quad> quad_cache_;
  std::vector<renderer::Binding_Sprite::Params> uniform_cache_;

  renderer::Binding_Sprite binding_;
  renderer::QuadBatch vertex_batch_[kFrameSets];
  SpriteBatchStaticBuffer uniform_upload_[kFrameSets];
  RRefPtr<Diligent::IBufferView> uniform_binding_;

  renderer::RenderDevice* device_ = nullptr;
  Diligent::IBuffer* batch_world_ = nullptr;
  int32_t frame_set_ = 0;
  int32_t batch_blend_ = 0;
};

}  // namespace content

#endif  //! CONTENT_RENDER_SPRITE_BATCH_H_
