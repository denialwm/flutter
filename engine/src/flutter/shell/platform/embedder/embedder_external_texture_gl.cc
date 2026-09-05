// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_texture_gl.h"

#include <cmath>

#include "flutter/fml/logging.h"
#include "impeller/core/texture_descriptor.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/geometry/size.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/handle_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"

#include "third_party/skia/include/core/SkAlphaType.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkColorType.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPaint.h"
#include "third_party/skia/include/core/SkSize.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/SkImageGanesh.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLTypes.h"

namespace flutter {

EmbedderExternalTextureGL::EmbedderExternalTextureGL(
    int64_t texture_identifier,
    const ExternalTextureCallback& callback,
    const ExternalTextureGlStateCallback& gl_state_callback,
    ExternalTexturePresentationCallback presentation_callback)
    : Texture(texture_identifier),
      external_texture_callback_(callback),
      external_texture_gl_state_callback_(gl_state_callback),
      presentation_callback_(std::move(presentation_callback)) {
  FML_DCHECK(external_texture_callback_);
}

EmbedderExternalTextureGL::~EmbedderExternalTextureGL() = default;

// |flutter::Texture|
void EmbedderExternalTextureGL::Paint(PaintContext& context,
                                      const DlRect& bounds,
                                      bool freeze,
                                      const DlImageSampling sampling) {
  if (last_image_ == nullptr) {
    last_image_ =
        ResolveTexture(Id(),                                                 //
                       context.gr_context,                                   //
                       context.aiks_context,                                 //
                       SkISize::Make(bounds.GetWidth(), bounds.GetHeight())  //
        );
    presentation_ = {};
    presentation_.struct_size = sizeof(presentation_);
    if (last_image_ && presentation_callback_) {
      const bool supplied = presentation_callback_(Id(), &presentation_);
      const auto finite_rect = [](const double* rect) {
        return std::isfinite(rect[0]) && std::isfinite(rect[1]) &&
               std::isfinite(rect[2]) && std::isfinite(rect[3]) &&
               rect[2] >= 0 && rect[3] >= 0;
      };
      if (!supplied || presentation_.struct_size != sizeof(presentation_) ||
          !std::isfinite(presentation_.width) ||
          !std::isfinite(presentation_.height) || presentation_.width <= 0 ||
          presentation_.height <= 0 || !finite_rect(presentation_.source) ||
          !finite_rect(presentation_.destination) ||
          !finite_rect(presentation_.background)) {
        presentation_ = {};
      }
    }
  }

  DlCanvas* canvas = context.canvas;
  const DlPaint* paint = context.paint;

  if (last_image_ && presentation_.width > 0) {
    const auto source =
        DlRect::MakeXYWH(presentation_.source[0], presentation_.source[1],
                         presentation_.source[2], presentation_.source[3]);
    const auto project = [&](const double* rect) {
      const double sx = bounds.GetWidth() / presentation_.width;
      const double sy = bounds.GetHeight() / presentation_.height;
      return DlRect::MakeXYWH(bounds.GetLeft() + rect[0] * sx,
                              bounds.GetTop() + rect[1] * sy, rect[2] * sx,
                              rect[3] * sy);
    };
    DlPaint background = paint ? *paint : DlPaint();
    background.setColor(DlColor(presentation_.background_argb)
                            .modulateOpacity(background.getOpacity()));
    canvas->Save();
    canvas->ClipRect(bounds);
    // A small solid quad and the original image share this TextureLayer.
    // No saveLayer, intermediate image, buffer copy, or sampled header texture.
    canvas->DrawRect(project(presentation_.background), background);
    if (!source.IsEmpty()) {
      canvas->DrawImageRect(last_image_, source,
                            project(presentation_.destination), sampling,
                            paint);
    }
    canvas->Restore();
    return;
  }
  if (last_image_) {
    DlRect image_bounds = DlRect::Make(last_image_->GetBounds());
    if (bounds != image_bounds) {
      canvas->DrawImageRect(last_image_, image_bounds, bounds, sampling, paint);
    } else {
      canvas->DrawImage(last_image_, bounds.GetOrigin(), sampling, paint);
    }
  }
}

sk_sp<DlImage> EmbedderExternalTextureGL::ResolveTexture(
    int64_t texture_id,
    GrDirectContext* context,
    impeller::AiksContext* aiks_context,
    const SkISize& size) {
  if (!!aiks_context) {
    return ResolveTextureImpeller(texture_id, aiks_context, size);
  } else {
    return ResolveTextureSkia(texture_id, context, size);
  }
}

sk_sp<DlImage> EmbedderExternalTextureGL::ResolveTextureSkia(
    int64_t texture_id,
    GrDirectContext* context,
    const SkISize& size) {
  const bool callback_may_modify_gl =
      !external_texture_gl_state_callback_ ||
      external_texture_gl_state_callback_(texture_id);
  if (callback_may_modify_gl) {
    context->flushAndSubmit();
    context->resetContext(kAll_GrBackendState);
  }
  std::unique_ptr<FlutterOpenGLTexture> texture =
      external_texture_callback_(texture_id, size.width(), size.height());

  if (!texture) {
    return nullptr;
  }

  GrGLTextureInfo gr_texture_info = {texture->target, texture->name,
                                     texture->format};

  size_t width = size.width();
  size_t height = size.height();

  if (texture->width != 0 && texture->height != 0) {
    width = texture->width;
    height = texture->height;
  }

  auto gr_backend_texture = GrBackendTextures::MakeGL(
      width, height, skgpu::Mipmapped::kNo, gr_texture_info);
  SkImages::TextureReleaseProc release_proc = texture->destruction_callback;
  auto image =
      SkImages::BorrowTextureFrom(context,                   // context
                                  gr_backend_texture,        // texture handle
                                  kTopLeft_GrSurfaceOrigin,  // origin
                                  kRGBA_8888_SkColorType,    // color type
                                  kPremul_SkAlphaType,       // alpha type
                                  nullptr,                   // colorspace
                                  release_proc,       // texture release proc
                                  texture->user_data  // texture release context
      );

  if (!image) {
    // In case Skia rejects the image, call the release proc so that
    // embedders can perform collection of intermediates.
    if (release_proc) {
      release_proc(texture->user_data);
    }
    FML_LOG(ERROR) << "Could not create external texture->";
    return nullptr;
  }

  // This image should not escape local use by EmbedderExternalTextureGL
  return DlImage::Make(std::move(image));
}

sk_sp<DlImage> EmbedderExternalTextureGL::ResolveTextureImpeller(
    int64_t texture_id,
    impeller::AiksContext* aiks_context,
    const SkISize& size) {
  const std::shared_ptr<impeller::Context>& impeller_context =
      aiks_context->GetContext();
  impeller::ContextGLES& context =
      impeller::ContextGLES::Cast(*impeller_context);
  const bool callback_may_modify_gl =
      !external_texture_gl_state_callback_ ||
      external_texture_gl_state_callback_(texture_id);
  if (callback_may_modify_gl && !impeller_context->FlushCommandBuffers()) {
    FML_LOG(ERROR) << "Could not flush Impeller before resolving an external "
                      "texture";
    return nullptr;
  }

  std::unique_ptr<FlutterOpenGLTexture> texture =
      external_texture_callback_(texture_id, size.width(), size.height());
  if (callback_may_modify_gl) {
    impeller_context->ResetThreadLocalState();
  }

  if (!texture) {
    return nullptr;
  }

  // Call the destruction callback if an error occurs.
  fml::ScopedCleanupClosure scoped_cleanup([&texture]() {
    if (texture->destruction_callback) {
      texture->destruction_callback(texture->user_data);
    }
  });

  if (texture->format != GL_RGBA8) {
    FML_LOG(ERROR) << "Only support GL_RGBA8 format now";
    return nullptr;
  }

  impeller::TextureDescriptor desc;
  desc.size = impeller::ISize(texture->width, texture->height);
  desc.format = impeller::PixelFormat::kR8G8B8A8UNormInt;

  impeller::HandleGLES handle = context.GetReactor()->CreateHandle(
      impeller::HandleType::kTexture, texture->name);
  std::shared_ptr<impeller::TextureGLES> image =
      impeller::TextureGLES::WrapTexture(context.GetReactor(), desc, handle);

  if (!image) {
    FML_LOG(ERROR) << "Could not create external texture";
    return nullptr;
  }

  VoidCallback destruction_callback = texture->destruction_callback;
  if (!destruction_callback) {
    // Set a no-op cleanup callback if the texture does not provide a callback.
    // The presence of a cleanup callback indicates that the embedder controls
    // the GL texture's lifetime and Impeller should not delete it.
    destruction_callback = [](void*) {};
  }
  auto cleanup_callback = [callback = destruction_callback,
                           user_data = texture->user_data]() {
    callback(user_data);
  };
  if (!context.GetReactor()->RegisterCleanupCallback(handle,
                                                     cleanup_callback)) {
    FML_LOG(ERROR) << "Could not register destruction callback";
    return nullptr;
  }

  image->SetCoordinateSystem(
      impeller::TextureCoordinateSystem::kUploadFromHost);

  scoped_cleanup.Release();

  return impeller::DlImageImpeller::Make(image, DlImage::OwningContext::kIO,
                                         /*is_external_texture=*/true);
}

// |flutter::Texture|
void EmbedderExternalTextureGL::OnGrContextCreated() {}

// |flutter::Texture|
void EmbedderExternalTextureGL::OnGrContextDestroyed() {}

// |flutter::Texture|
void EmbedderExternalTextureGL::MarkNewFrameAvailable() {
  last_image_ = nullptr;
}

// |flutter::Texture|
void EmbedderExternalTextureGL::OnTextureUnregistered() {}

}  // namespace flutter
