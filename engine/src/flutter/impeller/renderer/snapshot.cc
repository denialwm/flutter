// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/snapshot.h"

#include <algorithm>
#include <optional>

namespace impeller {

std::optional<Rect> Snapshot::GetCoverage() const {
  if (!texture) {
    return std::nullopt;
  }
  return Rect::MakeSize(texture->GetSize()).TransformBounds(transform);
}

std::optional<Matrix> Snapshot::GetUVTransform() const {
  if (!texture || texture->GetSize().IsEmpty()) {
    return std::nullopt;
  }
  return Matrix::MakeScale(1 / Vector2(texture->GetSize())) *
         transform.Invert();
}

std::optional<std::array<Point, 4>> Snapshot::GetCoverageUVs(
    const Rect& coverage) const {
  auto uv_transform = GetUVTransform();
  if (!uv_transform.has_value()) {
    return std::nullopt;
  }
  return coverage.GetTransformedPoints(uv_transform.value());
}

Vector4 Snapshot::GetUnboundedSampleUVs() {
  // Wide enough for any coordinate a filter samples, yet representable at
  // half precision.
  constexpr Scalar kUnbounded = 16384.0f;
  return Vector4(-kUnbounded, -kUnbounded, kUnbounded, kUnbounded);
}

Vector4 Snapshot::GetSampleBoundsUVs() const {
  if (!texture || !sample_bounds.has_value() || sample_bounds->IsEmpty() ||
      texture->GetSize().IsEmpty()) {
    return GetUnboundedSampleUVs();
  }
  const Size size(texture->GetSize());
  const Rect& bounds = sample_bounds.value();
  const Scalar inset_x = std::min(0.5f, bounds.GetWidth() * 0.5f);
  const Scalar inset_y = std::min(0.5f, bounds.GetHeight() * 0.5f);
  const Scalar left = (bounds.GetLeft() + inset_x) / size.width;
  const Scalar right = (bounds.GetRight() - inset_x) / size.width;
  Scalar top = (bounds.GetTop() + inset_y) / size.height;
  Scalar bottom = (bounds.GetBottom() - inset_y) / size.height;
  if (texture->GetYCoordScale() < 0.0f) {
    const Scalar flipped_top = 1.0f - bottom;
    bottom = 1.0f - top;
    top = flipped_top;
  }
  return Vector4(left, top, right, bottom);
}

}  // namespace impeller
