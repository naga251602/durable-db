#pragma once
// The deterministic operation sequence used by the crash test. The writer and
// the verifier both include this, so the verifier can recompute what the
// database must contain after any number of acknowledged operations.
//
//   n % 6 == 5 : Delete key_(n-3)
//   n % 6 == 4 : Put key_(n-4) = "over_n"      (overwrite an old key)
//   otherwise  : Put key_n = "value_n"
#include <map>
#include <string>

struct Op {
  bool del;
  std::string key;
  std::string value;
};

inline Op OpAt(long n) {
  if (n % 6 == 5) return {true, "key_" + std::to_string(n - 3), ""};
  if (n % 6 == 4) return {false, "key_" + std::to_string(n - 4), "over_" + std::to_string(n)};
  return {false, "key_" + std::to_string(n), "value_" + std::to_string(n)};
}

// Expected contents after operations 0..last have been applied.
inline std::map<std::string, std::string> ModelAfter(long last) {
  std::map<std::string, std::string> m;
  for (long n = 0; n <= last; n++) {
    Op op = OpAt(n);
    if (op.del) m.erase(op.key);
    else m[op.key] = op.value;
  }
  return m;
}
