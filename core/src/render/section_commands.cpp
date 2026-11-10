// 구역 설정(한글 2024 구역 설정 대화 상자).
//
// 칸과 값은 한글 2024 대화 상자와 저장 결과로 확인했다(HWP 구역 정의 비트와 HWPX secPr 이 같은 값이다):
// - 시작 쪽 번호 종류: 이어서(0), 짝수(1), 홀수(2), 사용자(3, 쪽 번호 값). 사용자면 HWPX 에 pageStartsOn 을 쓰지 않는다.
// - 개체 시작 번호(그림·표·수식): 이어서(0) 또는 사용자 번호.
// - 첫 쪽에만 머리말/꼬리말 감추기는 머리말·꼬리말 둘 다, 첫 쪽에만 테두리/배경 감추기는 테두리·배경 둘 다 HIDE_FIRST 다.
// - 단 사이 간격·기본 탭 간격은 pt 로 보인다(11.3pt = 1134, 40.0pt = 4000).
// - 적용 범위는 구역이 하나면 문서 전체·새 구역으로다. 구역이 여럿이면 현재 구역도 준다.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

const FormatValue* fieldOf(const FormatFields& fields, const std::string& key) {
    const FormatValue* found = nullptr;
    for (const auto& [name, value] : fields) {
        if (name == key) found = &value;
    }
    return found;
}
std::string textOf(const FormatFields& fields, const std::string& key, const std::string& fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const std::string* t = std::get_if<std::string>(v)) return *t;
    }
    return fallback;
}
double numberOf(const FormatFields& fields, const std::string& key, double fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const double* d = std::get_if<double>(v)) return *d;
    }
    return fallback;
}
bool flagOf(const FormatFields& fields, const std::string& key, bool fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const bool* b = std::get_if<bool>(v)) return *b;
    }
    return fallback;
}

const char* const kStartsOn[] = {"CONTINUE", "EVEN", "ODD", "USER"};

double huToPt(std::int32_t hu) { return std::round(hu / 10.0) / 10.0; }
std::int32_t ptToHu(double pt) { return static_cast<std::int32_t>(std::lround(pt * 100.0)); }

// 구역 첫 문단의 구역 정의 컨트롤.
model::Control* sectionDefOf(model::Section& section) {
    if (section.paragraphs.empty()) return nullptr;
    for (model::Run& run : section.paragraphs.front().runs) {
        for (model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::SectionDef) {
                return &item.control;
            }
        }
    }
    return nullptr;
}

// 고른 값을 구역과 그 구역 정의 컨트롤에 같이 넣는다. 저장은 둘 중 하나를 먼저 보는 곳이 있다.
void applyTo(model::Section& section, const FormatFields& fields) {
    model::Control* def = sectionDefOf(section);
    const std::string startsOn = textOf(fields, "startsOn", kStartsOn[std::min<int>(section.startsOn, 3)]);
    std::uint8_t kind = 0;
    for (std::uint8_t i = 0; i < 4; ++i) {
        if (startsOn == kStartsOn[i]) kind = i;
    }
    const auto pageNumber = static_cast<std::uint32_t>(
        std::clamp(std::lround(numberOf(fields, "pageNumber", section.pageStart > 0 ? section.pageStart : 1)), 1L, 65535L));
    const std::uint32_t pageStart = kind == 3 ? pageNumber : 0;
    const auto objectStart = [&](const char* key, std::uint16_t now) {
        const double v = numberOf(fields, key, now);
        return static_cast<std::uint16_t>(std::clamp(std::lround(v), 0L, 65535L));
    };
    const std::uint16_t picture = objectStart("pictureStart", section.pictureStart);
    const std::uint16_t table = objectStart("tableStart", section.tableStart);
    const std::uint16_t equation = objectStart("equationStart", section.equationStart);
    const bool headFoot = flagOf(fields, "hideFirstHeaderFooter", section.hideFirstHeader && section.hideFirstFooter);
    const bool master = flagOf(fields, "hideFirstMaster", section.hideFirstMaster);
    const bool borderFill =
        flagOf(fields, "hideFirstBorderFill", section.borderShow == "HIDE_FIRST" && section.fillShow == "HIDE_FIRST");
    const bool emptyLine = flagOf(fields, "hideEmptyLine", section.hideFirstEmptyLine);
    const std::int32_t gap = std::max(0, ptToHu(numberOf(fields, "columnGapPt", huToPt(section.columnRegionGap))));
    const std::int32_t tab = std::max(1, ptToHu(numberOf(fields, "defaultTabPt", huToPt(section.defaultTab))));
    // 줄 번호(한글 줄 번호 설정 대화 상자: 시작 번호, 본문과의 간격, 번호 간격, 쪽마다·구역마다·이어서).
    // 처음 켤 때 값이 비어 있으면 한글 ShowLineNumbers 의 기본값(이어서 3, 간격 1, 5mm, 시작 1)이다.
    model::LineNumberShape lineNumbers = section.lineNumbers;
    lineNumbers.show = flagOf(fields, "lineNumberShow", lineNumbers.show);
    if (lineNumbers.show && lineNumbers.countBy == 0 && lineNumbers.distance == 0 && lineNumbers.start == 0) {
        lineNumbers.restart = 3;
        lineNumbers.countBy = 1;
        lineNumbers.distance = 1417;
        lineNumbers.start = 1;
    }
    // 준 값만 바꾼다(대화 상자를 그냥 닫아도 파일의 값이 바뀌지 않게).
    const auto given = [&](const char* key) {
        return std::any_of(fields.begin(), fields.end(), [&](const auto& field) { return field.first == key; });
    };
    if (given("lineNumberRestart")) {
        lineNumbers.restart =
            static_cast<std::uint8_t>(std::clamp(std::lround(numberOf(fields, "lineNumberRestart", 3)), 0L, 3L));
    }
    if (given("lineNumberCountBy")) {
        lineNumbers.countBy =
            static_cast<std::uint16_t>(std::clamp(std::lround(numberOf(fields, "lineNumberCountBy", 1)), 1L, 100L));
    }
    if (given("lineNumberStart")) {
        lineNumbers.start =
            static_cast<std::uint16_t>(std::clamp(std::lround(numberOf(fields, "lineNumberStart", 1)), 1L, 65535L));
    }
    if (given("lineNumberDistanceMm")) {
        lineNumbers.distance = static_cast<std::uint32_t>(
            std::clamp(std::lround(numberOf(fields, "lineNumberDistanceMm", 5) * 7200.0 / 25.4), 0L, 200000L));
    }

    const auto put = [&](auto& target) {
        target.startsOn = kind;
        target.pictureStart = picture;
        target.tableStart = table;
        target.equationStart = equation;
        target.hideFirstHeader = headFoot;
        target.hideFirstFooter = headFoot;
        target.hideFirstMaster = master;
        // 테두리·배경 칸 하나다. 끄면 첫 쪽 감춤만 푼다(첫 쪽만 보이기는 이 대화 상자에 없다).
        if (borderFill) {
            target.borderShow = "HIDE_FIRST";
            target.fillShow = "HIDE_FIRST";
        } else {
            if (target.borderShow == "HIDE_FIRST") target.borderShow = "SHOW_ALL";
            if (target.fillShow == "HIDE_FIRST") target.fillShow = "SHOW_ALL";
        }
        target.hideFirstEmptyLine = emptyLine;
        target.columnRegionGap = gap;
        target.defaultTab = tab;
        target.lineNumbers = lineNumbers;
    };
    put(section);
    section.pageStart = pageStart;
    if (def) {
        put(*def);
        def->number = pageStart;
    }
    for (model::Paragraph& para : section.paragraphs) para.lineSegs.clear();
}

int caretSection(const model::Document& doc) { return std::max(0, cursorSection(doc)); }

}  // namespace

FormatFields sectionSettingsView(const model::Document& doc) {
    FormatFields out;
    if (doc.sections.empty()) return out;
    const model::Section& section = doc.sections[static_cast<std::size_t>(caretSection(doc))];
    const int kind = section.pageStart > 0 && section.startsOn == 0 ? 3 : std::min<int>(section.startsOn, 3);
    out.emplace_back("startsOn", std::string(kStartsOn[kind]));
    out.emplace_back("pageNumber", static_cast<double>(section.pageStart > 0 ? section.pageStart : 1));
    out.emplace_back("pictureStart", static_cast<double>(section.pictureStart));
    out.emplace_back("tableStart", static_cast<double>(section.tableStart));
    out.emplace_back("equationStart", static_cast<double>(section.equationStart));
    out.emplace_back("hideFirstHeaderFooter", section.hideFirstHeader && section.hideFirstFooter);
    out.emplace_back("hideFirstMaster", section.hideFirstMaster);
    out.emplace_back("hideFirstBorderFill", section.borderShow == "HIDE_FIRST" && section.fillShow == "HIDE_FIRST");
    out.emplace_back("hideEmptyLine", section.hideFirstEmptyLine);
    out.emplace_back("columnGapPt", huToPt(section.columnRegionGap));
    out.emplace_back("defaultTabPt", huToPt(section.defaultTab));
    out.emplace_back("lineNumberShow", section.lineNumbers.show);
    out.emplace_back("lineNumberRestart", static_cast<double>(section.lineNumbers.restart == 1 || section.lineNumbers.restart == 2
                                                                    ? section.lineNumbers.restart
                                                                    : 3));
    out.emplace_back("lineNumberCountBy", static_cast<double>(std::max<int>(1, section.lineNumbers.countBy)));
    out.emplace_back("lineNumberStart", static_cast<double>(std::max<int>(1, section.lineNumbers.start)));
    out.emplace_back("lineNumberDistanceMm",
                     std::round((section.lineNumbers.countBy == 0 && section.lineNumbers.distance == 0 ? 1417 : section.lineNumbers.distance) *
                                254.0 / 7200.0) / 10.0);
    out.emplace_back("applyOptions", std::string(doc.sections.size() > 1 ? "CURRENT,WHOLE,NEW_SECTION" : "WHOLE,NEW_SECTION"));
    out.emplace_back("applyTo", std::string(doc.sections.size() > 1 ? "CURRENT" : "WHOLE"));
    return out;
}

EditResult sectionSettingsNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    if (doc.sections.empty()) return {};
    PointerHold& hold = pointerHold();
    if (hold.bandStory > 0) {
        why = "머리말·꼬리말을 고치는 중에는 할 수 없습니다.";
        return {};
    }
    clearComposition(hold);
    const std::string target = textOf(fields, "applyTo", doc.sections.size() > 1 ? "CURRENT" : "WHOLE");
    if (target == "NEW_SECTION") {
        // 캐럿에서 구역을 나누고(구역 나누기) 새 구역에 넣는다.
        const EditResult split = insertBreakNow(doc, pages, BreakKind::Section, useStoredLineHeight);
        if (!split.documentChanged) {
            why = "새 구역은 본문에서만 나눌 수 있습니다.";
            return {};
        }
        applyTo(doc.sections[static_cast<std::size_t>(caretSection(doc))], fields);
    } else if (target == "CURRENT") {
        applyTo(doc.sections[static_cast<std::size_t>(caretSection(doc))], fields);
    } else {
        for (model::Section& section : doc.sections) applyTo(section, fields);
    }
    const PointerCursor before = pointerCursor();
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    if (before.active && before.story > 0) {
        setPointerCursor(doc, pages, model::CursorPos{before.story, before.paragraph, before.offset});
    }
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

}  // namespace chwp::render
