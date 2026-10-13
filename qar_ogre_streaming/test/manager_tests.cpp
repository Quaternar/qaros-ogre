// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// Manager create/destroy bookkeeping + construction validation. These exercise
// the validation and test-double paths only — no Ogre is initialized (the fake
// scene manager pointer is never dereferenced).

#include <memory>
#include <stdexcept>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming_test_support/immediate_render_thread_executor.hpp"
#include "qar_ogre_streaming_test_support/mock_qar_render_sender_adapter.hpp"
#include "test_helpers.hpp"

namespace qos = qar_ogre_streaming;

namespace
{

// Diagnostics sink that counts emitted messages.
class CountingSink : public qos::OgreQarDiagnosticsSink
{
public:
  void on_message(
    qos::DiagnosticSeverity, const std::string &, const std::string &,
    const std::string &) override
  {
    ++count;
  }
  int count = 0;
};

struct ManagerFixture
{
  std::shared_ptr<qos::test_support::ImmediateRenderThreadExecutor> executor =
    std::make_shared<qos::test_support::ImmediateRenderThreadExecutor>();
  std::shared_ptr<qos::test_support::MockQarRenderSenderFactory> factory =
    std::make_shared<qos::test_support::MockQarRenderSenderFactory>();
  CountingSink sink;

  qos::OgreQarStreamingConfig make_config()
  {
    qos::OgreQarStreamingConfig cfg;
    cfg.scene_manager = qos::test::fake_scene_manager();
    cfg.render_thread_executor = executor;
    cfg.qar_sender_factory = factory;
    cfg.diagnostics = &sink;
    return cfg;
  }
};

}  // namespace

TEST(ManagerConstruction, RejectsNullSceneManager)
{
  ManagerFixture f;
  auto cfg = f.make_config();
  cfg.scene_manager = nullptr;
  EXPECT_THROW(qos::OgreQarStreamingManager{cfg}, std::invalid_argument);
}

TEST(ManagerConstruction, RejectsNullExecutor)
{
  ManagerFixture f;
  auto cfg = f.make_config();
  cfg.render_thread_executor = nullptr;
  EXPECT_THROW(qos::OgreQarStreamingManager{cfg}, std::invalid_argument);
}

TEST(ManagerConstruction, RejectsNullSenderFactory)
{
  ManagerFixture f;
  auto cfg = f.make_config();
  cfg.qar_sender_factory = nullptr;
  EXPECT_THROW(qos::OgreQarStreamingManager{cfg}, std::invalid_argument);
}

TEST(ManagerBookkeeping, CreateDestroyStream)
{
  ManagerFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());

  EXPECT_EQ(mgr.stream_count(), 0u);

  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(7);
  EXPECT_TRUE(mgr.create_stream(s).ok());
  EXPECT_EQ(mgr.stream_count(), 1u);
  EXPECT_TRUE(mgr.has_stream(s.peer_id));
  EXPECT_NE(mgr.get_stream(s.peer_id), nullptr);

  // A sender was requested from the factory for this peer.
  ASSERT_EQ(f.factory->created().size(), 1u);

  // Duplicate create => AlreadyExists.
  EXPECT_EQ(mgr.create_stream(s).status, qos::Status::AlreadyExists);
  EXPECT_EQ(mgr.stream_count(), 1u);

  // Destroy.
  EXPECT_TRUE(mgr.destroy_stream(s.peer_id).ok());
  EXPECT_EQ(mgr.stream_count(), 0u);
  EXPECT_FALSE(mgr.has_stream(s.peer_id));

  // Destroy again => NotFound.
  EXPECT_EQ(mgr.destroy_stream(s.peer_id).status, qos::Status::NotFound);
}

TEST(ManagerBookkeeping, PrimeAndTickLiveSwitchesReachExistingAndFutureStreams)
{
  ManagerFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());

  // Stream created BEFORE the toggle.
  qos::StereoStreamConfig s1;
  s1.peer_id = qos::test::make_peer_id(1);
  ASSERT_TRUE(mgr.create_stream(s1).ok());
  ASSERT_EQ(f.factory->created().size(), 1u);
  // Copy the shared_ptr BY VALUE, not a reference into the vector: create_stream(s2)
  // below triggers created_.push_back(), which can reallocate the vector and
  // invalidate any reference/iterator taken into it beforehand.
  auto sender1 = f.factory->created()[0];
  EXPECT_TRUE(sender1->prime_enabled());
  EXPECT_EQ(sender1->tick_ms(), 0.1);

  mgr.set_prime_enabled(false);
  mgr.set_tick_ms(4);
  EXPECT_FALSE(sender1->prime_enabled());
  EXPECT_EQ(sender1->tick_ms(), 4);

  // Stream created AFTER the toggle starts with the now-current setting, not the
  // library default — a peer added post-toggle must not briefly run pre-toggle.
  qos::StereoStreamConfig s2;
  s2.peer_id = qos::test::make_peer_id(2);
  ASSERT_TRUE(mgr.create_stream(s2).ok());
  ASSERT_EQ(f.factory->created().size(), 2u);
  auto sender2 = f.factory->created()[1];
  EXPECT_FALSE(sender2->prime_enabled());
  EXPECT_EQ(sender2->tick_ms(), 4);

  // Toggling back reaches both existing streams.
  mgr.set_prime_enabled(true);
  EXPECT_TRUE(sender1->prime_enabled());
  EXPECT_TRUE(sender2->prime_enabled());
}

TEST(ManagerBookkeeping, MultiplePeers)
{
  ManagerFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());

  for (uint8_t i = 1; i <= 3; ++i) {
    qos::StereoStreamConfig s;
    s.peer_id = qos::test::make_peer_id(i);
    EXPECT_TRUE(mgr.create_stream(s).ok());
  }
  EXPECT_EQ(mgr.stream_count(), 3u);

  const auto snap = mgr.diagnostics_snapshot();
  EXPECT_EQ(snap.stream_count, 3u);
  EXPECT_EQ(snap.streams.size(), 3u);
}

TEST(ManagerBookkeeping, RejectsInvalidStreamConfig)
{
  ManagerFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());

  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(9);
  s.max_fps = 0;  // invalid
  EXPECT_EQ(mgr.create_stream(s).status, qos::Status::InvalidArgument);
  EXPECT_EQ(mgr.stream_count(), 0u);
}

TEST(ManagerBookkeeping, SenderFactoryFailureSurfaces)
{
  ManagerFixture f;
  f.factory->set_fail_creation(true);
  qos::OgreQarStreamingManager mgr(f.make_config());

  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(5);
  EXPECT_EQ(mgr.create_stream(s).status, qos::Status::InvalidState);
  EXPECT_EQ(mgr.stream_count(), 0u);
}

TEST(ManagerRenderTick, ThreadAffinity)
{
  ManagerFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());

  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(2);
  ASSERT_TRUE(mgr.create_stream(s).ok());
  mgr.get_stream(s.peer_id)->set_active(true);
  mgr.set_streaming_active(true);

  // On the render thread the tick runs the batch. With the default recording sender
  // (no frame source) begin_frame() declines a frame (should_render=false), so
  // the batch completes without touching Ogre and reports Ok. The stream is
  // collected but accounted as a (declined) skip.
  f.executor->set_is_render_thread(true);
  EXPECT_TRUE(mgr.render_tick().ok());
  EXPECT_EQ(mgr.diagnostics_snapshot().streams.at(0).frames_submitted, 0u);
  EXPECT_GE(mgr.diagnostics_snapshot().streams.at(0).frames_skipped, 1u);

  // Off the render thread it must refuse before touching Ogre or the render lock.
  f.executor->set_is_render_thread(false);
  EXPECT_EQ(mgr.render_tick().status, qos::Status::RenderThreadRequired);
}

TEST(ManagerRenderTick, InactiveStreamingIsEmptyTick)
{
  ManagerFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());
  // Not active => Ok with nothing to do.
  EXPECT_TRUE(mgr.render_tick().ok());
}
