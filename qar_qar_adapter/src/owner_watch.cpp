// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/owner_watch.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "qar_qar_adapter/log_time.hpp"
#include "qar_qar_adapter/qar_session_owner.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace qar_qar_adapter
{

namespace
{

constexpr std::uint32_t kSilentBits = kSilenceHubPeersGone | kSilenceCallStalled;

struct ProcessName
{
  const char * stem;          // as the log line spells it
  std::uint32_t HubHints::* pid;
};

constexpr ProcessName kProcesses[] = {
  {"qaros-hub-service", &HubHints::hub_pid},
  {"qar-runtime-launcher", &HubHints::launcher_pid},
  {"qar-streaming-viz", &HubHints::viz_pid},
  {"qar-stereokit-gui", &HubHints::gui_pid},
};

std::string pid_text(std::uint32_t pid)
{
  return std::to_string(static_cast<unsigned long>(pid));
}

}  // namespace

std::string hub_endpoint_path()
{
#ifdef _WIN32
  char * buffer = nullptr;
  std::size_t size = 0;
  std::string base;
  if (_dupenv_s(&buffer, &size, "LOCALAPPDATA") == 0 && buffer != nullptr) {
    base = buffer;
  }
  std::free(buffer);
  if (base.empty()) {
    return std::string();
  }
  return (std::filesystem::u8path(base) / "Quaternar" / "Qaros" / "hub-endpoint.json").u8string();
#else
  return std::string();
#endif
}

HubHints read_hub_hints()
{
  const auto started = std::chrono::steady_clock::now();
  HubHints out;
  const std::string path = hub_endpoint_path();
  if (!path.empty()) {
    // Status only: the file holds a key and is never opened.
    const std::filesystem::path file = std::filesystem::u8path(path);
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(file, ec);
    if (!ec || ec == std::errc::no_such_file_or_directory) {
      out.endpoint_known = true;
      out.endpoint_present = !ec && std::filesystem::is_regular_file(status);
      if (out.endpoint_present) {
        std::error_code time_ec;
        const auto written = std::filesystem::last_write_time(file, time_ec);
        if (!time_ec) {
          out.endpoint_mtime = static_cast<std::int64_t>(written.time_since_epoch().count());
          const auto offset = std::chrono::duration_cast<std::chrono::system_clock::duration>(
            written - std::filesystem::file_time_type::clock::now());
          out.endpoint_written =
            hms_ms_of(std::chrono::system_clock::now() + offset).substr(0, 8);
        }
        std::error_code size_ec;
        const std::uintmax_t size = std::filesystem::file_size(file, size_ec);
        out.endpoint_size = size_ec ? 0 : size;
      }
    }
  }
#ifdef _WIN32
  // One snapshot of the process list: names and ids are read, no process is opened.
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    out.processes_known = true;
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
      more = Process32NextW(snapshot, &entry))
    {
      std::string name;
      for (const wchar_t * c = entry.szExeFile; *c != L'\0'; ++c) {
        name.push_back(*c < 128 ? static_cast<char>(std::tolower(static_cast<int>(*c))) : '?');
      }
      for (const ProcessName & known : kProcesses) {
        if (name == std::string(known.stem) + ".exe" && out.*(known.pid) == 0) {
          out.*(known.pid) = entry.th32ProcessID;
        }
      }
    }
    CloseHandle(snapshot);
  }
#endif
  out.scan_ms = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - started).count();
  return out;
}

std::vector<std::string> describe_hint_changes(const HubHints & before, const HubHints & now)
{
  std::vector<std::string> lines;
  if (now.endpoint_known) {
    const bool first = !before.endpoint_known;
    if (!now.endpoint_present) {
      if (first || before.endpoint_present) {
        lines.push_back("hint: hub endpoint file missing");
      }
    } else if (first || !before.endpoint_present || before.endpoint_mtime != now.endpoint_mtime) {
      lines.push_back("hint: hub endpoint file present (written " + now.endpoint_written + ")");
    }
  }
  if (now.processes_known) {
    const bool first = !before.processes_known;
    for (const ProcessName & known : kProcesses) {
      const std::uint32_t was = before.*(known.pid);
      const std::uint32_t is = now.*(known.pid);
      if (is != 0 && (first || was == 0)) {
        lines.push_back(
          std::string("process ") + known.stem + " running (pid " + pid_text(is) + ")");
      } else if (is == 0 && !first && was != 0) {
        lines.push_back(
          std::string("process ") + known.stem + " gone (pid " + pid_text(was) + ")");
      }
    }
  }
  return lines;
}

WatchVerdict OutagePolicy::judge(const WatchInput & in) const
{
  WatchVerdict out;
  const std::int64_t call_limit = timings_.call_stall.count();
  const std::int64_t task_limit = timings_.task_stall.count();
  const bool call_stalled = in.call_ms >= 0 && in.call_ms >= call_limit;
  const bool task_stalled = in.task_ms >= 0 && in.task_ms >= task_limit;
  out.call_stalled = call_stalled || task_stalled;
  if (call_stalled) {
    out.stalled_in = in.call;
    out.stalled_ms = in.call_ms;
  } else if (task_stalled) {
    out.stalled_in = "task " + in.task;
    out.stalled_ms = in.task_ms;
  }
  // Only a call is left behind: a task is a list of calls, and the call that blocks is the one
  // the owner thread is inside.
  out.abandon_due = in.call_ms >= 0 && in.call_ms >= timings_.abandon_after.count();
  if (in.joined && (in.silence & kSilentBits) != 0) {
    const bool hint = in.endpoint_gone_long || in.hub_process_gone_long;
    const std::int64_t limit =
      (hint ? timings_.lost_after_hint : timings_.lost_after).count();
    out.lost_due = in.silent_ms >= limit;
  }
  out.rejoin_now = in.lost && in.hub_came_back;
  return out;
}

}  // namespace qar_qar_adapter
