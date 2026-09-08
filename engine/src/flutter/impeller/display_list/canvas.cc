// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/canvas.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include "display_list/dl_vertices.h"
#include "display_list/effects/color_filters/dl_blend_color_filter.h"
#include "display_list/effects/color_filters/dl_matrix_color_filter.h"
#include "display_list/effects/dl_color_filter.h"
#include "display_list/effects/dl_color_source.h"
#include "display_list/effects/dl_image_filter.h"
#include "display_list/effects/image_filters/dl_blur_image_filter.h"
#include "display_list/effects/image_filters/dl_glass_image_filter.h"
#include "display_list/image/dl_image.h"
#include "flutter/fml/closure.h"
#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"
#include "impeller/base/validation.h"
#include "impeller/core/formats.h"
#include "impeller/display_list/color_filter.h"
#include "impeller/display_list/dl_vertices_geometry.h"
#include "impeller/display_list/image_filter.h"
#include "impeller/display_list/skia_conversions.h"
#include "impeller/entity/contents/atlas_contents.h"
#include "impeller/entity/contents/circle_contents.h"
#include "impeller/entity/contents/clip_contents.h"
#include "impeller/entity/contents/color_source_contents.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/filters/filter_contents.h"
#include "impeller/entity/contents/filters/glass_filter_contents.h"
#include "impeller/entity/contents/framebuffer_blend_contents.h"
#include "impeller/entity/contents/line_contents.h"
#include "impeller/entity/contents/shadow_vertices_contents.h"
#include "impeller/entity/contents/solid_color_contents.h"
#include "impeller/entity/contents/solid_rrect_blur_contents.h"
#include "impeller/entity/contents/solid_rsuperellipse_blur_contents.h"
#include "impeller/entity/contents/text_contents.h"
#include "impeller/entity/contents/text_shadow_cache.h"
#include "impeller/entity/contents/texture_contents.h"
#include "impeller/entity/contents/uber_sdf_contents.h"
#include "impeller/entity/contents/vertices_contents.h"
#include "impeller/entity/geometry/arc_geometry.h"
#include "impeller/entity/geometry/circle_geometry.h"
#include "impeller/entity/geometry/cover_geometry.h"
#include "impeller/entity/geometry/ellipse_geometry.h"
#include "impeller/entity/geometry/fill_path_geometry.h"
#include "impeller/entity/geometry/geometry.h"
#include "impeller/entity/geometry/line_geometry.h"
#include "impeller/entity/geometry/point_field_geometry.h"
#include "impeller/entity/geometry/rect_geometry.h"
#include "impeller/entity/geometry/shadow_path_geometry.h"
#include "impeller/entity/geometry/stroke_path_geometry.h"
#include "impeller/entity/save_layer_utils.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/constants.h"
#include "impeller/geometry/rstransform.h"
#include "impeller/renderer/command_buffer.h"

namespace impeller {

namespace {

constexpr Scalar kAntialiasPadding = 1.0f;

bool IsDirectGlassMaterialRequested() {
  static const bool requested = [] {
    const char* value = std::getenv("DENIA_GLASS_DIRECT_MATERIAL");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
  }();
  return requested;
}

bool IsPooledGlassTargetPaddingRequested() {
  static const bool requested = [] {
    const char* value = std::getenv("DENIA_GLASS_POOLED_TARGET_PADDING");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
  }();
  return requested;
}

bool IsPooledGlassMaterialPaddingRequested() {
  static const bool requested = [] {
    const char* value = std::getenv("DENIA_GLASS_POOLED_MATERIAL_PADDING");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
  }();
  return requested;
}

struct BackdropLayerPlanAudit {
  using Clock = std::chrono::steady_clock;

  Clock::time_point period_start = Clock::now();
  uint64_t direct_layers = 0;
  uint64_t single_sample_layers = 0;
  uint64_t multisample_layers = 0;
  uint64_t direct_pixels = 0;
  uint64_t single_sample_pixels = 0;
  uint64_t multisample_pixels = 0;
  uint64_t direct_attempts = 0;
  uint64_t direct_predicate_accepts = 0;
  uint64_t direct_filter_evaluations = 0;
  uint64_t direct_snapshot_builds = 0;
  uint64_t direct_snapshot_hits = 0;
  uint64_t fused_candidates = 0;
  uint64_t fusion_attempts = 0;
  uint64_t fusion_predicate_accepts = 0;
  uint64_t fused_composites = 0;
  uint64_t fused_fallbacks = 0;
  uint64_t deferred_rrect_clips = 0;
  uint64_t analytic_rrect_clips = 0;
  uint64_t flushed_rrect_clips = 0;
  std::array<uint64_t, 7> fusion_rejections = {};
  std::array<uint64_t, 9> direct_rejections = {};
  bool first_report_pending = true;
};

struct BackdropGraphPlanAudit {
  using Clock = std::chrono::steady_clock;

  Clock::time_point period_start = Clock::now();
  uint64_t frames = 0;
  uint64_t scopes = 0;
  uint64_t epochs = 0;
  uint64_t dependency_edges = 0;
  uint64_t write_read_hazards = 0;
  uint64_t write_write_hazards = 0;
  uint64_t read_write_hazards = 0;
  uint64_t scene_barriers = 0;
  uint64_t parallel_rects_max = 0;
  uint64_t epoch_snapshot_flips = 0;
  uint64_t epoch_snapshot_reuses = 0;
  uint64_t epoch_plan_misses = 0;
  bool first_report_pending = true;
};

enum class BackdropEpochExecution {
  kSnapshotFlip,
  kSnapshotReuse,
  kPlanMiss,
};

enum class BackdropDirectSource {
  kFilterEvaluation,
  kSnapshotBuild,
  kSnapshotHit,
};

static bool IsDenialRenderAuditEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("DENIA_RENDER_AUDIT");
    return value != nullptr && value[0] != '\0' &&
           !(value[0] == '0' && value[1] == '\0');
  }();
  return enabled;
}

static BackdropGraphPlanAudit& GetBackdropGraphPlanAudit() {
  static thread_local BackdropGraphPlanAudit audit;
  return audit;
}

static BackdropLayerPlanAudit& GetBackdropLayerPlanAudit() {
  static thread_local BackdropLayerPlanAudit audit;
  return audit;
}

static void RecordBackdropEpochExecution(BackdropEpochExecution execution) {
  if (!IsDenialRenderAuditEnabled()) {
    return;
  }
  BackdropGraphPlanAudit& audit = GetBackdropGraphPlanAudit();
  switch (execution) {
    case BackdropEpochExecution::kSnapshotFlip:
      audit.epoch_snapshot_flips++;
      break;
    case BackdropEpochExecution::kSnapshotReuse:
      audit.epoch_snapshot_reuses++;
      break;
    case BackdropEpochExecution::kPlanMiss:
      audit.epoch_plan_misses++;
      break;
  }
}

static void RecordBackdropLayerPlan(
    ISize size,
    bool use_msaa,
    std::optional<BackdropDirectSource> direct_source = std::nullopt) {
  if (!IsDenialRenderAuditEnabled()) {
    return;
  }

  BackdropLayerPlanAudit& audit = GetBackdropLayerPlanAudit();
  const uint64_t pixels = static_cast<uint64_t>(size.Area());
  if (direct_source.has_value()) {
    audit.direct_layers++;
    audit.direct_pixels += pixels;
    switch (direct_source.value()) {
      case BackdropDirectSource::kFilterEvaluation:
        audit.direct_filter_evaluations++;
        break;
      case BackdropDirectSource::kSnapshotBuild:
        audit.direct_snapshot_builds++;
        break;
      case BackdropDirectSource::kSnapshotHit:
        audit.direct_snapshot_hits++;
        break;
    }
  } else if (use_msaa) {
    audit.multisample_layers++;
    audit.multisample_pixels += pixels;
  } else {
    audit.single_sample_layers++;
    audit.single_sample_pixels += pixels;
  }
}

static void RecordBackdropDirectPredicate(uint32_t rejections) {
  if (!IsDenialRenderAuditEnabled()) {
    return;
  }
  BackdropLayerPlanAudit& audit = GetBackdropLayerPlanAudit();
  audit.direct_attempts++;
  if (rejections == 0u) {
    audit.direct_predicate_accepts++;
    return;
  }
  for (size_t bit = 0u; bit < audit.direct_rejections.size(); bit++) {
    if ((rejections & (1u << bit)) != 0u) {
      audit.direct_rejections[bit]++;
    }
  }
}

enum class BackdropFusionAuditEvent {
  kCandidate,
  kComposite,
  kFallback,
  kDeferredRRect,
  kAnalyticRRect,
  kFlushedRRect,
};

static void RecordBackdropFusionEvent(BackdropFusionAuditEvent event) {
  if (!IsDenialRenderAuditEnabled()) {
    return;
  }
  BackdropLayerPlanAudit& audit = GetBackdropLayerPlanAudit();
  switch (event) {
    case BackdropFusionAuditEvent::kCandidate:
      audit.fused_candidates++;
      break;
    case BackdropFusionAuditEvent::kComposite:
      audit.fused_composites++;
      break;
    case BackdropFusionAuditEvent::kFallback:
      audit.fused_fallbacks++;
      break;
    case BackdropFusionAuditEvent::kDeferredRRect:
      audit.deferred_rrect_clips++;
      break;
    case BackdropFusionAuditEvent::kAnalyticRRect:
      audit.analytic_rrect_clips++;
      break;
    case BackdropFusionAuditEvent::kFlushedRRect:
      audit.flushed_rrect_clips++;
      break;
  }
}

static void RecordBackdropFusionPredicate(uint32_t rejections) {
  if (!IsDenialRenderAuditEnabled()) {
    return;
  }
  BackdropLayerPlanAudit& audit = GetBackdropLayerPlanAudit();
  audit.fusion_attempts++;
  if (rejections == 0u) {
    audit.fusion_predicate_accepts++;
    return;
  }
  for (size_t bit = 0u; bit < audit.fusion_rejections.size(); bit++) {
    if ((rejections & (1u << bit)) != 0u) {
      audit.fusion_rejections[bit]++;
    }
  }
}

static void FlushBackdropLayerPlanAudit() {
  if (!IsDenialRenderAuditEnabled()) {
    return;
  }

  BackdropLayerPlanAudit& audit = GetBackdropLayerPlanAudit();
  const uint64_t layers = audit.direct_layers + audit.single_sample_layers +
                          audit.multisample_layers;
  if (layers == 0u) {
    return;
  }

  const auto now = BackdropLayerPlanAudit::Clock::now();
  const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - audit.period_start);
  if (!audit.first_report_pending && interval < std::chrono::seconds(1)) {
    return;
  }

  FML_LOG(IMPORTANT)
      << "Denial backdrop render plan"
      << " interval_ms=" << interval.count()
      << " direct_layers=" << audit.direct_layers
      << " single_sample_layers=" << audit.single_sample_layers
      << " multisample_layers=" << audit.multisample_layers
      << " direct_pixels=" << audit.direct_pixels
      << " single_sample_pixels=" << audit.single_sample_pixels
      << " multisample_pixels=" << audit.multisample_pixels
      << " avoided_color_samples="
      << audit.direct_pixels * 4u + audit.single_sample_pixels * 3u
      << " direct_attempts=" << audit.direct_attempts
      << " direct_predicate_accepts=" << audit.direct_predicate_accepts
      << " direct_filter_evaluations=" << audit.direct_filter_evaluations
      << " direct_snapshot_builds=" << audit.direct_snapshot_builds
      << " direct_snapshot_hits=" << audit.direct_snapshot_hits
      << " fused_candidates=" << audit.fused_candidates
      << " fusion_attempts=" << audit.fusion_attempts
      << " fusion_predicate_accepts=" << audit.fusion_predicate_accepts
      << " fused_composites=" << audit.fused_composites
      << " fused_fallbacks=" << audit.fused_fallbacks
      << " deferred_rrect_clips=" << audit.deferred_rrect_clips
      << " analytic_rrect_clips=" << audit.analytic_rrect_clips
      << " flushed_rrect_clips=" << audit.flushed_rrect_clips
      << " fusion_reject_backend=" << audit.fusion_rejections[0]
      << " fusion_reject_backdrop_texture=" << audit.fusion_rejections[1]
      << " fusion_reject_external_surface=" << audit.fusion_rejections[2]
      << " fusion_reject_surface_sampling=" << audit.fusion_rejections[3]
      << " fusion_reject_surface_blend=" << audit.fusion_rejections[4]
      << " fusion_reject_transform=" << audit.fusion_rejections[5]
      << " fusion_reject_coverage=" << audit.fusion_rejections[6]
      << " reject_missing_backdrop=" << audit.direct_rejections[0]
      << " reject_content_msaa=" << audit.direct_rejections[1]
      << " reject_uncontained_bounds=" << audit.direct_rejections[2]
      << " reject_nested_pass=" << audit.direct_rejections[3]
      << " reject_restore_blend=" << audit.direct_rejections[4]
      << " reject_restore_alpha=" << audit.direct_rejections[5]
      << " reject_restore_effects=" << audit.direct_rejections[6]
      << " reject_shared_backdrop=" << audit.direct_rejections[7]
      << " reject_inherited_opacity=" << audit.direct_rejections[8];
  audit = BackdropLayerPlanAudit{.period_start = now,
                                 .first_report_pending = false};
}

static void RecordBackdropGraphPlan(const BackdropEpochPlan& plan) {
  if (!IsDenialRenderAuditEnabled()) {
    return;
  }

  BackdropGraphPlanAudit& audit = GetBackdropGraphPlanAudit();
  audit.frames++;
  audit.scopes += plan.epoch_for_scope.size();
  audit.epochs += plan.scopes_by_epoch.size();
  audit.dependency_edges += plan.dependency_edges;
  audit.write_read_hazards += plan.write_read_hazards;
  audit.write_write_hazards += plan.write_write_hazards;
  audit.read_write_hazards += plan.read_write_hazards;
  audit.scene_barriers += plan.scene_barriers;
  audit.parallel_rects_max =
      std::max<uint64_t>(audit.parallel_rects_max, plan.GetMaxEpochWidth());
}

static void FlushBackdropGraphPlanAudit() {
  if (!IsDenialRenderAuditEnabled()) {
    return;
  }

  BackdropGraphPlanAudit& audit = GetBackdropGraphPlanAudit();
  const auto now = BackdropGraphPlanAudit::Clock::now();
  const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - audit.period_start);
  if (!audit.first_report_pending && interval < std::chrono::seconds(1)) {
    return;
  }

  const double parallel_rects_avg =
      audit.epochs == 0u ? 0.0
                         : static_cast<double>(audit.scopes) / audit.epochs;
  FML_LOG(IMPORTANT) << "Denial backdrop graph plan"
                     << " interval_ms=" << interval.count()
                     << " frames=" << audit.frames << " scopes=" << audit.scopes
                     << " epochs=" << audit.epochs
                     << " dependency_edges=" << audit.dependency_edges
                     << " write_read_hazards=" << audit.write_read_hazards
                     << " write_write_hazards=" << audit.write_write_hazards
                     << " read_write_hazards=" << audit.read_write_hazards
                     << " scene_barriers=" << audit.scene_barriers
                     << " parallel_rects_avg=" << parallel_rects_avg
                     << " parallel_rects_max=" << audit.parallel_rects_max
                     << " epoch_snapshot_flips=" << audit.epoch_snapshot_flips
                     << " epoch_snapshot_reuses=" << audit.epoch_snapshot_reuses
                     << " epoch_plan_misses=" << audit.epoch_plan_misses;
  audit = BackdropGraphPlanAudit{.period_start = now,
                                 .first_report_pending = false};
}

bool IsPipelineBlendOrMatrixFilter(const flutter::DlColorFilter* filter) {
  return filter->type() == flutter::DlColorFilterType::kMatrix ||
         (filter->type() == flutter::DlColorFilterType::kBlend &&
          filter->asBlend()->mode() <= Entity::kLastPipelineBlendMode);
}

static bool UseColorSourceContents(
    const std::shared_ptr<VerticesGeometry>& vertices,
    const Paint& paint) {
  // If there are no vertex color or texture coordinates. Or if there
  // are vertex coordinates but its just a color.
  if (vertices->HasVertexColors()) {
    return false;
  }
  if (vertices->HasTextureCoordinates() && !paint.color_source) {
    return true;
  }
  return !vertices->HasTextureCoordinates();
}

static IRect32 SetClipScissor(std::optional<Rect> clip_coverage,
                              RenderPass& pass,
                              Point global_pass_position) {
  // Set the scissor to the clip coverage area. We do this prior to rendering
  // the clip itself and all its contents.
  IRect32 scissor;
  if (clip_coverage.has_value()) {
    clip_coverage = clip_coverage->Shift(-global_pass_position);
    scissor = IRect32::RoundOut(clip_coverage.value());
    // The scissor rect must not exceed the size of the render target.
    scissor =
        scissor.Intersection(IRect32::MakeSize(pass.GetRenderTargetSize()))
            .value_or(IRect32());
  }
  pass.SetScissor(scissor);
  return scissor;
}

static void ApplyFramebufferBlend(Entity& entity) {
  auto src_contents = entity.GetContents();
  auto contents = std::make_shared<FramebufferBlendContents>();
  contents->SetChildContents(src_contents);
  contents->SetBlendMode(entity.GetBlendMode());
  entity.SetContents(std::move(contents));
  entity.SetBlendMode(BlendMode::kSrc);
}

/// @brief Create the subpass restore contents, appling any filters or opacity
///        from the provided paint object.
static std::shared_ptr<Contents> CreateContentsForSubpassTarget(
    const Paint& paint,
    const std::shared_ptr<Texture>& target,
    const Matrix& effect_transform,
    std::string_view label,
    const std::optional<ISize>& texture_region) {
  const Rect region =
      Rect::MakeSize(texture_region.value_or(target->GetSize()));
  auto contents = TextureContents::MakeRect(region);
  contents->SetTexture(target);
  contents->SetLabel(label);
  contents->SetSourceRect(region);
  contents->SetOpacity(paint.color.alpha);
  contents->SetDeferApplyingOpacity(true);

  return paint.WithFiltersForSubpassTarget(std::move(contents),
                                           effect_transform);
}

static const constexpr RenderTarget::AttachmentConfig kDefaultStencilConfig =
    RenderTarget::AttachmentConfig{
        .storage_mode = StorageMode::kDeviceTransient,
        .load_action = LoadAction::kDontCare,
        .store_action = StoreAction::kDontCare,
    };

static const constexpr RenderTarget::AttachmentConfig kPersistentStencilConfig =
    RenderTarget::AttachmentConfig{
        .storage_mode = StorageMode::kDevicePrivate,
        .load_action = LoadAction::kDontCare,
        .store_action = StoreAction::kDontCare,
    };

static std::unique_ptr<EntityPassTarget> CreateRenderTarget(
    ContentContext& renderer,
    ISize size,
    const Color& clear_color,
    bool use_msaa,
    bool preserve_depth_stencil_between_passes = false,
    bool pooled_glass_layer = false) {
  const std::shared_ptr<Context>& context = renderer.GetContext();

  /// All of the load/store actions are managed by `InlinePassContext` when
  /// `RenderPasses` are created, so we just set them to `kDontCare` here.
  /// What's important is the `StorageMode` of the textures, which cannot be
  /// changed for the lifetime of the textures.

  RenderTarget target;
  const RenderTarget::AttachmentConfig stencil_config =
      preserve_depth_stencil_between_passes ? kPersistentStencilConfig
                                            : kDefaultStencilConfig;
  if (use_msaa && context->GetCapabilities()->SupportsOffscreenMSAA()) {
    target = renderer.GetRenderTargetCache()->CreateOffscreenMSAA(
        /*context=*/*context,
        /*size=*/size,
        /*mip_count=*/1,
        /*label=*/
        pooled_glass_layer ? "Denial pooled glass layer" : "EntityPass",
        /*color_attachment_config=*/
        RenderTarget::AttachmentConfigMSAA{
            .storage_mode = StorageMode::kDeviceTransient,
            .resolve_storage_mode = StorageMode::kDevicePrivate,
            .load_action = LoadAction::kDontCare,
            .store_action = StoreAction::kMultisampleResolve,
            .clear_color = clear_color},
        /*stencil_attachment_config=*/stencil_config);
  } else {
    target = renderer.GetRenderTargetCache()->CreateOffscreen(
        *context,  // context
        size,      // size
        /*mip_count=*/1,
        "EntityPass",  // label
        RenderTarget::AttachmentConfig{
            .storage_mode = StorageMode::kDevicePrivate,
            .load_action = LoadAction::kDontCare,
            .store_action = StoreAction::kDontCare,
            .clear_color = clear_color,
        },              // color_attachment_config
        stencil_config  //
    );
  }

  return std::make_unique<EntityPassTarget>(
      target,                                                           //
      renderer.GetDeviceCapabilities().SupportsReadFromResolve(),       //
      renderer.GetDeviceCapabilities().SupportsImplicitResolvingMSAA()  //
  );
}

}  // namespace

class Canvas::RRectBlurShape : public BlurShape {
 public:
  RRectBlurShape(const Rect& rect, Scalar corner_radius)
      : rect_(rect), corner_radius_(corner_radius) {}

  Rect GetBounds() const override { return rect_; }

  std::shared_ptr<SolidBlurContents> BuildBlurContent(Sigma sigma) override {
    auto contents = std::make_shared<SolidRRectBlurContents>();
    contents->SetSigma(sigma);
    contents->SetShape(rect_, corner_radius_);
    return contents;
  }

  const Geometry& BuildDrawGeometry() override {
    return geom_.emplace(rect_, Size(corner_radius_));
  }

 private:
  const Rect rect_;
  const Scalar corner_radius_;

  std::optional<RoundRectGeometry> geom_;  // optional stack allocation
};

class Canvas::RSuperellipseBlurShape : public BlurShape {
 public:
  RSuperellipseBlurShape(const Rect& rect, Scalar corner_radius)
      : rect_(rect), corner_radius_(corner_radius) {}

  Rect GetBounds() const override { return rect_; }

  std::shared_ptr<SolidBlurContents> BuildBlurContent(Sigma sigma) override {
    auto contents = std::make_shared<SolidRSuperellipseBlurContents>();
    contents->SetSigma(sigma);
    contents->SetShape(rect_, corner_radius_);
    return contents;
  }

  const Geometry& BuildDrawGeometry() override {
    return geom_.emplace(rect_, corner_radius_);
  }

 private:
  const Rect rect_;
  const Scalar corner_radius_;

  std::optional<RoundSuperellipseGeometry> geom_;  // optional stack allocation
};

class Canvas::PathBlurShape : public BlurShape {
 public:
  /// Construct a PathBlurShape from a path source, a set of shadow vertices
  /// (typically produced by ShadowPathGeometry) and the sigma that was used
  /// to generate the vertex mesh.
  ///
  /// The sigma was already used to generate the shadow vertices, so it is
  /// provided here only to make sure it matches the sigma we will see in
  /// our BuildBlurContent method.
  ///
  /// The source was used to generate the mesh and it might be used again
  /// for the SOLID mask operation so we save it here in case the mask
  /// rendering code calls our BuildDrawGeometry method. Its lifetime
  /// must survive the lifetime of this object, typically because the
  /// source object was stack allocated not long before this object is
  /// also being stack allocated.
  PathBlurShape(const PathSource& source [[clang::lifetimebound]],
                std::shared_ptr<ShadowVertices> shadow_vertices,
                Sigma sigma)
      : sigma_(sigma),
        source_(source),
        shadow_vertices_(std::move(shadow_vertices)) {}

  Rect GetBounds() const override {
    return shadow_vertices_->GetBounds().value_or(Rect());
  }

  std::shared_ptr<SolidBlurContents> BuildBlurContent(Sigma sigma) override {
    // We have to use the sigma to generate the mesh up front in order to
    // even know if we can perform the operation, but then the method that
    // actually uses our contents informs us of the sigma, but it's too
    // late to make use of it. Instead we remember what sigma we used and
    // make sure they match.
    FML_DCHECK(sigma_.sigma == sigma.sigma);
    return ShadowVerticesContents::Make(shadow_vertices_);
  }

  const Geometry& BuildDrawGeometry() override {
    return source_geometry_.emplace(source_);
  }

 private:
  const Sigma sigma_;
  const PathSource& source_;
  const std::shared_ptr<ShadowVertices> shadow_vertices_;

  // optional stack allocation - for BuildGeometry
  std::optional<FillPathFromSourceGeometry> source_geometry_;
};

Canvas::Canvas(ContentContext& renderer,
               const RenderTarget& render_target,
               bool is_onscreen,
               bool requires_readback)
    : renderer_(renderer),
      render_target_(render_target),
      is_onscreen_(is_onscreen),
      requires_readback_(requires_readback),
      clip_coverage_stack_(EntityPassClipStack(
          Rect::MakeSize(render_target.GetRenderTargetSize()))) {
  Initialize(std::nullopt);
  SetupRenderPass();
}

Canvas::Canvas(ContentContext& renderer,
               const RenderTarget& render_target,
               bool is_onscreen,
               bool requires_readback,
               Rect cull_rect)
    : renderer_(renderer),
      render_target_(render_target),
      is_onscreen_(is_onscreen),
      requires_readback_(requires_readback),
      clip_coverage_stack_(EntityPassClipStack(
          Rect::MakeSize(render_target.GetRenderTargetSize()))) {
  Initialize(cull_rect);
  SetupRenderPass();
}

Canvas::Canvas(ContentContext& renderer,
               const RenderTarget& render_target,
               bool is_onscreen,
               bool requires_readback,
               IRect32 cull_rect)
    : renderer_(renderer),
      render_target_(render_target),
      is_onscreen_(is_onscreen),
      requires_readback_(requires_readback),
      clip_coverage_stack_(EntityPassClipStack(
          Rect::MakeSize(render_target.GetRenderTargetSize()))) {
  Initialize(Rect::MakeLTRB(cull_rect.GetLeft(), cull_rect.GetTop(),
                            cull_rect.GetRight(), cull_rect.GetBottom()));
  SetupRenderPass();
}

void Canvas::Initialize(std::optional<Rect> cull_rect) {
  initial_cull_rect_ = cull_rect;
  transform_stack_.emplace_back(CanvasStackEntry{
      .clip_depth = kMaxDepth,
  });
  FML_DCHECK(GetSaveCount() == 1u);
}

void Canvas::Reset() {
  current_depth_ = 0u;
  transform_stack_ = {};
}

void Canvas::Concat(const Matrix& transform) {
  transform_stack_.back().transform = GetCurrentTransform() * transform;
}

void Canvas::PreConcat(const Matrix& transform) {
  transform_stack_.back().transform = transform * GetCurrentTransform();
}

void Canvas::ResetTransform() {
  transform_stack_.back().transform = {};
}

void Canvas::Transform(const Matrix& transform) {
  Concat(transform);
}

const Matrix& Canvas::GetCurrentTransform() const {
  return transform_stack_.back().transform;
}

void Canvas::Translate(const Vector3& offset) {
  Concat(Matrix::MakeTranslation(offset));
}

void Canvas::Scale(const Vector2& scale) {
  Concat(Matrix::MakeScale(scale));
}

void Canvas::Scale(const Vector3& scale) {
  Concat(Matrix::MakeScale(scale));
}

void Canvas::Skew(Scalar sx, Scalar sy) {
  Concat(Matrix::MakeSkew(sx, sy));
}

void Canvas::Rotate(Radians radians) {
  Concat(Matrix::MakeRotationZ(radians));
}

Point Canvas::GetGlobalPassPosition() const {
  if (save_layer_state_.empty()) {
    return Point(0, 0);
  }
  return save_layer_state_.back().coverage.GetOrigin();
}

// clip depth of the previous save or 0.
size_t Canvas::GetClipHeightFloor() const {
  if (transform_stack_.size() > 1) {
    return transform_stack_[transform_stack_.size() - 2].clip_height;
  }
  return 0;
}

size_t Canvas::GetSaveCount() const {
  return transform_stack_.size();
}

bool Canvas::IsSkipping() const {
  return transform_stack_.back().skipping;
}

void Canvas::RestoreToCount(size_t count) {
  while (GetSaveCount() > count) {
    if (!Restore()) {
      return;
    }
  }
}

void Canvas::DrawPath(const flutter::DlPath& path, const Paint& paint) {
  if (IsShadowBlurDrawOperation(paint)) {
    if (AttemptDrawBlurredPathSource(path, paint)) {
      return;
    }
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  if (paint.style == Paint::Style::kFill) {
    FillPathGeometry geom(path);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  } else {
    StrokePathGeometry geom(path, paint.stroke);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  }
}

void Canvas::DrawPaint(const Paint& paint) {
  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  CoverGeometry geom;
  AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
}

// Optimization: if the texture has a color filter that is a simple
// porter-duff blend or matrix filter, then instead of performing a save layer
// we should swap out the shader for the porter duff blend shader and avoid a
// saveLayer. This optimization is important for Flame.
bool Canvas::AttemptColorFilterOptimization(
    const std::shared_ptr<Texture>& image,
    Rect source,
    Rect dest,
    const Paint& paint,
    const SamplerDescriptor& sampler,
    SourceRectConstraint src_rect_constraint) {
  if (!paint.color_filter ||                     //
      paint.image_filter != nullptr ||           //
      paint.invert_colors ||                     //
      paint.mask_blur_descriptor.has_value() ||  //
      !IsPipelineBlendOrMatrixFilter(paint.color_filter)) {
    return false;
  }

  if (paint.color_filter->type() == flutter::DlColorFilterType::kBlend) {
    const flutter::DlBlendColorFilter* blend_filter =
        paint.color_filter->asBlend();
    DrawImageRectAtlasGeometry geometry = DrawImageRectAtlasGeometry(
        /*texture=*/image,
        /*source=*/source,
        /*destination=*/dest,
        /*color=*/skia_conversions::ToColor(blend_filter->color()),
        /*blend_mode=*/blend_filter->mode(),
        /*desc=*/sampler,
        /*use_strict_src_rect=*/src_rect_constraint ==
            SourceRectConstraint::kStrict);

    auto atlas_contents = std::make_shared<AtlasContents>();
    atlas_contents->SetGeometry(&geometry);
    atlas_contents->SetAlpha(paint.color.alpha);

    Entity entity;
    entity.SetTransform(GetCurrentTransform());
    entity.SetBlendMode(paint.blend_mode);
    entity.SetContents(atlas_contents);

    AddRenderEntityToCurrentPass(entity);
  } else {
    // src_rect_constraint is only supported in the porter-duff mode
    // for now.
    if (src_rect_constraint == SourceRectConstraint::kStrict) {
      return false;
    }

    const flutter::DlMatrixColorFilter* matrix_filter =
        paint.color_filter->asMatrix();

    DrawImageRectAtlasGeometry geometry = DrawImageRectAtlasGeometry(
        /*texture=*/image,
        /*source=*/source,
        /*destination=*/dest,
        /*color=*/Color::Khaki(),            // ignored
        /*blend_mode=*/BlendMode::kSrcOver,  // ignored
        /*desc=*/sampler,
        /*use_strict_src_rect=*/src_rect_constraint ==
            SourceRectConstraint::kStrict);

    auto atlas_contents = std::make_shared<ColorFilterAtlasContents>();
    atlas_contents->SetGeometry(&geometry);
    atlas_contents->SetAlpha(paint.color.alpha);
    impeller::ColorMatrix color_matrix;
    matrix_filter->get_matrix(color_matrix.array);
    atlas_contents->SetMatrix(color_matrix);

    Entity entity;
    entity.SetTransform(GetCurrentTransform());
    entity.SetBlendMode(paint.blend_mode);
    entity.SetContents(atlas_contents);

    AddRenderEntityToCurrentPass(entity);
  }
  return true;
}

bool Canvas::AttemptDrawAntialiasedCircle(const Point& center,
                                          Scalar radius,
                                          const Paint& paint) {
  if (paint.HasColorFilter() || paint.image_filter || paint.invert_colors ||
      paint.color_source || paint.mask_blur_descriptor.has_value()) {
    return false;
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  const bool is_stroked = paint.style == Paint::Style::kStroke;
  std::unique_ptr<CircleGeometry> geom;
  if (is_stroked) {
    geom = std::make_unique<CircleGeometry>(center, radius, paint.stroke.width);
  } else {
    geom = std::make_unique<CircleGeometry>(center, radius);
  }
  geom->SetAntialiasPadding(kAntialiasPadding);

  auto contents =
      CircleContents::Make(std::move(geom), paint.color, is_stroked);
  entity.SetContents(std::move(contents));
  AddRenderEntityToCurrentPass(entity);

  return true;
}

bool Canvas::IsShadowBlurDrawOperation(const Paint& paint) {
  if (paint.style != Paint::Style::kFill) {
    return false;
  }

  if (paint.color_source) {
    return false;
  }

  if (!paint.mask_blur_descriptor.has_value()) {
    return false;
  }

  // A blur sigma that is not positive enough should not result in a blur.
  // We test both the sigma value and the converted radius value as the
  // algorithms might use either and either indicates the blur is too small
  // to be noticeable.
  if (paint.mask_blur_descriptor->sigma.sigma <= kEhCloseEnough) {
    return false;
  }
  Radius radius = paint.mask_blur_descriptor->sigma;
  if (radius.radius <= kEhCloseEnough) {
    return false;
  }

  return true;
}

bool Canvas::AttemptDrawBlurredPathSource(const PathSource& source,
                                          const Paint& paint) {
  FML_DCHECK(IsShadowBlurDrawOperation);

  // This has_value() test should always succeed as it is checked by the
  // IsShadowBlurDrawOperation method which should have been called before
  // this method, but we check again here to avoid warnings from the
  // following code.
  if (paint.mask_blur_descriptor.has_value()) {
    // This value was determined by empirical eyesight tests so that the
    // shadow mesh results will match the results of the shape-specific
    // optimized shadow shaders.
    static constexpr Scalar kSigmaScale = 2.8f;

    Sigma sigma = paint.mask_blur_descriptor->sigma;
    const Matrix& matrix = GetCurrentTransform();
    Scalar basis_scale = matrix.GetMaxBasisLengthXY();
    Scalar device_radius = sigma.sigma * kSigmaScale * basis_scale;
    std::shared_ptr<ShadowVertices> shadow_vertices =
        ShadowPathGeometry::MakeAmbientShadowVertices(
            renderer_.GetTessellator(), source, device_radius, matrix);
    if (shadow_vertices) {
      PathBlurShape shape(source, std::move(shadow_vertices), sigma);
      return AttemptDrawBlur(shape, paint);
    }
  }
  return false;
}

Scalar Canvas::GetCommonRRectLikeRadius(const RoundingRadii& radii) {
  if (!radii.AreAllCornersSame()) {
    return -1;
  }
  const Size& corner_radii = radii.top_left;
  if (ScalarNearlyEqual(corner_radii.width, corner_radii.height)) {
    return corner_radii.width;
  }
  return -1;
}

bool Canvas::AttemptDrawBlurredRRect(const RoundRect& round_rect,
                                     const Paint& paint) {
  Scalar radius = GetCommonRRectLikeRadius(round_rect.GetRadii());
  if (radius < 0) {
    RoundRectPathSource source(round_rect);
    return AttemptDrawBlurredPathSource(source, paint);
  }
  RRectBlurShape shape(round_rect.GetBounds(), radius);
  return AttemptDrawBlur(shape, paint);
}

bool Canvas::AttemptDrawBlurredRSuperellipse(const RoundSuperellipse& rse,
                                             const Paint& paint) {
  Scalar radius = GetCommonRRectLikeRadius(rse.GetRadii());
  if (radius < 0) {
    RoundSuperellipsePathSource source(rse);
    return AttemptDrawBlurredPathSource(source, paint);
  }
  RSuperellipseBlurShape shape(rse.GetBounds(), radius);
  return AttemptDrawBlur(shape, paint);
}

bool Canvas::AttemptDrawBlur(BlurShape& shape, const Paint& paint) {
  FML_DCHECK(IsShadowBlurDrawOperation(paint));

  // For symmetrically mask blurred solid RRects, absorb the mask blur and use
  // a faster SDF approximation.
  Color rrect_color = paint.color;
  if (paint.invert_colors) {
    rrect_color = rrect_color.ApplyColorMatrix(kColorInversion);
  }
  if (paint.color_filter) {
    rrect_color = GetCPUColorFilterProc(paint.color_filter)(rrect_color);
  }

  Paint rrect_paint = {.mask_blur_descriptor = paint.mask_blur_descriptor};

  if (!rrect_paint.mask_blur_descriptor.has_value()) {
    // This should never happen in practice because the caller would have
    // first called |IsShadowBlurDrawOperation| on the paint object, but
    // we test anyway to make the compiler happy about the dereferences
    // below.
    return false;
  }

  // In some cases, we need to render the mask blur to a separate layer.
  //
  //   1. If the blur style is normal, we'll be drawing using one draw call and
  //      no clips. And so we can just wrap the RRect contents with the
  //      ImageFilter, which will get applied to the result as per usual.
  //
  //   2. If the blur style is solid, we combine the non-blurred RRect with the
  //      blurred RRect via two separate draw calls, and so we need to defer any
  //      fancy blending, translucency, or image filtering until after these two
  //      draws have been combined in a separate layer.
  //
  //   3. If the blur style is outer or inner, we apply the blur style via a
  //      clip. The ImageFilter needs to be applied to the mask blurred result.
  //      And so if there's an ImageFilter, we need to defer applying it until
  //      after the clipped RRect blur has been drawn to a separate texture.
  //      However, since there's only one draw call that produces color, we
  //      don't need to worry about the blend mode or translucency (unlike with
  //      BlurStyle::kSolid).
  //
  if ((paint.mask_blur_descriptor->style !=
           FilterContents::BlurStyle::kNormal &&
       paint.image_filter) ||
      (paint.mask_blur_descriptor->style == FilterContents::BlurStyle::kSolid &&
       (!rrect_color.IsOpaque() || paint.blend_mode != BlendMode::kSrcOver))) {
    Rect render_bounds = shape.GetBounds();
    if (paint.mask_blur_descriptor->style !=
        FilterContents::BlurStyle::kInner) {
      render_bounds =
          render_bounds.Expand(paint.mask_blur_descriptor->sigma.sigma * 4.0);
    }
    // Defer the alpha, blend mode, and image filter to a separate layer.
    SaveLayer(
        Paint{
            .color = Color::White().WithAlpha(rrect_color.alpha),
            .image_filter = paint.image_filter,
            .blend_mode = paint.blend_mode,
        },
        render_bounds, nullptr, ContentBoundsPromise::kContainsContents, 1u);
    rrect_paint.color = rrect_color.WithAlpha(1);
  } else {
    rrect_paint.color = rrect_color;
    rrect_paint.blend_mode = paint.blend_mode;
    rrect_paint.image_filter = paint.image_filter;
    Save(1u);
  }

  auto draw_blurred_rrect = [this, &rrect_paint, &shape]() {
    std::shared_ptr<SolidBlurContents> contents =
        shape.BuildBlurContent(rrect_paint.mask_blur_descriptor->sigma);
    FML_DCHECK(contents);

    contents->SetColor(rrect_paint.color);

    Entity blurred_rrect_entity;
    blurred_rrect_entity.SetTransform(GetCurrentTransform());
    blurred_rrect_entity.SetBlendMode(rrect_paint.blend_mode);

    rrect_paint.mask_blur_descriptor = std::nullopt;
    blurred_rrect_entity.SetContents(
        rrect_paint.WithFilters(std::move(contents)));
    AddRenderEntityToCurrentPass(blurred_rrect_entity);
  };

  switch (rrect_paint.mask_blur_descriptor->style) {
    case FilterContents::BlurStyle::kNormal: {
      draw_blurred_rrect();
      break;
    }
    case FilterContents::BlurStyle::kSolid: {
      // First, draw the blurred RRect.
      draw_blurred_rrect();
      // Then, draw the non-blurred RRect on top.
      Entity entity;
      entity.SetTransform(GetCurrentTransform());
      entity.SetBlendMode(rrect_paint.blend_mode);

      const Geometry& geom = shape.BuildDrawGeometry();
      AddRenderEntityWithFiltersToCurrentPass(entity, &geom, rrect_paint,
                                              /*reuse_depth=*/true);
      break;
    }
    case FilterContents::BlurStyle::kOuter: {
      const Geometry& geom = shape.BuildDrawGeometry();
      ClipGeometry(geom, Entity::ClipOperation::kDifference);
      draw_blurred_rrect();
      break;
    }
    case FilterContents::BlurStyle::kInner: {
      const Geometry& geom = shape.BuildDrawGeometry();
      ClipGeometry(geom, Entity::ClipOperation::kIntersect);
      draw_blurred_rrect();
      break;
    }
  }

  Restore();

  return true;
}

void Canvas::DrawLine(const Point& p0,
                      const Point& p1,
                      const Paint& paint,
                      bool reuse_depth) {
  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  auto geometry = std::make_unique<LineGeometry>(p0, p1, paint.stroke);

  if ((renderer_.GetContext()->GetFlags().antialiased_lines ||
       renderer_.GetContext()->GetFlags().use_sdfs) &&
      !paint.color_filter && !paint.invert_colors && !paint.image_filter &&
      !paint.mask_blur_descriptor.has_value() && !paint.color_source) {
    auto contents = LineContents::Make(std::move(geometry), paint.color);
    entity.SetContents(std::move(contents));
    AddRenderEntityToCurrentPass(entity, reuse_depth);
  } else {
    AddRenderEntityWithFiltersToCurrentPass(entity, geometry.get(), paint,
                                            /*reuse_depth=*/reuse_depth);
  }
}

void Canvas::DrawDashedLine(const Point& p0,
                            const Point& p1,
                            Scalar on_length,
                            Scalar off_length,
                            const Paint& paint) {
  // Reasons to defer to regular DrawLine:
  // - performance for degenerate and "regular line" cases
  // - length is non-positive - DrawLine will draw appropriate "dot"
  // - off_length is non-positive - no gaps, DrawLine will draw it solid
  // - on_length is negative - invalid dashing
  //
  // Note that a 0 length "on" dash will draw "dot"s every "off" distance
  // apart so we proceed with the dashing process in that case.
  Scalar length = p0.GetDistance(p1);
  if (length > 0.0f && on_length >= 0.0f && off_length > 0.0f) {
    Entity entity;
    entity.SetTransform(GetCurrentTransform());
    entity.SetBlendMode(paint.blend_mode);

    StrokeDashedLineGeometry geom(p0, p1, on_length, off_length, paint.stroke);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  } else {
    DrawLine(p0, p1, paint);
  }
}

void Canvas::DrawRect(const Rect& rect, const Paint& paint) {
  if (IsShadowBlurDrawOperation(paint)) {
    RRectBlurShape shape(rect, 0.0f);
    if (AttemptDrawBlur(shape, paint)) {
      return;
    }
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  if (renderer_.GetContext()->GetFlags().use_sdfs &&
      !paint.mask_blur_descriptor.has_value()) {
    Scalar expand_size = kAntialiasPadding;
    if (paint.style == Paint::Style::kStroke) {
      expand_size += LineGeometry::ComputePixelHalfWidth(GetCurrentTransform(),
                                                         paint.stroke.width);
    }

    FillRectGeometry geometry(rect);
    geometry.SetAntialiasPadding(expand_size);

    auto contents = UberSDFContents::MakeRect(
        /*color=*/paint.color, /*stroke_width=*/paint.stroke.width,
        /*stroke_join=*/paint.stroke.join,
        /*stroked=*/paint.style == Paint::Style::kStroke, &geometry);

    const Geometry* geom = contents->GetGeometry();

    AddRenderSDFEntityToCurrentPass(entity, geom, paint, std::move(contents));
    return;
  }

  if (paint.style == Paint::Style::kStroke) {
    StrokeRectGeometry geom(rect, paint.stroke);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  } else {
    FillRectGeometry geom(rect);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  }
}

void Canvas::DrawOval(const Rect& rect, const Paint& paint) {
  // TODO(jonahwilliams): This additional condition avoids an assert in the
  // stroke circle geometry generator. I need to verify the condition that this
  // assert prevents.
  if (rect.IsSquare() && (paint.style == Paint::Style::kFill ||
                          (paint.style == Paint::Style::kStroke &&
                           paint.stroke.width < rect.GetWidth()))) {
    // Circles have slightly less overhead and can do stroking
    DrawCircle(rect.GetCenter(), rect.GetWidth() * 0.5f, paint);
    return;
  }

  if (IsShadowBlurDrawOperation(paint)) {
    if (rect.IsSquare()) {
      // RRectBlurShape takes the corner radii which are half of the
      // overall width and height of the DrawOval bounds rect.
      RRectBlurShape shape(rect, rect.GetWidth() * 0.5f);
      if (AttemptDrawBlur(shape, paint)) {
        return;
      }
    } else {
      EllipsePathSource source(rect);
      if (AttemptDrawBlurredPathSource(source, paint)) {
        return;
      }
    }
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  if (paint.style == Paint::Style::kStroke) {
    StrokeEllipseGeometry geom(rect, paint.stroke);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  } else {
    EllipseGeometry geom(rect);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  }
}

void Canvas::DrawArc(const Arc& arc, const Paint& paint) {
  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  if (paint.style == Paint::Style::kFill) {
    ArcGeometry geom(arc);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
    return;
  }

  const Rect& oval_bounds = arc.GetOvalBounds();
  if (paint.stroke.width > oval_bounds.GetSize().MaxDimension()) {
    // This is a special case for rendering arcs whose stroke width is so large
    // you are effectively drawing a sector of a circle.
    // https://github.com/flutter/flutter/issues/158567
    Arc expanded_arc(oval_bounds.Expand(Size(paint.stroke.width * 0.5f)),
                     arc.GetStart(), arc.GetSweep(), true);

    ArcGeometry geom(expanded_arc);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
    return;
  }

  // IncludeCenter incurs lots of extra work for stroking an arc, including:
  // - It introduces segments to/from the center point (not too hard).
  // - It introduces joins on those segments (a bit more complicated).
  // - Even if the sweep is >=360 degrees, we still draw the segment to
  //   the center and it basically looks like a pie cut into the complete
  //   boundary circle, as if the slice were cut, but not extracted
  //   (hard to express as a continuous kTriangleStrip).
  if (!arc.IncludeCenter()) {
    if (arc.IsFullCircle()) {
      return DrawOval(oval_bounds, paint);
    }

    // Our fast stroking code only works for circular bounds as it assumes
    // that the inner and outer radii can be scaled along each angular step
    // of the arc - which is not true for elliptical arcs where the inner
    // and outer samples are perpendicular to the traveling direction of the
    // elliptical curve which may not line up with the center of the bounds.
    if (oval_bounds.IsSquare()) {
      ArcGeometry geom(arc, paint.stroke);
      AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
      return;
    }
  }

  ArcStrokeGeometry geom(arc, paint.stroke);
  AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
}

void Canvas::DrawRoundRect(const RoundRect& round_rect, const Paint& paint) {
  if (IsShadowBlurDrawOperation(paint)) {
    if (AttemptDrawBlurredRRect(round_rect, paint)) {
      return;
    }
  }

  if (round_rect.GetRadii().AreAllCornersSame() &&
      paint.style == Paint::Style::kFill) {
    Entity entity;
    entity.SetTransform(GetCurrentTransform());
    entity.SetBlendMode(paint.blend_mode);

    RoundRectGeometry geom(round_rect.GetBounds(),
                           round_rect.GetRadii().top_left);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
    return;
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  if (paint.style == Paint::Style::kFill) {
    FillRoundRectGeometry geom(round_rect);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  } else {
    StrokeRoundRectGeometry geom(round_rect, paint.stroke);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  }
}

void Canvas::DrawDiffRoundRect(const RoundRect& outer,
                               const RoundRect& inner,
                               const Paint& paint) {
  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  if (paint.style == Paint::Style::kFill) {
    FillDiffRoundRectGeometry geom(outer, inner);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  } else {
    StrokeDiffRoundRectGeometry geom(outer, inner, paint.stroke);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  }
}

void Canvas::DrawRoundSuperellipse(const RoundSuperellipse& round_superellipse,
                                   const Paint& paint) {
  if (IsShadowBlurDrawOperation(paint)) {
    if (AttemptDrawBlurredRSuperellipse(round_superellipse, paint)) {
      return;
    }
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  if (paint.style == Paint::Style::kFill) {
    RoundSuperellipseGeometry geom(round_superellipse.GetBounds(),
                                   round_superellipse.GetRadii());
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  } else {
    StrokeRoundSuperellipseGeometry geom(round_superellipse, paint.stroke);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  }
}

void Canvas::DrawCircle(const Point& center,
                        Scalar radius,
                        const Paint& paint) {
  if (IsShadowBlurDrawOperation(paint)) {
    Rect bounds = Rect::MakeLTRB(center.x - radius, center.y - radius,
                                 center.x + radius, center.y + radius);
    RRectBlurShape shape(bounds, radius);
    if (AttemptDrawBlur(shape, paint)) {
      return;
    }
  }

  if (renderer_.GetContext()->GetFlags().use_sdfs &&
      !paint.mask_blur_descriptor.has_value()) {
    const bool is_stroked = paint.style == Paint::Style::kStroke;

    std::optional<CircleGeometry> geometry;
    if (is_stroked) {
      geometry.emplace(center, radius, paint.stroke.width);
    } else {
      geometry.emplace(center, radius);
    }
    geometry->SetAntialiasPadding(1.0f);

    auto contents = UberSDFContents::MakeCircle(
        /*color=*/paint.color, /*stroked=*/is_stroked, &geometry.value());

    Entity entity;
    entity.SetTransform(GetCurrentTransform());
    entity.SetBlendMode(paint.blend_mode);

    const Geometry* geom = contents->GetGeometry();

    AddRenderSDFEntityToCurrentPass(entity, geom, paint, std::move(contents));
    return;
  }

  if (AttemptDrawAntialiasedCircle(center, radius, paint)) {
    return;
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  if (paint.style == Paint::Style::kStroke) {
    CircleGeometry geom(center, radius, paint.stroke.width);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  } else {
    CircleGeometry geom(center, radius);
    AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
  }
}

void Canvas::ClipGeometry(
    const Geometry& geometry,
    Entity::ClipOperation clip_op,
    bool is_aa,
    std::optional<BackdropSurfaceContents::AnalyticRRect> analytic_round_rect) {
  if (IsSkipping()) {
    return;
  }

  // Ideally the clip depth would be greater than the current rendering
  // depth because any rendering calls that follow this clip operation will
  // pre-increment the depth and then be rendering above our clip depth,
  // but that case will be caught by the CHECK in AddRenderEntity above.
  // In practice we sometimes have a clip set with no rendering after it
  // and in such cases the current depth will equal the clip depth.
  // Eventually the DisplayList should optimize these out, but it is hard
  // to know if a clip will actually be used in advance of storing it in
  // the DisplayList buffer.
  // See https://github.com/flutter/flutter/issues/147021
  FML_DCHECK(current_depth_ <= transform_stack_.back().clip_depth)
      << current_depth_ << " <=? " << transform_stack_.back().clip_depth;
  uint32_t clip_depth = transform_stack_.back().clip_depth;

  const Matrix clip_transform =
      Matrix::MakeTranslation(Vector3(-GetGlobalPassPosition())) *
      GetCurrentTransform();

  std::optional<Rect> clip_coverage = geometry.GetCoverage(clip_transform);
  if (!clip_coverage.has_value()) {
    return;
  }

  ClipContents clip_contents(
      clip_coverage.value(),
      /*is_axis_aligned_rect=*/geometry.IsAxisAlignedRect() &&
          GetCurrentTransform().IsTranslationScaleOnly());
  clip_contents.SetClipOperation(clip_op);

  EntityPassClipStack::ClipStateResult clip_state_result =
      clip_coverage_stack_.RecordClip(
          clip_contents,                                     //
          /*transform=*/clip_transform,                      //
          /*global_pass_position=*/GetGlobalPassPosition(),  //
          /*clip_depth=*/clip_depth,                         //
          /*clip_height_floor=*/GetClipHeightFloor(),        //
          /*is_aa=*/is_aa);

  std::optional<IRect32> clip_scissor;
  if (clip_state_result.clip_did_change) {
    // We only need to update the pass scissor if the clip state has changed.
    clip_scissor = SetClipScissor(
        clip_coverage_stack_.CurrentClipCoverage(),
        *render_passes_.back().GetInlinePassContext()->GetRenderPass(),
        GetGlobalPassPosition());
  }

  ++transform_stack_.back().clip_height;
  ++transform_stack_.back().num_clips;

  if (!clip_state_result.should_render) {
    return;
  }

  const bool can_defer_round_rect =
      analytic_round_rect.has_value() &&
      clip_op == Entity::ClipOperation::kIntersect && is_aa &&
      render_passes_.size() == 1u && clip_transform.IsTranslationScaleOnly() &&
      FindPendingBackdropComposite() != nullptr &&
      FindDeferredRRectClip() == nullptr &&
      renderer_.GetContext()->GetBackendType() ==
          Context::BackendType::kOpenGLES;

  // Scissor-only clips remain logical and can participate in fusion. A clip
  // that emits GPU work must follow the pending backdrop, so materialize the
  // exact fallback state before constructing this command.
  if (!can_defer_round_rect) {
    if (FindPendingBackdropComposite() != nullptr) {
      FlushPendingBackdropComposite();
    } else {
      FlushDeferredRRectClip();
    }
  }

  // Note: this is a bit of a hack. Its not possible to construct a geometry
  // result without begninning the render pass. We should refactor the geometry
  // objects so that they only need a reference to the render pass size and/or
  // orthographic transform.
  Entity entity;
  entity.SetTransform(clip_transform);
  entity.SetClipDepth(clip_depth);

  GeometryResult geometry_result = geometry.GetPositionBuffer(
      renderer_,                                                      //
      entity,                                                         //
      *render_passes_.back().GetInlinePassContext()->GetRenderPass()  //
  );
  clip_contents.SetGeometry(geometry_result);
  clip_coverage_stack_.GetLastReplayResult().clip_contents.SetGeometry(
      geometry_result);

  if (can_defer_round_rect) {
    const Matrix basis = clip_transform.Basis();
    const Size radii = analytic_round_rect->radii;
    const Size transformed_radii(
        (basis * Vector2(radii.width, 0.0f)).GetLength(),
        (basis * Vector2(0.0f, radii.height)).GetLength());
    transform_stack_.back().deferred_rrect_clip =
        std::make_shared<DeferredRRectClip>(DeferredRRectClip{
            .contents = std::move(clip_contents),
            .clip_depth = clip_depth,
            .cover_scissor = clip_scissor,
            .analytic_clip = {.bounds =
                                  analytic_round_rect->bounds.TransformBounds(
                                      clip_transform),
                              .radii = transformed_radii},
        });
    RecordBackdropFusionEvent(BackdropFusionAuditEvent::kDeferredRRect);
    return;
  }

  clip_contents.Render(
      renderer_, *render_passes_.back().GetInlinePassContext()->GetRenderPass(),
      clip_depth, /*is_backdrop_replay=*/false, clip_scissor);
}

void Canvas::DrawPoints(const Point points[],
                        uint32_t count,
                        Scalar radius,
                        const Paint& paint,
                        PointStyle point_style) {
  if (radius <= 0) {
    return;
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  PointFieldGeometry geom(points, count, radius,
                          /*round=*/point_style == PointStyle::kRound);
  AddRenderEntityWithFiltersToCurrentPass(entity, &geom, paint);
}

void Canvas::DrawImage(const std::shared_ptr<Texture>& image,
                       Point offset,
                       const Paint& paint,
                       const SamplerDescriptor& sampler) {
  if (!image) {
    return;
  }

  const Rect source = Rect::MakeSize(image->GetSize());
  const Rect dest = source.Shift(offset);

  DrawImageRect(image, source, dest, paint, sampler);
}

void Canvas::DrawImageRect(const std::shared_ptr<Texture>& image,
                           Rect source,
                           Rect dest,
                           const Paint& paint,
                           const SamplerDescriptor& sampler,
                           SourceRectConstraint src_rect_constraint,
                           bool is_external_texture) {
  if (!image || source.IsEmpty() || dest.IsEmpty()) {
    return;
  }

  ISize size = image->GetSize();
  if (size.IsEmpty()) {
    return;
  }

  std::optional<Rect> clipped_source =
      source.Intersection(Rect::MakeSize(size));
  if (!clipped_source) {
    return;
  }

  if (AttemptColorFilterOptimization(image, source, dest, paint, sampler,
                                     src_rect_constraint)) {
    return;
  }

  if (*clipped_source != source) {
    Scalar sx = dest.GetWidth() / source.GetWidth();
    Scalar sy = dest.GetHeight() / source.GetHeight();
    Scalar tx = dest.GetLeft() - source.GetLeft() * sx;
    Scalar ty = dest.GetTop() - source.GetTop() * sy;
    Matrix src_to_dest = Matrix::MakeTranslateScale({sx, sy, 1}, {tx, ty, 0});
    dest = clipped_source->TransformBounds(src_to_dest);
  }

  auto texture_contents = TextureContents::MakeRect(dest);
  texture_contents->SetTexture(image);
  texture_contents->SetIsExternalTexture(is_external_texture);
  texture_contents->SetSourceRect(*clipped_source);
  texture_contents->SetStrictSourceRect(src_rect_constraint ==
                                        SourceRectConstraint::kStrict);
  texture_contents->SetSamplerDescriptor(sampler);
  texture_contents->SetOpacity(paint.color.alpha);
  texture_contents->SetDeferApplyingOpacity(paint.HasColorFilter());

  Entity entity;
  entity.SetBlendMode(paint.blend_mode);
  entity.SetTransform(GetCurrentTransform());

  if (!paint.mask_blur_descriptor.has_value()) {
    std::shared_ptr<TextureContents> texture_candidate = texture_contents;
    std::shared_ptr<Contents> filtered_contents =
        paint.WithFilters(std::move(texture_contents));
    const bool is_unwrapped_texture =
        filtered_contents.get() == texture_candidate.get();
    entity.SetContents(std::move(filtered_contents));
    AddRenderEntityToCurrentPass(
        entity, /*reuse_depth=*/false,
        is_unwrapped_texture ? std::move(texture_candidate) : nullptr);
    return;
  }

  FillRectGeometry out_rect(Rect{});

  entity.SetContents(paint.WithFilters(
      paint.mask_blur_descriptor->CreateMaskBlur(texture_contents, &out_rect)));
  AddRenderEntityToCurrentPass(entity);
}

size_t Canvas::GetClipHeight() const {
  return transform_stack_.back().clip_height;
}

void Canvas::DrawVertices(const std::shared_ptr<VerticesGeometry>& vertices,
                          BlendMode blend_mode,
                          const Paint& paint) {
  // Override the blend mode with kDestination in order to match the behavior
  // of Skia's SK_LEGACY_IGNORE_DRAW_VERTICES_BLEND_WITH_NO_SHADER flag, which
  // is enabled when the Flutter engine builds Skia.
  if (!paint.color_source) {
    blend_mode = BlendMode::kDst;
  }

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);

  // If there are no vertex colors.
  if (UseColorSourceContents(vertices, paint)) {
    AddRenderEntityWithFiltersToCurrentPass(entity, vertices.get(), paint);
    return;
  }

  // If the blend mode is destination don't bother to bind or create a texture.
  if (blend_mode == BlendMode::kDst) {
    auto contents = std::make_shared<VerticesSimpleBlendContents>();
    contents->SetBlendMode(blend_mode);
    contents->SetAlpha(paint.color.alpha);
    contents->SetGeometry(vertices);
    entity.SetContents(paint.WithFilters(std::move(contents)));
    AddRenderEntityToCurrentPass(entity);
    return;
  }

  // If there is a texture, use this directly. Otherwise render the color
  // source to a texture.
  if (paint.color_source &&
      paint.color_source->type() == flutter::DlColorSourceType::kImage) {
    const flutter::DlImageColorSource* image_color_source =
        paint.color_source->asImage();
    FML_DCHECK(image_color_source &&
               image_color_source->image()->impeller_texture());
    auto texture = image_color_source->image()->impeller_texture();
    auto x_tile_mode = static_cast<Entity::TileMode>(
        image_color_source->horizontal_tile_mode());
    auto y_tile_mode =
        static_cast<Entity::TileMode>(image_color_source->vertical_tile_mode());
    auto sampler_descriptor =
        skia_conversions::ToSamplerDescriptor(image_color_source->sampling());
    auto effect_transform = image_color_source->matrix();

    auto contents = std::make_shared<VerticesSimpleBlendContents>();
    contents->SetBlendMode(blend_mode);
    contents->SetAlpha(paint.color.alpha);
    contents->SetGeometry(vertices);
    contents->SetEffectTransform(effect_transform);
    contents->SetTexture(texture);
    contents->SetTileMode(x_tile_mode, y_tile_mode);
    contents->SetSamplerDescriptor(sampler_descriptor);

    entity.SetContents(paint.WithFilters(std::move(contents)));
    AddRenderEntityToCurrentPass(entity);
    return;
  }

  auto src_paint = paint;
  src_paint.color = paint.color.WithAlpha(1.0);

  std::shared_ptr<ColorSourceContents> src_contents =
      src_paint.CreateContents(vertices.get());

  // If the color source has an intrinsic size, then we use that to
  // create the src contents as a simplification. Otherwise we use
  // the extent of the texture coordinates to determine how large
  // the src contents should be. If neither has a value we fall back
  // to using the geometry coverage data.
  Rect src_coverage;
  auto size = src_contents->GetColorSourceSize();
  if (size.has_value()) {
    src_coverage = Rect::MakeXYWH(0, 0, size->width, size->height);
  } else {
    auto cvg = vertices->GetCoverage(Matrix{});
    FML_CHECK(cvg.has_value());
    auto texture_coverage = vertices->GetTextureCoordinateCoverage();
    if (texture_coverage.has_value()) {
      src_coverage =
          Rect::MakeOriginSize(texture_coverage->GetOrigin(),
                               texture_coverage->GetSize().Max({1, 1}));
    } else {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      src_coverage = cvg.value();
    }
  }
  clip_geometry_.push_back(Geometry::MakeRect(Rect::Round(src_coverage)));
  src_contents = src_paint.CreateContents(clip_geometry_.back().get());

  auto contents = std::make_shared<VerticesSimpleBlendContents>();
  contents->SetBlendMode(blend_mode);
  contents->SetAlpha(paint.color.alpha);
  contents->SetGeometry(vertices);
  contents->SetLazyTextureCoverage(src_coverage);
  contents->SetLazyTexture(
      [src_contents, src_coverage](
          const ContentContext& renderer) -> std::shared_ptr<Texture> {
        // Applying the src coverage as the coverage limit prevents the 1px
        // coverage pad from adding a border that is picked up by developer
        // specified UVs.
        auto snapshot = src_contents->RenderToSnapshot(
            renderer, {}, {.coverage_limit = Rect::Round(src_coverage)});
        return snapshot.has_value() ? snapshot->texture : nullptr;
      });
  entity.SetContents(paint.WithFilters(std::move(contents)));
  AddRenderEntityToCurrentPass(entity);
}

void Canvas::DrawAtlas(const std::shared_ptr<AtlasContents>& atlas_contents,
                       const Paint& paint) {
  atlas_contents->SetAlpha(paint.color.alpha);

  Entity entity;
  entity.SetTransform(GetCurrentTransform());
  entity.SetBlendMode(paint.blend_mode);
  entity.SetContents(paint.WithFilters(atlas_contents));

  AddRenderEntityToCurrentPass(entity);
}

/// Compositor Functionality
/////////////////////////////////////////

void Canvas::SetupRenderPass() {
  renderer_.GetRenderTargetCache()->Start();
  ColorAttachment color0 = render_target_.GetColorAttachment(0);

  // Set up the clear color of the root pass.
  color0.clear_color = Color::BlackTransparent();
  render_target_.SetColorAttachment(color0, 0);

  const TextureDescriptor& root_texture =
      color0.texture->GetTextureDescriptor();
  const bool preserve_depth_stencil_between_passes = requires_readback_;
  if (requires_readback_ && (root_texture.usage & TextureUsage::kShaderRead) &&
      root_texture.sample_count == SampleCount::kCount1) {
    requires_readback_ = false;
  }

  const bool root_preserves_depth_stencil =
      preserve_depth_stencil_between_passes && !requires_readback_;
  auto& stencil_attachment = render_target_.GetStencilAttachment();
  auto& depth_attachment = render_target_.GetDepthAttachment();
  if (root_preserves_depth_stencil || !stencil_attachment.has_value() ||
      !depth_attachment.has_value()) {
    // Inter-pass readback needs persistent attachments. All other root targets
    // retain the cheaper transient depth/stencil allocation.
    render_target_.SetupDepthStencilAttachments(
        *renderer_.GetContext(),
        *renderer_.GetContext()->GetResourceAllocator(),
        color0.texture->GetSize(),
        renderer_.GetContext()->GetCapabilities()->SupportsOffscreenMSAA() &&
            color0.texture->GetTextureDescriptor().sample_count >
                SampleCount::kCount1,
        "ImpellerOnscreen",
        root_preserves_depth_stencil ? kPersistentStencilConfig
                                     : kDefaultStencilConfig);
  }

  // If requires_readback is true, then there is a backdrop filter or emulated
  // advanced blend in the first save layer. This requires a readback, which
  // isn't supported by onscreen textures. To support this, we immediately begin
  // a second save layer with the same dimensions as the onscreen. When
  // rendering is completed, we must blit this saveLayer to the onscreen.
  if (requires_readback_) {
    auto entity_pass_target =
        CreateRenderTarget(renderer_,                  //
                           color0.texture->GetSize(),  //
                           /*clear_color=*/Color::BlackTransparent(),
                           /*use_msaa=*/true,
                           /*preserve_depth_stencil_between_passes=*/true);
    render_passes_.push_back(
        LazyRenderingConfig(renderer_, std::move(entity_pass_target),
                            preserve_depth_stencil_between_passes));
  } else {
    auto entity_pass_target = std::make_unique<EntityPassTarget>(
        render_target_,                                                    //
        renderer_.GetDeviceCapabilities().SupportsReadFromResolve(),       //
        renderer_.GetDeviceCapabilities().SupportsImplicitResolvingMSAA()  //
    );
    render_passes_.push_back(
        LazyRenderingConfig(renderer_, std::move(entity_pass_target),
                            preserve_depth_stencil_between_passes));
  }
}

void Canvas::SkipUntilMatchingRestore(size_t total_content_depth) {
  auto entry = CanvasStackEntry{};
  entry.skipping = true;
  entry.clip_depth = current_depth_ + total_content_depth;
  transform_stack_.push_back(entry);
}

void Canvas::Save(uint32_t total_content_depth) {
  if (IsSkipping()) {
    return SkipUntilMatchingRestore(total_content_depth);
  }

  auto entry = CanvasStackEntry{};
  entry.transform = transform_stack_.back().transform;
  entry.clip_depth = current_depth_ + total_content_depth;
  entry.distributed_opacity = transform_stack_.back().distributed_opacity;
  FML_DCHECK(entry.clip_depth <= transform_stack_.back().clip_depth)
      << entry.clip_depth << " <=? " << transform_stack_.back().clip_depth
      << " after allocating " << total_content_depth;
  entry.clip_height = transform_stack_.back().clip_height;
  entry.rendering_mode = Entity::RenderingMode::kDirect;
  transform_stack_.push_back(entry);
}

std::optional<Rect> Canvas::GetLocalCoverageLimit() const {
  if (!clip_coverage_stack_.HasCoverage()) {
    // The current clip is empty. This means the pass texture won't be
    // visible, so skip it.
    return std::nullopt;
  }

  std::optional<Rect> maybe_current_clip_coverage =
      clip_coverage_stack_.CurrentClipCoverage();
  if (!maybe_current_clip_coverage.has_value()) {
    return std::nullopt;
  }

  Rect current_clip_coverage = maybe_current_clip_coverage.value();

  FML_CHECK(!render_passes_.empty());
  const LazyRenderingConfig& back_render_pass = render_passes_.back();
  std::shared_ptr<Texture> back_texture =
      back_render_pass.GetInlinePassContext()->GetTexture();
  FML_CHECK(back_texture) << "Context is valid:"
                          << back_render_pass.GetInlinePassContext()->IsValid();

  // The maximum coverage of the subpass. Subpasses textures should never
  // extend outside the parent pass texture or the current clip coverage.
  std::optional<Rect> maybe_coverage_limit =
      Rect::MakeOriginSize(GetGlobalPassPosition(),
                           Size(back_texture->GetSize()))
          .Intersection(current_clip_coverage);

  if (!maybe_coverage_limit.has_value() || maybe_coverage_limit->IsEmpty()) {
    return std::nullopt;
  }

  return maybe_coverage_limit->Intersection(
      Rect::MakeSize(render_target_.GetRenderTargetSize()));
}

void Canvas::SaveLayer(const Paint& paint,
                       std::optional<Rect> bounds,
                       const flutter::DlImageFilter* backdrop_filter,
                       ContentBoundsPromise bounds_promise,
                       uint32_t total_content_depth,
                       bool can_distribute_opacity,
                       std::optional<int64_t> backdrop_id,
                       bool content_is_single_sample_compatible) {
  TRACE_EVENT0("flutter", "Canvas::saveLayer");
  if (IsSkipping()) {
    return SkipUntilMatchingRestore(total_content_depth);
  }
  if (FindPendingBackdropComposite() != nullptr) {
    FlushPendingBackdropComposite();
  } else {
    FlushDeferredRRectClip();
  }

  auto maybe_coverage_limit = GetLocalCoverageLimit();
  if (!maybe_coverage_limit.has_value()) {
    return SkipUntilMatchingRestore(total_content_depth);
  }
  auto coverage_limit = maybe_coverage_limit.value();

  std::optional<Scalar> backdrop_alpha_threshold;
  bool backdrop_alpha_threshold_is_single_surface = false;
  if (backdrop_filter &&
      (backdrop_filter->type() == flutter::DlImageFilterType::kBlur ||
       backdrop_filter->type() == flutter::DlImageFilterType::kGlass)) {
    const Scalar threshold =
        backdrop_filter->type() == flutter::DlImageFilterType::kBlur
            ? backdrop_filter->asBlur()->backdrop_alpha_threshold()
            : backdrop_filter->asGlass()->backdrop_alpha_threshold();
    if (threshold >= 0.0f) {
      backdrop_alpha_threshold = std::clamp(threshold, 0.0f, 1.0f);
      backdrop_alpha_threshold_is_single_surface =
          backdrop_filter->type() == flutter::DlImageFilterType::kBlur
              ? backdrop_filter->asBlur()
                    ->backdrop_alpha_threshold_is_single_surface()
              : backdrop_filter->asGlass()
                    ->backdrop_alpha_threshold_is_single_surface();
    }
  }

  if (can_distribute_opacity && !backdrop_filter &&
      Paint::CanApplyOpacityPeephole(paint) &&
      bounds_promise != ContentBoundsPromise::kMayClipContents) {
    Save(total_content_depth);
    transform_stack_.back().distributed_opacity *= paint.color.alpha;
    return;
  }

  std::shared_ptr<FilterContents> filter_contents = paint.WithImageFilter(
      Rect(), transform_stack_.back().transform,
      Entity::RenderingMode::kSubpassPrependSnapshotTransform);

  std::optional<Rect> maybe_subpass_coverage = ComputeSaveLayerCoverage(
      bounds.value_or(Rect::MakeMaximum()),
      transform_stack_.back().transform,  //
      coverage_limit,                     //
      filter_contents,                    //
      /*flood_output_coverage=*/
      Entity::IsBlendModeDestructive(paint.blend_mode),  //
      /*flood_input_coverage=*/!!backdrop_filter ||
          (paint.color_filter &&
           paint.color_filter->modifies_transparent_black())  //
  );

  if (!maybe_subpass_coverage.has_value()) {
    return SkipUntilMatchingRestore(total_content_depth);
  }

  auto subpass_coverage = maybe_subpass_coverage.value();

  // When an image filter is present, clamp to avoid flicking due to nearest
  // sampled image. For other cases, round out to ensure than any geometry is
  // not cut off.
  //
  // See also this bug: https://github.com/flutter/flutter/issues/144213
  //
  // TODO(jonahwilliams): this could still round out for filters that use decal
  // sampling mode.
  ISize subpass_size;
  bool did_round_out = false;
  Point coverage_origin_adjustment = Point{0, 0};
  if (paint.image_filter) {
    subpass_size = ISize(subpass_coverage.GetSize());
  } else {
    did_round_out = true;
    subpass_size =
        static_cast<ISize>(IRect::RoundOut(subpass_coverage).GetSize());
    // If rounding out, adjust the coverage to account for the subpixel shift.
    coverage_origin_adjustment =
        Point(subpass_coverage.GetLeftTop().x -
                  std::floor(subpass_coverage.GetLeftTop().x),
              subpass_coverage.GetLeftTop().y -
                  std::floor(subpass_coverage.GetLeftTop().y));
  }
  if (subpass_size.IsEmpty()) {
    return SkipUntilMatchingRestore(total_content_depth);
  }

  // When there are scaling filters present, these contents may exceed the
  // maximum texture size. Perform a clamp here, which may cause rendering
  // artifacts.
  subpass_size = subpass_size.Min(renderer_.GetContext()
                                      ->GetCapabilities()
                                      ->GetMaximumRenderPassAttachmentSize());

  // Flutter's retained-layer diff supplies a versioned cache token for every
  // backdrop filter. Sharing is a separate semantic fact: only multiple uses
  // of the same token in this DisplayList must observe one common input.
  bool will_cache_backdrop_texture = false;
  BackdropData* backdrop_data = nullptr;
  size_t backdrop_count = 1u;
  if (backdrop_filter && backdrop_id.has_value()) {
    auto backdrop_data_it = backdrop_data_.find(backdrop_id.value());
    if (backdrop_data_it != backdrop_data_.end()) {
      backdrop_data = &backdrop_data_it->second;
      will_cache_backdrop_texture = backdrop_data->backdrop_count > 1u;
      backdrop_count = backdrop_data->backdrop_count;
    }
  }

  // A backdrop layer restored with kSrc is an assignment, not an opacity
  // group. When its child program contains no geometric draws, the exact same
  // expression can be evaluated in the parent target:
  //
  //   layer = backdrop; layer = children over layer; parent = layer (kSrc)
  //
  // becomes:
  //
  //   parent = backdrop (kSrc); parent = children over parent
  //
  // This is the first physical plan which treats saveLayer as a compositing
  // relationship instead of an allocation request. The DisplayList proof is
  // conservative: geometric content, nested layers, vertices, atlases,
  // shadows, and nested DisplayLists all reject the plan. Paint attributes on
  // the restore and semantically grouped backdrops retain the materialized
  // path. A single-use cache identity does not change the expression.
  const bool restore_has_effects =
      paint.color_source != nullptr || paint.color_filter != nullptr ||
      paint.image_filter != nullptr || paint.invert_colors ||
      paint.mask_blur_descriptor.has_value();
  const BackdropLayerDirectPlanInputs direct_plan_inputs = {
      .has_backdrop_filter = backdrop_filter != nullptr,
      .content_is_single_sample_compatible =
          content_is_single_sample_compatible,
      .content_bounds_are_contained =
          bounds_promise == ContentBoundsPromise::kContainsContents,
      .is_root_pass = render_passes_.size() == 1u,
      .restore_blend_mode = paint.blend_mode,
      .restore_is_opaque = paint.color.IsOpaque(),
      .restore_has_effects = restore_has_effects,
      .shares_backdrop_input = will_cache_backdrop_texture,
      .inherited_opacity = transform_stack_.back().distributed_opacity,
  };
  const uint32_t direct_plan_rejections =
      GetBackdropLayerDirectRejections(direct_plan_inputs);
  const bool can_render_backdrop_directly =
      direct_plan_rejections == 0u &&
      (!backdrop_alpha_threshold.has_value() ||
       backdrop_alpha_threshold_is_single_surface);
  if (backdrop_filter) {
    RecordBackdropDirectPredicate(direct_plan_rejections);
  }

  std::optional<uint32_t> planned_backdrop_epoch;
  if (backdrop_filter && render_passes_.size() == 1u) {
    planned_backdrop_epoch =
        ClaimBackdropEpoch(subpass_coverage, *backdrop_filter);
  }
  if (!can_render_backdrop_directly || !planned_backdrop_epoch.has_value()) {
    active_backdrop_epoch_.reset();
    active_backdrop_epoch_texture_.reset();
    if (can_render_backdrop_directly) {
      RecordBackdropEpochExecution(BackdropEpochExecution::kPlanMiss);
    }
  }

  // Backdrop filter state, ignored if there is no BDF.
  std::shared_ptr<FilterContents> backdrop_filter_contents;
  std::optional<Snapshot> isolated_backdrop_snapshot;
  std::optional<Snapshot> direct_scene_snapshot;
  bool isolated_backdrop_cache_hit = false;
  bool should_materialize_isolated_snapshot = false;
  Point local_position = Point(0, 0);
  auto make_backdrop_snapshot_entity =
      [&](const Snapshot& snapshot, bool is_direct_cache_composite = false) {
        // Snapshot::transform maps texture pixels into scene coordinates. Crop
        // both sides of that mapping to the demanded saveLayer coverage instead
        // of throwing the transform away and treating the texture as a screen
        // copy. This makes the draw exactly equivalent to placing the snapshot
        // in a coverage-sized subpass and restoring that subpass into the
        // parent.
        std::shared_ptr<TextureContents> contents = TextureContents::MakeRect(
            subpass_coverage.Shift(-GetGlobalPassPosition()));
        auto scaled =
            subpass_coverage.TransformBounds(snapshot.transform.Invert());
        contents->SetTexture(snapshot.texture);
        contents->SetSourceRect(scaled);
        contents->SetSamplerDescriptor(snapshot.sampler_descriptor);
        if (is_direct_cache_composite) {
          contents->SetLabel("Denial backdrop cached composite");
        }

        Entity backdrop_entity;
        backdrop_entity.SetContents(std::move(contents));
        backdrop_entity.SetBlendMode(paint.blend_mode);
        return backdrop_entity;
      };
  auto render_backdrop_snapshot = [&](const Snapshot& snapshot,
                                      bool is_direct_cache_composite = false) {
    Entity backdrop_entity =
        make_backdrop_snapshot_entity(snapshot, is_direct_cache_composite);
    backdrop_entity.SetClipDepth(++current_depth_);
    FlushDeferredRRectClip();
    backdrop_entity.Render(renderer_, GetCurrentRenderPass());
  };
  if (backdrop_filter) {
    local_position = subpass_coverage.GetOrigin() - GetGlobalPassPosition();
    const Matrix material_transform = GetCurrentTransform();
    Canvas::BackdropFilterProc backdrop_filter_proc =
        [backdrop_filter, material_transform](
            const FilterInput::Ref& input, const Matrix& effect_transform,
            Entity::RenderingMode rendering_mode) {
          auto filter = WrapInput(backdrop_filter, input);
          if (backdrop_filter->asGlass()) {
            std::static_pointer_cast<GlassFilterContents>(filter)
                ->SetMaterialTransform(material_transform);
          }
          filter->SetEffectTransform(effect_transform);
          filter->SetRenderingMode(rendering_mode);
          filter->SetIsBackdropFilter(true);
          return filter;
        };

    std::shared_ptr<Texture> input_texture;

    // If the backdrop ID is not nullopt and there is more than one usage
    // of it in the current scene, cache the backdrop texture and remove it from
    // the current entity pass flip.
    auto consume_backdrop_count = [&]() {
      if (!backdrop_data || !backdrop_data->backdrop_count_consumed) {
        backdrop_count_ -= backdrop_count;
        if (backdrop_data) {
          backdrop_data->backdrop_count_consumed = true;
        }
      }
    };

    const bool can_cache_across_frames = backdrop_id.has_value() &&
                                         backdrop_data &&
                                         backdrop_data->all_filters_equal;
    if (can_cache_across_frames) {
      auto cached = renderer_.GetCachedBackdropSnapshot(backdrop_id.value());
      const auto cached_coverage =
          cached.has_value() ? cached->GetCoverage() : std::nullopt;
      if (cached_coverage.has_value() &&
          cached_coverage->Contains(subpass_coverage)) {
        renderer_.RecordBackdropSnapshotReuse(backdrop_id.value());
        consume_backdrop_count();
        if (will_cache_backdrop_texture) {
          backdrop_data->shared_filter_snapshot = cached;
          render_backdrop_snapshot(cached.value());
          Save(0);
          return;
        }
        isolated_backdrop_snapshot = std::move(cached);
        isolated_backdrop_cache_hit = true;
      }
    }

    if (!isolated_backdrop_snapshot.has_value() &&
        !will_cache_backdrop_texture && can_cache_across_frames) {
      // MSAA child content needs a color layer, but does not make a changing
      // backdrop worth retaining. Persistent snapshots disable the temporary
      // render-target pool while filtering. Eagerly materializing every new
      // generation therefore reallocates the entire filter chain during a
      // drag. Use the same reuse observation as the direct path; until the
      // input stabilizes, the ordinary subpass can use pooled filter targets.
      should_materialize_isolated_snapshot =
          renderer_.ShouldMaterializeBackdropSnapshot(backdrop_id.value());
    }

    if (!isolated_backdrop_snapshot.has_value() &&
        (!will_cache_backdrop_texture || !backdrop_data->texture_slot)) {
      consume_backdrop_count();

      const bool can_reuse_epoch_snapshot =
          can_render_backdrop_directly && planned_backdrop_epoch.has_value() &&
          active_backdrop_epoch_ == planned_backdrop_epoch &&
          active_backdrop_epoch_texture_;
      if (can_reuse_epoch_snapshot) {
        input_texture = active_backdrop_epoch_texture_;
        RecordBackdropEpochExecution(BackdropEpochExecution::kSnapshotReuse);
      } else {
        // The onscreen texture can be flipped to if:
        // 1. The device supports framebuffer fetch
        // 2. There are no more backdrop filters
        // 3. The current render pass is for the onscreen pass.
        const bool should_use_onscreen =
            renderer_.GetDeviceCapabilities().SupportsFramebufferFetch() &&
            backdrop_count_ == 0 && render_passes_.size() == 1u;
        input_texture = FlipBackdrop(
            GetGlobalPassPosition(),                                //
            /*should_remove_texture=*/will_cache_backdrop_texture,  //
            /*should_use_onscreen=*/should_use_onscreen             //
        );
        if (input_texture && can_render_backdrop_directly &&
            planned_backdrop_epoch.has_value()) {
          active_backdrop_epoch_ = planned_backdrop_epoch;
          active_backdrop_epoch_texture_ = input_texture;
          RecordBackdropEpochExecution(BackdropEpochExecution::kSnapshotFlip);
        }
      }
      if (!input_texture) {
        // Validation failures are logged in FlipBackdrop.
        return;
      }

      if (can_render_backdrop_directly) {
        direct_scene_snapshot = Snapshot{
            .texture = input_texture,
            .transform = Matrix(),
        };
      }

      if (will_cache_backdrop_texture) {
        backdrop_data->texture_slot = input_texture;
      }
    } else if (!isolated_backdrop_snapshot.has_value()) {
      input_texture = backdrop_data->texture_slot;
    }

    if (!isolated_backdrop_snapshot.has_value()) {
      const Entity::RenderingMode backdrop_rendering_mode =
          can_render_backdrop_directly && !should_materialize_isolated_snapshot
              ? Entity::RenderingMode::kDirect
          : transform_stack_.back().transform.HasTranslation()
              ? Entity::RenderingMode::kSubpassPrependSnapshotTransform
              : Entity::RenderingMode::kSubpassAppendSnapshotTransform;
      backdrop_filter_contents = backdrop_filter_proc(
          FilterInput::Make(std::move(input_texture)),
          transform_stack_.back().transform.Basis(), backdrop_rendering_mode);

      auto render_persistent_snapshot = [&]() {
        // A cross-frame snapshot must own its texture exclusively. Otherwise
        // Impeller's per-frame render-target pool may recycle and overwrite it.
        renderer_.GetRenderTargetCache()->DisableCache();
        fml::ScopedCleanupClosure restore_render_target_cache(
            [&] { renderer_.GetRenderTargetCache()->EnableCache(); });
        // An ungrouped filter only needs the pixels covered by its saveLayer.
        // Supplying that output limit also lets filters crop their input work.
        // Grouped filters still need one snapshot that can serve every member.
        const std::optional<Rect> coverage_limit =
            will_cache_backdrop_texture ? std::nullopt
                                        : std::make_optional(subpass_coverage);
        return backdrop_filter_contents->RenderToSnapshot(
            renderer_, {}, {.coverage_limit = coverage_limit});
      };

      std::optional<Snapshot> maybe_snapshot;
      if (will_cache_backdrop_texture) {
        FML_DCHECK(backdrop_data);
        if (backdrop_data->all_filters_equal &&
            !backdrop_data->shared_filter_snapshot.has_value()) {
          // TODO(157110): compute minimum input hint.
          backdrop_data->shared_filter_snapshot = render_persistent_snapshot();
        }
        maybe_snapshot = backdrop_data->shared_filter_snapshot;
      } else if (should_materialize_isolated_snapshot) {
        maybe_snapshot = render_persistent_snapshot();
      }

      if (maybe_snapshot.has_value()) {
        if (can_cache_across_frames) {
          renderer_.CacheBackdropSnapshot(backdrop_id.value(),
                                          maybe_snapshot.value());
        }
        if (will_cache_backdrop_texture) {
          render_backdrop_snapshot(maybe_snapshot.value());
          return;
        }
        isolated_backdrop_snapshot = std::move(maybe_snapshot);
      }
    }
  }

  // When applying a save layer, absorb any pending distributed opacity.
  Paint paint_copy = paint;
  paint_copy.color.alpha *= transform_stack_.back().distributed_opacity;
  transform_stack_.back().distributed_opacity = 1.0;

  if (can_render_backdrop_directly &&
      (backdrop_filter_contents || isolated_backdrop_snapshot.has_value())) {
    // The physical subpass used to provide this crop implicitly. A logical
    // scope must carry the demanded output region explicitly when its filter
    // is evaluated directly in the parent target.
    Entity backdrop_entity;
    std::optional<Entity> resolved_backdrop_entity;
    std::shared_ptr<TextureContents> resolved_backdrop_contents;
    if (isolated_backdrop_snapshot.has_value()) {
      resolved_backdrop_entity =
          make_backdrop_snapshot_entity(isolated_backdrop_snapshot.value(),
                                        /*is_direct_cache_composite=*/true);
      resolved_backdrop_contents = std::static_pointer_cast<TextureContents>(
          resolved_backdrop_entity->GetContents());
    } else {
      backdrop_filter_contents->SetCoverageHint(subpass_coverage);
      backdrop_entity.SetContents(backdrop_filter_contents);
      backdrop_entity.SetBlendMode(BlendMode::kSrc);
      backdrop_entity.SetTransform(
          Matrix::MakeTranslation(Vector3(-GetGlobalPassPosition())));
    }

    // Reserve the depth slot consumed by the logical backdrop assignment even
    // when its final texture draw is deferred and fused with the first proven
    // external surface. Filter evaluation is resolved now: Gaussian blur
    // already returns a texture-backed entity, so this does not allocate or
    // draw another intermediate.
    const uint32_t backdrop_depth = ++current_depth_;
    if (!resolved_backdrop_entity.has_value()) {
      backdrop_entity.SetClipDepth(backdrop_depth);
      const bool direct_glass_material =
          IsDirectGlassMaterialRequested() &&
          backdrop_filter->type() == flutter::DlImageFilterType::kGlass &&
          !backdrop_alpha_threshold.has_value() &&
          renderer_.GetContext()->GetBackendType() ==
              Context::BackendType::kOpenGLES;
      if (direct_glass_material) {
        // WrapInput maps a DlGlassImageFilter to GlassFilterContents. Only
        // this non-threshold, uncached direct path may return a shader entity;
        // texture fusion and persistent snapshots keep their existing type.
        resolved_backdrop_entity =
            std::static_pointer_cast<GlassFilterContents>(
                backdrop_filter_contents)
                ->GetDirectEntity(renderer_, backdrop_entity, subpass_coverage);
      } else {
        resolved_backdrop_entity = backdrop_filter_contents->GetEntity(
            renderer_, backdrop_entity, subpass_coverage);
      }
      if (!direct_glass_material && resolved_backdrop_entity.has_value() &&
          (backdrop_filter->type() == flutter::DlImageFilterType::kBlur ||
           backdrop_filter->type() == flutter::DlImageFilterType::kGlass)) {
        // GaussianBlurFilterContents resolves its final pass through
        // Entity::FromSnapshot. The filter discriminator is the explicit type
        // proof; Flutter's engine deliberately builds without C++ RTTI.
        resolved_backdrop_contents = std::static_pointer_cast<TextureContents>(
            resolved_backdrop_entity->GetContents());
      }
    }
    if (resolved_backdrop_entity.has_value()) {
      resolved_backdrop_entity->SetClipDepth(backdrop_depth);
    }

    const BackdropDirectSource direct_source =
        !isolated_backdrop_snapshot.has_value()
            ? BackdropDirectSource::kFilterEvaluation
        : isolated_backdrop_cache_hit ? BackdropDirectSource::kSnapshotHit
                                      : BackdropDirectSource::kSnapshotBuild;
    RecordBackdropLayerPlan(subpass_size, /*use_msaa=*/false, direct_source);

    // Child operations retain their original depth budget. On GLES, hold a
    // texture-backed result until the first child draw. A compatible external
    // texture consumes it through BackdropSurfaceContents; every other child
    // causes the exact old draw to flush first.
    Save(total_content_depth);
    if (resolved_backdrop_entity.has_value() &&
        resolved_backdrop_contents != nullptr &&
        renderer_.GetContext()->GetBackendType() ==
            Context::BackendType::kOpenGLES) {
      transform_stack_.back().pending_backdrop_composite =
          std::make_shared<PendingBackdropComposite>(PendingBackdropComposite{
              .fallback_entity = std::move(*resolved_backdrop_entity),
              .backdrop_contents = std::move(resolved_backdrop_contents),
              .scene_snapshot = std::move(direct_scene_snapshot),
              .alpha_threshold = backdrop_alpha_threshold,
              .coverage = subpass_coverage,
              .scissor = SetClipScissor(
                  clip_coverage_stack_.CurrentClipCoverage(),
                  GetCurrentRenderPass(), GetGlobalPassPosition()),
          });
      RecordBackdropFusionEvent(BackdropFusionAuditEvent::kCandidate);
    } else if (resolved_backdrop_entity.has_value()) {
      FlushDeferredRRectClip();
      resolved_backdrop_entity->Render(renderer_, GetCurrentRenderPass());
    }
    return;
  }

  const bool use_msaa = !content_is_single_sample_compatible;
  if (backdrop_filter) {
    RecordBackdropLayerPlan(subpass_size, use_msaa);
  }

  std::optional<SaveLayerState::AlphaThresholdBackdrop>
      alpha_threshold_backdrop;
  if (backdrop_alpha_threshold.has_value() && !restore_has_effects &&
      paint.blend_mode == BlendMode::kSrc &&
      renderer_.GetContext()->GetBackendType() ==
          Context::BackendType::kOpenGLES) {
    std::optional<Entity> resolved_entity;
    std::shared_ptr<TextureContents> resolved_contents;
    if (isolated_backdrop_snapshot.has_value()) {
      resolved_entity =
          make_backdrop_snapshot_entity(isolated_backdrop_snapshot.value());
      resolved_contents = std::static_pointer_cast<TextureContents>(
          resolved_entity->GetContents());
    } else if (backdrop_filter_contents) {
      backdrop_filter_contents->SetCoverageHint(subpass_coverage);
      Entity backdrop_entity;
      backdrop_entity.SetContents(backdrop_filter_contents);
      backdrop_entity.SetBlendMode(BlendMode::kSrc);
      backdrop_entity.SetTransform(
          Matrix::MakeTranslation(Vector3(-GetGlobalPassPosition())));
      resolved_entity = backdrop_filter_contents->GetEntity(
          renderer_, backdrop_entity, subpass_coverage);
      if (resolved_entity.has_value()) {
        resolved_contents = std::static_pointer_cast<TextureContents>(
            resolved_entity->GetContents());
      }
    }
    if (resolved_entity.has_value() && resolved_contents) {
      alpha_threshold_backdrop = SaveLayerState::AlphaThresholdBackdrop{
          .entity = std::move(resolved_entity.value()),
          .contents = std::move(resolved_contents),
          .threshold = backdrop_alpha_threshold.value(),
      };
    }
  }

  std::optional<ISize> texture_region;
  const bool pooled_glass_layer =
      IsPooledGlassTargetPaddingRequested() && use_msaa && backdrop_filter &&
      backdrop_filter->type() == flutter::DlImageFilterType::kGlass &&
      !backdrop_alpha_threshold.has_value() && !paint.image_filter &&
      !paint.color_filter &&
      renderer_.GetContext()->GetBackendType() ==
          Context::BackendType::kOpenGLES;
  if (pooled_glass_layer) {
    // During motion, clipping changes a color layer's exact allocation size
    // almost every frame. Pooling a small set of padded sizes avoids retiring
    // large MSAA attachments at that rate. Coverage, origin, clip state and
    // restore geometry remain exact; only the backing allocation grows.
    constexpr int64_t kGranularity = 128;
    const ISize pooled_size =
        ISize{((subpass_size.width + kGranularity - 1) / kGranularity) *
                  kGranularity,
              ((subpass_size.height + kGranularity - 1) / kGranularity) *
                  kGranularity}
            .Min(renderer_.GetContext()
                     ->GetCapabilities()
                     ->GetMaximumRenderPassAttachmentSize());
    if (pooled_size != subpass_size) {
      texture_region = subpass_size;
      subpass_size = pooled_size;
    }
  }

  render_passes_.push_back(
      LazyRenderingConfig(renderer_,                                     //
                          CreateRenderTarget(renderer_,                  //
                                             subpass_size,               //
                                             Color::BlackTransparent(),  //
                                             use_msaa,                   //
                                             false,                      //
                                             pooled_glass_layer          //
                                             )));
  save_layer_state_.push_back(SaveLayerState{
      paint_copy, subpass_coverage.Shift(-coverage_origin_adjustment),
      backdrop_filter != nullptr, std::move(alpha_threshold_backdrop),
      texture_region});

  render_passes_.back().GetInlinePassContext()->GetRenderPass()->SetLabel(
      backdrop_filter ? "Denial Backdrop Layer Color"
                      : "EntityPass Layer Color");

  CanvasStackEntry entry;
  entry.transform = transform_stack_.back().transform;
  entry.clip_depth = current_depth_ + total_content_depth;
  FML_DCHECK(entry.clip_depth <= transform_stack_.back().clip_depth)
      << entry.clip_depth << " <=? " << transform_stack_.back().clip_depth
      << " after allocating " << total_content_depth;
  entry.clip_height = transform_stack_.back().clip_height;
  entry.rendering_mode = Entity::RenderingMode::kSubpassAppendSnapshotTransform;
  entry.did_round_out = did_round_out;
  transform_stack_.emplace_back(entry);

  // Start non-collapsed subpasses with a fresh clip coverage stack limited by
  // the subpass coverage. This is important because image filters applied to
  // save layers may transform the subpass texture after it's rendered,
  // causing parent clip coverage to get misaligned with the actual area that
  // the subpass will affect in the parent pass.
  clip_coverage_stack_.PushSubpass(subpass_coverage, GetClipHeight());

  if (save_layer_state_.back().alpha_threshold_backdrop.has_value()) {
    return;
  }

  if (!backdrop_filter_contents && !isolated_backdrop_snapshot.has_value()) {
    return;
  }

  // Render the backdrop entity.
  Entity backdrop_entity;
  if (isolated_backdrop_snapshot.has_value()) {
    const Snapshot& snapshot = isolated_backdrop_snapshot.value();
    backdrop_entity = Entity::FromSnapshot(snapshot, BlendMode::kSrcOver);
    backdrop_entity.SetTransform(
        Matrix::MakeTranslation(Vector3(-local_position)) *
        backdrop_entity.GetTransform());
  } else {
    if (IsPooledGlassMaterialPaddingRequested() && use_msaa &&
        backdrop_filter &&
        backdrop_filter->type() == flutter::DlImageFilterType::kGlass &&
        !backdrop_alpha_threshold.has_value() && !paint.image_filter &&
        !paint.color_filter &&
        renderer_.GetContext()->GetBackendType() ==
            Context::BackendType::kOpenGLES) {
      // This uncached material is consumed immediately by the child layer.
      // Persistent snapshots and threshold consumers never enter this path.
      std::static_pointer_cast<GlassFilterContents>(backdrop_filter_contents)
          ->SetMaterialTargetPaddingEnabled(true);
    }
    backdrop_entity.SetContents(std::move(backdrop_filter_contents));
    backdrop_entity.SetTransform(
        Matrix::MakeTranslation(Vector3(-local_position)));
  }
  backdrop_entity.SetClipDepth(std::numeric_limits<uint32_t>::max());
  backdrop_entity.Render(renderer_, GetCurrentRenderPass());
}

bool Canvas::Restore() {
  FML_DCHECK(transform_stack_.size() > 0);
  if (transform_stack_.size() == 1) {
    return false;
  }

  // A rounded clip that reached its matching restore without another physical
  // consumer was either used analytically by a fused composite or guarded no
  // pixels at all. In both cases its stencil-and-cover fallback is unnecessary.
  transform_stack_.back().deferred_rrect_clip.reset();
  if (transform_stack_.back().pending_backdrop_composite) {
    FlushPendingBackdropComposite();
  }

  // This check is important to make sure we didn't exceed the depth
  // that the clips were rendered at while rendering any of the
  // rendering ops. It is OK for the current depth to equal the
  // outgoing clip depth because that means the clipping would have
  // been successful up through the last rendering op, but it cannot
  // be greater.
  // Also, we bump the current rendering depth to the outgoing clip
  // depth so that future rendering operations are not clipped by
  // any of the pixels set by the expiring clips. It is OK for the
  // estimates used to determine the clip depth in save/saveLayer
  // to be overly conservative, but we need to jump the depth to
  // the clip depth so that the next rendering op will get a
  // larger depth (it will pre-increment the current_depth_ value).
  FML_DCHECK(current_depth_ <= transform_stack_.back().clip_depth)
      << current_depth_ << " <=? " << transform_stack_.back().clip_depth;
  current_depth_ = transform_stack_.back().clip_depth;

  if (IsSkipping()) {
    transform_stack_.pop_back();
    return true;
  }

  if (transform_stack_.back().rendering_mode ==
          Entity::RenderingMode::kSubpassAppendSnapshotTransform ||
      transform_stack_.back().rendering_mode ==
          Entity::RenderingMode::kSubpassPrependSnapshotTransform) {
    auto lazy_render_pass = std::move(render_passes_.back());
    render_passes_.pop_back();
    // Force the render pass to be constructed if it never was.
    lazy_render_pass.GetInlinePassContext()->GetRenderPass();

    SaveLayerState save_layer_state = std::move(save_layer_state_.back());
    save_layer_state_.pop_back();
    auto global_pass_position = GetGlobalPassPosition();

    std::shared_ptr<Contents> contents = CreateContentsForSubpassTarget(
        save_layer_state.paint,                                    //
        lazy_render_pass.GetInlinePassContext()->GetTexture(),     //
        Matrix::MakeTranslation(Vector3{-global_pass_position}) *  //
            transform_stack_.back().transform,                     //
        save_layer_state.has_backdrop_filter ? "Denial backdrop layer restore"
                                             : "Subpass",  //
        save_layer_state.texture_region);

    lazy_render_pass.GetInlinePassContext()->EndPass();

    // Round the subpass texture position for pixel alignment with the parent
    // pass render target. By default, we draw subpass textures with nearest
    // sampling, so aligning here is important for avoiding visual nearest
    // sampling errors caused by limited floating point precision when
    // straddling a half pixel boundary.
    Point subpass_texture_position;
    if (transform_stack_.back().did_round_out) {
      // Subpass coverage was rounded out, origin potentially moved "down" by
      // as much as a pixel.
      subpass_texture_position =
          (save_layer_state.coverage.GetOrigin() - global_pass_position)
              .Floor();
    } else {
      // Subpass coverage was truncated. Pick the closest phyiscal pixel.
      subpass_texture_position =
          (save_layer_state.coverage.GetOrigin() - global_pass_position)
              .Round();
    }

    if (save_layer_state.alpha_threshold_backdrop.has_value()) {
      auto alpha_threshold_backdrop =
          std::move(save_layer_state.alpha_threshold_backdrop.value());
      auto surface_contents =
          std::static_pointer_cast<TextureContents>(contents);
      const Matrix surface_transform =
          Matrix::MakeTranslation(Vector3(subpass_texture_position));
      auto composite = BackdropSurfaceContents::Make(
          alpha_threshold_backdrop.entity, alpha_threshold_backdrop.contents,
          /*scene_snapshot=*/std::nullopt, surface_contents, surface_transform,
          save_layer_state.coverage.Shift(-global_pass_position),
          alpha_threshold_backdrop.threshold,
          /*analytic_clip=*/std::nullopt,
          /*require_external_surface=*/false);
      if (composite) {
        Entity element_entity;
        element_entity.SetClipDepth(++current_depth_);
        element_entity.SetContents(std::move(composite));
        element_entity.SetBlendMode(BlendMode::kSrcOver);
        element_entity.SetTransform(surface_transform);
        element_entity.Render(
            renderer_,
            *render_passes_.back().GetInlinePassContext()->GetRenderPass());
        clip_coverage_stack_.PopSubpass();
        transform_stack_.pop_back();
        return true;
      }
    }

    Entity element_entity;
    element_entity.SetClipDepth(++current_depth_);
    element_entity.SetContents(std::move(contents));
    element_entity.SetBlendMode(save_layer_state.paint.blend_mode);
    element_entity.SetTransform(
        Matrix::MakeTranslation(Vector3(subpass_texture_position)));

    if (element_entity.GetBlendMode() > Entity::kLastPipelineBlendMode) {
      if (renderer_.GetDeviceCapabilities().SupportsFramebufferFetch()) {
        ApplyFramebufferBlend(element_entity);
      } else {
        // End the active pass and flush the buffer before rendering "advanced"
        // blends. Advanced blends work by binding the current render target
        // texture as an input ("destination"), blending with a second texture
        // input ("source"), writing the result to an intermediate texture, and
        // finally copying the data from the intermediate texture back to the
        // render target texture. And so all of the commands that have written
        // to the render target texture so far need to execute before it's bound
        // for blending (otherwise the blend pass will end up executing before
        // all the previous commands in the active pass).
        auto input_texture = FlipBackdrop(GetGlobalPassPosition());
        if (!input_texture) {
          return false;
        }

        FilterInput::Vector inputs = {
            FilterInput::Make(input_texture,
                              element_entity.GetTransform().Invert()),
            FilterInput::Make(element_entity.GetContents())};
        auto contents = ColorFilterContents::MakeBlend(
            element_entity.GetBlendMode(), inputs);
        contents->SetCoverageHint(element_entity.GetCoverage());
        element_entity.SetContents(std::move(contents));
        element_entity.SetBlendMode(BlendMode::kSrc);
      }
    }

    element_entity.Render(
        renderer_,                                                      //
        *render_passes_.back().GetInlinePassContext()->GetRenderPass()  //
    );
    clip_coverage_stack_.PopSubpass();
    transform_stack_.pop_back();

    // We don't need to restore clips if a saveLayer was performed, as the clip
    // state is per render target, and no more rendering operations will be
    // performed as the render target workloaded is completed in the restore.
    return true;
  }

  size_t num_clips = transform_stack_.back().num_clips;
  transform_stack_.pop_back();

  if (num_clips > 0) {
    EntityPassClipStack::ClipStateResult clip_state_result =
        clip_coverage_stack_.RecordRestore(GetGlobalPassPosition(),
                                           GetClipHeight());

    // Clip restores are never required with depth based clipping.
    FML_DCHECK(!clip_state_result.should_render);
    if (clip_state_result.clip_did_change) {
      // We only need to update the pass scissor if the clip state has changed.
      SetClipScissor(
          clip_coverage_stack_.CurrentClipCoverage(),                      //
          *render_passes_.back().GetInlinePassContext()->GetRenderPass(),  //
          GetGlobalPassPosition()                                          //
      );
    }
  }

  return true;
}

bool Canvas::AttemptBlurredTextOptimization(
    const std::shared_ptr<TextFrame>& text_frame,
    const std::shared_ptr<TextContents>& text_contents,
    Entity& entity,
    const Paint& paint) {
  if (!paint.mask_blur_descriptor.has_value() ||  //
      paint.image_filter != nullptr ||            //
      paint.color_filter != nullptr ||            //
      paint.invert_colors) {
    return false;
  }

  // TODO(bdero): This mask blur application is a hack. It will always wind up
  //              doing a gaussian blur that affects the color source itself
  //              instead of just the mask. The color filter text support
  //              needs to be reworked in order to interact correctly with
  //              mask filters.
  //              https://github.com/flutter/flutter/issues/133297
  std::shared_ptr<FilterContents> filter =
      paint.mask_blur_descriptor->CreateMaskBlur(
          FilterInput::Make(text_contents),
          /*is_solid_color=*/true, GetCurrentTransform());

  std::optional<Glyph> maybe_glyph = text_frame->AsSingleGlyph();
  int64_t identifier = maybe_glyph.has_value()
                           ? maybe_glyph.value().index
                           : reinterpret_cast<int64_t>(text_frame.get());
  TextShadowCache::TextShadowCacheKey cache_key(
      /*p_max_basis=*/entity.GetTransform().GetMaxBasisLengthXY(),
      /*p_identifier=*/identifier,
      /*p_is_single_glyph=*/maybe_glyph.has_value(),
      /*p_font=*/text_frame->GetFont(),
      /*p_sigma=*/paint.mask_blur_descriptor->sigma,
      /*p_color=*/paint.color);

  std::optional<Entity> result = renderer_.GetTextShadowCache().Lookup(
      renderer_, entity, filter, cache_key);
  if (result.has_value()) {
    AddRenderEntityToCurrentPass(result.value(), /*reuse_depth=*/false);
    return true;
  } else {
    return false;
  }
}

// If the text point size * max basis XY is larger than this value,
// render the text as paths (if available) for faster and higher
// fidelity rendering. This is a somewhat arbitrary cutoff
static constexpr Scalar kMaxTextScale = 250;

void Canvas::DrawTextFrame(const std::shared_ptr<TextFrame>& text_frame,
                           Point position,
                           const Paint& paint) {
  Scalar max_scale = GetCurrentTransform().GetMaxBasisLengthXY();
  if (max_scale * text_frame->GetFont().GetMetrics().point_size >
      kMaxTextScale) {
    fml::StatusOr<flutter::DlPath> path = text_frame->GetPath();
    if (path.ok()) {
      Save(1);
      Concat(Matrix::MakeTranslation(position));
      DrawPath(path.value(), paint);
      Restore();
      return;
    }
  }

  Entity entity;
  entity.SetClipDepth(GetClipHeight());
  entity.SetBlendMode(paint.blend_mode);

  auto text_contents = std::make_shared<TextContents>();
  text_contents->SetTextFrame(text_frame);
  text_contents->SetPosition(position);
  text_contents->SetScreenTransform(GetCurrentTransform());
  text_contents->SetForceTextColor(paint.mask_blur_descriptor.has_value());
  text_contents->SetColor(paint.color);
  text_contents->SetTextProperties(paint.color,
                                   paint.style == Paint::Style::kStroke
                                       ? std::optional(paint.stroke)
                                       : std::nullopt);

  entity.SetTransform(GetCurrentTransform().Translate(position));

  if (AttemptBlurredTextOptimization(text_frame, text_contents, entity,
                                     paint)) {
    return;
  }

  entity.SetContents(paint.WithFilters(std::move(text_contents)));
  AddRenderEntityToCurrentPass(entity, false);
}

void Canvas::AddRenderSDFEntityToCurrentPass(
    Entity& entity,
    const Geometry* geom,
    const Paint& paint,
    std::shared_ptr<ColorSourceContents> contents) {
  if (paint.color_source) {
    // UberSDF doesn't perform things like gradients so we blend the SDF
    // with the color source.
    std::shared_ptr<Contents> color_source_contents =
        paint.CreateContents(geom);
    std::shared_ptr<Contents> final_contents = ColorFilterContents::MakeBlend(
        BlendMode::kSrcIn, {FilterInput::Make(std::move(contents)),
                            FilterInput::Make(color_source_contents)});

    Paint new_paint = paint;
    new_paint.color_source = nullptr;
    AddRenderEntityWithFiltersToCurrentPass(entity, geom, new_paint,
                                            /*reuse_depth=*/false,
                                            /*override_contents=*/
                                            std::move(final_contents));
  } else {
    AddRenderEntityWithFiltersToCurrentPass(entity, geom, paint,
                                            /*reuse_depth=*/false,
                                            /*override_contents=*/
                                            std::move(contents));
  }
}

void Canvas::AddRenderEntityWithFiltersToCurrentPass(
    Entity& entity,
    const Geometry* geometry,
    const Paint& paint,
    bool reuse_depth,
    std::shared_ptr<Contents> override_contents) {
  std::shared_ptr<ColorSourceContents> color_source_contents;
  std::shared_ptr<Contents> contents;
  if (override_contents) {
    contents = std::move(override_contents);
  } else {
    color_source_contents = paint.CreateContents(geometry);
    contents = color_source_contents;
  }

  if (!paint.color_filter && !paint.invert_colors && !paint.image_filter &&
      !paint.mask_blur_descriptor.has_value()) {
    entity.SetContents(std::move(contents));
    AddRenderEntityToCurrentPass(entity, reuse_depth);
    return;
  }

  // Attempt to apply the color filter on the CPU first.
  // Note: This is not just an optimization; some color sources rely on
  //       CPU-applied color filters to behave properly.
  bool needs_color_filter = paint.color_filter || paint.invert_colors;
  if (needs_color_filter &&
      contents->ApplyColorFilter([&](Color color) -> Color {
        if (paint.color_filter) {
          color = GetCPUColorFilterProc(paint.color_filter)(color);
        }
        if (paint.invert_colors) {
          color = color.ApplyColorMatrix(kColorInversion);
        }
        return color;
      })) {
    needs_color_filter = false;
  }

  bool can_apply_mask_filter = geometry->CanApplyMaskFilter();

  if (can_apply_mask_filter && paint.mask_blur_descriptor.has_value()) {
    // If there's a mask blur and we need to apply the color filter on the GPU,
    // we need to be careful to only apply the color filter to the source
    // colors. CreateMaskBlur is able to handle this case.
    FML_DCHECK(color_source_contents) << "Mask blur is only supported when no "
                                         "override contents are provided.";
    FillRectGeometry out_rect(Rect{});
    auto filter = paint.mask_blur_descriptor->CreateMaskBlur(
        paint, geometry, color_source_contents, needs_color_filter, &out_rect);
    entity.SetContents(std::move(filter));
    AddRenderEntityToCurrentPass(entity, reuse_depth);
    return;
  }

  std::shared_ptr<Contents> contents_copy = std::move(contents);

  // Image input types will directly set their color filter,
  // if any. See `TiledTextureContents.SetColorFilter`.
  if (needs_color_filter &&
      (!paint.color_source ||
       paint.color_source->type() != flutter::DlColorSourceType::kImage)) {
    if (paint.color_filter) {
      contents_copy = WrapWithGPUColorFilter(
          paint.color_filter, FilterInput::Make(std::move(contents_copy)),
          ColorFilterContents::AbsorbOpacity::kYes);
    }
    if (paint.invert_colors) {
      contents_copy =
          WrapWithInvertColors(FilterInput::Make(std::move(contents_copy)),
                               ColorFilterContents::AbsorbOpacity::kYes);
    }
  }

  if (paint.image_filter) {
    std::shared_ptr<FilterContents> filter = WrapInput(
        paint.image_filter, FilterInput::Make(std::move(contents_copy)));
    filter->SetRenderingMode(Entity::RenderingMode::kDirect);
    entity.SetContents(filter);
    AddRenderEntityToCurrentPass(entity, reuse_depth);
    return;
  }

  entity.SetContents(std::move(contents_copy));
  AddRenderEntityToCurrentPass(entity, reuse_depth);
}

std::shared_ptr<PendingBackdropComposite>*
Canvas::FindPendingBackdropComposite() {
  for (auto entry = transform_stack_.rbegin(); entry != transform_stack_.rend();
       entry++) {
    if (entry->pending_backdrop_composite) {
      return &entry->pending_backdrop_composite;
    }
  }
  return nullptr;
}

std::shared_ptr<DeferredRRectClip>* Canvas::FindDeferredRRectClip() {
  for (auto entry = transform_stack_.rbegin(); entry != transform_stack_.rend();
       entry++) {
    if (entry->deferred_rrect_clip) {
      return &entry->deferred_rrect_clip;
    }
  }
  return nullptr;
}

void Canvas::FlushDeferredRRectClip() {
  auto* slot = FindDeferredRRectClip();
  if (slot == nullptr) {
    return;
  }
  std::shared_ptr<DeferredRRectClip> deferred = std::move(*slot);
  slot->reset();
  const std::shared_ptr<RenderPass>& pass =
      render_passes_.back().GetInlinePassContext()->GetRenderPass();
  if (pass) {
    deferred->contents.Render(renderer_, *pass, deferred->clip_depth,
                              /*is_backdrop_replay=*/false,
                              deferred->cover_scissor);
    // ClipContents emits two commands and consumes encoder state. Re-establish
    // the current logical scissor for the draw that forced materialization.
    SetClipScissor(clip_coverage_stack_.CurrentClipCoverage(), *pass,
                   GetGlobalPassPosition());
  }
  RecordBackdropFusionEvent(BackdropFusionAuditEvent::kFlushedRRect);
}

void Canvas::FlushPendingBackdropComposite() {
  auto* slot = FindPendingBackdropComposite();
  if (slot == nullptr) {
    return;
  }
  std::shared_ptr<PendingBackdropComposite> pending = std::move(*slot);
  slot->reset();
  const std::shared_ptr<RenderPass>& pass =
      render_passes_.back().GetInlinePassContext()->GetRenderPass();
  if (pass) {
    pass->SetScissor(pending->scissor);
    pending->fallback_entity.Render(renderer_, *pass);
    SetClipScissor(clip_coverage_stack_.CurrentClipCoverage(), *pass,
                   GetGlobalPassPosition());
  }
  // The rounded clip is a child operation, so it follows the backdrop
  // assignment when this deferred plan falls back.
  FlushDeferredRRectClip();
  RecordBackdropFusionEvent(BackdropFusionAuditEvent::kFallback);
}

bool Canvas::TryBackdropSurfaceComposite(
    Entity& surface_entity,
    const std::shared_ptr<TextureContents>& surface) {
  auto* pending_slot = FindPendingBackdropComposite();
  if (pending_slot == nullptr) {
    return false;
  }
  const auto surface_coverage = surface_entity.GetCoverage();
  const Rect& required_coverage = (*pending_slot)->coverage;
  const bool coverage_matches =
      surface_coverage.has_value() &&
      surface_coverage->Expand(1.0f).Contains(required_coverage) &&
      required_coverage.Expand(1.0f).Contains(surface_coverage.value());
  const std::shared_ptr<TextureContents>& backdrop =
      (*pending_slot)->backdrop_contents;
  const std::shared_ptr<Texture> surface_texture =
      surface != nullptr ? surface->GetTexture() : nullptr;
  const bool surface_sampling_is_direct =
      surface_texture != nullptr && !surface->GetStrictSourceRect() &&
      Rect::MakeSize(surface_texture->GetSize())
          .Contains(surface->GetSourceRect()) &&
      surface->GetSamplerDescriptor().width_address_mode ==
          SamplerAddressMode::kClampToEdge &&
      surface->GetSamplerDescriptor().height_address_mode ==
          SamplerAddressMode::kClampToEdge;
  const BackdropSurfaceCompositePlanInputs plan_inputs = {
      .backend_supports_external_sampler =
          renderer_.GetContext()->GetBackendType() ==
          Context::BackendType::kOpenGLES,
      .backdrop_is_texture = backdrop != nullptr,
      .surface_is_external_texture =
          BackdropSurfaceContents::SupportsSurfaceTexture(surface),
      .surface_sampling_is_direct = surface_sampling_is_direct,
      .surface_blend_mode = surface_entity.GetBlendMode(),
      .transforms_are_translation_scale =
          surface_entity.GetTransform().IsTranslationScaleOnly() &&
          (*pending_slot)
              ->fallback_entity.GetTransform()
              .IsTranslationScaleOnly(),
      .surface_covers_backdrop_scope = coverage_matches,
  };
  const uint32_t rejections =
      GetBackdropSurfaceCompositeRejections(plan_inputs);
  RecordBackdropFusionPredicate(rejections);
  if (rejections != 0u) {
    return false;
  }

  std::optional<BackdropSurfaceContents::AnalyticRRect> analytic_clip;
  std::shared_ptr<DeferredRRectClip>* deferred_slot = FindDeferredRRectClip();
  const bool use_analytic_clip = deferred_slot != nullptr &&
                                 ((*pending_slot)->scene_snapshot.has_value() ||
                                  (*pending_slot)->alpha_threshold.has_value());
  if (use_analytic_clip) {
    analytic_clip = (*deferred_slot)->analytic_clip;
  }
  auto fused = BackdropSurfaceContents::Make(
      (*pending_slot)->fallback_entity, backdrop,
      (*pending_slot)->scene_snapshot, surface, surface_entity.GetTransform(),
      required_coverage, (*pending_slot)->alpha_threshold, analytic_clip);
  if (!fused) {
    return false;
  }

  const bool uses_alpha_threshold =
      (*pending_slot)->alpha_threshold.has_value();
  pending_slot->reset();
  surface_entity.SetContents(std::move(fused));
  surface_entity.SetBlendMode(uses_alpha_threshold ? BlendMode::kSrcOver
                                                   : BlendMode::kSrc);
  if (use_analytic_clip) {
    RecordBackdropFusionEvent(BackdropFusionAuditEvent::kAnalyticRRect);
  } else if (deferred_slot != nullptr) {
    // A cached filtered backdrop may outlive its unfiltered scene snapshot.
    // Preserve exact clip antialiasing by materializing the already-deferred
    // clip after the logical backdrop has been consumed into the composite.
    FlushDeferredRRectClip();
  }
  RecordBackdropFusionEvent(BackdropFusionAuditEvent::kComposite);
  return true;
}

void Canvas::AddRenderEntityToCurrentPass(
    Entity& entity,
    bool reuse_depth,
    std::shared_ptr<TextureContents> texture_contents) {
  if (IsSkipping()) {
    return;
  }

  entity.SetTransform(
      Matrix::MakeTranslation(Vector3(-GetGlobalPassPosition())) *
      entity.GetTransform());
  entity.SetInheritedOpacity(transform_stack_.back().distributed_opacity);
  if (!TryBackdropSurfaceComposite(entity, texture_contents)) {
    if (FindPendingBackdropComposite() != nullptr) {
      FlushPendingBackdropComposite();
    } else {
      FlushDeferredRRectClip();
    }
  }
  if (entity.GetBlendMode() == BlendMode::kSrcOver &&
      entity.GetContents()->IsOpaque(entity.GetTransform())) {
    entity.SetBlendMode(BlendMode::kSrc);
  }

  // If the entity covers the current render target and is a solid color, then
  // conditionally update the backdrop color to its solid color value blended
  // with the current backdrop.
  if (render_passes_.back().IsApplyingClearColor()) {
    std::optional<Color> maybe_color = entity.AsBackgroundColor(
        render_passes_.back().GetInlinePassContext()->GetTexture()->GetSize());
    if (maybe_color.has_value()) {
      Color color = maybe_color.value();
      RenderTarget& render_target = render_passes_.back()
                                        .GetInlinePassContext()
                                        ->GetPassTarget()
                                        .GetRenderTarget();
      ColorAttachment attachment = render_target.GetColorAttachment(0);
      // Attachment.clear color needs to be premultiplied at all times, but the
      // Color::Blend function requires unpremultiplied colors.
      attachment.clear_color = attachment.clear_color.Unpremultiply()
                                   .Blend(color, entity.GetBlendMode())
                                   .Premultiply();
      render_target.SetColorAttachment(attachment, 0u);
      return;
    }
  }
  if (!reuse_depth) {
    ++current_depth_;
  }

  // We can render at a depth up to and including the depth of the currently
  // active clips and we will still be clipped out, but we cannot render at
  // a depth that is greater than the current clips or we will not be clipped.
  FML_DCHECK(current_depth_ <= transform_stack_.back().clip_depth)
      << current_depth_ << " <=? " << transform_stack_.back().clip_depth;
  entity.SetClipDepth(current_depth_);

  if (entity.GetBlendMode() > Entity::kLastPipelineBlendMode) {
    if (renderer_.GetDeviceCapabilities().SupportsFramebufferFetch()) {
      ApplyFramebufferBlend(entity);
    } else {
      // End the active pass and flush the buffer before rendering "advanced"
      // blends. Advanced blends work by binding the current render target
      // texture as an input ("destination"), blending with a second texture
      // input ("source"), writing the result to an intermediate texture, and
      // finally copying the data from the intermediate texture back to the
      // render target texture. And so all of the commands that have written
      // to the render target texture so far need to execute before it's bound
      // for blending (otherwise the blend pass will end up executing before
      // all the previous commands in the active pass).
      auto input_texture = FlipBackdrop(GetGlobalPassPosition(),  //
                                        /*should_remove_texture=*/false,
                                        /*should_use_onscreen=*/false,
                                        /*post_depth_increment=*/true);
      if (!input_texture) {
        return;
      }

      // The coverage hint tells the rendered Contents which portion of the
      // rendered output will actually be used, and so we set this to the
      // current clip coverage (which is the max clip bounds). The contents may
      // optionally use this hint to avoid unnecessary rendering work.
      auto element_coverage_hint = entity.GetContents()->GetCoverageHint();
      entity.GetContents()->SetCoverageHint(Rect::Intersection(
          element_coverage_hint, clip_coverage_stack_.CurrentClipCoverage()));

      FilterInput::Vector inputs = {
          FilterInput::Make(input_texture, entity.GetTransform().Invert()),
          FilterInput::Make(entity.GetContents())};
      auto contents =
          ColorFilterContents::MakeBlend(entity.GetBlendMode(), inputs);
      entity.SetContents(std::move(contents));
      entity.SetBlendMode(BlendMode::kSrc);
    }
  }

  const std::shared_ptr<RenderPass>& result =
      render_passes_.back().GetInlinePassContext()->GetRenderPass();
  if (!result) {
    // Failure to produce a render pass should be explained by specific errors
    // in `InlinePassContext::GetRenderPass()`, so avoid log spam and don't
    // append a validation log here.
    return;
  }

  entity.Render(renderer_, *result);
}

RenderPass& Canvas::GetCurrentRenderPass() const {
  return *render_passes_.back().GetInlinePassContext()->GetRenderPass();
}

void Canvas::SetBackdropData(
    std::unordered_map<int64_t, BackdropData> backdrop_data,
    size_t backdrop_count,
    BackdropEpochPlan backdrop_epoch_plan) {
  backdrop_data_ = std::move(backdrop_data);
  backdrop_count_ = backdrop_count;
  backdrop_epoch_plan_ = std::move(backdrop_epoch_plan);
  backdrop_epoch_cursor_.Reset();
  active_backdrop_epoch_.reset();
  active_backdrop_epoch_texture_.reset();
  RecordBackdropGraphPlan(backdrop_epoch_plan_);
}

std::optional<uint32_t> Canvas::ClaimBackdropEpoch(
    const Rect& write_region,
    const flutter::DlImageFilter& backdrop_filter) {
  Rect read_region = write_region;
  if (!write_region.IsEmpty()) {
    flutter::DlIRect input_bounds;
    if (backdrop_filter.get_input_device_bounds(
            flutter::DlIRect::RoundOut(write_region), GetCurrentTransform(),
            input_bounds)) {
      read_region = Rect::Make(input_bounds);
      if (initial_cull_rect_.has_value() && !initial_cull_rect_->IsMaximum()) {
        read_region = read_region.Intersection(initial_cull_rect_.value())
                          .value_or(Rect());
      }
    } else if (initial_cull_rect_.has_value()) {
      read_region = initial_cull_rect_.value();
    } else {
      return std::nullopt;
    }
  }

  return backdrop_epoch_cursor_.Claim(backdrop_epoch_plan_, write_region,
                                      read_region);
}

std::shared_ptr<Texture> Canvas::FlipBackdrop(Point global_pass_position,
                                              bool should_remove_texture,
                                              bool should_use_onscreen,
                                              bool post_depth_increment) {
  // Any standalone readback closes the active graph epoch. A direct backdrop
  // caller may install the returned scene snapshot as the input for a new
  // epoch after this method returns.
  active_backdrop_epoch_.reset();
  active_backdrop_epoch_texture_.reset();

  LazyRenderingConfig rendering_config = std::move(render_passes_.back());
  render_passes_.pop_back();

  const ColorAttachment prior_color = rendering_config.GetEntityPassTarget()
                                          ->GetRenderTarget()
                                          .GetColorAttachment(0);

  // If the very first thing we render in this EntityPass is a subpass that
  // happens to have a backdrop filter or advanced blend, than that backdrop
  // filter/blend will sample from an uninitialized texture.
  //
  // By calling `pass_context.GetRenderPass` here, we force the texture to pass
  // through at least one RenderPass with the correct clear configuration before
  // any sampling occurs.
  //
  // In cases where there are no contents, we
  // could instead check the clear color and initialize a 1x2 CPU texture
  // instead of ending the pass.
  rendering_config.GetInlinePassContext()->GetRenderPass();
  if (!rendering_config.GetInlinePassContext()->EndPass()) {
    VALIDATION_LOG
        << "Failed to end the current render pass in order to read from "
           "the backdrop texture and apply an advanced blend or backdrop "
           "filter.";
    // Note: adding this render pass ensures there are no later crashes from
    // unbalanced save layers. Ideally, this method would return false and the
    // renderer could handle that by terminating dispatch.
    render_passes_.emplace_back(std::move(rendering_config));
    return nullptr;
  }

  const std::shared_ptr<Texture>& input_texture =
      rendering_config.GetInlinePassContext()->GetTexture();

  if (!input_texture) {
    VALIDATION_LOG << "Failed to fetch the color texture in order to "
                      "apply an advanced blend or backdrop filter.";

    // Note: see above.
    render_passes_.emplace_back(std::move(rendering_config));
    return nullptr;
  }

  const bool clip_state_is_preserved =
      !should_use_onscreen && rendering_config.GetInlinePassContext()
                                  ->PreservesDepthStencilBetweenPasses();

  if (should_use_onscreen) {
    ColorAttachment color0 = render_target_.GetColorAttachment(0);
    // When MSAA is being used, we end up overriding the entire backdrop by
    // drawing the previous pass texture, and so we don't have to clear it and
    // can use kDontCare.
    color0.load_action = color0.resolve_texture != nullptr
                             ? LoadAction::kDontCare
                             : LoadAction::kLoad;
    render_target_.SetColorAttachment(color0, 0);

    auto entity_pass_target = std::make_unique<EntityPassTarget>(
        render_target_,                                                    //
        renderer_.GetDeviceCapabilities().SupportsReadFromResolve(),       //
        renderer_.GetDeviceCapabilities().SupportsImplicitResolvingMSAA()  //
    );
    render_passes_.push_back(
        LazyRenderingConfig(renderer_, std::move(entity_pass_target)));
    requires_readback_ = false;
  } else {
    render_passes_.emplace_back(std::move(rendering_config));
    // If the current texture is being cached for a BDF we need to ensure we
    // don't recycle it during recording; remove it from the entity pass target.
    if (should_remove_texture) {
      render_passes_.back().GetEntityPassTarget()->RemoveSecondary();
    }
  }
  RenderPass& current_render_pass =
      *render_passes_.back().GetInlinePassContext()->GetRenderPass();

  const ColorAttachment current_color = render_passes_.back()
                                            .GetEntityPassTarget()
                                            ->GetRenderTarget()
                                            .GetColorAttachment(0);
  if (current_color.resolve_texture) {
    // MSAA attachments cannot load their single-sample resolve texture. Copy
    // the resolved backdrop into the new attachment instead. Single-sample
    // targets preserve the same storage with LoadAction::kLoad and need no
    // restore draw.
    Rect size_rect = Rect::MakeSize(input_texture->GetSize());
    auto msaa_backdrop_contents = TextureContents::MakeRect(size_rect);
    msaa_backdrop_contents->SetStencilEnabled(false);
    msaa_backdrop_contents->SetLabel("MSAA backdrop");
    msaa_backdrop_contents->SetSourceRect(size_rect);
    msaa_backdrop_contents->SetTexture(input_texture);

    Entity msaa_backdrop_entity;
    msaa_backdrop_entity.SetContents(std::move(msaa_backdrop_contents));
    msaa_backdrop_entity.SetBlendMode(BlendMode::kSrc);
    msaa_backdrop_entity.SetClipDepth(std::numeric_limits<uint32_t>::max());
    if (!msaa_backdrop_entity.Render(renderer_, current_render_pass)) {
      VALIDATION_LOG << "Failed to render MSAA backdrop entity.";
      return nullptr;
    }
  }

  // A resumed pass over the same target loads its existing depth/stencil
  // attachments. The scissor is encoder state rather than attachment state,
  // however, and every new render pass starts without it. Restore the current
  // scissor even when replaying the stencil clips is unnecessary.
  if (clip_state_is_preserved) {
    SetClipScissor(clip_coverage_stack_.CurrentClipCoverage(),
                   current_render_pass, global_pass_position);
  } else {
    auto& replay_entities = clip_coverage_stack_.GetReplayEntities();
    uint64_t current_depth =
        post_depth_increment ? current_depth_ - 1 : current_depth_;
    for (const auto& replay : replay_entities) {
      if (replay.clip_depth <= current_depth) {
        continue;
      }

      const IRect32 clip_scissor = SetClipScissor(
          replay.clip_coverage, current_render_pass, global_pass_position);
      if (!replay.clip_contents.Render(
              renderer_, current_render_pass, replay.clip_depth,
              /*is_backdrop_replay=*/true, clip_scissor)) {
        VALIDATION_LOG << "Failed to render entity for clip restore.";
      }
    }
  }

  return input_texture;
}

bool Canvas::SupportsBlitToOnscreen() const {
  return renderer_.GetContext()
             ->GetCapabilities()
             ->SupportsTextureToTextureBlits() &&
         renderer_.GetContext()->GetBackendType() ==
             Context::BackendType::kMetal;
}

bool Canvas::BlitToOnscreen(bool is_onscreen) {
  auto command_buffer = renderer_.GetContext()->CreateCommandBuffer();
  command_buffer->SetLabel("EntityPass Root Command Buffer");
  auto offscreen_target = render_passes_.back()
                              .GetInlinePassContext()
                              ->GetPassTarget()
                              .GetRenderTarget();
  if (SupportsBlitToOnscreen()) {
    auto blit_pass = command_buffer->CreateBlitPass();
    blit_pass->AddCopy(offscreen_target.GetRenderTargetTexture(),
                       render_target_.GetRenderTargetTexture());
    if (!blit_pass->EncodeCommands()) {
      VALIDATION_LOG << "Failed to encode root pass blit command.";
      return false;
    }
  } else {
    auto render_pass = command_buffer->CreateRenderPass(render_target_);
    render_pass->SetLabel("EntityPass Root Render Pass");

    {
      auto size_rect = Rect::MakeSize(offscreen_target.GetRenderTargetSize());
      auto contents = TextureContents::MakeRect(size_rect);
      contents->SetTexture(offscreen_target.GetRenderTargetTexture());
      contents->SetSourceRect(size_rect);
      contents->SetLabel("Root pass blit");

      Entity entity;
      entity.SetContents(contents);
      entity.SetBlendMode(BlendMode::kSrc);

      if (!entity.Render(renderer_, *render_pass)) {
        VALIDATION_LOG << "Failed to render EntityPass root blit.";
        return false;
      }
    }

    if (!render_pass->EncodeCommands()) {
      VALIDATION_LOG << "Failed to encode root pass command buffer.";
      return false;
    }
  }

  if (is_onscreen) {
    return renderer_.GetContext()->SubmitOnscreen(std::move(command_buffer));
  } else {
    return renderer_.GetContext()->EnqueueCommandBuffer(
        std::move(command_buffer));
  }
}

bool Canvas::EnsureFinalMipmapGeneration() const {
  if (!render_target_.GetRenderTargetTexture()->NeedsMipmapGeneration()) {
    return true;
  }
  std::shared_ptr<CommandBuffer> cmd_buffer =
      renderer_.GetContext()->CreateCommandBuffer();
  if (!cmd_buffer) {
    return false;
  }
  std::shared_ptr<BlitPass> blit_pass = cmd_buffer->CreateBlitPass();
  if (!blit_pass) {
    return false;
  }
  blit_pass->GenerateMipmap(render_target_.GetRenderTargetTexture());
  blit_pass->EncodeCommands();
  return renderer_.GetContext()->EnqueueCommandBuffer(std::move(cmd_buffer));
}

void Canvas::EndReplay() {
  FML_DCHECK(render_passes_.size() == 1u);
  render_passes_.back().GetInlinePassContext()->GetRenderPass();
  render_passes_.back().GetInlinePassContext()->EndPass(
      /*is_onscreen=*/!requires_readback_ && is_onscreen_);
  FlushBackdropLayerPlanAudit();
  FlushBackdropGraphPlanAudit();
  backdrop_data_.clear();
  backdrop_epoch_plan_ = {};
  backdrop_epoch_cursor_.Reset();
  active_backdrop_epoch_.reset();
  active_backdrop_epoch_texture_.reset();

  // If requires_readback_ was true, then we rendered to an offscreen texture
  // instead of to the onscreen provided in the render target. Now we need to
  // draw or blit the offscreen back to the onscreen.
  if (requires_readback_) {
    BlitToOnscreen(/*is_onscreen_=*/is_onscreen_);
  }
  if (!EnsureFinalMipmapGeneration()) {
    VALIDATION_LOG << "Failed to generate onscreen mipmaps.";
  }
  if (!renderer_.GetContext()->FlushCommandBuffers()) {
    // Not much we can do.
    VALIDATION_LOG << "Failed to submit command buffers";
  }
  render_passes_.clear();
  renderer_.GetRenderTargetCache()->End();
  clip_geometry_.clear();

  Reset();
  Initialize(initial_cull_rect_);
}

LazyRenderingConfig::LazyRenderingConfig(
    ContentContext& renderer,
    std::unique_ptr<EntityPassTarget> p_entity_pass_target,
    bool preserve_depth_stencil_between_passes)
    : entity_pass_target_(std::move(p_entity_pass_target)) {
  inline_pass_context_ = std::make_unique<InlinePassContext>(
      renderer, *entity_pass_target_, preserve_depth_stencil_between_passes);
}

bool LazyRenderingConfig::IsApplyingClearColor() const {
  return !inline_pass_context_->IsActive();
}

EntityPassTarget* LazyRenderingConfig::GetEntityPassTarget() const {
  return entity_pass_target_.get();
}

InlinePassContext* LazyRenderingConfig::GetInlinePassContext() const {
  return inline_pass_context_.get();
}

}  // namespace impeller
