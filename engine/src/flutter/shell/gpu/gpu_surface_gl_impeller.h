// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_GPU_GPU_SURFACE_GL_IMPELLER_H_
#define FLUTTER_SHELL_GPU_GPU_SURFACE_GL_IMPELLER_H_

#include "flutter/common/graphics/gl_context_switch.h"
#include "flutter/flow/surface.h"
#include "flutter/fml/macros.h"
#include "flutter/fml/memory/weak_ptr.h"
#include "flutter/impeller/display_list/aiks_context.h"
#include "flutter/impeller/renderer/context.h"
#include "flutter/shell/gpu/gpu_surface_gl_delegate.h"

namespace flutter {
namespace testing {
FML_TEST_CLASS(EmbedderSurfaceGLImpellerTest,
               ImpellerPresentPreservesFBOAndReportsFullDamage);
FML_TEST_CLASS(EmbedderSurfaceGLImpellerTest,
               ImpellerUsesEmbedderRootSurfaceTransformation);
}  // namespace testing

class GPUSurfaceGLImpeller final : public Surface {
 public:
  explicit GPUSurfaceGLImpeller(GPUSurfaceGLDelegate* delegate,
                                std::shared_ptr<impeller::Context> context,
                                bool render_to_surface,
                                bool fbo_zero_is_no_target = false);

  // |Surface|
  ~GPUSurfaceGLImpeller() override;

  // |Surface|
  bool IsValid() override;

 private:
  FML_FRIEND_TEST(testing::EmbedderSurfaceGLImpellerTest,
                  ImpellerPresentPreservesFBOAndReportsFullDamage);
  FML_FRIEND_TEST(testing::EmbedderSurfaceGLImpellerTest,
                  ImpellerUsesEmbedderRootSurfaceTransformation);
  GPUSurfaceGLDelegate* delegate_ = nullptr;
  std::shared_ptr<impeller::Context> impeller_context_;
  bool render_to_surface_ = true;
  bool fbo_zero_is_no_target_ = false;
  std::shared_ptr<impeller::AiksContext> aiks_context_;
  bool is_valid_ = false;
  fml::TaskRunnerAffineWeakPtrFactory<GPUSurfaceGLImpeller> weak_factory_;

  // |Surface|
  std::unique_ptr<SurfaceFrame> AcquireFrame(const DlISize& size) override;

  static bool PresentFrame(GPUSurfaceGLDelegate* delegate,
                           uint32_t fbo_id,
                           const DlISize& size);

  static DlMatrix RootTransformation(GPUSurfaceGLDelegate* delegate);

  static void ConfigureRootCanvas(DlCanvas* canvas,
                                  GPUSurfaceGLDelegate* delegate);

  // |Surface|
  DlMatrix GetRootTransformation() const override;

  // |Surface|
  GrDirectContext* GetContext() override;

  // |Surface|
  std::unique_ptr<GLContextResult> MakeRenderContextCurrent() override;

  // |Surface|
  bool ClearRenderContext() override;

  // |Surface|
  bool AllowsDrawingWhenGpuDisabled() const override;

  // |Surface|
  bool EnableRasterCache() const override;

  // |Surface|
  std::shared_ptr<impeller::AiksContext> GetAiksContext() const override;

  FML_DISALLOW_COPY_AND_ASSIGN(GPUSurfaceGLImpeller);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_GPU_GPU_SURFACE_GL_IMPELLER_H_
