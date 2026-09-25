// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
precision highp float;
#include <impeller/constants.glsl>
#include <impeller/types.glsl>
uniform f16sampler2D surface_texture_sampler;
vec4 sampleSurface(vec2 uv) {
  return vec4(texture(surface_texture_sampler, uv, float16_t(kDefaultMipBias)));
}
#include "window_surface.glsl"
