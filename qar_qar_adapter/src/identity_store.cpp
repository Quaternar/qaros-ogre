// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/identity_store.hpp"

#include "qar_qar_adapter/env_config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace qar_qar_adapter
{

namespace
{

namespace fs = std::filesystem;

constexpr const char * kFileName = "identity.txt";
constexpr const char * kOverrideVariable = "QAR_PANEL_IDENTITY_DIR";

fs::path to_path(const std::string & utf8)
{
  return fs::u8path(utf8);
}

std::string from_path(const fs::path & path)
{
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::string sanitize_label(const std::string & text)
{
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    out.push_back((std::isalnum(u) != 0 || c == '-' || c == '_') ? c : '-');
  }
  return out;
}

std::string trim(const std::string & text)
{
  const std::size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos) {
    return std::string();
  }
  const std::size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

#ifdef _WIN32
// The per-user application data folder of the Windows account, as UTF-8: the profile's
// LOCALAPPDATA, else AppData/Local under the profile folder.
std::string local_app_data_utf8()
{
  std::string folder = read_environment("LOCALAPPDATA");
  if (folder.empty()) {
    const std::string profile = read_environment("USERPROFILE");
    if (!profile.empty()) {
      folder = from_path(to_path(profile) / "AppData" / "Local");
    }
  }
  return folder;
}
#endif

}  // namespace

std::string default_identity_dir(const std::string & program_tag, const std::string & instance)
{
  std::string base;
  const std::string override_dir = read_environment(kOverrideVariable);
  if (!override_dir.empty()) {
    base = override_dir;
  } else {
#ifdef _WIN32
    const std::string local = local_app_data_utf8();
    if (!local.empty()) {
      base = from_path(to_path(local) / "Quaternar" / "QarOS-ROS");
    }
#else
    const std::string home = read_environment("HOME");
    if (!home.empty()) {
      base = from_path(fs::path(home) / ".local" / "share" / "Quaternar" / "QarOS-ROS");
    }
#endif
  }
  if (base.empty()) {
    return std::string();
  }
  std::string leaf = sanitize_label(program_tag.empty() ? std::string("program") : program_tag);
  if (!instance.empty()) {
    leaf += "." + sanitize_label(instance);
  }
  return from_path(to_path(base) / leaf);
}

std::string identity_file_path(const std::string & dir)
{
  return from_path(to_path(dir) / kFileName);
}

std::string format_identity(const IdentityRecord & record)
{
  std::ostringstream text;
  text << "format=1\n"
       << "onboarding_id=" << record.id << "\n"
       << "hub_host=" << record.host << "\n"
       << "hub_port=" << record.port << "\n"
       << "paired_unix_s=" << record.paired_unix_s << "\n";
  return text.str();
}

IdentityRead parse_identity(const std::string & text, IdentityRecord & out, std::string & error)
{
  IdentityRecord parsed;
  bool has_format = false;
  bool has_port = false;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const std::string stripped = trim(line);
    if (stripped.empty()) {
      continue;
    }
    const std::size_t eq = stripped.find('=');
    if (eq == std::string::npos) {
      error = "a line without '=': " + stripped.substr(0, 40);
      return IdentityRead::kMalformed;
    }
    const std::string key = trim(stripped.substr(0, eq));
    const std::string value = trim(stripped.substr(eq + 1));
    if (key == "format") {
      if (value != "1") {
        error = "unknown format " + value;
        return IdentityRead::kMalformed;
      }
      has_format = true;
    } else if (key == "onboarding_id") {
      parsed.id = value;
    } else if (key == "hub_host") {
      parsed.host = value;
    } else if (key == "hub_port") {
      char * end = nullptr;
      const long port = std::strtol(value.c_str(), &end, 10);
      if (value.empty() || end == nullptr || *end != '\0' || port < 1 || port > 65535) {
        error = "hub_port is not a port number";
        return IdentityRead::kMalformed;
      }
      parsed.port = static_cast<std::uint16_t>(port);
      has_port = true;
    } else if (key == "paired_unix_s") {
      char * end = nullptr;
      const long long seconds = std::strtoll(value.c_str(), &end, 10);
      if (!value.empty() && end != nullptr && *end == '\0') {
        parsed.paired_unix_s = seconds;
      }
    }
  }
  if (!has_format) {
    error = "no format line";
    return IdentityRead::kMalformed;
  }
  if (parsed.id.empty()) {
    error = "no onboarding_id";
    return IdentityRead::kMalformed;
  }
  if (parsed.host.empty() || !has_port) {
    error = "no hub_host or hub_port";
    return IdentityRead::kMalformed;
  }
  out = parsed;
  return IdentityRead::kOk;
}

IdentityRead read_identity(const std::string & path_utf8, IdentityRecord & out, std::string & error)
{
  std::error_code ec;
  const fs::path path = to_path(path_utf8);
  if (!fs::exists(path, ec)) {
    return IdentityRead::kAbsent;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "the file cannot be opened";
    return IdentityRead::kMalformed;
  }
  std::ostringstream content;
  content << file.rdbuf();
  return parse_identity(content.str(), out, error);
}

bool write_identity_atomic(
  const std::string & path_utf8, const IdentityRecord & record, std::string & error)
{
  const fs::path path = to_path(path_utf8);
  std::error_code ec;
  if (path.has_parent_path()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
      error = "the folder cannot be made: " + ec.message();
      return false;
    }
  }
  const fs::path tmp = fs::path(path.native() + fs::path(".tmp").native());
  const std::string text = format_identity(record);
#ifdef _WIN32
  HANDLE file = CreateFileW(
    tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = "the temporary file cannot be opened (error " + std::to_string(GetLastError()) + ")";
    return false;
  }
  DWORD written = 0;
  bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 &&
    written == text.size();
  ok = (FlushFileBuffers(file) != 0) && ok;
  ok = (CloseHandle(file) != 0) && ok;
#else
  FILE * file = std::fopen(tmp.c_str(), "wb");
  if (file == nullptr) {
    error = "the temporary file cannot be opened";
    return false;
  }
  bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
  ok = (std::fflush(file) == 0) && ok;
  ok = (std::fclose(file) == 0) && ok;
#endif
  if (!ok) {
    error = "the temporary file cannot be written";
    fs::remove(tmp, ec);
    return false;
  }
#ifdef _WIN32
  if (MoveFileExW(
      tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
  {
    error = "the file cannot be replaced (error " + std::to_string(GetLastError()) + ")";
    fs::remove(tmp, ec);
    return false;
  }
#else
  fs::rename(tmp, path, ec);
  if (ec) {
    error = "the file cannot be replaced: " + ec.message();
    fs::remove(tmp, ec);
    return false;
  }
#endif
  return true;
}

bool remove_identity(const std::string & path_utf8)
{
  std::error_code ec;
  const fs::path path = to_path(path_utf8);
  fs::remove(path, ec);
  return !fs::exists(path, ec);
}

}  // namespace qar_qar_adapter
