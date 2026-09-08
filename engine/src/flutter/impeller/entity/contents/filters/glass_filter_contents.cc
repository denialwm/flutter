// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/glass_filter_contents.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "fml/closure.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/texture_contents.h"
#include "impeller/entity/glass.frag.h"
#include "impeller/entity/glass.vert.h"
#include "impeller/renderer/command.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/vertex_buffer_builder.h"

namespace impeller {

namespace {

// A direct material may sample the current parent attachment. Preserve only
// the demanded scene region before drawing into it; cached/offscreen materials
// already render into separate storage and can use the input snapshot directly.
std::optional<Snapshot> CopyGlassScene(const ContentContext& renderer,
                                       const Snapshot& scene,
                                       const Rect& coverage) {
  const auto scene_coverage = scene.GetCoverage();
  const auto copy_coverage =
      scene_coverage ? coverage.Intersection(*scene_coverage) : std::nullopt;
  if (!copy_coverage) {
    return std::nullopt;
  }
  const Rect bounds = Rect::RoundOut(*copy_coverage);
  auto commands = renderer.GetContext()->CreateCommandBuffer();
  if (!commands) {
    return std::nullopt;
  }
  auto target = renderer.MakeSubpass(
      "Denial Glass Scene", ISize::Ceil(bounds.GetSize()), commands,
      [&](const ContentContext& context, RenderPass& pass) {
        Entity copy = Entity::FromSnapshot(scene, BlendMode::kSrc);
        copy.SetTransform(Matrix::MakeTranslation(-bounds.GetOrigin()) *
                          copy.GetTransform());
        return copy.Render(context, pass);
      },
      /*msaa_enabled=*/false, /*depth_stencil_enabled=*/false);
  if (!target.ok() ||
      !renderer.GetContext()->EnqueueCommandBuffer(std::move(commands))) {
    return std::nullopt;
  }
  return Snapshot{
      .texture = target.value().GetRenderTargetTexture(),
      .transform = Matrix::MakeTranslation(bounds.GetOrigin()),
      .sampler_descriptor = scene.sampler_descriptor,
  };
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

Scalar GlassRefractiveIndex(Scalar refraction) {
  // Match liquid_glass_renderer's Figma-compatible refraction control:
  // 0...1 maps from air (1.0) to its established liquid-glass IOR (1.2).
  return 1.0f + std::clamp(refraction, 0.0f, 1.0f) * 0.2f;
}

Scalar MaximumGlassDisplacement(Scalar thickness,
                                Scalar refraction,
                                Scalar dispersion,
                                Scalar bevel_width_scale,
                                Scalar refraction_depth_scale) {
  const Scalar refractive_index = GlassRefractiveIndex(refraction);
  // At the outside edge the surface normal lies in the XY plane and the
  // reference model's optical path is 8 * thickness. This is the maximum ray
  // displacement over the original circular surface. A scaled bevel changes
  // the surface slope, so bound its full path (height + 8 * thickness) by
  // 9 * thickness. Keep the original padding at the default width.
  const Scalar refraction_distance =
      std::max(thickness, 0.0f) * (bevel_width_scale == 1.0f ? 8.0f : 9.0f) *
      refraction_depth_scale *
      std::sqrt(std::max(refractive_index * refractive_index - 1.0f, 0.0f));
  return refraction_distance *
         (1.0f + std::clamp(dispersion, 0.0f, 1.0f) * 0.5f);
}

}  // namespace

std::optional<GlassMaterialDraw> ResolveGlassMaterialDraw(
    const Rect& coverage,
    const Rect& material_bounds,
    const Matrix& material_transform) {
  if (material_bounds.IsEmpty() || !material_transform.IsInvertible()) {
    return std::nullopt;
  }
  const auto draw_coverage = coverage.Intersection(
      material_bounds.TransformBounds(material_transform));
  if (!draw_coverage || draw_coverage->IsEmpty()) {
    return std::nullopt;
  }

  // Evaluate the SDF in physical material coordinates, not the target's
  // axis-aligned bounding box. Corner identities then survive any reflection
  // or rotation without special cases or damage-dependent radius remapping.
  const Vector2 scale = material_transform.GetBasisScaleXY();
  const Matrix target_to_material =
      Matrix::MakeScale(scale) *
      Matrix::MakeTranslation(-material_bounds.GetOrigin()) *
      material_transform.Invert();
  Quad coordinates = draw_coverage->GetPoints();
  for (Point& coordinate : coordinates) {
    coordinate = target_to_material * coordinate;
  }
  // A distance-field gradient transforms with the inverse transpose. Map it
  // back to target space before refraction and lighting use that direction.
  const Matrix normals = target_to_material.Basis().Transpose();
  return GlassMaterialDraw{
      .coverage = *draw_coverage,
      .material_size = Size(material_bounds.GetWidth() * scale.x,
                            material_bounds.GetHeight() * scale.y),
      .material_scale = scale,
      .material_coordinates = coordinates,
      .normal_transform =
          Vector4(normals.GetBasisX2D().x, normals.GetBasisX2D().y,
                  normals.GetBasisY2D().x, normals.GetBasisY2D().y),
  };
}

bool GlassFrostNeedsBlur(Scalar sigma_x, Scalar sigma_y) {
  return sigma_x != 0.0f || sigma_y != 0.0f;
}

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
                                         Scalar edge_strength,
                                         Scalar bevel_width_scale,
                                         Scalar refraction_depth_scale,
                                         Scalar rim_width,
                                         Scalar rim_falloff,
                                         Scalar opposite_light_strength)
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
      edge_strength_(edge_strength),
      bevel_width_scale_(bevel_width_scale),
      refraction_depth_scale_(refraction_depth_scale),
      rim_width_(rim_width),
      rim_falloff_(rim_falloff),
      opposite_light_strength_(opposite_light_strength) {}

GlassFilterContents::~GlassFilterContents() = default;

void GlassFilterContents::SetMaterialTransform(const Matrix& transform) {
  material_transform_ = transform;
}

void GlassFilterContents::SetMaterialTargetPaddingEnabled(bool enabled) {
  material_target_padding_enabled_ = enabled;
}

std::optional<Entity> GlassFilterContents::GetDirectEntity(
    const ContentContext& renderer,
    const Entity& entity,
    const std::optional<Rect>& coverage_hint) {
  const bool previous = std::exchange(render_material_directly_, true);
  fml::ScopedCleanupClosure restore(
      [&] { render_material_directly_ = previous; });
  return GetEntity(renderer, entity, coverage_hint);
}

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

  const Matrix material_transform =
      entity.GetTransform() * material_transform_.value_or(effect_transform);
  const std::optional<GlassMaterialDraw> material_draw =
      ResolveGlassMaterialDraw(material_coverage, shape_.GetBounds(),
                               material_transform);
  if (!material_draw.has_value()) {
    return std::nullopt;
  }
  material_coverage = material_draw->coverage;

  std::optional<Snapshot> scene_snapshot = inputs[0]->GetSnapshot(
      "Denial Glass Scene", renderer, entity, material_coverage);
  if (!scene_snapshot) {
    return std::nullopt;
  }

  // The frost path normally resolves through Impeller's established
  // crop-aware Gaussian filter. Resource pressure must not make an entire
  // backdrop scope disappear, so retain the undiffused scene as a graceful
  // fallback if that intermediate cannot be allocated.
  std::optional<Snapshot> blurred_snapshot = inputs[1]->GetSnapshot(
      "Denial Glass Frost", renderer, entity, material_coverage);
  if (!blurred_snapshot.has_value()) {
    blurred_snapshot = scene_snapshot;
  }

  if (render_material_directly_) {
    const bool frost_uses_scene =
        blurred_snapshot->texture == scene_snapshot->texture;
    // Zero frost (or the allocation fallback) refracts the original scene, so
    // its copy must also include the optical sampling margin.
    const Rect copy_coverage =
        frost_uses_scene
            ? GetFilterSourceCoverage(effect_transform, material_coverage)
                  .value_or(material_coverage)
            : material_coverage;
    scene_snapshot = CopyGlassScene(renderer, *scene_snapshot, copy_coverage);
    if (!scene_snapshot) {
      return std::nullopt;
    }
    if (frost_uses_scene) {
      blurred_snapshot = scene_snapshot;
    }
  }

  const std::optional<Quad> scene_uvs =
      scene_snapshot->GetCoverageUVs(material_coverage);
  const std::optional<Quad> blurred_uvs =
      blurred_snapshot->GetCoverageUVs(material_coverage);
  if (!scene_uvs || !blurred_uvs) {
    return std::nullopt;
  }

  const Size draw_size = material_coverage.GetSize();
  const Size material_size = material_draw->material_size;
  const Quad material_coordinates = material_draw->material_coordinates;
  const Vector4 normal_transform = material_draw->normal_transform;
  const Scalar scale_x = material_draw->material_scale.x;
  const Scalar scale_y = material_draw->material_scale.y;
  const Scalar physical_thickness =
      std::min(thickness_ * (scale_x + scale_y) * 0.5f,
               std::min(material_size.width, material_size.height) * 0.5f);
  const RoundingRadii& radii = shape_.GetRadii();
  const Vector4 corner_radii(
      PhysicalCornerRadius(radii.top_left, scale_x, scale_y),
      PhysicalCornerRadius(radii.top_right, scale_x, scale_y),
      PhysicalCornerRadius(radii.bottom_right, scale_x, scale_y),
      PhysicalCornerRadius(radii.bottom_left, scale_x, scale_y));

  const Quad scene_coordinates = scene_uvs.value();
  const Quad blurred_coordinates = blurred_uvs.value();
  const Vector4 blurred_uv_basis =
      TextureUvBasis(blurred_coordinates, draw_size,
                     blurred_snapshot->texture->GetYCoordScale());

  Contents::RenderProc render_material =
      [scene_snapshot, scene_coordinates, blurred_snapshot, blurred_coordinates,
       draw_size, material_size, material_coordinates, normal_transform,
       corner_radii, blurred_uv_basis, physical_thickness,
       preserve_scene = IsBackdropFilter(),
       refractive_index = GlassRefractiveIndex(refraction_),
       dispersion = dispersion_, saturation = saturation_, tint = tint_,
       tint_strength = tint_strength_, brightness = brightness_,
       light_angle = light_angle_, light_intensity = light_intensity_,
       edge_strength = edge_strength_, bevel_width_scale = bevel_width_scale_,
       refraction_depth_scale = refraction_depth_scale_, rim_width = rim_width_,
       rim_falloff = rim_falloff_,
       opposite_light_strength = opposite_light_strength_](
          const ContentContext& renderer, const Entity& material_entity,
          RenderPass& pass) {
        auto& data = renderer.GetTransientsDataBuffer();
        const std::array<VS::PerVertexData, 4> vertices = {
            VS::PerVertexData{Point(0, 0), blurred_coordinates[0],
                              scene_coordinates[0], material_coordinates[0]},
            VS::PerVertexData{Point(draw_size.width, 0), blurred_coordinates[1],
                              scene_coordinates[1], material_coordinates[1]},
            VS::PerVertexData{Point(0, draw_size.height),
                              blurred_coordinates[2], scene_coordinates[2],
                              material_coordinates[2]},
            VS::PerVertexData{Point(draw_size.width, draw_size.height),
                              blurred_coordinates[3], scene_coordinates[3],
                              material_coordinates[3]},
        };

        VS::FrameInfo frame_info;
        // The allocation rounds up; keep geometry and optical coordinates at
        // their physical size instead of stretching them across that padding.
        frame_info.mvp = material_entity.GetShaderTransform(pass);
        frame_info.blurred_sampler_y_coord_scale =
            blurred_snapshot->texture->GetYCoordScale();

        frame_info.scene_sampler_y_coord_scale =
            scene_snapshot->texture->GetYCoordScale();

        FS::FragInfo frag_info;
        frag_info.normal_transform = normal_transform;
        frag_info.scene_opacity = preserve_scene ? scene_snapshot->opacity : 0;
        frag_info.material_size = Vector2(material_size);
        frag_info.corner_radii = corner_radii;
        frag_info.blurred_uv_basis = blurred_uv_basis;
        frag_info.tint = Vector4(tint.red, tint.green, tint.blue, tint.alpha);
        frag_info.thickness = physical_thickness;
        frag_info.refractive_index = refractive_index;
        frag_info.dispersion = dispersion;
        frag_info.saturation = saturation;
        frag_info.tint_strength = tint_strength;
        frag_info.brightness = brightness;
        frag_info.light_angle = light_angle;
        frag_info.light_intensity = light_intensity;
        frag_info.edge_strength = edge_strength;
        frag_info.bevel_width_scale = bevel_width_scale;
        frag_info.refraction_depth_scale = refraction_depth_scale;
        frag_info.rim_width = rim_width;
        frag_info.rim_falloff = rim_falloff;
        frag_info.opposite_light_strength = opposite_light_strength;
        frag_info.blurred_opacity = blurred_snapshot->opacity;

        SamplerDescriptor blurred_sampler =
            blurred_snapshot->sampler_descriptor;
        blurred_sampler.min_filter = MinMagFilter::kLinear;
        blurred_sampler.mag_filter = MinMagFilter::kLinear;
        blurred_sampler.mip_filter = MipFilter::kBase;
        blurred_sampler.width_address_mode = SamplerAddressMode::kClampToEdge;
        blurred_sampler.height_address_mode = SamplerAddressMode::kClampToEdge;

        auto options = OptionsFromPassAndEntity(pass, material_entity);
        options.primitive_type = PrimitiveType::kTriangleStrip;
        pass.SetPipeline(renderer.GetGlassPipeline(options));
        pass.SetCommandLabel("Denial Glass Material");
        pass.SetVertexBuffer(CreateVertexBuffer(vertices, data));
        VS::BindFrameInfo(pass, data.EmplaceUniform(frame_info));
        FS::BindFragInfo(pass, data.EmplaceUniform(frag_info));
        FS::BindBlurredTextureSampler(
            pass, blurred_snapshot->texture,
            renderer.GetContext()->GetSamplerLibrary()->GetSampler(
                blurred_sampler));
        FS::BindSceneTextureSampler(
            pass, scene_snapshot->texture,
            renderer.GetContext()->GetSamplerLibrary()->GetSampler(
                blurred_sampler));
        return pass.Draw().ok();
      };

  if (render_material_directly_) {
    Entity result;
    result.SetBlendMode(entity.GetBlendMode());
    result.SetTransform(Matrix::MakeTranslation(material_coverage.GetOrigin()));
    result.SetContents(Contents::MakeAnonymous(
        std::move(render_material),
        [draw_size](const Entity& material_entity) -> std::optional<Rect> {
          return Rect::MakeSize(draw_size).TransformBounds(
              material_entity.GetTransform());
        }));
    return result;
  }

  ContentContext::SubpassCallback callback =
      [render_material = std::move(render_material)](
          const ContentContext& renderer, RenderPass& pass) {
        Entity material_entity;
        material_entity.SetBlendMode(BlendMode::kSrc);
        return render_material(renderer, material_entity, pass);
      };

  std::shared_ptr<CommandBuffer> command_buffer =
      renderer.GetContext()->CreateCommandBuffer();
  if (!command_buffer) {
    return std::nullopt;
  }
  const ISize material_pixel_size = ISize::Ceil(draw_size);
  ISize target_size = material_pixel_size;
  const ISize maximum_size = renderer.GetContext()
                                 ->GetCapabilities()
                                 ->GetMaximumRenderPassAttachmentSize();
  if (material_target_padding_enabled_ &&
      material_pixel_size.width <= maximum_size.width &&
      material_pixel_size.height <= maximum_size.height) {
    constexpr int64_t kGranularity = 128;
    target_size =
        ISize{((target_size.width + kGranularity - 1) / kGranularity) *
                  kGranularity,
              ((target_size.height + kGranularity - 1) / kGranularity) *
                  kGranularity}
            .Min(maximum_size);
  }
  fml::StatusOr<RenderTarget> render_target = renderer.MakeSubpass(
      material_target_padding_enabled_ ? "Denial pooled glass material"
                                       : "Denial Glass Material",
      target_size, command_buffer, callback,
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
  if (target_size != material_pixel_size) {
    // Retain the original geometry and texel coordinates. Strict sampling
    // clamps to the original edge texel centers, matching CLAMP_TO_EDGE on
    // the former exact-size texture even at fractional layer translations.
    // The transparent allocation padding is never sampled or composited.
    const Rect source_rect = Rect::MakeSize(material_pixel_size);
    auto contents = TextureContents::MakeRect(source_rect);
    contents->SetTexture(render_target.value().GetRenderTargetTexture());
    contents->SetSourceRect(source_rect);
    contents->SetStrictSourceRect(true);
    contents->SetSamplerDescriptor(output_sampler);
    Entity result;
    result.SetBlendMode(entity.GetBlendMode());
    result.SetTransform(Matrix::MakeTranslation(material_coverage.GetOrigin()));
    result.SetContents(std::move(contents));
    return result;
  }
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
      entity.GetTransform() * material_transform_.value_or(effect_transform));
}

std::optional<Rect> GlassFilterContents::GetFilterSourceCoverage(
    const Matrix& effect_transform,
    const Rect& output_limit) const {
  const Scalar padding =
      MaximumGlassDisplacement(thickness_, refraction_, dispersion_,
                               bevel_width_scale_, refraction_depth_scale_);
  const Vector2 transformed =
      effect_transform.TransformDirection(Vector2(padding, 0)).Abs() +
      effect_transform.TransformDirection(Vector2(0, padding)).Abs();
  return output_limit.Expand(transformed);
}

}  // namespace impeller
