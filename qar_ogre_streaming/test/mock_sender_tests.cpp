// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "qar_ogre_streaming_test_support/immediate_render_thread_executor.hpp"
#include "qar_ogre_streaming_test_support/mock_qar_render_sender_adapter.hpp"
#include "test_helpers.hpp"

namespace qos = qar_ogre_streaming;
namespace qts = qar_ogre_streaming::test_support;

TEST(MockSender, RecordsBeginFrameAndReturnsScriptedInfo)
{
  qts::MockQarRenderSenderAdapter sender(qos::test::make_peer_id(3));

  qos::QarFrameInfo scripted;
  scripted.frame_index = 42;
  scripted.should_render = true;
  scripted.left_eye.fov.angle_left = -0.5f;
  scripted.right_eye.fov.angle_right = 0.5f;
  sender.set_next_frame_info(scripted);

  qos::QarFrameInfo out{};
  EXPECT_TRUE(sender.begin_frame(out));
  EXPECT_EQ(out.frame_index, 42u);
  EXPECT_TRUE(out.should_render);
  EXPECT_FLOAT_EQ(out.left_eye.fov.angle_left, -0.5f);

  EXPECT_EQ(sender.begin_frame_calls(), 1u);
  ASSERT_EQ(sender.begin_frame_infos().size(), 1u);
  EXPECT_EQ(sender.begin_frame_infos().front().frame_index, 42u);
}

TEST(MockSender, ScriptedBeginFrameResult)
{
  qts::MockQarRenderSenderAdapter sender;
  sender.set_begin_frame_result(false);
  qos::QarFrameInfo out{};
  EXPECT_FALSE(sender.begin_frame(out));
  EXPECT_EQ(sender.begin_frame_calls(), 1u);
}

TEST(MockSender, RecordsSubmittedFramesWithArguments)
{
  qts::MockQarRenderSenderAdapter sender;

  qos::QarOpenGlStereoFrame frame;
  frame.peer_id = qos::test::make_peer_id(8);
  frame.frame_index = 5;
  frame.left_color.texture_id = 111;
  frame.right_color.texture_id = 222;
  frame.layout = QAR_FRAME_LAYOUT_SEPARATED_TEXTURES;

  EXPECT_TRUE(sender.submit_opengl_stereo_frame(frame));
  EXPECT_EQ(sender.submit_calls(), 1u);
  ASSERT_EQ(sender.submitted_frames().size(), 1u);
  const auto & rec = sender.submitted_frames().front();
  EXPECT_EQ(rec.frame_index, 5u);
  EXPECT_EQ(rec.left_color.texture_id, 111u);
  EXPECT_EQ(rec.right_color.texture_id, 222u);
  EXPECT_EQ(rec.layout, QAR_FRAME_LAYOUT_SEPARATED_TEXTURES);
}

TEST(MockSender, ScriptedSubmitResult)
{
  qts::MockQarRenderSenderAdapter sender;
  sender.set_submit_result(false);
  qos::QarOpenGlStereoFrame frame;
  EXPECT_FALSE(sender.submit_opengl_stereo_frame(frame));
  EXPECT_EQ(sender.submit_calls(), 1u);
}

TEST(MockFactory, CreatesAndTracksAdapters)
{
  qts::MockQarRenderSenderFactory factory;
  auto a = factory.create_sender(qos::test::make_peer_id(1));
  auto b = factory.create_sender(qos::test::make_peer_id(2));
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(factory.created().size(), 2u);

  factory.set_fail_creation(true);
  EXPECT_EQ(factory.create_sender(qos::test::make_peer_id(3)), nullptr);
  EXPECT_EQ(factory.created().size(), 2u);
}

TEST(ImmediateExecutor, RunsInlineAndReportsRenderThread)
{
  qts::ImmediateRenderThreadExecutor exec;
  EXPECT_TRUE(exec.is_render_thread());

  bool ran = false;
  exec.post_to_render_thread([&ran]() { ran = true; });
  EXPECT_TRUE(ran);
  EXPECT_EQ(exec.posted_count(), 1u);

  exec.set_is_render_thread(false);
  EXPECT_FALSE(exec.is_render_thread());
}
