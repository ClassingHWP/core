// 각주·미주 넣기. 한글 2024 InsertFootnote·InsertEndnote 와 같다.
// 캐럿 자리에 주 컨트롤(글자 한 칸)을 넣고, 주 글 묶음은 "각주"(미주는 "미주") 스타일 문단 하나에
// 번호(자동 번호, 모양은 구역의 주 모양)와 빈칸 하나다. 캐럿은 그 빈칸 뒤로 간다.
// 머리말·꼬리말 편집 중이거나 주 안이면 넣지 않는다.

#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace chwp::render {

using namespace paint_detail;

namespace {

const model::Style* styleNamed(const model::Document& doc, const char* engName, const char* name) {
    for (const model::Style& style : doc.header.styles) {
        if (style.engName == engName) return &style;
    }
    for (const model::Style& style : doc.header.styles) {
        if (style.name == name) return &style;
    }
    return nullptr;
}

// 캐럿이 주·머리말·꼬리말 글 묶음 안인가.
bool inNoteOrBand(const model::Document& doc, int story) {
    const model::StoryMap& map = ensureStoryMap(doc);
    for (int depth = 0; depth < 32 && map.valid(story); ++depth) {
        const model::StorySlot* slot = map.slot(story);
        if (slot->control) {
            const model::ControlKind kind = slot->control->kind;
            if (kind == model::ControlKind::Footnote || kind == model::ControlKind::Endnote ||
                kind == model::ControlKind::Header || kind == model::ControlKind::Footer) {
                return true;
            }
        }
        if (slot->parentStory <= 0) break;
        story = slot->parentStory;
    }
    return false;
}

// 문서 차례로 앞에 있는 같은 종류 주의 수 + 1. 번호 그리기는 구역의 주 모양으로 다시 센다.
std::uint32_t noteOrdinal(const model::Document& doc, const model::Control* target, model::ControlKind kind) {
    std::uint32_t count = 0;
    bool found = false;
    std::function<void(const std::vector<model::Paragraph>&)> walk = [&](const std::vector<model::Paragraph>& paras) {
        for (const model::Paragraph& para : paras) {
            for (const model::Run& run : para.runs) {
                for (const model::InlineItem& item : run.items) {
                    if (found || item.type != model::InlineItem::Type::Control) continue;
                    const model::Control& c = item.control;
                    if (&c == target) {
                        found = true;
                        return;
                    }
                    if (c.kind == kind) ++count;
                    if (c.table) {
                        for (const model::TableCell& cell : c.table->cells) walk(cell.content.paragraphs);
                    }
                    if (c.kind != model::ControlKind::Footnote && c.kind != model::ControlKind::Endnote) {
                        for (const model::SubList& sub : c.subLists) walk(sub.paragraphs);
                    }
                }
            }
        }
    };
    for (const model::Section& section : doc.sections) {
        if (!found) walk(section.paragraphs);
    }
    return count + 1;
}

// 그 문단 묶음을 그린 글 묶음 번호. 주는 쪽마다 번호를 바꾼 사본을 별칭 글 묶음으로 그린다.
int shownStory(const model::Document& doc, const std::vector<Node>& pages, const std::vector<model::Paragraph>* paragraphs) {
    const model::StoryMap& map = ensureStoryMap(doc);
    std::vector<const Node*> stack;
    for (const Node& page : pages) stack.push_back(&page);
    while (!stack.empty()) {
        const Node* node = stack.back();
        stack.pop_back();
        if (node->kind == NodeKind::Line && node->story > 0) {
            const model::StorySlot* slot = map.slot(node->story);
            if (slot && slot->paragraphs == paragraphs) return node->story;
        }
        for (const Node& child : node->children) stack.push_back(&child);
    }
    return map.storyOf(paragraphs);
}

}  // namespace

// 각주↔미주 바꾸기(한글 2024 ExchangeFootnoteEndnote, 한글 2024 에서 확인). mode 0 모든 각주를 미주로,
// 1 모든 미주를 각주로, 2 서로 바꾸기. 바꾼 주는 종류(fn·en), 안 글의 번호 종류(FOOTNOTE·ENDNOTE), 그리고 안 글 문단이
// 각주 스타일이면 미주 스타일로(거꾸로도) 바뀐다. 문단·글자 모양은 그대로다. 번호는 문서 차례로 다시 센다.
EditResult exchangeNotesNow(model::Document& doc, std::vector<Node>& pages, int mode, bool useStoredLineHeight,
                            std::string& why) {
    if (readOnly()) return {};
    const model::Style* footStyle = styleNamed(doc, "Footnote", "각주");
    const model::Style* endStyle = styleNamed(doc, "Endnote", "미주");
    const std::uint32_t footId = footStyle ? footStyle->id : 0xFFFFFFFFu;
    const std::uint32_t endId = endStyle ? endStyle->id : 0xFFFFFFFFu;
    std::vector<model::Control*> notes;
    int changed = 0;
    std::function<void(std::vector<model::Paragraph>&)> walk = [&](std::vector<model::Paragraph>& paras) {
        for (model::Paragraph& para : paras) {
            for (model::Run& run : para.runs) {
                for (model::InlineItem& item : run.items) {
                    if (item.type != model::InlineItem::Type::Control) continue;
                    model::Control& c = item.control;
                    if (c.table) {
                        for (model::TableCell& cell : c.table->cells) walk(cell.content.paragraphs);
                    }
                    const bool foot = c.kind == model::ControlKind::Footnote;
                    const bool end = c.kind == model::ControlKind::Endnote;
                    if (!foot && !end) {
                        for (model::SubList& sub : c.subLists) walk(sub.paragraphs);
                        continue;
                    }
                    notes.push_back(&c);
                    if (!(mode == 2 || (mode == 0 && foot) || (mode == 1 && end))) continue;
                    c.kind = foot ? model::ControlKind::Endnote : model::ControlKind::Footnote;
                    c.ctrlId = foot ? "en  " : "fn  ";
                    for (model::SubList& sub : c.subLists) {
                        for (model::Paragraph& inner : sub.paragraphs) {
                            if (inner.styleId == (foot ? footId : endId)) inner.styleId = foot ? endId : footId;
                            inner.lineSegs.clear();
                            for (model::Run& r : inner.runs) {
                                for (model::InlineItem& in : r.items) {
                                    if (in.type == model::InlineItem::Type::Control &&
                                        in.control.kind == model::ControlKind::AutoNumber &&
                                        in.control.numType == (foot ? "FOOTNOTE" : "ENDNOTE")) {
                                        in.control.numType = foot ? "ENDNOTE" : "FOOTNOTE";
                                    }
                                }
                            }
                        }
                    }
                    para.lineSegs.clear();
                    ++changed;
                }
            }
        }
    };
    for (model::Section& section : doc.sections) walk(section.paragraphs);
    if (changed == 0) {
        why = mode == 1 ? "바꿀 미주가 없습니다." : mode == 0 ? "바꿀 각주가 없습니다." : "바꿀 각주·미주가 없습니다.";
        return {};
    }
    for (model::Control* c : notes) c->number = noteOrdinal(doc, c, c->kind);
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    hold.preferredX = -1;
    const PointerCursor cur = pointerCursor();
    const int caretChar = cur.active && cur.story > 0 ? offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset) : 0;
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    if (cur.active && cur.story > 0 && resolveStory(doc, cur.story)) {
        setPointerCursor(doc, pages,
                         model::CursorPos{cur.story, cur.paragraph, charIndexToOffset(doc, pages, cur.story, cur.paragraph, caretChar)});
    }
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

EditResult insertNoteNow(model::Document& doc, std::vector<Node>& pages, bool endnote,
                                       bool useStoredLineHeight, std::string& error) {
    if (readOnly()) return {};
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    if (hold.bandStory > 0 || inNoteOrBand(doc, cur.story)) {
        error = endnote ? "미주는 본문과 표·글상자 안에서만 넣을 수 있습니다."
                        : "각주는 본문과 표·글상자 안에서만 넣을 수 있습니다.";
        return {};
    }
    model::Paragraph* host = storyParagraph(doc, cur.story, cur.paragraph);
    if (!host) return {};
    clearComposition(hold);
    const int section = std::max(0, cursorSection(doc));
    const model::NoteShape& shape = endnote ? doc.sections[static_cast<std::size_t>(section)].endnote
                                            : doc.sections[static_cast<std::size_t>(section)].footnote;
    const int charIndex = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);

    model::Control note;
    note.kind = endnote ? model::ControlKind::Endnote : model::ControlKind::Footnote;
    note.ctrlId = endnote ? "en  " : "fn  ";
    model::SubList list;
    list.vertAlign = "TOP";
    model::Paragraph para;
    model::Run run;
    if (const model::Style* style = endnote ? styleNamed(doc, "Endnote", "미주") : styleNamed(doc, "Footnote", "각주")) {
        para.styleId = style->id;
        para.paraShapeId = style->paraShapeId;
        run.charShapeId = style->charShapeId;
    } else {
        para.styleId = host->styleId;
        para.paraShapeId = host->paraShapeId;
        run.charShapeId = charShapeAtParagraphChar(*host, charIndex, false);
    }
    model::Control number;
    number.kind = model::ControlKind::AutoNumber;
    number.ctrlId = "atno";
    number.numType = endnote ? "ENDNOTE" : "FOOTNOTE";
    number.numFormat = shape.numFormat.empty() ? "DIGIT" : shape.numFormat;
    number.prefix = shape.prefix;
    number.suffix = shape.suffix;
    run.items.push_back(model::InlineItem::makeControl(std::move(number)));
    run.items.push_back(model::InlineItem::makeText(" "));
    para.runs.push_back(std::move(run));
    list.paragraphs.push_back(std::move(para));
    note.subLists.push_back(std::move(list));

    // 주는 글자 한 칸이다. 캐럿 자리의 글 조각을 갈라 그 사이에 둔다.
    const std::uint32_t charShape = charShapeAtParagraphChar(*host, charIndex, false);
    model::Paragraph right = splitParagraphAt(*host, charIndex);
    model::Run noteRun;
    noteRun.charShapeId = charShape;
    noteRun.items.push_back(model::InlineItem::makeControl(std::move(note)));
    host->runs.push_back(std::move(noteRun));
    const std::size_t noteRunIndex = host->runs.size() - 1;
    for (model::Run& r : right.runs) {
        if (!r.items.empty()) host->runs.push_back(std::move(r));
    }
    host->lineSegs.clear();
    model::Control* placed = &host->runs[noteRunIndex].items.front().control;
    placed->number = noteOrdinal(doc, placed, placed->kind);

    hold.preferredX = -1;
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    const int noteStory = shownStory(doc, pages, &placed->subLists.front().paragraphs);
    if (noteStory > 0) {
        setPointerCursor(doc, pages, model::CursorPos{noteStory, 0, charIndexToOffset(doc, pages, noteStory, 0, 1)});
    }
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

// 하이퍼링크 명령의 주소. 한글 2024 하이퍼링크 대화 상자는 \ : ; ? # | 앞에 \ 를 붙여 저장한다
// ("https://a.kr:8080/p;q?x=1&y=2#f|b\c" → "https\://a.kr\:8080/p\;q\?x=1&y=2\#f\|b\\c"). 다른 문장 부호(& = % ! , 등)·
// 한글은 그대로다. 붙이지 않으면 한글은 ? 뒤를 주소로 보지 않는다(하이퍼링크 고치기 대화 상자가 "…/watch" 만 보인다).
static std::string hyperlinkCommandUrl(const std::string& url) {
    std::string out;
    out.reserve(url.size() + 8);
    for (const char c : url) {
        if (c == '\\' || c == ':' || c == ';' || c == '?' || c == '#' || c == '|') out += '\\';
        out += c;
    }
    return out;
}

// 하이퍼링크 넣기(한글 InsertHyperlink). 고른 글이 있으면 지우고, 캐럿 자리에 필드 시작(%hlk, 명령
// "주소;1;0;0;", 주소는 hyperlinkCommandUrl) · 보일 글 · 필드 끝을 넣는다. 보일 글은 캐럿 글자 모양에 파란 글자와
// 파란 밑줄이다. 캐럿은 보일 글 뒤로 간다.
EditResult insertHyperlinkNow(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                              const std::string& url, bool useStoredLineHeight, std::string& error) {
    if (readOnly()) return {};
    if (url.empty()) {
        error = "연결할 주소가 없습니다.";
        return {};
    }
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    if (!storyParagraph(doc, cur.story, cur.paragraph)) return {};
    clearComposition(hold);
    int paraIdx = cur.paragraph;
    int charIndex = 0;
    if (cur.hasSelection) {
        deleteSelectionRange(doc, pages, cur.story, cur.startParagraph, cur.startOffset, cur.endParagraph,
                             cur.endOffset, paraIdx, charIndex);
    } else {
        charIndex = offsetToCharIndex(doc, pages, cur.story, paraIdx, cur.offset);
    }
    model::Paragraph* host = storyParagraph(doc, cur.story, paraIdx);
    if (!host) return {};
    const std::string shown = text.empty() ? url : text;
    const std::uint32_t baseShape = charShapeAtParagraphChar(*host, charIndex, false);
    FormatPatch link;
    link.hasColor = true;
    link.color = "#0000FF";
    link.hasUnderlineType = true;
    link.underlineType = "BOTTOM";
    link.fields.emplace_back("underlineShape", std::string("SOLID"));
    link.fields.emplace_back("underlineColor", std::string("#0000FF"));
    const std::uint32_t linkShape = deriveCharShapeId(doc, baseShape, link);

    model::Control begin;
    begin.kind = model::ControlKind::Field;
    begin.ctrlId = "%hlk";
    begin.command = hyperlinkCommandUrl(url) + ";1;0;0;";
    model::Control end;
    end.kind = model::ControlKind::FieldEnd;
    end.ctrlId = "fend";

    model::Paragraph right = splitParagraphAt(*host, charIndex);
    model::Run open;
    open.charShapeId = baseShape;
    open.items.push_back(model::InlineItem::makeControl(std::move(begin)));
    model::Run body;
    body.charShapeId = linkShape;
    body.items.push_back(model::InlineItem::makeText(shown));
    model::Run close;
    close.charShapeId = baseShape;
    close.items.push_back(model::InlineItem::makeControl(std::move(end)));
    host->runs.push_back(std::move(open));
    host->runs.push_back(std::move(body));
    host->runs.push_back(std::move(close));
    for (model::Run& r : right.runs) {
        if (!r.items.empty()) host->runs.push_back(std::move(r));
    }
    host->lineSegs.clear();
    hold.preferredX = -1;
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    const int after = charIndex + utf8CodePointCount(shown);
    const int afterOff = charIndexToOffset(doc, pages, cur.story, paraIdx, after);
    // 캐럿은 필드 끝 뒤다. 이어 치는 글은 링크 모양이 아니라 원래 글자 모양이다.
    hold.hasPendingCharShape = true;
    hold.pendingCharShapeId = baseShape;
    hold.pendingCharStory = cur.story;
    hold.pendingCharPara = paraIdx;
    hold.pendingCharOffset = afterOff;
    setPointerCursor(doc, pages, model::CursorPos{cur.story, paraIdx, afterOff});
    return makeEditResult(doc, true, true, cur.story, paraIdx, &relayout);
}

EditResult leaveNoteNow(model::Document& doc, std::vector<Node>& pages) {
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    const model::StorySlot* slot = resolveStory(doc, cur.story);
    if (!slot || !slot->control || slot->parentStory <= 0 ||
        (slot->control->kind != model::ControlKind::Footnote && slot->control->kind != model::ControlKind::Endnote)) {
        return {};
    }
    const model::Paragraph* host = storyParagraph(doc, slot->parentStory, slot->parentParagraph);
    if (!host) return {};
    // 주는 글자 한 칸이다. 그 칸 뒤 글자 번호를 센다.
    int charIndex = 0;
    bool found = false;
    for (const model::Run& run : host->runs) {
        for (const model::InlineItem& item : run.items) {
            if (found) break;
            if (item.type == model::InlineItem::Type::Text) {
                charIndex += utf8CodePointCount(item.text);
            } else if (&item.control == slot->control) {
                charIndex += 1;
                found = true;
            } else if (isCaretCharControl(item.control)) {
                charIndex += 1;
            }
        }
    }
    if (!found) return {};
    clearComposition(pointerHold());
    const int story = slot->parentStory;
    const int paragraph = slot->parentParagraph;
    setPointerCursor(doc, pages, model::CursorPos{story, paragraph, charIndexToOffset(doc, pages, story, paragraph, charIndex)});
    return makeEditResult(doc, true, false, story, paragraph);
}

}  // namespace chwp::render
