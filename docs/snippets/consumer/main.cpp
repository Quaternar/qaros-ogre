// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Consumer program of the installed qar_ogre_adapter package. It prints the version of the package
// and the targets it was linked with, and calls three functions of three components that touch
// neither Ogre nor the QarOS SDK.

#include <cstdio>

#include "qar_ar_core/qar_streaming_c.hpp"
#include "qar_ogre_streaming/types.hpp"
#include "qar_qar_adapter/env_config.hpp"

int main()
{
  std::printf("qar_ogre_adapter %s\n", CONSUMER_PACKAGE_VERSION);
  std::printf("targets: %s\n", CONSUMER_TARGETS);
  std::printf("QarOS library loaded: %s\n", qar_is_library_loaded() ? "yes" : "no");
  std::printf("status text: %s\n", qar_ogre_streaming::to_string(qar_ogre_streaming::Status::Ok));
  std::printf("parse_bool_env(\"on\"): %s\n", qar_qar_adapter::parse_bool_env("on", false) ? "true" : "false");
  return 0;
}
