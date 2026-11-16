// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The version of the build (qar_ar_core/version.hpp, cmake/QarOgreVersion.cmake): X.Y.Z from the
// file VERSION (the last release) for a release build, else the next patch with the part that
// git or the build adds.

#include <gtest/gtest.h>

#include <fstream>
#include <iostream>
#include <regex>
#include <string>

#include "qar_ar_core/version.hpp"

namespace
{

TEST(LibraryVersion, IsTheVersionFileWithTheDerivedPart)
{
  const std::string version = qar_ar_core::kLibraryVersion;
  const std::string core = qar_ar_core::kLibraryVersionCore;
  std::cout << "qar_ogre_adapter " << version << " (VERSION " << core << ")\n";

  std::ifstream file(QAR_TEST_VERSION_FILE);
  std::string from_file;
  ASSERT_TRUE(std::getline(file, from_file)) << QAR_TEST_VERSION_FILE;
  from_file = std::regex_replace(from_file, std::regex(R"(^\s+|\s+$)"), "");
  EXPECT_EQ(core, from_file);

  std::smatch core_parts;
  ASSERT_TRUE(std::regex_match(core, core_parts, std::regex(R"(([0-9]+)\.([0-9]+)\.([0-9]+))"))) << core;
  const std::string next_patch = core_parts[1].str() + "." + core_parts[2].str() + "." +
                                 std::to_string(std::stoi(core_parts[3].str()) + 1);

  // A release is X.Y.Z of VERSION; any other build is X.Y.(Z+1)-dev[.N][+sha] or X.Y.(Z+1)-pr.N.
  std::smatch parts;
  ASSERT_TRUE(std::regex_match(
    version, parts, std::regex(R"(([0-9]+\.[0-9]+\.[0-9]+)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?)")))
    << version;
  if (parts[2].matched) {
    EXPECT_EQ(parts[1].str(), next_patch) << version;
    EXPECT_TRUE(std::regex_match(parts[2].str(), std::regex(R"(-(dev|pr)(\..*)?)"))) << version;
  } else {
    EXPECT_EQ(parts[1].str(), core) << version;
  }
}

}  // namespace
