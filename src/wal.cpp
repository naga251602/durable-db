#include "durabledb/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include "durabledb/coding.h"
#include "durabledb/crc32.h"
#include "durabledb/fault.h"
#include "durabledb/io.h"

namespace durabledb {

namespace {
constexpr size_t kHeaderSize = 8;               // crc + payload_len
constexpr size_t kPayloadFixed = 9;             // type + key_len + val_len
constexpr uint32_t kMaxPayload = 1u << 30;      // sanity limit (1 GiB)

std::string EncodeRecord(RecType type, const std::string& key, const std::string& value) {
  std::string body;  // payload_len + payload (this is what the CRC covers)
  PutU32(body, static_cast<uint32_t>(kPayloadFixed + key.size() + value.size()));
  body.push_back(static_cast<char>(type));
  PutU32(body, static_cast<uint32_t>(key.size()));
  PutU32(body, static_cast<uint32_t>(value.size()));
  body += key;
  body += value;

  std::string rec;
  PutU32(rec, crc32(body.data(), body.size()));
  rec += body;
  return rec;
}
}  // namespace

WAL::WAL(const std::string& path, bool sync_on_write) : path_(path), sync_(sync_on_write) {
  fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (fd_ >= 0) {
    off_t end = ::lseek(fd_, 0, SEEK_END);
    offset_ = end < 0 ? 0 : static_cast<uint64_t>(end);
    // make sure the (possibly new) file name is durable
    io::FsyncDir(io::DirName(path));
  }
}

WAL::~WAL() {
  if (fd_ >= 0) ::close(fd_);
}

bool WAL::Append(RecType type, const std::string& key, const std::string& value) {
  if (fd_ < 0) return false;
  std::string rec = EncodeRecord(type, key, value);

  fault::CrashPoint();  // crash before anything was written

  bool ok;
  int& fail = fault::fail_wal_append_after();
  if (fail == 0) {
    // injected fault: write only half of the record, then report failure
    fail = -1;
    io::WriteAll(fd_, rec.data(), rec.size() / 2);
    ok = false;
  } else {
    if (fail > 0) fail--;
    ok = io::WriteAll(fd_, rec.data(), rec.size());
    fault::CrashPoint();  // crash after write() but before fsync()
    if (ok && sync_) ok = ::fsync(fd_) == 0;
  }

  if (!ok) {
    // Cut off whatever part of the record got written.
    if (::ftruncate(fd_, static_cast<off_t>(offset_)) != 0) { /* best effort */ }
    return false;
  }
  offset_ += rec.size();
  return true;
}

bool WAL::Reset() {
  if (fd_ < 0) return false;
  if (::ftruncate(fd_, 0) != 0) return false;
  if (::fsync(fd_) != 0) return false;
  offset_ = 0;
  return true;
}

size_t WAL::Replay(const std::string& path, const ReplayFn& fn) {
  std::string buf;
  if (!io::ReadFile(path, &buf)) return 0;

  size_t off = 0, count = 0;
  while (off + kHeaderSize <= buf.size()) {
    uint32_t crc = GetU32(&buf[off]);
    uint32_t plen = GetU32(&buf[off + 4]);
    if (plen < kPayloadFixed || plen > kMaxPayload) break;      // nonsense length
    if (off + kHeaderSize + plen > buf.size()) break;           // torn tail
    if (crc32(&buf[off + 4], 4 + plen) != crc) break;           // corrupted

    const char* p = &buf[off + kHeaderSize];
    uint8_t t = static_cast<uint8_t>(p[0]);
    uint32_t klen = GetU32(p + 1);
    uint32_t vlen = GetU32(p + 5);
    if ((t != 1 && t != 2) || kPayloadFixed + uint64_t(klen) + vlen != plen) break;

    fn(static_cast<RecType>(t), std::string(p + kPayloadFixed, klen),
       std::string(p + kPayloadFixed + klen, vlen));

    off += kHeaderSize + plen;
    count++;
  }

  if (off < buf.size()) {
    // Drop the bad tail so new appends continue from a clean point.
    int fd = ::open(path.c_str(), O_RDWR);
    if (fd >= 0) {
      if (::ftruncate(fd, static_cast<off_t>(off)) == 0) ::fsync(fd);
      ::close(fd);
    }
  }
  return count;
}

}  // namespace durabledb
