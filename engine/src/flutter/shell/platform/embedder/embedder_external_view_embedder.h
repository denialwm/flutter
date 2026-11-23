// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_VIEW_EMBEDDER_H_
#define FLUTTER_SHELL_PLATFORM_EMBEDDER_EMBEDDER_EXTERNAL_VIEW_EMBEDDER_H_

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include <unordered_map>

#include "flutter/common/constants.h"
#include "flutter/flow/embedded_views.h"
#include "flutter/fml/hash_combine.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/time/time_point.h"
#include "flutter/shell/platform/embedder/embedder.h"
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
  // Always on for Denial output views (negative view ids): cuts
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
  bool DenialTakeSubmitFailure(int64_t flutter_view_id) override;

  void DenialRecompose(
      int64_t flutter_view_id,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      std::unique_ptr<SurfaceFrame> frame) override;
  bool DenialPinBackdrop(int64_t token) override;
  // The engine composes `flutter_view_id`'s frame. After a frame the scene
  // composer drew, or with a backdrop it pinned, neither the target nor the
  // damage history is the engine's: the frame repaints and reports the whole
  // target. Returns whether it does.
  bool DenialTakeComposerHandover(int64_t flutter_view_id, SurfaceFrame& frame);

  // GPU storage of one slice: its sampled color texture of `size`. The
  // attachments that only matter while rendering (depth/stencil, and MSAA
  // when the slice uses it) are created per render. A slice renders into
  // the top-left corner, so storage is reused while the slice fits in at
  // most twice its area. `bottom_up` storage renders without the GLES
  // offscreen flip, as the output's FBO does (DenialRenderSlice).
  struct DenialSliceStorage {
    DlISize size;
    bool bottom_up = false;
    std::shared_ptr<impeller::Texture> texture;
    sk_sp<DlImage> image;
    // Set when the scene composer provided the storage: the immutable
    // version this texture holds. Never rendered into again once published.
    std::optional<DenialFlutterSliceKey> composer_key;
    // Written, and not yet listed as rewritten in a staged scene.
    bool composer_unpublished = false;
  };

  // A slice render's multisample color and depth/stencil attachments. Every
  // slice pass clears them and stores neither, so later renders of the same
  // size reuse them instead of allocating their own (Vulkan; Impeller's
  // render target cache does the same for its offscreen passes).
  struct DenialSliceAttachments {
    std::shared_ptr<impeller::Texture> msaa_color;
    std::shared_ptr<impeller::Texture> depth_stencil;
    uint64_t last_frame = 0;
  };

  // One retained engine-rendered slice of a Denial output view. `rects` are
  // the parts of `bounds` the slice paints, which composition blits instead
  // of the whole bounds when they cover markedly less.
  struct DenialSlice {
    sk_sp<DisplayList> display_list;
    DlIRect bounds;
    // The surface transformation the storage was rendered with.
    DlMatrix transformation;
    // Whether the storage was rendered multisampled.
    bool msaa = false;
    std::vector<DlIRect> rects;
    DenialSliceStorage storage;
  };

  // One entry of a layered build, in paint order: an engine slice (an index
  // into the view's slots) or a cut with its mutators.
  struct DenialComposedEntry {
    std::optional<size_t> slice;
    DenialCutEntry cut;
    EmbeddedViewParams params;
  };

  // The slices of one output view, in entry order. `entries` are the last
  // layered build's, valid for recomposing `tree` at `frame_size` and
  // `transformation` while `tree` is set.
  struct DenialViewSlices {
    std::vector<DenialSlice> slots;
    std::vector<DenialComposedEntry> entries;
    const void* tree = nullptr;
    DlISize frame_size;
    DlMatrix transformation;
    // Consecutive texture-only frames, and a tree whose build registered no
    // cuts, which then stays direct.
    uint32_t texture_only_streak = 0;
    const void* unlayered_tree = nullptr;
    // Scene composer: the revision of `entries`, bumped by every layered
    // build.
    uint64_t scene_revision = 0;
    // Why the scene composer cannot show `tree`, or nullptr. Structural:
    // holds for every recomposition of the tree.
    const char* composer_ineligible = nullptr;
    // A slot of `tree` sits in engine storage because the composer refused
    // a writer, and whether `tree` already rebuilt once to retry.
    bool composer_engine_slots = false;
    bool composer_retried = false;
    // The scene composer drew the view's last frame.
    bool composer_drew = false;
  };

  // Hands output views to the embedder's scene composer.
  void SetDenialSceneComposer(DenialFlutterSceneComposer composer);

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
  // The mode policy of DenialBeginOutputFrame.
  DenialOutputMode DenialChooseOutputMode(int64_t flutter_view_id,
                                          const void* layer_tree,
                                          bool texture_only);

  const bool avoid_backing_store_cache_;
  const CreateRenderTargetCallback create_render_target_callback_;
  const PresentCallback present_callback_;
  ExistingDamageCallback existing_damage_callback_;
  SurfaceTransformationCallback surface_transformation_callback_;
  std::unique_ptr<EmbedderRenderTarget> pending_denial_render_target_;
  int64_t pending_flutter_view_id_ = kFlutterImplicitViewId;
  DenialOutputMode denial_output_mode_ = DenialOutputMode::kDirect;
  const void* denial_frame_tree_ = nullptr;
  int64_t denial_next_cut_ = 0;
  std::unordered_map<int64_t, DenialCutEntry> denial_cuts_;
  std::unordered_map<int64_t, DenialViewSlices> denial_slices_;
  std::shared_ptr<TextureRegistry> denial_texture_registry_;
  std::optional<DenialFlutterSceneComposer> denial_scene_composer_;
  // The scene composer's ticket for the output frame being drawn; zero when
  // the engine composes it.
  uint64_t denial_ticket_ = 0;
  // The scene composer pinned a backdrop for this frame's damage plan: if
  // the engine composes the frame after all, it repaints all of it (no
  // Impeller snapshot stands behind the pin).
  bool denial_composer_pinned_ = false;
  // Why this frame's registered cuts keep it from the scene composer, found
  // during preroll; nullptr when they don't.
  const char* denial_composer_ineligible_ = nullptr;
  // Logged once per reason: why a frame fell back to engine composition.
  uint32_t denial_stage_fallbacks_logged_ = 0;

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
    // Output pixels composed: the buffer damage of layered frames; and the
    // slice pixels blitted there, before the damage clip.
    uint64_t composed_pixels = 0;
    uint64_t slice_blit_pixels = 0;
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
    // Storage created, and storage taken over instead from a slice that
    // released it earlier in the same build.
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
  // Composes `view`'s entries onto `target` (null: the pending output
  // target) inside `buffer_damage`, keeping the target's pixels outside it.
  bool DenialComposeEntries(
      const DenialViewSlices& view,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      const std::optional<DlRegion>& buffer_damage,
      DenialLayeredAudit* audit,
      bool* transients_reset,
      impeller::RenderTarget* target = nullptr);
  // Vulkan renderer, a frame the scene composer could not stage: composes
  // `view`'s entries in the engine, as the GLES renderer does, into a
  // full-frame storage version of the composer, with the client images it
  // lends, and stages a scene of that one slice under the same ticket. False
  // skips the frame. `reason` is why the composer could not take it;
  // `transients_reset` as for DenialComposeEntries.
  bool DenialVulkanFallback(
      int64_t flutter_view_id,
      const DenialViewSlices& view,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      DenialLayeredAudit* audit,
      std::unique_ptr<SurfaceFrame>& frame,
      const char* reason,
      bool* transients_reset);
  // The composer lends client images (Vulkan renderer): frames it cannot
  // stage keep their ticket for DenialVulkanFallback.
  bool denial_vulkan_fallback_ = false;
  uint64_t denial_fallback_revision_ = 0;
  uint64_t denial_fallbacks_ = 0;
  // Scene composer: describes `view`'s entries for the embedder and stages
  // them for this frame's ticket. False when an entry cannot be described or
  // the embedder declines; the engine then composes the frame itself.
  // A staged frame also runs the reactor: the engine issues no GLES work
  // for it.
  bool DenialStageScene(
      int64_t flutter_view_id,
      DenialViewSlices& view,
      const std::shared_ptr<impeller::AiksContext>& aiks_context);
  // A frame of a target only the scene composer draws that it did not
  // stage: logged and counted; the caller skips the frame.
  void DenialLogDecline(int64_t flutter_view_id, const char* reason);
  uint64_t denial_declines_ = 0;
  // Views whose last Denial frame was not presented (DenialTakeSubmitFailure).
  std::set<int64_t> denial_failed_views_;
  // Views whose frame took the composer handover's full damage: it stays
  // owed when that frame is not presented.
  std::set<int64_t> denial_handover_taken_;
  // Slice attachments by width, height and multisampling, with the layered
  // build that last used them (`denial_slice_frame_`).
  std::map<std::tuple<int64_t, int64_t, bool>, DenialSliceAttachments>
      denial_slice_attachments_;
  uint64_t denial_slice_frame_ = 0;
  void DenialLogComposerFallback(int64_t flutter_view_id, const char* reason);
  // Ends a begun ticket that was not staged.
  void DenialCancelTicket();
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
