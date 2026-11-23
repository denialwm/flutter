// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_RESOLVER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_RESOLVER_H_

#include <memory>

#include "flutter/common/graphics/texture.h"
#include "flutter/shell/platform/embedder/embedder.h"

#ifdef SHELL_ENABLE_GL
#include "flutter/shell/platform/embedder/embedder_external_texture_gl.h"
#endif

#ifdef SHELL_ENABLE_METAL
#include "flutter/shell/platform/embedder/embedder_external_texture_metal.h"
#endif

namespace flutter {

// Raster thread, Vulkan renderer: set while the engine composes an output
// frame the scene composer could not stage. Client textures resolve their
// images through it; outside a fallback they draw nothing.
struct DenialTextureLending {
  DenialFlutterSceneComposer composer;
  uint64_t ticket = 0;
  // Some texture resolved an image the composer must lend.
  bool resolved = false;
};

// Starts (non-null) or ends (null) the lending of the current fallback.
void DenialSetTextureLending(DenialTextureLending* lending);

class EmbedderExternalTextureResolver {
 public:
  EmbedderExternalTextureResolver() = default;

  ~EmbedderExternalTextureResolver() = default;

#ifdef SHELL_ENABLE_GL
  explicit EmbedderExternalTextureResolver(
      EmbedderExternalTextureGL::ExternalTextureCallback gl_callback);

  void SetExternalTextureGlStateCallback(
      EmbedderExternalTextureGL::ExternalTextureGlStateCallback callback);
  void SetExternalTexturePresentationCallback(
      EmbedderExternalTextureGL::ExternalTexturePresentationCallback callback);
#endif

#ifdef SHELL_ENABLE_METAL
  explicit EmbedderExternalTextureResolver(
      EmbedderExternalTextureMetal::ExternalTextureCallback metal_callback);
#endif

  std::unique_ptr<Texture> ResolveExternalTexture(int64_t texture_id);

  // Denial, Vulkan renderer: the scene composer draws every client texture
  // itself from the IDs the engine registers. Textures resolve to entries
  // that keep their registration, frame-available marks and damage, and
  // paint only the images the composer lends while the engine composes a
  // frame it could not (DenialSetTextureLending).
  void EnableDenialComposerTextures() { denial_composer_textures_ = true; }

  bool SupportsExternalTextures();

 private:
  bool denial_composer_textures_ = false;

#ifdef SHELL_ENABLE_GL
  EmbedderExternalTextureGL::ExternalTextureCallback gl_callback_;
  EmbedderExternalTextureGL::ExternalTextureGlStateCallback gl_state_callback_;
  EmbedderExternalTextureGL::ExternalTexturePresentationCallback
      presentation_callback_;
#endif

#ifdef SHELL_ENABLE_METAL
  EmbedderExternalTextureMetal::ExternalTextureCallback metal_callback_;
#endif

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderExternalTextureResolver);
};
}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_TEXTURE_RESOLVER_H_
