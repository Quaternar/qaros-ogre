// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT
//
// The `user_state` slots every QarOS gesture subscription of the adapter hands to the
// runtime, and the process-lifetime arena that owns them. The panel input source
// (component `panels`) and the spatial gesture source (component `gestures`) share ONE
// arena, so it lives in the base component `capi`.

#ifndef QAR_QAR_ADAPTER__QAR_CAPI_INPUT_SLOTS_HPP_
#define QAR_QAR_ADAPTER__QAR_CAPI_INPUT_SLOTS_HPP_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

namespace qar_qar_adapter
{

namespace detail
{

/// @brief The piece of state a QarOS callback is allowed to see.
///
/// ## Why this exists, and why it is never freed
///
/// `user_state` is a raw pointer the library hands back on a background thread.
/// The C API documents how to STOP a subscription (cancel its QarCancelToken)
/// but nowhere promises that a callback already running has returned by the
/// time the cancel call does. So the memory `user_state` points at must not be
/// freed while the runtime could still be inside it — the classic crash on this
/// path, and it would land during RViz shutdown, which is the worst possible
/// place to debug.
///
/// The rules here are therefore:
///
///  * every slot is owned by a process-lifetime graveyard and NEVER freed, so
///    the pointer the runtime holds always addresses live memory;
///  * `owner` is cleared under `mutex` when the subscription is retired, and
///    the callback takes the same mutex before touching the owner. Once
///    `retire()` returns, no callback is inside the owner and none ever will
///    be again — it is that, not the cancel token, that makes teardown safe;
///  * a retired slot is never reused. A reused slot would need the callback to
///    prove which generation it belongs to, and it has nothing but the pointer
///    to prove it with. Slots are ~100 bytes and one is minted per panel
///    subscription, so the bound is trivial; @ref slot_arena_size makes it
///    observable, and a hard cap keeps a pathological churn honest.
struct PointerSlot
{
  std::mutex mutex;
  /// Null once retired. Guarded by `mutex`.
  void * owner = nullptr;
  /// What this subscription is about — copied at subscribe time so the callback
  /// reads no shared state but its own slot.
  std::string panel_key;
  std::uint32_t content_width = 0;
  std::uint32_t content_height = 0;
  /// For the app-volume gesture source: which pointer id its samples carry.
  /// The panel source derives one per (peer, hand) instead — see
  /// @ref panel_pointer_id_for — so this is its FALLBACK only.
  std::uint32_t pointer_id = 0;
};

/// @brief Slots minted so far this process. Diagnostics, and a test hook.
std::size_t slot_arena_size();

/// @brief A fresh slot owned by the process, or null at the ceiling. Thread-safe.
PointerSlot * mint_slot(void * owner);

/// @brief Disown a slot: once this returns, no callback is inside its owner. Thread-safe.
void retire_slot(PointerSlot * slot);

}  // namespace detail

}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__QAR_CAPI_INPUT_SLOTS_HPP_
