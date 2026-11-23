// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_PRESENTATION_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_PRESENTATION_H_

#include <functional>
#include <optional>

#include "flutter/common/graphics/texture.h"
#include "flutter/display_list/image/dl_image.h"
#include "flutter/shell/platform/embedder/embedder.h"

namespace flutter {

// How a Denial client texture's image is drawn: the presentation the
// embedder supplied for it, checked, and the draws it makes. The GLES and
// Vulkan textures draw through it, so both backends paint the same list.
class DenialTexturePresentation {
 public:
  // Forgets the last image's presentation.
  void Reset();

  // Takes `supplied` for an image of `image_bounds` (nullptr: none), or
  // drops it when it is malformed.
  void Prepare(const DlRect& image_bounds,
               const DenialFlutterExternalTexturePresentation* supplied);

  // The image alone, as the direct window plan samples it: no presentation,
  // or one that only crops.
  bool Direct() const;

  // Draws `image` into `bounds` as presented.
  void Paint(DlCanvas* canvas,
             const DlPaint* paint,
             const sk_sp<DlImage>& image,
             const DlRect& bounds,
             DlImageSampling sampling) const;

  // A window's layer around `paint_surface`, which draws the client with the
  // child context it is given.
  static void PaintWindow(
      Texture::PaintContext& context,
      bool direct,
      const DlMatrix& texture_transform,
      const DlWindowSurfaceFilter& window,
      std::optional<int64_t> backdrop_id,
      DlScalar surface_opacity,
      const std::function<void(Texture::PaintContext&)>& paint_surface);

 private:
  DenialFlutterExternalTexturePresentation presentation_ = {};
  DlRect background_sample_;
  // A presentation that only crops the image is still one image draw.
  bool crop_only_ = false;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_PRESENTATION_H_
