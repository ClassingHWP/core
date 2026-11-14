#include "chwp/render/format_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chwp/unit.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"
#include "render/scene_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

using ValueMap = std::map<std::string, FormatValue>;

ValueMap mapOf(const FormatFields& fields) {
    ValueMap out;
    for (const auto& [key, value] : fields) out[key] = value;
    return out;
}

const FormatValue* find(const ValueMap& map, const std::string& key) {
    const auto found = map.find(key);
    return found == map.end() ? nullptr : &found->second;
}

double numberIn(const ValueMap& map, const std::string& key, double fallback = 0) {
    const FormatValue* value = find(map, key);
    if (!value) return fallback;
    if (const double* n = std::get_if<double>(value)) return *n;
    if (const bool* b = std::get_if<bool>(value)) return *b ? 1 : 0;
    return fallback;
}

std::string textIn(const ValueMap& map, const std::string& key, const std::string& fallback = {}) {
    const FormatValue* value = find(map, key);
    const std::string* t = value ? std::get_if<std::string>(value) : nullptr;
    return t ? *t : fallback;
}

bool flagIn(const ValueMap& map, const std::string& key) { return numberIn(map, key) != 0; }

// 길이 칸. 화면 단위·자릿수·범위·한 칸.
struct LengthKey {
    const char* key;
    LengthUnit unit;
    int digits;
    double min;
    double max;
    double step;
};

const std::vector<LengthKey>& lengthKeys(FormatViewKind kind) {
    static const std::vector<LengthKey> text = {
        {"fontSize", LengthUnit::Pt, 1, 1, 4096, 1},
        {"marginLeft", LengthUnit::Pt, 1, -5000, 5000, 1},
        {"marginRight", LengthUnit::Pt, 1, -5000, 5000, 1},
        {"spacingPrev", LengthUnit::Pt, 1, -5000, 5000, 1},
        {"spacingNext", LengthUnit::Pt, 1, -5000, 5000, 1},
        {"sectionDefaultTab", LengthUnit::Pt, 1, 1, 5000, 1},
        {"borderOffsetLeft", LengthUnit::Mm, 2, 0, 1000, 0.5},
        {"borderOffsetRight", LengthUnit::Mm, 2, 0, 1000, 0.5},
        {"borderOffsetTop", LengthUnit::Mm, 2, 0, 1000, 0.5},
        {"borderOffsetBottom", LengthUnit::Mm, 2, 0, 1000, 0.5},
    };
    static const std::vector<LengthKey> page = {
        {"paperWidth", LengthUnit::Mm, 1, 10, 1000, 1},
        {"paperHeight", LengthUnit::Mm, 1, 10, 1000, 1},
        {"marginLeft", LengthUnit::Mm, 1, 0, 1000, 1},
        {"marginRight", LengthUnit::Mm, 1, 0, 1000, 1},
        {"marginTop", LengthUnit::Mm, 1, 0, 1000, 1},
        {"marginBottom", LengthUnit::Mm, 1, 0, 1000, 1},
        {"marginHeader", LengthUnit::Mm, 1, 0, 1000, 1},
        {"marginFooter", LengthUnit::Mm, 1, 0, 1000, 1},
        {"marginGutter", LengthUnit::Mm, 1, 0, 1000, 1},
        {"lineGrid", LengthUnit::Pt, 1, 1, 500, 1},
        {"charGrid", LengthUnit::Pt, 1, 1, 500, 1},
    };
    return kind == FormatViewKind::Page ? page : text;
}

std::string unitKey(const LengthKey& key) { return std::string(key.key) + "@" + lengthUnitName(key.unit); }

FieldSpec numberSpec(std::string key, std::string unit, double min, double max, double step, int digits) {
    FieldSpec spec;
    spec.key = std::move(key);
    spec.unit = std::move(unit);
    spec.min = min;
    spec.max = max;
    spec.step = step;
    spec.digits = digits;
    return spec;
}

FieldSpec choiceSpec(std::string key, std::vector<std::pair<std::string, std::string>> options) {
    FieldSpec spec;
    spec.key = std::move(key);
    spec.options = std::move(options);
    return spec;
}

std::vector<std::pair<std::string, std::string>> ids(std::initializer_list<const char*> list) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const char* id : list) out.emplace_back(id, std::string());
    return out;
}

const char* const kBorderSides[] = {"Left", "Right", "Top", "Bottom"};

// 한글 테두리 굵기 목록(mm). 저장값은 mm 규칙으로 바꾼 HWPUNIT 이다.
const double kBorderWidthsMm[] = {0.1, 0.12, 0.15, 0.2, 0.25, 0.3, 0.4, 0.5, 0.6, 0.7, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0};

std::string widthLabel(double mm) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%gmm", mm);
    return buf;
}

// 저장된 굵기를 목록의 가장 가까운 굵기로.
double nearestBorderWidth(double hu) {
    double best = mmToHu(0.12);
    for (const double mm : kBorderWidthsMm) {
        const double w = mmToHu(mm);
        if (std::abs(w - hu) < std::abs(best - hu)) best = w;
    }
    return best;
}

// 용지 종류. 크기는 좁게(세로) 놓은 HWPUNIT 이다.
struct PaperKind {
    const char* id;
    const char* label;
    std::int32_t width;
    std::int32_t height;
};
const PaperKind kPaperKinds[] = {
    {"A4", "A4 (국배판) 210 × 297", 59528, 84188},     {"A3", "A3 297 × 420", 84188, 119055},
    {"A5", "A5 (국판) 148 × 210", 41953, 59528},       {"B4", "B4 (타블로이드) 257 × 364", 72850, 103181},
    {"B5", "B5 (46배판) 182 × 257", 51591, 72850},     {"LETTER", "Letter 216 × 279", 61200, 79200},
    {"LEGAL", "Legal 216 × 356", 61200, 100800},
};

std::string paperKindOf(double width, double height) {
    for (const PaperKind& kind : kPaperKinds) {
        if (std::abs(width - kind.width) <= 60 && std::abs(height - kind.height) <= 60) return kind.id;
    }
    return "CUSTOM";
}

// 쪽 여백 묶음(mm). 기본은 한글 새 문서 여백이다.
struct MarginPreset {
    const char* id;
    double top, bottom, left, right, header, footer;
};
const MarginPreset kMarginPresets[] = {
    {"기본", 20, 15, 30, 30, 15, 15},
    {"좁게", 12.7, 12.7, 12.7, 12.7, 10, 10},
    {"보통", 20, 20, 20, 20, 10, 10},
    {"넓게", 25, 25, 40, 40, 15, 15},
};

// 본문 칸(HWPUNIT). 제본 여백은 방식에 따라 폭이나 높이에서 뺀다. 가로면 긴 변이 폭이다.
struct PageBody {
    double paperW = 0;
    double paperH = 0;
    double width = 0;
    double height = 0;
};

PageBody pageBodyOf(const ValueMap& v) {
    PageBody body;
    const bool wide = flagIn(v, "landscape");
    const double w = numberIn(v, "paperWidth");
    const double h = numberIn(v, "paperHeight");
    body.paperW = wide ? std::max(w, h) : w;
    body.paperH = wide ? std::min(w, h) : h;
    const double gutter = numberIn(v, "marginGutter");
    const bool top = numberIn(v, "gutterType") == 2;
    body.width = std::max(0.0, body.paperW - numberIn(v, "marginLeft") - numberIn(v, "marginRight") - (top ? 0 : gutter));
    body.height = std::max(0.0, body.paperH - numberIn(v, "marginTop") - numberIn(v, "marginBottom") -
                                     numberIn(v, "marginHeader") - numberIn(v, "marginFooter") - (top ? gutter : 0));
    return body;
}

// ---------------------------------------------------------------------------
// 칸 패치 풀기

// 공통: 길이 표에 있는 이름@단위 → 문서 이름(HWPUNIT). 표에 없으면 거짓이다.
bool resolveLength(FormatViewKind kind, const std::string& key, const FormatValue& value, ValueMap& out) {
    const std::size_t at = key.find('@');
    if (at == std::string::npos) return false;
    const std::string base = key.substr(0, at);
    const std::string unit = key.substr(at + 1);
    const double* n = std::get_if<double>(&value);
    for (const LengthKey& length : lengthKeys(kind)) {
        if (base != length.key || unit != lengthUnitName(length.unit)) continue;
        if (n) out[base] = static_cast<double>(lengthToHu(std::clamp(*n, length.min, length.max), length.unit));
        return true;
    }
    return false;
}

ValueMap resolveText(const ValueMap& now, const FormatFields& patch) {
    ValueMap out;
    // 패치는 고친 차례다. 줄 간격 종류를 바꾸면 그 종류의 기본값이 들어가고, 그 뒤에 친 값이 이긴다.
    for (const auto& [key, value] : patch) {
        if (resolveLength(FormatViewKind::Char, key, value, out)) continue;
        const bool* b = std::get_if<bool>(&value);
        const double* n = std::get_if<double>(&value);
        const std::string* t = std::get_if<std::string>(&value);
        if (key == "plain") {
            if (b && *b) {
                out["bold"] = false;
                out["italic"] = false;
                out["underlineType"] = std::string("NONE");
                out["strikeout"] = false;
                out["outline"] = 0.0;
                out["shadow"] = 0.0;
                out["emboss"] = false;
                out["engrave"] = false;
                out["superscript"] = false;
                out["subscript"] = false;
            }
        } else if (key == "underlineOn" && b) {
            const std::string place = textIn(now, "underlineType", "NONE");
            out["underlineType"] = *b ? (place == "NONE" ? std::string("BOTTOM") : place) : std::string("NONE");
        } else if ((key == "outlineOn" || key == "shadowOn") && b) {
            const std::string base = key == "outlineOn" ? "outline" : "shadow";
            const double have = numberIn(now, base);
            out[base] = *b ? (have > 0 ? have : 1.0) : 0.0;
        } else if (key == "strikeView" && t) {
            // 취소선 모양 칸의 "없음"은 취소선을 끈다. 다른 모양은 켜고 그 모양을 쓴다.
            if (*t == "NONE") {
                out["strikeout"] = false;
            } else {
                out["strikeout"] = true;
                out["strikeShape"] = *t;
            }
        } else if (key == "indentMode" && t) {
            const double size = std::abs(numberIn(now, "indent"));
            // 들여쓰기·내어쓰기를 처음 고르면 10pt 로 시작한다.
            const double start = size > 0 ? size : 1000.0;
            out["indent"] = *t == "in" ? start : (*t == "out" ? -start : 0.0);
        } else if (key == "indentValue@pt" && n) {
            const double sign = numberIn(out.count("indent") ? out : now, "indent") < 0 ? -1 : 1;
            out["indent"] = sign * static_cast<double>(ptToHu(std::clamp(*n, 0.0, 5000.0)));
        } else if (key == "lineSpacingType" && t) {
            const std::string before = textIn(out.count("lineSpacingType") ? out : now, "lineSpacingType", "PERCENT");
            out["lineSpacingType"] = *t;
            if (*t != before) out["lineSpacing"] = *t == "PERCENT" ? 160.0 : (*t == "BETWEEN_LINES" ? 0.0 : 1000.0);
        } else if (key == "lineSpacingView" && n) {
            const std::string type =
                textIn(out.count("lineSpacingType") ? out : now, "lineSpacingType", "PERCENT");
            out["lineSpacing"] = type == "PERCENT" ? std::round(std::clamp(*n, 0.0, 500.0))
                                                   : static_cast<double>(ptToHu(std::clamp(*n, 0.0, 5000.0)));
        } else if (key == "condenseMin" && n) {
            out["condense"] = 100.0 - std::round(std::clamp(*n, 25.0, 100.0));
        } else if (key == "hangulBreak" && t) {
            // 한글 2024 는 한글 단위의 이름을 거꾸로 적는다. 어절이 BREAK_WORD, 글자가 KEEP_WORD 다.
            out["breakNonLatinWord"] = std::string(*t == "char" ? "KEEP_WORD" : "BREAK_WORD");
        } else if (key == "latinBreak" && t) {
            out["breakLatinWord"] =
                std::string(*t == "hyphen" ? "HYPHENATION" : (*t == "char" ? "BREAK_WORD" : "KEEP_WORD"));
        } else if (key == "oneLine" && b) {
            out["lineWrap"] = std::string(*b ? "SQUEEZE" : "BREAK");
        } else if (key == "tabAdd" && t) {
            // "위치pt,종류,채움". 같은 위치는 새 것으로 바꾸고 차례는 core 가 맞춘다(tabStopsOf).
            const std::size_t a = t->find(',');
            const std::size_t c = a == std::string::npos ? a : t->find(',', a + 1);
            if (a != std::string::npos && c != std::string::npos) {
                const std::int32_t pos = ptToHu(std::max(0.0, std::atof(t->substr(0, a).c_str())));
                const std::string have = textIn(out.count("tabStops") ? out : now, "tabStops");
                out["tabStops"] = have + (have.empty() ? "" : ";") + std::to_string(pos) + t->substr(a);
            }
        } else if (key == "tabRemove" && n) {
            const std::string have = textIn(out.count("tabStops") ? out : now, "tabStops");
            std::vector<model::TabStop> stops = parseTabStops(have);
            const int index = static_cast<int>(*n);
            if (index >= 0 && index < static_cast<int>(stops.size())) stops.erase(stops.begin() + index);
            out["tabStops"] = tabStopsTextOf(stops);
        } else if (key == "tabClear" && b && *b) {
            out["tabStops"] = std::string();
        } else if (key.size() > 8 && (key.rfind("char.pen", 0) == 0 || key.rfind("para.pen", 0) == 0)) {
            // 펜은 테두리를 그릴 때 쓰는 보기 값이다. 문서에는 들어가지 않는다.
        } else if (key.find("Width") != std::string::npos && (key.rfind("char.border", 0) == 0 ||
                                                               key.rfind("para.border", 0) == 0) && n) {
            out[key] = nearestBorderWidth(*n);
        } else {
            out[key] = value;
        }
    }
    return out;
}

ValueMap resolvePage(const ValueMap& now, const FormatFields& patch) {
    ValueMap out;
    ValueMap merged = now;
    const auto put = [&](const std::string& key, const FormatValue& value) {
        out[key] = value;
        merged[key] = value;
    };
    std::optional<double> lineCount;
    std::optional<double> cellCount;
    std::string gridMode;
    for (const auto& [key, value] : patch) {
        ValueMap length;
        if (resolveLength(FormatViewKind::Page, key, value, length)) {
            for (const auto& [k, v] : length) put(k, v);
            continue;
        }
        const double* n = std::get_if<double>(&value);
        const std::string* t = std::get_if<std::string>(&value);
        if (key == "paperKind" && t) {
            for (const PaperKind& kind : kPaperKinds) {
                if (*t != kind.id) continue;
                put("paperWidth", static_cast<double>(kind.width));
                put("paperHeight", static_cast<double>(kind.height));
            }
        } else if (key == "marginPreset" && t) {
            for (const MarginPreset& preset : kMarginPresets) {
                if (*t != preset.id) continue;
                put("marginTop", static_cast<double>(mmToHu(preset.top)));
                put("marginBottom", static_cast<double>(mmToHu(preset.bottom)));
                put("marginLeft", static_cast<double>(mmToHu(preset.left)));
                put("marginRight", static_cast<double>(mmToHu(preset.right)));
                put("marginHeader", static_cast<double>(mmToHu(preset.header)));
                put("marginFooter", static_cast<double>(mmToHu(preset.footer)));
            }
        } else if (key == "gridMode" && t) {
            gridMode = *t;
        } else if (key == "lineCount" && n) {
            lineCount = *n;
        } else if (key == "cellCount" && n) {
            cellCount = *n;
        } else if (key == "landscape" || key == "gutterType" || key == "wongoji" || key == "textDirection" ||
                   key == "paperWidth" || key == "paperHeight" || key.rfind("margin", 0) == 0 || key == "lineGrid" ||
                   key == "charGrid") {
            put(key, value);
        }
    }
    // 격자 방식: 없음은 둘 다 끄고, 줄은 줄 격자만, 둘 다는 글자 격자까지 켠다. 원고지는 둘 다일 때만이다.
    if (!gridMode.empty()) {
        const double line = numberIn(merged, "lineGrid");
        const double cell = numberIn(merged, "charGrid");
        put("lineGrid", gridMode == "none" ? 0.0 : (line > 0 ? line : 1600.0));
        put("charGrid", gridMode == "both" ? (cell > 0 ? cell : 2000.0) : 0.0);
        if (gridMode != "both") put("wongoji", false);
    }
    // 줄 수·눈금 수는 지금 본문 크기로 간격을 정한다.
    const PageBody body = pageBodyOf(merged);
    if (lineCount && *lineCount >= 1) {
        put("lineGrid", std::max(1.0, std::floor(body.height / std::max(1.0, std::round(*lineCount)))));
    }
    if (cellCount && *cellCount >= 1) {
        put("charGrid", std::max(1.0, std::floor(body.width / std::max(1.0, std::round(*cellCount)))));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 사본에 적용해 다시 읽기

// 그림 바이트는 빼고 문서 머리만 베낀다. 미리보기는 모양 표만 쓴다.
model::Document scratchOf(const model::Document& doc) {
    model::Document scratch;
    scratch.header.sectionCount = doc.header.sectionCount;
    scratch.header.beginNumbers = doc.header.beginNumbers;
    scratch.header.fonts = doc.header.fonts;
    scratch.header.borderFillCount = doc.header.borderFillCount;
    scratch.header.borderFills = doc.header.borderFills;
    scratch.header.charShapes = doc.header.charShapes;
    scratch.header.paraShapes = doc.header.paraShapes;
    scratch.header.styles = doc.header.styles;
    scratch.header.numberings = doc.header.numberings;
    scratch.header.bullets = doc.header.bullets;
    scratch.header.tabDefs = doc.header.tabDefs;
    return scratch;
}

struct Derived {
    model::Document scratch;
    std::uint32_t charShapeId = 0;
    std::uint32_t paraShapeId = 0;
    std::uint32_t styleId = 0;
    std::int32_t sectionDefaultTab = 0;
};

int cursorSectionIndex(const model::Document& doc) {
    if (const model::StorySlot* slot = resolveStory(doc, pointerCursor().story)) {
        return std::clamp(slot->sectionIndex, 0, std::max(0, static_cast<int>(doc.sections.size()) - 1));
    }
    return 0;
}

CursorStyle baseStyle(const model::Document& doc, const std::vector<Node>& pages, const CaretPlace* base) {
    return base ? cursorStyleForCaret(doc, *base) : cursorStyle(doc, pages);
}

Derived derive(const model::Document& doc, const std::vector<Node>& pages, const FormatFields& resolved,
               const CaretPlace* base = nullptr) {
    Derived out;
    const CursorStyle st = baseStyle(doc, pages, base);
    out.scratch = scratchOf(doc);
    out.styleId = st.styleId;
    FormatPatch patch;
    for (const auto& [key, value] : resolved) setFormatField(patch, key, value);
    out.charShapeId = deriveCharShapeId(out.scratch, st.charShapeId, patch);
    out.paraShapeId = deriveParaShapeId(out.scratch, st.paraShapeId, patch);
    const int section = base ? 0 : cursorSectionIndex(doc);
    out.sectionDefaultTab = doc.sections.empty() ? 4000 : doc.sections[static_cast<std::size_t>(section)].defaultTab;
    if (std::int32_t tab = 0; sectionDefaultTabOf(patch.fields, tab)) out.sectionDefaultTab = tab;
    return out;
}

FormatFields fieldsOf(const Derived& derived) {
    CaretPlace caret;
    caret.styleId = derived.styleId;
    caret.charShapeId = derived.charShapeId;
    caret.paraShapeId = derived.paraShapeId;
    const CursorStyle st = cursorStyleForCaret(derived.scratch, caret);
    return formatFieldsOf(derived.scratch, st, derived.sectionDefaultTab);
}

void addLengths(FormatViewKind kind, const ValueMap& values, FormatView& view) {
    for (const LengthKey& length : lengthKeys(kind)) {
        const FormatValue* value = find(values, length.key);
        if (!value || !std::get_if<double>(value)) continue;
        const auto hu = static_cast<std::int32_t>(std::llround(*std::get_if<double>(value)));
        view.values.emplace_back(unitKey(length), huToLength(hu, length.unit, length.digits));
        view.specs.push_back(
            numberSpec(unitKey(length), lengthUnitName(length.unit), length.min, length.max, length.step, length.digits));
    }
}

void addFillView(const char* prefix, const ValueMap& values, FormatView& view) {
    const std::string p(prefix);
    std::vector<std::pair<std::string, std::string>> widths;
    for (const double mm : kBorderWidthsMm) widths.emplace_back(std::to_string(mmToHu(mm)), widthLabel(mm));
    // 펜: 선이 있는 첫 변의 모양. 없으면 실선 0.12mm 검정이다.
    std::string penType = "SOLID";
    double penWidth = mmToHu(0.12);
    std::string penColor = "#000000";
    for (const char* side : {"Top", "Bottom", "Left", "Right"}) {
        const std::string type = textIn(values, p + "border" + side + "Type");
        if (type.empty() || type == "NONE") continue;
        penType = type;
        penWidth = nearestBorderWidth(numberIn(values, p + "border" + side + "Width"));
        const std::string color = textIn(values, p + "border" + side + "Color");
        if (!color.empty()) penColor = color;
        break;
    }
    view.values.emplace_back(p + "penType", penType);
    view.values.emplace_back(p + "penWidth", std::to_string(static_cast<std::int32_t>(penWidth)));
    view.values.emplace_back(p + "penColor", penColor);
    for (const char* side : kBorderSides) {
        view.values.emplace_back(p + "border" + side + "WidthView",
                                 std::to_string(static_cast<std::int32_t>(
                                     nearestBorderWidth(numberIn(values, p + "border" + side + "Width")))));
    }
    view.specs.push_back(choiceSpec(p + "penWidth", widths));
    view.specs.push_back(choiceSpec(p + "penType", ids({"SOLID", "DASH", "DOT", "DASH_DOT", "DASH_DOT_DOT", "LONG_DASH",
                                                         "CIRCLE", "DOUBLE_SLIM", "SLIM_THICK", "THICK_SLIM",
                                                         "SLIM_THICK_SLIM"})));
    view.specs.push_back(choiceSpec(
        p + "fillHatchStyle",
        ids({"NONE", "HORIZONTAL", "VERTICAL", "SLASH", "BACK_SLASH", "CROSS", "CROSS_DIAGONAL"})));
}

FormatView textView(const FormatFields& fields) {
    FormatView view;
    view.values = fields;
    const ValueMap values = mapOf(fields);
    addLengths(FormatViewKind::Char, values, view);

    // 글자 모양
    view.specs.push_back(numberSpec("relSize", "%", 10, 250, 1, 0));
    view.specs.push_back(numberSpec("charOffset", "%", -100, 100, 1, 0));
    view.specs.push_back(numberSpec("ratio", "%", 50, 200, 1, 0));
    view.specs.push_back(numberSpec("spacing", "%", -50, 50, 1, 0));
    // 언어별 값(언어 고르기). 이름은 "ratio.LATIN" 처럼 언어를 붙인다.
    for (std::size_t i = 0; i < model::kLangCount; ++i) {
        const std::string lang = std::string(".") + model::langName(static_cast<model::Lang>(i));
        view.specs.push_back(numberSpec("relSize" + lang, "%", 10, 250, 1, 0));
        view.specs.push_back(numberSpec("charOffset" + lang, "%", -100, 100, 1, 0));
        view.specs.push_back(numberSpec("ratio" + lang, "%", 50, 200, 1, 0));
        view.specs.push_back(numberSpec("spacing" + lang, "%", -50, 50, 1, 0));
    }
    view.specs.push_back(numberSpec("shadowOffsetX", "%", -100, 100, 1, 0));
    view.specs.push_back(numberSpec("shadowOffsetY", "%", -100, 100, 1, 0));
    view.specs.push_back(choiceSpec("shadow", ids({"0", "1", "2"})));
    view.specs.push_back(choiceSpec("underlineType", ids({"NONE", "BOTTOM", "TOP"})));
    const auto lineShapes = ids({"SOLID", "DASH", "DOT", "DASH_DOT", "DASH_DOT_DOT", "LONG_DASH", "CIRCLE",
                                 "DOUBLE_SLIM", "SLIM_THICK", "THICK_SLIM", "SLIM_THICK_SLIM", "WAVE", "DOUBLE_WAVE"});
    view.specs.push_back(choiceSpec("underlineShape", lineShapes));
    auto strikes = lineShapes;
    strikes.insert(strikes.begin(), {"NONE", std::string()});
    view.specs.push_back(choiceSpec("strikeView", strikes));
    view.specs.push_back(choiceSpec("outline", ids({"0", "1", "2", "3", "4", "5", "6"})));
    view.specs.push_back(choiceSpec("emphasis", ids({"0", "1", "2", "3", "4", "5", "6"})));
    view.values.emplace_back("underlineOn", textIn(values, "underlineType", "NONE") != "NONE");
    view.values.emplace_back("outlineOn", numberIn(values, "outline") > 0);
    view.values.emplace_back("shadowOn", numberIn(values, "shadow") > 0);
    view.values.emplace_back("strikeView", flagIn(values, "strikeout")
                                               ? textIn(values, "strikeShape", "SOLID")
                                               : std::string("NONE"));

    // 문단 모양
    view.specs.push_back(choiceSpec("align", ids({"JUSTIFY", "LEFT", "RIGHT", "CENTER", "DISTRIBUTE",
                                                  "DISTRIBUTE_SPACE"})));
    const double indent = numberIn(values, "indent");
    view.values.emplace_back("indentMode", std::string(indent > 0 ? "in" : (indent < 0 ? "out" : "normal")));
    view.values.emplace_back("indentValue@pt", huToPt(static_cast<std::int32_t>(std::abs(indent)), 1));
    view.specs.push_back(choiceSpec("indentMode", ids({"normal", "in", "out"})));
    view.specs.push_back(numberSpec("indentValue@pt", "pt", 0, 5000, 1, 1));
    const std::string spacingType = textIn(values, "lineSpacingType", "PERCENT");
    const bool percent = spacingType == "PERCENT";
    const auto spacing = static_cast<std::int32_t>(numberIn(values, "lineSpacing", 160));
    view.values.emplace_back("lineSpacingView", percent ? static_cast<double>(spacing) : huToPt(spacing, 1));
    view.specs.push_back(choiceSpec("lineSpacingType", ids({"PERCENT", "FIXED", "BETWEEN_LINES", "AT_LEAST"})));
    view.specs.push_back(percent ? numberSpec("lineSpacingView", "%", 0, 500, 10, 0)
                                 : numberSpec("lineSpacingView", "pt", 0, 5000, 1, 1));
    view.values.emplace_back("condenseMin", 100.0 - numberIn(values, "condense"));
    view.specs.push_back(numberSpec("condenseMin", "%", 25, 100, 1, 0));
    view.values.emplace_back("hangulBreak",
                             std::string(textIn(values, "breakNonLatinWord") == "KEEP_WORD" ? "char" : "word"));
    view.specs.push_back(choiceSpec("hangulBreak", ids({"word", "char"})));
    const std::string latin = textIn(values, "breakLatinWord", "KEEP_WORD");
    view.values.emplace_back("latinBreak",
                             std::string(latin == "HYPHENATION" ? "hyphen" : (latin == "BREAK_WORD" ? "char" : "word")));
    view.specs.push_back(choiceSpec("latinBreak", ids({"word", "hyphen", "char"})));
    view.values.emplace_back("oneLine", textIn(values, "lineWrap") == "SQUEEZE");
    view.specs.push_back(choiceSpec("vertAlign", ids({"BASELINE", "TOP", "CENTER", "BOTTOM"})));
    view.specs.push_back(choiceSpec("headingType", ids({"NONE", "OUTLINE", "NUMBER", "BULLET"})));
    view.specs.push_back(choiceSpec("headingLevel", ids({"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"})));

    // 탭 목록은 칸마다 나눠 준다. 위치는 pt 다.
    const std::vector<model::TabStop> stops = parseTabStops(textIn(values, "tabStops"));
    view.values.emplace_back("tabCount", static_cast<double>(stops.size()));
    for (std::size_t i = 0; i < stops.size(); ++i) {
        const std::string p = "tab." + std::to_string(i) + ".";
        view.values.emplace_back(p + "pos@pt", huToPt(stops[i].pos, 1));
        view.values.emplace_back(p + "type", stops[i].type);
        view.values.emplace_back(p + "leader", stops[i].leader);
    }
    view.specs.push_back(choiceSpec("tabType", ids({"LEFT", "RIGHT", "CENTER", "DECIMAL"})));
    view.specs.push_back(choiceSpec("tabLeader", ids({"NONE", "SOLID", "DASH", "DOT", "DASH_DOT", "DASH_DOT_DOT",
                                                      "LONG_DASH", "CIRCLE", "DOUBLE_SLIM", "SLIM_THICK", "THICK_SLIM",
                                                      "SLIM_THICK_SLIM"})));
    view.specs.push_back(numberSpec("tabPos@pt", "pt", 0, 5000, 1, 1));

    addFillView("char.", values, view);
    addFillView("para.", values, view);
    return view;
}

FormatView pageView(const model::Document& doc, const ValueMap& values) {
    FormatView view;
    for (const auto& [key, value] : values) view.values.emplace_back(key, value);
    addLengths(FormatViewKind::Page, values, view);
    const double paperW = numberIn(values, "paperWidth");
    const double paperH = numberIn(values, "paperHeight");
    view.values.emplace_back("paperKind", paperKindOf(paperW, paperH));
    std::vector<std::pair<std::string, std::string>> kinds;
    for (const PaperKind& kind : kPaperKinds) kinds.emplace_back(kind.id, kind.label);
    kinds.emplace_back("CUSTOM", "사용자 정의");
    view.specs.push_back(choiceSpec("paperKind", kinds));
    std::vector<std::pair<std::string, std::string>> presets;
    for (const MarginPreset& preset : kMarginPresets) presets.emplace_back(preset.id, preset.id);
    view.specs.push_back(choiceSpec("marginPreset", presets));

    const double line = numberIn(values, "lineGrid");
    const double cell = numberIn(values, "charGrid");
    view.values.emplace_back("gridMode", std::string(line > 0 ? (cell > 0 ? "both" : "line") : "none"));
    view.specs.push_back(choiceSpec("gridMode", ids({"none", "line", "both"})));
    const PageBody body = pageBodyOf(values);
    view.values.emplace_back("lineCount", line > 0 ? std::floor(body.height / line) : 0.0);
    view.values.emplace_back("cellCount", cell > 0 ? std::floor(body.width / cell) : 0.0);
    view.specs.push_back(numberSpec("lineCount", "", 1, 500, 1, 0));
    view.specs.push_back(numberSpec("cellCount", "", 1, 500, 1, 0));
    // 격자를 끈 칸에는 기본 간격을 보인다.
    if (line <= 0) {
        for (auto& [key, value] : view.values) {
            if (key == "lineGrid@pt") value = 16.0;
        }
    }
    if (cell <= 0) {
        for (auto& [key, value] : view.values) {
            if (key == "charGrid@pt") value = 20.0;
        }
    }

    const double sections = std::max(1.0, numberIn(values, "sectionCount", 1));
    view.values.emplace_back("wholeDefault", sections <= 1);
    view.specs.push_back(sections > 1 ? choiceSpec("scope", ids({"section", "document"}))
                                      : choiceSpec("scope", ids({"document"})));

    // 용지 그림: 종이와 본문 칸, 제본 여백(홀수 쪽 기준). mm 다.
    const auto mmOf = [](double hu) { return huToMm(static_cast<std::int32_t>(std::llround(hu)), 2); };
    const bool top = numberIn(values, "gutterType") == 2;
    const double gutter = numberIn(values, "marginGutter");
    const double left = numberIn(values, "marginLeft") + (top ? 0 : gutter);
    const double upper = numberIn(values, "marginTop") + numberIn(values, "marginHeader") + (top ? gutter : 0);
    view.values.emplace_back("sheetW", mmOf(body.paperW));
    view.values.emplace_back("sheetH", mmOf(body.paperH));
    view.values.emplace_back("sheetBodyX", mmOf(left));
    view.values.emplace_back("sheetBodyY", mmOf(upper));
    view.values.emplace_back("sheetBodyW", mmOf(body.width));
    view.values.emplace_back("sheetBodyH", mmOf(body.height));
    view.values.emplace_back("sheetGutterX", mmOf(top ? 0 : numberIn(values, "marginLeft")));
    view.values.emplace_back("sheetGutterY", mmOf(top ? numberIn(values, "marginTop") : 0));
    view.values.emplace_back("sheetGutterW", mmOf(top ? body.paperW : gutter));
    view.values.emplace_back("sheetGutterH", mmOf(top ? gutter : body.paperH));
    (void)doc;
    return view;
}

}  // namespace

namespace {

// 쪽 테두리/배경: 고른 종류(which)의 값에 패치를 얹는다. 종류를 바꾸면 그 종류의 값을 새로 읽는다.
ValueMap resolvePageBorder(const model::Document& doc, const FormatFields& patch) {
    std::string which = "BOTH";
    for (const auto& [key, value] : patch) {
        if (key == "which") {
            if (const std::string* t = std::get_if<std::string>(&value)) which = *t;
        }
    }
    ValueMap values = mapOf(pageBorderValues(doc, which));
    for (const auto& [key, value] : patch) {
        if (key == "which") continue;
        values[key] = value;
    }
    return values;
}

FormatView pageBorderView(const ValueMap& values) {
    FormatView view;
    for (const auto& [key, value] : values) view.values.emplace_back(key, value);
    addFillView("page.", values, view);
    view.specs.push_back(choiceSpec("which", {{"BOTH", "양쪽"}, {"ODD", "홀수 쪽"}, {"EVEN", "짝수 쪽"}}));
    view.specs.push_back(choiceSpec("basis", {{"PAPER", "종이 기준"}, {"PAGE", "쪽 기준"}}));
    view.specs.push_back(choiceSpec("fillArea", {{"PAPER", "종이"}, {"PAGE", "쪽"}, {"BORDER", "테두리"}}));
    view.specs.push_back(choiceSpec("applyPages", {{"ALL", "모두"}, {"EXCEPT_FIRST", "첫 쪽 제외"}, {"FIRST_ONLY", "첫 쪽만"}}));
    for (const char* side : {"offsetLeft", "offsetTop", "offsetRight", "offsetBottom"}) {
        view.specs.push_back(numberSpec(side, "mm", 0, 100, 0.1, 1));
    }
    const double sections = std::max(1.0, numberIn(values, "sectionCount", 1));
    view.specs.push_back(sections > 1 ? choiceSpec("scope", ids({"section", "document"}))
                                      : choiceSpec("scope", ids({"document"})));
    return view;
}

}  // namespace

FormatFields resolveFormatView(const model::Document& doc, const std::vector<Node>& pages, FormatViewKind kind,
                               const FormatFields& viewPatch, const CaretPlace* base) {
    FormatFields out;
    if (kind == FormatViewKind::PageBorder) {
        for (const auto& [key, value] : resolvePageBorder(doc, viewPatch)) out.emplace_back(key, value);
        return out;
    }
    if (kind == FormatViewKind::Page) {
        for (const auto& [key, value] : resolvePage(mapOf(pageSetupAtCursor(doc)), viewPatch)) out.emplace_back(key, value);
        return out;
    }
    const FormatFields now =
        base ? formatFieldsOf(doc, cursorStyleForCaret(doc, *base),
                              doc.sections.empty() ? 4000 : doc.sections.front().defaultTab)
             : formatFieldsAtCursor(doc, pages);
    for (const auto& [key, value] : resolveText(mapOf(now), viewPatch)) {
        out.emplace_back(key, value);
    }
    return out;
}

FormatView formatView(const model::Document& doc, const std::vector<Node>& pages, FormatViewKind kind,
                      const FormatFields& viewPatch, const CaretPlace* base) {
    if (kind == FormatViewKind::Table) {
        FormatView view;
        view.values = {{"rows", static_cast<double>(kTableStartRows)}, {"cols", static_cast<double>(kTableStartCols)}};
        view.specs = {numberSpec("rows", "", 1, kTableMaxRows, 1, 0), numberSpec("cols", "", 1, kTableMaxCols, 1, 0)};
        return view;
    }
    if (kind == FormatViewKind::PageBorder) return pageBorderView(resolvePageBorder(doc, viewPatch));
    if (kind == FormatViewKind::Page) {
        ValueMap values = mapOf(pageSetupAtCursor(doc));
        for (const auto& [key, value] : resolvePage(values, viewPatch)) values[key] = value;
        return pageView(doc, values);
    }
    const FormatFields resolved = resolveFormatView(doc, pages, kind, viewPatch, base);
    return textView(fieldsOf(derive(doc, pages, resolved, base)));
}

PageBitmap renderFormatPreview(const model::Document& doc, const std::vector<Node>& pages, FormatViewKind kind,
                               const FormatFields& viewPatch, int width, int height, const CaretPlace* base) {
    if (kind == FormatViewKind::Page || kind == FormatViewKind::Table || kind == FormatViewKind::PageBorder || width <= 0 ||
        height < 32) {
        return {};
    }
    Derived d = derive(doc, pages, resolveFormatView(doc, pages, kind, viewPatch, base), base);
    model::Document& sample = d.scratch;

    // 앞뒤 문단은 흐린 글자로 쓴다.
    std::uint32_t mutedId = d.charShapeId;
    if (kind == FormatViewKind::Para) {
        if (const model::CharShape* base = Painter::findCharShape(sample, d.charShapeId)) {
            model::CharShape muted = *base;
            std::uint32_t maxId = 0;
            for (const model::CharShape& shape : sample.header.charShapes) maxId = std::max(maxId, shape.id);
            muted.id = maxId + 1;
            muted.textColor = 0xA3A3A3u;
            sample.header.charShapes.push_back(muted);
            mutedId = muted.id;
        }
    }
    // 글자 견본과 앞뒤 문단은 간격·여백·번호·테두리 없는 문단 모양으로 쓴다. 제목처럼 위 간격이 큰 문단도 견본이 보인다.
    std::uint32_t plainPara = d.paraShapeId;
    if (const model::ParaShape* base = Painter::findParaShape(sample, d.paraShapeId)) {
        model::ParaShape plain = *base;
        std::uint32_t maxId = 0;
        for (const model::ParaShape& shape : sample.header.paraShapes) maxId = std::max(maxId, shape.id);
        plain.id = maxId + 1;
        plain.spacingPrev = plain.spacingNext = 0;
        plain.marginLeft = plain.marginRight = plain.indent = 0;
        plain.headingType = "NONE";
        plain.headingIdRef = 0;
        plain.pageBreakBefore = false;
        plain.borderFillId = 0;
        sample.header.paraShapes.push_back(plain);
        plainPara = plain.id;
    }
    const auto paragraph = [&](const char* text, std::uint32_t charShape, std::uint32_t paraShape) {
        model::Paragraph para;
        para.paraShapeId = paraShape;
        para.styleId = d.styleId;
        model::Run run;
        run.charShapeId = charShape;
        run.items.push_back(model::InlineItem::makeText(text));
        para.runs.push_back(std::move(run));
        return para;
    };
    model::Section section;
    model::PageDef page;
    // 그림 칸 비율의 종이. 글자 견본은 한 줄 높이, 문단 견본은 세 문단이 들어가는 높이다.
    page.height = kind == FormatViewKind::Char ? 6000 : 24000;
    page.width = static_cast<std::uint32_t>(static_cast<double>(page.height) * width / height);
    page.marginLeft = page.marginRight = 600;
    page.marginTop = page.marginBottom = 600;
    section.page = page;
    if (kind == FormatViewKind::Char) {
        section.paragraphs.push_back(paragraph("한글Eng123漢字あいう※☉", d.charShapeId, plainPara));
    } else {
        section.paragraphs.push_back(paragraph("이전 문단 이전 문단 이전 문단 이전 문단 이전 문단 이전 문단", mutedId,
                                               plainPara));
        section.paragraphs.push_back(paragraph(
            "가나다라AaBbCc12345 가나다라AaBbCc12345 가나다라AaBbCc12345 가나다라AaBbCc12345 가나다라AaBbCc12345",
            d.charShapeId, d.paraShapeId));
        section.paragraphs.push_back(paragraph("다음 문단 다음 문단 다음 문단 다음 문단 다음 문단 다음 문단", mutedId,
                                               plainPara));
    }
    sample.sections.push_back(std::move(section));
    sample.header.sectionCount = 1;
    const std::vector<Node> laid = layoutPages(sample);
    if (laid.empty()) return {};
    return renderPageBitmap(sample, laid.front(), height, 0, 0, 0, 0, PaintLayer::All, 0, 250);
}

}  // namespace chwp::render
