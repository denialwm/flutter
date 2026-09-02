// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/testing/testing.h"
#include "impeller/entity/contents/filters/glass_filter_contents.h"
#include "impeller/geometry/geometry_asserts.h"

namespace impeller {
namespace testing {

TEST(GlassFilterContentsTest, ReflectedEffectKeepsShapeInTargetCoordinates) {
  GlassFilterContents contents(
      RoundRect::MakeRectXY(Rect::MakeXYWH(0, 0, 89, 22), 10, 10),
      /*thickness=*/20, /*refraction=*/0.55, /*dispersion=*/0.12,
      /*saturation=*/1.2, Color::White(), /*tint_strength=*/0.08,
      /*brightness=*/0.06, /*light_angle=*/0, /*light_intensity=*/0.7,
      /*edge_strength=*/0.4);
  contents.SetInputs({FilterInput::Make(Rect::MakeXYWH(0, 0, 1920, 1080))});
  contents.SetEffectTransform(Matrix::MakeScale(Vector3(1.1f, -1.1f, 1.0f)));

  Entity entity;
  const std::optional<Rect> coverage = contents.GetCoverage(entity);

  ASSERT_TRUE(coverage.has_value());
  EXPECT_TRUE(RectNear(coverage.value(), Rect::MakeXYWH(0, 0, 97.9f, 24.2f)));
}

TEST(GlassFilterContentsTest, BackdropCoverageUsesSaveLayerCoordinates) {
  GlassFilterContents contents(
      RoundRect::MakeRectXY(Rect::MakeLTRB(1, 1, 1232.8f, 826.5f), 20, 20),
      /*thickness=*/20, /*refraction=*/0.55, /*dispersion=*/0.12,
      /*saturation=*/1.2, Color::White(), /*tint_strength=*/0.08,
      /*brightness=*/0.06, /*light_angle=*/0, /*light_intensity=*/0.7,
      /*edge_strength=*/0.4);
  contents.SetInputs({FilterInput::Make(Rect::MakeXYWH(0, 0, 1920, 1080))});
  contents.SetEffectTransform(Matrix::MakeScale(Vector3(1.1f, -1.1f, 1.0f)));
  contents.SetIsBackdropFilter(true);
  const Rect save_layer_coverage = Rect::MakeLTRB(431, 105, 1787.1f, 1014.1f);
  contents.SetCoverageHint(save_layer_coverage);

  Entity entity;
  const std::optional<Rect> coverage = contents.GetCoverage(entity);

  ASSERT_TRUE(coverage.has_value());
  EXPECT_TRUE(RectNear(coverage.value(), save_layer_coverage));
}

TEST(GlassFilterContentsTest, SourceCoverageIncludesSnellRefractionDistance) {
  GlassFilterContents contents(
      RoundRect::MakeRectXY(Rect::MakeXYWH(0, 0, 400, 300), 20, 20),
      /*thickness=*/20, /*refraction=*/0.55, /*dispersion=*/0.12,
      /*saturation=*/1.2, Color::White(), /*tint_strength=*/0.08,
      /*brightness=*/0.06, /*light_angle=*/0, /*light_intensity=*/0.7,
      /*edge_strength=*/0.4);
  contents.SetInputs({FilterInput::Make(Rect::MakeXYWH(0, 0, 1920, 1080))});

  const Rect output_limit = Rect::MakeXYWH(200, 100, 400, 300);
  const std::optional<Rect> source =
      contents.GetSourceCoverage(Matrix(), output_limit);

  ASSERT_TRUE(source.has_value());
  // refraction 0.55 maps to IOR 1.11. At the rim, the 8 * thickness
  // optical path displaces 77.0828 px; dispersion expands that by 6%.
  constexpr Scalar kExpectedPadding = 81.7078f;
  EXPECT_TRUE(
      RectNear(source.value(), output_limit.Expand(Vector2(kExpectedPadding))));
}

}  // namespace testing
}  // namespace impeller
