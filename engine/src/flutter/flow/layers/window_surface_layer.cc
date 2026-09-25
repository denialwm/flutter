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
void WindowSurfaceLayer::Paint(PaintContext& context) const {
  FML_DCHECK(needs_painting(context));
  const auto token =
      cache_prepared_ ? std::make_optional(cache_->token()) : std::nullopt;
  auto applied = context.state_stack.applyState(
      paint_bounds(), LayerStateStack::kCallerCanApplyOpacity);
  auto material = std::make_shared<DlWindowSurfaceFilter>(style_, backdrop_,
                                                          texture_id_ >= 0);
  if (texture_id_ >= 0) {
    DlPaint paint;
    context.state_stack.fill(paint);
    auto texture = context.texture_registry
                       ? context.texture_registry->GetTexture(texture_id_)
                       : nullptr;
    if (texture) {
      Texture::PaintContext ctx{context.canvas, context.gr_context,
                                context.aiks_context, &paint};
      texture->PaintWindow(ctx, texture_bounds_, texture_transform_, *material,
                           token, sampling_, surface_opacity_);
    } else {
      paint.setBlendMode(DlBlendMode::kSrc);
      context.canvas->SaveLayer(style_.bounds, &paint, material.get(), token);
      context.canvas->Restore();
    }
  } else {
    auto state = context.state_stack.save();
    state.applyBackdropFilter(paint_bounds(), material, DlBlendMode::kSrc,
                              token);
    state.clipRect(style_.content_bounds, false);
    PaintChildren(context);
  }
}
}  // namespace flutter
