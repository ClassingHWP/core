// 표를 문자열로·문자열을 표로(한글 2024 TableTableToString·TableStringToTable, 숨긴 한글 COM 으로 확인).
//   표를 문자열로: 캐럿이 든 표를 줄마다 문단 하나로 바꾼다. 칸 사이에는 구분자(탭·쉼표·빈칸·기타 문자열)를 넣고,
//     칸 안 여러 문단은 문단을 나눠 둔다(a2 탭 "b2 x" / "second" 탭 c2). 칸 글의 글자 모양은 그대로다.
//   문자열을 표로: 블록이 걸친 문단마다 줄 하나, 칸 수는 가장 많이 나뉜 줄이다(모자란 칸은 빈칸).
//     자동으로 넣기는 탭과 3칸 이상 빈칸으로 나눈다. 분리 방법 지정은 탭·쉼표·3칸 이상 빈칸·기타 문자(한 글자만,
//     두 글자 이상이면 나누지 않는다)·문자열(그 문자열 그대로)이다. 구분자 유지면 구분자를 칸 글 끝에 둔다.
//     블록이 없으면 하지 않는다. 선 그리기 문자는 한글에서도 COM 으로 되지 않아 두지 않았다.

#include <algorithm>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
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
std::string textOf(const FormatFields& fields, const std::string& key, const std::string& fallback = {}) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const std::string* t = std::get_if<std::string>(v)) return *t;
    }
    return fallback;
}
bool flagOf(const FormatFields& fields, const std::string& key) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const double* d = std::get_if<double>(v)) return *d != 0;
        if (const bool* b = std::get_if<bool>(v)) return *b;
    }
    return false;
}

EditResult relaidAll(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight, int story, int paragraph,
                     int caretChar) {
    pointerHold().preferredX = -1;
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    setPointerCursor(doc, pages, model::CursorPos{story, paragraph, charIndexToOffset(doc, pages, story, paragraph, caretChar)});
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    return makeEditResult(doc, true, true, story, paragraph, &relayout);
}

// 문단 글(조판 부호 칸 없이)을 구분 규칙으로 나눈다.
std::vector<std::string> splitFields(const std::string& line, const std::string& how, const std::string& extra, bool keep) {
    std::vector<std::string> out;
    std::string cur;
    const auto cut = [&](const std::string& sep) {
        if (keep) cur += sep;
        out.push_back(cur);
        cur.clear();
    };
    std::size_t i = 0;
    while (i < line.size()) {
        if ((how == "auto" || how == "tab") && line[i] == '\t') {
            cut("\t");
            ++i;
            continue;
        }
        if (how == "comma" && line[i] == ',') {
            cut(",");
            ++i;
            continue;
        }
        if ((how == "auto" || how == "space") && line[i] == ' ') {
            std::size_t j = i;
            while (j < line.size() && line[j] == ' ') ++j;
            if (j - i >= 3) {
                cut(line.substr(i, j - i));
                i = j;
                continue;
            }
        }
        if (how == "char" && utf8CodePointCount(extra) == 1 && line.compare(i, extra.size(), extra) == 0) {
            cut(extra);
            i += extra.size();
            continue;
        }
        if (how == "string" && !extra.empty() && line.compare(i, extra.size(), extra) == 0) {
            cut(extra);
            i += extra.size();
            continue;
        }
        cur += line[i];
        ++i;
    }
    out.push_back(cur);
    return out;
}

}  // namespace

EditResult tableToTextNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    const PointerCursor cur = pointerCursor();
    const model::StorySlot* slot = cur.active && cur.story > 0 ? resolveStory(doc, cur.story) : nullptr;
    // 칸 안 글이면 그 표, 표 안의 표면 가장 안쪽 표다.
    while (slot && !(slot->tableCell && slot->control && slot->control->table)) {
        slot = slot->parentStory > 0 ? resolveStory(doc, slot->parentStory) : nullptr;
    }
    if (!slot) {
        why = "캐럿을 표 안에 두십시오.";
        return {};
    }
    const int hostStory = slot->parentStory;
    const int hostParagraph = slot->parentParagraph;
    const model::Control* tableControl = slot->control;
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, hostStory);
    if (!paras || hostParagraph < 0 || hostParagraph >= static_cast<int>(paras->size())) return {};
    model::Paragraph& host = (*paras)[static_cast<std::size_t>(hostParagraph)];

    const std::string how = textOf(fields, "delimiter", "tab");
    const std::string sep = how == "comma" ? "," : how == "space" ? " " : how == "other" ? textOf(fields, "text") : "\t";

    // 표가 든 run·item 자리와 그 앞뒤 글.
    std::vector<model::Run> before;
    std::vector<model::Run> after;
    const model::Control* found = nullptr;
    std::uint32_t tableShape = 0;
    model::Table table;
    for (model::Run& run : host.runs) {
        model::Run left{run.charShapeId, {}};
        model::Run right{run.charShapeId, {}};
        for (model::InlineItem& item : run.items) {
            if (!found && item.type == model::InlineItem::Type::Control && &item.control == tableControl) {
                found = &item.control;
                tableShape = run.charShapeId;
                table = *item.control.table;
                continue;
            }
            (found ? right : left).items.push_back(item);
        }
        if (!left.items.empty()) before.push_back(std::move(left));
        if (!right.items.empty()) after.push_back(std::move(right));
    }
    if (!found) return {};

    // 줄마다 문단. 칸은 줄·칸 차례다.
    std::vector<const model::TableCell*> cells;
    for (const model::TableCell& cell : table.cells) cells.push_back(&cell);
    std::stable_sort(cells.begin(), cells.end(), [](const model::TableCell* a, const model::TableCell* b) {
        return a->row != b->row ? a->row < b->row : a->col < b->col;
    });
    std::vector<model::Paragraph> lines;
    const auto newLine = [&]() {
        model::Paragraph p;
        p.paraShapeId = host.paraShapeId;
        p.styleId = host.styleId;
        lines.push_back(std::move(p));
    };
    int row = -1;
    for (const model::TableCell* cell : cells) {
        if (static_cast<int>(cell->row) != row) {
            row = static_cast<int>(cell->row);
            newLine();
        } else if (!sep.empty()) {
            lines.back().runs.push_back(model::Run{tableShape, {model::InlineItem::makeText(sep)}});
        }
        for (std::size_t k = 0; k < cell->content.paragraphs.size(); ++k) {
            if (k > 0) newLine();
            for (const model::Run& run : cell->content.paragraphs[k].runs) lines.back().runs.push_back(run);
        }
    }
    if (lines.empty()) newLine();
    // 표 앞 글은 첫 줄 앞에, 표 뒤 글은 마지막 줄 뒤에 둔다.
    lines.front().runs.insert(lines.front().runs.begin(), before.begin(), before.end());
    for (model::Run& r : after) lines.back().runs.push_back(std::move(r));
    lines.front().pageBreak = host.pageBreak;
    lines.front().columnBreak = host.columnBreak;
    for (model::Paragraph& p : lines) {
        if (p.runs.empty()) p.runs.push_back(model::Run{tableShape, {}});
    }
    int caretChar = 0;
    for (const model::Run& r : before) {
        for (const model::InlineItem& item : r.items) {
            caretChar += item.type == model::InlineItem::Type::Text ? utf8CodePointCount(item.text)
                                                                     : (isCaretCharControl(item.control) ? 1 : 0);
        }
    }
    clearComposition(pointerHold());
    paras->erase(paras->begin() + hostParagraph);
    paras->insert(paras->begin() + hostParagraph, lines.begin(), lines.end());
    dropParentLineSegsOf(doc, hostStory);
    return relaidAll(doc, pages, useStoredLineHeight, hostStory, hostParagraph, caretChar);
}

EditResult textToTableNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0 || !cur.hasSelection) {
        why = "표로 바꿀 글을 블록으로 정하십시오.";
        return {};
    }
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.endParagraph >= static_cast<int>(paras->size())) return {};
    const std::string how = textOf(fields, "delimiter", "auto");
    const std::string extra = textOf(fields, "text");
    const bool keep = flagOf(fields, "keep");
    std::vector<std::vector<std::string>> rows;
    std::vector<std::uint32_t> shapes;
    std::size_t cols = 1;
    for (int p = cur.startParagraph; p <= cur.endParagraph; ++p) {
        const model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
        rows.push_back(splitFields(para.text(), how, extra, keep));
        shapes.push_back(charShapeAtParagraphChar(para, 0, true));
        cols = std::max(cols, rows.back().size());
    }
    // 블록이 걸친 문단 전체를 고르고 그 자리에 표를 넣는다.
    const int story = cur.story;
    const int first = cur.startParagraph;
    const int last = cur.endParagraph;
    const int lastLen = paragraphCharCount((*paras)[static_cast<std::size_t>(last)]);
    setPointerCursor(doc, pages, model::CursorPos{story, first, charIndexToOffset(doc, pages, story, first, 0)},
                     model::CursorPos{story, last, charIndexToOffset(doc, pages, story, last, lastLen)});
    EditResult made = insertTableNow(doc, pages, static_cast<int>(rows.size()), static_cast<int>(cols), useStoredLineHeight);
    if (!made.documentChanged) {
        why = "표를 넣지 못했습니다.";
        return made;
    }
    paras = storyParagraphs(doc, story);
    if (!paras) return made;
    model::Control* control = nullptr;
    int hostParagraph = -1;
    for (int p = std::max(0, first - 1); p < static_cast<int>(paras->size()) && !control && p <= first + 1; ++p) {
        for (model::Run& run : (*paras)[static_cast<std::size_t>(p)].runs) {
            for (model::InlineItem& item : run.items) {
                if (!control && item.type == model::InlineItem::Type::Control && item.control.table) {
                    control = &item.control;
                    hostParagraph = p;
                }
            }
        }
    }
    if (!control) return made;
    for (model::TableCell& cell : control->table->cells) {
        if (cell.row >= rows.size() || cell.content.paragraphs.empty()) continue;
        const std::vector<std::string>& line = rows[cell.row];
        model::Paragraph& target = cell.content.paragraphs.front();
        const std::uint32_t shape = shapes[cell.row];
        target.runs.clear();
        model::Run run{shape, {}};
        if (cell.col < line.size() && !line[cell.col].empty()) run.items.push_back(model::InlineItem::makeText(line[cell.col]));
        target.runs.push_back(std::move(run));
        target.lineSegs.clear();
    }
    (*paras)[static_cast<std::size_t>(hostParagraph)].lineSegs.clear();
    // 캐럿은 첫 칸 처음이다.
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    const model::StoryMap& map = ensureStoryMap(doc);
    for (const model::TableCell& cell : control->table->cells) {
        if (cell.row == 0 && cell.col == 0) {
            const int cellStory = map.storyOf(&cell.content.paragraphs);
            if (cellStory > 0) setPointerCursor(doc, pages, model::CursorPos{cellStory, 0, 0});
        }
    }
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    return makeEditResult(doc, true, true, story, hostParagraph, &relayout);
}

}  // namespace chwp::render
