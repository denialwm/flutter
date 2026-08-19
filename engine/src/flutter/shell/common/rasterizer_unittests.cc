// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER

#include "flow/surface_frame.h"
#include "flutter/shell/common/rasterizer.h"

#include <memory>
#include <optional>

#include "flutter/flow/frame_timings.h"
#include "flutter/flow/layers/clip_rect_layer.h"
#include "flutter/flow/layers/transform_layer.h"
#include "flutter/fml/synchronization/count_down_latch.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/shell/common/thread_host.h"
#include "flutter/testing/testing.h"

#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrTypes.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"

#include "gmock/gmock.h"

using testing::_;
using testing::ByMove;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

namespace flutter {
namespace {

constexpr float kDevicePixelRatio = 2.0f;
constexpr int64_t kImplicitViewId = 0;

std::vector<std::unique_ptr<LayerTreeTask>> SingleLayerTreeList(
    int64_t view_id,
    std::unique_ptr<LayerTree> layer_tree,
    float pixel_ratio) {
  std::vector<std::unique_ptr<LayerTreeTask>> tasks;
  tasks.push_back(std::make_unique<LayerTreeTask>(
      view_id, std::move(layer_tree), pixel_ratio));
  return tasks;
}

class MockDelegate : public Rasterizer::Delegate {
 public:
  MOCK_METHOD(void,
              OnFrameRasterized,
              (const FrameTiming& frame_timing),
              (override));
  MOCK_METHOD(fml::Milliseconds, GetFrameBudget, (), (override));
  MOCK_METHOD(fml::TimePoint, GetLatestFrameTargetTime, (), (const, override));
  MOCK_METHOD(const TaskRunners&, GetTaskRunners, (), (const, override));
  MOCK_METHOD(const fml::RefPtr<fml::RasterThreadMerger>,
              GetParentRasterThreadMerger,
              (),
              (const, override));
  MOCK_METHOD(std::shared_ptr<const fml::SyncSwitch>,
              GetIsGpuDisabledSyncSwitch,
              (),
              (const, override));
  MOCK_METHOD(const Settings&, GetSettings, (), (const, override));
  MOCK_METHOD(bool,
              ShouldDiscardLayerTree,
              (int64_t, const flutter::LayerTree&),
              (override));
};

class MockSurface : public Surface {
 public:
  MOCK_METHOD(bool, IsValid, (), (override));
  MOCK_METHOD(std::unique_ptr<SurfaceFrame>,
              AcquireFrame,
              (const DlISize& size),
              (override));
  MOCK_METHOD(DlMatrix, GetRootTransformation, (), (const, override));
  MOCK_METHOD(GrDirectContext*, GetContext, (), (override));
  MOCK_METHOD(std::unique_ptr<GLContextResult>,
              MakeRenderContextCurrent,
              (),
              (override));
  MOCK_METHOD(bool, ClearRenderContext, (), (override));
  MOCK_METHOD(bool, AllowsDrawingWhenGpuDisabled, (), (const, override));
};

class MockExternalViewEmbedder : public ExternalViewEmbedder {
 public:
  MOCK_METHOD(DlCanvas*, GetRootCanvas, (), (override));
  MOCK_METHOD(void, CancelFrame, (), (override));
  MOCK_METHOD(
      void,
      BeginFrame,
      (GrDirectContext * context,
       const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger),
      (override));
  MOCK_METHOD(void,
              PrepareFlutterView,
              (DlISize frame_size, double device_pixel_ratio),
              (override));
  MOCK_METHOD(void,
              PrerollCompositeEmbeddedView,
              (int64_t view_id, std::unique_ptr<EmbeddedViewParams> params),
              (override));
  MOCK_METHOD(
      PostPrerollResult,
      PostPrerollAction,
      (const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger),
      (override));
  MOCK_METHOD(DlCanvas*, CompositeEmbeddedView, (int64_t view_id), (override));
  MOCK_METHOD(void,
              SubmitFlutterView,
              (int64_t flutter_view_id,
               GrDirectContext* context,
               const std::shared_ptr<impeller::AiksContext>& aiks_context,
               std::unique_ptr<SurfaceFrame> frame),
              (override));
  MOCK_METHOD(
      void,
      EndFrame,
      (bool should_resubmit_frame,
       const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger),
      (override));
  MOCK_METHOD(bool, SupportsDynamicThreadMerging, (), (override));
};
}  // namespace

class RasterizerTestPeer {
 public:
  static std::vector<std::unique_ptr<LayerTreeTask>> ExpandRenderOutputs(
      Rasterizer& rasterizer,
      std::vector<std::unique_ptr<LayerTreeTask>> tasks) {
    return rasterizer.ExpandDenialRenderOutputTasks(std::move(tasks));
  }

  static const DenialRenderOutput* FindRenderOutput(Rasterizer& rasterizer,
                                                    int64_t view_id) {
    return rasterizer.FindDenialRenderOutput(view_id);
  }

  static uint64_t ConfigurationGeneration(const Rasterizer& rasterizer) {
    return rasterizer.denial_render_output_generation_;
  }

  static const LayerTreeTask* PendingRenderOutput(const Rasterizer& rasterizer,
                                                  int64_t view_id) {
    auto found = rasterizer.denial_pending_output_tasks_.find(view_id);
    return found == rasterizer.denial_pending_output_tasks_.end()
               ? nullptr
               : found->second.get();
  }
};

TEST(RasterizerTest, create) {
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  EXPECT_TRUE(rasterizer != nullptr);
}

TEST(RasterizerTest, DenialRenderOutputsProjectOneSceneToNativeTargets) {
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  Rasterizer rasterizer(delegate);

  constexpr uint64_t kGeneration = 41;
  const DenialRenderOutput right_output = {
      .render_view_id = -3,
      .configuration_generation = kGeneration,
      .source_physical_bounds = DlRect::MakeXYWH(200, 100, 800, 600),
      .target_size = DlISize(1200, 900),
      .scale_120 = 180,
      .transform = DenialRenderOutputTransform::kNormal,
  };
  const DenialRenderOutput left_output = {
      .render_view_id = -8,
      .configuration_generation = kGeneration,
      .source_physical_bounds = DlRect::MakeXYWH(0, 0, 200, 900),
      .target_size = DlISize(200, 900),
      .scale_120 = 120,
      .transform = DenialRenderOutputTransform::kNormal,
  };
  rasterizer.SetDenialRenderOutputs({right_output, left_output});

  EXPECT_EQ(RasterizerTestPeer::ConfigurationGeneration(rasterizer),
            kGeneration);
  ASSERT_NE(RasterizerTestPeer::FindRenderOutput(rasterizer, -8), nullptr);
  ASSERT_NE(RasterizerTestPeer::FindRenderOutput(rasterizer, -3), nullptr);
  EXPECT_EQ(RasterizerTestPeer::FindRenderOutput(rasterizer, -8)->target_size,
            left_output.target_size);
  EXPECT_EQ(RasterizerTestPeer::FindRenderOutput(rasterizer, -3)->target_size,
            right_output.target_size);

  auto source_root = std::make_shared<ContainerLayer>();
  auto tasks = SingleLayerTreeList(
      kImplicitViewId,
      std::make_unique<LayerTree>(source_root, DlISize(1000, 900)), 1.5f);
  tasks.front()->is_reused_layer_tree = true;
  tasks.front()->dirty_texture_ids = std::unordered_set<int64_t>{7, 11};
  auto expanded =
      RasterizerTestPeer::ExpandRenderOutputs(rasterizer, std::move(tasks));

  ASSERT_EQ(expanded.size(), 2u);
  EXPECT_EQ(expanded[0]->view_id, -8);
  EXPECT_EQ(expanded[1]->view_id, -3);
  EXPECT_EQ(expanded[0]->layer_tree->frame_size(), DlISize(200, 900));
  EXPECT_EQ(expanded[1]->layer_tree->frame_size(), DlISize(1200, 900));
  EXPECT_FLOAT_EQ(expanded[0]->device_pixel_ratio, 1.0f);
  EXPECT_FLOAT_EQ(expanded[1]->device_pixel_ratio, 1.5f);
  EXPECT_EQ(expanded[0]->render_output_configuration_generation, kGeneration);
  EXPECT_EQ(expanded[1]->render_output_configuration_generation, kGeneration);
  ASSERT_TRUE(expanded[0]->dirty_texture_ids.has_value());
  ASSERT_TRUE(expanded[1]->dirty_texture_ids.has_value());
  EXPECT_EQ(*expanded[0]->dirty_texture_ids,
            (std::unordered_set<int64_t>{7, 11}));
  EXPECT_EQ(*expanded[1]->dirty_texture_ids,
            (std::unordered_set<int64_t>{7, 11}));

  const DenialRenderOutput expected_outputs[] = {left_output, right_output};
  for (size_t index = 0; index < expanded.size(); index++) {
    const auto& task = expanded[index];
    const auto& expected = expected_outputs[index];
    auto* clip = static_cast<ClipRectLayer*>(task->layer_tree->root_layer());
    ASSERT_NE(clip, nullptr);
    EXPECT_EQ(clip->clip_rect(), DlRect::MakeWH(expected.target_size.width,
                                                expected.target_size.height));
    ASSERT_EQ(clip->layers().size(), 1u);
    auto* transform =
        static_cast<TransformLayer*>(clip->layers().front().get());
    ASSERT_NE(transform, nullptr);
    ASSERT_EQ(transform->layers().size(), 1u);
    EXPECT_EQ(transform->layers().front(), source_root);
    EXPECT_EQ(expected.source_physical_bounds.TransformAndClipBounds(
                  transform->transform()),
              DlRect::MakeWH(expected.target_size.width,
                             expected.target_size.height));
  }
}

TEST(RasterizerTest, DenialSyntheticRenderTasksAreNeverExpandedAgain) {
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  Rasterizer rasterizer(delegate);
  rasterizer.SetDenialRenderOutputs({{
      .render_view_id = -1,
      .configuration_generation = 9,
      .source_physical_bounds = DlRect::MakeXYWH(0, 0, 800, 600),
      .target_size = DlISize(800, 600),
      .scale_120 = 120,
      .transform = DenialRenderOutputTransform::kNormal,
  }});

  auto root = std::make_shared<ContainerLayer>();
  auto tasks = SingleLayerTreeList(
      -1, std::make_unique<LayerTree>(root, DlISize(800, 600)), 1.0f);
  tasks.front()->is_reused_layer_tree = true;
  auto expanded =
      RasterizerTestPeer::ExpandRenderOutputs(rasterizer, std::move(tasks));

  ASSERT_EQ(expanded.size(), 1u);
  EXPECT_EQ(expanded.front()->view_id, -1);
  EXPECT_EQ(expanded.front()->layer_tree->root_layer_shared(), root);
  EXPECT_TRUE(expanded.front()->is_reused_layer_tree);
}

TEST(RasterizerTest, DenialRenderSelectionDefersOtherOutputs) {
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  Rasterizer rasterizer(delegate);
  rasterizer.SetDenialRenderOutputs({
      {
          .render_view_id = -2,
          .configuration_generation = 17,
          .source_physical_bounds = DlRect::MakeXYWH(0, 0, 400, 600),
          .target_size = DlISize(400, 600),
          .scale_120 = 120,
          .transform = DenialRenderOutputTransform::kNormal,
      },
      {
          .render_view_id = -1,
          .configuration_generation = 17,
          .source_physical_bounds = DlRect::MakeXYWH(400, 0, 800, 600),
          .target_size = DlISize(1200, 900),
          .scale_120 = 180,
          .transform = DenialRenderOutputTransform::kNormal,
      },
  });

  rasterizer.PrepareDenialRenderOutputs({-2}, {});
  auto source_root = std::make_shared<ContainerLayer>();
  auto tasks = SingleLayerTreeList(
      kImplicitViewId,
      std::make_unique<LayerTree>(source_root, DlISize(1200, 600)), 1.0f);
  auto selected =
      RasterizerTestPeer::ExpandRenderOutputs(rasterizer, std::move(tasks));

  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected.front()->view_id, -2);
  const auto* pending = RasterizerTestPeer::PendingRenderOutput(rasterizer, -1);
  ASSERT_NE(pending, nullptr);
  EXPECT_EQ(pending->layer_tree->frame_size(), DlISize(1200, 900));
  auto* clip = static_cast<ClipRectLayer*>(pending->layer_tree->root_layer());
  ASSERT_EQ(clip->layers().size(), 1u);
  auto* transform = static_cast<TransformLayer*>(clip->layers().front().get());
  ASSERT_EQ(transform->layers().size(), 1u);
  EXPECT_EQ(transform->layers().front(), source_root);
}

TEST(RasterizerTest, DenialRenderOutputReplacementDropsTheOldGeneration) {
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  Rasterizer rasterizer(delegate);
  rasterizer.SetDenialRenderOutputs({{
      .render_view_id = -4,
      .configuration_generation = 11,
      .source_physical_bounds = DlRect::MakeXYWH(0, 0, 640, 480),
      .target_size = DlISize(640, 480),
      .scale_120 = 120,
      .transform = DenialRenderOutputTransform::kNormal,
  }});
  ASSERT_NE(RasterizerTestPeer::FindRenderOutput(rasterizer, -4), nullptr);

  rasterizer.SetDenialRenderOutputs({{
      .render_view_id = -9,
      .configuration_generation = 12,
      .source_physical_bounds = DlRect::MakeXYWH(0, 0, 1280, 720),
      .target_size = DlISize(1920, 1080),
      .scale_120 = 180,
      .transform = DenialRenderOutputTransform::kRotate90,
  }});

  EXPECT_EQ(RasterizerTestPeer::FindRenderOutput(rasterizer, -4), nullptr);
  ASSERT_NE(RasterizerTestPeer::FindRenderOutput(rasterizer, -9), nullptr);
  EXPECT_EQ(RasterizerTestPeer::ConfigurationGeneration(rasterizer), 12u);
}

TEST(RasterizerTest, isAiksContextInitialized) {
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  auto rasterizer = std::make_shared<Rasterizer>(delegate);

  EXPECT_TRUE(rasterizer != nullptr);
  std::shared_ptr<SnapshotController::Delegate> snapshot_delegate = rasterizer;

  EXPECT_FALSE(snapshot_delegate->IsAiksContextInitialized());
}

static std::unique_ptr<FrameTimingsRecorder> CreateFinishedBuildRecorder(
    fml::TimePoint timestamp) {
  std::unique_ptr<FrameTimingsRecorder> recorder =
      std::make_unique<FrameTimingsRecorder>();
  recorder->RecordVsync(timestamp, timestamp);
  recorder->RecordBuildStart(timestamp);
  recorder->RecordBuildEnd(timestamp);
  return recorder;
}

static std::unique_ptr<FrameTimingsRecorder> CreateFinishedBuildRecorder() {
  return CreateFinishedBuildRecorder(fml::TimePoint::Now());
}

TEST(RasterizerTest, drawEmptyPipeline) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  ON_CALL(delegate, GetTaskRunners()).WillByDefault(ReturnRef(task_runners));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));
  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest,
     drawWithExternalViewEmbedderExternalViewEmbedderSubmitFrameCalled) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();

  std::shared_ptr<NiceMock<MockExternalViewEmbedder>> external_view_embedder =
      std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  EXPECT_CALL(*surface, AcquireFrame(DlISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  EXPECT_CALL(*external_view_embedder,
              BeginFrame(/*context=*/nullptr,
                         /*raster_thread_merger=*/
                         fml::RefPtr<fml::RasterThreadMerger>(nullptr)))
      .Times(1);
  EXPECT_CALL(*external_view_embedder, PrepareFlutterView(
                                           /*frame_size=*/DlISize(),
                                           /*device_pixel_ratio=*/2.0))
      .Times(1);
  EXPECT_CALL(*external_view_embedder,
              SubmitFlutterView(/*flutter_view_id=*/kImplicitViewId, _, _, _))
      .Times(1);
  EXPECT_CALL(
      *external_view_embedder,
      EndFrame(/*should_resubmit_frame=*/false,
               /*raster_thread_merger=*/fml::RefPtr<fml::RasterThreadMerger>(
                   nullptr)))
      .Times(1);

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(/*root_layer=*/nullptr,
                                                  /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithExternalViewEmbedderAndThreadMergerNotMergedExternalViewEmbedderSubmitFrameNotCalled) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  std::shared_ptr<NiceMock<MockExternalViewEmbedder>> external_view_embedder =
      std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);
  EXPECT_CALL(*external_view_embedder, SupportsDynamicThreadMerging)
      .WillRepeatedly(Return(true));
  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;
  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  EXPECT_CALL(*surface, AcquireFrame(DlISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  EXPECT_CALL(*external_view_embedder, BeginFrame(/*context=*/nullptr,
                                                  /*raster_thread_merger=*/_))
      .Times(1);
  EXPECT_CALL(*external_view_embedder, PrepareFlutterView(
                                           /*frame_size=*/DlISize(),
                                           /*device_pixel_ratio=*/2.0))
      .Times(1);
  EXPECT_CALL(*external_view_embedder,
              SubmitFlutterView(/*flutter_view_id=*/kImplicitViewId, _, _, _))
      .Times(0);
  EXPECT_CALL(*external_view_embedder, EndFrame(/*should_resubmit_frame=*/false,
                                                /*raster_thread_merger=*/_))
      .Times(1);

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithExternalViewEmbedderAndThreadsMergedExternalViewEmbedderSubmitFrameCalled) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  TaskRunners task_runners("test",
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());

  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();

  std::shared_ptr<NiceMock<MockExternalViewEmbedder>> external_view_embedder =
      std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  EXPECT_CALL(*surface, AcquireFrame(DlISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));
  EXPECT_CALL(*external_view_embedder, SupportsDynamicThreadMerging)
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*external_view_embedder, BeginFrame(/*context=*/nullptr,
                                                  /*raster_thread_merger=*/_))
      .Times(1);
  EXPECT_CALL(*external_view_embedder, PrepareFlutterView(
                                           /*frame_size=*/DlISize(),
                                           /*device_pixel_ratio=*/2.0))
      .Times(1);
  EXPECT_CALL(*external_view_embedder,
              SubmitFlutterView(/*flutter_view_id=*/kImplicitViewId, _, _, _))
      .Times(1);
  EXPECT_CALL(*external_view_embedder, EndFrame(/*should_resubmit_frame=*/false,
                                                /*raster_thread_merger=*/_))
      .Times(1);

  rasterizer->Setup(std::move(surface));

  auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
  auto layer_tree = std::make_unique<LayerTree>(/*root_layer=*/nullptr,
                                                /*frame_size=*/DlISize());
  auto layer_tree_item = std::make_unique<FrameItem>(
      SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                          kDevicePixelRatio),
      CreateFinishedBuildRecorder());
  PipelineProduceResult result =
      pipeline->Produce().Complete(std::move(layer_tree_item));
  EXPECT_TRUE(result.success);
  ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
  rasterizer->Draw(pipeline);
}

TEST(RasterizerTest,
     drawLastLayerTreeWithThreadsMergedExternalViewEmbedderAndEndFrameCalled) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  TaskRunners task_runners("test",
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           fml::MessageLoop::GetCurrent().GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());

  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();

  std::shared_ptr<NiceMock<MockExternalViewEmbedder>> external_view_embedder =
      std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame1 = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  auto surface_frame2 = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled())
      .WillRepeatedly(Return(true));
  // Prepare two frames for Draw() and DrawLastLayerTrees().
  EXPECT_CALL(*surface, AcquireFrame(DlISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame1))))
      .WillOnce(Return(ByMove(std::move(surface_frame2))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));
  EXPECT_CALL(*external_view_embedder, SupportsDynamicThreadMerging)
      .WillRepeatedly(Return(true));

  EXPECT_CALL(*external_view_embedder, BeginFrame(/*context=*/nullptr,
                                                  /*raster_thread_merger=*/_))
      .Times(2);
  EXPECT_CALL(*external_view_embedder, PrepareFlutterView(
                                           /*frame_size=*/DlISize(),
                                           /*device_pixel_ratio=*/2.0))
      .Times(2);
  EXPECT_CALL(*external_view_embedder,
              SubmitFlutterView(/*flutter_view_id=*/kImplicitViewId, _, _, _))
      .Times(2);
  EXPECT_CALL(*external_view_embedder, EndFrame(/*should_resubmit_frame=*/false,
                                                /*raster_thread_merger=*/_))
      .Times(2);

  rasterizer->Setup(std::move(surface));

  auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
  auto layer_tree = std::make_unique<LayerTree>(/*root_layer=*/nullptr,
                                                /*frame_size=*/DlISize());
  auto layer_tree_item = std::make_unique<FrameItem>(
      SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                          kDevicePixelRatio),
      CreateFinishedBuildRecorder());
  PipelineProduceResult result =
      pipeline->Produce().Complete(std::move(layer_tree_item));
  EXPECT_TRUE(result.success);

  // The Draw() will respectively call BeginFrame(), SubmitFlutterView() and
  // EndFrame() one time.
  ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
  rasterizer->Draw(pipeline);

  // The DrawLastLayerTrees() will respectively call BeginFrame(),
  // SubmitFlutterView() and EndFrame() one more time, totally 2 times.
  rasterizer->DrawLastLayerTrees(CreateFinishedBuildRecorder());
}

TEST(RasterizerTest, externalViewEmbedderDoesntEndFrameWhenNoSurfaceIsSet) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);

  std::shared_ptr<NiceMock<MockExternalViewEmbedder>> external_view_embedder =
      std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);

  EXPECT_CALL(
      *external_view_embedder,
      EndFrame(/*should_resubmit_frame=*/false,
               /*raster_thread_merger=*/fml::RefPtr<fml::RasterThreadMerger>(
                   nullptr)))
      .Times(0);

  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest, externalViewEmbedderDoesntEndFrameWhenNotUsedThisFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(false);
  ON_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillByDefault(Return(is_gpu_disabled_sync_switch));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  std::shared_ptr<NiceMock<MockExternalViewEmbedder>> external_view_embedder =
      std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);
  rasterizer->Setup(std::move(surface));

  EXPECT_CALL(*external_view_embedder, BeginFrame(/*context=*/nullptr,
                                                  /*raster_thread_merger=*/_))
      .Times(0);
  EXPECT_CALL(*external_view_embedder, PrepareFlutterView(
                                           /*frame_size=*/DlISize(),
                                           /*device_pixel_ratio=*/2.0))
      .Times(0);
  EXPECT_CALL(
      *external_view_embedder,
      EndFrame(/*should_resubmit_frame=*/false,
               /*raster_thread_merger=*/fml::RefPtr<fml::RasterThreadMerger>(
                   nullptr)))
      .Times(0);

  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    // Always discard the layer tree.
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(true));
    DrawStatus status = rasterizer->Draw(pipeline);
    EXPECT_EQ(status, DrawStatus::kDone);
    EXPECT_EQ(rasterizer->GetLastDrawStatus(kImplicitViewId),
              DrawSurfaceStatus::kDiscarded);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest, externalViewEmbedderDoesntEndFrameWhenPipelineIsEmpty) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  std::shared_ptr<NiceMock<MockExternalViewEmbedder>> external_view_embedder =
      std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);
  rasterizer->Setup(std::move(surface));

  EXPECT_CALL(
      *external_view_embedder,
      EndFrame(/*should_resubmit_frame=*/false,
               /*raster_thread_merger=*/fml::RefPtr<fml::RasterThreadMerger>(
                   nullptr)))
      .Times(0);

  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    DrawStatus status = rasterizer->Draw(pipeline);
    EXPECT_EQ(status, DrawStatus::kPipelineEmpty);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest, drawMultipleViewsWithExternalViewEmbedder) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  std::shared_ptr<NiceMock<MockExternalViewEmbedder>> external_view_embedder =
      std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  rasterizer->SetExternalViewEmbedder(external_view_embedder);
  EXPECT_CALL(*external_view_embedder, SupportsDynamicThreadMerging)
      .WillRepeatedly(Return(false));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  EXPECT_CALL(*surface, AcquireFrame(DlISize())).Times(2);
  ON_CALL(*surface, AcquireFrame).WillByDefault([](const DlISize& size) {
    SurfaceFrame::FramebufferInfo framebuffer_info;
    framebuffer_info.supports_readback = true;
    return std::make_unique<SurfaceFrame>(
        /*surface=*/
        nullptr, framebuffer_info,
        /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
        /*submit_callback=*/[](const SurfaceFrame&) { return true; },
        /*frame_size=*/DlISize(800, 600));
  });
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  EXPECT_CALL(*external_view_embedder, BeginFrame(/*context=*/nullptr,
                                                  /*raster_thread_merger=*/_))
      .Times(1);
  EXPECT_CALL(*external_view_embedder,
              PrepareFlutterView(/*frame_size=*/DlISize(),
                                 /*device_pixel_ratio=*/1.5))
      .Times(1);
  EXPECT_CALL(*external_view_embedder,
              SubmitFlutterView(/*flutter_view_id=*/0, _, _, _))
      .Times(1);
  EXPECT_CALL(*external_view_embedder,
              PrepareFlutterView(/*frame_size=*/DlISize(),
                                 /*device_pixel_ratio=*/2.0))
      .Times(1);
  EXPECT_CALL(*external_view_embedder,
              SubmitFlutterView(/*flutter_view_id=*/1, _, _, _))
      .Times(1);
  EXPECT_CALL(*external_view_embedder, EndFrame(/*should_resubmit_frame=*/false,
                                                /*raster_thread_merger=*/_))
      .Times(1);

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    std::vector<std::unique_ptr<LayerTreeTask>> tasks;
    tasks.push_back(std::make_unique<LayerTreeTask>(
        0, std::make_unique<LayerTree>(nullptr, DlISize()), 1.5));
    tasks.push_back(std::make_unique<LayerTreeTask>(
        1, std::make_unique<LayerTree>(nullptr, DlISize()), 2.0));
    auto layer_tree_item = std::make_unique<FrameItem>(
        std::move(tasks), CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest,
     drawWithGpuEnabledAndSurfaceAllowsDrawingWhenGpuDisabledDoesAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(false);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;
  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, /*framebuffer_info=*/framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  ON_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillByDefault(Return(is_gpu_disabled_sync_switch));
  EXPECT_CALL(delegate, GetIsGpuDisabledSyncSwitch()).Times(0);
  EXPECT_CALL(*surface, AcquireFrame(DlISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithGpuDisabledAndSurfaceAllowsDrawingWhenGpuDisabledDoesAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(true);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, /*framebuffer_info=*/framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(true));
  ON_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillByDefault(Return(is_gpu_disabled_sync_switch));
  EXPECT_CALL(delegate, GetIsGpuDisabledSyncSwitch()).Times(0);
  EXPECT_CALL(*surface, AcquireFrame(DlISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    DrawStatus status = rasterizer->Draw(pipeline);
    EXPECT_EQ(status, DrawStatus::kDone);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithGpuEnabledAndSurfaceDisallowsDrawingWhenGpuDisabledDoesAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_));
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(false);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, /*framebuffer_info=*/framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(false));
  EXPECT_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillOnce(Return(is_gpu_disabled_sync_switch));
  EXPECT_CALL(*surface, AcquireFrame(DlISize()))
      .WillOnce(Return(ByMove(std::move(surface_frame))));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    DrawStatus status = rasterizer->Draw(pipeline);
    EXPECT_EQ(status, DrawStatus::kDone);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    drawWithGpuDisabledAndSurfaceDisallowsDrawingWhenGpuDisabledDoesntAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_)).Times(0);
  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(true);

  SurfaceFrame::FramebufferInfo framebuffer_info;
  framebuffer_info.supports_readback = true;

  auto surface_frame = std::make_unique<SurfaceFrame>(
      /*surface=*/
      nullptr, /*framebuffer_info=*/framebuffer_info,
      /*encode_callback=*/[](const SurfaceFrame&, DlCanvas*) { return true; },
      /*submit_callback=*/[](const SurfaceFrame&) { return true; },
      /*frame_size=*/DlISize(800, 600));
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillOnce(Return(false));
  EXPECT_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillOnce(Return(is_gpu_disabled_sync_switch));
  EXPECT_CALL(*surface, AcquireFrame(DlISize())).Times(0);
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));

  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    DrawStatus status = rasterizer->Draw(pipeline);
    EXPECT_EQ(status, DrawStatus::kGpuUnavailable);
    latch.Signal();
  });
  latch.Wait();
}

TEST(
    RasterizerTest,
    FrameTimingRecorderShouldStartRecordingRasterTimeBeforeSurfaceAcquireFrame) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));
  EXPECT_CALL(delegate, OnFrameRasterized(_))
      .WillOnce([&](const FrameTiming& frame_timing) {
        fml::TimePoint now = fml::TimePoint::Now();
        fml::TimePoint raster_start =
            frame_timing.Get(FrameTiming::kRasterStart);
        EXPECT_TRUE(now - raster_start < fml::TimeDelta::FromSecondsF(1));
      });

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  auto is_gpu_disabled_sync_switch =
      std::make_shared<const fml::SyncSwitch>(false);
  ON_CALL(delegate, GetIsGpuDisabledSyncSwitch())
      .WillByDefault(Return(is_gpu_disabled_sync_switch));
  ON_CALL(*surface, AcquireFrame(DlISize())).WillByDefault([] {
    return nullptr;
  });
  EXPECT_CALL(*surface, AcquireFrame(DlISize()));
  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillOnce(Return(ByMove(std::make_unique<GLContextDefaultResult>(true))));
  rasterizer->Setup(std::move(surface));
  fml::AutoResetWaitableEvent latch;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                            kDevicePixelRatio),
        CreateFinishedBuildRecorder());
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    DrawStatus status = rasterizer->Draw(pipeline);
    EXPECT_EQ(status, DrawStatus::kDone);
    EXPECT_EQ(rasterizer->GetLastDrawStatus(kImplicitViewId),
              DrawSurfaceStatus::kFailed);
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest,
     drawLayerTreeWithCorrectFrameTimingWhenPipelineIsMoreAvailable) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  ON_CALL(delegate, GetTaskRunners()).WillByDefault(ReturnRef(task_runners));

  fml::AutoResetWaitableEvent latch;
  std::unique_ptr<Rasterizer> rasterizer;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer = std::make_unique<Rasterizer>(delegate);
    latch.Signal();
  });
  latch.Wait();

  auto surface = std::make_unique<NiceMock<MockSurface>>();
  EXPECT_CALL(*surface, AllowsDrawingWhenGpuDisabled())
      .WillRepeatedly(Return(true));
  ON_CALL(*surface, AcquireFrame(DlISize())).WillByDefault([] {
    SurfaceFrame::FramebufferInfo framebuffer_info;
    framebuffer_info.supports_readback = true;
    return std::make_unique<SurfaceFrame>(
        /*surface=*/
        nullptr, framebuffer_info,
        /*encode_callback=*/
        [](const SurfaceFrame&, DlCanvas*) { return true; },
        /*submit_callback=*/[](const SurfaceFrame& frame) { return true; },
        /*frame_size=*/DlISize(800, 600));
  });
  ON_CALL(*surface, MakeRenderContextCurrent()).WillByDefault([] {
    return std::make_unique<GLContextDefaultResult>(true);
  });

  fml::CountDownLatch count_down_latch(2);
  auto first_timestamp = fml::TimePoint::Now();
  auto second_timestamp = first_timestamp + fml::TimeDelta::FromMilliseconds(8);
  std::vector<fml::TimePoint> timestamps = {first_timestamp, second_timestamp};
  int frame_rasterized_count = 0;
  EXPECT_CALL(delegate, OnFrameRasterized(_))
      .Times(2)
      .WillRepeatedly([&](const FrameTiming& frame_timing) {
        EXPECT_EQ(timestamps[frame_rasterized_count],
                  frame_timing.Get(FrameTiming::kVsyncStart));
        EXPECT_EQ(timestamps[frame_rasterized_count],
                  frame_timing.Get(FrameTiming::kBuildStart));
        EXPECT_EQ(timestamps[frame_rasterized_count],
                  frame_timing.Get(FrameTiming::kBuildFinish));
        frame_rasterized_count++;
        count_down_latch.CountDown();
      });

  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer->Setup(std::move(surface));
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    for (int i = 0; i < 2; i++) {
      auto layer_tree = std::make_unique<LayerTree>(
          /*root_layer=*/nullptr, /*frame_size=*/DlISize());
      auto layer_tree_item = std::make_unique<FrameItem>(
          SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                              kDevicePixelRatio),
          CreateFinishedBuildRecorder(timestamps[i]));
      PipelineProduceResult result =
          pipeline->Produce().Complete(std::move(layer_tree_item));
      EXPECT_TRUE(result.success);
      EXPECT_EQ(result.is_first_item, i == 0);
    }
    // Although we only call 'Rasterizer::Draw' once, it will be called twice
    // finally because there are two items in the pipeline.
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
  });
  count_down_latch.Wait();
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer.reset();
    latch.Signal();
  });
  latch.Wait();
}

TEST(RasterizerTest, TeardownFreesResourceCache) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());

  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  auto context = GrDirectContext::MakeMock(nullptr);
  context->setResourceCacheLimit(0);

  EXPECT_CALL(*surface, MakeRenderContextCurrent())
      .WillRepeatedly([]() -> std::unique_ptr<GLContextResult> {
        return std::make_unique<GLContextDefaultResult>(true);
      });
  EXPECT_CALL(*surface, GetContext()).WillRepeatedly(Return(context.get()));

  rasterizer->Setup(std::move(surface));
  EXPECT_EQ(context->getResourceCacheLimit(), 0ul);

  rasterizer->SetResourceCacheMaxBytes(10000000, false);
  EXPECT_EQ(context->getResourceCacheLimit(), 10000000ul);
  EXPECT_EQ(context->getResourceCachePurgeableBytes(), 0ul);

  int count = 0;
  size_t bytes = 0;
  context->getResourceCacheUsage(&count, &bytes);
  EXPECT_EQ(bytes, 0ul);

  auto image_info =
      SkImageInfo::MakeN32Premul(500, 500, SkColorSpace::MakeSRGB());
  auto sk_surface = SkSurfaces::RenderTarget(context.get(),
                                             skgpu::Budgeted::kYes, image_info);
  EXPECT_TRUE(sk_surface);

  SkPaint paint;
  sk_surface->getCanvas()->drawPaint(paint);
  context->flushAndSubmit(GrSyncCpu::kYes);

  EXPECT_EQ(context->getResourceCachePurgeableBytes(), 0ul);

  sk_surface.reset();

  context->getResourceCacheUsage(&count, &bytes);
  EXPECT_GT(bytes, 0ul);
  EXPECT_GT(context->getResourceCachePurgeableBytes(), 0ul);

  rasterizer->Teardown();
  EXPECT_EQ(context->getResourceCachePurgeableBytes(), 0ul);
}

TEST(RasterizerTest, TeardownNoSurface) {
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform |
                             ThreadHost::Type::kRaster | ThreadHost::Type::kIo |
                             ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());

  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  EXPECT_CALL(delegate, GetTaskRunners())
      .WillRepeatedly(ReturnRef(task_runners));

  auto rasterizer = std::make_unique<Rasterizer>(delegate);

  EXPECT_TRUE(rasterizer);
  rasterizer->Teardown();
}

TEST(RasterizerTest, presentationTimeSetWhenVsyncTargetInFuture) {
  GTEST_SKIP() << "eglPresentationTime is disabled due to "
                  "https://github.com/flutter/flutter/issues/112503";
#if false
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform | ThreadHost::Type::kRaster |
                             ThreadHost::Type::kIo | ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());

  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  ON_CALL(delegate, GetTaskRunners()).WillByDefault(ReturnRef(task_runners));

  fml::AutoResetWaitableEvent latch;
  std::unique_ptr<Rasterizer> rasterizer;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer = std::make_unique<Rasterizer>(delegate);
    latch.Signal();
  });
  latch.Wait();

  const auto millis_16 = fml::TimeDelta::FromMilliseconds(16);
  const auto first_timestamp = fml::TimePoint::Now() + millis_16;
  auto second_timestamp = first_timestamp + millis_16;
  std::vector<fml::TimePoint> timestamps = {first_timestamp, second_timestamp};

  int frames_submitted = 0;
  fml::CountDownLatch submit_latch(2);
  auto surface = std::make_unique<MockSurface>();
  ON_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillByDefault(Return(true));
  ON_CALL(*surface, AcquireFrame(DlISize()))
      .WillByDefault([&] {
        SurfaceFrame::FramebufferInfo framebuffer_info;
        framebuffer_info.supports_readback = true;
        return std::make_unique<SurfaceFrame>(
            /*surface=*/nullptr, framebuffer_info,
            /*submit_callback=*/
            [&](const SurfaceFrame& frame, DlCanvas*) {
              const auto pres_time = *frame.submit_info().presentation_time;
              const auto diff = pres_time - first_timestamp;
              int num_frames_submitted = frames_submitted++;
              EXPECT_EQ(diff.ToMilliseconds(),
                        num_frames_submitted * millis_16.ToMilliseconds());
              submit_latch.CountDown();
              return true;
            },
            /*frame_size=*/DlISize(800, 600));
      });

  ON_CALL(*surface, MakeRenderContextCurrent())
      .WillByDefault(
          [] { return std::make_unique<GLContextDefaultResult>(true); });

  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer->Setup(std::move(surface));
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    for (int i = 0; i < 2; i++) {
      auto layer_tree = std::make_unique<LayerTree>(
          /*root_layer=*/nullptr, /*frame_size=*/DlISize());
      auto layer_tree_item = std::make_unique<FrameItem>(
          SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                             kDevicePixelRatio),
          CreateFinishedBuildRecorder(timestamps[i]));
      PipelineProduceResult result =
          pipeline->Produce().Complete(std::move(layer_tree_item));
      EXPECT_TRUE(result.success);
      EXPECT_EQ(result.is_first_item, i == 0);
    }
    // Although we only call 'Rasterizer::Draw' once, it will be called twice
    // finally because there are two items in the pipeline.
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
  });

  submit_latch.Wait();
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer.reset();
    latch.Signal();
  });
  latch.Wait();
#endif  // false
}

TEST(RasterizerTest, presentationTimeNotSetWhenVsyncTargetInPast) {
  GTEST_SKIP() << "eglPresentationTime is disabled due to "
                  "https://github.com/flutter/flutter/issues/112503";
#if false
  std::string test_name =
      ::testing::UnitTest::GetInstance()->current_test_info()->name();
  ThreadHost thread_host("io.flutter.test." + test_name + ".",
                         ThreadHost::Type::kPlatform | ThreadHost::Type::kRaster |
                             ThreadHost::Type::kIo | ThreadHost::Type::kUi);
  TaskRunners task_runners("test", thread_host.platform_thread->GetTaskRunner(),
                           thread_host.raster_thread->GetTaskRunner(),
                           thread_host.ui_thread->GetTaskRunner(),
                           thread_host.io_thread->GetTaskRunner());

  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  ON_CALL(delegate, GetTaskRunners()).WillByDefault(ReturnRef(task_runners));

  fml::AutoResetWaitableEvent latch;
  std::unique_ptr<Rasterizer> rasterizer;
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer = std::make_unique<Rasterizer>(delegate);
    latch.Signal();
  });
  latch.Wait();

  const auto millis_16 = fml::TimeDelta::FromMilliseconds(16);
  const auto first_timestamp = fml::TimePoint::Now() - millis_16;

  fml::CountDownLatch submit_latch(1);
  auto surface = std::make_unique<MockSurface>();
  ON_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillByDefault(Return(true));
  ON_CALL(*surface, AcquireFrame(DlISize()))
      .WillByDefault([&] {
        SurfaceFrame::FramebufferInfo framebuffer_info;
        framebuffer_info.supports_readback = true;
        return std::make_unique<SurfaceFrame>(
            /*surface=*/nullptr, framebuffer_info,
            /*submit_callback=*/
            [&](const SurfaceFrame& frame, DlCanvas*) {
              const std::optional<fml::TimePoint> pres_time =
                  frame.submit_info().presentation_time;
              EXPECT_EQ(pres_time, std::nullopt);
              submit_latch.CountDown();
              return true;
            },
            /*frame_size=*/DlISize(800, 600));
      });

  ON_CALL(*surface, MakeRenderContextCurrent())
      .WillByDefault(
          [] { return std::make_unique<GLContextDefaultResult>(true); });

  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer->Setup(std::move(surface));
    auto pipeline = std::make_shared<FramePipeline>(/*depth=*/10);
    auto layer_tree = std::make_unique<LayerTree>(
        /*root_layer=*/nullptr, /*frame_size=*/DlISize());
    auto layer_tree_item = std::make_unique<FrameItem>(
        SingleLayerTreeList(kImplicitViewId, std::move(layer_tree),
                           kDevicePixelRatio),
        CreateFinishedBuildRecorder(first_timestamp));
    PipelineProduceResult result =
        pipeline->Produce().Complete(std::move(layer_tree_item));
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.is_first_item, true);
    ON_CALL(delegate, ShouldDiscardLayerTree).WillByDefault(Return(false));
    rasterizer->Draw(pipeline);
  });

  submit_latch.Wait();
  thread_host.raster_thread->GetTaskRunner()->PostTask([&] {
    rasterizer.reset();
    latch.Signal();
  });
  latch.Wait();
#endif  // false
}

}  // namespace flutter
