// 본문 정렬(한글 2024 Sort, 글 블록). 숨긴 한글 COM 으로 확인했다.
//   - 블록이 걸친 문단을 통째로 옮긴다. 블록 끝이 문단 맨 앞이면 그 문단은 빼고, 남은 문단이 둘 미만이면 하지 않는다.
//   - 키는 블록 안 글이다(첫 문단은 블록 시작부터, 끝 문단은 블록 끝까지). 비교·차례는 model::paragraphSortOrder.
//   - 첫 문단의 구역·단 정의는 정렬 뒤에도 첫 문단에 남는다(한글 HWPX 의 secPr 이 그대로 첫 문단).
//   - 정렬하면 블록을 풀고 캐럿을 마지막 문단 끝에 둔다. 되돌리기 한 칸이다.

#include <algorithm>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
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
double numberOf(const FormatFields& fields, const std::string& key, double fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const double* d = std::get_if<double>(v)) return *d;
        if (const bool* b = std::get_if<bool>(v)) return *b ? 1.0 : 0.0;
    }
    return fallback;
}
std::string textOf(const FormatFields& fields, const std::string& key) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const std::string* t = std::get_if<std::string>(v)) return *t;
    }
    return {};
}

// 대화 상자 값(keyNPosition·keyNFormat·keyNDesc, N 은 1~3) → 기준. 없음은 위치 0 이고, 짝짓기는 model 이 한다.
std::vector<model::TableSortKey> keysOf(const FormatFields& fields) {
    std::vector<model::TableSortKey> keys;
    for (int n = 1; n <= 3; ++n) {
        const std::string prefix = "key" + std::to_string(n);
        keys.push_back(model::TableSortKey{static_cast<int>(numberOf(fields, prefix + "Position", 0)),
                                           static_cast<int>(numberOf(fields, prefix + "Format", 0)),
                                           numberOf(fields, prefix + "Desc", 0) != 0});
    }
    return keys;
}

model::ParagraphSortOptions optionsOf(const FormatFields& fields) {
    model::ParagraphSortOptions o;
    o.fromRear = numberOf(fields, "fromRear", 0) != 0;
    o.extendYear = numberOf(fields, "extendYear", 1) != 0;
    o.yearBase = static_cast<int>(numberOf(fields, "yearBase", 1930));
    o.oldHangul = numberOf(fields, "oldHangul", 0) != 0;
    o.finalFirst = numberOf(fields, "finalFirst", 0) != 0;
    o.delimiter = static_cast<int>(numberOf(fields, "delimiter", 0));
    o.delimiterChars = utf8ToUtf16(textOf(fields, "delimiterChars"));
    o.ignoreMultiDelimiter = numberOf(fields, "ignoreMulti", 1) != 0;
    o.eachParagraph = numberOf(fields, "eachPara", 1) != 0;
    return o;
}

// 블록이 걸친 문단과 블록 안 글.
struct SortBlock {
    int story = -1;
    int first = -1;
    int last = -1;
    std::vector<model::ParagraphSortText> texts;
};

bool sortBlockOf(const model::Document& doc, const std::vector<Node>& pages, SortBlock& block, std::string& why) {
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0 || !cur.hasSelection || pointerHold().drag.mode == DragMode::Cells) {
        why = "정렬할 문단을 블록으로 정하세요.";
        return false;
    }
    const std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.startParagraph < 0 || cur.endParagraph >= static_cast<int>(paras->size()) ||
        cur.startParagraph > cur.endParagraph) {
        return false;
    }
    block.story = cur.story;
    block.first = cur.startParagraph;
    block.last = cur.endParagraph;
    const int startChar = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
    int endChar = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
    // 블록 끝이 문단 맨 앞이면 그 문단은 블록에 들지 않는다.
    if (block.last > block.first && endChar <= 0) {
        --block.last;
        endChar = paragraphCharCount((*paras)[static_cast<std::size_t>(block.last)]);
    }
    for (int p = block.first; p <= block.last; ++p) {
        std::vector<std::size_t> starts;
        model::ParagraphSortText t;
        t.text = model::paragraphSortText((*paras)[static_cast<std::size_t>(p)], &starts, isCaretCharControl);
        const std::size_t rawLen = t.text.size();
        const auto rawAt = [&](int ch) { return starts[static_cast<std::size_t>(std::clamp(ch, 0, static_cast<int>(starts.size()) - 1))]; };
        const std::size_t from = p == block.first ? rawAt(startChar) : 0;
        const std::size_t to = p == block.last ? rawAt(endChar) : rawLen;
        t.from = from;
        t.length = to > from ? to - from : 0;
        t.text += u'\r';
        block.texts.push_back(std::move(t));
    }
    return true;
}

// 구역·단 정의(첫 문단 앞머리 컨트롤)는 자리를 지킨다.
bool sectionLead(const model::InlineItem& item) {
    return item.type == model::InlineItem::Type::Control &&
           (item.control.kind == model::ControlKind::SectionDef || item.control.kind == model::ControlKind::ColumnDef);
}

EditResult relaidToEnd(model::Document& doc, std::vector<Node>& pages, int story, int last, bool useStoredLineHeight) {
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    dropParentLineSegsOf(doc, story);
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    const int lastLen = paragraphCharCount((*paras)[static_cast<std::size_t>(last)]);
    setPointerCursor(doc, pages, model::CursorPos{story, last, charIndexToOffset(doc, pages, story, last, lastLen)});
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    return makeEditResult(doc, true, true, story, last, &full);
}

// 단어 정렬. 차례가 그대로면 한글처럼 고치지 않는다.
EditResult sortWordsNow(model::Document& doc, std::vector<Node>& pages, const SortBlock& block,
                        const std::vector<model::TableSortKey>& keys, const model::ParagraphSortOptions& options,
                        bool useStoredLineHeight, std::string& why) {
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, block.story);
    if (!paras) return {};
    std::vector<model::Paragraph*> targets;
    for (int p = block.first; p <= block.last; ++p) targets.push_back(&(*paras)[static_cast<std::size_t>(p)]);
    if (!model::sortParagraphWords(targets, block.texts, keys, options, &why)) {
        if (why.empty()) why = "정렬해도 단어 차례가 바뀌지 않습니다.";
        return {};
    }
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    clearPendingCharShape(hold);
    hold.preferredX = -1;
    return relaidToEnd(doc, pages, block.story, block.last, useStoredLineHeight);
}

}  // namespace

EditResult sortParagraphsNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                             bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    SortBlock block;
    if (!sortBlockOf(doc, pages, block, why)) return {};
    const std::vector<model::TableSortKey> keys = keysOf(fields);
    if (keys.front().column == model::kSortWordPosition) return sortWordsNow(doc, pages, block, keys, optionsOf(fields), useStoredLineHeight, why);
    const std::vector<int> order = model::paragraphSortOrder(block.texts, keys, optionsOf(fields), &why);
    if (order.empty()) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, block.story);
    if (!paras) return {};
    bool moved = false;
    for (std::size_t i = 0; i < order.size(); ++i) moved = moved || order[i] != static_cast<int>(i);

    PointerHold& hold = pointerHold();
    clearComposition(hold);
    clearPendingCharShape(hold);
    hold.preferredX = -1;
    const int story = block.story;
    const int last = block.last;
    if (!moved) {
        setPointerCursor(doc, pages, model::CursorPos{story, last, charIndexToOffset(doc, pages, story, last,
                                                                                     paragraphCharCount((*paras)[static_cast<std::size_t>(last)]))});
        return makeEditResult(doc, true, false, story, last);
    }
    const auto begin = paras->begin() + block.first;
    std::vector<model::Paragraph> old(begin, paras->begin() + last + 1);
    // 첫 문단 앞머리의 구역·단 정의를 떼어 새 첫 문단 앞에 둔다.
    model::Run lead;
    bool hasLead = false;
    {
        model::Paragraph& head = old.front();
        for (model::Run& run : head.runs) {
            std::size_t n = 0;
            while (n < run.items.size() && sectionLead(run.items[n])) ++n;
            if (n > 0) {
                if (!hasLead) lead.charShapeId = run.charShapeId;
                hasLead = true;
                lead.items.insert(lead.items.end(), run.items.begin(), run.items.begin() + static_cast<std::ptrdiff_t>(n));
                run.items.erase(run.items.begin(), run.items.begin() + static_cast<std::ptrdiff_t>(n));
            }
            if (!run.items.empty()) break;
        }
        if (hasLead) {
            head.runs.erase(std::remove_if(head.runs.begin(), head.runs.end(),
                                           [](const model::Run& r) { return r.items.empty(); }),
                            head.runs.end());
            if (head.runs.empty()) head.runs.push_back(model::Run{lead.charShapeId, {}});
        }
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        model::Paragraph& p = (*paras)[static_cast<std::size_t>(block.first) + i];
        p = std::move(old[static_cast<std::size_t>(order[i])]);
        p.lineSegs.clear();
    }
    if (hasLead) {
        model::Paragraph& head = (*paras)[static_cast<std::size_t>(block.first)];
        head.runs.insert(head.runs.begin(), std::move(lead));
    }
    return relaidToEnd(doc, pages, story, last, useStoredLineHeight);
}

FormatFields sortView(const model::Document& doc, const std::vector<Node>& pages, const FormatFields& fields) {
    FormatFields out;
    if (pointerHold().drag.mode == DragMode::Cells) {
        out.emplace_back("mode", std::string("table"));
        return out;
    }
    SortBlock block;
    std::string why;
    if (!sortBlockOf(doc, pages, block, why)) {
        out.emplace_back("mode", std::string());
        return out;
    }
    out.emplace_back("mode", std::string("text"));
    out.emplace_back("paragraphs", static_cast<double>(block.texts.size()));
    out.emplace_back("fields", static_cast<double>(model::paragraphSortFieldCount(block.texts, optionsOf(fields))));
    return out;
}

}  // namespace chwp::render
