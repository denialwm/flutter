// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <map>
#include <set>
#include <tuple>

#include "flutter/testing/testing.h"
#include "impeller/renderer/backend/gles/buffer_bindings_gles.h"
#include "impeller/renderer/backend/gles/pass_bindings_gles.h"
#include "impeller/renderer/backend/gles/program_gles.h"
#include "impeller/renderer/backend/gles/sampler_library_gles.h"
#include "impeller/renderer/backend/gles/test/mock_gles.h"
#include "impeller/renderer/backend/gles/texture_gles.h"

namespace impeller::testing {
namespace {

// Model effective GL state, so tests check what the next draw would observe,
// not merely how many setters were removed. No real GL context is created.
class StateGL : public MockGLESImpl {
 public:
  GLuint active = 0;
  GLuint next_sampler = 100;
  size_t active_calls = 0, uniform_calls = 0, parameter_calls = 0;
  size_t enable_calls = 0, disable_calls = 0, divisor_calls = 0;
  size_t texture_binds = 0, waits = 0, pointer_calls = 0;
  std::map<GLuint, GLuint> textures, samplers;
  std::map<GLint, GLint> uniforms;
  std::map<std::pair<GLuint, GLenum>, GLint> sampler_params, texture_params;
  std::set<GLuint> enabled, deleted_samplers;
  std::map<GLuint, GLuint> divisors;

  void DeleteTextures(GLsizei, const GLuint*) override {}
  void GetIntegerv(GLenum name, GLint* value) override { *value = 16; }
  void ActiveTexture(GLenum unit) override {
    active = unit - GL_TEXTURE0;
    active_calls++;
  }
  void Uniform1i(GLint location, GLint value) override {
    uniforms[location] = value;
    uniform_calls++;
  }
  void BindTexture(GLenum target, GLuint texture) override {
    textures[active] = texture;
    texture_binds++;
  }
  void GenSamplers(GLsizei count, GLuint* names) override {
    for (GLsizei i = 0; i < count; i++) {
      names[i] = next_sampler++;
    }
  }
  void DeleteSamplers(GLsizei count, const GLuint* names) override {
    for (GLsizei i = 0; i < count; i++) {
      deleted_samplers.insert(names[i]);
    }
  }
  GLboolean IsSampler(GLuint sampler) override { return GL_TRUE; }
  void BindSampler(GLuint unit, GLuint sampler) override {
    samplers[unit] = sampler;
  }
  void SamplerParameteri(GLuint sampler,
                         GLenum parameter,
                         GLint value) override {
    sampler_params[{sampler, parameter}] = value;
    parameter_calls++;
  }
  void TexParameteri(GLenum target, GLenum parameter, GLint value) override {
    texture_params[{textures[active], parameter}] = value;
  }
  void EnableVertexAttribArray(GLuint index) override {
    enabled.insert(index);
    enable_calls++;
  }
  void DisableVertexAttribArray(GLuint index) override {
    enabled.erase(index);
    disable_calls++;
  }
  void VertexAttribDivisor(GLuint index, GLuint divisor) override {
    divisors[index] = divisor;
    divisor_calls++;
  }
  void VertexAttribPointer(GLuint,
                           GLint,
                           GLenum,
                           GLboolean,
                           GLsizei,
                           const void*) override {
    pointer_calls++;
  }
  GLsync FenceSync(GLenum, GLbitfield) override {
    return reinterpret_cast<GLsync>(1);
  }
  void WaitSync(GLsync, GLbitfield, GLuint64) override { waits++; }

  GLint MinFilter(GLuint unit) {
    if (samplers[unit]) {
      return sampler_params.at({samplers[unit], GL_TEXTURE_MIN_FILTER});
    }
    return texture_params.at({textures[unit], GL_TEXTURE_MIN_FILTER});
  }
};

class Worker : public ReactorGLES::Worker {
 public:
  bool CanReactorReactOnCurrentThreadNow(const ReactorGLES&) const override {
    return true;
  }
};
}  // namespace

class GLESStateCacheTest : public ::testing::Test {
 protected:
  StateGL* state;
  std::shared_ptr<MockGLES> mock;
  std::shared_ptr<ReactorGLES> reactor;
  std::shared_ptr<Worker> worker;

  void Initialize(const char* version = "OpenGL ES 3.0") {
    auto impl = std::make_unique<StateGL>();
    state = impl.get();
    mock = MockGLES::Init(std::move(impl), std::nullopt, version);
    reactor = std::make_shared<ReactorGLES>(
        std::make_unique<ProcTableGLES>(kMockResolverGLES));
    worker = std::make_shared<Worker>();
    reactor->AddWorker(worker);
  }

  void TearDown() override {
    if (reactor) {
      EXPECT_TRUE(reactor->React());
    }
    reactor.reset();
    worker.reset();
    mock.reset();
  }

  std::shared_ptr<TextureGLES> Texture(
      GLuint name,
      size_t mips = 1,
      TextureType type = TextureType::kTexture2D) {
    TextureDescriptor desc;
    desc.type = type;
    desc.format = PixelFormat::kR8G8B8A8UNormInt;
    desc.size = {16, 16};
    desc.mip_count = mips;
    return TextureGLES::WrapTexture(
        reactor, desc, reactor->CreateHandle(HandleType::kTexture, name));
  }

  void SetTextureLocation(BufferBindingsGLES& bindings, GLint location) {
    bindings.SetUniformBindings({{"IMAGE", location}});
  }

  bool Bind(BufferBindingsGLES& bindings,
            ProgramGLES& program,
            PassBindingsGLES& pass,
            const std::shared_ptr<TextureGLES>& texture,
            raw_ptr<const Sampler> sampler) {
    ShaderMetadata metadata{.name = "image"};
    std::vector<TextureAndSampler> textures;
    textures.push_back({.slot = {},
                        .stage = ShaderStage::kFragment,
                        .texture = TextureResource(&metadata, texture),
                        .sampler = sampler});
    return bindings.BindUniformData(reactor->GetProcTable(), textures, {},
                                    Range{0, 1}, Range{0, 0}, &program, &pass);
  }
};

TEST_F(GLESStateCacheTest, SamplerUniformsFollowSharedProgramAndLifetime) {
  Initialize();
  auto shared = std::make_shared<ProgramGLES>(UniqueHandleGLES(
      reactor, reactor->CreateHandle(HandleType::kProgram, 17)));
  auto variant_a = shared;
  auto variant_b = shared;
  const auto& gl = reactor->GetProcTable();
  variant_a->SetSamplerUnit(gl, 3, 0);
  variant_a->SetSamplerUnit(gl, 3, 0);
  EXPECT_EQ(state->uniform_calls, 1u);
  variant_b->SetSamplerUnit(gl, 3, 1);
  variant_a->SetSamplerUnit(gl, 3, 0);
  EXPECT_EQ(state->uniforms[3], 0);
  EXPECT_EQ(state->uniform_calls, 3u);
  ProgramGLES replacement(UniqueHandleGLES(
      reactor, reactor->CreateHandle(HandleType::kProgram, 18)));
  replacement.SetSamplerUnit(gl, 3, 0);
  EXPECT_EQ(state->uniform_calls, 4u);
  replacement.SetSamplerUnit(gl, -1, 0);
  EXPECT_EQ(state->uniform_calls, 4u);
}

TEST_F(GLESStateCacheTest, NativeSamplersSurvivePassesAndTextureRotation) {
  Initialize();
  const auto& gl = reactor->GetProcTable();
  SamplerLibraryGLES library(false, reactor);
  auto sampler =
      static_cast<SamplerLibrary&>(library).GetSampler(SamplerDescriptor{});
  ProgramGLES program(UniqueHandleGLES(
      reactor, reactor->CreateHandle(HandleType::kProgram, 17)));
  BufferBindingsGLES a, b;
  SetTextureLocation(a, 3);
  SetTextureLocation(b, 3);
  auto texture_a = Texture(40);
  auto texture_b = Texture(41);
  GLuint native = 0;
  for (const auto& texture : {texture_a, texture_b, texture_a}) {
    PassBindingsGLES pass(gl);
    ASSERT_TRUE(Bind(a, program, pass, texture, sampler));
    if (!native) {
      native = state->samplers[0];
    }
    EXPECT_NE(native, 0u);
    EXPECT_EQ(state->samplers[0], native);
    EXPECT_EQ(state->textures[0], *texture->GetGLHandle());
    EXPECT_EQ(state->MinFilter(0), GL_NEAREST);
    EXPECT_EQ(state->texture_params[std::make_pair(
                  *texture->GetGLHandle(), GLenum(GL_TEXTURE_MAX_LEVEL))],
              0);
    ASSERT_TRUE(Bind(b, program, pass, texture, sampler));
  }
  EXPECT_EQ(state->parameter_calls, 4u);
  EXPECT_EQ(state->uniform_calls, 1u);
  EXPECT_EQ(state->active_calls, 3u);   // unknown on each pass entry
  EXPECT_EQ(state->texture_binds, 6u);  // acquisition is never cached away
  EXPECT_EQ(state->samplers[0], 0u);
}

TEST_F(GLESStateCacheTest, MipsAndLegacyFallbackPreserveSampling) {
  Initialize();
  const auto& gl = reactor->GetProcTable();
  SamplerLibraryGLES library(true, reactor);
  SamplerDescriptor desc;
  desc.min_filter = MinMagFilter::kLinear;
  desc.mip_filter = MipFilter::kLinear;
  auto sampler = static_cast<SamplerLibrary&>(library).GetSampler(desc);
  desc.max_anisotropy = 2;
  auto fallback = static_cast<SamplerLibrary&>(library).GetSampler(desc);
  ProgramGLES program(UniqueHandleGLES(
      reactor, reactor->CreateHandle(HandleType::kProgram, 17)));
  BufferBindingsGLES bindings;
  SetTextureLocation(bindings, 3);
  auto single = Texture(40);
  auto mipped = Texture(41, 3);
  PassBindingsGLES pass(gl);
  ASSERT_TRUE(Bind(bindings, program, pass, single, sampler));
  GLuint base_sampler = state->samplers[0];
  EXPECT_EQ(state->MinFilter(0), GL_LINEAR);
  ASSERT_TRUE(Bind(bindings, program, pass, mipped, sampler));
  EXPECT_NE(state->samplers[0], base_sampler);
  EXPECT_EQ(state->MinFilter(0), GL_LINEAR_MIPMAP_LINEAR);
  EXPECT_EQ(
      state->texture_params[std::make_pair(41u, GLenum(GL_TEXTURE_MAX_LEVEL))],
      2);
  ASSERT_TRUE(Bind(bindings, program, pass, mipped, fallback));
  EXPECT_EQ(state->samplers[0], 0u);
  EXPECT_EQ(state->MinFilter(0), GL_LINEAR_MIPMAP_LINEAR);
  ASSERT_TRUE(Bind(bindings, program, pass, single, sampler));
  EXPECT_EQ(state->samplers[0], base_sampler);
  EXPECT_EQ(state->parameter_calls, 8u);
}

TEST_F(GLESStateCacheTest, UnsupportedNativeConfigurationsUseLegacyPath) {
  Initialize("OpenGL ES 2.0");
  SamplerLibraryGLES library(false, reactor);
  auto sampler =
      static_cast<SamplerLibrary&>(library).GetSampler(SamplerDescriptor{});
  EXPECT_EQ(SamplerGLES::Cast(*sampler).GetNativeSampler(*Texture(40)), 0u);
  EXPECT_EQ(state->parameter_calls, 0u);
}

TEST_F(GLESStateCacheTest,
       DecalAndExternalTexturesDoNotAllocateNativeSamplers) {
  Initialize();
  SamplerLibraryGLES library(true, reactor);
  SamplerDescriptor desc;
  desc.width_address_mode = SamplerAddressMode::kDecal;
  auto decal = static_cast<SamplerLibrary&>(library).GetSampler(desc);
  EXPECT_EQ(SamplerGLES::Cast(*decal).GetNativeSampler(*Texture(40)), 0u);
  auto ordinary =
      static_cast<SamplerLibrary&>(library).GetSampler(SamplerDescriptor{});
  EXPECT_EQ(SamplerGLES::Cast(*ordinary).GetNativeSampler(
                *Texture(41, 1, TextureType::kTextureExternalOES)),
            0u);
  EXPECT_EQ(state->parameter_calls, 0u);
}

TEST_F(GLESStateCacheTest, MissingSamplerEntryPointKeepsLegacyPath) {
  Initialize();
  auto resolver = [](const char* name) -> void* {
    return strcmp(name, "glSamplerParameteri") == 0 ? nullptr
                                                    : kMockResolverGLES(name);
  };
  auto other =
      std::make_shared<ReactorGLES>(std::make_unique<ProcTableGLES>(resolver));
  other->AddWorker(worker);
  SamplerLibraryGLES library(false, other);
  auto sampler =
      static_cast<SamplerLibrary&>(library).GetSampler(SamplerDescriptor{});
  EXPECT_FALSE(other->GetProcTable().SupportsSamplerObjects());
  EXPECT_EQ(SamplerGLES::Cast(*sampler).GetNativeSampler(*Texture(40)), 0u);
  EXPECT_EQ(state->parameter_calls, 0u);
}

TEST_F(GLESStateCacheTest, CachedSamplingStillWaitsForNewProducerFence) {
  Initialize();
  SamplerLibraryGLES library(false, reactor);
  auto sampler =
      static_cast<SamplerLibrary&>(library).GetSampler(SamplerDescriptor{});
  ProgramGLES program(UniqueHandleGLES(
      reactor, reactor->CreateHandle(HandleType::kProgram, 17)));
  BufferBindingsGLES bindings;
  SetTextureLocation(bindings, 3);
  auto texture = Texture(40);
  PassBindingsGLES pass(reactor->GetProcTable());
  ASSERT_TRUE(Bind(bindings, program, pass, texture, sampler));
  texture->SetFence(reactor->CreateHandle(HandleType::kFence));
  ASSERT_TRUE(Bind(bindings, program, pass, texture, sampler));
  EXPECT_EQ(state->waits, 1u);
  EXPECT_EQ(state->texture_binds, 2u);
  EXPECT_EQ(state->parameter_calls, 4u);
}

TEST_F(GLESStateCacheTest, AttributeTransitionsAndEarlyExitCleanUp) {
  Initialize();
  const auto& gl = reactor->GetProcTable();
  auto draw = [&](PassBindingsGLES& pass, GLuint divisor) {
    pass.BeginDraw();
    pass.EnableVertexAttribute(0, divisor);
    pass.EndVertexSetup();
    EXPECT_EQ(state->enabled, std::set<GLuint>{0});
    EXPECT_EQ(state->divisors[0], divisor);
  };
  auto early_return = [&] {
    PassBindingsGLES pass(gl);
    draw(pass, 1);
    draw(pass, 1);
    EXPECT_EQ(state->enable_calls, 1u);
    EXPECT_EQ(state->divisor_calls, 1u);
    draw(pass, 0);
    EXPECT_EQ(state->divisors[0], 0u);
    pass.BeginDraw();
    pass.EnableVertexAttribute(2, 0);
    pass.EndVertexSetup();
    EXPECT_EQ(state->enabled, std::set<GLuint>{2});
    pass.BindSampler(0, 42);
    return false;
  };
  EXPECT_FALSE(early_return());
  EXPECT_TRUE(state->enabled.empty());
  EXPECT_EQ(state->samplers[0], 0u);
  PassBindingsGLES next(gl);
  draw(next, 0);
  EXPECT_EQ(state->divisor_calls,
            4u);  // first use never inherits a cached divisor
}

TEST_F(GLESStateCacheTest, AliasedTexturesUseTheRequestedSampler) {
  Initialize();
  SamplerLibraryGLES library(false, reactor);
  SamplerDescriptor desc;
  auto nearest = static_cast<SamplerLibrary&>(library).GetSampler(desc);
  desc.min_filter = MinMagFilter::kLinear;
  auto linear = static_cast<SamplerLibrary&>(library).GetSampler(desc);
  ProgramGLES program(UniqueHandleGLES(
      reactor, reactor->CreateHandle(HandleType::kProgram, 17)));
  BufferBindingsGLES bindings;
  SetTextureLocation(bindings, 3);
  auto make_alias = [&] {
    auto handle = reactor->CreateHandle(HandleType::kTexture, 40);
    // Borrowed names are retired by the embedder, not by each wrapper.
    EXPECT_TRUE(reactor->RegisterCleanupCallback(handle, [] {}));
    TextureDescriptor desc;
    desc.format = PixelFormat::kR8G8B8A8UNormInt;
    desc.size = {16, 16};
    return TextureGLES::WrapTexture(reactor, desc, handle);
  };
  auto a = make_alias();
  auto b = make_alias();
  PassBindingsGLES pass(reactor->GetProcTable());
  ASSERT_TRUE(Bind(bindings, program, pass, a, nearest));
  EXPECT_EQ(state->MinFilter(0), GL_NEAREST);
  ASSERT_TRUE(Bind(bindings, program, pass, b, linear));
  EXPECT_EQ(state->MinFilter(0), GL_LINEAR);
  ASSERT_TRUE(Bind(bindings, program, pass, a, nearest));
  EXPECT_EQ(state->MinFilter(0), GL_NEAREST);
  EXPECT_EQ(state->parameter_calls, 8u);
}

TEST_F(GLESStateCacheTest, NativeSamplerDeletionUsesReactor) {
  Initialize();
  GLuint name;
  {
    SamplerLibraryGLES library(false, reactor);
    auto sampler =
        static_cast<SamplerLibrary&>(library).GetSampler(SamplerDescriptor{});
    name = SamplerGLES::Cast(*sampler).GetNativeSampler(*Texture(40));
    ASSERT_NE(name, 0u);
  }
  EXPECT_TRUE(state->deleted_samplers.empty());
  ASSERT_TRUE(reactor->React());
  EXPECT_EQ(state->deleted_samplers, std::set<GLuint>{name});
}

}  // namespace impeller::testing
