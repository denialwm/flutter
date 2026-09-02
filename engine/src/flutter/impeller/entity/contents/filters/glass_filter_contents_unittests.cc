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

}  // namespace testing
}  // namespace impeller
