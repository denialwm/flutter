// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_BACKDROP_GRAPH_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_BACKDROP_GRAPH_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "impeller/geometry/rect.h"

namespace impeller {

/// The regions which participate in backdrop ordering for one logical scope.
/// Both rectangles are expressed in global device coordinates.
struct BackdropScopeRegion {
  /// Pixels written when the scope is composited into scene color.
  Rect write_region;

  /// Scene-color pixels needed to produce the scope's filtered backdrop,
  /// including the filter footprint.
  Rect read_region;

  /// Monotonic version of scene color immediately before this scope. Two
  /// scopes with different generations are never placed in the same epoch
  /// unless a later region-sensitive proof replaces this conservative
  /// barrier.
  uint64_t scene_color_generation = 0u;
};

/// A dependency-ordered partition of backdrop scopes. Scopes in one epoch are
/// pairwise independent and may have their filter work recorded or batched
/// together. Epochs themselves retain scene-color order.
struct BackdropEpochPlan {
  std::vector<BackdropScopeRegion> scopes;
  std::vector<uint32_t> epoch_for_scope;
  std::vector<std::vector<uint32_t>> scopes_by_epoch;
  size_t dependency_edges = 0u;
  size_t scene_barriers = 0u;

  size_t GetMaxEpochWidth() const;
};

/// Builds the conservative backdrop dependency DAG and returns its earliest
/// valid epoch partition. Candidate intersections are found through R-trees;
/// the planner does not perform an unconditional all-pairs scan.
BackdropEpochPlan PlanBackdropEpochs(std::vector<BackdropScopeRegion> scopes);

/// Matches second-pass scopes against a first-pass plan without assuming a
/// one-to-one dispatch. A planned scope may be absent because the two passes
/// cull differently. A scope must match exactly; any unplanned or differently
/// covered second-pass scope invalidates the cursor and forces exact
/// standalone rendering for the rest of the frame.
class BackdropEpochCursor {
 public:
  std::optional<uint32_t> Claim(const BackdropEpochPlan& plan,
                                const Rect& write_region,
                                const Rect& read_region);

  void Reset();

 private:
  size_t next_scope_ = 0u;
  bool valid_ = true;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_BACKDROP_GRAPH_H_
