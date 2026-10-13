// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// RenderTimingState: the 3D stream's stage timing (render3d.csv). Header-only and QarOS-free,
// so it is exercised here without a session: the adapter feeds it the results of its
// try_begin_frame polls and its submits, and the analyser reads the rows pinned below.

#include <gtest/gtest.h>

#include "qar_qar_adapter/render_timing.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using qar_qar_adapter::RenderTimingState;
using qar_qar_adapter::SubmitTimingGuard;
using qar_ar_core::kTimingBlank;
using qar_ar_core::TimingLog;
using qar_ar_core::timing_epoch_us;

using Row = std::map<std::string, std::string>;

class TempDir
{
public:
  TempDir()
  {
    std::random_device rd;
    std::ostringstream name;
    name << "qar_render_timing_test_" << std::hex << rd() << rd();
    path_ = fs::temp_directory_path() / name.str();
    fs::create_directories(path_);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  TempDir(const TempDir &) = delete;
  TempDir & operator=(const TempDir &) = delete;
  const fs::path & path() const { return path_; }

private:
  fs::path path_;
};

std::vector<Row> read_rows(const fs::path & file)
{
  std::ifstream in(file, std::ios::binary);
  std::string line;
  std::vector<std::string> header;
  std::vector<Row> rows;
  bool first = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::vector<std::string> cells;
    std::string cell;
    std::istringstream cs(line);
    while (std::getline(cs, cell, ',')) {
      cells.push_back(cell);
    }
    if (!line.empty() && line.back() == ',') {
      cells.emplace_back();
    }
    if (first) {
      header = cells;
      first = false;
      continue;
    }
    Row row;
    for (std::size_t c = 0; c < header.size(); ++c) {
      row[header[c]] = (c < cells.size()) ? cells[c] : std::string();
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::int64_t as_int(const Row & row, const std::string & column)
{
  const auto it = row.find(column);
  EXPECT_NE(it, row.end()) << "no column " << column;
  return (it == row.end() || it->second.empty()) ? kTimingBlank : std::stoll(it->second);
}

/// A RenderTimingState writing to <dir>/render3d.csv.
struct Fixture
{
  Fixture()
  : log(dir.path().string(), std::chrono::milliseconds(10000)),
    state(log.open("render3d", RenderTimingState::columns(), RenderTimingState::kinds(),
        RenderTimingState::kKindColumn))
  {
  }
  ~Fixture() { log.shutdown(); }

  std::vector<Row> rows()
  {
    log.flush_all();
    return read_rows(dir.path() / "render3d.csv");
  }

  TempDir dir;
  TimingLog log;
  RenderTimingState state;
};

void pause_us(int us)
{
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

}  // namespace

TEST(RenderTiming, CsvHeaderIsTheDocumentedColumnSet)
{
  Fixture f;
  f.state.note_poll_call(timing_epoch_us());
  f.state.note_ready(false);
  std::ifstream in((f.dir.path() / "render3d.csv").string());   // not flushed yet
  EXPECT_FALSE(in.good()) << "queued only; the writer thread creates the file";
  f.log.flush_all();
  std::ifstream written((f.dir.path() / "render3d.csv").string());
  std::string header;
  std::getline(written, header);
  EXPECT_EQ(header, "kind,start_us,end_us,count,frame,detail,mid_us");
}

TEST(RenderTiming, ConsecutiveNotReadyPollsFoldIntoOneRunClosedByTheReady)
{
  Fixture f;
  for (int i = 0; i < 5; ++i) {
    f.state.note_poll_call(timing_epoch_us());
    f.state.note_pending();
    pause_us(300);
  }
  f.state.note_poll_call(timing_epoch_us());
  f.state.note_ready(/*from_prime=*/false);

  const std::vector<Row> rows = f.rows();
  ASSERT_EQ(rows.size(), 2u) << "thousands of polls per second must become ONE run row";
  EXPECT_EQ(rows[0].at("kind"), "not_ready");
  EXPECT_EQ(as_int(rows[0], "count"), 5);
  EXPECT_GT(as_int(rows[0], "end_us"), as_int(rows[0], "start_us"))
    << "the run lasts from the first pending poll to the poll that found it ready";
  EXPECT_EQ(rows[1].at("kind"), "ready");
  EXPECT_EQ(as_int(rows[1], "start_us"), as_int(rows[1], "end_us"));
  EXPECT_EQ(as_int(rows[1], "end_us"), as_int(rows[0], "end_us"))
    << "the run ends at the very poll the ready row records";
  EXPECT_EQ(as_int(rows[1], "detail"), 0);
}

TEST(RenderTiming, AReadyWithNoPendingBeforeItWritesNoRunAndAPrimedReadyIsFlagged)
{
  Fixture f;
  f.state.note_poll_call(timing_epoch_us());
  f.state.note_ready(/*from_prime=*/true);
  const std::vector<Row> rows = f.rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("kind"), "ready");
  EXPECT_EQ(as_int(rows[0], "detail"), 1) << "1 = observed by prime_next_frame()";
}

TEST(RenderTiming, ASubmitRowSpansConsumeToShowFrameWithTheRenderDoneInBetween)
{
  Fixture f;
  f.state.note_consume(41);
  pause_us(500);
  {
    SubmitTimingGuard guard(&f.state);   // stamps render done at construction
    pause_us(500);
    guard.show_returned(true);
    pause_us(500);                        // e.g. prime_next_frame(): must NOT count as submit time
  }
  const std::vector<Row> rows = f.rows();
  ASSERT_EQ(rows.size(), 1u);
  const Row & r = rows[0];
  EXPECT_EQ(r.at("kind"), "submit");
  EXPECT_EQ(as_int(r, "frame"), 41);
  EXPECT_EQ(as_int(r, "detail"), 1);
  EXPECT_LT(as_int(r, "start_us"), as_int(r, "mid_us"));
  EXPECT_LT(as_int(r, "mid_us"), as_int(r, "end_us"));
}

TEST(RenderTiming, AFailedSubmitIsRecordedWithOkZero)
{
  Fixture f;
  f.state.note_consume(7);
  {
    SubmitTimingGuard guard(&f.state);   // leaves without show_returned: a failure return
  }
  const std::vector<Row> rows = f.rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("kind"), "submit");
  EXPECT_EQ(as_int(rows[0], "detail"), 0);
}

TEST(RenderTiming, ASubmitWithoutAConsumedFrameWritesNothingAndANullStateIsInert)
{
  Fixture f;
  {
    SubmitTimingGuard guard(&f.state);
    guard.show_returned(true);
  }
  EXPECT_TRUE(f.rows().empty());

  SubmitTimingGuard inert(nullptr);      // timing off: the adapter passes a null state
  inert.show_returned(true);
}

TEST(RenderTiming, ATryBeginCallThatItselfBlocksIsFlaggedAsASlowPoll)
{
  Fixture f;
  f.state.note_poll_call(timing_epoch_us());          // returns at once: nothing to report
  f.state.note_poll_call(timing_epoch_us() - 9000);   // "took" 9 ms
  const std::vector<Row> rows = f.rows();
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].at("kind"), "slow_poll");
  EXPECT_GE(as_int(rows[0], "end_us") - as_int(rows[0], "start_us"), 9000);
}
