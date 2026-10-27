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

#include <functional>

#include "qar_ar_core/qar_streaming_c.hpp"

namespace qar_qar_adapter
{

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
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_SESSION_OWNER_HPP_
