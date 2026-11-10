#include "chwp/common/bytes.hpp"

#include <cstring>

#include "chwp/common/unicode.hpp"

namespace chwp {

std::uint16_t readU16LE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t readU32LE(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void writeU16LE(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
}

void writeU32LE(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

bool ByteReader::require(std::size_t count) {
    if (!ok_ || count > remaining()) {
        ok_ = false;
        pos_ = data_.size();
        return false;
    }
    return true;
}

void ByteReader::seek(std::size_t pos) {
    if (pos > data_.size()) {
        ok_ = false;
        pos_ = data_.size();
        return;
    }
    pos_ = pos;
}

void ByteReader::skip(std::size_t count) {
    if (require(count)) {
        pos_ += count;
    }
}

std::uint8_t ByteReader::u8() {
    if (!require(1)) return 0;
    return data_[pos_++];
}

std::uint16_t ByteReader::u16() {
    if (!require(2)) return 0;
    const auto v = readU16LE(data_.data() + pos_);
    pos_ += 2;
    return v;
}

std::uint32_t ByteReader::u32() {
    if (!require(4)) return 0;
    const auto v = readU32LE(data_.data() + pos_);
    pos_ += 4;
    return v;
}

std::uint64_t ByteReader::u64() {
    const std::uint64_t lo = u32();
    const std::uint64_t hi = u32();
    return lo | (hi << 32);
}

ByteView ByteReader::bytes(std::size_t count) {
    if (!require(count)) return {};
    auto view = data_.subspan(pos_, count);
    pos_ += count;
    return view;
}

std::u16string ByteReader::utf16(std::size_t codeUnits) {
    std::u16string out;
    if (codeUnits > remaining() / 2) {
        require(codeUnits * 2);
        return out;
    }
    out.reserve(codeUnits);
    for (std::size_t i = 0; i < codeUnits; ++i) {
        out.push_back(static_cast<char16_t>(u16()));
    }
    return out;
}

std::string ByteReader::hwpString() {
    const std::size_t length = u16();
    return utf16ToUtf8(utf16(length));
}

void ByteWriter::u8(std::uint8_t v) {
    data_.push_back(v);
}

void ByteWriter::u16(std::uint16_t v) {
    data_.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    data_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void ByteWriter::u32(std::uint32_t v) {
    data_.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    data_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    data_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    data_.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void ByteWriter::u64(std::uint64_t v) {
    u32(static_cast<std::uint32_t>(v & 0xFFFFFFFFull));
    u32(static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFull));
}

void ByteWriter::f64(double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    u64(bits);
}

void ByteWriter::bytes(ByteView span) {
    if (!span.empty()) {
        data_.insert(data_.end(), span.begin(), span.end());
    }
}

void ByteWriter::bytes(const void* ptr, std::size_t count) {
    if (ptr && count > 0) {
        const auto* p = static_cast<const std::uint8_t*>(ptr);
        data_.insert(data_.end(), p, p + count);
    }
}

void ByteWriter::zeros(std::size_t count) {
    if (count > 0) {
        data_.resize(data_.size() + count, 0);
    }
}

void ByteWriter::utf16(std::u16string_view text) {
    for (char16_t ch : text) {
        u16(static_cast<std::uint16_t>(ch));
    }
}

void ByteWriter::hwpString(std::string_view utf8) {
    const std::u16string w = utf8ToUtf16(utf8);
    const std::uint16_t len = static_cast<std::uint16_t>(std::min<std::size_t>(w.size(), 0xFFFFu));
    u16(len);
    utf16(std::u16string_view(w.data(), len));
}

bool decodeBase64(std::string_view text, Bytes& out) {
    out.clear();
    if (text.substr(0, 5) == "data:") {
        const std::size_t comma = text.find(',');
        if (comma == std::string_view::npos || text.substr(0, comma).find(";base64") == std::string_view::npos) return false;
        text.remove_prefix(comma + 1);
    }
    std::uint32_t acc = 0;
    int bits = 0;
    out.reserve(text.size() * 3 / 4);
    bool ended = false;
    for (const char ch : text) {
        int v = -1;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else if (ch == '=') {
            ended = true;
            continue;
        } else if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            continue;
        }
        if (v < 0 || ended) return false;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return !out.empty();
}

}  // namespace chwp
