// 표 구조 편집 명령. 구조를 고치는 규칙은 model/table_edit 이고, 여기서는 칸을 찾고, 표를 다시 놓고,
// 캐럿을 옮긴다. 되돌리기는 undo.cpp 의 editTable 이 감싼다.

#include "chwp/model/table_edit.hpp"
#include "chwp/model/table_formula.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "chwp/unit.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"
#include "render/scene_detail.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace chwp::render {

using namespace paint_detail;

namespace {

bool hasStory(const Node& node, int story, int depth) {
    if (node.story == story) return true;
    if (depth <= 0) return false;
    return std::any_of(node.children.begin(), node.children.end(),
                       [&](const Node& child) { return hasStory(child, story, depth - 1); });
}

// 그려 둔 칸에서 글 높이 + 위아래 안 여백을 잰다. 칸을 찾지 못하면 0.
std::int32_t drawnCellNeed(const std::vector<Node>& pages, int story) {
    std::vector<const Node*> stack;
    for (const Node& page : pages) stack.push_back(&page);
    while (!stack.empty()) {
        const Node* node = stack.back();
        stack.pop_back();
        if (node->kind == NodeKind::Cell) {
            for (const Node& content : node->children) {
                if (content.turned != 0 || !hasStory(content, story, 2)) continue;
                return node->marginTop + node->marginBottom + detail::rowContentBottom(content);
            }
        }
        for (const Node& child : node->children) stack.push_back(&child);
    }
    return 0;
}

// 표를 담은 문단에서 그 표 개체를 뺀다.
bool removeControl(model::Document& doc, int story, int paragraph, const model::Control* control) {
    model::Paragraph* host = storyParagraph(doc, story, paragraph);
    if (!host) return false;
    for (model::Run& run : host->runs) {
        for (auto it = run.items.begin(); it != run.items.end(); ++it) {
            if (it->type == model::InlineItem::Type::Control && &it->control == control) {
                run.items.erase(it);
                host->lineSegs.clear();
                return true;
            }
        }
    }
    return false;
}

// 그려 둔 칸 상자의 높이. 칸을 찾지 못하면 -1.
std::int32_t drawnCellHeight(const std::vector<Node>& pages, int story) {
    std::vector<const Node*> stack;
    for (const Node& page : pages) stack.push_back(&page);
    while (!stack.empty()) {
        const Node* node = stack.back();
        stack.pop_back();
        if (node->kind == NodeKind::Cell) {
            for (const Node& content : node->children) {
                if (content.turned == 0 && hasStory(content, story, 2)) return node->height;
            }
        }
        for (const Node& child : node->children) stack.push_back(&child);
    }
    return -1;
}

bool cellBorderKindOf(const std::string& name, model::CellBorderKind& kind) {
    static const std::pair<const char*, model::CellBorderKind> names[] = {
        {"all", model::CellBorderKind::All},           {"outside", model::CellBorderKind::Outside},
        {"inside", model::CellBorderKind::Inside},     {"top", model::CellBorderKind::Top},
        {"bottom", model::CellBorderKind::Bottom},     {"left", model::CellBorderKind::Left},
        {"right", model::CellBorderKind::Right},       {"insideHorz", model::CellBorderKind::InsideHorz},
        {"insideVert", model::CellBorderKind::InsideVert}, {"none", model::CellBorderKind::None},
        {"diagonalDown", model::CellBorderKind::DiagonalDown}, {"diagonalUp", model::CellBorderKind::DiagonalUp},
    };
    for (const auto& [n, k] : names) {
        if (name == n) {
            kind = k;
            return true;
        }
    }
    return false;
}

int cellStoryOf(model::Document& doc, const model::Control* control, int cell) {
    const model::StoryMap& map = ensureStoryMap(doc);
    for (int s = 1; s <= map.count(); ++s) {
        const model::StorySlot* slot = map.slot(s);
        if (slot && slot->control == control && slot->tableCell && slot->subIndex == cell &&
            map.storyOf(slot->paragraphs) == s) {
            return s;
        }
    }
    return -1;
}

// 한글 표 여백 목록(표 레이아웃 탭 표 여백). 값은 위·아래·왼쪽·오른쪽 mm 이고 HWPUNIT 은 버림이다
// (넓게 0.8 mm → 226, 3 mm → 850). outer 는 바깥 여백, inner 는 표 모든 셀의 안 여백이다.
bool marginPreset(const std::string& margin, const std::string& preset, std::int32_t out[4]) {
    struct Row {
        const char* name;
        double outer[4];
        double inner[4];
    };
    static const Row rows[] = {
        {"normal", {1.0, 1.0, 1.0, 1.0}, {0.5, 0.5, 1.8, 1.8}},
        {"wide", {3.0, 3.0, 3.0, 3.0}, {0.8, 0.8, 3.0, 3.0}},
        {"narrow", {0.5, 0.5, 0.5, 0.5}, {0.4, 0.4, 1.5, 1.5}},
        {"none", {0, 0, 0, 0}, {0, 0, 0, 0}},
    };
    if (margin != "outer" && margin != "inner") return false;
    for (const Row& row : rows) {
        if (preset != row.name) continue;
        const double* mm = margin == "outer" ? row.outer : row.inner;
        for (int i = 0; i < 4; ++i) out[i] = static_cast<std::int32_t>(mm[i] * 7200.0 / 25.4);
        return true;
    }
    return false;
}

const model::Style* captionStyle(const model::Document& doc) {
    for (const model::Style& style : doc.header.styles) {
        if (style.engName == "Caption") return &style;
    }
    for (const model::Style& style : doc.header.styles) {
        if (style.name == "캡션") return &style;
    }
    return nullptr;
}

// 문서 차례로 표 캡션 번호를 세어 control 의 번호를 정한다. control 앞의 표 번호 수 + 1.
int captionNumberOf(const model::Document& doc, const model::Control* control, const std::string& numType) {
    // 개체 번호 범주와 같으면 그 개체 번호다(캡션 없는 앞 개체도 센다).
    if (model::objectNumberingType(control->kind, control->placement ? control->placement->numberingType : "") == numType) {
        return model::objectNumber(doc, control);
    }
    int count = 0;
    bool found = false;
    std::function<void(const std::vector<model::Paragraph>&)> walk = [&](const std::vector<model::Paragraph>& paras) {
        for (const model::Paragraph& para : paras) {
            for (const model::Run& run : para.runs) {
                for (const model::InlineItem& item : run.items) {
                    if (found || item.type != model::InlineItem::Type::Control) continue;
                    const model::Control& c = item.control;
                    if (&c == control) {
                        found = true;
                        return;
                    }
                    if (c.kind == model::ControlKind::AutoNumber && c.numType == numType) ++count;
                    if (c.caption) walk(c.caption->content.paragraphs);
                    if (c.table) {
                        for (const model::TableCell& cell : c.table->cells) walk(cell.content.paragraphs);
                    }
                    for (const model::SubList& sub : c.subLists) walk(sub.paragraphs);
                }
            }
        }
    };
    for (const model::Section& section : doc.sections) {
        if (!found) walk(section.paragraphs);
    }
    return count + 1;
}

model::Control* tableIn(model::Paragraph* para) {
    if (!para) return nullptr;
    for (model::Run& run : para->runs) {
        for (model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control && item.control.table) return &item.control;
        }
    }
    return nullptr;
}

std::int32_t rowsHeight(const model::Table& table) {
    std::int32_t total = 0;
    for (const std::int32_t h : model::tableRowHeights(table)) total += h;
    return total;
}

}  // namespace

const char* tableEditName(TableEditKind kind) {
    switch (kind) {
        case TableEditKind::InsertRows: return "insertRows";
        case TableEditKind::InsertColumns: return "insertColumns";
        case TableEditKind::DeleteRows: return "deleteRows";
        case TableEditKind::DeleteColumns: return "deleteColumns";
        case TableEditKind::Merge: return "mergeCells";
        case TableEditKind::Split: return "splitCell";
        case TableEditKind::CellBorder: return "cellBorder";
        case TableEditKind::CellFill: return "cellFill";
        case TableEditKind::EqualWidth: return "equalWidth";
        case TableEditKind::EqualHeight: return "equalHeight";
        case TableEditKind::CellAlign: return "cellAlign";
        case TableEditKind::CellDirection: return "cellDirection";
        case TableEditKind::TableMargin: return "tableMargin";
        case TableEditKind::TreatAsChar: return "treatAsChar";
        case TableEditKind::Wrap: return "wrap";
        case TableEditKind::Caption: return "caption";
        case TableEditKind::SplitTable: return "splitTable";
        case TableEditKind::MergeTable: return "mergeTable";
        case TableEditKind::Comma: return "comma";
        case TableEditKind::Flip: return "flip";
        case TableEditKind::AutoFill: return "autoFill";
        case TableEditKind::Sort: return "sort";
        case TableEditKind::Formula: return "formula";
    }
    return "table";
}

bool tableEditKindOf(const std::string& name, TableEditKind& kind) {
    for (const TableEditKind k : {TableEditKind::InsertRows, TableEditKind::InsertColumns, TableEditKind::DeleteRows,
                                  TableEditKind::DeleteColumns, TableEditKind::Merge, TableEditKind::Split,
                                  TableEditKind::CellBorder, TableEditKind::CellFill, TableEditKind::EqualWidth,
                                  TableEditKind::EqualHeight, TableEditKind::CellAlign, TableEditKind::CellDirection,
                                  TableEditKind::TableMargin, TableEditKind::TreatAsChar, TableEditKind::Wrap,
                                  TableEditKind::Caption, TableEditKind::SplitTable, TableEditKind::MergeTable,
                                  TableEditKind::Comma, TableEditKind::Flip,
                                  TableEditKind::AutoFill, TableEditKind::Sort, TableEditKind::Formula}) {
        if (name == tableEditName(k)) {
            kind = k;
            return true;
        }
    }
    return false;
}

bool tableCommandAtCursor(const model::Document& doc, TableEditCommand& cmd) {
    const model::StoryMap& map = ensureStoryMap(doc);
    const auto cellStory = [&](const model::Control* control, int row, int col) {
        for (int s = 1; s <= map.count(); ++s) {
            const model::StorySlot* slot = map.slot(s);
            if (slot && slot->control == control && slot->tableCell && static_cast<int>(slot->tableCell->row) == row &&
                static_cast<int>(slot->tableCell->col) == col && map.storyOf(slot->paragraphs) == s) {
                return s;
            }
        }
        return -1;
    };
    const DragHost& drag = pointerHold().drag;
    if (drag.mode == DragMode::Cells && drag.control && drag.focusRow >= 0 && drag.focusCol >= 0) {
        cmd.story = cellStory(drag.control, drag.row, drag.col);
        cmd.toStory = cellStory(drag.control, drag.focusRow, drag.focusCol);
        return cmd.story > 0 && cmd.toStory > 0;
    }
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return false;
    const model::StorySlot* slot = map.valid(cur.story) ? map.slot(cur.story) : nullptr;
    if (!slot || !slot->tableCell || !slot->paragraphs) return false;
    cmd.story = map.storyOf(slot->paragraphs);
    cmd.toStory = -1;
    return true;
}

bool tableEditHost(const model::Document& doc, const TableEditCommand& cmd, int& story, int& paragraph) {
    const model::StorySlot* slot = resolveStory(doc, cmd.story);
    if (!slot || !slot->tableCell || !slot->control || !slot->control->table) return false;
    story = slot->parentStory;
    paragraph = slot->parentParagraph;
    return story > 0 && paragraph >= 0;
}

// 캡션 넣기·옮기기·떼기(한글 2024 TableCaptionPos*·CaptionPos*·DetachCaption). 표·그림·도형·수식이 같은 규칙이다:
// 폭 30 mm(8504), 간격 3 mm(850), "캡션" 스타일 문단에 번호 종류 이름("표 "·"그림 "·"수식 ") + 번호 + " ".
// 있는 캡션은 글을 두고 자리만 바꾼다. 가로 캡션의 lastWidth 는 개체 폭이다(한글 COM 으로 확인).
namespace paint_detail {

CaptionChange setObjectCaption(model::Document& doc, model::Control& control, const std::string& wanted, int hostStory,
                               int hostParagraph, bool& created, std::string& error) {
    created = false;
    // TOGGLE 은 한글 캡션 넣기(Ctrl+N,C, 화면으로 확인): 캡션이 없으면 아래에 넣고, 있으면 없앤다. 그림·표 모두 아래다.
    const std::string caption = wanted == "TOGGLE" ? std::string(control.caption ? "NONE" : "BOTTOM") : wanted;
    if (caption == "NONE") {
        if (!control.caption) return CaptionChange::Same;
        control.caption.reset();
        return CaptionChange::Changed;
    }
    std::string side;
    std::string vert = "TOP";
    if (caption == "TOP" || caption == "BOTTOM") {
        side = caption;
    } else {
        const std::size_t cut = caption.find('_');
        side = cut == std::string::npos ? std::string() : caption.substr(0, cut);
        vert = cut == std::string::npos ? std::string() : caption.substr(cut + 1);
        if ((side != "LEFT" && side != "RIGHT") || (vert != "TOP" && vert != "CENTER" && vert != "BOTTOM")) {
            error = "캡션 자리 " + caption + " 는 모릅니다.";
            return CaptionChange::Refused;
        }
    }
    const bool horizontal = side == "TOP" || side == "BOTTOM";
    const std::uint32_t objectWidth = control.placement ? control.placement->width : 0;
    if (control.caption) {
        control.caption->side = side;
        control.caption->content.vertAlign = vert;
        control.caption->lastWidth = horizontal ? objectWidth : control.caption->width;
        for (model::Paragraph& para : control.caption->content.paragraphs) para.lineSegs.clear();
        return CaptionChange::Changed;
    }
    std::string numType = control.table ? std::string("TABLE") : std::string("PICTURE");
    if (control.placement && !control.placement->numberingType.empty() && control.placement->numberingType != "NONE") {
        numType = control.placement->numberingType;
    }
    const char* label = numType == "TABLE" ? "표 " : numType == "EQUATION" ? "수식 " : "그림 ";
    model::Caption cap;
    cap.side = side;
    cap.width = 8504;
    cap.gap = 850;
    cap.lastWidth = horizontal ? objectWidth : 8504;
    cap.content.vertAlign = vert;
    model::Paragraph para;
    model::Run run;
    if (const model::Style* style = captionStyle(doc)) {
        para.styleId = style->id;
        para.paraShapeId = style->paraShapeId;
        run.charShapeId = style->charShapeId;
    } else if (const model::Paragraph* host = storyParagraph(doc, hostStory, hostParagraph)) {
        para.styleId = host->styleId;
        para.paraShapeId = host->paraShapeId;
        run.charShapeId = host->runs.empty() ? 0 : host->runs.front().charShapeId;
    }
    model::Control number;
    number.kind = model::ControlKind::AutoNumber;
    number.ctrlId = "atno";
    number.numType = numType;
    number.numFormat = "DIGIT";
    number.number = static_cast<std::uint32_t>(captionNumberOf(doc, &control, numType));
    run.items.push_back(model::InlineItem::makeText(label));
    run.items.push_back(model::InlineItem::makeControl(std::move(number)));
    run.items.push_back(model::InlineItem::makeText(" "));
    para.runs.push_back(std::move(run));
    cap.content.paragraphs.push_back(std::move(para));
    control.caption = std::move(cap);
    created = true;
    return CaptionChange::Changed;
}

// 고른 개체(그림·도형·수식·글상자)의 캡션. 표 캡션과 같은 규칙이고, 새 캡션이면 캐럿을 캡션 끝에 둔다.
EditResult objectCaptionNow(model::Document& doc, std::vector<Node>& pages, const std::string& caption,
                            bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    PointerHold& hold = pointerHold();
    const Node* picked = hold.selectedObject;
    if (!picked || picked->ext().obj().anchorStory <= 0 || picked->ext().obj().anchorItem < 0) {
        why = "캡션을 달 개체를 고르십시오.";
        return {};
    }
    const int story = picked->ext().obj().anchorStory;
    const int paragraph = picked->ext().obj().anchorParagraph;
    const int item = picked->ext().obj().anchorItem;
    const int page = hold.boxes.empty() ? -1 : hold.boxes.front().page;
    model::Control* control = anchoredControl(doc, story, paragraph, item);
    if (!control) {
        why = "캡션을 달 개체를 고르십시오.";
        return {};
    }
    bool created = false;
    const CaptionChange change = setObjectCaption(doc, *control, caption, story, paragraph, created, why);
    if (change == CaptionChange::Refused) return {};
    if (change == CaptionChange::Same) return makeEditResult(doc, true, false, story, paragraph);
    clearComposition(hold);
    if (model::Paragraph* host = storyParagraph(doc, story, paragraph)) host->lineSegs.clear();
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    hold.selectedObject = nullptr;
    hold.boxes.clear();
    hold.objectHandles = false;
    const int captionStory = created && control->caption ? ensureStoryMap(doc).storyOf(&control->caption->content.paragraphs) : -1;
    if (captionStory > 0 && !control->caption->content.paragraphs.empty()) {
        const int end = paragraphCharCount(control->caption->content.paragraphs.front());
        setPointerCursor(doc, pages, model::CursorPos{captionStory, 0, charIndexToOffset(doc, pages, captionStory, 0, end)});
    } else {
        reselectAnchoredObject(doc, pages, page, story, paragraph, item);
    }
    return makeEditResult(doc, true, true, story, paragraph, &relayout);
}

}  // namespace paint_detail

namespace {

// 표 개체 자체를 고치는 명령(표 여백, 글자처럼 취급, 배치, 캡션, 표 나누기·붙이기). 규칙은 한글 2024 에서 확인했다.
// 문서 전체를 다시 짠다. 떠 있는 표는 다른 글이 비켜 가고, 나누기·붙이기는 문단이 바뀐다.
EditResult tableObjectNow(model::Document& doc, std::vector<Node>& pages, const TableEditCommand& cmd,
                          model::Control& control, int hostStory, int hostParagraph, int caretRow,
                          bool useStoredLineHeight, std::string& error) {
    model::Table& table = *control.table;
    const PointerCursor caretBefore = pointerCursor();
    const DragHost block = pointerHold().drag;
    int caretStory = -1;  // 고친 뒤 캐럿을 둘 글 묶음(0 번 문단). -1 이면 캐럿·블록을 그대로 둔다
    int caretEnd = 0;     // 그 문단의 글자 번호
    const model::Control* focusTable = nullptr;  // 캐럿을 그 표 첫 칸에 둔다
    const model::Control* captionOf = nullptr;   // 캐럿을 그 표 캡션 끝에 둔다
    std::int32_t values[4] = {};

    switch (cmd.kind) {
        case TableEditKind::TableMargin: {
            if (!marginPreset(cmd.margin, cmd.preset, values)) {
                error = "표 여백 " + cmd.margin + ":" + cmd.preset + " 은 모릅니다.";
                return {};
            }
            if (cmd.margin == "inner") {
                table.marginTop = values[0];
                table.marginBottom = values[1];
                table.marginLeft = values[2];
                table.marginRight = values[3];
                for (model::TableCell& c : table.cells) {
                    for (model::Paragraph& para : c.content.paragraphs) para.lineSegs.clear();
                }
            } else {
                if (!control.placement) control.placement = model::Placement{};
                control.placement->marginTop = values[0];
                control.placement->marginBottom = values[1];
                control.placement->marginLeft = values[2];
                control.placement->marginRight = values[3];
            }
            break;
        }
        case TableEditKind::TreatAsChar: {
            // 한글은 글자처럼 취급만 뒤집고 배치·기준·오프셋은 그대로 둔다.
            if (!control.placement) control.placement = model::Placement{};
            control.placement->treatAsChar = !control.placement->treatAsChar;
            break;
        }
        case TableEditKind::Wrap: {
            if (cmd.wrap != "SQUARE" && cmd.wrap != "TOP_AND_BOTTOM" && cmd.wrap != "IN_FRONT_OF_TEXT" &&
                cmd.wrap != "BEHIND_TEXT") {
                error = "배치 " + cmd.wrap + " 는 모릅니다.";
                return {};
            }
            // 글자처럼 취급인 표는 한글에서 배치 단추가 꺼져 있다.
            if (!control.placement || control.placement->treatAsChar) {
                error = "글자처럼 취급한 표는 배치를 고를 수 없습니다.";
                return {};
            }
            control.placement->textWrap = cmd.wrap;
            break;
        }
        case TableEditKind::Caption: {
            bool created = false;
            const CaptionChange change = setObjectCaption(doc, control, cmd.caption, hostStory, hostParagraph, created, error);
            if (change == CaptionChange::Refused) return {};
            if (change == CaptionChange::Same) return makeEditResult(doc, true, false, hostStory, hostParagraph);
            if (created) captionOf = &control;  // 새 캡션이면 캐럿을 캡션 끝에 둔다
            break;
        }
        case TableEditKind::SplitTable: {
            model::Table tail;
            const model::TableEdit edit = model::splitTableRows(table, caretRow, tail);
            if (!edit.ok()) {
                error = edit.error;
                return {};
            }
            std::vector<model::Paragraph>* paras = storyParagraphs(doc, hostStory);
            model::Paragraph* host = storyParagraph(doc, hostStory, hostParagraph);
            if (!paras || !host) return {};
            // 뗀 줄은 새 문단의 새 표다. 캡션은 앞 표에 남는다.
            model::Control second = control;
            second.caption.reset();
            second.table = std::move(tail);
            if (control.placement) control.placement->height = static_cast<std::uint32_t>(std::max(1, rowsHeight(table)));
            if (second.placement) {
                second.placement->height = static_cast<std::uint32_t>(std::max(1, rowsHeight(*second.table)));
            }
            model::Paragraph next;
            next.paraShapeId = host->paraShapeId;
            next.styleId = host->styleId;
            model::Run run;
            run.charShapeId = host->runs.empty() ? 0 : host->runs.front().charShapeId;
            run.items.push_back(model::InlineItem::makeControl(std::move(second)));
            next.runs.push_back(std::move(run));
            host->lineSegs.clear();
            paras->insert(paras->begin() + hostParagraph + 1, std::move(next));
            focusTable = tableIn(storyParagraph(doc, hostStory, hostParagraph + 1));
            break;
        }
        case TableEditKind::MergeTable: {
            model::Paragraph* nextPara = storyParagraph(doc, hostStory, hostParagraph + 1);
            model::Control* next = tableIn(nextPara);
            if (!next || next == &control) {
                error = "바로 뒤에 붙일 표가 없습니다.";
                return {};
            }
            const std::int32_t nextHeight = next->placement ? static_cast<std::int32_t>(next->placement->height) : 0;
            const model::TableEdit edit = model::appendTableRows(table, *next->table);
            if (!edit.ok()) {
                error = edit.error;
                return {};
            }
            if (control.placement) {
                control.placement->width = static_cast<std::uint32_t>(
                    std::max<std::int64_t>(1, static_cast<std::int64_t>(control.placement->width) + edit.widthDelta));
                control.placement->height += static_cast<std::uint32_t>(std::max(0, nextHeight));
            }
            // 뒤 표가 있던 문단은 빈 문단으로 남는다(한글 2024).
            for (model::Run& run : nextPara->runs) {
                for (auto it = run.items.begin(); it != run.items.end(); ++it) {
                    if (it->type == model::InlineItem::Type::Control && &it->control == next) {
                        run.items.erase(it);
                        break;
                    }
                }
            }
            nextPara->lineSegs.clear();
            focusTable = &control;
            break;
        }
        default: return {};
    }
    clearComposition(pointerHold());
    pointerHold().drag = DragHost{};
    if (model::Paragraph* host = storyParagraph(doc, hostStory, hostParagraph)) host->lineSegs.clear();
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;

    if (focusTable) {
        caretStory = cellStoryOf(doc, focusTable, 0);
    } else if (captionOf && captionOf->caption) {
        caretStory = ensureStoryMap(doc).storyOf(&captionOf->caption->content.paragraphs);
        if (caretStory > 0 && !captionOf->caption->content.paragraphs.empty()) {
            caretEnd = paragraphCharCount(captionOf->caption->content.paragraphs.front());
        }
    }
    if (caretStory > 0) {
        setPointerCursor(doc, pages,
                         model::CursorPos{caretStory, 0, charIndexToOffset(doc, pages, caretStory, 0, caretEnd)});
    } else if (caretBefore.active && caretBefore.story > 0 && resolveStory(doc, caretBefore.story)) {
        setPointerCursor(doc, pages, model::CursorPos{caretBefore.story, caretBefore.paragraph, caretBefore.offset});
        if (block.mode == DragMode::Cells) {
            pointerHold().drag = block;
            rememberSelection(doc, pages);
        }
    } else {
        setPointerCursor(doc, pages, model::CursorPos{hostStory, hostParagraph, 0});
    }
    return makeEditResult(doc, true, true, hostStory, hostParagraph, &relayout);
}

}  // namespace

EditResult paint_detail::tableEditNow(model::Document& doc, std::vector<Node>& pages, const TableEditCommand& cmd,
                                      bool useStoredLineHeight, std::string& error) {
    if (readOnly()) {
        error = "읽기 전용 문서입니다.";
        return {};
    }
    model::StorySlot* slot = resolveStory(doc, cmd.story);
    if (!slot || !slot->tableCell || !slot->control || !slot->control->table || slot->subIndex < 0) {
        error = "story " + std::to_string(cmd.story) + " 은 표 칸이 아닙니다.";
        return {};
    }
    model::Control* control = slot->control;
    model::Table& table = *control->table;
    const int hostStory = slot->parentStory;
    const int hostParagraph = slot->parentParagraph;
    const int cell = slot->subIndex;
    int other = cell;
    if (cmd.toStory > 0 && cmd.toStory != cmd.story) {
        const model::StorySlot* to = resolveStory(doc, cmd.toStory);
        if (!to || to->control != control || !to->tableCell || to->subIndex < 0) {
            error = "story " + std::to_string(cmd.toStory) + " 은 같은 표의 칸이 아닙니다.";
            return {};
        }
        other = to->subIndex;
    }
    const model::TableCell a = table.cells[static_cast<std::size_t>(cell)];
    const model::TableCell b = table.cells[static_cast<std::size_t>(other)];
    const int row0 = static_cast<int>(std::min(a.row, b.row));
    const int col0 = static_cast<int>(std::min(a.col, b.col));
    const int row1 = static_cast<int>(std::max(a.row + std::max(1u, a.rowSpan), b.row + std::max(1u, b.rowSpan))) - 1;
    const int col1 = static_cast<int>(std::max(a.col + std::max(1u, a.colSpan), b.col + std::max(1u, b.colSpan))) - 1;

    if (cmd.kind == TableEditKind::TableMargin || cmd.kind == TableEditKind::TreatAsChar ||
        cmd.kind == TableEditKind::Wrap || cmd.kind == TableEditKind::Caption ||
        cmd.kind == TableEditKind::SplitTable || cmd.kind == TableEditKind::MergeTable) {
        return tableObjectNow(doc, pages, cmd, *control, hostStory, hostParagraph, static_cast<int>(a.row),
                              useStoredLineHeight, error);
    }

    model::TableEdit edit;
    switch (cmd.kind) {
        case TableEditKind::InsertRows: edit = model::insertTableRows(table, cell, cmd.after, cmd.count); break;
        case TableEditKind::InsertColumns: edit = model::insertTableColumns(table, cell, cmd.after, cmd.count); break;
        case TableEditKind::DeleteRows: edit = model::deleteTableRows(table, row0, row1); break;
        case TableEditKind::DeleteColumns: edit = model::deleteTableColumns(table, col0, col1); break;
        case TableEditKind::Merge: edit = model::mergeTableCells(table, row0, col0, row1, col1); break;
        case TableEditKind::CellBorder:
        case TableEditKind::CellFill: {
            model::BorderFillAccess fills;
            fills.read = [&doc](std::uint32_t id) { return borderFillOf(doc, id); };
            fills.intern = [&doc](const model::BorderFill& f) { return internBorderFillOf(doc, f); };
            if (cmd.kind == TableEditKind::CellFill) {
                const std::string color = cmd.color == "none" ? std::string() : cmd.color;
                edit = model::fillTableCells(table, row0, col0, row1, col1, color, fills);
                break;
            }
            model::CellBorderKind border = model::CellBorderKind::All;
            if (!cellBorderKindOf(cmd.border, border)) {
                error = "테두리 " + cmd.border + " 는 모릅니다.";
                return {};
            }
            const model::BorderSide line{cmd.lineType.empty() ? std::string("SOLID") : cmd.lineType,
                                         cmd.lineColor.empty() ? std::string("#000000") : cmd.lineColor,
                                         cmd.lineWidth > 0 ? cmd.lineWidth : mmToHu(0.12)};
            edit = model::setTableCellBorders(table, row0, col0, row1, col1, border, line, fills);
            break;
        }
        case TableEditKind::AutoFill:
            // 한글 도움말: 채울 칸을 셀 블록으로 잡은 다음 실행한다.
            if (pointerHold().drag.mode != DragMode::Cells) {
                error = "표 자동 채우기는 칸 블록을 잡은 뒤에 합니다.";
                return {};
            }
            edit = model::autoFillTableCells(table, row0, col0, row1, col1);
            break;
        case TableEditKind::Sort: {
            // 한글은 칸 블록에서만 정렬한다(캐럿만 있으면 Sort 매개변수가 없어 실행되지 않는다).
            if (pointerHold().drag.mode != DragMode::Cells) {
                error = "표 정렬은 두 줄 이상을 칸 블록으로 잡은 뒤에 합니다.";
                return {};
            }
            std::vector<model::TableSortKey> keys;
            for (const TableEditCommand::SortKey& k : cmd.sortKeys) {
                keys.push_back(model::TableSortKey{k.column, k.format, k.descending});
            }
            if (keys.empty()) keys.push_back(model::TableSortKey{});
            model::TableSortOptions options;
            options.fromRear = cmd.sortFromRear;
            options.extendYear = cmd.sortExtendYear;
            options.yearBase = cmd.sortYearBase;
            options.oldHangul = cmd.sortOldHangul;
            options.finalFirst = cmd.sortFinalFirst;
            edit = model::sortTableRows(table, row0, col0, row1, col1, keys, options);
            break;
        }
        case TableEditKind::Formula: {
            const model::FormulaFunc func = cmd.formulaFunc == 1   ? model::FormulaFunc::Average
                                            : cmd.formulaFunc == 2 ? model::FormulaFunc::Product
                                                                   : model::FormulaFunc::Sum;
            const bool cells = pointerHold().drag.mode == DragMode::Cells;
            if (cmd.formulaMode == "block") {
                // 한글 블록 계산식은 칸 블록에서만 한다.
                if (!cells) {
                    error = "블록 계산식은 칸 블록을 잡은 뒤에 합니다.";
                    return {};
                }
                if (model::insertBlockFormulas(table, row0, col0, row1, col1, func) == 0) {
                    error = "블록의 마지막 줄이나 칸에 수가 있어 결과를 넣을 곳이 없습니다.";
                    return {};
                }
                edit.focusCell = cell;
                break;
            }
            // 쉬운 계산식·계산식: 칸 블록이면 끝 칸, 아니면 캐럿 칸.
            const int target = cells ? other : cell;
            const int targetStory = cells ? cmd.toStory : cmd.story;
            const model::TableCell& t = table.cells[static_cast<std::size_t>(target)];
            std::string formula = cmd.formulaText;
            std::string format = cmd.formulaFormat.empty() ? std::string("%g,") : cmd.formulaFormat;
            if (cmd.formulaMode == "horizontal" || cmd.formulaMode == "vertical") {
                const bool horizontal = cmd.formulaMode == "horizontal";
                const bool last = horizontal ? t.col + std::max(1u, t.colSpan) >= table.colCount
                                             : t.row + std::max(1u, t.rowSpan) >= table.rowCount;
                formula = model::easyFormula(func, horizontal, last);
                format = model::formulaDefaultFormat(func);
            } else if (cmd.formulaMode != "custom") {
                error = "계산식 종류 " + cmd.formulaMode + " 은 모릅니다.";
                return {};
            }
            if (!placeTableFormula(doc, pages, table, target, targetStory > 0 ? targetStory : cmd.story, formula, format,
                                   error)) {
                return {};
            }
            edit.focusCell = target;
            break;
        }
        case TableEditKind::Flip: {
            if (cmd.flip < 0 || cmd.flip > 5) {
                error = "표 뒤집기 " + std::to_string(cmd.flip) + " 은 모릅니다.";
                return {};
            }
            model::BorderFillAccess fills;
            fills.read = [&doc](std::uint32_t id) { return borderFillOf(doc, id); };
            fills.intern = [&doc](const model::BorderFill& f) { return internBorderFillOf(doc, f); };
            edit = model::flipTable(table, static_cast<model::TableFlip>(cmd.flip), cmd.swapMargin, fills, cell);
            break;
        }
        case TableEditKind::EqualWidth: edit = model::equalizeTableCells(table, row0, col0, row1, col1, false); break;
        case TableEditKind::CellAlign: {
            edit = model::alignTableCells(table, row0, col0, row1, col1, cmd.vertAlign);
            if (!edit.ok() || cmd.horzAlign.empty()) break;
            if (cmd.horzAlign != "LEFT" && cmd.horzAlign != "CENTER" && cmd.horzAlign != "RIGHT" &&
                cmd.horzAlign != "JUSTIFY") {
                error = "가로 정렬 " + cmd.horzAlign + " 은 모릅니다.";
                return {};
            }
            // 가로 정렬은 칸 문단의 문단 모양이다(한글 TableCellAlign*).
            FormatPatch patch;
            patch.hasAlign = true;
            patch.align = cmd.horzAlign;
            for (model::TableCell& c : table.cells) {
                if (static_cast<int>(c.row) < row0 || static_cast<int>(c.row + std::max(1u, c.rowSpan)) - 1 > row1 ||
                    static_cast<int>(c.col) < col0 || static_cast<int>(c.col + std::max(1u, c.colSpan)) - 1 > col1) {
                    continue;
                }
                for (model::Paragraph& para : c.content.paragraphs) {
                    para.paraShapeId = deriveParaShapeId(doc, para.paraShapeId, patch);
                    para.lineSegs.clear();
                }
            }
            break;
        }
        case TableEditKind::CellDirection:
            edit = model::setTableCellDirection(table, row0, col0, row1, col1,
                                                static_cast<std::uint8_t>(std::clamp(cmd.direction, 0, 2)));
            break;
        case TableEditKind::Comma: {
            // 한글은 칸 블록(또는 고른 표)일 때만 한다.
            if (pointerHold().drag.mode != DragMode::Cells) {
                error = "1,000 단위 구분 쉼표는 칸 블록을 잡은 뒤에 합니다.";
                return {};
            }
            if (cmd.comma != "insert" && cmd.comma != "delete") {
                error = "쉼표 " + cmd.comma + " 는 모릅니다.";
                return {};
            }
            bool changed = false;
            for (model::TableCell& c : table.cells) {
                if (static_cast<int>(c.row) < row0 || static_cast<int>(c.row + std::max(1u, c.rowSpan)) - 1 > row1 ||
                    static_cast<int>(c.col) < col0 || static_cast<int>(c.col + std::max(1u, c.colSpan)) - 1 > col1) {
                    continue;
                }
                for (model::Paragraph& para : c.content.paragraphs) {
                    changed = rewriteNumberCommas(para, cmd.comma == "insert") || changed;
                }
            }
            if (!changed) return makeEditResult(doc, false, false, hostStory, hostParagraph);
            break;
        }
        case TableEditKind::TableMargin:
        case TableEditKind::TreatAsChar:
        case TableEditKind::Wrap:
        case TableEditKind::Caption:
        case TableEditKind::SplitTable:
        case TableEditKind::MergeTable: break;
        case TableEditKind::EqualHeight: {
            // 한글은 그려진 줄 높이(글이 늘린 높이까지)로 나눈다. 칸마다 그린 높이로 줄 높이를 푼다.
            std::vector<model::SpanSize> spans;
            for (int i = 0; i < static_cast<int>(table.cells.size()); ++i) {
                const model::TableCell& c = table.cells[static_cast<std::size_t>(i)];
                const int story = cellStoryOf(doc, control, i);
                const std::int32_t drawn = story > 0 ? drawnCellHeight(pages, story) : -1;
                spans.push_back(model::SpanSize{c.row, static_cast<int>(std::max(1u, c.rowSpan)),
                                                drawn > 0 ? drawn : static_cast<std::int32_t>(c.height)});
            }
            edit = model::equalizeTableCells(table, row0, col0, row1, col1, true,
                                             model::solveSpanSizes(table.rowCount, std::move(spans)));
            break;
        }
        case TableEditKind::Split: {
            // 한글은 나눈 첫 줄에 그 칸 글의 높이를, 가운데 줄에 빈 줄 높이를 준다.
            const std::int32_t margins = a.hasMargin ? a.marginTop + a.marginBottom : table.marginTop + table.marginBottom;
            std::int32_t lineHeight = 1000;
            if (!a.content.paragraphs.empty() && !a.content.paragraphs.front().runs.empty()) {
                const std::uint32_t id = a.content.paragraphs.front().runs.front().charShapeId;
                if (const model::CharShape* shape = Painter::findCharShape(doc, id); shape && shape->height > 0) {
                    lineHeight = shape->height;
                }
            }
            const std::int32_t drawn = drawnCellNeed(pages, cmd.story);
            edit = model::splitTableCell(table, cell, cmd.rows, cmd.cols, drawn > 0 ? drawn : margins + lineHeight,
                                         margins + lineHeight);
            break;
        }
    }
    if (!edit.ok()) {
        error = edit.error;
        return {};
    }
    clearComposition(pointerHold());
    // 테두리·배경은 칸 자리를 바꾸지 않는다. 칸 블록을 남겨 이어서 고치게 한다.
    const bool keepBlock = cmd.kind == TableEditKind::CellBorder || cmd.kind == TableEditKind::CellFill ||
                           cmd.kind == TableEditKind::CellAlign || cmd.kind == TableEditKind::CellDirection ||
                           cmd.kind == TableEditKind::Comma || cmd.kind == TableEditKind::AutoFill ||
                           cmd.kind == TableEditKind::Sort ||
                           (cmd.kind == TableEditKind::Formula && cmd.formulaMode == "block");
    const DragHost block = pointerHold().drag;
    const PointerCursor caretBefore = pointerCursor();
    pointerHold().drag = DragHost{};

    if (edit.removeTable) {
        if (!removeControl(doc, hostStory, hostParagraph, control)) {
            error = "표를 지우지 못했습니다.";
            return {};
        }
    } else if (control->placement && cmd.kind == TableEditKind::Flip) {
        // 줄·칸이 맞바뀌면 표 바깥 크기도 맞바꾼다. 한글은 높이에 줄 높이 합을, 폭에 그린 폭을 적는데, 옛 폭(칸 폭 합)과
        // 옛 그린 높이가 그 둘이다. 나머지 뒤집기는 크기가 그대로다.
        if (cmd.flip == 2 || cmd.flip == 3 || cmd.flip == 5) std::swap(control->placement->width, control->placement->height);
    } else if (control->placement) {
        model::Placement& placement = *control->placement;
        placement.width = static_cast<std::uint32_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(placement.width) + edit.widthDelta));
        placement.height = static_cast<std::uint32_t>(std::max<std::int64_t>(1, static_cast<std::int64_t>(placement.height) + edit.heightDelta));
    }
    rebuildStoryMap(doc);

    // 글자처럼 놓인 표는 제 문단만 다시 놓는다. 떠 있는 표는 다른 글이 비켜 가므로 전체를 다시 놓는다.
    EditRelayoutResult relayout;
    const bool inlineTable = edit.removeTable || !control->placement || control->placement->treatAsChar;
    if (inlineTable) {
        relayout = relayoutObjectParagraph(doc, pages, hostStory, hostParagraph, useStoredLineHeight);
    } else {
        if (model::Paragraph* host = storyParagraph(doc, hostStory, hostParagraph)) host->lineSegs.clear();
        invalidateLinePlaces({}, true);
        model::ensureParagraphIds(doc);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
    }

    // 캐럿: 고친 뒤의 칸 첫 글자. 표를 지웠으면 표가 있던 자리. 테두리·배경은 캐럿·블록을 그대로 둔다.
    if (keepBlock) {
        if (caretBefore.active && caretBefore.story > 0) {
            setPointerCursor(doc, pages, model::CursorPos{caretBefore.story, caretBefore.paragraph, caretBefore.offset});
        }
        if (block.mode == DragMode::Cells) {
            pointerHold().drag = block;
            rememberSelection(doc, pages);
        }
    } else if (edit.removeTable) {
        setPointerCursor(doc, pages, model::CursorPos{hostStory, hostParagraph, 0});
    } else if (const int focus = cellStoryOf(doc, control, edit.focusCell); focus > 0) {
        setPointerCursor(doc, pages, model::CursorPos{focus, 0, 0});
    }
    return makeEditResult(doc, true, true, hostStory, hostParagraph, &relayout);
}

}  // namespace chwp::render
