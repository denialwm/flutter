// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_BASE_FLAGS_H_
#define FLUTTER_IMPELLER_BASE_FLAGS_H_

namespace impeller {
struct Flags {
  /// When turned on DrawLine will use the experimental antialiased path.
  bool antialiased_lines = false;
  /// Use SDFs for rendering.
  bool use_sdfs = false;
  /// Denial: offscreen targets default to half float so content brighter
  /// than SDR white survives layers and filters.
  bool float_color = false;

  bool operator==(const Flags&) const = default;
};
}  // namespace impeller

#endif  // FLUTTER_IMPELLER_BASE_FLAGS_H_
