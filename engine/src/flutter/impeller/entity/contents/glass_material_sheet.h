// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_GLASS_MATERIAL_SHEET_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_GLASS_MATERIAL_SHEET_H_

#include <optional>
#include <vector>

#include "impeller/geometry/rect.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

/// One persistent render target that holds the glass materials of windows,
/// each at its position in the root pass that composites it.
///
/// A dedicated material texture follows its window's size, so moving or
/// resizing windows allocates a new texture per window per frame. Windows
/// that share a backdrop never overlap, and a window that overlaps another
/// first ends the pass that composites the one below it. The regions still
/// waiting for their composite therefore never overlap. A region stays
/// reserved until its owner has encoded the pass that samples it; GLES
/// executes passes in encoding order, so a later write follows that read.
class GlassMaterialSheet {
 public:
  GlassMaterialSheet();

  ~GlassMaterialSheet();

  /// Reserves `region` for `owner` and returns the sheet to render it into.
  /// The sheet grows to at least `minimum_size` and never shrinks. Returns
  /// nothing when `region` overlaps a region that is still reserved, or when
  /// the sheet cannot contain it.
  std::optional<RenderTarget> Reserve(const Context& context,
                                      const void* owner,
                                      const IRect& region,
                                      ISize minimum_size);

  /// Releases every region `owner` reserved.
  void Release(const void* owner);

 private:
  struct Reservation {
    const void* owner;
    IRect region;
  };

  std::optional<RenderTarget> target_;
  std::vector<Reservation> reservations_;

  GlassMaterialSheet(const GlassMaterialSheet&) = delete;

  GlassMaterialSheet& operator=(const GlassMaterialSheet&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_GLASS_MATERIAL_SHEET_H_
