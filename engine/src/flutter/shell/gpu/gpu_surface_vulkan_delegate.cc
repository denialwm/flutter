// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/gpu/gpu_surface_vulkan_delegate.h"

namespace flutter {

GPUSurfaceVulkanDelegate::~GPUSurfaceVulkanDelegate() = default;

bool GPUSurfaceVulkanDelegate::SupportsBorrowedImages() const {
  return false;
}

bool GPUSurfaceVulkanDelegate::AcquireImage2(const DlISize& size,
                                             FlutterVulkanImage2* image) {
  return false;
}

bool GPUSurfaceVulkanDelegate::PresentImage2(const FlutterVulkanImage2& image,
                                             fml::UniqueFD release_fence) {
  return false;
}

}  // namespace flutter
