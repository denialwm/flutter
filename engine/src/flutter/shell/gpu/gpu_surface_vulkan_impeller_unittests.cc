// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vulkan/vulkan.h>

#include <vector>

#include "flutter/shell/gpu/gpu_surface_vulkan_delegate.h"
#include "flutter/shell/gpu/gpu_surface_vulkan_impeller.h"
#include "flutter/testing/test_vulkan_context.h"
#include "flutter/testing/test_vulkan_surface.h"
#include "gtest/gtest.h"
#include "impeller/entity/vk/entity_shaders_vk.h"
#include "impeller/entity/vk/framebuffer_blend_shaders_vk.h"
#include "impeller/entity/vk/modern_shaders_vk.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"

namespace flutter {
namespace testing {

std::vector<std::shared_ptr<fml::Mapping>> ShaderLibraryMappings() {
  return {
      std::make_shared<fml::NonOwnedMapping>(impeller_entity_shaders_vk_data,
                                             impeller_entity_shaders_vk_length),
      std::make_shared<fml::NonOwnedMapping>(impeller_modern_shaders_vk_data,
                                             impeller_modern_shaders_vk_length),
      std::make_shared<fml::NonOwnedMapping>(
          impeller_framebuffer_blend_shaders_vk_data,
          impeller_framebuffer_blend_shaders_vk_length),
  };
}

class TestGPUSurfaceVulkanDelegate : public GPUSurfaceVulkanDelegate {
 public:
  struct FrameEvent {
    FlutterVulkanFrameStatus status;
    bool has_image;
    bool has_release_fence;
  };

  TestGPUSurfaceVulkanDelegate()
      : vk_(fml::MakeRefCounted<vulkan::VulkanProcTable>(
            vkGetInstanceProcAddr)),
        test_context_(fml::MakeRefCounted<TestVulkanContext>()),
        test_surface_(TestVulkanSurface::Create(*test_context_, {100, 100})) {}

  const vulkan::VulkanProcTable& vk() override { return *vk_; }

  FlutterVulkanImage AcquireImage(const DlISize& size) override {
    if (!image_available_) {
      return {};
    }
    return {
        .struct_size = sizeof(FlutterVulkanImage),
        .image = reinterpret_cast<uint64_t>(test_surface_->GetImage()),
        .format = VK_FORMAT_R8G8B8A8_UNORM,
    };
  }

  bool AcquireFrameImage(const DlISize& size,
                         FlutterVulkanFrameImage* image) override {
    if (!image_available_) {
      return false;
    }
    *image = {
        .struct_size = sizeof(FlutterVulkanFrameImage),
        .image = reinterpret_cast<uint64_t>(test_surface_->GetImage()),
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .external_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
    };
    return true;
  }

  bool PresentImage(VkImage image, VkFormat format) override {
    present_count_++;
    return true;
  }

  bool SupportsVulkanFrameCallback() const override {
    return supports_frame_callback_;
  }

  bool OnVulkanFrame(FlutterVulkanFrameStatus status,
                     const FlutterVulkanFrameImage* image,
                     fml::UniqueFD release_fence) override {
    frame_events_.push_back(
        {status, image != nullptr, release_fence.is_valid()});
    return frame_callback_result_;
  }

  void SetImageAvailable(bool available) { image_available_ = available; }

  void SetSupportsFrameCallback(bool supports) {
    supports_frame_callback_ = supports;
  }

  void SetFrameCallbackResult(bool result) { frame_callback_result_ = result; }

  size_t GetPresentCount() const { return present_count_; }

  const std::vector<FrameEvent>& GetFrameEvents() const {
    return frame_events_;
  }

 private:
  fml::RefPtr<vulkan::VulkanProcTable> vk_;
  fml::RefPtr<TestVulkanContext> test_context_;
  std::unique_ptr<TestVulkanSurface> test_surface_;
  bool image_available_ = true;
  bool supports_frame_callback_ = false;
  bool frame_callback_result_ = true;
  size_t present_count_ = 0u;
  std::vector<FrameEvent> frame_events_;
};

TEST(GPUSurfaceVulkanImpeller, DisposesThreadLocalResources) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));

  TestGPUSurfaceVulkanDelegate delegate;

  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  // Add a command pool to the global map.
  auto pool = context->GetCommandPoolRecycler()->Get();
  EXPECT_EQ(impeller::CommandPoolRecyclerVK::GetGlobalPoolCount(*context), 1);

  // Check that AcquireFrame disposes thread local resources and removes
  // the pool from the global map.
  auto frame = surface->AcquireFrame(DlISize(100, 100));
  EXPECT_EQ(impeller::CommandPoolRecyclerVK::GetGlobalPoolCount(*context), 0);
}

TEST(GPUSurfaceVulkanImpeller, ExplicitBackpressureCompletesSkippedFrame) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));

  TestGPUSurfaceVulkanDelegate delegate;
  delegate.SetImageAvailable(false);
  delegate.SetSupportsFrameCallback(true);
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  auto frame = surface->AcquireFrame(DlISize(100, 100));
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->Submit());
  ASSERT_EQ(delegate.GetFrameEvents().size(), 1u);
  EXPECT_EQ(delegate.GetFrameEvents()[0].status, kFlutterVulkanFrameSkipped);
  EXPECT_FALSE(delegate.GetFrameEvents()[0].has_image);
  EXPECT_FALSE(delegate.GetFrameEvents()[0].has_release_fence);

  frame.reset();
  EXPECT_EQ(delegate.GetFrameEvents().size(), 1u);
}

TEST(GPUSurfaceVulkanImpeller, DroppedBackpressureFrameCompletesOnce) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));

  TestGPUSurfaceVulkanDelegate delegate;
  delegate.SetImageAvailable(false);
  delegate.SetSupportsFrameCallback(true);
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  auto frame = surface->AcquireFrame(DlISize(100, 100));
  ASSERT_NE(frame, nullptr);
  frame.reset();
  ASSERT_EQ(delegate.GetFrameEvents().size(), 1u);
  EXPECT_EQ(delegate.GetFrameEvents()[0].status, kFlutterVulkanFrameSkipped);
  EXPECT_FALSE(delegate.GetFrameEvents()[0].has_image);
}

TEST(GPUSurfaceVulkanImpeller, NoSurfaceFrameCompletesSkipped) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));

  TestGPUSurfaceVulkanDelegate delegate;
  delegate.SetSupportsFrameCallback(true);
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  surface->NotifyFrameSkipped();
  ASSERT_EQ(delegate.GetFrameEvents().size(), 1u);
  EXPECT_EQ(delegate.GetFrameEvents()[0].status, kFlutterVulkanFrameSkipped);
  EXPECT_FALSE(delegate.GetFrameEvents()[0].has_image);
  EXPECT_FALSE(delegate.GetFrameEvents()[0].has_release_fence);
}

TEST(GPUSurfaceVulkanImpeller, LegacyZeroImageRemainsAnError) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));

  TestGPUSurfaceVulkanDelegate delegate;
  delegate.SetImageAvailable(false);
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  EXPECT_EQ(surface->AcquireFrame(DlISize(100, 100)), nullptr);
  EXPECT_TRUE(delegate.GetFrameEvents().empty());
}

TEST(GPUSurfaceVulkanImpeller, AcquiredImageIsCancelledWhenFrameIsDropped) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));

  TestGPUSurfaceVulkanDelegate delegate;
  delegate.SetSupportsFrameCallback(true);
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  auto frame = surface->AcquireFrame(DlISize(100, 100));
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(delegate.GetFrameEvents().empty());

  frame.reset();
  ASSERT_EQ(delegate.GetFrameEvents().size(), 1u);
  EXPECT_EQ(delegate.GetFrameEvents()[0].status, kFlutterVulkanFrameCancelled);
  EXPECT_TRUE(delegate.GetFrameEvents()[0].has_image);
  EXPECT_FALSE(delegate.GetFrameEvents()[0].has_release_fence);
}

TEST(GPUSurfaceVulkanImpeller, EncodedImageWaitsThenCancelsWhenDropped) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));
  if (!context->SupportsFrameFence()) {
    GTEST_SKIP() << "Vulkan sync-file export is unavailable.";
  }

  TestGPUSurfaceVulkanDelegate delegate;
  delegate.SetSupportsFrameCallback(true);
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  auto frame = surface->AcquireFrame(DlISize(100, 100));
  ASSERT_NE(frame, nullptr);
  ASSERT_TRUE(frame->Encode());
  frame.reset();
  ASSERT_EQ(delegate.GetFrameEvents().size(), 1u);
  EXPECT_EQ(delegate.GetFrameEvents()[0].status, kFlutterVulkanFrameCancelled);
  EXPECT_TRUE(delegate.GetFrameEvents()[0].has_image);
  EXPECT_FALSE(delegate.GetFrameEvents()[0].has_release_fence);
}

TEST(GPUSurfaceVulkanImpeller, ReadyFrameCompletesOnce) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));
  if (!context->SupportsFrameFence()) {
    GTEST_SKIP() << "Vulkan sync-file export is unavailable.";
  }

  TestGPUSurfaceVulkanDelegate delegate;
  delegate.SetSupportsFrameCallback(true);
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  auto frame = surface->AcquireFrame(DlISize(100, 100));
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->Submit());
  ASSERT_EQ(delegate.GetFrameEvents().size(), 1u);
  EXPECT_EQ(delegate.GetFrameEvents()[0].status, kFlutterVulkanFrameReady);
  EXPECT_TRUE(delegate.GetFrameEvents()[0].has_image);
  frame.reset();
  EXPECT_EQ(delegate.GetFrameEvents().size(), 1u);
  EXPECT_EQ(delegate.GetPresentCount(), 0u);
}

TEST(GPUSurfaceVulkanImpeller, RejectedReadyFrameIsStillTerminal) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));
  if (!context->SupportsFrameFence()) {
    GTEST_SKIP() << "Vulkan sync-file export is unavailable.";
  }

  TestGPUSurfaceVulkanDelegate delegate;
  delegate.SetSupportsFrameCallback(true);
  delegate.SetFrameCallbackResult(false);
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  auto frame = surface->AcquireFrame(DlISize(100, 100));
  ASSERT_NE(frame, nullptr);
  EXPECT_FALSE(frame->Submit());
  ASSERT_EQ(delegate.GetFrameEvents().size(), 1u);
  EXPECT_EQ(delegate.GetFrameEvents()[0].status, kFlutterVulkanFrameReady);
  frame.reset();
  EXPECT_EQ(delegate.GetFrameEvents().size(), 1u);
}

TEST(GPUSurfaceVulkanImpeller, LegacyFrameStillUsesPresentCallback) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));

  TestGPUSurfaceVulkanDelegate delegate;
  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  auto frame = surface->AcquireFrame(DlISize(100, 100));
  ASSERT_NE(frame, nullptr);
  EXPECT_TRUE(frame->Submit());
  EXPECT_EQ(delegate.GetPresentCount(), 1u);
  EXPECT_TRUE(delegate.GetFrameEvents().empty());
}

}  // namespace testing
}  // namespace flutter
