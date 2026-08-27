// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

uniform f16sampler2D backdrop_texture_sampler;
uniform f16sampler2D scene_texture_sampler;

uniform FragInfo {
  vec4 clip_bounds;
  vec2 clip_radii;
  float surface_opacity;
  float backdrop_opacity;
  float alpha_threshold;
  float has_analytic_clip;
}
frag_info;

in highp vec2 v_surface_texture_coords;
in highp vec2 v_backdrop_texture_coords;
in highp vec2 v_scene_texture_coords;
in highp vec2 v_pass_position;

out vec4 frag_color;

float roundedRectCoverage() {
  if (frag_info.has_analytic_clip < 0.5) {
    return 1.0;
  }

  vec2 half_size =
      max((frag_info.clip_bounds.zw - frag_info.clip_bounds.xy) * 0.5,
          vec2(0.0001));
  vec2 center = (frag_info.clip_bounds.xy + frag_info.clip_bounds.zw) * 0.5;
  vec2 radii = clamp(frag_info.clip_radii, vec2(0.0001), half_size);
  vec2 q = (abs(v_pass_position - center) - (half_size - radii)) / radii;
  float distance = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - 1.0;
  float fade = max(fwidth(distance) * 0.5, 0.0001);
  return 1.0 - smoothstep(-fade, fade, distance);
}

void main() {
  // Sample the client first. A fully transparent client texel owns no part of
  // the backdrop scope, so leave the destination untouched without issuing a
  // blur or scene sample.
  vec4 surface = sampleSurface(v_surface_texture_coords);
  if (surface.a <= 0.0) {
    discard;
  }

  float coverage = roundedRectCoverage();
  if (coverage <= 0.0) {
    discard;
  }

  surface *= frag_info.surface_opacity;
  vec4 composite = surface;
  bool threshold_enabled = frag_info.alpha_threshold >= 0.0;
  float final_surface_alpha = surface.a * coverage;
  bool use_filtered_backdrop = !threshold_enabled ||
                               final_surface_alpha > frag_info.alpha_threshold;
  if (use_filtered_backdrop && surface.a < 1.0 - 1.0 / 1024.0) {
    vec4 backdrop = vec4(texture(backdrop_texture_sampler,
                                 v_backdrop_texture_coords,
                                 float16_t(kDefaultMipBias)));
    backdrop *= frag_info.backdrop_opacity;
    composite += backdrop * (1.0 - surface.a);
  }

  // Thresholded composites use source-over blending. Below the threshold,
  // writing only the client surface preserves the untouched destination.
  // Above it, this writes the complete surface-over-filtered-backdrop result.
  // Coverage can be applied directly because source-over supplies the scene at
  // antialiased clip edges without a second scene sample.
  if (threshold_enabled) {
    frag_color = composite * coverage;
    return;
  }

  // Source blending writes the complete logical result. Only antialiased clip
  // edges need the original scene; full-coverage pixels avoid that sample.
  if (coverage < 1.0 - 1.0 / 1024.0) {
    vec4 scene = vec4(texture(scene_texture_sampler, v_scene_texture_coords,
                              float16_t(kDefaultMipBias)));
    composite = mix(scene, composite, coverage);
  }
  frag_color = composite;
}
