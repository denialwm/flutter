// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_IMPELLER_DISPLAY_LIST_DL_IMAGE_IMPELLER_H_
#define FLUTTER_IMPELLER_DISPLAY_LIST_DL_IMAGE_IMPELLER_H_

#include "flutter/display_list/image/dl_image.h"
#include "impeller/core/texture.h"

namespace impeller {

class AiksContext;

class DlImageImpeller final : public flutter::DlImage {
 public:
  static sk_sp<DlImageImpeller> Make(
      std::shared_ptr<Texture> texture,
      OwningContext owning_context = OwningContext::kIO,
      bool is_external_texture = false);

  static sk_sp<DlImageImpeller> MakeFromYUVTextures(
      AiksContext* aiks_context,
      std::shared_ptr<Texture> y_texture,
      std::shared_ptr<Texture> uv_texture,
      YUVColorSpace yuv_color_space);

  // |DlImage|
  ~DlImageImpeller() override;

  // |DlImage|
  sk_sp<SkImage> skia_image() const override;

  // |DlImage|
  std::shared_ptr<impeller::Texture> impeller_texture() const override;

  // |DlImage|
  bool isOpaque() const override;

  // |DlImage|
  bool isTextureBacked() const override;

  // |DlImage|
  bool isExternalTexture() const override { return is_external_texture_; }

  // |DlImage|
  bool isUIThreadSafe() const override;

  // |DlImage|
  flutter::DlISize GetSize() const override;

  // |DlImage|
  size_t GetApproximateByteSize() const override;

  // |DlImage|
  OwningContext owning_context() const override { return owning_context_; }

 private:
  std::shared_ptr<Texture> texture_;
  OwningContext owning_context_;
  bool is_external_texture_ = false;

  explicit DlImageImpeller(std::shared_ptr<Texture> texture,
                           OwningContext owning_context = OwningContext::kIO,
                           bool is_external_texture = false);

  DlImageImpeller(const DlImageImpeller&) = delete;

  DlImageImpeller& operator=(const DlImageImpeller&) = delete;
};

}  // namespace impeller

#endif  // FLUTTER_IMPELLER_DISPLAY_LIST_DL_IMAGE_IMPELLER_H_
