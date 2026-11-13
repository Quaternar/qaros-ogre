// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__PANEL_STATE_GUARD_HPP_
#define QAR_QAR_ADAPTER__PANEL_STATE_GUARD_HPP_

/// @file panel_state_guard.hpp
/// @brief Keeps a panel in the state the adapter asked for, against a QarOS that reports the
/// opposite right after the adapter set it.
///
/// A panel that an earlier session left behind in the state HIDDEN can be reported as HIDDEN
/// again by the runtime a few milliseconds after the adapter showed it, with no call of the
/// adapter in between. The adapter asks again, a few times and only shortly after its own change,
/// so a state that the operator sets later in AR is never fought.
///
/// Two classes: PanelStateGuard (qar_ar_core/panel_state_guard.hpp) is the decision, pure and
/// without a clock of its own; PanelStateKeeper owns one panel's watch (the panel-update
/// subscription of the runtime) and carries the decision out. Both panel adapters use the keeper.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "qar_ar_core/panel_state_guard.hpp"
#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/log_time.hpp"
#include "qar_qar_adapter/panel_state_log.hpp"
#include "qar_qar_adapter/qar_capi_input_slots.hpp"
#include "qar_qar_adapter/qar_capi_session_owner.hpp"

namespace qar_qar_adapter
{

/// @brief The decision class lives in qar_ar_core (a pure value type with no QarOS call); the
/// keeper below drives it.
using qar_ar_core::PanelStateGuard;

/// @brief The watch of one panel: subscribes to the panel's updates in the runtime, feeds each
/// state it reports to a PanelStateGuard and posts the task that asks again.
///
/// Threads. The state requests are made on the GUI thread, start() and stop() on the thread that
/// owns the session, the callback of the runtime on a thread of its own; the guard is behind one
/// mutex. The callback reads the state, decides and posts: it makes no QarOS call besides the
/// getter, prints nothing and holds no lock across the post. The ask runs as a task on the thread
/// that owns the session.
///
/// The keeper must be held by a shared_ptr. stop() ends the subscription: once it returns, no
/// callback of the runtime is running and none will be again. The tasks hold the keeper weakly, so
/// a keeper whose owner let go of it is simply gone when they run.
class PanelStateKeeper : public std::enable_shared_from_this<PanelStateKeeper>
{
public:
  using Clock = PanelStateGuard::Clock;
  using ClockFn = std::function<Clock::time_point()>;

  /// @param owner Borrowed; posts the tasks. @param tag The log prefix without brackets, for
  /// example `qar-panel-adapter`. @param key The panel's name in the log lines.
  PanelStateKeeper(IQarSessionOwner * owner, std::string tag, std::string key)
  : owner_(owner), tag_(std::move(tag)), key_(std::move(key))
  {
  }

  ~PanelStateKeeper() { abandon(); }

  PanelStateKeeper(const PanelStateKeeper &) = delete;
  PanelStateKeeper & operator=(const PanelStateKeeper &) = delete;

  /// @brief Replaces the clock the decision reads; the steady clock is the default. For tests.
  void set_clock(ClockFn clock)
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(clock);
  }

  // --- GUI thread -----------------------------------------------------------------------------

  /// @brief A task that sets @p state is about to be queued.
  void request(QarGuiPanelState state)
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    guard_.request(state);
  }

  /// @brief The task of request() was refused or dropped.
  void request_dropped()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    guard_.request_dropped();
  }

  /// @brief The adapter is closed or detached: no record asks for anything from here on.
  void retire()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    retired_ = true;
  }

  /// @brief For the consumer's thread, on every tick: asks again when the hold of an ask has ended
  /// and the last report since then still contradicts the wanted state. Makes no QarOS call and
  /// no I/O, only a short lock; the ask is posted like one that a record causes.
  void poll()
  {
    PanelStateGuard::Decision decision;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (retired_) {
        return;
      }
      decision = guard_.poll(now_locked());
    }
    carry_out(decision);
  }

  // --- the thread that owns the session -------------------------------------------------------

  /// @brief The task of request() has run (also when it found nothing to do).
  void request_done()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    guard_.request_done(now_locked());
  }

  /// @brief The panel @p id exists and is in the state the adapter wants: VISIBLE unless the
  /// caller asked for another state already. Starts the watch.
  /// @return false when the watch could not be started (the reason is printed); the adapter
  /// works without it.
  bool start(QarSession * session, const QarGuiPanelId & id)
  {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      panel_id_ = id;
      guard_.created(now_locked());
    }
    if (slot_ != nullptr) {
      return true;
    }
    slot_ = detail::mint_slot(this);
    if (slot_ == nullptr) {
      print_watch_failure("no callback slot is left");
      return false;
    }
    QarResult r = QAR_CALL(qar_cancel_token_create, &token_);
    if (qar_result_is_error(r) || token_ == nullptr) {
      token_ = nullptr;
      print_watch_failure("qar_cancel_token_create failed");
      stop();
      return false;
    }
    r = QAR_CALL(
        qar_gui_panels_subscribe_panel_updates, session, &id, &PanelStateKeeper::on_update, slot_,
        token_);
    if (qar_result_is_error(r)) {
      print_watch_failure("qar_gui_panels_subscribe_panel_updates failed");
      stop();
      return false;
    }
    return true;
  }

  /// @brief Ends the watch: the subscription is cancelled and, when this returns, no callback is
  /// running or will run. Idempotent.
  void stop()
  {
    if (token_ != nullptr) {
      QAR_CALL(qar_cancel_token_cancel, token_);
    }
    detail::retire_slot(slot_);
    slot_ = nullptr;
    if (token_ != nullptr) {
      QAR_CALL(qar_cancel_token_handle_destroy, token_);
      token_ = nullptr;
    }
  }

  /// @brief For a session that ended with the watch still running: disowns the callback slot
  /// without a QarOS call. The token goes with the session.
  void abandon()
  {
    detail::retire_slot(slot_);
    slot_ = nullptr;
  }

private:
  /// The runtime's callback: one panel was republished.
  static void on_update(QarGuiPanel * handle, void * user_state)
  {
    auto * slot = static_cast<detail::PointerSlot *>(user_state);
    if (slot == nullptr || handle == nullptr) {
      return;
    }
    const std::lock_guard<std::mutex> lock(slot->mutex);
    auto * self = static_cast<PanelStateKeeper *>(slot->owner);
    if (self == nullptr) {
      return;  // retired; the keeper may be gone
    }
    QarGuiPanelState state = QAR_GUI_PANEL_STATE_VISIBLE;
    if (qar_result_is_error(QAR_CALL(qar_gui_panel_get_state, handle, &state))) {
      return;
    }
    self->on_record(state);
  }

  void on_record(QarGuiPanelState seen)
  {
    PanelStateGuard::Decision decision;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (retired_) {
        return;
      }
      decision = guard_.on_record(seen, now_locked());
    }
    carry_out(decision);
  }

  /// Posts the task a decision calls for.
  void carry_out(const PanelStateGuard::Decision & decision)
  {
    if (decision.kind == PanelStateGuard::Decision::Kind::kNone || owner_ == nullptr) {
      return;
    }
    // The tasks hold the keeper weakly: the callback thread must never be the one that drops the
    // last reference, because the destructor waits for the callbacks to leave.
    const std::weak_ptr<PanelStateKeeper> weak = weak_from_this();
    if (decision.kind == PanelStateGuard::Decision::Kind::kGaveUp) {
      owner_->post_session_task(
        ("panel-state-warn:" + key_).c_str(), [weak, decision](QarSession *) {
          if (const std::shared_ptr<PanelStateKeeper> self = weak.lock()) {
            self->print_gave_up(decision);
          }
        });
      return;
    }
    const bool posted = owner_->post_session_task(
      ("panel-state-ask:" + key_).c_str(),
      [weak, decision](QarSession * session) {
        if (const std::shared_ptr<PanelStateKeeper> self = weak.lock()) {
          self->run_ask(session, decision);
        }
      },
      [weak] {
        if (const std::shared_ptr<PanelStateKeeper> self = weak.lock()) {
          self->finish_ask();
        }
      });
    if (!posted) {
      finish_ask();
    }
  }

  /// The task that asks again; on the thread that owns the session.
  void run_ask(QarSession * session, const PanelStateGuard::Decision & decision)
  {
    QarGuiPanelId id{};
    bool go = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      id = panel_id_;
      go = !retired_ && guard_.has_wanted() && guard_.wanted() == decision.wanted;
    }
    if (go) {
      std::fprintf(
        stderr,
        "[%s] %s panel '%s' QarOS reported %s while %s is wanted - asked again (%u/%u)\n",
        tag_.c_str(), wall_clock_hms_ms().c_str(), key_.c_str(),
        panel_state_name(decision.seen).c_str(), panel_state_name(decision.wanted).c_str(),
        decision.number, PanelStateGuard::kMaxAsks);
      std::fflush(stderr);
      const QarResult r = QAR_CALL(qar_gui_panels_set_state, session, &id, decision.wanted);
      if (qar_result_is_error(r)) {
        std::fprintf(
          stderr, "[%s] %s panel '%s' *** qar_gui_panels_set_state failed: code=%lld\n",
          tag_.c_str(), wall_clock_hms_ms().c_str(), key_.c_str(),
          static_cast<long long>(r.code));
        std::fflush(stderr);
      }
    }
    finish_ask();
  }

  void finish_ask()
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    guard_.ask_done();
  }

  void print_gave_up(const PanelStateGuard::Decision & decision)
  {
    std::fprintf(
      stderr, "[%s] %s panel '%s' WARNING: QarOS keeps reporting %s while %s is wanted - gave up "
      "after %u asks\n",
      tag_.c_str(), wall_clock_hms_ms().c_str(), key_.c_str(),
      panel_state_name(decision.seen).c_str(), panel_state_name(decision.wanted).c_str(),
      PanelStateGuard::kMaxAsks);
    std::fflush(stderr);
  }

  void print_watch_failure(const char * what)
  {
    std::fprintf(
      stderr, "[%s] %s panel '%s' does not watch its state: %s\n", tag_.c_str(),
      wall_clock_hms_ms().c_str(), key_.c_str(), what);
    std::fflush(stderr);
  }

  Clock::time_point now_locked() const { return clock_ ? clock_() : Clock::now(); }

  IQarSessionOwner * owner_ = nullptr;
  const std::string tag_;
  const std::string key_;

  /// Guards everything below up to the members of the owner thread.
  mutable std::mutex mutex_;
  PanelStateGuard guard_;
  QarGuiPanelId panel_id_{};
  bool retired_ = false;
  ClockFn clock_;

  // The members below belong to the thread that owns the session.
  detail::PointerSlot * slot_ = nullptr;
  QarCancelToken * token_ = nullptr;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__PANEL_STATE_GUARD_HPP_
