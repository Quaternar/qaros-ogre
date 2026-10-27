// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// What the tests of the session owner share: an owner on the fake library, a folder of its own
// for the stored pairing, the log lines it printed, and the events the consumer polled.

#ifndef QAR_QAR_ADAPTER__TEST__SESSION_OWNER_HARNESS_HPP_
#define QAR_QAR_ADAPTER__TEST__SESSION_OWNER_HARNESS_HPP_

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fake_qar_api.hpp"
#include "qar_qar_adapter/identity_store.hpp"
#include "qar_qar_adapter/qar_session_owner.hpp"

namespace qar_qar_adapter
{
namespace test
{

using namespace std::chrono_literals;

/// Tells a test whether the heap block that holds a given address was freed. alloc_watch.cpp,
/// linked into the executable of the session tests, replaces the global operator new and delete
/// with ones that keep the size of each block in front of it.
namespace alloc_watch
{
/// Starts watching the block that contains @p address (any thread). Replaces an earlier watch.
void arm(const void * address);
/// True when the watched block was freed since arm().
bool freed();
/// True once a block was watched.
bool armed();
}  // namespace alloc_watch

/// A listener that records what it hears and can be made to block.
class RecordingListener : public ITargetListener
{
public:
  void on_target_up(const TargetInfo & target) override
  {
    entered_.store(true);
    std::function<void()> hook;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      hook = on_up_hook_;
    }
    if (hook) {
      hook();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ups_.push_back(target);
    sequence_.push_back("up:" + std::to_string(target.id.data[0]));
  }

  void on_target_down(const QarPeerId & id, QarPeerPresence presence) override
  {
    std::lock_guard<std::mutex> lock(mutex_);
    downs_.emplace_back(id, presence);
    sequence_.push_back("down:" + std::to_string(id.data[0]));
  }

  std::size_t ups() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return ups_.size();
  }
  std::size_t downs() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return downs_.size();
  }
  std::vector<TargetInfo> up_list() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return ups_;
  }
  std::vector<std::pair<QarPeerId, QarPeerPresence>> down_list() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return downs_;
  }
  /// What the listener heard, in order: "up:<first id byte>" and "down:<first id byte>".
  std::vector<std::string> sequence() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return sequence_;
  }
  void set_up_hook(std::function<void()> hook)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    on_up_hook_ = std::move(hook);
  }
  bool entered() const { return entered_.load(); }

private:
  mutable std::mutex mutex_;
  std::vector<TargetInfo> ups_;
  std::vector<std::pair<QarPeerId, QarPeerPresence>> downs_;
  std::vector<std::string> sequence_;
  std::function<void()> on_up_hook_;
  std::atomic<bool> entered_{false};
};

class Harness
{
public:
  /// @param mode The mode the owner is asked for.
  /// @param installed What the loader says about an installed QAROS.
  explicit Harness(HubMode mode = HubMode::kLocal, bool installed = true, bool load_ok = true)
  : installed_(installed), load_ok_(load_ok)
  {
    static std::atomic<int> counter{0};
    dir_ = (std::filesystem::temp_directory_path() /
      ("qar_identity_tests_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
      std::to_string(counter.fetch_add(1)))).u8string();
    config_.mode = mode;
    config_.program_tag = "test-program";
    config_.display_name = "Test program on host";
    config_.identity_dir = dir_;
    config_.log_folder_path = (std::filesystem::u8path(dir_) / "library-logs").u8string();
    config_.log_sink = [this](const std::string & line) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        log_.push_back(line);
      };
  }

  ~Harness()
  {
    owner_.reset();
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::u8path(dir_), ec);
  }

  SessionOwnerConfig & config() { return config_; }
  FakeQarApi & fake() { return fake_; }
  QarSessionOwner & owner() { return *owner_; }
  std::shared_ptr<QarSessionOwner> owner_ptr() { return owner_; }
  const std::string & dir() const { return dir_; }
  std::string identity_path() const { return identity_file_path(dir_); }
  const std::vector<SessionEvent> & events() const { return events_; }

  /// Makes the owner; call after the configuration and the fake are set up.
  void start()
  {
    owner_ = std::make_shared<QarSessionOwner>(config_, fake_.make_loader(installed_, load_ok_));
  }

  /// Polls until @p predicate holds or @p timeout passes.
  bool wait_for(const std::function<bool()> & predicate, std::chrono::milliseconds timeout = 5s)
  {
    const auto end = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      owner_->poll(events_);
      if (predicate()) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= end) {
        return false;
      }
      std::this_thread::sleep_for(2ms);
    }
  }

  bool wait_state(SessionState state, std::chrono::milliseconds timeout = 5s)
  {
    return wait_for([this, state] { return owner_->state() == state; }, timeout);
  }

  /// connect() and wait until the consumer can use the session.
  bool connect_and_run()
  {
    owner_->connect();
    return wait_for(
      [this] { return owner_->state() == SessionState::kRunning && owner_->ready(); });
  }

  /// Destroys the owner within @p limit. @return false when it took longer.
  bool destroy_owner_within(std::chrono::milliseconds limit = 5s)
  {
    auto future = std::async(std::launch::async, [this] { owner_.reset(); });
    return future.wait_for(limit) == std::future_status::ready;
  }

  bool logged(const std::string & text) const
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    for (const std::string & line : log_) {
      if (line.find(text) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  std::vector<std::string> log_lines() const
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    return log_;
  }

  /// The first state event that carries @p state, or null.
  const SessionEvent * state_event(SessionState state) const
  {
    for (const SessionEvent & event : events_) {
      if (event.kind == SessionEvent::Kind::kState && event.state == state) {
        return &event;
      }
    }
    return nullptr;
  }

  /// The last state event, or null.
  const SessionEvent * last_state_event() const
  {
    const SessionEvent * last = nullptr;
    for (const SessionEvent & event : events_) {
      if (event.kind == SessionEvent::Kind::kState) {
        last = &event;
      }
    }
    return last;
  }

  /// The thread that made the first call into the fake library: the owner thread.
  std::thread::id owner_thread() const
  {
    const auto calls = const_cast<FakeQarApi &>(fake_).calls();
    return calls.empty() ? std::thread::id() : calls.front().thread;
  }

private:
  FakeQarApi fake_;
  SessionOwnerConfig config_;
  bool installed_;
  bool load_ok_;
  std::string dir_;
  mutable std::mutex log_mutex_;
  std::vector<std::string> log_;
  std::vector<SessionEvent> events_;
  std::shared_ptr<QarSessionOwner> owner_;
};

}  // namespace test
}  // namespace qar_qar_adapter

#endif  // QAR_QAR_ADAPTER__TEST__SESSION_OWNER_HARNESS_HPP_
