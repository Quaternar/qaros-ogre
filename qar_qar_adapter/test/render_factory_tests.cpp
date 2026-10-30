// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The render sender factory on the session owner: how it takes its targets from the owner, what
// attach() asks the Hub for, and what it queues for the producer. The owner runs on the fake
// QarOS library; no test loads the real one and no Ogre or GL object is made.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include "qar_ar_core/common_name.hpp"
#include "qar_qar_adapter/qar_capi_render_sender_adapter.hpp"
#include "session_owner_harness.hpp"

namespace qar_qar_adapter
{

// The friend of the factory that lets a test start a creation that is in flight and answer it.
struct QarCApiSenderFactoryTestAccess
{
  /// Registers a creation for @p peer as create_sender() does and returns the state that the
  /// runtime would hand to the result callback.
  static void * start_pending(QarCApiSenderFactory & factory, const QarPeerId & peer)
  {
    auto pending = std::make_shared<QarCApiSenderFactory::PendingSender>();
    pending->peer_id = peer;
    factory.pending_senders_.push_back(pending);
    return new std::shared_ptr<QarCApiSenderFactory::PendingSender>(pending);
  }

  static void answer(void * user_state, QarRenderSender * stream)
  {
    QarResult status{};
    status.code = stream != nullptr ? QAR_STATUS_SUCCESS : QAR_STATUS_UNCLASSIFIED;
    QarCApiSenderFactory::sender_created_trampoline(status, stream, user_state);
  }
};

}  // namespace qar_qar_adapter

namespace
{

using qar_qar_adapter::QarCApiSenderFactory;
using qar_qar_adapter::QarCApiSenderFactoryTestAccess;
using qar_qar_adapter::TargetInfo;
using qar_qar_adapter::test::Harness;
using qar_qar_adapter::test::make_peer;

TargetInfo target_with(std::uint8_t tag)
{
  TargetInfo info;
  for (std::size_t i = 0; i < sizeof(info.id.data); ++i) {
    info.id.data[i] = tag;
  }
  return info;
}

QarCApiSenderFactory::Config scoped_config()
{
  QarCApiSenderFactory::Config config;
  config.name_scope = qar_ar_core::make_name_scope("test-program", "", "lab-pc");
  return config;
}

std::vector<QarPeerId> drain(QarCApiSenderFactory & factory)
{
  std::vector<QarPeerId> out;
  factory.poll_new_peer_requests(out);
  return out;
}

std::vector<QarPeerId> closed(QarCApiSenderFactory & factory)
{
  std::vector<QarPeerId> out;
  factory.poll_closed_peers(out);
  return out;
}

TEST(RenderFactoryQueue, ATargetThatComesUpIsQueuedOnceAndDrained)
{
  QarCApiSenderFactory factory(nullptr, scoped_config());
  factory.on_target_up(target_with(1));
  factory.on_target_up(target_with(1));
  EXPECT_EQ(drain(factory).size(), 1u);
  EXPECT_TRUE(drain(factory).empty());
  EXPECT_EQ(factory.served_peer_count(), 1u);
}

TEST(RenderFactoryQueue, ATargetThatGoesDownIsListedAsClosedUntilTheProducerForgetsIt)
{
  QarCApiSenderFactory factory(nullptr, scoped_config());
  factory.on_target_up(target_with(1));
  ASSERT_EQ(drain(factory).size(), 1u);
  factory.on_target_down(target_with(1).id, QAR_PEER_PRESENCE_OFFLINE);
  EXPECT_EQ(closed(factory).size(), 1u);
  EXPECT_EQ(closed(factory).size(), 1u) << "listing consumes nothing";
  factory.forget(target_with(1).id);
  EXPECT_TRUE(closed(factory).empty());
  EXPECT_EQ(factory.served_peer_count(), 0u);
}

TEST(RenderFactoryQueue, ATargetThatLeavesBeforeTheProducerAskedForItIsNeverQueued)
{
  QarCApiSenderFactory factory(nullptr, scoped_config());
  factory.on_target_up(target_with(1));
  factory.on_target_down(target_with(1).id, QAR_PEER_PRESENCE_LEAVING);
  EXPECT_TRUE(drain(factory).empty());
}

TEST(RenderFactoryQueue, TargetsBeyondTheCeilingAreRefused)
{
  QarCApiSenderFactory::Config config = scoped_config();
  config.max_peers = 2;
  QarCApiSenderFactory factory(nullptr, config);
  factory.on_target_up(target_with(1));
  factory.on_target_up(target_with(2));
  factory.on_target_up(target_with(3));
  EXPECT_EQ(drain(factory).size(), 2u);
  EXPECT_EQ(factory.served_peer_count(), 2u);
  factory.forget(target_with(1).id);
  factory.on_target_up(target_with(3));
  EXPECT_EQ(drain(factory).size(), 1u) << "a freed place is served again";
}

TEST(RenderFactoryQueue, TheProducerCanQueueAPeerAgainWithoutDuplicates)
{
  QarCApiSenderFactory factory(nullptr, scoped_config());
  factory.on_render_request(target_with(4).id);
  factory.on_render_request(target_with(4).id);
  EXPECT_EQ(drain(factory).size(), 1u);
}

TEST(RenderFactoryQueue, ARequestQueuedAgainForAPeerThatLeftIsDroppedWhenTheProducerForgetsIt)
{
  QarCApiSenderFactory factory(nullptr, scoped_config());
  factory.on_target_up(target_with(1));
  ASSERT_EQ(drain(factory).size(), 1u);
  // The target goes down after the producer polled it and before it queues the peer again,
  // which it does when the sender is not made.
  factory.on_target_down(target_with(1).id, QAR_PEER_PRESENCE_OFFLINE);
  factory.on_render_request(target_with(1).id);
  ASSERT_EQ(closed(factory).size(), 1u);
  factory.forget(target_with(1).id);
  EXPECT_TRUE(drain(factory).empty()) << "the peer is gone: nobody asks for it again";
  EXPECT_TRUE(closed(factory).empty());
  EXPECT_EQ(factory.served_peer_count(), 0u);
}

TEST(RenderFactoryQueue, APeerThatComesBackWithTheSameIdIsServedByItsNextRequest)
{
  QarCApiSenderFactory factory(nullptr, scoped_config());
  factory.on_target_up(target_with(1));
  ASSERT_EQ(drain(factory).size(), 1u);
  factory.on_target_down(target_with(1).id, QAR_PEER_PRESENCE_OFFLINE);
  factory.on_render_request(target_with(1).id);
  factory.forget(target_with(1).id);
  ASSERT_TRUE(drain(factory).empty());
  factory.on_target_up(target_with(1));
  EXPECT_EQ(drain(factory).size(), 1u);
  EXPECT_EQ(factory.served_peer_count(), 1u);
  factory.on_target_down(target_with(1).id, QAR_PEER_PRESENCE_OFFLINE);
  EXPECT_EQ(closed(factory).size(), 1u) << "the returned peer is tracked again";
}

TEST(RenderFactoryAttach, TheVolumeIsGotByItsStableCommonNameWithThePlacementOfTheConfig)
{
  Harness h;
  h.fake().set_initial_peers({make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  ASSERT_TRUE(h.connect_and_run());

  QarCApiSenderFactory::Config config = scoped_config();
  config.app_volume_name = "Scene of the lab";
  config.app_volume_center_y_m = 1.5f;
  config.app_volume_scale = 0.5f;
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), config);
  EXPECT_TRUE(factory->ready());
  EXPECT_FALSE(factory->attached());
  ASSERT_TRUE(factory->attach()) << factory->last_error();
  EXPECT_TRUE(factory->attached());

  const auto requests = h.fake().volume_requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].common_name, qar_ar_core::make_volume_common_name(config.name_scope));
  EXPECT_EQ(requests[0].display_name, "Scene of the lab");
  EXPECT_FLOAT_EQ(requests[0].centre_y, 1.5f);
  EXPECT_FLOAT_EQ(requests[0].width, 10.0f);
  EXPECT_FLOAT_EQ(requests[0].app_scale, 0.5f);

  QarAppVolumeId id{};
  EXPECT_TRUE(factory->app_volume(id));
  qar_ar_core::ScenePlacement placement;
  ASSERT_TRUE(factory->scene_placement(placement));
  EXPECT_TRUE(placement.known);
  EXPECT_FLOAT_EQ(placement.volume_pose.position.y, 1.5f);
  EXPECT_FLOAT_EQ(placement.app_scale, 0.5f);

  // The target the owner already knew is handed to the factory when it attaches.
  EXPECT_EQ(drain(*factory).size(), 1u);
  EXPECT_NE(factory->session(), nullptr);
  EXPECT_TRUE(factory->attach()) << "attaching twice is harmless";
  EXPECT_EQ(h.fake().volume_requests().size(), 1u);
}

TEST(RenderFactoryAttach, ATargetThatComesUpAfterAttachIsQueued)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  EXPECT_TRUE(drain(*factory).empty());
  h.fake().deliver_peer(make_peer(5, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  EXPECT_EQ(drain(*factory).size(), 1u);
  h.fake().deliver_peer(make_peer(5, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_OFFLINE));
  EXPECT_EQ(closed(*factory).size(), 1u);
}

TEST(RenderFactoryAttach, ForgetAsksAgainForATargetThatIsStillHere)
{
  Harness h;
  h.fake().set_initial_peers({make_peer(1, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE)});
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  const auto first = drain(*factory);
  ASSERT_EQ(first.size(), 1u);
  factory->forget(first[0]);
  EXPECT_EQ(drain(*factory).size(), 1u) << "QAR closed the stream; the target is still here";
}

TEST(RenderFactoryAttach, DetachStopsTheFactoryFromHearingTargets)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  factory->detach();
  EXPECT_FALSE(factory->attached());
  h.fake().deliver_peer(make_peer(5, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  EXPECT_TRUE(drain(*factory).empty());
  EXPECT_EQ(factory->create_sender(make_peer(5, QAR_PEER_ROLE_TARGET,
    QAR_PEER_PRESENCE_ONLINE).id), nullptr) << "no stream without a volume";
}

TEST(RenderFactoryAttach, AnOwnerThatIsNotRunningCannotBeAttachedTo)
{
  Harness h;
  h.start();
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  EXPECT_FALSE(factory->ready());
  EXPECT_FALSE(factory->attach());
  EXPECT_NE(factory->last_error().find("not running"), std::string::npos);
  EXPECT_EQ(h.fake().volume_requests().size(), 0u);
  EXPECT_EQ(factory->session(), nullptr);
}

TEST(RenderFactoryAttach, AVolumeTheHubRefusesFailsTheAttach)
{
  Harness h;
  h.fake().set_volume_code(QAR_STATUS_LOGIC_ERROR);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  EXPECT_FALSE(factory->attach());
  EXPECT_FALSE(factory->last_error().empty());
  QarAppVolumeId id{};
  EXPECT_FALSE(factory->app_volume(id));
  EXPECT_FALSE(factory->attached());
}

TEST(RenderFactoryAttach, AFactoryThatNoSharedPointerOwnsCannotRegisterAsAListener)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  QarCApiSenderFactory factory(h.owner_ptr(), scoped_config());
  EXPECT_FALSE(factory.attach());
  EXPECT_NE(factory.last_error().find("shared_ptr"), std::string::npos);
  EXPECT_EQ(h.fake().volume_requests().size(), 0u) << "nothing is asked of the Hub";
}

TEST(RenderFactoryAttach, WithoutAVolumeTheFactoryStillListensButHasNoVolumeToStreamInto)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  QarCApiSenderFactory::Config config = scoped_config();
  config.create_app_volume = false;
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), config);
  ASSERT_TRUE(factory->attach());
  EXPECT_EQ(h.fake().volume_requests().size(), 0u);
  QarAppVolumeId id{};
  EXPECT_FALSE(factory->app_volume(id));
}

// ---------------------------------------------------------------------------------------------
// Lifetime: the factory does not own the owner, and a factory is never released by a QAR thread
// ---------------------------------------------------------------------------------------------

// A factory whose target listener blocks until a test lets it go, and that notes which thread
// destroyed it.
class ObservedFactory : public QarCApiSenderFactory
{
public:
  ObservedFactory(
    std::shared_ptr<qar_qar_adapter::QarSessionOwner> owner, const Config & config,
    std::mutex & gate, std::atomic<bool> & entered, std::thread::id & destroyed_on)
  : QarCApiSenderFactory(std::move(owner), config), gate_(gate), entered_(entered),
    destroyed_on_(destroyed_on)
  {
  }
  ~ObservedFactory() override { destroyed_on_ = std::this_thread::get_id(); }

  void on_target_up(const TargetInfo & target) override
  {
    entered_.store(true);
    {
      std::lock_guard<std::mutex> wait(gate_);
    }
    QarCApiSenderFactory::on_target_up(target);
  }

private:
  std::mutex & gate_;
  std::atomic<bool> & entered_;
  std::thread::id & destroyed_on_;
};

TEST(RenderFactoryLifetime, TheFactoryDoesNotOwnTheOwner)
{
  Harness h;
  h.start();
  std::weak_ptr<qar_qar_adapter::QarSessionOwner> watch = h.owner_ptr();
  const long before = watch.use_count();
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  EXPECT_EQ(watch.use_count(), before) << "a factory holds no reference to the owner";
  factory.reset();
  EXPECT_EQ(watch.use_count(), before);
}

TEST(RenderFactoryLifetime, AFactoryThatOutlivesItsOwnerDetachesWithoutACall)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  ASSERT_TRUE(h.destroy_owner_within());
  EXPECT_FALSE(factory->ready());
  EXPECT_EQ(factory->session(), nullptr);
  factory->detach();
  factory.reset();
}

TEST(RenderFactoryLifetime, DetachWaitsForADeliveryAndTheFactoryDiesOnTheConsumerThread)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  std::mutex gate;
  gate.lock();
  std::atomic<bool> entered{false};
  std::thread::id destroyed_on;
  auto factory = std::make_shared<ObservedFactory>(
    h.owner_ptr(), scoped_config(), gate, entered, destroyed_on);
  ASSERT_TRUE(factory->attach());

  std::thread library_thread([&h] {
      h.fake().deliver_peer(make_peer(5, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
    });
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!entered.load() && std::chrono::steady_clock::now() < end) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(entered.load()) << "the delivery is inside the factory";

  // The consumer detaches and lets go of the factory while the delivery is still running.
  std::thread::id consumer;
  auto release = std::async(std::launch::async, [&factory, &consumer] {
      consumer = std::this_thread::get_id();
      factory->detach();
      factory.reset();
    });
  EXPECT_EQ(release.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout)
    << "detach() waits for the delivery";

  gate.unlock();
  library_thread.join();
  ASSERT_EQ(release.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_NE(destroyed_on, std::thread::id()) << "the factory was destroyed";
  EXPECT_EQ(destroyed_on, consumer) << "a QAR thread released the last reference";
  EXPECT_TRUE(h.destroy_owner_within()) << "the owner still ends on the consumer thread";
}

TEST(RenderFactoryPending, AStreamThatArrivesAfterDetachIsDestroyed)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  void * user_state = QarCApiSenderFactoryTestAccess::start_pending(
    *factory, target_with(5).id);

  factory->detach();
  EXPECT_EQ(h.fake().render_streams_destroyed(), 0) << "nothing arrived yet";
  auto * stream = reinterpret_cast<QarRenderSender *>(&user_state);
  QarCApiSenderFactoryTestAccess::answer(user_state, stream);
  EXPECT_EQ(h.fake().render_streams_destroyed(), 1) << "the late stream was released";
}

TEST(RenderFactoryPending, AStreamThatArrivedBeforeDetachButWasNeverPolledIsDestroyedByDetach)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  void * user_state = QarCApiSenderFactoryTestAccess::start_pending(
    *factory, target_with(5).id);
  auto * stream = reinterpret_cast<QarRenderSender *>(&user_state);
  QarCApiSenderFactoryTestAccess::answer(user_state, stream);
  EXPECT_EQ(h.fake().render_streams_destroyed(), 0);

  factory->detach();
  EXPECT_EQ(h.fake().render_streams_destroyed(), 1);
  factory->detach();
  EXPECT_EQ(h.fake().render_streams_destroyed(), 1) << "once only";
}

TEST(RenderFactoryPending, AFailedCreationAfterDetachDestroysNothing)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  void * user_state = QarCApiSenderFactoryTestAccess::start_pending(
    *factory, target_with(5).id);
  factory->detach();
  QarCApiSenderFactoryTestAccess::answer(user_state, nullptr);
  EXPECT_EQ(h.fake().render_streams_destroyed(), 0);
}

// ---------------------------------------------------------------------------------------------
// The scene placement is a cache: no QarOS call on the thread that asks for it
// ---------------------------------------------------------------------------------------------

using namespace std::chrono_literals;

void slow_volume_queries(Harness & h)
{
  h.fake().set_call_hook([](const std::string & name) {
      if (name == "query_app_volumes_count" || name == "query_app_volumes") {
        std::this_thread::sleep_for(2000ms);
      }
    });
}

bool wait_for_attach(QarCApiSenderFactory & factory, std::chrono::milliseconds timeout = 20s)
{
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (factory.attach_state() == QarCApiSenderFactory::AttachState::kPending) {
    if (std::chrono::steady_clock::now() >= end) {
      return false;
    }
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

TEST(RenderFactoryAsync, ScenePlacementReturnsAtOnceWhileQueriesTakeTwoSeconds)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  slow_volume_queries(h);
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach()) << factory->last_error();

  for (int i = 0; i < 20; ++i) {
    qar_ar_core::ScenePlacement placement;
    bool known = false;
    const auto start = std::chrono::steady_clock::now();
    known = factory->scene_placement(placement);
    const double took_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
    ASSERT_LT(took_ms, 50.0) << "call " << i << " of scene_placement()";
    ASSERT_TRUE(known) << "call " << i;
    EXPECT_TRUE(placement.known);
  }
  const auto test_thread = std::this_thread::get_id();
  int queries = 0;
  for (const auto & call : h.fake().calls()) {
    if (call.name == "query_app_volumes_count" || call.name == "query_app_volumes") {
      ++queries;
      EXPECT_NE(call.thread, test_thread) << call.name << " ran on the thread that asked";
    }
  }
  EXPECT_GE(queries, 2) << "the attach read the placement once";
}

TEST(RenderFactoryAsync, BeginAttachReturnsAtOnce)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_call_hook([](const std::string & name) {
      if (name == "app_volumes_get_or_create") {
        std::this_thread::sleep_for(2000ms);
      }
    });
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  EXPECT_EQ(factory->attach_state(), QarCApiSenderFactory::AttachState::kIdle);

  bool begun = false;
  const auto start = std::chrono::steady_clock::now();
  begun = factory->begin_attach();
  const double took_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start).count();
  ASSERT_TRUE(begun) << factory->last_error();
  EXPECT_LT(took_ms, 50.0);
  EXPECT_EQ(factory->attach_state(), QarCApiSenderFactory::AttachState::kPending);
  EXPECT_FALSE(factory->attached());

  ASSERT_TRUE(wait_for_attach(*factory, 10s));
  EXPECT_EQ(factory->attach_state(), QarCApiSenderFactory::AttachState::kAttached);
  EXPECT_TRUE(factory->attached());
  const int created = h.fake().first_index("app_volumes_get_or_create");
  ASSERT_GE(created, 0);
  EXPECT_EQ(h.fake().calls()[static_cast<std::size_t>(created)].thread, h.owner_thread());
  QarAppVolumeId id{};
  EXPECT_TRUE(factory->app_volume(id));
}

bool wait_until(const std::function<bool()> & predicate, std::chrono::milliseconds timeout = 10s)
{
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= end) {
      return false;
    }
    std::this_thread::sleep_for(2ms);
  }
  return true;
}

QarPose pose_at(float x, float y, float z)
{
  QarPose pose = qar_pose_default();
  pose.position.x = x;
  pose.position.y = y;
  pose.position.z = z;
  return pose;
}

TEST(RenderFactoryAsync, AFailedAttachCanBeStartedAgain)
{
  Harness h;
  h.fake().set_volume_code(QAR_STATUS_LOGIC_ERROR);
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->begin_attach());
  ASSERT_TRUE(wait_for_attach(*factory));
  EXPECT_EQ(factory->attach_state(), QarCApiSenderFactory::AttachState::kFailed);
  EXPECT_FALSE(factory->last_error().empty());
  EXPECT_FALSE(factory->attached());

  h.fake().set_volume_code(0);
  ASSERT_TRUE(factory->begin_attach()) << "kFailed retries";
  ASSERT_TRUE(wait_for_attach(*factory));
  EXPECT_EQ(factory->attach_state(), QarCApiSenderFactory::AttachState::kAttached);
  EXPECT_TRUE(factory->last_error().empty());
}

TEST(RenderFactoryAsync, BeginAttachRefusesAnOwnerThatIsNotRunning)
{
  Harness h;
  h.start();
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  EXPECT_FALSE(factory->begin_attach());
  EXPECT_NE(factory->last_error().find("not running"), std::string::npos);
  EXPECT_EQ(factory->attach_state(), QarCApiSenderFactory::AttachState::kIdle);
}

TEST(RenderFactoryAsync, TheCacheFollowsTheUpdatesOfTheRuntime)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  EXPECT_EQ(h.fake().volume_subscriptions(), 1);
  const int queries_after_attach = h.fake().count("query_app_volumes_count");
  EXPECT_EQ(queries_after_attach, 1) << "the attach reads the placement with one query";

  h.fake().set_volume_placement(pose_at(1.0f, 2.0f, 3.0f), pose_at(0.5f, 0.0f, 0.0f), 2.5f);
  h.fake().deliver_volume_update();
  qar_ar_core::ScenePlacement placement;
  ASSERT_TRUE(factory->scene_placement(placement));
  EXPECT_FLOAT_EQ(placement.volume_pose.position.x, 1.0f);
  EXPECT_FLOAT_EQ(placement.volume_pose.position.z, 3.0f);
  EXPECT_FLOAT_EQ(placement.app_pose.position.x, 0.5f);
  EXPECT_FLOAT_EQ(placement.app_scale, 2.5f);
  EXPECT_EQ(h.fake().count("query_app_volumes_count"), queries_after_attach)
    << "an update costs no query";
}

TEST(RenderFactoryAsync, AnOldCacheIsRefreshedWithTheGettersOnTheOwnersThread)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  h.fake().set_volume_placement(pose_at(4.0f, 0.0f, 0.0f), pose_at(0.0f, 0.0f, 0.0f), 0.25f);

  // Older than a second: the next read asks the owner's thread for a refresh and returns the
  // value it has.
  std::this_thread::sleep_for(1100ms);
  qar_ar_core::ScenePlacement placement;
  ASSERT_TRUE(factory->scene_placement(placement));
  EXPECT_FLOAT_EQ(placement.app_scale, 1.0f) << "the value of the attach, at once";
  ASSERT_TRUE(wait_until([&] {
        qar_ar_core::ScenePlacement now_seen;
        return factory->scene_placement(now_seen) && now_seen.app_scale == 0.25f;
      })) << "the refresh arrives";
  const auto calls = h.fake().calls();
  const int latest = h.fake().first_index("app_volume_get_latest_pose");
  ASSERT_GE(latest, 0);
  EXPECT_EQ(calls[static_cast<std::size_t>(latest)].thread, h.owner_thread());
  EXPECT_EQ(h.fake().count("query_app_volumes_count"), 1) << "no query: the getters answered";
}

TEST(RenderFactoryAsync, FailingGettersKeepTheCacheWhileTheSubscriptionRuns)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  h.fake().set_volume_latest_code(QAR_STATUS_LOGIC_ERROR);
  std::this_thread::sleep_for(1100ms);
  qar_ar_core::ScenePlacement placement;
  ASSERT_TRUE(factory->scene_placement(placement));
  ASSERT_TRUE(wait_until([&h] { return h.fake().count("app_volume_get_latest_pose") >= 1; }));
  std::this_thread::sleep_for(100ms);
  ASSERT_TRUE(factory->scene_placement(placement)) << "the value stays known";
  EXPECT_EQ(h.fake().count("query_app_volumes_count"), 1) << "the subscription runs: no query";
}

TEST(RenderFactoryAsync, AFirstReadThatFailedAtTheAttachIsTriedAgain)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_call_failure("query_app_volumes_count", QAR_STATUS_LOGIC_ERROR);
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach()) << "the attach does not fail because the placement is unknown";
  // The placement the getters answer with is set after the attach (which creates the volume)
  // and before the first scene_placement() call: that call posts the read to the owner's
  // thread, which may run at once and would otherwise store the placement of the new volume.
  h.fake().set_volume_placement(pose_at(1.0f, 0.5f, 2.0f), pose_at(0.0f, 0.0f, 0.0f), 0.5f);
  qar_ar_core::ScenePlacement placement;
  EXPECT_FALSE(factory->scene_placement(placement)) << "nothing is known after the failed read";

  // The call asks for a read on the owner's thread; the cache fills once a read works.
  ASSERT_TRUE(wait_until([&] { return factory->scene_placement(placement); }))
    << "scene_placement() never got a placement";
  EXPECT_FLOAT_EQ(placement.app_scale, 0.5f);
  EXPECT_FLOAT_EQ(placement.volume_pose.position.z, 2.0f);
  const int latest = h.fake().first_index("app_volume_get_latest_pose");
  ASSERT_GE(latest, 0);
  EXPECT_EQ(h.fake().calls()[static_cast<std::size_t>(latest)].thread, h.owner_thread());
}

TEST(RenderFactoryAsync, AFirstReadThatFailedIsReadByAQueryWhenTheGettersFailToo)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  h.fake().set_call_failure("query_app_volumes_count", QAR_STATUS_LOGIC_ERROR);
  h.fake().set_volume_latest_code(QAR_STATUS_LOGIC_ERROR);
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  h.fake().set_call_failure("query_app_volumes_count", 0);
  h.fake().set_volume_placement(pose_at(3.0f, 0.0f, 0.0f), pose_at(0.0f, 0.0f, 0.0f), 0.75f);

  qar_ar_core::ScenePlacement placement;
  ASSERT_TRUE(wait_until([&] { return factory->scene_placement(placement); }))
    << "the query read the placement";
  EXPECT_FLOAT_EQ(placement.app_scale, 0.75f);
  EXPECT_FLOAT_EQ(placement.volume_pose.position.x, 3.0f);
}

TEST(RenderFactoryAsync, ARefreshThatChangesThePlacementIsLoggedOncePerFiveSeconds)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  qar_ar_core::ScenePlacement placement;
  ASSERT_TRUE(factory->scene_placement(placement));

  testing::internal::CaptureStderr();
  h.fake().set_volume_placement(pose_at(4.0f, 0.0f, 0.0f), pose_at(0.0f, 0.0f, 0.0f), 0.25f);
  std::this_thread::sleep_for(1100ms);
  ASSERT_TRUE(factory->scene_placement(placement));
  ASSERT_TRUE(wait_until([&] {
        return factory->scene_placement(placement) && placement.app_scale == 0.25f;
      }));
  // Another change inside the five seconds: stored, not logged again.
  h.fake().set_volume_placement(pose_at(5.0f, 0.0f, 0.0f), pose_at(0.0f, 0.0f, 0.0f), 0.2f);
  std::this_thread::sleep_for(1100ms);
  ASSERT_TRUE(factory->scene_placement(placement));
  ASSERT_TRUE(wait_until([&] {
        return factory->scene_placement(placement) && placement.app_scale == 0.2f;
      }));
  const std::string log = testing::internal::GetCapturedStderr();

  const std::string marker = "placement changed (qar_app_volume_get_latest_pose";
  const std::size_t first = log.find(marker);
  ASSERT_NE(first, std::string::npos) << log;
  EXPECT_EQ(log.find(marker, first + 1), std::string::npos) << "rate-limited: one line\n" << log;
  EXPECT_NE(log.find("app_scale 1.000 -> 0.250", first), std::string::npos) << log;
  EXPECT_NE(log.find("(4.000, 0.000, 0.000)", first), std::string::npos)
    << "the volume position after the change";
}

TEST(RenderFactoryAsync, DetachEndsThePlacementSubscriptionOnTheOwnersThread)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  ASSERT_EQ(h.fake().volume_subscriptions(), 1);
  const int tokens = h.fake().live_tokens();

  factory->detach();
  EXPECT_FALSE(factory->attached());
  EXPECT_EQ(factory->attach_state(), QarCApiSenderFactory::AttachState::kIdle);
  qar_ar_core::ScenePlacement placement;
  EXPECT_FALSE(factory->scene_placement(placement)) << "nothing is known after a detach";
  ASSERT_TRUE(wait_until([&h] { return h.fake().volume_subscriptions() == 0; }));
  EXPECT_EQ(h.fake().live_tokens(), tokens - 1) << "the token of the subscription is released";
  const int cancel = h.fake().last_index("cancel_token_cancel");
  ASSERT_GE(cancel, 0);
  EXPECT_EQ(h.fake().calls()[static_cast<std::size_t>(cancel)].thread, h.owner_thread());
}

TEST(RenderFactoryAsync, ADetachBeforeTheAttachRanLeavesNothingBehind)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  // The attach waits behind a task that holds the owner's thread.
  std::atomic<bool> release{false};
  std::atomic<bool> held{false};
  ASSERT_TRUE(h.owner().post_session_task("hold", [&](QarSession *) {
        held.store(true);
        while (!release.load()) {
          std::this_thread::sleep_for(1ms);
        }
      }));
  ASSERT_TRUE(wait_until([&held] { return held.load(); }));
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->begin_attach());
  factory->detach();
  release.store(true);
  ASSERT_TRUE(h.wait_for([&h] { return h.fake().count("session_get_my_spec") >= 1; }));
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(h.fake().count("app_volumes_get_or_create"), 0) << "the retired attach does nothing";
  EXPECT_EQ(h.fake().volume_subscriptions(), 0);
  EXPECT_FALSE(factory->attached());
  h.fake().deliver_peer(make_peer(5, QAR_PEER_ROLE_TARGET, QAR_PEER_PRESENCE_ONLINE));
  EXPECT_TRUE(drain(*factory).empty()) << "the listener was not registered";
}

TEST(RenderFactoryAsync, TheWorldAnchorIsQueuedAndItsOutcomeComesBack)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  h.fake().set_call_hook([](const std::string & name) {
      if (name == "app_volumes_change_app_world_anchor") {
        std::this_thread::sleep_for(2000ms);
      }
    });

  std::string error;
  EXPECT_FALSE(factory->take_geo_anchor_result(error)) << "nothing yet";
  std::string queued;
  const auto start = std::chrono::steady_clock::now();
  queued = factory->set_geo_anchor_wgs84(50.0, 14.0, 300.0);
  const double took_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start).count();
  EXPECT_TRUE(queued.empty()) << queued;
  EXPECT_LT(took_ms, 50.0);
  EXPECT_FALSE(factory->has_geo_anchor());
  ASSERT_TRUE(wait_until([&] { return factory->take_geo_anchor_result(error); }, 10s));
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_TRUE(factory->has_geo_anchor());
  const int call = h.fake().first_index("app_volumes_change_app_world_anchor");
  ASSERT_GE(call, 0);
  EXPECT_EQ(h.fake().calls()[static_cast<std::size_t>(call)].thread, h.owner_thread());

  // A refusal by the runtime arrives as the outcome of the call.
  h.fake().set_call_failure("app_volumes_change_app_world_anchor", QAR_STATUS_LOGIC_ERROR);
  EXPECT_TRUE(factory->set_geo_anchor_wgs84(50.1, 14.1, 300.0).empty());
  ASSERT_TRUE(wait_until([&] { return factory->take_geo_anchor_result(error); }, 10s));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(factory->has_geo_anchor());

  EXPECT_TRUE(factory->clear_geo_anchor().empty());
  ASSERT_TRUE(wait_until([&] { return factory->take_geo_anchor_result(error); }));
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(h.fake().count("app_volumes_clear_app_world_anchor"), 1);
}

TEST(RenderFactoryAsync, ANewFixReplacesOneThatHasNotRunYet)
{
  Harness h;
  h.start();
  ASSERT_TRUE(h.connect_and_run());
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  ASSERT_TRUE(factory->attach());
  std::atomic<bool> release{false};
  std::atomic<bool> held{false};
  ASSERT_TRUE(h.owner().post_session_task("hold", [&](QarSession *) {
        held.store(true);
        while (!release.load()) {
          std::this_thread::sleep_for(1ms);
        }
      }));
  ASSERT_TRUE(wait_until([&held] { return held.load(); }));
  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(factory->set_geo_anchor_wgs84(50.0 + i, 14.0, 300.0).empty());
  }
  release.store(true);
  std::string error;
  ASSERT_TRUE(wait_until([&] { return factory->take_geo_anchor_result(error); }));
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(h.fake().count("app_volumes_change_app_world_anchor"), 1)
    << "five fixes while the thread was busy make one call";
}

TEST(RenderFactoryAsync, AnchorRequestsAreRefusedWithoutASessionOrAVolume)
{
  Harness h;
  h.start();
  auto factory = std::make_shared<QarCApiSenderFactory>(h.owner_ptr(), scoped_config());
  EXPECT_NE(factory->set_geo_anchor_wgs84(50.0, 14.0, 300.0), "");
  EXPECT_NE(factory->set_geo_anchor_wgs84(500.0, 14.0, 300.0), "") << "a fix out of range";
  ASSERT_TRUE(h.connect_and_run());
  EXPECT_NE(factory->set_geo_anchor_wgs84(50.0, 14.0, 300.0), "") << "no volume before the attach";
  EXPECT_NE(factory->clear_geo_anchor(), "");
  EXPECT_EQ(h.fake().count("app_volumes_change_app_world_anchor"), 0);
}

}  // namespace
