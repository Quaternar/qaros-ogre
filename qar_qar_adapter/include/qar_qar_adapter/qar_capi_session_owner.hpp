// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The one thing the GUI-panel and URL-panel factories need from whoever owns the QarOS
// session: the session itself, and whether it is up yet. QarCApiSenderFactory (component
// `render`) implements it; a host that streams panels without a 3D scene can implement it
// with its own session bootstrap.

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_SESSION_OWNER_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_SESSION_OWNER_HPP_

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

  /// @brief True once the session exists (connect succeeded).
  virtual bool ready() const = 0;

  /// @brief The QarOS session, borrowed; null until @ref ready. Callers must not destroy it.
  virtual QarSession * session() const = 0;
};

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_SESSION_OWNER_HPP_
