// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_RENDER_WINDOW_IMPL_H_
#define CONTENT_RENDER_WINDOW_IMPL_H_

#include "content/canvas/canvas_impl.h"
#include "content/common/color_impl.h"
#include "content/common/rect_impl.h"
#include "content/common/tone_impl.h"
#include "content/context/disposable.h"
#include "content/public/engine_window.h"
#include "content/render/drawable_controller.h"
#include "content/screen/viewport_impl.h"
#include "renderer/device/render_device.h"

namespace content {

class WindowImpl : public Window, public EngineObject, public Disposable {
 public:
  struct GPUData {
    // One set per frame in flight, exactly like the sprite batch: a single buffer that is
    // overwritten every frame can be read by a frame still executing while the next one
    // writes it, and the window then draws from another frame's vertices - which shows up
    // as the whole window (border and contents together) vanishing for that frame and
    // coming back on the next. Rotated by SpriteBatch::FrameSetIndex().
    static constexpr int32_t kFrameSets = 8;
    renderer::QuadBatch background_batch[kFrameSets];
    std::vector<renderer::Quad> background_cache;
    renderer::QuadBatch controls_batch[kFrameSets];
    std::vector<renderer::Quad> controls_cache;

    renderer::Binding_Base base_binding;
    renderer::Binding_Base content_binding;

    int32_t background_draw_count = 0;
    int32_t controls_draw_count = 0;
    int32_t contents_quad_offset = 0;


    int32_t bg_write_set = -1;
    int32_t ctrl_write_set = -1;

  };

  // A window layer is composed in the enqueue stage and drawn in the draw stage. The
  // drawable list is re-linked while a frame runs, so the window node can be missing
  // from the draw stage and its frame (with its contents) vanishes for that frame -
  // measured on the TV as composites=18000 draws=17998. The pass that is ending calls
  // this to emit the layers that never reached their draw stage. Only the layers
  // composed under |world| are emitted, which keeps each level's windows in the pass
  // that owns the render target and transform.
  static void FlushPendingLayers(Diligent::IDeviceContext* context,
                                 Diligent::IBuffer* world,
                                 const base::Rect& scissor);



  WindowImpl(ExecutionContext* execution_context,
             scoped_refptr<ViewportImpl> parent,
             int32_t scale);
  ~WindowImpl() override;

  WindowImpl(const WindowImpl&) = delete;
  WindowImpl& operator=(const WindowImpl&) = delete;

  URGE_DECLARE_DISPOSABLE;
  void Update(ExceptionState& exception_state) override;

  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Viewport, scoped_refptr<Viewport>);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Windowskin, scoped_refptr<Bitmap>);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Contents, scoped_refptr<Bitmap>);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Stretch, bool);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(CursorRect, scoped_refptr<Rect>);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Active, bool);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Visible, bool);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Pause, bool);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(X, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Y, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Width, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Height, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Z, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Ox, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Oy, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Opacity, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(BackOpacity, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(ContentsOpacity, int32_t);

 private:
  void OnObjectDisposed() override;
  std::string DisposedObjectName() override { return "Window"; }
  void BackgroundNodeHandlerInternal(
      DrawableNode::RenderStage stage,
      DrawableNode::RenderControllerParams* params);
  void ControlNodeHandlerInternal(DrawableNode::RenderStage stage,
                                  DrawableNode::RenderControllerParams* params);

  void GPUCreateWindowInternal();
  void GPUCompositeBackgroundLayerInternal(
      Diligent::IDeviceContext* render_context,
      BitmapTexture* windowskin);
  void GPUCompositeControlLayerInternal(
      Diligent::IDeviceContext* render_context,
      BitmapTexture* windowskin,
      BitmapTexture* contents);
  void GPURenderBackgroundLayerInternal(
      Diligent::IDeviceContext* render_context,
      Diligent::IBuffer* world_binding,
      BitmapTexture* windowskin);
  void GPURenderControlLayerInternal(Diligent::IDeviceContext* render_context,
                                     Diligent::IBuffer* world_binding,
                                     BitmapTexture* windowskin,
                                     BitmapTexture* contents,
                                     ScissorStack* scissor_stack);

  GPUData gpu_;
  DrawableNode background_node_;
  DrawableNode control_node_;

  int32_t scale_ = 2;
  int32_t pause_index_ = 0;
  int32_t cursor_opacity_ = 255;
  bool cursor_fade_ = false;

  scoped_refptr<ViewportImpl> viewport_;
  scoped_refptr<CanvasImpl> windowskin_;
  scoped_refptr<CanvasImpl> contents_;
  scoped_refptr<RectImpl> cursor_rect_;

  bool stretch_ = true;
  bool active_ = true;
  bool pause_ = false;

  base::Rect bound_;
  base::Vec2i origin_;

  int32_t opacity_ = 255;
  int32_t back_opacity_ = 255;
  int32_t contents_opacity_ = 255;

  // Last skin / contents whose texture was actually usable. A window must not become
  // invisible because its bitmap was cleared or replaced: the border layer builds no
  // quad at all without a skin and the control layer draws nothing without contents,
  // so a window with both missing draws no pixel (measured: 46 skin assignments, 18 of
  // them clears, and the window disappears for those frames while the background shows
  // through). Holding the reference also keeps the GPU texture alive while it is used.
  scoped_refptr<CanvasImpl> last_windowskin_;
  scoped_refptr<CanvasImpl> last_contents_;

  // Skin / contents to draw with, falling back to the last usable ones.
  BitmapTexture* ResolveWindowskinInternal();
  BitmapTexture* ResolveContentsInternal();
};

}  // namespace content

#endif  //! CONTENT_RENDER_WINDOW_IMPL_H_
