// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Headless tests for the single-peer stereo stream. These cover:
//   * the pure per-eye pose/FOV conversion (compute_eye_render_params) vs the
//     pose helpers it is built from,
//   * separated-texture frame assembly (layout, eye labels, texture indices),
//   * stream lifecycle bookkeeping + render_and_submit() early-outs, and
//   * the recording sender capturing per-eye metadata driven by the frame source.
// None of these call an Ogre runtime function (the fake scene manager pointer is
// never dereferenced because no render target is ever created here); the real
// Ogre render path is exercised in RViz and in the standalone sample.

#include <cmath>
#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/mock_qar_frame_info_source.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming/qar_pose_conversion.hpp"
#include "qar_ogre_streaming_test_support/mock_qar_render_sender_adapter.hpp"
#include "test_helpers.hpp"

namespace qos = qar_ogre_streaming;
namespace qts = qar_ogre_streaming::test_support;

// --- compute_eye_render_params: must match the pose conversions verbatim ----

TEST(EyeRenderParams, MatchesM2PoseAndProjection)
{
  qos::EyeView eye{};
  eye.pose.orientation = QarQuaternion{0.0f, 0.2f, 0.0f, 0.98f};  // xyzw, non-unit
  eye.pose.position = QarVector3{1.0f, -2.0f, 3.0f};
  eye.fov = QarFov{-0.6f, 0.4f, 0.5f, -0.3f};
  eye.near_far = QarNearFar{0.1f, 100.0f};

  const qos::EyeRenderParams p = qos::compute_eye_render_params(eye);

  // Reference: call the pose helpers directly.
  qos::Vec3 ref_pos{};
  qos::Quat ref_orient{};
  ASSERT_TRUE(qos::qar_pose_to_ogre(eye.pose, ref_pos, ref_orient).ok());
  qos::Mat4 ref_proj{};
  ASSERT_TRUE(
    qos::make_projection_from_qar_fov(eye.fov, 0.1f, 100.0f, ref_proj).ok());

  EXPECT_TRUE(p.pose_valid);
  EXPECT_TRUE(p.projection_valid);
  EXPECT_FLOAT_EQ(p.position.x, ref_pos.x);
  EXPECT_FLOAT_EQ(p.position.y, ref_pos.y);
  EXPECT_FLOAT_EQ(p.position.z, ref_pos.z);
  EXPECT_FLOAT_EQ(p.orientation.w, ref_orient.w);
  EXPECT_FLOAT_EQ(p.orientation.x, ref_orient.x);
  for (int i = 0; i < 16; ++i) {
    EXPECT_FLOAT_EQ(p.projection.m[i], ref_proj.m[i]) << "proj entry " << i;
  }
}

TEST(EyeRenderParams, DegenerateInputsFlaggedInvalid)
{
  qos::EyeView eye{};
  eye.pose.orientation = QarQuaternion{0.0f, 0.0f, 0.0f, 0.0f};  // zero => degenerate
  eye.fov = QarFov{0.4f, -0.6f, 0.5f, -0.3f};  // angle_right <= angle_left => invalid
  eye.near_far = QarNearFar{0.1f, 100.0f};

  const qos::EyeRenderParams p = qos::compute_eye_render_params(eye);
  EXPECT_FALSE(p.pose_valid);
  EXPECT_FALSE(p.projection_valid);
}

// --- Separated-texture frame assembly -----------------------------------------

TEST(StereoFrameAssembly, SeparatedLayoutEyeLabelsAndTextures)
{
  qos::StereoFrameTextures t;
  t.color_size = QarTextureSize{QAR_PIXEL_FORMAT_R8G8B8A8, 1024u, 768u, 1u};
  t.left_color_id = 101u;
  t.right_color_id = 202u;
  t.has_depth = true;
  t.depth_size = QarTextureSize{QAR_PIXEL_FORMAT_R32_FLOAT, 1024u, 768u, 1u};
  t.left_depth_id = 303u;
  t.right_depth_id = 404u;

  const QarPeerId peer = qos::test::make_peer_id(0x5A);
  const qos::QarOpenGlStereoFrame f = qos::assemble_separated_stereo_frame(peer, 9u, t);

  EXPECT_EQ(f.layout, QAR_FRAME_LAYOUT_SEPARATED_TEXTURES);
  EXPECT_EQ(f.frame_index, 9u);
  EXPECT_TRUE(qos::peer_id_equal(f.peer_id, peer));

  // Positional textures: [0]=left color, [1]=right color.
  EXPECT_EQ(f.left_color.texture_id, 101u);
  EXPECT_EQ(f.right_color.texture_id, 202u);
  EXPECT_EQ(f.left_color.target, qos::kGlTexture2D);
  EXPECT_EQ(f.left_color.size.width, 1024u);
  EXPECT_EQ(f.left_color.size.format, QAR_PIXEL_FORMAT_R8G8B8A8);

  ASSERT_TRUE(f.has_depth);
  EXPECT_EQ(f.left_depth.texture_id, 303u);
  EXPECT_EQ(f.right_depth.texture_id, 404u);
  EXPECT_EQ(f.left_depth.size.format, QAR_PIXEL_FORMAT_R32_FLOAT);

  // Explicit views: view[0]=left color/tex0, view[1]=right color/tex1,
  // view[2]=left depth/tex2, view[3]=right depth/tex3.
  ASSERT_EQ(f.views_count, 4u);
  EXPECT_EQ(f.views[0].data_type, QAR_VIDEO_FRAME_VIEW_TYPE_COLOR);
  EXPECT_EQ(f.views[0].eye, QAR_VIDEO_FRAME_VIEW_EYE_LEFT);
  EXPECT_EQ(f.views[0].texture_index, 0u);
  EXPECT_EQ(f.views[1].eye, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT);
  EXPECT_EQ(f.views[1].texture_index, 1u);
  EXPECT_EQ(f.views[2].data_type, QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH);
  EXPECT_EQ(f.views[2].eye, QAR_VIDEO_FRAME_VIEW_EYE_LEFT);
  EXPECT_EQ(f.views[2].texture_index, 2u);
  EXPECT_EQ(f.views[3].eye, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT);
  EXPECT_EQ(f.views[3].texture_index, 3u);

  // No GL fence at assembly time (the stream attaches one when it submits).
  EXPECT_EQ(f.synchronization.fence_sync, nullptr);
  EXPECT_FALSE(f.synchronization.qar_waits_on_fence);
}

TEST(StereoFrameAssembly, ColorOnlyHasTwoViews)
{
  qos::StereoFrameTextures t;
  t.color_size = QarTextureSize{QAR_PIXEL_FORMAT_R8G8B8A8, 640u, 480u, 1u};
  t.left_color_id = 1u;
  t.right_color_id = 2u;
  t.has_depth = false;

  const qos::QarOpenGlStereoFrame f =
    qos::assemble_separated_stereo_frame(qos::test::make_peer_id(1), 0u, t);
  EXPECT_FALSE(f.has_depth);
  EXPECT_EQ(f.views_count, 2u);
}

// --- Stream lifecycle bookkeeping (no Ogre resources created) ----------------

namespace
{
std::shared_ptr<qts::MockQarRenderSenderAdapter> make_sender()
{
  return std::make_shared<qts::MockQarRenderSenderAdapter>();
}

qos::StereoStreamConfig make_stream_config(uint8_t seed)
{
  qos::StereoStreamConfig c;
  c.peer_id = qos::test::make_peer_id(seed);
  c.width = 640;
  c.height = 480;
  c.max_fps = 60;
  return c;
}
}  // namespace

TEST(StereoStreamLifecycle, ConstructionValidatesArguments)
{
  // Null scene manager rejected.
  EXPECT_THROW(
    qos::OgreQarStereoStream(nullptr, make_stream_config(1), make_sender()),
    std::invalid_argument);

  // Null sender rejected.
  EXPECT_THROW(
    qos::OgreQarStereoStream(
      qos::test::fake_scene_manager(), make_stream_config(1), nullptr),
    std::invalid_argument);

  // Invalid config (zero fps) rejected.
  qos::StereoStreamConfig bad = make_stream_config(1);
  bad.max_fps = 0;
  EXPECT_THROW(
    qos::OgreQarStereoStream(qos::test::fake_scene_manager(), bad, make_sender()),
    std::invalid_argument);
}

TEST(StereoStreamLifecycle, AccessorsAndActiveFlag)
{
  qos::OgreQarStereoStream s(qos::test::fake_scene_manager(), make_stream_config(4), make_sender());
  EXPECT_TRUE(qos::peer_id_equal(s.peer_id(), qos::test::make_peer_id(4)));
  EXPECT_EQ(s.config().width, 640u);
  EXPECT_FALSE(s.active());
  EXPECT_FALSE(s.targets_ready());
  s.set_active(true);
  EXPECT_TRUE(s.active());

  const qos::StreamDiagnostics d = s.diagnostics();
  EXPECT_EQ(d.width, 640u);
  EXPECT_EQ(d.max_fps, 60u);
  EXPECT_TRUE(d.active);
  EXPECT_EQ(d.frames_submitted, 0u);
  EXPECT_EQ(d.frames_skipped, 0u);
}

// --- Skip reasons: a skipped tick must say WHY --------------------------------
//
// A stall presents as `sub=` frozen while `skip=` climbs at the tick rate, with no
// error logged anywhere, unless every skip path names itself: "the viewer is not
// asking us to render" and "the adapter failed" are indistinguishable behind one
// bare counter. These lock in that each path names itself and that the per-reason
// totals always add up to frames_skipped.

TEST(StereoStreamSkipReason, SenderNotReadyIsNamedAndDetailed)
{
  auto sender = make_sender();
  qos::QarFrameInfo declined{};
  declined.should_render = false;  // adapter is fine, it just has no frame for us
  sender->set_next_frame_info(declined);
  sender->set_skip_detail("begin-frame pending; consecutive=7");

  qos::OgreQarStereoStream s(
    qos::test::fake_scene_manager(), make_stream_config(11), sender);
  s.set_active(true);

  EXPECT_FALSE(s.begin_frame_phase());
  EXPECT_EQ(s.last_skip_reason(), qos::SkipReason::SenderNotReady);

  const qos::StreamDiagnostics d = s.diagnostics();
  EXPECT_EQ(d.frames_skipped, 1u);
  EXPECT_EQ(d.last_skip_reason, qos::SkipReason::SenderNotReady);
  EXPECT_EQ(d.last_skip_detail, "begin-frame pending; consecutive=7");
  EXPECT_EQ(d.skips_sender_not_ready, 1u);
  EXPECT_EQ(d.skips_sender_error, 0u);
}

TEST(StereoStreamSkipReason, SenderErrorIsDistinctFromNotReady)
{
  auto sender = make_sender();
  qos::QarFrameInfo wanted{};
  wanted.should_render = true;  // proves the FALSE RETURN, not should_render, decides
  sender->set_next_frame_info(wanted);
  sender->set_begin_frame_result(false);
  sender->set_skip_detail("try_begin_frame failed: code=42");

  qos::OgreQarStereoStream s(
    qos::test::fake_scene_manager(), make_stream_config(12), sender);
  s.set_active(true);

  EXPECT_FALSE(s.begin_frame_phase());
  const qos::StreamDiagnostics d = s.diagnostics();
  EXPECT_EQ(d.last_skip_reason, qos::SkipReason::SenderError);
  EXPECT_EQ(d.last_skip_detail, "try_begin_frame failed: code=42");
  EXPECT_EQ(d.skips_sender_error, 1u);
  EXPECT_EQ(d.skips_sender_not_ready, 0u);
}

TEST(StereoStreamSkipReason, BudgetShedIsNamedAndTotalsReconcile)
{
  auto sender = make_sender();
  qos::QarFrameInfo declined{};
  declined.should_render = false;
  sender->set_next_frame_info(declined);

  qos::OgreQarStereoStream s(
    qos::test::fake_scene_manager(), make_stream_config(13), sender);
  s.set_active(true);

  s.begin_frame_phase();   // -> SenderNotReady
  s.note_budget_skip();    // -> BudgetShed, and it must be the LAST reason
  s.begin_frame_phase();   // -> SenderNotReady again
  s.note_budget_skip();

  const qos::StreamDiagnostics d = s.diagnostics();
  EXPECT_EQ(d.last_skip_reason, qos::SkipReason::BudgetShed);
  EXPECT_EQ(d.skips_budget, 2u);
  EXPECT_EQ(d.skips_sender_not_ready, 2u);
  // The whole point of routing every bail-out through one accountant: the
  // per-reason totals can never drift away from the headline counter.
  EXPECT_EQ(
    d.skips_sender_not_ready + d.skips_sender_error + d.skips_target_init +
      d.skips_budget,
    d.frames_skipped);
}

TEST(StereoStreamSkipReason, ReasonNamesAreStableStrings)
{
  EXPECT_STREQ(qos::to_string(qos::SkipReason::None), "none");
  EXPECT_STREQ(qos::to_string(qos::SkipReason::SenderNotReady), "sender_not_ready");
  EXPECT_STREQ(qos::to_string(qos::SkipReason::SenderError), "sender_error");
  EXPECT_STREQ(qos::to_string(qos::SkipReason::TargetInitFailed), "target_init_failed");
  EXPECT_STREQ(qos::to_string(qos::SkipReason::BudgetShed), "budget_shed");
}

TEST(StereoStreamLifecycle, ForwardsPrimeAndTickToSender)
{
  // Neither knob has a real effect on the test double (priming is an implementation
  // detail of the concrete C-API adapter) — this verifies that the panel -> manager
  // -> stream -> adapter plumbing reaches the sender.
  auto sender = make_sender();
  qos::OgreQarStereoStream s(qos::test::fake_scene_manager(), make_stream_config(7), sender);

  EXPECT_TRUE(sender->prime_enabled());  // IQarRenderSenderAdapter default
  s.set_prime_enabled(false);
  EXPECT_FALSE(sender->prime_enabled());
  EXPECT_EQ(sender->set_prime_enabled_calls(), 1u);

  EXPECT_EQ(sender->tick_ms(), 0.1);  // default
  s.set_tick_ms(4);
  EXPECT_EQ(sender->tick_ms(), 4);
  EXPECT_EQ(sender->set_tick_ms_calls(), 1u);

  // The test double never opens a trace file.
  EXPECT_EQ(s.frame_trace_path(), std::string());
}

TEST(StereoStreamLifecycle, RenderAndSubmitEarlyOuts)
{
  auto sender = make_sender();
  qos::OgreQarStereoStream s(qos::test::fake_scene_manager(), make_stream_config(6), sender);

  // Inactive => InvalidState, and the sender is never queried.
  EXPECT_EQ(s.render_and_submit().status, qos::Status::InvalidState);
  EXPECT_EQ(sender->begin_frame_calls(), 0u);

  // Active but targets not created => InvalidState (no Ogre touched).
  s.set_active(true);
  EXPECT_EQ(s.render_and_submit().status, qos::Status::InvalidState);
  EXPECT_EQ(sender->begin_frame_calls(), 0u);
  EXPECT_FALSE(s.targets_ready());
}

// --- Recording sender records per-eye metadata from the animated source ------

TEST(SenderFrameSource, RecordsPerEyeMetadataMatchingSource)
{
  auto source = std::make_shared<qos::MockQarFrameInfoSource>();
  qts::MockQarRenderSenderAdapter sender(qos::test::make_peer_id(2));
  sender.set_frame_source(source);

  qos::QarFrameInfo out0{};
  ASSERT_TRUE(sender.begin_frame(out0));
  qos::QarFrameInfo out1{};
  ASSERT_TRUE(sender.begin_frame(out1));

  // The sender hands back exactly what the source generates for indices 0, 1.
  const qos::QarFrameInfo s0 = source->frame_at(0);
  const qos::QarFrameInfo s1 = source->frame_at(1);
  EXPECT_FLOAT_EQ(out0.left_eye.pose.position.x, s0.left_eye.pose.position.x);
  EXPECT_FLOAT_EQ(out1.right_eye.pose.position.z, s1.right_eye.pose.position.z);
  EXPECT_NE(out0.left_eye.pose.position.x, out1.left_eye.pose.position.x)
    << "frames should animate between indices";

  // Recorded infos carry the IPD-offset poses + asymmetric per-eye FOV.
  ASSERT_EQ(sender.begin_frame_infos().size(), 2u);
  const qos::QarFrameInfo & rec = sender.begin_frame_infos().front();
  EXPECT_NE(rec.left_eye.pose.position.x, rec.right_eye.pose.position.x);
  EXPECT_NE(rec.left_eye.fov.angle_left, -rec.left_eye.fov.angle_right);

  // Submitting a frame is recorded verbatim (eye labels + layout preserved).
  qos::StereoFrameTextures t;
  t.color_size = QarTextureSize{QAR_PIXEL_FORMAT_R8G8B8A8, 800u, 600u, 1u};
  t.left_color_id = 7u;
  t.right_color_id = 8u;
  const qos::QarOpenGlStereoFrame f =
    qos::assemble_separated_stereo_frame(sender.peer_id(), 0u, t);
  EXPECT_TRUE(sender.submit_opengl_stereo_frame(f));
  ASSERT_EQ(sender.submitted_frames().size(), 1u);
  EXPECT_EQ(sender.submitted_frames().front().views[1].eye, QAR_VIDEO_FRAME_VIEW_EYE_RIGHT);
  EXPECT_EQ(sender.submitted_frames().front().layout, QAR_FRAME_LAYOUT_SEPARATED_TEXTURES);
}
