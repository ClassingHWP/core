#include "chwp/model/memo.hpp"
#include "chwp/model/proofreading.hpp"
#include "chwp/hwp/hwp_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "chwp/common/unicode.hpp"
#include "chwp/container/cfb.hpp"
#include "chwp/container/zip.hpp"
#include "chwp/hwp/records.hpp"
#include "chwp/reader.hpp"
#include "chwp/unit.hpp"

namespace chwp::hwp {

namespace {

using namespace chwp::model;

// HWP 레코드 하나를 ByteWriter 에 추가한다.
void appendRecord(ByteWriter& stream, std::uint16_t tagId, std::uint16_t level, ByteView payload) {
    const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
    const std::uint32_t sizeField = size < 0xFFFu ? size : 0xFFFu;
    const std::uint32_t header = (static_cast<std::uint32_t>(tagId) & 0x3FFu) |
                                 ((static_cast<std::uint32_t>(level) & 0x3FFu) << 10) |
                                 (sizeField << 20);
    stream.u32(header);
    if (size >= 0xFFFu) {
        stream.u32(size);
    }
    stream.bytes(payload);
}

// 주석 번호 모양 번호(HWP). 이름은 읽기(numberShapeName)와 같다.
std::uint32_t noteNumberShapeCode(const std::string& name) {
    static const char* const kNames[] = {"DIGIT", "CIRCLED_DIGIT", "ROMAN_CAPITAL", "ROMAN_SMALL", "LATIN_CAPITAL",
                                         "LATIN_SMALL", "CIRCLED_LATIN_CAPITAL", "CIRCLED_LATIN_SMALL",
                                         "HANGUL_SYLLABLE", "CIRCLED_HANGUL_SYLLABLE", "HANGUL_JAMO",
                                         "CIRCLED_HANGUL_JAMO", "HANGUL_PHONETIC", "IDEOGRAPH", "CIRCLED_IDEOGRAPH",
                                         "DECAGON", "DECAGON_HANJA"};
    for (std::uint32_t i = 0; i < std::size(kNames); ++i) {
        if (name == kNames[i]) return i;
    }
    if (name == "DECAGON_CIRCLE") return 15;
    if (name == "DECAGON_CIRCLE_HANJA") return 16;
    if (name == "SYMBOL") return 0x80;
    if (name == "USER_CHAR") return 0x81;
    return 0;
}

// 글의 첫 글자(UTF-16 한 칸). 없으면 0.
std::uint16_t firstWchar(const std::string& text) {
    if (text.empty()) return 0;
    const std::u16string wide = utf8ToUtf16(text);
    return wide.empty() ? 0 : static_cast<std::uint16_t>(wide.front());
}

std::uint32_t toColorRef(Color c, std::uint32_t defaultVal = 0x00000000u) {
    if (!c) return defaultVal;
    const std::uint32_t rgb = *c & 0xFFFFFFu;
    const std::uint32_t r = (rgb >> 16) & 0xFFu;
    const std::uint32_t g = (rgb >> 8) & 0xFFu;
    const std::uint32_t b = rgb & 0xFFu;
    return (b << 16) | (g << 8) | r;
}

// 메모 쓰기 문맥(write 가 처음에 매긴다). 메모 번호는 문서 차례로 1 부터, 필드 고유 id 는 있던 것을 쓰고 없으면 새로 매긴다.
struct MemoWriteContext {
    std::vector<const Control*> order;
    std::unordered_map<const Control*, std::uint32_t> number;
    std::unordered_map<const Control*, std::uint32_t> id;
};
thread_local MemoWriteContext gMemos;

std::uint32_t hexToColorRef(std::string_view s, std::uint32_t defaultVal = 0x00000000u) {
    if (s.empty() || s == "none") return defaultVal;
    if (s.front() == '#') s.remove_prefix(1);
    if (s.size() > 6) s.remove_prefix(s.size() - 6);
    const std::uint32_t rgb =
        static_cast<std::uint32_t>(std::strtoul(std::string(s).c_str(), nullptr, 16));
    return toColorRef(rgb, defaultVal);
}

std::uint8_t toHwpBorderKind(std::string_view type) {
    if (type.empty() || type == "NONE") return 0;
    static constexpr std::pair<std::string_view, std::uint8_t> kKinds[] = {
        {"SOLID", 1},
        {"DASH", 2},
        {"DOT", 3},
        {"DASH_DOT", 4},
        {"DASH_DOT_DOT", 5},
        {"LONG_DASH", 6},
        {"CIRCLE", 7},
        {"DOUBLE_SLIM", 8},
        {"SLIM_THICK", 9},
        {"THICK_SLIM", 10},
        {"SLIM_THICK_SLIM", 11},
        {"WAVE", 12},
        {"DOUBLE_WAVE", 13},
        {"THICK_3D", 14},
        {"THICK_3D_REVERSE_LIGHTING", 15},
        {"3D", 16},
        {"3D_REVERSE_LIGHTING", 17},
    };
    for (const auto& [k, v] : kKinds) {
        if (type == k) return v;
    }
    return 1;
}

std::uint8_t toHwpBorderThick(std::int32_t widthHu) {
    static constexpr double kMm[] = {0.1, 0.12, 0.15, 0.2,  0.25, 0.3, 0.4, 0.5,
                                     0.6, 0.7,  1.0,  1.5,  2.0,  3.0, 4.0, 5.0};
    if (widthHu <= 0) return 1;
    std::uint8_t best = 0;
    std::int32_t bestDiff = std::abs(widthHu - mmToHu(kMm[0]));
    for (std::size_t i = 1; i < std::size(kMm); ++i) {
        const std::int32_t d = std::abs(widthHu - mmToHu(kMm[i]));
        if (d < bestDiff) {
            bestDiff = d;
            best = static_cast<std::uint8_t>(i);
        }
    }
    return best;
}

unsigned toHwpDiagonalShape(std::string_view type) {
    if (type.empty() || type == "NONE") return 0;
    if (type == "CENTER_BELOW") return 3;
    if (type == "CENTER_ABOVE") return 6;
    if (type == "ALL") return 7;
    return 2;  // CENTER / STRAIGHT
}

unsigned toHwpNumFormat(std::string_view fmt) {
    if (fmt == "CIRCLED_DIGIT") return 1;
    if (fmt == "ROMAN_CAPITAL") return 2;
    if (fmt == "ROMAN_SMALL") return 3;
    if (fmt == "LATIN_CAPITAL") return 4;
    if (fmt == "LATIN_SMALL") return 5;
    if (fmt == "CIRCLED_LATIN_CAPITAL") return 6;
    if (fmt == "CIRCLED_LATIN_SMALL") return 7;
    if (fmt == "HANGUL_SYLLABLE") return 8;
    if (fmt == "CIRCLED_HANGUL_SYLLABLE") return 9;
    if (fmt == "HANGUL_JAMO") return 10;
    if (fmt == "CIRCLED_HANGUL_JAMO") return 11;
    if (fmt == "HANGUL_PHONETIC") return 12;
    if (fmt == "IDEOGRAPH") return 13;
    if (fmt == "CIRCLED_IDEOGRAPH") return 14;
    return 0;  // DIGIT
}

char16_t firstUtf16Char(std::string_view utf8) {
    const std::u16string u16 = utf8ToUtf16(utf8);
    return u16.empty() ? 0 : u16.front();
}

struct HwpBinEntry {
    std::uint16_t binId = 1;  // 1-based
    std::string ext;
    std::string streamPath;   // "BinData/BIN0001.png"
    bool isLink = false;
    std::string linkPath;
    Bytes rawData;
};

struct HwpBinRegistry {
    std::vector<HwpBinEntry> entries;
    std::unordered_map<std::string, std::uint16_t> idMap;

    std::uint16_t resolve(const std::string& key) const {
        if (key.empty()) return 0;
        auto it = idMap.find(key);
        if (it != idMap.end()) return it->second;
        const long v = std::strtol(key.c_str(), nullptr, 10);
        if (v > 0 && v <= 0xFFFF) return static_cast<std::uint16_t>(v);
        return 0;
    }
};

HwpBinRegistry buildHwpBinRegistry(const Document& doc, ByteView sourceData) {
    HwpBinRegistry reg;
    for (std::size_t i = 0; i < doc.header.binItems.size(); ++i) {
        const auto& item = doc.header.binItems[i];
        HwpBinEntry e;
        e.binId = static_cast<std::uint16_t>(i + 1);
        e.ext = item.extension.empty() ? "bin" : item.extension;
        if (!e.ext.empty() && e.ext.front() == '.') e.ext.erase(e.ext.begin());
        e.isLink = (item.storage == "LINK");
        e.linkPath = item.path;

        char nameBuf[64];
        std::snprintf(nameBuf, sizeof(nameBuf), "BinData/BIN%04X.%s", e.binId, e.ext.c_str());
        e.streamPath = nameBuf;

        if (!e.isLink) {
            if (!item.data.empty()) {
                e.rawData = item.data;
            } else if (!sourceData.empty()) {
                readEmbeddedBin(sourceData, doc, item, e.rawData);
            }
        }

        if (!item.id.empty()) reg.idMap[item.id] = e.binId;
        reg.idMap[std::to_string(e.binId)] = e.binId;
        reg.entries.push_back(std::move(e));
    }
    return reg;
}

void writeBorderSideBytes(ByteWriter& w, const BorderSide& side) {
    const std::uint8_t kind = toHwpBorderKind(side.type);
    const std::uint8_t thick = toHwpBorderThick(side.width);
    const std::uint32_t color = hexToColorRef(side.color, 0x00000000u);
    w.u8(kind);
    w.u8(thick);
    w.u32(color);
}

Bytes buildDocInfoStream(const Document& doc, const HwpBinRegistry& bins, bool compress) {
    const DocHeader& h = doc.header;
    ByteWriter stream;

    const std::uint16_t secCount =
        static_cast<std::uint16_t>(std::max<std::size_t>(1, doc.sections.size()));

    // 1. DocumentProperties (level 0)
    {
        ByteWriter w;
        w.u16(secCount);
        w.u16(static_cast<std::uint16_t>(h.beginNumbers.page));
        w.u16(static_cast<std::uint16_t>(h.beginNumbers.footnote));
        w.u16(static_cast<std::uint16_t>(h.beginNumbers.endnote));
        w.u16(static_cast<std::uint16_t>(h.beginNumbers.picture));
        w.u16(static_cast<std::uint16_t>(h.beginNumbers.table));
        w.u16(static_cast<std::uint16_t>(h.beginNumbers.equation));
        w.u32(0);  // list id
        w.u32(0);  // para id
        w.u32(0);  // char pos
        appendRecord(stream, tag::DocumentProperties, 0, w.view());
    }

    // 준비된 각 테이블 개수 계산 (비어 있으면 기본 1개씩 생성)
    PerLang<std::vector<Font>> fonts = h.fonts;
    for (auto& bucket : fonts) {
        if (bucket.empty()) {
            Font f;
            f.id = 0;
            f.face = "함초롬바탕";
            f.type = "TTF";
            bucket.push_back(std::move(f));
        }
    }
    std::vector<BorderFill> borderFills = h.borderFills;
    if (borderFills.empty()) borderFills.emplace_back();

    std::vector<CharShape> charShapes = h.charShapes;
    if (charShapes.empty()) {
        CharShape cs;
        cs.ratio.fill(100);
        cs.relSize.fill(100);
        charShapes.push_back(cs);
    }

    std::vector<TabDef> tabDefs = h.tabDefs;
    if (tabDefs.empty()) tabDefs.emplace_back();

    std::vector<ParaShape> paraShapes = h.paraShapes;
    if (paraShapes.empty()) paraShapes.emplace_back();

    std::vector<Style> styles = h.styles;
    if (styles.empty()) {
        Style st;
        st.id = 0;
        st.type = "PARA";
        st.name = "바탕글";
        st.engName = "Normal";
        st.langId = 1042;
        styles.push_back(std::move(st));
    }

    // 2. IdMappings (level 0)
    {
        ByteWriter w;
        w.i32(static_cast<std::int32_t>(bins.entries.size()));
        for (std::size_t i = 0; i < kLangCount; ++i) {
            w.i32(static_cast<std::int32_t>(fonts[i].size()));
        }
        w.i32(static_cast<std::int32_t>(borderFills.size()));
        w.i32(static_cast<std::int32_t>(charShapes.size()));
        w.i32(static_cast<std::int32_t>(tabDefs.size()));
        w.i32(static_cast<std::int32_t>(h.numberings.size()));
        w.i32(static_cast<std::int32_t>(h.bullets.size()));
        w.i32(static_cast<std::int32_t>(paraShapes.size()));
        w.i32(static_cast<std::int32_t>(styles.size()));
        auto keptCount = [&](std::uint16_t tagId) {
            int count = 0;
            for (const KeptRecord& rec : h.kept) {
                if (rec.tag == tagId) ++count;
            }
            return count;
        };
        // 메모 모양, 변경 추적 내용, 변경 추적 작성자 개수. 문서 수준 변경 추적(TrackChange)은 세지 않는다.
        w.i32(static_cast<std::int32_t>(h.memoShapes.size()));
        w.i32(keptCount(tag::TrackChangeContent));
        w.i32(keptCount(tag::TrackChangeAuthor));
        appendRecord(stream, tag::IdMappings, 0, w.view());
    }

    // 3. BinData (level 1)
    for (const auto& b : bins.entries) {
        ByteWriter w;
        if (b.isLink) {
            w.u16(0);  // LINK
            w.hwpString(b.linkPath);
            w.hwpString(b.linkPath);
        } else {
            const std::uint16_t compBits = compress ? (1u << 4) : (2u << 4);
            w.u16(static_cast<std::uint16_t>(1u | compBits));  // EMBEDDING
            w.u16(b.binId);
            w.hwpString(b.ext);
        }
        appendRecord(stream, tag::BinData, 1, w.view());
    }

    // 4. FaceName (level 1)
    for (std::size_t lang = 0; lang < kLangCount; ++lang) {
        for (const auto& f : fonts[lang]) {
            ByteWriter w;
            std::uint8_t attr = 0;
            if (!f.subst.empty()) attr |= 0x80u;
            if (f.typeInfo.size() == 10) attr |= 0x40u;
            if (!f.base.empty()) attr |= 0x20u;
            w.u8(attr);
            w.hwpString(f.face.empty() ? "함초롬바탕" : f.face);
            if (!f.subst.empty()) {
                w.u8(1);  // TTF
                w.hwpString(f.subst);
            }
            if (f.typeInfo.size() == 10) {
                for (unsigned char byte : f.typeInfo) w.u8(byte);
            }
            if (!f.base.empty()) {
                w.hwpString(f.base);
            }
            appendRecord(stream, tag::FaceName, 1, w.view());
        }
    }

    // 5. BorderFill (level 1)
    for (const auto& bf : borderFills) {
        ByteWriter w;
        std::uint16_t attr = 0;
        attr |= static_cast<std::uint16_t>((toHwpDiagonalShape(bf.slash.type) & 0x7u) << 2);
        attr |= static_cast<std::uint16_t>((toHwpDiagonalShape(bf.backSlash.type) & 0x7u) << 5);
        if (bf.slash.crooked) attr |= (1u << 8);
        if (bf.backSlash.crooked) attr |= (1u << 9);
        if (bf.breakCellSeparateLine) attr |= (1u << 10);
        if (bf.slash.counter) attr |= (1u << 11);
        if (bf.backSlash.counter) attr |= (1u << 12);
        if (!bf.centerLine.empty() && bf.centerLine != "NONE") {
            attr |= (1u << 13);
            if (bf.centerLine == "HORIZONTAL" || bf.centerLine == "CROSS") attr |= (1u << 8);
            if (bf.centerLine == "VERTICAL" || bf.centerLine == "CROSS") attr |= (1u << 9);
        }
        w.u16(attr);

        writeBorderSideBytes(w, bf.left);
        writeBorderSideBytes(w, bf.right);
        writeBorderSideBytes(w, bf.top);
        writeBorderSideBytes(w, bf.bottom);
        writeBorderSideBytes(w, bf.diagonal);

        const bool hasFace = !bf.faceColor.empty() && bf.faceColor != "none";
        const bool hasGrad = !bf.gradColor.empty() && bf.gradColor != "none";
        const std::uint16_t imgBinId = bins.resolve(bf.imageId);
        const bool hasImg = imgBinId > 0 && bf.imageMode >= 0;

        static const char* const hatches[] = {"HORIZONTAL", "VERTICAL", "SLASH", "BACK_SLASH", "CROSS", "CROSS_DIAGONAL"};
        std::int32_t hatch = -1;
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(std::size(hatches)); ++i) {
            if (bf.hatchStyle == hatches[i]) hatch = i;
        }
        std::uint32_t fillType = 0;
        if (hasGrad) fillType |= 4u;
        else if (hasFace || hatch >= 0) fillType |= 1u;
        if (hasImg) fillType |= 2u;
        w.u32(fillType);

        if ((fillType & 1u) != 0) {
            // 무늬만 있으면 면 색은 0xFFFFFFFF(없음)다.
            w.u32(hasFace ? hexToColorRef(bf.faceColor, 0xFFFFFFFFu) : 0xFFFFFFFFu);
            w.u32(hatch >= 0 ? hexToColorRef(bf.hatchColor.empty() ? "#000000" : bf.hatchColor, 0u) : 0u);
            w.i32(hatch);
        }
        if ((fillType & 4u) != 0) {
            w.u8(1);  // linear gradient
            w.i32(bf.gradAngle);
            w.i32(0);
            w.i32(0);
            w.i32(50);
            w.i32(2);  // 2 colors
            w.u32(hexToColorRef(hasFace ? bf.faceColor : "#FFFFFF", 0x00FFFFFFu));
            w.u32(hexToColorRef(bf.gradColor, 0x00FFFFFFu));
        }
        if ((fillType & 2u) != 0) {
            w.u8(static_cast<std::uint8_t>(bf.imageMode));
            w.u8(0);
            w.u8(0);
            w.u8(0);
            w.u16(imgBinId);
        }
        w.u32(0);  // extra size
        if ((fillType & 1u) != 0) {
            w.u8(0);
        }
        appendRecord(stream, tag::BorderFill, 1, w.view());
    }

    // 6. CharShape (level 1)
    for (const auto& cs : charShapes) {
        ByteWriter w;
        for (std::size_t b = 0; b < kLangCount; ++b) {
            w.u16(static_cast<std::uint16_t>(cs.fontRef[b]));
        }
        for (std::size_t b = 0; b < kLangCount; ++b) {
            const std::int32_t r = cs.ratio[b] > 0 ? cs.ratio[b] : 100;
            w.u8(static_cast<std::uint8_t>(std::clamp(r, 10, 250)));
        }
        for (std::size_t b = 0; b < kLangCount; ++b) {
            w.i8(static_cast<std::int8_t>(std::clamp(cs.spacing[b], -50, 50)));
        }
        for (std::size_t b = 0; b < kLangCount; ++b) {
            const std::int32_t rs = cs.relSize[b] > 0 ? cs.relSize[b] : 100;
            w.u8(static_cast<std::uint8_t>(std::clamp(rs, 10, 250)));
        }
        for (std::size_t b = 0; b < kLangCount; ++b) {
            w.i8(static_cast<std::int8_t>(std::clamp(cs.offset[b], -100, 100)));
        }
        w.i32(cs.height > 0 ? cs.height : 1000);

        std::uint32_t attr = 0;
        if (cs.italic) attr |= (1u << 0);
        if (cs.bold) attr |= (1u << 1);
        if (cs.underline == "BOTTOM") attr |= (1u << 2);
        else if (cs.underline == "TOP") attr |= (3u << 2);
        if (cs.superscript) attr |= (1u << 15);
        if (cs.subscript) attr |= (1u << 16);
        if (cs.strikeout) {
            attr |= (1u << 18);
            attr |= (1u << 26);  // strikeout shape = SOLID
        }
        if (cs.useFontSpace) attr |= (1u << 25);
        w.u32(attr);

        w.i8(10);  // shadow offset X
        w.i8(10);  // shadow offset Y
        const std::uint32_t txtCol = toColorRef(cs.textColor, 0x00000000u);
        w.u32(txtCol);
        w.u32(txtCol);  // underline color
        w.u32(toColorRef(cs.shadeColor, 0xFFFFFFFFu));
        w.u32(0x00B2B2B2u);  // shadow color
        w.u16(static_cast<std::uint16_t>(cs.borderFillId > 0 ? cs.borderFillId : 1));
        w.u32(txtCol);  // strikeout color
        appendRecord(stream, tag::CharShape, 1, w.view());
    }

    // 7. TabDef (level 1)
    for (const auto& td : tabDefs) {
        ByteWriter w;
        std::uint32_t attr = 0;
        if (td.autoTabLeft) attr |= 1u;
        if (td.autoTabRight) attr |= 2u;
        w.u32(attr);
        w.i32(static_cast<std::int32_t>(td.stops.size()));
        for (const auto& stop : td.stops) {
            w.i32(stop.pos * 2);
            std::uint8_t t = 0;
            if (stop.type == "RIGHT") t = 1;
            else if (stop.type == "CENTER") t = 2;
            else if (stop.type == "DECIMAL") t = 3;
            w.u8(t);
            // 탭 채움은 테두리 선과 달리 2 가 점선, 3 이 파선이다(한글 2024, tmp/roundtrip/leaders).
            std::uint8_t leader = toHwpBorderKind(stop.leader);
            if (leader == 2) leader = 3;
            else if (leader == 3) leader = 2;
            w.u8(leader);
            w.u16(0);
        }
        appendRecord(stream, tag::TabDef, 1, w.view());
    }

    // 8. Numbering (level 1)
    for (const auto& nd : h.numberings) {
        ByteWriter w;
        auto writeLevel = [&](std::size_t idx) {
            ParaHead ph;
            if (idx < nd.levels.size()) ph = nd.levels[idx];
            std::uint32_t attr = 0;
            if (ph.align == "CENTER") attr |= 1u;
            else if (ph.align == "RIGHT") attr |= 2u;
            if (ph.useInstWidth) attr |= (1u << 2);
            if (ph.autoIndent) attr |= (1u << 3);
            if (ph.textOffsetType == "HWPUNIT") attr |= (1u << 4);
            attr |= ((toHwpNumFormat(ph.numFormat) & 0xFu) << 5);
            w.u32(attr);
            w.i16(static_cast<std::int16_t>(ph.widthAdjust));
            w.i16(static_cast<std::int16_t>(ph.textOffset));
            w.u32(ph.charShapeId);
            w.hwpString(ph.format);
        };
        for (std::size_t i = 0; i < 7; ++i) writeLevel(i);
        w.u16(static_cast<std::uint16_t>(nd.start > 0 ? nd.start : 1));
        for (std::size_t i = 0; i < 7; ++i) {
            const std::uint32_t lvStart =
                (i < nd.levels.size() && nd.levels[i].start > 0)
                    ? static_cast<std::uint32_t>(nd.levels[i].start)
                    : 1u;
            w.u32(lvStart);
        }
        for (std::size_t i = 7; i < 10; ++i) writeLevel(i);
        appendRecord(stream, tag::Numbering, 1, w.view());
    }

    // 9. Bullet (level 1)
    for (const auto& bd : h.bullets) {
        ByteWriter w;
        const auto& ph = bd.head;
        std::uint32_t attr = 0;
        if (ph.align == "CENTER") attr |= 1u;
        else if (ph.align == "RIGHT") attr |= 2u;
        if (ph.useInstWidth) attr |= (1u << 2);
        if (ph.autoIndent) attr |= (1u << 3);
        if (ph.textOffsetType == "HWPUNIT") attr |= (1u << 4);
        w.u32(attr);
        w.i16(static_cast<std::int16_t>(ph.widthAdjust));
        w.i16(static_cast<std::int16_t>(ph.textOffset));
        w.u32(ph.charShapeId);
        w.u16(static_cast<std::uint16_t>(firstUtf16Char(bd.text)));
        const std::uint16_t imgId = bins.resolve(bd.imageId);
        w.i32(static_cast<std::int32_t>(imgId));
        w.i8(static_cast<std::int8_t>(bd.contrast));
        w.i8(static_cast<std::int8_t>(bd.bright));
        w.u8(static_cast<std::uint8_t>(bd.effect));
        w.u8(0);
        w.u8(0);
        w.u16(static_cast<std::uint16_t>(firstUtf16Char(bd.checkedText)));
        appendRecord(stream, tag::Bullet, 1, w.view());
    }

    // 10. ParaShape (level 1)
    for (const auto& ps : paraShapes) {
        ByteWriter w;
        std::uint32_t lsType = 0;
        if (ps.lineSpacingType == "FIXED") lsType = 1;
        else if (ps.lineSpacingType == "BETWEEN_LINES") lsType = 2;
        else if (ps.lineSpacingType == "AT_LEAST") lsType = 3;

        std::uint32_t alignCode = 0;
        if (ps.align == "LEFT") alignCode = 1;
        else if (ps.align == "RIGHT") alignCode = 2;
        else if (ps.align == "CENTER") alignCode = 3;
        else if (ps.align == "DISTRIBUTE") alignCode = 4;
        else if (ps.align == "DISTRIBUTE_SPACE") alignCode = 5;

        std::uint32_t latinBrk = 0;
        if (ps.breakLatinWord == "HYPHENATION") latinBrk = 1;
        else if (ps.breakLatinWord == "BREAK_WORD") latinBrk = 2;

        const std::uint32_t nonLatinBrk = (ps.breakNonLatinWord == "BREAK_WORD") ? 0u : 1u;

        std::uint32_t vAlign = 0;
        if (ps.vertAlign == "TOP") vAlign = 1;
        else if (ps.vertAlign == "CENTER") vAlign = 2;
        else if (ps.vertAlign == "BOTTOM") vAlign = 3;

        std::uint32_t headType = 0;
        if (ps.headingType == "OUTLINE") headType = 1;
        else if (ps.headingType == "NUMBER") headType = 2;
        else if (ps.headingType == "BULLET") headType = 3;

        std::uint32_t attr1 = 0;
        attr1 |= (lsType & 0x3u);
        attr1 |= ((alignCode & 0x7u) << 2);
        attr1 |= ((latinBrk & 0x3u) << 5);
        attr1 |= ((nonLatinBrk & 0x1u) << 7);
        if (ps.snapToGrid) attr1 |= (1u << 8);
        attr1 |= ((static_cast<std::uint32_t>(std::clamp(ps.condense, 0, 75)) & 0x7Fu) << 9);
        if (ps.widowOrphan) attr1 |= (1u << 16);
        if (ps.keepWithNext) attr1 |= (1u << 17);
        if (ps.keepLines) attr1 |= (1u << 18);
        if (ps.pageBreakBefore) attr1 |= (1u << 19);
        attr1 |= ((vAlign & 0x3u) << 20);
        if (ps.fontLineHeight) attr1 |= (1u << 22);
        attr1 |= ((headType & 0x3u) << 23);
        // 수준 3비트는 6 에서 멈추고, 끝에 0~9 수준을 따로 적는다(한글 2022 이상).
        attr1 |= ((std::min<std::uint32_t>(ps.headingLevel, 6u) & 0x7u) << 25);
        if (ps.borderConnect) attr1 |= (1u << 28);
        if (ps.borderIgnoreMargin) attr1 |= (1u << 29);

        w.u32(attr1);
        w.i32(ps.marginLeft * 2);
        w.i32(ps.marginRight * 2);
        w.i32(ps.indent * 2);
        w.i32(ps.spacingPrev * 2);
        w.i32(ps.spacingNext * 2);
        const std::int32_t lsValue =
            (lsType == 0) ? ps.lineSpacing : (ps.lineSpacing * 2);
        w.i32(lsValue);
        w.u16(static_cast<std::uint16_t>(ps.tabDefId));
        w.u16(static_cast<std::uint16_t>(ps.headingIdRef));
        w.u16(static_cast<std::uint16_t>(ps.borderFillId > 0 ? ps.borderFillId : 1));
        w.i16(static_cast<std::int16_t>(ps.borderOffsetLeft));
        w.i16(static_cast<std::int16_t>(ps.borderOffsetRight));
        w.i16(static_cast<std::int16_t>(ps.borderOffsetTop));
        w.i16(static_cast<std::int16_t>(ps.borderOffsetBottom));

        std::uint32_t attr2 = 0;
        if (ps.lineWrap == "SQUEEZE") attr2 |= 1u;
        else if (ps.lineWrap == "KEEP") attr2 |= 2u;
        if (ps.autoSpacingEng) attr2 |= (1u << 4);
        if (ps.autoSpacingNum) attr2 |= (1u << 5);
        if (ps.suppressLineNumbers) attr2 |= (1u << 6);
        w.u32(attr2);
        w.u32(lsType);
        w.u32(static_cast<std::uint32_t>(lsValue));
        w.u32(std::min<std::uint32_t>(ps.headingLevel, 9u));
        appendRecord(stream, tag::ParaShape, 1, w.view());
    }

    // 11. Style (level 1)
    for (std::size_t i = 0; i < styles.size(); ++i) {
        const auto& st = styles[i];
        ByteWriter w;
        w.hwpString(st.name.empty() ? "바탕글" : st.name);
        w.hwpString(st.engName);
        w.u8(st.type == "CHAR" ? 1 : 0);
        w.u8(static_cast<std::uint8_t>(st.nextStyleId < styles.size() ? st.nextStyleId : i));
        w.i16(static_cast<std::int16_t>(st.langId != 0 ? st.langId : 1042));
        w.u16(static_cast<std::uint16_t>(st.paraShapeId));
        w.u16(static_cast<std::uint16_t>(st.charShapeId));
        w.u16(0);
        appendRecord(stream, tag::Style, 1, w.view());
    }

    // 메모 모양(22바이트)은 스타일 뒤에 온다.
    for (const MemoShape& m : h.memoShapes) {
        ByteWriter w;
        w.u32(m.width);
        w.u8(m.lineType);
        w.u8(m.lineWidth);
        w.u32(hexToColorRef(m.lineColor, 0x00AED7B6u));
        w.u32(hexToColorRef(m.fillColor, 0x00E9FFF0u));
        w.u32(hexToColorRef(m.activeColor, 0x00C7F1CFu));
        w.u32(m.memoType);
        appendRecord(stream, tag::MemoShape, 1, w.view());
    }

    auto writeKept = [&](std::uint16_t tagId, int level) {
        bool wrote = false;
        for (const KeptRecord& rec : h.kept) {
            if (rec.tag != tagId) continue;
            appendRecord(stream, tagId, static_cast<std::uint16_t>(level), ByteView(rec.data));
            wrote = true;
        }
        return wrote;
    };
    for (const KeptRecord& rec : h.kept) {
        if (rec.tag == tag::CompatibleDocument || rec.tag == tag::LayoutCompatibility || rec.tag == tag::TrackChange ||
            rec.tag == tag::MemoShape) {
            continue;
        }
        appendRecord(stream, rec.tag, 1, ByteView(rec.data));
    }
    // 12. CompatibleDocument(단계 0)와 그 아래 LayoutCompatibility(단계 1), 그 뒤 변경 추적(단계 1).
    // 한글은 이 차례·단계가 다르면 파일을 열지 않는다.
    if (!writeKept(tag::CompatibleDocument, 0)) {
        ByteWriter w;
        w.u32(0);
        appendRecord(stream, tag::CompatibleDocument, 0, w.view());
    }
    if (!writeKept(tag::LayoutCompatibility, 1)) {
        ByteWriter w;
        for (int i = 0; i < 5; ++i) w.u32(0);
        appendRecord(stream, tag::LayoutCompatibility, 1, w.view());
    }
    writeKept(tag::TrackChange, 1);

    return stream.take();
}

constexpr std::uint32_t kCtrlSecd = makeCtrlId('s', 'e', 'c', 'd');
constexpr std::uint32_t kCtrlCold = makeCtrlId('c', 'o', 'l', 'd');
constexpr std::uint32_t kCtrlTable = makeCtrlId('t', 'b', 'l', ' ');
constexpr std::uint32_t kCtrlGso = makeCtrlId('g', 's', 'o', ' ');
constexpr std::uint32_t kCtrlEquation = makeCtrlId('e', 'q', 'e', 'd');
constexpr std::uint32_t kCtrlHeader = makeCtrlId('h', 'e', 'a', 'd');
constexpr std::uint32_t kCtrlFooter = makeCtrlId('f', 'o', 'o', 't');
constexpr std::uint32_t kCtrlFootnote = makeCtrlId('f', 'n', ' ', ' ');
constexpr std::uint32_t kCtrlEndnote = makeCtrlId('e', 'n', ' ', ' ');
constexpr std::uint32_t kCtrlAutoNum = makeCtrlId('a', 't', 'n', 'o');
constexpr std::uint32_t kCtrlNewNum = makeCtrlId('n', 'w', 'n', 'o');
constexpr std::uint32_t kCtrlPageHiding = makeCtrlId('p', 'g', 'h', 'd');
constexpr std::uint32_t kCtrlPageNumPos = makeCtrlId('p', 'g', 'n', 'p');
constexpr std::uint32_t kCtrlBookmark = makeCtrlId('b', 'o', 'k', 'm');
constexpr std::uint32_t kCtrlHiddenComment = makeCtrlId('t', 'c', 'm', 't');
constexpr std::uint32_t kCtrlDutmal = makeCtrlId('t', 'd', 'u', 't');
constexpr std::uint32_t kCtrlCompose = makeCtrlId('t', 'c', 'p', 's');

std::uint32_t resolveCtrlId(const Control& ctrl) {
    switch (ctrl.kind) {
        case ControlKind::SectionDef: return kCtrlSecd;
        case ControlKind::ColumnDef: return kCtrlCold;
        case ControlKind::Table: return kCtrlTable;
        case ControlKind::Picture:
        case ControlKind::Shape:
        case ControlKind::Container:
        case ControlKind::Ole: return kCtrlGso;
        case ControlKind::Equation: return kCtrlEquation;
        case ControlKind::Header: return kCtrlHeader;
        case ControlKind::Footer: return kCtrlFooter;
        case ControlKind::Footnote: return kCtrlFootnote;
        case ControlKind::Endnote: return kCtrlEndnote;
        case ControlKind::AutoNumber: return kCtrlAutoNum;
        case ControlKind::NewNumber: return kCtrlNewNum;
        case ControlKind::PageHiding: return kCtrlPageHiding;
        case ControlKind::PageNumPos: return kCtrlPageNumPos;
        case ControlKind::Bookmark: return kCtrlBookmark;
        case ControlKind::HiddenComment: return kCtrlHiddenComment;
        case ControlKind::Dutmal: return kCtrlDutmal;
        case ControlKind::Compose: return kCtrlCompose;
        case ControlKind::Field:
            if (ctrl.ctrlId.size() == 4) {
                return makeCtrlId(ctrl.ctrlId[0], ctrl.ctrlId[1], ctrl.ctrlId[2], ctrl.ctrlId[3]);
            }
            return makeCtrlId('%', 'h', 'l', 'k');
        case ControlKind::Unknown:
            if (ctrl.ctrlId.size() == 4) {
                return makeCtrlId(ctrl.ctrlId[0], ctrl.ctrlId[1], ctrl.ctrlId[2], ctrl.ctrlId[3]);
            }
            return kCtrlGso;
        case ControlKind::FieldEnd: return 0;
    }
    return kCtrlGso;
}

std::uint16_t extendedCharForControl(const Control& ctrl) {
    switch (ctrl.kind) {
        case ControlKind::SectionDef:
        case ControlKind::ColumnDef: return 2;
        case ControlKind::Table:
        case ControlKind::Picture:
        case ControlKind::Shape:
        case ControlKind::Container:
        case ControlKind::Ole:
        case ControlKind::Equation: return 11;
        case ControlKind::HiddenComment: return 15;
        case ControlKind::Header:
        case ControlKind::Footer:
        case ControlKind::Bookmark:
        case ControlKind::Dutmal:
        case ControlKind::Compose: return 16;
        case ControlKind::Footnote:
        case ControlKind::Endnote: return 17;
        case ControlKind::AutoNumber:
        case ControlKind::NewNumber: return 18;
        case ControlKind::PageHiding:
        case ControlKind::PageNumPos: return 21;
        case ControlKind::Field: return 3;
        case ControlKind::FieldEnd: return 4;
        case ControlKind::Unknown: return 11;
    }
    return 11;
}

void writePlacementBytes(ByteWriter& w, const Control& ctrl) {
    const Placement p = ctrl.placement.value_or(Placement{});
    std::uint32_t attr = p.attrKeep & ~kObjectKnownBits;
    if (p.treatAsChar) attr |= 1u;

    std::uint32_t vRel = 2;  // PARA
    if (p.vertRelTo == "PAPER") vRel = 0;
    else if (p.vertRelTo == "PAGE") vRel = 1;
    attr |= ((vRel & 0x3u) << 3);

    std::uint32_t vAlign = 0;  // TOP
    if (p.vertAlign == "CENTER") vAlign = 1;
    else if (p.vertAlign == "BOTTOM") vAlign = 2;
    else if (p.vertAlign == "INSIDE") vAlign = 3;
    else if (p.vertAlign == "OUTSIDE") vAlign = 4;
    attr |= ((vAlign & 0x7u) << 5);

    std::uint32_t hRel = 3;  // PARA
    if (p.horzRelTo == "PAPER") hRel = 0;
    else if (p.horzRelTo == "PAGE") hRel = 1;
    else if (p.horzRelTo == "COLUMN") hRel = 2;
    attr |= ((hRel & 0x3u) << 8);

    std::uint32_t hAlign = 0;  // LEFT
    if (p.horzAlign == "CENTER") hAlign = 1;
    else if (p.horzAlign == "RIGHT") hAlign = 2;
    else if (p.horzAlign == "INSIDE") hAlign = 3;
    else if (p.horzAlign == "OUTSIDE") hAlign = 4;
    attr |= ((hAlign & 0x7u) << 10);

    if (p.flowWithText) attr |= (1u << 13);
    if (p.allowOverlap) attr |= (1u << 14);
    attr |= (4u << 15);  // widthRelTo = ABSOLUTE
    attr |= (2u << 18);  // heightRelTo = ABSOLUTE

    std::uint32_t wrap = 0;  // SQUARE
    if (p.textWrap == "TOP_AND_BOTTOM") wrap = 1;
    else if (p.textWrap == "BEHIND_TEXT") wrap = 2;
    else if (p.textWrap == "IN_FRONT_OF_TEXT") wrap = 3;
    attr |= ((wrap & 0x7u) << 21);

    std::uint32_t flow = 0;  // BOTH_SIDES
    if (p.textFlow == "LEFT_ONLY") flow = 1;
    else if (p.textFlow == "RIGHT_ONLY") flow = 2;
    else if (p.textFlow == "LARGEST_ONLY") flow = 3;
    attr |= ((flow & 0x3u) << 24);

    const std::string_view numbering = objectNumberingType(ctrl.kind, p.numberingType);
    std::uint32_t numberingBits = 0;  // NONE
    if (numbering == "PICTURE") numberingBits = 1;
    else if (numbering == "TABLE") numberingBits = 2;
    else if (numbering == "EQUATION") numberingBits = 3;
    attr |= (numberingBits << 26);
    if (ctrl.caption) attr |= (1u << 29);

    w.u32(attr);
    w.i32(p.vertOffset);
    w.i32(p.horzOffset);
    w.u32(p.width);
    w.u32(p.height);
    w.i32(p.zOrder);
    w.i16(static_cast<std::int16_t>(p.marginLeft));
    w.i16(static_cast<std::int16_t>(p.marginRight));
    w.i16(static_cast<std::int16_t>(p.marginTop));
    w.i16(static_cast<std::int16_t>(p.marginBottom));
    w.u32(0x50000001u);  // instance id
    w.i32(0);            // prevent page break
    w.hwpString("");     // description
}

std::uint32_t listAttrFromVertAlign(std::string_view va, std::uint32_t extraBits = 0) {
    std::uint32_t v = 0;
    if (va == "CENTER") v = 1;
    else if (va == "BOTTOM") v = 2;
    return ((v & 0x3u) << 5) | extraBits;
}

void writeParagraphRecords(ByteWriter& stream, const Paragraph& para, std::uint16_t baseLevel,
                           const HwpBinRegistry& bins, const Section* firstParaSec = nullptr,
                           bool isLastInList = false);

void writeSubListParagraphs(ByteWriter& stream, const std::vector<Paragraph>& paragraphs,
                            std::uint16_t paraLevel, const HwpBinRegistry& bins) {
    if (paragraphs.empty()) {
        Paragraph empty;
        writeParagraphRecords(stream, empty, paraLevel, bins, nullptr, true);
    } else {
        for (std::size_t i = 0; i < paragraphs.size(); ++i) {
            writeParagraphRecords(stream, paragraphs[i], paraLevel, bins, nullptr,
                                  i + 1 == paragraphs.size());
        }
    }
}

// 바탕쪽 리스트 머리(34바이트)와 문단. 한글 2024 가 저장한 바이트와 같다: 문단 수, 속성(세로 정렬), 글 너비·높이,
// u16 0, u32 종류(0 양쪽·홀수·짝수, 3 마지막 쪽, 3+n 임의 n 쪽), u32 비트(1 겹치게 하기, 2 인쇄 시 맨 위), 0 여덟 바이트.
void writeMasterList(ByteWriter& stream, const MasterPage& mp, const PageDef& pd, std::uint16_t level,
                     const HwpBinRegistry& bins) {
    ByteWriter w;
    w.u32(static_cast<std::uint32_t>(std::max<std::size_t>(1, mp.content.paragraphs.size())));
    w.u32(listAttrFromVertAlign(mp.content.vertAlign, 0));
    const std::int64_t textW = static_cast<std::int64_t>(pd.width) - pd.marginLeft - pd.marginRight;
    const std::int64_t textH = static_cast<std::int64_t>(pd.height) - pd.marginTop - pd.marginBottom - pd.marginHeader -
                               pd.marginFooter;
    w.u32(mp.content.textWidth > 0 ? mp.content.textWidth : static_cast<std::uint32_t>(std::max<std::int64_t>(0, textW)));
    w.u32(mp.content.textHeight > 0 ? mp.content.textHeight : static_cast<std::uint32_t>(std::max<std::int64_t>(0, textH)));
    w.u16(0);
    std::uint32_t code = 0;
    if (mp.applyPageType == "LAST_PAGE") code = 3;
    else if (mp.applyPageType == "OPTIONAL_PAGE") code = 3 + std::max<std::uint32_t>(1, mp.pageNumber);
    w.u32(code);
    w.u32((mp.duplicate ? 1u : 0u) | (mp.printOnTop ? 2u : 0u));
    w.zeros(8);
    appendRecord(stream, tag::ListHeader, level, w.view());
    writeSubListParagraphs(stream, mp.content.paragraphs, static_cast<std::uint16_t>(level), bins);
}

std::size_t extMasterCount(const Section& sec) {
    std::size_t n = 0;
    for (const auto& mp : sec.masterPages) {
        if (mp.applyPageType == "LAST_PAGE" || mp.applyPageType == "OPTIONAL_PAGE") ++n;
    }
    return n;
}

void writeCaptionRecords(ByteWriter& stream, const Caption& cap, std::uint16_t level,
                         const HwpBinRegistry& bins) {
    ByteWriter w;
    const std::uint32_t pCount =
        static_cast<std::uint32_t>(std::max<std::size_t>(1, cap.content.paragraphs.size()));
    w.u32(pCount);
    w.u32(listAttrFromVertAlign(cap.content.vertAlign, cap.content.textDirection & 0x7u));
    std::uint32_t sideCode = 3;  // BOTTOM
    if (cap.side == "LEFT") sideCode = 0;
    else if (cap.side == "RIGHT") sideCode = 1;
    else if (cap.side == "TOP") sideCode = 2;
    const std::uint32_t capAttr = sideCode | (cap.fullSize ? 4u : 0u);
    w.u32(capAttr);
    w.u32(cap.width > 0 ? cap.width : 8504u);
    w.i16(static_cast<std::int16_t>(cap.gap));
    w.u32(cap.lastWidth);
    w.zeros(12);
    appendRecord(stream, tag::ListHeader, level, w.view());
    writeSubListParagraphs(stream, cap.content.paragraphs, level, bins);
}

void writeParameterSetString(ByteWriter& stream, std::uint16_t level, std::uint16_t itemId,
                             std::string_view value) {
    ByteWriter w;
    w.u16(0x0218);  // parameter set ID
    w.i16(1);       // 1 item
    w.u16(0);       // reserved 2 bytes (matches hwp_reader.cpp skip(2))
    w.u16(itemId);
    w.u16(1);       // kPitBstr
    w.hwpString(value);
    appendRecord(stream, tag::CtrlData, level, w.view());
}

std::uint32_t shapeIdForControl(const Control& ctrl, std::string& st) {
    st = ctrl.shapeType;
    if (ctrl.kind == ControlKind::Picture || st == "pic") {
        st = "pic";
        return makeCtrlId('$', 'p', 'i', 'c');
    }
    if (ctrl.kind == ControlKind::Container || st == "container") {
        st = "container";
        return makeCtrlId('$', 'c', 'o', 'n');
    }
    if (ctrl.kind == ControlKind::Ole || st == "ole") {
        st = "ole";
        return makeCtrlId('$', 'o', 'l', 'e');
    }
    if (st == "ellipse") return makeCtrlId('$', 'e', 'l', 'l');
    if (st == "line") return makeCtrlId('$', 'l', 'i', 'n');
    if (st == "arc") return makeCtrlId('$', 'a', 'r', 'c');
    if (st == "polygon") return makeCtrlId('$', 'p', 'o', 'l');
    if (st == "curve") return makeCtrlId('$', 'c', 'u', 'r');
    if (st == "textart") return makeCtrlId('$', 't', 'a', 't');
    st = "rect";
    return makeCtrlId('$', 'r', 'e', 'c');
}

void writeShapeComponentRecords(ByteWriter& stream, const Control& ctrl, std::uint16_t compLevel,
                                const HwpBinRegistry& bins, int groupDepth = 0) {
    std::string st;
    const std::uint32_t shapeId = shapeIdForControl(ctrl, st);

    const Placement p = ctrl.placement.value_or(Placement{});
    const std::uint32_t curW = p.width;
    const std::uint32_t curH = p.height;
    const double sx = (p.scaleX > 1e-6) ? p.scaleX : 1.0;
    const double sy = (p.scaleY > 1e-6) ? p.scaleY : 1.0;
    const std::uint32_t initW =
        (std::fabs(sx - 1.0) > 1e-6)
            ? static_cast<std::uint32_t>(std::llround(static_cast<double>(curW) / sx))
            : curW;
    const std::uint32_t initH =
        (std::fabs(sy - 1.0) > 1e-6)
            ? static_cast<std::uint32_t>(std::llround(static_cast<double>(curH) / sy))
            : curH;

    {
        ByteWriter w;
        w.u32(shapeId);
        if (groupDepth == 0) {
            w.u32(shapeId);  // 최상위 SHAPE_COMPONENT만 ID를 2회 기록
        }
        w.i32(p.groupX - p.matrixX);
        w.i32(p.groupY - p.matrixY);
        w.u16(static_cast<std::uint16_t>(groupDepth));
        w.u16(1);
        w.u32(initW);
        w.u32(initH);
        w.u32(curW);
        w.u32(curH);
        std::uint32_t flipFlags = 0;
        if (p.flipHorizontal) flipFlags |= 1u;
        if (p.flipVertical) flipFlags |= 2u;
        if (!ctrl.subLists.empty()) flipFlags |= 0x01000000u;  // 글상자(LIST_HEADER) 존재 플래그
        if (st == "pic") flipFlags |= 0x24080000u;
        else if (st == "container") flipFlags |= 0x00090000u;
        else flipFlags |= 0x00080000u;
        w.u32(flipFlags);
        w.u16(0);  // rotation angle
        w.i32(static_cast<std::int32_t>(curW / 2));
        w.i32(static_cast<std::int32_t>(curH / 2));
        const std::uint16_t matrixPairs = static_cast<std::uint16_t>(groupDepth + 1);
        w.u16(matrixPairs);

        // transMatrix (6 doubles = 48 bytes)
        const std::array<double, 6> identMat = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
        for (double v : identMat) w.f64(v);

        // scaMatrix + rotMatrix pairs
        const std::array<double, 6> scaMat = {
            sx, 0.0, static_cast<double>(p.matrixX), 0.0, sy, static_cast<double>(p.matrixY)};
        for (double v : scaMat) w.f64(v);
        for (double v : identMat) w.f64(v);
        for (int g = 0; g < groupDepth; ++g) {
            for (double v : identMat) w.f64(v);
            for (double v : identMat) w.f64(v);
        }

        const bool hasInk = (st == "rect" || st == "ellipse" || st == "line" || st == "arc" ||
                             st == "polygon" || st == "curve");
        if (hasInk) {
            const std::uint8_t lineKind = toHwpBorderKind(ctrl.strokeType);
            const std::uint32_t strokeAttr =
                static_cast<std::uint32_t>(lineKind & 0x3Fu) |
                ((static_cast<std::uint32_t>(ctrl.strokeEnd) & 0xFu) << 6);
            w.u32(hexToColorRef(ctrl.strokeColor, 0x00000000u));
            w.i32(lineKind != 0 ? ctrl.strokeWidth : 0);
            w.u32(strokeAttr);
            w.u8(0);

            if (!ctrl.fillColor.empty() && ctrl.fillColor != "none") {
                w.u32(1u);  // solid fill
                w.u32(hexToColorRef(ctrl.fillColor, 0x00FFFFFFu));
                w.u32(0xFFFFFFFFu);
                w.i32(-1);
                w.u32(0u);
                w.u8(0);
            } else {
                w.u32(0u);
                w.u32(0u);
            }
            // 그림자(16B) + instanceId(4B) + 여분(2B) = 22 bytes
            w.u32(0u);
            w.u32(0x00B2B2B2u);
            w.i32(0);
            w.i32(0);
            w.u32(0u);
            w.u16(0u);
        } else if (st == "container") {
            w.u16(static_cast<std::uint16_t>(ctrl.children.size()));
            for (const auto& child : ctrl.children) {
                std::string childSt;
                w.u32(shapeIdForControl(child, childSt));
            }
            w.u32(0u);  // instanceId
        }
        appendRecord(stream, tag::ShapeComponent, compLevel, w.view());
    }

    // 글상자(LIST_HEADER + 소속 문단)는 도형별 세부 레코드보다 먼저 온다.
    if (!ctrl.subLists.empty()) {
        const auto& sub = ctrl.subLists.front();
        ByteWriter w;
        const std::uint32_t pCount =
            static_cast<std::uint32_t>(std::max<std::size_t>(1, sub.paragraphs.size()));
        w.u32(pCount);
        w.u32(listAttrFromVertAlign(sub.vertAlign, sub.textDirection & 0x7u));
        w.i16(static_cast<std::int16_t>(ctrl.boxPadLeft));
        w.i16(static_cast<std::int16_t>(ctrl.boxPadRight));
        w.i16(static_cast<std::int16_t>(ctrl.boxPadTop));
        w.i16(static_cast<std::int16_t>(ctrl.boxPadBottom));
        w.u32(ctrl.boxTextWidth > 0 ? ctrl.boxTextWidth : p.width);
        w.zeros(13);
        appendRecord(stream, tag::ListHeader, compLevel + 1, w.view());
        writeSubListParagraphs(stream, sub.paragraphs, compLevel + 1, bins);
    }

    const std::int32_t rw = static_cast<std::int32_t>(initW);
    const std::int32_t rh = static_cast<std::int32_t>(initH);
    // 모델의 점은 현재 크기 안의 좌표다. 파일에는 처음 크기 안의 좌표로 줄여 적는다.
    std::vector<ShapePoint> points = ctrl.points;
    for (ShapePoint& point : points) {
        if (curW > 0 && initW != curW) {
            point.x = static_cast<std::int32_t>(std::llround(static_cast<double>(point.x) * initW / curW));
        }
        if (curH > 0 && initH != curH) {
            point.y = static_cast<std::int32_t>(std::llround(static_cast<double>(point.y) * initH / curH));
        }
    }

    if (st == "pic") {
        ByteWriter w;
        const std::uint8_t lineKind = toHwpBorderKind(ctrl.strokeType);
        const std::uint32_t borderAttr =
            static_cast<std::uint32_t>(lineKind & 0x3Fu) |
            ((static_cast<std::uint32_t>(ctrl.strokeEnd) & 0xFu) << 6);
        w.u32(hexToColorRef(ctrl.strokeColor, 0x00000000u));
        w.i32(lineKind != 0 ? ctrl.strokeWidth : 0);
        w.u32(borderAttr);

        const std::int32_t dimW =
            ctrl.picture.originWidth > 0 ? ctrl.picture.originWidth : static_cast<std::int32_t>(initW);
        const std::int32_t dimH =
            ctrl.picture.originHeight > 0 ? ctrl.picture.originHeight : static_cast<std::int32_t>(initH);
        const std::int32_t rectW = ctrl.picture.rectWidth > 0 ? ctrl.picture.rectWidth : dimW;
        const std::int32_t rectH = ctrl.picture.rectHeight > 0 ? ctrl.picture.rectHeight : dimH;
        w.i32(0);
        w.i32(0);
        w.i32(rectW);
        w.i32(0);
        w.i32(rectW);
        w.i32(rectH);
        w.i32(0);
        w.i32(rectH);

        w.i32(ctrl.picture.clipLeft);
        w.i32(ctrl.picture.clipTop);
        w.i32(ctrl.picture.clipRight);
        w.i32(ctrl.picture.clipBottom);

        w.i16(static_cast<std::int16_t>(ctrl.picture.padLeft));
        w.i16(static_cast<std::int16_t>(ctrl.picture.padRight));
        w.i16(static_cast<std::int16_t>(ctrl.picture.padTop));
        w.i16(static_cast<std::int16_t>(ctrl.picture.padBottom));

        w.i8(static_cast<std::int8_t>(ctrl.picture.bright));
        w.i8(static_cast<std::int8_t>(ctrl.picture.contrast));
        w.u8(static_cast<std::uint8_t>(ctrl.picture.effect));
        w.u16(bins.resolve(ctrl.binItemId));
        w.u8(0);  // border transparency
        w.u32(0x50000002u);
        w.u32(0);  // effectFlags = 0
        w.i32(dimW);
        w.i32(dimH);
        w.i8(static_cast<std::int8_t>(std::clamp(ctrl.picture.transparency, 0, 255)));
        appendRecord(stream, tag::ShapeComponentPicture, compLevel + 1, w.view());
    } else if (st == "rect") {
        ByteWriter w;
        w.u8(static_cast<std::uint8_t>(std::clamp(ctrl.cornerRatio, 0, 100)));
        w.i32(0);
        w.i32(0);
        w.i32(rw);
        w.i32(0);
        w.i32(rw);
        w.i32(rh);
        w.i32(0);
        w.i32(rh);
        appendRecord(stream, tag::ShapeComponentRectangle, compLevel + 1, w.view());
    } else if (st == "line") {
        ByteWriter w;
        if (points.size() >= 2) {
            w.i32(points[0].x);
            w.i32(points[0].y);
            w.i32(points[1].x);
            w.i32(points[1].y);
        } else {
            w.i32(0);
            w.i32(0);
            w.i32(rw);
            w.i32(rh);
        }
        w.u32(0u);
        appendRecord(stream, tag::ShapeComponentLine, compLevel + 1, w.view());
    } else if (st == "ellipse") {
        ByteWriter w;
        w.u32(0u);
        w.i32(rw / 2);
        w.i32(rh / 2);
        w.i32(rw);
        w.i32(rh / 2);
        w.i32(rw / 2);
        w.i32(0);
        w.zeros(32);
        appendRecord(stream, tag::ShapeComponentEllipse, compLevel + 1, w.view());
    } else if (st == "arc") {
        ByteWriter w;
        w.u8(0u);
        w.i32(rw / 2);
        w.i32(rh / 2);
        w.i32(rw);
        w.i32(rh / 2);
        w.i32(rw / 2);
        w.i32(0);
        appendRecord(stream, tag::ShapeComponentArc, compLevel + 1, w.view());
    } else if (st == "polygon") {
        ByteWriter w;
        if (points.size() >= 2) {
            const bool closed = (points.front().x == points.back().x &&
                                 points.front().y == points.back().y);
            const std::uint32_t cnt =
                static_cast<std::uint32_t>(points.size() + (closed ? 0 : 1));
            w.u32(cnt);
            for (const auto& pt : points) {
                w.i32(pt.x);
                w.i32(pt.y);
            }
            if (!closed) {
                w.i32(points.front().x);
                w.i32(points.front().y);
            }
        } else {
            w.u32(5u);
            w.i32(0);
            w.i32(0);
            w.i32(rw);
            w.i32(0);
            w.i32(rw);
            w.i32(rh);
            w.i32(0);
            w.i32(rh);
            w.i32(0);
            w.i32(0);
        }
        appendRecord(stream, tag::ShapeComponentPolygon, compLevel + 1, w.view());
    } else if (st == "curve") {
        ByteWriter w;
        if (points.size() >= 2) {
            const std::uint32_t cnt = static_cast<std::uint32_t>(points.size());
            w.u32(cnt);
            for (const auto& pt : points) {
                w.i32(pt.x);
                w.i32(pt.y);
            }
            for (std::uint32_t i = 0; i + 1 < cnt; ++i) w.u8(1u);
            w.zeros(4);
        } else {
            w.u32(2u);
            w.i32(0);
            w.i32(0);
            w.i32(rw);
            w.i32(rh);
            w.u8(1u);
            w.zeros(4);
        }
        appendRecord(stream, tag::ShapeComponentCurve, compLevel + 1, w.view());
    } else if (st == "container") {
        for (const auto& child : ctrl.children) {
            writeShapeComponentRecords(stream, child, compLevel + 1, bins, groupDepth + 1);
        }
    }
    for (const KeptRecord& rec : ctrl.kept) {
        if (rec.tag == tag::ShapeComponentLine || rec.tag == tag::ShapeComponentEllipse ||
            rec.tag == tag::ShapeComponentArc || rec.tag == tag::ShapeComponentPolygon ||
            rec.tag == tag::ShapeComponentCurve || rec.tag == tag::ShapeComponentRectangle ||
            rec.tag == tag::ShapeComponentPicture || rec.tag == tag::ShapeComponent) {
            continue;
        }
        appendRecord(stream, rec.tag, static_cast<std::uint16_t>(compLevel + 1), ByteView(rec.data));
    }
}

void writeControlRecords(ByteWriter& stream, const Control& ctrl, std::uint16_t ctrlLevel,
                         const HwpBinRegistry& bins, const Section* firstParaSec) {
    const std::uint32_t id = resolveCtrlId(ctrl);
    switch (ctrl.kind) {
        case ControlKind::SectionDef: {
            PageDef pd;
            if (ctrl.page) pd = *ctrl.page;
            else if (firstParaSec && firstParaSec->page) pd = *firstParaSec->page;

            // 구역 설정은 구역 값이 기준이다(편집은 구역과 구역 정의를 같이 고친다). 구역이 없으면 컨트롤 값이다.
            const bool hideHeader = firstParaSec ? firstParaSec->hideFirstHeader : ctrl.hideFirstHeader;
            const bool hideFooter = firstParaSec ? firstParaSec->hideFirstFooter : ctrl.hideFirstFooter;
            const bool hideMaster = firstParaSec ? firstParaSec->hideFirstMaster : ctrl.hideFirstMaster;
            const bool hidePageNum = firstParaSec ? firstParaSec->hideFirstPageNum : ctrl.hideFirstPageNum;
            const std::string& borderShow = firstParaSec ? firstParaSec->borderShow : ctrl.borderShow;
            const std::string& fillShow = firstParaSec ? firstParaSec->fillShow : ctrl.fillShow;
            const bool hideEmptyLine = firstParaSec ? firstParaSec->hideFirstEmptyLine : ctrl.hideFirstEmptyLine;
            const LineNumberShape lineNumbers = firstParaSec ? firstParaSec->lineNumbers : ctrl.lineNumbers;
            const std::uint32_t startsOn = firstParaSec ? firstParaSec->startsOn : ctrl.startsOn;
            const std::uint16_t pictureStart = firstParaSec ? firstParaSec->pictureStart : ctrl.pictureStart;
            const std::uint16_t tableStart = firstParaSec ? firstParaSec->tableStart : ctrl.tableStart;
            const std::uint16_t equationStart = firstParaSec ? firstParaSec->equationStart : ctrl.equationStart;
            const std::int32_t colRegionGap =
                ctrl.columnRegionGap > 0
                    ? ctrl.columnRegionGap
                    : (firstParaSec && firstParaSec->columnRegionGap > 0
                           ? firstParaSec->columnRegionGap
                           : 1134);
            const std::int32_t defaultTab =
                ctrl.defaultTab > 0
                    ? ctrl.defaultTab
                    : (firstParaSec && firstParaSec->defaultTab > 0 ? firstParaSec->defaultTab
                                                                     : 4000);
            const std::uint32_t pageStart =
                ctrl.number > 0 ? ctrl.number : (firstParaSec ? firstParaSec->pageStart : 0);
            // 격자는 구역 값이 기준이다. 편집은 구역만 고친다.
            const std::int32_t lineGrid = firstParaSec ? firstParaSec->lineGrid : ctrl.lineGrid;
            const std::int32_t charGrid = firstParaSec ? firstParaSec->charGrid : ctrl.charGrid;
            const bool wongoji = firstParaSec ? firstParaSec->wongoji : ctrl.wongoji;
            const std::uint32_t textDirection = firstParaSec ? firstParaSec->textDirection : ctrl.textDirection;
            // 모델이 풀지 않는 속성 비트는 읽은 그대로 쓴다. 읽은 값이 없으면(새 문서, HWPX) 비운다.
            const std::optional<std::uint32_t> keptAttr =
                firstParaSec && firstParaSec->sectionAttrKeep ? firstParaSec->sectionAttrKeep : ctrl.sectionAttrKeep;

            {
                ByteWriter w;
                w.u32(kCtrlSecd);
                std::uint32_t attr = keptAttr ? (*keptAttr & ~kSecdKnownBits) : 0u;
                if (hideHeader) attr |= 0x01u;
                if (hideFooter) attr |= 0x02u;
                if (hideMaster) attr |= 0x04u;
                if (hidePageNum) attr |= 0x20u;
                if (borderShow == "HIDE_FIRST") attr |= 0x08u;
                if (borderShow == "SHOW_FIRST") attr |= 0x100u;
                if (fillShow == "HIDE_FIRST") attr |= 0x10u;
                if (fillShow == "SHOW_FIRST") attr |= 0x200u;
                if (hideEmptyLine) attr |= 1u << 19;
                if (lineNumbers.show) attr |= 1u << 24;
                // 사용자 시작 번호는 종류 3 이다(한글 2024). 옛 모델은 번호만 있었다.
                attr |= (std::min<std::uint32_t>(pageStart > 0 && startsOn == 0 ? 3u : startsOn, 3u) & 0x3u) << 20;
                if (wongoji) attr |= 1u << 22;
                // 양쪽·짝수·홀수 바탕쪽이 있으면 비트 29·30·31 이다(한글 2024). 마지막 쪽·임의 쪽은 아래 확장 수다.
                if (firstParaSec) {
                    for (const auto& mp : firstParaSec->masterPages) {
                        if (mp.applyPageType == "EVEN") attr |= 1u << 30;
                        else if (mp.applyPageType == "ODD") attr |= 1u << 31;
                        else if (mp.applyPageType != "LAST_PAGE" && mp.applyPageType != "OPTIONAL_PAGE") attr |= 1u << 29;
                    }
                }
                // 글자 방향은 한글 2024 처럼 속성 비트 16–18 이 아니라 아래 36번째 바이트에만 쓴다.
                w.u32(attr);
                w.u16(static_cast<std::uint16_t>(colRegionGap));
                w.u16(static_cast<std::uint16_t>(std::clamp(lineGrid, 0, 0xFFFF)));
                w.u16(static_cast<std::uint16_t>(std::clamp(charGrid, 0, 0xFFFF)));
                w.u32(static_cast<std::uint32_t>(defaultTab * 2));
                w.u16(1);
                w.u16(static_cast<std::uint16_t>(pageStart));
                w.u16(pictureStart);
                w.u16(tableStart);
                w.u16(equationStart);
                // 한글 2024 처럼 47바이트로 쓴다. 대표 언어 2바이트 뒤 확장 바탕쪽(마지막 쪽·임의 쪽) 수 2바이트,
                // 4바이트를 비우고, 36번째 바이트에 글자 방향 × 2 를 둔다.
                w.u16(0);
                w.u16(static_cast<std::uint16_t>(firstParaSec ? extMasterCount(*firstParaSec) : 0));
                w.u16(0);
                // 34번째 바이트: 구역 메모 모양 번호(1 부터, 0 기본).
                w.u16(firstParaSec ? firstParaSec->memoShapeId : ctrl.memoShapeId);
                w.u8(static_cast<std::uint8_t>((std::min<std::uint32_t>(textDirection, 2) & 0x3u) << 1));
                // 줄 번호(38번째 바이트부터): restart, countBy, distance, start. 보이지 않아도 값은 남긴다(한글 2024).
                w.u8(0);
                w.u8(lineNumbers.restart);
                w.u16(lineNumbers.countBy);
                w.u32(lineNumbers.distance);
                w.u16(lineNumbers.start);
                appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            }
            {
                ByteWriter w;
                w.u32(pd.storedWidth());
                w.u32(pd.storedHeight());
                w.u32(pd.marginLeft);
                w.u32(pd.marginRight);
                w.u32(pd.marginTop);
                w.u32(pd.marginBottom);
                w.u32(pd.marginHeader);
                w.u32(pd.marginFooter);
                w.u32(pd.marginGutter);
                w.u32((pd.landscape ? 1u : 0u) | (static_cast<std::uint32_t>(std::min<int>(pd.gutterType, 2)) << 1));
                appendRecord(stream, tag::PageDef, ctrlLevel + 1, w.view());
            }
            // 각주·미주 모양. 한글 2024 레코드 28바이트: 속성, 사용자 기호·앞 장식·뒤 장식(WCHAR), 시작 번호,
            // 구분선 길이(INT32), 구분선 위·아래·주 사이, 선 종류·굵기, 선 색.
            for (int fn = 0; fn < 2; ++fn) {
                const NoteShape& shape = firstParaSec ? (fn == 0 ? firstParaSec->footnote : firstParaSec->endnote)
                                                      : (fn == 0 ? ctrl.footnoteShape : ctrl.endnoteShape);
                ByteWriter w;
                std::uint32_t attr = noteNumberShapeCode(shape.numFormat);
                attr |= (static_cast<std::uint32_t>(std::clamp(shape.place, 0, 3)) & 0x3u) << 8;
                attr |= (static_cast<std::uint32_t>(std::clamp(shape.numbering, 0, 3)) & 0x3u) << 10;
                if (shape.contentSuperscript) attr |= 0x1000u;
                if (shape.beneathText) attr |= 0x2000u;
                w.u32(attr);
                w.u16(firstWchar(shape.userChar));
                w.u16(firstWchar(shape.prefix));
                w.u16(firstWchar(shape.suffix));
                w.u16(shape.startNumber);
                w.u32(static_cast<std::uint32_t>(shape.lineLength));
                w.u16(static_cast<std::uint16_t>(std::clamp(shape.above, 0, 0xFFFF)));
                w.u16(static_cast<std::uint16_t>(std::clamp(shape.below, 0, 0xFFFF)));
                w.u16(static_cast<std::uint16_t>(std::clamp(shape.between, 0, 0xFFFF)));
                w.u8(shape.lineType.empty() || shape.lineType == "NONE" ? 0 : toHwpBorderKind(shape.lineType));
                w.u8(toHwpBorderThick(shape.lineWidth));
                w.u32(hexToColorRef(shape.lineColor));
                appendRecord(stream, tag::FootnoteShape, ctrlLevel + 1, w.view());
            }
            const auto& pageBorders = firstParaSec ? firstParaSec->pageBorders : ctrl.pageBorders;
            for (const auto& border : pageBorders) {
                ByteWriter w;
                std::uint32_t attr = 0;
                if (border.textBorder) attr |= 0x1u;
                if (border.headerInside) attr |= 0x2u;
                if (border.footerInside) attr |= 0x4u;
                attr |= (static_cast<std::uint32_t>(border.fillArea) & 0x3u) << 3;
                w.u32(attr);
                w.i16(border.left);
                w.i16(border.right);
                w.i16(border.top);
                w.i16(border.bottom);
                w.u16(border.borderFillId);
                appendRecord(stream, tag::PageBorderFill, ctrlLevel + 1, w.view());
            }
            // 양쪽·짝수·홀수 바탕쪽을 그 차례로 구역 정의 아래에 쓴다(비트 29·30·31 차례).
            if (firstParaSec) {
                for (const char* kind : {"BOTH", "EVEN", "ODD"}) {
                    for (const auto& mp : firstParaSec->masterPages) {
                        const bool both = mp.applyPageType != "EVEN" && mp.applyPageType != "ODD" &&
                                          mp.applyPageType != "LAST_PAGE" && mp.applyPageType != "OPTIONAL_PAGE";
                        if (both ? std::string_view(kind) != "BOTH" : mp.applyPageType != kind) continue;
                        writeMasterList(stream, mp, pd, ctrlLevel + 1, bins);
                        break;
                    }
                }
            }
            break;
        }
        case ControlKind::ColumnDef: {
            ByteWriter w;
            w.u32(kCtrlCold);
            std::uint16_t attr = 0;
            if (ctrl.columnType == "PARALLEL") attr |= 2u;
            else if (ctrl.columnType == "BALANCED_NEWSPAPER") attr |= 1u;
            const std::uint16_t cnt =
                static_cast<std::uint16_t>(std::clamp<std::uint32_t>(ctrl.columnCount, 1, 255));
            attr |= static_cast<std::uint16_t>((cnt & 0xFFu) << 2);
            if (ctrl.columnLayout == "RIGHT") attr |= (1u << 10);
            else if (ctrl.columnLayout == "BOTH") attr |= (2u << 10);
            const bool sameSz = ctrl.columnSameSize || ctrl.columnWidths.empty();
            if (sameSz) attr |= (1u << 12);
            w.u16(attr);
            w.u16(static_cast<std::uint16_t>(std::max<std::int32_t>(0, ctrl.columnGap)));
            if (!sameSz) {
                for (std::uint16_t i = 0; i < cnt; ++i) {
                    const std::int32_t sw =
                        i < ctrl.columnWidths.size() ? ctrl.columnWidths[i].width : 0;
                    w.u16(static_cast<std::uint16_t>(std::max<std::int32_t>(0, sw)));
                }
            }
            w.u16(0);  // attr high
            BorderSide divSide;
            divSide.type = ctrl.columnLineType;
            divSide.width = ctrl.columnLineWidth;
            divSide.color = ctrl.columnLineColor;
            writeBorderSideBytes(w, divSide);
            appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            break;
        }
        case ControlKind::Table: {
            if (!ctrl.table) break;
            const Table& tbl = *ctrl.table;
            {
                ByteWriter w;
                w.u32(kCtrlTable);
                writePlacementBytes(w, ctrl);
                appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            }
            if (ctrl.caption) {
                writeCaptionRecords(stream, *ctrl.caption, ctrlLevel + 1, bins);
            }
            std::uint16_t rowCount = static_cast<std::uint16_t>(std::max<std::uint32_t>(1, tbl.rowCount));
            for (const auto& c : tbl.cells) {
                if (c.row + 1 > rowCount) rowCount = static_cast<std::uint16_t>(c.row + 1);
            }
            std::vector<std::uint16_t> rowCellCounts(rowCount, 0);
            for (const auto& c : tbl.cells) {
                if (c.row < rowCellCounts.size()) ++rowCellCounts[c.row];
            }
            {
                ByteWriter w;
                std::uint32_t attr = 2u;  // CELL
                if (tbl.pageBreak == "NONE") attr = 0u;
                else if (tbl.pageBreak == "TABLE") attr = 1u;
                if (tbl.repeatHeader) attr |= (1u << 2);
                if (tbl.noAdjust) attr |= (1u << 4);
                w.u32(attr);
                w.u16(rowCount);
                w.u16(static_cast<std::uint16_t>(std::max<std::uint32_t>(1, tbl.colCount)));
                w.u16(static_cast<std::uint16_t>(tbl.cellSpacing));
                w.i16(static_cast<std::int16_t>(tbl.marginLeft));
                w.i16(static_cast<std::int16_t>(tbl.marginRight));
                w.i16(static_cast<std::int16_t>(tbl.marginTop));
                w.i16(static_cast<std::int16_t>(tbl.marginBottom));
                for (std::uint16_t rc : rowCellCounts) w.u16(rc);
                w.u16(static_cast<std::uint16_t>(tbl.borderFillId > 0 ? tbl.borderFillId : 1));
                w.u16(0);  // zone count
                appendRecord(stream, tag::Table, ctrlLevel + 1, w.view());
            }
            for (const auto& c : tbl.cells) {
                ByteWriter w;
                const std::uint32_t pCount = static_cast<std::uint32_t>(
                    std::max<std::size_t>(1, c.content.paragraphs.size()));
                w.u32(pCount);
                std::uint32_t extra = c.content.textDirection & 0x7u;
                if (c.hasMargin) extra |= (1u << 16);
                if (c.header) extra |= (1u << 18);
                const std::string_view va =
                    !c.vertAlign.empty() ? std::string_view(c.vertAlign)
                                         : std::string_view(c.content.vertAlign);
                w.u32(listAttrFromVertAlign(va, extra));
                w.u16(static_cast<std::uint16_t>(c.col));
                w.u16(static_cast<std::uint16_t>(c.row));
                w.u16(static_cast<std::uint16_t>(c.colSpan));
                w.u16(static_cast<std::uint16_t>(c.rowSpan));
                w.u32(c.width);
                w.u32(c.height);
                w.i16(static_cast<std::int16_t>(c.marginLeft));
                w.i16(static_cast<std::int16_t>(c.marginRight));
                w.i16(static_cast<std::int16_t>(c.marginTop));
                w.i16(static_cast<std::int16_t>(c.marginBottom));
                w.u16(static_cast<std::uint16_t>(c.borderFillId > 0 ? c.borderFillId : 1));
                w.u32(c.width);
                if (c.name.empty()) {
                    w.u8(0);
                    w.hwpString("");
                } else {
                    // 셀 필드 이름: 0xff, 파라미터 셋(ID 0x021b, 아이템 0x4000 문자열), 0 여덟 바이트
                    // (숨긴 한글 2024 가 저장한 바이트와 같다).
                    w.u8(0xff);
                    w.u16(0x021b);
                    w.u16(1);
                    w.u16(0);
                    w.u16(0x4000);
                    w.u16(1);
                    w.hwpString(c.name);
                    w.zeros(8);
                }
                appendRecord(stream, tag::ListHeader, ctrlLevel + 1, w.view());
                writeSubListParagraphs(stream, c.content.paragraphs, ctrlLevel + 1, bins);
            }
            break;
        }
        case ControlKind::Picture:
        case ControlKind::Shape:
        case ControlKind::Container:
        case ControlKind::Ole: {
            {
                ByteWriter w;
                w.u32(kCtrlGso);
                writePlacementBytes(w, ctrl);
                appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            }
            if (ctrl.caption) {
                writeCaptionRecords(stream, *ctrl.caption, ctrlLevel + 1, bins);
            }
            writeShapeComponentRecords(stream, ctrl, ctrlLevel + 1, bins);
            break;
        }
        case ControlKind::Equation: {
            {
                ByteWriter w;
                w.u32(kCtrlEquation);
                writePlacementBytes(w, ctrl);
                appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            }
            if (ctrl.caption) {
                writeCaptionRecords(stream, *ctrl.caption, ctrlLevel + 1, bins);
            }
            {
                ByteWriter w;
                w.u32(0);  // attr
                w.hwpString(ctrl.script);
                w.u32(static_cast<std::uint32_t>(ctrl.baseUnit > 0 ? ctrl.baseUnit : 1000));
                w.u32(0x00000000u);
                w.i16(static_cast<std::int16_t>(ctrl.baseLine >= 0 ? ctrl.baseLine : 85));
                w.u16(0);
                w.hwpString("Equation Version 60");
                w.hwpString(ctrl.equationFont.empty() ? "HancomEQN" : ctrl.equationFont);
                appendRecord(stream, tag::EqEdit, ctrlLevel + 1, w.view());
            }
            break;
        }
        case ControlKind::Header:
        case ControlKind::Footer: {
            {
                ByteWriter w;
                w.u32(id);
                std::uint32_t applyCode = 0;
                if (ctrl.applyPageType == "EVEN") applyCode = 1;
                else if (ctrl.applyPageType == "ODD") applyCode = 2;
                w.u32(applyCode);
                w.u32(0);
                appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            }
            const SubList sub = ctrl.subLists.empty() ? SubList{} : ctrl.subLists.front();
            {
                ByteWriter w;
                const std::uint32_t pCount =
                    static_cast<std::uint32_t>(std::max<std::size_t>(1, sub.paragraphs.size()));
                w.u32(pCount);
                w.u32(listAttrFromVertAlign(sub.vertAlign, sub.textDirection & 0x7u));
                w.u32(sub.textWidth);
                w.u32(sub.textHeight);
                appendRecord(stream, tag::ListHeader, ctrlLevel + 1, w.view());
            }
            writeSubListParagraphs(stream, sub.paragraphs, ctrlLevel + 1, bins);
            break;
        }
        case ControlKind::Footnote:
        case ControlKind::Endnote: {
            {
                ByteWriter w;
                w.u32(id);
                w.u32(ctrl.number > 0 ? ctrl.number : 1);
                w.zeros(12);
                appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            }
            const SubList sub = ctrl.subLists.empty() ? SubList{} : ctrl.subLists.front();
            {
                ByteWriter w;
                const std::uint32_t pCount =
                    static_cast<std::uint32_t>(std::max<std::size_t>(1, sub.paragraphs.size()));
                w.u32(pCount);
                w.u32(listAttrFromVertAlign(sub.vertAlign, sub.textDirection & 0x7u));
                appendRecord(stream, tag::ListHeader, ctrlLevel + 1, w.view());
            }
            writeSubListParagraphs(stream, sub.paragraphs, ctrlLevel + 1, bins);
            break;
        }
        case ControlKind::AutoNumber:
        case ControlKind::NewNumber: {
            ByteWriter w;
            w.u32(id);
            std::uint32_t numType = 0;
            if (ctrl.numType == "FOOTNOTE") numType = 1;
            else if (ctrl.numType == "ENDNOTE") numType = 2;
            else if (ctrl.numType == "PICTURE") numType = 3;
            else if (ctrl.numType == "TABLE") numType = 4;
            else if (ctrl.numType == "EQUATION") numType = 5;
            const std::uint32_t attr =
                (numType & 0xFu) | ((toHwpNumFormat(ctrl.numFormat) & 0xFFu) << 4);
            w.u32(attr);
            w.u16(static_cast<std::uint16_t>(ctrl.number > 0 ? ctrl.number : 1));
            w.u16(0);
            w.u16(static_cast<std::uint16_t>(firstUtf16Char(ctrl.prefix)));
            w.u16(static_cast<std::uint16_t>(firstUtf16Char(ctrl.suffix)));
            appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            break;
        }
        case ControlKind::PageHiding: {
            ByteWriter w;
            w.u32(kCtrlPageHiding);
            std::uint32_t attr = 0;
            if (ctrl.hideHeader) attr |= 0x01u;
            if (ctrl.hideFooter) attr |= 0x02u;
            if (ctrl.hideMaster) attr |= 0x04u;
            if (ctrl.hideBorder) attr |= 0x08u;
            if (ctrl.hideFill) attr |= 0x10u;
            if (ctrl.hidePageNum) attr |= 0x20u;
            w.u32(attr);
            appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            break;
        }
        case ControlKind::PageNumPos: {
            ByteWriter w;
            w.u32(kCtrlPageNumPos);
            static constexpr std::string_view kPlaces[] = {
                "NONE",        "TOP_LEFT",       "TOP_CENTER",   "TOP_RIGHT",
                "BOTTOM_LEFT", "BOTTOM_CENTER",  "BOTTOM_RIGHT", "OUTSIDE_TOP",
                "OUTSIDE_BOTTOM", "INSIDE_TOP", "INSIDE_BOTTOM"};
            std::uint32_t place = 5;  // BOTTOM_CENTER
            for (std::size_t i = 0; i < std::size(kPlaces); ++i) {
                if (ctrl.pageNumPos == kPlaces[i]) {
                    place = static_cast<std::uint32_t>(i);
                    break;
                }
            }
            const std::uint32_t attr =
                (toHwpNumFormat(ctrl.numFormat) & 0xFFu) | ((place & 0xFu) << 8);
            w.u32(attr);
            // 한글 2024 처럼 사용자 기호·앞·뒤 장식 문자는 0 이고, 줄표(sideChar)는 넷째 글자다.
            w.u16(0);
            w.u16(0);
            w.u16(0);
            w.u16(static_cast<std::uint16_t>(firstUtf16Char(ctrl.prefix)));
            appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            break;
        }
        case ControlKind::Bookmark: {
            ByteWriter w;
            w.u32(kCtrlBookmark);
            appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            writeParameterSetString(stream, ctrlLevel + 1, 0x4000, ctrl.name);
            break;
        }
        case ControlKind::Field: {
            ByteWriter w;
            if (isMemoField(ctrl)) {
                // 한글은 메모 필드의 CTRL_HEADER 를 %unk 로 쓰고 글 쪽 확장 문자에 %%me 를 둔다(한글 2024 와 같다).
                // 명령 안 메모 번호와 꼬리의 메모 번호는 문서 차례로 다시 매긴 값이다.
                const std::uint32_t number = gMemos.number.count(&ctrl) ? gMemos.number[&ctrl] : 1u;
                MemoCommand cmd = parseMemoCommand(ctrl.command);
                cmd.number = number;
                w.u32(makeCtrlId('%', 'u', 'n', 'k'));
                w.u32(ctrl.fieldProps);
                w.u8(ctrl.fieldExtra);
                w.hwpString(memoCommandText(cmd));
                w.u32(gMemos.id.count(&ctrl) ? gMemos.id[&ctrl] : ctrl.fieldId);
                w.u32(number);
                appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
                break;
            }
            const bool proof = proofKindOf(ctrl) != ProofKind::None;
            // 한글 2024 는 교정 부호(HWP 5.0 뒤에 생긴 필드)의 CTRL_HEADER 를 %unk 로 쓰고 글 쪽에 진짜 id 를 둔다.
            w.u32(proof ? makeCtrlId('%', 'u', 'n', 'k') : id);
            w.u32(ctrl.fieldProps);
            w.u8(ctrl.fieldExtra);
            w.hwpString(ctrl.command);
            w.u32(ctrl.fieldId);
            if (proof) w.u32(ctrl.memoNumber);
            appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            if (!ctrl.name.empty()) {
                writeParameterSetString(stream, ctrlLevel + 1, 0x4000, ctrl.name);
            }
            break;
        }
        case ControlKind::HiddenComment: {
            {
                ByteWriter w;
                w.u32(kCtrlHiddenComment);
                appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            }
            const SubList sub = ctrl.subLists.empty() ? SubList{} : ctrl.subLists.front();
            {
                ByteWriter w;
                const std::uint32_t pCount =
                    static_cast<std::uint32_t>(std::max<std::size_t>(1, sub.paragraphs.size()));
                w.u32(pCount);
                w.u32(listAttrFromVertAlign(sub.vertAlign, sub.textDirection & 0x7u));
                appendRecord(stream, tag::ListHeader, ctrlLevel + 1, w.view());
            }
            writeSubListParagraphs(stream, sub.paragraphs, ctrlLevel + 1, bins);
            break;
        }
        case ControlKind::Dutmal: {
            ByteWriter w;
            w.u32(kCtrlDutmal);
            w.hwpString(ctrl.text);
            w.hwpString(ctrl.subText);
            w.u32(dutmalPosCode(ctrl.dutmalPos));
            w.u32(ctrl.dutmalSizeRatio);
            w.u32(ctrl.dutmalOption);
            w.u32(ctrl.dutmalStyle);
            w.u32(dutmalAlignCode(ctrl.dutmalAlign));
            appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            break;
        }
        case ControlKind::Compose: {
            ByteWriter w;
            w.u32(kCtrlCompose);
            w.hwpString(ctrl.text);
            w.u8(static_cast<std::uint8_t>(composeCircleCode(ctrl.composeCircle)));
            w.u8(static_cast<std::uint8_t>(ctrl.composeCharSize));
            w.u8(ctrl.composeType == "SPREAD" ? 0 : 1);
            // 한글은 글자 모양을 늘 10 칸 쓰고 남는 칸은 0xFFFFFFFF 다.
            std::vector<std::uint32_t> shapes = ctrl.composeCharShapes;
            if (shapes.size() < 10) shapes.resize(10, 0xFFFFFFFFu);
            w.u8(static_cast<std::uint8_t>(std::min<std::size_t>(shapes.size(), 255)));
            for (std::size_t i = 0; i < shapes.size() && i < 255; ++i) w.u32(shapes[i]);
            appendRecord(stream, tag::CtrlHeader, ctrlLevel, w.view());
            break;
        }
        case ControlKind::Unknown:
            break;
    }
}

void writeParagraphRecords(ByteWriter& stream, const Paragraph& para, std::uint16_t baseLevel,
                           const HwpBinRegistry& bins, const Section* firstParaSec,
                           bool isLastInList) {
    ByteWriter textWriter;
    std::uint32_t wcharPos = 0;
    std::uint32_t ctrlMask = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> charShapeMap;
    struct MarkRange {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        std::uint32_t colorRef24 = 0;
    };
    std::vector<MarkRange> markRanges;

    struct PendingControl {
        const Control* ptr = nullptr;
        Control synthSecCtrl{};
        bool isSynthSec = false;
    };
    std::vector<PendingControl> controlsToEmit;

    auto emitExtendedCtrlChar = [&](std::uint16_t chCode, std::uint32_t ctrlId) {
        ctrlMask |= (1u << chCode);
        textWriter.u16(chCode);
        textWriter.u32(ctrlId);
        for (int k = 0; k < 4; ++k) textWriter.u16(0);
        textWriter.u16(chCode);
        wcharPos += 8;
    };

    std::uint32_t openFieldId = makeCtrlId('%', 'h', 'l', 'k');
    // 열린 필드(안쪽이 뒤). 메모 필드 끝은 그 메모 번호를 둔다(한글 2024: 끝 문자 셋째 u32, 답글이 겹치면 안쪽부터).
    std::vector<const Control*> openFields;
    auto emitInlineCtrlChar = [&](std::uint16_t chCode) {
        ctrlMask |= (1u << chCode);
        textWriter.u16(chCode);
        for (int k = 0; k < 6; ++k) textWriter.u16(0);
        textWriter.u16(chCode);
        wcharPos += 8;
    };

    bool hasExplicitSecDef = false;
    for (const auto& r : para.runs) {
        for (const auto& it : r.items) {
            if (it.type == InlineItem::Type::Control &&
                it.control.kind == ControlKind::SectionDef) {
                hasExplicitSecDef = true;
                break;
            }
        }
        if (hasExplicitSecDef) break;
    }

    if (firstParaSec && !hasExplicitSecDef) {
        charShapeMap.emplace_back(0, para.runs.empty() ? 0 : para.runs.front().charShapeId);
        PendingControl pc;
        pc.isSynthSec = true;
        pc.synthSecCtrl.kind = ControlKind::SectionDef;
        pc.synthSecCtrl.ctrlId = "secd";
        pc.synthSecCtrl.page = firstParaSec->page;
        pc.synthSecCtrl.defaultTab = firstParaSec->defaultTab;
        pc.synthSecCtrl.columnRegionGap = firstParaSec->columnRegionGap;
        pc.synthSecCtrl.number = firstParaSec->pageStart;
        pc.synthSecCtrl.hideHeader = firstParaSec->hideHeader;
        pc.synthSecCtrl.hideFooter = firstParaSec->hideFooter;
        pc.synthSecCtrl.hideMaster = firstParaSec->hideMaster;
        pc.synthSecCtrl.hidePageNum = firstParaSec->hidePageNum;
        pc.synthSecCtrl.pageBorders = firstParaSec->pageBorders;
        controlsToEmit.push_back(std::move(pc));
        emitExtendedCtrlChar(2, kCtrlSecd);
    }

    for (const auto& r : para.runs) {
        if (charShapeMap.empty() || charShapeMap.back().second != r.charShapeId) {
            if (!charShapeMap.empty() && charShapeMap.back().first == wcharPos) {
                charShapeMap.back().second = r.charShapeId;
            } else {
                charShapeMap.emplace_back(wcharPos, r.charShapeId);
            }
        }
        for (const auto& item : r.items) {
            const std::uint32_t itemStartPos = wcharPos;
            if (item.type == InlineItem::Type::Text) {
                const std::u16string u16 = utf8ToUtf16(item.text);
                for (char16_t ch : u16) {
                    if (ch == u'\t') {
                        emitInlineCtrlChar(9);
                    } else if (ch == u'\n') {
                        ctrlMask |= (1u << 10);
                        textWriter.u16(10);
                        ++wcharPos;
                    } else if (ch == 0x00A0) {
                        ctrlMask |= (1u << 30);
                        textWriter.u16(30);
                        ++wcharPos;
                    } else if (ch == 0x2007) {
                        ctrlMask |= (1u << 31);
                        textWriter.u16(31);
                        ++wcharPos;
                    } else if (ch >= 32) {
                        textWriter.u16(static_cast<std::uint16_t>(ch));
                        ++wcharPos;
                    }
                }
            } else if (item.control.kind == ControlKind::FieldEnd) {
                // 한글 2024: 필드 끝은 인라인 문자 4 에 시작 컨트롤 ID 의 아래 세 바이트를 둔다(%hlk → "klh\0").
                ctrlMask |= (1u << 4);
                textWriter.u16(4);
                const Control* closing = openFields.empty() ? nullptr : openFields.back();
                if (!openFields.empty()) openFields.pop_back();
                const std::uint32_t closingId = closing ? resolveCtrlId(*closing) : openFieldId;
                textWriter.u32(closingId & 0x00FFFFFFu);
                if (closing && isMemoField(*closing)) {
                    textWriter.u32(0x00FFFF01u);
                    textWriter.u32(gMemos.number.count(closing) ? gMemos.number[closing] : 0u);
                } else if (closing && proofKindOf(*closing) != ProofKind::None) {
                    textWriter.u32(0);
                    textWriter.u32(closing->memoNumber);
                } else {
                    for (int k = 0; k < 4; ++k) textWriter.u16(0);
                }
                textWriter.u16(4);
                wcharPos += 8;
            } else if (item.control.kind != ControlKind::Unknown) {
                const std::uint16_t chCode = extendedCharForControl(item.control);
                if (item.control.kind == ControlKind::Field) {
                    openFieldId = resolveCtrlId(item.control);
                    openFields.push_back(&item.control);
                }
                const std::uint32_t ctrlId = resolveCtrlId(item.control);
                emitExtendedCtrlChar(chCode, ctrlId);
                PendingControl pc;
                pc.ptr = &item.control;
                controlsToEmit.push_back(pc);
            }
            if (!item.markpen.empty() && item.markpen != "none" && wcharPos > itemStartPos) {
                const std::uint32_t cref = hexToColorRef(item.markpen, 0x00FFFF00u) & 0xFFFFFFu;
                markRanges.push_back(MarkRange{itemStartPos, wcharPos, cref});
            }
        }
    }

    if (charShapeMap.empty()) {
        charShapeMap.emplace_back(0, 0);
    }

    // 문단 끝 문자 (CR = 13)
    textWriter.u16(13);
    ++wcharPos;

    // 형광펜 밖의 영역(글자 겹침·책갈피 등)은 읽은 그대로 다시 쓴다. 형광펜(종류 2)은 위에서 글자로부터 다시 만들었다.
    std::vector<const TextRange*> keptRanges;
    for (const TextRange& range : para.ranges) {
        if (range.kind != 2) keptRanges.push_back(&range);
    }
    // 한글은 문단 머리의 영역 개수가 PARA_RANGE_TAG 의 항목 수와 다르면 파일을 열지 않는다.
    const std::size_t rangeCount = markRanges.size() + keptRanges.size();

    // 1. ParaHeader
    {
        ByteWriter w;
        w.u32(wcharPos | (isLastInList ? 0x80000000u : 0u));
        w.u32(ctrlMask);
        w.u16(static_cast<std::uint16_t>(para.paraShapeId));
        w.u8(static_cast<std::uint8_t>(para.styleId));
        std::uint8_t brk = 0;
        if (para.layoutBreak) brk |= 0x02u;
        if (para.pageBreak) brk |= 0x04u;
        if (para.columnBreak) brk |= 0x08u;
        w.u8(brk);
        w.u16(static_cast<std::uint16_t>(charShapeMap.size()));
        w.u16(static_cast<std::uint16_t>(rangeCount));
        w.u16(static_cast<std::uint16_t>(para.lineSegs.size()));
        w.u32(0);
        w.u16(0);
        appendRecord(stream, tag::ParaHeader, baseLevel, w.view());
    }

    // 2. ParaText (빈 문단 wcharPos == 1 은 PARA_TEXT 레코드를 생략한다)
    if (wcharPos > 1) {
        appendRecord(stream, tag::ParaText, baseLevel + 1, textWriter.view());
    }

    // 3. ParaCharShape
    {
        ByteWriter w;
        for (const auto& [pos, csId] : charShapeMap) {
            w.u32(pos);
            w.u32(csId);
        }
        appendRecord(stream, tag::ParaCharShape, baseLevel + 1, w.view());
    }

    // 4. ParaLineSeg
    if (!para.lineSegs.empty()) {
        ByteWriter w;
        for (const auto& seg : para.lineSegs) {
            w.u32(seg.textPos);
            w.i32(seg.vertPos);
            w.i32(seg.vertSize);
            w.i32(seg.textHeight);
            w.i32(seg.baseline);
            w.i32(seg.spacing);
            w.i32(seg.horzPos);
            w.i32(seg.horzSize);
            w.u32(seg.flags);
        }
        appendRecord(stream, tag::ParaLineSeg, baseLevel + 1, w.view());
    }

    // 5. ParaRangeTag. 형광펜은 글자에서 다시 만들고, 그 외 종류는 읽어 둔 영역을 그대로 적는다.
    if (rangeCount > 0) {
        ByteWriter w;
        for (const auto& mr : markRanges) {
            w.u32(mr.start);
            w.u32(mr.end);
            w.u32((2u << 24) | (mr.colorRef24 & 0xFFFFFFu));
        }
        for (const TextRange* range : keptRanges) {
            w.u32(range->start);
            w.u32(range->end);
            w.u32((range->kind << 24) | (range->data & 0xFFFFFFu));
        }
        appendRecord(stream, tag::ParaRangeTag, baseLevel + 1, w.view());
    }
    for (const KeptRecord& rec : para.kept) {
        if (rec.tag == tag::ParaRangeTag || rec.tag == tag::ParaText || rec.tag == tag::ParaCharShape ||
            rec.tag == tag::ParaLineSeg || rec.tag == tag::CtrlHeader) {
            continue;
        }
        appendRecord(stream, rec.tag, static_cast<std::uint16_t>(baseLevel + 1), ByteView(rec.data));
    }

    // 6. Controls
    for (const auto& pc : controlsToEmit) {
        const Control& c = pc.isSynthSec ? pc.synthSecCtrl : *pc.ptr;
        writeControlRecords(stream, c, baseLevel + 1, bins,
                            c.kind == ControlKind::SectionDef ? firstParaSec : nullptr);
    }
}

Bytes buildSectionStream(const Section& sec, const HwpBinRegistry& bins, bool lastSection) {
    ByteWriter stream;
    if (sec.paragraphs.empty()) {
        Paragraph empty;
        writeParagraphRecords(stream, empty, 0, bins, &sec, true);
    } else {
        for (std::size_t i = 0; i < sec.paragraphs.size(); ++i) {
            writeParagraphRecords(stream, sec.paragraphs[i], 0, bins,
                                  i == 0 ? &sec : nullptr,
                                  i + 1 == sec.paragraphs.size());
        }
    }
    // 마지막 쪽·임의 쪽 바탕쪽은 마지막 문단의 자식(레벨 1)으로, 마지막 쪽 다음 임의 쪽 차례다(한글 2024).
    const PageDef pd = sec.page.value_or(PageDef{});
    for (const char* kind : {"LAST_PAGE", "OPTIONAL_PAGE"}) {
        for (const auto& mp : sec.masterPages) {
            if (mp.applyPageType == kind) writeMasterList(stream, mp, pd, 1, bins);
        }
    }
    // 메모 글: 마지막 구역 마지막 문단의 자식(레벨 1)으로 메모마다 MEMO_LIST(번호), LIST_HEADER(16바이트), 문단을 둔다.
    if (lastSection) {
        for (const Control* memo : gMemos.order) {
            {
                ByteWriter w;
                w.u32(gMemos.number[memo]);
                appendRecord(stream, tag::MemoList, 1, w.view());
            }
            const std::vector<Paragraph> empty(1);
            const std::vector<Paragraph>& paras =
                memo->subLists.empty() || memo->subLists.front().paragraphs.empty() ? empty : memo->subLists.front().paragraphs;
            ByteWriter w;
            w.u32(static_cast<std::uint32_t>(paras.size()));
            w.u32(0);
            w.zeros(8);
            appendRecord(stream, tag::ListHeader, 1, w.view());
            writeSubListParagraphs(stream, paras, 1, bins);
        }
    }
    return stream.take();
}

Bytes buildSummaryInformationStream(const Metadata& m) {
    struct PropEntry {
        std::uint32_t pid = 0;
        std::string value;
    };
    std::vector<PropEntry> strProps;
    auto addIfNotEmpty = [&](std::uint32_t pid, const std::string& val) {
        if (!val.empty()) strProps.push_back(PropEntry{pid, val});
    };
    addIfNotEmpty(2, m.title);
    addIfNotEmpty(3, m.subject);
    addIfNotEmpty(4, m.author);
    addIfNotEmpty(5, m.keywords);
    addIfNotEmpty(6, m.comments);
    addIfNotEmpty(8, m.lastSavedBy);

    // PID 1 (CodePage = 1200 UTF-16LE) + strProps
    const std::uint32_t totalProps = static_cast<std::uint32_t>(1 + strProps.size());
    const std::uint32_t tableBytes = 8 + totalProps * 8;

    ByteWriter valuesWriter;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> propOffsets;

    // PID 1: VT_I2 (0x0002), value = 1200 (UTF-16)
    propOffsets.emplace_back(1u, tableBytes + static_cast<std::uint32_t>(valuesWriter.size()));
    valuesWriter.u32(0x0002);
    valuesWriter.u16(1200);
    valuesWriter.u16(0);

    for (const auto& sp : strProps) {
        propOffsets.emplace_back(sp.pid,
                                 tableBytes + static_cast<std::uint32_t>(valuesWriter.size()));
        valuesWriter.u32(0x001F);  // VT_LPWSTR
        const std::u16string u16 = utf8ToUtf16(sp.value);
        const std::uint32_t charCountWithNull = static_cast<std::uint32_t>(u16.size() + 1);
        valuesWriter.u32(charCountWithNull);
        valuesWriter.utf16(u16);
        valuesWriter.u16(0);
        if ((charCountWithNull * 2) % 4 != 0) {
            valuesWriter.u16(0);
        }
    }

    const std::uint32_t sectionSize =
        tableBytes + static_cast<std::uint32_t>(valuesWriter.size());

    ByteWriter out;
    out.u16(0xFFFE);  // byte order
    out.u16(0);       // version
    out.u32(0x00020105);
    out.zeros(16);    // CLSID
    out.u32(1);       // 1 property section
    // FMTID {F29F85E0-4FF9-1068-AB91-08002B27B3D9}
    static constexpr std::uint8_t kFmtId[16] = {0xE0, 0x85, 0x9F, 0xF2, 0xF9, 0x4F, 0x68, 0x10,
                                                0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9};
    out.bytes(kFmtId, sizeof(kFmtId));
    out.u32(48);  // section offset = 48

    out.u32(sectionSize);
    out.u32(totalProps);
    for (const auto& [pid, off] : propOffsets) {
        out.u32(pid);
        out.u32(off);
    }
    out.bytes(valuesWriter.view());
    return out.take();
}

Bytes buildPrvTextStream(const Document& doc) {
    std::string text = doc.previewText;
    if (text.empty()) {
        for (const auto& sec : doc.sections) {
            for (const auto& p : sec.paragraphs) {
                if (!text.empty()) text += "\r\n";
                text += p.text();
            }
        }
    }
    ByteWriter w;
    w.utf16(utf8ToUtf16(text));
    return w.take();
}

Bytes buildFileHeaderStream(bool compressed) {
    Bytes header(256, 0);
    static constexpr char kSig[] = "HWP Document File";
    std::memcpy(header.data(), kSig, sizeof(kSig) - 1);
    writeU32LE(header.data() + 32, 0x05010100u);  // 5.1.1.0
    writeU32LE(header.data() + 36, compressed ? 1u : 0u);
    return header;
}

bool maybeCompress(ByteView raw, bool compress, Bytes& out, std::string* error) {
    if (!compress) {
        out.assign(raw.begin(), raw.end());
        return true;
    }
    if (!zip::deflateRaw(raw, out)) {
        if (error) *error = "HWP stream deflate failed";
        return false;
    }
    return true;
}

}  // namespace

bool write(const model::Document& doc, Bytes& out, const WriteOptions& options,
           std::string* error) {
    out.clear();
    cfb::CompoundWriter cfb;

    // 1. FileHeader (256 bytes, 항상 비압축)
    cfb.addStream("FileHeader", buildFileHeaderStream(options.compress));

    const HwpBinRegistry bins = buildHwpBinRegistry(doc, options.sourceData);

    // 메모 번호·필드 id 를 매긴다.
    gMemos = MemoWriteContext{};
    gMemos.order = collectMemos(doc);
    {
        std::uint32_t maxId = 0;
        for (const Control* m : gMemos.order) maxId = std::max(maxId, m->fieldId);
        std::uint32_t nextId = std::max<std::uint32_t>(maxId + 1, 0x40000000u);
        for (std::size_t i = 0; i < gMemos.order.size(); ++i) {
            gMemos.number[gMemos.order[i]] = static_cast<std::uint32_t>(i + 1);
            gMemos.id[gMemos.order[i]] = gMemos.order[i]->fieldId != 0 ? gMemos.order[i]->fieldId : nextId++;
        }
    }

    // 2. DocInfo
    {
        const Bytes rawDocInfo = buildDocInfoStream(doc, bins, options.compress);
        Bytes storedDocInfo;
        if (!maybeCompress(rawDocInfo, options.compress, storedDocInfo, error)) return false;
        cfb.addStream("DocInfo", std::move(storedDocInfo));
    }

    // 3. BodyText/SectionN
    std::vector<Section> fallbackSections;
    const std::vector<Section>* sectionsPtr = &doc.sections;
    if (sectionsPtr->empty()) {
        fallbackSections.emplace_back();
        sectionsPtr = &fallbackSections;
    }
    for (std::size_t i = 0; i < sectionsPtr->size(); ++i) {
        const Bytes rawSec = buildSectionStream((*sectionsPtr)[i], bins, i + 1 == sectionsPtr->size());
        Bytes storedSec;
        if (!maybeCompress(rawSec, options.compress, storedSec, error)) return false;
        cfb.addStream("BodyText/Section" + std::to_string(i), std::move(storedSec));
    }

    // 4. BinData/BINXXXX.ext
    for (const auto& b : bins.entries) {
        if (!b.isLink && !b.rawData.empty()) {
            Bytes storedBin;
            if (!maybeCompress(b.rawData, options.compress, storedBin, error)) return false;
            cfb.addStream(b.streamPath, std::move(storedBin));
        }
    }

    // 답글 관계(MemoExtended): 레코드 태그 101 하나에 개수와 (답글 필드 id, 원 메모 필드 id) 쌍. 답글이 없으면 쓰지 않는다.
    {
        ByteWriter w;
        std::uint32_t count = 0;
        for (const Control* m : gMemos.order) count += m->memoParentId != 0 ? 1 : 0;
        if (count > 0) {
            w.u32(count);
            for (const Control* m : gMemos.order) {
                if (m->memoParentId == 0) continue;
                w.u32(gMemos.id[m]);
                w.u32(m->memoParentId);
            }
            ByteWriter stream;
            appendRecord(stream, 101, 0, w.view());
            Bytes stored;
            if (!maybeCompress(stream.take(), options.compress, stored, error)) return false;
            cfb.addStream("MemoExtended", std::move(stored));
        }
    }

    // 5. \x05HwpSummaryInformation & PrvText
    cfb.addStream("\x05HwpSummaryInformation", buildSummaryInformationStream(doc.metadata));
    cfb.addStream("PrvText", buildPrvTextStream(doc));

    return cfb.build(out, error);
}

}  // namespace chwp::hwp

