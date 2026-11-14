// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The version of the build (qar_ar_core/version.hpp, cmake/QarOgreVersion.cmake): X.Y.Z from the
// file VERSION, with the part that git or the release build adds.

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

  EXPECT_TRUE(std::regex_match(core, std::regex(R"([0-9]+\.[0-9]+\.[0-9]+)")));
  EXPECT_EQ(version.rfind(core, 0), 0u) << version;
  // X.Y.Z, X.Y.Z-rc.N, X.Y.Z-dev[.N[+sha]] or a release build's own suffix.
  EXPECT_TRUE(std::regex_match(version, std::regex(R"([0-9]+\.[0-9]+\.[0-9]+([-+][0-9A-Za-z.+-]+)?)")))
    << version;
}

}  // namespace
