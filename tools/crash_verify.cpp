// Reopens the database after a crash and checks that every acknowledged
// operation survived and nothing else is there.
//
// The operation right after the last acknowledged one may or may not have been
// applied (the process can die after the WAL write but before printing the
// ack), so the database must match EITHER the state after `last` OR the state
// after `last + 1`. Anything else is a bug.
//
// usage: crash_verify <dir> <last_acked_index>
#include <iostream>

#include "crash_ops.h"
#include "durabledb/db.h"

using Model = std::map<std::string, std::string>;

static bool Matches(durabledb::DB& db, const Model& m, long key_range) {
  for (long i = 0; i <= key_range; i++) {
    std::string k = "key_" + std::to_string(i);
    auto got = db.Get(k);
    auto it = m.find(k);
    if (it == m.end()) {
      if (got) return false;
    } else if (!got || *got != it->second) {
      return false;
    }
  }
  return true;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: crash_verify <dir> <last_acked_index>\n";
    return 2;
  }
  long last = std::atol(argv[2]);

  durabledb::Options opts;
  opts.compaction_trigger = 3;
  auto db = durabledb::DB::Open(argv[1], opts);

  long range = last + 8;
  if (Matches(*db, ModelAfter(last), range) || Matches(*db, ModelAfter(last + 1), range)) {
    std::cout << "OK  (" << last + 1 << " acknowledged ops verified, " << db->NumTables()
              << " sstables)\n";
    return 0;
  }
  std::cerr << "FAIL: database state does not match after " << last << " acknowledged ops\n";
  return 1;
}
