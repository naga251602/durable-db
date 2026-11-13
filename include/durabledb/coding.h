#pragma once
// Fixed-width little-endian encoding helpers. Done byte by byte so the on-disk
// format is the same on every platform.
#include <cstdint>
#include <string>

namespace durabledb {

inline void PutU32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

inline void PutU64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; i++) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

inline uint32_t GetU32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

inline uint64_t GetU64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  return v;
}

}  // namespace durabledb
