// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/flow/denial_retained_cache.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "flutter/display_list/dl_builder.h"
#include "flutter/flow/layers/container_layer.h"
#include "flutter/flow/layers/layer_state_stack.h"
#include "third_party/skia/include/core/SkColorSpace.h"

#if IMPELLER_SUPPORTS_RENDERING
#include "impeller/display_list/aiks_context.h"       // nogncheck
#include "impeller/display_list/dl_dispatcher.h"      // nogncheck
#include "impeller/display_list/dl_image_impeller.h"  // nogncheck
#include "impeller/renderer/context.h"                // nogncheck
#endif

namespace flutter {

namespace {

// This first retained path is deliberately bounded. Large or rotated
// categories use normal Flutter painting.
constexpr int64_t kMaxEntryPixels = 8 * 1024 * 1024;
constexpr int64_t kMaxCacheBytes = 64 * 1024 * 1024;
constexpr size_t kMaxEntries = 8;

}  // namespace

struct DenialRetainedCache::State {
#if IMPELLER_SUPPORTS_RENDERING
  struct Entry {
    uint64_t layer_id;
    DlMatrix parent_matrix;
    std::optional<DlSize> output_size;
    DlIRect bounds;
    std::shared_ptr<impeller::Texture> texture;
    int64_t bytes;
    uint64_t last_use;
  };

  std::weak_ptr<impeller::Context> context;
  std::vector<Entry> entries;
  int64_t bytes = 0;
  uint64_t clock = 0;
#endif
};

DenialRetainedCache::DenialRetainedCache() = default;
DenialRetainedCache::~DenialRetainedCache() = default;

void DenialRetainedCache::Clear() {
  state_.reset();
}

bool DenialRetainedCache::Draw(const DenialCategoryLayer& layer,
                               PaintContext& context) {
#if !IMPELLER_SUPPORTS_RENDERING
  return false;
#else
  if (!context.aiks_context || !context.canvas ||
      !layer.paint_bounds().IsFinite() || layer.paint_bounds().IsEmpty()) {
    return false;
  }

  const DlMatrix parent_matrix = context.state_stack.matrix();
  if (!parent_matrix.IsTranslationScaleOnly() || parent_matrix.m[0] <= 0 ||
      parent_matrix.m[5] <= 0 || !parent_matrix.IsFinite()) {
    return false;
  }

  const DlRect transformed =
      layer.paint_bounds().TransformAndClipBounds(parent_matrix);
  if (!transformed.IsFinite() || std::abs(transformed.GetLeft()) > 32768 ||
      std::abs(transformed.GetTop()) > 32768 ||
      std::abs(transformed.GetRight()) > 32768 ||
      std::abs(transformed.GetBottom()) > 32768) {
    return false;
  }

  const DlIRect bounds = DlIRect::RoundOut(layer.paint_bounds());
  const auto pixel_left = static_cast<int>(
      std::floor(parent_matrix.m[0] * bounds.GetLeft() + parent_matrix.m[12]));
  const auto pixel_top = static_cast<int>(
      std::floor(parent_matrix.m[5] * bounds.GetTop() + parent_matrix.m[13]));
  const auto pixel_right = static_cast<int>(
      std::ceil(parent_matrix.m[0] * bounds.GetRight() + parent_matrix.m[12]));
  const auto pixel_bottom = static_cast<int>(
      std::ceil(parent_matrix.m[5] * bounds.GetBottom() + parent_matrix.m[13]));
  const int64_t width = static_cast<int64_t>(pixel_right) - pixel_left;
  const int64_t height = static_cast<int64_t>(pixel_bottom) - pixel_top;
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
      width * height > kMaxEntryPixels) {
    return false;
  }
  const int64_t bytes = width * height * 4;

  auto impeller_context = context.aiks_context->GetContext();
  if (!state_) {
    state_ = std::make_unique<State>();
  }
  if (state_->context.lock() != impeller_context) {
    state_->entries.clear();
    state_->bytes = 0;
    state_->context = impeller_context;
  }

  const auto match = [&](const State::Entry& entry) {
    return entry.layer_id == layer.unique_id() &&
           entry.parent_matrix == parent_matrix &&
           entry.output_size == context.denial_render_output_logical_size &&
           entry.bounds == bounds;
  };
  auto found =
      std::find_if(state_->entries.begin(), state_->entries.end(), match);
  if (found == state_->entries.end()) {
    DisplayListBuilder builder(DlRect::MakeWH(width, height));
    builder.Clear(DlColor::kTransparent());
    builder.Translate(-pixel_left, -pixel_top);
    builder.Transform(parent_matrix);

    LayerStateStack child_state_stack;
    child_state_stack.set_delegate(&builder);
    PaintContext child_context = {
        .state_stack = child_state_stack,
        .canvas = &builder,
        .rendering_above_platform_view = context.rendering_above_platform_view,
        .gr_context = context.gr_context,
        .dst_color_space = context.dst_color_space,
        .view_embedder = context.view_embedder,
        .raster_time = context.raster_time,
        .ui_time = context.ui_time,
        .texture_registry = context.texture_registry,
#if !SLIMPELLER
        .raster_cache = nullptr,
#endif
        .impeller_enabled = true,
        .aiks_context = context.aiks_context,
        .denial_retained_cache = nullptr,
        .denial_render_output_logical_size =
            context.denial_render_output_logical_size,
    };
    layer.PaintChildren(child_context);

    auto texture = impeller::DisplayListToTexture(
        builder.Build(), impeller::ISize(width, height), *context.aiks_context,
        /*reset_host_buffer=*/false);
    if (!texture) {
      return false;
    }

    while (!state_->entries.empty() &&
           (state_->entries.size() >= kMaxEntries ||
            state_->bytes + bytes > kMaxCacheBytes)) {
      auto oldest =
          std::min_element(state_->entries.begin(), state_->entries.end(),
                           [](const State::Entry& a, const State::Entry& b) {
                             return a.last_use < b.last_use;
                           });
      state_->bytes -= oldest->bytes;
      state_->entries.erase(oldest);
    }
    state_->bytes += bytes;
    state_->entries.push_back(
        State::Entry{layer.unique_id(), parent_matrix,
                     context.denial_render_output_logical_size, bounds,
                     std::move(texture), bytes, ++state_->clock});
    found = std::prev(state_->entries.end());
  } else {
    found->last_use = ++state_->clock;
  }

  auto image = impeller::DlImageImpeller::Make(found->texture,
                                               DlImage::OwningContext::kRaster);
  auto restore = context.state_stack.applyState(layer.paint_bounds(),
                                                Layer::kRasterCacheRenderFlags);
  DlPaint paint;
  const DlRect destination = DlRect::MakeLTRB(
      (pixel_left - parent_matrix.m[12]) / parent_matrix.m[0],
      (pixel_top - parent_matrix.m[13]) / parent_matrix.m[5],
      (pixel_right - parent_matrix.m[12]) / parent_matrix.m[0],
      (pixel_bottom - parent_matrix.m[13]) / parent_matrix.m[5]);
  context.canvas->DrawImageRect(image, destination,
                                DlImageSampling::kNearestNeighbor,
                                context.state_stack.fill(paint));
  return true;
#endif
}

}  // namespace flutter
