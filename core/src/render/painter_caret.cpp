#include "render/painter.hpp"

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "chwp/common/unicode.hpp"
#include "chwp/reader.hpp"
#include "chwp/render/view.hpp"
#include "render/equation_raster.hpp"

// Painter: 캐럿과 선택: 글자 경계, 줄 찾기, 선택 칸, 복사

namespace chwp::render::paint_detail {

std::vector<std::int32_t> Painter::glyphEdgesWithMarks(const Node& node, const std::vector<Mark>& marks,
                                                       std::int32_t lineX, std::int32_t lineY,
                                                       std::vector<NestedCaret>* nested,
                                                       std::vector<ObjectBand>* bands) {
    // 글자를 그리는 Painter::line 과 같은 자리다. 정렬 식을 여기 다시 쓰지 않는다.
    // 자동 번호 대신 넣은 글자는 자리만 차지한다. 커서 경계는 원래 문서 글자에만 둔다.
    const LinePlacement place = placeLine(node, marks);
    std::vector<std::int32_t> edges;
    edges.reserve(marks.size() + 1);
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const Mark& mark = marks[i];
        if (mark.synthetic) continue;
        const int caretIndex = static_cast<int>(edges.size());
        const std::int32_t at = lineX + place.at[i];
        edges.push_back(at);
        if (mark.object && nested) {
            nested->push_back({at + mark.object->marginLeft, lineY + mark.object->y + mark.object->marginTop,
                               mark.object});
        }
        if (mark.object && bands) {
            const std::int32_t ox = at + mark.object->marginLeft;
            const std::int32_t oy = lineY + mark.object->y + mark.object->marginTop;
            const std::int32_t ow = std::max<std::int32_t>(0, mark.object->width);
            const std::int32_t oh = std::max<std::int32_t>(
                0, keepsFileHeight(*mark.object) ? std::max(mark.object->height, paintedExtent(*mark.object))
                                                 : mark.object->height);
            std::int32_t x0 = ox;
            std::int32_t y0 = oy;
            std::int32_t x1 = ox + ow;
            std::int32_t y1 = oy + oh;
            bool any = x1 > x0 && y1 > y0;
            for (const Node& child : mark.object->children) {
                if (child.role == "caption" || keepsFileHeight(*mark.object)) {
                    expandObjectBandBounds(child, ox, oy, x0, y0, x1, y1, any);
                }
            }
            if (any && x1 > x0 && y1 > y0) {
                bands->push_back(ObjectBand{x0, y0, x1 - x0, y1 - y0, caretIndex});
            }
        }
    }
    edges.push_back(lineX + place.end);
    return edges;
}

std::vector<std::int32_t> Painter::glyphEdges(const Node& node, std::int32_t lineX, std::int32_t lineY,
                                              std::vector<NestedCaret>* nested, std::vector<ObjectBand>* bands) {
    return glyphEdgesWithMarks(node, collectMarks(node), lineX, lineY, nested, bands);
}

void Painter::chooseCaretEdge(const Node& node, const std::vector<std::int32_t>& edges, std::int32_t px, bool drag,
                              int anchorPara, int anchorOff, int& chosenIndex, std::int32_t& chosen) const {
    chosen = edges.front();
    chosenIndex = 0;
    if (!drag) {
        std::int32_t bestDist = px > chosen ? px - chosen : chosen - px;
        for (int i = 0; i < static_cast<int>(edges.size()); ++i) {
            const std::int32_t edge = edges[static_cast<std::size_t>(i)];
            const std::int32_t dist = px > edge ? px - edge : edge - px;
            if (dist < bestDist || (dist == bestDist && edge > chosen)) {
                bestDist = dist;
                chosen = edge;
                chosenIndex = i;
            }
        }
        return;
    }
    const int last = static_cast<int>(edges.size()) - 1;
    if (px <= edges.front()) return;
    if (px >= edges.back()) {
        chosenIndex = last;
        chosen = edges.back();
        return;
    }
    int cell = 0;
    while (cell < last && px >= edges[static_cast<std::size_t>(cell + 1)]) ++cell;
    if (cell >= last) {
        chosenIndex = last;
        chosen = edges.back();
        return;
    }
    const std::int32_t cellLeft = edges[static_cast<std::size_t>(cell)];
    const std::int32_t cellRight = edges[static_cast<std::size_t>(cell + 1)];
    const std::int32_t width = cellRight - cellLeft;
    const std::int32_t into = px - cellLeft;
    const int rightOff = node.start + cell + 1;
    const bool backward = anchorPara >= 0 && !posBefore(anchorPara, anchorOff, node.paragraph, rightOff);
    bool passed = false;
    if (width > 0) {
        if (!backward) passed = static_cast<std::int64_t>(into) * 5 >= static_cast<std::int64_t>(width) * 3;
        else passed = static_cast<std::int64_t>(into) * 5 <= static_cast<std::int64_t>(width) * 2;
    }
    chosenIndex = !backward ? (passed ? cell + 1 : cell) : (passed ? cell : cell + 1);
    chosen = edges[static_cast<std::size_t>(chosenIndex)];
}

bool Painter::lineHasObjectChild(const Node& line) {
    for (const Node& child : line.children) {
        if (child.kind == NodeKind::Object) return true;
    }
    return false;
}

bool Painter::subtreeHasStory(const Node& node, int story) {
    if (story <= 0) return false;
    if (node.story == story &&
        (node.kind == NodeKind::Column || node.kind == NodeKind::Paragraph || node.kind == NodeKind::Line)) {
        return true;
    }
    if (node.kind == NodeKind::Line) {
        for (const Node& child : node.children) {
            if (child.kind == NodeKind::Object && subtreeHasStory(child, story)) return true;
        }
        return false;
    }
    for (const Node& child : node.children) {
        if (subtreeHasStory(child, story)) return true;
    }
    return false;
}

bool Painter::subtreeHasStoryRange(const Node& node, int story, int fromParagraph, int toParagraph) {
    if (story <= 0) return false;
    if (node.kind == NodeKind::Column && node.story == story) {
        for (const Node& child : node.children) {
            if (child.kind == NodeKind::Paragraph && child.story == story &&
                child.paragraph >= fromParagraph && child.paragraph <= toParagraph) {
                return true;
            }
        }
        return false;
    }
    if (node.story == story && (node.kind == NodeKind::Paragraph || node.kind == NodeKind::Line)) {
        return node.paragraph >= fromParagraph && node.paragraph <= toParagraph;
    }
    if (node.kind == NodeKind::Line) {
        for (const Node& child : node.children) {
            if (child.kind == NodeKind::Object &&
                subtreeHasStoryRange(child, story, fromParagraph, toParagraph)) {
                return true;
            }
        }
        return false;
    }
    for (const Node& child : node.children) {
        if (subtreeHasStoryRange(child, story, fromParagraph, toParagraph)) return true;
    }
    return false;
}

void Painter::hitLine(const Node& node, std::int32_t lineX, std::int32_t lineY, std::int32_t px, std::int32_t py,
                      CaretPlace& best, bool requireInside, bool drag, int anchorPara, int anchorOff) {
    // 세로쓰기 틀의 줄이면 종이 점을 틀 점으로 바꿔 잰다. 안 개체로 다시 들어갈 때는 종이 점을 넘긴다.
    const std::int32_t paperPx = px;
    const std::int32_t paperPy = py;
    paperToTurn(caretTurn, px, py);
    const std::int32_t span = node.height > 0 ? node.height : (node.lineData().textHeight > 0 ? node.lineData().textHeight : node.fontSize);
    const std::int32_t top = lineY - std::max<std::int32_t>(0, node.lineData().auditRise);
    const std::int32_t bottom = lineY + std::max<std::int32_t>(span, 1) + std::max<std::int32_t>(0, node.lineData().auditBelow);
    const std::int32_t left = lineX;
    const std::int32_t right = left + std::max<std::int32_t>(node.width, 1);
    const bool inLineBox = py >= top && py < bottom && px >= left && px <= right;
    const bool hasObject = lineHasObjectChild(node);
    if (requireInside && !inLineBox && !hasObject) return;

    const LineBlockCacheEntry& cache = ensureLineBlockCache(node, hasObject, requireInside && hasObject);
    const std::vector<std::int32_t>& relEdges = cache.relEdges;
    if (relEdges.empty()) return;

    bool insideOuter = !requireInside || inLineBox;
    if (!insideOuter && requireInside) {
        std::int32_t bandTop = top;
        std::int32_t bandBottom = bottom;
        for (const LineBlockBand& band : cache.bands) {
            const std::int32_t bx = lineX + band.relX;
            const std::int32_t by = lineY + band.relY;
            bandTop = std::min(bandTop, by);
            bandBottom = std::max(bandBottom, by + band.height);
            if (px >= bx && px <= bx + band.width && py >= by && py < by + band.height) {
                insideOuter = true;
                break;
            }
        }
        if (!insideOuter && px >= left && px <= right && py >= bandTop && py < bandBottom) {
            insideOuter = true;
        }
    }
    if (insideOuter) {
        std::int32_t relChosen = relEdges.front();
        int chosenIndex = 0;
        chooseCaretEdge(node, relEdges, px - lineX, drag, anchorPara, anchorOff, chosenIndex, relChosen);
        best.hit = true;
        best.x = lineX + relChosen;
        best.story = node.story;
        best.paragraph = node.paragraph;
        best.offset = node.start + chosenIndex;
        best.hasBefore = false;
        best.hasAfter = false;
        best.before = 0;
        best.after = 0;
        const std::vector<Mark> marks = caretMarks(node);
        applyCaretCharMetrics(node, lineY, marks, chosenIndex, best);
        if (chosenIndex > 0 && chosenIndex - 1 < static_cast<int>(marks.size())) {
            best.hasBefore = true;
            best.before = marks[static_cast<std::size_t>(chosenIndex - 1)].cp;
        }
        if (chosenIndex >= 0 && chosenIndex < static_cast<int>(marks.size())) {
            best.hasAfter = true;
            best.after = marks[static_cast<std::size_t>(chosenIndex)].cp;
        }
        turnCaretToPaper(best, caretTurn);
    }
    if (!requireInside) return;
    // 글자처럼 넣은 개체 안 글이 더 위에 있으면 그 줄로 바꾼다.
    for (const LineBlockNested& item : cache.relNested) {
        if (!item.object) continue;
        const CaretTurnScope scope(*this);
        std::int32_t cx = lineX + item.relX;
        std::int32_t cy = lineY + item.relY;
        enterUprightObject(*item.object, cx, cy);
        for (const Node& child : item.object->children) {
            walkCaret(child, cx, cy, paperPx, paperPy, best, drag, anchorPara, anchorOff);
        }
    }
}

void Painter::walkCaret(const Node& node, std::int32_t ox, std::int32_t oy, std::int32_t px, std::int32_t py,
                        CaretPlace& best, bool drag, int anchorPara, int anchorOff) {
    // 세로쓰기 틀은 돌린 틀에서 찾는다. 점은 종이 그대로 넘기고, 줄에서 틀 점으로 바꾼다(hitLine).
    const CaretTurnScope scope(*this);
    bool uprightBox = false;
    if (enterCaretFrame(node, ox, oy, uprightBox) && uprightBox) {
        for (const Node& child : node.children) walkCaret(child, ox, oy, px, py, best, drag, anchorPara, anchorOff);
        return;
    }
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Line) {
        hitLine(node, x, y, px, py, best, true, drag, anchorPara, anchorOff);
        return;
    }
    if (node.treatAsChar && node.kind == NodeKind::Object) return;
    std::vector<const Node*> behind;
    std::vector<const Node*> front;
    std::vector<const Node*> bands;
    std::vector<const Node*> flow;
    for (const auto& child : node.children) {
        if (child.pageBand) {
            bands.push_back(&child);
            continue;
        }
        if (child.kind == NodeKind::Object && !child.treatAsChar) {
            const bool masterBack = child.fromMaster && !child.masterOnTop;
            const bool masterFront = child.fromMaster && !child.masterOnTop;
            if (masterFront || (!masterBack && child.ext().obj().textWrap != "BEHIND_TEXT")) front.push_back(&child);
            else behind.push_back(&child);
        } else {
            flow.push_back(&child);
        }
    }
    const auto byZ = [](const Node* a, const Node* b) { return a->ext().obj().zOrder < b->ext().obj().zOrder; };
    std::stable_sort(behind.begin(), behind.end(), byZ);
    std::stable_sort(front.begin(), front.end(), byZ);
    for (const Node* child : behind) walkCaret(*child, x, y, px, py, best, drag, anchorPara, anchorOff);
    for (const Node* child : bands) walkCaret(*child, x, y, px, py, best, drag, anchorPara, anchorOff);
    for (const Node* child : flow) walkCaret(*child, x, y, px, py, best, drag, anchorPara, anchorOff);
    for (const Node* child : front) walkCaret(*child, x, y, px, py, best, drag, anchorPara, anchorOff);
}

void Painter::gatherStory(const Node& node, std::int32_t ox, std::int32_t oy, int story, std::vector<StoryLine>& out) {
    if ((node.kind == NodeKind::Object || node.kind == NodeKind::Cell) && !subtreeHasStory(node, story)) return;
    const CaretTurnScope scope(*this);
    bool uprightBox = false;
    if (enterCaretFrame(node, ox, oy, uprightBox) && uprightBox) {
        for (const Node& child : node.children) gatherStory(child, ox, oy, story, out);
        return;
    }
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Line) {
        if (node.story == story) {
            out.push_back({&node, x, y, caretTurn});
            return;
        }
        bool anyNestedStory = false;
        for (const Node& child : node.children) {
            if (child.kind == NodeKind::Object && subtreeHasStory(child, story)) {
                anyNestedStory = true;
                break;
            }
        }
        if (!anyNestedStory) return;
        enterPaintedObjects(
            node, x, y, [&](const Node& child, std::int32_t cx, std::int32_t cy, const Node* /*painted*/) {
                if (!subtreeHasStory(child, story)) return;
                gatherStory(child, cx, cy, story, out);
            });
        return;
    }
    for (const Node& child : node.children) {
        gatherStory(child, x, y, story, out);
    }
}

void Painter::gatherStoryParagraph(const Node& node, std::int32_t ox, std::int32_t oy, int story, int paraIdx,
                                   std::vector<StoryLine>& out) {
    if ((node.kind == NodeKind::Object || node.kind == NodeKind::Cell) &&
        !subtreeHasStoryRange(node, story, paraIdx, paraIdx)) {
        return;
    }
    const CaretTurnScope scope(*this);
    bool uprightBox = false;
    if (enterCaretFrame(node, ox, oy, uprightBox) && uprightBox) {
        for (const Node& child : node.children) gatherStoryParagraph(child, ox, oy, story, paraIdx, out);
        return;
    }
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Column && node.story == story) {
        for (const Node& child : node.children) {
            if (child.kind == NodeKind::Paragraph && child.story == story && child.paragraph == paraIdx) {
                const std::int32_t px = x + child.x + child.marginLeft;
                const std::int32_t py = y + child.y;
                for (const Node& line : child.children) {
                    if (line.kind == NodeKind::Line && line.story == story) {
                        out.push_back({&line, px + line.x, py + line.y, caretTurn});
                    }
                }
            }
        }
        return;
    }
    if (node.kind == NodeKind::Line) {
        if (node.story == story) {
            if (node.paragraph == paraIdx) out.push_back({&node, x, y, caretTurn});
            return;
        }
        bool anyNestedStory = false;
        for (const Node& child : node.children) {
            if (child.kind == NodeKind::Object && subtreeHasStoryRange(child, story, paraIdx, paraIdx)) {
                anyNestedStory = true;
                break;
            }
        }
        if (!anyNestedStory) return;
        enterPaintedObjects(
            node, x, y, [&](const Node& child, std::int32_t cx, std::int32_t cy, const Node* /*painted*/) {
                if (!subtreeHasStoryRange(child, story, paraIdx, paraIdx)) return;
                gatherStoryParagraph(child, cx, cy, story, paraIdx, out);
            });
        return;
    }
    for (const Node& child : node.children) {
        gatherStoryParagraph(child, x, y, story, paraIdx, out);
    }
}

CaretPlace Painter::placeOnLine(const StoryLine& line, std::int32_t px, bool drag, int anchorPara, int anchorOff) {
    CaretPlace place;
    hitLine(*line.node, line.x, line.y, px, line.y, place, false, drag, anchorPara, anchorOff);
    turnCaretToPaper(place, line.turn);
    return place;
}

void Painter::applyCaretCharMetrics(const Node& node, std::int32_t lineY, const std::vector<Mark>& marks, int chosenIndex,
                                    CaretPlace& best) const {
    const model::Paragraph* para = doc ? storyParagraph(*doc, node.story, node.paragraph) : nullptr;
    best.styleId = para ? para->styleId : 0;
    best.paraShapeId = para ? para->paraShapeId : 0;

    const Mark* refMark = nullptr;
    if (!marks.empty()) {
        if (chosenIndex > 0 && chosenIndex - 1 < static_cast<int>(marks.size())) {
            const Mark& prev = marks[static_cast<std::size_t>(chosenIndex - 1)];
            if (prev.object && chosenIndex < static_cast<int>(marks.size()) &&
                !marks[static_cast<std::size_t>(chosenIndex)].object) {
                refMark = &marks[static_cast<std::size_t>(chosenIndex)];
            } else {
                refMark = &prev;
            }
        } else {
            refMark = &marks.front();
        }
    }

    std::uint32_t charShapeId = refMark ? refMark->charShapeId : node.charShapeId;
    if ((!refMark || (refMark->object && refMark->charShapeId == 0)) && para) {
        if (!para->runs.empty()) {
            charShapeId = para->runs.front().charShapeId;
        } else if (doc) {
            if (const model::Style* st = findStyle(*doc, best.styleId)) {
                charShapeId = st->charShapeId;
            }
        }
    }
    if (const auto pending = pendingCharShapeForCaret(best.story, best.paragraph, best.offset)) {
        charShapeId = *pending;
    }
    best.charShapeId = charShapeId;

    const model::CharShape* shape = doc ? findCharShape(*doc, charShapeId) : nullptr;
    std::int32_t charSize = 0;
    if (shape && shape->height > 0) {
        charSize = shape->height;
    }
    if (charSize <= 0) {
        if (refMark && refMark->size > 0 && refMark->noteText.empty()) charSize = refMark->size;
        else if (node.fontSize > 0) charSize = node.fontSize;
        else if (node.lineData().textHeight > 0) charSize = node.lineData().textHeight;
        else charSize = 1000;
    }
    best.height = std::max<std::int32_t>(charSize, 1);

    // 커서 상자는 그 글자를 그린 상자와 같은 자리다. 세로 정렬과 글자 위치(%)는 glyphBaseIn 하나로 정한다.
    // 번호는 3/4로 그려지고 위로 올라간다. 커서는 옆 글자와 같은 칸을 쓴다.
    const int cOff = (refMark && refMark->noteText.empty()) ? refMark->charOffset : 0;
    std::string charFont = (refMark && !refMark->font.empty()) ? refMark->font : node.font.str();
    if (charFont.empty() && doc && shape) charFont = resolveCharShapeFontName(*doc, *shape);
    const GlyphBase base = glyphBaseIn(node, charSize, charFont, cOff);
    best.y = lineY + node.marginTop + base.baseline - base.ownBase;
}

CaretPlace Painter::placeOnLineAtOffset(const StoryLine& line, int offset) {
    CaretPlace best;
    if (!line.node) return best;
    const Node& node = *line.node;
    const std::vector<std::int32_t> edges = glyphEdges(node, line.x, line.y, nullptr);
    if (edges.empty()) return best;
    const int last = static_cast<int>(edges.size()) - 1;
    const int chosenIndex = std::clamp(offset - node.start, 0, last);
    best.hit = true;
    best.x = edges[static_cast<std::size_t>(chosenIndex)];
    best.story = node.story;
    best.paragraph = node.paragraph;
    best.offset = node.start + chosenIndex;
    const std::vector<Mark> marks = caretMarks(node);
    applyCaretCharMetrics(node, line.y, marks, chosenIndex, best);
    if (chosenIndex > 0 && chosenIndex - 1 < static_cast<int>(marks.size())) {
        best.hasBefore = true;
        best.before = marks[static_cast<std::size_t>(chosenIndex - 1)].cp;
    }
    if (chosenIndex >= 0 && chosenIndex < static_cast<int>(marks.size())) {
        best.hasAfter = true;
        best.after = marks[static_cast<std::size_t>(chosenIndex)].cp;
    }
    turnCaretToPaper(best, line.turn);
    return best;
}

const Painter::StoryLine* Painter::nearestLine(const std::vector<StoryLine>& lines, std::int32_t paperX,
                                               std::int32_t paperY) const {
    const StoryLine* best = nullptr;
    std::int32_t bestDy = 0;
    std::int32_t bestDx = 0;
    std::int32_t bestCenter = 0;
    for (const StoryLine& line : lines) {
        if (!line.node) continue;
        // 세로쓰기 줄은 종이 점을 돌린 틀 좌표로 바꿔 잰다.
        std::int32_t x = paperX;
        std::int32_t y = paperY;
        paperToTurn(line.turn, x, y);
        const std::int32_t span =
            line.node->height > 0 ? line.node->height
                                  : (line.node->lineData().textHeight > 0 ? line.node->lineData().textHeight : line.node->fontSize);
        std::int32_t top = line.y - std::max<std::int32_t>(0, line.node->lineData().auditRise);
        std::int32_t bottom =
            line.y + std::max<std::int32_t>(span, 1) + std::max<std::int32_t>(0, line.node->lineData().auditBelow);
        std::int32_t left = line.x;
        std::int32_t right = line.x + std::max<std::int32_t>(line.node->width, 1);
        for (const Node& child : line.node->children) {
            if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
            const std::int32_t objTop = line.y + line.node->marginTop + child.y + child.marginTop;
            const std::int32_t objBottom = objTop + child.height;
            top = std::min(top, objTop);
            bottom = std::max(bottom, objBottom);
            const std::int32_t objLeft = line.x + child.x;
            const std::int32_t objRight = objLeft + std::max<std::int32_t>(child.width, 0);
            left = std::min(left, objLeft);
            right = std::max(right, objRight);
        }
        std::int32_t dy = 0;
        if (y < top) dy = top - y;
        else if (y >= bottom) dy = y - (bottom - 1);
        std::int32_t dx = 0;
        if (x < left) dx = left - x;
        else if (x > right) dx = x - right;
        const std::int32_t mid = left + std::max<std::int32_t>(right - left, 1) / 2;
        const std::int32_t center = x >= mid ? x - mid : mid - x;
        if (!best || dy < bestDy || (dy == bestDy && (dx < bestDx || (dx == bestDx && center < bestCenter)))) {
            best = &line;
            bestDy = dy;
            bestDx = dx;
            bestCenter = center;
        }
    }
    return best;
}

const Painter::StoryLine* Painter::edgeLine(const std::vector<StoryLine>& lines, bool atStart) const {
    const StoryLine* pick = nullptr;
    for (const StoryLine& line : lines) {
        if (!pick) {
            pick = &line;
            continue;
        }
        const bool earlier = posBefore(line.node->paragraph, line.node->start, pick->node->paragraph, pick->node->start);
        const bool later = posBefore(pick->node->paragraph, pick->node->end, line.node->paragraph, line.node->end) ||
                           (line.node->paragraph == pick->node->paragraph && line.node->end >= pick->node->end &&
                            line.node->start >= pick->node->start);
        if (atStart ? earlier : later) pick = &line;
    }
    return pick;
}

std::string Painter::markBackground(const Mark* mark, const std::string& inherited) {
    if (mark) {
        if (paintColor(mark->markpen)) return mark->markpen;
        if (paintColor(mark->shade)) return mark->shade;
        if (paintColor(mark->background)) return mark->background;
    }
    if (paintColor(inherited)) return inherited;
    return "#ffffff";
}

void Painter::pushBlockRgb(std::vector<BlockBox>& out, int page, std::int32_t left, std::int32_t right, std::int32_t top,
                           std::int32_t height, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                           const ClipBounds& clip) const {
    if (right <= left || height <= 0) return;
    // 세로쓰기 틀의 칸은 종이 자리로 돌린다. 줄 방향(left..right)은 종이 세로, 줄 높이는 종이 가로다.
    if (caretTurn.on()) {
        const std::int32_t paperLeft = caretTurn.x + caretTurn.span - (top + height);
        const std::int32_t paperTop = caretTurn.y + left;
        const std::int32_t paperHeight = right - left;
        left = paperLeft;
        right = paperLeft + height;
        top = paperTop;
        height = paperHeight;
    }
    const std::int32_t cLeft = std::max(left, clip.x0);
    const std::int32_t cRight = std::min(right, clip.x1);
    const std::int32_t cTop = std::max(top, clip.y0);
    const std::int32_t cBottom = std::min(top + height, clip.y1);
    if (cRight <= cLeft || cBottom <= cTop) return;
    BlockBox box;
    box.page = page;
    box.x = cLeft;
    box.y = cTop;
    box.width = cRight - cLeft;
    box.height = cBottom - cTop;
    box.backR = r;
    box.backG = g;
    box.backB = b;
    out.push_back(box);
}

void Painter::pushBlock(std::vector<BlockBox>& out, int page, std::int32_t left, std::int32_t right, std::int32_t top,
                        std::int32_t height, const std::string& color, const ClipBounds& clip) const {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    parseHex(color, r, g, b);
    pushBlockRgb(out, page, left, right, top, height, r, g, b, clip);
}

const LineBlockCacheEntry& Painter::ensureLineBlockCache(const Node& node, bool needBands, bool needNested) {
    const std::int32_t firstW = node.children.empty() ? -1 : node.children.front().width;
    const std::size_t firstLen = node.children.empty() ? 0 : node.children.front().text().size();
    std::uint64_t childrenSig = 14695981039346656037ULL;
    for (const Node& child : node.children) {
        childrenSig ^= static_cast<std::uint64_t>(child.kind) + 0x9e3779b97f4a7c15ULL + (childrenSig << 6) +
                       (childrenSig >> 2);
        childrenSig ^= static_cast<std::uint64_t>(child.x) + 0x9e3779b97f4a7c15ULL + (childrenSig << 6) +
                       (childrenSig >> 2);
        childrenSig ^= static_cast<std::uint64_t>(child.width) + 0x9e3779b97f4a7c15ULL + (childrenSig << 6) +
                       (childrenSig >> 2);
        childrenSig ^= static_cast<std::uint64_t>(child.height) + 0x9e3779b97f4a7c15ULL + (childrenSig << 6) +
                       (childrenSig >> 2);
        for (unsigned char c : child.text()) {
            childrenSig = (childrenSig ^ c) * 1099511628211ULL;
        }
        // 글자 자리(placeLine)를 바꾸는 값은 모두 열쇠에 넣는다. 빠지면 옛 캐럿 자리를 그대로 쓴다.
        const std::uint64_t childFlags = (child.headingMark ? 1u : 0u) | (child.treatAsChar ? 4u : 0u);
        childrenSig = (childrenSig ^ childFlags) * 1099511628211ULL;
        childrenSig = (childrenSig ^ static_cast<std::uint32_t>(child.marginLeft)) * 1099511628211ULL;
        childrenSig = (childrenSig ^ static_cast<std::uint32_t>(child.marginRight)) * 1099511628211ULL;
        childrenSig = (childrenSig ^ static_cast<std::uint32_t>(child.y)) * 1099511628211ULL;
        childrenSig = (childrenSig ^ static_cast<std::uint32_t>(child.marginTop)) * 1099511628211ULL;
        childrenSig = (childrenSig ^ static_cast<std::uint32_t>(child.marginBottom)) * 1099511628211ULL;
    }
    // 본문 글자는 줄 글자 표에 있다. 글자·구간·폭이 바뀌면 열쇠가 바뀐다.
    for (unsigned char c : node.text()) childrenSig = (childrenSig ^ c) * 1099511628211ULL;
    const LineGlyphs glyphs = lineGlyphs(node);
    for (const LineRun& run : glyphs.runs) {
        const std::uint64_t runKey = static_cast<std::uint64_t>(run.kind) | (run.synthetic ? 8u : 0u) |
                                     (static_cast<std::uint64_t>(run.textEnd) << 8) |
                                     (static_cast<std::uint64_t>(static_cast<std::uint32_t>(run.width)) << 32);
        childrenSig = (childrenSig ^ runKey) * 1099511628211ULL;
    }
    for (const std::int32_t advance : glyphs.advances) {
        childrenSig = (childrenSig ^ static_cast<std::uint32_t>(advance)) * 1099511628211ULL;
    }
    const std::uint64_t lineFlags = (node.lineData().lastLine ? 1u : 0u) | (node.lineData().lineBreak ? 2u : 0u) |
                                    (node.lineData().deferTabs ? 4u : 0u);
    childrenSig = (childrenSig ^ lineFlags) * 1099511628211ULL;
    for (unsigned char c : node.align) childrenSig = (childrenSig ^ c) * 1099511628211ULL;
    LineBlockCacheEntry& entry = lineBlockCache()[&node];
    const bool match = (entry.story == node.story && entry.paragraph == node.paragraph &&
                        entry.start == node.start && entry.end == node.end &&
                        entry.width == node.width && entry.height == node.height &&
                        entry.textHeight == node.lineData().textHeight && entry.fontSize == node.fontSize &&
                        entry.indent == node.indent && entry.childCount == node.children.size() &&
                        entry.firstChildW == firstW && entry.firstChildTextLen == firstLen &&
                        entry.childrenSig == childrenSig &&
                        !entry.relEdges.empty());
    if (match && (!needBands || entry.bandsBuilt) && (!needNested || entry.nestedBuilt)) {
        return entry;
    }
    const std::vector<Mark> drawn = collectMarks(node);
    std::vector<Mark> marks = drawn;
    marks.erase(std::remove_if(marks.begin(), marks.end(), [](const Mark& mark) { return mark.synthetic; }),
                marks.end());
    const int markCount = static_cast<int>(marks.size());
    bool hasObj = false;
    for (const Mark& m : marks) {
        if (m.object) {
            hasObj = true;
            break;
        }
    }
    const bool buildBands = needBands && hasObj;
    const bool buildNested = needNested && hasObj;
    std::vector<ObjectBand> rawBands;
    std::vector<NestedCaret> rawNested;
    entry.relEdges = glyphEdgesWithMarks(node, drawn, 0, 0,
                                         buildNested ? &rawNested : nullptr,
                                         buildBands ? &rawBands : nullptr);
    entry.story = node.story;
    entry.paragraph = node.paragraph;
    entry.start = node.start;
    entry.end = node.end;
    entry.width = node.width;
    entry.height = node.height;
    entry.textHeight = node.lineData().textHeight;
    entry.fontSize = node.fontSize;
    entry.indent = node.indent;
    entry.childCount = node.children.size();
    entry.firstChildW = firstW;
    entry.firstChildTextLen = firstLen;
    entry.childrenSig = childrenSig;
    entry.endExclusive = std::max(node.end, node.start + markCount);
    const std::int32_t span =
        node.height > 0 ? node.height : (node.lineData().textHeight > 0 ? node.lineData().textHeight : node.fontSize);
    const std::int32_t textBox =
        node.lineData().textHeight > 0 ? node.lineData().textHeight : (node.fontSize > 0 ? node.fontSize : span);
    const std::int32_t gap = std::max<std::int32_t>(0, node.lineData().storedSeg.spacing);
    entry.boxHeight = std::max<std::int32_t>(textBox + gap, 1);

    entry.markHasOwnBg.assign(static_cast<std::size_t>(markCount), 0);
    entry.markR.assign(static_cast<std::size_t>(markCount), 255);
    entry.markG.assign(static_cast<std::size_t>(markCount), 255);
    entry.markB.assign(static_cast<std::size_t>(markCount), 255);
    for (int i = 0; i < markCount; ++i) {
        const Mark& m = marks[static_cast<std::size_t>(i)];
        const std::string* ownColor = nullptr;
        if (paintColor(m.markpen)) ownColor = &m.markpen;
        else if (paintColor(m.shade)) ownColor = &m.shade;
        else if (paintColor(m.background)) ownColor = &m.background;
        if (ownColor) {
            entry.markHasOwnBg[static_cast<std::size_t>(i)] = 1;
            parseHex(*ownColor, entry.markR[static_cast<std::size_t>(i)],
                     entry.markG[static_cast<std::size_t>(i)],
                     entry.markB[static_cast<std::size_t>(i)]);
        }
    }

    entry.fullRuns.clear();
    if (entry.relEdges.size() > 1) {
        const int last = static_cast<int>(entry.relEdges.size()) - 1;
        int run = 0;
        while (run < last) {
            const bool own = run < markCount && entry.markHasOwnBg[static_cast<std::size_t>(run)] != 0;
            const std::uint8_t r = own ? entry.markR[static_cast<std::size_t>(run)] : 255;
            const std::uint8_t g = own ? entry.markG[static_cast<std::size_t>(run)] : 255;
            const std::uint8_t b = own ? entry.markB[static_cast<std::size_t>(run)] : 255;
            int next = run + 1;
            while (next < last) {
                const bool laterOwn = next < markCount && entry.markHasOwnBg[static_cast<std::size_t>(next)] != 0;
                const std::uint8_t lr = laterOwn ? entry.markR[static_cast<std::size_t>(next)] : 255;
                const std::uint8_t lg = laterOwn ? entry.markG[static_cast<std::size_t>(next)] : 255;
                const std::uint8_t lb = laterOwn ? entry.markB[static_cast<std::size_t>(next)] : 255;
                if (laterOwn != own || (own && (lr != r || lg != g || lb != b))) break;
                ++next;
            }
            LineBlockFullRun fr;
            fr.relLeft = entry.relEdges[static_cast<std::size_t>(run)];
            fr.relRight = entry.relEdges[static_cast<std::size_t>(next)];
            fr.r = r;
            fr.g = g;
            fr.b = b;
            fr.hasOwnBg = own;
            entry.fullRuns.push_back(fr);
            run = next;
        }
    }

    if (!hasObj) {
        entry.bands.clear();
        entry.relNested.clear();
        entry.bandsBuilt = true;
        entry.nestedBuilt = true;
    } else {
        if (buildBands) {
            entry.bands.clear();
            entry.bands.reserve(rawBands.size());
            for (const ObjectBand& ob : rawBands) {
                LineBlockBand lb;
                lb.relX = ob.x;
                lb.relY = ob.y;
                lb.width = ob.width;
                lb.height = ob.height;
                lb.index = ob.index;
                if (ob.index >= 0 && ob.index < markCount &&
                    entry.markHasOwnBg[static_cast<std::size_t>(ob.index)] != 0) {
                    lb.hasOwnBg = true;
                    lb.r = entry.markR[static_cast<std::size_t>(ob.index)];
                    lb.g = entry.markG[static_cast<std::size_t>(ob.index)];
                    lb.b = entry.markB[static_cast<std::size_t>(ob.index)];
                }
                entry.bands.push_back(lb);
            }
            entry.bandsBuilt = true;
        }
        if (buildNested) {
            entry.relNested.clear();
            entry.relNested.reserve(rawNested.size());
            for (const NestedCaret& nc : rawNested) {
                entry.relNested.push_back(LineBlockNested{nc.x, nc.y, nc.object});
            }
            entry.nestedBuilt = true;
        }
    }
    return entry;
}

void Painter::appendBlock(const Node& node, std::int32_t ox, std::int32_t oy, int page, int story, int fromParagraph,
                          int fromOffset, int toParagraph, int toOffset, std::vector<BlockBox>& out,
                          const std::string& inheritedFill, const ClipBounds& clip) {
    if (node.kind == NodeKind::Paragraph && node.story == story) {
        if (node.paragraph < fromParagraph || node.paragraph > toParagraph) return;
    }
    if ((node.kind == NodeKind::Object || node.kind == NodeKind::Cell) &&
        !subtreeHasStoryRange(node, story, fromParagraph, toParagraph)) {
        return;
    }
    // 세로쓰기 틀은 돌린 틀에서 칸을 잡고, 칸을 넣을 때 종이 자리로 돌린다(pushBlockRgb).
    const CaretTurnScope scope(*this);
    bool uprightBox = false;
    if (enterCaretFrame(node, ox, oy, uprightBox) && uprightBox) {
        for (const Node& child : node.children) {
            appendBlock(child, ox, oy, page, story, fromParagraph, fromOffset, toParagraph, toOffset, out,
                        inheritedFill, clip);
        }
        return;
    }
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    const std::string fill = paintColor(node.ext().fillColor) ? node.ext().fillColor.str() : inheritedFill;
    if (node.kind == NodeKind::Line) {
        if (node.story == story) {
            if (node.paragraph < fromParagraph || node.paragraph > toParagraph) return;
            if (node.end > node.start) {
                if (!posBefore(fromParagraph, fromOffset, node.paragraph, node.end) ||
                    !posBefore(node.paragraph, node.start, toParagraph, toOffset)) {
                    return;
                }
            }
            const LineBlockCacheEntry& cached = ensureLineBlockCache(node, true, false);
            const int endExclusive = cached.endExclusive;
            const bool hasSpan = endExclusive > node.start;
            const bool covers = hasSpan
                                    ? posBefore(fromParagraph, fromOffset, node.paragraph, endExclusive) &&
                                          posBefore(node.paragraph, node.start, toParagraph, toOffset)
                                    : posBefore(fromParagraph, fromOffset, node.paragraph, node.start) &&
                                          posBefore(node.paragraph, node.start, toParagraph, toOffset);
            if (covers && !cached.relEdges.empty()) {
                const int last = static_cast<int>(cached.relEdges.size()) - 1;
                int begin = 0;
                int finish = last;
                if (fromParagraph == node.paragraph) {
                    begin = std::clamp(fromOffset - node.start, 0, last);
                }
                if (toParagraph == node.paragraph) finish = std::clamp(toOffset - node.start, 0, last);
                if (finish >= begin) {
                    const std::int32_t top = y + node.marginTop;
                    const std::int32_t height = cached.boxHeight;
                    std::uint8_t fillR = 255;
                    std::uint8_t fillG = 255;
                    std::uint8_t fillB = 255;
                    if (paintColor(fill)) parseHex(fill, fillR, fillG, fillB);
                    if (finish == begin && begin == 0 && finish == last) {
                        const std::int32_t left = x + cached.relEdges.front();
                        const std::int32_t right =
                            left + std::max<std::int32_t>(node.width, std::max<std::int32_t>(height, 1));
                        pushBlockRgb(out, page, left, right, top, height, fillR, fillG, fillB, clip);
                    } else if (finish > begin) {
                        if (begin == 0 && finish == last) {
                            for (const LineBlockFullRun& fr : cached.fullRuns) {
                                const std::uint8_t r = fr.hasOwnBg ? fr.r : fillR;
                                const std::uint8_t g = fr.hasOwnBg ? fr.g : fillG;
                                const std::uint8_t b = fr.hasOwnBg ? fr.b : fillB;
                                pushBlockRgb(out, page, x + fr.relLeft, x + fr.relRight, top, height, r, g, b, clip);
                            }
                        } else {
                            const int markCount = static_cast<int>(cached.markHasOwnBg.size());
                            int run = begin;
                            while (run < finish) {
                                const bool own =
                                    run < markCount && cached.markHasOwnBg[static_cast<std::size_t>(run)] != 0;
                                const std::uint8_t r = own ? cached.markR[static_cast<std::size_t>(run)] : fillR;
                                const std::uint8_t g = own ? cached.markG[static_cast<std::size_t>(run)] : fillG;
                                const std::uint8_t b = own ? cached.markB[static_cast<std::size_t>(run)] : fillB;
                                int next = run + 1;
                                while (next < finish) {
                                    const bool lOwn =
                                        next < markCount && cached.markHasOwnBg[static_cast<std::size_t>(next)] != 0;
                                    const std::uint8_t lr =
                                        lOwn ? cached.markR[static_cast<std::size_t>(next)] : fillR;
                                    const std::uint8_t lg =
                                        lOwn ? cached.markG[static_cast<std::size_t>(next)] : fillG;
                                    const std::uint8_t lb =
                                        lOwn ? cached.markB[static_cast<std::size_t>(next)] : fillB;
                                    if (lr != r || lg != g || lb != b) break;
                                    ++next;
                                }
                                pushBlockRgb(out, page,
                                             x + cached.relEdges[static_cast<std::size_t>(run)],
                                             x + cached.relEdges[static_cast<std::size_t>(next)],
                                             top, height, r, g, b, clip);
                                run = next;
                            }
                        }
                        for (const LineBlockBand& band : cached.bands) {
                            if (band.index < begin || band.index >= finish) continue;
                            const std::uint8_t r = band.hasOwnBg ? band.r : fillR;
                            const std::uint8_t g = band.hasOwnBg ? band.g : fillG;
                            const std::uint8_t b = band.hasOwnBg ? band.b : fillB;
                            pushBlockRgb(out, page, x + band.relX, x + band.relX + band.width,
                                         y + band.relY, band.height, r, g, b, clip);
                        }
                    }
                }
            }
            return;
        }
        bool anyNestedRange = false;
        for (const Node& child : node.children) {
            if (child.kind == NodeKind::Object &&
                subtreeHasStoryRange(child, story, fromParagraph, toParagraph)) {
                anyNestedRange = true;
                break;
            }
        }
        if (!anyNestedRange) return;
        enterPaintedObjects(node, x, y,
                            [&](const Node& child, std::int32_t cx, std::int32_t cy, const Node* painted) {
                                if (!subtreeHasStoryRange(child, story, fromParagraph, toParagraph)) return;
                                const std::string& next =
                                    painted && paintColor(painted->ext().fillColor) ? painted->ext().fillColor.str() : fill;
                                const ClipBounds nextClip =
                                    painted ? childClipOf(*painted, cx, cy, child, clip) : clip;
                                appendBlock(child, cx, cy, page, story, fromParagraph, fromOffset, toParagraph,
                                            toOffset, out, next, nextClip);
                            });
        return;
    }
    for (const Node& child : node.children) {
        appendBlock(child, x, y, page, story, fromParagraph, fromOffset, toParagraph, toOffset, out, fill,
                    childClipOf(node, x, y, child, clip));
    }
}

std::string Painter::copySelection(const std::vector<Node>& pages, int story, int fromParagraph, int fromOffset,
                                   int toParagraph, int toOffset) {
    if (story <= 0) return {};
    if (toParagraph < fromParagraph || (toParagraph == fromParagraph && toOffset < fromOffset)) {
        std::swap(fromParagraph, toParagraph);
        std::swap(fromOffset, toOffset);
    }
    if (fromParagraph == toParagraph && fromOffset == toOffset) return {};
    std::vector<StoryLine> lines;
    for (const Node& page : pages) {
        if (!subtreeHasStoryRange(page, story, fromParagraph, toParagraph)) continue;
        if (page.spansLater && doc) ensurePageSpans(*doc, const_cast<Node&>(page));
        gatherStory(page, 0, 0, story, lines);
    }
    std::vector<std::string> parts(static_cast<std::size_t>(toParagraph - fromParagraph + 1));
    for (const StoryLine& line : lines) {
        const Node& node = *line.node;
        if (node.paragraph < fromParagraph || node.paragraph > toParagraph) continue;
        const std::vector<Mark> marks = caretMarks(node);
        const int last = static_cast<int>(marks.size());
        const int endExclusive = std::max(node.end, node.start + last);
        const bool hasSpan = endExclusive > node.start;
        const bool covers = hasSpan
                                ? posBefore(fromParagraph, fromOffset, node.paragraph, endExclusive) &&
                                      posBefore(node.paragraph, node.start, toParagraph, toOffset)
                                : posBefore(fromParagraph, fromOffset, node.paragraph, node.start) &&
                                      posBefore(node.paragraph, node.start, toParagraph, toOffset);
        if (!covers) continue;
        int begin = 0;
        int finish = last;
        if (fromParagraph == node.paragraph) begin = std::clamp(fromOffset - node.start, 0, last);
        if (toParagraph == node.paragraph) finish = std::clamp(toOffset - node.start, 0, last);
        std::string& part = parts[static_cast<std::size_t>(node.paragraph - fromParagraph)];
        for (int i = begin; i < finish; ++i) {
            const Mark& mark = marks[static_cast<std::size_t>(i)];
            if (mark.object) continue;
            if (!mark.noteText.empty()) {
                part += mark.noteText;
                continue;
            }
            if (mark.cp == 0) continue;
            appendUtf8(part, mark.cp);
        }
    }
    std::string text;
    for (int para = fromParagraph; para <= toParagraph; ++para) {
        if (para != fromParagraph) text.push_back('\n');
        text += parts[static_cast<std::size_t>(para - fromParagraph)];
    }
    return text;
}

}  // namespace chwp::render::paint_detail
