// 표 계산식 명령(한글 2024 계산식). 계산 규칙은 model/table_formula.cpp.
//   - 계산식·쉬운 계산식은 캐럿 자리에 계산식 필드(%fmu)를 넣는다. 셀 원래 글은 그대로다. 칸 블록이면 끝 칸 맨 앞이다.
//   - 캐럿이 계산식 결과 안이면 넣지 않고 그 식을 고친다(한글 계산식 고치기).
//   - 다시 계산은 편집 마무리 알림이 한다(edit_events.cpp).
#include <string>
#include <vector>

#include "chwp/model/table_formula.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

bool formulaField(const model::InlineItem& item) {
    return item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::Field &&
           item.control.ctrlId == "%fmu";
}

// 문단 글자 자리 charIndex 가 든 계산식 필드(결과 글 앞·안·뒤). 없으면 거짓.
bool formulaFieldAt(const model::Paragraph& para, int charIndex, std::size_t& outRun, std::size_t& outItem) {
    int chars = 0;
    bool open = false;
    int openAt = 0;
    std::size_t openRun = 0;
    std::size_t openItem = 0;
    for (std::size_t r = 0; r < para.runs.size(); ++r) {
        for (std::size_t k = 0; k < para.runs[r].items.size(); ++k) {
            const model::InlineItem& item = para.runs[r].items[k];
            if (item.type == model::InlineItem::Type::Text) {
                chars += utf8CodePointCount(item.text);
                continue;
            }
            if (formulaField(item)) {
                open = true;
                openAt = chars;
                openRun = r;
                openItem = k;
            } else if (item.control.kind == model::ControlKind::FieldEnd && open) {
                open = false;
                if (charIndex >= openAt && charIndex <= chars) {
                    outRun = openRun;
                    outItem = openItem;
                    return true;
                }
            } else if (isCaretCharControl(item.control)) {
                ++chars;
            }
        }
    }
    return false;
}

}  // namespace

bool paint_detail::placeTableFormula(model::Document& doc, const std::vector<Node>& pages, model::Table& table, int cell,
                                     int cellStory, const std::string& formula, const std::string& format,
                                     std::string& error) {
    if (cell < 0 || cell >= static_cast<int>(table.cells.size())) return false;
    model::TableCell& target = table.cells[static_cast<std::size_t>(cell)];
    if (target.content.paragraphs.empty()) target.content.paragraphs.emplace_back();
    // 캐럿이 이 칸에 있으면 그 자리, 아니면 칸 맨 앞.
    int paraIdx = 0;
    int charIndex = 0;
    const PointerCursor cur = pointerCursor();
    if (cur.active && cur.story == cellStory && !cur.hasSelection && pointerHold().drag.mode != DragMode::Cells &&
        cur.paragraph >= 0 &&
        cur.paragraph < static_cast<int>(target.content.paragraphs.size())) {
        paraIdx = cur.paragraph;
        charIndex = offsetToCharIndex(doc, pages, cellStory, paraIdx, cur.offset);
    }
    model::Paragraph& para = target.content.paragraphs[static_cast<std::size_t>(paraIdx)];
    model::FormulaCommand f;
    f.formula = formula;
    f.format = format;
    f.hasFormat = true;
    std::size_t run = 0;
    std::size_t item = 0;
    if (formulaFieldAt(para, charIndex, run, item)) {
        para.runs[run].items[item].control.command = model::formulaCommandText(f);
        model::refreshFormulaField(table, static_cast<std::size_t>(cell), para, run, item);
        return true;
    }
    if (formula.empty()) {
        error = "계산식을 넣으십시오.";
        return false;
    }
    f.result = model::formulaResultText(table, static_cast<std::size_t>(cell), f.formula, f.format);
    const std::uint32_t shape = charShapeAtParagraphChar(para, charIndex, false);
    model::Paragraph right = splitParagraphAt(para, charIndex);
    para.runs.push_back(model::formulaFieldRun(model::formulaCommandText(f), shape));
    for (model::Run& r : right.runs) {
        if (!r.items.empty()) para.runs.push_back(std::move(r));
    }
    para.lineSegs.clear();
    return true;
}

FormatFields tableFormulaView(const model::Document& doc, const std::vector<Node>& pages) {
    FormatFields out;
    const PointerCursor cur = pointerCursor();
    const model::StorySlot* slot = cur.active && cur.story > 0 ? resolveStory(doc, cur.story) : nullptr;
    if (!slot || !slot->tableCell) {
        out.emplace_back("inTable", false);
        return out;
    }
    out.emplace_back("inTable", true);
    const model::TableCell& cell = *slot->tableCell;
    std::string name;
    for (int c = static_cast<int>(cell.col) + 1; c > 0; c = (c - 1) / 26) name.insert(name.begin(), static_cast<char>('A' + (c - 1) % 26));
    out.emplace_back("cell", name + std::to_string(cell.row + 1));
    if (!cur.hasSelection && cur.paragraph >= 0 && cur.paragraph < static_cast<int>(cell.content.paragraphs.size())) {
        const model::Paragraph& para = cell.content.paragraphs[static_cast<std::size_t>(cur.paragraph)];
        std::size_t run = 0;
        std::size_t item = 0;
        if (formulaFieldAt(para, offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset), run, item)) {
            const model::FormulaCommand f = model::parseFormulaCommand(para.runs[run].items[item].control.command);
            out.emplace_back("formula", f.formula);
            out.emplace_back("format", f.hasFormat ? f.format : std::string("%g"));
        }
    }
    return out;
}

}  // namespace chwp::render
