// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#define FML_USED_ON_EMBEDDER
#include "flutter/flow/layers/clip_rect_layer.h"
#include "flutter/fml/message_loop.h"
#include "flutter/shell/common/animator.h"
#include "flutter/shell/common/rasterizer.h"
#include "flutter/shell/common/thread_host.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

namespace flutter {
namespace {
std::vector<std::unique_ptr<LayerTreeTask>> SingleLayerTreeList(
    int64_t view_id,
    std::unique_ptr<LayerTree> tree,
    float ratio) {
  std::vector<std::unique_ptr<LayerTreeTask>> tasks;
  tasks.push_back(
      std::make_unique<LayerTreeTask>(view_id, std::move(tree), ratio));
  return tasks;
}

static std::unique_ptr<FrameTimingsRecorder> CreateFinishedBuildRecorder() {
  auto recorder = std::make_unique<FrameTimingsRecorder>();
  const auto now = fml::TimePoint::Now();
  recorder->RecordVsync(now, now);
  recorder->RecordBuildStart(now);
  recorder->RecordBuildEnd(now);
  return recorder;
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
  MOCK_METHOD(std::optional<PreparedRenderTarget>,
              PrepareDenialRenderTarget,
              (int64_t,
               GrDirectContext*,
               const std::shared_ptr<impeller::AiksContext>&),
              (override));
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

class ImmediateVsyncWaiter : public VsyncWaiter {
 public:
  explicit ImmediateVsyncWaiter(const TaskRunners& runners)
      : VsyncWaiter(runners) {}

 private:
  void AwaitVSync() override {
    const auto now = fml::TimePoint::Now();
    FireCallback(now, now + fml::TimeDelta::FromMilliseconds(16), false);
  }
};

class EmptyAnimatorDelegate : public Animator::Delegate {
 public:
  MOCK_METHOD(void,
              OnAnimatorBeginFrame,
              (fml::TimePoint, uint64_t),
              (override));
  MOCK_METHOD(void,
              OnAnimatorDraw,
              (std::shared_ptr<FramePipeline>),
              (override));
  void OnAnimatorNotifyIdle(fml::TimeDelta) override {}
  void OnAnimatorUpdateLatestFrameTargetTime(fml::TimePoint) override {}
  void OnAnimatorDrawLastLayerTrees(
      std::unique_ptr<FrameTimingsRecorder>) override {}
};
}  // namespace

class RasterizerTestPeer {
 public:
  static const LayerTreeTask* PendingRenderOutput(const Rasterizer& rasterizer,
                                                  int64_t id) {
    auto found = rasterizer.denial_pending_output_tasks_.find(id);
    return found == rasterizer.denial_pending_output_tasks_.end()
               ? nullptr
               : found->second.get();
  }
};

TEST(RasterizerTest, DenialRejectedFramesRecoverWithoutAnotherFrameworkScene) {
  ThreadHost threads("denial.recovery.",
                     ThreadHost::Type::kPlatform | ThreadHost::Type::kRaster |
                         ThreadHost::Type::kIo | ThreadHost::Type::kUi);
  TaskRunners runners("test", threads.platform_thread->GetTaskRunner(),
                      threads.raster_thread->GetTaskRunner(),
                      threads.ui_thread->GetTaskRunner(),
                      threads.io_thread->GetTaskRunner());
  NiceMock<MockDelegate> delegate;
  Settings settings;
  ON_CALL(delegate, GetSettings()).WillByDefault(ReturnRef(settings));
  ON_CALL(delegate, GetTaskRunners()).WillByDefault(ReturnRef(runners));
  Rasterizer rasterizer(delegate);
  auto surface = std::make_unique<NiceMock<MockSurface>>();
  ON_CALL(*surface, MakeRenderContextCurrent()).WillByDefault([] {
    return std::make_unique<GLContextDefaultResult>(true);
  });
  ON_CALL(*surface, AllowsDrawingWhenGpuDisabled()).WillByDefault(Return(true));
  ON_CALL(*surface, AcquireFrame).WillByDefault([](const DlISize& size) {
    return std::make_unique<SurfaceFrame>(
        nullptr, SurfaceFrame::FramebufferInfo{},
        [](const SurfaceFrame&, DlCanvas*) { return true; },
        [](const SurfaceFrame&) { return true; }, size);
  });
  auto embedder = std::make_shared<NiceMock<MockExternalViewEmbedder>>();
  int attempts = 0;
  ON_CALL(*embedder, PrepareDenialRenderTarget)
      .WillByDefault(
          [&](int64_t, GrDirectContext*,
              const std::shared_ptr<impeller::AiksContext>&)
              -> std::optional<ExternalViewEmbedder::PreparedRenderTarget> {
            if (++attempts % 2 == 1) {
              return std::nullopt;
            }
            return ExternalViewEmbedder::PreparedRenderTarget{.existing_damage =
                                                                  DlRegion()};
          });
  rasterizer.SetExternalViewEmbedder(embedder);
  rasterizer.Setup(std::move(surface));
  fml::AutoResetWaitableEvent done;
  runners.GetRasterTaskRunner()->PostTask([&] {
    rasterizer.SetDenialRenderOutputs({{
        .render_view_id = -2,
        .configuration_generation = 41,
        .source_physical_bounds = DlRect::MakeWH(64, 64),
        .target_size = DlISize(64, 64),
        .scale_120 = 120,
        .source_to_target_transform = DlMatrix(),
    }});
    auto pipeline = std::make_shared<FramePipeline>(2);
    const auto submitted =
        pipeline->Produce().Complete(std::make_unique<FrameItem>(
            SingleLayerTreeList(
                0,
                std::make_unique<LayerTree>(std::make_shared<ContainerLayer>(),
                                            DlISize(64, 64)),
                1.0f),
            CreateFinishedBuildRecorder()));
    EXPECT_TRUE(submitted.success);
    rasterizer.PrepareDenialRenderOutputs({-2}, {});
    rasterizer.Draw(pipeline);  // A newly built scene fails target acquisition.
    EXPECT_EQ(attempts, 1);
    EXPECT_NE(RasterizerTestPeer::PendingRenderOutput(rasterizer, -2), nullptr);
    for (int i = 0; i < 3; ++i) {
      if (i == 1) {
        // Cached-only framework work uses a tagged empty pipeline item too.
        auto item = std::make_unique<FrameItem>(
            std::vector<std::unique_ptr<LayerTreeTask>>{},
            CreateFinishedBuildRecorder());
        item->denial_transaction = std::make_shared<DenialRenderTransaction>();
        item->denial_transaction->render_view_ids = {-2};
        item->denial_transaction->on_begin = [&] { EXPECT_EQ(attempts, 2); };
        item->denial_transaction->on_complete = [&] { EXPECT_EQ(attempts, 3); };
        const auto queued = pipeline->Produce().Complete(std::move(item));
        EXPECT_TRUE(queued.success);
        rasterizer.Draw(pipeline);
      } else {
        rasterizer.DrawDenialRenderOutputs({-2}, {},
                                           CreateFinishedBuildRecorder());
      }
      if (i % 2 == 0) {
        EXPECT_NE(rasterizer.GetLastLayerTree(-2), nullptr);
        EXPECT_EQ(RasterizerTestPeer::PendingRenderOutput(rasterizer, -2),
                  nullptr);
      } else {
        EXPECT_NE(RasterizerTestPeer::PendingRenderOutput(rasterizer, -2),
                  nullptr);
      }
    }
    EXPECT_EQ(attempts,
              4);  // A retained-scene failure also recovers autonomously.
    done.Signal();
  });
  done.Wait();
}

TEST(AnimatorTest, DenialNoRasterVsyncCompletesItsReservation) {
  for (bool secondary_only : {false, true}) {
    SCOPED_TRACE(secondary_only ? "secondary-only vsync"
                                : "empty framework frame");
    ThreadHost threads("denial.empty.",
                       ThreadHost::Type::kPlatform | ThreadHost::Type::kRaster |
                           ThreadHost::Type::kIo | ThreadHost::Type::kUi);
    TaskRunners runners("test", threads.platform_thread->GetTaskRunner(),
                        threads.raster_thread->GetTaskRunner(),
                        threads.ui_thread->GetTaskRunner(),
                        threads.io_thread->GetTaskRunner());
    NiceMock<EmptyAnimatorDelegate> delegate;
    fml::AutoResetWaitableEvent completed;
    fml::AutoResetWaitableEvent destroyed;
    std::unique_ptr<Animator> animator;
    EXPECT_CALL(delegate, OnAnimatorDraw).Times(0);
    if (secondary_only) {
      EXPECT_CALL(delegate, OnAnimatorBeginFrame).Times(0);
    } else {
      EXPECT_CALL(delegate, OnAnimatorBeginFrame).WillOnce([&] {
        EXPECT_FALSE(completed.IsSignaledForTest());
      });
    }
    runners.GetUITaskRunner()->PostTask([&] {
      animator = std::make_unique<Animator>(
          delegate, runners, std::make_unique<ImmediateVsyncWaiter>(runners));
      // Secondary callbacks can request an embedder vsync without requesting
      // an Animator frame. Native admission then delivers a reservation too.
      if (secondary_only) {
        animator->ScheduleSecondaryVsyncCallback(
            1, [&] { EXPECT_FALSE(completed.IsSignaledForTest()); });
      }
      auto transaction = std::make_shared<DenialRenderTransaction>();
      transaction->render_view_ids = {-2, -3};
      transaction->on_begin = [] {
        ADD_FAILURE() << "No-raster vsync began raster work";
      };
      transaction->on_complete = [&] { completed.Signal(); };
      animator->SetDenialRenderTransaction(std::move(transaction));
      if (!secondary_only) {
        animator->RequestFrame();
      }
    });
    EXPECT_FALSE(
        completed.WaitWithTimeout(fml::TimeDelta::FromMilliseconds(500)));
    runners.GetUITaskRunner()->PostTask([&] {
      animator.reset();
      destroyed.Signal();
    });
    destroyed.Wait();
  }
}

}  // namespace flutter

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  fml::MessageLoop::EnsureInitializedForCurrentThread();
  return RUN_ALL_TESTS();
}
