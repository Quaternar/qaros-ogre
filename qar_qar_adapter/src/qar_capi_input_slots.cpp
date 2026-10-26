// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/qar_capi_input_slots.hpp"

#include <cstddef>
#include <deque>
#include <mutex>

namespace qar_qar_adapter
{

namespace detail
{
namespace
{

std::mutex &
arena_mutex()
{
  static std::mutex m;
  return m;
}

/// Deliberately leaked: see PointerSlot's docs. A deque so existing elements
/// never move — the runtime holds raw pointers into this.
std::deque<PointerSlot> &
arena()
{
  static auto * slots = new std::deque<PointerSlot>();
  return *slots;
}

/// A generous ceiling on a number that should stay in single digits. Reached
/// only by something pathological (a panel re-subscribing every frame), and
/// failing loudly beats leaking without bound.
constexpr std::size_t kMaxSlots = 4096;

}  // namespace

std::size_t
slot_arena_size()
{
  const std::lock_guard<std::mutex> lock(arena_mutex());
  return arena().size();
}

/// @return a fresh slot owned by the process, or null at the ceiling.
PointerSlot *
mint_slot(void * owner)
{
  const std::lock_guard<std::mutex> lock(arena_mutex());
  if (arena().size() >= kMaxSlots) {
    return nullptr;
  }
  arena().emplace_back();
  PointerSlot & slot = arena().back();
  slot.owner = owner;
  return &slot;
}

/// Disown a slot. Once this returns, no callback is inside the owner: either it
/// has not taken the mutex yet (and will find a null owner), or it held the
/// mutex and we waited for it here.
void
retire_slot(PointerSlot * slot)
{
  if (slot == nullptr) {
    return;
  }
  const std::lock_guard<std::mutex> lock(slot->mutex);
  slot->owner = nullptr;
}

}  // namespace detail

}  // namespace qar_qar_adapter
