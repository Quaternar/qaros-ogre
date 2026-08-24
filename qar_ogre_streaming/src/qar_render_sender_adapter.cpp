// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// IQarRenderSenderAdapter / IQarRenderSenderFactory are pure interfaces. This
// translation unit exists to (a) keep the header self-contained/compilable and
// (b) host any shared adapter helpers added later. It compiles the header on its
// own and statically asserts the abstract shape stays intact.

#include "qar_ogre_streaming/qar_render_sender_adapter.hpp"

#include <type_traits>

namespace qar_ogre_streaming
{

static_assert(
  std::is_abstract<IQarRenderSenderAdapter>::value,
  "IQarRenderSenderAdapter must remain an abstract interface");
static_assert(
  std::is_abstract<IQarRenderSenderFactory>::value,
  "IQarRenderSenderFactory must remain an abstract interface");

}  // namespace qar_ogre_streaming
