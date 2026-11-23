// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/static_picture_cache.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/utils/dl_receiver_utils.h"
#include "flutter/fml/hash_combine.h"
#include "impeller/core/sampler_descriptor.h"
#include "impeller/display_list/canvas.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/renderer/render_target.h"

#ifdef IMPELLER_ENABLE_OPENGLES
#include "impeller/renderer/backend/gles/capabilities_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"
#endif  // IMPELLER_ENABLE_OPENGLES

namespace impeller {
namespace {

constexpr uint32_t kAllocationFailureRetryCooldown = 60u;

class StaticPictureEligibility final
    : public flutter::IgnoreAttributeDispatchHelper,
      public flutter::IgnoreClipDispatchHelper,
      public flutter::IgnoreTransformDispatchHelper,
      public flutter::IgnoreDrawDispatchHelper {
 public:
  bool is_eligible() const { return is_eligible_; }

  void setBlendMode(flutter::DlBlendMode mode) override {
    if (mode != flutter::DlBlendMode::kSrcOver) {
      is_eligible_ = false;
    }
  }

  void setColorSource(const flutter::DlColorSource* source) override {
    if (source != nullptr) {
      is_eligible_ = false;
    }
  }

  void setColorFilter(const flutter::DlColorFilter* filter) override {
    if (filter != nullptr) {
      is_eligible_ = false;
    }
  }

  void setInvertColors(bool invert) override {
    if (invert) {
      is_eligible_ = false;
    }
  }

  void setImageFilter(const flutter::DlImageFilter* filter) override {
    if (filter != nullptr) {
      is_eligible_ = false;
    }
  }

  void saveLayer(const flutter::DlRect&,
                 flutter::SaveLayerOptions,
                 const flutter::DlImageFilter*,
                 std::optional<int64_t>) override {
    is_eligible_ = false;
  }

  void drawColor(flutter::DlColor, flutter::DlBlendMode) override {
    is_eligible_ = false;
  }

  void drawPaint() override { is_eligible_ = false; }

  void drawVertices(const std::shared_ptr<flutter::DlVertices>&,
                    flutter::DlBlendMode) override {
    is_eligible_ = false;
  }

  void drawImage(const sk_sp<flutter::DlImage>,
                 const flutter::DlPoint&,
                 flutter::DlImageSampling,
                 bool) override {
    is_eligible_ = false;
  }

  void drawImageRect(const sk_sp<flutter::DlImage>,
                     const flutter::DlRect&,
                     const flutter::DlRect&,
                     flutter::DlImageSampling,
                     bool,
                     flutter::DlSrcRectConstraint) override {
    is_eligible_ = false;
  }

  void drawImageNine(const sk_sp<flutter::DlImage>,
                     const flutter::DlIRect&,
                     const flutter::DlRect&,
                     flutter::DlFilterMode,
                     bool) override {
    is_eligible_ = false;
  }

  void drawAtlas(const sk_sp<flutter::DlImage>,
                 const flutter::DlRSTransform[],
                 const flutter::DlRect[],
                 const flutter::DlColor[],
                 int,
                 flutter::DlBlendMode,
                 flutter::DlImageSampling,
                 const flutter::DlRect*,
                 bool) override {
    is_eligible_ = false;
  }

  void drawDisplayList(const sk_sp<flutter::DisplayList>,
                       flutter::DlScalar) override {
    is_eligible_ = false;
  }

  void drawText(const std::shared_ptr<flutter::DlText>&,
                flutter::DlScalar,
                flutter::DlScalar) override {
    is_eligible_ = false;
  }

  void drawShadow(const flutter::DlPath&,
                  flutter::DlColor,
                  flutter::DlScalar,
                  bool,
                  flutter::DlScalar) override {
    is_eligible_ = false;
  }

 private:
  bool is_eligible_ = true;
};

float FractionalPart(float value) {
  float result = value - std::floor(value);
  return result == 0.0f ? 0.0f : result;
}

}  // namespace

StaticPictureCache::StaticPictureCache(ContentContext& renderer)
    : StaticPictureCache(renderer, Limits()) {}

StaticPictureCache::StaticPictureCache(ContentContext& renderer,
                                       Limits limits,
                                       Snapshotter snapshotter)
    : renderer_(renderer),
      limits_(limits),
      snapshotter_(std::move(snapshotter)) {}

StaticPictureCache::~StaticPictureCache() = default;

bool StaticPictureCache::Key::operator==(const Key& other) const {
  return display_list_id == other.display_list_id && scale_x == other.scale_x &&
         scale_y == other.scale_y && alignment_x == other.alignment_x &&
         alignment_y == other.alignment_y && pass == other.pass;
}

size_t StaticPictureCache::Key::Hash::operator()(const Key& key) const {
  return fml::HashCombine(
      key.display_list_id, key.scale_x, key.scale_y, key.alignment_x,
      key.alignment_y, static_cast<int>(key.pass.sample_count),
      static_cast<int>(key.pass.format), key.pass.bottom_up);
}

std::optional<StaticPictureCache::Plan> StaticPictureCache::MakePlan(
    const sk_sp<flutter::DisplayList>& display_list,
    const Matrix& transform,
    const Pass& pass) {
  if (!display_list || !transform.IsTranslationScaleOnly() ||
      !std::isfinite(transform.m[0]) || !std::isfinite(transform.m[5]) ||
      !std::isfinite(transform.m[12]) || !std::isfinite(transform.m[13]) ||
      !std::isfinite(transform.m[10]) || !std::isfinite(transform.m[14])) {
    return std::nullopt;
  }

  const Rect& local_bounds = display_list->GetBounds();
  if (local_bounds.IsEmpty() || !local_bounds.IsFinite()) {
    return std::nullopt;
  }

  const Rect device_bounds = local_bounds.TransformBounds(transform);
  if (device_bounds.IsEmpty() || !device_bounds.IsFinite()) {
    return std::nullopt;
  }
  const IRect rounded = IRect::RoundOut(device_bounds);
  if (rounded.IsEmpty()) {
    return std::nullopt;
  }

  const int64_t width = rounded.GetWidth();
  const int64_t height = rounded.GetHeight();
  const size_t bytes_per_pixel =
      pass.format == PixelFormat::kUnknown
          ? 4u
          : std::max<size_t>(1u, BytesPerPixelForPixelFormat(pass.format));
  if (width <= 0 || height <= 0 ||
      static_cast<uint64_t>(width) > std::numeric_limits<size_t>::max() /
                                         bytes_per_pixel /
                                         static_cast<uint64_t>(height)) {
    return std::nullopt;
  }

  // Rasterize in the pass's own pixel coordinates, moved only by whole
  // pixels. Impeller rounds some coordinates in pass space: a
  // non-antialiased rect clip becomes a scissor through Rect::Round, which
  // takes halves away from zero, so a picture rendered in any other
  // orientation (as this cache once un-reflected Denial's Y-reflected GLES
  // output) puts an edge on a half pixel into the other row. The pass's
  // sample count and row order (CreateSnapshot) do the rest.
  // IsTranslationScaleOnly already excludes zero scales, skew and perspective.
  const Matrix origin =
      Matrix::MakeTranslation({static_cast<Scalar>(rounded.GetLeft()),
                               static_cast<Scalar>(rounded.GetTop()), 0.0f});

  return Plan{
      .key = {.display_list_id = display_list->unique_id(),
              .scale_x = transform.m[0],
              .scale_y = transform.m[5],
              .alignment_x = FractionalPart(device_bounds.GetLeft()),
              .alignment_y = FractionalPart(device_bounds.GetTop()),
              .pass = pass},
      .device_bounds = rounded,
      .snapshot_transform = origin.Invert() * transform,
      .estimated_bytes = static_cast<size_t>(width) *
                         static_cast<size_t>(height) * bytes_per_pixel,
  };
}

bool StaticPictureCache::IsPictureEligible(
    const sk_sp<flutter::DisplayList>& display_list) {
  if (!display_list || display_list->op_count() == 0u ||
      display_list->GetBounds().IsEmpty() ||
      !display_list->GetBounds().IsFinite() ||
      display_list->root_has_backdrop_filter() ||
      display_list->root_is_unbounded()) {
    return false;
  }

  StaticPictureEligibility receiver;
  display_list->Dispatch(receiver);
  return receiver.is_eligible();
}

bool StaticPictureCache::IsPictureEligibleForTesting(
    const sk_sp<flutter::DisplayList>& display_list) {
  return IsPictureEligible(display_list);
}

bool StaticPictureCache::IsEligible(
    const sk_sp<flutter::DisplayList>& display_list) {
  const auto found = eligibility_.find(display_list->unique_id());
  if (found != eligibility_.end()) {
    found->second.last_access = ++access_;
    return found->second.eligible;
  }

  const bool eligible = IsPictureEligible(display_list);
  statistics_.eligibility_checks++;
  if (!eligible) {
    statistics_.rejected_pictures++;
  }
  eligibility_.emplace(display_list->unique_id(),
                       EligibilityRecord{eligible, ++access_});
  TrimEligibilityRecords();
  return eligible;
}

bool StaticPictureCache::Observe(const Plan& plan) {
  auto [found, inserted] =
      observations_.try_emplace(plan.key, Observation{0u, 0u, ++access_});
  Observation& observation = found->second;
  observation.last_access = ++access_;
  if (observation.retry_cooldown > 0u) {
    observation.retry_cooldown--;
    statistics_.admission_misses++;
    TrimObservations();
    return false;
  }
  observation.count++;
  statistics_.admission_misses++;
  if (observation.count < std::max(1u, limits_.admission_threshold)) {
    TrimObservations();
    return false;
  }
  observations_.erase(found);
  return true;
}

void StaticPictureCache::Request(
    const sk_sp<flutter::DisplayList>& display_list) {
  if (!display_list) {
    return;
  }
  requests_.insert_or_assign(display_list->unique_id(), ++access_);
  TrimRequests();
}

std::optional<StaticPictureCache::Pass> StaticPictureCache::DescribePass(
    const RenderTarget& target) const {
  if (!target.HasColorAttachment(0u)) {
    return std::nullopt;
  }
  const std::shared_ptr<Texture>& color = target.GetColorAttachment(0u).texture;
  if (!color) {
    return std::nullopt;
  }
  const TextureDescriptor& descriptor = color->GetTextureDescriptor();
  // Impeller renders every multisampled pass at four samples.
  if (descriptor.sample_count != SampleCount::kCount1 &&
      descriptor.sample_count != SampleCount::kCount4) {
    return std::nullopt;
  }
  Pass pass{.sample_count = descriptor.sample_count,
            .format = descriptor.format};
#ifdef IMPELLER_ENABLE_OPENGLES
  if (renderer_.GetContext()->GetBackendType() ==
      Context::BackendType::kOpenGLES) {
    // RenderPassGLES flips every other pass at the vertex stage.
    const TextureGLES& gles = TextureGLES::Cast(*color);
    pass.bottom_up = gles.IsWrapped() || gles.IsStoredBottomUp();
    // The snapshot carries the picture's coverage in its alpha, which the
    // live draw blends with at full precision. A 10-bit output keeps two
    // bits of alpha: a soft shadow stored in it falls to four levels. Store
    // such a picture at half float, which holds the 10-bit colour and the
    // alpha alike.
    if (pass.format == PixelFormat::kR10G10B10A2UNormInt) {
      pass.format =
          CapabilitiesGLES::Cast(*renderer_.GetContext()->GetCapabilities())
                  .SupportsHalfFloatColorAttachments()
              ? PixelFormat::kR16G16B16A16Float
              : PixelFormat::kR8G8B8A8UNormInt;
    }
  }
#endif  // IMPELLER_ENABLE_OPENGLES
  return pass;
}

bool StaticPictureCache::Draw(const sk_sp<flutter::DisplayList>& display_list,
                              Canvas& canvas) {
  // A snapshot's own replay of the picture is live.
  if (generating_ || !display_list) {
    return false;
  }
  const auto requested = requests_.find(display_list->unique_id());
  if (requested == requests_.end()) {
    return false;
  }
  requested->second = ++access_;
  // Nothing of it shows; the live replay skips it as well.
  if (!canvas.GetLocalCoverageLimit().has_value()) {
    return false;
  }
  const RenderTarget* target = canvas.GetCurrentPassRenderTarget();
  const std::optional<Pass> pass =
      target ? DescribePass(*target) : std::nullopt;
  if (!pass.has_value()) {
    statistics_.rejected_passes++;
    return false;
  }
  const Matrix pass_transform = canvas.GetCurrentPassTransform();
  const std::optional<Image> image =
      Lookup(display_list, pass_transform, pass.value());
  if (!image.has_value()) {
    return false;
  }

  // Draw the image's pixels one to one onto the pass pixels they were
  // rendered for: under the current transform and then this one, the image
  // lands on the pass by a translation to its origin.
  const Matrix image_transform =
      pass_transform.Invert() *
      Matrix::MakeTranslation({static_cast<Scalar>(image->origin.x),
                               static_cast<Scalar>(image->origin.y), 0.0f});
  const Matrix saved = canvas.GetCurrentTransform();
  canvas.Concat(image_transform);
  const Rect bounds = Rect::MakeSize(image->texture->GetSize());
  SamplerDescriptor nearest;
  nearest.min_filter = MinMagFilter::kNearest;
  nearest.mag_filter = MinMagFilter::kNearest;
  canvas.DrawImageRect(image->texture, bounds, bounds, Paint{}, nearest,
                       SourceRectConstraint::kStrict);
  canvas.ResetTransform();
  canvas.Concat(saved);
  return true;
}

std::optional<StaticPictureCache::Image> StaticPictureCache::Lookup(
    const sk_sp<flutter::DisplayList>& display_list,
    const Matrix& pass_transform,
    const Pass& pass) {
  const std::optional<Plan> plan = MakePlan(display_list, pass_transform, pass);
  if (!plan.has_value()) {
    statistics_.rejected_transforms++;
    return std::nullopt;
  }
  const IPoint origin = plan->device_bounds.GetOrigin();

  auto found = entries_.find(plan->key);
  if (found != entries_.end()) {
    found->second.last_access = ++access_;
    found->second.last_draw = ++found->second.picture->draws;
    statistics_.hits++;
    return Image{found->second.texture, origin};
  }

  if (!IsEligible(display_list)) {
    return std::nullopt;
  }
  if (limits_.max_entries == 0u || limits_.max_bytes == 0u ||
      plan->estimated_bytes > limits_.max_entry_bytes ||
      plan->estimated_bytes > limits_.max_bytes) {
    statistics_.rejected_by_budget++;
    return std::nullopt;
  }
  if (!Observe(*plan)) {
    return std::nullopt;
  }

  flutter::DisplayListBuilder snapshot_builder(flutter::DlRect::MakeWH(
      plan->device_bounds.GetWidth(), plan->device_bounds.GetHeight()));
  snapshot_builder.Transform(plan->snapshot_transform);
  snapshot_builder.DrawDisplayList(display_list);
  sk_sp<flutter::DisplayList> snapshot_display_list = snapshot_builder.Build();
  const ISize snapshot_size(plan->device_bounds.GetSize());
  std::shared_ptr<Texture> texture;
  {
    // The snapshot renders while the frame's canvas is mid-replay: it must
    // neither come back here nor see the frame's repaint clip.
    generating_ = true;
    const std::optional<Rect> repaint_bounds =
        renderer_.GetFrameRepaintBounds();
    renderer_.SetFrameRepaintBounds(std::nullopt);
    texture = snapshotter_
                  ? snapshotter_(snapshot_display_list, snapshot_size)
                  : CreateSnapshot(snapshot_display_list, snapshot_size, pass);
    renderer_.SetFrameRepaintBounds(repaint_bounds);
    generating_ = false;
  }
  if (!texture) {
    statistics_.allocation_failures++;
    observations_.insert_or_assign(
        plan->key, Observation{0u, kAllocationFailureRetryCooldown, ++access_});
    TrimObservations();
    return std::nullopt;
  }

  const size_t bytes =
      texture->GetTextureDescriptor().GetByteSizeOfBaseMipLevel();
  if (bytes > limits_.max_entry_bytes || bytes > limits_.max_bytes) {
    statistics_.rejected_by_budget++;
    return std::nullopt;
  }
  MakeRoom(bytes);

  Picture& picture = pictures_[plan->key.display_list_id];
  if (!picture.display_list) {
    picture.display_list = display_list;
  }
  Entry entry{texture, &picture, bytes, ++access_, ++picture.draws};
  auto [inserted, did_insert] = entries_.emplace(plan->key, std::move(entry));
  if (!did_insert) {
    if (picture.entries == 0u) {
      pictures_.erase(plan->key.display_list_id);
    }
    return std::nullopt;
  }
  picture.entries++;
  resident_bytes_ += bytes;
  statistics_.generations++;
  return Image{std::move(texture), origin};
}

std::shared_ptr<Texture> StaticPictureCache::CreateSnapshot(
    const sk_sp<flutter::DisplayList>& snapshot_display_list,
    ISize size,
    const Pass& pass) {
  // Static pictures exclude text, images, saveLayers, and backdrops. Render
  // them directly into a persistent, transparent target shaped like the pass
  // the picture lands in. This intentionally avoids DisplayListToTexture's
  // text-shadow frame markers, glyph reset, transient-buffer reset, and
  // thread-local cleanup: a static picture snapshot has no work that
  // requires those global frame hooks, and invoking them here could evict
  // unrelated caches while the frame is being replayed.
  const std::shared_ptr<Context>& context = renderer_.GetContext();
  const PixelFormat format =
      pass.format == PixelFormat::kUnknown
          ? context->GetCapabilities()->GetDefaultColorFormat()
          : pass.format;
  TextureDescriptor descriptor;
  descriptor.storage_mode = StorageMode::kDevicePrivate;
  descriptor.format = format;
  descriptor.size = size;
  descriptor.mip_count = 1u;
  descriptor.usage = TextureUsage::kRenderTarget | TextureUsage::kShaderRead;
  std::shared_ptr<Texture> texture =
      context->GetResourceAllocator()->CreateTexture(descriptor);
  if (!texture) {
    return nullptr;
  }
  texture->SetLabel("Static Picture Cache");

  RenderTargetAllocator allocator(context->GetResourceAllocator());
  RenderTarget target;
  if (pass.sample_count == SampleCount::kCount1) {
    target = allocator.CreateOffscreen(
        *context, size, 1u, "Static Picture Cache",
        RenderTarget::kDefaultColorAttachmentConfig,
        RenderTarget::kDefaultStencilAttachmentConfig, texture,
        /*existing_depth_stencil_texture=*/nullptr, format);
  } else {
    // A multisampled pass resolves the picture's edges; resolve them alike.
    target = allocator.CreateOffscreenMSAA(
        *context, size, 1u, "Static Picture Cache",
        RenderTarget::AttachmentConfigMSAA{
            .storage_mode = StorageMode::kDeviceTransient,
            .resolve_storage_mode = StorageMode::kDevicePrivate,
            .load_action = LoadAction::kClear,
            .store_action = StoreAction::kMultisampleResolve,
            .clear_color = Color::BlackTransparent()},
        RenderTarget::kDefaultStencilAttachmentConfig,
        /*existing_color_msaa_texture=*/nullptr, texture,
        /*existing_depth_stencil_texture=*/nullptr, format);
  }
  if (!target.IsValid()) {
    return nullptr;
  }
#ifdef IMPELLER_ENABLE_OPENGLES
  // Rendered without the offscreen vertex flip, as the pass is; the texture
  // the pass draws to decides it, the resolved one samples with the
  // matching Y scale.
  if (pass.bottom_up &&
      context->GetBackendType() == Context::BackendType::kOpenGLES) {
    TextureGLES::Cast(*target.GetColorAttachment(0u).texture)
        .SetStoredBottomUp();
    TextureGLES::Cast(*texture).SetStoredBottomUp();
  }
#endif  // IMPELLER_ENABLE_OPENGLES

  // A snapshot with a failed pass misses draws: it is never cached, so a
  // retry of the frame renders it again.
  const uint64_t failures = renderer_.GetPassFailures();
  CanvasDlDispatcher dispatcher(renderer_, target, false,
                                /*has_root_backdrop_filter=*/false,
                                flutter::DlBlendMode::kSrcOver,
                                IRect32::MakeSize(size));
  snapshot_display_list->Dispatch(
      dispatcher, flutter::DlIRect::MakeWH(size.width, size.height));
  if (!dispatcher.FinishRecording() ||
      renderer_.GetPassFailures() != failures) {
    return nullptr;
  }
  return texture;
}

void StaticPictureCache::Clear() {
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    entry = EraseEntry(entry);
  }
}

void StaticPictureCache::Collect() {
  for (auto entry = entries_.begin(); entry != entries_.end();) {
    const Picture& picture = *entry->second.picture;
    if (picture.display_list->unique()) {
      statistics_.collected++;
      entry = EraseEntry(entry);
    } else if (picture.draws - entry->second.last_draw >
               limits_.stale_key_draws) {
      statistics_.stale_keys++;
      entry = EraseEntry(entry);
    } else {
      ++entry;
    }
  }
}

StaticPictureCache::Entries::iterator StaticPictureCache::EraseEntry(
    Entries::iterator entry) {
  resident_bytes_ -= entry->second.bytes;
  Picture* picture = entry->second.picture;
  const uint32_t picture_id = entry->first.display_list_id;
  const auto next = entries_.erase(entry);
  if (--picture->entries == 0u) {
    pictures_.erase(picture_id);
  }
  return next;
}

std::string StaticPictureCache::Describe() const {
  std::ostringstream out;
  out << "entries=" << entries_.size() << " bytes=" << resident_bytes_
      << " hits=" << statistics_.hits
      << " generations=" << statistics_.generations
      << " evictions=" << statistics_.evictions
      << " collected=" << statistics_.collected
      << " stale_keys=" << statistics_.stale_keys
      << " rejected_by_budget=" << statistics_.rejected_by_budget
      << " rejected_transforms=" << statistics_.rejected_transforms
      << " rejected_passes=" << statistics_.rejected_passes;
  for (const auto& [key, entry] : entries_) {
    const ISize size = entry.texture->GetSize();
    out << " | picture=" << key.display_list_id << " scale=" << key.scale_x
        << "," << key.scale_y << " alignment=" << key.alignment_x << ","
        << key.alignment_y
        << " samples=" << static_cast<int>(key.pass.sample_count)
        << " format=" << PixelFormatToString(key.pass.format)
        << " bottom_up=" << key.pass.bottom_up << " size=" << size.width << "x"
        << size.height << " bytes=" << entry.bytes
        << " idle=" << access_ - entry.last_access
        << " behind=" << entry.picture->draws - entry.last_draw;
  }
  return out.str();
}

void StaticPictureCache::MakeRoom(size_t bytes) {
  while (!entries_.empty() && (entries_.size() >= limits_.max_entries ||
                               resident_bytes_ > limits_.max_bytes - bytes)) {
    auto oldest = std::min_element(
        entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
          return a.second.last_access < b.second.last_access;
        });
    EraseEntry(oldest);
    statistics_.evictions++;
  }
}

void StaticPictureCache::TrimObservations() {
  while (observations_.size() > limits_.max_observations) {
    auto oldest =
        std::min_element(observations_.begin(), observations_.end(),
                         [](const auto& a, const auto& b) {
                           return a.second.last_access < b.second.last_access;
                         });
    observations_.erase(oldest);
  }
}

void StaticPictureCache::TrimEligibilityRecords() {
  while (eligibility_.size() > limits_.max_eligibility_records) {
    auto oldest =
        std::min_element(eligibility_.begin(), eligibility_.end(),
                         [](const auto& a, const auto& b) {
                           return a.second.last_access < b.second.last_access;
                         });
    eligibility_.erase(oldest);
  }
}

void StaticPictureCache::TrimRequests() {
  while (requests_.size() > limits_.max_eligibility_records) {
    auto oldest = std::min_element(
        requests_.begin(), requests_.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    requests_.erase(oldest);
  }
}

}  // namespace impeller
