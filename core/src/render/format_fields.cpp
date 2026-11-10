#include "chwp/render/paint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "render/paint_detail.hpp"
#include "render/painter.hpp"
#include "render/scene_detail.hpp"

// 글자 모양·문단 모양 대화 상자가 읽고 쓰는 값의 이름표.
// 셸은 이름과 값을 그대로 넘기고, 이름·단위·고치는 법은 여기 한 곳에서 정한다.

namespace chwp::render {
using namespace paint_detail;

namespace {

const double* numberOf(const FormatValue& value) {
    return std::get_if<double>(&value);
}

std::int32_t intOf(const FormatValue& value, std::int32_t lo, std::int32_t hi) {
    const double* n = numberOf(value);
    const double v = n ? *n : 0.0;
    return static_cast<std::int32_t>(std::clamp<long long>(std::llround(v), lo, hi));
}

std::string hexOfColor(const model::Color& color) {
    if (!color) return {};
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06x", static_cast<unsigned>(*color & 0xFFFFFFu));
    return buf;
}

// 빈 문자열은 색 없음이다.
model::Color colorOf(const FormatValue& value) {
    const std::string* text = std::get_if<std::string>(&value);
    if (!text || text->empty()) return std::nullopt;
    std::uint32_t rgb = 0;
    if (!parseHexColorRgb(*text, rgb)) return std::nullopt;
    return rgb;
}

// 밑줄·취소선 모양. 저장값은 이 목록 번호에서 1을 뺀 값이다(0 이 실선).
constexpr const char* kLineShapes[] = {"NONE",       "SOLID",       "DASH",          "DOT",
                                       "DASH_DOT",   "DASH_DOT_DOT", "LONG_DASH",     "CIRCLE",
                                       "DOUBLE_SLIM", "SLIM_THICK",  "THICK_SLIM",    "SLIM_THICK_SLIM",
                                       "WAVE",       "DOUBLE_WAVE",  "THICK_3D",      "THICK_3D_REVERSE_LIGHTING",
                                       "3D",         "3D_REVERSE_LIGHTING"};

void setLineShape(std::string& name, std::uint8_t& id, const FormatValue& value) {
    const std::string* text = std::get_if<std::string>(&value);
    if (!text) return;
    for (unsigned i = 1; i < std::size(kLineShapes); ++i) {
        if (*text == kLineShapes[i]) {
            name = *text;
            id = static_cast<std::uint8_t>(i - 1);
            return;
        }
    }
}

template <typename T>
void fillLang(model::PerLang<T>& values, T v) {
    values.fill(v);
}

struct CharField {
    const char* key;
    std::function<FormatValue(const model::CharShape&)> read;
    std::function<void(model::CharShape&, const FormatValue&)> write;
};

struct ParaField {
    const char* key;
    std::function<FormatValue(const model::ParaShape&)> read;
    std::function<void(model::ParaShape&, const FormatValue&)> write;
};

#define CHWP_CHAR_BOOL(name, member)                                                        \
    CharField {                                                                               \
        name, [](const model::CharShape& c) { return FormatValue{c.member}; },                \
            [](model::CharShape& c, const FormatValue& v) {                                   \
                if (const bool* b = std::get_if<bool>(&v)) c.member = *b;                     \
            }                                                                                 \
    }
#define CHWP_CHAR_LANG(name, member, lo, hi, zeroAs)                                                 \
    CharField {                                                                                        \
        name, [](const model::CharShape& c) {                                                          \
            return FormatValue{static_cast<double>(c.member[0] != 0 ? c.member[0] : zeroAs)};          \
        },                                                                                             \
            [](model::CharShape& c, const FormatValue& v) {                                            \
                if (numberOf(v)) fillLang(c.member, intOf(v, lo, hi));                                 \
            }                                                                                          \
    }
#define CHWP_CHAR_COLOR(name, member)                                                     \
    CharField {                                                                             \
        name, [](const model::CharShape& c) { return FormatValue{hexOfColor(c.member)}; },  \
            [](model::CharShape& c, const FormatValue& v) { c.member = colorOf(v); }        \
    }
#define CHWP_PARA_BOOL(name, member)                                                        \
    ParaField {                                                                               \
        name, [](const model::ParaShape& p) { return FormatValue{p.member}; },                \
            [](model::ParaShape& p, const FormatValue& v) {                                   \
                if (const bool* b = std::get_if<bool>(&v)) p.member = *b;                     \
            }                                                                                 \
    }
#define CHWP_PARA_INT(name, member, lo, hi)                                                           \
    ParaField {                                                                                         \
        name, [](const model::ParaShape& p) { return FormatValue{static_cast<double>(p.member)}; },     \
            [](model::ParaShape& p, const FormatValue& v) {                                             \
                if (numberOf(v)) p.member = intOf(v, lo, hi);                                           \
            }                                                                                           \
    }
#define CHWP_PARA_TEXT(name, member)                                                        \
    ParaField {                                                                               \
        name, [](const model::ParaShape& p) { return FormatValue{p.member}; },                \
            [](model::ParaShape& p, const FormatValue& v) {                                   \
                if (const std::string* t = std::get_if<std::string>(&v); t && !t->empty()) {  \
                    p.member = *t;                                                            \
                }                                                                             \
            }                                                                                 \
    }

// 글자 모양. 언어별 값은 대표 언어(한글) 값을 보여 주고, 고치면 모든 언어에 같은 값을 넣는다.
const std::vector<CharField>& charFields() {
    static const std::vector<CharField> fields = {
        CHWP_CHAR_LANG("ratio", ratio, 50, 200, 100),
        CHWP_CHAR_LANG("spacing", spacing, -50, 50, 0),
        CHWP_CHAR_LANG("relSize", relSize, 10, 250, 100),
        CHWP_CHAR_LANG("charOffset", offset, -100, 100, 0),
        CHWP_CHAR_COLOR("shadeColor", shadeColor),
        CharField{"underlineShape", [](const model::CharShape& c) { return FormatValue{c.underlineShape}; },
                  [](model::CharShape& c, const FormatValue& v) { setLineShape(c.underlineShape, c.underlineShapeId, v); }},
        CHWP_CHAR_COLOR("underlineColor", underlineColor),
        CharField{"strikeShape", [](const model::CharShape& c) { return FormatValue{c.strikeShape}; },
                  [](model::CharShape& c, const FormatValue& v) { setLineShape(c.strikeShape, c.strikeShapeId, v); }},
        CHWP_CHAR_COLOR("strikeColor", strikeColor),
        CharField{"outline", [](const model::CharShape& c) { return FormatValue{static_cast<double>(c.outline)}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (numberOf(v)) c.outline = static_cast<std::uint8_t>(intOf(v, 0, 6));
                  }},
        CharField{"shadow", [](const model::CharShape& c) { return FormatValue{static_cast<double>(c.shadow)}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (numberOf(v)) c.shadow = static_cast<std::uint8_t>(intOf(v, 0, 2));
                  }},
        CharField{"shadowOffsetX",
                  [](const model::CharShape& c) { return FormatValue{static_cast<double>(c.shadowOffsetX)}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (numberOf(v)) c.shadowOffsetX = static_cast<std::int8_t>(intOf(v, -100, 100));
                  }},
        CharField{"shadowOffsetY",
                  [](const model::CharShape& c) { return FormatValue{static_cast<double>(c.shadowOffsetY)}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (numberOf(v)) c.shadowOffsetY = static_cast<std::int8_t>(intOf(v, -100, 100));
                  }},
        CHWP_CHAR_COLOR("shadowColor", shadowColor),
        // 양각과 음각은 같이 켜지 않는다. 위·아래 첨자도 그렇다.
        CharField{"emboss", [](const model::CharShape& c) { return FormatValue{c.emboss}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (const bool* b = std::get_if<bool>(&v)) {
                          c.emboss = *b;
                          if (*b) c.engrave = false;
                      }
                  }},
        CharField{"engrave", [](const model::CharShape& c) { return FormatValue{c.engrave}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (const bool* b = std::get_if<bool>(&v)) {
                          c.engrave = *b;
                          if (*b) c.emboss = false;
                      }
                  }},
        CharField{"emphasis", [](const model::CharShape& c) { return FormatValue{static_cast<double>(c.emphasis)}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (numberOf(v)) c.emphasis = static_cast<std::uint8_t>(intOf(v, 0, 6));
                  }},
        CharField{"superscript", [](const model::CharShape& c) { return FormatValue{c.superscript}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (const bool* b = std::get_if<bool>(&v)) {
                          c.superscript = *b;
                          if (*b) c.subscript = false;
                      }
                  }},
        CharField{"subscript", [](const model::CharShape& c) { return FormatValue{c.subscript}; },
                  [](model::CharShape& c, const FormatValue& v) {
                      if (const bool* b = std::get_if<bool>(&v)) {
                          c.subscript = *b;
                          if (*b) c.superscript = false;
                      }
                  }},
        CHWP_CHAR_BOOL("useFontSpace", useFontSpace),
        CHWP_CHAR_BOOL("useKerning", useKerning),
    };
    return fields;
}

// 문단 모양. 여백·간격은 HWPUNIT 이다.
const std::vector<ParaField>& paraFields() {
    static const std::vector<ParaField> fields = {
        CHWP_PARA_TEXT("vertAlign", vertAlign),
        CHWP_PARA_INT("indent", indent, -1000000, 1000000),
        CHWP_PARA_INT("marginLeft", marginLeft, -1000000, 1000000),
        CHWP_PARA_INT("marginRight", marginRight, -1000000, 1000000),
        CHWP_PARA_INT("spacingPrev", spacingPrev, -1000000, 1000000),
        CHWP_PARA_INT("spacingNext", spacingNext, -1000000, 1000000),
        CHWP_PARA_TEXT("breakLatinWord", breakLatinWord),
        CHWP_PARA_TEXT("breakNonLatinWord", breakNonLatinWord),
        CHWP_PARA_INT("condense", condense, 0, 75),
        CHWP_PARA_BOOL("widowOrphan", widowOrphan),
        CHWP_PARA_BOOL("keepWithNext", keepWithNext),
        CHWP_PARA_BOOL("keepLines", keepLines),
        CHWP_PARA_BOOL("pageBreakBefore", pageBreakBefore),
        CHWP_PARA_BOOL("snapToGrid", snapToGrid),
        CHWP_PARA_BOOL("fontLineHeight", fontLineHeight),
        CHWP_PARA_BOOL("autoSpacingEng", autoSpacingEng),
        CHWP_PARA_BOOL("autoSpacingNum", autoSpacingNum),
        CHWP_PARA_BOOL("suppressLineNumbers", suppressLineNumbers),
        CHWP_PARA_TEXT("lineWrap", lineWrap),
        CHWP_PARA_INT("borderOffsetLeft", borderOffsetLeft, 0, 1000000),
        CHWP_PARA_INT("borderOffsetRight", borderOffsetRight, 0, 1000000),
        CHWP_PARA_INT("borderOffsetTop", borderOffsetTop, 0, 1000000),
        CHWP_PARA_INT("borderOffsetBottom", borderOffsetBottom, 0, 1000000),
        CHWP_PARA_BOOL("borderConnect", borderConnect),
        CHWP_PARA_BOOL("borderIgnoreMargin", borderIgnoreMargin),
        // 종류는 정의 번호까지 같이 정해야 해서 applyParaHeadingFields 가 고친다.
        ParaField{"headingType",
                  [](const model::ParaShape& p) {
                      return FormatValue{p.headingType.empty() ? std::string("NONE") : p.headingType};
                  },
                  [](model::ParaShape&, const FormatValue&) {}},
        // 수준은 0부터다. 한글 대화상자의 1 수준이 0 이다.
        CHWP_PARA_INT("headingLevel", headingLevel, 0, 9),
    };
    return fields;
}

// 테두리/배경. 글자 모양과 문단 모양이 같은 이름을 "char." · "para." 를 붙여 쓴다. 굵기는 HWPUNIT 이다.
// 한글처럼 #RRGGBB 대문자로 적는다. 같은 테두리/배경을 찾을 때 글자 그대로 견준다.
std::string upperHex(const model::Color& color) {
    std::string hex = hexOfColor(color);
    for (char& ch : hex) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return hex;
}

struct FillField {
    std::string key;
    std::function<FormatValue(const model::BorderFill&)> read;
    std::function<void(model::BorderFill&, const FormatValue&)> write;
};

using SideOf = model::BorderSide model::BorderFill::*;

const std::vector<FillField>& fillFields() {
    static const std::vector<FillField> fields = [] {
        std::vector<FillField> out;
        const std::pair<const char*, SideOf> sides[] = {
            {"Left", &model::BorderFill::left},
            {"Right", &model::BorderFill::right},
            {"Top", &model::BorderFill::top},
            {"Bottom", &model::BorderFill::bottom},
        };
        for (const auto& [name, member] : sides) {
            out.push_back({std::string("border") + name + "Type",
                           [member](const model::BorderFill& f) { return FormatValue{(f.*member).type}; },
                           [member](model::BorderFill& f, const FormatValue& v) {
                               if (const std::string* t = std::get_if<std::string>(&v); t && !t->empty()) {
                                   (f.*member).type = *t;
                               }
                           }});
            out.push_back({std::string("border") + name + "Width",
                           [member](const model::BorderFill& f) {
                               return FormatValue{static_cast<double>((f.*member).width)};
                           },
                           [member](model::BorderFill& f, const FormatValue& v) {
                               if (numberOf(v)) (f.*member).width = intOf(v, 0, 10000);
                           }});
            out.push_back({std::string("border") + name + "Color",
                           [member](const model::BorderFill& f) { return FormatValue{(f.*member).color}; },
                           [member](model::BorderFill& f, const FormatValue& v) {
                               if (const model::Color c = colorOf(v)) (f.*member).color = upperHex(c);
                           }});
        }
        out.push_back({"fillFaceColor",
                       [](const model::BorderFill& f) {
                           return FormatValue{f.faceColor == "none" ? std::string() : f.faceColor};
                       },
                       [](model::BorderFill& f, const FormatValue& v) {
                           const model::Color c = colorOf(v);
                           f.faceColor = c ? upperHex(c) : std::string();
                       }});
        out.push_back({"fillHatchStyle", [](const model::BorderFill& f) { return FormatValue{f.hatchStyle}; },
                       [](model::BorderFill& f, const FormatValue& v) {
                           if (const std::string* t = std::get_if<std::string>(&v)) {
                               f.hatchStyle = *t == "NONE" ? std::string() : *t;
                               if (!f.hatchStyle.empty() && f.hatchColor.empty()) f.hatchColor = "#000000";
                           }
                       }});
        out.push_back({"fillHatchColor", [](const model::BorderFill& f) { return FormatValue{f.hatchColor}; },
                       [](model::BorderFill& f, const FormatValue& v) {
                           if (const model::Color c = colorOf(v)) f.hatchColor = upperHex(c);
                       }});
        return out;
    }();
    return fields;
}

// "char.borderLeftType" 처럼 접두가 맞으면 그 테두리/배경 이름표.
const FillField* fillFieldOf(const std::string& key, const char* prefix) {
    const std::size_t n = std::char_traits<char>::length(prefix);
    if (key.compare(0, n, prefix) != 0) return nullptr;
    for (const FillField& field : fillFields()) {
        if (key.compare(n, std::string::npos, field.key) == 0) return &field;
    }
    return nullptr;
}

bool sameSide(const model::BorderSide& a, const model::BorderSide& b) {
    return a.type == b.type && a.color == b.color && a.width == b.width;
}

bool sameBorderFill(const model::BorderFill& a, const model::BorderFill& b) {
    return sameSide(a.left, b.left) && sameSide(a.right, b.right) && sameSide(a.top, b.top) &&
           sameSide(a.bottom, b.bottom) && sameSide(a.diagonal, b.diagonal) && a.slash.type == b.slash.type &&
           a.slash.crooked == b.slash.crooked && a.slash.counter == b.slash.counter &&
           a.backSlash.type == b.backSlash.type && a.backSlash.crooked == b.backSlash.crooked &&
           a.backSlash.counter == b.backSlash.counter && a.centerLine == b.centerLine &&
           a.breakCellSeparateLine == b.breakCellSeparateLine && a.faceColor == b.faceColor &&
           a.hatchStyle == b.hatchStyle && (a.hatchStyle.empty() || a.hatchColor == b.hatchColor) &&
           a.gradColor == b.gradColor && a.gradAngle == b.gradAngle && a.imageId == b.imageId &&
           a.imageMode == b.imageMode;
}

const model::BorderFill* borderFillById(const model::Document& doc, std::uint32_t id) {
    for (const model::BorderFill& fill : doc.header.borderFills) {
        if (fill.id == id) return &fill;
    }
    return nullptr;
}

// 바꾼 테두리/배경과 같은 것이 있으면 그 번호, 없으면 새로 넣은 번호.
std::uint32_t internBorderFill(model::Document& doc, model::BorderFill fill) {
    std::uint32_t maxId = 0;
    for (const model::BorderFill& have : doc.header.borderFills) {
        maxId = std::max(maxId, have.id);
        if (sameBorderFill(have, fill)) return have.id;
    }
    fill.id = maxId + 1;
    doc.header.borderFills.push_back(fill);
    return fill.id;
}

// 고칠 이름이 하나라도 있으면 지금 테두리/배경을 고쳐 붙인 번호를 돌려준다. 없으면 그대로다.
std::uint32_t applyFillFields(model::Document& doc, std::uint32_t current, const FormatFields& fields,
                              const char* prefix) {
    bool touched = false;
    model::BorderFill fill;
    if (const model::BorderFill* have = borderFillById(doc, current)) fill = *have;
    for (const auto& [key, value] : fields) {
        if (const FillField* field = fillFieldOf(key, prefix)) {
            field->write(fill, value);
            touched = true;
        }
    }
    return touched ? internBorderFill(doc, fill) : current;
}

// 탭 목록 글. "위치,종류,채움;..." 이고 위치는 HWPUNIT 이다. 셸은 이 글을 나눠 목록으로 보인다.
std::string tabStopsText(const std::vector<model::TabStop>& stops) {
    std::string out;
    for (const model::TabStop& stop : stops) {
        if (!out.empty()) out += ';';
        out += std::to_string(stop.pos) + ',' + (stop.type.empty() ? "LEFT" : stop.type) + ',' +
               (stop.leader.empty() ? "NONE" : stop.leader);
    }
    return out;
}

std::vector<model::TabStop> tabStopsOf(const std::string& text) {
    std::vector<model::TabStop> out;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(';', at);
        if (end == std::string::npos) end = text.size();
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        const std::size_t a = item.find(',');
        const std::size_t b = a == std::string::npos ? a : item.find(',', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        model::TabStop stop;
        stop.pos = static_cast<std::int32_t>(std::strtol(item.substr(0, a).c_str(), nullptr, 10));
        stop.type = item.substr(a + 1, b - a - 1);
        stop.leader = item.substr(b + 1);
        if (stop.pos < 0) continue;
        if (stop.type != "LEFT" && stop.type != "RIGHT" && stop.type != "CENTER" && stop.type != "DECIMAL") {
            stop.type = "LEFT";
        }
        if (stop.leader.empty()) stop.leader = "NONE";
        out.push_back(stop);
    }
    // 한글처럼 위치 차례로 두고, 같은 위치는 나중 것만 남긴다.
    std::stable_sort(out.begin(), out.end(),
                     [](const model::TabStop& x, const model::TabStop& y) { return x.pos < y.pos; });
    std::vector<model::TabStop> unique;
    for (const model::TabStop& stop : out) {
        if (!unique.empty() && unique.back().pos == stop.pos) unique.back() = stop;
        else unique.push_back(stop);
    }
    return unique;
}

const model::TabDef* tabDefById(const model::Document& doc, std::uint32_t id) {
    for (const model::TabDef& def : doc.header.tabDefs) {
        if (def.id == id) return &def;
    }
    return nullptr;
}

void readTabFields(const model::Document& doc, std::uint32_t id, FormatFields& out) {
    model::TabDef def;
    if (const model::TabDef* have = tabDefById(doc, id)) def = *have;
    out.emplace_back("tabStops", tabStopsText(def.stops));
    out.emplace_back("tabAutoLeft", def.autoTabLeft);
    out.emplace_back("tabAutoRight", def.autoTabRight);
}

void readFillFields(const model::Document& doc, std::uint32_t id, const char* prefix, FormatFields& out) {
    model::BorderFill fill;
    if (const model::BorderFill* have = borderFillById(doc, id)) fill = *have;
    for (const FillField& field : fillFields()) out.emplace_back(prefix + field.key, field.read(fill));
}

#undef CHWP_CHAR_BOOL
#undef CHWP_CHAR_LANG
#undef CHWP_CHAR_COLOR
#undef CHWP_PARA_BOOL
#undef CHWP_PARA_INT
#undef CHWP_PARA_TEXT

template <typename Field>
const Field* findField(const std::vector<Field>& fields, const std::string& key) {
    for (const Field& field : fields) {
        if (key == field.key) return &field;
    }
    return nullptr;
}

}  // namespace

namespace paint_detail {

model::BorderFill borderFillOf(const model::Document& doc, std::uint32_t id) {
    if (const model::BorderFill* have = borderFillById(doc, id)) return *have;
    return {};
}

std::uint32_t internBorderFillOf(model::Document& doc, const model::BorderFill& fill) { return internBorderFill(doc, fill); }

// 언어별 값: "ratio.LATIN" 처럼 이름 뒤에 언어(model::langName)를 붙인다. 붙이지 않은 이름은 모든 언어에 넣는다.
// 글꼴(font.LANG)은 문서 글꼴 표에 넣어야 해서 deriveCharShapeId 가 넣는다.
int langOfKey(const std::string& key, std::string& base) {
    const std::size_t dot = key.rfind('.');
    if (dot == std::string::npos || dot == 0) return -1;
    const std::string tail = key.substr(dot + 1);
    for (std::size_t i = 0; i < model::kLangCount; ++i) {
        if (tail == model::langName(static_cast<model::Lang>(i))) {
            base = key.substr(0, dot);
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool langCharKey(const std::string& key) {
    std::string base;
    if (langOfKey(key, base) < 0) return false;
    return base == "ratio" || base == "spacing" || base == "relSize" || base == "charOffset" || base == "font";
}

void writeLangCharField(model::CharShape& shape, const std::string& key, const FormatValue& value) {
    std::string base;
    const int lang = langOfKey(key, base);
    if (lang < 0 || !numberOf(value)) return;
    const auto at = static_cast<std::size_t>(lang);
    if (base == "ratio") shape.ratio[at] = intOf(value, 50, 200);
    else if (base == "spacing") shape.spacing[at] = intOf(value, -50, 50);
    else if (base == "relSize") shape.relSize[at] = intOf(value, 10, 250);
    else if (base == "charOffset") shape.offset[at] = intOf(value, -100, 100);
}

bool formatFieldsTouchChar(const FormatFields& fields) {
    return std::any_of(fields.begin(), fields.end(), [](const auto& f) {
        return findField(charFields(), f.first) != nullptr || fillFieldOf(f.first, "char.") != nullptr ||
               langCharKey(f.first);
    });
}

bool isTabKey(const std::string& key) {
    return key == "tabStops" || key == "tabAutoLeft" || key == "tabAutoRight" || key == "sectionDefaultTab";
}

bool formatFieldsTouchPara(const FormatFields& fields) {
    return std::any_of(fields.begin(), fields.end(), [](const auto& f) {
        return findField(paraFields(), f.first) != nullptr || fillFieldOf(f.first, "para.") != nullptr ||
               isTabKey(f.first);
    });
}

void applyParaHeadingFields(model::Document& doc, model::ParaShape& shape, const FormatFields& fields) {
    const std::string* kind = nullptr;
    for (const auto& [key, value] : fields) {
        if (key == "headingType") kind = std::get_if<std::string>(&value);
    }
    if (!kind || kind->empty()) return;
    const std::string before = shape.headingType.empty() ? "NONE" : shape.headingType;
    if (*kind == before) return;
    shape.headingType = *kind;
    if (*kind == "NONE") {
        shape.headingIdRef = 0;
        shape.headingLevel = 0;
    } else if (*kind == "OUTLINE") {
        // 개요는 구역의 개요 번호를 쓴다. 한글은 idRef 를 0 으로 적는다.
        shape.headingIdRef = 0;
    } else if (*kind == "BULLET") {
        // 정의가 없으면 0 번(●)이다. 한글 2024 도 정의 없이 0 을 적는다.
        shape.headingIdRef = doc.header.bullets.empty() ? 0 : doc.header.bullets.front().id;
    } else if (*kind == "NUMBER") {
        // 한글은 기본 번호 모양을 베낀 정의를 새로 만든다. 같은 모양 정의가 이미 있으면 그것을 쓴다.
        // 1번은 구역 개요가 쓰므로 고르지 않는다.
        const model::Numbering& base = detail::builtinOutline();
        const auto sameLevels = [&](const model::Numbering& n) {
            if (n.levels.size() < 7) return false;
            for (std::size_t i = 0; i < 7; ++i) {
                if (n.levels[i].format != base.levels[i].format || n.levels[i].numFormat != base.levels[i].numFormat) {
                    return false;
                }
            }
            return true;
        };
        std::uint32_t maxId = 1;
        std::uint32_t found = 0;
        for (const model::Numbering& n : doc.header.numberings) {
            maxId = std::max(maxId, n.id);
            if (!found && n.id != 1 && sameLevels(n)) found = n.id;
        }
        if (!found) {
            model::Numbering made = base;
            made.id = maxId + 1;
            doc.header.numberings.push_back(made);
            found = made.id;
        }
        shape.headingIdRef = found;
    }
}

void applyParaTabFields(model::Document& doc, model::ParaShape& shape, const FormatFields& fields) {
    bool touched = false;
    model::TabDef def;
    if (const model::TabDef* have = tabDefById(doc, shape.tabDefId)) def = *have;
    for (const auto& [key, value] : fields) {
        if (key == "tabStops") {
            if (const std::string* t = std::get_if<std::string>(&value)) {
                def.stops = tabStopsOf(*t);
                touched = true;
            }
        } else if (key == "tabAutoLeft" || key == "tabAutoRight") {
            if (const bool* b = std::get_if<bool>(&value)) {
                (key == "tabAutoLeft" ? def.autoTabLeft : def.autoTabRight) = *b;
                touched = true;
            }
        }
    }
    if (!touched) return;
    const auto sameStops = [](const model::TabDef& x, const model::TabDef& y) {
        if (x.autoTabLeft != y.autoTabLeft || x.autoTabRight != y.autoTabRight || x.stops.size() != y.stops.size()) {
            return false;
        }
        for (std::size_t i = 0; i < x.stops.size(); ++i) {
            if (x.stops[i].pos != y.stops[i].pos || x.stops[i].type != y.stops[i].type ||
                x.stops[i].leader != y.stops[i].leader) {
                return false;
            }
        }
        return true;
    };
    std::uint32_t maxId = 0;
    for (const model::TabDef& have : doc.header.tabDefs) {
        maxId = std::max(maxId, have.id);
        if (sameStops(have, def)) {
            shape.tabDefId = have.id;
            return;
        }
    }
    def.id = doc.header.tabDefs.empty() ? 0 : maxId + 1;
    doc.header.tabDefs.push_back(def);
    shape.tabDefId = def.id;
}

bool sectionDefaultTabOf(const FormatFields& fields, std::int32_t& value) {
    for (const auto& [key, v] : fields) {
        if (key == "sectionDefaultTab" && numberOf(v)) {
            value = intOf(v, 100, 1000000);
            return true;
        }
    }
    return false;
}

void applyCharFillFields(model::Document& doc, model::CharShape& shape, const FormatFields& fields) {
    shape.borderFillId = applyFillFields(doc, shape.borderFillId, fields, "char.");
}

void applyParaFillFields(model::Document& doc, model::ParaShape& shape, const FormatFields& fields) {
    shape.borderFillId = applyFillFields(doc, shape.borderFillId, fields, "para.");
}

void applyCharFields(model::CharShape& shape, const FormatFields& fields) {
    for (const auto& [key, value] : fields) {
        if (const CharField* field = findField(charFields(), key)) field->write(shape, value);
        else if (langCharKey(key)) writeLangCharField(shape, key, value);
    }
}

void applyCharLangFonts(model::Document& doc, model::CharShape& shape, const FormatFields& fields) {
    for (const auto& [key, value] : fields) {
        std::string base;
        const int lang = langOfKey(key, base);
        const std::string* face = std::get_if<std::string>(&value);
        if (lang < 0 || base != "font" || !face || face->empty()) continue;
        const auto slot = static_cast<std::size_t>(lang);
        if (slot >= doc.header.fonts.size()) continue;
        auto& fonts = doc.header.fonts[slot];
        std::size_t idx = 0;
        while (idx < fonts.size() && fonts[idx].face != *face) ++idx;
        if (idx == fonts.size()) {
            model::Font font;
            font.id = static_cast<std::uint32_t>(fonts.size());
            font.face = *face;
            font.type = "TTF";
            fonts.push_back(std::move(font));
        }
        shape.fontRef[slot] = static_cast<std::uint32_t>(idx);
    }
}

void applyParaFields(model::ParaShape& shape, const FormatFields& fields) {
    for (const auto& [key, value] : fields) {
        if (const ParaField* field = findField(paraFields(), key)) field->write(shape, value);
    }
}

}  // namespace paint_detail

bool setFormatField(FormatPatch& patch, const std::string& key, const FormatValue& value) {
    const double* n = numberOf(value);
    const bool* b = std::get_if<bool>(&value);
    const std::string* t = std::get_if<std::string>(&value);
    // 서식 도구 상자가 쓰던 이름은 원래 자리에 넣는다. 글꼴 등록·문단 스타일처럼 따로 하는 일이 있다.
    if (key == "styleId" && n) {
        patch.hasStyleId = true;
        patch.styleId = static_cast<std::uint32_t>(std::max(0.0, *n));
        return true;
    }
    if (key == "styleName" && t) {
        patch.styleName = *t;
        return !t->empty();
    }
    if (key == "indentStep" && n) {
        patch.indentStep = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
        return patch.indentStep != 0;
    }
    if (key == "marginLeftStep" && n) {
        patch.marginLeftStep = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
        return patch.marginLeftStep != 0;
    }
    if (key == "marginRightStep" && n) {
        patch.marginRightStep = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
        return patch.marginRightStep != 0;
    }
    if (key == "spacingStep" && n) {
        patch.spacingStep = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
        return patch.spacingStep != 0;
    }
    if (t && (key == "pickUnderlineShape" || key == "pickUnderlineColor" || key == "pickStrikeShape" ||
              key == "pickStrikeColor" || key == "pickCharBorderType" || key == "pickCharBorderColor")) {
        patch.linePicks.emplace_back(key.substr(4), *t);
        return true;
    }
    if (key == "normalShape" && b) {
        patch.normalShape = *b;
        return *b;
    }
    if (key == "ratioStep" && n) {
        patch.ratioStep = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
        return patch.ratioStep != 0;
    }
    if (key == "headingToggle" && t) {
        if (*t != "NUMBER" && *t != "BULLET" && *t != "OUTLINE") return false;
        patch.headingToggle = *t;
        return true;
    }
    if (key == "headingLevelStep" && n) {
        patch.headingLevelStep = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
        return patch.headingLevelStep != 0;
    }
    if (key == "align" && t) {
        patch.hasAlign = true;
        patch.align = *t;
        return true;
    }
    if (key == "lineSpacingType" && t) {
        patch.hasLineSpacingType = true;
        patch.lineSpacingType = *t;
        return true;
    }
    if (key == "lineSpacing" && n) {
        patch.hasLineSpacing = true;
        patch.lineSpacing = static_cast<std::int32_t>(std::llround(*n));
        return true;
    }
    if (key == "lineSpacingValue" && n) {
        patch.hasLineSpacingValue = true;
        patch.lineSpacingValue = *n;
        return true;
    }
    if (key == "lineSpacingStep" && n) {
        patch.lineSpacingStep = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
        return patch.lineSpacingStep != 0;
    }
    // 글자 크기 칸: fontSizeValue 와 그 단위 fontSizeUnit(pt, mm, cm, inch, px). 적용할 때 HU 로 바꾼다.
    if (key == "fontSizeUnit" && t) {
        patch.fontSizeUnit = *t;
        return true;
    }
    if (key == "fontSizeValue" && n) {
        patch.hasFontSize = true;
        patch.fontSizeValueSet = true;
        patch.fontSizeValue = *n;
        return true;
    }
    if (key == "fontSizeStep" && n) {
        patch.fontSizeStep = *n > 0 ? 1 : (*n < 0 ? -1 : 0);
        return patch.fontSizeStep != 0;
    }
    if (key == "font" && t) {
        patch.hasFont = true;
        patch.font = *t;
        return true;
    }
    if ((key == "fontSize" || key == "fontSizePt") && n) {
        patch.hasFontSize = true;
        patch.fontSize = static_cast<std::int32_t>(std::llround(key == "fontSize" ? *n : *n * 100.0));
        return true;
    }
    if (key == "bold" && b) {
        patch.hasBold = true;
        patch.bold = *b;
        return true;
    }
    if (key == "italic" && b) {
        patch.hasItalic = true;
        patch.italic = *b;
        return true;
    }
    if (key == "underline" && b) {
        patch.hasUnderline = true;
        patch.underline = *b;
        return true;
    }
    if (key == "underlineType" && t) {
        patch.hasUnderlineType = true;
        patch.underlineType = *t;
        return true;
    }
    if (key == "strikeout" && b) {
        patch.hasStrikeout = true;
        patch.strikeout = *b;
        return true;
    }
    if (key == "color" && t) {
        // 빈 색은 기본 글자 색(검정)이다.
        patch.hasColor = true;
        patch.color = t->empty() ? std::string("#000000") : *t;
        return true;
    }
    if (key == "markpen" && t) {
        patch.hasMarkpen = true;
        patch.markpen = *t == "none" ? std::string() : *t;
        if (!patch.markpen.empty()) {
            for (char& ch : patch.markpen) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return true;
    }
    if (key == "toggle" && t) {
        if (*t != "bold" && *t != "italic" && *t != "underline" && *t != "strikeout" && *t != "superscript" &&
            *t != "subscript" && *t != "superSub" && *t != "charBorder") {
            return false;
        }
        patch.toggles.push_back(*t);
        return true;
    }
    if (findField(charFields(), key) || findField(paraFields(), key) || fillFieldOf(key, "char.") || langCharKey(key) ||
        fillFieldOf(key, "para.") || paint_detail::isTabKey(key)) {
        patch.fields.emplace_back(key, value);
        return true;
    }
    return false;
}

namespace paint_detail {

std::vector<model::TabStop> parseTabStops(const std::string& text) { return tabStopsOf(text); }
std::string tabStopsTextOf(const std::vector<model::TabStop>& stops) { return tabStopsText(stops); }

void readFillFieldsOf(const model::Document& doc, std::uint32_t id, const char* prefix, FormatFields& out) {
    readFillFields(doc, id, prefix, out);
}

std::uint32_t applyFillFieldsOf(model::Document& doc, std::uint32_t current, const FormatFields& fields, const char* prefix) {
    return applyFillFields(doc, current, fields, prefix);
}

// 글자 모양 하나의 대화 상자 값(글꼴·크기 같은 첫 값 뒤에 붙는 것). 언어별 값과 "char." 테두리/배경까지.
void appendCharShapeFields(const model::Document& doc, const model::CharShape& cs, FormatFields& out) {
    for (const CharField& field : charFields()) out.emplace_back(field.key, field.read(cs));
    readFillFields(doc, cs.borderFillId, "char.", out);
    // 언어별 글꼴·장평·자간·상대 크기·글자 위치(글자 모양 대화 상자의 언어 고르기).
    for (std::size_t i = 0; i < model::kLangCount; ++i) {
        const std::string lang = model::langName(static_cast<model::Lang>(i));
        std::string face;
        if (i < doc.header.fonts.size() && cs.fontRef[i] < doc.header.fonts[i].size()) {
            face = doc.header.fonts[i][cs.fontRef[i]].face;
        }
        out.emplace_back("font." + lang, face);
        out.emplace_back("ratio." + lang, static_cast<double>(cs.ratio[i] != 0 ? cs.ratio[i] : 100));
        out.emplace_back("spacing." + lang, static_cast<double>(cs.spacing[i]));
        out.emplace_back("relSize." + lang, static_cast<double>(cs.relSize[i] != 0 ? cs.relSize[i] : 100));
        out.emplace_back("charOffset." + lang, static_cast<double>(cs.offset[i]));
    }
}

void appendParaShapeFields(const model::Document& doc, const model::ParaShape& ps, FormatFields& out) {
    for (const ParaField& field : paraFields()) out.emplace_back(field.key, field.read(ps));
    readFillFields(doc, ps.borderFillId, "para.", out);
    readTabFields(doc, ps.tabDefId, out);
}

FormatFields formatFieldsOf(const model::Document& doc, const CursorStyle& st, std::int32_t sectionDefaultTab) {
    FormatFields out = {
        {"font", st.font},
        {"fontSize", static_cast<double>(st.fontSize)},
        {"bold", st.bold},
        {"italic", st.italic},
        {"underlineType", st.underlineType},
        {"strikeout", st.strikeout},
        {"color", st.color},
        {"align", st.align},
        {"lineSpacingType", st.lineSpacingType},
        {"lineSpacing", static_cast<double>(st.lineSpacing)},
    };
    if (const model::CharShape* cs = Painter::findCharShape(doc, st.charShapeId)) appendCharShapeFields(doc, *cs, out);
    if (const model::ParaShape* ps = Painter::findParaShape(doc, st.paraShapeId)) appendParaShapeFields(doc, *ps, out);
    if (sectionDefaultTab > 0) out.emplace_back("sectionDefaultTab", static_cast<double>(sectionDefaultTab));
    return out;
}

}  // namespace paint_detail

FormatFields formatFieldsAtCursor(const model::Document& doc, const std::vector<Node>& pages) {
    std::int32_t defaultTab = 0;
    if (const model::StorySlot* slot = resolveStory(doc, pointerCursor().story)) {
        const int section = std::max(0, slot->sectionIndex);
        if (section < static_cast<int>(doc.sections.size())) {
            defaultTab = doc.sections[static_cast<std::size_t>(section)].defaultTab;
        }
    }
    return formatFieldsOf(doc, cursorStyle(doc, pages), defaultTab);
}

FormatFields charShapeFields(const model::Document& doc, std::uint32_t charShapeId) {
    FormatFields out;
    const model::CharShape* cs = Painter::findCharShape(doc, charShapeId);
    if (!cs) return out;
    std::string color = "#000000";
    if (cs->textColor) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(*cs->textColor & 0xFFFFFFu));
        color = buf;
    }
    out.emplace_back("font", Painter::resolveCharShapeFontName(doc, *cs));
    out.emplace_back("fontSize", static_cast<double>(cs->height > 0 ? cs->height : 1000));
    out.emplace_back("bold", cs->bold);
    out.emplace_back("italic", cs->italic);
    out.emplace_back("underlineType", cs->underline.empty() ? std::string("NONE") : cs->underline);
    out.emplace_back("strikeout", cs->strikeout);
    out.emplace_back("color", color);
    paint_detail::appendCharShapeFields(doc, *cs, out);
    return out;
}

FormatFields paraShapeFields(const model::Document& doc, std::uint32_t paraShapeId) {
    FormatFields out;
    const model::ParaShape* ps = Painter::findParaShape(doc, paraShapeId);
    if (!ps) return out;
    out.emplace_back("align", ps->align.empty() ? std::string("JUSTIFY") : ps->align);
    out.emplace_back("lineSpacingType", ps->lineSpacingType.empty() ? std::string("PERCENT") : ps->lineSpacingType);
    out.emplace_back("lineSpacing", static_cast<double>(ps->lineSpacing > 0 ? ps->lineSpacing : 160));
    paint_detail::appendParaShapeFields(doc, *ps, out);
    return out;
}

FormatFields tableFields(const model::Document& doc, const model::Table& table) {
    FormatFields out;
    out.emplace_back("cellSpacing", static_cast<double>(table.cellSpacing));
    out.emplace_back("pageBreak", table.pageBreak);
    out.emplace_back("repeatHeader", table.repeatHeader);
    out.emplace_back("marginLeft", static_cast<double>(table.marginLeft));
    out.emplace_back("marginRight", static_cast<double>(table.marginRight));
    out.emplace_back("marginTop", static_cast<double>(table.marginTop));
    out.emplace_back("marginBottom", static_cast<double>(table.marginBottom));
    paint_detail::readFillFieldsOf(doc, table.borderFillId, "table.", out);
    return out;
}

FormatFields tableCellFields(const model::Document& doc, const model::Table& table, const model::TableCell& cell) {
    FormatFields out;
    out.emplace_back("vertAlign", cell.vertAlign.empty() ? std::string("CENTER") : cell.vertAlign);
    out.emplace_back("header", cell.header);
    // 칸 안 여백. 칸이 제 여백을 쓰지 않으면 표 안쪽 여백이다.
    out.emplace_back("ownMargin", cell.hasMargin);
    out.emplace_back("marginLeft", static_cast<double>(cell.hasMargin ? cell.marginLeft : table.marginLeft));
    out.emplace_back("marginRight", static_cast<double>(cell.hasMargin ? cell.marginRight : table.marginRight));
    out.emplace_back("marginTop", static_cast<double>(cell.hasMargin ? cell.marginTop : table.marginTop));
    out.emplace_back("marginBottom", static_cast<double>(cell.hasMargin ? cell.marginBottom : table.marginBottom));
    paint_detail::readFillFieldsOf(doc, cell.borderFillId, "cell.", out);
    return out;
}

FormatFields placementFields(const model::Placement& p) {
    FormatFields out;
    out.emplace_back("treatAsChar", p.treatAsChar);
    out.emplace_back("textWrap", p.textWrap.empty() ? std::string("TOP_AND_BOTTOM") : p.textWrap);
    out.emplace_back("textFlow", p.textFlow);
    out.emplace_back("vertRelTo", p.vertRelTo.empty() ? std::string("PARA") : p.vertRelTo);
    out.emplace_back("horzRelTo", p.horzRelTo.empty() ? std::string("COLUMN") : p.horzRelTo);
    out.emplace_back("vertAlign", p.vertAlign);
    out.emplace_back("horzAlign", p.horzAlign);
    out.emplace_back("vertOffset", static_cast<double>(p.vertOffset));
    out.emplace_back("horzOffset", static_cast<double>(p.horzOffset));
    out.emplace_back("width", static_cast<double>(p.width));
    out.emplace_back("height", static_cast<double>(p.height));
    out.emplace_back("zOrder", static_cast<double>(p.zOrder));
    out.emplace_back("flowWithText", p.flowWithText);
    out.emplace_back("allowOverlap", p.allowOverlap);
    return out;
}

}  // namespace chwp::render
