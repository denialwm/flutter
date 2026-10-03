// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_BLUR_WORKSPACE_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_BLUR_WORKSPACE_H_

#include <array>
#include <optional>

#include "impeller/geometry/size.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

/// Two persistent render targets for the passes of a Gaussian blur whose
/// result is consumed before any other blur renders.
///
/// Such a blur renders each pass into the top-left region of one target and
/// reads the previous pass from the other. A blur sized by a moving or
/// resizing window would otherwise allocate new targets on every frame. GLES
/// executes passes in encoding order, so a later blur's writes follow every
/// read of the previous result.
class BlurWorkspace {
 public:
  BlurWorkspace();

  ~BlurWorkspace();

  /// Returns both targets, grown to contain at least `size`. They never
  /// shrink. Returns nothing when they cannot contain `size`.
  std::optional<std::array<RenderTarget, 2>> Get(const Context& context,
                                                 ISize size);

 private:
  std::optional<std::array<RenderTarget, 2>> targets_;

  BlurWorkspace(const BlurWorkspace&) = delete;

  BlurWorkspace& operator=(const BlurWorkspace&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_BLUR_WORKSPACE_H_
