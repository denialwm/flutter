// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/backdrop_surface_contents.h"

#include <array>
#include <utility>

#include "flutter/fml/build_config.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/pipelines.h"
#include "impeller/renderer/command.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/vertex_buffer_builder.h"

#if defined(IMPELLER_ENABLE_OPENGLES) && !defined(FML_OS_EMSCRIPTEN)
#include "impeller/entity/backdrop_surface_composite.frag.h"
#include "impeller/entity/backdrop_surface_composite.vert.h"
#endif

namespace impeller {

namespace {

Point MapPoint(const Rect& destination,
               const Rect& normalized_source,
               Point point) {
  const Vector2 unit =
      (point - destination.GetOrigin()) / Vector2(destination.GetSize());
  return normalized_source.GetOrigin() + unit * normalized_source.GetSize();
}

}  // namespace

std::shared_ptr<BackdropSurfaceContents> BackdropSurfaceContents::Make(
    const Entity& backdrop_entity,
    const std::shared_ptr<TextureContents>& backdrop_contents,
    const std::optional<Snapshot>& scene_snapshot,
    const std::shared_ptr<TextureContents>& surface_contents,
    const Matrix& surface_transform,
    const Rect& composite_coverage,
    std::optional<AnalyticRRect> analytic_clip) {
  if (!surface_contents || !surface_contents->IsExternalTexture() ||
      !surface_contents->GetTexture() ||
      surface_contents->GetTexture()->GetTextureDescriptor().type !=
          TextureType::kTextureExternalOES ||
      surface_contents->GetStrictSourceRect() ||
      !Rect::MakeSize(surface_contents->GetTexture()->GetSize())
           .Contains(surface_contents->GetSourceRect()) ||
      surface_contents->GetSamplerDescriptor().width_address_mode !=
          SamplerAddressMode::kClampToEdge ||
      surface_contents->GetSamplerDescriptor().height_address_mode !=
          SamplerAddressMode::kClampToEdge ||
      !backdrop_entity.GetTransform().IsTranslationScaleOnly() ||
      !surface_transform.IsTranslationScaleOnly()) {
    return nullptr;
  }
  if (scene_snapshot.has_value() &&
      (!scene_snapshot->texture ||
       scene_snapshot->texture->GetTextureDescriptor().type ==
           TextureType::kTextureExternalOES ||
       !scene_snapshot->GetUVTransform().has_value())) {
    return nullptr;
  }
  if (analytic_clip.has_value() && !scene_snapshot.has_value()) {
    return nullptr;
  }
  if (!backdrop_contents || !backdrop_contents->GetTexture() ||
      backdrop_entity.GetContents().get() != backdrop_contents.get() ||
      backdrop_contents->IsExternalTexture() ||
      backdrop_contents->GetTexture()->GetTextureDescriptor().type ==
          TextureType::kTextureExternalOES) {
    return nullptr;
  }

  TextureInput backdrop = {
      .texture = backdrop_contents->GetTexture(),
      .source_rect = backdrop_contents->GetSourceRect(),
      .destination_rect = backdrop_contents->GetDestinationRect(),
      .sampler = backdrop_contents->GetSamplerDescriptor(),
      .transform = backdrop_entity.GetTransform(),
      .opacity = backdrop_contents->GetOpacity(),
  };
  if (backdrop.source_rect.IsEmpty() || backdrop.destination_rect.IsEmpty()) {
    return nullptr;
  }
  const std::optional<Rect> destination =
      surface_contents->GetDestinationRect().Intersection(
          composite_coverage.TransformBounds(surface_transform.Invert()));
  if (!destination.has_value()) {
    return nullptr;
  }
  return std::shared_ptr<BackdropSurfaceContents>(new BackdropSurfaceContents(
      std::move(backdrop), scene_snapshot, surface_contents,
      destination.value(), std::move(analytic_clip)));
}

BackdropSurfaceContents::BackdropSurfaceContents(
    TextureInput backdrop,
    std::optional<Snapshot> scene,
    std::shared_ptr<TextureContents> surface,
    Rect destination,
    std::optional<AnalyticRRect> analytic_clip)
    : backdrop_(std::move(backdrop)),
      scene_(std::move(scene)),
      surface_(std::move(surface)),
      destination_(destination),
      analytic_clip_(std::move(analytic_clip)) {}

BackdropSurfaceContents::~BackdropSurfaceContents() = default;

std::optional<Rect> BackdropSurfaceContents::GetCoverage(
    const Entity& entity) const {
  return destination_.TransformBounds(entity.GetTransform());
}

bool BackdropSurfaceContents::Render(const ContentContext& renderer,
                                     const Entity& entity,
                                     RenderPass& pass) const {
#if defined(IMPELLER_ENABLE_OPENGLES) && !defined(FML_OS_EMSCRIPTEN)
  using VS = BackdropSurfaceCompositeVertexShader;
  using FS = BackdropSurfaceCompositeFragmentShader;

  const Rect destination = destination_;
  const auto surface_texture = surface_->GetTexture();
  if (!surface_texture || destination.IsEmpty() ||
      !entity.GetTransform().IsTranslationScaleOnly()) {
    return false;
  }

  const Rect surface_uvs = Rect::MakeSize(surface_texture->GetSize())
                               .Project(surface_->GetSourceRect());
  const Rect backdrop_uvs = Rect::MakeSize(backdrop_.texture->GetSize())
                                .Project(backdrop_.source_rect);
  const Matrix backdrop_inverse = backdrop_.transform.Invert();
  const Matrix scene_uv_transform =
      scene_.has_value() ? scene_->GetUVTransform().value() : Matrix();
  const auto positions = destination.GetPoints();

  std::array<VS::PerVertexData, 4> vertices;
  for (size_t i = 0u; i < vertices.size(); i++) {
    const Point pass_position = entity.GetTransform() * positions[i];
    const Point backdrop_position = backdrop_inverse * pass_position;
    vertices[i] = VS::PerVertexData{
        positions[i],
        MapPoint(surface_->GetDestinationRect(), surface_uvs, positions[i]),
        MapPoint(backdrop_.destination_rect, backdrop_uvs, backdrop_position),
        scene_.has_value() ? scene_uv_transform * pass_position : Point(),
    };
  }

  auto options = OptionsFromPassAndEntity(pass, entity);
  options.blend_mode = BlendMode::kSrc;
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.depth_write_enabled = false;
  pass.SetPipeline(renderer.GetBackdropSurfaceCompositePipeline(options));
  pass.SetCommandLabel("Denial backdrop surface composite");
  pass.SetCommandAuditCategory(CommandAuditCategory::kBackdropSurfaceComposite);

  auto& data = renderer.GetTransientsDataBuffer();
  pass.SetVertexBuffer(CreateVertexBuffer(vertices, data));

  VS::FrameInfo frame_info;
  frame_info.mvp = entity.GetShaderTransform(pass);
  frame_info.model = entity.GetTransform();
  frame_info.surface_sampler_y_coord_scale = surface_texture->GetYCoordScale();
  frame_info.backdrop_sampler_y_coord_scale =
      backdrop_.texture->GetYCoordScale();
  const std::shared_ptr<Texture>& scene_texture =
      scene_.has_value() ? scene_->texture : backdrop_.texture;
  frame_info.scene_sampler_y_coord_scale = scene_texture->GetYCoordScale();
  VS::BindFrameInfo(pass, data.EmplaceUniform(frame_info));

  FS::FragInfo frag_info;
  frag_info.surface_opacity = surface_->GetOpacity();
  frag_info.backdrop_opacity = backdrop_.opacity;
  frag_info.has_analytic_clip = analytic_clip_.has_value() ? 1.0f : 0.0f;
  if (analytic_clip_.has_value()) {
    frag_info.clip_bounds = Vector4(analytic_clip_->bounds.GetLTRB());
    frag_info.clip_radii = Vector2(analytic_clip_->radii);
  } else {
    frag_info.clip_bounds = Vector4();
    frag_info.clip_radii = Vector2();
  }
  FS::BindFragInfo(pass, data.EmplaceUniform(frag_info));

  SamplerDescriptor surface_sampler = surface_->GetSamplerDescriptor();
  surface_sampler.width_address_mode = SamplerAddressMode::kClampToEdge;
  surface_sampler.height_address_mode = SamplerAddressMode::kClampToEdge;
  surface_sampler.mip_filter = MipFilter::kBase;
  FS::BindSAMPLEREXTERNALOESSurfaceTextureSampler(
      pass, surface_texture,
      renderer.GetContext()->GetSamplerLibrary()->GetSampler(surface_sampler));
  FS::BindBackdropTextureSampler(
      pass, backdrop_.texture,
      renderer.GetContext()->GetSamplerLibrary()->GetSampler(
          backdrop_.sampler));
  SamplerDescriptor scene_sampler =
      scene_.has_value() ? scene_->sampler_descriptor : backdrop_.sampler;
  scene_sampler.width_address_mode = SamplerAddressMode::kClampToEdge;
  scene_sampler.height_address_mode = SamplerAddressMode::kClampToEdge;
  scene_sampler.mip_filter = MipFilter::kBase;
  FS::BindSceneTextureSampler(
      pass, scene_texture,
      renderer.GetContext()->GetSamplerLibrary()->GetSampler(scene_sampler));
  return pass.Draw().ok();
#else
  (void)renderer;
  (void)entity;
  (void)pass;
  return false;
#endif
}

}  // namespace impeller
