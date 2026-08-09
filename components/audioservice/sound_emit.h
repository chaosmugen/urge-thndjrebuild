// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUDIOSERVICE_SOUND_EMIT_H_
#define COMPONENTS_AUDIOSERVICE_SOUND_EMIT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "miniaudio.h"

namespace audioservice {

class SoundEmit {
 public:
  ~SoundEmit();

  SoundEmit(const SoundEmit&) = delete;
  SoundEmit& operator=(const SoundEmit&) = delete;

  ma_result Play(const std::string& filename, int32_t volume, int32_t pitch);
  void Stop();

 private:
  friend class AudioService;
  SoundEmit(ma_engine* engine);

  // Fixed-size voice pool. Voices are lazily initialized on first use and
  // recycled afterwards to avoid per-play allocation and file re-opening.
  static constexpr size_t kMaxVoices = 32;

  struct Voice {
    ma_sound sound{};
    std::string filename;          // Last file loaded on this voice.
    bool initialized = false;      // Whether the ma_sound is initialized.
    uint64_t last_start_seq = 0;   // Sequence of the last start (for stealing).

    Voice() { memset(&sound, 0, sizeof(sound)); }
    Voice(const Voice&) = delete;  // ma_sound must not be shallow-copied.
    Voice& operator=(const Voice&) = delete;
  };

  ma_engine* engine_;
  std::vector<std::unique_ptr<Voice>> voices_;
  uint64_t play_seq_ = 0;
};

}  // namespace audioservice

#endif  // !COMPONENTS_AUDIOSERVICE_SOUND_EMIT_H_
