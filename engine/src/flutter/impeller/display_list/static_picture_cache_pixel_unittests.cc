// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Pixel tests for the static picture cache on a real GLES implementation
// (ANGLE on SwiftShader): a picture drawn from the cache must come out with
// the same pixels as the picture replayed live.

#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/effects/dl_color_filter.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/impeller/entity/gles/entity_shaders_gles.h"
#include "flutter/impeller/entity/gles/framebuffer_blend_shaders_gles.h"
#include "flutter/impeller/entity/gles/modern_shaders_gles.h"
#include "flutter/testing/test_gl_context.h"
#include "flutter/testing/test_gl_surface.h"
#include "gtest/gtest.h"
#include "impeller/core/device_buffer_descriptor.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/static_picture_cache.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/renderer/backend/gles/capabilities_gles.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/device_buffer_gles.h"
#include "impeller/renderer/backend/gles/proc_table_gles.h"
#include "impeller/renderer/backend/gles/reactor_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"
#include "impeller/renderer/blit_pass.h"
#include "impeller/renderer/command_buffer.h"
#include "impeller/renderer/command_queue.h"
#include "impeller/renderer/render_target.h"

namespace impeller {
namespace testing {
namespace {

using namespace flutter;

constexpr int kSize = 80;

// Denial's GLES output: the root canvas reaches the render target through a
// Y reflection (embedder_external_view_embedder.cc), and the root pass draws
// into a borrowed FBO without the backend's offscreen vertex flip.
DlMatrix ReflectedRoot() {
  return DlMatrix::MakeRow(1, 0, 0, 0,       //
                           0, -1, 0, kSize,  //
                           0, 0, 1, 0,       //
                           0, 0, 0, 1);
}

// The shell's scale on the OnePlus Pad 4, at an offset that keeps every edge
// below on half a pixel.
DlMatrix CanvasTransform() {
  return DlMatrix::MakeTranslation({4.0f, 2.0f}) *
         DlMatrix::MakeScale({1.5f, 1.5f, 1.0f});
}

// Hard edges that pass through pixel centres at scale 1.5: a non-antialiased
// clip rect (a scissor), a non-antialiased fill, and a non-antialiased
// even-odd path clip (a stencil), as in the window shadow's bands.
sk_sp<DisplayList> MakeHardEdgedPicture() {
  DisplayListBuilder builder(DlRect::MakeWH(48, 48));
  builder.Save();
  builder.ClipRect(DlRect::MakeLTRB(1, 1, 39, 7), DlClipOp::kIntersect,
                   /*is_aa=*/false);
  builder.DrawRect(DlRect::MakeWH(48, 48),
                   DlPaint().setColor(DlColor::kRed().withAlphaF(0.5f)));
  builder.Restore();

  builder.DrawRect(DlRect::MakeLTRB(3, 13, 37, 21),
                   DlPaint().setColor(DlColor::kGreen()));

  DlPathBuilder frame;
  frame.SetFillType(DlPathFillType::kOdd)
      .AddRect(DlRect::MakeLTRB(1, 23, 45, 47))
      .AddRoundRect(
          DlRoundRect::MakeRectRadius(DlRect::MakeLTRB(5, 25, 41, 39), 4));
  builder.Save();
  builder.ClipPath(frame.TakePath(), DlClipOp::kIntersect, /*is_aa=*/false);
  builder.DrawRect(DlRect::MakeWH(48, 48),
                   DlPaint().setColor(DlColor::kBlue().withAlphaF(0.75f)));
  builder.Restore();
  return builder.Build();
}

// A window shadow's falloff: black bands at low coverage, the levels a
// 2-bit alpha cannot hold. Dyadic alphas keep every blend exact.
sk_sp<DisplayList> MakeSoftShadowPicture() {
  DisplayListBuilder builder(DlRect::MakeWH(48, 48));
  const float alphas[] = {0.0625f, 0.125f, 0.25f, 0.375f, 0.5f, 0.8125f};
  for (int band = 0; band < 6; band++) {
    builder.DrawRect(
        DlRect::MakeLTRB(2, 2 + band * 7, 46, 8 + band * 7),
        DlPaint().setColor(DlColor::kBlack().withAlphaF(alphas[band])));
  }
  return builder.Build();
}

class StaticPictureCachePixelTest : public ::testing::Test {
 protected:
  class Worker final : public ReactorGLES::Worker {
    bool CanReactorReactOnCurrentThreadNow(const ReactorGLES&) const override {
      return true;
    }
  };

  void SetUp() override {
    surface_ = std::make_unique<flutter::testing::TestGLOnscreenOnlySurface>(
        std::make_shared<flutter::testing::TestEGLContext>(), DlISize(1, 1));
    ASSERT_TRUE(surface_->MakeCurrent());
    auto* surface = surface_.get();
    auto procs = std::make_unique<ProcTableGLES>(
        [surface](const char* name) { return surface->GetProcAddress(name); });
    ASSERT_TRUE(procs->IsValid());
    context_ = ContextGLES::Create(
        Flags{}, std::move(procs),
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
    ASSERT_TRUE(context_ && Base().IsValid());
    worker_ = std::make_shared<Worker>();
    context_->AddReactorWorker(worker_);
    renderer_ = std::make_unique<ContentContext>(context_, nullptr);
    ASSERT_TRUE(renderer_->IsValid());
  }

  void TearDown() override {
    renderer_.reset();
    if (context_) {
      Base().Shutdown();
    }
    context_.reset();
    surface_.reset();
  }

  // Where the frame's root canvas puts the picture.
  enum class Placement {
    // Straight into the root pass.
    kRoot,
    // Inside a save layer with a color filter: a real offscreen pass.
    kFilteredLayer,
    // With group opacity, as an OpacityLayer above a picture records it.
    kGroupOpacity,
  };

  // The frame's root canvas, as the layer tree records it: the picture as a
  // nested DisplayList under the canvas transform.
  static sk_sp<DisplayList> MakeFrame(const sk_sp<DisplayList>& picture,
                                      Placement placement,
                                      bool backdrop) {
    DisplayListBuilder frame(DlRect::MakeWH(kSize, kSize));
    if (backdrop) {
      // What a shadow lands on: an opaque wallpaper.
      frame.DrawRect(DlRect::MakeWH(kSize, kSize),
                     DlPaint().setColor(DlColor::RGBA(0.8f, 0.6f, 0.4f, 1.0f)));
    }
    frame.Transform(CanvasTransform());
    switch (placement) {
      case Placement::kRoot:
        frame.DrawDisplayList(picture);
        break;
      case Placement::kFilteredLayer: {
        const float matrix[20] = {0.9f, 0, 0, 0, 0,  //
                                  0,    1, 0, 0, 0,  //
                                  0,    0, 1, 0, 0,  //
                                  0,    0, 0, 1, 0};
        DlPaint layer;
        layer.setColorFilter(DlColorFilter::MakeMatrix(matrix));
        frame.SaveLayer(DlRect::MakeWH(48, 48), &layer);
        frame.DrawDisplayList(picture);
        frame.Restore();
        break;
      }
      case Placement::kGroupOpacity:
        frame.DrawDisplayList(picture, 0.5f);
        break;
    }
    return frame.Build();
  }

  // Renders the frame as the embedder does (root canvas through `root`) and
  // returns the target's bytes as stored.
  // Both output formats store four bytes a pixel.
  std::vector<uint8_t> Render(const sk_sp<DisplayList>& frame,
                              const DlMatrix& root,
                              bool bottom_up,
                              PixelFormat format) {
    const ISize size(kSize, kSize);
    TextureDescriptor descriptor;
    descriptor.storage_mode = StorageMode::kDevicePrivate;
    descriptor.format = format;
    descriptor.size = size;
    descriptor.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
    auto texture = Base().GetResourceAllocator()->CreateTexture(descriptor);
    EXPECT_TRUE(texture);
    if (!texture) {
      return {};
    }
    if (bottom_up) {
      TextureGLES::Cast(*texture).SetStoredBottomUp();
    }
    RenderTargetAllocator allocator(Base().GetResourceAllocator());
    RenderTarget target = allocator.CreateOffscreen(
        *context_, size, 1, "Pixel test",
        RenderTarget::AttachmentConfig{
            .storage_mode = StorageMode::kDevicePrivate,
            .load_action = LoadAction::kClear,
            .store_action = StoreAction::kStore,
            .clear_color = Color::BlackTransparent()},
        RenderTarget::kDefaultStencilAttachmentConfig, texture);
    EXPECT_TRUE(target.IsValid());

    DisplayListBuilder output(DlRect::MakeWH(kSize, kSize));
    output.Transform(root);
    output.DrawDisplayList(frame);
    EXPECT_TRUE(RenderToTarget(*renderer_, target, output.Build(),
                               Rect::MakeSize(size),
                               /*reset_host_buffer=*/true,
                               /*is_onscreen=*/false));

    const size_t bytes = static_cast<size_t>(kSize) * kSize * 4u;
    auto buffer =
        Base().GetResourceAllocator()->CreateBuffer(DeviceBufferDescriptor{
            .storage_mode = StorageMode::kHostVisible, .size = bytes});
    auto command_buffer = Base().CreateCommandBuffer();
    auto blit = command_buffer->CreateBlitPass();
    EXPECT_TRUE(blit->AddCopy(texture, buffer));
    EXPECT_TRUE(blit->EncodeCommands());
    EXPECT_TRUE(Base().GetCommandQueue()->Submit({command_buffer}).ok());
    EXPECT_TRUE(context_->GetReactor()->React());
    const uint8_t* data = DeviceBufferGLES::Cast(*buffer).GetBufferData();
    return std::vector<uint8_t>(data, data + bytes);
  }

  // Rows (in stored order) whose pixels differ, for the failure message.
  static std::string DifferingRows(const std::vector<uint8_t>& a,
                                   const std::vector<uint8_t>& b) {
    std::ostringstream out;
    for (int row = 0; row < kSize; ++row) {
      int count = 0;
      for (int x = 0; x < kSize * 4; ++x) {
        const size_t index = static_cast<size_t>(row) * kSize * 4 + x;
        count += a[index] != b[index] ? 1 : 0;
      }
      if (count > 0) {
        out << " row " << row << ": " << count << " bytes;";
      }
    }
    return out.str();
  }

  static size_t CoveredPixels(const std::vector<uint8_t>& pixels) {
    size_t covered = 0;
    for (size_t i = 3; i < pixels.size(); i += 4) {
      covered += pixels[i] != 0 ? 1 : 0;
    }
    return covered;
  }

  // Draws the frame live, then with the picture requested from the cache:
  // the first frame only sights it, the second generates its image, the
  // third draws the image again. Every one must match the live pixels.
  void ExpectCacheMatchesLive(
      const DlMatrix& root,
      bool bottom_up,
      Placement placement,
      const sk_sp<DisplayList>& picture = MakeHardEdgedPicture(),
      PixelFormat output = PixelFormat::kR8G8B8A8UNormInt,
      bool backdrop = false,
      PixelFormat snapshot = PixelFormat::kR8G8B8A8UNormInt) {
    const sk_sp<DisplayList> frame = MakeFrame(picture, placement, backdrop);
    const std::vector<uint8_t> live = Render(frame, root, bottom_up, output);
    EXPECT_GT(CoveredPixels(live), 1000u);

    StaticPictureCache cache(*renderer_);
    renderer_->SetStaticPictureCache(&cache);
    cache.Request(picture);
    for (int draw = 0; draw < 3; draw++) {
      SCOPED_TRACE(::testing::Message() << "draw " << draw);
      const std::vector<uint8_t> cached =
          Render(frame, root, bottom_up, output);
      ASSERT_EQ(live.size(), cached.size());
      EXPECT_TRUE(live == cached)
          << "cached picture differs from live:" << DifferingRows(live, cached);
    }
    renderer_->SetStaticPictureCache(nullptr);
    EXPECT_EQ(cache.GetStatistics().generations, 1u);
    EXPECT_EQ(cache.GetStatistics().hits, 1u);
    // The snapshot took the pass the picture landed in.
    const bool layer = placement == Placement::kFilteredLayer;
    const bool msaa =
        layer && Base().GetCapabilities()->SupportsOffscreenMSAA();
    const std::string pass = std::string(" samples=") + (msaa ? "4" : "1") +
                             " format=" + PixelFormatToString(snapshot) +
                             " bottom_up=" + (bottom_up && !layer ? "1" : "0");
    EXPECT_NE(cache.Describe().find(pass), std::string::npos)
        << cache.Describe();
  }

  // ContextGLES keeps the Context overrides private.
  Context& Base() const { return *context_; }

  std::unique_ptr<flutter::testing::TestGLOnscreenOnlySurface> surface_;
  std::shared_ptr<ContextGLES> context_;
  std::shared_ptr<Worker> worker_;
  std::unique_ptr<ContentContext> renderer_;
};

// Denial's GLES output: Y-reflected root transform, borrowed-FBO row order.
TEST_F(StaticPictureCachePixelTest, MatchesLiveUnderReflectedOutput) {
  ExpectCacheMatchesLive(ReflectedRoot(), /*bottom_up=*/true, Placement::kRoot);
}

// A target without reflection, drawn with the offscreen flip.
TEST_F(StaticPictureCachePixelTest, MatchesLiveUnderUnreflectedOffscreen) {
  ExpectCacheMatchesLive(DlMatrix(), /*bottom_up=*/false, Placement::kRoot);
}

// Inside a save layer, as under a window's material: the picture lands in a
// multisampled offscreen pass with the offscreen flip.
TEST_F(StaticPictureCachePixelTest, MatchesLiveInsideAnOffscreenLayer) {
  ExpectCacheMatchesLive(ReflectedRoot(), /*bottom_up=*/true,
                         Placement::kFilteredLayer);
}

TEST_F(StaticPictureCachePixelTest, MatchesLiveWithGroupOpacity) {
  ExpectCacheMatchesLive(ReflectedRoot(), /*bottom_up=*/true,
                         Placement::kGroupOpacity);
}

// A 10-bit output, as on a desktop monitor: its two alpha bits cannot hold a
// shadow's falloff, so the snapshot is stored at half float.
TEST_F(StaticPictureCachePixelTest, MatchesLiveSoftShadowOnTenBitOutput) {
  const PixelFormat snapshot = CapabilitiesGLES::Cast(*Base().GetCapabilities())
                                       .SupportsHalfFloatColorAttachments()
                                   ? PixelFormat::kR16G16B16A16Float
                                   : PixelFormat::kR8G8B8A8UNormInt;
  ExpectCacheMatchesLive(ReflectedRoot(), /*bottom_up=*/true, Placement::kRoot,
                         MakeSoftShadowPicture(),
                         PixelFormat::kR10G10B10A2UNormInt,
                         /*backdrop=*/true, snapshot);
}

}  // namespace
}  // namespace testing
}  // namespace impeller
