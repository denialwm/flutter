// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
uniform f16sampler2D backdrop_texture_sampler;
uniform FragInfo {
  vec4 bounds;
  vec4 content_bounds;
  vec4 source_limits;
  vec4 source_coverage;
  vec4 frame_color;
  float radius;
  float opacity;
  float surface_opacity;
  float backdrop_opacity;
  float alpha_threshold;
  float has_backdrop;
  float has_surface;
} frag_info;
in highp vec2 v_position;
in highp vec2 v_surface_uv;
in highp vec2 v_backdrop_uv;
out vec4 frag_color;

float roundedCoverage(vec4 bounds, float radius) {
  vec2 half_size = max((bounds.zw - bounds.xy) * 0.5, vec2(0.0));
  radius = clamp(radius, 0.0, min(half_size.x, half_size.y));
  vec2 q = abs(v_position - (bounds.xy + bounds.zw) * 0.5) - half_size + radius;
  float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
  // Derivatives keep the fringe one physical pixel wide during output-scale,
  // overview and window animation transforms, without stencil/MSAA passes.
  return clamp(0.5 - d / max(fwidth(d), 0.00001), 0.0, 1.0);
}
void main() {
  float outer = roundedCoverage(frag_info.bounds, frag_info.radius);
  float inset = max(frag_info.content_bounds.x - frag_info.bounds.x, 0.0);
  float inner = min(outer, roundedCoverage(frag_info.content_bounds,
                                         max(frag_info.radius - inset, 0.0)));
  if (outer <= 0.0) { discard; }
  vec4 surface = vec4(0.0);
  // No client texture fetch for the solid frame; no filtered-backdrop fetch
  // for opaque clients, transparent holes or disabled blur/glass.
  if (inner > 0.0 && frag_info.has_surface > 0.5 &&
      all(greaterThanEqual(v_surface_uv, frag_info.source_coverage.xy)) &&
      all(lessThanEqual(v_surface_uv, frag_info.source_coverage.zw))) {
    surface = sampleSurface(clamp(v_surface_uv, frag_info.source_limits.xy,
                                  frag_info.source_limits.zw));
    surface *= frag_info.surface_opacity;
    if (frag_info.has_backdrop > 0.5 && surface.a > 0.0 &&
        surface.a * inner > frag_info.alpha_threshold &&
        surface.a < 1.0 - 1.0 / 1024.0) {
      vec4 backdrop = vec4(texture(backdrop_texture_sampler, v_backdrop_uv,
                                   float16_t(kDefaultMipBias)));
      surface += backdrop * frag_info.backdrop_opacity * (1.0 - surface.a);
    }
  }
  // Disjoint coverages avoid double blending at the inner AA edge.
  frag_color = (surface * inner + frag_info.frame_color * (outer - inner)) *
               frag_info.opacity;
}
