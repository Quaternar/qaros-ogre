// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__PANEL_OP_RESULT_HPP_
#define QAR_AR_CORE__PANEL_OP_RESULT_HPP_

/// @file panel_op_result.hpp
/// @brief The outcome of one queued call on a panel. Qt-free, plain values.

#include "qar_ar_core/qar_streaming_c.hpp"

#include <string>

namespace qar_ar_core
{

/// @brief The outcome of one queued call on a panel, handed back by drain_results() of
/// IQarPanelSenderAdapter and IQarUrlPanelAdapter.
struct PanelOpResult
{
  enum class Op { Create, Placement, State, Navigate, VisibleTo, Close, Detach };
  Op op = Op::Create;
  bool ok = true;
  /// Op::State: the state that was asked for.
  QarGuiPanelState state = QAR_GUI_PANEL_STATE_VISIBLE;
  /// Op::Create: the panel of an earlier session was found again.
  bool reattached = false;
  /// The runtime's words when !ok.
  std::string error;
};

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__PANEL_OP_RESULT_HPP_
