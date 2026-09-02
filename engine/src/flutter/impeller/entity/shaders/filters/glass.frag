// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// The rounded surface, Snell refraction, and adaptive rim-lighting model is
// adapted from liquid_glass_renderer, Copyright 2025 Tim Lehmann for
// whynotmake.it, used under the MIT License.

precision highp float;

#include <impeller/types.glsl>

uniform f16sampler2D blurred_texture_sampler;

uniform FragInfo {
  vec2 material_size;
  vec4 corner_radii;
  vec4 blurred_uv_basis;
  vec4 tint;
  float thickness;
  float refractive_index;
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

float glassHeight(float signed_distance, float thickness) {
  if (signed_distance < -thickness) {
    return thickness;
  }
  float x = thickness + signed_distance;
  return sqrt(max(0.0, thickness * thickness - x * x));
}

vec3 adaptiveHighlightColor(vec3 background_color) {
  const vec3 luma_weights = vec3(0.299, 0.587, 0.114);
  float luminance = dot(background_color, luma_weights);
  float maximum = max(max(background_color.r, background_color.g),
                      background_color.b);
  float minimum = min(min(background_color.r, background_color.g),
                      background_color.b);
  float color_saturation =
      maximum > 0.0 ? (maximum - minimum) / maximum : 0.0;
  vec3 colored_highlight = vec3(1.0);
  if (luminance > 0.001) {
    colored_highlight = background_color / luminance;
    float gray = dot(colored_highlight, luma_weights);
    colored_highlight =
        min(mix(vec3(gray), colored_highlight, 1.3), vec3(1.0));
  }
  float color_influence = smoothstep(0.0, 0.6, luminance) *
                          smoothstep(0.0, 0.4, color_saturation);
  return mix(vec3(1.0), colored_highlight, color_influence);
}

vec3 glassLighting(vec3 surface_normal,
                   float signed_distance,
                   float thickness,
                   float height,
                   vec2 light_direction,
                   vec3 background_color) {
  float normalized_height = height / thickness;
  float surface_shape = clamp((1.0 - normalized_height) * 1.111, 0.0, 1.0);
  float thickness_factor = clamp((thickness - 5.0) * 0.5, 0.0, 1.0);
  float rim_position = signed_distance / 1.5;
  float rim = 1.0 / (1.0 + 0.89 * rim_position * rim_position);
  if (surface_shape < 0.01 || thickness_factor < 0.01 || rim < 0.01 ||
      frag_info.light_intensity < 0.01 || frag_info.edge_strength < 0.01) {
    return vec3(0.0);
  }

  float main_light = max(0.0, dot(surface_normal.xy, light_direction));
  float opposite_light =
      max(0.0, dot(surface_normal.xy, -light_direction));
  float influence = main_light + opposite_light * 0.8;
  vec3 highlight_color = adaptiveHighlightColor(background_color);
  vec3 directional = highlight_color * 0.7 * influence * influence *
                     frag_info.light_intensity * 2.0;
  return directional * rim * thickness_factor * surface_shape *
         frag_info.edge_strength;
}

vec3 applyGlassTint(vec3 color) {
  float strength = clamp(frag_info.tint_strength * frag_info.tint.a, 0.0, 1.0);
  float tint_luminance =
      dot(frag_info.tint.rgb, vec3(0.299, 0.587, 0.114));
  if (tint_luminance < 0.5) {
    return mix(color, color * frag_info.tint.rgb * 2.0, strength);
  }
  vec3 screened =
      vec3(1.0) - (vec3(1.0) - color) * (vec3(1.0) - frag_info.tint.rgb);
  return mix(color, screened, strength);
}

void main() {
  float signed_distance;
  vec2 outward_normal;
  roundedBoxField(v_material_position, signed_distance, outward_normal);

  float thickness = max(frag_info.thickness, 0.0001);
  float foreground_alpha = 1.0 - smoothstep(-2.0, 0.0, signed_distance);
  if (foreground_alpha < 0.01) {
    frag_color = f16vec4(0.0hf);
    return;
  }

  // This rounded surface and Snell refraction model follows the established
  // liquid_glass_renderer implementation by Tim Lehmann (MIT). The analytical
  // rounded-box gradient is equivalent to the reference shader's SDF
  // derivatives, without requiring a separate geometry texture.
  float normal_xy_length =
      clamp((thickness + signed_distance) / thickness, 0.0, 1.0);
  float normal_z =
      sqrt(max(0.0, 1.0 - normal_xy_length * normal_xy_length));
  vec3 surface_normal =
      normalize(vec3(outward_normal * normal_xy_length, normal_z));
  float height = glassHeight(signed_distance, thickness);
  vec3 incident = vec3(0.0, 0.0, -1.0);
  vec3 refracted_ray =
      refract(incident, surface_normal,
              1.0 / max(frag_info.refractive_index, 1.0));
  float ray_length = (height + thickness * 8.0) /
                     max(0.001, abs(refracted_ray.z));
  vec2 displacement = refracted_ray.xy * ray_length;

  vec4 refracted_green = sampleFrost(displacement, 1.0);
  vec3 refracted_rgb = straightRgb(refracted_green);
  if (frag_info.dispersion > 0.0001) {
    float chroma = frag_info.dispersion * 0.5;
    vec4 refracted_red = sampleFrost(displacement, 1.0 + chroma);
    vec4 refracted_blue = sampleFrost(displacement, 1.0 - chroma);
    refracted_rgb = vec3(straightRgb(refracted_red).r, refracted_rgb.g,
                         straightRgb(refracted_blue).b);
  }

  float material_alpha = refracted_green.a * foreground_alpha;
  vec3 material_rgb = applyGlassTint(refracted_rgb);
  vec2 light_direction =
      vec2(cos(frag_info.light_angle), sin(frag_info.light_angle));
  material_rgb += glassLighting(surface_normal, signed_distance, thickness,
                                height, light_direction, refracted_rgb);
  float luminance = dot(material_rgb, vec3(0.2126, 0.7152, 0.0722));
  material_rgb = mix(vec3(luminance), material_rgb, frag_info.saturation);
  material_rgb += frag_info.brightness >= 0.0
                      ? frag_info.brightness * (1.0 - material_rgb)
                      : frag_info.brightness * material_rgb;
  material_rgb = clamp(material_rgb, 0.0, 1.0);

  frag_color = f16vec4(material_rgb * material_alpha, material_alpha);
}
