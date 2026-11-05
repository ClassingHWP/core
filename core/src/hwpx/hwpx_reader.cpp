#include "chwp/hwpx/hwpx_reader.hpp"

#include <algorithm>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/unit.hpp"
#include "chwp/container/zip.hpp"
#include "pugixml.hpp"

namespace chwp::hwpx {

namespace {

using namespace chwp::model;

// 접두사(hp:, hh: ...)는 관례일 뿐이므로 로컬 이름으로 비교한다.
std::string_view localName(const char* name) {
    std::string_view s(name);
    const auto colon = s.find(':');
    return colon == std::string_view::npos ? s : s.substr(colon + 1);
}

bool is(const pugi::xml_node& node, std::string_view name) {
    return node.type() == pugi::node_element && localName(node.name()) == name;
}

bool nearHu(std::uint32_t a, std::uint32_t b) {
    const std::uint32_t delta = a > b ? a - b : b - a;
    return delta <= 2;
}

// curSz 가 0이면 지금 크기에 전체 배율을 곱한다.
// curSz 가 앞쪽 배율까지만 반영돼 있으면 원본×전체 배율을 쓴다. 어느 배율과도 다르면 적힌 크기를 유지한다.
void applyScale(std::uint32_t& size, std::uint32_t org, bool current, const std::vector<double>& prefixes) {
    if (prefixes.empty() || size == 0) return;
    const double scale = prefixes.back();
    if (!(std::fabs(scale - 1.0) > 1e-9)) return;
    if (!current) {
        size = static_cast<std::uint32_t>(std::llround(static_cast<double>(size) * scale));
        return;
    }
    if (org == 0) return;
    const auto full = static_cast<std::uint32_t>(std::llround(static_cast<double>(org) * scale));
    if (nearHu(size, full)) return;
    for (std::size_t i = 0; i + 1 < prefixes.size(); ++i) {
        const auto partial = static_cast<std::uint32_t>(std::llround(static_cast<double>(org) * prefixes[i]));
        if (!nearHu(size, partial)) continue;
        size = full;
        return;
    }
}

pugi::xml_node child(const pugi::xml_node& node, std::string_view name) {
    for (auto c : node.children()) {
        if (is(c, name)) return c;
    }
    return {};
}

// 전위 순회로 처음 나오는 요소. hp:switch 에서는 hp:case 가 hp:default 보다 먼저 선택된다.
pugi::xml_node descendant(const pugi::xml_node& node, std::string_view name) {
    for (auto c : node.children()) {
        if (c.type() != pugi::node_element) continue;
        if (is(c, name)) return c;
        if (auto found = descendant(c, name)) return found;
    }
    return {};
}

std::uint32_t u32(const pugi::xml_node& node, const char* attr, std::uint32_t def = 0) {
    return node.attribute(attr).as_uint(def);
}

// 음수가 부호 없는 값(4294967295 등)으로 기록된 경우도 있다.
std::int32_t i32(const pugi::xml_node& node, const char* attr, std::int32_t def = 0) {
    const auto a = node.attribute(attr);
    if (!a) return def;
    return static_cast<std::int32_t>(a.as_llong(def));
}

bool flag(const pugi::xml_node& node, const char* attr) {
    const std::string_view v = node.attribute(attr).as_string();
    return v == "1" || v == "true";
}

std::string str(const pugi::xml_node& node, const char* attr) {
    return node.attribute(attr).as_string();
}

int pictureEffect(const std::string& name) {
    std::string key;
    for (unsigned char c : name) {
        if (c == '_' || c == '-' || c == ' ') continue;
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        key.push_back(static_cast<char>(c));
    }
    if (key == "grayscale" || key == "gray") return 1;
    if (key == "blackwhite") return 2;
    if (key == "pattern8x8") return 4;
    return 0;
}

std::int32_t lengthHu(const std::string& text) {
    if (text.find("mm") != std::string::npos) return mmToHu(std::strtod(text.c_str(), nullptr));
    return static_cast<std::int32_t>(std::strtoll(text.c_str(), nullptr, 10));
}

std::int32_t marginHu(const pugi::xml_node& node) {
    if (!node) return 0;
    const std::string unit = str(node, "unit");
    const std::string text = str(node, "value");
    if (unit == "mm" || text.find("mm") != std::string::npos) return mmToHu(std::strtod(text.c_str(), nullptr));
    return i32(node, "value");
}

// HWPX 선 종류 번호는 2가 DOT, 3이 DASH다.
// 그리는 번호는 2가 파선, 3이 점선이다. 파일의 그 두 문자열만 그린 이름과 맞바꾼다.
// DASH_DOT처럼 더 긴 이름은 번호가 양쪽에서 같다. .hwp 바이트는 이미 그리는 번호다.
std::string hwpxLineType(const std::string& type) {
    std::string key;
    key.reserve(type.size());
    for (unsigned char c : type) {
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
        key.push_back(static_cast<char>(c));
    }
    if (key == "DOT") return "DASH";
    if (key == "DASH") return "DOT";
    return type;
}

std::string upperToken(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

std::string canonicalDrawLine(std::string name) {
    name = upperToken(std::move(name));
    if (name == "DASHDOT") return "DASH_DOT";
    if (name == "DASHDOTDOT") return "DASH_DOT_DOT";
    if (name == "LONGDASH") return "LONG_DASH";
    if (name == "DOUBLESLIM") return "DOUBLE_SLIM";
    if (name == "SLIMTHICK") return "SLIM_THICK";
    if (name == "THICKSLIM") return "THICK_SLIM";
    if (name == "SLIMTHICKSLIM") return "SLIM_THICK_SLIM";
    if (name == "DOUBLEWAVE") return "DOUBLE_WAVE";
    if (name == "THICK3D") return "THICK_3D";
    if (name == "THICKREV3D" || name == "THICK3DREVERSELIGHTING") return "THICK_3D_REVERSE_LIGHTING";
    if (name == "3DREVERSELIGHTING") return "3D_REVERSE_LIGHTING";
    return name;
}

std::uint8_t storedLineShape(const std::string& drawName) {
    static const char* kinds[] = {"NONE",     "SOLID",     "DASH",
                                  "DOT",      "DASH_DOT",  "DASH_DOT_DOT",
                                  "LONG_DASH","CIRCLE",    "DOUBLE_SLIM",
                                  "SLIM_THICK","THICK_SLIM","SLIM_THICK_SLIM",
                                  "WAVE",     "DOUBLE_WAVE","THICK_3D",
                                  "THICK_3D_REVERSE_LIGHTING", "3D", "3D_REVERSE_LIGHTING"};
    for (unsigned i = 1; i < std::size(kinds); ++i) {
        if (drawName == kinds[i]) return static_cast<std::uint8_t>(i - 1);
    }
    return 0;
}

std::uint8_t outlineCode(const std::string& type) {
    const std::string key = upperToken(type);
    if (key == "SOLID") return 1;
    if (key == "DOT") return 2;
    if (key == "THICK") return 3;
    if (key == "DASH") return 4;
    if (key == "DASH_DOT" || key == "DASHDOT") return 5;
    if (key == "DASH_DOT_DOT" || key == "DASHDOTDOT") return 6;
    return 0;
}

std::uint8_t shadowCode(const std::string& type) {
    const std::string key = upperToken(type);
    if (key == "CONTINUOUS" || key == "CONT") return 2;
    if (key == "DROP" || key == "DISCONT" || key == "DISCONTINUOUS") return 1;
    return 0;
}

std::uint8_t emphasisCode(const std::string& type) {
    const std::string key = upperToken(type);
    if (key.empty() || key == "NONE") return 0;
    static const char* names[] = {"NONE", "DOT", "CIRCLE", "CARON", "TILDE", "COMMA", "COLON"};
    for (unsigned i = 1; i < std::size(names); ++i) {
        if (key == names[i]) return static_cast<std::uint8_t>(i);
    }
    bool digits = true;
    int number = 0;
    for (unsigned char ch : key) {
        if (ch < '0' || ch > '9') {
            digits = false;
            break;
        }
        number = number * 10 + (ch - '0');
    }
    if (digits && number > 0 && number < 16) return static_cast<std::uint8_t>(number);
    return 0;
}

BorderSide readSide(const pugi::xml_node& node) {
    BorderSide side;
    if (!node) return side;
    side.type = hwpxLineType(str(node, "type"));
    side.color = str(node, "color");
    side.width = lengthHu(str(node, "width"));
    return side;
}

int imageModeOf(const std::string& mode) {
    if (mode == "CENTER") return 6;
    if (mode == "TILE" || mode.find("TILE") == 0) return 0;
    return 5;
}

void readFill(const pugi::xml_node& brush, std::string& face, std::string& grad, std::int32_t& angle,
              std::string& imageId, int& imageMode) {
    if (auto win = child(brush, "winBrush")) face = str(win, "faceColor");
    if (auto gradNode = child(brush, "gradation")) {
        angle = i32(gradNode, "angle");
        int index = 0;
        for (auto color : gradNode.children()) {
            if (!is(color, "color")) continue;
            if (index == 0) face = str(color, "value");
            if (index == 1) grad = str(color, "value");
            ++index;
        }
    }
    if (auto img = child(brush, "imgBrush")) {
        imageMode = imageModeOf(str(img, "mode"));
        if (auto image = child(img, "img")) imageId = str(image, "binaryItemIDRef");
    }
}

DiagonalSpec readDiagonalSpec(const pugi::xml_node& node) {
    DiagonalSpec spec;
    if (!node) return spec;
    const std::string type = str(node, "type");
    if (!type.empty()) spec.type = type;
    spec.crooked = flag(node, "Crooked") || flag(node, "isCrooked") || flag(node, "crooked");
    spec.counter = flag(node, "isCounter") || flag(node, "counter");
    return spec;
}

BorderFill readBorderFill(const pugi::xml_node& node) {
    BorderFill fill;
    fill.id = u32(node, "id");
    if (const std::string center = str(node, "centerLine"); !center.empty()) fill.centerLine = center;
    fill.breakCellSeparateLine = flag(node, "breakCellSeparateLine");
    fill.slash = readDiagonalSpec(child(node, "slash"));
    fill.backSlash = readDiagonalSpec(child(node, "backSlash"));
    fill.left = readSide(child(node, "leftBorder"));
    fill.right = readSide(child(node, "rightBorder"));
    fill.top = readSide(child(node, "topBorder"));
    fill.bottom = readSide(child(node, "bottomBorder"));
    if (auto diag = child(node, "diagonal")) {
        fill.diagonal = readSide(diag);
    } else if (fill.slash.type != "NONE" || fill.backSlash.type != "NONE") {
        const auto sNode = child(node, "slash");
        const auto bNode = child(node, "backSlash");
        const auto src = (sNode && (sNode.attribute("width") || sNode.attribute("color"))) ? sNode : bNode;
        if (src && (src.attribute("width") || src.attribute("color"))) {
            fill.diagonal.type = "SOLID";
            fill.diagonal.color = str(src, "color");
            if (fill.diagonal.color.empty()) fill.diagonal.color = "#000000";
            fill.diagonal.width = lengthHu(str(src, "width"));
            if (fill.diagonal.width <= 0) fill.diagonal.width = 28;
        }
    }
    if (auto brush = child(node, "fillBrush")) {
        readFill(brush, fill.faceColor, fill.gradColor, fill.gradAngle, fill.imageId, fill.imageMode);
        if (auto win = child(brush, "winBrush")) {
            const std::string hatch = str(win, "hatchStyle");
            if (!hatch.empty() && hatch != "NONE") {
                fill.hatchStyle = hatch;
                fill.hatchColor = str(win, "hatchColor");
            }
        }
    }
    return fill;
}

Color parseColor(std::string_view s) {
    if (s.empty() || s == "none" || s[0] != '#') return std::nullopt;
    s.remove_prefix(1);
    if (s.size() > 6) s.remove_prefix(s.size() - 6);
    return static_cast<std::uint32_t>(std::strtoul(std::string(s).c_str(), nullptr, 16));
}

std::string collectText(const pugi::xml_node& node);

ParaHead readParaHead(const pugi::xml_node& node) {
    ParaHead head;
    head.level = i32(node, "level", 1);
    head.start = i32(node, "start", 1);
    head.align = str(node, "align");
    if (head.align.empty()) head.align = "LEFT";
    head.numFormat = str(node, "numFormat");
    if (head.numFormat.empty()) head.numFormat = "DIGIT";
    head.format = collectText(node);
    head.charShapeId = u32(node, "charPrIDRef");
    head.useInstWidth = flag(node, "useInstWidth");
    head.autoIndent = flag(node, "autoIndent");
    head.widthAdjust = i32(node, "widthAdjust");
    head.textOffset = i32(node, "textOffset");
    head.textOffsetType = str(node, "textOffsetType");
    if (head.textOffsetType.empty()) head.textOffsetType = "PERCENT";
    return head;
}

std::string collectText(const pugi::xml_node& node) {
    std::string out;
    for (auto c : node.children()) {
        if (c.type() == pugi::node_pcdata || c.type() == pugi::node_cdata) out += c.value();
    }
    return out;
}

template <typename T>
void readPerLang(const pugi::xml_node& node, PerLang<T>& out) {
    static const char* names[] = {"hangul", "latin", "hanja", "japanese", "other", "symbol", "user"};
    for (std::size_t i = 0; i < kLangCount; ++i) {
        out[i] = static_cast<T>(node.attribute(names[i]).as_llong(0));
    }
}

int langIndex(std::string_view lang) {
    static const char* names[] = {"HANGUL", "LATIN", "HANJA", "JAPANESE", "OTHER", "SYMBOL", "USER"};
    for (int i = 0; i < static_cast<int>(kLangCount); ++i) {
        if (lang == names[i]) return i;
    }
    return -1;
}

bool isObjectElement(std::string_view name) {
    static const std::string_view names[] = {
        "tbl", "pic", "equation", "rect", "ellipse", "line", "arc", "polygon", "curve",
        "connectLine", "container", "ole", "textart", "chart", "video"};
    for (auto n : names) {
        if (n == name) return true;
    }
    return false;
}

std::string fieldCtrlId(std::string_view type) {
    static const std::pair<std::string_view, const char*> map[] = {
        {"HYPERLINK", "%hlk"}, {"CLICK_HERE", "%clk"}, {"BOOKMARK", "%bmk"},
        {"FORMULA", "%fmu"},   {"DATE", "%dte"},       {"DOC_DATE", "%ddt"},
        {"PATH", "%pat"},      {"CROSSREF", "%xrf"},   {"MAILMERGE", "%mmg"},
        {"MEMO", "%%me"},      {"SUMMARY", "%smr"},    {"USER_INFO", "%usr"}};
    for (const auto& [k, v] : map) {
        if (k == type) return v;
    }
    return "%unk";
}

std::string extensionOf(std::string_view path) {
    const auto dot = path.rfind('.');
    const auto slash = path.rfind('/');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) return {};
    return std::string(path.substr(dot + 1));
}

class Parser {
public:
    Parser(ByteView data, const ParseOptions& options, ParseResult& result)
        : data_(data), options_(options), result_(result), doc_(result.document) {}

    bool run();

private:
    struct ManifestItem {
        std::string href;
        std::string mediaType;
        bool embedded = false;
    };

    void warn(std::string message) { result_.warnings.push_back(std::move(message)); }
    bool fail(std::string message) {
        result_.error = std::move(message);
        return false;
    }

    bool loadXml(const std::string& path, pugi::xml_document& xml);
    void parseVersion();
    std::string findRootFile();
    bool isEncrypted();
    void parsePackage(const std::string& hpfPath, std::string& headerPath,
                      std::vector<std::string>& sectionPaths);
    void parseHeader(const pugi::xml_node& head);
    void parseCharShape(const pugi::xml_node& node);
    void parseParaShape(const pugi::xml_node& node);
    void parseStyle(const pugi::xml_node& node);

    void parseSection(const pugi::xml_node& sec, Section& section);
    void parseMasterPage(const std::string& id, Section& section);
    Paragraph parseParagraph(const pugi::xml_node& p);
    void parseRun(const pugi::xml_node& run, Run& out);
    SubList parseSubList(const pugi::xml_node& subList);
    Control parseSecPr(const pugi::xml_node& node);
    bool parseCtrl(const pugi::xml_node& node, Control& out);
    Control parseObject(const pugi::xml_node& node);
    static Placement readPlacement(const pugi::xml_node& node);

    void parsePreviewText();

    ByteView data_;
    const ParseOptions& options_;
    ParseResult& result_;
    Document& doc_;
    zip::Archive zip_;
    std::unordered_map<std::string, std::string> hrefById_;
};

bool Parser::run() {
    doc_.format.format = SourceFormat::Hwpx;

    std::string error;
    if (!zip_.open(data_, &error)) return fail(error);

    Bytes mimetype;
    if (zip_.read("mimetype", mimetype)) {
        const std::string_view m(reinterpret_cast<const char*>(mimetype.data()), mimetype.size());
        if (m.find("application/hwp+zip") == std::string_view::npos) {
            warn("unexpected mimetype: " + std::string(m));
        }
    } else {
        warn("missing mimetype entry");
    }

    parseVersion();
    if (isEncrypted()) {
        doc_.format.encrypted = true;
        return fail("password-protected HWPX documents are not supported");
    }

    std::string headerPath = "Contents/header.xml";
    std::vector<std::string> sectionPaths;
    parsePackage(findRootFile(), headerPath, sectionPaths);

    pugi::xml_document header;
    if (!loadXml(headerPath, header)) return fail("failed to read " + headerPath);
    parseHeader(header.document_element());

    for (const auto& path : sectionPaths) {
        pugi::xml_document xml;
        if (!loadXml(path, xml)) {
            warn("failed to read " + path);
            continue;
        }
        Section section;
        parseSection(xml.document_element(), section);
        doc_.sections.push_back(std::move(section));
    }
    if (doc_.header.sectionCount != doc_.sections.size()) {
        warn("section count mismatch: header=" + std::to_string(doc_.header.sectionCount) +
             ", files=" + std::to_string(doc_.sections.size()));
    }

    parsePreviewText();
    result_.ok = true;
    return true;
}

bool Parser::loadXml(const std::string& path, pugi::xml_document& xml) {
    Bytes bytes;
    if (!zip_.read(path, bytes)) return false;
    // 공백만 있는 텍스트(<hp:t>  </hp:t>)도 본문이므로 보존한다.
    const auto res = xml.load_buffer(bytes.data(), bytes.size(),
                                     pugi::parse_default | pugi::parse_ws_pcdata,
                                     pugi::encoding_utf8);
    if (!res) {
        warn(path + ": XML parse error: " + res.description());
        return false;
    }
    return true;
}

void Parser::parseVersion() {
    pugi::xml_document xml;
    if (!loadXml("version.xml", xml)) return;
    const auto v = xml.document_element();
    doc_.format.version = str(v, "major") + "." + str(v, "minor") + "." + str(v, "micro") + "." +
                          str(v, "buildNumber");
    doc_.format.application = str(v, "application");
    const std::string appVersion = str(v, "appVersion");
    if (!appVersion.empty()) doc_.format.application += " " + appVersion;
}

std::string Parser::findRootFile() {
    pugi::xml_document xml;
    if (loadXml("META-INF/container.xml", xml)) {
        if (auto rootfiles = child(xml.document_element(), "rootfiles")) {
            for (auto rf : rootfiles.children()) {
                if (is(rf, "rootfile") && str(rf, "media-type") == "application/hwpml-package+xml") {
                    return str(rf, "full-path");
                }
            }
        }
    }
    return "Contents/content.hpf";
}

bool Parser::isEncrypted() {
    if (!zip_.contains("META-INF/manifest.xml")) return false;
    pugi::xml_document xml;
    if (!loadXml("META-INF/manifest.xml", xml)) return false;
    return static_cast<bool>(descendant(xml, "encryption-data"));
}

void Parser::parsePackage(const std::string& hpfPath, std::string& headerPath,
                          std::vector<std::string>& sectionPaths) {
    pugi::xml_document xml;
    if (!loadXml(hpfPath, xml)) {
        warn("missing package file: " + hpfPath);
    }
    const auto package = xml.document_element();

    if (auto meta = child(package, "metadata")) {
        Metadata& m = doc_.metadata;
        for (auto n : meta.children()) {
            if (is(n, "title")) m.title = collectText(n);
            else if (is(n, "language")) m.language = collectText(n);
            else if (is(n, "meta")) {
                const std::string name = str(n, "name");
                const std::string value = collectText(n);
                if (name == "creator") m.author = value;
                else if (name == "subject") m.subject = value;
                else if (name == "description") m.comments = value;
                else if (name == "lastsaveby") m.lastSavedBy = value;
                else if (name == "CreatedDate") m.createdAt = value;
                else if (name == "ModifiedDate") m.modifiedAt = value;
                else if (name == "keyword") m.keywords = value;
            }
        }
    }

    std::unordered_map<std::string, ManifestItem> manifest;
    std::vector<std::string> manifestOrder;
    if (auto items = child(package, "manifest")) {
        for (auto item : items.children()) {
            if (!is(item, "item")) continue;
            ManifestItem mi;
            mi.href = str(item, "href");
            mi.mediaType = str(item, "media-type");
            mi.embedded = flag(item, "isEmbeded");
            const std::string id = str(item, "id");
            manifestOrder.push_back(id);
            manifest[id] = std::move(mi);
        }
    }

    // 경로는 패키지 루트 기준이지만, 없으면 content.hpf 위치 기준으로도 찾아본다.
    const std::string hpfDir = hpfPath.substr(0, hpfPath.rfind('/') + 1);
    auto resolve = [&](const std::string& href) {
        if (zip_.contains(href) || hpfDir.empty()) return href;
        return zip_.contains(hpfDir + href) ? hpfDir + href : href;
    };

    for (const auto& id : manifestOrder) {
        const ManifestItem& mi = manifest[id];
        hrefById_[id] = resolve(mi.href);
        if (id == "header") headerPath = resolve(mi.href);
        if (mi.href.rfind("BinData/", 0) == 0) {
            BinItem bin;
            bin.id = id;
            bin.path = resolve(mi.href);
            bin.extension = extensionOf(mi.href);
            bin.mediaType = mi.mediaType;
            bin.storage = mi.embedded ? "EMBEDDING" : "LINK";
            for (const auto& e : zip_.entries()) {
                if (e.name == bin.path) bin.size = e.size;
            }
            if (options_.loadBinData && mi.embedded) {
                if (!zip_.read(bin.path, bin.data)) warn("failed to read bin data: " + bin.path);
            }
            doc_.header.binItems.push_back(std::move(bin));
        }
    }

    if (auto spine = child(package, "spine")) {
        for (auto ref : spine.children()) {
            if (!is(ref, "itemref")) continue;
            const std::string idref = str(ref, "idref");
            auto it = manifest.find(idref);
            if (it == manifest.end() || idref == "header") continue;
            if (extensionOf(it->second.href) == "xml") sectionPaths.push_back(resolve(it->second.href));
        }
    }
    if (sectionPaths.empty()) {
        for (int i = 0;; ++i) {
            const std::string path = "Contents/section" + std::to_string(i) + ".xml";
            if (!zip_.contains(path)) break;
            sectionPaths.push_back(path);
        }
    }
}

// ---------------------------------------------------------------------------
// header.xml
// ---------------------------------------------------------------------------

void Parser::parseHeader(const pugi::xml_node& head) {
    DocHeader& h = doc_.header;
    h.sectionCount = u32(head, "secCnt");
    if (auto b = child(head, "beginNum")) {
        h.beginNumbers.page = u32(b, "page", 1);
        h.beginNumbers.footnote = u32(b, "footnote", 1);
        h.beginNumbers.endnote = u32(b, "endnote", 1);
        h.beginNumbers.picture = u32(b, "pic", 1);
        h.beginNumbers.table = u32(b, "tbl", 1);
        h.beginNumbers.equation = u32(b, "equation", 1);
    }

    const auto refList = child(head, "refList");
    if (auto faces = child(refList, "fontfaces")) {
        for (auto face : faces.children()) {
            if (!is(face, "fontface")) continue;
            const int lang = langIndex(str(face, "lang"));
            if (lang < 0) continue;
            for (auto f : face.children()) {
                if (!is(f, "font")) continue;
                Font font;
                font.id = u32(f, "id");
                font.face = str(f, "face");
                font.type = str(f, "type");
                if (auto subst = child(f, "substFont")) {
                    font.subst = str(subst, "face");
                    if (font.subst.empty()) font.subst = str(subst, "name");
                }
                h.fonts[static_cast<std::size_t>(lang)].push_back(std::move(font));
            }
        }
    }
    if (auto fills = child(refList, "borderFills")) {
        for (auto f : fills.children()) {
            if (!is(f, "borderFill")) continue;
            ++h.borderFillCount;
            h.borderFills.push_back(readBorderFill(f));
        }
    }
    if (auto chars = child(refList, "charProperties")) {
        for (auto n : chars.children()) {
            if (is(n, "charPr")) parseCharShape(n);
        }
    }
    if (auto paras = child(refList, "paraProperties")) {
        for (auto n : paras.children()) {
            if (is(n, "paraPr")) parseParaShape(n);
        }
    }
    if (auto tabs = child(refList, "tabProperties")) {
        for (auto n : tabs.children()) {
            if (!is(n, "tabPr")) continue;
            TabDef def;
            def.id = u32(n, "id");
            def.autoTabLeft = flag(n, "autoTabLeft");
            def.autoTabRight = flag(n, "autoTabRight");
            auto take = [&](const pugi::xml_node& item) {
                TabStop stop;
                stop.pos = i32(item, "pos");
                stop.type = str(item, "type");
                stop.leader = str(item, "leader");
                if (stop.type.empty()) stop.type = "LEFT";
                if (stop.leader.empty()) stop.leader = "NONE";
                def.stops.push_back(stop);
            };
            bool sawSwitch = false;
            for (auto ch : n.children()) {
                if (!is(ch, "switch")) continue;
                sawSwitch = true;
                pugi::xml_node found;
                for (auto branch : ch.children()) {
                    if (is(branch, "tabItem")) {
                        found = branch;
                        break;
                    }
                    for (auto item : branch.children()) {
                        if (!is(item, "tabItem")) continue;
                        found = item;
                        break;
                    }
                    if (found) break;
                }
                if (found) take(found);
            }
            if (!sawSwitch) {
                for (auto item : n.children()) {
                    if (is(item, "tabItem")) take(item);
                }
            }
            h.tabDefs.push_back(std::move(def));
        }
    }
    if (auto styles = child(refList, "styles")) {
        for (auto n : styles.children()) {
            if (is(n, "style")) parseStyle(n);
        }
    }
    if (auto nums = child(refList, "numberings")) {
        for (auto n : nums.children()) {
            if (!is(n, "numbering")) continue;
            Numbering numbering;
            numbering.id = u32(n, "id");
            numbering.start = i32(n, "start");
            for (auto level : n.children()) {
                if (is(level, "paraHead")) numbering.levels.push_back(readParaHead(level));
            }
            h.numberings.push_back(std::move(numbering));
        }
    }
    if (auto bullets = child(refList, "bullets")) {
        for (auto n : bullets.children()) {
            if (!is(n, "bullet")) continue;
            Bullet bullet;
            bullet.id = u32(n, "id");
            bullet.text = str(n, "char");
            bullet.checkedText = str(n, "checkedChar");
            if (auto level = child(n, "paraHead")) bullet.head = readParaHead(level);
            if (flag(n, "useImage")) {
                auto image = child(n, "image");
                if (!image) image = child(n, "img");
                if (image) {
                    bullet.imageId = str(image, "binaryItemIDRef");
                    bullet.bright = i32(image, "bright");
                    bullet.contrast = i32(image, "contrast");
                    bullet.effect = pictureEffect(str(image, "effect"));
                }
            }
            h.bullets.push_back(std::move(bullet));
        }
    }
    auto takeForbidden = [&](const pugi::xml_node& list) {
        for (auto word : list.children()) {
            if (!is(word, "forbidden") && !is(word, "forbiddenWord")) continue;
            std::string text = collectText(word);
            if (text.empty()) text = str(word, "text");
            if (!text.empty()) h.forbiddenWords.push_back(std::move(text));
        }
    };
    if (auto list = child(head, "forbiddenWordList")) takeForbidden(list);
    if (auto summary = child(head, "docSummary")) {
        if (auto list = child(summary, "forbiddenWordList")) takeForbidden(list);
        if (auto list = child(summary, "forbiddenString")) takeForbidden(list);
    }
}

void Parser::parseCharShape(const pugi::xml_node& node) {
    CharShape cs;
    cs.id = u32(node, "id");
    cs.height = i32(node, "height", 1000);
    cs.textColor = parseColor(node.attribute("textColor").as_string());
    cs.shadeColor = parseColor(node.attribute("shadeColor").as_string());
    cs.borderFillId = u32(node, "borderFillIDRef");
    readPerLang(child(node, "fontRef"), cs.fontRef);
    readPerLang(child(node, "ratio"), cs.ratio);
    readPerLang(child(node, "spacing"), cs.spacing);
    readPerLang(child(node, "relSz"), cs.relSize);
    readPerLang(child(node, "offset"), cs.offset);
    cs.bold = static_cast<bool>(child(node, "bold"));
    cs.italic = static_cast<bool>(child(node, "italic"));
    cs.superscript = static_cast<bool>(child(node, "supscript"));
    cs.subscript = static_cast<bool>(child(node, "subscript"));
    cs.useFontSpace = flag(node, "useFontSpace");
    cs.useKerning = flag(node, "useKerning");
    cs.emphasis = emphasisCode(str(node, "symMark"));
    if (auto u = child(node, "underline")) {
        const std::string type = upperToken(str(u, "type"));
        if (!type.empty()) cs.underline = type;
        const std::string shape = canonicalDrawLine(hwpxLineType(str(u, "shape")));
        if (!shape.empty() && shape != "NONE") {
            cs.underlineShape = shape;
            cs.underlineShapeId = storedLineShape(shape);
        }
        if (u.attribute("color")) cs.underlineColor = parseColor(str(u, "color"));
    }
    if (auto s = child(node, "strikeout")) {
        const std::string shape = canonicalDrawLine(hwpxLineType(str(s, "shape")));
        cs.strikeout = !shape.empty() && shape != "NONE";
        if (cs.strikeout) {
            cs.strikeShape = shape;
            cs.strikeShapeId = storedLineShape(shape);
        }
        if (s.attribute("color")) cs.strikeColor = parseColor(str(s, "color"));
    }
    if (auto o = child(node, "outline")) cs.outline = outlineCode(str(o, "type"));
    if (auto sh = child(node, "shadow")) {
        cs.shadow = shadowCode(str(sh, "type"));
        if (sh.attribute("color")) cs.shadowColor = parseColor(str(sh, "color"));
        cs.shadowOffsetX = static_cast<std::int8_t>(std::clamp(i32(sh, "offsetX", 10), -100, 100));
        cs.shadowOffsetY = static_cast<std::int8_t>(std::clamp(i32(sh, "offsetY", 10), -100, 100));
    }
    cs.emboss = static_cast<bool>(child(node, "emboss"));
    cs.engrave = static_cast<bool>(child(node, "engrave"));
    doc_.header.charShapes.push_back(cs);
}

void Parser::parseParaShape(const pugi::xml_node& node) {
    ParaShape ps;
    ps.id = u32(node, "id");
    ps.tabDefId = u32(node, "tabPrIDRef");
    ps.condense = std::min<std::int32_t>(75, static_cast<std::int32_t>(u32(node, "condense")));
    ps.snapToGrid = flag(node, "snapToGrid");
    ps.fontLineHeight = flag(node, "fontLineHeight");
    if (auto a = child(node, "align")) {
        ps.align = str(a, "horizontal");
        if (const std::string vertical = str(a, "vertical"); !vertical.empty()) ps.vertAlign = vertical;
    }
    if (auto hd = child(node, "heading")) {
        ps.headingType = str(hd, "type");
        ps.headingIdRef = u32(hd, "idRef");
        ps.headingLevel = u32(hd, "level");
    }
    ps.checked = flag(node, "checked");
    if (auto b = child(node, "breakSetting")) {
        ps.breakLatinWord = str(b, "breakLatinWord");
        ps.breakNonLatinWord = str(b, "breakNonLatinWord");
        ps.widowOrphan = flag(b, "widowOrphan");
        ps.keepWithNext = flag(b, "keepWithNext");
        ps.keepLines = flag(b, "keepLines");
        ps.pageBreakBefore = flag(b, "pageBreakBefore");
        if (const std::string wrap = str(b, "lineWrap"); !wrap.empty()) ps.lineWrap = wrap;
    }
    if (auto spacing = child(node, "autoSpacing")) {
        ps.autoSpacingEng = flag(spacing, "eAsianEng");
        ps.autoSpacingNum = flag(spacing, "eAsianNum");
    }
    // HwpUnitChar 분기가 실제 여백이다. default 분기는 그 두 배라 쓰지 않는다.
    if (auto m = descendant(node, "margin")) {
        ps.indent = marginHu(child(m, "intent"));
        ps.marginLeft = marginHu(child(m, "left"));
        ps.marginRight = marginHu(child(m, "right"));
        ps.spacingPrev = marginHu(child(m, "prev"));
        ps.spacingNext = marginHu(child(m, "next"));
    }
    if (auto ls = descendant(node, "lineSpacing")) {
        ps.lineSpacingType = str(ls, "type");
        ps.lineSpacing = i32(ls, "value");
    }
    if (auto border = child(node, "border")) {
        ps.borderFillId = u32(border, "borderFillIDRef");
        ps.borderOffsetLeft = i32(border, "offsetLeft");
        ps.borderOffsetRight = i32(border, "offsetRight");
        ps.borderOffsetTop = i32(border, "offsetTop");
        ps.borderOffsetBottom = i32(border, "offsetBottom");
        ps.borderConnect = flag(border, "connect");
        ps.borderIgnoreMargin = flag(border, "ignoreMargin");
    }
    doc_.header.paraShapes.push_back(ps);
}

void Parser::parseStyle(const pugi::xml_node& node) {
    Style st;
    st.id = u32(node, "id");
    st.type = str(node, "type");
    st.name = str(node, "name");
    st.engName = str(node, "engName");
    st.paraShapeId = u32(node, "paraPrIDRef");
    st.charShapeId = u32(node, "charPrIDRef");
    st.nextStyleId = u32(node, "nextStyleIDRef");
    st.langId = i32(node, "langID");
    doc_.header.styles.push_back(std::move(st));
}

// ---------------------------------------------------------------------------
// section*.xml
// ---------------------------------------------------------------------------

void Parser::parseSection(const pugi::xml_node& sec, Section& section) {
    for (auto p : sec.children()) {
        if (is(p, "p")) section.paragraphs.push_back(parseParagraph(p));
    }
    if (!section.paragraphs.empty()) {
        for (const auto& run : section.paragraphs.front().runs) {
            for (const auto& item : run.items) {
                if (item.type == InlineItem::Type::Control &&
                    item.control.kind == ControlKind::SectionDef && item.control.page) {
                    section.page = item.control.page;
                    if (item.control.defaultTab > 0) section.defaultTab = item.control.defaultTab;
                    if (item.control.columnRegionGap > 0) section.columnRegionGap = item.control.columnRegionGap;
                    section.textDirection = item.control.textDirection;
                    section.lineGrid = item.control.lineGrid;
                    section.charGrid = item.control.charGrid;
                    section.wongoji = item.control.wongoji;
                    section.pageStart = item.control.number;
                    section.hideHeader = item.control.hideHeader;
                    section.hideFooter = item.control.hideFooter;
                    section.hidePageNum = item.control.hidePageNum;
                    section.hideFirstHeader = item.control.hideFirstHeader;
                    section.hideFirstFooter = item.control.hideFirstFooter;
                    section.hideFirstMaster = item.control.hideFirstMaster;
                    section.hideFirstPageNum = item.control.hideFirstPageNum;
                    section.footnote = item.control.footnoteShape;
                    section.endnote = item.control.endnoteShape;
                    section.pageBorders = item.control.pageBorders;
                    for (const std::string& id : item.control.masterPageRefs) parseMasterPage(id, section);
                }
            }
        }
    }
}

Paragraph Parser::parseParagraph(const pugi::xml_node& p) {
    Paragraph para;
    para.paraShapeId = u32(p, "paraPrIDRef");
    para.styleId = u32(p, "styleIDRef");
    para.pageBreak = flag(p, "pageBreak");
    para.columnBreak = flag(p, "columnBreak");
    for (auto c : p.children()) {
        if (is(c, "run")) {
            Run run;
            run.charShapeId = u32(c, "charPrIDRef");
            parseRun(c, run);
            para.runs.push_back(std::move(run));
        } else if (is(c, "linesegarray") && options_.keepLineSegs) {
            for (auto s : c.children()) {
                if (!is(s, "lineseg")) continue;
                LineSeg seg;
                seg.textPos = u32(s, "textpos");
                seg.vertPos = i32(s, "vertpos");
                seg.vertSize = i32(s, "vertsize");
                seg.textHeight = i32(s, "textheight");
                seg.baseline = i32(s, "baseline");
                seg.spacing = i32(s, "spacing");
                seg.horzPos = i32(s, "horzpos");
                seg.horzSize = i32(s, "horzsize");
                seg.flags = u32(s, "flags");
                para.lineSegs.push_back(seg);
            }
        }
    }
    if (para.runs.empty()) para.runs.emplace_back();
    for (const auto& run : para.runs) {
        for (const auto& item : run.items) {
            if (item.type == InlineItem::Type::Control && item.control.kind == ControlKind::ColumnDef) {
                para.layoutBreak = true;
            }
        }
    }
    return para;
}

void Parser::parseRun(const pugi::xml_node& run, Run& out) {
    std::string pending;
    std::string markpen;
    auto flush = [&] {
        if (!pending.empty()) {
            InlineItem item = InlineItem::makeText(std::move(pending));
            item.markpen = markpen;
            out.items.push_back(std::move(item));
            pending.clear();
        }
    };

    for (auto c : run.children()) {
        if (c.type() != pugi::node_element) continue;
        const std::string_view name = localName(c.name());
        if (name == "t") {
            for (auto n : c.children()) {
                if (n.type() == pugi::node_pcdata || n.type() == pugi::node_cdata) {
                    pending += n.value();
                    continue;
                }
                const std::string_view inner = localName(n.name());
                if (inner == "tab") pending += '\t';
                else if (inner == "lineBreak") pending += '\n';
                else if (inner == "hyphen") pending += '-';
                else if (inner == "nbSpace") appendUtf8(pending, 0x00A0);
                else if (inner == "fwSpace") appendUtf8(pending, 0x2007);
                else if (inner == "markpenBegin") {
                    flush();
                    const std::string color = str(n, "color");
                    markpen = color == "none" ? std::string() : color;
                } else if (inner == "markpenEnd") {
                    flush();
                    markpen.clear();
                }
            }
        } else if (name == "markpenBegin") {
            flush();
            const std::string color = str(c, "color");
            markpen = color == "none" ? std::string() : color;
        } else if (name == "markpenEnd") {
            flush();
            markpen.clear();
        } else if (name == "secPr") {
            flush();
            out.items.push_back(InlineItem::makeControl(parseSecPr(c)));
        } else if (name == "ctrl") {
            flush();
            for (auto cc : c.children()) {
                if (cc.type() != pugi::node_element) continue;
                Control control;
                if (parseCtrl(cc, control)) {
                    InlineItem item = InlineItem::makeControl(std::move(control));
                    item.markpen = markpen;
                    out.items.push_back(std::move(item));
                }
            }
        } else if (isObjectElement(name)) {
            flush();
            out.items.push_back(InlineItem::makeControl(parseObject(c)));
        } else if (name == "dutmal") {
            flush();
            Control control;
            control.kind = ControlKind::Dutmal;
            control.ctrlId = "tdut";
            control.text = collectText(child(c, "mainText"));
            control.subText = collectText(child(c, "subText"));
            out.items.push_back(InlineItem::makeControl(std::move(control)));
        } else if (name == "compose") {
            flush();
            Control control;
            control.kind = ControlKind::Compose;
            control.ctrlId = "tcps";
            control.text = str(c, "composeText");
            out.items.push_back(InlineItem::makeControl(std::move(control)));
        }
    }
    flush();
}

SubList Parser::parseSubList(const pugi::xml_node& subList) {
    SubList out;
    if (const std::string align = str(subList, "vertAlign"); !align.empty()) out.vertAlign = align;
    {
        const std::string dir = str(subList, "textDirection");
        out.textDirection = dir == "VERTICALALL" ? 2 : (dir == "VERTICAL" ? 1 : 0);
    }
    out.textWidth = u32(subList, "textWidth");
    out.textHeight = u32(subList, "textHeight");
    for (auto p : subList.children()) {
        if (is(p, "p")) out.paragraphs.push_back(parseParagraph(p));
    }
    return out;
}

std::string noteFormatName(std::string type) {
    type = upperToken(std::move(type));
    if (type == "DIGIT" || type.empty()) return "DIGIT";
    if (type == "CIRCLEDDIGIT") return "CIRCLED_DIGIT";
    if (type == "ROMANCAPITAL") return "ROMAN_CAPITAL";
    if (type == "ROMANSMALL") return "ROMAN_SMALL";
    if (type == "LATINCAPITAL") return "LATIN_CAPITAL";
    if (type == "LATINSMALL") return "LATIN_SMALL";
    if (type == "CIRCLEDLATINCAPITAL") return "CIRCLED_LATIN_CAPITAL";
    if (type == "CIRCLEDLATINSMALL") return "CIRCLED_LATIN_SMALL";
    if (type == "HANGULSYLLABLE") return "HANGUL_SYLLABLE";
    if (type == "CIRCLEDHANGULSYLLABLE") return "CIRCLED_HANGUL_SYLLABLE";
    if (type == "HANGULJAMO") return "HANGUL_JAMO";
    if (type == "CIRCLEDHANGULJAMO") return "CIRCLED_HANGUL_JAMO";
    if (type == "HANGULPHONETIC") return "HANGUL_PHONETIC";
    if (type == "IDEOGRAPH") return "IDEOGRAPH";
    if (type == "CIRCLEDIDEOGRAPH") return "CIRCLED_IDEOGRAPH";
    if (type == "DECAGONCIRCLE") return "DECAGON";
    if (type == "DECAGONCIRCLEHANJA") return "DECAGON_HANJA";
    if (type == "SYMBOL") return "SYMBOL";
    if (type == "USERCHAR") return "USER_CHAR";
    return type;
}

int noteNumberingOf(const std::string& type) {
    const std::string key = upperToken(type);
    if (key == "ON_SECTION" || key == "ONSECTION") return 1;
    if (key == "ON_PAGE" || key == "ONPAGE" || key == "PAGE") return 2;
    return 0;
}

int notePlaceOf(const std::string& place) {
    const std::string key = upperToken(place);
    if (key == "MERGED_COLUMN" || key == "MERGEDCOLUMN" || key == "END_OF_SECTION" || key == "ENDOFSECTION") return 1;
    if (key == "RIGHTMOST_COLUMN" || key == "RIGHT_MOST_COLUMN" || key == "RIGHTMOSTCOLUMN") return 2;
    return 0;
}

std::int32_t noteLengthOf(const std::string& text) {
    if (text.empty()) return -1;
    std::string key;
    key.reserve(text.size());
    for (unsigned char c : text) {
        if (c == ' ' || c == '_') continue;
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 'a' + 'A');
        key.push_back(static_cast<char>(c));
    }
    if (key == "COLUMN/3" || key == "COLUMN3") return -1;
    if (key == "COLUMN") return 0x7FFFFFFF;
    if (key == "5CM") return mmToHu(50);
    if (key == "2CM") return mmToHu(20);
    if (key == "0" || key == "NONE") return 0;
    return static_cast<std::int32_t>(std::strtoll(text.c_str(), nullptr, 10));
}

NoteShape readNotePr(const pugi::xml_node& node) {
    NoteShape shape;
    if (auto fmt = child(node, "autoNumFormat")) {
        const std::string type = str(fmt, "type");
        if (!type.empty()) shape.numFormat = noteFormatName(type);
        shape.userChar = str(fmt, "userChar");
        shape.prefix = str(fmt, "prefixChar");
        shape.suffix = str(fmt, "suffixChar");
        shape.contentSuperscript = flag(fmt, "supscript") || flag(fmt, "superscript");
    }
    if (auto line = child(node, "noteLine")) {
        const std::string length = str(line, "length");
        if (!length.empty()) shape.lineLength = noteLengthOf(length);
        std::string type = canonicalDrawLine(hwpxLineType(str(line, "type")));
        if (!type.empty()) shape.lineType = std::move(type);
        const std::string width = str(line, "width");
        if (!width.empty()) shape.lineWidth = std::max<std::int32_t>(1, lengthHu(width));
        const std::string color = str(line, "color");
        if (!color.empty() && color != "none") shape.lineColor = color;
    }
    if (auto gap = child(node, "noteSpacing")) {
        if (gap.attribute("aboveLine")) shape.above = gap.attribute("aboveLine").as_int();
        if (gap.attribute("belowLine")) shape.below = gap.attribute("belowLine").as_int();
        if (gap.attribute("betweenNotes")) shape.between = gap.attribute("betweenNotes").as_int();
    }
    if (auto num = child(node, "numbering")) {
        shape.numbering = noteNumberingOf(str(num, "type"));
        if (num.attribute("newNum")) {
            shape.startNumber = static_cast<std::uint16_t>(std::max(0, num.attribute("newNum").as_int()));
        }
    }
    if (auto place = child(node, "placement")) {
        const std::string where = str(place, "place");
        if (!where.empty()) shape.place = notePlaceOf(where);
        shape.beneathText = flag(place, "beneathText");
    }
    return shape;
}

Control Parser::parseSecPr(const pugi::xml_node& node) {
    Control c;
    c.kind = ControlKind::SectionDef;
    c.ctrlId = "secd";
    if (auto pagePr = child(node, "pagePr")) {
        PageDef page;
        page.width = u32(pagePr, "width");
        page.height = u32(pagePr, "height");
        page.landscape = str(pagePr, "landscape") == "NARROWLY";
        const std::string gutter = str(pagePr, "gutterType");
        page.gutterType = gutter == "LEFT_RIGHT" ? 1 : (gutter == "TOP_BOTTOM" ? 2 : 0);
        if (auto m = child(pagePr, "margin")) {
            page.marginLeft = u32(m, "left");
            page.marginRight = u32(m, "right");
            page.marginTop = u32(m, "top");
            page.marginBottom = u32(m, "bottom");
            page.marginHeader = u32(m, "header");
            page.marginFooter = u32(m, "footer");
            page.marginGutter = u32(m, "gutter");
        }
        page.applyOrientation();
        c.page = page;
    }
    const std::int32_t tabVal = i32(node, "tabStopVal");
    const std::int32_t tabRaw = i32(node, "tabStop");
    c.defaultTab = tabVal > 0 ? tabVal : (tabRaw > 0 ? tabRaw / 2 : 4000);
    if (const std::int32_t gap = i32(node, "spaceColumns"); gap > 0) c.columnRegionGap = gap;
    if (auto start = child(node, "startNum")) c.number = u32(start, "page");
    // 글자 방향 이름: 0 HORIZONTAL, 1 VERTICAL, 2 VERTICALALL (한글 2024 와 같은 차례).
    {
        const std::string dir = str(node, "textDirection");
        c.textDirection = dir == "VERTICALALL" ? 2 : (dir == "VERTICAL" ? 1 : 0);
    }
    if (auto grid = child(node, "grid")) {
        c.lineGrid = i32(grid, "lineGrid");
        c.charGrid = i32(grid, "charGrid");
        c.wongoji = i32(grid, "wonggojiFormat") != 0;
    }
    if (auto vis = child(node, "visibility")) {
        c.hideFirstHeader = flag(vis, "hideFirstHeader");
        c.hideFirstFooter = flag(vis, "hideFirstFooter");
        c.hideFirstMaster = flag(vis, "hideFirstMasterPage");
        c.hideFirstPageNum = flag(vis, "hideFirstPageNum");
    }
    for (auto page : node.children()) {
        if (!is(page, "masterPage")) continue;
        const std::string id = str(page, "idRef");
        if (!id.empty()) c.masterPageRefs.push_back(id);
    }
    if (auto fn = child(node, "footNotePr")) c.footnoteShape = readNotePr(fn);
    if (auto en = child(node, "endNotePr")) c.endnoteShape = readNotePr(en);
    int pageBorderAt = 0;
    for (auto border : node.children()) {
        if (!is(border, "pageBorderFill") || pageBorderAt >= 3) continue;
        PageBorderFill fill = c.pageBorders[pageBorderAt];
        if (const std::string type = upperToken(str(border, "type")); !type.empty()) fill.type = type;
        // CONTENT 는 종이다. PAPER 는 쪽이다.
        fill.textBorder = false;
        if (border.attribute("textBorder")) {
            const std::string basis = upperToken(str(border, "textBorder"));
            fill.textBorder = basis == "PAPER" || basis == "1" || basis == "TRUE";
        }
        fill.headerInside = flag(border, "headerInside");
        fill.footerInside = flag(border, "footerInside");
        const std::string area = upperToken(str(border, "fillArea"));
        if (area == "PAGE") fill.fillArea = 1;
        else if (area == "BORDER") fill.fillArea = 2;
        else if (area == "PAPER" || !area.empty()) fill.fillArea = 0;
        if (border.attribute("borderFillIDRef")) {
            fill.borderFillId = static_cast<std::uint16_t>(u32(border, "borderFillIDRef"));
        }
        if (auto offset = child(border, "offset")) {
            if (offset.attribute("left")) fill.left = static_cast<std::int16_t>(i32(offset, "left"));
            if (offset.attribute("right")) fill.right = static_cast<std::int16_t>(i32(offset, "right"));
            if (offset.attribute("top")) fill.top = static_cast<std::int16_t>(i32(offset, "top"));
            if (offset.attribute("bottom")) fill.bottom = static_cast<std::int16_t>(i32(offset, "bottom"));
        }
        c.pageBorders[pageBorderAt] = std::move(fill);
        ++pageBorderAt;
    }
    return c;
}

void Parser::parseMasterPage(const std::string& id, Section& section) {
    std::string path = "Contents/" + id + ".xml";
    if (const auto found = hrefById_.find(id); found != hrefById_.end() && !found->second.empty()) {
        path = found->second;
    }
    pugi::xml_document xml;
    if (!loadXml(path, xml)) {
        warn("failed to read master page " + path);
        return;
    }
    const auto root = xml.document_element();
    MasterPage master;
    if (const std::string type = str(root, "type"); !type.empty()) master.applyPageType = type;
    master.printOnTop = flag(root, "pageFront");
    if (auto sub = child(root, "subList")) master.content = parseSubList(sub);
    if (!master.content.paragraphs.empty()) section.masterPages.push_back(std::move(master));
}

bool Parser::parseCtrl(const pugi::xml_node& node, Control& c) {
    const std::string_view name = localName(node.name());
    if (name == "colPr") {
        c.kind = ControlKind::ColumnDef;
        c.ctrlId = "cold";
        c.columnCount = u32(node, "colCount", 1);
        c.columnGap = i32(node, "sameGap");
        c.columnSameSize = node.attribute("sameSz") ? flag(node, "sameSz") : true;
        c.columnType = str(node, "type");
        c.columnLayout = str(node, "layout");
        for (auto kid : node.children()) {
            if (is(kid, "colSz")) {
                ColumnWidth slice;
                slice.width = i32(kid, "width");
                slice.gap = i32(kid, "gap");
                c.columnWidths.push_back(slice);
            } else if (is(kid, "colLine")) {
                std::string type = str(kid, "type");
                for (char& ch : type) {
                    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
                }
                type = hwpxLineType(type);
                if (type.empty() || type == "NONE") continue;
                c.columnLineType = type;
                c.columnLineColor = str(kid, "color");
                c.columnLineWidth = lengthHu(str(kid, "width"));
            }
        }
    } else if (name == "header" || name == "footer") {
        c.kind = name == "header" ? ControlKind::Header : ControlKind::Footer;
        c.ctrlId = name == "header" ? "head" : "foot";
        c.applyPageType = str(node, "applyPageType");
        if (c.applyPageType.empty()) c.applyPageType = "BOTH";
        if (auto sub = child(node, "subList")) c.subLists.push_back(parseSubList(sub));
    } else if (name == "footNote" || name == "endNote") {
        c.kind = name == "footNote" ? ControlKind::Footnote : ControlKind::Endnote;
        c.ctrlId = name == "footNote" ? "fn  " : "en  ";
        c.number = u32(node, "number");
        if (auto sub = child(node, "subList")) c.subLists.push_back(parseSubList(sub));
    } else if (name == "autoNum" || name == "newNum") {
        c.kind = name == "autoNum" ? ControlKind::AutoNumber : ControlKind::NewNumber;
        c.ctrlId = name == "autoNum" ? "atno" : "nwno";
        c.numType = str(node, "numType");
        c.number = u32(node, "num");
        if (auto fmt = child(node, "autoNumFormat")) {
            c.numFormat = str(fmt, "type");
            c.prefix = str(fmt, "prefixChar");
            c.suffix = str(fmt, "suffixChar");
        }
    } else if (name == "pageHiding") {
        c.kind = ControlKind::PageHiding;
        c.ctrlId = "pghd";
        c.hideHeader = flag(node, "hideHeader");
        c.hideFooter = flag(node, "hideFooter");
        c.hidePageNum = flag(node, "hidePageNum");
    } else if (name == "pageNum") {
        c.kind = ControlKind::PageNumPos;
        c.ctrlId = "pgnp";
        c.pageNumPos = str(node, "pos");
        c.numFormat = str(node, "formatType");
        const std::string side = str(node, "sideChar");
        if (!side.empty()) {
            c.prefix = side;
            c.suffix = side;
        }
    } else if (name == "bookmark") {
        c.kind = ControlKind::Bookmark;
        c.ctrlId = "bokm";
        c.name = str(node, "name");
    } else if (name == "fieldBegin") {
        c.kind = ControlKind::Field;
        c.ctrlId = fieldCtrlId(str(node, "type"));
        c.name = str(node, "name");
        for (auto param : child(node, "parameters").children()) {
            if (is(param, "stringParam") && str(param, "name") == "Command") {
                c.command = collectText(param);
            }
        }
    } else if (name == "fieldEnd") {
        return false;  // HWP 에서도 인라인 문자일 뿐 컨트롤이 아니다
    } else if (name == "hiddenComment" || name == "memo") {
        c.kind = ControlKind::HiddenComment;
        c.ctrlId = name == "memo" ? "memo" : "tcmt";
        if (auto sub = child(node, "subList")) c.subLists.push_back(parseSubList(sub));
        if (c.subLists.empty()) c.text = collectText(node);
    } else if (name == "button" || name == "radioButton" || name == "checkButton" || name == "comboBox" ||
               name == "edit" || name == "listBox" || name == "scrollBar") {
        c.kind = ControlKind::Unknown;
        c.ctrlId = "form";
        c.shapeType = std::string(name);
        c.name = str(node, "name");
        c.text = collectText(node);
    } else {
        c.kind = ControlKind::Unknown;
        c.ctrlId = std::string(name);
    }
    return true;
}

Placement Parser::readPlacement(const pugi::xml_node& node) {
    Placement p;
    p.zOrder = i32(node, "zOrder");
    p.textWrap = str(node, "textWrap");
    if (const std::string flow = str(node, "textFlow"); !flow.empty()) p.textFlow = flow;
    p.numberingType = str(node, "numberingType");
    std::uint32_t orgW = 0;
    std::uint32_t orgH = 0;
    if (auto org = child(node, "orgSz")) {
        orgW = u32(org, "width");
        orgH = u32(org, "height");
        p.width = orgW;
        p.height = orgH;
    }
    if (auto sz = child(node, "sz")) {
        p.width = u32(sz, "width");
        p.height = u32(sz, "height");
    }
    // curSz 가 0이면 배율 적용 전과 같다. 0이 아니어도 앞쪽 배율만 들어 있을 수 있다.
    bool widthIsCurrent = false;
    bool heightIsCurrent = false;
    if (auto cur = child(node, "curSz")) {
        if (const std::uint32_t width = u32(cur, "width")) {
            p.width = width;
            widthIsCurrent = true;
        }
        if (const std::uint32_t height = u32(cur, "height")) {
            p.height = height;
            heightIsCurrent = true;
        }
    }
    if (auto off = child(node, "offset")) {
        p.groupX = i32(off, "x");
        p.groupY = i32(off, "y");
    }
    // 그룹할 때마다 행렬이 하나 더 붙는다. 배율은 크기에 곱하고, 행렬의 이동(e3, e6)은 offset에 더한다.
    // 이동은 뒤 행렬부터 로컬 원점에 적용한다. transMatrix 는 offset 과 같으므로 다시 더하지 않는다.
    double scaleX = 1;
    double scaleY = 1;
    std::vector<double> scaleXs{1.0};
    std::vector<double> scaleYs{1.0};
    struct Affix {
        double e1 = 1;
        double e2 = 0;
        double e3 = 0;
        double e4 = 0;
        double e5 = 1;
        double e6 = 0;
        bool translation = false;
    };
    std::vector<Affix> affixes;
    if (auto info = child(node, "renderingInfo")) {
        for (auto matrix : info.children()) {
            const bool translation = is(matrix, "transMatrix");
            if (!translation && !is(matrix, "scaMatrix") && !is(matrix, "rotMatrix")) continue;
            Affix affix;
            affix.e1 = matrix.attribute("e1").as_double(1);
            affix.e2 = matrix.attribute("e2").as_double(0);
            affix.e3 = matrix.attribute("e3").as_double(0);
            affix.e4 = matrix.attribute("e4").as_double(0);
            affix.e5 = matrix.attribute("e5").as_double(1);
            affix.e6 = matrix.attribute("e6").as_double(0);
            affix.translation = translation;
            affixes.push_back(affix);
            if (!translation && is(matrix, "scaMatrix")) {
                if (affix.e1 != 0) scaleX *= affix.e1;
                if (affix.e5 != 0) scaleY *= affix.e5;
                scaleXs.push_back(scaleX);
                scaleYs.push_back(scaleY);
            }
        }
    }
    double shiftX = 0;
    double shiftY = 0;
    for (auto it = affixes.rbegin(); it != affixes.rend(); ++it) {
        if (it->translation) continue;
        const double nextX = it->e1 * shiftX + it->e2 * shiftY + it->e3;
        const double nextY = it->e4 * shiftX + it->e5 * shiftY + it->e6;
        shiftX = nextX;
        shiftY = nextY;
    }
    p.matrixX = static_cast<std::int32_t>(std::llround(shiftX));
    p.matrixY = static_cast<std::int32_t>(std::llround(shiftY));
    p.scaleX = scaleX;
    p.scaleY = scaleY;
    p.groupX += p.matrixX;
    p.groupY += p.matrixY;
    applyScale(p.width, orgW, widthIsCurrent, scaleXs);
    applyScale(p.height, orgH, heightIsCurrent, scaleYs);
    // 배치 크기는 sz 다. curSz 는 원본×배율의 소수점을 버려 1 작을 수 있다. 40575×0.195613 은 sz 7937, curSz 7936이다.
    // 묶음 안 자식에는 sz 가 없어 위 계산을 그대로 쓴다.
    if (auto sz = child(node, "sz")) {
        const std::uint32_t szW = u32(sz, "width");
        const std::uint32_t szH = u32(sz, "height");
        if (szW > 0 && nearHu(szW, p.width)) p.width = szW;
        if (szH > 0 && nearHu(szH, p.height)) p.height = szH;
    }
    if (auto pos = child(node, "pos")) {
        p.treatAsChar = flag(pos, "treatAsChar");
        if (pos.attribute("flowWithText")) p.flowWithText = flag(pos, "flowWithText");
        if (pos.attribute("allowOverlap")) p.allowOverlap = flag(pos, "allowOverlap");
        p.vertRelTo = str(pos, "vertRelTo");
        p.horzRelTo = str(pos, "horzRelTo");
        if (const std::string align = str(pos, "vertAlign"); !align.empty()) p.vertAlign = align;
        if (const std::string align = str(pos, "horzAlign"); !align.empty()) p.horzAlign = align;
        p.vertOffset = i32(pos, "vertOffset");
        p.horzOffset = i32(pos, "horzOffset");
    }
    if (auto margin = child(node, "outMargin")) {
        p.marginLeft = i32(margin, "left");
        p.marginRight = i32(margin, "right");
        p.marginTop = i32(margin, "top");
        p.marginBottom = i32(margin, "bottom");
    }
    if (auto flip = child(node, "flip")) {
        p.flipHorizontal = flag(flip, "horizontal");
        p.flipVertical = flag(flip, "vertical");
    }
    return p;
}

Control Parser::parseObject(const pugi::xml_node& node) {
    const std::string_view name = localName(node.name());
    Control c;
    c.placement = readPlacement(node);
    if (auto line = child(node, "lineShape")) {
        c.strokeType = hwpxLineType(str(line, "style"));
        c.strokeColor = str(line, "color");
        c.strokeWidth = i32(line, "width");
    }
    if (auto brush = child(node, "fillBrush")) {
        std::string imageId;
        int imageMode = -1;
        readFill(brush, c.fillColor, c.gradColor, c.gradAngle, imageId, imageMode);
        if (!imageId.empty()) c.binItemId = imageId;
    }
    std::uint32_t orgW = 0;
    std::uint32_t orgH = 0;
    if (auto org = child(node, "orgSz")) {
        orgW = u32(org, "width");
        orgH = u32(org, "height");
    }
    // 배율이 0인 축은 상자 크기에서는 건너뛰지만 점은 그 축으로 납작해진다.
    // (교육과정 머리말의 선: 100x100 선을 가로 484.71배, 세로 0배로 그려 수평선이 된다.)
    bool flatX = false;
    bool flatY = false;
    if (auto info = child(node, "renderingInfo")) {
        for (auto matrix : info.children()) {
            if (!is(matrix, "scaMatrix")) continue;
            if (matrix.attribute("e1").as_double(1) == 0) flatX = true;
            if (matrix.attribute("e5").as_double(1) == 0) flatY = true;
        }
    }
    auto pushOrgPoint = [&](std::int32_t rawX, std::int32_t rawY) {
        double x = static_cast<double>(rawX);
        double y = static_cast<double>(rawY);
        if (flatX) x = 0;
        else if (orgW > 0 && c.placement && c.placement->width > 0) {
            x *= static_cast<double>(c.placement->width) / static_cast<double>(orgW);
        }
        if (flatY) y = 0;
        else if (orgH > 0 && c.placement && c.placement->height > 0) {
            y *= static_cast<double>(c.placement->height) / static_cast<double>(orgH);
        }
        ShapePoint point;
        point.x = static_cast<std::int32_t>(std::llround(x));
        point.y = static_cast<std::int32_t>(std::llround(y));
        c.points.push_back(point);
    };
    if (name == "line" || name == "connectLine") {
        if (auto start = child(node, "startPt")) pushOrgPoint(i32(start, "x"), i32(start, "y"));
        if (auto end = child(node, "endPt")) pushOrgPoint(i32(end, "x"), i32(end, "y"));
    } else if (name == "curve") {
        bool have = false;
        std::int32_t lastX = 0;
        std::int32_t lastY = 0;
        for (auto seg : node.children()) {
            if (!is(seg, "seg")) continue;
            const std::int32_t x1 = i32(seg, "x1");
            const std::int32_t y1 = i32(seg, "y1");
            const std::int32_t x2 = i32(seg, "x2");
            const std::int32_t y2 = i32(seg, "y2");
            if (!have || x1 != lastX || y1 != lastY) pushOrgPoint(x1, y1);
            pushOrgPoint(x2, y2);
            lastX = x2;
            lastY = y2;
            have = true;
        }
    } else if (name == "textart") {
        c.text = str(node, "text");
        for (const char* tag : {"pt0", "pt1", "pt2", "pt3"}) {
            if (auto pt = child(node, tag)) pushOrgPoint(i32(pt, "x"), i32(pt, "y"));
        }
        if (auto outline = child(node, "outline")) {
            for (auto pt : outline.children()) {
                if (is(pt, "pt")) pushOrgPoint(i32(pt, "x"), i32(pt, "y"));
            }
        }
        if (auto shape = child(node, "textartPr")) {
            if (c.equationFont.empty()) c.equationFont = str(shape, "fontName");
        }
    } else if (name == "ellipse" || name == "arc") {
        for (const char* tag : {"center", "ax1", "ax2", "start1", "end1", "start2", "end2"}) {
            if (auto pt = child(node, tag)) pushOrgPoint(i32(pt, "x"), i32(pt, "y"));
        }
    } else {
        for (auto pt : node.children()) {
            if (!is(pt, "pt")) continue;
            pushOrgPoint(i32(pt, "x"), i32(pt, "y"));
        }
    }
    // 좌우·상하 반전은 현재 상자 안에서 점을 뒤집는다.
    if (c.placement && (c.placement->flipHorizontal || c.placement->flipVertical)) {
        const std::int32_t boxW = static_cast<std::int32_t>(c.placement->width);
        const std::int32_t boxH = static_cast<std::int32_t>(c.placement->height);
        for (ShapePoint& point : c.points) {
            if (c.placement->flipHorizontal && boxW > 0) point.x = boxW - point.x;
            if (c.placement->flipVertical && boxH > 0) point.y = boxH - point.y;
        }
    }
    if (auto cap = child(node, "caption")) {
        Caption caption;
        caption.side = str(cap, "side");
        caption.fullSize = flag(cap, "fullSz");
        caption.width = u32(cap, "width");
        caption.gap = i32(cap, "gap");
        caption.lastWidth = u32(cap, "lastWidth");
        if (auto sub = child(cap, "subList")) caption.content = parseSubList(sub);
        c.caption = std::move(caption);
    }

    if (name == "tbl") {
        c.kind = ControlKind::Table;
        c.ctrlId = "tbl ";
        Table t;
        t.rowCount = u32(node, "rowCnt");
        t.colCount = u32(node, "colCnt");
        t.cellSpacing = u32(node, "cellSpacing");
        t.borderFillId = u32(node, "borderFillIDRef");
        if (const std::string pb = str(node, "pageBreak"); !pb.empty()) t.pageBreak = pb;
        t.repeatHeader = flag(node, "repeatHeader");
        t.noAdjust = flag(node, "noAdjust");
        if (auto margin = child(node, "inMargin")) {
            t.marginLeft = i32(margin, "left");
            t.marginRight = i32(margin, "right");
            t.marginTop = i32(margin, "top");
            t.marginBottom = i32(margin, "bottom");
        }
        for (auto tr : node.children()) {
            if (!is(tr, "tr")) continue;
            for (auto tc : tr.children()) {
                if (!is(tc, "tc")) continue;
                TableCell cell;
                cell.borderFillId = u32(tc, "borderFillIDRef");
                cell.hasMargin = flag(tc, "hasMargin");
                cell.header = flag(tc, "header");
                if (auto addr = child(tc, "cellAddr")) {
                    cell.col = u32(addr, "colAddr");
                    cell.row = u32(addr, "rowAddr");
                }
                if (auto span = child(tc, "cellSpan")) {
                    cell.colSpan = u32(span, "colSpan", 1);
                    cell.rowSpan = u32(span, "rowSpan", 1);
                }
                if (auto sz = child(tc, "cellSz")) {
                    cell.width = u32(sz, "width");
                    cell.height = u32(sz, "height");
                }
                if (auto margin = child(tc, "cellMargin")) {
                    cell.marginLeft = i32(margin, "left");
                    cell.marginRight = i32(margin, "right");
                    cell.marginTop = i32(margin, "top");
                    cell.marginBottom = i32(margin, "bottom");
                }
                if (auto sub = child(tc, "subList")) {
                    cell.content = parseSubList(sub);
                    if (!cell.content.vertAlign.empty()) cell.vertAlign = cell.content.vertAlign;
                }
                t.cells.push_back(std::move(cell));
            }
        }
        c.table = std::move(t);
        return c;
    }

    if (name == "equation") {
        c.kind = ControlKind::Equation;
        c.ctrlId = "eqed";
        c.script = collectText(child(node, "script"));
        if (!node.attribute("baseLine").empty()) c.baseLine = i32(node, "baseLine");
        c.fileBaseLine = c.baseLine;
        if (!node.attribute("baseUnit").empty()) c.baseUnit = i32(node, "baseUnit");
        c.equationFont = str(node, "font");
        return c;
    }

    c.ctrlId = "gso ";
    c.shapeType = name == "connectLine" ? "line" : std::string(name);
    if (name == "pic") {
        c.kind = ControlKind::Picture;
        if (auto img = descendant(node, "img")) {
            c.binItemId = str(img, "binaryItemIDRef");
            c.picture.bright = i32(img, "bright");
            c.picture.contrast = i32(img, "contrast");
            c.picture.effect = pictureEffect(str(img, "effect"));
            const int alpha = i32(img, "alpha");
            c.picture.transparency = alpha < 0 ? 0 : std::min(alpha, 255);
        }
        if (auto clip = child(node, "imgClip")) {
            c.picture.clipLeft = i32(clip, "left");
            c.picture.clipTop = i32(clip, "top");
            c.picture.clipRight = i32(clip, "right");
            c.picture.clipBottom = i32(clip, "bottom");
        }
        if (auto dim = child(node, "imgDim")) {
            c.picture.originWidth = i32(dim, "dimwidth");
            c.picture.originHeight = i32(dim, "dimheight");
        }
        if (auto rect = child(node, "imgRect")) {
            bool any = false;
            std::int32_t minX = 0;
            std::int32_t minY = 0;
            std::int32_t maxX = 0;
            std::int32_t maxY = 0;
            for (auto pt : rect.children()) {
                if (pt.type() != pugi::node_element) continue;
                const std::string_view ptName = localName(pt.name());
                if (ptName.size() < 2 || ptName[0] != 'p' || ptName[1] != 't') continue;
                const std::int32_t x = i32(pt, "x");
                const std::int32_t y = i32(pt, "y");
                if (!any) {
                    minX = maxX = x;
                    minY = maxY = y;
                    any = true;
                } else {
                    minX = std::min(minX, x);
                    minY = std::min(minY, y);
                    maxX = std::max(maxX, x);
                    maxY = std::max(maxY, y);
                }
            }
            if (any) {
                c.picture.rectWidth = maxX - minX;
                c.picture.rectHeight = maxY - minY;
                if (c.picture.originWidth <= 0 || c.picture.originHeight <= 0) {
                    c.picture.originWidth = c.picture.rectWidth;
                    c.picture.originHeight = c.picture.rectHeight;
                }
            }
        }
        if (auto margin = child(node, "inMargin")) {
            c.picture.padLeft = i32(margin, "left");
            c.picture.padRight = i32(margin, "right");
            c.picture.padTop = i32(margin, "top");
            c.picture.padBottom = i32(margin, "bottom");
        }
    } else if (name == "container") {
        c.kind = ControlKind::Container;
        for (auto sub : node.children()) {
            if (sub.type() == pugi::node_element && isObjectElement(localName(sub.name()))) {
                Control child = parseObject(sub);
                // 묶음 안 좌표는 가장 바깥 묶음 기준이다. 부모 묶음 위치를 빼야 그 묶음 안에 들어간다.
                if (child.placement) child.placement->groupInRoot = true;
                c.children.push_back(std::move(child));
            }
        }
    } else if (name == "ole") {
        c.kind = ControlKind::Ole;
    } else if (name == "textart" || name == "chart" || name == "video") {
        c.kind = ControlKind::Shape;
        if (name == "textart" && c.text.empty()) c.text = str(node, "text");
        if (auto pr = child(node, "textartPr")) {
            if (c.equationFont.empty()) c.equationFont = str(pr, "fontName");
        }
    } else {
        c.kind = ControlKind::Shape;
        // 모서리 곡률 %. 0 은 직각, 20 은 둥근 모서리, 50 은 짧은 변의 반원.
        if (name == "rect") c.cornerRatio = i32(node, "ratio");
    }
    auto readBoxPad = [&](const pugi::xml_node& margin) {
        if (!margin) return;
        c.boxPadLeft = i32(margin, "left");
        c.boxPadRight = i32(margin, "right");
        c.boxPadTop = i32(margin, "top");
        c.boxPadBottom = i32(margin, "bottom");
    };
    if (auto drawText = child(node, "drawText")) {
        c.boxTextWidth = u32(drawText, "lastWidth");
        readBoxPad(child(drawText, "textMargin"));
        if (auto sub = child(drawText, "subList")) c.subLists.push_back(parseSubList(sub));
    }
    if (c.boxPadLeft == 0 && c.boxPadRight == 0) readBoxPad(child(node, "textMargin"));
    return c;
}

void Parser::parsePreviewText() {
    Bytes bytes;
    if (!zip_.read("Preview/PrvText.txt", bytes)) return;
    std::string text(bytes.begin(), bytes.end());
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    doc_.previewText = std::move(text);
}

}  // namespace

ParseResult parse(ByteView data, const ParseOptions& options) {
    ParseResult result;
    Parser parser(data, options, result);
    parser.run();
    return result;
}

}  // namespace chwp::hwpx
