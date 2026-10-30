// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The two decisions of the session owner that need no QarOS call: which peer is a render
// target, and which onboarding mode a start uses. Header-only, so a test or a host can
// ask the same question the owner asks.

#ifndef QAR_QAR_ADAPTER__TARGET_RULE_HPP_
#define QAR_QAR_ADAPTER__TARGET_RULE_HPP_

#include "qar_ar_core/qar_streaming_c.hpp"

namespace qar_qar_adapter
{

/// @brief How this program joins the QAROS Hub.
enum class HubMode
{
  kAuto,    ///< Decided at connect: a stored pairing, else the installed Hub, else a code.
  kLocal,   ///< A source app on the Hub's own PC: the Hub asks for approval once, no code.
  kRemote   ///< A Hub on another PC: a pairing code once, then a stored pairing.
};

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

/// @brief The mode a start uses.
///
/// An explicit request wins. In auto mode a stored pairing means remote (this program paired
/// with another PC's Hub before), an installed QAROS means local, and anything else means remote.
/// @param reason When not null, receives a short phrase naming the deciding fact.
inline HubMode resolve_hub_mode(
  HubMode requested, bool has_remote_identity, bool qaros_installed,
  const char ** reason = nullptr)
{
  const char * why = "set in the settings";
  HubMode mode = requested;
  if (requested == HubMode::kAuto) {
    if (has_remote_identity) {
      mode = HubMode::kRemote;
      why = "a pairing with another PC is stored";
    } else if (qaros_installed) {
      mode = HubMode::kLocal;
      why = "QAROS is installed on this PC";
    } else {
      mode = HubMode::kRemote;
      why = "QAROS is not installed on this PC";
    }
  }
  if (reason != nullptr) {
    *reason = why;
  }
  return mode;
}

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__TARGET_RULE_HPP_
