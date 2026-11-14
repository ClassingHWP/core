// 찾아가기의 줄·스타일·조판 부호(한글 2024 편집 → 찾기 → 찾아가기). 쪽·구역·책갈피는
// bookmark_commands.cpp 에 있다. 규칙은 한글 2024 화면에서 같은 문서로 해 본 결과다.
//   줄: 번호는 문서 본문 줄을 처음부터 이어 센다(쪽마다 다시 세지 않는다). 표는 그 표가 놓인 줄 하나로 세고, 표 칸·각주·
//     머리말 줄은 세지 않는다. 목록은 첫 줄과 마지막 줄 번호이고, 마지막보다 큰 번호는 마지막 줄로 간다. 캐럿은 줄 처음이다.
//   스타일: 목록은 문서 스타일이다. 캐럿 문단 다음 문단부터 그 스타일 문단을 찾고, 문서 끝을 넘으면 처음부터 찾는다.
//     캐럿은 그 문단 처음이다.
//   조판 부호: 목록은 한글의 조판 부호 이름이다. 캐럿 자리(그 자리의 것도 넣어)부터 문서 차례로 그 종류를 찾고, 끝을
//     넘으면 처음부터 찾는다. 각주·표 칸·캡션 같은 안쪽 글도 그 컨트롤 자리에서 뒤진다. 캐럿은 그 조판 부호 바로 앞이다.
//     없으면 "찾는 내용이 없습니다."
// 스타일 목록의 글자 스타일, 여러 구역 문서의 줄 번호는 한글로 확인하지 않았다. 스타일 목록은 문단 스타일만 둔다.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
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
std::string textOf(const FormatFields& fields, const std::string& key) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const std::string* t = std::get_if<std::string>(v)) return *t;
    }
    return {};
}
double numberOf(const FormatFields& fields, const std::string& key, double fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const double* d = std::get_if<double>(v)) return *d;
    }
    return fallback;
}

// 본문 줄(구역 글 묶음의 줄) 차례.
struct BodyLine {
    int story = -1;
    int paragraph = -1;
    int start = 0;
};

std::vector<BodyLine> bodyLines(const model::Document& doc, const std::vector<Node>& pages) {
    const model::StoryMap& map = ensureStoryMap(doc);
    std::vector<BodyLine> out;
    // 줄 노드의 글 묶음·문단 번호가 비었으면 감싼 문단 노드의 값이다(편집 뒤 다시 놓은 줄).
    const auto walk = [&](const auto& self, const Node& node, int story, int paragraph) -> void {
        if (node.pageBand || node.fromMaster || node.role == "lineNumbers" || node.role == "memos") return;
        if (node.kind == NodeKind::Paragraph) {
            if (node.story > 0) story = node.story;
            if (node.paragraph >= 0) paragraph = node.paragraph;
        }
        if (node.kind == NodeKind::Line) {
            const int s = node.story > 0 ? node.story : story;
            const int p = node.paragraph >= 0 ? node.paragraph : paragraph;
            const model::StorySlot* slot = s > 0 ? map.slot(s) : nullptr;
            if (slot && slot->kind == model::StoryKind::Section && p >= 0) out.push_back(BodyLine{s, p, std::max(0, node.start)});
            return;  // 줄 안 개체(글자처럼 놓은 표 등)의 줄은 세지 않는다
        }
        for (const Node& child : node.children) self(self, child, story, paragraph);
    };
    for (const Node& page : pages) walk(walk, page, -1, -1);
    return out;
}

// 한글 조판 부호 이름(Hwp.String 의 찾아가기·조판 부호 목록 글).
const char* const kAllMarks = "[모든 조판 부호-탭,줄,문단 등 제외]";
bool autoNumberOf(const model::Control& c, const char* numType) {
    return c.kind == model::ControlKind::AutoNumber && c.numType == numType;
}
bool newNumberOf(const model::Control& c, const char* numType) {
    return c.kind == model::ControlKind::NewNumber && c.numType == numType;
}
bool fieldOfId(const model::Control& c, const char* id) {
    return c.kind == model::ControlKind::Field && c.ctrlId == id;
}

// 이름이 그 컨트롤을 가리키는지.
bool markMatches(const std::string& name, const model::Control& c) {
    using K = model::ControlKind;
    if (name == kAllMarks) return c.kind != K::Unknown && c.kind != K::FieldEnd;
    if (name == "표") return c.kind == K::Table;
    if (name == "그림") return c.kind == K::Picture;
    if (name == "그리기") return c.kind == K::Container || (c.kind == K::Shape && c.subLists.empty());
    if (name == "글상자") return c.kind == K::Shape && !c.subLists.empty();
    if (name == "수식") return c.kind == K::Equation;
    if (name == "OLE 개체") return c.kind == K::Ole;
    if (name == "머리말") return c.kind == K::Header;
    if (name == "꼬리말") return c.kind == K::Footer;
    if (name == "각주") return c.kind == K::Footnote;
    if (name == "미주") return c.kind == K::Endnote;
    if (name == "덧말") return c.kind == K::Dutmal;
    if (name == "글자 겹치기") return c.kind == K::Compose;
    if (name == "책갈피") return c.kind == K::Bookmark;
    if (name == "숨은 설명") return c.kind == K::HiddenComment;
    if (name == "감추기") return c.kind == K::PageHiding;
    if (name == "쪽 번호 위치") return c.kind == K::PageNumPos;
    if (name == "새 단 정의") return c.kind == K::ColumnDef;
    if (name == "구역 시작") return c.kind == K::SectionDef;
    if (name == "쪽 번호") return autoNumberOf(c, "PAGE");
    if (name == "각주 번호") return autoNumberOf(c, "FOOTNOTE");
    if (name == "미주 번호") return autoNumberOf(c, "ENDNOTE");
    if (name == "그림 번호") return autoNumberOf(c, "PICTURE");
    if (name == "표 번호") return autoNumberOf(c, "TABLE");
    if (name == "수식 번호") return autoNumberOf(c, "EQUATION");
    if (name == "전체 쪽수") return autoNumberOf(c, "TOTAL_PAGE");
    if (name == "새 쪽 번호") return newNumberOf(c, "PAGE");
    if (name == "새 각주 번호") return newNumberOf(c, "FOOTNOTE");
    if (name == "새 미주 번호") return newNumberOf(c, "ENDNOTE");
    if (name == "새 그림 번호") return newNumberOf(c, "PICTURE");
    if (name == "새 표 번호") return newNumberOf(c, "TABLE");
    if (name == "새 수식 번호") return newNumberOf(c, "EQUATION");
    if (name == "하이퍼링크") return fieldOfId(c, "%hlk");
    if (name == "누름틀") return fieldOfId(c, "%clk");
    if (name == "상호 참조") return fieldOfId(c, "%xrf");
    if (name == "현재 날짜") return fieldOfId(c, "%dte");
    if (name == "만든 날짜") return fieldOfId(c, "%ddt");
    if (name == "파일 경로") return fieldOfId(c, "%pat");
    if (name == "문서 요약") return fieldOfId(c, "%smr");
    if (name == "사용자 정보") return fieldOfId(c, "%usr");
    if (name == "메일 머지") return fieldOfId(c, "%mmg");
    if (name == "계산식") return fieldOfId(c, "%fmu");
    if (name == "책갈피 영역") return fieldOfId(c, "%bmk");
    return false;
}

// 글자로 된 조판 부호.
bool markCharMatches(const std::string& name, char32_t ch) {
    if (name == "탭") return ch == U'\t';
    if (name == "강제 줄 나눔") return ch == U'\n';
    if (name == "묶음 빈 칸") return ch == 0xA0;
    if (name == "고정폭 빈 칸") return ch == 0x2007;
    return false;
}

// 찾아가기 목록에 두는 이름. 한글 목록 글 가운데 이 문서 모델이 가릴 수 있는 것이다.
const std::vector<std::string>& markNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v = {
            "OLE 개체", "각주", "각주 번호", "감추기", "강제 줄 나눔", "계산식", "고정폭 빈 칸", "구역 시작", "그리기", "그림",
            "그림 번호", "글상자", "글자 겹치기", "꼬리말", "누름틀", "덧말", "만든 날짜", "머리말", "메일 머지", "묶음 빈 칸",
            "문서 요약", "미주", "미주 번호", "사용자 정보", "상호 참조", "새 각주 번호", "새 그림 번호", "새 단 정의",
            "새 미주 번호", "새 수식 번호", "새 쪽 번호", "새 표 번호", "수식", "수식 번호", "숨은 설명", "전체 쪽수", "쪽 번호",
            "쪽 번호 위치", "책갈피", "책갈피 영역", "탭", "파일 경로", "표", "표 번호", "하이퍼링크", "현재 날짜"};
        v.insert(v.begin(), kAllMarks);
        return v;
    }();
    return names;
}

std::u32string charsOf(const std::string& text) {
    std::u32string out;
    for (const unsigned char c : text) {
        if ((c & 0xC0) == 0x80 && !out.empty()) out.back() = (out.back() << 6) | (c & 0x3F);
        else if (c >= 0xF0) out.push_back(c & 0x07);
        else if (c >= 0xE0) out.push_back(c & 0x0F);
        else if (c >= 0xC0) out.push_back(c & 0x1F);
        else out.push_back(c);
    }
    return out;
}

// 문서 차례로 조판 부호를 뒤진다. 컨트롤 안쪽 글(각주·표 칸·캡션·글상자)은 그 컨트롤 바로 뒤에 뒤진다.
struct MarkHit {
    int story = -1;
    int paragraph = -1;
    int charIndex = 0;
    long seq = 0;
};
struct MarkWalk {
    const model::StoryMap& map;
    const std::string& name;
    int caretStory;
    int caretParagraph;
    int caretChar;
    long seq = 0;
    long caretSeq = -1;
    std::vector<MarkHit> hits;

    void at(int story, int paragraph, int index) {
        if (caretSeq < 0 && story == caretStory && paragraph == caretParagraph && index >= caretChar) caretSeq = seq;
    }
    void paragraphs(const std::vector<model::Paragraph>& paras) {
        const int story = map.storyOf(&paras);
        for (std::size_t p = 0; p < paras.size(); ++p) {
            const int pi = static_cast<int>(p);
            int index = 0;
            for (const model::Run& run : paras[p].runs) {
                for (const model::InlineItem& item : run.items) {
                    if (item.type == model::InlineItem::Type::Text) {
                        for (const char32_t ch : charsOf(item.text)) {
                            at(story, pi, index);
                            if (markCharMatches(name, ch)) hits.push_back(MarkHit{story, pi, index, seq});
                            ++seq;
                            ++index;
                        }
                        continue;
                    }
                    at(story, pi, index);
                    if (markMatches(name, item.control)) hits.push_back(MarkHit{story, pi, index, seq});
                    ++seq;
                    inside(item.control);
                    if (isCaretCharControl(item.control)) ++index;
                }
            }
            at(story, pi, index);
            ++seq;
        }
    }
    void inside(const model::Control& c) {
        for (const model::SubList& list : c.subLists) paragraphs(list.paragraphs);
        if (c.table) {
            for (const model::TableCell& cell : c.table->cells) paragraphs(cell.content.paragraphs);
        }
        if (c.caption) paragraphs(c.caption->content.paragraphs);
    }
};

// 캐럿이 든 본문 문단(표 칸·각주 안이면 그것을 품은 본문 문단).
bool bodyParagraphOf(const model::Document& doc, int story, int paragraph, int& section, int& bodyParagraph) {
    for (const model::StorySlot* slot = resolveStory(doc, story); slot; slot = resolveStory(doc, slot->parentStory)) {
        if (slot->kind == model::StoryKind::Section) {
            section = slot->sectionIndex;
            bodyParagraph = paragraph;
            return section >= 0;
        }
        paragraph = slot->parentParagraph;
        if (slot->parentStory <= 0) break;
    }
    return false;
}

// 조판 부호 지우기: 문단 [from, to) 글자 자리(to 가 -1 이면 끝까지)의 고른 종류를 지운다. 안쪽 글(표 칸·글상자 등)은
// 그 컨트롤이 남으면 그 안 전체를 뒤진다. 하이퍼링크·누름틀 같은 필드는 그 필드 끝도 같이 뺀다. 지운 수를 돌려준다.
int eraseMarks(std::vector<model::Paragraph>& paras, int firstPara, int fromChar, int lastPara, int toChar,
               const std::vector<std::string>& names) {
    const auto wanted = [&](const model::Control& c) {
        return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return markMatches(n, c); });
    };
    const auto wantedChar = [&](char32_t ch) {
        return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return markCharMatches(n, ch); });
    };
    int erased = 0;
    for (int p = std::max(0, firstPara); p <= lastPara && p < static_cast<int>(paras.size()); ++p) {
        model::Paragraph& para = paras[static_cast<std::size_t>(p)];
        const int lo = p == firstPara ? fromChar : 0;
        const int hi = p == lastPara && toChar >= 0 ? toChar : 1 << 30;
        int index = 0;
        int openFields = 0;  // 지운 필드 시작 수. 그만큼 뒤의 필드 끝도 뺀다
        for (model::Run& run : para.runs) {
            std::vector<model::InlineItem> kept;
            for (model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Text) {
                    std::string text;
                    for (const char32_t ch : charsOf(item.text)) {
                        if (index >= lo && index < hi && wantedChar(ch)) {
                            ++erased;
                        } else {
                            appendUtf8(text, ch);
                        }
                        ++index;
                    }
                    item.text = std::move(text);
                    if (!item.text.empty()) kept.push_back(std::move(item));
                    continue;
                }
                const bool inside = index >= lo && index < hi;
                if (isCaretCharControl(item.control)) ++index;
                if (item.control.kind == model::ControlKind::FieldEnd && openFields > 0) {
                    --openFields;
                    continue;
                }
                if (inside && wanted(item.control)) {
                    ++erased;
                    if (item.control.kind == model::ControlKind::Field) ++openFields;
                    continue;
                }
                if (inside) {
                    for (model::SubList& sub : item.control.subLists) {
                        erased += eraseMarks(sub.paragraphs, 0, 0, static_cast<int>(sub.paragraphs.size()) - 1, -1, names);
                    }
                    if (item.control.table) {
                        for (model::TableCell& cell : item.control.table->cells) {
                            erased += eraseMarks(cell.content.paragraphs, 0, 0,
                                                 static_cast<int>(cell.content.paragraphs.size()) - 1, -1, names);
                        }
                    }
                    if (item.control.caption) {
                        auto& cap = item.control.caption->content.paragraphs;
                        erased += eraseMarks(cap, 0, 0, static_cast<int>(cap.size()) - 1, -1, names);
                    }
                }
                kept.push_back(std::move(item));
            }
            run.items = std::move(kept);
        }
        para.runs.erase(std::remove_if(para.runs.begin(), para.runs.end(),
                                       [&](const model::Run& r) { return r.items.empty() && para.runs.size() > 1; }),
                        para.runs.end());
        para.lineSegs.clear();
    }
    return erased;
}

EditResult moveCaret(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int charIndex) {
    clearComposition(pointerHold());
    pointerHold().preferredX = -1;
    setPointerCursor(doc, pages, model::CursorPos{story, paragraph, charIndexToOffset(doc, pages, story, paragraph, charIndex)});
    return makeEditResult(doc, true, false, story, paragraph);
}

}  // namespace

FormatFields goToView(const model::Document& doc, const std::vector<Node>& pages) {
    FormatFields out;
    out.emplace_back("lineCount", static_cast<double>(bodyLines(doc, pages).size()));
    std::string styles;
    for (const model::Style& style : doc.header.styles) {
        if (style.type == "CHAR") continue;
        if (!styles.empty()) styles += "\n";
        styles += std::to_string(style.id) + "\t" + style.name;
    }
    out.emplace_back("styles", styles);
    std::string marks;
    for (const std::string& name : markNames()) {
        if (!marks.empty()) marks += "\n";
        marks += name;
    }
    out.emplace_back("marks", marks);
    return out;
}

EditResult goToNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, std::string& why) {
    const std::string kind = textOf(fields, "kind");
    if (kind == "line") {
        const std::vector<BodyLine> lines = bodyLines(doc, pages);
        const long line = std::lround(numberOf(fields, "line", 1));
        if (lines.empty() || line < 1) {
            why = "찾아갈 줄이 없습니다.";
            return {};
        }
        const BodyLine& to = lines[static_cast<std::size_t>(std::min<long>(line, static_cast<long>(lines.size())) - 1)];
        clearComposition(pointerHold());
        pointerHold().preferredX = -1;
        setPointerCursor(doc, pages, model::CursorPos{to.story, to.paragraph, to.start});
        return makeEditResult(doc, true, false, to.story, to.paragraph);
    }
    const PointerCursor cur = pointerCursor();
    if (kind == "style") {
        const long styleId = std::lround(numberOf(fields, "style", -1));
        int section = 0;
        int paragraph = -1;
        if (!(cur.active && cur.story > 0 && bodyParagraphOf(doc, cur.story, cur.paragraph, section, paragraph))) {
            section = 0;
            paragraph = -1;
        }
        // 다음 문단부터 끝까지, 그다음 처음부터 캐럿 문단까지.
        std::vector<std::pair<int, int>> order;
        for (int s = 0; s < static_cast<int>(doc.sections.size()); ++s) {
            for (int p = 0; p < static_cast<int>(doc.sections[static_cast<std::size_t>(s)].paragraphs.size()); ++p) order.emplace_back(s, p);
        }
        const auto here = std::find(order.begin(), order.end(), std::make_pair(section, paragraph));
        const std::size_t first = here == order.end() ? 0 : static_cast<std::size_t>(here - order.begin()) + 1;
        const model::StoryMap& map = ensureStoryMap(doc);
        for (std::size_t k = 0; k < order.size(); ++k) {
            const auto [s, p] = order[(first + k) % order.size()];
            const model::Section& sec = doc.sections[static_cast<std::size_t>(s)];
            if (static_cast<long>(sec.paragraphs[static_cast<std::size_t>(p)].styleId) != styleId) continue;
            return moveCaret(doc, pages, map.storyOf(&sec.paragraphs), p, 0);
        }
        why = "찾는 내용이 없습니다.";
        return {};
    }
    if (kind == "mark") {
        const std::string name = textOf(fields, "mark");
        const model::StoryMap& map = ensureStoryMap(doc);
        const int caretChar = cur.active && cur.story > 0 ? offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset) : 0;
        MarkWalk walk{map, name, cur.active ? cur.story : -1, cur.paragraph, caretChar};
        for (const model::Section& sec : doc.sections) walk.paragraphs(sec.paragraphs);
        if (walk.hits.empty()) {
            why = "찾는 내용이 없습니다.";
            return {};
        }
        const MarkHit* pick = &walk.hits.front();
        for (const MarkHit& hit : walk.hits) {
            if (walk.caretSeq >= 0 && hit.seq >= walk.caretSeq) {
                pick = &hit;
                break;
            }
        }
        return moveCaret(doc, pages, pick->story, pick->paragraph, pick->charIndex);
    }
    why = "찾아갈 곳(line·style·mark)을 모릅니다.";
    return {};
}

// 조판 부호 지우기(한글 2024 DeleteCtrls, 화면으로 확인): 목록은 늘 모든 종류이고 처음엔 모두 꺼져 있다.
// 블록이 있으면 블록 안, 없으면 문서 전체에서 고른 종류를 지운다. 각주를 지우면 그 글도 같이 없어진다.
EditResult deleteMarksNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    std::vector<std::string> names;
    std::string list = textOf(fields, "marks");
    for (std::size_t at = 0; at <= list.size();) {
        const std::size_t cut = std::min(list.find('\n', at), list.size());
        if (cut > at) names.push_back(list.substr(at, cut - at));
        at = cut + 1;
    }
    if (names.empty()) {
        why = "지울 조판 부호를 고르십시오.";
        return {};
    }
    const PointerCursor cur = pointerCursor();
    int erased = 0;
    if (cur.active && cur.story > 0 && cur.hasSelection) {
        std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
        if (!paras) return {};
        const int from = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
        const int to = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
        erased = eraseMarks(*paras, cur.startParagraph, from, cur.endParagraph, to, names);
    } else {
        for (model::Section& section : doc.sections) {
            erased += eraseMarks(section.paragraphs, 0, 0, static_cast<int>(section.paragraphs.size()) - 1, -1, names);
        }
    }
    if (erased == 0) {
        why = "지울 조판 부호가 없습니다.";
        return {};
    }
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    hold.preferredX = -1;
    const int caretChar = cur.active && cur.story > 0 ? offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset) : 0;
    if (cur.story > 0) dropParentLineSegsOf(doc, cur.story);
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    if (cur.active && cur.story > 0 && resolveStory(doc, cur.story)) {
        const model::Paragraph* para = storyParagraph(doc, cur.story, cur.paragraph);
        const int end = para ? paragraphCharCount(*para) : 0;
        setPointerCursor(doc, pages, model::CursorPos{cur.story, cur.paragraph,
                                                      charIndexToOffset(doc, pages, cur.story, cur.paragraph, std::min(caretChar, end))});
    }
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

}  // namespace chwp::render
