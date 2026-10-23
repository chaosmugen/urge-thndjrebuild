// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/screen/renderscreen_impl.h"

#include <sstream>

#if defined(OS_ANDROID)
#include <sys/system_properties.h>
#endif
#include <cstring>
#include <unordered_map>
#include <vector>

#include "SDL3/SDL_events.h"
#include "SDL3/SDL_timer.h"

#include "content/render/sprite_batch.h"
#include "content/render/window_impl.h"
#include "renderer/device/gpu_audit.h"
#include "magic_enum/magic_enum.hpp"

#include "Common/interface/BasicMath.hpp"
#include "Graphics/GraphicsAccessories/interface/GraphicsAccessories.hpp"
#if defined(OS_ANDROID)
#include "Graphics/GraphicsEngineOpenGL/interface/RenderDeviceGLES.h"
#endif

#include "base/debug/logging.h"
#include "content/canvas/canvas_scheduler.h"
#include "content/common/rect_impl.h"
#include "content/context/execution_context.h"
#include "content/gpu/buffer_impl.h"
#include "content/gpu/device_context_impl.h"
#include "content/gpu/render_device_impl.h"
#include "content/profile/command_ids.h"
#include "renderer/utils/texture_utils.h"
#include "renderer/device/gpu_driver.h"

// Defined further down. Marker: URGE_DIAG_GATE
extern "C" bool UrgeDiagEnabled();


// TEMPORARY offscreen-then-copy experiment. Marker: URGE_OFFSCREEN
namespace content {


///////////////////////////////////////////////////////////////////////////////
// RenderScreenImpl Implement

RenderScreenImpl::RenderScreenImpl(ExecutionContext* execution_context,
                                   uint32_t frame_rate)
    : EngineObject(execution_context),
      limiter_(frame_rate),
      frozen_(false),
      brightness_(255),
      frame_count_(0),
      frame_rate_(frame_rate) {
  // Setup render device on render thread if possible
  GPUCreateGraphicsHostInternal();

  // Hand the texture factory what the load drain needs: the context it submits
  // through, and whether the backend is the Vulkan one the drain works around.
  renderer::SetLoadDrainContext(context()->primary_render_context);
  renderer::SetVulkanBackend(
      (*context()->render_device)->GetDeviceInfo().Type ==
      Diligent::RENDER_DEVICE_TYPE_VULKAN);

  // Initialize fps limiter
  limiter_.Reset();
}

RenderScreenImpl::~RenderScreenImpl() = default;

void RenderScreenImpl::PresentScreenBuffer(
    Diligent::ImGuiDiligentRenderer* gui_renderer) {
  // Present to screen surface
  GPUPresentScreenBufferInternal(context()->primary_render_context,
                                 gui_renderer);
}

void RenderScreenImpl::ResetFPSCounter() {
  limiter_.Reset();
}

void RenderScreenImpl::CreateButtonGUISettings() {
  const auto& adapter_info = (*context()->render_device)->GetAdapterInfo();
  const auto& device_info = (*context()->render_device)->GetDeviceInfo();
  auto& settings_profile = *context()->engine_profile;

  if (ImGui::CollapsingHeader(
          context()
              ->i18n_profile->GetI18NString(IDS_SETTINGS_GRAPHICS, "Graphics")
              .c_str())) {
    ImGui::TextWrapped(
        "%s: %s (%d.%d)",
        context()
            ->i18n_profile->GetI18NString(IDS_GRAPHICS_BACKEND, "Backend")
            .c_str(),
        Diligent::GetRenderDeviceTypeString(device_info.Type),
        device_info.APIVersion.Major, device_info.APIVersion.Minor);
    ImGui::Separator();
    ImGui::TextWrapped(
        "%s: %s (PCI: %X)",
        context()
            ->i18n_profile->GetI18NString(IDS_GRAPHICS_RENDERER, "Renderer")
            .c_str(),
        adapter_info.Description, adapter_info.DeviceId);
    ImGui::Separator();
    ImGui::TextWrapped(
        "%s: %s (PCI: %X)",
        context()
            ->i18n_profile->GetI18NString(IDS_GRAPHICS_VENDOR, "Vendor")
            .c_str(),
        magic_enum::enum_name(adapter_info.Vendor).data() +
            sizeof("ADAPTER_VENDOR"),
        adapter_info.VendorId);
    ImGui::Separator();

    // Keep Ratio
    ImGui::Checkbox(
        context()
            ->i18n_profile->GetI18NString(IDS_GRAPHICS_KEEP_RATIO, "Keep Ratio")
            .c_str(),
        &settings_profile.keep_ratio);

    // Skip Frame
    ImGui::Checkbox(
        context()
            ->i18n_profile->GetI18NString(IDS_GRAPHICS_SKIP_FRAME, "Skip Frame")
            .c_str(),
        &settings_profile.allow_skip_frame);

    // Fullscreen
    bool is_fullscreen = context()->window->IsFullscreen(),
         last_fullscreen = is_fullscreen;
    ImGui::Checkbox(
        context()
            ->i18n_profile->GetI18NString(IDS_GRAPHICS_FULLSCREEN, "Fullscreen")
            .c_str(),
        &is_fullscreen);
    if (last_fullscreen != is_fullscreen)
      context()->window->SetFullscreen(is_fullscreen);

    // Background running
    ImGui::Checkbox(context()
                        ->i18n_profile
                        ->GetI18NString(IDS_GRAPHICS_BACKGROUND_RUNNING,
                                        "Background Running")
                        .c_str(),
                    &settings_profile.background_running);
  }
}

void RenderScreenImpl::Update(ExceptionState& exception_state) {
#if defined(OS_ANDROID)
  // Sync pending lifecycle events BEFORE the render decision: while the
  // scripts were loading no events were pumped, so a background/foreground
  // transition from that window only surfaces here. Without this sync the
  // first frame submits GPU commands on a surface that is already gone.
  SDL_PumpEvents();

  // Traced only while the surface is gone (see PollEventQueueInternal).
  const bool traced = !context()->render_device->IsSurfaceValid();
  if (traced)
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[Graphics] update enter (surface invalid)";
#endif  //! OS_ANDROID

  const bool frozen_render = frozen_;
  const bool need_skip_frame = limiter_.RequireFrameSkip() &&
                               context()->engine_profile->allow_skip_frame;

  // Rotate the screen targets before rendering. After the swap the scene renders into the
  // buffer that holds the frame before last, while the composite below samples the one
  // rendered on the previous frame - so the frame being read is never the frame being
  // written. The failure this addresses was measured as a frame that is part old and part
  // new (the top 78% of the screen from one frame, the bottom from another, identical again
  // on the next frame), which is exactly what compositing a target that is still being
  // rendered produces, and which no amount of synchronisation on this driver removed.
  // Gated and OFF by default: measured with the row probe it made things worse (14 events
  // against 3 with the single buffer, four of them an identical 519-magnitude band on rows
  // 33..39 - the bottom of the picture). The most likely reading is that this engine does
  // not write every part of the screen target every frame, so alternating targets makes
  // those parts alternate between two buffers instead of fixing anything.
  {
    static int32_t s_db_poll = 0;
    static bool s_db_enabled = false;
    if (++s_db_poll >= 30) {
      s_db_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "screenbuf2.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_db_enabled = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/screenbuf2.txt");
      }
    }
    if (s_db_enabled && gpu_.screen_buffer && gpu_.screen_buffer_prev)
      std::swap(gpu_.screen_buffer, gpu_.screen_buffer_prev);
  }

  if (!frozen_render && !need_skip_frame) {
    // Render a frame and push into render queue
    // This function only encodes the render commands
    RenderFrameInternal(gpu_.screen_buffer, gpu_.screen_depth_stencil);
  }

  if (need_skip_frame)
    limiter_.Reset();

  // Process frame delay
  // This calling will yield to event coroutine and present
  FrameProcessInternal(gpu_.screen_buffer);


#if defined(OS_ANDROID)
  if (traced)
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[Graphics] update leave (surface invalid)";
#endif  //! OS_ANDROID
}

void RenderScreenImpl::Wait(uint32_t duration,
                            ExceptionState& exception_state) {
  for (uint32_t i = 0; i < duration; ++i)
    Update(exception_state);
}

void RenderScreenImpl::FadeOut(uint32_t duration,
                               ExceptionState& exception_state) {
  duration = std::max(duration, 1u);

  float current_brightness = static_cast<float>(brightness_);
  for (uint32_t i = 0; i < duration; ++i) {
    brightness_ = current_brightness -
                  current_brightness * ((i + 1) / static_cast<float>(duration));
    if (frozen_) {
      FrameProcessInternal(gpu_.frozen_buffer);
    } else {
      Update(exception_state);
    }
  }
}

void RenderScreenImpl::FadeIn(uint32_t duration,
                              ExceptionState& exception_state) {
  duration = std::max(duration, 1u);

  float current_brightness = static_cast<float>(brightness_);
  float diff = 255.0f - current_brightness;
  for (uint32_t i = 0; i < duration; ++i) {
    brightness_ =
        current_brightness + diff * ((i + 1) / static_cast<float>(duration));

    if (frozen_) {
      FrameProcessInternal(gpu_.frozen_buffer);
    } else {
      Update(exception_state);
    }
  }
}

void RenderScreenImpl::Freeze(ExceptionState& exception_state) {
  if (!frozen_) {
    // Get frozen scene snapshot for transition
    RenderFrameInternal(gpu_.frozen_buffer, gpu_.frozen_depth_stencil);

    // Set forzen flag for blocking frame update
    frozen_ = true;
  }
}

void RenderScreenImpl::Transition(ExceptionState& exception_state) {
  Transition(10, std::string(), 40, exception_state);
}

void RenderScreenImpl::Transition(uint32_t duration,
                                  ExceptionState& exception_state) {
  Transition(duration, std::string(), 40, exception_state);
}

void RenderScreenImpl::Transition(uint32_t duration,
                                  const std::string& filename,
                                  ExceptionState& exception_state) {
  Transition(duration, filename, 40, exception_state);
}

void RenderScreenImpl::Transition(uint32_t duration,
                                  const std::string& filename,
                                  uint32_t vague,
                                  ExceptionState& exception_state) {
  scoped_refptr<CanvasImpl> transition_mapping = nullptr;
  if (!filename.empty())
    transition_mapping =
        CanvasImpl::Create(context(), filename, exception_state);
  if (exception_state.HadException())
    return;

  TransitionWithBitmap(duration, transition_mapping, vague, exception_state);
}

void RenderScreenImpl::TransitionWithBitmap(uint32_t duration,
                                            scoped_refptr<Bitmap> bitmap,
                                            uint32_t vague,
                                            ExceptionState& exception_state) {
  if (!frozen_)
    return;

  if (duration > 0) {
    // Reset brightness
    brightness_ = 255;
    // Transition attribute
    const float vague_norm = std::clamp<uint32_t>(vague, 1, 256) / 256.0f;

    // Derive transition mapping if available
    scoped_refptr<CanvasImpl> mapping_bitmap = CanvasImpl::FromBitmap(bitmap);
    BitmapTexture* texture_agent =
        Disposable::IsValid(mapping_bitmap.get()) ? **mapping_bitmap : nullptr;
    Diligent::ITextureView* transition_mapping =
        texture_agent ? texture_agent->shader_resource_view.RawPtr() : nullptr;

    // Get current scene snapshot for transition
    auto* render_context = context()->primary_render_context;
    RenderFrameInternal(gpu_.transition_buffer, gpu_.transition_depth_stencil);

    // Transition render loop
    for (uint32_t i = 0; i < duration; ++i) {
      // Norm transition progress
      float progress = (i + 1) * (1.0f / duration);

      // Render per transition frame
      if (transition_mapping)
        GPURenderVagueTransitionFrameInternal(
            render_context, transition_mapping, progress, vague_norm);
      else
        GPURenderAlphaTransitionFrameInternal(render_context, progress);

      // Present to screen
      FrameProcessInternal(gpu_.screen_buffer);
    }
  } else {
    // Update frame as common
    Update(exception_state);
  }

  // Transition process complete
  frozen_ = false;
}

scoped_refptr<Bitmap> RenderScreenImpl::SnapToBitmap(
    ExceptionState& exception_state) {
  scoped_refptr<CanvasImpl> target =
      CanvasImpl::Create(context(), context()->resolution, exception_state);
  BitmapTexture* texture_agent =
      Disposable::IsValid(target.get()) ? **target : nullptr;

  if (texture_agent) {
    // Invalidate render target bitmap cache
    target->InvalidateSurfaceCache();

    // Execute rendering
    RenderFrameInternal(texture_agent->data, texture_agent->depth);
  }

  return target;
}

void RenderScreenImpl::FrameReset(ExceptionState& exception_state) {
  limiter_.Reset();
}

uint32_t RenderScreenImpl::Width(ExceptionState& exception_state) {
  return context()->resolution.x;
}

uint32_t RenderScreenImpl::Height(ExceptionState& exception_state) {
  return context()->resolution.y;
}

void RenderScreenImpl::ResizeScreen(uint32_t width,
                                    uint32_t height,
                                    ExceptionState& exception_state) {
  // Debounce: rebuilding the screen buffers frees and reallocates a whole
  // device-local memory page per call. Something triggers this every other
  // frame (the GPU audit shows ~22 rebuilds/s), which shreds the driver's
  // memory manager and eventually crashes it. Same size = no-op.
  if (context()->resolution == base::Vec2i(width, height)) {
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[Graphics] resize_screen(" << width << ", " << height
              << ") ignored: resolution unchanged.";
    return;
  }

  if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
  LOG(INFO) << "[Graphics] resize_screen(" << width << ", " << height
            << ") from " << context()->resolution.x << "x"
            << context()->resolution.y;

  context()->resolution = base::Vec2i(width, height);
  GPUResetScreenBufferInternal();
}

void RenderScreenImpl::Reset(ExceptionState& exception_state) {
  /* Reset freeze */
  frozen_ = false;

  /* Reset attribute */
  frame_rate_ = context()->engine_profile->api_version ==
                        ContentProfile::APIVersion::RGSS1
                    ? 40
                    : 60;
  brightness_ = 255;
  FrameReset(exception_state);
}

void RenderScreenImpl::MoveWindow(int32_t x,
                                  int32_t y,
                                  int32_t width,
                                  int32_t height,
                                  ExceptionState& exception_state) {
  auto* window = context()->window->AsSDLWindow();
  SDL_SetWindowSize(window, width, height);
  SDL_SetWindowPosition(window, x, y);
}

scoped_refptr<Rect> RenderScreenImpl::GetWindowRect(
    ExceptionState& exception_state) {
  auto* window = context()->window->AsSDLWindow();
  base::Rect window_rect;
  SDL_GetWindowPosition(window, &window_rect.x, &window_rect.y);
  SDL_GetWindowSize(window, &window_rect.width, &window_rect.height);
  return base::MakeRefCounted<RectImpl>(window_rect);
}

uint32_t RenderScreenImpl::GetDisplayID(ExceptionState& exception_state) {
  return SDL_GetDisplayForWindow(context()->window->AsSDLWindow());
}

void RenderScreenImpl::SetWindowIcon(scoped_refptr<Bitmap> icon,
                                     ExceptionState& exception_state) {
  scoped_refptr<CanvasImpl> data = CanvasImpl::FromBitmap(icon);
  SDL_Surface* icon_surface = data->RequireMemorySurface();
  SDL_SetWindowIcon(context()->window->AsSDLWindow(), icon_surface);
}

int32_t RenderScreenImpl::GetMaxTextureSize(ExceptionState& exception_state) {
  return context()->render_device->MaxTextureSize();
}

scoped_refptr<GPURenderDevice> RenderScreenImpl::GetRenderDevice(
    ExceptionState& exception_state) {
  return base::MakeRefCounted<RenderDeviceImpl>(context(),
                                                **context()->render_device);
}

scoped_refptr<GPUDeviceContext> RenderScreenImpl::GetImmediateContext(
    ExceptionState& exception_state) {
  return base::MakeRefCounted<DeviceContextImpl>(
      context(), context()->primary_render_context);
}

scoped_refptr<GPUBuffer> RenderScreenImpl::GetGenericQuadIndexBuffer(
    uint32_t draw_quad_count,
    ExceptionState& exception_state) {
  auto& quad_index = context()->render.quad_index;
  quad_index->Allocate(draw_quad_count);
  return base::MakeRefCounted<BufferImpl>(context(), **quad_index);
}

GPU::ValueType RenderScreenImpl::GetInternalIndexType(
    ExceptionState& exception_state) {
  auto& quad_index = context()->render.quad_index;
  return static_cast<GPU::ValueType>(quad_index->GetIndexType());
}

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    FrameRate,
    uint32_t,
    RenderScreenImpl,
    { return frame_rate_; },
    {
      frame_rate_ = value;
      limiter_.SetFrameRate(frame_rate_);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    FrameCount,
    uint32_t,
    RenderScreenImpl,
    { return frame_count_; },
    { frame_count_ = value; });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Brightness,
    uint32_t,
    RenderScreenImpl,
    { return brightness_; },
    { brightness_ = std::clamp<uint32_t>(value, 0, 255); });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    VSync,
    uint32_t,
    RenderScreenImpl,
    { return context()->engine_profile->vsync; },
    {
      context()->engine_profile->vsync = std::clamp<uint32_t>(value, 0, 255);
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Fullscreen,
    bool,
    RenderScreenImpl,
    { return context()->window->IsFullscreen(); },
    { context()->window->SetFullscreen(value); });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Skipframe,
    bool,
    RenderScreenImpl,
    { return context()->engine_profile->allow_skip_frame; },
    { context()->engine_profile->allow_skip_frame = value; });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    KeepRatio,
    bool,
    RenderScreenImpl,
    { return context()->engine_profile->keep_ratio; },
    { context()->engine_profile->keep_ratio = value; });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    BackgroundRunning,
    bool,
    RenderScreenImpl,
    { return context()->engine_profile->background_running; },
    { context()->engine_profile->background_running = value; });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Ox,
    int32_t,
    RenderScreenImpl,
    { return origin_.x; },
    {
      origin_.x = value;
      GPUUpdateScreenWorldInternal();
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    Oy,
    int32_t,
    RenderScreenImpl,
    { return origin_.y; },
    {
      origin_.y = value;
      GPUUpdateScreenWorldInternal();
    });

URGE_DEFINE_OVERRIDE_ATTRIBUTE(
    WindowTitle,
    std::string,
    RenderScreenImpl,
    { return SDL_GetWindowTitle(context()->window->AsSDLWindow()); },
    { SDL_SetWindowTitle(context()->window->AsSDLWindow(), value.c_str()); });

// TEMP: pixel probe state at file scope, so the probe can be called at the end of the
// frame as well. Marker: URGE_PROBEEND
static int32_t s_probe_poll = 0;
static bool s_probe_enabled = false;
static Diligent::RefCntAutoPtr<Diligent::ITexture> s_probe_stage[2];
static int32_t s_probe_index = 0;
static uint32_t s_probe_ok = 0;
static uint32_t s_probe_busy = 0;

void RenderScreenImpl::FrameProcessInternal(
    Diligent::ITexture* present_target) {
  // Increase frame render count
  ++frame_count_;

  // TEMPORARY: which screen buffer this frame presents, and with what brightness. The
  // engine keeps a main, a frozen (snapshot) and a transition buffer; a one-frame pixel
  // change that leaves the draw data identical is exactly what presenting / compositing a
  // different buffer for a single frame looks like, so every frame records its source.
  if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
  LOG(INFO) << "[PresentPath] f=" << frame_count_ << " src="
            << (present_target == gpu_.screen_buffer.RawPtr()
                    ? "main"
                    : present_target == gpu_.frozen_buffer.RawPtr()
                          ? "frozen"
                          : present_target == gpu_.transition_buffer.RawPtr()
                                ? "transition"
                                : "other")
            << " brightness=" << brightness_
            // Whether the deferred-release graveyard is on (renderer::DeferredTextureRelease),
            // so a session played by hand can be split by it afterwards without the host
            // having to flip switches in step with it. The rotating arms are gone: every file
            // computed its own arm from the frame count and mapped it to its own measure, so
            // several of them ran at once and the numbers could not be read - a keep-alive
            // arm, for instance, also carried the canvas and present drains.
            << " ring=" << (renderer::DeferredReleaseEnabled() ? 1 : 0);

  // TEMPORARY pixel probe. Every flicker this project has recorded shows one frame in
  // which a large region of the picture is missing while every CPU-side counter stays
  // correct, so the only thing that can tie the event to a frame number is the pixels the
  // GPU actually produced. While urge_debug/pixprobe.txt is nonzero, a 6x4 grid of 16x16
  // blocks covering the whole frame is averaged and logged on EVERY frame. Logging on
  // change alone was wrong: the test that looked for a value returning on the next line
  // can never fire while the scene is animating, which is exactly when the flicker
  // happens. With one line per frame the event is found afterwards as a frame whose blocks
  // jump away from their own neighbours and come straight back - an outlier, which needs
  // no repetition and survives animation.
  {
    // On by default now. Arming this from the host required launching the game *after* the
    // switch file was written, and a whole session of real play went by with the offscreen
    // half of the evidence missing for that reason alone. urge_debug/pixprobe.txt = 0 turns
    // it off.
    // Two staging textures, because the copy submitted on one frame is read on the next. The
    // single-texture version had to call WaitForIdle before reading, which cost a third of
    // the frame rate and - worse for a probe - could hide a race between that copy and the
    // draw that follows it.

    if (++s_probe_poll >= 30) {
      s_probe_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "pixprobe.txt",
          "r");
      if (file) {
        int32_t value = 1;
        if (fscanf(file, "%d", &value) == 1)
          s_probe_enabled = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/pixprobe.txt");
      }
    }

  }


  // Diagnostics: how many scene renders happened since the previous present. Normal play
  // renders once per presented frame; a freeze or transition snapshot renders the scene
  // a second time inside the same frame, rotating the sprite set again.
  const uint32_t renders = context()->sprite_batcher->TakeRenderTicks();
  if (renders > 1)
    if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
    LOG(INFO) << "[FrameRotate] frame=" << frame_count_ << " renders=" << renders;

  // Determine wait delay time
  limiter_.Delay();

  // Tick callback
  frame_tick_handler_.Run(present_target);

  // Frame drain, on by default since the loss was localized outside the application's own
  // output. It was first tried as a candidate fix for the one-frame
  // region loss: the screen buffer is cleared outside a render pass every frame while the
  // composite that samples it may still be in flight, and this drain removes that overlap.
  // A cross-session comparison looked convincing (31 such frames in five minutes without it,
  // 0 with it), but a within-session alternating A/B - the only method that survives the
  // frame-to-frame variance of this metric, since the same build scored 0 and 37 in two
  // different sessions - showed no improvement, so it is not shipped as a fix. Keep it here
  // as a switch: urge_debug/framesync.txt, nonzero for on. NOTE: the events counted by the
  // probes below at that time were later identified as present-side scaling noise (a few
  // levels per block); the real loss is a large, region-local deviation.
  {
    static int32_t s_sync_poll = 0;
    // Off again: the row probe later showed it made no difference, and a per-frame drain is
    // the heaviest change this project carries.
    static bool s_sync_enabled = false;
    if (++s_sync_poll >= 30) {
      s_sync_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "framesync.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_sync_enabled = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/framesync.txt");
      }
    }
    if (s_sync_enabled && context()->primary_render_context) {
      context()->primary_render_context->Flush();
      context()->primary_render_context->WaitForIdle();
    }
  }

  // The frame is complete: rotate the per-frame buffer sets here, on the boundary the GPU
  // finishes on, so every render of the next frame shares one set instead of splitting it.
  // See SpriteBatch::AdvanceFrameSet.
  context()->sprite_batcher->AdvanceFrameSet();

  // A frame is on screen: see ConsiderLoadDrain(), which keeps its GPU drain out
  // of normal play by relying on exactly this boundary.
  //
  // Temporary probe. Every per-frame vertex and parameter buffer lives in kFrameSets
  // sets rotating once per frame, so the CPU must never get further ahead of the GPU
  // than that: a longer stretch means this frame wrote into a set the GPU may still be
  // reading, and one element of that frame then comes out drawn from another frame's
  // data (invisible or misplaced) while every CPU-side counter still reads perfect.
  // The frames elapsed between two presents measure that margin directly.
  {
    static uint64_t s_last_present_ms = 0;
    static uint64_t s_last_frame = 0;
    static uint64_t s_presents = 0;
    const uint64_t now = SDL_GetTicks();
    const uint64_t elapsed_ms = s_last_present_ms ? now - s_last_present_ms : 0;
    const uint64_t frames = frame_count_ - s_last_frame;
    if (s_last_present_ms && (elapsed_ms > 50 || ++s_presents % 300 == 0))
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[PresentAudit] n=" << s_presents << " interval=" << elapsed_ms
                << "ms frames_since_last_present=" << frames
                << " frame=" << frame_count_;
    s_last_present_ms = now;
    s_last_frame = frame_count_;
  }

  renderer::NotifyFramePresented();
}

// TEMPORARY: the frame's render target views, recorded so other translation units can force a pass
// boundary without threading the textures through. Marker: URGE_FRAME_RTV
static Diligent::ITextureView* g_urge_frame_rtv = nullptr;
static Diligent::ITextureView* g_urge_frame_dsv = nullptr;

// Called by every pass that opens, with the target it just bound. Marker: URGE_FRAME_RTV
//
// The boundary has to reopen the target of the pass that was interrupted, not the frame's: a pass is
// opened not only for the frame. ViewportImpl::Render opens one on a bitmap it was handed, which is
// how the title's mirror is assembled - the raw animation is drawn into a viewport, that viewport is
// rendered into a bitmap, and the bitmap is masked before anything reaches the screen. Reopening the
// frame's target in the middle of that pass sends the second copy of the sprite to the screen instead,
// and because the pass's scissor is the viewport rect the raw animation appears on the left half of
// the screen for exactly one frame. That is the flash reported on the title screen, and it was caused
// by the old unconditional sprite redraw: it does not happen with the redraw off, and it happened on
// every entry into a title sub-scene with it on. The repair that ships now only redraws runs whose
// quad is clipped by the sprite screen, which is why it does not reopen this path - keep it that way.
// TEMP: pixel probe geometry - file scope so the probe can be called at the end of the
// frame as well as during it. Marker: URGE_PROBEEND
// TEMP: pixel probe state, at file scope so the probe can run at the end of the frame too.
// Marker: URGE_PROBEEND
static constexpr int32_t kProbeGridX = 25;
static constexpr int32_t kProbeGridY = 19;
static constexpr int32_t kProbeBlock = 32;
static constexpr int32_t kProbeCount = kProbeGridX * kProbeGridY;

extern "C" void UrgeSetPassTarget(Diligent::ITextureView* render_target,
                                 Diligent::ITextureView* depth_stencil) {
  g_urge_frame_rtv = render_target;
  g_urge_frame_dsv = depth_stencil;
  // Marker: URGE_STENCILFILL - one line per distinct (colour, depth-stencil) format pair, so it is
  // visible which pass the sprites actually draw into and whether that pass carries a stencil
  // aspect at all. Without this the DSV read at clear time may belong to a different pass.
  static int32_t s_seen = 0;
  if (s_seen < 12) {
    const char* rtv_name = "null";
    const char* dsv_name = "null";
    // The *view* format is what the render pass is built from: a D24S8 texture can be exposed as a
    // depth-only view, and in that case every stencil write is silently dropped.
    if (render_target)
      rtv_name = Diligent::GetTextureFormatAttribs(render_target->GetDesc().Format).Name;
    if (depth_stencil)
      dsv_name = Diligent::GetTextureFormatAttribs(depth_stencil->GetDesc().Format).Name;
    static std::string s_last;
    std::string key = std::string(rtv_name) + "|" + dsv_name;
    if (key != s_last) {
      s_last = key;
      ++s_seen;
      LOG(INFO) << "[PassFmt] rtv=" << rtv_name << " dsv=" << dsv_name;
    }
  }
}

// See UrgeQuadClippedByTarget. Marker: URGE_CLIPREDRAW
//
// The region the sprites are laid out in (the game screen, in game coordinates). A run whose quad
// sticks out of it is the only kind measured to lose its fragments, so re-drawing exactly those
// keeps the repair where it is needed and keeps the double-blend off the large translucent panels
// that form the seam.
static int32_t g_urge_viewport_w = 0;
static int32_t g_urge_viewport_h = 0;

extern "C" void UrgePublishViewportRegion(int32_t x, int32_t y, int32_t w, int32_t h) {
  (void)x;
  (void)y;
  g_urge_viewport_w = w;
  g_urge_viewport_h = h;
}

extern "C" bool UrgeQuadClippedByTarget(float x0, float y0, float x1, float y1) {
  if (g_urge_viewport_w <= 0 || g_urge_viewport_h <= 0)
    return false;
  return x0 < 0.f || y0 < 0.f || x1 > static_cast<float>(g_urge_viewport_w) ||
         y1 > static_cast<float>(g_urge_viewport_h);
}

extern "C" void UrgeForcePassBoundary(Diligent::IDeviceContext* render_context) {
  if (!render_context || !g_urge_frame_rtv)
    return;
  render_context->SetRenderTargets(
      0, nullptr, nullptr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->SetRenderTargets(
      1, &g_urge_frame_rtv, g_urge_frame_dsv,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void RenderScreenImpl::RenderFrameInternal(
    Diligent::ITexture* render_target,
                                           Diligent::ITexture* depth_stencil) {
  // Submitting GPU commands after the surface has been taken away crashes the
  // Vulkan driver (the buffers it is rendering into no longer exist), and it is
  // also forbidden on Android once the activity is in background. The frame
  // logic keeps running: only the GPU work is skipped.
  if (!context()->render_device->IsSurfaceValid())
    return;

  // Start of a render sequence. On Android TV the batched sprite caches are kept for
  // the whole sequence (see SubmitBatchDataAndResetCache, which must not clear them per
  // pass), so the reset belongs here, once per frame: it drops the previous frame's
  // queued quads and rotates the frame set. Every path that writes per-frame vertex
  // data - the batcher's own buffers and the window layers - picks its set from
  // SpriteBatch::FrameSetIndex(), and a set that never rotates is one the GPU may still
  // be reading from the previous frame while this frame writes over it.
  context()->sprite_batcher->ResetFrameCache();

  // Advance the deferred-release graveyard (renderer::DeferredTextureRelease). This is the
  // frame clock it measures against, so it has to run once per frame: a texture the game
  // dropped stays alive for kDeferredReleaseFrames frames, which is long enough for every
  // draw that could still be sampling it to have completed.
  renderer::AdvanceDeferredRelease();

  // Candidate fix, rotating with the auto arms (1 and 3 drain, 0 and 2 do not).
  // The amplified experiment in the plane path shows what this is for: a blit that reads a
  // texture written earlier in the frame can read stale tiles, and retiring the pending work
  // before that blit cuts the loss from 80 events per 10000 frames to 16.7. The canvas
  // commands submitted just below are that shape - blits whose sources were written by
  // earlier commands or by an earlier stage - and they are what produce the game's bitmaps,
  // which is where the plane and the windows take their pixels from.
  {
    static int32_t s_canvas_sync_poll = 0;
    static bool s_canvas_sync_auto = false;
    // Off by default: this drain was on in every build the developer played, together with
    // the present drain and the text drain, and the flicker did not change. Its rotating arm
    // is gone as well - each file interpreted its own arm, so the comparison was confounded.
    static bool s_canvas_sync_manual = false;
    if (++s_canvas_sync_poll >= 30) {
      s_canvas_sync_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "canvasdrain.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_canvas_sync_manual = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/canvasdrain.txt");
      }
      FILE* auto_file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "autosync.txt",
          "r");
      if (auto_file) {
        int32_t value = 1;
        if (fscanf(auto_file, "%d", &value) == 1)
          s_canvas_sync_auto = value != 0;
        fclose(auto_file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/autosync.txt");
      }
    }
    const uint32_t canvas_frame = context()->sprite_batcher->FrameCount();
    const uint32_t canvas_arm = (canvas_frame / 1000u) % 4u;
    // 2x2 with the plane drain: arm 0 nothing, 1 this drain, 2 this drain and the plane
    // drain, 3 the plane drain only. InvalidateState was tried in arm 3 and measured no
    // better than no fix at all (36.3 events per 10000 frames against 29.3 for the
    // baseline), so it is gone.
    const bool want_canvas_drain =
        s_canvas_sync_auto ? (canvas_arm == 1 || canvas_arm == 2)
                           : s_canvas_sync_manual;
    if (want_canvas_drain && context()->primary_render_context) {
      context()->primary_render_context->Flush();
      context()->primary_render_context->WaitForIdle();
    }
  }

  // Submit pending canvas commands
  context()->canvas_scheduler->SubmitPendingPaintCommands();

  // Candidate fix for the one-frame region loss, gated on urge_debug/paintdrain.txt so it
  // can be A/B tested without a rebuild. The canvas commands above are texture updates
  // recorded at the head of this frame - script-side blits into the layer textures the
  // scene samples a few instructions later. Everything is on one queue, so correctness is
  // the driver's job, and this mt5895 Mali driver does not always give it: the frame shows
  // the background replaced by bright garbage while sprites on the same pass are fine.
  // Reproduced deterministically with a movement-only session (same script, same frame
  // numbers every run), which is what makes this measurable: with the drain off, eight
  // frames per 150 s lose a region; the drain completes the updates before anything
  // samples them.
  {
    static int32_t s_drain_poll = 0;
    static bool s_drain_enabled = false;
    if (++s_drain_poll >= 30) {
      s_drain_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "paintdrain.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_drain_enabled = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/paintdrain.txt");
      }
    }
    if (s_drain_enabled && context()->primary_render_context) {
      context()->primary_render_context->Flush();
      context()->primary_render_context->WaitForIdle();
    }
  }

  // Setup viewport
  controller_.CurrentViewport().bound = context()->resolution;
  controller_.CurrentViewport().origin = origin_;
  // The screen is the root of the tree; nothing can hide it, and its own node is what the walk
  // consults for the drawables directly under it.
  controller_.CurrentViewport().visible = true;

  // Prepare for rendering context
  DrawableNode::RenderControllerParams controller_params;
  controller_params.context = context()->primary_render_context;
  controller_params.screen_buffer = render_target;
  controller_params.screen_depth_stencil = depth_stencil;
  controller_params.screen_size = context()->resolution;

  // 1) Execute pre-composite handler
  controller_.BroadCastNotification(DrawableNode::BEFORE_RENDER,
                                    &controller_params);

  // 1.5) Update sprite batch data
  context()->sprite_batcher->SubmitBatchDataAndResetCache(
      context()->render.quad_index.get(), controller_params.context);

  // Candidate fix for the one-frame region loss, gated on urge_debug/invalidatestate.txt for
  // A/B testing. A layer of the map view - the Plane that draws the game's 670x402 map
  // bitmap - is rendered with somebody else's pixels for exactly one frame, while its
  // parameters, its place in the submission order, and even the contents of the texture it
  // samples are byte-identical to the frames around it (all three were measured). With the
  // engine's side of the contract demonstrably correct, what is left is the driver not
  // making this frame's earlier writes to that texture visible to this draw. Invalidating
  // Diligent's resource state on the boundary between every upload of this frame and its
  // draws makes it re-insert the full set of barriers, which is the Vulkan-side way to give
  // the driver no room to get that wrong. Costs a barrier per resource per frame; only
  // worth keeping if it removes the region loss.
  {
    static int32_t s_invalidate_poll = 0;
    static bool s_invalidate = false;
    if (++s_invalidate_poll >= 30) {
      s_invalidate_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "invalidatestate.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_invalidate = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/invalidatestate.txt");
      }
    }
    if (s_invalidate && context()->primary_render_context)
      context()->primary_render_context->InvalidateState();
  }

  // Candidate fix for the one-frame region loss, gated on urge_debug/rendersync.txt. This
  // is the boundary between every enqueue-stage upload of the frame - canvas blits into the
  // game's bitmaps, tilemap atlas copies, plane and window composition - and any draw that
  // samples their results. The measured evidence points here rather than earlier: draining
  // right after the canvas commands (urge_debug/paintdrain.txt) changes nothing at all
  // (83.9% of frames lost a region against 84.5% with it), while draining next to the
  // sampling plane removes almost all of it (89.3% against 4.4%), so what matters is
  // retiring the writes of this frame before the first draw reads them, not the writes
  // earlier in the pipe. Only worth shipping if it holds up in a within-session A/B: this
  // metric varies so much between runs that the same build scored 0 and 37 events in two
  // sessions, so cross-session comparison proves nothing.
  {
    static int32_t s_render_sync_poll = 0;
    static bool s_render_sync = false;
    if (++s_render_sync_poll >= 30) {
      s_render_sync_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "rendersync.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_render_sync = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/rendersync.txt");
      }
    }
    if (s_render_sync && context()->primary_render_context) {
      context()->primary_render_context->Flush();
      context()->primary_render_context->WaitForIdle();
    }
  }

  // See UrgeSetPassTarget: a forced boundary reopens this target, so the frame pass has to publish
  // it. Marker: URGE_FRAME_RTV
  g_urge_frame_rtv =
      render_target ? render_target->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET)
                    : nullptr;
  g_urge_frame_dsv = depth_stencil
                         ? depth_stencil->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL)
                         : nullptr;

  // 2) Setup renderpass
  GPUFrameBeginRenderPassInternal(controller_params.context, render_target,
                                  depth_stencil);


  // 3) Notify render a frame
  ScissorStack scissor_stack(controller_params.context, context()->resolution);
  controller_params.root_world = gpu_.root_transform;
  controller_params.world_binding = gpu_.world_transform;
  controller_params.scissors = &scissor_stack;
  controller_.BroadCastNotification(DrawableNode::ON_RENDERING,
                                    &controller_params);

  // 3.5) Emit anything the draw stage left queued, while still inside the pass.
  FlushSpriteQueueInternal(controller_params.context);


  // TEMPORARY sprite-batch audit. Marker: URGE_SPRAUDIT
  //
  // The UI strips are drawn through the batcher; if a frame's submission differs, these numbers say
  // so. Printed only when one of them changes, so a steady frame stays silent.
  {
    static uint32_t s_last_spr[3] = {0, 0, 0};
    uint32_t drawn = 0, fallback = 0, null_tex = 0;
    context()->sprite_batcher->UrgeAuditCounters(drawn, fallback, null_tex);
    if (UrgeDiagEnabled() && (drawn != s_last_spr[0] || fallback != s_last_spr[1] ||
        null_tex != s_last_spr[2])) {
      s_last_spr[0] = drawn;
      s_last_spr[1] = fallback;
      s_last_spr[2] = null_tex;
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[SprAudit] f=" << context()->sprite_batcher->FrameCount()
                << " drawn=" << drawn << " fallbackRuns=" << fallback
                << " nullTexture=" << null_tex;
    }
  }

  // TEMPORARY A/B. Marker: URGE_SCREENSTORE
  //
  // The frame is drawn into this offscreen buffer and then reaches the screen as an ImGui texture,
  // so it is sampled right after this pass ends. Nothing in this file ever says so: there is not one
  // TransitionResourceStates call for it. On a tile-based renderer that leaves the driver free to
  // discard the attachment instead of storing it when the pass ends, and what shows up then is
  // content from an earlier frame over part of the picture - measured on the device as 3.8% of
  // frames losing the bottom-right region (260 transients in 1151 presented frames), while the very
  // same frames read back from this buffer are pixel-identical to their neighbours (0 transients in
  // 467 whole-frame samples). A drain before the composite does not help (3.61%), and neither does a
  // render-scope boundary before presenting - a store is not a drain and is not a scope change.
  // Declaring the buffer as shader-readable is what makes the store necessary.
  //
  // urge_debug/screenstore.txt = 0 turns it off again.
  {
    static int32_t s_store_poll = 0;
    static bool s_store_enabled = true;
    if (++s_store_poll >= 30) {
      s_store_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "screenstore.txt", "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_store_enabled = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/screenstore.txt");
      }
    }
    if (s_store_enabled && gpu_.screen_buffer) {
      Diligent::StateTransitionDesc transitions[1];
      transitions[0].pResource = gpu_.screen_buffer.RawPtr();
      transitions[0].OldState = Diligent::RESOURCE_STATE_RENDER_TARGET;
      transitions[0].NewState = Diligent::RESOURCE_STATE_SHADER_RESOURCE;
      transitions[0].TransitionType = Diligent::STATE_TRANSITION_TYPE_IMMEDIATE;
      transitions[0].Flags = Diligent::STATE_TRANSITION_FLAG_NONE;
      controller_params.context->TransitionResourceStates(1, transitions);
    }
  }

  // 4) End render pass and process after-render effect
  GPUFrameEndRenderPassInternal(controller_params.context);

  // window and canvas stages draw afterwards - and a loss caused by one of those later stages is
  // exactly what it then reports as "steady". Sampled here instead: after the pass ended and after
  // the after-render effect wrote the last full-screen quad, i.e. the image about to be presented.
  // Marker: URGE_PROBEEND
    if (s_probe_enabled && gpu_.screen_buffer &&
        context()->primary_render_context) {
      auto* render_context = context()->primary_render_context;
      const auto& screen_desc = gpu_.screen_buffer->GetDesc();
      const int32_t w = static_cast<int32_t>(screen_desc.Width);
      const int32_t h = static_cast<int32_t>(screen_desc.Height);

      if (!s_probe_stage[0] && w > 0 && h > 0) {
        renderer::CreateTexture2D(
            **context()->render_device, &s_probe_stage[0], "pixel.probe.stage0",
            base::Vec2i(kProbeGridX * kProbeBlock, kProbeGridY * kProbeBlock),
            Diligent::USAGE_STAGING, Diligent::BIND_NONE,
            Diligent::CPU_ACCESS_READ, Diligent::TEX_FORMAT_RGBA8_UNORM);
        renderer::CreateTexture2D(
            **context()->render_device, &s_probe_stage[1], "pixel.probe.stage1",
            base::Vec2i(kProbeGridX * kProbeBlock, kProbeGridY * kProbeBlock),
            Diligent::USAGE_STAGING, Diligent::BIND_NONE,
            Diligent::CPU_ACCESS_READ, Diligent::TEX_FORMAT_RGBA8_UNORM);
      }

      if (s_probe_stage[0] && s_probe_stage[1] && w >= kProbeBlock &&
          h >= kProbeBlock) {
        // Which coordinate space the anchor below is in - see the note there.
        const bool probe_screen_space = (w >= 1600 && h >= 900);
        bool copied = true;
        for (int32_t gy = 0; gy < kProbeGridY && copied; ++gy) {
          for (int32_t gx = 0; gx < kProbeGridX && copied; ++gx) {
            // TEMP: the compass in the bottom-right corner, which drops out for a single frame now
            // and then. Which coordinate space applies depends on what the screen buffer actually is
            // - the game renders 800x600 but the buffer this copies from has been 1920x1080 in some
            // builds - so the anchor follows the size and both are logged. Measuring it this way
            // matters: reading the render target and the screen recording disagree (the recording
            // goes through the display pipeline), and an anchor in the wrong space samples
            // something else entirely, which is how an earlier round concluded "impossible to see".
            const int32_t anchor_x = 0;
            const int32_t anchor_y = 0;
            const int32_t x = std::min(anchor_x + gx * kProbeBlock, w - kProbeBlock);
            const int32_t y = std::min(anchor_y + gy * kProbeBlock, h - kProbeBlock);
            if (x < 0 || y < 0) {
              copied = false;
              break;
            }
            Diligent::Box region;
            region.MinX = x;
            region.MaxX = x + kProbeBlock;
            region.MinY = y;
            region.MaxY = y + kProbeBlock;
            Diligent::CopyTextureAttribs attribs;
            attribs.pSrcTexture = gpu_.screen_buffer;
            attribs.pSrcBox = &region;
            attribs.SrcTextureTransitionMode =
                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            attribs.pDstTexture = s_probe_stage[s_probe_index];
            attribs.DstTextureTransitionMode =
                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
            attribs.DstX = gx * kProbeBlock;
            attribs.DstY = gy * kProbeBlock;
            render_context->CopyTexture(attribs);
          }
        }

        // Read what the previous frame copied - never this frame's staging texture, which the
        // copies just submitted are still writing - and do not wait for it.
        Diligent::MappedTextureSubresource mapped;
        if (copied)
          render_context->MapTextureSubresource(
              s_probe_stage[1 - s_probe_index], 0, 0, Diligent::MAP_READ,
              Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
        if (copied && mapped.pData) {
          ++s_probe_ok;
          // Average each block: a region losing its overlay moves the average by tens of
          // levels, while animation, dithering and single pixels only wobble it by a few.
          uint32_t values[kProbeCount] = {};
          for (int32_t gy = 0; gy < kProbeGridY; ++gy) {
            for (int32_t gx = 0; gx < kProbeGridX; ++gx) {
              uint32_t sum[3] = {0, 0, 0};
              for (int32_t row = 0; row < kProbeBlock; ++row) {
                const uint8_t* line =
                    static_cast<const uint8_t*>(mapped.pData) +
                    (gy * kProbeBlock + row) * mapped.Stride +
                    gx * kProbeBlock * 4;
                for (int32_t col = 0; col < kProbeBlock; ++col) {
                  sum[0] += line[col * 4 + 0];
                  sum[1] += line[col * 4 + 1];
                  sum[2] += line[col * 4 + 2];
                }
              }
              const uint32_t pixels = kProbeBlock * kProbeBlock;
              values[gy * kProbeGridX + gx] = (sum[0] / pixels) << 16 |
                                              (sum[1] / pixels) << 8 |
                                              (sum[2] / pixels);
            }
          }

          char line_buffer[kProbeCount * 8 + 1];
          size_t written = 0;
          for (int32_t i = 0; i < kProbeCount; ++i)
            written += snprintf(line_buffer + written,
                                sizeof(line_buffer) - written, "%s%06x",
                                i ? "," : "", values[i]);
          if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
          LOG(INFO) << "[PixelProbe] f="
                    << context()->sprite_batcher->FrameCount()
                    << " buf=" << w << "x" << h
                    << " space=" << (probe_screen_space ? "screen" : "internal")
                    << " g=" << line_buffer << " ok=" << s_probe_ok
                    << " busy=" << s_probe_busy;
          render_context->UnmapTextureSubresource(s_probe_stage[1 - s_probe_index],
                                                  0, 0);
        } else {
          ++s_probe_busy;
        }
        s_probe_index = 1 - s_probe_index;
      }
    }

}

void RenderScreenImpl::GPUCreateGraphicsHostInternal() {
  // Create generic quads batch
  gpu_.transition_quads = renderer::QuadBatch::Make(**context()->render_device);
  gpu_.effect_quads = renderer::QuadBatch::Make(**context()->render_device);

  // Create generic shader binding
  gpu_.transition_binding_alpha =
      context()->render.pipeline_loader->alphatrans.CreateBinding();
  gpu_.transition_binding_vague =
      context()->render.pipeline_loader->mappedtrans.CreateBinding();
  gpu_.effect_binding =
      context()->render.pipeline_loader->color.CreateBinding();

  // Create screen buffer
  GPUResetScreenBufferInternal();
}

void RenderScreenImpl::GPUUpdateScreenWorldInternal() {
  renderer::WorldTransform& world_transform = gpu_.world_transform_cpu;
  renderer::MakeProjectionMatrix(world_transform.projection,
                                 context()->resolution.Recast<float>());
  renderer::MakeTransformMatrix(world_transform.transform,
                                base::Vec2(-origin_.x, -origin_.y));

  gpu_.world_transform.Release();
  Diligent::CreateUniformBuffer(
      **context()->render_device, sizeof(world_transform),
      "graphics.world.transform", &gpu_.world_transform,
      Diligent::USAGE_IMMUTABLE, Diligent::BIND_UNIFORM_BUFFER,
      Diligent::CPU_ACCESS_NONE, &world_transform);
}

// TEMPORARY A/B switches for how the frame reaches the screen. Marker: URGE_SWEXPERIMENT
//
// Every repair aimed at the drawing side measured nothing - drawing the affected layers a second
// time, draining the queues in six places, clearing depth before the draws, staging the viewport
// effect with a draw instead of a copy. The shape that does differ is the frame being written into
// an offscreen buffer and then read (sampled) to get onto the screen, so these two touch that
// instead: the format the offscreen buffer is written in, and the pacing of the presentation.
// Both are off by default and a normal session is unchanged.
//
//   urge_debug/swformat.txt   1 -> create the screen buffer as BGRA8 rather than RGBA8, the channel
//                                  order most Android/Mali surfaces are native in, which changes the
//                                  write-back path the frame takes. Read once, when the buffer is made.
//   urge_debug/swvsync.txt    0 -> present without waiting for vsync, 1 -> force vsync. Absent, or
//                                  any other value, leaves the engine's own setting alone.
//
// Note: the swap chain's present mode is not exposed through SwapChainDesc here, so it cannot be
// switched this way; the queue-depth lever that does exist is the buffer-count switch already in
// renderer/device/render_device.cc.
static Diligent::TEXTURE_FORMAT ScreenBufferFormatInternal() {
  static const char kPath[] =
      "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
      "swformat.txt";
  FILE* file = fopen(kPath, "r");
  if (file) {
    int32_t value = 0;
    if (fscanf(file, "%d", &value) == 1 && value != 0) {
      fclose(file);
      remove(kPath);
      LOG(INFO) << "[SwFormat] screen buffer format=BGRA8";
      return Diligent::TEX_FORMAT_BGRA8_UNORM;
    }
    fclose(file);
    remove(kPath);
  }
  return Diligent::TEX_FORMAT_RGBA8_UNORM;
}

static int32_t ScreenVsyncOverrideInternal() {
  static const char kPath[] =
      "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
      "swvsync.txt";
  static int32_t s_poll = 0;
  static int32_t s_override = -1;  // -1: leave the engine's own setting alone
  if (++s_poll >= 60) {
    s_poll = 0;
    FILE* file = fopen(kPath, "r");
    if (file) {
      int32_t value = -1;
      if (fscanf(file, "%d", &value) == 1)
        s_override = (value == 0 || value == 1) ? value : -1;
      fclose(file);
      remove(kPath);
      LOG(INFO) << "[SwVsync] override=" << s_override;
    }
  }
  return s_override;
}

void RenderScreenImpl::GPUResetScreenBufferInternal() {
  // Reset original buffer. These are parked rather than destroyed: this runs on a resize or a
  // graphics reset, and a frame the GPU is still executing can be drawing into, or
  // compositing from, the old buffers. See renderer::DeferredTextureRelease.
  renderer::DeferredTextureRelease(gpu_.screen_buffer);
  renderer::DeferredTextureRelease(gpu_.screen_buffer_prev);
  renderer::DeferredTextureRelease(gpu_.screen_depth_stencil);
  renderer::DeferredTextureRelease(gpu_.frozen_buffer);
  renderer::DeferredTextureRelease(gpu_.frozen_depth_stencil);
  renderer::DeferredTextureRelease(gpu_.transition_buffer);
  renderer::DeferredTextureRelease(gpu_.transition_depth_stencil);

  // Color attachment
  renderer::CreateTexture2D(
      **context()->render_device, &gpu_.screen_buffer, "screen.main.buffer",
      context()->resolution, Diligent::USAGE_DEFAULT,
      Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE,
      Diligent::CPU_ACCESS_NONE,
      // See ScreenBufferFormatInternal. Marker: URGE_SWEXPERIMENT
      ScreenBufferFormatInternal());
  renderer::CreateTexture2D(
      **context()->render_device, &gpu_.screen_buffer_prev,
      "screen.main.buffer.prev", context()->resolution,
      Diligent::USAGE_DEFAULT,
      Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE);

  renderer::CreateTexture2D(
      **context()->render_device, &gpu_.frozen_buffer, "screen.frozen.buffer",
      context()->resolution, Diligent::USAGE_DEFAULT,
      Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE);
  renderer::CreateTexture2D(
      **context()->render_device, &gpu_.transition_buffer,
      "screen.transition.buffer", context()->resolution,
      Diligent::USAGE_DEFAULT,
      Diligent::BIND_RENDER_TARGET | Diligent::BIND_SHADER_RESOURCE);

  // Depth stencil
  renderer::CreateTexture2D(
      **context()->render_device, &gpu_.screen_depth_stencil,
      "screen.main.depth_stencil", context()->resolution,
      Diligent::USAGE_DEFAULT, Diligent::BIND_DEPTH_STENCIL,
      Diligent::CPU_ACCESS_NONE, Diligent::TEX_FORMAT_D24_UNORM_S8_UINT);
  renderer::CreateTexture2D(
      **context()->render_device, &gpu_.frozen_depth_stencil,
      "screen.frozen.depth_stencil", context()->resolution,
      Diligent::USAGE_DEFAULT, Diligent::BIND_DEPTH_STENCIL,
      Diligent::CPU_ACCESS_NONE, Diligent::TEX_FORMAT_D24_UNORM_S8_UINT);
  renderer::CreateTexture2D(
      **context()->render_device, &gpu_.transition_depth_stencil,
      "screen.transition.depth_stencil", context()->resolution,
      Diligent::USAGE_DEFAULT, Diligent::BIND_DEPTH_STENCIL,
      Diligent::CPU_ACCESS_NONE, Diligent::TEX_FORMAT_D24_UNORM_S8_UINT);

  // Root transform
  renderer::WorldTransform world_transform;
  renderer::MakeProjectionMatrix(world_transform.projection,
                                 context()->resolution.Recast<float>());
  renderer::MakeIdentityMatrix(world_transform.transform);

  gpu_.root_transform.Release();
  Diligent::CreateUniformBuffer(
      **context()->render_device, sizeof(world_transform),
      "graphics.root.transform", &gpu_.root_transform,
      Diligent::USAGE_IMMUTABLE, Diligent::BIND_UNIFORM_BUFFER,
      Diligent::CPU_ACCESS_NONE, &world_transform);

  // Offset transform
  GPUUpdateScreenWorldInternal();
}

// Switch for the whole-cell probe. Marker: URGE_CELLPROBE_GATE
//
// The probe reads the presented frame back to the CPU every frame (a grid of 16x16 blocks copied out of
// the backbuffer, averaged, and logged) and it was left running unconditionally during the
// investigation - which is why the debug log ran to tens of megabytes an hour and why the probe cost
// GPU bandwidth and file I/O on every frame. It is diagnostic equipment, not part of the engine's
// behaviour, so it is now opt-in: write a nonzero value into urge_debug/cellprobe.txt to arm it for an
// investigation, and it stays off in normal play. Everything downstream of it ([CellAll], [CellUI],
// the frame dumps) is driven from this same readback, so this one switch covers them all.
// See the mx field of [CellProbe]. Marker: URGE_UIWATCH
// Limits the per-pixel difference to the right-hand UI strip so that map scrolling does not
// saturate it; the text there is static, so any missing row shows up as a spike.
static int32_t UiWatchEnabledInternal() {
  static int32_t s_poll = 0;
  static int32_t s_mode = 0;
  if (++s_poll >= 30) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "uiwatch.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_mode = value;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/uiwatch.txt");
      LOG(INFO) << "[UiWatch] mode=" << s_mode;
    }
  }
  return s_mode;
}

static bool CellProbeEnabledInternal() {
  static int32_t s_poll = 0;
  static bool s_enabled = false;
  if (++s_poll >= 60) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "cellprobe.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_enabled = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/cellprobe.txt");
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[CellProbeGate] enabled=" << s_enabled;
    }
  }
  return s_enabled;
}

void RenderScreenImpl::GPUPresentScreenBufferInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::ImGuiDiligentRenderer* gui_renderer) {
  // Initial swapchain attribute
  Diligent::ISwapChain* swapchain = context()->render_device->GetSwapChain();
  if (!swapchain || !context()->render_device->IsSurfaceValid())
    return;

  // The swap chain was rebuilt this frame: let the driver publish the new
  // surface geometry before the first vkQueuePresentKHR.
  if (context()->render_device->ConsumeSkipPresentOnce()) {
    URGE_GPU_AUDIT("present:skip-after-recreate", true);
    return;
  }

  auto* render_target_view = swapchain->GetCurrentBackBufferRTV();
  URGE_GPU_AUDIT("present:get-rtv", render_target_view != nullptr);
  if (!render_target_view)
    return;

  URGE_GPU_AUDIT("present:bind-backbuffer",
                 context()->render_device->IsSurfaceValid());

  // Prepare for rendering
  float clear_color[] = {0, 0, 0, 1};
  URGE_GPU_AUDIT("present:before-setrt", true);
  render_context->SetRenderTargets(
      1, &render_target_view, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->ClearRenderTarget(
      render_target_view, clear_color,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  URGE_GPU_AUDIT("present:after-clear", true);

  // Apply present scissor
  Diligent::Rect present_scissor(0, 0, swapchain->GetDesc().Width,
                                 swapchain->GetDesc().Height);
  render_context->SetScissorRects(1, &present_scissor, UINT32_MAX, UINT32_MAX);

  // Candidate fix, rotating with the auto arms (1 and 3 drain, 0 and 2 do not). This is the
  // one boundary the evidence now points at, and the only one the three drains already in
  // place do not cover: the frame is rendered into the offscreen screen buffer and then
  // reaches the screen as an ImGui texture, so the composite samples a texture the engine
  // finished writing moments earlier - the exact write-then-sample shape measured to read
  // stale tiles on this driver (blitting a fresh copy of it multiplied the loss 20x, and a
  // one-frame-late copy was just as bad, so what goes wrong is the blit's own content, not
  // when it is sampled). The measurements that led here: a five-minute battle session in
  // which the presented image carried 11 repeated-signature events (the defect) while the
  // offscreen buffer carried 2 tiny ones, plus the developer reporting the same loss as
  // before despite the canvas and plane drains.
  {
    static int32_t s_present_sync_poll = 0;
    static bool s_present_sync_auto = false;
    // Off by default, for the same reason as the canvas drain above: throughout the sessions
    // that still flickered, this one - which also drained the canvas context - was on.
    static bool s_present_sync_manual = false;
    if (++s_present_sync_poll >= 30) {
      s_present_sync_poll = 0;
      FILE* file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "presentsync.txt",
          "r");
      if (file) {
        int32_t value = 0;
        if (fscanf(file, "%d", &value) == 1)
          s_present_sync_manual = value != 0;
        fclose(file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/presentsync.txt");
      }
      FILE* auto_file = fopen(
          "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
          "autosync.txt",
          "r");
      if (auto_file) {
        int32_t value = 1;
        if (fscanf(auto_file, "%d", &value) == 1)
          s_present_sync_auto = value != 0;
        fclose(auto_file);
        remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
               "urge_debug/autosync.txt");
      }
    }
    const uint32_t present_arm =
        (context()->sprite_batcher->FrameCount() / 1000u) % 4u;
    const bool want_present_drain =
        s_present_sync_auto ? (present_arm == 1 || present_arm == 3)
                            : s_present_sync_manual;
    if (want_present_drain && context()->primary_render_context) {
      context()->primary_render_context->Flush();
      context()->primary_render_context->WaitForIdle();
      // The canvas commands are encoded on the scheduler's own device context, so draining
      // the primary one does not wait for them. A frame that mixed two frames - the top 78%
      // of the screen from one, the bottom from another, measured three times in twelve
      // minutes - is what compositing while the canvas writes are still in flight looks
      // like, and it survived draining only the primary context.
      if (auto* canvas_context =
              context()->canvas_scheduler->GetDiscreteRenderContext()) {
        canvas_context->Flush();
        canvas_context->WaitForIdle();
      }
    }
  }

  // Render GUI and present
  gui_renderer->RenderDrawData(render_context, ImGui::GetDrawData());
  URGE_GPU_AUDIT("present:after-gui", true);

  // TEMPORARY present-side probe: a 16x16 grid of 16x16 blocks (256 of them) spread evenly
  // over the back buffer, one line per frame, read back a frame later without waiting.
  //
  // It replaced a full-width row probe, and the reason matters. Averaging a whole row
  // dilutes a localised defect: a 120x80 region turning bright moves a 1920-wide row average
  // by only ten or twenty levels, which is under any sane threshold - so while the row probe
  // reported "clean" (largest deviation 239 over 25300 frames) the developer kept seeing the
  // flicker just as clearly. A dense block grid is the instrument that matches the symptom,
  // and it is also the one that caught it first: the earlier 6x4 block probe recorded
  // repeated events of the same block flipping to the same wrong value, up to magnitude
  // 1441, which were written off as effects at the time.
  {
    constexpr int32_t kCells = 16;
    constexpr int32_t kBlock = 16;
    constexpr int32_t kCount = kCells * kCells;
    constexpr int32_t kStageW = kCells * kBlock;
    static Diligent::RefCntAutoPtr<Diligent::ITexture> s_stage;
    static uint32_t s_prev_frame = 0;
    static uint32_t s_ok = 0;
    static uint32_t s_busy = 0;
    static int32_t s_stage_w = 0;

    // TEMPORARY aimed dump, see the [CellDump] note below. The last three readbacks: their
    // frame labels, their 256 block averages (the values the line above prints) and their raw
    // 256x256 pixels. Three is exactly what it takes to recognise the defect's fingerprint -
    // one frame differs, the frame before and the frame after are byte-identical - and a
    // block average cannot say *what* was drawn there, which is the question this answers.
    static uint32_t s_hist_label[3] = {0, 0, 0};
    static std::vector<uint32_t> s_hist_value[3];
    static std::vector<uint8_t> s_hist_pixels[3];
    // Previous frame, full size: used for the per-pixel maximum difference (see the mx field).
    static std::vector<uint8_t> s_prev_pixels;

    static int32_t s_dump_count = 0;
    static uint32_t s_last_dump_frame = 0;

    auto* backbuffer = render_target_view->GetTexture();
    // See CellProbeEnabledInternal: the probe is opt-in now. Marker: URGE_CELLPROBE_GATE
    if (backbuffer && CellProbeEnabledInternal()) {
      const auto& desc = backbuffer->GetDesc();
      const int32_t w = static_cast<int32_t>(desc.Width);
      const int32_t h = static_cast<int32_t>(desc.Height);
      if ((!s_stage || s_stage_w != w) && w >= kCells * kBlock &&
          h >= kCells * kBlock) {
        s_stage.Release();
        renderer::CreateTexture2D(
            **context()->render_device, &s_stage, "present.cell.stage",
            base::Vec2i(w, h), Diligent::USAGE_STAGING,
            Diligent::BIND_NONE, Diligent::CPU_ACCESS_READ,
            Diligent::TEX_FORMAT_RGBA8_UNORM);
        s_stage_w = w;
      }

      if (s_stage && w >= kCells * kBlock && h >= kCells * kBlock) {
        // Read the cells copied on the previous frame, before they are overwritten.
        Diligent::MappedTextureSubresource mapped;
        render_context->MapTextureSubresource(
            s_stage, 0, 0, Diligent::MAP_READ,
            Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr, mapped);
        if (mapped.pData) {
          ++s_ok;
          uint32_t cells[kCount];
          char line_buffer[kCount * 8 + 1];
          size_t written = 0;
          for (int32_t cell = 0; cell < kCount; ++cell) {
            const int32_t cx = cell % kCells;
            const int32_t cy = cell / kCells;
            // Average over the WHOLE cell, sampling every 4th pixel in both axes.
            //
            // The previous version read a single 16x16 block at the cell centre, i.e. 256 of the
            // 2,073,600 pixels of the frame (3.2%). After the pass-boundary change the defect
            // stopped taking out a whole sprite and started taking out a patch spanning two
            // panels, so the probe reported 0.000% while the flicker was still on screen - a
            // false negative caused by the instrument, not by the fix.
            const int32_t x0 = cx * w / kCells;
            const int32_t x1 = (cx + 1) * w / kCells;
            const int32_t y0 = cy * h / kCells;
            const int32_t y1 = (cy + 1) * h / kCells;
            uint64_t sum[3] = {0, 0, 0};
            uint64_t pixels = 0;
            for (int32_t row = y0; row < y1; row += 4) {
              const uint8_t* line =
                  static_cast<const uint8_t*>(mapped.pData) +
                  static_cast<size_t>(row) * mapped.Stride;
              for (int32_t col = x0; col < x1; col += 4) {
                sum[0] += line[col * 4 + 0];
                sum[1] += line[col * 4 + 1];
                sum[2] += line[col * 4 + 2];
                ++pixels;
              }
            }
            if (!pixels)
              pixels = 1;
            cells[cell] = static_cast<uint32_t>(sum[0] / pixels) << 16 |
                          static_cast<uint32_t>(sum[1] / pixels) << 8 |
                          static_cast<uint32_t>(sum[2] / pixels);
            written += snprintf(line_buffer + written,
                                sizeof(line_buffer) - written, "%s%06x",
                                cell ? "," : "", cells[cell]);
          }
          // sf= is the sprite batcher's own frame counter. The draw probes below log it, the
          // [PresentPath] line logs frame_count_; this line carries both, so an anomaly frame
          // can be matched to what the draws submitted for it.
          // Per-pixel maximum difference against the previous frame. The cell averages above
          // cannot see a small patch: a 30x30 px loss inside a 120x67 cell moves the average by
          // only a few levels. The maximum difference is the full contrast of the patch, so any
          // localized change shows up as a spike in mx regardless of its size. mc names the cell
          // it happened in.
          int32_t mx = 0;
          int32_t mc = -1;
          const uint8_t* staged = static_cast<const uint8_t*>(mapped.pData);
          const size_t row_bytes = static_cast<size_t>(w) * 4;
          // With uiwatch on, the difference is taken only inside the right-hand UI strip, so the
          // map scrolling under the character cannot drown out a flicker in the text itself.
          // 0 = whole frame, 1 = the compass block (bottom-right), 2 = the right-hand text strip.
          const int32_t ui_mode = UiWatchEnabledInternal();
          const int32_t ux0 = ui_mode == 1 ? 1380 : (ui_mode == 2 ? 1340 : 0);
          const int32_t ux1 = ui_mode == 1 ? 1720 : (ui_mode == 2 ? 1620 : w);
          const int32_t uy0 = ui_mode == 1 ? 740 : (ui_mode == 2 ? 240 : 0);
          const int32_t uy1 = ui_mode == 1 ? 1080 : (ui_mode == 2 ? 880 : h);
          // How many pixels changed, and where. A text row going missing is a wide, tall cluster
          // of changed pixels; a blinking cursor is a small box. The max alone cannot tell them
          // apart, which is why this was invisible before.
          int32_t changed = 0;
          int32_t bx0 = w;
          int32_t by0 = h;
          int32_t bx1 = -1;
          int32_t by1 = -1;
          if (s_prev_pixels.size() == static_cast<size_t>(h) * row_bytes) {
            for (int32_t y = uy0; y < uy1; ++y) {
              const uint8_t* cur = staged + static_cast<size_t>(y) * mapped.Stride;
              const uint8_t* prv =
                  s_prev_pixels.data() + static_cast<size_t>(y) * row_bytes;
              for (int32_t x = ux0; x < ux1; ++x) {
                int32_t pixel_dev = 0;
                for (int32_t ch = 0; ch < 3; ++ch) {
                  const int32_t d = std::abs(static_cast<int32_t>(cur[x * 4 + ch]) -
                                             static_cast<int32_t>(prv[x * 4 + ch]));
                  if (d > pixel_dev)
                    pixel_dev = d;
                }
                if (pixel_dev > mx) {
                  mx = pixel_dev;
                  mc = (y * kCells / h) * kCells + (x * kCells / w);
                }
                if (pixel_dev > 40) {
                  ++changed;
                  if (x < bx0) bx0 = x;
                  if (x > bx1) bx1 = x;
                  if (y < by0) by0 = y;
                  if (y > by1) by1 = y;
                }
              }
            }
          }
          s_prev_pixels.resize(static_cast<size_t>(h) * row_bytes);
          for (int32_t y = 0; y < h; ++y)
            std::memcpy(s_prev_pixels.data() + static_cast<size_t>(y) * row_bytes,
                        staged + static_cast<size_t>(y) * mapped.Stride, row_bytes);


          // Presence metric. Marker: URGE_PRESENCE
          //
          // Every metric above compares one frame with another, so a region that is missing in
          // *every* frame reads as "nothing changed" - which is exactly how the right-hand text
          // column's disappearance went unnoticed. This counts the bright pixels in that column
          // each frame, so absence is visible whether it lasts one frame or a hundred.
          int32_t bright_text = 0;
          {
            const int32_t px0 = std::min(1450, w - 1);
            const int32_t px1 = std::min(1700, w);
            const int32_t py0 = std::min(300, h - 1);
            const int32_t py1 = std::min(830, h);
            for (int32_t y = py0; y < py1; ++y) {
              const uint8_t* line = staged + static_cast<size_t>(y) * mapped.Stride;
              for (int32_t x = px0; x < px1; ++x) {
                const int32_t r = line[x * 4 + 0];
                const int32_t g = line[x * 4 + 1];
                const int32_t b = line[x * 4 + 2];
                if (std::max(r, std::max(g, b)) > 200)
                  ++bright_text;
              }
            }
          }

          if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
          LOG(INFO) << "[CellProbe] f=" << s_prev_frame
                    << " sf=" << context()->sprite_batcher->FrameCount()
                    << " mx=" << mx << " mc=" << mc << " n=" << changed
                    << " box=" << bx0 << "," << by0 << "-" << bx1 << "," << by1
                    << " bl=" << bright_text
                    << " c=" << line_buffer << " ok=" << s_ok << " busy=" << s_busy;

          // Roll the three-frame history and judge it with the criterion the offline analysis
          // uses: per block and channel, back = |prev - next| is the part that comes back and
          // diff = |cur - prev| is the deviation, so the frame is an event when the summed
          // remainder of the three reaches 100 and at least two blocks moved by 25 or more
          // without coming back. Then the middle frame is the one to look at: the averages
          // above can say that a block changed and to what, never what was drawn there, so
          // its raw pixels - and both neighbours', so the difference can be seen - are written
          // out for reading on the host.
          for (int32_t i = 0; i < 2; ++i) {
            s_hist_label[i] = s_hist_label[i + 1];
            s_hist_value[i].swap(s_hist_value[i + 1]);
            s_hist_pixels[i].swap(s_hist_pixels[i + 1]);
          }
          s_hist_label[2] = s_prev_frame;
          s_hist_value[2].assign(cells, cells + kCount);
          s_hist_pixels[2].resize(static_cast<size_t>(kStageW) * kCells * kBlock * 4);
          // The staging texture is now full size, so the dump image is a strided sample of it
          // rather than a straight copy: 256x256 covering the whole frame.
          for (int32_t row = 0; row < kCells * kBlock; ++row) {
            const uint8_t* src =
                static_cast<const uint8_t*>(mapped.pData) +
                static_cast<size_t>(row * h / (kCells * kBlock)) * mapped.Stride;
            uint8_t* dst =
                s_hist_pixels[2].data() + static_cast<size_t>(row) * kStageW * 4;
            for (int32_t col = 0; col < kStageW; ++col) {
              const uint8_t* p = src + (col * w / kStageW) * 4;
              dst[col * 4 + 0] = p[0];
              dst[col * 4 + 1] = p[1];
              dst[col * 4 + 2] = p[2];
              dst[col * 4 + 3] = 255;
            }
          }

          if (s_hist_label[1] == s_hist_label[0] + 1 &&
              s_hist_label[2] == s_hist_label[1] + 1) {
            int64_t deviation = 0;
            int32_t qualifying = 0;
            for (int32_t cell = 0; cell < kCount; ++cell) {
              bool cell_qualifies = false;
              for (int32_t shift = 16; shift >= 0; shift -= 8) {
                const int32_t prev = (s_hist_value[0][cell] >> shift) & 0xFF;
                const int32_t cur = (s_hist_value[1][cell] >> shift) & 0xFF;
                const int32_t next = (s_hist_value[2][cell] >> shift) & 0xFF;
                const int32_t back = prev > next ? prev - next : next - prev;
                const int32_t diff = cur > prev ? cur - prev : prev - cur;
                if (diff > back)
                  deviation += diff - back;
                // TEMPORARY sensitivity raise: the map/sprite repairs brought the big-region events to
                // zero while the user still sees the UI flicker, so the visible defect is smaller or
                // lower in contrast than this detector's original floor (25 per channel, two cells,
                // 100 total) could see. Marker: URGE_PROBE_SENSITIVE
                if (diff >= 8 && back <= 4)
                  cell_qualifies = true;
              }
              if (cell_qualifies)
                ++qualifying;
            }

            // Every event is reported, and only the pixel files are capped. The cap used to
            // cover this line as well, so a session that had seen 32 events went silent and
            // read as "the defect stopped" - it had not, the probe had. Marker for checking
            // that a build carries this: URGE_PROBE_ALLEVENTS.
            // TEMPORARY UI-strip detector. Marker: URGE_UIPROBE
            //
            // The remaining flicker is small and sits on the bottom and right strips - the system
            // frame panels - and the wide-screen detector above misses it: its floor is eight levels
            // per channel over at least one whole sixteen-pixel cell, and the strips are drawn from
            // cached bitmaps that change by less than that. This looks only at the cells in the
            // bottom quarter and the right quarter of the frame, with a much lower floor, and logs
            // which cells moved. It fires on its own, so the moment the user sees the flicker is in
            // the log without anyone having to note the time.
            {
              int32_t ui_deviation = 0;
              int32_t ui_cells = 0;
              char ui_list[256];
              ui_list[0] = '\0';
              size_t ui_written = 0;
              for (int32_t cell = 0; cell < kCount; ++cell) {
                const int32_t cell_x = cell % kCells;
                const int32_t cell_y = cell / kCells;
                if (cell_x < kCells * 3 / 4 && cell_y < kCells * 3 / 4)
                  continue;
                int32_t cell_dev = 0;
                for (int32_t shift = 16; shift >= 0; shift -= 8) {
                  const int32_t prev = (s_hist_value[0][cell] >> shift) & 0xFF;
                  const int32_t cur = (s_hist_value[1][cell] >> shift) & 0xFF;
                  const int32_t next = (s_hist_value[2][cell] >> shift) & 0xFF;
                  const int32_t back = prev > next ? prev - next : next - prev;
                  const int32_t diff = cur > prev ? cur - prev : prev - cur;
                  if (diff > back)
                    cell_dev += diff - back;
                }
                if (cell_dev >= 6) {
                  ui_deviation += cell_dev;
                  ++ui_cells;
                  if (ui_written < sizeof(ui_list) - 8)
                    ui_written += snprintf(ui_list + ui_written,
                                           sizeof(ui_list) - ui_written, "%s%d",
                                           ui_written ? "," : "", cell);
                }
              }
              if (ui_cells >= 1 && ui_deviation >= 6)
                if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
                LOG(INFO) << "[CellUI] f=" << s_hist_label[1] << " dev=" << ui_deviation
                          << " cells=" << ui_cells << " list=" << ui_list;
            }

            if (deviation >= 20 && qualifying >= 1) {  // TEMPORARY, see above
              if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
              LOG(INFO) << "[CellAll] cellprobe_" << s_hist_label[1] << ".dat f="
                        << s_hist_label[1] << " dev=" << deviation
                        << " blocks=" << qualifying << " dumps=" << s_dump_count;
              if (s_dump_count < 2000 &&
                  (s_last_dump_frame == 0 ||
                   s_hist_label[2] - s_last_dump_frame > 60)) {
                char dump_path[256];
                snprintf(dump_path, sizeof(dump_path),
                         "/storage/emulated/0/Android/data/com.admenri.urge/files/"
                         "urge_debug/cellprobe_%u.dat",
                         s_hist_label[1]);
                if (FILE* dump_file = fopen(dump_path, "wb")) {
                  char dump_header[128];
                  const int32_t dump_header_len = snprintf(
                      dump_header, sizeof(dump_header),
                      "CELLDUMP prev=%u anomaly=%u next=%u size=%dx%d block=%d\n",
                      s_hist_label[0], s_hist_label[1], s_hist_label[2], kStageW,
                      kCells * kBlock, kBlock);
                  fwrite(dump_header, 1, dump_header_len, dump_file);
                  for (int32_t i = 0; i < 3; ++i)
                    fwrite(s_hist_pixels[i].data(), 1,
                           s_hist_pixels[i].size(), dump_file);
                  fclose(dump_file);
                  ++s_dump_count;
                  s_last_dump_frame = s_hist_label[2];
                }
              }
            }
          }

          render_context->UnmapTextureSubresource(s_stage, 0, 0);
        } else {
          ++s_busy;
        }

        // Submit the whole back buffer; the cell averages are taken on the CPU over each cell's
        // full area. Copying 256 centre blocks (the old way) covered 3.2% of the frame and could
        // not see a partial region flicker.
        {
          Diligent::CopyTextureAttribs attribs;
          attribs.pSrcTexture = backbuffer;
          attribs.SrcTextureTransitionMode =
              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
          attribs.pDstTexture = s_stage;
          attribs.DstTextureTransitionMode =
              Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
          render_context->CopyTexture(attribs);
        }
        s_prev_frame = frame_count_;
      }
    }
  }

  // Flush command buffer and present GPU surface
  URGE_GPU_AUDIT("present:before-present", true);
  // See ScreenVsyncOverrideInternal. Marker: URGE_SWEXPERIMENT
  {
    const int32_t vsync_override = ScreenVsyncOverrideInternal();
    swapchain->Present(vsync_override >= 0 ? vsync_override
                                          : context()->engine_profile->vsync);
  }
  URGE_GPU_AUDIT("present:after-present", true);
}

// Master switch for the diagnostic output. Marker: URGE_DIAG_GATE
//
// Every audit, probe and counter print in this tree exists to investigate the one-frame loss, not to
// run the game: the per-frame [PassAudit] block alone walks ten counters and formats a line, and the
// cell probe used to read a grid back from the GPU every frame. They are now behind one switch so a
// normal session carries none of that cost - write a nonzero value into urge_debug/diag.txt to arm
// them for an investigation. The instrumentation code itself is left in place (its snapshot lives in
// doc/patches), so arming it needs no rebuild.
extern "C" bool UrgeDiagEnabled() {
  static int32_t s_poll = 0;
  static bool s_enabled = false;
  // Polled rather than read every call: the gate sits in front of ninety-odd log sites, some of them
  // in per-draw paths, and a failed fopen costs a syscall. At this interval the hottest of them
  // attempt one about every few frames, which is nothing next to the draws they guard.
  if (++s_poll >= 600) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "diag.txt",
        "r");
    if (file) {
      int32_t value = 0;
      if (fscanf(file, "%d", &value) == 1)
        s_enabled = value != 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/diag.txt");
      if (UrgeDiagEnabled())  // TEMPORARY gate, see renderscreen_impl.cc. Marker: URGE_DIAG_GATE
      LOG(INFO) << "[Diag] enabled=" << s_enabled;
    }
  }
  return s_enabled;
}

extern "C" bool UrgeDiagEnabled();

void RenderScreenImpl::GPUFrameBeginRenderPassInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::ITexture* render_target,
    Diligent::ITexture* depth_stencil) {
  // Setup screen effect params
  if (brightness_ < 255) {
    renderer::Quad effect_quad;
    renderer::Quad::SetPositionRect(&effect_quad,
                                    base::Rect(context()->resolution));
    renderer::Quad::SetColor(&effect_quad,
                             base::Vec4(0, 0, 0, (255 - brightness_) / 255.0f));
    gpu_.effect_quads.QueueWrite(render_context, &effect_quad);
  }

  // Setup render pass
  auto* render_target_view =
      render_target->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
  auto* depth_stencil_view =
      depth_stencil->GetDefaultView(Diligent::TEXTURE_VIEW_DEPTH_STENCIL);

  Diligent::ITextureView* pass_target_view = render_target_view;
  Diligent::ITextureView* pass_depth_view = depth_stencil_view;
  render_context->SetRenderTargets(
      1, &pass_target_view, pass_depth_view,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  Diligent::Rect render_scissor(0, 0, render_target->GetDesc().Width,
                                render_target->GetDesc().Height);
  render_context->SetScissorRects(1, &render_scissor, UINT32_MAX, UINT32_MAX);

  const float clear_color[] = {0.0f, 0.0f, 0.0f, 1.0f};
  render_context->ClearRenderTarget(
      pass_target_view, clear_color,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  render_context->ClearDepthStencil(
      depth_stencil_view, Diligent::CLEAR_DEPTH_FLAG, 1.0f, 0,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void RenderScreenImpl::FlushSpriteQueueInternal(
    Diligent::IDeviceContext* render_context) {
  // Batches and window layers are queued in the enqueue stage and drawn by their own
  // node in the draw stage. The drawable list is re-linked while a frame runs, so a
  // node can be missing when the draws are issued and its part of the picture would
  // be lost for that frame. Emitting it here - still inside the pass, with this pass's
  // transform and clip rect - keeps the frame complete. What a node did draw is
  // skipped, so a normal frame behaves exactly as before.
  const auto& resolution = context()->resolution;
  const base::Rect scissor(0, 0, resolution.x, resolution.y);

  context()->sprite_batcher->FlushPass(render_context,
                                       context()->render.quad_index.get(),
                                       context()->render.pipeline_states.get(),
                                       scissor, gpu_.world_transform);
  WindowImpl::FlushPendingLayers(render_context, gpu_.world_transform, scissor);
}

// TEMPORARY A/B. Marker: URGE_NOBRIGHT
//
// The one-frame loss was measured to be the frame getting *brighter*: with the map idle, the deviating
// frame's differing area went from luminance 46 to 171 over 38% of the screen, and only that one
// frame differed. That is the shape of this pass - a full-screen quad that darkens the frame while
// the game's brightness is below maximum - losing its writes for one frame, so the frame keeps the
// content from before the quad. Skipping the pass removes the quad and with it the thing that can go
// missing; the picture then stays unshaded (measurably brighter), which is also the self-check that
// the arm is really active.
static bool BrightnessEffectSkippedInternal() {
  static int32_t s_poll = 0;
  static bool s_skipped = false;
  if (++s_poll >= 30) {
    s_poll = 0;
    FILE* file = fopen(
        "/storage/emulated/0/Android/data/com.admenri.urge/files/urge_debug/"
        "tmbright.txt",
        "r");
    if (file) {
      int32_t value = 1;
      if (fscanf(file, "%d", &value) == 1)
        s_skipped = value == 0;
      fclose(file);
      remove("/storage/emulated/0/Android/data/com.admenri.urge/files/"
             "urge_debug/tmbright.txt");
    }
  }
  return s_skipped;
}

void RenderScreenImpl::GPUFrameEndRenderPassInternal(
    Diligent::IDeviceContext* render_context) {
  // Render screen effect if need
  if (brightness_ < 255 && !BrightnessEffectSkippedInternal()) {
    // Apply brightness effect
    auto* pipeline = context()->render.pipeline_states->brightness.RawPtr();

    // Set world transform
    gpu_.effect_binding.u_transform->Set(gpu_.world_transform);

    // Apply pipeline state
    render_context->SetPipelineState(pipeline);
    render_context->CommitShaderResources(
        *gpu_.effect_binding,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Apply vertex index
    Diligent::IBuffer* const vertex_buffer = *gpu_.effect_quads;
    render_context->SetVertexBuffers(
        0, 1, &vertex_buffer, nullptr,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    render_context->SetIndexBuffer(
        **context()->render.quad_index, 0,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // Execute render command
    Diligent::DrawIndexedAttribs draw_indexed_attribs;
    draw_indexed_attribs.NumIndices = 6;
    draw_indexed_attribs.IndexType =
        context()->render.quad_index->GetIndexType();
    render_context->DrawIndexed(draw_indexed_attribs);
  }
}

void RenderScreenImpl::GPURenderAlphaTransitionFrameInternal(
    Diligent::IDeviceContext* render_context,
    float progress) {
  // See RenderFrameInternal(): no GPU work while the surface is gone.
  if (!context()->render_device->IsSurfaceValid())
    return;

  // Update transition uniform
  renderer::Quad transient_quad;
  renderer::Quad::SetPositionRect(&transient_quad,
                                  base::RectF(-1.0f, 1.0f, 2.0f, -2.0f));
  renderer::Quad::SetTexCoordRectNorm(
      &transient_quad, base::RectF(base::Vec2(0), base::Vec2(1)));
  renderer::Quad::SetColor(&transient_quad, base::Vec4(progress));
  gpu_.transition_quads.QueueWrite(render_context, &transient_quad);

  // Composite transition frame
  auto render_target_view =
      gpu_.screen_buffer->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
  render_context->SetRenderTargets(
      1, &render_target_view, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const float clear_color[] = {0.0f, 0.0f, 0.0f, 1.0f};
  render_context->ClearRenderTarget(
      render_target_view, clear_color,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Derive pipeline sets
  auto* pipeline = context()->render.pipeline_states->alpha_transition.RawPtr();

  // Set uniform texture
  gpu_.transition_binding_alpha.u_current_texture->Set(
      gpu_.transition_buffer->GetDefaultView(
          Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
  gpu_.transition_binding_alpha.u_frozen_texture->Set(
      gpu_.frozen_buffer->GetDefaultView(
          Diligent::TEXTURE_VIEW_SHADER_RESOURCE));

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.transition_binding_alpha,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer = *gpu_.transition_quads;
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

void RenderScreenImpl::GPURenderVagueTransitionFrameInternal(
    Diligent::IDeviceContext* render_context,
    Diligent::ITextureView* trans_mapping,
    float progress,
    float vague) {
  // See RenderFrameInternal(): no GPU work while the surface is gone.
  if (!context()->render_device->IsSurfaceValid())
    return;

  // Update transition uniform
  renderer::Quad transient_quad;
  renderer::Quad::SetPositionRect(&transient_quad,
                                  base::RectF(-1.0f, 1.0f, 2.0f, -2.0f));
  renderer::Quad::SetTexCoordRectNorm(
      &transient_quad, base::RectF(base::Vec2(0), base::Vec2(1)));
  renderer::Quad::SetColor(&transient_quad, base::Vec4(vague, 0, 0, progress));
  gpu_.transition_quads.QueueWrite(render_context, &transient_quad);

  // Composite transition frame
  auto render_target_view =
      gpu_.screen_buffer->GetDefaultView(Diligent::TEXTURE_VIEW_RENDER_TARGET);
  render_context->SetRenderTargets(
      1, &render_target_view, nullptr,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
  const float clear_color[] = {0, 0, 0, 1};
  render_context->ClearRenderTarget(
      render_target_view, clear_color,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Derive pipeline sets
  auto* pipeline = context()->render.pipeline_states->vague_transition.RawPtr();

  // Set uniform texture
  gpu_.transition_binding_vague.u_current_texture->Set(
      gpu_.transition_buffer->GetDefaultView(
          Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
  gpu_.transition_binding_vague.u_frozen_texture->Set(
      gpu_.frozen_buffer->GetDefaultView(
          Diligent::TEXTURE_VIEW_SHADER_RESOURCE));
  gpu_.transition_binding_vague.u_trans_texture->Set(trans_mapping);

  // Apply pipeline state
  render_context->SetPipelineState(pipeline);
  render_context->CommitShaderResources(
      *gpu_.transition_binding_vague,
      Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

  // Apply vertex index
  Diligent::IBuffer* const vertex_buffer = *gpu_.transition_quads;
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
