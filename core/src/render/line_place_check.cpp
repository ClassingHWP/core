#include "render/line_place_check.hpp"

#include <map>

#include "render/painter.hpp"

namespace noting::render {

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

void checkNode(const model::Document& doc, const Node& node, LinePlaceCheck& out) {
    if (node.kind == NodeKind::Line) {
        ++out.lines;
        Painter caret;
        caret.doc = &doc;
        const std::vector<std::int32_t> edges = caret.glyphEdges(node, 0, 0, nullptr);
        for (const auto& [index, drawnX] : drawnGlyphStarts(doc, node)) {
            if (index >= static_cast<int>(edges.size())) continue;
            ++out.compared;
            if (edges[static_cast<std::size_t>(index)] == drawnX) continue;
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
    if (node.kind == NodeKind::Line) {
        ++out.lines;
        Painter caret;
        caret.doc = &doc;
        const std::vector<Painter::Mark> marks = caret.collectMarks(node);
        for (const auto& [index, drawnBase] : drawnGlyphBaselines(doc, node)) {
            if (index < 0 || index >= static_cast<int>(marks.size())) continue;
            const Painter::Mark& mark = marks[static_cast<std::size_t>(index)];
            if (mark.object || mark.noteSpan || mark.superscript || mark.subscript) continue;
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

}  // namespace noting::render
