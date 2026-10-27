// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "session_owner_harness.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

namespace
{

constexpr std::size_t kHeader = 16;
constexpr std::uint64_t kMagic = 0x51A7C0DE0A110C8Eull;

struct Header
{
  std::uint64_t magic;
  std::uint64_t size;
};

std::atomic<std::uintptr_t> g_watched{0};
std::atomic<bool> g_freed{false};

void * allocate(std::size_t size)
{
  void * raw = std::malloc(size + kHeader);
  if (raw == nullptr) {
    return nullptr;
  }
  auto * header = static_cast<Header *>(raw);
  header->magic = kMagic;
  header->size = size;
  return static_cast<char *>(raw) + kHeader;
}

void release(void * pointer)
{
  if (pointer == nullptr) {
    return;
  }
  auto * raw = static_cast<char *>(pointer) - kHeader;
  auto * header = reinterpret_cast<Header *>(raw);
  if (header->magic != kMagic) {
    // Not ours: a block that code outside this executable allocated with malloc.
    std::free(pointer);
    return;
  }
  const std::uintptr_t watched = g_watched.load();
  const auto begin = reinterpret_cast<std::uintptr_t>(pointer);
  if (watched != 0 && watched >= begin && watched < begin + header->size) {
    g_freed.store(true);
  }
  header->magic = 0;
  std::free(raw);
}

}  // namespace

void * operator new(std::size_t size)
{
  void * pointer = allocate(size);
  if (pointer == nullptr) {
    throw std::bad_alloc();
  }
  return pointer;
}

void * operator new[](std::size_t size) {return operator new(size);}

void * operator new(std::size_t size, const std::nothrow_t &) noexcept {return allocate(size);}

void * operator new[](std::size_t size, const std::nothrow_t &) noexcept {return allocate(size);}

void operator delete(void * pointer) noexcept {release(pointer);}

void operator delete[](void * pointer) noexcept {release(pointer);}

void operator delete(void * pointer, std::size_t) noexcept {release(pointer);}

void operator delete[](void * pointer, std::size_t) noexcept {release(pointer);}

void operator delete(void * pointer, const std::nothrow_t &) noexcept {release(pointer);}

void operator delete[](void * pointer, const std::nothrow_t &) noexcept {release(pointer);}

namespace qar_qar_adapter
{
namespace test
{
namespace alloc_watch
{

void arm(const void * address)
{
  g_freed.store(false);
  g_watched.store(reinterpret_cast<std::uintptr_t>(address));
}

bool freed() {return g_freed.load();}

bool armed() {return g_watched.load() != 0;}

}  // namespace alloc_watch
}  // namespace test
}  // namespace qar_qar_adapter
