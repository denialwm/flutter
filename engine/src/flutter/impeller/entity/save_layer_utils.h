// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_SAVE_LAYER_UTILS_H_
#define FLUTTER_IMPELLER_ENTITY_SAVE_LAYER_UTILS_H_

#include <memory>
#include <optional>

#include "impeller/entity/contents/filters/filter_contents.h"
#include "impeller/geometry/color.h"
#include "impeller/geometry/rect.h"

namespace impeller {

/// The semantic facts required to prove that a backdrop saveLayer can be
/// evaluated directly in its parent color target.
///
/// This deliberately contains no backend resources. It is a logical-plan
/// predicate: allocation and pass scheduling happen only after it succeeds.
struct BackdropLayerDirectPlanInputs {
  bool has_backdrop_filter = false;
  bool content_is_single_sample_compatible = false;
  bool content_bounds_are_contained = false;
  bool is_root_pass = false;
  BlendMode restore_blend_mode = BlendMode::kSrcOver;
  bool restore_is_opaque = false;
  bool restore_has_effects = false;
  bool shares_backdrop_input = false;
  Scalar inherited_opacity = 1.0f;
};

enum class BackdropLayerDirectRejection : uint32_t {
  kMissingBackdropFilter = 1u << 0u,
  kContentNeedsMultisampling = 1u << 1u,
  kContentBoundsUncontained = 1u << 2u,
  kNestedPass = 1u << 3u,
  kRestoreBlendMode = 1u << 4u,
  kRestoreNotOpaque = 1u << 5u,
  kRestoreHasEffects = 1u << 6u,
  kSharedBackdropInput = 1u << 7u,
  kInheritedOpacity = 1u << 8u,
};

/// Returns a bitset explaining every failed direct-plan predicate. A zero
/// result means the rewrite is accepted.
uint32_t GetBackdropLayerDirectRejections(
    const BackdropLayerDirectPlanInputs& inputs);

/// Returns true when the following saveLayer expression is an exact rewrite:
///
///   layer = backdrop; layer = children over layer; parent = layer (kSrc)
///
/// as:
///
///   parent = backdrop (kSrc); parent = children over parent
bool CanRenderBackdropLayerDirectly(
    const BackdropLayerDirectPlanInputs& inputs);

/// @brief Compute the coverage of a subpass in the global coordinate space.
///
/// @param content_coverage the computed coverage of the contents of the save
///                         layer. This value may be empty if the save layer has
///                         no contents, or  Rect::Maximum if the contents are
///                         unbounded (like a destructive blend).
///
/// @param effect_transform The CTM of the subpass.
/// @param coverage_limit   The current clip coverage. This is used to bound the
///                         subpass size.
/// @param image_filter     A subpass image filter, or nullptr.
/// @param flood_output_coverage Whether the coverage should be flooded to clip
/// coverage regardless of input coverage. This should be set to true when the
/// restore Paint has a destructive blend mode.
/// @param flood_input_coverage  Whther the content coverage should be flooded.
/// This should be set to true if the paint has a backdrop filter or if there is
/// a transparent black effecting color filter.
///
/// The coverage computation expects `content_coverage` to be in the child
/// coordinate space. `effect_transform` is used to transform this back into the
/// global coordinate space. A return value of std::nullopt indicates that the
/// coverage is empty or otherwise does not intersect with the parent coverage
/// limit and should be discarded.
std::optional<Rect> ComputeSaveLayerCoverage(
    const Rect& content_coverage,
    const Matrix& effect_transform,
    const Rect& coverage_limit,
    const std::shared_ptr<FilterContents>& image_filter,
    bool flood_output_coverage = false,
    bool flood_input_coverage = false);

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_SAVE_LAYER_UTILS_H_
