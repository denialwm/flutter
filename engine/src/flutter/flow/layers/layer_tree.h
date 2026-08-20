// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_LAYERS_LAYER_TREE_H_
#define FLUTTER_FLOW_LAYERS_LAYER_TREE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_set>

#include "flutter/common/graphics/texture.h"
#include "flutter/flow/compositor_context.h"
#include "flutter/flow/layers/layer.h"
#include "flutter/flow/raster_cache.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/time/time_delta.h"

class GrDirectContext;

namespace flutter {

class LayerTree {
 public:
  LayerTree(const std::shared_ptr<Layer>& root_layer,
            const DlISize& frame_size);

  // Perform a preroll pass on the tree and return information about
  // the tree that affects rendering this frame.
  //
  // Returns:
  // - a boolean indicating whether or not the top level of the
  //   layer tree performs any operations that require readback
  //   from the root surface.
  bool Preroll(CompositorContext::ScopedFrame& frame,
               bool ignore_raster_cache = false,
               DlRect cull_rect = kGiantRect);

#if !SLIMPELLER
  static void TryToRasterCache(
      const std::vector<RasterCacheItem*>& raster_cached_entries,
      const PaintContext* paint_context,
      bool ignore_raster_cache = false);
#endif  //  !SLIMPELLER

  void Paint(CompositorContext::ScopedFrame& frame,
             bool ignore_raster_cache = false) const;

  sk_sp<DisplayList> Flatten(
      const DlRect& bounds,
      const std::shared_ptr<TextureRegistry>& texture_registry = nullptr,
      GrDirectContext* gr_context = nullptr);

  Layer* root_layer() const { return root_layer_.get(); }
  const std::shared_ptr<Layer>& root_layer_shared() const {
    return root_layer_;
  }
  const DlISize& frame_size() const { return frame_size_; }

  const PaintRegionMap& paint_region_map() const { return paint_region_map_; }
  PaintRegionMap& paint_region_map() { return paint_region_map_; }

  const TexturePaintRegionList& texture_paint_regions() const {
    return texture_paint_regions_;
  }
  TexturePaintRegionList& texture_paint_regions() {
    return texture_paint_regions_;
  }

  const ReadbackRegionList& readback_regions() const {
    return readback_regions_;
  }
  ReadbackRegionList& readback_regions() { return readback_regions_; }

  const BackdropFilterCacheMetadataList& backdrop_filter_caches() const {
    return backdrop_filter_caches_;
  }
  BackdropFilterCacheMetadataList& backdrop_filter_caches() {
    return backdrop_filter_caches_;
  }

  const RetainedSubtreeDiffMetadataMap& retained_subtree_diff_metadata() const {
    return retained_subtree_diff_metadata_;
  }
  RetainedSubtreeDiffMetadataMap& retained_subtree_diff_metadata() {
    return retained_subtree_diff_metadata_;
  }

  bool has_diff_metadata() const { return has_diff_metadata_; }
  void set_has_diff_metadata(bool value) { has_diff_metadata_ = value; }

 private:
  std::shared_ptr<Layer> root_layer_;
  DlISize frame_size_;  // Physical pixels.

  PaintRegionMap paint_region_map_;
  TexturePaintRegionList texture_paint_regions_;
  ReadbackRegionList readback_regions_;
  BackdropFilterCacheMetadataList backdrop_filter_caches_;
  RetainedSubtreeDiffMetadataMap retained_subtree_diff_metadata_;
  bool has_diff_metadata_ = false;

  std::vector<RasterCacheItem*> raster_cache_items_;

  FML_DISALLOW_COPY_AND_ASSIGN(LayerTree);
};

// The information to draw a layer tree to a specified view.
struct LayerTreeTask {
 public:
  LayerTreeTask(int64_t view_id,
                std::unique_ptr<LayerTree> layer_tree,
                float device_pixel_ratio)
      : view_id(view_id),
        layer_tree(std::move(layer_tree)),
        device_pixel_ratio(device_pixel_ratio) {}

  /// The target view to draw to.
  int64_t view_id;
  /// The target layer tree to be drawn.
  std::unique_ptr<LayerTree> layer_tree;
  /// The pixel ratio of the target view.
  float device_pixel_ratio;
  /// Whether this task was taken from Rasterizer's last-successful cache for
  /// an autonomous external-texture redraw. While such a task is in flight it
  /// is also the previous tree against which frame damage must be computed.
  bool is_reused_layer_tree = false;
  /// Texture IDs that requested this autonomous redraw. A missing value keeps
  /// Flutter's conservative behavior and damages every TextureLayer.
  std::optional<std::unordered_set<int64_t>> dirty_texture_ids;
  /// Denial's physical-output configuration generation for a synthetic
  /// render-view task. The implicit Dart view leaves this unset.
  std::optional<uint64_t> render_output_configuration_generation;

 private:
  FML_DISALLOW_COPY_AND_ASSIGN(LayerTreeTask);
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_LAYERS_LAYER_TREE_H_
