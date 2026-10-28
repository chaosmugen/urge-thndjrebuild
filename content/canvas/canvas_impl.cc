// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/canvas/canvas_impl.h"

#include <cstdio>

#include "base/debug/logging.h"

#include "SDL3_image/SDL_image.h"
#include "SDL3_ttf/SDL_ttf.h"

#include "content/canvas/canvas_scheduler.h"
#include "content/canvas/image_utils.h"
#include "content/canvas/surface_impl.h"
#include "content/common/color_impl.h"
#include "content/common/rect_impl.h"
#include "content/context/execution_context.h"
#include "content/gpu/buffer_impl.h"
#include "content/gpu/texture_impl.h"
#include "content/io/iostream_impl.h"
#include "content/screen/renderscreen_impl.h"
#include "renderer/device/gpu_audit.h"
#include "renderer/utils/texture_utils.h"

namespace content {


// static
scoped_refptr<Bitmap> Bitmap::New(ExecutionContext* execution_context,
                                  const std::string& filename,
                                  ExceptionState& exception_state) {
  return CanvasImpl::Create(execution_context, filename, exception_state);
}

// static
scoped_refptr<Bitmap> Bitmap::New(ExecutionContext* execution_context,
                                  uint32_t width,
                                  uint32_t height,
                                  ExceptionState& exception_state) {
  return CanvasImpl::Create(execution_context, base::Vec2i(width, height),
                            exception_state);
}

// static
scoped_refptr<Bitmap> Bitmap::FromSurface(ExecutionContext* execution_context,
                                          scoped_refptr<Surface> target,
                                          ExceptionState& exception_state) {
  auto surface_obj = SurfaceImpl::From(target);
  if (!Disposable::IsValid(surface_obj.get())) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "invalid surface target");
    return nullptr;
  }

  auto* surface_data = **surface_obj;
  const int32_t max_texture_size =
      execution_context->render_device->MaxTextureSize();
  if (surface_data->w > max_texture_size ||
      surface_data->h > max_texture_size) {
    exception_state.ThrowError(
        ExceptionCode::GPU_ERROR,
        "texture size exceeds hardware limit: %dx%d (GPU max support: %d)",
        surface_data->w, surface_data->h, max_texture_size);
    return nullptr;
  }

  // Do not take ownership
  return base::MakeRefCounted<CanvasImpl>(execution_context, surface_data,
                                          false, "SurfaceData.Bitmap");
}

// static
scoped_refptr<Bitmap> Bitmap::FromTexture(ExecutionContext* execution_context,
                                          scoped_refptr<GPUTexture> gpu_texture,
                                          ExceptionState& exception_state) {
  auto* texture_obj = static_cast<TextureImpl*>(gpu_texture.get());
  if (!Disposable::IsValid(texture_obj)) {
    exception_state.ThrowError(ExceptionCode::GPU_ERROR, "invalid GPU texture");
    return nullptr;
  }

  return base::MakeRefCounted<CanvasImpl>(
      execution_context, texture_obj->AsRawPtr(), "GPUTexture.Bitmap");
}

// static
scoped_refptr<Bitmap> Bitmap::FromStream(ExecutionContext* execution_context,
                                         scoped_refptr<IOStream> stream,
                                         const std::string& extname,
                                         ExceptionState& exception_state) {
  auto stream_obj = IOStreamImpl::From(stream);
  if (!Disposable::IsValid(stream_obj.get())) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "invalid iostream input");
    return nullptr;
  }

  SDL_Surface* memory_texture =
      IMG_LoadTyped_IO(**stream_obj, false, extname.c_str());
  if (!memory_texture) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "failed to load image from iostream (%s)",
                               SDL_GetError());
    return nullptr;
  }

  const int32_t max_texture_size =
      execution_context->render_device->MaxTextureSize();
  // Taller than the device limit is served by page storage in
  // GPUCreateTextureInternal; only an over-wide bitmap is rejected here.
  if (memory_texture->w > max_texture_size) {
    exception_state.ThrowError(
        ExceptionCode::GPU_ERROR,
        "texture size exceeds hardware limit: %dx%d (GPU max support: %d)",
        memory_texture->w, memory_texture->h, max_texture_size);
    SDL_DestroySurface(memory_texture);
    return nullptr;
  }

  return base::MakeRefCounted<CanvasImpl>(execution_context, memory_texture,
                                          true, "IOStream.Bitmap");
}

// static
scoped_refptr<Bitmap> Bitmap::Copy(ExecutionContext* execution_context,
                                   scoped_refptr<Bitmap> other,
                                   ExceptionState& exception_state) {
  auto other_obj = CanvasImpl::FromBitmap(other);
  if (!Disposable::IsValid(other_obj.get())) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "invalid source bitmap");
    return nullptr;
  }

  auto duplicate_bitmap = CanvasImpl::Create(
      execution_context, (**other_obj)->size, exception_state);
  if (!duplicate_bitmap)
    return nullptr;

  duplicate_bitmap->Blt(0, 0, other_obj, other_obj->GetRect(exception_state),
                        255, -1, exception_state);

  return duplicate_bitmap;
}

// static
scoped_refptr<Bitmap> Bitmap::Deserialize(ExecutionContext* execution_context,
                                          const std::string& data,
                                          ExceptionState& exception_state) {
  const uint8_t* raw_data = reinterpret_cast<const uint8_t*>(data.data());

  if (data.size() < sizeof(uint32_t) * 2) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "invalid bitmap header data");
    return nullptr;
  }

  uint32_t surface_width = 0, surface_height = 0;
  std::memcpy(&surface_width, raw_data + sizeof(uint32_t) * 0,
              sizeof(uint32_t));
  std::memcpy(&surface_height, raw_data + sizeof(uint32_t) * 1,
              sizeof(uint32_t));

  const int32_t max_texture_size =
      execution_context->render_device->MaxTextureSize();
  if (static_cast<int32_t>(surface_width) > max_texture_size ||
      static_cast<int32_t>(surface_height) > max_texture_size) {
    exception_state.ThrowError(
        ExceptionCode::GPU_ERROR,
        "texture size exceeds hardware limit: %dx%d (GPU max support: %d)",
        surface_width, surface_height, max_texture_size);
    return nullptr;
  }

  if (data.size() < sizeof(uint32_t) * 2 + surface_width * surface_height * 4) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "invalid bitmap dump data");
    return nullptr;
  }

  SDL_Surface* surface =
      SDL_CreateSurface(surface_width, surface_height, kSurfaceInternalFormat);
  std::memcpy(surface->pixels, raw_data + sizeof(uint32_t) * 2,
              surface->pitch * surface->h);

  return base::MakeRefCounted<CanvasImpl>(execution_context, surface, true,
                                          "MemoryData");
}

// static
std::string Bitmap::Serialize(ExecutionContext* execution_context,
                              scoped_refptr<Bitmap> value,
                              ExceptionState& exception_state) {
  scoped_refptr<CanvasImpl> bitmap = CanvasImpl::FromBitmap(value);
  SDL_Surface* surface = bitmap->RequireMemorySurface();

  std::string serialized_data(
      sizeof(uint32_t) * 2 + surface->pitch * surface->h, 0);

  uint32_t surface_width = surface->w, surface_height = surface->h;
  std::memcpy(serialized_data.data() + sizeof(uint32_t) * 0, &surface_width,
              sizeof(uint32_t));
  std::memcpy(serialized_data.data() + sizeof(uint32_t) * 1, &surface_height,
              sizeof(uint32_t));
  std::memcpy(serialized_data.data() + sizeof(uint32_t) * 2, surface->pixels,
              surface->pitch * surface->h);

  return serialized_data;
}

// static
scoped_refptr<CanvasImpl> CanvasImpl::Create(
    ExecutionContext* execution_context,
    const base::Vec2i& size,
    ExceptionState& exception_state) {
  if (size.x <= 0 || size.y <= 0) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "invalid bitmap size: %dx%d", size.x, size.y);
    return nullptr;
  }

  const int32_t max_texture_size =
      execution_context->render_device->MaxTextureSize();
  if (size.x > max_texture_size) {
    exception_state.ThrowError(
        ExceptionCode::GPU_ERROR,
        "texture size exceeds hardware limit: %dx%d (GPU max support: %d)",
        size.x, size.y, max_texture_size);
    return nullptr;
  }

  // A bitmap the script creates itself is drawn into every frame in the list
  // and scroll paths. Making such a bitmap paged means drawing into page
  // textures, which crashes inside the NVIDIA D3D11 driver (scratch surface +
  // copy lowered it from ~67% to ~25%, it never went to zero). Clamping keeps
  // the game running; read-only content loaded from a file still pages.
  base::Vec2i create_size = size;
  if (create_size.y > max_texture_size) {
    LOG(INFO) << "[Canvas] Clamp bitmap " << size.x << "x" << size.y << " to "
              << create_size.x << "x" << max_texture_size
              << " (device limit)";
    create_size.y = max_texture_size;
  }

  auto* empty_surface =
      SDL_CreateSurface(create_size.x, create_size.y, kSurfaceInternalFormat);
  if (!empty_surface) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR, SDL_GetError());
    return nullptr;
  }

  return base::MakeRefCounted<CanvasImpl>(execution_context, empty_surface,
                                          true, "Empty.Bitmap");
}

// static
scoped_refptr<CanvasImpl> CanvasImpl::Create(
    ExecutionContext* execution_context,
    const std::string& filename,
    ExceptionState& exception_state) {
  SDL_Surface* memory_texture = nullptr;
  auto file_handler = base::BindRepeating(
      [](SDL_Surface** surface, SDL_IOStream* stream, const std::string& ext) {
        *surface = IMG_LoadTyped_IO(stream, true, ext.c_str());
        return !!*surface;
      },
      &memory_texture);

  filesystem::IOState io_state;
  execution_context->io_service->OpenRead(filename, file_handler, &io_state);
  if (io_state.error_count) {
    exception_state.ThrowError(ExceptionCode::IO_ERROR, "%s: %s",
                               io_state.error_message.c_str(),
                               filename.c_str());
    return nullptr;
  }

  if (!memory_texture) {
    exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                               "failed to load image: %s (%s)",
                               filename.c_str(), SDL_GetError());
    return nullptr;
  }

  const int32_t max_texture_size =
      execution_context->render_device->MaxTextureSize();
  // Taller than the device limit is served by page storage in
  // GPUCreateTextureInternal; only an over-wide bitmap is rejected here.
  if (memory_texture->w > max_texture_size) {
    exception_state.ThrowError(
        ExceptionCode::GPU_ERROR,
        "texture size exceeds hardware limit: %dx%d (GPU max support: %d)",
        memory_texture->w, memory_texture->h, max_texture_size);
    SDL_DestroySurface(memory_texture);
    return nullptr;
  }

  return base::MakeRefCounted<CanvasImpl>(execution_context, memory_texture,
                                          true, filename);
}

// static
scoped_refptr<CanvasImpl> CanvasImpl::FromBitmap(scoped_refptr<Bitmap> host) {
  return static_cast<CanvasImpl*>(host.get());
}

///////////////////////////////////////////////////////////////////////////////
// Canvas Implement

CanvasImpl::CanvasImpl(ExecutionContext* execution_context,
                       SDL_Surface* memory_surface,
                       bool has_ownership,
                       const std::string& debug_name)
    : EngineObject(execution_context),
      Disposable(execution_context->disposable_parent),
      surface_cache_(nullptr),
      font_(base::MakeRefCounted<FontImpl>(execution_context->font_context)) {
  // Link with scheduler node
  context()->canvas_scheduler->children_.Append(this);

  // Normalize the surface format before anything else takes a reference on it.
  // The conversion destroys the original surface and replaces the pointer, so
  // it must happen before |surface_cache_| is bound, otherwise the cache would
  // dangle for any source image that is not already in the internal format
  // (e.g. 24-bit JPEG or palette-based PNG).
  if (memory_surface)
    MakeSureSurfaceFormat(memory_surface);

  surface_cache_ = has_ownership ? memory_surface : nullptr;

  // Create renderer texture
  gpu_.name = debug_name;
  GPUCreateTextureInternal(memory_surface);

  // A bitmap loaded from disk keeps a full CPU copy for get_pixel and
  // create_surface. For the oversized animation sheets that copy is as large as
  // the texture itself (960x7680 = 28 MB each) and playback never reads it
  // back, so drop it for big bitmaps; RequireMemorySurface rebuilds on demand.
  if (kDropLargeCpuCopy && surface_cache_ &&
      static_cast<int64_t>(surface_cache_->w) * surface_cache_->h >=
          kCpuCopyKeepLimit) {
    SDL_DestroySurface(surface_cache_);
    surface_cache_ = nullptr;
  }
}

CanvasImpl::CanvasImpl(ExecutionContext* execution_context,
                       Diligent::ITexture* gpu_texture,
                       const std::string& debug_name)
    : EngineObject(execution_context),
      Disposable(execution_context->disposable_parent),
      surface_cache_(nullptr),
      font_(base::MakeRefCounted<FontImpl>(execution_context->font_context)) {
  // Link with scheduler node
  context()->canvas_scheduler->children_.Append(this);

  // Create renderer texture with texture
  gpu_.name = debug_name;
  gpu_.data = gpu_texture;
  GPUCreateTextureInternal(nullptr);
}

DISPOSABLE_DEFINITION(CanvasImpl);

SDL_Surface* CanvasImpl::RequireMemorySurface() {
  if (!surface_cache_) {
    // Create empty cache
    surface_cache_ =
        SDL_CreateSurface(gpu_.size.x, gpu_.size.y, kSurfaceInternalFormat);

    // Submit pending commands
    SubmitQueuedCommands();

    // Sync fetch pixels to memory
    GPUFetchTexturePixelsDataInternal();
  }

  return surface_cache_;
}

void CanvasImpl::InvalidateSurfaceCache() {
  // Notify children observers
  observers_.Notify();

  if (surface_cache_) {
    // Set cache ptr to null
    SDL_DestroySurface(surface_cache_);
    surface_cache_ = nullptr;
  }
}

void CanvasImpl::SubmitQueuedCommands(bool at_frame_boundary) {
  // TODO(canvas-order): temporary ordering marker. The window contents are painted by the
  // game (clear, then redraw) and applied here; the window samples the texture. If a
  // window draws before the paint commands of that frame are applied it shows the cleared
  // - but not yet redrawn - contents, i.e. an empty window for exactly one frame.
  Command* command_sequence = commands_;
  // Execute pending commands,
  // encode draw command in wgpu encoder.
  while (command_sequence) {
    switch (command_sequence->id) {
      case CommandID::CLEAR: {
        if (gpu_.IsPaged()) {
          for (int32_t page = 0; page < gpu_.PageCount(); ++page) {
            BeginPageTargetInternal(page);
            GPUCanvasClearInternal();
            EndPageTargetInternal();
          }
        } else {
          GPUCanvasClearInternal();
        }
      } break;
      case CommandID::GRADIENT_FILL_RECT: {
        const auto* c =
            static_cast<Command_GradientFillRect*>(command_sequence);
        if (gpu_.IsPaged()) {
          for (int32_t page = 0; page < gpu_.PageCount(); ++page) {
            const int32_t page_top = gpu_.PageTop(page);
            const int32_t top = std::max(c->region.y, page_top);
            const int32_t bottom = std::min(c->region.y + c->region.height,
                                            page_top + gpu_.PageHeightAt(page));
            if (bottom <= top)
              continue;

            base::Vec4 color1 = c->color1;
            base::Vec4 color2 = c->color2;
            if (c->vertical && c->region.height > 0) {
              // A vertical gradient spans the whole region, so each page slice
              // samples it at its own position.
              const float t0 = static_cast<float>(top - c->region.y) /
                               static_cast<float>(c->region.height);
              const float t1 = static_cast<float>(bottom - c->region.y) /
                               static_cast<float>(c->region.height);
              color1 = base::Vec4(
                  c->color1.x + (c->color2.x - c->color1.x) * t0,
                  c->color1.y + (c->color2.y - c->color1.y) * t0,
                  c->color1.z + (c->color2.z - c->color1.z) * t0,
                  c->color1.w + (c->color2.w - c->color1.w) * t0);
              color2 = base::Vec4(
                  c->color1.x + (c->color2.x - c->color1.x) * t1,
                  c->color1.y + (c->color2.y - c->color1.y) * t1,
                  c->color1.z + (c->color2.z - c->color1.z) * t1,
                  c->color1.w + (c->color2.w - c->color1.w) * t1);
            }

            base::Rect slice_dst(c->region.x, top - page_top, c->region.width,
                                 bottom - top);
            BeginPageTargetInternal(page);
            GPUCanvasGradientFillRectInternal(slice_dst, color1, color2,
                                              c->vertical);
            EndPageTargetInternal();
          }
        } else {
          GPUCanvasGradientFillRectInternal(c->region, c->color1, c->color2,
                                            c->vertical);
        }
      } break;
      case CommandID::HUE_CHANGE: {
        const auto* c = static_cast<Command_HueChange*>(command_sequence);
        if (gpu_.IsPaged()) {
          for (int32_t page = 0; page < gpu_.PageCount(); ++page) {
            BeginPageTargetInternal(page);
            GPUCanvasHueChange(c->hue);
            EndPageTargetInternal();
          }
        } else {
          GPUCanvasHueChange(c->hue);
        }
      } break;
      case CommandID::DRAW_TEXT: {
        auto* c = static_cast<Command_DrawText*>(command_sequence);
        if (gpu_.IsPaged()) {
          // The text surface is consumed by the draw (freed at the end), so a
          // copy is what each page slice reads from.
          SDL_Surface* text_copy = SDL_DuplicateSurface(c->text);
          if (text_copy) {
            SDL_DestroySurface(c->text);
            c->text = nullptr;

            for (int32_t page = 0; page < gpu_.PageCount(); ++page) {
              const int32_t page_top = gpu_.PageTop(page);
              const int32_t top = std::max(c->region.y, page_top);
              const int32_t bottom = std::min(c->region.y + c->region.height,
                                              page_top +
                                                  gpu_.PageHeightAt(page));
              if (bottom <= top)
                continue;

              const int32_t row_offset = top - c->region.y;
              SDL_Surface* text_slice = SDL_CreateSurfaceFrom(
                  text_copy->w, bottom - top, text_copy->format,
                  static_cast<uint8_t*>(text_copy->pixels) +
                      static_cast<size_t>(row_offset) * text_copy->pitch,
                  text_copy->pitch);
              if (!text_slice)
                continue;

              base::Rect slice_dst(c->region.x, top - page_top, c->region.width,
                                   bottom - top);
              BeginPageTargetInternal(page);
              GPUCanvasDrawTextSurfaceInternal(slice_dst, text_slice,
                                               c->opacity, 0);
              EndPageTargetInternal();
            }

            SDL_DestroySurface(text_copy);
          }
        } else {
          GPUCanvasDrawTextSurfaceInternal(c->region, c->text, c->opacity,
                                           c->align);
        }
      } break;
      default:
        NOTREACHED();
    }

    command_sequence = command_sequence->next;
  }

  // Clear command pool,
  // no memory release.
  ClearPendingCommands();
}

uint32_t CanvasImpl::Width(ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(0);

  return gpu_.size.x;
}

uint32_t CanvasImpl::Height(ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(0);

  return gpu_.size.y;
}

scoped_refptr<Rect> CanvasImpl::GetRect(ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  return base::MakeRefCounted<RectImpl>(gpu_.size);
}

void CanvasImpl::Blt(int32_t x,
                     int32_t y,
                     scoped_refptr<Bitmap> src_bitmap,
                     scoped_refptr<Rect> src_rect,
                     uint32_t opacity,
                     int32_t blend_type,
                     ExceptionState& exception_state) {
  DISPOSE_CHECK;

  auto src_bitmap_obj = FromBitmap(src_bitmap);
  if (!Disposable::IsValid(src_bitmap_obj.get()))
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid blt source");

  auto src_rect_obj = RectImpl::From(src_rect);
  if (!src_rect_obj)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid source rect object");

  const auto src_rect_base = src_rect_obj->AsBaseRect();
  const base::Rect blit_region(x, y, src_rect_base.width, src_rect_base.height);
  BlitTextureInternal(blit_region, src_bitmap_obj.get(), src_rect_base,
                      blend_type, opacity);
}

void CanvasImpl::StretchBlt(scoped_refptr<Rect> dest_rect,
                            scoped_refptr<Bitmap> src_bitmap,
                            scoped_refptr<Rect> src_rect,
                            uint32_t opacity,
                            int32_t blend_type,
                            ExceptionState& exception_state) {
  DISPOSE_CHECK;

  auto src_bitmap_obj = FromBitmap(src_bitmap);
  if (!Disposable::IsValid(src_bitmap_obj.get()))
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid blt source");

  auto dest_rect_obj = RectImpl::From(dest_rect);
  if (!dest_rect_obj)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid destination rect object");

  auto src_rect_obj = RectImpl::From(src_rect);
  if (!src_rect_obj)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid source rect object");

  BlitTextureInternal(dest_rect_obj->AsBaseRect(), src_bitmap_obj.get(),
                      src_rect_obj->AsBaseRect(), blend_type, opacity);
}

void CanvasImpl::ClipBlt(scoped_refptr<Rect> dest_rect,
                         scoped_refptr<Bitmap> src_bitmap,
                         scoped_refptr<Rect> src_rect,
                         scoped_refptr<Bitmap> clip_bitmap,
                         ExceptionState& exception_state) {
  DISPOSE_CHECK;

  auto src_bitmap_obj = FromBitmap(src_bitmap);
  if (!Disposable::IsValid(src_bitmap_obj.get()))
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid blt source");

  auto clip_bitmap_obj = FromBitmap(clip_bitmap);
  if (!Disposable::IsValid(clip_bitmap_obj.get()))
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid clip source");

  auto dest_rect_obj = RectImpl::From(dest_rect);
  if (!dest_rect_obj)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid destination rect object");

  auto src_rect_obj = RectImpl::From(src_rect);
  if (!src_rect_obj)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid source rect object");

  ClipTextureInternal(dest_rect_obj->AsBaseRect(), src_bitmap_obj.get(),
                      src_rect_obj->AsBaseRect(), clip_bitmap_obj.get());
}

void CanvasImpl::FillRect(int32_t x,
                          int32_t y,
                          uint32_t width,
                          uint32_t height,
                          scoped_refptr<Color> color,
                          ExceptionState& exception_state) {
  GradientFillRect(x, y, width, height, color, color, false, exception_state);
}

void CanvasImpl::FillRect(scoped_refptr<Rect> rect,
                          scoped_refptr<Color> color,
                          ExceptionState& exception_state) {
  if (!rect)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid rect object");

  FillRect(rect->Get_X(exception_state), rect->Get_Y(exception_state),
           rect->Get_Width(exception_state), rect->Get_Height(exception_state),
           color, exception_state);
}

void CanvasImpl::GradientFillRect(int32_t x,
                                  int32_t y,
                                  uint32_t width,
                                  uint32_t height,
                                  scoped_refptr<Color> color1,
                                  scoped_refptr<Color> color2,
                                  bool vertical,
                                  ExceptionState& exception_state) {
  DISPOSE_CHECK;

  if (width <= 0 || height <= 0)
    return;

  if (!color1 || !color2)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid color object");

  auto* command = AllocateCommand<Command_GradientFillRect>();
  command->region = base::Rect(x, y, width, height);
  command->color1 = ColorImpl::From(color1.get())->AsNormColor();
  command->color2 = ColorImpl::From(color2.get())->AsNormColor();
  command->vertical = vertical;

  InvalidateSurfaceCache();
}

void CanvasImpl::GradientFillRect(int32_t x,
                                  int32_t y,
                                  uint32_t width,
                                  uint32_t height,
                                  scoped_refptr<Color> color1,
                                  scoped_refptr<Color> color2,
                                  ExceptionState& exception_state) {
  GradientFillRect(x, y, width, height, color1, color2, false, exception_state);
}

void CanvasImpl::GradientFillRect(scoped_refptr<Rect> rect,
                                  scoped_refptr<Color> color1,
                                  scoped_refptr<Color> color2,
                                  bool vertical,
                                  ExceptionState& exception_state) {
  if (!rect)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid rect object");

  GradientFillRect(rect->Get_X(exception_state), rect->Get_Y(exception_state),
                   rect->Get_Width(exception_state),
                   rect->Get_Height(exception_state), color1, color2, vertical,
                   exception_state);
}

void CanvasImpl::GradientFillRect(scoped_refptr<Rect> rect,
                                  scoped_refptr<Color> color1,
                                  scoped_refptr<Color> color2,
                                  ExceptionState& exception_state) {
  if (!rect)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid rect object");

  GradientFillRect(rect->Get_X(exception_state), rect->Get_Y(exception_state),
                   rect->Get_Width(exception_state),
                   rect->Get_Height(exception_state), color1, color2, false,
                   exception_state);
}

void CanvasImpl::Clear(ExceptionState& exception_state) {
  DISPOSE_CHECK;

  AllocateCommand<Command_Clear>();

  InvalidateSurfaceCache();
}

void CanvasImpl::ClearRect(int32_t x,
                           int32_t y,
                           uint32_t width,
                           uint32_t height,
                           ExceptionState& exception_state) {
  DISPOSE_CHECK;

  auto* command = AllocateCommand<Command_GradientFillRect>();
  command->region = base::Rect(x, y, width, height);
  command->color1 = base::Vec4(0);
  command->color2 = command->color1;
  command->vertical = false;

  InvalidateSurfaceCache();
}

void CanvasImpl::ClearRect(scoped_refptr<Rect> rect,
                           ExceptionState& exception_state) {
  if (!rect)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid rect object");

  ClearRect(rect->Get_X(exception_state), rect->Get_Y(exception_state),
            rect->Get_Width(exception_state), rect->Get_Height(exception_state),
            exception_state);
}

scoped_refptr<Color> CanvasImpl::GetPixel(int32_t x,
                                          int32_t y,
                                          ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  if (x < 0 || x >= gpu_.size.x || y < 0 || y >= gpu_.size.y)
    return nullptr;

  SDL_Surface* surface = RequireMemorySurface();
  auto* pixel_detail = SDL_GetPixelFormatDetails(surface->format);
  int32_t bpp = pixel_detail->bytes_per_pixel;
  uint8_t* pixel = static_cast<uint8_t*>(surface->pixels) +
                   static_cast<size_t>(y) * surface->pitch +
                   static_cast<size_t>(x) * bpp;

  uint8_t color[4];
  SDL_GetRGBA(*reinterpret_cast<uint32_t*>(pixel), pixel_detail, nullptr,
              &color[0], &color[1], &color[2], &color[3]);

  return base::MakeRefCounted<ColorImpl>(
      base::Vec4(color[0], color[1], color[2], color[3]));
}

void CanvasImpl::SetPixel(int32_t x,
                          int32_t y,
                          scoped_refptr<Color> color,
                          ExceptionState& exception_state) {
  DISPOSE_CHECK;

  if (x < 0 || x >= gpu_.size.x || y < 0 || y >= gpu_.size.y)
    return;

  auto color_obj = ColorImpl::From(color.get());
  if (!color_obj)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid color object");

  auto* command = AllocateCommand<Command_GradientFillRect>();
  command->region = base::Rect(x, y, 1, 1);
  command->color1 = color_obj->AsNormColor();
  command->color2 = command->color1;
  command->vertical = false;

  if (surface_cache_) {
    SDL_Surface* surface = RequireMemorySurface();
    if (surface) {
      const SDL_Color color_unorm = color_obj->AsSDLColor();
      auto* pixel_detail = SDL_GetPixelFormatDetails(surface->format);
      int bpp = pixel_detail->bytes_per_pixel;
      uint8_t* pixel =
          static_cast<uint8_t*>(surface->pixels) + y * surface->pitch + x * bpp;
      *reinterpret_cast<uint32_t*>(pixel) =
          SDL_MapRGBA(pixel_detail, nullptr, color_unorm.r, color_unorm.g,
                      color_unorm.b, color_unorm.a);
    }
  }
}

void CanvasImpl::HueChange(int32_t hue, ExceptionState& exception_state) {
  DISPOSE_CHECK;

  if (hue % 360 == 0)
    return;
  while (hue < 0)
    hue += 360;
  hue %= 360;

  auto* command = AllocateCommand<Command_HueChange>();
  command->hue = hue;

  InvalidateSurfaceCache();
}

void CanvasImpl::Blur(ExceptionState& exception_state) {
  DISPOSE_CHECK;

  // TODO:
}

void CanvasImpl::RadialBlur(int32_t angle,
                            int32_t division,
                            ExceptionState& exception_state) {
  DISPOSE_CHECK;

  // TODO:
}

void CanvasImpl::DrawText(int32_t x,
                          int32_t y,
                          uint32_t width,
                          uint32_t height,
                          const std::string& str,
                          int32_t align,
                          ExceptionState& exception_state) {
  DISPOSE_CHECK;

  auto font_obj = FontImpl::From(font_);
  if (!font_obj->GetCanonicalFont(exception_state))
    return;

  uint8_t font_opacity = 0;
  auto* text_surface =
      font_obj->RenderText(str, &font_opacity, exception_state);
  if (!text_surface)
    return;

  // Destroy memory surface in queue deferrer.
  auto* command = AllocateCommand<Command_DrawText>();
  command->region = base::Rect(x, y, width, height);
  command->text = text_surface;  // Transient surface object
  command->opacity = static_cast<float>(font_opacity);
  command->align = align;

  InvalidateSurfaceCache();
}

void CanvasImpl::DrawText(int32_t x,
                          int32_t y,
                          uint32_t width,
                          uint32_t height,
                          const std::string& str,
                          ExceptionState& exception_state) {
  DrawText(x, y, width, height, str, 0, exception_state);
}

void CanvasImpl::DrawText(scoped_refptr<Rect> rect,
                          const std::string& str,
                          int32_t align,
                          ExceptionState& exception_state) {
  if (!rect)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid rect object");

  DrawText(rect->Get_X(exception_state), rect->Get_Y(exception_state),
           rect->Get_Width(exception_state), rect->Get_Height(exception_state),
           str, align, exception_state);
}

void CanvasImpl::DrawText(scoped_refptr<Rect> rect,
                          const std::string& str,
                          ExceptionState& exception_state) {
  if (!rect)
    return exception_state.ThrowError(ExceptionCode::CONTENT_ERROR,
                                      "invalid rect object");

  DrawText(rect->Get_X(exception_state), rect->Get_Y(exception_state),
           rect->Get_Width(exception_state), rect->Get_Height(exception_state),
           str, 0, exception_state);
}

scoped_refptr<Rect> CanvasImpl::TextSize(const std::string& str,
                                         ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  auto font_obj = FontImpl::From(font_);
  TTF_Font* font = font_obj->GetCanonicalFont(exception_state);
  if (!font)
    return nullptr;

  int32_t w, h;
  TTF_GetStringSize(font, str.c_str(), str.size(), &w, &h);
  return base::MakeRefCounted<RectImpl>(base::Rect(0, 0, w, h));
}

scoped_refptr<Surface> CanvasImpl::CreateSurface(
    ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  SDL_Surface* surface = RequireMemorySurface();
  return base::MakeRefCounted<SurfaceImpl>(context(), surface);
}

scoped_refptr<GPUTexture> CanvasImpl::GetTexture(
    ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  // Submit pending draw commands
  SubmitQueuedCommands();

  // Clear surface cache
  InvalidateSurfaceCache();

  return base::MakeRefCounted<TextureImpl>(context(), gpu_.data);
}

scoped_refptr<GPUTexture> CanvasImpl::GetDepthStencil(
    ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  // Submit pending draw commands
  SubmitQueuedCommands();

  // Clear surface cache
  InvalidateSurfaceCache();

  return base::MakeRefCounted<TextureImpl>(context(), gpu_.depth);
}

scoped_refptr<GPUTextureView> CanvasImpl::GetShaderResourceView(
    ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  // Submit pending draw commands
  SubmitQueuedCommands();

  // Clear surface cache
  InvalidateSurfaceCache();

  return base::MakeRefCounted<TextureViewImpl>(context(),
                                               gpu_.shader_resource_view);
}

scoped_refptr<GPUTextureView> CanvasImpl::GetRenderTargetView(
    ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  // Submit pending draw commands
  SubmitQueuedCommands();

  // Clear surface cache
  InvalidateSurfaceCache();

  return base::MakeRefCounted<TextureViewImpl>(context(),
                                               gpu_.render_target_view);
}

scoped_refptr<GPUTextureView> CanvasImpl::GetDepthStencilView(
    ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  // Submit pending draw commands
  SubmitQueuedCommands();

  // Clear surface cache
  InvalidateSurfaceCache();

  return base::MakeRefCounted<TextureViewImpl>(context(),
                                               gpu_.depth_stencil_view);
}

scoped_refptr<GPUBuffer> CanvasImpl::GetWorldUniformBuffer(
    ExceptionState& exception_state) {
  DISPOSE_CHECK_RETURN(nullptr);

  return base::MakeRefCounted<BufferImpl>(context(), gpu_.world_buffer);
}

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Font,
    scoped_refptr<Font>,
    CanvasImpl,
    {
      DISPOSE_CHECK_RETURN(nullptr);
      return font_;
    },
    {
      DISPOSE_CHECK;
      CHECK_ATTRIBUTE_VALUE;
      *font_ = *FontImpl::From(value);
    });

void CanvasImpl::OnObjectDisposed() {
  // Unlink from canvas scheduler
  base::LinkNode<CanvasImpl>::RemoveFromList();

  // Free memory surface cache
  InvalidateSurfaceCache();

  // Park the GPU objects instead of destroying them here. This is where a game bitmap dies
  // - including the window contents bitmap, which the game replaces several times a second
  // while the window keeps drawing - and a texture destroyed while a draw recorded in an
  // earlier frame may still be sampling it has its memory handed to the next allocation on
  // Vulkan, which paints a piece of some other image into exactly one frame. See
  // renderer::DeferredTextureRelease.
  renderer::DeferredTextureRelease(gpu_.data);
  renderer::DeferredTextureRelease(gpu_.depth);
  // Paged bitmaps own their page textures on top of the page-0 aliases above.
  for (auto& it : gpu_.pages)
    renderer::DeferredTextureRelease(it);
  for (auto& it : gpu_.pages_srv)
    renderer::DeferredTextureRelease(it);
  for (auto& it : gpu_.pages_depth)
    renderer::DeferredTextureRelease(it);
  for (auto& it : gpu_.pages_dsv)
    renderer::DeferredTextureRelease(it);
  for (auto& it : gpu_.pages_rtv)
    renderer::DeferredTextureRelease(it);
  for (auto& it : gpu_.pages_world)
    renderer::DeferredTextureRelease(it);
  renderer::DeferredTextureRelease(gpu_.page_scratch);
  renderer::DeferredTextureRelease(gpu_.page_scratch_depth);
  renderer::DeferredTextureRelease(gpu_.page_scratch_srv);
  renderer::DeferredTextureRelease(gpu_.page_scratch_rtv);
  renderer::DeferredTextureRelease(gpu_.page_scratch_dsv);
  renderer::DeferredTextureRelease(gpu_.page_scratch_world);
  // The three views hang off those two textures. Parking the textures but destroying their
  // views here leaves every descriptor still in flight naming a freed view object - the same
  // hazard one level up, so they go into the same graveyard.
  renderer::DeferredTextureRelease(gpu_.shader_resource_view);
  renderer::DeferredTextureRelease(gpu_.render_target_view);
  renderer::DeferredTextureRelease(gpu_.depth_stencil_view);

  // Destroy GPU texture
  GPUData empty_data;
  std::swap(gpu_, empty_data);
}

void CanvasImpl::BlitTextureInternal(const base::Rect& dst_rect,
                                     CanvasImpl* src_texture,
                                     const base::Rect& src_rect,
                                     int32_t blend_type,
                                     uint32_t opacity) {
  // Synchronize pending queue immediately,
  // blit the sourcetexture to destination texture immediately.
  src_texture->SubmitQueuedCommands();
  SubmitQueuedCommands();

  // Temporary ordering marker: the paint commands of a canvas are applied at a frame
  // boundary, but the blits that finish its content run here, when the game gets to them.
  // If a node samples the destination texture before this line for the same frame, that
  // frame draws the half-painted bitmap - the background losing its content for a frame.
  // The frame number and the texture pointers say which canvas and in which order.

  // Emits one blit. When the source bitmap is paged the copy is issued once per
  // source page: each pass binds that page's view and works in page-local
  // source rows, with the destination rows scaled the same way the whole blit
  // is, so a stretched blit keeps its ratio inside every page slice.
  auto EmitBlit = [&](const base::Rect& dst_slice,
                      const base::Rect& src_slice,
                      int32_t blend,
                      uint32_t alpha) {
    if (!src_texture->gpu_.IsPaged()) {
      if (blend >= 0) {
        GPUBlendBlitTextureInternal(dst_slice, &src_texture->gpu_, src_slice,
                                    blend, alpha);
      } else {
        GPUApproximateBlitTextureInternal(dst_slice, &src_texture->gpu_,
                                          src_slice, alpha);
      }
      return;
    }

    const GPUData& src_gpu = src_texture->gpu_;
    const int32_t src_first = src_gpu.PageIndexAt(src_slice.y);
    const int32_t src_last =
        src_gpu.PageIndexAt(src_slice.y + src_slice.height - 1);

    for (int32_t src_page = src_first; src_page <= src_last; ++src_page) {
      const int32_t src_page_top = src_gpu.PageTop(src_page);
      const int32_t src_page_height = src_gpu.PageHeightAt(src_page);
      const int32_t s_top = std::max(src_slice.y, src_page_top);
      const int32_t s_bottom = std::min(src_slice.y + src_slice.height,
                                        src_page_top + src_page_height);
      if (s_bottom <= s_top)
        continue;

      const int32_t src_rows = s_bottom - s_top;
      const int32_t src_offset = s_top - src_slice.y;
      int32_t dst_rows = src_rows;
      int32_t dst_offset = src_offset;
      if (src_slice.height > 0 && dst_slice.height != src_slice.height) {
        dst_rows = std::max(
            1, static_cast<int32_t>(static_cast<int64_t>(src_rows) *
                                    dst_slice.height / src_slice.height));
        dst_offset = static_cast<int32_t>(
            static_cast<int64_t>(src_offset) * dst_slice.height /
            src_slice.height);
      }

      base::Rect page_src(src_slice.x, s_top - src_page_top, src_slice.width,
                          src_rows);
      base::Rect page_dst(dst_slice.x, dst_slice.y + dst_offset,
                          dst_slice.width, dst_rows);

      // A stand-in for the source: this page's view and this page's height, so
      // the UVs the blit builds are relative to the page it actually samples.
      GPUData page_data = src_gpu;
      page_data.shader_resource_view = src_gpu.pages_srv[src_page];
      page_data.size.y = src_page_height;

      if (blend >= 0) {
        GPUBlendBlitTextureInternal(page_dst, &page_data, page_src, blend,
                                    alpha);
      } else {
        GPUApproximateBlitTextureInternal(page_dst, &page_data, page_src,
                                          alpha);
      }
    }
  };

  // A paged target is written one page at a time: each page gets the part of
  // the destination it owns, in page-local coordinates, with the matching part
  // of the source rect.
  if (gpu_.IsPaged()) {
    const int32_t first_page = gpu_.PageIndexAt(dst_rect.y);
    const int32_t last_page =
        gpu_.PageIndexAt(dst_rect.y + dst_rect.height - 1);
    const int32_t clamped_blend =
        blend_type >= 0 ? std::clamp<int32_t>(blend_type, 0, BLEND_TYPE_NUMS - 1)
                        : -1;

    for (int32_t page = first_page; page <= last_page; ++page) {
      const int32_t page_top = gpu_.PageTop(page);
      const int32_t top = std::max(dst_rect.y, page_top);
      const int32_t bottom = std::min(dst_rect.y + dst_rect.height,
                                      page_top + gpu_.PageHeightAt(page));
      if (bottom <= top)
        continue;

      base::Rect slice_src = src_rect;
      const int32_t dst_offset = top - dst_rect.y;
      if (src_rect.height > 0 && src_rect.height != dst_rect.height &&
          dst_rect.height > 0) {
        // Stretched blit: the source rows scale with the destination rows.
        slice_src.y += static_cast<int32_t>(static_cast<int64_t>(dst_offset) *
                                            src_rect.height /
                                            dst_rect.height);
        slice_src.height =
            std::max(1, static_cast<int32_t>(
                            static_cast<int64_t>(bottom - top) *
                            src_rect.height / dst_rect.height));
      } else {
        slice_src.y += dst_offset;
        slice_src.height = bottom - top;
      }

      base::Rect slice_dst(dst_rect.x, top - page_top, dst_rect.width,
                           bottom - top);

      BeginPageTargetInternal(page);
      EmitBlit(slice_dst, slice_src, clamped_blend, opacity);
      EndPageTargetInternal();
    }

    InvalidateSurfaceCache();
    return;
  }

  // Execute blit immediately.
  if (blend_type >= 0) {
    EmitBlit(dst_rect, src_rect,
             std::clamp<int32_t>(blend_type, 0, BLEND_TYPE_NUMS - 1), opacity);
  } else {
    // Apply approximate blend pipeline render pass (DDraw style)
    EmitBlit(dst_rect, src_rect, -1, opacity);
  }

  // Invalidate memory cache
  InvalidateSurfaceCache();
}

void CanvasImpl::ClipTextureInternal(const base::Rect& dst_rect,
                                     CanvasImpl* src_texture,
                                     const base::Rect& src_rect,
                                     CanvasImpl* clip_texture) {
  // Synchronize pending queue immediately,
  // blit the sourcetexture to destination texture immediately.
  src_texture->SubmitQueuedCommands();
  SubmitQueuedCommands();

  // Temporary ordering marker, same purpose as the one in BlitTextureInternal: the only
  // other path that writes into a canvas' texture. See the blt marker above.

  // Execute clip blit immediately.
  GPUClipTextureInternal(dst_rect, &src_texture->gpu_, src_rect,
                         &clip_texture->gpu_);

  // Invalidate memory cache
  InvalidateSurfaceCache();
}

// Uploads one page of a paged bitmap: |rows| rows of |data| starting at |top|.
// Same texture description as renderer::CreateTexture2D(surface); only the
// height and the source row offset differ.
static void CreatePageTexture2D(Diligent::IRenderDevice* device,
                               Diligent::ITexture** texture,
                               const std::string& name,
                               SDL_Surface* data,
                               int32_t top,
                               int32_t rows) {
  Diligent::TextureDesc texture_desc;
  texture_desc.Name = name.c_str();
  texture_desc.Type = Diligent::RESOURCE_DIM_TEX_2D;
  texture_desc.Width = data->w;
  texture_desc.Height = rows;
  texture_desc.Format = Diligent::TEX_FORMAT_RGBA8_UNORM;
  texture_desc.BindFlags =
      Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE;
  texture_desc.Usage = Diligent::USAGE_DEFAULT;
  texture_desc.CPUAccessFlags = Diligent::CPU_ACCESS_NONE;
  texture_desc.ClearValue.Format = texture_desc.Format;
  std::memset(texture_desc.ClearValue.Color, 0,
              sizeof(texture_desc.ClearValue.Color));

  Diligent::TextureSubResData texture_sub_res_data;
  texture_sub_res_data.pData =
      static_cast<uint8_t*>(data->pixels) + static_cast<size_t>(top) * data->pitch;
  texture_sub_res_data.Stride = static_cast<uint32_t>(data->pitch);

  Diligent::TextureData texture_data;
  texture_data.pSubResources = &texture_sub_res_data;
  texture_data.NumSubresources = 1;

  device->CreateTexture(texture_desc, &texture_data, texture);
}

void CanvasImpl::GPUCreateTextureInternal(SDL_Surface* texture_data) {
  auto* scheduler = context()->canvas_scheduler;
  auto& render_device = *scheduler->GetRenderDevice();
  const int32_t max_texture_size =
      context()->render_device->MaxTextureSize();

  if (texture_data) {
    // Make sure surface format
    MakeSureSurfaceFormat(texture_data);

    // Logical bitmap size: always the whole image, also when it is paged, so
    // every script-visible coordinate stays unchanged.
    gpu_.size = base::Vec2i(texture_data->w, texture_data->h);

    if (texture_data->h > max_texture_size) {
      // Split into row-aligned pages. Page 0 doubles as the single-texture
      // handle (|data| / views) so the existing single-texture paths still
      // sample something sensible; writes switch to |pages_rtv| /
      // |pages_world| page by page (see BeginPageTargetInternal).
      gpu_.page_height = max_texture_size;
      const int32_t page_count =
          (texture_data->h + gpu_.page_height - 1) / gpu_.page_height;
      for (int32_t i = 0; i < page_count; ++i) {
        const int32_t top = i * gpu_.page_height;
        const int32_t rows = std::min(gpu_.page_height, texture_data->h - top);
        RRefPtr<Diligent::ITexture> page;
        CreatePageTexture2D(*render_device, &page, gpu_.name, texture_data, top,
                            rows);
        if (!page)
          continue;

        RRefPtr<Diligent::ITextureView> page_view;
        page_view = page->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
        RRefPtr<Diligent::ITextureView> page_rtv;
        page_rtv = page->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);

        const base::Vec2i page_size(texture_data->w, rows);
        RRefPtr<Diligent::ITexture> page_depth;
        renderer::CreateTexture2D(*render_device, &page_depth, gpu_.name,
                                  page_size, Diligent::USAGE_DEFAULT,
                                  Diligent::BIND_DEPTH_STENCIL,
                                  Diligent::CPU_ACCESS_NONE,
                                  Diligent::TEX_FORMAT_D24_UNORM_S8_UINT);
        RRefPtr<Diligent::ITextureView> page_dsv;
        if (page_depth) {
          page_dsv =
              page_depth->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL);
        }

        renderer::WorldTransform page_world;
        renderer::MakeProjectionMatrix(page_world.projection,
                                       page_size.Recast<float>());
        renderer::MakeIdentityMatrix(page_world.transform);
        RRefPtr<Diligent::IBuffer> page_world_buffer;
        Diligent::CreateUniformBuffer(
            *render_device, sizeof(page_world), "bitmap.page.world",
            &page_world_buffer, Diligent::USAGE_IMMUTABLE,
            Diligent::BIND_UNIFORM_BUFFER, Diligent::CPU_ACCESS_WRITE,
            &page_world);

        gpu_.pages.push_back(page);
        gpu_.pages_srv.push_back(page_view);
        gpu_.pages_depth.push_back(page_depth);
        gpu_.pages_dsv.push_back(page_dsv);
        gpu_.pages_rtv.push_back(page_rtv);
        gpu_.pages_world.push_back(page_world_buffer);
      }

      if (!gpu_.pages.empty()) {
        gpu_.data = gpu_.pages.front();
        gpu_.shader_resource_view = gpu_.pages_srv.front();
        gpu_.render_target_view = gpu_.pages_rtv.front();

      }

      LOG(INFO) << "[Canvas] Paged bitmap \"" << gpu_.name << "\" "
                << gpu_.size.x << "x" << gpu_.size.y << " into "
                << gpu_.PageCount() << " page(s) of " << gpu_.page_height
                << " rows (limit " << max_texture_size << ")";
    } else {
      // Setup GPU texture if necessary
      renderer::CreateTexture2D(
          *render_device, &gpu_.data, gpu_.name, texture_data,
          Diligent::USAGE_DEFAULT,
          Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE);
    }
  } else {
    // Bitmap size comes from the texture we were handed.
    gpu_.size =
        base::Vec2i(gpu_.data->GetDesc().Width, gpu_.data->GetDesc().Height);
  }

  // Depth-Stencil. Only page 0 gets one for now: it is the page the canvas
  // currently paints into (see the write paths).
  const base::Vec2i depth_size =
      gpu_.IsPaged() ? base::Vec2i(gpu_.size.x, gpu_.PageHeightAt(0))
                     : gpu_.size;
  renderer::CreateTexture2D(
      *render_device, &gpu_.depth, gpu_.name, depth_size,
      Diligent::USAGE_DEFAULT, Diligent::BIND_DEPTH_STENCIL,
      Diligent::CPU_ACCESS_NONE, Diligent::TEX_FORMAT_D24_UNORM_S8_UINT);

  // Setup access texture view
  gpu_.shader_resource_view =
      gpu_.data->GetDefaultView(Diligent::TEXTURE_VIEW_SHADER_RESOURCE);
  gpu_.render_target_view =
      gpu_.data->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
  gpu_.depth_stencil_view =
      gpu_.depth->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL);

  // Make bitmap draw transform
  // Make transform matrix uniform for discrete draw command
  renderer::WorldTransform world_matrix;
  renderer::MakeProjectionMatrix(world_matrix.projection,
                                 gpu_.size.Recast<float>());
  renderer::MakeIdentityMatrix(world_matrix.transform);

  // Per bitmap create uniform once
  Diligent::CreateUniformBuffer(
      *render_device, sizeof(world_matrix), "bitmap.binding.world",
      &gpu_.world_buffer, Diligent::USAGE_IMMUTABLE,
      Diligent::BIND_UNIFORM_BUFFER, Diligent::CPU_ACCESS_WRITE, &world_matrix);

  // Binding table
  auto& loader = context()->render.pipeline_loader;
  gpu_.base_binding = loader->base.CreateBinding();
  gpu_.color_binding = loader->color.CreateBinding();
  gpu_.blt_binding = loader->bitmapblt.CreateBinding();
  gpu_.clipblt_binding = loader->bitmapclipblt.CreateBinding();
  gpu_.hue_binding = loader->bitmaphue.CreateBinding();
}

void CanvasImpl::BeginPageTargetInternal(int32_t page) {
  saved_page_data_ = gpu_.data;
  saved_page_srv_ = gpu_.shader_resource_view;
  saved_page_rtv_ = gpu_.render_target_view;
  saved_page_dsv_ = gpu_.depth_stencil_view;
  saved_page_world_ = gpu_.world_buffer;

  gpu_.active_page = page;
  gpu_.data = gpu_.pages[page];
  gpu_.shader_resource_view = gpu_.pages_srv[page];
  gpu_.render_target_view = gpu_.pages_rtv[page];
  gpu_.depth_stencil_view = gpu_.pages_dsv[page];
  gpu_.world_buffer = gpu_.pages_world[page];
}

base::Vec2i CanvasImpl::CurrentTargetSizeInternal() {
  if (gpu_.data) {
    const auto& desc = gpu_.data->GetDesc();
    return base::Vec2i(static_cast<int32_t>(desc.Width),
                       static_cast<int32_t>(desc.Height));
  }
  return gpu_.size;
}

void CanvasImpl::EndPageTargetInternal() {
  gpu_.data = saved_page_data_;
  gpu_.shader_resource_view = saved_page_srv_;
  gpu_.render_target_view = saved_page_rtv_;
  gpu_.depth_stencil_view = saved_page_dsv_;
  gpu_.world_buffer = saved_page_world_;

  gpu_.active_page = -1;

  saved_page_data_.Release();
  saved_page_srv_.Release();
  saved_page_rtv_.Release();
  saved_page_dsv_.Release();
  saved_page_world_.Release();
}

void CanvasImpl::GPUBlendBlitTextureInternal(const base::Rect& dst_region,
                                             GPUData* src_texture,
                                             const base::Rect& src_region,
                                             int32_t blend_type,
                                             uint32_t opacity) {
  auto* scheduler = context()->canvas_scheduler;
  auto* render_context = scheduler->GetDiscreteRenderContext();

  // Custom blend blit pipeline
  auto* pipeline =
      context()->render.pipeline_states->bitmap.base[blend_type].RawPtr();

  // Norm opacity value
  base::Vec4 blend_alpha;
  blend_alpha.w = static_cast<float>(opacity) / 255.0f;

  // Make drawing vertices
  renderer::Quad transient_quad;
  renderer::Quad::SetTexCoordRect(&transient_quad, src_region,
                                  src_texture->size.Recast<float>());
  renderer::Quad::SetPositionRect(&transient_quad, dst_region);
  renderer::Quad::SetColor(&transient_quad, blend_alpha);
  scheduler->quad_batch().QueueWrite(render_context, &transient_quad);

  // Setup render target
  scheduler->SetupRenderTarget(gpu_.render_target_view, nullptr, false);

  // Setup uniform params
  gpu_.base_binding.u_transform->Set(gpu_.world_buffer);
  gpu_.base_binding.u_texture->Set(src_texture->shader_resource_view);

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.base_binding, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer = *scheduler->quad_batch();
  render_context->SetVertexBuffers(
      0, 1, &vertex_buffer, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->SetIndexBuffer(
      **context()->render.quad_index, 0,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Execute render command
  Diligent::DrawIndexedAttribs draw_indexed_attribs;
  draw_indexed_attribs.NumIndices = 6;
  draw_indexed_attribs.IndexType = context()->render.quad_index->GetIndexType();
  render_context->DrawIndexed(draw_indexed_attribs);
}

void CanvasImpl::GPUApproximateBlitTextureInternal(const base::Rect& dst_region,
                                                   GPUData* src_texture,
                                                   const base::Rect& src_region,
                                                   uint32_t opacity) {
  auto* scheduler = context()->canvas_scheduler;
  auto* render_context = scheduler->GetDiscreteRenderContext();

  // Clamp blit region
  const auto blit_region =
      base::MakeIntersect(dst_region, CurrentTargetSizeInternal());
  if (!blit_region.width || !blit_region.height)
    return;

  // Copy dst region to intermediate texture
  auto* intermediate_cache =
      scheduler->RequireBltCacheTexture(dst_region.Size());
  const base::Vec2 intermediate_size(intermediate_cache->GetDesc().Width,
                                     intermediate_cache->GetDesc().Height);

  // Reset render targets
  scheduler->SetupRenderTarget(nullptr, nullptr, false);

  // Blt target region to intermediate cache
  Diligent::Box copy_region;
  copy_region.MinX = blit_region.x;
  copy_region.MaxX = copy_region.MinX + blit_region.width;
  copy_region.MinY = blit_region.y;
  copy_region.MaxY = copy_region.MinY + blit_region.height;

  Diligent::CopyTextureAttribs copy_attribs;
  copy_attribs.pSrcTexture = gpu_.data;
  copy_attribs.pSrcBox = &copy_region;
  copy_attribs.SrcTextureTransitionMode =
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
  copy_attribs.pDstTexture = intermediate_cache;
  copy_attribs.DstTextureTransitionMode =
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
  copy_attribs.DstX = -std::min(0, dst_region.x);
  copy_attribs.DstY = -std::min(0, dst_region.y);
  render_context->CopyTexture(copy_attribs);
  renderer::ConsiderLoadDrain();


  // Custom blend blit pipeline
  auto* pipeline = context()->render.pipeline_states->bitmap.bitmapblt.RawPtr();

  // Make drawing vertices
  renderer::Quad transient_quad;
  renderer::Quad::SetPositionRect(&transient_quad, dst_region);
  renderer::Quad::SetTexCoordRect(&transient_quad, src_region,
                                  src_texture->size.Recast<float>());

  // Norm opacity value
  const float norm_opacity = static_cast<float>(opacity) / 255.0f;

  // Set dst texture uv
  const base::Vec2 dst_uv(dst_region.width / intermediate_size.x,
                          dst_region.height / intermediate_size.y);
  transient_quad.vertices[0].color = base::Vec4(0, 0, 0, norm_opacity);
  transient_quad.vertices[1].color = base::Vec4(dst_uv.x, 0, 0, norm_opacity);
  transient_quad.vertices[2].color =
      base::Vec4(dst_uv.x, dst_uv.y, 0, norm_opacity);
  transient_quad.vertices[3].color = base::Vec4(0, dst_uv.y, 0, norm_opacity);

  // Update vertices
  scheduler->quad_batch().QueueWrite(render_context, &transient_quad);

  // Setup render target
  scheduler->SetupRenderTarget(gpu_.render_target_view, nullptr, false);

  // Setup uniform params
  gpu_.blt_binding.u_transform->Set(gpu_.world_buffer);
  gpu_.blt_binding.u_texture->Set(src_texture->shader_resource_view);
  gpu_.blt_binding.u_dst_texture->Set(intermediate_cache->GetDefaultView(
      Diligent::TEXTURE_VIEW_SHADER_RESOURCE));

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.blt_binding, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer = *scheduler->quad_batch();
  render_context->SetVertexBuffers(
      0, 1, &vertex_buffer, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->SetIndexBuffer(
      **context()->render.quad_index, 0,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Execute render command
  Diligent::DrawIndexedAttribs draw_indexed_attribs;
  draw_indexed_attribs.NumIndices = 6;
  draw_indexed_attribs.IndexType = context()->render.quad_index->GetIndexType();
  render_context->DrawIndexed(draw_indexed_attribs);
}

void CanvasImpl::GPUClipTextureInternal(const base::Rect& dst_region,
                                        GPUData* src_texture,
                                        const base::Rect& src_region,
                                        GPUData* clip_texture) {
  auto* scheduler = context()->canvas_scheduler;
  auto* render_context = scheduler->GetDiscreteRenderContext();

  // Clamp blit region
  const auto blit_region =
      base::MakeIntersect(dst_region, CurrentTargetSizeInternal());
  if (!blit_region.width || !blit_region.height)
    return;

  // Copy dst region to intermediate texture
  auto* intermediate_cache =
      scheduler->RequireBltCacheTexture(dst_region.Size());
  const base::Vec2 intermediate_size(intermediate_cache->GetDesc().Width,
                                     intermediate_cache->GetDesc().Height);

  // Reset render targets
  scheduler->SetupRenderTarget(nullptr, nullptr, false);

  Diligent::Box copy_region;
  copy_region.MinX = blit_region.x;
  copy_region.MaxX = copy_region.MinX + blit_region.width;
  copy_region.MinY = blit_region.y;
  copy_region.MaxY = copy_region.MinY + blit_region.height;

  Diligent::CopyTextureAttribs copy_attribs;
  copy_attribs.pSrcTexture = gpu_.data;
  copy_attribs.pSrcBox = &copy_region;
  copy_attribs.SrcTextureTransitionMode =
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
  copy_attribs.pDstTexture = intermediate_cache;
  copy_attribs.DstTextureTransitionMode =
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
  copy_attribs.DstX = -std::min(0, dst_region.x);
  copy_attribs.DstY = -std::min(0, dst_region.y);
  render_context->CopyTexture(copy_attribs);
  renderer::ConsiderLoadDrain();

  // Custom blend blit pipeline
  auto* pipeline =
      context()->render.pipeline_states->bitmap.bitmapclip.RawPtr();

  // Make drawing vertices
  renderer::Quad transient_quad;
  renderer::Quad::SetPositionRect(&transient_quad, dst_region);
  renderer::Quad::SetTexCoordRect(&transient_quad, src_region,
                                  src_texture->size.Recast<float>());

  // Set dst texture uv, and stamp the quad corner into the otherwise unused colour .z:
  // the clip shader must not take the corner from SV_VertexID, which the Mali driver on
  // the target TV delivers wrong (every draw resolves the same index, so a quad could
  // sample one corner's clip alpha everywhere and vanish for a frame). See
  // kHLSL_BitmapClipBltRender_Vertex.
  const base::Vec2 dst_uv(dst_region.width / intermediate_size.x,
                          dst_region.height / intermediate_size.y);
  transient_quad.vertices[0].color = base::Vec4(0, 0, 0.0f, 0);
  transient_quad.vertices[1].color = base::Vec4(dst_uv.x, 0, 1.0f, 0);
  transient_quad.vertices[2].color = base::Vec4(dst_uv.x, dst_uv.y, 2.0f, 0);
  transient_quad.vertices[3].color = base::Vec4(0, dst_uv.y, 3.0f, 0);

  // Update vertices
  scheduler->quad_batch().QueueWrite(render_context, &transient_quad);

  // Setup render target
  scheduler->SetupRenderTarget(gpu_.render_target_view, nullptr, false);

  // Setup uniform params
  gpu_.clipblt_binding.u_transform->Set(gpu_.world_buffer);
  gpu_.clipblt_binding.u_texture->Set(src_texture->shader_resource_view);
  gpu_.clipblt_binding.u_clip_texture->Set(clip_texture->shader_resource_view);
  gpu_.clipblt_binding.u_dst_texture->Set(intermediate_cache->GetDefaultView(
      Diligent::TEXTURE_VIEW_SHADER_RESOURCE));

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.clipblt_binding,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer = *scheduler->quad_batch();
  render_context->SetVertexBuffers(
      0, 1, &vertex_buffer, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->SetIndexBuffer(
      **context()->render.quad_index, 0,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Execute render command
  Diligent::DrawIndexedAttribs draw_indexed_attribs;
  draw_indexed_attribs.NumIndices = 6;
  draw_indexed_attribs.IndexType = context()->render.quad_index->GetIndexType();
  render_context->DrawIndexed(draw_indexed_attribs);
}

void CanvasImpl::GPUFetchTexturePixelsDataInternal() {
  auto* scheduler = context()->canvas_scheduler;
  auto& render_device = *scheduler->GetRenderDevice();
  auto* render_context = scheduler->GetDiscreteRenderContext();

  // Reset render target
  scheduler->SetupRenderTarget(nullptr, nullptr, false);

  // A paged bitmap reads back page by page into the rows it occupies.
  if (gpu_.IsPaged()) {
    for (int32_t page = 0; page < gpu_.PageCount(); ++page) {
      Diligent::ITexture* page_texture = gpu_.pages[page].RawPtr();
      if (!page_texture)
        continue;

      Diligent::TextureDesc stage_buffer_desc = page_texture->GetDesc();
      stage_buffer_desc.Name = "bitmap.readstage.page";
      stage_buffer_desc.Usage = Diligent::USAGE_STAGING;
      stage_buffer_desc.BindFlags = Diligent::BIND_NONE;
      stage_buffer_desc.CPUAccessFlags = Diligent::CPU_ACCESS_READ;

      RRefPtr<Diligent::ITexture> stage_read_buffer;
      render_device->CreateTexture(stage_buffer_desc, nullptr,
                                   &stage_read_buffer);
      if (!stage_read_buffer)
        continue;

      Diligent::CopyTextureAttribs copy_texture_attribs(
          page_texture, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
          stage_read_buffer.RawPtr(),
          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
      render_context->CopyTexture(copy_texture_attribs);
      render_context->WaitForIdle();

      Diligent::MappedTextureSubresource MappedData;
      render_context->MapTextureSubresource(
          stage_read_buffer, 0, 0, Diligent::MAP_READ,
          Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr, MappedData);

      if (MappedData.pData) {
        const int32_t top = gpu_.PageTop(page);
        const int32_t rows = std::min(gpu_.PageHeightAt(page),
                                      surface_cache_->h - top);
        uint8_t* dst_data = static_cast<uint8_t*>(surface_cache_->pixels) +
                            static_cast<size_t>(top) * surface_cache_->pitch;
        uint8_t* src_data = static_cast<uint8_t*>(MappedData.pData);
        for (int32_t i = 0; i < rows; ++i) {
          std::memcpy(dst_data, src_data, surface_cache_->pitch);
          dst_data += surface_cache_->pitch;
          src_data += MappedData.Stride;
        }
      }

      render_context->UnmapTextureSubresource(stage_read_buffer, 0, 0);
    }

    return;
  }

  // Create transient read stage texture
  Diligent::TextureDesc stage_buffer_desc = gpu_.data->GetDesc();
  stage_buffer_desc.Name = "bitmap.readstage.buffer";
  stage_buffer_desc.Usage = Diligent::USAGE_STAGING;
  stage_buffer_desc.BindFlags = Diligent::BIND_NONE;
  stage_buffer_desc.CPUAccessFlags = Diligent::CPU_ACCESS_READ;

  RRefPtr<Diligent::ITexture> stage_read_buffer;
  render_device->CreateTexture(stage_buffer_desc, nullptr, &stage_read_buffer);

  // Copy textrue to temp buffer
  Diligent::CopyTextureAttribs copy_texture_attribs(
      gpu_.data.RawPtr(), Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
      stage_read_buffer.RawPtr(),
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->CopyTexture(copy_texture_attribs);
  render_context->WaitForIdle();

  // Read buffer data from mapping buffer
  Diligent::MappedTextureSubresource MappedData;
  render_context->MapTextureSubresource(
      stage_read_buffer, 0, 0, Diligent::MAP_READ,
      Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr, MappedData);

  if (MappedData.pData) {
    // Fill in memory surface
    uint8_t* dst_data = static_cast<uint8_t*>(surface_cache_->pixels);
    uint8_t* src_data = static_cast<uint8_t*>(MappedData.pData);
    for (int32_t i = 0; i < surface_cache_->h; ++i) {
      std::memcpy(dst_data, src_data, surface_cache_->pitch);
      dst_data += surface_cache_->pitch;
      src_data += MappedData.Stride;
    }
  }

  // End of mapping
  render_context->UnmapTextureSubresource(stage_read_buffer, 0, 0);
}

void CanvasImpl::GPUCanvasClearInternal() {
  // Clear all data in texture
  context()->canvas_scheduler->SetupRenderTarget(gpu_.render_target_view,
                                                 nullptr, true);
}

void CanvasImpl::GPUCanvasGradientFillRectInternal(const base::Rect& region,
                                                   const base::Vec4& color1,
                                                   const base::Vec4& color2,
                                                   bool vertical) {
  auto* scheduler = context()->canvas_scheduler;
  auto* render_context = scheduler->GetDiscreteRenderContext();

  // Pipeline state
  auto* pipeline = context()->render.pipeline_states->bitmap.color.RawPtr();

  // Make transient vertices data
  renderer::Quad transient_quad;
  renderer::Quad::SetPositionRect(&transient_quad, region);
  if (vertical) {
    transient_quad.vertices[0].color = color1;
    transient_quad.vertices[1].color = color1;
    transient_quad.vertices[2].color = color2;
    transient_quad.vertices[3].color = color2;
  } else {
    transient_quad.vertices[0].color = color1;
    transient_quad.vertices[1].color = color2;
    transient_quad.vertices[2].color = color2;
    transient_quad.vertices[3].color = color1;
  }
  scheduler->quad_batch().QueueWrite(render_context, &transient_quad);

  // Setup render target
  scheduler->SetupRenderTarget(gpu_.render_target_view, nullptr, false);

  // Setup uniform params
  gpu_.color_binding.u_transform->Set(gpu_.world_buffer);

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.color_binding, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer = *scheduler->quad_batch();
  render_context->SetVertexBuffers(
      0, 1, &vertex_buffer, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->SetIndexBuffer(
      **context()->render.quad_index, 0,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Execute render command
  Diligent::DrawIndexedAttribs draw_indexed_attribs;
  draw_indexed_attribs.NumIndices = 6;
  draw_indexed_attribs.IndexType = context()->render.quad_index->GetIndexType();
  render_context->DrawIndexed(draw_indexed_attribs);
}

void CanvasImpl::GPUCanvasDrawTextSurfaceInternal(const base::Rect& region,
                                                  SDL_Surface* text,
                                                  float opacity,
                                                  int32_t align) {
  auto* scheduler = context()->canvas_scheduler;
  auto& render_device = *scheduler->GetRenderDevice();
  auto* render_context = scheduler->GetDiscreteRenderContext();


  // Reset text upload stage buffer if need. A paged draw runs one pass per page,
  // so the surface handed in changes height from slice to slice: round the cache
  // up instead of rebuilding it for every slice (re-creating a texture in the
  // middle of a command stream is what the backends dislike).
  const int32_t required_cache_width = std::max<int32_t>(text->w, 256);
  int32_t required_cache_height = std::max<int32_t>(text->h, 256);
  if (gpu_.IsPaged()) {
    // A paged bitmap draws the same text once per page and the slices differ
    // in height, so size the cache for the tallest slice straight away. Letting
    // it grow inside the per-page loop would create a texture mid-render.
    required_cache_height =
        std::max<int32_t>(required_cache_height,
                          std::min<int32_t>(
                              gpu_.page_height,
                              context()->render_device->MaxTextureSize()));
  }
  if (!gpu_.text_cache_texture ||
      gpu_.text_cache_size.x < required_cache_width ||
      gpu_.text_cache_size.y < required_cache_height) {
    gpu_.text_cache_size.x =
        std::max<int32_t>(gpu_.text_cache_size.x, required_cache_width);
    gpu_.text_cache_size.y =
        std::max<int32_t>(gpu_.text_cache_size.y, required_cache_height);

    renderer::DeferredTextureRelease(gpu_.text_cache_texture);
    renderer::CreateTexture2D(*render_device, &gpu_.text_cache_texture,
                              "textdraw.cache", gpu_.text_cache_size);
  }

  // Update text texture cache
  Diligent::TextureSubResData texture_sub_res_data(text->pixels, text->pitch);
  Diligent::Box box(0, text->w, 0, text->h);

  URGE_GPU_AUDIT("canvas:update-text-texture", true);
  render_context->UpdateTexture(
      gpu_.text_cache_texture, 0, 0, box, texture_sub_res_data,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);


  // Render text stage data to current texture
  base::Vec4 blend_alpha;
  blend_alpha.w = opacity / 255.0f;

  int32_t align_x = region.x,
          align_y = region.y + (region.height - text->h) / 2;
  switch (align) {
    default:
    case 0:  // Left
      break;
    case 1:  // Center
      align_x += (region.width - text->w) / 2;
      break;
    case 2:  // Right
      align_x += region.width - text->w;
      break;
  }

  // Assemble attributes
  float zoom_x = static_cast<float>(region.width) / text->w;
  zoom_x = std::min(zoom_x, 1.0f);
  const base::Rect compose_position(align_x, align_y, text->w * zoom_x,
                                    text->h);
  const base::Vec2i text_cache_size(gpu_.text_cache_texture->GetDesc().Width,
                                    gpu_.text_cache_texture->GetDesc().Height);
  const base::Vec2i text_surface_size(text->w, text->h);

  // Release text surface
  SDL_DestroySurface(text);

  // Clamp blit region
  const base::Vec2i text_target_size = CurrentTargetSizeInternal();
  const auto blit_region =
      base::MakeIntersect(compose_position, text_target_size);
  if (!blit_region.width || !blit_region.height)
    return;

  // Copy dst region to intermediate texture
  auto* intermediate_cache =
      scheduler->RequireBltCacheTexture(compose_position.Size());

  // Reset render targets
  scheduler->SetupRenderTarget(nullptr, nullptr, false);

  Diligent::Box copy_region;
  copy_region.MinX = blit_region.x;
  copy_region.MaxX = copy_region.MinX + blit_region.width;
  copy_region.MinY = blit_region.y;
  copy_region.MaxY = copy_region.MinY + blit_region.height;

  Diligent::CopyTextureAttribs copy_attribs;
  copy_attribs.pSrcTexture = gpu_.data;
  copy_attribs.pSrcBox = &copy_region;
  copy_attribs.SrcTextureTransitionMode =
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
  copy_attribs.pDstTexture = intermediate_cache;
  copy_attribs.DstTextureTransitionMode =
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
  copy_attribs.DstX = -std::min(0, compose_position.x);
  copy_attribs.DstY = -std::min(0, compose_position.y);
  render_context->CopyTexture(copy_attribs);
  renderer::ConsiderLoadDrain();

  // Custom blend blit pipeline
  auto* pipeline = context()->render.pipeline_states->bitmap.bitmapblt.RawPtr();

  // Make drawing vertices
  renderer::Quad transient_quad;
  renderer::Quad::SetPositionRect(&transient_quad, compose_position);
  renderer::Quad::SetTexCoordRect(&transient_quad,
                                  base::Rect(text_surface_size),
                                  text_cache_size.Recast<float>());

  // Norm opacity value
  const float norm_opacity = static_cast<float>(opacity) / 255.0f;

  // Set dst texture uv
  const base::Vec2 dst_uv(static_cast<float>(compose_position.width) /
                              intermediate_cache->GetDesc().Width,
                          static_cast<float>(compose_position.height) /
                              intermediate_cache->GetDesc().Height);
  transient_quad.vertices[0].color = base::Vec4(0, 0, 0, norm_opacity);
  transient_quad.vertices[1].color = base::Vec4(dst_uv.x, 0, 0, norm_opacity);
  transient_quad.vertices[2].color =
      base::Vec4(dst_uv.x, dst_uv.y, 0, norm_opacity);
  transient_quad.vertices[3].color = base::Vec4(0, dst_uv.y, 0, norm_opacity);

  // Update vertices
  scheduler->quad_batch().QueueWrite(render_context, &transient_quad);

  // Setup render target
  scheduler->SetupRenderTarget(gpu_.render_target_view, nullptr, false);

  // Setup uniform params
  gpu_.blt_binding.u_transform->Set(gpu_.world_buffer);
  gpu_.blt_binding.u_texture->Set(gpu_.text_cache_texture->GetDefaultView(
      Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
  gpu_.blt_binding.u_dst_texture->Set(intermediate_cache->GetDefaultView(
      Diligent::TEXTURE_VIEW_SHADER_RESOURCE));

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.blt_binding, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer = *scheduler->quad_batch();
  render_context->SetVertexBuffers(
      0, 1, &vertex_buffer, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->SetIndexBuffer(
      **context()->render.quad_index, 0,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Execute render command
  Diligent::DrawIndexedAttribs draw_indexed_attribs;
  draw_indexed_attribs.NumIndices = 6;
  draw_indexed_attribs.IndexType = context()->render.quad_index->GetIndexType();
  render_context->DrawIndexed(draw_indexed_attribs);
}

void CanvasImpl::GPUCanvasHueChange(int32_t hue) {
  auto* scheduler = context()->canvas_scheduler;
  auto* render_context = scheduler->GetDiscreteRenderContext();

  // Intermediate layer. A paged bitmap runs one pass per page, so the pass
  // covers the page that is currently bound, not the whole bitmap.
  const base::Vec2i hue_target_size = CurrentTargetSizeInternal();
  auto* intermediate_effect_layer =
      scheduler->RequireBltCacheTexture(hue_target_size);

  // Pipeline state
  auto* pipeline = context()->render.pipeline_states->bitmap.bitmaphue.RawPtr();

  // Copy current texture to stage intermediate texture
  scheduler->SetupRenderTarget(nullptr, nullptr, false);

  Diligent::CopyTextureAttribs copy_texture_attribs;
  copy_texture_attribs.pSrcTexture = gpu_.data;
  copy_texture_attribs.pSrcBox = nullptr;
  copy_texture_attribs.SrcTextureTransitionMode =
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
  copy_texture_attribs.pDstTexture = intermediate_effect_layer;
  copy_texture_attribs.DstX = 0;
  copy_texture_attribs.DstY = 0;
  copy_texture_attribs.DstTextureTransitionMode =
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
  render_context->CopyTexture(copy_texture_attribs);

  // Make transient vertices
  renderer::Quad transient_quad;
  renderer::Quad::SetPositionRect(&transient_quad,
                                  base::RectF(-1.0f, 1.0f, 2.0f, -2.0f));
  renderer::Quad::SetTexCoordRect(
      &transient_quad, hue_target_size.Recast<float>(),
      base::Vec2(intermediate_effect_layer->GetDesc().Width,
                 intermediate_effect_layer->GetDesc().Height));
  renderer::Quad::SetColor(&transient_quad, base::Vec4(hue / 360.0f));
  scheduler->quad_batch().QueueWrite(render_context, &transient_quad);

  // Setup render target
  scheduler->SetupRenderTarget(gpu_.render_target_view, nullptr, false);

  // Setup uniform params
  gpu_.hue_binding.u_texture->Set(intermediate_effect_layer->GetDefaultView(
      Diligent::TEXTURE_VIEW_SHADER_RESOURCE));

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.hue_binding, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer = *scheduler->quad_batch();
  render_context->SetVertexBuffers(
      0, 1, &vertex_buffer, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->SetIndexBuffer(
      **context()->render.quad_index, 0,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Execute render command
  Diligent::DrawIndexedAttribs draw_indexed_attribs;
  draw_indexed_attribs.NumIndices = 6;
  draw_indexed_attribs.IndexType = context()->render.quad_index->GetIndexType();
  render_context->DrawIndexed(draw_indexed_attribs);
}

}  // namespace content
