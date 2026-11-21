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
#include "impeller/renderer/backend/gles/texture_gles.h"
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

  void TearDown() override {
    if (renderer_) {
      renderer_->SetStaticPictureCache(nullptr);
    }
  }

  // A frame that replays `picture` as a nested DisplayList, as the layer
  // tree records it.
  static sk_sp<DisplayList> MakeFrame(const sk_sp<DisplayList>& picture,
                                      DlScalar opacity = 1.0f) {
    DisplayListBuilder frame(DlRect::MakeWH(200, 120));
    frame.DrawDisplayList(picture, opacity);
    return frame.Build();
  }

  static bool Cached(StaticPictureCache& cache,
                     const sk_sp<DisplayList>& picture,
                     const DlMatrix& transform,
                     const StaticPictureCache::Pass& pass = {}) {
    return cache.Lookup(picture, transform, pass).has_value();
  }

  std::shared_ptr<MockGLES> mock_;
  std::shared_ptr<ContextGLES> context_;
  std::shared_ptr<Worker> worker_;
  std::unique_ptr<ContentContext> renderer_;
};

TEST_F(StaticPictureCacheTest,
       GeneratesWholeCutoutThenHitsWithOneImageCommand) {
  StaticPictureCache cache(*renderer_);
  renderer_->SetStaticPictureCache(&cache);
  const auto shadow = MakeShadowPicture();
  const auto frame = MakeFrame(shadow);

  DrawCounter shadow_ops;
  shadow->Dispatch(shadow_ops);
  EXPECT_EQ(shadow_ops.clip_paths, 1u);
  EXPECT_EQ(shadow_ops.round_rects, 1u);
  EXPECT_EQ(shadow_ops.last_path_fill, DlPathFillType::kOdd);

  // Not requested by its layer: always replayed.
  const size_t live_commands = RenderCommandCount(frame);
  EXPECT_EQ(RenderCommandCount(frame), live_commands);
  EXPECT_EQ(cache.GetStatistics().admission_misses, 0u);
  EXPECT_EQ(live_commands, 3u);

  cache.Request(shadow);
  EXPECT_EQ(RenderCommandCount(frame), live_commands);
  EXPECT_EQ(cache.GetStatistics().admission_misses, 1u);
  EXPECT_EQ(RenderCommandCount(frame), 1u);
  EXPECT_EQ(cache.GetStatistics().generations, 1u);
  EXPECT_EQ(RenderCommandCount(frame), 1u);
  EXPECT_EQ(cache.GetStatistics().hits, 1u);

  // With group opacity the image goes through the same opacity handling
  // the live replay gets.
  EXPECT_EQ(RenderCommandCount(MakeFrame(shadow, 0.75f)), 1u);
  EXPECT_EQ(cache.GetStatistics().hits, 2u);

  EXPECT_EQ(RenderTarget::kDefaultColorAttachmentConfig.clear_color,
            Color::BlackTransparent());
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

  const DlMatrix at_fraction = DlMatrix::MakeTranslation({10.25f, 4.5f});
  EXPECT_FALSE(Cached(cache, shadow, at_fraction));
  EXPECT_TRUE(Cached(cache, shadow, at_fraction));
  EXPECT_TRUE(Cached(cache, shadow, DlMatrix::MakeTranslation({11.25f, 5.5f})));
  EXPECT_EQ(generated_sizes.size(), 1u);
  EXPECT_EQ(cache.GetStatistics().hits, 1u);

  EXPECT_FALSE(Cached(cache, shadow, DlMatrix::MakeTranslation({11.5f, 5.5f})));
  EXPECT_FALSE(
      Cached(cache, shadow, DlMatrix::MakeScale(Vector2(1.25f, 1.25f))));
  EXPECT_FALSE(Cached(cache, MakeRectPicture(DlColor::kRed()), DlMatrix()));
  EXPECT_EQ(generated_sizes.size(), 1u);
}

TEST_F(StaticPictureCacheTest, PassTransformParticipatesInScaleAndAlignment) {
  std::vector<ISize> generated_sizes;
  StaticPictureCache cache(*renderer_, StaticPictureCache::Limits(),
                           [&](const sk_sp<DisplayList>&, ISize size) {
                             generated_sizes.push_back(size);
                             return MakeTexture(size);
                           });
  const auto shadow = MakeShadowPicture();
  const DlMatrix canvas_transform = DlMatrix::MakeTranslation({10.0f, 5.0f});
  const DlMatrix output = DlMatrix::MakeTranslation({0.25f, 0.5f}) *
                          DlMatrix::MakeScale(Vector2(2.0f, 2.0f));

  EXPECT_FALSE(Cached(cache, shadow, output * canvas_transform));
  EXPECT_TRUE(Cached(cache, shadow, output * canvas_transform));
  ASSERT_EQ(generated_sizes.size(), 1u);
  EXPECT_EQ(generated_sizes[0], ISize(401, 241));

  EXPECT_TRUE(
      Cached(cache, shadow, output * DlMatrix::MakeTranslation({11.0f, 6.0f})));
  EXPECT_EQ(generated_sizes.size(), 1u);

  const DlMatrix shifted_output = DlMatrix::MakeTranslation({0.5f, 0.5f}) *
                                  DlMatrix::MakeScale(Vector2(2.0f, 2.0f));
  EXPECT_FALSE(Cached(cache, shadow, shifted_output * canvas_transform));
  EXPECT_EQ(generated_sizes.size(), 1u);
}

// The snapshot is rendered in the pass's own pixel coordinates, moved by
// whole pixels only, so pass-space rounding (a non-antialiased rect clip's
// scissor) picks the same pixels as the live replay; the image then lands on
// those pixels one to one.
TEST_F(StaticPictureCacheTest,
       NegativeBoundsMapToIdenticalPixelsWithNonUniformTransform) {
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

  const DlMatrix effective = DlMatrix::MakeTranslation({0.375f, 0.625f}) *
                             DlMatrix::MakeTranslation({-7.25f, 13.5f}) *
                             DlMatrix::MakeScale(Vector2(1.5f, 2.25f));
  const DlIRect rounded = DlIRect::RoundOut(outer.TransformBounds(effective));
  const DlMatrix expected_snapshot_transform =
      DlMatrix::MakeTranslation({-static_cast<float>(rounded.GetLeft()),
                                 -static_cast<float>(rounded.GetTop())}) *
      effective;

  EXPECT_FALSE(cache.Lookup(shadow, effective, {}).has_value());
  const auto image = cache.Lookup(shadow, effective, {});
  ASSERT_TRUE(image.has_value());
  ASSERT_TRUE(snapshot_transform.has_value());
  EXPECT_EQ(snapshot_size, ISize(rounded.GetSize()));
  for (size_t i = 0u; i < 16u; i++) {
    EXPECT_NEAR(snapshot_transform->m[i], expected_snapshot_transform.m[i],
                0.00001f);
  }
  EXPECT_EQ(image->origin, IPoint(rounded.GetLeft(), rounded.GetTop()));
  EXPECT_EQ(image->texture->GetSize(), ISize(rounded.GetSize()));
}

TEST_F(StaticPictureCacheTest, GlesYFlipBuildsOnceAndReusesIntegerMovement) {
  StaticPictureCache cache(*renderer_);
  const auto shadow = MakeShadowPicture();
  const DlMatrix flip = DlMatrix::MakeTranslation({0, 1080}) *
                        DlMatrix::MakeScale(Vector2(1, -1));
  const StaticPictureCache::Pass output{.bottom_up = true};
  const DlMatrix placement = DlMatrix::MakeTranslation({619, 575});
  EXPECT_FALSE(Cached(cache, shadow, flip * placement, output));
  ASSERT_TRUE(Cached(cache, shadow, flip * placement, output));
  EXPECT_TRUE(Cached(cache, shadow, flip * placement, output));
  EXPECT_TRUE(Cached(cache, shadow,
                     flip * DlMatrix::MakeTranslation({620, 576}), output));
  EXPECT_EQ(cache.GetStatistics().generations, 1u);
  EXPECT_EQ(cache.GetStatistics().hits, 2u);
  EXPECT_EQ(cache.GetStatistics().rejected_transforms, 0u);
  EXPECT_EQ(cache.GetEntryCount(), 1u);
  EXPECT_GT(cache.GetResidentBytes(), 0u);
}

TEST_F(StaticPictureCacheTest,
       ReflectionsKeepPassOrientationAndPixelPlacement) {
  const auto shadow = MakeShadowPicture(DlRect::MakeLTRB(-64, -48, 136, 72),
                                        DlRect::MakeLTRB(-28, -24, 100, 48));
  for (float sign_x : {1.0f, -1.0f}) {
    for (float sign_y : {1.0f, -1.0f}) {
      SCOPED_TRACE(::testing::Message() << sign_x << ", " << sign_y);
      std::optional<DlMatrix> snapshot_transform;
      ISize snapshot_size;
      StaticPictureCache cache(
          *renderer_, StaticPictureCache::Limits(),
          [&](const sk_sp<DisplayList>& snapshot, ISize size) {
            DrawCounter ops;
            snapshot->Dispatch(ops);
            snapshot_transform = ops.last_transform;
            snapshot_size = size;
            EXPECT_EQ(ops.clip_paths, 1u);
            EXPECT_EQ(ops.round_rects, 1u);
            return MakeTexture(size);
          });
      const DlMatrix effective =
          DlMatrix::MakeTranslation({0.375f, 1080.625f}) *
          DlMatrix::MakeScale(Vector2(1.5f * sign_x, 2.25f * sign_y)) *
          DlMatrix::MakeTranslation({-7.25f, 13.5f});
      const DlIRect rounded =
          DlIRect::RoundOut(shadow->GetBounds().TransformBounds(effective));
      const DlMatrix origin =
          DlMatrix::MakeTranslation({static_cast<float>(rounded.GetLeft()),
                                     static_cast<float>(rounded.GetTop())});
      EXPECT_FALSE(cache.Lookup(shadow, effective, {}).has_value());
      const auto image = cache.Lookup(shadow, effective, {});
      ASSERT_TRUE(image.has_value());
      ASSERT_TRUE(snapshot_transform.has_value());
      EXPECT_EQ(snapshot_size, ISize(rounded.GetSize()));
      const DlMatrix expected_snapshot = origin.Invert() * effective;
      for (size_t i = 0u; i < 16u; i++) {
        EXPECT_NEAR(snapshot_transform->m[i], expected_snapshot.m[i], 0.0001f);
      }
      EXPECT_EQ(image->origin, IPoint(rounded.GetLeft(), rounded.GetTop()));
      // Follow asymmetric picture points through the snapshot and the
      // image's placement. Comparing only bounding rectangles would miss a
      // double reflection.
      const DlMatrix placed =
          DlMatrix::MakeTranslation({static_cast<float>(image->origin.x),
                                     static_cast<float>(image->origin.y)});
      for (const DlPoint point :
           {DlPoint(-64, -48), DlPoint(136, -48), DlPoint(136, 72),
            DlPoint(-64, 72), DlPoint(11, 27)}) {
        const DlPoint actual = placed * (snapshot_transform.value() * point);
        const DlPoint expected = effective * point;
        EXPECT_NEAR(actual.x, expected.x, 0.0002f);
        EXPECT_NEAR(actual.y, expected.y, 0.0002f);
      }
    }
  }
}

TEST_F(StaticPictureCacheTest,
       ReflectedKeysTrackSignScaleAndPhaseButRejectDegenerateTransforms) {
  StaticPictureCache::Limits limits;
  limits.admission_threshold = 1u;
  StaticPictureCache cache(
      *renderer_, limits,
      [&](const sk_sp<DisplayList>&, ISize size) { return MakeTexture(size); });
  const auto shadow = MakeShadowPicture();
  const DlMatrix flip = DlMatrix::MakeTranslation({0, 1080}) *
                        DlMatrix::MakeScale(Vector2(1, -1));
  const DlMatrix placement = DlMatrix::MakeTranslation({10.25f, 4.5f});
  EXPECT_TRUE(Cached(cache, shadow, flip * placement));
  EXPECT_TRUE(
      Cached(cache, shadow, flip * DlMatrix::MakeTranslation({11.25f, 5.5f})));
  EXPECT_EQ(cache.GetStatistics().generations, 1u);
  EXPECT_EQ(cache.GetStatistics().hits, 1u);
  EXPECT_TRUE(
      Cached(cache, shadow, flip * DlMatrix::MakeTranslation({11.25f, 5.75f})));
  EXPECT_TRUE(Cached(cache, shadow, placement));
  EXPECT_TRUE(Cached(cache, shadow,
                     flip * placement * DlMatrix::MakeScale(Vector2(2, 2))));
  EXPECT_EQ(cache.GetStatistics().generations, 4u);
  EXPECT_FALSE(Cached(cache, shadow, DlMatrix::MakeScale(Vector2(0, -1))));
  EXPECT_FALSE(Cached(cache, shadow, DlMatrix::MakeScale(Vector2(-1, 0))));
  EXPECT_EQ(cache.GetStatistics().rejected_transforms, 2u);
}

// A snapshot is rendered like the pass it lands in, so passes that differ
// in sample count, color format or row order each get their own.
TEST_F(StaticPictureCacheTest, PassPropertiesKeySnapshots) {
  StaticPictureCache::Limits limits;
  limits.admission_threshold = 1u;
  StaticPictureCache cache(
      *renderer_, limits,
      [&](const sk_sp<DisplayList>&, ISize size) { return MakeTexture(size); });
  const auto shadow = MakeShadowPicture();
  using Pass = StaticPictureCache::Pass;
  const Pass passes[] = {
      Pass{},
      Pass{.sample_count = SampleCount::kCount4},
      Pass{.bottom_up = true},
      Pass{.format = PixelFormat::kR16G16B16A16Float},
  };
  for (const Pass& pass : passes) {
    EXPECT_TRUE(Cached(cache, shadow, DlMatrix(), pass));
  }
  EXPECT_EQ(cache.GetStatistics().generations, 4u);
  for (const Pass& pass : passes) {
    EXPECT_TRUE(Cached(cache, shadow, DlMatrix(), pass));
  }
  EXPECT_EQ(cache.GetStatistics().hits, 4u);
  EXPECT_EQ(cache.GetEntryCount(), 4u);
}

TEST_F(StaticPictureCacheTest, DescribesThePassFromItsColorAttachment) {
  StaticPictureCache cache(*renderer_);
  RenderTargetAllocator allocator(
      renderer_->GetContext()->GetResourceAllocator());
  const RenderTarget single =
      allocator.CreateOffscreen(*context_, {16, 16}, 1u, "Single");
  const auto single_pass = cache.DescribePass(single);
  ASSERT_TRUE(single_pass.has_value());
  EXPECT_EQ(single_pass->sample_count, SampleCount::kCount1);
  EXPECT_EQ(
      single_pass->format,
      single.GetColorAttachment(0u).texture->GetTextureDescriptor().format);
  EXPECT_FALSE(single_pass->bottom_up);

  TextureDescriptor descriptor;
  descriptor.storage_mode = StorageMode::kDevicePrivate;
  descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
  descriptor.size = {16, 16};
  descriptor.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  auto texture = renderer_->GetContext()->GetResourceAllocator()->CreateTexture(
      descriptor);
  ASSERT_TRUE(texture);
  TextureGLES::Cast(*texture).SetStoredBottomUp();
  const RenderTarget bottom_up = allocator.CreateOffscreen(
      *context_, {16, 16}, 1u, "Bottom up",
      RenderTarget::kDefaultColorAttachmentConfig,
      RenderTarget::kDefaultStencilAttachmentConfig, texture);
  const auto bottom_up_pass = cache.DescribePass(bottom_up);
  ASSERT_TRUE(bottom_up_pass.has_value());
  EXPECT_TRUE(bottom_up_pass->bottom_up);
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
  EXPECT_TRUE(
      Cached(eviction_cache, MakeRectPicture(DlColor::kRed()), DlMatrix()));
  EXPECT_TRUE(
      Cached(eviction_cache, MakeRectPicture(DlColor::kBlue()), DlMatrix()));
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
  EXPECT_FALSE(
      Cached(budget_cache, MakeRectPicture(DlColor::kGreen()), DlMatrix()));
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
  EXPECT_FALSE(Cached(failure_cache, failed_picture, DlMatrix()));
  EXPECT_EQ(attempts, 1u);
  for (size_t i = 0u; i < 60u; i++) {
    EXPECT_FALSE(Cached(failure_cache, failed_picture, DlMatrix()));
  }
  EXPECT_EQ(attempts, 1u);
  EXPECT_FALSE(Cached(failure_cache, failed_picture, DlMatrix()));
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

  EXPECT_TRUE(Cached(cache, red, DlMatrix()));
  EXPECT_TRUE(Cached(cache, green, DlMatrix()));
  EXPECT_TRUE(Cached(cache, red, DlMatrix()));
  EXPECT_TRUE(Cached(cache, blue, DlMatrix()));
  EXPECT_EQ(cache.GetEntryCount(), 2u);
  EXPECT_EQ(cache.GetResidentBytes(), limits.max_bytes);
  EXPECT_EQ(cache.GetStatistics().evictions, 1u);

  EXPECT_TRUE(Cached(cache, red, DlMatrix()));
  EXPECT_TRUE(Cached(cache, green, DlMatrix()));
  EXPECT_EQ(snapshots, 4u);
  EXPECT_EQ(cache.GetStatistics().hits, 2u);
  EXPECT_EQ(cache.GetStatistics().evictions, 2u);
}

TEST_F(StaticPictureCacheTest, DropsEntriesWhosePictureIsGone) {
  StaticPictureCache::Limits limits;
  limits.admission_threshold = 1u;
  StaticPictureCache cache(
      *renderer_, limits,
      [&](const sk_sp<DisplayList>&, ISize size) { return MakeTexture(size); });
  auto red = MakeRectPicture(DlColor::kRed());
  const auto green = MakeRectPicture(DlColor::kGreen());

  EXPECT_TRUE(Cached(cache, red, DlMatrix()));
  EXPECT_TRUE(Cached(cache, green, DlMatrix()));
  cache.Collect();
  EXPECT_EQ(cache.GetEntryCount(), 2u);
  EXPECT_EQ(cache.GetStatistics().collected, 0u);

  // The framework re-recorded the picture and let the old one go.
  red.reset();
  cache.Collect();
  EXPECT_EQ(cache.GetEntryCount(), 1u);
  EXPECT_EQ(cache.GetResidentBytes(), 32u * 32u * 4u);
  EXPECT_EQ(cache.GetStatistics().collected, 1u);
  EXPECT_EQ(cache.GetStatistics().evictions, 0u);

  EXPECT_TRUE(Cached(cache, green, DlMatrix()));
  EXPECT_EQ(cache.GetStatistics().hits, 1u);
}

TEST_F(StaticPictureCacheTest, DropsKeysAPictureLeftBehind) {
  StaticPictureCache::Limits limits;
  limits.admission_threshold = 1u;
  limits.stale_key_draws = 3u;
  StaticPictureCache cache(
      *renderer_, limits,
      [&](const sk_sp<DisplayList>&, ISize size) { return MakeTexture(size); });
  auto red = MakeRectPicture(DlColor::kRed());
  const DlMatrix moved = DlMatrix::MakeTranslation({0.5f, 0.0f});

  EXPECT_TRUE(Cached(cache, red, DlMatrix()));
  EXPECT_TRUE(Cached(cache, red, moved));
  EXPECT_EQ(cache.GetEntryCount(), 2u);

  // Two outputs showing the picture use both entries in turn.
  for (int frame = 0; frame < 8; frame++) {
    EXPECT_TRUE(Cached(cache, red, DlMatrix()));
    EXPECT_TRUE(Cached(cache, red, moved));
    cache.Collect();
  }
  EXPECT_EQ(cache.GetEntryCount(), 2u);

  // The picture now rests at one of them. The other entry is three draws
  // behind after two more frames: the last frame both were drawn in drew
  // it first.
  for (int frame = 0; frame < 2; frame++) {
    EXPECT_TRUE(Cached(cache, red, moved));
    cache.Collect();
  }
  EXPECT_EQ(cache.GetEntryCount(), 2u);
  EXPECT_TRUE(Cached(cache, red, moved));
  cache.Collect();
  EXPECT_EQ(cache.GetEntryCount(), 1u);
  EXPECT_EQ(cache.GetStatistics().stale_keys, 1u);
  EXPECT_EQ(cache.GetStatistics().collected, 0u);

  // Every entry of a picture goes with it, however many it had.
  EXPECT_TRUE(Cached(cache, red, DlMatrix()));
  EXPECT_EQ(cache.GetEntryCount(), 2u);
  red.reset();
  cache.Collect();
  EXPECT_EQ(cache.GetEntryCount(), 0u);
  EXPECT_EQ(cache.GetResidentBytes(), 0u);
  EXPECT_EQ(cache.GetStatistics().collected, 2u);
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
  EXPECT_FALSE(Cached(cache, image, DlMatrix()));
  EXPECT_FALSE(Cached(cache, image, DlMatrix()));
  EXPECT_EQ(cache.GetStatistics().eligibility_checks, 1u);
  EXPECT_EQ(cache.GetStatistics().rejected_pictures, 1u);
}

}  // namespace
}  // namespace testing
}  // namespace impeller
