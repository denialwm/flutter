// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_texture_presentation.h"

#include <algorithm>
#include <cmath>

namespace flutter {

void DenialTexturePresentation::Reset() {
  presentation_ = {};
  presentation_.struct_size = sizeof(presentation_);
  background_sample_ = {};
  crop_only_ = false;
}

void DenialTexturePresentation::Prepare(
    const DlRect& image_bounds,
    const DenialFlutterExternalTexturePresentation* supplied) {
  Reset();
  if (supplied) {
    presentation_ = *supplied;
    const auto finite_rect = [](const double* rect) {
      return std::isfinite(rect[0]) && std::isfinite(rect[1]) &&
             std::isfinite(rect[2]) && std::isfinite(rect[3]) &&
             rect[2] >= 0 && rect[3] >= 0;
    };
    if (presentation_.struct_size != sizeof(presentation_) ||
        !std::isfinite(presentation_.width) ||
        !std::isfinite(presentation_.height) || presentation_.width <= 0 ||
        presentation_.height <= 0 || !finite_rect(presentation_.source) ||
        !finite_rect(presentation_.destination) ||
        !finite_rect(presentation_.background)) {
      Reset();
    }
  }
  if (presentation_.width <= 0) {
    return;
  }
  const auto visible_source =
      DlRect::MakeXYWH(presentation_.source[0], presentation_.source[1],
                       presentation_.source[2], presentation_.source[3])
          .Intersection(image_bounds);
  if (visible_source.has_value()) {
    // Retain coordinates, never a CPU copy of the colour. Both the strip
    // and app sample this same imported image, including on buffer-only
    // updates. Clamp small/cropped buffers to a texel they actually cover.
    const auto sample_axis = [](DlScalar start, DlScalar end) {
      return std::clamp(std::floor(start + 5), std::floor(start),
                        std::ceil(end) - 1);
    };
    background_sample_ = DlRect::MakeXYWH(
        sample_axis(visible_source->GetLeft(), visible_source->GetRight()),
        sample_axis(visible_source->GetTop(), visible_source->GetBottom()), 1,
        1);
  }
  // Without a backing strip, a source inside the image drawn into a
  // destination inside the canvas needs neither a clip nor a second draw:
  // the same single image draw as an unmodified texture.
  const auto source =
      DlRect::MakeXYWH(presentation_.source[0], presentation_.source[1],
                       presentation_.source[2], presentation_.source[3]);
  const auto destination = DlRect::MakeXYWH(
      presentation_.destination[0], presentation_.destination[1],
      presentation_.destination[2], presentation_.destination[3]);
  crop_only_ =
      (presentation_.background[2] <= 0 || presentation_.background[3] <= 0) &&
      !source.IsEmpty() && !destination.IsEmpty() &&
      image_bounds.Contains(source) &&
      DlRect::MakeWH(presentation_.width, presentation_.height)
          .Contains(destination);
}

bool DenialTexturePresentation::Direct() const {
  return presentation_.width <= 0 || crop_only_;
}

void DenialTexturePresentation::Paint(DlCanvas* canvas,
                                      const DlPaint* paint,
                                      const sk_sp<DlImage>& image,
                                      const DlRect& bounds,
                                      DlImageSampling sampling) const {
  if (!image) {
    return;
  }
  if (presentation_.width > 0) {
    const auto source =
        DlRect::MakeXYWH(presentation_.source[0], presentation_.source[1],
                         presentation_.source[2], presentation_.source[3]);
    const auto project = [&](const double* rect) {
      const double sx = bounds.GetWidth() / presentation_.width;
      const double sy = bounds.GetHeight() / presentation_.height;
      return DlRect::MakeXYWH(bounds.GetLeft() + rect[0] * sx,
                              bounds.GetTop() + rect[1] * sy, rect[2] * sx,
                              rect[3] * sy);
    };
    if (crop_only_) {
      // Strict sampling keeps filtering from reading texels outside the
      // crop, such as the unused rows of an over-allocated client buffer.
      canvas->DrawImageRect(image, source, project(presentation_.destination),
                            sampling, paint, DlSrcRectConstraint::kStrict);
      return;
    }
    canvas->Save();
    canvas->ClipRect(bounds);
    if (!background_sample_.IsEmpty()) {
      // Strict sampling clamps this 1x1 source to its texel centre. Replacing
      // the solid quad with a textured quad adds no draw, image allocation,
      // saveLayer, readback, or extra frame scheduling. Preserve pixel alpha
      // and the same inherited paint/opacity as the app content below it.
      canvas->DrawImageRect(image, background_sample_,
                            project(presentation_.background),
                            DlImageSampling::kNearestNeighbor, paint,
                            DlSrcRectConstraint::kStrict);
    } else {
      DlPaint background = paint ? *paint : DlPaint();
      background.setColor(DlColor(presentation_.background_argb)
                              .modulateOpacity(background.getOpacity()));
      canvas->DrawRect(project(presentation_.background), background);
    }
    if (!source.IsEmpty()) {
      canvas->DrawImageRect(image, source, project(presentation_.destination),
                            sampling, paint);
    }
    canvas->Restore();
    return;
  }
  DlRect image_bounds = DlRect::Make(image->GetBounds());
  if (bounds != image_bounds) {
    canvas->DrawImageRect(image, image_bounds, bounds, sampling, paint);
  } else {
    canvas->DrawImage(image, bounds.GetOrigin(), sampling, paint);
  }
}

void DenialTexturePresentation::PaintWindow(
    Texture::PaintContext& context,
    bool direct,
    const DlMatrix& texture_transform,
    const DlWindowSurfaceFilter& window,
    std::optional<int64_t> backdrop_id,
    DlScalar surface_opacity,
    const std::function<void(Texture::PaintContext&)>& paint_surface) {
  // Some mobile clients use the embedder's content-inset presentation, which
  // paints a sampled backing strip as well as the client. That is explicitly
  // a composed input, not an attempted direct draw that can silently decline.
  // A crop-only presentation remains the direct plan's single image.
  const DlWindowSurfaceFilter material(window.style(), window.backdrop(),
                                       direct);
  DlPaint restore = context.paint ? *context.paint : DlPaint();
  restore.setBlendMode(DlBlendMode::kSrc);
  context.canvas->SaveLayer(window.style().bounds, &restore, &material,
                            backdrop_id);
  if (!direct) {
    context.canvas->ClipRect(window.style().content_bounds);
  }
  context.canvas->Transform(texture_transform);
  DlPaint surface;
  surface.setOpacity(surface_opacity);
  auto child = context;
  child.paint = &surface;
  paint_surface(child);
  context.canvas->Restore();
}

}  // namespace flutter
