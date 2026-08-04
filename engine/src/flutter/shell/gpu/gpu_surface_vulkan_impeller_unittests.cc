// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vulkan/vulkan.h>

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
  explicit TestGPUSurfaceVulkanDelegate(bool persistent = false)
      : persistent_(persistent),
        vk_(fml::MakeRefCounted<vulkan::VulkanProcTable>(
            vkGetInstanceProcAddr)),
        test_context_(fml::MakeRefCounted<TestVulkanContext>()),
        test_surface_(TestVulkanSurface::Create(*test_context_, {100, 100})) {}

  const vulkan::VulkanProcTable& vk() override { return *vk_; }

  FlutterVulkanImage AcquireImage(const DlISize& size) override {
    return {
        .struct_size = sizeof(FlutterVulkanImage),
        .image = reinterpret_cast<uint64_t>(test_surface_->GetImage()),
        .format = VK_FORMAT_R8G8B8A8_UNORM,
    };
  }

  bool PresentImage(VkImage image, VkFormat format) override { return true; }

  bool SupportsBorrowedImages() const override { return persistent_; }

  bool AcquireImage2(const DlISize&, FlutterVulkanImage2* image) override {
    if (!persistent_) {
      return false;
    }
    *image = {
        .struct_size = sizeof(FlutterVulkanImage2),
        .image = reinterpret_cast<uint64_t>(test_surface_->GetImage()),
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .layout = VK_IMAGE_LAYOUT_GENERAL,
        .external_queue_family_index = VK_QUEUE_FAMILY_IGNORED,
        .flags = kFlutterVulkanImageFlagPersistent,
    };
    return true;
  }

  bool PresentImage2(const FlutterVulkanImage2&, fml::UniqueFD) override {
    return true;
  }

 private:
  bool persistent_;
  fml::RefPtr<vulkan::VulkanProcTable> vk_;
  fml::RefPtr<TestVulkanContext> test_context_;
  std::unique_ptr<TestVulkanSurface> test_surface_;
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

TEST(GPUSurfaceVulkanImpeller,
     PreservesThreadLocalResourcesForPersistentBorrowedImages) {
  impeller::ContextVK::Settings context_settings;
  context_settings.proc_address_callback = vkGetInstanceProcAddr;
  context_settings.shader_libraries_data = ShaderLibraryMappings();
  auto context = impeller::ContextVK::Create(std::move(context_settings));

  TestGPUSurfaceVulkanDelegate delegate(/*persistent=*/true);

  std::unique_ptr<Surface> surface =
      std::make_unique<GPUSurfaceVulkanImpeller>(&delegate, context);

  auto pool = context->GetCommandPoolRecycler()->Get();
  EXPECT_EQ(impeller::CommandPoolRecyclerVK::GetGlobalPoolCount(*context), 1);

  auto first_frame = surface->AcquireFrame(DlISize(100, 100));
  EXPECT_TRUE(first_frame);
  EXPECT_EQ(impeller::CommandPoolRecyclerVK::GetGlobalPoolCount(*context), 0);
  first_frame.reset();

  auto persistent_pool = context->GetCommandPoolRecycler()->Get();
  EXPECT_EQ(impeller::CommandPoolRecyclerVK::GetGlobalPoolCount(*context), 1);
  auto next_frame = surface->AcquireFrame(DlISize(100, 100));
  EXPECT_TRUE(next_frame);
  EXPECT_EQ(impeller::CommandPoolRecyclerVK::GetGlobalPoolCount(*context), 1);
}

}  // namespace testing
}  // namespace flutter
