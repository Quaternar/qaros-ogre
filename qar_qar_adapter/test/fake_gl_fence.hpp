// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// A fake of the GL calls behind the fence waits and the GPU progress watch (GlFenceOps): fences
// are numbers, a fence is signalled when the test says so, and every call is recorded with the
// thread that made it. No GL context is made and no GL function is called.

#ifndef QAR_QAR_ADAPTER_TEST__FAKE_GL_FENCE_HPP_
#define QAR_QAR_ADAPTER_TEST__FAKE_GL_FENCE_HPP_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "qar_qar_adapter/gpu_progress_watch.hpp"

namespace qar_qar_adapter
{
namespace test
{

class FakeGl
{
public:
  // One fake per process: GlFenceOps holds plain function pointers.
  static FakeGl & get()
  {
    static FakeGl fake;
    return fake;
  }

  // Back to the start state; the caller makes sure no thread is inside a call.
  void reset()
  {
    std::lock_guard<std::mutex> lock(m_);
    next_ = 1;
    signalled_upto_ = 0;
    auto_signal_ = false;
    hard_block_ = false;
    bind_result_ = 0;
    create_result_ = reinterpret_cast<void *>(0xC0DE);
    inserts_ = waits_ = removes_ = binds_ = unbinds_ = creates_ = deletes_ = 0;
    wait_results_ = 0;
    latency_ = std::chrono::milliseconds(0);
    signalled_.clear();
    failing_.clear();
    inserted_at_.clear();
    insert_threads_.clear();
    wait_threads_.clear();
    bind_threads_.clear();
    removed_.clear();
    cv_.notify_all();
  }

  GlFenceOps ops() const
  {
    GlFenceOps o;
    o.insert = &FakeGl::do_insert;
    o.wait = &FakeGl::do_wait;
    o.remove = &FakeGl::do_remove;
    o.bind = &FakeGl::do_bind;
    o.unbind = &FakeGl::do_unbind;
    o.create_context = &FakeGl::do_create;
    o.delete_context = &FakeGl::do_delete;
    o.last_error = &FakeGl::do_last_error;
    return o;
  }

  // --- Controls --------------------------------------------------------------------------------

  // Signals every fence inserted so far.
  void signal_all()
  {
    std::lock_guard<std::mutex> lock(m_);
    signalled_upto_ = next_ - 1;
    cv_.notify_all();
  }
  // One fence (by its number, 1 for the first insert after reset) is signalled; the others are not.
  void signal_fence(std::uintptr_t id)
  {
    std::lock_guard<std::mutex> lock(m_);
    signalled_.insert(id);
    cv_.notify_all();
  }
  // A wait for this fence fails (GL_WAIT_FAILED) at once.
  void fail_fence(std::uintptr_t id)
  {
    std::lock_guard<std::mutex> lock(m_);
    failing_.insert(id);
    cv_.notify_all();
  }
  // Every fence is signalled this long after it was inserted (0: only the controls signal).
  void set_latency(std::chrono::milliseconds latency)
  {
    std::lock_guard<std::mutex> lock(m_);
    latency_ = latency;
    cv_.notify_all();
  }
  // Every fence, also later ones, is signalled at once.
  void set_auto_signal(bool on)
  {
    std::lock_guard<std::mutex> lock(m_);
    auto_signal_ = on;
    cv_.notify_all();
  }
  // A wait ignores its timeout and returns only after unblock(): a driver call that does not
  // return.
  void block_hard()
  {
    std::lock_guard<std::mutex> lock(m_);
    hard_block_ = true;
  }
  void unblock()
  {
    std::lock_guard<std::mutex> lock(m_);
    hard_block_ = false;
    cv_.notify_all();
  }
  void set_bind_result(int result)
  {
    std::lock_guard<std::mutex> lock(m_);
    bind_result_ = result;
  }
  void set_create_result(void * context)
  {
    std::lock_guard<std::mutex> lock(m_);
    create_result_ = context;
  }

  // --- Observations ----------------------------------------------------------------------------

  int inserts() const { return count(inserts_); }
  int waits() const { return count(waits_); }
  int removes() const { return count(removes_); }
  int binds() const { return count(binds_); }
  int unbinds() const { return count(unbinds_); }
  int creates() const { return count(creates_); }
  int deletes() const { return count(deletes_); }

  std::set<std::thread::id> insert_threads() const { return copy(insert_threads_); }
  std::set<std::thread::id> wait_threads() const { return copy(wait_threads_); }
  std::set<std::thread::id> bind_threads() const { return copy(bind_threads_); }
  bool was_removed(std::uintptr_t fence) const
  {
    std::lock_guard<std::mutex> lock(m_);
    return removed_.count(fence) != 0;
  }

private:
  int count(const int & value) const
  {
    std::lock_guard<std::mutex> lock(m_);
    return value;
  }
  std::set<std::thread::id> copy(const std::set<std::thread::id> & value) const
  {
    std::lock_guard<std::mutex> lock(m_);
    return value;
  }

  using Clock = std::chrono::steady_clock;

  static void * do_insert()
  {
    FakeGl & g = get();
    std::lock_guard<std::mutex> lock(g.m_);
    ++g.inserts_;
    g.insert_threads_.insert(std::this_thread::get_id());
    const std::uintptr_t id = static_cast<std::uintptr_t>(g.next_++);
    g.inserted_at_[id] = Clock::now();
    return reinterpret_cast<void *>(id);
  }

  // The time a fence is signalled by the latency, if there is one. Under m_.
  bool ready_at(std::uintptr_t id, Clock::time_point * when) const
  {
    const auto it = inserted_at_.find(id);
    if (latency_ <= std::chrono::milliseconds(0) || it == inserted_at_.end()) { return false; }
    *when = it->second + latency_;
    return true;
  }

  // Under m_.
  bool is_done(std::uintptr_t id) const
  {
    if (auto_signal_ || id <= signalled_upto_ || signalled_.count(id) != 0) { return true; }
    Clock::time_point when;
    return ready_at(id, &when) && Clock::now() >= when;
  }

  static int do_wait(void * fence, std::uint64_t timeout_ns)
  {
    FakeGl & g = get();
    const std::uintptr_t id = reinterpret_cast<std::uintptr_t>(fence);
    std::unique_lock<std::mutex> lock(g.m_);
    ++g.waits_;
    g.wait_threads_.insert(std::this_thread::get_id());
    if (g.hard_block_) {
      g.cv_.wait(lock, [&g] { return !g.hard_block_; });
    }
    const Clock::time_point deadline = Clock::now() + std::chrono::nanoseconds(timeout_ns);
    for (;;) {
      if (g.failing_.count(id) != 0) { return -1; }
      if (g.is_done(id)) { return 1; }
      if (Clock::now() >= deadline) { return 0; }
      Clock::time_point until = deadline;
      Clock::time_point when;
      if (g.ready_at(id, &when) && when < until) { until = when; }
      g.cv_.wait_until(lock, until);
    }
  }

  static void do_remove(void * fence)
  {
    FakeGl & g = get();
    std::lock_guard<std::mutex> lock(g.m_);
    ++g.removes_;
    g.removed_.insert(reinterpret_cast<std::uintptr_t>(fence));
  }

  static int do_bind(void *, void *)
  {
    FakeGl & g = get();
    std::lock_guard<std::mutex> lock(g.m_);
    ++g.binds_;
    g.bind_threads_.insert(std::this_thread::get_id());
    return g.bind_result_;
  }

  static void do_unbind()
  {
    FakeGl & g = get();
    std::lock_guard<std::mutex> lock(g.m_);
    ++g.unbinds_;
  }

  static void * do_create(void *, void *)
  {
    FakeGl & g = get();
    std::lock_guard<std::mutex> lock(g.m_);
    ++g.creates_;
    return g.create_result_;
  }

  static void do_delete(void *)
  {
    FakeGl & g = get();
    std::lock_guard<std::mutex> lock(g.m_);
    ++g.deletes_;
  }

  static int do_last_error() { return 87; }

  mutable std::mutex m_;
  std::condition_variable cv_;
  std::uint64_t next_ = 1;
  std::uint64_t signalled_upto_ = 0;
  bool auto_signal_ = false;
  bool hard_block_ = false;
  int bind_result_ = 0;
  void * create_result_ = reinterpret_cast<void *>(0xC0DE);
  int inserts_ = 0;
  int waits_ = 0;
  int removes_ = 0;
  int binds_ = 0;
  int unbinds_ = 0;
  int creates_ = 0;
  int deletes_ = 0;
  int wait_results_ = 0;
  std::set<std::thread::id> insert_threads_;
  std::set<std::thread::id> wait_threads_;
  std::set<std::thread::id> bind_threads_;
  std::set<std::uintptr_t> removed_;
  std::set<std::uintptr_t> signalled_;
  std::set<std::uintptr_t> failing_;
  std::map<std::uintptr_t, Clock::time_point> inserted_at_;
  std::chrono::milliseconds latency_{0};
};

}  // namespace test
}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER_TEST__FAKE_GL_FENCE_HPP_
