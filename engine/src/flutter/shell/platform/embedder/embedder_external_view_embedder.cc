// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_view_embedder.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>

#include "flutter/common/constants.h"
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/display_list/utils/dl_receiver_utils.h"
#include "flutter/flow/diff_context.h"
#include "flutter/shell/platform/embedder/embedder_layers.h"
#include "flutter/shell/platform/embedder/embedder_render_target.h"
#include "third_party/skia/include/gpu/ganesh/GrDirectContext.h"

#ifdef IMPELLER_SUPPORTS_RENDERING
#include "impeller/core/texture_descriptor.h"         // nogncheck
#include "impeller/display_list/dl_dispatcher.h"      // nogncheck
#include "impeller/display_list/dl_image_impeller.h"  // nogncheck
#include "impeller/renderer/capabilities.h"           // nogncheck
#include "impeller/renderer/render_target.h"          // nogncheck
#endif  // IMPELLER_SUPPORTS_RENDERING

namespace flutter {

static const auto kRootViewIdentifier = EmbedderExternalView::ViewIdentifier{};

// Matches deniald's DENIAL_RENDER_AUDIT flag: the canonical name wins over
// the legacy DENIA_ spelling; 1, true, yes or on, trimmed, any case.
static bool DenialRenderAuditRequested() {
  const char* value = std::getenv("DENIAL_RENDER_AUDIT");
  if (value == nullptr) {
    value = std::getenv("DENIA_RENDER_AUDIT");
  }
  if (value == nullptr) {
    return false;
  }
  std::string flag(value);
  const auto first = flag.find_first_not_of(" \t\n\r\f\v");
  if (first == std::string::npos) {
    return false;
  }
  flag = flag.substr(first, flag.find_last_not_of(" \t\n\r\f\v") - first + 1);
  for (char& c : flag) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return flag == "1" || flag == "true" || flag == "yes" || flag == "on";
}

// Cut identifiers never collide with Dart platform view identifiers.
static constexpr int64_t kDenialCutBase = int64_t{1} << 40;

// Texture-only frames of one tree that are drawn directly before a layered
// build. A build records and renders every changed slice, which only pays off
// once the tree has stopped changing; a shorter streak would build between
// the frames of an animation.
static constexpr uint32_t kDenialLayeredBuildStreak = 4;

EmbedderExternalViewEmbedder::EmbedderExternalViewEmbedder(
    bool avoid_backing_store_cache,
    const CreateRenderTargetCallback& create_render_target_callback,
    const PresentCallback& present_callback)
    : avoid_backing_store_cache_(avoid_backing_store_cache),
      create_render_target_callback_(create_render_target_callback),
      present_callback_(present_callback),
      denial_layered_audit_enabled_(DenialRenderAuditRequested()) {
  FML_DCHECK(create_render_target_callback_);
  FML_DCHECK(present_callback_);
}

bool EmbedderExternalViewEmbedder::DenialLayeredComposition() const {
  return denial_output_mode_ == DenialOutputMode::kLayeredBuild;
}

DenialOutputMode EmbedderExternalViewEmbedder::DenialBeginOutputFrame(
    int64_t flutter_view_id,
    const void* layer_tree,
    bool texture_only) {
  denial_frame_tree_ = layer_tree;
  denial_output_mode_ =
      DenialChooseOutputMode(flutter_view_id, layer_tree, texture_only);
  if (denial_output_mode_ == DenialOutputMode::kDirect &&
      flutter_view_id < kFlutterImplicitViewId) {
    // A direct frame paints only the root slice and presents the whole
    // target, so nothing asks for its paint region: record it without the
    // rtree, which costs a search and copy per picture layer and a region
    // per frame. Layered builds keep it for slice bounds and blit rects.
    if (auto root = pending_views_.find(kRootViewIdentifier);
        root != pending_views_.end()) {
      root->second->RecordWithoutRegion();
    }
  }
  return denial_output_mode_;
}

DenialOutputMode EmbedderExternalViewEmbedder::DenialChooseOutputMode(
    int64_t flutter_view_id,
    const void* layer_tree,
    bool texture_only) {
  if (flutter_view_id >= kFlutterImplicitViewId) {
    return DenialOutputMode::kDirect;
  }
  auto& view = denial_slices_[flutter_view_id];
  view.texture_only_streak = texture_only ? view.texture_only_streak + 1 : 0;
  if (!texture_only) {
    view.unlayered_tree = nullptr;
  }
  if (texture_only && view.tree == layer_tree &&
      view.frame_size == pending_frame_size_ &&
      view.transformation == pending_surface_transformation_) {
    return DenialOutputMode::kRecompose;
  }
  // Every other frame replaces the target's pixels without the entries, so
  // they no longer describe it.
  view.tree = nullptr;
  view.entries.clear();
  if (texture_only && layer_tree != view.unlayered_tree &&
      view.texture_only_streak >= kDenialLayeredBuildStreak) {
    return DenialOutputMode::kLayeredBuild;
  }
  return DenialOutputMode::kDirect;
}

int64_t EmbedderExternalViewEmbedder::DenialRegisterCut(
    std::unique_ptr<EmbeddedViewParams> params,
    DenialCutEntry entry) {
  const int64_t cut_id = kDenialCutBase + denial_next_cut_++;
  PrerollCompositeEmbeddedView(cut_id, std::move(params));
  denial_cuts_.emplace(cut_id, std::move(entry));
  return cut_id;
}

EmbedderExternalViewEmbedder::DenialLayeredAudit*
EmbedderExternalViewEmbedder::DenialLayeredAuditFrame(int64_t flutter_view_id) {
  if (!denial_layered_audit_enabled_) {
    return nullptr;
  }
  const fml::TimePoint now = fml::TimePoint::Now();
  auto [it, inserted] = denial_layered_audits_.try_emplace(flutter_view_id);
  DenialLayeredAudit& audit = it->second;
  if (inserted) {
    audit.interval_start = now;
    return &audit;
  }
  const fml::TimeDelta interval = now - audit.interval_start;
  if (interval < fml::TimeDelta::FromSeconds(1)) {
    return &audit;
  }
  // The interval is [interval_start, now): the frame being submitted belongs
  // to the next one. Retained storage is a gauge at its end.
  uint64_t retained_slices = 0;
  uint64_t retained_pixels = 0;
  if (auto slices = denial_slices_.find(flutter_view_id);
      slices != denial_slices_.end()) {
    for (const auto& slot : slices->second.slots) {
      if (slot.storage.image) {
        ++retained_slices;
        retained_pixels += static_cast<uint64_t>(slot.storage.size.width) *
                           static_cast<uint64_t>(slot.storage.size.height);
      }
    }
  }
  const uint64_t rendered =
      audit.rendered_new + audit.rendered_bounds + audit.rendered_content;
  FML_LOG(IMPORTANT)
      << "Denial layered composition audit view=" << flutter_view_id
      << " interval_ms=" << interval.ToMillisecondsF()
      << " direct_frames=" << audit.direct_frames
      << " layered_frames=" << audit.layered_frames
      << " recomposed_frames=" << audit.recomposed_frames
      << " single_slice_frames=" << audit.single_slice_frames
      << " failed_frames=" << audit.failed_frames
      << " composed_pixels=" << audit.composed_pixels
      << " slice_blit_pixels=" << audit.slice_blit_pixels
      << " window_cuts=" << audit.window_cuts
      << " material_cuts=" << audit.material_cuts
      << " texture_cuts=" << audit.texture_cuts
      << " max_cuts=" << audit.max_cuts << " slices=" << audit.slices
      << " max_slices=" << audit.max_slices
      << " slices_reused=" << audit.slices_reused
      << " slices_rendered=" << rendered
      << " rendered_new=" << audit.rendered_new
      << " rendered_bounds=" << audit.rendered_bounds
      << " rendered_content=" << audit.rendered_content
      << " rendered_pixels=" << audit.rendered_pixels
      << " rendered_slice_pixels=" << audit.rendered_slice_pixels
      << " slice_allocations=" << audit.slice_allocations
      << " slice_pool_reuses=" << audit.slice_pool_reuses
      << " retained_slices=" << retained_slices
      << " retained_slice_pixels=" << retained_pixels
      << " slice_render_total_us=" << audit.slice_render_time.ToMicrosecondsF()
      << " compose_total_us=" << audit.compose_time.ToMicrosecondsF()
      << " compose_max_us=" << audit.compose_max.ToMicrosecondsF()
      << " recompose_total_us=" << audit.recompose_time.ToMicrosecondsF()
      << " recompose_max_us=" << audit.recompose_max.ToMicrosecondsF();
  audit = DenialLayeredAudit{};
  audit.interval_start = now;
  return &audit;
}

EmbedderExternalViewEmbedder::~EmbedderExternalViewEmbedder() = default;

void EmbedderExternalViewEmbedder::CollectView(int64_t view_id) {
  render_target_caches_.erase(view_id);
  denial_slices_.erase(view_id);
  denial_layered_audits_.erase(view_id);
}

void EmbedderExternalViewEmbedder::SetSurfaceTransformationCallback(
    SurfaceTransformationCallback surface_transformation_callback) {
  surface_transformation_callback_ = std::move(surface_transformation_callback);
}

void EmbedderExternalViewEmbedder::SetExistingDamageCallback(
    ExistingDamageCallback existing_damage_callback) {
  existing_damage_callback_ = std::move(existing_damage_callback);
}

std::optional<ExternalViewEmbedder::PreparedRenderTarget>
EmbedderExternalViewEmbedder::PrepareDenialRenderTarget(
    int64_t flutter_view_id,
    GrDirectContext* context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    const std::shared_ptr<TextureRegistry>& texture_registry) {
  FML_DCHECK(flutter_view_id < kFlutterImplicitViewId);
  FML_DCHECK(!pending_denial_render_target_);
  denial_texture_registry_ = texture_registry;

  FlutterBackingStoreConfig config = {};
  config.struct_size = sizeof(config);
  config.size.width = pending_frame_size_.width;
  config.size.height = pending_frame_size_.height;
  config.view_id = flutter_view_id;
  auto target = create_render_target_callback_(context, aiks_context, config);
  if (!target) {
    return std::nullopt;
  }

  std::optional<DlRegion> existing_damage;
  const FlutterBackingStore* backing_store = target->GetBackingStore();
  if (existing_damage_callback_ && backing_store &&
      backing_store->type == kFlutterBackingStoreTypeOpenGL &&
      backing_store->open_gl.type == kFlutterOpenGLTargetTypeFramebuffer) {
    existing_damage =
        existing_damage_callback_(backing_store->open_gl.framebuffer.name);
  }
  pending_denial_render_target_ = std::move(target);
  return PreparedRenderTarget{.existing_damage = std::move(existing_damage)};
}

DlMatrix EmbedderExternalViewEmbedder::GetSurfaceTransformation() const {
  if (!surface_transformation_callback_) {
    return DlMatrix{};
  }

  return surface_transformation_callback_();
}

void EmbedderExternalViewEmbedder::Reset() {
  pending_denial_render_target_.reset();
  pending_views_.clear();
  composition_order_.clear();
  denial_cuts_.clear();
  denial_next_cut_ = 0;
  denial_output_mode_ = DenialOutputMode::kDirect;
  denial_frame_tree_ = nullptr;
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::CancelFrame() {
  Reset();
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::BeginFrame(
    GrDirectContext* context,
    const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger) {}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::PrepareFlutterView(
    DlISize frame_size,
    double device_pixel_ratio) {
  PrepareFlutterView(kFlutterImplicitViewId, frame_size, device_pixel_ratio);
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::PrepareFlutterView(
    int64_t flutter_view_id,
    DlISize frame_size,
    double device_pixel_ratio) {
  Reset();

  pending_flutter_view_id_ = flutter_view_id;
  pending_frame_size_ = frame_size;
  pending_device_pixel_ratio_ = device_pixel_ratio;
  if (flutter_view_id < kFlutterImplicitViewId) {
    // Denial reserves negative view IDs for physical render targets. OpenGL
    // backing stores have a bottom-left origin, so each target needs its own
    // height in the root flip instead of inheriting the implicit atlas height.
    pending_surface_transformation_ = DlMatrix(
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, static_cast<DlScalar>(frame_size.height), 0.0f, 1.0f);
  } else {
    pending_surface_transformation_ = GetSurfaceTransformation();
  }

  pending_views_[kRootViewIdentifier] = std::make_unique<EmbedderExternalView>(
      pending_frame_size_, pending_surface_transformation_);
  composition_order_.push_back(kRootViewIdentifier);
}

// |ExternalViewEmbedder|
void EmbedderExternalViewEmbedder::PrerollCompositeEmbeddedView(
    int64_t view_id,
    std::unique_ptr<EmbeddedViewParams> params) {
  auto vid = EmbedderExternalView::ViewIdentifier(view_id);
  FML_DCHECK(pending_views_.count(vid) == 0);

  pending_views_[vid] = std::make_unique<EmbedderExternalView>(
      pending_frame_size_,              // frame size
      pending_surface_transformation_,  // surface xformation
      vid,                              // view identifier
      std::move(params)                 // embedded view params
  );
  composition_order_.push_back(vid);
}

// |ExternalViewEmbedder|
DlCanvas* EmbedderExternalViewEmbedder::GetRootCanvas() {
  auto found = pending_views_.find(kRootViewIdentifier);
  if (found == pending_views_.end()) {
    FML_DLOG(WARNING)
        << "No root canvas could be found. This is extremely unlikely and "
           "indicates that the external view embedder did not receive the "
           "notification to begin the frame.";
    return nullptr;
  }
  return found->second->GetCanvas();
}

// |ExternalViewEmbedder|
std::optional<DlMatrix>
EmbedderExternalViewEmbedder::GetRootCanvasToRenderTargetTransform() const {
  return pending_surface_transformation_;
}

// |ExternalViewEmbedder|
DlCanvas* EmbedderExternalViewEmbedder::CompositeEmbeddedView(int64_t view_id) {
  auto vid = EmbedderExternalView::ViewIdentifier(view_id);
  auto found = pending_views_.find(vid);
  if (found == pending_views_.end()) {
    FML_DCHECK(false) << "Attempted to composite a view that was not "
                         "pre-rolled.";
    return nullptr;
  }
  return found->second->GetCanvas();
}

static FlutterBackingStoreConfig MakeBackingStoreConfig(
    int64_t view_id,
    const DlISize& backing_store_size) {
  FlutterBackingStoreConfig config = {};

  config.struct_size = sizeof(config);

  config.size.width = backing_store_size.width;
  config.size.height = backing_store_size.height;
  config.view_id = view_id;

  return config;
}

namespace {

struct PlatformView {
  EmbedderExternalView::ViewIdentifier view_identifier;
  const EmbeddedViewParams* params;

  // The frame of the platform view, after clipping, in screen coordinates.
  SkRect clipped_frame;

  explicit PlatformView(const EmbedderExternalView* view) {
    FML_DCHECK(view->HasPlatformView());
    view_identifier = view->GetViewIdentifier();
    params = view->GetEmbeddedViewParams();

    DlRect clip = view->GetEmbeddedViewParams()->finalBoundingRect();
    DlMatrix matrix;
    for (auto i = params->mutatorsStack().Begin();
         i != params->mutatorsStack().End(); ++i) {
      const auto& m = *i;
      switch (m->GetType()) {
        case MutatorType::kClipRect: {
          auto rect = m->GetRect().TransformAndClipBounds(matrix);
          clip = clip.IntersectionOrEmpty(rect);
          break;
        }
        case MutatorType::kClipRRect: {
          auto rect = m->GetRRect().GetBounds().TransformAndClipBounds(matrix);
          clip = clip.IntersectionOrEmpty(rect);
          break;
        }
        case MutatorType::kClipRSE: {
          auto rect = m->GetRSE().GetBounds().TransformAndClipBounds(matrix);
          clip = clip.IntersectionOrEmpty(rect);
          break;
        }
        case MutatorType::kClipPath: {
          auto rect = m->GetPath().GetBounds().TransformAndClipBounds(matrix);
          clip = clip.IntersectionOrEmpty(rect);
          break;
        }
        case MutatorType::kTransform: {
          matrix = matrix * m->GetMatrix();
          break;
        }
        case MutatorType::kOpacity:
        case MutatorType::kBackdropFilter:
        case MutatorType::kBackdropClipRect:
        case MutatorType::kBackdropClipRRect:
        case MutatorType::kBackdropClipRSuperellipse:
        case MutatorType::kBackdropClipPath:
          break;
      }
    }
    clipped_frame = ToSkRect(clip);
  }
};

/// Each layer will result in a single physical surface that contains Flutter
/// contents. It may contain multiple platform views and the slices
/// that would be otherwise rendered between these platform views will be
/// collapsed into this layer, as long as they do not intersect any of the
/// platform views.
/// In Z order the Flutter contents of Layer is above the platform views.
class Layer {
 public:
  /// Returns whether the rectangle intersects any of the platform views of
  /// this layer.
  bool IntersectsPlatformView(const SkRect& rect) {
    for (auto& platform_view : platform_views_) {
      if (platform_view.clipped_frame.intersects(rect)) {
        return true;
      }
    }
    return false;
  }

  /// Returns whether the region intersects any of the platform views of this
  /// layer.
  bool IntersectsPlatformView(const DlRegion& region) {
    for (auto& platform_view : platform_views_) {
      auto clipped_frame = ToDlIRect(platform_view.clipped_frame.roundOut());
      if (region.intersects(clipped_frame)) {
        return true;
      }
    }
    return false;
  }

  /// Returns whether the rectangle intersects any of the Flutter contents of
  /// this layer.
  bool IntersectsFlutterContents(const SkRect& rect) {
    return flutter_contents_region_.intersects(ToDlIRect(rect.roundOut()));
  }

  /// Returns whether the region intersects any of the Flutter contents of this
  /// layer.
  bool IntersectsFlutterContents(const DlRegion& region) {
    return flutter_contents_region_.intersects(region);
  }

  /// Adds a platform view to this layer.
  void AddPlatformView(const PlatformView& platform_view) {
    platform_views_.push_back(platform_view);
  }

  /// Adds Flutter contents to this layer.
  void AddFlutterContents(EmbedderExternalView* contents,
                          const DlRegion& contents_region) {
    flutter_contents_.push_back(contents);
    flutter_contents_region_ =
        DlRegion::MakeUnion(flutter_contents_region_, contents_region);
  }

  bool has_flutter_contents() const { return !flutter_contents_.empty(); }

  void SetRenderTarget(std::unique_ptr<EmbedderRenderTarget> target) {
    FML_DCHECK(render_target_ == nullptr);
    FML_DCHECK(has_flutter_contents());
    render_target_ = std::move(target);
  }

  /// Renders this layer Flutter contents to the render target previously
  /// assigned with SetRenderTarget.
  void RenderFlutterContents(bool frame_boundary) {
    FML_DCHECK(has_flutter_contents());
    if (!render_target_) {
      return;
    }

#ifdef IMPELLER_SUPPORTS_RENDERING
    if (render_target_->GetImpellerRenderTarget()) {
      RenderFlutterContentsImpeller(frame_boundary);
      return;
    }
#endif  // IMPELLER_SUPPORTS_RENDERING

#if SLIMPELLER
    FML_LOG(FATAL) << "Impeller opt-out unavailable.";
#else   // SLIMPELLER
    RenderFlutterContentsSkia();
#endif  // SLIMPELLER
  }

  /// Returns platform views for this layer. In Z-order the platform views are
  /// positioned *below* this layer's Flutter contents.
  const std::vector<PlatformView>& platform_views() const {
    return platform_views_;
  }

  EmbedderRenderTarget* render_target() { return render_target_.get(); }

  std::vector<DlIRect> coverage() {
    return flutter_contents_region_.getRects();
  }

 private:
#if !SLIMPELLER
  // TODO(https://github.com/flutter/flutter/issues/151670): Implement this
  // for Impeller as well.
  static void InvalidateApiState(SkSurface& skia_surface) {
    auto recording_context = skia_surface.recordingContext();

    // Should never happen.
    FML_DCHECK(recording_context) << "Recording context was null.";

    auto direct_context = recording_context->asDirectContext();
    if (direct_context == nullptr) {
      // Can happen when using software rendering.
      // Print an error but otherwise continue in that case.
      FML_LOG(ERROR) << "Embedder asked to invalidate cached graphics API "
                        "state but Flutter is not using a graphics API.";
    } else {
      direct_context->resetContext(kAll_GrBackendState);
    }
  }

  void RenderFlutterContentsSkia() {
    auto skia_surface = render_target_->GetSkiaSurface();
    if (!skia_surface) {
      return;
    }

    auto [ok, invalidate_api_state] = render_target_->MaybeMakeCurrent();

    if (invalidate_api_state) {
      InvalidateApiState(*skia_surface);
    }
    if (!ok) {
      FML_LOG(ERROR) << "Could not make the surface current.";
      return;
    }

    // Clear the current render target (most likely EGLSurface) at the
    // end of this scope.
    fml::ScopedCleanupClosure clear_current_surface([&]() {
      auto [ok, invalidate_api_state] = render_target_->MaybeClearCurrent();
      if (invalidate_api_state) {
        InvalidateApiState(*skia_surface);
      }
      if (!ok) {
        FML_LOG(ERROR) << "Could not clear the current surface.";
      }
    });

    auto canvas = skia_surface->getCanvas();
    if (!canvas) {
      return;
    }

    DlSkCanvasAdapter dl_canvas(canvas);
    bool clear_surface = true;
    for (auto c : flutter_contents_) {
      FML_DCHECK(render_target_->GetRenderTargetSize() ==
                 c->GetRenderSurfaceSize());
      c->Render(dl_canvas, clear_surface);
      clear_surface = false;
    }
    dl_canvas.Flush();
  }
#endif  //  !SLIMPELLER

#ifdef IMPELLER_SUPPORTS_RENDERING
  void RenderFlutterContentsImpeller(bool frame_boundary) {
    auto dl_builder = DisplayListBuilder();
    bool clear_surface = true;
    for (auto c : flutter_contents_) {
      FML_DCHECK(render_target_->GetRenderTargetSize() ==
                 c->GetRenderSurfaceSize());
      c->Render(dl_builder, clear_surface);
      clear_surface = false;
    }
    auto display_list = dl_builder.Build();

    auto* impeller_target = render_target_->GetImpellerRenderTarget();
    auto aiks_context = render_target_->GetAiksContext();
    auto cull_rect =
        impeller::Rect::MakeSize(impeller_target->GetRenderTargetSize());

    impeller::RenderToTarget(aiks_context->GetContentContext(),     //
                             *impeller_target,                      //
                             display_list,                          //
                             cull_rect,                             //
                             /*reset_host_buffer=*/frame_boundary,  //
                             /*is_onscreen=*/false                  //
    );
  }
#endif  // IMPELLER_SUPPORTS_RENDERING

  std::vector<PlatformView> platform_views_;
  std::vector<EmbedderExternalView*> flutter_contents_;
  DlRegion flutter_contents_region_;
  std::unique_ptr<EmbedderRenderTarget> render_target_;
  friend class LayerBuilder;
};

/// A layout builder is responsible for building an optimized list of Layers
/// from a list of `EmbedderExternalView`s. Single EmbedderExternalView contains
/// at most one platform view and at most one layer of Flutter contents
/// ('slice'). LayerBuilder is responsible for producing as few Layers from the
/// list of EmbedderExternalViews as possible while maintaining identical visual
/// result.
///
/// Implements https://flutter.dev/go/optimized-platform-view-layers
class LayerBuilder {
 public:
  using RenderTargetProvider =
      std::function<std::unique_ptr<EmbedderRenderTarget>(
          const DlISize& frame_size)>;

  explicit LayerBuilder(DlISize frame_size) : frame_size_(frame_size) {
    layers_.push_back(Layer());
  }

  /// Adds the platform view and/or flutter contents from the
  /// EmbedderExternalView instance.
  ///
  /// This will try to add the content and platform view to an existing layer
  /// if possible. If not, a new layer will be created.
  void AddExternalView(EmbedderExternalView* view) {
    if (view->HasPlatformView()) {
      PlatformView platform_view(view);
      AddPlatformView(platform_view);
    }
    if (view->HasEngineRenderedContents()) {
      AddFlutterContents(view);
    }
  }

  /// Prepares the render targets for all layers that have Flutter contents.
  void PrepareBackingStore(const RenderTargetProvider& target_provider) {
    for (auto& layer : layers_) {
      if (layer.has_flutter_contents()) {
        layer.SetRenderTarget(target_provider(frame_size_));
      }
    }
  }

  /// Renders all layers with Flutter contents to their respective render
  /// targets.
  void Render() {
    // Find the last layer that has Flutter contents.  The frame boundary flag
    // will be set for this layer.
    auto last_flutter_layer_rev_iter =
        std::find_if(layers_.rbegin(), layers_.rend(),
                     [](const Layer& l) { return l.has_flutter_contents(); });
    if (last_flutter_layer_rev_iter == layers_.rend()) {
      return;
    }
    auto last_flutter_layer_iter = last_flutter_layer_rev_iter.base() - 1;

    for (auto iter = layers_.begin(); iter != layers_.end(); iter++) {
      bool frame_boundary = iter == last_flutter_layer_iter;
      if (iter->has_flutter_contents()) {
        iter->RenderFlutterContents(frame_boundary);
      }
    }
  }

  /// Populates EmbedderLayers from layer builder's layers.
  void PushLayers(EmbedderLayers& layers) {
    for (auto& layer : layers_) {
      for (auto& view : layer.platform_views()) {
        auto platform_view_id = view.view_identifier.platform_view_id;
        if (platform_view_id.has_value()) {
          layers.PushPlatformViewLayer(platform_view_id.value(), *view.params);
        }
      }
      if (layer.render_target() != nullptr) {
        layers.PushBackingStoreLayer(layer.render_target()->GetBackingStore(),
                                     layer.coverage());
      }
    }
  }

  /// Removes the render targets from layers and returns them for collection.
  std::vector<std::unique_ptr<EmbedderRenderTarget>>
  ClearAndCollectRenderTargets() {
    std::vector<std::unique_ptr<EmbedderRenderTarget>> result;
    for (auto& layer : layers_) {
      if (layer.render_target() != nullptr) {
        result.push_back(std::move(layer.render_target_));
      }
    }
    layers_.clear();
    return result;
  }

 private:
  void AddPlatformView(PlatformView view) {
    GetLayerForPlatformView(view).AddPlatformView(view);
  }

  void AddFlutterContents(EmbedderExternalView* contents) {
    FML_DCHECK(contents->HasEngineRenderedContents());

    DlRegion region = contents->GetDlRegion();
    GetLayerForFlutterContentsRegion(region).AddFlutterContents(contents,
                                                                region);
  }

  /// Returns the deepest layer to which the platform view can be added. That
  /// would be (whichever comes first):
  /// - First layer from back that has platform view that intersects with this
  ///   view
  /// - Very last layer from back that has surface that doesn't intersect with
  ///   this. That is because layer content renders on top of the platform view.
  Layer& GetLayerForPlatformView(PlatformView view) {
    for (auto iter = layers_.rbegin(); iter != layers_.rend(); ++iter) {
      // This layer has surface that intersects with this view. That means we
      // went one too far and need the layer before this.
      if (iter->IntersectsFlutterContents(view.clipped_frame)) {
        if (iter == layers_.rbegin()) {
          layers_.emplace_back();
          return layers_.back();
        } else {
          --iter;
          return *iter;
        }
      }
      if (iter->IntersectsPlatformView(view.clipped_frame)) {
        return *iter;
      }
    }
    return layers_.front();
  }

  /// Finds layer to which the Flutter content can be added. That would
  /// be first layer from back that has any intersection with this region.
  Layer& GetLayerForFlutterContentsRegion(const DlRegion& region) {
    for (auto iter = layers_.rbegin(); iter != layers_.rend(); ++iter) {
      if (iter->IntersectsPlatformView(region) ||
          iter->IntersectsFlutterContents(region)) {
        return *iter;
      }
    }
    return layers_.front();
  }

  std::vector<Layer> layers_;
  DlISize frame_size_;
};

};  // namespace

void EmbedderExternalViewEmbedder::SubmitFlutterView(
    int64_t flutter_view_id,
    GrDirectContext* context,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    std::unique_ptr<SurfaceFrame> frame) {
  if (flutter_view_id < kFlutterImplicitViewId &&
      pending_denial_render_target_) {
    if (composition_order_.size() > 1u) {
      // Cuts were registered: compose slices and cuts onto the scanout target.
      SubmitDenialLayered(flutter_view_id, aiks_context, std::move(frame));
      return;
    }
    if (denial_output_mode_ == DenialOutputMode::kLayeredBuild) {
      // Nothing to layer: draw this tree directly until it changes.
      denial_slices_[flutter_view_id].unlayered_tree = denial_frame_tree_;
    }
    if (auto* audit = DenialLayeredAuditFrame(flutter_view_id)) {
      if (denial_output_mode_ == DenialOutputMode::kLayeredBuild) {
        ++audit->single_slice_frames;
      } else {
        ++audit->direct_frames;
      }
    }
    auto root = pending_views_.find(kRootViewIdentifier);
    FML_DCHECK(root != pending_views_.end());

    const auto& buffer_damage = frame->submit_info().buffer_damage;
    const bool full_repaint =
        !buffer_damage.has_value() ||
        (!buffer_damage->isEmpty() && buffer_damage->isSimple() &&
         buffer_damage->bounds() == DlIRect::MakeSize(pending_frame_size_));
    if (!buffer_damage.has_value() || !buffer_damage->isEmpty()) {
      if (!root->second->Render(*pending_denial_render_target_, full_repaint,
                                full_repaint)) {
        pending_denial_render_target_.reset();
        frame->Submit();
        return;
      }
    }

    const auto presentation_time_optional =
        frame->submit_info().presentation_time;
    const uint64_t presentation_time =
        presentation_time_optional.has_value()
            ? presentation_time_optional->ToEpochDelta().ToNanoseconds()
            : 0;
    EmbedderLayers presented_layers(
        pending_frame_size_, pending_device_pixel_ratio_,
        pending_surface_transformation_, presentation_time);
    // The target keeps its previous pixels outside the damage, so all of it
    // is painted content; frame and buffer damage travel with the present.
    presented_layers.PushBackingStoreLayer(
        pending_denial_render_target_->GetBackingStore(),
        {DlIRect::MakeSize(pending_frame_size_)});
    presented_layers.InvokePresentCallback(flutter_view_id, present_callback_);

    // The embedder owns the FBO allocation. Releasing this temporary wrapper
    // only ends Flutter's borrow; the root frame then delivers the exact frame
    // and buffer damage through present-with-info.
    pending_denial_render_target_.reset();
    frame->Submit();
    return;
  }

  // The unordered_map render_target_cache creates a new entry if the view ID is
  // unrecognized.
  EmbedderRenderTargetCache& render_target_cache =
      render_target_caches_[flutter_view_id];
  DlRect _rect = DlRect::MakeSize(pending_frame_size_)
                     .TransformAndClipBounds(pending_surface_transformation_);

  LayerBuilder builder(DlIRect::RoundOut(_rect).GetSize());

  for (auto view_id : composition_order_) {
    auto& view = pending_views_[view_id];
    builder.AddExternalView(view.get());
  }

  builder.PrepareBackingStore([&](const DlISize& frame_size) {
    if (!avoid_backing_store_cache_) {
      std::unique_ptr<EmbedderRenderTarget> target =
          render_target_cache.GetRenderTarget(
              EmbedderExternalView::RenderTargetDescriptor(frame_size));
      if (target != nullptr) {
        return target;
      }
    }
    auto config = MakeBackingStoreConfig(flutter_view_id, frame_size);
    return create_render_target_callback_(context, aiks_context, config);
  });

  // This is where unused render targets will be collected. Control may flow
  // to the embedder. Here, the embedder has the opportunity to trample on the
  // OpenGL context.
  //
  // For optimum performance, we should tell the render target cache to clear
  // its unused entries before allocating new ones. This collection step
  // before allocating new render targets ameliorates peak memory usage within
  // the frame. But, this causes an issue in a known internal embedder. To
  // work around this issue while that embedder migrates, collection of render
  // targets is deferred after the presentation.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  auto deferred_cleanup_render_targets =
      render_target_cache.ClearAllRenderTargetsInCache();

#if !SLIMPELLER
  // The OpenGL context could have been trampled by the embedder at this point
  // as it attempted to collect old render targets and create new ones. Tell
  // Skia to not rely on existing bindings.
  if (context) {
    context->resetContext(kAll_GrBackendState);
  }
#endif  //  !SLIMPELLER

  builder.Render();

#if !SLIMPELLER
  // We are going to be transferring control back over to the embedder there
  // the context may be trampled upon again. Flush all operations to the
  // underlying rendering API.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  if (context) {
    context->flushAndSubmit();
  }
#endif  //  !SLIMPELLER

  {
    auto presentation_time_optional = frame->submit_info().presentation_time;
    uint64_t presentation_time =
        presentation_time_optional.has_value()
            ? presentation_time_optional->ToEpochDelta().ToNanoseconds()
            : 0;

    // Submit the scribbled layer to the embedder for presentation.
    //
    // @warning: Embedder may trample on our OpenGL context here.
    EmbedderLayers presented_layers(
        pending_frame_size_, pending_device_pixel_ratio_,
        pending_surface_transformation_, presentation_time);

    builder.PushLayers(presented_layers);

    presented_layers.InvokePresentCallback(flutter_view_id, present_callback_);
  }

  // See why this is necessary in the comment where this collection in
  // realized.
  //
  // @warning: Embedder may trample on our OpenGL context here.
  deferred_cleanup_render_targets.clear();

  auto render_targets = builder.ClearAndCollectRenderTargets();
  for (auto& render_target : render_targets) {
    if (!avoid_backing_store_cache_) {
      render_target_cache.CacheRenderTarget(std::move(render_target));
    }
  }

  frame->Submit();
}

// Denial layered composition ------------------------------------------------

namespace {

// Replays a cut's accumulated transforms, clips and opacity onto the
// composition canvas, exactly as the state stack would have applied them to
// the inline paint. Opacity is folded into the entry's paint. The first
// `applied` mutators are already on the canvas: those of an open material,
// which the cuts it contains share as the bottom of their own stacks.
void DenialReplayMutators(DlCanvas& canvas,
                          const MutatorsStack& mutators,
                          size_t applied,
                          DlPaint& paint) {
  auto it = mutators.Begin() + std::min(applied, mutators.stack_count());
  for (; it != mutators.End(); ++it) {
    const auto& mutator = *it;
    switch (mutator->GetType()) {
      case MutatorType::kClipRect:
        canvas.ClipRect(mutator->GetRect(), DlClipOp::kIntersect, false);
        break;
      case MutatorType::kClipRRect:
        canvas.ClipRoundRect(mutator->GetRRect(), DlClipOp::kIntersect, true);
        break;
      case MutatorType::kClipRSE:
        canvas.ClipRoundSuperellipse(mutator->GetRSE(), DlClipOp::kIntersect,
                                     true);
        break;
      case MutatorType::kClipPath:
        canvas.ClipPath(mutator->GetPath(), DlClipOp::kIntersect, true);
        break;
      case MutatorType::kTransform:
        canvas.Transform(mutator->GetMatrix());
        break;
      case MutatorType::kOpacity:
        paint.setOpacity(paint.getOpacity() * mutator->GetAlphaFloat());
        break;
      case MutatorType::kBackdropFilter:
      case MutatorType::kBackdropClipRect:
      case MutatorType::kBackdropClipRRect:
      case MutatorType::kBackdropClipRSuperellipse:
      case MutatorType::kBackdropClipPath:
        break;
    }
  }
}

#ifdef IMPELLER_SUPPORTS_RENDERING
// Slice storage extents are rounded up so small layout jitter fits.
constexpr int32_t kDenialSliceGranularity = 64;

int32_t DenialRoundSliceExtent(int32_t extent) {
  const int32_t rounded =
      (extent + kDenialSliceGranularity - 1) / kDenialSliceGranularity;
  return std::max(rounded, 1) * kDenialSliceGranularity;
}

// Blitting a slice rect by rect pays per draw; the rects are worth it only
// when few and markedly smaller than the bounds.
constexpr size_t kDenialSliceRectLimit = 32;
constexpr uint64_t kDenialSliceRectAreaPercent = 75;

uint64_t DenialSliceArea(const DlISize& size) {
  return static_cast<uint64_t>(size.width) * static_cast<uint64_t>(size.height);
}

// The parts of `bounds` a slice paints: its region's disjoint rects, or the
// bounds alone when the rects are too many or save too little. Rects must
// stay disjoint, since an overlap would blend its pixels twice.
void DenialSliceRects(const DlRegion& region,
                      const DlIRect& bounds,
                      std::vector<DlIRect>& rects) {
  rects.clear();
  uint64_t area = 0;
  for (const DlIRect& rect : region.getRects()) {
    const auto clipped = rect.Intersection(bounds);
    if (clipped.has_value() && !clipped->IsEmpty()) {
      rects.push_back(*clipped);
      area += DenialSliceArea(clipped->GetSize());
    }
  }
  if (rects.empty() || rects.size() > kDenialSliceRectLimit ||
      area * 100 >
          DenialSliceArea(bounds.GetSize()) * kDenialSliceRectAreaPercent) {
    rects.assign(1, bounds);
  }
}

// Audit diagnostics: captures the nested display list of one
// DrawDisplayList op.
class DenialNestedCapture final : public virtual DlOpReceiver,
                                  private IgnoreAttributeDispatchHelper,
                                  private IgnoreClipDispatchHelper,
                                  private IgnoreTransformDispatchHelper,
                                  private IgnoreDrawDispatchHelper {
 public:
  void drawDisplayList(const sk_sp<DisplayList> display_list,
                       DlScalar opacity) override {
    nested = display_list;
  }
  sk_sp<DisplayList> nested;
};

// Lists the ops of `display_list` whose bounds cover at least `threshold`
// pixels, descending into nested display lists up to `depth` levels.
void DenialDescribeLargeOps(const sk_sp<DisplayList>& display_list,
                            uint64_t threshold,
                            int depth,
                            std::ostringstream& out) {
  const auto rtree = display_list->rtree();
  if (!rtree) {
    out << " [no-rtree ops=" << display_list->op_count(true) << "]";
    return;
  }
  int shown = 0;
  for (int i = 0; i < rtree->leaf_count() && shown < 4; ++i) {
    const DlIRect leaf = DlIRect::RoundOut(rtree->bounds(i));
    if (static_cast<uint64_t>(leaf.GetWidth()) * leaf.GetHeight() < threshold) {
      continue;
    }
    const int id = rtree->id(i);
    const auto type = display_list->GetOpType(id);
    out << " op" << id << ":type" << static_cast<int>(type) << "@"
        << leaf.GetLeft() << "," << leaf.GetTop() << "," << leaf.GetWidth()
        << "x" << leaf.GetHeight();
    ++shown;
    if (type == DisplayListOpType::kDrawDisplayList && depth > 0) {
      DenialNestedCapture capture;
      display_list->Dispatch(capture, id);
      if (capture.nested) {
        out << " {ops=" << capture.nested->op_count(true)
            << " unbounded=" << capture.nested->root_is_unbounded();
        DenialDescribeLargeOps(capture.nested, threshold, depth - 1, out);
        out << " }";
      }
    }
  }
}

// Storage suits a slice when the slice fits and the storage is at most twice
// its area, since every re-render clears the whole storage.
bool DenialStorageSuits(
    const EmbedderExternalViewEmbedder::DenialSliceStorage& storage,
    const DlISize& needed) {
  return storage.image && storage.size.width >= needed.width &&
         storage.size.height >= needed.height &&
         DenialSliceArea(storage.size) <= 2 * DenialSliceArea(needed);
}

// The retained part of a slice: its sampled color texture.
std::optional<EmbedderExternalViewEmbedder::DenialSliceStorage>
DenialCreateSliceStorage(const std::shared_ptr<impeller::Context>& context,
                         const DlISize& size) {
  impeller::TextureDescriptor descriptor;
  descriptor.storage_mode = impeller::StorageMode::kDevicePrivate;
  descriptor.format = context->GetCapabilities()->GetDefaultColorFormat();
  descriptor.size = impeller::ISize(size.width, size.height);
  descriptor.mip_count = 1u;
  descriptor.usage = impeller::TextureUsage::kRenderTarget |
                     impeller::TextureUsage::kShaderRead;
  auto texture = context->GetResourceAllocator()->CreateTexture(descriptor);
  if (!texture) {
    FML_LOG(ERROR) << "Denial slice allocation failed";
    return std::nullopt;
  }
  texture->SetLabel("Denial slice");
  EmbedderExternalViewEmbedder::DenialSliceStorage storage;
  storage.size = size;
  storage.texture = texture;
  storage.image =
      impeller::DlImageImpeller::Make(texture, DlImage::OwningContext::kRaster);
  return storage;
}

// Makes `storage` suit `needed`: keeps it when it does, otherwise takes the
// smallest suitable storage another slice released earlier in this build
// (`pool`), and only then allocates exactly `needed`. Storage it replaces
// goes to `pool` for the build's later slices; the build frees the rest.
bool DenialPrepareSliceStorage(
    const std::shared_ptr<impeller::Context>& context,
    std::vector<EmbedderExternalViewEmbedder::DenialSliceStorage>& pool,
    EmbedderExternalViewEmbedder::DenialSliceStorage& storage,
    const DlISize& needed,
    uint64_t& allocations,
    uint64_t& pool_reuses) {
  if (DenialStorageSuits(storage, needed)) {
    return true;
  }
  auto best = pool.end();
  for (auto it = pool.begin(); it != pool.end(); ++it) {
    if (DenialStorageSuits(*it, needed) &&
        (best == pool.end() ||
         DenialSliceArea(it->size) < DenialSliceArea(best->size))) {
      best = it;
    }
  }
  if (best != pool.end()) {
    std::swap(storage, *best);
    if (!best->image) {
      pool.erase(best);
    }
    ++pool_reuses;
    return true;
  }
  auto created = DenialCreateSliceStorage(context, needed);
  if (!created.has_value()) {
    return false;
  }
  ++allocations;
  if (storage.image) {
    pool.push_back(std::move(storage));
  }
  storage = std::move(created.value());
  return true;
}

// A rect of the frame's top-left space in target pixels. Output views flip
// into the backing store's bottom-left origin (see PrepareFlutterView).
DlIRect DenialTargetRect(const DlIRect& rect, const DlMatrix& transformation) {
  return DlIRect::RoundOut(
      DlRect::Make(rect).TransformAndClipBounds(transformation));
}

// Renders a slice as the output's root pass would draw it: with the output's
// sample count (`msaa`), so layered and direct frames match. The content
// context's render target cache ignores existing textures and hands out
// pooled ones, so slice targets are built directly on the allocator around
// the retained texture. Depth/stencil and MSAA color are transient: the
// allocator creates them for this render and they go with the target.
bool DenialRenderSlice(
    impeller::ContentContext& content_context,
    const std::shared_ptr<impeller::Context>& context,
    const EmbedderExternalViewEmbedder::DenialSliceStorage& storage,
    bool msaa,
    const DlIRect& bounds,
    const DlISize& needed,
    const DlMatrix& transformation,
    const sk_sp<DisplayList>& display_list) {
  const impeller::ISize texture_size(storage.size.width, storage.size.height);
  impeller::RenderTargetAllocator allocator(context->GetResourceAllocator());
  impeller::RenderTarget target;
  if (msaa) {
    target = allocator.CreateOffscreenMSAA(
        *context, texture_size, /*mip_count=*/1, "Denial slice",
        impeller::RenderTarget::AttachmentConfigMSAA{
            .storage_mode = impeller::StorageMode::kDeviceTransient,
            .resolve_storage_mode = impeller::StorageMode::kDevicePrivate,
            .load_action = impeller::LoadAction::kClear,
            .store_action = impeller::StoreAction::kMultisampleResolve,
            .clear_color = impeller::Color::BlackTransparent()},
        impeller::RenderTarget::kDefaultStencilAttachmentConfig,
        /*existing_color_msaa_texture=*/nullptr, storage.texture);
  } else {
    target = allocator.CreateOffscreen(
        *context, texture_size, /*mip_count=*/1, "Denial slice",
        impeller::RenderTarget::AttachmentConfig{
            .storage_mode = impeller::StorageMode::kDevicePrivate,
            .load_action = impeller::LoadAction::kClear,
            .store_action = impeller::StoreAction::kStore,
            .clear_color = impeller::Color::BlackTransparent()},
        impeller::RenderTarget::kDefaultStencilAttachmentConfig,
        storage.texture);
  }
  if (!target.IsValid()) {
    FML_LOG(ERROR) << "Denial slice render target is invalid";
    return false;
  }
  // Render in target pixels, as the direct path does. Glyphs snap to whole
  // pixels, and the flip rounds a glyph on a half pixel the other way, so an
  // unflipped slice moved such text by a row against direct frames.
  const DlIRect pixels = DenialTargetRect(bounds, transformation);
  DisplayListBuilder builder(DlRect::MakeSize(DlSize(needed)));
  builder.Translate(-pixels.GetLeft(), -pixels.GetTop());
  builder.Transform(transformation);
  builder.DrawDisplayList(display_list);
  return impeller::RenderToTarget(
      content_context, target, builder.Build(),
      impeller::Rect::MakeSize(impeller::ISize(needed.width, needed.height)),
      /*reset_host_buffer=*/false,
      /*is_onscreen=*/false);
}

// `open_materials` holds the mutator depth of each open material, innermost
// last. A cut inside a material composes into its save layer, where the
// material's mutators are still applied.
void DenialComposeCut(DlCanvas& canvas,
                      const EmbeddedViewParams& params,
                      const DenialCutEntry& entry,
                      impeller::AiksContext* aiks_context,
                      TextureRegistry* registry,
                      std::vector<size_t>& open_materials) {
  if (entry.kind == DenialCutEntry::Kind::kMaterialEnd) {
    if (!open_materials.empty()) {
      open_materials.pop_back();
      canvas.Restore();  // The material's save layer.
      canvas.Restore();  // Its replayed mutators.
    }
    return;
  }
  canvas.Save();
  DlPaint paint;
  DenialReplayMutators(canvas, params.mutatorsStack(),
                       open_materials.empty() ? 0 : open_materials.back(),
                       paint);
  // Read now: a recomposition's damage pass may have invalidated the cache.
  const std::optional<int64_t> backdrop_token =
      entry.backdrop_cache ? std::make_optional(entry.backdrop_cache->token())
                           : entry.backdrop_id;
  switch (entry.kind) {
    case DenialCutEntry::Kind::kWindowSurface: {
      auto texture =
          registry ? registry->GetTexture(entry.texture_id) : nullptr;
      if (texture) {
        Texture::PaintContext ctx{.canvas = &canvas,
                                  .gr_context = nullptr,
                                  .aiks_context = aiks_context,
                                  .paint = &paint};
        texture->PaintWindow(ctx, entry.texture_bounds, entry.texture_transform,
                             *entry.window, backdrop_token, entry.sampling,
                             entry.surface_opacity);
      } else {
        paint.setBlendMode(DlBlendMode::kSrc);
        canvas.SaveLayer(entry.window_bounds, &paint, entry.window.get(),
                         backdrop_token);
        canvas.Restore();
      }
      break;
    }
    case DenialCutEntry::Kind::kTexture: {
      auto texture =
          registry ? registry->GetTexture(entry.texture_id) : nullptr;
      if (texture) {
        Texture::PaintContext ctx{.canvas = &canvas,
                                  .gr_context = nullptr,
                                  .aiks_context = aiks_context,
                                  .paint = &paint};
        texture->Paint(ctx, entry.texture_bounds, entry.freeze, entry.sampling);
      }
      break;
    }
    case DenialCutEntry::Kind::kMaterialBegin: {
      paint.setBlendMode(entry.blend_mode);
      canvas.SaveLayer(entry.bounds, &paint, entry.filter.get(),
                       backdrop_token);
      if (entry.content_clip.has_value()) {
        canvas.ClipRect(*entry.content_clip, DlClipOp::kIntersect, false);
      }
      open_materials.push_back(params.mutatorsStack().stack_count());
      // Closed, with the mutator save, by the matching kMaterialEnd.
      return;
    }
    case DenialCutEntry::Kind::kMaterialEnd:
      break;
  }
  canvas.Restore();
}
#endif  // IMPELLER_SUPPORTS_RENDERING

}  // namespace

void EmbedderExternalViewEmbedder::SubmitDenialLayered(
    int64_t flutter_view_id,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    std::unique_ptr<SurfaceFrame> frame) {
  DenialLayeredAudit* audit = DenialLayeredAuditFrame(flutter_view_id);
  const fml::TimePoint compose_start =
      audit ? fml::TimePoint::Now() : fml::TimePoint();
  if (audit) {
    ++audit->layered_frames;
    uint64_t cuts = 0;
    for (const auto& [cut_id, entry] : denial_cuts_) {
      switch (entry.kind) {
        case DenialCutEntry::Kind::kWindowSurface:
          ++audit->window_cuts;
          ++cuts;
          break;
        case DenialCutEntry::Kind::kMaterialBegin:
          ++audit->material_cuts;
          ++cuts;
          break;
        case DenialCutEntry::Kind::kTexture:
          ++audit->texture_cuts;
          ++cuts;
          break;
        case DenialCutEntry::Kind::kMaterialEnd:
          break;
      }
    }
    audit->max_cuts = std::max(audit->max_cuts, cuts);
  }
  bool composed = false;
#ifdef IMPELLER_SUPPORTS_RENDERING
  if (pending_denial_render_target_->GetImpellerRenderTarget() &&
      aiks_context) {
    auto& content_context = aiks_context->GetContentContext();
    const auto context = aiks_context->GetContext();
    auto& view_slices = denial_slices_[flutter_view_id];
    auto& slices = view_slices.slots;
    auto& entries = view_slices.entries;
    entries.clear();
    const DlIRect frame_rect = DlIRect::MakeSize(pending_frame_size_);
    // Slices stand in for parts of the output's root pass.
    const bool msaa = pending_denial_render_target_->GetImpellerRenderTarget()
                              ->GetColorAttachment(0)
                              .texture->GetTextureDescriptor()
                              .sample_count != impeller::SampleCount::kCount1 &&
                      context->GetCapabilities()->SupportsOffscreenMSAA();
    // Storage that slices of this build released, for its later slices.
    std::vector<DenialSliceStorage> pool;
    // The repaint bounds describe the output's damage clip; slices render
    // in full into their own targets.
    const auto repaint_bounds = content_context.GetFrameRepaintBounds();
    content_context.SetFrameRepaintBounds(std::nullopt);
    size_t slice_index = 0;
    composed = true;
    for (const auto& view_id : composition_order_) {
      auto& view = pending_views_[view_id];
      if (view_id.platform_view_id.has_value()) {
        auto cut = denial_cuts_.find(view_id.platform_view_id.value());
        if (cut != denial_cuts_.end() && view->GetEmbeddedViewParams()) {
          entries.push_back({.slice = std::nullopt,
                             .cut = std::move(cut->second),
                             .params = *view->GetEmbeddedViewParams()});
        }
      }
      if (!view->HasEngineRenderedContents()) {
        continue;
      }
      const auto bounds = view->GetDlRegion().bounds().Intersection(frame_rect);
      if (!bounds.has_value() || bounds->IsEmpty()) {
        continue;
      }
      if (slices.size() <= slice_index) {
        slices.resize(slice_index + 1);
      }
      DenialSlice& slot = slices[slice_index];
      entries.push_back({.slice = slice_index});
      ++slice_index;
      DenialSliceRects(view->GetDlRegion(), *bounds, slot.rects);
      const sk_sp<DisplayList>& display_list = view->GetDisplayList();
      if (audit) {
        // Which ops make a slice's paint region large: a slice blitted by
        // its bounds costs those pixels on every composition.
        const auto& region = view->GetDlRegion();
        uint64_t region_area = 0;
        for (const DlIRect& rect : region.getRects()) {
          region_area += DenialSliceArea(rect.GetSize());
        }
        std::ostringstream large;
        DenialDescribeLargeOps(display_list,
                               DenialSliceArea(pending_frame_size_) / 8,
                               /*depth=*/3, large);
        FML_LOG(IMPORTANT)
            << "Denial layered slice view=" << flutter_view_id
            << " index=" << slice_index - 1 << " after_cut="
            << (entries.size() > 1 &&
                        !entries[entries.size() - 2].slice.has_value()
                    ? static_cast<int>(entries[entries.size() - 2].cut.kind)
                    : -1)
            << " bounds=" << bounds->GetLeft() << "," << bounds->GetTop() << ","
            << bounds->GetWidth() << "x" << bounds->GetHeight()
            << " region_rects=" << region.getRects().size()
            << " region_px=" << region_area
            << " blit_rects=" << slot.rects.size()
            << " ops=" << display_list->op_count(true)
            << " large:" << large.str();
      }
      const bool reuse =
          slot.storage.image && slot.display_list && slot.bounds == *bounds &&
          slot.transformation == pending_surface_transformation_ &&
          slot.display_list->Equals(display_list);
      if (audit) {
        if (reuse) {
          ++audit->slices_reused;
        } else if (!slot.storage.image || !slot.display_list) {
          ++audit->rendered_new;
        } else if (slot.bounds != *bounds) {
          ++audit->rendered_bounds;
        } else {
          ++audit->rendered_content;
        }
      }
      if (reuse) {
        continue;
      }
      const fml::TimePoint render_start =
          audit ? fml::TimePoint::Now() : fml::TimePoint();
      const DlISize needed(DenialRoundSliceExtent(bounds->GetWidth()),
                           DenialRoundSliceExtent(bounds->GetHeight()));
      uint64_t allocations = 0;
      uint64_t pool_reuses = 0;
      // The slot's content is replaced either way; drop it first so a
      // failure never leaves stale content marked reusable.
      slot.display_list.reset();
      if (!DenialPrepareSliceStorage(context, pool, slot.storage, needed,
                                     allocations, pool_reuses) ||
          !DenialRenderSlice(content_context, context, slot.storage, msaa,
                             *bounds, needed, pending_surface_transformation_,
                             display_list)) {
        composed = false;
        break;
      }
      if (audit) {
        audit->slice_render_time =
            audit->slice_render_time + (fml::TimePoint::Now() - render_start);
        audit->rendered_pixels += DenialSliceArea(slot.storage.size);
        audit->rendered_slice_pixels += DenialSliceArea(needed);
        audit->slice_allocations += allocations;
        audit->slice_pool_reuses += pool_reuses;
      }
      slot.display_list = display_list;
      slot.bounds = *bounds;
      slot.transformation = pending_surface_transformation_;
    }
    content_context.SetFrameRepaintBounds(repaint_bounds);
    if (composed) {
      // Slices beyond this frame's last free their storage.
      slices.resize(slice_index);
    }
    if (audit) {
      audit->slices += slice_index;
      audit->max_slices =
          std::max(audit->max_slices, static_cast<uint64_t>(slice_index));
    }
    composed = composed &&
               DenialComposeEntries(view_slices, aiks_context,
                                    frame->submit_info().buffer_damage, audit);
    if (composed) {
      view_slices.tree = denial_frame_tree_;
      view_slices.frame_size = pending_frame_size_;
      view_slices.transformation = pending_surface_transformation_;
    } else {
      entries.clear();
    }
  }
#endif  // IMPELLER_SUPPORTS_RENDERING
  if (audit) {
    // CPU time to render changed slices and encode the composition, before
    // the present callback hands the target to deniald.
    const fml::TimeDelta elapsed = fml::TimePoint::Now() - compose_start;
    audit->compose_time = audit->compose_time + elapsed;
    audit->compose_max = std::max(audit->compose_max, elapsed);
    if (!composed) {
      ++audit->failed_frames;
    }
  }
  DenialPresentLayered(flutter_view_id, composed, std::move(frame));
}

void EmbedderExternalViewEmbedder::DenialRecompose(
    int64_t flutter_view_id,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    std::unique_ptr<SurfaceFrame> frame) {
  DenialLayeredAudit* audit = DenialLayeredAuditFrame(flutter_view_id);
  const fml::TimePoint start = audit ? fml::TimePoint::Now() : fml::TimePoint();
  bool composed = false;
  auto view = denial_slices_.find(flutter_view_id);
#ifdef IMPELLER_SUPPORTS_RENDERING
  if (view != denial_slices_.end() && pending_denial_render_target_) {
    composed = DenialComposeEntries(view->second, aiks_context,
                                    frame->submit_info().buffer_damage, audit);
  }
#endif  // IMPELLER_SUPPORTS_RENDERING
  if (!composed && view != denial_slices_.end()) {
    view->second.tree = nullptr;
    view->second.entries.clear();
  }
  if (audit) {
    ++audit->recomposed_frames;
    const fml::TimeDelta elapsed = fml::TimePoint::Now() - start;
    audit->recompose_time = audit->recompose_time + elapsed;
    audit->recompose_max = std::max(audit->recompose_max, elapsed);
    if (!composed) {
      ++audit->failed_frames;
    }
  }
  DenialPresentLayered(flutter_view_id, composed, std::move(frame));
}

bool EmbedderExternalViewEmbedder::DenialComposeEntries(
    const DenialViewSlices& view,
    const std::shared_ptr<impeller::AiksContext>& aiks_context,
    const std::optional<DlRegion>& buffer_damage,
    DenialLayeredAudit* audit) {
#ifdef IMPELLER_SUPPORTS_RENDERING
  auto* impeller_target =
      pending_denial_render_target_->GetImpellerRenderTarget();
  if (!impeller_target || !aiks_context) {
    return false;
  }
  const DlIRect frame_rect = DlIRect::MakeSize(pending_frame_size_);
  const bool full_repaint =
      !buffer_damage.has_value() ||
      (buffer_damage->isSimple() && !buffer_damage->isEmpty() &&
       buffer_damage->bounds() == frame_rect);
  if (!full_repaint && buffer_damage->isEmpty()) {
    // The target already holds this frame.
    return true;
  }
  if (audit) {
    if (full_repaint) {
      audit->composed_pixels += DenialSliceArea(pending_frame_size_);
    } else {
      for (const DlIRect& rect : buffer_damage->getRects()) {
        audit->composed_pixels += DenialSliceArea(rect.GetSize());
      }
    }
  }
  DisplayListBuilder composition(DlRect::Make(frame_rect));
  composition.Transform(pending_surface_transformation_);
  if (!full_repaint) {
    // As the direct path's partial repaint: keep the target outside the
    // damage and clear the repaired pixels themselves.
    const std::vector<DlIRect> rects = buffer_damage->getRects();
    if (rects.size() == 1u) {
      composition.ClipRect(DlRect::Make(rects.front()), DlClipOp::kIntersect,
                           false);
    } else {
      DlPathBuilder path;
      for (const DlIRect& rect : rects) {
        path.AddRect(DlRect::Make(rect));
      }
      composition.ClipPath(path.TakePath(), DlClipOp::kIntersect, false);
    }
    composition.Clear(DlColor::kTransparent());
  }
  std::vector<size_t> open_materials;
  for (const auto& entry : view.entries) {
    if (!entry.slice.has_value()) {
      DenialComposeCut(composition, entry.params, entry.cut, aiks_context.get(),
                       denial_texture_registry_.get(), open_materials);
      continue;
    }
    const DenialSlice& slot = view.slots[entry.slice.value()];
    // Slices hold target pixels (DenialRenderSlice). Inside an open material
    // the replayed mutators are active, so copy them in target space.
    composition.Save();
    composition.TransformReset();
    const DlIRect slice_target =
        DenialTargetRect(slot.bounds, slot.transformation);
    // Pixels outside the slice's paint region are transparent; skip them.
    for (const DlIRect& rect : slot.rects) {
      if (audit) {
        audit->slice_blit_pixels += DenialSliceArea(rect.GetSize());
      }
      const DlIRect target = DenialTargetRect(rect, slot.transformation);
      composition.DrawImageRect(
          slot.storage.image,
          DlRect::Make(
              target.Shift(-slice_target.GetLeft(), -slice_target.GetTop())),
          DlRect::Make(target), DlImageSampling::kNearestNeighbor, nullptr,
          DlSrcRectConstraint::kStrict);
    }
    composition.Restore();
  }
  for (size_t i = 0; i < open_materials.size(); ++i) {
    composition.Restore();
    composition.Restore();
  }
  auto color0 = impeller_target->GetColorAttachment(0u);
  color0.load_action =
      full_repaint ? impeller::LoadAction::kClear : impeller::LoadAction::kLoad;
  impeller_target->SetColorAttachment(color0, 0u);
  return impeller::RenderToTarget(
      aiks_context->GetContentContext(), *impeller_target, composition.Build(),
      impeller::Rect::MakeSize(impeller_target->GetRenderTargetSize()),
      /*reset_host_buffer=*/true,
      /*is_onscreen=*/false);
#else
  return false;
#endif  // IMPELLER_SUPPORTS_RENDERING
}

void EmbedderExternalViewEmbedder::DenialPresentLayered(
    int64_t flutter_view_id,
    bool composed,
    std::unique_ptr<SurfaceFrame> frame) {
  if (!composed) {
    FML_LOG(ERROR) << "Denial layered composition failed; frame skipped";
    pending_denial_render_target_.reset();
    frame->Submit();
    return;
  }

  const auto presentation_time_optional =
      frame->submit_info().presentation_time;
  const uint64_t presentation_time =
      presentation_time_optional.has_value()
          ? presentation_time_optional->ToEpochDelta().ToNanoseconds()
          : 0;
  EmbedderLayers presented_layers(
      pending_frame_size_, pending_device_pixel_ratio_,
      pending_surface_transformation_, presentation_time);
  presented_layers.PushBackingStoreLayer(
      pending_denial_render_target_->GetBackingStore(),
      {DlIRect::MakeSize(pending_frame_size_)});
  presented_layers.InvokePresentCallback(flutter_view_id, present_callback_);
  pending_denial_render_target_.reset();
  frame->Submit();
}

}  // namespace flutter
