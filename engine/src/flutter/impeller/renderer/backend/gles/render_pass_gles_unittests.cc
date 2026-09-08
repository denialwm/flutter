// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include "flutter/testing/testing.h"  // IWYU pragma: keep
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "impeller/core/formats.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/gles/entity_shaders_gles.h"
#include "impeller/entity/gles/framebuffer_blend_shaders_gles.h"
#include "impeller/entity/gles/modern_shaders_gles.h"
#include "impeller/renderer/backend/gles/command_buffer_gles.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/proc_table_gles.h"
#include "impeller/renderer/backend/gles/reactor_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/render_target.h"

namespace impeller {
namespace testing {

using ::testing::_;
using ::testing::Args;
using ::testing::ElementsAreArray;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::TestWithParam;

class TestReactorGLES : public ReactorGLES {
 public:
  TestReactorGLES()
      : ReactorGLES(std::make_unique<ProcTableGLES>(kMockResolverGLES)) {}

  ~TestReactorGLES() = default;
};

class MockWorker final : public ReactorGLES::Worker {
 public:
  MockWorker() = default;

  // |ReactorGLES::Worker|
  bool CanReactorReactOnCurrentThreadNow(
      const ReactorGLES& reactor) const override {
    return true;
  }
};

struct DiscardFrameBufferParams {
  GLuint frame_buffer_id;
  std::array<GLenum, 3> expected_attachments;
};

class RenderPassGLESWithDiscardFrameBufferExtTest
    : public TestWithParam<DiscardFrameBufferParams> {};

namespace {
std::shared_ptr<ContextGLES> CreateFakeGLESContext() {
  auto dummy_gl_procs = std::make_unique<ProcTableGLES>(kMockResolverGLES);
  auto dummy_shader_library = std::vector<std::shared_ptr<fml::Mapping>>{};
  auto flags = Flags{};
  return ContextGLES::Create(flags, std::move(dummy_gl_procs),
                             dummy_shader_library, false);
}
}  // namespace

TEST(RenderPassGLESTest, DirectGlassPipelineRetainsDepthClipping) {
  // Exercise the production pipeline variant cache with mocked GL calls.
  // No window, real graphics context, draw, or pixel readback is needed.
  const ProcTableGLES::Resolver resolver = [](const char* name) -> void* {
    if (strcmp(name, "glCreateShader") == 0) {
      return reinterpret_cast<void*>(+[](GLenum) -> GLuint { return 1; });
    }
    if (strcmp(name, "glCreateProgram") == 0) {
      return reinterpret_cast<void*>(+[]() -> GLuint { return 1; });
    }
    if (strcmp(name, "glIsProgram") == 0) {
      return reinterpret_cast<void*>(
          +[](GLuint) -> GLboolean { return GL_TRUE; });
    }
    if (strcmp(name, "glGetShaderiv") == 0) {
      return reinterpret_cast<void*>(+[](GLuint, GLenum query, GLint* value) {
        *value = query == GL_COMPILE_STATUS ? GL_TRUE : 0;
      });
    }
    if (strcmp(name, "glGetProgramiv") == 0) {
      return reinterpret_cast<void*>(+[](GLuint, GLenum query, GLint* value) {
        *value = query == GL_LINK_STATUS ? GL_TRUE : 0;
      });
    }
    return kMockResolverGLES(name);
  };
  auto mock_gl = MockGLES::Init(std::nullopt, "OpenGL ES 2.0", resolver);
  auto context =
      ContextGLES::Create(Flags{}, std::make_unique<ProcTableGLES>(resolver),
                          {std::make_shared<fml::NonOwnedMapping>(
                               impeller_entity_shaders_gles_data,
                               impeller_entity_shaders_gles_length),
                           std::make_shared<fml::NonOwnedMapping>(
                               impeller_modern_shaders_gles_data,
                               impeller_modern_shaders_gles_length),
                           std::make_shared<fml::NonOwnedMapping>(
                               impeller_framebuffer_blend_shaders_gles_data,
                               impeller_framebuffer_blend_shaders_gles_length)},
                          false);
  ASSERT_TRUE(context);
  auto worker = std::make_shared<MockWorker>();
  context->AddReactorWorker(worker);
  ContentContext renderer(context, nullptr);
  ASSERT_TRUE(renderer.IsValid());

  ContentContextOptions options;
  options.sample_count = SampleCount::kCount1;
  options.color_attachment_pixel_format =
      renderer.GetDeviceCapabilities().GetDefaultColorFormat();
  options.primitive_type = PrimitiveType::kTriangleStrip;
  options.blend_mode = BlendMode::kSrc;
  options.depth_compare = CompareFunction::kGreaterEqual;
  options.stencil_mode = ContentContextOptions::StencilMode::kIgnore;
  // Resolve an offscreen variant first, as cached material rendering does.
  options.has_depth_stencil_attachments = false;
  auto offscreen = renderer.GetGlassPipeline(options);
  ASSERT_TRUE(offscreen);
  EXPECT_FALSE(
      offscreen->GetDescriptor().GetDepthStencilAttachmentDescriptor());

  options.has_depth_stencil_attachments = true;
  auto direct = renderer.GetGlassPipeline(options);
  ASSERT_TRUE(direct);
  const auto& descriptor = direct->GetDescriptor();
  ASSERT_TRUE(descriptor.GetDepthStencilAttachmentDescriptor());
  EXPECT_EQ(descriptor.GetDepthStencilAttachmentDescriptor()->depth_compare,
            CompareFunction::kGreaterEqual);
  EXPECT_TRUE(descriptor.GetFrontStencilAttachmentDescriptor());
  EXPECT_NE(descriptor.GetDepthPixelFormat(), PixelFormat::kUnknown);
  EXPECT_NE(descriptor.GetStencilPixelFormat(), PixelFormat::kUnknown);
  EXPECT_EQ(renderer.GetGlassPipeline(options), direct);
}

TEST_P(RenderPassGLESWithDiscardFrameBufferExtTest, DiscardFramebufferExt) {
  auto mock_gl_impl = std::make_unique<NiceMock<MockGLESImpl>>();
  auto& mock_gl_impl_ref = *mock_gl_impl;
  auto mock_gl =
      MockGLES::Init(std::move(mock_gl_impl), {{"GL_EXT_discard_framebuffer"}},
                     "OpenGL ES 2.0");

  auto context = CreateFakeGLESContext();
  auto dummy_worker = std::make_shared<MockWorker>();
  context->AddReactorWorker(dummy_worker);
  auto reactor = context->GetReactor();

  const auto command_buffer =
      std::static_pointer_cast<Context>(context)->CreateCommandBuffer();
  auto render_target = RenderTarget{};
  const auto description = TextureDescriptor{
      .format = PixelFormat::kR8G8B8A8UNormInt, .size = {10, 10}};

  const auto& test_params = GetParam();
  auto framebuffer_texture =
      TextureGLES::WrapFBO(reactor, description, test_params.frame_buffer_id);

  auto color_attachment = ColorAttachment{Attachment{
      .texture = framebuffer_texture, .store_action = StoreAction::kDontCare}};
  render_target.SetColorAttachment(color_attachment, 0);
  const auto render_pass = command_buffer->CreateRenderPass(render_target);

  EXPECT_CALL(mock_gl_impl_ref, GetIntegerv(GL_FRAMEBUFFER_BINDING, _))
      .WillOnce(SetArgPointee<1>(test_params.frame_buffer_id));

  EXPECT_CALL(mock_gl_impl_ref, DiscardFramebufferEXT(GL_FRAMEBUFFER, _, _))
      .With(Args<2, 1>(ElementsAreArray(test_params.expected_attachments)))
      .Times(1);
  ASSERT_TRUE(render_pass->EncodeCommands());
  ASSERT_TRUE(reactor->React());
}

INSTANTIATE_TEST_SUITE_P(
    FrameBufferObject,
    RenderPassGLESWithDiscardFrameBufferExtTest,
    ::testing::ValuesIn(std::vector<DiscardFrameBufferParams>{
        {.frame_buffer_id = 0,
         .expected_attachments = {GL_COLOR_EXT, GL_DEPTH_EXT, GL_STENCIL_EXT}},
        {.frame_buffer_id = 1,
         .expected_attachments = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT,
                                  GL_STENCIL_ATTACHMENT}}}),
    [](const ::testing::TestParamInfo<DiscardFrameBufferParams>& info) {
      return (info.param.frame_buffer_id == 0) ? "Default" : "NonDefault";
    });

TEST_P(RenderPassGLESWithDiscardFrameBufferExtTest, InvalidateFramebuffer) {
  auto mock_gl_impl = std::make_unique<NiceMock<MockGLESImpl>>();
  auto& mock_gl_impl_ref = *mock_gl_impl;
  auto mock_gl =
      MockGLES::Init(std::move(mock_gl_impl), std::nullopt, "OpenGL ES 3.0");

  auto context = CreateFakeGLESContext();
  auto dummy_worker = std::make_shared<MockWorker>();
  context->AddReactorWorker(dummy_worker);
  auto reactor = context->GetReactor();

  const auto command_buffer =
      std::static_pointer_cast<Context>(context)->CreateCommandBuffer();
  auto render_target = RenderTarget{};
  const auto description = TextureDescriptor{
      .format = PixelFormat::kR8G8B8A8UNormInt, .size = {10, 10}};

  const auto& test_params = GetParam();
  auto framebuffer_texture =
      TextureGLES::WrapFBO(reactor, description, test_params.frame_buffer_id);

  auto color_attachment = ColorAttachment{Attachment{
      .texture = framebuffer_texture, .store_action = StoreAction::kDontCare}};
  render_target.SetColorAttachment(color_attachment, 0);
  const auto render_pass = command_buffer->CreateRenderPass(render_target);

  EXPECT_CALL(mock_gl_impl_ref, GetIntegerv(GL_FRAMEBUFFER_BINDING, _))
      .WillOnce(SetArgPointee<1>(test_params.frame_buffer_id));

  // InvalidateFramebuffer should be called instead of DiscardFramebufferEXT
  EXPECT_CALL(mock_gl_impl_ref, InvalidateFramebuffer(GL_FRAMEBUFFER, _, _))
      .With(Args<2, 1>(ElementsAreArray(test_params.expected_attachments)))
      .Times(1);
  EXPECT_CALL(mock_gl_impl_ref, DiscardFramebufferEXT(GL_FRAMEBUFFER, _, _))
      .Times(0);

  ASSERT_TRUE(render_pass->EncodeCommands());
  ASSERT_TRUE(reactor->React());
}

TEST(RenderPassGLESTest, ResolvingMultisampleTextureCachesResolveFBO) {
  auto mock_gl_impl = std::make_unique<NiceMock<MockGLESImpl>>();
  auto& mock_gl_impl_ref = *mock_gl_impl;
  // Make sure implicit resolving isn't supported so we go down explicit path.
  auto mock_gl =
      MockGLES::Init(std::move(mock_gl_impl), std::nullopt, "OpenGL ES 3.0");

  auto context = CreateFakeGLESContext();
  auto dummy_worker = std::make_shared<MockWorker>();
  context->AddReactorWorker(dummy_worker);
  auto reactor = context->GetReactor();

  const auto command_buffer =
      std::static_pointer_cast<Context>(context)->CreateCommandBuffer();

  const auto msaa_desc =
      TextureDescriptor{.type = TextureType::kTexture2DMultisample,
                        .format = PixelFormat::kR8G8B8A8UNormInt,
                        .size = {10, 10},
                        .usage = TextureUsage::kRenderTarget,
                        .sample_count = SampleCount::kCount4};
  const auto resolve_desc =
      TextureDescriptor{.storage_mode = StorageMode::kDevicePrivate,
                        .type = TextureType::kTexture2D,
                        .format = PixelFormat::kR8G8B8A8UNormInt,
                        .size = {10, 10},
                        .usage = TextureUsage::kRenderTarget,
                        .sample_count = SampleCount::kCount1};

  auto msaa_tex = std::make_shared<TextureGLES>(reactor, msaa_desc);
  auto resolve_tex = std::make_shared<TextureGLES>(reactor, resolve_desc);

  auto render_target = RenderTarget{};
  auto color_attachment = ColorAttachment{Attachment{
      .texture = msaa_tex,
      .resolve_texture = resolve_tex,
      .load_action = LoadAction::kClear,
      .store_action = StoreAction::kMultisampleResolve,
  }};
  color_attachment.clear_color = Color::Black();
  render_target.SetColorAttachment(color_attachment, 0);

  EXPECT_CALL(mock_gl_impl_ref, CheckFramebufferStatus(_))
      .WillRepeatedly(Return(GL_FRAMEBUFFER_COMPLETE));

  // Expect GenFramebuffers is called exactly once for the offscreen FBO,
  // and exactly once for the resolve FBO over two passes.
  EXPECT_CALL(mock_gl_impl_ref, GenFramebuffers(_, _)).Times(2);

  {
    const auto render_pass = command_buffer->CreateRenderPass(render_target);
    ASSERT_TRUE(render_pass->EncodeCommands());
    ASSERT_TRUE(reactor->React());
  }
  {
    const auto render_pass2 = command_buffer->CreateRenderPass(render_target);
    ASSERT_TRUE(render_pass2->EncodeCommands());
    ASSERT_TRUE(reactor->React());
  }
}

}  // namespace testing
}  // namespace impeller
