// Three benchmarks, each writes a CSV file:
//   1. write throughput: fsync on every write vs no fsync
//   2. read latency before and after compaction (hits and misses)
//   3. recovery time vs WAL size
//
// usage: bench <work_dir> <out_dir>
// Use a work_dir on a REAL disk. On tmpfs fsync is free and the numbers lie.
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "durabledb/db.h"

using Clock = std::chrono::steady_clock;
using durabledb::DB;
using durabledb::Options;

static double Secs(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

static void RemoveDir(const std::string& dir) {
  std::string cmd = "rm -rf '" + dir + "'";
  if (std::system(cmd.c_str()) != 0) std::cerr << "warning: could not remove " << dir << "\n";
}

static std::string Key(long i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key_%09ld", i);
  return buf;
}

// ---------- 1. write throughput ----------
static void BenchWrites(const std::string& work, std::ofstream& csv) {
  std::cout << "\n[1] write throughput (100 byte values)\n";
  csv << "mode,ops,seconds,ops_per_sec\n";
  for (bool sync : {true, false}) {
    std::string dir = work + "/writes";
    RemoveDir(dir);
    Options o;
    o.sync_wal = sync;
    auto db = DB::Open(dir, o);

    long n = 0;
    std::string value(100, 'x');
    auto t0 = Clock::now();
    double limit = sync ? 3.0 : 2.0;  // run for a fixed time
    while (Secs(t0, Clock::now()) < limit) {
      for (int i = 0; i < 100; i++) db->Put(Key(n++), value);
    }
    double s = Secs(t0, Clock::now());
    std::printf("  fsync=%-5s %10ld ops in %.2fs = %12.0f ops/sec\n", sync ? "on" : "off", n, s, n / s);
    csv << (sync ? "fsync_on" : "fsync_off") << "," << n << "," << s << "," << n / s << "\n";
    db.reset();
    RemoveDir(dir);
  }
}

// ---------- 2. read latency before/after compaction ----------
struct Lat { double mean_us, p50_us, p99_us; };

static Lat MeasureReads(DB& db, const std::vector<std::string>& keys) {
  std::vector<double> t;
  t.reserve(keys.size());
  for (const auto& k : keys) {
    auto a = Clock::now();
    auto v = db.Get(k);
    auto b = Clock::now();
    if (v && v->empty()) std::cerr << "";  // keep the call from being optimised away
    t.push_back(std::chrono::duration<double, std::micro>(b - a).count());
  }
  std::sort(t.begin(), t.end());
  double sum = 0;
  for (double x : t) sum += x;
  return {sum / t.size(), t[t.size() / 2], t[static_cast<size_t>(t.size() * 0.99)]};
}

static void BenchReads(const std::string& work, std::ofstream& csv) {
  std::cout << "\n[2] read latency before / after compaction\n";
  csv << "phase,lookup,sstables,mean_us,p50_us,p99_us\n";
  std::string dir = work + "/reads";
  RemoveDir(dir);

  const long N = 200000;
  Options o;
  o.sync_wal = false;
  o.memtable_limit = 256 * 1024;   // tiny memtable -> many SSTables
  o.compaction_trigger = 100000;   // never compact on its own
  auto db = DB::Open(dir, o);
  std::string value(100, 'x');
  for (long i = 0; i < N; i++) db->Put(Key(i), value);
  db->Flush();
  size_t tables_before = db->NumTables();

  std::mt19937_64 rng(1);
  std::vector<std::string> hits, misses;
  for (int i = 0; i < 20000; i++) {
    hits.push_back(Key(rng() % N));
    misses.push_back(Key(N + 1 + rng() % N));  // keys that were never written
  }

  for (const char* phase : {"before", "after"}) {
    if (std::string(phase) == "after") db->Compact();
    size_t tables = db->NumTables();
    Lat h = MeasureReads(*db, hits);
    Lat m = MeasureReads(*db, misses);
    std::printf("  %-6s compaction (%3zu sstables)  hit: mean %7.1f us p99 %7.1f us | miss: mean %7.1f us p99 %7.1f us\n",
                phase, tables, h.mean_us, h.p99_us, m.mean_us, m.p99_us);
    csv << phase << ",hit," << tables << "," << h.mean_us << "," << h.p50_us << "," << h.p99_us << "\n";
    csv << phase << ",miss," << tables << "," << m.mean_us << "," << m.p50_us << "," << m.p99_us << "\n";
  }
  (void)tables_before;
  db.reset();
  RemoveDir(dir);
}

// ---------- 3. recovery time vs WAL size ----------
static void BenchRecovery(const std::string& work, std::ofstream& csv) {
  std::cout << "\n[3] recovery time vs WAL size\n";
  csv << "wal_mb,records,recovery_seconds\n";
  for (int mb : {1, 10, 100}) {
    std::string dir = work + "/recovery";
    RemoveDir(dir);
    Options o;
    o.sync_wal = false;
    o.memtable_limit = size_t(1) << 30;  // never flush: everything stays in the WAL
    long records = 0;
    {
      auto db = DB::Open(dir, o);
      std::string value(100, 'x');
      // WAL record = 8 header + 9 fixed + key (13) + value (100) = 130 bytes
      long target = static_cast<long>(mb) * 1024 * 1024 / 130;
      while (records < target) db->Put(Key(records++), value);
    }  // closing without Flush == the process went away; the WAL is all that is left
    auto t0 = Clock::now();
    auto db = DB::Open(dir, o);
    double s = Secs(t0, Clock::now());
    std::printf("  WAL %4d MB (%8ld records): recovered in %.3fs\n", mb, records, s);
    csv << mb << "," << records << "," << s << "\n";
    db.reset();
    RemoveDir(dir);
  }
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "usage: bench <work_dir> <out_dir>\n";
    return 2;
  }
  std::string work = argv[1], out = argv[2];
  ::mkdir(work.c_str(), 0755);
  ::mkdir(out.c_str(), 0755);

  std::ofstream c1(out + "/writes.csv"), c2(out + "/reads.csv"), c3(out + "/recovery.csv");
  BenchWrites(work, c1);
  BenchReads(work, c2);
  BenchRecovery(work, c3);
  return 0;
}
