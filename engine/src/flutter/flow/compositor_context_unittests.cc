// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/compositor_context.h"

#include <initializer_list>
#include <optional>
#include <vector>

#include "flutter/flow/layers/backdrop_filter_layer.h"
#include "flutter/flow/layers/clip_rect_layer.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/layers/texture_layer.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {
namespace {

constexpr DlISize kFrameSize = DlISize(100, 100);
const DlIRect kFullFrame = DlIRect::MakeSize(kFrameSize);

void ExpectRegion(const DlRegion& region,
                  std::initializer_list<DlIRect> expected) {
  const DlRegion expected_region{std::vector<DlIRect>(expected)};
  EXPECT_EQ(region.getRects(), expected_region.getRects());
}

void ExpectFullRepaint(const RasterDamagePlan& plan) {
  EXPECT_FALSE(plan.repaint_region.has_value());
  ExpectRegion(plan.buffer_damage, {kFullFrame});
}

class CountingTextureLayer final : public TextureLayer {
 public:
  CountingTextureLayer(const DlPoint& offset,
                       const DlSize& size,
                       int64_t texture_id)
      : TextureLayer(offset,
                     size,
                     texture_id,
                     false,
                     DlImageSampling::kLinear) {}

  void Diff(DiffContext* context, const Layer* old_layer) override {
    diff_count_++;
    TextureLayer::Diff(context, old_layer);
  }

  int diff_count() const { return diff_count_; }

 private:
  int diff_count_ = 0;
};

TEST(RasterDamagePlanTest, UnknownDamageRepaintsFullTarget) {
  const RasterDamagePlan plan = RasterDamagePlan::Make(
      std::nullopt, kFrameSize, RasterDamagePolicy::kUseDamageRegion,
      RasterBackend::kSkiaGanesh);
  ExpectFullRepaint(plan);
}

TEST(RasterDamagePlanTest, ExplicitPolicyRepaintsFullTarget) {
  const DlRegion damage(DlIRect::MakeLTRB(10, 20, 30, 40));
  const RasterDamagePlan plan = RasterDamagePlan::Make(
      damage, kFrameSize, RasterDamagePolicy::kFullRepaint,
      RasterBackend::kSkiaGanesh);
  ExpectFullRepaint(plan);
}

TEST(RasterDamagePlanTest, EmptyDamageRemainsKnownEmpty) {
  const RasterDamagePlan plan = RasterDamagePlan::Make(
      DlRegion(), kFrameSize, RasterDamagePolicy::kUseDamageRegion,
      RasterBackend::kSkiaGanesh);
  ASSERT_TRUE(plan.repaint_region.has_value());
  ExpectRegion(*plan.repaint_region, {});
  ExpectRegion(plan.buffer_damage, {});
}

TEST(RasterDamagePlanTest, SimpleDamageRemainsExact) {
  const DlIRect rect = DlIRect::MakeLTRB(10, 20, 30, 40);
  for (const RasterBackend backend :
       {RasterBackend::kSkiaGanesh, RasterBackend::kSkiaSoftware,
        RasterBackend::kImpeller}) {
    const RasterDamagePlan plan =
        RasterDamagePlan::Make(DlRegion(rect), kFrameSize,
                               RasterDamagePolicy::kUseDamageRegion, backend);
    ASSERT_TRUE(plan.repaint_region.has_value());
    ExpectRegion(*plan.repaint_region, {rect});
    ExpectRegion(plan.buffer_damage, {rect});
  }
}

TEST(RasterDamagePlanTest, GaneshUsesBoundingScissorForComplexDamage) {
  const DlIRect first = DlIRect::MakeLTRB(5, 6, 20, 30);
  const DlIRect second = DlIRect::MakeLTRB(40, 10, 60, 25);
  const DlIRect bounds = DlIRect::MakeLTRB(5, 6, 60, 30);
  const RasterDamagePlan plan = RasterDamagePlan::Make(
      DlRegion({first, second}), kFrameSize,
      RasterDamagePolicy::kUseDamageRegion, RasterBackend::kSkiaGanesh);

  ASSERT_TRUE(plan.repaint_region.has_value());
  ExpectRegion(*plan.repaint_region, {bounds});
  ExpectRegion(plan.buffer_damage, {bounds});
}

TEST(RasterDamagePlanTest, GaneshDropsTargetSizedComplexClip) {
  const DlRegion sparse_damage(
      {DlIRect::MakeLTRB(0, 0, 10, 100), DlIRect::MakeLTRB(90, 0, 100, 100)});
  const RasterDamagePlan plan = RasterDamagePlan::Make(
      sparse_damage, kFrameSize, RasterDamagePolicy::kUseDamageRegion,
      RasterBackend::kSkiaGanesh);
  ExpectFullRepaint(plan);
}

TEST(RasterDamagePlanTest, SoftwareSkiaKeepsComplexDamageExact) {
  const DlIRect first = DlIRect::MakeLTRB(5, 6, 20, 30);
  const DlIRect second = DlIRect::MakeLTRB(40, 10, 60, 25);
  const RasterDamagePlan plan = RasterDamagePlan::Make(
      DlRegion({first, second}), kFrameSize,
      RasterDamagePolicy::kUseDamageRegion, RasterBackend::kSkiaSoftware);

  ASSERT_TRUE(plan.repaint_region.has_value());
  ExpectRegion(*plan.repaint_region, {first, second});
  ExpectRegion(plan.buffer_damage, {first, second});
}

TEST(RasterDamagePlanTest, ImpellerKeepsSparseComplexDamageExact) {
  const DlIRect first = DlIRect::MakeLTRB(0, 0, 10, 100);
  const DlIRect second = DlIRect::MakeLTRB(90, 0, 100, 100);
  const RasterDamagePlan plan = RasterDamagePlan::Make(
      DlRegion({first, second}), kFrameSize,
      RasterDamagePolicy::kUseDamageRegion, RasterBackend::kImpeller);

  ASSERT_TRUE(plan.repaint_region.has_value());
  ExpectRegion(*plan.repaint_region, {first, second});
  ExpectRegion(plan.buffer_damage, {first, second});
}

TEST(RasterDamagePlanTest, ImpellerRejectsLargeComplexDamage) {
  const DlRegion large_damage(
      {DlIRect::MakeLTRB(0, 0, 40, 100), DlIRect::MakeLTRB(60, 0, 100, 100)});
  const RasterDamagePlan plan = RasterDamagePlan::Make(
      large_damage, kFrameSize, RasterDamagePolicy::kUseDamageRegion,
      RasterBackend::kImpeller);
  ExpectFullRepaint(plan);
}

TEST(FrameDamageTest, ReusedTreeDamagesDirtyTextureWithoutDiffingLayers) {
  auto root = std::make_shared<ContainerLayer>();
  auto first =
      std::make_shared<CountingTextureLayer>(DlPoint(), DlSize(20, 20), 1);
  auto second = std::make_shared<CountingTextureLayer>(DlPoint(50, 10),
                                                       DlSize(20, 30), 2);
  root->Add(first);
  root->Add(second);
  LayerTree tree(root, kFrameSize);

  FrameDamage initial_frame;
  initial_frame.ComputeDamageRegion(tree, true, false);
  EXPECT_EQ(first->diff_count(), 1);
  EXPECT_EQ(second->diff_count(), 1);

  const std::unordered_set<int64_t> dirty_texture_ids = {2};
  FrameDamage autonomous_frame;
  autonomous_frame.SetPreviousLayerTree(&tree);
  autonomous_frame.SetDirtyTextureIds(&dirty_texture_ids);
  autonomous_frame.SetExistingDamage(DlRegion());
  autonomous_frame.ComputeDamageRegion(tree, true, false);

  EXPECT_EQ(first->diff_count(), 1);
  EXPECT_EQ(second->diff_count(), 1);
  ASSERT_TRUE(autonomous_frame.GetFrameDamage().has_value());
  ExpectRegion(*autonomous_frame.GetFrameDamage(),
               {DlIRect::MakeLTRB(50, 10, 70, 40)});
}

TEST(FrameDamageTest, ReusedTreePreservesReadbackDamageDependencies) {
  auto root = std::make_shared<ContainerLayer>();
  auto texture = std::make_shared<CountingTextureLayer>(DlPoint(10, 10),
                                                        DlSize(10, 10), 7);
  root->Add(texture);

  auto clip = std::make_shared<ClipRectLayer>(DlRect::MakeLTRB(60, 60, 80, 80),
                                              Clip::kHardEdge);
  auto filter = DlImageFilter::MakeMatrix(DlMatrix::MakeTranslation({50, 50}),
                                          DlImageSampling::kLinear);
  clip->Add(
      std::make_shared<BackdropFilterLayer>(filter, DlBlendMode::kSrcOver));
  root->Add(clip);
  LayerTree tree(root, kFrameSize);

  FrameDamage initial_frame;
  initial_frame.ComputeDamageRegion(tree, true, false);
  EXPECT_EQ(texture->diff_count(), 1);

  const std::unordered_set<int64_t> dirty_texture_ids = {7};
  FrameDamage autonomous_frame;
  autonomous_frame.SetPreviousLayerTree(&tree);
  autonomous_frame.SetDirtyTextureIds(&dirty_texture_ids);
  autonomous_frame.SetExistingDamage(DlRegion());
  autonomous_frame.ComputeDamageRegion(tree, true, false);

  EXPECT_EQ(texture->diff_count(), 1);
  ASSERT_TRUE(autonomous_frame.GetFrameDamage().has_value());
  ExpectRegion(
      *autonomous_frame.GetFrameDamage(),
      {DlIRect::MakeLTRB(10, 10, 30, 30), DlIRect::MakeLTRB(60, 60, 80, 80)});
}

}  // namespace
}  // namespace testing
}  // namespace flutter
