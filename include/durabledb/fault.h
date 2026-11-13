#pragma once
// Test hooks for crash and fault injection. They do nothing unless switched on.
//
// 1. CrashPoint(): if the environment variable DURABLEDB_CRASH_AT=N is set, the
//    process kills itself with SIGKILL the N-th time a crash point is reached.
//    Crash points sit at the risky moments (between "SSTable written" and
//    "manifest updated" and so on), so the crash test can stop the engine at
//    exactly the places where ordering mistakes would lose data.
//
// 2. fail_wal_append_after: makes a WAL append fail half way through, so tests
//    can check that the engine stays consistent after an I/O error.
#include <signal.h>
#include <unistd.h>

#include <cstdlib>

namespace durabledb::fault {

inline long& CrashCounter() {
  static long c = 0;
  return c;
}

inline long CrashTarget() {
  static long target = [] {
    const char* e = std::getenv("DURABLEDB_CRASH_AT");
    return e ? std::atol(e) : 0L;
  }();
  return target;
}

inline void CrashPoint() {
  long target = CrashTarget();
  if (target > 0 && ++CrashCounter() == target) ::kill(::getpid(), SIGKILL);
}

// -1 = off. 0 = the next append fails half way. N = N appends succeed first.
inline int& fail_wal_append_after() {
  static int v = -1;
  return v;
}

}  // namespace durabledb::fault
