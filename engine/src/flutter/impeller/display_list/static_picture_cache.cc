// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "impeller/display_list/static_picture_cache.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/utils/dl_receiver_utils.h"
#include "flutter/fml/hash_combine.h"
#include "impeller/display_list/dl_dispatcher.h"
#include "impeller/display_list/dl_image_impeller.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/renderer/render_target.h"

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
         alignment_y == other.alignment_y;
}

size_t StaticPictureCache::Key::Hash::operator()(const Key& key) const {
  return fml::HashCombine(key.display_list_id, key.scale_x, key.scale_y,
                          key.alignment_x, key.alignment_y);
}

std::optional<StaticPictureCache::Plan> StaticPictureCache::MakePlan(
    const sk_sp<flutter::DisplayList>& display_list,
    const flutter::DlMatrix& transform) {
  if (!display_list || !transform.IsTranslationScaleOnly() ||
      !std::isfinite(transform.m[0]) || !std::isfinite(transform.m[5]) ||
      !std::isfinite(transform.m[12]) || !std::isfinite(transform.m[13]) ||
      transform.m[0] <= 0.0f || transform.m[5] <= 0.0f) {
    return std::nullopt;
  }

  const flutter::DlRect& local_bounds = display_list->GetBounds();
  if (local_bounds.IsEmpty() || !local_bounds.IsFinite()) {
    return std::nullopt;
  }

  const flutter::DlRect device_bounds = local_bounds.TransformBounds(transform);
  if (device_bounds.IsEmpty() || !device_bounds.IsFinite()) {
    return std::nullopt;
  }
  const flutter::DlIRect rounded = flutter::DlIRect::RoundOut(device_bounds);
  if (rounded.IsEmpty()) {
    return std::nullopt;
  }

  const int64_t width = rounded.GetWidth();
  const int64_t height = rounded.GetHeight();
  if (width <= 0 || height <= 0 ||
      static_cast<uint64_t>(width) > std::numeric_limits<size_t>::max() / 4u /
                                         static_cast<uint64_t>(height)) {
    return std::nullopt;
  }

  const flutter::DlMatrix snapshot_transform =
      flutter::DlMatrix::MakeTranslation(
          {-static_cast<float>(rounded.GetLeft()),
           -static_cast<float>(rounded.GetTop())}) *
      transform;
  const flutter::DlRect destination =
      flutter::DlRect::Make(rounded).TransformBounds(transform.Invert());

  return Plan{
      .key = {.display_list_id = display_list->unique_id(),
              .scale_x = transform.m[0],
              .scale_y = transform.m[5],
              .alignment_x = FractionalPart(device_bounds.GetLeft()),
              .alignment_y = FractionalPart(device_bounds.GetTop())},
      .device_bounds = rounded,
      .destination = destination,
      .snapshot_transform = snapshot_transform,
      .estimated_bytes =
          static_cast<size_t>(width) * static_cast<size_t>(height) * 4u,
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

bool StaticPictureCache::Draw(
    const sk_sp<flutter::DisplayList>& display_list,
    const flutter::DlMatrix& canvas_transform,
    const std::optional<flutter::DlMatrix>& canvas_to_render_target,
    flutter::DlScalar opacity,
    flutter::DlCanvas& canvas) {
  if (!canvas_to_render_target.has_value()) {
    statistics_.rejected_transforms++;
    return false;
  }
  const flutter::DlMatrix render_target_transform =
      canvas_to_render_target.value() * canvas_transform;
  const std::optional<Plan> plan =
      MakePlan(display_list, render_target_transform);
  if (!plan.has_value()) {
    statistics_.rejected_transforms++;
    return false;
  }

  auto found = entries_.find(plan->key);
  if (found != entries_.end()) {
    found->second.last_access = ++access_;
    statistics_.hits++;
    DrawEntry(found->second, *plan, opacity, canvas);
    return true;
  }

  if (!IsEligible(display_list)) {
    return false;
  }
  if (limits_.max_entries == 0u || limits_.max_bytes == 0u ||
      plan->estimated_bytes > limits_.max_entry_bytes ||
      plan->estimated_bytes > limits_.max_bytes) {
    statistics_.rejected_by_budget++;
    return false;
  }
  if (!Observe(*plan)) {
    return false;
  }

  flutter::DisplayListBuilder snapshot_builder(flutter::DlRect::MakeWH(
      plan->device_bounds.GetWidth(), plan->device_bounds.GetHeight()));
  snapshot_builder.Transform(plan->snapshot_transform);
  snapshot_builder.DrawDisplayList(display_list);
  sk_sp<flutter::DisplayList> snapshot_display_list = snapshot_builder.Build();
  const ISize snapshot_size(plan->device_bounds.GetSize());
  std::shared_ptr<Texture> texture =
      snapshotter_ ? snapshotter_(snapshot_display_list, snapshot_size)
                   : CreateSnapshot(snapshot_display_list, snapshot_size);
  if (!texture) {
    statistics_.allocation_failures++;
    observations_.insert_or_assign(
        plan->key, Observation{0u, kAllocationFailureRetryCooldown, ++access_});
    TrimObservations();
    return false;
  }

  const size_t bytes =
      texture->GetTextureDescriptor().GetByteSizeOfBaseMipLevel();
  if (bytes > limits_.max_entry_bytes || bytes > limits_.max_bytes) {
    statistics_.rejected_by_budget++;
    return false;
  }
  MakeRoom(bytes);

  sk_sp<flutter::DlImage> image = DlImageImpeller::Make(
      std::move(texture), flutter::DlImage::OwningContext::kRaster);
  if (!image) {
    statistics_.allocation_failures++;
    observations_.insert_or_assign(
        plan->key, Observation{0u, kAllocationFailureRetryCooldown, ++access_});
    TrimObservations();
    return false;
  }

  Entry entry{std::move(image), bytes, ++access_};
  auto [inserted, did_insert] = entries_.emplace(plan->key, std::move(entry));
  if (!did_insert) {
    return false;
  }
  resident_bytes_ += bytes;
  statistics_.generations++;
  DrawEntry(inserted->second, *plan, opacity, canvas);
  return true;
}

std::shared_ptr<Texture> StaticPictureCache::CreateSnapshot(
    const sk_sp<flutter::DisplayList>& snapshot_display_list,
    ISize size) {
  // Static pictures exclude text, images, saveLayers, and backdrops. Render
  // them directly into a persistent, single-sampled, transparent target. This
  // intentionally avoids DisplayListToTexture's text-shadow frame markers,
  // glyph reset, transient-buffer reset, and thread-local cleanup: a static
  // picture snapshot has no work that requires those global frame hooks, and
  // invoking them here could evict unrelated caches while the root frame is
  // still being recorded. A single-sampled target matches Impeller's external
  // root targets and avoids changing clip-edge coverage through an MSAA
  // resolve.
  RenderTargetAllocator allocator(
      renderer_.GetContext()->GetResourceAllocator());
  RenderTarget target = allocator.CreateOffscreen(*renderer_.GetContext(), size,
                                                  1u, "Static Picture Cache");
  if (!target.IsValid()) {
    return nullptr;
  }

  CanvasDlDispatcher dispatcher(renderer_, target, false,
                                /*has_root_backdrop_filter=*/false,
                                flutter::DlBlendMode::kSrcOver,
                                IRect32::MakeSize(size));
  snapshot_display_list->Dispatch(
      dispatcher, flutter::DlIRect::MakeWH(size.width, size.height));
  dispatcher.FinishRecording();
  return target.GetRenderTargetTexture();
}

void StaticPictureCache::DrawEntry(const Entry& entry,
                                   const Plan& plan,
                                   flutter::DlScalar opacity,
                                   flutter::DlCanvas& canvas) const {
  flutter::DlPaint paint;
  paint.setOpacity(opacity);
  canvas.DrawImageRect(entry.image, entry.image->GetBounds(), plan.destination,
                       flutter::DlImageSampling::kNearestNeighbor, &paint,
                       flutter::DlSrcRectConstraint::kStrict);
}

void StaticPictureCache::MakeRoom(size_t bytes) {
  while (!entries_.empty() && (entries_.size() >= limits_.max_entries ||
                               resident_bytes_ > limits_.max_bytes - bytes)) {
    auto oldest = std::min_element(
        entries_.begin(), entries_.end(), [](const auto& a, const auto& b) {
          return a.second.last_access < b.second.last_access;
        });
    resident_bytes_ -= oldest->second.bytes;
    entries_.erase(oldest);
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

}  // namespace impeller
