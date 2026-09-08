// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RENDERER_DEVICE_GPU_AUDIT_H_
#define RENDERER_DEVICE_GPU_AUDIT_H_

#if defined(__ANDROID__)

#include <atomic>
#include <cstdint>

#include "SDL3/SDL_timer.h"

namespace renderer {

// Ring buffer of the most recent GPU operations. The crash handler dumps it
// into the crash report, so the report tells exactly what the engine was doing
// on the GPU right before a driver crash - instead of having to guess from the
// crash address alone.
class GpuAudit {
 public:
  static constexpr int kCapacity = 256;

  struct Entry {
    const char* what;
    uint64_t timestamp;
    uint32_t seq;
    bool surface_valid;
  };

  static void Record(const char* what, bool surface_valid) {
    uint32_t slot = next_slot_.fetch_add(1, std::memory_order_relaxed);
    Entry& entry = entries_[slot % kCapacity];
    entry.what = what;
    entry.timestamp = SDL_GetPerformanceCounter();
    entry.seq = slot;
    entry.surface_valid = surface_valid;
  }

  static uint32_t count() {
    uint32_t n = next_slot_.load(std::memory_order_relaxed);
    return n < kCapacity ? n : kCapacity;
  }

  // Oldest-to-newest access: entries_[RingIndex(i)]
  static uint32_t RingIndex(uint32_t i) {
    uint32_t next = next_slot_.load(std::memory_order_relaxed);
    uint32_t base = next < kCapacity ? 0 : next;
    return (base + i) % kCapacity;
  }

  static const Entry& Get(uint32_t i) { return entries_[RingIndex(i)]; }

  // Timestamp of the entry, in milliseconds relative to the newest one.
  static double MillisBefore(const Entry& entry, const Entry& newest) {
    static const double freq =
        static_cast<double>(SDL_GetPerformanceFrequency());
    return static_cast<double>(newest.timestamp - entry.timestamp) * 1000.0 /
           freq;
  }

 private:
  static std::atomic<uint32_t> next_slot_;
  static Entry entries_[kCapacity];
};

}  // namespace renderer

#define URGE_GPU_AUDIT(what, surface_valid) \
  renderer::GpuAudit::Record(what, surface_valid)

#else

#define URGE_GPU_AUDIT(what, surface_valid) ((void)0)

#endif  // OS_ANDROID

#endif  // RENDERER_DEVICE_GPU_AUDIT_H_
