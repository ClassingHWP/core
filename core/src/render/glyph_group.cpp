#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "export/layout.hpp"
#include "render/scene_detail.hpp"

// 덧말(tdut)과 글자 겹치기(tcps). 글자처럼 줄에 앉는 글자 묶음 개체로 만든다.
// 식은 한글 2024 와 같고, 한글로 만든 PDF·lineseg 와 맞춰 보았다.
//
// 덧말
//   폭: 본말을 본말 글자 모양으로 잰 폭.
//   높이: q = 글자 크기 ÷ 4, 높이 = 4 × (비율 × q ÷ 100 + 위면 q, 아래면 q × 91 ÷ 100).
//     비율 0 은 50%다. 10pt 위 50% 는 1500, 아래 50% 는 1408, 11pt 위 50% 는 1648 이다(한글 lineseg textheight).
//   기준선: 높이의 MulDiv(85 + 위면 비율, 100, 비율 + 100)% 다. 위 30·50·70·100% 는 88·90·91·93%.
//   덧말 글자 모양: 본말 모양에서 크기만 MulDiv(크기, 비율, 100), 장평·상대 크기 100, 자간 0.
//     덧말이 본말보다 넓으면 장평을 1씩 줄인다(50까지). 정렬은 양쪽·왼쪽 0, 오른쪽 본말−덧말, 나머지 그 절반.
//     자간도 -50 까지 줄이는 고리가 있지만, 한글 PDF 의 긴 덧말(8자, 본말 1자)은 장평 50 만 줄고 자간 변화가 없다.
//   덧말 세로 자리: 위는 본말 상자 바로 위, 아래는 본말 상자 바로 아래에 덧말 상자를 둔다.
//     한글 PDF 와 0.1~0.25pt 다르다. 그 차이는 글꼴 어센더로 찍는 그리기 차이다.
//   위치 CENTER 는 한글이 HWPX 를 읽을 때 TOP 으로 바꾼다. 여기서도 위로 둔다.
//   옵션 비트 4(덧말 스타일): 스타일 번호의 글자 모양에서 글꼴·굵게·기울임·색·장평을 가져오고 크기만 본말 비율이다.
//     한글 PDF: 차례 제목 스타일(돋움, 파랑)에 굵게·기울임·밑줄·장평 80·자간 10 을 더하면 덧말은
//     돋움 굵은 기울임 파랑, 장평 80 이고 밑줄·자간은 없다. 긴 덧말은 그 장평 80 에서 50 까지 줄인다.
//   본말·덧말·겹친 글자는 글자 점만 찍는다. 밑줄·취소선·음영은 쪽 글자 고리가
//     묶음 폭 전체에 본말 자리로 긋는다(한글 PDF: 밑줄 친 런 안 덧말은 본말 아래에만 밑줄, 덧말에는 없다).
//     그래서 안쪽 줄(role glyph·reverse)은 글자와 강조점만 그리고, 장식은 본말 기준선의 빈 줄(role glyphDeco) 하나가 긋는다.
//
// 글자 겹치기
//   글자 묶음 앞 글자: 모양이면 모양 글자 표의 글자, 모양 없이 펼침이면 U+3000, 모양 없이 겹침이면 없다.
//   폭: 앞 글자와 각 글자 폭 중 가장 큰 값.
//   글자 모양 번호: 0 은 앞 글자, i+1 은 i 번째 글자. 0·1 이 비면 그 런 모양, 2 부터 비면 1 번 모양.
//   모양 안 글자 크기는 100 + 10 × 크기 %, +2 에서 멈춘다(한글 PDF: +3·+4 도 120%). 모양 없이는 크기를 쓰지 않는다.
//   모양 안 글자는 모양 높이 × 크기 ÷ 모양별 값만큼 내린다. 원 -3 은 120 위로.
//   겹침은 각 글자를 묶음 폭 가운데에 둔다. 모양 안 글자가 하나뿐이어도 겹침이다.
//   펼침은 양쪽 10% 를 비우고 글자를 이어 놓는다. 비율 = 0.8 × 폭 ÷ 글자 폭 합(1 이하),
//     글자 장평은 그 비율 × 100(50 아래면 50), 다음 글자는 폭 × 비율만큼 민다. 남는 폭의 절반만큼 민다.
//   반전 모양(●■▲◆)은 안 글자를 흰색으로 찍는다.
//   줄 높이에는 들지 않는다. 여기서는 그 런 글자 크기 상자로 둔다.

namespace chwp::render::detail {
namespace {

constexpr std::array<char32_t, 14> kComposeLead = {0,      0x25EF, 0x25CF, 0x25A1, 0x25A0, 0x25B3, 0x25B2,
                                                   0x263C, 0x25C7, 0x25C6, 0x25A2, 0x2672, 0x267A, 0x267B};
constexpr std::array<bool, 14> kComposeReversed = {false, false, true,  false, true,  false, true,
                                                   false, false, true,  false, false, false, false};
constexpr std::array<int, 14> kComposeDrop = {0, 25, 30, 30, 30, 35, 35, 30, 30, 30, 30, 25, 25, 25};
constexpr std::uint32_t kNoShape = 0xFFFFFFFFu;

// 곱한 뒤 나눈다. 0.5 는 0 에서 멀어지게 올린다.
std::int32_t mulDiv(std::int64_t a, std::int64_t b, std::int64_t c) {
    if (c == 0) return 0;
    const std::int64_t n = a * b;
    const std::int64_t q = (std::llabs(n) + std::llabs(c) / 2) / std::llabs(c);
    return static_cast<std::int32_t>((n < 0) != (c < 0) ? -q : q);
}

std::vector<char32_t> codePointsOf(const std::string& text) {
    std::vector<char32_t> out;
    std::size_t index = 0;
    char32_t cp = 0;
    while (nextCodePoint(text, index, cp)) out.push_back(cp);
    return out;
}

const CharShape& runShape(const Document& doc, std::uint32_t id) {
    const CharShape* shape = findCharShape(doc, id);
    return shape ? *shape : fallbackCharShape();
}

int sizeOf(const CharShape& shape) { return shape.height > 0 ? shape.height : static_cast<int>(kEm); }

// 묶음 안 글자 하나. 그 글자를 재고 찍을 글자 모양과 자리다.
struct Piece {
    char32_t cp = 0;
    std::uint32_t shapeId = 0;
    CharShape shape;
    std::int32_t x = 0;
    std::int32_t advance = 0;
};

// 같은 기준선에 놓는 글자 줄. 줄 노드의 기준선은 글자 크기의 85% 다(glyphBaseIn).
Node pieceLine(const Document& doc, const std::vector<Piece>& pieces, std::int32_t baselineY, bool reversed) {
    Node line;
    line.kind = NodeKind::Line;
    line.align = "LEFT";
    line.role = reversed ? "reverse" : "glyph";
    int size = 0;
    for (const Piece& piece : pieces) size = std::max(size, sizeOf(piece.shape));
    if (size <= 0) size = kEm;
    const std::int32_t base = layout::glyphBaseline(size);
    line.fontSize = size;
    line.height = size;
    line.editContent().textHeight = size;
    line.editContent().lineBaseline = base;
    line.y = baselineY - base;
    std::int32_t left = pieces.empty() ? 0 : pieces.front().x;
    for (const Piece& piece : pieces) left = std::min(left, piece.x);
    line.x = left;
    std::int32_t pen = left;
    std::string& text = line.textMut();
    for (const Piece& piece : pieces) {
        const GlyphFace face = glyphFace(doc, piece.shape, piece.cp, true);
        if (line.font.empty()) line.font = face.font;
        // 글자 자리는 앞 글자 폭으로 잇는다. 겹치는 글자는 앞 글자 폭을 그 자리까지로 줄인다.
        const std::size_t at = text.size();
        const std::int32_t gap = piece.x - pen;
        if (gap != 0 && !line.glyphs().runs.empty()) {
            LineGlyphs& glyphs = line.glyphsMut();
            glyphs.advances[glyphs.runs.back().advanceAt] += gap;
            glyphs.inks[glyphs.runs.back().inkAt] += gap;
            pen += gap;
        }
        char buf[4];
        int n = 0;
        const char32_t cp = piece.cp;
        if (cp < 0x80) buf[n++] = static_cast<char>(cp);
        else if (cp < 0x800) {
            buf[n++] = static_cast<char>(0xC0 | (cp >> 6));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[n++] = static_cast<char>(0xE0 | (cp >> 12));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            buf[n++] = static_cast<char>(0xF0 | (cp >> 18));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
        text.append(buf, static_cast<std::size_t>(n));
        LineRun run;
        run.textBegin = static_cast<std::uint16_t>(at);
        run.textEnd = static_cast<std::uint16_t>(text.size());
        run.charShapeId = piece.shapeId;
        run.style = runStyleOf(sizeOf(piece.shape), face.font, std::string{}, std::string{}, face.ratio, face.relSize,
                               0, 0);
        const std::int32_t advance = piece.advance;
        appendLineRun(line.glyphsMut(), run, &advance, 1, &advance, 1);
        pen += advance;
    }
    // 앞 글자로 민 자리가 음수면(겹친 글자가 앞으로 나감) 줄 왼쪽을 그 글자로 옮긴 것과 같다.
    line.width = pen - left;
    trimLineGlyphs(line.glyphsMut());
    return line;
}

// 묶음 폭 전체의 밑줄·취소선·음영을 긋는 빈 줄. 글자(U+00A0)는 찍지 않는다.
Node decoLine(std::uint32_t shapeId, const CharShape& shape, std::int32_t width, std::int32_t baselineY) {
    Node line;
    line.kind = NodeKind::Line;
    line.align = "LEFT";
    line.role = "glyphDeco";
    const int size = sizeOf(shape);
    const std::int32_t base = layout::glyphBaseline(size);
    line.fontSize = size;
    line.height = size;
    line.editContent().textHeight = size;
    line.editContent().lineBaseline = base;
    line.y = baselineY - base;
    line.width = width;
    line.textMut() = "\xC2\xA0";  // U+00A0. 글자를 찍지 않는다
    LineRun run;
    run.textBegin = 0;
    run.textEnd = 2;
    run.charShapeId = shapeId;
    run.style = runStyleOf(size, std::string{}, std::string{}, std::string{}, 100, 100, 0, 0);
    appendLineRun(line.glyphsMut(), run, &width, 1, &width, 1);
    trimLineGlyphs(line.glyphsMut());
    return line;
}

std::int32_t textWidth(const Document& doc, const std::vector<char32_t>& cps, const CharShape& shape) {
    std::int32_t sum = 0;
    for (const char32_t cp : cps) sum += glyphWidth(doc, cp, shape);
    return sum;
}

// ---- 덧말 ----

struct RubyBox {
    std::int32_t mainWidth = 0;
    std::int32_t height = 0;
    int basePercent = 85;
    bool bottom = false;
    int mainSize = kEm;
};

RubyBox rubyBox(const Document& doc, const Control& control, std::uint32_t charShapeId) {
    const CharShape& main = runShape(doc, charShapeId);
    RubyBox box;
    box.mainSize = sizeOf(main);
    box.mainWidth = textWidth(doc, codePointsOf(control.text), main);
    const int ratio = control.dutmalSizeRatio > 0 ? static_cast<int>(control.dutmalSizeRatio) : 50;
    box.bottom = control.dutmalPos == "BOTTOM";
    const int q = box.mainSize / 4;
    box.height = 4 * (ratio * q / 100 + (box.bottom ? q * 91 / 100 : q));
    box.basePercent = mulDiv(85 + (box.bottom ? 0 : ratio), 100, ratio + 100);
    return box;
}

Node makeRuby(const Control& control, std::uint32_t charShapeId, const Document& doc, std::int32_t x) {
    const RubyBox box = rubyBox(doc, control, charShapeId);
    const CharShape& main = runShape(doc, charShapeId);
    Node node;
    node.kind = NodeKind::Object;
    node.role = "glyphGroup";
    node.treatAsChar = true;
    node.x = x;
    node.width = box.mainWidth;
    node.height = box.height;
    node.extMut().objMut().baseLine = box.basePercent;
    const std::int32_t base = mulDiv(box.height, box.basePercent, 100);

    std::vector<Piece> mainPieces;
    std::int32_t pen = 0;
    for (const char32_t cp : codePointsOf(control.text)) {
        Piece piece{cp, charShapeId, main, pen, glyphWidth(doc, cp, main)};
        pen += piece.advance;
        mainPieces.push_back(std::move(piece));
    }
    node.children.push_back(decoLine(charShapeId, main, box.mainWidth, base));
    if (!mainPieces.empty()) node.children.push_back(pieceLine(doc, mainPieces, base, false));

    const std::vector<char32_t> subCps = codePointsOf(control.subText);
    if (subCps.empty()) return node;
    const int ratio = control.dutmalSizeRatio > 0 ? static_cast<int>(control.dutmalSizeRatio) : 50;
    std::uint32_t subShapeId = charShapeId;
    CharShape sub = main;
    sub.ratio.fill(100);
    if ((control.dutmalOption & 4u) != 0 && control.dutmalStyle < doc.header.styles.size()) {
        const std::uint32_t styleShape = doc.header.styles[control.dutmalStyle].charShapeId;
        if (findCharShape(doc, styleShape)) {
            subShapeId = styleShape;
            sub = runShape(doc, styleShape);
        }
    }
    sub.height = mulDiv(box.mainSize, ratio, 100);
    sub.relSize.fill(100);
    sub.spacing.fill(0);
    std::int32_t subWidth = textWidth(doc, subCps, sub);
    const auto shrinkable = [&] {
        return std::any_of(sub.ratio.begin(), sub.ratio.end(), [](auto r) { return r > 50; });
    };
    while (subWidth > box.mainWidth && shrinkable()) {
        for (auto& r : sub.ratio) r = std::max(50, static_cast<int>(r) - 1);
        subWidth = textWidth(doc, subCps, sub);
    }
    std::int32_t offset = 0;
    if (control.dutmalAlign == "RIGHT") offset = box.mainWidth - subWidth;
    else if (control.dutmalAlign != "JUSTIFY" && control.dutmalAlign != "LEFT") offset = (box.mainWidth - subWidth) / 2;
    if (offset < 0) offset = 0;
    const int subSize = sizeOf(sub);
    const std::int32_t subBase =
        box.bottom ? base + (box.mainSize - layout::glyphBaseline(box.mainSize)) + layout::glyphBaseline(subSize)
                   : base - layout::glyphBaseline(box.mainSize) - subSize + layout::glyphBaseline(subSize);
    std::vector<Piece> subPieces;
    pen = offset;
    for (const char32_t cp : subCps) {
        Piece piece{cp, subShapeId, sub, pen, glyphWidth(doc, cp, sub)};
        pen += piece.advance;
        subPieces.push_back(std::move(piece));
    }
    node.children.push_back(pieceLine(doc, subPieces, subBase, false));
    return node;
}

// ---- 글자 겹치기 ----

struct ComposeLayout {
    std::int32_t width = 0;
    std::uint32_t leadShapeId = 0;
    CharShape leadShape;
    char32_t lead = 0;
    bool reversed = false;
    bool spread = false;
    std::int32_t drop = 0;
    std::vector<Piece> chars;
};

ComposeLayout composeLayout(const Document& doc, const Control& control, std::uint32_t charShapeId) {
    ComposeLayout out;
    const std::uint32_t circle = std::min<std::uint32_t>(model::composeCircleCode(control.composeCircle), 13);
    out.spread = control.composeType == "SPREAD";
    out.lead = circle != 0 ? kComposeLead[circle] : (out.spread ? char32_t{0x3000} : char32_t{0});
    out.reversed = kComposeReversed[circle];
    const auto idAt = [&](std::size_t i) {
        return i < control.composeCharShapes.size() ? control.composeCharShapes[i] : kNoShape;
    };
    const auto validId = [&](std::uint32_t id) { return id != kNoShape && findCharShape(doc, id) != nullptr; };
    out.leadShapeId = validId(idAt(0)) ? idAt(0) : charShapeId;
    const std::uint32_t firstId = validId(idAt(1)) ? idAt(1) : charShapeId;
    out.leadShape = runShape(doc, out.leadShapeId);
    const int charSz = std::min<int>(control.composeCharSize, 2);
    const int percent = std::max(10, 100 + 10 * charSz);
    const std::vector<char32_t> cps = codePointsOf(control.text);
    std::int32_t widest = out.lead != 0 ? glyphWidth(doc, out.lead, out.leadShape) : 0;
    const std::int32_t leadWidth = widest;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        Piece piece;
        piece.cp = cps[i];
        piece.shapeId = validId(idAt(i + 1)) ? idAt(i + 1) : firstId;
        piece.shape = runShape(doc, piece.shapeId);
        if (circle != 0) piece.shape.height = sizeOf(piece.shape) * percent / 100;
        piece.advance = glyphWidth(doc, piece.cp, piece.shape);
        widest = std::max(widest, piece.advance);
        out.chars.push_back(std::move(piece));
    }
    out.width = widest;
    if (circle != 0) out.drop = sizeOf(out.leadShape) * charSz / kComposeDrop[circle];
    // 글자를 놓는 폭: 모양이면 모양 글자 폭, 모양 없이는 묶음 폭이다.
    const std::int32_t room = circle != 0 ? leadWidth : out.width;
    const bool spreads = out.spread && (circle == 0 || out.chars.size() > 1);
    if (!spreads) {
        for (Piece& piece : out.chars) piece.x = (room - piece.advance) / 2;
        return out;
    }
    std::int32_t total = 0;
    for (const Piece& piece : out.chars) total += piece.advance;
    const float inner = static_cast<float>(room * 80);
    const std::int32_t margin = static_cast<std::int32_t>((static_cast<float>(room) - inner / 100.f) * 0.5f);
    float scale = total > 0 ? inner / (static_cast<float>(total) * 100.f) : 1.f;
    if (scale > 1.f) scale = 1.f;
    const int squeeze = std::max(50, static_cast<int>(scale * 100.f));
    std::int32_t run = margin;
    std::int32_t end = 0;
    for (const Piece& piece : out.chars) {
        end = std::max(end, run + piece.advance * squeeze / 100);
        run = static_cast<std::int32_t>(static_cast<float>(piece.advance) * scale + static_cast<float>(run));
    }
    const std::int32_t offset = (room - margin - end) / 2;
    std::int32_t at = margin + offset;
    for (Piece& piece : out.chars) {
        const std::int32_t step = static_cast<std::int32_t>(static_cast<float>(piece.advance) * scale);
        piece.shape.ratio.fill(squeeze);
        piece.x = at;
        piece.advance = step;
        at += step;
    }
    return out;
}

Node makeCompose(const Control& control, std::uint32_t charShapeId, const Document& doc, std::int32_t x) {
    const ComposeLayout layout = composeLayout(doc, control, charShapeId);
    const int size = sizeOf(runShape(doc, charShapeId));
    Node node;
    node.kind = NodeKind::Object;
    node.role = "glyphGroup";
    node.treatAsChar = true;
    node.x = x;
    node.width = layout.width;
    node.height = size;
    node.extMut().objMut().baseLine = 85;
    const std::int32_t base = mulDiv(size, 85, 100);
    node.children.push_back(decoLine(charShapeId, runShape(doc, charShapeId), layout.width, base));
    if (layout.lead != 0 && layout.lead != 0x3000) {
        Piece lead{layout.lead, layout.leadShapeId, layout.leadShape, 0, glyphWidth(doc, layout.lead, layout.leadShape)};
        node.children.push_back(pieceLine(doc, {lead}, base, false));
    }
    if (layout.chars.empty()) return node;
    const bool overlap = !layout.spread || (layout.lead != 0 && layout.lead != 0x3000 && layout.chars.size() <= 1);
    if (overlap) {
        for (const Piece& piece : layout.chars) {
            node.children.push_back(pieceLine(doc, {piece}, base + layout.drop, layout.reversed));
        }
    } else {
        node.children.push_back(pieceLine(doc, layout.chars, base + layout.drop, layout.reversed));
    }
    return node;
}

}  // namespace

bool glyphGroupControl(const Control& control) {
    return control.kind == ControlKind::Dutmal || control.kind == ControlKind::Compose;
}

std::int32_t glyphGroupWidth(const Document& doc, const Control& control, std::uint32_t charShapeId) {
    if (control.kind == ControlKind::Dutmal) return rubyBox(doc, control, charShapeId).mainWidth;
    if (control.kind == ControlKind::Compose) return composeLayout(doc, control, charShapeId).width;
    return 0;
}

Node makeGlyphGroup(const Control& control, std::uint32_t charShapeId, const Document& doc, std::int32_t x) {
    if (control.kind == ControlKind::Dutmal) return makeRuby(control, charShapeId, doc, x);
    return makeCompose(control, charShapeId, doc, x);
}

Node atomObject(const Atom& atom, std::int32_t x, const Document& doc, int depth, std::int32_t defaultTab,
                bool useStoredLineHeight, bool deferSpans) {
    if (glyphGroupControl(*atom.control)) return makeGlyphGroup(*atom.control, atom.charShapeId, doc, x);
    return makeObject(*atom.control, x, 0, doc, depth, defaultTab, useStoredLineHeight, deferSpans);
}

}  // namespace chwp::render::detail
