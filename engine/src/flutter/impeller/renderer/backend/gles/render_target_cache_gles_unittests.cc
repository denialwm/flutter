// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/render_target_cache.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/testing/mocks.h"

namespace impeller::testing {
namespace {

class CountingTargetAllocator final : public Allocator {
 public:
  ISize GetMaxTextureSizeSupported() const override { return {4096, 4096}; }

  size_t allocations = 0;

 private:
  std::shared_ptr<DeviceBuffer> OnCreateBuffer(
      const DeviceBufferDescriptor&) override {
    return nullptr;
  }

  std::shared_ptr<Texture> OnCreateTexture(const TextureDescriptor& desc,
                                           bool threadsafe) override {
    allocations++;
    auto texture = std::make_shared<::testing::NiceMock<MockTexture>>(desc);
    ON_CALL(*texture, GetSize()).WillByDefault(::testing::Return(desc.size));
    ON_CALL(*texture, IsValid()).WillByDefault(::testing::Return(true));
    return texture;
  }
};

class RenderTargetRetentionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_gl_ = MockGLES::Init();
    context_ = ContextGLES::Create(
        Flags{}, std::make_unique<ProcTableGLES>(kMockResolverGLES), {}, false);
    ASSERT_TRUE(context_);
  }

  RenderTarget ColorOnly(RenderTargetCache& cache, ISize size) {
    return cache.CreateOffscreen(*context_, size, 1, "Test",
                                 RenderTarget::kDefaultColorAttachmentConfig,
                                 std::nullopt);
  }

  void Idle(RenderTargetCache& cache, size_t frames = 1) {
    for (size_t i = 0; i < frames; i++) {
      cache.Start();
      cache.End();
    }
  }

  std::shared_ptr<MockGLES> mock_gl_;
  std::shared_ptr<ContextGLES> context_;
  std::shared_ptr<CountingTargetAllocator> allocator_ =
      std::make_shared<CountingTargetAllocator>();
};

TEST_F(RenderTargetRetentionTest, ReusesAttachmentsAfterIntermittentFrames) {
  RenderTargetCache cache(allocator_);
  cache.Start();
  auto material = ColorOnly(cache, {1253, 1422});
  auto msaa = cache.CreateOffscreenMSAA(*context_, {23, 167}, 1);
  ASSERT_TRUE(material.IsValid());
  ASSERT_TRUE(msaa.IsValid());
  const auto allocation_count = allocator_->allocations;
  cache.End();

  // A slower view/effect can disappear for more than the old four-frame
  // grace period while another view continues submitting high-rate frames.
  for (size_t cycle = 0; cycle < 30; cycle++) {
    Idle(cache, 12);
    cache.Start();
    auto next_material = ColorOnly(cache, {1253, 1422});
    auto next_msaa = cache.CreateOffscreenMSAA(*context_, {23, 167}, 1);
    EXPECT_EQ(next_material.GetRenderTargetTexture(),
              material.GetRenderTargetTexture());
    EXPECT_EQ(next_msaa.GetColorAttachment(0).texture,
              msaa.GetColorAttachment(0).texture);
    EXPECT_EQ(next_msaa.GetColorAttachment(0).resolve_texture,
              msaa.GetColorAttachment(0).resolve_texture);
    ASSERT_TRUE(next_msaa.GetDepthAttachment());
    EXPECT_EQ(next_msaa.GetDepthAttachment()->texture,
              msaa.GetDepthAttachment()->texture);
    EXPECT_EQ(next_msaa.GetStencilAttachment()->texture,
              msaa.GetStencilAttachment()->texture);
    cache.End();
  }
  EXPECT_EQ(allocator_->allocations, allocation_count);
}

TEST_F(RenderTargetRetentionTest, IdleBudgetEvictsLeastRecentlyUsedTargets) {
  // Each color-only target is 40,000 bytes; exactly two fit in the idle pool.
  RenderTargetCache cache(allocator_, 0, 80000);
  cache.Start();
  std::weak_ptr<Texture> oldest =
      ColorOnly(cache, {100, 100}).GetRenderTargetTexture();
  cache.End();
  cache.Start();
  std::weak_ptr<Texture> middle =
      ColorOnly(cache, {80, 125}).GetRenderTargetTexture();
  cache.End();
  cache.Start();
  std::weak_ptr<Texture> newest =
      ColorOnly(cache, {125, 80}).GetRenderTargetTexture();
  cache.End();
  Idle(cache);
  EXPECT_TRUE(oldest.expired());
  EXPECT_FALSE(middle.expired());
  EXPECT_FALSE(newest.expired());
  EXPECT_EQ(cache.CachedTextureCount(), 2u);

  cache.Start();
  EXPECT_EQ(ColorOnly(cache, {80, 125}).GetRenderTargetTexture(),
            middle.lock());
  EXPECT_EQ(ColorOnly(cache, {125, 80}).GetRenderTargetTexture(),
            newest.lock());
  cache.End();
  EXPECT_EQ(allocator_->allocations, 3u);
}

TEST_F(RenderTargetRetentionTest, ReuseRefreshesIdlePriority) {
  RenderTargetCache cache(allocator_, 0, 80000);
  cache.Start();
  std::weak_ptr<Texture> first =
      ColorOnly(cache, {100, 100}).GetRenderTargetTexture();
  cache.End();
  cache.Start();
  std::weak_ptr<Texture> second =
      ColorOnly(cache, {80, 125}).GetRenderTargetTexture();
  cache.End();
  cache.Start();
  EXPECT_EQ(ColorOnly(cache, {100, 100}).GetRenderTargetTexture(),
            first.lock());
  cache.End();
  cache.Start();
  ColorOnly(cache, {125, 80});
  cache.End();
  Idle(cache);
  EXPECT_FALSE(first.expired());
  EXPECT_TRUE(second.expired());
}

TEST_F(RenderTargetRetentionTest, OversizedIdleTargetIsReleased) {
  RenderTargetCache cache(allocator_, 0, 39999);
  cache.Start();
  std::weak_ptr<Texture> target =
      ColorOnly(cache, {100, 100}).GetRenderTargetTexture();
  cache.End();
  EXPECT_FALSE(target.expired());  // The active frame is not evicted.
  Idle(cache);
  EXPECT_TRUE(target.expired());
  EXPECT_EQ(cache.CachedTextureCount(), 0u);
}

TEST_F(RenderTargetRetentionTest,
       BudgetCountsSamplesAndSharedDepthStencilOnce) {
  // 100x100 RGBA8: four color samples, one resolve sample, and four D24S8
  // samples. Depth and stencil share the same allocation.
  constexpr size_t kAttachmentBytes = 100 * 100 * 4 * (4 + 1 + 4);
  for (size_t budget : {kAttachmentBytes - 1, kAttachmentBytes}) {
    RenderTargetCache cache(allocator_, 0, budget);
    cache.Start();
    {
      auto target = cache.CreateOffscreenMSAA(*context_, {100, 100}, 1);
      ASSERT_TRUE(target.IsValid());
      EXPECT_EQ(target.GetDepthAttachment()->texture,
                target.GetStencilAttachment()->texture);
    }
    cache.End();
    Idle(cache);
    EXPECT_EQ(cache.CachedTextureCount(), budget == kAttachmentBytes ? 1u : 0u);
  }
}

TEST_F(RenderTargetRetentionTest, BudgetIncludesMipLevels) {
  constexpr size_t kAttachmentBytes = (8 * 8 + 4 * 4 + 2 * 2) * 4;
  for (size_t budget : {kAttachmentBytes - 1, kAttachmentBytes}) {
    RenderTargetCache cache(allocator_, 0, budget);
    cache.Start();
    cache.CreateOffscreen(*context_, {8, 8}, 3, "Mipmapped",
                          RenderTarget::kDefaultColorAttachmentConfig,
                          std::nullopt);
    cache.End();
    Idle(cache);
    EXPECT_EQ(cache.CachedTextureCount(), budget == kAttachmentBytes ? 1u : 0u);
  }
}

TEST_F(RenderTargetRetentionTest, ZeroIdleBudgetHonorsGracePeriod) {
  RenderTargetCache cache(allocator_, 4, 0);
  cache.Start();
  std::weak_ptr<Texture> target =
      ColorOnly(cache, {10, 10}).GetRenderTargetTexture();
  cache.End();
  Idle(cache, 4);
  EXPECT_FALSE(target.expired());
  Idle(cache);
  EXPECT_TRUE(target.expired());
}

TEST_F(RenderTargetRetentionTest, IdleObjectCountIsBounded) {
  RenderTargetCache cache(allocator_, 0);
  cache.Start();
  for (size_t i = 0; i < 129; i++) {
    ColorOnly(cache, {1, 1});
  }
  EXPECT_EQ(cache.CachedTextureCount(), 129u);
  cache.End();
  Idle(cache);
  EXPECT_EQ(cache.CachedTextureCount(), 128u);
}

TEST_F(RenderTargetRetentionTest, CacheBypassNeverReusesPersistentSnapshots) {
  RenderTargetCache cache(allocator_, 0);
  cache.Start();
  auto pooled = ColorOnly(cache, {10, 10}).GetRenderTargetTexture();
  cache.End();
  Idle(cache, 10);
  cache.Start();
  cache.DisableCache();
  cache.DisableCache();
  auto first = ColorOnly(cache, {10, 10}).GetRenderTargetTexture();
  cache.EnableCache();
  auto second = ColorOnly(cache, {10, 10}).GetRenderTargetTexture();
  EXPECT_NE(first, pooled);
  EXPECT_NE(second, pooled);
  EXPECT_NE(first, second);
  cache.EnableCache();
  EXPECT_EQ(ColorOnly(cache, {10, 10}).GetRenderTargetTexture(), pooled);
  EXPECT_EQ(cache.CachedTextureCount(), 1u);
  cache.End();
}

TEST_F(RenderTargetRetentionTest, ColorFormatAndStorageMustMatch) {
  RenderTargetCache cache(allocator_, 0);
  auto create = [&](PixelFormat format, StorageMode storage) {
    auto color = RenderTarget::kDefaultColorAttachmentConfig;
    color.storage_mode = storage;
    return cache
        .CreateOffscreen(*context_, {10, 10}, 1, "Test", color, std::nullopt,
                         nullptr, nullptr, format)
        .GetRenderTargetTexture();
  };
  cache.Start();
  auto rgba =
      create(PixelFormat::kR8G8B8A8UNormInt, StorageMode::kDevicePrivate);
  cache.End();
  cache.Start();
  auto bgra =
      create(PixelFormat::kB8G8R8A8UNormInt, StorageMode::kDevicePrivate);
  cache.End();
  cache.Start();
  auto host = create(PixelFormat::kR8G8B8A8UNormInt, StorageMode::kHostVisible);
  cache.End();
  EXPECT_NE(rgba, bgra);
  EXPECT_NE(rgba, host);
  Idle(cache, 10);
  cache.Start();
  EXPECT_EQ(create(PixelFormat::kR8G8B8A8UNormInt, StorageMode::kDevicePrivate),
            rgba);
  EXPECT_EQ(create(PixelFormat::kB8G8R8A8UNormInt, StorageMode::kDevicePrivate),
            bgra);
  EXPECT_EQ(create(PixelFormat::kR8G8B8A8UNormInt, StorageMode::kHostVisible),
            host);
  cache.End();
  EXPECT_EQ(allocator_->allocations, 3u);
}

TEST_F(RenderTargetRetentionTest, MsaaResolveStorageMustMatch) {
  RenderTargetCache cache(allocator_, 0);
  auto color = RenderTarget::kDefaultColorAttachmentConfigMSAA;
  cache.Start();
  auto device =
      cache.CreateOffscreenMSAA(*context_, {10, 10}, 1, "Test", color);
  cache.End();
  color.resolve_storage_mode = StorageMode::kHostVisible;
  cache.Start();
  auto host = cache.CreateOffscreenMSAA(*context_, {10, 10}, 1, "Test", color);
  EXPECT_NE(host.GetRenderTargetTexture(), device.GetRenderTargetTexture());
  EXPECT_EQ(host.GetRenderTargetTexture()->GetTextureDescriptor().storage_mode,
            StorageMode::kHostVisible);
  cache.End();
  Idle(cache, 10);
  const auto allocations = allocator_->allocations;
  cache.Start();
  auto reused =
      cache.CreateOffscreenMSAA(*context_, {10, 10}, 1, "Test", color);
  EXPECT_EQ(reused.GetRenderTargetTexture(), host.GetRenderTargetTexture());
  EXPECT_EQ(allocator_->allocations, allocations);
  cache.End();
}

}  // namespace
}  // namespace impeller::testing
