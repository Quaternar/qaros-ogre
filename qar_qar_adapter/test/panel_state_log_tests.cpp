// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The log lines a panel adapter prints about a panel: the change of its state (a URL panel hides
// and shows with `qar_gui_panels_set_state`), the runtime's own record of it, a transfer that is
// reconnecting and the first frame shown. The calls themselves need a QarOS session; what is
// tested here is the lines: their words, the ids as the runtime writes them, and the clock stamp,
// and the logic that decides when a record of the runtime is a change and whether it names the
// stream the adapter built the panel's sender with.

#include <gtest/gtest.h>

#include "qar_qar_adapter/log_time.hpp"
#include "qar_qar_adapter/panel_state_log.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using qar_qar_adapter::panel_id_uuid;
using qar_qar_adapter::panel_state_name;
using qar_qar_adapter::url_panel_state_line;
using qar_qar_adapter::wall_clock_hms_ms;

namespace
{

QarGuiPanelId
sequential_id()
{
  QarGuiPanelId id{};
  for (std::size_t i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    id.data[i] = static_cast<std::uint8_t>(i);
  }
  return id;
}

}  // namespace

TEST(PanelStateLog, TheStateNamesAreTheOnesTheStreamPanelAdapterPrints)
{
  EXPECT_EQ(panel_state_name(QAR_GUI_PANEL_STATE_VISIBLE), "VISIBLE");
  EXPECT_EQ(panel_state_name(QAR_GUI_PANEL_STATE_MINIMIZED), "MINIMIZED");
  EXPECT_EQ(panel_state_name(QAR_GUI_PANEL_STATE_HIDDEN), "HIDDEN");
  EXPECT_EQ(panel_state_name(QAR_GUI_PANEL_STATE_CLOSED), "CLOSED");
  EXPECT_EQ(panel_state_name(static_cast<QarGuiPanelState>(77)), "state 77");
}

TEST(PanelStateLog, AnIdIsTheCanonicalUuidTextTheRuntimeLogsItAs)
{
  EXPECT_EQ(panel_id_uuid(sequential_id()), "00010203-0405-0607-0809-0a0b0c0d0e0f");
  QarGuiPanelId high{};
  for (std::size_t i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    high.data[i] = 0xF0;
  }
  EXPECT_EQ(panel_id_uuid(high), "f0f0f0f0-f0f0-f0f0-f0f0-f0f0f0f0f0f0")
    << "lower case, and a byte above 127 does not turn negative";
  EXPECT_EQ(panel_id_uuid(QarGuiPanelId{}), "00000000-0000-0000-0000-000000000000");
}

TEST(PanelStateLog, ASuccessfulChangeIsOneLineWithTheStateAndTheId)
{
  const std::string id = panel_id_uuid(sequential_id());
  EXPECT_EQ(
    url_panel_state_line("12:22:11.631", "/qar_demo/battery", id, QAR_GUI_PANEL_STATE_HIDDEN, ""),
    "[qar-url-panel-adapter] 12:22:11.631 panel '/qar_demo/battery' state -> HIDDEN "
    "(id 00010203-0405-0607-0809-0a0b0c0d0e0f)");
  EXPECT_EQ(
    url_panel_state_line("12:22:12.000", "AR control", id, QAR_GUI_PANEL_STATE_VISIBLE, ""),
    "[qar-url-panel-adapter] 12:22:12.000 panel 'AR control' state -> VISIBLE "
    "(id 00010203-0405-0607-0809-0a0b0c0d0e0f)");
}

TEST(PanelStateLog, AFailedChangeSaysSoWithTheReason)
{
  const std::string line = url_panel_state_line(
    "12:22:11.631", "AR control", "none", QAR_GUI_PANEL_STATE_HIDDEN, "no live panel to change");
  EXPECT_EQ(
    line,
    "[qar-url-panel-adapter] 12:22:11.631 panel 'AR control' state -> HIDDEN FAILED (id none): "
    "no live panel to change");
  const std::string with_code = url_panel_state_line(
    "12:22:11.631", "AR control", panel_id_uuid(sequential_id()), QAR_GUI_PANEL_STATE_VISIBLE,
    "qar_gui_panels_set_state failed: code=12 msg=bad");
  EXPECT_NE(with_code.find("state -> VISIBLE FAILED (id 00010203-"), std::string::npos);
  EXPECT_NE(with_code.find("code=12 msg=bad"), std::string::npos);
}

TEST(PanelStateLog, TheLineKeepsTheOrderTimeKeyStateOfTheStreamPanelAdaptersLine)
{
  // "[qar-panel-adapter] <time> panel '<key>' state -> <STATE>" is the stream-panel adapter's
  // line; the URL panel's differs in the tag and in the id that follows the state.
  const std::string line = url_panel_state_line(
    "01:02:03.004", "k", "none", QAR_GUI_PANEL_STATE_MINIMIZED, "");
  EXPECT_EQ(line.rfind("[qar-url-panel-adapter] 01:02:03.004 panel 'k' state -> MINIMIZED", 0), 0u);
}

TEST(LogTime, TheStampIsHoursMinutesSecondsAndMilliseconds)
{
  const std::string stamp = wall_clock_hms_ms();
  ASSERT_EQ(stamp.size(), 12u) << stamp;
  for (std::size_t i = 0; i < stamp.size(); ++i) {
    if (i == 2 || i == 5) {
      EXPECT_EQ(stamp[i], ':') << stamp;
    } else if (i == 8) {
      EXPECT_EQ(stamp[i], '.') << stamp;
    } else {
      EXPECT_TRUE(std::isdigit(static_cast<unsigned char>(stamp[i]))) << stamp;
    }
  }
  EXPECT_LE(std::stoi(stamp.substr(0, 2)), 23);
  EXPECT_LE(std::stoi(stamp.substr(3, 2)), 59);
  EXPECT_LE(std::stoi(stamp.substr(6, 2)), 60);
}

// ---------------------------------------------------------------------------
// The runtime's own record of a panel, a transfer that reconnects and the first frame shown.
// ---------------------------------------------------------------------------

namespace
{

using qar_qar_adapter::PanelRecordTrack;
using qar_qar_adapter::PanelRuntimeRecord;
using qar_qar_adapter::RuntimeRecordLog;
using qar_qar_adapter::id_bytes_uuid;
using qar_qar_adapter::runtime_record_changed;
using qar_qar_adapter::runtime_record_line;
using qar_qar_adapter::runtime_record_matches;

const char * const kOurs = "00010203-0405-0607-0809-0a0b0c0d0e0f";
const char * const kOther = "f0f0f0f0-f0f0-f0f0-f0f0-f0f0f0f0f0f0";

PanelRuntimeRecord
record_with_stream(std::uint8_t first_byte, bool known = true)
{
  PanelRuntimeRecord record;
  record.state_known = true;
  record.state = QAR_GUI_PANEL_STATE_VISIBLE;
  record.stream_known = known;
  for (std::size_t i = 0; i < QAR_MAX_ID_LENGTH; ++i) {
    record.stream_id[i] = static_cast<std::uint8_t>(first_byte + i);
  }
  record.size_known = true;
  record.width_meters = 0.57f;
  record.height_meters = 0.565336f;
  return record;
}

std::chrono::system_clock::time_point
at_time(int hour, int minute, int second)
{
  std::tm local{};
  local.tm_year = 126;
  local.tm_mon = 9;
  local.tm_mday = 22;
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_sec = second;
  local.tm_isdst = -1;
  return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

}  // namespace

TEST(RuntimeRecord, AStreamThatIsOursReadsMatchAndAnotherOneReadsMISMATCH)
{
  const PanelRuntimeRecord ours = record_with_stream(0x00);
  EXPECT_EQ(id_bytes_uuid(ours.stream_id), kOurs);
  EXPECT_TRUE(runtime_record_matches(ours, kOurs));
  EXPECT_FALSE(runtime_record_matches(ours, kOther));

  EXPECT_EQ(
    runtime_record_line("10:40:05.357", "dialog:Add", 2, ours, kOurs),
    std::string("[qar-panel-adapter] 10:40:05.357 panel 'dialog:Add' runtime record #2: ") +
    "state=VISIBLE content_stream=" + kOurs + " (ours " + kOurs + ": match) " +
    "size=0.570000x0.565336 m");
  EXPECT_EQ(
    runtime_record_line("10:40:05.357", "dialog:Add", 2, ours, kOther),
    std::string("[qar-panel-adapter] 10:40:05.357 panel 'dialog:Add' runtime record #2: ") +
    "state=VISIBLE content_stream=" + kOurs + " (ours " + kOther + ": MISMATCH) " +
    "size=0.570000x0.565336 m");
}

TEST(RuntimeRecord, NothingIsAMatchUnlessBothIdsAreThereAndEqual)
{
  const PanelRuntimeRecord no_stream = record_with_stream(0x00, false);
  EXPECT_FALSE(runtime_record_matches(no_stream, kOurs))
    << "the runtime named no stream for the panel";
  EXPECT_FALSE(runtime_record_matches(record_with_stream(0x00), ""))
    << "the adapter has no stream id to compare with";
  EXPECT_EQ(
    runtime_record_line("01:02:03.004", "k", 1, no_stream, kOurs),
    std::string("[qar-panel-adapter] 01:02:03.004 panel 'k' runtime record #1: state=VISIBLE ") +
    "content_stream=none (ours " + kOurs + ": MISMATCH) size=0.570000x0.565336 m");
  EXPECT_EQ(
    runtime_record_line("01:02:03.004", "k", 1, record_with_stream(0x00), ""),
    std::string("[qar-panel-adapter] 01:02:03.004 panel 'k' runtime record #1: state=VISIBLE ") +
    "content_stream=" + kOurs + " (ours none: MISMATCH) size=0.570000x0.565336 m");
}

TEST(RuntimeRecord, AFieldTheRuntimeRefusedReadsUnknown)
{
  PanelRuntimeRecord record;
  EXPECT_EQ(
    runtime_record_line("01:02:03.004", "k", 3, record, kOurs),
    std::string("[qar-panel-adapter] 01:02:03.004 panel 'k' runtime record #3: state=unknown ") +
    "content_stream=none (ours " + kOurs + ": MISMATCH) size=unknown");
}

TEST(RuntimeRecord, AChangeOfStateStreamOrSizeIsAChangeAndNothingElseIs)
{
  const PanelRuntimeRecord base = record_with_stream(0x00);
  EXPECT_FALSE(runtime_record_changed(base, base));

  PanelRuntimeRecord state = base;
  state.state = QAR_GUI_PANEL_STATE_HIDDEN;
  EXPECT_TRUE(runtime_record_changed(base, state));

  PanelRuntimeRecord stream = base;
  stream.stream_id[15] ^= 0x01;
  EXPECT_TRUE(runtime_record_changed(base, stream));

  PanelRuntimeRecord unnamed = base;
  unnamed.stream_known = false;
  EXPECT_TRUE(runtime_record_changed(base, unnamed));

  PanelRuntimeRecord size = base;
  size.height_meters = 0.585336f;
  EXPECT_TRUE(runtime_record_changed(base, size));

  PanelRuntimeRecord jitter = base;
  jitter.height_meters = base.height_meters + 1.0e-8f;
  EXPECT_FALSE(runtime_record_changed(base, jitter))
    << "a float that differs beyond the sixth decimal is not a change";
}

TEST(RuntimeRecord, TheTrackNumbersTheChangesFromOneAndSkipsRepeats)
{
  PanelRecordTrack track(kOurs);
  EXPECT_EQ(track.ours(), kOurs);
  EXPECT_EQ(track.count(), 0u);
  const PanelRuntimeRecord first = record_with_stream(0x00);
  EXPECT_TRUE(track.note(first)) << "the first record is always a change";
  EXPECT_EQ(track.count(), 1u);
  EXPECT_FALSE(track.note(first));
  EXPECT_EQ(track.count(), 1u);
  PanelRuntimeRecord hidden = first;
  hidden.state = QAR_GUI_PANEL_STATE_HIDDEN;
  EXPECT_TRUE(track.note(hidden));
  EXPECT_EQ(track.count(), 2u);
  EXPECT_TRUE(track.note(first)) << "back to the first record is a change from the second";
  EXPECT_EQ(track.count(), 3u);
}

TEST(RuntimeRecordLog, EachChangeIsOneLineWithTheTimeItArrivedAt)
{
  RuntimeRecordLog log;
  log.begin("dialog:Add", kOurs);
  PanelRuntimeRecord record = record_with_stream(0x00);
  EXPECT_TRUE(log.note("dialog:Add", record, at_time(10, 40, 5)));
  EXPECT_FALSE(log.note("dialog:Add", record, at_time(10, 40, 6))) << "the same record again";
  record.state = QAR_GUI_PANEL_STATE_HIDDEN;
  EXPECT_TRUE(log.note("dialog:Add", record, at_time(10, 40, 16)));

  const std::vector<std::string> lines = log.take_lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(
    lines[0].rfind(
      "[qar-panel-adapter] 10:40:05.000 panel 'dialog:Add' runtime record #1: state=VISIBLE", 0),
    0u) << lines[0];
  EXPECT_EQ(
    lines[1].rfind(
      "[qar-panel-adapter] 10:40:16.000 panel 'dialog:Add' runtime record #2: state=HIDDEN", 0),
    0u) << lines[1];
  EXPECT_NE(lines[0].find("(ours " + std::string(kOurs) + ": match)"), std::string::npos);
  EXPECT_TRUE(log.take_lines().empty()) << "the queue is empty after it was taken";
}

TEST(RuntimeRecordLog, EveryPanelIsNumberedAndComparedOnItsOwn)
{
  RuntimeRecordLog log;
  log.begin("a", kOurs);
  log.begin("b", kOther);
  const PanelRuntimeRecord record = record_with_stream(0x00);
  EXPECT_TRUE(log.note("a", record, at_time(1, 0, 0)));
  EXPECT_TRUE(log.note("b", record, at_time(1, 0, 1)));
  const std::vector<std::string> lines = log.take_lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_NE(lines[0].find("panel 'a' runtime record #1"), std::string::npos);
  EXPECT_NE(lines[0].find(": match)"), std::string::npos);
  EXPECT_NE(lines[1].find("panel 'b' runtime record #1"), std::string::npos);
  EXPECT_NE(lines[1].find(": MISMATCH)"), std::string::npos);
}

TEST(RuntimeRecordLog, APanelThatWasNeverBegunOrWasEndedQueuesNothing)
{
  RuntimeRecordLog log;
  const PanelRuntimeRecord record = record_with_stream(0x00);
  EXPECT_FALSE(log.note("unknown", record, at_time(1, 0, 0)));
  log.begin("a", kOurs);
  log.end("a");
  EXPECT_FALSE(log.note("a", record, at_time(1, 0, 0)))
    << "records of a retiring subscription are ignored";
  EXPECT_TRUE(log.take_lines().empty());
}

TEST(RuntimeRecordLog, ABeginStartsTheNumberingAgainForARebuiltPanel)
{
  RuntimeRecordLog log;
  log.begin("a", kOurs);
  EXPECT_TRUE(log.note("a", record_with_stream(0x00), at_time(1, 0, 0)));
  log.begin("a", kOther);
  EXPECT_TRUE(log.note("a", record_with_stream(0x00), at_time(1, 0, 1)))
    << "the first record of the new subscription is a change";
  const std::vector<std::string> lines = log.take_lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_NE(lines[1].find("runtime record #1"), std::string::npos) << lines[1];
  EXPECT_NE(lines[1].find("(ours " + std::string(kOther) + ": MISMATCH)"), std::string::npos);
}

TEST(RuntimeRecordLog, AFullQueueDropsAndCountsAndTheNextLineShowsTheGapInItsNumber)
{
  RuntimeRecordLog log;
  log.begin("a", kOurs);
  PanelRuntimeRecord record = record_with_stream(0x00);
  for (std::size_t i = 0; i < RuntimeRecordLog::kMaxQueued; ++i) {
    record.width_meters = 1.0f + static_cast<float>(i);
    ASSERT_TRUE(log.note("a", record, at_time(1, 0, 0))) << i;
  }
  record.width_meters = 9999.0f;
  EXPECT_FALSE(log.note("a", record, at_time(1, 0, 1)));
  EXPECT_EQ(log.dropped(), 1u);
  EXPECT_EQ(log.take_lines().size(), RuntimeRecordLog::kMaxQueued);
  record.width_meters = 10000.0f;
  EXPECT_TRUE(log.note("a", record, at_time(1, 0, 2)));
  const std::vector<std::string> lines = log.take_lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(
    lines[0].find("runtime record #" + std::to_string(RuntimeRecordLog::kMaxQueued + 2)),
    std::string::npos) << lines[0];
}

TEST(PanelLines, ATransferThatReconnectsIsOneLineWithTheStreamAndTheFrameCount)
{
  EXPECT_EQ(
    qar_qar_adapter::reconnecting_line(
      "10:40:28.301", "dialog:Add", "808c5a8c-20af-46f6-8946-6eb33c1c32ab", 49),
    "[qar-panel-adapter] 10:40:28.301 panel 'dialog:Add' stream "
    "808c5a8c-20af-46f6-8946-6eb33c1c32ab: transfer RECONNECTING after 49 frames");
}

TEST(PanelLines, TheFirstFrameIsOneLineWithTheThread)
{
  EXPECT_EQ(
    qar_qar_adapter::first_panel_frame_line("10:40:05.724", "dialog:Add", 14972ull),
    "[qar-panel-adapter] 10:40:05.724 panel 'dialog:Add' first show_frame tid=14972");
}

TEST(LogTime, ATimePointIsFormattedLikeTheClockLines)
{
  EXPECT_EQ(qar_qar_adapter::hms_ms_of(at_time(7, 8, 9)), "07:08:09.000");
  EXPECT_EQ(
    qar_qar_adapter::hms_ms_of(at_time(23, 59, 58) + std::chrono::milliseconds(42)),
    "23:59:58.042");
}

TEST(LogOnce, OnlyTheFirstCallPassesAndEveryObjectCountsOnItsOwn)
{
  qar_qar_adapter::LogOnce one;
  qar_qar_adapter::LogOnce two;
  EXPECT_FALSE(one.done());
  EXPECT_TRUE(one.first());
  EXPECT_TRUE(one.done());
  EXPECT_FALSE(one.first());
  EXPECT_FALSE(one.first());
  EXPECT_TRUE(two.first()) << "another object has its own first call";
}

TEST(LogOnce, OfManyThreadsAskingAtOnceExactlyOneGetsThrough)
{
  qar_qar_adapter::LogOnce once;
  std::atomic<int> winners{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&] {
        while (!go.load()) {}
        for (int k = 0; k < 1000; ++k) {
          if (once.first()) { ++winners; }
        }
      });
  }
  go = true;
  for (std::thread & thread : threads) { thread.join(); }
  EXPECT_EQ(winners.load(), 1);
}

#ifdef _WIN32
TEST(ThreadNumber, IsTheOperatingSystemThreadIdOnWindows)
{
  EXPECT_EQ(
    qar_qar_adapter::thread_number(), static_cast<unsigned long long>(GetCurrentThreadId()));
  unsigned long long other = 0;
  unsigned long long system_id = 0;
  std::thread thread([&] {
      other = qar_qar_adapter::thread_number();
      system_id = static_cast<unsigned long long>(GetCurrentThreadId());
    });
  thread.join();
  EXPECT_EQ(other, system_id) << "the number a log line prints is the one a debugger names";
}
#endif

TEST(ThreadNumber, IsTheSameWithinAThreadAndDiffersBetweenTwoThatLiveAtOnce)
{
  const unsigned long long mine = qar_qar_adapter::thread_number();
  EXPECT_NE(mine, 0ull) << "this library prints a thread id as a number";
  EXPECT_EQ(qar_qar_adapter::thread_number(), mine);

  unsigned long long other = 0;
  std::atomic<bool> release{false};
  std::atomic<bool> measured{false};
  std::thread thread([&] {
      other = qar_qar_adapter::thread_number();
      measured = true;
      while (!release.load()) {}
    });
  while (!measured.load()) {}
  EXPECT_NE(other, 0ull);
  EXPECT_NE(other, mine);
  release = true;
  thread.join();
}
