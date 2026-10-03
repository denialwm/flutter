// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_SNAPSHOT_H_
#define FLUTTER_IMPELLER_RENDERER_SNAPSHOT_H_

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "impeller/core/formats.h"
#include "impeller/core/sampler_descriptor.h"
#include "impeller/core/texture.h"
#include "impeller/geometry/matrix.h"
#include "impeller/geometry/rect.h"

namespace impeller {

class ContentContext;
class Entity;

/// Represents a texture and its intended draw transform/sampler configuration.
struct Snapshot {
  std::shared_ptr<Texture> texture;
  /// The transform that should be applied to this texture for rendering.
  Matrix transform;

  SamplerDescriptor sampler_descriptor =
      SamplerDescriptor("Default Snapshot Sampler",
                        MinMagFilter::kLinear,
                        MinMagFilter::kLinear,
                        MipFilter::kNearest);

  Scalar opacity = 1.0f;

  /// @brief Whether this snapshot needs to be re-rasterized when used as an
  /// input to a runtime effect.
  /// @details This is required because there is no good heuristic to determine
  /// if a `Snapshot` needs to be rerasterized before applying a RuntimeFilter.
  /// In particular the GaussianBlurContents will return a Snapshot that
  /// includes padding for the blur halo which is not possible for the
  /// RuntimeEffectContents to know about. This value will tell
  /// RuntimeEffectContents that the Snapshot will have to be rerasterized to
  /// capture the padding.
  bool needs_rasterization_for_runtime_effects = false;

  /// The texels this snapshot represents, in texture pixel coordinates.
  /// Filters that honor it read the edge texels outside this rectangle, as if
  /// the texture had been cropped to it and sampled with clamp-to-edge. A
  /// filter can then read a region of a larger texture without copying it.
  std::optional<Rect> sample_bounds;

  /// Any snapshot that is scaled should re-rasterize because we should be
  /// performing the RuntimeEffect at the resolution of the screen, not the
  /// scaled up or scaled down version of the snapshot.
  bool ShouldRasterizeForRuntimeEffects() const {
    // If the transform has a rotation we don't re-rasterize because we'll lose
    // the rotation.
    // TODO(tbd): We should re-rasterize scaled and rotated snapshots.
    return (!transform.IsTranslationOnly() &&
            transform.IsTranslationScaleOnly()) ||
           needs_rasterization_for_runtime_effects;
  }

  std::optional<Rect> GetCoverage() const;

  /// @brief  Get the transform that converts screen space coordinates to the UV
  ///         space of this snapshot.
  std::optional<Matrix> GetUVTransform() const;

  /// @brief  Map a coverage rect to this filter input's UV space.
  ///         Result order: Top left, top right, bottom left, bottom right.
  std::optional<std::array<Point, 4>> GetCoverageUVs(
      const Rect& coverage) const;

  /// @brief  The texture coordinates of the edge texel centers of
  ///         `sample_bounds`, as (left, top, right, bottom) in the texture's
  ///         sampling orientation. Clamping linear samples to this rectangle
  ///         matches clamp-to-edge sampling of the cropped texture. Without
  ///         sample bounds, the rectangle does not restrict sampling.
  Vector4 GetSampleBoundsUVs() const;

  /// @brief  Sample bounds texture coordinates that do not restrict sampling.
  static Vector4 GetUnboundedSampleUVs();
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_SNAPSHOT_H_
