// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_VK_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_VK_H_

#include <functional>
#include <memory>

#include "flutter/common/graphics/texture.h"
#include "flutter/fml/macros.h"
#include "flutter/shell/platform/embedder/embedder.h"

namespace impeller {
class TextureSourceVK;
}

namespace flutter {

class EmbedderExternalTextureVK final : public flutter::Texture {
 public:
  using ExternalTextureCallback = std::function<
      std::unique_ptr<FlutterVulkanExternalTexture>(int64_t, size_t, size_t)>;

  EmbedderExternalTextureVK(int64_t texture_identifier,
                            const ExternalTextureCallback& callback);

  ~EmbedderExternalTextureVK() override;

 private:
  const ExternalTextureCallback& external_texture_callback_;
  sk_sp<DlImage> last_image_;
  std::shared_ptr<const impeller::TextureSourceVK> last_borrowed_source_;
  uint32_t last_external_layout_ = 0u;
  uint32_t last_external_queue_family_ = 0u;

  sk_sp<DlImage> ResolveTexture(int64_t texture_id,
                                impeller::AiksContext* aiks_context,
                                const DlISize& size);

  // |flutter::Texture|
  void Paint(PaintContext& context,
             const DlRect& bounds,
             bool freeze,
             const DlImageSampling sampling) override;

  // |flutter::Texture|
  void OnGrContextCreated() override;

  // |flutter::Texture|
  void OnGrContextDestroyed() override;

  // |flutter::Texture|
  void MarkNewFrameAvailable() override;

  // |flutter::Texture|
  void OnTextureUnregistered() override;

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderExternalTextureVK);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_VK_H_
