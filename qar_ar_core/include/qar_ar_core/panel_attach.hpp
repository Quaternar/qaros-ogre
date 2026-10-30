// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__PANEL_ATTACH_HPP_
#define QAR_AR_CORE__PANEL_ATTACH_HPP_

/// @file panel_attach.hpp
/// @brief What to do with a panel that `qar_gui_panels_get_or_create` returned: the pure
/// decision from the values a probe read, and the log line that records it.
///
/// `get_or_create` hands back the panel of the same common name that an earlier session left
/// behind, hidden, instead of creating one. The adapters read what that panel is (stream or
/// website, its content, its state, its size), hand the values to @ref decide_panel_attach and
/// carry out what it says. No QarOS call is made here.

#include "qar_ar_core/qar_streaming_c.hpp"

#include <string>
#include <vector>

namespace qar_ar_core
{

/// @brief How many suffixes "-r1" .. "-r8" are tried after a name whose panel is CLOSED. The
/// base name and the eight suffixed names are nine names in all.
constexpr unsigned kMaxClosedPanelRetries = 8;

/// @brief The values of the panel that `get_or_create` returned, as the probe read them.
struct PanelProbe
{
  /// The panel was listed by `qar_query_gui_panels`. False when the query failed or the id is
  /// not in the list; the decision is then "created", with nothing to carry out.
  bool found = false;
  /// The panel shows the other kind of content than this adapter does: a website for an adapter
  /// that streams video, a video stream for a URL adapter. The content type is fixed when the
  /// panel is created, so such a panel cannot be reused.
  bool content_mismatch = false;
  /// The content differs from what this adapter wants: the panel's stream is not the stream of
  /// the video sender made for this adapter, or its URL is not the URL to show.
  bool content_differs = false;
  QarGuiPanelState state = QAR_GUI_PANEL_STATE_VISIBLE;
  /// The size the panel has differs from the size this adapter wants.
  bool size_differs = false;
};

/// @brief What the probe says about the panel.
enum class PanelAttachKind
{
  /// The panel was just created, or is exactly what the adapter wants: nothing to carry out.
  Created,
  /// The panel is an earlier panel of the same name: swap its content, fit size and pose, show
  /// it.
  Reattached,
  /// The name is taken by a panel that cannot be used: ask `get_or_create` again with
  /// @ref PanelAttachDecision::retry_suffix appended to the common name the host started from
  /// (not to the name of the last try).
  RetryWithSuffix,
  /// The name is taken by a panel that cannot be used and no retry is left.
  Refused
};

/// @brief The decision and the steps it implies.
struct PanelAttachDecision
{
  PanelAttachKind kind = PanelAttachKind::Created;
  /// Reattached: put our content on the panel (`qar_gui_panels_set_stream` with our stream,
  /// `qar_gui_panels_navigate_to_uri` with our URL).
  bool swap_content = false;
  /// Reattached: `qar_gui_panels_change_size` (the pose is updated whenever the kind is
  /// Reattached).
  bool change_size = false;
  /// Reattached: the panel was HIDDEN or MINIMIZED; `qar_gui_panels_set_state(VISIBLE)`.
  bool show = false;
  /// RetryWithSuffix: "-v2" for a panel of the other content type, "-r1" .. "-r8" for a closed
  /// one (the number is @c retries_done + 1). Empty otherwise.
  std::string retry_suffix;
  /// Why a panel is not reused: "other content type" or "closed". Empty otherwise. The adapter
  /// words its error from it ("a web panel of that name exists").
  std::string reason;
};

/// @brief Decides from @p probe. @p retries_done is the number of repeats with a suffix made so
/// far: 0 on the first `get_or_create`, 1 on the next, and so on.
///
/// A panel of the other content type is checked before the state: its content type is fixed at
/// creation, so the content of this adapter cannot be put on it; the repeat uses "-v2", once. A
/// CLOSED panel is not reused: the repeats walk "-r1" .. "-r8" (kMaxClosedPanelRetries) until a
/// name is not CLOSED, and only a name still CLOSED after the last gives Refused. A found panel
/// that is VISIBLE, has the wanted content and the wanted size needs nothing: Created.
PanelAttachDecision decide_panel_attach(const PanelProbe & probe, unsigned retries_done);

/// @brief What to change in the visible-to list of a panel to bring it to a wanted list.
struct VisibleToPlan
{
  std::vector<QarPeerId> additions;
  std::vector<QarPeerId> removals;
  bool empty() const { return additions.empty() && removals.empty(); }
};

/// @brief The additions and removals that turn @p current into @p wanted (both are sets of peer
/// ids; the order does not matter). An empty list means every peer sees the panel, so a wanted
/// list that is empty removes every peer of @p current.
VisibleToPlan plan_visible_to(
  const std::vector<QarPeerId> & current, const std::vector<QarPeerId> & wanted);

/// @brief The name of a panel state as the log prints it: "VISIBLE", "MINIMIZED", "HIDDEN",
/// "CLOSED"; "state N" for a value outside the enum.
std::string panel_state_name(QarGuiPanelState state);

/// @brief The fields of the line that records an attach.
struct PanelAttachLogFields
{
  std::string time;         ///< `HH:MM:SS.mmm`, from the caller's clock.
  std::string key;          ///< The panel key of the host.
  bool reattached = false;  ///< `reattached` or `created`.
  std::string common_name;
  std::string panel_id;     ///< Canonical UUID text.
  /// The state the probe read, or "none" when the probe found no panel.
  std::string state_was = "none";
  /// The content (stream or URL) of the panel was replaced.
  bool stream_swapped = false;
  bool size_changed = false;
};

/// @brief `[qar-panel-adapter] <time> panel '<key>' <created|reattached> name=<common> id=<id>
/// state_was=<state> stream_swapped=<y|n> size_changed=<y|n>`, on one line, no newline.
std::string panel_attach_log_line(const PanelAttachLogFields & fields);

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__PANEL_ATTACH_HPP_
