// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/entity/render_target_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>

#include "impeller/core/formats.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_target.h"

namespace impeller {

namespace {

bool IsGlassTargetRetentionRequested() {
  static const bool requested = [] {
    const char* value = std::getenv("DENIA_GLASS_RETAIN_TARGETS");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return requested;
}

bool IsGlassTargetRetentionByBudgetRequested() {
  static const bool requested = [] {
    const char* value = std::getenv("DENIA_GLASS_RETAIN_TARGETS_BY_BUDGET");
    return value && value[0] == '1' && value[1] == '\0';
  }();
  return requested;
}

size_t AttachmentBytes(const std::shared_ptr<Texture>& color,
                       const std::shared_ptr<Texture>& resolve,
                       const std::shared_ptr<Texture>& depth_stencil) {
  const std::array<Texture*, 3> textures = {color.get(), resolve.get(),
                                            depth_stencil.get()};
  size_t bytes = 0;
  for (size_t i = 0; i < textures.size(); i++) {
    if (!textures[i] || std::find(textures.begin(), textures.begin() + i,
                                  textures[i]) != textures.begin() + i) {
      continue;
    }
    const auto& desc = textures[i]->GetTextureDescriptor();
    // Include the sample count, but count aliased depth/stencil and implicit
    // resolve views only once. This is a storage estimate, not a driver query.
    bytes += desc.GetByteSizeOfAllMipLevels() *
             static_cast<size_t>(desc.sample_count);
  }
  return bytes;
}

size_t MotionRetentionBytes(const Context& context,
                            std::string_view label,
                            size_t attachment_bytes) {
  return IsGlassTargetRetentionRequested() &&
                 (label == "Denial pooled glass layer" ||
                  label == "Denial pooled glass material") &&
                 context.GetBackendType() == Context::BackendType::kOpenGLES
             ? attachment_bytes
             : 0;
}

}  // namespace

RenderTargetCache::RenderTargetCache(std::shared_ptr<Allocator> allocator,
                                     uint32_t keep_alive_frame_count,
                                     size_t max_idle_bytes)
    : RenderTargetAllocator(std::move(allocator)),
      max_idle_bytes_(max_idle_bytes),
      keep_alive_frame_count_(keep_alive_frame_count) {}

void RenderTargetCache::Start() {
  cache_disabled_count_ = 0;
  frame_number_++;
}

void RenderTargetCache::End() {
  cache_disabled_count_ = 0;
  idle_candidates_.clear();
  constexpr size_t kMaxIdleTargets = 128u;
  size_t available_idle_bytes = max_idle_bytes_;
  bool idle_budget_fits = true;
  int64_t now_us = 0;
  auto now = [&] {
    if (now_us == 0) {
      now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
    }
    return now_us;
  };

  for (size_t i = 0; i < render_target_data_.size(); i++) {
    auto& td = render_target_data_[i];
    td.pending_eviction = false;
    if (td.last_used_frame == frame_number_) {
      if (td.motion_retained_bytes > 0) {
        td.motion_last_used_us = now();
      }
    } else if (frame_number_ - td.last_used_frame > keep_alive_frame_count_) {
      idle_candidates_.push_back(i);
      if (td.byte_size <= available_idle_bytes) {
        available_idle_bytes -= td.byte_size;
      } else {
        idle_budget_fits = false;
      }
    }
  }

  // The stable working set needs no sorting or compaction while it fits.
  if (idle_candidates_.empty() ||
      (max_idle_bytes_ > 0 && idle_budget_fits &&
       idle_candidates_.size() <= kMaxIdleTargets)) {
    return;
  }

  // Frame-count expiry alone makes occasional effects and slower outputs
  // repeatedly allocate the same targets at high submission rates. Retain
  // the most recently used idle targets within a fixed storage budget.
  std::sort(idle_candidates_.begin(), idle_candidates_.end(),
            [&](size_t a, size_t b) {
              return render_target_data_[a].last_used_frame >
                     render_target_data_[b].last_used_frame;
            });
  constexpr size_t kMotionIdleBudget = 256u * 1024u * 1024u;
  size_t idle_bytes = 0;
  size_t idle_count = 0;
  size_t motion_bytes = 0;
  for (size_t index : idle_candidates_) {
    auto& td = render_target_data_[index];
    td.pending_eviction = true;
    if (td.motion_retained_bytes > 0 &&
        (IsGlassTargetRetentionByBudgetRequested() ||
         now() - td.motion_last_used_us <= 2500000) &&
        td.motion_retained_bytes <= kMotionIdleBudget - motion_bytes) {
      // Preserve the explicitly enabled glass-retention experiment's budget.
      motion_bytes += td.motion_retained_bytes;
      td.pending_eviction = false;
    } else if (max_idle_bytes_ > 0 && idle_count < kMaxIdleTargets &&
               td.byte_size <= max_idle_bytes_ - idle_bytes) {
      idle_bytes += td.byte_size;
      idle_count++;
      td.pending_eviction = false;
    }
  }

  // Retained targets stay in place unless an earlier entry was evicted. This
  // avoids rebuilding a vector and copying all attachment references per frame.
  std::erase_if(render_target_data_,
                [](const auto& td) { return td.pending_eviction; });
}

void RenderTargetCache::DisableCache() {
  cache_disabled_count_++;
}

bool RenderTargetCache::CacheEnabled() const {
  return cache_disabled_count_ == 0;
}

void RenderTargetCache::EnableCache() {
  FML_DCHECK(cache_disabled_count_ > 0);
  if (cache_disabled_count_ == 0) {
    return;
  }
  cache_disabled_count_--;
}

RenderTarget RenderTargetCache::CreateOffscreen(
    const Context& context,
    ISize size,
    int mip_count,
    std::string_view label,
    RenderTarget::AttachmentConfig color_attachment_config,
    std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config,
    const std::shared_ptr<Texture>& existing_color_texture,
    const std::shared_ptr<Texture>& existing_depth_stencil_texture,
    std::optional<PixelFormat> target_pixel_format) {
  if (size.IsEmpty()) {
    return {};
  }

  FML_DCHECK(existing_color_texture == nullptr &&
             existing_depth_stencil_texture == nullptr);
  auto config = RenderTargetConfig{
      .size = size,
      .mip_count = static_cast<size_t>(mip_count),
      .has_msaa = false,
      .has_depth_stencil = stencil_attachment_config.has_value(),
      .depth_stencil_storage_mode =
          stencil_attachment_config.has_value()
              ? stencil_attachment_config->storage_mode
              : StorageMode::kDeviceTransient,
  };

  const ColorConfig color_config{
      target_pixel_format.value_or(
          context.GetCapabilities()->GetDefaultColorFormat()),
      color_attachment_config.storage_mode, StorageMode::kDeviceTransient};
  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      if (render_target_data.last_used_frame != frame_number_ &&
          render_target_data.config == config &&
          render_target_data.color_config == color_config) {
        render_target_data.last_used_frame = frame_number_;
        return RenderTargetAllocator::CreateOffscreen(
            context, size, mip_count, label, color_attachment_config,
            stencil_attachment_config, render_target_data.color_texture,
            render_target_data.depth_stencil_texture, target_pixel_format);
      }
    }
  }
  RenderTarget created_target = RenderTargetAllocator::CreateOffscreen(
      context, size, mip_count, label, color_attachment_config,
      stencil_attachment_config, nullptr, nullptr, target_pixel_format);
  if (!created_target.IsValid()) {
    return created_target;
  }
  if (CacheEnabled()) {
    const auto color = created_target.GetColorAttachment(0);
    const auto& depth = created_target.GetDepthAttachment();
    const size_t bytes = AttachmentBytes(color.texture, color.resolve_texture,
                                         depth ? depth->texture : nullptr);
    render_target_data_.push_back(RenderTargetData{
        .config = config,
        .color_config = color_config,
        .last_used_frame = frame_number_,
        .color_texture = color.texture,
        .resolve_texture = color.resolve_texture,
        .depth_stencil_texture = depth ? depth->texture : nullptr,
        .byte_size = bytes,
        .motion_retained_bytes = MotionRetentionBytes(context, label, bytes),
    });
  }
  return created_target;
}

RenderTarget RenderTargetCache::CreateOffscreenMSAA(
    const Context& context,
    ISize size,
    int mip_count,
    std::string_view label,
    RenderTarget::AttachmentConfigMSAA color_attachment_config,
    std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config,
    const std::shared_ptr<Texture>& existing_color_msaa_texture,
    const std::shared_ptr<Texture>& existing_color_resolve_texture,
    const std::shared_ptr<Texture>& existing_depth_stencil_texture,
    std::optional<PixelFormat> target_pixel_format) {
  if (size.IsEmpty()) {
    return {};
  }

  FML_DCHECK(existing_color_msaa_texture == nullptr &&
             existing_color_resolve_texture == nullptr &&
             existing_depth_stencil_texture == nullptr);
  auto config = RenderTargetConfig{
      .size = size,
      .mip_count = static_cast<size_t>(mip_count),
      .has_msaa = true,
      .has_depth_stencil = stencil_attachment_config.has_value(),
      .depth_stencil_storage_mode =
          stencil_attachment_config.has_value()
              ? stencil_attachment_config->storage_mode
              : StorageMode::kDeviceTransient,
  };
  const ColorConfig color_config{
      target_pixel_format.value_or(
          context.GetCapabilities()->GetDefaultColorFormat()),
      color_attachment_config.storage_mode,
      color_attachment_config.resolve_storage_mode};
  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      if (render_target_data.last_used_frame != frame_number_ &&
          render_target_data.config == config &&
          render_target_data.color_config == color_config) {
        render_target_data.last_used_frame = frame_number_;
        return RenderTargetAllocator::CreateOffscreenMSAA(
            context, size, mip_count, label, color_attachment_config,
            stencil_attachment_config, render_target_data.color_texture,
            render_target_data.resolve_texture,
            render_target_data.depth_stencil_texture, target_pixel_format);
      }
    }
  }
  RenderTarget created_target = RenderTargetAllocator::CreateOffscreenMSAA(
      context, size, mip_count, label, color_attachment_config,
      stencil_attachment_config, nullptr, nullptr, nullptr,
      target_pixel_format);
  if (!created_target.IsValid()) {
    return created_target;
  }
  if (CacheEnabled()) {
    const auto color = created_target.GetColorAttachment(0);
    const auto& depth = created_target.GetDepthAttachment();
    const size_t bytes = AttachmentBytes(color.texture, color.resolve_texture,
                                         depth ? depth->texture : nullptr);
    render_target_data_.push_back(RenderTargetData{
        .config = config,
        .color_config = color_config,
        .last_used_frame = frame_number_,
        .color_texture = color.texture,
        .resolve_texture = color.resolve_texture,
        .depth_stencil_texture = depth ? depth->texture : nullptr,
        .byte_size = bytes,
        .motion_retained_bytes = MotionRetentionBytes(context, label, bytes),
    });
  }
  return created_target;
}

size_t RenderTargetCache::CachedTextureCount() const {
  return render_target_data_.size();
}

}  // namespace impeller
