#pragma once

// MS Compound File Binary (OLE2 구조적 저장소) 읽기 전용 파서. HWP 5.0 의 컨테이너.
// 규격: [MS-CFB] https://learn.microsoft.com/openspecs/windows_protocols/ms-cfb

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/common/bytes.hpp"

namespace chwp::cfb {

enum class EntryType : std::uint8_t { Unknown = 0, Storage = 1, Stream = 2, Root = 5 };

struct Entry {
    std::string name;  // UTF-8
    std::string path;  // "BodyText/Section0" 처럼 '/' 로 구분 (루트는 "")
    EntryType type = EntryType::Unknown;
    std::uint64_t size = 0;
    std::uint32_t startSector = 0;
};

class CompoundFile {
public:
    static bool isCompoundFile(ByteView data);

    // data 는 CompoundFile 이 살아 있는 동안 유효해야 한다.
    bool open(ByteView data, std::string* error = nullptr);

    [[nodiscard]] const std::vector<Entry>& entries() const { return entries_; }
    [[nodiscard]] const Entry* find(std::string_view path) const;

    bool readStream(const Entry& entry, Bytes& out, std::string* error = nullptr) const;
    bool readStream(std::string_view path, Bytes& out, std::string* error = nullptr) const;

private:
    struct DirEntry {
        std::string name;
        EntryType type = EntryType::Unknown;
        std::uint32_t left = 0xFFFFFFFF;
        std::uint32_t right = 0xFFFFFFFF;
        std::uint32_t child = 0xFFFFFFFF;
        std::uint32_t start = 0;
        std::uint64_t size = 0;
    };

    bool readChain(const std::vector<std::uint32_t>& fat, std::uint32_t start, bool mini,
                   std::uint64_t maxBytes, Bytes& out) const;
    bool appendSector(std::uint32_t sector, Bytes& out) const;
    void walk(std::uint32_t index, const std::string& parentPath, int depth,
              std::vector<bool>& visited);

    ByteView data_;
    std::uint32_t sectorSize_ = 512;
    std::uint32_t miniSectorSize_ = 64;
    std::uint32_t miniStreamCutoff_ = 4096;
    std::uint16_t majorVersion_ = 3;
    std::vector<std::uint32_t> fat_;
    std::vector<std::uint32_t> miniFat_;
    Bytes miniStream_;
    std::vector<DirEntry> dir_;
    std::vector<Entry> entries_;
};

// MS Compound File Binary (OLE2 v3, 512바이트 섹터) 작성기. HWP 5.0 내보내기용.
class CompoundWriter {
public:
    void addStream(std::string_view path, ByteView data);
    void addStream(std::string_view path, Bytes&& data);
    bool build(Bytes& out, std::string* error = nullptr) const;

private:
    struct StreamItem {
        std::string path;
        Bytes data;
    };
    std::vector<StreamItem> streams_;
};

// 한글 OLE 묶음(앞 4 바이트 길이 + 복합 파일, 또는 복합 파일)에서 스트림 하나를 읽는다. 없으면 거짓.
bool readOleStream(ByteView ole, std::string_view name, Bytes& out);

}  // namespace chwp::cfb
