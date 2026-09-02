// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

precision highp float;

#include <impeller/color.glsl>
#include <impeller/types.glsl>

uniform f16sampler2D sharp_texture_sampler;
uniform f16sampler2D blurred_texture_sampler;

uniform FragInfo {
  vec2 material_size;
  vec4 corner_radii;
  vec4 sharp_uv_basis;
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
  float sharp_opacity;
  float blurred_opacity;
}
frag_info;

in highp vec2 v_sharp_texture_coords;
in highp vec2 v_blurred_texture_coords;
in highp vec2 v_material_position;

out f16vec4 frag_color;

float selectedCornerRadius(vec2 centered) {
  if (centered.y < 0.0) {
    return centered.x < 0.0 ? frag_info.corner_radii.x
                            : frag_info.corner_radii.y;
  }
  return centered.x < 0.0 ? frag_info.corner_radii.w
                          : frag_info.corner_radii.z;
}

void roundedBoxField(vec2 position,
                     out float signed_distance,
                     out vec2 outward_normal) {
  vec2 half_size = max(frag_info.material_size * 0.5, vec2(0.0001));
  vec2 centered = position - half_size;
  float radius = clamp(selectedCornerRadius(centered), 0.0,
                       min(half_size.x, half_size.y));
  vec2 q = abs(centered) - (half_size - vec2(radius));
  vec2 outside = max(q, 0.0);
  signed_distance = length(outside) + min(max(q.x, q.y), 0.0) - radius;

  vec2 sign_position = vec2(centered.x < 0.0 ? -1.0 : 1.0,
                            centered.y < 0.0 ? -1.0 : 1.0);
  if (outside.x > 0.0 && outside.y > 0.0) {
    outward_normal = normalize(outside) * sign_position;
  } else if (q.x > q.y) {
    outward_normal = vec2(sign_position.x, 0.0);
  } else {
    outward_normal = vec2(0.0, sign_position.y);
  }
}

vec2 sharpUvOffset(vec2 pixel_offset) {
  return frag_info.sharp_uv_basis.xy * pixel_offset.x +
         frag_info.sharp_uv_basis.zw * pixel_offset.y;
}

vec2 blurredUvOffset(vec2 pixel_offset) {
  return frag_info.blurred_uv_basis.xy * pixel_offset.x +
         frag_info.blurred_uv_basis.zw * pixel_offset.y;
}

vec4 sampleSharp(vec2 pixel_offset, float spread) {
  return vec4(texture(sharp_texture_sampler,
                      v_sharp_texture_coords +
                          sharpUvOffset(pixel_offset * spread))) *
         frag_info.sharp_opacity;
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

  vec4 blurred = vec4(texture(
                     blurred_texture_sampler,
                     v_blurred_texture_coords +
                         blurredUvOffset(displacement * 0.22))) *
                 frag_info.blurred_opacity;
  vec4 sharp_green = sampleSharp(displacement, 1.0);
  vec4 sharp = sharp_green;
  if (frag_info.dispersion > 0.0001) {
    float chroma = frag_info.dispersion * 0.16;
    vec4 sharp_red = sampleSharp(displacement, 1.0 + chroma);
    vec4 sharp_blue = sampleSharp(displacement, 1.0 - chroma);
    sharp = vec4(sharp_red.r, sharp_green.g, sharp_blue.b, sharp_green.a);
  }

  vec4 material = mix(blurred, sharp, curved_edge * frag_info.refraction);
  vec4 straight = IPUnpremultiply(material);
  float luminance = dot(straight.rgb, vec3(0.2126, 0.7152, 0.0722));
  straight.rgb = mix(vec3(luminance), straight.rgb, frag_info.saturation);
  straight.rgb = mix(straight.rgb, frag_info.tint.rgb,
                     frag_info.tint_strength * frag_info.tint.a);
  straight.rgb += frag_info.brightness >= 0.0
                      ? frag_info.brightness * (1.0 - straight.rgb)
                      : frag_info.brightness * straight.rgb;

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
  straight.rgb = clamp(straight.rgb + vec3(highlight - shadow), 0.0, 1.0);

  frag_color = f16vec4(IPPremultiply(straight));
}
