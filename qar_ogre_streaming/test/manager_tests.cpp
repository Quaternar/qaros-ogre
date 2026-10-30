// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Manager create/destroy bookkeeping + construction validation. These exercise
// the validation and test-double paths only — no Ogre is initialized (the fake
// scene manager pointer is never dereferenced).

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "qar_ogre_streaming/ogre_qar_streaming_manager.hpp"
#include "qar_ogre_streaming/ogre_qar_stereo_stream.hpp"
#include "qar_ogre_streaming_test_support/immediate_render_thread_executor.hpp"
#include "qar_ogre_streaming_test_support/manual_clock.hpp"
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

// Diagnostics sink that keeps every line.
class RecordingSink : public qos::OgreQarDiagnosticsSink
{
public:
  struct Line
  {
    qos::DiagnosticSeverity severity;
    std::string message;
    std::string peer;
  };

  void on_message(
    qos::DiagnosticSeverity severity, const std::string &, const std::string & message,
    const std::string & peer) override
  {
    lines.push_back({severity, message, peer});
  }

  // The lines about the wait for a sender (the start line and the end line).
  std::vector<Line> sender_lines() const
  {
    std::vector<Line> out;
    for (const Line & line : lines) {
      if (line.message.find("sender factory returned") != std::string::npos ||
        line.message.find("sender wait") != std::string::npos)
      {
        out.push_back(line);
      }
    }
    return out;
  }

  std::vector<Line> lines;
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

namespace
{

struct WaitFixture
{
  std::shared_ptr<qos::test_support::ImmediateRenderThreadExecutor> executor =
    std::make_shared<qos::test_support::ImmediateRenderThreadExecutor>();
  std::shared_ptr<qos::test_support::MockQarRenderSenderFactory> factory =
    std::make_shared<qos::test_support::MockQarRenderSenderFactory>();
  std::shared_ptr<qos::test_support::ManualClock> clock =
    std::make_shared<qos::test_support::ManualClock>(10.0);
  RecordingSink sink;

  WaitFixture() { factory->set_fail_creation(true); }

  qos::OgreQarStreamingConfig make_config()
  {
    qos::OgreQarStreamingConfig cfg;
    cfg.scene_manager = qos::test::fake_scene_manager();
    cfg.render_thread_executor = executor;
    cfg.qar_sender_factory = factory;
    cfg.diagnostics = &sink;
    cfg.clock = clock;
    return cfg;
  }
};

bool contains(const std::string & text, const std::string & part)
{
  return text.find(part) != std::string::npos;
}

}  // namespace

TEST(ManagerSenderWait, ManyNullsThenASenderAreTwoLines)
{
  WaitFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());
  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(3);

  for (int i = 0; i < 5; ++i) {
    // Every null keeps its result and its last error; only the log is quiet.
    const qos::Result r = mgr.create_stream(s);
    EXPECT_EQ(r.status, qos::Status::InvalidState);
    EXPECT_EQ(r.message, "sender factory returned null for peer");
    EXPECT_EQ(mgr.diagnostics_snapshot().last_error, "sender factory returned null for peer");
    f.clock->advance(0.05);
  }
  ASSERT_EQ(f.sink.sender_lines().size(), 1u);
  const RecordingSink::Line start = f.sink.sender_lines()[0];
  EXPECT_EQ(start.severity, qos::DiagnosticSeverity::Error);
  EXPECT_EQ(start.message.rfind("sender factory returned null for peer", 0), 0u);
  EXPECT_EQ(start.peer, qos::peer_id_to_hex(s.peer_id));

  f.factory->set_fail_creation(false);
  EXPECT_TRUE(mgr.create_stream(s).ok());
  const auto lines = f.sink.sender_lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1].severity, qos::DiagnosticSeverity::Info);
  EXPECT_TRUE(contains(lines[1].message, "after 5 null results in 250 ms")) << lines[1].message;
  EXPECT_EQ(lines[1].peer, qos::peer_id_to_hex(s.peer_id));

  // The wait is over: a duplicate create adds nothing of this kind.
  EXPECT_EQ(mgr.create_stream(s).status, qos::Status::AlreadyExists);
  EXPECT_EQ(f.sink.sender_lines().size(), 2u);
}

TEST(ManagerSenderWait, ASingleNullThenASenderCountsOne)
{
  WaitFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());
  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(4);
  EXPECT_EQ(mgr.create_stream(s).status, qos::Status::InvalidState);
  f.factory->set_fail_creation(false);
  EXPECT_TRUE(mgr.create_stream(s).ok());
  const auto lines = f.sink.sender_lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_TRUE(contains(lines[1].message, "after 1 null result in 0 ms")) << lines[1].message;
}

TEST(ManagerSenderWait, ANewWaitAfterAnEndStartsANewCount)
{
  WaitFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());
  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(6);
  for (int i = 0; i < 4; ++i) {
    mgr.create_stream(s);
  }
  f.factory->set_fail_creation(false);
  ASSERT_TRUE(mgr.create_stream(s).ok());
  ASSERT_TRUE(mgr.destroy_stream(s.peer_id).ok());
  ASSERT_EQ(f.sink.sender_lines().size(), 2u);

  f.factory->set_fail_creation(true);
  mgr.create_stream(s);
  mgr.create_stream(s);
  f.factory->set_fail_creation(false);
  ASSERT_TRUE(mgr.create_stream(s).ok());
  const auto lines = f.sink.sender_lines();
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[2].severity, qos::DiagnosticSeverity::Error);
  EXPECT_TRUE(contains(lines[3].message, "after 2 null results")) << lines[3].message;
}

TEST(ManagerSenderWait, TwoPeersWaitIndependently)
{
  WaitFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());
  qos::StereoStreamConfig a;
  a.peer_id = qos::test::make_peer_id(1);
  qos::StereoStreamConfig b;
  b.peer_id = qos::test::make_peer_id(2);
  mgr.create_stream(a);
  mgr.create_stream(b);
  mgr.create_stream(a);
  EXPECT_EQ(f.sink.sender_lines().size(), 2u);  // one start line for each peer
  f.factory->set_fail_creation(false);
  ASSERT_TRUE(mgr.create_stream(a).ok());
  const auto lines = f.sink.sender_lines();
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_TRUE(contains(lines[2].message, "after 2 null results")) << lines[2].message;
  EXPECT_EQ(lines[2].peer, qos::peer_id_to_hex(a.peer_id));
}

TEST(ManagerSenderWait, ThePeerRemovedEndsTheWaitWithAWarning)
{
  WaitFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());
  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(8);
  mgr.create_stream(s);
  mgr.create_stream(s);
  mgr.create_stream(s);
  f.clock->advance(1.5);
  EXPECT_EQ(mgr.destroy_stream(s.peer_id).status, qos::Status::NotFound);
  const auto lines = f.sink.sender_lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1].severity, qos::DiagnosticSeverity::Warning);
  EXPECT_TRUE(contains(lines[1].message, "ended without a sender")) << lines[1].message;
  EXPECT_TRUE(contains(lines[1].message, "(stream destroyed)")) << lines[1].message;
  EXPECT_TRUE(contains(lines[1].message, "after 3 null results in 1500 ms")) << lines[1].message;

  // A second removal finds no wait and says nothing more.
  EXPECT_EQ(mgr.destroy_stream(s.peer_id).status, qos::Status::NotFound);
  EXPECT_EQ(f.sink.sender_lines().size(), 2u);
}

TEST(ManagerSenderWait, ANewSenderFactoryEndsTheWaitWithAWarning)
{
  WaitFixture f;
  qos::OgreQarStreamingManager mgr(f.make_config());
  qos::StereoStreamConfig s;
  s.peer_id = qos::test::make_peer_id(9);
  mgr.create_stream(s);
  mgr.create_stream(s);
  mgr.set_sender_factory(std::make_shared<qos::test_support::MockQarRenderSenderFactory>());
  const auto lines = f.sink.sender_lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1].severity, qos::DiagnosticSeverity::Warning);
  EXPECT_TRUE(contains(lines[1].message, "(sender factory replaced)")) << lines[1].message;
  EXPECT_TRUE(contains(lines[1].message, "after 2 null results")) << lines[1].message;
}

TEST(ManagerSenderWait, TheManagerDestroyedDropsTheWaitWithoutCallingTheSink)
{
  WaitFixture f;  // the sink outlives the manager, so a call from the destructor would be seen
  {
    qos::OgreQarStreamingManager mgr(f.make_config());
    qos::StereoStreamConfig s;
    s.peer_id = qos::test::make_peer_id(10);
    for (int i = 0; i < 7; ++i) {
      mgr.create_stream(s);
    }
    EXPECT_EQ(f.sink.sender_lines().size(), 1u);
    f.sink.lines.clear();  // from here on, any line comes from the destructor
  }
  EXPECT_TRUE(f.sink.lines.empty());
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
