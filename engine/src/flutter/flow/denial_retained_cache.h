// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_DENIAL_RETAINED_CACHE_H_
#define FLUTTER_FLOW_DENIAL_RETAINED_CACHE_H_

#include <memory>

namespace flutter {

class DenialCategoryLayer;
struct PaintContext;

// Raster-thread storage for explicitly marked, texture-independent desktop
// drawing. The cache is empty for ordinary Flutter scenes.
class DenialRetainedCache final {
 public:
  DenialRetainedCache();
  ~DenialRetainedCache();

  // Returns false when this category must use normal Flutter painting.
  bool Draw(const DenialCategoryLayer& layer, PaintContext& context);
  void Clear();

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_DENIAL_RETAINED_CACHE_H_
