#include "chwp/reader.hpp"

#include <cctype>
#include <memory>
#include <mutex>

#include "chwp/container/cfb.hpp"
#include "chwp/container/zip.hpp"
#include "chwp/hwp/hwp_reader.hpp"
#include "chwp/hwp/records.hpp"
#include "chwp/hwpx/hwpx_reader.hpp"

namespace chwp {

model::SourceFormat detectFormat(ByteView data) {
    if (cfb::CompoundFile::isCompoundFile(data)) return model::SourceFormat::Hwp;
    if (zip::Archive::isZip(data)) return model::SourceFormat::Hwpx;
    return model::SourceFormat::Unknown;
}

ParseResult parseDocument(ByteView data, const ParseOptions& options) {
    const auto numbered = [](ParseResult result) {
        if (result.ok) model::ensureParagraphIds(result.document);
        return result;
    };
    switch (detectFormat(data)) {
        case model::SourceFormat::Hwp: return numbered(hwp::parse(data, options));
        case model::SourceFormat::Hwpx: return numbered(hwpx::parse(data, options));
        case model::SourceFormat::Unknown: break;
    }
    ParseResult result;
    result.error = "unknown file format (expected HWP or HWPX)";
    return result;
}

namespace {

struct BinCache {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    bool bound = false;
    std::unique_ptr<cfb::CompoundFile> cfb;
    std::unique_ptr<zip::Archive> zip;
};

BinCache gBinCache;
std::mutex gBinMu;

bool samePath(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

void bindBinCache(ByteView file) {
    if (gBinCache.bound && gBinCache.data == file.data() && gBinCache.size == file.size()) return;
    gBinCache = {};
    gBinCache.data = file.data();
    gBinCache.size = file.size();
    gBinCache.bound = true;
    if (file.empty()) return;
    const model::SourceFormat format = detectFormat(file);
    if (format == model::SourceFormat::Hwp) {
        auto cfbFile = std::make_unique<cfb::CompoundFile>();
        if (cfbFile->open(file)) gBinCache.cfb = std::move(cfbFile);
    } else if (format == model::SourceFormat::Hwpx) {
        auto zip = std::make_unique<zip::Archive>();
        if (zip->open(file)) gBinCache.zip = std::move(zip);
    }
}

}  // namespace

void releaseEmbeddedBinCache() {
    std::lock_guard lock(gBinMu);
    gBinCache = {};
}

bool readEmbeddedBin(ByteView file, const model::Document& doc, const model::BinItem& item, Bytes& out) {
    out.clear();
    if (file.empty() || item.path.empty() || item.storage == "LINK") return false;
    // 묶음 안 그림은 여러 스레드가 같이 읽는다. 읽다가 묶음을 바꾸면 그림이 빠진다.
    std::lock_guard lock(gBinMu);
    bindBinCache(file);
    if (gBinCache.cfb) {
        const cfb::Entry* entry = nullptr;
        for (const auto& candidate : gBinCache.cfb->entries()) {
            if (samePath(candidate.path, item.path)) {
                entry = &candidate;
                break;
            }
        }
        if (!entry || entry->type != cfb::EntryType::Stream) return false;
        Bytes raw;
        if (!gBinCache.cfb->readStream(*entry, raw)) return false;
        const bool compressed =
            item.compression == 1 || (item.compression == 0 && doc.format.compressed);
        if (!compressed) {
            out = std::move(raw);
            return !out.empty();
        }
        return hwp::decompressStream(raw, out) && !out.empty();
    }
    if (gBinCache.zip) return gBinCache.zip->read(item.path, out) && !out.empty();
    return false;
}

const char* formatName(model::SourceFormat format) {
    switch (format) {
        case model::SourceFormat::Hwp: return "HWP";
        case model::SourceFormat::Hwpx: return "HWPX";
        case model::SourceFormat::Unknown: break;
    }
    return "Unknown";
}

}  // namespace chwp
