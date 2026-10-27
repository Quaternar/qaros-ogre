// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The file that remembers a pairing with the QAROS Hub of another PC: the onboarding id the
// runtime returned, and the host and port of the Hub that was paired. It holds no secret (the
// pairing code is never stored). Plain file work, no QarOS call.
//
// Format: UTF-8, LF, one `key=value` per line:
//   format=1
//   onboarding_id=<uuid text>
//   hub_host=<host>
//   hub_port=<port>
//   paired_unix_s=<seconds since 1970>

#ifndef QAR_QAR_ADAPTER__IDENTITY_STORE_HPP_
#define QAR_QAR_ADAPTER__IDENTITY_STORE_HPP_

#include <cstdint>
#include <string>

namespace qar_qar_adapter
{

/// @brief What the identity file holds.
struct IdentityRecord
{
  std::string id;                  ///< The onboarding id as UUID text.
  std::string host;                ///< Host name or address of the paired Hub.
  std::uint16_t port = 0;          ///< Onboarding port of the paired Hub.
  std::int64_t paired_unix_s = 0;  ///< When the pairing happened; 0 when unknown.
};

/// @brief The outcome of reading the file.
enum class IdentityRead
{
  kOk,         ///< The record was read.
  kAbsent,     ///< There is no file.
  kMalformed   ///< The file exists but cannot be used; the error text says why.
};

/// @brief The folder one program keeps its pairing and its runtime storage in:
/// `<base>\<program_tag>[.<instance>]`. The base is the environment variable
/// QAR_PANEL_IDENTITY_DIR when it is set and not empty, else
/// `%LOCALAPPDATA%\Quaternar\QarOS-ROS` (the known folder of the Windows account, as UTF-8).
/// The tag and the instance keep only letters, digits, '-' and '_'; any other character becomes
/// '-'. Empty when no base can be found.
std::string default_identity_dir(const std::string & program_tag, const std::string & instance);

/// @brief `<dir>\identity.txt`.
std::string identity_file_path(const std::string & dir);

/// @brief Reads @p path_utf8.
IdentityRead read_identity(
  const std::string & path_utf8, IdentityRecord & out, std::string & error);

/// @brief Writes @p record to @p path_utf8 so that a reader sees either the file as it was or
/// the complete replacement: the text goes to `<path>.tmp`, is flushed, and replaces the file in
/// one step. Makes the folder when it is missing. On failure the file stays untouched.
bool write_identity_atomic(
  const std::string & path_utf8, const IdentityRecord & record, std::string & error);

/// @brief Deletes the file. @return true when it is gone afterwards (also when it was absent).
bool remove_identity(const std::string & path_utf8);

/// @brief The text of @p record in the file format.
std::string format_identity(const IdentityRecord & record);

/// @brief Parses the file format.
IdentityRead parse_identity(const std::string & text, IdentityRecord & out, std::string & error);

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__IDENTITY_STORE_HPP_
