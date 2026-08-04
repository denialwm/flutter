// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/gpu/gpu_surface_vulkan_delegate.h"

namespace flutter {

GPUSurfaceVulkanDelegate::~GPUSurfaceVulkanDelegate() = default;

bool GPUSurfaceVulkanDelegate::AcquireFrameImage(const DlISize&,
                                                 FlutterVulkanFrameImage*) {
  return false;
}

bool GPUSurfaceVulkanDelegate::SupportsVulkanFrameCallback() const {
  return false;
}

bool GPUSurfaceVulkanDelegate::OnVulkanFrame(FlutterVulkanFrameStatus,
                                             const FlutterVulkanFrameImage*,
                                             fml::UniqueFD) {
  return false;
}

}  // namespace flutter
