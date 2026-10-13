// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Headless unit tests for the OpenGL texture bridge. These exercise the parts
// that need NO GL context and NO Ogre runtime: the QarVideoFrameOpenGl marshalling
// (pure), null-argument validation (returns before any Ogre call), and the GL
// interop value helpers. The GL-context-dependent paths (native id extraction,
// glIsTexture/dims validation, fence create/wait, and the zero-copy roundtrip) need
// a live GL context and are not covered here.

#include <gtest/gtest.h>

#include "qar_ogre_streaming/gl_interop.hpp"
#include "qar_ogre_streaming/ogre_opengl_texture_bridge.hpp"
#include "qar_ogre_streaming/types.hpp"

using namespace qar_ogre_streaming;

namespace
{

GlVideoTexture makeTex(uint32_t id, QarPixelFormat fmt, uint32_t w, uint32_t h)
{
  GlVideoTexture t{};
  t.texture_id = id;
  t.target = gl_interop::gl_texture_2d();
  t.size.format = fmt;
  t.size.width = w;
  t.size.height = h;
  t.size.array_layers = 1u;
  return t;
}

QarOpenGlStereoFrame makeColorOnlyFrame()
{
  QarOpenGlStereoFrame f{};
  f.layout = QAR_FRAME_LAYOUT_SEPARATED_TEXTURES;
  f.left_color = makeTex(11u, QAR_PIXEL_FORMAT_R8G8B8A8, 1280u, 720u);
  f.right_color = makeTex(22u, QAR_PIXEL_FORMAT_R8G8B8A8, 1280u, 720u);
  f.has_depth = false;
  f.views[0] = QarRenderFrameView{QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_LEFT, 0u};
  f.views[1] =
    QarRenderFrameView{QAR_VIDEO_FRAME_VIEW_TYPE_COLOR, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT, 1u};
  f.views_count = 2;
  return f;
}

}  // namespace

TEST(OpenGlBridge, FillFrameColorOnlyMapsTexturesAndViews)
{
  QarOpenGlStereoFrame src = makeColorOnlyFrame();
  src.synchronization.fence_sync = reinterpret_cast<void *>(0xABCDull);
  src.synchronization.qar_waits_on_fence = true;

  QarVideoFrameOpenGl out{};
  const Result r = OgreOpenGlTextureBridge::fill_opengl_frame(src, out);
  ASSERT_TRUE(r.ok()) << r.message;

  ASSERT_EQ(out.textures_count, 2u);
  // The real QAR ABI texture struct QarVideoTextureOpenGl exposes `texture` (the
  // GL id) and `size` only — no `target` (implicit GL_TEXTURE_2D per layout).
  EXPECT_EQ(out.textures[0].texture, 11u);  // left color
  EXPECT_EQ(out.textures[1].texture, 22u);  // right color

  ASSERT_EQ(out.texture_views_count, 2u);
  // View 0 = left color -> texture 0, full-frame rect, format carried from texture.
  EXPECT_EQ(out.texture_views[0].data_type, QAR_VIDEO_FRAME_VIEW_TYPE_COLOR);
  EXPECT_EQ(out.texture_views[0].eye, QAR_VIDEO_FRAME_VIEW_EYE_LEFT);
  EXPECT_EQ(out.texture_views[0].texture_index, 0u);
  EXPECT_EQ(out.texture_views[0].start_x, 0u);
  EXPECT_EQ(out.texture_views[0].start_y, 0u);
  EXPECT_EQ(out.texture_views[0].end_x, 1280u);
  EXPECT_EQ(out.texture_views[0].end_y, 720u);
  EXPECT_EQ(out.texture_views[0].texture_format, QAR_PIXEL_FORMAT_R8G8B8A8);
  EXPECT_EQ(out.texture_views[1].eye, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT);
  EXPECT_EQ(out.texture_views[1].texture_index, 1u);

  // NOTE: the SDK's QarVideoFrameOpenGl has NO synchronization field, so the GL
  // fence is intentionally NOT carried on the ABI struct (the producer keeps it in
  // its own GlFrameSync). The src.synchronization set above is ignored here.
}

TEST(OpenGlBridge, FillFrameWithDepthHasFourTexturesAndViews)
{
  QarOpenGlStereoFrame src = makeColorOnlyFrame();
  src.has_depth = true;
  src.left_depth = makeTex(33u, QAR_PIXEL_FORMAT_R32_FLOAT, 1280u, 720u);
  src.right_depth = makeTex(44u, QAR_PIXEL_FORMAT_R32_FLOAT, 1280u, 720u);
  src.views[2] = QarRenderFrameView{QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_LEFT, 2u};
  src.views[3] =
    QarRenderFrameView{QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT, 3u};
  src.views_count = 4;

  QarVideoFrameOpenGl out{};
  ASSERT_TRUE(OgreOpenGlTextureBridge::fill_opengl_frame(src, out).ok());
  ASSERT_EQ(out.textures_count, 4u);
  EXPECT_EQ(out.textures[2].texture, 33u);
  EXPECT_EQ(out.textures[3].texture, 44u);
  ASSERT_EQ(out.texture_views_count, 4u);
  EXPECT_EQ(out.texture_views[2].data_type, QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH);
  EXPECT_EQ(out.texture_views[2].texture_format, QAR_PIXEL_FORMAT_R32_FLOAT);
  EXPECT_EQ(out.texture_views[3].texture_index, 3u);
}

TEST(OpenGlBridge, FillFrameRejectsOutOfRangeViewIndex)
{
  QarOpenGlStereoFrame src = makeColorOnlyFrame();
  // A view references a depth texture index that does not exist (has_depth=false).
  src.views[1] =
    QarRenderFrameView{QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT, 3u};

  QarVideoFrameOpenGl out{};
  const Result r = OgreOpenGlTextureBridge::fill_opengl_frame(src, out);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status, Status::InvalidArgument);
}

TEST(OpenGlBridge, ExtractNullTextureIsInvalidArgument)
{
  OgreOpenGlTextureBridge bridge;
  GlVideoTexture out{};
  const Result r = bridge.extract_gl_texture(nullptr, out);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status, Status::InvalidArgument);
}

TEST(OpenGlBridge, CpuReadbackNullTextureIsInvalidArgument)
{
  OgreOpenGlTextureBridge bridge;
  std::vector<uint8_t> bytes;
  QarVideoTextureCpu out{};
  const Result r = bridge.cpu_readback(nullptr, bytes, out);
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status, Status::InvalidArgument);
}

TEST(OpenGlBridge, ModeDefaultsToZeroCopy)
{
  OgreOpenGlTextureBridge bridge;
  EXPECT_EQ(bridge.mode(), BridgeMode::ZeroCopyGl);
  OgreOpenGlTextureBridge fallback(BridgeMode::CpuReadbackFallback);
  EXPECT_EQ(fallback.mode(), BridgeMode::CpuReadbackFallback);
}

TEST(GlInterop, TextureTargetConstant)
{
  EXPECT_EQ(gl_interop::gl_texture_2d(), 0x0DE1u);  // GL_TEXTURE_2D
}

TEST(GlInterop, FenceWaitStatusToString)
{
  using S = gl_interop::FenceWaitStatus;
  EXPECT_STREQ(gl_interop::to_string(S::AlreadySignaled), "GL_ALREADY_SIGNALED");
  EXPECT_STREQ(gl_interop::to_string(S::ConditionSatisfied), "GL_CONDITION_SATISFIED");
  EXPECT_STREQ(gl_interop::to_string(S::TimeoutExpired), "GL_TIMEOUT_EXPIRED");
  EXPECT_STREQ(gl_interop::to_string(S::WaitFailed), "GL_WAIT_FAILED");
  EXPECT_STREQ(gl_interop::to_string(S::NoFence), "NO_FENCE");
}
