#pragma once
// Plain table-driven CRC32 (IEEE polynomial, same as zlib).
#include <array>
#include <cstddef>
#include <cstdint>

namespace durabledb {

namespace detail {
inline const std::array<uint32_t, 256>& Crc32Table() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  return table;
}
}  // namespace detail

inline uint32_t crc32(const void* data, size_t len, uint32_t crc = 0) {
  const auto& table = detail::Crc32Table();
  crc = ~crc;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

}  // namespace durabledb
