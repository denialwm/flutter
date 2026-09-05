// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/effects/image_filters/dl_glass_image_filter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace flutter {

namespace {

bool UseInwardGlassBounds() {
  static const bool requested = [] {
    const char* value = std::getenv("DENIA_GLASS_INWARD_BOUNDS");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
  }();
  return requested;
}

DlScalar MaximumGlassDisplacement(DlScalar thickness,
                                  DlScalar refraction,
                                  DlScalar dispersion) {
  const DlScalar refractive_index = 1.0f + refraction * 0.2f;
  const DlScalar refraction_distance =
      thickness * 8.0f *
      std::sqrt(std::max(refractive_index * refractive_index - 1.0f, 0.0f));
  return refraction_distance * (1.0f + dispersion * 0.5f);
}

DlScalar GlassBoundsPadding(DlScalar sigma_x,
                            DlScalar sigma_y,
                            DlScalar thickness,
                            DlScalar refraction,
                            DlScalar dispersion) {
  return std::max(
      {sigma_x * 3.0f, sigma_y * 3.0f,
       MaximumGlassDisplacement(thickness, refraction, dispersion)});
}

}  // namespace

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
  const DlScalar padding = GlassBoundsPadding(sigma_x_, sigma_y_, thickness_,
                                              refraction_, dispersion_);
  output_bounds = input_bounds.Expand(padding);
  return &output_bounds;
}

DlIRect* DlGlassImageFilter::map_device_bounds(const DlIRect& input_bounds,
                                               const DlMatrix& ctm,
                                               DlIRect& output_bounds) const {
  const DlScalar padding = GlassBoundsPadding(sigma_x_, sigma_y_, thickness_,
                                              refraction_, dispersion_);
  return outset_device_bounds(input_bounds, padding, padding, ctm,
                              output_bounds);
}

DlIRect* DlGlassImageFilter::get_input_device_bounds(
    const DlIRect& output_bounds,
    const DlMatrix& ctm,
    DlIRect& input_bounds) const {
  if (UseInwardGlassBounds() && backdrop_alpha_threshold_ < 0.0f &&
      ctm.IsTranslationScaleOnly() &&
      DlIRect::RoundOut(shape_.GetBounds().TransformBounds(ctm)) ==
          output_bounds) {
    // The material shader refracts opposite the outward normal. For a full
    // axis-aligned rounded box, each coordinate therefore moves toward its
    // centre. Every source coordinate is bounded by the union of the material
    // rectangle and [centre - maximum_ray, centre + maximum_ray]. Thus a ray
    // shorter than a half-extent requires no extra halo along that axis.
    // Use 9 * thickness (height <= thickness plus the 8 * thickness optical
    // path), maximum basis scale and two pixels of rounding slack. Cropped,
    // rotated or thresholded materials retain the general bound.
    const DlScalar half_width = output_bounds.GetWidth() * 0.5f;
    const DlScalar half_height = output_bounds.GetHeight() * 0.5f;
    const DlScalar physical_thickness =
        std::min(thickness_ * ctm.GetMaxBasisLengthXY(),
                 std::min(half_width, half_height));
    const DlScalar maximum_ray =
        MaximumGlassDisplacement(physical_thickness, refraction_, dispersion_) *
        (9.0f / 8.0f);
    DlIRect blur_bounds;
    if (std::isfinite(maximum_ray) &&
        outset_device_bounds(output_bounds, sigma_x_ * 3.0f, sigma_y_ * 3.0f,
                             ctm, blur_bounds)) {
      input_bounds = DlIRect::RoundOut(
          DlRect::Make(blur_bounds)
              .Expand(std::max(maximum_ray - half_width, 0.0f) + 2.0f,
                      std::max(maximum_ray - half_height, 0.0f) + 2.0f));
      return &input_bounds;
    }
  }
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
