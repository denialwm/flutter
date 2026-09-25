// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "impeller/entity/contents/window_surface_contents.h"
#include <array>
#include "flutter/fml/build_config.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/vertex_buffer_builder.h"
#if defined(IMPELLER_ENABLE_OPENGLES) && !defined(FML_OS_EMSCRIPTEN)
#include "impeller/entity/window_surface.frag.h"
#include "impeller/entity/window_surface.vert.h"
#include "impeller/entity/window_surface_texture.frag.h"
#endif
namespace impeller {
namespace {
Matrix UVTransform(const WindowSurfaceContents::Input& input) {
  if (!input.contents) {
    return {};
  }
  const auto& contents = *input.contents;
  const auto& destination = contents.GetDestinationRect();
  const auto texture = contents.GetTexture();
  const Rect source =
      Rect::MakeSize(texture->GetSize()).Project(contents.GetSourceRect());
  Matrix uv = Matrix::MakeTranslation(source.GetOrigin()) *
              Matrix::MakeScale(Vector2(source.GetSize()) /
                                Vector2(destination.GetSize())) *
              Matrix::MakeTranslation(-destination.GetOrigin()) *
              input.window_to_input;
  if (texture->GetYCoordScale() < 0) {
    uv = Matrix::MakeTranslation(Vector2(0, 1)) *
         Matrix::MakeScale(Vector2(1, -1)) * uv;
  }
  return uv;
}
Rect SourceLimits(const WindowSurfaceContents::Input& input,
                  bool inset = true) {
  if (!input.contents) {
    return Rect::MakeLTRB(0, 0, 1, 1);
  }
  const auto texture = input.contents->GetTexture();
  Rect rect = input.contents->GetSourceRect();
  if (inset && input.contents->GetStrictSourceRect()) {
    const Point half{std::min(0.5f, rect.GetWidth() * 0.5f),
                     std::min(0.5f, rect.GetHeight() * 0.5f)};
    rect = Rect::MakeLTRB(rect.GetLeft() + half.x, rect.GetTop() + half.y,
                          rect.GetRight() - half.x, rect.GetBottom() - half.y);
  }
  rect = Rect::MakeSize(texture->GetSize()).Project(rect);
  if (texture->GetYCoordScale() < 0) {
    rect = Rect::MakeLTRB(rect.GetLeft(), 1 - rect.GetBottom(), rect.GetRight(),
                          1 - rect.GetTop());
  }
  return rect;
}
#if defined(IMPELLER_ENABLE_OPENGLES) && !defined(FML_OS_EMSCRIPTEN)
template <typename FS>
void BindFragment(RenderPass& pass,
                  HostBuffer& data,
                  const flutter::DlWindowSurfaceFilter::Style& style,
                  const WindowSurfaceContents::Input& surface,
                  const WindowSurfaceContents::Input& backdrop,
                  Scalar threshold,
                  Scalar opacity,
                  const std::shared_ptr<Texture>& backdrop_texture,
                  raw_ptr<const Sampler> backdrop_sampler) {
  typename FS::FragInfo info;
  info.data[0] = Vector4(style.bounds.GetLTRB());
  info.data[1] = Vector4(style.content_bounds.GetLTRB());
  info.data[2] = Vector4(SourceLimits(surface).GetLTRB());
  info.data[3] = Vector4(SourceLimits(surface, false).GetLTRB());
  const auto color = style.frame_color;
  info.data[4] =
      Vector4(color.getRedF() * color.getAlphaF(),
              color.getGreenF() * color.getAlphaF(),
              color.getBlueF() * color.getAlphaF(), color.getAlphaF());
  info.data[5] =
      Vector4(style.radius, opacity,
              surface.contents ? surface.contents->GetOpacity() : 0,
              backdrop.contents ? backdrop.contents->GetOpacity() : 0);
  info.data[6] = Vector4(threshold, backdrop.contents ? 1 : 0,
                         surface.contents ? 1 : 0, 0);
  FS::BindFragInfo(pass, data.EmplaceUniform(info));
  FS::BindBackdropTextureSampler(pass, backdrop_texture, backdrop_sampler);
}
#endif
}  // namespace
WindowSurfaceContents::WindowSurfaceContents(
    flutter::DlWindowSurfaceFilter::Style style,
    Input surface,
    Input backdrop,
    Scalar alpha_threshold,
    Scalar opacity)
    : style_(style),
      surface_(std::move(surface)),
      backdrop_(std::move(backdrop)),
      threshold_(alpha_threshold),
      opacity_(opacity) {}
std::optional<Rect> WindowSurfaceContents::GetCoverage(
    const Entity& entity) const {
  return style_.bounds.Expand(1).TransformBounds(entity.GetTransform());
}
bool WindowSurfaceContents::Render(const ContentContext& renderer,
                                   const Entity& entity,
                                   RenderPass& pass) const {
#if defined(IMPELLER_ENABLE_OPENGLES) && !defined(FML_OS_EMSCRIPTEN)
  using VS = WindowSurfaceVertexShader;
  using ExternalFS = WindowSurfaceFragmentShader;
  using TextureFS = WindowSurfaceTextureFragmentShader;
  const auto surface_texture = surface_.contents
                                   ? surface_.contents->GetTexture()
                                   : renderer.GetEmptyTexture();
  const auto backdrop_texture = backdrop_.contents
                                    ? backdrop_.contents->GetTexture()
                                    : renderer.GetEmptyTexture();
  const auto points = style_.bounds.Expand(1).GetPoints();
  const auto surface_uv = UVTransform(surface_);
  const auto backdrop_uv = UVTransform(backdrop_);
  std::array<VS::PerVertexData, 4> vertices;
  for (size_t i = 0; i < vertices.size(); i++) {
    vertices[i] = {points[i], surface_uv * points[i], backdrop_uv * points[i]};
  }
  auto options = OptionsFromPassAndEntity(pass, entity);
  options.blend_mode = BlendMode::kSrcOver;
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.depth_write_enabled = false;
  const bool external = surface_texture->GetTextureDescriptor().type ==
                        TextureType::kTextureExternalOES;
  pass.SetPipeline(external
                       ? renderer.GetWindowSurfacePipeline(options)
                       : renderer.GetWindowSurfaceTexturePipeline(options));
  pass.SetCommandLabel("Denial WindowSurface");
  pass.SetCommandAuditCategory(CommandAuditCategory::kBackdropSurfaceComposite);
  auto& data = renderer.GetTransientsDataBuffer();
  pass.SetVertexBuffer(CreateVertexBuffer(vertices, data));
  VS::FrameInfo frame;
  frame.mvp = entity.GetShaderTransform(pass);
  VS::BindFrameInfo(pass, data.EmplaceUniform(frame));
  const auto sampler = [&](const Input& input) {
    SamplerDescriptor descriptor = input.contents
                                       ? input.contents->GetSamplerDescriptor()
                                       : SamplerDescriptor();
    descriptor.width_address_mode = SamplerAddressMode::kClampToEdge;
    descriptor.height_address_mode = SamplerAddressMode::kClampToEdge;
    descriptor.mip_filter = MipFilter::kBase;
    return renderer.GetContext()->GetSamplerLibrary()->GetSampler(descriptor);
  };
  const auto surface_sampler = sampler(surface_);
  const auto backdrop_sampler = sampler(backdrop_);
  if (external) {
    BindFragment<ExternalFS>(pass, data, style_, surface_, backdrop_,
                             threshold_, opacity_, backdrop_texture,
                             backdrop_sampler);
    ExternalFS::BindSAMPLEREXTERNALOESSurfaceTextureSampler(
        pass, surface_texture, surface_sampler);
  } else {
    BindFragment<TextureFS>(pass, data, style_, surface_, backdrop_, threshold_,
                            opacity_, backdrop_texture, backdrop_sampler);
    TextureFS::BindSurfaceTextureSampler(pass, surface_texture,
                                         surface_sampler);
  }
  return pass.Draw().ok();
#else
  return false;
#endif
}
}  // namespace impeller
