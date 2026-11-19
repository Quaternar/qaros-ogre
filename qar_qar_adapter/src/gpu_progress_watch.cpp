// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/gpu_progress_watch.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "module_pin.hpp"
#include "qar_qar_adapter/env_config.hpp"
#include "qar_qar_adapter/log_time.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace qar_qar_adapter
{

namespace
{

using Clock = std::chrono::steady_clock;

std::int64_t steady_ns(Clock::time_point moment)
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(moment.time_since_epoch()).count();
}

std::int64_t steady_now_ns() { return steady_ns(Clock::now()); }

#ifdef _WIN32

// WGL and the GL 3.2 sync functions, resolved at run time like the rest of the adapter: no
// link-time dependency on opengl32.lib. The sync functions come from wglGetProcAddress, which
// needs a current context, so they are resolved by the first insert() on the render thread.
struct WglFns
{
  HMODULE module = nullptr;
  HGLRC(WINAPI * create_context)(HDC) = nullptr;
  BOOL(WINAPI * delete_context)(HGLRC) = nullptr;
  BOOL(WINAPI * share_lists)(HGLRC, HGLRC) = nullptr;
  BOOL(WINAPI * make_current)(HDC, HGLRC) = nullptr;
  PROC(WINAPI * get_proc_address)(LPCSTR) = nullptr;
  void(WINAPI * flush)(void) = nullptr;
};

const WglFns & wgl_fns()
{
  static const WglFns fns = [] {
      WglFns f;
      f.module = GetModuleHandleA("opengl32.dll");
      if (f.module == nullptr) { f.module = LoadLibraryA("opengl32.dll"); }
      if (f.module == nullptr) { return f; }
      auto sym = [&f](const char * name) {
          return reinterpret_cast<void *>(::GetProcAddress(f.module, name));
        };
      f.create_context = reinterpret_cast<decltype(f.create_context)>(sym("wglCreateContext"));
      f.delete_context = reinterpret_cast<decltype(f.delete_context)>(sym("wglDeleteContext"));
      f.share_lists = reinterpret_cast<decltype(f.share_lists)>(sym("wglShareLists"));
      f.make_current = reinterpret_cast<decltype(f.make_current)>(sym("wglMakeCurrent"));
      f.get_proc_address =
        reinterpret_cast<decltype(f.get_proc_address)>(sym("wglGetProcAddress"));
      f.flush = reinterpret_cast<decltype(f.flush)>(sym("glFlush"));
      return f;
    }();
  return fns;
}

constexpr unsigned int kSyncGpuCommandsComplete = 0x9117;
constexpr unsigned int kAlreadySignaled = 0x911A;
constexpr unsigned int kTimeoutExpired = 0x911B;
constexpr unsigned int kConditionSatisfied = 0x911C;

using PfnFenceSync = void * (WINAPI *)(unsigned int, unsigned int);
using PfnClientWaitSync = unsigned int (WINAPI *)(void *, unsigned int, unsigned long long);
using PfnDeleteSync = void(WINAPI *)(void *);

struct SyncFns
{
  std::atomic<PfnFenceSync> fence_sync{nullptr};
  std::atomic<PfnClientWaitSync> client_wait{nullptr};
  std::atomic<PfnDeleteSync> delete_sync{nullptr};
};

SyncFns & sync_fns()
{
  static SyncFns fns;
  return fns;
}

void * resolve_proc(const char * name)
{
  const WglFns & w = wgl_fns();
  if (w.get_proc_address == nullptr) { return nullptr; }
  void * p = reinterpret_cast<void *>(w.get_proc_address(name));
  // wglGetProcAddress answers with these values, not only null, when a function is missing.
  if (p == reinterpret_cast<void *>(1) || p == reinterpret_cast<void *>(2) ||
    p == reinterpret_cast<void *>(3) || p == reinterpret_cast<void *>(-1))
  {
    return nullptr;
  }
  return p;
}

void * wgl_insert()
{
  SyncFns & s = sync_fns();
  if (s.fence_sync.load() == nullptr) {
    s.client_wait.store(reinterpret_cast<PfnClientWaitSync>(resolve_proc("glClientWaitSync")));
    s.delete_sync.store(reinterpret_cast<PfnDeleteSync>(resolve_proc("glDeleteSync")));
    s.fence_sync.store(reinterpret_cast<PfnFenceSync>(resolve_proc("glFenceSync")));
  }
  const PfnFenceSync fence = s.fence_sync.load();
  if (fence == nullptr || s.client_wait.load() == nullptr || s.delete_sync.load() == nullptr) {
    return nullptr;
  }
  void * const sync = fence(kSyncGpuCommandsComplete, 0u);
  const WglFns & w = wgl_fns();
  if (sync != nullptr && w.flush != nullptr) { w.flush(); }
  return sync;
}

int wgl_wait(void * fence, std::uint64_t timeout_ns)
{
  const PfnClientWaitSync wait = sync_fns().client_wait.load();
  if (wait == nullptr || fence == nullptr) { return -1; }
  // The commands were flushed where the fence was inserted, so no flush bit is needed here.
  const unsigned int r = wait(fence, 0u, timeout_ns);
  if (r == kAlreadySignaled || r == kConditionSatisfied) { return 1; }
  return r == kTimeoutExpired ? 0 : -1;
}

void wgl_remove(void * fence)
{
  const PfnDeleteSync del = sync_fns().delete_sync.load();
  if (del != nullptr && fence != nullptr) { del(fence); }
}

int wgl_bind(void * hdc, void * context)
{
  const WglFns & w = wgl_fns();
  if (w.make_current == nullptr) { return -1; }
  if (w.make_current(static_cast<HDC>(hdc), static_cast<HGLRC>(context)) != FALSE) { return 0; }
  const DWORD err = GetLastError();
  return err != 0 ? static_cast<int>(err) : -1;
}

void wgl_unbind()
{
  const WglFns & w = wgl_fns();
  if (w.make_current != nullptr) { w.make_current(nullptr, nullptr); }
}

void * wgl_create_context(void * hdc, void * share_with)
{
  const WglFns & w = wgl_fns();
  if (hdc == nullptr || share_with == nullptr || w.create_context == nullptr ||
    w.share_lists == nullptr)
  {
    return nullptr;
  }
  HGLRC created = w.create_context(static_cast<HDC>(hdc));
  if (created == nullptr) { return nullptr; }
  if (w.share_lists(static_cast<HGLRC>(share_with), created) == FALSE) {
    const DWORD err = GetLastError();
    if (w.delete_context != nullptr) { w.delete_context(created); }
    SetLastError(err);
    return nullptr;
  }
  return static_cast<void *>(created);
}

void wgl_delete_context(void * context)
{
  const WglFns & w = wgl_fns();
  if (context != nullptr && w.delete_context != nullptr) {
    w.delete_context(static_cast<HGLRC>(context));
  }
}

int wgl_last_error() { return static_cast<int>(GetLastError()); }

#endif  // _WIN32

void print_line(const std::string & text)
{
  std::fprintf(stderr, "%s\n", text.c_str());
  std::fflush(stderr);
}

}  // namespace

const GlFenceOps & default_gl_fence_ops()
{
  static const GlFenceOps ops = [] {
      GlFenceOps o;
#ifdef _WIN32
      o.insert = &wgl_insert;
      o.wait = &wgl_wait;
      o.remove = &wgl_remove;
      o.bind = &wgl_bind;
      o.unbind = &wgl_unbind;
      o.create_context = &wgl_create_context;
      o.delete_context = &wgl_delete_context;
      o.last_error = &wgl_last_error;
#endif
      return o;
    }();
  return ops;
}

// ===========================================================================
//  FenceContext
// ===========================================================================

FenceContext::~FenceContext()
{
  // A context that a worker bound and did not release may still be current on that worker.
  if (state_.load() == kFresh && context_ != nullptr && ops_.delete_context != nullptr) {
    ops_.delete_context(context_);
    context_ = nullptr;
  }
}

int FenceContext::bind()
{
  if (ops_.bind == nullptr || context_ == nullptr) { return -1; }
  // Marked before the call: a bind that never returns leaves the context unreleased for good.
  state_.store(kBound);
  const int err = ops_.bind(hdc_, context_);
  if (err != 0) {
    // Current on no thread: the destructor may delete it.
    state_.store(kFresh);
  }
  return err;
}

void FenceContext::release_and_delete()
{
  if (state_.load() != kBound) { return; }
  if (ops_.unbind != nullptr) { ops_.unbind(); }
  if (context_ != nullptr && ops_.delete_context != nullptr) {
    ops_.delete_context(context_);
    context_ = nullptr;
  }
  state_.store(kGone);
}

// ===========================================================================
//  Environment
// ===========================================================================

bool default_gpu_gate_enabled()
{
  static const bool enabled = [] {
      const std::string value = read_environment("QAR_GPU_GATE");
      return parse_bool_env(environment_is_set("QAR_GPU_GATE") ? value.c_str() : nullptr, true);
    }();
  return enabled;
}

double default_gpu_gate_test_seconds()
{
  static const double seconds = [] {
      if (!environment_is_set("QAR_GPU_GATE_TEST")) { return 0.0; }
      const std::string value = read_environment("QAR_GPU_GATE_TEST");
      char * end = nullptr;
      const double parsed = std::strtod(value.c_str(), &end);
      return (end != value.c_str() && parsed > 0.0 && parsed < 3600.0) ? parsed : 0.0;
    }();
  return seconds;
}

std::string gpu_gate_banner(bool enabled, const GpuGateTimings & timings)
{
  char text[256];
  if (enabled) {
    std::snprintf(
      text, sizeof(text),
      "[qar-gpu] GPU progress gate on (QAR_GPU_GATE=on): RViz frames and the 3D render batch "
      "pause while a frame stays unfinished for more than max(%lld ms, %d x the median frame "
      "time)",
      static_cast<long long>(timings.min_age.count()), timings.age_factor);
  } else {
    std::snprintf(
      text, sizeof(text),
      "[qar-gpu] GPU progress gate off (QAR_GPU_GATE=off): RViz frames and the 3D render batch "
      "are never paused");
  }
  return text;
}

// ===========================================================================
//  GpuProgressWatch
// ===========================================================================

namespace
{

constexpr int kSeriesRviz = 0;
constexpr int kSeriesBatch = 1;
constexpr int kSeriesCount = 2;

int series_of(GpuProgressWatch::Kind kind)
{
  return kind == GpuProgressWatch::Kind::kBatch ? kSeriesBatch : kSeriesRviz;
}

// The first five of a kind of line are printed, then every 100th.
bool print_due(std::uint64_t n) { return n <= 5 || (n % 100) == 0; }

struct Pending
{
  void * fence = nullptr;
  std::int64_t issued_ns = 0;
  int series = kSeriesRviz;
  std::uint64_t ticket = 0;
};

// The fence that proves the GPU progresses while a series is closed.
struct Probe
{
  void * fence = nullptr;
  std::uint64_t ticket = 0;
};

// The threads of stopped watches, joined by reap() once they have ended.
struct Parked
{
  std::thread thread;
  std::shared_ptr<std::atomic<bool>> exited;
};

std::mutex & parked_mutex()
{
  static std::mutex m;
  return m;
}

std::vector<Parked> & parked_threads()
{
  // Never destroyed: a thread that is still running at the end of the process must not meet a
  // destructor of a joinable std::thread.
  static std::vector<Parked> * const v = new std::vector<Parked>();
  return *v;
}

std::atomic<int> & live_threads()
{
  // Never destroyed, like the list: the guard below reads it at the end of the process.
  static std::atomic<int> * const n = new std::atomic<int>(0);
  return *n;
}

// A watch thread that has not returned when this module is torn down (RViz exits while the GPU is
// stalled) would return into unmapped code: the module stays loaded then, as for a frame worker.
struct ModulePinGuard
{
  ~ModulePinGuard()
  {
    if (live_threads().load() > 0) { pin_this_module(); }
  }
};

ModulePinGuard g_module_pin_guard;

}  // namespace

struct GpuProgressWatch::State
{
  GlFenceOps ops;
  GpuGateTimings timings;
  double test_close_seconds = 0.0;
  bool enabled = true;

  // Fixed by start().
  void * hdc = nullptr;
  void * context = nullptr;

  std::mutex m;
  std::condition_variable cv;
  std::deque<Pending> queue;      // in issue order; under m
  Probe probe;                    // under m
  std::uint64_t next_ticket = 1;  // under m
  std::atomic<bool> stop{false};
  /// 0 not started, 1 the thread is binding, 2 bound and watching, 3 the bind failed, a wait
  /// failed, or the watch stopped.
  std::atomic<int> phase{0};
  std::shared_ptr<std::atomic<bool>> exited = std::make_shared<std::atomic<bool>>(false);

  /// What the render thread reads of one series; written under `m`.
  struct Series
  {
    /// The issue time of the oldest unfinished frame; 0 when none is unfinished.
    std::atomic<std::int64_t> oldest_ns{0};
    /// The age limit; written by the watch thread.
    std::atomic<std::int64_t> limit_ns{0};
  };
  Series series[kSeriesCount];

  std::atomic<std::uint64_t> pending{0};
  std::atomic<std::uint64_t> issued{0};
  std::atomic<std::uint64_t> completed{0};
  std::atomic<std::uint64_t> closes{0};
  std::atomic<std::uint64_t> skipped_frames{0};
  std::atomic<std::uint64_t> skipped_batches{0};
  std::atomic<std::uint64_t> probes{0};
  std::atomic<std::uint64_t> dropped{0};
  std::atomic<bool> wait_failed{false};
  /// The time of the first 3D batch (QAR_GPU_GATE_TEST); 0 before it.
  std::atomic<std::int64_t> first_batch_ns{0};
  /// The time of the last probe fence the render thread inserted.
  std::atomic<std::int64_t> last_probe_ns{0};

  // The watch thread's alone.
  std::vector<std::int64_t> history[kSeriesCount];
  std::size_t history_next[kSeriesCount] = {0, 0};
  bool logged_closed = false;
  bool close_printed = false;
  std::int64_t closed_since_ns = 0;
  std::uint64_t frames_skipped_at_close = 0;
  std::uint64_t batches_skipped_at_close = 0;
  bool test_logged = false;
  std::uint64_t drop_events = 0;

  std::int64_t min_age_ns() const
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(timings.min_age).count();
  }

  bool test_window(std::int64_t now_ns) const
  {
    const std::int64_t first = first_batch_ns.load();
    if (test_close_seconds <= 0.0 || first == 0) { return false; }
    const std::int64_t begin =
      first + std::chrono::duration_cast<std::chrono::nanoseconds>(timings.test_start_after)
      .count();
    const std::int64_t end = begin + static_cast<std::int64_t>(test_close_seconds * 1e9);
    return now_ns >= begin && now_ns < end;
  }

  /// True when the oldest unfinished frame of the series is older than the age limit.
  bool series_closed(int which, std::int64_t now_ns) const
  {
    const std::int64_t oldest = series[which].oldest_ns.load();
    return oldest != 0 && now_ns - oldest > series[which].limit_ns.load();
  }

  /// The gate of one series (the forced window closes both).
  bool closed(int which, std::int64_t now_ns) const
  {
    if (phase.load() != 2) { return false; }
    return series_closed(which, now_ns) || test_window(now_ns);
  }

  bool any_closed(std::int64_t now_ns) const
  {
    if (phase.load() != 2) { return false; }
    return series_closed(kSeriesRviz, now_ns) || series_closed(kSeriesBatch, now_ns) ||
           test_window(now_ns);
  }

  // Under m: the oldest unfinished frame of each series, from the queue.
  void publish_series_locked()
  {
    std::int64_t oldest[kSeriesCount] = {0, 0};
    for (const Pending & p : queue) {
      if (oldest[p.series] == 0) { oldest[p.series] = p.issued_ns; }
    }
    for (int i = 0; i < kSeriesCount; ++i) { series[i].oldest_ns.store(oldest[i]); }
    pending.store(queue.size());
  }

  // The watch thread: a frame of the series completed after @p latency_ns; the age limit follows
  // the median of the last completed frame times.
  void record_latency(int which, std::int64_t latency_ns)
  {
    std::vector<std::int64_t> & h = history[which];
    const std::size_t window =
      static_cast<std::size_t>(timings.median_window > 1 ? timings.median_window : 1);
    if (h.size() < window) {
      h.push_back(latency_ns);
    } else {
      h[history_next[which] % window] = latency_ns;
      history_next[which] = (history_next[which] + 1) % window;
    }
    std::vector<std::int64_t> sorted = h;
    const std::size_t mid = sorted.size() / 2;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(mid),
      sorted.end());
    const std::int64_t factor = timings.age_factor > 1 ? timings.age_factor : 1;
    series[which].limit_ns.store(std::max(min_age_ns(), factor * sorted[mid]));
  }

  // The render thread, with the host's context current: at most one probe fence per probe
  // interval while a series is closed, and only when the last probe is done.
  void insert_probe_if_due(std::int64_t now_ns)
  {
    const std::int64_t interval =
      std::chrono::duration_cast<std::chrono::nanoseconds>(timings.probe_interval).count();
    if (now_ns - last_probe_ns.load() < interval) { return; }
    {
      // Under the lock that the watch thread publishes the series with: a probe that just ended
      // and reopened the gate is not followed by another one.
      std::lock_guard<std::mutex> lock(m);
      if (probe.fence != nullptr || !series_closed(kSeriesRviz, now_ns)) { return; }
    }
    last_probe_ns.store(now_ns);
    void * const fence = ops.insert();
    if (fence == nullptr) { return; }
    {
      std::lock_guard<std::mutex> lock(m);
      probe.fence = fence;
      probe.ticket = next_ticket++;
    }
    probes.fetch_add(1);
    cv.notify_one();
  }

  // The watch thread: prints the line when the gate closed or opened since the last look.
  void monitor()
  {
    const std::int64_t now = steady_now_ns();
    const bool forced = test_window(now);
    const bool rviz = series_closed(kSeriesRviz, now);
    const bool batch = series_closed(kSeriesBatch, now);
    const bool is_closed = forced || rviz || batch;
    if (is_closed && !logged_closed) {
      logged_closed = true;
      std::int64_t since = now;
      for (int i = 0; i < kSeriesCount; ++i) {
        const std::int64_t oldest = series[i].oldest_ns.load();
        if (series_closed(i, now) && oldest != 0 && oldest < since) { since = oldest; }
      }
      closed_since_ns = since;
      frames_skipped_at_close = skipped_frames.load();
      batches_skipped_at_close = skipped_batches.load();
      const std::uint64_t n = closes.fetch_add(1) + 1;
      close_printed = print_due(n);
      if (forced && !test_logged) {
        test_logged = true;
        char text[200];
        std::snprintf(
          text, sizeof(text),
          "[qar-gpu] %s QAR_GPU_GATE_TEST=%g: the gate is closed on purpose for %g s",
          wall_clock_hms_ms().c_str(), test_close_seconds, test_close_seconds);
        print_line(text);
      }
      if (close_printed) {
        char text[256];
        std::snprintf(
          text, sizeof(text),
          "[qar-gpu] %s the GPU has not finished a frame for %lld ms: RViz frames and the 3D "
          "render batch pause",
          wall_clock_hms_ms().c_str(), static_cast<long long>((now - closed_since_ns) / 1000000));
        std::string line = text;
        if (n > 5) { line += " (close " + std::to_string(n) + ")"; }
        print_line(line);
      }
    } else if (!is_closed && logged_closed) {
      logged_closed = false;
      if (close_printed) {
        char text[256];
        std::snprintf(
          text, sizeof(text),
          "[qar-gpu] %s the GPU finishes frames again after %.1f s; %llu RViz frame(s) and %llu "
          "render batch(es) were skipped",
          wall_clock_hms_ms().c_str(), static_cast<double>(now - closed_since_ns) / 1e9,
          static_cast<unsigned long long>(skipped_frames.load() - frames_skipped_at_close),
          static_cast<unsigned long long>(skipped_batches.load() - batches_skipped_at_close));
        print_line(text);
      }
    }
  }

  // The watch thread: the oldest fence is done.
  void complete_front(void * fence)
  {
    int which = kSeriesRviz;
    std::int64_t issued_ns = 0;
    {
      std::lock_guard<std::mutex> lock(m);
      if (queue.empty() || queue.front().fence != fence) { return; }
      which = queue.front().series;
      issued_ns = queue.front().issued_ns;
      queue.pop_front();
      publish_series_locked();
    }
    record_latency(which, steady_now_ns() - issued_ns);
    if (ops.remove != nullptr) { ops.remove(fence); }
    completed.fetch_add(1);
  }

  // The watch thread: a probe that completed proves progress, so fences issued before it that
  // are still pending are stale; a probe that failed is only forgotten.
  void check_probe()
  {
    void * probe_fence = nullptr;
    {
      std::lock_guard<std::mutex> lock(m);
      probe_fence = probe.fence;
    }
    if (probe_fence == nullptr) { return; }
    const int r = ops.wait(probe_fence, 0);
    if (r == 0) { return; }
    std::vector<void *> stale;
    {
      std::lock_guard<std::mutex> lock(m);
      probe.fence = nullptr;
      if (r > 0) {
        while (!queue.empty() && queue.front().ticket < probe.ticket) {
          stale.push_back(queue.front().fence);
          queue.pop_front();
        }
        publish_series_locked();
      }
    }
    if (ops.remove != nullptr) {
      ops.remove(probe_fence);
      for (void * fence : stale) { ops.remove(fence); }
    }
    if (stale.empty()) { return; }
    dropped.fetch_add(stale.size());
    if (print_due(++drop_events)) {
      char text[256];
      std::snprintf(
        text, sizeof(text),
        "[qar-gpu] %s a probe fence finished while %llu older fence(s) stayed unfinished; they "
        "were dropped",
        wall_clock_hms_ms().c_str(), static_cast<unsigned long long>(stale.size()));
      print_line(text);
    }
  }

  void run()
  {
    const int err = ops.bind(hdc, context);
    if (err != 0) {
      phase.store(3);
      char text[200];
      std::snprintf(
        text, sizeof(text),
        "[qar-gpu] %s the GPU watch could not bind its GL context (err=%d); the gate is off",
        wall_clock_hms_ms().c_str(), err);
      print_line(text);
      ops.delete_context(context);
      context = nullptr;
      return;
    }
    phase.store(2);
    const std::uint64_t slice_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(timings.slice).count());
    while (!stop.load()) {
      void * front = nullptr;
      {
        std::unique_lock<std::mutex> lock(m);
        cv.wait_for(lock, timings.slice, [this] { return stop.load() || !queue.empty(); });
        if (stop.load()) { break; }
        if (!queue.empty()) { front = queue.front().fence; }
      }
      if (front != nullptr) {
        const int r = ops.wait(front, slice_ns);
        if (r > 0) {
          complete_front(front);
        } else if (r < 0) {
          // A failed wait cannot be repeated usefully, and the frames can no longer be followed:
          // the gate opens for good.
          wait_failed.store(true);
          phase.store(3);
          char text[200];
          std::snprintf(
            text, sizeof(text),
            "[qar-gpu] %s the GPU watch could not wait for a fence; the gate is off",
            wall_clock_hms_ms().c_str());
          print_line(text);
          break;
        }
      }
      check_probe();
      monitor();
    }
    // The fences that were never waited for are deleted with the context.
    std::deque<Pending> rest;
    void * probe_fence = nullptr;
    {
      std::lock_guard<std::mutex> lock(m);
      rest.swap(queue);
      probe_fence = probe.fence;
      probe.fence = nullptr;
      publish_series_locked();
    }
    phase.store(3);
    if (ops.remove != nullptr) {
      for (const Pending & p : rest) { ops.remove(p.fence); }
      if (probe_fence != nullptr) { ops.remove(probe_fence); }
    }
    ops.unbind();
    ops.delete_context(context);
    context = nullptr;
  }
};

GpuProgressWatch::GpuProgressWatch(
  const GlFenceOps & ops, bool enabled, const GpuGateTimings & timings,
  double test_close_seconds)
: state_(std::make_shared<State>())
{
  state_->ops = ops;
  state_->enabled = enabled;
  state_->timings = timings;
  state_->test_close_seconds = test_close_seconds;
  for (int i = 0; i < kSeriesCount; ++i) {
    state_->series[i].limit_ns.store(state_->min_age_ns());
  }
}

GpuProgressWatch::~GpuProgressWatch()
{
  stop();
}

bool GpuProgressWatch::start(void * hdc, void * share_with)
{
  State & s = *state_;
  if (started_ || !s.enabled || hdc == nullptr || share_with == nullptr || s.stop.load()) {
    return false;
  }
  if (!s.ops.complete()) { return false; }
  started_ = true;
  void * const context = s.ops.create_context(hdc, share_with);
  if (context == nullptr) {
    s.phase.store(3);
    char text[200];
    std::snprintf(
      text, sizeof(text),
      "[qar-gpu] %s the GPU watch could not create its GL context (err=%d); the gate is off",
      wall_clock_hms_ms().c_str(), s.ops.last_error());
    print_line(text);
    return false;
  }
  s.hdc = hdc;
  s.context = context;
  s.phase.store(1);
  const std::shared_ptr<State> state = state_;
  live_threads().fetch_add(1);
  try {
    std::thread thread([state] {
        state->run();
        state->exited->store(true);
        live_threads().fetch_sub(1);
      });
    std::lock_guard<std::mutex> lock(parked_mutex());
    parked_threads().push_back(Parked{std::move(thread), s.exited});
  } catch (...) {
    live_threads().fetch_sub(1);
    s.phase.store(3);
    s.ops.delete_context(context);
    s.context = nullptr;
    return false;
  }
  return true;
}

void GpuProgressWatch::stop()
{
  State & s = *state_;
  s.stop.store(true);
  s.cv.notify_all();
  // The thread stays in the process-wide list; reap() joins it once it has ended.
}

bool GpuProgressWatch::active() const
{
  return state_->phase.load() == 2 && !state_->stop.load();
}

void GpuProgressWatch::note_frame_issued(Kind kind)
{
  State & s = *state_;
  if (!active()) { return; }
  const std::int64_t now = steady_now_ns();
  const int which = series_of(kind);
  if (kind == Kind::kBatch) {
    std::int64_t expected = 0;
    s.first_batch_ns.compare_exchange_strong(expected, now);
  }
  if (s.closed(which, now)) { return; }
  void * const fence = s.ops.insert();
  if (fence == nullptr) { return; }
  {
    std::lock_guard<std::mutex> lock(s.m);
    s.queue.push_back(Pending{fence, now, which, s.next_ticket++});
    s.pending.store(s.queue.size());
    if (s.series[which].oldest_ns.load() == 0) { s.series[which].oldest_ns.store(now); }
  }
  s.issued.fetch_add(1);
  s.cv.notify_one();
}

bool GpuProgressWatch::gate_closed()
{
  State & s = *state_;
  if (s.stop.load()) { return false; }
  return s.closed(kSeriesBatch, steady_now_ns());
}

bool GpuProgressWatch::rviz_gate_closed()
{
  State & s = *state_;
  if (s.stop.load()) { return false; }
  const std::int64_t now = steady_now_ns();
  if (!s.closed(kSeriesRviz, now)) { return false; }
  // Only a natural close gets a probe; the forced window of QAR_GPU_GATE_TEST ends by itself.
  if (s.series_closed(kSeriesRviz, now)) { s.insert_probe_if_due(now); }
  return true;
}

bool GpuProgressWatch::any_gate_closed()
{
  State & s = *state_;
  if (s.stop.load()) { return false; }
  return s.any_closed(steady_now_ns());
}

std::int64_t GpuProgressWatch::age_limit_ms(Kind kind) const
{
  return state_->series[series_of(kind)].limit_ns.load() / 1000000;
}

void GpuProgressWatch::note_batch_skipped()
{
  state_->skipped_batches.fetch_add(1);
}

void GpuProgressWatch::note_frame_skipped()
{
  state_->skipped_frames.fetch_add(1);
}

std::uint64_t GpuProgressWatch::frames_issued() const { return state_->issued.load(); }
std::uint64_t GpuProgressWatch::frames_completed() const { return state_->completed.load(); }
std::uint64_t GpuProgressWatch::frames_pending() const { return state_->pending.load(); }
std::uint64_t GpuProgressWatch::skipped_frames() const { return state_->skipped_frames.load(); }
std::uint64_t GpuProgressWatch::skipped_batches() const { return state_->skipped_batches.load(); }
std::uint64_t GpuProgressWatch::closes() const { return state_->closes.load(); }
std::uint64_t GpuProgressWatch::probes() const { return state_->probes.load(); }
std::uint64_t GpuProgressWatch::dropped_fences() const { return state_->dropped.load(); }
bool GpuProgressWatch::wait_failed() const { return state_->wait_failed.load(); }

void GpuProgressWatch::reap()
{
  std::vector<Parked> ended;
  {
    std::lock_guard<std::mutex> lock(parked_mutex());
    std::vector<Parked> & all = parked_threads();
    for (auto it = all.begin(); it != all.end();) {
      if (it->exited->load()) {
        ended.push_back(std::move(*it));
        it = all.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (Parked & p : ended) {
    if (p.thread.joinable()) { p.thread.join(); }
  }
}

int GpuProgressWatch::running_threads()
{
  return live_threads().load();
}

}  // namespace qar_qar_adapter
