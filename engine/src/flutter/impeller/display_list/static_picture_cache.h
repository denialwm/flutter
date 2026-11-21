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
#include <string>
#include <unordered_map>

#include "flutter/display_list/display_list.h"
#include "impeller/core/formats.h"
#include "impeller/core/texture.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/point.h"
#include "impeller/geometry/rect.h"
#include "impeller/geometry/size.h"

namespace impeller {

class Canvas;
class ContentContext;
class RenderTarget;

// A small, conservative GPU-result cache for DisplayLists explicitly marked
// as complex and static by the framework. Unlike Flutter's legacy raster
// cache, this cache owns Impeller textures and never routes through Ganesh.
//
// The accepted DisplayList subset is intentionally narrow. In particular,
// cached pictures cannot sample external state (images or backdrops) or use
// destination-dependent blending. This makes a cached transparent texture a
// semantic replacement for replaying the picture into its parent.
//
// The cache works where Impeller replays the picture, not where the layer
// tree records it: only there is the pass the picture lands in known. Its
// snapshot is rendered as that pass would render the picture (same sample
// count, color format and row order, in the pass's own pixel coordinates
// moved by whole pixels), so the image has the pixels the live replay
// would have.
class StaticPictureCache final {
 public:
  struct Limits {
    size_t max_entries = 32u;
    size_t max_bytes = 64u * 1024u * 1024u;
    size_t max_entry_bytes = 16u * 1024u * 1024u;
    size_t max_observations = 128u;
    size_t max_eligibility_records = 128u;
    uint32_t admission_threshold = 2u;
    // How often a picture may be drawn at other keys before an entry of it
    // counts as left behind. A picture two outputs show at different scales
    // uses both of its entries in turn and keeps them.
    uint32_t stale_key_draws = 240u;
  };

  struct Statistics {
    uint64_t hits = 0u;
    uint64_t generations = 0u;
    uint64_t admission_misses = 0u;
    uint64_t eligibility_checks = 0u;
    uint64_t rejected_pictures = 0u;
    uint64_t rejected_transforms = 0u;
    uint64_t rejected_passes = 0u;
    uint64_t rejected_by_budget = 0u;
    uint64_t allocation_failures = 0u;
    uint64_t evictions = 0u;
    uint64_t collected = 0u;
    uint64_t stale_keys = 0u;
  };

  // What a snapshot must share with the pass its picture lands in to come
  // out with the same pixels. `format` is the snapshot's own: the pass's,
  // unless the pass keeps too little alpha to hold the picture (DescribePass).
  struct Pass {
    SampleCount sample_count = SampleCount::kCount1;
    // kUnknown: the context's default color format.
    PixelFormat format = PixelFormat::kUnknown;
    // Rendered without the GLES backend's offscreen vertex flip: a borrowed
    // FBO such as Denial's output, or a texture stored bottom-up. Mirrored
    // rows break a tie on a pixel centre the other way.
    bool bottom_up = false;

    bool operator==(const Pass& other) const = default;
  };

  // A cached picture: its texture, and where its top-left pixel goes in the
  // pass's pixels. The texture maps one to one onto pass pixels.
  struct Image {
    std::shared_ptr<Texture> texture;
    IPoint origin;
  };

  using Snapshotter =
      std::function<std::shared_ptr<Texture>(const sk_sp<flutter::DisplayList>&,
                                             ISize)>;

  explicit StaticPictureCache(ContentContext& renderer);
  StaticPictureCache(ContentContext& renderer,
                     Limits limits,
                     Snapshotter snapshotter = nullptr);
  ~StaticPictureCache();

  // Marks a picture the framework called complex and static, as the layer
  // tree records it: where Impeller replays it, Draw may draw it from the
  // cache.
  void Request(const sk_sp<flutter::DisplayList>& display_list);

  // Called where a nested DisplayList is replayed into `canvas`. Draws a
  // requested picture as its cached image, rendered for the canvas's current
  // pass, and returns true; returns false when the caller must replay it.
  bool Draw(const sk_sp<flutter::DisplayList>& display_list, Canvas& canvas);

  // One draw's cache work: the image of `display_list` drawn with
  // `pass_transform` (the transform its entities would get) into `pass`.
  // Counts the sighting, and generates the image once it is admitted.
  // Draw's core, exposed for tests.
  std::optional<Image> Lookup(const sk_sp<flutter::DisplayList>& display_list,
                              const Matrix& pass_transform,
                              const Pass& pass);

  // The pass properties of `target`, or none when a snapshot cannot match
  // them.
  std::optional<Pass> DescribePass(const RenderTarget& target) const;

  // Drops the entries that cannot be drawn again. Called once per frame.
  //
  // An entry whose picture nothing but this cache holds any more: a
  // picture's id is never used again. And an entry its picture has left
  // behind: the picture keeps being drawn, at another scale or pixel
  // alignment. Without this, each re-recorded picture and each place an
  // animation rested would leave a texture until the byte budget pushed it
  // out.
  void Collect();

  // The render audit's view of the cache: its counters, then every entry
  // with its picture id, key, size and how many draws ago it was last used.
  std::string Describe() const;

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
    Pass pass;

    bool operator==(const Key& other) const;

    struct Hash {
      size_t operator()(const Key& key) const;
    };
  };

  struct Plan {
    Key key;
    IRect device_bounds;
    // The pass transform, moved to the rounded bounds' origin.
    Matrix snapshot_transform;
    size_t estimated_bytes = 0u;
  };

  // One per cached picture, however many entries it has: with this single
  // reference the cache can tell when it is the picture's last holder.
  struct Picture {
    sk_sp<flutter::DisplayList> display_list;
    uint64_t draws = 0u;
    size_t entries = 0u;
  };

  struct Entry {
    std::shared_ptr<Texture> texture;
    Picture* picture = nullptr;
    size_t bytes = 0u;
    uint64_t last_access = 0u;
    // The picture's draw count when this entry was last drawn.
    uint64_t last_draw = 0u;
  };

  using Entries = std::unordered_map<Key, Entry, Key::Hash>;

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
      const Matrix& transform,
      const Pass& pass);
  static bool IsPictureEligible(
      const sk_sp<flutter::DisplayList>& display_list);

  bool IsEligible(const sk_sp<flutter::DisplayList>& display_list);
  bool Observe(const Plan& plan);
  std::shared_ptr<Texture> CreateSnapshot(
      const sk_sp<flutter::DisplayList>& snapshot_display_list,
      ISize size,
      const Pass& pass);
  Entries::iterator EraseEntry(Entries::iterator entry);
  void MakeRoom(size_t bytes);
  void TrimObservations();
  void TrimEligibilityRecords();
  void TrimRequests();

  ContentContext& renderer_;
  const Limits limits_;
  Snapshotter snapshotter_;
  Statistics statistics_;
  uint64_t access_ = 0u;
  size_t resident_bytes_ = 0u;
  // Set while a snapshot renders: its own replay of the picture is live.
  bool generating_ = false;
  Entries entries_;
  std::unordered_map<uint32_t, Picture> pictures_;
  std::unordered_map<Key, Observation, Key::Hash> observations_;
  std::unordered_map<uint32_t, EligibilityRecord> eligibility_;
  // Requested picture ids and when each was last requested or drawn.
  std::unordered_map<uint32_t, uint64_t> requests_;

  StaticPictureCache(const StaticPictureCache&) = delete;
  StaticPictureCache& operator=(const StaticPictureCache&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_STATIC_PICTURE_CACHE_H_
