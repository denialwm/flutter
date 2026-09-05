// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_RESOURCE_AUDIT_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_RESOURCE_AUDIT_GLES_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "fml/logging.h"
#include "fml/time/time_point.h"

namespace impeller {

// Worker-only CPU diagnostics. No queries, fences, waits or trace recording.
// The disabled path does not read the clock. Output is bounded per process.
class DenialResourceAuditGLES {
 public:
  enum class Stage {
    kCreateHandle,
    kCollectHandle,
    kConsolidate,
    kTextureStorage,
    kRenderbufferStorage,
    kCount,
  };

  explicit DenialResourceAuditGLES(Stage stage,
                                   int kind = 0,
                                   int64_t width = 0,
                                   int64_t height = 0)
      : stage_(stage), kind_(kind), width_(width), height_(height) {
    static const bool enabled = [] {
      const char* value = std::getenv("DENIA_GL_RESOURCE_AUDIT");
      return value && value[0] == '1' && value[1] == '\0';
    }();
    if (enabled) {
      start_us_ = NowMicros();
    }
  }

  ~DenialResourceAuditGLES() {
    if (start_us_ == 0) {
      return;
    }
    const int64_t end_us = NowMicros();
    const int64_t elapsed_us = end_us - start_us_;
    static std::atomic<uint32_t> slow_records = 0;
    if (elapsed_us >= 2000 &&
        slow_records.fetch_add(1, std::memory_order_relaxed) < 512) {
      FML_LOG(IMPORTANT) << "DENIA_GL_RESOURCE_SLOW start_us=" << start_us_
                         << " duration_us=" << elapsed_us
                         << " stage=" << static_cast<int>(stage_)
                         << " kind=" << kind_ << " width=" << width_
                         << " height=" << height_;
    }

    struct Summary {
      int64_t start_us = 0;
      std::array<uint64_t, static_cast<size_t>(Stage::kCount)> calls = {};
      uint64_t allocated_pixels = 0;
    };
    static thread_local Summary summary;
    if (summary.start_us == 0) {
      summary.start_us = start_us_;
    }
    summary.calls[static_cast<size_t>(stage_)]++;
    if (width_ > 0 && height_ > 0) {
      summary.allocated_pixels += static_cast<uint64_t>(width_) * height_;
    }
    if (end_us - summary.start_us >= 1000000) {
      static std::atomic<uint32_t> summary_records = 0;
      if (summary_records.fetch_add(1, std::memory_order_relaxed) < 128) {
        FML_LOG(IMPORTANT) << "DENIA_GL_RESOURCE_COUNTS start_us="
                           << summary.start_us << " end_us=" << end_us
                           << " create=" << summary.calls[0]
                           << " collect=" << summary.calls[1]
                           << " consolidate=" << summary.calls[2]
                           << " texture_storage=" << summary.calls[3]
                           << " renderbuffer_storage=" << summary.calls[4]
                           << " allocated_pixels=" << summary.allocated_pixels;
      }
      summary = {};
    }
  }

 private:
  static int64_t NowMicros() {
    return fml::TimePoint::Now().ToEpochDelta().ToMicroseconds();
  }

  Stage stage_;
  int kind_;
  int64_t width_;
  int64_t height_;
  int64_t start_us_ = 0;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_RESOURCE_AUDIT_GLES_H_
