// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/transform_layer.h"

#include "flutter/flow/testing/diff_context_test.h"
#include "flutter/flow/testing/layer_test.h"
#include "flutter/flow/testing/mock_layer.h"
#include "flutter/fml/macros.h"

namespace flutter {
namespace testing {

using TransformLayerTest = LayerTest;

#ifndef NDEBUG
TEST_F(TransformLayerTest, PaintingEmptyLayerDies) {
  auto layer = std::make_shared<TransformLayer>(DlMatrix());  // identity

  layer->Preroll(preroll_context());
  EXPECT_EQ(layer->paint_bounds(), DlRect());
  EXPECT_EQ(layer->child_paint_bounds(), DlRect());
  EXPECT_FALSE(layer->needs_painting(paint_context()));

  EXPECT_DEATH_IF_SUPPORTED(layer->Paint(paint_context()),
                            "needs_painting\\(context\\)");
}

TEST_F(TransformLayerTest, PaintBeforePrerollDies) {
  const DlPath child_path = DlPath::MakeRectLTRB(5.0f, 6.0f, 20.5f, 21.5f);
  auto mock_layer = std::make_shared<MockLayer>(child_path, DlPaint());
  auto layer = std::make_shared<TransformLayer>(DlMatrix());  // identity
  layer->Add(mock_layer);

  EXPECT_DEATH_IF_SUPPORTED(layer->Paint(paint_context()),
                            "needs_painting\\(context\\)");
}
#endif

TEST_F(TransformLayerTest, Identity) {
  const DlPath child_path = DlPath::MakeRectLTRB(5.0f, 6.0f, 20.5f, 21.5f);
  DlRect cull_rect = DlRect::MakeXYWH(2.0f, 2.0f, 14.0f, 14.0f);
  auto mock_layer = std::make_shared<MockLayer>(child_path, DlPaint());
  auto layer = std::make_shared<TransformLayer>(DlMatrix());  // identity
  layer->Add(mock_layer);

  preroll_context()->state_stack.set_preroll_delegate(cull_rect);
  layer->Preroll(preroll_context());
  EXPECT_EQ(mock_layer->paint_bounds(), child_path.GetBounds());
  EXPECT_EQ(layer->paint_bounds(), mock_layer->paint_bounds());
  EXPECT_EQ(layer->child_paint_bounds(), mock_layer->paint_bounds());
  EXPECT_TRUE(mock_layer->needs_painting(paint_context()));
  EXPECT_TRUE(layer->needs_painting(paint_context()));
  EXPECT_EQ(mock_layer->parent_matrix(), DlMatrix());  // identity
  EXPECT_EQ(mock_layer->parent_cull_rect(), cull_rect);
  EXPECT_EQ(mock_layer->parent_mutators().stack_count(), 0u);
  EXPECT_EQ(mock_layer->parent_mutators(), MutatorsStack());

  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected_builder;
  /* (Transform)layer::Paint */ {
    expected_builder.Save();
    {
      expected_builder.Transform(DlMatrix());
      /* mock_layer::Paint */ {
        expected_builder.DrawPath(child_path, DlPaint());
      }
    }
    expected_builder.Restore();
  }
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected_builder.Build()));
}

TEST_F(TransformLayerTest, Simple) {
  const DlPath child_path = DlPath::MakeRectLTRB(5.0f, 6.0f, 20.5f, 21.5f);
  DlMatrix initial_transform = DlMatrix::MakeTranslation({-0.5f, -0.5f});
  DlRect local_cull_rect = DlRect::MakeXYWH(2.0f, 2.0f, 14.0f, 14.0f);
  DlRect device_cull_rect =
      local_cull_rect.TransformAndClipBounds(initial_transform);
  DlMatrix layer_transform = DlMatrix::MakeTranslation({2.5f, 2.5f});
  EXPECT_TRUE(layer_transform.IsInvertible());
  DlMatrix inverse_layer_transform = layer_transform.Invert();

  auto mock_layer = std::make_shared<MockLayer>(child_path, DlPaint());
  auto layer = std::make_shared<TransformLayer>(layer_transform);
  layer->Add(mock_layer);

  preroll_context()->state_stack.set_preroll_delegate(device_cull_rect,
                                                      initial_transform);
  layer->Preroll(preroll_context());
  EXPECT_EQ(mock_layer->paint_bounds(), child_path.GetBounds());
  EXPECT_EQ(layer->paint_bounds(),
            mock_layer->paint_bounds().TransformAndClipBounds(layer_transform));
  EXPECT_EQ(layer->child_paint_bounds(), mock_layer->paint_bounds());
  EXPECT_TRUE(mock_layer->needs_painting(paint_context()));
  EXPECT_TRUE(layer->needs_painting(paint_context()));
  EXPECT_EQ(mock_layer->parent_matrix(), initial_transform * layer_transform);
  EXPECT_EQ(mock_layer->parent_cull_rect(),
            local_cull_rect.TransformAndClipBounds(inverse_layer_transform));
  EXPECT_EQ(mock_layer->parent_mutators(),
            std::vector({Mutator(layer_transform)}));

  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected_builder;
  /* (Transform)layer::Paint */ {
    expected_builder.Save();
    {
      expected_builder.Transform(layer_transform);
      /* mock_layer::Paint */ {
        expected_builder.DrawPath(child_path, DlPaint());
      }
    }
    expected_builder.Restore();
  }
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected_builder.Build()));
}

TEST_F(TransformLayerTest, OutputRelativeTransformUsesFallbackSize) {
  const DlPath child_path = DlPath::MakeRectLTRB(0, 400, 20, 430);
  auto mock_layer = std::make_shared<MockLayer>(child_path, DlPaint());
  auto layer = std::make_shared<OutputRelativeTransformLayer>(
      DlPoint(0.25f, -0.5f), DlSize(400, 200));
  layer->Add(mock_layer);

  const DlMatrix expected_transform = DlMatrix::MakeTranslation({100, -100});
  layer->Preroll(preroll_context());

  EXPECT_EQ(mock_layer->parent_matrix(), expected_transform);
  EXPECT_EQ(layer->paint_bounds(),
            child_path.GetBounds().TransformAndClipBounds(expected_transform));

  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected_builder;
  expected_builder.Save();
  expected_builder.Transform(expected_transform);
  expected_builder.DrawPath(child_path, DlPaint());
  expected_builder.Restore();
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected_builder.Build()));
}

TEST_F(TransformLayerTest, OutputRelativeTransformUsesActiveOutputSize) {
  const DlPath child_path = DlPath::MakeRectLTRB(0, 400, 20, 430);
  auto mock_layer = std::make_shared<MockLayer>(child_path, DlPaint());
  auto layer = std::make_shared<OutputRelativeTransformLayer>(
      DlPoint(0.25f, -0.5f), DlSize(400, 200));
  layer->Add(mock_layer);

  const DlSize output_size(200, 600);
  preroll_context()->denial_render_output_logical_size = output_size;
  paint_context().denial_render_output_logical_size = output_size;
  const DlMatrix expected_transform = DlMatrix::MakeTranslation({50, -300});
  layer->Preroll(preroll_context());

  EXPECT_EQ(mock_layer->parent_matrix(), expected_transform);
  EXPECT_EQ(layer->paint_bounds(),
            child_path.GetBounds().TransformAndClipBounds(expected_transform));

  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected_builder;
  expected_builder.Save();
  expected_builder.Transform(expected_transform);
  expected_builder.DrawPath(child_path, DlPaint());
  expected_builder.Restore();
  // A retained transform is the only additional render operation. In
  // particular, this contains no saveLayer or offscreen image pass.
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected_builder.Build()));
}

TEST_F(TransformLayerTest, ComplexMatrix) {
  const DlPath child_path = DlPath::MakeRectLTRB(5.0f, 6.0f, 20.5f, 21.5f);
  DlMatrix initial_transform = DlMatrix::MakeTranslation({-2.0f, -2.0f});
  DlRect local_cull_rect = DlRect::MakeXYWH(4.0f, 4.0f, 16.0f, 16.0f);
  DlRect device_cull_rect =
      local_cull_rect.TransformAndClipBounds(initial_transform);
  DlMatrix layer_transform = (DlMatrix::MakeTranslation({1.0f, 1.0f}) *
                              DlMatrix::MakeRotationX(DlDegrees(20.0f))) *
                             DlMatrix::MakeRotationY(DlDegrees(10.0f));

  EXPECT_TRUE(layer_transform.IsInvertible());
  DlMatrix inverse_layer_transform = layer_transform.Invert();

  auto mock_layer = std::make_shared<MockLayer>(child_path, DlPaint());
  auto layer = std::make_shared<TransformLayer>(layer_transform);
  layer->Add(mock_layer);

  preroll_context()->state_stack.set_preroll_delegate(device_cull_rect,
                                                      initial_transform);
  layer->Preroll(preroll_context());
  EXPECT_EQ(mock_layer->paint_bounds(), child_path.GetBounds());
  EXPECT_EQ(layer->paint_bounds(),
            mock_layer->paint_bounds().TransformAndClipBounds(layer_transform));
  EXPECT_EQ(layer->child_paint_bounds(), mock_layer->paint_bounds());
  EXPECT_TRUE(mock_layer->needs_painting(paint_context()));
  EXPECT_TRUE(layer->needs_painting(paint_context()));
  EXPECT_EQ(mock_layer->parent_matrix(), initial_transform * layer_transform);

  // Even having switched to binary-ieee friendly numbers for the
  // initial conditions, these numbers which are based on a matrix
  // concatenation and inversion still aren't exact, so we are using
  // fuzzy float comparisons to check them.
  DlRect parent_cull_rect = mock_layer->parent_cull_rect();
  DlRect expect_parent_cull_rect =
      local_cull_rect.TransformAndClipBounds(inverse_layer_transform);
  EXPECT_FLOAT_EQ(parent_cull_rect.GetLeft(),
                  expect_parent_cull_rect.GetLeft());
  EXPECT_FLOAT_EQ(parent_cull_rect.GetTop(),  //
                  expect_parent_cull_rect.GetTop());
  EXPECT_FLOAT_EQ(parent_cull_rect.GetRight(),
                  expect_parent_cull_rect.GetRight());
  EXPECT_FLOAT_EQ(parent_cull_rect.GetBottom(),
                  expect_parent_cull_rect.GetBottom());

  EXPECT_EQ(mock_layer->parent_mutators(),
            std::vector({Mutator(layer_transform)}));

  layer->Paint(display_list_paint_context());
  DisplayListBuilder expected_builder;
  /* (Transform)layer::Paint */ {
    expected_builder.Save();
    {
      expected_builder.Transform(layer_transform);
      /* mock_layer::Paint */ {
        expected_builder.DrawPath(child_path, DlPaint());
      }
    }
    expected_builder.Restore();
  }
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected_builder.Build()));
}

TEST_F(TransformLayerTest, Nested) {
  DlPath child_path = DlPath::MakeRectLTRB(5.0f, 6.0f, 20.5f, 21.5f);
  DlMatrix initial_transform = DlMatrix::MakeTranslation({-0.5f, -0.5f});
  DlRect local_cull_rect = DlRect::MakeXYWH(2.0f, 2.0f, 14.0f, 14.0f);
  DlRect device_cull_rect =
      local_cull_rect.TransformAndClipBounds(initial_transform);
  DlMatrix layer1_transform = DlMatrix::MakeTranslation({2.5f, 2.5f});
  DlMatrix layer2_transform = DlMatrix::MakeTranslation({3.5f, 3.5f});
  EXPECT_TRUE(layer1_transform.IsInvertible());
  EXPECT_TRUE(layer2_transform.IsInvertible());
  DlMatrix inverse_layer1_transform = layer1_transform.Invert();
  DlMatrix inverse_layer2_transform = layer2_transform.Invert();

  auto mock_layer = std::make_shared<MockLayer>(child_path, DlPaint());
  auto layer1 = std::make_shared<TransformLayer>(layer1_transform);
  auto layer2 = std::make_shared<TransformLayer>(layer2_transform);
  layer1->Add(layer2);
  layer2->Add(mock_layer);

  preroll_context()->state_stack.set_preroll_delegate(device_cull_rect,
                                                      initial_transform);
  layer1->Preroll(preroll_context());
  EXPECT_EQ(mock_layer->paint_bounds(), child_path.GetBounds());
  EXPECT_EQ(
      layer2->paint_bounds(),
      mock_layer->paint_bounds().TransformAndClipBounds(layer2_transform));
  EXPECT_EQ(layer2->child_paint_bounds(), mock_layer->paint_bounds());
  EXPECT_EQ(layer1->paint_bounds(),
            layer2->paint_bounds().TransformAndClipBounds(layer1_transform));
  EXPECT_EQ(layer1->child_paint_bounds(), layer2->paint_bounds());
  EXPECT_TRUE(mock_layer->needs_painting(paint_context()));
  EXPECT_TRUE(layer2->needs_painting(paint_context()));
  EXPECT_TRUE(layer1->needs_painting(paint_context()));
  EXPECT_EQ(mock_layer->parent_matrix(),
            (initial_transform * layer1_transform) * layer2_transform);
  EXPECT_EQ(mock_layer->parent_cull_rect(),
            local_cull_rect.TransformAndClipBounds(inverse_layer1_transform)
                .TransformAndClipBounds(inverse_layer2_transform));
  EXPECT_EQ(
      mock_layer->parent_mutators(),
      std::vector({Mutator(layer1_transform), Mutator(layer2_transform)}));

  layer1->Paint(display_list_paint_context());
  DisplayListBuilder expected_builder;
  /* (Transform)layer1::Paint */ {
    expected_builder.Save();
    {
      expected_builder.Transform(layer1_transform);
      /* (Transform)layer1::Paint */ {
        expected_builder.Save();
        {
          expected_builder.Transform(layer2_transform);
          /* mock_layer::Paint */ {
            expected_builder.DrawPath(child_path, DlPaint());
          }
        }
        expected_builder.Restore();
      }
    }
    expected_builder.Restore();
  }
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected_builder.Build()));
}

TEST_F(TransformLayerTest, NestedSeparated) {
  DlPath child_path = DlPath::MakeRectLTRB(5.0f, 6.0f, 20.5f, 21.5f);
  DlMatrix initial_transform = DlMatrix::MakeTranslation({-0.5f, -0.5f});
  DlRect local_cull_rect = DlRect::MakeXYWH(2.0f, 2.0f, 14.0f, 14.0f);
  DlRect device_cull_rect =
      local_cull_rect.TransformAndClipBounds(initial_transform);
  DlMatrix layer1_transform = DlMatrix::MakeTranslation({2.5f, 2.5f});
  DlMatrix layer2_transform = DlMatrix::MakeTranslation({3.5f, 3.5f});
  EXPECT_TRUE(layer1_transform.IsInvertible());
  EXPECT_TRUE(layer2_transform.IsInvertible());
  DlMatrix inverse_layer1_transform = layer1_transform.Invert();
  DlMatrix inverse_layer2_transform = layer2_transform.Invert();
  DlPaint child_paint1(DlColor::kBlue());
  DlPaint child_paint2(DlColor::kGreen());

  auto mock_layer1 = std::make_shared<MockLayer>(child_path, child_paint1);
  auto mock_layer2 = std::make_shared<MockLayer>(child_path, child_paint2);
  auto layer1 = std::make_shared<TransformLayer>(layer1_transform);
  auto layer2 = std::make_shared<TransformLayer>(layer2_transform);
  layer1->Add(mock_layer1);
  layer1->Add(layer2);
  layer2->Add(mock_layer2);

  preroll_context()->state_stack.set_preroll_delegate(device_cull_rect,
                                                      initial_transform);
  layer1->Preroll(preroll_context());
  DlRect layer1_child_bounds =
      layer2->paint_bounds().Union(mock_layer1->paint_bounds());
  DlRect expected_layer1_bounds =
      layer1_child_bounds.TransformAndClipBounds(layer1_transform);

  EXPECT_EQ(mock_layer2->paint_bounds(), child_path.GetBounds());
  EXPECT_EQ(
      layer2->paint_bounds(),
      mock_layer2->paint_bounds().TransformAndClipBounds(layer2_transform));
  EXPECT_EQ(layer2->child_paint_bounds(), mock_layer2->paint_bounds());
  EXPECT_EQ(mock_layer1->paint_bounds(), child_path.GetBounds());
  EXPECT_EQ(layer1->paint_bounds(), expected_layer1_bounds);
  EXPECT_EQ(layer1->child_paint_bounds(), layer1_child_bounds);
  EXPECT_TRUE(mock_layer2->needs_painting(paint_context()));
  EXPECT_TRUE(layer2->needs_painting(paint_context()));
  EXPECT_TRUE(mock_layer1->needs_painting(paint_context()));
  EXPECT_TRUE(layer1->needs_painting(paint_context()));
  EXPECT_EQ(mock_layer1->parent_matrix(), initial_transform * layer1_transform);
  EXPECT_EQ(mock_layer2->parent_matrix(),
            (initial_transform * layer1_transform) * layer2_transform);
  EXPECT_EQ(mock_layer1->parent_cull_rect(),
            local_cull_rect.TransformAndClipBounds(inverse_layer1_transform));
  EXPECT_EQ(mock_layer2->parent_cull_rect(),
            local_cull_rect.TransformAndClipBounds(inverse_layer1_transform)
                .TransformAndClipBounds(inverse_layer2_transform));
  EXPECT_EQ(mock_layer1->parent_mutators(),
            std::vector({Mutator(layer1_transform)}));
  EXPECT_EQ(
      mock_layer2->parent_mutators(),
      std::vector({Mutator(layer1_transform), Mutator(layer2_transform)}));

  layer1->Paint(display_list_paint_context());
  DisplayListBuilder expected_builder;
  /* (Transform)layer1::Paint */ {
    expected_builder.Save();
    {
      expected_builder.Transform(layer1_transform);
      /* mock_layer::Paint */ {
        expected_builder.DrawPath(child_path, child_paint1);
      }
      /* (Transform)layer1::Paint */ {
        expected_builder.Save();
        {
          expected_builder.Transform(layer2_transform);
          /* mock_layer::Paint */ {
            expected_builder.DrawPath(child_path, child_paint2);
          }
        }
        expected_builder.Restore();
      }
    }
    expected_builder.Restore();
  }
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected_builder.Build()));
}

TEST_F(TransformLayerTest, OpacityInheritance) {
  auto path1 = DlPath::MakeRectLTRB(10, 10, 30, 30);
  auto mock1 = MockLayer::MakeOpacityCompatible(path1);
  auto transform1 =
      std::make_shared<TransformLayer>(DlMatrix::MakeScale({2.0f, 2.0f, 1.0f}));
  transform1->Add(mock1);

  // TransformLayer will pass through compatibility from a compatible child
  PrerollContext* context = preroll_context();
  transform1->Preroll(context);
  EXPECT_EQ(context->renderable_state_flags,
            LayerStateStack::kCallerCanApplyOpacity);

  auto path2 = DlPath::MakeRectLTRB(40, 40, 50, 50);
  auto mock2 = MockLayer::MakeOpacityCompatible(path2);
  transform1->Add(mock2);

  // TransformLayer will pass through compatibility from multiple
  // non-overlapping compatible children
  transform1->Preroll(context);
  EXPECT_EQ(context->renderable_state_flags,
            LayerStateStack::kCallerCanApplyOpacity);

  auto path3 = DlPath::MakeRectLTRB(20, 20, 40, 40);
  auto mock3 = MockLayer::MakeOpacityCompatible(path3);
  transform1->Add(mock3);

  // TransformLayer will not pass through compatibility from multiple
  // overlapping children even if they are individually compatible
  transform1->Preroll(context);
  EXPECT_EQ(context->renderable_state_flags, 0);

  auto transform2 =
      std::make_shared<TransformLayer>(DlMatrix::MakeScale({2.0f, 2.0f, 1.0f}));
  transform2->Add(mock1);
  transform2->Add(mock2);

  // Double check first two children are compatible and non-overlapping
  transform2->Preroll(context);
  EXPECT_EQ(context->renderable_state_flags,
            LayerStateStack::kCallerCanApplyOpacity);

  auto path4 = DlPath::MakeRectLTRB(60, 60, 70, 70);
  auto mock4 = MockLayer::Make(path4);
  transform2->Add(mock4);

  // The third child is non-overlapping, but not compatible so the
  // TransformLayer should end up incompatible
  transform2->Preroll(context);
  EXPECT_EQ(context->renderable_state_flags, 0);
}

TEST_F(TransformLayerTest, OpacityInheritancePainting) {
  auto path1 = DlPath::MakeRectLTRB(10, 10, 30, 30);
  auto mock1 = MockLayer::MakeOpacityCompatible(path1);
  auto path2 = DlPath::MakeRectLTRB(40, 40, 50, 50);
  auto mock2 = MockLayer::MakeOpacityCompatible(path2);
  auto transform = DlMatrix::MakeScale({2.0f, 2.0f, 1.0f});
  auto transform_layer = std::make_shared<TransformLayer>(transform);
  transform_layer->Add(mock1);
  transform_layer->Add(mock2);

  // TransformLayer will pass through compatibility from multiple
  // non-overlapping compatible children
  PrerollContext* context = preroll_context();
  transform_layer->Preroll(context);
  EXPECT_EQ(context->renderable_state_flags,
            LayerStateStack::kCallerCanApplyOpacity);

  uint8_t opacity_alpha = 0x7F;
  DlPoint offset = DlPoint(10, 10);
  auto opacity_layer = std::make_shared<OpacityLayer>(opacity_alpha, offset);
  opacity_layer->Add(transform_layer);
  opacity_layer->Preroll(context);
  EXPECT_TRUE(opacity_layer->children_can_accept_opacity());

  DisplayListBuilder expected_builder;
  /* opacity_layer paint */ {
    expected_builder.Save();
    {
      expected_builder.Translate(offset.x, offset.y);
      /* transform_layer paint */ {
        expected_builder.Save();
        expected_builder.Transform(transform);
        /* child layer1 paint */ {
          expected_builder.DrawPath(path1, DlPaint().setAlpha(opacity_alpha));
        }
        /* child layer2 paint */ {
          expected_builder.DrawPath(path2, DlPaint().setAlpha(opacity_alpha));
        }
        expected_builder.Restore();
      }
    }
    expected_builder.Restore();
  }

  opacity_layer->Paint(display_list_paint_context());
  EXPECT_TRUE(DisplayListsEQ_Verbose(display_list(), expected_builder.Build()));
}

using TransformLayerLayerDiffTest = DiffContextTest;

TEST_F(TransformLayerLayerDiffTest, Transform) {
  auto path1 = DlPath::MakeRectLTRB(0, 0, 50, 50);
  auto m1 = std::make_shared<MockLayer>(path1);

  auto transform1 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({10, 10}));
  transform1->Add(m1);

  MockLayerTree t1;
  t1.root()->Add(transform1);

  auto damage = DiffLayerTree(t1, MockLayerTree());
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(10, 10, 60, 60));

  auto transform2 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({20, 20}));
  transform2->Add(m1);
  transform2->AssignOldLayer(transform1.get());

  MockLayerTree t2;
  t2.root()->Add(transform2);

  damage = DiffLayerTree(t2, t1);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(10, 10, 70, 70));

  auto transform3 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({20, 20}));
  transform3->Add(m1);
  transform3->AssignOldLayer(transform2.get());

  MockLayerTree t3;
  t3.root()->Add(transform3);

  damage = DiffLayerTree(t3, t2);
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect());
}

TEST_F(TransformLayerLayerDiffTest,
       OutputRelativeTransformDiffUsesActiveOutputSize) {
  auto child = std::make_shared<MockLayer>(DlPath::MakeRectLTRB(0, 0, 50, 50));

  auto transform1 = std::make_shared<OutputRelativeTransformLayer>(
      DlPoint(0.25f, 0), DlSize(400, 400));
  transform1->Add(child);
  MockLayerTree tree1(DlISize(500, 500));
  tree1.root()->Add(transform1);

  MockLayerTree empty(DlISize(500, 500));
  DiffContext first_context(tree1.size(), tree1.paint_region_map(),
                            empty.paint_region_map(), true, false);
  first_context.set_denial_render_output_logical_size(DlSize(200, 300));
  first_context.PushCullRect(DlRect::MakeSize(tree1.size()));
  tree1.root()->Diff(&first_context, empty.root());
  EXPECT_EQ(first_context.ComputeDamage(DlRegion()).frame_damage.bounds(),
            DlIRect::MakeLTRB(50, 0, 100, 50));

  auto transform2 = std::make_shared<OutputRelativeTransformLayer>(
      DlPoint(0.5f, 0), DlSize(400, 400));
  transform2->Add(child);
  transform2->AssignOldLayer(transform1.get());
  MockLayerTree tree2(DlISize(500, 500));
  tree2.root()->Add(transform2);

  DiffContext second_context(tree2.size(), tree2.paint_region_map(),
                             tree1.paint_region_map(), true, false);
  second_context.set_denial_render_output_logical_size(DlSize(200, 300));
  second_context.PushCullRect(DlRect::MakeSize(tree2.size()));
  tree2.root()->Diff(&second_context, tree1.root());
  EXPECT_EQ(second_context.ComputeDamage(DlRegion()).frame_damage.bounds(),
            DlIRect::MakeLTRB(50, 0, 150, 50));
}

TEST_F(TransformLayerLayerDiffTest, TransformNested) {
  auto path1 = DlPath::MakeRectLTRB(0, 0, 50, 50);
  auto m1 = CreateContainerLayer(std::make_shared<MockLayer>(path1));
  auto m2 = CreateContainerLayer(std::make_shared<MockLayer>(path1));
  auto m3 = CreateContainerLayer(std::make_shared<MockLayer>(path1));

  auto transform1 =
      std::make_shared<TransformLayer>(DlMatrix::MakeScale({2.0f, 2.0f, 1.0f}));

  auto transform1_1 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({10, 10}));
  transform1_1->Add(m1);
  transform1->Add(transform1_1);

  auto transform1_2 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({100, 100}));
  transform1_2->Add(m2);
  transform1->Add(transform1_2);

  auto transform1_3 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({200, 200}));
  transform1_3->Add(m3);
  transform1->Add(transform1_3);

  MockLayerTree l1;
  l1.root()->Add(transform1);

  auto damage = DiffLayerTree(l1, MockLayerTree());
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(20, 20, 500, 500));

  auto transform2 =
      std::make_shared<TransformLayer>(DlMatrix::MakeScale({2.0f, 2.0f, 1.0f}));

  auto transform2_1 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({10, 10}));
  transform2_1->Add(m1);
  transform2_1->AssignOldLayer(transform1_1.get());
  transform2->Add(transform2_1);

  // Offset 1px from transform1_2 so that they're not the same
  auto transform2_2 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({100, 101}));
  transform2_2->Add(m2);
  transform2_2->AssignOldLayer(transform1_2.get());
  transform2->Add(transform2_2);

  auto transform2_3 =
      std::make_shared<TransformLayer>(DlMatrix::MakeTranslation({200, 200}));
  transform2_3->Add(m3);
  transform2_3->AssignOldLayer(transform1_3.get());
  transform2->Add(transform2_3);

  MockLayerTree l2;
  l2.root()->Add(transform2);

  damage = DiffLayerTree(l2, l1);

  // transform2 has not transform1 assigned as old layer, so it should be
  // invalidated completely
  EXPECT_EQ(damage.frame_damage.bounds(), DlIRect::MakeLTRB(20, 20, 500, 500));

  // now diff the tree properly, the only difference being transform2_2 and
  // transform_2_1
  transform2->AssignOldLayer(transform1.get());
  damage = DiffLayerTree(l2, l1);

  EXPECT_EQ(damage.frame_damage.bounds(),
            DlIRect::MakeLTRB(200, 200, 300, 302));
}

}  // namespace testing
}  // namespace flutter
