// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/glass_material_sheet.h"

#include <algorithm>

namespace impeller {

GlassMaterialSheet::GlassMaterialSheet() = default;

GlassMaterialSheet::~GlassMaterialSheet() = default;

std::optional<RenderTarget> GlassMaterialSheet::Reserve(const Context& context,
                                                        const void* owner,
                                                        const IRect& region,
                                                        ISize minimum_size) {
  if (region.IsEmpty() || region.GetLeft() < 0 || region.GetTop() < 0) {
    return std::nullopt;
  }
  for (const Reservation& reservation : reservations_) {
    if (reservation.region.IntersectsWithRect(region)) {
      return std::nullopt;
    }
  }

  const ISize current =
      target_.has_value() ? target_->GetRenderTargetSize() : ISize();
  const ISize required = current.Max(minimum_size)
                             .Max(ISize(region.GetRight(), region.GetBottom()));
  if (required != current) {
    const ISize maximum =
        context.GetCapabilities()->GetMaximumRenderPassAttachmentSize();
    if (required.width > maximum.width || required.height > maximum.height) {
      return std::nullopt;
    }
    // Later materials keep the regions already rendered into the sheet.
    RenderTarget::AttachmentConfig color = {
        .storage_mode = StorageMode::kDevicePrivate,
        .load_action = LoadAction::kLoad,
        .store_action = StoreAction::kStore,
        .clear_color = Color::BlackTransparent(),
    };
    RenderTargetAllocator allocator(context.GetResourceAllocator());
    RenderTarget target = allocator.CreateOffscreen(
        context, required, /*mip_count=*/1, "Denial Glass Material Sheet",
        color, /*stencil_attachment_config=*/std::nullopt);
    if (!target.IsValid()) {
      return std::nullopt;
    }
    // Pending composites keep the previous sheet alive and nothing writes to
    // it again, so their regions no longer constrain the new sheet.
    target_ = std::move(target);
    reservations_.clear();
  }
  reservations_.push_back({.owner = owner, .region = region});
  return target_;
}

void GlassMaterialSheet::Release(const void* owner) {
  std::erase_if(reservations_, [owner](const Reservation& reservation) {
    return reservation.owner == owner;
  });
}

}  // namespace impeller
