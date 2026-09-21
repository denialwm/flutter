// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <impeller/types.glsl>

uniform FrameInfo {
  mat4 mvp;
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
  v_blurred_texture_coords = blurred_texture_coords;
  v_scene_texture_coords = scene_texture_coords;
  v_material_position = material_position;
}
