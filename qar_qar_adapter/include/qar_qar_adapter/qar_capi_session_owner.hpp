// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The one thing the GUI-panel and URL-panel factories need from whoever owns the QarOS
// session: the session itself, whether it is up yet, and a way to run a call on the thread
// that owns the session. QarSessionOwner (component `session`) implements it, and
// QarCApiSenderFactory (component `render`) forwards to its owner; a host that streams panels
// without either can implement it with its own bootstrap.

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_SESSION_OWNER_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_SESSION_OWNER_HPP_

#include <atomic>
#include <functional>
#include <memory>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"

namespace qar_qar_adapter
{

/// @brief Releases QarOS handles on the thread that owns the session.
class IReleaseLane
{
public:
  virtual ~IReleaseLane() = default;

  /// @brief Queues @p release for the owner's thread. Any thread; it never blocks and never
  /// calls QarOS on the caller's thread. Releases run in posting order with the owner's other
  /// work, also while the session is lost, and before a disconnect or a hand-back that was
  /// posted after them. @p name is copied.
  /// @return false when the owner's thread has ended or was left behind: the caller then leaves
  /// the handles unreleased and makes no QarOS call on them.
  virtual bool post_release(const char * name, std::function<void()> release) = 0;

  /// @brief What the owner needs to know about one frame worker: whether the owner left it
  /// behind, and which QarOS call it is inside. The worker owns it (shared) and marks its thread
  /// with it; the lane keeps a weak reference.
  struct WorkerSlot
  {
    /// Set by the owner when it stops waiting for the worker: from then on every QarOS call on
    /// the worker's thread returns at once and calls nothing.
    std::atomic<bool> abandoned{false};
    /// The QarOS call the worker's thread is inside (see OwnerCallClock).
    OwnerCallClock clock;
  };

  /// @brief Registers a frame worker that is about to start, so that the owner waits for it
  /// before it destroys the session. Any thread; it never blocks. Every accepted worker calls
  /// leave_worker() exactly once, when its thread is done with QarOS handles.
  /// @return false when the owner's thread has ended or was left behind: the worker must not
  /// start, the caller keeps the work on its own thread. The default accepts and counts nothing.
  virtual bool enter_worker(const std::shared_ptr<WorkerSlot> & slot)
  {
    (void)slot;
    return true;
  }

  /// @brief A registered frame worker is done: its handles are released or posted, and it makes no
  /// further QarOS call. Any thread; it never blocks. The default does nothing.
  virtual void leave_worker() {}

  /// @brief Like leave_worker(), for the worker that entered with @p slot. A worker the owner gave
  /// up on (its session was left unreleased) is not counted after that, so its late leave does not
  /// take away the count of a worker of a later session. The default calls leave_worker().
  virtual void leave_worker(const std::shared_ptr<WorkerSlot> & slot)
  {
    (void)slot;
    leave_worker();
  }

  /// @brief The number of registered frame workers that have not called leave_worker() yet.
  /// The default is 0.
  virtual int workers_alive() const { return 0; }
};

/// @brief Owner of the live QarOS session that panels borrow.
///
/// A panel's stream id is only accepted from a video sender owned by the SAME session as
/// the panel (QarGuiPanelStreamIdExt), so panel factories borrow the owner's session
/// rather than create their own.
class IQarSessionOwner
{
public:
  virtual ~IQarSessionOwner() = default;

  /// @brief True once the session exists and the consumer may use it.
  virtual bool ready() const = 0;

  /// @brief The QarOS session, borrowed; null until @ref ready. Callers must not destroy it.
  virtual QarSession * session() const = 0;

  /// @brief A call on the session. It runs on the thread that owns the session; @p session is
  /// the open session and must not be kept.
  using SessionTask = std::function<void(QarSession * session)>;

  /// @brief Queues @p task for the thread that owns the session. Any thread; it never blocks and
  /// never calls QarOS on the caller's thread. When the session goes away before the task's turn,
  /// or the drain budget of a disconnect runs out, @p dropped runs instead (on that thread; it
  /// may only record). @p name names the task in the log; it is copied.
  ///
  /// The default, for a host without an executor, runs @p task at once on the calling thread.
  /// @return false when no session is running for the consumer; neither function is called then.
  virtual bool post_session_task(
    const char * name, SessionTask task, std::function<void()> dropped = {})
  {
    (void)name;
    (void)dropped;
    QarSession * const open = ready() ? session() : nullptr;
    if (open == nullptr) {
      return false;
    }
    task(open);
    return true;
  }

  /// @brief The release lane of the owner, or null for a host without one: such a host releases
  /// at once on its own thread. Objects take it when they are made and keep it.
  virtual std::shared_ptr<IReleaseLane> release_lane() const { return nullptr; }
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_SESSION_OWNER_HPP_
