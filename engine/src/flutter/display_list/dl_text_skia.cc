// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/display_list/dl_text_skia.h"

namespace flutter {
std::shared_ptr<DlTextSkia> DlTextSkia::Make(const sk_sp<SkTextBlob>& blob) {
  return std::make_shared<DlTextSkia>(blob);
}

DlTextSkia::DlTextSkia(const sk_sp<SkTextBlob>& blob) : blob_(blob) {}

bool DlTextSkia::IsSingleGlyph() const {
  int glyphs = 0;
  SkTextBlob::Iter::Run run;
  for (SkTextBlob::Iter it(*blob_); it.next(&run);) {
    glyphs += run.fGlyphCount;
  }
  return glyphs == 1;
}

}  // namespace flutter
