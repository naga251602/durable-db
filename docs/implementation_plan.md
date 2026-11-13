> **Note:** this is the original plan, kept for reference. The finished project follows it
> closely, with these changes, all made after thinking through crash cases:
>
> - **MANIFEST file** added. The plan found SSTables by listing the directory, but that lets a
>   deleted key come back to life after a crash during compaction (see README).
> - **SSTable footer has a CRC32** over the whole file, checked on open.
> - **Failed WAL appends are rolled back** (file truncated to its previous length).
> - **`flock` on a `LOCK` file** stops two processes using one directory.
> - **Crash points** (`fault.h`) were added so the crash test can kill the engine at every risky
>   step, not just at random times.
> - Code below is the early skeleton. The real code is in `src/` and `include/`.

# Durable DB | Implementation Plan

A hand-built key-value storage engine in C++ on Linux. It covers persistence, write-ahead logging (WAL), SSTables, compaction, and crash recovery. Everything is written from scratch, no existing database.

## 1. Problem and solution

**Problem:** Using a database teaches you its API, not how durability works. WAL, crash recovery, and on-disk layout stay a black box, so performance and correctness claims can't be explained from first principles.

**Solution:** Build the storage layer myself so every durability and performance tradeoff is implemented, tested, and explainable.

## 2. Scope

Single-node key-value store with three operations: `Put`, `Get`, `Delete`.

**In scope:** WAL, memtable, SSTables, simple compaction, crash recovery, tests, benchmarks.

**Out of scope (future work in README):** SQL, transactions, multi-threaded writers, replication, networking, compression, bloom filters.

## 3. Tech stack

| Area | Choice |
|---|---|
| Language | C++17 |
| OS | Linux (Ubuntu, or WSL2 / VM) |
| Build | CMake + Ninja |
| File I/O | POSIX (`open`, `write`, `fsync`, `pread`, `rename`) |
| Memtable | `std::map` (skip list is an optional upgrade) |
| Checksums | CRC32 |
| Testing | GoogleTest |
| Benchmarks | Google Benchmark + custom load generator |
| Debugging | gdb, valgrind, AddressSanitizer |
| Profiling | `perf`, `strace` |
| CI | GitHub Actions |

## 4. Repo layout

```
durable-db/
  CMakeLists.txt
  include/durabledb/
    db.h
    wal.h
    memtable.h
    sstable.h
    crc32.h
  src/
    db.cpp
    wal.cpp
    memtable.cpp
    sstable.cpp
  tests/
    wal_test.cpp
    memtable_test.cpp
    sstable_test.cpp
    recovery_test.cpp
    random_vs_map_test.cpp
  bench/
    bench_main.cpp
  tools/
    crash_writer.cpp
    crash_verify.cpp
    crash_test.sh
  .github/workflows/ci.yml
  README.md
```

## 5. Architecture

1. **Write path:** append record to WAL, `fsync`, then insert into memtable.
2. **Flush:** when memtable reaches the size limit (4 MB), write it as a sorted SSTable, `fsync`, rename into place, then start a fresh WAL.
3. **Read path:** memtable first, then SSTables from newest to oldest.
4. **Compaction:** when there are 4 or more SSTables, merge them into one. Newest value wins, tombstones get dropped.
5. **Recovery:** on startup, load SSTables, then replay the WAL into the memtable. Stop at the first torn or corrupted record (checksum mismatch) and truncate the WAL there.

### On-disk files

```
data_dir/
  wal.log            current write-ahead log
  000001.sst         SSTables, higher number = newer
  000002.sst
  *.tmp              in-progress files, deleted on startup
```

### WAL record format

```
| crc32 (4) | payload_len (4) | payload (payload_len bytes) |

payload = | type (1) | key_len (4) | val_len (4) | key | value |
type: 1 = PUT, 2 = DELETE   (DELETE has val_len = 0)
```

The CRC covers `payload_len` and `payload`. All integers are little-endian.

### SSTable format

```
[ data section ]  records sorted by key:
                  | type (1) | key_len (4) | val_len (4) | key | value |
[ index section ] one entry every 16 records:
                  | key_len (4) | key | offset (8) |
[ footer (20 B) ] | index_offset (8) | index_count (4) | magic (8) |
```

## 6. Implementation

The code below is a starting skeleton. It is meant to compile and be easy to read, not to be perfect. Fill in error handling as you go.

### 6.1 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(durable_db CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_ASAN "Build with AddressSanitizer" OFF)
if(ENABLE_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_compile_options(-Wall -Wextra -O2 -g)

add_library(durabledb
  src/db.cpp src/wal.cpp src/memtable.cpp src/sstable.cpp)
target_include_directories(durabledb PUBLIC include)

# Tests
include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
FetchContent_MakeAvailable(googletest)
enable_testing()

file(GLOB TEST_SRCS tests/*.cpp)
add_executable(unit_tests ${TEST_SRCS})
target_link_libraries(unit_tests durabledb GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(unit_tests)

# Crash tools
add_executable(crash_writer tools/crash_writer.cpp)
target_link_libraries(crash_writer durabledb)
add_executable(crash_verify tools/crash_verify.cpp)
target_link_libraries(crash_verify durabledb)

# Benchmarks
add_executable(bench bench/bench_main.cpp)
target_link_libraries(bench durabledb)
```

### 6.2 crc32.h

```cpp
#pragma once
#include <cstdint>
#include <cstddef>

namespace durabledb {

inline uint32_t crc32(const void* data, size_t len, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++)
    crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

}  // namespace durabledb
```

### 6.3 memtable.h

```cpp
#pragma once
#include <map>
#include <optional>
#include <string>

namespace durabledb {

struct Entry {
  bool tombstone = false;
  std::string value;
};

class MemTable {
 public:
  void Put(const std::string& k, const std::string& v);
  void Delete(const std::string& k);

  // Returns true if the key is present in the memtable (value or tombstone).
  bool Lookup(const std::string& k, Entry* out) const;

  size_t ApproxSize() const { return approx_size_; }
  bool Empty() const { return map_.empty(); }
  void Clear() { map_.clear(); approx_size_ = 0; }
  const std::map<std::string, Entry>& Data() const { return map_; }

 private:
  std::map<std::string, Entry> map_;
  size_t approx_size_ = 0;
};

}  // namespace durabledb
```

```cpp
// memtable.cpp
#include "durabledb/memtable.h"

namespace durabledb {

void MemTable::Put(const std::string& k, const std::string& v) {
  approx_size_ += k.size() + v.size();
  map_[k] = Entry{false, v};
}

void MemTable::Delete(const std::string& k) {
  approx_size_ += k.size();
  map_[k] = Entry{true, ""};
}

bool MemTable::Lookup(const std::string& k, Entry* out) const {
  auto it = map_.find(k);
  if (it == map_.end()) return false;
  *out = it->second;
  return true;
}

}  // namespace durabledb
```

### 6.4 wal.h and wal.cpp

```cpp
// wal.h
#pragma once
#include <functional>
#include <string>

namespace durabledb {

enum class RecType : uint8_t { PUT = 1, DELETE = 2 };

class WAL {
 public:
  explicit WAL(const std::string& path, bool sync_on_write = true);
  ~WAL();

  bool Append(RecType type, const std::string& key, const std::string& value);

  // Replays all valid records. Stops at the first bad record and truncates
  // the file there so new appends start from a clean point.
  // Returns number of records replayed.
  static size_t Replay(
      const std::string& path,
      const std::function<void(RecType, const std::string&, const std::string&)>& fn);

  // Delete the current log and start a new empty one.
  void Reset();

 private:
  std::string path_;
  int fd_ = -1;
  bool sync_;
};

}  // namespace durabledb
```

```cpp
// wal.cpp
#include "durabledb/wal.h"
#include "durabledb/crc32.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <vector>

namespace durabledb {

static void PutU32(std::string& s, uint32_t v) {
  s.append(reinterpret_cast<char*>(&v), 4);   // little-endian on x86/ARM Linux
}

WAL::WAL(const std::string& path, bool sync_on_write)
    : path_(path), sync_(sync_on_write) {
  fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
}

WAL::~WAL() { if (fd_ >= 0) ::close(fd_); }

bool WAL::Append(RecType type, const std::string& key, const std::string& value) {
  std::string payload;
  payload.push_back(static_cast<char>(type));
  PutU32(payload, key.size());
  PutU32(payload, value.size());
  payload += key;
  payload += value;

  std::string body;
  PutU32(body, payload.size());
  body += payload;

  uint32_t crc = crc32(body.data(), body.size());
  std::string rec;
  PutU32(rec, crc);
  rec += body;

  // One write() call per record so a torn write is at most one record.
  ssize_t n = ::write(fd_, rec.data(), rec.size());
  if (n != static_cast<ssize_t>(rec.size())) return false;
  if (sync_ && ::fsync(fd_) != 0) return false;
  return true;
}

size_t WAL::Replay(
    const std::string& path,
    const std::function<void(RecType, const std::string&, const std::string&)>& fn) {
  int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) return 0;

  off_t size = ::lseek(fd, 0, SEEK_END);
  std::vector<char> buf(size);
  ::pread(fd, buf.data(), size, 0);

  size_t off = 0, count = 0, good_end = 0;
  while (off + 8 <= buf.size()) {
    uint32_t crc, plen;
    std::memcpy(&crc, &buf[off], 4);
    std::memcpy(&plen, &buf[off + 4], 4);
    if (off + 8 + plen > buf.size()) break;               // torn tail
    if (crc32(&buf[off + 4], 4 + plen) != crc) break;     // corrupted

    const char* p = &buf[off + 8];
    RecType type = static_cast<RecType>(p[0]);
    uint32_t klen, vlen;
    std::memcpy(&klen, p + 1, 4);
    std::memcpy(&vlen, p + 5, 4);
    std::string key(p + 9, klen);
    std::string val(p + 9 + klen, vlen);
    fn(type, key, val);

    off += 8 + plen;
    good_end = off;
    count++;
  }

  if (good_end < buf.size()) {        // drop the bad tail
    ::ftruncate(fd, good_end);
    ::fsync(fd);
  }
  ::close(fd);
  return count;
}

void WAL::Reset() {
  if (fd_ >= 0) ::close(fd_);
  ::unlink(path_.c_str());
  fd_ = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
}

}  // namespace durabledb
```

### 6.5 sstable.h and sstable.cpp

```cpp
// sstable.h
#pragma once
#include "durabledb/memtable.h"
#include <map>
#include <string>
#include <vector>

namespace durabledb {

class SSTable {
 public:
  // Write a sorted map to path (via path + ".tmp" then rename).
  static bool Write(const std::string& path,
                    const std::map<std::string, Entry>& data);

  static SSTable* Open(const std::string& path);
  ~SSTable();

  // true if key found in this table (value or tombstone)
  bool Get(const std::string& key, Entry* out) const;

  // Load every record, used by compaction.
  void ReadAll(std::map<std::string, Entry>* out) const;

  const std::string& path() const { return path_; }

 private:
  struct IndexEntry { std::string key; uint64_t offset; };
  std::string path_;
  int fd_ = -1;
  uint64_t data_end_ = 0;
  std::vector<IndexEntry> index_;
};

}  // namespace durabledb
```

```cpp
// sstable.cpp (core logic, add error checks as you go)
#include "durabledb/sstable.h"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace durabledb {

static const uint64_t kMagic = 0x44555241424C4531ULL;   // "DURABLE1"
static const int kIndexEvery = 16;

static void PutU32(std::string& s, uint32_t v) { s.append((char*)&v, 4); }
static void PutU64(std::string& s, uint64_t v) { s.append((char*)&v, 8); }

bool SSTable::Write(const std::string& path,
                    const std::map<std::string, Entry>& data) {
  std::string out, index;
  uint32_t index_count = 0, i = 0;

  for (const auto& [k, e] : data) {
    if (i % kIndexEvery == 0) {
      PutU32(index, k.size());
      index += k;
      PutU64(index, out.size());
      index_count++;
    }
    out.push_back(e.tombstone ? 2 : 1);
    PutU32(out, k.size());
    PutU32(out, e.value.size());
    out += k;
    out += e.value;
    i++;
  }

  uint64_t index_offset = out.size();
  out += index;
  PutU64(out, index_offset);
  PutU32(out, index_count);
  PutU64(out, kMagic);

  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) return false;
  if (::write(fd, out.data(), out.size()) != (ssize_t)out.size()) return false;
  ::fsync(fd);                        // data is durable before it becomes visible
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) return false;

  // fsync the directory so the rename itself is durable
  std::string dir = path.substr(0, path.find_last_of('/'));
  int dfd = ::open(dir.c_str(), O_RDONLY);
  if (dfd >= 0) { ::fsync(dfd); ::close(dfd); }
  return true;
}

SSTable* SSTable::Open(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return nullptr;
  off_t size = ::lseek(fd, 0, SEEK_END);
  if (size < 20) { ::close(fd); return nullptr; }

  char footer[20];
  ::pread(fd, footer, 20, size - 20);
  uint64_t index_offset, magic;
  uint32_t index_count;
  std::memcpy(&index_offset, footer, 8);
  std::memcpy(&index_count, footer + 8, 4);
  std::memcpy(&magic, footer + 12, 8);
  if (magic != kMagic) { ::close(fd); return nullptr; }

  auto* t = new SSTable();
  t->path_ = path;
  t->fd_ = fd;
  t->data_end_ = index_offset;

  std::string idx(size - 20 - index_offset, '\0');
  ::pread(fd, idx.data(), idx.size(), index_offset);
  size_t p = 0;
  for (uint32_t n = 0; n < index_count; n++) {
    uint32_t klen; uint64_t off;
    std::memcpy(&klen, &idx[p], 4); p += 4;
    std::string key(&idx[p], klen); p += klen;
    std::memcpy(&off, &idx[p], 8); p += 8;
    t->index_.push_back({key, off});
  }
  return t;
}

SSTable::~SSTable() { if (fd_ >= 0) ::close(fd_); }

bool SSTable::Get(const std::string& key, Entry* out) const {
  if (index_.empty()) return false;
  // last index entry with key <= target
  auto it = std::upper_bound(index_.begin(), index_.end(), key,
      [](const std::string& k, const IndexEntry& e) { return k < e.key; });
  if (it == index_.begin()) return false;
  --it;
  uint64_t start = it->offset;
  uint64_t end = (it + 1 == index_.end()) ? data_end_ : (it + 1)->offset;

  std::string block(end - start, '\0');
  ::pread(fd_, block.data(), block.size(), start);

  size_t p = 0;
  while (p < block.size()) {
    uint8_t type = block[p];
    uint32_t klen, vlen;
    std::memcpy(&klen, &block[p + 1], 4);
    std::memcpy(&vlen, &block[p + 5], 4);
    std::string k(&block[p + 9], klen);
    if (k == key) {
      out->tombstone = (type == 2);
      out->value.assign(&block[p + 9 + klen], vlen);
      return true;
    }
    if (k > key) return false;
    p += 9 + klen + vlen;
  }
  return false;
}

void SSTable::ReadAll(std::map<std::string, Entry>* out) const {
  std::string data(data_end_, '\0');
  ::pread(fd_, data.data(), data.size(), 0);
  size_t p = 0;
  while (p < data.size()) {
    uint8_t type = data[p];
    uint32_t klen, vlen;
    std::memcpy(&klen, &data[p + 1], 4);
    std::memcpy(&vlen, &data[p + 5], 4);
    std::string k(&data[p + 9], klen);
    Entry e{type == 2, std::string(&data[p + 9 + klen], vlen)};
    (*out)[k] = e;
    p += 9 + klen + vlen;
  }
}

}  // namespace durabledb
```

### 6.6 db.h and db.cpp

```cpp
// db.h
#pragma once
#include "durabledb/memtable.h"
#include "durabledb/sstable.h"
#include "durabledb/wal.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace durabledb {

struct Options {
  size_t memtable_limit = 4 * 1024 * 1024;   // 4 MB
  size_t compaction_trigger = 4;             // number of SSTables
  bool sync_wal = true;                      // turn off only for benchmarks
};

class DB {
 public:
  static std::unique_ptr<DB> Open(const std::string& dir, const Options& opts = {});

  bool Put(const std::string& key, const std::string& value);
  bool Delete(const std::string& key);
  std::optional<std::string> Get(const std::string& key);

  void Flush();      // force memtable to disk
  void Compact();    // force compaction

 private:
  DB(const std::string& dir, const Options& opts);
  void Recover();
  void MaybeFlush();
  uint64_t NextId() { return ++last_id_; }
  std::string SstPath(uint64_t id) const;

  std::string dir_;
  Options opts_;
  MemTable mem_;
  std::unique_ptr<WAL> wal_;
  std::vector<std::unique_ptr<SSTable>> tables_;   // oldest first
  std::vector<uint64_t> table_ids_;
  uint64_t last_id_ = 0;
};

}  // namespace durabledb
```

```cpp
// db.cpp
#include "durabledb/db.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>

namespace durabledb {

DB::DB(const std::string& dir, const Options& opts) : dir_(dir), opts_(opts) {}

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& opts) {
  ::mkdir(dir.c_str(), 0755);
  std::unique_ptr<DB> db(new DB(dir, opts));
  db->Recover();
  return db;
}

std::string DB::SstPath(uint64_t id) const {
  char name[32];
  std::snprintf(name, sizeof(name), "/%06llu.sst", (unsigned long long)id);
  return dir_ + name;
}

void DB::Recover() {
  // 1. clean leftover tmp files and find SSTables
  std::vector<uint64_t> ids;
  DIR* d = ::opendir(dir_.c_str());
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n.size() > 4 && n.substr(n.size() - 4) == ".tmp") {
      ::unlink((dir_ + "/" + n).c_str());
    } else if (n.size() > 4 && n.substr(n.size() - 4) == ".sst") {
      ids.push_back(std::stoull(n.substr(0, n.size() - 4)));
    }
  }
  ::closedir(d);
  std::sort(ids.begin(), ids.end());

  // 2. open SSTables
  for (uint64_t id : ids) {
    tables_.emplace_back(SSTable::Open(SstPath(id)));
    table_ids_.push_back(id);
    last_id_ = std::max(last_id_, id);
  }

  // 3. replay WAL into memtable
  std::string wal_path = dir_ + "/wal.log";
  WAL::Replay(wal_path, [&](RecType t, const std::string& k, const std::string& v) {
    if (t == RecType::PUT) mem_.Put(k, v);
    else mem_.Delete(k);
  });
  wal_ = std::make_unique<WAL>(wal_path, opts_.sync_wal);
}

bool DB::Put(const std::string& key, const std::string& value) {
  if (!wal_->Append(RecType::PUT, key, value)) return false;
  mem_.Put(key, value);
  MaybeFlush();
  return true;
}

bool DB::Delete(const std::string& key) {
  if (!wal_->Append(RecType::DELETE, key, "")) return false;
  mem_.Delete(key);
  MaybeFlush();
  return true;
}

std::optional<std::string> DB::Get(const std::string& key) {
  Entry e;
  if (mem_.Lookup(key, &e))
    return e.tombstone ? std::nullopt : std::optional<std::string>(e.value);
  for (auto it = tables_.rbegin(); it != tables_.rend(); ++it) {   // newest first
    if ((*it)->Get(key, &e))
      return e.tombstone ? std::nullopt : std::optional<std::string>(e.value);
  }
  return std::nullopt;
}

void DB::MaybeFlush() {
  if (mem_.ApproxSize() >= opts_.memtable_limit) Flush();
}

void DB::Flush() {
  if (mem_.Empty()) return;
  uint64_t id = NextId();
  // ORDER MATTERS: SSTable must be durable BEFORE the WAL is dropped.
  SSTable::Write(SstPath(id), mem_.Data());
  tables_.emplace_back(SSTable::Open(SstPath(id)));
  table_ids_.push_back(id);
  mem_.Clear();
  wal_->Reset();
  if (tables_.size() >= opts_.compaction_trigger) Compact();
}

void DB::Compact() {
  if (tables_.size() < 2) return;
  std::map<std::string, Entry> merged;
  for (auto& t : tables_) t->ReadAll(&merged);   // oldest to newest, newest wins

  // We merge ALL tables, so tombstones have nothing older to hide. Drop them.
  for (auto it = merged.begin(); it != merged.end();) {
    if (it->second.tombstone) it = merged.erase(it);
    else ++it;
  }

  uint64_t id = NextId();
  SSTable::Write(SstPath(id), merged);        // tmp + fsync + rename inside

  // swap in the new table, then delete old files
  std::vector<uint64_t> old_ids = table_ids_;
  tables_.clear();
  table_ids_.clear();
  tables_.emplace_back(SSTable::Open(SstPath(id)));
  table_ids_.push_back(id);
  for (uint64_t oid : old_ids) ::unlink(SstPath(oid).c_str());
}

}  // namespace durabledb
```

**Why the order matters (good interview point):**
- Flush: SSTable is fsynced and renamed first, then the WAL is reset. If we crash in between, the WAL is replayed again on startup. That is safe because replaying a Put twice gives the same state.
- Compaction: the new file is fully written before old ones are deleted. A crash in between leaves duplicate data (new merged table plus old tables), which is still correct because newer tables win on reads.

> Known gap to note in the README: if we crash after writing the merged table but before deleting old ones, the merged table has the highest id so it wins on reads, so results are still right. Cleaning up the stale old tables on next startup is a nice extra.

## 7. Crash testing

The goal is to prove that **no acknowledged write is lost** after `kill -9`.

### tools/crash_writer.cpp

```cpp
// Writes key_0, key_1, ... and prints "ACK n" ONLY after Put() returns.
#include "durabledb/db.h"
#include <iostream>

int main(int argc, char** argv) {
  auto db = durabledb::DB::Open(argv[1]);
  for (int i = 0;; i++) {
    std::string k = "key_" + std::to_string(i);
    db->Put(k, "value_" + std::to_string(i));
    std::cout << "ACK " << i << std::endl;   // endl flushes the line
  }
}
```

### tools/crash_verify.cpp

```cpp
// usage: crash_verify <dir> <last_acked_index>
#include "durabledb/db.h"
#include <iostream>

int main(int argc, char** argv) {
  auto db = durabledb::DB::Open(argv[1]);
  int last = std::stoi(argv[2]);
  for (int i = 0; i <= last; i++) {
    auto v = db->Get("key_" + std::to_string(i));
    if (!v || *v != "value_" + std::to_string(i)) {
      std::cerr << "LOST WRITE at key_" << i << "\n";
      return 1;
    }
  }
  std::cout << "OK: all " << last + 1 << " acked writes present\n";
  return 0;
}
```

### tools/crash_test.sh

```bash
#!/usr/bin/env bash
# Runs N rounds of: start writer, kill -9 at a random time, verify.
set -e
BUILD=${1:-build}
ROUNDS=${2:-50}
DIR=$(mktemp -d)

for r in $(seq 1 $ROUNDS); do
  $BUILD/crash_writer "$DIR" > "$DIR/acks.txt" &
  PID=$!
  sleep "0.$((RANDOM % 9 + 1))"
  kill -9 $PID
  wait $PID 2>/dev/null || true

  LAST=$(grep '^ACK' "$DIR/acks.txt" | tail -n 1 | awk '{print $2}')
  [ -z "$LAST" ] && continue
  $BUILD/crash_verify "$DIR" "$LAST" || { echo "FAILED in round $r"; exit 1; }
  echo "round $r passed (last ack $LAST)"
done
echo "All $ROUNDS rounds passed"
```

Note: the writer keeps appending the same `key_0...` sequence after restart, which is fine because each round starts over and overwrites the same keys with the same values.

## 8. Unit tests (GoogleTest)

Test list to write:

| File | What it checks |
|---|---|
| `memtable_test.cpp` | put/get/delete, tombstones, size tracking |
| `wal_test.cpp` | append then replay returns same records |
| `wal_test.cpp` | truncated last record is skipped and file is truncated |
| `wal_test.cpp` | flipped byte in the middle fails the CRC and replay stops |
| `sstable_test.cpp` | write then open then get every key, missing keys return false |
| `recovery_test.cpp` | put keys, destroy DB without flush, reopen, keys are there |
| `recovery_test.cpp` | flush, put more, reopen, both old and new keys are there |
| `recovery_test.cpp` | delete then reopen, key stays deleted |
| `random_vs_map_test.cpp` | 100k random ops compared against `std::map`, with random reopen |

Example of the randomized test:

```cpp
TEST(RandomVsMap, MatchesStdMap) {
  std::string dir = MakeTempDir();
  Options opts; opts.memtable_limit = 4096;   // tiny, forces many flushes
  auto db = DB::Open(dir, opts);
  std::map<std::string, std::string> ref;
  std::mt19937 rng(42);

  for (int i = 0; i < 100000; i++) {
    std::string k = "k" + std::to_string(rng() % 500);
    int op = rng() % 10;
    if (op < 6)      { std::string v = std::to_string(rng()); db->Put(k, v); ref[k] = v; }
    else if (op < 8) { db->Delete(k); ref.erase(k); }
    else {
      auto got = db->Get(k);
      auto it = ref.find(k);
      if (it == ref.end()) ASSERT_FALSE(got.has_value());
      else { ASSERT_TRUE(got.has_value()); ASSERT_EQ(*got, it->second); }
    }
    if (i % 20000 == 0) db = DB::Open(dir, opts);   // simulate restart
  }
}
```

Tip: set `sync_wal = false` for this test so it runs fast. Durability is covered by the crash test.

## 9. Benchmarks

Measure these three things and put charts in the README.

1. **Write throughput:** `sync_wal = true` vs `false` (shows the cost of `fsync`).
2. **Read latency:** before vs after compaction (shows why compaction matters).
3. **Recovery time:** vs WAL size (1 MB, 10 MB, 100 MB).

```cpp
// bench/bench_main.cpp (simple custom timer version)
#include "durabledb/db.h"
#include <chrono>
#include <iostream>

using Clock = std::chrono::steady_clock;

static double Secs(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

int main() {
  for (bool sync : {true, false}) {
    std::string dir = "/tmp/bench_" + std::to_string(sync);
    durabledb::Options o; o.sync_wal = sync;
    auto db = durabledb::DB::Open(dir, o);

    int n = sync ? 5000 : 200000;
    auto t0 = Clock::now();
    for (int i = 0; i < n; i++)
      db->Put("key_" + std::to_string(i), std::string(100, 'x'));
    double s = Secs(t0, Clock::now());
    std::cout << "sync=" << sync << " writes/sec=" << n / s << "\n";
  }
}
```

Run the benchmark on a real disk path, not `/tmp` if `/tmp` is tmpfs, otherwise `fsync` will look free. Use `perf stat` and `strace -c` to show where time goes.

Charts: dump results to CSV and plot with a short Python matplotlib script.

## 10. CI (GitHub Actions)

```yaml
# .github/workflows/ci.yml
name: ci
on: [push, pull_request]

jobs:
  build-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - name: Install tools
        run: sudo apt-get update && sudo apt-get install -y cmake ninja-build
      - name: Configure (ASan)
        run: cmake -S . -B build -G Ninja -DENABLE_ASAN=ON
      - name: Build
        run: cmake --build build
      - name: Unit tests
        run: ctest --test-dir build --output-on-failure
      - name: Crash test
        run: bash tools/crash_test.sh build 20
```

## 11. 8-week timeline with checklists

**Week 1 | Setup and basics**
- [ ] Create repo `durable-db`, add CMake, GoogleTest, CI skeleton
- [ ] Implement `MemTable` with tests
- [ ] Write down the record format in the README

**Week 2 | Write-ahead log**
- [ ] `crc32.h`
- [ ] `WAL::Append` with `fsync`
- [ ] `WAL::Replay` with unit tests (normal, truncated, corrupted)

**Week 3 | Crash recovery**
- [ ] `DB::Open` replays WAL
- [ ] Torn last record handled and file truncated
- [ ] `crash_writer`, `crash_verify`, `crash_test.sh` working locally

**Week 4 | SSTables**
- [ ] `SSTable::Write` (tmp file, fsync, rename, directory fsync)
- [ ] `SSTable::Open` and `Get` with sparse index
- [ ] `DB::Flush` and read path across memtable and tables
- [ ] **Checkpoint: working end-to-end version**

**Week 5 | Compaction**
- [ ] `DB::Compact` merging all tables
- [ ] Tombstone dropping
- [ ] Startup cleanup of `.tmp` files and stale tables

**Week 6 | Testing and hardening**
- [ ] Randomized test vs `std::map`
- [ ] Fault injection: truncate the WAL at every byte offset and check recovery
- [ ] Run everything under AddressSanitizer and valgrind

**Week 7 | Benchmarks**
- [ ] Write throughput (sync vs no sync)
- [ ] Read latency before and after compaction
- [ ] Recovery time vs WAL size
- [ ] Charts for the README

**Week 8 | Polish**
- [ ] README: architecture diagram, design decisions, benchmark results
- [ ] Code cleanup and comments
- [ ] "What I learned" and "future work" sections

## 12. Stretch goals (only if ahead of schedule)

- Streaming k-way merge in compaction instead of loading everything into a map
- Bloom filter per SSTable to skip useless reads
- Skip list memtable and compare against `std::map`
- Leveled compaction
- Simple iterator API for range scans
- `mmap` reads vs `pread` comparison

## 13. Risks and fixes

| Risk | Fix |
|---|---|
| Falling behind schedule | Drop sparse index (use a full index) and compaction tuning first |
| `fsync` looks too fast in benchmarks | Make sure the data dir is on a real disk, not tmpfs |
| Hard to debug partial writes | Write a helper that truncates the WAL at every byte offset and reopens |
| Compaction bugs | Keep the randomized `std::map` test running with a tiny memtable limit |

The WAL and crash recovery are the core of this project, so protect those above everything else.

## 14. Deliverables

- GitHub repo: github.com/naga251602/durable-db
- README with design, tradeoffs, and benchmark charts
- Test suite running in CI
- `tools/crash_test.sh` that anyone can run to watch recovery work

## 15. Resume bullet (draft)

Built a key-value storage engine in C++ on Linux with write-ahead logging, SSTables, compaction, and crash recovery | verified durability with randomized `kill -9` crash tests and benchmarked write throughput and recovery time.

## 16. Interview talking points

- Why `fsync` after every write, and what it costs (show the benchmark numbers)
- Why the WAL is only deleted after the SSTable is durable
- What a torn write is, and how the CRC detects it
- Why compaction writes to a temp file and uses `rename` (atomic swap)
- What tombstones are and when it is safe to drop them
- What I would do differently with more time (bloom filters, leveled compaction, concurrent writers)
