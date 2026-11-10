// 편집 마무리 알림. 모든 편집 명령은 되돌리기 기록(UndoTrack)을 거치고, 그 끝에서 바뀐 범위(EditChange)를 여기로
// 알린다(되돌리기·다시 하기는 문서 전체). 듣는 쪽은 그 범위를 보고 파생 값을 다시 맞춘다. 모델을 고치는 자리가 수십
// 곳이라 고칠 때마다 알리면 하나만 빠져도 어긋나므로, 알림은 편집 하나가 끝나는 이 자리 하나에서만 낸다.
// 지금 듣는 쪽은 표 계산식이다(규칙은 model/table_formula.hpp).
#include <algorithm>
#include <vector>

#include "chwp/model/table_formula.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

// 듣는 쪽이 고친 표: 그 표 칸 하나의 글 묶음과 표를 품은 문단.
struct Touched {
    int cellStory = -1;
    int hostStory = -1;
    int hostParagraph = -1;
};

using EditListener = void (*)(model::Document& doc, const EditChange& change, std::vector<Touched>& touched);

// 표(그 칸 글 묶음 cellStory)가 바뀐 문단 [first, first + count) 안에 드는가. 글상자·다른 표 칸에 겹쳐 든 것도 본다.
bool insideChangedParagraphs(model::Document& doc, int cellStory, const std::vector<model::Paragraph>* changed,
                             const EditChange& change) {
    int s = cellStory;
    for (int depth = 0; s > 0 && depth < 64; ++depth) {
        const model::StorySlot* slot = resolveStory(doc, s);
        if (!slot) return false;
        const model::StorySlot* parent = slot->parentStory > 0 ? resolveStory(doc, slot->parentStory) : nullptr;
        if (parent && parent->paragraphs == changed) {
            return slot->parentParagraph >= change.first && slot->parentParagraph < change.first + change.count;
        }
        s = slot->parentStory;
    }
    return false;
}

// 표 계산식: 바뀐 범위에 든 표, 바로 고친 칸이 든 표를 다시 계산한다. 바로 고친 칸에서 결과 글이 명령의 마지막 결과와
// 달라진 계산식은 손으로 고친 것이라 먼저 풀어 보통 글로 둔다.
void recalcFormulas(model::Document& doc, const EditChange& change, std::vector<Touched>& touched) {
    const model::StorySlot* edited = change.whole ? nullptr : resolveStory(doc, change.story);
    const std::vector<model::Paragraph>* changed = edited ? edited->paragraphs : nullptr;
    if (!change.whole && !changed) return;
    model::StoryMap& map = ensureStoryMap(doc);
    struct Entry {
        model::Control* control = nullptr;
        int cellStory = -1;
        std::vector<model::Paragraph>* direct = nullptr;
    };
    std::vector<Entry> tables;
    for (int s = 1; s <= map.count(); ++s) {
        model::StorySlot* slot = map.slot(s);
        if (!slot || !slot->tableCell || !slot->control || !slot->control->table) continue;
        // 바탕쪽은 쪽마다 별칭 글 묶음이 있다. 처음 번호 하나만 본다.
        if (map.storyOf(slot->paragraphs) != s) continue;
        // 한 표의 칸 글 묶음은 번호가 이어지니 마지막 표부터 본다.
        auto it = !tables.empty() && tables.back().control == slot->control
                      ? tables.end() - 1
                      : std::find_if(tables.begin(), tables.end(), [&](const Entry& e) { return e.control == slot->control; });
        if (it == tables.end()) {
            tables.push_back(Entry{slot->control, s, nullptr});
            it = tables.end() - 1;
        }
        if (changed && slot->paragraphs == changed) it->direct = slot->paragraphs;
    }
    for (const Entry& e : tables) {
        if (!change.whole && !e.direct && !insideChangedParagraphs(doc, e.cellStory, changed, change)) continue;
        model::Table& table = *e.control->table;
        if (!model::tableHasFormulas(table)) continue;
        bool dirty = false;
        if (e.direct) {
            std::vector<model::Paragraph>& paras = *e.direct;
            const int from = std::max(0, change.first);
            const int to = std::min(static_cast<int>(paras.size()), change.first + change.count);
            for (int i = from; i < to; ++i) dirty = model::unwrapEditedFormulas(paras[static_cast<std::size_t>(i)]) > 0 || dirty;
        }
        dirty = model::recalcTableFormulas(table) || dirty;
        if (!dirty) continue;
        const model::StorySlot* cell = map.slot(e.cellStory);
        if (cell) touched.push_back(Touched{e.cellStory, cell->parentStory, cell->parentParagraph});
    }
}

constexpr EditListener kListeners[] = {recalcFormulas};

// 캐럿을 글 묶음·문단 번호와 글자 번호로 잡는다. 듣는 쪽은 문단을 넣거나 빼지 않고 필드(글자가 아닌 조종 문자)만
// 빼므로 글자 번호가 그대로다.
struct CaretChars {
    PointerCursor cur;
    int anchorChar = 0;
    int focusChar = 0;
};

}  // namespace

EditResult paint_detail::afterEditCommitted(model::Document& doc, std::vector<Node>& pages, const EditResult& result,
                                            const EditChange& change, bool useStoredLineHeight, bool* changedMore) {
    if (changedMore) *changedMore = false;
    if (!result.documentChanged || result.composing || readOnly()) return result;
    CaretChars caret;
    caret.cur = pointerCursor();
    const bool hadCaret = caret.cur.active && caret.cur.story > 0;
    if (hadCaret) {
        caret.anchorChar = offsetToCharIndex(doc, pages, caret.cur.story, caret.cur.anchorParagraph, caret.cur.anchorOffset);
        caret.focusChar = offsetToCharIndex(doc, pages, caret.cur.story, caret.cur.paragraph, caret.cur.offset);
    }
    std::vector<Touched> touched;
    for (const EditListener listener : kListeners) listener(doc, change, touched);
    // 같은 문단에 든 표는 한 번만 다시 놓는다.
    std::vector<Touched> hosts;
    for (const Touched& t : touched) {
        const bool seen = std::any_of(hosts.begin(), hosts.end(), [&](const Touched& h) {
            return h.hostStory == t.hostStory && h.hostParagraph == t.hostParagraph;
        });
        if (!seen) hosts.push_back(t);
    }
    if (hosts.empty()) return result;
    if (changedMore) *changedMore = true;
    for (const Touched& t : hosts) {
        if (model::Paragraph* host = storyParagraph(doc, t.hostStory, t.hostParagraph)) host->lineSegs.clear();
        dropParentLineSegsOf(doc, t.cellStory);
    }
    model::ensureParagraphIds(doc);
    EditRelayoutResult relayout;
    const model::Paragraph* host = storyParagraph(doc, hosts.front().hostStory, hosts.front().hostParagraph);
    if (hosts.size() == 1 && host) {
        // 문서 전체가 아니라 그 표를 품은 문단만 다시 놓는다(증분 조판이 뒤 쪽을 잇는다).
        EditRelayoutSpec spec;
        spec.story = hosts.front().hostStory;
        spec.paraIndex = hosts.front().hostParagraph;
        spec.paraDelta = 0;
        spec.editStartOff = 0;
        spec.editOldEndOff = spec.editNewEndOff =
            charIndexToOffset(doc, pages, spec.story, spec.paraIndex, paragraphCharCount(*host));
        spec.useStoredLineHeight = useStoredLineHeight;
        spec.paraShapeChanged = true;
        relayout = relayoutAfterEdit(doc, pages, spec);
    } else {
        relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    }
    if (hadCaret) {
        const PointerCursor& c = caret.cur;
        setPointerCursor(doc, pages,
                         model::CursorPos{c.story, c.anchorParagraph,
                                          charIndexToOffset(doc, pages, c.story, c.anchorParagraph, caret.anchorChar)},
                         model::CursorPos{c.story, c.paragraph,
                                          charIndexToOffset(doc, pages, c.story, c.paragraph, caret.focusChar)});
    }
    const PointerCursor now = pointerCursor();
    EditResult out = makeEditResult(doc, true, true, now.active ? now.story : result.dirtyStory,
                                    now.active ? now.paragraph : result.dirtyParagraph, &relayout);
    // 편집이 먼저 다시 놓은 범위도 같이 다시 그린다. 쪽이 옮겨졌으면 모두 다시 그린다.
    out.invalidations.insert(out.invalidations.end(), result.invalidations.begin(), result.invalidations.end());
    out.fullInvalidation = out.fullInvalidation || result.fullInvalidation;
    out.pageListChanged = out.pageListChanged || result.pageListChanged;
    if (result.pagesMoved || result.pageShiftDelta != 0) {
        out.fullInvalidation = true;
        out.pageListChanged = true;
    }
    out.needsFontSwap = result.needsFontSwap;
    return out;
}

}  // namespace chwp::render
