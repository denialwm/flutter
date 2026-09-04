// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/testing/diff_context_test.h"

#include <initializer_list>
#include <optional>
#include <vector>

namespace flutter {
namespace testing {
namespace {

void ExpectRegion(const DlRegion& region,
                  std::initializer_list<DlIRect> expected) {
  EXPECT_EQ(region.getRects(), std::vector<DlIRect>(expected));
}

Damage DiffLayerTreeWithExistingDamage(
    MockLayerTree& layer_tree,
    const MockLayerTree& old_layer_tree,
    const std::optional<DlRegion>& existing_damage,
    int horizontal_clip_alignment = 0,
    int vertical_clip_alignment = 0) {
  DiffContext dc(layer_tree.size(), layer_tree.paint_region_map(),
                 old_layer_tree.paint_region_map(), true, false);
  dc.PushCullRect(DlRect::MakeSize(layer_tree.size()));
  layer_tree.root()->Diff(&dc, old_layer_tree.root());
  return dc.ComputeDamage(existing_damage, horizontal_clip_alignment,
                          vertical_clip_alignment);
}

}  // namespace

TEST_F(DiffContextTest, ClipAlignment) {
  MockLayerTree t1;
  t1.root()->Add(CreateDisplayListLayer(
      CreateDisplayList(DlRect::MakeLTRB(30, 30, 50, 50))));
  auto damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 0, 0);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(30, 30, 50, 50));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(30, 30, 50, 50));

  damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 1, 1);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(30, 30, 50, 50));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(30, 30, 50, 50));

  damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 8, 1);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(24, 30, 56, 50));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(24, 30, 56, 50));

  damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 1, 8);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(30, 24, 50, 56));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(30, 24, 50, 56));

  damage = DiffLayerTree(t1, MockLayerTree(), DlIRect(), 16, 16);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(16, 16, 64, 64));
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect::MakeLTRB(16, 16, 64, 64));
}

TEST_F(DiffContextTest, DisjointDamage) {
  DlISize frame_size = DlISize(90, 90);
  auto in_bounds_dl = CreateDisplayList(DlRect::MakeLTRB(30, 30, 50, 50));
  auto out_bounds_dl = CreateDisplayList(DlRect::MakeLTRB(100, 100, 120, 120));

  // We need both DisplayLists to be non-empty.
  ASSERT_FALSE(in_bounds_dl->GetBounds().IsEmpty());
  ASSERT_FALSE(out_bounds_dl->GetBounds().IsEmpty());

  // We need the in_bounds DisplayList to be inside the frame size.
  // We need the out_bounds DisplayList to be completely outside the frame.
  ASSERT_TRUE(DlRect::MakeSize(frame_size).Contains(in_bounds_dl->GetBounds()));
  ASSERT_FALSE(DlRect::MakeSize(frame_size)
                   .IntersectsWithRect(out_bounds_dl->GetBounds()));

  MockLayerTree t1(frame_size);
  t1.root()->Add(CreateDisplayListLayer(in_bounds_dl));

  MockLayerTree t2(frame_size);
  // Include previous
  t2.root()->Add(CreateDisplayListLayer(in_bounds_dl));
  // Add a new layer that is out of frame bounds
  t2.root()->Add(CreateDisplayListLayer(out_bounds_dl));

  // Cannot use DiffLayerTree because it implicitly adds a clip layer
  // around the tree, but we want the out of bounds dl to not be pruned
  // to test the intersection code inside layer::Diff/ComputeDamage
  // damage = DiffLayerTree(t2, t1, DlIRect(), 0, 0);

  DiffContext dc(frame_size, t2.paint_region_map(), t1.paint_region_map(), true,
                 false);
  t2.root()->Diff(&dc, t1.root());
  auto damage = dc.ComputeDamage(DlRegion(), 0, 0);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect());
  EXPECT_EQ(damage.buffer_damage.bounds(), DlIRect());
}

TEST_F(DiffContextTest, PreservesDisjointFrameDamageRectangles) {
  const DlIRect first = DlIRect::MakeLTRB(10, 10, 20, 20);
  const DlIRect second = DlIRect::MakeLTRB(70, 70, 80, 80);

  MockLayerTree current;
  current.root()->Add(
      CreateDisplayListLayer(CreateDisplayList(DlRect::Make(first))));
  current.root()->Add(
      CreateDisplayListLayer(CreateDisplayList(DlRect::Make(second))));

  auto damage = DiffLayerTree(current, MockLayerTree());
  ExpectRegion(damage.frame_damage, {first, second});
  ExpectRegion(damage.buffer_damage, {first, second});
}

TEST_F(DiffContextTest, BufferDamageUnionsFrameAndExistingRegions) {
  const DlIRect changed = DlIRect::MakeLTRB(10, 10, 20, 20);
  const DlIRect old_first = DlIRect::MakeLTRB(40, 40, 50, 50);
  const DlIRect old_second = DlIRect::MakeLTRB(70, 70, 80, 80);

  MockLayerTree current;
  current.root()->Add(
      CreateDisplayListLayer(CreateDisplayList(DlRect::Make(changed))));

  const DlRegion existing_damage(std::vector<DlIRect>{old_first, old_second});
  auto damage = DiffLayerTreeWithExistingDamage(current, MockLayerTree(),
                                                existing_damage);
  ExpectRegion(damage.frame_damage, {changed});
  ExpectRegion(damage.buffer_damage, {changed, old_first, old_second});
}

TEST_F(DiffContextTest, EmptyAndUnknownExistingDamageAreDistinct) {
  MockLayerTree current;
  MockLayerTree previous;
  DiffContext dc(current.size(), current.paint_region_map(),
                 previous.paint_region_map(), true, false);
  dc.PushCullRect(DlRect::MakeSize(current.size()));
  current.root()->Diff(&dc, previous.root());

  const auto known_empty = dc.ComputeDamage(DlRegion());
  EXPECT_TRUE(known_empty.frame_damage.isEmpty());
  EXPECT_TRUE(known_empty.buffer_damage.isEmpty());

  const auto unknown = dc.ComputeDamage(std::nullopt);
  EXPECT_TRUE(unknown.frame_damage.isEmpty());
  ExpectRegion(unknown.buffer_damage, {DlIRect::MakeSize(current.size())});
}

TEST_F(DiffContextTest, AlignsEachDamageRectangleWithoutFillingGaps) {
  const DlIRect first = DlIRect::MakeLTRB(9, 9, 13, 13);
  const DlIRect second = DlIRect::MakeLTRB(41, 41, 45, 45);

  MockLayerTree current;
  current.root()->Add(
      CreateDisplayListLayer(CreateDisplayList(DlRect::Make(first))));
  current.root()->Add(
      CreateDisplayListLayer(CreateDisplayList(DlRect::Make(second))));

  auto damage = DiffLayerTreeWithExistingDamage(current, MockLayerTree(),
                                                DlRegion(), 8, 0);
  ExpectRegion(damage.frame_damage, {DlIRect::MakeLTRB(8, 9, 16, 13),
                                     DlIRect::MakeLTRB(40, 41, 48, 45)});
  ExpectRegion(damage.buffer_damage, {DlIRect::MakeLTRB(8, 9, 16, 13),
                                      DlIRect::MakeLTRB(40, 41, 48, 45)});
}

TEST_F(DiffContextTest, ExpandsChainedReadbackDependenciesToFixedPoint) {
  const DlIRect first_paint = DlIRect::MakeLTRB(10, 10, 20, 20);
  const DlIRect shared = DlIRect::MakeLTRB(30, 30, 40, 40);
  const DlIRect changed = DlIRect::MakeLTRB(80, 80, 90, 90);

  PaintRegionMap current_regions;
  PaintRegionMap previous_regions;
  DiffContext dc(DlISize(100, 100), current_regions, previous_regions, true,
                 false);
  dc.MarkSubtreeDirty(DlRect::Make(changed));
  // Add this dependency first so reaching it requires a second pass.
  dc.AddReadbackRegion(first_paint, shared);
  dc.AddReadbackRegion(shared, changed);

  const auto damage = dc.ComputeDamage(DlRegion());
  ExpectRegion(damage.frame_damage, {first_paint, shared, changed});
  ExpectRegion(damage.buffer_damage, {first_paint, shared, changed});
}

TEST_F(DiffContextTest, ReadbackExpandsHistoricalBufferDamageOnly) {
  const DlIRect first_paint = DlIRect::MakeLTRB(10, 10, 20, 20);
  const DlIRect shared = DlIRect::MakeLTRB(30, 30, 40, 40);
  const DlIRect historical = DlIRect::MakeLTRB(80, 80, 90, 90);

  PaintRegionMap current_regions;
  PaintRegionMap previous_regions;
  DiffContext dc(DlISize(100, 100), current_regions, previous_regions, true,
                 false);
  dc.AddReadbackRegion(first_paint, shared);
  dc.AddReadbackRegion(shared, historical);

  const auto damage = dc.ComputeDamage(DlRegion(historical));
  EXPECT_TRUE(damage.frame_damage.isEmpty());
  ExpectRegion(damage.buffer_damage, {first_paint, shared, historical});
}

TEST_F(DiffContextTest, SnapshotPinsRequireUnambiguousUnfilteredCoverage) {
  PaintRegionMap current_regions, previous_regions;
  const auto state = std::make_shared<BackdropFilterCacheState>();
  const DlIRect paint = DlIRect::MakeLTRB(20, 20, 80, 80);
  const DlIRect input = DlIRect::MakeLTRB(2, 2, 98, 98);
  const DlRegion repair(DlIRect::MakeLTRB(30, 30, 34, 34));
  int calls = 0;
  const BackdropSnapshotPin pin = [&](int64_t, const DlRect&) {
    calls++;
    return true;
  };
  DiffContext duplicate(DlISize(100, 100), current_regions, previous_regions,
                        false, true);
  duplicate.AddReadbackRegion(paint, input, state);
  duplicate.AddReadbackRegion(paint, input, state);
  ExpectRegion(duplicate.ComputeDamage(repair, 0, 0, pin).buffer_damage,
               {input});
  EXPECT_EQ(calls, 0);

  DiffContext filtered(DlISize(100, 100), current_regions, previous_regions,
                       false, true);
  filtered.PushFilterBoundsAdjustment([](DlRect rect) { return rect; });
  filtered.AddReadbackRegion(paint, input, state);
  ExpectRegion(filtered.ComputeDamage(repair, 0, 0, pin).buffer_damage,
               {input});
  EXPECT_EQ(calls, 0);

  DiffContext unknown(DlISize(100, 100), current_regions, previous_regions,
                      false, true);
  unknown.AddReadbackRegion(paint, input, state);
  ExpectRegion(unknown.ComputeDamage(std::nullopt, 0, 0, pin).buffer_damage,
               {DlIRect::MakeWH(100, 100)});
  EXPECT_EQ(calls, 1);
}

}  // namespace testing
}  // namespace flutter
