// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FILTER_CONTENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FILTER_CONTENTS_H_

#include "impeller/entity/contents/filters/filter_contents.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/round_rect.h"

namespace impeller {

class GlassFilterContents final : public FilterContents {
 public:
  GlassFilterContents(RoundRect shape,
                      Scalar thickness,
                      Scalar refraction,
                      Scalar dispersion,
                      Scalar saturation,
                      Color tint,
                      Scalar tint_strength,
                      Scalar brightness,
                      Scalar light_angle,
                      Scalar light_intensity,
                      Scalar edge_strength);

  ~GlassFilterContents() override;

  // Resolve a shader draw for a direct backdrop assignment. Unlike GetEntity,
  // this result is not TextureContents. Snapshot and alpha-threshold consumers
  // must keep using GetEntity so they retain the materialized texture.
  std::optional<Entity> GetDirectEntity(
      const ContentContext& renderer,
      const Entity& entity,
      const std::optional<Rect>& coverage_hint);

 private:
  std::optional<Entity> RenderFilter(
      const FilterInput::Vector& inputs,
      const ContentContext& renderer,
      const Entity& entity,
      const Matrix& effect_transform,
      const Rect& coverage,
      const std::optional<Rect>& coverage_hint) const override;

  std::optional<Rect> GetFilterCoverage(
      const FilterInput::Vector& inputs,
      const Entity& entity,
      const Matrix& effect_transform) const override;

  std::optional<Rect> GetFilterSourceCoverage(
      const Matrix& effect_transform,
      const Rect& output_limit) const override;

  const RoundRect shape_;
  const Scalar thickness_;
  const Scalar refraction_;
  const Scalar dispersion_;
  const Scalar saturation_;
  const Color tint_;
  const Scalar tint_strength_;
  const Scalar brightness_;
  const Scalar light_angle_;
  const Scalar light_intensity_;
  const Scalar edge_strength_;
  bool render_material_directly_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FILTER_CONTENTS_H_
