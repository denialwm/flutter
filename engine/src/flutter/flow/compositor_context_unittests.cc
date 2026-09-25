// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/effects/image_filters/dl_blur_image_filter.h"
#include "flutter/display_list/effects/image_filters/dl_glass_image_filter.h"
#include "flutter/flow/compositor_context.h"

#include <initializer_list>
#include <optional>
#include <vector>

#include "flutter/flow/layers/backdrop_filter_layer.h"
#include "flutter/flow/layers/clip_rect_layer.h"
#include "flutter/flow/layers/image_filter_layer.h"
#include "flutter/flow/layers/layer_tree.h"
#include "flutter/flow/layers/texture_layer.h"
#include "flutter/flow/layers/transform_layer.h"
#include "flutter/flow/layers/window_surface_layer.h"
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

class CountingBackdropFilterLayer final : public BackdropFilterLayer {
 public:
  using BackdropFilterLayer::BackdropFilterLayer;

  void Diff(DiffContext* context, const Layer* old_layer) override {
    diff_count_++;
    BackdropFilterLayer::Diff(context, old_layer);
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

TEST(FrameDamageTest, RetainedTextureSubtreeReusesCompleteDiffMetadata) {
  auto retained = std::make_shared<ContainerLayer>();
  auto retained_texture = std::make_shared<CountingTextureLayer>(
      DlPoint(10, 10), DlSize(20, 20), 7);
  retained->Add(retained_texture);
  const std::unordered_set<int64_t> first_dirty_texture = {100};

  auto first_root = std::make_shared<ContainerLayer>();
  first_root->Add(retained);
  first_root->Add(std::make_shared<TextureLayer>(
      DlPoint(60, 0), DlSize(10, 10), 100, false, DlImageSampling::kLinear));
  LayerTree first(first_root, kFrameSize);
  FrameDamage first_frame;
  first_frame.SetDirtyTextureIds(&first_dirty_texture);
  first_frame.ComputeDamageRegion(first, true, false);
  EXPECT_EQ(retained_texture->diff_count(), 1);

  // The first retained frame performs the normal diff once and captures the
  // complete metadata block for subsequent frames.
  auto second_root = std::make_shared<ContainerLayer>();
  second_root->Add(retained);
  second_root->Add(std::make_shared<TextureLayer>(
      DlPoint(70, 0), DlSize(10, 10), 101, false, DlImageSampling::kLinear));
  LayerTree second(second_root, kFrameSize);
  const std::unordered_set<int64_t> second_dirty_texture = {101};
  FrameDamage second_frame;
  second_frame.SetPreviousLayerTree(&first);
  second_frame.SetDirtyTextureIds(&second_dirty_texture);
  second_frame.SetExistingDamage(DlRegion());
  second_frame.ComputeDamageRegion(second, true, false);
  EXPECT_EQ(retained_texture->diff_count(), 2);
  ASSERT_TRUE(
      second.retained_subtree_diff_metadata().contains(retained->unique_id()));
  const auto metadata =
      second.retained_subtree_diff_metadata().at(retained->unique_id());
  EXPECT_EQ(metadata->layer_paint_regions.size(), 2u);
  ASSERT_EQ(metadata->texture_paint_regions.size(), 1u);
  EXPECT_EQ(metadata->texture_paint_regions.front().texture_id, 7);

  // Changing an unrelated sibling still produces exact sibling damage, but
  // the retained textured child is not visited and its metadata block is
  // shared with the new LayerTree.
  auto third_root = std::make_shared<ContainerLayer>();
  third_root->Add(retained);
  third_root->Add(std::make_shared<TextureLayer>(
      DlPoint(80, 0), DlSize(10, 10), 102, false, DlImageSampling::kLinear));
  LayerTree third(third_root, kFrameSize);
  const std::unordered_set<int64_t> third_dirty_texture = {102};
  FrameDamage third_frame;
  third_frame.SetPreviousLayerTree(&second);
  third_frame.SetDirtyTextureIds(&third_dirty_texture);
  third_frame.SetExistingDamage(DlRegion());
  third_frame.ComputeDamageRegion(third, true, false);

  EXPECT_EQ(retained_texture->diff_count(), 2);
  ASSERT_TRUE(
      third.retained_subtree_diff_metadata().contains(retained->unique_id()));
  EXPECT_EQ(third.retained_subtree_diff_metadata().at(retained->unique_id()),
            metadata);
  ASSERT_TRUE(third_frame.GetFrameDamage().has_value());
  ExpectRegion(*third_frame.GetFrameDamage(),
               {DlIRect::MakeLTRB(70, 0, 90, 10)});

  const std::unordered_set<int64_t> no_dirty_textures;
  auto fourth_root = std::make_shared<ContainerLayer>();
  fourth_root->Add(retained);
  LayerTree fourth(fourth_root, kFrameSize);
  FrameDamage fourth_frame;
  fourth_frame.SetPreviousLayerTree(&third);
  fourth_frame.SetDirtyTextureIds(&no_dirty_textures);
  fourth_frame.SetExistingDamage(DlRegion());
  fourth_frame.ComputeDamageRegion(fourth, true, false);
  EXPECT_EQ(retained_texture->diff_count(), 2);
  ASSERT_TRUE(fourth_frame.GetFrameDamage().has_value());
  ExpectRegion(*fourth_frame.GetFrameDamage(),
               {DlIRect::MakeLTRB(80, 0, 90, 10)});
}

TEST(FrameDamageTest, RetainedTextureMetadataChecksEveryTextureOccurrence) {
  auto retained = std::make_shared<ContainerLayer>();
  auto first_texture =
      std::make_shared<CountingTextureLayer>(DlPoint(0, 10), DlSize(10, 10), 7);
  auto second_texture = std::make_shared<CountingTextureLayer>(
      DlPoint(30, 10), DlSize(10, 10), 7);
  retained->Add(first_texture);
  retained->Add(second_texture);

  auto make_tree = [&](int64_t changing_texture_id) {
    auto root = std::make_shared<ContainerLayer>();
    root->Add(retained);
    root->Add(std::make_shared<TextureLayer>(DlPoint(80, 80), DlSize(10, 10),
                                             changing_texture_id, false,
                                             DlImageSampling::kLinear));
    return std::make_unique<LayerTree>(root, kFrameSize);
  };

  auto first = make_tree(100);
  const std::unordered_set<int64_t> first_dirty_texture = {100};
  FrameDamage first_frame;
  first_frame.SetDirtyTextureIds(&first_dirty_texture);
  first_frame.ComputeDamageRegion(*first, true, false);

  auto second = make_tree(101);
  const std::unordered_set<int64_t> second_dirty_texture = {101};
  FrameDamage second_frame;
  second_frame.SetPreviousLayerTree(first.get());
  second_frame.SetDirtyTextureIds(&second_dirty_texture);
  second_frame.SetExistingDamage(DlRegion());
  second_frame.ComputeDamageRegion(*second, true, false);
  ASSERT_EQ(second->retained_subtree_diff_metadata()
                .at(retained->unique_id())
                ->texture_paint_regions.size(),
            2u);

  const std::unordered_set<int64_t> unrelated_texture = {99, 102};
  auto third = make_tree(102);
  FrameDamage third_frame;
  third_frame.SetPreviousLayerTree(second.get());
  third_frame.SetDirtyTextureIds(&unrelated_texture);
  third_frame.SetExistingDamage(DlRegion());
  third_frame.ComputeDamageRegion(*third, true, false);
  EXPECT_EQ(first_texture->diff_count(), 2);
  EXPECT_EQ(second_texture->diff_count(), 2);

  const std::unordered_set<int64_t> dirty_shared_texture = {7, 103};
  auto fourth = make_tree(103);
  FrameDamage fourth_frame;
  fourth_frame.SetPreviousLayerTree(third.get());
  fourth_frame.SetDirtyTextureIds(&dirty_shared_texture);
  fourth_frame.SetExistingDamage(DlRegion());
  fourth_frame.ComputeDamageRegion(*fourth, true, false);
  EXPECT_EQ(first_texture->diff_count(), 3);
  EXPECT_EQ(second_texture->diff_count(), 3);
  ASSERT_TRUE(fourth_frame.GetFrameDamage().has_value());
  ExpectRegion(
      *fourth_frame.GetFrameDamage(),
      {DlIRect::MakeLTRB(0, 10, 10, 20), DlIRect::MakeLTRB(30, 10, 40, 20),
       DlIRect::MakeLTRB(80, 80, 90, 90)});

  // A missing dirty set preserves upstream's conservative behavior.
  auto fifth = make_tree(104);
  FrameDamage fifth_frame;
  fifth_frame.SetPreviousLayerTree(fourth.get());
  fifth_frame.SetExistingDamage(DlRegion());
  fifth_frame.ComputeDamageRegion(*fifth, true, false);
  EXPECT_EQ(first_texture->diff_count(), 4);
  EXPECT_EQ(second_texture->diff_count(), 4);

  // The conservative diff refreshes the reusable block for a later clean
  // frame instead of permanently disabling the optimization.
  const std::unordered_set<int64_t> sixth_dirty_texture = {105};
  auto sixth = make_tree(105);
  FrameDamage sixth_frame;
  sixth_frame.SetPreviousLayerTree(fifth.get());
  sixth_frame.SetDirtyTextureIds(&sixth_dirty_texture);
  sixth_frame.SetExistingDamage(DlRegion());
  sixth_frame.ComputeDamageRegion(*sixth, true, false);
  EXPECT_EQ(first_texture->diff_count(), 4);
  EXPECT_EQ(second_texture->diff_count(), 4);
}

TEST(FrameDamageTest, RetainedTextureMetadataRejectsChangedAncestorTransform) {
  auto retained = std::make_shared<ContainerLayer>();
  auto retained_texture = std::make_shared<CountingTextureLayer>(
      DlPoint(10, 10), DlSize(20, 20), 7);
  retained->Add(retained_texture);
  const std::unordered_set<int64_t> no_dirty_textures;

  auto first_transform =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({0, 0}));
  first_transform->Add(retained);
  auto first_root = std::make_shared<ContainerLayer>();
  first_root->Add(first_transform);
  LayerTree first(first_root, kFrameSize);
  FrameDamage first_frame;
  first_frame.SetDirtyTextureIds(&no_dirty_textures);
  first_frame.ComputeDamageRegion(first, true, false);

  auto second_transform =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({0, 0}));
  second_transform->AssignOldLayer(first_transform.get());
  second_transform->Add(retained);
  auto second_root = std::make_shared<ContainerLayer>();
  second_root->Add(second_transform);
  LayerTree second(second_root, kFrameSize);
  FrameDamage second_frame;
  second_frame.SetPreviousLayerTree(&first);
  second_frame.SetDirtyTextureIds(&no_dirty_textures);
  second_frame.SetExistingDamage(DlRegion());
  second_frame.ComputeDamageRegion(second, true, false);
  EXPECT_EQ(retained_texture->diff_count(), 2);

  auto third_transform =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({40, 0}));
  third_transform->AssignOldLayer(second_transform.get());
  third_transform->Add(retained);
  auto third_root = std::make_shared<ContainerLayer>();
  third_root->Add(third_transform);
  LayerTree third(third_root, kFrameSize);
  FrameDamage third_frame;
  third_frame.SetPreviousLayerTree(&second);
  third_frame.SetDirtyTextureIds(&no_dirty_textures);
  third_frame.SetExistingDamage(DlRegion());
  third_frame.ComputeDamageRegion(third, true, false);

  EXPECT_EQ(retained_texture->diff_count(), 3);
  ASSERT_TRUE(third_frame.GetFrameDamage().has_value());
  ExpectRegion(
      *third_frame.GetFrameDamage(),
      {DlIRect::MakeLTRB(10, 10, 30, 30), DlIRect::MakeLTRB(50, 10, 70, 30)});
}

TEST(FrameDamageTest, RetainedReadbackTextureSubtreeUsesNormalDiff) {
  auto retained = std::make_shared<ContainerLayer>();
  auto retained_texture = std::make_shared<CountingTextureLayer>(
      DlPoint(10, 10), DlSize(10, 10), 7);
  retained->Add(retained_texture);
  auto clip = std::make_shared<ClipRectLayer>(DlRect::MakeLTRB(60, 60, 80, 80),
                                              Clip::kHardEdge);
  auto backdrop = std::make_shared<CountingBackdropFilterLayer>(
      DlImageFilter::MakeMatrix(DlMatrix::MakeTranslation({50, 50}),
                                DlImageSampling::kLinear),
      DlBlendMode::kSrcOver);
  clip->Add(backdrop);
  retained->Add(clip);

  auto make_tree = [&](int64_t changing_texture_id) {
    auto root = std::make_shared<ContainerLayer>();
    root->Add(retained);
    root->Add(std::make_shared<TextureLayer>(DlPoint(90, 90), DlSize(5, 5),
                                             changing_texture_id, false,
                                             DlImageSampling::kLinear));
    return std::make_unique<LayerTree>(root, kFrameSize);
  };

  auto first = make_tree(100);
  const std::unordered_set<int64_t> first_dirty_texture = {100};
  FrameDamage first_frame;
  first_frame.SetDirtyTextureIds(&first_dirty_texture);
  first_frame.ComputeDamageRegion(*first, true, false);

  auto second = make_tree(101);
  const std::unordered_set<int64_t> second_dirty_texture = {101};
  FrameDamage second_frame;
  second_frame.SetPreviousLayerTree(first.get());
  second_frame.SetDirtyTextureIds(&second_dirty_texture);
  second_frame.SetExistingDamage(DlRegion());
  second_frame.ComputeDamageRegion(*second, true, false);

  auto third = make_tree(102);
  const std::unordered_set<int64_t> third_dirty_texture = {102};
  FrameDamage third_frame;
  third_frame.SetPreviousLayerTree(second.get());
  third_frame.SetDirtyTextureIds(&third_dirty_texture);
  third_frame.SetExistingDamage(DlRegion());
  third_frame.ComputeDamageRegion(*third, true, false);

  EXPECT_EQ(backdrop->diff_count(), 3);
  EXPECT_EQ(retained_texture->diff_count(), 2);
  EXPECT_FALSE(
      second->retained_subtree_diff_metadata().contains(retained->unique_id()));
  EXPECT_FALSE(
      third->retained_subtree_diff_metadata().contains(retained->unique_id()));
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

TEST(FrameDamageTest,
     ReusedTreeInvalidatesBackdropForLowerTextureButNotItsChild) {
  auto root = std::make_shared<ContainerLayer>();
  root->Add(
      std::make_shared<CountingTextureLayer>(DlPoint(), DlSize(100, 100), 7));

  auto clip = std::make_shared<ClipRectLayer>(DlRect::MakeLTRB(20, 20, 80, 80),
                                              Clip::kHardEdge);
  auto backdrop = std::make_shared<BackdropFilterLayer>(
      DlImageFilter::MakeBlur(6, 6, DlTileMode::kClamp), DlBlendMode::kSrc);
  backdrop->Add(std::make_shared<CountingTextureLayer>(DlPoint(20, 20),
                                                       DlSize(60, 60), 8));
  clip->Add(backdrop);
  root->Add(clip);
  LayerTree tree(root, kFrameSize);

  FrameDamage initial_frame;
  initial_frame.ComputeDamageRegion(tree, true, true);
  ASSERT_EQ(tree.backdrop_filter_caches().size(), 1u);
  const auto& cache = tree.backdrop_filter_caches().front();
  EXPECT_EQ(cache.input_texture_ids, std::vector<int64_t>({7}));
  const int64_t initial_token = cache.state->token();

  const std::unordered_set<int64_t> dirty_child = {8};
  FrameDamage child_frame;
  child_frame.SetPreviousLayerTree(&tree);
  child_frame.SetDirtyTextureIds(&dirty_child);
  child_frame.SetExistingDamage(DlRegion());
  child_frame.ComputeDamageRegion(tree, true, true);
  EXPECT_EQ(cache.state->token(), initial_token);

  const std::unordered_set<int64_t> dirty_backdrop = {7};
  FrameDamage backdrop_frame;
  backdrop_frame.SetPreviousLayerTree(&tree);
  backdrop_frame.SetDirtyTextureIds(&dirty_backdrop);
  backdrop_frame.SetExistingDamage(DlRegion());
  backdrop_frame.ComputeDamageRegion(tree, true, true);
  const int64_t invalidated_token = cache.state->token();
  EXPECT_NE(invalidated_token, initial_token);
  EXPECT_EQ(GetBackdropFilterCacheFamily(invalidated_token),
            GetBackdropFilterCacheFamily(initial_token));
}

TEST(FrameDamageTest,
     WindowLocalBackdropIgnoresClippedNeighborsButKeepsOverlap) {
  for (bool glass : {false, true}) {
    for (bool overlap : {false, true}) {
      SCOPED_TRACE(glass);
      SCOPED_TRACE(overlap);
      auto root = std::make_shared<ContainerLayer>();
      const DlRect lower_bounds =
          DlRect::MakeLTRB(0, 0, overlap ? 65 : 45, 100);
      auto lower =
          std::make_shared<ClipRectLayer>(lower_bounds, Clip::kHardEdge);
      // Deliberately larger than the clip, like a decorated client buffer.
      lower->Add(std::make_shared<CountingTextureLayer>(DlPoint(),
                                                        DlSize(75, 100), 7));
      root->Add(lower);
      const DlRect bounds = DlRect::MakeLTRB(55.25f, 2.5f, 95.75f, 97.5f);
      auto clip = std::make_shared<ClipRectLayer>(bounds, Clip::kHardEdge);
      std::shared_ptr<DlImageFilter> filter =
          glass ? DlGlassImageFilter::Make(
                      7, 7, DlRoundRect::MakeRectXY(bounds, 8, 8), 1, 27, 0.52f,
                      0.51f, 1, DlColor::kTransparent(), 0, 1, 0, 1, 1, 0.65f,
                      true)
                : DlBlurImageFilter::Make(6, 6, DlTileMode::kClamp,
                                          std::nullopt, 1, 0.65f, true);
      ASSERT_TRUE(filter);
      ASSERT_TRUE(filter->is_window_backdrop());
      auto backdrop =
          std::make_shared<BackdropFilterLayer>(filter, DlBlendMode::kSrc);
      backdrop->Add(std::make_shared<CountingTextureLayer>(DlPoint(55, 0),
                                                           DlSize(45, 100), 8));
      clip->Add(backdrop);
      root->Add(clip);
      LayerTree tree(root, kFrameSize);
      FrameDamage first;
      first.ComputeDamageRegion(tree, true, true);
      ASSERT_EQ(tree.backdrop_filter_caches().size(), 1u);
      const auto& cache = tree.backdrop_filter_caches().front();
      EXPECT_EQ(cache.input_texture_ids,
                overlap ? std::vector<int64_t>{7} : std::vector<int64_t>{});
      const auto token = cache.state->token();
      const std::unordered_set<int64_t> foreground_dirty = {8};
      FrameDamage next;
      next.SetPreviousLayerTree(&tree);
      next.SetExistingDamage(DlRegion());
      next.SetDirtyTextureIds(&foreground_dirty);
      next.ComputeDamageRegion(tree, true, true);
      EXPECT_EQ(cache.state->token(), token);
      const std::unordered_set<int64_t> background_dirty = {7};
      next.SetDirtyTextureIds(&background_dirty);
      next.ComputeDamageRegion(tree, true, true);
      EXPECT_EQ(cache.state->token() != token, overlap);
    }
  }
}

TEST(FrameDamageTest, ExplicitWindowKeepsVisibleTextureCacheDependencies) {
  for (bool overlap : {false, true}) {
    for (bool fresh_tree : {false, true}) {
      SCOPED_TRACE(overlap);
      SCOPED_TRACE(fresh_tree);
      const auto lower_rect = DlRect::MakeLTRB(0, 0, overlap ? 65 : 45, 100);
      const auto upper_rect = DlRect::MakeLTRB(55, 2, 96, 98);
      auto lower = std::make_shared<WindowSurfaceLayer>(
          DlWindowSurfaceFilter::Style{lower_rect, lower_rect, 8,
                                       DlColor::kRed()},
          nullptr, 7, DlRect::MakeLTRB(-10, -10, 90, 110), DlMatrix(),
          DlImageSampling::kLinear, 1);
      auto upper = std::make_shared<WindowSurfaceLayer>(
          DlWindowSurfaceFilter::Style{upper_rect, upper_rect.Expand(-2), 8,
                                       DlColor::kRed()},
          DlBlurImageFilter::Make(6, 6, DlTileMode::kClamp, std::nullopt, 1,
                                  0.65f),
          8, DlRect::MakeLTRB(40, -10, 110, 110), DlMatrix(),
          DlImageSampling::kLinear, 1);
      auto make_tree = [&] {
        auto root = std::make_shared<ContainerLayer>();
        root->Add(lower);
        root->Add(upper);
        return std::make_unique<LayerTree>(root, kFrameSize);
      };
      auto first = make_tree();
      FrameDamage initial;
      initial.ComputeDamageRegion(*first, false, true);
      ASSERT_EQ(first->backdrop_filter_caches().size(), 1u);
      ASSERT_EQ(first->readback_regions().size(), 1u);
      EXPECT_EQ(first->readback_regions().front().cache_coverage,
                upper_rect.Expand(-2));
      const auto state = first->backdrop_filter_caches().front().state;
      const auto token = state->token();
      EXPECT_EQ(first->backdrop_filter_caches().front().input_texture_ids,
                overlap ? std::vector<int64_t>{7} : std::vector<int64_t>{});
      auto next = fresh_tree ? make_tree() : nullptr;
      LayerTree& tree = next ? *next : *first;
      const std::unordered_set<int64_t> own_dirty = {8};
      FrameDamage own;
      own.SetPreviousLayerTree(first.get());
      own.SetExistingDamage(DlRegion());
      own.SetDirtyTextureIds(&own_dirty);
      own.ComputeDamageRegion(tree, false, true);
      EXPECT_EQ(state->token(), token);
      const std::unordered_set<int64_t> lower_dirty = {7};
      FrameDamage underneath;
      underneath.SetPreviousLayerTree(&tree);
      underneath.SetExistingDamage(DlRegion());
      underneath.SetDirtyTextureIds(&lower_dirty);
      underneath.ComputeDamageRegion(tree, false, true);
      EXPECT_EQ(state->token() != token, overlap);
    }
  }
}

TEST(FrameDamageTest, FreshTreeWindowBackdropIgnoresClippedTextureDamage) {
  // Existing generic-backdrop regression tests below remain independent of
  // the explicit window primitive's own cache dependency tests.
  for (bool glass : {false, true}) {
    for (bool overlap : {false, true}) {
      SCOPED_TRACE(glass);
      SCOPED_TRACE(overlap);
      const DlRect lower_bounds =
          DlRect::MakeLTRB(0, 0, overlap ? 65 : 45, 100);
      auto lower =
          std::make_shared<ClipRectLayer>(lower_bounds, Clip::kHardEdge);
      auto texture = std::make_shared<CountingTextureLayer>(DlPoint(-10, -10),
                                                            DlSize(85, 120), 7);
      lower->Add(texture);
      const DlRect bounds = DlRect::MakeLTRB(55.25f, 2.5f, 95.75f, 97.5f);
      auto clip = std::make_shared<ClipRectLayer>(bounds, Clip::kAntiAlias);
      std::shared_ptr<DlImageFilter> filter =
          glass ? DlGlassImageFilter::Make(
                      7, 7, DlRoundRect::MakeRectXY(bounds, 8, 8), 1, 27, 0.52f,
                      0.51f, 1, DlColor::kTransparent(), 0, 1, 0, 1, 1, 0.65f,
                      true)
                : DlBlurImageFilter::Make(6, 6, DlTileMode::kClamp,
                                          std::nullopt, 1, 0.65f, true);
      auto backdrop = std::make_shared<CountingBackdropFilterLayer>(
          filter, DlBlendMode::kSrc);
      backdrop->Add(std::make_shared<CountingTextureLayer>(DlPoint(45, -10),
                                                           DlSize(65, 120), 8));
      clip->Add(backdrop);
      auto make_tree = [&] {
        auto root = std::make_shared<ContainerLayer>();
        root->Add(lower);
        root->Add(clip);
        return std::make_unique<LayerTree>(root, kFrameSize);
      };
      auto first = make_tree();
      FrameDamage initial;
      initial.ComputeDamageRegion(*first, false, true);
      const auto state = first->backdrop_filter_caches().front().state;
      const auto token = state->token();

      // A fresh tree forces normal Diff even though the child layers are
      // retained. The oversized old AND current texture regions must be
      // clipped.
      auto second = make_tree();
      const std::unordered_set<int64_t> dirty = {7, 8};
      FrameDamage next;
      next.SetPreviousLayerTree(first.get());
      next.SetDirtyTextureIds(&dirty);
      next.SetExistingDamage(DlRegion());
      next.ComputeDamageRegion(*second, false, true);
      EXPECT_EQ(texture->diff_count(), 2);
      EXPECT_EQ(backdrop->diff_count(), 2);
      EXPECT_EQ(state->token() != token, overlap);
      if (!overlap) {
        ExpectRegion(*next.GetFrameDamage(), {DlIRect::RoundOut(lower_bounds),
                                              DlIRect::RoundOut(bounds)});
      }

      // A clean lower texture carries its clipped paint region forward;
      // changes to the backdrop's own child must not invalidate its input.
      auto third = make_tree();
      const auto second_token = state->token();
      const std::unordered_set<int64_t> foreground_dirty = {8};
      FrameDamage foreground;
      foreground.SetPreviousLayerTree(second.get());
      foreground.SetDirtyTextureIds(&foreground_dirty);
      foreground.SetExistingDamage(DlRegion());
      foreground.ComputeDamageRegion(*third, false, true);
      EXPECT_EQ(state->token(), second_token);
      EXPECT_EQ(backdrop->diff_count(), 3);
    }
  }
}

TEST(FrameDamageTest, ClippedTextureDamagePreservesOldClipOnChangeAndRemoval) {
  // Old-clip damage remains a separate invariant from window materials.
  const DlRect old_bounds = DlRect::MakeLTRB(10.25f, 10.5f, 40.75f, 40.5f);
  for (const DlRect new_bounds : {
           DlRect::MakeLTRB(20.25f, 20.5f, 30.75f, 30.5f),  // Shrink.
           DlRect::MakeLTRB(1.25f, 1.5f, 50.75f, 50.5f),    // Grow.
           DlRect::MakeLTRB(60.25f, 60.5f, 90.75f, 90.5f),  // Disjoint move.
           DlRect::MakeLTRB(110, 110, 140, 140),            // Fully offscreen.
       }) {
    SCOPED_TRACE(new_bounds);
    auto texture = std::make_shared<CountingTextureLayer>(DlPoint(-20, -20),
                                                          DlSize(180, 180), 7);
    std::shared_ptr<ClipRectLayer> previous_clip;
    auto make_tree = [&](const DlRect& bounds) {
      auto root = std::make_shared<ContainerLayer>();
      auto clip = std::make_shared<ClipRectLayer>(bounds, Clip::kAntiAlias);
      if (previous_clip) {
        clip->AssignOldLayer(previous_clip.get());
      }
      previous_clip = clip;
      clip->Add(texture);
      root->Add(clip);
      return std::make_unique<LayerTree>(root, kFrameSize);
    };
    const std::unordered_set<int64_t> no_dirty_textures;
    auto first = make_tree(old_bounds);
    FrameDamage initial;
    initial.ComputeDamageRegion(*first, false, true);
    // The first frame intentionally damages the full output, but the stored
    // texture region must already be clipped for later updates/removal.
    ExpectRegion(*initial.GetFrameDamage(), {kFullFrame});
    EXPECT_EQ(
        first->paint_region_map().at(texture->unique_id()).ComputeBounds(),
        old_bounds);

    // Also exercise the clean retained-texture metadata path before changing
    // the clip, so old damage must survive being carried between trees.
    auto retained = make_tree(old_bounds);
    FrameDamage clean;
    clean.SetPreviousLayerTree(first.get());
    clean.SetDirtyTextureIds(&no_dirty_textures);
    clean.SetExistingDamage(DlRegion());
    clean.ComputeDamageRegion(*retained, false, true);
    ExpectRegion(*clean.GetFrameDamage(), {});

    auto changed = make_tree(new_bounds);
    FrameDamage change;
    change.SetPreviousLayerTree(retained.get());
    change.SetDirtyTextureIds(&no_dirty_textures);
    change.SetExistingDamage(DlRegion());
    change.ComputeDamageRegion(*changed, false, true);
    const DlIRect visible_new =
        DlIRect::RoundOut(new_bounds).IntersectionOrEmpty(kFullFrame);
    ExpectRegion(*change.GetFrameDamage(),
                 {DlIRect::RoundOut(old_bounds), visible_new});

    LayerTree empty(std::make_shared<ContainerLayer>(), kFrameSize);
    FrameDamage removal;
    removal.SetPreviousLayerTree(changed.get());
    removal.SetDirtyTextureIds(&no_dirty_textures);
    removal.SetExistingDamage(DlRegion());
    removal.ComputeDamageRegion(empty, false, true);
    ExpectRegion(*removal.GetFrameDamage(), {visible_new});
  }
}

TEST(FrameDamageTest, ClippedTextureDamagePreservesOldTransform) {
  auto clip = std::make_shared<ClipRectLayer>(
      DlRect::MakeLTRB(10.25f, 10.5f, 20.75f, 20.5f), Clip::kAntiAlias);
  clip->Add(std::make_shared<CountingTextureLayer>(DlPoint(-20, -20),
                                                   DlSize(80, 80), 7));
  std::shared_ptr<TransformLayer> previous_transform;
  auto make_tree = [&](float x) {
    auto root = std::make_shared<ContainerLayer>();
    auto transform = std::make_shared<TransformLayer>(
        DlMatrix::MakeTranslation({x, 0}) * DlMatrix::MakeScale({2, 2, 1}));
    if (previous_transform) {
      transform->AssignOldLayer(previous_transform.get());
    }
    previous_transform = transform;
    transform->Add(clip);
    root->Add(transform);
    return std::make_unique<LayerTree>(root, kFrameSize);
  };
  auto first = make_tree(0);
  FrameDamage initial;
  initial.ComputeDamageRegion(*first, false, true);
  auto moved = make_tree(40);
  const std::unordered_set<int64_t> no_dirty_textures;
  FrameDamage movement;
  movement.SetPreviousLayerTree(first.get());
  movement.SetDirtyTextureIds(&no_dirty_textures);
  movement.SetExistingDamage(DlRegion());
  movement.ComputeDamageRegion(*moved, false, true);
  ExpectRegion(*movement.GetFrameDamage(), {DlIRect::MakeLTRB(20, 21, 42, 41),
                                            DlIRect::MakeLTRB(60, 21, 82, 41)});
}

TEST(FrameDamageTest, ClippedTextureKeepsConservativeAncestorFilterDamage) {
  auto clip = std::make_shared<ClipRectLayer>(DlRect::MakeLTRB(20, 20, 40, 40),
                                              Clip::kHardEdge);
  clip->Add(std::make_shared<CountingTextureLayer>(DlPoint(10, 10),
                                                   DlSize(40, 40), 7));
  auto blur = DlImageFilter::MakeBlur(6, 6, DlTileMode::kDecal);
  auto filter = std::make_shared<ImageFilterLayer>(blur, DlPoint());
  filter->Add(clip);
  auto make_tree = [&] {
    auto root = std::make_shared<ContainerLayer>();
    root->Add(filter);
    return std::make_unique<LayerTree>(root, kFrameSize);
  };
  auto first = make_tree();
  FrameDamage initial;
  initial.ComputeDamageRegion(*first, false, true);
  auto second = make_tree();
  const std::unordered_set<int64_t> dirty = {7};
  FrameDamage next;
  next.SetPreviousLayerTree(first.get());
  next.SetDirtyTextureIds(&dirty);
  next.SetExistingDamage(DlRegion());
  next.ComputeDamageRegion(*second, false, true);
  DlIRect expanded;
  blur->map_device_bounds(DlIRect::MakeLTRB(10, 10, 50, 50), DlMatrix(),
                          expanded);
  ExpectRegion(*next.GetFrameDamage(),
               {expanded.IntersectionOrEmpty(kFullFrame)});
}

TEST(FrameDamageTest, OrdinaryBackdropKeepsItsOutsideSamplingDependency) {
  auto root = std::make_shared<ContainerLayer>();
  root->Add(
      std::make_shared<CountingTextureLayer>(DlPoint(), DlSize(45, 100), 7));
  auto clip = std::make_shared<ClipRectLayer>(DlRect::MakeLTRB(55, 0, 100, 100),
                                              Clip::kHardEdge);
  auto filter = DlImageFilter::MakeBlur(6, 6, DlTileMode::kClamp);
  EXPECT_FALSE(filter->is_window_backdrop());
  clip->Add(std::make_shared<BackdropFilterLayer>(filter, DlBlendMode::kSrc));
  root->Add(clip);
  LayerTree tree(root, kFrameSize);
  FrameDamage first;
  first.ComputeDamageRegion(tree, true, true);
  ASSERT_EQ(tree.backdrop_filter_caches().size(), 1u);
  EXPECT_EQ(tree.backdrop_filter_caches().front().input_texture_ids,
            std::vector<int64_t>{7});
}

TEST(FrameDamageTest, MovingForegroundAboveCachedBackdropKeepsDamageSmall) {
  auto background =
      std::make_shared<CountingTextureLayer>(DlPoint(), DlSize(100, 100), 7);
  const DlRect filter_bounds = DlRect::MakeLTRB(2.25f, 2.5f, 97.75f, 97.5f);
  auto clip = std::make_shared<ClipRectLayer>(filter_bounds, Clip::kHardEdge);
  clip->Add(std::make_shared<BackdropFilterLayer>(
      DlImageFilter::MakeBlur(6, 6, DlTileMode::kClamp), DlBlendMode::kSrc));
  auto foreground =
      std::make_shared<CountingTextureLayer>(DlPoint(), DlSize(4, 4), 8);
  std::shared_ptr<TransformLayer> previous_transform;
  auto make_tree = [&](float x) {
    auto root = std::make_shared<ContainerLayer>();
    root->Add(background);
    root->Add(clip);
    auto transform =
        std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({x, 32}));
    if (previous_transform) {
      transform->AssignOldLayer(previous_transform.get());
    }
    previous_transform = transform;
    transform->Add(foreground);
    root->Add(transform);
    return std::make_unique<LayerTree>(root, kFrameSize);
  };
  auto first = make_tree(32);
  FrameDamage initial;
  initial.ComputeDamageRegion(*first, false, true);
  const int64_t token = first->backdrop_filter_caches().front().state->token();

  auto second = make_tree(36);
  const std::unordered_set<int64_t> no_dirty_textures;
  FrameDamage moved;
  moved.SetPreviousLayerTree(first.get());
  moved.SetDirtyTextureIds(&no_dirty_textures);
  moved.SetExistingDamage(DlRegion());
  moved.ComputeDamageRegion(*second, false, true);
  EXPECT_EQ(second->backdrop_filter_caches().front().state->token(), token);
  ExpectRegion(*moved.GetFrameDamage(), {kFullFrame});

  // A real renderer must pin an existing snapshot with this exact version.
  // A merely unchanged filter is insufficient: its snapshot might be absent.
  BackdropSnapshotPin pin = [&](int64_t key, const DlRect& coverage) {
    // A snapshot need only contain the real filter extent, including when
    // readback metadata is reused on an autonomous frame.
    EXPECT_EQ(coverage, filter_bounds);
    return key == token;
  };
  moved.ComputeDamageRegion(*second, false, true, pin);
  ExpectRegion(*moved.GetFrameDamage(), {DlIRect::MakeLTRB(32, 32, 40, 36)});
  ExpectRegion(*moved.GetBufferDamage(), {DlIRect::MakeLTRB(32, 32, 40, 36)});

  // An older swapchain buffer also repairs the cursor's historical position,
  // without expanding that repair into the entire backdrop.
  moved.SetExistingDamage(DlRegion(DlIRect::MakeLTRB(10, 10, 14, 14)));
  moved.ComputeDamageRegion(*second, false, true, pin);
  ExpectRegion(*moved.GetFrameDamage(), {DlIRect::MakeLTRB(32, 32, 40, 36)});
  ExpectRegion(*moved.GetBufferDamage(), {DlIRect::MakeLTRB(10, 10, 14, 14),
                                          DlIRect::MakeLTRB(32, 32, 40, 36)});

  // The reused-tree path must read the current state, not a captured token.
  const std::unordered_set<int64_t> dirty_background = {7};
  FrameDamage changed_input;
  changed_input.SetPreviousLayerTree(second.get());
  changed_input.SetDirtyTextureIds(&dirty_background);
  changed_input.SetExistingDamage(DlRegion());
  changed_input.ComputeDamageRegion(*second, false, true, pin);
  EXPECT_NE(second->backdrop_filter_caches().front().state->token(), token);
  ExpectRegion(*changed_input.GetFrameDamage(), {kFullFrame});
}

TEST(FrameDamageTest, GroupedBackdropFiltersShareInvalidationState) {
  auto root = std::make_shared<ContainerLayer>();
  root->Add(
      std::make_shared<CountingTextureLayer>(DlPoint(), DlSize(100, 100), 1));

  auto first_clip = std::make_shared<ClipRectLayer>(
      DlRect::MakeLTRB(0, 0, 40, 40), Clip::kHardEdge);
  first_clip->Add(std::make_shared<BackdropFilterLayer>(
      DlImageFilter::MakeBlur(6, 6, DlTileMode::kClamp), DlBlendMode::kSrc,
      42));
  root->Add(first_clip);

  root->Add(std::make_shared<CountingTextureLayer>(DlPoint(60, 60),
                                                   DlSize(20, 20), 2));
  auto second_clip = std::make_shared<ClipRectLayer>(
      DlRect::MakeLTRB(60, 60, 100, 100), Clip::kHardEdge);
  second_clip->Add(std::make_shared<BackdropFilterLayer>(
      DlImageFilter::MakeBlur(6, 6, DlTileMode::kClamp), DlBlendMode::kSrc,
      42));
  root->Add(second_clip);
  LayerTree tree(root, kFrameSize);

  FrameDamage initial_frame;
  initial_frame.ComputeDamageRegion(tree, true, true);
  ASSERT_EQ(tree.backdrop_filter_caches().size(), 2u);
  EXPECT_EQ(tree.backdrop_filter_caches()[0].state,
            tree.backdrop_filter_caches()[1].state);

  const int64_t initial_token = tree.backdrop_filter_caches()[0].state->token();
  const std::unordered_set<int64_t> dirty_between_group_members = {2};
  FrameDamage autonomous_frame;
  autonomous_frame.SetPreviousLayerTree(&tree);
  autonomous_frame.SetDirtyTextureIds(&dirty_between_group_members);
  autonomous_frame.SetExistingDamage(DlRegion());
  autonomous_frame.ComputeDamageRegion(tree, true, true);
  EXPECT_NE(tree.backdrop_filter_caches()[0].state->token(), initial_token);
}

}  // namespace
}  // namespace testing
}  // namespace flutter
