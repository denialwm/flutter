// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/denial_gpu_audit_gles.h"

#include "flutter/testing/testing.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"

namespace impeller {
namespace testing {

TEST(DenialGpuAuditGLES, ClassifiesPhysicalPasses) {
  EXPECT_EQ(DenialGpuAuditGLES::ClassifyPass("EntityPass Render Pass"),
            DenialGpuAuditStage::kRootPass);
  EXPECT_EQ(DenialGpuAuditGLES::ClassifyPass("Denial Backdrop Layer Color"),
            DenialGpuAuditStage::kBackdropLayerColor);
  EXPECT_EQ(DenialGpuAuditGLES::ClassifyPass("Denial Backdrop Blur Downsample"),
            DenialGpuAuditStage::kBackdropDownsample);
  EXPECT_EQ(DenialGpuAuditGLES::ClassifyPass("Denial Backdrop Blur Vertical"),
            DenialGpuAuditStage::kBackdropBlurVertical);
  EXPECT_EQ(DenialGpuAuditGLES::ClassifyPass("Denial Backdrop Blur Horizontal"),
            DenialGpuAuditStage::kBackdropBlurHorizontal);
  EXPECT_EQ(DenialGpuAuditGLES::ClassifyPass("EntityPass Layer Color"),
            DenialGpuAuditStage::kOtherPass);
}

TEST(DenialGpuAuditGLES, MockResolvesTimestampMarkerEntryPoint) {
  auto gl = MockGLES::Init();
  EXPECT_TRUE(gl->GetProcTable().QueryCounterEXT.IsAvailable());
}

}  // namespace testing
}  // namespace impeller
