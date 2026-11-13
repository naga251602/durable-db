#include "durabledb/sstable.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>

#include "durabledb/coding.h"
#include "durabledb/crc32.h"
#include "durabledb/io.h"

namespace durabledb {

namespace {
constexpr uint64_t kMagic = 0x315F454C42415544ULL;  // bytes spell "DUABLE_1"
constexpr size_t kFooterSize = 24;
constexpr size_t kRecordFixed = 9;  // type + key_len + val_len
constexpr uint32_t kIndexEvery = 16;
}  // namespace

bool SSTable::Write(const std::string& path, const std::map<std::string, Entry>& data) {
  std::string out, index;
  uint32_t index_count = 0, i = 0;

  for (const auto& [k, e] : data) {
    if (i % kIndexEvery == 0) {
      PutU32(index, static_cast<uint32_t>(k.size()));
      index += k;
      PutU64(index, out.size());
      index_count++;
    }
    out.push_back(e.tombstone ? 2 : 1);
    PutU32(out, static_cast<uint32_t>(k.size()));
    PutU32(out, static_cast<uint32_t>(e.value.size()));
    out += k;
    out += e.value;
    i++;
  }

  uint64_t index_offset = out.size();
  out += index;
  uint32_t crc = crc32(out.data(), out.size());
  PutU64(out, index_offset);
  PutU32(out, index_count);
  PutU32(out, crc);
  PutU64(out, kMagic);

  return io::WriteFileAtomic(path, out);
}

std::unique_ptr<SSTable> SSTable::Open(const std::string& path) {
  std::string file;
  if (!io::ReadFile(path, &file) || file.size() < kFooterSize) return nullptr;

  const char* footer = file.data() + file.size() - kFooterSize;
  uint64_t index_offset = GetU64(footer);
  uint32_t index_count = GetU32(footer + 8);
  uint32_t crc = GetU32(footer + 12);
  uint64_t magic = GetU64(footer + 16);

  size_t body = file.size() - kFooterSize;
  if (magic != kMagic || index_offset > body) return nullptr;
  if (crc32(file.data(), body) != crc) return nullptr;

  std::unique_ptr<SSTable> t(new SSTable());
  t->path_ = path;
  t->data_end_ = index_offset;

  size_t p = index_offset;
  for (uint32_t n = 0; n < index_count; n++) {
    if (p + 4 > body) return nullptr;
    uint32_t klen = GetU32(&file[p]);
    p += 4;
    if (p + klen + 8 > body) return nullptr;
    std::string key(&file[p], klen);
    p += klen;
    uint64_t off = GetU64(&file[p]);
    p += 8;
    if (off > index_offset) return nullptr;
    t->index_.push_back({std::move(key), off});
  }

  t->fd_ = ::open(path.c_str(), O_RDONLY);
  if (t->fd_ < 0) return nullptr;
  return t;
}

SSTable::~SSTable() {
  if (fd_ >= 0) ::close(fd_);
}

bool SSTable::Get(const std::string& key, Entry* out) const {
  if (index_.empty()) return false;

  // Binary search the sparse index: last block whose first key is <= target.
  auto it = std::upper_bound(index_.begin(), index_.end(), key,
                             [](const std::string& k, const IndexEntry& e) { return k < e.key; });
  if (it == index_.begin()) return false;
  --it;
  uint64_t start = it->offset;
  uint64_t end = (it + 1 == index_.end()) ? data_end_ : (it + 1)->offset;

  std::string block(end - start, '\0');
  if (!io::ReadAt(fd_, block.data(), block.size(), static_cast<off_t>(start))) return false;

  // Scan the (at most 16) records in this block.
  size_t p = 0;
  while (p + kRecordFixed <= block.size()) {
    uint8_t type = static_cast<uint8_t>(block[p]);
    uint32_t klen = GetU32(&block[p + 1]);
    uint32_t vlen = GetU32(&block[p + 5]);
    if (p + kRecordFixed + uint64_t(klen) + vlen > block.size()) return false;
    std::string k(&block[p + kRecordFixed], klen);
    if (k == key) {
      out->tombstone = (type == 2);
      out->value.assign(&block[p + kRecordFixed + klen], vlen);
      return true;
    }
    if (k > key) return false;  // sorted, so we passed it
    p += kRecordFixed + klen + vlen;
  }
  return false;
}

bool SSTable::ReadAll(std::map<std::string, Entry>* out) const {
  std::string data(data_end_, '\0');
  if (data_end_ > 0 && !io::ReadAt(fd_, data.data(), data.size(), 0)) return false;
  size_t p = 0;
  while (p + kRecordFixed <= data.size()) {
    uint8_t type = static_cast<uint8_t>(data[p]);
    uint32_t klen = GetU32(&data[p + 1]);
    uint32_t vlen = GetU32(&data[p + 5]);
    if (p + kRecordFixed + uint64_t(klen) + vlen > data.size()) return false;
    std::string k(&data[p + kRecordFixed], klen);
    (*out)[k] = Entry{type == 2, std::string(&data[p + kRecordFixed + klen], vlen)};
    p += kRecordFixed + klen + vlen;
  }
  return true;
}

}  // namespace durabledb
