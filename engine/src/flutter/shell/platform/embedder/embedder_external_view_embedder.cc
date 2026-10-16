// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_view_embedder.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>

#include "flutter/common/constants.h"
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

static bool DenialLayeredCompositionRequested() {
  const char* value = std::getenv("DENIAL_LAYERED_COMPOSITION");
  return value != nullptr && value[0] == '1' && value[1] == '\0';
}

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

EmbedderExternalViewEmbedder::EmbedderExternalViewEmbedder(
    bool avoid_backing_store_cache,
    const CreateRenderTargetCallback& create_render_target_callback,
    const PresentCallback& present_callback)
    : avoid_backing_store_cache_(avoid_backing_store_cache),
      create_render_target_callback_(create_render_target_callback),
      present_callback_(present_callback),
      denial_layered_enabled_(DenialLayeredCompositionRequested()),
      denial_layered_audit_enabled_(denial_layered_enabled_ &&
                                    DenialRenderAuditRequested()) {
  if (denial_layered_enabled_) {
    FML_LOG(IMPORTANT) << "Denial layered composition enabled for output views"
                       << (denial_layered_audit_enabled_ ? " (audited)" : "");
  }
  FML_DCHECK(create_render_target_callback_);
  FML_DCHECK(present_callback_);
}

bool EmbedderExternalViewEmbedder::DenialLayeredComposition() const {
  return denial_layered_enabled_ &&
         pending_flutter_view_id_ < kFlutterImplicitViewId;
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
  // to the next one. Retained and pooled storage are gauges at its end.
  uint64_t retained_slices = 0;
  uint64_t retained_pixels = 0;
  uint64_t pooled_slices = 0;
  uint64_t pooled_pixels = 0;
  if (auto slices = denial_slices_.find(flutter_view_id);
      slices != denial_slices_.end()) {
    const auto area = [](const DenialSliceStorage& storage) {
      return static_cast<uint64_t>(storage.size.width) *
             static_cast<uint64_t>(storage.size.height);
    };
    for (const auto& slot : slices->second.slots) {
      if (slot.storage.image) {
        ++retained_slices;
        retained_pixels += area(slot.storage);
      }
    }
    for (const auto& storage : slices->second.pool) {
      ++pooled_slices;
      pooled_pixels += area(storage);
    }
  }
  const uint64_t rendered =
      audit.rendered_new + audit.rendered_bounds + audit.rendered_content;
  FML_LOG(IMPORTANT)
      << "Denial layered composition audit view=" << flutter_view_id
      << " interval_ms=" << interval.ToMillisecondsF()
      << " layered_frames=" << audit.layered_frames
      << " single_slice_frames=" << audit.single_slice_frames
      << " failed_frames=" << audit.failed_frames
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
      << " pooled_slices=" << pooled_slices
      << " pooled_slice_pixels=" << pooled_pixels
      << " slice_render_total_us=" << audit.slice_render_time.ToMicrosecondsF()
      << " compose_total_us=" << audit.compose_time.ToMicrosecondsF()
      << " compose_max_us=" << audit.compose_max.ToMicrosecondsF();
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
    if (auto* audit = DenialLayeredAuditFrame(flutter_view_id)) {
      ++audit->single_slice_frames;
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
    presented_layers.PushBackingStoreLayer(
        pending_denial_render_target_->GetBackingStore(),
        root->second->GetDlRegion().getRects());
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
// the inline paint. Opacity is folded into the entry's paint.
void DenialReplayMutators(DlCanvas& canvas,
                          const MutatorsStack& mutators,
                          DlPaint& paint) {
  for (auto it = mutators.Begin(); it != mutators.End(); ++it) {
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
// Spare storage kept per view. The pool holds what disappearing or growing
// slices released; beyond this the smallest entries are freed.
constexpr size_t kDenialSlicePoolLimit = 4;

int32_t DenialRoundSliceExtent(int32_t extent) {
  const int32_t rounded =
      (extent + kDenialSliceGranularity - 1) / kDenialSliceGranularity;
  return std::max(rounded, 1) * kDenialSliceGranularity;
}

uint64_t DenialSliceArea(const DlISize& size) {
  return static_cast<uint64_t>(size.width) * static_cast<uint64_t>(size.height);
}

bool DenialStorageFits(
    const EmbedderExternalViewEmbedder::DenialSliceStorage& storage,
    const DlISize& needed) {
  return storage.image && storage.size.width >= needed.width &&
         storage.size.height >= needed.height;
}

// An extent that has to grow grows by at least half, up to the output's
// extent, so a steadily growing slice reallocates only a few times.
int32_t DenialGrowExtent(int32_t current, int32_t needed, int32_t limit) {
  if (needed <= current) {
    return current;
  }
  const int32_t grown =
      DenialRoundSliceExtent(std::max(needed, current + current / 2));
  return std::max(needed, std::min(grown, limit));
}

void DenialPoolSliceStorage(
    std::vector<EmbedderExternalViewEmbedder::DenialSliceStorage>& pool,
    EmbedderExternalViewEmbedder::DenialSliceStorage&& storage) {
  if (!storage.image) {
    return;
  }
  pool.push_back(std::move(storage));
  storage = {};
  if (pool.size() > kDenialSlicePoolLimit) {
    auto smallest = std::min_element(
        pool.begin(), pool.end(), [](const auto& a, const auto& b) {
          return DenialSliceArea(a.size) < DenialSliceArea(b.size);
        });
    pool.erase(smallest);
  }
}

std::shared_ptr<impeller::Texture> DenialCreateSliceTexture(
    const std::shared_ptr<impeller::Context>& context,
    const DlISize& size,
    impeller::PixelFormat format,
    bool multisample,
    bool sampled,
    const char* label) {
  impeller::TextureDescriptor descriptor;
  descriptor.storage_mode = sampled ? impeller::StorageMode::kDevicePrivate
                                    : impeller::StorageMode::kDeviceTransient;
  descriptor.type = multisample ? impeller::TextureType::kTexture2DMultisample
                                : impeller::TextureType::kTexture2D;
  descriptor.sample_count = multisample ? impeller::SampleCount::kCount4
                                        : impeller::SampleCount::kCount1;
  descriptor.format = format;
  descriptor.size = impeller::ISize(size.width, size.height);
  descriptor.mip_count = 1u;
  descriptor.usage = sampled ? impeller::TextureUsage::kRenderTarget |
                                   impeller::TextureUsage::kShaderRead
                             : impeller::TextureUsage::kRenderTarget;
  auto texture = context->GetResourceAllocator()->CreateTexture(descriptor);
  if (!texture) {
    FML_LOG(ERROR) << label << " allocation failed";
    return nullptr;
  }
  texture->SetLabel(label);
  return texture;
}

std::optional<EmbedderExternalViewEmbedder::DenialSliceStorage>
DenialCreateSliceStorage(const std::shared_ptr<impeller::Context>& context,
                         const DlISize& size) {
  const auto& capabilities = context->GetCapabilities();
  const bool msaa = capabilities->SupportsOffscreenMSAA();
  EmbedderExternalViewEmbedder::DenialSliceStorage storage;
  storage.size = size;
  storage.texture = DenialCreateSliceTexture(
      context, size, capabilities->GetDefaultColorFormat(),
      /*multisample=*/false, /*sampled=*/true, "Denial slice");
  storage.depth_stencil = DenialCreateSliceTexture(
      context, size, capabilities->GetDefaultDepthStencilFormat(), msaa,
      /*sampled=*/false, "Denial slice depth/stencil");
  if (msaa) {
    storage.msaa = DenialCreateSliceTexture(
        context, size, capabilities->GetDefaultColorFormat(),
        /*multisample=*/true, /*sampled=*/false, "Denial slice MSAA");
  }
  if (!storage.texture || !storage.depth_stencil || (msaa && !storage.msaa)) {
    return std::nullopt;
  }
  storage.image = impeller::DlImageImpeller::Make(
      storage.texture, DlImage::OwningContext::kRaster);
  return storage;
}

// Makes `storage` hold at least `needed`: keeps it when the slice fits,
// otherwise swaps in the smallest pooled storage that fits, and only then
// allocates grown storage. Released storage goes to the pool.
bool DenialPrepareSliceStorage(
    const std::shared_ptr<impeller::Context>& context,
    EmbedderExternalViewEmbedder::DenialViewSlices& view,
    EmbedderExternalViewEmbedder::DenialSliceStorage& storage,
    const DlISize& needed,
    const DlISize& limit,
    uint64_t& allocations,
    uint64_t& pool_reuses) {
  if (DenialStorageFits(storage, needed)) {
    return true;
  }
  auto best = view.pool.end();
  for (auto it = view.pool.begin(); it != view.pool.end(); ++it) {
    if (DenialStorageFits(*it, needed) &&
        (best == view.pool.end() ||
         DenialSliceArea(it->size) < DenialSliceArea(best->size))) {
      best = it;
    }
  }
  if (best != view.pool.end()) {
    std::swap(storage, *best);
    if (!best->image) {
      view.pool.erase(best);
    }
    ++pool_reuses;
    return true;
  }
  const DlISize grown(
      DenialGrowExtent(storage.size.width, needed.width, limit.width),
      DenialGrowExtent(storage.size.height, needed.height, limit.height));
  auto created = DenialCreateSliceStorage(context, grown);
  if (!created.has_value()) {
    return false;
  }
  ++allocations;
  DenialPoolSliceStorage(view.pool, std::move(storage));
  storage = std::move(created.value());
  return true;
}

bool DenialRenderSlice(
    impeller::ContentContext& content_context,
    const std::shared_ptr<impeller::Context>& context,
    const EmbedderExternalViewEmbedder::DenialSliceStorage& storage,
    const DlIRect& bounds,
    const DlISize& needed,
    const sk_sp<DisplayList>& display_list) {
  // The content context's render target cache ignores existing textures and
  // hands out pooled ones, so slice targets are built directly on the
  // allocator around the retained slice storage.
  const impeller::ISize texture_size(storage.size.width, storage.size.height);
  impeller::RenderTargetAllocator allocator(context->GetResourceAllocator());
  impeller::RenderTarget target;
  if (storage.msaa) {
    target = allocator.CreateOffscreenMSAA(
        *context, texture_size, /*mip_count=*/1, "Denial slice",
        impeller::RenderTarget::AttachmentConfigMSAA{
            .storage_mode = impeller::StorageMode::kDeviceTransient,
            .resolve_storage_mode = impeller::StorageMode::kDevicePrivate,
            .load_action = impeller::LoadAction::kClear,
            .store_action = impeller::StoreAction::kMultisampleResolve,
            .clear_color = impeller::Color::BlackTransparent()},
        impeller::RenderTarget::kDefaultStencilAttachmentConfig, storage.msaa,
        storage.texture, storage.depth_stencil);
  } else {
    target = allocator.CreateOffscreen(
        *context, texture_size, /*mip_count=*/1, "Denial slice",
        impeller::RenderTarget::AttachmentConfig{
            .storage_mode = impeller::StorageMode::kDevicePrivate,
            .load_action = impeller::LoadAction::kClear,
            .store_action = impeller::StoreAction::kStore,
            .clear_color = impeller::Color::BlackTransparent()},
        impeller::RenderTarget::kDefaultStencilAttachmentConfig,
        storage.texture, storage.depth_stencil);
  }
  if (!target.IsValid()) {
    FML_LOG(ERROR) << "Denial slice render target is invalid";
    return false;
  }
  DisplayListBuilder builder(DlRect::MakeSize(DlSize(needed)));
  builder.Translate(-bounds.GetLeft(), -bounds.GetTop());
  builder.DrawDisplayList(display_list);
  return impeller::RenderToTarget(
      content_context, target, builder.Build(),
      impeller::Rect::MakeSize(impeller::ISize(needed.width, needed.height)),
      /*reset_host_buffer=*/false,
      /*is_onscreen=*/false);
}

void DenialComposeCut(DlCanvas& canvas,
                      const EmbeddedViewParams& params,
                      const DenialCutEntry& entry,
                      impeller::AiksContext* aiks_context,
                      TextureRegistry* registry,
                      int& open_materials) {
  if (entry.kind == DenialCutEntry::Kind::kMaterialEnd) {
    if (open_materials > 0) {
      --open_materials;
      canvas.Restore();  // The material's save layer.
      canvas.Restore();  // Its replayed mutators.
    }
    return;
  }
  canvas.Save();
  DlPaint paint;
  DenialReplayMutators(canvas, params.mutatorsStack(), paint);
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
                             *entry.window, std::nullopt, entry.sampling,
                             entry.surface_opacity);
      } else {
        paint.setBlendMode(DlBlendMode::kSrc);
        canvas.SaveLayer(entry.window_bounds, &paint, entry.window.get());
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
      canvas.SaveLayer(entry.bounds, &paint, entry.filter.get());
      if (entry.content_clip.has_value()) {
        canvas.ClipRect(*entry.content_clip, DlClipOp::kIntersect, false);
      }
      ++open_materials;
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
  auto* impeller_target =
      pending_denial_render_target_->GetImpellerRenderTarget();
  if (impeller_target && aiks_context) {
    auto& content_context = aiks_context->GetContentContext();
    const auto context = aiks_context->GetContext();
    auto& view_slices = denial_slices_[flutter_view_id];
    auto& slices = view_slices.slots;
    const DlIRect frame_rect = DlIRect::MakeSize(pending_frame_size_);
    const DlISize storage_limit(
        DenialRoundSliceExtent(pending_frame_size_.width),
        DenialRoundSliceExtent(pending_frame_size_.height));
    DisplayListBuilder composition(DlRect::Make(frame_rect));
    composition.Transform(pending_surface_transformation_);
    size_t slice_index = 0;
    int open_materials = 0;
    composed = true;
    for (const auto& view_id : composition_order_) {
      auto& view = pending_views_[view_id];
      if (view_id.platform_view_id.has_value()) {
        auto cut = denial_cuts_.find(view_id.platform_view_id.value());
        if (cut != denial_cuts_.end() && view->GetEmbeddedViewParams()) {
          DenialComposeCut(composition, *view->GetEmbeddedViewParams(),
                           cut->second, aiks_context.get(),
                           denial_texture_registry_.get(), open_materials);
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
      DenialSlice& slot = slices[slice_index++];
      const sk_sp<DisplayList>& display_list = view->GetDisplayList();
      const bool reuse = slot.storage.image && slot.display_list &&
                         slot.bounds == *bounds &&
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
      if (!reuse) {
        const fml::TimePoint render_start =
            audit ? fml::TimePoint::Now() : fml::TimePoint();
        const DlISize needed(DenialRoundSliceExtent(bounds->GetWidth()),
                             DenialRoundSliceExtent(bounds->GetHeight()));
        uint64_t allocations = 0;
        uint64_t pool_reuses = 0;
        // The slot's content is replaced either way; drop it first so a
        // failure never leaves stale content marked reusable.
        slot.display_list.reset();
        if (!DenialPrepareSliceStorage(context, view_slices, slot.storage,
                                       needed, storage_limit, allocations,
                                       pool_reuses) ||
            !DenialRenderSlice(content_context, context, slot.storage, *bounds,
                               needed, display_list)) {
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
      }
      // Slices are recorded in the frame's top-left space. Inside an open
      // material the replayed mutators are active, so return to the root
      // transformation for the slice image.
      composition.Save();
      composition.TransformReset();
      composition.Transform(pending_surface_transformation_);
      composition.DrawImageRect(
          slot.storage.image, DlRect::MakeSize(DlSize(bounds->GetSize())),
          DlRect::Make(*bounds), DlImageSampling::kNearestNeighbor, nullptr,
          DlSrcRectConstraint::kStrict);
      composition.Restore();
    }
    if (composed) {
      // Slices beyond this frame's last give their storage to the pool.
      for (size_t i = slice_index; i < slices.size(); ++i) {
        DenialPoolSliceStorage(view_slices.pool, std::move(slices[i].storage));
      }
      slices.resize(slice_index);
    }
    if (audit) {
      audit->slices += slice_index;
      audit->max_slices =
          std::max(audit->max_slices, static_cast<uint64_t>(slice_index));
    }
    while (open_materials-- > 0) {
      composition.Restore();
      composition.Restore();
    }
    if (composed) {
      auto color0 = impeller_target->GetColorAttachment(0u);
      color0.load_action = impeller::LoadAction::kClear;
      impeller_target->SetColorAttachment(color0, 0u);
      composed = impeller::RenderToTarget(
          content_context, *impeller_target, composition.Build(),
          impeller::Rect::MakeSize(impeller_target->GetRenderTargetSize()),
          /*reset_host_buffer=*/true,
          /*is_onscreen=*/false);
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
