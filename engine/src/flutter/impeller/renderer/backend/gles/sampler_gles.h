// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_SAMPLER_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_SAMPLER_GLES_H_

#include "impeller/base/backend_cast.h"
#include "impeller/core/sampler.h"
#include "impeller/renderer/backend/gles/unique_handle_gles.h"

namespace impeller {

class TextureGLES;
class SamplerLibraryGLES;
class ProcTableGLES;

class SamplerGLES final : public Sampler,
                          public BackendCast<SamplerGLES, Sampler> {
 public:
  ~SamplerGLES();

  // Returns zero for configurations that must use texture parameters. Called
  // only while executing a render pass with a current context.
  GLuint GetNativeSampler(const TextureGLES& texture) const;

  bool ConfigureBoundTexture(const TextureGLES& texture,
                             const ProcTableGLES& gl) const;

 private:
  friend class SamplerLibraryGLES;

  explicit SamplerGLES(const SamplerDescriptor&, std::shared_ptr<ReactorGLES>);

  std::shared_ptr<ReactorGLES> reactor_;
  // Extension texture parameters need not be supported on sampler objects.
  // Decide compatibility once, retaining the legacy path for those samplers.
  const bool supports_native_sampler_;
  // Separate immutable effective filters for textures with and without mips.
  mutable UniqueHandleGLES native_samplers_[2];

  SamplerGLES(const SamplerGLES&) = delete;

  SamplerGLES& operator=(const SamplerGLES&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_SAMPLER_GLES_H_
