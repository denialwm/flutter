// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_PASS_BINDINGS_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_PASS_BINDINGS_GLES_H_

#include <optional>

#include "impeller/renderer/backend/gles/proc_table_gles.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace impeller {

// Context state is tracked only while a render pass executes. Entry state is
// unknown: external GL and resource operations may have changed it. The owning
// stack frame guarantees cleanup on both success and early return.
class PassBindingsGLES {
 public:
  explicit PassBindingsGLES(const ProcTableGLES& gl)
      : gl_(gl), supports_samplers_(gl.SupportsSamplerObjects()) {}
  ~PassBindingsGLES();

  PassBindingsGLES(const PassBindingsGLES&) = delete;
  PassBindingsGLES& operator=(const PassBindingsGLES&) = delete;

  void ActiveTexture(GLuint unit);
  void BindSampler(GLuint unit, GLuint sampler);

  void BeginDraw();
  void EnableVertexAttribute(GLuint index, GLuint divisor);
  void EndVertexSetup();

 private:
  struct Attribute {
    bool enabled = false;
    bool needed = false;
    std::optional<GLuint> divisor;
  };
  const ProcTableGLES& gl_;
  const bool supports_samplers_;
  std::optional<GLuint> active_texture_;
  absl::InlinedVector<std::optional<GLuint>, 8> samplers_;
  absl::InlinedVector<Attribute, 16> attributes_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_PASS_BINDINGS_GLES_H_
