// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RENDERER_DEVICE_DEVICE_MODE_H_
#define RENDERER_DEVICE_DEVICE_MODE_H_

namespace renderer {

// Android TV switches.
//
// Everything that differs on an Android TV box is gated on this flag so that a
// regular Android device keeps exactly the behaviour it had before. On TV the
// panel is landscape and its surface needs no rotation, so the swap chain must
// stay in the identity orientation; the crash self-capture, the U盘 log mirror
// and the texture audit belong to the TV port as well.
//
// Set from the JNI entry point (URGEMain.onCreate → nativeSetTvDevice). That
// runs before the SDL thread exists — SDL starts it only once the activity
// reaches the RESUMED state — so the flag is already in place when the engine's
// main() reads it.
inline bool& AndroidTvFlag() {
  static bool enabled = false;
  return enabled;
}

inline void SetAndroidTvMode(bool enabled) {
  AndroidTvFlag() = enabled;
}

inline bool IsAndroidTvMode() {
  return AndroidTvFlag();
}

}  // namespace renderer

#endif  //! RENDERER_DEVICE_DEVICE_MODE_H_
