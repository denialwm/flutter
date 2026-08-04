// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_COMMAND_QUEUE_VK_H_
#define FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_COMMAND_QUEUE_VK_H_

#include "impeller/renderer/backend/vulkan/vk.h"
#include "impeller/renderer/command_queue.h"

namespace impeller {

class ContextVK;

class CommandQueueVK : public CommandQueue {
 public:
  explicit CommandQueueVK(const std::weak_ptr<ContextVK>& context);

  ~CommandQueueVK() override;

  fml::Status Submit(const std::vector<std::shared_ptr<CommandBuffer>>& buffers,
                     const CompletionCallback& completion_callback = {},
                     bool block_on_schedule = false) override;

  fml::Status SubmitWithSignalSemaphore(
      const std::vector<std::shared_ptr<CommandBuffer>>& buffers,
      vk::Semaphore signal_semaphore,
      const CompletionCallback& completion_callback = {});

 private:
  fml::Status SubmitInternal(
      const std::vector<std::shared_ptr<CommandBuffer>>& buffers,
      vk::Semaphore signal_semaphore,
      const CompletionCallback& completion_callback);

  std::weak_ptr<ContextVK> context_;

  CommandQueueVK(const CommandQueueVK&) = delete;

  CommandQueueVK& operator=(const CommandQueueVK&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_RENDERER_BACKEND_VULKAN_COMMAND_QUEUE_VK_H_
