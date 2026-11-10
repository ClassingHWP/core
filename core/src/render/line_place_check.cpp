#include "render/line_place_check.hpp"

#include <map>

#include "render/painter.hpp"

namespace chwp::render {

namespace {

using paint_detail::GlyphSpot;
using paint_detail::Painter;

// 줄을 그릴 때 글자를 놓는 자리. 마크 번호마다 첫 글자 하나다(각주 번호는 한 마크에 여러 글자).
std::map<int, std::int32_t> drawnGlyphStarts(const model::Document& doc, const Node& line) {
    Painter painter;
    painter.doc = &doc;
    painter.scale = 1.f;
    painter.bitmap.width = 1;
    painter.bitmap.height = 1;
    painter.bitmap.px.assign(4, 0);
    painter.bitmap.clipR = 1;
    painter.bitmap.clipB = 1;
    painter.skipGlyphs = true;
    painter.placesOnly = true;
    painter.skipInlineObjects = true;
    std::vector<GlyphSpot> spots;
    painter.placeSink = &spots;
    painter.line(line, 0, 0, false);
    std::map<int, std::int32_t> starts;
    for (const GlyphSpot& spot : spots) {
        if (spot.offset < line.start) continue;
        starts.emplace(spot.offset - line.start, spot.huX);
    }
    return starts;
}

// 덧말·글자 겹치기 묶음 안 줄은 그리기만 하는 줄이다. 캐럿은 묶음 전체를 한 칸으로 지난다.
bool drawOnlyLines(const Node& node) { return node.kind == NodeKind::Object && node.role == "glyphGroup"; }

void checkNode(const model::Document& doc, const Node& node, LinePlaceCheck& out) {
    if (drawOnlyLines(node)) return;
    if (node.kind == NodeKind::Line) {
        ++out.lines;
        Painter caret;
        caret.doc = &doc;
        const std::vector<std::int32_t> edges = caret.glyphEdges(node, 0, 0, nullptr);
        const std::vector<Painter::Mark> marks = caret.caretMarks(node);
        for (const auto& [index, drawnX] : drawnGlyphStarts(doc, node)) {
            if (index >= static_cast<int>(edges.size())) continue;
            ++out.compared;
            // 칸 안에 글자를 미는 폭(세로쓰기 세운 글자, 글자 격자). 캐럿은 칸 경계에 선다.
            const std::int32_t lead =
                index < static_cast<int>(marks.size()) ? marks[static_cast<std::size_t>(index)].lead : 0;
            if (edges[static_cast<std::size_t>(index)] + lead == drawnX) continue;
            ++out.mismatched;
            if (out.firstMiss.empty()) {
                out.firstMiss = "paragraph " + std::to_string(node.paragraph) + " offset " +
                                std::to_string(node.start + index) + " drawn " + std::to_string(drawnX) +
                                " caret " + std::to_string(edges[static_cast<std::size_t>(index)]);
            }
        }
    }
    for (const Node& child : node.children) checkNode(doc, child, out);
}

// 그린 글자의 기준선. 마크 번호마다 첫 글자 하나다.
std::map<int, std::int32_t> drawnGlyphBaselines(const model::Document& doc, const Node& line) {
    Painter painter;
    painter.doc = &doc;
    painter.scale = 1.f;
    painter.bitmap.width = 1;
    painter.bitmap.height = 1;
    painter.bitmap.px.assign(4, 0);
    painter.bitmap.clipR = 1;
    painter.bitmap.clipB = 1;
    painter.skipGlyphs = true;
    painter.placesOnly = true;
    painter.skipInlineObjects = true;
    std::vector<GlyphSpot> spots;
    painter.placeSink = &spots;
    painter.line(line, 0, 0, false);
    std::map<int, std::int32_t> bases;
    for (const GlyphSpot& spot : spots) {
        if (spot.offset < line.start) continue;
        bases.emplace(spot.offset - line.start, spot.baseline);
    }
    return bases;
}

void checkTops(const model::Document& doc, const Node& node, LinePlaceCheck& out) {
    if (drawOnlyLines(node)) return;
    if (node.kind == NodeKind::Line) {
        ++out.lines;
        Painter caret;
        caret.doc = &doc;
        const std::vector<Painter::Mark> marks = caret.caretMarks(node);
        for (const auto& [index, drawnBase] : drawnGlyphBaselines(doc, node)) {
            if (index < 0 || index >= static_cast<int>(marks.size())) continue;
            const Painter::Mark& mark = marks[static_cast<std::size_t>(index)];
            if (mark.object || !mark.noteText.empty() || mark.superscript || mark.subscript) continue;
            // 이 글자 바로 뒤 캐럿은 이 글자로 높이를 잡는다. 다음 마크가 개체면 그쪽으로 넘어간다.
            if (index + 1 < static_cast<int>(marks.size()) && marks[static_cast<std::size_t>(index) + 1].object) {
                continue;
            }
            const CaretPlace place =
                caret.placeOnLineAtOffset(Painter::StoryLine{&node, 0, 0}, node.start + index + 1);
            if (!place.hit) continue;
            const Painter::GlyphBase base = Painter::glyphBaseIn(node, mark.size, mark.font, mark.charOffset);
            const std::int32_t drawnTop = drawnBase - base.ownBase;
            ++out.compared;
            if (place.y == drawnTop) continue;
            ++out.mismatched;
            if (out.firstMiss.empty()) {
                out.firstMiss = "paragraph " + std::to_string(node.paragraph) + " offset " +
                                std::to_string(node.start + index) + " drawn top " + std::to_string(drawnTop) +
                                " caret top " + std::to_string(place.y) + " align " + node.vertAlign;
            }
        }
    }
    for (const Node& child : node.children) checkTops(doc, child, out);
}

}  // namespace

LinePlaceCheck checkCaretTops(const model::Document& doc, const std::vector<Node>& pages) {
    LinePlaceCheck out;
    for (const Node& page : pages) checkTops(doc, page, out);
    return out;
}

LinePlaceCheck checkLinePlaces(const model::Document& doc, const std::vector<Node>& pages) {
    LinePlaceCheck out;
    for (const Node& page : pages) checkNode(doc, page, out);
    return out;
}

std::vector<std::int32_t> cachedCaretEdges(const model::Document& doc, const Node& line) {
    Painter caret;
    caret.doc = &doc;
    return caret.ensureLineBlockCache(line, false, false).relEdges;
}

std::vector<std::int32_t> freshCaretEdges(const model::Document& doc, const Node& line) {
    Painter caret;
    caret.doc = &doc;
    return caret.glyphEdges(line, 0, 0, nullptr);
}

std::vector<std::string> lineMarkLabels(const model::Document& doc, const Node& line) {
    Painter painter;
    painter.doc = &doc;
    const std::vector<Painter::Mark> marks = painter.collectMarks(line);
    std::vector<std::string> out;
    for (const Painter::LineLabel& label : painter.placeLine(line, marks).labels) out.push_back(label.text);
    return out;
}

int transparentBorderPieces(const model::Document& doc, const Node& table) {
    std::vector<paint_detail::BorderPiece> pieces;
    Painter painter;
    painter.doc = &doc;
    painter.borderSink = &pieces;
    painter.paintTableBorders(table, 0, 0);
    int count = 0;
    for (const paint_detail::BorderPiece& piece : pieces) {
        if (piece.r == 0xFF && piece.g == 0 && piece.b == 0) ++count;
    }
    return count;
}

std::vector<std::string> lineProofSpans(const model::Document& doc, const Node& line) {
    Painter painter;
    painter.doc = &doc;
    const std::vector<Painter::Mark> marks = painter.collectMarks(line);
    const Painter::LinePlacement place = painter.placeLine(line, marks);
    std::vector<std::string> out;
    for (const Painter::ProofSpan& span : painter.lineProofs(line, marks, place)) {
        model::Control probe;
        probe.kind = model::ControlKind::Field;
        for (const char* id : {"%%*d", "%%*a", "%%*C", "%%*S", "%%*T", "%%*P", "%%*L", "%%*c", "%%*h", "%%*A", "%%*i",
                               "%%*t", "%%*r", "%%*l", "%%*n", "%%*e", "%spl", "%%mr", "%sig"}) {
            if (model::proofKindOfId(id) == span.kind) probe.ctrlId = id;
        }
        probe.command = span.kind == model::ProofKind::Sign ? "$RevisionSign;" + std::to_string(span.sign) + ";" : "";
        out.push_back(model::proofName(probe) + " " + std::to_string(span.x0) + " " + std::to_string(span.x1) + " " +
                      (span.head ? "1" : "0") + " " + (span.tail ? "1" : "0") +
                      (span.split >= 0 ? " split " + std::to_string(span.split) : std::string()) +
                      (span.text.empty() ? std::string() : " text " + span.text));
    }
    return out;
}

}  // namespace chwp::render
