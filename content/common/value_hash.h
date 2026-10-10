// Copyright 2018-2025 Admenri.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_COMMON_VALUE_HASH_H_
#define CONTENT_COMMON_VALUE_HASH_H_

#include <cstring>
#include <stdint.h>

namespace content {

// Mixes |value| into |seed|.
//
// The comparable objects (Color / Rect / Tone) hash their members through this
// so that `hash` stays consistent with the member-wise CompareWithOther they
// already implement. Ruby's contract is that objects comparing equal must have
// equal `hash`; without it Hash / Set / Array#uniq never find the same value
// again, which is exactly what the engine's default (identity based) Object#hash
// produced.
inline uint64_t CombineValueHash(uint64_t seed, uint64_t value) {
  return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

inline uint64_t HashValuePart(int32_t value) {
  return static_cast<uint64_t>(static_cast<uint32_t>(value));
}

inline uint64_t HashValuePart(float value) {
  // -0.0f compares equal to 0.0f but has a different bit pattern, so fold it
  // first; NaN never compares equal to anything, so its payload is irrelevant.
  if (value == 0.0f)
    value = 0.0f;
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return static_cast<uint64_t>(bits);
}

constexpr uint64_t kValueHashSeed = 1469598103934665603ULL;

}  // namespace content

#endif  //! CONTENT_COMMON_VALUE_HASH_H_
