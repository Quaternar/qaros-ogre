// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The opt-in per-frame stage timing (timing_log.hpp): the CSV sink and writer thread, the
// trace a decoded frame carries through qar_qt_streaming's LatestVideoFrame, and the row
// that its QtPanelStream writes for every capture tick of a video panel. The analyser
// (analyze_video_timing.py in qar_rviz_plugin) reads exactly these columns, so the tests
// pin the formats.

#include <gtest/gtest.h>

#include "qar_ar_core/timing_log.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using qar_ar_core::kTimingBlank;
using qar_ar_core::TimingLog;
using qar_ar_core::TimingSink;

namespace
{

namespace fs = std::filesystem;

constexpr int kThreads = 4;
constexpr int kRowsEach = 3000;

/// A fresh scratch folder, removed with the object.
class TempDir
{
public:
  TempDir()
  {
    std::random_device rd;
    std::ostringstream name;
    name << "qar_timing_test_" << std::hex << rd() << rd();
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
  std::string str() const { return path_.string(); }

private:
  fs::path path_;
};

std::vector<std::string> read_lines(const fs::path & file)
{
  std::vector<std::string> lines;
  std::ifstream in(file, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  return lines;
}

std::vector<std::string> split_csv(const std::string & line)
{
  std::vector<std::string> cells;
  std::string cell;
  std::istringstream in(line);
  while (std::getline(in, cell, ',')) {
    cells.push_back(cell);
  }
  if (!line.empty() && line.back() == ',') {
    cells.emplace_back();  // a trailing empty field
  }
  return cells;
}

/// One row of a CSV as header-name -> text.
using Row = std::map<std::string, std::string>;

std::vector<Row> read_rows(const fs::path & file)
{
  const std::vector<std::string> lines = read_lines(file);
  std::vector<Row> rows;
  if (lines.empty()) {
    return rows;
  }
  const std::vector<std::string> header = split_csv(lines[0]);
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const std::vector<std::string> cells = split_csv(lines[i]);
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

/// Poll until @p file has at least @p lines lines (the writer thread is asynchronous).
bool wait_for_lines(const fs::path & file, std::size_t lines, std::chrono::milliseconds limit)
{
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (read_lines(file).size() >= lines) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return read_lines(file).size() >= lines;
}

/// Makes TimingLog::instance() return @p log for the scope.
class ScopedTimingLog
{
public:
  explicit ScopedTimingLog(TimingLog & log)
  : previous_(TimingLog::override_instance_for_testing(&log))
  {
  }
  ~ScopedTimingLog() { TimingLog::override_instance_for_testing(previous_); }
  ScopedTimingLog(const ScopedTimingLog &) = delete;
  ScopedTimingLog & operator=(const ScopedTimingLog &) = delete;

private:
  TimingLog * previous_;
};

void set_env(const char * name, const char * value)
{
#ifdef _WIN32
  _putenv_s(name, value);   // "" removes the variable on Windows
#else
  if (*value == '\0') { unsetenv(name); } else { setenv(name, value, 1); }
#endif
}

}  // namespace

// ---------------------------------------------------------------------------
// TimingSink
// ---------------------------------------------------------------------------

TEST(TimingSink, AppendOnlyQueuesAndTheWriterCreatesTheFileWithItsHeader)
{
  TempDir dir;
  const fs::path file = dir.path() / "nested" / "rows.csv";
  TimingSink sink(file.string(), {"a", "b"});

  sink.append({1, 2});
  sink.append({3, 4});
  EXPECT_FALSE(fs::exists(file)) << "append must never do file I/O (it runs on the GUI thread)";
  EXPECT_EQ(sink.rows_appended(), 2u);

  EXPECT_EQ(sink.flush(), 2u);
  ASSERT_TRUE(fs::exists(file)) << "the folder and the file are created by the first flush";
  const std::vector<std::string> lines = read_lines(file);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "a,b");
  EXPECT_EQ(lines[1], "1,2");
  EXPECT_EQ(lines[2], "3,4");
  EXPECT_EQ(sink.rows_written(), 2u);

  sink.append({5, 6});
  EXPECT_EQ(sink.flush(), 1u);
  EXPECT_EQ(read_lines(file).size(), 4u) << "later flushes append, they do not rewrite";
}

TEST(TimingSink, BlankCellsAreEmptyFieldsAndLabelsReplaceTheirNumbers)
{
  TempDir dir;
  const fs::path file = dir.path() / "labels.csv";
  TimingSink sink(file.string(), {"t_us", "gap_us", "outcome"}, {"submitted", "not_ready"}, 2);

  sink.append({1790963860123456, kTimingBlank, 1});
  sink.append({-5, 7, 0});
  sink.append({9, kTimingBlank, 9});  // out of the label range: the number itself
  sink.flush();

  const std::vector<std::string> lines = read_lines(file);
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[1], "1790963860123456,,not_ready");
  EXPECT_EQ(lines[2], "-5,7,submitted");
  EXPECT_EQ(lines[3], "9,,9");
}

TEST(TimingSink, ARowOfTheWrongWidthIsDroppedNotWritten)
{
  TempDir dir;
  TimingSink sink((dir.path() / "w.csv").string(), {"a", "b", "c"});
  sink.append({1, 2});
  sink.append({1, 2, 3, 4});
  sink.append({1, 2, 3});
  EXPECT_EQ(sink.rows_dropped(), 2u);
  EXPECT_EQ(sink.flush(), 1u);
  const std::vector<std::string> lines = read_lines(dir.path() / "w.csv");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1], "1,2,3");
}

TEST(TimingSink, AStalledWriterCannotGrowTheQueueWithoutBound)
{
  TempDir dir;
  TimingSink sink((dir.path() / "cap.csv").string(), {"x"});
  const std::size_t extra = 10;
  for (std::size_t i = 0; i < TimingSink::kMaxPendingRows + extra; ++i) {
    sink.append({static_cast<std::int64_t>(i)});
  }
  EXPECT_EQ(sink.rows_dropped(), extra) << "past the cap rows are dropped and COUNTED";
  EXPECT_EQ(sink.flush(), TimingSink::kMaxPendingRows);
  EXPECT_EQ(read_lines(dir.path() / "cap.csv").size(), TimingSink::kMaxPendingRows + 1);
}

TEST(TimingSink, ConcurrentAppendersAndFlushesLoseNoRowAndCorruptNoLine)
{
  TempDir dir;
  const fs::path file = dir.path() / "mt.csv";
  TimingSink sink(file.string(), {"thread", "n"});
  std::atomic<bool> done{false};
  std::thread flusher([&] {
      while (!done.load()) {
        sink.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  std::vector<std::thread> producers;
  for (int t = 0; t < kThreads; ++t) {
    producers.emplace_back([&sink, t] {
        for (int n = 0; n < kRowsEach; ++n) {
          sink.append({t, n});
        }
      });
  }
  for (auto & p : producers) { p.join(); }
  done.store(true);
  flusher.join();
  sink.flush();

  const std::vector<Row> rows = read_rows(file);
  ASSERT_EQ(rows.size(), static_cast<std::size_t>(kThreads * kRowsEach));
  std::vector<int> next(kThreads, 0);
  for (const Row & row : rows) {
    const int t = static_cast<int>(as_int(row, "thread"));
    ASSERT_GE(t, 0);
    ASSERT_LT(t, kThreads);
    EXPECT_EQ(as_int(row, "n"), next[t]++) << "a thread's own rows must stay in order";
  }
}

TEST(TimingSink, AnUnwritableFolderIsReportedOnceAndRowsAreDiscardedQuietly)
{
  TempDir dir;
  const fs::path blocker = dir.path() / "blocker";
  { std::ofstream(blocker) << "a file where a folder is needed"; }

  TimingSink sink((blocker / "x.csv").string(), {"a"});
  sink.append({1});
  EXPECT_EQ(sink.flush(), 0u);
  sink.append({2});
  EXPECT_EQ(sink.flush(), 0u);          // no retry storm, no throw
  EXPECT_EQ(sink.rows_written(), 0u);
}

// ---------------------------------------------------------------------------
// TimingLog
// ---------------------------------------------------------------------------

TEST(TimingLog, EpochClockIsTheSystemClockInMicroseconds)
{
  using std::chrono::duration_cast;
  using std::chrono::microseconds;
  using std::chrono::system_clock;
  const auto before = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
  const std::int64_t stamped = qar_ar_core::timing_epoch_us();
  const auto after = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
  EXPECT_GE(stamped, before);
  EXPECT_LE(stamped, after);
  EXPECT_GT(stamped, 1600000000000000LL) << "microseconds since 1970, not a steady-clock count";
}

TEST(TimingLog, ADisabledLogOpensNothing)
{
  TimingLog log("");
  EXPECT_FALSE(log.enabled());
  EXPECT_EQ(log.open("anything", {"a"}), nullptr);
}

TEST(TimingLog, SanitizeNameMakesSafeFileStems)
{
  EXPECT_EQ(TimingLog::sanitize_name("video:/qar_test_video/image_raw"),
    "video_qar_test_video_image_raw");
  EXPECT_EQ(TimingLog::sanitize_name("/cam/left image"), "cam_left_image");
  EXPECT_EQ(TimingLog::sanitize_name("a..b-c"), "a..b-c");
  EXPECT_EQ(TimingLog::sanitize_name("///"), "unnamed");
  EXPECT_EQ(TimingLog::sanitize_name(""), "unnamed");
  EXPECT_EQ(TimingLog::sanitize_name("x.csv."), "x.csv");
  EXPECT_EQ(TimingLog::sanitize_name(std::string(300, 'a')).size(), 100u);
}

TEST(TimingLog, OpenReturnsTheSameSinkForTheSameNameAndColumnsAndSuffixesOtherwise)
{
  TempDir dir;
  TimingLog log(dir.str(), std::chrono::milliseconds(10000));
  auto first = log.open("video_x", {"a", "b"});
  auto again = log.open("video_x", {"a", "b"});
  auto other = log.open("video_x", {"a", "b", "c"});
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, again);
  ASSERT_NE(other, nullptr);
  EXPECT_NE(first, other);
  EXPECT_EQ(fs::path(first->path()).filename(), "video_x.csv");
  EXPECT_EQ(fs::path(other->path()).filename(), "video_x_2.csv");
  log.shutdown();
}

TEST(TimingLog, TheWriterThreadFlushesWithoutBeingAskedAndShutdownWritesTheRest)
{
  TempDir dir;
  TimingLog log(dir.str(), std::chrono::milliseconds(20));
  auto sink = log.open("bg", {"v"});
  ASSERT_NE(sink, nullptr);
  const fs::path file = dir.path() / "bg.csv";

  sink->append({11});
  ASSERT_TRUE(wait_for_lines(file, 2, std::chrono::milliseconds(5000)))
    << "the background thread must write the row on its own";

  sink->append({22});
  log.shutdown();   // joins the writer and writes whatever is still queued
  const std::vector<std::string> lines = read_lines(file);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[2], "22");

  // Restartable: a later open (or the same sink) goes on being written.
  auto sink2 = log.open("bg2", {"v"});
  sink2->append({33});
  ASSERT_TRUE(wait_for_lines(dir.path() / "bg2.csv", 2, std::chrono::milliseconds(5000)));
}

TEST(TimingLog, InstanceHonoursTheOverrideSeamAndTheEnvironmentVariable)
{
  TempDir dir;
  TimingLog log(dir.str(), std::chrono::milliseconds(10000));
  {
    ScopedTimingLog scope(log);
    EXPECT_EQ(&TimingLog::instance(), &log);
  }
  EXPECT_NE(&TimingLog::instance(), &log);

  set_env("QAR_VIDEO_TIMING_DIR", dir.str().c_str());
  EXPECT_EQ(TimingLog::directory_from_environment(), dir.str());
  set_env("QAR_VIDEO_TIMING_DIR", "");
  EXPECT_EQ(TimingLog::directory_from_environment(), "");
  log.shutdown();
}
