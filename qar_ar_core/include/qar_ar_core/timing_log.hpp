// Copyright 2026 Quaternar
// SPDX-License-Identifier: MIT

#ifndef QAR_AR_CORE__TIMING_LOG_HPP_
#define QAR_AR_CORE__TIMING_LOG_HPP_

/// @file timing_log.hpp
/// @brief Opt-in per-frame stage timing, written as CSV by a background thread.
///
/// ## What it is for
///
/// "Is the stutter ours?" needs numbers that attribute every long gap to one stage of
/// the pipeline: the publisher, the ROS transport, our decode, our GUI-thread capture,
/// or QarOS not offering a destination (`try_begin_frame` -> not ready). Each stage
/// stamps its own events with the same clock, and the `analyze_video_timing.py`
/// script of qar_rviz_plugin merges the files. The clock is `std::chrono::system_clock` as microseconds since the
/// Unix epoch (@ref timing_epoch_us): the one clock the publisher process, this process
/// and any other agree on, so cross-process latencies are meaningful.
///
/// ## Off by default, and cheap when on
///
/// Nothing exists unless the environment variable `QAR_VIDEO_TIMING_DIR` names a folder
/// (the live launcher's `-TimingCsv` sets it to a fresh `build\timing\<stamp>`). When it
/// is unset @ref TimingLog::open returns null, and every hook in the pipeline is one
/// null-pointer test.
///
/// When on, a hook builds a row of 64-bit integers on the calling thread and appends it
/// to a @ref TimingSink: one mutex-protected `vector::insert` (tens of nanoseconds), no
/// formatting, no allocation in the steady state, **no file I/O**. A single background
/// thread owned by @ref TimingLog wakes about once a second, swaps each sink's buffer
/// out under the lock (O(1)) and does all the number formatting and `fwrite`s outside
/// it. That is the rule the instrument has to keep: the GUI thread must never wait on a
/// disk, least of all while the question being asked is whether the disk stalls
/// everything. (Files are created by that thread too, on the first flush.)
///
/// The sinks buffer in memory without blocking while a disk is stalled; each is capped
/// (@ref TimingSink::kMaxPendingRows) so a dead disk cannot grow the process without
/// bound - rows past the cap are counted in @ref TimingSink::rows_dropped.
///
/// ## Lifetime in a plugin
///
/// The process-wide instance is created on first use and never destroyed (a detached
/// thread must not outlive code that was unloaded, and static destructors in a DLL run
/// under the loader lock where a join would deadlock). The host calls
/// @ref TimingLog::shutdown from its destructor - the panel does - which joins the
/// writer and flushes; a later @ref TimingLog::open simply restarts the thread.
///
/// Header-only and Qt/ROS-free on purpose: the ROS source, the Qt panel stream and the
/// QarOS render adapter all include it.

#ifdef _MSC_VER
#include <share.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace qar_ar_core
{

/// @brief Microseconds since the Unix epoch on the system clock - the timestamp of every
/// timing CSV. (Not steady_clock: that clock has a different origin in every process.)
inline std::int64_t timing_epoch_us()
{
  using std::chrono::duration_cast;
  using std::chrono::microseconds;
  using std::chrono::system_clock;
  return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

/// @brief A cell value meaning "no value": written as an empty CSV field.
inline constexpr std::int64_t kTimingBlank = std::numeric_limits<std::int64_t>::min();

namespace timing_detail
{

/// getenv without MSVC's C4996 ("unsafe") warning: a copy, "" when unset.
inline std::string get_env(const char * name)
{
#ifdef _MSC_VER
  char * value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
    return std::string();
  }
  std::string out(value);
  std::free(value);
  return out;
#else
  const char * value = std::getenv(name);
  return (value != nullptr) ? std::string(value) : std::string();
#endif
}

/// fopen without C4996; null on failure. On MSVC through _fsopen with _SH_DENYNO, because
/// fopen_s opens "not sharable": the analyser (or a human with a spreadsheet) could then not
/// read a CSV while RViz is still writing it.
inline std::FILE * open_file(const char * path, const char * mode)
{
#ifdef _MSC_VER
  return _fsopen(path, mode, _SH_DENYNO);
#else
  return std::fopen(path, mode);
#endif
}

}  // namespace timing_detail

/// @brief One CSV file: a fixed set of integer columns, optionally one of them printed
/// as a word from a label table (an enum such as the outcome of a tick).
///
/// Thread-safe: any number of threads may @ref append while the writer flushes.
class TimingSink
{
public:
  /// Rows buffered before further rows are dropped (and counted). 64k rows of a dozen
  /// columns is ~6 MB: minutes of a busy video stream, i.e. a disk stalled far longer
  /// than the stalls under study.
  static constexpr std::size_t kMaxPendingRows = 65536;

  /// @param file_path     File to write (created, with its parent folder, on the first flush).
  /// @param column_names  Header names; every appended row must have exactly this many cells.
  /// @param label_words   Words for @p label_column: a cell value v prints as label_words[v]
  ///                      (the number itself when out of range).
  /// @param label_column  Index into @p column_names, or -1 for none.
  TimingSink(
    std::string file_path, std::vector<std::string> column_names,
    std::vector<std::string> label_words = {}, int label_column = -1)
  : path_(std::move(file_path)),
    columns_(std::move(column_names)),
    labels_(std::move(label_words)),
    label_column_(label_column)
  {
    pending_.reserve(columns_.size() * 256);   // a few seconds of rows: no growth on the hot path
  }

  TimingSink(const TimingSink &) = delete;
  TimingSink & operator=(const TimingSink &) = delete;

  ~TimingSink()
  {
    flush();
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

  /// @brief Queue one row. Hot path: a lock and a copy, nothing else. A row with the wrong
  /// number of cells is ignored (and counted as dropped) rather than corrupting the file.
  void append(std::initializer_list<std::int64_t> cells)
  {
    append(cells.begin(), cells.size());
  }

  void append(const std::int64_t * cells, std::size_t count)
  {
    if (columns_.empty() || count != columns_.size()) {
      rows_dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.size() / columns_.size() >= kMaxPendingRows) {
      rows_dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    pending_.insert(pending_.end(), cells, cells + count);
    rows_appended_.fetch_add(1, std::memory_order_relaxed);
  }

  /// @brief Drain the queue to the file (creating it, with its header, on first use) and
  /// flush the stream. Called by the writer thread about once a second, by
  /// @ref TimingLog::flush_all and by the destructor. Never call it on a GUI thread.
  /// @return rows written by this call.
  std::size_t flush()
  {
    std::vector<std::int64_t> rows;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      rows.swap(pending_);
    }
    std::lock_guard<std::mutex> file_lock(file_mutex_);
    if (!open_locked()) {
      return 0;
    }
    const std::size_t width = columns_.size();
    if (width == 0) {
      return 0;
    }
    const std::size_t row_count = rows.size() / width;
    if (row_count > 0) {
      std::string text;
      text.reserve(row_count * width * 9);
      char number[24];
      for (std::size_t r = 0; r < row_count; ++r) {
        for (std::size_t c = 0; c < width; ++c) {
          if (c > 0) {
            text.push_back(',');
          }
          const std::int64_t value = rows[r * width + c];
          if (value == kTimingBlank) {
            continue;
          }
          if (static_cast<int>(c) == label_column_ && value >= 0 &&
            static_cast<std::size_t>(value) < labels_.size())
          {
            text += labels_[static_cast<std::size_t>(value)];
            continue;
          }
          const auto result = std::to_chars(number, number + sizeof(number), value);
          text.append(number, result.ptr);
        }
        text.push_back('\n');
      }
      std::fwrite(text.data(), 1, text.size(), file_);
      rows_written_.fetch_add(row_count, std::memory_order_relaxed);
    }
    std::fflush(file_);
    // Hand the drained buffer back as the next pending one, so its capacity is reused and
    // append() does not have to grow a fresh vector (on the GUI thread) after every flush.
    rows.clear();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_.empty() && pending_.capacity() < rows.capacity()) {
        pending_.swap(rows);
      }
    }
    return row_count;
  }

  const std::string & path() const { return path_; }
  const std::vector<std::string> & columns() const { return columns_; }
  std::uint64_t rows_appended() const { return rows_appended_.load(std::memory_order_relaxed); }
  std::uint64_t rows_written() const { return rows_written_.load(std::memory_order_relaxed); }
  std::uint64_t rows_dropped() const { return rows_dropped_.load(std::memory_order_relaxed); }

private:
  /// Create the folder and the file, write the header. Caller holds file_mutex_.
  bool open_locked()
  {
    if (file_ != nullptr) {
      return true;
    }
    if (open_failed_) {
      return false;
    }
    std::error_code ec;
    const std::filesystem::path target(path_);
    if (target.has_parent_path()) {
      std::filesystem::create_directories(target.parent_path(), ec);
    }
    file_ = timing_detail::open_file(path_.c_str(), "wb");
    if (file_ == nullptr) {
      open_failed_ = true;
      std::fprintf(stderr, "[qar-timing] cannot create '%s' - its rows are discarded\n",
        path_.c_str());
      std::fflush(stderr);
      return false;
    }
    std::string header;
    for (std::size_t c = 0; c < columns_.size(); ++c) {
      if (c > 0) {
        header.push_back(',');
      }
      header += columns_[c];
    }
    header.push_back('\n');
    std::fwrite(header.data(), 1, header.size(), file_);
    return true;
  }

  const std::string path_;
  const std::vector<std::string> columns_;
  const std::vector<std::string> labels_;
  const int label_column_;

  std::mutex mutex_;                 ///< guards pending_
  std::vector<std::int64_t> pending_;

  std::mutex file_mutex_;            ///< serialises flushes; guards file_ / open_failed_
  std::FILE * file_ = nullptr;
  bool open_failed_ = false;

  std::atomic<std::uint64_t> rows_appended_{0};
  std::atomic<std::uint64_t> rows_written_{0};
  std::atomic<std::uint64_t> rows_dropped_{0};
};

/// @brief The set of timing CSVs of one run, and the thread that writes them.
class TimingLog
{
public:
  /// @param directory      Folder the CSVs go into; empty = disabled (@ref open returns null).
  /// @param flush_interval How often the background thread writes (~1 s in production).
  explicit TimingLog(
    std::string directory,
    std::chrono::milliseconds flush_interval = std::chrono::milliseconds(1000))
  : directory_(std::move(directory)), flush_interval_(flush_interval)
  {
  }

  TimingLog(const TimingLog &) = delete;
  TimingLog & operator=(const TimingLog &) = delete;

  ~TimingLog() { shutdown(); }

  /// @brief The process-wide log, configured once from `QAR_VIDEO_TIMING_DIR`. Created on
  /// first use and intentionally never destroyed - see the file comment, "Lifetime".
  static TimingLog & instance()
  {
    if (TimingLog * replacement = override_slot().load()) {
      return *replacement;
    }
    static TimingLog * const singleton = new TimingLog(directory_from_environment());
    return *singleton;
  }

  /// @brief Test seam: make @ref instance return @p replacement (null restores the
  /// environment-configured one). Returns the previous replacement.
  static TimingLog * override_instance_for_testing(TimingLog * replacement)
  {
    return override_slot().exchange(replacement);
  }

  /// @brief `QAR_VIDEO_TIMING_DIR`, or "" when unset/empty.
  static std::string directory_from_environment()
  {
    return timing_detail::get_env("QAR_VIDEO_TIMING_DIR");
  }

  bool enabled() const { return !directory_.empty(); }
  const std::string & directory() const { return directory_; }

  /// @brief A file-name stem for @p key: anything but letters, digits, '.' and '-' becomes
  /// '_', runs collapse, the ends are trimmed, and the result is capped at 100 characters
  /// ("video:/qar_test_video/image_raw" -> "video_qar_test_video_image_raw").
  static std::string sanitize_name(const std::string & key)
  {
    std::string out;
    out.reserve(key.size());
    for (const char ch : key) {
      const unsigned char u = static_cast<unsigned char>(ch);
      const bool keep = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') ||
        (u >= 'A' && u <= 'Z') || u == '.' || u == '-';
      if (keep) {
        out.push_back(ch);
      } else if (!out.empty() && out.back() != '_') {
        out.push_back('_');
      }
    }
    while (!out.empty() && (out.back() == '_' || out.back() == '.')) {
      out.pop_back();
    }
    if (out.size() > 100) {
      out.resize(100);
    }
    return out.empty() ? std::string("unnamed") : out;
  }

  /// @brief The sink writing `<directory>/<name>.csv`, creating it on first request.
  /// Null when timing is disabled. Asking again for the same @p name with the same columns
  /// returns the same sink; the same name with different columns gets a `_2`, `_3`, ...
  /// suffix. Does no file I/O: the writer thread creates the file on its first flush.
  /// Starts the writer thread if it is not running.
  std::shared_ptr<TimingSink> open(
    const std::string & name, std::vector<std::string> columns,
    std::vector<std::string> labels = {}, int label_column = -1)
  {
    if (!enabled()) {
      return nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::string unique = name;
    for (int n = 2;; ++n) {
      const auto found = sinks_.find(unique);
      if (found == sinks_.end()) {
        break;
      }
      if (found->second->columns() == columns) {
        start_thread_locked();
        return found->second;
      }
      unique = name + "_" + std::to_string(n);
    }
    const std::filesystem::path file = std::filesystem::path(directory_) / (unique + ".csv");
    auto sink = std::make_shared<TimingSink>(
      file.string(), std::move(columns), std::move(labels), label_column);
    sinks_[unique] = sink;
    start_thread_locked();
    return sink;
  }

  /// @brief Write every sink now (the caller's thread does the I/O).
  void flush_all()
  {
    std::vector<std::shared_ptr<TimingSink>> sinks;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sinks.reserve(sinks_.size());
      for (const auto & entry : sinks_) {
        sinks.push_back(entry.second);
      }
    }
    for (const auto & sink : sinks) {
      sink->flush();
    }
  }

  /// @brief Stop the writer thread (joining it) and write everything that is pending.
  /// Idempotent; a later @ref open restarts the thread. Call it from the host's
  /// destructor, never from a static destructor.
  void shutdown()
  {
    std::thread writer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
      shutting_down_ = true;   // an open() racing this must not start a second writer
      writer = std::move(thread_);
    }
    cv_.notify_all();
    if (writer.joinable()) {
      writer.join();
    }
    flush_all();
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = false;
  }

private:
  static std::atomic<TimingLog *> & override_slot()
  {
    static std::atomic<TimingLog *> slot{nullptr};
    return slot;
  }

  void start_thread_locked()
  {
    if (thread_.joinable() || shutting_down_) {
      return;
    }
    stop_ = false;
    thread_ = std::thread([this] { run(); });
  }

  void run()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_) {
      cv_.wait_for(lock, flush_interval_, [this] { return stop_; });
      if (stop_) {
        break;
      }
      lock.unlock();
      flush_all();
      lock.lock();
    }
  }

  const std::string directory_;
  const std::chrono::milliseconds flush_interval_;

  std::mutex mutex_;                 ///< guards sinks_, thread_, stop_
  std::condition_variable cv_;
  std::map<std::string, std::shared_ptr<TimingSink>> sinks_;
  std::thread thread_;
  bool stop_ = false;
  bool shutting_down_ = false;
};

}  // namespace qar_ar_core

#endif  // QAR_AR_CORE__TIMING_LOG_HPP_
