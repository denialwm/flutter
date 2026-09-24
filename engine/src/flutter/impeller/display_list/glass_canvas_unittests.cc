// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>
#include <memory>

#include "flutter/common/backdrop_filter_cache_key.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/display_list/effects/image_filters/dl_glass_image_filter.h"
#include "flutter/testing/testing.h"
#include "fml/mapping.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/display_list/canvas.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/filters/glass_filter_contents.h"
#include "impeller/entity/gles3/entity_shaders_gles.h"
#include "impeller/entity/gles3/framebuffer_blend_shaders_gles.h"
#include "impeller/entity/gles3/modern_shaders_gles.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/render_target.h"

namespace impeller::testing {
namespace {

class HeadlessWorker final : public ReactorGLES::Worker {
 public:
  bool CanReactorReactOnCurrentThreadNow(
      const ReactorGLES& reactor) const override {
    return true;
  }
};

class HeadlessGlassCanvasTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const ProcTableGLES::Resolver resolver = [](const char* name) -> void* {
      if (strcmp(name, "glCreateShader") == 0) {
        return reinterpret_cast<void*>(+[](GLenum) -> GLuint { return 1; });
      }
      if (strcmp(name, "glCreateProgram") == 0) {
        return reinterpret_cast<void*>(+[]() -> GLuint { return 1; });
      }
      if (strcmp(name, "glIsProgram") == 0) {
        return reinterpret_cast<void*>(
            +[](GLuint) -> GLboolean { return GL_TRUE; });
      }
      if (strcmp(name, "glGetShaderiv") == 0) {
        return reinterpret_cast<void*>(+[](GLuint, GLenum query, GLint* value) {
          *value = query == GL_COMPILE_STATUS ? GL_TRUE : 0;
        });
      }
      if (strcmp(name, "glGetProgramiv") == 0) {
        return reinterpret_cast<void*>(+[](GLuint, GLenum query, GLint* value) {
          *value = query == GL_LINK_STATUS ? GL_TRUE : 0;
        });
      }
      if (strcmp(name, "glGetUniformLocation") == 0) {
        return reinterpret_cast<void*>(
            +[](GLuint, const GLchar*) -> GLint { return -1; });
      }
      return kMockResolverGLES(name);
    };
    mock_gl_ = MockGLES::Init(std::nullopt, "OpenGL ES 3.0", resolver);
    context_ = ContextGLES::Create(
        Flags{}, std::make_unique<ProcTableGLES>(resolver),
        {std::make_shared<fml::NonOwnedMapping>(
             impeller_entity_shaders_gles3_data,
             impeller_entity_shaders_gles3_length),
         std::make_shared<fml::NonOwnedMapping>(
             impeller_modern_shaders_gles3_data,
             impeller_modern_shaders_gles3_length),
         std::make_shared<fml::NonOwnedMapping>(
             impeller_framebuffer_blend_shaders_gles3_data,
             impeller_framebuffer_blend_shaders_gles3_length)},
        false);
    ASSERT_TRUE(context_);
    worker_ = std::make_shared<HeadlessWorker>();
    worker_id_ = context_->AddReactorWorker(worker_);
    ASSERT_TRUE(worker_id_);
  }

  void TearDown() override {
    if (context_ && worker_id_) {
      context_->RemoveReactorWorker(*worker_id_);
    }
  }

  std::shared_ptr<MockGLES> mock_gl_;
  std::shared_ptr<ContextGLES> context_;
  std::shared_ptr<HeadlessWorker> worker_;
  std::optional<ReactorGLES::WorkerID> worker_id_;
};

std::unique_ptr<Canvas> CreateHeadlessCanvas(
    ContentContext& context,
    std::optional<Rect> cull_rect = std::nullopt,
    bool requires_readback = false) {
  TextureDescriptor desc;
  desc.size = {100, 100};
  desc.format = context.GetDeviceCapabilities().GetDefaultColorFormat();
  desc.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  desc.storage_mode = StorageMode::kDevicePrivate;
  desc.sample_count = SampleCount::kCount1;
  auto onscreen =
      context.GetContext()->GetResourceAllocator()->CreateTexture(desc);
  ColorAttachment color;
  color.texture = onscreen;
  color.load_action = LoadAction::kClear;
  RenderTarget target;
  target.SetColorAttachment(color, 0);
  if (cull_rect) {
    return std::make_unique<Canvas>(context, target, false, requires_readback,
                                    *cull_rect);
  }
  return std::make_unique<Canvas>(context, target, false, requires_readback);
}

class CountingGlassTargetAllocator final : public RenderTargetAllocator {
 public:
  explicit CountingGlassTargetAllocator(std::shared_ptr<Allocator> allocator)
      : RenderTargetAllocator(std::move(allocator)) {}

  RenderTarget CreateOffscreen(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label,
      RenderTarget::AttachmentConfig color_attachment_config,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config,
      const std::shared_ptr<Texture>& existing_color_texture,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture,
      std::optional<PixelFormat> target_pixel_format) override {
    if (label == "Denial Glass Material" ||
        label == "Denial pooled glass material") {
      material_passes++;
    }
    if (label.starts_with("Denial Backdrop Blur ")) {
      blur_target_allocations++;
    }
    return RenderTargetAllocator::CreateOffscreen(
        context, size, mip_count, label, color_attachment_config,
        stencil_attachment_config, existing_color_texture,
        existing_depth_stencil_texture, target_pixel_format);
  }

  size_t material_passes = 0;
  size_t blur_target_allocations = 0;
};

TEST_F(HeadlessGlassCanvasTest,
       GlassChildLayerRejectsSamplingAndAttachmentHazards) {
  ContentContext context(context_, nullptr);
  TextureDescriptor descriptor;
  descriptor.size = {100, 100};
  descriptor.format = context.GetDeviceCapabilities().GetDefaultColorFormat();
  descriptor.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  descriptor.storage_mode = StorageMode::kDevicePrivate;
  auto make_texture = [&] {
    return context.GetContext()->GetResourceAllocator()->CreateTexture(
        descriptor);
  };
  const auto destination = make_texture();
  const auto scene_texture = make_texture();
  const auto frost_texture = make_texture();
  ASSERT_TRUE(destination);
  ASSERT_TRUE(scene_texture);
  ASSERT_TRUE(frost_texture);

  RenderTarget target;
  ColorAttachment color;
  color.texture = destination;
  color.load_action = LoadAction::kClear;
  target.SetColorAttachment(color, 0);
  const Snapshot scene{.texture = scene_texture};
  const Snapshot frost{.texture = frost_texture};
  const Rect integral = Rect::MakeXYWH(10, 20, 40, 30);
  auto allowed = [&](const RenderTarget& candidate, const Rect& coverage) {
    return CanDrawGlassIntoChildLayer(candidate, scene, frost, coverage,
                                      descriptor.format);
  };
  EXPECT_TRUE(allowed(target, integral));
  color.load_action = LoadAction::kDontCare;
  RenderTarget undefined_color = target;
  undefined_color.SetColorAttachment(color, 0);
  EXPECT_FALSE(allowed(undefined_color, integral));
  color.load_action = LoadAction::kLoad;
  RenderTarget loaded_color = target;
  loaded_color.SetColorAttachment(color, 0);
  EXPECT_FALSE(allowed(loaded_color, integral));
  color.load_action = LoadAction::kClear;
  EXPECT_FALSE(allowed(target, Rect::MakeXYWH(10.5, 20, 40, 30)));
  EXPECT_FALSE(allowed(target, Rect::MakeXYWH(10, 20, 40.5, 30)));
  EXPECT_FALSE(allowed(target, Rect::MakeXYWH(80, 20, 40, 30)));
  EXPECT_FALSE(CanDrawGlassIntoChildLayer(target, scene, frost, integral,
                                          PixelFormat::kUnknown));

  RenderTarget scene_color = target;
  color.texture = scene_texture;
  scene_color.SetColorAttachment(color, 0);
  EXPECT_FALSE(allowed(scene_color, integral));

  RenderTarget frost_resolve = target;
  color.resolve_texture = frost_texture;
  color.store_action = StoreAction::kMultisampleResolve;
  frost_resolve.SetColorAttachment(color, 0);
  EXPECT_FALSE(allowed(frost_resolve, integral));

  RenderTarget other_color = target;
  ColorAttachment second;
  second.texture = frost_texture;
  other_color.SetColorAttachment(second, 1);
  EXPECT_FALSE(allowed(other_color, integral));

  RenderTarget depth_target = target;
  DepthAttachment depth;
  depth.texture = scene_texture;
  depth_target.SetDepthAttachment(depth);
  EXPECT_FALSE(allowed(depth_target, integral));

  RenderTarget stencil_target = target;
  StencilAttachment stencil;
  stencil.texture = frost_texture;
  stencil_target.SetStencilAttachment(stencil);
  EXPECT_FALSE(allowed(stencil_target, integral));
}

std::shared_ptr<flutter::DlImageFilter> MakeChildLayerTestGlass(
    float backdrop_alpha_threshold = -1.0f,
    float shape_x = 10.0f,
    float sigma = 0.0f,
    float tint_strength = 0.08f) {
  return flutter::DlGlassImageFilter::Make(
      /*sigma_x=*/sigma, /*sigma_y=*/sigma,
      flutter::DlRoundRect::MakeRectXY(
          flutter::DlRect::MakeXYWH(shape_x, 20, 40, 30), 8, 8),
      /*downsample_scale=*/1, /*thickness=*/20, /*refraction=*/0.55,
      /*dispersion=*/0.12, /*saturation=*/1.2, flutter::DlColor::kWhite(),
      /*tint_strength=*/tint_strength, /*brightness=*/0.06, /*light_angle=*/0,
      /*light_intensity=*/0.7, /*edge_strength=*/0.4, backdrop_alpha_threshold);
}

TEST_F(HeadlessGlassCanvasTest,
       GlassChildLayerSkipsMaterialPassWhenPixelAligned) {
  auto allocator = std::make_shared<CountingGlassTargetAllocator>(
      std::static_pointer_cast<Context>(context_)->GetResourceAllocator());
  ContentContext context(context_, nullptr, allocator);
  ASSERT_TRUE(context.IsValid());
  const auto filter = MakeChildLayerTestGlass();
  ASSERT_TRUE(filter);

  auto aligned = CreateHeadlessCanvas(context, Rect::MakeWH(100, 100),
                                      /*requires_readback=*/true);
  aligned->SaveLayer({}, Rect::MakeXYWH(10, 20, 40, 30), filter.get(),
                     ContentBoundsPromise::kContainsContents,
                     /*total_content_depth=*/1,
                     /*can_distribute_opacity=*/false, std::nullopt,
                     /*content_is_single_sample_compatible=*/false);
  aligned->Restore();
  EXPECT_EQ(allocator->material_passes, 0u);

  // A fractional child origin needs the old texture composite and its own
  // single-sample material pass. This also runs the ordinary shader-uniform
  // path.
  auto fractional_filter = MakeChildLayerTestGlass(-1.0f, 10.25f);
  ASSERT_TRUE(fractional_filter);
  auto fractional = CreateHeadlessCanvas(context, Rect::MakeWH(100, 100),
                                         /*requires_readback=*/true);
  fractional->SaveLayer({}, Rect::MakeXYWH(10, 20, 41, 30),
                        fractional_filter.get(),
                        ContentBoundsPromise::kContainsContents,
                        /*total_content_depth=*/1,
                        /*can_distribute_opacity=*/false, std::nullopt,
                        /*content_is_single_sample_compatible=*/false);
  fractional->Restore();
  EXPECT_EQ(allocator->material_passes, 1u);
}

TEST_F(HeadlessGlassCanvasTest,
       GlassChildLayerKeepsThresholdAndGroupedMaterialPasses) {
  auto allocator = std::make_shared<CountingGlassTargetAllocator>(
      std::static_pointer_cast<Context>(context_)->GetResourceAllocator());
  ContentContext context(context_, nullptr, allocator);
  ASSERT_TRUE(context.IsValid());
  const Rect bounds = Rect::MakeXYWH(10, 20, 40, 30);

  auto threshold_filter = MakeChildLayerTestGlass(0.5f);
  ASSERT_TRUE(threshold_filter);
  auto threshold = CreateHeadlessCanvas(context, Rect::MakeWH(100, 100), true);
  threshold->SaveLayer({}, bounds, threshold_filter.get(),
                       ContentBoundsPromise::kContainsContents,
                       /*total_content_depth=*/1,
                       /*can_distribute_opacity=*/false, std::nullopt,
                       /*content_is_single_sample_compatible=*/false);
  threshold->Restore();
  EXPECT_EQ(allocator->material_passes, 1u);

  const int64_t key = flutter::MakeBackdropFilterCacheKey(41u, 1u);
  auto grouped_filter = MakeChildLayerTestGlass();
  ASSERT_TRUE(grouped_filter);
  auto grouped = CreateHeadlessCanvas(context, Rect::MakeWH(100, 100), true);
  grouped->SetBackdropData({{key, BackdropData{.backdrop_count = 2u}}}, 2u);
  grouped->SaveLayer({}, bounds, grouped_filter.get(),
                     ContentBoundsPromise::kContainsContents,
                     /*total_content_depth=*/1,
                     /*can_distribute_opacity=*/false, key,
                     /*content_is_single_sample_compatible=*/false);
  grouped->Restore();
  EXPECT_EQ(allocator->material_passes, 2u);
}

TEST_F(HeadlessGlassCanvasTest, GroupedGlassSharesExactFrostFootprint) {
  auto allocator = std::make_shared<CountingGlassTargetAllocator>(
      std::static_pointer_cast<Context>(context_)->GetResourceAllocator());
  ContentContext context(context_, nullptr, allocator);
  ASSERT_TRUE(context.IsValid());
  const int64_t key = flutter::MakeBackdropFilterCacheKey(50u, 1u);
  auto canvas = CreateHeadlessCanvas(context, Rect::MakeWH(100, 100), true);
  canvas->SetBackdropData(
      {{key, BackdropData{.backdrop_count = 4u, .all_filters_equal = false}}},
      4u);

  auto first = MakeChildLayerTestGlass(-1.0f, 10.0f, 8.0f, 0.08f);
  auto second = MakeChildLayerTestGlass(-1.0f, 10.0f, 8.0f, 0.2f);
  auto changed_sigma = MakeChildLayerTestGlass(-1.0f, 10.0f, 12.0f, 0.2f);
  auto changed_crop = MakeChildLayerTestGlass(-1.0f, 30.0f, 8.0f, 0.2f);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(changed_sigma);
  ASSERT_TRUE(changed_crop);

  const auto draw = [&](const std::shared_ptr<flutter::DlImageFilter>& filter,
                        const Rect& bounds) {
    canvas->SaveLayer({}, bounds, filter.get(),
                      ContentBoundsPromise::kContainsContents,
                      /*total_content_depth=*/1,
                      /*can_distribute_opacity=*/false, key,
                      /*content_is_single_sample_compatible=*/false);
    canvas->Restore();
  };
  draw(first, Rect::MakeXYWH(10, 20, 40, 30));
  const size_t first_blur_targets = allocator->blur_target_allocations;
  EXPECT_EQ(first_blur_targets, 2u);
  EXPECT_EQ(allocator->material_passes, 1u);

  // The two materials differ in tint but read the same grouped scene at the
  // same crop and blur kernel. The second material still gets its own pass.
  draw(second, Rect::MakeXYWH(10, 20, 40, 30));
  EXPECT_EQ(allocator->blur_target_allocations, first_blur_targets);
  EXPECT_EQ(allocator->material_passes, 2u);

  draw(changed_sigma, Rect::MakeXYWH(10, 20, 40, 30));
  const size_t sigma_blur_targets = allocator->blur_target_allocations;
  EXPECT_EQ(sigma_blur_targets, first_blur_targets + 2u);
  EXPECT_EQ(allocator->material_passes, 3u);

  draw(changed_crop, Rect::MakeXYWH(30, 20, 40, 30));
  EXPECT_EQ(allocator->blur_target_allocations, sigma_blur_targets + 2u);
  EXPECT_EQ(allocator->material_passes, 4u);

  // A separate Canvas owns a different scene epoch and cannot reuse the first
  // Canvas's frosted texture even with identical material and blur settings.
  auto next_scene = CreateHeadlessCanvas(context, Rect::MakeWH(100, 100), true);
  next_scene->SetBackdropData(
      {{key, BackdropData{.backdrop_count = 2u, .all_filters_equal = false}}},
      2u);
  next_scene->SaveLayer({}, Rect::MakeXYWH(10, 20, 40, 30), first.get(),
                        ContentBoundsPromise::kContainsContents,
                        /*total_content_depth=*/1,
                        /*can_distribute_opacity=*/false, key,
                        /*content_is_single_sample_compatible=*/false);
  next_scene->Restore();
  EXPECT_EQ(allocator->blur_target_allocations, sigma_blur_targets + 4u);
  EXPECT_EQ(allocator->material_passes, 5u);
}

TEST_F(HeadlessGlassCanvasTest, GlassCachedChildLayerDoesNotRecomputeMaterial) {
  auto allocator = std::make_shared<CountingGlassTargetAllocator>(
      std::static_pointer_cast<Context>(context_)->GetResourceAllocator());
  ContentContext context(context_, nullptr, allocator);
  ASSERT_TRUE(context.IsValid());
  TextureDescriptor descriptor;
  descriptor.size = {100, 100};
  descriptor.format = context.GetDeviceCapabilities().GetDefaultColorFormat();
  descriptor.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  descriptor.storage_mode = StorageMode::kDevicePrivate;
  auto cached_texture =
      context.GetContext()->GetResourceAllocator()->CreateTexture(descriptor);
  ASSERT_TRUE(cached_texture);
  const int64_t key = flutter::MakeBackdropFilterCacheKey(42u, 1u);
  context.CacheBackdropSnapshot(key, Snapshot{.texture = cached_texture});

  auto filter = MakeChildLayerTestGlass();
  ASSERT_TRUE(filter);
  auto canvas = CreateHeadlessCanvas(context, Rect::MakeWH(100, 100), true);
  canvas->SetBackdropData({{key, BackdropData{.backdrop_count = 1u}}}, 1u);
  canvas->SaveLayer({}, Rect::MakeXYWH(10, 20, 40, 30), filter.get(),
                    ContentBoundsPromise::kContainsContents,
                    /*total_content_depth=*/1,
                    /*can_distribute_opacity=*/false, key,
                    /*content_is_single_sample_compatible=*/false);
  canvas->Restore();
  EXPECT_EQ(allocator->material_passes, 0u);
}

}  // namespace
}  // namespace impeller::testing
