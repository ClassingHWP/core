#include "export/font_metrics.hpp"

#include "noting/model/document.hpp"

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

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace noting::layout {
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

struct Metrics {
    std::shared_ptr<const std::vector<std::uint8_t>> data;
    std::uint16_t unitsPerEm = 0;
    std::int16_t xAvgCharWidth = 0;
    std::uint16_t winAscent = 0;
    std::uint16_t winDescent = 0;
    std::int16_t ascender = 0;
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
// 라이선스 때문에 파일을 올리지 못한 글꼴 → 그 폴백을 만든 OFL 원본 글꼴(글꼴 서버의 license-swap.json).
std::unordered_map<std::string, std::string> gLicenseSwap;
bool gAvailableReady = false;
FontFaceRequestHandler gFontFaceHandler = nullptr;
void* gFontFaceUser = nullptr;
std::unordered_map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> gFileBytes;
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
    if (!font.data) return false;
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
    if (!font.data || cp < 0 || cp > 0x10FFFF) return -1;
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
    if (!font.data) return;
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

void registerFontFace(std::string face, std::vector<std::uint8_t> bytes) {
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
    if (!held) {
        if (bytes.size() < 12) return;
        held = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
        if (!file.empty()) gFileBytes[file] = held;
    }
    if (!adoptSharedFaceBytes(face, std::move(held))) return;
    gFailed.erase(face);
    gFetchMiss.erase(face);
    gAlias.erase(face);
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
    if (const auto cached = gFileBytes.find(file); cached != gFileBytes.end()) return cached->second;
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

void requestDocumentFontFaces(const model::Document& doc) {
    FontFaceRequestHandler handler = nullptr;
    void* user = nullptr;
    {
        std::lock_guard lock(gMutex);
        handler = gFontFaceHandler;
        user = gFontFaceUser;
    }
    if (!handler) return;
    std::vector<std::string> faces;
    auto add = [&](const std::string& name) {
        if (name.empty()) return;
        if (std::find(faces.begin(), faces.end(), name) != faces.end()) return;
        faces.push_back(name);
    };
    for (const auto& group : doc.header.fonts) {
        for (const auto& font : group) {
            add(font.face);
            add(font.subst);
        }
    }
    // 글리프가 없을 때 core 가 넘어가는 글꼴. 사용자 영역 기호는 Wingdings 까지 간다.
    for (const char* fallback : {"함초롬바탕", "함초롬돋움", "Wingdings"}) add(fallback);
    std::vector<const char*> ptrs;
    ptrs.reserve(faces.size());
    for (const std::string& face : faces) ptrs.push_back(face.c_str());
    handler(user, ptrs.empty() ? nullptr : ptrs.data(), static_cast<int>(ptrs.size()));
}

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

bool fontLicenseBlocked(const std::string& face) {
    std::lock_guard lock(gMutex);
    return catalogBlocked(face);
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
    if (!metrics || !metrics->data || metrics->data->empty()) return false;
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
    if (!regular || !regular->data || regular->data->empty()) return false;
    const Metrics* chosen = regular;
    if (bold) {
        if (regular->weight >= 600) {
            designedBold = true;
        } else if (const Metrics* heavy = boldCompanion(face)) {
            chosen = heavy;
            designedBold = true;
        }
    }
    out = chosen->data;
    fontOffset = static_cast<int>(chosen->fontOffset);
    return true;
}

bool fontDesignedBold(const std::string& face) {
    if (face.empty()) return false;
    const FontWhyFallback why("굵기", 0, &face);
    std::lock_guard lock(gMutex);
    const Metrics* regular = metricsFor(face);
    if (!regular || !regular->data || regular->data->empty()) return false;
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

}  // namespace noting::layout
