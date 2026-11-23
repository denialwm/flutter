// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_texture_resolver.h"

#include <memory>
#include <utility>

#include "flutter/fml/logging.h"
#include "flutter/shell/platform/embedder/embedder_external_texture_presentation.h"

#if defined(SHELL_ENABLE_VULKAN) && defined(IMPELLER_SUPPORTS_RENDERING)
#include "impeller/display_list/aiks_context.h"  // nogncheck
#include "impeller/display_list/dl_image_impeller.h"  // nogncheck
#include "impeller/renderer/backend/vulkan/texture_source_vk.h"  // nogncheck
#include "impeller/renderer/backend/vulkan/texture_vk.h"  // nogncheck
#endif

namespace flutter {

namespace {

DenialTextureLending* g_denial_lending = nullptr;

#if defined(SHELL_ENABLE_VULKAN) && defined(IMPELLER_SUPPORTS_RENDERING)
// A client image the composer lends for one engine composition. The composer
// keeps it, in the layout it was lent in, until the composition's
// submissions completed.
class DenialLentTextureSourceVK final : public impeller::TextureSourceVK {
 public:
  DenialLentTextureSourceVK(impeller::TextureDescriptor descriptor,
                            impeller::vk::Image image,
                            impeller::vk::ImageView image_view)
      : TextureSourceVK(descriptor), image_(image), image_view_(image_view) {
    SetLayoutWithoutEncoding(impeller::vk::ImageLayout::eShaderReadOnlyOptimal);
  }

  impeller::vk::Image GetImage() const override { return image_; }

  impeller::vk::ImageView GetImageView() const override { return image_view_; }

  impeller::vk::ImageView GetRenderTargetView(
      uint32_t mip_level,
      uint32_t array_layer) const override {
    return image_view_;
  }

  bool IsSwapchainImage() const override { return false; }

 private:
  impeller::vk::Image image_;
  impeller::vk::ImageView image_view_;
};

std::optional<impeller::PixelFormat> DenialLentFormat(uint32_t format) {
  switch (static_cast<impeller::vk::Format>(format)) {
    case impeller::vk::Format::eR8G8B8A8Unorm:
      return impeller::PixelFormat::kR8G8B8A8UNormInt;
    case impeller::vk::Format::eB8G8R8A8Unorm:
      return impeller::PixelFormat::kB8G8R8A8UNormInt;
    case impeller::vk::Format::eR16G16B16A16Sfloat:
      return impeller::PixelFormat::kR16G16B16A16Float;
    default:
      return std::nullopt;
  }
}
#endif  // SHELL_ENABLE_VULKAN && IMPELLER_SUPPORTS_RENDERING

// A client texture the Denial scene composer draws. The engine keeps it for
// its ID: registration, frame-available marks and the damage they cause.
// While the engine composes a frame the composer could not, it paints the
// image the composer lends, through the same presentation and window code as
// the GLES texture; otherwise a paint draws nothing and is logged.
class DenialComposerTexture final : public Texture {
 public:
  explicit DenialComposerTexture(int64_t id) : Texture(id) {}

  void Paint(PaintContext& context,
             const DlRect& bounds,
             bool freeze,
             const DlImageSampling sampling) override {
    DenialTexturePresentation presentation;
    const sk_sp<DlImage> image = Resolve(context, presentation);
    presentation.Paint(context.canvas, context.paint, image, bounds, sampling);
  }

  void PaintWindow(PaintContext& context,
                   const DlRect& texture_bounds,
                   const DlMatrix& texture_transform,
                   const DlWindowSurfaceFilter& window,
                   std::optional<int64_t> backdrop_id,
                   DlImageSampling sampling,
                   DlScalar surface_opacity) override {
    // Without an image the window keeps its frame and material, as the GLES
    // texture's does.
    DenialTexturePresentation presentation;
    const sk_sp<DlImage> image = Resolve(context, presentation);
    DenialTexturePresentation::PaintWindow(
        context, presentation.Direct(), texture_transform, window, backdrop_id,
        surface_opacity, [&](PaintContext& child) {
          presentation.Paint(child.canvas, child.paint, image, texture_bounds,
                             sampling);
        });
  }

  void MarkNewFrameAvailable() override {}

  void OnTextureUnregistered() override {}

  void OnGrContextCreated() override {}

  void OnGrContextDestroyed() override {}

 private:
  // The lent image, and its presentation; null outside a fallback or when
  // the composer has none.
  sk_sp<DlImage> Resolve(PaintContext& context,
                         DenialTexturePresentation& presentation) {
    presentation.Reset();
#if defined(SHELL_ENABLE_VULKAN) && defined(IMPELLER_SUPPORTS_RENDERING)
    DenialTextureLending* lending = g_denial_lending;
    if (lending != nullptr && context.aiks_context != nullptr &&
        lending->composer.resolve_texture_vk != nullptr) {
      DenialFlutterTextureImageVK lent = {};
      lent.struct_size = sizeof(lent);
      if (!lending->composer.resolve_texture_vk(lending->composer.user_data,
                                                lending->ticket, Id(),
                                                &lent)) {
        return nullptr;
      }
      // Even an image refused below was resolved: the composer lends and
      // takes back everything it handed out.
      lending->resolved = true;
      const auto format = DenialLentFormat(lent.format);
      if (lent.struct_size != sizeof(lent) || lent.image == 0 ||
          lent.image_view == 0 || lent.width == 0 || lent.height == 0 ||
          !format.has_value()) {
        LogUnusable();
        return nullptr;
      }
      impeller::TextureDescriptor descriptor;
      descriptor.type = impeller::TextureType::kTexture2D;
      descriptor.storage_mode = impeller::StorageMode::kDevicePrivate;
      descriptor.format = format.value();
      descriptor.size = impeller::ISize(lent.width, lent.height);
      descriptor.mip_count = 1u;
      descriptor.sample_count = impeller::SampleCount::kCount1;
      descriptor.usage = impeller::TextureUsage::kShaderRead;
      auto source = std::make_shared<DenialLentTextureSourceVK>(
          descriptor, impeller::vk::Image(reinterpret_cast<VkImage>(lent.image)),
          impeller::vk::ImageView(
              reinterpret_cast<VkImageView>(lent.image_view)));
      auto texture = std::make_shared<impeller::TextureVK>(
          context.aiks_context->GetContext(), std::move(source));
      static_cast<impeller::Texture&>(*texture).SetLabel(
          "Denial lent client");
      sk_sp<DlImage> image = impeller::DlImageImpeller::Make(
          texture, DlImage::OwningContext::kRaster,
          /*is_external_texture=*/true);
      presentation.Prepare(DlRect::Make(image->GetBounds()),
                           lent.has_presentation ? &lent.presentation
                                                 : nullptr);
      return image;
    }
#endif  // SHELL_ENABLE_VULKAN && IMPELLER_SUPPORTS_RENDERING
    LogPaint();
    return nullptr;
  }

  void LogPaint() {
    static uint64_t paints = 0;
    ++paints;
    if (paints <= 8 || (paints & (paints - 1)) == 0) {
      FML_LOG(ERROR) << "Denial composer texture " << Id()
                     << " asked to paint outside a fallback; drawn empty, "
                        "paints="
                     << paints;
    }
  }

  void LogUnusable() {
    static uint64_t unusable = 0;
    ++unusable;
    if (unusable <= 8 || (unusable & (unusable - 1)) == 0) {
      FML_LOG(ERROR) << "Denial composer lent an unusable image for texture "
                     << Id() << "; drawn empty, count=" << unusable;
    }
  }
};

}  // namespace

void DenialSetTextureLending(DenialTextureLending* lending) {
  g_denial_lending = lending;
}

#ifdef SHELL_ENABLE_GL
EmbedderExternalTextureResolver::EmbedderExternalTextureResolver(
    EmbedderExternalTextureGL::ExternalTextureCallback gl_callback)
    : gl_callback_(std::move(gl_callback)) {}
#endif

#ifdef SHELL_ENABLE_GL
void EmbedderExternalTextureResolver::SetExternalTextureGlStateCallback(
    EmbedderExternalTextureGL::ExternalTextureGlStateCallback callback) {
  gl_state_callback_ = std::move(callback);
}

void EmbedderExternalTextureResolver::SetExternalTexturePresentationCallback(
    EmbedderExternalTextureGL::ExternalTexturePresentationCallback callback) {
  presentation_callback_ = std::move(callback);
}
#endif

#ifdef SHELL_ENABLE_METAL
EmbedderExternalTextureResolver::EmbedderExternalTextureResolver(
    EmbedderExternalTextureMetal::ExternalTextureCallback metal_callback)
    : metal_callback_(std::move(metal_callback)) {}
#endif

std::unique_ptr<Texture>
EmbedderExternalTextureResolver::ResolveExternalTexture(int64_t texture_id) {
  if (denial_composer_textures_) {
    return std::make_unique<DenialComposerTexture>(texture_id);
  }
#ifdef SHELL_ENABLE_GL
  if (gl_callback_) {
    return std::make_unique<EmbedderExternalTextureGL>(
        texture_id, gl_callback_, gl_state_callback_, presentation_callback_);
  }
#endif

#ifdef SHELL_ENABLE_METAL
  if (metal_callback_) {
    return std::make_unique<EmbedderExternalTextureMetal>(texture_id,
                                                          metal_callback_);
  }
#endif

  return nullptr;
}

bool EmbedderExternalTextureResolver::SupportsExternalTextures() {
  if (denial_composer_textures_) {
    return true;
  }
#ifdef SHELL_ENABLE_GL
  if (gl_callback_) {
    return true;
  }
#endif

#ifdef SHELL_ENABLE_METAL
  if (metal_callback_) {
    return true;
  }
#endif

  return false;
}

}  // namespace flutter
