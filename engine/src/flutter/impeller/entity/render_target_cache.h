// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_
#define FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_

#include <string_view>

#include "impeller/renderer/render_target.h"

namespace impeller {

/// @brief An implementation of the [RenderTargetAllocator] that caches all
///        allocated texture data for at least one frame.
///
///        Unused targets survive brief gaps in rendering in a bounded idle
///        pool. The frame count protects the recent working set before targets
///        compete for that pool. A zero idle budget restores frame-only expiry.
class RenderTargetCache : public RenderTargetAllocator {
 public:
  explicit RenderTargetCache(std::shared_ptr<Allocator> allocator,
                             uint32_t keep_alive_frame_count = 4,
                             size_t max_idle_bytes = 32u * 1024u * 1024u);

  ~RenderTargetCache() = default;

  // |RenderTargetAllocator|
  void Start() override;

  // |RenderTargetAllocator|
  void End() override;

  // |RenderTargetAllocator|
  void DisableCache() override;

  // |RenderTargetAllocator|
  void EnableCache() override;

  RenderTarget CreateOffscreen(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label = "Offscreen",
      RenderTarget::AttachmentConfig color_attachment_config =
          RenderTarget::kDefaultColorAttachmentConfig,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config =
          RenderTarget::kDefaultStencilAttachmentConfig,
      const std::shared_ptr<Texture>& existing_color_texture = nullptr,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture = nullptr,
      std::optional<PixelFormat> target_pixel_format = std::nullopt) override;

  RenderTarget CreateOffscreenMSAA(
      const Context& context,
      ISize size,
      int mip_count,
      std::string_view label = "Offscreen MSAA",
      RenderTarget::AttachmentConfigMSAA color_attachment_config =
          RenderTarget::kDefaultColorAttachmentConfigMSAA,
      std::optional<RenderTarget::AttachmentConfig> stencil_attachment_config =
          RenderTarget::kDefaultStencilAttachmentConfig,
      const std::shared_ptr<Texture>& existing_color_msaa_texture = nullptr,
      const std::shared_ptr<Texture>& existing_color_resolve_texture = nullptr,
      const std::shared_ptr<Texture>& existing_depth_stencil_texture = nullptr,
      std::optional<PixelFormat> target_pixel_format = std::nullopt) override;

  // visible for testing.
  size_t CachedTextureCount() const;

 private:
  struct ColorConfig {
    PixelFormat format;
    StorageMode storage_mode;
    StorageMode resolve_storage_mode;

    bool operator==(const ColorConfig&) const = default;
  };

  struct RenderTargetData {
    RenderTargetConfig config;
    ColorConfig color_config;
    uint64_t last_used_frame = 0;
    // Keep only the resources. Each request constructs its own attachment
    // actions, and depth and stencil share the same texture. Vector compaction
    // moves these references rather than copying a full RenderTarget.
    std::shared_ptr<Texture> color_texture;
    std::shared_ptr<Texture> resolve_texture;
    std::shared_ptr<Texture> depth_stencil_texture;
    size_t byte_size = 0;
    bool pending_eviction = false;
    // Optional glass experiment with a separate idle retention budget.
    size_t motion_retained_bytes = 0;
    int64_t motion_last_used_us = 0;
  };

  bool CacheEnabled() const;

  std::vector<RenderTargetData> render_target_data_;
  // Indices remain valid until End finishes selection and compacts the cache.
  // Keep scratch capacity so a steady workload does not allocate every frame.
  std::vector<size_t> idle_candidates_;
  uint64_t frame_number_ = 0;
  const size_t max_idle_bytes_;
  uint32_t keep_alive_frame_count_;
  uint32_t cache_disabled_count_ = 0;

  RenderTargetCache(const RenderTargetCache&) = delete;

  RenderTargetCache& operator=(const RenderTargetCache&) = delete;

 public:
  /// Visible for testing.
  std::vector<RenderTargetData>::const_iterator GetRenderTargetDataBegin()
      const {
    return render_target_data_.begin();
  }

  /// Visible for testing.
  std::vector<RenderTargetData>::const_iterator GetRenderTargetDataEnd() const {
    return render_target_data_.end();
  }
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_RENDER_TARGET_CACHE_H_
