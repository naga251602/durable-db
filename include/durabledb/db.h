#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "durabledb/memtable.h"
#include "durabledb/sstable.h"
#include "durabledb/wal.h"

namespace durabledb {

struct Options {
  size_t memtable_limit = 4 * 1024 * 1024;  // flush when the memtable reaches ~4 MB
  size_t compaction_trigger = 4;            // compact when this many SSTables exist
  bool sync_wal = true;                     // fsync every write (turn off only for benchmarks)
};

// Single-node, single-threaded key-value store (LSM tree).
//
//   Put/Delete -> WAL (fsync) -> memtable -> [flush] -> SSTable -> [compaction]
//   Get        -> memtable -> SSTables newest to oldest
//
// Files in the data directory:
//   LOCK        flock() so two processes can't open the same directory
//   MANIFEST    list of live SSTable ids (the single source of truth)
//   wal.log     write-ahead log for the current memtable
//   NNNNNN.sst  SSTables (higher number = newer)
class DB {
 public:
  // Opens (and recovers) the database. Throws std::runtime_error on failure.
  static std::unique_ptr<DB> Open(const std::string& dir, const Options& opts = {});
  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  // Return true once the change is durable (WAL written, and fsynced if sync_wal).
  bool Put(const std::string& key, const std::string& value);
  bool Delete(const std::string& key);
  std::optional<std::string> Get(const std::string& key);

  void Flush();    // write the memtable to a new SSTable
  void Compact();  // merge all SSTables into one

  size_t NumTables() const { return tables_.size(); }
  size_t MemTableBytes() const { return mem_.ApproxSize(); }

 private:
  DB(const std::string& dir, const Options& opts) : dir_(dir), opts_(opts) {}
  void Recover();
  void MaybeFlush();
  void WriteManifest(const std::vector<uint64_t>& ids);
  std::string SstPath(uint64_t id) const;

  struct Table {
    uint64_t id;
    std::unique_ptr<SSTable> sst;
  };

  std::string dir_;
  Options opts_;
  int lock_fd_ = -1;
  MemTable mem_;
  std::unique_ptr<WAL> wal_;
  std::vector<Table> tables_;  // oldest first
  uint64_t last_id_ = 0;
};

}  // namespace durabledb
