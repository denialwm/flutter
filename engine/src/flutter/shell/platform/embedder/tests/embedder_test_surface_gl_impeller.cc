// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flatbuffers/stl_emulation.h"
#include "flutter/impeller/renderer/backend/gles/test/mock_gles.h"
#include "flutter/shell/platform/embedder/embedder_surface_gl_impeller.h"
#include "impeller/renderer/backend/gles/shader_function_gles.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace flutter {
namespace testing {

using ::testing::Not;
using ::testing::StartsWith;

namespace {
EmbedderSurfaceGLSkia::GLDispatchTable StubDispatchTable(
    std::string_view version) {
  impeller::testing::MockGLES::Init(flatbuffers::nullopt, version.data());
  static constexpr auto dummy_always_true = [] { return true; };
  return EmbedderSurfaceGLSkia::GLDispatchTable{
      .gl_make_current_callback = dummy_always_true,
      .gl_clear_current_callback = dummy_always_true,
      .gl_present_callback = [](const auto) { return true; },
      .gl_fbo_callback = [](const auto) { return 0; },
      .gl_make_resource_current_callback = dummy_always_true,
      .gl_surface_transformation_callback = [] { return DlMatrix{}; },
      .gl_proc_resolver = impeller::testing::kMockResolverGLES,
      .gl_populate_existing_damage = [](const auto) { return GLFBOInfo{}; },
  };
}
}  // namespace

TEST(EmbedderSurfaceGLImpellerTest, GLES3ContextHasGLES3Shaders) {
  const auto gl_dispatch_table =
      StubDispatchTable(/* version */ "OpenGL ES 3.0");
  const auto surface = EmbedderSurfaceGLImpeller(
      gl_dispatch_table, /* fbo_reset_after_present */ false,
      /* fbo_zero_is_no_target */ false,
      /* external_view_embedder */ nullptr);

  const std::shared_ptr<impeller::Context> context =
      surface.CreateImpellerContext();
  const std::shared_ptr<impeller::ShaderLibrary> shaders =
      context->GetShaderLibrary();
  const std::shared_ptr<const impeller::ShaderFunction> func =
      shaders->GetFunction("imp_line_fragment_main",
                           impeller::ShaderStage::kFragment);
  const auto gles_func = impeller::ShaderFunctionGLES::Cast(func.get());
  const std::shared_ptr<const fml::Mapping> source =
      gles_func->GetSourceMapping();
  const auto text =
      std::string_view(reinterpret_cast<const char*>(source->GetMapping()));
  EXPECT_THAT(text, StartsWith("#version 300 es"));
}

TEST(EmbedderSurfaceGLImpellerTest, GLES2ContextDoesNotHaveGLES3Shaders) {
  const auto gl_dispatch_table =
      StubDispatchTable(/* version */ "OpenGL ES 2.0");
  const auto surface = EmbedderSurfaceGLImpeller(
      gl_dispatch_table, /* fbo_reset_after_present */ false,
      /* fbo_zero_is_no_target */ false,
      /* external_view_embedder */ nullptr);

  const std::shared_ptr<impeller::Context> context =
      surface.CreateImpellerContext();
  const std::shared_ptr<impeller::ShaderLibrary> shaders =
      context->GetShaderLibrary();
  const std::shared_ptr<const impeller::ShaderFunction> func =
      shaders->GetFunction("imp_line_fragment_main",
                           impeller::ShaderStage::kFragment);
  const auto gles_func = impeller::ShaderFunctionGLES::Cast(func.get());
  const std::shared_ptr<const fml::Mapping> source =
      gles_func->GetSourceMapping();
  const auto text =
      std::string_view(reinterpret_cast<const char*>(source->GetMapping()));
  EXPECT_THAT(text, StartsWith("#version 100"));
}

TEST(EmbedderSurfaceGLImpellerTest,
     ImpellerPresentPreservesFBOAndReportsDamage) {
  auto gl_dispatch_table = StubDispatchTable(/* version */ "OpenGL ES 3.0");
  uint32_t presented_fbo = 0u;
  std::optional<DlRegion> frame_damage;
  std::optional<DlRegion> buffer_damage;
  gl_dispatch_table.gl_present_callback = [&](const GLPresentInfo& info) {
    presented_fbo = info.fbo_id;
    frame_damage = info.frame_damage;
    buffer_damage = info.buffer_damage;
    return true;
  };
  auto surface = EmbedderSurfaceGLImpeller(
      gl_dispatch_table, /* fbo_reset_after_present */ false,
      /* fbo_zero_is_no_target */ false,
      /* external_view_embedder */ nullptr);

  const DlISize frame_size = DlISize(64, 32);
  const DlRegion expected_frame_damage(DlIRect::MakeLTRB(4, 5, 20, 16));
  const DlRegion expected_buffer_damage(DlIRect::MakeLTRB(2, 3, 24, 18));
  SurfaceFrame::SubmitInfo submit_info = {
      .frame_damage = expected_frame_damage,
      .buffer_damage = expected_buffer_damage,
  };
  EXPECT_TRUE(GPUSurfaceGLImpeller::PresentFrame(&surface, /* fbo_id */ 73u,
                                                 frame_size, submit_info));
  EXPECT_EQ(presented_fbo, 73u);
  ASSERT_TRUE(frame_damage.has_value());
  ASSERT_TRUE(buffer_damage.has_value());
  EXPECT_EQ(frame_damage->getRects(), expected_frame_damage.getRects());
  EXPECT_EQ(buffer_damage->getRects(), expected_buffer_damage.getRects());
}

TEST(EmbedderSurfaceGLImpellerTest,
     ImpellerUsesEmbedderRootSurfaceTransformation) {
  auto gl_dispatch_table = StubDispatchTable(/* version */ "OpenGL ES 3.0");
  const DlMatrix expected = DlMatrix::MakeTranslation({0.0, 32.0}) *
                            DlMatrix::MakeScale({1.0, -1.0, 1.0});
  gl_dispatch_table.gl_surface_transformation_callback = [expected] {
    return expected;
  };
  auto surface = EmbedderSurfaceGLImpeller(
      gl_dispatch_table, /* fbo_reset_after_present */ false,
      /* fbo_zero_is_no_target */ false,
      /* external_view_embedder */ nullptr);

  DisplayListBuilder canvas(DlRect::MakeWH(64.0, 32.0));
  GPUSurfaceGLImpeller::ConfigureRootCanvas(&canvas, &surface);

  EXPECT_EQ(canvas.GetMatrix(), expected);
}
}  // namespace testing
}  // namespace flutter
