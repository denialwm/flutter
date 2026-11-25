// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_AIKS_CONTEXT_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_AIKS_CONTEXT_H_

#include <memory>

#include "impeller/entity/contents/content_context.h"
#include "impeller/renderer/context.h"

#include "impeller/renderer/render_target.h"
#include "impeller/typographer/typographer_context.h"

namespace impeller {

struct Picture;
class StaticPictureCache;

class AiksContext {
 public:
  /// Construct a new AiksContext.
  ///
  /// @param context              The Impeller context that Aiks should use for
  ///                             allocating resources and executing device
  ///                             commands. Required.
  /// @param typographer_context  The text backend to use for rendering text. If
  ///                             `nullptr` is supplied, then attempting to draw
  ///                             text with Aiks will result in validation
  ///                             errors.
  /// @param render_target_allocator Injects a render target allocator or
  ///                                allocates its own if none is supplied.
  AiksContext(std::shared_ptr<Context> context,
              std::shared_ptr<TypographerContext> typographer_context,
              std::optional<std::shared_ptr<RenderTargetAllocator>>
                  render_target_allocator = std::nullopt);

  ~AiksContext();

  bool IsValid() const;

  std::shared_ptr<Context> GetContext() const;

  ContentContext& GetContentContext() const;

  StaticPictureCache& GetStaticPictureCache() const;

  /// The multisample color and depth/stencil attachments of the last picture
  /// snapshot (DisplayListToTexture), kept for the next snapshot of the same
  /// size and format. They only live while a snapshot renders, but at four
  /// samples a full-screen snapshot needs 2x130 MB of them on a 3392x2400
  /// screen, which the driver would otherwise allocate and zero again for
  /// every snapshot, in its first frame.
  struct SnapshotAttachments {
    std::shared_ptr<Texture> color;
    std::shared_ptr<Texture> depth_stencil;
  };

  SnapshotAttachments& GetSnapshotAttachments();

  /// Drops the kept snapshot attachments (an application that is hidden
  /// keeps no GPU memory it does not need).
  void ReleaseSnapshotAttachments();

 private:
  std::shared_ptr<Context> context_;
  std::unique_ptr<ContentContext> content_context_;
  std::unique_ptr<StaticPictureCache> static_picture_cache_;
  SnapshotAttachments snapshot_attachments_;
  bool is_valid_ = false;

  AiksContext(const AiksContext&) = delete;

  AiksContext& operator=(const AiksContext&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_AIKS_CONTEXT_H_
