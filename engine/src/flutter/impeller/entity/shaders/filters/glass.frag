// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision highp float;

#include <impeller/types.glsl>

uniform f16sampler2D blurred_texture_sampler;

uniform FragInfo {
  vec2 material_size;
  vec4 corner_radii;
  vec4 blurred_uv_basis;
  vec4 tint;
  float thickness;
  float refraction;
  float dispersion;
  float saturation;
  float tint_strength;
  float brightness;
  float light_angle;
  float light_intensity;
  float edge_strength;
  float blurred_opacity;
}
frag_info;

in highp vec2 v_blurred_texture_coords;
in highp vec2 v_material_position;

out f16vec4 frag_color;

float selectedCornerRadius(vec2 centered) {
  if (centered.y < 0.0) {
    return centered.x < 0.0 ? frag_info.corner_radii.x
                            : frag_info.corner_radii.y;
  }
  return centered.x < 0.0 ? frag_info.corner_radii.w : frag_info.corner_radii.z;
}

void roundedBoxField(vec2 position,
                     out float signed_distance,
                     out vec2 outward_normal) {
  vec2 half_size = max(frag_info.material_size * 0.5, vec2(0.0001));
  vec2 centered = position - half_size;
  float radius =
      clamp(selectedCornerRadius(centered), 0.0, min(half_size.x, half_size.y));
  vec2 q = abs(centered) - (half_size - vec2(radius));
  vec2 outside = max(q, 0.0);
  signed_distance = length(outside) + min(max(q.x, q.y), 0.0) - radius;

  vec2 sign_position =
      vec2(centered.x < 0.0 ? -1.0 : 1.0, centered.y < 0.0 ? -1.0 : 1.0);
  if (outside.x > 0.0 && outside.y > 0.0) {
    outward_normal = normalize(outside) * sign_position;
  } else if (q.x > q.y) {
    outward_normal = vec2(sign_position.x, 0.0);
  } else {
    outward_normal = vec2(0.0, sign_position.y);
  }
}

vec2 blurredUvOffset(vec2 pixel_offset) {
  return frag_info.blurred_uv_basis.xy * pixel_offset.x +
         frag_info.blurred_uv_basis.zw * pixel_offset.y;
}

vec4 sampleFrost(vec2 pixel_offset, float spread) {
  return vec4(texture(blurred_texture_sampler,
                      v_blurred_texture_coords +
                          blurredUvOffset(pixel_offset * spread))) *
         frag_info.blurred_opacity;
}

vec3 straightRgb(vec4 color) {
  return color.rgb / max(color.a, 0.00001);
}

void main() {
  float signed_distance;
  vec2 outward_normal;
  roundedBoxField(v_material_position, signed_distance, outward_normal);

  float thickness = max(frag_info.thickness, 0.0001);
  float edge = 1.0 - smoothstep(0.0, thickness, max(-signed_distance, 0.0));
  float curved_edge = edge * edge * (3.0 - 2.0 * edge);
  vec2 displacement =
      -outward_normal * thickness * frag_info.refraction * curved_edge;

  // Refraction displaces one coherent optical medium. Switching from frost to
  // an unfiltered scene at the edge reads as a transparent cutout rather than
  // curved glass, so all wavelength samples come from the frosted backdrop.
  vec4 refracted_green = sampleFrost(displacement, 1.0);
  vec3 refracted_rgb = straightRgb(refracted_green);
  if (frag_info.dispersion > 0.0001) {
    float chroma = frag_info.dispersion * 0.16;
    vec4 refracted_red = sampleFrost(displacement, 1.0 + chroma);
    vec4 refracted_blue = sampleFrost(displacement, 1.0 - chroma);
    refracted_rgb = vec3(straightRgb(refracted_red).r, refracted_rgb.g,
                         straightRgb(refracted_blue).b);
  }

  float material_alpha = refracted_green.a;
  vec3 material_rgb = refracted_rgb;
  float luminance = dot(material_rgb, vec3(0.2126, 0.7152, 0.0722));
  material_rgb = mix(vec3(luminance), material_rgb, frag_info.saturation);
  material_rgb = mix(material_rgb, frag_info.tint.rgb,
                     frag_info.tint_strength * frag_info.tint.a);
  material_rgb += frag_info.brightness >= 0.0
                      ? frag_info.brightness * (1.0 - material_rgb)
                      : frag_info.brightness * material_rgb;

  vec2 light_direction =
      vec2(cos(frag_info.light_angle), sin(frag_info.light_angle));
  float facing_light = max(dot(outward_normal, light_direction), 0.0);
  float facing_shadow = max(dot(outward_normal, -light_direction), 0.0);
  vec3 optical_normal =
      normalize(vec3(outward_normal * curved_edge * 0.85, 1.0));
  float fresnel = pow(1.0 - optical_normal.z, 2.0);
  float caustic_position = (edge - 0.46) / 0.17;
  float caustic_band = exp(-caustic_position * caustic_position);
  float highlight =
      curved_edge * facing_light * frag_info.light_intensity * 0.18 +
      fresnel * frag_info.edge_strength * 0.28 +
      caustic_band * facing_light * frag_info.edge_strength * 0.1;
  float shadow =
      curved_edge * facing_shadow * frag_info.light_intensity * 0.075;
  material_rgb = clamp(material_rgb + vec3(highlight - shadow), 0.0, 1.0);

  frag_color = f16vec4(material_rgb * material_alpha, material_alpha);
}
