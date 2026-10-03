// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
uniform f16sampler2D backdrop_texture_sampler;
uniform FragInfo {
  // GLES lowers a uniform block to individual glUniform calls. One tightly
  // packed vec4 array uploads this material in ONE call instead of fifteen,
  // with no array-repacking allocation and the same 144-byte payload.
  vec4 data[9];
}
frag_info;
#define window_bounds frag_info.data[0]
#define content_bounds frag_info.data[1]
#define source_limits frag_info.data[2]
#define source_coverage frag_info.data[3]
#define frame_color frag_info.data[4]
#define radius frag_info.data[5].x
#define opacity frag_info.data[5].y
#define surface_opacity frag_info.data[5].z
#define backdrop_opacity frag_info.data[5].w
#define alpha_threshold frag_info.data[6].x
#define has_backdrop frag_info.data[6].y
#define has_surface frag_info.data[6].z
#define has_material_bounds frag_info.data[6].w
#define material_bounds frag_info.data[7]
#define backdrop_limits frag_info.data[8]
in highp vec2 v_position;
in highp vec2 v_surface_uv;
in highp vec2 v_backdrop_uv;
out vec4 frag_color;

float roundedCoverage(vec4 bounds, float corner_radius) {
  vec2 half_size = max((bounds.zw - bounds.xy) * 0.5, vec2(0.0));
  corner_radius = clamp(corner_radius, 0.0, min(half_size.x, half_size.y));
  vec2 q = abs(v_position - (bounds.xy + bounds.zw) * 0.5) - half_size +
           corner_radius;
  float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - corner_radius;
  // Derivatives keep the fringe one physical pixel wide during output-scale,
  // overview and window animation transforms, without stencil/MSAA passes.
  return clamp(0.5 - d / max(fwidth(d), 0.00001), 0.0, 1.0);
}

// The backdrop material may be confined to window-space bounds. Outside them,
// the client composites over the unfiltered destination.
float materialCoverage() {
  if (has_material_bounds < 0.5) {
    return 1.0;
  }
  vec2 inside =
      min(v_position - material_bounds.xy, material_bounds.zw - v_position);
  vec2 coverage =
      clamp(inside / max(fwidth(v_position), vec2(0.00001)) + 0.5, 0.0, 1.0);
  return coverage.x * coverage.y;
}
void main() {
  float outer = roundedCoverage(window_bounds, radius);
  float inset = max(content_bounds.x - window_bounds.x, 0.0);
  float inner =
      min(outer, roundedCoverage(content_bounds, max(radius - inset, 0.0)));
  float material = materialCoverage();
  if (outer <= 0.0) {
    discard;
  }
  vec4 surface = vec4(0.0);
  // No client texture fetch for the solid frame; no filtered-backdrop fetch
  // for opaque clients, transparent holes or disabled blur/glass.
  if (inner > 0.0 && has_surface > 0.5 &&
      all(greaterThanEqual(v_surface_uv, source_coverage.xy)) &&
      all(lessThanEqual(v_surface_uv, source_coverage.zw))) {
    surface =
        sampleSurface(clamp(v_surface_uv, source_limits.xy, source_limits.zw));
    surface *= surface_opacity;
    if (has_backdrop > 0.5 && material > 0.0 && surface.a > 0.0 &&
        surface.a * inner > alpha_threshold && surface.a < 1.0 - 1.0 / 1024.0) {
      vec2 backdrop_uv =
          clamp(v_backdrop_uv, backdrop_limits.xy, backdrop_limits.zw);
      vec4 backdrop = vec4(texture(backdrop_texture_sampler, backdrop_uv,
                                   float16_t(kDefaultMipBias)));
      surface += backdrop * backdrop_opacity * material * (1.0 - surface.a);
    }
  }
  // Disjoint coverages avoid double blending at the inner AA edge.
  frag_color = (surface * inner + frame_color * (outer - inner)) * opacity;
}
