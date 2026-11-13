#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "durabledb/memtable.h"

namespace durabledb {

// Immutable sorted file on disk.
//
//   [ data section ]   records sorted by key:
//                      | type (1) | key_len (4) | val_len (4) | key | value |
//   [ index section ]  one entry per 16 records (sparse index):
//                      | key_len (4) | key | offset (8) |
//   [ footer (24 B) ]  | index_offset (8) | index_count (4) | crc32 (4) | magic (8) |
//
// The crc32 covers everything before the footer, so a damaged file is detected
// when it is opened.
class SSTable {
 public:
  // Writes the table crash-safely (tmp file, fsync, rename, directory fsync).
  static bool Write(const std::string& path, const std::map<std::string, Entry>& data);

  // Returns nullptr if the file is missing, short, or fails its checksum.
  static std::unique_ptr<SSTable> Open(const std::string& path);

  ~SSTable();
  SSTable(const SSTable&) = delete;
  SSTable& operator=(const SSTable&) = delete;

  // true if the key is in this table (as a value or a tombstone).
  bool Get(const std::string& key, Entry* out) const;

  // Load every record (used by compaction).
  bool ReadAll(std::map<std::string, Entry>* out) const;

  const std::string& path() const { return path_; }

 private:
  SSTable() = default;
  struct IndexEntry {
    std::string key;
    uint64_t offset;
  };
  std::string path_;
  int fd_ = -1;
  uint64_t data_end_ = 0;
  std::vector<IndexEntry> index_;
};

}  // namespace durabledb
