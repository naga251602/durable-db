#include "durabledb/memtable.h"

namespace durabledb {

void MemTable::Put(const std::string& key, const std::string& value) {
  auto it = map_.find(key);
  if (it != map_.end()) {
    // overwrite: only the size difference changes
    approx_size_ -= it->second.value.size();
    approx_size_ += value.size();
    it->second = Entry{false, value};
    return;
  }
  approx_size_ += key.size() + value.size();
  map_.emplace(key, Entry{false, value});
}

void MemTable::Delete(const std::string& key) {
  auto it = map_.find(key);
  if (it != map_.end()) {
    approx_size_ -= it->second.value.size();
    it->second = Entry{true, ""};
    return;
  }
  approx_size_ += key.size();
  map_.emplace(key, Entry{true, ""});
}

bool MemTable::Lookup(const std::string& key, Entry* out) const {
  auto it = map_.find(key);
  if (it == map_.end()) return false;
  *out = it->second;
  return true;
}

void MemTable::Clear() {
  map_.clear();
  approx_size_ = 0;
}

}  // namespace durabledb
