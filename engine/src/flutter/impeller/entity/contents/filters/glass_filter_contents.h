// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FILTER_CONTENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FILTER_CONTENTS_H_

#include "impeller/entity/contents/filters/filter_contents.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/round_rect.h"

namespace impeller {

struct GlassMaterialDraw {
  Rect coverage;
  Size material_size;
  Vector2 material_scale;
  Quad material_coordinates;
  Vector4 normal_transform;
};

// Resolves the portion of a glass material that needs drawing without
// redefining its rounded-box coordinate system to that damage crop.
std::optional<GlassMaterialDraw> ResolveGlassMaterialDraw(
    const Rect& coverage,
    const Rect& material_bounds,
    const Matrix& material_transform = Matrix());

bool GlassFrostNeedsBlur(Scalar sigma_x, Scalar sigma_y);

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
                      Scalar edge_strength,
                      Scalar bevel_width_scale = 1.0f,
                      Scalar refraction_depth_scale = 1.0f,
                      Scalar rim_width = 1.5f,
                      Scalar rim_falloff = 0.89f,
                      Scalar opposite_light_strength = 0.8f);

  ~GlassFilterContents() override;

  // Resolve a shader draw for a direct backdrop assignment. Unlike GetEntity,
  // this result is not TextureContents. Snapshot and alpha-threshold consumers
  // must keep using GetEntity so they retain the materialized texture.
  std::optional<Entity> GetDirectEntity(
      const ContentContext& renderer,
      const Entity& entity,
      const std::optional<Rect>& coverage_hint);

  // Maps the complete layout shape into backdrop input coordinates. Retain
  // orientation as well as bounds so damage crops, reflections and rotations
  // keep the same rounded boundary and optical normals.
  void SetMaterialTransform(const Matrix& transform);

  void SetMaterialTargetPaddingEnabled(bool enabled);

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
  const Scalar bevel_width_scale_;
  const Scalar refraction_depth_scale_;
  const Scalar rim_width_;
  const Scalar rim_falloff_;
  const Scalar opposite_light_strength_;
  std::optional<Matrix> material_transform_;
  bool render_material_directly_ = false;
  bool material_target_padding_enabled_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FILTER_CONTENTS_H_
