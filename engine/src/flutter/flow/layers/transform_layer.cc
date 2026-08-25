// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/layers/transform_layer.h"

#include <optional>

namespace flutter {

TransformLayer::TransformLayer(const DlMatrix& transform)
    : transform_(transform) {
  FML_DCHECK(transform_.IsFinite());
  if (!transform_.IsFinite()) {
    FML_LOG(ERROR) << "TransformLayer is constructed with an invalid matrix.";
    transform_ = {};
  }
}

void TransformLayer::Diff(DiffContext* context, const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  auto* prev = static_cast<const TransformLayer*>(old_layer);
  if (!context->IsSubtreeDirty()) {
    FML_DCHECK(prev);
    if (transform_ != prev->transform_) {
      context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(old_layer));
    }
  }
  context->PushTransform(transform_);
  DiffChildren(context, prev);
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

void TransformLayer::Preroll(PrerollContext* context) {
  auto mutator = context->state_stack.save();
  mutator.transform(transform_);

  DlRect child_paint_bounds;
  PrerollChildren(context, &child_paint_bounds);

  child_paint_bounds = child_paint_bounds.TransformAndClipBounds(transform_);
  set_paint_bounds(child_paint_bounds);
}

void TransformLayer::Paint(PaintContext& context) const {
  FML_DCHECK(needs_painting(context));

  auto mutator = context.state_stack.save();
  mutator.transform(transform_);

  PaintChildren(context);
}

OutputRelativeTransformLayer::OutputRelativeTransformLayer(
    const DlPoint& offset_factor,
    const DlSize& fallback_size)
    : offset_factor_(offset_factor), fallback_size_(fallback_size) {
  if (!offset_factor_.IsFinite()) {
    FML_LOG(ERROR) << "OutputRelativeTransformLayer received a non-finite "
                      "offset factor.";
    offset_factor_ = {};
  }
  if (!fallback_size_.IsFinite() || fallback_size_.IsEmpty()) {
    FML_LOG(ERROR) << "OutputRelativeTransformLayer received an invalid "
                      "fallback size.";
    fallback_size_ = DlSize(1, 1);
  }
}

DlMatrix OutputRelativeTransformLayer::ResolveTransform(
    const std::optional<DlSize>& output_size) const {
  const DlSize size = output_size.value_or(fallback_size_);
  return DlMatrix::MakeTranslation(
      {offset_factor_.x * size.width, offset_factor_.y * size.height});
}

void OutputRelativeTransformLayer::Diff(DiffContext* context,
                                        const Layer* old_layer) {
  DiffContext::AutoSubtreeRestore subtree(context);
  auto* previous = static_cast<const OutputRelativeTransformLayer*>(old_layer);
  if (!context->IsSubtreeDirty()) {
    FML_DCHECK(previous);
    if (offset_factor_ != previous->offset_factor_ ||
        fallback_size_ != previous->fallback_size_) {
      context->MarkSubtreeDirty(context->GetOldLayerPaintRegion(old_layer));
    }
  }
  context->PushTransform(
      ResolveTransform(context->denial_render_output_logical_size()));
  DiffChildren(context, previous);
  context->SetLayerPaintRegion(this, context->CurrentSubtreeRegion());
}

void OutputRelativeTransformLayer::Preroll(PrerollContext* context) {
  const DlMatrix transform =
      ResolveTransform(context->denial_render_output_logical_size);
  auto mutator = context->state_stack.save();
  mutator.transform(transform);

  DlRect child_paint_bounds;
  PrerollChildren(context, &child_paint_bounds);

  set_paint_bounds(child_paint_bounds.TransformAndClipBounds(transform));
}

void OutputRelativeTransformLayer::Paint(PaintContext& context) const {
  FML_DCHECK(needs_painting(context));

  auto mutator = context.state_stack.save();
  mutator.transform(
      ResolveTransform(context.denial_render_output_logical_size));

  PaintChildren(context);
}

}  // namespace flutter
