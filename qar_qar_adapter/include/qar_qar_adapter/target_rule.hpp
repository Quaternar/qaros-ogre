// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The decisions of the session owner about peers that need no QarOS call: which peer is a render
// target, and when a peer is gone. Header-only, so a test or a host can
// ask the same question the owner asks.

#ifndef QAR_QAR_ADAPTER__TARGET_RULE_HPP_
#define QAR_QAR_ADAPTER__TARGET_RULE_HPP_

#include "qar_ar_core/qar_streaming_c.hpp"

namespace qar_qar_adapter
{

/// @brief True when a peer is one this program renders a stream for.
///
/// The role must be TARGET and the peer must be here: ONLINE, or UNKNOWN (a hub that has not
/// reconciled it yet, which reads like ONLINE). A presence value this header does not name
/// reads as UNKNOWN. This program's own peer is never a target.
inline bool is_render_target(QarPeerRole role, QarPeerPresence presence, bool is_self)
{
  if (is_self || role != QAR_PEER_ROLE_TARGET) {
    return false;
  }
  switch (presence) {
    case QAR_PEER_PRESENCE_OFFLINE:
    case QAR_PEER_PRESENCE_LEAVING:
    case QAR_PEER_PRESENCE_RETIRED:
      return false;
    case QAR_PEER_PRESENCE_ONLINE:
    case QAR_PEER_PRESENCE_UNKNOWN:
    default:
      return true;
  }
}

/// @brief True when a peer is gone: its stream stops. A peer that is OFFLINE or LEAVING can read
/// ONLINE again under the same id; a peer that left for good never does.
inline bool is_gone(QarPeerPresence presence)
{
  return presence == QAR_PEER_PRESENCE_OFFLINE || presence == QAR_PEER_PRESENCE_LEAVING ||
         presence == QAR_PEER_PRESENCE_RETIRED;
}

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__TARGET_RULE_HPP_
