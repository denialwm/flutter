// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "flutter/flow/layers/window_surface_layer.h"
#include "flutter/common/graphics/texture.h"
namespace flutter {
WindowSurfaceLayer::WindowSurfaceLayer(DlWindowSurfaceFilter::Style style,
                                       std::shared_ptr<DlImageFilter> backdrop,
                                       int64_t texture_id,
                                       DlRect texture_bounds,
                                       DlMatrix texture_transform,
                                       DlImageSampling sampling,
                                       DlScalar surface_opacity)
    : style_(style),
      backdrop_(std::move(backdrop)),
      material_(std::make_shared<DlWindowSurfaceFilter>(style_,
                                                        backdrop_,
                                                        texture_id >= 0)),
      texture_id_(texture_id),
      texture_bounds_(texture_bounds),
      texture_transform_(texture_transform),
      sampling_(sampling),
      surface_opacity_(surface_opacity) {}
void WindowSurfaceLayer::Diff(DiffContext* context, const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  const auto* prev = static_cast<const WindowSurfaceLayer*>(old_layer);
  const bool material_changed = !prev || NotEquals(backdrop_, prev->backdrop_);
  if (!context->IsSubtreeDirty() && prev &&
      (!(style_ == prev->style_) || material_changed ||
       texture_id_ != prev->texture_id_ ||
       texture_bounds_ != prev->texture_bounds_ ||
       texture_transform_ != prev->texture_transform_ ||
       sampling_ != prev->sampling_ ||
       surface_opacity_ != prev->surface_opacity_)) {
    context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(prev));
  }
  context->AddLayerBounds(style_.bounds.Expand(1));
  if (backdrop_) {
    // Match Canvas's material scope, not the outer frame or its AA fringe.
    const auto mapped = context->MapRect(style_.content_bounds);
    const auto target = DlIRect::RoundOut(mapped);
    const bool compatible = prev && prev->cache_prepared_ &&
                            !material_changed &&
                            target == prev->cache_target_ &&
                            context->GetMatrix() == prev->cache_matrix_;
    if (compatible) {
      cache_ = prev->cache_;
    }
    cache_ = context->RegisterBackdropFilterCache(std::nullopt,
                                                  std::move(cache_), target);
    if (!compatible || context->BackdropInputIsDirty(target)) {
      cache_->Invalidate();
    }
    cache_target_ = target;
    cache_matrix_ = context->GetMatrix();
    cache_prepared_ = true;
    context->AddReadbackRegion(target, target, cache_, mapped);
  }
  if (texture_id_ >= 0) {
    context->MarkSubtreeHasTextureLayer();
    if (!context->IsSubtreeDirty() && context->IsTextureDirty(texture_id_)) {
      context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(prev));
    }
    DiffContext::AutoSubtreeRestore client(context);
    context->PushCullRect(style_.content_bounds);
    context->AddTextureLayerBounds(style_.content_bounds);
    context->CacheTexturePaintRegion(texture_id_,
                                     context->CurrentSubtreeRegion());
  } else {
    context->PushCullRect(style_.content_bounds);
    DiffChildren(context, prev);
  }
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}
void WindowSurfaceLayer::Preroll(PrerollContext* context) {
  denial_cut_ = -1;
  denial_end_cut_ = -1;
  if (context->view_embedder &&
      context->view_embedder->DenialLayeredComposition()) {
    // The material reads the composed scene below this window, so the window
    // is a cut: the embedder composes it after the slice recorded so far.
    context->has_texture_layer = true;
    MutatorsStack mutators;
    context->state_stack.fill(&mutators);
    DenialCutEntry entry;
    entry.window = material_;
    entry.window_bounds = style_.bounds;
    entry.content_bounds = style_.content_bounds;
    if (cache_prepared_) {
      entry.backdrop_cache = cache_;
    }
    if (texture_id_ >= 0) {
      entry.kind = DenialCutEntry::Kind::kWindowSurface;
      entry.texture_id = texture_id_;
      entry.texture_bounds = texture_bounds_;
      entry.texture_transform = texture_transform_;
      entry.sampling = sampling_;
      entry.surface_opacity = surface_opacity_;
    } else {
      entry.kind = DenialCutEntry::Kind::kMaterialBegin;
      entry.filter = material_;
      entry.bounds = style_.bounds;
      entry.blend_mode = DlBlendMode::kSrc;
      entry.content_clip = style_.content_bounds;
    }
    denial_cut_ = context->view_embedder->DenialRegisterCut(
        std::make_unique<EmbeddedViewParams>(context->state_stack.matrix(),
                                             style_.bounds.GetSize(), mutators),
        std::move(entry));
    if (texture_id_ < 0) {
      auto clip = context->state_stack.save();
      clip.clipRect(style_.content_bounds, false);
      DlRect child_bounds;
      PrerollChildren(context, &child_bounds);
      DenialCutEntry end;
      end.kind = DenialCutEntry::Kind::kMaterialEnd;
      end.begin_cut = denial_cut_;
      MutatorsStack end_mutators;
      context->state_stack.fill(&end_mutators);
      denial_end_cut_ = context->view_embedder->DenialRegisterCut(
          std::make_unique<EmbeddedViewParams>(context->state_stack.matrix(),
                                               style_.bounds.GetSize(),
                                               end_mutators),
          std::move(end));
    }
    set_paint_bounds(style_.bounds.Expand(1));
    context->renderable_state_flags = LayerStateStack::kCallerCanApplyOpacity;
    return;
  }
  auto saved =
      Layer::AutoPrerollSaveLayerState::Create(context, true, bool{backdrop_});
  if (texture_id_ >= 0) {
    context->has_texture_layer = true;
  } else {
    auto clip = context->state_stack.save();
    clip.clipRect(style_.content_bounds, false);
    DlRect child_bounds;
    PrerollChildren(context, &child_bounds);
  }
  set_paint_bounds(style_.bounds.Expand(1));
  context->renderable_state_flags = LayerStateStack::kCallerCanApplyOpacity;
}
static void DenialContinueInSlice(PaintContext& context, int64_t cut) {
  DlCanvas* canvas = context.view_embedder->CompositeEmbeddedView(cut);
  context.canvas = canvas;
  context.state_stack.set_delegate(canvas);
}
void WindowSurfaceLayer::Paint(PaintContext& context) const {
  FML_DCHECK(needs_painting(context));
  if (denial_cut_ >= 0 && context.view_embedder) {
    DenialContinueInSlice(context, denial_cut_);
    if (texture_id_ < 0) {
      {
        auto state = context.state_stack.save();
        state.clipRect(style_.content_bounds, false);
        PaintChildren(context);
      }
      DenialContinueInSlice(context, denial_end_cut_);
    }
    return;
  }
  const auto token =
      cache_prepared_ ? std::make_optional(cache_->token()) : std::nullopt;
  auto applied = context.state_stack.applyState(
      paint_bounds(), LayerStateStack::kCallerCanApplyOpacity);
  if (texture_id_ >= 0) {
    DlPaint paint;
    context.state_stack.fill(paint);
    auto texture = context.texture_registry
                       ? context.texture_registry->GetTexture(texture_id_)
                       : nullptr;
    if (texture) {
      Texture::PaintContext ctx{context.canvas, context.gr_context,
                                context.aiks_context, &paint};
      texture->PaintWindow(ctx, texture_bounds_, texture_transform_, *material_,
                           token, sampling_, surface_opacity_);
    } else {
      paint.setBlendMode(DlBlendMode::kSrc);
      context.canvas->SaveLayer(style_.bounds, &paint, material_.get(), token);
      context.canvas->Restore();
    }
  } else {
    auto state = context.state_stack.save();
    state.applyBackdropFilter(paint_bounds(), material_, DlBlendMode::kSrc,
                              token);
    state.clipRect(style_.content_bounds, false);
    PaintChildren(context);
  }
}
}  // namespace flutter
