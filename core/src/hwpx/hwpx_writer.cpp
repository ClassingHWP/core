#include "chwp/hwpx/hwpx_writer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <pugixml.hpp>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "chwp/container/zip.hpp"
#include "chwp/reader.hpp"
#include "chwp/unit.hpp"

namespace chwp::hwpx {

namespace {

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
    if (ctrlId == "%smr") return "SUMMARY";
    if (ctrlId == "%usr") return "USER_INFO";
    return "HYPERLINK";
}

// BinItem 매핑 도우미: HWP("1", "2"...) 및 HWPX("image1"...) 모두 일관된 HWPX manifest id 로 매핑.
struct BinRegistry {
    struct Entry {
        std::string id;       // manifest id (예: "image1")
        std::string zipPath;  // "BinData/image1.png"
        std::string mediaType;
        std::string ext;
        bool embedded = true;
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
        pp.append_attribute("suppressLineNumbers") = 0;
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
    objNode.append_attribute("id") = defaultId;
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
    secPr.append_attribute("memoShapeIDRef") = 0;
    secPr.append_attribute("textVerticalWidthHead") = 0;
    secPr.append_attribute("masterPageCnt") =
        static_cast<unsigned>(secPtr ? secPtr->masterPages.size() : 0);

    auto grid = secPr.append_child("hp:grid");
    grid.append_attribute("lineGrid") = lineGrid;
    grid.append_attribute("charGrid") = charGrid;
    grid.append_attribute("wonggojiFormat") = wongoji ? 1 : 0;

    auto startNum = secPr.append_child("hp:startNum");
    startNum.append_attribute("pageStartsOn") = "BOTH";
    startNum.append_attribute("page") = pageStart;
    startNum.append_attribute("pic") = 0;
    startNum.append_attribute("tbl") = 0;
    startNum.append_attribute("equation") = 0;

    auto vis = secPr.append_child("hp:visibility");
    vis.append_attribute("hideFirstHeader") = hideFirstHeader ? 1 : 0;
    vis.append_attribute("hideFirstFooter") = hideFirstFooter ? 1 : 0;
    vis.append_attribute("hideFirstMasterPage") = hideFirstMaster ? 1 : 0;
    vis.append_attribute("border") = "SHOW_ALL";
    vis.append_attribute("fill") = "SHOW_ALL";
    vis.append_attribute("hideFirstPageNum") = hideFirstPageNum ? 1 : 0;
    vis.append_attribute("hideFirstEmptyLine") = 0;
    vis.append_attribute("showLineNumber") = 0;

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
            tc.append_attribute("name") = "";
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

void writeObjectNode(pugi::xml_node parent, const Control& ctrl, const BinRegistry& bins, int groupLevel) {
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
    std::string tag = "hp:rect";
    if (ctrl.kind == ControlKind::Container || ctrl.shapeType == "container") tag = "hp:container";
    else if (ctrl.kind == ControlKind::Ole || ctrl.shapeType == "ole") tag = "hp:ole";
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
            for (int i = 0; i < 7; ++i) {
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
            node.append_attribute("hideBorder") = 0;
            node.append_attribute("hideFill") = 0;
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
            fb.append_attribute("id") = 0;
            fb.append_attribute("type") = toHwpxFieldType(ctrl.ctrlId);
            fb.append_attribute("name") = ctrl.name.c_str();
            auto params = fb.append_child("hp:parameters");
            auto sp = params.append_child("hp:stringParam");
            sp.append_attribute("name") = "Command";
            sp.text().set(ctrl.command.c_str());
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
            dm.append_attribute("posType") = "TOP";
            dm.append_attribute("szRatio") = 50;
            dm.append_attribute("align") = "CENTER";
            dm.append_child("hp:mainText").text().set(ctrl.text.c_str());
            dm.append_child("hp:subText").text().set(ctrl.subText.c_str());
            break;
        }
        case ControlKind::Compose: {
            auto cmp = runNode.append_child("hp:compose");
            cmp.append_attribute("composeText") = ctrl.text.c_str();
            break;
        }
        case ControlKind::Unknown:
            break;
    }
}

void writeTextWithSpecials(pugi::xml_node runNode, std::string_view text) {
    if (text.empty()) return;
    auto tNode = runNode.append_child("hp:t");
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
}

void writeParagraphNode(pugi::xml_node parent, const Paragraph& para, const BinRegistry& bins,
                        const Section* firstParaSec, std::size_t masterPageBaseIdx) {
    auto pNode = parent.append_child("hp:p");
    pNode.append_attribute("id") = 2147483648u;
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
        for (const auto& r : para.runs) {
            auto runNode = pNode.append_child("hp:run");
            runNode.append_attribute("charPrIDRef") = r.charShapeId;
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
                if (item.type == InlineItem::Type::Text) {
                    writeTextWithSpecials(runNode, item.text);
                } else {
                    writeControlInRun(runNode, item.control, bins, firstParaSec, masterPageBaseIdx);
                    if (item.control.kind == ControlKind::SectionDef) {
                        secPrWritten = true;
                    }
                }
            }
            if (!activeMarkpen.empty()) {
                runNode.append_child("hp:markpenEnd");
            }
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
    root.append_attribute("pageNumber") = 0;
    root.append_attribute("pageDuplicate") = 0;
    root.append_attribute("pageFront") = mp.printOnTop ? 1 : 0;

    writeSubListNode(root, mp.content, bins);
    return serializeXml(xml);
}

std::string writeContentHpfXml(const Document& doc, const BinRegistry& bins,
                               std::size_t sectionCount, std::size_t masterPageCount) {
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
        addItem(b.id, b.zipPath, b.mediaType, b.embedded);
    }
    for (std::size_t i = 0; i < sectionCount; ++i) {
        const std::string secId = "section" + std::to_string(i);
        addItem(secId, "Contents/" + secId + ".xml", "application/xml");
    }
    addItem("settings", "settings.xml", "application/xml");

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

std::string writeManifestXml() {
    pugi::xml_document xml;
    auto manifest = xml.append_child("odf:manifest");
    manifest.append_attribute("xmlns:odf") = "urn:oasis:names:tc:opendocument:xmlns:manifest:1.0";
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
    out.clear();
    zip::Writer zw;

    // 1. mimetype 은 ZIP 첫 번째 엔트리로 비압축(STORED) 저장
    if (!zw.addText("mimetype", "application/hwp+zip", false, error)) return false;

    const BinRegistry bins = buildBinRegistry(doc, options.sourceData);

    // 2. version.xml, settings.xml, META-INF/*
    if (!zw.addText("version.xml", writeVersionXml(), true, error)) return false;
    if (!zw.addText("settings.xml", writeSettingsXml(), true, error)) return false;
    if (!zw.addText("META-INF/container.xml", writeContainerXml(), true, error)) return false;
    if (!zw.addText("META-INF/manifest.xml", writeManifestXml(), true, error)) return false;

    // 3. Contents/header.xml
    if (!zw.addText("Contents/header.xml", writeHeaderXml(doc, bins), true, error)) return false;

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
        if (!zw.addText(secPath, writeSectionXml(sec, bins, masterPageIdx), true, error)) {
            return false;
        }
        for (const auto& mp : sec.masterPages) {
            const std::string mpPath =
                "Contents/masterpage" + std::to_string(masterPageIdx) + ".xml";
            if (!zw.addText(mpPath, writeMasterPageXml(mp, masterPageIdx, bins), true, error)) {
                return false;
            }
            ++masterPageIdx;
        }
    }

    // 5. BinData/*
    for (const auto& b : bins.entries) {
        if (!b.data.empty()) {
            if (!zw.addFile(b.zipPath, b.data, true, error)) return false;
        }
    }

    // 6. Contents/content.hpf
    if (!zw.addText("Contents/content.hpf",
                    writeContentHpfXml(doc, bins, sectionsPtr->size(), masterPageIdx), true,
                    error)) {
        return false;
    }

    // 7. Preview/PrvText.txt
    if (!zw.addText("Preview/PrvText.txt", buildPreviewText(doc), true, error)) return false;

    return zw.finalize(out, error);
}

}  // namespace chwp::hwpx

