// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/backdrop_graph.h"

#include "flutter/testing/testing.h"

namespace impeller {
namespace testing {

TEST(BackdropGraphTest, EmptyGraphHasNoEpochs) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({});
  EXPECT_TRUE(plan.scopes.empty());
  EXPECT_TRUE(plan.epoch_for_scope.empty());
  EXPECT_TRUE(plan.scopes_by_epoch.empty());
  EXPECT_EQ(plan.dependency_edges, 0u);
  EXPECT_EQ(plan.scene_barriers, 0u);
  EXPECT_EQ(plan.GetMaxEpochWidth(), 0u);
}

TEST(BackdropGraphTest, NonIntersectingScopesShareOneEpoch) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = Rect::MakeLTRB(0, 0, 10, 10),
       .read_region = Rect::MakeLTRB(-2, -2, 12, 12)},
      {.write_region = Rect::MakeLTRB(30, 0, 40, 10),
       .read_region = Rect::MakeLTRB(28, -2, 42, 12)},
      {.write_region = Rect::MakeLTRB(60, 0, 70, 10),
       .read_region = Rect::MakeLTRB(58, -2, 72, 12)},
  });

  EXPECT_EQ(plan.epoch_for_scope, (std::vector<uint32_t>{0u, 0u, 0u}));
  ASSERT_EQ(plan.scopes_by_epoch.size(), 1u);
  EXPECT_EQ(plan.scopes_by_epoch[0], (std::vector<uint32_t>{0u, 1u, 2u}));
  EXPECT_EQ(plan.dependency_edges, 0u);
  EXPECT_EQ(plan.GetMaxEpochWidth(), 3u);
}

TEST(BackdropGraphTest, OverlappingWritesPreserveSceneOrder) {
  const Rect region = Rect::MakeLTRB(0, 0, 20, 20);
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = region, .read_region = region},
      {.write_region = region, .read_region = region},
      {.write_region = region, .read_region = region},
  });

  EXPECT_EQ(plan.epoch_for_scope, (std::vector<uint32_t>{0u, 1u, 2u}));
  EXPECT_EQ(plan.dependency_edges, 3u);
  EXPECT_EQ(plan.GetMaxEpochWidth(), 1u);
}

TEST(BackdropGraphTest, BlurHaloCreatesDependencyForDisjointOutputs) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = Rect::MakeLTRB(0, 0, 10, 10),
       .read_region = Rect::MakeLTRB(-2, -2, 12, 12)},
      {.write_region = Rect::MakeLTRB(20, 0, 30, 10),
       .read_region = Rect::MakeLTRB(5, -2, 32, 12)},
  });

  EXPECT_EQ(plan.epoch_for_scope, (std::vector<uint32_t>{0u, 1u}));
  EXPECT_EQ(plan.dependency_edges, 1u);
}

TEST(BackdropGraphTest, EpochsRetainTransitiveDependencies) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = Rect::MakeLTRB(0, 0, 10, 10),
       .read_region = Rect::MakeLTRB(0, 0, 12, 10)},
      {.write_region = Rect::MakeLTRB(10, 0, 20, 10),
       .read_region = Rect::MakeLTRB(8, 0, 22, 10)},
      {.write_region = Rect::MakeLTRB(20, 0, 30, 10),
       .read_region = Rect::MakeLTRB(18, 0, 30, 10)},
  });

  EXPECT_EQ(plan.epoch_for_scope, (std::vector<uint32_t>{0u, 1u, 2u}));
  EXPECT_EQ(plan.GetMaxEpochWidth(), 1u);
}

TEST(BackdropGraphTest, LaterWriteCannotShareEarlierReadFootprint) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = Rect::MakeLTRB(0, 0, 10, 10),
       .read_region = Rect::MakeLTRB(0, 0, 30, 10)},
      {.write_region = Rect::MakeLTRB(20, 0, 30, 10),
       .read_region = Rect::MakeLTRB(20, 0, 30, 10)},
  });

  EXPECT_EQ(plan.epoch_for_scope, (std::vector<uint32_t>{0u, 1u}));
  EXPECT_EQ(plan.dependency_edges, 1u);
}

TEST(BackdropGraphTest, UnknownInterveningWriteCreatesHardEpochBoundary) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = Rect::MakeLTRB(0, 0, 10, 10),
       .read_region = Rect::MakeLTRB(0, 0, 10, 10),
       .scene_color_generation = 4u},
      {.write_region = Rect::MakeLTRB(30, 0, 40, 10),
       .read_region = Rect::MakeLTRB(30, 0, 40, 10),
       .scene_color_generation = 5u},
      {.write_region = Rect::MakeLTRB(60, 0, 70, 10),
       .read_region = Rect::MakeLTRB(60, 0, 70, 10),
       .scene_color_generation = 5u},
  });

  EXPECT_EQ(plan.epoch_for_scope, (std::vector<uint32_t>{0u, 1u, 1u}));
  EXPECT_EQ(plan.scene_barriers, 1u);
  EXPECT_EQ(plan.dependency_edges, 0u);
  ASSERT_EQ(plan.scopes.size(), 3u);
  EXPECT_EQ(plan.scopes[1].scene_color_generation, 5u);
}

TEST(BackdropGraphTest, CursorMatchesExactSecondPassScope) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = Rect::MakeLTRB(0, 0, 20, 20),
       .read_region = Rect::MakeLTRB(-4, -4, 24, 24)},
  });
  BackdropEpochCursor cursor;

  EXPECT_EQ(cursor.Claim(plan, Rect::MakeLTRB(0, 0, 20, 20),
                         Rect::MakeLTRB(-4, -4, 24, 24)),
            0u);
}

TEST(BackdropGraphTest, CursorCanSkipScopeCulledFromSecondPass) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = Rect::MakeLTRB(0, 0, 10, 10),
       .read_region = Rect::MakeLTRB(0, 0, 10, 10)},
      {.write_region = Rect::MakeLTRB(30, 0, 40, 10),
       .read_region = Rect::MakeLTRB(30, 0, 40, 10)},
  });
  BackdropEpochCursor cursor;

  EXPECT_EQ(cursor.Claim(plan, Rect::MakeLTRB(30, 0, 40, 10),
                         Rect::MakeLTRB(30, 0, 40, 10)),
            0u);
}

TEST(BackdropGraphTest, CursorFailsClosedForDifferentSecondPassScope) {
  const BackdropEpochPlan plan = PlanBackdropEpochs({
      {.write_region = Rect::MakeLTRB(0, 0, 10, 10),
       .read_region = Rect::MakeLTRB(0, 0, 10, 10)},
  });
  BackdropEpochCursor cursor;

  EXPECT_EQ(cursor.Claim(plan, Rect::MakeLTRB(0, 0, 11, 10),
                         Rect::MakeLTRB(0, 0, 11, 10)),
            std::nullopt);
  cursor.Reset();
  EXPECT_EQ(cursor.Claim(plan, Rect::MakeLTRB(0, 0, 10, 10),
                         Rect::MakeLTRB(0, 0, 10, 10)),
            0u);
}

}  // namespace testing
}  // namespace impeller
