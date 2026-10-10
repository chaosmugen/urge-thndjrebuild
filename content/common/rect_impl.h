// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_COMMON_RECT_IMPL_H_
#define CONTENT_COMMON_RECT_IMPL_H_

#include "base/math/rectangle.h"
#include "content/common/value_observer.h"
#include "content/public/engine_rect.h"

namespace content {

class RectImpl : public Rect, public ValueNotification {
 public:
  RectImpl(const base::Rect& rect);
  RectImpl(const RectImpl& other);
  ~RectImpl() override = default;

  RectImpl& operator=(const RectImpl& other);

  static scoped_refptr<RectImpl> From(scoped_refptr<Rect> host);

  bool CompareWithOther(scoped_refptr<Rect> other,
                        ExceptionState& exception_state) override;

  uint64_t GetHashCode() override;

  void Set(int32_t x,
           int32_t y,
           int32_t width,
           int32_t height,
           ExceptionState& exception_state) override;
  void Set(scoped_refptr<Rect> other, ExceptionState& exception_state) override;
  void Empty(ExceptionState& exception_state) override;

  URGE_DECLARE_OVERRIDE_ATTRIBUTE(X, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Y, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Width, int32_t);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Height, int32_t);

  void SetBase(const base::Rect& base);
  base::Rect AsBaseRect();
  SDL_Rect AsSDLRect();

 private:
  friend class Rect;

  base::Rect rect_;
};

}  // namespace content

#endif  //! CONTENT_COMMON_RECT_IMPL_H_
