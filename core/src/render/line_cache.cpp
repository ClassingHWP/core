#include "noting/render/scene.hpp"

#include "render/column_geom.hpp"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "noting/common/unicode.hpp"

namespace noting::render {

namespace {

using noting::model::CharShape;
using noting::model::Control;
using noting::model::Document;
using noting::model::InlineItem;
using noting::model::Lang;
using noting::model::LineSeg;
using noting::model::PageDef;
using noting::model::ParaShape;
using noting::model::Paragraph;
using noting::model::Section;

constexpr std::uint32_t kPageFirst = 1u;
constexpr std::uint32_t kColumnFirst = 2u;

struct LineHit {
    int section = 0;
    int paragraph = 0;
    int seg = 0;
    std::int32_t origin = 0;  // 이 단 묶음이 본문 위에서 시작하는 위치. 단 노드는 여기 두고 줄의 vertPos 는 단 안 좌표다
    int column = 0;
    int columnCount = 1;
    std::int32_t columnGap = 0;
};

bool nextCodePoint(std::string_view text, std::size_t& index, char32_t& out) {
    if (index >= text.size()) return false;
    const auto c = static_cast<unsigned char>(text[index]);
    if (c < 0x80) {
        out = c;
        index += 1;
        return true;
    }
    if ((c & 0xE0) == 0xC0 && index + 1 < text.size()) {
        out = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
        index += 2;
        return true;
    }
    if ((c & 0xF0) == 0xE0 && index + 2 < text.size()) {
        out = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6) |
              (static_cast<unsigned char>(text[index + 2]) & 0x3F);
        index += 3;
        return true;
    }
    if ((c & 0xF8) == 0xF0 && index + 3 < text.size()) {
        out = ((c & 0x07) << 18) | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
              ((static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
              (static_cast<unsigned char>(text[index + 3]) & 0x3F);
        index += 4;
        return true;
    }
    out = c;
    index += 1;
    return true;
}

int utf16Units(char32_t cp) { return cp > 0xFFFF ? 2 : 1; }

const ParaShape* findParaShape(const Document& doc, std::uint32_t id) {
    const auto& shapes = doc.header.paraShapes;
    if (id < shapes.size() && shapes[id].id == id) return &shapes[id];
    for (const auto& shape : shapes) {
        if (shape.id == id) return &shape;
    }
    return nullptr;
}

const CharShape* findCharShape(const Document& doc, std::uint32_t id) {
    const auto& shapes = doc.header.charShapes;
    if (id < shapes.size() && shapes[id].id == id) return &shapes[id];
    for (const auto& shape : shapes) {
        if (shape.id == id) return &shape;
    }
    return nullptr;
}

bool paragraphStartsPage(const Paragraph& para, const Document& doc) {
    if (para.pageBreak) return true;
    if (const ParaShape* shape = findParaShape(doc, para.paraShapeId)) return shape->pageBreakBefore;
    return false;
}

std::string colorHex(const noting::model::Color& color) {
    if (!color) return {};
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(*color & 0xFFFFFFu));
    return buf;
}

std::int32_t bodyHeight(const Section& section) {
    const PageDef page = section.page.value_or(PageDef{});
    const std::int32_t top = static_cast<std::int32_t>(page.marginTop) + static_cast<std::int32_t>(page.marginHeader);
    const std::int32_t bottom =
        static_cast<std::int32_t>(page.marginBottom) + static_cast<std::int32_t>(page.marginFooter);
    return std::max<std::int32_t>(200, static_cast<std::int32_t>(page.height) - top - bottom);
}

std::int32_t regionGap(const Section& section) {
    return section.columnRegionGap > 0 ? section.columnRegionGap : 1134;
}

std::int32_t bodyWidth(const Section& section) {
    const PageDef page = section.page.value_or(PageDef{});
    const std::int32_t left = static_cast<std::int32_t>(page.marginLeft);
    const std::int32_t right = static_cast<std::int32_t>(page.marginRight);
    return std::max<std::int32_t>(200, static_cast<std::int32_t>(page.width) - left - right);
}

// 문단에 들어 있는 마지막 단 정의. 없으면 false.
bool columnDefOf(const Paragraph& para, int& count, std::int32_t& gap, std::string& type) {
    bool found = false;
    for (const auto& run : para.runs) {
        for (const auto& item : run.items) {
            if (item.type != InlineItem::Type::Control || item.control.kind != noting::model::ControlKind::ColumnDef) continue;
            if (item.control.columnCount == 0) continue;
            count = static_cast<int>(item.control.columnCount);
            gap = item.control.columnGap;
            type = item.control.columnType;
            found = true;
        }
    }
    return found;
}

// lineseg 의 vertPos·horzPos 는 쪽 좌표가 아니라 지금 단 안에서의 위치다.
// 쪽 첫 줄·단 첫 줄 비트는 비어 있는 파일이 있다.
// 세로 위치가 줄거나, 다단에서 같은 높이에 다시 시작하면 같은 쪽의 다음 단이다. 마지막 단이었으면 다음 쪽이다.
// 단 설정이 바뀌면 각 단의 글자 바닥 중 가장 아래 글자 높이 아래 단 간격에 놓고,
// 그 첫 줄의 글자 바닥이 본문 끝에 닿으면 다음 쪽이다. 새 단 설정과 단 나누기가 한 문단에 있으면 왼쪽 단에서 시작한다.
std::vector<std::vector<LineHit>> pagesFromLineSegs(const Document& doc) {
    std::vector<std::vector<LineHit>> pages;
    std::vector<LineHit> page;
    std::optional<std::int32_t> prevVert;
    std::int32_t origin = 0;
    int columnCount = 1;
    std::int32_t columnGap = 0;
    std::string columnType;
    int columnIndex = 0;
    std::vector<std::int32_t> bottoms{0};
    bool hasLine = false;
    auto flush = [&] {
        if (page.empty()) return;
        pages.push_back(std::move(page));
        page.clear();
        prevVert.reset();
        origin = 0;
        columnIndex = 0;
        bottoms.assign(static_cast<std::size_t>(std::max(1, columnCount)), 0);
        hasLine = false;
    };
    for (int si = 0; si < static_cast<int>(doc.sections.size()); ++si) {
        if (!page.empty()) flush();
        columnCount = 1;
        columnGap = 0;
        columnType.clear();
        columnIndex = 0;
        bottoms.assign(1, 0);
        const Section& section = doc.sections[static_cast<std::size_t>(si)];
        const std::int32_t height = bodyHeight(section);
        const std::int32_t gap = regionGap(section);
        for (int pi = 0; pi < static_cast<int>(section.paragraphs.size()); ++pi) {
            const Paragraph& para = section.paragraphs[static_cast<std::size_t>(pi)];
            if (paragraphStartsPage(para, doc) && hasLine) flush();
            if (!hasLine && !para.layoutBreak) {
                int count = 1;
                std::int32_t between = 0;
                std::string type;
                if (columnDefOf(para, count, between, type)) {
                    columnCount = std::max(1, count);
                    columnGap = columnCount > 1 ? std::max<std::int32_t>(0, between) : 0;
                    columnType = std::move(type);
                    bottoms.assign(static_cast<std::size_t>(columnCount), 0);
                }
            }
            const bool balance = balancedColumns(columnType) && columnCount > 1;
            bool region = para.layoutBreak || (balance && para.columnBreak);
            bool columnBreak = para.columnBreak;
            for (int segIndex = 0; segIndex < static_cast<int>(para.lineSegs.size()); ++segIndex) {
                const LineSeg& seg = para.lineSegs[static_cast<std::size_t>(segIndex)];
                const std::int32_t textHeight = seg.textHeight > 0 ? seg.textHeight : seg.vertSize;
                if ((seg.flags & kPageFirst) != 0 && hasLine) flush();
                if (region) {
                    std::int32_t band = 0;
                    for (const std::int32_t bottom : bottoms) band = std::max(band, bottom);
                    std::int32_t nextTop = 0;
                    if (hasLine) {
                        nextTop = origin + band + gap;
                        if (nextTop + textHeight >= height) {
                            flush();
                            nextTop = 0;
                        }
                    }
                    origin = nextTop;
                    if (para.layoutBreak) {
                        int count = 1;
                        std::int32_t between = 0;
                        std::string type;
                        columnDefOf(para, count, between, type);
                        columnCount = std::max(1, count);
                        columnGap = columnCount > 1 ? std::max<std::int32_t>(0, between) : 0;
                        columnType = std::move(type);
                    }
                    columnIndex = 0;
                    bottoms.assign(static_cast<std::size_t>(columnCount), 0);
                    prevVert.reset();
                    region = false;
                    columnBreak = false;
                } else if (hasLine && (columnBreak || (seg.flags & kColumnFirst) != 0 ||
                                        (prevVert && seg.vertPos < *prevVert) ||
                                        (columnCount > 1 && prevVert && seg.vertPos == *prevVert))) {
                    if (columnIndex + 1 < columnCount) {
                        ++columnIndex;
                    } else {
                        flush();
                    }
                    prevVert.reset();
                    columnBreak = false;
                }
                if (columnIndex < 0 || columnIndex >= columnCount) columnIndex = 0;
                if (static_cast<int>(bottoms.size()) != columnCount) {
                    bottoms.assign(static_cast<std::size_t>(columnCount), 0);
                }
                bottoms[static_cast<std::size_t>(columnIndex)] =
                    std::max(bottoms[static_cast<std::size_t>(columnIndex)], seg.vertPos + textHeight);
                page.push_back(LineHit{si, pi, segIndex, origin, columnIndex, columnCount, columnGap});
                prevVert = seg.vertPos;
                hasLine = true;
                columnBreak = false;
            }
        }
    }
    flush();
    if (pages.empty()) pages.emplace_back();
    return pages;
}

struct StreamPiece {
    int begin = 0;
    int end = 0;
    char32_t cp = 0;
    bool text = false;
    bool tab = false;
    std::uint32_t charShapeId = 0;
    std::string markpen;
    std::int32_t objectWidth = 0;
};

std::vector<StreamPiece> paragraphPieces(const Paragraph& para) {
    std::vector<StreamPiece> pieces;
    int pos = 0;
    for (const auto& run : para.runs) {
        for (const auto& item : run.items) {
            if (item.type == InlineItem::Type::Text) {
                std::size_t index = 0;
                char32_t cp = 0;
                while (nextCodePoint(item.text, index, cp)) {
                    StreamPiece piece;
                    piece.begin = pos;
                    // 탭은 lineseg 에서 8칸이다. 줄바꿈은 1칸이고 글자로 그리지 않는다.
                    piece.end = pos + (cp == '\t' ? 8 : utf16Units(cp));
                    piece.cp = cp;
                    piece.text = cp != '\n';
                    piece.tab = cp == '\t';
                    piece.charShapeId = run.charShapeId;
                    piece.markpen = item.markpen;
                    pieces.push_back(std::move(piece));
                    pos = pieces.back().end;
                }
            } else {
                StreamPiece piece;
                piece.begin = pos;
                piece.end = pos + 8;
                piece.charShapeId = run.charShapeId;
                if (item.control.placement && item.control.placement->treatAsChar) {
                    const auto& place = *item.control.placement;
                    piece.objectWidth = static_cast<std::int32_t>(place.width) + place.marginLeft + place.marginRight;
                }
                pieces.push_back(std::move(piece));
                pos = pieces.back().end;
            }
        }
    }
    return pieces;
}

struct FaceStyle {
    std::string font;
    std::int32_t ratio = 100;
    std::int32_t relSize = 100;
    std::int32_t spacing = 0;
    std::int32_t offset = 0;
    std::int32_t size = 1000;
};

FaceStyle faceFor(const Document& doc, const CharShape* shape, char32_t cp) {
    FaceStyle face;
    if (!shape) return face;
    face.size = shape->height > 0 ? shape->height : 1000;
    const bool hangul = cp >= 0xAC00 && cp <= 0xD7A3;
    const Lang lang = hangul ? Lang::Hangul : Lang::Latin;
    const auto slot = static_cast<std::size_t>(lang);
    face.ratio = slot < shape->ratio.size() && shape->ratio[slot] != 0 ? shape->ratio[slot] : 100;
    face.relSize = slot < shape->relSize.size() && shape->relSize[slot] != 0 ? shape->relSize[slot] : 100;
    face.spacing = slot < shape->spacing.size() ? shape->spacing[slot] : 0;
    face.offset = slot < shape->offset.size() ? shape->offset[slot] : 0;
    if (slot < doc.header.fonts.size() && slot < shape->fontRef.size()) {
        const auto& fonts = doc.header.fonts[slot];
        const std::uint32_t id = shape->fontRef[slot];
        if (id < fonts.size()) {
            const layout::FontLoadScope fontWhy("줄배치", cp, &fonts[id].face);
            face.font = layout::fontWithGlyph(fonts[id].face, cp, fonts[id].subst, fonts[id].base);
        }
    }
    return face;
}

void paintShape(Node& node, const Document& doc, const CharShape* shape, const std::string& markpen) {
    if (!shape) return;
    node.color = colorHex(shape->textColor);
    node.shade = colorHex(shape->shadeColor);
    node.bold = shape->bold;
    node.italic = shape->italic;
    if (!shape->underline.empty() && shape->underline != "NONE") node.underline = shape->underline;
    node.strikeout = shape->strikeout;
    node.markpen = markpen;
    if (!shape->borderFillId) return;
    const auto& fills = doc.header.borderFills;
    const std::uint32_t id = shape->borderFillId;
    if (id > 0 && id - 1 < fills.size() && fills[id - 1].id == id) {
        if (!fills[id - 1].faceColor.empty() && fills[id - 1].faceColor != "none") node.fillColor = fills[id - 1].faceColor;
        return;
    }
    if (id < fills.size() && fills[id].id == id) {
        if (!fills[id].faceColor.empty() && fills[id].faceColor != "none") node.fillColor = fills[id].faceColor;
        return;
    }
    for (const auto& fill : fills) {
        if (fill.id != id) continue;
        if (!fill.faceColor.empty() && fill.faceColor != "none") node.fillColor = fill.faceColor;
        break;
    }
}

Node lineFromSeg(const Document& doc, const Section& section, const Paragraph& para, const LineSeg& seg,
                 int segIndex, const std::vector<StreamPiece>& pieces) {
    const ParaShape* paraShape = findParaShape(doc, para.paraShapeId);
    Node line;
    line.kind = NodeKind::Line;
    line.x = seg.horzPos;
    line.width = seg.horzSize > 0 ? seg.horzSize : 0;
    line.height = seg.vertSize > 0 ? seg.vertSize : seg.textHeight;
    line.textHeight = seg.textHeight > 0 ? seg.textHeight : line.height;
    line.charShapeId = !para.runs.empty() ? para.runs.front().charShapeId : 0;
    const CharShape* initShape = findCharShape(doc, line.charShapeId);
    line.fontSize = initShape && initShape->height > 0 ? initShape->height : 1000;
    line.storedSeg = seg;
    line.align = paraShape && !paraShape->align.empty() ? paraShape->align : "JUSTIFY";
    line.vertAlign = paraShape && !paraShape->vertAlign.empty() ? paraShape->vertAlign : "BASELINE";
    line.lastLine = segIndex + 1 == static_cast<int>(para.lineSegs.size());
    line.start = static_cast<int>(seg.textPos);
    const int endPos = segIndex + 1 < static_cast<int>(para.lineSegs.size())
                           ? static_cast<int>(para.lineSegs[static_cast<std::size_t>(segIndex + 1)].textPos)
                           : std::numeric_limits<int>::max();
    line.end = endPos;

    bool setFirstPiece = false;
    const std::int32_t tabInterval = section.defaultTab > 0 ? section.defaultTab : 4000;
    std::int32_t pen = 0;
    Node span;
    bool spanOpen = false;
    auto flush = [&] {
        if (!spanOpen) return;
        if (!span.text.empty() || !span.advances.empty()) line.children.push_back(std::move(span));
        span = Node{};
        spanOpen = false;
    };
    for (const StreamPiece& piece : pieces) {
        if (piece.end <= line.start || piece.begin >= endPos) continue;
        const CharShape* shape = findCharShape(doc, piece.charShapeId);
        if (!setFirstPiece) {
            setFirstPiece = true;
            line.charShapeId = piece.charShapeId;
            if (shape && shape->height > 0) line.fontSize = shape->height;
        }
        if (piece.objectWidth > 0) {
            flush();
            Node gap;
            gap.kind = NodeKind::Span;
            gap.width = piece.objectWidth;
            gap.charShapeId = piece.charShapeId;
            gap.fontSize = shape && shape->height > 0 ? shape->height : 1000;
            line.children.push_back(std::move(gap));
            pen += piece.objectWidth;
            continue;
        }
        if (!piece.text) continue;
        if (piece.tab) {
            flush();
            Node tab;
            tab.kind = NodeKind::Span;
            tab.text = "\t";
            tab.width = std::max<std::int32_t>(tabInterval / 4, tabInterval - pen % tabInterval);
            tab.fontSize = shape && shape->height > 0 ? shape->height : 1000;
            tab.charShapeId = piece.charShapeId;
            paintShape(tab, doc, shape, piece.markpen);
            pen += tab.width;
            line.children.push_back(std::move(tab));
            continue;
        }
        const FaceStyle face = faceFor(doc, shape, piece.cp);
        const bool same = spanOpen && span.charShapeId == piece.charShapeId && span.font == face.font &&
                          span.ratio == face.ratio && span.relSize == face.relSize && span.spacing == face.spacing &&
                          span.charOffset == face.offset && span.markpen == piece.markpen;
        if (!same) {
            flush();
            span.kind = NodeKind::Span;
            span.charShapeId = piece.charShapeId;
            span.font = face.font;
            span.fontSize = face.size;
            span.ratio = face.ratio;
            span.relSize = face.relSize;
            span.spacing = face.spacing;
            span.charOffset = face.offset;
            paintShape(span, doc, shape, piece.markpen);
            spanOpen = true;
        }
        const Lang lang = (piece.cp >= 0xAC00 && piece.cp <= 0xD7A3) ? Lang::Hangul : Lang::Latin;
        CharShape fallback;
        fallback.height = 1000;
        fallback.ratio.fill(100);
        const CharShape* measure = shape ? shape : &fallback;
        const std::int32_t advance = layout::glyphAdvance(piece.cp, *measure, lang, &doc);
        const std::int32_t trail = layout::glyphTrail(piece.cp, *measure, lang, &doc);
        const std::int32_t width = advance > 0 ? advance : 0;
        span.advances.push_back(width);
        span.inks.push_back(std::max<std::int32_t>(0, width - trail));
        noting::appendUtf8(span.text, piece.cp);
        pen += width;
    }
    flush();
    return line;
}

Node paperFor(const Section& section) {
    const PageDef page = section.page.value_or(PageDef{});
    const std::int32_t paperW = static_cast<std::int32_t>(page.width);
    const std::int32_t paperH = static_cast<std::int32_t>(page.height);
    const std::int32_t contentLeft = static_cast<std::int32_t>(page.marginLeft);
    const std::int32_t contentTop = static_cast<std::int32_t>(page.marginTop) + static_cast<std::int32_t>(page.marginHeader);
    const std::int32_t contentW = std::max<std::int32_t>(200, paperW - contentLeft - static_cast<std::int32_t>(page.marginRight));
    const std::int32_t contentH = std::max<std::int32_t>(
        200, paperH - contentTop - static_cast<std::int32_t>(page.marginBottom) - static_cast<std::int32_t>(page.marginFooter));

    Node paper;
    paper.kind = NodeKind::Paper;
    paper.width = paperW;
    paper.height = paperH;
    paper.marginTop = kDefaultPageMarginTopHu;
    paper.marginLeft = kDefaultPageMarginLeftHu;
    paper.marginRight = kDefaultPageMarginRightHu;
    paper.marginBottom = kDefaultPageMarginBottomHu;
    Node body;
    body.kind = NodeKind::Body;
    body.x = contentLeft;
    body.y = contentTop;
    body.width = contentW;
    body.height = contentH;
    Node column;
    column.kind = NodeKind::Column;
    column.width = contentW;
    column.height = contentH;
    body.children.push_back(std::move(column));
    paper.children.push_back(std::move(body));
    return paper;
}

}  // namespace

std::vector<PlannedPage> planLineCache(const Document& doc) {
    const std::vector<std::vector<LineHit>> pages = pagesFromLineSegs(doc);
    std::vector<PlannedPage> boxes;
    boxes.reserve(pages.size());
    for (const auto& hits : pages) {
        const int sectionIndex = hits.empty() ? 0 : hits.front().section;
        PlannedPage box;
        if (sectionIndex >= 0 && sectionIndex < static_cast<int>(doc.sections.size())) {
            const PageDef page = doc.sections[static_cast<std::size_t>(sectionIndex)].page.value_or(PageDef{});
            box.width = static_cast<std::int32_t>(page.width);
            box.height = static_cast<std::int32_t>(page.height);
        }
        boxes.push_back(box);
    }
    if (boxes.empty()) boxes.push_back({});
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        boxes[i].marginTop = defaultPageMarginTopHu(i);
        boxes[i].marginBottom = defaultPageMarginBottomHu(i, boxes.size());
    }
    return boxes;
}

LineCachePage lineCachePage(const Document& doc, int pageIndex) {
    LineCachePage out;
    const std::vector<std::vector<LineHit>> pages = pagesFromLineSegs(doc);
    out.count = static_cast<int>(pages.size());
    if (pageIndex < 0 || pageIndex >= out.count) return out;
    const std::vector<LineHit>& hits = pages[static_cast<std::size_t>(pageIndex)];
    const int sectionIndex = hits.empty() ? 0 : hits.front().section;
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(doc.sections.size())) return out;
    const Section& section = doc.sections[static_cast<std::size_t>(sectionIndex)];
    const int sectionStory = ensureStoryMap(doc).storyOf(&section.paragraphs);
    out.page = paperFor(section);
    out.page.marginTop = defaultPageMarginTopHu(static_cast<std::size_t>(pageIndex));
    out.page.marginBottom =
        defaultPageMarginBottomHu(static_cast<std::size_t>(pageIndex), static_cast<std::size_t>(out.count));
    if (hits.empty() || out.page.children.empty() || out.page.children.front().children.empty()) return out;
    Node& body = out.page.children.front();
    body.children.clear();
    const std::int32_t contentW = bodyWidth(section);
    Node column;
    bool columnOpen = false;
    int openColumn = -1;
    std::int32_t openOrigin = 0;
    Node paragraph;
    bool open = false;
    int openParagraph = -1;
    std::vector<StreamPiece> openPieces;
    auto closeParagraph = [&] {
        if (!open) return;
        std::int32_t bottom = 0;
        for (const Node& child : paragraph.children) bottom = std::max(bottom, child.y + child.height);
        paragraph.height = bottom;
        paragraph.width = column.width;
        paragraph.story = sectionStory;
        column.children.push_back(std::move(paragraph));
        paragraph = Node{};
        open = false;
    };
    auto closeColumn = [&] {
        closeParagraph();
        if (!columnOpen) return;
        std::int32_t bottom = 0;
        for (const Node& child : column.children) bottom = std::max(bottom, child.y + child.height);
        column.height = std::max<std::int32_t>(bottom, 1);
        body.children.push_back(std::move(column));
        column = Node{};
        columnOpen = false;
    };
    for (const LineHit& hit : hits) {
        if (hit.section != sectionIndex) continue;
        const Paragraph& para = section.paragraphs[static_cast<std::size_t>(hit.paragraph)];
        if (hit.seg < 0 || hit.seg >= static_cast<int>(para.lineSegs.size())) continue;
        const LineSeg& seg = para.lineSegs[static_cast<std::size_t>(hit.seg)];
        if (!columnOpen || openColumn != hit.column || openOrigin != hit.origin) {
            closeColumn();
            ColumnLayout layout = columnLayoutAt(section.paragraphs, hit.paragraph);
            if (layout.count != std::max(1, hit.columnCount)) {
                layout = ColumnLayout{};
                layout.count = std::max(1, hit.columnCount);
                layout.gap = hit.columnGap;
            }
            const std::vector<ColumnBox> boxes = columnBoxes(layout, contentW, false);
            const int index = std::clamp(hit.column, 0, static_cast<int>(boxes.size()) - 1);
            const ColumnBox& box = boxes[static_cast<std::size_t>(index)];
            column.kind = NodeKind::Column;
            column.x = box.x;
            column.y = hit.origin;
            column.width = box.width;
            applyColumnRule(column, layout, index + 1 < static_cast<int>(boxes.size()));
            columnOpen = true;
            openColumn = hit.column;
            openOrigin = hit.origin;
        }
        if (!open || openParagraph != hit.paragraph) {
            closeParagraph();
            paragraph.kind = NodeKind::Paragraph;
            paragraph.paragraph = hit.paragraph;
            paragraph.story = sectionStory;
            paragraph.y = seg.vertPos;
            if (const ParaShape* shape = findParaShape(doc, para.paraShapeId)) {
                paragraph.align = shape->align;
            }
            openPieces = paragraphPieces(para);
            open = true;
            openParagraph = hit.paragraph;
        }
        Node line = lineFromSeg(doc, section, para, seg, hit.seg, openPieces);
        line.paragraph = hit.paragraph;
        line.story = sectionStory;
        line.y = seg.vertPos - paragraph.y;
        paragraph.children.push_back(std::move(line));
    }
    closeColumn();
    return out;
}

}  // namespace noting::render
