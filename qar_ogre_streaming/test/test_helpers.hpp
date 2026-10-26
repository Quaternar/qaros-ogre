// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// Small shared helpers for qar_ogre_streaming unit tests.

#ifndef QAR_OGRE_STREAMING_TEST__TEST_HELPERS_HPP_
#define QAR_OGRE_STREAMING_TEST__TEST_HELPERS_HPP_

#include "qar_ogre_streaming/types.hpp"

namespace qar_ogre_streaming
{
namespace test
{

/// @brief Deterministic non-zero peer id (all bytes = @p seed).
inline QarPeerId make_peer_id(uint8_t seed)
{
  QarPeerId id{};
  for (auto & byte : id.data) {
    byte = seed;
  }
  return id;
}

/// @brief A non-null but never-dereferenced Ogre::SceneManager pointer.
///
/// The paths the unit tests exercise store the scene manager pointer for
/// bookkeeping and never call into it, so a fake non-null address is safe and lets
/// validation and test-double paths be exercised with no Ogre initialization.
inline Ogre::SceneManager * fake_scene_manager()
{
  return reinterpret_cast<Ogre::SceneManager *>(0x1);
}

}  // namespace test
}  // namespace qar_ogre_streaming

#endif  // QAR_OGRE_STREAMING_TEST__TEST_HELPERS_HPP_
