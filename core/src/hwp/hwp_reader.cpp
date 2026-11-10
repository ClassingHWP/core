#include "chwp/hwp/hwp_reader.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/model/memo.hpp"
#include "chwp/model/proofreading.hpp"
#include "chwp/unit.hpp"
#include "chwp/container/cfb.hpp"
#include "chwp/hwp/records.hpp"
#include "common/time_format.hpp"
#include "hwp/hwp_detail.hpp"

namespace chwp::hwp {

namespace detail {

namespace {

constexpr std::uint16_t kPitNull = 0;
constexpr std::uint16_t kPitBstr = 1;
constexpr std::uint16_t kPitSet = 0x8000;
constexpr std::uint16_t kPitArray = 0x8001;
constexpr std::uint16_t kPitBinData = 0x8002;
constexpr int kMaxParameterDepth = 8;

bool skipParameterSet(ByteReader& r, int depth);

// 아이템 값 하나를 건너뛴다. 모르는 종류면 이후 위치를 알 수 없으므로 false.
bool skipParameterValue(ByteReader& r, std::uint16_t type, int depth) {
    if (depth > kMaxParameterDepth) return false;
    switch (type) {
        case kPitNull: return true;
        case kPitBstr: r.skip(std::size_t{r.u16()} * 2); break;
        case kPitSet: return skipParameterSet(r, depth + 1);
        case kPitArray: {
            const std::int16_t count = r.i16();
            if (count <= 0) break;
            r.u16();  // 배열 원소 공통 아이템 ID
            for (std::int16_t i = 0; i < count; ++i) {
                const std::uint16_t elementType = r.u16();
                if (!skipParameterValue(r, elementType, depth + 1)) return false;
            }
            break;
        }
        case kPitBinData: r.u16(); break;
        default:
            if (type < 2 || type > 9) return false;
            r.u32();  // 정수 계열은 모두 4바이트
            break;
    }
    return r.ok();
}

bool skipParameterSet(ByteReader& r, int depth) {
    r.u16();  // 셋 ID
    const std::int16_t count = r.i16();
    r.skip(2);
    for (std::int16_t i = 0; i < count && r.ok(); ++i) {
        r.u16();  // 아이템 ID
        const std::uint16_t type = r.u16();
        if (!skipParameterValue(r, type, depth)) return false;
    }
    return r.ok();
}

}  // namespace

std::optional<std::string> findParameterString(ByteView data,
                                               std::optional<std::uint16_t> itemId) {
    ByteReader r(data);
    r.u16();  // 셋 ID
    const std::int16_t count = r.i16();
    r.skip(2);
    for (std::int16_t i = 0; i < count && r.ok(); ++i) {
        const std::uint16_t id = r.u16();
        const std::uint16_t type = r.u16();
        if (type == kPitBstr) {
            std::string value = r.hwpString();
            if (!r.ok()) break;
            if (!itemId || id == *itemId) return value;
        } else if (!skipParameterValue(r, type, 0)) {
            break;
        }
    }
    return std::nullopt;
}

model::Caption readCaptionHeader(ByteView data) {
    static const char* sides[] = {"LEFT", "RIGHT", "TOP", "BOTTOM"};
    ByteReader r(data);
    r.u32();  // 문단 수
    const std::uint32_t listAttr = r.u32();
    model::Caption caption;
    const std::uint32_t attr = r.u32();
    caption.attr = attr;
    caption.side = sides[attr & 0x3];
    caption.fullSize = ((attr >> 2) & 0x1) != 0;
    caption.width = r.u32();
    // 간격은 부호 있는 HWPUNIT16이다. 음수면 캡션이 개체 안으로 들어간다.
    caption.gap = static_cast<std::int16_t>(r.u16());
    caption.lastWidth = r.u32();
    const unsigned valign = (listAttr >> 5) & 0x3;
    if (valign == 1) caption.content.vertAlign = "CENTER";
    else if (valign == 2) caption.content.vertAlign = "BOTTOM";
    caption.content.textDirection = static_cast<std::uint8_t>(std::min<std::uint32_t>(listAttr & 0x7, 2));
    return caption;
}

ControlMatcher::ControlMatcher(std::vector<std::uint32_t> ctrlIds)
    : ids_(std::move(ctrlIds)), used_(ids_.size(), false) {}

std::optional<std::size_t> ControlMatcher::match(std::optional<std::uint32_t> ctrlId) {
    std::optional<std::size_t> firstUnused;
    for (std::size_t k = 0; k < ids_.size(); ++k) {
        if (used_[k]) continue;
        if (!firstUnused) firstUnused = k;
        if (ctrlId && ids_[k] == *ctrlId) {
            used_[k] = true;
            return k;
        }
    }
    if (!firstUnused) return std::nullopt;
    if (ctrlId) ++mismatches_;
    used_[*firstUnused] = true;
    return firstUnused;
}

std::vector<std::size_t> ControlMatcher::remaining() const {
    std::vector<std::size_t> out;
    for (std::size_t k = 0; k < ids_.size(); ++k) {
        if (!used_[k]) out.push_back(k);
    }
    return out;
}

}  // namespace detail

namespace {

using namespace chwp::model;

constexpr char kSignature[] = "HWP Document File";
constexpr std::uint16_t kCellNameItem = 0x4000;  // 셀 필드 이름 파라미터 아이템

constexpr std::uint32_t kFlagCompressed = 1u << 0;
constexpr std::uint32_t kFlagEncrypted = 1u << 1;
constexpr std::uint32_t kFlagDistribution = 1u << 2;
constexpr std::uint32_t kFlagDrm = 1u << 4;
constexpr std::uint32_t kFlagCertEncrypted = 1u << 8;
constexpr std::uint32_t kFlagCertDrm = 1u << 10;

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

constexpr std::size_t kLineSegSize = 36;
constexpr std::size_t kPictureBinIdOffset = 71;

// 문단 텍스트 안의 제어 문자 (0~31) 분류
enum class CharKind { Char, Inline, Extended };

CharKind classifyControlChar(char16_t ch) {
    switch (ch) {
        case 4: case 5: case 6: case 7: case 8: case 9: case 19: case 20:
            return CharKind::Inline;
        case 1: case 2: case 3: case 11: case 12: case 14: case 15: case 16: case 17: case 18:
        case 21: case 22: case 23:
            return CharKind::Extended;
        default:
            return CharKind::Char;
    }
}

// COLORREF(0x00BBGGRR) → 0xRRGGBB
Color fromColorRef(std::uint32_t v) {
    if (v == 0xFFFFFFFF) return std::nullopt;
    return ((v & 0xFF) << 16) | (v & 0xFF00) | ((v >> 16) & 0xFF);
}

std::string hexColor(Color color) {
    if (!color) return "none";
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(*color & 0xFFFFFFu));
    return buf;
}

// 표 26. 굵기 번호만 저장되고, HWPUNIT은 mm × 283465 / 1000 을 버린 값이다.
std::int32_t borderWidthHu(unsigned thick) {
    static const double mm[] = {0.1, 0.12, 0.15, 0.2,  0.25, 0.3, 0.4, 0.5,
                                 0.6, 0.7,  1.0,  1.5,  2.0,  3.0, 4.0, 5.0};
    if (thick >= std::size(mm)) thick = 0;
    return mmToHu(mm[thick]);
}

BorderSide readBorderSide(ByteReader& r) {
    BorderSide side;
    if (r.remaining() < 6) return side;
    const unsigned kind = r.u8();
    const unsigned thick = r.u8();
    const Color color = fromColorRef(r.u32());
    if (kind == 0) return side;
    static const char* kinds[] = {"NONE",     "SOLID",    "DASH",
                                  "DOT",      "DASH_DOT", "DASH_DOT_DOT",
                                  "LONG_DASH","CIRCLE",   "DOUBLE_SLIM",
                                  "SLIM_THICK","THICK_SLIM","SLIM_THICK_SLIM",
                                  "WAVE",     "DOUBLE_WAVE","THICK_3D",
                                  "THICK_3D_REVERSE_LIGHTING", "3D", "3D_REVERSE_LIGHTING"};
    side.type = kind < std::size(kinds) ? kinds[kind] : "SOLID";
    side.width = borderWidthHu(thick);
    side.color = hexColor(color);
    if (side.color == "none") side.color = "#000000";
    return side;
}

std::string diagonalShapeName(unsigned shape) {
    switch (shape) {
        case 0: return "NONE";
        case 2: return "CENTER";
        case 3: return "CENTER_BELOW";
        case 6: return "CENTER_ABOVE";
        case 7: return "ALL";
        default: return "CENTER";
    }
}

// 방향마다 종류·굵기·색이 붙어 있다. 채우기 없음은 40바이트, 단색은 53바이트.
// 그러데이션은 종류 1바이트 뒤에 각도·중심·번짐·색 수를 4바이트씩 둔다.
BorderFill readBorderFill(ByteReader r, std::uint32_t id) {
    BorderFill fill;
    fill.id = id;
    if (r.remaining() < 2) return fill;
    const std::uint16_t attr = r.u16();
    fill.slash.type = diagonalShapeName((attr >> 2) & 0x7u);
    fill.backSlash.type = diagonalShapeName((attr >> 5) & 0x7u);
    fill.slash.crooked = ((attr >> 8) & 0x1u) != 0;
    fill.backSlash.crooked = ((attr >> 9) & 0x1u) != 0;
    fill.breakCellSeparateLine = ((attr >> 10) & 0x1u) != 0;
    fill.slash.counter = ((attr >> 11) & 0x1u) != 0;
    fill.backSlash.counter = ((attr >> 12) & 0x1u) != 0;
    if (((attr >> 13) & 0x1u) != 0) {
        const bool h = ((attr >> 8) & 0x1u) != 0;
        const bool v = ((attr >> 9) & 0x1u) != 0;
        fill.centerLine = (h && v) ? "CROSS" : (h ? "HORIZONTAL" : (v ? "VERTICAL" : "NONE"));
    }
    fill.left = readBorderSide(r);
    fill.right = readBorderSide(r);
    fill.top = readBorderSide(r);
    fill.bottom = readBorderSide(r);
    if (r.remaining() < 6) return fill;
    fill.diagonal = readBorderSide(r);
    if (r.remaining() < 4) return fill;
    const std::uint32_t type = r.u32();
    if ((type & 1u) != 0 && r.remaining() >= 16) {
        fill.faceColor = hexColor(fromColorRef(r.u32()));
        const Color hatchColor = fromColorRef(r.u32());
        const std::int32_t hatch = r.i32();
        static const char* const hatches[] = {"HORIZONTAL", "VERTICAL", "SLASH", "BACK_SLASH", "CROSS", "CROSS_DIAGONAL"};
        if (hatch >= 0 && hatch < static_cast<std::int32_t>(std::size(hatches))) {
            fill.hatchStyle = hatches[hatch];
            fill.hatchColor = hatchColor ? hexColor(hatchColor) : "#000000";
        }
    }
    if ((type & 4u) != 0 && r.remaining() >= 21) {
        const unsigned gradKind = r.u8();
        const std::int32_t angle = r.i32();
        r.i32();
        r.i32();
        r.i32();
        const std::int32_t num = r.i32();
        if (gradKind >= 1 && gradKind <= 4 && num >= 1 && num <= 8 &&
            r.remaining() >= static_cast<std::size_t>(num) * 4u) {
            fill.gradAngle = angle;
            fill.faceColor = hexColor(fromColorRef(r.u32()));
            if (num >= 2) fill.gradColor = hexColor(fromColorRef(r.u32()));
            for (std::int32_t i = 2; i < num; ++i) r.u32();
        }
    }
    if ((type & 2u) != 0 && r.remaining() >= 6) {
        fill.imageMode = static_cast<int>(r.u8());
        r.skip(3);
        fill.imageId = std::to_string(r.u16());
    }
    if (r.remaining() >= 4) {
        const auto extra = static_cast<std::size_t>(r.u32());
        if (extra > 0 && extra < 64 && r.remaining() >= extra) r.skip(extra);
    }
    if ((type & 1u) != 0 && r.remaining() >= 1) r.u8();
    return fill;
}

std::string numTypeName(std::uint32_t v) {
    static const char* names[] = {"PAGE", "FOOTNOTE", "ENDNOTE", "PICTURE", "TABLE", "EQUATION"};
    return v < std::size(names) ? names[v] : "UNKNOWN";
}

std::string numberShapeName(unsigned shape) {
    switch (shape) {
        case 1: return "CIRCLED_DIGIT";
        case 2: return "ROMAN_CAPITAL";
        case 3: return "ROMAN_SMALL";
        case 4: return "LATIN_CAPITAL";
        case 5: return "LATIN_SMALL";
        case 6: return "CIRCLED_LATIN_CAPITAL";
        case 7: return "CIRCLED_LATIN_SMALL";
        case 8: return "HANGUL_SYLLABLE";
        case 9: return "CIRCLED_HANGUL_SYLLABLE";
        case 10: return "HANGUL_JAMO";
        case 11: return "CIRCLED_HANGUL_JAMO";
        case 12: return "HANGUL_PHONETIC";
        case 13: return "IDEOGRAPH";
        case 14: return "CIRCLED_IDEOGRAPH";
        case 15: return "DECAGON";
        case 16: return "DECAGON_HANJA";
        case 0x80: return "SYMBOL";
        case 0x81: return "USER_CHAR";
        default: return "DIGIT";
    }
}

std::string oneWchar(ByteReader& r) {
    if (r.remaining() < 2) return {};
    const char16_t ch = static_cast<char16_t>(r.u16());
    if (ch == 0) return {};
    return utf16ToUtf8(std::u16string(1, ch));
}

// 그리기 개체 선 종류는 테두리와 같다. 0이 선 없음, 1이 실선이다.
std::string shapeLineType(unsigned kind) {
    static const char* lines[] = {"NONE",      "SOLID",     "DASH",
                                  "DOT",       "DASH_DOT",  "DASH_DOT_DOT",
                                  "LONG_DASH", "CIRCLE",    "DOUBLE_SLIM",
                                  "SLIM_THICK","THICK_SLIM","SLIM_THICK_SLIM",
                                  "WAVE",      "DOUBLE_WAVE", "THICK_3D",
                                  "THICK_3D_REVERSE_LIGHTING", "3D", "3D_REVERSE_LIGHTING"};
    if (kind >= std::size(lines)) return "SOLID";
    return lines[kind];
}

bool shapeHasInk(const std::string& type) {
    return type == "rect" || type == "ellipse" || type == "line" || type == "arc" || type == "polygon" ||
           type == "curve";
}

// 테두리 13바이트 다음의 채우기. 단색만 색을 남긴다.
void readShapeInk(ByteReader& r, Control& control) {
    if (!shapeHasInk(control.shapeType) || r.remaining() < 13) return;
    const Color color = fromColorRef(r.u32());
    const std::int32_t width = r.i32();
    const std::uint32_t attr = r.u32();
    r.u8();
    if (!r.ok()) return;
    const std::string line = shapeLineType(attr & 0x3Fu);
    if (width > 0 && line != "NONE") {
        control.strokeWidth = width;
        control.strokeColor = hexColor(color);
        if (control.strokeColor == "none") control.strokeColor = "#000000";
        control.strokeType = line;
        control.strokeEnd = static_cast<int>((attr >> 6) & 0xFu);
    }
    if (r.remaining() < 8) return;
    const std::uint32_t fill = r.u32();
    if ((fill & 0x1u) == 0 || r.remaining() < 4) return;
    control.fillColor = hexColor(fromColorRef(r.u32()));
}

// 개체 공통 헤더에서 선·채우기 앞까지. 최상위는 컨트롤 ID가 한 번 더 있다.
// 배율과 행렬 이동은 묶음 자식과 같은 행렬 쌍에서 읽는다. 크기는 바꾸지 않는다.
void skipShapeHeader(ByteReader& r, double& scaleX, double& scaleY, double& shiftX, double& shiftY,
                     std::uint32_t& initW, std::uint32_t& initH, std::uint32_t& curW, std::uint32_t& curH) {
    scaleX = 1;
    scaleY = 1;
    shiftX = 0;
    shiftY = 0;
    initW = 0;
    initH = 0;
    curW = 0;
    curH = 0;
    if (r.remaining() < 28) return;
    r.i32();
    r.i32();
    r.u16();
    r.u16();
    initW = r.u32();
    initH = r.u32();
    curW = r.u32();
    curH = r.u32();
    if (r.remaining() < 16) return;
    r.u32();
    r.u16();
    r.i32();
    r.i32();
    const std::uint16_t pairs = r.u16();
    r.skip(48);
    struct Affix {
        double e1 = 1;
        double e2 = 0;
        double e3 = 0;
        double e4 = 0;
        double e5 = 1;
        double e6 = 0;
    };
    std::vector<Affix> affixes;
    auto readDouble = [&]() {
        const std::uint64_t bits = r.u64();
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    for (std::uint16_t i = 0; i < pairs && r.remaining() >= 96; ++i) {
        Affix scale;
        scale.e1 = readDouble();
        scale.e2 = readDouble();
        scale.e3 = readDouble();
        scale.e4 = readDouble();
        scale.e5 = readDouble();
        scale.e6 = readDouble();
        Affix rot;
        rot.e1 = readDouble();
        rot.e2 = readDouble();
        rot.e3 = readDouble();
        rot.e4 = readDouble();
        rot.e5 = readDouble();
        rot.e6 = readDouble();
        affixes.push_back(scale);
        affixes.push_back(rot);
        if (scale.e1 != 0) scaleX *= scale.e1;
        if (scale.e5 != 0) scaleY *= scale.e5;
    }
    for (auto it = affixes.rbegin(); it != affixes.rend(); ++it) {
        const double nextX = it->e1 * shiftX + it->e2 * shiftY + it->e3;
        const double nextY = it->e4 * shiftX + it->e5 * shiftY + it->e6;
        shiftX = nextX;
        shiftY = nextY;
    }
}

std::string shapeTypeFromId(std::uint32_t id) {
    switch (id) {
        case makeCtrlId('$', 'p', 'i', 'c'): return "pic";
        case makeCtrlId('$', 'r', 'e', 'c'): return "rect";
        case makeCtrlId('$', 'e', 'l', 'l'): return "ellipse";
        case makeCtrlId('$', 'l', 'i', 'n'): return "line";
        case makeCtrlId('$', 'a', 'r', 'c'): return "arc";
        case makeCtrlId('$', 'p', 'o', 'l'): return "polygon";
        case makeCtrlId('$', 'c', 'u', 'r'): return "curve";
        case makeCtrlId('$', 'c', 'o', 'n'): return "container";
        case makeCtrlId('$', 'o', 'l', 'e'): return "ole";
        case makeCtrlId('$', 't', 'a', 't'): return "textart";
        default: return ctrlIdToString(id);
    }
}

std::string mediaTypeFromExtension(std::string ext) {
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == "bmp") return "image/bmp";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "tif" || ext == "tiff") return "image/tiff";
    if (ext == "wmf") return "image/x-wmf";
    if (ext == "emf") return "image/x-emf";
    if (ext == "svg") return "image/svg+xml";
    return "application/octet-stream";
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

class Parser {
public:
    Parser(ByteView data, const ParseOptions& options, ParseResult& result)
        : data_(data), options_(options), result_(result), doc_(result.document) {}

    bool run();

private:
    struct BinStorage {
        std::string streamPath;
        std::uint32_t compression = 0;  // 0: 문서 설정 따름, 1: 압축, 2: 비압축
    };

    void warn(std::string message) { result_.warnings.push_back(std::move(message)); }
    bool fail(std::string message) {
        result_.error = std::move(message);
        return false;
    }

    const cfb::Entry* findEntry(std::string_view path, bool ignoreCase) const;
    bool loadStream(std::string_view path, bool compressed, Bytes& out);

    bool readFileHeader();
    void parseDocInfo(ByteView stream);
    void parseCharShape(ByteReader& r);
    void parseNumbering(ByteReader& r);
    void parseBullet(ByteReader& r);
    void parseParaShape(ByteReader& r);
    void parseTabDef(ByteReader& r);
    void parseStyle(ByteReader& r);
    void parseBinData(ByteReader& r);
    void parseSection(ByteView stream, Section& section);

    Paragraph parseParagraph(const RecordTree& tree, std::size_t index);
    Control parseControl(const RecordTree& tree, std::size_t index);
    void parseShapeComponent(const RecordTree& tree, std::size_t index, Control& control);
    void parseTable(const RecordTree& tree, const RecordTree::Node& node, Control& control);
    void parseCaption(const RecordTree& tree, const RecordTree::Node& node, Control& control);
    void collectSubLists(const RecordTree& tree, const RecordTree::Node& node,
                         std::vector<SubList>& out);
    void parseMasterPages(const RecordTree& tree, const RecordTree::Node& node, Control& control, std::uint32_t secdAttr);
    SubList readMasterList(const RecordTree& tree, const RecordTree::Node& node);
    static Placement readPlacement(ByteReader& r);
    static PageDef readPageDef(ByteView data);

    void parseSummaryInformation();
    void parsePreviewText();
    void loadBinData();

    ByteView data_;
    const ParseOptions& options_;
    ParseResult& result_;
    Document& doc_;
    cfb::CompoundFile cfb_;
    std::uint32_t flags_ = 0;
    std::vector<BinStorage> binStorage_;
    // 구역 끝(마지막 문단 자식)의 마지막 쪽·임의 쪽 바탕쪽. parseSection 이 구역에 붙인다.
    std::vector<SubList> extMasters_;
    // 마지막 구역 끝의 메모 글(메모 번호, 글). 모든 구역을 읽은 뒤 번호가 같은 메모 필드에 붙인다.
    std::vector<std::pair<std::uint32_t, SubList>> memoTexts_;
    void attachMemos();
    std::size_t orphanControlChars_ = 0;
    std::size_t mismatchedControlChars_ = 0;
};

const Record* findChild(const RecordTree& tree, const RecordTree::Node& node, std::uint16_t tag) {
    for (std::size_t child : node.children) {
        if (tree.at(child).record->tag == tag) return tree.at(child).record;
    }
    return nullptr;
}

bool Parser::run() {
    doc_.format.format = SourceFormat::Hwp;

    std::string error;
    if (!cfb_.open(data_, &error)) return fail("CFB: " + error);
    if (!readFileHeader()) return false;

    if (doc_.format.encrypted) {
        return fail("password-protected HWP documents are not supported");
    }
    if (flags_ & (kFlagDrm | kFlagCertDrm)) {
        return fail("DRM-protected HWP documents are not supported");
    }
    if (flags_ & kFlagCertEncrypted) {
        return fail("certificate-encrypted HWP documents are not supported");
    }

    Bytes docInfo;
    if (!loadStream("DocInfo", doc_.format.compressed, docInfo)) {
        return fail("failed to read DocInfo stream");
    }
    parseDocInfo(docInfo);

    if (doc_.format.distribution) {
        warn("distribution document: encrypted body (ViewText) is not supported");
    } else {
        for (std::uint32_t i = 0;; ++i) {
            const std::string path = "BodyText/Section" + std::to_string(i);
            if (!findEntry(path, false)) break;
            Bytes stream;
            if (!loadStream(path, doc_.format.compressed, stream)) {
                warn("failed to read " + path);
                continue;
            }
            Section section;
            parseSection(stream, section);
            doc_.sections.push_back(std::move(section));
        }
        if (doc_.header.sectionCount != doc_.sections.size()) {
            warn("section count mismatch: DocInfo=" + std::to_string(doc_.header.sectionCount) +
                 ", streams=" + std::to_string(doc_.sections.size()));
        }
    }

    if (orphanControlChars_ > 0) {
        warn(std::to_string(orphanControlChars_) + " control characters without CTRL_HEADER");
    }
    if (mismatchedControlChars_ > 0) {
        warn(std::to_string(mismatchedControlChars_) +
             " control characters whose ctrlId does not match any CTRL_HEADER");
    }

    attachMemos();
    parseSummaryInformation();
    parsePreviewText();
    loadBinData();

    result_.ok = true;
    return true;
}

// 메모 글(MEMO_LIST 번호)을 메모 필드에 붙이고, MemoExtended 흐름(태그 101: 개수, (답글 id, 원 id)…)으로 답글을 단다.
void Parser::attachMemos() {
    std::vector<Control*> memos = collectMemos(doc_);
    for (auto& [number, text] : memoTexts_) {
        for (Control* memo : memos) {
            if (memo->memoNumber != number || !memo->subLists.empty()) continue;
            text.vertAlign = "TOP";
            memo->subLists.push_back(std::move(text));
            break;
        }
    }
    memoTexts_.clear();
    Bytes extended;
    if (!memos.empty() && loadStream("MemoExtended", doc_.format.compressed, extended)) {
        std::vector<Record> records;
        std::string error;
        if (parseRecords(extended, records, &error)) {
            for (const Record& rec : records) {
                if (rec.tag != 101) continue;
                ByteReader r(rec.data);
                const std::uint32_t count = r.remaining() >= 4 ? r.u32() : 0;
                for (std::uint32_t i = 0; i < count && r.remaining() >= 8; ++i) {
                    const std::uint32_t id = r.u32();
                    const std::uint32_t parent = r.u32();
                    for (Control* memo : memos) {
                        if (memo->fieldId == id) memo->memoParentId = parent;
                    }
                }
            }
        }
    }
}

const cfb::Entry* Parser::findEntry(std::string_view path, bool ignoreCase) const {
    if (!ignoreCase) return cfb_.find(path);
    for (const auto& e : cfb_.entries()) {
        if (equalsIgnoreCase(e.path, path)) return &e;
    }
    return nullptr;
}

bool Parser::loadStream(std::string_view path, bool compressed, Bytes& out) {
    const cfb::Entry* entry = findEntry(path, false);
    if (!entry) return false;
    Bytes raw;
    if (!cfb_.readStream(*entry, raw)) return false;
    if (!compressed) {
        out = std::move(raw);
        return true;
    }
    return decompressStream(raw, out);
}

bool Parser::readFileHeader() {
    Bytes header;
    if (!cfb_.readStream("FileHeader", header) || header.size() < 40) {
        return fail("missing FileHeader stream");
    }
    if (std::memcmp(header.data(), kSignature, sizeof(kSignature) - 1) != 0) {
        return fail("invalid HWP signature");
    }
    ByteReader r(header);
    r.seek(32);
    const std::uint32_t v = r.u32();
    flags_ = r.u32();

    char version[32];
    std::snprintf(version, sizeof(version), "%u.%u.%u.%u", (v >> 24) & 0xFF, (v >> 16) & 0xFF,
                  (v >> 8) & 0xFF, v & 0xFF);
    doc_.format.version = version;
    // 주 버전(MM)이 다르면 구조가 호환되지 않는다.
    if (((v >> 24) & 0xFF) != 5) {
        return fail(std::string("unsupported HWP version ") + version + " (only 5.x is supported)");
    }
    doc_.format.compressed = (flags_ & kFlagCompressed) != 0;
    doc_.format.encrypted = (flags_ & kFlagEncrypted) != 0;
    doc_.format.distribution = (flags_ & kFlagDistribution) != 0;
    return true;
}

// ---------------------------------------------------------------------------
// DocInfo
// ---------------------------------------------------------------------------

void Parser::parseDocInfo(ByteView stream) {
    std::vector<Record> records;
    std::string error;
    if (!parseRecords(stream, records, &error)) warn("DocInfo: " + error);

    DocHeader& h = doc_.header;
    PerLang<std::int32_t> fontCounts{};
    bool haveMappings = false;
    std::size_t fontLang = 0;
    std::size_t borderFills = 0;

    for (const auto& rec : records) {
        ByteReader r(rec.data);
        switch (rec.tag) {
            case tag::DocumentProperties:
                h.sectionCount = r.u16();
                h.beginNumbers.page = r.u16();
                h.beginNumbers.footnote = r.u16();
                h.beginNumbers.endnote = r.u16();
                h.beginNumbers.picture = r.u16();
                h.beginNumbers.table = r.u16();
                h.beginNumbers.equation = r.u16();
                break;
            case tag::IdMappings:
                r.u32();  // binData count
                for (auto& count : fontCounts) count = r.i32();
                haveMappings = true;
                break;
            case tag::BinData: parseBinData(r); break;
            case tag::FaceName: {
                while (haveMappings && fontLang < kLangCount &&
                       static_cast<std::int32_t>(h.fonts[fontLang].size()) >= fontCounts[fontLang]) {
                    ++fontLang;
                }
                const std::size_t lang = std::min(fontLang, kLangCount - 1);
                Font font;
                font.id = static_cast<std::uint32_t>(h.fonts[lang].size());
                const auto attr = r.u8();
                font.face = r.hwpString();
                if ((attr & 0x80) != 0) {
                    r.u8();  // 대체 글꼴 유형
                    font.subst = r.hwpString();
                }
                if ((attr & 0x40) != 0) {
                    const ByteView info = r.bytes(10);
                    font.typeInfo.assign(info.begin(), info.end());
                }
                if ((attr & 0x20) != 0) font.base = r.hwpString();
                h.fonts[lang].push_back(std::move(font));
                break;
            }
            case tag::BorderFill:
                h.borderFills.push_back(readBorderFill(r, static_cast<std::uint32_t>(borderFills + 1)));
                ++borderFills;
                break;
            case tag::CharShape: parseCharShape(r); break;
            case tag::Numbering: parseNumbering(r); break;
            case tag::Bullet: parseBullet(r); break;
            case tag::ParaShape: parseParaShape(r); break;
            case tag::TabDef: parseTabDef(r); break;
            case tag::Style: parseStyle(r); break;
            case tag::MemoShape: {
                // 메모 모양 22바이트: 너비, 선 종류, 선 굵기, 선 색, 채우기 색, 활성 채우기 색, 메모 종류.
                // 너비가 0 이면 한글은 기본값 전체로 바꾼다.
                MemoShape shape;
                if (r.remaining() >= 22) {
                    const std::uint32_t width = r.u32();
                    const std::uint8_t lineType = r.u8();
                    const std::uint8_t lineWidth = r.u8();
                    const Color line = fromColorRef(r.u32());
                    const Color fill = fromColorRef(r.u32());
                    const Color active = fromColorRef(r.u32());
                    const std::uint32_t memoType = r.u32();
                    if (width > 0) {
                        shape.width = width;
                        shape.lineType = lineType;
                        shape.lineWidth = lineWidth;
                        shape.lineColor = hexColor(line);
                        shape.fillColor = hexColor(fill);
                        shape.activeColor = hexColor(active);
                        shape.memoType = memoType;
                    }
                }
                h.memoShapes.push_back(shape);
                break;
            }
            default: {
                KeptRecord kept;
                kept.tag = rec.tag;
                kept.data.assign(rec.data.begin(), rec.data.end());
                h.kept.push_back(std::move(kept));
                break;
            }
        }
    }
    h.borderFillCount = borderFills;
}

std::string numberingFormatName(unsigned format) {
    switch (format) {
        case 1: return "CIRCLED_DIGIT";
        case 2: return "ROMAN_CAPITAL";
        case 3: return "ROMAN_SMALL";
        case 4: return "LATIN_CAPITAL";
        case 5: return "LATIN_SMALL";
        case 6: return "CIRCLED_LATIN_CAPITAL";
        case 7: return "CIRCLED_LATIN_SMALL";
        case 8: return "HANGUL_SYLLABLE";
        case 9: return "CIRCLED_HANGUL_SYLLABLE";
        case 10: return "HANGUL_JAMO";
        case 11: return "CIRCLED_HANGUL_JAMO";
        case 12: return "HANGUL_PHONETIC";
        case 13: return "IDEOGRAPH";
        case 14: return "CIRCLED_IDEOGRAPH";
        default: return "DIGIT";
    }
}

void Parser::parseNumbering(ByteReader& r) {
    Numbering numbering;
    numbering.id = static_cast<std::uint32_t>(doc_.header.numberings.size() + 1);
    auto readLevel = [&](int level) {
        if (!r.ok() || r.remaining() < 14) return false;
        ParaHead head;
        head.level = level;
        const auto attr = r.u32();
        head.widthAdjust = r.u16();
        head.textOffset = r.u16();
        head.charShapeId = r.u32();
        head.numFormat = numberingFormatName((attr >> 5) & 0xF);
        const int align = attr & 0x3;
        head.align = align == 1 ? "CENTER" : align == 2 ? "RIGHT" : "LEFT";
        head.useInstWidth = (attr & 0x4) != 0;
        head.autoIndent = (attr & 0x8) != 0;
        head.textOffsetType = (attr & 0x10) != 0 ? "HWPUNIT" : "PERCENT";
        head.format = r.hwpString();
        head.start = 1;
        if (!r.ok()) return false;
        numbering.levels.push_back(std::move(head));
        return true;
    };
    for (int level = 1; level <= 7; ++level) {
        if (!readLevel(level)) break;
    }
    if (r.ok() && r.remaining() >= 2) numbering.start = r.u16();
    for (int i = 0; i < 7 && r.ok() && r.remaining() >= 4; ++i) {
        const auto start = r.u32();
        if (i < static_cast<int>(numbering.levels.size()) && start > 0) {
            numbering.levels[static_cast<std::size_t>(i)].start = static_cast<int>(start);
        }
    }
    for (int level = 8; level <= 10; ++level) {
        if (!readLevel(level)) break;
    }
    if (!numbering.levels.empty()) doc_.header.numberings.push_back(std::move(numbering));
}

static std::string bmpToUtf8(std::uint16_t unit) {
    if (unit == 0) return {};
    std::string out;
    if (unit < 0x80) {
        out.push_back(static_cast<char>(unit));
    } else if (unit < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (unit >> 6)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (unit >> 12)));
        out.push_back(static_cast<char>(0x80 | ((unit >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (unit & 0x3F)));
    }
    return out;
}

// 별책5 실측 25바이트. 12바이트 머리 + 문자 + 이미지 ID + 그림 파라미터 4바이트 + 1바이트 + 체크 문자.
void Parser::parseBullet(ByteReader& r) {
    if (!r.ok() || r.remaining() < 14) return;
    Bullet bullet;
    bullet.id = static_cast<std::uint32_t>(doc_.header.bullets.size() + 1);
    ParaHead& head = bullet.head;
    const auto attr = r.u32();
    head.widthAdjust = static_cast<std::int16_t>(r.u16());
    head.textOffset = static_cast<std::int16_t>(r.u16());
    head.charShapeId = r.u32();
    const int align = attr & 0x3;
    head.align = align == 1 ? "CENTER" : align == 2 ? "RIGHT" : "LEFT";
    head.useInstWidth = (attr & 0x4) != 0;
    head.autoIndent = (attr & 0x8) != 0;
    head.textOffsetType = (attr & 0x10) != 0 ? "HWPUNIT" : "PERCENT";
    bullet.text = bmpToUtf8(r.u16());
    if (r.remaining() >= 4) {
        const std::int32_t image = r.i32();
        if (image > 0) bullet.imageId = std::to_string(image);
    }
    if (r.remaining() >= 4) {
        bullet.contrast = r.i8();
        bullet.bright = r.i8();
        bullet.effect = r.u8();
        r.u8();
    }
    if (r.remaining() > 2) r.u8();
    if (r.remaining() >= 2) bullet.checkedText = bmpToUtf8(r.u16());
    doc_.header.bullets.push_back(std::move(bullet));
}

void Parser::parseCharShape(ByteReader& r) {
    CharShape cs;
    cs.id = static_cast<std::uint32_t>(doc_.header.charShapes.size());
    for (auto& v : cs.fontRef) v = r.u16();
    for (auto& v : cs.ratio) v = r.u8();
    for (auto& v : cs.spacing) v = r.i8();
    for (auto& v : cs.relSize) v = r.u8();
    for (auto& v : cs.offset) v = r.i8();
    cs.height = r.i32();
    const std::uint32_t attr = r.u32();
    cs.shadowOffsetX = r.i8();
    cs.shadowOffsetY = r.i8();
    cs.textColor = fromColorRef(r.u32());
    cs.underlineColor = fromColorRef(r.u32());
    cs.shadeColor = fromColorRef(r.u32());
    cs.shadowColor = fromColorRef(r.u32());
    if (r.remaining() >= 2) cs.borderFillId = r.u16();
    if (r.remaining() >= 4) cs.strikeColor = fromColorRef(r.u32());

    cs.italic = (attr & 0x1) != 0;
    cs.bold = (attr & 0x2) != 0;
    // 글자 모양 대화상자의 밑줄 위치는 없음·글자 아래·글자 위뿐이다. 값 2는 그 목록에 없다.
    const unsigned underlinePos = (attr >> 2) & 0x3;
    cs.underline = underlinePos == 1 ? "BOTTOM" : underlinePos == 3 ? "TOP" : "NONE";
    // 밑줄·취소선 모양은 표 25 저장값이다. 그릴 때는 1을 더해 0을 실선으로 본다.
    static const char* lineNames[] = {"NONE",     "SOLID",    "DASH",
                                      "DOT",      "DASH_DOT", "DASH_DOT_DOT",
                                      "LONG_DASH","CIRCLE",   "DOUBLE_SLIM",
                                      "SLIM_THICK","THICK_SLIM","SLIM_THICK_SLIM",
                                      "WAVE",     "DOUBLE_WAVE","THICK_3D",
                                      "THICK_3D_REVERSE_LIGHTING", "3D", "3D_REVERSE_LIGHTING"};
    const auto lineName = [&](unsigned stored) {
        const unsigned kind = stored + 1;
        return kind < std::size(lineNames) ? lineNames[kind] : "SOLID";
    };
    cs.underlineShapeId = static_cast<std::uint8_t>((attr >> 4) & 0xF);
    cs.underlineShape = lineName(cs.underlineShapeId);
    cs.outline = static_cast<std::uint8_t>((attr >> 8) & 0x7);
    cs.shadow = static_cast<std::uint8_t>((attr >> 11) & 0x3);
    cs.emboss = ((attr >> 13) & 0x1) != 0;
    cs.engrave = ((attr >> 14) & 0x1) != 0;
    cs.superscript = ((attr >> 15) & 0x1) != 0;
    cs.subscript = ((attr >> 16) & 0x1) != 0;
    cs.strikeout = ((attr >> 18) & 0x7) != 0;
    cs.emphasis = static_cast<std::uint8_t>((attr >> 21) & 0xF);
    cs.useFontSpace = ((attr >> 25) & 0x1) != 0;
    cs.strikeShapeId = static_cast<std::uint8_t>((attr >> 26) & 0xF);
    cs.strikeShape = lineName(cs.strikeShapeId);
    cs.useKerning = ((attr >> 30) & 0x1) != 0;
    doc_.header.charShapes.push_back(cs);
}

void Parser::parseTabDef(ByteReader& r) {
    static const char* types[] = {"LEFT", "RIGHT", "CENTER", "DECIMAL"};
    // 탭 채움은 테두리 선과 달리 2 가 점선, 3 이 파선이다(한글 2024, tmp/roundtrip/leaders).
    static const char* leaders[] = {"NONE",     "SOLID",    "DOT",
                                    "DASH",     "DASH_DOT", "DASH_DOT_DOT",
                                    "LONG_DASH","CIRCLE",   "DOUBLE_SLIM",
                                    "SLIM_THICK","THICK_SLIM","SLIM_THICK_SLIM",
                                    "WAVE",     "DOUBLE_WAVE","THICK_3D",
                                    "THICK_3D_REVERSE_LIGHTING", "3D", "3D_REVERSE_LIGHTING"};
    TabDef def;
    def.id = static_cast<std::uint32_t>(doc_.header.tabDefs.size());
    const std::uint32_t attr = r.u32();
    def.autoTabLeft = (attr & 0x1) != 0;
    def.autoTabRight = (attr & 0x2) != 0;
    const std::int32_t count = r.i32();
    for (std::int32_t i = 0; i < count && r.remaining() >= 8; ++i) {
        TabStop stop;
        stop.pos = r.i32() / 2;
        const unsigned type = r.u8();
        const unsigned leader = r.u8();
        r.u16();
        stop.type = type < std::size(types) ? types[type] : "LEFT";
        stop.leader = leader < std::size(leaders) ? leaders[leader] : "NONE";
        def.stops.push_back(stop);
    }
    doc_.header.tabDefs.push_back(std::move(def));
}

void Parser::parseParaShape(ByteReader& r) {
    static const char* aligns[] = {"JUSTIFY", "LEFT", "RIGHT", "CENTER", "DISTRIBUTE",
                                   "DISTRIBUTE_SPACE"};
    static const char* latinBreaks[] = {"KEEP_WORD", "HYPHENATION", "BREAK_WORD", "BREAK_WORD"};
    static const char* headings[] = {"NONE", "OUTLINE", "NUMBER", "BULLET"};
    static const char* lineSpacings[] = {"PERCENT", "FIXED", "BETWEEN_LINES", "AT_LEAST"};

    ParaShape ps;
    ps.id = static_cast<std::uint32_t>(doc_.header.paraShapes.size());
    // 여백/간격은 실제 HWPUNIT 의 2배로 저장된다 (HWPX 의 hp:default 분기와 같은 값).
    const std::uint32_t attr1 = r.u32();
    ps.marginLeft = r.i32() / 2;
    ps.marginRight = r.i32() / 2;
    ps.indent = r.i32() / 2;
    ps.spacingPrev = r.i32() / 2;
    ps.spacingNext = r.i32() / 2;
    const std::int32_t legacyLineSpacing = r.i32();
    ps.tabDefId = r.u16();
    ps.headingIdRef = r.u16();
    ps.borderFillId = r.u16();
    ps.borderOffsetLeft = r.i16();
    ps.borderOffsetRight = r.i16();
    ps.borderOffsetTop = r.i16();
    ps.borderOffsetBottom = r.i16();
    std::uint32_t lineSpacingType = attr1 & 0x3;
    ps.lineSpacing = legacyLineSpacing;
    std::uint32_t attr2 = 0;
    if (r.remaining() >= 12) {  // 5.0.2.5 이상
        attr2 = r.u32();
        lineSpacingType = r.u32() & 0x1F;
        ps.lineSpacing = static_cast<std::int32_t>(r.u32());
    }
    // 한글 2022 부터 문단 모양 끝에 수준(0~9)이 하나 더 있다. 속성 1 의 수준 3비트는 6 에서 멈춘다(tmp/roundtrip).
    std::int32_t fullLevel = -1;
    if (r.remaining() >= 4) fullLevel = static_cast<std::int32_t>(r.u32());

    const std::uint32_t align = (attr1 >> 2) & 0x7;
    ps.align = align < std::size(aligns) ? aligns[align] : "JUSTIFY";
    ps.breakLatinWord = latinBreaks[(attr1 >> 5) & 0x3];
    // bit 7 = 1 은 규격대로 글자다. HWPX 는 이것을 KEEP_WORD 로, 어절(0)을 BREAK_WORD 로 적는다(tmp/wordbreak).
    ps.breakNonLatinWord = ((attr1 >> 7) & 0x1) ? "KEEP_WORD" : "BREAK_WORD";
    ps.snapToGrid = ((attr1 >> 8) & 0x1) != 0;
    ps.condense = std::min<std::int32_t>(75, static_cast<std::int32_t>((attr1 >> 9) & 0x7F));
    ps.widowOrphan = ((attr1 >> 16) & 0x1) != 0;
    ps.keepWithNext = ((attr1 >> 17) & 0x1) != 0;
    ps.keepLines = ((attr1 >> 18) & 0x1) != 0;
    ps.pageBreakBefore = ((attr1 >> 19) & 0x1) != 0;
    static const char* vertAligns[] = {"BASELINE", "TOP", "CENTER", "BOTTOM"};
    ps.vertAlign = vertAligns[(attr1 >> 20) & 0x3];
    ps.fontLineHeight = ((attr1 >> 22) & 0x1) != 0;
    ps.headingType = headings[(attr1 >> 23) & 0x3];
    ps.borderConnect = ((attr1 >> 28) & 0x1) != 0;
    ps.borderIgnoreMargin = ((attr1 >> 29) & 0x1) != 0;
    ps.autoSpacingEng = ((attr2 >> 4) & 0x1) != 0;
    ps.autoSpacingNum = ((attr2 >> 5) & 0x1) != 0;
    ps.suppressLineNumbers = ((attr2 >> 6) & 0x1) != 0;
    static const char* wraps[] = {"BREAK", "SQUEEZE", "KEEP"};
    const std::uint32_t wrap = attr2 & 0x3;
    if (wrap < std::size(wraps)) ps.lineWrap = wraps[wrap];
    ps.headingLevel = (attr1 >> 25) & 0x7;
    if (fullLevel >= 0 && fullLevel <= 9) ps.headingLevel = static_cast<std::uint32_t>(fullLevel);
    ps.lineSpacingType = lineSpacingType < std::size(lineSpacings) ? lineSpacings[lineSpacingType]
                                                                   : "PERCENT";
    // 고정값·여백만·최소는 여백처럼 2배로 저장된다. 고정값 2400은 줄 높이 1200이다.
    if (ps.lineSpacingType != "PERCENT") ps.lineSpacing /= 2;
    doc_.header.paraShapes.push_back(ps);
}

void Parser::parseStyle(ByteReader& r) {
    Style st;
    st.id = static_cast<std::uint32_t>(doc_.header.styles.size());
    st.name = r.hwpString();
    st.engName = r.hwpString();
    const std::uint8_t attr = r.u8();
    st.type = (attr & 0x7) == 1 ? "CHAR" : "PARA";
    st.nextStyleId = r.u8();
    st.langId = r.i16();
    st.paraShapeId = r.u16();
    st.charShapeId = r.u16();
    doc_.header.styles.push_back(std::move(st));
}

void Parser::parseBinData(ByteReader& r) {
    const std::uint16_t attr = r.u16();
    BinItem item;
    BinStorage storage;
    // 그림 등이 참조하는 ID 는 BIN_DATA 레코드 순번(1부터)이다. 스토리지 ID 는 스트림 이름에만 쓴다.
    item.id = std::to_string(doc_.header.binItems.size() + 1);
    item.compression = storage.compression = (attr >> 4) & 0x3;

    switch (attr & 0xF) {
        case 0:
            item.storage = "LINK";
            item.path = r.hwpString();  // 절대 경로
            r.hwpString();              // 상대 경로
            break;
        case 1:
        case 2: {
            item.storage = (attr & 0xF) == 1 ? "EMBEDDING" : "STORAGE";
            const std::uint16_t binId = r.u16();
            if ((attr & 0xF) == 1) item.extension = r.hwpString();
            char name[32];
            std::snprintf(name, sizeof(name), "BIN%04X", binId);
            item.path = std::string("BinData/") + name;
            if (!item.extension.empty()) item.path += "." + item.extension;
            storage.streamPath = item.path;
            break;
        }
        default: item.storage = "UNKNOWN"; break;
    }
    item.mediaType = mediaTypeFromExtension(item.extension);
    doc_.header.binItems.push_back(std::move(item));
    binStorage_.push_back(std::move(storage));
}

// ---------------------------------------------------------------------------
// BodyText
// ---------------------------------------------------------------------------

MasterPage masterFromList(const SubList& list) {
    MasterPage master;
    master.applyPageType = list.applyPageType.empty() ? "BOTH" : list.applyPageType;
    master.pageNumber = list.masterPageNumber;
    master.duplicate = list.masterDuplicate;
    master.printOnTop = list.masterFront;
    master.content = list;
    return master;
}

void Parser::parseSection(ByteView stream, Section& section) {
    extMasters_.clear();
    std::vector<Record> records;
    std::string error;
    if (!parseRecords(stream, records, &error)) warn("BodyText: " + error);

    const RecordTree tree = RecordTree::build(records);
    for (std::size_t child : tree.root().children) {
        if (tree.at(child).record->tag == tag::ParaHeader) {
            section.paragraphs.push_back(parseParagraph(tree, child));
        }
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
                    section.sectionAttrKeep = item.control.sectionAttrKeep;
                    section.lineGrid = item.control.lineGrid;
                    section.charGrid = item.control.charGrid;
                    section.wongoji = item.control.wongoji;
                    section.pageStart = item.control.number;
                    section.hideHeader = item.control.hideHeader;
                    section.hideFooter = item.control.hideFooter;
                    section.hideMaster = item.control.hideMaster;
                    section.hidePageNum = item.control.hidePageNum;
                    section.hideFirstHeader = item.control.hideFirstHeader;
                    section.hideFirstFooter = item.control.hideFirstFooter;
                    section.hideFirstMaster = item.control.hideFirstMaster;
                    section.hideFirstPageNum = item.control.hideFirstPageNum;
                    section.startsOn = item.control.startsOn;
                    section.pictureStart = item.control.pictureStart;
                    section.tableStart = item.control.tableStart;
                    section.equationStart = item.control.equationStart;
                    section.borderShow = item.control.borderShow;
                    section.fillShow = item.control.fillShow;
                    section.hideFirstEmptyLine = item.control.hideFirstEmptyLine;
                    section.lineNumbers = item.control.lineNumbers;
                    section.memoShapeId = item.control.memoShapeId;
                    section.footnote = item.control.footnoteShape;
                    section.endnote = item.control.endnoteShape;
                    section.pageBorders = item.control.pageBorders;
                    for (const auto& list : item.control.subLists) section.masterPages.push_back(masterFromList(list));
                }
            }
        }
    }
    for (const SubList& list : extMasters_) section.masterPages.push_back(masterFromList(list));
    extMasters_.clear();
}

Paragraph Parser::parseParagraph(const RecordTree& tree, std::size_t index) {
    const auto& node = tree.at(index);
    Paragraph para;
    {
        ByteReader h(node.record->data);
        h.u32();  // 글자 수 (최상위 비트는 플래그)
        h.u32();  // 컨트롤 마스크
        para.paraShapeId = h.u16();
        para.styleId = h.u8();
        const std::uint8_t breakType = h.u8();
        para.pageBreak = (breakType & 0x04) != 0;
        para.columnBreak = (breakType & 0x08) != 0;
        para.layoutBreak = (breakType & 0x02) != 0;
    }

    ByteView text;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> shapes;  // (위치, 글자 모양 ID)
    // 영역 태그. 끝은 포함하지 않는다. 형광펜(종류 2)의 하위 24비트는 COLORREF.
    struct RangeTag {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        std::uint32_t kind = 0;
        std::uint32_t data = 0;
    };
    std::vector<RangeTag> ranges;
    std::vector<std::size_t> controls;
    std::uint16_t lastTag = 0;
    bool inExtMaster = false;
    bool inMemo = false;
    for (std::size_t child : node.children) {
        const Record& rec = *tree.at(child).record;
        const std::uint16_t prevTag = lastTag;
        lastTag = rec.tag;
        switch (rec.tag) {
            case tag::ParaText: text = rec.data; break;
            case tag::ParaCharShape: {
                ByteReader r(rec.data);
                while (r.remaining() >= 8) {
                    const std::uint32_t pos = r.u32();
                    shapes.emplace_back(pos, r.u32());
                }
                break;
            }
            case tag::ParaRangeTag: {
                ByteReader r(rec.data);
                while (r.remaining() >= 12) {
                    RangeTag range;
                    range.start = r.u32();
                    range.end = r.u32();
                    const std::uint32_t raw = r.u32();
                    range.kind = raw >> 24;
                    range.data = raw & 0xFFFFFFu;
                    ranges.push_back(range);
                }
                break;
            }
            default: {
                // 마지막 쪽·임의 쪽 바탕쪽은 구역 마지막 문단의 자식 리스트 머리와 뒤따르는 문단(같은 레벨)이다.
                // 메모 글도 같은 모양이지만 리스트 머리 앞에 메모 목록(MEMO_LIST)이 온다.
                if (rec.tag == tag::MemoList) {
                    // 메모 글: MEMO_LIST(메모 번호), LIST_HEADER, 문단(같은 레벨)이 메모마다 온다.
                    ByteReader m(rec.data);
                    memoTexts_.push_back({m.remaining() >= 4 ? m.u32() : 0u, SubList{}});
                    inExtMaster = false;
                    inMemo = true;
                    break;
                }
                if (rec.tag == tag::ListHeader && prevTag == tag::MemoList && inMemo) break;
                if (rec.tag == tag::ListHeader && prevTag != tag::MemoList) {
                    extMasters_.push_back(readMasterList(tree, tree.at(child)));
                    inExtMaster = true;
                    inMemo = false;
                    break;
                }
                if (rec.tag == tag::ParaHeader && inExtMaster) {
                    Paragraph extPara = parseParagraph(tree, child);
                    extMasters_.back().paragraphs.push_back(std::move(extPara));
                    break;
                }
                if (rec.tag == tag::ParaHeader && inMemo) {
                    Paragraph memoPara = parseParagraph(tree, child);
                    memoTexts_.back().second.paragraphs.push_back(std::move(memoPara));
                    break;
                }
                KeptRecord kept;
                kept.tag = rec.tag;
                kept.data.assign(rec.data.begin(), rec.data.end());
                para.kept.push_back(std::move(kept));
                break;
            }
            case tag::ParaLineSeg:
                if (options_.keepLineSegs) {
                    ByteReader r(rec.data);
                    while (r.remaining() >= kLineSegSize) {
                        LineSeg seg;
                        seg.textPos = r.u32();
                        seg.vertPos = r.i32();
                        seg.vertSize = r.i32();
                        seg.textHeight = r.i32();
                        seg.baseline = r.i32();
                        seg.spacing = r.i32();
                        seg.horzPos = r.i32();
                        seg.horzSize = r.i32();
                        seg.flags = r.u32();
                        para.lineSegs.push_back(seg);
                    }
                }
                break;
            case tag::CtrlHeader: controls.push_back(child); break;
        }
    }

    std::size_t shapeIndex = 0;
    auto shapeAt = [&](std::size_t pos) -> std::uint32_t {
        if (shapes.empty()) return 0;
        while (shapeIndex + 1 < shapes.size() && shapes[shapeIndex + 1].first <= pos) ++shapeIndex;
        return shapes[shapeIndex].second;
    };

    // 형광펜 종류. 하위 24비트는 0x00BBGGRR.
    constexpr std::uint32_t kRangeMarkPen = 2;
    auto markpenAt = [&](std::size_t pos) {
        std::string pen;
        for (const RangeTag& range : ranges) {
            if (range.kind != kRangeMarkPen || pos < range.start || pos >= range.end) continue;
            if (const Color color = fromColorRef(range.data)) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(*color & 0xFFFFFFu));
                pen = buf;
            }
        }
        return pen;
    };
    auto markpenOver = [&](std::size_t begin, std::size_t end) {
        for (std::size_t pos = begin; pos < end; ++pos) {
            const std::string pen = markpenAt(pos);
            if (!pen.empty()) return pen;
        }
        return std::string();
    };

    Run current;
    bool hasRun = false;
    struct MarkedChar {
        char16_t ch = 0;
        std::size_t pos = 0;
    };
    std::vector<MarkedChar> pending;
    auto flushText = [&] {
        std::u16string chunk;
        std::string pen;
        auto push = [&] {
            if (chunk.empty()) return;
            InlineItem item = InlineItem::makeText(utf16ToUtf8(chunk));
            item.markpen = pen;
            current.items.push_back(std::move(item));
            chunk.clear();
        };
        for (const MarkedChar& marked : pending) {
            const std::string here = markpenAt(marked.pos);
            if (!chunk.empty() && here != pen) push();
            pen = here;
            chunk.push_back(marked.ch);
        }
        push();
        pending.clear();
    };
    auto switchRun = [&](std::size_t pos) {
        const std::uint32_t id = shapeAt(pos);
        if (!hasRun) {
            current.charShapeId = id;
            hasRun = true;
        } else if (id != current.charShapeId) {
            flushText();
            para.runs.push_back(std::move(current));
            current = Run{};
            current.charShapeId = id;
        }
    };

    std::vector<std::uint32_t> controlIds;
    controlIds.reserve(controls.size());
    for (std::size_t ctrlIndex : controls) {
        ByteReader r(tree.at(ctrlIndex).record->data);
        controlIds.push_back(r.u32());
    }
    detail::ControlMatcher matcher(std::move(controlIds));

    const std::size_t count = text.size() / 2;
    std::size_t i = 0;
    while (i < count) {
        const char16_t ch = static_cast<char16_t>(readU16LE(text.data() + i * 2));
        switchRun(i);
        if (ch >= 32) {
            pending.push_back(MarkedChar{ch, i});
            ++i;
            continue;
        }
        switch (classifyControlChar(ch)) {
            case CharKind::Char:
                if (ch == 10) pending.push_back(MarkedChar{u'\n', i});
                else if (ch == 24) pending.push_back(MarkedChar{u'-', i});
                else if (ch == 30) pending.push_back(MarkedChar{u'\u00A0', i});
                else if (ch == 31) pending.push_back(MarkedChar{u'\u2007', i});
                i += 1;
                break;
            case CharKind::Inline:
                if (ch == 9) pending.push_back(MarkedChar{u'\t', i});
                if (ch == 4) {
                    // 필드 끝. 버리면 저장한 하이퍼링크가 범위를 잃는다.
                    flushText();
                    Control end;
                    end.kind = ControlKind::FieldEnd;
                    end.ctrlId = "fend";
                    current.items.push_back(InlineItem::makeControl(std::move(end)));
                }
                i += 8;
                break;
            case CharKind::Extended: {
                flushText();
                // 확장 제어 문자의 WCHAR 1~2 에 해당 컨트롤의 ctrlId 가 들어 있다.
                std::optional<std::uint32_t> ctrlId;
                if ((i + 3) * 2 <= text.size()) ctrlId = readU32LE(text.data() + (i + 1) * 2);
                if (const auto k = matcher.match(ctrlId)) {
                    InlineItem item = InlineItem::makeControl(parseControl(tree, controls[*k]));
                    // 한글은 메모 등 HWP 5.0 규격 뒤에 생긴 필드의 CTRL_HEADER 를 %unk 로 쓰고 글 쪽에 진짜 id 를 둔다.
                    // 종류는 글 쪽 id 다.
                    if (item.control.kind == ControlKind::Field && item.control.ctrlId == "%unk" && ctrlId) {
                        item.control.ctrlId = ctrlIdToString(*ctrlId);
                    }
                    if (!isMemoField(item.control) && proofKindOf(item.control) == ProofKind::None) {
                        item.control.memoNumber = 0;
                    }
                    item.markpen = markpenOver(i, i + 8);
                    current.items.push_back(std::move(item));
                } else {
                    ++orphanControlChars_;
                }
                i += 8;
                break;
            }
        }
    }
    flushText();
    if (!hasRun) current.charShapeId = shapeAt(0);
    for (std::size_t k : matcher.remaining()) {
        current.items.push_back(InlineItem::makeControl(parseControl(tree, controls[k])));
    }
    mismatchedControlChars_ += matcher.mismatches();
    para.runs.push_back(std::move(current));
    para.ranges.reserve(ranges.size());
    for (const RangeTag& range : ranges) {
        TextRange tag;
        tag.start = range.start;
        tag.end = range.end;
        tag.kind = range.kind;
        tag.data = range.data;
        para.ranges.push_back(tag);
    }
    return para;
}

Placement Parser::readPlacement(ByteReader& r) {
    static const char* vertRel[] = {"PAPER", "PAGE", "PARA", "PARA"};
    static const char* horzRel[] = {"PAPER", "PAGE", "COLUMN", "PARA"};
    static const char* wraps[] = {"SQUARE", "TOP_AND_BOTTOM", "BEHIND_TEXT", "IN_FRONT_OF_TEXT"};
    static const char* vertAligns[] = {"TOP", "CENTER", "BOTTOM", "INSIDE", "OUTSIDE"};
    static const char* horzAligns[] = {"LEFT", "CENTER", "RIGHT", "INSIDE", "OUTSIDE"};
    static const char* flows[] = {"BOTH_SIDES", "LEFT_ONLY", "RIGHT_ONLY", "LARGEST_ONLY"};

    Placement p;
    const std::uint32_t attr = r.u32();
    p.vertOffset = r.i32();
    p.horzOffset = r.i32();
    p.width = r.u32();
    p.height = r.u32();
    p.zOrder = r.i32();
    p.treatAsChar = (attr & 0x1) != 0;
    p.flowWithText = (attr & (1u << 13)) != 0;
    p.allowOverlap = (attr & (1u << 14)) != 0;
    p.relativeWidth = ((attr >> 15) & 0x7u) < 4u;
    p.relativeHeight = ((attr >> 18) & 0x3u) < 2u;
    p.vertRelTo = vertRel[(attr >> 3) & 0x3];
    p.horzRelTo = horzRel[(attr >> 8) & 0x3];
    const std::uint32_t vertAlign = (attr >> 5) & 0x7;
    const std::uint32_t horzAlign = (attr >> 10) & 0x7;
    p.vertAlign = vertAlign < std::size(vertAligns) ? vertAligns[vertAlign] : "TOP";
    p.horzAlign = horzAlign < std::size(horzAligns) ? horzAligns[horzAlign] : "LEFT";
    const std::uint32_t wrap = (attr >> 21) & 0x7;
    p.textWrap = wrap < std::size(wraps) ? wraps[wrap] : "SQUARE";
    p.textFlow = flows[(attr >> 24) & 0x3];
    static const char* numberings[] = {"NONE", "PICTURE", "TABLE", "EQUATION"};
    const std::uint32_t numbering = (attr >> 26) & 0x3;
    p.numberingType = numbering < std::size(numberings) ? numberings[numbering] : "NONE";
    p.attrKeep = attr & ~kObjectKnownBits;
    if (r.remaining() >= 8) {
        p.marginLeft = r.i16();
        p.marginRight = r.i16();
        p.marginTop = r.i16();
        p.marginBottom = r.i16();
    }
    return p;
}

NoteShape readNoteShape(ByteView data) {
    NoteShape shape;
    ByteReader r(data);
    if (r.remaining() < 4) return shape;
    const std::uint32_t attr = r.u32();
    shape.numFormat = numberShapeName(attr & 0xFF);
    shape.place = static_cast<int>((attr >> 8) & 0x3);
    shape.numbering = static_cast<int>((attr >> 10) & 0x3);
    shape.contentSuperscript = (attr & 0x1000) != 0;
    shape.beneathText = (attr & 0x2000) != 0;
    if (r.remaining() < 2) return shape;
    shape.userChar = oneWchar(r);
    if (r.remaining() < 2) return shape;
    shape.prefix = oneWchar(r);
    if (r.remaining() < 2) return shape;
    shape.suffix = oneWchar(r);
    if (r.remaining() < 2) return shape;
    shape.startNumber = r.u16();
    // 구분선 길이는 4바이트다. 0은 선 없음, 음수는 단의 1/3로 둔다.
    if (r.remaining() >= 4) shape.lineLength = r.i32();
    else if (r.remaining() >= 2) shape.lineLength = r.i16();
    if (r.remaining() >= 2) shape.above = r.i16();
    if (r.remaining() >= 2) shape.below = r.i16();
    if (r.remaining() >= 2) shape.between = r.i16();
    if (r.remaining() >= 1) {
        const unsigned kind = r.u8();
        shape.lineType = kind == 0 ? "NONE" : shapeLineType(kind);
    }
    if (r.remaining() >= 1) shape.lineWidth = borderWidthHu(r.u8());
    if (r.remaining() >= 4) {
        const std::string color = hexColor(fromColorRef(r.u32()));
        if (!color.empty() && color != "none") shape.lineColor = color;
    }
    return shape;
}

PageBorderFill readPageBorderFill(ByteView data) {
    PageBorderFill out;
    ByteReader r(data);
    if (r.remaining() < 4) return out;
    const std::uint32_t attr = r.u32();
    // 비트 0이 1이면 쪽이다. hwpx의 PAPER와 같은 그림이다.
    out.textBorder = (attr & 0x1) != 0;
    out.headerInside = (attr & 0x2) != 0;
    out.footerInside = (attr & 0x4) != 0;
    out.fillArea = static_cast<int>((attr >> 3) & 0x3);
    if (r.remaining() >= 2) out.left = r.i16();
    if (r.remaining() >= 2) out.right = r.i16();
    if (r.remaining() >= 2) out.top = r.i16();
    if (r.remaining() >= 2) out.bottom = r.i16();
    if (r.remaining() >= 2) out.borderFillId = r.u16();
    return out;
}

PageDef Parser::readPageDef(ByteView data) {
    ByteReader r(data);
    PageDef page;
    page.width = r.u32();
    page.height = r.u32();
    page.marginLeft = r.u32();
    page.marginRight = r.u32();
    page.marginTop = r.u32();
    page.marginBottom = r.u32();
    page.marginHeader = r.u32();
    page.marginFooter = r.u32();
    page.marginGutter = r.u32();
    const std::uint32_t attr = r.u32();
    page.landscape = (attr & 0x1) != 0;
    page.gutterType = static_cast<std::uint8_t>(std::min<std::uint32_t>((attr >> 1) & 0x3, 2));
    page.applyOrientation();
    return page;
}

Control Parser::parseControl(const RecordTree& tree, std::size_t index) {
    const auto& node = tree.at(index);
    ByteReader r(node.record->data);
    const std::uint32_t id = r.u32();

    Control c;
    c.ctrlId = ctrlIdToString(id);
    switch (id) {
        case kCtrlSecd:
            c.kind = ControlKind::SectionDef;
            if (r.remaining() >= 4) {
                const std::uint32_t attr = r.u32();
                // 비트 0·1·2·5 는 첫 쪽에만 감추기다(한글 2024: HWPX visibility hideFirst* 와 같은 값으로 저장).
                c.hideFirstHeader = (attr & 0x1) != 0;
                c.hideFirstFooter = (attr & 0x2) != 0;
                c.hideFirstMaster = (attr & 0x4) != 0;
                c.hideFirstPageNum = (attr & 0x20) != 0;
                c.borderShow = (attr & 0x8) ? "HIDE_FIRST" : ((attr & 0x100) ? "SHOW_FIRST" : "SHOW_ALL");
                c.fillShow = (attr & 0x10) ? "HIDE_FIRST" : ((attr & 0x200) ? "SHOW_FIRST" : "SHOW_ALL");
                c.hideFirstEmptyLine = (attr & (1u << 19)) != 0;
                c.lineNumbers.show = (attr & (1u << 24)) != 0;
                // 줄 번호: 47바이트 레코드의 38번째 바이트부터 restart, countBy, distance, start(숨긴 한글 2024 로 확인).
                if (node.record->data.size() >= 47) {
                    ByteReader ln(node.record->data);
                    ln.seek(38);
                    c.lineNumbers.restart = ln.u8();
                    c.lineNumbers.countBy = ln.u16();
                    c.lineNumbers.distance = ln.u32();
                    c.lineNumbers.start = ln.u16();
                }
                c.startsOn = static_cast<std::uint8_t>((attr >> 20) & 0x3);
                c.wongoji = (attr & (1u << 22)) != 0;
                c.textDirection = static_cast<std::uint8_t>(std::min<std::uint32_t>((attr >> 16) & 0x7, 2));
                c.sectionAttrKeep = attr & ~kSecdKnownBits;
                // 한글 2024 는 속성 비트 16–18 을 비우고, 47바이트 레코드의 36번째 바이트에 방향 × 2 를 둔다.
                // 가로 00, 세로(영문 눕힘) 02, 세로(영문 세움) 04 (한글 2024 로 확인).
                // 34번째 바이트(u16)는 구역 메모 모양 번호(1 부터, 0 기본)다(HWPX secPr@memoShapeIDRef 와 같다).
                if (node.record->data.size() >= 36) {
                    c.memoShapeId = static_cast<std::uint16_t>(node.record->data[34] | (node.record->data[35] << 8));
                }
                if (node.record->data.size() > 36) {
                    const unsigned ext = (node.record->data[36] >> 1) & 0x3u;
                    if (ext != 0 || c.textDirection == 0) c.textDirection = static_cast<std::uint8_t>(std::min(ext, 2u));
                }
                if (r.remaining() >= 10) {
                    const auto gap = r.u16();
                    if (gap > 0) c.columnRegionGap = gap;
                    c.lineGrid = r.u16();
                    c.charGrid = r.u16();
                    const std::uint32_t raw = r.u32();
                    c.defaultTab = raw > 0 ? static_cast<std::int32_t>(raw / 2) : 4000;
                    if (r.remaining() >= 4) {
                        r.u16();
                        c.number = r.u16();
                    }
                    if (r.remaining() >= 6) {
                        c.pictureStart = r.u16();
                        c.tableStart = r.u16();
                        c.equationStart = r.u16();
                    }
                }
            }
            {
                int noteShapeAt = 0;
                int pageBorderAt = 0;
                static const char* pageBorderTypes[] = {"BOTH", "EVEN", "ODD"};
                for (std::size_t child : node.children) {
                    const std::uint16_t childTag = tree.at(child).record->tag;
                    if (childTag == tag::PageDef) {
                        c.page = readPageDef(tree.at(child).record->data);
                    } else if (childTag == tag::FootnoteShape) {
                        const NoteShape shape = readNoteShape(tree.at(child).record->data);
                        if (noteShapeAt == 0) c.footnoteShape = shape;
                        else c.endnoteShape = shape;
                        ++noteShapeAt;
                    } else if (childTag == tag::PageBorderFill && pageBorderAt < 3) {
                        PageBorderFill border = readPageBorderFill(tree.at(child).record->data);
                        border.type = pageBorderTypes[pageBorderAt];
                        c.pageBorders[pageBorderAt] = std::move(border);
                        ++pageBorderAt;
                    }
                }
            }
            {
                std::uint32_t secdAttr = 0;
                if (node.record->data.size() >= 8) {
                    ByteReader head(node.record->data);
                    head.u32();
                    secdAttr = head.u32();
                }
                parseMasterPages(tree, node, c, secdAttr);
            }
            break;
        case kCtrlCold:
            c.kind = ControlKind::ColumnDef;
            if (r.remaining() >= 2) {
                const auto attr = r.u16();
                c.columnCount = (attr >> 2) & 0xFF;
                c.columnSameSize = ((attr >> 12) & 1) != 0;
                const int kind = attr & 0x3;
                // 단 종류 0 일반, 1 배분, 2 평행. 이름은 HWPX(OWPML)와 같다.
                c.columnType = kind == 2 ? "PARALLEL" : kind == 1 ? "BALANCED_NEWSPAPER" : "NEWSPAPER";
                const int dir = (attr >> 10) & 0x3;
                c.columnLayout = dir == 1 ? "RIGHT" : dir == 2 ? "BOTH" : "LEFT";
            }
            if (r.remaining() >= 2) c.columnGap = r.u16();
            // 너비가 다르면 단마다 폭이 붙는다. 간격은 위의 단 사이 간격 하나다.
            if (!c.columnSameSize && c.columnCount > 0) {
                for (std::uint32_t i = 0; i < c.columnCount && r.remaining() >= 2; ++i) {
                    ColumnWidth slice;
                    slice.width = static_cast<std::int32_t>(r.u16());
                    slice.gap = i + 1 < c.columnCount ? c.columnGap : 0;
                    c.columnWidths.push_back(slice);
                }
            }
            // 속성 상위 16비트 다음이 단 구분선이다. 종류 0은 선 없음.
            if (r.remaining() >= 2) r.u16();
            if (r.remaining() >= 6) {
                const unsigned kind = r.u8();
                const unsigned thick = r.u8();
                const Color color = fromColorRef(r.u32());
                if (kind != 0) {
                    c.columnLineType = shapeLineType(kind);
                    c.columnLineWidth = borderWidthHu(thick);
                    c.columnLineColor = hexColor(color);
                    if (c.columnLineColor == "none") c.columnLineColor = "#000000";
                }
            }
            break;
        case kCtrlTable:
            c.kind = ControlKind::Table;
            c.placement = readPlacement(r);
            parseCaption(tree, node, c);
            parseTable(tree, node, c);
            break;
        case kCtrlGso:
            c.kind = ControlKind::Shape;
            c.placement = readPlacement(r);
            parseCaption(tree, node, c);
            for (std::size_t child : node.children) {
                if (tree.at(child).record->tag == tag::ShapeComponent) {
                    parseShapeComponent(tree, child, c);
                    break;
                }
            }
            break;
        case kCtrlEquation:
            c.kind = ControlKind::Equation;
            c.placement = readPlacement(r);
            parseCaption(tree, node, c);
            for (std::size_t child : node.children) {
                if (tree.at(child).record->tag == tag::EqEdit) {
                    ByteReader e(tree.at(child).record->data);
                    e.u32();  // 속성
                    c.script = e.hwpString();
                    // 조판 상자는 공통 헤더 그대로다. 글자 크기는 측정에만 쓰고, 기준선 퍼센트는 줄 기준선에 쓴다.
                    if (e.remaining() >= 4) c.baseUnit = static_cast<std::int32_t>(e.u32());
                    if (e.remaining() >= 4) e.u32();  // 글자 색
                    if (e.remaining() >= 2) {
                        c.fileBaseLine = e.i16();
                        c.baseLine = c.fileBaseLine;
                    }
                    if (e.remaining() >= 2) e.u16();  // 미기재
                    if (e.remaining() >= 2) e.hwpString();  // 버전
                    if (e.remaining() >= 2) c.equationFont = e.hwpString();
                }
            }
            break;
        case kCtrlHeader:
        case kCtrlFooter: {
            static const char* pageTypes[] = {"BOTH", "EVEN", "ODD", "BOTH"};
            c.kind = id == kCtrlHeader ? ControlKind::Header : ControlKind::Footer;
            c.applyPageType = pageTypes[r.u32() & 0x3];
            collectSubLists(tree, node, c.subLists);
            {
                std::size_t listAt = 0;
                for (std::size_t child : node.children) {
                    const Record& rec = *tree.at(child).record;
                    if (rec.tag != tag::ListHeader || listAt >= c.subLists.size()) continue;
                    if (rec.data.size() >= 16) {
                        ByteReader box(rec.data);
                        box.seek(8);
                        c.subLists[listAt].textWidth = box.u32();
                        c.subLists[listAt].textHeight = box.u32();
                    }
                    ++listAt;
                }
            }
            break;
        }
        case kCtrlFootnote:
        case kCtrlEndnote:
            c.kind = id == kCtrlFootnote ? ControlKind::Footnote : ControlKind::Endnote;
            collectSubLists(tree, node, c.subLists);
            break;
        case kCtrlAutoNum:
        case kCtrlNewNum:
            c.kind = id == kCtrlAutoNum ? ControlKind::AutoNumber : ControlKind::NewNumber;
            {
                const std::uint32_t attr = r.u32();
                c.numType = numTypeName(attr & 0xF);
                c.numFormat = numberShapeName((attr >> 4) & 0xFF);
                c.number = r.u16();
                if (id == kCtrlAutoNum && r.remaining() >= 6) {
                    r.u16();
                    c.prefix = oneWchar(r);
                    c.suffix = oneWchar(r);
                }
            }
            break;
        case kCtrlPageHiding:
            c.kind = ControlKind::PageHiding;
            if (r.remaining() >= 4) {
                const std::uint32_t attr = r.u32();
                c.hideHeader = (attr & 0x01) != 0;
                c.hideFooter = (attr & 0x02) != 0;
                c.hideMaster = (attr & 0x04) != 0;
                c.hideBorder = (attr & 0x08) != 0;
                c.hideFill = (attr & 0x10) != 0;
                c.hidePageNum = (attr & 0x20) != 0;
            }
            break;
        case kCtrlPageNumPos:
            c.kind = ControlKind::PageNumPos;
            if (r.remaining() >= 4) {
                const std::uint32_t attr = r.u32();
                c.numFormat = numberShapeName(attr & 0xFF);
                static const char* places[] = {
                    "NONE",        "TOP_LEFT",      "TOP_CENTER",    "TOP_RIGHT",
                    "BOTTOM_LEFT", "BOTTOM_CENTER", "BOTTOM_RIGHT",  "OUTSIDE_TOP",
                    "OUTSIDE_BOTTOM", "INSIDE_TOP", "INSIDE_BOTTOM"};
                const unsigned place = (attr >> 8) & 0xF;
                c.pageNumPos = place < std::size(places) ? places[place] : "NONE";
                // 사용자 기호, 앞·뒤 장식 문자, 줄표(HWPX sideChar) 차례다. 한글 2024 는 앞의 셋을 0 으로 두고
                // 줄표 넣기를 켜면 넷째에 '-' 를 쓴다(한글 2024 로 확인). 줄표는 번호 앞뒤에 붙는다.
                if (r.remaining() >= 6) {
                    r.u16();
                    c.prefix = oneWchar(r);
                    c.suffix = oneWchar(r);
                    const std::string side = oneWchar(r);
                    if (!side.empty()) c.prefix = c.suffix = side;
                }
            }
            break;
        case kCtrlBookmark:
            c.kind = ControlKind::Bookmark;
            if (const Record* data = findChild(tree, node, tag::CtrlData)) {
                auto name = detail::findParameterString(data->data, detail::kParamItemName);
                if (!name) name = detail::findParameterString(data->data, std::nullopt);
                c.name = name.value_or("");
            }
            break;
        case kCtrlHiddenComment:
            c.kind = ControlKind::HiddenComment;
            collectSubLists(tree, node, c.subLists);
            break;
        case kCtrlDutmal:
            c.kind = ControlKind::Dutmal;
            c.text = r.hwpString();
            c.subText = r.hwpString();
            if (r.remaining() >= 20) {
                c.dutmalPos = dutmalPosName(r.u32());
                c.dutmalSizeRatio = r.u32();
                c.dutmalOption = r.u32();
                c.dutmalStyle = r.u32();
                c.dutmalAlign = dutmalAlignName(r.u32());
            }
            break;
        case kCtrlCompose:
            c.kind = ControlKind::Compose;
            c.text = r.hwpString();
            if (r.remaining() >= 4) {
                c.composeCircle = composeCircleName(r.u8());
                c.composeCharSize = r.i8();
                c.composeType = r.u8() == 0 ? "SPREAD" : "OVERLAP";
                const std::uint8_t count = r.u8();
                for (std::uint8_t i = 0; i < count && r.remaining() >= 4; ++i) c.composeCharShapes.push_back(r.u32());
            }
            break;
        default:
            if (c.ctrlId[0] == '%') {
                c.kind = ControlKind::Field;
                c.fieldProps = r.u32();  // 속성
                c.fieldExtra = r.u8();   // 기타 속성
                c.command = r.hwpString();
                // 명령 뒤 u32 필드 고유 id, 메모 필드는 그 뒤 u32 메모 번호(MEMO_LIST 짝).
                if (r.remaining() >= 4) c.fieldId = r.u32();
                if (r.remaining() >= 4) c.memoNumber = r.u32();
                if (const Record* data = findChild(tree, node, tag::CtrlData)) {
                    c.name = detail::findParameterString(data->data, detail::kParamItemName)
                                 .value_or("");
                }
            }
            collectSubLists(tree, node, c.subLists);
            break;
    }
    return c;
}

// 그림 개체 속성. 효과 플래그가 0일 때만 그 뒤 9바이트(최초 크기, 투명도)를 읽는다.
// 플래그 비트 배정이 없어 효과 덩어리 길이를 알 수 없다.
void readPicture(ByteView data, Control& control) {
    ByteReader p(data);
    if (p.remaining() < 73) {
        p.seek(kPictureBinIdOffset);
        const std::uint16_t binId = p.u16();
        if (p.ok()) control.binItemId = std::to_string(binId);
        return;
    }
    const Color border = fromColorRef(p.u32());
    const std::int32_t borderWidth = p.i32();
    const std::uint32_t borderAttr = p.u32();
    std::int32_t minX = 0;
    std::int32_t minY = 0;
    std::int32_t maxX = 0;
    std::int32_t maxY = 0;
    for (int i = 0; i < 4; ++i) {
        const std::int32_t x = p.i32();
        const std::int32_t y = p.i32();
        if (i == 0) {
            minX = maxX = x;
            minY = maxY = y;
        } else {
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    control.picture.rectWidth = maxX - minX;
    control.picture.rectHeight = maxY - minY;
    control.picture.clipLeft = p.i32();
    control.picture.clipTop = p.i32();
    control.picture.clipRight = p.i32();
    control.picture.clipBottom = p.i32();
    control.picture.padLeft = p.i16();
    control.picture.padRight = p.i16();
    control.picture.padTop = p.i16();
    control.picture.padBottom = p.i16();
    control.picture.bright = p.i8();
    control.picture.contrast = p.i8();
    control.picture.effect = p.u8();
    const std::uint16_t binId = p.u16();
    if (p.ok()) control.binItemId = std::to_string(binId);
    if (p.remaining() < 5) return;
    p.u8();  // 테두리 투명도
    p.u32();  // instance ID
    if (p.remaining() < 4) return;
    const std::uint32_t effectFlags = p.u32();
    if (effectFlags == 0 && p.remaining() >= 8) {
        control.picture.originWidth = p.i32();
        control.picture.originHeight = p.i32();
        if (p.remaining() >= 1) {
            const int transparency = p.i8();
            control.picture.transparency = transparency < 0 ? 0 : transparency;
        }
    }
    if (control.picture.originWidth <= 0 || control.picture.originHeight <= 0) {
        control.picture.originWidth = control.picture.rectWidth;
        control.picture.originHeight = control.picture.rectHeight;
    }
    const std::string line = shapeLineType(borderAttr & 0x3Fu);
    if (borderWidth > 0 && line != "NONE" && control.strokeType.empty()) {
        control.strokeWidth = borderWidth;
        control.strokeColor = hexColor(border);
        if (control.strokeColor == "none") control.strokeColor = "#000000";
        control.strokeType = line;
        control.strokeEnd = static_cast<int>((borderAttr >> 6) & 0xFu);
    }
}

void Parser::parseShapeComponent(const RecordTree& tree, std::size_t index, Control& control) {
    const auto& node = tree.at(index);
    ByteReader r(node.record->data);
    const std::uint32_t shapeId = r.u32();

    control.shapeType = shapeTypeFromId(shapeId);
    // 선·다각형·곡선의 점은 처음 크기 안의 좌표로 적혀 있다. 아래에서 현재 크기로 늘린다.
    // 묶음 배율이 0인 축은 상자 크기에서는 건너뛰지만 점은 그 축으로 납작해진다.
    // (교육과정 머리말의 선: 100x100 선을 가로 484.71배, 세로 0배로 그려 수평선이 된다.)
    std::uint32_t pointsW = 0;
    std::uint32_t pointsH = 0;
    bool flatX = false;
    bool flatY = false;
    if (control.shapeType == "pic") control.kind = ControlKind::Picture;
    else if (control.shapeType == "container") control.kind = ControlKind::Container;
    else if (control.shapeType == "ole") control.kind = ControlKind::Ole;
    else control.kind = ControlKind::Shape;

    // 묶음 자식은 쪽 배치가 없고, 그룹 안 오프셋과 현재 크기가 여기 있다.
    // GenShapeObject 는 id 를 한 번 더 적는다. 최상위는 항상 그렇고, 묶음 안에도 그렇게 남은 묶음이 있다.
    if (!control.placement) {
        if (r.remaining() >= 4) {
            const std::size_t at = r.position();
            if (r.u32() != shapeId) r.seek(at);
        }
    }
    if (!control.placement && r.remaining() >= 28) {
        const std::int32_t groupX = r.i32();
        const std::int32_t groupY = r.i32();
        r.u16();
        r.u16();
        const std::uint32_t initW = r.u32();
        const std::uint32_t initH = r.u32();
        const std::uint32_t curW = r.u32();
        const std::uint32_t curH = r.u32();
        double scaleX = 1;
        double scaleY = 1;
        std::vector<double> scaleXs{1.0};
        std::vector<double> scaleYs{1.0};
        // 배율·회전의 이동(e3, e6)은 offset에 더한다. transMatrix 는 offset과 같아서 다시 더하지 않는다.
        // 뒤 행렬부터 로컬 원점에 적용한다. 이 결과는 가장 바깥 묶음 기준이다.
        struct Affix {
            double e1 = 1;
            double e2 = 0;
            double e3 = 0;
            double e4 = 0;
            double e5 = 1;
            double e6 = 0;
        };
        std::vector<Affix> affixes;
        if (r.remaining() >= 16) {
            r.u32();
            r.u16();
            r.i32();
            r.i32();
            const std::uint16_t pairs = r.u16();
            if (r.remaining() >= 48) r.skip(48);
            auto readDouble = [&]() {
                const std::uint64_t bits = r.u64();
                double value = 0;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            };
            for (std::uint16_t i = 0; i < pairs && r.remaining() >= 96; ++i) {
                Affix scale;
                scale.e1 = readDouble();
                scale.e2 = readDouble();
                scale.e3 = readDouble();
                scale.e4 = readDouble();
                scale.e5 = readDouble();
                scale.e6 = readDouble();
                Affix rot;
                rot.e1 = readDouble();
                rot.e2 = readDouble();
                rot.e3 = readDouble();
                rot.e4 = readDouble();
                rot.e5 = readDouble();
                rot.e6 = readDouble();
                affixes.push_back(scale);
                affixes.push_back(rot);
                if (scale.e1 != 0) scaleX *= scale.e1;
                else flatX = true;
                if (scale.e5 != 0) scaleY *= scale.e5;
                else flatY = true;
                scaleXs.push_back(scaleX);
                scaleYs.push_back(scaleY);
            }
        }
        double shiftX = 0;
        double shiftY = 0;
        for (auto it = affixes.rbegin(); it != affixes.rend(); ++it) {
            const double nextX = it->e1 * shiftX + it->e2 * shiftY + it->e3;
            const double nextY = it->e4 * shiftX + it->e5 * shiftY + it->e6;
            shiftX = nextX;
            shiftY = nextY;
        }
        if (r.ok()) {
            Placement place;
            place.groupInRoot = true;
            place.matrixX = static_cast<std::int32_t>(std::llround(shiftX));
            place.matrixY = static_cast<std::int32_t>(std::llround(shiftY));
            place.scaleX = scaleX;
            place.scaleY = scaleY;
            place.groupX = static_cast<std::int32_t>(static_cast<std::int64_t>(groupX) + place.matrixX);
            place.groupY = static_cast<std::int32_t>(static_cast<std::int64_t>(groupY) + place.matrixY);
            const auto axis = [](std::uint32_t init, std::uint32_t cur, const std::vector<double>& prefixes) {
                const double scale = prefixes.empty() ? 1.0 : prefixes.back();
                const double base = init > 0 ? static_cast<double>(init) : static_cast<double>(cur);
                const auto scaled = static_cast<std::uint32_t>(std::llround(base * scale));
                if (cur > 0 && cur != init && std::fabs(scale - 1.0) <= 1e-9) return cur;
                if (!(cur > 0 && cur != init)) return scaled;
                const auto delta = cur > scaled ? cur - scaled : scaled - cur;
                if (delta <= 2) return cur;
                if (init == 0) return cur;
                for (std::size_t i = 0; i + 1 < prefixes.size(); ++i) {
                    const auto partial = static_cast<std::uint32_t>(std::llround(static_cast<double>(init) * prefixes[i]));
                    const auto gap = cur > partial ? cur - partial : partial - cur;
                    if (gap <= 2) return scaled;
                }
                return cur;
            };
            place.width = axis(initW, curW, scaleXs);
            place.height = axis(initH, curH, scaleYs);
            control.placement = place;
            pointsW = initW;
            pointsH = initH;
            readShapeInk(r, control);
        }
    } else if (control.placement) {
        if (r.remaining() >= 4) r.u32();
        double scaleX = 1;
        double scaleY = 1;
        double shiftX = 0;
        double shiftY = 0;
        std::uint32_t curW = 0;
        std::uint32_t curH = 0;
        skipShapeHeader(r, scaleX, scaleY, shiftX, shiftY, pointsW, pointsH, curW, curH);
        if (control.placement->relativeWidth && curW > 0) control.placement->width = curW;
        if (control.placement->relativeHeight && curH > 0) control.placement->height = curH;
        control.placement->scaleX = scaleX;
        control.placement->scaleY = scaleY;
        control.placement->matrixX = static_cast<std::int32_t>(std::llround(shiftX));
        control.placement->matrixY = static_cast<std::int32_t>(std::llround(shiftY));
        if (shapeHasInk(control.shapeType)) readShapeInk(r, control);
    }

    auto keepRecord = [&](std::uint16_t tagId, ByteView data) {
        KeptRecord kept;
        kept.tag = tagId;
        kept.data.assign(data.begin(), data.end());
        control.kept.push_back(std::move(kept));
    };
    // x 다음 y 순서로 읽는다. 두 읽기를 한 호출의 인자로 쓰면 컴파일러마다 순서가 달라진다(MSVC 는 y 먼저).
    auto pushPoint = [&](ByteReader& r) {
        const std::int32_t x = r.i32();
        const std::int32_t y = r.i32();
        control.points.push_back(ShapePoint{x, y});
    };
    auto readPointPairs = [&](ByteReader& r, int count) {
        if (count <= 0 || count > 100000) return;
        if (r.remaining() < static_cast<std::size_t>(count) * 8u) return;
        for (int i = 0; i < count; ++i) pushPoint(r);
    };
    for (std::size_t child : node.children) {
        const Record& rec = *tree.at(child).record;
        if (rec.tag == tag::ShapeComponentRectangle && !rec.data.empty()) {
            control.cornerRatio = static_cast<int>(rec.data[0]);
        } else if (rec.tag == tag::ShapeComponentPicture) {
            readPicture(rec.data, control);
        } else if (rec.tag == tag::ShapeComponentLine || rec.tag == tag::ShapeComponentEllipse ||
                   rec.tag == tag::ShapeComponentArc || rec.tag == tag::ShapeComponentPolygon ||
                   rec.tag == tag::ShapeComponentCurve || rec.tag == tag::ShapeComponentOle ||
                   rec.tag == tag::ShapeComponentTextArt || rec.tag == tag::FormObject ||
                   rec.tag == tag::ChartData || rec.tag == tag::VideoData ||
                   rec.tag == tag::ShapeComponentUnknown) {
            keepRecord(rec.tag, rec.data);
            ByteReader shape(rec.data);
            if (rec.tag == tag::ShapeComponentLine && shape.remaining() >= 16) {
                pushPoint(shape);
                pushPoint(shape);
            } else if (rec.tag == tag::ShapeComponentEllipse || rec.tag == tag::ShapeComponentArc) {
                if (shape.remaining() >= 4) shape.u32();
                const int pairs = rec.tag == tag::ShapeComponentArc ? 3 : 7;
                for (int i = 0; i < pairs && shape.remaining() >= 8; ++i) pushPoint(shape);
            } else if (rec.tag == tag::ShapeComponentPolygon || rec.tag == tag::ShapeComponentCurve) {
                if (shape.remaining() >= 4) {
                    const std::int32_t count = shape.i32();
                    readPointPairs(shape, count);
                }
            }
        } else if (rec.tag == tag::ShapeComponent) {
            Control sub;
            sub.ctrlId = "gso ";
            parseShapeComponent(tree, child, sub);
            control.children.push_back(std::move(sub));
        }
    }
    // 모델의 점은 hwpx 와 같이 현재 크기 안의 좌표다(hwpx 의 orgSz 가 처음 크기).
    if (control.placement && !control.points.empty()) {
        const double stretchX = flatX ? 0.0
                                : pointsW > 0 && control.placement->width > 0
                                    ? static_cast<double>(control.placement->width) / static_cast<double>(pointsW)
                                    : 1.0;
        const double stretchY = flatY ? 0.0
                                : pointsH > 0 && control.placement->height > 0
                                    ? static_cast<double>(control.placement->height) / static_cast<double>(pointsH)
                                    : 1.0;
        if (std::fabs(stretchX - 1.0) > 1e-9 || std::fabs(stretchY - 1.0) > 1e-9) {
            for (ShapePoint& point : control.points) {
                point.x = static_cast<std::int32_t>(std::llround(point.x * stretchX));
                point.y = static_cast<std::int32_t>(std::llround(point.y * stretchY));
            }
        }
    }
    for (std::size_t child : node.children) {
        const Record& rec = *tree.at(child).record;
        if (rec.tag != tag::ListHeader || rec.data.size() < 16) continue;
        ByteReader box(rec.data);
        box.seek(8);
        control.boxPadLeft = box.i16();
        control.boxPadRight = box.i16();
        control.boxPadTop = box.i16();
        control.boxPadBottom = box.i16();
        if (rec.data.size() >= 20) control.boxTextWidth = box.u32();
        break;
    }
    collectSubLists(tree, node, control.subLists);
}

void Parser::parseCaption(const RecordTree& tree, const RecordTree::Node& node, Control& control) {
    // 캡션은 본체 레코드(TABLE, SHAPE_COMPONENT, EQEDIT)보다 앞에 오는 LIST_HEADER 와
    // 그 뒤를 잇는 형제 PARA_HEADER 들이다.
    for (std::size_t child : node.children) {
        const auto& childNode = tree.at(child);
        const std::uint16_t t = childNode.record->tag;
        if (t == tag::Table || t == tag::ShapeComponent || t == tag::EqEdit) break;
        if (t == tag::ListHeader) {
            if (control.caption) break;
            control.caption = detail::readCaptionHeader(childNode.record->data);
            for (std::size_t grand : childNode.children) {
                if (tree.at(grand).record->tag == tag::ParaHeader) {
                    control.caption->content.paragraphs.push_back(parseParagraph(tree, grand));
                }
            }
        } else if (t == tag::ParaHeader && control.caption) {
            control.caption->content.paragraphs.push_back(parseParagraph(tree, child));
        }
    }
}

void Parser::parseTable(const RecordTree& tree, const RecordTree::Node& node, Control& control) {
    Table table;
    bool seenTable = false;  // TABLE 앞의 LIST_HEADER 는 캡션이다
    for (std::size_t child : node.children) {
        const auto& childNode = tree.at(child);
        const Record& rec = *childNode.record;
        if (rec.tag == tag::Table) {
            seenTable = true;
            ByteReader r(rec.data);
            const std::uint32_t attr = r.u32();
            table.rowCount = r.u16();
            table.colCount = r.u16();
            table.cellSpacing = r.u16();
            table.marginLeft = r.u16();
            table.marginRight = r.u16();
            table.marginTop = r.u16();
            table.marginBottom = r.u16();
            r.skip(static_cast<std::size_t>(table.rowCount) * 2);
            table.borderFillId = r.u16();
            const unsigned breakKind = attr & 0x3u;
            table.pageBreak = breakKind == 0 ? "NONE" : (breakKind == 1 ? "TABLE" : "CELL");
            table.repeatHeader = ((attr >> 2) & 0x1u) != 0;
            table.noAdjust = ((attr >> 4) & 0x1u) != 0;
        } else if (!seenTable) {
            continue;
        } else if (rec.tag == tag::ListHeader) {
            ByteReader r(rec.data);
            r.seek(4);
            const std::uint32_t listAttr = r.u32();
            TableCell cell;
            const unsigned valign = (listAttr >> 5) & 0x3;
            if (valign == 1) cell.vertAlign = "CENTER";
            else if (valign == 2) cell.vertAlign = "BOTTOM";
            cell.content.textDirection = static_cast<std::uint8_t>(std::min<std::uint32_t>(listAttr & 0x7, 2));
            cell.hasMargin = (listAttr & (1u << 16)) != 0;
            cell.header = (listAttr & (1u << 18)) != 0;
            cell.col = r.u16();
            cell.row = r.u16();
            cell.colSpan = r.u16();
            cell.rowSpan = r.u16();
            cell.width = r.u32();
            cell.height = r.u32();
            cell.marginLeft = r.u16();
            cell.marginRight = r.u16();
            cell.marginTop = r.u16();
            cell.marginBottom = r.u16();
            cell.borderFillId = r.u16();
            // 셀 필드 이름: 글 폭(u32) 뒤 바이트가 0 이 아니면 파라미터 셋(ID 0x021b)이 오고, 아이템 0x4000 문자열이
            // 이름이다. 이름이 없으면 한글은 0 만 쓴다(숨긴 한글 2024 로 저장해 확인).
            if (r.remaining() >= 5) {
                r.skip(4);
                if (r.u8() != 0) {
                    if (auto name = detail::findParameterString(ByteView(rec.data).subspan(r.position()), kCellNameItem)) {
                        cell.name = std::move(*name);
                    }
                }
            }
            for (std::size_t grand : childNode.children) {
                if (tree.at(grand).record->tag == tag::ParaHeader) {
                    cell.content.paragraphs.push_back(parseParagraph(tree, grand));
                }
            }
            table.cells.push_back(std::move(cell));
        } else if (rec.tag == tag::ParaHeader && !table.cells.empty()) {
            table.cells.back().content.paragraphs.push_back(parseParagraph(tree, child));
        }
    }
    control.table = std::move(table);
}

// 바탕쪽 리스트 머리(34바이트, 숨긴 한글 2024 로 저장해 확인): 문단 수, 속성, 글 너비, 글 높이, u16 0,
// u32 종류(0 양쪽·홀수·짝수, 3 구역 마지막 쪽, 3+n 구역 안 n 번째 쪽), u32 비트(1 겹치게 하기, 2 인쇄 시 맨 위).
SubList Parser::readMasterList(const RecordTree& tree, const RecordTree::Node& node) {
    SubList list;
    ByteReader header(node.record->data);
    if (header.remaining() >= 8) {
        header.u32();
        const std::uint32_t attr = header.u32();
        const unsigned valign = (attr >> 5) & 0x3;
        if (valign == 1) list.vertAlign = "CENTER";
        else if (valign == 2) list.vertAlign = "BOTTOM";
        static const char* pageTypes[] = {"BOTH", "EVEN", "ODD", "BOTH"};
        list.applyPageType = pageTypes[attr & 0x3];
        if (header.remaining() >= 8) {
            list.textWidth = header.u32();
            list.textHeight = header.u32();
        }
        if (header.remaining() >= 10) {
            header.u16();
            const std::uint32_t code = header.u32();
            const std::uint32_t bits = header.u32();
            if (code == 3) {
                list.applyPageType = "LAST_PAGE";
            } else if (code > 3) {
                list.applyPageType = "OPTIONAL_PAGE";
                list.masterPageNumber = code - 3;
            }
            list.masterDuplicate = (bits & 0x1u) != 0;
            list.masterFront = (bits & 0x2u) != 0;
        }
    }
    for (std::size_t grand : node.children) {
        if (tree.at(grand).record->tag == tag::ParaHeader) list.paragraphs.push_back(parseParagraph(tree, grand));
    }
    return list;
}

// 구역 정의 아래 바탕쪽(양쪽·짝수·홀수). 종류는 구역 정의 속성 비트 29·30·31 이 있는 차례다. 비트 수가 리스트 수와
// 다르면(옛 저장) 리스트 머리 속성 아래 두 비트를 쓴다.
void Parser::parseMasterPages(const RecordTree& tree, const RecordTree::Node& node, Control& control,
                              std::uint32_t secdAttr) {
    std::vector<SubList> lists;
    for (std::size_t child : node.children) {
        const auto& childNode = tree.at(child);
        const std::uint16_t t = childNode.record->tag;
        if (t == tag::ListHeader) {
            lists.push_back(readMasterList(tree, childNode));
        } else if (t == tag::ParaHeader) {
            if (lists.empty()) {
                SubList list;
                list.applyPageType = "BOTH";
                lists.push_back(std::move(list));
            }
            lists.back().paragraphs.push_back(parseParagraph(tree, child));
        }
    }
    std::vector<const char*> kinds;
    if (secdAttr & (1u << 29)) kinds.push_back("BOTH");
    if (secdAttr & (1u << 30)) kinds.push_back("EVEN");
    if (secdAttr & (1u << 31)) kinds.push_back("ODD");
    if (kinds.size() == lists.size()) {
        for (std::size_t i = 0; i < lists.size(); ++i) lists[i].applyPageType = kinds[i];
    }
    for (SubList& list : lists) {
        if (list.applyPageType.empty()) list.applyPageType = "BOTH";
        control.subLists.push_back(std::move(list));
    }
}

void Parser::collectSubLists(const RecordTree& tree, const RecordTree::Node& node,
                             std::vector<SubList>& out) {
    for (std::size_t child : node.children) {
        const auto& childNode = tree.at(child);
        const std::uint16_t t = childNode.record->tag;
        if (t == tag::ListHeader) {
            out.emplace_back();
            if (childNode.record->data.size() >= 8) {
                ByteReader header(childNode.record->data);
                header.seek(4);
                const std::uint32_t listAttr = header.u32();
                const unsigned valign = (listAttr >> 5) & 0x3;
                if (valign == 1) out.back().vertAlign = "CENTER";
                else if (valign == 2) out.back().vertAlign = "BOTTOM";
                out.back().textDirection = static_cast<std::uint8_t>(std::min<std::uint32_t>(listAttr & 0x7, 2));
            }
            for (std::size_t grand : childNode.children) {
                if (tree.at(grand).record->tag == tag::ParaHeader) {
                    out.back().paragraphs.push_back(parseParagraph(tree, grand));
                }
            }
        } else if (t == tag::ParaHeader) {
            if (out.empty()) out.emplace_back();
            out.back().paragraphs.push_back(parseParagraph(tree, child));
        }
    }
}

// ---------------------------------------------------------------------------
// 요약 정보 / 미리보기 / 바이너리 데이터
// ---------------------------------------------------------------------------

void Parser::parseSummaryInformation() {
    Bytes stream;
    if (!cfb_.readStream("\x05HwpSummaryInformation", stream)) return;

    // OLE Property Set: 헤더(28) + FMTID(16) + 오프셋(4)
    ByteReader r(stream);
    r.seek(24);
    if (r.u32() == 0) return;
    r.skip(16);
    const std::uint32_t sectionOffset = r.u32();
    r.seek(sectionOffset);
    r.u32();  // 섹션 크기
    const std::uint32_t propCount = r.u32();
    if (!r.ok() || propCount > 1024) return;

    std::vector<std::pair<std::uint32_t, std::uint32_t>> props;
    for (std::uint32_t i = 0; i < propCount && r.ok(); ++i) {
        const std::uint32_t pid = r.u32();
        props.emplace_back(pid, r.u32());
    }

    constexpr std::uint32_t kPidCodePage = 1;
    constexpr std::uint16_t kCodePageUtf16 = 1200;
    constexpr std::uint16_t kCodePageUtf8 = 65001;
    std::uint16_t codePage = 0;
    for (const auto& [pid, offset] : props) {
        if (pid != kPidCodePage) continue;
        ByteReader v(stream);
        v.seek(sectionOffset + offset);
        if (v.u32() == 0x02) codePage = v.u16();  // VT_I2
    }

    Metadata& m = doc_.metadata;
    bool skippedCodePageString = false;
    for (const auto& [pid, offset] : props) {
        ByteReader v(stream);
        v.seek(sectionOffset + offset);
        const std::uint32_t type = v.u32();
        std::string value;
        if (type == 0x1F) {  // VT_LPWSTR (실제 한글 파일)
            std::u16string s = v.utf16(v.u32());
            while (!s.empty() && s.back() == u'\0') s.pop_back();
            value = utf16ToUtf8(s);
        } else if (type == 0x1E) {  // VT_LPSTR (규격 표기): 길이는 바이트 수
            const std::uint32_t length = v.u32();
            if (codePage == kCodePageUtf16) {
                std::u16string s = v.utf16(length / 2);
                while (!s.empty() && s.back() == u'\0') s.pop_back();
                value = utf16ToUtf8(s);
            } else {
                const ByteView raw = v.bytes(length);
                value.assign(raw.begin(), raw.end());
                while (!value.empty() && value.back() == '\0') value.pop_back();
                // CP949 등은 변환표 없이 디코딩할 수 없으므로 UTF-8/ASCII 만 받는다.
                const bool ascii = std::all_of(value.begin(), value.end(), [](char ch) {
                    return static_cast<unsigned char>(ch) < 0x80;
                });
                if (codePage != kCodePageUtf8 && !ascii) {
                    skippedCodePageString = true;
                    continue;
                }
            }
        } else if (type == 0x40) {  // VT_FILETIME
            value = chwp::detail::fileTimeToIso8601(v.u64());
        } else {
            continue;
        }
        if (!v.ok()) continue;
        switch (pid) {
            case 2: m.title = value; break;
            case 3: m.subject = value; break;
            case 4: m.author = value; break;
            case 5: m.keywords = value; break;
            case 6: m.comments = value; break;
            case 8: m.lastSavedBy = value; break;
            case 12: m.createdAt = value; break;
            case 13: m.modifiedAt = value; break;
            default: break;
        }
    }
    if (skippedCodePageString) {
        warn("summary information: strings in code page " + std::to_string(codePage) +
             " are not supported");
    }
}

void Parser::parsePreviewText() {
    Bytes stream;
    if (!cfb_.readStream("PrvText", stream)) return;
    ByteReader r(stream);
    std::u16string text = r.utf16(stream.size() / 2);
    while (!text.empty() && text.back() == u'\0') text.pop_back();
    doc_.previewText = utf16ToUtf8(text);
}

void Parser::loadBinData() {
    auto& items = doc_.header.binItems;
    for (std::size_t i = 0; i < items.size() && i < binStorage_.size(); ++i) {
        const BinStorage& storage = binStorage_[i];
        if (storage.streamPath.empty()) continue;
        const cfb::Entry* entry = findEntry(storage.streamPath, true);
        if (!entry) {
            warn("missing bin data stream: " + storage.streamPath);
            continue;
        }
        // OLE 개체(STORAGE)는 하위 스트림을 가진 스토리지라 한 스트림으로 읽을 수 없다.
        if (entry->type == cfb::EntryType::Storage) continue;
        items[i].size = entry->size;
        if (!options_.loadBinData) continue;

        Bytes raw;
        if (!cfb_.readStream(*entry, raw)) {
            warn("failed to read bin data: " + storage.streamPath);
            continue;
        }
        const bool compressed = storage.compression == 1 ||
                                (storage.compression == 0 && doc_.format.compressed);
        if (compressed) {
            Bytes inflated;
            if (decompressStream(raw, inflated)) {
                raw = std::move(inflated);
            } else {
                warn("failed to decompress bin data: " + storage.streamPath);
            }
        }
        items[i].size = raw.size();
        items[i].data = std::move(raw);
    }
}

}  // namespace

ParseResult parse(ByteView data, const ParseOptions& options) {
    ParseResult result;
    Parser parser(data, options, result);
    parser.run();
    return result;
}

}  // namespace chwp::hwp
