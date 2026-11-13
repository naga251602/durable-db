// Applies the operation sequence from crash_ops.h and prints "ACK <n> ." only
// AFTER operation n has returned from the database. If this process is killed,
// every operation that was acknowledged must survive recovery.
//
// usage: crash_writer <dir> [memtable_limit_bytes] [max_ops] [sync 0|1]
#include <cstdlib>
#include <iostream>

#include "crash_ops.h"
#include "durabledb/db.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: crash_writer <dir> [memtable_limit] [max_ops]\n";
    return 2;
  }
  durabledb::Options opts;
  opts.memtable_limit = argc > 2 ? std::strtoul(argv[2], nullptr, 10) : 4096;  // small: many flushes
  opts.compaction_trigger = 3;                                                  // frequent compactions
  long max_ops = argc > 3 ? std::atol(argv[3]) : 1000000000L;
  // fsync is not needed to test PROCESS crashes: the OS page cache survives a
  // kill -9. (It would be needed to survive power loss.)
  opts.sync_wal = argc > 4 ? std::atoi(argv[4]) != 0 : true;

  auto db = durabledb::DB::Open(argv[1], opts);
  for (long n = 0; n < max_ops; n++) {
    Op op = OpAt(n);
    bool ok = op.del ? db->Delete(op.key) : db->Put(op.key, op.value);
    if (!ok) return 1;
    std::cout << "ACK " << n << " ." << std::endl;  // endl flushes the line
  }
  return 0;
}
