// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/effects/image_filters/dl_glass_image_filter.h"

#include <algorithm>
#include <cmath>

namespace flutter {

std::shared_ptr<DlImageFilter> DlGlassImageFilter::Make(
    DlScalar sigma_x,
    DlScalar sigma_y,
    DlRoundRect shape,
    DlScalar downsample_scale,
    DlScalar thickness,
    DlScalar refraction,
    DlScalar dispersion,
    DlScalar saturation,
    DlColor tint,
    DlScalar tint_strength,
    DlScalar brightness,
    DlScalar light_angle,
    DlScalar light_intensity,
    DlScalar edge_strength,
    DlScalar backdrop_alpha_threshold,
    bool backdrop_alpha_threshold_is_single_surface) {
  if (!shape.IsFinite() || shape.IsEmpty() || !std::isfinite(sigma_x) ||
      !std::isfinite(sigma_y) || !std::isfinite(downsample_scale) ||
      !std::isfinite(thickness) || !std::isfinite(refraction) ||
      !std::isfinite(dispersion) || !std::isfinite(saturation) ||
      !std::isfinite(tint_strength) || !std::isfinite(brightness) ||
      !std::isfinite(light_angle) || !std::isfinite(light_intensity) ||
      !std::isfinite(edge_strength) ||
      !std::isfinite(backdrop_alpha_threshold)) {
    return nullptr;
  }
  sigma_x = std::max(0.0f, sigma_x);
  sigma_y = std::max(0.0f, sigma_y);
  downsample_scale = std::clamp(downsample_scale, 0.0625f, 1.0f);
  thickness = std::clamp(thickness, 0.0f, 256.0f);
  refraction = std::clamp(refraction, 0.0f, 1.0f);
  dispersion = std::clamp(dispersion, 0.0f, 1.0f);
  saturation = std::clamp(saturation, 0.0f, 4.0f);
  tint_strength = std::clamp(tint_strength, 0.0f, 1.0f);
  brightness = std::clamp(brightness, -1.0f, 1.0f);
  light_intensity = std::clamp(light_intensity, 0.0f, 4.0f);
  edge_strength = std::clamp(edge_strength, 0.0f, 4.0f);
  backdrop_alpha_threshold = std::clamp(backdrop_alpha_threshold, -1.0f, 1.0f);
  return std::make_shared<DlGlassImageFilter>(
      sigma_x, sigma_y, shape, downsample_scale, thickness, refraction,
      dispersion, saturation, tint, tint_strength, brightness, light_angle,
      light_intensity, edge_strength, backdrop_alpha_threshold,
      backdrop_alpha_threshold_is_single_surface);
}

DlRect* DlGlassImageFilter::map_local_bounds(const DlRect& input_bounds,
                                             DlRect& output_bounds) const {
  const DlScalar padding =
      std::max({sigma_x_ * 3.0f, sigma_y_ * 3.0f,
                thickness_ * refraction_ * (1.0f + dispersion_)});
  output_bounds = input_bounds.Expand(padding);
  return &output_bounds;
}

DlIRect* DlGlassImageFilter::map_device_bounds(const DlIRect& input_bounds,
                                               const DlMatrix& ctm,
                                               DlIRect& output_bounds) const {
  const DlScalar padding =
      std::max({sigma_x_ * 3.0f, sigma_y_ * 3.0f,
                thickness_ * refraction_ * (1.0f + dispersion_)});
  return outset_device_bounds(input_bounds, padding, padding, ctm,
                              output_bounds);
}

DlIRect* DlGlassImageFilter::get_input_device_bounds(
    const DlIRect& output_bounds,
    const DlMatrix& ctm,
    DlIRect& input_bounds) const {
  return map_device_bounds(output_bounds, ctm, input_bounds);
}

bool DlGlassImageFilter::equals_(const DlImageFilter& other) const {
  FML_DCHECK(other.type() == DlImageFilterType::kGlass);
  const auto* that = static_cast<const DlGlassImageFilter*>(&other);
  return DlScalarNearlyEqual(sigma_x_, that->sigma_x_) &&
         DlScalarNearlyEqual(sigma_y_, that->sigma_y_) &&
         shape_ == that->shape_ &&
         DlScalarNearlyEqual(downsample_scale_, that->downsample_scale_) &&
         DlScalarNearlyEqual(thickness_, that->thickness_) &&
         DlScalarNearlyEqual(refraction_, that->refraction_) &&
         DlScalarNearlyEqual(dispersion_, that->dispersion_) &&
         DlScalarNearlyEqual(saturation_, that->saturation_) &&
         tint_ == that->tint_ &&
         DlScalarNearlyEqual(tint_strength_, that->tint_strength_) &&
         DlScalarNearlyEqual(brightness_, that->brightness_) &&
         DlScalarNearlyEqual(light_angle_, that->light_angle_) &&
         DlScalarNearlyEqual(light_intensity_, that->light_intensity_) &&
         DlScalarNearlyEqual(edge_strength_, that->edge_strength_) &&
         DlScalarNearlyEqual(backdrop_alpha_threshold_,
                             that->backdrop_alpha_threshold_) &&
         backdrop_alpha_threshold_is_single_surface_ ==
             that->backdrop_alpha_threshold_is_single_surface_;
}

}  // namespace flutter
