// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__PANEL_STATE_GUARD_HPP_
#define QAR_AR_CORE__PANEL_STATE_GUARD_HPP_

/// @file panel_state_guard.hpp
/// @brief The decision that keeps a panel in the state the adapter asked for, against a QarOS that
/// reports the opposite right after the adapter set it. Pure: it reads no clock of its own and
/// calls nothing.
///
/// A panel that an earlier session left behind in the state HIDDEN can be reported as HIDDEN
/// again by the runtime a few milliseconds after the adapter showed it, with no call of the
/// adapter in between. The adapter asks again, a few times and only shortly after its own change,
/// so a state that the operator sets later in AR is never fought.

#include <chrono>
#include <deque>

#include "qar_ar_core/qar_streaming_c.hpp"

namespace qar_ar_core
{

/// @brief The decision: whether a record of the runtime about a panel is a reason to ask for the
/// state again. Not thread safe; the keeper serialises the calls.
///
/// Only the pair HIDDEN and VISIBLE is ever contested: a wanted or reported MINIMIZED or CLOSED
/// is the operator's. Nothing is asked while a state task of the adapter is queued or running (the
/// record may be older than that task), nor while a task of the guard itself is. After an ask the
/// guard waits: a contrary record within kAskHold of it may be the echo of the state that was
/// there before, so only a record that CONFIRMS the wanted state or the end of the hold lets it
/// ask again. Asking is bounded: only within kAfterChange of the adapter's own last change, and at
/// most kMaxAsks times within kAskWindow. A confirming record and every state the adapter sets
/// itself start the count anew, so the give-up is reported only for asks that all went unconfirmed.
///
/// A record decides only when it arrives, and a contrary record inside the hold asks nothing. The
/// guard therefore keeps the last state reported since its last ask or change, and poll() asks
/// again once the hold has ended when that state still contradicts the wanted one and nothing
/// newer came: a runtime that hides the panel again inside the hold and then falls silent is
/// answered as well.
class PanelStateGuard
{
public:
  using Clock = std::chrono::steady_clock;

  /// How long after the adapter's own change (create, reattach, a state it set) a contrary record
  /// is contested.
  static constexpr std::chrono::milliseconds kAfterChange{5000};
  /// The span in which at most kMaxAsks asks are made.
  static constexpr std::chrono::milliseconds kAskWindow{10000};
  static constexpr unsigned kMaxAsks = 3;
  /// How long after an ask a contrary record is taken for the echo of the state the ask replaces.
  static constexpr std::chrono::milliseconds kAskHold{1000};

  struct Decision
  {
    enum class Kind { kNone, kAsk, kGaveUp };
    Kind kind = Kind::kNone;
    QarGuiPanelState wanted = QAR_GUI_PANEL_STATE_VISIBLE;
    QarGuiPanelState seen = QAR_GUI_PANEL_STATE_VISIBLE;
    /// The 1-based number of the ask within the window (kAsk).
    unsigned number = 0;
  };

  /// @brief The adapter is about to queue a task that sets @p state.
  void request(QarGuiPanelState state)
  {
    wanted_ = state;
    has_wanted_ = true;
    requested_ = true;
    ++pending_;
    has_seen_ = false;
    forget_asks();
  }

  /// @brief The task of a request() was refused or dropped before it ran.
  void request_dropped()
  {
    if (pending_ > 0) {
      --pending_;
    }
  }

  /// @brief The task of a request() has run, successfully or not: the adapter's own change is
  /// dated @p now.
  void request_done(Clock::time_point now)
  {
    request_dropped();
    changed_at_ = now;
    has_change_ = true;
    has_seen_ = false;
    forget_asks();
  }

  /// @brief The panel was made or found again, and shown if it was not: the state VISIBLE is
  /// wanted unless a state was requested already, and the change is dated @p now.
  void created(Clock::time_point now)
  {
    if (!requested_) {
      wanted_ = QAR_GUI_PANEL_STATE_VISIBLE;
      has_wanted_ = true;
    }
    changed_at_ = now;
    has_change_ = true;
    has_seen_ = false;
    forget_asks();
  }

  /// @brief The task of a kAsk has run or was dropped.
  void ask_done() { asking_ = false; }

  /// @brief The runtime reported @p seen for the panel at @p now.
  Decision on_record(QarGuiPanelState seen, Clock::time_point now)
  {
    seen_ = seen;
    has_seen_ = true;
    return decide(seen, now, true);
  }

  /// @brief Time has passed without a record: asks again when the hold of the last ask has ended,
  /// the last report since then still contradicts the wanted state and the rules of on_record()
  /// allow an ask. Never reports a give-up (nothing new was reported); kNone or kAsk only.
  Decision poll(Clock::time_point now)
  {
    if (!has_seen_) {
      Decision out;
      out.wanted = wanted_;
      return out;
    }
    return decide(seen_, now, false);
  }

  QarGuiPanelState wanted() const { return wanted_; }
  bool has_wanted() const { return has_wanted_; }

private:
  static bool contested(QarGuiPanelState state)
  {
    return state == QAR_GUI_PANEL_STATE_VISIBLE || state == QAR_GUI_PANEL_STATE_HIDDEN;
  }

  /// The rules shared by on_record() and poll(); @p may_give_up is true for a fresh record.
  Decision decide(QarGuiPanelState seen, Clock::time_point now, bool may_give_up)
  {
    Decision out;
    out.seen = seen;
    out.wanted = wanted_;
    if (!has_wanted_ || !has_change_ || !contested(wanted_) || !contested(seen)) {
      return out;
    }
    if (seen == wanted_) {
      forget_asks();  // the runtime shows what is wanted: whatever was asked has worked
      return out;
    }
    if (pending_ > 0 || asking_) {
      return out;
    }
    if (now - changed_at_ > kAfterChange) {
      return out;
    }
    if (holding_) {
      if (now - asked_at_ < kAskHold) {
        return out;
      }
      holding_ = false;
    }
    while (!asks_.empty() && now - asks_.front() > kAskWindow) {
      asks_.pop_front();
    }
    if (asks_.size() >= kMaxAsks) {
      if (may_give_up && !warned_) {
        warned_ = true;
        out.kind = Decision::Kind::kGaveUp;
      }
      return out;
    }
    asks_.push_back(now);
    asking_ = true;
    holding_ = true;
    asked_at_ = now;
    has_seen_ = false;  // the ask replaces what was reported; only a newer report counts
    out.kind = Decision::Kind::kAsk;
    out.number = static_cast<unsigned>(asks_.size());
    return out;
  }

  /// The state the panel is in as far as the guard is concerned is settled: no ask is counted,
  /// none is held back and a give-up may be reported again.
  void forget_asks()
  {
    asks_.clear();
    holding_ = false;
    warned_ = false;
  }

  QarGuiPanelState wanted_ = QAR_GUI_PANEL_STATE_VISIBLE;
  bool has_wanted_ = false;
  /// A state was asked for by the adapter's caller (as opposed to the one the creation implies).
  bool requested_ = false;
  bool has_change_ = false;
  Clock::time_point changed_at_{};
  /// State tasks of the adapter that are queued or running.
  unsigned pending_ = 0;
  /// A task of the guard itself is queued or running.
  bool asking_ = false;
  bool warned_ = false;
  /// The last state the runtime reported since the last ask or change of the adapter, for poll().
  bool has_seen_ = false;
  QarGuiPanelState seen_ = QAR_GUI_PANEL_STATE_VISIBLE;
  /// The last ask is too young for a contrary record to count (see kAskHold).
  bool holding_ = false;
  Clock::time_point asked_at_{};
  std::deque<Clock::time_point> asks_;
};

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__PANEL_STATE_GUARD_HPP_
