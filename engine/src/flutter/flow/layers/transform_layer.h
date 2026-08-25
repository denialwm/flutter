// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_LAYERS_TRANSFORM_LAYER_H_
#define FLUTTER_FLOW_LAYERS_TRANSFORM_LAYER_H_

#include "flutter/flow/layers/container_layer.h"

namespace flutter {

class TransformLayer : public ContainerLayer {
 public:
  explicit TransformLayer(const DlMatrix& transform);

  const DlMatrix& transform() const { return transform_; }

  void Diff(DiffContext* context, const Layer* old_layer) override;

  void Preroll(PrerollContext* context) override;

  void Paint(PaintContext& context) const override;

 private:
  DlMatrix transform_;

  FML_DISALLOW_COPY_AND_ASSIGN(TransformLayer);
};

// Translates children by a fraction of the active Denial render output's
// logical size. Ordinary Flutter views, snapshots, and embedders use the
// supplied fallback size, so the layer remains deterministic outside Denial's
// per-output raster traversal.
class OutputRelativeTransformLayer : public ContainerLayer {
 public:
  OutputRelativeTransformLayer(const DlPoint& offset_factor,
                               const DlSize& fallback_size);

  const DlPoint& offset_factor() const { return offset_factor_; }
  const DlSize& fallback_size() const { return fallback_size_; }

  void Diff(DiffContext* context, const Layer* old_layer) override;

  void Preroll(PrerollContext* context) override;

  void Paint(PaintContext& context) const override;

 private:
  DlMatrix ResolveTransform(const std::optional<DlSize>& output_size) const;

  DlPoint offset_factor_;
  DlSize fallback_size_;

  FML_DISALLOW_COPY_AND_ASSIGN(OutputRelativeTransformLayer);
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_TRANSFORM_LAYER_H_
