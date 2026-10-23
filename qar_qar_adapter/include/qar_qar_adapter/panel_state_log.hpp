// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_QAR_ADAPTER__PANEL_STATE_LOG_HPP_
#define QAR_QAR_ADAPTER__PANEL_STATE_LOG_HPP_

/// @file panel_state_log.hpp
/// @brief The words of the log lines a panel adapter prints about a panel: the change of its
/// state (`qar_gui_panels_set_state`), the runtime's own record of it, a transfer that is
/// reconnecting and the first frame shown. Pure functions over a state, an id and strings, so a
/// line is exactly testable without a QarOS session.

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_qar_adapter/log_time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace qar_qar_adapter
{

/// @brief The name of a panel state as the logs print it: `VISIBLE`, `MINIMIZED`, `HIDDEN`,
/// `CLOSED`, or `state <number>` for a value the enum does not name.
inline std::string
panel_state_name(QarGuiPanelState state)
{
  switch (state) {
    case QAR_GUI_PANEL_STATE_VISIBLE:
      return "VISIBLE";
    case QAR_GUI_PANEL_STATE_MINIMIZED:
      return "MINIMIZED";
    case QAR_GUI_PANEL_STATE_HIDDEN:
      return "HIDDEN";
    case QAR_GUI_PANEL_STATE_CLOSED:
      return "CLOSED";
  }
  return "state " + std::to_string(static_cast<int>(state));
}

/// @brief The 16 bytes of an id of the runtime (a panel, a stream) as the canonical 8-4-4-4-12
/// UUID text. The runtime names the same id that way in its own log, so a line here is found next
/// to its line there with one search.
inline std::string
id_bytes_uuid(const std::uint8_t (& data)[QAR_MAX_ID_LENGTH])
{
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(kHex[(data[i] >> 4) & 0x0F]);
    out.push_back(kHex[data[i] & 0x0F]);
  }
  return out;
}

/// @brief A panel id as the canonical UUID text (see id_bytes_uuid()).
inline std::string
panel_id_uuid(const QarGuiPanelId & id)
{
  return id_bytes_uuid(id.data);
}

/// @brief What the runtime says about one panel when it republishes it: the state, the stream
/// its content is, and its size. A field the runtime refused to give is marked unknown.
struct PanelRuntimeRecord
{
  bool state_known = false;
  QarGuiPanelState state = QAR_GUI_PANEL_STATE_VISIBLE;
  bool stream_known = false;
  std::uint8_t stream_id[QAR_MAX_ID_LENGTH] = {0};
  bool size_known = false;
  float width_meters = 0.0f;
  float height_meters = 0.0f;
};

/// @brief The size of a record as `<w>x<h>` in metres with six decimals, `unknown` if the runtime
/// gave none. The change of a record is decided on this text, so a float that differs in the last
/// bits is not a change.
inline std::string
runtime_record_size_text(const PanelRuntimeRecord & record)
{
  if (!record.size_known) {
    return "unknown";
  }
  char buf[64] = {0};
  std::snprintf(
    buf, sizeof(buf) - 1, "%.6fx%.6f", static_cast<double>(record.width_meters),
    static_cast<double>(record.height_meters));
  return std::string(buf);
}

/// @brief Whether two records differ in the state, the stream or the size (to six decimals).
inline bool
runtime_record_changed(const PanelRuntimeRecord & a, const PanelRuntimeRecord & b)
{
  if (a.state_known != b.state_known || (a.state_known && a.state != b.state)) {
    return true;
  }
  if (a.stream_known != b.stream_known) {
    return true;
  }
  if (a.stream_known) {
    for (std::size_t i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
      if (a.stream_id[i] != b.stream_id[i]) {
        return true;
      }
    }
  }
  return runtime_record_size_text(a) != runtime_record_size_text(b);
}

/// @brief Whether the stream the runtime names as the content of the panel is the one the adapter
/// built the panel's sender with (@p ours, canonical UUID text). False when the runtime named no
/// stream and when the adapter has none to compare with: nothing is called a match unless both
/// ids are there and equal.
inline bool
runtime_record_matches(const PanelRuntimeRecord & record, const std::string & ours)
{
  return record.stream_known && !ours.empty() && id_bytes_uuid(record.stream_id) == ours;
}

/// @brief Remembers the last record of one panel, tells whether a new one is a change and numbers
/// the changes from 1. Not thread safe: the owner serialises the calls.
class PanelRecordTrack
{
public:
  /// @param ours The stream id the adapter built the panel's sender with, canonical UUID text.
  explicit PanelRecordTrack(std::string ours = std::string())
  : ours_(std::move(ours))
  {
  }

  /// @return true when @p record differs from the last one noted, the first record always does;
  /// count() is then the number of this change.
  bool note(const PanelRuntimeRecord & record)
  {
    if (has_last_ && !runtime_record_changed(last_, record)) {
      return false;
    }
    last_ = record;
    has_last_ = true;
    ++count_;
    return true;
  }

  /// @brief The changes noted so far.
  std::uint64_t count() const { return count_; }
  const std::string & ours() const { return ours_; }

private:
  std::string ours_;
  PanelRuntimeRecord last_;
  bool has_last_ = false;
  std::uint64_t count_ = 0;
};

/// @brief The line the panel path prints for each change of the runtime's record of a panel:
/// `[qar-panel-adapter] <time> panel '<key>' runtime record #<n>: state=<STATE>
/// content_stream=<uuid> (ours <uuid>: match|MISMATCH) size=<w>x<h> m`, all on one line. A stream
/// that the runtime did not name reads `none`, an adapter without a stream id `ours none`; both
/// are a MISMATCH.
///
/// @param time Wall-clock stamp, `HH:MM:SS.mmm` (wall_clock_hms_ms()).
/// @param number The 1-based number of this change for the panel (PanelRecordTrack::count()).
/// @param ours The adapter's own stream id, canonical UUID text, or empty.
inline std::string
runtime_record_line(
  const std::string & time, const std::string & key, std::uint64_t number,
  const PanelRuntimeRecord & record, const std::string & ours)
{
  std::string line = "[qar-panel-adapter] " + time + " panel '" + key + "' runtime record #" +
    std::to_string(number) + ": state=" +
    (record.state_known ? panel_state_name(record.state) : std::string("unknown")) +
    " content_stream=" + (record.stream_known ? id_bytes_uuid(record.stream_id) : "none") +
    " (ours " + (ours.empty() ? std::string("none") : ours) + ": " +
    (runtime_record_matches(record, ours) ? "match" : "MISMATCH") + ") size=" +
    runtime_record_size_text(record);
  if (record.size_known) {
    line += " m";
  }
  return line;
}

/// @brief The runtime's records of all panels: queues a line for each change, from the thread of
/// the runtime's callback, for the render thread to print. The callback only reads the handle and
/// queues; it does no I/O. Not thread safe: the owner serialises every call with its own lock.
class RuntimeRecordLog
{
public:
  /// The most lines kept for the render thread. A record that arrives with the queue full is
  /// counted as dropped, and the next one that is printed shows the gap in its number.
  static constexpr std::size_t kMaxQueued = 256;

  /// @brief A panel's sender was built with stream @p ours (canonical UUID text): its records
  /// are numbered from 1 and compared with that id from here on.
  void begin(const std::string & key, const std::string & ours)
  {
    tracks_[key] = PanelRecordTrack(ours);
  }

  /// @brief The panel is retired: records that still arrive for @p key are ignored.
  void end(const std::string & key) { tracks_.erase(key); }

  /// @brief The runtime republished the panel @p key as @p record at @p when.
  /// @return true when it differs from the last record and was queued; false when it is the
  /// same, when the queue is full (then dropped() grows) and for a panel without begin().
  bool note(
    const std::string & key, const PanelRuntimeRecord & record,
    std::chrono::system_clock::time_point when)
  {
    const auto track = tracks_.find(key);
    if (track == tracks_.end() || !track->second.note(record)) {
      return false;
    }
    if (queued_.size() >= kMaxQueued) {
      ++dropped_;
      return false;
    }
    queued_.push_back(Queued{key, track->second.count(), record, track->second.ours(), when});
    return true;
  }

  /// @brief The lines of everything queued, oldest first, each with the time it arrived; the
  /// queue is empty afterwards.
  std::vector<std::string> take_lines()
  {
    std::vector<std::string> lines;
    lines.reserve(queued_.size());
    for (const Queued & item : queued_) {
      lines.push_back(
        runtime_record_line(hms_ms_of(item.when), item.key, item.number, item.record, item.ours));
    }
    queued_.clear();
    return lines;
  }

  /// @brief Records that arrived with the queue full.
  std::uint64_t dropped() const { return dropped_; }

private:
  struct Queued
  {
    std::string key;
    std::uint64_t number;
    PanelRuntimeRecord record;
    std::string ours;
    std::chrono::system_clock::time_point when;
  };

  std::map<std::string, PanelRecordTrack> tracks_;
  std::vector<Queued> queued_;
  std::uint64_t dropped_ = 0;
};

/// @brief The line printed once per stream when its transfer turns to RECONNECTING after frames
/// were shown: `[qar-panel-adapter] <time> panel '<key>' stream <uuid>: transfer RECONNECTING
/// after <n> frames`.
inline std::string
reconnecting_line(
  const std::string & time, const std::string & key, const std::string & stream,
  std::uint64_t frames)
{
  return "[qar-panel-adapter] " + time + " panel '" + key + "' stream " + stream +
         ": transfer RECONNECTING after " + std::to_string(frames) + " frames";
}

/// @brief The line printed once per panel sender for its first frame shown:
/// `[qar-panel-adapter] <time> panel '<key>' first show_frame tid=<n>`.
inline std::string
first_panel_frame_line(
  const std::string & time, const std::string & key, unsigned long long thread)
{
  return "[qar-panel-adapter] " + time + " panel '" + key + "' first show_frame tid=" +
         std::to_string(thread);
}

/// @brief The line a URL panel adapter prints after asking QarOS to change a panel's state:
/// `[qar-url-panel-adapter] <time> panel '<label>' state -> <STATE> (id <uuid>)`, and
/// `... state -> <STATE> FAILED (id <uuid>): <error>` when the call failed (@p error not empty).
/// It has the shape of the line the stream-panel adapter prints, plus the id: panels of the same
/// label (the instances of one page) are told apart by it.
///
/// @param time Wall-clock stamp, `HH:MM:SS.mmm` (wall_clock_hms_ms()).
/// @param label The panel's name, which is its title.
/// @param id_text The panel's id as text (panel_id_uuid()), or `none` if the adapter has no
///        panel.
inline std::string
url_panel_state_line(
  const std::string & time, const std::string & label, const std::string & id_text,
  QarGuiPanelState state, const std::string & error)
{
  std::string line = "[qar-url-panel-adapter] " + time + " panel '" + label + "' state -> " +
    panel_state_name(state);
  if (error.empty()) {
    line += " (id " + id_text + ")";
  } else {
    line += " FAILED (id " + id_text + "): " + error;
  }
  return line;
}

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__PANEL_STATE_LOG_HPP_
