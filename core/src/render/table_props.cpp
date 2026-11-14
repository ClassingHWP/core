// 표/셀 속성(한글 2024 표/셀 속성 대화 상자의 기본·여백/캡션·표·셀 탭).
//
// 칸 이름은 한글 2024 대화 상자 차례다(Hwp.Dlg 의 개체 속성 탭: 기본, 여백/캡션, 테두리, 배경, 표, 셀).
// 이번에는 모델에 값이 있는 칸만 고친다: 기본(글자처럼 취급, 본문과의 배치, 가로·세로 기준·정렬·거리,
// 쪽 영역 안으로 제한, 서로 겹침 허용, 번호 종류), 여백/캡션(바깥 여백, 캡션 자리·크기·간격·여백 부분까지 너비 확대),
// 표(쪽 경계 나누기, 제목 줄 자동 반복, 모든 셀의 안 여백), 셀(안 여백, 제목 셀, 세로쓰기, 세로 정렬, 필드 이름).
// 셀 필드 이름은 칸 블록의 모든 셀에 준다. 표 계산식 LEFT:이름·ALL:이름 이 이 이름을 쓴다.
// 표·셀 크기(보기만), 회전·기울이기·표 필드 이름·테두리/배경 탭은 아직 없다(테두리·배경은 표 디자인 탭에서 고친다).
// 셀 크기는 한 칸에 줄 때 한글이 열 전체를 바꾸는지 COM 으로 확인하지 못해 고치지 않는다.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "chwp/model/table_edit.hpp"
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
double mm(std::int64_t hu) { return std::round(static_cast<double>(hu) * 254.0 / 7200.0) / 10.0; }
std::int32_t hu(double millimetres) { return static_cast<std::int32_t>(std::lround(millimetres * 7200.0 / 25.4)); }

// 캐럿 표와 칸 범위.
struct TablePlace {
    model::Control* control = nullptr;
    int row0 = 0, col0 = 0, row1 = 0, col1 = 0;
    int story = -1;
};

bool placeOf(model::Document& doc, TablePlace& out) {
    TableEditCommand cmd;
    if (!tableCommandAtCursor(doc, cmd)) return false;
    model::StorySlot* slot = resolveStory(doc, cmd.story);
    if (!slot || !slot->control || !slot->control->table || !slot->tableCell) return false;
    model::TableCell a = *slot->tableCell;
    model::TableCell b = a;
    if (cmd.toStory > 0) {
        if (const model::StorySlot* to = resolveStory(doc, cmd.toStory);
            to && to->control == slot->control && to->tableCell) {
            b = *to->tableCell;
        }
    }
    out.control = slot->control;
    out.story = cmd.story;
    out.row0 = static_cast<int>(std::min(a.row, b.row));
    out.col0 = static_cast<int>(std::min(a.col, b.col));
    out.row1 = static_cast<int>(std::max(a.row + std::max(1u, static_cast<unsigned>(a.rowSpan)),
                                         b.row + std::max(1u, static_cast<unsigned>(b.rowSpan)))) - 1;
    out.col1 = static_cast<int>(std::max(a.col + std::max(1u, static_cast<unsigned>(a.colSpan)),
                                         b.col + std::max(1u, static_cast<unsigned>(b.colSpan)))) - 1;
    return true;
}

bool inBlock(const model::TableCell& c, const TablePlace& at) {
    const int rowEnd = static_cast<int>(c.row) + std::max(1, static_cast<int>(c.rowSpan)) - 1;
    const int colEnd = static_cast<int>(c.col) + std::max(1, static_cast<int>(c.colSpan)) - 1;
    return static_cast<int>(c.row) >= at.row0 && rowEnd <= at.row1 && static_cast<int>(c.col) >= at.col0 &&
           colEnd <= at.col1;
}

// 캡션 자리 이름(표 레이아웃 탭 캡션 목록과 같다).
std::string captionName(const model::Control& control) {
    if (!control.caption) return "NONE";
    const std::string& side = control.caption->side;
    if (side == "TOP" || side == "BOTTOM") return side;
    const std::string valign = control.caption->content.vertAlign.empty() ? "TOP" : control.caption->content.vertAlign;
    return side + "_" + valign;
}

}  // namespace

FormatFields tablePropertiesView(model::Document& doc) {
    FormatFields out;
    TablePlace at;
    if (!placeOf(doc, at)) return out;
    const model::Control& control = *at.control;
    const model::Table& table = *control.table;
    const model::Placement place = control.placement.value_or(model::Placement{});
    out.emplace_back("widthMm", mm(place.width));
    out.emplace_back("heightMm", mm(place.height));
    out.emplace_back("treatAsChar", place.treatAsChar);
    out.emplace_back("wrap", place.textWrap.empty() ? std::string("TOP_AND_BOTTOM") : place.textWrap);
    out.emplace_back("horzRelTo", place.horzRelTo.empty() ? std::string("COLUMN") : place.horzRelTo);
    out.emplace_back("horzAlign", place.horzAlign.empty() ? std::string("LEFT") : place.horzAlign);
    out.emplace_back("horzOffsetMm", mm(place.horzOffset));
    out.emplace_back("vertRelTo", place.vertRelTo.empty() ? std::string("PARA") : place.vertRelTo);
    out.emplace_back("vertAlign", place.vertAlign.empty() ? std::string("TOP") : place.vertAlign);
    out.emplace_back("vertOffsetMm", mm(place.vertOffset));
    out.emplace_back("flowWithText", place.flowWithText);
    out.emplace_back("allowOverlap", place.allowOverlap);
    out.emplace_back("numberingType", place.numberingType.empty() ? std::string("TABLE") : place.numberingType);
    out.emplace_back("outLeftMm", mm(place.marginLeft));
    out.emplace_back("outRightMm", mm(place.marginRight));
    out.emplace_back("outTopMm", mm(place.marginTop));
    out.emplace_back("outBottomMm", mm(place.marginBottom));
    out.emplace_back("caption", captionName(control));
    out.emplace_back("captionWidthMm", mm(control.caption ? control.caption->width : 8504));
    out.emplace_back("captionGapMm", mm(control.caption ? control.caption->gap : 850));
    out.emplace_back("captionFullSize", control.caption && control.caption->fullSize);
    out.emplace_back("pageBreak", table.pageBreak.empty() ? std::string("CELL") : table.pageBreak);
    out.emplace_back("repeatHeader", table.repeatHeader);
    out.emplace_back("inLeftMm", mm(table.marginLeft));
    out.emplace_back("inRightMm", mm(table.marginRight));
    out.emplace_back("inTopMm", mm(table.marginTop));
    out.emplace_back("inBottomMm", mm(table.marginBottom));
    // 셀 탭: 칸 블록(없으면 캐럿 칸)의 첫 칸 값.
    const model::TableCell* first = nullptr;
    for (const model::TableCell& c : table.cells) {
        if (!first && inBlock(c, at)) first = &c;
    }
    if (first) {
        out.emplace_back("cellWidthMm", mm(first->width));
        out.emplace_back("cellHeightMm", mm(first->height));
        out.emplace_back("cellMargin", first->hasMargin);
        out.emplace_back("cellLeftMm", mm(first->hasMargin ? first->marginLeft : table.marginLeft));
        out.emplace_back("cellRightMm", mm(first->hasMargin ? first->marginRight : table.marginRight));
        out.emplace_back("cellTopMm", mm(first->hasMargin ? first->marginTop : table.marginTop));
        out.emplace_back("cellBottomMm", mm(first->hasMargin ? first->marginBottom : table.marginBottom));
        out.emplace_back("header", first->header);
        out.emplace_back("direction", static_cast<double>(first->content.textDirection));
        out.emplace_back("cellVertAlign", first->vertAlign.empty() ? std::string("CENTER") : first->vertAlign);
        out.emplace_back("fieldName", first->name);
    }
    return out;
}

EditResult tablePropertiesNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    TablePlace at;
    if (!placeOf(doc, at)) {
        why = "캐럿이 표 칸에 있지 않습니다.";
        return {};
    }
    // 캡션 자리는 표 레이아웃 탭 캡션 명령과 같은 일이다(새 캡션 글·번호를 넣는다). 표 개체는 그 자리에 있어
    // 컨트롤 주소는 그대로이고, 캐럿은 끝에 블록 첫 칸으로 되돌린다.
    const std::string caption = textOf(fields, "caption", captionName(*at.control));
    if (caption != captionName(*at.control)) {
        TableEditCommand cmd;
        cmd.kind = TableEditKind::Caption;
        cmd.caption = caption;
        cmd.story = at.story;
        const EditResult made = tableEditNow(doc, pages, cmd, useStoredLineHeight, why);
        if (!made.handled) return {};
    }
    model::Control& control = *at.control;
    model::Table& table = *control.table;
    if (!control.placement) control.placement = model::Placement{};
    model::Placement& place = *control.placement;
    place.treatAsChar = flagOf(fields, "treatAsChar", place.treatAsChar);
    place.textWrap = textOf(fields, "wrap", place.textWrap);
    place.horzRelTo = textOf(fields, "horzRelTo", place.horzRelTo);
    place.horzAlign = textOf(fields, "horzAlign", place.horzAlign);
    place.horzOffset = hu(numberOf(fields, "horzOffsetMm", mm(place.horzOffset)));
    place.vertRelTo = textOf(fields, "vertRelTo", place.vertRelTo);
    place.vertAlign = textOf(fields, "vertAlign", place.vertAlign);
    place.vertOffset = hu(numberOf(fields, "vertOffsetMm", mm(place.vertOffset)));
    place.flowWithText = flagOf(fields, "flowWithText", place.flowWithText);
    place.allowOverlap = flagOf(fields, "allowOverlap", place.allowOverlap);
    place.numberingType = textOf(fields, "numberingType", place.numberingType);
    place.marginLeft = std::max(0, hu(numberOf(fields, "outLeftMm", mm(place.marginLeft))));
    place.marginRight = std::max(0, hu(numberOf(fields, "outRightMm", mm(place.marginRight))));
    place.marginTop = std::max(0, hu(numberOf(fields, "outTopMm", mm(place.marginTop))));
    place.marginBottom = std::max(0, hu(numberOf(fields, "outBottomMm", mm(place.marginBottom))));
    if (control.caption) {
        control.caption->width = static_cast<std::uint32_t>(std::max(0, hu(numberOf(fields, "captionWidthMm", mm(control.caption->width)))));
        control.caption->gap = hu(numberOf(fields, "captionGapMm", mm(control.caption->gap)));
        control.caption->fullSize = flagOf(fields, "captionFullSize", control.caption->fullSize);
    }
    const std::string pageBreak = textOf(fields, "pageBreak", table.pageBreak);
    if (pageBreak == "NONE" || pageBreak == "TABLE" || pageBreak == "CELL") table.pageBreak = pageBreak;
    table.repeatHeader = flagOf(fields, "repeatHeader", table.repeatHeader);
    table.marginLeft = std::max(0, hu(numberOf(fields, "inLeftMm", mm(table.marginLeft))));
    table.marginRight = std::max(0, hu(numberOf(fields, "inRightMm", mm(table.marginRight))));
    table.marginTop = std::max(0, hu(numberOf(fields, "inTopMm", mm(table.marginTop))));
    table.marginBottom = std::max(0, hu(numberOf(fields, "inBottomMm", mm(table.marginBottom))));

    for (model::TableCell& c : table.cells) {
        if (!inBlock(c, at)) continue;
        if (fieldOf(fields, "cellMargin")) {
            c.hasMargin = flagOf(fields, "cellMargin", c.hasMargin);
            if (c.hasMargin) {
                c.marginLeft = std::max(0, hu(numberOf(fields, "cellLeftMm", mm(c.marginLeft))));
                c.marginRight = std::max(0, hu(numberOf(fields, "cellRightMm", mm(c.marginRight))));
                c.marginTop = std::max(0, hu(numberOf(fields, "cellTopMm", mm(c.marginTop))));
                c.marginBottom = std::max(0, hu(numberOf(fields, "cellBottomMm", mm(c.marginBottom))));
            }
        }
        c.header = flagOf(fields, "header", c.header);
        c.name = textOf(fields, "fieldName", c.name);
        for (model::Paragraph& para : c.content.paragraphs) para.lineSegs.clear();
    }
    // 세로 정렬·글자 방향은 표 레이아웃 탭 단추와 같은 일이다.
    if (fieldOf(fields, "cellVertAlign")) {
        const model::TableEdit aligned =
            model::alignTableCells(table, at.row0, at.col0, at.row1, at.col1, textOf(fields, "cellVertAlign", "TOP"));
        if (!aligned.ok()) {
            why = aligned.error;
            return {};
        }
    }
    if (fieldOf(fields, "direction")) {
        model::setTableCellDirection(table, at.row0, at.col0, at.row1, at.col1,
                                     static_cast<std::uint8_t>(std::clamp(static_cast<int>(numberOf(fields, "direction", 0)), 0, 2)));
    }
    clearComposition(pointerHold());
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    const model::StoryMap& map = ensureStoryMap(doc);
    for (int st = 1; st <= map.count(); ++st) {
        const model::StorySlot* slot = map.slot(st);
        if (slot && slot->control == &control && slot->tableCell && static_cast<int>(slot->tableCell->row) == at.row0 &&
            static_cast<int>(slot->tableCell->col) == at.col0) {
            setPointerCursor(doc, pages, model::CursorPos{st, 0, 0});
            break;
        }
    }
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

}  // namespace chwp::render
