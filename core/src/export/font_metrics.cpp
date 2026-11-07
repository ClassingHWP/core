#include "export/font_metrics.hpp"

#include "chwp/model/document.hpp"
#include "export/layout.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <woff2/decode.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_DRIVER_H
#include FT_MODULE_H
#include FT_OUTLINE_H

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace chwp::layout {
std::vector<std::uint8_t> sfntFromDelivered(std::vector<std::uint8_t> bytes);
namespace {

// 같은 전진폭은 한 번만 둔다. 구간이 한 폭이면 값 하나, 몇 칸만 다르면 그 칸만 예외로 둔다.
struct WidthExcept {
    std::uint16_t index = 0;
    std::uint16_t advance = 0;
};

// 글리프 번호는 글자마다 다르다. 전진폭은 대표 값으로 묶는다.
struct CodeBlock {
    std::vector<std::uint16_t> glyphs;
    std::uint16_t advance = 0;
    bool shared = true;
    std::vector<WidthExcept> excepts;
    std::vector<std::uint8_t> widthOf;
    std::vector<std::uint16_t> palette;
    std::vector<std::uint16_t> rawAdvance;
    bool closed = true;
};

// 한글 음절은 배열이다. 한자 구간은 글꼴이 절반 이상을 가질 때만 배열로 둔다.
// 그 밖은 실제로 있는 코드만 코드포인트 순으로 둔다.
struct GlyphTable {
    CodeBlock hangul;
    CodeBlock cjk;
    std::vector<char32_t> otherCp;
    std::vector<std::uint16_t> otherGlyph;
    std::vector<std::uint16_t> otherAdvance;
    bool ready = false;
    // 드문 코드가 표 한도를 넘기면 그 코드만 cmap을 다시 본다.
    bool overflow = false;
};

// 받은 글꼴 파일 하나. 압축본(WOFF2)은 늘 들고, 푼 TTF 는 열려 있을 때만 든다.
// 접으면 TTF 를 놓고, 다시 필요하면 압축본을 풀어 연다(openData).
struct PackedFont {
    std::shared_ptr<const std::vector<std::uint8_t>> packed;
    std::shared_ptr<const std::vector<std::uint8_t>> open;
    std::int64_t lastUseMs = 0;
};

struct Metrics {
    // 열린 TTF. 접힌 글꼴은 비어 있고 openData 가 다시 채운다. 잠금(gMutex) 안에서만 바꾼다.
    mutable std::shared_ptr<const std::vector<std::uint8_t>> data;
    PackedFont* file = nullptr;
    std::uint16_t unitsPerEm = 0;
    std::int16_t xAvgCharWidth = 0;
    std::uint16_t winAscent = 0;
    std::uint16_t winDescent = 0;
    std::int16_t ascender = 0;
    std::int16_t descender = 0;
    std::uint16_t weight = 400;
    std::uint16_t numHMetrics = 0;
    std::uint32_t hmtx = 0;
    int cmapFormat = 0;
    std::uint32_t cmap = 0;
    std::uint32_t fontOffset = 0;
    GlyphTable glyphs;
};

void buildGlyphTable(Metrics& font);

std::mutex gMutex;
std::unordered_map<std::string, Metrics> gFaces;
std::unordered_map<std::string, std::string> gAlias;
std::unordered_map<std::string, std::string> gBoldName;
std::unordered_set<std::string> gFailed;
std::unordered_set<std::string> gFetchMiss;
// 서버 글꼴 목록의 글꼴 이름 → 파일 이름. 같은 파일은 한 번만 읽는다.
std::unordered_map<std::string, std::string> gFaceFile;
// 라이선스 때문에 파일을 올리지 못한 글꼴 → 그 폴백(font/standin)을 만든 OFL 원본 글꼴(review/pack-web-fonts.mjs).
std::unordered_map<std::string, std::string> gLicenseSwap;
std::unordered_map<std::string, std::string> gFontReplace;
bool gAvailableReady = false;
FontFaceRequestHandler gFontFaceHandler = nullptr;
void* gFontFaceUser = nullptr;
std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> gFileBytes;
// 압축본으로 받은 글꼴 파일. 키는 압축본 주소다. 셸 목록의 파일 이름으로도 찾는다.
std::unordered_map<const void*, std::unique_ptr<PackedFont>> gPacked;
std::unordered_map<std::string, PackedFont*> gFilePacked;

std::int64_t fontNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void appendFontWhy(std::string& out);
void writeFontLog(const std::string& line);

// 글꼴 파일을 쓸 수 있게 연다. 접혀 있으면 압축본을 풀고, 같은 파일을 가리키는 이름들이 함께 쓴다.
bool openData(const Metrics& font) {
    if (font.file) font.file->lastUseMs = fontNowMs();
    if (font.data) return true;
    PackedFont* file = font.file;
    if (!file || !file->packed) return false;
    if (!file->open) {
        std::vector<std::uint8_t> sfnt = sfntFromDelivered(*file->packed);
        if (sfnt.size() < 12) return false;
        file->open = std::make_shared<const std::vector<std::uint8_t>>(std::move(sfnt));
        // 접은 글꼴을 다시 연 이유. 잦으면 접는 기준이 맞지 않는다는 뜻이다.
        std::string note = "{\"글꼴다시열기\":" + std::to_string(file->open->size());
        appendFontWhy(note);
        note += "}\n";
        writeFontLog(note);
        for (auto& [name, bytes] : gFileBytes) {
            const auto packed = gFilePacked.find(name);
            if (packed != gFilePacked.end() && packed->second == file) bytes = file->open;
        }
    }
    font.data = file->open;
    return true;
}
std::unordered_map<std::string, std::filesystem::path> gFiles;
bool gFilesReady = false;

struct FontLoadWhy {
    const char* why = nullptr;
    const char* step = nullptr;
    char32_t cp = 0;
    const std::string* asked = nullptr;
};

thread_local FontLoadWhy gFontWhy;
thread_local int gFontLoadPage = 0;
thread_local double gFontSideMs = 0;

double fontLoadMs(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void appendJsonText(std::string& out, const std::string& text) {
    for (const unsigned char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
}

void appendUtf8Char(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void appendFontWhy(std::string& out) {
    out += ",\"이유\":\"";
    out += gFontWhy.why ? gFontWhy.why : "직접";
    out += '"';
    if (gFontWhy.step && gFontWhy.step[0] != '\0') {
        out += ",\"단계\":\"";
        out += gFontWhy.step;
        out += '"';
    }
    if (gFontLoadPage > 0) {
        out += ",\"쪽\":";
        out += std::to_string(gFontLoadPage);
    }
    if (gFontWhy.cp != 0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), ",\"코드\":%u", static_cast<unsigned>(gFontWhy.cp));
        out += buf;
        if (gFontWhy.cp >= 0x20 && gFontWhy.cp != 0x7F) {
            std::string utf8;
            appendUtf8Char(utf8, gFontWhy.cp);
            out += ",\"글자\":\"";
            appendJsonText(out, utf8);
            out += '"';
        }
    }
}

void writeFontLog(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fflush(stdout);
}

void logFontSide(const char* key, double ms) {
    if (ms < 0.5) return;
    std::string out = "{\"";
    out += key;
    out += "\":true";
    appendFontWhy(out);
    char buf[32];
    std::snprintf(buf, sizeof(buf), ",\"ms\":%.1f}\n", ms);
    out += buf;
    writeFontLog(out);
}

void logFontLoad(const std::string& face, const char* result, double ms) {
    std::string out = "{\"글꼴\":\"";
    appendJsonText(out, face);
    out += '"';
    appendFontWhy(out);
    if (gFontWhy.asked && !gFontWhy.asked->empty() && *gFontWhy.asked != face) {
        out += ",\"요청\":\"";
        appendJsonText(out, *gFontWhy.asked);
        out += '"';
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), ",\"ms\":%.1f,\"결과\":\"%s\"}\n", ms, result);
    out += buf;
    writeFontLog(out);
}

std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t u32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::int16_t i16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(u16(p));
}

bool inRange(const std::vector<std::uint8_t>& data, std::uint32_t offset, std::uint32_t size) {
    return size <= data.size() && offset <= data.size() - size;
}

std::uint32_t tableOffset(const std::uint8_t* dir, std::uint16_t count, const char* tag) {
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* rec = dir + 12 + i * 16;
        if (std::memcmp(rec, tag, 4) == 0) return u32(rec + 8);
    }
    return 0;
}

std::uint32_t tableLength(const std::uint8_t* dir, std::uint16_t count, const char* tag) {
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* rec = dir + 12 + i * 16;
        if (std::memcmp(rec, tag, 4) == 0) return u32(rec + 12);
    }
    return 0;
}

std::string utf16beToUtf8(const std::uint8_t* p, std::uint32_t size) {
    std::string out;
    for (std::uint32_t i = 0; i + 1 < size; i += 2) {
        char32_t cp = u16(p + i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < size) {
            const char32_t low = u16(p + i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

void collectNames(const std::uint8_t* name, std::uint32_t size, std::vector<std::string>& faces) {
    if (size < 6) return;
    const std::uint16_t count = u16(name + 2);
    const std::uint16_t stringOffset = u16(name + 4);
    if (6u + count * 12u > size) return;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint8_t* rec = name + 6 + i * 12;
        const std::uint16_t platform = u16(rec);
        const std::uint16_t nameId = u16(rec + 6);
        const std::uint16_t length = u16(rec + 8);
        const std::uint16_t offset = u16(rec + 10);
        if (nameId != 1 && nameId != 4 && nameId != 16) continue;
        if (static_cast<std::uint32_t>(stringOffset) + offset + length > size) continue;
        const std::uint8_t* str = name + stringOffset + offset;
        std::string face = platform == 3 || platform == 0 ? utf16beToUtf8(str, length)
                                                          : std::string(reinterpret_cast<const char*>(str), length);
        if (!face.empty()) faces.push_back(std::move(face));
    }
}

bool parseMetrics(std::shared_ptr<const std::vector<std::uint8_t>> sharedData, std::uint32_t origin, Metrics& out) {
    if (!sharedData) return false;
    const auto& data = *sharedData;
    if (!inRange(data, origin, 12)) return false;
    const std::uint16_t count = u16(data.data() + origin + 4);
    if (!inRange(data, origin, 12u + count * 16u)) return false;
    const std::uint8_t* dir = data.data() + origin;
    const std::uint32_t head = tableOffset(dir, count, "head");
    const std::uint32_t hhea = tableOffset(dir, count, "hhea");
    const std::uint32_t hmtx = tableOffset(dir, count, "hmtx");
    const std::uint32_t cmap = tableOffset(dir, count, "cmap");
    if (!head || !hhea || !hmtx || !cmap) return false;
    if (!inRange(data, head, 20) || !inRange(data, hhea, 36) || !inRange(data, cmap, 4)) return false;
    out.unitsPerEm = u16(data.data() + head + 18);
    const std::uint32_t os2 = tableOffset(dir, count, "OS/2");
    if (os2 && inRange(data, os2, 4)) out.xAvgCharWidth = i16(data.data() + os2 + 2);
    if (os2 && inRange(data, os2, 6)) {
        const std::uint16_t weight = u16(data.data() + os2 + 4);
        if (weight > 0) out.weight = weight;
    }
    if (os2 && inRange(data, os2, 0x4E)) {
        out.winAscent = u16(data.data() + os2 + 0x4A);
        out.winDescent = u16(data.data() + os2 + 0x4C);
    }
    out.ascender = i16(data.data() + hhea + 4);
    out.descender = i16(data.data() + hhea + 6);
    out.numHMetrics = u16(data.data() + hhea + 34);
    out.hmtx = hmtx;
    if (out.unitsPerEm == 0 || out.numHMetrics == 0) return false;
    if (!inRange(data, hmtx, out.numHMetrics * 4u)) return false;
    const std::uint16_t nEnc = u16(data.data() + cmap + 2);
    if (!inRange(data, cmap, 4u + nEnc * 8u)) return false;
    std::uint32_t chosen = 0;
    int format = 0;
    std::uint32_t symbol = 0;
    for (std::uint16_t i = 0; i < nEnc; ++i) {
        const std::uint8_t* rec = data.data() + cmap + 4 + i * 8;
        const std::uint16_t platform = u16(rec);
        const std::uint16_t encoding = u16(rec + 2);
        const std::uint32_t off = cmap + u32(rec + 4);
        if (!inRange(data, off, 2)) continue;
        const int fmt = u16(data.data() + off);
        if (platform == 3 && encoding == 0 && fmt == 4) symbol = off;
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (!unicode || (fmt != 4 && fmt != 12)) continue;
        if (fmt == 12 || format != 12) {
            chosen = off;
            format = fmt;
        }
    }
    // Wingdings 같은 기호 글꼴은 기호 표(3, 0)만 있다. 코드가 U+F020..F0FF 라 한글이 넘기는 글자 그대로 찾는다.
    if (!chosen && symbol) {
        chosen = symbol;
        format = 4;
    }
    if (!chosen) return false;
    out.cmap = chosen;
    out.cmapFormat = format;
    out.fontOffset = origin;
    out.data = std::move(sharedData);
    buildGlyphTable(out);
    return true;
}

constexpr char32_t kHangulFirst = 0xAC00;
constexpr int kHangulCount = 11172;
constexpr char32_t kHangulLast = kHangulFirst + kHangulCount - 1;
constexpr char32_t kCjkFirst = 0x4E00;
constexpr int kCjkCount = 20992;
constexpr char32_t kCjkLast = kCjkFirst + kCjkCount - 1;
constexpr std::size_t kSparseCap = 65536;

int hmtxAdvance(const Metrics& font, int glyph) {
    if (glyph <= 0 || !font.data) return -1;
    const auto& bytes = *font.data;
    const auto gid = static_cast<std::uint32_t>(glyph);
    const std::uint32_t index = gid < font.numHMetrics ? gid : font.numHMetrics - 1u;
    if (!inRange(bytes, font.hmtx + index * 4u, 2)) return -1;
    return u16(bytes.data() + font.hmtx + index * 4u);
}

int blockAdvance(const CodeBlock& block, std::size_t index) {
    if (block.shared) return block.advance;
    if (!block.rawAdvance.empty()) return block.rawAdvance[index];
    if (!block.widthOf.empty()) return block.palette[block.widthOf[index]];
    const auto slot = static_cast<std::uint16_t>(index);
    const auto found = std::lower_bound(block.excepts.begin(), block.excepts.end(), slot,
                                         [](const WidthExcept& row, std::uint16_t probe) { return row.index < probe; });
    if (found != block.excepts.end() && found->index == slot) return found->advance;
    return block.advance;
}

void packWidths(CodeBlock& block, std::vector<std::uint16_t> advances) {
    if (block.glyphs.empty()) return;
    std::unordered_map<std::uint16_t, int> freq;
    int count = 0;
    for (std::size_t i = 0; i < block.glyphs.size(); ++i) {
        if (block.glyphs[i] == 0) continue;
        ++freq[advances[i]];
        ++count;
    }
    if (count == 0) {
        block.glyphs.clear();
        return;
    }
    std::uint16_t mode = 0;
    int modeCount = 0;
    for (const auto& [width, seen] : freq) {
        if (seen > modeCount) {
            modeCount = seen;
            mode = width;
        }
    }
    block.advance = mode;
    const int exceptions = count - modeCount;
    if (exceptions == 0) {
        block.shared = true;
        return;
    }
    block.shared = false;
    // 예외 목록이 칸마다 번호를 두는 것보다 작을 때만 예외로 남긴다.
    if (static_cast<std::size_t>(exceptions) * 4u <= block.glyphs.size()) {
        block.excepts.reserve(static_cast<std::size_t>(exceptions));
        for (std::size_t i = 0; i < block.glyphs.size(); ++i) {
            if (block.glyphs[i] == 0 || advances[i] == mode) continue;
            block.excepts.push_back(WidthExcept{static_cast<std::uint16_t>(i), advances[i]});
        }
        return;
    }
    if (freq.size() <= 256) {
        std::unordered_map<std::uint16_t, std::uint8_t> index;
        block.palette.reserve(freq.size());
        block.widthOf.assign(block.glyphs.size(), 0);
        for (std::size_t i = 0; i < block.glyphs.size(); ++i) {
            if (block.glyphs[i] == 0) continue;
            const auto known = index.find(advances[i]);
            if (known == index.end()) {
                const auto id = static_cast<std::uint8_t>(block.palette.size());
                block.palette.push_back(advances[i]);
                index.emplace(advances[i], id);
                block.widthOf[i] = id;
            } else {
                block.widthOf[i] = known->second;
            }
        }
        return;
    }
    block.rawAdvance = std::move(advances);
}

struct ScratchRow {
    char32_t cp = 0;
    std::uint16_t glyph = 0;
    std::uint16_t advance = 0;
};

struct Scratch {
    std::vector<std::uint16_t> hangulGlyph;
    std::vector<std::uint16_t> hangulAdv;
    bool hangulClosed = true;
    std::vector<std::uint16_t> cjkGlyph;
    std::vector<std::uint16_t> cjkAdv;
    int cjkCount = 0;
    bool cjkClosed = true;
    std::vector<ScratchRow> other;
    bool overflow = false;
};

void placeMapped(Scratch& scratch, const Metrics& font, std::uint32_t cp, int glyph) {
    if (glyph <= 0) return;
    const int advance = hmtxAdvance(font, glyph);
    if (advance < 0) return;
    if (glyph > 0xFFFF) {
        scratch.overflow = true;
        if (cp >= kHangulFirst && cp <= kHangulLast) scratch.hangulClosed = false;
        if (cp >= kCjkFirst && cp <= kCjkLast) scratch.cjkClosed = false;
        return;
    }
    const auto gid = static_cast<std::uint16_t>(glyph);
    const auto width = static_cast<std::uint16_t>(advance);
    if (cp >= kHangulFirst && cp <= kHangulLast) {
        if (scratch.hangulGlyph.empty()) {
            scratch.hangulGlyph.assign(static_cast<std::size_t>(kHangulCount), 0);
            scratch.hangulAdv.assign(static_cast<std::size_t>(kHangulCount), 0);
        }
        const auto index = static_cast<std::size_t>(cp - kHangulFirst);
        scratch.hangulGlyph[index] = gid;
        scratch.hangulAdv[index] = width;
        return;
    }
    if (cp >= kCjkFirst && cp <= kCjkLast) {
        if (scratch.cjkGlyph.empty()) {
            scratch.cjkGlyph.assign(static_cast<std::size_t>(kCjkCount), 0);
            scratch.cjkAdv.assign(static_cast<std::size_t>(kCjkCount), 0);
        }
        const auto index = static_cast<std::size_t>(cp - kCjkFirst);
        if (scratch.cjkGlyph[index] == 0) ++scratch.cjkCount;
        scratch.cjkGlyph[index] = gid;
        scratch.cjkAdv[index] = width;
        return;
    }
    if (scratch.other.size() >= kSparseCap) {
        scratch.overflow = true;
        return;
    }
    scratch.other.push_back(ScratchRow{static_cast<char32_t>(cp), gid, width});
}

bool stepMapped(std::uint32_t& cp, std::uint32_t last, bool overflow) {
    if (cp >= last || cp >= 0x10FFFF) return false;
    if (!overflow) {
        ++cp;
        return true;
    }
    if (cp < kCjkFirst) {
        cp = kCjkFirst;
        return cp <= last;
    }
    if (cp <= kCjkLast) {
        ++cp;
        return cp <= last;
    }
    if (cp < kHangulFirst) {
        cp = kHangulFirst;
        return cp <= last;
    }
    if (cp <= kHangulLast) {
        ++cp;
        return cp <= last;
    }
    return false;
}

bool walkMappings(const Metrics& font, Scratch& scratch) {
    if (!openData(font)) return false;
    const auto& bytes = *font.data;
    const auto* data = bytes.data();
    if (font.cmapFormat == 12) {
        if (!inRange(bytes, font.cmap, 16)) return false;
        const std::uint32_t groups = u32(data + font.cmap + 12);
        if (!inRange(bytes, font.cmap + 16, groups * 12u)) return false;
        for (std::uint32_t i = 0; i < groups; ++i) {
            const std::uint8_t* rec = data + font.cmap + 16 + i * 12;
            const std::uint32_t start = u32(rec);
            const std::uint32_t end = u32(rec + 4);
            if (start > end || start > 0x10FFFF) continue;
            const std::uint32_t last = std::min(end, 0x10FFFFu);
            const std::uint32_t startGlyph = u32(rec + 8);
            std::uint32_t cp = start;
            while (cp <= last) {
                const std::uint64_t glyph = static_cast<std::uint64_t>(startGlyph) + (cp - start);
                if (glyph > 0xFFFF) {
                    scratch.overflow = true;
                    if (cp >= kHangulFirst && cp <= kHangulLast) scratch.hangulClosed = false;
                    if (cp >= kCjkFirst && cp <= kCjkLast) scratch.cjkClosed = false;
                } else {
                    placeMapped(scratch, font, cp, static_cast<int>(glyph));
                }
                if (!stepMapped(cp, last, scratch.overflow)) break;
            }
        }
        return true;
    }
    if (font.cmapFormat != 4 || !inRange(bytes, font.cmap, 14)) return false;
    const std::uint16_t segCount = u16(data + font.cmap + 6) / 2;
    const std::uint32_t endOff = font.cmap + 14;
    const std::uint32_t startOff = endOff + segCount * 2u + 2u;
    const std::uint32_t deltaOff = startOff + segCount * 2u;
    const std::uint32_t rangeOff = deltaOff + segCount * 2u;
    if (!inRange(bytes, rangeOff, segCount * 2u)) return false;
    for (std::uint16_t i = 0; i < segCount; ++i) {
        const std::uint16_t start = u16(data + startOff + i * 2);
        const std::uint16_t end = u16(data + endOff + i * 2);
        if (start > end || start == 0xFFFF) continue;
        const std::int16_t delta = i16(data + deltaOff + i * 2);
        const std::uint16_t range = u16(data + rangeOff + i * 2);
        std::uint32_t cp = start;
        while (cp <= end) {
            int glyph = 0;
            if (range == 0) {
                glyph = static_cast<int>((cp + static_cast<std::uint16_t>(delta)) & 0xFFFF);
            } else {
                const std::uint32_t glyphOff = rangeOff + i * 2u + range + (cp - start) * 2u;
                if (!inRange(bytes, glyphOff, 2)) return false;
                glyph = u16(data + glyphOff);
            }
            placeMapped(scratch, font, cp, glyph);
            if (!stepMapped(cp, end, scratch.overflow)) break;
        }
    }
    return true;
}

void buildGlyphTable(Metrics& font) {
    Scratch scratch;
    if (!walkMappings(font, scratch)) return;
    if (!scratch.cjkGlyph.empty() && scratch.cjkCount * 2 < kCjkCount) {
        for (int i = 0; i < kCjkCount; ++i) {
            if (scratch.cjkGlyph[static_cast<std::size_t>(i)] == 0) continue;
            if (scratch.other.size() >= kSparseCap) {
                scratch.overflow = true;
                break;
            }
            scratch.other.push_back(ScratchRow{static_cast<char32_t>(kCjkFirst + i),
                                                scratch.cjkGlyph[static_cast<std::size_t>(i)],
                                                scratch.cjkAdv[static_cast<std::size_t>(i)]});
        }
        scratch.cjkGlyph.clear();
        scratch.cjkAdv.clear();
    }
    std::stable_sort(scratch.other.begin(), scratch.other.end(),
                     [](const ScratchRow& left, const ScratchRow& right) { return left.cp < right.cp; });
    std::vector<ScratchRow> rows;
    rows.reserve(scratch.other.size());
    for (const ScratchRow& row : scratch.other) {
        if (!rows.empty() && rows.back().cp == row.cp) rows.back() = row;
        else rows.push_back(row);
    }
    GlyphTable table;
    table.hangul.glyphs = std::move(scratch.hangulGlyph);
    table.hangul.closed = scratch.hangulClosed;
    packWidths(table.hangul, std::move(scratch.hangulAdv));
    table.cjk.glyphs = std::move(scratch.cjkGlyph);
    table.cjk.closed = scratch.cjkClosed;
    packWidths(table.cjk, std::move(scratch.cjkAdv));
    table.otherCp.reserve(rows.size());
    table.otherGlyph.reserve(rows.size());
    table.otherAdvance.reserve(rows.size());
    for (const ScratchRow& row : rows) {
        table.otherCp.push_back(row.cp);
        table.otherGlyph.push_back(row.glyph);
        table.otherAdvance.push_back(row.advance);
    }
    table.overflow = scratch.overflow;
    table.ready = true;
    font.glyphs = std::move(table);
}

enum class MapFind { Hit, Miss, Fallback };

MapFind locateMapped(const Metrics& font, char32_t cp, int& glyph, int& advance) {
    if (!font.glyphs.ready || cp < 0 || cp > 0x10FFFF) return MapFind::Fallback;
    const CodeBlock* block = nullptr;
    int index = 0;
    bool flat = false;
    if (cp >= kHangulFirst && cp <= kHangulLast) {
        block = &font.glyphs.hangul;
        index = static_cast<int>(cp - kHangulFirst);
        flat = true;
    } else if (!font.glyphs.cjk.glyphs.empty() && cp >= kCjkFirst && cp <= kCjkLast) {
        block = &font.glyphs.cjk;
        index = static_cast<int>(cp - kCjkFirst);
        flat = true;
    }
    if (flat) {
        if (block->glyphs.empty() || block->glyphs[static_cast<std::size_t>(index)] == 0) {
            return block->closed ? MapFind::Miss : MapFind::Fallback;
        }
        glyph = block->glyphs[static_cast<std::size_t>(index)];
        advance = blockAdvance(*block, static_cast<std::size_t>(index));
        return MapFind::Hit;
    }
    const auto& cps = font.glyphs.otherCp;
    const auto found = std::lower_bound(cps.begin(), cps.end(), cp);
    if (found == cps.end() || *found != cp) return font.glyphs.overflow ? MapFind::Fallback : MapFind::Miss;
    const auto slot = static_cast<std::size_t>(found - cps.begin());
    glyph = font.glyphs.otherGlyph[slot];
    advance = font.glyphs.otherAdvance[slot];
    return MapFind::Hit;
}

int lookupGlyphCmap(const Metrics& font, char32_t cp) {
    if (cp < 0 || cp > 0x10FFFF || !openData(font)) return -1;
    const auto& bytes = *font.data;
    const auto* data = bytes.data();
    if (font.cmapFormat == 12) {
        if (!inRange(bytes, font.cmap, 16)) return -1;
        const std::uint32_t groups = u32(data + font.cmap + 12);
        if (!inRange(bytes, font.cmap + 16, groups * 12u)) return -1;
        std::uint32_t lo = 0;
        std::uint32_t hi = groups;
        while (lo < hi) {
            const std::uint32_t mid = lo + (hi - lo) / 2;
            const std::uint8_t* rec = data + font.cmap + 16 + mid * 12;
            const std::uint32_t start = u32(rec);
            const std::uint32_t end = u32(rec + 4);
            if (static_cast<std::uint32_t>(cp) < start) hi = mid;
            else if (static_cast<std::uint32_t>(cp) > end) lo = mid + 1;
            else return static_cast<int>(u32(rec + 8) + (static_cast<std::uint32_t>(cp) - start));
        }
        return -1;
    }
    if (font.cmapFormat != 4 || !inRange(bytes, font.cmap, 14)) return -1;
    const std::uint16_t segCount = u16(data + font.cmap + 6) / 2;
    const std::uint32_t endOff = font.cmap + 14;
    const std::uint32_t startOff = endOff + segCount * 2u + 2u;
    const std::uint32_t deltaOff = startOff + segCount * 2u;
    const std::uint32_t rangeOff = deltaOff + segCount * 2u;
    if (!inRange(bytes, rangeOff, segCount * 2u)) return -1;
    std::uint16_t lo = 0;
    std::uint16_t hi = segCount;
    while (lo < hi) {
        const std::uint16_t mid = static_cast<std::uint16_t>(lo + (hi - lo) / 2);
        const std::uint16_t end = u16(data + endOff + mid * 2);
        if (static_cast<std::uint32_t>(cp) > end) lo = static_cast<std::uint16_t>(mid + 1);
        else hi = mid;
    }
    if (lo >= segCount) return -1;
    const std::uint16_t start = u16(data + startOff + lo * 2);
    const std::uint16_t end = u16(data + endOff + lo * 2);
    if (static_cast<std::uint32_t>(cp) < start || static_cast<std::uint32_t>(cp) > end) return -1;
    const std::int16_t delta = i16(data + deltaOff + lo * 2);
    const std::uint16_t range = u16(data + rangeOff + lo * 2);
    if (range == 0) return (static_cast<std::uint32_t>(cp) + static_cast<std::uint16_t>(delta)) & 0xFFFF;
    const std::uint32_t glyphOff = rangeOff + lo * 2u + range + (static_cast<std::uint32_t>(cp) - start) * 2u;
    if (!inRange(bytes, glyphOff, 2)) return -1;
    return u16(data + glyphOff);
}

int lookupGlyph(const Metrics& font, char32_t cp) {
    int glyph = -1;
    int advance = -1;
    switch (locateMapped(font, cp, glyph, advance)) {
    case MapFind::Hit: return glyph;
    case MapFind::Miss: return -1;
    case MapFind::Fallback: return lookupGlyphCmap(font, cp);
    }
    return -1;
}

int designAdvanceOf(const Metrics& font, char32_t cp) {
    int glyph = -1;
    int advance = -1;
    switch (locateMapped(font, cp, glyph, advance)) {
    case MapFind::Hit: return advance;
    case MapFind::Miss: return -1;
    case MapFind::Fallback: break;
    }
    return hmtxAdvance(font, lookupGlyphCmap(font, cp));
}

void emitBlock(const CodeBlock& block, char32_t first, void (*sink)(void*, char32_t, std::int32_t), void* ctx) {
    for (std::size_t i = 0; i < block.glyphs.size(); ++i) {
        if (block.glyphs[i] == 0) continue;
        const int advance = blockAdvance(block, i);
        if (advance >= 0) sink(ctx, static_cast<char32_t>(first + static_cast<char32_t>(i)), advance);
    }
}

void visitDesignAdvances(const Metrics& font, void (*sink)(void*, char32_t, std::int32_t), void* ctx) {
    if (!sink) return;
    if (font.glyphs.ready && !font.glyphs.overflow && font.glyphs.hangul.closed && font.glyphs.cjk.closed) {
        emitBlock(font.glyphs.hangul, kHangulFirst, sink, ctx);
        emitBlock(font.glyphs.cjk, kCjkFirst, sink, ctx);
        for (std::size_t i = 0; i < font.glyphs.otherCp.size(); ++i) {
            sink(ctx, font.glyphs.otherCp[i], font.glyphs.otherAdvance[i]);
        }
        return;
    }
    const auto emit = [&](std::uint32_t cp) {
        if (cp > 0x10FFFF) return;
        const int advance = designAdvanceOf(font, static_cast<char32_t>(cp));
        if (advance >= 0) sink(ctx, static_cast<char32_t>(cp), advance);
    };
    if (!openData(font)) return;
    const auto& bytes = *font.data;
    const auto* data = bytes.data();
    if (font.cmapFormat == 12) {
        if (!inRange(bytes, font.cmap, 16)) return;
        const std::uint32_t groups = u32(data + font.cmap + 12);
        if (!inRange(bytes, font.cmap + 16, groups * 12u)) return;
        for (std::uint32_t i = 0; i < groups; ++i) {
            const std::uint8_t* rec = data + font.cmap + 16 + i * 12;
            const std::uint32_t start = u32(rec);
            const std::uint32_t end = u32(rec + 4);
            if (start > end || start > 0x10FFFF) continue;
            const std::uint32_t last = std::min(end, 0x10FFFFu);
            for (std::uint32_t cp = start; cp <= last; ++cp) emit(cp);
        }
        return;
    }
    if (font.cmapFormat != 4 || !inRange(bytes, font.cmap, 14)) return;
    const std::uint16_t segCount = u16(data + font.cmap + 6) / 2;
    const std::uint32_t endOff = font.cmap + 14;
    const std::uint32_t startOff = endOff + segCount * 2u + 2u;
    if (!inRange(bytes, startOff, segCount * 2u)) return;
    for (std::uint16_t i = 0; i < segCount; ++i) {
        const std::uint16_t start = u16(data + startOff + i * 2);
        const std::uint16_t end = u16(data + endOff + i * 2);
        if (start > end || start == 0xFFFF) continue;
        for (std::uint32_t cp = start; cp <= end; ++cp) emit(cp);
    }
}

std::int32_t advanceOf(const Metrics& font, char32_t cp, std::int32_t em) {
    const int advance = designAdvanceOf(font, cp);
    if (advance < 0) return -1;
    return static_cast<std::int32_t>(static_cast<std::int64_t>(advance) * em / font.unitsPerEm);
}

void remember(std::string face, Metrics metrics) {
    if (face.empty() || gFaces.count(face)) return;
    gFaces.emplace(std::move(face), std::move(metrics));
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!in) return {};
    return data;
}

void indexFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::uint8_t head[12];
    in.read(reinterpret_cast<char*>(head), 12);
    if (!in) return;
    if (std::memcmp(head, "ttcf", 4) == 0) {
        in.seekg(8);
        std::uint8_t countBuf[4];
        in.read(reinterpret_cast<char*>(countBuf), 4);
        const std::uint32_t count = u32(countBuf);
        std::vector<std::uint8_t> offsets(count * 4u);
        in.read(reinterpret_cast<char*>(offsets.data()), static_cast<std::streamsize>(offsets.size()));
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t origin = u32(offsets.data() + i * 4);
            in.seekg(origin);
            std::uint8_t dirHead[12];
            in.read(reinterpret_cast<char*>(dirHead), 12);
            if (!in) return;
            const std::uint16_t tables = u16(dirHead + 4);
            std::vector<std::uint8_t> dir(12u + tables * 16u);
            std::memcpy(dir.data(), dirHead, 12);
            in.read(reinterpret_cast<char*>(dir.data() + 12), tables * 16);
            const std::uint32_t nameOff = tableOffset(dir.data(), tables, "name");
            const std::uint32_t nameLen = tableLength(dir.data(), tables, "name");
            if (!nameOff || nameLen > 1024 * 1024) continue;
            std::vector<std::uint8_t> name(nameLen);
            in.seekg(nameOff);
            in.read(reinterpret_cast<char*>(name.data()), static_cast<std::streamsize>(name.size()));
            std::vector<std::string> faces;
            collectNames(name.data(), nameLen, faces);
            for (auto& face : faces) {
                if (!face.empty() && !gFiles.count(face)) gFiles.emplace(std::move(face), path);
            }
        }
        return;
    }
    in.seekg(0, std::ios::end);
    const auto fileSize = static_cast<std::uint32_t>(in.tellg());
    const std::uint16_t tables = u16(head + 4);
    std::vector<std::uint8_t> dir(12u + tables * 16u);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(dir.data()), static_cast<std::streamsize>(dir.size()));
    if (!in || dir.size() > fileSize) return;
    const std::uint32_t nameOff = tableOffset(dir.data(), tables, "name");
    const std::uint32_t nameLen = tableLength(dir.data(), tables, "name");
    if (!nameOff || nameOff + nameLen > fileSize || nameLen > 1024 * 1024) return;
    std::vector<std::uint8_t> name(nameLen);
    in.seekg(nameOff);
    in.read(reinterpret_cast<char*>(name.data()), static_cast<std::streamsize>(name.size()));
    std::vector<std::string> faces;
    collectNames(name.data(), static_cast<std::uint32_t>(name.size()), faces);
    for (auto& face : faces) {
        if (!face.empty() && !gFiles.count(face)) gFiles.emplace(std::move(face), path);
    }
}

void ensureFileIndex() {
    if (gFilesReady) return;
    gFilesReady = true;
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> dirs;
#ifdef _WIN32
    dirs.push_back(std::filesystem::path(L"C:\\Windows\\Fonts"));
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA")) {
        dirs.push_back(std::filesystem::path(local) / L"Microsoft" / L"Windows" / L"Fonts");
    }
    // 한글이 같이 두는 트루타입. 윈도우에 설치되지 않은 글꼴(양재난초체 등)을 그릴 때 쓴다.
    dirs.push_back(std::filesystem::path(L"C:\\Program Files (x86)\\Hnc\\Office 2024\\HOffice130\\Shared\\TTF\\Hwp"));
    dirs.push_back(std::filesystem::path(L"C:\\Program Files (x86)\\Hnc\\Office 2024\\HOffice130\\Shared\\TTF\\All"));
#endif
    for (const auto& dir : dirs) {
        std::error_code error;
        if (!std::filesystem::is_directory(dir, error)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            if (error) break;
            const auto ext = entry.path().extension().wstring();
            std::wstring lower = ext;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower != L".ttf" && lower != L".otf" && lower != L".ttc") continue;
            indexFile(entry.path());
        }
    }
    const double ms = fontLoadMs(started);
    gFontSideMs += ms;
    logFontSide("글꼴색인", ms);
}

std::string pathKey(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

// 한 파일의 한글 이름과 영문 이름은 같은 글꼴이다. 영문 이름으로 다시 받지 않게 잇는다.
void bindSiblingNames(const std::string& face, const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        if (name.empty() || name == face || gFaces.count(name)) continue;
        const auto existing = gAlias.find(name);
        if (existing != gAlias.end() && !existing->second.empty() && existing->second != face) continue;
        gAlias[name] = face;
        gFailed.erase(name);
        gFetchMiss.erase(name);
    }
}

std::shared_ptr<const std::vector<std::uint8_t>> bytesForPath(const std::filesystem::path& path) {
    const std::string key = pathKey(path);
    if (const auto found = gFileBytes.find(key); found != gFileBytes.end()) return found->second;
    auto data = std::make_shared<const std::vector<std::uint8_t>>(readFile(path));
    if (data->size() < 12) return {};
    gFileBytes.emplace(key, data);
    return data;
}

bool adoptSharedFaceBytes(const std::string& face, std::shared_ptr<const std::vector<std::uint8_t>> bytes) {
    if (!bytes || bytes->size() < 12) return false;
    const auto& raw = *bytes;
    if (std::memcmp(raw.data(), "ttcf", 4) == 0) {
        const std::uint32_t count = u32(raw.data() + 8);
        for (std::uint32_t i = 0; i < count; ++i) {
            if (12u + (i + 1u) * 4u > raw.size()) break;
            const std::uint32_t origin = u32(raw.data() + 12 + i * 4);
            if (!inRange(raw, origin, 12)) continue;
            const std::uint16_t tables = u16(raw.data() + origin + 4);
            if (!inRange(raw, origin, 12u + tables * 16u)) continue;
            std::vector<std::string> faces;
            const std::uint32_t nameOff = tableOffset(raw.data() + origin, tables, "name");
            const std::uint32_t nameLen = tableLength(raw.data() + origin, tables, "name");
            if (nameOff && inRange(raw, nameOff, nameLen)) {
                collectNames(raw.data() + nameOff, nameLen, faces);
            }
            if (std::find(faces.begin(), faces.end(), face) == faces.end()) continue;
            Metrics metrics;
            if (!parseMetrics(bytes, origin, metrics)) continue;
            remember(face, std::move(metrics));
            bindSiblingNames(face, faces);
            return true;
        }
        return false;
    }
    Metrics metrics;
    if (!parseMetrics(bytes, 0, metrics)) return false;
    std::vector<std::string> names;
    if (raw.size() >= 12) {
        const std::uint16_t tables = u16(raw.data() + 4);
        if (inRange(raw, 0, 12u + tables * 16u)) {
            const std::uint32_t nameOff = tableOffset(raw.data(), tables, "name");
            const std::uint32_t nameLen = tableLength(raw.data(), tables, "name");
            if (nameOff && inRange(raw, nameOff, nameLen)) {
                collectNames(raw.data() + nameOff, nameLen, names);
            }
        }
    }
    remember(face, std::move(metrics));
    bindSiblingNames(face, names);
    return gFaces.count(face) != 0;
}

bool loadFaceFromDisk(const std::string& face) {
    ensureFileIndex();
    const auto found = gFiles.find(face);
    if (found == gFiles.end()) return false;
    auto held = bytesForPath(found->second);
    if (!held || held->size() < 12) return false;
    return adoptSharedFaceBytes(face, std::move(held));
}

}  // namespace

FontLoadScope::FontLoadScope(const char* why, char32_t cp, const std::string* asked, bool force) {
    if (!force && gFontWhy.why) return;
    why_ = gFontWhy.why;
    step_ = gFontWhy.step;
    cp_ = gFontWhy.cp;
    asked_ = gFontWhy.asked;
    active_ = true;
    gFontWhy.why = why;
    gFontWhy.step = nullptr;
    gFontWhy.cp = cp;
    gFontWhy.asked = asked;
}

FontLoadScope::~FontLoadScope() {
    if (!active_) return;
    gFontWhy.why = why_;
    gFontWhy.step = step_;
    gFontWhy.cp = cp_;
    gFontWhy.asked = asked_;
}

FontLoadStep::FontLoadStep(const char* step) {
    step_ = gFontWhy.step;
    active_ = true;
    gFontWhy.step = step;
}

FontLoadStep::~FontLoadStep() {
    if (!active_) return;
    gFontWhy.step = step_;
}

void setFontLoadPage(int page) { gFontLoadPage = page; }

namespace {

struct FontWhyFallback {
    bool active = false;
    FontLoadWhy saved{};
    FontWhyFallback(const char* why, char32_t cp, const std::string* asked) {
        if (gFontWhy.why) return;
        active = true;
        saved = gFontWhy;
        gFontWhy.why = why;
        gFontWhy.step = nullptr;
        gFontWhy.cp = cp;
        gFontWhy.asked = asked;
    }
    ~FontWhyFallback() {
        if (active) gFontWhy = saved;
    }
};

}  // namespace

bool adoptFaceBytes(const std::string& face, std::vector<std::uint8_t> bytes) {
    if (bytes.size() < 12) return false;
    return adoptSharedFaceBytes(face, std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
}

std::vector<std::uint8_t> sfntFromDelivered(std::vector<std::uint8_t> bytes) {
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "wOF2", 4) != 0) return bytes;
    const std::size_t outSize = woff2::ComputeWOFF2FinalSize(bytes.data(), bytes.size());
    constexpr std::size_t kMaxFont = 64u * 1024u * 1024u;
    if (outSize == 0 || outSize > kMaxFont) return {};
    std::vector<std::uint8_t> sfnt(outSize);
    if (!woff2::ConvertWOFF2ToTTF(sfnt.data(), sfnt.size(), bytes.data(), bytes.size())) return {};
    return sfnt;
}

void fontMemory(std::size_t& fileBytes, std::size_t& tableBytes, int& faces) {
    std::lock_guard lock(gMutex);
    fileBytes = 0;
    tableBytes = 0;
    faces = static_cast<int>(gFaces.size());
    std::unordered_set<const std::vector<std::uint8_t>*> seen;
    const auto blockBytes = [](const CodeBlock& b) {
        return b.glyphs.capacity() * 2 + b.excepts.capacity() * sizeof(WidthExcept) + b.widthOf.capacity() +
               b.palette.capacity() * 2 + b.rawAdvance.capacity() * 2;
    };
    for (const auto& [name, metrics] : gFaces) {
        if (metrics.data && seen.insert(metrics.data.get()).second) fileBytes += metrics.data->capacity();
        const GlyphTable& t = metrics.glyphs;
        tableBytes += blockBytes(t.hangul) + blockBytes(t.cjk) + t.otherCp.capacity() * 4 + t.otherGlyph.capacity() * 2 +
                      t.otherAdvance.capacity() * 2;
    }
    for (const auto& [file, bytes] : gFileBytes) {
        if (bytes && seen.insert(bytes.get()).second) fileBytes += bytes->capacity();
    }
}

std::size_t packedFontBytes() {
    std::lock_guard lock(gMutex);
    std::size_t total = 0;
    for (const auto& [key, file] : gPacked) total += file->packed ? file->packed->capacity() : 0;
    return total;
}

std::vector<OpenFontFile> openFontFiles() {
    std::lock_guard lock(gMutex);
    std::unordered_map<const PackedFont*, OpenFontFile> files;
    std::unordered_set<const PackedFont*> blocked;
    for (const auto& [name, metrics] : gFaces) {
        if (!metrics.file || !metrics.file->open) continue;
        // 글자 번호·폭 표가 다 차지 않은 글꼴은 cmap 을 다시 읽어야 해서 접지 않는다.
        if (!metrics.glyphs.ready || metrics.glyphs.overflow) blocked.insert(metrics.file);
        OpenFontFile& entry = files[metrics.file];
        entry.key = metrics.file;
        entry.bytes = metrics.file->open->capacity();
        entry.lastUseMs = metrics.file->lastUseMs;
        entry.faces.push_back(name);
    }
    std::vector<OpenFontFile> out;
    for (auto& [file, entry] : files) {
        if (!blocked.count(file)) out.push_back(std::move(entry));
    }
    return out;
}

const void* foldFontFile(const void* key) {
    std::lock_guard lock(gMutex);
    PackedFont* file = nullptr;
    for (auto& [packed, record] : gPacked) {
        if (record.get() == key) file = record.get();
    }
    if (!file || !file->open) return nullptr;
    const void* data = file->open.get();
    for (auto& [name, metrics] : gFaces) {
        if (metrics.file == file) metrics.data.reset();
    }
    for (auto& [name, bytes] : gFileBytes) {
        if (bytes.get() == data) bytes.reset();
    }
    file->open.reset();
    return data;
}

std::int64_t fontClockMs() { return fontNowMs(); }

const void* fontFileKey(const std::string& face) {
    std::lock_guard lock(gMutex);
    auto found = gFaces.find(face);
    if (found == gFaces.end()) {
        const auto alias = gAlias.find(face);
        if (alias == gAlias.end() || alias->second.empty()) return nullptr;
        found = gFaces.find(alias->second);
        if (found == gFaces.end()) return nullptr;
    }
    return found->second.file;
}

void registerFontFace(std::string face, std::vector<std::uint8_t> bytes) {
    // 압축본으로 왔으면 그대로 남겨 둔다. 접은 뒤 다시 열 때 네트워크 없이 푼다.
    std::shared_ptr<const std::vector<std::uint8_t>> packed;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "wOF2", 4) == 0) {
        packed = std::make_shared<const std::vector<std::uint8_t>>(bytes);
    }
    if (!bytes.empty()) bytes = sfntFromDelivered(std::move(bytes));
    std::lock_guard lock(gMutex);
    if (face.empty() || gFaces.count(face)) return;
    std::shared_ptr<const std::vector<std::uint8_t>> held;
    const auto named = gFaceFile.find(face);
    const std::string file = (named != gFaceFile.end() && !named->second.empty()) ? named->second : std::string();
    if (!file.empty()) {
        if (const auto cached = gFileBytes.find(file);
            cached != gFileBytes.end() && cached->second && cached->second->size() >= 12) {
            held = cached->second;
        }
    }
    PackedFont* packedFile = nullptr;
    if (!file.empty()) {
        if (const auto known = gFilePacked.find(file); known != gFilePacked.end()) packedFile = known->second;
    }
    if (!held) {
        if (bytes.size() < 12) return;
        held = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
        if (!file.empty()) gFileBytes[file] = held;
        if (packed) {
            auto record = std::make_unique<PackedFont>();
            record->packed = packed;
            record->open = held;
            record->lastUseMs = fontNowMs();
            packedFile = record.get();
            gPacked.emplace(packed.get(), std::move(record));
            if (!file.empty()) gFilePacked[file] = packedFile;
        }
    }
    if (!adoptSharedFaceBytes(face, std::move(held))) return;
    if (packedFile) {
        if (const auto adopted = gFaces.find(face); adopted != gFaces.end()) adopted->second.file = packedFile;
    }
    gFailed.erase(face);
    gFetchMiss.erase(face);
    gAlias.erase(face);
    // 이 글꼴이 없을 때 굵은 짝 이름("face Bold", "face 볼드")을 대체 글꼴로 적어 두었으면 지운다.
    // 남겨 두면 글꼴이 들어온 뒤에도 굵은 글자를 대체 글꼴로 잰다(편집으로 고른 글꼴은 편집 뒤에 들어온다).
    const std::string prefix = face + " ";
    for (auto it = gAlias.begin(); it != gAlias.end();) {
        if (it->first.compare(0, prefix.size(), prefix) == 0 && it->second != face) {
            it = gAlias.erase(it);
        } else {
            ++it;
        }
    }
}

bool looksLikeFont(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 12) return false;
    return std::memcmp(bytes.data(), "ttcf", 4) == 0 || std::memcmp(bytes.data(), "OTTO", 4) == 0 ||
           std::memcmp(bytes.data(), "\x00\x01\x00\x00", 4) == 0 || std::memcmp(bytes.data(), "true", 4) == 0;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// {"글꼴":"파일"} 에서 이름과 파일 이름을 읽는다. 값이 문자열이 아니면 파일 이름은 비운다.
bool readJsonString(const std::string& json, std::size_t& i, std::string& out) {
    if (i >= json.size() || json[i] != '"') return false;
    ++i;
    out.clear();
    while (i < json.size() && json[i] != '"') {
        if (json[i] != '\\') {
            out.push_back(json[i++]);
            continue;
        }
        if (++i >= json.size()) return false;
        const char escaped = json[i++];
        if (escaped == '"' || escaped == '\\' || escaped == '/') {
            out.push_back(escaped);
        } else if (escaped == 'b') {
            out.push_back('\b');
        } else if (escaped == 'f') {
            out.push_back('\f');
        } else if (escaped == 'n') {
            out.push_back('\n');
        } else if (escaped == 'r') {
            out.push_back('\r');
        } else if (escaped == 't') {
            out.push_back('\t');
        } else if (escaped == 'u' && i + 4 <= json.size()) {
            int value = 0;
            for (int n = 0; n < 4; ++n) {
                const int digit = hexValue(json[i++]);
                if (digit < 0) return false;
                value = (value << 4) | digit;
            }
            appendUtf8(out, static_cast<char32_t>(value));
        } else {
            return false;
        }
    }
    if (i >= json.size() || json[i] != '"') return false;
    ++i;
    return true;
}

void collectFaceFiles(const std::string& json, std::unordered_map<std::string, std::string>& out) {
    std::size_t i = 0;
    while (i < json.size()) {
        if (json[i] != '"') {
            ++i;
            continue;
        }
        std::string key;
        if (!readJsonString(json, i, key)) {
            ++i;
            continue;
        }
        while (i < json.size() &&
               (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) {
            ++i;
        }
        if (i >= json.size() || json[i] != ':') continue;
        ++i;
        while (i < json.size() &&
               (json[i] == ' ' || json[i] == '\n' || json[i] == '\r' || json[i] == '\t')) {
            ++i;
        }
        std::string file;
        if (i < json.size() && json[i] == '"') readJsonString(json, i, file);
        out.emplace(std::move(key), std::move(file));
    }
}

void ensureAvailableFaces() {
    if (gAvailableReady) return;
    gAvailableReady = true;
}

std::shared_ptr<const std::vector<std::uint8_t>> fetchFaceBytes(const std::string& face) {
    if (gFetchMiss.count(face)) return {};
    ensureAvailableFaces();
    const auto named = gFaceFile.find(face);
    if (named == gFaceFile.end()) {
        gFetchMiss.insert(face);
        return {};
    }
    const std::string& file = named->second.empty() ? face : named->second;
    if (const auto cached = gFileBytes.find(file); cached != gFileBytes.end() && cached->second) return cached->second;
    // 접힌 파일은 압축본을 풀어 다시 쓴다.
    if (const auto packed = gFilePacked.find(file); packed != gFilePacked.end()) {
        Metrics probe;
        probe.file = packed->second;
        if (openData(probe)) return probe.data;
    }
    gFetchMiss.insert(face);
    return {};
}

bool loadResolvedFace(const std::string& face) {
    if (face.empty() || gFailed.count(face)) return false;
    if (gFaces.count(face)) return true;
    if (const auto alias = gAlias.find(face); alias != gAlias.end()) {
        if (alias->second.empty()) return false;
        if (gFaces.count(alias->second)) return true;
    }
    const auto started = std::chrono::steady_clock::now();
    const double sideBefore = gFontSideMs;
    const char* result = "없음";
    bool ok = false;
    if (loadFaceFromDisk(face)) {
        ok = true;
        result = "파일";
    } else {
        auto downloaded = fetchFaceBytes(face);
        if (downloaded && looksLikeFont(*downloaded) && adoptSharedFaceBytes(face, std::move(downloaded))) {
            ok = true;
            result = "받기";
            if (const auto named = gFaceFile.find(face); named != gFaceFile.end()) {
                const auto packed = gFilePacked.find(named->second.empty() ? face : named->second);
                const auto adopted = gFaces.find(face);
                if (packed != gFilePacked.end() && adopted != gFaces.end()) adopted->second.file = packed->second;
            }
        } else {
            gFailed.insert(face);
        }
    }
    const double wall = fontLoadMs(started);
    const double side = gFontSideMs - sideBefore;
    logFontLoad(face, result, wall > side ? wall - side : 0);
    return ok;
}

bool faceHasGlyph(const std::string& face, char32_t cp) {
    if (face.empty() || !loadResolvedFace(face)) return false;
    const Metrics* metrics = nullptr;
    if (const auto found = gFaces.find(face); found != gFaces.end()) {
        metrics = &found->second;
    } else if (const auto alias = gAlias.find(face); alias != gAlias.end() && !alias->second.empty()) {
        if (const auto aliased = gFaces.find(alias->second); aliased != gFaces.end()) metrics = &aliased->second;
    }
    if (!metrics) return false;
    return lookupGlyph(*metrics, cp) > 0;
}

bool isPrivateUse(char32_t cp) {
    return (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) ||
           (cp >= 0x100000 && cp <= 0x10FFFD);
}

bool contains(const std::string& face, const char* needle) {
    return face.find(needle) != std::string::npos;
}

void appendFamilyFallbacks(const std::string& face, std::vector<const char*>& out) {
    const bool gothic = contains(face, "돋움") || contains(face, "굴림") || contains(face, "고딕") ||
                        contains(face, "Dotum") || contains(face, "Gulim") || contains(face, "Gothic");
    const bool serif = contains(face, "바탕") || contains(face, "명조") || contains(face, "궁서") ||
                       contains(face, "Batang");
    // 없는 글리프의 대체는 함초롬에서 끝낸다. 문서가 지목한 글꼴 파일은 한글 TTF 폴더에 있으면 그대로 쓴다.
    if (gothic) out.push_back("함초롬돋움");
    if (serif) out.push_back("함초롬바탕");
}

const Metrics* metricsFor(const std::string& face) {
    if (const auto alias = gAlias.find(face); alias != gAlias.end()) {
        if (alias->second.empty()) return nullptr;
        const auto found = gFaces.find(alias->second);
        if (found != gFaces.end()) return &found->second;
    }
    if (loadResolvedFace(face)) {
        const auto found = gFaces.find(face);
        if (found != gFaces.end()) return &found->second;
    }
    for (const char* fallback : {"함초롬바탕", "맑은 고딕", "Malgun Gothic", "바탕", "돋움"}) {
        if (face == fallback) continue;
        const FontLoadStep step("없는글꼴대체");
        if (!loadResolvedFace(fallback)) continue;
        gAlias[face] = fallback;
        const auto found = gFaces.find(fallback);
        if (found != gFaces.end()) return &found->second;
    }
    gAlias.emplace(face, std::string());
    return nullptr;
}

void setFontFaceRequestHandler(FontFaceRequestHandler handler, void* user) {
    std::lock_guard lock(gMutex);
    gFontFaceHandler = handler;
    gFontFaceUser = user;
}

void requestFontFaces(const std::vector<std::string>& faces) {
    FontFaceRequestHandler handler = nullptr;
    void* user = nullptr;
    {
        std::lock_guard lock(gMutex);
        handler = gFontFaceHandler;
        user = gFontFaceUser;
    }
    if (!handler || faces.empty()) return;
    std::vector<const char*> ptrs;
    ptrs.reserve(faces.size());
    for (const std::string& face : faces) ptrs.push_back(face.c_str());
    handler(user, ptrs.data(), static_cast<int>(ptrs.size()));
}

void requestDocumentFontFaces(const model::Document& doc) { requestFontFaces(documentFontFaces(doc)); }

void setFontCatalog(const std::string& json) {
    std::lock_guard lock(gMutex);
    gFaceFile.clear();
    collectFaceFiles(json, gFaceFile);
    gFetchMiss.clear();
    gAvailableReady = true;
}

namespace {

// 셸 목록에서 metrics/ 는 폭만 재 둔 이름이고, full/ 은 글꼴 파일을 올린 이름이다.
bool catalogHasFile(const std::string& path) { return path.rfind("full/", 0) == 0; }

// standin/ 은 막힌 글꼴 이름에 OFL 대역 파일을 붙인 것이다. 보기는 대역으로 하고 편집할 때 바꾼다.
bool catalogBlocked(const std::string& face) {
    const auto found = gFaceFile.find(face);
    if (found == gFaceFile.end()) return false;
    return found->second.rfind("metrics/", 0) == 0 || found->second.rfind("standin/", 0) == 0;
}

}  // namespace

void setFontLicenseSwaps(const std::string& json) {
    std::lock_guard lock(gMutex);
    gLicenseSwap.clear();
    collectFaceFiles(json, gLicenseSwap);
}

void setFontReplacements(const std::string& json) {
    std::lock_guard lock(gMutex);
    gFontReplace.clear();
    collectFaceFiles(json, gFontReplace);
}

std::string fontReplacement(const std::string& face) {
    std::lock_guard lock(gMutex);
    const auto found = gFontReplace.find(face);
    return found == gFontReplace.end() ? std::string() : found->second;
}

std::string fontCatalogPath(const std::string& face) {
    std::lock_guard lock(gMutex);
    const auto found = gFaceFile.find(face);
    return found == gFaceFile.end() ? std::string() : found->second;
}

bool fontLicenseBlocked(const std::string& face) {
    std::lock_guard lock(gMutex);
    return catalogBlocked(face);
}

bool fontFaceAvailable(const std::string& face) {
    std::lock_guard lock(gMutex);
    return loadResolvedFace(face);
}

std::string fontLicenseSwapTarget(const std::string& face) {
    std::lock_guard lock(gMutex);
    if (!catalogBlocked(face)) return {};
    if (const auto found = gLicenseSwap.find(face); found != gLicenseSwap.end()) {
        const auto file = gFaceFile.find(found->second);
        if (file != gFaceFile.end() && catalogHasFile(file->second)) return found->second;
    }
    // 표에 없으면 함초롬 중 가까운 쪽. 폴백이 없는 윈도우 글꼴과 원본이 하나가 아닌 폴백이 여기로 온다.
    std::vector<const char*> family;
    appendFamilyFallbacks(face, family);
    return family.empty() ? std::string("함초롬바탕") : std::string(family.front());
}

bool fontPickable(const std::string& face) {
    std::lock_guard lock(gMutex);
    const auto found = gFaceFile.find(face);
    return found != gFaceFile.end() && catalogHasFile(found->second);
}

std::vector<std::string> pickableFonts() {
    std::lock_guard lock(gMutex);
    // 한글처럼 한글 이름을 가나다순으로 먼저, 그다음 나머지를 ABC순으로 둔다. 영문은 대소문자를 가리지 않는다.
    // UTF-8 바이트 차례는 코드포인트 차례와 같고, 한글 음절 코드포인트는 가나다순이다.
    const auto hangulFirst = [](const std::string& face) {
        if (face.size() < 3) return false;
        const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(face[at]); };
        const std::uint32_t code = ((byte(0) & 0x0Fu) << 12) | ((byte(1) & 0x3Fu) << 6) | (byte(2) & 0x3Fu);
        return (byte(0) & 0xF0u) == 0xE0u && ((code >= 0xAC00 && code <= 0xD7A3) || (code >= 0x3131 && code <= 0x318E));
    };
    struct Row {
        bool hangul;
        std::string key;
        const std::string* face;
    };
    std::vector<Row> rows;
    rows.reserve(gFaceFile.size());
    for (const auto& [face, file] : gFaceFile) {
        if (!catalogHasFile(file)) continue;
        std::string key = face;
        for (char& ch : key) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        rows.push_back({hangulFirst(face), std::move(key), &face});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& left, const Row& right) {
        if (left.hangul != right.hangul) return left.hangul;
        return left.key != right.key ? left.key < right.key : *left.face < *right.face;
    });
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const Row& row : rows) out.push_back(*row.face);
    return out;
}

std::vector<FontSwap> licenseFontSwaps(const model::Document& doc) {
    std::vector<FontSwap> out;
    auto add = [&](const std::string& face) {
        if (face.empty()) return;
        for (const FontSwap& swap : out) {
            if (swap.from == face) return;
        }
        std::string target = fontLicenseSwapTarget(face);
        if (!target.empty()) out.push_back(FontSwap{face, std::move(target)});
    };
    for (const auto& group : doc.header.fonts) {
        for (const auto& font : group) {
            add(font.face);
            add(font.subst);
        }
    }
    return out;
}

void warmDocumentFonts(const model::Document& doc) {
    std::lock_guard lock(gMutex);
    auto take = [&](const std::string& name) {
        if (name.empty()) return;
        const FontLoadScope scope("문서글꼴", 0, &name);
        loadResolvedFace(name);
    };
    for (const auto& group : doc.header.fonts) {
        for (const auto& font : group) {
            take(font.face);
            take(font.subst);
        }
    }
    for (const char* fallback : {"함초롬바탕", "함초롬돋움", "맑은 고딕", "바탕", "돋움"}) {
        take(fallback);
    }
}

const Metrics* boldCompanion(const std::string& face) {
    if (const auto named = gBoldName.find(face); named != gBoldName.end()) {
        if (named->second.empty()) return nullptr;
        const auto found = gFaces.find(named->second);
        return found == gFaces.end() ? nullptr : &found->second;
    }
    for (const char* tail : {" Bold", " 볼드"}) {
        const std::string name = face + tail;
        if (!loadResolvedFace(name)) continue;
        const auto found = gFaces.find(name);
        if (found == gFaces.end() || found->second.weight < 600) continue;
        gBoldName.emplace(face, name);
        return &found->second;
    }
    gBoldName.emplace(face, std::string());
    return nullptr;
}

bool fontFaceBytes(const std::string& face, std::shared_ptr<const std::vector<std::uint8_t>>& out, int& fontOffset) {
    if (face.empty()) return false;
    const FontWhyFallback why("그리기", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics || !openData(*metrics) || metrics->data->empty()) return false;
    out = metrics->data;
    fontOffset = static_cast<int>(metrics->fontOffset);
    return true;
}

bool fontOutlineBytes(const std::string& face, bool bold, std::shared_ptr<const std::vector<std::uint8_t>>& out,
                      int& fontOffset, bool& designedBold) {
    designedBold = false;
    if (face.empty()) return false;
    const FontWhyFallback why("윤곽", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* regular = metricsFor(face);
    if (!regular) return false;
    const Metrics* chosen = regular;
    if (bold) {
        if (regular->weight >= 600) {
            designedBold = true;
        } else if (const Metrics* heavy = boldCompanion(face)) {
            chosen = heavy;
            designedBold = true;
        }
    }
    if (!openData(*chosen) || chosen->data->empty()) return false;
    out = chosen->data;
    fontOffset = static_cast<int>(chosen->fontOffset);
    return true;
}

bool fontDesignedBold(const std::string& face) {
    if (face.empty()) return false;
    const FontWhyFallback why("굵기", 0, &face);
    std::lock_guard lock(gMutex);
    // 접힌 글꼴도 올라온 글꼴이다. 굵기는 표에서 읽은 값이라 파일을 열지 않는다.
    const Metrics* regular = metricsFor(face);
    if (!regular) return false;
    if (regular->weight >= 600) return true;
    return boldCompanion(face) != nullptr;
}

bool fontHasGlyph(const std::string& face, char32_t codePoint) {
    const FontWhyFallback why("글리프확인", codePoint, &face);
    std::lock_guard lock(gMutex);
    return faceHasGlyph(face, codePoint);
}

std::string fontWithGlyph(const std::string& face, char32_t codePoint, const std::string& subst,
                          const std::string& base) {
    const FontWhyFallback why("글리프", codePoint, &face);
    std::lock_guard lock(gMutex);
    if (faceHasGlyph(face, codePoint)) return face;
    std::vector<std::string> names;
    auto push = [&](const std::string& name) {
        if (name.empty() || name == face) return;
        if (std::find(names.begin(), names.end(), name) != names.end()) return;
        names.push_back(name);
    };
    push(subst);
    push(base);
    // 사용자 영역만 같은 계열 함초롬으로 넘긴다. 그 밖은 기존처럼 함초롬바탕 폭을 유지한다.
    // 함초롬에도 없으면 한글처럼 Wingdings 로 넘긴다. 글머리표 U+F09F(작은 원)가 여기 있다.
    // 웹은 Wingdings 대신 그 자리에 같은 크기의 원만 넣은 글꼴을 받는다.
    if (isPrivateUse(codePoint)) {
        std::vector<const char*> family;
        appendFamilyFallbacks(face, family);
        for (const char* name : family) push(name);
        for (const char* name : {"함초롬돋움", "함초롬바탕", "Wingdings"}) push(name);
    }
    for (const auto& name : names) {
        const FontLoadStep step("글리프대체");
        if (faceHasGlyph(name, codePoint)) return name;
    }
    return face;
}

std::int32_t fontGlyphAdvance(const std::string& face, char32_t codePoint, std::int32_t em) {
    if (face.empty() || em <= 0) return -1;
    const FontWhyFallback why("폭", codePoint, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics) return -1;
    return advanceOf(*metrics, codePoint, em);
}

std::int32_t fontGlyphDesignAdvance(const std::string& face, char32_t codePoint) {
    if (face.empty()) return -1;
    const FontWhyFallback why("폭", codePoint, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics) return -1;
    return designAdvanceOf(*metrics, codePoint);
}

std::uint16_t fontUnitsPerEm(const std::string& face) {
    if (face.empty()) return 0;
    const FontWhyFallback why("폭", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics || metrics->unitsPerEm == 0) return 0;
    return metrics->unitsPerEm;
}

std::int32_t fontXAvgCharWidth(const std::string& face) {
    if (face.empty()) return 0;
    const FontWhyFallback why("폭", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics || metrics->xAvgCharWidth <= 0) return 0;
    return metrics->xAvgCharWidth;
}

std::int32_t fontWinCell(const std::string& face) {
    if (face.empty()) return 0;
    const FontWhyFallback why("폭", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics) return 0;
    const std::int32_t cell = static_cast<std::int32_t>(metrics->winAscent) + metrics->winDescent;
    return cell > 0 ? cell : 0;
}

bool fontCopyDesignAdvances(const std::string& face, std::uint16_t& unitsPerEm, std::int32_t& xAvgCharWidth,
                            std::int32_t& winCell, void (*sink)(void* ctx, char32_t cp, std::int32_t advance),
                            void* ctx) {
    unitsPerEm = 0;
    xAvgCharWidth = 0;
    winCell = 0;
    if (face.empty() || !sink) return false;
    const FontWhyFallback why("폭", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics || metrics->unitsPerEm == 0) return false;
    unitsPerEm = metrics->unitsPerEm;
    xAvgCharWidth = metrics->xAvgCharWidth > 0 ? metrics->xAvgCharWidth : 0;
    const std::int32_t cell = static_cast<std::int32_t>(metrics->winAscent) + metrics->winDescent;
    winCell = cell > 0 ? cell : 0;
    visitDesignAdvances(*metrics, sink, ctx);
    return true;
}

namespace {

// 정사각 크기 진행폭을 FreeType 으로 재는 글꼴 하나. 글꼴 바이트를 붙잡아 두어 FT_Face 가 그 바이트를 계속 본다.
struct GridFace {
    std::shared_ptr<const std::vector<std::uint8_t>> data;
    FT_Face face = nullptr;
    FT_UInt ppem = 0;
    bool failed = false;
    std::unordered_map<std::uint64_t, std::int32_t> advance;  // (ppem << 32) | 글리프
};

FT_Library gGridLibrary = nullptr;
std::unordered_map<const Metrics*, GridFace> gGridFaces;

const std::uint8_t* fontDirectory(const Metrics& font, std::uint16_t& count) {
    count = 0;
    if (!font.data || !inRange(*font.data, font.fontOffset, 12)) return nullptr;
    const std::uint8_t* dir = font.data->data() + font.fontOffset;
    count = u16(dir + 4);
    if (!inRange(*font.data, font.fontOffset, 12u + count * 16u)) {
        count = 0;
        return nullptr;
    }
    return dir;
}

// 글꼴 원본인지. 윤곽(glyf)이나 내장 비트맵(EBLC)이 있어야 GDI 처럼 잴 수 있다. 폭 정보 파일에는 없다.
bool hasGlyphPrograms(const Metrics& font) {
    std::uint16_t count = 0;
    const std::uint8_t* dir = fontDirectory(font, count);
    return dir && (tableOffset(dir, count, "glyf") != 0 || tableOffset(dir, count, "EBLC") != 0);
}

// TTC 안 글꼴의 번호. FreeType 은 오프셋이 아니라 번호로 연다.
FT_Long collectionIndex(const Metrics& font) {
    const auto& bytes = *font.data;
    if (font.fontOffset == 0 || !inRange(bytes, 0, 12) || std::memcmp(bytes.data(), "ttcf", 4) != 0) return 0;
    const std::uint32_t count = u32(bytes.data() + 8);
    for (std::uint32_t i = 0; i < count && inRange(bytes, 12 + i * 4, 4); ++i) {
        if (u32(bytes.data() + 12 + i * 4) == font.fontOffset) return static_cast<FT_Long>(i);
    }
    return 0;
}

// GDI 는 정사각 크기에서 TrueType 힌팅 명령을 돌린 진행폭을 쓰고, 그 크기에 내장 비트맵이 있으면 비트맵 폭을 쓴다.
// FreeType 의 v35 실행기가 같은 값을 낸다(HancomEQN·HYhwpEQ 5~300px 41만여 칸, 한컴바탕 기호 모두 GDI 와 같다).
// 기본 실행기(v40)는 가로 힌팅을 하지 않아 HancomEQN 에서 6천여 칸이 다르다.
// remember 가 거짓이면 잰 값을 담아 두지 않는다. 표를 만들 때는 칸이 수백만이라 담지 않는다.
std::int32_t hintedAdvance(const Metrics& font, int glyph, std::int32_t ppem, bool remember) {
    if (glyph <= 0 || ppem <= 0 || ppem > 0xFFFF || !openData(font) || !hasGlyphPrograms(font)) return -1;
    GridFace& grid = gGridFaces[&font];
    const std::uint64_t key = (static_cast<std::uint64_t>(ppem) << 32) | static_cast<std::uint32_t>(glyph);
    if (const auto found = grid.advance.find(key); found != grid.advance.end()) return found->second;
    if (grid.failed) return -1;
    if (!grid.face) {
        if (!gGridLibrary) {
            if (FT_Init_FreeType(&gGridLibrary) != 0) {
                gGridLibrary = nullptr;
                grid.failed = true;
                return -1;
            }
            FT_UInt version = TT_INTERPRETER_VERSION_35;
            FT_Property_Set(gGridLibrary, "truetype", "interpreter-version", &version);
        }
        grid.data = font.data;
        if (FT_New_Memory_Face(gGridLibrary, grid.data->data(), static_cast<FT_Long>(grid.data->size()),
                               collectionIndex(font), &grid.face) != 0) {
            grid.face = nullptr;
            grid.failed = true;
            return -1;
        }
    }
    if (grid.ppem != static_cast<FT_UInt>(ppem)) {
        if (FT_Set_Pixel_Sizes(grid.face, 0, static_cast<FT_UInt>(ppem)) != 0) return -1;
        grid.ppem = static_cast<FT_UInt>(ppem);
    }
    std::int32_t px = -1;
    if (FT_Load_Glyph(grid.face, static_cast<FT_UInt>(glyph), FT_LOAD_DEFAULT) == 0) {
        px = static_cast<std::int32_t>((grid.face->glyph->advance.x + 32) >> 6);
    }
    if (remember) grid.advance.emplace(key, px);
    return px;
}

// 폭 정보 파일의 CHgw 표: u16 판, u16 다룬 가장 큰 ppem, u32 칸 수, 칸마다 u32 코드 · u16 ppem · u16 진행폭(픽셀).
// 칸은 (코드, ppem) 오름차순이다. 힌팅 없는 식과 다른 칸만 든다.
constexpr std::uint32_t kGridHeader = 8;
constexpr std::uint32_t kGridCell = 8;

bool hasGridTable(const Metrics& font) {
    std::uint16_t count = 0;
    const std::uint8_t* dir = fontDirectory(font, count);
    return dir && tableOffset(dir, count, "CHgw") != 0;
}

std::int32_t gridTableAdvance(const Metrics& font, char32_t cp, std::int32_t ppem) {
    if (!openData(font)) return -1;
    std::uint16_t count = 0;
    const std::uint8_t* dir = fontDirectory(font, count);
    if (!dir) return -1;
    const std::uint32_t table = tableOffset(dir, count, "CHgw");
    const std::uint32_t length = tableLength(dir, count, "CHgw");
    const auto& bytes = *font.data;
    if (!table || length < kGridHeader || !inRange(bytes, table, length)) return -1;
    const std::uint8_t* p = bytes.data() + table;
    if (u16(p) != 1 || ppem > u16(p + 2)) return -1;
    const std::uint32_t cells = std::min(u32(p + 4), (length - kGridHeader) / kGridCell);
    const std::uint64_t want = (static_cast<std::uint64_t>(cp) << 16) | static_cast<std::uint16_t>(ppem);
    std::uint32_t lo = 0;
    std::uint32_t hi = cells;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        const std::uint8_t* cell = p + kGridHeader + mid * kGridCell;
        const std::uint64_t at = (static_cast<std::uint64_t>(u32(cell)) << 16) | u16(cell + 4);
        if (at == want) return u16(cell + 6);
        if (at < want) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

}  // namespace

std::int32_t fontGridAdvancePx(const std::string& face, char32_t cp, std::int32_t ppem) {
    if (face.empty() || ppem <= 0) return -1;
    const FontWhyFallback why("폭", cp, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics || !openData(*metrics)) return -1;
    // 수식 폴백 글꼴처럼 윤곽은 있어도 원래 글꼴의 힌팅이 없는 파일은 원래 글꼴로 잰 CHgw 표를 따른다.
    if (hasGlyphPrograms(*metrics) && !hasGridTable(*metrics)) {
        return hintedAdvance(*metrics, lookupGlyph(*metrics, cp), ppem, true);
    }
    return gridTableAdvance(*metrics, cp, ppem);
}

std::int32_t fontHintedAdvancePx(const std::string& face, char32_t cp, std::int32_t ppem) {
    if (face.empty() || ppem <= 0) return -1;
    const FontWhyFallback why("폭", cp, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics) return -1;
    return hintedAdvance(*metrics, lookupGlyph(*metrics, cp), ppem, false);
}

namespace {

// 원본 윤곽의 상자. 힌팅·크기 없이 디자인 단위로 연다. 윤곽이 없거나 비면 거짓.
bool outlineInkBox(const Metrics& font, int glyph, FontInkBox& box) {
    if (glyph <= 0 || !openData(font) || !hasGlyphPrograms(font)) return false;
    hintedAdvance(font, glyph, 16, false);  // FT_Face 를 연다
    GridFace& grid = gGridFaces[&font];
    if (!grid.face) return false;
    if (FT_Load_Glyph(grid.face, static_cast<FT_UInt>(glyph), FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
        return false;
    const FT_Outline& outline = grid.face->glyph->outline;
    if (grid.face->glyph->format != FT_GLYPH_FORMAT_OUTLINE || outline.n_points <= 0) return false;
    FT_BBox bbox{};
    FT_Outline_Get_CBox(&outline, &bbox);
    box.xMin = static_cast<std::int32_t>(bbox.xMin);
    box.yMin = static_cast<std::int32_t>(bbox.yMin);
    box.xMax = static_cast<std::int32_t>(bbox.xMax);
    box.yMax = static_cast<std::int32_t>(bbox.yMax);
    return box.xMax > box.xMin || box.yMax > box.yMin;
}

}  // namespace

bool fontGlyphInkBox(const std::string& face, char32_t cp, FontInkBox& box) {
    if (face.empty()) return false;
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics || !openData(*metrics)) return false;
    return outlineInkBox(*metrics, lookupGlyph(*metrics, cp), box);
}

std::vector<std::uint8_t> fontGridTable(std::vector<FontGridCell> cells, std::int32_t maxPpem) {
    std::sort(cells.begin(), cells.end(), [](const FontGridCell& a, const FontGridCell& b) {
        return a.codePoint != b.codePoint ? a.codePoint < b.codePoint : a.ppem < b.ppem;
    });
    std::vector<std::uint8_t> out;
    out.reserve(kGridHeader + cells.size() * kGridCell);
    const auto put16 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v));
    };
    const auto put32 = [&](std::uint32_t v) {
        put16(v >> 16);
        put16(v & 0xFFFF);
    };
    put16(1);
    put16(static_cast<std::uint32_t>(std::clamp(maxPpem, 0, 0xFFFF)));
    put32(static_cast<std::uint32_t>(cells.size()));
    for (const FontGridCell& cell : cells) {
        put32(static_cast<std::uint32_t>(cell.codePoint));
        put16(static_cast<std::uint32_t>(cell.ppem));
        put16(static_cast<std::uint32_t>(cell.advancePx));
    }
    return out;
}

bool fontWinMetrics(const std::string& face, std::int32_t& ascent, std::int32_t& descent) {
    ascent = 0;
    descent = 0;
    if (face.empty()) return false;
    const FontWhyFallback why("폭", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics) return false;
    // OS/2 가 없는 폭 정보 파일은 hhea 값을 쓴다. 수식 글꼴과 바탕 계열은 두 값이 같다.
    if (metrics->winAscent + metrics->winDescent > 0) {
        ascent = metrics->winAscent;
        descent = metrics->winDescent;
    } else {
        ascent = std::max<std::int32_t>(metrics->ascender, 0);
        descent = std::max<std::int32_t>(-static_cast<std::int32_t>(metrics->descender), 0);
    }
    return ascent + descent > 0;
}

std::int32_t fontBaseline(const std::string& face, std::int32_t em) {
    if (face.empty() || em <= 0) return -1;
    const FontWhyFallback why("기준선", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* metrics = metricsFor(face);
    if (!metrics || metrics->unitsPerEm == 0 || metrics->ascender <= 0) return -1;
    const std::int64_t numer = static_cast<std::int64_t>(em) * metrics->ascender;
    const std::int64_t den = metrics->unitsPerEm;
    return static_cast<std::int32_t>((numer + den / 2) / den);
}

#include "font_line_box_table.inc"

namespace {

std::int32_t mulDiv1024(std::int32_t size, std::int32_t value) {
    return static_cast<std::int32_t>((static_cast<std::int64_t>(size) * value + 512) / 1024);
}

// 1em = 1024 의 위·아래. 0.5는 올린다.
std::pair<std::int32_t, std::int32_t> lineBoxUnits(const std::string& face) {
    const auto begin = std::begin(kFontLineBox);
    const auto end = std::end(kFontLineBox);
    const auto found = std::lower_bound(begin, end, face, [](const FontLineBoxRow& row, const std::string& name) {
        return std::strcmp(row.name, name.c_str()) < 0;
    });
    if (found != end && std::strcmp(found->name, face.c_str()) == 0) return {found->up, found->down};
    if (!face.empty()) {
        const FontWhyFallback why("기준선", 0, &face);
        std::lock_guard lock(gMutex);
        const Metrics* metrics = metricsFor(face);
        if (metrics && metrics->unitsPerEm > 0 && metrics->winAscent + metrics->winDescent > 0) {
            const std::int32_t upm = metrics->unitsPerEm;
            return {(metrics->winAscent * 1024 + upm / 2) / upm, (metrics->winDescent * 1024 + upm / 2) / upm};
        }
    }
    return {870, 154};
}

}  // namespace

FontLineBox fontLineBox(const std::string& face, std::int32_t size) {
    const auto [up, down] = lineBoxUnits(face);
    FontLineBox box;
    box.ascent = mulDiv1024(size, up);
    box.height = box.ascent + mulDiv1024(size, down);
    return box;
}

#include "font_em_table.inc"

namespace {

enum class FaceProbe { Hit, Miss, Unsettled };

FaceProbe probeLoaded(const std::string& face, char32_t cp) {
    if (face.empty()) return FaceProbe::Miss;
    if (const auto alias = gAlias.find(face); alias != gAlias.end()) {
        if (alias->second.empty()) return FaceProbe::Miss;
        const auto found = gFaces.find(alias->second);
        if (found == gFaces.end()) return FaceProbe::Unsettled;
        return lookupGlyph(found->second, cp) > 0 ? FaceProbe::Hit : FaceProbe::Miss;
    }
    const auto found = gFaces.find(face);
    if (found == gFaces.end()) return gFailed.count(face) ? FaceProbe::Miss : FaceProbe::Unsettled;
    return lookupGlyph(found->second, cp) > 0 ? FaceProbe::Hit : FaceProbe::Miss;
}

const Metrics* metricsLoaded(const std::string& face) {
    if (const auto alias = gAlias.find(face); alias != gAlias.end()) {
        if (alias->second.empty()) return nullptr;
        const auto found = gFaces.find(alias->second);
        return found == gFaces.end() ? nullptr : &found->second;
    }
    const auto found = gFaces.find(face);
    return found == gFaces.end() ? nullptr : &found->second;
}

}  // namespace

ReadyGlyph measureReadyGlyph(const std::string& face, char32_t codePoint, const std::string& subst,
                             const std::string& base) {
    std::lock_guard lock(gMutex);
    ReadyGlyph out;
    const FaceProbe first = probeLoaded(face, codePoint);
    if (first == FaceProbe::Unsettled) return out;
    if (first == FaceProbe::Hit) {
        out.resolved = face;
    } else {
        std::vector<std::string> names;
        auto push = [&](const std::string& name) {
            if (name.empty() || name == face) return;
            if (std::find(names.begin(), names.end(), name) != names.end()) return;
            names.push_back(name);
        };
        push(subst);
        push(base);
        if (isPrivateUse(codePoint)) {
            std::vector<const char*> family;
            appendFamilyFallbacks(face, family);
            for (const char* name : family) push(name);
            for (const char* name : {"함초롬돋움", "함초롬바탕", "Wingdings"}) push(name);
        }
        for (const auto& name : names) {
            const FaceProbe probe = probeLoaded(name, codePoint);
            if (probe == FaceProbe::Unsettled) return out;
            if (probe == FaceProbe::Hit) {
                out.resolved = name;
                break;
            }
        }
        if (out.resolved.empty()) out.resolved = face;
    }
    const Metrics* metrics = metricsLoaded(out.resolved);
    if (!metrics) return out;
    out.known = true;
    out.units = metrics->unitsPerEm;
    out.advance = designAdvanceOf(*metrics, codePoint);
    return out;
}

std::uint16_t fontTableEm(const std::string& face) {
    const auto begin = std::begin(kFontEm);
    const auto end = std::end(kFontEm);
    const auto found = std::lower_bound(begin, end, face, [](const FontEmRow& row, const std::string& name) {
        return std::strcmp(row.name, name.c_str()) < 0;
    });
    if (found != end && std::strcmp(found->name, face.c_str()) == 0) return found->em;
    return 1024;
}

namespace {

// 문서 글자가 쓰는 글꼴과 그 글꼴로 그릴 글자.
struct UsedFace {
    std::unordered_set<char32_t> chars;
    bool bold = false;
    std::vector<std::string> alternates;  // 그 자리의 문서 대체 글꼴(subst, base)
};

struct UsedFaces {
    std::unordered_map<std::string, UsedFace> faces;
    std::vector<std::string> order;  // 처음 만난 차례
    bool nameless = false;           // 글꼴 이름이 없는 글자. 그리기가 함초롬바탕으로 간다
    std::vector<std::string> equationFaces;  // 수식을 재고 그리는 수식 글꼴
};

bool nextUtf8(const std::string& text, std::size_t& i, char32_t& out) {
    if (i >= text.size()) return false;
    const auto c = static_cast<unsigned char>(text[i]);
    int extra = 0;
    if (c < 0x80) {
        out = c;
    } else if ((c & 0xE0) == 0xC0) {
        out = c & 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        out = c & 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        out = c & 0x07;
        extra = 3;
    } else {
        out = c;
    }
    ++i;
    for (int k = 0; k < extra && i < text.size(); ++k, ++i) out = (out << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
    return true;
}

const model::CharShape* usedCharShape(const model::Document& doc, std::uint32_t id) {
    if (id < doc.header.charShapes.size() && doc.header.charShapes[id].id == id) return &doc.header.charShapes[id];
    for (const auto& shape : doc.header.charShapes) {
        if (shape.id == id) return &shape;
    }
    return nullptr;
}

void useChar(const model::Document& doc, const model::CharShape& shape, model::Lang lang, char32_t cp, UsedFaces& out) {
    const std::size_t slot = static_cast<std::size_t>(lang);
    if (slot >= doc.header.fonts.size() || slot >= shape.fontRef.size()) {
        out.nameless = true;
        return;
    }
    const auto& fonts = doc.header.fonts[slot];
    const std::uint32_t id = shape.fontRef[slot];
    if (id >= fonts.size() || fonts[id].face.empty()) {
        out.nameless = true;
        return;
    }
    const model::Font& font = fonts[id];
    auto [at, fresh] = out.faces.try_emplace(font.face);
    if (fresh) out.order.push_back(font.face);
    UsedFace& used = at->second;
    used.chars.insert(cp);
    if (shape.bold) used.bold = true;
    for (const std::string* alt : {&font.subst, &font.base}) {
        if (alt->empty() || std::find(used.alternates.begin(), used.alternates.end(), *alt) != used.alternates.end()) continue;
        used.alternates.push_back(*alt);
    }
}

// 본문 글자는 한글 음절이면 한글 칸, 아니면 영문 칸이다(glyphFace). 문단 머리는 hancomCharLang 칸이다.
void useText(const model::Document& doc, std::uint32_t charShapeId, const std::string& text, bool heading,
             UsedFaces& out) {
    const model::CharShape* shape = usedCharShape(doc, charShapeId);
    if (!shape) {
        if (!text.empty()) out.nameless = true;
        return;
    }
    std::size_t i = 0;
    char32_t cp = 0;
    while (nextUtf8(text, i, cp)) {
        if (cp < 0x20) continue;
        const model::Lang lang =
            heading ? hancomCharLang(cp, true) : (cp >= 0xAC00 && cp <= 0xD7A3 ? model::Lang::Hangul : model::Lang::Latin);
        useChar(doc, *shape, lang, cp, out);
    }
}

void useParagraphs(const model::Document& doc, const std::vector<model::Paragraph>& paras, UsedFaces& out, int depth);

void useControl(const model::Document& doc, const model::Control& control, UsedFaces& out, int depth) {
    for (const auto& list : control.subLists) useParagraphs(doc, list.paragraphs, out, depth + 1);
    if (control.table) {
        for (const auto& cell : control.table->cells) useParagraphs(doc, cell.content.paragraphs, out, depth + 1);
    }
    if (control.caption) useParagraphs(doc, control.caption->content.paragraphs, out, depth + 1);
}

const model::ParaShape* usedParaShape(const model::Document& doc, std::uint32_t id) {
    if (id < doc.header.paraShapes.size() && doc.header.paraShapes[id].id == id) return &doc.header.paraShapes[id];
    for (const auto& shape : doc.header.paraShapes) {
        if (shape.id == id) return &shape;
    }
    return nullptr;
}

// 개요·번호·글머리표 머리. 번호는 숫자·로마자·한글 차례 글자로 쓴다.
void useHeading(const model::Document& doc, const model::Paragraph& para, UsedFaces& out) {
    const model::ParaShape* shape = usedParaShape(doc, para.paraShapeId);
    if (!shape || shape->headingType.empty() || shape->headingType == "NONE") return;
    const std::uint32_t bodyShape = para.runs.empty() ? 0 : para.runs.front().charShapeId;
    const auto headShape = [&](const model::ParaHead& head) {
        return head.charShapeId != 0xFFFFFFFFu ? head.charShapeId : bodyShape;
    };
    if (shape->headingType == "BULLET") {
        for (const auto& bullet : doc.header.bullets) {
            if (bullet.id != shape->headingIdRef) continue;
            useText(doc, headShape(bullet.head), bullet.text, true, out);
            useText(doc, headShape(bullet.head), bullet.checkedText, true, out);
        }
        // 정의 없는 0번은 ● 를 쓴다.
        if (shape->headingIdRef == 0) useText(doc, bodyShape, "\u25CF", true, out);
        return;
    }
    const model::Numbering* numbering = nullptr;
    for (const auto& item : doc.header.numberings) {
        if (item.id == shape->headingIdRef) numbering = &item;
    }
    static const std::string kNumberChars = "0123456789ivxlcdmIVXLCDM가나다라마바사아자차카타파하①②③㉠㉮";
    if (!numbering) {
        useText(doc, bodyShape, kNumberChars, true, out);
        return;
    }
    for (const model::ParaHead& head : numbering->levels) {
        useText(doc, headShape(head), head.format, true, out);
        useText(doc, headShape(head), kNumberChars, true, out);
    }
}

void useParagraphs(const model::Document& doc, const std::vector<model::Paragraph>& paras, UsedFaces& out, int depth) {
    if (depth > 32) return;
    for (const auto& para : paras) {
        useHeading(doc, para, out);
        for (const auto& run : para.runs) {
            for (const auto& item : run.items) {
                if (item.type == model::InlineItem::Type::Text) {
                    useText(doc, run.charShapeId, item.text, false, out);
                    continue;
                }
                const model::Control& control = item.control;
                switch (control.kind) {
                case model::ControlKind::Footnote:
                case model::ControlKind::Endnote:
                case model::ControlKind::AutoNumber:
                case model::ControlKind::NewNumber:
                case model::ControlKind::PageNumPos:
                    // 각주·미주·쪽 번호는 그 런 글자 모양의 숫자와 장식으로 찍는다.
                    useText(doc, run.charShapeId, "0123456789()-.ivxIVX" + control.prefix + control.suffix, false, out);
                    break;
                case model::ControlKind::Equation: {
                    // 수식은 고른 수식 글꼴로 재고 그리며, 그 글꼴에 없는 글자는 HyhwpEQ·HancomEQN 에서 찾는다
                    // (render/equation_draw symbolFaces). 받을 차례도 같다. 이름이 비면 HyhwpEQ 다.
                    const std::string chosen = control.equationFont.empty() || control.equationFont == "HYhwpEQ"
                                                   ? std::string("HyhwpEQ")
                                                   : control.equationFont;
                    for (const std::string& face : {chosen, std::string("HyhwpEQ"), std::string("HancomEQN")}) {
                        if (std::find(out.equationFaces.begin(), out.equationFaces.end(), face) == out.equationFaces.end())
                            out.equationFaces.push_back(face);
                    }
                    break;
                }
                case model::ControlKind::Dutmal:
                case model::ControlKind::Compose:
                    useText(doc, run.charShapeId, control.text, false, out);
                    useText(doc, run.charShapeId, control.subText, false, out);
                    break;
                default:
                    break;
                }
                useControl(doc, control, out, depth);
            }
        }
    }
}

UsedFaces documentUsedFaces(const model::Document& doc) {
    UsedFaces out;
    for (const auto& section : doc.sections) {
        useParagraphs(doc, section.paragraphs, out, 0);
        for (const auto& master : section.masterPages) useParagraphs(doc, master.content.paragraphs, out, 0);
    }
    return out;
}

}  // namespace

std::vector<std::string> documentFontFaces(const model::Document& doc) {
    const UsedFaces used = documentUsedFaces(doc);
    std::vector<std::string> out;
    const auto add = [&](const std::string& name) {
        if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    };
    bool batang = used.nameless;
    for (const std::string& face : used.order) {
        const UsedFace& use = used.faces.at(face);
        add(face);
        if (use.bold) {
            add(face + " Bold");
            add(face + " 볼드");
        }
        // 글꼴 파일이 없으면(폭만 잰 글꼴 포함) 폭은 metricsFor 대체, 그리기는 함초롬바탕이다.
        const std::string replaced = fontReplacement(face);
        const std::string path = fontCatalogPath(replaced.empty() ? face : replaced);
        if (path.rfind("full/", 0) != 0 && path.rfind("standin/", 0) != 0) batang = true;
    }
    for (const std::string& face : used.equationFaces) add(face);
    if (batang) add("함초롬바탕");
    return out;
}

std::vector<DrawnGlyph> documentDrawnGlyphs(const model::Document& doc) {
    const UsedFaces used = documentUsedFaces(doc);
    std::vector<DrawnGlyph> out;
    for (const std::string& face : used.order) {
        const UsedFace& use = used.faces.at(face);
        const std::string subst = use.alternates.size() > 0 ? use.alternates[0] : std::string();
        const std::string base = use.alternates.size() > 1 ? use.alternates[1] : std::string();
        for (const char32_t cp : use.chars) {
            if (cp <= 0x20 || cp == 0xA0 || cp == 0x2007) continue;
            // 줄은 문서 글꼴 이름 그대로 그리기도 하고(미룬 줄), 빠진 글자는 대체까지 고른 이름으로 그리기도 한다.
            out.push_back(DrawnGlyph{face, cp, use.bold});
            std::string resolved = fontWithGlyph(face, cp, subst, base);
            if (resolved != face) out.push_back(DrawnGlyph{std::move(resolved), cp, use.bold});
        }
    }
    return out;
}

std::vector<std::string> charShapeFaces(const model::Document& doc, std::uint32_t charShapeId) {
    std::vector<std::string> out;
    const model::CharShape* shape = usedCharShape(doc, charShapeId);
    if (!shape) return out;
    for (const model::Lang lang : {model::Lang::Hangul, model::Lang::Latin}) {
        const std::size_t slot = static_cast<std::size_t>(lang);
        if (slot >= doc.header.fonts.size() || slot >= shape->fontRef.size()) continue;
        const auto& fonts = doc.header.fonts[slot];
        const std::uint32_t id = shape->fontRef[slot];
        if (id >= fonts.size() || fonts[id].face.empty()) continue;
        out.push_back(fonts[id].face);
        if (shape->bold) {
            out.push_back(fonts[id].face + " Bold");
            out.push_back(fonts[id].face + " 볼드");
        }
    }
    return out;
}

std::vector<std::string> documentFontGaps(const model::Document& doc) {
    const UsedFaces used = documentUsedFaces(doc);
    std::vector<std::string> out;
    std::lock_guard lock(gMutex);
    const auto add = [&](const std::string& name) {
        if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
    };
    const auto loaded = [&](const std::string& name) -> const Metrics* {
        const auto found = gFaces.find(name);
        return found == gFaces.end() ? nullptr : &found->second;
    };
    const auto fetchable = [&](const std::string& name) {
        const auto replaced = gFontReplace.find(name);
        const std::string& asked = replaced == gFontReplace.end() || replaced->second.empty() ? name : replaced->second;
        const auto file = gFaceFile.find(asked);
        return file != gFaceFile.end() && !file->second.empty();
    };
    for (const std::string& face : used.order) {
        // 올라오지 않은 글꼴은 파일이 없는 글꼴이다. 폭·그리기를 대신하는 함초롬바탕은 처음 목록에 있다.
        const Metrics* metrics = loaded(face);
        if (!metrics) continue;
        const UsedFace& use = used.faces.at(face);
        for (const char32_t cp : use.chars) {
            if (cp <= 0x20 || cp == 0xA0 || cp == 0x2007) continue;
            if (lookupGlyph(*metrics, cp) > 0) continue;
            // fontWithGlyph 와 같은 차례로 본다: 문서 대체, 기본 글꼴, 사용자 영역이면 같은 계열 함초롬과 Wingdings.
            // 올라온 글꼴에 글자가 있으면 거기서 끝이고, 아직 없는 글꼴은 받을 이름으로 내고 거기서 멈춘다.
            std::vector<std::string> chain = use.alternates;
            if (isPrivateUse(cp)) {
                std::vector<const char*> family;
                appendFamilyFallbacks(face, family);
                for (const char* name : family) chain.emplace_back(name);
                for (const char* name : {"함초롬돋움", "함초롬바탕", "Wingdings"}) chain.emplace_back(name);
            }
            bool settled = false;
            for (const std::string& name : chain) {
                if (name.empty() || name == face) continue;
                if (const Metrics* alt = loaded(name)) {
                    if (lookupGlyph(*alt, cp) > 0) {
                        settled = true;
                        break;
                    }
                    continue;
                }
                if (!fetchable(name)) continue;
                add(name);
                settled = true;
                break;
            }
            // 어디에도 없으면 폭과 그리기가 함초롬바탕으로 간다.
            if (!settled && !loaded("함초롬바탕")) add("함초롬바탕");
        }
    }
    return out;
}

std::vector<MissingGlyph> documentMissingGlyphs(const model::Document& doc) {
    const UsedFaces used = documentUsedFaces(doc);
    // 파일이 없거나 폭만 있는 글꼴은 함초롬바탕이 대신 그린다(documentFontFaces 와 같은 판단).
    std::unordered_map<std::string, std::string> drawFace;
    for (const std::string& face : used.order) {
        const std::string replaced = fontReplacement(face);
        const std::string path = fontCatalogPath(replaced.empty() ? face : replaced);
        const bool hasFile = path.rfind("full/", 0) == 0 || path.rfind("standin/", 0) == 0;
        drawFace[face] = hasFile ? face : std::string("함초롬바탕");
    }
    std::vector<MissingGlyph> out;
    std::lock_guard lock(gMutex);
    for (const std::string& face : used.order) {
        const UsedFace& use = used.faces.at(face);
        std::vector<char32_t> chars(use.chars.begin(), use.chars.end());
        std::sort(chars.begin(), chars.end());
        for (const char32_t cp : chars) {
            if (cp <= 0x20 || cp == 0xA0 || cp == 0x2007) continue;
            // fontWithGlyph 와 같은 차례: 그리는 글꼴, 문서 대체, 사용자 영역이면 같은 계열 함초롬과 Wingdings.
            std::vector<std::string> chain{drawFace[face]};
            for (const std::string& name : use.alternates) chain.push_back(name);
            if (isPrivateUse(cp)) {
                std::vector<const char*> family;
                appendFamilyFallbacks(face, family);
                for (const char* name : family) chain.emplace_back(name);
                for (const char* name : {"함초롬돋움", "함초롬바탕", "Wingdings"}) chain.emplace_back(name);
            }
            const bool drawn = std::any_of(chain.begin(), chain.end(),
                                           [&](const std::string& name) { return faceHasGlyph(name, cp); });
            if (!drawn) out.push_back(MissingGlyph{face, cp});
        }
    }
    return out;
}

}  // namespace chwp::layout
