#include "noting/render/paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "render/paint_detail.hpp"
#include "render/painter.hpp"

// 글자 모양·문단 모양 대화 상자가 읽고 쓰는 값의 이름표.
// 셸은 이름과 값을 그대로 넘기고, 이름·단위·고치는 법은 여기 한 곳에서 정한다.

namespace noting::render {
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

#define NOTING_CHAR_BOOL(name, member)                                                        \
    CharField {                                                                               \
        name, [](const model::CharShape& c) { return FormatValue{c.member}; },                \
            [](model::CharShape& c, const FormatValue& v) {                                   \
                if (const bool* b = std::get_if<bool>(&v)) c.member = *b;                     \
            }                                                                                 \
    }
#define NOTING_CHAR_LANG(name, member, lo, hi, zeroAs)                                                 \
    CharField {                                                                                        \
        name, [](const model::CharShape& c) {                                                          \
            return FormatValue{static_cast<double>(c.member[0] != 0 ? c.member[0] : zeroAs)};          \
        },                                                                                             \
            [](model::CharShape& c, const FormatValue& v) {                                            \
                if (numberOf(v)) fillLang(c.member, intOf(v, lo, hi));                                 \
            }                                                                                          \
    }
#define NOTING_CHAR_COLOR(name, member)                                                     \
    CharField {                                                                             \
        name, [](const model::CharShape& c) { return FormatValue{hexOfColor(c.member)}; },  \
            [](model::CharShape& c, const FormatValue& v) { c.member = colorOf(v); }        \
    }
#define NOTING_PARA_BOOL(name, member)                                                        \
    ParaField {                                                                               \
        name, [](const model::ParaShape& p) { return FormatValue{p.member}; },                \
            [](model::ParaShape& p, const FormatValue& v) {                                   \
                if (const bool* b = std::get_if<bool>(&v)) p.member = *b;                     \
            }                                                                                 \
    }
#define NOTING_PARA_INT(name, member, lo, hi)                                                           \
    ParaField {                                                                                         \
        name, [](const model::ParaShape& p) { return FormatValue{static_cast<double>(p.member)}; },     \
            [](model::ParaShape& p, const FormatValue& v) {                                             \
                if (numberOf(v)) p.member = intOf(v, lo, hi);                                           \
            }                                                                                           \
    }
#define NOTING_PARA_TEXT(name, member)                                                        \
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
        NOTING_CHAR_LANG("ratio", ratio, 50, 200, 100),
        NOTING_CHAR_LANG("spacing", spacing, -50, 50, 0),
        NOTING_CHAR_LANG("relSize", relSize, 10, 250, 100),
        NOTING_CHAR_LANG("charOffset", offset, -100, 100, 0),
        NOTING_CHAR_COLOR("shadeColor", shadeColor),
        CharField{"underlineShape", [](const model::CharShape& c) { return FormatValue{c.underlineShape}; },
                  [](model::CharShape& c, const FormatValue& v) { setLineShape(c.underlineShape, c.underlineShapeId, v); }},
        NOTING_CHAR_COLOR("underlineColor", underlineColor),
        CharField{"strikeShape", [](const model::CharShape& c) { return FormatValue{c.strikeShape}; },
                  [](model::CharShape& c, const FormatValue& v) { setLineShape(c.strikeShape, c.strikeShapeId, v); }},
        NOTING_CHAR_COLOR("strikeColor", strikeColor),
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
        NOTING_CHAR_COLOR("shadowColor", shadowColor),
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
        NOTING_CHAR_BOOL("useFontSpace", useFontSpace),
        NOTING_CHAR_BOOL("useKerning", useKerning),
    };
    return fields;
}

// 문단 모양. 여백·간격은 HWPUNIT 이다.
const std::vector<ParaField>& paraFields() {
    static const std::vector<ParaField> fields = {
        NOTING_PARA_TEXT("vertAlign", vertAlign),
        NOTING_PARA_INT("indent", indent, -1000000, 1000000),
        NOTING_PARA_INT("marginLeft", marginLeft, -1000000, 1000000),
        NOTING_PARA_INT("marginRight", marginRight, -1000000, 1000000),
        NOTING_PARA_INT("spacingPrev", spacingPrev, -1000000, 1000000),
        NOTING_PARA_INT("spacingNext", spacingNext, -1000000, 1000000),
        NOTING_PARA_TEXT("breakLatinWord", breakLatinWord),
        NOTING_PARA_TEXT("breakNonLatinWord", breakNonLatinWord),
        NOTING_PARA_INT("condense", condense, 0, 75),
        NOTING_PARA_BOOL("widowOrphan", widowOrphan),
        NOTING_PARA_BOOL("keepWithNext", keepWithNext),
        NOTING_PARA_BOOL("keepLines", keepLines),
        NOTING_PARA_BOOL("pageBreakBefore", pageBreakBefore),
        NOTING_PARA_BOOL("snapToGrid", snapToGrid),
        NOTING_PARA_BOOL("fontLineHeight", fontLineHeight),
        NOTING_PARA_BOOL("autoSpacingEng", autoSpacingEng),
        NOTING_PARA_BOOL("autoSpacingNum", autoSpacingNum),
        NOTING_PARA_TEXT("lineWrap", lineWrap),
        NOTING_PARA_INT("borderOffsetLeft", borderOffsetLeft, 0, 1000000),
        NOTING_PARA_INT("borderOffsetRight", borderOffsetRight, 0, 1000000),
        NOTING_PARA_INT("borderOffsetTop", borderOffsetTop, 0, 1000000),
        NOTING_PARA_INT("borderOffsetBottom", borderOffsetBottom, 0, 1000000),
        NOTING_PARA_BOOL("borderConnect", borderConnect),
        NOTING_PARA_BOOL("borderIgnoreMargin", borderIgnoreMargin),
    };
    return fields;
}

#undef NOTING_CHAR_BOOL
#undef NOTING_CHAR_LANG
#undef NOTING_CHAR_COLOR
#undef NOTING_PARA_BOOL
#undef NOTING_PARA_INT
#undef NOTING_PARA_TEXT

template <typename Field>
const Field* findField(const std::vector<Field>& fields, const std::string& key) {
    for (const Field& field : fields) {
        if (key == field.key) return &field;
    }
    return nullptr;
}

}  // namespace

namespace paint_detail {

bool formatFieldsTouchChar(const FormatFields& fields) {
    return std::any_of(fields.begin(), fields.end(),
                       [](const auto& f) { return findField(charFields(), f.first) != nullptr; });
}

bool formatFieldsTouchPara(const FormatFields& fields) {
    return std::any_of(fields.begin(), fields.end(),
                       [](const auto& f) { return findField(paraFields(), f.first) != nullptr; });
}

void applyCharFields(model::CharShape& shape, const FormatFields& fields) {
    for (const auto& [key, value] : fields) {
        if (const CharField* field = findField(charFields(), key)) field->write(shape, value);
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
        patch.hasColor = true;
        patch.color = *t;
        return true;
    }
    if (findField(charFields(), key) || findField(paraFields(), key)) {
        patch.fields.emplace_back(key, value);
        return true;
    }
    return false;
}

FormatFields formatFieldsAtCursor(const model::Document& doc, const std::vector<Node>& pages) {
    const CursorStyle st = cursorStyle(doc, pages);
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
    if (const model::CharShape* cs = Painter::findCharShape(doc, st.charShapeId)) {
        for (const CharField& field : charFields()) out.emplace_back(field.key, field.read(*cs));
    }
    if (const model::ParaShape* ps = Painter::findParaShape(doc, st.paraShapeId)) {
        for (const ParaField& field : paraFields()) out.emplace_back(field.key, field.read(*ps));
    }
    return out;
}

}  // namespace noting::render
