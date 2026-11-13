#include <gtest/gtest.h>

#include "durabledb/crc32.h"
#include "durabledb/memtable.h"

using namespace durabledb;

TEST(Crc32, KnownVector) {
  // standard CRC32 check value
  EXPECT_EQ(crc32("123456789", 9), 0xCBF43926u);
  EXPECT_EQ(crc32("", 0), 0u);
}

TEST(Crc32, DetectsSingleBitFlip) {
  std::string a = "hello durable world";
  std::string b = a;
  b[5] ^= 0x01;
  EXPECT_NE(crc32(a.data(), a.size()), crc32(b.data(), b.size()));
}

TEST(MemTable, PutGetOverwrite) {
  MemTable m;
  Entry e;
  EXPECT_FALSE(m.Lookup("a", &e));
  m.Put("a", "1");
  ASSERT_TRUE(m.Lookup("a", &e));
  EXPECT_FALSE(e.tombstone);
  EXPECT_EQ(e.value, "1");
  m.Put("a", "2");
  ASSERT_TRUE(m.Lookup("a", &e));
  EXPECT_EQ(e.value, "2");
}

TEST(MemTable, DeleteLeavesTombstone) {
  MemTable m;
  m.Put("a", "1");
  m.Delete("a");
  Entry e;
  ASSERT_TRUE(m.Lookup("a", &e));
  EXPECT_TRUE(e.tombstone);
  // deleting a key that was never written must also be remembered
  m.Delete("never_existed");
  ASSERT_TRUE(m.Lookup("never_existed", &e));
  EXPECT_TRUE(e.tombstone);
}

TEST(MemTable, SizeTracking) {
  MemTable m;
  EXPECT_EQ(m.ApproxSize(), 0u);
  m.Put("key", "value");           // 3 + 5
  EXPECT_EQ(m.ApproxSize(), 8u);
  m.Put("key", "v");               // overwrite: only the value size changes
  EXPECT_EQ(m.ApproxSize(), 4u);
  m.Clear();
  EXPECT_EQ(m.ApproxSize(), 0u);
  EXPECT_TRUE(m.Empty());
}
