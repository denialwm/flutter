// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/compositor_context.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include "flutter/display_list/geometry/dl_path_builder.h"
#include "flutter/flow/layers/layer_tree.h"

namespace flutter {
namespace {

bool RegionCoversFrame(const DlRegion& region, DlISize frame_size) {
  return region.isSimple() && !region.isEmpty() &&
         region.bounds() == DlIRect::MakeSize(frame_size);
}

void ClipCanvasToRegion(DlCanvas* canvas, const DlRegion& region) {
  if (region.isEmpty()) {
    canvas->ClipRect(DlRect(), DlClipOp::kIntersect, false);
    return;
  }

  std::vector<DlIRect> rects = region.getRects();
  if (rects.size() == 1u) {
    canvas->ClipRect(DlRect::Make(rects.front()), DlClipOp::kIntersect, false);
    return;
  }

  DlPathBuilder builder;
  for (const DlIRect& rect : rects) {
    builder.AddRect(DlRect::Make(rect));
  }
  canvas->ClipPath(builder.TakePath(), DlClipOp::kIntersect, false);
}

double RegionArea(const DlRegion& region) {
  double area = 0.0;
  for (const DlIRect& rect : region.getRects()) {
    area += static_cast<double>(rect.GetWidth()) * rect.GetHeight();
  }
  return area;
}

/// The max ratio of dirty pixels to target pixels for which Impeller attempts
/// a partial repaint. Impeller needs a large resolve texture and a final blit
/// for partial repaint, so small reductions in painted area are not useful.
constexpr float kImpellerRepaintRatio = 0.7f;

bool ShouldPerformImpellerPartialRepaint(const DlRegion& damage,
                                         DlISize layer_tree_size) {
  if (damage.isEmpty()) {
    return true;
  }
  if (RegionCoversFrame(damage, layer_tree_size)) {
    return false;
  }
  const double frame_area =
      static_cast<double>(layer_tree_size.width) * layer_tree_size.height;
  return frame_area > 0.0 &&
         RegionArea(damage) / frame_area <= kImpellerRepaintRatio;
}

}  // namespace

RasterDamagePlan RasterDamagePlan::Make(const std::optional<DlRegion>& damage,
                                        DlISize layer_tree_size,
                                        RasterDamagePolicy damage_policy,
                                        RasterBackend backend) {
  const DlRegion full_region(DlIRect::MakeSize(layer_tree_size));
  const auto full_repaint = [&]() {
    return RasterDamagePlan{std::nullopt, full_region};
  };

  if (damage_policy == RasterDamagePolicy::kFullRepaint ||
      !damage.has_value() || RegionCoversFrame(*damage, layer_tree_size)) {
    return full_repaint();
  }

  if (backend == RasterBackend::kImpeller &&
      !ShouldPerformImpellerPartialRepaint(*damage, layer_tree_size)) {
    return full_repaint();
  }

  if (backend == RasterBackend::kSkiaGanesh && damage->isComplex()) {
    // Ganesh turns a non-rectangular root clip into a path. Its ClipStack then
    // analyzes and applies that path for every draw op, which can cost more CPU
    // than the pixels it avoids. A rectangular conservative superset stays on
    // the scissor path and still limits preroll and fragment work to the
    // narrowest rectangle that contains all repair damage.
    DlRegion rectangular_damage(damage->bounds());
    if (RegionCoversFrame(rectangular_damage, layer_tree_size)) {
      return full_repaint();
    }
    return RasterDamagePlan{rectangular_damage, rectangular_damage};
  }

  return RasterDamagePlan{*damage, *damage};
}

std::optional<DlRegion> FrameDamage::ComputeDamageRegion(
    flutter::LayerTree& layer_tree,
    bool has_raster_cache,
    bool impeller_enabled) {
  if (layer_tree.root_layer()) {
    PaintRegionMap empty_paint_region_map;
    const bool reuse_diff_metadata = prev_layer_tree_ == &layer_tree &&
                                     dirty_texture_ids_ != nullptr &&
                                     layer_tree.has_diff_metadata();
    DiffContext context(layer_tree.frame_size(), layer_tree.paint_region_map(),
                        prev_layer_tree_ ? prev_layer_tree_->paint_region_map()
                                         : empty_paint_region_map,
                        has_raster_cache, impeller_enabled, dirty_texture_ids_);
    context.PushCullRect(DlRect::MakeSize(layer_tree.frame_size()));
    if (reuse_diff_metadata) {
      context.UseCachedReadbackRegions(&layer_tree.readback_regions());
      for (const auto& cache : layer_tree.backdrop_filter_caches()) {
        if (std::any_of(cache.input_texture_ids.begin(),
                        cache.input_texture_ids.end(), [&](int64_t texture_id) {
                          return dirty_texture_ids_->contains(texture_id);
                        })) {
          cache.state->Invalidate();
        }
      }
      for (const int64_t texture_id : *dirty_texture_ids_) {
        const auto& regions = layer_tree.texture_paint_regions();
        auto region =
            std::lower_bound(regions.begin(), regions.end(), texture_id,
                             [](const TexturePaintRegion& region, int64_t id) {
                               return region.texture_id < id;
                             });
        while (region != regions.end() && region->texture_id == texture_id) {
          context.AddDamage(region->paint_region);
          ++region;
        }
      }
    } else {
      context.SetDiffMetadataCache(
          &layer_tree.texture_paint_regions(), &layer_tree.readback_regions(),
          &layer_tree.backdrop_filter_caches(),
          &layer_tree.retained_subtree_diff_metadata(),
          prev_layer_tree_ ? &prev_layer_tree_->retained_subtree_diff_metadata()
                           : nullptr);
      DiffContext::AutoSubtreeRestore subtree(&context);
      const Layer* prev_root_layer = nullptr;
      if (!prev_layer_tree_ ||
          prev_layer_tree_->frame_size() != layer_tree.frame_size()) {
        // If there is no previous layer tree assume the entire frame must be
        // repainted.
        context.MarkSubtreeDirty(DlRect::MakeSize(layer_tree.frame_size()));
      } else {
        prev_root_layer = prev_layer_tree_->root_layer();
      }
      layer_tree.root_layer()->Diff(&context, prev_root_layer);
      auto& texture_regions = layer_tree.texture_paint_regions();
      std::sort(texture_regions.begin(), texture_regions.end(),
                [](const TexturePaintRegion& a, const TexturePaintRegion& b) {
                  return a.texture_id < b.texture_id;
                });
      layer_tree.set_has_diff_metadata(true);
    }

    damage_ = context.ComputeDamage(
        existing_damage_, horizontal_clip_alignment_, vertical_clip_alignment_);
    return damage_->buffer_damage;
  }
  return std::nullopt;
}

void FrameDamage::SetBufferDamage(DlRegion buffer_damage) {
  if (damage_) {
    damage_->buffer_damage = std::move(buffer_damage);
  }
}

CompositorContext::CompositorContext()
    : texture_registry_(std::make_shared<TextureRegistry>()),
      raster_time_(fixed_refresh_rate_updater_),
      ui_time_(fixed_refresh_rate_updater_) {}

CompositorContext::CompositorContext(Stopwatch::RefreshRateUpdater& updater)
    : texture_registry_(std::make_shared<TextureRegistry>()),
      raster_time_(updater),
      ui_time_(updater) {}

CompositorContext::~CompositorContext() = default;

void CompositorContext::BeginFrame(ScopedFrame& frame,
                                   bool enable_instrumentation) {
  if (enable_instrumentation) {
    raster_time_.Start();
  }
}

void CompositorContext::EndFrame(ScopedFrame& frame,
                                 bool enable_instrumentation) {
  if (enable_instrumentation) {
    raster_time_.Stop();
  }
}

std::unique_ptr<CompositorContext::ScopedFrame> CompositorContext::AcquireFrame(
    GrDirectContext* gr_context,
    DlCanvas* canvas,
    ExternalViewEmbedder* view_embedder,
    const DlMatrix& root_surface_transformation,
    bool instrumentation_enabled,
    bool surface_supports_readback,
    fml::RefPtr<fml::RasterThreadMerger>
        raster_thread_merger,  // NOLINT(performance-unnecessary-value-param)
    impeller::AiksContext* aiks_context) {
  return std::make_unique<ScopedFrame>(
      *this, gr_context, canvas, view_embedder, root_surface_transformation,
      instrumentation_enabled, surface_supports_readback, raster_thread_merger,
      aiks_context);
}

CompositorContext::ScopedFrame::ScopedFrame(
    CompositorContext& context,
    GrDirectContext* gr_context,
    DlCanvas* canvas,
    ExternalViewEmbedder* view_embedder,
    const DlMatrix& root_surface_transformation,
    bool instrumentation_enabled,
    bool surface_supports_readback,
    fml::RefPtr<fml::RasterThreadMerger> raster_thread_merger,
    impeller::AiksContext* aiks_context)
    : context_(context),
      gr_context_(gr_context),
      canvas_(canvas),
      aiks_context_(aiks_context),
      view_embedder_(view_embedder),
      root_surface_transformation_(root_surface_transformation),
      instrumentation_enabled_(instrumentation_enabled),
      surface_supports_readback_(surface_supports_readback),
      raster_thread_merger_(std::move(raster_thread_merger)) {
  context_.BeginFrame(*this, instrumentation_enabled_);
}

CompositorContext::ScopedFrame::~ScopedFrame() {
  context_.EndFrame(*this, instrumentation_enabled_);
}

RasterStatus CompositorContext::ScopedFrame::Raster(
    flutter::LayerTree& layer_tree,
    bool ignore_raster_cache,
    FrameDamage* frame_damage,
    RasterDamagePolicy damage_policy) {
  TRACE_EVENT0("flutter", "CompositorContext::ScopedFrame::Raster");

  std::optional<DlRegion> clip_region;
  if (frame_damage) {
    clip_region = frame_damage->ComputeDamageRegion(
        layer_tree, !ignore_raster_cache, !gr_context_);

    const RasterBackend backend =
        aiks_context_ ? RasterBackend::kImpeller
                      : (gr_context_ ? RasterBackend::kSkiaGanesh
                                     : RasterBackend::kSkiaSoftware);
    RasterDamagePlan plan = RasterDamagePlan::Make(
        clip_region, layer_tree.frame_size(), damage_policy, backend);
    clip_region = std::move(plan.repaint_region);
    frame_damage->SetBufferDamage(std::move(plan.buffer_damage));
  }

  const DlRect preroll_cull =
      clip_region ? DlRect::Make(clip_region->bounds()) : kGiantRect;
  bool root_needs_readback =
      layer_tree.Preroll(*this, ignore_raster_cache, preroll_cull);
  bool needs_save_layer = root_needs_readback && !surface_supports_readback();
  PostPrerollResult post_preroll_result = PostPrerollResult::kSuccess;
  if (view_embedder_ && raster_thread_merger_) {
    post_preroll_result =
        view_embedder_->PostPrerollAction(raster_thread_merger_);
  }

  if (post_preroll_result == PostPrerollResult::kResubmitFrame) {
    return RasterStatus::kResubmit;
  }
  if (post_preroll_result == PostPrerollResult::kSkipAndRetryFrame) {
    return RasterStatus::kSkipAndRetry;
  }

  if (aiks_context_) {
    PaintLayerTreeImpeller(layer_tree, clip_region, ignore_raster_cache);
  } else {
    PaintLayerTreeSkia(layer_tree, clip_region, needs_save_layer,
                       ignore_raster_cache);
  }
  return RasterStatus::kSuccess;
}

void CompositorContext::ScopedFrame::PaintLayerTreeSkia(
    flutter::LayerTree& layer_tree,
    const std::optional<DlRegion>& clip_region,
    bool needs_save_layer,
    bool ignore_raster_cache) {
  DlAutoCanvasRestore restore(canvas(), clip_region.has_value());

  if (canvas()) {
    if (clip_region) {
      ClipCanvasToRegion(canvas(), *clip_region);
    }

    if (needs_save_layer) {
      TRACE_EVENT0("flutter", "Canvas::saveLayer");
      DlRect bounds = DlRect::MakeSize(layer_tree.frame_size());
      DlPaint paint;
      paint.setBlendMode(DlBlendMode::kSrc);
      canvas()->SaveLayer(bounds, &paint);
    }
    canvas()->Clear(DlColor::kTransparent());
  }

  // The canvas()->Restore() is taken care of by the DlAutoCanvasRestore
  layer_tree.Paint(*this, ignore_raster_cache);
}

void CompositorContext::ScopedFrame::PaintLayerTreeImpeller(
    flutter::LayerTree& layer_tree,
    const std::optional<DlRegion>& clip_region,
    bool ignore_raster_cache) {
  DlAutoCanvasRestore restore(canvas(), clip_region.has_value());

  if (canvas()) {
    if (clip_region) {
      ClipCanvasToRegion(canvas(), *clip_region);
      // Partial repaint preserves the target outside the clip. Clear the
      // repaired pixels themselves so transparent content removed by this
      // frame cannot survive from an older buffer generation.
      canvas()->Clear(DlColor::kTransparent());
    }
  }

  // The canvas()->Restore() is taken care of by the DlAutoCanvasRestore
  layer_tree.Paint(*this, ignore_raster_cache);
}

void CompositorContext::OnGrContextCreated() {
  texture_registry_->OnGrContextCreated();
#if !SLIMPELLER
  raster_cache_.Clear();
#endif  //  !SLIMPELLER
}

void CompositorContext::OnGrContextDestroyed() {
  texture_registry_->OnGrContextDestroyed();
#if !SLIMPELLER
  raster_cache_.Clear();
#endif  //  !SLIMPELLER
}

}  // namespace flutter
