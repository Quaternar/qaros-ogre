// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "fake_qar_api.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <utility>

namespace qar_qar_adapter
{
namespace test
{

namespace
{

QarResult ok_result()
{
  QarResult result{};
  result.code = QAR_STATUS_SUCCESS;
  result.error_handle = 0;
  return result;
}

QarResult error_result(int code)
{
  QarResult result{};
  result.code = static_cast<QarStatusCode>(code);
  result.error_handle = 1;
  return result;
}

struct FakeToken
{
  bool cancelled = false;
  /// The same fact as @c cancelled, readable by a render sender creation after the token is gone.
  std::shared_ptr<bool> flag = std::make_shared<bool>(false);
};

/// One qar_render_sender_create_async that has not answered yet.
struct RenderCreateState
{
  std::shared_ptr<bool> token_flag;  ///< Null when the call had no token.
  bool answered = false;
  bool cancel_counted = false;
};

struct RenderSenderObject
{
  QarPeerId peer{};
  QarStreamId stream{};
};

/// One render stream request as the fake delivers it.
struct RenderRequestObject
{
  QarPeerId peer{};
  QarStreamId stream{};
};

struct RenderFrameInfoObject
{
  int magic = 0xF4A3;
};

struct GestureObject
{
  FakeGesture gesture;
  QarAppVolumeId volume{};
};

struct FakeSession
{
  int magic = 0x5E55;
};

struct FakeRuntime
{
  int magic = 0x2071;
};

struct HandleObject
{
  FakePeer peer;
  bool borrowed = false;
};

}  // namespace

struct FakeQarApi::State
{
  struct Subscription
  {
    FakeToken * token = nullptr;
    qar_peer_update_callback_t callback = nullptr;
    void * user = nullptr;
    int running = 0;
    bool active = true;
  };

  struct VolumeSubscription
  {
    FakeToken * token = nullptr;
    qar_app_volume_update_callback_t callback = nullptr;
    void * user = nullptr;
    int running = 0;
    bool active = true;
  };

  struct PanelRecord
  {
    QarGuiPanelId id{};
    PanelInfo info;
  };

  /// A subscription to the updates of one GUI panel.
  struct PanelSubscription
  {
    FakeToken * token = nullptr;
    qar_gui_panel_update_callback_t callback = nullptr;
    void * user = nullptr;
    QarGuiPanelId id{};
    int running = 0;
    bool active = true;
  };

  /// A subscription to the gesture events of an app volume.
  struct GestureSubscription
  {
    FakeToken * token = nullptr;
    qar_app_volume_gesture_event_callback_t callback = nullptr;
    void * user = nullptr;
    int kind = -1;  ///< A QarGestureKind, or -1 for every kind.
    int running = 0;
    bool active = true;
  };

  /// A subscription to the gesture events of a GUI panel (only counted).
  struct PanelGestureSubscription
  {
    FakeToken * token = nullptr;
    bool active = true;
  };

  /// A subscription to the render stream requests of the session.
  struct RenderRequestSubscription
  {
    FakeToken * token = nullptr;
    qar_render_sender_request_callback_t callback = nullptr;
    void * user = nullptr;
    int running = 0;
    bool active = true;
  };

  struct SenderObject
  {
    QarStreamId stream{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> buffer;
  };

  mutable std::mutex mutex;
  std::condition_variable cv;
  std::vector<Call> calls;
  std::list<VolumeSubscription> volume_subs;
  std::list<PanelSubscription> panel_subs;
  std::list<GestureSubscription> gesture_subs;
  std::list<PanelGestureSubscription> panel_gesture_subs;
  std::list<RenderRequestSubscription> render_request_subs;
  std::atomic<int> next_render_stream{1};
  std::atomic<int> render_requests_released{0};
  std::map<std::string, int> failures;
  int latest_code = 0;
  std::vector<PanelRecord> panels;
  bool sender_ready = false;
  std::atomic<int> live_senders{0};
  std::atomic<int> frames_shown{0};
  std::atomic<int> next_sender{1};
  std::vector<OnboardRequest> onboard_requests;
  std::vector<std::string> invite_requests;
  std::vector<VolumeRequest> volume_requests;
  QarAppVolumeId volume_id{};
  QarPose volume_pose{};
  QarPose volume_app_pose{};
  float volume_scale = 1.0f;
  bool volume_exists = false;
  int volume_code = 0;
  std::deque<Outcome> onboard_script;
  std::deque<Outcome> invite_script;
  std::vector<FakePeer> initial_peers;
  std::list<Subscription> subscriptions;
  std::vector<std::thread> threads;
  std::vector<std::thread> detached;
  int detached_running = 0;
  std::deque<Outcome> render_script;
  std::vector<std::shared_ptr<RenderCreateState>> render_creating;
  std::set<const void *> render_senders;
  RenderFrameScript render_frame;
  HubGone hub_gone = HubGone::kNo;
  int hub_code = 1802;
  int hub_blocked = 0;
  bool frame_block = false;
  int frame_blocked = 0;
  int frame_delay_all = 0;
  std::map<std::string, int> frame_delay;
  std::function<void(const std::string &)> hook;
  std::function<void(const void *)> sync_failure_hook;
  std::uint64_t release_epoch = 0;
  bool shutting_down = false;
  int library_init_code = 0;
  int runtime_create_code = 0;
  std::atomic<int> borrowed_destroys{0};
  std::atomic<int> owned_outstanding{0};
  std::atomic<int> live_sessions{0};
  std::atomic<int> onboard_results{0};
  std::atomic<int> invite_results{0};
  std::atomic<int> live_tokens{0};
  std::atomic<int> render_streams_destroyed{0};
  std::atomic<int> live_render_senders{0};
  std::atomic<int> render_creates{0};
  std::atomic<int> render_creates_cancelled{0};
  std::atomic<int> live_render_infos{0};
  std::atomic<int> render_frames_shown{0};
  std::atomic<int> live_runtimes{0};
  std::atomic<int> runtime_creates{0};
  std::atomic<int> next_id{1};
  FakeRuntime runtime;
  int sentinel = 0;
};

namespace
{

FakeQarApi::State * g_state = nullptr;

void record(const std::string & name)
{
  std::function<void(const std::string &)> hook;
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    g_state->calls.push_back(Call{name, std::this_thread::get_id()});
    hook = g_state->hook;
  }
  if (hook) {
    hook(name);
  }
}

// Decides what a session call does while the Hub may be gone. Waits (in kBlockAll) until the mode
// changes, release_pending() is called after @p epoch, @p also_wake says so, or the fake ends.
// @return 0 when the Hub is there (or @p also_wake ended the wait), else the status the call fails
// with. The caller holds @p lock.
int hub_answer(
  std::unique_lock<std::mutex> & lock, std::uint64_t epoch,
  const std::function<bool()> & also_wake = nullptr)
{
  if (g_state->hub_gone == HubGone::kBlockAll) {
    ++g_state->hub_blocked;
    g_state->cv.wait(lock, [&epoch, &also_wake] {
        return g_state->hub_gone != HubGone::kBlockAll || g_state->release_epoch > epoch ||
               g_state->shutting_down || (also_wake && also_wake());
      });
    --g_state->hub_blocked;
    g_state->cv.notify_all();
    if (also_wake && also_wake()) {
      return 0;
    }
    if (g_state->shutting_down) {
      return g_state->hub_code;
    }
  }
  return g_state->hub_gone == HubGone::kNo ? 0 : g_state->hub_code;
}

// The gate of every session call: @return 0 to go on, else the status to fail with.
int hub_gate()
{
  std::unique_lock<std::mutex> lock(g_state->mutex);
  if (g_state->hub_gone == HubGone::kNo) {
    return 0;
  }
  return hub_answer(lock, g_state->release_epoch);
}

// The gate of a frame call: waits while the frame calls are blocked, then sleeps the scripted time.
void frame_gate(const char * name)
{
  int delay_ms = 0;
  {
    std::unique_lock<std::mutex> lock(g_state->mutex);
    if (g_state->frame_block) {
      ++g_state->frame_blocked;
      g_state->cv.wait(lock, [] { return !g_state->frame_block || g_state->shutting_down; });
      --g_state->frame_blocked;
      g_state->cv.notify_all();
    }
    delay_ms = g_state->frame_delay_all;
    const auto it = g_state->frame_delay.find(name);
    if (it != g_state->frame_delay.end()) {
      delay_ms = it->second;
    }
  }
  if (delay_ms > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }
}

QarPeerSpec * as_handle(HandleObject * object)
{
  return reinterpret_cast<QarPeerSpec *>(object);
}

HandleObject * as_object(QarPeerSpec * handle)
{
  return reinterpret_cast<HandleObject *>(handle);
}

QarPeerSpec * new_owned_handle(const FakePeer & peer)
{
  auto * object = new HandleObject{peer, false};
  g_state->owned_outstanding.fetch_add(1);
  return as_handle(object);
}

// --- result ------------------------------------------------------------------------------------

bool fake_result_is_error(QarResult result) { return result.code != QAR_STATUS_SUCCESS; }
bool fake_result_is_success(QarResult result) { return result.code == QAR_STATUS_SUCCESS; }
bool fake_result_has_code(QarResult result, QarStatusCode code) { return result.code == code; }
void fake_result_message(QarResult result, char * buffer, size_t size)
{
  if (buffer != nullptr && size > 0) {
    std::snprintf(buffer, size, "fake message for code %d", static_cast<int>(result.code));
  }
}

// --- types ---------------------------------------------------------------------------------

QarResult fake_uuid_to_string(const uint8_t * bytes, char * out, size_t size)
{
  if (bytes == nullptr || out == nullptr || size < 37) {
    return error_result(QAR_STATUS_ARGUMENT_NOT_SUPPORTED);
  }
  std::snprintf(out, size,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8],
    bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return ok_result();
}

QarResult fake_uuid_from_string(const char * text, uint8_t * out)
{
  if (text == nullptr || out == nullptr || std::strlen(text) != 36) {
    return error_result(QAR_STATUS_ARGUMENT_NOT_SUPPORTED);
  }
  auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') { return c - '0'; }
      if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
      if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
      return -1;
    };
  int byte_index = 0;
  for (int i = 0; i < 36;) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (text[i] != '-') {
        return error_result(QAR_STATUS_ARGUMENT_NOT_SUPPORTED);
      }
      ++i;
      continue;
    }
    const int high = digit(text[i]);
    const int low = digit(text[i + 1]);
    if (high < 0 || low < 0) {
      return error_result(QAR_STATUS_ARGUMENT_NOT_SUPPORTED);
    }
    out[byte_index++] = static_cast<uint8_t>(high * 16 + low);
    i += 2;
  }
  return ok_result();
}

// --- cancel tokens ---------------------------------------------------------------------------

QarResult fake_cancel_token_create(QarCancelToken ** out)
{
  record("cancel_token_create");
  *out = reinterpret_cast<QarCancelToken *>(new FakeToken());
  g_state->live_tokens.fetch_add(1);
  return ok_result();
}

// Ends the subscriptions of @p token and waits for a callback of theirs that is running.
void end_subscriptions(FakeToken * token)
{
  std::unique_lock<std::mutex> lock(g_state->mutex);
  for (auto & sub : g_state->subscriptions) {
    if (sub.token == token) {
      sub.active = false;
    }
  }
  for (auto & sub : g_state->volume_subs) {
    if (sub.token == token) {
      sub.active = false;
    }
  }
  for (auto & sub : g_state->panel_subs) {
    if (sub.token == token) {
      sub.active = false;
    }
  }
  for (auto & sub : g_state->gesture_subs) {
    if (sub.token == token) {
      sub.active = false;
    }
  }
  for (auto & sub : g_state->panel_gesture_subs) {
    if (sub.token == token) {
      sub.active = false;
    }
  }
  for (auto & sub : g_state->render_request_subs) {
    if (sub.token == token) {
      sub.active = false;
    }
  }
  g_state->cv.wait(lock, [&token] {
      for (const auto & sub : g_state->subscriptions) {
        if (sub.token == token && sub.running > 0) {
          return false;
        }
      }
      for (const auto & sub : g_state->volume_subs) {
        if (sub.token == token && sub.running > 0) {
          return false;
        }
      }
      for (const auto & sub : g_state->panel_subs) {
        if (sub.token == token && sub.running > 0) {
          return false;
        }
      }
      for (const auto & sub : g_state->gesture_subs) {
        if (sub.token == token && sub.running > 0) {
          return false;
        }
      }
      for (const auto & sub : g_state->render_request_subs) {
        if (sub.token == token && sub.running > 0) {
          return false;
        }
      }
      return true;
    });
}

// Marks @p token cancelled and counts the render sender creations it owned that had not answered.
// The caller holds the mutex.
void mark_cancelled(FakeToken * token)
{
  token->cancelled = true;
  *token->flag = true;
  for (auto & creating : g_state->render_creating) {
    if (creating->token_flag == token->flag && !creating->answered && !creating->cancel_counted) {
      creating->cancel_counted = true;
      g_state->render_creates_cancelled.fetch_add(1);
    }
  }
}

QarResult fake_cancel_token_cancel(QarCancelToken * handle)
{
  record("cancel_token_cancel");
  auto * token = reinterpret_cast<FakeToken *>(handle);
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    mark_cancelled(token);
  }
  g_state->cv.notify_all();
  end_subscriptions(token);
  return ok_result();
}

void fake_cancel_token_handle_destroy(QarCancelToken * handle)
{
  record("cancel_token_handle_destroy");
  auto * token = reinterpret_cast<FakeToken *>(handle);
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    mark_cancelled(token);
  }
  g_state->cv.notify_all();
  end_subscriptions(token);
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    g_state->subscriptions.remove_if([token](const FakeQarApi::State::Subscription & sub) {
        return sub.token == token;
      });
    g_state->volume_subs.remove_if([token](const FakeQarApi::State::VolumeSubscription & sub) {
        return sub.token == token;
      });
    g_state->panel_subs.remove_if([token](const FakeQarApi::State::PanelSubscription & sub) {
        return sub.token == token;
      });
    g_state->gesture_subs.remove_if([token](const FakeQarApi::State::GestureSubscription & sub) {
        return sub.token == token;
      });
    g_state->panel_gesture_subs.remove_if(
      [token](const FakeQarApi::State::PanelGestureSubscription & sub) {
        return sub.token == token;
      });
    g_state->render_request_subs.remove_if(
      [token](const FakeQarApi::State::RenderRequestSubscription & sub) {
        return sub.token == token;
      });
  }
  delete token;
  g_state->live_tokens.fetch_sub(1);
}

// --- runtime -----------------------------------------------------------------------------------

QarResult fake_library_init(const QarLibraryInit *)
{
  record("library_init");
  return g_state->library_init_code == 0 ? ok_result() : error_result(g_state->library_init_code);
}

QarResult fake_library_destroy()
{
  record("library_destroy");
  return ok_result();
}

QarResult fake_runtime_info(QarRuntimeInfo * info)
{
  record("runtime_info");
  info->version = "0.9.0-fake";
  info->flavor = "release";
  info->c_api = "v0";
  return ok_result();
}

QarResult fake_runtime_create(const QarRuntimeInit *, QarRuntime ** out)
{
  record("runtime_create");
  if (g_state->runtime_create_code != 0) {
    *out = nullptr;
    return error_result(g_state->runtime_create_code);
  }
  *out = reinterpret_cast<QarRuntime *>(&g_state->runtime);
  g_state->runtime_creates.fetch_add(1);
  g_state->live_runtimes.fetch_add(1);
  return ok_result();
}

void fake_runtime_handle_destroy(QarRuntime *)
{
  record("runtime_handle_destroy");
  int live = g_state->live_runtimes.load();
  while (live > 0 && !g_state->live_runtimes.compare_exchange_weak(live, live - 1)) {
  }
}

void fake_session_handle_destroy(QarSession * handle)
{
  record("session_handle_destroy");
  hub_gate();
  delete reinterpret_cast<FakeSession *>(handle);
  g_state->live_sessions.fetch_sub(1);
}

// --- render streams ----------------------------------------------------------------------------

void fake_render_stream_handle_destroy(QarRenderSender * handle)
{
  record("render_stream_handle_destroy");
  g_state->render_streams_destroyed.fetch_add(1);
  hub_gate();
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    known = g_state->render_senders.erase(handle) > 0;
  }
  if (known) {
    delete reinterpret_cast<RenderSenderObject *>(handle);
    g_state->live_render_senders.fetch_sub(1);
  }
}

// --- asynchronous calls ------------------------------------------------------------------------

// A call that returned a synchronous error has had its token destroyed by the caller; the fake
// does not read it afterwards.
template<typename Op>
bool token_cancelled(const Op & op)
{
  return op.outcome.sync_error_code == 0 && op.token->cancelled;
}

struct OnboardOp
{
  qar_runtime_onboard_result_callback_t result = nullptr;
  qar_progress_callback_t progress = nullptr;
  void * user = nullptr;
  FakeToken * token = nullptr;
  Outcome outcome;
  QarOnboardingId id{};
  std::uint64_t epoch = 0;
};

void run_onboard(OnboardOp op)
{
  // A Hub that is gone answers with its code (after the release, in kBlockAll).
  int gone = 0;
  {
    std::unique_lock<std::mutex> lock(g_state->mutex);
    gone = hub_answer(lock, op.epoch, [&op] { return token_cancelled(op); });
  }
  if (gone == 0 && op.outcome.progress && op.progress != nullptr) {
    op.progress(QAR_ACTION_SEVERITY_INFO, 10.0f, "Waiting for approval in QAROS hub", op.user);
  }
  {
    std::unique_lock<std::mutex> lock(g_state->mutex);
    if (gone != 0) {
      // Nothing to wait for.
    } else if (op.outcome.wait_for_release) {
      g_state->cv.wait(lock, [&op] {
          return token_cancelled(op) || g_state->shutting_down || g_state->release_epoch > op.epoch;
        });
    } else if (op.outcome.delay_ms > 0) {
      g_state->cv.wait_for(lock, std::chrono::milliseconds(op.outcome.delay_ms), [&op] {
          return token_cancelled(op) || g_state->shutting_down;
        });
    }
  }
  QarResult result{};
  QarSession * session = nullptr;
  const QarOnboardingId * id = nullptr;
  bool cancelled = false;
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    cancelled = token_cancelled(op);
  }
  if (cancelled || g_state->shutting_down) {
    result = error_result(QAR_STATUS_UNCLASSIFIED);
  } else if (gone != 0) {
    result = error_result(gone);
  } else if (op.outcome.code != 0) {
    result = error_result(op.outcome.code);
  } else {
    result = ok_result();
    session = reinterpret_cast<QarSession *>(new FakeSession());
    g_state->live_sessions.fetch_add(1);
    id = &op.id;
  }
  g_state->onboard_results.fetch_add(1);
  op.result(result, id, session, op.user);
}

Outcome next_outcome(std::deque<Outcome> & script)
{
  std::lock_guard<std::mutex> lock(g_state->mutex);
  if (script.empty()) {
    return Outcome{};
  }
  Outcome outcome = script.front();
  script.pop_front();
  return outcome;
}

// Reports the callback context of a call that fails synchronously and keeps its callback.
void note_sync_failure(const void * user)
{
  std::function<void(const void *)> hook;
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    hook = g_state->sync_failure_hook;
  }
  if (hook) {
    hook(user);
  }
}

// @return the status the call returns.
QarResult start_onboard(
  const OnboardRequest & request, qar_runtime_onboard_result_callback_t result,
  qar_progress_callback_t progress, void * user, QarCancelToken * token, const QarOnboardingId & id)
{
  OnboardOp op;
  op.result = result;
  op.progress = progress;
  op.user = user;
  op.token = reinterpret_cast<FakeToken *>(token);
  op.outcome = next_outcome(g_state->onboard_script);
  op.id = id;
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    g_state->onboard_requests.push_back(request);
    op.epoch = g_state->release_epoch;
    g_state->threads.emplace_back([op] { run_onboard(op); });
  }
  if (op.outcome.sync_error_code != 0) {
    note_sync_failure(user);
    return error_result(op.outcome.sync_error_code);
  }
  return ok_result();
}

QarOnboardingId fresh_id()
{
  QarOnboardingId id{};
  const int n = g_state->next_id.fetch_add(1);
  for (int i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    id.data[i] = static_cast<std::uint8_t>(0xA0 + n + i);
  }
  return id;
}

void read_presentation(const QarPeerPresentation & presentation, OnboardRequest & request)
{
  if (presentation.display_name != nullptr) { request.display_name = presentation.display_name; }
  if (presentation.app_version != nullptr) { request.app_version = presentation.app_version; }
  if (presentation.app_custom_peer_info != nullptr) {
    request.custom_info = presentation.app_custom_peer_info;
  }
}

QarResult fake_runtime_onboard_async(
  QarRuntime *, const QarOnboardInit * init, qar_runtime_onboard_result_callback_t result,
  qar_progress_callback_t progress, void * user, QarCancelToken * token)
{
  record("runtime_onboard_async");
  OnboardRequest request;
  request.kind = "local";
  read_presentation(init->presentation, request);
  for (const auto * header = static_cast<const QarStructureHeader *>(init->header.next);
    header != nullptr; header = static_cast<const QarStructureHeader *>(header->next))
  {
    switch (header->type) {
      case QAR_STRUCTURE_TYPE_RUNTIME_ONBOARD_LOCAL_APP_EXT: {
        const auto * ext = reinterpret_cast<const QarOnboardLocalAppExt *>(header);
        request.approval_timeout_ms = ext->approval_timeout_ms;
        break;
      }
      default:
        break;
    }
  }
  return start_onboard(request, result, progress, user, token, fresh_id());
}

struct InviteOp
{
  qar_target_app_invite_result_callback_t result = nullptr;
  void * user = nullptr;
  FakeToken * token = nullptr;
  Outcome outcome;
  std::uint64_t epoch = 0;
};

void run_invite(InviteOp op)
{
  int gone = 0;
  {
    std::unique_lock<std::mutex> lock(g_state->mutex);
    gone = hub_answer(lock, op.epoch, [&op] { return token_cancelled(op); });
  }
  {
    std::unique_lock<std::mutex> lock(g_state->mutex);
    if (gone != 0) {
      // Nothing to wait for.
    } else if (op.outcome.wait_for_release) {
      g_state->cv.wait(lock, [&op] {
          return token_cancelled(op) || g_state->shutting_down || g_state->release_epoch > op.epoch;
        });
    } else if (op.outcome.delay_ms > 0) {
      g_state->cv.wait_for(lock, std::chrono::milliseconds(op.outcome.delay_ms), [&op] {
          return token_cancelled(op) || g_state->shutting_down;
        });
    }
  }
  bool cancelled = false;
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    cancelled = token_cancelled(op);
  }
  QarPeerId invited{};
  for (int i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    invited.data[i] = static_cast<std::uint8_t>(0xD0 + i);
  }
  QarResult result = ok_result();
  if (cancelled || g_state->shutting_down) {
    result = error_result(QAR_STATUS_UNCLASSIFIED);
  } else if (gone != 0) {
    result = error_result(gone);
  } else if (op.outcome.code != 0) {
    result = error_result(op.outcome.code);
  }
  g_state->invite_results.fetch_add(1);
  op.result(result, result.code == QAR_STATUS_SUCCESS ? &invited : nullptr, op.user);
}

QarResult fake_session_invite_target_app_async(
  QarSession *, const QarSessionInviteTargetAppInit * init,
  qar_target_app_invite_result_callback_t result, qar_progress_callback_t, void * user,
  QarCancelToken * token)
{
  record("session_invite_target_app_async");
  InviteOp op;
  op.result = result;
  op.user = user;
  op.token = reinterpret_cast<FakeToken *>(token);
  op.outcome = next_outcome(g_state->invite_script);
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    g_state->invite_requests.push_back(init->connection_string != nullptr ?
      init->connection_string : "");
    op.epoch = g_state->release_epoch;
    g_state->threads.emplace_back([op] { run_invite(op); });
  }
  if (op.outcome.sync_error_code != 0) {
    note_sync_failure(user);
    return error_result(op.outcome.sync_error_code);
  }
  return ok_result();
}

// --- app volumes -----------------------------------------------------------------------------

struct VolumeObject
{
  QarAppVolumeId id{};
  QarPose pose{};
  QarPose app_pose{};
  float scale = 1.0f;
};

QarResult fake_app_volumes_get_or_create(
  QarSession *, const QarAppVolumeInit * init, QarAppVolumeId * out)
{
  record("app_volumes_get_or_create");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  VolumeRequest request;
  request.common_name = init->common_name != nullptr ? init->common_name : "";
  request.display_name = init->display_name != nullptr ? init->display_name : "";
  request.centre_y = init->pose.position.y;
  request.width = init->size.width_meters;
  request.length = init->size.length_meters;
  request.height = init->size.height_meters;
  request.app_scale = init->app_scale;
  g_state->volume_requests.push_back(request);
  if (g_state->volume_code != 0) {
    return error_result(g_state->volume_code);
  }
  for (int i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    g_state->volume_id.data[i] = static_cast<std::uint8_t>(0xB0 + i);
  }
  g_state->volume_pose = init->pose;
  g_state->volume_app_pose = init->app_pose;
  g_state->volume_scale = init->app_scale;
  g_state->volume_exists = true;
  *out = g_state->volume_id;
  return ok_result();
}

int forced_code(const char * name);

QarResult fake_query_app_volumes_count(QarSession *, size_t * count)
{
  record("query_app_volumes_count");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  if (const int code = forced_code("query_app_volumes_count")) {
    return error_result(code);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  *count = g_state->volume_exists ? 1u : 0u;
  return ok_result();
}

QarResult fake_query_app_volumes(
  QarSession *, QarAppVolume ** handles, size_t capacity, size_t * written)
{
  record("query_app_volumes");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  size_t n = 0;
  if (g_state->volume_exists && capacity > 0) {
    auto * object = new VolumeObject();
    object->id = g_state->volume_id;
    object->pose = g_state->volume_pose;
    object->app_pose = g_state->volume_app_pose;
    object->scale = g_state->volume_scale;
    handles[0] = reinterpret_cast<QarAppVolume *>(object);
    n = 1;
  }
  *written = n;
  return ok_result();
}

QarResult fake_app_volume_get_id(QarAppVolume * handle, QarAppVolumeId * out)
{
  *out = reinterpret_cast<VolumeObject *>(handle)->id;
  return ok_result();
}

QarResult fake_app_volume_get_pose(QarAppVolume * handle, QarPose * out)
{
  *out = reinterpret_cast<VolumeObject *>(handle)->pose;
  return ok_result();
}

QarResult fake_app_volume_get_app_pose(QarAppVolume * handle, QarPose * out)
{
  *out = reinterpret_cast<VolumeObject *>(handle)->app_pose;
  return ok_result();
}

QarResult fake_app_volume_get_app_scale(QarAppVolume * handle, float * out)
{
  *out = reinterpret_cast<VolumeObject *>(handle)->scale;
  return ok_result();
}

void fake_app_volume_handle_destroy(QarAppVolume * handle)
{
  delete reinterpret_cast<VolumeObject *>(handle);
}

// --- peers -------------------------------------------------------------------------------------

QarResult fake_peer_spec_get_id(QarPeerSpec * handle, QarPeerId * out)
{
  *out = as_object(handle)->peer.id;
  return ok_result();
}

QarResult fake_peer_spec_get_role(QarPeerSpec * handle, QarPeerRole * out)
{
  *out = as_object(handle)->peer.role;
  return ok_result();
}

QarResult fake_peer_spec_get_presence(QarPeerSpec * handle, QarPeerPresence * out)
{
  *out = as_object(handle)->peer.presence;
  return ok_result();
}

QarResult fake_peer_spec_get_category(QarPeerSpec * handle, QarDeviceCategory * out)
{
  *out = as_object(handle)->peer.category;
  return ok_result();
}

QarResult copy_text(const std::string & text, char * out, size_t size)
{
  if (out == nullptr || size == 0) {
    return error_result(QAR_STATUS_LOGIC_ERROR);
  }
  std::snprintf(out, size, "%s", text.c_str());
  return ok_result();
}

// --- more app-volume calls -----------------------------------------------------------------

int forced_code(const char * name)
{
  std::lock_guard<std::mutex> lock(g_state->mutex);
  const auto it = g_state->failures.find(name);
  return it == g_state->failures.end() ? 0 : it->second;
}

QarResult fake_app_volumes_subscribe_updates(
  QarSession *, qar_app_volume_update_callback_t callback, void * user, QarCancelToken * token)
{
  record("app_volumes_subscribe_updates");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("app_volumes_subscribe_updates");
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  FakeQarApi::State::VolumeSubscription sub;
  sub.token = reinterpret_cast<FakeToken *>(token);
  sub.callback = callback;
  sub.user = user;
  g_state->volume_subs.push_back(sub);
  return ok_result();
}

QarResult latest_result(const char * name)
{
  record(name);
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  return g_state->latest_code == 0 ? ok_result() : error_result(g_state->latest_code);
}

QarResult fake_app_volume_get_latest_pose(QarSession *, const QarAppVolumeId *, QarPose * out)
{
  const QarResult r = latest_result("app_volume_get_latest_pose");
  if (r.code == QAR_STATUS_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    *out = g_state->volume_pose;
  }
  return r;
}

QarResult fake_app_volume_get_latest_app_pose(
  QarSession *, const QarAppVolumeId *, QarPose * out)
{
  const QarResult r = latest_result("app_volume_get_latest_app_pose");
  if (r.code == QAR_STATUS_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    *out = g_state->volume_app_pose;
  }
  return r;
}

QarResult fake_app_volume_get_latest_app_scale(QarSession *, const QarAppVolumeId *, float * out)
{
  const QarResult r = latest_result("app_volume_get_latest_app_scale");
  if (r.code == QAR_STATUS_SUCCESS) {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    *out = g_state->volume_scale;
  }
  return r;
}

QarResult fake_app_volumes_change_app_world_anchor(
  QarSession *, const QarAppVolumeId *, const QarGeoAnchorFrame *)
{
  record("app_volumes_change_app_world_anchor");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("app_volumes_change_app_world_anchor");
  return forced == 0 ? ok_result() : error_result(forced);
}

QarResult fake_app_volumes_clear_app_world_anchor(QarSession *, const QarAppVolumeId *)
{
  record("app_volumes_clear_app_world_anchor");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("app_volumes_clear_app_world_anchor");
  return forced == 0 ? ok_result() : error_result(forced);
}

// --- app-volume gestures -----------------------------------------------------------------------

QarResult add_gesture_subscription(
  const char * name, int kind, qar_app_volume_gesture_event_callback_t callback, void * user,
  QarCancelToken * token)
{
  record(name);
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code(name);
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  FakeQarApi::State::GestureSubscription sub;
  sub.token = reinterpret_cast<FakeToken *>(token);
  sub.callback = callback;
  sub.user = user;
  sub.kind = kind;
  g_state->gesture_subs.push_back(sub);
  return ok_result();
}

QarResult fake_app_volumes_subscribe_all_gesture_events(
  QarSession *, const QarAppVolumeId *, qar_app_volume_gesture_event_callback_t callback,
  void * user, QarCancelToken * token)
{
  return add_gesture_subscription(
    "app_volumes_subscribe_all_gesture_events", -1, callback, user, token);
}

QarResult fake_app_volumes_subscribe_gesture_events(
  QarSession *, const QarAppVolumeId *, QarGestureKind kind,
  qar_app_volume_gesture_event_callback_t callback, void * user, QarCancelToken * token)
{
  return add_gesture_subscription(
    "app_volumes_subscribe_gesture_events", static_cast<int>(kind), callback, user, token);
}

GestureObject * as_gesture(QarAppVolumeGesture * handle)
{
  return reinterpret_cast<GestureObject *>(handle);
}

QarResult fake_app_volume_gesture_get_lifecycle(
  QarAppVolumeGesture * handle, QarPeerId * out_source, QarAppVolumeId * out_volume)
{
  *out_source = as_gesture(handle)->gesture.source;
  *out_volume = as_gesture(handle)->volume;
  return ok_result();
}

QarResult fake_app_volume_gesture_get_classification(
  QarAppVolumeGesture * handle, QarGestureKind * out_kind, QarGesturePhase * out_phase)
{
  *out_kind = as_gesture(handle)->gesture.kind;
  *out_phase = as_gesture(handle)->gesture.phase;
  return ok_result();
}

QarResult fake_app_volume_gesture_get_points(
  QarAppVolumeGesture * handle, QarVector3 * out_action, bool * out_has_action,
  QarVector3 * out_start, bool * out_has_start)
{
  const FakeGesture & gesture = as_gesture(handle)->gesture;
  *out_action = gesture.action_point;
  *out_has_action = gesture.has_action_point;
  *out_start = gesture.start_point;
  *out_has_start = gesture.has_start_point;
  return ok_result();
}

QarResult fake_app_volume_gesture_get_timestamp(
  QarAppVolumeGesture * handle, QarTimePoint * out_timestamp)
{
  out_timestamp->unix_ns = as_gesture(handle)->gesture.unix_ns;
  return ok_result();
}

// --- render senders ----------------------------------------------------------------------------

struct RenderCreateOp
{
  qar_render_sender_create_callback_t result = nullptr;
  void * user = nullptr;
  Outcome outcome;
  QarPeerId peer{};
  std::uint64_t epoch = 0;
  std::shared_ptr<RenderCreateState> state;
};

// The creation answers on a thread of its own. Cancelling its token wakes it only when the outcome
// says fire_on_cancel; otherwise it keeps waiting for release_pending() or its delay and then
// answers as scripted (a late sender for a caller that gave up). A fake that ends calls back only
// for a cancelled creation, because the caller may be gone.
void run_render_create(RenderCreateOp op)
{
  bool cancelled = false;
  bool fire = true;
  {
    std::unique_lock<std::mutex> lock(g_state->mutex);
    const auto woken = [&op] {
        return g_state->shutting_down ||
               (op.outcome.fire_on_cancel && op.state->token_flag && *op.state->token_flag);
      };
    if (op.outcome.wait_for_release) {
      g_state->cv.wait(lock, [&op, &woken] {
          return woken() || g_state->release_epoch > op.epoch;
        });
    } else if (op.outcome.delay_ms > 0) {
      g_state->cv.wait_for(lock, std::chrono::milliseconds(op.outcome.delay_ms), woken);
    }
    cancelled = op.outcome.fire_on_cancel && op.state->token_flag && *op.state->token_flag;
    fire = cancelled || !g_state->shutting_down;
    op.state->answered = true;
  }
  if (!fire) {
    return;
  }
  QarResult result{};
  QarRenderSender * stream = nullptr;
  if (cancelled) {
    result = error_result(QAR_STATUS_UNCLASSIFIED);
  } else if (op.outcome.code != 0) {
    result = error_result(op.outcome.code);
  } else {
    auto * object = new RenderSenderObject{op.peer};
    std::memset(
      object->stream.data, FakeQarApi::render_stream_tag(g_state->next_render_stream.fetch_add(1)),
      sizeof(object->stream.data));
    {
      std::lock_guard<std::mutex> lock(g_state->mutex);
      g_state->render_senders.insert(object);
    }
    g_state->live_render_senders.fetch_add(1);
    stream = reinterpret_cast<QarRenderSender *>(object);
    result = ok_result();
  }
  op.result(result, stream, op.user);
}

QarResult fake_render_sender_create_async(
  QarSession *, QarRenderSenderInit * init, qar_render_sender_create_callback_t callback,
  void * user, QarCancelToken * token)
{
  record("render_sender_create_async");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("render_sender_create_async");
  if (forced != 0) {
    return error_result(forced);
  }
  RenderCreateOp op;
  op.result = callback;
  op.user = user;
  op.peer = init != nullptr ? init->peer_id : QarPeerId{};
  op.state = std::make_shared<RenderCreateState>();
  if (token != nullptr) {
    op.state->token_flag = reinterpret_cast<FakeToken *>(token)->flag;
  }
  op.outcome = next_outcome(g_state->render_script);
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    op.epoch = g_state->release_epoch;
    auto & creating = g_state->render_creating;
    creating.erase(
      std::remove_if(creating.begin(), creating.end(),
      [](const std::shared_ptr<RenderCreateState> & state) { return state->answered; }),
      creating.end());
    creating.push_back(op.state);
    g_state->threads.emplace_back([op] { run_render_create(op); });
  }
  g_state->render_creates.fetch_add(1);
  if (op.outcome.sync_error_code != 0) {
    note_sync_failure(user);
    return error_result(op.outcome.sync_error_code);
  }
  return ok_result();
}

QarResult fake_render_sender_subscribe_requests(
  QarSession *, qar_render_sender_request_callback_t callback, void * user, QarCancelToken * token)
{
  record("render_sender_subscribe_requests");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("render_sender_subscribe_requests");
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  FakeQarApi::State::RenderRequestSubscription sub;
  sub.token = reinterpret_cast<FakeToken *>(token);
  sub.callback = callback;
  sub.user = user;
  g_state->render_request_subs.push_back(sub);
  return ok_result();
}

QarResult fake_render_request_get_target_peer_id(
  QarRenderStreamRequest * request, QarPeerId * out)
{
  *out = reinterpret_cast<RenderRequestObject *>(request)->peer;
  return ok_result();
}

QarResult fake_render_request_get_stream_id(QarRenderStreamRequest * request, QarStreamId * out)
{
  *out = reinterpret_cast<RenderRequestObject *>(request)->stream;
  return ok_result();
}

void fake_render_request_handle_destroy(QarRenderStreamRequest *)
{
  g_state->render_requests_released.fetch_add(1);
}

QarResult fake_render_sender_stream_id(QarRenderSender * stream, QarStreamId * out)
{
  record("render_sender_stream_id");
  const int forced = forced_code("render_sender_stream_id");
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  if (g_state->render_senders.count(stream) == 0) {
    return error_result(QAR_STATUS_UNCLASSIFIED);
  }
  *out = reinterpret_cast<RenderSenderObject *>(stream)->stream;
  return ok_result();
}

QarResult fake_render_sender_try_begin_frame(
  QarRenderSender *, QarRenderFrameInfo ** out_info, bool * out_ready)
{
  record("render_sender_try_begin_frame");
  frame_gate("render_sender_try_begin_frame");
  RenderFrameScript script;
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    script = g_state->render_frame;
  }
  if (out_info != nullptr) {
    *out_info = nullptr;
  }
  if (out_ready != nullptr) {
    *out_ready = false;
  }
  if (script.error_code != 0) {
    return error_result(script.error_code);
  }
  if (script.ready && out_info != nullptr && out_ready != nullptr) {
    *out_info = reinterpret_cast<QarRenderFrameInfo *>(new RenderFrameInfoObject());
    g_state->live_render_infos.fetch_add(1);
    *out_ready = true;
  }
  return ok_result();
}

void fake_render_frame_info_handle_destroy(QarRenderFrameInfo * handle)
{
  record("render_frame_info_handle_destroy");
  if (handle != nullptr) {
    delete reinterpret_cast<RenderFrameInfoObject *>(handle);
    g_state->live_render_infos.fetch_sub(1);
  }
}

QarResult fake_render_frame_info_get_view_pose(QarRenderFrameInfo *, size_t, QarPose * out_pose)
{
  record("render_frame_info_get_view_pose");
  const int forced = forced_code("render_frame_info_get_view_pose");
  if (forced != 0) {
    return error_result(forced);
  }
  *out_pose = QarPose{};
  out_pose->orientation.w = 1.0f;
  return ok_result();
}

QarResult fake_render_frame_info_get_view_fov(QarRenderFrameInfo *, size_t, QarFov * out_fov)
{
  record("render_frame_info_get_view_fov");
  const int forced = forced_code("render_frame_info_get_view_fov");
  if (forced != 0) {
    return error_result(forced);
  }
  out_fov->angle_left = -0.5f;
  out_fov->angle_right = 0.5f;
  out_fov->angle_up = 0.5f;
  out_fov->angle_down = -0.5f;
  return ok_result();
}

/// The layout a render sender settled on: two colour and two depth views of 64 x 64 pixels, one
/// texture each.
QarResult fake_render_sender_layout(QarRenderSender *, QarVideoFrameLayout * out)
{
  record("render_sender_layout");
  const int forced = forced_code("render_sender_layout");
  if (forced != 0) {
    return error_result(forced);
  }
  *out = QarVideoFrameLayout{};
  for (int i = 0; i < 4; ++i) {
    QarTextureSize & size = out->textures[i];
    size.format = i < 2 ? QAR_PIXEL_FORMAT_R8G8B8A8 : QAR_PIXEL_FORMAT_R32_FLOAT;
    size.width = 64;
    size.height = 64;
    size.array_layers = 1;
    QarVideoFrameView & view = out->views[i];
    view.end_x = 64;
    view.end_y = 64;
    view.texture_index = static_cast<uint32_t>(i);
    view.texture_format = size.format;
    view.data_type = i < 2 ? QAR_VIDEO_FRAME_VIEW_TYPE_COLOR : QAR_VIDEO_FRAME_VIEW_TYPE_DEPTH;
    view.eye = (i % 2) == 0 ? QAR_VIDEO_FRAME_VIEW_EYE_LEFT : QAR_VIDEO_FRAME_VIEW_EYE_RIGHT;
  }
  out->textures_count = 4;
  out->views_count = 4;
  return ok_result();
}

QarResult fake_render_sender_frame_opengl(QarRenderSender *, QarVideoFrameOpenGl *)
{
  record("render_sender_frame_opengl");
  frame_gate("render_sender_frame_opengl");
  const int forced = forced_code("render_sender_frame_opengl");
  return forced == 0 ? ok_result() : error_result(forced);
}

QarResult fake_render_sender_show_frame(QarRenderSender *, const QarRenderFrameShow *)
{
  record("render_sender_show_frame");
  frame_gate("render_sender_show_frame");
  const int forced = forced_code("render_sender_show_frame");
  if (forced != 0) {
    return error_result(forced);
  }
  g_state->render_frames_shown.fetch_add(1);
  return ok_result();
}

QarResult fake_render_sender_last_hands(QarRenderSender *, QarDeviceHandsWithJoints * out_hands)
{
  record("render_sender_last_hands");
  frame_gate("render_sender_last_hands");
  const int forced = forced_code("render_sender_last_hands");
  if (forced != 0) {
    return error_result(forced);
  }
  *out_hands = QarDeviceHandsWithJoints{};
  return ok_result();
}

// --- GUI panels --------------------------------------------------------------------------------

struct PanelHandleObject
{
  FakeQarApi::State::PanelRecord record;
};

QarGuiPanelId panel_id_for(const std::string & name)
{
  QarGuiPanelId id{};
  std::uint32_t hash = 2166136261u;
  for (int i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    for (char c : name) {
      hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619u;
    }
    hash = (hash ^ static_cast<std::uint32_t>(i + 1)) * 16777619u;
    id.data[i] = static_cast<std::uint8_t>(hash >> 24);
  }
  return id;
}

FakeQarApi::State::PanelRecord * find_panel(const QarGuiPanelId * id)
{
  for (auto & panel : g_state->panels) {
    if (std::memcmp(panel.id.data, id->data, sizeof(id->data)) == 0) {
      return &panel;
    }
  }
  return nullptr;
}

QarResult fake_gui_panels_get_or_create(
  QarSession *, const QarGuiPanelInit * init, QarGuiPanelId * out)
{
  record("gui_panels_get_or_create");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("gui_panels_get_or_create");
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  const std::string name = init->common_name != nullptr ? init->common_name : "";
  const QarGuiPanelId id = panel_id_for(name);
  if (FakeQarApi::State::PanelRecord * existing = find_panel(&id)) {
    *out = existing->id;
    return ok_result();
  }
  FakeQarApi::State::PanelRecord panel;
  panel.id = id;
  panel.info.common_name = name;
  panel.info.size = init->size;
  panel.info.pose = init->pose;
  for (const auto * header = static_cast<const QarStructureHeader *>(init->header.next);
    header != nullptr; header = static_cast<const QarStructureHeader *>(header->next))
  {
    if (header->type == QAR_STRUCTURE_TYPE_GUI_PANEL_STREAM_ID_EXT) {
      panel.info.has_stream = true;
      panel.info.stream = reinterpret_cast<const QarGuiPanelStreamIdExt *>(header)->stream_id;
    } else if (header->type == QAR_STRUCTURE_TYPE_GUI_PANEL_URL_EXT) {
      panel.info.is_url = true;
      const char * url = reinterpret_cast<const QarGuiPanelUrlExt *>(header)->url;
      panel.info.uri = url != nullptr ? url : "";
    }
  }
  if (!panel.info.has_stream) {
    panel.info.is_url = true;
  }
  g_state->panels.push_back(panel);
  *out = id;
  return ok_result();
}

template<typename Change>
QarResult change_panel(const char * name, const QarGuiPanelId * id, Change change)
{
  record(name);
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code(name);
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  FakeQarApi::State::PanelRecord * panel = find_panel(id);
  if (panel == nullptr) {
    return error_result(QAR_STATUS_LOGIC_ERROR);
  }
  change(*panel);
  return ok_result();
}

QarResult fake_gui_panels_update_pose(QarSession *, const QarGuiPanelId * id, const QarPose * pose)
{
  return change_panel("gui_panels_update_pose", id,
      [pose](FakeQarApi::State::PanelRecord & panel) { panel.info.pose = *pose; });
}

QarResult fake_gui_panels_change_size(
  QarSession *, const QarGuiPanelId * id, const QarGuiPanelSize * size)
{
  return change_panel("gui_panels_change_size", id,
      [size](FakeQarApi::State::PanelRecord & panel) { panel.info.size = *size; });
}

QarResult fake_gui_panels_set_state(QarSession *, const QarGuiPanelId * id, QarGuiPanelState state)
{
  return change_panel("gui_panels_set_state", id,
      [state](FakeQarApi::State::PanelRecord & panel) { panel.info.state = state; });
}

QarResult fake_gui_panels_close_panel(QarSession *, const QarGuiPanelId * id)
{
  return change_panel("gui_panels_close_panel", id,
      [](FakeQarApi::State::PanelRecord & panel) {
        panel.info.state = QAR_GUI_PANEL_STATE_CLOSED;
      });
}

QarResult fake_gui_panels_navigate_to_uri(
  QarSession *, const QarGuiPanelId * id, const char * uri)
{
  return change_panel("gui_panels_navigate_to_uri", id,
      [uri](FakeQarApi::State::PanelRecord & panel) {
        panel.info.is_url = true;
        panel.info.has_stream = false;
        panel.info.uri = uri != nullptr ? uri : "";
      });
}

QarResult fake_gui_panels_set_stream(
  QarSession *, const QarGuiPanelId * id, const QarStreamId * stream)
{
  return change_panel("gui_panels_set_stream", id,
      [stream](FakeQarApi::State::PanelRecord & panel) {
        panel.info.is_url = false;
        panel.info.has_stream = true;
        panel.info.stream = *stream;
      });
}

QarResult fake_gui_panels_update_visible_to(
  QarSession *, const QarGuiPanelId * id, const QarPeerId **, size_t, const QarPeerId **, size_t)
{
  return change_panel("gui_panels_update_visible_to", id,
      [](FakeQarApi::State::PanelRecord &) {});
}

QarResult fake_gui_panels_subscribe_all_gesture_events(
  QarSession *, const QarGuiPanelId *, qar_gui_panel_gesture_event_callback_t, void *,
  QarCancelToken * token)
{
  record("gui_panels_subscribe_all_gesture_events");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("gui_panels_subscribe_all_gesture_events");
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  FakeQarApi::State::PanelGestureSubscription sub;
  sub.token = reinterpret_cast<FakeToken *>(token);
  g_state->panel_gesture_subs.push_back(sub);
  return ok_result();
}

QarResult fake_gui_panels_subscribe_panel_updates(
  QarSession *, const QarGuiPanelId * id, qar_gui_panel_update_callback_t callback, void * user,
  QarCancelToken * token)
{
  record("gui_panels_subscribe_panel_updates");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("gui_panels_subscribe_panel_updates");
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  FakeQarApi::State::PanelSubscription sub;
  sub.token = reinterpret_cast<FakeToken *>(token);
  sub.callback = callback;
  sub.user = user;
  sub.id = *id;
  g_state->panel_subs.push_back(sub);
  return ok_result();
}

QarResult fake_query_gui_panels_count(QarSession *, size_t * count)
{
  record("query_gui_panels_count");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("query_gui_panels_count");
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  *count = g_state->panels.size();
  return ok_result();
}

QarResult fake_query_gui_panels(
  QarSession *, QarGuiPanel ** handles, size_t capacity, size_t * written)
{
  record("query_gui_panels");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  const int forced = forced_code("query_gui_panels");
  if (forced != 0) {
    return error_result(forced);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  size_t n = 0;
  for (; n < g_state->panels.size() && n < capacity; ++n) {
    auto * object = new PanelHandleObject{g_state->panels[n]};
    handles[n] = reinterpret_cast<QarGuiPanel *>(object);
  }
  *written = n;
  return ok_result();
}

PanelHandleObject * as_panel(QarGuiPanel * handle)
{
  return reinterpret_cast<PanelHandleObject *>(handle);
}

void fake_gui_panel_handle_destroy(QarGuiPanel * handle)
{
  delete as_panel(handle);
}

QarResult fake_gui_panel_get_id(QarGuiPanel * handle, QarGuiPanelId * out)
{
  *out = as_panel(handle)->record.id;
  return ok_result();
}

QarResult fake_gui_panel_get_size(QarGuiPanel * handle, QarGuiPanelSize * out)
{
  *out = as_panel(handle)->record.info.size;
  return ok_result();
}

QarResult fake_gui_panel_get_pose(QarGuiPanel * handle, QarPose * out)
{
  *out = as_panel(handle)->record.info.pose;
  return ok_result();
}

QarResult fake_gui_panel_get_state(QarGuiPanel * handle, QarGuiPanelState * out)
{
  *out = as_panel(handle)->record.info.state;
  return ok_result();
}

QarResult fake_gui_panel_get_content_type(QarGuiPanel * handle, QarGuiPanelContentType * out)
{
  *out = as_panel(handle)->record.info.is_url ? QAR_GUI_PANEL_CONTENT_TYPE_WEBSITE_URI :
    QAR_GUI_PANEL_CONTENT_TYPE_STREAM_ID;
  return ok_result();
}

QarResult fake_gui_panel_get_content_stream_id(QarGuiPanel * handle, QarStreamId * out)
{
  *out = as_panel(handle)->record.info.stream;
  return ok_result();
}

QarResult fake_gui_panel_get_content_uri(QarGuiPanel * handle, char * out, size_t size)
{
  return copy_text(as_panel(handle)->record.info.uri, out, size);
}

QarResult fake_gui_panel_get_visible_to_peers_count(QarGuiPanel *, size_t * count)
{
  *count = 0;
  return ok_result();
}

QarResult fake_gui_panel_get_visible_to_peers(QarGuiPanel *, QarPeerId *, size_t, size_t * written)
{
  *written = 0;
  return ok_result();
}

// --- video senders -----------------------------------------------------------------------------

QarResult fake_video_sender_create(
  QarSession *, const QarVideoSenderInit * init, QarVideoSender ** out)
{
  record("video_sender_create");
  if (const int gone = hub_gate()) {
    *out = nullptr;
    return error_result(gone);
  }
  const int forced = forced_code("video_sender_create");
  if (forced != 0) {
    *out = nullptr;
    return error_result(forced);
  }
  auto * sender = new FakeQarApi::State::SenderObject();
  const int n = g_state->next_sender.fetch_add(1);
  for (int i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    sender->stream.data[i] = static_cast<std::uint8_t>(0xC0 + n + i);
  }
  sender->width = init->layout.textures[0].width;
  sender->height = init->layout.textures[0].height;
  sender->buffer.assign(static_cast<std::size_t>(sender->width) * sender->height * 4u, 0);
  g_state->live_senders.fetch_add(1);
  *out = reinterpret_cast<QarVideoSender *>(sender);
  return ok_result();
}

QarResult fake_video_sender_stream_id(QarVideoSender * handle, QarStreamId * out)
{
  *out = reinterpret_cast<FakeQarApi::State::SenderObject *>(handle)->stream;
  return ok_result();
}

QarResult fake_video_sender_try_begin_frame(QarVideoSender *, bool * out_ready)
{
  record("video_sender_try_begin_frame");
  frame_gate("video_sender_try_begin_frame");
  std::lock_guard<std::mutex> lock(g_state->mutex);
  *out_ready = g_state->sender_ready;
  return ok_result();
}

QarResult fake_video_sender_frame_cpu(QarVideoSender * handle, QarVideoFrameCpu * out)
{
  record("video_sender_frame_cpu");
  frame_gate("video_sender_frame_cpu");
  auto * sender = reinterpret_cast<FakeQarApi::State::SenderObject *>(handle);
  out->textures_count = 1;
  out->textures[0].size.width = sender->width;
  out->textures[0].size.height = sender->height;
  out->textures[0].pitch = sender->width * 4u;
  out->textures[0].texture_data = sender->buffer.data();
  out->textures[0].texture_data_size = sender->buffer.size();
  return ok_result();
}

QarResult fake_video_sender_show_frame(QarVideoSender *)
{
  record("video_sender_show_frame");
  frame_gate("video_sender_show_frame");
  g_state->frames_shown.fetch_add(1);
  return ok_result();
}

void fake_video_sender_handle_destroy(QarVideoSender * handle)
{
  record("video_sender_handle_destroy");
  hub_gate();
  delete reinterpret_cast<FakeQarApi::State::SenderObject *>(handle);
  g_state->live_senders.fetch_sub(1);
}

QarResult fake_peer_spec_get_device_id(QarPeerSpec * handle, char * out, size_t size)
{
  return copy_text(as_object(handle)->peer.device_id, out, size);
}

QarResult fake_peer_spec_get_device_family(QarPeerSpec * handle, char * out, size_t size)
{
  return copy_text(as_object(handle)->peer.device_family, out, size);
}

QarResult fake_peer_spec_get_device_name(QarPeerSpec * handle, char * out, size_t size)
{
  return copy_text(as_object(handle)->peer.device_name, out, size);
}

QarResult fake_peer_spec_get_display_name(QarPeerSpec * handle, char * out, size_t size)
{
  return copy_text(as_object(handle)->peer.display_name, out, size);
}

void fake_peer_spec_handle_destroy(QarPeerSpec * handle)
{
  record("peer_spec_handle_destroy");
  HandleObject * object = as_object(handle);
  if (object->borrowed) {
    g_state->borrowed_destroys.fetch_add(1);
    return;
  }
  delete object;
  g_state->owned_outstanding.fetch_sub(1);
}

QarResult fake_session_get_my_spec(const QarSession *, QarPeerSpec ** out)
{
  record("session_get_my_spec");
  if (const int gone = hub_gate()) {
    *out = nullptr;
    return error_result(gone);
  }
  FakePeer self;
  self.id = fake_self_id();
  self.role = QAR_PEER_ROLE_SOURCE;
  self.category = QAR_DEVICE_CATEGORY_DESKTOP_PC;
  *out = new_owned_handle(self);
  return ok_result();
}

QarResult fake_query_peer_specs_count(QarSession *, size_t * count)
{
  record("query_peer_specs_count");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  *count = g_state->initial_peers.size();
  return ok_result();
}

QarResult fake_query_peer_specs(QarSession *, QarPeerSpec ** handles, size_t capacity,
  size_t * written)
{
  record("query_peer_specs");
  if (const int gone = hub_gate()) {
    *written = 0;
    return error_result(gone);
  }
  std::vector<FakePeer> peers;
  {
    std::lock_guard<std::mutex> lock(g_state->mutex);
    peers = g_state->initial_peers;
  }
  size_t n = 0;
  for (; n < peers.size() && n < capacity; ++n) {
    handles[n] = new_owned_handle(peers[n]);
  }
  *written = n;
  return ok_result();
}

QarResult fake_peer_subscribe_updates(
  QarSession *, qar_peer_update_callback_t callback, void * user, QarCancelToken * token)
{
  record("peer_subscribe_updates");
  if (const int gone = hub_gate()) {
    return error_result(gone);
  }
  std::lock_guard<std::mutex> lock(g_state->mutex);
  FakeQarApi::State::Subscription sub;
  sub.token = reinterpret_cast<FakeToken *>(token);
  sub.callback = callback;
  sub.user = user;
  g_state->subscriptions.push_back(sub);
  return ok_result();
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// The loader
// ---------------------------------------------------------------------------------------------

namespace
{

class FakeLoader : public ILibraryLoader
{
public:
  FakeLoader(bool installed, bool load_ok) : installed_(installed), load_ok_(load_ok) {}

  bool load(const std::string &, std::string & out_error) override
  {
    record("library_load");
    if (!load_ok_) {
      out_error = "the fake library is not available";
      return false;
    }
    g_qar_dynamic_library_handle =
      reinterpret_cast<QAR_DLL_HANDLE_TYPE>(&g_state->sentinel);
    return true;
  }

  void unload() override
  {
    record("library_unload");
    g_qar_dynamic_library_handle = nullptr;
  }

  bool installed_bin_folder(std::string & out_utf8) override
  {
    record("installed_bin_folder");
    if (!installed_) {
      return false;
    }
    out_utf8 = "fake-qaros/bin";
    return true;
  }

  std::string loaded_from() override { return "fake-qaros/bin/qar-streaming-c.dll"; }

private:
  bool installed_;
  bool load_ok_;
};

}  // namespace

// ---------------------------------------------------------------------------------------------
// FakeQarApi
// ---------------------------------------------------------------------------------------------

FakePeer make_peer(std::uint8_t tag, QarPeerRole role, QarPeerPresence presence)
{
  FakePeer peer;
  for (int i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    peer.id.data[i] = static_cast<std::uint8_t>(tag);
  }
  peer.role = role;
  peer.presence = presence;
  return peer;
}

QarPeerId fake_self_id()
{
  QarPeerId id{};
  for (int i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    id.data[i] = 0xEE;
  }
  return id;
}

FakeQarApi::FakeQarApi() : state_(std::make_unique<State>())
{
  g_state = state_.get();

  g_qar_result_api.result_is_error = &fake_result_is_error;
  g_qar_result_api.result_is_success = &fake_result_is_success;
  g_qar_result_api.result_has_code = &fake_result_has_code;
  g_qar_result_api.result_message = &fake_result_message;

  g_qar_types_api.uuid_to_string = &fake_uuid_to_string;
  g_qar_types_api.uuid_from_string = &fake_uuid_from_string;

  g_qar_cancelation_token_api.cancel_token_create = &fake_cancel_token_create;
  g_qar_cancelation_token_api.cancel_token_cancel = &fake_cancel_token_cancel;
  g_qar_cancelation_token_api.cancel_token_handle_destroy = &fake_cancel_token_handle_destroy;

  g_qar_runtime_api.library_init = &fake_library_init;
  g_qar_runtime_api.library_destroy = &fake_library_destroy;
  g_qar_runtime_api.runtime_info = &fake_runtime_info;
  g_qar_runtime_api.runtime_create = &fake_runtime_create;
  g_qar_runtime_api.runtime_handle_destroy = &fake_runtime_handle_destroy;

  g_qar_session_api.session_handle_destroy = &fake_session_handle_destroy;

  g_qar_render_stream_sender_api.render_stream_handle_destroy = &fake_render_stream_handle_destroy;
  g_qar_render_stream_sender_api.render_sender_create_async = &fake_render_sender_create_async;
  g_qar_render_stream_sender_api.render_sender_subscribe_requests =
    &fake_render_sender_subscribe_requests;
  g_qar_render_stream_sender_api.render_request_get_target_peer_id =
    &fake_render_request_get_target_peer_id;
  g_qar_render_stream_sender_api.render_request_get_stream_id =
    &fake_render_request_get_stream_id;
  g_qar_render_stream_sender_api.render_request_handle_destroy =
    &fake_render_request_handle_destroy;
  g_qar_render_stream_sender_api.render_sender_stream_id = &fake_render_sender_stream_id;
  g_qar_render_stream_sender_api.render_sender_try_begin_frame =
    &fake_render_sender_try_begin_frame;
  g_qar_render_stream_sender_api.render_frame_info_handle_destroy =
    &fake_render_frame_info_handle_destroy;
  g_qar_render_stream_sender_api.render_frame_info_get_view_pose =
    &fake_render_frame_info_get_view_pose;
  g_qar_render_stream_sender_api.render_frame_info_get_view_fov =
    &fake_render_frame_info_get_view_fov;
  g_qar_render_stream_sender_api.render_sender_layout = &fake_render_sender_layout;
  g_qar_render_stream_sender_api.render_sender_frame_opengl = &fake_render_sender_frame_opengl;
  g_qar_render_stream_sender_api.render_sender_show_frame = &fake_render_sender_show_frame;
  g_qar_render_stream_sender_api.render_sender_last_hands = &fake_render_sender_last_hands;

  g_qar_onboarding_api.runtime_onboard_async = &fake_runtime_onboard_async;

  g_qar_target_apps_api.session_invite_target_app_async = &fake_session_invite_target_app_async;

  g_qar_app_volumes_api.app_volumes_get_or_create = &fake_app_volumes_get_or_create;
  g_qar_app_volumes_api.query_app_volumes_count = &fake_query_app_volumes_count;
  g_qar_app_volumes_api.query_app_volumes = &fake_query_app_volumes;
  g_qar_app_volumes_api.app_volume_get_id = &fake_app_volume_get_id;
  g_qar_app_volumes_api.app_volume_get_pose = &fake_app_volume_get_pose;
  g_qar_app_volumes_api.app_volume_get_app_pose = &fake_app_volume_get_app_pose;
  g_qar_app_volumes_api.app_volume_get_app_scale = &fake_app_volume_get_app_scale;
  g_qar_app_volumes_api.app_volume_handle_destroy = &fake_app_volume_handle_destroy;
  g_qar_app_volumes_api.app_volumes_subscribe_updates = &fake_app_volumes_subscribe_updates;
  g_qar_app_volumes_api.app_volume_get_latest_pose = &fake_app_volume_get_latest_pose;
  g_qar_app_volumes_api.app_volume_get_latest_app_pose = &fake_app_volume_get_latest_app_pose;
  g_qar_app_volumes_api.app_volume_get_latest_app_scale = &fake_app_volume_get_latest_app_scale;
  g_qar_app_volumes_api.app_volumes_change_app_world_anchor =
    &fake_app_volumes_change_app_world_anchor;
  g_qar_app_volumes_api.app_volumes_clear_app_world_anchor =
    &fake_app_volumes_clear_app_world_anchor;
  g_qar_app_volumes_api.app_volumes_subscribe_all_gesture_events =
    &fake_app_volumes_subscribe_all_gesture_events;
  g_qar_app_volumes_api.app_volumes_subscribe_gesture_events =
    &fake_app_volumes_subscribe_gesture_events;
  g_qar_app_volumes_api.app_volume_gesture_get_lifecycle = &fake_app_volume_gesture_get_lifecycle;
  g_qar_app_volumes_api.app_volume_gesture_get_classification =
    &fake_app_volume_gesture_get_classification;
  g_qar_app_volumes_api.app_volume_gesture_get_points = &fake_app_volume_gesture_get_points;
  g_qar_app_volumes_api.app_volume_gesture_get_timestamp = &fake_app_volume_gesture_get_timestamp;

  g_qar_gui_panels_api.gui_panels_get_or_create = &fake_gui_panels_get_or_create;
  g_qar_gui_panels_api.gui_panels_update_pose = &fake_gui_panels_update_pose;
  g_qar_gui_panels_api.gui_panels_change_size = &fake_gui_panels_change_size;
  g_qar_gui_panels_api.gui_panels_set_state = &fake_gui_panels_set_state;
  g_qar_gui_panels_api.gui_panels_close_panel = &fake_gui_panels_close_panel;
  g_qar_gui_panels_api.gui_panels_navigate_to_uri = &fake_gui_panels_navigate_to_uri;
  g_qar_gui_panels_api.gui_panels_set_stream = &fake_gui_panels_set_stream;
  g_qar_gui_panels_api.gui_panels_update_visible_to = &fake_gui_panels_update_visible_to;
  g_qar_gui_panels_api.gui_panels_subscribe_all_gesture_events =
    &fake_gui_panels_subscribe_all_gesture_events;
  g_qar_gui_panels_api.gui_panels_subscribe_panel_updates =
    &fake_gui_panels_subscribe_panel_updates;
  g_qar_gui_panels_api.query_gui_panels_count = &fake_query_gui_panels_count;
  g_qar_gui_panels_api.query_gui_panels = &fake_query_gui_panels;
  g_qar_gui_panels_api.gui_panel_handle_destroy = &fake_gui_panel_handle_destroy;
  g_qar_gui_panels_api.gui_panel_get_id = &fake_gui_panel_get_id;
  g_qar_gui_panels_api.gui_panel_get_size = &fake_gui_panel_get_size;
  g_qar_gui_panels_api.gui_panel_get_pose = &fake_gui_panel_get_pose;
  g_qar_gui_panels_api.gui_panel_get_state = &fake_gui_panel_get_state;
  g_qar_gui_panels_api.gui_panel_get_content_type = &fake_gui_panel_get_content_type;
  g_qar_gui_panels_api.gui_panel_get_content_stream_id = &fake_gui_panel_get_content_stream_id;
  g_qar_gui_panels_api.gui_panel_get_content_uri = &fake_gui_panel_get_content_uri;
  g_qar_gui_panels_api.gui_panel_get_visible_to_peers_count =
    &fake_gui_panel_get_visible_to_peers_count;
  g_qar_gui_panels_api.gui_panel_get_visible_to_peers = &fake_gui_panel_get_visible_to_peers;

  g_qar_video_sender_api.video_sender_create = &fake_video_sender_create;
  g_qar_video_sender_api.video_sender_stream_id = &fake_video_sender_stream_id;
  g_qar_video_sender_api.video_sender_try_begin_frame = &fake_video_sender_try_begin_frame;
  g_qar_video_sender_api.video_sender_frame_cpu = &fake_video_sender_frame_cpu;
  g_qar_video_sender_api.video_sender_show_frame = &fake_video_sender_show_frame;
  g_qar_video_sender_api.video_sender_handle_destroy = &fake_video_sender_handle_destroy;

  g_qar_peer_management_api.peer_spec_get_id = &fake_peer_spec_get_id;
  g_qar_peer_management_api.peer_spec_get_role = &fake_peer_spec_get_role;
  g_qar_peer_management_api.peer_spec_get_presence = &fake_peer_spec_get_presence;
  g_qar_peer_management_api.peer_spec_get_category = &fake_peer_spec_get_category;
  g_qar_peer_management_api.peer_spec_get_device_id = &fake_peer_spec_get_device_id;
  g_qar_peer_management_api.peer_spec_get_device_family = &fake_peer_spec_get_device_family;
  g_qar_peer_management_api.peer_spec_get_device_name = &fake_peer_spec_get_device_name;
  g_qar_peer_management_api.peer_spec_get_display_name = &fake_peer_spec_get_display_name;
  g_qar_peer_management_api.peer_spec_handle_destroy = &fake_peer_spec_handle_destroy;
  g_qar_peer_management_api.session_get_my_spec = &fake_session_get_my_spec;
  g_qar_peer_management_api.query_peer_specs_count = &fake_query_peer_specs_count;
  g_qar_peer_management_api.query_peer_specs = &fake_query_peer_specs;
  g_qar_peer_management_api.peer_subscribe_updates = &fake_peer_subscribe_updates;
}

FakeQarApi::~FakeQarApi()
{
  std::vector<std::thread> threads;
  std::vector<std::thread> detached;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->shutting_down = true;
    threads.swap(state_->threads);
    detached.swap(state_->detached);
  }
  state_->cv.notify_all();
  {
    // The calls that wait in a blocking mode leave their wait and fail; a test releases them first
    // all the same, because what they do after the wait reads this state.
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->cv.wait_for(lock, std::chrono::seconds(5), [this] {
        return state_->hub_blocked == 0 && state_->frame_blocked == 0;
      });
  }
  for (std::thread & thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  for (std::thread & thread : detached) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  g_qar_result_api = QarResultLoadedApi{};
  g_qar_types_api = QarTypesLoadedApi{};
  g_qar_cancelation_token_api = QarCancelationTokenLoadedApi{};
  g_qar_runtime_api = QarRuntimeLoadedApi{};
  g_qar_session_api = QarSessionLoadedApi{};
  g_qar_render_stream_sender_api = QarRenderStreamSenderLoadedApi{};
  g_qar_onboarding_api = QarOnboardingLoadedApi{};
  g_qar_target_apps_api = QarTargetAppsLoadedApi{};
  g_qar_peer_management_api = QarPeerManagementLoadedApi{};
  g_qar_app_volumes_api = QarAppVolumesLoadedApi{};
  g_qar_gui_panels_api = QarGuiPanelsLoadedApi{};
  g_qar_video_sender_api = QarVideoSenderLoadedApi{};
  g_qar_dynamic_library_handle = nullptr;
  g_state = nullptr;
}

void FakeQarApi::script_onboard(const Outcome & outcome)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->onboard_script.push_back(outcome);
}

void FakeQarApi::script_invite(const Outcome & outcome)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->invite_script.push_back(outcome);
}

void FakeQarApi::set_library_init_code(int code) { state_->library_init_code = code; }

void FakeQarApi::set_runtime_create_code(int code) { state_->runtime_create_code = code; }

void FakeQarApi::set_volume_code(int code) { state_->volume_code = code; }

void FakeQarApi::set_initial_peers(std::vector<FakePeer> peers)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->initial_peers = std::move(peers);
}

void FakeQarApi::set_call_hook(std::function<void(const std::string &)> hook)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->hook = std::move(hook);
}

void FakeQarApi::set_call_failure(const std::string & name, int code)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (code == 0) {
    state_->failures.erase(name);
  } else {
    state_->failures[name] = code;
  }
}

void FakeQarApi::set_volume_latest_code(int code)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->latest_code = code;
}

void FakeQarApi::set_volume_placement(const QarPose & pose, const QarPose & app_pose, float scale)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->volume_pose = pose;
  state_->volume_app_pose = app_pose;
  state_->volume_scale = scale;
}

void FakeQarApi::set_video_sender_ready(bool ready)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->sender_ready = ready;
}

void FakeQarApi::set_sync_failure_hook(std::function<void(const void *)> hook)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->sync_failure_hook = std::move(hook);
}

void FakeQarApi::script_render_create(const Outcome & outcome)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->render_script.push_back(outcome);
}

void FakeQarApi::set_render_frame(const RenderFrameScript & script)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->render_frame = script;
}

void FakeQarApi::set_hub_gone(HubGone mode, int code)
{
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->hub_gone = mode;
    state_->hub_code = code;
  }
  state_->cv.notify_all();
}

void FakeQarApi::set_frame_block(bool block)
{
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->frame_block = block;
  }
  state_->cv.notify_all();
}

void FakeQarApi::set_frame_delay_ms(int ms, const std::string & call_name)
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (call_name.empty()) {
    state_->frame_delay_all = ms;
  } else {
    state_->frame_delay[call_name] = ms;
  }
}

namespace
{

// Marks the active peer subscriptions as running a callback and returns them.
std::vector<FakeQarApi::State::Subscription *> begin_peer_deliveries(FakeQarApi::State & state)
{
  std::vector<FakeQarApi::State::Subscription *> targets;
  std::lock_guard<std::mutex> lock(state.mutex);
  for (auto & sub : state.subscriptions) {
    if (sub.active) {
      ++sub.running;
      targets.push_back(&sub);
    }
  }
  return targets;
}

void run_peer_deliveries(
  FakeQarApi::State & state, const std::vector<FakeQarApi::State::Subscription *> & targets,
  const FakePeer & peer)
{
  for (FakeQarApi::State::Subscription * sub : targets) {
    HandleObject object{peer, true};
    sub->callback(as_handle(&object), sub->user);
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      --sub->running;
    }
    state.cv.notify_all();
  }
}

// Marks the gesture subscriptions that want @p kind as running a callback and returns them.
std::vector<FakeQarApi::State::GestureSubscription *> begin_gesture_deliveries(
  FakeQarApi::State & state, QarGestureKind kind)
{
  std::vector<FakeQarApi::State::GestureSubscription *> targets;
  std::lock_guard<std::mutex> lock(state.mutex);
  for (auto & sub : state.gesture_subs) {
    if (sub.active && (sub.kind < 0 || sub.kind == static_cast<int>(kind))) {
      ++sub.running;
      targets.push_back(&sub);
    }
  }
  return targets;
}

void run_gesture_deliveries(
  FakeQarApi::State & state, const std::vector<FakeQarApi::State::GestureSubscription *> & targets,
  const FakeGesture & gesture)
{
  GestureObject object;
  object.gesture = gesture;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    object.volume = state.volume_id;
  }
  for (FakeQarApi::State::GestureSubscription * sub : targets) {
    sub->callback(reinterpret_cast<QarAppVolumeGesture *>(&object), sub->user);
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      --sub->running;
    }
    state.cv.notify_all();
  }
}

// Runs @p work on a thread the test does not wait for; wait_detached_deliveries() sees it end.
void run_detached(FakeQarApi::State & state, std::function<void()> work)
{
  std::lock_guard<std::mutex> lock(state.mutex);
  ++state.detached_running;
  state.detached.emplace_back([&state, work = std::move(work)] {
      work();
      {
        std::lock_guard<std::mutex> inner(state.mutex);
        --state.detached_running;
      }
      state.cv.notify_all();
    });
}

}  // namespace

void FakeQarApi::deliver_peer(const FakePeer & peer)
{
  const auto targets = begin_peer_deliveries(*state_);
  std::thread library_thread([this, &targets, &peer] {
      run_peer_deliveries(*state_, targets, peer);
    });
  library_thread.join();
}

void FakeQarApi::deliver_peer_detached(const FakePeer & peer)
{
  const auto targets = begin_peer_deliveries(*state_);
  run_detached(*state_, [this, targets, peer] { run_peer_deliveries(*state_, targets, peer); });
}

void FakeQarApi::deliver_volume_gesture(const FakeGesture & gesture)
{
  const auto targets = begin_gesture_deliveries(*state_, gesture.kind);
  std::thread library_thread([this, &targets, &gesture] {
      run_gesture_deliveries(*state_, targets, gesture);
    });
  library_thread.join();
}

void FakeQarApi::deliver_render_request(const QarPeerId & peer, const QarStreamId & stream)
{
  std::vector<State::RenderRequestSubscription *> targets;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (auto & sub : state_->render_request_subs) {
      if (sub.active) {
        ++sub.running;
        targets.push_back(&sub);
      }
    }
  }
  std::thread library_thread([this, &targets, &peer, &stream] {
      for (State::RenderRequestSubscription * sub : targets) {
        RenderRequestObject object;
        object.peer = peer;
        object.stream = stream;
        sub->callback(reinterpret_cast<QarRenderStreamRequest *>(&object), sub->user);
        {
          std::lock_guard<std::mutex> lock(state_->mutex);
          --sub->running;
        }
        state_->cv.notify_all();
      }
    });
  library_thread.join();
}

QarStreamId FakeQarApi::render_stream_id(int n)
{
  QarStreamId id{};
  std::memset(id.data, render_stream_tag(n), sizeof(id.data));
  return id;
}

int FakeQarApi::render_requests_released() const { return state_->render_requests_released.load(); }

void FakeQarApi::deliver_volume_gesture_detached(const FakeGesture & gesture)
{
  const auto targets = begin_gesture_deliveries(*state_, gesture.kind);
  run_detached(*state_, [this, targets, gesture] {
      run_gesture_deliveries(*state_, targets, gesture);
    });
}

bool FakeQarApi::wait_detached_deliveries(int timeout_ms)
{
  std::unique_lock<std::mutex> lock(state_->mutex);
  return state_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
      return state_->detached_running == 0;
    });
}

void FakeQarApi::deliver_volume_update()
{
  std::thread library_thread([this] {
      std::vector<State::VolumeSubscription *> targets;
      QarAppVolumeId id{};
      QarPose pose{};
      QarPose app_pose{};
      float scale = 1.0f;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        id = state_->volume_id;
        pose = state_->volume_pose;
        app_pose = state_->volume_app_pose;
        scale = state_->volume_scale;
        for (auto & sub : state_->volume_subs) {
          if (sub.active) {
            ++sub.running;
            targets.push_back(&sub);
          }
        }
      }
      for (State::VolumeSubscription * sub : targets) {
        VolumeObject object;
        object.id = id;
        object.pose = pose;
        object.app_pose = app_pose;
        object.scale = scale;
        sub->callback(reinterpret_cast<QarAppVolume *>(&object), sub->user);
        {
          std::lock_guard<std::mutex> lock(state_->mutex);
          --sub->running;
        }
        state_->cv.notify_all();
      }
    });
  library_thread.join();
}

int FakeQarApi::panel_subscriptions() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return static_cast<int>(std::count_if(state_->panel_subs.begin(), state_->panel_subs.end(),
    [](const State::PanelSubscription & sub) { return sub.active; }));
}

void FakeQarApi::deliver_panel_state(const std::string & common_name, QarGuiPanelState state)
{
  std::thread library_thread([this, &common_name, state] {
      std::vector<State::PanelSubscription *> targets;
      State::PanelRecord record;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const State::PanelRecord * found = nullptr;
        for (const State::PanelRecord & panel : state_->panels) {
          if (panel.info.common_name == common_name) {
            found = &panel;
          }
        }
        if (found == nullptr) {
          return;
        }
        record = *found;
        for (auto & sub : state_->panel_subs) {
          if (sub.active && std::memcmp(sub.id.data, record.id.data, sizeof(record.id.data)) == 0) {
            ++sub.running;
            targets.push_back(&sub);
          }
        }
      }
      // The report names the state the test scripts, whatever the fake holds for the panel.
      record.info.state = state;
      for (State::PanelSubscription * sub : targets) {
        PanelHandleObject object{record};
        sub->callback(reinterpret_cast<QarGuiPanel *>(&object), sub->user);
        {
          std::lock_guard<std::mutex> lock(state_->mutex);
          --sub->running;
        }
        state_->cv.notify_all();
      }
    });
  library_thread.join();
}

void FakeQarApi::release_pending()
{
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    ++state_->release_epoch;
  }
  state_->cv.notify_all();
}

std::vector<Call> FakeQarApi::calls() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->calls;
}

int FakeQarApi::count(const std::string & name) const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return static_cast<int>(std::count_if(state_->calls.begin(), state_->calls.end(),
    [&name](const Call & call) { return call.name == name; }));
}

int FakeQarApi::first_index(const std::string & name) const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (std::size_t i = 0; i < state_->calls.size(); ++i) {
    if (state_->calls[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int FakeQarApi::last_index(const std::string & name) const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (std::size_t i = state_->calls.size(); i > 0; --i) {
    if (state_->calls[i - 1].name == name) {
      return static_cast<int>(i - 1);
    }
  }
  return -1;
}

std::vector<OnboardRequest> FakeQarApi::onboard_requests() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->onboard_requests;
}

std::vector<std::string> FakeQarApi::invite_requests() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->invite_requests;
}

std::vector<VolumeRequest> FakeQarApi::volume_requests() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->volume_requests;
}

bool FakeQarApi::subscription_active() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return std::any_of(state_->subscriptions.begin(), state_->subscriptions.end(),
    [](const State::Subscription & sub) { return sub.active; });
}

int FakeQarApi::volume_subscriptions() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return static_cast<int>(std::count_if(state_->volume_subs.begin(), state_->volume_subs.end(),
    [](const State::VolumeSubscription & sub) { return sub.active; }));
}

std::vector<PanelInfo> FakeQarApi::panels() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  std::vector<PanelInfo> out;
  for (const State::PanelRecord & panel : state_->panels) {
    out.push_back(panel.info);
  }
  return out;
}

int FakeQarApi::live_video_senders() const { return state_->live_senders.load(); }
int FakeQarApi::video_frames_shown() const { return state_->frames_shown.load(); }

int FakeQarApi::borrowed_handle_destroys() const { return state_->borrowed_destroys.load(); }
int FakeQarApi::owned_handles_outstanding() const { return state_->owned_outstanding.load(); }
int FakeQarApi::live_sessions() const { return state_->live_sessions.load(); }
int FakeQarApi::onboard_results_fired() const { return state_->onboard_results.load(); }
int FakeQarApi::invite_results_fired() const { return state_->invite_results.load(); }

int FakeQarApi::render_streams_destroyed() const
{
  return state_->render_streams_destroyed.load();
}
int FakeQarApi::live_tokens() const { return state_->live_tokens.load(); }
int FakeQarApi::live_render_senders() const { return state_->live_render_senders.load(); }
int FakeQarApi::render_creates() const { return state_->render_creates.load(); }
int FakeQarApi::render_creates_cancelled() const
{
  return state_->render_creates_cancelled.load();
}

int FakeQarApi::render_creates_pending() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return static_cast<int>(std::count_if(
    state_->render_creating.begin(), state_->render_creating.end(),
    [](const std::shared_ptr<RenderCreateState> & creating) { return !creating->answered; }));
}

int FakeQarApi::live_render_frame_infos() const { return state_->live_render_infos.load(); }
int FakeQarApi::render_frames_shown() const { return state_->render_frames_shown.load(); }

int FakeQarApi::gesture_subscriptions() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return static_cast<int>(std::count_if(state_->gesture_subs.begin(), state_->gesture_subs.end(),
    [](const State::GestureSubscription & sub) { return sub.active; }));
}

int FakeQarApi::panel_gesture_subscriptions() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return static_cast<int>(std::count_if(
    state_->panel_gesture_subs.begin(), state_->panel_gesture_subs.end(),
    [](const State::PanelGestureSubscription & sub) { return sub.active; }));
}

int FakeQarApi::live_runtimes() const { return state_->live_runtimes.load(); }
int FakeQarApi::runtime_creates() const { return state_->runtime_creates.load(); }

int FakeQarApi::hub_blocked_calls() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->hub_blocked;
}

int FakeQarApi::frame_calls_blocked() const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->frame_blocked;
}

std::thread::id FakeQarApi::thread_of_last(const std::string & name) const
{
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (std::size_t i = state_->calls.size(); i > 0; --i) {
    if (state_->calls[i - 1].name == name) {
      return state_->calls[i - 1].thread;
    }
  }
  return std::thread::id();
}

std::unique_ptr<ILibraryLoader> FakeQarApi::make_loader(bool installed, bool load_ok)
{
  return std::make_unique<FakeLoader>(installed, load_ok);
}

}  // namespace test
}  // namespace qar_qar_adapter
