// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Common names of QarOS panels and the app volume: ASCII, stable, bounded, and distinct for
// distinct keys.

#include "qar_ar_core/common_name.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <set>
#include <string>

using qar_ar_core::fnv1a32;
using qar_ar_core::kCommonNameMax;
using qar_ar_core::make_name_scope;
using qar_ar_core::make_panel_common_name;
using qar_ar_core::make_volume_common_name;
using qar_ar_core::NameScope;
using qar_ar_core::sanitize_label;

namespace
{

bool
is_ascii_name(const std::string & name)
{
  for (const char raw : name) {
    const unsigned char c = static_cast<unsigned char>(raw);
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
    if (!ok) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST(CommonName, SanitizeKeepsLowerCaseLettersDigitsAndDashesAndMapsTheRestToDash)
{
  EXPECT_EQ(sanitize_label("Dialog:Open File/1", 64), "dialog-open-file-1");
  EXPECT_EQ(sanitize_label("a-b-9", 64), "a-b-9");
  EXPECT_EQ(sanitize_label("", 8), "");
}

TEST(CommonName, SanitizeTurnsEveryByteOfAMultiByteCharacterIntoADash)
{
  // "e" + U+00E9 (two bytes in UTF-8) + "x".
  EXPECT_EQ(sanitize_label("e\xC3\xA9x", 16), "e--x");
  EXPECT_TRUE(is_ascii_name(sanitize_label("\xE2\x82\xAC\x7F\x01 host", 32)));
}

TEST(CommonName, SanitizeCutsToTheLimit)
{
  EXPECT_EQ(sanitize_label("abcdefghij", 4), "abcd");
  EXPECT_EQ(sanitize_label("abc", 0), "");
}

TEST(CommonName, Fnv1aMatchesTheReferenceVectors)
{
  EXPECT_EQ(fnv1a32(""), 0x811C9DC5u);
  EXPECT_EQ(fnv1a32("a"), 0xE40C292Cu);
  EXPECT_EQ(fnv1a32("foobar"), 0xBF9CF968u);
}

TEST(CommonName, ScopeIsSanitizedAndCapped)
{
  const NameScope scope = make_name_scope(
    "RViz2 Plugin", "Second Copy Of It, Really",
    "PC-\xC3\xA9-01.with.a.very.long.host.name.example");
  EXPECT_EQ(scope.program_tag, "rviz2-plugin");
  EXPECT_EQ(scope.instance.size(), 16u);
  EXPECT_EQ(scope.instance, "second-copy-of-i");
  EXPECT_EQ(scope.host.size(), 24u);
  EXPECT_TRUE(is_ascii_name(scope.host));
}

TEST(CommonName, PanelNameHasTheDocumentedShape)
{
  const NameScope scope = make_name_scope("rviz2-plugin", "", "devpc");
  const std::string key = "hidden:Displays";
  const std::string name = make_panel_common_name(key, scope);
  char hash[9] = {0};
  std::snprintf(hash, sizeof(hash), "%08x", static_cast<unsigned>(fnv1a32(key)));
  EXPECT_EQ(name, "hidden-displays-" + std::string(hash) + ".rviz2-plugin.devpc.qaros-ros");

  const NameScope with_instance = make_name_scope("rviz2-plugin", "B", "devpc");
  EXPECT_EQ(
    make_panel_common_name(key, with_instance),
    "hidden-displays-" + std::string(hash) + ".rviz2-plugin.b.devpc.qaros-ros");
}

TEST(CommonName, TheHashKeepsKeysThatSanitizeAlikeApart)
{
  const NameScope scope = make_name_scope("p", "", "h");
  const std::string a = make_panel_common_name("Tool:Options", scope);
  const std::string b = make_panel_common_name("tool/options", scope);
  const std::string c = make_panel_common_name("tool options", scope);
  EXPECT_EQ(a.substr(0, 13), b.substr(0, 13));  // "tool-options-" alike
  std::set<std::string> distinct{a, b, c};
  EXPECT_EQ(distinct.size(), 3u);
}

TEST(CommonName, NamesAreAsciiBoundedAndStable)
{
  // A scope built by hand with oversized, unclean members is cleaned again when a name is made.
  NameScope scope;
  scope.program_tag = std::string(80, 'P');
  scope.instance = std::string(80, 'I');
  scope.host = "Host \xC3\xA9 name that is far too long to be a label";
  const std::string key(300, 'K');
  const std::string name = make_panel_common_name(key, scope);
  EXPECT_LE(name.size(), kCommonNameMax);
  EXPECT_TRUE(is_ascii_name(name));
  EXPECT_EQ(name, make_panel_common_name(key, scope));
  EXPECT_NE(name.find(".qaros-ros"), std::string::npos);
  EXPECT_LE(make_volume_common_name(scope).size(), kCommonNameMax);
}

TEST(CommonName, VolumeNameIsTheSceneOfTheScope)
{
  const NameScope scope = make_name_scope("qt-panel-example", "", "devpc");
  EXPECT_EQ(make_volume_common_name(scope), "scene.qt-panel-example.devpc.qaros-ros");
  const NameScope second = make_name_scope("qt-panel-example", "2", "devpc");
  EXPECT_EQ(make_volume_common_name(second), "scene.qt-panel-example.2.devpc.qaros-ros");
}

TEST(CommonName, EmptyPartsNeverLeaveTwoDotsSideBySide)
{
  const NameScope scope = make_name_scope("", "", "");
  const std::string name = make_panel_common_name("", scope);
  EXPECT_EQ(name.find(".."), std::string::npos);
  EXPECT_EQ(make_volume_common_name(scope).find(".."), std::string::npos);
  EXPECT_TRUE(is_ascii_name(name));
}
