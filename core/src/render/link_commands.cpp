// 하이퍼링크 따라가기(한글 2024, 보이는 한글 화면에서 잰 동작):
// - 링크 글을 누르면(끌지 않은 한 번 누르기) 따라간다. 링크 위에서는 손 모양 커서다.
// - 명령 "대상;형식;종류;열기;" 의 대상이 "?#id" 면 문서 안 개체다. 종류 1 은 개요 문단(문단 id), 2 표, 3 그림, 4 수식
//   (개체 id)이다. "?이름" 은 책갈피, 빈 대상은 문서의 처음, 형식 1 은 웹 주소(\ 를 푼 글)다.
// - 개요는 그 문단 맨 앞, 개체는 그 개체 바로 앞(개체를 고르지 않는다), 책갈피는 그 자리, 문서의 처음은 본문 첫 자리로
//   캐럿이 간다. 화면은 대상 줄을 가운데에 둔다(셸 카메라는 코어 세션이 옮긴다).
// - 따라간 링크 글은 글자·밑줄 색이 #800080 으로 바뀌어 저장된다. 이 바꿈은 되돌리기 한 칸이다.
// - 이전(HyperlinkPrev, Ctrl+Q,B)은 따라오기 전 링크의 맨 앞으로, 다음(HyperlinkNext, Ctrl+Q,R)은 다시 그 대상으로 간다.
//   이력은 브라우저처럼 따라간 차례이고, 이전으로 간 뒤 새로 따라가면 뒤쪽 이력은 버린다.

#include "chwp/render/link_commands.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

// ; 로 나눈다. \ 뒤 글자는 그대로 둔다(\ 는 뺀다).
std::vector<std::string> splitCommand(const std::string& command) {
    std::vector<std::string> out(1);
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '\\' && i + 1 < command.size()) {
            out.back() += command[++i];
        } else if (c == ';') {
            out.emplace_back();
        } else {
            out.back() += c;
        }
    }
    return out;
}

struct Spot {
    bool found = false;
    int story = 0;
    int paragraph = 0;
    int charIndex = 0;
};

// 문서 안 대상 자리. 개요 문단(문단 id)·개체(개체 id).
Spot findTarget(const model::Document& doc, const LinkTarget& target) {
    Spot out;
    const model::StoryMap& map = ensureStoryMap(doc);
    std::function<void(const std::vector<model::Paragraph>&)> walk = [&](const std::vector<model::Paragraph>& paras) {
        const int story = map.storyOf(&paras);
        for (std::size_t p = 0; p < paras.size() && !out.found; ++p) {
            if (target.kind == LinkTarget::Kind::Outline && paras[p].fileId == target.id && story > 0) {
                out = Spot{true, story, static_cast<int>(p), 0};
                return;
            }
            int charIndex = 0;
            for (const model::Run& run : paras[p].runs) {
                for (const model::InlineItem& item : run.items) {
                    if (out.found) return;
                    if (item.type == model::InlineItem::Type::Text) {
                        charIndex += static_cast<int>(utf8CodePointCount(item.text));
                        continue;
                    }
                    const model::Control& c = item.control;
                    if (target.kind != LinkTarget::Kind::Outline && c.placement && c.placement->instanceId == target.id &&
                        story > 0) {
                        out = Spot{true, story, static_cast<int>(p), charIndex};
                        return;
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
    for (const model::Section& section : doc.sections) {
        if (!out.found) walk(section.paragraphs);
    }
    return out;
}

Spot resolve(const model::Document& doc, const LinkTarget& target) {
    switch (target.kind) {
        case LinkTarget::Kind::DocumentStart: {
            if (doc.sections.empty()) return {};
            const int story = ensureStoryMap(doc).storyOf(&doc.sections.front().paragraphs);
            return story > 0 ? Spot{true, story, 0, 0} : Spot{};
        }
        case LinkTarget::Kind::Bookmark:
            for (const BookmarkEntry& entry : bookmarkList(doc)) {
                if (entry.name == target.name) return Spot{true, entry.story, entry.paragraph, entry.charIndex};
            }
            return {};
        case LinkTarget::Kind::Outline:
        case LinkTarget::Kind::Table:
        case LinkTarget::Kind::Figure:
        case LinkTarget::Kind::Equation:
            return findTarget(doc, target);
        default:
            return {};
    }
}

// 자리로 캐럿을 옮긴다. 문서가 바뀌어 자리가 없으면 그 글 묶음 안으로 줄인다.
bool moveCaretTo(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int charIndex) {
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || paras->empty()) return false;
    paragraph = std::clamp(paragraph, 0, static_cast<int>(paras->size()) - 1);
    charIndex = std::clamp(charIndex, 0, paragraphCharCount((*paras)[static_cast<std::size_t>(paragraph)]));
    PointerHold& hold = pointerHold();
    if (hold.composing) clearComposition(hold);
    hold.preferredX = -1;
    setPointerCursor(doc, pages, model::CursorPos{story, paragraph, charIndexToOffset(doc, pages, story, paragraph, charIndex)});
    return true;
}

}  // namespace

LinkTarget parseLinkCommand(const std::string& command) {
    LinkTarget out;
    const std::vector<std::string> parts = splitCommand(command);
    const std::string& where = parts[0];
    const std::string form = parts.size() > 1 ? parts[1] : std::string();
    const int kind = parts.size() > 2 && !parts[2].empty() ? std::atoi(parts[2].c_str()) : 0;
    if (form == "1") {
        out.kind = where.empty() ? LinkTarget::Kind::None : LinkTarget::Kind::Web;
        out.url = where;
        return out;
    }
    if (where.empty()) {
        out.kind = LinkTarget::Kind::DocumentStart;
        return out;
    }
    if (where[0] != '?') {
        out.kind = LinkTarget::Kind::OtherFile;
        out.url = where;
        return out;
    }
    const std::string rest = where.substr(1);
    if (!rest.empty() && rest[0] == '#') {
        out.id = static_cast<std::uint32_t>(std::strtoul(rest.c_str() + 1, nullptr, 10));
        out.kind = kind == 2   ? LinkTarget::Kind::Table
                   : kind == 3 ? LinkTarget::Kind::Figure
                   : kind == 4 ? LinkTarget::Kind::Equation
                               : LinkTarget::Kind::Outline;
        return out;
    }
    out.kind = LinkTarget::Kind::Bookmark;
    out.name = rest;
    return out;
}

bool linkUnder(const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y) {
    return linkSpotAt(doc, pages, page, x, y).found;
}

EditResult paint_detail::followLinkNow(model::Document& doc, std::vector<Node>& pages, int page, int x, int y,
                                       bool useStoredLineHeight, std::string& url, std::string& why) {
    const LinkSpot spot = linkSpotAt(doc, pages, page, x, y);
    if (!spot.found) return {};
    model::Paragraph* para = storyParagraph(doc, spot.story, spot.paragraph);
    if (!para) return {};
    int begin = 0;
    int end = 0;
    const model::Control* field = hyperlinkAround(*para, spot.charIndex, begin, end);
    if (!field) return {};
    const LinkTarget target = parseLinkCommand(field->command);
    Spot to;
    if (target.kind == LinkTarget::Kind::Web) {
        url = target.url;
    } else if (target.kind == LinkTarget::Kind::OtherFile) {
        why = "다른 파일로 가는 하이퍼링크는 아직 열 수 없습니다.";
        return {};
    } else {
        to = resolve(doc, target);
        if (!to.found) {
            why = target.kind == LinkTarget::Kind::Bookmark ? "책갈피 " + target.name + " 이(가) 없습니다."
                                                            : std::string("하이퍼링크가 가리키는 곳이 문서에 없습니다.");
            return {};
        }
    }
    // 방문한 링크 색. 읽기 전용이면 문서를 바꾸지 않는다.
    const bool changed = !readOnly() && markLinkVisited(doc, *para, begin, end);
    EditRelayoutResult relayout;
    if (changed) {
        relayout = relayoutAfterEdit(doc, pages,
                                     EditRelayoutSpec{spot.story, spot.paragraph, 0, begin, end, end, useStoredLineHeight});
    }
    if (!to.found) return makeEditResult(doc, true, changed, spot.story, spot.paragraph, changed ? &relayout : nullptr);
    PointerHold& hold = pointerHold();
    hold.linkJumps.resize(std::min(hold.linkJumpAt, hold.linkJumps.size()));
    hold.linkJumps.push_back(LinkJump{spot.story, spot.paragraph, begin, to.story, to.paragraph, to.charIndex});
    hold.linkJumpAt = hold.linkJumps.size();
    moveCaretTo(doc, pages, to.story, to.paragraph, to.charIndex);
    return makeEditResult(doc, true, changed, changed ? spot.story : to.story, changed ? spot.paragraph : to.paragraph,
                          changed ? &relayout : nullptr);
}

EditResult editLinkHistory(model::Document& doc, std::vector<Node>& pages, bool forward) {
    PointerHold& hold = pointerHold();
    if (forward) {
        if (hold.linkJumpAt >= hold.linkJumps.size()) return {};
        const LinkJump jump = hold.linkJumps[hold.linkJumpAt];
        if (!moveCaretTo(doc, pages, jump.toStory, jump.toPara, jump.toChar)) return {};
        ++hold.linkJumpAt;
        return makeEditResult(doc, true, false, jump.toStory, jump.toPara);
    }
    if (hold.linkJumpAt == 0 || hold.linkJumps.empty()) return {};
    const LinkJump jump = hold.linkJumps[--hold.linkJumpAt];
    if (!moveCaretTo(doc, pages, jump.fromStory, jump.fromPara, jump.fromChar)) return {};
    return makeEditResult(doc, true, false, jump.fromStory, jump.fromPara);
}

}  // namespace chwp::render
