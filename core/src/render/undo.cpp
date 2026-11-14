#include "chwp/render/paint.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "chwp/render/scene.hpp"
#include "chwp/render/style_commands.hpp"
#include "chwp/render/toc_commands.hpp"
#include "chwp/render/link_commands.hpp"
#include "chwp/render/track_commands.hpp"
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

// 되돌린 캐럿이 머리말·꼬리말 글이면 그 편집으로, 아니면 본문으로 둔다. 머리말 만들기·지우기를 되돌리면
// 머리말 편집에 들어가고 나오는 것도 같이 돌아간다. 바뀌었으면 참이다.
bool syncBandMode(const model::Document& doc) {
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    const model::StorySlot* slot = cur.active && cur.story > 0 ? resolveStory(doc, cur.story) : nullptr;
    const bool band = slot && slot->kind == model::StoryKind::SubList && slot->control &&
                      (slot->control->kind == model::ControlKind::Header ||
                       slot->control->kind == model::ControlKind::Footer);
    const bool master = slot && slot->kind == model::StoryKind::MasterPage;
    if (band || master) {
        if (hold.bandStory == cur.story && hold.bandMaster == master) return false;
        hold.bandStory = cur.story;
        hold.editStory = cur.story;
        hold.bandHeader = band && slot->control->kind == model::ControlKind::Header;
        hold.bandMaster = master;
        setMasterView(master);
        return true;
    }
    if (hold.bandStory <= 0) {
        // 커서를 비운 뒤(agent apply 끝 등)에도 바탕쪽 보기가 남지 않게 한다.
        if (!masterView()) return false;
        setMasterView(false);
        return true;
    }
    if (hold.editStory == hold.bandStory) hold.editStory = 0;
    hold.bandStory = 0;
    hold.bandMaster = false;
    setMasterView(false);
    return true;
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
    if (a.paraShapeId != b.paraShapeId || a.styleId != b.styleId || a.fileId != b.fileId || a.pageBreak != b.pageBreak ||
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
            if (ia.type != ib.type || ia.text != ib.text || ia.markpen != ib.markpen || ia.charStyleId != ib.charStyleId ||
                ia.synthetic != ib.synthetic || ia.replacedUnits != ib.replacedUnits) {
                return false;
            }
            // 필드 명령(계산식 등)만 고쳐도 다른 문단이다. 결과 글이 같은 계산식 고치기도 되돌리기 칸에 남긴다.
            if (ia.type == model::InlineItem::Type::Control &&
                (ia.control.kind != ib.control.kind || ia.control.command != ib.control.command)) {
                return false;
            }
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
            styles_ = doc.header.styles;
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
        // 숨긴 누름틀 안내문은 보기 상태다. 되돌리면 안내문이 보이는 문단으로 돌아온다.
        const HiddenClickGuide& hidden = pointerHold().hiddenGuide;
        if (hidden.story == story_ && hidden.para >= lo && hidden.para <= hi) {
            model::Paragraph& kept = paragraphs_[static_cast<std::size_t>(hidden.para - lo)];
            kept = shownClickHereParagraph(kept, hidden);
        }
        armed_ = true;
    }

    // 기록하고 바뀐 범위를 알린다(afterEditCommitted). result 는 알림을 들은 쪽이 더 고친 것까지 합친 결과가 된다.
    void finish(model::Document& doc, std::vector<Node>& pages, EditResult& result, bool useStoredLineHeight) {
        const EditChange change = record(doc, pages, result);
        result = afterEditCommitted(doc, pages, result, change, useStoredLineHeight);
    }

private:
    // 편집 앞뒤를 견줘 되돌리기 칸을 남기고 바뀐 범위를 돌려준다.
    EditChange record(const model::Document& doc, const std::vector<Node>& pages, const EditResult& result) {
        if (!result.documentChanged) return {};
        EditChange whole;
        whole.whole = true;
        if (!armed_) return whole;
        UndoHistory& h = historyFor(doc);
        UndoRecord record;
        record.kind = kind_;
        record.group = h.openGroup;
        record.caretBefore = caretBefore_;
        record.caretAfter = caretMark(doc, pages);
        if (kind_ == UndoKind::Sections) {
            record.sections = std::move(sections_);
            record.fonts = std::move(fonts_);
            record.styles = std::move(styles_);
            push(h, std::move(record));
            return whole;
        }
        const std::vector<model::Paragraph>* paras = storyParagraphs(doc, story_);
        if (!paras) return whole;
        const int countAfter = static_cast<int>(paras->size());
        int first = first_;
        int count = static_cast<int>(paragraphs_.size()) + (countAfter - countBefore_);
        if (count < 0 || first + count > countAfter) return whole;
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
        EditChange change;
        change.story = story_;
        change.first = first;
        change.count = count;
        if (tail == head && count == 0) return change;
        const bool sameCount = countAfter == countBefore_;
        // 같은 문단에 이어 친 글자·이어 지운 글자는 앞 칸에 잇는다. 앞 칸이 가진 옛 문단이 그대로 되돌릴 모습이다.
        if (!h.undo.empty() && sameCount && (kind_ == UndoKind::Typing || kind_ == UndoKind::Deleting)) {
            UndoRecord& top = h.undo.back();
            if (!top.closed && top.group == h.openGroup && top.kind == kind_ && top.story == story_ &&
                top.caretAfter == caretBefore_ &&
                first >= top.first && first + count <= top.first + top.count) {
                top.caretAfter = record.caretAfter;
                h.redo.clear();
                return change;
            }
        }
        record.story = story_;
        record.first = first;
        record.count = count;
        record.paragraphs.assign(std::make_move_iterator(paragraphs_.begin() + static_cast<std::ptrdiff_t>(head)),
                                 std::make_move_iterator(paragraphs_.begin() + static_cast<std::ptrdiff_t>(tail)));
        push(h, std::move(record));
        return change;
    }

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
    std::vector<model::Style> styles_;
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
        // 스타일 표(추가·편집·지우기·차례). 예전 기록에 없으면 그대로 둔다.
        if (!record.styles.empty()) {
            inverse.styles = std::move(doc.header.styles);
            doc.header.styles = std::move(record.styles);
        }
        rebuildStoryMap(doc);
        invalidateLinePlaces({}, true);
        model::ensureParagraphIds(doc);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        relayout.incremental = false;
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
        placeCaretMark(doc, pages, record.caretBefore);
        if (syncBandMode(doc)) placeCaretMark(doc, pages, record.caretBefore);
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

bool syncEditBand(const model::Document& doc) { return syncBandMode(doc); }

EditResult editInsertText(model::Document& doc, std::vector<Node>& pages, const std::string& text, bool composing,
                          bool useStoredLineHeight) {
    const bool lineBreak = text.find_first_of("\r\n") != std::string::npos;
    // 변경 내용 추적: 기록 중이거나 문서에 바뀜 기록이 있으면 친 글의 표시를 정한다(기록 중이면 넣은 글).
    const bool tracking = trackRecording(doc) || !doc.header.trackChanges.empty();
    if (tracking && trackRecording(doc) && pointerCursor().hasSelection && !pointerHold().composing) {
        // 고른 글 위에 치면 고른 글은 지운 글로 남기고 그 뒤에 친다.
        UndoTrack track(doc, pages, UndoKind::Other);
        EditResult cut;
        if (trackDeleteNow(doc, pages, EditKey::Delete, useStoredLineHeight, cut)) track.finish(doc, pages, cut, useStoredLineHeight);
    }
    int story = 0;
    int para = 0;
    int from = 0;
    if (tracking) {
        const PointerHold& hold = pointerHold();
        const PointerCursor cur = pointerCursor();
        story = hold.composing ? hold.compStory : cur.story;
        para = hold.composing ? hold.compPara : cur.paragraph;
        from = hold.composing ? hold.compStart : offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    }
    UndoTrack track(doc, pages, lineBreak ? UndoKind::Other : UndoKind::Typing);
    // 손대지 않은 누름틀 안에 치면 안내문을 먼저 지운다(같은 되돌리기 한 칸). 쪽을 처음부터 다시 놓았으니 화면도 다시다.
    const bool guideCleared = clearClickHereGuideAtCaret(doc, pages, useStoredLineHeight);
    EditResult result = insertTextNow(doc, pages, text, composing, useStoredLineHeight);
    if (guideCleared) {
        result.fullInvalidation = true;
        result.pageListChanged = true;
    }
    if (tracking && result.documentChanged && story > 0) {
        trackMarkInserted(doc, pages, story, para, from, lineBreak);
        result = makeEditResult(doc, true, true, story, para);
        invalidateLinePlaces({}, true);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        result.fullInvalidation = true;
        result.pageListChanged = true;
    }
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editKeyDown(model::Document& doc, std::vector<Node>& pages, EditKey key, bool shift, bool ctrl, bool alt,
                       bool useStoredLineHeight) {
    // 개체를 골랐으면 Delete·Backspace 는 고른 개체(여러 개체 고르기 포함)를 지운다(한글과 같다). 되돌리기 한 칸이다.
    if ((key == EditKey::Delete || key == EditKey::Backspace) && !shift && !ctrl && !alt &&
        pointerHold().selectedObject && pointerHold().objectHandles) {
        UndoTrack track(doc, pages, UndoKind::Sections);
        EditResult result = deleteSelectedObjectsNow(doc, pages, useStoredLineHeight);
        track.finish(doc, pages, result, useStoredLineHeight);
        if (result.documentChanged) return result;
    }
    // 변경 내용 기록 중이면 지우기는 지운 글 표시다(넣은 글은 정말 지운다).
    if ((key == EditKey::Delete || key == EditKey::Backspace) && !shift && !ctrl && !alt && trackRecording(doc)) {
        UndoTrack track(doc, pages, UndoKind::Other);
        EditResult tracked;
        if (trackDeleteNow(doc, pages, key, useStoredLineHeight, tracked)) {
            track.finish(doc, pages, tracked, useStoredLineHeight);
            return tracked;
        }
    }
    // 기록 중 Enter 는 앞 문단 끝을 넣은 문단 끝으로 표시한다.
    if (key == EditKey::Enter && !shift && !ctrl && !alt && trackRecording(doc)) {
        const PointerCursor before = pointerCursor();
        UndoTrack track(doc, pages, UndoKind::Other);
        EditResult result = keyDownNow(doc, pages, key, shift, ctrl, alt, useStoredLineHeight);
        const PointerCursor after = pointerCursor();
        if (result.documentChanged && after.story == before.story && after.paragraph == before.paragraph + 1) {
            const std::vector<model::Paragraph>* paras = storyParagraphs(doc, after.story);
            if (paras) {
                trackMarkInserted(doc, pages, after.story, before.paragraph,
                                  paragraphCharCount((*paras)[static_cast<std::size_t>(before.paragraph)]), true);
            }
        }
        track.finish(doc, pages, result, useStoredLineHeight);
        return result;
    }
    // 누름틀: ←·→ 는 손대지 않은 누름틀을 한 칸으로 건너고, 그 안에서 지우면 누름틀을 통째로 지운다(한글).
    if (!shift && !ctrl && !alt && moveOverClickHere(doc, pages, key)) {
        const PointerCursor now = pointerCursor();
        return makeEditResult(doc, true, false, now.story, now.paragraph);
    }
    if (!shift && !ctrl && !alt && (key == EditKey::Delete || key == EditKey::Backspace)) {
        UndoTrack track(doc, pages, UndoKind::Other);
        if (deleteClickHereAtCaret(doc, pages, key, useStoredLineHeight)) {
            const PointerCursor now = pointerCursor();
            EditResult result = makeEditResult(doc, true, true, now.story, now.paragraph);
            result.fullInvalidation = true;
            result.pageListChanged = true;
            track.finish(doc, pages, result, useStoredLineHeight);
            return result;
        }
    }
    // 지우기가 메모 표시에 닿으면 메모를 지운다. 메모 지우기 명령과 같이 구역째 베껴 둔 한 칸이다.
    const bool memoErase = !shift && !ctrl && !alt && memoMarkerToErase(doc, pages, key) != nullptr;
    UndoTrack track(doc, pages, memoErase ? UndoKind::Sections : keyKind(key));
    EditResult result = keyDownNow(doc, pages, key, shift, ctrl, alt, useStoredLineHeight);
    // 친 글을 다 지운 누름틀은 다시 안내문을 보인다.
    if (result.documentChanged && (key == EditKey::Delete || key == EditKey::Backspace) &&
        restoreClickHereGuides(doc, pages, useStoredLineHeight)) {
        const PointerCursor now = pointerCursor();
        result = makeEditResult(doc, true, true, now.story, now.paragraph);
        result.fullInvalidation = true;
        result.pageListChanged = true;
    }
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editApplyFormat(model::Document& doc, std::vector<Node>& pages, const FormatPatch& patch,
                           bool useStoredLineHeight) {
    int hostStory = -1;
    int hostParagraph = -1;
    if (cellBlockHost(hostStory, hostParagraph)) {
        UndoTrack track(doc, pages, UndoKind::Other, hostStory, hostParagraph);
        EditResult result = applyFormatToCellsNow(doc, pages, patch, useStoredLineHeight);
        track.finish(doc, pages, result, useStoredLineHeight);
        return result;
    }
    UndoTrack track(doc, pages, UndoKind::Other);
    EditResult result = applyFormatNow(doc, pages, patch, useStoredLineHeight);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editApplyPageSetup(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool wholeDocument, bool useStoredLineHeight) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = applyPageSetupNow(doc, pages, fields, wholeDocument, useStoredLineHeight);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

namespace {

// 막힌 글꼴 때문에 받지 못한 표·그림 넣기. 글꼴을 바꾸면 그 캐럿 자리에 이어서 넣는다.
struct PendingInsert {
    int kind = 0;  // 0 없음, 1 표, 2 그림
    int rows = 0;  // 그림이면 정한 너비(HWPUNIT, 0 이면 그림 크기)
    int cols = 0;  // 그림이면 정한 높이
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
    track.finish(doc, pages, result, useStoredLineHeight);
    if (result.needsFontSwap) pendingInsert() = PendingInsert{1, rows, cols, {}};
    return result;
}

EditResult editInsertNote(model::Document& doc, std::vector<Node>& pages, bool endnote, bool useStoredLineHeight,
                          std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = insertNoteNow(doc, pages, endnote, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editInsertClickHere(model::Document& doc, std::vector<Node>& pages, const std::string& direction,
                               const std::string& memo, const std::string& name, bool useStoredLineHeight,
                               std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Other);
    std::string why;
    EditResult result = insertClickHereNow(doc, pages, direction, memo, name, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editModifyClickHere(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                               bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Other);
    std::string why;
    EditResult result = modifyClickHereNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

FormatFields clickHereFields(model::Document& doc, const std::vector<Node>& pages) { return clickHereView(doc, pages); }

EditResult editInsertHyperlink(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                               const std::string& url, bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Other);
    std::string why;
    EditResult result = insertHyperlinkNow(doc, pages, text, url, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editLeaveNote(model::Document& doc, std::vector<Node>& pages) { return leaveNoteNow(doc, pages); }

EditResult editNoteShape(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, bool endnote,
                         bool wholeDocument, bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = noteShapeNow(doc, pages, fields, endnote, wholeDocument, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editCommitStyle(model::Document& doc, std::vector<Node>& pages, const StyleDraft& draft,
                           bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = commitStyleNow(doc, pages, draft, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editDeleteStyle(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId,
                           std::uint32_t replacement, bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = deleteStyleNow(doc, pages, styleId, replacement, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editMoveStyle(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId, int delta,
                         bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = moveStyleNow(doc, pages, styleId, delta, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editStyleFromCaret(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId,
                              bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = styleFromCaretNow(doc, pages, styleId, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editImportStyles(model::Document& doc, std::vector<Node>& pages, const model::Document& source,
                            const std::vector<std::string>& names, bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = importStylesNow(doc, pages, source, names, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editResolveChangesUndo(model::Document& doc, std::vector<Node>& pages, bool accept, const std::string& scope,
                                  bool useStoredLineHeight, std::string* why) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = editResolveChanges(doc, pages, accept, scope, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editFollowLinkAt(model::Document& doc, std::vector<Node>& pages, int page, int x, int y,
                            bool useStoredLineHeight, std::string* url, std::string* why) {
    // 방문한 링크 색 바꾸기가 되돌리기 한 칸이다(한글과 같다). 이동만이면 칸이 생기지 않는다.
    // 되돌릴 문단은 캐럿이 아니라 누른 링크 문단이다.
    const LinkSpot spot = linkSpotAt(doc, pages, page, x, y);
    if (!spot.found) return {};
    UndoTrack track(doc, pages, UndoKind::Other, spot.story, spot.paragraph);
    std::string openUrl;
    std::string reason;
    EditResult result = followLinkNow(doc, pages, page, x, y, useStoredLineHeight, openUrl, reason);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (url) *url = openUrl;
    if (why) *why = reason;
    return result;
}

EditResult editMakeToc(model::Document& doc, std::vector<Node>& pages, const TocOptions& options, bool useStoredLineHeight,
                       std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = makeTocNow(doc, pages, options, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editUpdateToc(model::Document& doc, std::vector<Node>& pages, bool all, bool useStoredLineHeight,
                         std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = updateTocNow(doc, pages, all, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editClearCharStyle(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                              std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = clearCharStyleNow(doc, pages, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editPageBorder(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, bool wholeDocument,
                          bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = pageBorderNow(doc, pages, fields, wholeDocument, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editCaption(model::Document& doc, std::vector<Node>& pages, const std::string& caption,
                       bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result;
    TableEditCommand cmd;
    if (pointerHold().selectedObject) {
        result = objectCaptionNow(doc, pages, caption, useStoredLineHeight, why);
    } else if (tableCommandAtCursor(doc, cmd)) {
        cmd.kind = TableEditKind::Caption;
        cmd.caption = caption;
        result = tableEditNow(doc, pages, cmd, useStoredLineHeight, why);
    } else {
        why = "캡션을 달 개체를 고르거나 캐럿을 표 안에 두십시오.";
    }
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editBookmark(model::Document& doc, std::vector<Node>& pages, const std::string& action, const std::string& name,
                        bool useStoredLineHeight, std::string* error, const std::string& newName) {
    std::string why;
    if (action == "goto") {
        EditResult moved = bookmarkNow(doc, pages, action, name, useStoredLineHeight, why);
        if (error) *error = why;
        return moved;
    }
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = bookmarkNow(doc, pages, action, name, useStoredLineHeight, why, newName);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editGlyphGroup(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = glyphGroupNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editConvertText(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                           bool useStoredLineHeight, std::string* error) {
    // 블록이 여러 문단에 걸칠 수 있다. 구역째 베껴 되돌리기 한 칸으로 둔다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = convertTextNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editSortParagraphs(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool useStoredLineHeight, std::string* error) {
    // 블록이 여러 문단에 걸친다. 구역째 베껴 되돌리기 한 칸으로 둔다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = sortParagraphsNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editDeleteMarks(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                           bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = deleteMarksNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editTableText(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                         bool useStoredLineHeight, std::string* error) {
    std::string action;
    for (const auto& [name, value] : fields) {
        if (name == "action") {
            if (const std::string* t = std::get_if<std::string>(&value)) action = *t;
        }
    }
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = action == "textToTable" ? textToTableNow(doc, pages, fields, useStoredLineHeight, why)
                                                : tableToTextNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editCopyShape(model::Document& doc, std::vector<Node>& pages, const std::string& action, int type,
                         bool useStoredLineHeight, std::string* error) {
    std::string why;
    if (action == "copy") {
        EditResult result = copyShapeNow(doc, pages, action, type, useStoredLineHeight, why);
        if (error) *error = why;
        return result;
    }
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = copyShapeNow(doc, pages, action, type, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editExchangeNotes(model::Document& doc, std::vector<Node>& pages, int mode, bool useStoredLineHeight,
                             std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = exchangeNotesNow(doc, pages, mode, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editGoToSection(model::Document& doc, std::vector<Node>& pages, int section, std::string* error) {
    std::string why;
    EditResult moved = goToSectionNow(doc, pages, section, why);
    if (error) *error = why;
    return moved;
}

EditResult editGoToPage(model::Document& doc, std::vector<Node>& pages, int page, std::string* error) {
    std::string why;
    EditResult moved = goToPageNow(doc, pages, page, why);
    if (error) *error = why;
    return moved;
}

EditResult editTableProperties(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                               bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = tablePropertiesNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editSectionSettings(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                               bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = sectionSettingsNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editColumns(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                       bool useStoredLineHeight, std::string* error) {
    // 문서 전체는 구역마다 첫 문단을 고치고, 새 단·새 쪽은 문단을 가른다. 구역째 베껴 둔다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = columnsNow(doc, pages, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editPageControl(model::Document& doc, std::vector<Node>& pages, PageControlKind kind,
                           const FormatFields& fields, bool useStoredLineHeight, std::string* error) {
    // 머리말·쪽 번호는 구역 첫 문단이나 표 칸 문단 등 캐럿 밖 문단을 고친다. 구역째 베껴 둔다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = pageControlNow(doc, pages, kind, fields, useStoredLineHeight, error);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editInsertBreak(model::Document& doc, std::vector<Node>& pages, BreakKind kind,
                           bool useStoredLineHeight) {
    // 구역 나누기는 구역 목록을, 나머지는 문단을 바꾼다. 둘 다 구역째 베껴 둔다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = insertBreakNow(doc, pages, kind, useStoredLineHeight);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editReplaceText(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                           const std::string& replacement, bool matchCase, bool useStoredLineHeight) {
    UndoTrack track(doc, pages, UndoKind::Other);
    EditResult result = replaceTextNow(doc, pages, text, replacement, matchCase, useStoredLineHeight);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editReplaceAllText(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                              const std::string& replacement, bool matchCase, bool useStoredLineHeight, int* count) {
    // 문서 곳곳의 문단을 고친다. 구역째 베껴 되돌리기 한 칸으로 둔다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    int changed = 0;
    EditResult result = replaceAllTextNow(doc, pages, text, replacement, matchCase, useStoredLineHeight, changed);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (count) *count = changed;
    return result;
}

EditResult editTable(model::Document& doc, std::vector<Node>& pages, const TableEditCommand& cmd,
                     bool useStoredLineHeight, std::string* error) {
    std::string why;
    int story = -1;
    int paragraph = -1;
    EditResult result;
    if (!tableEditHost(doc, cmd, story, paragraph)) {
        why = "story " + std::to_string(cmd.story) + " 은 표 칸이 아닙니다.";
    } else {
        // 표를 담은 문단을 베껴 둔다. 표 구조와 칸 글이 모두 그 문단 안에 있다.
        UndoTrack track(doc, pages, UndoKind::Other, story, paragraph);
        result = tableEditNow(doc, pages, cmd, useStoredLineHeight, why);
        track.finish(doc, pages, result, useStoredLineHeight);
    }
    if (error) *error = why;
    return result;
}

EditResult editInsertPicture(model::Document& doc, std::vector<Node>& pages, const Bytes& image,
                             bool useStoredLineHeight, std::int32_t width, std::int32_t height) {
    UndoTrack track(doc, pages, UndoKind::Other);
    EditResult result = insertPictureNow(doc, pages, image, useStoredLineHeight, width, height);
    track.finish(doc, pages, result, useStoredLineHeight);
    // 그림은 rows·cols 칸에 정한 크기를 둔다.
    if (result.needsFontSwap) pendingInsert() = PendingInsert{2, width, height, image};
    return result;
}

bool paragraphsAlike(const model::Paragraph& a, const model::Paragraph& b) { return sameParagraph(a, b); }

namespace paint_detail {

EditResult resumePendingInsert(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PendingInsert pending = std::move(pendingInsert());
    pendingInsert() = PendingInsert{};
    if (pending.kind == 1) return editInsertTable(doc, pages, pending.rows, pending.cols, useStoredLineHeight);
    if (pending.kind == 2) {
        return editInsertPicture(doc, pages, pending.image, useStoredLineHeight, pending.rows, pending.cols);
    }
    return editInsertDrawnShape(doc, pages, useStoredLineHeight);
}

}  // namespace paint_detail

void dropPendingInsert() {
    pendingInsert() = PendingInsert{};
    dropPendingDrawnShape();
}

void setDrawTool(int tool) {
    PointerHold& hold = pointerHold();
    hold.drawTool = tool >= 1 && tool <= kLastDrawTool ? tool : 0;
    hold.drawing = false;
    hold.drawPoints.clear();
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
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editInsertShapePoints(model::Document& doc, std::vector<Node>& pages, int tool, int page,
                                 const std::vector<model::ShapePoint>& points, bool useStoredLineHeight) {
    if (points.empty()) return {};
    std::int32_t top = points.front().y;
    for (const model::ShapePoint& pt : points) top = std::min(top, pt.y);
    int story = -1;
    int paragraph = -1;
    shapeAnchor(doc, pages, page, top, story, paragraph);
    UndoTrack track(doc, pages, UndoKind::Other, story, paragraph);
    EditResult result = insertShapeNow(doc, pages, tool, page, 0, 0, 0, 0, useStoredLineHeight, &points);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editInsertDrawnShape(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerHold& hold = pointerHold();
    if (!hold.drawPending) return {};
    int story = -1;
    int paragraph = -1;
    // 도형이 붙을 문단(insertShapeNow 와 같은 자리): 도형 상자 위쪽. 점으로 그린 도형은 가장 위 점이다.
    std::int32_t top = std::min(hold.drawY0, hold.drawY1);
    if (!hold.drawPoints.empty()) {
        top = hold.drawPoints.front().y;
        for (const model::ShapePoint& pt : hold.drawPoints) top = std::min(top, pt.y);
    }
    shapeAnchor(doc, pages, hold.drawPage, top, story, paragraph);
    UndoTrack track(doc, pages, UndoKind::Other, story, paragraph);
    EditResult result = insertDrawnShapeNow(doc, pages, useStoredLineHeight);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editObjectProperties(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                                const FormatFields& fields, bool useStoredLineHeight, std::string* error) {
    // 글을 비켜 가는 개체는 그 글 묶음의 저장된 줄을 다 지우므로 구역째 남긴다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = objectPropertiesNow(doc, pages, story, paragraph, item, fields, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editDeleteObject(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                            bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = deleteObjectNow(doc, pages, story, paragraph, item, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editInsertEquation(model::Document& doc, std::vector<Node>& pages, const std::string& script,
                              std::int32_t baseUnit, bool useStoredLineHeight, std::string* error) {
    UndoTrack track(doc, pages, UndoKind::Other);
    std::string why;
    EditResult result = insertEquationNow(doc, pages, script, baseUnit, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editEquationAt(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                          const std::string& script, std::int32_t baseUnit, bool useStoredLineHeight,
                          std::string* error) {
    // 띄운 수식은 글 묶음 줄을 다 다시 짜므로 구역째 남긴다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    std::string why;
    EditResult result = editEquationNow(doc, pages, story, paragraph, item, script, baseUnit, useStoredLineHeight, why);
    track.finish(doc, pages, result, useStoredLineHeight);
    if (error) *error = why;
    return result;
}

EditResult editEquation(model::Document& doc, std::vector<Node>& pages, const std::string& script,
                        std::int32_t baseUnit, bool useStoredLineHeight, std::string* error) {
    int story = -1;
    int paragraph = -1;
    int item = -1;
    if (!selectedEquationAt(doc, story, paragraph, item)) {
        if (error) *error = "고른 수식이 없습니다.";
        return {};
    }
    return editEquationAt(doc, pages, story, paragraph, item, script, baseUnit, useStoredLineHeight, error);
}

EditResult editMoveObject(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerHold& hold = pointerHold();
    if (!hold.movePending) return {};
    // 글을 비켜 가는 개체는 그 글 묶음의 저장된 줄을 다 지우므로 구역째 남긴다.
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = moveObjectNow(doc, pages, useStoredLineHeight);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editResizeObject(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerHold& hold = pointerHold();
    if (!hold.resizeObjectPending) return {};
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = resizeObjectNow(doc, pages, useStoredLineHeight);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

EditResult editResizeTableBorder(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerHold& hold = pointerHold();
    if (!hold.borderPending) return {};
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = resizeTableBorderNow(doc, pages, useStoredLineHeight);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

// 막힌 글꼴 바꾸기는 글꼴 표와 모든 문단의 저장된 줄 나눔을 바꾸므로 구역 칸으로 남긴다.
EditResult editSwapLicenseFonts(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                                const std::vector<std::pair<std::string, std::string>>& chosen) {
    UndoTrack track(doc, pages, UndoKind::Sections);
    EditResult result = swapLicenseFontsNow(doc, pages, useStoredLineHeight, chosen);
    track.finish(doc, pages, result, useStoredLineHeight);
    return result;
}

// 되돌리기·다시 하기도 편집이다. 계산식 결과는 입력에서 나오는 값이라 되돌리기 칸에 담지 않고, 되돌린 뒤 다시 계산한다.
namespace {

// 되돌리기·다시 하기는 쪽을 새로 짜서 고른 개체 노드가 낡는다. 고른 개체·손잡이·끌기 상태를 놓는다.
// 남겨 두면 옛 자리에 손잡이가 남고, 그 손잡이로 끌면 없어진 노드를 읽는다.
void dropObjectSelectionForHistory() {
    PointerHold& hold = pointerHold();
    hold.selectedObject = nullptr;
    hold.objectHandles = false;
    hold.resizeHandle = -1;
    hold.multiPicked.clear();
    hold.moving = false;
    hold.dragSnapshot.reset();
    hold.dragSnapshotId.clear();
    hold.boxes.clear();
    bumpPointerPicture();
}

}  // namespace

EditResult editUndo(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    dropObjectSelectionForHistory();
    EditChange whole;
    whole.whole = true;
    return afterEditCommitted(doc, pages, stepHistory(doc, pages, useStoredLineHeight, true), whole, useStoredLineHeight);
}

EditResult editRedo(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    dropObjectSelectionForHistory();
    EditChange whole;
    whole.whole = true;
    return afterEditCommitted(doc, pages, stepHistory(doc, pages, useStoredLineHeight, false), whole, useStoredLineHeight);
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
