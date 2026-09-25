// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_FLOW_LAYERS_WINDOW_SURFACE_LAYER_H_
#define FLUTTER_FLOW_LAYERS_WINDOW_SURFACE_LAYER_H_
#include "flutter/display_list/effects/image_filters/dl_window_surface_filter.h"
#include "flutter/flow/layers/container_layer.h"
namespace flutter {
class WindowSurfaceLayer final : public ContainerLayer {
 public:
  WindowSurfaceLayer(DlWindowSurfaceFilter::Style style,
                     std::shared_ptr<DlImageFilter> backdrop,
                     int64_t texture_id,
                     DlRect texture_bounds,
                     DlMatrix texture_transform,
                     DlImageSampling sampling,
                     DlScalar surface_opacity);
  void Diff(DiffContext* context, const Layer* old_layer) override;
  void Preroll(PrerollContext* context) override;
  void Paint(PaintContext& context) const override;

 private:
  DlWindowSurfaceFilter::Style style_;
  std::shared_ptr<DlImageFilter> backdrop_;
  // Immutable across retained texture-only frames.
  std::shared_ptr<DlWindowSurfaceFilter> material_;
  int64_t texture_id_;
  DlRect texture_bounds_;
  DlMatrix texture_transform_;
  DlImageSampling sampling_;
  DlScalar surface_opacity_;
  std::shared_ptr<BackdropFilterCacheState> cache_ =
      std::make_shared<BackdropFilterCacheState>();
  DlIRect cache_target_;
  DlMatrix cache_matrix_;
  bool cache_prepared_ = false;
};
}  // namespace flutter
#endif
