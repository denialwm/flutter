// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/glass_frost_cache.h"

#include "flutter/testing/testing.h"
#include "impeller/entity/contents/filters/gaussian_blur_filter_contents.h"
#include "impeller/renderer/testing/mocks.h"

namespace impeller {
namespace testing {
namespace {

std::shared_ptr<Texture> MakeFrostTexture() {
  TextureDescriptor descriptor;
  descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
  descriptor.size = {64, 64};
  return std::make_shared<::testing::NiceMock<MockTexture>>(descriptor);
}

GlassFrostKey MakeFrostKey() {
  return {.size = {64, 64},
          .source_uvs = Rect::MakeWH(1, 1).GetPoints(),
          .scaled_sigma = {12, 12},
          .effective_scale = {0.125, 0.125},
          .sampler_key = 7,
          .source_y_scale = -1,
          .source_mips_ready = true};
}

TEST(GlassFrostCacheTest, ReusesPixelsOnlyForIdenticalSamplingWork) {
  GlassFrostCache cache;
  const auto source = MakeFrostTexture();
  const auto frost = MakeFrostTexture();
  const auto key = MakeFrostKey();
  cache.SetSource(source);
  cache.Store(key, frost);
  EXPECT_EQ(cache.Find(key), frost);

  auto changed = key;
  changed.size.width++;
  EXPECT_EQ(cache.Find(changed), nullptr);
  changed = key;
  changed.source_uvs[0].x += 0.01f;
  EXPECT_EQ(cache.Find(changed), nullptr);
  changed = key;
  changed.scaled_sigma.y++;
  EXPECT_EQ(cache.Find(changed), nullptr);
  changed = key;
  changed.effective_scale.x = 0.25f;
  EXPECT_EQ(cache.Find(changed), nullptr);
  changed = key;
  changed.sampler_key++;
  EXPECT_EQ(cache.Find(changed), nullptr);
  changed = key;
  changed.source_y_scale = 1;
  EXPECT_EQ(cache.Find(changed), nullptr);
  changed = key;
  changed.source_mips_ready = false;
  EXPECT_EQ(cache.Find(changed), nullptr);
}

TEST(GlassFrostCacheTest, SceneFlipInvalidatesEvenRecycledTextureIdentity) {
  GlassFrostCache cache;
  const auto source = MakeFrostTexture();
  const auto key = MakeFrostKey();
  cache.SetSource(source);
  cache.Store(key, MakeFrostTexture());
  cache.SetSource(source);
  ASSERT_NE(cache.Find(key), nullptr);
  cache.Clear();
  cache.SetSource(source);
  EXPECT_EQ(cache.Find(key), nullptr);

  cache.Store(key, MakeFrostTexture());
  cache.SetSource(MakeFrostTexture());
  EXPECT_EQ(cache.Find(key), nullptr);
}

TEST(GlassFrostCacheTest, CapacityIsBoundedWithoutEvictingSubmittedResults) {
  GlassFrostCache cache;
  cache.SetSource(MakeFrostTexture());
  auto key = MakeFrostKey();
  const auto first = MakeFrostTexture();
  cache.Store(key, first);
  for (size_t i = 1; i < GlassFrostCache::kCapacity; ++i) {
    key.sampler_key++;
    cache.Store(key, MakeFrostTexture());
  }
  key.sampler_key++;
  cache.Store(key, MakeFrostTexture());
  EXPECT_EQ(cache.Find(key), nullptr);
  EXPECT_EQ(cache.Find(MakeFrostKey()), first);
  cache.Clear();
  EXPECT_EQ(cache.Find(MakeFrostKey()), nullptr);
}

TEST(GlassFrostCacheTest, ClearReleasesCacheOwnershipButNotConsumers) {
  GlassFrostCache cache;
  cache.SetSource(MakeFrostTexture());
  const auto key = MakeFrostKey();
  auto frost = MakeFrostTexture();
  std::weak_ptr<Texture> weak = frost;
  cache.Store(key, frost);
  frost.reset();
  auto consumer = cache.Find(key);
  cache.Clear();
  ASSERT_NE(consumer, nullptr);
  EXPECT_FALSE(weak.expired());
  consumer.reset();
  EXPECT_TRUE(weak.expired());
}

TEST(GlassFrostCacheTest, NormalizesOnlyLiveKernelCoefficients) {
  for (int radius : {1, 5, 16, 30}) {
    const auto kernel = GenerateBlurInfo({.blur_uv_offset = {0, 0.01f},
                                          .blur_sigma = 2,
                                          .blur_radius = radius,
                                          .step_size = 1,
                                          .apply_unpremultiply = false});
    float sum = 0;
    for (int i = 0; i < kernel.sample_count; ++i) {
      EXPECT_GE(kernel.samples[i].coefficient, 0);
      sum += kernel.samples[i].coefficient;
    }
    EXPECT_NEAR(sum, 1, 1e-6);
  }
}

}  // namespace
}  // namespace testing
}  // namespace impeller
