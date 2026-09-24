// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FROST_CACHE_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FROST_CACHE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include "impeller/core/texture.h"
#include "impeller/geometry/rect.h"

namespace impeller {

// Describes the pixels produced by an unbounded clamp Gaussian, independently
// of the material that will sample them. Exact UVs and scale deliberately keep
// different crop boundaries/sampling grids apart; containing coverage alone is
// not sufficient because each cropped convolution clamps at its own edges.
struct GlassFrostKey {
  ISize size;
  Quad source_uvs;
  Vector2 scaled_sigma;
  Vector2 effective_scale;
  uint64_t sampler_key = 0;
  Scalar source_y_scale = 1;
  bool source_mips_ready = false;

  bool operator==(const GlassFrostKey& other) const = default;
};

// One immutable scene snapshot, one Canvas submission. Canvas clears this at
// every scene flip and at frame end. Keeping the source alive also prevents
// resource identity reuse. No hashing, allocation, eviction, or recency updates
// occur during a lookup. A full cache simply leaves later misses uncached.
class GlassFrostCache {
 public:
  static constexpr size_t kCapacity = 8;

  void SetSource(const std::shared_ptr<Texture>& source) {
    if (source_ != source) {
      Clear();
      source_ = source;
    }
  }

  const std::shared_ptr<Texture>& GetSource() const { return source_; }

  std::shared_ptr<Texture> Find(const GlassFrostKey& key) const {
    for (size_t i = 0; i < count_; ++i) {
      if (entries_[i].key == key) {
        return entries_[i].texture;
      }
    }
    return nullptr;
  }

  void Store(const GlassFrostKey& key, std::shared_ptr<Texture> texture) {
    if (source_ && texture && count_ < entries_.size()) {
      entries_[count_++] = Entry{key, std::move(texture)};
    }
  }

  void Clear() {
    for (size_t i = 0; i < count_; ++i) {
      entries_[i].texture.reset();
    }
    count_ = 0;
    source_.reset();
  }

 private:
  struct Entry {
    GlassFrostKey key;
    std::shared_ptr<Texture> texture;
  };
  std::array<Entry, kCapacity> entries_;
  size_t count_ = 0;
  std::shared_ptr<Texture> source_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_FILTERS_GLASS_FROST_CACHE_H_
