#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace noting {

using Bytes = std::vector<std::uint8_t>;
using ByteView = std::span<const std::uint8_t>;

// 리틀 엔디언 바이트 리더. 범위를 벗어나면 예외 대신 ok() 가 false 가 되고 0 을 돌려준다.
class ByteReader {
public:
    explicit ByteReader(ByteView data) : data_(data) {}

    [[nodiscard]] std::size_t position() const { return pos_; }
    [[nodiscard]] std::size_t size() const { return data_.size(); }
    [[nodiscard]] std::size_t remaining() const { return data_.size() - pos_; }
    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] bool atEnd() const { return pos_ >= data_.size(); }

    void seek(std::size_t pos);
    void skip(std::size_t count);

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    ByteView bytes(std::size_t count);
    std::u16string utf16(std::size_t codeUnits);

    // HWP 문자열: UINT16 길이(글자 수) + UTF-16LE → UTF-8
    std::string hwpString();

private:
    bool require(std::size_t count);

    ByteView data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

std::uint16_t readU16LE(const std::uint8_t* p);
std::uint32_t readU32LE(const std::uint8_t* p);
void writeU16LE(std::uint8_t* p, std::uint16_t v);
void writeU32LE(std::uint8_t* p, std::uint32_t v);

// 리틀 엔디언 바이트 라이터.
class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(std::size_t reserveBytes) { data_.reserve(reserveBytes); }

    [[nodiscard]] const Bytes& data() const { return data_; }
    [[nodiscard]] Bytes& data() { return data_; }
    [[nodiscard]] ByteView view() const { return data_; }
    [[nodiscard]] std::size_t size() const { return data_.size(); }
    [[nodiscard]] bool empty() const { return data_.empty(); }
    Bytes take() { return std::move(data_); }
    void clear() { data_.clear(); }

    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i8(std::int8_t v) { u8(static_cast<std::uint8_t>(v)); }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f64(double v);

    void bytes(ByteView span);
    void bytes(const void* ptr, std::size_t count);
    void zeros(std::size_t count);
    void utf16(std::u16string_view text);

    // HWP 문자열: UINT16 길이(글자 수) + UTF-16LE
    void hwpString(std::string_view utf8);

private:
    Bytes data_;
};

}  // namespace noting
