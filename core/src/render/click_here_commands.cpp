// 누름틀(%clk). 한글 2024 로 잰 동작:
// - 넣기(입력 탭 누름틀): 캐럿 자리에 필드 시작 · 안내문(캐럿 글자 모양에 빨간 기울임) · 필드 끝. 고친 표시(dirty)는 없다.
// - 손대지 않은 누름틀의 안내문은 내용이 아니다(필드 글은 빈 글). 캐럿이 서는 자리는 누름틀 안 한 곳뿐이다.
// - 그 안에 치면 안내문이 사라지고 친 글은 누름틀 앞 글자 모양으로 들어가며 고친 표시가 붙는다.
// - 안에서 Delete·BackSpace 는 누름틀을 통째로 지운다. ← 는 누름틀 앞, → 는 누름틀 뒤로 간다.
// - 친 글을 다 지우면 다시 안내문이 보인다(빈 누름틀). 우리는 안내문을 다시 넣고 손대지 않은 것으로 되돌린다
//   (한글도 손대지 않은 누름틀은 안내문을 글로 저장한다).
// 모델에서 누름틀 시작·끝은 글자 칸이 없어 "누름틀 앞"과 "누름틀 안"이 같은 글자 번호다. 안쪽 자리는 캐럿 상태
// (PointerHold::clickHere*)가 따로 들고 있다.

#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace chwp::render {

using namespace paint_detail;

namespace {

constexpr const char* kDefaultGuide = "이곳을 마우스로 누르고 내용을 입력하세요.";

// 누름틀 명령(한글 2024 가 쓰는 꼴): "Clickhere:set:<본문 글자 수>:Direction:wstring:<n>:<안내문> HelpState:wstring:<m>:<메모>  ".
// 본문은 두 항목(끝에 빈칸 하나씩)이고 그 뒤에 빈칸이 하나 더 있다.
std::string clickHereCommand(const std::string& direction, const std::string& memo) {
    const auto part = [](const char* key, const std::string& value) {
        return std::string(key) + ":wstring:" + std::to_string(utf8CodePointCount(value)) + ":" + value + " ";
    };
    const std::string body = part("Direction", direction) + part("HelpState", memo);
    return "Clickhere:set:" + std::to_string(utf8CodePointCount(body)) + ":" + body + " ";
}

// 명령의 "<key>:wstring:<n>:<값>" 에서 값(글자 수 n 만큼).
std::string commandValue(const std::string& command, const std::string& key) {
    const std::string tag = key + ":wstring:";
    const std::size_t at = command.find(tag);
    if (at == std::string::npos) return {};
    std::size_t i = at + tag.size();
    const std::size_t colon = command.find(':', i);
    if (colon == std::string::npos) return {};
    const int count = std::atoi(command.substr(i, colon - i).c_str());
    const std::string rest = command.substr(colon + 1);
    return utf8SliceCodePoints(rest, 0, std::max(0, count));
}

bool isClickHere(const model::InlineItem& item) {
    return item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::Field &&
           item.control.ctrlId == "%clk";
}

bool isFieldEnd(const model::InlineItem& item) {
    return item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::FieldEnd;
}

int chars(const model::InlineItem& item) {
    if (item.type == model::InlineItem::Type::Text) return static_cast<int>(utf8CodePointCount(item.text));
    return isCaretCharControl(item.control) ? 1 : 0;
}

// 문단 안 누름틀 하나: 시작·끝 항목 자리와 그 사이 글자 구간.
struct ClickHereSpan {
    model::Control* begin = nullptr;
    std::size_t openRun = 0, openItem = 0;
    std::size_t endRun = 0, endItem = 0;
    int openPos = 0;
    int endPos = 0;
    std::uint32_t openShape = 0;
    [[nodiscard]] bool untouched() const { return (begin->fieldProps & 0x8000u) == 0; }
};

std::vector<ClickHereSpan> clickHereSpans(model::Paragraph& para) {
    std::vector<ClickHereSpan> out;
    int pos = 0;
    ClickHereSpan open;
    for (std::size_t r = 0; r < para.runs.size(); ++r) {
        for (std::size_t i = 0; i < para.runs[r].items.size(); ++i) {
            model::InlineItem& item = para.runs[r].items[i];
            if (isClickHere(item)) {
                open = ClickHereSpan{};
                open.begin = &item.control;
                open.openRun = r;
                open.openItem = i;
                open.openPos = pos;
                open.openShape = para.runs[r].charShapeId;
            } else if (isFieldEnd(item) && open.begin) {
                open.endRun = r;
                open.endItem = i;
                open.endPos = pos;
                out.push_back(open);
                open = ClickHereSpan{};
            }
            pos += chars(item);
        }
    }
    return out;
}

bool slotHere(const PointerHold& hold, int story, int para, int at) {
    return hold.clickHereStory == story && hold.clickHerePara == para && hold.clickHereChar == at;
}

void setSlot(PointerHold& hold, int story, int para, int at) {
    hold.clickHereStory = story;
    hold.clickHerePara = para;
    hold.clickHereChar = at;
}

void clearSlot(PointerHold& hold) { hold.clickHereStory = -1; }

// 캐럿(블록 없음) 자리: 이야기·문단·글자 번호. 없으면 거짓.
bool caretChar(const model::Document& doc, const std::vector<Node>& pages, int& story, int& para, int& at) {
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0 || cur.hasSelection) return false;
    story = cur.story;
    para = cur.paragraph;
    at = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    return true;
}

// 캐럿이 누름틀 "안"인 손대지 않은 누름틀(안내문 글자 사이, 또는 시작 자리에 안쪽 표시).
ClickHereSpan* untouchedAt(std::vector<ClickHereSpan>& spans, const PointerHold& hold, int story, int para, int at) {
    for (ClickHereSpan& span : spans) {
        if (!span.untouched()) continue;
        if ((at > span.openPos && at < span.endPos) || (at == span.openPos && slotHere(hold, story, para, at))) {
            return &span;
        }
    }
    return nullptr;
}

void placeAt(model::Document& doc, std::vector<Node>& pages, int story, int para, int at) {
    setPointerCursor(doc, pages, model::CursorPos{story, para, charIndexToOffset(doc, pages, story, para, at)});
}

void relayoutWhole(model::Document& doc, std::vector<Node>& pages, model::Paragraph& para, bool useStoredLineHeight) {
    para.lineSegs.clear();
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
}

std::uint32_t guideShape(model::Document& doc, std::uint32_t baseShape) {
    FormatPatch patch;
    patch.hasColor = true;
    patch.color = "#FF0000";
    patch.hasItalic = true;
    patch.italic = true;
    return deriveCharShapeId(doc, baseShape, patch);
}

}  // namespace

std::string clickHereGuide(const model::Control& field) {
    for (const auto& [name, value] : field.fieldStrings) {
        if (name == "Direction" && !value.empty()) return value;
    }
    const std::string fromCommand = commandValue(field.command, "Direction");
    return fromCommand.empty() ? std::string(kDefaultGuide) : fromCommand;
}

std::string clickHereMemo(const model::Control& field) {
    for (const auto& [name, value] : field.fieldStrings) {
        if (name == "HelpState") return value;
    }
    return commandValue(field.command, "HelpState");
}

EditResult insertClickHereNow(model::Document& doc, std::vector<Node>& pages, const std::string& direction,
                              const std::string& memo, const std::string& name, bool useStoredLineHeight,
                              std::string& error) {
    if (readOnly()) return {};
    const std::string guide = direction.empty() ? std::string(kDefaultGuide) : direction;
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
    if (!host) {
        error = "누름틀을 넣을 자리가 없습니다.";
        return {};
    }
    // 안내문은 캐럿 글자 모양에 빨간 기울임이다(한글이 손대지 않은 누름틀을 저장할 때 쓰는 모양과 같다).
    const std::uint32_t baseShape = charShapeAtParagraphChar(*host, charIndex, false);
    const std::uint32_t shownShape = guideShape(doc, baseShape);

    model::Control begin;
    begin.kind = model::ControlKind::Field;
    begin.ctrlId = "%clk";
    begin.fieldType = "CLICK_HERE";
    begin.name = name;
    begin.command = clickHereCommand(guide, memo);
    // 메모가 없으면 한글은 HelpState 매개변수를 두지 않는다.
    begin.fieldStrings = {{"Direction", guide}};
    if (!memo.empty()) begin.fieldStrings.emplace_back("HelpState", memo);
    begin.fieldProps = 1;  // 고칠 수 있음. 고친 표시(0x8000)는 없다
    begin.fieldExtra = 9;
    model::Control end;
    end.kind = model::ControlKind::FieldEnd;
    end.ctrlId = "fend";

    host = storyParagraph(doc, cur.story, paraIdx);
    model::Paragraph right = splitParagraphAt(*host, charIndex);
    model::Run open;
    open.charShapeId = baseShape;
    open.items.push_back(model::InlineItem::makeControl(std::move(begin)));
    model::Run body;
    body.charShapeId = shownShape;
    body.items.push_back(model::InlineItem::makeText(guide));
    model::Run close;
    close.charShapeId = baseShape;
    close.items.push_back(model::InlineItem::makeControl(std::move(end)));
    host->runs.push_back(std::move(open));
    host->runs.push_back(std::move(body));
    host->runs.push_back(std::move(close));
    for (model::Run& r : right.runs) {
        if (!r.items.empty()) host->runs.push_back(std::move(r));
    }
    hold.preferredX = -1;
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    relayoutWhole(doc, pages, *host, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    // 캐럿은 누름틀 안이다(한글). 바로 치면 안내문이 사라지고 친 글이 들어간다.
    placeAt(doc, pages, cur.story, paraIdx, charIndex);
    setSlot(pointerHold(), cur.story, paraIdx, charIndex);
    return makeEditResult(doc, true, true, cur.story, paraIdx, &relayout);
}

bool clearClickHereGuideAtCaret(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    int story = 0, paraIdx = 0, at = 0;
    if (readOnly() || hold.composing || !caretChar(doc, pages, story, paraIdx, at)) return false;
    model::Paragraph* para = storyParagraph(doc, story, paraIdx);
    if (!para) return false;
    std::vector<ClickHereSpan> spans = clickHereSpans(*para);
    ClickHereSpan* span = untouchedAt(spans, hold, story, paraIdx, at);
    if (!span) return false;
    // 시작 뒤부터 끝 앞까지의 항목(안내문)을 지우고, 안 자리를 빈 글 조각으로 둔다(바로 친 글이 누름틀 안에 들어간다).
    for (std::size_t rr = span->openRun; rr <= span->endRun; ++rr) {
        auto& items = para->runs[rr].items;
        const std::size_t from = rr == span->openRun ? span->openItem + 1 : 0;
        const std::size_t to = rr == span->endRun ? span->endItem : items.size();
        if (from < to) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(from), items.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
    span->begin->fieldProps |= 0x8000u;
    auto& openItems = para->runs[span->openRun].items;
    openItems.insert(openItems.begin() + static_cast<std::ptrdiff_t>(span->openItem + 1), model::InlineItem::makeText(""));
    const int openPos = span->openPos;
    const std::uint32_t openShape = span->openShape;
    std::erase_if(para->runs, [](const model::Run& run) { return run.items.empty(); });
    relayoutWhole(doc, pages, *para, useStoredLineHeight);
    const int offset = charIndexToOffset(doc, pages, story, paraIdx, openPos);
    setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, offset});
    clearSlot(hold);
    hold.hasPendingCharShape = true;
    hold.pendingCharShapeId = openShape;
    hold.pendingCharStory = story;
    hold.pendingCharPara = paraIdx;
    hold.pendingCharOffset = offset;
    return true;
}

bool restoreClickHereGuides(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    int story = 0, paraIdx = 0, at = 0;
    if (!caretChar(doc, pages, story, paraIdx, at)) return false;
    model::Paragraph* para = storyParagraph(doc, story, paraIdx);
    if (!para) return false;
    bool changed = false;
    int slotAt = -1;
    // 뒤에서부터 채워야 앞 누름틀 자리가 밀리지 않는다.
    std::vector<ClickHereSpan> spans = clickHereSpans(*para);
    for (auto it = spans.rbegin(); it != spans.rend(); ++it) {
        ClickHereSpan& span = *it;
        if (span.untouched() || span.endPos != span.openPos) continue;
        // 빈 글 조각 말고는 아무것도 없는 고친 누름틀: 안내문을 다시 넣고 손대지 않은 것으로 되돌린다.
        bool empty = true;
        for (std::size_t rr = span.openRun; rr <= span.endRun && empty; ++rr) {
            const auto& items = para->runs[rr].items;
            const std::size_t from = rr == span.openRun ? span.openItem + 1 : 0;
            const std::size_t to = rr == span.endRun ? span.endItem : items.size();
            for (std::size_t k = from; k < to; ++k) {
                if (items[k].type != model::InlineItem::Type::Text || !items[k].text.empty()) empty = false;
            }
        }
        if (!empty) continue;
        model::Control& field = *span.begin;
        const std::string guide = clickHereGuide(field);
        field.fieldProps &= ~0x8000u;
        const std::uint32_t shown = guideShape(doc, span.openShape);
        // 시작 항목 뒤에서 조각을 나눠 안내문 조각을 넣는다.
        model::Run& openRun = para->runs[span.openRun];
        model::Run tail;
        tail.charShapeId = openRun.charShapeId;
        tail.items.assign(openRun.items.begin() + static_cast<std::ptrdiff_t>(span.openItem + 1), openRun.items.end());
        openRun.items.erase(openRun.items.begin() + static_cast<std::ptrdiff_t>(span.openItem + 1), openRun.items.end());
        std::erase_if(tail.items, [](const model::InlineItem& item) {
            return item.type == model::InlineItem::Type::Text && item.text.empty();
        });
        model::Run body;
        body.charShapeId = shown;
        body.items.push_back(model::InlineItem::makeText(guide));
        auto where = para->runs.begin() + static_cast<std::ptrdiff_t>(span.openRun + 1);
        where = para->runs.insert(where, std::move(body));
        if (!tail.items.empty()) para->runs.insert(where + 1, std::move(tail));
        if (at == span.openPos) slotAt = span.openPos;
        changed = true;
    }
    if (!changed) return false;
    std::erase_if(para->runs, [](const model::Run& run) { return run.items.empty(); });
    relayoutWhole(doc, pages, *para, useStoredLineHeight);
    placeAt(doc, pages, story, paraIdx, at);
    if (slotAt >= 0) setSlot(pointerHold(), story, paraIdx, slotAt);
    return true;
}

bool deleteClickHereAtCaret(model::Document& doc, std::vector<Node>& pages, EditKey key, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    int story = 0, paraIdx = 0, at = 0;
    if (readOnly() || !caretChar(doc, pages, story, paraIdx, at)) return false;
    model::Paragraph* para = storyParagraph(doc, story, paraIdx);
    if (!para) return false;
    std::vector<ClickHereSpan> spans = clickHereSpans(*para);
    ClickHereSpan* target = untouchedAt(spans, hold, story, paraIdx, at);
    if (!target) {
        // 누름틀 바로 앞에서 Delete, 바로 뒤에서 BackSpace 도 손대지 않은 누름틀 하나를 통째로 지운다.
        for (ClickHereSpan& span : spans) {
            if (!span.untouched()) continue;
            if ((key == EditKey::Delete && at == span.openPos) || (key == EditKey::Backspace && at == span.endPos)) {
                target = &span;
                break;
            }
        }
    }
    if (!target) return false;
    for (std::size_t rr = target->endRun + 1; rr-- > target->openRun;) {
        auto& items = para->runs[rr].items;
        const std::size_t from = rr == target->openRun ? target->openItem : 0;
        const std::size_t to = rr == target->endRun ? target->endItem + 1 : items.size();
        if (from < to) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(from), items.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
    const int openPos = target->openPos;
    std::erase_if(para->runs, [](const model::Run& run) { return run.items.empty(); });
    if (para->runs.empty()) para->runs.emplace_back();
    relayoutWhole(doc, pages, *para, useStoredLineHeight);
    placeAt(doc, pages, story, paraIdx, openPos);
    clearSlot(hold);
    return true;
}

bool moveOverClickHere(const model::Document& doc, const std::vector<Node>& pages, EditKey key) {
    if (key != EditKey::ArrowLeft && key != EditKey::ArrowRight) return false;
    PointerHold& hold = pointerHold();
    int story = 0, paraIdx = 0, at = 0;
    if (!caretChar(doc, pages, story, paraIdx, at)) return false;
    model::Paragraph* para = storyParagraph(const_cast<model::Document&>(doc), story, paraIdx);
    if (!para) return false;
    std::vector<ClickHereSpan> spans = clickHereSpans(*para);
    for (ClickHereSpan& span : spans) {
        if (!span.untouched()) continue;
        const bool inside = (at > span.openPos && at < span.endPos) || (at == span.openPos && slotHere(hold, story, paraIdx, at));
        if (key == EditKey::ArrowLeft) {
            // 안에서 ← 는 누름틀 앞(같은 글자 번호, 안쪽 표시만 뗀다). 뒤에서 ← 는 안으로 들어간다.
            if (inside) {
                placeAt(const_cast<model::Document&>(doc), const_cast<std::vector<Node>&>(pages), story, paraIdx, span.openPos);
                clearSlot(hold);
                return true;
            }
            if (at == span.endPos) {
                placeAt(const_cast<model::Document&>(doc), const_cast<std::vector<Node>&>(pages), story, paraIdx, span.openPos);
                setSlot(hold, story, paraIdx, span.openPos);
                return true;
            }
        } else {
            // 안에서 → 는 누름틀 뒤. 앞에서 → 는 안으로 들어간다.
            if (inside) {
                placeAt(const_cast<model::Document&>(doc), const_cast<std::vector<Node>&>(pages), story, paraIdx, span.endPos);
                clearSlot(hold);
                return true;
            }
            if (at == span.openPos) {
                setSlot(hold, story, paraIdx, span.openPos);
                return true;
            }
        }
    }
    return false;
}

void snapCaretIntoClickHere(const model::Document& doc, const std::vector<Node>& pages) {
    PointerHold& hold = pointerHold();
    int story = 0, paraIdx = 0, at = 0;
    if (!caretChar(doc, pages, story, paraIdx, at)) return;
    model::Paragraph* para = storyParagraph(const_cast<model::Document&>(doc), story, paraIdx);
    if (!para) return;
    for (const ClickHereSpan& span : clickHereSpans(*para)) {
        if (!span.untouched() || !(at > span.openPos && at < span.endPos)) continue;
        // 안내문을 누르면 캐럿은 누름틀 안 한 자리다(한글은 안내문 글자 사이에 캐럿을 두지 않는다).
        setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, charIndexToOffset(doc, pages, story, paraIdx, span.openPos)});
        setSlot(hold, story, paraIdx, span.openPos);
        return;
    }
}

namespace {

// 누름틀 시작 뒤부터 끝 앞까지의 항목을 지운다(빈 조각은 runs 정리에서 빠진다).
void eraseBody(model::Paragraph& para, const ClickHereSpan& span) {
    for (std::size_t rr = span.endRun + 1; rr-- > span.openRun;) {
        auto& items = para.runs[rr].items;
        const std::size_t from = rr == span.openRun ? span.openItem + 1 : 0;
        const std::size_t to = rr == span.endRun ? span.endItem : items.size();
        if (from < to) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(from), items.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
}

// 누름틀 안이 빈 글 조각뿐인가.
bool bodyEmpty(const model::Paragraph& para, const ClickHereSpan& span) {
    for (std::size_t rr = span.openRun; rr <= span.endRun; ++rr) {
        const auto& items = para.runs[rr].items;
        const std::size_t from = rr == span.openRun ? span.openItem + 1 : 0;
        const std::size_t to = rr == span.endRun ? span.endItem : items.size();
        for (std::size_t k = from; k < to; ++k) {
            if (items[k].type != model::InlineItem::Type::Text || !items[k].text.empty()) return false;
        }
    }
    return true;
}

// 숨긴 자리(시작이 openPos 인 손대지 않은 빈 누름틀)에 안내문 조각을 다시 넣는다. 넣었으면 참.
bool putGuideBack(model::Paragraph& para, const HiddenClickGuide& hidden) {
    std::vector<ClickHereSpan> spans = clickHereSpans(para);
    for (ClickHereSpan& span : spans) {
        if (span.openPos != hidden.openPos || !span.untouched() || !bodyEmpty(para, span)) continue;
        eraseBody(para, span);
        model::Run& openRun = para.runs[span.openRun];
        model::Run tail;
        tail.charShapeId = openRun.charShapeId;
        tail.items.assign(openRun.items.begin() + static_cast<std::ptrdiff_t>(span.openItem + 1), openRun.items.end());
        openRun.items.erase(openRun.items.begin() + static_cast<std::ptrdiff_t>(span.openItem + 1), openRun.items.end());
        model::Run body;
        body.charShapeId = hidden.shape;
        body.items.push_back(model::InlineItem::makeText(hidden.text));
        auto where = para.runs.insert(para.runs.begin() + static_cast<std::ptrdiff_t>(span.openRun + 1), std::move(body));
        if (!tail.items.empty()) para.runs.insert(where + 1, std::move(tail));
        std::erase_if(para.runs, [](const model::Run& run) { return run.items.empty(); });
        return true;
    }
    return false;
}

// 캐럿이 손대지 않은 누름틀 안이고 안내문이 보이면 숨긴다.
bool hideGuideAtCaret(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                      EditRelayoutResult& relayout) {
    PointerHold& hold = pointerHold();
    int story = 0, paraIdx = 0, at = 0;
    if (hold.composing || hold.hiddenGuide.story >= 0 || !caretChar(doc, pages, story, paraIdx, at)) return false;
    model::Paragraph* para = storyParagraph(doc, story, paraIdx);
    if (!para) return false;
    std::vector<ClickHereSpan> spans = clickHereSpans(*para);
    const ClickHereSpan* span = untouchedAt(spans, hold, story, paraIdx, at);
    if (!span || span->endPos <= span->openPos) return false;
    HiddenClickGuide hidden;
    hidden.story = story;
    hidden.para = paraIdx;
    hidden.openPos = span->openPos;
    hidden.chars = span->endPos - span->openPos;
    hidden.useStoredLineHeight = useStoredLineHeight;
    hidden.shape = span->openShape;
    bool other = false;
    for (std::size_t rr = span->openRun; rr <= span->endRun; ++rr) {
        const auto& items = para->runs[rr].items;
        const std::size_t from = rr == span->openRun ? span->openItem + 1 : 0;
        const std::size_t to = rr == span->endRun ? span->endItem : items.size();
        for (std::size_t k = from; k < to; ++k) {
            if (items[k].type != model::InlineItem::Type::Text) {
                other = true;
                continue;
            }
            if (items[k].text.empty()) continue;
            if (hidden.text.empty()) hidden.shape = para->runs[rr].charShapeId;
            hidden.text += items[k].text;
        }
    }
    // 안내문 말고 다른 것(개체 등)이 들었으면 숨기지 않는다.
    if (other || static_cast<int>(utf8CodePointCount(hidden.text)) != hidden.chars) return false;
    hidden.before = *para;
    const ClickHereSpan kept = *span;
    eraseBody(*para, kept);
    // 안 자리는 빈 글 조각으로 둔다(친 글을 다 지운 빈 누름틀과 같은 꼴).
    auto& openItems = para->runs[kept.openRun].items;
    openItems.insert(openItems.begin() + static_cast<std::ptrdiff_t>(kept.openItem + 1), model::InlineItem::makeText(""));
    std::erase_if(para->runs, [](const model::Run& run) { return run.items.empty(); });
    para->lineSegs.clear();
    hidden.hidden = *para;
    const int openPos = hidden.openPos;
    const int endPos = openPos + hidden.chars;
    hold.hiddenGuide = std::move(hidden);
    relayout = relayoutAfterEdit(doc, pages,
                                 EditRelayoutSpec{story, paraIdx, 0, openPos, endPos, openPos, useStoredLineHeight});
    placeAt(doc, pages, story, paraIdx, openPos);
    setSlot(hold, story, paraIdx, openPos);
    return true;
}

}  // namespace

model::Paragraph shownClickHereParagraph(const model::Paragraph& para, const HiddenClickGuide& hidden) {
    if (hidden.story < 0 || !hidden.before || !hidden.hidden) return para;
    if (paragraphsAlike(para, *hidden.hidden)) return *hidden.before;
    model::Paragraph shown = para;
    if (putGuideBack(shown, hidden)) shown.lineSegs.clear();
    return shown;
}

bool revealClickHereGuide(model::Document& doc, std::vector<Node>& pages, EditRelayoutResult& relayout) {
    PointerHold& hold = pointerHold();
    if (hold.hiddenGuide.story < 0) return false;
    const HiddenClickGuide hidden = std::move(hold.hiddenGuide);
    hold.hiddenGuide = HiddenClickGuide{};
    model::Paragraph* para = storyParagraph(doc, hidden.story, hidden.para);
    if (!para) return false;
    // 캐럿의 논리 자리: 숨긴 누름틀 안, 그 문단의 다른 자리(누름틀 뒤면 안내문 글자만큼 민다), 다른 문단.
    int story = 0, paraIdx = 0, at = 0;
    const bool caret = caretChar(doc, pages, story, paraIdx, at);
    const bool samePara = caret && story == hidden.story && paraIdx == hidden.para;
    const bool inside = samePara && at == hidden.openPos && slotHere(hold, story, paraIdx, at);
    model::Paragraph shown = shownClickHereParagraph(*para, hidden);
    if (paragraphsAlike(shown, *para)) return false;
    *para = std::move(shown);
    relayout = relayoutAfterEdit(doc, pages,
                                 EditRelayoutSpec{hidden.story, hidden.para, 0, hidden.openPos, hidden.openPos,
                                                  hidden.openPos + hidden.chars, hidden.useStoredLineHeight});
    if (samePara) {
        placeAt(doc, pages, story, paraIdx, at > hidden.openPos ? at + hidden.chars : at);
        if (inside) setSlot(hold, story, paraIdx, hidden.openPos);
    } else if (caret) {
        // 쪽 위 자리가 바뀌었을 수 있다. 같은 논리 자리에 다시 놓는다.
        placeAt(doc, pages, story, paraIdx, at);
    }
    return true;
}

bool clickHereGuideStale(const model::Document& doc, const std::vector<Node>& pages) {
    const PointerHold& hold = pointerHold();
    int story = 0, paraIdx = 0, at = 0;
    const bool caret = !hold.composing && caretChar(doc, pages, story, paraIdx, at);
    const HiddenClickGuide& hidden = hold.hiddenGuide;
    if (hidden.story >= 0) {
        // 캐럿이 아직 숨긴 누름틀 안이면 그대로 둔다.
        return !(caret && story == hidden.story && paraIdx == hidden.para && at == hidden.openPos &&
                 slotHere(hold, story, paraIdx, at));
    }
    if (!caret) return false;
    model::Paragraph* para = storyParagraph(const_cast<model::Document&>(doc), story, paraIdx);
    if (!para) return false;
    std::vector<ClickHereSpan> spans = clickHereSpans(*para);
    const ClickHereSpan* span = untouchedAt(spans, hold, story, paraIdx, at);
    return span && span->endPos > span->openPos;
}

bool syncClickHereGuide(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                        EditRelayoutResult& relayout) {
    if (!clickHereGuideStale(doc, pages)) return false;
    EditRelayoutResult shown;
    const bool revealed = revealClickHereGuide(doc, pages, shown);
    EditRelayoutResult hiddenNow;
    const bool hid = hideGuideAtCaret(doc, pages, useStoredLineHeight, hiddenNow);
    if (revealed && hid) {
        // 두 번 다시 놓았다. 화면은 통째로 다시 본다.
        relayout = hiddenNow;
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
    } else {
        relayout = hid ? hiddenNow : shown;
    }
    return revealed || hid;
}

namespace {

// 캐럿(블록이면 그 시작)이 든 누름틀. at 이 시작~끝 사이면 그 누름틀이다.
bool clickHereAround(model::Document& doc, const std::vector<Node>& pages, int& story, int& paraIdx,
                     std::vector<ClickHereSpan>& spans, ClickHereSpan*& found) {
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return false;
    story = cur.story;
    paraIdx = cur.hasSelection ? cur.startParagraph : cur.paragraph;
    const int at = offsetToCharIndex(doc, pages, story, paraIdx, cur.hasSelection ? cur.startOffset : cur.offset);
    model::Paragraph* para = storyParagraph(doc, story, paraIdx);
    if (!para) return false;
    spans = clickHereSpans(*para);
    for (ClickHereSpan& span : spans) {
        if (at >= span.openPos && at <= span.endPos) {
            found = &span;
            return true;
        }
    }
    return false;
}

std::string textField(const FormatFields& fields, const char* key, const std::string& fallback) {
    for (const auto& [name, value] : fields) {
        if (name != key) continue;
        if (const std::string* text = std::get_if<std::string>(&value)) return *text;
    }
    return fallback;
}

bool boolField(const FormatFields& fields, const char* key, bool fallback) {
    for (const auto& [name, value] : fields) {
        if (name != key) continue;
        if (const bool* flag = std::get_if<bool>(&value)) return *flag;
        if (const double* number = std::get_if<double>(&value)) return *number != 0;
    }
    return fallback;
}

}  // namespace

// 누름틀 고치기(한글 "필드 입력 고치기" 대화 상자의 누름틀 탭): 입력할 내용의 안내문, 메모 내용, 필드 이름,
// 양식 모드에서 편집 가능.
FormatFields clickHereView(model::Document& doc, const std::vector<Node>& pages) {
    FormatFields out;
    int story = 0, paraIdx = 0;
    std::vector<ClickHereSpan> spans;
    ClickHereSpan* span = nullptr;
    if (!clickHereAround(doc, pages, story, paraIdx, spans, span)) {
        out.emplace_back("found", false);
        return out;
    }
    const model::Control& field = *span->begin;
    out.emplace_back("found", true);
    out.emplace_back("direction", clickHereGuide(field));
    out.emplace_back("memo", clickHereMemo(field));
    out.emplace_back("name", field.name);
    out.emplace_back("editable", (field.fieldProps & 1u) != 0);
    return out;
}

EditResult modifyClickHereNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool useStoredLineHeight, std::string& error) {
    if (readOnly()) return {};
    int story = 0, paraIdx = 0;
    std::vector<ClickHereSpan> spans;
    ClickHereSpan* span = nullptr;
    if (!clickHereAround(doc, pages, story, paraIdx, spans, span)) {
        error = "캐럿이 누름틀 안에 있지 않습니다.";
        return {};
    }
    model::Control& field = *span->begin;
    const std::string oldGuide = clickHereGuide(field);
    std::string guide = textField(fields, "direction", oldGuide);
    if (guide.empty()) guide = kDefaultGuide;
    const std::string memo = textField(fields, "memo", clickHereMemo(field));
    field.name = textField(fields, "name", field.name);
    field.command = clickHereCommand(guide, memo);
    field.fieldStrings.erase(std::remove_if(field.fieldStrings.begin(), field.fieldStrings.end(),
                                            [](const auto& kv) { return kv.first == "Direction" || kv.first == "HelpState"; }),
                             field.fieldStrings.end());
    field.fieldStrings.emplace_back("Direction", guide);
    if (!memo.empty()) field.fieldStrings.emplace_back("HelpState", memo);
    if (boolField(fields, "editable", (field.fieldProps & 1u) != 0)) field.fieldProps |= 1u;
    else field.fieldProps &= ~1u;
    // 손대지 않은 누름틀은 보이는 안내문도 새 안내문이다.
    model::Paragraph* para = storyParagraph(doc, story, paraIdx);
    if (span->untouched() && guide != oldGuide && para) {
        bool replaced = false;
        for (std::size_t rr = span->openRun; rr <= span->endRun && para; ++rr) {
            auto& items = para->runs[rr].items;
            const std::size_t from = rr == span->openRun ? span->openItem + 1 : 0;
            const std::size_t to = rr == span->endRun ? span->endItem : items.size();
            for (std::size_t k = to; k-- > from;) {
                if (items[k].type != model::InlineItem::Type::Text) continue;
                if (!replaced) {
                    items[k].text = guide;
                    replaced = true;
                } else {
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(k));
                }
            }
        }
        relayoutWhole(doc, pages, *para, useStoredLineHeight);
        EditRelayoutResult relayout;
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
        placeAt(doc, pages, story, paraIdx, span->openPos);
        setSlot(pointerHold(), story, paraIdx, span->openPos);
        return makeEditResult(doc, true, true, story, paraIdx, &relayout);
    }
    return makeEditResult(doc, true, true, story, paraIdx);
}

bool clickHereGuideNeedsSync(const model::Document& doc, const std::vector<Node>& pages) {
    return clickHereGuideStale(doc, pages);
}

EditResult editSyncClickHereGuide(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    EditRelayoutResult relayout;
    if (!syncClickHereGuide(doc, pages, useStoredLineHeight, relayout)) return {};
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

bool clickHereGuideHidden() { return pointerHold().hiddenGuide.story >= 0; }

EditResult editRevealClickHereGuide(model::Document& doc, std::vector<Node>& pages) {
    EditRelayoutResult relayout;
    if (!revealClickHereGuide(doc, pages, relayout)) return {};
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

}  // namespace chwp::render
