// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The timer of QarOS calls: a call that takes longer than the limit on the marked GUI thread is
// named in one warning line, and a call on any other thread is not timed.

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "qar_qar_adapter/gui_call_guard.hpp"

namespace
{

using namespace std::chrono_literals;
using qar_qar_adapter::reset_gui_call_stats;
using qar_qar_adapter::set_gui_call_sink;

// Collects the warning lines instead of printing them, and restores stderr when it goes.
class Capture
{
public:
  Capture()
  {
    reset_gui_call_stats();
    set_gui_call_sink([this](const std::string & line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(line);
      });
  }
  ~Capture()
  {
    set_gui_call_sink({});
    reset_gui_call_stats();
  }
  std::vector<std::string> lines() const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

TEST(GuiCallGuard, WarnsAboveTheLimitOnTheGuiThreadOnly)
{
  Capture capture;
  ASSERT_DOUBLE_EQ(qar_qar_adapter::gui_call_limit_ms(), 20.0);

  std::thread gui([] {
      qar_qar_adapter::mark_gui_thread();
      EXPECT_TRUE(qar_qar_adapter::on_gui_thread());
      {
        QAR_GUI_TIMED("qar_fast_call");
        std::this_thread::sleep_for(1ms);
      }
      {
        QAR_GUI_TIMED("qar_slow_call");
        std::this_thread::sleep_for(60ms);
      }
    });
  gui.join();

  std::thread other([] {
      EXPECT_FALSE(qar_qar_adapter::on_gui_thread());
      QAR_GUI_TIMED("qar_slow_call_elsewhere");
      std::this_thread::sleep_for(60ms);
    });
  other.join();

  const auto lines = capture.lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("[qar-gui-call] "), std::string::npos);
  EXPECT_NE(lines[0].find(" WARNING qar_slow_call took "), std::string::npos);
  EXPECT_NE(lines[0].find(" ms on the GUI thread (limit 20 ms)"), std::string::npos);
  const auto stats = qar_qar_adapter::gui_call_stats();
  EXPECT_EQ(stats.slow, 1u);
  EXPECT_EQ(stats.worst, "qar_slow_call");
  EXPECT_GE(stats.worst_ms, 50.0);
}

TEST(GuiCallGuard, PrintsTheFirstFiveOfACallAndThenOneInHundred)
{
  Capture capture;
  std::thread gui([] {
      qar_qar_adapter::mark_gui_thread();
      for (int i = 0; i < 101; ++i) {
        QAR_GUI_TIMED("qar_repeated_call");
        std::this_thread::sleep_for(21ms);
      }
    });
  gui.join();
  EXPECT_EQ(capture.lines().size(), 6u) << "calls 1..5 and call 100";
  EXPECT_EQ(qar_qar_adapter::gui_call_stats().slow, 101u);
}

TEST(GuiCallGuard, HandsACallOfHalfASecondOrMoreOnTheOwnerThreadToItsReport)
{
  Capture capture;
  std::mutex mutex;
  std::vector<std::string> reported;
  std::thread owner([&] {
      qar_qar_adapter::mark_owner_thread([&](const char * call, double took_ms) {
          std::lock_guard<std::mutex> lock(mutex);
          reported.push_back(std::string(call) + (took_ms >= 500.0 ? " slow" : " fast"));
        });
      {
        QAR_GUI_TIMED("qar_owner_quick");
        std::this_thread::sleep_for(60ms);
      }
      {
        QAR_GUI_TIMED("qar_owner_slow");
        std::this_thread::sleep_for(540ms);
      }
      qar_qar_adapter::mark_owner_thread({});
      {
        QAR_GUI_TIMED("qar_after_the_mark_ended");
        std::this_thread::sleep_for(520ms);
      }
    });
  owner.join();
  std::thread other([] {
      QAR_GUI_TIMED("qar_other_thread");
      std::this_thread::sleep_for(520ms);
    });
  other.join();
  ASSERT_EQ(reported.size(), 1u);
  EXPECT_EQ(reported[0], "qar_owner_slow slow");
  EXPECT_TRUE(capture.lines().empty()) << "the owner thread is not the GUI thread";
  EXPECT_EQ(qar_qar_adapter::gui_call_stats().slow, 0u);
}

TEST(GuiCallGuard, AnotherCallNameStartsItsOwnCount)
{
  Capture capture;
  std::thread gui([] {
      qar_qar_adapter::mark_gui_thread();
      for (int i = 0; i < 7; ++i) {
        QAR_GUI_TIMED("qar_call_a");
        std::this_thread::sleep_for(21ms);
      }
      {
        QAR_GUI_TIMED("qar_call_b");
        std::this_thread::sleep_for(21ms);
      }
    });
  gui.join();
  EXPECT_EQ(capture.lines().size(), 6u) << "five of a, one of b";
}

}  // namespace
