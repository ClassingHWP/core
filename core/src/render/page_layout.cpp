// 쪽 하나의 그려진 배치(줄·칸·개체 상자). 그리기와 같은 자리 계산(Painter::placeLine)을 쓰고, 그리지는 않는다.
// agent 가 그림 없이 넘침·잘림·줄 나눔을 보려고 쓴다.

#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace chwp::render {

namespace {

using paint_detail::Painter;

// 칸 노드가 담은 글 묶음. 칸 안 첫 줄(또는 내용 노드)의 story 다.
int storyInside(const Node& node, int depth = 4) {
    for (const Node& child : node.children) {
        if (child.story > 0 && child.kind != NodeKind::Object) return child.story;
        if (depth > 0) {
            if (const int s = storyInside(child, depth - 1); s > 0) return s;
        }
    }
    return -1;
}

struct Describer {
    const model::Document& doc;
    const std::vector<Node>& pages;
    Painter painter;
    PageLayout& out;
    bool band = false;
    bool master = false;
    bool turned = false;
    int objectDepth = 0;  // 개체·칸 안이면 1 이상

    Describer(const model::Document& d, const std::vector<Node>& p, PageLayout& o) : doc(d), pages(p), out(o) {
        painter.doc = &doc;
        painter.scale = 1.f;
    }

    void box(const Node& node, const char* kind, std::int32_t x, std::int32_t y, bool inlineChar) {
        LayoutBox b;
        b.kind = kind;
        b.x = x;
        b.y = y;
        b.width = node.width;
        b.height = node.height;
        b.inlineChar = inlineChar;
        b.band = band;
        b.master = master;
        b.turned = turned;
        if (node.kind == NodeKind::Cell) {
            b.story = storyInside(node);
            b.row = node.ext().row;
            b.col = node.ext().col;
            b.rowSpan = std::max(1, node.ext().rowSpan);
            b.colSpan = std::max(1, node.ext().colSpan);
        } else {
            b.story = node.story;
            b.paragraph = node.paragraph;
        }
        out.boxes.push_back(std::move(b));
    }

    void line(const Node& node, std::int32_t x, std::int32_t y) {
        const std::vector<Painter::Mark> marks = painter.collectMarks(node);
        const Painter::LinePlacement place = painter.placeLine(node, marks);
        LayoutLine l;
        l.story = node.story;
        l.paragraph = node.paragraph;
        if (node.story > 0 && node.paragraph >= 0) {
            l.from = paint_detail::offsetToCharIndex(doc, pages, node.story, node.paragraph, std::max(0, node.start));
            l.to = paint_detail::offsetToCharIndex(doc, pages, node.story, node.paragraph, std::max(0, node.end));
        }
        l.x = x;
        l.y = y;
        l.width = node.width;
        l.height = node.height > 0 ? node.height : node.lineData().textHeight;
        // 글자 끝: 줄 끝 빈칸은 한글처럼 틀 밖으로 나갈 수 있어 넣지 않는다. 글자가 없으면 들여쓰기 자리다.
        l.inkRight = x + place.end;
        for (std::size_t i = std::min(marks.size(), place.at.size()); i-- > 0;) {
            const Painter::Mark& mark = marks[i];
            if (!mark.object && (mark.cp == U' ' || mark.cp == 0x3000)) continue;
            l.inkRight = x + place.at[i] + (mark.object ? mark.object->width : mark.width);
            break;
        }
        if (const HeadingRun* head = lineGlyphs(node).heading.get()) l.heading = head->text;
        l.band = band;
        l.master = master;
        l.turned = turned;
        out.lines.push_back(std::move(l));
        // 글자처럼 넣은 개체: 그리기와 같이 마크 자리에 왼쪽·위 바깥 여백을 더한 곳이다.
        for (std::size_t i = 0; i < marks.size() && i < place.at.size(); ++i) {
            const Painter::Mark& mark = marks[i];
            if (!mark.object) continue;
            const std::int32_t ox = x + place.at[i] + mark.object->marginLeft;
            const std::int32_t oy = y + mark.object->y + mark.object->marginTop;
            object(*mark.object, ox, oy, true);
        }
    }

    // 개체 노드의 왼쪽 위가 (x, y)다. 자식은 그 자리에서 잰다.
    void object(const Node& node, std::int32_t x, std::int32_t y, bool inlineChar) {
        const std::string role = node.role;
        box(node, role.empty() ? "object" : role.c_str(), x, y, inlineChar);
        ++objectDepth;
        for (const Node& child : node.children) walk(child, x, y);
        --objectDepth;
    }

    void walk(const Node& node, std::int32_t ox, std::int32_t oy) {
        if (node.role == "pageFill" || node.role == "pageBorder") return;
        const bool oldBand = band;
        const bool oldMaster = master;
        const bool oldTurned = turned;
        band = band || node.pageBand;
        master = master || node.fromMaster;
        // 세로쓰기 틀 안은 돌린 틀의 좌표다. 그대로 내고 turned 로 알린다.
        turned = turned || node.turned != 0;
        const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
        const std::int32_t y = oy + node.y;
        switch (node.kind) {
            case NodeKind::Line: line(node, x, y); break;
            case NodeKind::Object: object(node, x, y, false); break;
            case NodeKind::Cell:
                box(node, "cell", x, y, false);
                ++objectDepth;
                for (const Node& child : node.children) walk(child, x, y);
                --objectDepth;
                break;
            case NodeKind::Body:
                if (!band && !master && out.bodyWidth == 0) {
                    out.bodyX = x;
                    out.bodyY = y;
                    out.bodyWidth = node.width;
                    out.bodyHeight = node.height;
                }
                for (const Node& child : node.children) walk(child, x, y);
                break;
            case NodeKind::Column:
                // 본문 단만 낸다. 칸·글상자 안의 단은 그 칸 상자로 충분하다.
                if (!band && !master && objectDepth == 0) box(node, "column", x, y, false);
                for (const Node& child : node.children) walk(child, x, y);
                break;
            default:
                for (const Node& child : node.children) walk(child, x, y);
                break;
        }
        band = oldBand;
        master = oldMaster;
        turned = oldTurned;
    }
};

}  // namespace

PageLayout describePage(const model::Document& doc, std::vector<Node>& pages, int pageIndex) {
    PageLayout out;
    if (pageIndex < 0 || pageIndex >= static_cast<int>(pages.size())) return out;
    Node& page = pages[static_cast<std::size_t>(pageIndex)];
    // 줄 글자·문단 번호는 그 쪽을 그리기 전에 채운다. 그리기와 같은 자리를 얻으려고 먼저 채운다.
    ensurePageSpans(doc, page);
    out.width = page.width;
    out.height = page.height;
    Describer describer(doc, pages, out);
    describer.painter.pageNode = &page;
    describer.painter.pageIndex = pageIndex;
    describer.walk(page, 0, 0);
    return out;
}

}  // namespace chwp::render
