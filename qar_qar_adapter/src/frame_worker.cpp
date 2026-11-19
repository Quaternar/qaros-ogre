// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT

#include "qar_qar_adapter/frame_worker.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "module_pin.hpp"
#include "qar_qar_adapter/log_time.hpp"

namespace qar_qar_adapter
{
namespace
{

using Clock = std::chrono::steady_clock;

constexpr std::uint64_t kPrintFirst = 5;
constexpr std::uint64_t kPrintEvery = 100;

std::atomic<int> g_running{0};

std::int64_t now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    Clock::now().time_since_epoch()).count();
}

std::string seconds_text(double seconds)
{
  char text[32] = {0};
  std::snprintf(text, sizeof(text) - 1, "%.1f", seconds);
  return text;
}

}  // namespace

struct FrameWorker::State
{
  std::string name;
  std::shared_ptr<IReleaseLane> lane;
  Jobs jobs;
  FrameWorkerTimings timings;
  FrameWorkerLog log;
  std::shared_ptr<IReleaseLane::WorkerSlot> slot = std::make_shared<IReleaseLane::WorkerSlot>();

  std::mutex m;
  std::condition_variable cv;
  bool kicked = false;
  bool handed = false;
  bool stop = false;

  std::atomic<std::int64_t> last_kick_ns{0};
  std::atomic<bool> exited{false};
  std::atomic<bool> notified{false};
  std::atomic<bool> left_line{false};
  std::atomic<unsigned long long> polls{0};
  std::atomic<unsigned long long> shows{0};
  // Worker thread only.
  std::map<std::string, std::uint64_t> slow_counts;

  void print(const std::string & text)
  {
    const std::string line = log.prefix + " " + wall_clock_hms_ms() + " " + name + " " + text;
    if (log.sink) {
      log.sink(line);
    } else {
      std::fprintf(stderr, "%s\n", line.c_str());
      std::fflush(stderr);
    }
  }

  // Worker thread: a call that took slow_call or longer.
  void report_slow(const char * call, double took_ms)
  {
    if (slot->abandoned.load()) {
      return;
    }
    const std::uint64_t count = ++slow_counts[call];
    if (count > kPrintFirst && (count % kPrintEvery) != 0) {
      return;
    }
    char ms[32] = {0};
    std::snprintf(ms, sizeof(ms) - 1, "%.0f", took_ms);
    print(std::string("frame call ") + call + " took " + ms + " ms on the frame worker");
  }

  // Any thread: the call the worker is inside, when it has run for wait_notice or longer.
  bool stuck(std::string * call, double * seconds) const
  {
    const char * const current = slot->clock.call.load();
    const std::int64_t since = slot->clock.since_ns.load();
    if (current == nullptr || since <= 0) {
      return false;
    }
    const double age_ms = static_cast<double>(now_ns() - since) / 1.0e6;
    if (age_ms < static_cast<double>(timings.wait_notice.count())) {
      return false;
    }
    if (call != nullptr) {
      *call = current;
    }
    if (seconds != nullptr) {
      *seconds = age_ms / 1000.0;
    }
    return true;
  }

  bool announce_wait(std::string * call, double * seconds)
  {
    std::string name_of_call;
    double age = 0.0;
    if (!stuck(&name_of_call, &age)) {
      return false;
    }
    if (!notified.exchange(true)) {
      print("frame worker waiting in " + name_of_call + " for " + seconds_text(age) + " s");
    }
    if (call != nullptr) {
      *call = name_of_call;
    }
    if (seconds != nullptr) {
      *seconds = age;
    }
    return true;
  }
};

namespace
{

// The threads of stopped workers. The GUI thread only appends; a thread is joined once it has
// ended.
struct Parked
{
  std::shared_ptr<void> keep;           // the worker's shared state, kept alive for its thread
  const std::atomic<bool> * exited;     // lives in that state
  std::thread thread;
};

struct Reaper
{
  std::mutex m;
  std::vector<Parked> parked;

  // A thread that never returned cannot be joined: it is detached and the module that holds its
  // code stays loaded, so that it can always return.
  ~Reaper()
  {
    std::vector<Parked> rest;
    {
      std::lock_guard<std::mutex> lock(m);
      rest.swap(parked);
    }
    bool detached = false;
    for (Parked & entry : rest) {
      if (!entry.thread.joinable()) {
        continue;
      }
      if (entry.exited->load()) {
        entry.thread.join();
      } else {
        entry.thread.detach();
        detached = true;
      }
    }
    if (detached) {
      pin_this_module();
    }
  }
};

Reaper & reaper()
{
  static Reaper instance;
  return instance;
}

}  // namespace

FrameWorker::FrameWorker(std::shared_ptr<State> state) : state_(std::move(state)) {}

std::shared_ptr<FrameWorker> FrameWorker::start(
  std::string name, std::shared_ptr<IReleaseLane> lane, Jobs jobs,
  const FrameWorkerTimings & timings, FrameWorkerLog log)
{
  auto state = std::make_shared<State>();
  state->name = std::move(name);
  state->lane = std::move(lane);
  state->jobs = std::move(jobs);
  state->timings = timings;
  state->log = std::move(log);
  if (state->lane && !state->lane->enter_worker(state->slot)) {
    return nullptr;
  }
  g_running.fetch_add(1);
  std::shared_ptr<FrameWorker> worker(new FrameWorker(state));
  try {
    worker->thread_ = std::thread([state] {
          State & s = *state;
          const double limit_ms = static_cast<double>(s.timings.slow_call.count());
          mark_frame_worker_thread(
            [&s](const char * call, double took_ms) { s.report_slow(call, took_ms); },
            &s.slot->clock, &s.slot->abandoned, limit_ms);
          if (!s.slot->abandoned.load()) {
            s.print("frame worker started tid=" + std::to_string(thread_number()));
          }
          for (;;) {
            bool run_show = false;
            {
              std::unique_lock<std::mutex> lock(s.m);
              s.cv.wait(lock, [&s] { return s.stop || s.handed || s.kicked; });
              if (s.stop) {
                break;
              }
              if (s.handed) {
                s.handed = false;
                run_show = true;
              } else {
                s.kicked = false;
              }
            }
            if (s.slot->abandoned.load()) {
              break;
            }
            const Clock::time_point began = Clock::now();
            const std::function<void()> & job = run_show ? s.jobs.show : s.jobs.poll;
            (run_show ? s.shows : s.polls).fetch_add(1);
            try {
              if (job) {
                job();
              }
            } catch (...) {
              // A job reports its failures itself; an exception must not end the process.
            }
            if (run_show) {
              std::lock_guard<std::mutex> lock(s.m);
              s.kicked = false;
            }
            if (s.notified.exchange(false) && !s.slot->abandoned.load()) {
              s.print(
                "frame worker free again after " +
                seconds_text(std::chrono::duration<double>(Clock::now() - began).count()) + " s");
            }
          }
          if (!s.slot->abandoned.load() && s.jobs.leave) {
            try {
              s.jobs.leave();
            } catch (...) {
              // See above.
            }
          }
          mark_frame_worker_thread({}, nullptr, nullptr);
          // From "exited" on the thread does nothing that can block, so the reaper may join it.
          g_running.fetch_sub(1);
          s.exited.store(true);
          if (s.lane) {
            s.lane->leave_worker(s.slot);
          }
        });
  } catch (...) {
    if (state->lane) {
      state->lane->leave_worker(state->slot);
    }
    g_running.fetch_sub(1);
    return nullptr;
  }
  return worker;
}

FrameWorker::~FrameWorker()
{
  stop();
}

void FrameWorker::kick()
{
  State & s = *state_;
  s.announce_wait(nullptr, nullptr);
  const std::int64_t now = now_ns();
  std::int64_t last = s.last_kick_ns.load();
  const std::int64_t gap =
    std::chrono::duration_cast<std::chrono::nanoseconds>(s.timings.kick_min).count();
  if (now - last < gap || !s.last_kick_ns.compare_exchange_strong(last, now)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(s.m);
    if (s.stop) {
      return;
    }
    s.kicked = true;
  }
  s.cv.notify_one();
}

void FrameWorker::hand_over()
{
  State & s = *state_;
  s.announce_wait(nullptr, nullptr);
  {
    std::lock_guard<std::mutex> lock(s.m);
    if (s.stop) {
      return;
    }
    s.handed = true;
  }
  s.cv.notify_one();
}

void FrameWorker::stop()
{
  if (stopped_.exchange(true)) {
    return;
  }
  State & s = *state_;
  {
    std::lock_guard<std::mutex> lock(s.m);
    s.stop = true;
  }
  s.cv.notify_one();
  std::string call;
  double seconds = 0.0;
  if (!s.exited.load() && s.stuck(&call, &seconds) && !s.left_line.exchange(true)) {
    s.print(
      "frame worker left running inside " + call + " after " + seconds_text(seconds) +
      " s; its " + s.log.handle + " stays unreleased");
    pin_this_module();
  }
  if (thread_.joinable()) {
    Reaper & r = reaper();
    {
      std::lock_guard<std::mutex> lock(r.m);
      r.parked.push_back(Parked{state_, &state_->exited, std::move(thread_)});
    }
    reap();
  }
}

bool FrameWorker::stopped() const
{
  return stopped_.load();
}

bool FrameWorker::exited() const
{
  return state_->exited.load();
}

bool FrameWorker::abandoned() const
{
  return state_->slot->abandoned.load();
}

bool FrameWorker::waiting(Wait * out)
{
  std::string call;
  double seconds = 0.0;
  if (!state_->announce_wait(&call, &seconds)) {
    return false;
  }
  if (out != nullptr) {
    out->call = call;
    out->seconds = seconds;
  }
  return true;
}

const OwnerCallClock & FrameWorker::clock() const
{
  return state_->slot->clock;
}

unsigned long long FrameWorker::polls() const
{
  return state_->polls.load();
}

unsigned long long FrameWorker::shows() const
{
  return state_->shows.load();
}

int FrameWorker::running()
{
  return g_running.load();
}

void FrameWorker::reap()
{
  Reaper & r = reaper();
  std::vector<Parked> ended;
  {
    std::lock_guard<std::mutex> lock(r.m);
    for (auto it = r.parked.begin(); it != r.parked.end();) {
      if (it->exited->load()) {
        ended.push_back(std::move(*it));
        it = r.parked.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (Parked & entry : ended) {
    if (entry.thread.joinable()) {
      entry.thread.join();
    }
  }
}

int FrameWorker::parked()
{
  Reaper & r = reaper();
  std::lock_guard<std::mutex> lock(r.m);
  return static_cast<int>(r.parked.size());
}

}  // namespace qar_qar_adapter
