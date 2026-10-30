// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__COMMON_NAME_HPP_
#define QAR_AR_CORE__COMMON_NAME_HPP_

/// @file common_name.hpp
/// @brief Stable ASCII common names for QarOS GUI panels and the app volume.
///
/// `qar_gui_panels_get_or_create` and `qar_app_volumes_get_or_create` derive the id of the
/// object from a required, stable, ASCII common name: the same name finds the same object
/// again in the next session, so a panel that was hidden at the end of one run is found and
/// reattached by the next. The names built here are DNS-style, "<object>.<program>.<host>.<org>".
/// Pure string functions, Qt-free and QarOS-free.

#include <cstddef>
#include <cstdint>
#include <string>

namespace qar_ar_core
{

/// @brief The part of a common name that tells programs, instances and machines apart.
///
/// Every member is already sanitized (see @ref make_name_scope).
struct NameScope
{
  std::string program_tag;  ///< Which program: "rviz2-plugin", "qt-panel-example", ...
  std::string instance;     ///< Optional second copy of the same program; empty for none.
  std::string host;         ///< The DNS host name of this PC.
};

/// @brief Longest `program_tag`, `instance` and `host` a scope keeps, in characters.
constexpr std::size_t kNameScopeProgramMax = 24;
constexpr std::size_t kNameScopeInstanceMax = 16;
constexpr std::size_t kNameScopeHostMax = 24;
/// @brief Longest panel key part of a panel common name, in characters.
constexpr std::size_t kPanelKeyLabelMax = 32;
/// @brief No common name made here is longer than this many characters.
constexpr std::size_t kCommonNameMax = 120;

/// @brief Builds the scope: each argument goes through @ref sanitize_label with its own
/// length cap.
/// @param host_utf8 The DNS host name as UTF-8; any non-ASCII byte becomes '-'.
NameScope make_name_scope(
  const std::string & program_tag, const std::string & instance, const std::string & host_utf8);

/// @brief Lower-cases ASCII letters, keeps digits and '-', turns every other byte into '-',
/// and cuts the result to @p max_len characters.
std::string sanitize_label(const std::string & utf8, std::size_t max_len);

/// @brief 32-bit FNV-1a hash of @p bytes (offset basis 2166136261, prime 16777619).
std::uint32_t fnv1a32(const std::string & bytes);

/// @brief `<key>-<hash8>.<program>[.<instance>].<host>.qaros-ros`.
///
/// `<key>` is the panel key sanitized to @ref kPanelKeyLabelMax characters and `<hash8>` is
/// the FNV-1a hash of the UNsanitized key in eight lower-case hex digits, so two keys that
/// sanitize alike stay apart. ASCII only, at most @ref kCommonNameMax characters.
std::string make_panel_common_name(const std::string & key, const NameScope & scope);

/// @brief `scene.<program>[.<instance>].<host>.qaros-ros`, the common name of the app volume.
std::string make_volume_common_name(const NameScope & scope);

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__COMMON_NAME_HPP_
