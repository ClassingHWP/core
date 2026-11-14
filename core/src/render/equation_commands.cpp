// 수식 넣기·고치기(한글 2024 입력 → 수식, Ctrl+N,M). 수식 편집기에서 스크립트와 글자 크기를 받아 캐럿 자리에 넣거나,
// 고른 수식을 고친다. 한글 2024 에서 새로 넣은 수식은:
//   글자처럼 취급, 문단 기준(세로·가로), 위아래 배치(TOP_AND_BOTTOM), 양쪽, 바깥 여백 왼쪽·오른쪽 56,
//   글꼴 HancomEQN, 글자 크기 1000(10pt), 색 검정, 줄 모드 CHAR, 버전 "Equation Version 60",
//   상자 크기와 기준선은 그 스크립트를 잰 값("a over b" 는 1025 × 2250, 기준선 66)이다.
// 미리 보기는 수식을 그린 윤곽을 SVG 로 낸다(셸은 받은 그림을 보여 주기만 한다).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "chwp/export/font.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/equation_draw.hpp"
#include "render/equation_layout.hpp"
#include "export/font_metrics.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

constexpr const char* kEquationFont = "HancomEQN";

// 이 수식을 재는 글꼴 중 셸이 아직 받는 중인 것이 있는지.
bool fontsPendingFor(const std::string& font) {
    for (const std::string& face : layout::equationFontFaces(font)) {
        if (layout::fontPending(face)) return true;
    }
    return false;
}

// 스크립트를 재어 상자 크기·기준선을 맞춘다. 글꼴이 아직 오지 않았으면 표시해 두고 글꼴이 오면 다시 잰다.
void measureInto(model::Control& control) {
    const std::string font = control.equationFont.empty() ? kEquationFont : control.equationFont;
    control.measurePending = fontsPendingFor(font);
    const EquationSize size = measureHangulEquation(control.script, control.baseUnit, font);
    if (!control.placement) control.placement = model::Placement{};
    control.placement->width = static_cast<std::uint32_t>(std::max<std::int32_t>(1, size.width));
    control.placement->height = static_cast<std::uint32_t>(std::max<std::int32_t>(1, size.height));
    control.baseLine = size.baseLine;
    control.fileBaseLine = size.baseLine;
}

std::string trimmed(const std::string& text) {
    std::size_t a = 0;
    std::size_t b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t' || text[a] == '\r' || text[a] == '\n')) ++a;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r' || text[b - 1] == '\n')) --b;
    return text.substr(a, b - a);
}

}  // namespace

// 고른 개체가 수식이면 그 자리(글 묶음·문단·항목 순번).
bool selectedEquationAt(const model::Document& doc, int& story, int& paragraph, int& item) {
    const PointerHold& hold = pointerHold();
    const Node* node = hold.selectedObject;
    if (!node || node->role != "equation") return false;
    story = node->ext().obj().anchorStory;
    paragraph = node->ext().obj().anchorParagraph;
    item = node->ext().obj().anchorItem;
    const model::Control* control = anchoredControl(const_cast<model::Document&>(doc), story, paragraph, item);
    return control && control->kind == model::ControlKind::Equation;
}


FormatFields equationView(const model::Document& doc) {
    FormatFields out;
    int story = -1;
    int paragraph = -1;
    int item = -1;
    if (selectedEquationAt(doc, story, paragraph, item)) {
        const model::Control* control = anchoredControl(const_cast<model::Document&>(doc), story, paragraph, item);
        out.emplace_back("editing", true);
        out.emplace_back("script", control->script);
        out.emplace_back("baseUnit", static_cast<double>(control->baseUnit > 0 ? control->baseUnit : 1000));
        return out;
    }
    out.emplace_back("editing", false);
    out.emplace_back("script", std::string());
    out.emplace_back("baseUnit", 1000.0);
    return out;
}

std::string equationSvg(const std::string& script, std::int32_t baseUnit, bool* ready) {
    if (ready) *ready = true;
    const std::string text = trimmed(script);
    if (text.empty()) return {};
    // 수식 글꼴이 아직 없으면(웹에서 받는 중) 다른 글꼴의 같은 자리 글자가 그려지고, 숫자·한글 자모를 재는 한컴바탕이
    // 없으면 그 글자 폭이 빠진다. 다 받은 뒤에 그린다.
    if (!layout::fontFaceAvailable(kEquationFont) || fontsPendingFor(kEquationFont)) {
        if (ready) *ready = false;
        return {};
    }
    EquationSize size;
    const auto inks = cachedEquationInk(text, baseUnit > 0 ? baseUnit : 1000, kEquationFont, size);
    if (size.width <= 0 || size.height <= 0) return {};
    // 수식 글꼴 윤곽이 아직 없으면(웹에서 받는 중) 빈 그림이다. 글꼴이 오면 다시 묻는다.
    if (!inks || inks->empty()) {
        if (ready) *ready = false;
        return {};
    }
    std::string svg;
    char buf[160];
    // 미리 보기는 250% 로 본다(96dpi 화면 픽셀). 셸은 이 크기 그대로 보인다.
    const double px = 96.0 / 7200.0 * 2.5;
    std::snprintf(buf, sizeof(buf),
                  R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%.1f" height="%.1f">)",
                  size.width, size.height, size.width * px, size.height * px);
    svg += buf;
    // 윤곽 점은 1/kEquationInkUnit HWPUNIT, y 는 위로 자란다(수식 상자 위가 0).
    const double unit = 1.0 / kEquationInkUnit;
    for (const EquationInk& ink : *inks) {
        std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(ink.color & 0xFFFFFFu));
        const std::string color = buf;
        if (ink.kind == EquationInk::Kind::Rect) {
            std::snprintf(buf, sizeof(buf), R"(<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" fill="%s"/>)", ink.x,
                          ink.y, ink.width, std::max(ink.height, 1.f), color.c_str());
            svg += buf;
            continue;
        }
        std::string d;
        for (const GlyphCommand& c : ink.commands) {
            const auto pt = [&](const GlyphPoint& p) {
                std::snprintf(buf, sizeof(buf), "%.2f %.2f ", p.x * unit, -p.y * unit);
                d += buf;
            };
            if (c.verb == GlyphVerb::Move) d += "M";
            else if (c.verb == GlyphVerb::Line) d += "L";
            else if (c.verb == GlyphVerb::Quad) {
                d += "Q";
                pt(c.c1);
            } else {
                d += "C";
                pt(c.c1);
                pt(c.c2);
            }
            pt(c.to);
        }
        if (d.empty()) continue;
        svg += R"(<path fill-rule="nonzero" fill=")" + color + R"(" d=")" + d + "Z\"";
        if (ink.stroke > 0.f) {
            std::snprintf(buf, sizeof(buf), R"( stroke="%s" stroke-width="%.2f")", color.c_str(), ink.stroke);
            svg += buf;
        }
        svg += "/>";
    }
    svg += "</svg>";
    return svg;
}

int remeasurePendingEquations(model::Document& doc) {
    int count = 0;
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        bool touched = false;
        for (model::Run& run : para.runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Control) continue;
                model::Control& control = item.control;
                if (control.kind != model::ControlKind::Equation || !control.measurePending) continue;
                const std::string font = control.equationFont.empty() ? kEquationFont : control.equationFont;
                if (fontsPendingFor(font)) continue;
                measureInto(control);
                touched = true;
                ++count;
            }
        }
        if (touched) para.lineSegs.clear();
    });
    return count;
}

namespace paint_detail {

EditResult insertEquationNow(model::Document& doc, std::vector<Node>& pages, const std::string& script,
                             std::int32_t baseUnit, bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    const std::string text = trimmed(script);
    if (text.empty()) {
        why = "수식 스크립트가 비어 있습니다.";
        return {};
    }
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) {
        why = "수식을 넣을 자리(캐럿)가 없습니다.";
        return {};
    }
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.paragraph < 0 || cur.paragraph >= static_cast<int>(paras->size())) return {};
    clearComposition(hold);

    const int story = cur.story;
    int paraIdx = cur.paragraph;
    int charIndex = 0;
    int paraDelta = 0;
    int oldEnd = 0;
    if (cur.hasSelection) {
        paraDelta = -(cur.endParagraph - cur.startParagraph);
        const int endChar =
            paraDelta == 0 ? offsetToCharIndex(doc, pages, story, cur.endParagraph, cur.endOffset) : 0;
        deleteSelectionRange(doc, pages, story, cur.startParagraph, cur.startOffset, cur.endParagraph, cur.endOffset,
                             paraIdx, charIndex);
        paras = storyParagraphs(doc, story);
        oldEnd = paraDelta == 0 ? endChar : charIndex;
    } else {
        charIndex = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
        oldEnd = charIndex;
    }

    model::Control control;
    control.kind = model::ControlKind::Equation;
    control.ctrlId = "eqed";
    model::Placement placement;
    placement.treatAsChar = true;
    placement.flowWithText = true;
    placement.allowOverlap = false;
    placement.textWrap = "TOP_AND_BOTTOM";
    placement.textFlow = "BOTH_SIDES";
    placement.vertRelTo = "PARA";
    placement.horzRelTo = "PARA";
    placement.vertAlign = "TOP";
    placement.horzAlign = "LEFT";
    placement.marginLeft = 56;
    placement.marginRight = 56;
    placement.numberingType = "EQUATION";
    control.placement = placement;
    control.script = text;
    control.baseUnit = baseUnit > 0 ? baseUnit : 1000;
    control.equationFont = kEquationFont;
    measureInto(control);

    model::Paragraph& host = (*paras)[static_cast<std::size_t>(paraIdx)];
    const std::uint32_t charShape = charShapeAtParagraphChar(host, charIndex, false);
    model::Paragraph right = splitParagraphAt(host, charIndex);
    model::Run equationRun;
    equationRun.charShapeId = charShape;
    equationRun.items.push_back(model::InlineItem::makeControl(std::move(control)));
    host.runs.push_back(std::move(equationRun));
    for (model::Run& run : right.runs) {
        if (!run.items.empty()) host.runs.push_back(std::move(run));
    }
    host.lineSegs.clear();
    rebuildStoryMap(doc);
    hold.preferredX = -1;
    const EditRelayoutResult relayout = relayoutAfterEdit(
        doc, pages, EditRelayoutSpec{story, paraIdx, paraDelta, charIndex, oldEnd, charIndex + 1, useStoredLineHeight});
    const int newOff = charIndexToOffset(doc, pages, story, paraIdx, charIndex + 1);
    setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
    return makeEditResult(doc, true, true, story, paraIdx, &relayout);
}

EditResult editEquationNow(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                           const std::string& script, std::int32_t baseUnit, bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    model::Control* control = anchoredControl(doc, story, paragraph, item);
    if (!control || control->kind != model::ControlKind::Equation) {
        why = "그 자리에 수식이 없습니다.";
        return {};
    }
    const std::string text = trimmed(script);
    if (text.empty()) {
        why = "수식 스크립트가 비어 있습니다.";
        return {};
    }
    control->script = text;
    if (baseUnit > 0) control->baseUnit = baseUnit;
    measureInto(*control);
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    const bool wraps = control->placement && !control->placement->treatAsChar;
    if (paras) {
        if (wraps) {
            for (model::Paragraph& para : *paras) para.lineSegs.clear();
        } else if (paragraph >= 0 && paragraph < static_cast<int>(paras->size())) {
            (*paras)[static_cast<std::size_t>(paragraph)].lineSegs.clear();
        }
    }
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    hold.selectedObject = nullptr;
    hold.objectHandles = false;
    hold.resizeHandle = -1;
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    EditResult result = makeEditResult(doc, true, true, story, paragraph, &relayout);
    // 고친 수식을 다시 고른다(한글처럼 고친 뒤에도 그 수식이 골라져 있다).
    selectAnchoredObject(doc, pages, story, paragraph, item);
    return result;
}

}  // namespace paint_detail

}  // namespace chwp::render
