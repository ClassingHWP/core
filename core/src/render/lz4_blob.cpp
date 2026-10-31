#include "noting/render/lz4_blob.hpp"

#include "lz4.h"

namespace noting::render {

std::vector<std::uint8_t> compressLz4(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0 || size > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) return {};
    const int source = static_cast<int>(size);
    const int bound = LZ4_compressBound(source);
    if (bound <= 0) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(bound));
    const int written = LZ4_compress_default(reinterpret_cast<const char*>(data), reinterpret_cast<char*>(out.data()),
                                              source, bound);
    if (written <= 0) return {};
    out.resize(static_cast<std::size_t>(written));
    return out;
}

std::vector<std::uint8_t> decompressLz4(const std::uint8_t* data, std::size_t size, std::size_t rawSize) {
    if (!data || size == 0 || rawSize == 0 || size > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE) ||
        rawSize > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
        return {};
    }
    std::vector<std::uint8_t> out(rawSize);
    const int written = LZ4_decompress_safe(reinterpret_cast<const char*>(data), reinterpret_cast<char*>(out.data()),
                                             static_cast<int>(size), static_cast<int>(rawSize));
    if (written != static_cast<int>(rawSize)) return {};
    return out;
}

}  // namespace noting::render
