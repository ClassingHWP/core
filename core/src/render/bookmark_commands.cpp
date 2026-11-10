// 책갈피와 찾아가기(한글 2024 책갈피 대화 상자, 찾아가기 Alt+G 의 쪽·책갈피).
//
// 한글 2024 에서 확인한 규칙(Bookmark 동작):
//   - 책갈피는 캐럿 자리에 놓는 폭 없는 조판 부호(bokm)다. 이름만 있다.
//   - 같은 이름이 이미 있으면 넣지 않는다(Execute 가 거짓, 문서 그대로).
//   - 대화 상자의 처음 이름은 캐럿 뒤 낱말(없으면 앞 낱말)이다.
// 블록 책갈피, 이름 바꾸기(아직 확인 못 함)는 아직 없다.

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

bool findBookmark(const model::Document& doc, const std::string& name, BookmarkEntry& out) {
    for (const BookmarkEntry& entry : bookmarkList(doc)) {
        if (entry.name == name) {
            out = entry;
            return true;
        }
    }
    return false;
}

bool wordChar32(char32_t c) { return c > 0x20 && c != 0x3000; }

}  // namespace

std::vector<BookmarkEntry> bookmarkList(const model::Document& doc) {
    // 문서 차례(한글 책갈피 목록의 "위치" 정렬): 문단을 앞에서부터 읽고, 개체를 만나면 그 안(표 칸·캡션·글상자 등)을
    // 그 자리에서 읽는다.
    std::vector<BookmarkEntry> out;
    const model::StoryMap& map = ensureStoryMap(doc);
    std::function<void(const std::vector<model::Paragraph>&)> walk = [&](const std::vector<model::Paragraph>& paras) {
        const int story = map.storyOf(&paras);
        for (std::size_t p = 0; p < paras.size(); ++p) {
            int charIndex = 0;
            for (const model::Run& run : paras[p].runs) {
                for (const model::InlineItem& item : run.items) {
                    if (item.type == model::InlineItem::Type::Text) {
                        charIndex += utf8CodePointCount(item.text);
                        continue;
                    }
                    const model::Control& c = item.control;
                    if (c.kind == model::ControlKind::Bookmark) {
                        if (story > 0) out.push_back(BookmarkEntry{c.name, story, static_cast<int>(p), charIndex});
                        continue;
                    }
                    if (isCaretCharControl(c)) ++charIndex;
                    if (c.caption) walk(c.caption->content.paragraphs);
                    if (c.table) {
                        for (const model::TableCell& cell : c.table->cells) walk(cell.content.paragraphs);
                    }
                    for (const model::SubList& sub : c.subLists) walk(sub.paragraphs);
                }
            }
        }
    };
    for (const model::Section& section : doc.sections) walk(section.paragraphs);
    return out;
}

std::string bookmarkDefaultName(const model::Document& doc, const std::vector<Node>& pages) {
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    const model::Paragraph* para = storyParagraph(doc, cur.story, cur.paragraph);
    if (!para) return {};
    const std::u32string text = caretCharsOf(*para);
    const int at = std::clamp(offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset), 0,
                              static_cast<int>(text.size()));
    int begin = at;
    int end = at;
    while (end < static_cast<int>(text.size()) && wordChar32(text[static_cast<std::size_t>(end)])) ++end;
    if (end == at) {
        while (begin > 0 && wordChar32(text[static_cast<std::size_t>(begin - 1)])) --begin;
    }
    std::string out;
    for (int i = begin; i < end; ++i) {
        const char32_t c = text[static_cast<std::size_t>(i)];
        if (c >= 0x20) appendUtf8(out, c);
    }
    return out;
}

EditResult bookmarkNow(model::Document& doc, std::vector<Node>& pages, const std::string& action,
                       const std::string& name, bool useStoredLineHeight, std::string& why, const std::string& newName) {
    BookmarkEntry found;
    const bool exists = !name.empty() && findBookmark(doc, name, found);
    if (action == "goto") {
        if (!exists) {
            why = "책갈피 " + name + " 이(가) 없습니다.";
            return {};
        }
        clearComposition(pointerHold());
        setPointerCursor(doc, pages,
                         model::CursorPos{found.story, found.paragraph,
                                          charIndexToOffset(doc, pages, found.story, found.paragraph, found.charIndex)});
        return makeEditResult(doc, true, false, found.story, found.paragraph);
    }
    if (readOnly()) return {};
    if (action == "insert") {
        if (name.empty()) {
            why = "책갈피 이름을 넣으십시오.";
            return {};
        }
        if (exists) {
            why = "같은 이름의 책갈피가 이미 있습니다.";
            return {};
        }
        const PointerCursor cur = pointerCursor();
        if (!cur.active || cur.story <= 0) return {};
        model::Paragraph* host = storyParagraph(doc, cur.story, cur.paragraph);
        if (!host) return {};
        clearComposition(pointerHold());
        const int charIndex = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
        const std::uint32_t shape = charShapeAtParagraphChar(*host, charIndex, false);
        model::Control mark;
        mark.kind = model::ControlKind::Bookmark;
        mark.ctrlId = "bokm";
        mark.name = name;
        model::Paragraph right = splitParagraphAt(*host, charIndex);
        model::Run run;
        run.charShapeId = shape;
        run.items.push_back(model::InlineItem::makeControl(std::move(mark)));
        host->runs.push_back(std::move(run));
        for (model::Run& r : right.runs) {
            if (!r.items.empty()) host->runs.push_back(std::move(r));
        }
    } else if (action == "rename") {
        // 책갈피 이름 바꾸기(한글 RenameBookmark: 새 책갈피 이름). 빈 이름·이미 있는 이름은 거절한다.
        if (!exists) {
            why = "책갈피 " + name + " 이(가) 없습니다.";
            return {};
        }
        if (newName.empty()) {
            why = "책갈피 이름을 넣으십시오.";
            return {};
        }
        if (newName == name) return makeEditResult(doc, true, false, found.story, found.paragraph);
        BookmarkEntry other;
        if (findBookmark(doc, newName, other)) {
            why = "같은 이름의 책갈피가 이미 있습니다.";
            return {};
        }
        std::vector<model::Paragraph>* paras = storyParagraphs(doc, found.story);
        if (!paras) return {};
        bool renamed = false;
        for (model::Run& run : (*paras)[static_cast<std::size_t>(found.paragraph)].runs) {
            for (model::InlineItem& item : run.items) {
                if (!renamed && item.type == model::InlineItem::Type::Control &&
                    item.control.kind == model::ControlKind::Bookmark && item.control.name == name) {
                    item.control.name = newName;
                    renamed = true;
                }
            }
        }
        if (!renamed) return {};
    } else if (action == "delete") {
        if (!exists) {
            why = "책갈피 " + name + " 이(가) 없습니다.";
            return {};
        }
        std::vector<model::Paragraph>* paras = storyParagraphs(doc, found.story);
        if (!paras) return {};
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(found.paragraph)];
        bool removed = false;
        for (model::Run& run : para.runs) {
            for (auto it = run.items.begin(); it != run.items.end(); ++it) {
                if (it->type == model::InlineItem::Type::Control && it->control.kind == model::ControlKind::Bookmark &&
                    it->control.name == name) {
                    run.items.erase(it);
                    removed = true;
                    break;
                }
            }
            if (removed) break;
        }
        if (!removed) return {};
        para.runs.erase(std::remove_if(para.runs.begin(), para.runs.end(),
                                       [&](const model::Run& r) { return r.items.empty() && para.runs.size() > 1; }),
                        para.runs.end());
    } else {
        why = "책갈피 명령 " + action + " 은 모릅니다.";
        return {};
    }
    // 책갈피는 폭이 없어 줄 모양이 그대로다. 캐럿 글자 번호를 지켜 다시 놓는다.
    const PointerCursor before = pointerCursor();
    const int beforeChar = before.active && before.story > 0
                               ? offsetToCharIndex(doc, pages, before.story, before.paragraph, before.offset)
                               : 0;
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    if (before.active && before.story > 0 && resolveStory(doc, before.story)) {
        setPointerCursor(doc, pages,
                         model::CursorPos{before.story, before.paragraph,
                                          charIndexToOffset(doc, pages, before.story, before.paragraph, beforeChar)});
    }
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

// 구역 찾아가기(한글 찾아가기 구역: 구역 번호를 넣고 가기를 누르면 커서가 옮겨진다). 그 구역 본문 맨 앞이다.
EditResult goToSectionNow(model::Document& doc, std::vector<Node>& pages, int section, std::string& why) {
    const model::StoryMap& map = ensureStoryMap(doc);
    for (int s = 1; s <= map.count(); ++s) {
        const model::StorySlot* slot = map.slot(s);
        if (!slot || slot->kind != model::StoryKind::Section || slot->sectionIndex != section) continue;
        clearComposition(pointerHold());
        setPointerCursor(doc, pages, model::CursorPos{s, 0, 0});
        return makeEditResult(doc, true, false, s, 0);
    }
    why = "구역 " + std::to_string(section + 1) + " 은(는) 없습니다.";
    return {};
}

EditResult goToPageNow(model::Document& doc, std::vector<Node>& pages, int page, std::string& why) {
    if (page < 0 || page >= static_cast<int>(pages.size())) {
        why = "쪽 " + std::to_string(page + 1) + " 은(는) 없습니다.";
        return {};
    }
    // 그 쪽 본문 맨 위 줄의 맨 앞(한글 찾아가기: 쪽 처음).
    const model::StoryMap& map = ensureStoryMap(doc);
    for (int s = 1; s <= map.count(); ++s) {
        const model::StorySlot* slot = map.slot(s);
        if (!slot || slot->kind != model::StoryKind::Section) continue;
        const CaretPlace place = caretInStory(doc, pages, page, 0, 0, s);
        if (!place.hit || place.page != page) continue;
        clearComposition(pointerHold());
        setPointerCursor(doc, pages, model::CursorPos{place.story, place.paragraph, place.offset});
        return makeEditResult(doc, true, false, place.story, place.paragraph);
    }
    why = "쪽 " + std::to_string(page + 1) + " 에 본문이 없습니다.";
    return {};
}

}  // namespace chwp::render
