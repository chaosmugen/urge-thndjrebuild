// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "renderer/device/gpu_audit.h"

#if defined(__ANDROID__)

#include <atomic>

namespace renderer {

std::atomic<uint32_t> GpuAudit::next_slot_{0};
GpuAudit::Entry GpuAudit::entries_[GpuAudit::kCapacity];

}  // namespace renderer

#endif  // OS_ANDROID
