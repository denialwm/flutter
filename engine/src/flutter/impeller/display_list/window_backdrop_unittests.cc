// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/effects/image_filters/dl_blur_image_filter.h"
#include "flutter/display_list/effects/image_filters/dl_glass_image_filter.h"
#include "impeller/display_list/image_filter.h"
#include "impeller/entity/contents/filters/gaussian_blur_filter_contents.h"
#include "impeller/entity/contents/filters/glass_filter_contents.h"

#include "gtest/gtest.h"
#include "impeller/core/allocator.h"
#include "impeller/entity/gles/entity_shaders_gles.h"
#include "impeller/entity/gles/framebuffer_blend_shaders_gles.h"
#include "impeller/entity/gles/modern_shaders_gles.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/proc_table_gles.h"
#include "impeller/renderer/backend/gles/reactor_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"

namespace impeller {
namespace testing {
namespace {
class WindowBackdropTest : public ::testing::Test {
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
    TextureDescriptor desc;
    desc.format = PixelFormat::kR8G8B8A8UNormInt;
    desc.size = ISize(200, 100);
    desc.usage = TextureUsage::kShaderRead;
    texture_ =
        renderer_->GetContext()->GetResourceAllocator()->CreateTexture(desc);
    ASSERT_TRUE(texture_);
  }
  std::shared_ptr<MockGLES> mock_;
  std::shared_ptr<ContextGLES> context_;
  std::shared_ptr<Worker> worker_;
  std::unique_ptr<ContentContext> renderer_;
  std::shared_ptr<Texture> texture_;
};

TEST_F(WindowBackdropTest, SourceTextureContainsOnlyWindowPixels) {
  const Rect window = Rect::MakeLTRB(55, 10, 96, 90);
  auto cropped = CropWindowBackdrop(*renderer_, texture_, window);
  ASSERT_TRUE(cropped);
  EXPECT_NE(cropped->texture, texture_);
  EXPECT_EQ(cropped->texture->GetSize(), ISize(41, 80));
  EXPECT_EQ(cropped->GetCoverage(), window);
  auto input = FilterInput::Make(cropped->texture, cropped->transform);
  Entity entity;
  entity.SetTransform(Matrix::MakeTranslation(Vector2(-5, -3)));
  EXPECT_EQ(input->GetCoverage(entity), window.Shift(Vector2(-5, -3)));
}

TEST_F(WindowBackdropTest,
       TranslatedCropKeepsBlurSamplingInTextureCoordinates) {
  auto cropped =
      CropWindowBackdrop(*renderer_, texture_, Rect::MakeLTRB(55, 10, 96, 90));
  ASSERT_TRUE(cropped);
  const auto size = cropped->texture->GetSize();
  for (const Rect source :
       {Rect::MakeSize(size), Rect::MakeSize(size).Expand(Vector2(8, 8))}) {
    const auto uvs = GaussianBlurFilterContents::CalculateUVs(source, size);
    const auto points = source.GetPoints();
    for (size_t i = 0; i < uvs.size(); i++) {
      EXPECT_FLOAT_EQ(uvs[i].x, points[i].x / size.width);
      EXPECT_FLOAT_EQ(uvs[i].y, points[i].y / size.height);
    }
  }
  // A black pixel three quarters across the crop must still sample at 0.75,
  // not beyond 1.0 where clamp repeats the crop's rightmost pixel.
  const auto uvs =
      GaussianBlurFilterContents::CalculateUVs(Rect::MakeSize(size), size);
  EXPECT_FLOAT_EQ(uvs[0].x * 0.25f + uvs[1].x * 0.75f, 0.75f);
}

TEST_F(WindowBackdropTest, CroppedFilterCoversWindowThroughCacheAndRootFlip) {
  const Rect window = Rect::MakeLTRB(55.25f, 10.5f, 195.75f, 97.5f);
  for (bool glass : {false, true}) {
    for (bool flipped : {false, true}) {
      SCOPED_TRACE(glass);
      SCOPED_TRACE(flipped);
      const Matrix transform = flipped
                                   ? Matrix::MakeTranslation(Vector2(0, 100)) *
                                         Matrix::MakeScale(Vector2(1, -1))
                                   : Matrix();
      const Rect coverage = window.TransformBounds(transform);
      auto cropped = CropWindowBackdrop(*renderer_, texture_, coverage);
      ASSERT_TRUE(cropped);
      std::shared_ptr<flutter::DlImageFilter> filter =
          glass ? flutter::DlGlassImageFilter::Make(
                      7, 7, RoundRect::MakeRectXY(window, 8, 8), 1, 27, 0.52f,
                      0.51f, 1, flutter::DlColor::kTransparent(), 0, 1, 0, 1, 1,
                      0.65f, true)
                : flutter::DlBlurImageFilter::Make(
                      6, 6, flutter::DlTileMode::kClamp, std::nullopt, 1, 0.65f,
                      true);
      auto contents =
          WrapInput(*renderer_, filter.get(),
                    FilterInput::Make(cropped->texture, cropped->transform));
      if (glass) {
        std::static_pointer_cast<GlassFilterContents>(contents)
            ->SetMaterialTransform(transform);
      }
      contents->SetEffectTransform(transform.Basis());
      contents->SetIsBackdropFilter(true);
      for (auto mode :
           {Entity::RenderingMode::kDirect,
            Entity::RenderingMode::kSubpassPrependSnapshotTransform,
            Entity::RenderingMode::kSubpassAppendSnapshotTransform}) {
        contents->SetRenderingMode(mode);
        auto snapshot = contents->RenderToSnapshot(
            *renderer_, Entity{}, {.coverage_limit = coverage});
        ASSERT_TRUE(snapshot);
        ASSERT_TRUE(snapshot->GetCoverage());
        EXPECT_TRUE(snapshot->GetCoverage()->Contains(coverage));
      }
    }
  }
}

TEST_F(WindowBackdropTest, FractionalWindowRoundsOutAndClampsToSource) {
  auto cropped = CropWindowBackdrop(
      *renderer_, texture_, Rect::MakeLTRB(55.25f, -2.5f, 195.75f, 97.5f));
  ASSERT_TRUE(cropped);
  EXPECT_EQ(cropped->texture->GetSize(), ISize(141, 98));
  EXPECT_EQ(cropped->GetCoverage(), Rect::MakeLTRB(55, 0, 196, 98));
  EXPECT_FALSE(CropWindowBackdrop(*renderer_, texture_,
                                  Rect::MakeLTRB(201, 0, 220, 20)));
}
}  // namespace
}  // namespace testing
}  // namespace impeller
