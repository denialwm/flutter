// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <memory>
#include <tuple>
#include "flutter/testing/testing.h"  // IWYU pragma: keep
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "impeller/core/device_buffer.h"
#include "impeller/core/formats.h"
#include "impeller/entity/contents/content_context.h"
#include "impeller/entity/gles/entity_shaders_gles.h"
#include "impeller/entity/gles/framebuffer_blend_shaders_gles.h"
#include "impeller/entity/gles/modern_shaders_gles.h"
#include "impeller/renderer/backend/gles/command_buffer_gles.h"
#include "impeller/renderer/backend/gles/context_gles.h"
#include "impeller/renderer/backend/gles/pipeline_gles.h"
#include "impeller/renderer/backend/gles/proc_table_gles.h"
#include "impeller/renderer/backend/gles/reactor_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"
#include "impeller/renderer/backend/gles/unique_handle_gles.h"
#include "impeller/renderer/context.h"
#include "impeller/renderer/render_pass.h"
#include "impeller/renderer/render_target.h"

namespace impeller {
namespace testing {

using ::testing::_;
using ::testing::Args;
using ::testing::ElementsAreArray;
using ::testing::NiceMock;
using ::testing::Pointee;
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
std::shared_ptr<ContextGLES> CreateFakeGLESContext(
    ProcTableGLES::Resolver resolver = kMockResolverGLES) {
  auto dummy_gl_procs = std::make_unique<ProcTableGLES>(std::move(resolver));
  auto dummy_shader_library = std::vector<std::shared_ptr<fml::Mapping>>{};
  auto flags = Flags{};
  return ContextGLES::Create(flags, std::move(dummy_gl_procs),
                             dummy_shader_library, false);
}

struct RenderPassGLESContext {
  std::shared_ptr<MockGLES> mock_gl;
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref;
  std::shared_ptr<ContextGLES> context;
  std::shared_ptr<MockWorker> dummy_worker;
  std::shared_ptr<ReactorGLES> reactor;
  std::shared_ptr<CommandBuffer> command_buffer;
  std::shared_ptr<RenderPass> render_pass;
  std::shared_ptr<PipelineGLES> pipeline;
};
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
  EXPECT_EQ(PipelineGLES::Cast(*offscreen).GetSharedProgram(),
            PipelineGLES::Cast(*direct).GetSharedProgram());
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

class RenderPassGLESCommandTest : public ::testing::Test {
 protected:
  // Builds a mock OpenGL ES context with a render pass and a minimal
  // pipeline. The [resolver] controls which GL entry points the backend can
  // see, which is how a caller selects the hardware or emulated instancing
  // path.
  static RenderPassGLESContext CreateRenderPassGLESContext(
      ProcTableGLES::Resolver resolver = kMockResolverGLES,
      GLuint program_id = 0) {
    std::unique_ptr<NiceMock<MockGLESImpl>> mock_gl_impl =
        std::make_unique<NiceMock<MockGLESImpl>>();
    testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = *mock_gl_impl;
    std::shared_ptr<MockGLES> mock_gl = MockGLES::Init(std::move(mock_gl_impl));

    std::shared_ptr<ContextGLES> context =
        CreateFakeGLESContext(std::move(resolver));
    std::shared_ptr<MockWorker> dummy_worker = std::make_shared<MockWorker>();
    context->AddReactorWorker(dummy_worker);
    std::shared_ptr<ReactorGLES> reactor = context->GetReactor();

    TextureDescriptor tex_desc;
    tex_desc.size = {100, 100};
    tex_desc.format = PixelFormat::kR8G8B8A8UNormInt;
    auto texture = std::make_shared<TextureGLES>(reactor, tex_desc, false);

    RenderTarget target;
    ColorAttachment color0;
    color0.texture = texture;
    color0.store_action = StoreAction::kDontCare;
    color0.load_action = LoadAction::kClear;
    target.SetColorAttachment(color0, 0);

    std::shared_ptr<CommandBuffer> command_buffer =
        std::static_pointer_cast<Context>(context)->CreateCommandBuffer();
    std::shared_ptr<RenderPass> render_pass =
        command_buffer->CreateRenderPass(target);

    EXPECT_CALL(mock_gl_impl_ref, CheckFramebufferStatus(_))
        .WillRepeatedly(Return(GL_FRAMEBUFFER_COMPLETE));

    PipelineDescriptor desc;
    ColorAttachmentDescriptor color0_desc;
    color0_desc.format = PixelFormat::kR8G8B8A8UNormInt;
    desc.SetColorAttachmentDescriptor(0, color0_desc);

    HandleGLES pipeline_handle =
        reactor->CreateHandle(HandleType::kProgram, program_id);
    std::shared_ptr<PipelineGLES> pipeline = std::shared_ptr<PipelineGLES>(
        new PipelineGLES(reactor, std::weak_ptr<PipelineLibrary>(), desc,
                         std::make_shared<ProgramGLES>(
                             UniqueHandleGLES(reactor, pipeline_handle))));
    pipeline->buffer_bindings_ = std::make_unique<BufferBindingsGLES>();

    return {std::move(mock_gl),     mock_gl_impl_ref,
            std::move(context),     std::move(dummy_worker),
            std::move(reactor),     std::move(command_buffer),
            std::move(render_pass), std::move(pipeline)};
  }

  static std::shared_ptr<PipelineGLES> CreateSharedProgramVariant(
      const RenderPassGLESContext& ctx,
      const PipelineDescriptor& descriptor) {
    auto pipeline = std::shared_ptr<PipelineGLES>(
        new PipelineGLES(ctx.reactor, std::weak_ptr<PipelineLibrary>(),
                         descriptor, ctx.pipeline->GetSharedProgram()));
    pipeline->buffer_bindings_ = std::make_unique<BufferBindingsGLES>();
    return pipeline;
  }

  static void SetYFlipLocation(const std::shared_ptr<PipelineGLES>& pipeline,
                               GLint location) {
    pipeline->y_flip_uniform_location_ = location;
  }

  static std::shared_ptr<PipelineGLES> CreateSecondPipeline(
      const RenderPassGLESContext& ctx,
      GLint y_flip_location,
      GLuint program_id) {
    HandleGLES handle =
        ctx.reactor->CreateHandle(HandleType::kProgram, program_id);
    auto pipeline = std::shared_ptr<PipelineGLES>(new PipelineGLES(
        ctx.reactor, std::weak_ptr<PipelineLibrary>(),
        ctx.pipeline->GetDescriptor(),
        std::make_shared<ProgramGLES>(UniqueHandleGLES(ctx.reactor, handle))));
    pipeline->buffer_bindings_ = std::make_unique<BufferBindingsGLES>();
    pipeline->y_flip_uniform_location_ = y_flip_location;
    return pipeline;
  }
};

TEST_F(RenderPassGLESCommandTest,
       PipelineStateAndDynamicStateRemainIndependent) {
  auto ctx = CreateRenderPassGLESContext(kMockResolverGLES, 101);
  auto desc_a = ctx.pipeline->GetDescriptor();
  ColorAttachmentDescriptor color;
  color.format = PixelFormat::kR8G8B8A8UNormInt;
  color.blending_enabled = true;
  color.src_color_blend_factor = BlendFactor::kOne;
  desc_a.SetColorAttachmentDescriptor(0, color);
  desc_a.SetDepthStencilAttachmentDescriptor(
      DepthAttachmentDescriptor{.depth_compare = CompareFunction::kLess});
  StencilAttachmentDescriptor stencil;
  stencil.stencil_compare = CompareFunction::kEqual;
  desc_a.SetStencilAttachmentDescriptors(stencil);
  desc_a.SetCullMode(CullMode::kBackFace);
  desc_a.SetWindingOrder(WindingOrder::kCounterClockwise);
  desc_a.SetPrimitiveType(PrimitiveType::kTriangleStrip);
  auto a = CreateSharedProgramVariant(ctx, desc_a);

  auto desc_b = desc_a;
  color.src_color_blend_factor = BlendFactor::kSourceAlpha;
  desc_b.SetColorAttachmentDescriptor(0, color);
  desc_b.ClearDepthAttachment();
  desc_b.ClearStencilAttachments();
  desc_b.SetCullMode(CullMode::kFrontFace);
  desc_b.SetWindingOrder(WindingOrder::kClockwise);
  desc_b.SetPrimitiveType(PrimitiveType::kTriangle);
  auto b = CreateSharedProgramVariant(ctx, desc_b);
  auto next_pass =
      ctx.command_buffer->CreateRenderPass(ctx.render_pass->GetRenderTarget());
  const std::shared_ptr<PipelineGLES> pipelines[] = {a, a, b, a, a};
  const uint32_t references[] = {1, 2, 7, 3, 3};
  for (size_t i = 0; i < 5; i++) {
    auto& pass = i == 4 ? next_pass : ctx.render_pass;
    pass->SetPipeline(PipelineRef(pipelines[i]));
    pass->SetStencilReference(references[i]);
    pass->SetElementCount(3);
    pass->SetIndexBuffer({}, IndexType::kNone);
    if (i == 0) {
      pass->SetViewport(Viewport{.rect = Rect::MakeXYWH(1, 2, 30, 40)});
    }
    ASSERT_TRUE(pass->Draw().ok());
  }

  std::map<GLenum, bool> enabled;
  GLint depth_func = 0, cull_face = 0, winding = 0, source_blend = 0;
  GLint reference = 0;
  IRect32 viewport;
  ON_CALL(ctx.mock_gl_impl_ref, Enable(_)).WillByDefault([&](GLenum cap) {
    enabled[cap] = true;
  });
  ON_CALL(ctx.mock_gl_impl_ref, Disable(_)).WillByDefault([&](GLenum cap) {
    enabled[cap] = false;
  });
  ON_CALL(ctx.mock_gl_impl_ref, DepthFunc(_)).WillByDefault([&](GLenum func) {
    depth_func = func;
  });
  ON_CALL(ctx.mock_gl_impl_ref, CullFace(_)).WillByDefault([&](GLenum face) {
    cull_face = face;
  });
  ON_CALL(ctx.mock_gl_impl_ref, FrontFace(_)).WillByDefault([&](GLenum value) {
    winding = value;
  });
  ON_CALL(ctx.mock_gl_impl_ref, BlendFuncSeparate(_, _, _, _))
      .WillByDefault(
          [&](GLenum src, GLenum, GLenum, GLenum) { source_blend = src; });
  EXPECT_CALL(ctx.mock_gl_impl_ref,
              StencilFuncSeparate(GL_FRONT_AND_BACK, GL_EQUAL, _, _))
      .Times(4)
      .WillRepeatedly(
          [&](GLenum, GLenum, GLint value, GLuint) { reference = value; });
  ON_CALL(ctx.mock_gl_impl_ref, Viewport(_, _, _, _))
      .WillByDefault([&](GLint x, GLint y, GLsizei width, GLsizei height) {
        viewport = IRect32::MakeXYWH(x, y, width, height);
      });
  size_t draw = 0;
  EXPECT_CALL(ctx.mock_gl_impl_ref, DrawArrays(_, _, _))
      .Times(5)
      .WillRepeatedly([&](GLenum mode, GLint, GLsizei) {
        SCOPED_TRACE(draw);
        const bool is_a = draw != 2;
        EXPECT_TRUE(enabled[GL_BLEND]);
        EXPECT_TRUE(enabled[GL_CULL_FACE]);
        EXPECT_EQ(enabled[GL_DEPTH_TEST], is_a);
        EXPECT_EQ(enabled[GL_STENCIL_TEST], is_a);
        EXPECT_EQ(source_blend, is_a ? GL_ONE : GL_SRC_ALPHA);
        EXPECT_EQ(cull_face, is_a ? GL_BACK : GL_FRONT);
        EXPECT_EQ(winding, is_a ? GL_CW : GL_CCW);
        EXPECT_EQ(mode,
                  static_cast<GLenum>(is_a ? GL_TRIANGLE_STRIP : GL_TRIANGLES));
        EXPECT_EQ(viewport, draw == 0 ? IRect32::MakeXYWH(1, 2, 30, 40)
                                      : IRect32::MakeXYWH(0, 0, 100, 100));
        if (is_a) {
          EXPECT_EQ(depth_func, GL_LESS);
          EXPECT_EQ(reference, static_cast<GLint>(references[draw]));
        }
        draw++;
      });
  ASSERT_TRUE(ctx.render_pass->EncodeCommands());
  ASSERT_TRUE(next_pass->EncodeCommands());
  ASSERT_TRUE(ctx.reactor->React());
  EXPECT_EQ(draw, 5u);
}

TEST_F(RenderPassGLESCommandTest, QueuedPassOwnsItsSnapshotUntilExecution) {
  auto ctx = CreateRenderPassGLESContext();
  auto target = ctx.render_pass->GetRenderTarget();
  auto color = target.GetColorAttachment(0);
  color.clear_color = Color::Red();
  target.SetColorAttachment(color, 0);
  auto red = ctx.command_buffer->CreateRenderPass(target);
  color.clear_color = Color::Blue();
  target.SetColorAttachment(color, 0);
  auto blue = ctx.command_buffer->CreateRenderPass(target);
  std::weak_ptr<RenderPass> red_lifetime = red;
  std::weak_ptr<RenderPass> blue_lifetime = blue;
  ASSERT_TRUE(red->EncodeCommands());
  ASSERT_TRUE(blue->EncodeCommands());
  red.reset();
  blue.reset();
  EXPECT_FALSE(red_lifetime.expired());
  EXPECT_FALSE(blue_lifetime.expired());
  {
    ::testing::InSequence sequence;
    EXPECT_CALL(ctx.mock_gl_impl_ref, ClearColor(1, 0, 0, 1)).Times(1);
    EXPECT_CALL(ctx.mock_gl_impl_ref, ClearColor(0, 0, 1, 1)).Times(1);
  }
  ASSERT_TRUE(ctx.reactor->React());
  EXPECT_TRUE(red_lifetime.expired());
  EXPECT_TRUE(blue_lifetime.expired());
}

TEST_F(RenderPassGLESCommandTest, ProgramAndYFlipTrackPipelineTransitions) {
  auto ctx = CreateRenderPassGLESContext(kMockResolverGLES, 101);
  SetYFlipLocation(ctx.pipeline, 7);
  auto second_pipeline = CreateSecondPipeline(ctx, 8, 202);

  for (const auto& pipeline :
       {ctx.pipeline, ctx.pipeline, second_pipeline, ctx.pipeline}) {
    ctx.render_pass->SetPipeline(PipelineRef(pipeline));
    ctx.render_pass->SetElementCount(1);
    ctx.render_pass->SetIndexBuffer({}, IndexType::kNone);
    ASSERT_TRUE(ctx.render_pass->Draw().ok());
  }

  // A wrapped framebuffer uses the opposite y-flip value. A new pass must
  // bind the program and upload its own value, even with the same pipeline.
  const TextureDescriptor description{.format = PixelFormat::kR8G8B8A8UNormInt,
                                      .size = {100, 100}};
  RenderTarget wrapped_target;
  ColorAttachment wrapped_color;
  wrapped_color.texture = TextureGLES::WrapFBO(ctx.reactor, description, 23);
  wrapped_color.store_action = StoreAction::kDontCare;
  wrapped_target.SetColorAttachment(wrapped_color, 0);
  auto wrapped_pass = ctx.command_buffer->CreateRenderPass(wrapped_target);
  wrapped_pass->SetPipeline(PipelineRef(ctx.pipeline));
  wrapped_pass->SetElementCount(1);
  wrapped_pass->SetIndexBuffer({}, IndexType::kNone);
  wrapped_pass->SetScissor(IRect32::MakeXYWH(10, 20, 30, 40));
  ASSERT_TRUE(wrapped_pass->Draw().ok());

  EXPECT_CALL(ctx.mock_gl_impl_ref, Scissor(10, 40, 30, 40)).Times(1);

  {
    ::testing::InSequence sequence;
    EXPECT_CALL(ctx.mock_gl_impl_ref, UseProgram(101)).Times(1);
    EXPECT_CALL(ctx.mock_gl_impl_ref, Uniform1fv(7, 1, Pointee(-1.0f)))
        .Times(1);
    EXPECT_CALL(ctx.mock_gl_impl_ref, UseProgram(202)).Times(1);
    EXPECT_CALL(ctx.mock_gl_impl_ref, Uniform1fv(8, 1, Pointee(-1.0f)))
        .Times(1);
    EXPECT_CALL(ctx.mock_gl_impl_ref, UseProgram(101)).Times(1);
    EXPECT_CALL(ctx.mock_gl_impl_ref, Uniform1fv(7, 1, Pointee(-1.0f)))
        .Times(1);
    EXPECT_CALL(ctx.mock_gl_impl_ref, UseProgram(101)).Times(1);
    EXPECT_CALL(ctx.mock_gl_impl_ref, Uniform1fv(7, 1, Pointee(1.0f))).Times(1);
  }

  ASSERT_TRUE(ctx.render_pass->EncodeCommands());
  ASSERT_TRUE(ctx.reactor->React());
  ASSERT_TRUE(wrapped_pass->EncodeCommands());
  ASSERT_TRUE(ctx.reactor->React());
}

// Canvas only emits SetScissor when clip coverage changes. Draw() clears
// pending command metadata, so a missing rectangle means keep the active clip.
TEST_F(RenderPassGLESCommandTest, DrawsInheritScissorUntilThePassEnds) {
  for (bool wrapped : {false, true}) {
    auto ctx = CreateRenderPassGLESContext();
    RenderTarget target = ctx.render_pass->GetRenderTarget();
    if (wrapped) {
      const TextureDescriptor desc{.format = PixelFormat::kR8G8B8A8UNormInt,
                                   .size = {100, 100}};
      ColorAttachment color;
      color.texture = TextureGLES::WrapFBO(ctx.reactor, desc, 23);
      color.store_action = StoreAction::kDontCare;
      target.SetColorAttachment(color, 0);
    }
    auto pass = ctx.command_buffer->CreateRenderPass(target);
    auto next_pass = ctx.command_buffer->CreateRenderPass(target);
    const IRect32 damage = IRect32::MakeXYWH(10, 20, 30, 40);
    const IRect32 full_target = IRect32::MakeXYWH(0, 0, 100, 100);
    const std::optional<IRect32> updates[] = {
        std::nullopt, damage, std::nullopt, damage, full_target, std::nullopt};
    for (const auto& update : updates) {
      pass->SetPipeline(PipelineRef(ctx.pipeline));
      pass->SetElementCount(1);
      pass->SetIndexBuffer({}, IndexType::kNone);
      if (update) {
        pass->SetScissor(*update);
      }
      ASSERT_TRUE(pass->Draw().ok());
    }
    next_pass->SetPipeline(PipelineRef(ctx.pipeline));
    next_pass->SetElementCount(1);
    next_pass->SetIndexBuffer({}, IndexType::kNone);
    ASSERT_TRUE(next_pass->Draw().ok());

    bool scissor_enabled = false;
    IRect32 actual_rect;
    ON_CALL(ctx.mock_gl_impl_ref, Enable(GL_SCISSOR_TEST))
        .WillByDefault([&](GLenum) { scissor_enabled = true; });
    ON_CALL(ctx.mock_gl_impl_ref, Disable(GL_SCISSOR_TEST))
        .WillByDefault([&](GLenum) { scissor_enabled = false; });
    ON_CALL(ctx.mock_gl_impl_ref, Scissor(_, _, _, _))
        .WillByDefault([&](GLint x, GLint y, GLsizei width, GLsizei height) {
          actual_rect = IRect32::MakeXYWH(x, y, width, height);
        });
    size_t draw_index = 0;
    EXPECT_CALL(ctx.mock_gl_impl_ref, DrawArrays(_, _, _))
        .Times(7)
        .WillRepeatedly([&](GLenum, GLint, GLsizei) {
          SCOPED_TRACE(draw_index);
          const bool should_clip = draw_index > 0 && draw_index < 6;
          EXPECT_EQ(scissor_enabled, should_clip);
          if (should_clip) {
            const auto expected =
                draw_index < 4
                    ? IRect32::MakeXYWH(10, wrapped ? 40 : 20, 30, 40)
                    : full_target;
            EXPECT_EQ(actual_rect, expected);
          }
          draw_index++;
        });
    ASSERT_TRUE(pass->EncodeCommands());
    ASSERT_TRUE(ctx.reactor->React());
    ASSERT_TRUE(next_pass->EncodeCommands());
    ASSERT_TRUE(ctx.reactor->React());
  }
}

TEST_F(RenderPassGLESCommandTest, ScissorTracksChangesAndResetsAtPassBoundary) {
  auto ctx = CreateRenderPassGLESContext();
  const IRect32 first = IRect32::MakeXYWH(10, 20, 30, 40);
  const IRect32 second = IRect32::MakeXYWH(5, 6, 7, 8);
  const std::optional<IRect32> scissors[] = {first, first, second, std::nullopt,
                                             first};
  for (const auto& scissor : scissors) {
    ctx.render_pass->SetPipeline(PipelineRef(ctx.pipeline));
    ctx.render_pass->SetElementCount(1);
    ctx.render_pass->SetIndexBuffer({}, IndexType::kNone);
    if (scissor.has_value()) {
      ctx.render_pass->SetScissor(*scissor);
    }
    ASSERT_TRUE(ctx.render_pass->Draw().ok());
  }

  auto next_pass =
      ctx.command_buffer->CreateRenderPass(ctx.render_pass->GetRenderTarget());
  next_pass->SetPipeline(PipelineRef(ctx.pipeline));
  next_pass->SetElementCount(1);
  next_pass->SetIndexBuffer({}, IndexType::kNone);
  next_pass->SetScissor(first);
  ASSERT_TRUE(next_pass->Draw().ok());

  EXPECT_CALL(ctx.mock_gl_impl_ref, Enable(_)).Times(::testing::AnyNumber());
  EXPECT_CALL(ctx.mock_gl_impl_ref, Disable(_)).Times(::testing::AnyNumber());
  EXPECT_CALL(ctx.mock_gl_impl_ref, Enable(GL_SCISSOR_TEST)).Times(2);
  // Only reset at pass boundaries; absent command metadata preserves the clip.
  EXPECT_CALL(ctx.mock_gl_impl_ref, Disable(GL_SCISSOR_TEST)).Times(2);
  EXPECT_CALL(ctx.mock_gl_impl_ref, Scissor(10, 20, 30, 40)).Times(3);
  EXPECT_CALL(ctx.mock_gl_impl_ref, Scissor(5, 6, 7, 8)).Times(1);

  ASSERT_TRUE(ctx.render_pass->EncodeCommands());
  ASSERT_TRUE(ctx.reactor->React());
  ASSERT_TRUE(next_pass->EncodeCommands());
  ASSERT_TRUE(ctx.reactor->React());
}

TEST_F(RenderPassGLESCommandTest, ViewportCachedAcrossCommands) {
  auto ctx = CreateRenderPassGLESContext();
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(1);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  EXPECT_TRUE(render_pass->Draw().ok());

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(1);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  render_pass->SetViewport(
      Viewport{Rect::MakeXYWH(0, 0, 50, 50), DepthRange{0.0f, 1.0f}});
  EXPECT_TRUE(render_pass->Draw().ok());

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(1);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  render_pass->SetViewport(
      Viewport{Rect::MakeXYWH(0, 0, 50, 50), DepthRange{0.0f, 1.0f}});
  EXPECT_TRUE(render_pass->Draw().ok());

  // Viewport should only be called twice. Once for the fallback, once for the
  // first override. We set a catch-all to 0 to ensure no other calls occur.
  EXPECT_CALL(mock_gl_impl_ref, Viewport(_, _, _, _)).Times(0);
  EXPECT_CALL(mock_gl_impl_ref, Viewport(0, 0, 100, 100)).Times(1);
  EXPECT_CALL(mock_gl_impl_ref, Viewport(0, 0, 50, 50)).Times(1);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

TEST_F(RenderPassGLESCommandTest,
       CommandsWithoutViewportGetRenderPassViewport) {
  auto ctx = CreateRenderPassGLESContext();
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(1);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  EXPECT_TRUE(render_pass->Draw().ok());

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(1);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  render_pass->SetViewport(
      Viewport{Rect::MakeXYWH(0, 0, 50, 50), DepthRange{0.0f, 1.0f}});
  EXPECT_TRUE(render_pass->Draw().ok());

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(1);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  EXPECT_TRUE(render_pass->Draw().ok());

  EXPECT_CALL(mock_gl_impl_ref, Viewport(_, _, _, _)).Times(0);
  EXPECT_CALL(mock_gl_impl_ref, Viewport(0, 0, 100, 100)).Times(2);
  EXPECT_CALL(mock_gl_impl_ref, Viewport(0, 0, 50, 50)).Times(1);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

// Sibling regression guard for the bug fixed alongside this test on the
// Vulkan backend. The GLES backend has always honored the X offset; this
// asserts that explicitly so a future change can't silently regress it.
TEST_F(RenderPassGLESCommandTest, ViewportWithNonZeroXOffsetReachesGL) {
  auto ctx = CreateRenderPassGLESContext();
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(1);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  render_pass->SetViewport(
      Viewport{Rect::MakeXYWH(25, 0, 50, 100), DepthRange{0.0f, 1.0f}});
  EXPECT_TRUE(render_pass->Draw().ok());

  EXPECT_CALL(mock_gl_impl_ref, Viewport(_, _, _, _)).Times(0);
  EXPECT_CALL(mock_gl_impl_ref, Viewport(25, 0, 50, 100)).Times(1);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

// When the driver exposes the hardware instancing entry points, a
// non-indexed instanced command issues a single glDrawArraysInstanced call
// that carries the instance count.
TEST_F(RenderPassGLESCommandTest, HardwareInstancedArrayDraw) {
  auto ctx = CreateRenderPassGLESContext();
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(3);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  render_pass->SetInstanceCount(4);
  EXPECT_TRUE(render_pass->Draw().ok());

  EXPECT_CALL(mock_gl_impl_ref,
              DrawArraysInstanced(/*mode=*/_, /*first=*/0, /*count=*/3,
                                  /*instancecount=*/4))
      .Times(1);
  EXPECT_CALL(mock_gl_impl_ref, DrawArrays(_, _, _)).Times(0);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

// The indexed counterpart: a hardware instanced indexed command issues a
// single glDrawElementsInstanced call.
TEST_F(RenderPassGLESCommandTest, HardwareInstancedElementsDraw) {
  auto ctx = CreateRenderPassGLESContext();
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  DeviceBufferDescriptor index_desc;
  index_desc.size = 6 * sizeof(uint16_t);
  index_desc.storage_mode = StorageMode::kHostVisible;
  auto index_buffer = std::static_pointer_cast<Context>(ctx.context)
                          ->GetResourceAllocator()
                          ->CreateBuffer(index_desc);
  ASSERT_TRUE(index_buffer);

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(6);
  ASSERT_TRUE(render_pass->SetIndexBuffer(
      DeviceBuffer::AsBufferView(index_buffer), IndexType::k16bit));
  render_pass->SetInstanceCount(4);
  EXPECT_TRUE(render_pass->Draw().ok());

  EXPECT_CALL(mock_gl_impl_ref,
              DrawElementsInstanced(/*mode=*/_, /*count=*/6, /*type=*/_,
                                    /*indices=*/_, /*instancecount=*/4))
      .Times(1);
  EXPECT_CALL(mock_gl_impl_ref, DrawElements(_, _, _, _)).Times(0);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

// When the hardware instancing entry points are missing, a non-indexed
// instanced command is emulated by repeating the plain draw once per
// instance.
TEST_F(RenderPassGLESCommandTest, EmulatedInstancedArrayDraw) {
  auto ctx = CreateRenderPassGLESContext(kMockResolverGLESWithoutInstancing);
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(3);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  render_pass->SetInstanceCount(4);
  EXPECT_TRUE(render_pass->Draw().ok());

  EXPECT_CALL(mock_gl_impl_ref,
              DrawArrays(/*mode=*/_, /*first=*/0, /*count=*/3))
      .Times(4);
  EXPECT_CALL(mock_gl_impl_ref, DrawArraysInstanced(_, _, _, _)).Times(0);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

// The indexed counterpart of the emulation path: the plain indexed draw is
// repeated once per instance.
TEST_F(RenderPassGLESCommandTest, EmulatedInstancedElementsDraw) {
  auto ctx = CreateRenderPassGLESContext(kMockResolverGLESWithoutInstancing);
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  DeviceBufferDescriptor index_desc;
  index_desc.size = 6 * sizeof(uint16_t);
  index_desc.storage_mode = StorageMode::kHostVisible;
  auto index_buffer = std::static_pointer_cast<Context>(ctx.context)
                          ->GetResourceAllocator()
                          ->CreateBuffer(index_desc);
  ASSERT_TRUE(index_buffer);

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(6);
  ASSERT_TRUE(render_pass->SetIndexBuffer(
      DeviceBuffer::AsBufferView(index_buffer), IndexType::k16bit));
  render_pass->SetInstanceCount(4);
  EXPECT_TRUE(render_pass->Draw().ok());

  EXPECT_CALL(mock_gl_impl_ref,
              DrawElements(/*mode=*/_, /*count=*/6, /*type=*/_, /*indices=*/_))
      .Times(4);
  EXPECT_CALL(mock_gl_impl_ref, DrawElementsInstanced(_, _, _, _, _)).Times(0);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

// Regression guard: a command with no instance count set draws a single
// instance through the plain, non-instanced entry point.
TEST_F(RenderPassGLESCommandTest, NonInstancedDrawIssuesSingleDrawArrays) {
  auto ctx = CreateRenderPassGLESContext();
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(3);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  EXPECT_TRUE(render_pass->Draw().ok());

  EXPECT_CALL(mock_gl_impl_ref,
              DrawArrays(/*mode=*/_, /*first=*/0, /*count=*/3))
      .Times(1);
  EXPECT_CALL(mock_gl_impl_ref, DrawArraysInstanced(_, _, _, _)).Times(0);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

// A command with an explicit instance count of zero draws nothing, matching
// the Metal and Vulkan backends.
TEST_F(RenderPassGLESCommandTest, ZeroInstanceCountIssuesNoDraw) {
  auto ctx = CreateRenderPassGLESContext();
  testing::NiceMock<MockGLESImpl>& mock_gl_impl_ref = ctx.mock_gl_impl_ref;
  std::shared_ptr<RenderPass>& render_pass = ctx.render_pass;
  std::shared_ptr<PipelineGLES>& pipeline = ctx.pipeline;
  std::shared_ptr<ReactorGLES>& reactor = ctx.reactor;

  render_pass->SetPipeline(PipelineRef(pipeline));
  render_pass->SetElementCount(3);
  render_pass->SetIndexBuffer({}, IndexType::kNone);
  render_pass->SetInstanceCount(0);
  EXPECT_TRUE(render_pass->Draw().ok());

  EXPECT_CALL(mock_gl_impl_ref, DrawArrays(_, _, _)).Times(0);
  EXPECT_CALL(mock_gl_impl_ref, DrawArraysInstanced(_, _, _, _)).Times(0);

  EXPECT_TRUE(render_pass->EncodeCommands());
  EXPECT_TRUE(reactor->React());
}

}  // namespace testing
}  // namespace impeller
