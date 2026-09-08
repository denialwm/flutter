// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/testing/testing.h"
#include "impeller/entity/contents/filters/glass_filter_contents.h"
#include "impeller/geometry/geometry_asserts.h"

namespace impeller {
namespace testing {

TEST(GlassFilterContentsTest, DamageCropKeepsFullMaterialCoordinates) {
  const Rect material_bounds = Rect::MakeXYWH(40, 30, 200, 100);
  const Rect damage_coverage = Rect::MakeXYWH(40, 30, 30, 20);

  const std::optional<GlassMaterialDraw> draw =
      ResolveGlassMaterialDraw(damage_coverage, material_bounds);

  ASSERT_TRUE(draw.has_value());
  EXPECT_EQ(draw->coverage, damage_coverage);
  EXPECT_EQ(draw->material_size, Size(200, 100));
  EXPECT_EQ(draw->material_coordinates[0], Point(0, 0));
}

TEST(GlassFilterContentsTest, InteriorDamageCropKeepsMaterialOffset) {
  const Rect material_bounds = Rect::MakeXYWH(40, 30, 200, 100);
  const Rect damage_coverage = Rect::MakeXYWH(90, 55, 30, 20);

  const std::optional<GlassMaterialDraw> draw =
      ResolveGlassMaterialDraw(damage_coverage, material_bounds);

  ASSERT_TRUE(draw.has_value());
  EXPECT_EQ(draw->coverage, damage_coverage);
  EXPECT_EQ(draw->material_size, Size(200, 100));
  EXPECT_EQ(draw->material_coordinates[0], Point(50, 25));
}

TEST(GlassFilterContentsTest, DamageOutsideMaterialIsRejected) {
  EXPECT_FALSE(ResolveGlassMaterialDraw(Rect::MakeXYWH(0, 0, 10, 10),
                                        Rect::MakeXYWH(40, 30, 200, 100))
                   .has_value());
}

TEST(GlassFilterContentsTest, UncachedLayerMatchesCachedMaterialCoordinates) {
  const Rect material_bounds = Rect::MakeXYWH(400, 300, 200, 50);
  const Matrix input_transform =
      Matrix::MakeTranslation(Vector3(-400, -300, 0));
  const Rect target_coverage = Rect::MakeXYWH(0, 0, 200, 50);
  const auto cached =
      ResolveGlassMaterialDraw(material_bounds, material_bounds);
  const auto uncached = ResolveGlassMaterialDraw(
      target_coverage, material_bounds, input_transform);

  ASSERT_TRUE(cached);
  ASSERT_TRUE(uncached);
  EXPECT_EQ(uncached->coverage,
            cached->coverage.TransformBounds(input_transform));
  EXPECT_EQ(uncached->material_size, cached->material_size);
  EXPECT_EQ(uncached->material_coordinates, cached->material_coordinates);
}

TEST(GlassFilterContentsTest, TranslatedDamageCropKeepsFullMaterialGeometry) {
  const Rect material_bounds = Rect::MakeXYWH(400.5f, 300.25f, 200, 50);
  const Rect damage_coverage = Rect::MakeXYWH(430.5f, 310.25f, 30, 20);
  // The intermediate target rounds its origin down. The input and the shape
  // must retain the same fractional offset within that target.
  const Matrix input_transform =
      Matrix::MakeTranslation(Vector3(-430, -310, 0));
  const auto draw =
      ResolveGlassMaterialDraw(damage_coverage.TransformBounds(input_transform),
                               material_bounds, input_transform);

  ASSERT_TRUE(draw);
  EXPECT_EQ(draw->coverage, Rect::MakeXYWH(0.5f, 0.25f, 30, 20));
  EXPECT_EQ(draw->material_size, Size(200, 50));
  EXPECT_EQ(draw->material_coordinates[0], Point(30, 10));
}

TEST(GlassFilterContentsTest, ReflectedShapeKeepsCornerIdentityAndNormals) {
  const Rect bounds = Rect::MakeXYWH(40, 30, 320, 200);
  const Matrix transform =
      Matrix::MakeTranslation({0, 600, 0}) * Matrix::MakeScale({2, -2, 1});
  const auto draw = ResolveGlassMaterialDraw(bounds.TransformBounds(transform),
                                             bounds, transform);
  ASSERT_TRUE(draw);
  EXPECT_EQ(draw->material_size, Size(640, 400));
  EXPECT_EQ(draw->material_scale, Vector2(2, 2));
  const Quad expected = {Point(0, 400), Point(640, 400), Point(0, 0),
                         Point(640, 0)};
  EXPECT_EQ(draw->material_coordinates, expected);
  EXPECT_EQ(draw->normal_transform, Vector4(1, 0, 0, -1));
}

TEST(GlassFilterContentsTest, ReflectionAndRotationKeepDamageInMaterialSpace) {
  const Rect bounds = Rect::MakeXYWH(40, 30, 320, 200);
  for (const Vector3 scale : {Vector3(2, 2, 1), Vector3(-2, 2, 1),
                              Vector3(2, -2, 1), Vector3(-2, -2, 1)}) {
    for (const Scalar angle : {0.0f, 30.0f, 90.0f, 180.0f}) {
      const Matrix transform = Matrix::MakeTranslation({800, 900, 0}) *
                               Matrix::MakeRotationZ(Degrees(angle)) *
                               Matrix::MakeScale(scale);
      const Rect full = bounds.TransformBounds(transform);
      const Rect damage =
          Rect::MakeXYWH(full.GetCenter().x, full.GetCenter().y, 10, 15);
      const auto draw = ResolveGlassMaterialDraw(damage, bounds, transform);
      ASSERT_TRUE(draw);
      const Quad target = damage.GetPoints();
      for (size_t i = 0; i < target.size(); i++) {
        const Point local =
            draw->material_coordinates[i] / 2 + bounds.GetOrigin();
        EXPECT_TRUE(PointNear(transform * local, target[i]));
      }
      EXPECT_TRUE(SizeNear(draw->material_size, Size(640, 400)));
      // The inverse-transpose normal follows the same reflected/rotated shape.
      const auto normal = draw->normal_transform;
      EXPECT_TRUE(PointNear(Point(normal.x, normal.y),
                            transform.TransformDirection(Vector2(1, 0)) / 2));
      EXPECT_TRUE(PointNear(Point(normal.z, normal.w),
                            transform.TransformDirection(Vector2(0, 1)) / 2));
    }
  }
}

TEST(GlassFilterContentsTest, SingularMaterialTransformIsRejected) {
  EXPECT_FALSE(ResolveGlassMaterialDraw(Rect::MakeWH(100, 100),
                                        Rect::MakeWH(100, 100),
                                        Matrix::MakeScale({0, 1, 1})));
}

TEST(GlassFilterContentsTest, ZeroFrostBypassesGaussianPass) {
  EXPECT_FALSE(GlassFrostNeedsBlur(0, 0));
  EXPECT_TRUE(GlassFrostNeedsBlur(1, 0));
  EXPECT_TRUE(GlassFrostNeedsBlur(0, 1));
}

TEST(GlassFilterContentsTest, ReflectedEffectTransformsShapeCoverage) {
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
  EXPECT_TRUE(
      RectNear(coverage.value(), Rect::MakeXYWH(0, -24.2f, 97.9f, 24.2f)));
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
  EXPECT_TRUE(RectNear(
      source.value(),
      output_limit.Expand(Vector2(kExpectedPadding, kExpectedPadding))));
}

}  // namespace testing
}  // namespace impeller
