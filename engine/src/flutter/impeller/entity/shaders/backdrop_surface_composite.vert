// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <impeller/types.glsl>

uniform FrameInfo {
  mat4 mvp;
  mat4 model;
}
frame_info;

in vec2 position;
in vec2 surface_texture_coords;
in vec2 backdrop_texture_coords;
in vec2 scene_texture_coords;

out highp vec2 v_surface_texture_coords;
out highp vec2 v_backdrop_texture_coords;
out highp vec2 v_scene_texture_coords;
out highp vec2 v_pass_position;

void main() {
  gl_Position = frame_info.mvp * vec4(position, 0.0, 1.0);
  v_pass_position = (frame_info.model * vec4(position, 0.0, 1.0)).xy;
  v_surface_texture_coords = surface_texture_coords;
  v_backdrop_texture_coords = backdrop_texture_coords;
  v_scene_texture_coords = scene_texture_coords;
}
