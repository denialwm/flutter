// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/embedder/embedder_external_texture_vk.h"

#include <limits>
#include <memory>

#include "flutter/fml/closure.h"
#include "flutter/fml/logging.h"
#include "flutter/impeller/display_list/dl_image_impeller.h"
#include "impeller/display_list/aiks_context.h"
#include "impeller/renderer/backend/vulkan/context_vk.h"
#include "impeller/renderer/backend/vulkan/formats_vk.h"
#include "impeller/renderer/backend/vulkan/texture_source_vk.h"
#include "impeller/renderer/backend/vulkan/texture_vk.h"

namespace flutter {
namespace {

class BorrowedTextureSourceVK final : public impeller::TextureSourceVK {
 public:
  BorrowedTextureSourceVK(impeller::TextureDescriptor descriptor,
                          impeller::vk::Image image,
                          impeller::vk::UniqueImageView image_view,
                          VoidCallback destruction_callback,
                          void* user_data)
      : TextureSourceVK(descriptor),
        image_(image),
        image_view_(std::move(image_view)),
        destruction_callback_(destruction_callback),
        user_data_(user_data) {}

  ~BorrowedTextureSourceVK() override {
    // The image view must die before the embedder is allowed to destroy the
    // borrowed image it references.
    image_view_.reset();
    if (destruction_callback_) {
      destruction_callback_(user_data_);
    }
  }

 private:
  impeller::vk::Image GetImage() const override { return image_; }

  impeller::vk::ImageView GetImageView() const override {
    return image_view_.get();
  }

  impeller::vk::ImageView GetRenderTargetView() const override {
    return image_view_.get();
  }

  bool IsSwapchainImage() const override { return false; }

  impeller::vk::Image image_;
  impeller::vk::UniqueImageView image_view_;
  VoidCallback destruction_callback_;
  void* user_data_;
};

}  // namespace

EmbedderExternalTextureVK::EmbedderExternalTextureVK(
    int64_t texture_identifier,
    const ExternalTextureCallback& callback)
    : Texture(texture_identifier), external_texture_callback_(callback) {
  FML_DCHECK(external_texture_callback_);
}

EmbedderExternalTextureVK::~EmbedderExternalTextureVK() = default;

void EmbedderExternalTextureVK::Paint(PaintContext& context,
                                      const DlRect& bounds,
                                      bool freeze,
                                      const DlImageSampling sampling) {
  if (!last_image_) {
    last_borrowed_source_.reset();
    last_image_ =
        ResolveTexture(Id(), context.aiks_context,
                       DlISize(bounds.GetWidth(), bounds.GetHeight()));
  }
  if (!last_image_) {
    return;
  }
  if (last_borrowed_source_) {
    if (!context.aiks_context ||
        !impeller::ContextVK::Cast(*context.aiks_context->GetContext())
             .AddFrameTexture(
                 last_borrowed_source_,
                 static_cast<impeller::vk::ImageLayout>(last_external_layout_),
                 last_external_queue_family_)) {
      FML_LOG(ERROR) << "Could not acquire a Vulkan external texture.";
      return;
    }
  }

  const DlRect image_bounds = DlRect::Make(last_image_->GetBounds());
  if (bounds != image_bounds) {
    context.canvas->DrawImageRect(last_image_, image_bounds, bounds, sampling,
                                  context.paint);
  } else {
    context.canvas->DrawImage(last_image_, bounds.GetOrigin(), sampling,
                              context.paint);
  }
}

sk_sp<DlImage> EmbedderExternalTextureVK::ResolveTexture(
    int64_t texture_id,
    impeller::AiksContext* aiks_context,
    const DlISize& size) {
  if (!aiks_context) {
    return nullptr;
  }
  auto external =
      external_texture_callback_(texture_id, size.width, size.height);
  if (!external) {
    return nullptr;
  }
  if (external->struct_size < sizeof(FlutterVulkanExternalTexture)) {
    FML_LOG(ERROR) << "Vulkan external texture is missing ownership metadata.";
    return nullptr;
  }
  if (!external->destruction_callback) {
    FML_LOG(ERROR) << "Vulkan external texture has no destruction callback.";
    return nullptr;
  }

  fml::ScopedCleanupClosure cleanup(
      [&external]() { external->destruction_callback(external->user_data); });
  if (external->width == 0 || external->height == 0 ||
      external->width >
          static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
      external->height >
          static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
    FML_LOG(ERROR) << "Invalid Vulkan external texture dimensions.";
    return nullptr;
  }

  const auto vk_format = static_cast<impeller::vk::Format>(external->format);
  const auto pixel_format = impeller::VkFormatToImpellerFormat(vk_format);
  if (!pixel_format) {
    FML_LOG(ERROR) << "Unsupported Vulkan external texture format: "
                   << impeller::vk::to_string(vk_format);
    return nullptr;
  }

  impeller::TextureDescriptor descriptor;
  descriptor.format = pixel_format.value();
  descriptor.size = impeller::ISize(external->width, external->height);
  descriptor.storage_mode = impeller::StorageMode::kDevicePrivate;
  descriptor.mip_count = 1;
  descriptor.usage = impeller::TextureUsage::kShaderRead;

  auto context = aiks_context->GetContext();
  auto& context_vk = impeller::ContextVK::Cast(*context);
  std::shared_ptr<impeller::Texture> texture;
  if (external->image != 0 && external->pixels == nullptr) {
    impeller::vk::ImageViewCreateInfo view_info = {};
    view_info.viewType = impeller::vk::ImageViewType::e2D;
    view_info.format = vk_format;
    view_info.subresourceRange.aspectMask =
        impeller::vk::ImageAspectFlagBits::eColor;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    view_info.image =
        impeller::vk::Image(reinterpret_cast<VkImage>(external->image));
    if (external->opaque) {
      view_info.components.a = impeller::vk::ComponentSwizzle::eOne;
    }
    auto [result, image_view] =
        context_vk.GetDevice().createImageViewUnique(view_info);
    if (result != impeller::vk::Result::eSuccess) {
      FML_LOG(ERROR) << "Could not create Vulkan external texture view: "
                     << impeller::vk::to_string(result);
      return nullptr;
    }
    auto source = std::make_shared<BorrowedTextureSourceVK>(
        descriptor, view_info.image, std::move(image_view),
        external->destruction_callback, external->user_data);
    source->SetLayoutWithoutEncoding(
        static_cast<impeller::vk::ImageLayout>(external->layout));
    last_borrowed_source_ = source;
    last_external_layout_ = external->layout;
    last_external_queue_family_ = external->external_queue_family_index;
    texture = std::make_shared<impeller::TextureVK>(context, std::move(source));
    cleanup.Release();
  } else if (external->image == 0 && external->pixels != nullptr) {
    const size_t byte_size = descriptor.GetByteSizeOfBaseMipLevel();
    if (external->row_bytes != external->width * 4u) {
      FML_LOG(ERROR) << "Vulkan external pixel rows must be tightly packed.";
      return nullptr;
    }
    texture = context->GetResourceAllocator()->CreateTexture(descriptor);
    if (!texture || !texture->SetContents(external->pixels, byte_size, 0,
                                          external->opaque)) {
      FML_LOG(ERROR) << "Could not upload Vulkan external texture pixels.";
      return nullptr;
    }
  } else {
    FML_LOG(ERROR) << "Vulkan external texture must supply one payload.";
    return nullptr;
  }

  return impeller::DlImageImpeller::Make(std::move(texture),
                                         DlImage::OwningContext::kRaster);
}

void EmbedderExternalTextureVK::OnGrContextCreated() {}

void EmbedderExternalTextureVK::OnGrContextDestroyed() {}

void EmbedderExternalTextureVK::MarkNewFrameAvailable() {
  last_image_ = nullptr;
  last_borrowed_source_.reset();
}

void EmbedderExternalTextureVK::OnTextureUnregistered() {
  last_image_ = nullptr;
  last_borrowed_source_.reset();
}

}  // namespace flutter
