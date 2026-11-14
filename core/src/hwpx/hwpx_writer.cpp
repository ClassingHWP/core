#include "chwp/hwpx/hwpx_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <pugixml.hpp>
#include <random>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "chwp/common/crypto.hpp"
#include "chwp/container/cfb.hpp"
#include "chwp/container/zip.hpp"
#include "chwp/model/table_formula.hpp"
#include "chwp/model/memo.hpp"
#include "chwp/model/proofreading.hpp"
#include "chwp/reader.hpp"
#include "chwp/unit.hpp"

namespace chwp::hwpx {

namespace {

// 필드 시작·끝 짝. 시작마다 번호를 주고, 끝은 아직 닫히지 않은 마지막 시작을 가리킨다(문서마다 새로 센다).
struct FieldIds {
    std::uint32_t next = 1;
    std::vector<std::uint32_t> open;
    // 메모: 문서 차례 번호(1 부터)와 필드 고유 id. 쓰기 처음에 매긴다(답글 관계가 id 를 쓴다).
    std::unordered_map<const chwp::model::Control*, std::uint32_t> memoNumber;
    std::unordered_map<const chwp::model::Control*, std::uint32_t> memoId;
};
thread_local FieldIds gFieldIds;

using namespace chwp::model;

std::string serializeXml(const pugi::xml_document& doc) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\" ?>";
    doc.save(oss, "", pugi::format_raw | pugi::format_no_declaration, pugi::encoding_utf8);
    return oss.str();
}

std::string formatColor(Color c, const char* noneValue = "none") {
    if (!c) return noneValue;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(*c & 0xFFFFFFu));
    return buf;
}

std::string formatFloat(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    std::string s(buf);
    while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.') {
        s.pop_back();
    }
    if (s.size() >= 2 && s.substr(s.size() - 2) == ".0") {
        s.resize(s.size() - 2);
    }
    return s;
}

// hwpx_reader.cpp 의 hwpxLineType 과 대칭:
// 모델의 "DASH" 는 HWPX XML 에서 "DOT", 모델의 "DOT" 은 HWPX XML 에서 "DASH" 로 기록한다.
std::string toHwpxLineType(std::string_view type) {
    if (type.empty()) return "NONE";
    std::string key(type);
    for (char& c : key) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (key == "DASH") return "DOT";
    if (key == "DOT") return "DASH";
    return key;
}

const char* outlineToken(unsigned code) {
    static const char* names[] = {"NONE", "SOLID", "DOT", "THICK", "DASH", "DASH_DOT", "DASH_DOT_DOT"};
    return code < std::size(names) ? names[code] : "NONE";
}

const char* shadowToken(unsigned code) {
    if (code == 2) return "CONTINUOUS";
    if (code == 1 || code == 3) return "DROP";
    return "NONE";
}

std::string emphasisToken(unsigned code) {
    static const char* names[] = {"NONE", "DOT", "CIRCLE", "CARON", "TILDE", "COMMA", "COLON"};
    if (code < std::size(names)) return names[code];
    return std::to_string(code);
}

std::string toHwpxBorderWidth(std::int32_t widthHu) {
    if (widthHu <= 0) return "0.1 mm";
    static constexpr double kMmTable[] = {0.1, 0.12, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5,
                                          0.6, 0.7,  1.0,  1.5, 2.0,  3.0, 4.0, 5.0};
    static constexpr const char* kStrTable[] = {
        "0.1 mm", "0.12 mm", "0.15 mm", "0.2 mm", "0.25 mm", "0.3 mm", "0.4 mm", "0.5 mm",
        "0.6 mm", "0.7 mm",  "1.0 mm",  "1.5 mm", "2.0 mm",  "3.0 mm", "4.0 mm", "5.0 mm"};
    std::size_t bestIdx = 0;
    std::int32_t bestDiff = std::abs(widthHu - mmToHu(kMmTable[0]));
    for (std::size_t i = 1; i < std::size(kMmTable); ++i) {
        const std::int32_t d = std::abs(widthHu - mmToHu(kMmTable[i]));
        if (d < bestDiff) {
            bestDiff = d;
            bestIdx = i;
        }
    }
    return kStrTable[bestIdx];
}

const char* toHwpxPictureEffect(int effect) {
    switch (effect) {
        case 1: return "GRAY_SCALE";
        case 2: return "BLACK_WHITE";
        case 4: return "PATTERN8x8";
        default: return "REAL_PIC";
    }
}

const char* toHwpxFieldType(std::string_view ctrlId) {
    if (const char* proof = proofHwpxType(ctrlId)) return proof;
    if (ctrlId == "%hlk") return "HYPERLINK";
    if (ctrlId == "%clk") return "CLICK_HERE";
    if (ctrlId == "%bmk") return "BOOKMARK";
    if (ctrlId == "%fmu") return "FORMULA";
    if (ctrlId == "%dte") return "DATE";
    if (ctrlId == "%ddt") return "DOC_DATE";
    if (ctrlId == "%pat") return "PATH";
    if (ctrlId == "%xrf") return "CROSSREF";
    if (ctrlId == "%mmg") return "MAILMERGE";
    if (ctrlId == "%%me") return "MEMO";
    if (ctrlId == "%smr") return "SUMMERY";  // 한글 2024 가 쓰는 철자 그대로(SUMMARY 가 아니다)
    if (ctrlId == "%usr") return "USER_INFO";
    if (ctrlId == "%toc") return "TABLEOFCONTENTS";
    return nullptr;
}

// BinItem 매핑 도우미: HWP("1", "2"...) 및 HWPX("image1"...) 모두 일관된 HWPX manifest id 로 매핑.
struct BinRegistry {
    struct Entry {
        std::string id;       // manifest id (예: "image1")
        std::string zipPath;  // "BinData/image1.png"
        std::string mediaType;
        std::string ext;
        bool embedded = true;
        // OLE 처럼 묶음 안에 있지만 한글이 isEmbeded="0" 으로 적는 항목(STORAGE).
        bool storage = false;
        Bytes data;
    };

    std::vector<Entry> entries;
    std::unordered_map<std::string, std::string> idMap;

    std::string resolveId(const std::string& rawId) const {
        if (rawId.empty()) return {};
        auto it = idMap.find(rawId);
        if (it != idMap.end()) return it->second;
        return rawId;
    }
};

std::string guessMediaType(std::string_view ext, std::string_view fallback) {
    if (!fallback.empty()) return std::string(fallback);
    std::string lower(ext);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (lower == "png") return "image/png";
    if (lower == "jpg" || lower == "jpeg") return "image/jpeg";
    if (lower == "gif") return "image/gif";
    if (lower == "bmp") return "image/bmp";
    if (lower == "tif" || lower == "tiff") return "image/tiff";
    if (lower == "svg") return "image/svg+xml";
    if (lower == "wmf") return "image/x-wmf";
    if (lower == "emf") return "image/x-emf";
    return "application/octet-stream";
}

BinRegistry buildBinRegistry(const Document& doc, ByteView sourceData) {
    BinRegistry reg;
    std::unordered_set<std::string> usedIds;
    std::size_t seq = 1;

    for (std::size_t idx = 0; idx < doc.header.binItems.size(); ++idx) {
        const auto& item = doc.header.binItems[idx];
        BinRegistry::Entry e;
        e.ext = item.extension.empty() ? "bin" : item.extension;
        if (!e.ext.empty() && e.ext.front() == '.') e.ext.erase(e.ext.begin());

        std::string candidateId = item.id;
        // HWP 의 숫자 ID("1")도 HWPX 에서 그대로 써도 무방하지만, 비어 있으면 "imageN" 부여
        while (candidateId.empty() || usedIds.count(candidateId) > 0) {
            candidateId = "image" + std::to_string(seq++);
        }
        usedIds.insert(candidateId);
        e.id = candidateId;

        if (!item.path.empty() && item.path.rfind("BinData/", 0) == 0 &&
            doc.format.format == SourceFormat::Hwpx) {
            e.zipPath = item.path;
        } else {
            e.zipPath = "BinData/" + e.id + "." + e.ext;
        }
        e.mediaType = guessMediaType(e.ext, item.mediaType);
        e.embedded = item.storage != "LINK";
        e.storage = item.storage == "STORAGE";

        if (!item.data.empty()) {
            e.data = item.data;
        } else if (!sourceData.empty() && e.embedded) {
            readEmbeddedBin(sourceData, doc, item, e.data);
        }

        if (!item.id.empty()) reg.idMap[item.id] = e.id;
        reg.idMap[std::to_string(idx + 1)] = e.id;
        reg.entries.push_back(std::move(e));
    }
    return reg;
}

void writeFillBrushNode(pugi::xml_node parent, const std::string& faceColor,
                        const std::string& gradColor, std::int32_t gradAngle,
                        const std::string& imageId, int imageMode, const BinRegistry& bins,
                        const std::string& hatchStyle = {}, const std::string& hatchColor = {}) {
    const bool hasFace = !faceColor.empty() && faceColor != "none";
    const bool hasGrad = !gradColor.empty() && gradColor != "none";
    const bool hasImg = !imageId.empty();
    const bool hasHatch = !hatchStyle.empty() && hatchStyle != "NONE";
    if (!hasFace && !hasGrad && !hasImg && !hasHatch) return;

    auto fb = parent.append_child("hc:fillBrush");
    if ((hasFace || hasHatch) && !hasGrad) {
        // 한글은 무늬만 있으면 faceColor 를 none 으로 적는다.
        auto wb = fb.append_child("hc:winBrush");
        wb.append_attribute("faceColor") = hasFace ? faceColor.c_str() : "none";
        wb.append_attribute("hatchColor") = hasHatch && !hatchColor.empty() ? hatchColor.c_str() : "#000000";
        if (hasHatch) wb.append_attribute("hatchStyle") = hatchStyle.c_str();
        wb.append_attribute("alpha") = "0";
    }
    if (hasGrad) {
        auto grad = fb.append_child("hc:gradation");
        grad.append_attribute("type") = "LINEAR";
        grad.append_attribute("angle") = gradAngle;
        grad.append_attribute("centerX") = 0;
        grad.append_attribute("centerY") = 0;
        grad.append_attribute("step") = 50;
        grad.append_attribute("colorNum") = 2;
        grad.append_attribute("stepCenter") = 50;
        grad.append_attribute("alpha") = "0";
        auto c0 = grad.append_child("hc:color");
        c0.append_attribute("value") = (hasFace ? faceColor : "#FFFFFF").c_str();
        auto c1 = grad.append_child("hc:color");
        c1.append_attribute("value") = gradColor.c_str();
    }
    if (hasImg) {
        auto ib = fb.append_child("hc:imgBrush");
        const char* modeStr = imageMode == 6 ? "CENTER" : (imageMode == 0 ? "TILE" : "TOTAL");
        ib.append_attribute("mode") = modeStr;
        auto img = ib.append_child("hc:img");
        img.append_attribute("binaryItemIDRef") = bins.resolveId(imageId).c_str();
        img.append_attribute("bright") = 0;
        img.append_attribute("contrast") = 0;
        img.append_attribute("effect") = "REAL_PIC";
        img.append_attribute("alpha") = "0";
    }
}

std::string writeHeaderXml(const Document& doc, const BinRegistry& bins) {
    const DocHeader& h = doc.header;
    pugi::xml_document xml;
    auto head = xml.append_child("hh:head");
    head.append_attribute("xmlns:ha") = "http://www.hancom.co.kr/hwpml/2011/app";
    head.append_attribute("xmlns:hp") = "http://www.hancom.co.kr/hwpml/2011/paragraph";
    head.append_attribute("xmlns:hp10") = "http://www.hancom.co.kr/hwpml/2016/paragraph";
    head.append_attribute("xmlns:hs") = "http://www.hancom.co.kr/hwpml/2011/section";
    head.append_attribute("xmlns:hc") = "http://www.hancom.co.kr/hwpml/2011/core";
    head.append_attribute("xmlns:hh") = "http://www.hancom.co.kr/hwpml/2011/head";
    head.append_attribute("xmlns:hhs") = "http://www.hancom.co.kr/hwpml/2011/history";
    head.append_attribute("xmlns:hm") = "http://www.hancom.co.kr/hwpml/2011/master-page";
    head.append_attribute("xmlns:hpf") = "http://www.hancom.co.kr/schema/2011/hpf";
    head.append_attribute("xmlns:dc") = "http://purl.org/dc/elements/1.1/";
    head.append_attribute("xmlns:opf") = "http://www.idpf.org/2007/opf/";
    head.append_attribute("xmlns:ooxmlchart") = "http://www.hancom.co.kr/hwpml/2016/ooxmlchart";
    head.append_attribute("xmlns:hwpunitchar") = "http://www.hancom.co.kr/hwpml/2016/HwpUnitChar";
    head.append_attribute("xmlns:epub") = "http://www.idpf.org/2007/ops";
    head.append_attribute("xmlns:config") = "urn:oasis:names:tc:opendocument:xmlns:config:1.0";
    head.append_attribute("version") = "1.4";
    head.append_attribute("secCnt") =
        static_cast<unsigned>(std::max<std::size_t>(1, doc.sections.size()));

    auto beginNum = head.append_child("hh:beginNum");
    beginNum.append_attribute("page") = h.beginNumbers.page;
    beginNum.append_attribute("footnote") = h.beginNumbers.footnote;
    beginNum.append_attribute("endnote") = h.beginNumbers.endnote;
    beginNum.append_attribute("pic") = h.beginNumbers.picture;
    beginNum.append_attribute("tbl") = h.beginNumbers.table;
    beginNum.append_attribute("equation") = h.beginNumbers.equation;

    auto refList = head.append_child("hh:refList");

    // 1. fontfaces
    static constexpr std::array<const char*, kLangCount> kLangNames = {
        "HANGUL", "LATIN", "HANJA", "JAPANESE", "OTHER", "SYMBOL", "USER"};
    auto fontfaces = refList.append_child("hh:fontfaces");
    fontfaces.append_attribute("itemCnt") = static_cast<unsigned>(kLangCount);
    for (std::size_t bucket = 0; bucket < kLangCount; ++bucket) {
        const auto& fonts = h.fonts[bucket];
        auto ff = fontfaces.append_child("hh:fontface");
        ff.append_attribute("lang") = kLangNames[bucket];
        ff.append_attribute("fontCnt") = static_cast<unsigned>(fonts.empty() ? 1 : fonts.size());
        if (fonts.empty()) {
            auto f = ff.append_child("hh:font");
            f.append_attribute("id") = 0;
            f.append_attribute("face") = "함초롬바탕";
            f.append_attribute("type") = "TTF";
            f.append_attribute("isEmbedded") = 0;
        } else {
            for (std::size_t i = 0; i < fonts.size(); ++i) {
                const auto& font = fonts[i];
                auto f = ff.append_child("hh:font");
                f.append_attribute("id") = font.id != 0 || i == 0 ? font.id : static_cast<unsigned>(i);
                f.append_attribute("face") = font.face.empty() ? "함초롬바탕" : font.face.c_str();
                f.append_attribute("type") = font.type.empty() ? "TTF" : font.type.c_str();
                f.append_attribute("isEmbedded") = 0;
                if (!font.subst.empty()) {
                    auto sub = f.append_child("hh:substFont");
                    sub.append_attribute("face") = font.subst.c_str();
                    sub.append_attribute("type") = "TTF";
                    sub.append_attribute("isEmbedded") = 0;
                }
            }
        }
    }

    // 2. borderFills (1-based ID)
    auto borderFills = refList.append_child("hh:borderFills");
    const std::size_t bfCount = std::max<std::size_t>(1, h.borderFills.size());
    borderFills.append_attribute("itemCnt") = static_cast<unsigned>(bfCount);
    for (std::size_t i = 0; i < bfCount; ++i) {
        const BorderFill bf = i < h.borderFills.size() ? h.borderFills[i] : BorderFill{};
        auto node = borderFills.append_child("hh:borderFill");
        node.append_attribute("id") = static_cast<unsigned>(bf.id > 0 ? bf.id : (i + 1));
        node.append_attribute("threeD") = 0;
        node.append_attribute("shadow") = 0;
        node.append_attribute("centerLine") =
            bf.centerLine.empty() ? "NONE" : bf.centerLine.c_str();
        node.append_attribute("breakCellSeparateLine") = bf.breakCellSeparateLine ? 1 : 0;

        auto writeDiag = [&](const char* tagName, const DiagonalSpec& d) {
            auto diag = node.append_child(tagName);
            diag.append_attribute("type") = d.type.empty() ? "NONE" : d.type.c_str();
            diag.append_attribute("Crooked") = d.crooked ? 1 : 0;
            diag.append_attribute("isCounter") = d.counter ? 1 : 0;
        };
        writeDiag("hh:slash", bf.slash);
        writeDiag("hh:backSlash", bf.backSlash);

        auto writeBorderSide = [&](const char* tagName, const BorderSide& side) {
            auto b = node.append_child(tagName);
            b.append_attribute("type") = toHwpxLineType(side.type).c_str();
            b.append_attribute("width") = toHwpxBorderWidth(side.width).c_str();
            b.append_attribute("color") =
                (side.color.empty() || side.color == "none") ? "#000000" : side.color.c_str();
        };
        writeBorderSide("hh:leftBorder", bf.left);
        writeBorderSide("hh:rightBorder", bf.right);
        writeBorderSide("hh:topBorder", bf.top);
        writeBorderSide("hh:bottomBorder", bf.bottom);
        writeBorderSide("hh:diagonal", bf.diagonal);

        writeFillBrushNode(node, bf.faceColor, bf.gradColor, bf.gradAngle, bf.imageId, bf.imageMode,
                           bins, bf.hatchStyle, bf.hatchColor);
    }

    // 3. charProperties (0-based ID)
    auto charProperties = refList.append_child("hh:charProperties");
    const std::size_t csCount = std::max<std::size_t>(1, h.charShapes.size());
    charProperties.append_attribute("itemCnt") = static_cast<unsigned>(csCount);
    static constexpr std::array<const char*, kLangCount> kLangAttr = {
        "hangul", "latin", "hanja", "japanese", "other", "symbol", "user"};
    for (std::size_t i = 0; i < csCount; ++i) {
        CharShape cs;
        if (i < h.charShapes.size()) {
            cs = h.charShapes[i];
        } else {
            cs.ratio.fill(100);
            cs.relSize.fill(100);
        }
        auto cp = charProperties.append_child("hh:charPr");
        cp.append_attribute("id") = static_cast<unsigned>(cs.id != 0 || i == 0 ? cs.id : i);
        cp.append_attribute("height") = cs.height;
        cp.append_attribute("textColor") = formatColor(cs.textColor, "#000000").c_str();
        cp.append_attribute("shadeColor") = formatColor(cs.shadeColor, "none").c_str();
        cp.append_attribute("useFontSpace") = cs.useFontSpace ? 1 : 0;
        cp.append_attribute("useKerning") = cs.useKerning ? 1 : 0;
        cp.append_attribute("symMark") = emphasisToken(cs.emphasis).c_str();
        cp.append_attribute("borderFillIDRef") = cs.borderFillId > 0 ? cs.borderFillId : 1;

        auto fontRef = cp.append_child("hh:fontRef");
        auto ratio = cp.append_child("hh:ratio");
        auto spacing = cp.append_child("hh:spacing");
        auto relSz = cp.append_child("hh:relSz");
        auto offset = cp.append_child("hh:offset");
        for (std::size_t b = 0; b < kLangCount; ++b) {
            fontRef.append_attribute(kLangAttr[b]) = cs.fontRef[b];
            ratio.append_attribute(kLangAttr[b]) = cs.ratio[b] > 0 ? cs.ratio[b] : 100;
            spacing.append_attribute(kLangAttr[b]) = cs.spacing[b];
            relSz.append_attribute(kLangAttr[b]) = cs.relSize[b] > 0 ? cs.relSize[b] : 100;
            offset.append_attribute(kLangAttr[b]) = cs.offset[b];
        }
        if (cs.bold) cp.append_child("hh:bold");
        if (cs.italic) cp.append_child("hh:italic");
        if (cs.superscript) cp.append_child("hh:supscript");
        if (cs.subscript) cp.append_child("hh:subscript");
        if (cs.emboss) cp.append_child("hh:emboss");
        if (cs.engrave) cp.append_child("hh:engrave");

        auto ul = cp.append_child("hh:underline");
        ul.append_attribute("type") = cs.underline.empty() ? "NONE" : cs.underline.c_str();
        ul.append_attribute("shape") =
            toHwpxLineType(cs.underlineShape.empty() ? "SOLID" : cs.underlineShape).c_str();
        ul.append_attribute("color") = formatColor(cs.underlineColor, "none").c_str();

        auto st = cp.append_child("hh:strikeout");
        const std::string strikeShape =
            cs.strikeout ? (cs.strikeShape.empty() ? "SOLID" : cs.strikeShape) : "NONE";
        st.append_attribute("shape") = toHwpxLineType(strikeShape).c_str();
        st.append_attribute("color") = formatColor(cs.strikeColor, "none").c_str();

        auto ol = cp.append_child("hh:outline");
        ol.append_attribute("type") = outlineToken(cs.outline);

        auto sh = cp.append_child("hh:shadow");
        sh.append_attribute("type") = shadowToken(cs.shadow);
        sh.append_attribute("color") = formatColor(cs.shadowColor, "#B2B2B2").c_str();
        sh.append_attribute("offsetX") = cs.shadowOffsetX;
        sh.append_attribute("offsetY") = cs.shadowOffsetY;
    }

    // 4. tabProperties
    auto tabProperties = refList.append_child("hh:tabProperties");
    const std::size_t tabCount = std::max<std::size_t>(1, h.tabDefs.size());
    tabProperties.append_attribute("itemCnt") = static_cast<unsigned>(tabCount);
    for (std::size_t i = 0; i < tabCount; ++i) {
        const TabDef td = i < h.tabDefs.size() ? h.tabDefs[i] : TabDef{};
        auto tp = tabProperties.append_child("hh:tabPr");
        tp.append_attribute("id") = static_cast<unsigned>(td.id != 0 || i == 0 ? td.id : i);
        tp.append_attribute("autoTabLeft") = td.autoTabLeft ? 1 : 0;
        tp.append_attribute("autoTabRight") = td.autoTabRight ? 1 : 0;
        for (const auto& stop : td.stops) {
            auto item = tp.append_child("hh:tabItem");
            item.append_attribute("pos") = stop.pos;
            item.append_attribute("type") = stop.type.empty() ? "LEFT" : stop.type.c_str();
            item.append_attribute("leader") = stop.leader.empty() ? "NONE" : stop.leader.c_str();
        }
    }

    // 5. numberings
    if (!h.numberings.empty()) {
        auto numberings = refList.append_child("hh:numberings");
        numberings.append_attribute("itemCnt") = static_cast<unsigned>(h.numberings.size());
        for (std::size_t i = 0; i < h.numberings.size(); ++i) {
            const auto& nd = h.numberings[i];
            auto num = numberings.append_child("hh:numbering");
            num.append_attribute("id") = static_cast<unsigned>(nd.id > 0 ? nd.id : (i + 1));
            num.append_attribute("start") = nd.start;
            for (const auto& ph : nd.levels) {
                auto node = num.append_child("hh:paraHead");
                node.append_attribute("start") = ph.start;
                node.append_attribute("level") = ph.level;
                node.append_attribute("align") = ph.align.empty() ? "LEFT" : ph.align.c_str();
                node.append_attribute("useInstWidth") = ph.useInstWidth ? 1 : 0;
                node.append_attribute("autoIndent") = ph.autoIndent ? 1 : 0;
                node.append_attribute("widthAdjust") = ph.widthAdjust;
                node.append_attribute("textOffsetType") =
                    ph.textOffsetType.empty() ? "PERCENT" : ph.textOffsetType.c_str();
                node.append_attribute("textOffset") = ph.textOffset;
                node.append_attribute("numFormat") =
                    ph.numFormat.empty() ? "DIGIT" : ph.numFormat.c_str();
                node.append_attribute("charPrIDRef") = ph.charShapeId;
                node.append_attribute("checkable") = 0;
                if (!ph.format.empty()) {
                    node.text().set(ph.format.c_str());
                }
            }
        }
    }

    // 6. bullets
    if (!h.bullets.empty()) {
        auto bullets = refList.append_child("hh:bullets");
        bullets.append_attribute("itemCnt") = static_cast<unsigned>(h.bullets.size());
        for (std::size_t i = 0; i < h.bullets.size(); ++i) {
            const auto& bd = h.bullets[i];
            auto b = bullets.append_child("hh:bullet");
            b.append_attribute("id") = static_cast<unsigned>(bd.id > 0 ? bd.id : (i + 1));
            b.append_attribute("char") = bd.text.c_str();
            if (!bd.checkedText.empty()) {
                b.append_attribute("checkedChar") = bd.checkedText.c_str();
            }
            const bool useImage = !bd.imageId.empty();
            b.append_attribute("useImage") = useImage ? 1 : 0;
            if (useImage) {
                auto img = b.append_child("hc:img");
                img.append_attribute("binaryItemIDRef") = bins.resolveId(bd.imageId).c_str();
                img.append_attribute("bright") = bd.bright;
                img.append_attribute("contrast") = bd.contrast;
                img.append_attribute("effect") = toHwpxPictureEffect(bd.effect);
            }
            auto ph = b.append_child("hh:paraHead");
            ph.append_attribute("level") = bd.head.level;
            ph.append_attribute("start") = bd.head.start;
            ph.append_attribute("align") = bd.head.align.empty() ? "LEFT" : bd.head.align.c_str();
            ph.append_attribute("useInstWidth") = bd.head.useInstWidth ? 1 : 0;
            ph.append_attribute("autoIndent") = bd.head.autoIndent ? 1 : 0;
            ph.append_attribute("widthAdjust") = bd.head.widthAdjust;
            ph.append_attribute("textOffsetType") =
                bd.head.textOffsetType.empty() ? "PERCENT" : bd.head.textOffsetType.c_str();
            ph.append_attribute("textOffset") = bd.head.textOffset;
            ph.append_attribute("numFormat") =
                bd.head.numFormat.empty() ? "DIGIT" : bd.head.numFormat.c_str();
            ph.append_attribute("charPrIDRef") = bd.head.charShapeId;
            ph.append_attribute("checkable") = 0;
        }
    }

    // 7. paraProperties (0-based ID)
    auto paraProperties = refList.append_child("hh:paraProperties");
    const std::size_t psCount = std::max<std::size_t>(1, h.paraShapes.size());
    paraProperties.append_attribute("itemCnt") = static_cast<unsigned>(psCount);
    for (std::size_t i = 0; i < psCount; ++i) {
        const ParaShape ps = i < h.paraShapes.size() ? h.paraShapes[i] : ParaShape{};
        auto pp = paraProperties.append_child("hh:paraPr");
        pp.append_attribute("id") = static_cast<unsigned>(ps.id != 0 || i == 0 ? ps.id : i);
        pp.append_attribute("tabPrIDRef") = ps.tabDefId;
        pp.append_attribute("condense") = ps.condense;
        pp.append_attribute("fontLineHeight") = ps.fontLineHeight ? 1 : 0;
        pp.append_attribute("snapToGrid") = ps.snapToGrid ? 1 : 0;
        pp.append_attribute("suppressLineNumbers") = ps.suppressLineNumbers ? 1 : 0;
        pp.append_attribute("checked") = ps.checked ? 1 : 0;

        auto align = pp.append_child("hh:align");
        align.append_attribute("horizontal") = ps.align.empty() ? "JUSTIFY" : ps.align.c_str();
        align.append_attribute("vertical") = ps.vertAlign.empty() ? "BASELINE" : ps.vertAlign.c_str();

        auto heading = pp.append_child("hh:heading");
        heading.append_attribute("type") = ps.headingType.empty() ? "NONE" : ps.headingType.c_str();
        heading.append_attribute("idRef") = ps.headingIdRef;
        heading.append_attribute("level") = ps.headingLevel;

        auto brk = pp.append_child("hh:breakSetting");
        brk.append_attribute("breakLatinWord") =
            ps.breakLatinWord.empty() ? "KEEP_WORD" : ps.breakLatinWord.c_str();
        brk.append_attribute("breakNonLatinWord") =
            ps.breakNonLatinWord.empty() ? "KEEP_WORD" : ps.breakNonLatinWord.c_str();
        brk.append_attribute("widowOrphan") = ps.widowOrphan ? 1 : 0;
        brk.append_attribute("keepWithNext") = ps.keepWithNext ? 1 : 0;
        brk.append_attribute("keepLines") = ps.keepLines ? 1 : 0;
        brk.append_attribute("pageBreakBefore") = ps.pageBreakBefore ? 1 : 0;
        brk.append_attribute("lineWrap") = ps.lineWrap.empty() ? "BREAK" : ps.lineWrap.c_str();

        auto autoSp = pp.append_child("hh:autoSpacing");
        autoSp.append_attribute("eAsianEng") = ps.autoSpacingEng ? 1 : 0;
        autoSp.append_attribute("eAsianNum") = ps.autoSpacingNum ? 1 : 0;

        auto sw = pp.append_child("hp:switch");
        auto csNode = sw.append_child("hp:case");
        csNode.append_attribute("hp:required-namespace") =
            "http://www.hancom.co.kr/hwpml/2016/HwpUnitChar";
        auto writeMarginAndSpacing = [&](pugi::xml_node target, std::int32_t scale) {
            auto m = target.append_child("hh:margin");
            auto addVal = [&](const char* childName, std::int32_t v) {
                auto c = m.append_child(childName);
                c.append_attribute("value") = v * scale;
                c.append_attribute("unit") = "HWPUNIT";
            };
            addVal("hc:intent", ps.indent);
            addVal("hc:left", ps.marginLeft);
            addVal("hc:right", ps.marginRight);
            addVal("hc:prev", ps.spacingPrev);
            addVal("hc:next", ps.spacingNext);

            auto ls = target.append_child("hh:lineSpacing");
            ls.append_attribute("type") =
                ps.lineSpacingType.empty() ? "PERCENT" : ps.lineSpacingType.c_str();
            const std::int32_t lsVal =
                (ps.lineSpacingType.empty() || ps.lineSpacingType == "PERCENT")
                    ? ps.lineSpacing
                    : (ps.lineSpacing * scale);
            ls.append_attribute("value") = lsVal;
            ls.append_attribute("unit") = "HWPUNIT";
        };
        writeMarginAndSpacing(csNode, 1);
        auto defNode = sw.append_child("hp:default");
        writeMarginAndSpacing(defNode, 2);

        auto border = pp.append_child("hh:border");
        border.append_attribute("borderFillIDRef") = ps.borderFillId > 0 ? ps.borderFillId : 1;
        border.append_attribute("offsetLeft") = ps.borderOffsetLeft;
        border.append_attribute("offsetRight") = ps.borderOffsetRight;
        border.append_attribute("offsetTop") = ps.borderOffsetTop;
        border.append_attribute("offsetBottom") = ps.borderOffsetBottom;
        border.append_attribute("connect") = ps.borderConnect ? 1 : 0;
        border.append_attribute("ignoreMargin") = ps.borderIgnoreMargin ? 1 : 0;
    }

    // 8. styles (0-based ID)
    auto styles = refList.append_child("hh:styles");
    const std::size_t stCount = std::max<std::size_t>(1, h.styles.size());
    styles.append_attribute("itemCnt") = static_cast<unsigned>(stCount);
    for (std::size_t i = 0; i < stCount; ++i) {
        Style sd;
        if (i < h.styles.size()) {
            sd = h.styles[i];
        } else {
            sd.id = 0;
            sd.name = "바탕글";
            sd.engName = "Normal";
        }
        auto st = styles.append_child("hh:style");
        st.append_attribute("id") = static_cast<unsigned>(sd.id != 0 || i == 0 ? sd.id : i);
        st.append_attribute("type") = sd.type.empty() ? "PARA" : sd.type.c_str();
        st.append_attribute("name") = sd.name.empty() ? "바탕글" : sd.name.c_str();
        st.append_attribute("engName") = sd.engName.c_str();
        st.append_attribute("paraPrIDRef") = sd.paraShapeId;
        st.append_attribute("charPrIDRef") = sd.charShapeId;
        st.append_attribute("nextStyleIDRef") = sd.nextStyleId;
        st.append_attribute("langID") = sd.langId != 0 ? sd.langId : 1042;
        st.append_attribute("lockForm") = 0;
    }

    // 9. 메모 모양(id 1 부터). 없으면 쓰지 않는다(한글 2024).
    if (!h.memoShapes.empty()) {
        auto memos = refList.append_child("hh:memoProperties");
        memos.append_attribute("itemCnt") = static_cast<unsigned>(h.memoShapes.size());
        static const char* lineTypes[] = {"NONE", "SOLID", "DOT", "DASH", "DASH_DOT", "DASH_DOT_DOT",
                                          "LONG_DASH", "CIRCLE", "DOUBLE_SLIM", "SLIM_THICK", "THICK_SLIM",
                                          "SLIM_THICK_SLIM", "WAVE", "DOUBLEWAVE", "THICK3D", "THICKREV3D",
                                          "3D", "REV3D"};
        static const char* memoTypes[] = {"NOMAL", "USER_INSERT", "USER_DELETE", "USER_UPDATE"};
        for (std::size_t i = 0; i < h.memoShapes.size(); ++i) {
            const MemoShape& m = h.memoShapes[i];
            auto pr = memos.append_child("hh:memoPr");
            pr.append_attribute("id") = static_cast<unsigned>(i + 1);
            pr.append_attribute("width") = m.width;
            pr.append_attribute("lineWidth") = static_cast<unsigned>(m.lineWidth);
            pr.append_attribute("lineType") = lineTypes[std::min<std::size_t>(m.lineType, std::size(lineTypes) - 1)];
            pr.append_attribute("lineColor") = m.lineColor.c_str();
            pr.append_attribute("fillColor") = m.fillColor.c_str();
            pr.append_attribute("activeColor") = m.activeColor.c_str();
            pr.append_attribute("memoType") = memoTypes[std::min<std::uint32_t>(m.memoType & 0x77FFFFFFu, 3u)];
        }
    }

    // 10. 변경 추적 기록·작성자(한글 2024 와 같은 자리: refList 끝). 기록 중 여부는 head 의 trackchageConfig.
    if (!h.trackChanges.empty()) {
        auto list = refList.append_child("hh:trackChanges");
        list.append_attribute("itemCnt") = static_cast<unsigned>(h.trackChanges.size());
        for (const TrackChange& tc : h.trackChanges) {
            auto n = list.append_child("hh:trackChange");
            n.append_attribute("type") = tc.type.c_str();
            n.append_attribute("date") = tc.date.c_str();
            n.append_attribute("authorID") = tc.authorId;
            n.append_attribute("hide") = tc.hide ? 1 : 0;
            n.append_attribute("id") = tc.id;
            if (tc.charShapeId) n.append_attribute("charshapeID") = *tc.charShapeId;
            if (tc.paraShapeId) n.append_attribute("parashapeID") = *tc.paraShapeId;
        }
    }
    if (!h.trackAuthors.empty()) {
        auto list = refList.append_child("hh:trackChangeAuthors");
        list.append_attribute("itemCnt") = static_cast<unsigned>(h.trackAuthors.size());
        for (const TrackAuthor& a : h.trackAuthors) {
            auto n = list.append_child("hh:trackChangeAuthor");
            n.append_attribute("name") = a.name.c_str();
            n.append_attribute("mark") = a.mark ? 1 : 0;
            n.append_attribute("id") = a.id;
        }
    }
    head.append_child("hh:trackchageConfig").append_attribute("flags") = h.trackConfigFlags;

    return serializeXml(xml);
}

void writeParagraphNode(pugi::xml_node parent, const Paragraph& para, const BinRegistry& bins,
                        const Section* firstParaSec = nullptr, std::size_t masterPageBaseIdx = 0);

// 한글 2024 가 쓰는 글자 방향 이름과 같은 차례다.
const char* textDirectionName(std::uint8_t dir) {
    return dir == 2 ? "VERTICALALL" : (dir == 1 ? "VERTICAL" : "HORIZONTAL");
}

void writeSubListNode(pugi::xml_node parent, const SubList& sub, const BinRegistry& bins) {
    auto subList = parent.append_child("hp:subList");
    subList.append_attribute("id") = "";
    subList.append_attribute("textDirection") = textDirectionName(sub.textDirection);
    subList.append_attribute("lineWrap") = "BREAK";
    subList.append_attribute("vertAlign") =
        sub.vertAlign.empty() ? "TOP" : sub.vertAlign.c_str();
    subList.append_attribute("linkListIDRef") = 0;
    subList.append_attribute("linkListNextIDRef") = 0;
    subList.append_attribute("textWidth") = sub.textWidth;
    subList.append_attribute("textHeight") = sub.textHeight;
    subList.append_attribute("hasTextRef") = 0;
    subList.append_attribute("hasNumRef") = 0;
    if (sub.paragraphs.empty()) {
        Paragraph emptyPara;
        writeParagraphNode(subList, emptyPara, bins, nullptr, 0);
    } else {
        for (const auto& p : sub.paragraphs) {
            writeParagraphNode(subList, p, bins, nullptr, 0);
        }
    }
}

void writePlacementNodes(pugi::xml_node objNode, const Control& ctrl, const BinRegistry& bins,
                         std::uint32_t defaultId, bool grouped = false) {
    const Placement p = ctrl.placement.value_or(Placement{});
    objNode.append_attribute("id") = p.instanceId != 0 ? p.instanceId : defaultId;
    objNode.append_attribute("zOrder") = p.zOrder;
    objNode.append_attribute("numberingType") = objectNumberingType(ctrl.kind, p.numberingType);
    if (!grouped || !p.textWrap.empty()) {
        objNode.append_attribute("textWrap") =
            p.textWrap.empty() ? (p.treatAsChar ? "TOP_AND_BOTTOM" : "SQUARE") : p.textWrap.c_str();
    }
    objNode.append_attribute("textFlow") =
        p.textFlow.empty() ? "BOTH_SIDES" : p.textFlow.c_str();
    objNode.append_attribute("lock") = 0;
    objNode.append_attribute("dropcapstyle") = "None";

    auto sz = objNode.append_child("hp:sz");
    sz.append_attribute("width") = p.width;
    sz.append_attribute("widthRelTo") = "ABSOLUTE";
    sz.append_attribute("height") = p.height;
    sz.append_attribute("heightRelTo") = "ABSOLUTE";
    sz.append_attribute("protect") = 0;

    // 묶음 자식은 쪽 기준 위치가 없다. pos 를 붙이면 저장 뒤에 묶음 밖으로 나간다.
    if (!grouped) {
        auto pos = objNode.append_child("hp:pos");
        pos.append_attribute("treatAsChar") = p.treatAsChar ? 1 : 0;
        pos.append_attribute("affectLSpacing") = 0;
        pos.append_attribute("flowWithText") = p.flowWithText ? 1 : 0;
        pos.append_attribute("allowOverlap") = p.allowOverlap ? 1 : 0;
        pos.append_attribute("holdAnchorAndSO") = 0;
        pos.append_attribute("vertRelTo") = p.vertRelTo.empty() ? "PARA" : p.vertRelTo.c_str();
        pos.append_attribute("horzRelTo") = p.horzRelTo.empty() ? "PARA" : p.horzRelTo.c_str();
        pos.append_attribute("vertAlign") = p.vertAlign.empty() ? "TOP" : p.vertAlign.c_str();
        pos.append_attribute("horzAlign") = p.horzAlign.empty() ? "LEFT" : p.horzAlign.c_str();
        pos.append_attribute("vertOffset") = p.vertOffset;
        pos.append_attribute("horzOffset") = p.horzOffset;
    }

    auto outMargin = objNode.append_child("hp:outMargin");
    outMargin.append_attribute("left") = p.marginLeft;
    outMargin.append_attribute("right") = p.marginRight;
    outMargin.append_attribute("top") = p.marginTop;
    outMargin.append_attribute("bottom") = p.marginBottom;

    if (ctrl.caption) {
        const auto& cap = *ctrl.caption;
        auto capNode = objNode.append_child("hp:caption");
        capNode.append_attribute("side") = cap.side.empty() ? "BOTTOM" : cap.side.c_str();
        capNode.append_attribute("fullSz") = cap.fullSize ? 1 : 0;
        capNode.append_attribute("width") = cap.width;
        capNode.append_attribute("gap") = cap.gap;
        capNode.append_attribute("lastWidth") = cap.lastWidth;
        writeSubListNode(capNode, cap.content, bins);
    }
}

void writeSecPrNode(pugi::xml_node runNode, const Control* secCtrl, const Section* secPtr,
                    std::size_t masterPageBaseIdx) {
    PageDef pd;
    if (secCtrl && secCtrl->page) pd = *secCtrl->page;
    else if (secPtr && secPtr->page) pd = *secPtr->page;

    const std::int32_t defaultTab =
        (secCtrl && secCtrl->defaultTab > 0) ? secCtrl->defaultTab
        : ((secPtr && secPtr->defaultTab > 0) ? secPtr->defaultTab : 4000);
    const std::int32_t colGap =
        (secCtrl && secCtrl->columnRegionGap > 0) ? secCtrl->columnRegionGap
        : ((secPtr && secPtr->columnRegionGap > 0) ? secPtr->columnRegionGap : 1134);
    const std::uint32_t pageStart =
        (secCtrl && secCtrl->number > 0) ? secCtrl->number
        : (secPtr ? secPtr->pageStart : 0);
    // 격자는 구역 값이 기준이다. 편집은 구역만 고친다.
    const std::int32_t lineGrid = secPtr ? secPtr->lineGrid : (secCtrl ? secCtrl->lineGrid : 0);
    const std::int32_t charGrid = secPtr ? secPtr->charGrid : (secCtrl ? secCtrl->charGrid : 0);
    const bool wongoji = secPtr ? secPtr->wongoji : (secCtrl && secCtrl->wongoji);
    const bool hideFirstHeader =
        (secCtrl && secCtrl->hideFirstHeader) || (secPtr && secPtr->hideFirstHeader);
    const bool hideFirstFooter =
        (secCtrl && secCtrl->hideFirstFooter) || (secPtr && secPtr->hideFirstFooter);
    const bool hideFirstMaster =
        (secCtrl && secCtrl->hideFirstMaster) || (secPtr && secPtr->hideFirstMaster);
    const bool hideFirstPageNum =
        (secCtrl && secCtrl->hideFirstPageNum) || (secPtr && secPtr->hideFirstPageNum);

    auto secPr = runNode.append_child("hp:secPr");
    secPr.append_attribute("id") = "";
    secPr.append_attribute("textDirection") =
        textDirectionName(secPtr ? secPtr->textDirection : (secCtrl ? secCtrl->textDirection : 0));
    secPr.append_attribute("spaceColumns") = colGap;
    secPr.append_attribute("tabStop") = defaultTab * 2;
    secPr.append_attribute("tabStopVal") = defaultTab;
    secPr.append_attribute("tabStopUnit") = "HWPUNIT";
    secPr.append_attribute("outlineShapeIDRef") = 1;
    secPr.append_attribute("memoShapeIDRef") =
        static_cast<unsigned>(secPtr ? secPtr->memoShapeId : (secCtrl ? secCtrl->memoShapeId : 0));
    secPr.append_attribute("textVerticalWidthHead") = 0;
    secPr.append_attribute("masterPageCnt") =
        static_cast<unsigned>(secPtr ? secPtr->masterPages.size() : 0);

    auto grid = secPr.append_child("hp:grid");
    grid.append_attribute("lineGrid") = lineGrid;
    grid.append_attribute("charGrid") = charGrid;
    grid.append_attribute("wonggojiFormat") = wongoji ? 1 : 0;

    // 구역 설정은 구역 값이 기준이다. 구역이 없으면 구역 정의 컨트롤 값이다.
    struct SectionSetting {
        std::uint8_t startsOn = 0;
        std::uint16_t pic = 0, tbl = 0, eq = 0;
        std::string border = "SHOW_ALL", fill = "SHOW_ALL";
        bool emptyLine = false;
        LineNumberShape lineNumbers;
    } setting;
    if (secPtr) {
        setting = {secPtr->startsOn, secPtr->pictureStart, secPtr->tableStart, secPtr->equationStart,
                   secPtr->borderShow, secPtr->fillShow, secPtr->hideFirstEmptyLine, secPtr->lineNumbers};
    } else if (secCtrl) {
        setting = {secCtrl->startsOn, secCtrl->pictureStart, secCtrl->tableStart, secCtrl->equationStart,
                   secCtrl->borderShow, secCtrl->fillShow, secCtrl->hideFirstEmptyLine, secCtrl->lineNumbers};
    }
    auto startNum = secPr.append_child("hp:startNum");
    // 한글 2024: 0 이어서 BOTH, 1 짝수 EVEN, 2 홀수 ODD, 3 사용자(쓰지 않음). 옛 모델은 사용자 번호만 있었다.
    const unsigned startsOn = pageStart > 0 && setting.startsOn == 0 ? 3u : setting.startsOn;
    if (startsOn != 3) startNum.append_attribute("pageStartsOn") = startsOn == 1 ? "EVEN" : startsOn == 2 ? "ODD" : "BOTH";
    startNum.append_attribute("page") = pageStart;
    startNum.append_attribute("pic") = setting.pic;
    startNum.append_attribute("tbl") = setting.tbl;
    startNum.append_attribute("equation") = setting.eq;

    auto vis = secPr.append_child("hp:visibility");
    vis.append_attribute("hideFirstHeader") = hideFirstHeader ? 1 : 0;
    vis.append_attribute("hideFirstFooter") = hideFirstFooter ? 1 : 0;
    vis.append_attribute("hideFirstMasterPage") = hideFirstMaster ? 1 : 0;
    vis.append_attribute("border") = setting.border.empty() ? "SHOW_ALL" : setting.border.c_str();
    vis.append_attribute("fill") = setting.fill.empty() ? "SHOW_ALL" : setting.fill.c_str();
    vis.append_attribute("hideFirstPageNum") = hideFirstPageNum ? 1 : 0;
    vis.append_attribute("hideFirstEmptyLine") = setting.emptyLine ? 1 : 0;
    vis.append_attribute("showLineNumber") = setting.lineNumbers.show ? 1 : 0;
    // 한글 2024 는 visibility 바로 뒤에 줄 번호 모양을 늘 쓴다.
    auto lineNumberShape = secPr.append_child("hp:lineNumberShape");
    lineNumberShape.append_attribute("restartType") = static_cast<unsigned>(setting.lineNumbers.restart);
    lineNumberShape.append_attribute("countBy") = static_cast<unsigned>(setting.lineNumbers.countBy);
    lineNumberShape.append_attribute("distance") = setting.lineNumbers.distance;
    lineNumberShape.append_attribute("startNumber") = static_cast<unsigned>(setting.lineNumbers.start);

    auto pagePr = secPr.append_child("hp:pagePr");
    pagePr.append_attribute("landscape") = pd.landscape ? "NARROWLY" : "WIDELY";
    pagePr.append_attribute("width") = pd.storedWidth();
    pagePr.append_attribute("height") = pd.storedHeight();
    pagePr.append_attribute("gutterType") =
        pd.gutterType == 1 ? "LEFT_RIGHT" : (pd.gutterType == 2 ? "TOP_BOTTOM" : "LEFT_ONLY");

    auto margin = pagePr.append_child("hp:margin");
    margin.append_attribute("left") = pd.marginLeft;
    margin.append_attribute("right") = pd.marginRight;
    margin.append_attribute("top") = pd.marginTop;
    margin.append_attribute("bottom") = pd.marginBottom;
    margin.append_attribute("header") = pd.marginHeader;
    margin.append_attribute("footer") = pd.marginFooter;
    margin.append_attribute("gutter") = pd.marginGutter;

    // 각주·미주 모양(한글 2024 secPr 차례: pagePr, footNotePr, endNotePr, pageBorderFill).
    for (int fn = 0; fn < 2; ++fn) {
        const model::NoteShape fallback;
        const model::NoteShape& shape = secPtr ? (fn == 0 ? secPtr->footnote : secPtr->endnote)
                                               : (secCtrl ? (fn == 0 ? secCtrl->footnoteShape : secCtrl->endnoteShape)
                                                          : fallback);
        auto pr = secPr.append_child(fn == 0 ? "hp:footNotePr" : "hp:endNotePr");
        auto fmt = pr.append_child("hp:autoNumFormat");
        // 한글 2024 는 기호 모양(목록 17 번부터)이면 type 을 쓰지 않는다.
        std::string type = shape.numFormat.empty() ? "DIGIT" : shape.numFormat;
        if (type == "DECAGON") type = "DECAGON_CIRCLE";
        if (type == "DECAGON_HANJA") type = "DECAGON_CIRCLE_HANJA";
        if (type != "SYMBOL" && type != "USER_CHAR") fmt.append_attribute("type") = type.c_str();
        fmt.append_attribute("userChar") = shape.userChar.c_str();
        fmt.append_attribute("prefixChar") = shape.prefix.c_str();
        fmt.append_attribute("suffixChar") = shape.suffix.c_str();
        fmt.append_attribute("supscript") = shape.contentSuperscript ? 1 : 0;
        auto line = pr.append_child("hp:noteLine");
        line.append_attribute("length") = shape.lineLength;
        line.append_attribute("type") =
            toHwpxLineType(shape.lineType.empty() ? std::string("NONE") : shape.lineType).c_str();
        line.append_attribute("width") = toHwpxBorderWidth(shape.lineWidth).c_str();
        line.append_attribute("color") = shape.lineColor.empty() ? "#000000" : shape.lineColor.c_str();
        auto spacing = pr.append_child("hp:noteSpacing");
        spacing.append_attribute("betweenNotes") = shape.between;
        spacing.append_attribute("belowLine") = shape.below;
        spacing.append_attribute("aboveLine") = shape.above;
        auto numbering = pr.append_child("hp:numbering");
        numbering.append_attribute("type") =
            shape.numbering == 1 ? "ON_SECTION" : shape.numbering == 2 ? "ON_PAGE" : "CONTINUOUS";
        numbering.append_attribute("newNum") = shape.startNumber;
        auto placement = pr.append_child("hp:placement");
        static const char* const kFootPlaces[] = {"EACH_COLUMN", "MERGED_COLUMN", "RIGHT_MOST_COLUMN"};
        static const char* const kEndPlaces[] = {"END_OF_DOCUMENT", "END_OF_SECTION"};
        placement.append_attribute("place") = fn == 0 ? kFootPlaces[std::clamp(shape.place, 0, 2)]
                                                       : kEndPlaces[std::clamp(shape.place, 0, 1)];
        placement.append_attribute("beneathText") = shape.beneathText ? 1 : 0;
    }

    const std::array<model::PageBorderFill, 3>* borders = nullptr;
    if (secPtr) borders = &secPtr->pageBorders;
    else if (secCtrl) borders = &secCtrl->pageBorders;
    if (borders) {
        static const char* areas[] = {"PAPER", "PAGE", "BORDER", "BORDER"};
        for (const auto& border : *borders) {
            auto node = secPr.append_child("hp:pageBorderFill");
            node.append_attribute("type") = border.type.c_str();
            node.append_attribute("borderFillIDRef") = border.borderFillId;
            node.append_attribute("textBorder") = border.textBorder ? "PAPER" : "CONTENT";
            node.append_attribute("headerInside") = border.headerInside ? "1" : "0";
            node.append_attribute("footerInside") = border.footerInside ? "1" : "0";
            const int area = border.fillArea < 0 ? 0 : (border.fillArea > 3 ? 3 : border.fillArea);
            node.append_attribute("fillArea") = areas[area];
            auto offset = node.append_child("hp:offset");
            offset.append_attribute("left") = border.left;
            offset.append_attribute("right") = border.right;
            offset.append_attribute("top") = border.top;
            offset.append_attribute("bottom") = border.bottom;
        }
    }

    if (secPtr) {
        for (std::size_t i = 0; i < secPtr->masterPages.size(); ++i) {
            auto mp = secPr.append_child("hp:masterPage");
            mp.append_attribute("idRef") =
                ("masterpage" + std::to_string(masterPageBaseIdx + i)).c_str();
        }
    }
}

void writeObjectNode(pugi::xml_node parent, const Control& ctrl, const BinRegistry& bins, int groupLevel = 0);

void writeTableNode(pugi::xml_node parent, const Control& ctrl, const BinRegistry& bins) {
    if (!ctrl.table) return;
    const Table& tbl = *ctrl.table;
    auto tblNode = parent.append_child("hp:tbl");
    writePlacementNodes(tblNode, ctrl, bins, 1820394742u);
    tblNode.append_attribute("pageBreak") =
        tbl.pageBreak.empty() ? "CELL" : tbl.pageBreak.c_str();
    tblNode.append_attribute("repeatHeader") = tbl.repeatHeader ? 1 : 0;
    tblNode.append_attribute("rowCnt") = tbl.rowCount;
    tblNode.append_attribute("colCnt") = tbl.colCount;
    tblNode.append_attribute("cellSpacing") = tbl.cellSpacing;
    tblNode.append_attribute("borderFillIDRef") = tbl.borderFillId > 0 ? tbl.borderFillId : 1;
    tblNode.append_attribute("noAdjust") = tbl.noAdjust ? 1 : 0;

    auto inMargin = tblNode.append_child("hp:inMargin");
    inMargin.append_attribute("left") = tbl.marginLeft;
    inMargin.append_attribute("right") = tbl.marginRight;
    inMargin.append_attribute("top") = tbl.marginTop;
    inMargin.append_attribute("bottom") = tbl.marginBottom;

    std::uint32_t maxRow = tbl.rowCount;
    for (const auto& cell : tbl.cells) {
        if (cell.row + 1 > maxRow) maxRow = cell.row + 1;
    }
    std::vector<std::vector<const TableCell*>> rows(std::max<std::uint32_t>(1, maxRow));
    for (const auto& cell : tbl.cells) {
        rows[cell.row].push_back(&cell);
    }
    for (auto& rCells : rows) {
        if (rCells.empty()) continue;
        std::sort(rCells.begin(), rCells.end(),
                  [](const TableCell* a, const TableCell* b) { return a->col < b->col; });
        auto tr = tblNode.append_child("hp:tr");
        for (const TableCell* c : rCells) {
            auto tc = tr.append_child("hp:tc");
            tc.append_attribute("name") = c->name.c_str();
            tc.append_attribute("header") = c->header ? 1 : 0;
            tc.append_attribute("hasMargin") = c->hasMargin ? 1 : 0;
            tc.append_attribute("protect") = 0;
            tc.append_attribute("editable") = 0;
            tc.append_attribute("dirty") = 0;
            tc.append_attribute("borderFillIDRef") = c->borderFillId > 0 ? c->borderFillId : 1;

            SubList cellSub = c->content;
            if (!c->vertAlign.empty() && cellSub.vertAlign.empty()) {
                cellSub.vertAlign = c->vertAlign;
            }
            writeSubListNode(tc, cellSub, bins);

            auto addr = tc.append_child("hp:cellAddr");
            addr.append_attribute("colAddr") = c->col;
            addr.append_attribute("rowAddr") = c->row;

            auto span = tc.append_child("hp:cellSpan");
            span.append_attribute("colSpan") = c->colSpan;
            span.append_attribute("rowSpan") = c->rowSpan;

            auto sz = tc.append_child("hp:cellSz");
            sz.append_attribute("width") = c->width;
            sz.append_attribute("height") = c->height;

            auto cm = tc.append_child("hp:cellMargin");
            cm.append_attribute("left") = c->marginLeft;
            cm.append_attribute("right") = c->marginRight;
            cm.append_attribute("top") = c->marginTop;
            cm.append_attribute("bottom") = c->marginBottom;
        }
    }
}

void writeShapeGeometryAndCommon(pugi::xml_node node, const Control& ctrl, const BinRegistry& bins,
                                 int groupLevel) {
    const Placement p = ctrl.placement.value_or(Placement{});
    const bool grouped = groupLevel > 0 || p.groupInRoot;
    writePlacementNodes(node, ctrl, bins, 1732656412u, grouped);
    node.append_attribute("href") = "";
    node.append_attribute("groupLevel") = groupLevel;
    static std::uint32_t nextInst = 1;
    node.append_attribute("instid") = nextInst++;

    auto offset = node.append_child("hp:offset");
    offset.append_attribute("x") = p.groupX - p.matrixX;
    offset.append_attribute("y") = p.groupY - p.matrixY;

    const std::uint32_t curW = p.width;
    const std::uint32_t curH = p.height;
    const std::uint32_t orgW =
        (p.scaleX > 1e-6 && std::fabs(p.scaleX - 1.0) > 1e-6)
            ? static_cast<std::uint32_t>(std::llround(static_cast<double>(curW) / p.scaleX))
            : curW;
    const std::uint32_t orgH =
        (p.scaleY > 1e-6 && std::fabs(p.scaleY - 1.0) > 1e-6)
            ? static_cast<std::uint32_t>(std::llround(static_cast<double>(curH) / p.scaleY))
            : curH;

    auto orgSz = node.append_child("hp:orgSz");
    orgSz.append_attribute("width") = orgW;
    orgSz.append_attribute("height") = orgH;

    auto curSz = node.append_child("hp:curSz");
    curSz.append_attribute("width") = curW;
    curSz.append_attribute("height") = curH;

    auto flip = node.append_child("hp:flip");
    flip.append_attribute("horizontal") = p.flipHorizontal ? 1 : 0;
    flip.append_attribute("vertical") = p.flipVertical ? 1 : 0;

    auto rot = node.append_child("hp:rotationInfo");
    rot.append_attribute("angle") = 0;
    rot.append_attribute("centerX") = curW / 2;
    rot.append_attribute("centerY") = curH / 2;
    rot.append_attribute("rotateimage") = 1;

    auto ri = node.append_child("hp:renderingInfo");
    auto writeMat = [&](const char* name, const std::array<double, 6>& m) {
        auto mat = ri.append_child(name);
        for (int i = 0; i < 6; ++i) {
            const std::string attrName = "e" + std::to_string(i + 1);
            mat.append_attribute(attrName.c_str()) =
                formatFloat(m[static_cast<std::size_t>(i)]).c_str();
        }
    };
    writeMat("hc:transMatrix",
             {1.0, 0.0, static_cast<double>(p.groupX - p.matrixX), 0.0, 1.0,
              static_cast<double>(p.groupY - p.matrixY)});
    writeMat("hc:scaMatrix",
             {p.scaleX != 0.0 ? p.scaleX : 1.0, 0.0, static_cast<double>(p.matrixX), 0.0,
              p.scaleY != 0.0 ? p.scaleY : 1.0, static_cast<double>(p.matrixY)});
    writeMat("hc:rotMatrix", {1.0, 0.0, 0.0, 0.0, 1.0, 0.0});
}

// 보관한 개체의 딸린 묶음 항목(차트 XML). write 가 비우고 마지막에 묶음에 담는다.
thread_local std::vector<std::pair<std::string, Bytes>> gKeptParts;

bool isKeptObjectName(std::string_view name) {
    static const std::string_view names[] = {"hp:ole", "hp:chart", "hp:video", "hp:textart"};
    for (auto n : names) {
        if (n == name) return true;
    }
    return false;
}

// 보관한 원본 XML 의 개체 하나에 지금 위치·크기를 적는다.
void patchKeptPlacement(pugi::xml_node obj, const Control& ctrl, bool grouped) {
    if (!ctrl.placement) return;
    const Placement& p = *ctrl.placement;
    const auto set = [](pugi::xml_node node, const char* name, auto value) {
        if (!node) return;
        pugi::xml_attribute attr = node.attribute(name);
        if (!attr) attr = node.append_attribute(name);
        attr = value;
    };
    set(obj, "zOrder", p.zOrder);
    if (!p.textWrap.empty()) set(obj, "textWrap", p.textWrap.c_str());
    if (!p.textFlow.empty()) set(obj, "textFlow", p.textFlow.c_str());
    if (pugi::xml_node sz = obj.child("hp:sz")) {
        set(sz, "width", p.width);
        set(sz, "height", p.height);
    }
    // 크기를 바꿨으면 현재 크기와 늘림 행렬도 같이 바꾼다(한글은 처음 크기 × 늘림 = 현재 크기로 그린다).
    pugi::xml_node org = obj.child("hp:orgSz");
    pugi::xml_node cur = obj.child("hp:curSz");
    if (cur && cur.attribute("width").as_int() != 0) {
        set(cur, "width", p.width);
        set(cur, "height", p.height);
        const int orgW = org.attribute("width").as_int();
        const int orgH = org.attribute("height").as_int();
        if (pugi::xml_node sca = obj.child("hp:renderingInfo").child("hc:scaMatrix"); sca && orgW > 0 && orgH > 0) {
            set(sca, "e1", static_cast<double>(p.width) / orgW);
            set(sca, "e5", static_cast<double>(p.height) / orgH);
        }
    }
    if (!grouped) {
        if (pugi::xml_node pos = obj.child("hp:pos")) {
            set(pos, "treatAsChar", p.treatAsChar ? 1 : 0);
            set(pos, "flowWithText", p.flowWithText ? 1 : 0);
            set(pos, "allowOverlap", p.allowOverlap ? 1 : 0);
            if (!p.vertRelTo.empty()) set(pos, "vertRelTo", p.vertRelTo.c_str());
            if (!p.horzRelTo.empty()) set(pos, "horzRelTo", p.horzRelTo.c_str());
            if (!p.vertAlign.empty()) set(pos, "vertAlign", p.vertAlign.c_str());
            if (!p.horzAlign.empty()) set(pos, "horzAlign", p.horzAlign.c_str());
            set(pos, "vertOffset", p.vertOffset);
            set(pos, "horzOffset", p.horzOffset);
        }
    }
    if (pugi::xml_node out = obj.child("hp:outMargin")) {
        set(out, "left", p.marginLeft);
        set(out, "right", p.marginRight);
        set(out, "top", p.marginTop);
        set(out, "bottom", p.marginBottom);
    }
}

// HWPX 에서 읽어 보관한 개체(차트·동영상·OLE·글맵시)를 원본 XML 로 쓴다. 위치·크기와 묶음 id 만 지금 값으로 고친다.
bool writeKeptObject(pugi::xml_node parent, const Control& ctrl, const BinRegistry& bins, int groupLevel) {
    pugi::xml_document kept;
    if (!kept.load_buffer(ctrl.hwpxXml.data(), ctrl.hwpxXml.size(), pugi::parse_default | pugi::parse_ws_pcdata,
                          pugi::encoding_utf8)) {
        return false;
    }
    pugi::xml_node root = kept.document_element();
    if (!root) return false;
    // 모든 요소의 묶음 id 를 고치고, 개체 요소(맨 위 개체, 또는 hp:switch 가지의 개체)에 위치·크기를 적는다.
    const auto visit = [&](auto& self, pugi::xml_node node) -> void {
        for (const char* ref : {"binaryItemIDRef", "fileIDRef", "imageIDRef"}) {
            if (pugi::xml_attribute attr = node.attribute(ref); attr && *attr.value() != 0) {
                attr = bins.resolveId(attr.value()).c_str();
            }
        }
        if (pugi::xml_attribute chart = node.attribute("chartIDRef")) {
            const std::string from = chart.value();
            for (const auto& [path, bytes] : ctrl.hwpxParts) {
                if (path != from) continue;
                const std::string to = "Chart/chart" + std::to_string(gKeptParts.size() + 1) + ".xml";
                gKeptParts.emplace_back(to, bytes);
                chart = to.c_str();
                break;
            }
        }
        const std::string_view up = node.parent() ? std::string_view(node.parent().name()) : std::string_view();
        if (isKeptObjectName(node.name()) && (node == root || up == "hp:case" || up == "hp:default")) {
            patchKeptPlacement(node, ctrl, groupLevel > 0);
        }
        for (pugi::xml_node child : node.children()) {
            if (child.type() == pugi::node_element) self(self, child);
        }
    };
    visit(visit, root);
    parent.append_copy(root);
    return true;
}

// 테두리 없음(한글이 OLE 에 쓰는 선 모양).
void writeNoLineShape(pugi::xml_node node) {
    auto ls = node.append_child("hp:lineShape");
    ls.append_attribute("color") = "#000000";
    ls.append_attribute("width") = 0;
    ls.append_attribute("style") = "NONE";
    ls.append_attribute("endCap") = "ROUND";
    ls.append_attribute("headStyle") = "NORMAL";
    ls.append_attribute("tailStyle") = "NORMAL";
    ls.append_attribute("headfill") = 0;
    ls.append_attribute("tailfill") = 0;
    ls.append_attribute("headSz") = "SMALL_SMALL";
    ls.append_attribute("tailSz") = "SMALL_SMALL";
    ls.append_attribute("outlineStyle") = "NORMAL";
    ls.append_attribute("alpha") = 0;
}

// OLE(한글 2024 hp:ole 꼴): 공통 속성·모양 뒤 objectType·binaryItemIDRef·hasMoniker·drawAspect·eqBaseLine,
// 자식은 hc:extent 와 선 모양.
void writeOleNode(pugi::xml_node parent, const Control& ctrl, const BinRegistry& bins, int groupLevel) {
    const OleInfo ole = ctrl.ole.value_or(OleInfo{});
    auto node = parent.append_child("hp:ole");
    writeShapeGeometryAndCommon(node, ctrl, bins, groupLevel);
    node.append_attribute("objectType") = ole.objectType.c_str();
    node.append_attribute("binaryItemIDRef") = bins.resolveId(ctrl.binItemId).c_str();
    node.append_attribute("hasMoniker") = ole.hasMoniker ? 1 : 0;
    node.append_attribute("drawAspect") = ole.drawAspect.c_str();
    node.append_attribute("eqBaseLine") = ole.eqBaseLine;
    const Placement p = ctrl.placement.value_or(Placement{});
    auto extent = node.append_child("hc:extent");
    extent.append_attribute("x") = ole.extentX > 0 ? ole.extentX : static_cast<std::int32_t>(p.width);
    extent.append_attribute("y") = ole.extentY > 0 ? ole.extentY : static_cast<std::int32_t>(p.height);
    writeNoLineShape(node);
}

// 동영상(한글 2024 hp:video 꼴): videotype·fileIDRef(동영상)·imageIDRef(첫 장면)·tag.
void writeVideoNode(pugi::xml_node parent, const Control& ctrl, const BinRegistry& bins, int groupLevel) {
    const VideoInfo video = ctrl.video.value_or(VideoInfo{});
    auto node = parent.append_child("hp:video");
    writeShapeGeometryAndCommon(node, ctrl, bins, groupLevel);
    node.append_attribute("videotype") = video.type.c_str();
    if (video.type != "Web") node.append_attribute("fileIDRef") = bins.resolveId(video.fileBinId).c_str();
    node.append_attribute("imageIDRef") = bins.resolveId(ctrl.binItemId).c_str();
    node.append_attribute("tag") = video.tag.c_str();
}

void writeObjectNode(pugi::xml_node parent, const Control& ctrl, const BinRegistry& bins, int groupLevel) {
    if (!ctrl.hwpxXml.empty() && writeKeptObject(parent, ctrl, bins, groupLevel)) return;
    if (ctrl.kind == ControlKind::Table) {
        writeTableNode(parent, ctrl, bins);
        return;
    }
    if (ctrl.kind == ControlKind::Equation) {
        auto eqNode = parent.append_child("hp:equation");
        writePlacementNodes(eqNode, ctrl, bins, 1910000001u);
        eqNode.append_attribute("version") = "Equation Version 60";
        if (ctrl.baseLine >= 0) eqNode.append_attribute("baseLine") = ctrl.baseLine;
        eqNode.append_attribute("textColor") = "#000000";
        eqNode.append_attribute("baseUnit") = ctrl.baseUnit > 0 ? ctrl.baseUnit : 1000;
        eqNode.append_attribute("lineMode") = "CHAR";
        if (!ctrl.equationFont.empty()) {
            eqNode.append_attribute("font") = ctrl.equationFont.c_str();
        }
        auto script = eqNode.append_child("hp:script");
        script.text().set(ctrl.script.c_str());
        return;
    }

    if (ctrl.kind == ControlKind::Picture) {
        auto picNode = parent.append_child("hp:pic");
        writeShapeGeometryAndCommon(picNode, ctrl, bins, groupLevel);
        picNode.append_attribute("reverse") = 0;

        if (!ctrl.strokeType.empty() && ctrl.strokeType != "NONE" && ctrl.strokeWidth > 0) {
            auto ls = picNode.append_child("hp:lineShape");
            ls.append_attribute("color") =
                ctrl.strokeColor.empty() ? "#000000" : ctrl.strokeColor.c_str();
            ls.append_attribute("width") = ctrl.strokeWidth;
            ls.append_attribute("style") = toHwpxLineType(ctrl.strokeType).c_str();
            ls.append_attribute("endCap") = ctrl.strokeEnd == 0 ? "ROUND" : "FLAT";
            ls.append_attribute("headStyle") = "NORMAL";
            ls.append_attribute("tailStyle") = "NORMAL";
            ls.append_attribute("headfill") = 1;
            ls.append_attribute("tailfill") = 1;
            ls.append_attribute("headSz") = "MEDIUM_MEDIUM";
            ls.append_attribute("tailSz") = "MEDIUM_MEDIUM";
            ls.append_attribute("outlineStyle") = "NORMAL";
            ls.append_attribute("alpha") = 0;
        }

        auto img = picNode.append_child("hc:img");
        img.append_attribute("binaryItemIDRef") = bins.resolveId(ctrl.binItemId).c_str();
        img.append_attribute("bright") = ctrl.picture.bright;
        img.append_attribute("contrast") = ctrl.picture.contrast;
        img.append_attribute("effect") = toHwpxPictureEffect(ctrl.picture.effect);
        img.append_attribute("alpha") = ctrl.picture.transparency;

        const Placement p = ctrl.placement.value_or(Placement{});
        const double sx = p.scaleX > 1e-6 ? p.scaleX : 1.0;
        const double sy = p.scaleY > 1e-6 ? p.scaleY : 1.0;
        const std::int32_t orgW =
            std::fabs(sx - 1.0) > 1e-6
                ? static_cast<std::int32_t>(std::llround(static_cast<double>(p.width) / sx))
                : static_cast<std::int32_t>(p.width);
        const std::int32_t orgH =
            std::fabs(sy - 1.0) > 1e-6
                ? static_cast<std::int32_t>(std::llround(static_cast<double>(p.height) / sy))
                : static_cast<std::int32_t>(p.height);
        const std::int32_t dimW = ctrl.picture.originWidth > 0 ? ctrl.picture.originWidth : orgW;
        const std::int32_t dimH = ctrl.picture.originHeight > 0 ? ctrl.picture.originHeight : orgH;
        const std::int32_t rectW = ctrl.picture.rectWidth > 0 ? ctrl.picture.rectWidth : dimW;
        const std::int32_t rectH = ctrl.picture.rectHeight > 0 ? ctrl.picture.rectHeight : dimH;

        auto imgRect = picNode.append_child("hp:imgRect");
        const std::array<ShapePoint, 4> pts = {{{0, 0}, {rectW, 0}, {rectW, rectH}, {0, rectH}}};
        for (int i = 0; i < 4; ++i) {
            const std::string ptName = "hc:pt" + std::to_string(i);
            auto pt = imgRect.append_child(ptName.c_str());
            pt.append_attribute("x") = pts[static_cast<std::size_t>(i)].x;
            pt.append_attribute("y") = pts[static_cast<std::size_t>(i)].y;
        }

        auto clip = picNode.append_child("hp:imgClip");
        clip.append_attribute("left") = ctrl.picture.clipLeft;
        clip.append_attribute("right") = ctrl.picture.clipRight;
        clip.append_attribute("top") = ctrl.picture.clipTop;
        clip.append_attribute("bottom") = ctrl.picture.clipBottom;

        auto inMargin = picNode.append_child("hp:inMargin");
        inMargin.append_attribute("left") = ctrl.picture.padLeft;
        inMargin.append_attribute("right") = ctrl.picture.padRight;
        inMargin.append_attribute("top") = ctrl.picture.padTop;
        inMargin.append_attribute("bottom") = ctrl.picture.padBottom;

        if (ctrl.picture.originWidth > 0 || ctrl.picture.originHeight > 0) {
            auto dim = picNode.append_child("hp:imgDim");
            dim.append_attribute("dimwidth") = ctrl.picture.originWidth;
            dim.append_attribute("dimheight") = ctrl.picture.originHeight;
        }
        return;
    }

    // Container / Ole / Shape
    // HWP 에서 온 차트(OLE 안에 OOXMLChartContents 가 든 것)는 한글처럼 hp:switch 로 쓴다:
    // hp:case(ooxmlchart) 의 hp:chart(크기·위치만, chartIDRef 는 차트 XML) + hp:default 의 hp:ole(그 OLE).
    if (ctrl.kind == ControlKind::Ole && !ctrl.binItemId.empty() && groupLevel == 0) {
        const std::string id = bins.resolveId(ctrl.binItemId);
        for (const auto& entry : bins.entries) {
            if (entry.id != id) continue;
            Bytes chartXml;
            if (!cfb::readOleStream(entry.data, "OOXMLChartContents", chartXml) || chartXml.empty()) break;
            const std::string path = "Chart/chart" + std::to_string(gKeptParts.size() + 1) + ".xml";
            gKeptParts.emplace_back(path, std::move(chartXml));
            auto sw = parent.append_child("hp:switch");
            auto cs = sw.append_child("hp:case");
            cs.append_attribute("hp:required-namespace") = "http://www.hancom.co.kr/hwpml/2016/ooxmlchart";
            auto chart = cs.append_child("hp:chart");
            writePlacementNodes(chart, ctrl, bins, 1732656412u, false);
            chart.append_attribute("chartIDRef") = path.c_str();
            Control fallback = ctrl;
            fallback.hwpxXml.clear();
            writeOleNode(sw.append_child("hp:default"), fallback, bins, groupLevel);
            return;
        }
    }
    if (ctrl.kind == ControlKind::Ole) {
        writeOleNode(parent, ctrl, bins, groupLevel);
        return;
    }
    if (ctrl.video) {
        writeVideoNode(parent, ctrl, bins, groupLevel);
        return;
    }
    std::string tag = "hp:rect";
    if (ctrl.kind == ControlKind::Container || ctrl.shapeType == "container") tag = "hp:container";
    else if (ctrl.shapeType == "ellipse") tag = "hp:ellipse";
    else if (ctrl.shapeType == "line") tag = "hp:line";
    else if (ctrl.shapeType == "arc") tag = "hp:arc";
    else if (ctrl.shapeType == "polygon") tag = "hp:polygon";
    else if (ctrl.shapeType == "curve") tag = "hp:curve";
    else if (ctrl.shapeType == "textart") tag = "hp:textart";

    auto node = parent.append_child(tag.c_str());
    writeShapeGeometryAndCommon(node, ctrl, bins, groupLevel);
    if (tag == "hp:rect") {
        node.append_attribute("ratio") = ctrl.cornerRatio;
    } else if (tag == "hp:line") {
        node.append_attribute("isReverseHV") = 0;
    } else if (tag == "hp:ellipse") {
        node.append_attribute("intervalDirty") = 0;
        node.append_attribute("hasArcPr") = 0;
        node.append_attribute("arcType") = "NORMAL";
    } else if (tag == "hp:arc") {
        node.append_attribute("type") = "NORMAL";
    } else if (tag == "hp:textart") {
        node.append_attribute("text") = ctrl.text.c_str();
    }

    if (ctrl.kind != ControlKind::Container && ctrl.shapeType != "container") {
        auto ls = node.append_child("hp:lineShape");
        const bool hasStroke =
            !ctrl.strokeType.empty() && ctrl.strokeType != "NONE" && ctrl.strokeWidth > 0;
        ls.append_attribute("color") =
            ctrl.strokeColor.empty() ? "#000000" : ctrl.strokeColor.c_str();
        ls.append_attribute("width") = hasStroke ? ctrl.strokeWidth : 0;
        ls.append_attribute("style") =
            hasStroke ? toHwpxLineType(ctrl.strokeType).c_str() : "NONE";
        ls.append_attribute("endCap") = ctrl.strokeEnd == 0 ? "ROUND" : "FLAT";
        ls.append_attribute("headStyle") = "NORMAL";
        ls.append_attribute("tailStyle") = "NORMAL";
        ls.append_attribute("headfill") = 1;
        ls.append_attribute("tailfill") = 1;
        ls.append_attribute("headSz") = "MEDIUM_MEDIUM";
        ls.append_attribute("tailSz") = "MEDIUM_MEDIUM";
        ls.append_attribute("outlineStyle") = "NORMAL";
        ls.append_attribute("alpha") = 0;

        writeFillBrushNode(node, ctrl.fillColor, ctrl.gradColor, ctrl.gradAngle, ctrl.binItemId,
                           ctrl.binItemId.empty() ? -1 : 5, bins);
    }

    const Placement p = ctrl.placement.value_or(Placement{});
    const std::int32_t boxW = static_cast<std::int32_t>(p.width);
    const std::int32_t boxH = static_cast<std::int32_t>(p.height);
    const std::int32_t orgW =
        (p.scaleX > 1e-6 && std::fabs(p.scaleX - 1.0) > 1e-6)
            ? static_cast<std::int32_t>(std::llround(static_cast<double>(boxW) / p.scaleX))
            : boxW;
    const std::int32_t orgH =
        (p.scaleY > 1e-6 && std::fabs(p.scaleY - 1.0) > 1e-6)
            ? static_cast<std::int32_t>(std::llround(static_cast<double>(boxH) / p.scaleY))
            : boxH;

    if (!ctrl.subLists.empty()) {
        auto dt = node.append_child("hp:drawText");
        dt.append_attribute("lastWidth") = ctrl.boxTextWidth > 0 ? ctrl.boxTextWidth : p.width;
        dt.append_attribute("name") = "";
        dt.append_attribute("editable") = 0;
        writeSubListNode(dt, ctrl.subLists.front(), bins);
        auto tm = dt.append_child("hp:textMargin");
        tm.append_attribute("left") = ctrl.boxPadLeft;
        tm.append_attribute("right") = ctrl.boxPadRight;
        tm.append_attribute("top") = ctrl.boxPadTop;
        tm.append_attribute("bottom") = ctrl.boxPadBottom;
    }

    auto transformPt = [&](ShapePoint ptVal) {
        if (p.flipHorizontal && boxW > 0) ptVal.x = boxW - ptVal.x;
        if (p.flipVertical && boxH > 0) ptVal.y = boxH - ptVal.y;
        if (p.scaleX > 1e-6 && std::fabs(p.scaleX - 1.0) > 1e-6) {
            ptVal.x = static_cast<std::int32_t>(std::llround(static_cast<double>(ptVal.x) / p.scaleX));
        }
        if (p.scaleY > 1e-6 && std::fabs(p.scaleY - 1.0) > 1e-6) {
            ptVal.y = static_cast<std::int32_t>(std::llround(static_cast<double>(ptVal.y) / p.scaleY));
        }
        return ptVal;
    };

    if (tag == "hp:textart") {
        const TextArt art = ctrl.textArt.value_or(TextArt{});
        auto shadow = node.append_child("hp:shadow");
        shadow.append_attribute("type") = "NONE";
        shadow.append_attribute("color") = "#B2B2B2";
        shadow.append_attribute("offsetX") = 0;
        shadow.append_attribute("offsetY") = 0;
        shadow.append_attribute("alpha") = 0;
        const bool noCorners = std::all_of(art.corners.begin(), art.corners.end(),
                                           [](const ShapePoint& pt) { return pt.x == 0 && pt.y == 0; });
        const ShapePoint fallback[4] = {{0, 0}, {orgW, 0}, {orgW, orgH}, {0, orgH}};
        for (int k = 0; k < 4; ++k) {
            const std::string ptName = "hc:pt" + std::to_string(k);
            const ShapePoint pt = noCorners ? fallback[k] : art.corners[static_cast<std::size_t>(k)];
            auto ptNode = node.append_child(ptName.c_str());
            ptNode.append_attribute("x") = pt.x;
            ptNode.append_attribute("y") = pt.y;
        }
        auto pr = node.append_child("hp:textartPr");
        pr.append_attribute("fontName") = art.fontName.c_str();
        pr.append_attribute("fontStyle") = art.fontStyle.c_str();
        pr.append_attribute("fontType") = art.fontType.c_str();
        const std::string shapeName = !art.shape.empty() ? art.shape : textArtShapeName(art.shapeNumber);
        pr.append_attribute("textShape") = shapeName.empty() ? "RECTANGLE" : shapeName.c_str();
        pr.append_attribute("lineSpacing") = art.lineSpacing;
        pr.append_attribute("charSpacing") = art.charSpacing;
        pr.append_attribute("align") = art.align.c_str();
        auto artShadow = pr.append_child("hp:shadow");
        artShadow.append_attribute("type") = art.shadowType.c_str();
        artShadow.append_attribute("color") = art.shadowColor.c_str();
        artShadow.append_attribute("offsetX") = art.shadowX;
        artShadow.append_attribute("offsetY") = art.shadowY;
        artShadow.append_attribute("alpha") = art.shadowAlpha;
    }
    if (tag == "hp:rect") {
        const std::pair<const char*, ShapePoint> corners[4] = {
            {"hc:pt0", {0, 0}},
            {"hc:pt1", {orgW, 0}},
            {"hc:pt2", {orgW, orgH}},
            {"hc:pt3", {0, orgH}},
        };
        for (const auto& [ptTag, cPt] : corners) {
            auto pt = node.append_child(ptTag);
            pt.append_attribute("x") = cPt.x;
            pt.append_attribute("y") = cPt.y;
        }
        for (ShapePoint ptVal : ctrl.points) {
            ptVal = transformPt(ptVal);
            auto pt = node.append_child("hc:pt");
            pt.append_attribute("x") = ptVal.x;
            pt.append_attribute("y") = ptVal.y;
        }
    } else if (tag == "hp:line") {
        ShapePoint sPt{0, 0};
        ShapePoint ePt{orgW, orgH};
        if (ctrl.points.size() >= 2) {
            sPt = transformPt(ctrl.points[0]);
            ePt = transformPt(ctrl.points[1]);
        }
        auto sp = node.append_child("hc:startPt");
        sp.append_attribute("x") = sPt.x;
        sp.append_attribute("y") = sPt.y;
        auto ep = node.append_child("hc:endPt");
        ep.append_attribute("x") = ePt.x;
        ep.append_attribute("y") = ePt.y;
    } else if (tag == "hp:ellipse" || tag == "hp:arc") {
        auto addPt = [&](const char* ptTag, std::int32_t x, std::int32_t y) {
            auto pt = node.append_child(ptTag);
            pt.append_attribute("x") = x;
            pt.append_attribute("y") = y;
        };
        if (ctrl.points.size() >= 3) {
            const char* tags[] = {"hc:center", "hc:ax1", "hc:ax2", "hc:start1", "hc:end1", "hc:start2", "hc:end2"};
            // 호는 중심과 두 축 끝뿐이다(한글 2024 가 쓰는 hp:arc).
            const int count = tag == "hp:arc" ? 3 : 7;
            for (int i = 0; i < count; ++i) {
                const ShapePoint pt = i < static_cast<int>(ctrl.points.size()) ? transformPt(ctrl.points[static_cast<std::size_t>(i)])
                                                                                : ShapePoint{0, 0};
                addPt(tags[i], pt.x, pt.y);
            }
        } else {
            addPt("hc:center", orgW / 2, orgH / 2);
            addPt("hc:ax1", orgW, orgH / 2);
            addPt("hc:ax2", orgW / 2, 0);
            addPt("hc:start1", 0, 0);
            addPt("hc:end1", 0, 0);
            addPt("hc:start2", 0, 0);
            addPt("hc:end2", 0, 0);
        }
    } else if (tag == "hp:curve") {
        if (ctrl.points.size() >= 2) {
            for (std::size_t i = 0; i + 1 < ctrl.points.size(); ++i) {
                const ShapePoint p1 = transformPt(ctrl.points[i]);
                const ShapePoint p2 = transformPt(ctrl.points[i + 1]);
                auto seg = node.append_child("hp:seg");
                seg.append_attribute("type") = "CURVE";
                seg.append_attribute("x1") = p1.x;
                seg.append_attribute("y1") = p1.y;
                seg.append_attribute("x2") = p2.x;
                seg.append_attribute("y2") = p2.y;
            }
        } else {
            auto seg = node.append_child("hp:seg");
            seg.append_attribute("type") = "CURVE";
            seg.append_attribute("x1") = 0;
            seg.append_attribute("y1") = 0;
            seg.append_attribute("x2") = orgW;
            seg.append_attribute("y2") = orgH;
        }
    } else {
        if (!ctrl.points.empty()) {
            for (ShapePoint ptVal : ctrl.points) {
                ptVal = transformPt(ptVal);
                auto pt = node.append_child("hc:pt");
                pt.append_attribute("x") = ptVal.x;
                pt.append_attribute("y") = ptVal.y;
            }
        } else if (tag == "hp:polygon") {
            const ShapePoint defaultPoly[5] = {{0, 0}, {orgW, 0}, {orgW, orgH}, {0, orgH}, {0, 0}};
            for (const auto& cPt : defaultPoly) {
                auto pt = node.append_child("hc:pt");
                pt.append_attribute("x") = cPt.x;
                pt.append_attribute("y") = cPt.y;
            }
        }
    }

    for (const auto& subCtrl : ctrl.children) {
        writeObjectNode(node, subCtrl, bins, groupLevel + 1);
    }
}

void writeControlInRun(pugi::xml_node runNode, const Control& ctrl, const BinRegistry& bins,
                       const Section* firstParaSec, std::size_t masterPageBaseIdx) {
    switch (ctrl.kind) {
        case ControlKind::SectionDef: {
            writeSecPrNode(runNode, &ctrl, firstParaSec, masterPageBaseIdx);
            break;
        }
        case ControlKind::ColumnDef: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto colPr = ctrlNode.append_child("hp:colPr");
            colPr.append_attribute("id") = "";
            colPr.append_attribute("type") =
                (ctrl.columnType.empty() || ctrl.columnType == "NORMAL")
                    ? "NEWSPAPER"
                    : ctrl.columnType.c_str();
            colPr.append_attribute("layout") =
                ctrl.columnLayout.empty() ? "LEFT" : ctrl.columnLayout.c_str();
            colPr.append_attribute("colCount") = ctrl.columnCount > 0 ? ctrl.columnCount : 1;
            colPr.append_attribute("sameSz") = ctrl.columnSameSize ? 1 : 0;
            colPr.append_attribute("sameGap") = ctrl.columnGap;
            for (const auto& slice : ctrl.columnWidths) {
                auto cs = colPr.append_child("hp:colSz");
                cs.append_attribute("width") = slice.width;
                cs.append_attribute("gap") = slice.gap;
            }
            if (!ctrl.columnLineType.empty() && ctrl.columnLineType != "NONE") {
                auto cl = colPr.append_child("hp:colLine");
                cl.append_attribute("type") = toHwpxLineType(ctrl.columnLineType).c_str();
                cl.append_attribute("width") = toHwpxBorderWidth(ctrl.columnLineWidth).c_str();
                cl.append_attribute("color") =
                    ctrl.columnLineColor.empty() ? "#000000" : ctrl.columnLineColor.c_str();
            }
            break;
        }
        case ControlKind::Table:
        case ControlKind::Picture:
        case ControlKind::Shape:
        case ControlKind::Container:
        case ControlKind::Ole:
        case ControlKind::Equation: {
            writeObjectNode(runNode, ctrl, bins);
            break;
        }
        case ControlKind::Header:
        case ControlKind::Footer: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto hfNode =
                ctrlNode.append_child(ctrl.kind == ControlKind::Footer ? "hp:footer" : "hp:header");
            hfNode.append_attribute("id") = 0;
            hfNode.append_attribute("applyPageType") =
                ctrl.applyPageType.empty() ? "BOTH" : ctrl.applyPageType.c_str();
            const SubList sub = ctrl.subLists.empty() ? SubList{} : ctrl.subLists.front();
            writeSubListNode(hfNode, sub, bins);
            break;
        }
        case ControlKind::Footnote:
        case ControlKind::Endnote: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto nNode = ctrlNode.append_child(
                ctrl.kind == ControlKind::Endnote ? "hp:endNote" : "hp:footNote");
            nNode.append_attribute("number") = ctrl.number;
            const SubList sub = ctrl.subLists.empty() ? SubList{} : ctrl.subLists.front();
            writeSubListNode(nNode, sub, bins);
            break;
        }
        case ControlKind::AutoNumber:
        case ControlKind::NewNumber: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto node = ctrlNode.append_child(
                ctrl.kind == ControlKind::NewNumber ? "hp:newNum" : "hp:autoNum");
            node.append_attribute("numType") =
                ctrl.numType.empty() ? "PAGE" : ctrl.numType.c_str();
            node.append_attribute("num") = ctrl.number;
            auto fmt = node.append_child("hp:autoNumFormat");
            fmt.append_attribute("type") =
                ctrl.numFormat.empty() ? "DIGIT" : ctrl.numFormat.c_str();
            fmt.append_attribute("userChar") = "";
            fmt.append_attribute("prefixChar") = ctrl.prefix.c_str();
            fmt.append_attribute("suffixChar") = ctrl.suffix.c_str();
            fmt.append_attribute("supscript") = 0;
            break;
        }
        case ControlKind::PageHiding: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto node = ctrlNode.append_child("hp:pageHiding");
            node.append_attribute("hideHeader") = ctrl.hideHeader ? 1 : 0;
            node.append_attribute("hideFooter") = ctrl.hideFooter ? 1 : 0;
            node.append_attribute("hideMasterPage") = ctrl.hideMaster ? 1 : 0;
            node.append_attribute("hideBorder") = ctrl.hideBorder ? 1 : 0;
            node.append_attribute("hideFill") = ctrl.hideFill ? 1 : 0;
            node.append_attribute("hidePageNum") = ctrl.hidePageNum ? 1 : 0;
            break;
        }
        case ControlKind::PageNumPos: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto node = ctrlNode.append_child("hp:pageNum");
            node.append_attribute("pos") =
                ctrl.pageNumPos.empty() ? "BOTTOM_CENTER" : ctrl.pageNumPos.c_str();
            node.append_attribute("formatType") =
                ctrl.numFormat.empty() ? "DIGIT" : ctrl.numFormat.c_str();
            node.append_attribute("sideChar") =
                !ctrl.prefix.empty() ? ctrl.prefix.c_str() : ctrl.suffix.c_str();
            break;
        }
        case ControlKind::Bookmark: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto node = ctrlNode.append_child("hp:bookmark");
            node.append_attribute("name") = ctrl.name.c_str();
            break;
        }
        case ControlKind::Field: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto fb = ctrlNode.append_child("hp:fieldBegin");
            const bool memo = isMemoField(ctrl);
            const auto memoId = gFieldIds.memoId.find(&ctrl);
            const std::uint32_t fieldId = memo && memoId != gFieldIds.memoId.end() ? memoId->second : gFieldIds.next++;
            gFieldIds.open.push_back(fieldId);
            fb.append_attribute("id") = fieldId;
            // 모르는 필드는 읽은 이름 그대로 쓴다. 이름도 없으면(HWP 에서 온 모르는 필드) 예전처럼 HYPERLINK 다.
            const char* fieldType = toHwpxFieldType(ctrl.ctrlId);
            fb.append_attribute("type") =
                fieldType ? fieldType : (!ctrl.fieldType.empty() ? ctrl.fieldType.c_str() : "HYPERLINK");
            fb.append_attribute("name") = ctrl.name.c_str();
            if (ctrl.fieldProps & 1u) fb.append_attribute("editable") = "1";
            if (ctrl.fieldProps & 0x8000u) fb.append_attribute("dirty") = "1";
            if (memo) {
                // 한글 2024 메모: zorder 는 문서 안 메모 차례, fieldid 는 %%me 의 정수, 매개변수 일곱, 메모 글 subList.
                const std::uint32_t number = gFieldIds.memoNumber.count(&ctrl) ? gFieldIds.memoNumber[&ctrl] : 1u;
                MemoCommand cmd = parseMemoCommand(ctrl.command);
                cmd.number = number;
                fb.append_attribute("zorder") = number;
                fb.append_attribute("fieldid") = ctrlIdNumber(ctrl.ctrlId);  // %%me 623209829, 메모 고침표 %%mr
                auto params = fb.append_child("hp:parameters");
                params.append_attribute("cnt") = 7;
                params.append_attribute("name") = "";
                const auto put = [&](const char* type, const char* name, const std::string& value) {
                    auto p = params.append_child(type);
                    p.append_attribute("name") = name;
                    p.text().set(value.c_str());
                };
                put("hp:integerParam", "Prop", std::to_string(static_cast<unsigned>(ctrl.fieldExtra)));
                put("hp:stringParam", "Command", memoCommandText(cmd));
                put("hp:stringParam", "ID", (cmd.kind == "MEMO$U" ? "memoc" : "memo") + std::to_string(number));
                put("hp:integerParam", "Number", std::to_string(number));
                put("hp:stringParam", "Author", cmd.author);
                put("hp:stringParam", "MemoShapeIDRef", std::to_string(cmd.shapeRef));
                put("hp:stringParam", "CreateDateTime", memoCreateDateTime(cmd.created));
                SubList text = ctrl.subLists.empty() ? SubList{} : ctrl.subLists.front();
                if (text.paragraphs.empty()) text.paragraphs.emplace_back();
                writeSubListNode(fb, text, bins);
                break;
            }
            const bool proof = proofKindOf(ctrl) != ProofKind::None;
            if (proof) {
                // 한글 2024 교정 부호: zorder 는 차례 번호(%sig 는 -1), fieldid 는 id 네 글자의 정수, Prop 은 늘 둔다.
                fb.append_attribute("zorder") = ctrl.memoNumber > 0 ? static_cast<int>(ctrl.memoNumber) : -1;
                fb.append_attribute("fieldid") = ctrlIdNumber(ctrl.ctrlId);
            } else if (ctrl.ctrlId == "%clk") {
                // 한글 2024 누름틀: 고친 표시를 늘 적고(손대지 않았으면 0 — 안의 글은 안내문이다), zorder -1, fieldid 는 %clk.
                if (!(ctrl.fieldProps & 0x8000u)) fb.append_attribute("dirty") = "0";
                fb.append_attribute("zorder") = -1;
                fb.append_attribute("fieldid") = ctrlIdNumber(ctrl.ctrlId);
            }
            auto params = fb.append_child("hp:parameters");
            params.append_attribute("cnt") = 0;
            params.append_attribute("name") = "";
            const auto stringParam = [&](const char* name, const std::string& value) {
                auto sp = params.append_child("hp:stringParam");
                sp.append_attribute("name") = name;
                // 명령 끝 빈칸이 뜻을 가진다(누름틀 "...:  "). 한글처럼 공백을 지키라고 적는다.
                if (std::string_view(name) == "Command") sp.append_attribute("xml:space") = "preserve";
                sp.text().set(value.c_str());
            };
            if (ctrl.fieldExtra != 0 || proof) {
                auto ip = params.append_child("hp:integerParam");
                ip.append_attribute("name") = "Prop";
                ip.text().set(static_cast<unsigned>(ctrl.fieldExtra));
            }
            stringParam("Command", ctrl.command);
            for (const auto& [name, value] : ctrl.fieldStrings) stringParam(name.c_str(), value);
            // 계산식은 한글처럼 명령을 나눈 식·결과 형식·마지막 결과도 둔다.
            // 형식이 없는 명령("식;결과")이면 한글처럼 명령 전체를 Formula 로 둔다.
            if (ctrl.ctrlId == "%fmu") {
                const FormulaCommand f = parseFormulaCommand(ctrl.command);
                stringParam("Formula", f.hasFormat ? f.formula : ctrl.command);
                if (f.hasFormat) {
                    stringParam("ResultFormat", f.format);
                    stringParam("LastResult", f.result);
                }
            }
            params.attribute("cnt") = static_cast<unsigned>(std::distance(params.children().begin(), params.children().end()));
            break;
        }
        case ControlKind::HiddenComment: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto hc = ctrlNode.append_child("hp:hiddenComment");
            const SubList sub = ctrl.subLists.empty() ? SubList{} : ctrl.subLists.front();
            writeSubListNode(hc, sub, bins);
            break;
        }
        case ControlKind::Dutmal: {
            auto dm = runNode.append_child("hp:dutmal");
            dm.append_attribute("posType") = ctrl.dutmalPos.c_str();
            dm.append_attribute("szRatio") = ctrl.dutmalSizeRatio;
            dm.append_attribute("option") = ctrl.dutmalOption;
            dm.append_attribute("styleIDRef") = ctrl.dutmalStyle;
            dm.append_attribute("align") = ctrl.dutmalAlign.c_str();
            dm.append_child("hp:mainText").text().set(ctrl.text.c_str());
            dm.append_child("hp:subText").text().set(ctrl.subText.c_str());
            break;
        }
        case ControlKind::Compose: {
            auto cmp = runNode.append_child("hp:compose");
            std::vector<std::uint32_t> shapes = ctrl.composeCharShapes;
            if (shapes.size() < 10) shapes.resize(10, 0xFFFFFFFFu);
            cmp.append_attribute("circleType") = ctrl.composeCircle.c_str();
            cmp.append_attribute("charSz") = static_cast<int>(ctrl.composeCharSize);
            cmp.append_attribute("composeType") = ctrl.composeType.c_str();
            cmp.append_attribute("charPrCnt") = static_cast<unsigned>(shapes.size());
            cmp.append_attribute("composeText") = ctrl.text.c_str();
            for (const std::uint32_t id : shapes) cmp.append_child("hp:charPr").append_attribute("prIDRef") = id;
            break;
        }
        case ControlKind::FieldEnd: {
            auto ctrlNode = runNode.append_child("hp:ctrl");
            auto fe = ctrlNode.append_child("hp:fieldEnd");
            std::uint32_t begin = 0;
            if (!gFieldIds.open.empty()) {
                begin = gFieldIds.open.back();
                gFieldIds.open.pop_back();
            }
            fe.append_attribute("beginIDRef") = begin;
            break;
        }
        case ControlKind::Unknown:
            break;
    }
}

// 변경 추적 표시(한글처럼 hp:t 안에 둔다). begin 이면 시작, 아니면 끝(paraend 0).
void appendTrackMark(pugi::xml_node tNode, std::uint8_t track, std::uint32_t change, std::uint32_t mark, bool begin) {
    const char* name = track == kTrackInsert ? (begin ? "hp:insertBegin" : "hp:insertEnd")
                                             : (begin ? "hp:deleteBegin" : "hp:deleteEnd");
    auto n = tNode.append_child(name);
    n.append_attribute("Id") = mark;
    n.append_attribute("TcId") = change;
    if (!begin) n.append_attribute("paraend") = 0;
}

void writeTextWithSpecials(pugi::xml_node runNode, std::string_view text, std::uint32_t charStyle = kNoCharStyle,
                           const InlineItem* openTrack = nullptr, const InlineItem* closeTrack = nullptr) {
    if (text.empty()) return;
    auto tNode = runNode.append_child("hp:t");
    if (charStyle != kNoCharStyle) tNode.append_attribute("charStyleIDRef") = charStyle;
    if (openTrack) appendTrackMark(tNode, openTrack->track, openTrack->trackChange, openTrack->trackMark, true);
    std::string chunk;
    auto flushChunk = [&]() {
        if (!chunk.empty()) {
            tNode.append_child(pugi::node_pcdata).set_value(chunk.c_str());
            chunk.clear();
        }
    };
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        if (c0 == '\t') {
            flushChunk();
            tNode.append_child("hp:tab");
            ++i;
        } else if (c0 == '\n') {
            flushChunk();
            tNode.append_child("hp:lineBreak");
            ++i;
        } else if (c0 == 0xC2 && i + 1 < text.size() &&
                   static_cast<unsigned char>(text[i + 1]) == 0xA0) {
            flushChunk();
            tNode.append_child("hp:nbSpace");
            i += 2;
        } else if (c0 == 0xE2 && i + 2 < text.size() &&
                   static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(text[i + 2]) == 0x87) {
            flushChunk();
            tNode.append_child("hp:fwSpace");
            i += 3;
        } else {
            chunk.push_back(text[i]);
            ++i;
        }
    }
    flushChunk();
    if (closeTrack) appendTrackMark(tNode, closeTrack->track, closeTrack->trackChange, closeTrack->trackMark, false);
}

void writeParagraphNode(pugi::xml_node parent, const Paragraph& para, const BinRegistry& bins,
                        const Section* firstParaSec, std::size_t masterPageBaseIdx) {
    auto pNode = parent.append_child("hp:p");
    pNode.append_attribute("id") = para.fileId != 0 ? para.fileId : 2147483648u;
    pNode.append_attribute("paraPrIDRef") = para.paraShapeId;
    pNode.append_attribute("styleIDRef") = para.styleId;
    pNode.append_attribute("pageBreak") = para.pageBreak ? 1 : 0;
    pNode.append_attribute("columnBreak") = para.columnBreak ? 1 : 0;
    pNode.append_attribute("merged") = 0;

    bool hasSecCtrlInPara = false;
    for (const auto& r : para.runs) {
        for (const auto& it : r.items) {
            if (it.type == InlineItem::Type::Control && it.control.kind == ControlKind::SectionDef) {
                hasSecCtrlInPara = true;
                break;
            }
        }
        if (hasSecCtrlInPara) break;
    }

    bool secPrWritten = false;
    if (para.runs.empty()) {
        auto runNode = pNode.append_child("hp:run");
        runNode.append_attribute("charPrIDRef") = 0;
        if (firstParaSec && !secPrWritten) {
            writeSecPrNode(runNode, nullptr, firstParaSec, masterPageBaseIdx);
            secPrWritten = true;
        }
    } else {
        // 변경 추적 쌍은 같은 (종류, 쌍 id) 가 이어지는 동안 하나다. 글 조각 안에 시작·끝을 넣고, 개체면 빈 hp:t 에 둔다.
        std::vector<const InlineItem*> flat;
        for (const auto& r : para.runs) {
            for (const auto& it : r.items) flat.push_back(&it);
        }
        const auto sameTrack = [](const InlineItem* a, const InlineItem* b) {
            return a && b && a->track == b->track && a->trackMark == b->trackMark && a->trackChange == b->trackChange;
        };
        std::size_t flatAt = 0;
        for (const auto& r : para.runs) {
            auto runNode = pNode.append_child("hp:run");
            runNode.append_attribute("charPrIDRef") = r.charShapeId;
            if (r.charTrackChange != 0) runNode.append_attribute("charTcId") = r.charTrackChange;
            if (firstParaSec && !secPrWritten && !hasSecCtrlInPara) {
                writeSecPrNode(runNode, nullptr, firstParaSec, masterPageBaseIdx);
                secPrWritten = true;
            }
            std::string activeMarkpen;
            for (const auto& item : r.items) {
                if (item.markpen != activeMarkpen) {
                    if (!activeMarkpen.empty()) {
                        runNode.append_child("hp:markpenEnd");
                    }
                    if (!item.markpen.empty()) {
                        auto mb = runNode.append_child("hp:markpenBegin");
                        mb.append_attribute("color") = item.markpen.c_str();
                    }
                    activeMarkpen = item.markpen;
                }
                const InlineItem* prev = flatAt > 0 ? flat[flatAt - 1] : nullptr;
                const InlineItem* next = flatAt + 1 < flat.size() ? flat[flatAt + 1] : nullptr;
                ++flatAt;
                const bool opens = item.track != 0 && !sameTrack(prev, &item);
                const bool closes = item.track != 0 && !sameTrack(&item, next);
                if (item.type == InlineItem::Type::Text && !item.text.empty()) {
                    writeTextWithSpecials(runNode, item.text, item.charStyleId, opens ? &item : nullptr,
                                          closes ? &item : nullptr);
                } else {
                    if (opens) appendTrackMark(runNode.append_child("hp:t"), item.track, item.trackChange, item.trackMark, true);
                    writeControlInRun(runNode, item.control, bins, firstParaSec, masterPageBaseIdx);
                    if (item.control.kind == ControlKind::SectionDef) {
                        secPrWritten = true;
                    }
                    if (closes) appendTrackMark(runNode.append_child("hp:t"), item.track, item.trackChange, item.trackMark, false);
                }
            }
            if (!activeMarkpen.empty()) {
                runNode.append_child("hp:markpenEnd");
            }
        }
        // 문단 끝의 빈 쌍(넣거나 지운 문단 나누기).
        if (para.endTrack.track != 0 && pNode.last_child()) {
            pugi::xml_node lastRun = pNode.last_child();
            // 한글처럼 마지막 hp:t 끝에 둔다. 따로 둔 hp:t 의 빈 쌍은 한글이 버린다.
            pugi::xml_node tNode;
            for (pugi::xml_node c = lastRun.last_child(); c; c = c.previous_sibling()) {
                if (std::string_view(c.name()) == "hp:t") {
                    tNode = c;
                    break;
                }
            }
            if (!tNode) tNode = lastRun.append_child("hp:t");
            appendTrackMark(tNode, para.endTrack.track, para.endTrack.change, para.endTrack.mark, true);
            appendTrackMark(tNode, para.endTrack.track, para.endTrack.change, para.endTrack.mark, false);
        }
    }

    if (!para.lineSegs.empty()) {
        auto lsa = pNode.append_child("hp:linesegarray");
        for (const auto& ls : para.lineSegs) {
            auto seg = lsa.append_child("hp:lineseg");
            seg.append_attribute("textpos") = ls.textPos;
            seg.append_attribute("vertpos") = ls.vertPos;
            seg.append_attribute("vertsize") = ls.vertSize;
            seg.append_attribute("textheight") = ls.textHeight;
            seg.append_attribute("baseline") = ls.baseline;
            seg.append_attribute("spacing") = ls.spacing;
            seg.append_attribute("horzpos") = ls.horzPos;
            seg.append_attribute("horzsize") = ls.horzSize;
            seg.append_attribute("flags") = ls.flags;
        }
    }
}

std::string writeSectionXml(const Section& sec, const BinRegistry& bins,
                            std::size_t masterPageBaseIdx) {
    pugi::xml_document xml;
    auto secNode = xml.append_child("hs:sec");
    secNode.append_attribute("xmlns:ha") = "http://www.hancom.co.kr/hwpml/2011/app";
    secNode.append_attribute("xmlns:hp") = "http://www.hancom.co.kr/hwpml/2011/paragraph";
    secNode.append_attribute("xmlns:hp10") = "http://www.hancom.co.kr/hwpml/2016/paragraph";
    secNode.append_attribute("xmlns:hs") = "http://www.hancom.co.kr/hwpml/2011/section";
    secNode.append_attribute("xmlns:hc") = "http://www.hancom.co.kr/hwpml/2011/core";
    secNode.append_attribute("xmlns:hh") = "http://www.hancom.co.kr/hwpml/2011/head";
    secNode.append_attribute("xmlns:hhs") = "http://www.hancom.co.kr/hwpml/2011/history";
    secNode.append_attribute("xmlns:hm") = "http://www.hancom.co.kr/hwpml/2011/master-page";
    secNode.append_attribute("xmlns:hpf") = "http://www.hancom.co.kr/schema/2011/hpf";
    secNode.append_attribute("xmlns:dc") = "http://purl.org/dc/elements/1.1/";
    secNode.append_attribute("xmlns:opf") = "http://www.idpf.org/2007/opf/";
    secNode.append_attribute("xmlns:ooxmlchart") = "http://www.hancom.co.kr/hwpml/2016/ooxmlchart";
    secNode.append_attribute("xmlns:hwpunitchar") = "http://www.hancom.co.kr/hwpml/2016/HwpUnitChar";
    secNode.append_attribute("xmlns:epub") = "http://www.idpf.org/2007/ops";
    secNode.append_attribute("xmlns:config") = "urn:oasis:names:tc:opendocument:xmlns:config:1.0";

    if (sec.paragraphs.empty()) {
        Paragraph emptyPara;
        writeParagraphNode(secNode, emptyPara, bins, &sec, masterPageBaseIdx);
    } else {
        for (std::size_t i = 0; i < sec.paragraphs.size(); ++i) {
            writeParagraphNode(secNode, sec.paragraphs[i], bins, i == 0 ? &sec : nullptr,
                               masterPageBaseIdx);
        }
    }

    return serializeXml(xml);
}

std::string writeMasterPageXml(const MasterPage& mp, std::size_t index, const BinRegistry& bins) {
    pugi::xml_document xml;
    auto root = xml.append_child("masterPage");
    root.append_attribute("xmlns:ha") = "http://www.hancom.co.kr/hwpml/2011/app";
    root.append_attribute("xmlns:hp") = "http://www.hancom.co.kr/hwpml/2011/paragraph";
    root.append_attribute("xmlns:hp10") = "http://www.hancom.co.kr/hwpml/2016/paragraph";
    root.append_attribute("xmlns:hs") = "http://www.hancom.co.kr/hwpml/2011/section";
    root.append_attribute("xmlns:hc") = "http://www.hancom.co.kr/hwpml/2011/core";
    root.append_attribute("xmlns:hh") = "http://www.hancom.co.kr/hwpml/2011/head";
    root.append_attribute("xmlns:hm") = "http://www.hancom.co.kr/hwpml/2011/master-page";
    root.append_attribute("id") = ("masterpage" + std::to_string(index)).c_str();
    root.append_attribute("type") =
        mp.applyPageType.empty() ? "BOTH" : mp.applyPageType.c_str();
    root.append_attribute("pageNumber") = mp.applyPageType == "OPTIONAL_PAGE" ? mp.pageNumber : 0u;
    root.append_attribute("pageDuplicate") = mp.duplicate ? 1 : 0;
    root.append_attribute("pageFront") = mp.printOnTop ? 1 : 0;

    writeSubListNode(root, mp.content, bins);
    return serializeXml(xml);
}

std::string writeContentHpfXml(const Document& doc, const BinRegistry& bins,
                               std::size_t sectionCount, std::size_t masterPageCount, bool memoExtended) {
    pugi::xml_document xml;
    auto pkg = xml.append_child("opf:package");
    pkg.append_attribute("xmlns:ha") = "http://www.hancom.co.kr/hwpml/2011/app";
    pkg.append_attribute("xmlns:hp") = "http://www.hancom.co.kr/hwpml/2011/paragraph";
    pkg.append_attribute("xmlns:hp10") = "http://www.hancom.co.kr/hwpml/2016/paragraph";
    pkg.append_attribute("xmlns:hs") = "http://www.hancom.co.kr/hwpml/2011/section";
    pkg.append_attribute("xmlns:hc") = "http://www.hancom.co.kr/hwpml/2011/core";
    pkg.append_attribute("xmlns:hh") = "http://www.hancom.co.kr/hwpml/2011/head";
    pkg.append_attribute("xmlns:hhs") = "http://www.hancom.co.kr/hwpml/2011/history";
    pkg.append_attribute("xmlns:hm") = "http://www.hancom.co.kr/hwpml/2011/master-page";
    pkg.append_attribute("xmlns:hpf") = "http://www.hancom.co.kr/schema/2011/hpf";
    pkg.append_attribute("xmlns:dc") = "http://purl.org/dc/elements/1.1/";
    pkg.append_attribute("xmlns:opf") = "http://www.idpf.org/2007/opf/";
    pkg.append_attribute("version") = "";
    pkg.append_attribute("unique-identifier") = "";
    pkg.append_attribute("id") = "";

    auto meta = pkg.append_child("opf:metadata");
    meta.append_child("opf:title").text().set(doc.metadata.title.c_str());
    meta.append_child("opf:language").text().set(
        doc.metadata.language.empty() ? "ko" : doc.metadata.language.c_str());
    auto addMeta = [&](const char* name, const std::string& val) {
        auto m = meta.append_child("opf:meta");
        m.append_attribute("name") = name;
        m.append_attribute("content") = "text";
        if (!val.empty()) m.text().set(val.c_str());
    };
    addMeta("creator", doc.metadata.author);
    addMeta("subject", doc.metadata.subject);
    addMeta("description", doc.metadata.comments);
    addMeta("lastsaveby", doc.metadata.lastSavedBy);
    addMeta("CreatedDate", doc.metadata.createdAt);
    addMeta("ModifiedDate", doc.metadata.modifiedAt);
    addMeta("keyword", doc.metadata.keywords);

    auto manifest = pkg.append_child("opf:manifest");
    auto addItem = [&](const std::string& id, const std::string& href, const std::string& media,
                       bool embedded = false) {
        auto item = manifest.append_child("opf:item");
        item.append_attribute("id") = id.c_str();
        item.append_attribute("href") = href.c_str();
        item.append_attribute("media-type") = media.c_str();
        if (embedded) item.append_attribute("isEmbeded") = "1";
    };

    addItem("header", "Contents/header.xml", "application/xml");
    for (std::size_t i = 0; i < masterPageCount; ++i) {
        const std::string mpId = "masterpage" + std::to_string(i);
        addItem(mpId, "Contents/" + mpId + ".xml", "application/xml");
    }
    for (const auto& b : bins.entries) {
        addItem(b.id, b.zipPath, b.mediaType, b.embedded && !b.storage);
    }
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const std::string secId = "section" + std::to_string(i);
        addItem(secId, "Contents/" + secId + ".xml", "application/xml");
    }
    addItem("settings", "settings.xml", "application/xml");
    if (memoExtended) addItem("memoextended", "Contents/memoExtended.xml", "application/xml");

    auto spine = pkg.append_child("opf:spine");
    {
        auto ref = spine.append_child("opf:itemref");
        ref.append_attribute("idref") = "header";
        ref.append_attribute("linear") = "yes";
    }
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const std::string secId = "section" + std::to_string(i);
        auto ref = spine.append_child("opf:itemref");
        ref.append_attribute("idref") = secId.c_str();
        ref.append_attribute("linear") = "yes";
    }

    return serializeXml(xml);
}

std::string writeVersionXml() {
    pugi::xml_document xml;
    auto ver = xml.append_child("hv:HCFVersion");
    ver.append_attribute("xmlns:hv") = "http://www.hancom.co.kr/hwpml/2011/version";
    ver.append_attribute("tagetApplication") = "WORDPROCESSOR";
    ver.append_attribute("major") = 5;
    ver.append_attribute("minor") = 1;
    ver.append_attribute("micro") = 1;
    ver.append_attribute("buildNumber") = 0;
    ver.append_attribute("os") = 1;
    ver.append_attribute("xmlVersion") = "1.4";
    ver.append_attribute("application") = "Hancom Office Hangul";
    ver.append_attribute("appVersion") = "11, 0, 0, 0";
    return serializeXml(xml);
}

std::string writeSettingsXml() {
    pugi::xml_document xml;
    auto app = xml.append_child("ha:HWPApplicationSetting");
    app.append_attribute("xmlns:ha") = "http://www.hancom.co.kr/hwpml/2011/app";
    app.append_attribute("xmlns:config") = "urn:oasis:names:tc:opendocument:xmlns:config:1.0";
    auto caret = app.append_child("ha:CaretPosition");
    caret.append_attribute("listIDRef") = 0;
    caret.append_attribute("paraIDRef") = 0;
    caret.append_attribute("pos") = 0;
    return serializeXml(xml);
}

std::string writeContainerXml() {
    pugi::xml_document xml;
    auto container = xml.append_child("ocf:container");
    container.append_attribute("xmlns:ocf") = "urn:oasis:names:tc:opendocument:xmlns:container";
    container.append_attribute("xmlns:hpf") = "http://www.hancom.co.kr/schema/2011/hpf";
    auto rootfiles = container.append_child("ocf:rootfiles");
    auto rf1 = rootfiles.append_child("ocf:rootfile");
    rf1.append_attribute("full-path") = "Contents/content.hpf";
    rf1.append_attribute("media-type") = "application/hwpml-package+xml";
    auto rf2 = rootfiles.append_child("ocf:rootfile");
    rf2.append_attribute("full-path") = "Preview/PrvText.txt";
    rf2.append_attribute("media-type") = "text/plain";
    return serializeXml(xml);
}

// 문서 암호로 잠근 항목 하나(manifest 의 file-entry).
struct LockedEntry {
    std::string path;
    std::string mediaType;
    std::size_t size = 0;
    std::string checksum;  // base64
};

// 문서 암호가 있으면 한글 2024 처럼 본문 항목을 잠근다. 암호 UTF-8 의 SHA-256 을 PBKDF2-HMAC-SHA1(1024 번)로 늘린
// 32 바이트 열쇠, 항목은 raw deflate 한 뒤 0 으로 16 바이트까지 채워 AES-256-CBC 로 감싸고 압축 없이 담는다.
// 초기 벡터와 소금은 같은 16 바이트이고(한글), 검사합은 원문 앞 1 KiB 의 SHA-256 이다.
struct Locker {
    bool on = false;
    Bytes key;
    Bytes iv;
    std::vector<LockedEntry> entries;

    explicit Locker(const std::string& password) {
        if (password.empty()) return;
        std::random_device rd;
        iv.resize(16);
        for (auto& b : iv) b = static_cast<std::uint8_t>(rd() & 0xFF);
        const auto start = crypto::sha256(
            ByteView(reinterpret_cast<const std::uint8_t*>(password.data()), password.size()));
        key = crypto::pbkdf2Sha1(start, iv, 1024, 32);
        on = true;
    }

    // 잠그는 항목인가. 꾸러미 정보(mimetype·version.xml·META-INF·content.hpf)는 한글도 그대로 둔다.
    static bool locks(std::string_view path) {
        return path != "mimetype" && path != "version.xml" && path.rfind("META-INF/", 0) != 0 &&
               path != "Contents/content.hpf";
    }

    bool add(zip::Writer& zw, const std::string& path, ByteView data, bool compress, const std::string& mediaType,
             std::string* error) {
        if (!on || !locks(path)) return zw.addFile(path, data, compress, error);
        Bytes packed;
        if (!zip::deflateRaw(data, packed)) {
            if (error) *error = "failed to compress " + path;
            return false;
        }
        packed.resize((packed.size() + 15) / 16 * 16, 0);
        const crypto::Aes aes(key);
        const Bytes sealed = crypto::aesCbcEncrypt(aes, iv, packed);
        const auto sum = crypto::sha256(data.subspan(0, std::min<std::size_t>(data.size(), 1024)));
        entries.push_back(LockedEntry{path, mediaType, data.size(), crypto::base64Encode(sum)});
        return zw.addFile(path, sealed, false, error);
    }

    bool addText(zip::Writer& zw, const std::string& path, std::string_view text, const std::string& mediaType,
                 std::string* error) {
        return add(zw, path, ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()), true,
                   mediaType, error);
    }
};

std::string writeManifestXml(const Locker& locker) {
    pugi::xml_document xml;
    auto manifest = xml.append_child("odf:manifest");
    manifest.append_attribute("xmlns:odf") = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0";
    const std::string iv = crypto::base64Encode(locker.iv);
    for (const LockedEntry& e : locker.entries) {
        auto entry = manifest.append_child("odf:file-entry");
        entry.append_attribute("full-path") = e.path.c_str();
        entry.append_attribute("media-type") = e.mediaType.c_str();
        entry.append_attribute("size") = static_cast<unsigned long long>(e.size);
        auto data = entry.append_child("odf:encryption-data");
        data.append_attribute("checksum-type") = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0#sha256-1k";
        data.append_attribute("checksum") = e.checksum.c_str();
        auto alg = data.append_child("odf:algorithm");
        alg.append_attribute("algorithm-name") = "http://www.w3.org/2001/04/xmlenc#aes256-cbc";
        alg.append_attribute("initialisation-vector") = iv.c_str();
        auto kd = data.append_child("odf:key-derivation");
        kd.append_attribute("key-derivation-name") = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0#pbkdf2";
        kd.append_attribute("key-size") = 32;
        kd.append_attribute("iteration-count") = 1024;
        kd.append_attribute("salt") = iv.c_str();
        auto sk = data.append_child("odf:start-key-generation");
        sk.append_attribute("start-key-generation-name") = "http://www.w3.org/2000/09/xmldsig#sha256";
        sk.append_attribute("key-size") = 32;
    }
    return serializeXml(xml);
}

std::string buildPreviewText(const Document& doc) {
    if (!doc.previewText.empty()) return doc.previewText;
    std::string out;
    for (const auto& sec : doc.sections) {
        for (const auto& p : sec.paragraphs) {
            if (!out.empty()) out += "\r\n";
            out += p.text();
        }
    }
    return out;
}

}  // namespace

bool write(const model::Document& doc, Bytes& out, const WriteOptions& options, std::string* error) {
    gFieldIds = FieldIds{};
    // 메모는 문서 차례로 1 부터 번호를 매기고, 필드 고유 id 는 있던 것을 쓰고 없으면 새로 매긴다(답글 관계가 쓴다).
    const std::vector<const Control*> memos = collectMemos(doc);
    std::uint32_t maxId = 0;
    for (const Control* m : memos) maxId = std::max(maxId, m->fieldId);
    std::uint32_t nextMemoId = std::max<std::uint32_t>(maxId + 1, 0x40000000u);
    for (std::size_t i = 0; i < memos.size(); ++i) {
        gFieldIds.memoNumber[memos[i]] = static_cast<std::uint32_t>(i + 1);
        gFieldIds.memoId[memos[i]] = memos[i]->fieldId != 0 ? memos[i]->fieldId : nextMemoId++;
    }
    gFieldIds.next = std::max<std::uint32_t>(nextMemoId, 1);
    out.clear();
    if (doc.format.distribution) {
        // 배포용 문서는 HWP 로만 배포용을 지킬 수 있다. HWPX 로 쓰면 복사·인쇄 막기와 잠금이 풀린다.
        if (error) *error = "배포용 문서는 HWP 로만 저장할 수 있습니다.";
        return false;
    }
    zip::Writer zw;
    Locker locker(doc.format.password);
    gKeptParts.clear();

    // 1. mimetype 은 ZIP 첫 번째 엔트리로 비압축(STORED) 저장
    if (!zw.addText("mimetype", "application/hwp+zip", false, error)) return false;

    const BinRegistry bins = buildBinRegistry(doc, options.sourceData);

    // 2. version.xml, settings.xml, META-INF/*
    if (!zw.addText("version.xml", writeVersionXml(), true, error)) return false;
    if (!locker.addText(zw, "settings.xml", writeSettingsXml(), "text/xml", error)) return false;
    if (!zw.addText("META-INF/container.xml", writeContainerXml(), true, error)) return false;

    // 3. Contents/header.xml
    if (!locker.addText(zw, "Contents/header.xml", writeHeaderXml(doc, bins), "application/xml", error)) return false;

    // 4. Contents/sectionN.xml 및 masterpageN.xml
    std::vector<Section> fallbackSections;
    const std::vector<Section>* sectionsPtr = &doc.sections;
    if (sectionsPtr->empty()) {
        fallbackSections.emplace_back();
        sectionsPtr = &fallbackSections;
    }

    std::size_t masterPageIdx = 0;
    for (std::size_t i = 0; i < sectionsPtr->size(); ++i) {
        const auto& sec = (*sectionsPtr)[i];
        const std::string secPath = "Contents/section" + std::to_string(i) + ".xml";
        if (!locker.addText(zw, secPath, writeSectionXml(sec, bins, masterPageIdx), "application/xml", error)) {
            return false;
        }
        for (const auto& mp : sec.masterPages) {
            const std::string mpPath =
                "Contents/masterpage" + std::to_string(masterPageIdx) + ".xml";
            if (!locker.addText(zw, mpPath, writeMasterPageXml(mp, masterPageIdx, bins), "application/xml", error)) {
                return false;
            }
            ++masterPageIdx;
        }
    }

    // 5. BinData/*
    for (const auto& b : bins.entries) {
        if (!b.data.empty()) {
            if (!locker.add(zw, b.zipPath, b.data, true, b.mediaType, error)) return false;
        }
    }

    // 답글 관계(Contents/memoExtended.xml). 답글이 없으면 만들지 않는다.
    bool replies = false;
    {
        pugi::xml_document extended;
        auto root = extended.append_child("he:memosEx");
        root.append_attribute("xmlns:he") = "http://www.hancom.co.kr/hwpml/2021/extended";
        for (const Control* m : memos) {
            if (m->memoParentId == 0) continue;
            auto e = root.append_child("he:memo");
            e.append_attribute("id") = gFieldIds.memoId[m];
            e.append_attribute("parentId") = m->memoParentId;
            replies = true;
        }
        if (replies && !locker.addText(zw, "Contents/memoExtended.xml", serializeXml(extended), "application/xml", error)) {
            return false;
        }
    }

    // 보관한 개체의 딸린 항목(차트 XML). 한글은 이것을 manifest 에 적지 않고 개체가 경로로 가리킨다.
    for (const auto& [path, bytes] : gKeptParts) {
        if (!locker.add(zw, path, bytes, true, "application/xml", error)) return false;
    }
    gKeptParts.clear();

    // 6. Contents/content.hpf
    if (!zw.addText("Contents/content.hpf",
                    writeContentHpfXml(doc, bins, sectionsPtr->size(), masterPageIdx, replies), true,
                    error)) {
        return false;
    }

    // 7. Preview/PrvText.txt
    if (!locker.addText(zw, "Preview/PrvText.txt", buildPreviewText(doc), "text/xml", error)) return false;

    // 8. META-INF/manifest.xml(잠근 항목의 암호 정보). 잠근 항목을 다 쓴 뒤에 만든다.
    if (!zw.addText("META-INF/manifest.xml", writeManifestXml(locker), true, error)) return false;

    return zw.finalize(out, error);
}

}  // namespace chwp::hwpx

