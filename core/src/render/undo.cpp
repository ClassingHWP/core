#include "chwp/render/paint.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/undo_history.hpp"

// 되돌리기·다시 하기. 편집 명령 앞에서 커서 둘레 문단을 베껴 두고, 명령 뒤에 바뀌지 않은 앞뒤 문단을 덜어
// "이 문단들이 그 몇 문단으로 바뀌었다"만 남긴다. 되돌리면 그 자리를 맞바꾸고 바뀐 문단만 다시 놓는다.
// 편집 용지는 구역 전체를 남긴다.

namespace chwp::render {

using namespace paint_detail;

namespace {

// 다른 문서를 고치기 시작하면 앞 문서의 기록은 쓸 수 없다.
UndoHistory& historyFor(const model::Document& doc) {
    UndoHistory& h = undoHistory();
    if (h.doc != &doc) {
        h.undo.clear();
        h.redo.clear();
        h.savedId = 0;
        h.doc = &doc;
    }
    return h;
}

CaretMark caretMark(const model::Document& doc, const std::vector<Node>& pages) {
    const PointerCursor cur = pointerCursor();
    CaretMark mark;
    if (!cur.active || cur.story <= 0) return mark;
    mark.story = cur.story;
    mark.anchorParagraph = cur.anchorParagraph >= 0 ? cur.anchorParagraph : cur.paragraph;
    mark.anchorChar = offsetToCharIndex(doc, pages, cur.story, mark.anchorParagraph,
                                        cur.anchorParagraph >= 0 ? cur.anchorOffset : cur.offset);
    mark.paragraph = cur.paragraph;
    mark.focusChar = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    return mark;
}

void placeCaretMark(const model::Document& doc, const std::vector<Node>& pages, const CaretMark& mark) {
    if (mark.story <= 0) return;
    const std::vector<model::Paragraph>* paras = storyParagraphs(doc, mark.story);
    if (!paras || paras->empty()) return;
    const int last = static_cast<int>(paras->size()) - 1;
    const int anchorPara = std::clamp(mark.anchorParagraph, 0, last);
    const int focusPara = std::clamp(mark.paragraph, 0, last);
    const int anchorOff = charIndexToOffset(doc, pages, mark.story, anchorPara, mark.anchorChar);
    const int focusOff = charIndexToOffset(doc, pages, mark.story, focusPara, mark.focusChar);
    setPointerCursor(doc, pages, model::CursorPos{mark.story, anchorPara, anchorOff},
                     model::CursorPos{mark.story, focusPara, focusOff});
}

// 문단 자체가 같은가. 줄 나눔 기록(lineSegs)은 편집이 지우므로 보지 않는다. 개체 안은 종류만 본다.
// 개체 안 글을 고치면 그 글의 story 에서 따로 기록한다.
bool sameParagraph(const model::Paragraph& a, const model::Paragraph& b);

// 칸 블록 서식은 바깥 문단은 그대로 두고 표 칸 안 글만 바꾼다. 칸 안까지 봐야 그 문단을 되돌리기 칸에 남긴다.
bool sameTable(const model::Control& a, const model::Control& b) {
    if (!a.table || !b.table) return !a.table && !b.table;
    const std::vector<model::TableCell>& ca = a.table->cells;
    const std::vector<model::TableCell>& cb = b.table->cells;
    if (ca.size() != cb.size()) return false;
    for (std::size_t c = 0; c < ca.size(); ++c) {
        const model::TableCell& x = ca[c];
        const model::TableCell& y = cb[c];
        if (x.width != y.width || x.height != y.height || x.borderFillId != y.borderFillId ||
            x.content.paragraphs.size() != y.content.paragraphs.size()) {
            return false;
        }
        for (std::size_t p = 0; p < x.content.paragraphs.size(); ++p) {
            if (!sameParagraph(x.content.paragraphs[p], y.content.paragraphs[p])) return false;
        }
    }
    return true;
}

bool sameParagraph(const model::Paragraph& a, const model::Paragraph& b) {
    if (a.paraShapeId != b.paraShapeId || a.styleId != b.styleId || a.pageBreak != b.pageBreak ||
        a.columnBreak != b.columnBreak || a.layoutBreak != b.layoutBreak || a.runs.size() != b.runs.size() ||
        a.ranges.size() != b.ranges.size()) {
        return false;
    }
    for (std::size_t r = 0; r < a.runs.size(); ++r) {
        const model::Run& ra = a.runs[r];
        const model::Run& rb = b.runs[r];
        if (ra.charShapeId != rb.charShapeId || ra.items.size() != rb.items.size()) return false;
        for (std::size_t i = 0; i < ra.items.size(); ++i) {
            const model::InlineItem& ia = ra.items[i];
            const model::InlineItem& ib = rb.items[i];
            if (ia.type != ib.type || ia.text != ib.text || ia.markpen != ib.markpen ||
                ia.synthetic != ib.synthetic || ia.replacedUnits != ib.replacedUnits) {
                return false;
            }
            if (ia.type == model::InlineItem::Type::Control && ia.control.kind != ib.control.kind) return false;
            if (ia.type == model::InlineItem::Type::Control && !sameTable(ia.control, ib.control)) return false;
        }
    }
    return true;
}

// 편집 명령 하나를 감싼다. 앞에서 커서 둘레 문단을 베끼고, 뒤에서 바뀐 문단만 기록한다.
class UndoTrack {
public:
    // story·paragraph 를 주면 커서 대신 그 문단 둘레를 베낀다(커서와 다른 문단을 고치는 도형 넣기).
    UndoTrack(const model::Document& doc, const std::vector<Node>& pages, UndoKind kind, int story = -1,
              int paragraph = -1)
        : kind_(kind) {
        if (readOnly()) return;
        caretBefore_ = caretMark(doc, pages);
        if (kind == UndoKind::Sections) {
            sections_ = doc.sections;
            fonts_ = doc.header.fonts;
            armed_ = true;
            return;
        }
        PointerCursor cur = pointerCursor();
        if (story > 0 && paragraph >= 0) {
            cur = PointerCursor{};
            cur.active = true;
            cur.story = story;
            cur.paragraph = paragraph;
        }
        if (!cur.active || cur.story <= 0) return;
        const std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
        if (!paras || paras->empty()) return;
        const int last = static_cast<int>(paras->size()) - 1;
        int lo = cur.hasSelection ? std::min(cur.startParagraph, cur.endParagraph) : cur.paragraph;
        int hi = cur.hasSelection ? std::max(cur.startParagraph, cur.endParagraph) : cur.paragraph;
        // 앞 문단과 합치는 지우기, 뒤 문단을 끌어오는 지우기도 담는다.
        lo = std::clamp(lo - 1, 0, last);
        hi = std::clamp(hi + 1, 0, last);
        story_ = cur.story;
        first_ = lo;
        countBefore_ = static_cast<int>(paras->size());
        paragraphs_.assign(paras->begin() + lo, paras->begin() + hi + 1);
        armed_ = true;
    }

    void finish(const model::Document& doc, const std::vector<Node>& pages, const EditResult& result) {
        if (!armed_ || !result.documentChanged) return;
        UndoHistory& h = historyFor(doc);
        UndoRecord record;
        record.kind = kind_;
        record.group = h.openGroup;
        record.caretBefore = caretBefore_;
        record.caretAfter = caretMark(doc, pages);
        if (kind_ == UndoKind::Sections) {
            record.sections = std::move(sections_);
            record.fonts = std::move(fonts_);
            push(h, std::move(record));
            return;
        }
        const std::vector<model::Paragraph>* paras = storyParagraphs(doc, story_);
        if (!paras) return;
        const int countAfter = static_cast<int>(paras->size());
        int first = first_;
        int count = static_cast<int>(paragraphs_.size()) + (countAfter - countBefore_);
        if (count < 0 || first + count > countAfter) return;
        // 바뀌지 않은 앞뒤 문단은 덜어 낸다.
        std::size_t head = 0;
        while (head < paragraphs_.size() && count > 0 &&
               sameParagraph(paragraphs_[head], (*paras)[static_cast<std::size_t>(first)])) {
            ++head;
            ++first;
            --count;
        }
        std::size_t tail = paragraphs_.size();
        while (tail > head && count > 0 &&
               sameParagraph(paragraphs_[tail - 1], (*paras)[static_cast<std::size_t>(first + count - 1)])) {
            --tail;
            --count;
        }
        if (tail == head && count == 0) return;
        const bool sameCount = countAfter == countBefore_;
        // 같은 문단에 이어 친 글자·이어 지운 글자는 앞 칸에 잇는다. 앞 칸이 가진 옛 문단이 그대로 되돌릴 모습이다.
        if (!h.undo.empty() && sameCount && (kind_ == UndoKind::Typing || kind_ == UndoKind::Deleting)) {
            UndoRecord& top = h.undo.back();
            if (!top.closed && top.group == h.openGroup && top.kind == kind_ && top.story == story_ &&
                top.caretAfter == caretBefore_ &&
                first >= top.first && first + count <= top.first + top.count) {
                top.caretAfter = record.caretAfter;
                h.redo.clear();
                return;
            }
        }
        record.story = story_;
        record.first = first;
        record.count = count;
        record.paragraphs.assign(std::make_move_iterator(paragraphs_.begin() + static_cast<std::ptrdiff_t>(head)),
                                 std::make_move_iterator(paragraphs_.begin() + static_cast<std::ptrdiff_t>(tail)));
        push(h, std::move(record));
    }

private:
    static void push(UndoHistory& h, UndoRecord record) {
        if (record.id == 0) record.id = h.nextId++;
        h.redo.clear();
        h.undo.push_back(std::move(record));
        if (h.undo.size() > kUndoLimit) h.undo.erase(h.undo.begin());
    }

    UndoKind kind_;
    bool armed_ = false;
    int story_ = -1;
    int first_ = 0;
    int countBefore_ = 0;
    std::vector<model::Paragraph> paragraphs_;
    std::vector<model::Section> sections_;
    model::PerLang<std::vector<model::Font>> fonts_;
    CaretMark caretBefore_;
};

// 쪽을 짤 때 놓는 개체(종이·쪽 기준)가 있는 문단. 문단 하나만 다시 짜면 빠지므로 쪽을 다시 짠다.
bool holdsFrameObject(const std::vector<model::Paragraph>& paragraphs) {
    for (const model::Paragraph& para : paragraphs) {
        for (const model::Run& run : para.runs) {
            for (const model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Control || !item.control.placement) continue;
                const model::Placement& placement = *item.control.placement;
                if (!placement.treatAsChar && placement.vertRelTo != "PARA") return true;
            }
        }
    }
    return false;
}

// 문단 칸을 맞바꾸고 바뀐 문단만 다시 놓는다. 돌려주는 칸을 다시 적용하면 원래대로 돌아온다.
UndoRecord applyRecord(model::Document& doc, std::vector<Node>& pages, UndoRecord record, bool useStoredLineHeight,
                       EditRelayoutResult& relayout) {
    UndoRecord inverse;
    inverse.kind = record.kind;
    inverse.closed = true;
    inverse.group = record.group;
    inverse.id = record.id;
    inverse.caretBefore = record.caretAfter;
    inverse.caretAfter = record.caretBefore;
    PointerHold& hold = pointerHold();
    if (hold.composing) clearComposition(hold);
    hold.preferredX = -1;

    if (record.kind == UndoKind::Sections) {
        inverse.sections = std::move(doc.sections);
        doc.sections = std::move(record.sections);
        inverse.fonts = std::move(doc.header.fonts);
        doc.header.fonts = std::move(record.fonts);
        rebuildStoryMap(doc);
        invalidateLinePlaces({}, true);
        model::ensureParagraphIds(doc);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        relayout.incremental = false;
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
        placeCaretMark(doc, pages, record.caretBefore);
        return inverse;
    }

    std::vector<model::Paragraph>* paras = storyParagraphs(doc, record.story);
    if (!paras || record.first < 0 || record.first + record.count > static_cast<int>(paras->size())) return inverse;
    const auto begin = paras->begin() + record.first;
    const int oldFirstLen = record.count > 0 ? paragraphCharCount(*begin) : 0;
    std::vector<int> oldLens;
    for (int i = 0; i < record.count; ++i) oldLens.push_back(paragraphCharCount(*(begin + i)));
    inverse.story = record.story;
    inverse.first = record.first;
    inverse.count = static_cast<int>(record.paragraphs.size());
    inverse.paragraphs.assign(std::make_move_iterator(begin), std::make_move_iterator(begin + record.count));
    paras->erase(begin, begin + record.count);
    paras->insert(paras->begin() + record.first, std::make_move_iterator(record.paragraphs.begin()),
                  std::make_move_iterator(record.paragraphs.end()));
    rebuildStoryMap(doc);

    const int newCount = inverse.count;
    const int oldCount = record.count;
    const int delta = newCount - oldCount;
    const bool frameObjects = holdsFrameObject(inverse.paragraphs) || [&] {
        const std::vector<model::Paragraph>* restored = storyParagraphs(doc, record.story);
        if (!restored) return false;
        const std::vector<model::Paragraph> span(restored->begin() + record.first,
                                                 restored->begin() + record.first + newCount);
        return holdsFrameObject(span);
    }();
    std::vector<model::Paragraph>* now = storyParagraphs(doc, record.story);
    auto lengthAt = [&](int index) {
        return now && index >= 0 && index < static_cast<int>(now->size())
                   ? paragraphCharCount((*now)[static_cast<std::size_t>(index)])
                   : 0;
    };
    // 문단 수가 그대로면 문단마다, 하나가 둘로·여럿이 하나로면 한 번에 놓는다(편집 명령과 같은 길). 그 밖은 구역째 놓는다.
    if (frameObjects) {
        invalidateLinePlaces({}, true);
        model::ensureParagraphIds(doc);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        relayout = EditRelayoutResult{};
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
    } else if (delta == 0 && newCount > 0) {
        relayout.incremental = true;
        for (int i = 0; i < newCount; ++i) {
            const EditRelayoutSpec spec{record.story, record.first + i, 0, 0, oldLens[static_cast<std::size_t>(i)],
                                        lengthAt(record.first + i), useStoredLineHeight};
            EditRelayoutResult step = relayoutAfterEdit(doc, pages, spec);
            if (step.fullInvalidation || !step.incremental) {
                relayout = std::move(step);
                if (relayout.fullInvalidation) break;
            } else {
                for (FrameInvalidation& inv : step.invalidations) relayout.invalidations.push_back(std::move(inv));
                relayout.pageListChanged = relayout.pageListChanged || step.pageListChanged;
            }
        }
    } else if ((oldCount == 1 && newCount >= 1) || (newCount == 1 && oldCount >= 1)) {
        const EditRelayoutSpec spec{record.story, record.first, delta, 0, oldFirstLen, lengthAt(record.first),
                                    useStoredLineHeight};
        relayout = relayoutAfterEdit(doc, pages, spec);
    } else {
        invalidateLinePlaces({}, true);
        model::ensureParagraphIds(doc);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        relayout = EditRelayoutResult{};
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
    }
    placeCaretMark(doc, pages, record.caretBefore);
    return inverse;
}

EditResult stepHistory(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight, bool backward) {
    if (readOnly()) return {};
    UndoHistory& h = historyFor(doc);
    std::vector<UndoRecord>& from = backward ? h.undo : h.redo;
    std::vector<UndoRecord>& to = backward ? h.redo : h.undo;
    if (from.empty()) return {};
    UndoRecord record = std::move(from.back());
    from.pop_back();
    const int story = record.story;
    const int paragraph = record.caretBefore.paragraph >= 0 ? record.caretBefore.paragraph : record.first;
    EditRelayoutResult relayout;
    // 칸 블록은 표 개체 주소를 들고 있다. 되돌리기가 그 문단을 바꿔 끼우면 주소가 낡으니 블록을 먼저 놓는다.
    pointerHold().drag = DragHost{};
    const int group = record.group;
    to.push_back(applyRecord(doc, pages, std::move(record), useStoredLineHeight, relayout));
    // 묶음이면 같은 번호의 칸을 모두 되돌린다. 칸마다 다시 놓았으니 화면 쪽은 통째로 다시 본다.
    bool several = false;
    while (group != 0 && !from.empty() && from.back().group == group) {
        UndoRecord next = std::move(from.back());
        from.pop_back();
        EditRelayoutResult more;
        to.push_back(applyRecord(doc, pages, std::move(next), useStoredLineHeight, more));
        several = true;
    }
    if (several) {
        relayout.incremental = false;
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
    }
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.active ? cur.story : story, cur.active ? cur.paragraph : paragraph,
                          &relayout);
}

UndoKind keyKind(EditKey key) {
    return key == EditKey::Backspace || key == EditKey::Delete ? UndoKind::Deleting : UndoKind::Other;
}

}  // namespace

EditResult editInsertText(model::Document& doc, std::vector<Node>& pages, const std::string& text, bool composing,
                          bool useStoredLineHeight) {
    const bool lineBreak = text.find_first_of("\r\n") != std::string::npos;
    UndoTrack track(doc, pages, lineBreak ? UndoKind::Other : UndoKind::Typing);
    EditResult result = insertTextNow(doc, pages, text, composing, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

EditResult editKeyDown(model::Document& doc, std::vector<Node>& pages, EditKey key, bool shift, bool ctrl, bool alt,
                       bool useStoredLineHeight) {
    UndoTrack track(doc, pages, keyKind(key));
    EditResult result = keyDownNow(doc, pages, key, shift, ctrl, alt, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

EditResult editApplyFormat(model::Document& doc, std::vector<Node>& pages, const FormatPatch& patch,
                           bool useStoredLineHeight) {
    int hostStory = -1;
    int hostParagraph = -1;
    if (cellBlockHost(hostStory, hostParagraph)) {
        UndoTrack track(doc, pages, UndoKind::Other, hostStory, hostParagraph);
        EditResult result = applyFormatToCellsNow(doc, pages, patch, useStoredLineHeight);
        track.finish(doc, pages, result);
        return result;
    }
    UndoTrack track(doc, pages, UndoKind::Other);
    EditResult result = applyFormatNow(doc, pages, patch, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

EditResult editApplyPageSetup(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool wholeDocument, bool useStoredLineHeight) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = applyPageSetupNow(doc, pages, fields, wholeDocument, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

namespace {

// 막힌 글꼴 때문에 받지 못한 표·그림 넣기. 글꼴을 바꾸면 그 캐럿 자리에 이어서 넣는다.
struct PendingInsert {
    int kind = 0;  // 0 없음, 1 표, 2 그림
    int rows = 0;
    int cols = 0;
    Bytes image;
};

PendingInsert& pendingInsert() {
    static PendingInsert pending;
    return pending;
}

}  // namespace

EditResult editInsertTable(model::Document& doc, std::vector<Node>& pages, int rows, int cols,
                           bool useStoredLineHeight) {
    UndoTrack track(doc, pages, UndoKind::Other);
    EditResult result = insertTableNow(doc, pages, rows, cols, useStoredLineHeight);
    track.finish(doc, pages, result);
    if (result.needsFontSwap) pendingInsert() = PendingInsert{1, rows, cols, {}};
    return result;
}

EditResult editInsertPicture(model::Document& doc, std::vector<Node>& pages, const Bytes& image,
                             bool useStoredLineHeight) {
    UndoTrack track(doc, pages, UndoKind::Other);
    EditResult result = insertPictureNow(doc, pages, image, useStoredLineHeight);
    track.finish(doc, pages, result);
    if (result.needsFontSwap) pendingInsert() = PendingInsert{2, 0, 0, image};
    return result;
}

namespace paint_detail {

EditResult resumePendingInsert(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PendingInsert pending = std::move(pendingInsert());
    pendingInsert() = PendingInsert{};
    if (pending.kind == 1) return editInsertTable(doc, pages, pending.rows, pending.cols, useStoredLineHeight);
    if (pending.kind == 2) return editInsertPicture(doc, pages, pending.image, useStoredLineHeight);
    return editInsertDrawnShape(doc, pages, useStoredLineHeight);
}

}  // namespace paint_detail

void dropPendingInsert() {
    pendingInsert() = PendingInsert{};
    dropPendingDrawnShape();
}

void setDrawTool(int tool) {
    PointerHold& hold = pointerHold();
    hold.drawTool = tool >= 1 && tool <= 5 ? tool : 0;
    hold.drawing = false;
    bumpPointerPicture();
}

int drawTool() { return pointerHold().drawTool; }

EditResult editInsertShape(model::Document& doc, std::vector<Node>& pages, int tool, int page, std::int32_t x0,
                           std::int32_t y0, std::int32_t x1, std::int32_t y1, bool useStoredLineHeight) {
    int story = -1;
    int paragraph = -1;
    shapeAnchor(doc, pages, page, std::min(y0, y1), story, paragraph);
    UndoTrack track(doc, pages, UndoKind::Other, story, paragraph);
    EditResult result = insertShapeNow(doc, pages, tool, page, x0, y0, x1, y1, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

EditResult editInsertDrawnShape(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerHold& hold = pointerHold();
    if (!hold.drawPending) return {};
    int story = -1;
    int paragraph = -1;
    shapeAnchor(doc, pages, hold.drawPage, std::min(hold.drawY0, hold.drawY1), story, paragraph);
    UndoTrack track(doc, pages, UndoKind::Other, story, paragraph);
    EditResult result = insertDrawnShapeNow(doc, pages, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

EditResult editMoveObject(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerHold& hold = pointerHold();
    if (!hold.movePending) return {};
    // 글을 비켜 가는 개체는 그 글 묶음의 저장된 줄을 다 지우므로 구역째 남긴다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = moveObjectNow(doc, pages, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

EditResult editResizeObject(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerHold& hold = pointerHold();
    if (!hold.resizeObjectPending) return {};
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = resizeObjectNow(doc, pages, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

EditResult editResizeTableBorder(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerHold& hold = pointerHold();
    if (!hold.borderPending) return {};
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = resizeTableBorderNow(doc, pages, useStoredLineHeight);
    track.finish(doc, pages, result);
    return result;
}

// 막힌 글꼴 바꾸기는 글꼴 표와 모든 문단의 저장된 줄 나눔을 바꾸므로 구역 칸으로 남긴다.
EditResult editSwapLicenseFonts(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                                const std::vector<std::pair<std::string, std::string>>& chosen) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = swapLicenseFontsNow(doc, pages, useStoredLineHeight, chosen);
    track.finish(doc, pages, result);
    return result;
}

EditResult editUndo(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    return stepHistory(doc, pages, useStoredLineHeight, true);
}

EditResult editRedo(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    return stepHistory(doc, pages, useStoredLineHeight, false);
}

bool canUndo() { return !undoHistory().undo.empty(); }

bool canRedo() { return !undoHistory().redo.empty(); }

void markUndoSaved() {
    UndoHistory& h = undoHistory();
    // 저장한 칸에 이어 치지 않게 닫는다. 이어 치면 저장한 모습과 달라진다.
    if (!h.undo.empty()) h.undo.back().closed = true;
    h.savedId = h.undo.empty() ? 0 : h.undo.back().id;
}

bool undoAtSaved() {
    const UndoHistory& h = undoHistory();
    return (h.undo.empty() ? 0 : h.undo.back().id) == h.savedId;
}

void clearUndoHistory() {
    UndoHistory& h = undoHistory();
    h.undo.clear();
    h.redo.clear();
    h.doc = nullptr;
    h.openGroup = 0;
    h.savedId = 0;
}

int beginUndoGroup() {
    UndoHistory& h = undoHistory();
    if (h.openGroup == 0) h.openGroup = h.nextGroup++;
    return h.openGroup;
}

int topUndoGroup() {
    const UndoHistory& h = undoHistory();
    return h.undo.empty() ? 0 : h.undo.back().group;
}

void endUndoGroup() { undoHistory().openGroup = 0; }

bool undoGroupOpen() { return undoHistory().openGroup != 0; }

void clearRedoHistory() { undoHistory().redo.clear(); }

}  // namespace chwp::render
