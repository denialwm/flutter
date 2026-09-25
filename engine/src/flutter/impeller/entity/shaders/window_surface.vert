// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <impeller/types.glsl>
uniform FrameInfo { mat4 mvp; } frame_info;
in vec2 position;
in vec2 surface_uv;
in vec2 backdrop_uv;
out highp vec2 v_position;
out highp vec2 v_surface_uv;
out highp vec2 v_backdrop_uv;
void main() {
  gl_Position = frame_info.mvp * vec4(position, 0.0, 1.0);
  v_position = position;
  v_surface_uv = surface_uv;
  v_backdrop_uv = backdrop_uv;
}
