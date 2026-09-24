// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_RENDER_PASS_STORAGE_GLES_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_RENDER_PASS_STORAGE_GLES_H_

#include <cstddef>
#include <mutex>
#include <vector>

#include "impeller/renderer/command.h"

namespace impeller {

// Only empty vector allocations are retained. No pass, target, context, or GPU
// resource may be stored here.
struct RenderPassStorageGLES {
  std::vector<Command> commands;
  std::vector<BufferView> vertex_buffers;
  std::vector<BufferResource> bound_buffers;
  std::vector<TextureAndSampler> bound_textures;

  void Clear();
  size_t CapacityInBytes() const;
};

class RenderPassStoragePoolGLES final {
 public:
  static constexpr size_t kMaxEntries = 64;
  static constexpr size_t kMaxBytes = 4 * 1024 * 1024;

  RenderPassStorageGLES Take();
  void Put(RenderPassStorageGLES storage);
  void Shutdown();

  // Visible to focused backend tests.
  size_t RetainedEntries() const;
  size_t RetainedBytes() const;

 private:
  mutable std::mutex mutex_;
  std::vector<RenderPassStorageGLES> entries_;
  size_t retained_bytes_ = 0;
  bool shutdown_ = false;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_GLES_RENDER_PASS_STORAGE_GLES_H_
