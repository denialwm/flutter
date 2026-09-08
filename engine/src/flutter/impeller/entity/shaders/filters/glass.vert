// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <impeller/conversions.glsl>
#include <impeller/types.glsl>

uniform FrameInfo {
  mat4 mvp;
  float blurred_sampler_y_coord_scale;
  float scene_sampler_y_coord_scale;
}
frame_info;

in vec2 position;
in vec2 blurred_texture_coords;
in vec2 scene_texture_coords;
in vec2 material_position;

out highp vec2 v_blurred_texture_coords;
out highp vec2 v_scene_texture_coords;
out highp vec2 v_material_position;

void main() {
  gl_Position = frame_info.mvp * vec4(position, 0.0, 1.0);
  v_blurred_texture_coords = IPRemapCoords(
      blurred_texture_coords, frame_info.blurred_sampler_y_coord_scale);
  v_scene_texture_coords = IPRemapCoords(
      scene_texture_coords, frame_info.scene_sampler_y_coord_scale);
  v_material_position = material_position;
}
