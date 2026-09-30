// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_DISPLAY_LIST_EFFECTS_IMAGE_FILTERS_DL_WINDOW_SURFACE_FILTER_H_
#define FLUTTER_DISPLAY_LIST_EFFECTS_IMAGE_FILTERS_DL_WINDOW_SURFACE_FILTER_H_

#include "flutter/display_list/dl_color.h"
#include "flutter/display_list/effects/dl_image_filter.h"

namespace flutter {

/// Explicit window material carried by a SaveLayer record. This is not a
/// general-purpose image filter and is never inferred from ordinary draws.
/// WindowSurfaceLayer is its producer; Canvas owns its direct/composed plans.
class DlWindowSurfaceFilter final : public DlImageFilter {
 public:
  struct Style {
    DlRect bounds;
    DlRect content_bounds;
    DlScalar radius = 0;
    DlColor frame_color = DlColor::kTransparent();
    bool operator==(const Style& other) const {
      return bounds == other.bounds && content_bounds == other.content_bounds &&
             radius == other.radius && frame_color == other.frame_color;
    }
  };

  DlWindowSurfaceFilter(Style style,
                        std::shared_ptr<DlImageFilter> backdrop,
                        bool direct_texture)
      : style_(style),
        backdrop_(std::move(backdrop)),
        direct_(direct_texture) {}

  DlImageFilterType type() const override {
    return DlImageFilterType::kWindowSurface;
  }
  size_t size() const override { return sizeof(*this); }
  std::shared_ptr<DlImageFilter> shared() const override {
    return std::make_shared<DlWindowSurfaceFilter>(*this);
  }
  const DlWindowSurfaceFilter* asWindowSurface() const override { return this; }
  const Style& style() const { return style_; }
  const std::shared_ptr<DlImageFilter>& backdrop() const { return backdrop_; }
  bool direct_texture() const { return direct_; }
  bool modifies_transparent_black() const override { return true; }

  DlRect* map_local_bounds(const DlRect&, DlRect& output) const override {
    output = style_.bounds.Expand(1);
    return &output;
  }
  DlIRect* map_device_bounds(const DlIRect&,
                             const DlMatrix& ctm,
                             DlIRect& output) const override {
    output = DlIRect::RoundOut(style_.bounds.TransformBounds(ctm).Expand(1));
    return &output;
  }
  DlIRect* get_input_device_bounds(const DlIRect& output,
                                   const DlMatrix&,
                                   DlIRect& input) const override {
    // Window-local sampling: the source outside the window is not a cache
    // dependency, even when the Gaussian kernel or glass refraction expands.
    input = output;
    return &input;
  }

 protected:
  bool equals_(const DlImageFilter& other) const override {
    const auto& window = *other.asWindowSurface();
    return style_ == window.style_ && direct_ == window.direct_ &&
           (backdrop_ == window.backdrop_ ||
            (backdrop_ && window.backdrop_ && *backdrop_ == *window.backdrop_));
  }

 private:
  Style style_;
  std::shared_ptr<DlImageFilter> backdrop_;
  bool direct_;
};

}  // namespace flutter
#endif  // FLUTTER_DISPLAY_LIST_EFFECTS_IMAGE_FILTERS_DL_WINDOW_SURFACE_FILTER_H_
