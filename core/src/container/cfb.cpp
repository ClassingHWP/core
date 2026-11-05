#include "chwp/container/cfb.hpp"

#include <algorithm>
#include <cstring>

#include "chwp/common/unicode.hpp"

namespace chwp::cfb {

namespace {

constexpr std::uint8_t kSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
constexpr std::uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr std::uint32_t kNoStream = 0xFFFFFFFF;
constexpr std::uint32_t kMaxRegSect = 0xFFFFFFFA;
constexpr std::size_t kHeaderDifatCount = 109;
constexpr std::size_t kDirEntrySize = 128;
constexpr int kMaxTreeDepth = 256;

void setError(std::string* error, const char* message) {
    if (error) *error = message;
}

}  // namespace

bool CompoundFile::isCompoundFile(ByteView data) {
    return data.size() >= 512 && std::memcmp(data.data(), kSignature, sizeof(kSignature)) == 0;
}

bool CompoundFile::appendSector(std::uint32_t sector, Bytes& out) const {
    const std::uint64_t offset = (static_cast<std::uint64_t>(sector) + 1) * sectorSize_;
    if (offset >= data_.size()) return false;
    const std::size_t available =
        static_cast<std::size_t>(std::min<std::uint64_t>(sectorSize_, data_.size() - offset));
    out.insert(out.end(), data_.begin() + static_cast<std::ptrdiff_t>(offset),
               data_.begin() + static_cast<std::ptrdiff_t>(offset + available));
    return true;
}

bool CompoundFile::readChain(const std::vector<std::uint32_t>& fat, std::uint32_t start, bool mini,
                             std::uint64_t maxBytes, Bytes& out) const {
    out.clear();
    if (maxBytes > 0 && maxBytes < UINT64_MAX) {
        out.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(maxBytes, data_.size())));
    }
    const std::uint32_t unit = mini ? miniSectorSize_ : sectorSize_;
    std::uint32_t sector = start;
    std::size_t guard = 0;
    while (sector != kEndOfChain && out.size() < maxBytes) {
        if (sector > kMaxRegSect || sector >= fat.size() || ++guard > fat.size()) {
            return false;
        }
        if (mini) {
            const std::uint64_t offset = static_cast<std::uint64_t>(sector) * unit;
            if (offset + unit > miniStream_.size()) return false;
            out.insert(out.end(), miniStream_.begin() + static_cast<std::ptrdiff_t>(offset),
                       miniStream_.begin() + static_cast<std::ptrdiff_t>(offset + unit));
        } else if (!appendSector(sector, out)) {
            return false;
        }
        sector = fat[sector];
    }
    if (out.size() > maxBytes) out.resize(static_cast<std::size_t>(maxBytes));
    return true;
}

bool CompoundFile::open(ByteView data, std::string* error) {
    *this = CompoundFile{};
    data_ = data;
    if (!isCompoundFile(data)) {
        setError(error, "not a compound file (bad signature)");
        return false;
    }

    ByteReader header(data.first(512));
    header.seek(0x1A);
    majorVersion_ = header.u16();
    header.seek(0x1E);
    const std::uint16_t sectorShift = header.u16();
    const std::uint16_t miniSectorShift = header.u16();
    if ((sectorShift != 9 && sectorShift != 12) || miniSectorShift != 6) {
        setError(error, "unsupported sector size");
        return false;
    }
    sectorSize_ = 1u << sectorShift;
    miniSectorSize_ = 1u << miniSectorShift;

    header.seek(0x2C);
    const std::uint32_t numFatSectors = header.u32();
    const std::uint32_t firstDirSector = header.u32();
    header.skip(4);  // transaction signature
    miniStreamCutoff_ = header.u32();
    const std::uint32_t firstMiniFatSector = header.u32();
    const std::uint32_t numMiniFatSectors = header.u32();
    std::uint32_t difatSector = header.u32();
    const std::uint32_t numDifatSectors = header.u32();

    // DIFAT → FAT 섹터 목록
    std::vector<std::uint32_t> fatSectors;
    for (std::size_t i = 0; i < kHeaderDifatCount; ++i) {
        const std::uint32_t s = header.u32();
        if (s <= kMaxRegSect) fatSectors.push_back(s);
    }
    const std::uint32_t perDifat = sectorSize_ / 4 - 1;
    for (std::uint32_t i = 0; i < numDifatSectors && difatSector <= kMaxRegSect; ++i) {
        Bytes sector;
        if (!appendSector(difatSector, sector) || sector.size() < sectorSize_) {
            setError(error, "corrupted DIFAT");
            return false;
        }
        for (std::uint32_t j = 0; j < perDifat; ++j) {
            const std::uint32_t s = readU32LE(sector.data() + j * 4);
            if (s <= kMaxRegSect) fatSectors.push_back(s);
        }
        difatSector = readU32LE(sector.data() + perDifat * 4);
    }
    if (fatSectors.size() > numFatSectors && numFatSectors > 0) {
        fatSectors.resize(numFatSectors);
    }

    for (std::uint32_t s : fatSectors) {
        Bytes sector;
        if (!appendSector(s, sector)) {
            setError(error, "corrupted FAT");
            return false;
        }
        for (std::size_t off = 0; off + 4 <= sector.size(); off += 4) {
            fat_.push_back(readU32LE(sector.data() + off));
        }
    }

    Bytes dirBytes;
    if (!readChain(fat_, firstDirSector, false, UINT64_MAX, dirBytes)) {
        setError(error, "corrupted directory chain");
        return false;
    }
    for (std::size_t off = 0; off + kDirEntrySize <= dirBytes.size(); off += kDirEntrySize) {
        ByteReader r(ByteView(dirBytes).subspan(off, kDirEntrySize));
        std::u16string rawName = r.utf16(32);
        r.seek(64);
        const std::uint16_t nameBytes = r.u16();
        const std::size_t nameLen = nameBytes >= 2 ? std::min<std::size_t>(nameBytes / 2 - 1, 31) : 0;
        rawName.resize(nameLen);

        DirEntry entry;
        entry.name = utf16ToUtf8(rawName);
        entry.type = static_cast<EntryType>(r.u8());
        r.skip(1);  // color
        entry.left = r.u32();
        entry.right = r.u32();
        entry.child = r.u32();
        r.seek(116);
        entry.start = r.u32();
        entry.size = r.u64();
        if (majorVersion_ == 3) entry.size &= 0xFFFFFFFFull;
        dir_.push_back(std::move(entry));
    }
    if (dir_.empty() || dir_[0].type != EntryType::Root) {
        setError(error, "missing root entry");
        return false;
    }

    if (numMiniFatSectors > 0 && firstMiniFatSector <= kMaxRegSect) {
        Bytes miniFatBytes;
        if (!readChain(fat_, firstMiniFatSector, false,
                       static_cast<std::uint64_t>(numMiniFatSectors) * sectorSize_, miniFatBytes)) {
            setError(error, "corrupted mini FAT");
            return false;
        }
        for (std::size_t off = 0; off + 4 <= miniFatBytes.size(); off += 4) {
            miniFat_.push_back(readU32LE(miniFatBytes.data() + off));
        }
    }
    if (dir_[0].size > 0 && !readChain(fat_, dir_[0].start, false, dir_[0].size, miniStream_)) {
        setError(error, "corrupted mini stream");
        return false;
    }

    Entry root;
    root.type = EntryType::Root;
    root.name = dir_[0].name;
    entries_.push_back(root);
    std::vector<bool> visited(dir_.size(), false);
    visited[0] = true;
    walk(dir_[0].child, "", 0, visited);
    return true;
}

void CompoundFile::walk(std::uint32_t index, const std::string& parentPath, int depth,
                        std::vector<bool>& visited) {
    if (index == kNoStream || index >= dir_.size() || visited[index] || depth > kMaxTreeDepth) {
        return;
    }
    visited[index] = true;
    const DirEntry& d = dir_[index];

    walk(d.left, parentPath, depth + 1, visited);

    Entry entry;
    entry.name = d.name;
    entry.path = parentPath.empty() ? d.name : parentPath + "/" + d.name;
    entry.type = d.type;
    entry.size = d.size;
    entry.startSector = d.start;
    entries_.push_back(entry);
    if (d.type == EntryType::Storage) {
        walk(d.child, entry.path, depth + 1, visited);
    }

    walk(d.right, parentPath, depth + 1, visited);
}

const Entry* CompoundFile::find(std::string_view path) const {
    for (const auto& e : entries_) {
        if (e.path == path) return &e;
    }
    return nullptr;
}

bool CompoundFile::readStream(const Entry& entry, Bytes& out, std::string* error) const {
    if (entry.type != EntryType::Stream) {
        setError(error, "entry is not a stream");
        return false;
    }
    if (entry.size == 0) {
        out.clear();
        return true;
    }
    const bool mini = entry.size < miniStreamCutoff_;
    const bool ok = readChain(mini ? miniFat_ : fat_, entry.startSector, mini, entry.size, out);
    if (!ok || out.size() != entry.size) {
        setError(error, "corrupted stream chain");
        return false;
    }
    return true;
}

bool CompoundFile::readStream(std::string_view path, Bytes& out, std::string* error) const {
    const Entry* entry = find(path);
    if (!entry) {
        setError(error, "stream not found");
        return false;
    }
    return readStream(*entry, out, error);
}

namespace {

constexpr std::uint32_t kFreeSect = 0xFFFFFFFF;
constexpr std::uint32_t kFatSect = 0xFFFFFFFD;
constexpr std::uint32_t kDifSect = 0xFFFFFFFC;
constexpr std::uint32_t kSectorSizeV3 = 512;
constexpr std::uint32_t kMiniSectorSizeV3 = 64;
constexpr std::uint32_t kMiniCutoffV3 = 4096;

char16_t toUpperCfb(char16_t ch) {
    if (ch >= u'a' && ch <= u'z') return static_cast<char16_t>(ch - u'a' + u'A');
    return ch;
}

// MS-CFB 2.6.4 Red-Black Tree 이름 비교 규칙:
// 1. UTF-16 코드유닛 길이가 짧은 쪽이 작다.
// 2. 길이가 같으면 대문자 변환 기준 코드유닛 값을 비교한다.
int compareCfbName(std::u16string_view a, std::u16string_view b) {
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char16_t ua = toUpperCfb(a[i]);
        const char16_t ub = toUpperCfb(b[i]);
        if (ua < ub) return -1;
        if (ua > ub) return 1;
    }
    return 0;
}

struct WriteDirNode {
    std::string name;
    std::u16string u16name;
    EntryType type = EntryType::Unknown;
    Bytes data;
    std::vector<std::uint32_t> children;
    std::uint32_t left = kNoStream;
    std::uint32_t right = kNoStream;
    std::uint32_t child = kNoStream;
    std::uint8_t color = 1;  // 1 = Black, 0 = Red
    std::uint32_t startSector = kEndOfChain;
    std::uint32_t size = 0;
};

std::uint32_t buildBalancedBst(std::vector<WriteDirNode>& nodes,
                               const std::vector<std::uint32_t>& sorted,
                               int lo, int hi, int depth, int maxDepth) {
    if (lo > hi) return kNoStream;
    const int mid = lo + (hi - lo) / 2;
    const std::uint32_t idx = sorted[static_cast<std::size_t>(mid)];
    nodes[idx].color = (depth == maxDepth && maxDepth > 0) ? 0 : 1;
    nodes[idx].left = buildBalancedBst(nodes, sorted, lo, mid - 1, depth + 1, maxDepth);
    nodes[idx].right = buildBalancedBst(nodes, sorted, mid + 1, hi, depth + 1, maxDepth);
    return idx;
}

}  // namespace

void CompoundWriter::addStream(std::string_view path, ByteView data) {
    Bytes copy(data.begin(), data.end());
    addStream(path, std::move(copy));
}

void CompoundWriter::addStream(std::string_view path, Bytes&& data) {
    for (auto& s : streams_) {
        if (s.path == path) {
            s.data = std::move(data);
            return;
        }
    }
    streams_.push_back(StreamItem{std::string(path), std::move(data)});
}

bool CompoundWriter::build(Bytes& out, std::string* error) const {
    out.clear();

    std::vector<WriteDirNode> nodes;
    WriteDirNode root;
    root.name = "Root Entry";
    root.u16name = utf8ToUtf16(root.name);
    root.type = EntryType::Root;
    root.startSector = kEndOfChain;
    nodes.push_back(std::move(root));

    // 경로를 트리 구조(Storage / Stream)로 변환
    for (const auto& item : streams_) {
        if (item.path.empty()) continue;
        std::uint32_t current = 0;
        std::size_t pos = 0;
        while (pos < item.path.size()) {
            const std::size_t slash = item.path.find('/', pos);
            const bool isLeaf = (slash == std::string::npos);
            const std::string part = isLeaf ? item.path.substr(pos) : item.path.substr(pos, slash - pos);
            pos = isLeaf ? item.path.size() : slash + 1;
            if (part.empty()) continue;

            const std::u16string u16part = utf8ToUtf16(part);
            if (u16part.size() > 31) {
                setError(error, "CFB entry name exceeds 31 UTF-16 code units");
                return false;
            }

            std::uint32_t found = kNoStream;
            for (std::uint32_t childIdx : nodes[current].children) {
                if (compareCfbName(nodes[childIdx].u16name, u16part) == 0) {
                    found = childIdx;
                    break;
                }
            }

            if (found == kNoStream) {
                WriteDirNode node;
                node.name = part;
                node.u16name = u16part;
                node.type = isLeaf ? EntryType::Stream : EntryType::Storage;
                if (isLeaf) {
                    node.data = item.data;
                    node.size = static_cast<std::uint32_t>(item.data.size());
                }
                found = static_cast<std::uint32_t>(nodes.size());
                nodes.push_back(std::move(node));
                nodes[current].children.push_back(found);
            } else if (isLeaf) {
                nodes[found].type = EntryType::Stream;
                nodes[found].data = item.data;
                nodes[found].size = static_cast<std::uint32_t>(item.data.size());
            }
            current = found;
        }
    }

    // 각 Storage 의 children 을 CFB 이름 규칙에 맞게 정렬하고 균형 이진 탐색 트리 구성
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].children.empty()) continue;
        auto sorted = nodes[i].children;
        std::sort(sorted.begin(), sorted.end(), [&](std::uint32_t a, std::uint32_t b) {
            return compareCfbName(nodes[a].u16name, nodes[b].u16name) < 0;
        });
        int maxDepth = 0;
        for (std::size_t n = sorted.size(); n > 1; n >>= 1) ++maxDepth;
        nodes[i].child = buildBalancedBst(nodes, sorted, 0, static_cast<int>(sorted.size()) - 1, 0, maxDepth);
    }

    // 1. MiniStream (size > 0 && size < 4096 인 Stream) 구성
    Bytes miniStream;
    std::vector<std::uint32_t> miniFat;
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        if (node.type != EntryType::Stream) {
            node.startSector = 0;
            node.size = 0;
            continue;
        }
        if (node.size == 0) {
            node.startSector = kEndOfChain;
            continue;
        }
        if (node.size < kMiniCutoffV3) {
            const std::uint32_t firstMini = static_cast<std::uint32_t>(miniFat.size());
            const std::uint32_t count = (node.size + kMiniSectorSizeV3 - 1) / kMiniSectorSizeV3;
            node.startSector = firstMini;
            for (std::uint32_t m = 0; m < count; ++m) {
                miniFat.push_back((m + 1 < count) ? (firstMini + m + 1) : kEndOfChain);
            }
            const std::size_t oldSize = miniStream.size();
            miniStream.resize(oldSize + static_cast<std::size_t>(count) * kMiniSectorSizeV3, 0);
            std::memcpy(miniStream.data() + oldSize, node.data.data(), node.size);
        }
    }

    // 일반 섹터 버퍼 및 FAT
    Bytes sectors;
    std::vector<std::uint32_t> fat;
    auto appendToSectors = [&](ByteView chunk) -> std::uint32_t {
        if (chunk.empty()) return kEndOfChain;
        const std::uint32_t firstSec = static_cast<std::uint32_t>(fat.size());
        const std::uint32_t count =
            static_cast<std::uint32_t>((chunk.size() + kSectorSizeV3 - 1) / kSectorSizeV3);
        for (std::uint32_t s = 0; s < count; ++s) {
            fat.push_back((s + 1 < count) ? (firstSec + s + 1) : kEndOfChain);
        }
        const std::size_t oldSize = sectors.size();
        sectors.resize(oldSize + static_cast<std::size_t>(count) * kSectorSizeV3, 0);
        std::memcpy(sectors.data() + oldSize, chunk.data(), chunk.size());
        return firstSec;
    };

    // 2. 일반 스트림(size >= 4096) 섹터 할당
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        auto& node = nodes[i];
        if (node.type == EntryType::Stream && node.size >= kMiniCutoffV3) {
            node.startSector = appendToSectors(node.data);
        }
    }

    // 3. Root Entry 의 MiniStream 섹터 할당
    if (!miniStream.empty()) {
        nodes[0].startSector = appendToSectors(miniStream);
        nodes[0].size = static_cast<std::uint32_t>(miniStream.size());
    } else {
        nodes[0].startSector = kEndOfChain;
        nodes[0].size = 0;
    }

    // 4. MiniFAT 섹터 할당
    std::uint32_t firstMiniFatSector = kEndOfChain;
    std::uint32_t numMiniFatSectors = 0;
    if (!miniFat.empty()) {
        const std::uint32_t entriesPerSector = kSectorSizeV3 / 4;
        numMiniFatSectors =
            (static_cast<std::uint32_t>(miniFat.size()) + entriesPerSector - 1) / entriesPerSector;
        const std::size_t totalEntries = static_cast<std::size_t>(numMiniFatSectors) * entriesPerSector;
        Bytes miniFatBytes(totalEntries * 4, 0xFF);
        for (std::size_t i = 0; i < miniFat.size(); ++i) {
            writeU32LE(miniFatBytes.data() + i * 4, miniFat[i]);
        }
        firstMiniFatSector = appendToSectors(miniFatBytes);
    }

    // 5. Directory 엔트리 섹터 할당 (섹터당 4개 = 512바이트)
    const std::size_t dirPerSector = kSectorSizeV3 / kDirEntrySize;
    const std::size_t numDirSectors = (nodes.size() + dirPerSector - 1) / dirPerSector;
    const std::size_t paddedDirCount = numDirSectors * dirPerSector;
    Bytes dirBytes(paddedDirCount * kDirEntrySize, 0);
    for (std::size_t i = 0; i < paddedDirCount; ++i) {
        std::uint8_t* entryPtr = dirBytes.data() + i * kDirEntrySize;
        if (i < nodes.size()) {
            const auto& n = nodes[i];
            const std::size_t copyChars = std::min<std::size_t>(n.u16name.size(), 31);
            for (std::size_t c = 0; c < copyChars; ++c) {
                writeU16LE(entryPtr + c * 2, static_cast<std::uint16_t>(n.u16name[c]));
            }
            writeU16LE(entryPtr + copyChars * 2, 0);
            const std::uint16_t nameByteLen =
                copyChars > 0 ? static_cast<std::uint16_t>((copyChars + 1) * 2) : 0;
            writeU16LE(entryPtr + 64, nameByteLen);
            entryPtr[66] = static_cast<std::uint8_t>(n.type);
            entryPtr[67] = n.color;
            writeU32LE(entryPtr + 68, n.left);
            writeU32LE(entryPtr + 72, n.right);
            writeU32LE(entryPtr + 76, n.child);
            writeU32LE(entryPtr + 116, n.startSector);
            writeU32LE(entryPtr + 120, n.size);
            writeU32LE(entryPtr + 124, 0);
        } else {
            // 미사용 디렉터리 슬롯
            writeU32LE(entryPtr + 68, kNoStream);
            writeU32LE(entryPtr + 72, kNoStream);
            writeU32LE(entryPtr + 76, kNoStream);
        }
    }
    const std::uint32_t firstDirSector = appendToSectors(dirBytes);

    // 6. FAT 및 DIFAT 섹터 계산 및 할당
    const std::uint32_t entriesPerSector = kSectorSizeV3 / 4;     // 128
    const std::uint32_t difatPerSector = entriesPerSector - 1;    // 127
    std::uint32_t numFatSectors = 0;
    std::uint32_t numDifatSectors = 0;
    while (true) {
        const std::uint32_t totalSectors =
            static_cast<std::uint32_t>(fat.size()) + numFatSectors + numDifatSectors;
        const std::uint32_t neededFat =
            (totalSectors + entriesPerSector - 1) / entriesPerSector;
        const std::uint32_t neededDifat =
            neededFat > kHeaderDifatCount
                ? (neededFat - static_cast<std::uint32_t>(kHeaderDifatCount) + difatPerSector - 1) / difatPerSector
                : 0;
        if (neededFat == numFatSectors && neededDifat == numDifatSectors) break;
        numFatSectors = neededFat;
        numDifatSectors = neededDifat;
    }

    std::vector<std::uint32_t> fatSectorIndices;
    fatSectorIndices.reserve(numFatSectors);
    for (std::uint32_t i = 0; i < numFatSectors; ++i) {
        fatSectorIndices.push_back(static_cast<std::uint32_t>(fat.size()));
        fat.push_back(kFatSect);
    }

    std::vector<std::uint32_t> difatSectorIndices;
    difatSectorIndices.reserve(numDifatSectors);
    for (std::uint32_t i = 0; i < numDifatSectors; ++i) {
        difatSectorIndices.push_back(static_cast<std::uint32_t>(fat.size()));
        fat.push_back(kDifSect);
    }

    // FAT 섹터 바이트 직렬화
    Bytes fatSectorBytes(static_cast<std::size_t>(numFatSectors) * kSectorSizeV3, 0xFF);
    for (std::size_t i = 0; i < fat.size(); ++i) {
        writeU32LE(fatSectorBytes.data() + i * 4, fat[i]);
    }
    sectors.insert(sectors.end(), fatSectorBytes.begin(), fatSectorBytes.end());

    // DIFAT 섹터 바이트 직렬화 (FAT 섹터 > 109 개일 때)
    if (numDifatSectors > 0) {
        Bytes difatBytes(static_cast<std::size_t>(numDifatSectors) * kSectorSizeV3, 0xFF);
        std::size_t fatIdx = kHeaderDifatCount;
        for (std::uint32_t d = 0; d < numDifatSectors; ++d) {
            std::uint8_t* secPtr = difatBytes.data() + static_cast<std::size_t>(d) * kSectorSizeV3;
            for (std::uint32_t slot = 0; slot < difatPerSector && fatIdx < fatSectorIndices.size(); ++slot, ++fatIdx) {
                writeU32LE(secPtr + slot * 4, fatSectorIndices[fatIdx]);
            }
            const std::uint32_t nextDifat =
                (d + 1 < numDifatSectors) ? difatSectorIndices[d + 1] : kEndOfChain;
            writeU32LE(secPtr + difatPerSector * 4, nextDifat);
        }
        sectors.insert(sectors.end(), difatBytes.begin(), difatBytes.end());
    }

    // 7. 512바이트 CFB 헤더 작성
    out.assign(kSectorSizeV3, 0);
    std::memcpy(out.data(), kSignature, sizeof(kSignature));
    writeU16LE(out.data() + 0x18, 0x003E);  // Minor version
    writeU16LE(out.data() + 0x1A, 0x0003);  // Major version 3
    writeU16LE(out.data() + 0x1C, 0xFFFE);  // Byte order (little-endian)
    writeU16LE(out.data() + 0x1E, 9);       // Sector shift (1 << 9 = 512)
    writeU16LE(out.data() + 0x20, 6);       // Mini sector shift (1 << 6 = 64)
    writeU32LE(out.data() + 0x28, 0);       // Num directory sectors (0 for v3)
    writeU32LE(out.data() + 0x2C, numFatSectors);
    writeU32LE(out.data() + 0x30, firstDirSector);
    writeU32LE(out.data() + 0x34, 0);       // Transaction signature
    writeU32LE(out.data() + 0x38, kMiniCutoffV3);
    writeU32LE(out.data() + 0x3C, firstMiniFatSector);
    writeU32LE(out.data() + 0x40, numMiniFatSectors);
    writeU32LE(out.data() + 0x44, numDifatSectors > 0 ? difatSectorIndices[0] : kEndOfChain);
    writeU32LE(out.data() + 0x48, numDifatSectors);

    for (std::size_t i = 0; i < kHeaderDifatCount; ++i) {
        const std::uint32_t sec =
            (i < fatSectorIndices.size()) ? fatSectorIndices[i] : kFreeSect;
        writeU32LE(out.data() + 0x4C + i * 4, sec);
    }

    out.insert(out.end(), sectors.begin(), sectors.end());
    return true;
}

}  // namespace chwp::cfb
