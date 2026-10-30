// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_ar_core/common_name.hpp"

namespace qar_ar_core
{
namespace
{

/// A label is never empty in a name: an empty one would leave two dots side by side.
std::string
non_empty_label(const std::string & utf8, std::size_t max_len)
{
  std::string label = sanitize_label(utf8, max_len);
  if (label.empty()) {
    label = "x";
  }
  return label;
}

/// `.<program>[.<instance>].<host>.qaros-ros`, the tail both kinds of name share.
std::string
scope_tail(const NameScope & scope)
{
  std::string tail = non_empty_label(scope.program_tag, kNameScopeProgramMax);
  if (!scope.instance.empty()) {
    tail += '.';
    tail += non_empty_label(scope.instance, kNameScopeInstanceMax);
  }
  tail += '.';
  tail += non_empty_label(scope.host, kNameScopeHostMax);
  tail += ".qaros-ros";
  return tail;
}

}  // namespace

std::string
sanitize_label(const std::string & utf8, std::size_t max_len)
{
  std::string out;
  out.reserve(utf8.size() < max_len ? utf8.size() : max_len);
  for (const char raw : utf8) {
    if (out.size() >= max_len) {
      break;
    }
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c));
    } else if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if ((c >= '0' && c <= '9') || c == '-') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('-');
    }
  }
  return out;
}

NameScope
make_name_scope(
  const std::string & program_tag, const std::string & instance, const std::string & host_utf8)
{
  NameScope scope;
  scope.program_tag = sanitize_label(program_tag, kNameScopeProgramMax);
  scope.instance = sanitize_label(instance, kNameScopeInstanceMax);
  scope.host = sanitize_label(host_utf8, kNameScopeHostMax);
  return scope;
}

std::uint32_t
fnv1a32(const std::string & bytes)
{
  std::uint32_t hash = 2166136261u;
  for (const char raw : bytes) {
    hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(raw));
    hash *= 16777619u;
  }
  return hash;
}

std::string
make_panel_common_name(const std::string & key, const NameScope & scope)
{
  static const char kHex[] = "0123456789abcdef";
  const std::uint32_t hash = fnv1a32(key);
  std::string name = sanitize_label(key, kPanelKeyLabelMax);
  name += '-';
  for (int shift = 28; shift >= 0; shift -= 4) {
    name.push_back(kHex[(hash >> shift) & 0x0Fu]);
  }
  name += '.';
  name += scope_tail(scope);
  return name;
}

std::string
make_volume_common_name(const NameScope & scope)
{
  return "scene." + scope_tail(scope);
}

}  // namespace qar_ar_core
