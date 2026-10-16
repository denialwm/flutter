// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_VIEW_EMBEDDER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_VIEW_EMBEDDER_H_

#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

#include "flutter/common/constants.h"
#include "flutter/flow/embedded_views.h"
#include "flutter/fml/hash_combine.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/shell/platform/embedder/embedder_external_view.h"
#include "flutter/shell/platform/embedder/embedder_render_target_cache.h"

namespace flutter {

//------------------------------------------------------------------------------
/// @brief      The external view embedder used by the generic embedder API.
///             This class acts a proxy between the rasterizer and the embedder
///             when the rasterizer is rendering into multiple layers. It asks
///             the embedder for the render targets for the various layers the
///             rasterizer is rendering into, recycles the render targets as
///             necessary and converts rasterizer specific metadata into an
///             embedder friendly format so that it can present the layers
///             on-screen.
///
class EmbedderExternalViewEmbedder final : public ExternalViewEmbedder {
 public:
  using CreateRenderTargetCallback =
      std::function<std::unique_ptr<EmbedderRenderTarget>(
          GrDirectContext* context,
          const std::shared_ptr<impeller::AiksContext>& aiks_context,
          const FlutterBackingStoreConfig& config)>;
  using PresentCallback =
      std::function<bool(FlutterViewId view_id,
                         const std::vector<const FlutterLayer*>& layers)>;
  using ExistingDamageCallback =
      std::function<std::optional<DlRegion>(intptr_t framebuffer)>;
  using SurfaceTransformationCallback = std::function<DlMatrix(void)>;

  //----------------------------------------------------------------------------
  /// @brief      Creates an external view embedder used by the generic embedder
  ///             API.
  ///
  /// @param[in] avoid_backing_store_cache If set, create_render_target_callback
  ///                                      will beinvoked every frame for every
  ///                                      engine composited layer. The result
  ///                                      will not cached.
  ///
  /// @param[in]  create_render_target_callback
  ///                                     The render target callback used to
  ///                                     request the render target for a layer.
  /// @param[in]  present_callback        The callback used to forward a
  ///                                     collection of layers (backed by
  ///                                     fulfilled render targets) to the
  ///                                     embedder for presentation.
  ///
  EmbedderExternalViewEmbedder(
      bool avoid_backing_store_cache,
      const CreateRenderTargetCallback& create_render_target_callback,
      const PresentCallback& present_callback);

  //----------------------------------------------------------------------------
  /// @brief      Collects the external view embedder.
  ///
  ~EmbedderExternalViewEmbedder() override;

  // |ExternalViewEmbedder|
  void CollectView(int64_t view_id) override;

  //----------------------------------------------------------------------------
  /// @brief      Sets the surface transformation callback used by the external
  ///             view embedder to ask the platform for the per frame root
  ///             surface transformation.
  ///
  /// @param[in]  surface_transformation_callback  The surface transformation
  ///                                              callback
  ///
  void SetSurfaceTransformationCallback(
      SurfaceTransformationCallback surface_transformation_callback);

  /// Selects the persistent backing store for one synthetic Denial output
  /// before layer-tree diffing. Buffer-age repair belongs to this exact FBO,
  /// so the rasterizer must know it before choosing the repaint region.
  std::optional<PreparedRenderTarget> PrepareDenialRenderTarget(
      int64_t flutter_view_id,
      GrDirectContext* context,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      const std::shared_ptr<TextureRegistry>& texture_registry) override;

  // Denial layered composition (docs/LAYERED_COMPOSITION.md in Denial).
  // Enabled by DENIAL_LAYERED_COMPOSITION=1 for Denial output views: cuts
  // registered during Preroll split the frame into engine-rendered slices
  // that are retained as textures while their display lists are unchanged.
  // The scanout target then receives one composition of slices and cuts.
  bool DenialLayeredComposition() const override;
  int64_t DenialRegisterCut(std::unique_ptr<EmbeddedViewParams> params,
                            DenialCutEntry entry) override;
  // Only texture-only frames are layered: once a view has drawn the same tree
  // for kDenialLayeredBuildStreak texture-only frames, one layered build
  // retains its entries, and the following texture-only frames recompose
  // them. Any other frame draws directly and drops the retained entries.
  DenialOutputMode DenialBeginOutputFrame(int64_t flutter_view_id,
                                          const void* layer_tree,
                                          bool texture_only) override;
  void DenialRecompose(
      int64_t flutter_view_id,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      std::unique_ptr<SurfaceFrame> frame) override;

  // GPU storage of one slice: the sampled color texture and its MSAA and
  // depth/stencil attachments, all of `size`. A slice renders into the
  // top-left corner, so storage is reused while the slice fits and otherwise
  // grows; it never shrinks while its view exists.
  struct DenialSliceStorage {
    DlISize size;
    std::shared_ptr<impeller::Texture> texture;
    std::shared_ptr<impeller::Texture> msaa;
    std::shared_ptr<impeller::Texture> depth_stencil;
    sk_sp<DlImage> image;
  };

  // One retained engine-rendered slice of a Denial output view.
  struct DenialSlice {
    sk_sp<DisplayList> display_list;
    DlIRect bounds;
    DenialSliceStorage storage;
  };

  // One entry of a layered build, in paint order: an engine slice (an index
  // into the view's slots) or a cut with its mutators.
  struct DenialComposedEntry {
    std::optional<size_t> slice;
    DenialCutEntry cut;
    EmbeddedViewParams params;
  };

  // The slices of one output view, in entry order, and the storage of slices
  // that disappeared or outgrew it, kept for the next slice that fits.
  // `entries` are the last layered build's, valid for recomposing `tree` at
  // `frame_size` and `transformation` while `tree` is set.
  struct DenialViewSlices {
    std::vector<DenialSlice> slots;
    std::vector<DenialSliceStorage> pool;
    std::vector<DenialComposedEntry> entries;
    const void* tree = nullptr;
    DlISize frame_size;
    DlMatrix transformation;
    // Consecutive texture-only frames, and a tree whose build registered no
    // cuts, which then stays direct.
    uint32_t texture_only_streak = 0;
    const void* unlayered_tree = nullptr;
  };

  void SetExistingDamageCallback(
      ExistingDamageCallback existing_damage_callback);

 private:
  friend class EmbedderExternalViewEmbedderTestPeer;

  // |ExternalViewEmbedder|
  void CancelFrame() override;

  // |ExternalViewEmbedder|
  void BeginFrame(GrDirectContext* context,
                  const fml::RefPtr<fml::RasterThreadMerger>&
                      raster_thread_merger) override;

  // |ExternalViewEmbedder|
  void PrepareFlutterView(int64_t flutter_view_id,
                          DlISize frame_size,
                          double device_pixel_ratio) override;

  // |ExternalViewEmbedder|
  void PrepareFlutterView(DlISize frame_size,
                          double device_pixel_ratio) override;

  // |ExternalViewEmbedder|
  void PrerollCompositeEmbeddedView(
      int64_t view_id,
      std::unique_ptr<EmbeddedViewParams> params) override;

  // |ExternalViewEmbedder|
  DlCanvas* CompositeEmbeddedView(int64_t view_id) override;

  // |ExternalViewEmbedder|
  void SubmitFlutterView(
      int64_t flutter_view_id,
      GrDirectContext* context,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      std::unique_ptr<SurfaceFrame> frame) override;

  // |ExternalViewEmbedder|
  DlCanvas* GetRootCanvas() override;

  // |ExternalViewEmbedder|
  std::optional<DlMatrix> GetRootCanvasToRenderTargetTransform() const override;

 private:
  const bool avoid_backing_store_cache_;
  const CreateRenderTargetCallback create_render_target_callback_;
  const PresentCallback present_callback_;
  ExistingDamageCallback existing_damage_callback_;
  SurfaceTransformationCallback surface_transformation_callback_;
  std::unique_ptr<EmbedderRenderTarget> pending_denial_render_target_;
  const bool denial_layered_enabled_;
  int64_t pending_flutter_view_id_ = kFlutterImplicitViewId;
  DenialOutputMode denial_output_mode_ = DenialOutputMode::kDirect;
  const void* denial_frame_tree_ = nullptr;
  int64_t denial_next_cut_ = 0;
  std::unordered_map<int64_t, DenialCutEntry> denial_cuts_;
  std::unordered_map<int64_t, DenialViewSlices> denial_slices_;
  std::shared_ptr<TextureRegistry> denial_texture_registry_;

  // Per-view layered composition counters. Collected only when
  // DENIAL_RENDER_AUDIT is on and logged once per second beside deniald's
  // "Flutter per-output render audit".
  struct DenialLayeredAudit {
    fml::TimePoint interval_start;
    // Frames by mode: drawn directly, layered builds, recompositions; and
    // builds whose tree registered no cuts.
    uint64_t direct_frames = 0;
    uint64_t layered_frames = 0;
    uint64_t recomposed_frames = 0;
    uint64_t single_slice_frames = 0;
    uint64_t failed_frames = 0;
    // Output pixels composed: the buffer damage of layered frames.
    uint64_t composed_pixels = 0;
    // Cuts by kind; material ends are not counted.
    uint64_t window_cuts = 0;
    uint64_t material_cuts = 0;
    uint64_t texture_cuts = 0;
    uint64_t max_cuts = 0;
    uint64_t slices = 0;
    uint64_t max_slices = 0;
    uint64_t slices_reused = 0;
    // Why a slice was re-rendered: no retained texture yet, changed bounds,
    // or an unequal display list.
    uint64_t rendered_new = 0;
    uint64_t rendered_bounds = 0;
    uint64_t rendered_content = 0;
    // Re-rendering clears and draws the whole storage (`rendered_pixels`),
    // which can exceed the slice's own rounded extent (`rendered_slice_pixels`)
    // when storage grew for an earlier, larger slice.
    uint64_t rendered_pixels = 0;
    uint64_t rendered_slice_pixels = 0;
    // Storage created, and storage taken from the pool instead.
    uint64_t slice_allocations = 0;
    uint64_t slice_pool_reuses = 0;
    fml::TimeDelta slice_render_time;
    fml::TimeDelta compose_time;
    fml::TimeDelta compose_max;
    fml::TimeDelta recompose_time;
    fml::TimeDelta recompose_max;
  };
  const bool denial_layered_audit_enabled_;
  std::unordered_map<int64_t, DenialLayeredAudit> denial_layered_audits_;
  // Returns the counters for the frame being submitted, logging and
  // restarting the interval first when a second has passed, or nullptr when
  // the audit is off.
  DenialLayeredAudit* DenialLayeredAuditFrame(int64_t flutter_view_id);

  void SubmitDenialLayered(
      int64_t flutter_view_id,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      std::unique_ptr<SurfaceFrame> frame);
  // Composes `view`'s entries onto the pending output target inside
  // `buffer_damage`, keeping the target's pixels outside it.
  bool DenialComposeEntries(
      const DenialViewSlices& view,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      const std::optional<DlRegion>& buffer_damage,
      DenialLayeredAudit* audit);
  // Presents the pending output target, or skips the frame when it was not
  // composed, then submits `frame`.
  void DenialPresentLayered(int64_t flutter_view_id,
                            bool composed,
                            std::unique_ptr<SurfaceFrame> frame);
  DlISize pending_frame_size_;
  double pending_device_pixel_ratio_ = 1.0;
  DlMatrix pending_surface_transformation_;
  EmbedderExternalView::PendingViews pending_views_;
  std::vector<EmbedderExternalView::ViewIdentifier> composition_order_;
  // The render target caches for views. Each key is a view ID.
  std::unordered_map<int64_t, EmbedderRenderTargetCache> render_target_caches_;

  void Reset();

  DlMatrix GetSurfaceTransformation() const;

  FML_DISALLOW_COPY_AND_ASSIGN(EmbedderExternalViewEmbedder);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_VIEW_EMBEDDER_H_
