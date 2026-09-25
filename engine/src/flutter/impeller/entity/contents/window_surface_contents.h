// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_WINDOW_SURFACE_CONTENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_WINDOW_SURFACE_CONTENTS_H_
#include "flutter/display_list/effects/image_filters/dl_window_surface_filter.h"
#include "impeller/entity/contents/texture_contents.h"
#include "impeller/entity/entity.h"
namespace impeller {
/// The final draw of an explicit WindowSurface, with no fast-path predicates.
/// Arbitrary child content is supplied as a composed texture by Canvas; an
/// imported client can be sampled directly (including strict source
/// rectangles).
class WindowSurfaceContents final : public Contents {
 public:
  struct Input {
    std::shared_ptr<TextureContents> contents;
    Matrix transform;
  };
  WindowSurfaceContents(flutter::DlWindowSurfaceFilter::Style style,
                        Input surface,
                        Input backdrop,
                        Scalar alpha_threshold,
                        Scalar opacity);
  std::optional<Rect> GetCoverage(const Entity& entity) const override;
  bool Render(const ContentContext& renderer,
              const Entity& entity,
              RenderPass& pass) const override;

 private:
  flutter::DlWindowSurfaceFilter::Style style_;
  Input surface_;
  Input backdrop_;
  Scalar threshold_;
  Scalar opacity_;
};
}  // namespace impeller
#endif
