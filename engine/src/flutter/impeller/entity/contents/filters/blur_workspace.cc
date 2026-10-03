// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/contents/filters/blur_workspace.h"

namespace impeller {

namespace {

// Grow in steps so that a growing window does not regrow on every frame.
constexpr int64_t kGrowthGranularity = 64;

int64_t RoundUp(int64_t value) {
  return (value + kGrowthGranularity - 1) / kGrowthGranularity *
         kGrowthGranularity;
}

}  // namespace

BlurWorkspace::BlurWorkspace() = default;

BlurWorkspace::~BlurWorkspace() = default;

std::optional<std::array<RenderTarget, 2>> BlurWorkspace::Get(
    const Context& context,
    ISize size) {
  if (size.IsEmpty()) {
    return std::nullopt;
  }
  const ISize current =
      targets_.has_value() ? (*targets_)[0].GetRenderTargetSize() : ISize();
  if (size.width <= current.width && size.height <= current.height) {
    return targets_;
  }
  const ISize maximum =
      context.GetCapabilities()->GetMaximumRenderPassAttachmentSize();
  if (size.width > maximum.width || size.height > maximum.height) {
    return std::nullopt;
  }
  const ISize required =
      current.Max(ISize(RoundUp(size.width), RoundUp(size.height)))
          .Min(maximum);
  // Every pass overwrites the region it later reads, so loading the previous
  // contents costs nothing and avoids clearing the whole target.
  const RenderTarget::AttachmentConfig color = {
      .storage_mode = StorageMode::kDevicePrivate,
      .load_action = LoadAction::kLoad,
      .store_action = StoreAction::kStore,
      .clear_color = Color::BlackTransparent(),
  };
  RenderTargetAllocator allocator(context.GetResourceAllocator());
  std::array<RenderTarget, 2> targets;
  for (RenderTarget& target : targets) {
    target = allocator.CreateOffscreen(
        context, required, /*mip_count=*/1, "Denial Blur Workspace", color,
        /*stencil_attachment_config=*/std::nullopt);
    if (!target.IsValid()) {
      return std::nullopt;
    }
  }
  targets_ = std::move(targets);
  return targets_;
}

}  // namespace impeller
