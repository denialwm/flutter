// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_COMMON_BACKDROP_FILTER_CACHE_KEY_H_
#define FLUTTER_COMMON_BACKDROP_FILTER_CACHE_KEY_H_

#include <cstdint>

namespace flutter {

constexpr int64_t MakeBackdropFilterCacheKey(uint32_t family,
                                             uint32_t generation) {
  return static_cast<int64_t>((static_cast<uint64_t>(family) << 32u) |
                              generation);
}

constexpr uint32_t GetBackdropFilterCacheFamily(int64_t key) {
  return static_cast<uint32_t>(static_cast<uint64_t>(key) >> 32u);
}

}  // namespace flutter

#endif  // FLUTTER_COMMON_BACKDROP_FILTER_CACHE_KEY_H_
