#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace chwp::render {

// LZ4 블록. 원본 크기는 호출하는 쪽이 기억한다. 실패하면 빈 벡터다.
std::vector<std::uint8_t> compressLz4(const std::uint8_t* data, std::size_t size);
std::vector<std::uint8_t> decompressLz4(const std::uint8_t* data, std::size_t size, std::size_t rawSize);

}  // namespace chwp::render
