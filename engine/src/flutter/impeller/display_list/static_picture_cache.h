// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_STATIC_PICTURE_CACHE_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_STATIC_PICTURE_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include "flutter/display_list/display_list.h"
#include "flutter/display_list/dl_canvas.h"
#include "flutter/display_list/geometry/dl_geometry_types.h"
#include "impeller/core/texture.h"
#include "impeller/geometry/size.h"

namespace impeller {

class ContentContext;

// A small, conservative GPU-result cache for DisplayLists explicitly marked
// as complex and static by the framework. Unlike Flutter's legacy raster
// cache, this cache owns Impeller textures and never routes through Ganesh.
//
// The accepted DisplayList subset is intentionally narrow. In particular,
// cached pictures cannot sample external state (images or backdrops) or use
// destination-dependent blending. This makes a cached transparent texture a
// semantic replacement for replaying the picture into its parent.
class StaticPictureCache final {
 public:
  struct Limits {
    size_t max_entries = 32u;
    size_t max_bytes = 64u * 1024u * 1024u;
    size_t max_entry_bytes = 16u * 1024u * 1024u;
    size_t max_observations = 128u;
    size_t max_eligibility_records = 128u;
    uint32_t admission_threshold = 2u;
  };

  struct Statistics {
    uint64_t hits = 0u;
    uint64_t generations = 0u;
    uint64_t admission_misses = 0u;
    uint64_t eligibility_checks = 0u;
    uint64_t rejected_pictures = 0u;
    uint64_t rejected_transforms = 0u;
    uint64_t rejected_by_budget = 0u;
    uint64_t allocation_failures = 0u;
    uint64_t evictions = 0u;
  };

  using Snapshotter =
      std::function<std::shared_ptr<Texture>(const sk_sp<flutter::DisplayList>&,
                                             ISize)>;

  explicit StaticPictureCache(ContentContext& renderer);
  StaticPictureCache(ContentContext& renderer,
                     Limits limits,
                     Snapshotter snapshotter = nullptr);
  ~StaticPictureCache();

  // Records one cached image draw and returns true on a cache hit or on the
  // frame that admits and generates a new entry. Returns false when the caller
  // must replay the original DisplayList.
  bool Draw(const sk_sp<flutter::DisplayList>& display_list,
            const flutter::DlMatrix& canvas_transform,
            const std::optional<flutter::DlMatrix>& canvas_to_render_target,
            flutter::DlScalar opacity,
            flutter::DlCanvas& canvas);

  const Statistics& GetStatistics() const { return statistics_; }
  size_t GetEntryCount() const { return entries_.size(); }
  size_t GetResidentBytes() const { return resident_bytes_; }

  // Exposed for focused eligibility tests; production callers use Draw().
  static bool IsPictureEligibleForTesting(
      const sk_sp<flutter::DisplayList>& display_list);

 private:
  struct Key {
    uint32_t display_list_id = 0u;
    float scale_x = 0.0f;
    float scale_y = 0.0f;
    float alignment_x = 0.0f;
    float alignment_y = 0.0f;

    bool operator==(const Key& other) const;

    struct Hash {
      size_t operator()(const Key& key) const;
    };
  };

  struct Plan {
    Key key;
    flutter::DlIRect device_bounds;
    flutter::DlRect destination;
    flutter::DlMatrix snapshot_transform;
    size_t estimated_bytes = 0u;
  };

  struct Entry {
    sk_sp<flutter::DlImage> image;
    size_t bytes = 0u;
    uint64_t last_access = 0u;
  };

  struct Observation {
    uint32_t count = 0u;
    uint32_t retry_cooldown = 0u;
    uint64_t last_access = 0u;
  };

  struct EligibilityRecord {
    bool eligible = false;
    uint64_t last_access = 0u;
  };

  static std::optional<Plan> MakePlan(
      const sk_sp<flutter::DisplayList>& display_list,
      const flutter::DlMatrix& transform);
  static bool IsPictureEligible(
      const sk_sp<flutter::DisplayList>& display_list);

  bool IsEligible(const sk_sp<flutter::DisplayList>& display_list);
  bool Observe(const Plan& plan);
  std::shared_ptr<Texture> CreateSnapshot(
      const sk_sp<flutter::DisplayList>& snapshot_display_list,
      ISize size);
  void DrawEntry(const Entry& entry,
                 const Plan& plan,
                 flutter::DlScalar opacity,
                 flutter::DlCanvas& canvas) const;
  void MakeRoom(size_t bytes);
  void TrimObservations();
  void TrimEligibilityRecords();

  ContentContext& renderer_;
  const Limits limits_;
  Snapshotter snapshotter_;
  Statistics statistics_;
  uint64_t access_ = 0u;
  size_t resident_bytes_ = 0u;
  std::unordered_map<Key, Entry, Key::Hash> entries_;
  std::unordered_map<Key, Observation, Key::Hash> observations_;
  std::unordered_map<uint32_t, EligibilityRecord> eligibility_;

  StaticPictureCache(const StaticPictureCache&) = delete;
  StaticPictureCache& operator=(const StaticPictureCache&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_STATIC_PICTURE_CACHE_H_
