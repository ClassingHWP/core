// 문서 글 찾기. 편집기 찾기·바꾸기와 에이전트 find 가 같이 쓴다.

#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <string>
#include <utility>
#include <vector>

namespace chwp::render {

namespace {

std::string lowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// 읽는 차례 열쇠. 구역 본문을 문단 차례로 읽고, 문단 안 개체(표 칸·글상자·각주)의 글은 그 문단 글 뒤에 읽는다.
// 한 문단에 개체가 여럿이면 story 번호 차례다.
std::vector<int> storyOrderKey(const model::StoryMap& map, int story) {
    std::vector<int> reversed;
    int s = story;
    for (int depth = 0; depth < 64; ++depth) {
        reversed.push_back(s);
        const model::StorySlot* slot = map.slot(s);
        if (!slot || slot->parentStory <= 0 || slot->parentParagraph < 0) break;
        reversed.push_back(INT_MAX);
        reversed.push_back(slot->parentParagraph);
        s = slot->parentStory;
    }
    return {reversed.rbegin(), reversed.rend()};
}

std::vector<int> positionKey(const model::StoryMap& map, int story, int paragraph, int index) {
    std::vector<int> key = storyOrderKey(map, story);
    key.push_back(paragraph);
    key.push_back(index);
    return key;
}

int canonicalStory(const model::StoryMap& map, int story) {
    const model::StorySlot* slot = map.slot(story);
    return slot && slot->paragraphs ? map.storyOf(slot->paragraphs) : story;
}

}  // namespace

namespace paint_detail {

bool searchTextEqual(const std::string& a, const std::string& b, bool matchCase) {
    return matchCase ? a == b : lowerAscii(a) == lowerAscii(b);
}

}  // namespace paint_detail

std::string paragraphSearchText(const model::Paragraph& para) {
    std::string text;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                text += item.text;
            } else if (paint_detail::isCaretCharControl(item.control)) {
                text += "\xEF\xBF\xBC";  // 글자처럼 놓인 개체는 U+FFFC 한 글자다
            }
        }
    }
    return text;
}

std::vector<TextHit> findText(const model::Document& doc, const std::string& text, bool matchCase, int story,
                              int limit) {
    std::vector<TextHit> out;
    if (text.empty()) return out;
    const std::string needle = matchCase ? text : lowerAscii(text);
    int needleLength = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++needleLength;
    }
    const model::StoryMap& map = ensureStoryMap(doc);
    for (int s = 1; s <= map.count(); ++s) {
        if (story != 0 && s != story) continue;
        const model::StorySlot* slot = map.slot(s);
        // 여러 쪽에 찍히는 글(머리말 등)은 원래 번호 하나만 본다.
        if (!slot || !slot->paragraphs || map.storyOf(slot->paragraphs) != s) continue;
        const std::vector<model::Paragraph>& paras = *slot->paragraphs;
        for (int p = 0; p < static_cast<int>(paras.size()); ++p) {
            const std::string body = paragraphSearchText(paras[static_cast<std::size_t>(p)]);
            const std::string hay = matchCase ? body : lowerAscii(body);
            std::vector<std::size_t> starts;
            for (std::size_t i = 0; i < body.size(); ++i) {
                if ((static_cast<unsigned char>(body[i]) & 0xC0) != 0x80) starts.push_back(i);
            }
            for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) {
                const auto hit = std::lower_bound(starts.begin(), starts.end(), at);
                if (hit == starts.end() || *hit != at) continue;  // 글자 가운데에서 맞은 것은 버린다
                const int index = static_cast<int>(hit - starts.begin());
                out.push_back(TextHit{s, p, index, index + needleLength});
                if (limit > 0 && static_cast<int>(out.size()) >= limit) return out;
            }
        }
    }
    return out;
}

EditResult selectNextText(const model::Document& doc, const std::vector<Node>& pages, const std::string& text,
                          bool matchCase, bool backward, bool* wrapped) {
    if (wrapped) *wrapped = false;
    std::vector<TextHit> hits = findText(doc, text, matchCase);
    if (hits.empty()) return {};
    const model::StoryMap& map = ensureStoryMap(doc);
    std::vector<std::pair<std::vector<int>, std::size_t>> order;
    order.reserve(hits.size());
    for (std::size_t i = 0; i < hits.size(); ++i) {
        order.emplace_back(positionKey(map, hits[i].story, hits[i].paragraph, hits[i].from), i);
    }
    std::sort(order.begin(), order.end());

    // 앞으로 찾으면 고른 글(없으면 캐럿) 끝에서, 뒤로 찾으면 그 앞에서 시작한다.
    // start 는 시작 자리 이상인 첫 찾은 곳의 차례다. 앞으로는 거기부터, 뒤로는 그 앞부터 본다.
    const std::size_t n = order.size();
    std::size_t start = backward ? n : 0;
    bool fromCursor = false;
    const PointerCursor cur = pointerCursor();
    if (cur.active && cur.story > 0) {
        const int para = backward ? (cur.hasSelection ? cur.startParagraph : cur.paragraph)
                                  : (cur.hasSelection ? cur.endParagraph : cur.paragraph);
        const int off = backward ? (cur.hasSelection ? cur.startOffset : cur.offset)
                                 : (cur.hasSelection ? cur.endOffset : cur.offset);
        const int index = paint_detail::offsetToCharIndex(doc, pages, cur.story, para, off);
        const std::vector<int> at = positionKey(map, canonicalStory(map, cur.story), para, index);
        start = static_cast<std::size_t>(
            std::lower_bound(order.begin(), order.end(), at,
                             [](const std::pair<std::vector<int>, std::size_t>& e, const std::vector<int>& k) {
                                 return e.first < k;
                             }) -
            order.begin());
        fromCursor = true;
    }
    // 끝에 닿으면 처음(뒤로 찾으면 끝)부터 돈다. 쪽에 놓이지 않은 글은 캐럿을 놓지 못하니 건너뛴다.
    for (std::size_t step = 0; step < n; ++step) {
        std::size_t slot = 0;
        bool around = false;
        if (backward) {
            around = step >= start;
            slot = around ? n - 1 - (step - start) : start - 1 - step;
        } else {
            around = start + step >= n;
            slot = (start + step) % n;
        }
        const TextHit& hit = hits[order[slot].second];
        const int from = paint_detail::charIndexToOffset(doc, pages, hit.story, hit.paragraph, hit.from);
        const int to = paint_detail::charIndexToOffset(doc, pages, hit.story, hit.paragraph, hit.to);
        setPointerCursor(doc, pages, model::CursorPos{hit.story, hit.paragraph, from},
                         model::CursorPos{hit.story, hit.paragraph, to});
        const PointerCursor now = pointerCursor();
        if (!now.active || canonicalStory(map, now.story) != hit.story) continue;
        if (wrapped) *wrapped = fromCursor && around;
        return paint_detail::makeEditResult(doc, true, false, now.story, hit.paragraph);
    }
    return {};
}

}  // namespace chwp::render
