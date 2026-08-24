// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_ENTITY_CONTENTS_BACKDROP_SURFACE_CONTENTS_H_
#define FLUTTER_IMPELLER_ENTITY_CONTENTS_BACKDROP_SURFACE_CONTENTS_H_

#include <memory>
#include <optional>

#include "impeller/entity/contents/contents.h"
#include "impeller/entity/contents/texture_contents.h"
#include "impeller/entity/entity.h"
#include "impeller/renderer/snapshot.h"

namespace impeller {

/// A physical plan for the common compositor expression
///
///   surface over filtered(scene)
///
/// evaluated in one destination draw. The backdrop filter still produces its
/// deliberately low-resolution texture; this contents fuses only the final
/// upsample, external-surface blend, transparent-pixel mask, and optional
/// analytic rounded coverage.
class BackdropSurfaceContents final : public Contents {
 public:
  struct AnalyticRRect {
    Rect bounds;
    Size radii;
  };

  static std::shared_ptr<BackdropSurfaceContents> Make(
      const Entity& backdrop_entity,
      const std::shared_ptr<TextureContents>& backdrop_contents,
      const std::optional<Snapshot>& scene_snapshot,
      const std::shared_ptr<TextureContents>& surface_contents,
      const Matrix& surface_transform,
      const Rect& composite_coverage,
      std::optional<AnalyticRRect> analytic_clip = std::nullopt);

  ~BackdropSurfaceContents() override;

  std::optional<Rect> GetCoverage(const Entity& entity) const override;

  bool Render(const ContentContext& renderer,
              const Entity& entity,
              RenderPass& pass) const override;

 private:
  struct TextureInput {
    std::shared_ptr<Texture> texture;
    Rect source_rect;
    Rect destination_rect;
    SamplerDescriptor sampler;
    Matrix transform;
    Scalar opacity = 1.0f;
  };

  BackdropSurfaceContents(TextureInput backdrop,
                          std::optional<Snapshot> scene,
                          std::shared_ptr<TextureContents> surface,
                          Rect destination,
                          std::optional<AnalyticRRect> analytic_clip);

  TextureInput backdrop_;
  std::optional<Snapshot> scene_;
  std::shared_ptr<TextureContents> surface_;
  Rect destination_;
  std::optional<AnalyticRRect> analytic_clip_;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_ENTITY_CONTENTS_BACKDROP_SURFACE_CONTENTS_H_
