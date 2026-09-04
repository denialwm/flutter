// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_DIFF_CONTEXT_H_
#define FLUTTER_FLOW_DIFF_CONTEXT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "display_list/geometry/dl_region.h"
#include "display_list/utils/dl_matrix_clip_tracker.h"
#include "flutter/common/backdrop_filter_cache_key.h"
#include "flutter/flow/paint_region.h"
#include "flutter/fml/macros.h"

namespace flutter {

class Layer;

// Represents regions that need to be updated in the front buffer
// (frame_damage) and painted into the selected back buffer (buffer_damage).
struct Damage {
  // This is the damage between current and previous frame.
  // If embedder supports partial update, this is the region that needs to be
  // repainted.
  // Corresponds to "surface damage" from EGL_KHR_partial_update.
  DlRegion frame_damage;

  // Reflects actual change to target framebuffer. This is frame_damage plus
  // damage previously accumulated for the selected target framebuffer.
  // All drawing will be clipped to this region. Knowing the affected area
  // upfront may be useful for tile based GPUs.
  // Corresponds to "buffer damage" from EGL_KHR_partial_update.
  DlRegion buffer_damage;
};

// Layer Unique Id to PaintRegion
using PaintRegionMap = std::unordered_map<uint64_t, PaintRegion>;

// Reusable metadata for autonomous frames that redraw an unchanged layer tree.
// A texture ID may occur in more than one TextureLayer. The list is sorted by
// ID after collection so autonomous frames can search it without pointer-heavy
// tree or hash-map traversal.
struct TexturePaintRegion {
  int64_t texture_id;
  PaintRegion paint_region;
};

using TexturePaintRegionList = std::vector<TexturePaintRegion>;

struct LayerPaintRegion {
  uint64_t layer_id;
  PaintRegion paint_region;
};

using LayerPaintRegionList = std::vector<LayerPaintRegion>;

// Complete metadata needed to preserve one retained texture subtree without
// walking its descendants. Readback subtrees are intentionally excluded: they
// must replay backdrop-filter ordering and dependencies during Diff.
struct RetainedSubtreeDiffMetadata {
  LayerPaintRegionList layer_paint_regions;
  TexturePaintRegionList texture_paint_regions;
};

using RetainedSubtreeDiffMetadataMap =
    std::unordered_map<uint64_t,
                       std::shared_ptr<const RetainedSubtreeDiffMetadata>>;

class BackdropFilterCacheState;

// A successful callback guarantees that this exact snapshot remains available
// to the renderer until the frame has been submitted.
using BackdropSnapshotPin = std::function<bool(int64_t, const DlIRect&)>;

struct ReadbackRegion {
  DlIRect paint_rect;
  DlIRect readback_rect;
  std::shared_ptr<BackdropFilterCacheState> cache_state;
};

using ReadbackRegionList = std::vector<ReadbackRegion>;

// Identifies one semantic version of a backdrop. The stable family identifies
// the filter and its generation advances whenever content below it changes;
// changes inside the filter's own subtree intentionally keep the token stable.
class BackdropFilterCacheState {
 public:
  BackdropFilterCacheState();

  int64_t token() const {
    return MakeBackdropFilterCacheKey(family_, generation_);
  }
  void Invalidate();

 private:
  uint32_t family_;
  uint32_t generation_ = 1u;
};

// Reusable ordering metadata for autonomous external-texture frames. Only
// textures painted before a backdrop filter can invalidate its cached input.
struct BackdropFilterCacheMetadata {
  std::shared_ptr<BackdropFilterCacheState> state;
  std::vector<int64_t> input_texture_ids;
};

using BackdropFilterCacheMetadataList =
    std::vector<BackdropFilterCacheMetadata>;

// Tracks state during tree diffing process and computes resulting damage
class DiffContext {
 public:
  explicit DiffContext(
      DlISize frame_size,
      PaintRegionMap& this_frame_paint_region_map,
      const PaintRegionMap& last_frame_paint_region_map,
      bool has_raster_cache,
      bool impeller_enabled,
      const std::unordered_set<int64_t>* dirty_texture_ids = nullptr);

  // Starts a new subtree.
  void BeginSubtree();

  // Ends current subtree; All modifications to state (transform, cullrect,
  // dirty) will be restored
  void EndSubtree();

  // Creates subtree in current scope and closes it on scope exit
  class AutoSubtreeRestore {
    FML_DISALLOW_COPY_ASSIGN_AND_MOVE(AutoSubtreeRestore);

   public:
    explicit AutoSubtreeRestore(DiffContext* context) : context_(context) {
      context->BeginSubtree();
    }
    ~AutoSubtreeRestore() { context_->EndSubtree(); }

   private:
    DiffContext* context_;
  };

  // Pushes additional transform for current subtree
  void PushTransform(const DlMatrix& transform);

  // Pushes cull rect for current subtree
  bool PushCullRect(const DlRect& clip);

  // Function that adjusts layer bounds (in device coordinates) depending
  // on filter.
  using FilterBoundsAdjustment = std::function<DlRect(DlRect)>;

  // Pushes filter bounds adjustment to current subtree. Every layer in this
  // subtree will have bounds adjusted by this function.
  void PushFilterBoundsAdjustment(const FilterBoundsAdjustment& filter);

  // Instruct DiffContext that current layer will paint with integral transform.
  void WillPaintWithIntegralTransform() { state_.integral_transform = true; }

  // Returns current transform as DlMatrix.
  const DlMatrix& GetMatrix() const;

  const std::optional<DlSize>& denial_render_output_logical_size() const {
    return denial_render_output_logical_size_;
  }

  void set_denial_render_output_logical_size(
      std::optional<DlSize> logical_size) {
    denial_render_output_logical_size_ = logical_size;
  }

  // Return cull rect for current subtree (in local coordinates).
  DlRect GetCullRect() const;

  // Sets the dirty flag on current subtree.
  //
  // previous_paint_region, which should represent region of previous subtree
  // at this level will be added to damage area.
  //
  // Each paint region added to dirty subtree (through AddPaintRegion) is also
  // added to damage.
  void MarkSubtreeDirty(
      const PaintRegion& previous_paint_region = PaintRegion());
  void MarkSubtreeDirty(const DlRect& previous_paint_region);

  bool IsSubtreeDirty() const { return state_.dirty; }

  // Marks that current subtree contains a TextureLayer. This is needed to
  // ensure that we'll Diff the TextureLayer even if inside retained layer.
  void MarkSubtreeHasTextureLayer();

  // Add layer bounds to current paint region; rect is in "local" (layer)
  // coordinates.
  void AddLayerBounds(const DlRect& rect);

  // Add entire paint region of retained layer for current subtree. This can
  // only be used in subtrees that are not dirty, otherwise ancestor transforms
  // or clips may result in different paint region.
  void AddExistingPaintRegion(const PaintRegion& region);

  // The idea of readback region is that if any part of the readback region
  // needs to be repainted, then the whole readback region must be repainted;
  //
  // paint_rect - rectangle where the filter paints contents (in screen
  //              coordinates)
  // readback_rect - rectangle where the filter samples from (in screen
  //                 coordinates)
  void AddReadbackRegion(
      const DlIRect& paint_rect,
      const DlIRect& readback_rect,
      std::shared_ptr<BackdropFilterCacheState> cache_state = nullptr);

  bool BackdropInputIsDirty(const DlIRect& readback_rect) const;

  std::shared_ptr<BackdropFilterCacheState> RegisterBackdropFilterCache(
      std::optional<int64_t> group_id,
      std::shared_ptr<BackdropFilterCacheState> state,
      const DlIRect& readback_rect);

  // Returns the paint region for current subtree; Each rect in paint region is
  // in screen coordinates; Once a layer accumulates the paint regions of its
  // children, this PaintRegion value can be associated with the current layer
  // using DiffContext::SetLayerPaintRegion.
  PaintRegion CurrentSubtreeRegion() const;

  // Computes final damage
  //
  // additional_damage is the previously accumulated frame damage for the
  // current framebuffer. nullopt means that the framebuffer contents are
  // unknown and therefore require a complete repaint. An empty region means
  // that the framebuffer already represents the current front buffer.
  //
  // clip_alignment controls the alignment of every rectangle in the resulting
  // frame and buffer damage.
  Damage ComputeDamage(const std::optional<DlRegion>& additional_damage,
                       int horizontal_clip_alignment = 0,
                       int vertical_clip_alignment = 0,
                       const BackdropSnapshotPin& pin_backdrop = {}) const;

  // Adds the region to current damage. Used for removed layers, where instead
  // of diffing the layer its paint region is direcly added to damage.
  void AddDamage(const PaintRegion& damage);

  // Associates the paint region with specified layer and current layer tree.
  // The paint region can not be stored directly in layer itself, because same
  // retained layer instance can possibly paint in different locations depending
  // on ancestor layers.
  void SetLayerPaintRegion(const Layer* layer, const PaintRegion& region);

  // Retrieves the paint region associated with specified layer and previous
  // frame layer tree.
  PaintRegion GetOldLayerPaintRegion(const Layer* layer) const;

  // Whether or not a raster cache is being used. If so, we must snap
  // all transformations to physical pixels if the layer may be raster
  // cached.
  bool has_raster_cache() const { return has_raster_cache_; }

  bool impeller_enabled() const { return impeller_enabled_; }

  // Returns true when normal diffing must conservatively repaint every
  // texture, or when an autonomous texture frame explicitly marked this ID.
  bool IsTextureDirty(int64_t texture_id) const {
    return dirty_texture_ids_ == nullptr ||
           dirty_texture_ids_->find(texture_id) != dirty_texture_ids_->end();
  }

  // Captures texture paint regions and readback dependencies while diffing a
  // new layer tree. The supplied containers are cleared before use.
  void SetDiffMetadataCache(
      TexturePaintRegionList* texture_regions,
      ReadbackRegionList* readback_regions,
      BackdropFilterCacheMetadataList* backdrop_filter_caches,
      RetainedSubtreeDiffMetadataMap* retained_subtrees,
      const RetainedSubtreeDiffMetadataMap* previous_retained_subtrees);

  // Records the paint region for a TextureLayer in the active metadata cache.
  void CacheTexturePaintRegion(int64_t texture_id,
                               const PaintRegion& paint_region);

  // Reuses all per-layer and external-texture metadata for an unchanged,
  // readback-free retained subtree if none of its textures are dirty.
  bool TryReuseRetainedSubtreeMetadata(const Layer* layer,
                                       const PaintRegion& paint_region);

  // Captures metadata produced while an otherwise reusable texture subtree is
  // diffed for the first time. Nested captures are folded into the outer block
  // to avoid duplicating metadata.
  class AutoRetainedSubtreeMetadataCapture {
    FML_DISALLOW_COPY_ASSIGN_AND_MOVE(AutoRetainedSubtreeMetadataCapture);

   public:
    AutoRetainedSubtreeMetadataCapture(DiffContext* context,
                                       const Layer* layer);
    ~AutoRetainedSubtreeMetadataCapture();

   private:
    DiffContext* context_;
    bool active_;
  };

  // Reuses readback dependencies captured for the exact same LayerTree.
  void UseCachedReadbackRegions(const ReadbackRegionList* readback_regions);

  class Statistics {
   public:
    // Picture replaced by different picture
    void AddNewPicture() { ++new_pictures_; }

    // Picture that would require deep comparison but was considered too complex
    // to serialize and thus was treated as new picture
    void AddPictureTooComplexToCompare() { ++pictures_too_complex_to_compare_; }

    // Picture that has identical instance between frames
    void AddSameInstancePicture() { ++same_instance_pictures_; };

    // Picture that had to be serialized to compare for equality
    void AddDeepComparePicture() { ++deep_compare_pictures_; }

    // Picture that had to be serialized to compare (different instances),
    // but were equal
    void AddDifferentInstanceButEqualPicture() {
      ++different_instance_but_equal_pictures_;
    };

    // Logs the statistics to trace counter
    void LogStatistics();

   private:
    int new_pictures_ = 0;
    int pictures_too_complex_to_compare_ = 0;
    int same_instance_pictures_ = 0;
    int deep_compare_pictures_ = 0;
    int different_instance_but_equal_pictures_ = 0;
  };

  Statistics& statistics() { return statistics_; }

  DlRect MapRect(const DlRect& rect);

 private:
  struct State {
    State();

    bool dirty = false;

    size_t rect_index = 0;

    // In order to replicate paint process closely, DiffContext needs to take
    // into account that some layers are painted with transform translation
    // snapped to integral coordinates.
    //
    // It's not possible to simply snap the transform itself, because culling
    // needs to happen with original (unsnapped) transform, just like it does
    // during paint. This means the integral coordinates must be applied after
    // culling before painting the layer content (either the layer itself, or
    // when starting subtree to paint layer children).
    bool integral_transform = false;

    // Current transform and clip for the layer
    DisplayListMatrixClipState matrix_clip;

    // Whether this subtree has filter bounds adjustment function. If so,
    // it will need to be removed from stack when subtree is closed.
    bool has_filter_bounds_adjustment = false;

    // Whether there is a texture layer in this subtree.
    bool has_texture = false;
  };

  void MakeTransformIntegral(DisplayListMatrixClipState& matrix_clip);

  std::shared_ptr<std::vector<DlRect>> rects_;
  State state_;
  DlISize frame_size_;
  std::vector<State> state_stack_;
  std::vector<FilterBoundsAdjustment> filter_bounds_adjustment_stack_;

  // Applies the filter bounds adjustment stack on provided rect.
  // Rect must be in device coordinates.
  DlRect ApplyFilterBoundsAdjustment(DlRect rect) const;

  DlRegion damage_;

  PaintRegionMap& this_frame_paint_region_map_;
  const PaintRegionMap& last_frame_paint_region_map_;
  bool has_raster_cache_;
  bool impeller_enabled_;
  const std::unordered_set<int64_t>* dirty_texture_ids_;

  // Constant for a Denial physical-output traversal. This lives outside the
  // ordinary subtree state because synthesized source crops set and restore it
  // around the shared Flutter root.
  std::optional<DlSize> denial_render_output_logical_size_;
  TexturePaintRegionList* texture_region_cache_ = nullptr;
  ReadbackRegionList* readback_region_cache_ = nullptr;
  BackdropFilterCacheMetadataList* backdrop_filter_cache_ = nullptr;
  RetainedSubtreeDiffMetadataMap* retained_subtree_cache_ = nullptr;
  const RetainedSubtreeDiffMetadataMap* previous_retained_subtree_cache_ =
      nullptr;
  const ReadbackRegionList* cached_readback_regions_ = nullptr;
  std::unordered_map<int64_t, std::shared_ptr<BackdropFilterCacheState>>
      backdrop_group_states_;

  void AddDamage(const DlRect& rect);

  DlIRect AlignRect(const DlIRect& rect,
                    int horizontal_alignment,
                    int vertical_clip_alignment) const;
  DlRegion AlignRegion(const DlRegion& region,
                       int horizontal_alignment,
                       int vertical_clip_alignment) const;

  struct Readback : ReadbackRegion {
    // Index of rects_ entry that this readback belongs to. Used to
    // determine if subtree has any readback
    size_t position;
  };

  std::vector<Readback> readbacks_;
  Statistics statistics_;

  struct RetainedSubtreeCapture {
    uint64_t layer_id;
    LayerPaintRegionList layer_paint_regions;
    TexturePaintRegionList texture_paint_regions;
  };

  bool BeginRetainedSubtreeMetadataCapture(const Layer* layer);
  void EndRetainedSubtreeMetadataCapture();
  std::optional<RetainedSubtreeCapture> retained_subtree_capture_;
};

}  // namespace flutter

#endif  // FLUTTER_FLOW_DIFF_CONTEXT_H_
