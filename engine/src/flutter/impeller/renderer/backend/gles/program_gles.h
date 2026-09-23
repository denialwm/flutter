// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_PROGRAM_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_PROGRAM_GLES_H_

#include <utility>

#include "impeller/renderer/backend/gles/unique_handle_gles.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"

namespace impeller {

// Pipeline variants share both the GL program and its uniform values. Programs
// are linked before publication; sampler uniforms are written only by the
// render executor. Replacing a program creates a fresh cache, even if GL reuses
// its name.
class ProgramGLES {
 public:
  explicit ProgramGLES(UniqueHandleGLES handle) : handle_(std::move(handle)) {}

  const HandleGLES& Get() const { return handle_.Get(); }
  bool IsValid() const { return handle_.IsValid(); }

  // Call with this program current, on the render executor. Uniform values
  // survive program switches and pass boundaries. No GL queries or allocations
  // are needed for the common case of up to four sampler uniforms.
  void SetSamplerUnit(const ProcTableGLES& gl, GLint location, GLint unit) {
    if (location < 0) {
      return;
    }
    for (auto& entry : sampler_units_) {
      if (entry.first == location) {
        if (entry.second != unit) {
          gl.Uniform1i(location, unit);
          entry.second = unit;
        }
        return;
      }
    }
    gl.Uniform1i(location, unit);
    sampler_units_.emplace_back(location, unit);
  }

 private:
  UniqueHandleGLES handle_;
  absl::InlinedVector<std::pair<GLint, GLint>, 4> sampler_units_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_PROGRAM_GLES_H_
