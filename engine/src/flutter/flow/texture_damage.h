// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_TEXTURE_DAMAGE_H_
#define FLUTTER_FLOW_TEXTURE_DAMAGE_H_

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "flutter/display_list/geometry/dl_geometry_types.h"

namespace flutter {

// Denial: what changed in an external texture's new frames, as rectangles in
// unit coordinates of the rect the texture is painted into ((0, 0) its top
// left, (1, 1) its bottom right). A dirty texture without an entry changed
// whole, as a texture without damage always did.
using TextureDamageMap = std::unordered_map<int64_t, std::vector<DlRect>>;

// More rectangles than this become their bounds.
constexpr size_t kMaxTextureDamageRects = 16;

// Adds `texture_id` to `ids`: changed in `rects`, or whole when null. Partial
// damage joins only partial damage; whole damage stays whole.
inline void AddDirtyTexture(std::unordered_set<int64_t>& ids,
                            TextureDamageMap& damage,
                            int64_t texture_id,
                            const std::vector<DlRect>* rects) {
  if (!rects) {
    ids.insert(texture_id);
    damage.erase(texture_id);
    return;
  }
  if (!ids.insert(texture_id).second) {
    auto found = damage.find(texture_id);
    if (found == damage.end()) {
      return;  // Already whole.
    }
    auto& joined = found->second;
    joined.insert(joined.end(), rects->begin(), rects->end());
    if (joined.size() > kMaxTextureDamageRects) {
      DlRect bounds = joined.front();
      for (const auto& rect : joined) {
        bounds = bounds.Union(rect);
      }
      joined = {bounds};
    }
    return;
  }
  damage[texture_id] = *rects;
}

// Adds every texture of `from_ids` with its damage.
inline void MergeDirtyTextures(std::unordered_set<int64_t>& ids,
                               TextureDamageMap& damage,
                               const std::unordered_set<int64_t>& from_ids,
                               const TextureDamageMap& from_damage) {
  for (const int64_t texture_id : from_ids) {
    const auto found = from_damage.find(texture_id);
    AddDirtyTexture(ids, damage, texture_id,
                    found == from_damage.end() ? nullptr : &found->second);
  }
}

}  // namespace flutter

#endif  // FLUTTER_FLOW_TEXTURE_DAMAGE_H_
