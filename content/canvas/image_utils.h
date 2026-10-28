// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_CANVAS_IMAGE_UTILS_H_
#define CONTENT_CANVAS_IMAGE_UTILS_H_

#include "SDL3/SDL_surface.h"

namespace content {

// aka RGBA32
constexpr SDL_PixelFormat kSurfaceInternalFormat = SDL_PIXELFORMAT_ABGR8888;

// Bitmaps at or above this pixel count (about 1024x1024) can drop their CPU
// copy after upload: it costs as much as the texture and playback never reads
// it back, so it is rebuilt lazily when get_pixel/create_surface asks for it.
constexpr bool kDropLargeCpuCopy = true;
constexpr int64_t kCpuCopyKeepLimit = 1024LL * 1024LL;

// Convert target surface to engine internal expected format.
// The converting will destroy origin surface.
inline void MakeSureSurfaceFormat(SDL_Surface*& surface) {
  if (surface->format != kSurfaceInternalFormat) {
    auto* converted_surface =
        SDL_ConvertSurface(surface, kSurfaceInternalFormat);
    SDL_DestroySurface(surface);
    surface = converted_surface;
  }
}

}  // namespace content

#endif  //! CONTENT_CANVAS_IMAGE_UTILS_H_
