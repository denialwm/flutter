// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/impeller/display_list/aiks_unittests.h"

#include "flutter/common/backdrop_filter_cache_key.h"
#include "flutter/display_list/dl_blend_mode.h"
#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/dl_color.h"
#include "flutter/display_list/dl_paint.h"
#include "flutter/display_list/dl_tile_mode.h"
#include "flutter/display_list/effects/dl_color_filter.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/display_list/effects/dl_mask_filter.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/testing/testing.h"

namespace impeller {
namespace testing {

using namespace flutter;

TEST_P(AiksTest, CanRenderNestedClips) {
  DisplayListBuilder builder;
  DlPaint paint;
  paint.setColor(DlColor::kFuchsia());

  builder.Save();
  builder.ClipPath(DlPath::MakeCircle(DlPoint(200, 400), 300));
  builder.Restore();
  builder.ClipPath(DlPath::MakeCircle(DlPoint(600, 400), 300));
  builder.ClipPath(DlPath::MakeCircle(DlPoint(400, 600), 300));
  builder.DrawRect(DlRect::MakeXYWH(200, 200, 400, 400), paint);

  ASSERT_TRUE(OpenPlaygroundHere(builder.Build()));
}

TEST_P(AiksTest, CanRenderDifferenceClips) {
  DisplayListBuilder builder;
  builder.Translate(400, 400);

  // Limit drawing to face circle with a clip.
  builder.ClipPath(DlPath::MakeCircle(DlPoint(0, 0), 200));
  builder.Save();

  // Cut away eyes/mouth using difference clips.
  builder.ClipPath(DlPath::MakeCircle(DlPoint(-100, -50), 30),
                   DlClipOp::kDifference);
  builder.ClipPath(DlPath::MakeCircle(DlPoint(100, -50), 30),
                   DlClipOp::kDifference);

  DlPathBuilder path_builder;
  path_builder.MoveTo(DlPoint(-100, 50));
  path_builder.QuadraticCurveTo(DlPoint(0, 150), DlPoint(100, 50));
  builder.ClipPath(path_builder.TakePath(), DlClipOp::kDifference);

  // Draw a huge yellow rectangle to prove the clipping works.
  DlPaint paint;
  paint.setColor(DlColor::kYellow());
  builder.DrawRect(DlRect::MakeXYWH(-1000, -1000, 2000, 2000), paint);

  // Remove the difference clips and draw hair that partially covers the eyes.
  builder.Restore();
  paint.setColor(DlColor::kMaroon());
  DlPathBuilder path_builder_2;
  path_builder_2.MoveTo(DlPoint(200, -200));
  path_builder_2.LineTo(DlPoint(-200, -200));
  path_builder_2.LineTo(DlPoint(-200, -40));
  path_builder_2.CubicCurveTo(DlPoint(0, -40), DlPoint(0, -80),
                              DlPoint(200, -80));

  builder.DrawPath(path_builder_2.TakePath(), paint);

  ASSERT_TRUE(OpenPlaygroundHere(builder.Build()));
}

TEST_P(AiksTest, CanRenderWithContiguousClipRestores) {
  DisplayListBuilder builder;

  // Cover the whole canvas with red.
  DlPaint paint;
  paint.setColor(DlColor::kRed());
  builder.DrawPaint(paint);

  builder.Save();

  // Append two clips, the second resulting in empty coverage.
  builder.ClipRect(DlRect::MakeXYWH(100, 100, 100, 100));
  builder.ClipRect(DlRect::MakeXYWH(300, 300, 100, 100));

  // Restore to no clips.
  builder.Restore();

  // Replace the whole canvas with green.
  paint.setColor(DlColor::kGreen());
  builder.DrawPaint(paint);

  ASSERT_TRUE(OpenPlaygroundHere(builder.Build()));
}

TEST_P(AiksTest, ClipsUseCurrentTransform) {
  std::array<DlColor, 5> colors = {DlColor::kWhite(), DlColor::kBlack(),
                                   DlColor::kSkyBlue(), DlColor::kRed(),
                                   DlColor::kYellow()};
  DisplayListBuilder builder;
  DlPaint paint;

  builder.Translate(300, 300);
  for (int i = 0; i < 15; i++) {
    builder.Scale(0.8, 0.8);

    paint.setColor(colors[i % colors.size()]);
    builder.ClipPath(DlPath::MakeCircle(DlPoint(0, 0), 300));
    builder.DrawRect(DlRect::MakeXYWH(-300, -300, 600, 600), paint);
  }
  ASSERT_TRUE(OpenPlaygroundHere(builder.Build()));
}

/// If correct, this test should draw a green circle. If any red is visible,
/// there is a depth bug.
TEST_P(AiksTest, FramebufferBlendsRespectClips) {
  DisplayListBuilder builder;

  // Clear the whole canvas with white.
  DlPaint paint;
  paint.setColor(DlColor::kWhite());
  builder.DrawPaint(paint);

  builder.ClipPath(DlPath::MakeCircle(DlPoint(150, 150), 50),
                   DlClipOp::kIntersect);

  // Draw a red rectangle that should not show through the circle clip.
  paint.setColor(DlColor::kRed());
  paint.setBlendMode(DlBlendMode::kMultiply);
  builder.DrawRect(DlRect::MakeXYWH(100, 100, 100, 100), paint);

  // Draw a green circle that shows through the clip.
  paint.setColor(DlColor::kGreen());
  paint.setBlendMode(DlBlendMode::kSrcOver);
  builder.DrawCircle(DlPoint(150, 150), 50, paint);

  ASSERT_TRUE(OpenPlaygroundHere(builder.Build()));
}

// Backdrop filters split the parent render pass. Depth/stencil attachment
// contents may survive that split, but dynamic encoder state such as the
// scissor does not. The frame drawn after the backdrop restore must remain
// bounded by the outer reveal clip instead of appearing at its final bounds.
TEST_P(AiksTest, BackdropFlipRestoresOuterClipForPostFilterFrame) {
  DisplayListBuilder builder;

  DlPaint paint;
  paint.setColor(DlColor::kCornflowerBlue());
  builder.DrawPaint(paint);

  builder.Save();
  builder.ClipPath(DlPath::MakeCircle(DlPoint(200, 200), 72));

  DlPaint backdrop_paint;
  backdrop_paint.setBlendMode(DlBlendMode::kSrc);
  auto backdrop_filter = DlImageFilter::MakeBlur(12, 12, DlTileMode::kClamp);
  builder.SaveLayer(DlRect::MakeXYWH(48, 48, 304, 304), &backdrop_paint,
                    backdrop_filter.get());
  paint.setColor(DlColor::ARGB(0x88, 0x10, 0x18, 0x24));
  builder.DrawRect(DlRect::MakeXYWH(48, 48, 304, 304), paint);
  builder.Restore();

  paint.setColor(DlColor::kLimeGreen());
  paint.setDrawStyle(DlDrawStyle::kStroke);
  paint.setStrokeWidth(8);
  builder.DrawRoundRect(
      DlRoundRect::MakeRectXY(DlRect::MakeXYWH(48, 48, 304, 304), 28, 28),
      paint);
  builder.Restore();

  ASSERT_TRUE(OpenPlaygroundHere(builder.Build()));
}

// Integral non-AA rectangle clips are represented only by the render-pass
// scissor. Unlike a stencil clip, no attachment contents can preserve them.
TEST_P(AiksTest, BackdropFlipRestoresScissorOnlyClip) {
  DisplayListBuilder builder;

  DlPaint paint;
  paint.setColor(DlColor::kCornflowerBlue());
  builder.DrawPaint(paint);

  builder.Save();
  builder.ClipRect(DlRect::MakeXYWH(128, 128, 144, 144), DlClipOp::kIntersect,
                   /*is_aa=*/false);

  DlPaint backdrop_paint;
  backdrop_paint.setBlendMode(DlBlendMode::kSrc);
  auto backdrop_filter = DlImageFilter::MakeBlur(12, 12, DlTileMode::kClamp);
  builder.SaveLayer(DlRect::MakeXYWH(48, 48, 304, 304), &backdrop_paint,
                    backdrop_filter.get());
  paint.setColor(DlColor::ARGB(0x88, 0x10, 0x18, 0x24));
  builder.DrawRect(DlRect::MakeXYWH(48, 48, 304, 304), paint);
  builder.Restore();

  // The frame lies completely outside the active scissor and must not appear.
  paint.setColor(DlColor::kLimeGreen());
  paint.setDrawStyle(DlDrawStyle::kStroke);
  paint.setStrokeWidth(8);
  builder.DrawRoundRect(
      DlRoundRect::MakeRectXY(DlRect::MakeXYWH(48, 48, 304, 304), 28, 28),
      paint);
  builder.Restore();

  ASSERT_TRUE(OpenPlaygroundHere(builder.Build()));
}

// A direct backdrop is evaluated in the loaded parent target instead of a
// physical saveLayer. Its filter footprint may read the preceding shadow, but
// neither that footprint nor the resumed pass may write outside the active
// hard clip. The orange ring makes any escaped blur immediately visible.
TEST_P(AiksTest, DirectBackdropPreservesPrecedingShadowOutsideHardClip) {
  uint32_t generation = 0u;
  auto build_frame = [&]() {
    DisplayListBuilder builder;

    DlPaint paint;
    paint.setColor(DlColor::kCornflowerBlue());
    builder.DrawPaint(paint);

    const DlRect window_bounds = DlRect::MakeXYWH(96, 96, 288, 208);
    const DlRoundRect window_shape =
        DlRoundRect::MakeRectXY(window_bounds, 24, 24);

    DlPaint shadow_paint;
    shadow_paint.setColor(DlColor::ARGB(0xA0, 0x08, 0x0C, 0x14));
    shadow_paint.setMaskFilter(
        DlBlurMaskFilter::Make(DlBlurStyle::kNormal, 18));
    builder.Save();
    builder.Translate(0, 14);
    builder.DrawRoundRect(window_shape, shadow_paint);
    builder.Restore();

    DlPaint canary_paint;
    canary_paint.setColor(DlColor::ARGB(0xFF, 0xFF, 0x98, 0x18));
    canary_paint.setDrawStyle(DlDrawStyle::kStroke);
    canary_paint.setStrokeWidth(6);
    builder.DrawRoundRect(
        DlRoundRect::MakeRectXY(window_bounds.Expand(20), 40, 40),
        canary_paint);

    builder.Save();
    builder.ClipRect(window_bounds, DlClipOp::kIntersect, /*is_aa=*/false);

    DlPaint backdrop_paint;
    backdrop_paint.setBlendMode(DlBlendMode::kSrc);
    auto backdrop_filter = DlImageFilter::MakeBlur(14, 14, DlTileMode::kClamp);
    const int64_t backdrop_key = MakeBackdropFilterCacheKey(91u, ++generation);
    builder.SaveLayer(window_bounds.Expand(40), &backdrop_paint,
                      backdrop_filter.get(), backdrop_key);
    builder.Restore();

    paint.setColor(DlColor::ARGB(0x70, 0x10, 0x18, 0x24));
    builder.DrawRect(window_bounds, paint);
    paint.setColor(DlColor::kLimeGreen());
    paint.setDrawStyle(DlDrawStyle::kStroke);
    paint.setStrokeWidth(5);
    builder.DrawRoundRect(window_shape, paint);
    builder.Restore();

    return builder.Build();
  };

  ASSERT_TRUE(OpenPlaygroundHere(build_frame));
}

}  // namespace testing
}  // namespace impeller
