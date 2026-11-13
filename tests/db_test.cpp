#include <gtest/gtest.h>

#include <map>
#include <random>

#include "durabledb/db.h"
#include "durabledb/fault.h"
#include "test_util.h"

using namespace durabledb;

static Options Small() {
  Options o;
  o.memtable_limit = 4096;  // tiny memtable -> lots of flushes
  o.sync_wal = false;       // speed; durability is covered by the crash tests
  return o;
}

TEST(DB, PutGetDelete) {
  TempDir d;
  auto db = DB::Open(d.path());
  EXPECT_FALSE(db->Get("a"));
  ASSERT_TRUE(db->Put("a", "1"));
  EXPECT_EQ(db->Get("a"), "1");
  ASSERT_TRUE(db->Put("a", "2"));
  EXPECT_EQ(db->Get("a"), "2");
  ASSERT_TRUE(db->Delete("a"));
  EXPECT_FALSE(db->Get("a"));
  ASSERT_TRUE(db->Delete("never_existed"));
  EXPECT_FALSE(db->Get("never_existed"));
}

TEST(DB, EmptyKeyAndLargeValue) {
  TempDir d;
  auto db = DB::Open(d.path(), Small());
  std::string big(300000, 'z');
  ASSERT_TRUE(db->Put("", "empty key"));
  ASSERT_TRUE(db->Put("big", big));
  db.reset();
  db = DB::Open(d.path(), Small());
  EXPECT_EQ(db->Get(""), "empty key");
  EXPECT_EQ(db->Get("big"), big);
}

// "Crash" = destroy the object without Flush(). Only the WAL has the data.
TEST(Recovery, UnflushedWritesComeBackFromWal) {
  TempDir d;
  {
    auto db = DB::Open(d.path());
    for (int i = 0; i < 100; i++) ASSERT_TRUE(db->Put("k" + std::to_string(i), "v" + std::to_string(i)));
    ASSERT_TRUE(db->Delete("k50"));
    EXPECT_EQ(db->NumTables(), 0u);  // nothing flushed
  }
  auto db = DB::Open(d.path());
  for (int i = 0; i < 100; i++) {
    if (i == 50) EXPECT_FALSE(db->Get("k50"));
    else EXPECT_EQ(db->Get("k" + std::to_string(i)), "v" + std::to_string(i));
  }
}

TEST(Recovery, FlushedPlusWalData) {
  TempDir d;
  {
    auto db = DB::Open(d.path());
    db->Put("old", "1");
    db->Flush();
    db->Put("new", "2");
    db->Delete("old");  // tombstone only in the WAL, value only in the SSTable
  }
  auto db = DB::Open(d.path());
  EXPECT_EQ(db->NumTables(), 1u);
  EXPECT_FALSE(db->Get("old"));
  EXPECT_EQ(db->Get("new"), "2");
}

TEST(Recovery, TornWalTailIsDropped) {
  TempDir d;
  {
    auto db = DB::Open(d.path());
    db->Put("a", "1");
    db->Put("b", "2");
  }
  // simulate a crash in the middle of the last record
  std::string wal = ReadAll(d.file("wal.log"));
  WriteAll(d.file("wal.log"), wal.substr(0, wal.size() - 3));
  auto db = DB::Open(d.path());
  EXPECT_EQ(db->Get("a"), "1");
  EXPECT_FALSE(db->Get("b"));  // was never fully written, so never acknowledged
  // and the database keeps working normally afterwards
  ASSERT_TRUE(db->Put("c", "3"));
  db.reset();
  db = DB::Open(d.path());
  EXPECT_EQ(db->Get("c"), "3");
}

// Cut the WAL at every byte. The database must always open and must hold a
// PREFIX of the writes: if write i survived, every write before i survived too.
TEST(Recovery, WalCutAtEveryOffsetGivesAPrefix) {
  TempDir src;
  const int N = 12;
  {
    auto db = DB::Open(src.path());
    for (int i = 0; i < N; i++) db->Put("key" + std::to_string(i), "val" + std::to_string(i));
  }
  std::string wal = ReadAll(src.file("wal.log"));

  for (size_t cut = 0; cut <= wal.size(); cut++) {
    TempDir d;
    WriteAll(d.file("wal.log"), wal.substr(0, cut));
    auto db = DB::Open(d.path());
    bool gap = false;
    for (int i = 0; i < N; i++) {
      auto v = db->Get("key" + std::to_string(i));
      if (v) {
        ASSERT_FALSE(gap) << "write " << i << " survived but an earlier one did not (cut " << cut << ")";
        ASSERT_EQ(*v, "val" + std::to_string(i));
      } else {
        gap = true;
      }
    }
  }
}

TEST(Flush, MemtableLimitTriggersFlush) {
  TempDir d;
  Options o = Small();
  o.compaction_trigger = 1000;
  auto db = DB::Open(d.path(), o);
  for (int i = 0; i < 500; i++) db->Put("key" + std::to_string(i), std::string(50, 'v'));
  EXPECT_GT(db->NumTables(), 1u);
  for (int i = 0; i < 500; i++) ASSERT_EQ(db->Get("key" + std::to_string(i)), std::string(50, 'v'));
  // after a flush the WAL only holds what is still in the memtable
  EXPECT_LT(ReadAll(d.file("wal.log")).size(), 4096u + 200);
}

TEST(Compaction, MergesTablesAndKeepsNewestValue) {
  TempDir d;
  Options o = Small();
  o.compaction_trigger = 1000;
  auto db = DB::Open(d.path(), o);
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 40; i++) db->Put("key" + std::to_string(i), "round" + std::to_string(round));
    db->Flush();
  }
  ASSERT_GE(db->NumTables(), 3u);
  db->Compact();
  EXPECT_EQ(db->NumTables(), 1u);
  for (int i = 0; i < 40; i++) EXPECT_EQ(db->Get("key" + std::to_string(i)), "round2");
}

TEST(Compaction, DeletedKeysStayDeletedAndTombstonesAreDropped) {
  TempDir d;
  Options o = Small();
  o.compaction_trigger = 1000;
  auto db = DB::Open(d.path(), o);
  for (int i = 0; i < 100; i++) db->Put("key" + std::to_string(i), "v");
  db->Flush();
  for (int i = 0; i < 100; i += 2) db->Delete("key" + std::to_string(i));
  db->Flush();
  db->Compact();
  ASSERT_EQ(db->NumTables(), 1u);
  db.reset();

  // reopen: the deleted keys must not come back
  db = DB::Open(d.path(), o);
  for (int i = 0; i < 100; i++) {
    if (i % 2 == 0) EXPECT_FALSE(db->Get("key" + std::to_string(i))) << i;
    else EXPECT_EQ(db->Get("key" + std::to_string(i)), "v") << i;
  }
  // tombstones were removed from the merged file: it is smaller than the
  // two inputs together would have been, and contains only the 50 live keys
  size_t total = 0;
  for (auto& n : ListDir(d.path()))
    if (n.size() > 4 && n.substr(n.size() - 4) == ".sst") total += ReadAll(d.file(n)).size();
  EXPECT_LT(total, 100u * 40);
}

TEST(Compaction, EverythingDeletedLeavesNoTables) {
  TempDir d;
  Options o = Small();
  o.compaction_trigger = 1000;
  auto db = DB::Open(d.path(), o);
  db->Put("a", "1");
  db->Flush();
  db->Delete("a");
  db->Flush();
  db->Compact();
  EXPECT_EQ(db->NumTables(), 0u);
  EXPECT_FALSE(db->Get("a"));
  db.reset();
  db = DB::Open(d.path(), o);
  EXPECT_FALSE(db->Get("a"));
}

TEST(Compaction, AutomaticWhenTriggerReached) {
  TempDir d;
  Options o = Small();
  o.compaction_trigger = 4;
  auto db = DB::Open(d.path(), o);
  for (int i = 0; i < 3000; i++) db->Put("key" + std::to_string(i % 400), "v" + std::to_string(i));
  EXPECT_LT(db->NumTables(), 4u);  // compaction keeps the table count down
}

// ---- simulated crashes in the middle of flush / compaction ----

TEST(CrashSafety, OrphanSstAndTmpFilesAreCleanedUp) {
  TempDir d;
  {
    auto db = DB::Open(d.path());
    db->Put("a", "1");
    db->Flush();
  }
  // leftovers from interrupted operations
  WriteAll(d.file("000099.sst"), "garbage that is not in the MANIFEST");
  WriteAll(d.file("000098.sst.tmp"), "half written");
  WriteAll(d.file("MANIFEST.tmp"), "half written");
  auto db = DB::Open(d.path());
  EXPECT_FALSE(Exists(d.file("000099.sst")));
  EXPECT_FALSE(Exists(d.file("000098.sst.tmp")));
  EXPECT_FALSE(Exists(d.file("MANIFEST.tmp")));
  EXPECT_EQ(db->Get("a"), "1");
}

// The dangerous compaction crash: merged table already written, old tables not
// deleted yet. If the engine listed the directory to find tables, the old
// tables (which still hold the value of a deleted key) would be read again and
// the key would come back from the dead. The MANIFEST prevents that.
TEST(CrashSafety, CrashAfterCompactionWriteBeforeCleanup) {
  TempDir d;
  Options o = Small();
  o.compaction_trigger = 1000;
  std::string saved_old1, saved_old2;
  {
    auto db = DB::Open(d.path(), o);
    db->Put("victim", "alive");
    db->Put("other", "x");
    db->Flush();                  // 000001.sst has the value
    db->Delete("victim");
    db->Flush();                  // 000002.sst has the tombstone
    saved_old1 = ReadAll(d.file("000001.sst"));
    saved_old2 = ReadAll(d.file("000002.sst"));
    db->Compact();                // 000003.sst, tombstone dropped, old files deleted
  }
  // put the old files back: exactly the state if we died before deleting them
  WriteAll(d.file("000001.sst"), saved_old1);
  WriteAll(d.file("000002.sst"), saved_old2);

  auto db = DB::Open(d.path(), o);
  EXPECT_FALSE(db->Get("victim")) << "deleted key came back after a compaction crash";
  EXPECT_EQ(db->Get("other"), "x");
  EXPECT_EQ(db->NumTables(), 1u);
  EXPECT_FALSE(Exists(d.file("000001.sst")));
}

TEST(CrashSafety, CrashAfterFlushBeforeWalReset) {
  TempDir d;
  std::string wal_before_flush;
  {
    auto db = DB::Open(d.path());
    db->Put("a", "1");
    db->Put("b", "2");
    db->Delete("a");
    wal_before_flush = ReadAll(d.file("wal.log"));
    db->Flush();  // SSTable + MANIFEST written, WAL reset
  }
  // restore the old WAL: the same records are now in the SSTable AND the WAL
  WriteAll(d.file("wal.log"), wal_before_flush);
  auto db = DB::Open(d.path());
  EXPECT_FALSE(db->Get("a"));
  EXPECT_EQ(db->Get("b"), "2");
}

TEST(CrashSafety, CorruptedSstableIsReportedNotIgnored) {
  TempDir d;
  {
    auto db = DB::Open(d.path());
    db->Put("a", "1");
    db->Flush();
  }
  std::string sst = ReadAll(d.file("000001.sst"));
  sst[3] ^= 0x01;
  WriteAll(d.file("000001.sst"), sst);
  EXPECT_THROW(DB::Open(d.path()), std::runtime_error);
}

TEST(CrashSafety, SecondInstanceIsRejected) {
  TempDir d;
  auto db = DB::Open(d.path());
  EXPECT_THROW(DB::Open(d.path()), std::runtime_error);
  db.reset();
  EXPECT_NO_THROW(DB::Open(d.path()));
}

// ---- I/O errors ----

TEST(Faults, FailedPutIsNotVisibleAndDbKeepsWorking) {
  TempDir d;
  auto db = DB::Open(d.path());
  ASSERT_TRUE(db->Put("a", "1"));
  fault::fail_wal_append_after() = 0;
  EXPECT_FALSE(db->Put("b", "2"));       // not acknowledged
  EXPECT_FALSE(db->Get("b"));            // and not applied
  ASSERT_TRUE(db->Put("c", "3"));        // later writes still work
  db.reset();
  db = DB::Open(d.path());
  EXPECT_EQ(db->Get("a"), "1");
  EXPECT_FALSE(db->Get("b"));
  EXPECT_EQ(db->Get("c"), "3");
}

// ---- randomized comparison against std::map ----

class RandomVsMap : public ::testing::TestWithParam<unsigned> {};

TEST_P(RandomVsMap, MatchesStdMapAcrossRestarts) {
  TempDir d;
  Options o = Small();
  o.compaction_trigger = 4;
  auto db = DB::Open(d.path(), o);
  std::map<std::string, std::string> ref;
  std::mt19937 rng(GetParam());

  for (int i = 0; i < 40000; i++) {
    std::string k = "k" + std::to_string(rng() % 300);
    int op = rng() % 20;
    if (op < 11) {
      std::string v = "v" + std::to_string(rng());
      ASSERT_TRUE(db->Put(k, v));
      ref[k] = v;
    } else if (op < 15) {
      ASSERT_TRUE(db->Delete(k));
      ref.erase(k);
    } else if (op < 19) {
      auto got = db->Get(k);
      auto it = ref.find(k);
      if (it == ref.end()) ASSERT_FALSE(got.has_value()) << "op " << i << " key " << k;
      else {
        ASSERT_TRUE(got.has_value()) << "op " << i << " key " << k;
        ASSERT_EQ(*got, it->second) << "op " << i << " key " << k;
      }
    } else if (rng() % 4 == 0) {
      // simulate a restart (a crash: no Flush)
      db.reset();
      db = DB::Open(d.path(), o);
    } else if (rng() % 8 == 0) {
      db->Compact();
    }
  }

  // final full comparison, before and after one more restart
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < 300; i++) {
      std::string k = "k" + std::to_string(i);
      auto got = db->Get(k);
      auto it = ref.find(k);
      if (it == ref.end()) ASSERT_FALSE(got.has_value()) << k;
      else { ASSERT_TRUE(got.has_value()) << k; ASSERT_EQ(*got, it->second) << k; }
    }
    db.reset();
    db = DB::Open(d.path(), o);
  }
}

INSTANTIATE_TEST_SUITE_P(Seeds, RandomVsMap, ::testing::Values(1u, 2u, 3u, 42u, 2024u));
