// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_LAYERS_BACKDROP_FILTER_LAYER_H_
#define FLUTTER_FLOW_LAYERS_BACKDROP_FILTER_LAYER_H_

#include "flutter/flow/layers/container_layer.h"

namespace flutter {

class BackdropFilterLayer : public ContainerLayer {
 public:
  BackdropFilterLayer(const std::shared_ptr<DlImageFilter>& filter,
                      DlBlendMode blend_mode,
                      std::optional<int64_t> backdrop_id = std::nullopt);

  void Diff(DiffContext* context, const Layer* old_layer) override;

  void Preroll(PrerollContext* context) override;

  void Paint(PaintContext& context) const override;

 private:
  std::shared_ptr<DlImageFilter> filter_;
  DlBlendMode blend_mode_;
  std::optional<int64_t> backdrop_id_;
  std::shared_ptr<BackdropFilterCacheState> backdrop_cache_state_ =
      std::make_shared<BackdropFilterCacheState>();
  DlIRect backdrop_cache_target_;
  DlIRect backdrop_cache_input_;
  DlMatrix backdrop_cache_matrix_;
  bool backdrop_cache_prepared_ = false;

  FML_DISALLOW_COPY_AND_ASSIGN(BackdropFilterLayer);
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_BACKDROP_FILTER_LAYER_H_
