// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/lib/ui/painting/image_filter.h"

#include "display_list/dl_sampling_options.h"
#include "display_list/effects/dl_image_filters.h"
#include "flutter/lib/ui/floating_point.h"
#include "flutter/lib/ui/painting/matrix.h"
#include "flutter/lib/ui/painting/rrect.h"
#include "flutter/lib/ui/ui_dart_state.h"
#include "lib/ui/painting/fragment_program.h"
#include "lib/ui/painting/fragment_shader.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_args.h"
#include "third_party/tonic/dart_binding_macros.h"
#include "third_party/tonic/dart_library_natives.h"

namespace flutter {

IMPLEMENT_WRAPPERTYPEINFO(ui, ImageFilter);

void ImageFilter::Create(Dart_Handle wrapper) {
  UIDartState::ThrowIfUIOperationsProhibited();
  auto res = fml::MakeRefCounted<ImageFilter>();
  res->AssociateWithDartWrapper(wrapper);
}

static const std::array<DlImageSampling, 4> kFilterQualities = {
    DlImageSampling::kNearestNeighbor,
    DlImageSampling::kLinear,
    DlImageSampling::kMipmapLinear,
    DlImageSampling::kCubic,
};

DlImageSampling ImageFilter::SamplingFromIndex(int filterQualityIndex) {
  if (filterQualityIndex < 0) {
    return kFilterQualities.front();
  } else if (static_cast<size_t>(filterQualityIndex) >=
             kFilterQualities.size()) {
    return kFilterQualities.back();
  } else {
    return kFilterQualities[filterQualityIndex];
  }
}

DlFilterMode ImageFilter::FilterModeFromIndex(int filterQualityIndex) {
  if (filterQualityIndex <= 0) {
    return DlFilterMode::kNearest;
  }
  return DlFilterMode::kLinear;
}

ImageFilter::ImageFilter() {}

ImageFilter::~ImageFilter() {}

const std::shared_ptr<DlImageFilter> ImageFilter::filter(
    DlTileMode mode) const {
  if (is_dynamic_tile_mode_) {
    FML_DCHECK(filter_.get() != nullptr);
    const DlBlurImageFilter* blur_filter = filter_->asBlur();
    FML_DCHECK(blur_filter != nullptr);
    if (blur_filter->tile_mode() != mode) {
      return DlBlurImageFilter::Make(
          blur_filter->sigma_x(), blur_filter->sigma_y(), mode,
          blur_filter->bounds(), blur_filter->downsample_scale(),
          blur_filter->backdrop_alpha_threshold(),
          blur_filter->backdrop_alpha_threshold_is_single_surface());
    }
  }
  return filter_;
}

void ImageFilter::initBlur(double sigma_x,
                           double sigma_y,
                           int tile_mode_index,
                           bool bounded,
                           double bounds_left,
                           double bounds_top,
                           double bounds_right,
                           double bounds_bottom,
                           double downsample_scale,
                           double backdrop_alpha_threshold,
                           bool backdrop_alpha_threshold_is_single_surface) {
  DlTileMode tile_mode;
  bool is_dynamic;
  if (tile_mode_index < 0) {
    is_dynamic = true;
    tile_mode = DlTileMode::kClamp;
  } else {
    is_dynamic = false;
    tile_mode = static_cast<DlTileMode>(tile_mode_index);
  }
  std::optional<DlRect> bounds;
  if (bounded) {
    bounds =
        DlRect::MakeLTRB(SafeNarrow(bounds_left), SafeNarrow(bounds_top),
                         SafeNarrow(bounds_right), SafeNarrow(bounds_bottom));
  }
  filter_ = DlBlurImageFilter::Make(
      SafeNarrow(sigma_x), SafeNarrow(sigma_y), tile_mode, bounds,
      SafeNarrow(downsample_scale), SafeNarrow(backdrop_alpha_threshold),
      backdrop_alpha_threshold_is_single_surface);
  // If it was a NOP filter, don't bother processing dynamic substitutions
  // (They'd fail the FML_DCHECK anyway)
  is_dynamic_tile_mode_ = is_dynamic && filter_;
}

void ImageFilter::initGlass(double sigma_x,
                            double sigma_y,
                            const RRect& shape,
                            double downsample_scale,
                            double thickness,
                            double refraction,
                            double dispersion,
                            double saturation,
                            uint32_t tint,
                            double tint_strength,
                            double brightness,
                            double light_angle,
                            double light_intensity,
                            double edge_strength,
                            double backdrop_alpha_threshold,
                            bool backdrop_alpha_threshold_is_single_surface) {
  is_dynamic_tile_mode_ = false;
  filter_ = DlGlassImageFilter::Make(
      SafeNarrow(sigma_x), SafeNarrow(sigma_y), shape.rrect,
      SafeNarrow(downsample_scale), SafeNarrow(thickness),
      SafeNarrow(refraction), SafeNarrow(dispersion), SafeNarrow(saturation),
      DlColor(tint), SafeNarrow(tint_strength), SafeNarrow(brightness),
      SafeNarrow(light_angle), SafeNarrow(light_intensity),
      SafeNarrow(edge_strength), SafeNarrow(backdrop_alpha_threshold),
      backdrop_alpha_threshold_is_single_surface);
}

void ImageFilter::initDilate(double radius_x, double radius_y) {
  is_dynamic_tile_mode_ = false;
  filter_ =
      DlDilateImageFilter::Make(SafeNarrow(radius_x), SafeNarrow(radius_y));
}

void ImageFilter::initErode(double radius_x, double radius_y) {
  is_dynamic_tile_mode_ = false;
  filter_ =
      DlErodeImageFilter::Make(SafeNarrow(radius_x), SafeNarrow(radius_y));
}

void ImageFilter::initMatrix(const tonic::Float64List& matrix4,
                             int filterQualityIndex) {
  is_dynamic_tile_mode_ = false;
  auto sampling = ImageFilter::SamplingFromIndex(filterQualityIndex);
  filter_ = DlMatrixImageFilter::Make(ToDlMatrix(matrix4), sampling);
}

void ImageFilter::initColorFilter(ColorFilter* colorFilter) {
  FML_DCHECK(colorFilter);
  is_dynamic_tile_mode_ = false;
  filter_ = DlColorFilterImageFilter::Make(colorFilter->filter());
}

void ImageFilter::initComposeFilter(ImageFilter* outer, ImageFilter* inner) {
  FML_DCHECK(outer && inner);
  is_dynamic_tile_mode_ = false;
  filter_ = DlComposeImageFilter::Make(outer->filter(DlTileMode::kClamp),
                                       inner->filter(DlTileMode::kClamp));
}

void ImageFilter::initShader(ReusableFragmentShader* shader) {
  FML_DCHECK(shader);
  filter_ = shader->as_image_filter();
}

bool ImageFilter::equals(Dart_Handle a_handle, Dart_Handle b_handle) {
  ImageFilter* a = tonic::DartConverter<ImageFilter*>::FromDart(a_handle);
  ImageFilter* b = tonic::DartConverter<ImageFilter*>::FromDart(b_handle);
  if (a == b) {
    return true;
  }
  if (!a || !b) {
    return false;
  }
  if (a->filter_ == b->filter_) {
    return true;
  }
  if (!a->filter_ || !b->filter_) {
    return false;
  }
  return *a->filter_ == *b->filter_;
}

}  // namespace flutter
