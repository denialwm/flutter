// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_GPU_AUDIT_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_GPU_AUDIT_GLES_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>

#include "impeller/renderer/backend/gles/proc_table_gles.h"

namespace impeller {

// Physical GLES stages measured by the Denial render audit. Pass stages are
// mutually exclusive. Root-command, restore, and resolve stages are nested
// subspans and are reported separately from the accounted pass total.
enum class DenialGpuAuditStage : uint8_t {
  kRootPass,
  kBackdropLayerColor,
  kBackdropDownsample,
  kBackdropBlurVertical,
  kBackdropBlurHorizontal,
  kOtherPass,
  kRootSceneTexture,
  kRootExternalTexture,
  kRootTiledTexture,
  kRootClipDirect,
  kRootClipStencil,
  kRootClipCover,
  kRootSolid,
  kRootText,
  kRootAtlas,
  kRootOtherDraw,
  kBackdropRestore,
  kMsaaBackdropRestore,
  kResolve,
  kCount,
};

class DenialGpuAuditGLES {
 public:
  struct Token {
    GLuint start = 0u;
    DenialGpuAuditStage stage = DenialGpuAuditStage::kOtherPass;
    uint64_t pixels = 0u;
  };

  DenialGpuAuditGLES();

  Token Begin(const ProcTableGLES& gl,
              DenialGpuAuditStage stage,
              uint64_t pixels);

  void End(const ProcTableGLES& gl, Token token);

  // Polls completed timestamp pairs without waiting for the GPU. This should
  // be called before recording a physical pass.
  void Poll(const ProcTableGLES& gl);

  static DenialGpuAuditStage ClassifyPass(std::string_view label);

 private:
  using Clock = std::chrono::steady_clock;

  struct PendingSample {
    GLuint start = 0u;
    GLuint end = 0u;
    DenialGpuAuditStage stage = DenialGpuAuditStage::kOtherPass;
    uint64_t pixels = 0u;
  };

  struct StageStats {
    uint64_t samples = 0u;
    uint64_t pixels = 0u;
    uint64_t total_nanos = 0u;
    uint64_t max_nanos = 0u;
    std::vector<uint64_t> values;

    void Record(uint64_t nanos, uint64_t sample_pixels);
  };

  bool InitializeIfNeeded(const ProcTableGLES& gl);
  void Delete(const ProcTableGLES& gl, const PendingSample& sample);
  void FlushIfDue();

  bool initialized_ = false;
  bool enabled_ = false;
  Clock::time_point period_start_ = Clock::now();
  std::deque<PendingSample> pending_;
  std::array<StageStats, static_cast<size_t>(DenialGpuAuditStage::kCount)>
      stats_;
  uint64_t disjoint_ = 0u;
  uint64_t abandoned_ = 0u;
  size_t pending_max_ = 0u;
};

DenialGpuAuditGLES& GetDenialGpuAuditGLES();

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_GPU_AUDIT_GLES_H_
