// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef FLUTTER_FLOW_LAYERS_FROZEN_GLASS_LAYER_H_
#define FLUTTER_FLOW_LAYERS_FROZEN_GLASS_LAYER_H_

#include <array>

#include "flutter/display_list/image/dl_image.h"
#include "flutter/flow/layers/container_layer.h"

namespace flutter {

// Denial glass over a frost made once for a whole scene. Denial's scene
// composer draws the material from the glass filter and the frost: the
// layer is a material cut without content, so the engine slice below it
// stays as it is while the glass moves. Otherwise, and when the engine
// composes the cut itself, the children draw the material.
class FrozenGlassLayer final : public ContainerLayer {
 public:
  // `overlay` is premultiplied RGBA.
  FrozenGlassLayer(std::shared_ptr<DlImageFilter> glass,
                   sk_sp<DlImage> frost,
                   DlRect frost_rect,
                   std::array<DlScalar, 4> overlay,
                   DlScalar opacity);

  void Preroll(PrerollContext* context) override;
  void Paint(PaintContext& context) const override;

 private:
  std::shared_ptr<DlImageFilter> glass_;
  sk_sp<DlImage> frost_;
  DlRect frost_rect_;
  std::array<DlScalar, 4> overlay_;
  DlScalar opacity_;
  // The children as painted last, shared with the cut.
  std::shared_ptr<sk_sp<DisplayList>> picture_ =
      std::make_shared<sk_sp<DisplayList>>();
  int64_t denial_cut_ = -1;
  int64_t denial_end_cut_ = -1;

  FML_DISALLOW_COPY_AND_ASSIGN(FrozenGlassLayer);
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_FROZEN_GLASS_LAYER_H_
