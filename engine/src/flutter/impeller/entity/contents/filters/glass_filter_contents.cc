// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/glass_filter_contents.h"

#include <algorithm>
#include <array>
#include <utility>

#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/glass.frag.h"
#include "impeller/entity/glass.vert.h"
#include "impeller/renderer/command.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/vertex_buffer_builder.h"

namespace impeller {

namespace {

Matrix GetMaterialGeometryTransform(const Matrix& effect_transform) {
  Matrix geometry_transform = effect_transform.Basis();
  // Backdrop inputs and their coverage live in render-target coordinates.
  // GLES expresses the root surface orientation as a reflected effect basis;
  // applying that reflection to layout-provided material bounds sends the
  // shape to the opposite side of the texture. Remove only that render-target
  // reflection while retaining user rotation and scale.
  if (geometry_transform.GetDeterminant() < 0.0f) {
    geometry_transform =
        Matrix::MakeScale(Vector3(1.0f, -1.0f, 1.0f)) * geometry_transform;
  }
  return geometry_transform;
}

Point RemapTextureCoordinate(Point coordinate, Scalar y_coord_scale) {
  if (y_coord_scale < 0.0f) {
    coordinate.y = 1.0f - coordinate.y;
  }
  return coordinate;
}

Vector4 TextureUvBasis(const Quad& uvs,
                       const Size& coverage_size,
                       Scalar y_coord_scale) {
  const Point top_left = RemapTextureCoordinate(uvs[0], y_coord_scale);
  const Point top_right = RemapTextureCoordinate(uvs[1], y_coord_scale);
  const Point bottom_left = RemapTextureCoordinate(uvs[2], y_coord_scale);
  const Vector2 x_basis =
      (top_right - top_left) / std::max(coverage_size.width, 0.0001f);
  const Vector2 y_basis =
      (bottom_left - top_left) / std::max(coverage_size.height, 0.0001f);
  return Vector4(x_basis.x, x_basis.y, y_basis.x, y_basis.y);
}

Scalar PhysicalCornerRadius(const Size& radius,
                            Scalar scale_x,
                            Scalar scale_y) {
  return std::min(radius.width * scale_x, radius.height * scale_y);
}

}  // namespace

GlassFilterContents::GlassFilterContents(RoundRect shape,
                                         Scalar thickness,
                                         Scalar refraction,
                                         Scalar dispersion,
                                         Scalar saturation,
                                         Color tint,
                                         Scalar tint_strength,
                                         Scalar brightness,
                                         Scalar light_angle,
                                         Scalar light_intensity,
                                         Scalar edge_strength)
    : shape_(std::move(shape)),
      thickness_(thickness),
      refraction_(refraction),
      dispersion_(dispersion),
      saturation_(saturation),
      tint_(tint),
      tint_strength_(tint_strength),
      brightness_(brightness),
      light_angle_(light_angle),
      light_intensity_(light_intensity),
      edge_strength_(edge_strength) {}

GlassFilterContents::~GlassFilterContents() = default;

std::optional<Entity> GlassFilterContents::RenderFilter(
    const FilterInput::Vector& inputs,
    const ContentContext& renderer,
    const Entity& entity,
    const Matrix& effect_transform,
    const Rect& coverage,
    const std::optional<Rect>& coverage_hint) const {
  using VS = GlassPipeline::VertexShader;
  using FS = GlassPipeline::FragmentShader;

  if (inputs.size() < 2u || coverage.IsEmpty()) {
    return std::nullopt;
  }

  Rect material_coverage = coverage;
  if (coverage_hint.has_value()) {
    const std::optional<Rect> intersection =
        material_coverage.Intersection(coverage_hint.value());
    if (!intersection.has_value()) {
      return std::nullopt;
    }
    material_coverage = intersection.value();
  }

  // The frost path normally resolves through Impeller's established
  // crop-aware Gaussian filter. Resource pressure must not make an entire
  // backdrop scope disappear, so retain the undiffused scene as a graceful
  // fallback if that intermediate cannot be allocated.
  std::optional<Snapshot> blurred_snapshot = inputs[1]->GetSnapshot(
      "Denial Glass Frost", renderer, entity, material_coverage);
  if (!blurred_snapshot.has_value()) {
    blurred_snapshot = inputs[0]->GetSnapshot(
        "Denial Glass Scene Fallback", renderer, entity, material_coverage);
  }
  if (!blurred_snapshot.has_value()) {
    return std::nullopt;
  }

  const std::optional<Quad> blurred_uvs =
      blurred_snapshot->GetCoverageUVs(material_coverage);
  if (!blurred_uvs.has_value()) {
    return std::nullopt;
  }

  const Size material_size = material_coverage.GetSize();
  const Matrix material_transform = entity.GetTransform() * effect_transform;
  const Scalar scale_x =
      std::max(material_transform.TransformDirection(Vector2(1, 0)).GetLength(),
               0.0001f);
  const Scalar scale_y =
      std::max(material_transform.TransformDirection(Vector2(0, 1)).GetLength(),
               0.0001f);
  const Scalar physical_thickness =
      std::min(thickness_ * (scale_x + scale_y) * 0.5f,
               std::min(material_size.width, material_size.height) * 0.5f);
  const RoundingRadii& radii = shape_.GetRadii();
  const Vector4 corner_radii(
      PhysicalCornerRadius(radii.top_left, scale_x, scale_y),
      PhysicalCornerRadius(radii.top_right, scale_x, scale_y),
      PhysicalCornerRadius(radii.bottom_right, scale_x, scale_y),
      PhysicalCornerRadius(radii.bottom_left, scale_x, scale_y));

  const Quad blurred_coordinates = blurred_uvs.value();
  const Vector4 blurred_uv_basis =
      TextureUvBasis(blurred_coordinates, material_size,
                     blurred_snapshot->texture->GetYCoordScale());

  ContentContext::SubpassCallback callback =
      [blurred_snapshot, blurred_coordinates, material_size, corner_radii,
       blurred_uv_basis, physical_thickness, refraction = refraction_,
       dispersion = dispersion_, saturation = saturation_, tint = tint_,
       tint_strength = tint_strength_, brightness = brightness_,
       light_angle = light_angle_, light_intensity = light_intensity_,
       edge_strength = edge_strength_](const ContentContext& renderer,
                                       RenderPass& pass) {
        auto& data = renderer.GetTransientsDataBuffer();
        const std::array<VS::PerVertexData, 4> vertices = {
            VS::PerVertexData{Point(0, 0), blurred_coordinates[0], Point(0, 0)},
            VS::PerVertexData{Point(1, 0), blurred_coordinates[1],
                              Point(material_size.width, 0)},
            VS::PerVertexData{Point(0, 1), blurred_coordinates[2],
                              Point(0, material_size.height)},
            VS::PerVertexData{Point(1, 1), blurred_coordinates[3],
                              Point(material_size.width, material_size.height)},
        };

        VS::FrameInfo frame_info;
        frame_info.mvp = Matrix::MakeOrthographic(ISize(1, 1));
        frame_info.blurred_sampler_y_coord_scale =
            blurred_snapshot->texture->GetYCoordScale();

        FS::FragInfo frag_info;
        frag_info.material_size = Vector2(material_size);
        frag_info.corner_radii = corner_radii;
        frag_info.blurred_uv_basis = blurred_uv_basis;
        frag_info.tint = Vector4(tint.red, tint.green, tint.blue, tint.alpha);
        frag_info.thickness = physical_thickness;
        frag_info.refraction = refraction;
        frag_info.dispersion = dispersion;
        frag_info.saturation = saturation;
        frag_info.tint_strength = tint_strength;
        frag_info.brightness = brightness;
        frag_info.light_angle = light_angle;
        frag_info.light_intensity = light_intensity;
        frag_info.edge_strength = edge_strength;
        frag_info.blurred_opacity = blurred_snapshot->opacity;

        SamplerDescriptor blurred_sampler =
            blurred_snapshot->sampler_descriptor;
        blurred_sampler.min_filter = MinMagFilter::kLinear;
        blurred_sampler.mag_filter = MinMagFilter::kLinear;
        blurred_sampler.mip_filter = MipFilter::kBase;
        blurred_sampler.width_address_mode = SamplerAddressMode::kClampToEdge;
        blurred_sampler.height_address_mode = SamplerAddressMode::kClampToEdge;

        auto options = OptionsFromPass(pass);
        options.primitive_type = PrimitiveType::kTriangleStrip;
        options.blend_mode = BlendMode::kSrc;
        pass.SetPipeline(renderer.GetGlassPipeline(options));
        pass.SetCommandLabel("Denial Glass Material");
        pass.SetVertexBuffer(CreateVertexBuffer(vertices, data));
        VS::BindFrameInfo(pass, data.EmplaceUniform(frame_info));
        FS::BindFragInfo(pass, data.EmplaceUniform(frag_info));
        FS::BindBlurredTextureSampler(
            pass, blurred_snapshot->texture,
            renderer.GetContext()->GetSamplerLibrary()->GetSampler(
                blurred_sampler));
        return pass.Draw().ok();
      };

  std::shared_ptr<CommandBuffer> command_buffer =
      renderer.GetContext()->CreateCommandBuffer();
  if (!command_buffer) {
    return std::nullopt;
  }
  fml::StatusOr<RenderTarget> render_target = renderer.MakeSubpass(
      "Denial Glass Material", ISize(material_size), command_buffer, callback,
      /*msaa_enabled=*/false, /*depth_stencil_enabled=*/false);
  if (!render_target.ok() ||
      !renderer.GetContext()->EnqueueCommandBuffer(std::move(command_buffer))) {
    return std::nullopt;
  }

  SamplerDescriptor output_sampler;
  output_sampler.min_filter = MinMagFilter::kLinear;
  output_sampler.mag_filter = MinMagFilter::kLinear;
  output_sampler.width_address_mode = SamplerAddressMode::kClampToEdge;
  output_sampler.height_address_mode = SamplerAddressMode::kClampToEdge;
  return Entity::FromSnapshot(
      Snapshot{
          .texture = render_target.value().GetRenderTargetTexture(),
          .transform = Matrix::MakeTranslation(material_coverage.GetOrigin()),
          .sampler_descriptor = output_sampler,
          .opacity = 1.0f},
      entity.GetBlendMode());
}

std::optional<Rect> GlassFilterContents::GetFilterCoverage(
    const FilterInput::Vector& inputs,
    const Entity& entity,
    const Matrix& effect_transform) const {
  if (inputs.empty()) {
    return std::nullopt;
  }
  // A backdrop material's shape is supplied in the save layer's local
  // coordinate space, while its coverage hint and scene input are already in
  // render-target coordinates. Transforming the local shape here and then
  // intersecting it with the hint mixes those spaces, shrinking the result by
  // the save layer's screen-space origin. Let the scene input establish broad
  // coverage instead; FilterContents will intersect it with the authoritative
  // save-layer hint. The shape still defines the material's rounded boundary
  // in RenderFilter.
  if (IsBackdropFilter()) {
    return inputs[0]->GetCoverage(entity);
  }
  return shape_.GetBounds().TransformBounds(
      entity.GetTransform() * GetMaterialGeometryTransform(effect_transform));
}

std::optional<Rect> GlassFilterContents::GetFilterSourceCoverage(
    const Matrix& effect_transform,
    const Rect& output_limit) const {
  const Scalar padding = thickness_ * refraction_ * (1.0f + dispersion_);
  const Vector2 transformed =
      effect_transform.TransformDirection(Vector2(padding, padding)).Abs();
  return output_limit.Expand(transformed);
}

}  // namespace impeller
