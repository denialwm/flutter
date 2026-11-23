// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/testing/testing.h"  // IWYU pragma: keep
#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/renderer/backend/vulkan/command_buffer_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_builder_vk.h"
#include "impeller/renderer/backend/vulkan/render_pass_vk.h"
#include "impeller/renderer/backend/vulkan/test/mock_vulkan.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"
#include "impeller/renderer/render_target.h"
#include "vulkan/vulkan_enums.hpp"

namespace impeller {
namespace testing {

TEST(RenderPassVK, DoesNotRedundantlySetStencil) {
  std::shared_ptr<ContextVK> context = MockVulkanContextBuilder().Build();
  std::shared_ptr<Context> copy = context;
  auto cmd_buffer = context->CreateCommandBuffer();

  RenderTargetAllocator allocator(context->GetResourceAllocator());
  RenderTarget target = allocator.CreateOffscreenMSAA(*copy.get(), {1, 1}, 1);

  std::shared_ptr<RenderPass> render_pass =
      cmd_buffer->CreateRenderPass(target);

  // Stencil reference set once at buffer start.
  auto called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            1);

  // Duplicate stencil ref is not replaced.
  render_pass->SetStencilReference(0);
  render_pass->SetStencilReference(0);
  render_pass->SetStencilReference(0);

  called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            1);

  // Different stencil value is updated.
  render_pass->SetStencilReference(1);
  called_functions = GetMockVulkanFunctions(context->GetDevice());
  EXPECT_EQ(std::count(called_functions->begin(), called_functions->end(),
                       "vkCmdSetStencilReference"),
            2);
}

TEST(RenderPassVK, CachedPassHonorsColorLoadAndClearTransitions) {
  auto context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto target = allocator.CreateOffscreen(*context, {100, 100}, 1);
  auto& texture = TextureVK::Cast(*target.GetRenderTargetTexture());
  auto first_buffer = context->CreateCommandBuffer();
  auto first = first_buffer->CreateRenderPass(target);
  ASSERT_TRUE(first->EncodeCommands());
  const auto clear = texture.GetCachedFrameData(SampleCount::kCount1);

  auto color = target.GetColorAttachment(0);
  color.load_action = LoadAction::kLoad;
  target.SetColorAttachment(color, 0);
  auto second_buffer = context->CreateCommandBuffer();
  auto second = second_buffer->CreateRenderPass(target);
  ASSERT_TRUE(second->EncodeCommands());
  const auto load = texture.GetCachedFrameData(SampleCount::kCount1);
  EXPECT_NE(clear.render_pass, load.render_pass);

  auto third_buffer = context->CreateCommandBuffer();
  auto third = third_buffer->CreateRenderPass(target);
  ASSERT_TRUE(third->EncodeCommands());
  const auto repeated_load = texture.GetCachedFrameData(SampleCount::kCount1);
  EXPECT_EQ(load.render_pass, repeated_load.render_pass);
  EXPECT_EQ(load.framebuffer, repeated_load.framebuffer);

  color.load_action = LoadAction::kClear;
  target.SetColorAttachment(color, 0);
  auto fourth_buffer = context->CreateCommandBuffer();
  auto fourth = fourth_buffer->CreateRenderPass(target);
  ASSERT_TRUE(fourth->EncodeCommands());
  EXPECT_NE(load.render_pass,
            texture.GetCachedFrameData(SampleCount::kCount1).render_pass);
}

TEST(RenderPassVK, CachedPassHonorsPreservedDepthStencil) {
  auto context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto target = allocator.CreateOffscreen(
      *context, {100, 100}, 1, "Preserved depth/stencil",
      RenderTarget::kDefaultColorAttachmentConfig,
      RenderTarget::AttachmentConfig{
          .storage_mode = StorageMode::kDevicePrivate,
          .load_action = LoadAction::kClear,
          .store_action = StoreAction::kStore,
          .clear_color = Color::BlackTransparent()});
  auto depth = target.GetDepthAttachment().value();
  auto stencil = target.GetStencilAttachment().value();
  depth.store_action = StoreAction::kStore;
  stencil.store_action = StoreAction::kStore;
  target.SetDepthAttachment(depth);
  target.SetStencilAttachment(stencil);
  auto& texture = TextureVK::Cast(*target.GetRenderTargetTexture());
  auto first_buffer = context->CreateCommandBuffer();
  auto first = first_buffer->CreateRenderPass(target);
  ASSERT_TRUE(first->EncodeCommands());
  const auto initial = texture.GetCachedFrameData(SampleCount::kCount1);

  depth.load_action = LoadAction::kLoad;
  stencil.load_action = LoadAction::kLoad;
  target.SetDepthAttachment(depth);
  target.SetStencilAttachment(stencil);
  auto second_buffer = context->CreateCommandBuffer();
  auto second = second_buffer->CreateRenderPass(target);
  ASSERT_TRUE(second->EncodeCommands());
  EXPECT_NE(initial.render_pass,
            texture.GetCachedFrameData(SampleCount::kCount1).render_pass);
}

TEST(RenderPassVK, CachedFramebufferHonorsReplacedMsaaAttachments) {
  auto context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto first_target = allocator.CreateOffscreenMSAA(*context, {100, 100}, 1);
  auto second_target = allocator.CreateOffscreenMSAA(*context, {100, 100}, 1);
  auto resolve = first_target.GetColorAttachment(0).resolve_texture;
  auto& texture = TextureVK::Cast(*resolve);
  auto first_buffer = context->CreateCommandBuffer();
  auto first = first_buffer->CreateRenderPass(first_target);
  ASSERT_TRUE(first->EncodeCommands());
  const auto initial = texture.GetCachedFrameData(SampleCount::kCount4);

  // Backdrop ping-pong reuses a resolve image with another target's MSAA
  // color and depth/stencil attachments.
  auto color = second_target.GetColorAttachment(0);
  color.resolve_texture = resolve;
  second_target.SetColorAttachment(color, 0);
  auto second_buffer = context->CreateCommandBuffer();
  auto second = second_buffer->CreateRenderPass(second_target);
  ASSERT_TRUE(second->EncodeCommands());
  const auto replaced = texture.GetCachedFrameData(SampleCount::kCount4);
  EXPECT_NE(initial.framebuffer, replaced.framebuffer);
  EXPECT_EQ(initial.render_pass, replaced.render_pass);
}

TEST(RenderPassVK, CachedFramebufferHonorsReplacedDepthStencil) {
  auto context = MockVulkanContextBuilder().Build();
  RenderTargetAllocator allocator(context->GetResourceAllocator());
  auto target = allocator.CreateOffscreen(*context, {100, 100}, 1);
  auto other = allocator.CreateOffscreen(*context, {100, 100}, 1);
  auto& texture = TextureVK::Cast(*target.GetRenderTargetTexture());
  auto first_buffer = context->CreateCommandBuffer();
  auto first = first_buffer->CreateRenderPass(target);
  ASSERT_TRUE(first->EncodeCommands());
  const auto initial = texture.GetCachedFrameData(SampleCount::kCount1);

  target.SetDepthAttachment(other.GetDepthAttachment());
  target.SetStencilAttachment(other.GetStencilAttachment());
  auto second_buffer = context->CreateCommandBuffer();
  auto second = second_buffer->CreateRenderPass(target);
  ASSERT_TRUE(second->EncodeCommands());
  const auto replaced = texture.GetCachedFrameData(SampleCount::kCount1);
  EXPECT_NE(initial.framebuffer, replaced.framebuffer);
  EXPECT_EQ(initial.render_pass, replaced.render_pass);
}

// Regression guard for the bug where `RenderPassVK::SetViewport` silently
// dropped the user's X and Y offsets and the depth range, only honoring
// width and height.
TEST(RenderPassVK, SetViewportPropagatesAllUserSuppliedFields) {
  std::shared_ptr<ContextVK> context = MockVulkanContextBuilder().Build();
  auto cmd_buffer = context->CreateCommandBuffer();

  RenderTargetAllocator allocator(context->GetResourceAllocator());
  RenderTarget target = allocator.CreateOffscreenMSAA(*context, {100, 100},
                                                      /*mip_count=*/1);

  std::shared_ptr<RenderPass> render_pass =
      cmd_buffer->CreateRenderPass(target);

  // The render pass constructor sets an initial full-target viewport. Capture
  // a reference to the recorded viewport list before the user-driven call so
  // we can isolate the call under test.
  VkCommandBuffer raw_cmd_buffer =
      CommandBufferVK::Cast(*cmd_buffer).GetCommandBuffer();
  const std::vector<VkViewport>& recorded =
      GetRecordedViewports(raw_cmd_buffer);
  ASSERT_EQ(recorded.size(), 1u);  // Initial full-target viewport.

  render_pass->SetViewport(Viewport{
      .rect = Rect::MakeXYWH(25, 10, 50, 80),
      .depth_range = DepthRange{.z_near = 0.25f, .z_far = 0.75f},
  });

  ASSERT_EQ(recorded.size(), 2u);
  const VkViewport& vp = recorded[1];
  EXPECT_FLOAT_EQ(vp.x, 25.0f);
  // Y is computed for the negative-height Y-flip: `y + height` of the
  // original rect.
  EXPECT_FLOAT_EQ(vp.y, 90.0f);
  EXPECT_FLOAT_EQ(vp.width, 50.0f);
  EXPECT_FLOAT_EQ(vp.height, -80.0f);
  EXPECT_FLOAT_EQ(vp.minDepth, 0.25f);
  EXPECT_FLOAT_EQ(vp.maxDepth, 0.75f);
}

}  // namespace testing
}  // namespace impeller
