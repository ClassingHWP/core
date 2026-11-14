// 쪽 탭 명령: 머리말·꼬리말 만들기·지우기, 쪽 번호 매기기, 새 번호로 시작, 현재 쪽만 감추기.
// 어느 문단의 어느 자리에 컨트롤을 두는지는 한글 2024 에서 같은 명령을 불러 확인한 대로다.
// - 머리말·꼬리말: 캐럿 문단(표 칸이면 그 칸 문단)의 맨 앞에 늘 새로 넣는다. 같은 쪽에 이미 있어도 새로 넣는다.
//   글 묶음 문단은 "머리말" 스타일이고, 그 쪽 머리말·꼬리말 편집으로 들어간다.
// - 쪽 번호: 캐럿이 어디 있든 구역 첫 문단 맨 앞이다. 이미 있으면 그것을 고친다. "NONE" 도 컨트롤로 남는다.
// - 새 번호로 시작: 캐럿 자리에 늘 새로 넣는다.
// - 현재 쪽만 감추기: 캐럿 문단의 것을 고치고, 없으면 캐럿 자리에 넣는다. 모두 끄면 지운다.
// 되돌리기는 구역째 베껴 한 칸으로 둔다.

#include "chwp/model/block_calc.hpp"
#include "chwp/model/memo.hpp"
#include "chwp/model/proofreading.hpp"
#include "chwp/model/table_formula.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace chwp::render {

using namespace paint_detail;

namespace {

std::string textField(const FormatFields& fields, const char* key, const std::string& fallback) {
    for (const auto& [name, value] : fields) {
        if (name != key) continue;
        if (const std::string* text = std::get_if<std::string>(&value)) return *text;
    }
    return fallback;
}

double numberField(const FormatFields& fields, const char* key, double fallback) {
    for (const auto& [name, value] : fields) {
        if (name != key) continue;
        if (const double* number = std::get_if<double>(&value)) return *number;
        if (const bool* flag = std::get_if<bool>(&value)) return *flag ? 1 : 0;
    }
    return fallback;
}

bool flagField(const FormatFields& fields, const char* key, bool fallback) {
    return numberField(fields, key, fallback ? 1 : 0) != 0;
}

bool isControl(const model::InlineItem& item, model::ControlKind kind) {
    return item.type == model::InlineItem::Type::Control && item.control.kind == kind;
}

// 글자 칸을 차지하지 않는 컨트롤을 문단 글자 charIndex 자리에 끼운다. 그 자리의 글 조각을 갈라 그 사이에 둔다.
// 맨 앞(0)이면 구역 정의·단 정의 뒤, 첫 글 앞이다.
model::Control* insertControlAt(model::Paragraph& para, int charIndex, model::Control control) {
    int pos = 0;
    for (model::Run& run : para.runs) {
        for (std::size_t i = 0; i < run.items.size(); ++i) {
            model::InlineItem& item = run.items[i];
            int len = 0;
            if (item.type == model::InlineItem::Type::Text) {
                len = utf8CodePointCount(item.text);
            } else if (isCaretCharControl(item.control)) {
                len = 1;
            } else {
                continue;
            }
            if (charIndex < pos || charIndex > pos + len || (charIndex == pos + len && len > 0 &&
                                                              item.type != model::InlineItem::Type::Text)) {
                pos += len;
                continue;
            }
            const int local = charIndex - pos;
            std::size_t at = i;
            if (item.type == model::InlineItem::Type::Text && local > 0) {
                if (local < len) {
                    model::InlineItem tail = model::InlineItem::makeText(utf8SliceCodePoints(item.text, local, len - local));
                    item.text = utf8SliceCodePoints(item.text, 0, local);
                    run.items.insert(run.items.begin() + static_cast<std::ptrdiff_t>(i + 1), std::move(tail));
                }
                at = i + 1;
            }
            run.items.insert(run.items.begin() + static_cast<std::ptrdiff_t>(at),
                             model::InlineItem::makeControl(std::move(control)));
            para.lineSegs.clear();
            return &run.items[at].control;
        }
    }
    if (para.runs.empty()) {
        model::Run run;
        para.runs.push_back(std::move(run));
    }
    model::Run& last = para.runs.back();
    last.items.push_back(model::InlineItem::makeControl(std::move(control)));
    para.lineSegs.clear();
    return &last.items.back().control;
}

// 문단에서 그 컨트롤 하나를 뺀다.
bool eraseControl(model::Paragraph& para, const model::Control* control) {
    for (model::Run& run : para.runs) {
        for (auto it = run.items.begin(); it != run.items.end(); ++it) {
            if (it->type == model::InlineItem::Type::Control && &it->control == control) {
                run.items.erase(it);
                para.lineSegs.clear();
                return true;
            }
        }
    }
    return false;
}

// 문단의 쪽 새 번호(번호 종류 PAGE).
model::Control* pageNewNumberIn(model::Paragraph& para) {
    for (model::Run& run : para.runs) {
        for (model::InlineItem& item : run.items) {
            if (isControl(item, model::ControlKind::NewNumber) &&
                (item.control.numType.empty() || item.control.numType == "PAGE")) {
                return &item.control;
            }
        }
    }
    return nullptr;
}

model::Control* findControl(model::Paragraph& para, model::ControlKind kind) {
    for (model::Run& run : para.runs) {
        for (model::InlineItem& item : run.items) {
            if (isControl(item, kind)) return &item.control;
        }
    }
    return nullptr;
}

const model::Control* findControl(const model::Paragraph& para, model::ControlKind kind) {
    return findControl(const_cast<model::Paragraph&>(para), kind);
}

// 머리말·꼬리말 글 묶음 문단의 스타일. 한글은 머리말·꼬리말 모두 "머리말"(Header) 스타일이다.
const model::Style* headerStyle(const model::Document& doc) {
    for (const model::Style& style : doc.header.styles) {
        if (style.engName == "Header") return &style;
    }
    for (const model::Style& style : doc.header.styles) {
        if (style.name == "머리말") return &style;
    }
    return nullptr;
}

}  // namespace

HeldCaret paint_detail::holdCaret(model::Document& doc, const std::vector<Node>& pages) {
    HeldCaret held;
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return held;
    model::ensureParagraphIds(doc);
    const model::Paragraph* anchor = storyParagraph(doc, cur.story, cur.anchorParagraph);
    const model::Paragraph* focus = storyParagraph(doc, cur.story, cur.paragraph);
    if (!anchor || !focus) return held;
    held.active = true;
    held.anchorId = anchor->id;
    held.focusId = focus->id;
    held.anchorChar = offsetToCharIndex(doc, pages, cur.story, cur.anchorParagraph, cur.anchorOffset);
    held.focusChar = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    return held;
}

void paint_detail::placeHeldCaret(const model::Document& doc, const std::vector<Node>& pages, const HeldCaret& held) {
    if (!held.active) return;
    const model::StoryMap& map = ensureStoryMap(doc);
    const model::StoryMap::ParagraphAt anchor = map.findParagraph(held.anchorId);
    const model::StoryMap::ParagraphAt focus = map.findParagraph(held.focusId);
    if (focus.story <= 0) return;
    const bool sameStory = anchor.story == focus.story;
    const int anchorPara = sameStory ? anchor.paragraph : focus.paragraph;
    const int anchorChar = sameStory ? held.anchorChar : held.focusChar;
    setPointerCursor(doc, pages,
                     model::CursorPos{focus.story, anchorPara,
                                      charIndexToOffset(doc, pages, focus.story, anchorPara, anchorChar)},
                     model::CursorPos{focus.story, focus.paragraph,
                                      charIndexToOffset(doc, pages, focus.story, focus.paragraph, held.focusChar)});
}

EditRelayoutResult paint_detail::relayoutWhole(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.incremental = false;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    return relayout;
}

namespace {

// 그 쪽에서 control 의 글 묶음을 보이는 띠. 띠는 종이의 바로 아래 자식이고 쪽마다 별칭 글 묶음이다.
int bandStoryOn(const model::Document& doc, const Node& paper, const model::Control* control) {
    const model::StoryMap& map = ensureStoryMap(doc);
    for (const Node& child : paper.children) {
        if (!child.pageBand) continue;
        std::vector<const Node*> stack{&child};
        while (!stack.empty()) {
            const Node* node = stack.back();
            stack.pop_back();
            if (node->story > 0 && (node->kind == NodeKind::Paragraph || node->kind == NodeKind::Line)) {
                const model::StorySlot* slot = map.slot(node->story);
                if (slot && slot->control == control) return node->story;
                break;
            }
            for (const Node& sub : node->children) stack.push_back(&sub);
        }
    }
    return 0;
}

// 머리말·꼬리말 편집으로 들어간다. 캐럿 쪽에서 먼저 찾고, 그 쪽에 없으면(감춘 쪽 등) 처음 보이는 쪽이다.
bool enterBand(const model::Document& doc, const std::vector<Node>& pages, const model::Control* control,
               int preferPage, bool header) {
    PointerHold& hold = pointerHold();
    int story = 0;
    if (preferPage >= 0 && preferPage < static_cast<int>(pages.size())) {
        story = bandStoryOn(doc, pages[static_cast<std::size_t>(preferPage)], control);
    }
    for (std::size_t i = 0; story <= 0 && i < pages.size(); ++i) story = bandStoryOn(doc, pages[i], control);
    if (story <= 0) return false;
    const PointerCursor before = pointerCursor();
    if (hold.bandStory <= 0 && before.active && before.story > 0) {
        hold.bandReturnStory = before.story;
        hold.bandReturnPara = before.paragraph;
        hold.bandReturnOff = before.offset;
    }
    hold.editStory = story;
    hold.bandStory = story;
    hold.bandHeader = header;
    setPointerCursor(doc, pages, model::CursorPos{story, 0, 0});
    hold.eventText = header ? "머리말" : "꼬리말";
    return true;
}

void leaveBand(const model::Document& doc, const std::vector<Node>& pages) {
    PointerHold& hold = pointerHold();
    if (hold.editStory == hold.bandStory) hold.editStory = 0;
    hold.bandStory = 0;
    const model::CursorPos back{hold.bandReturnStory > 0 ? hold.bandReturnStory : 1,
                                hold.bandReturnStory > 0 ? hold.bandReturnPara : 0,
                                hold.bandReturnStory > 0 ? hold.bandReturnOff : 0};
    setPointerCursor(doc, pages, back);
}

const char* kBandBusy = "머리말·꼬리말을 고치는 중에는 할 수 없습니다.";

// 캐럿 문단. 머리말·꼬리말 편집 중이면 없다.
model::Paragraph* caretParagraph(model::Document& doc, std::string& why) {
    const PointerCursor cur = pointerCursor();
    if (pointerHold().bandStory > 0) {
        why = pointerHold().bandMaster ? "바탕쪽을 고치는 중에는 할 수 없습니다." : kBandBusy;
        return nullptr;
    }
    if (!cur.active || cur.story <= 0) {
        why = "캐럿이 없습니다.";
        return nullptr;
    }
    return storyParagraph(doc, cur.story, cur.paragraph);
}

EditResult makeHeaderFooter(model::Document& doc, std::vector<Node>& pages, bool header, const FormatFields& fields,
                            bool useStoredLineHeight, std::string& why) {
    model::Paragraph* host = caretParagraph(doc, why);
    if (!host) return {};
    const int section = cursorSection(doc);
    if (section < 0) return {};
    std::string apply = textField(fields, "apply", "BOTH");
    if (apply != "ODD" && apply != "EVEN") apply = "BOTH";
    const model::PageDef page = doc.sections[static_cast<std::size_t>(section)].page.value_or(model::PageDef{});

    model::Control control;
    control.kind = header ? model::ControlKind::Header : model::ControlKind::Footer;
    control.ctrlId = header ? "head" : "foot";
    control.applyPageType = apply;
    model::SubList sub;
    sub.vertAlign = header ? "TOP" : "BOTTOM";
    sub.textWidth = static_cast<std::uint32_t>(
        std::max<std::int64_t>(0, static_cast<std::int64_t>(page.width) - page.marginLeft - page.marginRight));
    sub.textHeight = header ? page.marginHeader : page.marginFooter;
    model::Paragraph para;
    model::Run run;
    if (const model::Style* style = headerStyle(doc)) {
        para.styleId = style->id;
        para.paraShapeId = style->paraShapeId;
        run.charShapeId = style->charShapeId;
    } else {
        para.styleId = host->styleId;
        para.paraShapeId = host->paraShapeId;
        run.charShapeId = charShapeAtParagraphChar(*host, 0, false);
    }
    para.runs.push_back(std::move(run));
    sub.paragraphs.push_back(std::move(para));
    control.subLists.push_back(std::move(sub));

    PointerHold& hold = pointerHold();
    clearComposition(hold);
    const int caretPage = hold.caretPage;
    const HeldCaret held = holdCaret(doc, pages);
    const model::Control* placed = insertControlAt(*host, 0, std::move(control));
    hold.preferredX = -1;
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    enterBand(doc, pages, placed, caretPage, header);
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

EditResult deleteBand(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight, std::string& why) {
    PointerHold& hold = pointerHold();
    if (hold.bandStory <= 0) {
        why = "머리말·꼬리말을 고치는 중이 아닙니다.";
        return {};
    }
    const model::StorySlot* slot = resolveStory(doc, hold.bandStory);
    if (!slot || !slot->control || slot->parentStory <= 0) return {};
    model::Paragraph* host = storyParagraph(doc, slot->parentStory, slot->parentParagraph);
    if (!host) return {};
    clearComposition(hold);
    // 나갈 본문 자리를 문단 고유 번호로 들고 있는다. 머리말을 빼면 글 묶음 번호가 바뀐다.
    model::ensureParagraphIds(doc);
    HeldCaret back;
    if (const model::Paragraph* ret = storyParagraph(doc, hold.bandReturnStory, hold.bandReturnPara)) {
        back.active = true;
        back.anchorId = back.focusId = ret->id;
        back.anchorChar = back.focusChar =
            offsetToCharIndex(doc, pages, hold.bandReturnStory, hold.bandReturnPara, hold.bandReturnOff);
    }
    if (!eraseControl(*host, slot->control)) return {};
    if (hold.editStory == hold.bandStory) hold.editStory = 0;
    hold.bandStory = 0;
    hold.preferredX = -1;
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    if (back.active) {
        placeHeldCaret(doc, pages, back);
    } else {
        setPointerCursor(doc, pages, model::CursorPos{1, 0, 0});
    }
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

EditResult setPageNumber(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                         bool useStoredLineHeight, std::string& why) {
    if (pointerHold().bandStory > 0) {
        why = kBandBusy;
        return {};
    }
    const int section = cursorSection(doc);
    if (section < 0) return {};
    std::vector<model::Paragraph>& paras = doc.sections[static_cast<std::size_t>(section)].paragraphs;
    if (paras.empty()) return {};
    model::Paragraph& first = paras.front();
    const HeldCaret held = holdCaret(doc, pages);
    model::Control* control = findControl(first, model::ControlKind::PageNumPos);
    if (!control) {
        model::Control fresh;
        fresh.kind = model::ControlKind::PageNumPos;
        fresh.ctrlId = "pgnp";
        fresh.pageNumPos = "BOTTOM_CENTER";
        fresh.numFormat = "DIGIT";
        fresh.prefix = fresh.suffix = "-";
        control = insertControlAt(first, 0, std::move(fresh));
    }
    control->pageNumPos = textField(fields, "pos", control->pageNumPos.empty() ? "BOTTOM_CENTER" : control->pageNumPos);
    control->numFormat = textField(fields, "format", control->numFormat.empty() ? "DIGIT" : control->numFormat);
    const std::string side = textField(fields, "sideChar", control->prefix);
    control->prefix = control->suffix = side;
    // 시작 번호(한글 2024 APageNumPos): 그 자리에 쪽 새 번호가 있으면 번호만 고치고,
    // 없으면 1 이 아닐 때만 새로 넣는다.
    if (const double start = numberField(fields, "start", 0); start >= 1) {
        const auto number = static_cast<std::uint32_t>(std::clamp(std::lround(start), 1L, 65535L));
        if (model::Control* again = pageNewNumberIn(first)) {
            again->number = number;
        } else if (number != 1) {
            model::Control fresh;
            fresh.kind = model::ControlKind::NewNumber;
            fresh.ctrlId = "nwno";
            fresh.numType = "PAGE";
            fresh.number = number;
            insertControlAt(first, 0, std::move(fresh));
        }
    }
    first.lineSegs.clear();
    clearComposition(pointerHold());
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

// 쪽 번호 넣기(한글 2024 InsertPageNum): 캐럿 자리에 쪽 자동 번호. 번호 모양은
// 쪽 번호 매기기와 상관없이 아라비아 숫자다(본문·머리말 모두 한글에서 확인).
EditResult insertPageNumber(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                            std::string& why) {
    const PointerCursor cur = pointerCursor();
    model::Paragraph* host = cur.active && cur.story > 0 ? storyParagraph(doc, cur.story, cur.paragraph) : nullptr;
    if (!host) {
        why = "캐럿이 없습니다.";
        return {};
    }
    model::Control control;
    control.kind = model::ControlKind::AutoNumber;
    control.ctrlId = "atno";
    control.numType = "PAGE";
    control.numFormat = "DIGIT";
    const HeldCaret held = holdCaret(doc, pages);
    clearComposition(pointerHold());
    insertControlAt(*host, held.focusChar, std::move(control));
    host->lineSegs.clear();
    pointerHold().preferredX = -1;
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

// 문서 차례의 머리말·꼬리말 컨트롤. 표 칸·글상자 안 것도 그 자리 차례로 넣는다.
void collectBands(const std::vector<model::Paragraph>& paras, std::vector<const model::Control*>& out) {
    for (const model::Paragraph& para : paras) {
        for (const model::Run& run : para.runs) {
            for (const model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Control) continue;
                const model::Control& control = item.control;
                if (control.kind == model::ControlKind::Header || control.kind == model::ControlKind::Footer) {
                    out.push_back(&control);
                    continue;
                }
                if (control.table) {
                    for (const model::TableCell& cell : control.table->cells) collectBands(cell.content.paragraphs, out);
                }
                for (const model::SubList& list : control.subLists) collectBands(list.paragraphs, out);
            }
        }
    }
}

// 이전·다음 머리말/꼬리말(한글 2024 HeaderFooterToPrev·Next):
// 캐럿을 감싼 머리말·꼬리말에서 컨트롤 사슬을 앞뒤로 따라가 처음 만나는 머리말이나 꼬리말 편집으로 간다.
// 지금 편집을 닫고(Close) 그 컨트롤이 처음 보이는 쪽에서 들어간다.
EditResult stepBand(model::Document& doc, std::vector<Node>& pages, bool next, std::string& why) {
    PointerHold& hold = pointerHold();
    if (hold.bandStory <= 0) {
        why = "머리말·꼬리말을 고치는 중이 아닙니다.";
        return {};
    }
    const model::StorySlot* slot = resolveStory(doc, hold.bandStory);
    if (!slot || !slot->control) return {};
    std::vector<const model::Control*> bands;
    for (const model::Section& section : doc.sections) collectBands(section.paragraphs, bands);
    const auto at = std::find(bands.begin(), bands.end(), slot->control);
    if (at == bands.end()) return {};
    if (next ? at + 1 == bands.end() : at == bands.begin()) {
        why = next ? "다음 머리말·꼬리말이 없습니다." : "앞 머리말·꼬리말이 없습니다.";
        return {};
    }
    const model::Control* target = next ? *(at + 1) : *(at - 1);
    clearComposition(hold);
    hold.preferredX = -1;
    if (!enterBand(doc, pages, target, -1, target->kind == model::ControlKind::Header)) {
        why = "그 머리말·꼬리말이 보이는 쪽이 없습니다.";
        return {};
    }
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, false, cur.story, cur.paragraph);
}

// 그 쪽 띠 가운데 band(머리말·꼬리말)·apply 가 맞는 컨트롤.
const model::Control* bandControlOn(const model::Document& doc, const Node& paper, bool header, const std::string& apply) {
    const model::StoryMap& map = ensureStoryMap(doc);
    for (const Node& child : paper.children) {
        if (!child.pageBand) continue;
        std::vector<const Node*> stack{&child};
        while (!stack.empty()) {
            const Node* node = stack.back();
            stack.pop_back();
            if (node->story > 0 && (node->kind == NodeKind::Paragraph || node->kind == NodeKind::Line)) {
                const model::StorySlot* slot = map.slot(node->story);
                const model::Control* c = slot ? slot->control : nullptr;
                if (c && c->kind == (header ? model::ControlKind::Header : model::ControlKind::Footer) &&
                    (c->applyPageType.empty() ? std::string("BOTH") : c->applyPageType) == apply) {
                    return c;
                }
                break;
            }
            for (const Node& sub : node->children) stack.push_back(&sub);
        }
    }
    return nullptr;
}

// 캐럿 쪽부터 앞쪽으로, 그다음 뒤쪽으로 처음 보이는 그 머리말·꼬리말과 그 쪽.
const model::Control* nearestBand(const model::Document& doc, const std::vector<Node>& pages, bool header,
                                  const std::string& apply, int& page) {
    const int count = static_cast<int>(pages.size());
    const int from = std::clamp(pointerHold().caretPage, 0, std::max(0, count - 1));
    for (int i = from; i >= 0 && i < count; --i) {
        if (const model::Control* c = bandControlOn(doc, pages[static_cast<std::size_t>(i)], header, apply)) {
            page = i;
            return c;
        }
    }
    for (int i = from + 1; i < count; ++i) {
        if (const model::Control* c = bandControlOn(doc, pages[static_cast<std::size_t>(i)], header, apply)) {
            page = i;
            return c;
        }
    }
    return nullptr;
}

std::string bandApply(const FormatFields& fields) {
    std::string apply = textField(fields, "apply", "BOTH");
    return apply == "ODD" || apply == "EVEN" ? apply : std::string("BOTH");
}

// 머리말/꼬리말 대화 상자의 편집.
EditResult editBand(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, std::string& why) {
    if (!caretParagraph(doc, why)) return {};
    const bool header = textField(fields, "band", "header") != "footer";
    const std::string apply = bandApply(fields);
    int page = -1;
    const model::Control* target = nearestBand(doc, pages, header, apply, page);
    if (!target) {
        // 한글 2024 의 알림 글이다(대화 상자는 이어서 새로 만들지 묻는다).
        why = "지정한 모양의 머리말/꼬리말이 없습니다.";
        return {};
    }
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    hold.preferredX = -1;
    if (!enterBand(doc, pages, target, page, header)) {
        why = "그 머리말·꼬리말이 보이는 쪽이 없습니다.";
        return {};
    }
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, false, cur.story, cur.paragraph);
}

EditResult insertNewNumber(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                           bool useStoredLineHeight, std::string& why) {
    model::Paragraph* host = caretParagraph(doc, why);
    if (!host) return {};
    const PointerCursor cur = pointerCursor();
    model::Control control;
    control.kind = model::ControlKind::NewNumber;
    control.ctrlId = "nwno";
    control.numType = textField(fields, "numType", "PAGE");
    control.number = static_cast<std::uint32_t>(std::clamp(std::lround(numberField(fields, "number", 1)), 1L, 65535L));
    const HeldCaret held = holdCaret(doc, pages);
    clearComposition(pointerHold());
    insertControlAt(*host, held.focusChar, std::move(control));
    pointerHold().preferredX = -1;
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

EditResult setPageHiding(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                         bool useStoredLineHeight, std::string& why) {
    model::Paragraph* host = caretParagraph(doc, why);
    if (!host) return {};
    model::Control* control = findControl(*host, model::ControlKind::PageHiding);
    model::Control next;
    if (control) next = *control;
    next.kind = model::ControlKind::PageHiding;
    next.ctrlId = "pghd";
    next.hideHeader = flagField(fields, "hideHeader", next.hideHeader);
    next.hideFooter = flagField(fields, "hideFooter", next.hideFooter);
    next.hideMaster = flagField(fields, "hideMaster", next.hideMaster);
    next.hideBorder = flagField(fields, "hideBorder", next.hideBorder);
    next.hideFill = flagField(fields, "hideFill", next.hideFill);
    next.hidePageNum = flagField(fields, "hidePageNum", next.hidePageNum);
    const bool any =
        next.hideHeader || next.hideFooter || next.hideMaster || next.hideBorder || next.hideFill || next.hidePageNum;
    if (!control && !any) return makeEditResult(doc, true, false, pointerCursor().story, pointerCursor().paragraph);
    const HeldCaret held = holdCaret(doc, pages);
    clearComposition(pointerHold());
    if (!any) {
        eraseControl(*host, control);
    } else if (control) {
        *control = std::move(next);
        host->lineSegs.clear();
    } else {
        insertControlAt(*host, held.focusChar, std::move(next));
    }
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

// ---- 바탕쪽 편집 ----
// 한글 2024 의 바탕쪽(MasterPageDlg·바탕쪽 도구 상자). 바탕쪽 글은 쪽마다 같은 글 묶음으로 찍히니(scene attachMaster)
// 머리말처럼 그 글 묶음에 캐럿을 둔다(bandStory, bandMaster). 편집 중에는 바탕쪽만 그린다(masterView).

const char* kMasterTypes[] = {"BOTH", "EVEN", "ODD", "LAST_PAGE", "OPTIONAL_PAGE"};

// 한글이 저장하는 차례: 양쪽, 짝수, 홀수, 마지막 쪽, 임의 쪽(쪽 차례).
int masterRank(const model::MasterPage& mp) {
    for (int i = 0; i < 5; ++i) {
        if (mp.applyPageType == kMasterTypes[i]) return i * 100000 + (i == 4 ? static_cast<int>(mp.pageNumber) : 0);
    }
    return 0;
}

bool sameMasterKind(const model::MasterPage& mp, const std::string& type, std::uint32_t pageNumber) {
    const std::string have = mp.applyPageType.empty() ? std::string("BOTH") : mp.applyPageType;
    return have == type && (type != "OPTIONAL_PAGE" || mp.pageNumber == pageNumber);
}

// 그 쪽에 놓인 바탕쪽 글의 글 묶음. 바탕쪽은 쪽마다 별칭 글 묶음으로 놓인다(머리말 띠와 같다).
int masterStoryOn(const model::Document& doc, const Node& paper, const model::MasterPage* master) {
    const model::StoryMap& map = ensureStoryMap(doc);
    for (const Node& child : paper.children) {
        if (!child.fromMaster) continue;
        std::vector<const Node*> stack{&child};
        while (!stack.empty()) {
            const Node* node = stack.back();
            stack.pop_back();
            if (node->story > 0 && (node->kind == NodeKind::Paragraph || node->kind == NodeKind::Line)) {
                const model::StorySlot* slot = map.slot(node->story);
                if (slot && slot->kind == model::StoryKind::MasterPage && slot->masterPage == master) return node->story;
            }
            for (const Node& sub : node->children) stack.push_back(&sub);
        }
    }
    return 0;
}

// 바탕쪽 편집으로 들어간다. 그 바탕쪽이 놓인 쪽(preferPage 먼저, 없으면 앞쪽부터)의 글 묶음에 캐럿을 둔다.
bool enterMaster(const model::Document& doc, const std::vector<Node>& pages, const model::MasterPage* master,
                 int preferPage) {
    int story = 0;
    if (preferPage >= 0 && preferPage < static_cast<int>(pages.size())) {
        story = masterStoryOn(doc, pages[static_cast<std::size_t>(preferPage)], master);
    }
    for (std::size_t i = 0; story <= 0 && i < pages.size(); ++i) story = masterStoryOn(doc, pages[i], master);
    if (story <= 0) return false;
    PointerHold& hold = pointerHold();
    const PointerCursor before = pointerCursor();
    if (hold.bandStory <= 0 && before.active && before.story > 0) {
        hold.bandReturnStory = before.story;
        hold.bandReturnPara = before.paragraph;
        hold.bandReturnOff = before.offset;
    }
    clearComposition(hold);
    hold.preferredX = -1;
    hold.editStory = story;
    hold.bandStory = story;
    hold.bandHeader = false;
    hold.bandMaster = true;
    setMasterView(true);
    setPointerCursor(doc, pages, model::CursorPos{story, 0, 0});
    hold.eventText = "바탕쪽";
    return true;
}

// 고치는 바탕쪽(구역 번호와 그 구역 바탕쪽 순번). 바탕쪽 편집이 아니면 거짓.
bool editedMaster(const model::Document& doc, int& section, int& index) {
    const PointerHold& hold = pointerHold();
    if (hold.bandStory <= 0 || !hold.bandMaster) return false;
    const model::StorySlot* slot = resolveStory(doc, hold.bandStory);
    if (!slot || slot->kind != model::StoryKind::MasterPage || !slot->masterPage) return false;
    section = slot->sectionIndex;
    if (section < 0 || section >= static_cast<int>(doc.sections.size())) return false;
    const auto& masters = doc.sections[static_cast<std::size_t>(section)].masterPages;
    for (std::size_t i = 0; i < masters.size(); ++i) {
        if (&masters[i] == slot->masterPage) {
            index = static_cast<int>(i);
            return true;
        }
    }
    return false;
}

// 나갈 본문 자리. 바탕쪽을 지우면 뒤 구역 글 묶음 번호가 바뀌니 문단 고유 번호로 들고 있는다.
HeldCaret masterReturn(model::Document& doc, const std::vector<Node>& pages) {
    const PointerHold& hold = pointerHold();
    model::ensureParagraphIds(doc);
    HeldCaret back;
    if (const model::Paragraph* ret = storyParagraph(doc, hold.bandReturnStory, hold.bandReturnPara)) {
        back.active = true;
        back.anchorId = back.focusId = ret->id;
        back.anchorChar = back.focusChar =
            offsetToCharIndex(doc, pages, hold.bandReturnStory, hold.bandReturnPara, hold.bandReturnOff);
    }
    return back;
}

void leaveMasterTo(const model::Document& doc, const std::vector<Node>& pages, const HeldCaret& back) {
    PointerHold& hold = pointerHold();
    if (hold.editStory == hold.bandStory) hold.editStory = 0;
    hold.bandStory = 0;
    hold.bandMaster = false;
    setMasterView(false);
    clearComposition(hold);
    hold.preferredX = -1;
    if (back.active) {
        placeHeldCaret(doc, pages, back);
    } else {
        setPointerCursor(doc, pages, model::CursorPos{1, 0, 0});
    }
}

void setSectionHideFirstMaster(model::Section& section, bool on) {
    section.hideFirstMaster = on;
    if (section.paragraphs.empty()) return;
    for (model::Run& run : section.paragraphs.front().runs) {
        for (model::InlineItem& item : run.items) {
            if (isControl(item, model::ControlKind::SectionDef)) item.control.hideFirstMaster = on;
        }
    }
}

// 바탕쪽 편집으로 들어간다. 그 구역에 그 종류가 없으면 빈 문단 하나로 만든다(한글은 양쪽·짝수·홀수·마지막 쪽·임의 쪽
// 차례로 둔다). 겹치게 하기·맨 위는 주어진 것만 바꾼다.
EditResult openMaster(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                      bool useStoredLineHeight, std::string& why) {
    PointerHold& hold = pointerHold();
    if (hold.bandStory > 0 && !hold.bandMaster) {
        why = kBandBusy;
        return {};
    }
    int section = -1;
    int current = -1;
    if (!editedMaster(doc, section, current)) section = cursorSection(doc);
    if (section < 0) return {};
    std::string type = textField(fields, "type", "BOTH");
    if (std::find_if(std::begin(kMasterTypes), std::end(kMasterTypes), [&](const char* t) { return type == t; }) ==
        std::end(kMasterTypes)) {
        type = "BOTH";
    }
    const std::uint32_t pageNumber =
        type == "OPTIONAL_PAGE"
            ? static_cast<std::uint32_t>(std::clamp(std::lround(numberField(fields, "pageNumber", 1)), 1L, 65535L))
            : 0u;
    model::Section& sec = doc.sections[static_cast<std::size_t>(section)];
    int index = -1;
    for (std::size_t i = 0; i < sec.masterPages.size(); ++i) {
        if (sameMasterKind(sec.masterPages[i], type, pageNumber)) {
            index = static_cast<int>(i);
            break;
        }
    }
    // 바탕쪽 편집 중 다른 바탕쪽으로 옮기면 나갈 본문 자리는 처음 것을 그대로 둔다(enterMaster).
    bool changed = false;
    if (index < 0) {
        model::MasterPage master;
        master.applyPageType = type;
        master.pageNumber = pageNumber;
        const model::PageDef page = sec.page.value_or(model::PageDef{});
        master.content.vertAlign = "TOP";
        master.content.textWidth = static_cast<std::uint32_t>(
            std::max<std::int64_t>(0, static_cast<std::int64_t>(page.width) - page.marginLeft - page.marginRight));
        master.content.textHeight = static_cast<std::uint32_t>(std::max<std::int64_t>(
            0, static_cast<std::int64_t>(page.height) - page.marginTop - page.marginBottom - page.marginHeader -
                   page.marginFooter));
        model::Paragraph para;
        model::Run run;
        if (!doc.header.styles.empty()) {
            para.styleId = doc.header.styles.front().id;
            para.paraShapeId = doc.header.styles.front().paraShapeId;
            run.charShapeId = doc.header.styles.front().charShapeId;
        }
        para.runs.push_back(std::move(run));
        master.content.paragraphs.push_back(std::move(para));
        const int rank = masterRank(master);
        auto at = std::find_if(sec.masterPages.begin(), sec.masterPages.end(),
                               [&](const model::MasterPage& mp) { return masterRank(mp) > rank; });
        index = static_cast<int>(at - sec.masterPages.begin());
        sec.masterPages.insert(at, std::move(master));
        changed = true;
    }
    model::MasterPage& target = sec.masterPages[static_cast<std::size_t>(index)];
    for (const auto& [name, value] : fields) {
        if (name == "duplicate" && target.duplicate != flagField(fields, "duplicate", target.duplicate)) {
            target.duplicate = !target.duplicate;
            changed = true;
        } else if (name == "front" && target.printOnTop != flagField(fields, "front", target.printOnTop)) {
            target.printOnTop = !target.printOnTop;
            changed = true;
        }
    }
    EditRelayoutResult relayout;
    if (changed) relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    if (!enterMaster(doc, pages, &sec.masterPages[static_cast<std::size_t>(index)], hold.caretPage)) {
        why = "그 바탕쪽이 보이는 쪽이 없습니다.";
        if (!changed) return {};
    }
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, changed, cur.story, cur.paragraph, changed ? &relayout : nullptr);
}

EditResult closeMaster(model::Document& doc, std::vector<Node>& pages, std::string& why) {
    int section = -1;
    int index = -1;
    if (!editedMaster(doc, section, index)) {
        why = "바탕쪽을 고치는 중이 아닙니다.";
        return {};
    }
    leaveMasterTo(doc, pages, masterReturn(doc, pages));
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, false, cur.story, cur.paragraph);
}

// 바탕쪽 지우기(한글 DeleteMasterPage·DeleteSectionMasterPage·DeleteDocumentMasterPage). 지우면 본문으로 나간다.
EditResult deleteMaster(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                        bool useStoredLineHeight, std::string& why) {
    int section = -1;
    int index = -1;
    if (!editedMaster(doc, section, index)) {
        why = "바탕쪽을 고치는 중이 아닙니다.";
        return {};
    }
    const std::string scope = textField(fields, "scope", "current");
    const HeldCaret back = masterReturn(doc, pages);
    if (scope == "document") {
        for (model::Section& sec : doc.sections) sec.masterPages.clear();
    } else if (scope == "section") {
        doc.sections[static_cast<std::size_t>(section)].masterPages.clear();
    } else {
        auto& masters = doc.sections[static_cast<std::size_t>(section)].masterPages;
        masters.erase(masters.begin() + index);
    }
    PointerHold& hold = pointerHold();
    if (hold.editStory == hold.bandStory) hold.editStory = 0;
    hold.bandStory = 0;
    hold.bandMaster = false;
    setMasterView(false);
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    leaveMasterTo(doc, pages, back);
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

// 앞·뒤 바탕쪽(한글 MasterPageToPrev·MasterPageToNext). 문서 차례(구역, 그 구역 바탕쪽 차례)로 옮긴다.
EditResult stepMaster(model::Document& doc, std::vector<Node>& pages, bool next, std::string& why) {
    int section = -1;
    int index = -1;
    if (!editedMaster(doc, section, index)) {
        why = "바탕쪽을 고치는 중이 아닙니다.";
        return {};
    }
    std::vector<const model::MasterPage*> all;
    std::size_t at = 0;
    for (std::size_t si = 0; si < doc.sections.size(); ++si) {
        for (std::size_t mi = 0; mi < doc.sections[si].masterPages.size(); ++mi) {
            if (static_cast<int>(si) == section && static_cast<int>(mi) == index) at = all.size();
            all.push_back(&doc.sections[si].masterPages[mi]);
        }
    }
    for (int k = static_cast<int>(at) + (next ? 1 : -1); k >= 0 && k < static_cast<int>(all.size()); k += next ? 1 : -1) {
        if (enterMaster(doc, pages, all[static_cast<std::size_t>(k)], pointerHold().caretPage)) {
            const PointerCursor cur = pointerCursor();
            return makeEditResult(doc, true, false, cur.story, cur.paragraph);
        }
    }
    why = next ? "다음 바탕쪽이 없습니다." : "앞 바탕쪽이 없습니다.";
    return {};
}

// 고치는 바탕쪽의 인쇄 시 맨 위(front)·겹치게 하기(duplicate)와 구역의 첫 쪽 제외(excludeFirst).
EditResult setMasterOptions(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                            bool useStoredLineHeight, std::string& why) {
    int section = -1;
    int index = -1;
    if (!editedMaster(doc, section, index)) {
        why = "바탕쪽을 고치는 중이 아닙니다.";
        return {};
    }
    model::Section& sec = doc.sections[static_cast<std::size_t>(section)];
    model::MasterPage& master = sec.masterPages[static_cast<std::size_t>(index)];
    const bool front = flagField(fields, "front", master.printOnTop);
    const bool duplicate = flagField(fields, "duplicate", master.duplicate);
    const bool excludeFirst = flagField(fields, "excludeFirst", sec.hideFirstMaster);
    if (front == master.printOnTop && duplicate == master.duplicate && excludeFirst == sec.hideFirstMaster) return {};
    master.printOnTop = front;
    master.duplicate = duplicate;
    setSectionHideFirstMaster(sec, excludeFirst);
    const PointerCursor before = pointerCursor();
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    // 다시 놓으면 쪽마다 붙는 별칭 글 묶음 번호가 바뀐다. 그 바탕쪽을 다시 찾아 같은 자리에 둔다.
    if (enterMaster(doc, pages, &master, pointerHold().caretPage)) {
        setPointerCursor(doc, pages, model::CursorPos{pointerHold().bandStory, before.paragraph, before.offset});
    }
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

// ---- 쪽 지우기 ----
// 숨긴 한글 2024 의 DeletePage 로 확인한 것: 쪽 지우기는 그 쪽 첫 글자부터 다음 쪽 첫 글자 앞까지를 블록으로 지운 것과
// 같다(쪽에 걸친 문단은 그 쪽 글자만 빠지고, 남는 문단은 앞 문단 모양이다). 구역 마지막 쪽은 앞 문단 끝부터 지워 빈 쪽이
// 남지 않고, 모든 쪽을 지우면 빈 문단 하나가 남는다. 첫 문단의 구역 정의·단 정의는 남고 머리말·쪽 번호 등은 같이 지운다.

struct PageStart {
    bool ok = false;
    int story = 0;
    int paragraph = 0;
    int offset = 0;
};

// 쪽의 본문 첫 줄 자리.
PageStart pageStartOf(const Node& paper) {
    PageStart out;
    for (const Node& body : paper.children) {
        if (body.kind != NodeKind::Body || body.pageBand || body.fromMaster || !body.role.empty()) continue;
        for (const Node& column : body.children) {
            for (const Node& para : column.children) {
                if (para.kind != NodeKind::Paragraph) continue;
                for (const Node& line : para.children) {
                    if (line.kind != NodeKind::Line) continue;
                    out.ok = true;
                    out.story = line.story > 0 ? line.story : para.story;
                    out.paragraph = line.paragraph >= 0 ? line.paragraph : para.paragraph;
                    out.offset = std::max(0, line.start);
                    return out;
                }
            }
        }
    }
    return out;
}

// "2-3,5,7-" 를 쪽(1 부터)으로. 끝이 빈 범위는 마지막 쪽까지다. 못 읽으면 비었다.
std::vector<int> parsePageRanges(const std::string& text, int last) {
    std::vector<int> out;
    std::size_t i = 0;
    const auto number = [&](int& value) {
        while (i < text.size() && text[i] == ' ') ++i;
        const std::size_t from = i;
        value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') value = value * 10 + (text[i++] - '0');
        while (i < text.size() && text[i] == ' ') ++i;
        return i > from;
    };
    while (i < text.size()) {
        int a = 0;
        if (!number(a)) return {};
        int b = a;
        if (i < text.size() && text[i] == '-') {
            ++i;
            if (!number(b)) b = last;
        }
        for (int k = a; k <= b && k <= last; ++k) out.push_back(k);
        if (i < text.size() && text[i] != ',') return {};
        if (i < text.size()) ++i;
    }
    return out;
}

EditResult deletePages(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                       bool useStoredLineHeight, std::string& why) {
    PointerHold& hold = pointerHold();
    if (hold.bandStory > 0) {
        why = hold.bandMaster ? "바탕쪽을 고치는 중에는 할 수 없습니다." : kBandBusy;
        return {};
    }
    if (pages.empty()) return {};
    const std::string range = textField(fields, "range", "current");
    std::vector<int> targets;  // 쪽 순번(0 부터)
    const PointerCursor cur = pointerCursor();
    const auto caretPageOf = [&](int story, int para, int off) {
        const CaretPlace place = caretForCursor(doc, pages, model::CursorPos{story, para, off});
        return place.hit ? place.page : -1;
    };
    if (range == "custom") {
        const bool byNumber = flagField(fields, "pageNumber", false);
        const int last = byNumber ? 65535 : static_cast<int>(pages.size());
        const std::vector<int> wanted = parsePageRanges(textField(fields, "pages", ""), last);
        for (const int w : wanted) {
            if (!byNumber) {
                if (w >= 1 && w <= static_cast<int>(pages.size())) targets.push_back(w - 1);
                continue;
            }
            for (std::size_t i = 0; i < pages.size(); ++i) {
                if (pages[i].pageNumber == w) targets.push_back(static_cast<int>(i));
            }
        }
    } else {
        const bool block = cur.active && cur.hasSelection && cur.story > 0;
        if (range == "selected" && !block) {
            why = "블록을 지정한 뒤 고르십시오.";
            return {};
        }
        int first = block ? caretPageOf(cur.story, cur.anchorParagraph, cur.anchorOffset) : -1;
        int last = cur.active ? caretPageOf(cur.story, cur.paragraph, cur.offset) : -1;
        if (last < 0) last = hold.caretPage;
        if (first < 0) first = last;
        if (first > last) std::swap(first, last);
        for (int i = std::max(0, first); i <= last && i < static_cast<int>(pages.size()); ++i) targets.push_back(i);
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    if (targets.empty()) {
        why = "지울 쪽이 없습니다.";
        return {};
    }
    // 이어진 쪽(같은 구역)을 한 범위로 묶고, 뒤 범위부터 지운다. 앞 범위의 글 자리는 그대로다.
    struct Cut {
        int story = 0;
        int startPara = 0, startOff = 0, endPara = 0, endOff = 0;
    };
    std::vector<Cut> cuts;
    for (std::size_t k = 0; k < targets.size();) {
        std::size_t e = k;
        while (e + 1 < targets.size() && targets[e + 1] == targets[e] + 1 &&
               pages[static_cast<std::size_t>(targets[e + 1])].section == pages[static_cast<std::size_t>(targets[k])].section) {
            ++e;
        }
        const int a = targets[k];
        const int b = targets[e];
        k = e + 1;
        const PageStart start = pageStartOf(pages[static_cast<std::size_t>(a)]);
        if (!start.ok) continue;
        std::vector<model::Paragraph>* paras = storyParagraphs(doc, start.story);
        if (!paras || paras->empty()) continue;
        Cut cut;
        cut.story = start.story;
        cut.startPara = start.paragraph;
        cut.startOff = start.offset;
        PageStart next;
        if (b + 1 < static_cast<int>(pages.size()) &&
            pages[static_cast<std::size_t>(b + 1)].section == pages[static_cast<std::size_t>(a)].section) {
            next = pageStartOf(pages[static_cast<std::size_t>(b + 1)]);
        }
        if (next.ok && next.story == start.story) {
            cut.endPara = next.paragraph;
            cut.endOff = next.offset;
        } else {
            // 구역 끝까지. 쪽이 문단 처음에서 시작하면 앞 문단 끝부터 지워 빈 문단이 남지 않게 한다.
            cut.endPara = static_cast<int>(paras->size()) - 1;
            const model::Paragraph& lastPara = (*paras)[static_cast<std::size_t>(cut.endPara)];
            cut.endOff = charIndexToOffset(doc, pages, cut.story, cut.endPara, paragraphCharCount(lastPara));
            if (cut.startOff == 0 && cut.startPara > 0) {
                --cut.startPara;
                const model::Paragraph& prev = (*paras)[static_cast<std::size_t>(cut.startPara)];
                cut.startOff = charIndexToOffset(doc, pages, cut.story, cut.startPara, paragraphCharCount(prev));
            }
        }
        // 첫 문단 맨 앞의 구역 정의·단 정의는 남긴다.
        if (cut.startPara == 0 && cut.startOff == 0) {
            const model::Paragraph& firstPara = paras->front();
            int keep = 0;
            for (const model::Run& run : firstPara.runs) {
                bool stop = false;
                for (const model::InlineItem& item : run.items) {
                    if (item.type == model::InlineItem::Type::Control &&
                        (item.control.kind == model::ControlKind::SectionDef ||
                         item.control.kind == model::ControlKind::ColumnDef)) {
                        if (isCaretCharControl(item.control)) ++keep;
                        continue;
                    }
                    stop = true;
                    break;
                }
                if (stop) break;
            }
            cut.startOff = charIndexToOffset(doc, pages, cut.story, 0, keep);
        }
        cuts.push_back(cut);
    }
    if (cuts.empty()) {
        why = "지울 쪽이 없습니다.";
        return {};
    }
    clearComposition(hold);
    hold.preferredX = -1;
    int outPara = 0;
    int outChar = 0;
    int outStory = 0;
    for (auto it = cuts.rbegin(); it != cuts.rend(); ++it) {
        if (!deleteSelectionRange(doc, pages, it->story, it->startPara, it->startOff, it->endPara, it->endOff, outPara,
                                  outChar)) {
            continue;
        }
        outStory = it->story;
    }
    if (outStory <= 0) return {};
    model::ensureParagraphIds(doc);
    const model::Paragraph* caretPara = storyParagraph(doc, outStory, outPara);
    HeldCaret held;
    if (caretPara) {
        held.active = true;
        held.anchorId = held.focusId = caretPara->id;
        held.anchorChar = held.focusChar = outChar;
    }
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    if (held.active) placeHeldCaret(doc, pages, held);
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

// ---- 메모 ----
// 메모는 필드(%%me)다. 시작은 블록 앞, 끝(FieldEnd)은 블록 뒤에 두고 메모 글은 필드의 글 목록이다(docs/memo-structure.md).
// 메모 글 문단은 "메모" 스타일(한글 새 문서 17번, 영문 Memo)을 쓴다. 없으면 바탕글이다.

struct MemoAt {
    model::Paragraph* para = nullptr;
    model::Control* memo = nullptr;
};

// 캐럿 문단에서 캐럿 글자를 품은 메모(겹치면 안쪽).
MemoAt memoAtCaret(model::Document& doc, const std::vector<Node>& pages) {
    MemoAt out;
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return out;
    model::Paragraph* para = storyParagraph(doc, cur.story, cur.paragraph);
    if (!para) return out;
    // 지우기가 표시에 닿아 고른 메모. 그 캐럿 자리 그대로면 그 메모다.
    PointerHold& hold = pointerHold();
    if (hold.pendingMemo && hold.pendingMemoStory == cur.story && hold.pendingMemoPara == cur.paragraph &&
        hold.pendingMemoOffset == cur.offset) {
        for (model::Run& run : para->runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Control && &item.control == hold.pendingMemo) {
                    out.para = para;
                    out.memo = &item.control;
                    return out;
                }
            }
        }
    }
    const int caret = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    std::vector<model::Control*> open;
    int chars = 0;
    model::Control* found = nullptr;
    for (model::Run& run : para->runs) {
        for (model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control) {
                if (model::isMemoField(item.control)) open.push_back(&item.control);
                else if (item.control.kind == model::ControlKind::FieldEnd && !open.empty()) {
                    if (!found && chars >= caret) found = open.back();
                    open.pop_back();
                }
                if (isCaretCharControl(item.control)) ++chars;
                continue;
            }
            if (item.type == model::InlineItem::Type::Text) {
                const int len = utf8CodePointCount(item.text);
                if (!found && !open.empty() && caret >= chars && caret <= chars + len) found = open.back();
                chars += len;
            }
        }
    }
    if (!found && !open.empty()) found = open.back();
    out.para = para;
    out.memo = found;
    return out;
}

std::vector<model::Paragraph> memoParagraphs(const model::Document& doc, const std::string& text) {
    const model::Style* style = nullptr;
    for (const model::Style& st : doc.header.styles) {
        if (st.engName == "Memo" || st.name == "메모") {
            style = &st;
            break;
        }
    }
    std::vector<model::Paragraph> out;
    std::size_t from = 0;
    while (true) {
        const std::size_t nl = text.find('\n', from);
        std::string line = text.substr(from, nl == std::string::npos ? std::string::npos : nl - from);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        model::Paragraph para;
        model::Run run;
        if (style) {
            para.styleId = style->id;
            para.paraShapeId = style->paraShapeId;
            run.charShapeId = style->charShapeId;
        }
        if (!line.empty()) run.items.push_back(model::InlineItem::makeText(line));
        para.runs.push_back(std::move(run));
        out.push_back(std::move(para));
        if (nl == std::string::npos) break;
        from = nl + 1;
    }
    return out;
}

// 블록 없이 메모를 넣으면 한글은 캐럿 낱말에 건다(숨긴 한글 InsertFieldMemo 로 확인). 낱말은 빈칸·탭으로 나뉜 글자
// 묶음이다(문장 부호·NBSP 는 낱말 글자). 캐럿 바로 앞이 낱말 글자면 그 낱말, 아니면 뒤쪽 첫 낱말, 뒤에 낱말이 없으면
// 앞쪽 낱말이다. 빈 문단은 빈 범위다. 글자처럼 놓인 개체는 낱말을 끊는다고 본다(한글로 재지 않음).
// 빈 범위 메모는 한글이 읽기는 하지만 화면에 보이지 않는다.
std::pair<int, int> memoWordAt(const model::Paragraph& para, int caret) {
    std::vector<char> word;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control) {
                if (isCaretCharControl(item.control)) word.push_back(0);
                continue;
            }
            if (item.type != model::InlineItem::Type::Text) continue;
            for (std::size_t i = 0; i < item.text.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(item.text[i]);
                if ((c & 0xC0) == 0x80) continue;  // UTF-8 이어지는 바이트
                word.push_back(c == ' ' || c == '\t' ? 0 : 1);
            }
        }
    }
    const int n = static_cast<int>(word.size());
    caret = std::clamp(caret, 0, n);
    int at = caret - 1;
    if (at < 0 || !word[static_cast<std::size_t>(at)]) {
        at = caret;
        while (at < n && !word[static_cast<std::size_t>(at)]) ++at;
        if (at >= n) {
            at = caret - 1;
            while (at >= 0 && !word[static_cast<std::size_t>(at)]) --at;
            if (at < 0) return {caret, caret};
        }
    }
    int from = at;
    while (from > 0 && word[static_cast<std::size_t>(from - 1)]) --from;
    int to = at;
    while (to < n && word[static_cast<std::size_t>(to)]) ++to;
    return {from, to};
}

EditResult insertMemo(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                      bool useStoredLineHeight, std::string& why, const char* ctrlId = "%%me") {
    model::Paragraph* host = caretParagraph(doc, why);
    if (!host) return {};
    const PointerCursor cur = pointerCursor();
    int fromChar = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    int toChar = fromChar;
    // 블록이 한 문단 안이면 그 범위에 건다. 여러 문단이면 블록 가운데 캐럿 문단에 든 부분이다(블록 끝 문단이면
    // 문단 맨 앞부터, 블록 첫 문단이면 문단 끝까지). 블록이 없으면 캐럿 낱말이다.
    if (cur.hasSelection && cur.startParagraph == cur.endParagraph && cur.startParagraph == cur.paragraph) {
        fromChar = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
        toChar = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
    } else if (cur.hasSelection && cur.paragraph == cur.endParagraph) {
        fromChar = 0;
        toChar = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
    } else if (cur.hasSelection && cur.paragraph == cur.startParagraph) {
        fromChar = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
        toChar = paragraphCharCount(*host);
    }
    if (fromChar == toChar) std::tie(fromChar, toChar) = memoWordAt(*host, fromChar);
    model::MemoCommand cmd;
    cmd.author = textField(fields, "author", "");
    cmd.created = model::filetimeNow();
    model::Control memo;
    memo.kind = model::ControlKind::Field;
    memo.ctrlId = ctrlId;
    memo.fieldProps = 0x8001u;
    memo.command = model::memoCommandText(cmd);
    model::SubList text;
    text.vertAlign = "TOP";
    text.paragraphs = memoParagraphs(doc, textField(fields, "text", ""));
    memo.subLists.push_back(std::move(text));
    model::Control end;
    end.kind = model::ControlKind::FieldEnd;
    end.ctrlId = "fend";
    const HeldCaret held = holdCaret(doc, pages);
    clearComposition(pointerHold());
    insertControlAt(*host, toChar, std::move(end));
    insertControlAt(*host, fromChar, std::move(memo));
    pointerHold().preferredX = -1;
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

// 교정 부호 넣기. 범위 부호는 캐럿 문단 안 블록을 필드 시작·끝으로 감싼다. 자리 부호는 캐럿 자리에 빈 범위로 둔다.
EditResult insertProofMark(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                           bool useStoredLineHeight, std::string& why) {
    struct Kind {
        const char* name;
        const char* ctrlId;
        const char* command;  // $Revision 뒤 이름
        int sign;             // %sig 번호. 0 이면 범위 부호, -1 이면 자리 옮김표
        bool text;            // 넣을·고칠 글이 있다
    };
    static const Kind kKinds[] = {
        {"delete", "%%*d", "Delete", 0, false},     {"clipping", "%%*C", "Clipping", 0, false},
        {"attach", "%%*a", "Attach", 0, false},     {"lineAttach", "%%*A", "LineAttach", 0, false},
        {"sawtooth", "%%*S", "Sawtooth", 0, false}, {"thinking", "%%*T", "Thinking", 0, false},
        {"praise", "%%*P", "Praise", 0, false},     {"line", "%%*L", "Line", 0, false},
        {"simpleChange", "%%*c", "SimpleChange", 0, true}, {"change", "%%mr", "", 0, false},
        {"space", "%sig", "Sign", 1, false},        {"lineSeparate", "%sig", "Sign", 2, false},
        {"lineInsert", "%sig", "Sign", 3, false},   {"symbol", "%sig", "Sign", 14, true},
        {"insert", "%sig", "Sign", 15, true},
        {"rightMove", "%%*r", "RightMove", -1, false}, {"leftMove", "%%*l", "LeftMove", -1, false},
    };
    const std::string name = textField(fields, "kind", "");
    const Kind* kind = nullptr;
    for (const Kind& k : kKinds) {
        if (name == k.name) kind = &k;
    }
    if (!kind) {
        why = "모르는 교정 부호입니다.";
        return {};
    }
    model::Paragraph* host = caretParagraph(doc, why);
    if (!host) return {};
    const PointerCursor cur = pointerCursor();
    const bool range = kind->sign == 0;
    const bool move = kind->sign < 0;
    if (range && kind->ctrlId != std::string("%%mr") && !cur.hasSelection) {
        why = "교정할 곳을 블록으로 잡으십시오.";
        return {};
    }
    if (!range && cur.hasSelection) {
        why = "블록 없이 커서 자리에 넣는 교정 부호입니다.";
        return {};
    }
    // 고칠·넣을 글. 명령의 ';' 는 값 사이 구분이라 글에서 뺀다.
    std::string text = textField(fields, "text", "");
    text.erase(std::remove(text.begin(), text.end(), ';'), text.end());
    if (kind->text && text.empty()) {
        why = "입력할 내용을 넣으십시오.";
        return {};
    }
    if (kind->ctrlId == std::string("%%mr")) {
        // 메모 고침표는 메모와 같은 모양이다(메모 넣기와 같은 범위 규칙).
        return insertMemo(doc, pages, fields, useStoredLineHeight, why, "%%mr");
    }
    int fromChar = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    int toChar = fromChar;
    if (range && cur.hasSelection) {
        // 블록이 한 문단 안이면 그 범위, 여러 문단이면 캐럿 문단에 든 부분이다(메모와 같다).
        if (cur.startParagraph == cur.endParagraph && cur.startParagraph == cur.paragraph) {
            fromChar = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
            toChar = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
        } else if (cur.paragraph == cur.endParagraph) {
            fromChar = 0;
            toChar = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
        } else {
            fromChar = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
            toChar = paragraphCharCount(*host);
        }
    }
    // 줄 붙임표는 블록 문단 맨 앞에 빈 범위로 들어간다(한글 2024).
    if (kind->ctrlId == std::string("%%*A")) {
        fromChar = 0;
        toChar = 0;
    }
    // 차례 번호: 범위 부호·자리 옮김표는 문서 안 교정 부호 번호 가운데 가장 큰 것 + 1, 띄움표 같은 %sig 는 0.
    std::uint32_t order = 0;
    if (range || move) {
        const auto scan = [&](const auto& self, const std::vector<model::Paragraph>& paras) -> void {
            for (const model::Paragraph& p : paras) {
                for (const model::Run& run : p.runs) {
                    for (const model::InlineItem& item : run.items) {
                        if (item.type != model::InlineItem::Type::Control) continue;
                        if (model::proofKindOf(item.control) != model::ProofKind::None && !model::isMemoField(item.control)) {
                            order = std::max(order, item.control.memoNumber);
                        }
                        for (const model::SubList& sub : item.control.subLists) self(self, sub.paragraphs);
                    }
                }
            }
        };
        for (const model::Section& section : doc.sections) scan(scan, section.paragraphs);
        ++order;
    }
    model::Control mark;
    mark.kind = model::ControlKind::Field;
    mark.ctrlId = kind->ctrlId;
    mark.fieldProps = range ? 0x8000u : 0u;
    mark.memoNumber = order;
    mark.command = std::string("$Revision") + kind->command + (kind->text ? "?" + text : std::string()) + ";";
    if (kind->sign > 0) mark.command += std::to_string(kind->sign) + ";";
    if (move) {
        // 자리 옮김표: 시작 위치·간격(pt). 한글 대화 상자의 처음 값은 오른 자리 0pt·10pt, 왼 자리 10pt·10pt 다.
        const double startPt = numberField(fields, "start", kind->ctrlId == std::string("%%*r") ? 0.0 : 10.0);
        const double gapPt = numberField(fields, "gap", 10.0);
        if (startPt < 0 || gapPt <= 0) {
            why = "시작 위치는 0pt 이상, 간격은 0pt 보다 커야 합니다.";
            return {};
        }
        mark.command = model::proofMoveCommand(kind->ctrlId == std::string("%%*r"), startPt, gapPt);
    }
    model::Control end;
    end.kind = model::ControlKind::FieldEnd;
    end.ctrlId = "fend";
    const HeldCaret held = holdCaret(doc, pages);
    clearComposition(pointerHold());
    insertControlAt(*host, toChar, std::move(end));
    insertControlAt(*host, fromChar, std::move(mark));
    pointerHold().preferredX = -1;
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

EditResult editMemo(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                    bool useStoredLineHeight, std::string& why) {
    const MemoAt at = memoAtCaret(doc, pages);
    if (!at.memo) {
        why = "캐럿 자리에 메모가 없습니다.";
        return {};
    }
    model::SubList text;
    text.vertAlign = "TOP";
    text.paragraphs = memoParagraphs(doc, textField(fields, "text", ""));
    if (at.memo->subLists.empty()) at.memo->subLists.push_back(std::move(text));
    else at.memo->subLists.front().paragraphs = std::move(text.paragraphs);
    const HeldCaret held = holdCaret(doc, pages);
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

// 메모 지우기: 본문 글은 남기고 메모 필드 시작과 그 짝인 끝을 뺀다.
EditResult deleteMemo(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight, std::string& why) {
    const MemoAt at = memoAtCaret(doc, pages);
    if (!at.memo || !at.para) {
        why = "캐럿 자리에 메모가 없습니다.";
        return {};
    }
    // 짝인 끝: 시작 뒤에서 열린 필드 수를 세어 0 이 되는 끝.
    model::Control* end = nullptr;
    bool after = false;
    int depth = 0;
    for (model::Run& run : at.para->runs) {
        for (model::InlineItem& item : run.items) {
            if (item.type != model::InlineItem::Type::Control) continue;
            if (&item.control == at.memo) {
                after = true;
                continue;
            }
            if (!after) continue;
            if (item.control.kind == model::ControlKind::Field) ++depth;
            if (item.control.kind == model::ControlKind::FieldEnd) {
                if (depth == 0) {
                    end = &item.control;
                    break;
                }
                --depth;
            }
        }
        if (end) break;
    }
    pointerHold().pendingMemo = nullptr;
    const HeldCaret held = holdCaret(doc, pages);
    if (end) eraseControl(*at.para, end);
    eraseControl(*at.para, at.memo);
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

FormatFields memoFields(model::Document& doc, const std::vector<Node>& pages) {
    FormatFields out;
    const MemoAt at = memoAtCaret(doc, pages);
    out.emplace_back("inMemo", at.memo != nullptr);
    if (!at.memo) return out;
    const model::MemoCommand cmd = model::parseMemoCommand(at.memo->command);
    out.emplace_back("author", cmd.author);
    std::string text;
    if (!at.memo->subLists.empty()) {
        for (const model::Paragraph& p : at.memo->subLists.front().paragraphs) {
            if (!text.empty() || &p != &at.memo->subLists.front().paragraphs.front()) text += "\n";
            for (const model::Run& r : p.runs) {
                for (const model::InlineItem& item : r.items) {
                    if (item.type == model::InlineItem::Type::Text) text += item.text;
                }
            }
        }
    }
    out.emplace_back("text", text);
    return out;
}

// ---- 블록 계산 ----

// 블록 글: 칸 블록이면 칸 글을 칸 차례(줄, 칸)로, 글 블록이면 고른 글. 블록이 없으면 거짓.
bool blockCalcText(const model::Document& doc, const std::vector<Node>& pages, std::string& text, bool& cells) {
    const DragHost& drag = pointerHold().drag;
    if (drag.mode == DragMode::Cells && drag.control && drag.control->table && drag.focusRow >= 0 && drag.focusCol >= 0) {
        const int r0 = std::min(drag.row, drag.focusRow);
        const int c0 = std::min(drag.col, drag.focusCol);
        const int r1 = std::max(drag.row + drag.rowSpan - 1, drag.focusRow + drag.focusRowSpan - 1);
        const int c1 = std::max(drag.col + drag.colSpan - 1, drag.focusCol + drag.focusColSpan - 1);
        std::vector<const model::TableCell*> inside;
        for (const model::TableCell& c : drag.control->table->cells) {
            const int row = static_cast<int>(c.row);
            const int col = static_cast<int>(c.col);
            if (row >= r0 && row <= r1 && col >= c0 && col <= c1) inside.push_back(&c);
        }
        std::stable_sort(inside.begin(), inside.end(), [](const model::TableCell* a, const model::TableCell* b) {
            return a->row != b->row ? a->row < b->row : a->col < b->col;
        });
        text.clear();
        for (const model::TableCell* c : inside) text += model::formulaCellText(*c) + "\n";
        cells = true;
        return true;
    }
    const PointerCursor cur = pointerCursor();
    if (!cur.active || !cur.hasSelection) return false;
    text = selectionText(doc, pages);
    cells = false;
    return !text.empty();
}

FormatFields blockCalcFields(const model::Document& doc, const std::vector<Node>& pages) {
    FormatFields out;
    std::string text;
    bool cells = false;
    const bool ok = blockCalcText(doc, pages, text, cells);
    out.emplace_back("ok", ok);
    out.emplace_back("cells", cells);
    const model::BlockNumbers numbers = ok ? model::blockNumbers(text) : model::BlockNumbers{};
    out.emplace_back("count", static_cast<double>(numbers.count));
    out.emplace_back("sum", numbers.count > 0 ? model::blockSumText(numbers) : std::string());
    out.emplace_back("average", model::blockAverageText(numbers));
    out.emplace_back("canInsert", ok && !cells && numbers.count > 0 && !readOnly());
    return out;
}

EditResult insertBlockCalc(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                           bool useStoredLineHeight, std::string& why) {
    std::string text;
    bool cells = false;
    if (!blockCalcText(doc, pages, text, cells)) {
        why = "계산할 블록이 없습니다.";
        return {};
    }
    if (cells) {
        why = "칸 블록의 결과는 넣지 않습니다. 복사해 쓰십시오.";
        return {};
    }
    const model::BlockNumbers numbers = model::blockNumbers(text);
    if (numbers.count == 0) {
        why = "블록에 수가 없습니다.";
        return {};
    }
    std::string value = textField(fields, "mode", "sum") == "average" ? model::blockAverageText(numbers)
                                                                      : model::blockSumText(numbers);
    const char last = text.back();
    if (last != ' ' && last != '\t' && last != '\n' && last != '\r') value.insert(value.begin(), ' ');
    const PointerCursor cur = pointerCursor();
    setPointerCursor(doc, pages, model::CursorPos{cur.story, cur.endParagraph, cur.endOffset});
    return insertTextNow(doc, pages, value, false, useStoredLineHeight);
}

FormatFields masterFields(const model::Document& doc) {
    FormatFields out;
    int section = -1;
    int index = -1;
    const bool inMaster = editedMaster(doc, section, index);
    if (!inMaster) section = cursorSection(doc);
    out.emplace_back("inMaster", inMaster);
    if (section < 0) return out;
    const model::Section& sec = doc.sections[static_cast<std::size_t>(section)];
    std::string optional;
    for (const char* type : kMasterTypes) {
        bool has = false;
        for (const model::MasterPage& mp : sec.masterPages) {
            if (!sameMasterKind(mp, type, mp.pageNumber)) continue;
            has = true;
            if (std::string(type) == "OPTIONAL_PAGE") optional += (optional.empty() ? "" : ",") + std::to_string(mp.pageNumber);
        }
        out.emplace_back(std::string("has") + type, has);
    }
    out.emplace_back("optionalPages", optional);
    out.emplace_back("excludeFirst", sec.hideFirstMaster);
    if (inMaster) {
        const model::MasterPage& mp = sec.masterPages[static_cast<std::size_t>(index)];
        out.emplace_back("type", mp.applyPageType.empty() ? std::string("BOTH") : mp.applyPageType);
        out.emplace_back("pageNumber", static_cast<double>(mp.pageNumber));
        out.emplace_back("duplicate", mp.duplicate);
        out.emplace_back("front", mp.printOnTop);
    }
    return out;
}


}  // namespace

EditResult paint_detail::deleteMemoNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    std::string why;
    return deleteMemo(doc, pages, useStoredLineHeight, why);
}

const char* pageControlName(PageControlKind kind) {
    switch (kind) {
        case PageControlKind::Header: return "header";
        case PageControlKind::Footer: return "footer";
        case PageControlKind::DeleteBand: return "deleteBand";
        case PageControlKind::PageNumber: return "pageNumber";
        case PageControlKind::NewNumber: return "newNumber";
        case PageControlKind::PageHiding: return "pageHiding";
        case PageControlKind::InsertPageNum: return "insertPageNum";
        case PageControlKind::PrevBand: return "prevBand";
        case PageControlKind::NextBand: return "nextBand";
        case PageControlKind::EditBand: return "editBand";
        case PageControlKind::MasterPage: return "masterPage";
        case PageControlKind::CloseMaster: return "closeMaster";
        case PageControlKind::DeleteMaster: return "deleteMaster";
        case PageControlKind::PrevMaster: return "prevMaster";
        case PageControlKind::NextMaster: return "nextMaster";
        case PageControlKind::MasterOptions: return "masterOptions";
        case PageControlKind::DeletePage: return "deletePage";
        case PageControlKind::InsertMemo: return "insertMemo";
        case PageControlKind::EditMemo: return "editMemo";
        case PageControlKind::DeleteMemo: return "deleteMemo";
        case PageControlKind::BlockCalc: return "blockCalc";
        case PageControlKind::InsertProofMark: return "insertProofMark";
    }
    return "";
}

bool pageControlKindOf(const std::string& name, PageControlKind& kind) {
    for (PageControlKind each : {PageControlKind::Header, PageControlKind::Footer, PageControlKind::DeleteBand,
                                 PageControlKind::PageNumber, PageControlKind::NewNumber,
                                 PageControlKind::PageHiding, PageControlKind::InsertPageNum,
                                 PageControlKind::PrevBand, PageControlKind::NextBand, PageControlKind::EditBand,
                                 PageControlKind::MasterPage, PageControlKind::CloseMaster, PageControlKind::DeleteMaster,
                                 PageControlKind::PrevMaster, PageControlKind::NextMaster, PageControlKind::MasterOptions,
                                 PageControlKind::DeletePage, PageControlKind::InsertMemo, PageControlKind::EditMemo,
                                 PageControlKind::DeleteMemo, PageControlKind::BlockCalc,
                                 PageControlKind::InsertProofMark}) {
        if (name == pageControlName(each)) {
            kind = each;
            return true;
        }
    }
    return false;
}

FormatFields pageControlFields(const model::Document& doc, const std::vector<Node>& pages, PageControlKind kind) {
    if (kind == PageControlKind::MasterPage || kind == PageControlKind::MasterOptions) return masterFields(doc);
    if (kind == PageControlKind::InsertMemo || kind == PageControlKind::EditMemo) {
        return memoFields(const_cast<model::Document&>(doc), pages);
    }
    if (kind == PageControlKind::BlockCalc) return blockCalcFields(doc, pages);
    FormatFields out;
    if (kind == PageControlKind::EditBand) {
        // 편집 단추를 켤지: 캐럿 쪽부터 찾아 그 종류·위치의 머리말·꼬리말이 보이는 쪽이 있는지.
        for (const char* band : {"header", "footer"}) {
            for (const char* apply : {"BOTH", "ODD", "EVEN"}) {
                int page = -1;
                const bool found = nearestBand(doc, pages, std::string(band) == "header", apply, page);
                out.emplace_back(std::string(band) + apply, found);
            }
        }
        out.emplace_back("inBand", pointerHold().bandStory > 0);
        return out;
    }
    if (kind == PageControlKind::PageNumber) {
        const int section = cursorSection(doc);
        const model::Control* control = nullptr;
        if (section >= 0 && !doc.sections[static_cast<std::size_t>(section)].paragraphs.empty()) {
            control = findControl(doc.sections[static_cast<std::size_t>(section)].paragraphs.front(),
                                  model::ControlKind::PageNumPos);
        }
        out.emplace_back("pos", control && !control->pageNumPos.empty() ? control->pageNumPos : std::string("BOTTOM_CENTER"));
        out.emplace_back("format", control && !control->numFormat.empty() ? control->numFormat : std::string("DIGIT"));
        out.emplace_back("sideChar", control ? control->prefix : std::string("-"));
        out.emplace_back("exists", control != nullptr);
        double start = 1;
        if (section >= 0 && !doc.sections[static_cast<std::size_t>(section)].paragraphs.empty()) {
            auto& first = const_cast<model::Paragraph&>(doc.sections[static_cast<std::size_t>(section)].paragraphs.front());
            if (const model::Control* again = pageNewNumberIn(first)) start = static_cast<double>(again->number);
        }
        out.emplace_back("start", start);
    } else if (kind == PageControlKind::NewNumber) {
        out.emplace_back("numType", std::string("PAGE"));
        out.emplace_back("number", 1.0);
    } else if (kind == PageControlKind::PageHiding) {
        const PointerCursor cur = pointerCursor();
        const model::Paragraph* host = cur.active ? storyParagraph(doc, cur.story, cur.paragraph) : nullptr;
        const model::Control* control = host ? findControl(*host, model::ControlKind::PageHiding) : nullptr;
        out.emplace_back("hideHeader", control && control->hideHeader);
        out.emplace_back("hideFooter", control && control->hideFooter);
        out.emplace_back("hideMaster", control && control->hideMaster);
        out.emplace_back("hideBorder", control && control->hideBorder);
        out.emplace_back("hideFill", control && control->hideFill);
        out.emplace_back("hidePageNum", control && control->hidePageNum);
    }
    return out;
}

EditResult pageControlNow(model::Document& doc, std::vector<Node>& pages, PageControlKind kind,
                          const FormatFields& fields, bool useStoredLineHeight, std::string* error) {
    if (readOnly()) return {};
    std::string why;
    EditResult result;
    switch (kind) {
        case PageControlKind::Header:
        case PageControlKind::Footer:
            result = makeHeaderFooter(doc, pages, kind == PageControlKind::Header, fields, useStoredLineHeight, why);
            break;
        case PageControlKind::DeleteBand: result = deleteBand(doc, pages, useStoredLineHeight, why); break;
        case PageControlKind::PageNumber: result = setPageNumber(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::NewNumber: result = insertNewNumber(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::PageHiding: result = setPageHiding(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::InsertPageNum: result = insertPageNumber(doc, pages, useStoredLineHeight, why); break;
        case PageControlKind::PrevBand: result = stepBand(doc, pages, false, why); break;
        case PageControlKind::NextBand: result = stepBand(doc, pages, true, why); break;
        case PageControlKind::EditBand: result = editBand(doc, pages, fields, why); break;
        case PageControlKind::MasterPage: result = openMaster(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::CloseMaster: result = closeMaster(doc, pages, why); break;
        case PageControlKind::DeleteMaster: result = deleteMaster(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::PrevMaster: result = stepMaster(doc, pages, false, why); break;
        case PageControlKind::NextMaster: result = stepMaster(doc, pages, true, why); break;
        case PageControlKind::MasterOptions:
            result = setMasterOptions(doc, pages, fields, useStoredLineHeight, why);
            break;
        case PageControlKind::DeletePage: result = deletePages(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::InsertMemo: result = insertMemo(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::EditMemo: result = editMemo(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::DeleteMemo: result = deleteMemo(doc, pages, useStoredLineHeight, why); break;
        case PageControlKind::BlockCalc: result = insertBlockCalc(doc, pages, fields, useStoredLineHeight, why); break;
        case PageControlKind::InsertProofMark:
            result = insertProofMark(doc, pages, fields, useStoredLineHeight, why);
            break;
    }
    if (error) *error = why;
    return result;
}

namespace {

// 쪽 테두리/배경 칸(구역의 양쪽·짝수·홀수 셋 가운데 which).
int pageBorderSlot(const model::Section& section, const std::string& which) {
    for (int i = 0; i < 3; ++i) {
        if (section.pageBorders[static_cast<std::size_t>(i)].type == which) return i;
    }
    return which == "EVEN" ? 1 : which == "ODD" ? 2 : 0;
}

double huToMm1(std::int32_t hu) { return std::round(hu * 254.0 / 7200.0) / 10.0; }

}  // namespace

// 한글 2024 쪽 테두리/배경 대화 상자 칸: 종류(양쪽·홀수 쪽·짝수 쪽), 테두리·배경("page." 접두), 위치(종이·쪽 기준,
// 간격 네 개, 머리말·꼬리말 포함), 채울 영역(종이·쪽·테두리), 적용 쪽(모두·첫 쪽 제외·첫 쪽만), 적용 범위.
FormatFields paint_detail::pageBorderValues(const model::Document& doc, const std::string& which) {
    FormatFields out;
    if (doc.sections.empty()) return out;
    const model::Section& section = doc.sections[static_cast<std::size_t>(std::max(0, cursorSection(doc)))];
    const model::PageBorderFill& spec = section.pageBorders[static_cast<std::size_t>(pageBorderSlot(section, which))];
    out.emplace_back("which", which);
    readFillFieldsOf(doc, spec.borderFillId, "page.", out);
    out.emplace_back("basis", std::string(spec.textBorder ? "PAGE" : "PAPER"));
    out.emplace_back("offsetLeft", huToMm1(spec.left));
    out.emplace_back("offsetTop", huToMm1(spec.top));
    out.emplace_back("offsetRight", huToMm1(spec.right));
    out.emplace_back("offsetBottom", huToMm1(spec.bottom));
    out.emplace_back("headerInside", spec.headerInside);
    out.emplace_back("footerInside", spec.footerInside);
    out.emplace_back("fillArea", std::string(spec.fillArea == 1 ? "PAGE" : spec.fillArea >= 2 ? "BORDER" : "PAPER"));
    // 적용 쪽은 구역 설정의 테두리 보이기와 같은 값이다(첫 쪽 제외 = HIDE_FIRST, 첫 쪽만 = SHOW_FIRST).
    out.emplace_back("applyPages", std::string(section.borderShow == "HIDE_FIRST"   ? "EXCEPT_FIRST"
                                               : section.borderShow == "SHOW_FIRST" ? "FIRST_ONLY"
                                                                                    : "ALL"));
    out.emplace_back("sectionCount", static_cast<double>(doc.sections.size()));
    return out;
}

EditResult paint_detail::pageBorderNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                                       bool wholeDocument, bool useStoredLineHeight, std::string& why) {
    if (readOnly() || doc.sections.empty()) return {};
    if (pointerHold().bandStory > 0) {
        why = kBandBusy;
        return {};
    }
    const std::string which = textField(fields, "which", "BOTH");
    const auto applyOne = [&](model::Section& section) {
        const int slot = pageBorderSlot(section, which);
        model::PageBorderFill spec = section.pageBorders[static_cast<std::size_t>(slot)];
        spec.borderFillId = static_cast<std::uint16_t>(applyFillFieldsOf(doc, spec.borderFillId, fields, "page."));
        spec.textBorder = textField(fields, "basis", spec.textBorder ? "PAGE" : "PAPER") == "PAGE";
        const auto mm = [&](const char* key, std::int16_t now) {
            const double v = numberField(fields, key, huToMm1(now));
            return static_cast<std::int16_t>(std::clamp<long>(std::lround(v * 7200.0 / 25.4), 0, 32767));
        };
        spec.left = mm("offsetLeft", spec.left);
        spec.top = mm("offsetTop", spec.top);
        spec.right = mm("offsetRight", spec.right);
        spec.bottom = mm("offsetBottom", spec.bottom);
        spec.headerInside = flagField(fields, "headerInside", spec.headerInside);
        spec.footerInside = flagField(fields, "footerInside", spec.footerInside);
        const std::string area = textField(fields, "fillArea", spec.fillArea == 1 ? "PAGE" : spec.fillArea >= 2 ? "BORDER" : "PAPER");
        spec.fillArea = area == "PAGE" ? 1 : area == "BORDER" ? 2 : 0;
        const std::string applyPages = textField(fields, "applyPages", "");
        const std::string show = applyPages == "EXCEPT_FIRST" ? "HIDE_FIRST" : applyPages == "FIRST_ONLY" ? "SHOW_FIRST" : "SHOW_ALL";
        section.pageBorders[static_cast<std::size_t>(slot)] = spec;
        if (!applyPages.empty()) {
            section.borderShow = show;
            section.fillShow = show;
        }
        // 저장은 구역 정의 컨트롤 값을 먼저 보는 곳이 있다. 같이 고친다.
        if (!section.paragraphs.empty()) {
            for (model::Run& run : section.paragraphs.front().runs) {
                for (model::InlineItem& item : run.items) {
                    if (item.type != model::InlineItem::Type::Control || item.control.kind != model::ControlKind::SectionDef) continue;
                    item.control.pageBorders = section.pageBorders;
                    if (!applyPages.empty()) {
                        item.control.borderShow = show;
                        item.control.fillShow = show;
                    }
                }
            }
        }
    };
    if (wholeDocument) {
        for (model::Section& section : doc.sections) applyOne(section);
    } else {
        applyOne(doc.sections[static_cast<std::size_t>(std::max(0, cursorSection(doc)))]);
    }
    const HeldCaret held = holdCaret(doc, pages);
    clearComposition(pointerHold());
    const EditRelayoutResult relayout = relayoutWhole(doc, pages, useStoredLineHeight);
    placeHeldCaret(doc, pages, held);
    const PointerCursor cur = pointerCursor();
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

}  // namespace chwp::render
