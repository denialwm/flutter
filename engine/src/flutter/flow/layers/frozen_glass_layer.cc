// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/frozen_glass_layer.h"

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/effects/image_filters/dl_glass_image_filter.h"

namespace flutter {

FrozenGlassLayer::FrozenGlassLayer(std::shared_ptr<DlImageFilter> glass,
                                   sk_sp<DlImage> frost,
                                   DlRect frost_rect,
                                   std::array<DlScalar, 4> overlay,
                                   DlScalar opacity)
    : glass_(std::move(glass)),
      frost_(std::move(frost)),
      frost_rect_(frost_rect),
      overlay_(overlay),
      opacity_(opacity) {}

void FrozenGlassLayer::Preroll(PrerollContext* context) {
  denial_cut_ = -1;
  denial_end_cut_ = -1;
  const DlGlassImageFilter* glass = glass_ ? glass_->asGlass() : nullptr;
  if (glass && frost_ && context->view_embedder &&
      context->view_embedder->DenialLayeredComposition()) {
    context->has_texture_layer = true;
    const DlRect bounds = glass->shape().GetBounds();
    MutatorsStack mutators;
    context->state_stack.fill(&mutators);
    DenialCutEntry entry;
    entry.kind = DenialCutEntry::Kind::kMaterialBegin;
    entry.filter = glass_;
    entry.bounds = bounds;
    entry.blend_mode = DlBlendMode::kSrcOver;
    entry.frozen_frost = frost_;
    entry.frozen_frost_rect = frost_rect_;
    entry.frozen_overlay = overlay_;
    entry.frozen_opacity = opacity_;
    entry.frozen_picture = picture_;
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
    context->renderable_state_flags = 0;
    return;
  }
  ContainerLayer::Preroll(context);
}

void FrozenGlassLayer::Paint(PaintContext& context) const {
  FML_DCHECK(needs_painting(context));
  if (denial_cut_ < 0 || !context.view_embedder) {
    PaintChildren(context);
    return;
  }
  // The cut ends the slice below. The children go to a recording the
  // engine draws only if it composes the cut itself, under the transform
  // and clips the state stack replays into it.
  context.view_embedder->CompositeEmbeddedView(denial_cut_);
  DisplayListBuilder recorder;
  context.canvas = &recorder;
  context.state_stack.set_delegate(&recorder);
  PaintChildren(context);
  context.state_stack.clear_delegate();
  *picture_ = recorder.Build();
  DlCanvas* canvas =
      context.view_embedder->CompositeEmbeddedView(denial_end_cut_);
  context.canvas = canvas;
  context.state_stack.set_delegate(canvas);
}

}  // namespace flutter
