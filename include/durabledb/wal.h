#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace durabledb {

enum class RecType : uint8_t { PUT = 1, DELETE = 2 };

// Write-ahead log. Every change is appended here (and fsynced) BEFORE it is
// applied to the memtable, so a crash can always be repaired by replaying it.
//
// Record layout (all integers little-endian):
//   | crc32 (4) | payload_len (4) | payload |
//   payload = | type (1) | key_len (4) | val_len (4) | key | value |
// The CRC covers payload_len and payload.
class WAL {
 public:
  WAL(const std::string& path, bool sync_on_write);
  ~WAL();
  WAL(const WAL&) = delete;
  WAL& operator=(const WAL&) = delete;

  bool ok() const { return fd_ >= 0; }

  // Returns true only when the record is fully written (and fsynced if
  // sync_on_write). On failure the file is cut back to its previous length so a
  // half-written record can never sit in front of later records.
  bool Append(RecType type, const std::string& key, const std::string& value);

  // Empty the log (after the memtable has been safely flushed to an SSTable).
  bool Reset();

  uint64_t size() const { return offset_; }

  using ReplayFn = std::function<void(RecType, const std::string&, const std::string&)>;

  // Calls fn for every valid record, in order. Stops at the first torn or
  // corrupted record and truncates the file there. Returns records replayed.
  static size_t Replay(const std::string& path, const ReplayFn& fn);

 private:
  std::string path_;
  int fd_ = -1;
  uint64_t offset_ = 0;
  bool sync_;
};

}  // namespace durabledb
