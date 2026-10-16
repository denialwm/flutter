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
    if (filter_->is_window_backdrop() && !backdrop_id_.has_value()) {
      // Canvas crops the source before blur/refraction, then clamps at that
      // texture's edges. The rounded pixel rectangle is the complete input.
      filter_input_bounds = filter_target_bounds;
    } else {
      filter_->get_input_device_bounds(
          filter_target_bounds, context->GetMatrix(), filter_input_bounds);
    }

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
                               backdrop_id_ ? nullptr : backdrop_cache_state_,
                               mapped_paint_bounds);
  } else {
    context->AddLayerBounds(paint_bounds);
  }

  DiffChildren(context, prev);

  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

void BackdropFilterLayer::Preroll(PrerollContext* context) {
  denial_cut_ = -1;
  denial_end_cut_ = -1;
  if (filter_ && context->view_embedder &&
      context->view_embedder->DenialLayeredComposition()) {
    // The filter reads the composed scene below it. Open the material as a
    // cut, record the children in their own slice, and close it after them.
    context->has_texture_layer = true;
    const DlRect bounds = context->state_stack.local_cull_rect();
    MutatorsStack mutators;
    context->state_stack.fill(&mutators);
    DenialCutEntry entry;
    entry.kind = DenialCutEntry::Kind::kMaterialBegin;
    entry.filter = filter_;
    entry.bounds = bounds;
    entry.blend_mode = blend_mode_;
    denial_cut_ = context->view_embedder->DenialRegisterCut(
        std::make_unique<EmbeddedViewParams>(context->state_stack.matrix(),
                                             bounds.GetSize(), mutators),
        std::move(entry));
    DlRect child_paint_bounds;
    PrerollChildren(context, &child_paint_bounds);
    DenialCutEntry end;
    end.kind = DenialCutEntry::Kind::kMaterialEnd;
    end.begin_cut = denial_cut_;
    MutatorsStack end_mutators;
    context->state_stack.fill(&end_mutators);
    denial_end_cut_ = context->view_embedder->DenialRegisterCut(
        std::make_unique<EmbeddedViewParams>(context->state_stack.matrix(),
                                             bounds.GetSize(), end_mutators),
        std::move(end));
    set_paint_bounds(child_paint_bounds.Union(bounds));
    context->renderable_state_flags = LayerStateStack::kCallerCanApplyOpacity;
    return;
  }
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
  if (denial_cut_ >= 0 && context.view_embedder) {
    DlCanvas* canvas =
        context.view_embedder->CompositeEmbeddedView(denial_cut_);
    context.canvas = canvas;
    context.state_stack.set_delegate(canvas);
    PaintChildren(context);
    canvas = context.view_embedder->CompositeEmbeddedView(denial_end_cut_);
    context.canvas = canvas;
    context.state_stack.set_delegate(canvas);
    return;
  }

  auto mutator = context.state_stack.save();
  mutator.applyBackdropFilter(
      paint_bounds(), filter_, blend_mode_,
      backdrop_cache_prepared_
          ? std::make_optional(backdrop_cache_state_->token())
          : backdrop_id_);

  PaintChildren(context);
}

}  // namespace flutter
