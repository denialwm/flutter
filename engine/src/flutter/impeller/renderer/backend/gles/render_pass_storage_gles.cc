// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/renderer/backend/gles/render_pass_storage_gles.h"

#include <utility>

namespace impeller {

void RenderPassStorageGLES::Clear() {
  commands.clear();
  vertex_buffers.clear();
  bound_buffers.clear();
  bound_textures.clear();
}

size_t RenderPassStorageGLES::CapacityInBytes() const {
  return commands.capacity() * sizeof(Command) +
         vertex_buffers.capacity() * sizeof(BufferView) +
         bound_buffers.capacity() * sizeof(BufferResource) +
         bound_textures.capacity() * sizeof(TextureAndSampler);
}

RenderPassStorageGLES RenderPassStoragePoolGLES::Take() {
  std::lock_guard lock(mutex_);
  if (shutdown_ || entries_.empty()) {
    return {};
  }
  auto storage = std::move(entries_.back());
  retained_bytes_ -= storage.CapacityInBytes();
  entries_.pop_back();
  return storage;
}

void RenderPassStoragePoolGLES::Put(RenderPassStorageGLES storage) {
  // Clearing may release the last reference to a GPU resource. Do it before
  // acquiring the mutex so resource teardown can enter the reactor freely.
  storage.Clear();
  const size_t bytes = storage.CapacityInBytes();
  if (bytes == 0 || bytes > kMaxBytes) {
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (!shutdown_ && entries_.size() < kMaxEntries &&
        bytes <= kMaxBytes - retained_bytes_) {
      retained_bytes_ += bytes;
      entries_.push_back(std::move(storage));
    }
  }
  // Rejected storage is destroyed outside the mutex.
}

void RenderPassStoragePoolGLES::Shutdown() {
  std::vector<RenderPassStorageGLES> discarded;
  {
    std::lock_guard lock(mutex_);
    shutdown_ = true;
    retained_bytes_ = 0;
    discarded.swap(entries_);
  }
}

size_t RenderPassStoragePoolGLES::RetainedEntries() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

size_t RenderPassStoragePoolGLES::RetainedBytes() const {
  std::lock_guard lock(mutex_);
  return retained_bytes_;
}

}  // namespace impeller
