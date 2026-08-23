// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/denial_gpu_audit_gles.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>

#include "flutter/fml/logging.h"

namespace impeller {

namespace {

constexpr size_t kMaximumPendingSamples = 8192u;

constexpr std::array<std::string_view,
                     static_cast<size_t>(DenialGpuAuditStage::kCount)>
    kStageNames = {
        "root_pass",        "backdrop_layer_color",  "blur_downsample",
        "blur_vertical",    "blur_horizontal",       "other_pass",
        "backdrop_restore", "msaa_backdrop_restore", "layer_resolve",
};

bool IsAuditRequested() {
  const char* value = std::getenv("DENIA_RENDER_AUDIT");
  return value != nullptr && value[0] != '\0' &&
         !(value[0] == '0' && value[1] == '\0');
}

uint64_t Percentile(std::vector<uint64_t> values, size_t percentile) {
  if (values.empty()) {
    return 0u;
  }
  std::sort(values.begin(), values.end());
  const size_t rank = ((values.size() * percentile + 99u) / 100u) - 1u;
  return values[std::min(rank, values.size() - 1u)];
}

template <class StageStats>
std::string FormatStage(std::string_view name, const StageStats& stats) {
  std::stringstream stream;
  const double total_us = static_cast<double>(stats.total_nanos) / 1000.0;
  const double average_us =
      stats.samples == 0u ? 0.0 : total_us / stats.samples;
  stream << name << "=" << stats.samples << ":total_us=" << total_us
         << "/avg_us=" << average_us << "/p95_us="
         << static_cast<double>(Percentile(stats.values, 95u)) / 1000.0
         << "/max_us=" << static_cast<double>(stats.max_nanos) / 1000.0
         << "/pixels=" << stats.pixels;
  return stream.str();
}

}  // namespace

void DenialGpuAuditGLES::StageStats::Record(uint64_t nanos,
                                            uint64_t sample_pixels) {
  samples++;
  pixels += sample_pixels;
  total_nanos += nanos;
  max_nanos = std::max(max_nanos, nanos);
  values.push_back(nanos);
}

DenialGpuAuditGLES::DenialGpuAuditGLES() = default;

bool DenialGpuAuditGLES::InitializeIfNeeded(const ProcTableGLES& gl) {
  if (initialized_) {
    return enabled_;
  }
  initialized_ = true;
  enabled_ = IsAuditRequested() &&
             gl.GetDescription()->HasExtension("GL_EXT_disjoint_timer_query") &&
             gl.GenQueriesEXT.IsAvailable() &&
             gl.DeleteQueriesEXT.IsAvailable() &&
             gl.QueryCounterEXT.IsAvailable() &&
             gl.GetQueryObjectuivEXT.IsAvailable() &&
             gl.GetQueryObjectui64vEXT.IsAvailable();
  if (IsAuditRequested() && !enabled_) {
    FML_LOG(IMPORTANT)
        << "Denial GPU stage audit unavailable: "
           "GL_EXT_disjoint_timer_query timestamp markers are missing";
  }
  return enabled_;
}

DenialGpuAuditGLES::Token DenialGpuAuditGLES::Begin(const ProcTableGLES& gl,
                                                    DenialGpuAuditStage stage,
                                                    uint64_t pixels) {
  if (!InitializeIfNeeded(gl)) {
    return {};
  }
  GLuint query = 0u;
  gl.GenQueriesEXT(1, &query);
  if (query == 0u) {
    abandoned_++;
    return {};
  }
  gl.QueryCounterEXT(query, GL_TIMESTAMP_EXT);
  return Token{.start = query, .stage = stage, .pixels = pixels};
}

void DenialGpuAuditGLES::End(const ProcTableGLES& gl, Token token) {
  if (token.start == 0u || !enabled_) {
    return;
  }
  GLuint end = 0u;
  gl.GenQueriesEXT(1, &end);
  if (end == 0u) {
    gl.DeleteQueriesEXT(1, &token.start);
    abandoned_++;
    return;
  }
  gl.QueryCounterEXT(end, GL_TIMESTAMP_EXT);
  pending_.push_back(PendingSample{.start = token.start,
                                   .end = end,
                                   .stage = token.stage,
                                   .pixels = token.pixels});
  pending_max_ = std::max(pending_max_, pending_.size());
  while (pending_.size() > kMaximumPendingSamples) {
    Delete(gl, pending_.front());
    pending_.pop_front();
    abandoned_++;
  }
}

void DenialGpuAuditGLES::Poll(const ProcTableGLES& gl) {
  if (!InitializeIfNeeded(gl)) {
    return;
  }

  GLint disjoint = GL_FALSE;
  gl.GetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
  if (disjoint != GL_FALSE) {
    disjoint_ += pending_.size();
    while (!pending_.empty()) {
      Delete(gl, pending_.front());
      pending_.pop_front();
    }
    FlushIfDue();
    return;
  }

  while (!pending_.empty()) {
    const PendingSample sample = pending_.front();
    GLuint available = GL_FALSE;
    gl.GetQueryObjectuivEXT(sample.end, GL_QUERY_RESULT_AVAILABLE_EXT,
                            &available);
    if (available != GL_TRUE) {
      break;
    }

    GLuint64 start = 0u;
    GLuint64 end = 0u;
    gl.GetQueryObjectui64vEXT(sample.start, GL_QUERY_RESULT_EXT, &start);
    gl.GetQueryObjectui64vEXT(sample.end, GL_QUERY_RESULT_EXT, &end);
    if (end >= start) {
      stats_[static_cast<size_t>(sample.stage)].Record(end - start,
                                                       sample.pixels);
    } else {
      disjoint_++;
    }
    Delete(gl, sample);
    pending_.pop_front();
  }
  FlushIfDue();
}

void DenialGpuAuditGLES::Delete(const ProcTableGLES& gl,
                                const PendingSample& sample) {
  const GLuint queries[] = {sample.start, sample.end};
  gl.DeleteQueriesEXT(2, queries);
}

void DenialGpuAuditGLES::FlushIfDue() {
  const Clock::time_point now = Clock::now();
  const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - period_start_);
  if (interval < std::chrono::seconds(1)) {
    return;
  }

  uint64_t accounted_nanos = 0u;
  uint64_t completed_samples = 0u;
  for (size_t i = 0u; i < stats_.size(); i++) {
    completed_samples += stats_[i].samples;
    if (i <= static_cast<size_t>(DenialGpuAuditStage::kOtherPass)) {
      accounted_nanos += stats_[i].total_nanos;
    }
  }
  if (completed_samples == 0u && disjoint_ == 0u && abandoned_ == 0u) {
    period_start_ = now;
    return;
  }

  std::stringstream stages;
  for (size_t i = 0u; i < stats_.size(); i++) {
    stages << " " << FormatStage(kStageNames[i], stats_[i]);
  }
  FML_LOG(IMPORTANT) << "Denial GPU stage audit"
                     << " interval_ms=" << interval.count()
                     << " accounted_pass_gpu_us="
                     << static_cast<double>(accounted_nanos) / 1000.0
                     << " completed_samples=" << completed_samples
                     << " pending=" << pending_.size()
                     << " pending_max=" << pending_max_
                     << " disjoint=" << disjoint_ << " abandoned=" << abandoned_
                     << stages.str();

  stats_ = {};
  disjoint_ = 0u;
  abandoned_ = 0u;
  pending_max_ = pending_.size();
  period_start_ = now;
}

DenialGpuAuditStage DenialGpuAuditGLES::ClassifyPass(std::string_view label) {
  if (label == "Denial Backdrop Layer Color") {
    return DenialGpuAuditStage::kBackdropLayerColor;
  }
  if (label == "Denial Backdrop Blur Downsample") {
    return DenialGpuAuditStage::kBackdropDownsample;
  }
  if (label == "Denial Backdrop Blur Vertical") {
    return DenialGpuAuditStage::kBackdropBlurVertical;
  }
  if (label == "Denial Backdrop Blur Horizontal") {
    return DenialGpuAuditStage::kBackdropBlurHorizontal;
  }
  if (label == "EntityPass Render Pass") {
    return DenialGpuAuditStage::kRootPass;
  }
  return DenialGpuAuditStage::kOtherPass;
}

DenialGpuAuditGLES& GetDenialGpuAuditGLES() {
  static thread_local DenialGpuAuditGLES audit;
  return audit;
}

}  // namespace impeller
