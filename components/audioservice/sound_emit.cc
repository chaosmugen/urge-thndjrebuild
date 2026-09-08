// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/audioservice/sound_emit.h"

#include <algorithm>

namespace audioservice {

namespace {

// Decoded in memory with asynchronous loading; spatialization is disabled
// because engine sounds are mixed without listener positioning.
constexpr ma_uint32 kSoundFlags = MA_SOUND_FLAG_ASYNC | MA_SOUND_FLAG_DECODE |
                                  MA_SOUND_FLAG_NO_SPATIALIZATION;

}  // namespace

SoundEmit::SoundEmit(ma_engine* engine) : engine_(engine) {
  voices_.resize(kMaxVoices);
  for (auto& voice : voices_)
    voice = std::make_unique<Voice>();
}

SoundEmit::~SoundEmit() {
  Stop();
}

ma_result SoundEmit::Play(const std::string& filename,
                          int32_t volume,
                          int32_t pitch) {
  // Guard against out-of-range parameters.
  volume = std::clamp(volume, 0, 100);
  pitch = std::clamp(pitch, 50, 200);

  // Pick a voice to reuse:
  //   1. An unused slot.
  //   2. The oldest voice that is not currently playing. Requiring
  //      ma_sound_at_end() here would leak voices that stopped without
  //      reaching the end (early stop, or async decoding stuck in MA_BUSY).
  //   3. Otherwise steal the oldest voice (every slot is playing).
  Voice* voice = nullptr;
  for (auto& candidate : voices_) {
    if (!candidate->initialized) {
      voice = candidate.get();
      break;
    }
  }
  if (!voice) {
    uint64_t oldest_seq = UINT64_MAX;
    for (auto& candidate : voices_) {
      if (candidate->initialized && !ma_sound_is_playing(&candidate->sound) &&
          candidate->last_start_seq < oldest_seq) {
        oldest_seq = candidate->last_start_seq;
        voice = candidate.get();
      }
    }
  }
  if (!voice) {
    uint64_t oldest_seq = UINT64_MAX;
    for (auto& candidate : voices_) {
      if (candidate->initialized && candidate->last_start_seq < oldest_seq) {
        oldest_seq = candidate->last_start_seq;
        voice = candidate.get();
      }
    }
  }
  if (!voice)
    return MA_OUT_OF_MEMORY;  // Unreachable with a fixed-size pool.

  // (Re)initialize the sound when targeting a different file.
  if (voice->initialized && voice->filename == filename) {
    // Restart the same file from the beginning. Stop is a no-op when the
    // sound is already stopped; seek failures during async loading are
    // harmless since miniaudio re-applies the seek on the next read.
    ma_sound_stop(&voice->sound);
    (void)ma_sound_seek_to_pcm_frame(&voice->sound, 0);
  } else {
    if (voice->initialized) {
      ma_sound_uninit(&voice->sound);
      voice->initialized = false;
    }

    auto result = ma_sound_init_from_file(
        engine_, filename.c_str(), kSoundFlags, nullptr, nullptr,
        &voice->sound);
    if (result != MA_SUCCESS) {
      voice->filename.clear();
      return result;
    }

    voice->initialized = true;
    voice->filename = filename;
  }

  // Apply parameters and start playing.
  ma_sound_set_volume(&voice->sound, volume / 100.0f);
  ma_sound_set_pitch(&voice->sound, pitch / 100.0f);

  auto result = ma_sound_start(&voice->sound);
  if (result != MA_SUCCESS) {
    // A failed start can leave the handle stuck in the end state without
    // ever producing sound (the internal seek back to the start failed).
    // Rebuild the handle once so subsequent plays are not silenced.
    ma_sound_uninit(&voice->sound);
    voice->initialized = false;

    result = ma_sound_init_from_file(
        engine_, filename.c_str(), kSoundFlags, nullptr, nullptr,
        &voice->sound);
    if (result != MA_SUCCESS) {
      voice->filename.clear();
      return result;
    }

    voice->initialized = true;
    ma_sound_set_volume(&voice->sound, volume / 100.0f);
    ma_sound_set_pitch(&voice->sound, pitch / 100.0f);

    result = ma_sound_start(&voice->sound);
    if (result != MA_SUCCESS) {
      ma_sound_uninit(&voice->sound);
      voice->initialized = false;
      voice->filename.clear();
      return result;
    }
  }

  voice->last_start_seq = ++play_seq_;

  return MA_SUCCESS;
}

void SoundEmit::Stop() {
  for (auto& voice : voices_) {
    if (voice->initialized) {
      ma_sound_uninit(&voice->sound);
      voice->initialized = false;
    }
    voice->filename.clear();
    voice->last_start_seq = 0;
  }
  play_seq_ = 0;
}

}  // namespace audioservice
