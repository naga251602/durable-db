#include "durabledb/db.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>
#include <stdexcept>

#include "durabledb/fault.h"
#include "durabledb/io.h"

namespace durabledb {

namespace {
bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}  // namespace

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& opts) {
  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
    throw std::runtime_error("cannot create directory " + dir);
  std::unique_ptr<DB> db(new DB(dir, opts));
  db->Recover();
  return db;
}

DB::~DB() {
  if (lock_fd_ >= 0) ::close(lock_fd_);  // releases the flock
}

std::string DB::SstPath(uint64_t id) const {
  char name[40];
  std::snprintf(name, sizeof(name), "/%06llu.sst", static_cast<unsigned long long>(id));
  return dir_ + name;
}

void DB::WriteManifest(const std::vector<uint64_t>& ids) {
  std::string text;
  for (uint64_t id : ids) text += std::to_string(id) + "\n";
  if (!io::WriteFileAtomic(dir_ + "/MANIFEST", text))
    throw std::runtime_error("failed to write MANIFEST");
}

void DB::Recover() {
  // 0. Only one process may use the directory at a time.
  lock_fd_ = ::open((dir_ + "/LOCK").c_str(), O_CREAT | O_RDWR, 0644);
  if (lock_fd_ < 0 || ::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0)
    throw std::runtime_error("database is locked by another instance: " + dir_);

  // 1. The MANIFEST says which SSTables are live.
  std::vector<uint64_t> live;
  std::string manifest;
  if (io::ReadFile(dir_ + "/MANIFEST", &manifest)) {
    std::istringstream in(manifest);
    std::string line;
    while (std::getline(in, line))
      if (!line.empty()) live.push_back(std::stoull(line));
  }
  std::set<uint64_t> live_set(live.begin(), live.end());

  // 2. Delete leftovers of an interrupted flush/compaction: *.tmp files and
  //    SSTables that the MANIFEST does not know about.
  DIR* d = ::opendir(dir_.c_str());
  if (!d) throw std::runtime_error("cannot open directory " + dir_);
  while (dirent* e = ::readdir(d)) {
    std::string name = e->d_name;
    if (EndsWith(name, ".tmp")) {
      ::unlink((dir_ + "/" + name).c_str());
    } else if (EndsWith(name, ".sst")) {
      uint64_t id = std::stoull(name.substr(0, name.size() - 4));
      if (!live_set.count(id)) ::unlink((dir_ + "/" + name).c_str());
    }
  }
  ::closedir(d);

  // 3. Open the live SSTables, oldest first.
  std::sort(live.begin(), live.end());
  for (uint64_t id : live) {
    auto sst = SSTable::Open(SstPath(id));
    if (!sst) throw std::runtime_error("missing or corrupted SSTable " + SstPath(id));
    tables_.push_back({id, std::move(sst)});
    last_id_ = std::max(last_id_, id);
  }

  // 4. Replay the WAL to rebuild the memtable. A torn last record is dropped.
  std::string wal_path = dir_ + "/wal.log";
  WAL::Replay(wal_path, [&](RecType t, const std::string& k, const std::string& v) {
    if (t == RecType::PUT) mem_.Put(k, v);
    else mem_.Delete(k);
  });
  wal_ = std::make_unique<WAL>(wal_path, opts_.sync_wal);
  if (!wal_->ok()) throw std::runtime_error("cannot open WAL");

  MaybeFlush();
}

bool DB::Put(const std::string& key, const std::string& value) {
  if (!wal_->Append(RecType::PUT, key, value)) return false;  // log first, then memtable
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
  for (auto it = tables_.rbegin(); it != tables_.rend(); ++it) {  // newest first
    if (it->sst->Get(key, &e))
      return e.tombstone ? std::nullopt : std::optional<std::string>(e.value);
  }
  return std::nullopt;
}

void DB::MaybeFlush() {
  if (mem_.ApproxSize() >= opts_.memtable_limit) Flush();
}

// Flush order (each step is safe to crash after):
//   1. write NNNNNN.sst            -> crash: file is an orphan, deleted at startup,
//                                     WAL still has everything
//   2. update MANIFEST             -> crash: table is live; WAL replay re-applies
//                                     the same records (replaying twice is harmless)
//   3. reset the WAL
void DB::Flush() {
  if (mem_.Empty()) return;
  uint64_t id = ++last_id_;
  if (!SSTable::Write(SstPath(id), mem_.Data())) throw std::runtime_error("failed to write SSTable");
  fault::CrashPoint();  // A: SSTable durable, MANIFEST not updated

  auto sst = SSTable::Open(SstPath(id));
  if (!sst) throw std::runtime_error("failed to reopen new SSTable");
  std::vector<uint64_t> ids;
  for (auto& t : tables_) ids.push_back(t.id);
  ids.push_back(id);
  WriteManifest(ids);
  fault::CrashPoint();  // B: MANIFEST updated, WAL not reset

  tables_.push_back({id, std::move(sst)});
  mem_.Clear();
  if (!wal_->Reset()) throw std::runtime_error("failed to reset WAL");
  fault::CrashPoint();  // C: all done

  if (tables_.size() >= opts_.compaction_trigger) Compact();
}

// Compaction merges ALL SSTables into one. Because nothing older remains, a
// tombstone has nothing left to hide and can be dropped for good.
//   1. write the merged table      -> crash: orphan, deleted at startup
//   2. update MANIFEST (atomic)    -> the switch from "old tables" to "new table"
//   3. delete the old files        -> crash: orphans, deleted at startup
// The MANIFEST is what makes this safe: without it, a crash after step 3 started
// could leave old tables that still hold a value whose tombstone was dropped,
// and a deleted key would come back to life.
void DB::Compact() {
  if (tables_.size() < 2) return;

  std::map<std::string, Entry> merged;
  for (auto& t : tables_)  // oldest to newest, so newer values overwrite older ones
    if (!t.sst->ReadAll(&merged)) throw std::runtime_error("failed to read SSTable during compaction");
  for (auto it = merged.begin(); it != merged.end();) {
    if (it->second.tombstone) it = merged.erase(it);
    else ++it;
  }

  std::vector<uint64_t> old_ids;
  for (auto& t : tables_) old_ids.push_back(t.id);

  std::vector<Table> fresh;
  std::vector<uint64_t> new_ids;
  if (!merged.empty()) {
    uint64_t id = ++last_id_;
    if (!SSTable::Write(SstPath(id), merged)) throw std::runtime_error("failed to write merged SSTable");
    auto sst = SSTable::Open(SstPath(id));
    if (!sst) throw std::runtime_error("failed to reopen merged SSTable");
    fresh.push_back({id, std::move(sst)});
    new_ids.push_back(id);
  }
  fault::CrashPoint();  // D: merged table written, MANIFEST still points at old tables

  WriteManifest(new_ids);
  fault::CrashPoint();  // E: MANIFEST switched, old files still on disk

  tables_ = std::move(fresh);  // closes the old files
  for (uint64_t oid : old_ids) {
    ::unlink(SstPath(oid).c_str());
    fault::CrashPoint();  // F: part way through deleting old files
  }
  io::FsyncDir(dir_);
}

}  // namespace durabledb
