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

size_t AttachmentBytes(const RenderTarget& target) {
  const auto color = target.GetColorAttachment(0);
  const auto& depth = target.GetDepthAttachment();
  const auto& stencil = target.GetStencilAttachment();
  const std::array<std::shared_ptr<Texture>, 4> textures = {
      color.texture, color.resolve_texture, depth ? depth->texture : nullptr,
      stencil ? stencil->texture : nullptr};
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

}  // namespace

RenderTargetCache::RenderTargetCache(std::shared_ptr<Allocator> allocator,
                                     uint32_t keep_alive_frame_count)
    : RenderTargetAllocator(std::move(allocator)),
      keep_alive_frame_count_(keep_alive_frame_count) {}

void RenderTargetCache::Start() {
  cache_disabled_count_ = 0;
  for (auto& td : render_target_data_) {
    td.used_this_frame = false;
  }
}

void RenderTargetCache::End() {
  cache_disabled_count_ = 0;
  std::vector<RenderTargetData> retain;
  std::vector<RenderTargetData*> motion_idle;
  int64_t now_us = 0;
  auto now = [&] {
    if (now_us == 0) {
      now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
    }
    return now_us;
  };

  for (RenderTargetData& td : render_target_data_) {
    if (td.used_this_frame) {
      if (td.motion_retained_bytes > 0) {
        td.motion_last_used_us = now();
      }
      retain.push_back(td);
    } else if (td.keep_alive_frame_count > 0) {
      td.keep_alive_frame_count--;
      retain.push_back(td);
    } else if (td.motion_retained_bytes > 0 &&
               now() - td.motion_last_used_us <= 2500000) {
      motion_idle.push_back(&td);
    }
  }
  // A moving clipped layer revisits a small set of padded sizes. Keeping
  // these GL objects across the return path avoids relying on a driver's
  // whole-second BO cache expiry. Limit extra retention independently of the
  // existing active/four-frame cache, and prefer the most recently used sizes.
  std::sort(motion_idle.begin(), motion_idle.end(),
            [](const auto* a, const auto* b) {
              return a->motion_last_used_us > b->motion_last_used_us;
            });
  constexpr size_t kMotionIdleBudget = 256u * 1024u * 1024u;
  size_t retained_bytes = 0;
  for (const auto* td : motion_idle) {
    if (td->motion_retained_bytes <= kMotionIdleBudget - retained_bytes) {
      retained_bytes += td->motion_retained_bytes;
      retain.push_back(*td);
    }
  }
  render_target_data_.swap(retain);
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

  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      const RenderTargetConfig other_config = render_target_data.config;
      if (!render_target_data.used_this_frame && other_config == config) {
        render_target_data.used_this_frame = true;
        render_target_data.keep_alive_frame_count = keep_alive_frame_count_;
        ColorAttachment color0 =
            render_target_data.render_target.GetColorAttachment(0);
        std::optional<DepthAttachment> depth =
            render_target_data.render_target.GetDepthAttachment();
        std::shared_ptr<Texture> depth_tex = depth ? depth->texture : nullptr;
        return RenderTargetAllocator::CreateOffscreen(
            context, size, mip_count, label, color_attachment_config,
            stencil_attachment_config, color0.texture, depth_tex,
            target_pixel_format);
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
    render_target_data_.push_back(RenderTargetData{
        .used_this_frame = true,                            //
        .keep_alive_frame_count = keep_alive_frame_count_,  //
        .config = config,                                   //
        .render_target = created_target                     //
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
  if (CacheEnabled()) {
    for (RenderTargetData& render_target_data : render_target_data_) {
      const RenderTargetConfig other_config = render_target_data.config;
      if (!render_target_data.used_this_frame && other_config == config) {
        render_target_data.used_this_frame = true;
        render_target_data.keep_alive_frame_count = keep_alive_frame_count_;
        ColorAttachment color0 =
            render_target_data.render_target.GetColorAttachment(0);
        std::optional<DepthAttachment> depth =
            render_target_data.render_target.GetDepthAttachment();
        std::shared_ptr<Texture> depth_tex = depth ? depth->texture : nullptr;
        return RenderTargetAllocator::CreateOffscreenMSAA(
            context, size, mip_count, label, color_attachment_config,
            stencil_attachment_config, color0.texture, color0.resolve_texture,
            depth_tex, target_pixel_format);
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
    const size_t motion_retained_bytes =
        IsGlassTargetRetentionRequested() &&
                label == "Denial pooled glass layer" &&
                context.GetBackendType() == Context::BackendType::kOpenGLES
            ? AttachmentBytes(created_target)
            : 0;
    render_target_data_.push_back(RenderTargetData{
        .used_this_frame = true,                            //
        .keep_alive_frame_count = keep_alive_frame_count_,  //
        .config = config,                                   //
        .render_target = created_target,                    //
        .motion_retained_bytes = motion_retained_bytes,     //
    });
  }
  return created_target;
}

size_t RenderTargetCache::CachedTextureCount() const {
  return render_target_data_.size();
}

}  // namespace impeller
