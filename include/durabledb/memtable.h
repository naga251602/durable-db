#pragma once
#include <map>
#include <string>

namespace durabledb {

struct Entry {
  bool tombstone = false;
  std::string value;
};

// In-memory sorted table holding the newest writes. A delete is stored as a
// tombstone so it can hide older values that live in SSTables on disk.
class MemTable {
 public:
  void Put(const std::string& key, const std::string& value);
  void Delete(const std::string& key);

  // true if the key is present (as a value or as a tombstone).
  bool Lookup(const std::string& key, Entry* out) const;

  size_t ApproxSize() const { return approx_size_; }
  bool Empty() const { return map_.empty(); }
  void Clear();
  const std::map<std::string, Entry>& Data() const { return map_; }

 private:
  std::map<std::string, Entry> map_;
  size_t approx_size_ = 0;
};

}  // namespace durabledb
