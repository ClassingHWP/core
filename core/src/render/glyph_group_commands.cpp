// 덧말 넣기와 글자 겹치기(한글 2024 입력 → 덧말 넣기, 입력 도우미 → 글자 겹치기), 고치기와 덧말 지움.
//
// 한글 2024 COM 으로 확인한 규칙(DutmalChars 의 HDutmal, ComposeChars 의 HChCompose, DeleteDutmal):
//   - 덧말: 위치 위, 크기 0(50%), 정렬 가운데로 넣는다. 블록이 있으면 그 글이 본말이고, 블록을 그 덧말로 바꾼다.
//     위치는 위·아래뿐이다(값 2 는 한글이 파일에 이름을 적지 않는다). 덧말 컨트롤은 블록(없으면 캐럿) 자리 글자 모양을 쓴다.
//     덧말 스타일을 고르면 옵션 4 와 그 스타일 번호를 적는다.
//   - 덧말 대화 상자는 본말·덧말·덧말 위치·덧말 스타일뿐이고, 고칠 때 덧말 지움이 있다(Hwp.String 리소스).
//   - 덧말 지우기: 캐럿 바로 뒤 덧말을 본말 글로 바꾼다(그 런 글자 모양, 덧말은 없어진다).
//   - 글자 겹치기: 모양 없음(CHAR), 크기 -3, 겹침(OVERLAP)으로 넣는다. 블록이 있으면 그 글이 겹칠 글자다.
//     글자 모양 번호는 0 칸이 그 자리 글자 모양, 블록에서 넣으면 i+1 칸이 i 번째 글자의 모양이고 나머지는 비운다.
//     한글은 같은 모양을 테두리 번호만 다른 새 글자 모양으로 복사해 쓴다. 보이는 모양이 같아 여기서는 그 모양을 쓴다.
//   - 글자 겹치기의 글자 크기 조절은 50%~150%, 10 씩이다(크기 -5~+5).
//     겹쳐 쓸 글자 칸에는 길이 한도가 없고(같은 대화 상자 Edit), COM 으로 12자도 넣어진다. 글자 모양 칸은 늘 10 개다.
// 고치기(한글 고치기-덧말·두 번 누르기)는 한 글자 블록이 덧말·겹치기이거나 캐럿 바로 뒤가 그것일 때다.
// COM 으로는 고치기 대화 상자의 처음 값을 읽지 못한다(GetDefault 가 빈 값). 그 컨트롤 값을 그대로 보여 준다.

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

constexpr std::uint32_t kNoShape = 0xFFFFFFFFu;

const FormatValue* fieldOf(const FormatFields& fields, const std::string& key) {
    const FormatValue* found = nullptr;
    for (const auto& [name, value] : fields) {
        if (name == key) found = &value;
    }
    return found;
}
std::string textOf(const FormatFields& fields, const std::string& key, const std::string& fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const std::string* t = std::get_if<std::string>(v)) return *t;
    }
    return fallback;
}
double numberOf(const FormatFields& fields, const std::string& key, double fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const double* d = std::get_if<double>(v)) return *d;
    }
    return fallback;
}

// 한 문단 안 블록의 글자 범위. 블록이 없거나 문단을 넘으면 거짓이다.
struct Picked {
    int paragraph = -1;
    int from = 0;
    int to = 0;
    std::u32string chars;
};

bool pickedRange(const model::Document& doc, const std::vector<Node>& pages, const PointerCursor& cur, Picked& out) {
    if (!cur.active || !cur.hasSelection || cur.story <= 0 || cur.startParagraph != cur.endParagraph) return false;
    const model::Paragraph* para = storyParagraph(doc, cur.story, cur.startParagraph);
    if (!para) return false;
    int a = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
    int b = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
    if (a > b) std::swap(a, b);
    const std::u32string all = caretCharsOf(*para);
    a = std::clamp(a, 0, static_cast<int>(all.size()));
    b = std::clamp(b, 0, static_cast<int>(all.size()));
    if (a == b) return false;
    out.paragraph = cur.startParagraph;
    out.from = a;
    out.to = b;
    out.chars = all.substr(static_cast<std::size_t>(a), static_cast<std::size_t>(b - a));
    return true;
}

// 글자만 있는 블록인지(개체·조판 부호·줄바꿈이 끼면 본말·겹칠 글자로 쓰지 않는다).
bool plainText(const std::u32string& chars) {
    return std::none_of(chars.begin(), chars.end(), [](char32_t c) { return c < 0x20; });
}

std::string utf8Of(const std::u32string& chars) {
    std::string out;
    for (const char32_t c : chars) appendUtf8(out, c);
    return out;
}

std::u32string codePoints(const std::string& text) {
    std::u32string out;
    std::size_t index = 0;
    char32_t cp = 0;
    while (nextCp(text, index, cp)) out.push_back(cp);
    return out;
}

// 고칠 덧말·겹치기. 한 글자 블록이 그것이거나, 블록 없이 캐럿 바로 뒤 글자가 그것이다.
struct Target {
    model::Paragraph* para = nullptr;
    int paragraph = -1;
    int charIndex = -1;
    model::Run* run = nullptr;
    std::size_t item = 0;
    model::Control* control = nullptr;
};

bool glyphGroupKind(const model::Control& c) {
    return c.kind == model::ControlKind::Dutmal || c.kind == model::ControlKind::Compose;
}

bool targetAt(model::Document& doc, const std::vector<Node>& pages, const PointerCursor& cur, Target& out) {
    if (!cur.active || cur.story <= 0) return false;
    int paragraph = cur.paragraph;
    int charIndex = -1;
    if (cur.hasSelection) {
        if (cur.startParagraph != cur.endParagraph) return false;
        int a = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
        int b = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
        if (a > b) std::swap(a, b);
        if (b - a != 1) return false;
        paragraph = cur.startParagraph;
        charIndex = a;
    } else {
        charIndex = offsetToCharIndex(doc, pages, cur.story, paragraph, cur.offset);
    }
    model::Paragraph* para = storyParagraph(doc, cur.story, paragraph);
    if (!para) return false;
    int at = 0;
    for (model::Run& run : para->runs) {
        for (std::size_t i = 0; i < run.items.size(); ++i) {
            model::InlineItem& item = run.items[i];
            if (item.type == model::InlineItem::Type::Text) {
                at += utf8CodePointCount(item.text);
                if (at > charIndex) return false;
                continue;
            }
            if (!isCaretCharControl(item.control)) continue;
            if (at == charIndex) {
                if (!glyphGroupKind(item.control)) return false;
                out = Target{para, paragraph, charIndex, &run, i, &item.control};
                return true;
            }
            ++at;
        }
    }
    return false;
}

// 글자 겹치기만 두 개 이상 든 블록(한글 2024: 그 블록에서 글자 겹치기를 부르면 "모양 바꾸기" 대화 상자가 뜬다.
// 글자와 겹치기가 섞인 블록은 "선택영역이 잘못 설정 되었습니다"로 거절한다). 블록은 한 문단 안이어야 한다.
struct ComposeBlock {
    model::Paragraph* para = nullptr;
    int paragraph = -1;
    std::vector<model::Control*> composes;
};

bool composeBlockAt(model::Document& doc, const std::vector<Node>& pages, const PointerCursor& cur, ComposeBlock& out) {
    if (!cur.active || cur.story <= 0 || !cur.hasSelection || cur.startParagraph != cur.endParagraph) return false;
    int a = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
    int b = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
    if (a > b) std::swap(a, b);
    if (b - a < 2) return false;
    model::Paragraph* para = storyParagraph(doc, cur.story, cur.startParagraph);
    if (!para) return false;
    int at = 0;
    for (model::Run& run : para->runs) {
        for (model::InlineItem& item : run.items) {
            int len = 0;
            if (item.type == model::InlineItem::Type::Text) {
                len = utf8CodePointCount(item.text);
                if (len > 0 && at < b && at + len > a) return false;  // 블록에 글자가 있다
            } else if (isCaretCharControl(item.control)) {
                len = 1;
                if (at >= a && at < b) {
                    if (item.control.kind != model::ControlKind::Compose) return false;
                    out.composes.push_back(&item.control);
                }
            }
            at += len;
        }
    }
    out.para = para;
    out.paragraph = cur.startParagraph;
    return static_cast<int>(out.composes.size()) == b - a;
}

// 덧말 대화 상자 값. 크기·정렬은 대화 상자에 없어 넣을 때 0(50%)·가운데이고, 값이 오면 쓴다(에이전트).
void applyRubyFields(const model::Document& doc, const FormatFields& fields, model::Control& control) {
    if (fieldOf(fields, "pos")) control.dutmalPos = textOf(fields, "pos", "TOP") == "BOTTOM" ? "BOTTOM" : "TOP";
    if (fieldOf(fields, "ratio")) {
        control.dutmalSizeRatio =
            static_cast<std::uint32_t>(std::clamp(std::lround(numberOf(fields, "ratio", 0.0)), 0L, 1000L));
    }
    if (fieldOf(fields, "align")) {
        control.dutmalAlign = model::dutmalAlignName(model::dutmalAlignCode(textOf(fields, "align", "CENTER")));
    }
    if (fieldOf(fields, "style")) {
        const long style = std::lround(numberOf(fields, "style", -1.0));
        if (style >= 0 && static_cast<std::size_t>(style) < doc.header.styles.size()) {
            control.dutmalOption |= 4u;
            control.dutmalStyle = static_cast<std::uint32_t>(style);
        } else {
            control.dutmalOption &= ~4u;
            control.dutmalStyle = 0;
        }
    }
}

void applyComposeFields(const FormatFields& fields, model::Control& control) {
    if (fieldOf(fields, "circle")) {
        control.composeCircle = model::composeCircleName(model::composeCircleCode(textOf(fields, "circle", "CHAR")));
    }
    if (fieldOf(fields, "size")) {
        control.composeCharSize =
            static_cast<std::int8_t>(std::clamp(std::lround(numberOf(fields, "size", -3.0)), -5L, 5L));
    }
    if (fieldOf(fields, "type")) control.composeType = textOf(fields, "type", "OVERLAP") == "SPREAD" ? "SPREAD" : "OVERLAP";
}

EditResult relaidOut(model::Document& doc, std::vector<Node>& pages, model::Paragraph& para, int story, int paragraph,
                     int caretChar, bool useStoredLineHeight) {
    para.lineSegs.clear();
    pointerHold().preferredX = -1;
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    setPointerCursor(doc, pages,
                     model::CursorPos{story, paragraph, charIndexToOffset(doc, pages, story, paragraph, caretChar)});
    return makeEditResult(doc, true, true, story, paragraph, &relayout);
}

// 고치기(modify)·덧말 지움(remove).
EditResult editTarget(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                      const std::string& action, bool useStoredLineHeight, std::string& why) {
    const PointerCursor cur = pointerCursor();
    Target target;
    if (!targetAt(doc, pages, cur, target)) {
        why = "고칠 덧말이나 겹친 글자가 캐럿 뒤에 없습니다.";
        return {};
    }
    clearComposition(pointerHold());
    model::Control& c = *target.control;
    if (action == "remove") {
        if (c.kind != model::ControlKind::Dutmal) {
            why = "덧말만 지울 수 있습니다.";
            return {};
        }
        // 본말만 그 런 글자 모양의 글로 남긴다.
        const std::string main = c.text;
        const int count = utf8CodePointCount(main);
        target.run->items[target.item] = model::InlineItem::makeText(main);
        return relaidOut(doc, pages, *target.para, cur.story, target.paragraph, target.charIndex + count,
                         useStoredLineHeight);
    }
    if (c.kind == model::ControlKind::Dutmal) {
        const std::string mainText = textOf(fields, "mainText", c.text);
        const std::string subText = textOf(fields, "subText", c.subText);
        if (mainText.empty()) {
            why = "본말을 입력하세요.";
            return {};
        }
        if (subText.empty()) {
            why = "덧말을 입력하세요.";
            return {};
        }
        c.text = mainText;
        c.subText = subText;
        applyRubyFields(doc, fields, c);
    } else {
        const std::u32string chars = codePoints(textOf(fields, "chars", c.text));
        if (chars.empty() || !plainText(chars)) {
            why = "겹쳐 쓸 글자를 입력하세요.";
            return {};
        }
        c.text = utf8Of(chars);
        applyComposeFields(fields, c);
    }
    return relaidOut(doc, pages, *target.para, cur.story, target.paragraph, target.charIndex + 1, useStoredLineHeight);
}

}  // namespace

FormatFields glyphGroupView(model::Document& doc, const std::vector<Node>& pages) {
    Picked picked;
    std::string text;
    if (pickedRange(doc, pages, pointerCursor(), picked) && plainText(picked.chars)) text = utf8Of(picked.chars);
    FormatFields out;
    out.emplace_back("mainText", text);
    out.emplace_back("subText", std::string());
    out.emplace_back("pos", std::string("TOP"));
    out.emplace_back("style", -1.0);
    out.emplace_back("chars", text);
    out.emplace_back("circle", std::string("CHAR"));
    out.emplace_back("size", -3.0);
    out.emplace_back("type", std::string("OVERLAP"));
    // 덧말 스타일 목록(문서 스타일 차례, 줄 번호가 곧 스타일 번호).
    std::string styles;
    for (std::size_t i = 0; i < doc.header.styles.size(); ++i) {
        if (i) styles += "\n";
        styles += doc.header.styles[i].name;
    }
    out.emplace_back("styles", styles);
    // 겹치기만 든 블록이면 모양 바꾸기다. 한글 대화 상자는 처음에 테두리 없음·모양 안에 겹치기 끔이다.
    ComposeBlock block;
    if (composeBlockAt(doc, pages, pointerCursor(), block)) {
        out.emplace_back("editKind", std::string("composeShape"));
        out.emplace_back("editCount", static_cast<double>(block.composes.size()));
        out.emplace_back("editCircle", std::string("CHAR"));
        out.emplace_back("editType", std::string("SPREAD"));
        return out;
    }
    // 고칠 덧말·겹치기가 있으면 그 값. 한 글자 블록으로 고른 것이면 editFromBlock 이다(한글은 그때 메뉴의
    // 글자 겹치기·덧말 넣기도 그 값을 채워 연다).
    Target target;
    if (!targetAt(doc, pages, pointerCursor(), target)) {
        out.emplace_back("editKind", std::string());
        return out;
    }
    out.emplace_back("editFromBlock", pointerCursor().hasSelection);
    const model::Control& c = *target.control;
    if (c.kind == model::ControlKind::Dutmal) {
        out.emplace_back("editKind", std::string("ruby"));
        out.emplace_back("editMainText", c.text);
        out.emplace_back("editSubText", c.subText);
        out.emplace_back("editPos", std::string(c.dutmalPos == "BOTTOM" ? "BOTTOM" : "TOP"));
        out.emplace_back("editStyle", (c.dutmalOption & 4u) != 0 ? static_cast<double>(c.dutmalStyle) : -1.0);
    } else {
        out.emplace_back("editKind", std::string("compose"));
        out.emplace_back("editChars", c.text);
        out.emplace_back("editCircle", c.composeCircle);
        out.emplace_back("editSize", static_cast<double>(c.composeCharSize));
        out.emplace_back("editType", c.composeType);
    }
    return out;
}

EditResult glyphGroupNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                         bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    const std::string action = textOf(fields, "action", "insert");
    if (action == "modify" || action == "remove") return editTarget(doc, pages, fields, action, useStoredLineHeight, why);
    if (action == "reshape") {
        // 모양 바꾸기: 블록 안 모든 겹치기에 같은 모양과 "모양 안에 글자 겹치기"를 준다. 글자·크기는 그대로다.
        const PointerCursor cur = pointerCursor();
        ComposeBlock block;
        if (!composeBlockAt(doc, pages, cur, block)) {
            why = "선택영역이 잘못 설정 되었습니다.";
            return {};
        }
        clearComposition(pointerHold());
        for (model::Control* c : block.composes) {
            if (fieldOf(fields, "circle")) {
                c->composeCircle = model::composeCircleName(model::composeCircleCode(textOf(fields, "circle", "CHAR")));
            }
            if (fieldOf(fields, "type")) c->composeType = textOf(fields, "type", "SPREAD") == "OVERLAP" ? "OVERLAP" : "SPREAD";
        }
        const int anchor = offsetToCharIndex(doc, pages, cur.story, cur.anchorParagraph, cur.anchorOffset);
        const int focus = offsetToCharIndex(doc, pages, cur.story, cur.focusParagraph, cur.focusOffset);
        EditResult result = relaidOut(doc, pages, *block.para, cur.story, block.paragraph, focus, useStoredLineHeight);
        // 블록은 그대로 둔다.
        setPointerCursor(doc, pages,
                         model::CursorPos{cur.story, block.paragraph, charIndexToOffset(doc, pages, cur.story, block.paragraph, anchor)},
                         model::CursorPos{cur.story, block.paragraph, charIndexToOffset(doc, pages, cur.story, block.paragraph, focus)});
        return result;
    }
    const std::string kind = textOf(fields, "kind", "");
    if (kind != "ruby" && kind != "compose") {
        why = "넣을 것(ruby·compose)을 모릅니다.";
        return {};
    }
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0 || !storyParagraph(doc, cur.story, cur.paragraph)) return {};
    if (cur.hasSelection && cur.startParagraph != cur.endParagraph) {
        why = "블록이 한 문단 안에 있어야 합니다.";
        return {};
    }
    Picked picked;
    const bool fromBlock = pickedRange(doc, pages, cur, picked);
    if (fromBlock && !plainText(picked.chars)) {
        why = "블록에 글자가 아닌 것이 있습니다.";
        return {};
    }

    model::Control control;
    std::vector<std::uint32_t> pickedShapes;
    if (fromBlock) {
        const model::Paragraph* para = storyParagraph(doc, cur.story, picked.paragraph);
        for (int i = picked.from; i < picked.to; ++i) pickedShapes.push_back(charShapeAtParagraphChar(*para, i, true));
    }
    if (kind == "ruby") {
        control.kind = model::ControlKind::Dutmal;
        control.ctrlId = "tdut";
        control.text = textOf(fields, "mainText", "");
        control.subText = textOf(fields, "subText", "");
        if (control.text.empty()) {
            why = "본말을 입력하세요.";
            return {};
        }
        if (control.subText.empty()) {
            why = "덧말을 입력하세요.";
            return {};
        }
        control.dutmalPos = "TOP";
        control.dutmalSizeRatio = 0;
        control.dutmalAlign = "CENTER";
        applyRubyFields(doc, fields, control);
    } else {
        control.kind = model::ControlKind::Compose;
        control.ctrlId = "tcps";
        const std::u32string chars = codePoints(textOf(fields, "chars", ""));
        if (chars.empty()) {
            why = "겹쳐 쓸 글자를 입력하세요.";
            return {};
        }
        if (!plainText(chars)) {
            why = "겹쳐 쓸 글자에 글자가 아닌 것이 있습니다.";
            return {};
        }
        control.text = utf8Of(chars);
        control.composeCircle = "CHAR";
        control.composeCharSize = -3;
        control.composeType = "OVERLAP";
        applyComposeFields(fields, control);
    }

    PointerHold& hold = pointerHold();
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
    const std::uint32_t shape =
        !pickedShapes.empty() ? pickedShapes.front() : charShapeAtParagraphChar(*host, charIndex, false);
    if (control.kind == model::ControlKind::Compose) {
        control.composeCharShapes.assign(10, kNoShape);
        control.composeCharShapes[0] = shape;
        // 블록에서 넣으면 블록 글자 모양을 글자마다 둔다(칸은 10 개, 0 칸은 앞 글자). 남는 글자는 1 칸 모양을 쓴다.
        const std::size_t count = std::min({pickedShapes.size(), codePoints(control.text).size(), std::size_t{9}});
        for (std::size_t i = 0; i < count; ++i) control.composeCharShapes[i + 1] = pickedShapes[i];
    }
    model::Paragraph right = splitParagraphAt(*host, charIndex);
    model::Run run;
    run.charShapeId = shape;
    run.items.push_back(model::InlineItem::makeControl(std::move(control)));
    host->runs.push_back(std::move(run));
    for (model::Run& r : right.runs) {
        if (!r.items.empty()) host->runs.push_back(std::move(r));
    }
    // 캐럿은 넣은 글자 칸 뒤다.
    return relaidOut(doc, pages, *host, cur.story, paraIdx, charIndex + 1, useStoredLineHeight);
}

}  // namespace chwp::render
