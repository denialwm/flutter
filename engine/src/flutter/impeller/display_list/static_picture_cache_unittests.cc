// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/static_picture_cache.h"

#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/effects/dl_image_filter.h"
#include "flutter/display_list/effects/dl_mask_filter.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/display_list/utils/dl_receiver_utils.h"
#include "flutter/impeller/entity/gles/entity_shaders_gles.h"
#include "flutter/impeller/entity/gles/framebuffer_blend_shaders_gles.h"
#include "flutter/impeller/entity/gles/modern_shaders_gles.h"
#include "gtest/gtest.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/proc_table_gles.h"
#include "impeller/renderer/backend/gles/reactor_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/render_target.h"

namespace impeller {
namespace testing {
namespace {

using namespace flutter;

class DrawCounter final : public virtual DlOpReceiver,
                          public IgnoreAttributeDispatchHelper,
                          public IgnoreClipDispatchHelper,
                          public IgnoreTransformDispatchHelper,
                          public IgnoreDrawDispatchHelper {
 public:
  void setColor(DlColor color) override { last_opacity = color.getAlphaF(); }

  void transform2DAffine(DlScalar mxx,
                         DlScalar mxy,
                         DlScalar mxt,
                         DlScalar myx,
                         DlScalar myy,
                         DlScalar myt) override {
    last_transform = DlMatrix::MakeRow(mxx, mxy, 0, mxt,  //
                                       myx, myy, 0, myt,  //
                                       0, 0, 1, 0,        //
                                       0, 0, 0, 1);
  }

  void transformFullPerspective(DlScalar mxx,
                                DlScalar mxy,
                                DlScalar mxz,
                                DlScalar mxt,
                                DlScalar myx,
                                DlScalar myy,
                                DlScalar myz,
                                DlScalar myt,
                                DlScalar mzx,
                                DlScalar mzy,
                                DlScalar mzz,
                                DlScalar mzt,
                                DlScalar mwx,
                                DlScalar mwy,
                                DlScalar mwz,
                                DlScalar mwt) override {
    last_transform = DlMatrix::MakeRow(mxx, mxy, mxz, mxt, myx, myy, myz, myt,
                                       mzx, mzy, mzz, mzt, mwx, mwy, mwz, mwt);
  }

  void clipPath(const DlPath& path, DlClipOp, bool) override {
    clip_paths++;
    last_path_fill = path.GetFillType();
  }

  void drawRoundRect(const DlRoundRect&) override { round_rects++; }

  void drawImageRect(const sk_sp<DlImage>,
                     const DlRect&,
                     const DlRect& destination,
                     DlImageSampling,
                     bool,
                     DlSrcRectConstraint) override {
    image_rects++;
    last_image_destination = destination;
  }

  void drawDisplayList(const sk_sp<DisplayList> display_list,
                       DlScalar) override {
    nested_display_lists++;
    display_list->Dispatch(*this);
  }

  size_t clip_paths = 0u;
  size_t round_rects = 0u;
  size_t image_rects = 0u;
  size_t nested_display_lists = 0u;
  std::optional<DlPathFillType> last_path_fill;
  std::optional<DlRect> last_image_destination;
  std::optional<DlMatrix> last_transform;
  std::optional<float> last_opacity;
};

class StaticPictureCacheTest : public ::testing::Test {
 protected:
  class Worker final : public ReactorGLES::Worker {
    bool CanReactorReactOnCurrentThreadNow(const ReactorGLES&) const override {
      return true;
    }
  };

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
      return kMockResolverGLES(name);
    };
    mock_ = MockGLES::Init(std::nullopt, "OpenGL ES 2.0", resolver);
    context_ = ContextGLES::Create(
        Flags{}, std::make_unique<ProcTableGLES>(resolver),
        {std::make_shared<fml::NonOwnedMapping>(
             impeller_entity_shaders_gles_data,
             impeller_entity_shaders_gles_length),
         std::make_shared<fml::NonOwnedMapping>(
             impeller_modern_shaders_gles_data,
             impeller_modern_shaders_gles_length),
         std::make_shared<fml::NonOwnedMapping>(
             impeller_framebuffer_blend_shaders_gles_data,
             impeller_framebuffer_blend_shaders_gles_length)},
        false);
    ASSERT_TRUE(context_);
    worker_ = std::make_shared<Worker>();
    context_->AddReactorWorker(worker_);
    renderer_ = std::make_unique<ContentContext>(context_, nullptr);
    ASSERT_TRUE(renderer_->IsValid());
  }

  sk_sp<DisplayList> MakeShadowPicture(
      DlRect outer = DlRect::MakeWH(200, 120),
      DlRect inner = DlRect::MakeLTRB(36, 24, 164, 96)) const {
    const DlRoundRect frame = DlRoundRect::MakeRectRadius(inner, 18);
    DlPathBuilder clip_builder;
    clip_builder.SetFillType(DlPathFillType::kOdd)
        .AddRect(outer)
        .AddRoundRect(frame);

    DisplayListBuilder builder(outer);
    builder.ClipPath(clip_builder.TakePath(), DlClipOp::kIntersect, true);
    DlPaint shadow;
    shadow.setColor(DlColor::kBlack().withAlphaF(0.35f));
    shadow.setMaskFilter(DlBlurMaskFilter::Make(DlBlurStyle::kNormal, 16.5f));
    builder.DrawRoundRect(
        DlRoundRect::MakeRectRadius(inner.Shift(0, 12).Expand(2), 22.5f),
        shadow);
    return builder.Build();
  }

  sk_sp<DisplayList> MakeRectPicture(DlColor color) const {
    DisplayListBuilder builder(DlRect::MakeWH(32, 32));
    builder.DrawRect(DlRect::MakeWH(32, 32), DlPaint().setColor(color));
    return builder.Build();
  }

  std::shared_ptr<Texture> MakeTexture(ISize size) const {
    TextureDescriptor descriptor;
    descriptor.storage_mode = StorageMode::kDevicePrivate;
    descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
    descriptor.size = size;
    descriptor.usage = TextureUsage::kShaderRead;
    return renderer_->GetContext()->GetResourceAllocator()->CreateTexture(
        descriptor);
  }

  size_t RenderCommandCount(const sk_sp<DisplayList>& display_list,
                            ISize size = {200, 120}) const {
    RenderTargetAllocator allocator(
        renderer_->GetContext()->GetResourceAllocator());
    RenderTarget target = allocator.CreateOffscreen(
        *context_, size, 1u, "Static Picture Cache Test Target");
    EXPECT_TRUE(target.IsValid());
    CanvasDlDispatcher dispatcher(
        *renderer_, target, false, display_list->root_has_backdrop_filter(),
        display_list->max_root_blend_mode(), IRect32::MakeSize(size));
    display_list->Dispatch(dispatcher,
                           DlIRect::MakeWH(size.width, size.height));
    const size_t count = dispatcher.GetCanvasForTesting()
                             .GetRenderPassForTesting()
                             .GetCommands()
                             .size();
    dispatcher.FinishRecording();
    return count;
  }

  std::shared_ptr<MockGLES> mock_;
  std::shared_ptr<ContextGLES> context_;
  std::shared_ptr<Worker> worker_;
  std::unique_ptr<ContentContext> renderer_;
};

TEST_F(StaticPictureCacheTest,
       GeneratesWholeCutoutThenHitsWithOneImageCommand) {
  StaticPictureCache cache(*renderer_);
  const auto shadow = MakeShadowPicture();
  const std::optional<DlMatrix> identity = DlMatrix();

  DisplayListBuilder first_frame;
  EXPECT_FALSE(cache.Draw(shadow, DlMatrix(), identity, 1.0f, first_frame));

  DisplayListBuilder generated_frame;
  EXPECT_TRUE(cache.Draw(shadow, DlMatrix(), identity, 1.0f, generated_frame));
  EXPECT_EQ(cache.GetStatistics().generations, 1u);

  DisplayListBuilder hit_frame;
  EXPECT_TRUE(cache.Draw(shadow, DlMatrix(), identity, 0.75f, hit_frame));
  EXPECT_EQ(cache.GetStatistics().hits, 1u);

  DrawCounter shadow_ops;
  shadow->Dispatch(shadow_ops);
  EXPECT_EQ(shadow_ops.clip_paths, 1u);
  EXPECT_EQ(shadow_ops.round_rects, 1u);
  EXPECT_EQ(shadow_ops.last_path_fill, DlPathFillType::kOdd);

  const auto hit = hit_frame.Build();
  DrawCounter hit_ops;
  hit->Dispatch(hit_ops);
  EXPECT_EQ(hit_ops.image_rects, 1u);
  EXPECT_EQ(hit_ops.clip_paths, 0u);
  EXPECT_EQ(hit_ops.round_rects, 0u);
  EXPECT_EQ(hit_ops.nested_display_lists, 0u);
  ASSERT_TRUE(hit_ops.last_opacity.has_value());
  EXPECT_NEAR(hit_ops.last_opacity.value(), DlColor::toAlpha(0.75f) / 255.0f,
              0.00001f);

  EXPECT_EQ(RenderTarget::kDefaultColorAttachmentConfig.clear_color,
            Color::BlackTransparent());

  const size_t direct_commands = RenderCommandCount(shadow);
  const size_t hit_commands = RenderCommandCount(hit);
  EXPECT_GT(direct_commands, hit_commands);
  EXPECT_EQ(direct_commands, 3u);
  EXPECT_EQ(hit_commands, 1u);
}

TEST_F(StaticPictureCacheTest,
       IntegerMovementReusesButFractionScaleAndContentInvalidate) {
  StaticPictureCache::Limits limits;
  std::vector<ISize> generated_sizes;
  StaticPictureCache cache(
      *renderer_, limits, [&](const sk_sp<DisplayList>& snapshot, ISize size) {
        DrawCounter snapshot_ops;
        snapshot->Dispatch(snapshot_ops);
        EXPECT_EQ(snapshot_ops.nested_display_lists, 1u);
        EXPECT_EQ(snapshot_ops.clip_paths, 1u);
        EXPECT_EQ(snapshot_ops.round_rects, 1u);
        EXPECT_EQ(snapshot_ops.last_path_fill, DlPathFillType::kOdd);
        generated_sizes.push_back(size);
        return MakeTexture(size);
      });
  const auto shadow = MakeShadowPicture();
  const std::optional<DlMatrix> identity = DlMatrix();

  DisplayListBuilder output;
  const DlMatrix at_fraction = DlMatrix::MakeTranslation({10.25f, 4.5f});
  EXPECT_FALSE(cache.Draw(shadow, at_fraction, identity, 1.0f, output));
  EXPECT_TRUE(cache.Draw(shadow, at_fraction, identity, 1.0f, output));
  EXPECT_TRUE(cache.Draw(shadow, DlMatrix::MakeTranslation({11.25f, 5.5f}),
                         identity, 1.0f, output));
  EXPECT_EQ(generated_sizes.size(), 1u);
  EXPECT_EQ(cache.GetStatistics().hits, 1u);

  EXPECT_FALSE(cache.Draw(shadow, DlMatrix::MakeTranslation({11.5f, 5.5f}),
                          identity, 1.0f, output));
  EXPECT_FALSE(cache.Draw(shadow, DlMatrix::MakeScale(Vector2(1.25f, 1.25f)),
                          identity, 1.0f, output));
  EXPECT_FALSE(cache.Draw(MakeRectPicture(DlColor::kRed()), DlMatrix(),
                          identity, 1.0f, output));
  EXPECT_EQ(generated_sizes.size(), 1u);
}

TEST_F(StaticPictureCacheTest,
       DeferredOutputTransformParticipatesInScaleAndAlignment) {
  std::vector<ISize> generated_sizes;
  StaticPictureCache cache(*renderer_, StaticPictureCache::Limits(),
                           [&](const sk_sp<DisplayList>&, ISize size) {
                             generated_sizes.push_back(size);
                             return MakeTexture(size);
                           });
  const auto shadow = MakeShadowPicture();
  DisplayListBuilder output;
  const DlMatrix canvas_transform = DlMatrix::MakeTranslation({10.0f, 5.0f});
  const std::optional<DlMatrix> deferred =
      DlMatrix::MakeTranslation({0.25f, 0.5f}) *
      DlMatrix::MakeScale(Vector2(2.0f, 2.0f));

  EXPECT_FALSE(cache.Draw(shadow, canvas_transform, deferred, 1.0f, output));
  EXPECT_TRUE(cache.Draw(shadow, canvas_transform, deferred, 1.0f, output));
  ASSERT_EQ(generated_sizes.size(), 1u);
  EXPECT_EQ(generated_sizes[0], ISize(401, 241));

  EXPECT_TRUE(cache.Draw(shadow, DlMatrix::MakeTranslation({11.0f, 6.0f}),
                         deferred, 1.0f, output));
  EXPECT_EQ(generated_sizes.size(), 1u);

  const std::optional<DlMatrix> shifted_deferred =
      DlMatrix::MakeTranslation({0.5f, 0.5f}) *
      DlMatrix::MakeScale(Vector2(2.0f, 2.0f));
  EXPECT_FALSE(
      cache.Draw(shadow, canvas_transform, shifted_deferred, 1.0f, output));
  EXPECT_FALSE(
      cache.Draw(shadow, canvas_transform, std::nullopt, 1.0f, output));
  EXPECT_EQ(generated_sizes.size(), 1u);
}

TEST_F(StaticPictureCacheTest,
       NegativeBoundsMapToIdenticalPixelsWithNonUniformDeferredTransform) {
  const DlRect outer = DlRect::MakeLTRB(-64, -48, 136, 72);
  const DlRect inner = DlRect::MakeLTRB(-28, -24, 100, 48);
  const auto shadow = MakeShadowPicture(outer, inner);
  std::optional<DlMatrix> snapshot_transform;
  ISize snapshot_size;
  StaticPictureCache cache(*renderer_, StaticPictureCache::Limits(),
                           [&](const sk_sp<DisplayList>& snapshot, ISize size) {
                             DrawCounter ops;
                             snapshot->Dispatch(ops);
                             snapshot_transform = ops.last_transform;
                             snapshot_size = size;
                             return MakeTexture(size);
                           });

  const DlMatrix canvas_transform = DlMatrix::MakeTranslation({-7.25f, 13.5f}) *
                                    DlMatrix::MakeScale(Vector2(1.5f, 2.25f));
  const std::optional<DlMatrix> deferred =
      DlMatrix::MakeTranslation({0.375f, 0.625f});
  const DlMatrix effective = deferred.value() * canvas_transform;
  const DlRect device_bounds = outer.TransformBounds(effective);
  const DlIRect rounded = DlIRect::RoundOut(device_bounds);
  const DlMatrix expected_snapshot_transform =
      DlMatrix::MakeTranslation({-static_cast<float>(rounded.GetLeft()),
                                 -static_cast<float>(rounded.GetTop())}) *
      effective;

  DisplayListBuilder output;
  EXPECT_FALSE(cache.Draw(shadow, canvas_transform, deferred, 0.37f, output));
  EXPECT_TRUE(cache.Draw(shadow, canvas_transform, deferred, 0.37f, output));
  ASSERT_TRUE(snapshot_transform.has_value());
  EXPECT_EQ(snapshot_size, ISize(rounded.GetSize()));
  for (size_t i = 0u; i < 16u; i++) {
    EXPECT_NEAR(snapshot_transform->m[i], expected_snapshot_transform.m[i],
                0.00001f);
  }

  DrawCounter cached_ops;
  output.Build()->Dispatch(cached_ops);
  ASSERT_TRUE(cached_ops.last_image_destination.has_value());
  const DlRect mapped_destination =
      cached_ops.last_image_destination->TransformBounds(effective);
  EXPECT_NEAR(mapped_destination.GetLeft(), rounded.GetLeft(), 0.0001f);
  EXPECT_NEAR(mapped_destination.GetTop(), rounded.GetTop(), 0.0001f);
  EXPECT_NEAR(mapped_destination.GetRight(), rounded.GetRight(), 0.0001f);
  EXPECT_NEAR(mapped_destination.GetBottom(), rounded.GetBottom(), 0.0001f);
  ASSERT_TRUE(cached_ops.last_opacity.has_value());
  EXPECT_NEAR(cached_ops.last_opacity.value(), DlColor::toAlpha(0.37f) / 255.0f,
              0.00001f);
}

TEST_F(StaticPictureCacheTest, EnforcesEntryAndByteBudgetsAndBacksOffFailure) {
  StaticPictureCache::Limits eviction_limits;
  eviction_limits.max_entries = 1u;
  eviction_limits.admission_threshold = 1u;
  size_t snapshots = 0u;
  StaticPictureCache eviction_cache(*renderer_, eviction_limits,
                                    [&](const sk_sp<DisplayList>&, ISize size) {
                                      snapshots++;
                                      return MakeTexture(size);
                                    });
  DisplayListBuilder output;
  const std::optional<DlMatrix> identity = DlMatrix();
  EXPECT_TRUE(eviction_cache.Draw(MakeRectPicture(DlColor::kRed()), DlMatrix(),
                                  identity, 1.0f, output));
  EXPECT_TRUE(eviction_cache.Draw(MakeRectPicture(DlColor::kBlue()), DlMatrix(),
                                  identity, 1.0f, output));
  EXPECT_EQ(eviction_cache.GetEntryCount(), 1u);
  EXPECT_EQ(eviction_cache.GetStatistics().evictions, 1u);
  EXPECT_EQ(snapshots, 2u);

  StaticPictureCache::Limits budget_limits;
  budget_limits.max_entry_bytes = 100u;
  budget_limits.admission_threshold = 1u;
  size_t budget_snapshots = 0u;
  StaticPictureCache budget_cache(*renderer_, budget_limits,
                                  [&](const sk_sp<DisplayList>&, ISize size) {
                                    budget_snapshots++;
                                    return MakeTexture(size);
                                  });
  EXPECT_FALSE(budget_cache.Draw(MakeRectPicture(DlColor::kGreen()), DlMatrix(),
                                 identity, 1.0f, output));
  EXPECT_EQ(budget_snapshots, 0u);
  EXPECT_EQ(budget_cache.GetEntryCount(), 0u);

  StaticPictureCache::Limits failure_limits;
  failure_limits.admission_threshold = 1u;
  size_t attempts = 0u;
  StaticPictureCache failure_cache(
      *renderer_, failure_limits,
      [&](const sk_sp<DisplayList>&, ISize) -> std::shared_ptr<Texture> {
        attempts++;
        return nullptr;
      });
  const auto failed_picture = MakeRectPicture(DlColor::kYellow());
  EXPECT_FALSE(
      failure_cache.Draw(failed_picture, DlMatrix(), identity, 1.0f, output));
  EXPECT_EQ(attempts, 1u);
  for (size_t i = 0u; i < 60u; i++) {
    EXPECT_FALSE(
        failure_cache.Draw(failed_picture, DlMatrix(), identity, 1.0f, output));
  }
  EXPECT_EQ(attempts, 1u);
  EXPECT_FALSE(
      failure_cache.Draw(failed_picture, DlMatrix(), identity, 1.0f, output));
  EXPECT_EQ(attempts, 2u);
}

TEST_F(StaticPictureCacheTest, TotalByteBudgetEvictsLeastRecentlyUsedEntry) {
  StaticPictureCache::Limits limits;
  limits.max_entries = 4u;
  limits.max_bytes = 2u * 32u * 32u * 4u;
  limits.max_entry_bytes = 32u * 32u * 4u;
  limits.admission_threshold = 1u;
  size_t snapshots = 0u;
  StaticPictureCache cache(*renderer_, limits,
                           [&](const sk_sp<DisplayList>&, ISize size) {
                             snapshots++;
                             return MakeTexture(size);
                           });
  const auto red = MakeRectPicture(DlColor::kRed());
  const auto green = MakeRectPicture(DlColor::kGreen());
  const auto blue = MakeRectPicture(DlColor::kBlue());
  const std::optional<DlMatrix> identity = DlMatrix();
  DisplayListBuilder output;

  EXPECT_TRUE(cache.Draw(red, DlMatrix(), identity, 1.0f, output));
  EXPECT_TRUE(cache.Draw(green, DlMatrix(), identity, 1.0f, output));
  EXPECT_TRUE(cache.Draw(red, DlMatrix(), identity, 1.0f, output));
  EXPECT_TRUE(cache.Draw(blue, DlMatrix(), identity, 1.0f, output));
  EXPECT_EQ(cache.GetEntryCount(), 2u);
  EXPECT_EQ(cache.GetResidentBytes(), limits.max_bytes);
  EXPECT_EQ(cache.GetStatistics().evictions, 1u);

  EXPECT_TRUE(cache.Draw(red, DlMatrix(), identity, 1.0f, output));
  EXPECT_TRUE(cache.Draw(green, DlMatrix(), identity, 1.0f, output));
  EXPECT_EQ(snapshots, 4u);
  EXPECT_EQ(cache.GetStatistics().hits, 2u);
  EXPECT_EQ(cache.GetStatistics().evictions, 2u);
}

TEST_F(StaticPictureCacheTest, RejectsDynamicAndDestinationDependentContent) {
  const auto texture = MakeTexture({8, 8});
  const auto external_image =
      DlImageImpeller::Make(texture, DlImage::OwningContext::kRaster, true);
  DisplayListBuilder image_builder(DlRect::MakeWH(8, 8));
  image_builder.DrawImage(external_image, DlPoint(), DlImageSampling::kLinear);
  const auto image = image_builder.Build();
  EXPECT_FALSE(StaticPictureCache::IsPictureEligibleForTesting(image));

  const auto child = MakeRectPicture(DlColor::kRed());
  DisplayListBuilder nested_builder(DlRect::MakeWH(32, 32));
  nested_builder.DrawDisplayList(child);
  EXPECT_FALSE(
      StaticPictureCache::IsPictureEligibleForTesting(nested_builder.Build()));

  DisplayListBuilder backdrop_builder(DlRect::MakeWH(32, 32));
  const auto blur = DlImageFilter::MakeBlur(4, 4, DlTileMode::kClamp);
  backdrop_builder.SaveLayer(DlRect::MakeWH(32, 32), nullptr, blur.get());
  backdrop_builder.DrawRect(DlRect::MakeWH(32, 32),
                            DlPaint().setColor(DlColor::kBlue()));
  backdrop_builder.Restore();
  EXPECT_FALSE(StaticPictureCache::IsPictureEligibleForTesting(
      backdrop_builder.Build()));

  DisplayListBuilder blend_builder(DlRect::MakeWH(32, 32));
  blend_builder.DrawRect(
      DlRect::MakeWH(32, 32),
      DlPaint().setColor(DlColor::kRed()).setBlendMode(DlBlendMode::kSrc));
  const auto blend = blend_builder.Build();
  EXPECT_FALSE(StaticPictureCache::IsPictureEligibleForTesting(blend));

  DisplayListBuilder clear_builder(DlRect::MakeWH(32, 32));
  clear_builder.DrawRect(
      DlRect::MakeWH(32, 32),
      DlPaint().setColor(DlColor::kRed()).setBlendMode(DlBlendMode::kClear));
  EXPECT_FALSE(
      StaticPictureCache::IsPictureEligibleForTesting(clear_builder.Build()));

  StaticPictureCache cache(*renderer_);
  DisplayListBuilder output;
  const std::optional<DlMatrix> identity = DlMatrix();
  EXPECT_FALSE(cache.Draw(image, DlMatrix(), identity, 1.0f, output));
  EXPECT_FALSE(cache.Draw(image, DlMatrix(), identity, 1.0f, output));
  EXPECT_EQ(cache.GetStatistics().eligibility_checks, 1u);
  EXPECT_EQ(cache.GetStatistics().rejected_pictures, 1u);
}

}  // namespace
}  // namespace testing
}  // namespace impeller
