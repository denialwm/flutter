// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/backdrop_graph.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "flutter/display_list/geometry/dl_rtree.h"

namespace impeller {

size_t BackdropEpochPlan::GetMaxEpochWidth() const {
  size_t result = 0u;
  for (const auto& epoch : scopes_by_epoch) {
    result = std::max(result, epoch.size());
  }
  return result;
}

BackdropEpochPlan PlanBackdropEpochs(std::vector<BackdropScopeRegion> scopes) {
  BackdropEpochPlan result;
  result.scopes = std::move(scopes);
  const std::vector<BackdropScopeRegion>& planned_scopes = result.scopes;
  if (planned_scopes.empty()) {
    return result;
  }

  const size_t scope_count = planned_scopes.size();
  FML_DCHECK(scope_count <=
             static_cast<size_t>(std::numeric_limits<int>::max()));

  std::vector<Rect> write_regions;
  std::vector<Rect> read_regions;
  std::vector<int> scope_ids;
  write_regions.reserve(scope_count);
  read_regions.reserve(scope_count);
  scope_ids.reserve(scope_count);
  for (size_t i = 0u; i < scope_count; i++) {
    write_regions.push_back(planned_scopes[i].write_region);
    read_regions.push_back(planned_scopes[i].read_region);
    scope_ids.push_back(static_cast<int>(i));
  }

  flutter::DlRTree write_index(write_regions.data(),
                               static_cast<int>(scope_count), scope_ids.data());
  flutter::DlRTree read_index(read_regions.data(),
                              static_cast<int>(scope_count), scope_ids.data());

  result.epoch_for_scope.resize(scope_count, 0u);
  std::vector<uint32_t> seen_generation(scope_count, 0u);
  uint32_t generation = 0u;
  uint32_t epoch_floor = 0u;
  uint32_t max_assigned_epoch = 0u;
  std::vector<int> hits;

  for (size_t later = 0u; later < scope_count; later++) {
    if (++generation == 0u) {
      std::fill(seen_generation.begin(), seen_generation.end(), 0u);
      generation = 1u;
    }

    if (later > 0u && planned_scopes[later].scene_color_generation !=
                          planned_scopes[later - 1u].scene_color_generation) {
      // An arbitrary scene write occurred between these scopes. Until all
      // writes are represented by regions in the graph, conservatively end
      // the current epoch range. Every scope in the new generation starts
      // after every epoch assigned to an older generation.
      epoch_floor = max_assigned_epoch + 1u;
      result.scene_barriers++;
    }

    uint32_t epoch = epoch_floor;
    auto consume_hits = [&](const flutter::DlRTree& index, const Rect& query) {
      hits.clear();
      index.search(query, &hits);
      for (int hit : hits) {
        const int id = index.id(hit);
        if (id < 0 || static_cast<size_t>(id) >= later ||
            seen_generation[id] == generation) {
          continue;
        }
        seen_generation[id] = generation;
        result.dependency_edges++;
        epoch = std::max(epoch, result.epoch_for_scope[id] + 1u);
      }
    };

    // Earlier writes which affect this scope's backdrop value.
    consume_hits(write_index, planned_scopes[later].read_region);
    // Earlier writes which overlap this scope's output and therefore require
    // z-order composition.
    consume_hits(write_index, planned_scopes[later].write_region);
    // A later write which overlaps an earlier backdrop footprint is kept out
    // of the same epoch. This is conservative and permits every epoch to read
    // all of its inputs before issuing any of its writes.
    consume_hits(read_index, planned_scopes[later].write_region);

    result.epoch_for_scope[later] = epoch;
    max_assigned_epoch = std::max(max_assigned_epoch, epoch);
    if (result.scopes_by_epoch.size() <= epoch) {
      result.scopes_by_epoch.resize(epoch + 1u);
    }
    result.scopes_by_epoch[epoch].push_back(static_cast<uint32_t>(later));
  }

  return result;
}

std::optional<uint32_t> BackdropEpochCursor::Claim(
    const BackdropEpochPlan& plan,
    const Rect& write_region,
    const Rect& read_region) {
  if (!valid_ || plan.scopes.size() != plan.epoch_for_scope.size()) {
    valid_ = false;
    return std::nullopt;
  }

  while (next_scope_ < plan.scopes.size()) {
    const size_t candidate = next_scope_++;
    const BackdropScopeRegion& scope = plan.scopes[candidate];
    if (scope.write_region == write_region &&
        scope.read_region == read_region) {
      return plan.epoch_for_scope[candidate];
    }
  }

  // The second pass saw no exact first-pass counterpart. The DisplayList
  // two-pass contract permits this, so optimization must fail closed rather
  // than guessing a correspondence.
  valid_ = false;
  return std::nullopt;
}

void BackdropEpochCursor::Reset() {
  next_scope_ = 0u;
  valid_ = true;
}

}  // namespace impeller
