// Copyright 2026 QuaternAR
// SPDX-License-Identifier: MIT
//
// The file that remembers a pairing: its format, the atomic replacement, a damaged file, a path
// with letters outside ASCII, and the folder a program keeps it in. Plain file work.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "qar_qar_adapter/identity_store.hpp"

namespace
{

namespace fs = std::filesystem;
using qar_qar_adapter::IdentityRead;
using qar_qar_adapter::IdentityRecord;

class TempFolder
{
public:
  TempFolder()
  {
    static std::atomic<int> counter{0};
    path_ = fs::temp_directory_path() /
      ("qar_identity_tests_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
      std::to_string(counter.fetch_add(1)));
    fs::create_directories(path_);
  }
  ~TempFolder()
  {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path & path() const { return path_; }
  std::string file(const std::string & name) const
  {
    const auto text = (path_ / fs::u8path(name)).u8string();
    return std::string(text.begin(), text.end());
  }

private:
  fs::path path_;
};

IdentityRecord sample()
{
  IdentityRecord record;
  record.id = "a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf";
  record.host = "hub.example";
  record.port = 19120;
  record.paired_unix_s = 1700000000;
  return record;
}

std::string slurp(const std::string & path)
{
  std::ifstream file(fs::u8path(path), std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

TEST(IdentityStore, ARecordSurvivesAWriteAndARead)
{
  TempFolder folder;
  const std::string path = folder.file("identity.txt");
  std::string error;
  ASSERT_TRUE(qar_qar_adapter::write_identity_atomic(path, sample(), error)) << error;

  IdentityRecord read;
  ASSERT_EQ(qar_qar_adapter::read_identity(path, read, error), IdentityRead::kOk) << error;
  EXPECT_EQ(read.id, sample().id);
  EXPECT_EQ(read.host, "hub.example");
  EXPECT_EQ(read.port, 19120);
  EXPECT_EQ(read.paired_unix_s, 1700000000);
  EXPECT_EQ(slurp(path),
    "format=1\nonboarding_id=a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf\nhub_host=hub.example\n"
    "hub_port=19120\npaired_unix_s=1700000000\n");
  EXPECT_FALSE(fs::exists(fs::u8path(path + ".tmp"))) << "no temporary file is left";
}

TEST(IdentityStore, AMissingFileIsAbsentNotDamaged)
{
  TempFolder folder;
  IdentityRecord read;
  std::string error;
  EXPECT_EQ(qar_qar_adapter::read_identity(folder.file("none.txt"), read, error),
    IdentityRead::kAbsent);
}

TEST(IdentityStore, AFailedWriteKeepsTheFileThatWasThere)
{
  TempFolder folder;
  const std::string path = folder.file("identity.txt");
  std::string error;
  ASSERT_TRUE(qar_qar_adapter::write_identity_atomic(path, sample(), error)) << error;
  const std::string before = slurp(path);

  // A folder where the temporary file belongs makes the write fail.
  fs::create_directories(fs::u8path(path + ".tmp"));
  IdentityRecord other = sample();
  other.host = "other.example";
  EXPECT_FALSE(qar_qar_adapter::write_identity_atomic(path, other, error));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(slurp(path), before);
}

TEST(IdentityStore, ARewriteReplacesTheFileInOneStep)
{
  TempFolder folder;
  const std::string path = folder.file("identity.txt");
  std::string error;
  ASSERT_TRUE(qar_qar_adapter::write_identity_atomic(path, sample(), error)) << error;
  IdentityRecord other = sample();
  other.host = "other.example";
  other.port = 19130;
  ASSERT_TRUE(qar_qar_adapter::write_identity_atomic(path, other, error)) << error;
  IdentityRecord read;
  ASSERT_EQ(qar_qar_adapter::read_identity(path, read, error), IdentityRead::kOk);
  EXPECT_EQ(read.host, "other.example");
  EXPECT_EQ(read.port, 19130);
}

TEST(IdentityStore, DamagedFilesAreReportedWithAReason)
{
  const struct { const char * text; const char * why; } cases[] = {
    {"hello\n", "without '='"},
    {"onboarding_id=x\nhub_host=h\nhub_port=1\n", "no format line"},
    {"format=2\nonboarding_id=x\nhub_host=h\nhub_port=1\n", "unknown format"},
    {"format=1\nhub_host=h\nhub_port=1\n", "no onboarding_id"},
    {"format=1\nonboarding_id=x\nhub_port=1\n", "no hub_host"},
    {"format=1\nonboarding_id=x\nhub_host=h\n", "no hub_host or hub_port"},
    {"format=1\nonboarding_id=x\nhub_host=h\nhub_port=0\n", "not a port"},
    {"format=1\nonboarding_id=x\nhub_host=h\nhub_port=70000\n", "not a port"},
    {"format=1\nonboarding_id=x\nhub_host=h\nhub_port=abc\n", "not a port"},
  };
  for (const auto & test : cases) {
    IdentityRecord read;
    std::string error;
    EXPECT_EQ(qar_qar_adapter::parse_identity(test.text, read, error), IdentityRead::kMalformed)
      << test.text;
    EXPECT_NE(error.find(test.why), std::string::npos) << test.text << " -> " << error;
  }
}

TEST(IdentityStore, UnknownKeysAndWindowsLineEndingsAreTolerated)
{
  IdentityRecord read;
  std::string error;
  ASSERT_EQ(qar_qar_adapter::parse_identity(
      "format=1\r\nonboarding_id=abc\r\nhub_host=h\r\nhub_port=19120\r\nfuture_key=1\r\n", read,
      error), IdentityRead::kOk) << error;
  EXPECT_EQ(read.id, "abc");
  EXPECT_EQ(read.paired_unix_s, 0);
}

TEST(IdentityStore, APathWithLettersOutsideAsciiWorks)
{
  TempFolder folder;
  // "zlutoucky kun" with its Czech letters, as UTF-8 bytes.
  const std::string leaf = "\xC5\xBElu\xC5\xA5ou\xC4\x8Dk\xC3\xBD k\xC5\xAF\xC5\x88";
  const std::string path = folder.file(leaf + "/identity.txt");
  std::string error;
  ASSERT_TRUE(qar_qar_adapter::write_identity_atomic(path, sample(), error)) << error;
  IdentityRecord read;
  ASSERT_EQ(qar_qar_adapter::read_identity(path, read, error), IdentityRead::kOk) << error;
  EXPECT_EQ(read.host, "hub.example");
  EXPECT_TRUE(qar_qar_adapter::remove_identity(path));
}

TEST(IdentityStore, RemovingAFileThatIsAbsentSucceeds)
{
  TempFolder folder;
  const std::string path = folder.file("identity.txt");
  EXPECT_TRUE(qar_qar_adapter::remove_identity(path));
  std::string error;
  ASSERT_TRUE(qar_qar_adapter::write_identity_atomic(path, sample(), error));
  EXPECT_TRUE(qar_qar_adapter::remove_identity(path));
  EXPECT_FALSE(fs::exists(fs::u8path(path)));
}

void set_environment(const char * name, const std::string & value)
{
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

TEST(IdentityStore, TheFolderOfAProgramIsNamedByItsTagAndInstance)
{
  TempFolder base;
  set_environment("QAR_PANEL_IDENTITY_DIR", base.file("base"));
  const std::string plain = qar_qar_adapter::default_identity_dir("rviz2-plugin", "");
  const std::string second = qar_qar_adapter::default_identity_dir("rviz2-plugin", "lab 2");
  const std::string odd = qar_qar_adapter::default_identity_dir("a/b c", "");
  set_environment("QAR_PANEL_IDENTITY_DIR", "");

  auto under_base = [&base](const std::string & leaf) {
      const auto text = (fs::u8path(base.file("base")) / leaf).u8string();
      return std::string(text.begin(), text.end());
    };
  EXPECT_EQ(plain, under_base("rviz2-plugin"));
  EXPECT_EQ(second, under_base("rviz2-plugin.lab-2"));
  EXPECT_EQ(odd, under_base("a-b-c"));
  const std::string file = qar_qar_adapter::identity_file_path(plain);
  EXPECT_EQ(file.rfind("identity.txt"), file.size() - 12);
  EXPECT_EQ(file.find(plain), 0u);
}

TEST(IdentityStore, WithoutAnOverrideTheFolderLiesUnderTheUsersApplicationData)
{
  set_environment("QAR_PANEL_IDENTITY_DIR", "");
  const std::string plain = qar_qar_adapter::default_identity_dir("rviz2-plugin", "");
  EXPECT_FALSE(plain.empty());
  EXPECT_NE(plain.find("QarOS-ROS"), std::string::npos);
  EXPECT_EQ(plain.rfind("rviz2-plugin"), plain.size() - 12);
}

}  // namespace
