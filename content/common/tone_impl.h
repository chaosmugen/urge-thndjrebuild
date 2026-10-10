// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_COMMON_TONE_IMPL_H_
#define CONTENT_COMMON_TONE_IMPL_H_

#include <tuple>

#include "base/math/vector.h"
#include "content/common/value_observer.h"
#include "content/public/engine_tone.h"

namespace content {

class ToneImpl : public Tone, public ValueNotification {
 public:
  ToneImpl(const base::Vec4& value);
  ToneImpl(const ToneImpl& other);
  ~ToneImpl() override = default;

  ToneImpl& operator=(const ToneImpl& other);

  static scoped_refptr<ToneImpl> From(scoped_refptr<Tone> host);

  bool CompareWithOther(scoped_refptr<Tone> other,
                        ExceptionState& exception_state) override;

  uint64_t GetHashCode() override;

  void Set(float red,
           float green,
           float blue,
           float gray,
           ExceptionState& exception_state) override;
  void Set(float red,
           float green,
           float blue,
           ExceptionState& exception_state) override;
  void Set(scoped_refptr<Tone> other, ExceptionState& exception_state) override;

  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Red, float);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Green, float);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Blue, float);
  URGE_DECLARE_OVERRIDE_ATTRIBUTE(Gray, float);

  base::Vec4 AsNormColor();
  bool IsValid() const { return value_.x || value_.y || value_.z || value_.w; }

 private:
  friend class Tone;
  void NotifyObservers() override;

  base::Vec4 value_;
  base::Vec4 norm_;
  bool dirty_;
};

}  // namespace content

#endif  //! CONTENT_COMMON_TONE_IMPL_H_
