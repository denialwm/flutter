// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/gpu/gpu_surface_gl_impeller.h"

#include "flow/surface_frame.h"
#include "flutter/fml/make_copyable.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/renderer/backend/gles/surface_gles.h"
#include "impeller/typographer/backends/skia/typographer_context_skia.h"

namespace flutter {

GPUSurfaceGLImpeller::GPUSurfaceGLImpeller(
    GPUSurfaceGLDelegate* delegate,
    std::shared_ptr<impeller::Context> context,
    bool render_to_surface,
    bool fbo_zero_is_no_target)
    : weak_factory_(this) {
  if (delegate == nullptr) {
    return;
  }

  if (!context || !context->IsValid()) {
    return;
  }

  auto aiks_context = std::make_shared<impeller::AiksContext>(
      context, impeller::TypographerContextSkia::Make());

  if (!aiks_context->IsValid()) {
    return;
  }

  delegate_ = delegate;
  impeller_context_ = std::move(context);
  render_to_surface_ = render_to_surface;
  fbo_zero_is_no_target_ = fbo_zero_is_no_target;
  aiks_context_ = std::move(aiks_context);
  is_valid_ = true;
}

// |Surface|
GPUSurfaceGLImpeller::~GPUSurfaceGLImpeller() = default;

// |Surface|
bool GPUSurfaceGLImpeller::IsValid() {
  return is_valid_;
}

bool GPUSurfaceGLImpeller::PresentFrame(
    GPUSurfaceGLDelegate* delegate,
    uint32_t fbo_id,
    const DlISize& size,
    const SurfaceFrame::SubmitInfo& submit_info) {
  const std::optional<DlRegion> full_damage = DlRegion(DlIRect::MakeSize(size));
  const std::optional<DlRegion>& frame_damage =
      submit_info.frame_damage.has_value() ? submit_info.frame_damage
                                           : full_damage;
  const std::optional<DlRegion>& buffer_damage =
      submit_info.buffer_damage.has_value() ? submit_info.buffer_damage
                                            : full_damage;
  delegate->GLContextSetDamageRegion(
      buffer_damage ? std::make_optional(buffer_damage->bounds())
                    : std::nullopt);
  GLPresentInfo present_info = {
      .fbo_id = fbo_id,
      .frame_damage = frame_damage,
      .presentation_time = submit_info.presentation_time,
      .buffer_damage = buffer_damage,
  };
  return delegate->GLContextPresent(present_info);
}

DlMatrix GPUSurfaceGLImpeller::RootTransformation(
    GPUSurfaceGLDelegate* delegate) {
  return delegate->GLContextSurfaceTransformation();
}

void GPUSurfaceGLImpeller::ConfigureRootCanvas(DlCanvas* canvas,
                                               GPUSurfaceGLDelegate* delegate) {
  if (canvas) {
    canvas->SetTransform(RootTransformation(delegate));
  }
}

// |Surface|
std::unique_ptr<SurfaceFrame> GPUSurfaceGLImpeller::AcquireFrame(
    const DlISize& size) {
  if (!IsValid()) {
    FML_LOG(ERROR) << "OpenGL surface was invalid.";
    return nullptr;
  }

  auto context_switch = delegate_->GLContextMakeCurrent();
  if (!context_switch->GetResult()) {
    FML_LOG(ERROR)
        << "Could not make the context current to acquire the frame.";
    return nullptr;
  }

  if (!render_to_surface_) {
    auto submit = [weak = weak_factory_.GetWeakPtr(), delegate = delegate_,
                   size, forward_damage = fbo_zero_is_no_target_](
                      const SurfaceFrame& surface_frame) {
      if (!forward_damage) {
        return true;
      }
      return weak &&
             PresentFrame(delegate, 0u, size, surface_frame.submit_info());
    };
    return std::make_unique<SurfaceFrame>(
        nullptr, SurfaceFrame::FramebufferInfo{.supports_readback = true},
        [](const SurfaceFrame& surface_frame, DlCanvas* canvas) {
          return true;
        },
        std::move(submit), size);
  }

  GLFrameInfo frame_info = {static_cast<uint32_t>(size.width),
                            static_cast<uint32_t>(size.height)};
  const GLFBOInfo fbo_info = delegate_->GLContextFBO(frame_info);
  auto framebuffer_info = delegate_->GLContextFramebufferInfo();
  if (!framebuffer_info.existing_damage.has_value()) {
    framebuffer_info.existing_damage = fbo_info.existing_damage;
  }
  auto present = [weak = weak_factory_.GetWeakPtr(), delegate = delegate_,
                  size](uint32_t fbo_id,
                        const SurfaceFrame& surface_frame) -> bool {
    if (!weak) {
      return false;
    }
    return PresentFrame(delegate, fbo_id, size, surface_frame.submit_info());
  };
  auto make_skipped_frame = [&](std::unique_ptr<GLContextResult> context) {
    return std::make_unique<SurfaceFrame>(
        nullptr, framebuffer_info,
        [](SurfaceFrame&, DlCanvas*) { return true; },
        [present](SurfaceFrame& surface_frame) {
          return present(0u, surface_frame);
        },
        size, std::move(context), true);
  };

  if (fbo_info.fbo_id == 0u && fbo_zero_is_no_target_) {
    return make_skipped_frame(std::move(context_switch));
  }

  auto swap_callback = []() -> bool { return true; };
  auto surface = impeller::SurfaceGLES::WrapFBO(
      impeller_context_,                         // context
      swap_callback,                             // swap_callback
      fbo_info.fbo_id,                           // fbo
      impeller::PixelFormat::kR8G8B8A8UNormInt,  // color_format
      impeller::ISize{size.width, size.height},  // fbo_size
      framebuffer_info.supports_readback);
  if (!surface) {
    FML_LOG(ERROR) << "Could not wrap Impeller OpenGL FBO " << fbo_info.fbo_id
                   << ".";
    if (fbo_zero_is_no_target_) {
      return make_skipped_frame(std::move(context_switch));
    }
    return nullptr;
  }

  impeller::RenderTarget render_target = surface->GetRenderTarget();

  SurfaceFrame::EncodeCallback encode_callback =
      [aiks_context = aiks_context_,  //
       render_target,
       size](SurfaceFrame& surface_frame, DlCanvas* canvas) mutable -> bool {
    if (!aiks_context) {
      return false;
    }

    auto display_list = surface_frame.BuildDisplayList();
    if (!display_list) {
      FML_LOG(ERROR) << "Could not build display list for surface frame.";
      return false;
    }

    const auto& buffer_damage = surface_frame.submit_info().buffer_damage;
    const bool full_repaint =
        !buffer_damage.has_value() ||
        (!buffer_damage->isEmpty() && buffer_damage->isSimple() &&
         buffer_damage->bounds() == DlIRect::MakeSize(size));
    auto color0 = render_target.GetColorAttachment(0u);
    color0.load_action = full_repaint ? impeller::LoadAction::kClear
                                      : impeller::LoadAction::kLoad;
    render_target.SetColorAttachment(color0, 0u);

    auto cull_rect =
        impeller::Rect::MakeSize(render_target.GetRenderTargetSize());
    return impeller::RenderToTarget(aiks_context->GetContentContext(),  //
                                    render_target,                      //
                                    display_list,                       //
                                    cull_rect,                          //
                                    /*reset_host_buffer=*/true          //
    );
  };

  auto frame = std::make_unique<SurfaceFrame>(
      nullptr,           // surface
      framebuffer_info,  // framebuffer info
      encode_callback,   // encode callback
      fml::MakeCopyable(
          [surface = std::move(surface), present,
           fbo_id = fbo_info.fbo_id](const SurfaceFrame& surface_frame) {
            return surface->Present() && present(fbo_id, surface_frame);
          }),                     // submit callback
      size,                       // frame size
      std::move(context_switch),  // context result
      true                        // display list fallback
  );
  ConfigureRootCanvas(frame->Canvas(), delegate_);
  return frame;
}

// |Surface|
DlMatrix GPUSurfaceGLImpeller::GetRootTransformation() const {
  return RootTransformation(delegate_);
}

// |Surface|
GrDirectContext* GPUSurfaceGLImpeller::GetContext() {
  // Impeller != Skia.
  return nullptr;
}

// |Surface|
std::unique_ptr<GLContextResult>
GPUSurfaceGLImpeller::MakeRenderContextCurrent() {
  return delegate_->GLContextMakeCurrent();
}

// |Surface|
bool GPUSurfaceGLImpeller::ClearRenderContext() {
  return delegate_->GLContextClearCurrent();
}

bool GPUSurfaceGLImpeller::AllowsDrawingWhenGpuDisabled() const {
  return delegate_->AllowsDrawingWhenGpuDisabled();
}

// |Surface|
bool GPUSurfaceGLImpeller::EnableRasterCache() const {
  return false;
}

// |Surface|
std::shared_ptr<impeller::AiksContext> GPUSurfaceGLImpeller::GetAiksContext()
    const {
  return aiks_context_;
}

}  // namespace flutter
