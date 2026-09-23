// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/pass_bindings_gles.h"

namespace impeller {

PassBindingsGLES::~PassBindingsGLES() {
  for (size_t i = 0; i < attributes_.size(); i++) {
    if (attributes_[i].enabled) {
      gl_.DisableVertexAttribArray(i);
    }
  }
  // Do not leave native samplers overriding texture parameters in external GL
  // or in a subsequent legacy sampling path.
  for (size_t i = 0; i < samplers_.size(); i++) {
    if (samplers_[i].value_or(0) != 0) {
      gl_.BindSampler(i, 0);
    }
  }
}

void PassBindingsGLES::ActiveTexture(GLuint unit) {
  if (active_texture_ != unit) {
    gl_.ActiveTexture(GL_TEXTURE0 + unit);
    active_texture_ = unit;
  }
}

void PassBindingsGLES::BindSampler(GLuint unit, GLuint sampler) {
  if (!supports_samplers_) {
    return;
  }
  if (samplers_.size() <= unit) {
    samplers_.resize(unit + 1);
  }
  if (samplers_[unit] != sampler) {
    gl_.BindSampler(unit, sampler);
    samplers_[unit] = sampler;
  }
}

void PassBindingsGLES::BeginDraw() {
  for (auto& attribute : attributes_) {
    attribute.needed = false;
  }
}

void PassBindingsGLES::EnableVertexAttribute(GLuint index, GLuint divisor) {
  if (attributes_.size() <= index) {
    attributes_.resize(index + 1);
  }
  auto& attribute = attributes_[index];
  attribute.needed = true;
  if (!attribute.enabled) {
    gl_.EnableVertexAttribArray(index);
    attribute.enabled = true;
  }
  if (attribute.divisor != divisor) {
    if (gl_.VertexAttribDivisor.IsAvailable()) {
      gl_.VertexAttribDivisor(index, divisor);
    } else if (gl_.VertexAttribDivisorEXT.IsAvailable()) {
      gl_.VertexAttribDivisorEXT(index, divisor);
    }
    attribute.divisor = divisor;
  }
}

void PassBindingsGLES::EndVertexSetup() {
  for (size_t i = 0; i < attributes_.size(); i++) {
    auto& attribute = attributes_[i];
    if (attribute.enabled && !attribute.needed) {
      gl_.DisableVertexAttribArray(i);
      attribute.enabled = false;
    }
  }
}

}  // namespace impeller
