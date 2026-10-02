// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/common/backdrop_filter_cache_key.h"
#include "flutter/display_list/effects/image_filters/dl_blur_image_filter.h"
#include "flutter/display_list/effects/image_filters/dl_glass_image_filter.h"
#include "impeller/display_list/canvas.h"
#include "impeller/display_list/image_filter.h"
#include "impeller/entity/contents/filters/gaussian_blur_filter_contents.h"
#include "impeller/entity/contents/filters/glass_filter_contents.h"
#include "impeller/entity/window_surface.frag.h"
#include "impeller/entity/window_surface.vert.h"
#include "impeller/entity/window_surface_texture.frag.h"
#include "impeller/renderer/backend/gles/device_buffer_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"

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

TEST_F(WindowBackdropTest,
       BackdropSnapshotOwnerRetirementPreservesOtherViewsAndPins) {
  auto& context = *renderer_;
  TextureDescriptor descriptor;
  descriptor.size = {4, 4};
  descriptor.format = context.GetDeviceCapabilities().GetDefaultColorFormat();
  descriptor.usage = TextureUsage::kRenderTarget;
  descriptor.storage_mode = StorageMode::kDevicePrivate;
  auto texture =
      context.GetContext()->GetResourceAllocator()->CreateTexture(descriptor);
  ASSERT_TRUE(texture);
  const int64_t removed = flutter::MakeBackdropFilterCacheKey(7u, 1u);
  const int64_t retained = flutter::MakeBackdropFilterCacheKey(8u, 1u);
  auto owner = std::make_shared<int>(7);
  auto other_view = std::make_shared<int>(8);
  auto previous_tree = owner;
  context.RegisterBackdropSnapshotOwner(removed, owner);
  context.RegisterBackdropSnapshotOwner(retained, other_view);
  auto removed_texture =
      context.GetContext()->GetResourceAllocator()->CreateTexture(descriptor);
  ASSERT_TRUE(removed_texture);
  std::weak_ptr<Texture> removed_texture_lifetime = removed_texture;
  context.CacheBackdropSnapshot(removed, Snapshot{.texture = removed_texture});
  removed_texture.reset();
  context.CacheBackdropSnapshot(retained, Snapshot{.texture = texture});
  context.CacheBackdropSnapshot(17, Snapshot{.texture = texture});
  // One view releasing its tree cannot evict a family another tree retains.
  owner.reset();
  context.PruneExpiredBackdropSnapshots();
  EXPECT_TRUE(context.GetCachedBackdropSnapshot(removed).has_value());
  {
    BackdropSnapshotPins pins(context);
    ASSERT_TRUE(pins.Pin(removed, Rect::MakeWH(4, 4)));
    previous_tree.reset();
    context.PruneExpiredBackdropSnapshots();
    // The cache's reference is gone, but this submission still sees its pin.
    EXPECT_TRUE(context.GetCachedBackdropSnapshot(removed).has_value());
    EXPECT_FALSE(removed_texture_lifetime.expired());
  }
  EXPECT_FALSE(context.GetCachedBackdropSnapshot(removed).has_value());
  EXPECT_TRUE(removed_texture_lifetime.expired());
  EXPECT_TRUE(context.GetCachedBackdropSnapshot(retained).has_value());
  EXPECT_TRUE(context.GetCachedBackdropSnapshot(17).has_value());
  other_view.reset();
  context.PruneExpiredBackdropSnapshots();
  EXPECT_FALSE(context.GetCachedBackdropSnapshot(retained).has_value());
}

TEST_F(WindowBackdropTest,
       BackdropSnapshotRetirementDrainsGLESCollectionAtIdle) {
  auto reactor = context_->GetReactor();
  auto handle = reactor->CreateHandle(HandleType::kTexture);
  ASSERT_FALSE(handle.IsDead());
  auto collected = std::make_shared<bool>(false);
  ASSERT_TRUE(reactor->RegisterCleanupCallback(
      handle, [collected] { *collected = true; }));
  TextureDescriptor descriptor;
  descriptor.size = {4, 4};
  descriptor.format = PixelFormat::kR8G8B8A8UNormInt;
  descriptor.usage = TextureUsage::kShaderRead;
  auto texture = TextureGLES::WrapTexture(reactor, descriptor, handle);
  ASSERT_TRUE(texture);
  auto owner = std::make_shared<int>(7);
  const int64_t key = flutter::MakeBackdropFilterCacheKey(7u, 1u);
  renderer_->RegisterBackdropSnapshotOwner(key, owner);
  renderer_->CacheBackdropSnapshot(key, Snapshot{.texture = texture});
  texture.reset();
  owner.reset();
  EXPECT_FALSE(*collected);
  renderer_->PruneExpiredBackdropSnapshots();
  // No new render pass or command submission is needed to collect the handle.
  EXPECT_TRUE(*collected);
  EXPECT_FALSE(renderer_->GetCachedBackdropSnapshot(key).has_value());
}

TEST_F(WindowBackdropTest,
       BackdropSnapshotOwnerChangeRetiresGenerationBeforeMaterializing) {
  auto& context = *renderer_;
  TextureDescriptor descriptor;
  descriptor.size = {4, 4};
  descriptor.format = context.GetDeviceCapabilities().GetDefaultColorFormat();
  descriptor.usage = TextureUsage::kRenderTarget;
  descriptor.storage_mode = StorageMode::kDevicePrivate;
  auto texture =
      context.GetContext()->GetResourceAllocator()->CreateTexture(descriptor);
  ASSERT_TRUE(texture);
  auto owner = std::make_shared<int>(7);
  const int64_t first = flutter::MakeBackdropFilterCacheKey(7u, 1u);
  const int64_t second = flutter::MakeBackdropFilterCacheKey(7u, 2u);
  context.RegisterBackdropSnapshotOwner(first, owner);
  context.CacheBackdropSnapshot(first, Snapshot{.texture = texture});
  context.RegisterBackdropSnapshotOwner(second, owner);
  EXPECT_FALSE(context.GetCachedBackdropSnapshot(first).has_value());
  EXPECT_FALSE(context.GetCachedBackdropSnapshot(second).has_value());
  EXPECT_FALSE(context.ShouldMaterializeBackdropSnapshot(second));
}

TEST_F(WindowBackdropTest, ExplicitWindowNeverUsesClipOrBorderDraws) {
  for (bool direct : {true, false}) {
    for (bool image : {true, false}) {
      for (bool blur : {true, false}) {
        SCOPED_TRACE(direct);
        SCOPED_TRACE(image);
        SCOPED_TRACE(blur);
        TextureDescriptor desc;
        desc.size = {200, 100};
        desc.format = PixelFormat::kR8G8B8A8UNormInt;
        desc.usage = TextureUsage::kRenderTarget;
        desc.storage_mode = StorageMode::kDevicePrivate;
        ColorAttachment color;
        color.texture =
            renderer_->GetContext()->GetResourceAllocator()->CreateTexture(
                desc);
        ASSERT_TRUE(color.texture);
        color.load_action = LoadAction::kClear;
        RenderTarget target;
        target.SetColorAttachment(color, 0);
        Canvas canvas(*renderer_, target, false, blur);
        canvas.SetBackdropData({}, blur ? 1 : 0);
        flutter::DlWindowSurfaceFilter::Style style{
            Rect::MakeLTRB(20, 10, 180, 90), Rect::MakeLTRB(22, 12, 178, 88),
            18, flutter::DlColor::kRed()};
        auto backdrop = blur ? flutter::DlBlurImageFilter::Make(
                                   4, 4, flutter::DlTileMode::kClamp,
                                   std::nullopt, 1, 0.6f, false)
                             : nullptr;
        flutter::DlWindowSurfaceFilter window(style, backdrop, direct);
        Paint restore;
        restore.blend_mode = BlendMode::kSrc;
        canvas.SaveLayer(restore, style.bounds, &window,
                         ContentBoundsPromise::kUnknown, image ? 1 : 0);
        if (image) {
          // Strict source + oversized buffer deliberately rejected the old
          // opportunistic fusion. Neither can reject an explicit WindowSurface.
          canvas.DrawImageRect(texture_, Rect::MakeSize(texture_->GetSize()),
                               Rect::MakeLTRB(-10, -20, 210, 110), {}, {},
                               SourceRectConstraint::kStrict, true);
        }
        canvas.Restore();
        const auto& commands = canvas.GetRenderPassForTesting().GetCommands();
        ASSERT_EQ(commands.size(), 1u);
        EXPECT_EQ(commands[0].audit_category,
                  CommandAuditCategory::kBackdropSurfaceComposite);
        EXPECT_EQ(commands[0].element_count, 4u);
        EXPECT_EQ(commands[0].bound_textures.length, 2u);
        canvas.EndReplay();
      }
    }
  }
}

TEST_F(WindowBackdropTest, WindowMaterialIsOneUnpaddedUniformUpload) {
  for (const auto* metadata :
       {&WindowSurfaceFragmentShader::kMetadataFragInfo,
        &WindowSurfaceTextureFragmentShader::kMetadataFragInfo}) {
    ASSERT_EQ(metadata->members.size(), 1u);
    const auto& data = metadata->members[0];
    EXPECT_EQ(data.type, ShaderType::kFloat);
    EXPECT_EQ(data.float_type, ShaderFloatType::kVec4);
    EXPECT_EQ(data.array_elements, 8u);
    EXPECT_EQ(data.size, 16u);
    EXPECT_EQ(data.offset, 0u);
    EXPECT_EQ(data.byte_length, 128u);
  }
  EXPECT_EQ(sizeof(WindowSurfaceFragmentShader::FragInfo), 128u);
  EXPECT_EQ(sizeof(WindowSurfaceTextureFragmentShader::FragInfo), 128u);
}

TEST_F(WindowBackdropTest, WindowSamplingSurvivesAllBufferTransforms) {
  using VS = WindowSurfaceVertexShader;
  using FS = WindowSurfaceTextureFragmentShader;
  const auto read_buffer = [](const BufferView& view) {
    return DeviceBufferGLES::Cast(*view.GetBuffer()).GetBufferData() +
           view.GetRange().offset;
  };
  for (bool flip_y : {false, true}) {
    const std::shared_ptr<Texture> sampled_texture =
        flip_y ? TextureGLES::WrapFBOTexture(context_->GetReactor(),
                                             texture_->GetTextureDescriptor(),
                                             1, 2)
               : texture_;
    for (int orientation = 0; orientation < 8; ++orientation) {
      SCOPED_TRACE(flip_y);
      SCOPED_TRACE(orientation);
      TextureDescriptor desc;
      desc.size = {200, 100};
      desc.format = PixelFormat::kR8G8B8A8UNormInt;
      desc.usage = TextureUsage::kRenderTarget;
      desc.storage_mode = StorageMode::kDevicePrivate;
      ColorAttachment color;
      color.texture =
          renderer_->GetContext()->GetResourceAllocator()->CreateTexture(desc);
      ASSERT_TRUE(color.texture);
      color.load_action = LoadAction::kClear;
      RenderTarget target;
      target.SetColorAttachment(color, 0);
      Canvas canvas(*renderer_, target, false, false);
      flutter::DlWindowSurfaceFilter::Style style{
          Rect::MakeLTRB(20, 10, 180, 90), Rect::MakeLTRB(22, 12, 178, 88), 18,
          flutter::DlColor::kRed()};
      const Matrix window_to_scene =
          Matrix::MakeTranslation(Vector2(3.25f, 90.5f)) *
          Matrix::MakeScale(Vector2(0.8f, -0.9f));
      const Matrix buffer_to_window =
          Matrix::MakeTranslation(Vector2(100, 50)) *
          Matrix::MakeRotationZ(Radians((orientation & 3) * kPiOver2)) *
          Matrix::MakeScale(Vector2(orientation & 4 ? -1.25f : 1.25f, 0.75f)) *
          Matrix::MakeTranslation(Vector2(-100, -50));
      const Rect source = Rect::MakeLTRB(8, 4, 190, 96);
      const Rect destination = Rect::MakeLTRB(-10, -20, 210, 110);
      flutter::DlWindowSurfaceFilter window(style, nullptr, true);
      Paint restore;
      restore.blend_mode = BlendMode::kSrc;
      restore.color.alpha = 0.8f;
      canvas.Transform(window_to_scene);
      canvas.SaveLayer(restore, style.bounds, &window,
                       ContentBoundsPromise::kUnknown, 1);
      canvas.Transform(buffer_to_window);
      Paint surface;
      surface.color.alpha = 0.6f;
      canvas.DrawImageRect(sampled_texture, source, destination, surface, {},
                           SourceRectConstraint::kStrict, true);
      canvas.Restore();
      const auto& pass = canvas.GetRenderPassForTesting();
      ASSERT_EQ(pass.GetCommands().size(), 1u);
      const auto& command = pass.GetCommands()[0];
      const auto* vertices = reinterpret_cast<
          const VS::PerVertexData*>(read_buffer(
          pass.GetVertexBuffersForTesting()[command.vertex_buffers.offset]));
      for (size_t i = 0; i < 4; ++i) {
        const Point input = buffer_to_window.Invert() * vertices[i].position;
        const Point pixel =
            source.GetOrigin() + (input - destination.GetOrigin()) *
                                     (source.GetSize() / destination.GetSize());
        EXPECT_NEAR(vertices[i].surface_uv.x, pixel.x / 200, 0.00001f);
        EXPECT_NEAR(vertices[i].surface_uv.y,
                    flip_y ? 1 - pixel.y / 100 : pixel.y / 100, 0.00001f);
      }
      bool checked_material = false;
      for (const auto& binding : pass.GetBoundBuffersForTesting()) {
        if (binding.GetMetadata() != &FS::kMetadataFragInfo) {
          continue;
        }
        const auto* material = reinterpret_cast<const FS::FragInfo*>(
            read_buffer(binding.resource));
        EXPECT_EQ(material->data[0], Vector4(style.bounds.GetLTRB()));
        EXPECT_EQ(material->data[1], Vector4(style.content_bounds.GetLTRB()));
        EXPECT_EQ(material->data[4], Vector4(1, 0, 0, 1));
        EXPECT_NEAR(material->data[5].x, 18, 0.00001f);
        EXPECT_NEAR(material->data[5].y, 0.8f, 0.00001f);
        EXPECT_NEAR(material->data[5].z, 0.6f, 0.00001f);
        EXPECT_EQ(material->data[6], Vector4(0, 0, 1, 0));
        EXPECT_EQ(material->data[7], Vector4());
        checked_material = true;
      }
      EXPECT_TRUE(checked_material);
      canvas.EndReplay();
    }
  }
}

TEST_F(WindowBackdropTest, ExplicitWindowReusesInsetGlassAndBlurAcrossFrames) {
  int64_t key = 100;
  for (bool glass : {false, true}) {
    for (bool direct : {true, false}) {
      for (Scalar border : {0.0f, 2.0f}) {
        for (int variant = 0; variant < 4; ++variant) {
          const bool transformed = variant & 1;
          const bool material_bounds = variant & 2;
          SCOPED_TRACE(glass);
          SCOPED_TRACE(direct);
          SCOPED_TRACE(border);
          SCOPED_TRACE(transformed);
          SCOPED_TRACE(material_bounds);
          ++key;
          flutter::DlWindowSurfaceFilter::Style style{
              Rect::MakeLTRB(20.25f, 10.5f, 179.75f, 89.5f),
              {},
              18,
              flutter::DlColor::kRed()};
          style.content_bounds = style.bounds.Expand(-border);
          if (material_bounds) {
            // A popup's window geometry inside its client-drawn shadow.
            style.material_bounds =
                Rect::MakeLTRB(40.5f, 25.25f, 150.75f, 70.5f);
          }
          std::shared_ptr<flutter::DlImageFilter> filter =
              glass ? flutter::DlGlassImageFilter::Make(
                          7, 7,
                          RoundRect::MakeRectXY(style.content_bounds, 8, 8), 1,
                          27, 0.52f, 0.51f, 1, flutter::DlColor::kTransparent(),
                          0, 1, 0, 1, 1, 0.65f, false)
                    : flutter::DlBlurImageFilter::Make(
                          6, 6, flutter::DlTileMode::kClamp, std::nullopt, 1,
                          0.65f, false);
          flutter::DlWindowSurfaceFilter window(style, filter, direct);
          const Matrix transform =
              transformed ? Matrix::MakeTranslation(Vector2(3, 98)) *
                                Matrix::MakeScale(Vector2(0.75f, -0.75f))
                          : Matrix();
          std::shared_ptr<Texture> cached_texture;
          for (int frame = 0; frame < 3; ++frame) {
            SCOPED_TRACE(frame);
            TextureDescriptor desc;
            desc.size = {200, 100};
            desc.format = PixelFormat::kR8G8B8A8UNormInt;
            desc.usage = TextureUsage::kRenderTarget;
            desc.storage_mode = StorageMode::kDevicePrivate;
            ColorAttachment color;
            color.texture =
                renderer_->GetContext()->GetResourceAllocator()->CreateTexture(
                    desc);
            ASSERT_TRUE(color.texture);
            color.load_action = LoadAction::kClear;
            RenderTarget target;
            target.SetColorAttachment(color, 0);
            Canvas canvas(*renderer_, target, false, true);
            canvas.SetBackdropData({{key, BackdropData{.backdrop_count = 1}}},
                                   1);
            canvas.Transform(transform);
            Paint restore;
            restore.blend_mode = BlendMode::kSrc;
            canvas.SaveLayer(restore, style.bounds, &window,
                             ContentBoundsPromise::kUnknown, 1, false, key);
            canvas.DrawImageRect(texture_, Rect::MakeSize(texture_->GetSize()),
                                 Rect::MakeLTRB(-10, -20, 210, 110), {}, {},
                                 SourceRectConstraint::kStrict, true);
            canvas.Restore();
            auto cached = renderer_->GetCachedBackdropSnapshot(key);
            ASSERT_TRUE(cached);
            ASSERT_TRUE(cached->GetCoverage());
            const Rect needed =
                style.material_bounds.value_or(style.content_bounds)
                    .TransformBounds(transform);
            EXPECT_TRUE(cached->GetCoverage()->Contains(needed));
            // The material is never evaluated outside its bounds.
            EXPECT_EQ(cached->GetCoverage()->Contains(
                          style.content_bounds.TransformBounds(transform)),
                      !material_bounds);
            const auto& pass = canvas.GetRenderPassForTesting();
            const auto& command = pass.GetCommands().back();
            ASSERT_EQ(command.audit_category,
                      CommandAuditCategory::kBackdropSurfaceComposite);
            bool checked_material = false;
            for (const auto& binding : pass.GetBoundBuffersForTesting()) {
              if (binding.GetMetadata() !=
                  &WindowSurfaceTextureFragmentShader::kMetadataFragInfo) {
                continue;
              }
              const auto* info = reinterpret_cast<
                  const WindowSurfaceTextureFragmentShader::FragInfo*>(
                  DeviceBufferGLES::Cast(*binding.resource.GetBuffer())
                      .GetBufferData() +
                  binding.resource.GetRange().offset);
              EXPECT_EQ(info->data[6].w, material_bounds ? 1 : 0);
              EXPECT_EQ(info->data[7],
                        material_bounds
                            ? Vector4(style.material_bounds->GetLTRB())
                            : Vector4());
              checked_material = true;
            }
            EXPECT_TRUE(checked_material);
            const auto& view =
                pass.GetVertexBuffersForTesting()[command.vertex_buffers
                                                      .offset];
            const auto* vertices = reinterpret_cast<
                const WindowSurfaceVertexShader::PerVertexData*>(
                DeviceBufferGLES::Cast(*view.GetBuffer()).GetBufferData() +
                view.GetRange().offset);
            const Matrix window_to_snapshot =
                cached->transform.Invert() * transform;
            const auto snapshot_size = cached->texture->GetSize();
            for (size_t i = 0; i < 4; ++i) {
              const Point pixel = window_to_snapshot * vertices[i].position;
              EXPECT_NEAR(vertices[i].backdrop_uv.x,
                          pixel.x / snapshot_size.width, 0.00001f);
              const Scalar y = pixel.y / snapshot_size.height;
              EXPECT_NEAR(vertices[i].backdrop_uv.y,
                          cached->texture->GetYCoordScale() < 0 ? 1 - y : y,
                          0.00001f);
            }
            canvas.EndReplay();
            if (cached_texture) {
              EXPECT_EQ(cached->texture, cached_texture)
                  << "Unchanged glass must not be rebuilt for the outer frame";
            }
            cached_texture = cached->texture;
            BackdropSnapshotPins pins(*renderer_);
            EXPECT_TRUE(pins.Pin(key, needed));
          }
        }
      }
    }
  }
}

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
