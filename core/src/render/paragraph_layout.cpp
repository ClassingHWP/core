#include "chwp/render/paragraph_layout.hpp"

#include <atomic>

namespace chwp::render {

namespace {

std::atomic<std::uint32_t> gNextLayoutVersion{1};

bool sameHeading(const HeadingRun* a, const HeadingRun* b);

bool sameContent(const LineContent* a, const LineContent* b) {
    if (a == b) return true;
    static const LineContent none;
    const LineContent& x = a ? *a : none;
    const LineContent& y = b ? *b : none;
    return x.text == y.text && x.glyphs.runs == y.glyphs.runs && x.glyphs.advances == y.glyphs.advances &&
           x.glyphs.inks == y.glyphs.inks && sameHeading(x.glyphs.heading.get(), y.glyphs.heading.get());
}

bool sameHeading(const HeadingRun* a, const HeadingRun* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return a->text == b->text && a->advances == b->advances && a->x == b->x && a->width == b->width &&
           a->fontSize == b->fontSize && a->charShapeId == b->charShapeId && a->font == b->font &&
           a->color == b->color && a->ratio == b->ratio && a->relSize == b->relSize && a->bold == b->bold &&
           a->italic == b->italic && a->superscript == b->superscript && a->subscript == b->subscript;
}

void collectNode(const Node& node, std::unordered_map<std::uint32_t, ParagraphLayout>& out) {
    if (node.kind == NodeKind::Paragraph && node.paragraphId != 0) {
        ParagraphLayout& para = out[node.paragraphId];
        // 반복 제목줄처럼 같은 문단을 다른 쪽에 다시 찍은 사본은 앞 조각보다 앞에서 시작한다. 처음 놓인 것만 든다.
        int firstStart = -1;
        for (const Node& line : node.children) {
            if (line.kind == NodeKind::Line) {
                firstStart = line.start;
                break;
            }
        }
        if (!para.lines.empty() && firstStart >= 0 && firstStart <= para.lines.back().start) {
            for (const Node& child : node.children) collectNode(child, out);
            return;
        }
        para.height += node.height;
        for (const Node& line : node.children) {
            if (line.kind != NodeKind::Line) continue;
            if (!para.heading && line.glyphs().heading) para.heading = line.glyphs().heading;
            para.lines.push_back(LineLayout{line.start, line.end, lineSegFor(line, node.y + line.y), line.content});
        }
    }
    for (const Node& child : node.children) collectNode(child, out);
}

}  // namespace

bool LineLayout::operator==(const LineLayout& other) const {
    return start == other.start && end == other.end && seg == other.seg && sameContent(content.get(), other.content.get());
}

bool ParagraphLayout::sameLayout(const ParagraphLayout& other) const {
    return height == other.height && lines == other.lines && sameHeading(heading.get(), other.heading.get());
}

const ParagraphLayout* ParagraphLayouts::find(std::uint32_t id) const {
    const auto found = byId_.find(id);
    return found == byId_.end() ? nullptr : &found->second;
}

const ParagraphLayout* ParagraphLayouts::put(std::uint32_t id, ParagraphLayout layout) {
    if (id == 0) return nullptr;
    const auto found = byId_.find(id);
    if (found != byId_.end() && found->second.sameLayout(layout)) return &found->second;
    layout.version = gNextLayoutVersion.fetch_add(1, std::memory_order_relaxed);
    ParagraphLayout& slot = byId_[id];
    slot = std::move(layout);
    return &slot;
}

void ParagraphLayouts::collect(const std::vector<Node>& pages) {
    // 일꾼이 쪽 조각을 채우는 중에 줄을 읽지 않게 쪽 채우기 잠금을 잡는다.
    PageFillGuard guard;
    std::unordered_map<std::uint32_t, ParagraphLayout> fresh;
    for (const Node& page : pages) {
        for (const Node& child : page.children) {
            // 머리말·꼬리말·각주·바탕쪽은 쪽 번호 등을 넣은 문단 사본으로 짠다. 원래 문단 자리와 맞지 않아 건너뛴다.
            if (child.kind != NodeKind::Body || child.pageBand || child.fromMaster || !child.role.empty()) continue;
            collectNode(child, fresh);
        }
    }
    for (auto it = byId_.begin(); it != byId_.end();) {
        if (fresh.count(it->first) == 0) it = byId_.erase(it);
        else ++it;
    }
    for (auto& [id, layout] : fresh) put(id, std::move(layout));
}

}  // namespace chwp::render
