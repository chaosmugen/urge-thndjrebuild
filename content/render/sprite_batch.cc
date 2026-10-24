// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>

#include "content/render/sprite_batch.h"

#include <cstdio>
#include <deque>
#include <utility>

#include "renderer/device/device_mode.h"
#include "renderer/device/gpu_driver.h"

// Fix for the one-frame loss on the sprite layer, same shape as the map's.
//
// Marker: URGE_SPRITEREDRAW
//
// Redraw every sprite run in a pass of its own: the repair for the one-frame region loss. Like the
// tilemap second pass and the window redraw above, this is a workaround measured on the Mali TV the
// project targets - a run that one pass loses is covered by the redraw, which is only ever needed
// where a pass can lose writes at all. No switch: the file polling this used to carry cost a dozen
// failed fopens a second for a knob that has a single useful value.
extern "C" bool SpriteRedrawEnabledInternal() {
  return renderer::IsMaliGpu();
}

namespace content {

///////////////////////////////////////////////////////////////////////////////
// SpriteBatch Implement

namespace {

}  // namespace

SpriteBatch::SpriteBatch(renderer::RenderDevice* device,
                         renderer::PipelineSet* loader)
    : current_texture_(TextureID()),
      last_batch_index_(-1),
      support_storage_buffer_batch_(loader->sprite.storage_buffer_support &&
                                    true),
      binding_(loader->sprite.CreateBinding()),
      device_(device) {
  for (auto& batch : vertex_batch_)
    batch = renderer::QuadBatch::Make(**device);
}

SpriteBatch::~SpriteBatch() = default;

void SpriteBatch::BeginBatch(TextureID texture) {
  // A new run starts: the previous one is complete and joins this pass.
  CloseCurrentRunInternal();
  current_texture_ = texture;
  last_batch_index_ = uniform_cache_.size();
}

void SpriteBatch::CloseCurrentRunInternal() {
  if (!support_storage_buffer_batch_ || current_texture_ == TextureID() ||
      last_batch_index_ < 0)
    return;

  const uint32_t start = static_cast<uint32_t>(last_batch_index_);
  const uint32_t count = static_cast<uint32_t>(uniform_cache_.size()) - start;
  if (!count)
    return;

  BatchRun run;
  run.texture = current_texture_;
  run.world = batch_world_;
  run.start = start;
  run.count = count;
  run.blend = batch_blend_;
  pass_runs_.push_back(run);
}

void SpriteBatch::MarkEmitted(uint32_t start, uint32_t count) {
  if (!support_storage_buffer_batch_ || !count)
    return;

  for (auto it = pass_runs_.rbegin(); it != pass_runs_.rend(); ++it) {
    if (it->start == start && it->count == count && !it->emitted) {
      it->emitted = true;
      return;
    }
  }
}

void SpriteBatch::PushSprite(const renderer::Quad& quad,
                             const renderer::Binding_Sprite::Params& uniform) {
  // The batched shader has to know which element of the parameter array belongs to
  // which quad. The natural carrier would be gl_VertexIndex, but the Mali driver on
  // the target TV does not deliver it as the index value: every draw of a frame
  // ends up resolving one and the same element (the batch's first), so all sprites
  // are drawn with somebody else's position, origin and scale. The element index is
  // therefore stamped into the vertex colour, which the sprite shader never reads
  // (it takes its colour from u_Params). See kHLSL_SpriteRender_Vertex.
  // Temporary: the parameters this quad will read back by its stamped index. A frame
  // that draws one element with another frame's colour or opacity (an overlay at full
  // strength, say) shows up here as a line that differs from its neighbours'.

  renderer::Quad stamped = quad;
  renderer::Quad::SetColor(
      &stamped,
      base::Vec4(static_cast<float>(uniform_cache_.size()), 0.0f, 0.0f, 1.0f));
  quad_cache_.push_back(stamped);
  uniform_cache_.push_back(uniform);
}

void SpriteBatch::EndBatch(uint32_t* instance_offset,
                           uint32_t* instance_count) {
  // Record the run this node just closed. The node draws it right after (and then
  // marks it emitted); if the node does not survive into the draw pass - the
  // drawable list is re-linked while a frame runs - FlushPass emits it at the end
  // of the pass instead of losing the bitmaps for that frame.
  CloseCurrentRunInternal();

  const int32_t draw_count = uniform_cache_.size() - last_batch_index_;

  *instance_offset = last_batch_index_;
  *instance_count = draw_count;

  current_texture_ = TextureID();
  last_batch_index_ = -1;
}

void SpriteBatch::SubmitBatchDataAndResetCache(
    renderer::QuadIndexCache* quad_cache,
    Diligent::IDeviceContext* render_context) {
  // Close whatever run is still open. A run must never span render levels: when a
  // nested viewport reaches this point, the outer level's unfinished run would
  // otherwise be extended with sprites drawn in another transform and target.
  CloseCurrentRunInternal();

  // Setup index buffer. The indices are generated with frame-global numbering
  // (quad i -> 4i + {0,1,2,2,3,0}) which is what the batched sprite shader needs:
  // it reads its per-sprite constants as u_Params[vertex_id / 4], i.e. it treats
  // the vertex index as an index into this array.
  quad_cache->Allocate(uniform_cache_.size());

  // Upload data and rebuild binding
  if (quad_cache_.size())
    vertex_batch_[frame_set_].QueueWrite(render_context, quad_cache_.data(),
                                         quad_cache_.size());

  if (uniform_cache_.size() && support_storage_buffer_batch_) {
    // Written with UpdateBuffer into a USAGE_DEFAULT buffer: every write becomes
    // its own copy in the command stream, so a draw recorded earlier in the frame
    // keeps reading the data it was recorded against. A DISCARD-mapped dynamic
    // buffer declares everything written before the newest Map as undefined, which
    // is what made the in-game picture flicker - a frame there contains several
    // passes (each viewport submits inside the screen pass), a frame on the title
    // screen only one.
    const size_t upload_bytes =
        uniform_cache_.size() * sizeof(renderer::Binding_Sprite::Params);
    SpriteBatchStaticBuffer& upload = uniform_upload_[frame_set_];
    if (!upload() || (*upload)->GetDesc().Size < upload_bytes) {
      upload = SpriteBatchStaticBuffer::Make(**device_, uniform_cache_.size());
    }
    render_context->UpdateBuffer(
        *upload, 0, static_cast<uint32_t>(upload_bytes),
        uniform_cache_.data(),
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    uniform_binding_ =
        (*upload)->GetDefaultView(Diligent::BUFFER_VIEW_SHADER_RESOURCE);
  }

  // Android TV: the caches stay alive for the whole render sequence.
  //
  // This function runs once per render pass, and a frame contains several of them
  // (every viewport submits, then the screen submits again); the viewport passes
  // run *inside* the screen's BEFORE_RENDER broadcast, i.e. before the screen
  // level has been uploaded and drawn. Clearing the caches here restarted the
  // quad numbering at 0 in the middle of the sequence, while the draws of the
  // outer pass were still queued against the buffer the nested pass had just
  // written at those same indices - every sprite then read another sprite's
  // position, origin and scale. GLES never showed this because it keeps those
  // values in a per-node constant buffer instead of the shared batched array.
  // Keeping the caches for the whole sequence makes both passes reference the
  // same, append-only layout (index == position in the arrays), so a later
  // upload cannot disturb an earlier draw's data. ResetFrameCache() clears them
  // at the start of each render sequence.
  // Only the per-sprite path restarts its arrays every pass: the batched path keeps
  // them for the whole frame so that a pending batch of an earlier pass cannot be
  // rewritten by a later one (indices and parameter slots stay stable).
  if (!support_storage_buffer_batch_) {
    quad_cache_.clear();
    uniform_cache_.clear();
  }
}

void SpriteBatch::FlushPass(Diligent::IDeviceContext* context,
                            renderer::QuadIndexCache* quad_index,
                            PipelineCollection* pipelines,
                            const base::Rect& scissor,
                            Diligent::IBuffer* world) {
  if (!support_storage_buffer_batch_)
    return;

  // A run still open when the pass closes has no node left to draw it.
  CloseCurrentRunInternal();
  current_texture_ = TextureID();
  last_batch_index_ = -1;

  if (pass_runs_.empty() || !quad_index || !pipelines)
    return;

  // Only this level's runs: a run's world transform is what identifies the level it
  // was queued for, and an outer level's runs belong to that level's own pass (its
  // draw stage has not run yet at this point, so they are not leftover).
  bool has_leftover = false;
  for (const BatchRun& run : pass_runs_) {
    if (!run.emitted && run.world == world) {
      has_leftover = true;
      break;
    }
  }
  if (!has_leftover)
    return;

  // Those runs are here because the node that closed them was gone by the time the
  // draws were issued - the drawable list is re-linked while a frame runs - so emit
  // them now instead of losing their bitmaps for this frame. Their data was uploaded
  // when this pass was set up (SubmitBatchDataAndResetCache). The clip rect is the
  // one the pass started with: the finer scissor a node would have set for its own
  // draw does not exist here any more.
  Diligent::Rect render_scissor(scissor.x, scissor.y,
                                scissor.x + scissor.width,
                                scissor.y + scissor.height);
  context->SetScissorRects(1, &render_scissor, UINT32_MAX, UINT32_MAX);

  Diligent::IBuffer* vertex_buffer = *vertex_batch_[frame_set_];
  context->SetVertexBuffers(0, 1, &vertex_buffer, nullptr,
                            Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  context->SetIndexBuffer(**quad_index, 0,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  for (BatchRun& run : pass_runs_) {
    if (run.emitted || run.world != world)
      continue;
    run.emitted = true;

    auto* texture = static_cast<content::BitmapTexture*>(run.texture);
    if (!texture || !run.count)
      continue;


    // Same fingerprint as the node path (see SpriteImpl::GPUOnSpriteRenderingInternal).

    binding_.u_transform->Set(world);
    binding_.u_texture->Set(texture->shader_resource_view);


    binding_.u_params->Set(uniform_binding_);
    context->SetPipelineState(pipelines->sprite[run.blend].RawPtr());
    context->CommitShaderResources(
        *binding_, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    Diligent::DrawIndexedAttribs draw;
    draw.NumIndices = 6 * run.count;
    draw.IndexType = quad_index->GetIndexType();
    draw.FirstIndexLocation = 6 * run.start;
    context->DrawIndexed(draw);

  }

}

void SpriteBatch::AdvanceFrameSet() {
  // One rotation per presented frame. Every render of that frame - and every node that
  // writes during any of them - then works in the same set, which is what a node that
  // latches the set while queueing its vertices and draws with it later depends on.
  frame_set_ = (frame_set_ + 1) % kFrameSets;
  ++audit_frame_;
}

void SpriteBatch::ResetFrameCache() {

  // audit_frame_ advances in AdvanceFrameSet, once per presented frame, so the count
  // reported here is the frame the sequence that just ended belonged to.

  quad_cache_.clear();
  uniform_cache_.clear();
  pass_runs_.clear();
  current_texture_ = TextureID();
  last_batch_index_ = -1;
}

}  // namespace content
