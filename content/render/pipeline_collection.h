// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDER_PIPELINE_COLLECTION_H_
#define CONTENT_RENDER_PIPELINE_COLLECTION_H_

#include <array>

#include "renderer/pipeline/render_pipeline.h"

namespace content {

/// Color Blend Type of Pipeline
///
enum BlendType {
  BLEND_TYPE_NORMAL = 0,
  BLEND_TYPE_ADDITION,
  BLEND_TYPE_SUBSTRACTION,
  BLEND_TYPE_MULTIPLY,
  BLEND_TYPE_KEEP_ALPHA,
  BLEND_TYPE_NO_BLEND,
  BLEND_TYPE_NUMS,
};

struct PipelineCollection {
  using PipelineObject = RRefPtr<Diligent::IPipelineState>;

  struct {
    std::array<PipelineObject, BLEND_TYPE_NUMS> base;
    PipelineObject color;
    PipelineObject bitmapblt;
    PipelineObject bitmapclip;
    PipelineObject bitmaphue;
  } bitmap;

  std::array<PipelineObject, BLEND_TYPE_NUMS> plane;
  std::array<PipelineObject, BLEND_TYPE_NUMS> sprite;

  // TEMPORARY A/B. Marker: URGE_SPRNODEPTH
  //
  // The remaining flicker - reproduced by opening the item menu and switching left and right quickly
  // - makes one whole panel of the system frame disappear for a single frame, revealing what is
  // behind it: the light-blue menu column is replaced by the gold frame art, the bottom strip by the
  // scene. A whole element vanishing looks like the element being rejected rather than its writes
  // going missing, and the sprite pipeline is the only one in the engine that keeps the depth test
  // on (its per-sprite sort key rides in the vertex z, and the note in pipeline_collection.cc records
  // that switching the test off once made a transition flash for a frame). This is the same pipelines
  // built with the test off, so the two can be compared on the device in one session.
  std::array<PipelineObject, BLEND_TYPE_NUMS> sprite_nodepth;

  PipelineObject viewport_flat;
  PipelineObject brightness;

  PipelineObject tilemap;
  // Tilemap pipeline with blending off, for the one-frame-loss A/B. Marker: URGE_NOBLEND
  // See renderer::PipelineSet::tilemap_noblend for why this exists.
  PipelineObject tilemap_noblend;
  PipelineObject tilemap2;

  PipelineObject alpha_transition;
  PipelineObject vague_transition;

  PipelineObject yuv;

  PipelineObject window;
  struct {
    PipelineObject viewport;
    PipelineObject viewport_alpha;
    PipelineObject base;
  } window2;

  PipelineCollection(renderer::PipelineSet* loader);
};

}  // namespace content

#endif  //! CONTENT_RENDER_PIPELINE_COLLECTION_H_
