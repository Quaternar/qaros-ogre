// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_url_panel_adapter.hpp"

#include "qar_ar_core/panel_attach.hpp"
#include "qar_qar_adapter/gui_call_guard.hpp"
#include "qar_qar_adapter/log_time.hpp"
#include "qar_qar_adapter/panel_state_guard.hpp"
#include "qar_qar_adapter/panel_state_log.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace qar_qar_adapter
{
namespace
{

/// The line for one change of panel state, on stderr like the stream-panel adapter's: the
/// outcome of every hide and show of a URL panel, successful or not, is in the log. The panel
/// is named by its title; its URL is not printed, because it carries the access token of the
/// page. A null @p id is an adapter with no panel.
void
log_url_panel_state(
  const std::string & title, const QarGuiPanelId * id, QarGuiPanelState state,
  const std::string & error)
{
  const std::string line = url_panel_state_line(
    wall_clock_hms_ms(), title.empty() ? std::string("untitled") : title,
    id != nullptr ? panel_id_uuid(*id) : std::string("none"), state, error);
  std::fprintf(stderr, "%s\n", line.c_str());
  std::fflush(stderr);
}

/// Same shape as the other adapters' helper: a readable "<call> failed: ..."
/// line, also echoed to stderr because the panel UI only shows the last error.
std::string
url_panel_result_error(const char * what, const QarResult & r)
{
  char buf[512] = {0};
  qar_result_message(r, buf, sizeof(buf) - 1);
  std::string out = std::string(what) + " failed: code=" +
    std::to_string(static_cast<long long>(r.code));
  if (buf[0] != '\0') {
    out += " msg=";
    out += buf;
  }
  std::fprintf(stderr, "[qar-url-panel-adapter] *** %s\n", out.c_str());
  std::fflush(stderr);
  return out;
}

/// True when two panel sizes are the same to a tenth of a millimetre and a ten-thousandth of a
/// content scale.
bool
same_panel_size(const QarGuiPanelSize & a, const QarGuiPanelSize & b)
{
  return std::fabs(a.width_meters - b.width_meters) < 1e-4f &&
         std::fabs(a.height_meters - b.height_meters) < 1e-4f &&
         std::fabs(a.content_scale - b.content_scale) < 1e-4f;
}

/// Handles of the first panel list query, and the largest list read at all.
constexpr std::size_t kPanelListFirst = 64;
constexpr std::size_t kPanelListMax = 1024;

/// Reads the session's panel list with one `qar_query_gui_panels` call into a buffer that grows
/// while it comes back full (or while the runtime reports more panels than the buffer holds).
/// The handles are owned by the caller. False when the query fails.
bool
query_panel_list(QarSession * session, std::vector<QarGuiPanel *> & out)
{
  out.clear();
  std::size_t capacity = kPanelListFirst;
  for (;;) {
    std::vector<QarGuiPanel *> handles(capacity, nullptr);
    std::size_t written = 0;
    if (qar_result_is_error(
        QAR_CALL(qar_query_gui_panels, session, handles.data(), handles.size(), &written)))
    {
      return false;
    }
    const bool more = (written >= capacity) && capacity < kPanelListMax;
    if (!more) {
      handles.resize(std::min(written, handles.size()));
      out = std::move(handles);
      return true;
    }
    for (QarGuiPanel * handle : handles) {
      if (handle != nullptr) {
        QAR_CALL(qar_gui_panel_handle_destroy, handle);
      }
    }
    capacity = std::min(std::max(capacity * 4, written), kPanelListMax);
  }
}

/// Reads the panel @p id out of the session's panel list: its state, its content type, its URL
/// and its size. The handles of the list are owned here and destroyed. A value the runtime does
/// not answer takes the safe default: HIDDEN (the panel is shown), a URL that differs (the
/// panel navigates) and a size that differs (it is set).
qar_ar_core::PanelProbe
probe_url_panel(
  QarSession * session, const QarGuiPanelId & id, const std::string & url,
  const QarGuiPanelSize & wanted)
{
  qar_ar_core::PanelProbe probe;
  std::vector<QarGuiPanel *> handles;
  if (!query_panel_list(session, handles)) {
    return probe;
  }
  for (QarGuiPanel * handle : handles) {
    if (handle == nullptr) {
      continue;
    }
    QarGuiPanelId listed{};
    if (!probe.found && !qar_result_is_error(QAR_CALL(qar_gui_panel_get_id, handle, &listed)) &&
      std::memcmp(listed.data, id.data, sizeof(id.data)) == 0)
    {
      probe.found = true;
      QarGuiPanelState state = QAR_GUI_PANEL_STATE_HIDDEN;
      if (qar_result_is_error(QAR_CALL(qar_gui_panel_get_state, handle, &state))) {
        state = QAR_GUI_PANEL_STATE_HIDDEN;
      }
      probe.state = state;
      QarGuiPanelContentType type = QAR_GUI_PANEL_CONTENT_TYPE_WEBSITE_URI;
      if (qar_result_is_error(QAR_CALL(qar_gui_panel_get_content_type, handle, &type))) {
        type = QAR_GUI_PANEL_CONTENT_TYPE_WEBSITE_URI;
      }
      probe.content_mismatch = type == QAR_GUI_PANEL_CONTENT_TYPE_STREAM_ID;
      probe.content_differs = true;
      if (!probe.content_mismatch) {
        std::vector<char> uri(url.size() + 64, '\0');
        if (!qar_result_is_error(
            QAR_CALL(qar_gui_panel_get_content_uri, handle, uri.data(), uri.size())))
        {
          probe.content_differs = url != uri.data();
        }
      }
      QarGuiPanelSize size{};
      probe.size_differs =
        qar_result_is_error(QAR_CALL(qar_gui_panel_get_size, handle, &size)) ||
        !same_panel_size(size, wanted);
    }
    QAR_CALL(qar_gui_panel_handle_destroy, handle);
  }
  return probe;
}

/// Reads the visible-to list of the panel @p id. False when the panel is not listed or the list
/// cannot be read.
bool
read_visible_to(QarSession * session, const QarGuiPanelId & id, std::vector<QarPeerId> & out)
{
  out.clear();
  std::vector<QarGuiPanel *> handles;
  if (!query_panel_list(session, handles)) {
    return false;
  }
  bool ok = false;
  for (QarGuiPanel * handle : handles) {
    if (handle == nullptr) {
      continue;
    }
    QarGuiPanelId listed{};
    if (!ok && !qar_result_is_error(QAR_CALL(qar_gui_panel_get_id, handle, &listed)) &&
      std::memcmp(listed.data, id.data, sizeof(id.data)) == 0)
    {
      size_t peers = 0;
      if (!qar_result_is_error(
          QAR_CALL(qar_gui_panel_get_visible_to_peers_count, handle, &peers)))
      {
        std::vector<QarPeerId> list(peers);
        size_t listed_count = 0;
        if (peers == 0 ||
          !qar_result_is_error(
            QAR_CALL(
              qar_gui_panel_get_visible_to_peers, handle, list.data(), list.size(),
              &listed_count)))
        {
          list.resize(std::min(listed_count, list.size()));
          out = std::move(list);
          ok = true;
        }
      }
    }
    QAR_CALL(qar_gui_panel_handle_destroy, handle);
  }
  return ok;
}

/// Brings the visible-to list of the panel @p id to @p wanted (empty: every peer sees it).
/// @return false, with @p error set, when the list cannot be read or changed.
bool
apply_visible_to(
  QarSession * session, const QarGuiPanelId & id, const std::vector<QarPeerId> & wanted,
  std::string & error)
{
  std::vector<QarPeerId> current;
  if (!read_visible_to(session, id, current)) {
    error = "the visible-to list of the panel cannot be read";
    std::fprintf(stderr, "[qar-url-panel-adapter] *** %s\n", error.c_str());
    std::fflush(stderr);
    return false;
  }
  const qar_ar_core::VisibleToPlan plan = qar_ar_core::plan_visible_to(current, wanted);
  if (plan.empty()) {
    return true;
  }
  std::vector<const QarPeerId *> additions;
  std::vector<const QarPeerId *> removals;
  for (const QarPeerId & peer : plan.additions) {
    additions.push_back(&peer);
  }
  for (const QarPeerId & peer : plan.removals) {
    removals.push_back(&peer);
  }
  const QarResult r = QAR_CALL(
      qar_gui_panels_update_visible_to, session, &id,
      additions.empty() ? nullptr : additions.data(), additions.size(),
      removals.empty() ? nullptr : removals.data(), removals.size());
  if (qar_result_is_error(r)) {
    error = url_panel_result_error("qar_gui_panels_update_visible_to", r);
    return false;
  }
  std::fprintf(
    stderr, "[qar-url-panel-adapter] %s visible-to list of the panel changed: +%zu -%zu\n",
    wall_clock_hms_ms().c_str(), additions.size(), removals.size());
  std::fflush(stderr);
  return true;
}

}  // namespace

// What the panel's tasks and the GUI thread share; see the class comment. The tasks hold it by
// shared_ptr, so it outlives an adapter that was released while its tasks are queued.
struct QarCApiUrlPanelAdapter::Core
{
  enum class Phase { kCreating, kReady, kFailed, kClosed };

  std::atomic<Phase> phase{Phase::kCreating};

  /// Guards everything below.
  mutable std::mutex mutex;
  qar_ar_core::UrlPanelDesc desc;
  QarGuiPanelId panel_id{};
  std::string common_name;
  std::string error;
  bool has_panel = false;
  bool reattached = false;
  /// A "url-place" task is queued; the newest placement asked for is in `wanted`.
  bool place_queued = false;
  bool has_wanted = false;
  qar_ar_core::UrlPanelPlacement wanted;
  std::vector<qar_ar_core::PanelOpResult> results;
  /// Keeps the panel in the state the adapter asked for; see panel_state_guard.hpp. Never null.
  std::shared_ptr<PanelStateKeeper> keeper;
};

QarCApiUrlPanelAdapter::QarCApiUrlPanelAdapter(
  IQarSessionOwner * owner, const qar_ar_core::UrlPanelDesc & desc)
: owner_(owner),
  title_(desc.display_name),
  core_(std::make_shared<Core>())
{
  core_->desc = desc;
  core_->common_name = desc.common_name;
  core_->keeper = std::make_shared<PanelStateKeeper>(
    owner, "qar-url-panel-adapter", title_.empty() ? std::string("untitled") : title_);

  // What can be checked without the runtime is refused at once.
  if (owner_ == nullptr) {
    refuse("no QarSession (is QAR streaming started?)");
    return;
  }
  if (desc.url.empty()) {
    refuse("panel URL must not be empty");
    return;
  }
  if (!(desc.placement.size.width_meters > 0.0f) || !(desc.placement.size.height_meters > 0.0f)) {
    refuse("panel size must be positive");
    return;
  }
  if (desc.common_name.empty()) {
    refuse("no common name for panel '" + desc.display_name + "'");
    return;
  }
  const std::shared_ptr<Core> core = core_;
  const bool posted = owner_->post_session_task(
    ("url-create:" + title_).c_str(),
    [core](QarSession * session) { run_create(session, core); },
    [core] {
      qar_ar_core::PanelOpResult result;
      result.op = qar_ar_core::PanelOpResult::Op::Create;
      result.ok = false;
      result.error = "the QarOS session ended before the panel was made";
      std::lock_guard<std::mutex> lock(core->mutex);
      core->error = result.error;
      core->results.push_back(result);
      Core::Phase expected = Core::Phase::kCreating;
      core->phase.compare_exchange_strong(expected, Core::Phase::kFailed);
    });
  if (!posted) {
    refuse("QAR session is not accepting calls");
  }
}

QarCApiUrlPanelAdapter::~QarCApiUrlPanelAdapter()
{
  close();
}

void
QarCApiUrlPanelAdapter::refuse(const std::string & reason)
{
  refused_ = true;
  last_error_ = reason;
  std::lock_guard<std::mutex> lock(core_->mutex);
  core_->error = reason;
  core_->phase.store(Core::Phase::kFailed, std::memory_order_release);
}

bool
QarCApiUrlPanelAdapter::ready() const
{
  return core_->phase.load(std::memory_order_acquire) == Core::Phase::kReady;
}

bool
QarCApiUrlPanelAdapter::pending() const
{
  return core_->phase.load(std::memory_order_acquire) == Core::Phase::kCreating;
}

QarGuiPanelId
QarCApiUrlPanelAdapter::panel_id() const
{
  std::lock_guard<std::mutex> lock(core_->mutex);
  return core_->panel_id;
}

bool
QarCApiUrlPanelAdapter::reattached() const
{
  std::lock_guard<std::mutex> lock(core_->mutex);
  return core_->reattached;
}

std::string
QarCApiUrlPanelAdapter::common_name() const
{
  std::lock_guard<std::mutex> lock(core_->mutex);
  return core_->common_name;
}

std::string
QarCApiUrlPanelAdapter::url() const
{
  std::lock_guard<std::mutex> lock(core_->mutex);
  return core_->desc.url;
}

void
QarCApiUrlPanelAdapter::drain_results(std::vector<qar_ar_core::PanelOpResult> & out)
{
  // The consumer calls this on every tick, which is when a panel that QarOS hid again inside the
  // hold of an ask and then left alone is asked about once more.
  core_->keeper->poll();
  std::vector<qar_ar_core::PanelOpResult> taken;
  {
    std::lock_guard<std::mutex> lock(core_->mutex);
    taken.swap(core_->results);
  }
  for (qar_ar_core::PanelOpResult & result : taken) {
    if (!result.ok && !result.error.empty()) {
      last_error_ = result.error;
    }
    out.push_back(std::move(result));
  }
}

// Chain QarGuiPanelUrlExt onto QarGuiPanelInit, get or create the panel, probe it and reattach it
// when it is an earlier one. Runs on the thread that owns the session. The failing step is
// recorded in Core::error and the phase becomes Failed.
void
QarCApiUrlPanelAdapter::run_create(QarSession * session, const std::shared_ptr<Core> & core)
{
  // An adapter closed or detached before its turn never made a panel: nothing is created, so the
  // hide and the detach queued behind this task find nothing to change and cost no call. One that
  // is closed while this task runs leaves the panel it made for those tasks (hide, detach).
  if (core->phase.load(std::memory_order_acquire) == Core::Phase::kClosed) {
    return;
  }
  const qar_ar_core::UrlPanelDesc desc = [&core] {
      std::lock_guard<std::mutex> lock(core->mutex);
      return core->desc;
    }();
  const auto fail = [&core](const std::string & error) {
      qar_ar_core::PanelOpResult result;
      result.op = qar_ar_core::PanelOpResult::Op::Create;
      result.ok = false;
      result.error = error;
      std::lock_guard<std::mutex> lock(core->mutex);
      core->error = error;
      core->results.push_back(result);
      Core::Phase expected = Core::Phase::kCreating;
      core->phase.compare_exchange_strong(expected, Core::Phase::kFailed);
    };

  // The panel's content IS the URL: chain QarGuiPanelUrlExt onto the init.
  // QarOS copies the string during the call, so a stack extension is fine.
  // Without any extension the runtime would open its default website instead.
  QarGuiPanelUrlExt url_ext = qar_gui_panel_url_ext_default();
  url_ext.url = desc.url.c_str();

  // The panel of this common name: created, or found again when an earlier session left it
  // behind. A name that is taken by a panel that cannot be reused is tried again with a suffix
  // appended to the base name: "-v2" for a stream panel, "-r1" .. "-r8" for closed panels.
  std::string common_name = desc.common_name;
  QarGuiPanelId panel_id{};
  qar_ar_core::PanelAttachDecision decision;
  qar_ar_core::PanelProbe probe;
  for (unsigned retries = 0;; ++retries) {
    QarGuiPanelInit panel_init = qar_gui_panel_init_default();
    panel_init.header.next = &url_ext;
    panel_init.common_name = common_name.c_str();
    panel_init.display_name = desc.display_name.empty() ? nullptr : desc.display_name.c_str();
    panel_init.pose = desc.placement.pose;
    panel_init.size = desc.placement.size;
    // An empty list leaves the init's NULL / 0: every peer sees the panel. desc is a local, so
    // the array outlives the call below.
    if (!desc.visible_to_peers.empty()) {
      panel_init.visible_to_peers = desc.visible_to_peers.data();
      panel_init.visible_to_peer_count = desc.visible_to_peers.size();
    }

    const QarResult r = QAR_CALL(qar_gui_panels_get_or_create, session, &panel_init, &panel_id);
    if (qar_result_is_error(r)) {
      fail(url_panel_result_error("qar_gui_panels_get_or_create", r));
      return;
    }

    probe = probe_url_panel(session, panel_id, desc.url, desc.placement.size);
    decision = qar_ar_core::decide_panel_attach(probe, retries);
    if (decision.kind != qar_ar_core::PanelAttachKind::RetryWithSuffix) {
      break;
    }
    std::fprintf(
      stderr, "[qar-url-panel-adapter] %s panel '%s' name=%s is taken by %s panel, trying %s%s\n",
      wall_clock_hms_ms().c_str(),
      desc.display_name.empty() ? "untitled" : desc.display_name.c_str(), common_name.c_str(),
      decision.reason == "closed" ? "a closed" : "a stream", desc.common_name.c_str(),
      decision.retry_suffix.c_str());
    std::fflush(stderr);
    common_name = desc.common_name + decision.retry_suffix;
  }
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->common_name = common_name;
    core->panel_id = panel_id;
  }
  if (decision.kind == qar_ar_core::PanelAttachKind::Refused) {
    fail(decision.reason == "closed" ?
      "closed panels of the names " + desc.common_name + " to " + common_name +
      " exist; no free name is left" :
      "a stream panel of the name " + common_name + " exists");
    return;
  }

  const bool reattached = decision.kind == qar_ar_core::PanelAttachKind::Reattached;
  if (reattached) {
    // The panel of an earlier session: its page, size and pose are set again and it is shown.
    if (decision.swap_content) {
      const QarResult r = QAR_CALL(
          qar_gui_panels_navigate_to_uri, session, &panel_id, desc.url.c_str());
      if (qar_result_is_error(r)) {
        fail(url_panel_result_error("qar_gui_panels_navigate_to_uri", r));
        return;
      }
    }
    if (decision.change_size) {
      const QarResult r = QAR_CALL(
          qar_gui_panels_change_size, session, &panel_id, &desc.placement.size);
      if (qar_result_is_error(r)) {
        fail(url_panel_result_error("qar_gui_panels_change_size", r));
        return;
      }
    }
    const QarResult posed = QAR_CALL(
        qar_gui_panels_update_pose, session, &panel_id, &desc.placement.pose);
    if (qar_result_is_error(posed)) {
      fail(url_panel_result_error("qar_gui_panels_update_pose", posed));
      return;
    }
    // get_or_create applies the visible-to list of its init to a new panel only; the list of a
    // panel that was there is brought to the wanted one.
    std::string visible_error;
    if (!apply_visible_to(session, panel_id, desc.visible_to_peers, visible_error)) {
      fail(visible_error);
      return;
    }
    if (decision.show) {
      const QarResult shown =
        QAR_CALL(qar_gui_panels_set_state, session, &panel_id, QAR_GUI_PANEL_STATE_VISIBLE);
      if (qar_result_is_error(shown)) {
        fail(url_panel_result_error("qar_gui_panels_set_state", shown));
        return;
      }
    }
  }
  // Marked only now: a step of the reattach that failed leaves the panel of the earlier session
  // alone, because the consumer drops a failed adapter and its teardown closes the panel it owns.
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->has_panel = true;
    core->reattached = reattached;
  }

  // How the panel came to be, in the format of the stream panels' line. The page address is not
  // printed: it carries the access token of the page.
  qar_ar_core::PanelAttachLogFields fields;
  fields.time = wall_clock_hms_ms();
  fields.key = desc.display_name.empty() ? std::string("untitled") : desc.display_name;
  fields.reattached = reattached;
  fields.common_name = common_name;
  fields.panel_id = panel_id_uuid(panel_id);
  if (probe.found) {
    fields.state_was = qar_ar_core::panel_state_name(probe.state);
  }
  fields.stream_swapped = reattached && decision.swap_content;
  fields.size_changed = reattached && decision.change_size;
  std::fprintf(stderr, "%s\n", qar_ar_core::panel_attach_log_line(fields).c_str());
  std::fflush(stderr);

  // The panel is shown, so VISIBLE is the state to keep. A runtime that reports the panel hidden
  // right after the adapter showed it is asked again. A watch that cannot start is reported and
  // the panel works without it.
  core->keeper->start(session, panel_id);

  qar_ar_core::PanelOpResult done;
  done.op = qar_ar_core::PanelOpResult::Op::Create;
  done.ok = true;
  done.reattached = reattached;
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->error.clear();
    core->results.push_back(done);
  }
  Core::Phase expected = Core::Phase::kCreating;
  core->phase.compare_exchange_strong(expected, Core::Phase::kReady);
}

void
QarCApiUrlPanelAdapter::run_navigate(
  QarSession * session, const std::shared_ptr<Core> & core, const std::string & url)
{
  QarGuiPanelId panel_id{};
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    if (!core->has_panel || core->phase.load() == Core::Phase::kFailed) {
      return;
    }
    panel_id = core->panel_id;
  }
  qar_ar_core::PanelOpResult result;
  result.op = qar_ar_core::PanelOpResult::Op::Navigate;
  const QarResult r = QAR_CALL(qar_gui_panels_navigate_to_uri, session, &panel_id, url.c_str());
  if (qar_result_is_error(r)) {
    result.ok = false;
    result.error = url_panel_result_error("qar_gui_panels_navigate_to_uri", r);
  }
  std::lock_guard<std::mutex> lock(core->mutex);
  if (result.ok) {
    core->desc.url = url;
  }
  core->results.push_back(std::move(result));
}

void
QarCApiUrlPanelAdapter::run_place(QarSession * session, const std::shared_ptr<Core> & core)
{
  qar_ar_core::UrlPanelPlacement placement;
  QarGuiPanelId panel_id{};
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    core->place_queued = false;
    if (!core->has_wanted) {
      return;
    }
    core->has_wanted = false;
    placement = core->wanted;
    panel_id = core->panel_id;
    if (!core->has_panel || core->phase.load() == Core::Phase::kFailed) {
      return;
    }
  }
  qar_ar_core::PanelOpResult result;
  result.op = qar_ar_core::PanelOpResult::Op::Placement;
  const QarResult posed = QAR_CALL(qar_gui_panels_update_pose, session, &panel_id, &placement.pose);
  if (qar_result_is_error(posed)) {
    result.ok = false;
    result.error = url_panel_result_error("qar_gui_panels_update_pose", posed);
  } else {
    const QarResult sized = QAR_CALL(
        qar_gui_panels_change_size, session, &panel_id, &placement.size);
    if (qar_result_is_error(sized)) {
      result.ok = false;
      result.error = url_panel_result_error("qar_gui_panels_change_size", sized);
    }
  }
  std::lock_guard<std::mutex> lock(core->mutex);
  if (result.ok) {
    core->desc.placement = placement;
  }
  core->results.push_back(std::move(result));
}

void
QarCApiUrlPanelAdapter::run_state(
  QarSession * session, const std::shared_ptr<Core> & core, QarGuiPanelState state)
{
  // Whatever this task finds to do, the keeper learns that the request has been served.
  struct Served
  {
    PanelStateKeeper & keeper;
    ~Served() { keeper.request_done(); }
  } served{*core->keeper};
  QarGuiPanelId panel_id{};
  std::string title;
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    title = core->desc.display_name;
    if (!core->has_panel || core->phase.load() == Core::Phase::kFailed) {
      return;
    }
    panel_id = core->panel_id;
  }
  qar_ar_core::PanelOpResult result;
  result.op = qar_ar_core::PanelOpResult::Op::State;
  result.state = state;
  const QarResult r = QAR_CALL(qar_gui_panels_set_state, session, &panel_id, state);
  if (qar_result_is_error(r)) {
    result.ok = false;
    result.error = url_panel_result_error("qar_gui_panels_set_state", r);
    log_url_panel_state(title, &panel_id, state, result.error);
  } else {
    log_url_panel_state(title, &panel_id, state, std::string());
  }
  std::lock_guard<std::mutex> lock(core->mutex);
  core->results.push_back(std::move(result));
}

void
QarCApiUrlPanelAdapter::run_close(QarSession * session, const std::shared_ptr<Core> & core)
{
  // The watch of the panel's state ends first, so the runtime's republishing of the panel that
  // this close causes asks for nothing.
  core->keeper->stop();
  QarGuiPanelId panel_id{};
  {
    std::lock_guard<std::mutex> lock(core->mutex);
    if (!core->has_panel) {
      return;
    }
    panel_id = core->panel_id;
    core->has_panel = false;
  }
  // Checked and reported like the close of a stream panel: a close that failed leaves
  // the panel in the session, and the line says so.
  const QarResult closed = QAR_CALL(qar_gui_panels_close_panel, session, &panel_id);
  if (qar_result_is_error(closed)) {
    const std::string error = url_panel_result_error("qar_gui_panels_close_panel", closed);
    std::lock_guard<std::mutex> lock(core->mutex);
    core->error = error;
  }
}

bool
QarCApiUrlPanelAdapter::live(const char * what)
{
  const Core::Phase phase = core_->phase.load(std::memory_order_acquire);
  if (closed_ || (phase != Core::Phase::kCreating && phase != Core::Phase::kReady)) {
    last_error_ = what;
    return false;
  }
  return true;
}

bool
QarCApiUrlPanelAdapter::navigate(const std::string & url)
{
  if (!live("no live panel to navigate")) {
    return false;
  }
  if (url.empty()) {
    last_error_ = "panel URL must not be empty";
    return false;
  }
  const std::shared_ptr<Core> core = core_;
  const bool posted = owner_->post_session_task(
    ("url-navigate:" + title_).c_str(),
    [core, url](QarSession * session) { run_navigate(session, core, url); });
  if (!posted) {
    last_error_ = "QAR session is not accepting calls";
    return false;
  }
  last_error_.clear();
  return true;
}

bool
QarCApiUrlPanelAdapter::set_placement(const qar_ar_core::UrlPanelPlacement & placement)
{
  if (!live("no live panel to place")) {
    return false;
  }
  bool queue = false;
  {
    // The newest placement wins: one task is queued at most and applies what was asked last.
    std::lock_guard<std::mutex> lock(core_->mutex);
    core_->wanted = placement;
    core_->has_wanted = true;
    queue = !core_->place_queued;
    core_->place_queued = true;
  }
  if (queue) {
    const std::shared_ptr<Core> core = core_;
    const bool posted = owner_->post_session_task(
      ("url-place:" + title_).c_str(),
      [core](QarSession * session) { run_place(session, core); },
      [core] {
        std::lock_guard<std::mutex> lock(core->mutex);
        core->place_queued = false;
        core->has_wanted = false;
      });
    if (!posted) {
      std::lock_guard<std::mutex> lock(core_->mutex);
      core_->place_queued = false;
      core_->has_wanted = false;
      last_error_ = "QAR session is not accepting calls";
      return false;
    }
  }
  last_error_.clear();
  return true;
}

bool
QarCApiUrlPanelAdapter::set_state(QarGuiPanelState state)
{
  if (!live("no live panel to change")) {
    log_url_panel_state(title_, nullptr, state, last_error_);
    return false;
  }
  const std::shared_ptr<Core> core = core_;
  core->keeper->request(state);
  const bool posted = owner_->post_session_task(
    ("url-state:" + title_).c_str(),
    [core, state](QarSession * session) { run_state(session, core, state); },
    [core] { core->keeper->request_dropped(); });
  if (!posted) {
    core->keeper->request_dropped();
    last_error_ = "QAR session is not accepting calls";
    log_url_panel_state(title_, nullptr, state, last_error_);
    return false;
  }
  last_error_.clear();
  return true;
}

void
QarCApiUrlPanelAdapter::set_state_guard_clock(PanelStateKeeper::ClockFn now)
{
  core_->keeper->set_clock(std::move(now));
}

void
QarCApiUrlPanelAdapter::detach()
{
  // The QarOS panel stays as it is (the caller hid it); the next session finds it under its
  // common name. The tasks that are queued ahead (the hide) still run, and none is accepted after
  // this. The one task queued here ends the watch of the panel's state.
  if (closed_) {
    return;
  }
  closed_ = true;
  core_->phase.store(Core::Phase::kClosed, std::memory_order_release);
  core_->keeper->retire();
  if (owner_ == nullptr) {
    return;
  }
  const std::shared_ptr<Core> core = core_;
  owner_->post_session_task(
    ("url-detach:" + title_).c_str(),
    [core](QarSession *) { core->keeper->stop(); },
    [core] { core->keeper->abandon(); });
}

void
QarCApiUrlPanelAdapter::close()
{
  if (closed_) {
    return;
  }
  closed_ = true;
  core_->phase.store(Core::Phase::kClosed, std::memory_order_release);
  core_->keeper->retire();
  if (owner_ == nullptr) {
    return;
  }
  const std::shared_ptr<Core> core = core_;
  owner_->post_session_task(
    ("url-close:" + title_).c_str(),
    [core](QarSession * session) { run_close(session, core); },
    [core] { core->keeper->abandon(); });
}

QarCApiUrlPanelFactory::QarCApiUrlPanelFactory(IQarSessionOwner * session_owner)
: session_owner_(session_owner)
{
}

std::shared_ptr<qar_ar_core::IQarUrlPanelAdapter>
QarCApiUrlPanelFactory::create_url_panel(const qar_ar_core::UrlPanelDesc & desc)
{
  if (session_owner_ == nullptr) {
    last_error_ = "no QAR session owner";
    return nullptr;
  }
  if (!session_owner_->ready()) {
    last_error_ = "QAR session is not ready yet";
    return nullptr;
  }
  if (session_owner_->session() == nullptr) {
    last_error_ = "QAR session handle is null";
    return nullptr;
  }

  // The panel is made on the thread that owns the session; the adapter is handed back at once,
  // still "creating".
  auto adapter = std::make_shared<QarCApiUrlPanelAdapter>(session_owner_, desc);
  if (adapter->refused()) {
    last_error_ = adapter->last_error();
    return nullptr;
  }
  last_error_.clear();
  return adapter;
}

}  // namespace qar_qar_adapter
