// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_IMAGE_FILTER_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_IMAGE_FILTER_H_

#include "display_list/effects/dl_image_filter.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/contents/filters/filter_contents.h"

namespace impeller {

// Copy just the window's physical pixel rectangle before filtering. Keeping
// its translation lets callers use the existing backdrop coordinate system;
// the independent texture makes clamp-to-edge apply to the window, not screen.
std::optional<Snapshot> CropWindowBackdrop(
    const ContentContext& renderer,
    const std::shared_ptr<Texture>& texture,
    const Rect& window_bounds);

/// @brief  Generate a new FilterContents using this filter's configuration.
///
std::shared_ptr<FilterContents> WrapInput(const ContentContext& renderer,
                                          const flutter::DlImageFilter* filter,
                                          const FilterInput::Ref& input);

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_IMAGE_FILTER_H_
