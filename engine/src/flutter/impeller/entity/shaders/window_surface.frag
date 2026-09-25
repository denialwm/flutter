// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
precision highp float;
#include <impeller/constants.glsl>
#include <impeller/external_texture_oes.glsl>
#include <impeller/types.glsl>
uniform sampler2D SAMPLER_EXTERNAL_OES_surface_texture_sampler;
vec4 sampleSurface(vec2 uv) {
  return texture(SAMPLER_EXTERNAL_OES_surface_texture_sampler, uv);
}
#include "window_surface.glsl"
