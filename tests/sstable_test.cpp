#include <gtest/gtest.h>

#include "durabledb/sstable.h"
#include "test_util.h"

using namespace durabledb;

static std::map<std::string, Entry> MakeData(int n) {
  std::map<std::string, Entry> m;
  for (int i = 0; i < n; i++) {
    char key[16];
    std::snprintf(key, sizeof(key), "key%05d", i);
    m[key] = Entry{i % 7 == 0, i % 7 == 0 ? "" : "value" + std::to_string(i)};
  }
  return m;
}

TEST(SSTable, WriteOpenGetEveryKey) {
  TempDir d;
  auto data = MakeData(1000);  // many index blocks
  ASSERT_TRUE(SSTable::Write(d.file("a.sst"), data));
  auto t = SSTable::Open(d.file("a.sst"));
  ASSERT_NE(t, nullptr);
  for (const auto& [k, expect] : data) {
    Entry got;
    ASSERT_TRUE(t->Get(k, &got)) << k;
    EXPECT_EQ(got.tombstone, expect.tombstone) << k;
    EXPECT_EQ(got.value, expect.value) << k;
  }
}

TEST(SSTable, MissingKeys) {
  TempDir d;
  ASSERT_TRUE(SSTable::Write(d.file("a.sst"), MakeData(100)));
  auto t = SSTable::Open(d.file("a.sst"));
  ASSERT_NE(t, nullptr);
  Entry e;
  EXPECT_FALSE(t->Get("", &e));            // before every key
  EXPECT_FALSE(t->Get("aaa", &e));         // before the first key
  EXPECT_FALSE(t->Get("key00005x", &e));   // between two keys
  EXPECT_FALSE(t->Get("zzz", &e));         // after every key
}

TEST(SSTable, SingleRecordAndEmpty) {
  TempDir d;
  std::map<std::string, Entry> one{{"only", Entry{false, "v"}}};
  ASSERT_TRUE(SSTable::Write(d.file("one.sst"), one));
  auto t = SSTable::Open(d.file("one.sst"));
  ASSERT_NE(t, nullptr);
  Entry e;
  ASSERT_TRUE(t->Get("only", &e));
  EXPECT_EQ(e.value, "v");

  ASSERT_TRUE(SSTable::Write(d.file("empty.sst"), {}));
  auto te = SSTable::Open(d.file("empty.sst"));
  ASSERT_NE(te, nullptr);
  EXPECT_FALSE(te->Get("x", &e));
}

TEST(SSTable, ReadAllRoundTrip) {
  TempDir d;
  auto data = MakeData(333);
  ASSERT_TRUE(SSTable::Write(d.file("a.sst"), data));
  auto t = SSTable::Open(d.file("a.sst"));
  ASSERT_NE(t, nullptr);
  std::map<std::string, Entry> back;
  ASSERT_TRUE(t->ReadAll(&back));
  ASSERT_EQ(back.size(), data.size());
  for (const auto& [k, e] : data) {
    EXPECT_EQ(back[k].tombstone, e.tombstone);
    EXPECT_EQ(back[k].value, e.value);
  }
}

TEST(SSTable, NoLeftoverTmpFile) {
  TempDir d;
  ASSERT_TRUE(SSTable::Write(d.file("a.sst"), MakeData(10)));
  EXPECT_TRUE(Exists(d.file("a.sst")));
  EXPECT_FALSE(Exists(d.file("a.sst.tmp")));
}

TEST(SSTable, DetectsCorruption) {
  TempDir d;
  ASSERT_TRUE(SSTable::Write(d.file("a.sst"), MakeData(200)));
  std::string good = ReadAll(d.file("a.sst"));

  // flip one bit in the data section
  std::string bad = good;
  bad[good.size() / 3] ^= 0x04;
  WriteAll(d.file("bad.sst"), bad);
  EXPECT_EQ(SSTable::Open(d.file("bad.sst")), nullptr);

  // truncated file
  WriteAll(d.file("short.sst"), good.substr(0, good.size() - 5));
  EXPECT_EQ(SSTable::Open(d.file("short.sst")), nullptr);

  // too small to hold a footer
  WriteAll(d.file("tiny.sst"), "abc");
  EXPECT_EQ(SSTable::Open(d.file("tiny.sst")), nullptr);

  // missing file
  EXPECT_EQ(SSTable::Open(d.file("nope.sst")), nullptr);
}
