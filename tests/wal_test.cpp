#include <gtest/gtest.h>

#include <tuple>
#include <vector>

#include "durabledb/fault.h"
#include "durabledb/wal.h"
#include "test_util.h"

using namespace durabledb;
using Rec = std::tuple<RecType, std::string, std::string>;

static std::vector<Rec> ReplayAll(const std::string& path) {
  std::vector<Rec> out;
  WAL::Replay(path, [&](RecType t, const std::string& k, const std::string& v) { out.emplace_back(t, k, v); });
  return out;
}

TEST(Wal, AppendThenReplay) {
  TempDir d;
  {
    WAL w(d.file("wal.log"), true);
    ASSERT_TRUE(w.ok());
    ASSERT_TRUE(w.Append(RecType::PUT, "a", "1"));
    ASSERT_TRUE(w.Append(RecType::PUT, "b", "2"));
    ASSERT_TRUE(w.Append(RecType::DELETE, "a", ""));
  }
  auto recs = ReplayAll(d.file("wal.log"));
  ASSERT_EQ(recs.size(), 3u);
  EXPECT_EQ(recs[0], Rec(RecType::PUT, "a", "1"));
  EXPECT_EQ(recs[1], Rec(RecType::PUT, "b", "2"));
  EXPECT_EQ(recs[2], Rec(RecType::DELETE, "a", ""));
}

TEST(Wal, EmptyAndMissingFile) {
  TempDir d;
  EXPECT_EQ(ReplayAll(d.file("nope.log")).size(), 0u);
  WriteAll(d.file("empty.log"), "");
  EXPECT_EQ(ReplayAll(d.file("empty.log")).size(), 0u);
}

TEST(Wal, BinaryKeysAndValues) {
  TempDir d;
  std::string k("k\0ey", 4), v("\0\xff\x01 v", 5);
  {
    WAL w(d.file("wal.log"), false);
    ASSERT_TRUE(w.Append(RecType::PUT, k, v));
    ASSERT_TRUE(w.Append(RecType::PUT, "empty_value", ""));
  }
  auto recs = ReplayAll(d.file("wal.log"));
  ASSERT_EQ(recs.size(), 2u);
  EXPECT_EQ(std::get<1>(recs[0]), k);
  EXPECT_EQ(std::get<2>(recs[0]), v);
  EXPECT_EQ(std::get<2>(recs[1]), "");
}

// A crash can cut the file at ANY byte. For every possible cut point, replay
// must return exactly the complete records before the cut, and nothing else.
TEST(Wal, TornTailAtEveryByteOffset) {
  TempDir d;
  std::vector<uint64_t> boundaries{0};
  {
    WAL w(d.file("wal.log"), false);
    for (int i = 0; i < 6; i++) {
      ASSERT_TRUE(w.Append(RecType::PUT, "key" + std::to_string(i), std::string(i * 7, 'v')));
      boundaries.push_back(w.size());
    }
  }
  std::string full = ReadAll(d.file("wal.log"));
  ASSERT_EQ(full.size(), boundaries.back());

  for (size_t cut = 0; cut <= full.size(); cut++) {
    std::string path = d.file("cut.log");
    WriteAll(path, full.substr(0, cut));
    auto recs = ReplayAll(path);

    size_t expected = 0;
    while (expected + 1 < boundaries.size() && boundaries[expected + 1] <= cut) expected++;
    ASSERT_EQ(recs.size(), expected) << "cut at byte " << cut;
    for (size_t i = 0; i < recs.size(); i++)
      ASSERT_EQ(std::get<1>(recs[i]), "key" + std::to_string(i));
    // the bad tail was removed from the file
    ASSERT_EQ(ReadAll(path).size(), boundaries[expected]) << "cut at byte " << cut;
  }
}

TEST(Wal, CorruptedByteStopsReplay) {
  TempDir d;
  std::vector<uint64_t> boundaries{0};
  {
    WAL w(d.file("wal.log"), false);
    for (int i = 0; i < 5; i++) {
      ASSERT_TRUE(w.Append(RecType::PUT, "key" + std::to_string(i), "value" + std::to_string(i)));
      boundaries.push_back(w.size());
    }
  }
  std::string data = ReadAll(d.file("wal.log"));
  // flip a bit inside the 3rd record (index 2)
  size_t victim = boundaries[2] + 12;
  data[victim] ^= 0x10;
  WriteAll(d.file("wal.log"), data);

  auto recs = ReplayAll(d.file("wal.log"));
  EXPECT_EQ(recs.size(), 2u);  // records 0 and 1 are fine, replay stops at 2
  EXPECT_EQ(ReadAll(d.file("wal.log")).size(), boundaries[2]);
}

TEST(Wal, ResetEmptiesLog) {
  TempDir d;
  WAL w(d.file("wal.log"), true);
  ASSERT_TRUE(w.Append(RecType::PUT, "a", "1"));
  ASSERT_TRUE(w.Reset());
  EXPECT_EQ(w.size(), 0u);
  ASSERT_TRUE(w.Append(RecType::PUT, "b", "2"));
  auto recs = ReplayAll(d.file("wal.log"));
  ASSERT_EQ(recs.size(), 1u);
  EXPECT_EQ(std::get<1>(recs[0]), "b");
}

// If an append fails half way, the half record must be removed. Otherwise the
// next (successful) append would sit behind garbage and replay would never
// reach it, silently losing an acknowledged write.
TEST(Wal, FailedAppendLeavesNoGarbageBehind) {
  TempDir d;
  WAL w(d.file("wal.log"), true);
  ASSERT_TRUE(w.Append(RecType::PUT, "a", "1"));
  fault::fail_wal_append_after() = 0;
  EXPECT_FALSE(w.Append(RecType::PUT, "b", std::string(100, 'x')));
  ASSERT_TRUE(w.Append(RecType::PUT, "c", "3"));

  auto recs = ReplayAll(d.file("wal.log"));
  ASSERT_EQ(recs.size(), 2u);
  EXPECT_EQ(std::get<1>(recs[0]), "a");
  EXPECT_EQ(std::get<1>(recs[1]), "c");
}
