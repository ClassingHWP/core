#pragma once

// 메모리 기반 ZIP 읽기 (HWPX 컨테이너). 구현은 miniz 를 감싼다.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/common/bytes.hpp"

namespace chwp::zip {

struct Entry {
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t compressedSize = 0;
    bool isDirectory = false;
};

class Archive {
public:
    Archive();
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    static bool isZip(ByteView data);

    // data 는 Archive 가 살아 있는 동안 유효해야 한다.
    bool open(ByteView data, std::string* error = nullptr);

    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
    [[nodiscard]] bool contains(std::string_view name) const;
    bool read(std::string_view name, Bytes& out, std::string* error = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<Entry> entries_;
};

// 메모리 기반 ZIP 쓰기 (HWPX 패키징).
class Writer {
public:
    Writer();
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool addFile(std::string_view name, ByteView data, bool compress = true, std::string* error = nullptr);
    bool addText(std::string_view name, std::string_view text, bool compress = true, std::string* error = nullptr);
    bool finalize(Bytes& out, std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// raw deflate(zlib 헤더 없음) 해제 및 압축. HWP 스트림 압축 방식.
bool inflateRaw(ByteView input, Bytes& out);
bool deflateRaw(ByteView input, Bytes& out);

}  // namespace chwp::zip
