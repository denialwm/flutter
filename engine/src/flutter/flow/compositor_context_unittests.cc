// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/compositor_context.h"

#include <initializer_list>
#include <optional>
#include <vector>

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

}  // namespace
}  // namespace testing
}  // namespace flutter
