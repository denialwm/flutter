// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_LAYERS_CONTAINER_LAYER_H_
#define FLUTTER_FLOW_LAYERS_CONTAINER_LAYER_H_

#include <vector>

#include "flutter/flow/layers/layer.h"

namespace flutter {

class ContainerLayer : public Layer {
 public:
  ContainerLayer();

  void Diff(DiffContext* context, const Layer* old_layer) override;
  void PreservePaintRegion(DiffContext* context) override;

  virtual void Add(std::shared_ptr<Layer> layer);

  void Preroll(PrerollContext* context) override;
  void Paint(PaintContext& context) const override;

  const std::vector<std::shared_ptr<Layer>>& layers() const { return layers_; }

  virtual void DiffChildren(DiffContext* context,
                            const ContainerLayer* old_layer);

  void PaintChildren(PaintContext& context) const override;

  const ContainerLayer* as_container_layer() const override { return this; }

  const DlRect& child_paint_bounds() const { return child_paint_bounds_; }
  void set_child_paint_bounds(const DlRect& bounds) {
    child_paint_bounds_ = bounds;
  }

  int children_renderable_state_flags() const {
    return children_renderable_state_flags_;
  }
  void set_children_renderable_state_flags(int flags) {
    children_renderable_state_flags_ = flags;
  }

 protected:
  void PrerollChildren(PrerollContext* context, DlRect* child_paint_bounds);

 private:
  std::vector<std::shared_ptr<Layer>> layers_;
  DlRect child_paint_bounds_;
  int children_renderable_state_flags_ = 0;

  FML_DISALLOW_COPY_AND_ASSIGN(ContainerLayer);
};

// Explicit opt-in markers. Ordinary Flutter scenes contain neither layer and
// continue through the existing composition path.
class DenialSceneLayer final : public ContainerLayer {
 public:
  const DenialSceneLayer* as_denial_scene_layer() const override {
    return this;
  }
};

class DenialCategoryLayer final : public ContainerLayer {
 public:
  explicit DenialCategoryLayer(int category) : category_(category) {}

  void Preroll(PrerollContext* context) override;
  void Paint(PaintContext& context) const override;

  int category() const { return category_; }
  const DenialCategoryLayer* as_denial_category_layer() const override {
    return this;
  }

 private:
  const int category_;
  bool has_live_texture_ = false;
  bool has_readback_ = false;
  bool has_platform_view_ = false;
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_CONTAINER_LAYER_H_
