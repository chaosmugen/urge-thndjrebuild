// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/render/sprite_batch.h"

#include <cstdio>
#include <deque>
#include <utility>

#include "renderer/device/device_mode.h"
#include "renderer/device/gpu_driver.h"
// Defined in content/screen/renderscreen_impl.cc. Marker: URGE_DIAG_GATE
extern "C" bool UrgeDiagEnabled();

// Fix for the one-frame loss on the sprite layer, same shape as the map's. Marker: URGE_SPRREDRAW
//
// The UI strips along the bottom and the right of the screen still lose part of themselves for a
// single frame after the map was repaired, and those are drawn from this batcher. The map's repair
// - draw the layer again in a pass of its own, so a lost write in one pass is covered by the other -
// applies here the same way, and the two draws are adjacent, so the frame's order does not change.
// Default: off. It was tried on for Mali GPUs and had to be withdrawn - a second draw re-blends
// translucent content so the picture can change, and the loss was still reported on the device.
// The switch file (sprredraw.txt) turns it on for a session.
extern "C" void UrgeForcePassBoundary(Diligent::IDeviceContext* render_context);
// TEMPORARY A/B. Marker: URGE_SPRBREAK
//
// The sprite redraw does two separate things, and only one of them is wanted here:
//   * the second draw over the same quads, which is what puts back a run whose writes were lost;
//   * a forced pass boundary before it, which is global - it lands between whatever draws come
//     next, including the two stacked window panels, and re-composites their overlapping
//     translucent edges a second time (measured: seam relative depth 0.210 instead of 0.098,
//     against 0.105 on the reference device).
// This switch keeps the second draw and drops the boundary. sprbreak.txt = 0 selects it.
extern "C" bool UrgeSpriteRedrawBreakEnabled();

extern "C" bool UrgeSpriteRedrawEnabled() {
  static int32_t s_poll = 0;
    // Uncertain fix, off by default: a second draw re-blends translucent content, so the
  // picture can change, and the loss was still reported on the device. Reachable via its switch.
  // Marker: URGE_DEFAULTS
  // Off, as its own note above concludes: repeating a translucent overlay re-blends it, so the
  // picture changes - measured on the device as the seam between two stacked panels alternating
  // between a value that matches the reference platform (relative depth 0.098) and one that is
  // exactly twice as deep (0.210), in 8 of 12 frames with every CPU-side input identical. The
  // loss it was meant to cover was still reported with it on, so nothing is given up by leaving
  // it off. sprredraw.txt = 1 turns it on for a session. Marker: URGE_DEFAULTS
  static bool s_enabled = false;
  if (++s_poll >= 30) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "sprredraw.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_enabled = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/sprredraw.txt");
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[SprRedraw] enabled=" << s_enabled;
    }
  }
  return s_enabled;
}

extern "C" bool UrgeSpriteRedrawBreakEnabled() {
  static int32_t s_poll = 0;
  static bool s_enabled = true;
  if (++s_poll >= 30) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "sprbreak.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_enabled = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/sprbreak.txt");
    }
  }
  return s_enabled;
}

namespace content {

///////////////////////////////////////////////////////////////////////////////
// SpriteBatch Implement

namespace {

// TEMPORARY A/B switch. With the storage-buffer batch on, every sprite of a run reads
// its parameters from one shared array, and the element index has to be carried in the
// vertex colour because the Mali driver on the target TV hands the same vertex index to
// every draw of a frame (see kHLSL_SpriteRender_Vertex). If that index resolves wrongly
// for a frame, the run's elements are all drawn with somebody else's parameters - which
// looks exactly like a region vanishing for one frame while every CPU-side counter stays
// correct. Setting urge_debug/nobatch.txt to 1 forces the fallback path, where each
// sprite is drawn with its own constant parameter block and that mechanism cannot apply.
bool BatchingDisabledByProbe() {
  FILE* file = fopen(
      "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/nobatch.txt",
      "r");
  if (!file)
    return false;
  int32_t value = 0;
  const bool read = fscanf(file, "%d", &value) == 1;
  fclose(file);
  return read && value != 0;
}

}  // namespace

SpriteBatch::SpriteBatch(renderer::RenderDevice* device,
                         renderer::PipelineSet* loader)
    : current_texture_(TextureID()),
      last_batch_index_(-1),
      support_storage_buffer_batch_(loader->sprite.storage_buffer_support &&
                                    !BatchingDisabledByProbe()),
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
  ++audit_runs_;
}

void SpriteBatch::MarkEmitted(uint32_t start, uint32_t count) {
  if (!support_storage_buffer_batch_ || !count)
    return;

  for (auto it = pass_runs_.rbegin(); it != pass_runs_.rend(); ++it) {
    if (it->start == start && it->count == count && !it->emitted) {
      it->emitted = true;
      audit_drawn_ += count;
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
  if (run_fp_enabled_)
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[QuadFp] f=" << audit_frame_ << " i=" << uniform_cache_.size()
              << " col=" << uniform.Color.x << "," << uniform.Color.y << ","
              << uniform.Color.z << "," << uniform.Color.w << " tone="
              << uniform.Tone.x << "," << uniform.Tone.y << "," << uniform.Tone.z
              << "," << uniform.Tone.w << " op=" << uniform.Opacity.x << " pos="
              << uniform.Position.x << "," << uniform.Position.y << " org="
              << uniform.Origin.x << "," << uniform.Origin.y << " sc="
              << uniform.Scale.x << "," << uniform.Scale.y << " rot="
              << uniform.Rotation.x;

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

    audit_drawn_ += run.count;
    ++audit_fallback_runs_;
    if (!texture->shader_resource_view)
      ++audit_null_texture_;

    // Same fingerprint as the node path (see SpriteImpl::GPUOnSpriteRenderingInternal).
    if (run_fp_enabled_)
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[RunFp] f=" << audit_frame_ << " src=fallback st=" << run.start
                << " n=" << run.count << " bl=" << run.blend
                << " t=" << static_cast<const void*>(texture)
                << " name=" << texture->name << " size=" << texture->size.x << "x"
                << texture->size.y << " srv="
                << static_cast<const void*>(texture->shader_resource_view.RawPtr())
                << " p=" << static_cast<const void*>(uniform_binding_)
                // See the note on the node path's identical line in sprite_impl.cc.
                << " w=" << static_cast<const void*>(world);

    binding_.u_transform->Set(world);
    binding_.u_texture->Set(texture->shader_resource_view);

    // Keep-alive ring for the texture this draw samples, switched by the auto A/B arm. The
    // game replaces a window's contents bitmap while the window keeps drawing, and the
    // bitmap it replaces is destroyed in the same frame. The flicker is one frame in which
    // a region is drawn from the wrong texels, while every CPU-side value - the window's
    // state, its quad counts, the draw's parameters, even the contents of the bitmap it
    // samples, read back across such a frame - matches the frames around it. A texture that
    // stops existing while a draw recorded in an earlier frame is still sampling it, and
    // whose memory is then handed to the next texture, would look exactly like that. Holding
    // a reference to the view each draw uses for a few frames keeps the resource out of the
    // allocator's reach until those draws have certainly completed.
    // Arms, 1000 frames each: 0 none, 1 eight frames, 2 thirty-two frames, 3 none (drift
    // check - the event rate of the same build has varied by more than 10x between runs, so
    // a second baseline arm is worth its quarter of the session).
    {
      static std::deque<std::pair<uint32_t, RRefPtr<Diligent::ITextureView>>> s_alive;
      static int32_t s_alive_poll = 0;
      static bool s_alive_auto = false;
      static int32_t s_alive_depth = 0;
      if (++s_alive_poll >= 30) {
        s_alive_poll = 0;
        FILE* file = fopen(
            "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
            "keepalive.txt",
            "r");
        if (file) {
          int32_t value = 0;
          if (fscanf(file, "%d", &value) == 1)
            s_alive_depth = value;
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
            s_alive_auto = value != 0;
          fclose(auto_file);
          remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
                 "urge_debug/autosync.txt");
        }
      }
      const uint32_t alive_arm = (audit_frame_ / 1000) % 4;
      const int32_t depth =
          s_alive_auto ? (alive_arm == 1 ? 8 : (alive_arm == 2 ? 32 : 0))
                       : s_alive_depth;
      if (depth > 0) {
        s_alive.push_back({audit_frame_, texture->shader_resource_view});
        const uint32_t oldest =
            audit_frame_ > static_cast<uint32_t>(depth)
                ? audit_frame_ - static_cast<uint32_t>(depth)
                : 0;
        while (!s_alive.empty() && s_alive.front().first < oldest)
          s_alive.pop_front();
      } else if (!s_alive.empty()) {
        s_alive.clear();
      }
    }

    binding_.u_params->Set(uniform_binding_);
    context->SetPipelineState(pipelines->sprite[run.blend].RawPtr());
    context->CommitShaderResources(
        *binding_, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    Diligent::DrawIndexedAttribs draw;
    draw.NumIndices = 6 * run.count;
    draw.IndexType = quad_index->GetIndexType();
    draw.FirstIndexLocation = 6 * run.start;
    context->DrawIndexed(draw);

    // See UrgeSpriteRedrawEnabled. Marker: URGE_SPRREDRAW
    if (UrgeSpriteRedrawEnabled()) {
      if (UrgeSpriteRedrawBreakEnabled())
        UrgeForcePassBoundary(context);
      context->DrawIndexed(draw);
    }
  }

  // TEMPORARY A/B. Marker: URGE_SPRPASS
  //
  // Measured on the device, with a camera on the panel: one decoration (the compass) drops out for
  // exactly one frame now and then. Repeating its draw inside the same pass does not help; ending
  // and reopening the pass right after it does. That is the only thing that has changed the
  // outcome, so this issues exactly one boundary per sprite flush - after everything has been
  // emitted, drawing nothing in between. It re-blends nothing, so it cannot affect the seam.
  // urge_debug/sprpass.txt = 1 enables it.
  {
    static int32_t s_sp_pass_poll = 0;
    static bool s_sp_pass_enabled = false;
    if (++s_sp_pass_poll >= 30) {
      s_sp_pass_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "sprpass.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_sp_pass_enabled = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/sprpass.txt");
      }
    }
    if (s_sp_pass_enabled && context)
      UrgeForcePassBoundary(context);
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

  // Temporary: read the per-run fingerprint switch (urge_debug/runfp.txt, 1 = on) about
  // twice a second, so the probe only writes while a recording window is open.
  if (++run_fp_poll_ >= 30) {
    run_fp_poll_ = 0;
    FILE* fp = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/runfp.txt",
        "r");
    if (fp) {
      int32_t value = -1;
      if (fscanf(fp, "%d", &value) == 1)
        run_fp_enabled_ = value != 0;
      fclose(fp);
      if (run_fp_enabled_)
        if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
        LOG(INFO) << "[RunFp] enabled";
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/runfp.txt");
    }
  }

  // Report the frame that just ended (this runs at the start of a sequence, while
  // the previous one's caches are still intact). A quiet log means every queued quad
  // reached the GPU through its own node; anything else is a frame that loses
  // bitmaps and says which way it was lost.
  const uint32_t pushed = static_cast<uint32_t>(uniform_cache_.size());
  const bool anomaly = pushed != audit_drawn_ || audit_fallback_runs_ ||
                       audit_null_texture_ || last_batch_index_ >= 0;
  // audit_frame_ advances in AdvanceFrameSet, once per presented frame, so the count
  // reported here is the frame the sequence that just ended belonged to.
  ++render_ticks_;
  if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
  LOG(INFO) << "[BatchAudit] f=" << audit_frame_ << " set=" << frame_set_
            << " idx=" << FrameSetIndex() << " sprites="
            << audit_sprites_ << " pushed=" << pushed
            << " drawn=" << audit_drawn_ << " runs=" << audit_runs_
            << " fallback_runs=" << audit_fallback_runs_
            << " null_texture=" << audit_null_texture_
            << " open_run_at_end=" << (last_batch_index_ >= 0)
            << (anomaly ? "  <== ANOMALY" : "");
  audit_runs_ = 0;
  audit_sprites_ = 0;
  audit_drawn_ = 0;
  audit_fallback_runs_ = 0;
  audit_null_texture_ = 0;

  quad_cache_.clear();
  uniform_cache_.clear();
  pass_runs_.clear();
  current_texture_ = TextureID();
  last_batch_index_ = -1;
}

}  // namespace content
