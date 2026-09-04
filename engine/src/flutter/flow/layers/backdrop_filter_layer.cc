// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/backdrop_filter_layer.h"

namespace flutter {

BackdropFilterLayer::BackdropFilterLayer(
    const std::shared_ptr<DlImageFilter>& filter,
    DlBlendMode blend_mode,
    std::optional<int64_t> backdrop_id)
    : filter_(filter), blend_mode_(blend_mode), backdrop_id_(backdrop_id) {}

void BackdropFilterLayer::Diff(DiffContext* context, const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  auto* prev = static_cast<const BackdropFilterLayer*>(old_layer);
  const bool filter_changed = prev && NotEquals(filter_, prev->filter_);

  // Backdrop filter paints everywhere in cull rect
  auto paint_bounds = context->GetCullRect();

  if (filter_) {
    auto mapped_paint_bounds = context->MapRect(paint_bounds);
    auto filter_target_bounds = DlIRect::RoundOut(mapped_paint_bounds);
    DlIRect filter_input_bounds;  // in screen coordinates
    filter_->get_input_device_bounds(filter_target_bounds, context->GetMatrix(),
                                     filter_input_bounds);

    const bool compatible =
        prev && prev->backdrop_cache_prepared_ && !filter_changed &&
        filter_target_bounds == prev->backdrop_cache_target_ &&
        filter_input_bounds == prev->backdrop_cache_input_ &&
        context->GetMatrix() == prev->backdrop_cache_matrix_;
    if (compatible) {
      backdrop_cache_state_ = prev->backdrop_cache_state_;
    }
    backdrop_cache_state_ = context->RegisterBackdropFilterCache(
        backdrop_id_, std::move(backdrop_cache_state_), filter_input_bounds);
    if (!compatible || context->BackdropInputIsDirty(filter_input_bounds)) {
      backdrop_cache_state_->Invalidate();
    }
    backdrop_cache_target_ = filter_target_bounds;
    backdrop_cache_input_ = filter_input_bounds;
    backdrop_cache_matrix_ = context->GetMatrix();
    backdrop_cache_prepared_ = true;

    if (filter_changed && !context->IsSubtreeDirty()) {
      context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(old_layer));
    }
    context->AddLayerBounds(paint_bounds);
    context->AddReadbackRegion(filter_target_bounds, filter_input_bounds,
                               backdrop_id_ ? nullptr : backdrop_cache_state_);
  } else {
    context->AddLayerBounds(paint_bounds);
  }

  DiffChildren(context, prev);

  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

void BackdropFilterLayer::Preroll(PrerollContext* context) {
  Layer::AutoPrerollSaveLayerState save =
      Layer::AutoPrerollSaveLayerState::Create(context, true, bool{filter_});
  if (filter_ && context->view_embedder != nullptr) {
    context->view_embedder->PushFilterToVisitedPlatformViews(
        filter_, context->state_stack.device_cull_rect());
  }
  DlRect child_paint_bounds;
  PrerollChildren(context, &child_paint_bounds);
  child_paint_bounds =
      child_paint_bounds.Union(context->state_stack.local_cull_rect());
  set_paint_bounds(child_paint_bounds);
  context->renderable_state_flags = kSaveLayerRenderFlags;
}

void BackdropFilterLayer::Paint(PaintContext& context) const {
  FML_DCHECK(needs_painting(context));

  auto mutator = context.state_stack.save();
  mutator.applyBackdropFilter(
      paint_bounds(), filter_, blend_mode_,
      backdrop_cache_prepared_
          ? std::make_optional(backdrop_cache_state_->token())
          : backdrop_id_);

  PaintChildren(context);
}

}  // namespace flutter
