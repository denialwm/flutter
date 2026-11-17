// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_RESOURCE_AUDIT_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_RESOURCE_AUDIT_GLES_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

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

// The render audit's memory report: every live GLES texture that owns its
// storage, grouped by label, size, format and sample count. Off unless
// DENIAL_RENDER_AUDIT=1; when on, each call takes one lock.
class DenialLiveTexturesGLES {
 public:
  static bool Enabled() {
    static const bool enabled = [] {
      const char* value = std::getenv("DENIAL_RENDER_AUDIT");
      return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
  }

  static void Created(const void* texture,
                      int64_t width,
                      int64_t height,
                      int format,
                      size_t bytes_per_pixel,
                      int samples,
                      size_t mip_count,
                      bool renderbuffer) {
    if (!Enabled()) {
      return;
    }
    State& state = GetState();
    std::scoped_lock lock(state.mutex);
    Entry& entry = state.live[texture];
    entry.width = width;
    entry.height = height;
    entry.format = format;
    entry.bytes_per_pixel = bytes_per_pixel;
    entry.samples = samples;
    entry.mip_count = mip_count;
    entry.renderbuffer = renderbuffer;
  }

  static void Labeled(const void* texture,
                      std::string_view label,
                      std::string_view trailing = {}) {
    if (!Enabled()) {
      return;
    }
    State& state = GetState();
    std::scoped_lock lock(state.mutex);
    auto found = state.live.find(texture);
    if (found == state.live.end()) {
      return;
    }
    found->second.label = std::string(label);
    if (!trailing.empty()) {
      found->second.label += " ";
      found->second.label += trailing;
    }
  }

  static void Destroyed(const void* texture) {
    if (!Enabled()) {
      return;
    }
    State& state = GetState();
    std::scoped_lock lock(state.mutex);
    state.live.erase(texture);
  }

  // One line: the totals, then the groups by size in bytes, largest first.
  // Bytes are every mip level's pixels times the sample count; a driver's
  // hidden multisample storage is not included.
  static std::string Describe() {
    using Key =
        std::tuple<std::string, int64_t, int64_t, int, int, size_t, bool>;
    std::map<Key, std::pair<uint64_t, uint64_t>> groups;
    uint64_t count = 0;
    uint64_t bytes = 0;
    {
      State& state = GetState();
      std::scoped_lock lock(state.mutex);
      for (const auto& [texture, entry] : state.live) {
        uint64_t pixels = 0;
        for (size_t level = 0; level < std::max<size_t>(entry.mip_count, 1u);
             ++level) {
          pixels += static_cast<uint64_t>(
                        std::max<int64_t>(entry.width >> level, 1)) *
                    static_cast<uint64_t>(
                        std::max<int64_t>(entry.height >> level, 1));
        }
        const uint64_t size =
            pixels * entry.bytes_per_pixel *
            static_cast<uint64_t>(entry.samples > 0 ? entry.samples : 1);
        auto& group =
            groups[Key(entry.label, entry.width, entry.height, entry.format,
                       entry.samples, entry.mip_count, entry.renderbuffer)];
        ++group.first;
        group.second += size;
        ++count;
        bytes += size;
      }
    }
    std::vector<std::pair<Key, std::pair<uint64_t, uint64_t>>> ordered(
        groups.begin(), groups.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
      return a.second.second > b.second.second;
    });
    std::ostringstream out;
    out << "count=" << count << " bytes=" << bytes;
    size_t shown = 0;
    for (const auto& [key, group] : ordered) {
      if (++shown > 48) {
        break;
      }
      const auto& [label, width, height, format, samples, mips, renderbuffer] =
          key;
      out << " | " << group.first << "x[" << (label.empty() ? "-" : label)
          << "] " << width << "x" << height << " format=" << format
          << " samples=" << samples << " mips=" << mips
          << (renderbuffer ? " renderbuffer" : "") << " bytes=" << group.second;
    }
    return out.str();
  }

 private:
  struct Entry {
    int64_t width = 0;
    int64_t height = 0;
    int format = 0;
    size_t bytes_per_pixel = 0;
    int samples = 1;
    size_t mip_count = 1;
    bool renderbuffer = false;
    std::string label;
  };

  struct State {
    std::mutex mutex;
    std::unordered_map<const void*, Entry> live;
  };

  static State& GetState() {
    static State* state = new State();
    return *state;
  }
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_DENIAL_RESOURCE_AUDIT_GLES_H_
