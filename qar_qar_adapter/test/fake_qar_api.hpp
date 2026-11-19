// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// A stand-in for the QarOS library, for the tests of the session owner. The QarOS C header
// resolves every call through the function tables g_qar_*_api (dynamic loading); this fake fills
// the entries the owner can reach with functions of its own, so no test loads the real library.
//
// What it imitates: the asynchronous calls fire their result callbacks exactly once on a thread
// of their own (also on cancellation), cancelling or destroying a token waits for a running
// callback of its subscription, and a peer spec handed to a callback is borrowed while one
// returned by a query is owned. Outcomes are scripted by the test.
//
// Outages: set_hub_gone() makes every call into the session fail or wait, the frame calls of the
// render sender and the video sender are slowed or blocked on their own (set_frame_delay_ms,
// set_frame_block), a render sender creation can answer late or never, and a delivery to a
// subscription can run on a thread the test does not wait for. Every blocking mode is released by
// the test (release_pending, set_hub_gone(kNo), set_frame_block(false)) or by the fake's end.

#ifndef QAR_QAR_ADAPTER__TEST__FAKE_QAR_API_HPP_
#define QAR_QAR_ADAPTER__TEST__FAKE_QAR_API_HPP_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_qar_adapter/qar_session_owner.hpp"

namespace qar_qar_adapter
{
namespace test
{

/// One peer of the fake session.
struct FakePeer
{
  QarPeerId id{};
  QarPeerRole role = QAR_PEER_ROLE_TARGET;
  QarPeerPresence presence = QAR_PEER_PRESENCE_ONLINE;
  QarDeviceCategory category = QAR_DEVICE_CATEGORY_HEADSET;
  std::string device_id = "MetaQuest3";
  std::string device_family = "MetaQuest";
  std::string device_name = "Meta Quest 3";
  std::string display_name = "quest";
};

/// A peer whose id is made of @p tag.
FakePeer make_peer(std::uint8_t tag, QarPeerRole role, QarPeerPresence presence);

/// The id the fake gives this program's own peer.
QarPeerId fake_self_id();

/// What an asynchronous call does.
struct Outcome
{
  int code = 0;                 ///< QarStatusCode of the result; 0 is success.
  bool wait_for_release = false;///< Waits for release_pending() or for the token to be cancelled.
  int delay_ms = 0;             ///< Waits this long (or until cancelled) before it answers.
  bool progress = false;        ///< Reports "Waiting for approval in QAROS hub" first.
  /// Nonzero: the call itself returns this error, yet the result callback still fires later with
  /// the outcome above (a library that reports the error and keeps the callback).
  int sync_error_code = 0;
  /// Render sender creation only: whether cancelling the token makes the callback fire (with an
  /// error and no sender). False: the creation goes on waiting for release_pending() or its delay,
  /// and then answers as scripted, also when its token was cancelled meanwhile.
  bool fire_on_cancel = true;
};

/// How the fake Hub answers session calls.
enum class HubGone
{
  kNo,       ///< The Hub is there.
  kFailAll,  ///< Every session call fails at once with the scripted code.
  kBlockAll  ///< Every session call waits until released, then fails with the scripted code.
};

/// What the next qar_render_sender_try_begin_frame answers.
struct RenderFrameScript
{
  bool ready = false;   ///< A frame is ready: the call hands out frame info.
  int error_code = 0;   ///< Nonzero: the call fails with this status (803 wrapping 603 etc.).
};

/// A gesture event of an app volume, as the fake delivers it.
struct FakeGesture
{
  QarGestureKind kind = QAR_GESTURE_CLICK;
  QarGesturePhase phase = QAR_GESTURE_PHASE_INSTANT;
  QarPeerId source{};
  QarVector3 action_point{0.0f, 0.0f, 0.0f};
  bool has_action_point = true;
  QarVector3 start_point{0.0f, 0.0f, 0.0f};
  bool has_start_point = true;
  std::int64_t unix_ns = 1;
};

/// One call into the fake library.
struct Call
{
  std::string name;
  std::thread::id thread;
};

/// What qar_app_volumes_get_or_create was given.
struct VolumeRequest
{
  std::string common_name;
  std::string display_name;
  float centre_y = 0.0f;
  float width = 0.0f;
  float length = 0.0f;
  float height = 0.0f;
  float app_scale = 0.0f;
};

/// What an onboarding call was given.
struct OnboardRequest
{
  std::string kind;                 ///< "local".
  std::uint32_t approval_timeout_ms = 0;
  std::string display_name, app_version, custom_info;
};

/// One GUI panel of the fake session, as the tests see it.
struct PanelInfo
{
  std::string common_name;
  QarGuiPanelState state = QAR_GUI_PANEL_STATE_VISIBLE;
  bool is_url = false;
  std::string uri;
  QarGuiPanelSize size{};
  QarPose pose{};
  bool has_stream = false;
  QarStreamId stream{};
};

class FakeQarApi
{
public:
  FakeQarApi();
  ~FakeQarApi();
  FakeQarApi(const FakeQarApi &) = delete;
  FakeQarApi & operator=(const FakeQarApi &) = delete;

  // --- scripting (before the owner connects) ---------------------------------------------
  /// Outcomes of onboard calls, used in order; the default is success.
  void script_onboard(const Outcome & outcome);
  void script_invite(const Outcome & outcome);
  void set_library_init_code(int code);
  void set_runtime_create_code(int code);
  /// The status qar_app_volumes_get_or_create returns (0 is success).
  void set_volume_code(int code);
  /// Peers that qar_query_peer_specs lists.
  void set_initial_peers(std::vector<FakePeer> peers);
  /// Runs on the calling thread of a call named @p name, before it returns.
  void set_call_hook(std::function<void(const std::string &)> hook);
  /// Makes every later call named @p name, as calls() records it, fail with the status @p code.
  /// A code of 0 clears it. The calls of the GUI panels, the video sender, the app volume and the
  /// count of the app volume query support it.
  void set_call_failure(const std::string & name, int code);
  /// The status of the qar_app_volume_get_latest_* calls (0 is success).
  void set_volume_latest_code(int code);
  /// What the volume reports from now on.
  void set_volume_placement(const QarPose & pose, const QarPose & app_pose, float scale);
  /// Whether qar_video_sender_try_begin_frame reserves a cycle (default: no).
  void set_video_sender_ready(bool ready);
  /// Runs on the calling thread of an asynchronous call that returns a synchronous error but
  /// keeps its callback, with the callback's user-state pointer, before the call returns.
  void set_sync_failure_hook(std::function<void(const void *)> hook);
  /// Outcomes of qar_render_sender_create_async calls, used in order; the default is a sender
  /// that arrives at once. A nonzero code makes the callback fail; wait_for_release (also with a
  /// token that is cancelled, see Outcome::fire_on_cancel) and delay_ms hold the answer back.
  void script_render_create(const Outcome & outcome);
  /// What qar_render_sender_try_begin_frame answers from now on (default: not ready, no error).
  void set_render_frame(const RenderFrameScript & script);
  /// Makes every session call fail with @p code (1802 by default) or wait until it is released
  /// by release_pending() or set_hub_gone(HubGone::kNo). The session calls are the queries, the
  /// GUI panels, the app volumes, the subscriptions, the video sender and render sender creation,
  /// the peer queries and the destroy of a session, a video sender and a render sender (these wait
  /// in kBlockAll and are never refused); onboarding and invite results answer @p code. Frame
  /// calls, getters of handles in hand, cancel tokens and the runtime calls are not affected.
  void set_hub_gone(HubGone mode, int code = 1802);
  /// Blocks the frame calls (try_begin_frame, frame_cpu / frame_opengl, show_frame, last_hands of
  /// the render sender and the video sender) until set_frame_block(false).
  void set_frame_block(bool block);
  /// Slows a frame call by @p ms. @p call_name is a name as calls() records it
  /// ("render_sender_show_frame"); empty sets the delay of every frame call. A named delay wins.
  void set_frame_delay_ms(int ms, const std::string & call_name = std::string());

  // --- while the owner runs ----------------------------------------------------------------
  /// Delivers @p peer to the peer subscription on a thread of the fake library, and returns when
  /// the callback returned. Does nothing when no subscription is active.
  void deliver_peer(const FakePeer & peer);
  /// Like deliver_peer, but returns at once: the callbacks run on a thread of the fake library
  /// that the test does not wait for, so a callback that never returns can be scripted. The
  /// subscription counts as running from the moment this returns. Wait with
  /// wait_detached_deliveries(); the fake's destructor joins the threads.
  void deliver_peer_detached(const FakePeer & peer);
  /// Delivers @p gesture to the gesture subscriptions of the app volume (all kinds, or the kind
  /// they asked for) on a thread of the fake library, and returns when the callbacks returned.
  void deliver_volume_gesture(const FakeGesture & gesture = FakeGesture());
  /// The detached form of deliver_volume_gesture (see deliver_peer_detached).
  void deliver_volume_gesture_detached(const FakeGesture & gesture = FakeGesture());
  /// Delivers one render stream request (a target asks for @p stream) to the request subscriptions
  /// on a thread of the fake library, and returns when the callbacks returned.
  void deliver_render_request(const QarPeerId & peer, const QarStreamId & stream);
  /// The stream id of the @p n-th render sender the fake created (n counts from 1): every byte is
  /// render_stream_tag(n).
  static QarStreamId render_stream_id(int n);
  static unsigned char render_stream_tag(int n) { return static_cast<unsigned char>(0xD0 + n); }
  /// Render requests the callbacks released with qar_render_request_handle_destroy.
  int render_requests_released() const;
  /// Waits until every detached delivery has returned; false after @p timeout_ms.
  bool wait_detached_deliveries(int timeout_ms);
  /// Lets the calls that wait_for_release answer.
  void release_pending();
  /// Delivers the current state of the volume to the update subscriptions, on a thread of the
  /// fake library, and returns when the callbacks returned.
  void deliver_volume_update();
  /// Delivers a report that the GUI panel named @p common_name is in @p state to the update
  /// subscriptions of that panel, on a thread of the fake library, and returns when the callbacks
  /// returned. The state the fake holds for the panel is not changed. Does nothing for a panel
  /// that does not exist.
  void deliver_panel_state(const std::string & common_name, QarGuiPanelState state);

  // --- observation ---------------------------------------------------------------------------
  std::vector<Call> calls() const;
  int count(const std::string & name) const;
  /// Index of the first call named @p name in calls(), or -1.
  int first_index(const std::string & name) const;
  int last_index(const std::string & name) const;
  std::vector<OnboardRequest> onboard_requests() const;
  std::vector<std::string> invite_requests() const;
  std::vector<VolumeRequest> volume_requests() const;
  bool subscription_active() const;
  /// Update subscriptions of app volumes that are active.
  int volume_subscriptions() const;
  /// Update subscriptions of GUI panels that are active.
  int panel_subscriptions() const;
  /// The GUI panels the fake session holds, in creation order.
  std::vector<PanelInfo> panels() const;
  /// Video senders that were created and not destroyed.
  int live_video_senders() const;
  /// Frames shown by video senders.
  int video_frames_shown() const;
  int borrowed_handle_destroys() const;
  int owned_handles_outstanding() const;
  int live_sessions() const;
  int onboard_results_fired() const;
  int invite_results_fired() const;
  /// How often qar_render_stream_handle_destroy was called.
  int render_streams_destroyed() const;
  int live_tokens() const;
  /// Render senders that were created and not destroyed.
  int live_render_senders() const;
  /// Calls of qar_render_sender_create_async that started a creation.
  int render_creates() const;
  /// Creations whose token was cancelled (or destroyed) before they answered.
  int render_creates_cancelled() const;
  /// Creations whose callback has not fired yet.
  int render_creates_pending() const;
  /// Frame infos handed out by qar_render_sender_try_begin_frame and not destroyed.
  int live_render_frame_infos() const;
  /// Frames shown by render senders.
  int render_frames_shown() const;
  /// Gesture subscriptions of app volumes that are active.
  int gesture_subscriptions() const;
  /// Gesture subscriptions of GUI panels that are active (the fake delivers none).
  int panel_gesture_subscriptions() const;
  /// Runtimes that were created and not destroyed, and how many creates succeeded.
  int live_runtimes() const;
  int runtime_creates() const;
  /// Session calls that wait inside set_hub_gone(HubGone::kBlockAll) now.
  int hub_blocked_calls() const;
  /// Frame calls that wait inside set_frame_block(true) now.
  int frame_calls_blocked() const;
  /// The thread of the last call named @p name, or a default-constructed id when there was none.
  std::thread::id thread_of_last(const std::string & name) const;
  bool library_loaded() const { return qar_is_library_loaded(); }

  /// The loader the owner is given. @p installed answers installed_bin_folder; @p load_ok
  /// answers load.
  std::unique_ptr<ILibraryLoader> make_loader(bool installed = true, bool load_ok = true);

  /// The state the functions of the fake share; defined in the .cpp.
  struct State;

private:
  std::unique_ptr<State> state_;
};

}  // namespace test
}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__TEST__FAKE_QAR_API_HPP_
