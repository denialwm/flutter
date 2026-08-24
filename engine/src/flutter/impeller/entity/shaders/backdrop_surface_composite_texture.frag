// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision highp float;

#include <impeller/constants.glsl>
#include <impeller/types.glsl>

uniform f16sampler2D surface_texture_sampler;

vec4 sampleSurface(highp vec2 coords) {
  return vec4(texture(surface_texture_sampler, coords,
                      float16_t(kDefaultMipBias)));
}

#include "backdrop_surface_composite.glsl"
