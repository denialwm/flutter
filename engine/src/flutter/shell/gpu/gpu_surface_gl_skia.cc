// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/gpu/gpu_surface_gl_skia.h"

#include "flutter/common/graphics/persistent_cache.h"
#include "flutter/fml/base32.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"
#include "flutter/shell/common/context_options.h"
#include "flutter/shell/gpu/gpu_surface_gl_delegate.h"
#include "third_party/skia/include/core/SkAlphaType.h"
#include "third_party/skia/include/core/SkColorFilter.h"
#include "third_party/skia/include/core/SkColorSpace.h"
#include "third_party/skia/include/core/SkColorType.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/GpuTypes.h"
#include "third_party/skia/include/gpu/ganesh/GrBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/GrContextOptions.h"
#include "third_party/skia/include/gpu/ganesh/SkSurfaceGanesh.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLInterface.h"
#include "third_party/skia/include/gpu/ganesh/gl/GrGLTypes.h"

// These are common defines present on all OpenGL headers. However, we don't
// want to perform GL header resolution on each platform we support. So just
// define these upfront. It is unlikely we will need more. But, if we do, we can
// add the same here.
#define GPU_GL_RGBA8 0x8058
#define GPU_GL_RGB8 0x8051
#define GPU_GL_RGBA4 0x8056
#define GPU_GL_RGB565 0x8D62
#define GPU_GL_FRAMEBUFFER 0x8D40
#define GPU_GL_COLOR_ATTACHMENT0 0x8CE0
#define GPU_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GPU_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GPU_GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL 0x8CD2
#define GPU_GL_SAMPLES 0x80A9
#define GPU_GL_STENCIL_BITS 0x0D57
#define GPU_GL_TEXTURE 0x1702
#define GPU_GL_TEXTURE_2D 0x0DE1
#define GPU_GL_TEXTURE_BINDING_2D 0x8069
#define GPU_GL_TEXTURE_INTERNAL_FORMAT 0x1003

namespace flutter {

// Default maximum number of bytes of GPU memory of budgeted resources in the
// cache.
// The shell will dynamically increase or decrease this cache based on the
// viewport size, unless a user has specifically requested a size on the Skia
// system channel.
static const size_t kGrCacheMaxByteSize = 24 * (1 << 20);

sk_sp<GrDirectContext> GPUSurfaceGLSkia::MakeGLContext(
    GPUSurfaceGLDelegate* delegate) {
  auto context_switch = delegate->GLContextMakeCurrent();
  if (!context_switch->GetResult()) {
    FML_LOG(ERROR)
        << "Could not make the context current to set up the Gr context.";
    return nullptr;
  }

  auto options =
      MakeDefaultContextOptions(ContextType::kRender, GrBackendApi::kOpenGL);
  // Denial supplies real stencil attachments on its embedder-owned targets.
  // Let Ganesh use them for GPU stencil and dynamic MSAA instead of falling
  // back to CPU-rasterized A8 clip masks.
  options.fAvoidStencilBuffers = false;

  auto context = GrDirectContexts::MakeGL(delegate->GetGLInterface(), options);

  if (!context) {
    FML_LOG(ERROR) << "Failed to set up Skia Gr context.";
    return nullptr;
  }

  context->setResourceCacheLimit(kGrCacheMaxByteSize);

  PersistentCache::GetCacheForProcess()->PrecompileKnownSkSLs(context.get());

  return context;
}

GPUSurfaceGLSkia::GPUSurfaceGLSkia(GPUSurfaceGLDelegate* delegate,
                                   bool render_to_surface)
    : GPUSurfaceGLSkia(MakeGLContext(delegate), delegate, render_to_surface) {
  context_owner_ = true;
}

GPUSurfaceGLSkia::GPUSurfaceGLSkia(const sk_sp<GrDirectContext>& gr_context,
                                   GPUSurfaceGLDelegate* delegate,
                                   bool render_to_surface)
    : delegate_(delegate),
      context_(gr_context),
      gl_interface_(delegate->GetGLInterface()),
      render_to_surface_(render_to_surface),
      weak_factory_(this) {
  auto context_switch = delegate_->GLContextMakeCurrent();
  if (!context_switch->GetResult()) {
    FML_LOG(ERROR)
        << "Could not make the context current to set up the Gr context.";
    return;
  }

  delegate_->GLContextClearCurrent();

  valid_ = gr_context != nullptr && gl_interface_ != nullptr;
}

GPUSurfaceGLSkia::~GPUSurfaceGLSkia() {
  if (!valid_) {
    return;
  }
  auto context_switch = delegate_->GLContextMakeCurrent();
  if (!context_switch->GetResult()) {
    FML_LOG(ERROR) << "Could not make the context current to destroy the "
                      "GrDirectContext resources.";
    return;
  }

  onscreen_surface_ = nullptr;
  fbo_id_ = 0;
  if (context_owner_) {
    context_->releaseResourcesAndAbandonContext();
  }
  context_ = nullptr;

  delegate_->GLContextClearCurrent();
}

// |Surface|
bool GPUSurfaceGLSkia::IsValid() {
  return valid_;
}

static SkColorType FirstSupportedColorType(GrDirectContext* context,
                                           GrGLenum* format) {
#define RETURN_IF_RENDERABLE(x, y)                 \
  if (context->colorTypeSupportedAsSurface((x))) { \
    *format = (y);                                 \
    return (x);                                    \
  }
  RETURN_IF_RENDERABLE(kRGBA_8888_SkColorType, GPU_GL_RGBA8);
  RETURN_IF_RENDERABLE(kARGB_4444_SkColorType, GPU_GL_RGBA4);
  RETURN_IF_RENDERABLE(kRGB_565_SkColorType, GPU_GL_RGB565);
  return kUnknown_SkColorType;
}

static sk_sp<SkSurface> WrapOnscreenSurface(GrDirectContext* context,
                                            const DlISize& size,
                                            intptr_t fbo,
                                            const GrGLInterface* gl) {
  GrGLint sample_count = 0;
  GrGLint color_attachment_type = 0;
  GrGLint color_attachment_name = 0;
  GrGLint color_attachment_level = -1;
  GrGLint color_attachment_format = 0;
  GrGLint stencil_bits = 0;
  gl->fFunctions.fBindFramebuffer(GPU_GL_FRAMEBUFFER,
                                  static_cast<GrGLuint>(fbo));
  gl->fFunctions.fGetIntegerv(GPU_GL_SAMPLES, &sample_count);
  gl->fFunctions.fGetIntegerv(GPU_GL_STENCIL_BITS, &stencil_bits);
  if (sample_count < 0) {
    sample_count = 0;
  }
  if (stencil_bits != 8 && stencil_bits != 16) {
    stencil_bits = 0;
  }
  if (fbo != 0) {
    gl->fFunctions.fGetFramebufferAttachmentParameteriv(
        GPU_GL_FRAMEBUFFER, GPU_GL_COLOR_ATTACHMENT0,
        GPU_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &color_attachment_type);
    if (color_attachment_type == GPU_GL_TEXTURE) {
      gl->fFunctions.fGetFramebufferAttachmentParameteriv(
          GPU_GL_FRAMEBUFFER, GPU_GL_COLOR_ATTACHMENT0,
          GPU_GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &color_attachment_name);
      gl->fFunctions.fGetFramebufferAttachmentParameteriv(
          GPU_GL_FRAMEBUFFER, GPU_GL_COLOR_ATTACHMENT0,
          GPU_GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &color_attachment_level);
      if (color_attachment_level >= 0 &&
          gl->fFunctions.fGetTexLevelParameteriv) {
        GrGLint previous_texture = 0;
        gl->fFunctions.fGetIntegerv(GPU_GL_TEXTURE_BINDING_2D,
                                    &previous_texture);
        gl->fFunctions.fBindTexture(
            GPU_GL_TEXTURE_2D, static_cast<GrGLuint>(color_attachment_name));
        gl->fFunctions.fGetTexLevelParameteriv(
            GPU_GL_TEXTURE_2D, color_attachment_level,
            GPU_GL_TEXTURE_INTERNAL_FORMAT, &color_attachment_format);
        gl->fFunctions.fBindTexture(GPU_GL_TEXTURE_2D,
                                    static_cast<GrGLuint>(previous_texture));
      }
    }
  }

  GrGLenum format = kUnknown_SkColorType;
  SkColorType color_type = kUnknown_SkColorType;
  if (color_attachment_format == GPU_GL_RGB8 &&
      context->colorTypeSupportedAsSurface(kRGB_888x_SkColorType)) {
    // EGL exposes DRM_FORMAT_XRGB8888 scanout images as RGB8: alpha is
    // logically one and is not part of the framebuffer format. Describing
    // the borrowed texture as RGBA8 makes Ganesh allocate an RGBA8 dynamic
    // MSAA renderbuffer, whose resolve into RGB8 is illegal on GLES.
    format = GPU_GL_RGB8;
    color_type = kRGB_888x_SkColorType;
  } else {
    color_type = FirstSupportedColorType(context, &format);
  }
  sk_sp<SkColorSpace> colorspace = SkColorSpace::MakeSRGB();
  SkSurfaceProps dynamic_msaa_props(SkSurfaceProps::kDynamicMSAA_Flag,
                                    kUnknown_SkPixelGeometry);

  // GLES cannot blit a single-sample framebuffer into an MSAA framebuffer.
  // Ganesh therefore loads its dynamic-MSAA attachment by sampling the
  // single-sample target as a texture. Wrapping only the embedder's FBO hides
  // that texture from Ganesh and leaves the MSAA load undefined. If the color
  // attachment is the level-zero 2D texture used by Denial's EGLImage target,
  // wrap that borrowed texture directly. Skia creates only its lightweight
  // FBO/stencil state around the same storage; no image copy is introduced.
  if (sample_count == 0 && color_attachment_type == GPU_GL_TEXTURE &&
      color_attachment_name > 0 && color_attachment_level == 0) {
    GrGLTextureInfo texture_info = {};
    texture_info.fTarget = GPU_GL_TEXTURE_2D;
    texture_info.fID = static_cast<GrGLuint>(color_attachment_name);
    texture_info.fFormat = format;
    auto backend_texture = GrBackendTextures::MakeGL(
        size.width, size.height, skgpu::Mipmapped::kNo, texture_info);
    auto texture_surface = SkSurfaces::WrapBackendTexture(
        context, backend_texture, GrSurfaceOrigin::kBottomLeft_GrSurfaceOrigin,
        0, color_type, colorspace, &dynamic_msaa_props);
    if (texture_surface) {
      return texture_surface;
    }
  }

  GrGLFramebufferInfo framebuffer_info = {};
  framebuffer_info.fFBOID = static_cast<GrGLuint>(fbo);
  framebuffer_info.fFormat = format;

  auto render_target =
      GrBackendRenderTargets::MakeGL(size.width,       // width
                                     size.height,      // height
                                     sample_count,     // sample count
                                     stencil_bits,     // stencil bits
                                     framebuffer_info  // framebuffer info
      );

  // A framebuffer whose color storage cannot also be wrapped as a texture
  // cannot preserve a dynamic-MSAA load on GLES. Keep the ordinary FBO path
  // correct and let Ganesh choose its non-DMSAA clip implementation.
  SkSurfaceProps framebuffer_props(0, kUnknown_SkPixelGeometry);

  return SkSurfaces::WrapBackendRenderTarget(
      context,                                       // Gr context
      render_target,                                 // render target
      GrSurfaceOrigin::kBottomLeft_GrSurfaceOrigin,  // origin
      color_type,                                    // color type
      colorspace,                                    // colorspace
      &framebuffer_props                             // surface properties
  );
}

bool GPUSurfaceGLSkia::CreateOrUpdateSurfaces(const DlISize& size) {
  if (onscreen_surface_ != nullptr &&
      size.width == onscreen_surface_->width() &&
      size.height == onscreen_surface_->height()) {
    // Surface size appears unchanged. So bail.
    return true;
  }

  // We need to do some updates.
  TRACE_EVENT0("flutter", "UpdateSurfacesSize");

  // Either way, we need to get rid of previous surface.
  onscreen_surface_ = nullptr;
  fbo_id_ = 0;

  if (size.IsEmpty()) {
    FML_LOG(ERROR) << "Cannot create surfaces of empty size.";
    return false;
  }

  sk_sp<SkSurface> onscreen_surface;

  GLFrameInfo frame_info = {static_cast<uint32_t>(size.width),
                            static_cast<uint32_t>(size.height)};
  const GLFBOInfo fbo_info = delegate_->GLContextFBO(frame_info);
  onscreen_surface = WrapOnscreenSurface(context_.get(),   // GL context
                                         size,             // root surface size
                                         fbo_info.fbo_id,  // window FBO ID
                                         gl_interface_.get());

  if (onscreen_surface == nullptr) {
    // If the onscreen surface could not be wrapped. There is absolutely no
    // point in moving forward.
    FML_LOG(ERROR) << "Could not wrap onscreen surface.";
    return false;
  }

  onscreen_surface_ = std::move(onscreen_surface);
  fbo_id_ = fbo_info.fbo_id;
  existing_damage_ = fbo_info.existing_damage;

  return true;
}

// |Surface|
DlMatrix GPUSurfaceGLSkia::GetRootTransformation() const {
  return delegate_->GLContextSurfaceTransformation();
}

// |Surface|
std::unique_ptr<SurfaceFrame> GPUSurfaceGLSkia::AcquireFrame(
    const DlISize& size) {
  if (delegate_ == nullptr) {
    return nullptr;
  }
  auto context_switch = delegate_->GLContextMakeCurrent();
  if (!context_switch->GetResult()) {
    FML_LOG(ERROR)
        << "Could not make the context current to acquire the frame.";
    return nullptr;
  }

  SurfaceFrame::FramebufferInfo framebuffer_info;

  // TODO(38466): Refactor GPU surface APIs take into account the fact that an
  // external view embedder may want to render to the root surface.
  if (!render_to_surface_) {
    framebuffer_info.supports_readback = true;
    return std::make_unique<SurfaceFrame>(
        nullptr, framebuffer_info,
        [](const SurfaceFrame& surface_frame, DlCanvas* canvas) {
          return true;
        },
        [](const SurfaceFrame& surface_frame) { return true; }, size);
  }

  const auto root_surface_transformation = GetRootTransformation();

  sk_sp<SkSurface> surface =
      AcquireRenderSurface(size, root_surface_transformation);

  if (surface == nullptr) {
    return nullptr;
  }

  surface->getCanvas()->setMatrix(ToSkM44(root_surface_transformation));

  SurfaceFrame::EncodeCallback encode_callback =
      [weak = weak_factory_.GetWeakPtr()](const SurfaceFrame& surface_frame,
                                          DlCanvas* canvas) {
        TRACE_EVENT0("flutter", "GrDirectContext::flushAndSubmit");
        if (weak) {
          weak->context_->flushAndSubmit();
          return true;
        }
        return false;
      };
  SurfaceFrame::SubmitCallback submit_callback =
      [weak = weak_factory_.GetWeakPtr()](const SurfaceFrame& surface_frame) {
        return weak ? weak->PresentSurface(surface_frame) : false;
      };

  framebuffer_info = delegate_->GLContextFramebufferInfo();
  if (!framebuffer_info.existing_damage.has_value()) {
    framebuffer_info.existing_damage = existing_damage_;
  }
  return std::make_unique<SurfaceFrame>(surface, framebuffer_info,
                                        encode_callback, submit_callback, size,
                                        std::move(context_switch));
}

bool GPUSurfaceGLSkia::PresentSurface(const SurfaceFrame& frame) {
  if (delegate_ == nullptr || context_ == nullptr) {
    return false;
  }

  delegate_->GLContextSetDamageRegion(frame.submit_info().buffer_damage);

  GLPresentInfo present_info = {
      .fbo_id = fbo_id_,
      .frame_damage = frame.submit_info().frame_damage,
      .presentation_time = frame.submit_info().presentation_time,
      .buffer_damage = frame.submit_info().buffer_damage,
  };
  if (!delegate_->GLContextPresent(present_info)) {
    return false;
  }

  if (delegate_->GLContextFBOResetAfterPresent()) {
    auto current_size =
        DlISize(onscreen_surface_->width(), onscreen_surface_->height());

    GLFrameInfo frame_info = {static_cast<uint32_t>(current_size.width),
                              static_cast<uint32_t>(current_size.height)};

    // The FBO has changed, ask the delegate for the new FBO and do a surface
    // re-wrap.
    const GLFBOInfo fbo_info = delegate_->GLContextFBO(frame_info);
    auto new_onscreen_surface =
        WrapOnscreenSurface(context_.get(),   // GL context
                            current_size,     // root surface size
                            fbo_info.fbo_id,  // window FBO ID
                            gl_interface_.get());

    if (!new_onscreen_surface) {
      return false;
    }

    onscreen_surface_ = std::move(new_onscreen_surface);
    fbo_id_ = fbo_info.fbo_id;
    existing_damage_ = fbo_info.existing_damage;
  }

  return true;
}

sk_sp<SkSurface> GPUSurfaceGLSkia::AcquireRenderSurface(
    const DlISize& untransformed_size,
    const DlMatrix& root_surface_transformation) {
  const auto transformed_rect =
      DlRect::MakeSize(untransformed_size)
          .TransformAndClipBounds(root_surface_transformation);

  const DlISize transformed_size =
      DlIRect::RoundOut(transformed_rect).GetSize();

  if (!CreateOrUpdateSurfaces(transformed_size)) {
    return nullptr;
  }

  return onscreen_surface_;
}

// |Surface|
GrDirectContext* GPUSurfaceGLSkia::GetContext() {
  return context_.get();
}

// |Surface|
std::unique_ptr<GLContextResult> GPUSurfaceGLSkia::MakeRenderContextCurrent() {
  return delegate_->GLContextMakeCurrent();
}

// |Surface|
bool GPUSurfaceGLSkia::ClearRenderContext() {
  return delegate_->GLContextClearCurrent();
}

// |Surface|
bool GPUSurfaceGLSkia::AllowsDrawingWhenGpuDisabled() const {
  return delegate_->AllowsDrawingWhenGpuDisabled();
}

}  // namespace flutter
