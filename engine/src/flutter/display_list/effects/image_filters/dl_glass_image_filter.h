// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_DISPLAY_LIST_EFFECTS_IMAGE_FILTERS_DL_GLASS_IMAGE_FILTER_H_
#define FLUTTER_DISPLAY_LIST_EFFECTS_IMAGE_FILTERS_DL_GLASS_IMAGE_FILTER_H_

#include "flutter/display_list/dl_color.h"
#include "flutter/display_list/effects/dl_image_filter.h"

namespace flutter {

class DlGlassImageFilter final : public DlImageFilter {
 public:
  DlGlassImageFilter(DlScalar sigma_x,
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
                     bool backdrop_alpha_threshold_is_single_surface,
                     DlScalar bevel_width_scale = 1.0f,
                     DlScalar refraction_depth_scale = 1.0f,
                     DlScalar rim_width = 1.5f,
                     DlScalar rim_falloff = 0.89f,
                     DlScalar opposite_light_strength = 0.8f)
      : sigma_x_(sigma_x),
        sigma_y_(sigma_y),
        shape_(shape),
        downsample_scale_(downsample_scale),
        thickness_(thickness),
        refraction_(refraction),
        dispersion_(dispersion),
        saturation_(saturation),
        tint_(tint),
        tint_strength_(tint_strength),
        brightness_(brightness),
        light_angle_(light_angle),
        light_intensity_(light_intensity),
        edge_strength_(edge_strength),
        backdrop_alpha_threshold_(backdrop_alpha_threshold),
        backdrop_alpha_threshold_is_single_surface_(
            backdrop_alpha_threshold_is_single_surface),
        bevel_width_scale_(bevel_width_scale),
        refraction_depth_scale_(refraction_depth_scale),
        rim_width_(rim_width),
        rim_falloff_(rim_falloff),
        opposite_light_strength_(opposite_light_strength) {}

  static std::shared_ptr<DlImageFilter> Make(
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
      DlScalar backdrop_alpha_threshold = -1.0f,
      bool backdrop_alpha_threshold_is_single_surface = false,
      DlScalar bevel_width_scale = 1.0f,
      DlScalar refraction_depth_scale = 1.0f,
      DlScalar rim_width = 1.5f,
      DlScalar rim_falloff = 0.89f,
      DlScalar opposite_light_strength = 0.8f);

  std::shared_ptr<DlImageFilter> shared() const override {
    return std::make_shared<DlGlassImageFilter>(*this);
  }

  DlImageFilterType type() const override { return DlImageFilterType::kGlass; }
  size_t size() const override { return sizeof(*this); }

  const DlGlassImageFilter* asGlass() const override { return this; }

  bool modifies_transparent_black() const override { return false; }

  DlRect* map_local_bounds(const DlRect& input_bounds,
                           DlRect& output_bounds) const override;

  DlIRect* map_device_bounds(const DlIRect& input_bounds,
                             const DlMatrix& ctm,
                             DlIRect& output_bounds) const override;

  DlIRect* get_input_device_bounds(const DlIRect& output_bounds,
                                   const DlMatrix& ctm,
                                   DlIRect& input_bounds) const override;

  DlScalar bevel_width_scale() const { return bevel_width_scale_; }
  DlScalar refraction_depth_scale() const { return refraction_depth_scale_; }
  DlScalar rim_width() const { return rim_width_; }
  DlScalar rim_falloff() const { return rim_falloff_; }
  DlScalar opposite_light_strength() const { return opposite_light_strength_; }

  DlScalar sigma_x() const { return sigma_x_; }
  DlScalar sigma_y() const { return sigma_y_; }
  const DlRoundRect& shape() const { return shape_; }
  DlScalar downsample_scale() const { return downsample_scale_; }
  DlScalar thickness() const { return thickness_; }
  DlScalar refraction() const { return refraction_; }
  DlScalar dispersion() const { return dispersion_; }
  DlScalar saturation() const { return saturation_; }
  DlColor tint() const { return tint_; }
  DlScalar tint_strength() const { return tint_strength_; }
  DlScalar brightness() const { return brightness_; }
  DlScalar light_angle() const { return light_angle_; }
  DlScalar light_intensity() const { return light_intensity_; }
  DlScalar edge_strength() const { return edge_strength_; }
  DlScalar backdrop_alpha_threshold() const {
    return backdrop_alpha_threshold_;
  }
  bool backdrop_alpha_threshold_is_single_surface() const {
    return backdrop_alpha_threshold_is_single_surface_;
  }

 protected:
  bool equals_(const DlImageFilter& other) const override;

 private:
  DlScalar sigma_x_;
  DlScalar sigma_y_;
  DlRoundRect shape_;
  DlScalar downsample_scale_;
  DlScalar thickness_;
  DlScalar refraction_;
  DlScalar dispersion_;
  DlScalar saturation_;
  DlColor tint_;
  DlScalar tint_strength_;
  DlScalar brightness_;
  DlScalar light_angle_;
  DlScalar light_intensity_;
  DlScalar edge_strength_;
  DlScalar backdrop_alpha_threshold_;
  bool backdrop_alpha_threshold_is_single_surface_;
  DlScalar bevel_width_scale_;
  DlScalar refraction_depth_scale_;
  DlScalar rim_width_;
  DlScalar rim_falloff_;
  DlScalar opposite_light_strength_;
};

}  // namespace flutter

#endif  // FLUTTER_DISPLAY_LIST_EFFECTS_IMAGE_FILTERS_DL_GLASS_IMAGE_FILTER_H_
