#include "noting/render/scene.hpp"
#include "noting/render/paint.hpp"

#include "render/column_geom.hpp"
#include "render/line_breaker.hpp"
#include "render/scene_detail.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "render/equation_layout.hpp"

namespace noting::render {

using namespace detail;

// 위 캡션이 개체를 내려 앉힌 양. 캡션 내용(마지막 줄 간격 뺌) + 간격이다.
// 간격이 음수여서 합이 음수면 그만큼 상자가 줄고, 개체는 줄 위로 올라간다.
std::int32_t inlineObjectCaptionDrop(const Node& child) {
    for (const Node& cap : child.children) {
        if (cap.role == "caption" && cap.fileHorzRel == "TOP") return -cap.y;
    }
    return 0;
}

// 글자처럼 취한 개체의 바깥 상자. 높이 + 위아래 여백이다.
// 위 캡션은 내려 앉힌 양을 더한다. 7601 개체에 내용 1842, 간격 -850 캡션이면 8593이다.
// 아래 캡션이 개체 바닥 밖으로 나가면 그만큼 높다. 캡션은 마지막 줄 간격을 빼고 잰다.
std::int32_t inlineObjectOuter(const Node& child) {
    std::int32_t height = child.height;
    for (const Node& cap : child.children) {
        if (cap.role != "caption" || cap.fileHorzRel != "BOTTOM") continue;
        height = std::max(height, cap.y + columnContentBottom(cap));
    }
    return inlineObjectCaptionDrop(child) + height + child.marginTop + child.marginBottom;
}

// 줄 안에서 개체 바깥 상자의 바닥. y 는 위 캡션만큼 내려 앉힌 개체 자리다.
std::int32_t inlineObjectBottom(const Node& child) {
    return child.y - inlineObjectCaptionDrop(child) + inlineObjectOuter(child);
}

// 그림·표·도형의 기준선. 바깥 상자의 85%다.
// 0.5는 0에서 멀어지게 올린다. 12815면 10893, 23812면 20240이다.
std::int32_t inlineObjectBaseline(const Node& child) {
    const std::int32_t outer = inlineObjectOuter(child);
    if (outer <= 0) return 0;
    const std::int64_t scaled = static_cast<std::int64_t>(outer) * 85;
    if (scaled >= 0) return static_cast<std::int32_t>((scaled + 50) / 100);
    return -static_cast<std::int32_t>((-scaled + 50) / 100);
}

// 글자 높이와, 글자처럼 취한 개체 높이 중 큰 값이다. 개체는 높이 + 위아래 여백이다.
std::int32_t measuredTextBox(const Node& line, std::int32_t charBox) {
    std::int32_t box = charBox;
    for (const Node& child : line.children) {
        if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
        box = std::max(box, inlineObjectOuter(child));
    }
    return box;
}

// 글자 기준선, 그림·표·도형은 위에서 85%, 수식은 수식 높이×그 백분율. 가장 큰 값이 줄 기준선이다.
// 수식 백분율도 0.5는 0에서 멀어지게 올린다. 1350의 71%는 959, 2745의 68%는 1867이다.
std::int32_t equationBaselinePoint(const Node& child) {
    const int percent = child.baseLine == 0 ? 85 : child.baseLine;
    const std::int32_t outer = inlineObjectOuter(child);
    if (outer <= 0 || percent <= 0) return 0;
    const std::int64_t scaled = static_cast<std::int64_t>(outer) * percent;
    if (scaled >= 0) return static_cast<std::int32_t>((scaled + 50) / 100);
    return -static_cast<std::int32_t>((-scaled + 50) / 100);
}

std::int32_t measuredLineBaseline(const Node& line, std::int32_t baseline) {
    for (const Node& child : line.children) {
        if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
        if (child.baseLine >= 0) baseline = std::max(baseline, equationBaselinePoint(child));
        else baseline = std::max(baseline, inlineObjectBaseline(child));
    }
    return baseline;
}

// lineseg 에 적는 기준선. 문단 세로 정렬이 위면 0, 가운데면 textheight 의 절반(나머지 버림), 아래면 textheight 다.
// 글자처럼 취한 개체가 있어도 같다. 가운데 1000은 500, 1133은 566이다.
std::int32_t lineSegBaseline(const Node& line, std::int32_t textHeight, std::int32_t glyphBaseline) {
    if (line.vertAlign == "TOP") return 0;
    if (line.vertAlign == "CENTER") return textHeight / 2;
    if (line.vertAlign == "BOTTOM") return textHeight;
    return measuredLineBaseline(line, glyphBaseline);
}

namespace detail {

bool gPageCalcLog = false;
bool gMeasuredLineHeight = false;
int gPageLogIndex = 0;
model::StoryMap gStoryMap;

void resetStories(const model::Document& doc) { gStoryMap.build(doc); }

int takeStory(const std::vector<model::Paragraph>* paragraphs = nullptr) {
    if (paragraphs) {
        const int found = gStoryMap.storyOf(paragraphs);
        if (found > 0) return found;
    }
    return gStoryMap.reserveSynthetic();
}

void stampStory(Node& paragraph, int story) {
    paragraph.story = story;
    for (Node& child : paragraph.children) {
        if (child.kind == NodeKind::Line) child.story = story;
    }
}

void rebindClonedStories(Node& node, std::unordered_map<int, int>& aliases) {
    if ((node.kind == NodeKind::Paragraph || node.kind == NodeKind::Line) && node.story > 0) {
        auto it = aliases.find(node.story);
        if (it == aliases.end()) {
            const int aliased = gStoryMap.aliasStory(node.story);
            it = aliases.emplace(node.story, aliased).first;
        }
        node.story = it->second;
    }
    for (Node& child : node.children) {
        rebindClonedStories(child, aliases);
    }
}

struct TempStoryBinding {
    std::vector<const std::vector<model::Paragraph>*> bound;

    TempStoryBinding(const std::vector<model::Paragraph>& source, const std::vector<model::Paragraph>& temp) {
        bindList(source, temp);
    }

    ~TempStoryBinding() {
        for (const auto* ptr : bound) gStoryMap.unbindTempPointer(ptr);
    }

    TempStoryBinding(const TempStoryBinding&) = delete;
    TempStoryBinding& operator=(const TempStoryBinding&) = delete;

    void bindControl(const model::Control& srcCtrl, const model::Control& tmpCtrl) {
        if (srcCtrl.table && tmpCtrl.table) {
            const std::size_t n = std::min(srcCtrl.table->cells.size(), tmpCtrl.table->cells.size());
            for (std::size_t i = 0; i < n; ++i) {
                bindList(srcCtrl.table->cells[i].content.paragraphs, tmpCtrl.table->cells[i].content.paragraphs);
            }
        }
        const std::size_t childN = std::min(srcCtrl.children.size(), tmpCtrl.children.size());
        for (std::size_t i = 0; i < childN; ++i) {
            bindControl(srcCtrl.children[i], tmpCtrl.children[i]);
        }
        const std::size_t subN = std::min(srcCtrl.subLists.size(), tmpCtrl.subLists.size());
        for (std::size_t i = 0; i < subN; ++i) {
            bindList(srcCtrl.subLists[i].paragraphs, tmpCtrl.subLists[i].paragraphs);
        }
        if (srcCtrl.caption && tmpCtrl.caption) {
            bindList(srcCtrl.caption->content.paragraphs, tmpCtrl.caption->content.paragraphs);
        }
    }

    void bindList(const std::vector<model::Paragraph>& src, const std::vector<model::Paragraph>& tmp) {
        const int base = gStoryMap.storyOf(&src);
        const int alias = base > 0 ? gStoryMap.aliasStory(base) : gStoryMap.reserveSynthetic();
        gStoryMap.bindTempPointer(&tmp, alias);
        bound.push_back(&tmp);
        const std::size_t paraN = std::min(src.size(), tmp.size());
        for (std::size_t pi = 0; pi < paraN; ++pi) {
            const std::size_t runN = std::min(src[pi].runs.size(), tmp[pi].runs.size());
            for (std::size_t ri = 0; ri < runN; ++ri) {
                const std::size_t itemN = std::min(src[pi].runs[ri].items.size(), tmp[pi].runs[ri].items.size());
                for (std::size_t ii = 0; ii < itemN; ++ii) {
                    const auto& sItem = src[pi].runs[ri].items[ii];
                    const auto& tItem = tmp[pi].runs[ri].items[ii];
                    if (sItem.type == model::InlineItem::Type::Control &&
                        tItem.type == model::InlineItem::Type::Control) {
                        bindControl(sItem.control, tItem.control);
                    }
                }
            }
        }
    }
};

double pageCalcMs(std::chrono::steady_clock::time_point started) {
    const auto elapsed = std::chrono::steady_clock::now() - started;
    return std::chrono::duration<double, std::milli>(elapsed).count();
}


// 줄 캐시 쪽 로그. 맨위 포함은 자식까지 넣은 시간이고, 종류별 자신은 그 개체만의 시간이다.
struct RoleAcc {
    int n = 0;
    double ms = 0;
};

struct PageObjAcc {
    RoleAcc container;
    RoleAcc table;
    RoleAcc shape;
    RoleAcc picture;
    RoleAcc equation;
    double reflowMs = 0;
    int reflowParas = 0;
    double glyphMs = 0;
    int glyphLines = 0;
    struct Top {
        const char* role = "";
        double ms = 0;
        int kids = 0;
    };
    std::vector<Top> tops;
};

PageObjAcc gPageObj;
bool gPageObjOn = false;
double gObjNestedMs = 0;

void resetPageObj() {
    gPageObj = {};
    gObjNestedMs = 0;
}

const char* objectRoleName(ControlKind kind) {
    switch (kind) {
    case ControlKind::Container: return "묶음";
    case ControlKind::Table: return "표";
    case ControlKind::Picture: return "그림";
    case ControlKind::Equation: return "수식";
    default: return "도형";
    }
}

RoleAcc* roleAcc(ControlKind kind) {
    switch (kind) {
    case ControlKind::Container: return &gPageObj.container;
    case ControlKind::Table: return &gPageObj.table;
    case ControlKind::Picture: return &gPageObj.picture;
    case ControlKind::Equation: return &gPageObj.equation;
    default: return &gPageObj.shape;
    }
}

int objectKids(const Control& control) {
    int kids = static_cast<int>(control.children.size());
    if (control.table) kids += static_cast<int>(control.table->cells.size());
    for (const SubList& list : control.subLists) kids += static_cast<int>(list.paragraphs.size());
    return kids;
}

void logLineCachePage(int page, int from, int to, double ms) {
    std::string out = "{\"쪽\":" + std::to_string(page);
    if (to > from) {
        out += ",\"문단\":[" + std::to_string(from) + "," + std::to_string(to) + "]";
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf), ",\"ms\":%.1f", ms);
    out += buf;
    if (gPageObj.reflowParas > 0) {
        std::snprintf(buf, sizeof(buf), ",\"다시조판\":%.1f,\"다시조판문단\":%d", gPageObj.reflowMs,
                      gPageObj.reflowParas);
        out += buf;
    }
    if (gPageObj.glyphMs >= 0.05) {
        std::snprintf(buf, sizeof(buf), ",\"글자폭\":%.1f,\"글자폭줄\":%d", gPageObj.glyphMs, gPageObj.glyphLines);
        out += buf;
    }
    std::vector<PageObjAcc::Top> ordered = gPageObj.tops;
    std::sort(ordered.begin(), ordered.end(),
              [](const PageObjAcc::Top& a, const PageObjAcc::Top& b) { return a.ms > b.ms; });
    std::string tops;
    int shown = 0;
    for (const PageObjAcc::Top& top : ordered) {
        if (top.ms < 1.0) continue;
        if (shown >= 8) break;
        if (shown) tops += ',';
        std::snprintf(buf, sizeof(buf), "{\"종류\":\"%s\",\"포함\":%.1f,\"자식\":%d}", top.role, top.ms, top.kids);
        tops += buf;
        ++shown;
    }
    if (shown > 0) {
        out += ",\"맨위\":[";
        out += tops;
        out += ']';
    }
    auto addRole = [&](const char* key, const RoleAcc& acc) {
        if (acc.n <= 0) return;
        std::snprintf(buf, sizeof(buf), ",\"%s\":{\"개수\":%d,\"자신\":%.1f}", key, acc.n, acc.ms);
        out += buf;
    };
    addRole("묶음", gPageObj.container);
    addRole("표", gPageObj.table);
    addRole("도형", gPageObj.shape);
    addRole("그림", gPageObj.picture);
    addRole("수식", gPageObj.equation);
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

struct Obstacle {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool block = false;
    int paragraph = -1;
};

struct Frame {
    const Document* doc = nullptr;
    const std::vector<Paragraph>* paragraphs = nullptr;
    PageDef pageDef;
    bool page = false;
    int depth = 0;
    HeadingState* headings = nullptr;
    std::int32_t defaultTab = 4000;
    std::int32_t columnRegionGap = 1134;  // 단 설정 나누기. 글자 높이 아래에 두는 간격
    std::string forceAlign;  // 비어 있지 않으면 문단 정렬 대신 쓴다
    bool useStoredLineHeight = false;  // 문단 lineseg 의 줄 경계와 줄 높이로 세로를 쌓는다
    bool deferSpans = false;           // 본문 줄은 위치만 남기고 글 칸은 나중에 채운다
    int sectionIndex = -1;
    int story = -1;                    // 이 문단 목록의 블록 범위. 섹션과 개체는 각각 하나다
    std::int32_t bottomReserve = 0;    // 쪽 아래에 각주·미주를 둘 높이
    int pageParagraph = 0;             // 이 쪽이 시작한 문단. 쪽마다 번호를 다시 셀 때 쓴다
};

constexpr int kMaxDepth = 8;
// 지금까지의 합 + 이번 글자가 줄 너비보다 크면 그 글자 앞에서 끊는다. 같으면 그 줄에 남긴다.
// 문단 줄 너비의 바닥. 0.2인치. 칸이나 글상자가 더 좁아도 horzsize 는 이 값이다.
constexpr std::int32_t kMinParagraphWidth = 1440;

// 여백을 뺀 폭이 0.2인치보다 좁으면 한글은 왼쪽 여백을 버리고 단 왼쪽부터 0.2인치를 쓴다.
// 폭 571 칸에 좌우 여백 100이면 왼쪽 0, 폭 1440이다.
ParagraphSpan paragraphSpan(std::int32_t columnWidth, const ParaShape* shape) {
    const std::int32_t left = shape ? shape->marginLeft : 0;
    const std::int32_t right = shape ? shape->marginRight : 0;
    if (columnWidth - left - right < kMinParagraphWidth) return {0, kMinParagraphWidth};
    return {left, columnWidth - left - right};
}

// 한 줄은 세그먼트 하나다. 들여쓰기는 첫 줄에, 내어쓰기는 그다음 줄에 적용된 것으로 표시한다.
std::uint32_t measuredLineFlags(bool firstLine, std::int32_t indent) {
    std::uint32_t flags = (1u << 17) | (1u << 18);
    if ((indent > 0 && firstLine) || (indent < 0 && !firstLine)) flags |= 1u << 20;
    return flags;
}

std::int32_t aligned(const std::string& align, std::int32_t origin, std::int32_t size,
                     std::int32_t obj, std::int32_t offset) {
    if (align == "CENTER") return origin + (size - obj) / 2 + offset;
    if (align == "RIGHT" || align == "BOTTOM" || align == "OUTSIDE") return origin + size - obj - offset;
    return origin + offset;
}

// 위치 값은 바깥 여백의 바깥쪽까지다. 그림 테두리는 그 안쪽으로 여백만큼 들어간다.
std::int32_t insetX(const Placement& placement, std::int32_t x) {
    if (placement.horzAlign == "RIGHT" || placement.horzAlign == "OUTSIDE") return x - placement.marginRight;
    if (placement.horzAlign == "CENTER") return x + (placement.marginLeft - placement.marginRight) / 2;
    return x + placement.marginLeft;
}

std::int32_t insetY(const Placement& placement, std::int32_t y) {
    if (placement.vertAlign == "BOTTOM" || placement.vertAlign == "OUTSIDE") return y - placement.marginBottom;
    if (placement.vertAlign == "CENTER") return y + (placement.marginTop - placement.marginBottom) / 2;
    return y + placement.marginTop;
}

const CharShape* findCharShape(const Document& doc, std::uint32_t id) {
    const auto& shapes = doc.header.charShapes;
    if (id < shapes.size() && shapes[id].id == id) return &shapes[id];
    for (const auto& shape : shapes) {
        if (shape.id == id) return &shape;
    }
    return nullptr;
}

const noting::model::BorderFill* findBorderFill(const Document& doc, std::uint32_t id) {
    if (id == 0) return nullptr;
    const auto& fills = doc.header.borderFills;
    if (id - 1 < fills.size() && fills[id - 1].id == id) return &fills[id - 1];
    if (id < fills.size() && fills[id].id == id) return &fills[id];
    for (const auto& fill : fills) {
        if (fill.id == id) return &fill;
    }
    return nullptr;
}

GlyphFace glyphFaceIn(const Document& doc, const CharShape& shape, Lang lang, char32_t cp, bool probeGlyph,
                      bool copyName) {
    const std::size_t slot = static_cast<std::size_t>(lang);
    GlyphFace face;
    face.ratio = slot < shape.ratio.size() && shape.ratio[slot] != 0 ? shape.ratio[slot] : 100;
    face.relSize = slot < shape.relSize.size() && shape.relSize[slot] != 0 ? shape.relSize[slot] : 100;
    face.spacing = slot < shape.spacing.size() ? shape.spacing[slot] : 0;
    if (!copyName) return face;
    if (slot < doc.header.fonts.size() && slot < shape.fontRef.size()) {
        const auto& fonts = doc.header.fonts[slot];
        const std::uint32_t id = shape.fontRef[slot];
        if (id < fonts.size()) {
            // 줄 캐시로 줄을 가를 때는 글꼴 파일을 열지 않는다. 그 쪽을 그릴 때 연다.
            if (probeGlyph) {
                const layout::FontLoadScope fontWhy("다시조판", cp, &fonts[id].face);
                face.font = layout::fontWithGlyph(fonts[id].face, cp, fonts[id].subst, fonts[id].base);
            } else {
                face.font = fonts[id].face;
            }
        }
    }
    return face;
}

GlyphFace glyphFace(const Document& doc, const CharShape& shape, char32_t cp, bool probeGlyph, bool copyName) {
    return glyphFaceIn(doc, shape, isHangul(cp) ? Lang::Hangul : Lang::Latin, cp, probeGlyph, copyName);
}

std::int32_t glyphWidth(const Document& doc, char32_t cp, const CharShape& face) {
    const Lang lang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
    const std::int32_t width = layout::glyphAdvance(cp, face, lang, &doc);
    const std::int32_t em = face.height > 0 ? face.height : kEm;
    return width > 0 ? width : (cp == ' ' || cp < 128 ? em / 2 : em);
}

bool nextCodePoint(std::string_view text, std::size_t& index, char32_t& out) {
    if (index >= text.size()) return false;
    const auto c = static_cast<unsigned char>(text[index]);
    if (c < 0x80) {
        out = c;
        index += 1;
        return true;
    }
    if ((c & 0xE0) == 0xC0 && index + 1 < text.size()) {
        out = ((c & 0x1F) << 6) | (text[index + 1] & 0x3F);
        index += 2;
        return true;
    }
    if ((c & 0xF0) == 0xE0 && index + 2 < text.size()) {
        out = ((c & 0x0F) << 12) | ((text[index + 1] & 0x3F) << 6) | (text[index + 2] & 0x3F);
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

// 어울림과 자리 차지만 글 자리를 비운다. 글 앞·글 뒤는 겹친다.
bool textAvoids(const std::string& wrap) {
    return wrap == "SQUARE" || wrap == "TOP_AND_BOTTOM";
}

bool visualObject(const Control& control) {
    return control.placement &&
           (control.kind == ControlKind::Shape || control.kind == ControlKind::Picture ||
            control.kind == ControlKind::Table || control.kind == ControlKind::Container ||
            control.kind == ControlKind::Equation);
}

// 머리말·꼬리말·각주·미주·쪽 번호는 여백에 따로 그리지만, 본문 문단의 그 자리도 빈 줄이 아니다.
bool occupiesBodyLine(const Control& control) {
    return control.kind == ControlKind::Header || control.kind == ControlKind::Footer ||
           control.kind == ControlKind::Footnote || control.kind == ControlKind::Endnote ||
           control.kind == ControlKind::PageNumPos;
}

struct ColumnResume;
Node layoutFrame(const Frame& frame, int startParagraph, std::size_t startAtom, ColumnResume* resume);
Node layoutFrame(const Frame& frame) { return layoutFrame(frame, 0, 0, nullptr); }

void copyEdge(BorderEdge& edge, const noting::model::BorderSide& side) {
    if (side.type.empty() || side.type == "NONE") return;
    edge.type = side.type;
    edge.color = side.color.empty() ? "#000000" : side.color;
    edge.width = side.width;
}

std::string colorHex(const noting::model::Color& color) {
    if (!color) return {};
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(*color & 0xFFFFFFu));
    return buf;
}

}  // namespace detail

void detail::paintCharStyle(Node& node, const Document& doc, std::uint32_t charShapeId) {
    const CharShape* shape = findCharShape(doc, charShapeId);
    if (!shape) return;
    node.color = colorHex(shape->textColor);
    node.shade = colorHex(shape->shadeColor);
    // 글자 배경은 음영과 따로다. 글자 모양의 테두리/배경 면색을 글자 상자에 칠한다.
    bool borderStroke = false;
    if (const auto* fill = findBorderFill(doc, shape->borderFillId)) {
        if (!fill->faceColor.empty() && fill->faceColor != "none") node.fillColor = fill->faceColor;
        auto side = [](const noting::model::BorderSide& edge) {
            return !edge.type.empty() && edge.type != "NONE";
        };
        borderStroke = side(fill->left) || side(fill->right) || side(fill->top) || side(fill->bottom);
    }
    node.bold = shape->bold;
    node.italic = shape->italic;
    if (!shape->underline.empty() && shape->underline != "NONE") node.underline = shape->underline;
    node.strikeout = shape->strikeout;
    node.superscript = shape->superscript;
    node.subscript = shape->subscript;
    node.bitmapDeco = !node.underline.empty() || node.strikeout || shape->emphasis != 0 || borderStroke;
}

namespace detail {

void applyBorderFill(Node& node, const Document& doc, std::uint32_t id) {
    const auto* fill = findBorderFill(doc, id);
    if (!fill) return;
    copyEdge(node.borderLeft, fill->left);
    copyEdge(node.borderRight, fill->right);
    copyEdge(node.borderTop, fill->top);
    copyEdge(node.borderBottom, fill->bottom);
    node.slash = fill->slash;
    node.backSlash = fill->backSlash;
    copyEdge(node.borderDiagonal, fill->diagonal);
    node.centerLine = fill->centerLine;
    node.breakCellSeparateLine = fill->breakCellSeparateLine;
    if (!fill->faceColor.empty() && fill->faceColor != "none") node.fillColor = fill->faceColor;
    if (!fill->gradColor.empty() && fill->gradColor != "none") {
        node.gradColor = fill->gradColor;
        node.gradAngle = fill->gradAngle;
    }
    if (!fill->imageId.empty() && fill->imageMode >= 0 && fill->imageMode != 15) {
        node.binItemId = fill->imageId;
        node.imageFillMode = fill->imageMode;
    }
}

void applyParagraphBorder(Node& paragraph, const Document& doc, const ParaShape* paraShape) {
    if (!paraShape || !paraShape->borderFillId) return;
    applyBorderFill(paragraph, doc, paraShape->borderFillId);
    paragraph.borderOffsetLeft = paraShape->borderOffsetLeft;
    paragraph.borderOffsetRight = paraShape->borderOffsetRight;
    paragraph.borderOffsetTop = paraShape->borderOffsetTop;
    paragraph.borderOffsetBottom = paraShape->borderOffsetBottom;
    paragraph.borderConnect = paraShape->borderConnect;
    paragraph.borderIgnoreMargin = paraShape->borderIgnoreMargin;
}

void applyStroke(Node& node, const Control& control) {
    if (control.strokeType.empty() || control.strokeType == "NONE") {
        if (!control.fillColor.empty() && control.fillColor != "none") node.fillColor = control.fillColor;
        return;
    }
    BorderEdge edge;
    edge.type = control.strokeType;
    edge.color = control.strokeColor.empty() ? "#000000" : control.strokeColor;
    edge.width = control.strokeWidth;
    edge.end = control.strokeEnd == 0 ? 0 : 1;
    node.borderLeft = node.borderRight = node.borderTop = node.borderBottom = edge;
    if (!control.fillColor.empty() && control.fillColor != "none") node.fillColor = control.fillColor;
}

// 칸 내용의 아래. 마지막 줄은 글자(와 글자처럼 취급하는 개체) 높이만 넣고, 줄 간격 추가분과
// 마지막 문단의 아래 간격은 빼 둔다. 한글 표 높이도 그 간격을 넣지 않는다.
// 캡션은 keepPitch 로 그 추가분까지 상자에 넣는다. 빼 두면 위 캡션 개체가 그만큼 위에 남는다.
std::int32_t rowContentBottom(const Node& node, bool keepPitch) {
    std::int32_t objects = 0;
    std::int32_t lastY = -1;
    std::int32_t lastHeight = 0;
    std::int32_t lastContent = 0;
    std::int32_t lastParaBottom = 0;
    const auto visit = [&](auto&& self, const Node& current, std::int32_t y, std::int32_t paraBottom) -> void {
        if (current.kind == NodeKind::Object) {
            objects = std::max(objects, y + current.height + current.marginBottom);
            return;
        }
        if (current.kind == NodeKind::Line) {
            std::int32_t content = current.textHeight > 0 ? current.textHeight : current.fontSize;
            for (const auto& child : current.children) {
                if (child.kind != NodeKind::Object) continue;
                content = std::max(content, inlineObjectBottom(child));
            }
            lastY = y;
            lastHeight = current.height;
            lastContent = content;
            lastParaBottom = paraBottom;
            return;
        }
        const std::int32_t nextPara = current.kind == NodeKind::Paragraph ? y + current.height : paraBottom;
        for (const auto& child : current.children) self(self, child, y + child.y, nextPara);
    };
    visit(visit, node, 0, 0);
    std::int32_t text = 0;
    if (lastY >= 0) {
        const std::int32_t full = lastY + lastHeight;
        const std::int32_t trailing = std::max<std::int32_t>(0, lastParaBottom - full);
        const std::int32_t body = keepPitch ? lastHeight : std::max<std::int32_t>(0, lastContent);
        text = lastY + body + (keepPitch ? trailing : 0);
    }
    return std::max(objects, text);
}

// 글상자 세로 맞춤에 쓰는 내용 높이. 줄 높이는 다음 줄이 시작할 자리만 정하고,
// 마지막 줄은 글자 높이만 넣는다. 문단 높이(줄간격 포함)를 쓰면 글이 그만큼 위로 밀린다.
std::int32_t columnContentBottom(const Node& body, bool keepPitch) {
    std::int32_t bottom = 0;
    bool sawColumn = false;
    for (const auto& child : body.children) {
        if (child.kind != NodeKind::Column) continue;
        sawColumn = true;
        bottom = std::max(bottom, child.y + rowContentBottom(child, keepPitch));
    }
    if (!sawColumn) return rowContentBottom(body, keepPitch);
    return bottom;
}

Node makeObject(const Control& control, std::int32_t x, std::int32_t y, const Document& doc, int depth,
                std::int32_t defaultTab, bool useStoredLineHeight);

// 위·아래 캡션 너비는 개체 너비다. 여백 포함이면 좌우 바깥 여백을 더하고 그만큼 왼쪽으로 당긴다.
// 왼쪽·오른쪽은 캡션에 적힌 너비다. 간격은 맞닿는 쪽에만 둔다. 음수면 그만큼 겹친다.
void attachCaption(Node& object, const Control& control, const Document& doc, int depth, std::int32_t defaultTab,
                   bool useStoredLineHeight) {
    if (!control.caption || control.caption->content.paragraphs.empty() || depth >= kMaxDepth) return;
    const Caption& cap = *control.caption;
    const bool horizontal = cap.side == "TOP" || cap.side == "BOTTOM";
    std::int32_t width = 0;
    std::int32_t originX = 0;
    if (horizontal) {
        width = object.width;
        if (cap.fullSize) {
            width += object.marginLeft + object.marginRight;
            originX = -object.marginLeft;
        }
        // 저장된 캡션 폭이 있으면 그 폭이다. 배율이 든 그림은 지금 크기 9575에 9576처럼 1 클 수 있다.
        if (cap.lastWidth > 0) width = static_cast<std::int32_t>(cap.lastWidth);
    } else {
        width = static_cast<std::int32_t>(cap.width);
        if (width <= 0) width = static_cast<std::int32_t>(cap.lastWidth);
        if (width <= 0) width = object.width;
    }
    if (width < 200) width = 200;
    Frame inner;
    inner.doc = &doc;
    inner.paragraphs = &cap.content.paragraphs;
    inner.pageDef.width = static_cast<std::uint32_t>(width);
    inner.pageDef.height = 2000000;
    inner.depth = depth + 1;
    inner.defaultTab = defaultTab;
    inner.useStoredLineHeight = useStoredLineHeight;
    inner.story = takeStory(&cap.content.paragraphs);
    Node laid = layoutFrame(inner);
    const std::int32_t used = std::max<std::int32_t>(1, columnContentBottom(laid, true));
    const std::int32_t gap = static_cast<std::int32_t>(cap.gap);
    std::int32_t x = originX;
    std::int32_t y = 0;
    if (cap.side == "TOP") {
        // 개체를 내리는 양은 마지막 줄 간격을 뺀 내용 높이로 잰다.
        y = -(columnContentBottom(laid) + gap);
    } else if (cap.side == "LEFT") {
        x = -(width + gap);
        if (cap.content.vertAlign == "CENTER") y = (object.height - used) / 2;
        else if (cap.content.vertAlign == "BOTTOM") y = object.height - used;
    } else if (cap.side == "RIGHT") {
        x = object.width + gap;
        if (cap.content.vertAlign == "CENTER") y = (object.height - used) / 2;
        else if (cap.content.vertAlign == "BOTTOM") y = object.height - used;
    } else {
        y = object.height + gap;
    }
    for (Node& column : laid.children) {
        if (column.kind == NodeKind::Column) column.height = used;
    }
    laid.role = "caption";
    laid.x = x;
    laid.y = y;
    laid.width = width;
    laid.height = used;
    laid.fileAudit = true;
    laid.fileHorzRel = cap.side;
    laid.fileX = static_cast<std::int32_t>(cap.gap);
    laid.fileWidth = width;
    laid.fileHeight = used;
    object.children.push_back(std::move(laid));
    // 위 캡션은 적힌 위치의 기준이 캡션 윗면이다. 개체는 캡션 높이만큼 아래로 내려간다.
    // 왼쪽 캡션은 캡션 왼쪽 끝이 그 자리다. 개체는 캡션 너비만큼 오른쪽으로 간다.
    if (control.placement && control.placement->groupInRoot) return;
    if (cap.side == "TOP") object.y -= y;
    else if (cap.side == "LEFT") object.x -= x;
}

void fillInside(Node& object, const Control& control, const Document& doc, int depth,
                std::int32_t defaultTab, bool useStoredLineHeight) {
    if (depth >= kMaxDepth) return;
    if (control.table) {
        const Table& table = *control.table;
        int rowCount = static_cast<int>(table.rowCount);
        int colCount = static_cast<int>(table.colCount);
        for (const auto& cell : table.cells) {
            const int rowSpan = std::max(1, static_cast<int>(cell.rowSpan));
            const int colSpan = std::max(1, static_cast<int>(cell.colSpan));
            rowCount = std::max(rowCount, static_cast<int>(cell.row) + rowSpan);
            colCount = std::max(colCount, static_cast<int>(cell.col) + colSpan);
        }
        std::vector<std::int32_t> colW(static_cast<std::size_t>(std::max(colCount, 0)), 0);
        std::vector<std::int32_t> rowH(static_cast<std::size_t>(std::max(rowCount, 0)), 0);
        struct SpanAssign {
            std::size_t start = 0;
            int span = 1;
            std::int32_t width = 0;
        };
        std::vector<SpanAssign> spans;
        for (const auto& cell : table.cells) {
            const int colSpan = std::max(1, static_cast<int>(cell.colSpan));
            const int rowSpan = std::max(1, static_cast<int>(cell.rowSpan));
            if (cell.col < colW.size() && cell.width > 0) {
                spans.push_back(SpanAssign{cell.col, colSpan, static_cast<std::int32_t>(cell.width)});
            }
            if (rowSpan == 1 && cell.row < rowH.size()) {
                rowH[cell.row] = std::max(rowH[cell.row], static_cast<std::int32_t>(cell.height));
            }
        }
        // 좁은 병합을 먼저 칸에 넣는다. 넓은 병합이 먼저 칸을 나누면 나중에 더 좁은 칸으로 줄일 수 없다.
        std::sort(spans.begin(), spans.end(), [](const SpanAssign& a, const SpanAssign& b) {
            if (a.span != b.span) return a.span < b.span;
            return a.width < b.width;
        });
        std::vector<std::int32_t> prefix(colW.size() + 1, -1);
        prefix[0] = 0;
        const auto validPrefix = [&](std::size_t idx, std::int32_t val) {
            if (val < 0) return false;
            for (std::size_t k = 0; k < prefix.size(); ++k) {
                if (prefix[k] < 0) continue;
                if (k < idx && prefix[k] >= val) return false;
                if (k > idx && prefix[k] <= val) return false;
            }
            return true;
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (const auto& span : spans) {
                if (span.start >= colW.size()) continue;
                int count = span.span;
                if (span.start + static_cast<std::size_t>(count) > colW.size()) {
                    count = static_cast<int>(colW.size() - span.start);
                }
                if (count <= 0) continue;
                const std::size_t a = span.start;
                const std::size_t b = span.start + static_cast<std::size_t>(count);
                if (prefix[a] >= 0 && prefix[b] < 0) {
                    const std::int32_t cand = prefix[a] + span.width;
                    if (validPrefix(b, cand)) {
                        prefix[b] = cand;
                        changed = true;
                    }
                } else if (prefix[b] >= 0 && prefix[a] < 0) {
                    const std::int32_t cand = prefix[b] - span.width;
                    if (validPrefix(a, cand)) {
                        prefix[a] = cand;
                        changed = true;
                    }
                }
            }
        }
        std::size_t prevKnown = 0;
        for (std::size_t i = 1; i < prefix.size(); ++i) {
            if (prefix[i] < 0) continue;
            if (i - prevKnown == 1 && prefix[i] > prefix[prevKnown]) {
                colW[prevKnown] = prefix[i] - prefix[prevKnown];
            } else if (i - prevKnown > 1 && prefix[i] > prefix[prevKnown]) {
                spans.push_back(SpanAssign{
                    prevKnown,
                    static_cast<int>(i - prevKnown),
                    prefix[i] - prefix[prevKnown]});
            }
            prevKnown = i;
        }
        std::sort(spans.begin(), spans.end(), [](const SpanAssign& a, const SpanAssign& b) {
            if (a.span != b.span) return a.span < b.span;
            return a.width < b.width;
        });
        for (const auto& span : spans) {
            if (span.start >= colW.size()) continue;
            int count = span.span;
            if (span.start + static_cast<std::size_t>(count) > colW.size()) {
                count = static_cast<int>(colW.size() - span.start);
            }
            if (count <= 0) continue;
            std::int32_t known = 0;
            int unknown = 0;
            for (int i = 0; i < count; ++i) {
                if (colW[span.start + static_cast<std::size_t>(i)] > 0) {
                    known += colW[span.start + static_cast<std::size_t>(i)];
                } else {
                    ++unknown;
                }
            }
            if (unknown == 0) {
                if (count == 1 && span.width > known) colW[span.start] = span.width;
                continue;
            }
            if (span.width <= known) continue;
            const std::int32_t each = (span.width - known) / unknown;
            std::int32_t remain = span.width - known - each * unknown;
            for (int i = 0; i < count; ++i) {
                if (colW[span.start + static_cast<std::size_t>(i)] > 0) continue;
                const std::int32_t add = each + (remain > 0 ? 1 : 0);
                if (add <= 0) continue;
                colW[span.start + static_cast<std::size_t>(i)] = add;
                if (remain > 0) --remain;
            }
        }
        auto cover = [](std::vector<std::int32_t>& slots, std::size_t start, int span, std::int32_t total) {
            if (span <= 0 || start >= slots.size() || start + static_cast<std::size_t>(span) > slots.size()) return;
            std::int32_t known = 0;
            int unknown = 0;
            for (int i = 0; i < span; ++i) {
                if (slots[start + static_cast<std::size_t>(i)] > 0) known += slots[start + static_cast<std::size_t>(i)];
                else ++unknown;
            }
            if (total <= known) return;
            if (unknown == 0) {
                slots[start + static_cast<std::size_t>(span - 1)] += total - known;
                return;
            }
            const std::int32_t each = (total - known) / unknown;
            std::int32_t remain = total - known - each * unknown;
            for (int i = 0; i < span; ++i) {
                if (slots[start + static_cast<std::size_t>(i)] > 0) continue;
                slots[start + static_cast<std::size_t>(i)] = each + (remain > 0 ? 1 : 0);
                if (remain > 0) --remain;
            }
        };
        const std::int32_t spacing = static_cast<std::int32_t>(table.cellSpacing);
        struct Draft {
            const TableCell* cell = nullptr;
            Node content;
            std::int32_t padL = 0;
            std::int32_t padR = 0;
            std::int32_t padT = 0;
            std::int32_t padB = 0;
            std::int32_t width = 0;
            std::int32_t used = 0;
        };
        std::vector<Draft> drafts;
        for (const auto& cell : table.cells) {
            const int colSpan = std::max(1, static_cast<int>(cell.colSpan));
            if (cell.col >= colW.size() || cell.row >= rowH.size()) continue;
            Draft draft;
            draft.cell = &cell;
            for (int i = 0; i < colSpan; ++i) draft.width += colW[cell.col + static_cast<std::size_t>(i)];
            if (colSpan > 1) draft.width += spacing * (colSpan - 1);
            draft.padL = cell.hasMargin ? cell.marginLeft : table.marginLeft;
            draft.padR = cell.hasMargin ? cell.marginRight : table.marginRight;
            draft.padT = cell.hasMargin ? cell.marginTop : table.marginTop;
            draft.padB = cell.hasMargin ? cell.marginBottom : table.marginBottom;
            Frame inner;
            inner.doc = &doc;
            inner.paragraphs = &cell.content.paragraphs;
            inner.pageDef.width = static_cast<std::uint32_t>(std::max<std::int32_t>(200, draft.width - draft.padL - draft.padR));
            inner.pageDef.height = 20000000;
            inner.depth = depth + 1;
            inner.defaultTab = defaultTab;
            inner.useStoredLineHeight = useStoredLineHeight;
            inner.story = takeStory(&cell.content.paragraphs);
            draft.content = layoutFrame(inner);
            // 적힌 높이는 최소다. 안의 글과 그림이 더 크면 그 높이까지 칸을 늘린다.
            draft.used = rowContentBottom(draft.content);
            drafts.push_back(std::move(draft));
        }
        std::vector<const Draft*> spanDrafts;
        for (const auto& draft : drafts) {
            const int rowSpan = std::max(1, static_cast<int>(draft.cell->rowSpan));
            if (rowSpan != 1) {
                spanDrafts.push_back(&draft);
                continue;
            }
            cover(rowH, draft.cell->row, 1, draft.padT + draft.padB + draft.used);
        }
        std::sort(spanDrafts.begin(), spanDrafts.end(), [](const Draft* a, const Draft* b) {
            return a->cell->rowSpan < b->cell->rowSpan;
        });
        for (const Draft* draft : spanDrafts) {
            const int rowSpan = std::max(1, static_cast<int>(draft->cell->rowSpan));
            const std::int32_t fileH = static_cast<std::int32_t>(draft->cell->height);
            const std::int32_t contentH = draft->padT + draft->padB + draft->used;
            cover(rowH, draft->cell->row, rowSpan, std::max(fileH, contentH));
        }
        std::int32_t tableW = 0;
        std::int32_t tableH = 0;
        for (const std::int32_t width : colW) tableW += width;
        for (const std::int32_t height : rowH) tableH += height;
        // 셀 간격은 칸 사이와, 칸과 표 테두리 사이에 같은 너비로 들어간다.
        if (colCount > 0) tableW += spacing * (colCount + 1);
        if (rowCount > 0) tableH += spacing * (rowCount + 1);
        if (tableW > object.width) object.width = tableW;
        if (tableH > object.height) object.height = tableH;
        for (auto& draft : drafts) {
            const TableCell& cell = *draft.cell;
            const int colSpan = std::max(1, static_cast<int>(cell.colSpan));
            const int rowSpan = std::max(1, static_cast<int>(cell.rowSpan));
            std::int32_t x = spacing;
            std::int32_t y = spacing;
            std::int32_t height = 0;
            for (std::size_t i = 0; i < cell.col; ++i) x += colW[i] + spacing;
            for (std::size_t i = 0; i < cell.row; ++i) y += rowH[i] + spacing;
            for (int i = 0; i < rowSpan; ++i) height += rowH[cell.row + static_cast<std::size_t>(i)];
            if (rowSpan > 1) height += spacing * (rowSpan - 1);
            const std::int32_t innerH = std::max<std::int32_t>(0, height - draft.padT - draft.padB);
            std::int32_t contentY = draft.padT;
            if (cell.vertAlign == "CENTER" && draft.used < innerH) contentY = draft.padT + (innerH - draft.used) / 2;
            else if (cell.vertAlign == "BOTTOM" && draft.used < innerH) contentY = draft.padT + (innerH - draft.used);
            draft.content.x = draft.padL;
            draft.content.y = contentY;
            Node cellNode;
            cellNode.kind = NodeKind::Cell;
            cellNode.x = x;
            cellNode.y = y;
            cellNode.width = draft.width;
            cellNode.height = height;
            cellNode.row = static_cast<int>(cell.row);
            cellNode.col = static_cast<int>(cell.col);
            cellNode.rowSpan = rowSpan;
            cellNode.colSpan = colSpan;
            if (rowSpan > 1) {
                std::int32_t accH = 0;
                for (int i = 0; i + 1 < rowSpan; ++i) {
                    accH += rowH[cell.row + static_cast<std::size_t>(i)] + spacing;
                    if (accH > 0 && accH < height) cellNode.spanRowSplits.push_back(accH);
                }
            }
            if (colSpan > 1 && cell.col + static_cast<std::size_t>(colSpan) <= colW.size()) {
                std::int32_t accW = 0;
                for (int i = 0; i + 1 < colSpan; ++i) {
                    accW += colW[cell.col + static_cast<std::size_t>(i)] + spacing;
                    if (accW > 0 && accW < draft.width) cellNode.spanColSplits.push_back(accW);
                }
            }
            cellNode.vertAlign = cell.vertAlign.empty() ? "TOP" : cell.vertAlign;
            cellNode.marginLeft = draft.padL;
            cellNode.marginRight = draft.padR;
            cellNode.marginTop = draft.padT;
            cellNode.marginBottom = draft.padB;
            cellNode.fileAudit = true;
            cellNode.fileWidth = static_cast<std::int32_t>(cell.width);
            cellNode.fileHeight = static_cast<std::int32_t>(cell.height);
            cellNode.filePadLeft = cell.marginLeft;
            cellNode.filePadRight = cell.marginRight;
            cellNode.filePadTop = cell.marginTop;
            cellNode.filePadBottom = cell.marginBottom;
            cellNode.fileOwnMargin = cell.hasMargin ? 1 : 0;
            applyBorderFill(cellNode, doc, cell.borderFillId);
            cellNode.children.push_back(std::move(draft.content));
            object.children.push_back(std::move(cellNode));
        }
        return;
    }
    // 자식 좌표는 묶음 왼쪽 위가 원점이다. HWP는 가장 바깥 묶음 기준이라, 그 묶음의 위치를 뺀다.
    const std::int32_t originX = control.placement && control.placement->groupInRoot ? control.placement->groupX : 0;
    const std::int32_t originY = control.placement && control.placement->groupInRoot ? control.placement->groupY : 0;
    for (const auto& child : control.children) {
        if (!child.placement) continue;
        const Placement& place = *child.placement;
        object.children.push_back(makeObject(child, place.groupX - originX, place.groupY - originY, doc, depth + 1,
                                             defaultTab, useStoredLineHeight));
    }
    if (control.subLists.empty()) return;
    // 줄 폭은 저장된 글상자 폭에서 잡는다. 배율이 있으면 지금 크기보다 1 클 수 있다. 37109 상자에 37110이다.
    const std::int32_t textWidth =
        control.boxTextWidth > 0 ? static_cast<std::int32_t>(control.boxTextWidth) : object.width;
    const std::int32_t innerW =
        std::max<std::int32_t>(200, textWidth - control.boxPadLeft - control.boxPadRight);
    const std::int32_t innerH =
        std::max<std::int32_t>(200, object.height - control.boxPadTop - control.boxPadBottom);
    // 적힌 높이는 최소다. 표 칸과 같이, 안쪽 줄 높이를 더한 값이 더 크면 글상자도 그만큼 늘어난다.
    std::int32_t grown = object.height;
    for (const auto& list : control.subLists) {
        Frame inner;
        inner.doc = &doc;
        inner.paragraphs = &list.paragraphs;
        inner.pageDef.width = static_cast<std::uint32_t>(innerW);
        inner.pageDef.height = static_cast<std::uint32_t>(innerH);
        inner.depth = depth + 1;
        inner.defaultTab = defaultTab;
        inner.useStoredLineHeight = useStoredLineHeight;
        inner.story = takeStory(&list.paragraphs);
        Node content = layoutFrame(inner);
        const std::int32_t used = columnContentBottom(content);
        const std::int32_t need = control.boxPadTop + control.boxPadBottom + used;
        if (need > grown) grown = need;
        std::int32_t contentY = control.boxPadTop;
        if (list.vertAlign == "CENTER" && used < innerH) {
            contentY = control.boxPadTop + (innerH - used) / 2;
        } else if (list.vertAlign == "BOTTOM" && used < innerH) {
            contentY = control.boxPadTop + (innerH - used);
        }
        content.x = control.boxPadLeft;
        content.y = contentY;
        content.vertAlign = list.vertAlign.empty() ? "TOP" : list.vertAlign;
        object.children.push_back(std::move(content));
    }
    if (grown > object.height) object.height = grown;
}

Node makeObject(const Control& control, std::int32_t x, std::int32_t y, const Document& doc, int depth,
                std::int32_t defaultTab, bool useStoredLineHeight) {
    struct ObjTimer {
        const Control& control;
        int depth;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        double nestedBefore = gObjNestedMs;
        ~ObjTimer() {
            if (!gPageObjOn) return;
            const double inclusive = pageCalcMs(started);
            const double nested = gObjNestedMs - nestedBefore;
            const double exclusive = inclusive > nested ? inclusive - nested : 0;
            gObjNestedMs += exclusive;
            RoleAcc* acc = roleAcc(control.kind);
            acc->n += 1;
            acc->ms += exclusive;
            if (depth == 0 && inclusive >= 0.05) {
                gPageObj.tops.push_back(PageObjAcc::Top{objectRoleName(control.kind), inclusive, objectKids(control)});
            }
        }
    } timer{control, depth};
    (void)timer;
    Node node;
    node.kind = NodeKind::Object;
    node.x = x;
    node.y = y;
    node.width = static_cast<std::int32_t>(control.placement->width);
    node.height = static_cast<std::int32_t>(control.placement->height);
    node.fileAudit = true;
    node.fileWidth = node.width;
    node.fileHeight = node.height;
    node.fileX = control.placement->horzOffset;
    node.fileY = control.placement->vertOffset;
    node.fileGroupX = control.placement->groupX;
    node.fileGroupY = control.placement->groupY;
    node.fileMatrixX = control.placement->matrixX;
    node.fileMatrixY = control.placement->matrixY;
    node.fileScaleX = control.placement->scaleX;
    node.fileScaleY = control.placement->scaleY;
    node.filePadLeft = control.boxPadLeft;
    node.filePadRight = control.boxPadRight;
    node.filePadTop = control.boxPadTop;
    node.filePadBottom = control.boxPadBottom;
    node.fileHorzRel = control.placement->horzRelTo;
    node.fileVertRel = control.placement->vertRelTo;
    node.fileHorzAlign = control.placement->horzAlign;
    node.shapeType = control.shapeType;
    if (control.table) {
        node.fileRows = control.table->rowCount;
        node.fileCols = control.table->colCount;
    }
    node.treatAsChar = control.placement->treatAsChar;
    node.vertAlign = control.placement->vertAlign;
    node.baseLine = control.baseLine;
    node.flipHorizontal = control.placement->flipHorizontal;
    node.flipVertical = control.placement->flipVertical;
    node.marginLeft = control.placement->marginLeft;
    node.marginRight = control.placement->marginRight;
    node.marginTop = control.placement->marginTop;
    node.marginBottom = control.placement->marginBottom;
    node.binItemId = control.binItemId;
    node.picture = control.picture;
    node.zOrder = control.placement->zOrder;
    node.textWrap = control.placement->textWrap;
    node.points = control.points;
    node.cornerRatio = control.cornerRatio;
    if (control.kind == ControlKind::Picture) node.role = "picture";
    else if (control.kind == ControlKind::Table) node.role = "table";
    else if (control.kind == ControlKind::Container) node.role = "container";
    else if (control.kind == ControlKind::Equation) {
        node.role = "equation";
        node.script = control.script;
        node.fileEqBase = control.fileBaseLine >= 0 ? control.fileBaseLine : control.baseLine;
        // 줄에 앉히는 상자는 파일 너비·높이다. 측정값은 요소 비교에만 남긴다.
        const EquationSize measured = control.equationFont.empty()
                                          ? measureHangulEquation(control.script, control.baseUnit)
                                          : measureHangulEquation(control.script, control.baseUnit, control.equationFont);
        node.eqWidth = measured.width;
        node.eqHeight = measured.height;
        node.eqBase = measured.baseLine;
        attachCaption(node, control, doc, depth, defaultTab, useStoredLineHeight);
        return node;
    } else node.role = "shape";
    if (control.kind == ControlKind::Table && control.table) {
        // 표 테두리는 칸 테두리와 따로다. 한글은 셀 간격을 켜야 이 바깥 프레임을 그린다.
        node.cellSpacing = static_cast<std::int32_t>(control.table->cellSpacing);
        if (control.table->cellSpacing > 0) applyBorderFill(node, doc, control.table->borderFillId);
        if (const auto* fill = findBorderFill(doc, control.table->borderFillId)) {
            node.breakCellSeparateLine = fill->breakCellSeparateLine;
        }
    } else {
        applyStroke(node, control);
    }
    fillInside(node, control, doc, depth, defaultTab, useStoredLineHeight);
    attachCaption(node, control, doc, depth, defaultTab, useStoredLineHeight);
    return node;
}

std::vector<std::pair<std::int32_t, std::int32_t>> freeGaps(std::int32_t y, std::int32_t pitch,
                                                           std::int32_t left, std::int32_t right,
                                                           const std::vector<Obstacle>& obstacles) {
    std::vector<std::pair<std::int32_t, std::int32_t>> free{{left, right}};
    for (const auto& obstacle : obstacles) {
        if (y + pitch <= obstacle.y || y >= obstacle.y + obstacle.height) continue;
        if (obstacle.block) return {};
        std::vector<std::pair<std::int32_t, std::int32_t>> next;
        for (const auto& gap : free) {
            const std::int32_t objL = obstacle.x;
            const std::int32_t objR = obstacle.x + obstacle.width;
            if (objR <= gap.first || objL >= gap.second) {
                next.push_back(gap);
                continue;
            }
            if (objL > gap.first) next.push_back({gap.first, objL});
            if (objR < gap.second) next.push_back({objR, gap.second});
        }
        free = std::move(next);
    }
    free.erase(std::remove_if(free.begin(), free.end(),
                              [](const auto& gap) { return gap.second - gap.first < 200; }),
               free.end());
    return free;
}

struct TableCarry {
    const Control* control = nullptr;
    Node table;
    std::int32_t ox = 0;
    std::int32_t oy = 0;
};

struct ColumnResume {
    int paragraph = 0;
    std::size_t atom = 0;
    int textPos = 0;  // atom 이 가리키는 글자 위치. 0 이면 문단 처음
    bool done = true;
    bool breakPage = false;  // 쪽 나눔. 다음 단으로 넘기지 않는다
    std::vector<TableCarry> carryTables;
};

const ParaShape* findParaShape(const Document& doc, std::uint32_t id);

bool paragraphStartsPage(const Paragraph& para, const Document& doc) {
    if (para.pageBreak) return true;
    if (const ParaShape* shape = findParaShape(doc, para.paraShapeId)) return shape->pageBreakBefore;
    return false;
}

int paragraphEm(const Document& doc, const Paragraph& para) {
    int em = 0;
    for (const auto& run : para.runs) {
        if (const CharShape* shape = findCharShape(doc, run.charShapeId)) {
            if (shape->height > em) em = shape->height;
        }
    }
    return em > 0 ? em : static_cast<int>(kEm);
}

// 글자 높이. 글꼴 크기이고, 글자처럼 취급한 개체는 바깥 여백까지 포함한 상자다.
// 그림과 글자 인라인의 lineseg textheight가 그 값이다. 줄간격 추가분은 넣지 않는다.
std::int32_t lineCharBox(const Node& line) {
    std::int32_t bottom =
        line.textHeight > 0 ? line.textHeight : (line.fontSize > 0 ? line.fontSize : kEm);
    for (const auto& child : line.children) {
        if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
        const std::int32_t outerBottom = inlineObjectBottom(child);
        if (outerBottom > bottom) bottom = outerBottom;
    }
    return bottom;
}

std::pair<Node, Node> splitCellBody(const Node& body, std::int32_t localCutY) {
    Node top = body;
    Node bottom = body;
    top.children.clear();
    bottom.children.clear();
    std::int32_t topMaxY = 0;
    std::int32_t botMaxY = 0;

    for (const Node& col : body.children) {
        if (col.kind != NodeKind::Column) {
            top.children.push_back(col);
            continue;
        }
        Node topCol = col;
        Node botCol = col;
        topCol.children.clear();
        botCol.children.clear();
        std::int32_t topColY = 0;
        std::int32_t botColY = 0;

        for (const Node& para : col.children) {
            if (para.kind != NodeKind::Paragraph) {
                topCol.children.push_back(para);
                continue;
            }
            Node topPara = para;
            Node botPara = para;
            topPara.children.clear();
            botPara.children.clear();
            std::int32_t topParaH = 0;
            std::int32_t botParaY = 0;
            bool anyTop = false;
            bool anyBot = false;

            for (const Node& child : para.children) {
                if (child.kind != NodeKind::Line) {
                    const std::int32_t objBottom = col.y + para.y + child.y + child.height;
                    if (objBottom <= localCutY || !anyTop) {
                        topPara.children.push_back(child);
                        topParaH = std::max(topParaH, child.y + child.height);
                    } else {
                        Node moved = child;
                        moved.y = std::max<std::int32_t>(0, moved.y - localCutY);
                        botPara.children.push_back(std::move(moved));
                        botParaY = std::max(botParaY, botPara.children.back().y + botPara.children.back().height);
                    }
                    continue;
                }
                const std::int32_t lineBottom = col.y + para.y + child.y + lineCharBox(child);
                if (!anyBot && lineBottom <= localCutY) {
                    topPara.children.push_back(child);
                    topParaH = std::max(topParaH, child.y + child.height);
                    anyTop = true;
                } else {
                    Node moved = child;
                    moved.y = botParaY;
                    botParaY += moved.height;
                    botPara.children.push_back(std::move(moved));
                    anyBot = true;
                }
            }
            if (anyTop || (!anyBot && !topPara.children.empty())) {
                topPara.height = topParaH;
                topCol.children.push_back(std::move(topPara));
                topColY = std::max(topColY, topCol.children.back().y + topCol.children.back().height);
            }
            if (anyBot) {
                botPara.y = botColY;
                botPara.height = botParaY;
                botCol.children.push_back(std::move(botPara));
                botColY += botParaY;
            }
        }
        topCol.height = topColY;
        botCol.height = botColY;
        top.children.push_back(std::move(topCol));
        bottom.children.push_back(std::move(botCol));
        topMaxY = std::max(topMaxY, col.y + topColY);
        botMaxY = std::max(botMaxY, col.y + botColY);
    }
    top.height = topMaxY;
    bottom.height = botMaxY;
    return {std::move(top), std::move(bottom)};
}

int countBodyLines(const Node& cell) {
    int count = 0;
    for (const Node& body : cell.children) {
        if (body.kind != NodeKind::Body) continue;
        for (const Node& col : body.children) {
            for (const Node& para : col.children) {
                for (const Node& line : para.children) {
                    if (line.kind == NodeKind::Line) ++count;
                }
            }
        }
    }
    return count;
}

int computeHeaderRowCount(const Table& tableModel) {
    if (!tableModel.repeatHeader || tableModel.cells.empty()) return 0;
    int maxRow = static_cast<int>(tableModel.rowCount);
    for (const TableCell& mc : tableModel.cells) {
        maxRow = std::max(maxRow, static_cast<int>(mc.row) + std::max(1, static_cast<int>(mc.rowSpan)));
    }
    if (maxRow <= 0) return 0;

    std::vector<bool> rowHasHeader(static_cast<std::size_t>(maxRow), false);
    std::vector<int> endGroupRow(static_cast<std::size_t>(maxRow), 0);
    for (int r = 0; r < maxRow; ++r) endGroupRow[static_cast<std::size_t>(r)] = r + 1;

    for (const TableCell& mc : tableModel.cells) {
        const int r = static_cast<int>(mc.row);
        if (r < 0 || r >= maxRow) continue;
        if (mc.header) rowHasHeader[static_cast<std::size_t>(r)] = true;
        const int spanEnd = std::min(maxRow, r + std::max(1, static_cast<int>(mc.rowSpan)));
        endGroupRow[static_cast<std::size_t>(r)] = std::max(endGroupRow[static_cast<std::size_t>(r)], spanEnd);
    }

    // 제목 줄 수. 한글처럼
    // 0행부터 시작하여 rowSpan으로 묶인 행 그룹 단위로 순회하며,
    // 그룹 시작 행에 제목 셀(cell.header)이 하나라도 있을 때만 groupEnd까지 제목 줄로 인정한다.
    // 제목 셀이 하나도 없으면 headerRowCount는 0이며 반복하지 않는다.
    int headerRowCount = 0;
    int groupStart = 0;
    while (groupStart < maxRow) {
        int groupEnd = endGroupRow[static_cast<std::size_t>(groupStart)];
        for (int r = groupStart + 1; r < groupEnd && r < maxRow; ++r) {
            groupEnd = std::max(groupEnd, endGroupRow[static_cast<std::size_t>(r)]);
        }
        if (!rowHasHeader[static_cast<std::size_t>(groupStart)]) break;
        headerRowCount = groupEnd;
        groupStart = groupEnd;
    }
    return headerRowCount;
}

bool splitTableNode(const Node& tableNode, const Table& tableModel, std::int32_t availH, Node& outTop,
                    Node& outBottom) {
    if (availH <= 0 || tableNode.height <= availH) return false;
    const std::string mode = tableModel.pageBreak.empty() ? "CELL" : tableModel.pageBreak;
    if (mode == "NONE") return false;

    std::vector<const Node*> cells;
    std::vector<const Node*> extras;
    for (const Node& child : tableNode.children) {
        if (child.kind == NodeKind::Cell) cells.push_back(&child);
        else extras.push_back(&child);
    }
    if (cells.empty()) return false;

    // 1. 행별 시작/끝 Y 수집
    std::vector<std::int32_t> yBreaks;
    for (const Node* c : cells) {
        yBreaks.push_back(c->y);
        yBreaks.push_back(c->y + c->height);
    }
    std::sort(yBreaks.begin(), yBreaks.end());
    yBreaks.erase(std::unique(yBreaks.begin(), yBreaks.end()), yBreaks.end());

    // availH 이하의 가장 큰 행 경계 탐색
    std::int32_t rowCutY = 0;
    for (std::int32_t yb : yBreaks) {
        if (yb <= availH) rowCutY = yb;
        else break;
    }

    // rowCutY에서 병합되지 않은 일반 행 경계인지 확인
    auto spansAcross = [&](std::int32_t cutY) {
        for (const Node* c : cells) {
            if (c->y < cutY && c->y + c->height > cutY) return true;
        }
        return false;
    };

    std::int32_t cleanRowCutY = 0;
    for (std::int32_t yb : yBreaks) {
        if (yb <= availH && yb > 0 && yb < tableNode.height && !spansAcross(yb)) cleanRowCutY = yb;
    }

    // TABLE 모드에서는 병합된 셀을 가로지르지 않는 행 경계(cleanRowCutY)를 사용하고,
    // CELL 모드에서는 걸치는 행 내부 텍스트(>=2줄)를 쪼갤 수 있으면 availH에서 자르되,
    // 그렇지 않으면 직전 행 경계(rowCutY)에서 자른다.
    std::int32_t splitY = cleanRowCutY;
    bool cellModeSplit = false;
    if (mode == "CELL") {
        std::int32_t candidateY = availH;
        bool canSplitAtAvail = true;
        bool anyMultiLineSpanning = false;
        bool anySpanningAtAvail = false;
        for (const Node* c : cells) {
            if (c->y < candidateY && c->y + c->height > candidateY) {
                anySpanningAtAvail = true;
                const std::int32_t relCut = candidateY - c->y - c->marginTop - c->marginBottom;
                const int totalLines = countBodyLines(*c);
                if (totalLines > 0) {
                    const Node* bodyPtr = nullptr;
                    for (const Node& ch : c->children) {
                        if (ch.kind == NodeKind::Body) {
                            bodyPtr = &ch;
                            break;
                        }
                    }
                    if (bodyPtr) {
                        auto [tb, bb] = splitCellBody(*bodyPtr, relCut);
                        Node probeTop, probeBot;
                        probeTop.children.push_back(std::move(tb));
                        probeBot.children.push_back(std::move(bb));
                        const int topL = countBodyLines(probeTop);
                        const int botL = countBodyLines(probeBot);
                        if (topL > 0 && botL > 0 && c->y >= rowCutY) {
                            anyMultiLineSpanning = true;
                        } else if (topL == 0 && c->y >= rowCutY) {
                            canSplitAtAvail = false;
                            break;
                        }
                    }
                } else if (relCut < 0 && c->y >= rowCutY) {
                    canSplitAtAvail = false;
                    break;
                }
            }
        }
        if (anySpanningAtAvail && canSplitAtAvail && (anyMultiLineSpanning || rowCutY == 0) &&
            candidateY > rowCutY) {
            splitY = candidateY;
            cellModeSplit = true;
        } else if (rowCutY > 0 && rowCutY < tableNode.height) {
            splitY = rowCutY;
            cellModeSplit = spansAcross(rowCutY);
        } else if (cleanRowCutY > 0) {
            splitY = cleanRowCutY;
            cellModeSplit = false;
        }
    }

    if (splitY <= 0 || splitY >= tableNode.height) return false;

    // 헤더 행 수집
    const int headerRowCount = computeHeaderRowCount(tableModel);
    std::vector<Node> headerCells;
    std::int32_t headerHeight = 0;
    if (headerRowCount > 0) {
        std::int32_t headerBottomY = 0;
        for (const Node* c : cells) {
            if (c->row >= 0 && c->row < headerRowCount) {
                headerBottomY = std::max(headerBottomY, c->y + c->height);
            }
        }
        // 제목 줄 영역 안에서 표가 잘리면 제목 줄만 남게 되므로 분할하지 않고 통째로 다음 단/쪽으로 넘긴다.
        if (headerBottomY > 0 && splitY <= headerBottomY) return false;
        for (const Node* c : cells) {
            if (c->row >= 0 && c->row < headerRowCount && c->y + c->height <= splitY) {
                headerCells.push_back(*c);
                headerHeight = std::max(headerHeight, c->y + c->height);
            }
        }
    }

    outTop = tableNode;
    outBottom = tableNode;
    outTop.children.clear();
    outBottom.children.clear();

    const std::int32_t spacing = tableNode.cellSpacing;
    std::int32_t topTableH = splitY;
    std::int32_t botExtraShift = 0;

    // 먼저 각 셀이 위/아래/양쪽 어디에 속하는지 분류하고, 셀 내부 텍스트가 늘어나면 아래쪽 행 높이를 보정한다.
    struct SplitPiece {
        Node cell;
        std::int32_t origY = 0;
    };
    std::vector<Node> topCells;
    std::vector<SplitPiece> botPieces;

    for (const Node* c : cells) {
        if (c->y + c->height <= splitY) {
            topCells.push_back(*c);
            continue;
        }
        if (c->y >= splitY) {
            SplitPiece sp;
            sp.cell = *c;
            sp.origY = c->y;
            botPieces.push_back(std::move(sp));
            continue;
        }
        // splitY 가 셀의 중간(c->y < splitY < c->y + c->height)을 가르는 경우
        Node topPart = *c;
        Node botPart = *c;
        topPart.height = splitY - c->y;
        botPart.y = splitY;
        botPart.height = (c->y + c->height) - splitY;

        // spanRowSplits 분할
        topPart.spanRowSplits.clear();
        botPart.spanRowSplits.clear();
        for (std::int32_t sr : c->spanRowSplits) {
            if (c->y + sr < splitY) topPart.spanRowSplits.push_back(sr);
            else if (c->y + sr > splitY) botPart.spanRowSplits.push_back((c->y + sr) - splitY);
        }

        // 셀 내부 Body 줄 분할
        if (!c->children.empty() && c->children[0].kind == NodeKind::Body) {
            const Node& body = c->children[0];
            const std::int32_t localCut = (splitY - c->y) - c->marginTop - c->marginBottom;
            auto [tb, bb] = splitCellBody(body, localCut);
            const std::int32_t neededBotH =
                c->marginTop + bb.height + c->marginBottom;
            if (neededBotH > botPart.height) {
                botExtraShift = std::max(botExtraShift, neededBotH - botPart.height);
                botPart.height = neededBotH;
            }
            auto alignBody = [&](Node& b, std::int32_t cellH) {
                const std::int32_t innerH = std::max<std::int32_t>(0, cellH - c->marginTop - c->marginBottom);
                const std::int32_t used = rowContentBottom(b);
                std::int32_t cy = c->marginTop;
                if (c->vertAlign == "CENTER" && used < innerH) cy = c->marginTop + (innerH - used) / 2;
                else if (c->vertAlign == "BOTTOM" && used < innerH) cy = c->marginTop + (innerH - used);
                b.y = cy;
            };
            alignBody(tb, topPart.height);
            alignBody(bb, botPart.height);
            topPart.children[0] = std::move(tb);
            botPart.children[0] = std::move(bb);
        }

        if (!tableNode.breakCellSeparateLine && !c->breakCellSeparateLine && c->rowSpan <= 1) {
            topPart.borderBottom = BorderEdge{};
            botPart.borderTop = BorderEdge{};
        }
        topCells.push_back(std::move(topPart));
        SplitPiece sp;
        sp.cell = std::move(botPart);
        sp.origY = splitY;
        botPieces.push_back(std::move(sp));
    }

    if (topCells.empty() || botPieces.empty()) return false;

    for (Node& tc : topCells) {
        topTableH = std::max(topTableH, tc.y + tc.height + spacing);
        outTop.children.push_back(std::move(tc));
    }
    outTop.height = topTableH;
    outTop.splitTable = true;

    // headerHeight 가 splitY 이상이면(헤더 자체가 쪼개진 경우) 헤더 반복 생략
    if (headerHeight >= splitY) {
        headerCells.clear();
        headerHeight = 0;
    }

    std::int32_t botTableH = 0;
    std::unordered_map<int, int> headerStoryAliases;
    for (const Node& hc : headerCells) {
        Node cloned = hc;
        rebindClonedStories(cloned, headerStoryAliases);
        botTableH = std::max(botTableH, cloned.y + cloned.height);
        outBottom.children.push_back(std::move(cloned));
    }

    // splitY 에서 잘린 셀이 아래쪽에서 필요한 높이만큼 늘어났다면(botExtraShift > 0),
    // splitY 에서 끝나는 원래 셀의 하단보다 아래에 있는 셀들도 함께 밀어준다.
    std::int32_t spanEnd = splitY;
    for (const Node* c : cells) {
        if (c->y < splitY && c->y + c->height > splitY) {
            spanEnd = std::max(spanEnd, c->y + c->height);
        }
    }

    for (auto& sp : botPieces) {
        Node& bc = sp.cell;
        std::int32_t newY = (bc.y - splitY) + headerHeight + spacing;
        if (botExtraShift > 0) {
            if (sp.origY >= spanEnd) {
                newY += botExtraShift;
            } else if (sp.origY == splitY && sp.origY + bc.height < spanEnd + botExtraShift) {
                bc.height = (spanEnd - splitY) + botExtraShift;
            }
        }
        bc.y = newY;
        botTableH = std::max(botTableH, bc.y + bc.height + spacing);
        outBottom.children.push_back(std::move(bc));
    }
    outBottom.height = botTableH;
    outBottom.splitTable = true;

    // 캡션 처리: 위 캡션은 outTop에, 아래 캡션은 outBottom에 배치
    for (const Node* ex : extras) {
        if (ex->role == "caption") {
            if (ex->y < 0) {
                outTop.children.push_back(*ex);
            } else {
                Node cap = *ex;
                cap.y = outBottom.height + cap.fileX;
                outBottom.children.push_back(std::move(cap));
            }
        } else {
            outTop.children.push_back(*ex);
        }
    }
    (void)cellModeSplit;
    return true;
}

// 마지막 줄은 글자 높이만 센다. 줄간격과 문단 아래 여백은 단 위치와 쪽 넘김에 넣지 않는다.
std::int32_t columnCharBottom(const Node& column) {
    std::int32_t bottom = 0;
    for (const auto& para : column.children) {
        if (para.kind != NodeKind::Paragraph) continue;
        bool hasLine = false;
        for (const auto& child : para.children) {
            if (child.kind != NodeKind::Line) continue;
            hasLine = true;
            bottom = std::max(bottom, para.y + child.y + lineCharBox(child));
        }
        if (!hasLine) {
            bottom = std::max(bottom, para.y + para.height);
        }
    }
    return bottom;
}

const ParaShape* findParaShape(const Document& doc, std::uint32_t id) {
    const auto& shapes = doc.header.paraShapes;
    if (id < shapes.size() && shapes[id].id == id) return &shapes[id];
    for (const auto& shape : shapes) {
        if (shape.id == id) return &shape;
    }
    return nullptr;
}

const Numbering* findNumbering(const Document& doc, std::uint32_t id) {
    for (const auto& numbering : doc.header.numberings) {
        if (numbering.id == id) return &numbering;
    }
    return nullptr;
}

const ParaHead* findHead(const Numbering& numbering, int level) {
    for (const auto& head : numbering.levels) {
        if (head.level == level) return &head;
    }
    return numbering.levels.empty() ? nullptr : &numbering.levels.front();
}

// 개요 번호 0번은 문서에 개요 정의가 없으면 한컴 기본 7수준을 쓴다.
const Numbering& builtinOutline() {
    static const Numbering numbering = [] {
        Numbering out;
        out.id = 0;
        struct Item {
            int level;
            const char* format;
            const char* numFormat;
        };
        const Item items[] = {
            {1, "^1.", "DIGIT"},
            {2, "^2.", "HANGUL_SYLLABLE"},
            {3, "^3)", "DIGIT"},
            {4, "^4)", "HANGUL_SYLLABLE"},
            {5, "(^5)", "DIGIT"},
            {6, "(^6)", "HANGUL_SYLLABLE"},
            {7, "^7", "CIRCLED_DIGIT"},
        };
        for (const Item& item : items) {
            ParaHead head;
            head.level = item.level;
            head.start = 1;
            head.format = item.format;
            head.numFormat = item.numFormat;
            out.levels.push_back(head);
        }
        for (int level = 8; level <= 10; ++level) {
            ParaHead head;
            head.level = level;
            head.useInstWidth = false;
            head.autoIndent = false;
            head.textOffset = 0;
            out.levels.push_back(head);
        }
        return out;
    }();
    return numbering;
}

const Numbering* numberingFor(const Document& doc, const std::string& type, std::uint32_t id) {
    if (const Numbering* found = findNumbering(doc, id)) return found;
    if (type == "OUTLINE" && id == 0) {
        if (const Numbering* found = findNumbering(doc, 1)) return found;
        return &builtinOutline();
    }
    return nullptr;
}

void copyHeadPlace(Node& paragraph, const ParaHead& head) {
    paragraph.head.autoIndent = head.autoIndent;
    paragraph.head.useInstWidth = head.useInstWidth;
    paragraph.head.widthAdjust = head.widthAdjust;
    paragraph.head.textOffset = head.textOffset;
    paragraph.head.offsetType = head.textOffsetType.empty() ? "PERCENT" : head.textOffsetType;
    paragraph.head.align = head.align.empty() ? "LEFT" : head.align;
    paragraph.head.charShapeId = head.charShapeId;
}

std::string romanNumeral(int value, bool lower) {
    static const int amounts[] = {1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char* words[] = {"M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
    std::string out;
    for (int i = 0; i < 13 && value > 0; ++i) {
        while (value >= amounts[i]) {
            out += words[i];
            value -= amounts[i];
        }
    }
    if (lower) {
        for (char& ch : out) ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

std::string formatNumber(int value, const std::string& numFormat) {
    if (value < 1) value = 1;
    if (numFormat == "HANGUL_SYLLABLE") {
        static const char* syllables[] = {"가", "나", "다", "라", "마", "바", "사", "아", "자", "차", "카", "타", "파", "하"};
        if (value <= 14) return syllables[value - 1];
    } else if (numFormat == "HANGUL_JAMO") {
        static const char* jamo[] = {"ㄱ", "ㄴ", "ㄷ", "ㄹ", "ㅁ", "ㅂ", "ㅅ", "ㅇ", "ㅈ", "ㅊ", "ㅋ", "ㅌ", "ㅍ", "ㅎ"};
        if (value <= 14) return jamo[value - 1];
    } else if (numFormat == "CIRCLED_DIGIT" && value <= 20) {
        std::string out;
        appendUtf8(out, static_cast<char32_t>(0x245F + value));
        return out;
    } else if (numFormat == "ROMAN_CAPITAL" || numFormat == "ROMAN_SMALL") {
        return romanNumeral(value, numFormat == "ROMAN_SMALL");
    } else if ((numFormat == "LATIN_CAPITAL" || numFormat == "LATIN_SMALL") && value <= 26) {
        const char base = numFormat == "LATIN_CAPITAL" ? 'A' : 'a';
        return std::string(1, static_cast<char>(base + value - 1));
    } else if (numFormat == "CIRCLED_LATIN_CAPITAL" && value <= 26) {
        std::string out;
        appendUtf8(out, static_cast<char32_t>(0x24B5 + value));
        return out;
    } else if (numFormat == "CIRCLED_LATIN_SMALL" && value <= 26) {
        std::string out;
        appendUtf8(out, static_cast<char32_t>(0x24CF + value));
        return out;
    } else if (numFormat == "CIRCLED_HANGUL_SYLLABLE" && value <= 14) {
        std::string out;
        appendUtf8(out, static_cast<char32_t>(0x325F + value));
        return out;
    } else if (numFormat == "CIRCLED_HANGUL_JAMO" && value <= 14) {
        std::string out;
        appendUtf8(out, static_cast<char32_t>(0x326D + value));
        return out;
    } else if (numFormat == "HANGUL_PHONETIC" && value <= 10) {
        static const char* names[] = {"일", "이", "삼", "사", "오", "육", "칠", "팔", "구", "십"};
        return names[value - 1];
    } else if (numFormat == "IDEOGRAPH" && value <= 10) {
        static const char32_t names[] = {0x4E00, 0x4E8C, 0x4E09, 0x56DB, 0x4E94,
                                         0x516D, 0x4E03, 0x516B, 0x4E5D, 0x5341};
        std::string out;
        appendUtf8(out, names[value - 1]);
        return out;
    } else if (numFormat == "CIRCLED_IDEOGRAPH" && value <= 10) {
        std::string out;
        appendUtf8(out, static_cast<char32_t>(0x327F + value));
        return out;
    }
    return std::to_string(value);
}

const NoteShape& noteShapeOf(const Document& doc, int sectionIndex, bool endnote) {
    static const NoteShape fallback;
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(doc.sections.size())) return fallback;
    const Section& section = doc.sections[static_cast<std::size_t>(sectionIndex)];
    return endnote ? section.endnote : section.footnote;
}

bool noteDecor(const std::string& text, std::string& out) {
    std::size_t index = 0;
    char32_t cp = 0;
    if (!nextCodePoint(text, index, cp) || cp < 0x20) return false;
    appendUtf8(out, cp);
    return true;
}

std::string noteLabel(const NoteShape& shape, int number) {
    if (number < 1) number = 1;
    std::string body;
    if (shape.numFormat == "USER_CHAR" && !shape.userChar.empty()) {
        body = shape.userChar;
    } else if (shape.numFormat == "SYMBOL") {
        static const char32_t marks[] = {0x203B, 0x2020, 0x2021, 0x00A7};
        appendUtf8(body, marks[(number - 1) % 4]);
    } else if (shape.numFormat == "DECAGON") {
        static const char* names[] = {"갑", "을", "병", "정", "무", "기", "경", "신", "임", "계"};
        body = names[(number - 1) % 10];
    } else if (shape.numFormat == "DECAGON_HANJA") {
        static const char32_t names[] = {0x7532, 0x4E59, 0x4E19, 0x4E01, 0x620A,
                                         0x5DF1, 0x5E9A, 0x8F9B, 0x58EC, 0x7678};
        appendUtf8(body, names[(number - 1) % 10]);
    } else {
        body = formatNumber(number, shape.numFormat);
    }
    std::string out;
    noteDecor(shape.prefix, out);
    out += body;
    noteDecor(shape.suffix, out);
    return out;
}

// 같은 종류의 주를 앞에서부터 센다. 표 칸 안의 주는 그 표 자리에서 센다. 찾으면 true 다.
bool countNotesUntil(const std::vector<Run>& runs, const Control& target, int& count) {
    for (const auto& run : runs) {
        for (const auto& item : run.items) {
            if (item.type != InlineItem::Type::Control) continue;
            if (&item.control == &target) return true;
            if (item.control.kind == target.kind) ++count;
            if (!item.control.table) continue;
            for (const auto& cell : item.control.table->cells) {
                for (const auto& para : cell.content.paragraphs) {
                    if (countNotesUntil(para.runs, target, count)) return true;
                }
            }
        }
    }
    return false;
}

// 파일에 번호가 있으면 그 값을 쓴다. 없으면 앞에서 같은 주를 센다.
// 쪽마다 다시 세는 값은 파일 속성이다. 어디서 다시 시작하는지는 그리기에서 확인하지 않았다.
int noteNumber(const Document& doc, int sectionIndex, int pageParagraph, const Control& control) {
    if (control.number > 0 && control.number < 1000000) return static_cast<int>(control.number);
    const bool endnote = control.kind == ControlKind::Endnote;
    const NoteShape& shape = noteShapeOf(doc, sectionIndex, endnote);
    int start = 1;
    if (shape.numbering == 0) {
        const std::uint32_t begin = endnote ? doc.header.beginNumbers.endnote : doc.header.beginNumbers.footnote;
        start = begin > 0 ? static_cast<int>(begin) : 1;
    } else if (shape.startNumber > 0) {
        start = shape.startNumber;
    }
    if (sectionIndex < 0) return start;
    const int first = shape.numbering == 0 ? 0 : sectionIndex;
    int count = 0;
    for (int section = first; section <= sectionIndex && section < static_cast<int>(doc.sections.size()); ++section) {
        const auto& paragraphs = doc.sections[static_cast<std::size_t>(section)].paragraphs;
        for (int pi = 0; pi < static_cast<int>(paragraphs.size()); ++pi) {
            if (shape.numbering == 2 && (section < sectionIndex || pi < pageParagraph)) continue;
            if (countNotesUntil(paragraphs[static_cast<std::size_t>(pi)].runs, control, count)) return start + count;
        }
    }
    return start + count;
}

void fillNoteAtom(Atom& atom, const Document& doc, const Control& control, const CharShape& shape, int fontSize,
                  int sectionIndex, int pageParagraph, bool measure) {
    if (control.kind != ControlKind::Footnote && control.kind != ControlKind::Endnote) return;
    const bool endnote = control.kind == ControlKind::Endnote;
    const NoteShape& noteShape = noteShapeOf(doc, sectionIndex, endnote);
    const std::string label = noteLabel(noteShape, noteNumber(doc, sectionIndex, pageParagraph, control));
    atom.noteText = label;
    atom.fontSize = fontSize > 0 ? fontSize : kEm;
    // 번호는 항상 3/4다. Y는 줄이기 전의 크기에서 21%를 뺀다. 그려지는 크기에서는 28%다.
    atom.inkSize = std::max(1, atom.fontSize * 3 / 4);
    atom.occupiesLine = true;
    std::size_t index = 0;
    char32_t cp = 0;
    if (!nextCodePoint(label, index, cp)) cp = U'1';
    const GlyphFace drawn = glyphFace(doc, shape, cp, false, true);
    atom.font = drawn.font;
    atom.ratio = drawn.ratio;
    atom.relSize = drawn.relSize;
    atom.spacing = drawn.spacing;
    const Lang lang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
    const std::size_t slot = static_cast<std::size_t>(lang);
    const std::int32_t base = slot < shape.offset.size() ? shape.offset[slot] : 0;
    atom.charOffset = base - 28;
    atom.width = 0;
    if (!measure || label.empty()) return;
    CharShape small = shape;
    small.height = atom.inkSize;
    index = 0;
    while (nextCodePoint(label, index, cp)) atom.width += glyphWidth(doc, cp, small);
}

int headValue(const Numbering* numbering, const std::vector<int>& values, int levelIndex) {
    int value = levelIndex >= 0 && levelIndex < static_cast<int>(values.size()) ? values[static_cast<std::size_t>(levelIndex)] : 0;
    if (value > 0 || !numbering) return std::max(value, 0);
    if (const ParaHead* head = findHead(*numbering, levelIndex + 1)) return head->start;
    return 1;
}

std::string expandHeading(const std::string& format, const Numbering* numbering, const std::vector<int>& values,
                          int levelIndex) {
    std::string out;
    for (std::size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '^' || i + 1 >= format.size()) {
            out.push_back(format[i]);
            continue;
        }
        const char mark = format[i + 1];
        if (mark == 'n' || mark == 'N') {
            std::string path;
            for (int level = 0; level <= levelIndex; ++level) {
                if (!path.empty()) path.push_back('.');
                const ParaHead* head = numbering ? findHead(*numbering, level + 1) : nullptr;
                path += formatNumber(headValue(numbering, values, level), head ? head->numFormat : "DIGIT");
            }
            if (mark == 'N' && !path.empty()) path.push_back('.');
            out += path;
            ++i;
            continue;
        }
        int level = 0;
        if (mark >= '1' && mark <= '9') {
            level = mark - '0';
            if (mark == '1' && i + 2 < format.size() && format[i + 2] == '0') {
                level = 10;
                ++i;
            }
            ++i;
        }
        const int index = level - 1;
        const ParaHead* head = numbering ? findHead(*numbering, level) : nullptr;
        int value = headValue(numbering, values, index);
        if (value <= 0) value = head ? head->start : 1;
        out += formatNumber(value, head ? head->numFormat : "DIGIT");
    }
    return out;
}

void applyHeading(Node& paragraph, const Paragraph& para, const Document& doc, HeadingState& state, bool counted) {
    const ParaShape* shape = findParaShape(doc, para.paraShapeId);
    if (!shape || shape->headingType.empty() || shape->headingType == "NONE") return;
    paragraph.headingType = shape->headingType;
    paragraph.headingLevel = static_cast<int>(shape->headingLevel);
    if (shape->headingType == "BULLET") {
        const Bullet* bullet = nullptr;
        for (const auto& item : doc.header.bullets) {
            if (item.id == shape->headingIdRef) {
                bullet = &item;
                break;
            }
        }
        if (!bullet) return;
        paragraph.headingText = shape->checked && !bullet->checkedText.empty() ? bullet->checkedText : bullet->text;
        copyHeadPlace(paragraph, bullet->head);
        // 칸 너비는 체크 여부와 상관없이 기록의 글머리 글자로 언어를 본다. 둘째 인자는 0이다.
        std::size_t index = 0;
        char32_t mark = 0;
        paragraph.head.halfSlot = nextCodePoint(bullet->text, index, mark) && layout::hancomCharLang(mark, false) == Lang::Latin;
        paragraph.head.imageId = bullet->imageId;
        paragraph.head.bright = bullet->bright;
        paragraph.head.contrast = bullet->contrast;
        paragraph.head.effect = bullet->effect;
        paragraph.head.ready = !paragraph.headingText.empty() || !bullet->imageId.empty();
        return;
    }
    if (shape->headingType != "OUTLINE" && shape->headingType != "NUMBER") return;
    const Numbering* numbering = numberingFor(doc, shape->headingType, shape->headingIdRef);
    int levelIndex = static_cast<int>(shape->headingLevel);
    if (levelIndex < 0) levelIndex = 0;
    if (levelIndex > 9) levelIndex = 9;
    auto& values = state.group(shape->headingType, shape->headingIdRef).values;
    const ParaHead* head = numbering ? findHead(*numbering, levelIndex + 1) : nullptr;
    if (!counted) {
        for (int deeper = levelIndex + 1; deeper < static_cast<int>(values.size()); ++deeper) values[static_cast<std::size_t>(deeper)] = 0;
        const int start = head ? head->start : 1;
        if (values[static_cast<std::size_t>(levelIndex)] == 0) values[static_cast<std::size_t>(levelIndex)] = start;
        else values[static_cast<std::size_t>(levelIndex)] += 1;
    }
    if (head) paragraph.headingFormat = head->format;
    if (head && !head->format.empty()) {
        paragraph.headingText = expandHeading(head->format, numbering, values, levelIndex);
    }
    if (head) copyHeadPlace(paragraph, *head);
    paragraph.head.ready = !paragraph.headingText.empty();
}

SpacingKind spacingKindOf(const ParaShape* para) {
    if (!para) return SpacingKind::Percent;
    if (para->lineSpacingType == "FIXED") return SpacingKind::Fixed;
    if (para->lineSpacingType == "BETWEEN_LINES") return SpacingKind::Between;
    if (para->lineSpacingType == "AT_LEAST") return SpacingKind::AtLeast;
    return SpacingKind::Percent;
}

int spacingValueOf(const ParaShape* para) {
    if (!para) return 160;
    if (spacingKindOf(para) == SpacingKind::Percent && para->lineSpacing < 0) return 160;
    return para->lineSpacing;
}

// 비율 줄간격의 추가분. 글자 상자(4의 배수)에만 비율을 곱한다.
// 0.5는 0에서 멀어지게 올린 뒤 4를 곱한다. 나머지 1·2·3은 비율을 타지 않는다.
// 10.04pt(1004)의 160% 추가분은 604이고, 10.5pt(1050)의 175% 추가분은 788이다.
int percentSpacingExtra(int size, int percent) {
    if (size <= 0) size = 1000;
    if (percent < 0) percent = 160;
    const int quant = size / 4;
    const std::int64_t numer = static_cast<std::int64_t>(quant) * (percent - 100);
    const std::int64_t rounded = numer >= 0 ? (numer + 50) / 100 : -(((-numer) + 50) / 100);
    return static_cast<int>(rounded * 4);
}

// 쪽 나눔에 쓰는 예상 높이. 한 줄의 최종 높이는 그 줄의 가장 큰 글자로 다시 계산한다.
int pitchFor(SpacingKind kind, int value, int em) {
    if (em <= 0) em = 1000;
    switch (kind) {
        case SpacingKind::Fixed: return value > 0 ? value : em;
        case SpacingKind::Between: return em + std::max(0, value);
        case SpacingKind::AtLeast: return std::max(em, value);
        case SpacingKind::Percent: {
            const int percent = value > 0 ? value : (value == 0 ? 0 : 160);
            return std::max(0, em + percentSpacingExtra(em, percent));
        }
    }
    return em;
}

// 글자와 한 줄에 있는 글자처럼 취급 개체의 세로 위치.
// 맞춤 기준은 글자 상자다. 줄간격 추가분(160%면 글자 높이의 60%)은 그 상자 아래에만 둔다.
// 개체의 textheight 는 높이 + 위아래 바깥 여백이다. 그림은 그 상자의 위에서 85%가 기준선이다.
// 줄 기준선은 글자 기준선, 개체 기준선, 수식 높이×그 백분율 중 가장 큰 값이다.
// 문단이 아래면 바깥 여백까지 포함한 상자 바닥을 글자 상자 바닥에 둔다.
// 위·가운데는 테두리를 글자 상자의 그 자리에 맞추고, 위 여백은 테두리 위에 둔다.
// 글자가 없는 줄은 개체를 줄 맨 위에 둔다.
std::int32_t characterBaseline(const Node& line, std::int32_t textBox) {
    std::int32_t glyphBox = 0;
    for (const Node& child : line.children) {
        // 문단 번호는 본문 글자 상자를 키우지 않는다.
        if (child.headingMark) continue;
        if (child.kind != NodeKind::Span || child.text.empty() || child.fontSize <= 0) continue;
        glyphBox = std::max(glyphBox, child.fontSize);
    }
    if (glyphBox > 0 && line.matchFontBaseline) {
        const std::int32_t fontBase = spanFontBox(line.children).baseline;
        if (fontBase >= 0) return fontBase;
    }
    if (glyphBox > 0) return layout::glyphBaseline(glyphBox);
    if (line.lineBaseline >= 0) return line.lineBaseline;
    return layout::glyphBaseline(textBox);
}

void placeInlineObjects(Node& line, SpacingKind kind, int spacingValue, bool keepHeight) {
    bool hasEquation = false;
    bool hasObject = false;
    for (const Node& child : line.children) {
        if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
        hasObject = true;
        if (child.baseLine >= 0) hasEquation = true;
    }
    // 글자도 조판 부호도 없이 개체만 있는 줄은 글자 상자가 0이다. 개체 높이가 곧 줄 높이다.
    const std::int32_t textBox =
        line.textHeight > 0 ? line.textHeight : (hasObject ? 0 : (line.fontSize > 0 ? line.fontSize : 1000));
    // 수식이 없으면 글꼴 어센더나 글자 상자 85%를 그대로 둔다.
    // 수식이 있으면 글자 기준선과 수식·그림 기준선 중 큰 값이 줄 기준선이다.
    const std::int32_t charBaseline =
        hasEquation ? characterBaseline(line, textBox)
                    : (line.lineBaseline >= 0 ? line.lineBaseline : layout::glyphBaseline(textBox));
    const std::int32_t baseline = hasEquation ? measuredLineBaseline(line, charBaseline) : charBaseline;
    if (hasEquation) line.lineBaseline = baseline;
    const std::string mode = line.vertAlign.empty() ? "BASELINE" : line.vertAlign;
    // 글 칸을 나중에 채우는 줄도 글자가 있다. 그때 개체를 줄 맨 위에 두면 수식이 기준선에서 뜬다.
    bool hasGlyph = line.spansLater;
    for (const Node& child : line.children) {
        if (child.headingMark) continue;
        if (child.kind == NodeKind::Span && !child.text.empty()) hasGlyph = true;
    }
    if (hasGlyph) {
        for (Node& child : line.children) {
            if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
            // 위·아래 캡션까지 든 바깥 상자를 맞춘다. 위 캡션은 아래에서 개체만 내린다.
            const std::int32_t outer = inlineObjectOuter(child);
            if (mode == "TOP") {
                child.y = -child.marginTop;
            } else if (mode == "CENTER") {
                child.y = (textBox - child.height) / 2 - child.marginTop;
            } else if (mode == "BOTTOM") {
                child.y = textBox - outer;
            } else if (child.baseLine >= 0) {
                child.y = baseline - equationBaselinePoint(child);
            } else {
                child.y = baseline - inlineObjectBaseline(child);
            }
        }
        std::int32_t rise = 0;
        for (const Node& child : line.children) {
            if (child.kind == NodeKind::Object && child.treatAsChar) rise = std::max(rise, -child.y);
        }
        if (rise > 0) {
            for (Node& child : line.children) {
                if (child.kind == NodeKind::Object) child.y += rise;
            }
        }
        line.marginTop = rise;
        // 줄에 다시 앉힌 뒤에도 위 캡션의 윗면이 그 자리다. 개체만 아래로 내린다.
        for (Node& child : line.children) {
            if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
            for (const Node& cap : child.children) {
                if (cap.role != "caption" || cap.fileHorzRel != "TOP") continue;
                child.y -= cap.y;
                break;
            }
        }
        // 글머리 그림의 y 는 붙일 때 기준선을 구워 둔다. 줄을 다시 앉히면 그 값도 맞춘다.
        for (Node& child : line.children) {
            if (!child.headingMark || child.kind != NodeKind::Object) continue;
            const int em = child.height > 0 ? child.height : 1000;
            child.y = line.marginTop + baseline - layout::glyphBaseline(em);
        }
    }
    // 쪽을 쌓은 뒤 글자만 채울 때는 다음 줄 자리를 그대로 둔다.
    if (keepHeight) return;
    // 줄 높이는 글자 상자와 개체 높이 중 큰 값이다. 글자가 없어도 문단 끝 표시·조판 부호의 상자는 든다.
    // 기준선을 맞추느라 위아래로 삐져나온 양은 더하지 않는다. 그 부분은 줄 간격 자리에 겹쳐 그린다.
    std::int32_t bottom = textBox;
    for (const Node& child : line.children) {
        if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
        bottom = std::max(bottom, inlineObjectOuter(child));
    }
    if (bottom <= 0) bottom = textBox;
    // 고정 값은 글자나 개체가 더 커도 줄 높이를 유지한다. 최소는 내용이 더 크면 늘어난다.
    // 글자에 따라·여백만 지정은 추가분을 내용 아래에 둔다. 고정·여백·최소는 샘플로 아직 확인하지 않았다.
    if (kind == SpacingKind::Fixed) {
        line.height = spacingValue > 0 ? spacingValue : textBox;
        return;
    }
    if (kind == SpacingKind::AtLeast) {
        line.height = std::max(bottom, spacingValue);
        return;
    }
    int extra = 0;
    if (kind == SpacingKind::Between) {
        extra = std::max(0, spacingValue);
    } else {
        // 0%는 글자 상자만큼 진행을 되돌려 다음 문단이 같은 높이에서 시작한다.
        // 100%보다 작으면 그만큼 다음 줄이 겹친다. 50%면 글자 높이의 절반만 진행한다.
        // 추가분은 글자 크기가 아니라 글자 상자에 비율을 곱한 값이다.
        // 글자처럼 넣은 개체의 글자 모양은 글자 상자에 들지 않지만 이 비율의 바탕에는 든다.
        const int percent = spacingValue > 0 ? spacingValue : (spacingValue == 0 ? 0 : 160);
        extra = percentSpacingExtra(std::max(line.spacingBox, textBox), percent);
    }
    line.height = std::max(0, bottom + extra);
}

// 저장 줄의 세로 진행. 다음 줄은 textHeight 에 spacing 을 더한 자리에 온다.
// vertSize 는 다음 줄 앞 글자까지 커질 수 있다. 그 값은 진행에 넣지 않는다.
// 줄의 textheight. 저장 줄과 견줄 때는 측정 상자, 아니면 글자 상자와 글자처럼 취한 개체 중 큰 값이다.
std::int32_t lineTextBox(const Node& line) {
    if (line.auditLine && line.measuredSeg.textHeight > 0) return line.measuredSeg.textHeight;
    const std::int32_t charBox = line.textHeight > 0 ? line.textHeight : line.fontSize;
    return measuredTextBox(line, charBox);
}

// 한글이 줄 높이를 잴 때 원자 하나가 올리는 값. 글자는 글자 크기, 글자처럼 취한 개체는 위아래 여백까지다.
std::int32_t scanAtomHeight(const Atom& atom) {
    if (atom.control && atom.control->placement) {
        const Placement& placement = *atom.control->placement;
        return static_cast<std::int32_t>(placement.height) + placement.marginTop + placement.marginBottom;
    }
    return atom.fontSize;
}

std::vector<std::int32_t> tallestAfter(const std::vector<Atom>& atoms) {
    std::vector<std::int32_t> tallest(atoms.size() + 1, 0);
    for (std::size_t i = atoms.size(); i > 0; --i) tallest[i - 1] = std::max(tallest[i], scanAtomHeight(atoms[i - 1]));
    return tallest;
}

namespace {

// 넘김 비교의 폭. 탭과 줄바꿈은 0이다. 저장된 줄의 원자는 폭이 비어 있어 여기서 잰다.
// trail 은 그 폭에 든 자간이다. 비교는 자간을 빼고, 펜은 자간까지 더한다.
struct ScanWidth {
    std::int32_t width = 0;
    std::int32_t trail = 0;
};

ScanWidth scanAtomWidth(const Document& doc, const Atom& atom) {
    if (atom.control) return {atom.width, 0};
    if (atom.tab || atom.hard || atom.occupiesLine) return {};
    if (atom.metricsReady || atom.width > 0) return {atom.width, atom.trail};
    const CharShape* face = findCharShape(doc, atom.charShapeId);
    if (!face) face = &fallbackCharShape();
    CharShape numberShape;
    if (atom.ignoreSpacing) {
        numberShape = *face;
        numberShape.spacing.fill(0);
        face = &numberShape;
    }
    const Lang lang = isHangul(atom.cp) ? Lang::Hangul : Lang::Latin;
    return {glyphWidth(doc, atom.cp, *face), layout::glyphTrail(atom.cp, *face, lang, &doc)};
}

// 넘쳐도 줄에 남는 글자. 공백류, 줄바꿈, 탭, U+001F 다.
bool scanKeepsOverflow(const Atom& atom) {
    if (atom.control) return false;
    const char32_t cp = atom.cp;
    return atom.hard || atom.tab || cp == U' ' || cp == 0x3000 || (cp >= 0x2000 && cp <= 0x200B) || cp == 0x1F;
}

}  // namespace

// 한글이 기록하는 줄 높이(vertsize). 줄 시작부터 단 폭을 넘치는 글자 바로 앞까지 원자 높이의 최댓값이다.
// 끊을 자리는 그 뒤에 물러나 고르므로 다음 줄로 넘어간 앞 글자도 들고, 줄바꿈에서도 멈추지 않는다.
// 단 폭은 문단 여백도 떠 있는 개체도 빼지 않는다. 뒤에 더 큰 원자가 없으면 폭을 재지 않는다.
std::int32_t scannedVertSize(const Document& doc, const std::vector<Atom>& atoms,
                             const std::vector<std::int32_t>& tallest, std::size_t from, std::size_t to,
                             std::int32_t own, std::int32_t pen, std::int32_t limit) {
    if (to >= atoms.size() || to >= tallest.size() || tallest[to] <= own) return own;
    std::int32_t height = own;
    bool placed = false;
    for (std::size_t i = from; i < atoms.size(); ++i) {
        const Atom& atom = atoms[i];
        const ScanWidth width = scanAtomWidth(doc, atom);
        if (placed && pen + width.width - width.trail > limit && !scanKeepsOverflow(atom)) break;
        pen += width.width;
        placed = true;
        if (i >= to) height = std::max(height, scanAtomHeight(atom));
        if (tallest[i + 1] <= height) break;
    }
    return height;
}

// 측정 높이로 그릴 때는 lineseg 의 줄 경계만 쓰고 높이는 빌리지 않는다.
const LineSeg* storedHeightOf(const LineSeg* seg) { return gMeasuredLineHeight ? nullptr : seg; }

int storedLinePitch(const LineSeg* seg) {
    if (!seg) return -1;
    const int box = seg->textHeight > 0 ? seg->textHeight : seg->vertSize;
    if (box <= 0 && seg->spacing == 0) return -1;
    return std::max(0, box + seg->spacing);
}

// 이 줄의 첫 글자 위치에 해당하는 저장 줄. 없으면 우리 줄 높이를 쓴다.
const LineSeg* storedLineAt(const Frame& frame, const Paragraph& para, int textPos) {
    if (!frame.useStoredLineHeight || para.lineSegs.empty()) return nullptr;
    const LineSeg* found = &para.lineSegs.front();
    for (const LineSeg& seg : para.lineSegs) {
        if (static_cast<int>(seg.textPos) <= textPos) found = &seg;
        else break;
    }
    return found;
}

void placeFrameObjects(Node& paper, Node* body, Node& column, const Frame& frame,
                       std::vector<Obstacle>& obstacles, int onlyParagraph);

std::int32_t headingEm(const CharShape& shape) {
    return shape.height > 0 ? shape.height : 1000;
}

std::int32_t measureHeading(const Document& doc, const std::string& text, const CharShape& shape,
                            std::vector<std::int32_t>* advances) {
    std::int32_t width = 0;
    std::size_t index = 0;
    char32_t cp = 0;
    while (nextCodePoint(text, index, cp)) {
        // 머리 글자도 본문처럼 한글 언어 판정(둘째 인자 1)이 고른 칸의 글꼴로 잰다.
        const std::int32_t measured = layout::glyphAdvance(cp, shape, layout::hancomCharLang(cp, true), &doc);
        const std::int32_t em = shape.height > 0 ? shape.height : kEm;
        const std::int32_t advance = measured > 0 ? measured : (cp == ' ' || cp < 128 ? em / 2 : em);
        if (advances) advances->push_back(advance);
        width += advance;
    }
    return width;
}

struct HeadMetrics {
    std::int32_t em = 1000;
    std::int32_t slot = 0;
    std::int32_t hang = 0;
    std::int32_t content = 0;
    CharShape face;
};

// 칸은 실제 문자열 폭 또는 글자 크기이고, 그다음 너비 조정을 더한다. 간격은 칸 밖이다.
// 영문 글머리표(그리기용이 아닌 글자 언어가 영문)는 칸이 글자 크기의 절반이다.
HeadMetrics headMetrics(const Node& paragraph, const Document& doc, const Paragraph& para,
                        std::vector<std::int32_t>* advances) {
    HeadMetrics out;
    if (!paragraph.head.ready) return out;
    const bool picture = !paragraph.head.imageId.empty();
    // 머리 글자 모양이 없으면 문단 끝 글자(길이 - 1)의 모양이다. 있어도 크기가 0이면 크기만 그 모양에서 받는다.
    const CharShape* ending = nullptr;
    for (auto run = para.runs.rbegin(); run != para.runs.rend() && !ending; ++run) {
        ending = findCharShape(doc, run->charShapeId);
    }
    const CharShape* shaped = nullptr;
    if (paragraph.head.charShapeId != 0xFFFFFFFFu) shaped = findCharShape(doc, paragraph.head.charShapeId);
    if (!shaped) shaped = ending;
    if (shaped) out.face = *shaped;
    else out.face.height = 1000;
    if (out.face.height == 0 && ending) out.face.height = ending->height;
    out.em = headingEm(out.face);
    const std::int32_t measured =
        picture ? out.em : measureHeading(doc, paragraph.headingText, out.face, advances);
    out.content = picture ? out.em : measured;
    out.slot = paragraph.head.useInstWidth ? measured : paragraph.head.halfSlot ? out.em / 2 : out.em;
    if (out.slot <= 0) out.slot = out.em;
    out.slot += paragraph.head.widthAdjust;
    if (out.slot < 0) out.slot = 0;
    std::int32_t gap = paragraph.head.textOffset;
    if (paragraph.head.offsetType != "HWPUNIT") {
        const long long num = static_cast<long long>(out.em) * paragraph.head.textOffset;
        gap = static_cast<std::int32_t>(num >= 0 ? (num + 50) / 100 : (num - 50) / 100);
    }
    out.hang = out.slot + gap;
    return out;
}

// 문단 번호·개요·글머리표는 줄 상자를 줄이지 않고, 그리는 글자만 칸만큼 민다.
// 한글은 머리 종류를 가리지 않고 머리 칸 + 간격만큼 본문 펜을 민다.
std::int32_t numberingPush(const Node& paragraph, const Document& doc, const Paragraph& para) {
    if (!paragraph.head.ready || !paragraph.head.autoIndent) return 0;
    if (paragraph.headingType != "NUMBER" && paragraph.headingType != "OUTLINE" && paragraph.headingType != "BULLET") return 0;
    return headMetrics(paragraph, doc, para, nullptr).hang;
}

// 머리는 칸만큼 본문보다 앞에 둔다. 본문은 그 칸을 줄 들여쓰기로 삼아 함께 민다.
void attachHeading(Node& line, const Node& paragraph, const Document& doc, const Paragraph& para, bool shrink) {
    if (!paragraph.head.ready) return;
    const bool picture = !paragraph.head.imageId.empty();
    if (!picture && paragraph.headingText.empty()) return;

    std::vector<std::int32_t> advances;
    const HeadMetrics box = headMetrics(paragraph, doc, para, picture ? nullptr : &advances);
    const CharShape& face = box.face;
    // 칸이 문자열보다 넓을 때만 남는 폭을 정렬로 나눈다.
    const std::int32_t spare = std::max<std::int32_t>(0, box.slot - box.content);
    std::int32_t pad = 0;
    if (paragraph.head.align == "CENTER") pad = spare / 2;
    else if (paragraph.head.align == "RIGHT") pad = spare;
    const std::int32_t at = (paragraph.head.autoIndent ? -box.hang : 0) + pad;

    if (picture) {
        Node obj;
        obj.kind = NodeKind::Object;
        obj.headingMark = true;
        obj.role = "picture";
        obj.binItemId = paragraph.head.imageId;
        obj.picture.bright = paragraph.head.bright;
        obj.picture.contrast = paragraph.head.contrast;
        obj.picture.effect = paragraph.head.effect;
        obj.width = box.em;
        obj.height = box.em;
        obj.x = at;
        const int textBox = line.textHeight > 0 ? line.textHeight : box.em;
        const int baseline = line.lineBaseline >= 0 ? line.lineBaseline : layout::glyphBaseline(textBox);
        obj.y = line.marginTop + baseline - layout::glyphBaseline(box.em);
        line.children.insert(line.children.begin(), std::move(obj));
    } else {
        Node span;
        span.kind = NodeKind::Span;
        span.headingMark = true;
        span.text = paragraph.headingText;
        span.fontSize = box.em;
        span.charShapeId = paragraph.head.charShapeId;
        span.advances = std::move(advances);
        span.width = box.content;
        span.x = at;
        span.color = colorHex(face.textColor);
        if (span.color.empty()) span.color = "#000000";
        span.bold = face.bold;
        span.italic = face.italic;
        span.superscript = face.superscript;
        span.subscript = face.subscript;
        char32_t first = 0;
        std::size_t index = 0;
        if (!nextCodePoint(span.text, index, first)) first = U'가';
        const GlyphFace chosen = glyphFaceIn(doc, face, layout::hancomCharLang(first, true), first, true, true);
        span.font = chosen.font;
        span.ratio = chosen.ratio > 0 ? chosen.ratio : 100;
        span.relSize = chosen.relSize > 0 ? chosen.relSize : 100;
        line.children.insert(line.children.begin(), std::move(span));
    }

    if (!paragraph.head.autoIndent && box.hang > 0) {
        line.x += box.hang;
        if (line.measuredSeg.horzSize > 0) line.measuredSeg.horzPos += box.hang;
        if (shrink) {
            line.width = std::max<std::int32_t>(0, line.width - box.hang);
            if (line.measuredSeg.horzSize > box.hang) line.measuredSeg.horzSize -= box.hang;
        }
    }
}

const CharShape& fallbackCharShape() {
    static const CharShape shape = [] {
        CharShape fallback;
        fallback.height = kEm;
        fallback.ratio.fill(100);
        return fallback;
    }();
    return shape;
}


std::vector<Atom> buildAtoms(const Document& doc, const Paragraph& para, const ParaShape* paraShape,
                             const AtomOptions& options) {
    const bool useLines = options.storedPositions;
    std::vector<Atom> atoms;
    int pos = 0;
    BreakClassifier breaks(paraShape);
    for (const auto& run : para.runs) {
        const CharShape* shape = findCharShape(doc, run.charShapeId);
        if (!shape) shape = &fallbackCharShape();
        const int fontSize = shape->height > 0 ? shape->height : kEm;
        for (const auto& item : run.items) {
            if (item.type == InlineItem::Type::Text) {
                CharShape numberShape;
                const CharShape* measure = shape;
                if (item.ignoreSpacing) {
                    numberShape = *shape;
                    numberShape.spacing.fill(0);
                    measure = &numberShape;
                }
                std::size_t index = 0;
                char32_t cp = 0;
                while (nextCodePoint(item.text, index, cp)) {
                    Atom atom;
                    atom.cp = cp;
                    atom.pos = pos;
                    // lineseg textpos 는 탭을 8칸으로 센다. 한 글자로 세면 줄바꿈 뒤 글자가 같은 줄에 남는다.
                    if (useLines && cp == '\t') pos += 8;
                    else if (useLines && cp > 0xFFFF) pos += 2;
                    else pos += 1;
                    atom.fontSize = fontSize;
                    atom.charShapeId = run.charShapeId;
                    if (options.lazyMetrics) {
                        atom.ignoreSpacing = item.ignoreSpacing;
                    } else {
                        const GlyphFace drawn = glyphFace(doc, *shape, cp, !useLines, !options.deferText);
                        if (!options.deferText) atom.font = drawn.font;
                        atom.ratio = drawn.ratio;
                        atom.relSize = drawn.relSize;
                        atom.spacing = item.ignoreSpacing ? 0 : drawn.spacing;
                    }
                    const Lang offsetLang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
                    const std::size_t offsetSlot = static_cast<std::size_t>(offsetLang);
                    atom.charOffset = offsetSlot < shape->offset.size() ? shape->offset[offsetSlot] : 0;
                    atom.markpen = item.markpen;
                    atom.hard = cp == '\n';
                    atom.space = cp == ' ' || cp == 0x2007;
                    atom.tab = cp == '\t';
                    if (options.lazyMetrics) {
                        // 폭은 이 글자를 줄에 올릴 때만 잰다. 앞뒤 줄은 통째로 가져온다.
                        if (atom.hard) {
                            atom.width = 0;
                            atom.trail = 0;
                            atom.metricsReady = true;
                        }
                    } else if (!useLines) {
                        atom.width = atom.hard ? 0 : glyphWidth(doc, cp, *measure);
                        const Lang trailLang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
                        atom.trail = atom.hard ? 0 : layout::glyphTrail(cp, *measure, trailLang, &doc);
                    }
                    breaks.classify(atom, atoms);
                    atoms.push_back(atom);
                }
            } else if (visualObject(item.control) && item.control.placement &&
                       item.control.placement->treatAsChar) {
                Atom atom;
                atom.pos = pos;
                pos += useLines ? 8 : 1;
                atom.control = &item.control;
                atom.fontSize = fontSize;
                atom.charShapeId = run.charShapeId;
                if (!options.deferText) atom.font = glyphFace(doc, *shape, U' ', !useLines, true).font;
                const Placement& placement = *item.control.placement;
                atom.width = static_cast<std::int32_t>(placement.width) + placement.marginLeft +
                             placement.marginRight;
                // 왼쪽 캡션은 개체 왼쪽이 아니라 캡션 왼쪽이 글자 자리다. 다음 글자는 밀린 개체 뒤에서 시작한다.
                if (item.control.caption && item.control.caption->side == "LEFT" && !placement.groupInRoot) {
                    const Caption& cap = *item.control.caption;
                    std::int32_t capWidth = static_cast<std::int32_t>(cap.width);
                    if (capWidth <= 0) capWidth = static_cast<std::int32_t>(cap.lastWidth);
                    if (capWidth <= 0) capWidth = static_cast<std::int32_t>(placement.width);
                    if (capWidth < 200) capWidth = 200;
                    atom.width += capWidth + static_cast<std::int32_t>(cap.gap);
                }
                breaks.endWord();
                atoms.push_back(atom);
            } else if (item.type == InlineItem::Type::Control && occupiesBodyLine(item.control)) {
                Atom atom;
                atom.pos = pos;
                const bool note =
                    item.control.kind == ControlKind::Footnote || item.control.kind == ControlKind::Endnote;
                if (useLines) pos += 8;
                else if (note) pos += 1;
                atom.fontSize = fontSize;
                atom.charShapeId = run.charShapeId;
                atom.occupiesLine = true;
                atom.width = 0;
                if (note) {
                    fillNoteAtom(atom, doc, item.control, *shape, fontSize, options.sectionIndex,
                                 options.pageParagraph, !useLines);
                }
                breaks.endWord();
                atoms.push_back(atom);
            } else if (item.type == InlineItem::Type::Control) {
                if (options.unseen) options.unseen->push_back({pos, fontSize});
                // 글자처럼 취급하지 않는 그림도 줄 위치에서는 8칸이다.
                // 이 칸을 빼면 다음 줄이 수식 뒤 글자에서 시작해 그 문장이 갈라진다.
                if (useLines) pos += 8;
            }
        }
    }
    // 문단 끝 표시는 마지막 구간의 글자 모양이다. 줄바꿈으로 끝나면 그 뒤 빈 줄에 붙는다.
    if (options.unseen && !para.runs.empty() && (atoms.empty() || !atoms.back().hard)) {
        const CharShape* shape = findCharShape(doc, para.runs.back().charShapeId);
        options.unseen->push_back({pos, shape && shape->height > 0 ? shape->height : kEm});
    }
    return atoms;
}

int unseenSize(const std::vector<UnseenMark>& marks, int from, int to) {
    int size = 0;
    for (const UnseenMark& mark : marks) {
        if (mark.pos >= from && mark.pos < to) size = std::max(size, mark.fontSize);
    }
    return size;
}

// lazyMetrics 로 만든 원자의 글꼴과 폭을 잰다. 탭 폭은 탭 위치에서 정한다.
void measureLazyAtom(const Document& doc, Atom& atom) {
    if (atom.metricsReady || atom.control || atom.occupiesLine) return;
    if (atom.hard) {
        atom.width = 0;
        atom.trail = 0;
        atom.metricsReady = true;
        return;
    }
    const CharShape* face = findCharShape(doc, atom.charShapeId);
    if (!face) face = &fallbackCharShape();
    CharShape numberShape;
    const CharShape* measure = face;
    if (atom.ignoreSpacing) {
        numberShape = *face;
        numberShape.spacing.fill(0);
        measure = &numberShape;
    }
    const GlyphFace drawn = glyphFace(doc, *face, atom.cp, true, true);
    atom.font = drawn.font;
    atom.ratio = drawn.ratio;
    atom.relSize = drawn.relSize;
    atom.spacing = atom.ignoreSpacing ? 0 : drawn.spacing;
    if (!atom.tab) {
        atom.width = glyphWidth(doc, atom.cp, *measure);
        const Lang trailLang = isHangul(atom.cp) ? Lang::Hangul : Lang::Latin;
        atom.trail = layout::glyphTrail(atom.cp, *measure, trailLang, &doc);
    }
    atom.metricsReady = true;
}

ColumnResume layoutColumn(Node& column, const Frame& frame, std::vector<Obstacle>& obstacles,
                           int startParagraph, std::size_t startAtom, Node& paper, Node* body, bool stopAtBottom,
                           int stopParagraph, bool commit, bool pageOnOverflow, bool pageHasContent,
                           std::vector<TableCarry> initialCarry = {}) {
    const auto& paragraphs = *frame.paragraphs;
    const Document& doc = *frame.doc;
    std::int32_t y = 0;
    ColumnResume resume;

    for (int pi = startParagraph; pi < static_cast<int>(paragraphs.size()); ++pi) {
        const std::size_t atomBegin = pi == startParagraph ? startAtom : 0;
        const bool hasCarry = pi == startParagraph && !initialCarry.empty();
        const bool fresh = atomBegin == 0 && !hasCarry;
        const Paragraph& para = paragraphs[static_cast<std::size_t>(pi)];
        const bool hasContent = y > 0 || !column.children.empty();
        // 다단 나누기 앞까지만 이 단에 넣는다. 다음 구역은 아래쪽에 새 단으로 시작한다.
        if (fresh && pi >= stopParagraph) {
            resume.paragraph = pi;
            resume.atom = 0;
            resume.done = false;
            return resume;
        }
        if (fresh && frame.page && (hasContent || pageHasContent) && paragraphStartsPage(para, doc)) {
            resume.paragraph = pi;
            resume.atom = 0;
            resume.done = false;
            resume.breakPage = true;
            return resume;
        }
        if (fresh && frame.page && hasContent && para.columnBreak) {
            resume.paragraph = pi;
            resume.atom = 0;
            resume.done = false;
            return resume;
        }
        if (fresh && commit) placeFrameObjects(paper, body, column, frame, obstacles, pi);
        Node paragraph;
        paragraph.kind = NodeKind::Paragraph;
        paragraph.paragraph = pi;
        paragraph.section = frame.sectionIndex;
        paragraph.x = 0;
        paragraph.y = y;
        // 쪽을 넘어 이어지는 글도 머리 자리만큼 본문을 민다. 번호는 앞 쪽에서 이미 셌다.
        if ((fresh || atomBegin > 0) && commit && frame.headings) {
            applyHeading(paragraph, para, doc, *frame.headings, !fresh);
        }
        std::int32_t localY = 0;
        const ParaShape* paraShape = findParaShape(doc, para.paraShapeId);
        // 여백은 저장된 HWPUNIT 그대로 뺀다. mm 소수점은 이미 버려져 있고, 4배수로 맞추지 않는다.
        const ParagraphSpan span = paragraphSpan(column.width, paraShape);
        const std::int32_t marginLeft = span.marginLeft;
        const std::int32_t marginRight = paraShape ? paraShape->marginRight : 0;
        const std::int32_t contentWidth = span.width;
        paragraph.width = contentWidth;

        auto placeParaObject = [&](const Control& ctrl, Node obj, std::int32_t ox, std::int32_t oy) -> bool {
            const Placement& placement = *ctrl.placement;
            const std::int32_t captionShift = std::max<std::int32_t>(0, obj.x - ox);
            const std::int32_t captionDrop = std::max<std::int32_t>(0, obj.y - oy);
            if (!placement.treatAsChar && placement.vertRelTo == "PARA" && placement.flowWithText) {
                std::int32_t outerTop = y + obj.y - captionDrop - placement.marginTop;
                if (outerTop < 0) {
                    obj.y += -outerTop;
                    outerTop = 0;
                }
                if (textAvoids(placement.textWrap)) {
                    const bool curBlock = placement.textWrap == "TOP_AND_BOTTOM";
                    const std::int32_t curLeft = marginLeft + ox - placement.marginLeft;
                    const std::int32_t curRight = curLeft + static_cast<std::int32_t>(placement.width) +
                                                  placement.marginLeft + placement.marginRight + captionShift;
                    const std::int32_t outerH = obj.height + captionDrop + placement.marginTop + placement.marginBottom;
                    for (std::size_t guard = 0; guard <= obstacles.size(); ++guard) {
                        bool moved = false;
                        for (const auto& obs : obstacles) {
                            const bool xOverlap =
                                curBlock || obs.block || (curLeft < obs.x + obs.width && curRight > obs.x);
                            if (!xOverlap) continue;
                            if (outerTop < obs.y + obs.height && outerTop + outerH > obs.y) {
                                const std::int32_t nextTop = obs.y + obs.height;
                                if (nextTop > outerTop) {
                                    obj.y += nextTop - outerTop;
                                    outerTop = nextTop;
                                    moved = true;
                                    break;
                                }
                            }
                        }
                        if (!moved) break;
                    }
                }
            }
            if (stopAtBottom && ctrl.kind == ControlKind::Table && ctrl.table) {
                const std::string pb = ctrl.table->pageBreak.empty() ? "CELL" : ctrl.table->pageBreak;
                const std::int32_t availH = column.height - (y + obj.y);
                if (pb == "NONE") {
                    if (obj.height > availH && (hasContent || pageHasContent)) {
                        return false;
                    }
                } else if (obj.height > availH) {
                    Node topPart, botPart;
                    if (splitTableNode(obj, *ctrl.table, availH, topPart, botPart)) {
                        obj = std::move(topPart);
                        TableCarry carry;
                        carry.control = &ctrl;
                        carry.table = std::move(botPart);
                        carry.table.x = ox;
                        carry.table.y = 0;
                        carry.ox = ox;
                        carry.oy = 0;
                        resume.carryTables.push_back(std::move(carry));
                    } else if (hasContent || pageHasContent) {
                        return false;
                    }
                }
            }
            const std::int32_t objH = obj.height;
            const std::int32_t objY = obj.y;
            paragraph.children.push_back(std::move(obj));
            Obstacle obstacle;
            obstacle.x = marginLeft + ox - placement.marginLeft;
            obstacle.y = y + objY - placement.marginTop;
            obstacle.width = static_cast<std::int32_t>(placement.width) + placement.marginLeft +
                             placement.marginRight + captionShift;
            obstacle.height = objH + placement.marginTop + placement.marginBottom;
            obstacle.block = placement.textWrap == "TOP_AND_BOTTOM";
            obstacle.paragraph = pi;
            if (textAvoids(placement.textWrap)) obstacles.push_back(obstacle);
            (void)oy;
            return true;
        };

        if (hasCarry) {
            std::vector<TableCarry> carries = std::move(initialCarry);
            initialCarry.clear();
            for (auto& c : carries) {
                if (!c.control || !c.control->placement) continue;
                placeParaObject(*c.control, std::move(c.table), c.ox, c.oy);
            }
        }

        for (const auto& run : para.runs) {
            for (const auto& item : run.items) {
                if (!visualObject(item.control) || !item.control.placement || item.control.placement->treatAsChar) {
                    continue;
                }
                const Placement& placement = *item.control.placement;
                if (fresh && (placement.vertRelTo == "PARA" || placement.horzRelTo == "PARA")) {
                    // 좌표는 문단 내용 기준이다. 여백은 문단 상자 밖에 둔다.
                    // 문단 기준은 문단 내용이다. 종이 기준 가로는 종이 왼쪽이 원점이라 본문 여백만큼 되돌린다.
                    std::int32_t ox = 0;
                    if (placement.horzRelTo == "PAPER" && frame.page && body) {
                        const std::int32_t paperX =
                            aligned(placement.horzAlign, 0, paper.width, static_cast<std::int32_t>(placement.width),
                                    placement.horzOffset);
                        ox = paperX - body->x - column.x - marginLeft;
                    } else {
                        const bool paraX = placement.horzRelTo == "PARA";
                        ox = aligned(placement.horzAlign, 0, paraX ? contentWidth : column.width,
                                     static_cast<std::int32_t>(placement.width), placement.horzOffset);
                        if (!paraX) ox -= marginLeft;
                        ox = insetX(placement, ox);
                    }
                    std::int32_t oy = 0;
                    if (placement.vertRelTo == "PAPER" && frame.page && body) {
                        const std::int32_t paperY =
                            aligned(placement.vertAlign, 0, paper.height, static_cast<std::int32_t>(placement.height),
                                    placement.vertOffset);
                        oy = paperY - body->y - column.y - y;
                    } else {
                        const bool paraY = placement.vertRelTo == "PARA";
                        oy = aligned(placement.vertAlign, 0, paraY ? 0 : column.height,
                                     static_cast<std::int32_t>(placement.height), placement.vertOffset);
                        if (!paraY) oy -= y;
                        oy = insetY(placement, oy);
                    }
                    Node built = makeObject(item.control, ox, oy, doc, frame.depth, frame.defaultTab,
                                            frame.useStoredLineHeight);
                    if (!placeParaObject(item.control, std::move(built), ox, oy)) {
                        resume.paragraph = pi;
                        resume.atom = 0;
                        resume.done = false;
                        if (pageOnOverflow) resume.breakPage = true;
                        return resume;
                    }
                }
            }
        }

        const std::int32_t indent = paraShape ? paraShape->indent : 0;
        const std::int32_t spacingPrev = fresh && paraShape ? paraShape->spacingPrev : 0;
        const std::int32_t spacingNext = paraShape ? paraShape->spacingNext : 0;
        const std::int32_t frameRight = contentWidth;
        paragraph.marginLeft = marginLeft;
        paragraph.marginRight = marginRight;
        paragraph.align = !frame.forceAlign.empty()
                              ? frame.forceAlign
                              : (paraShape && !paraShape->align.empty() ? paraShape->align : "JUSTIFY");
        paragraph.vertAlign = paraShape && !paraShape->vertAlign.empty() ? paraShape->vertAlign : "BASELINE";
        applyParagraphBorder(paragraph, doc, paraShape);
        paragraph.indent = indent;
        paragraph.spacingPrev = spacingPrev;
        paragraph.spacingNext = spacingNext;
        if (spacingPrev > 0) localY += spacingPrev;
        bool firstLine = atomBegin == 0;
        // 들여쓰기·내어쓰기는 lineseg 가로에 없다. 줄 위치는 빼고, 글자만 가로로 민다.
        const bool storedLines = frame.useStoredLineHeight && !para.lineSegs.empty();
        auto glyphShift = [&]() {
            if (!storedLines) return numberingPush(paragraph, doc, para);
            std::int32_t shift = 0;
            if (firstLine && indent > 0) shift = indent;
            if (!firstLine && indent < 0) shift = -indent;
            // 번호는 lineseg 가로를 바꾸지 않는다. 글자만 칸만큼 오른쪽으로 민다.
            shift += numberingPush(paragraph, doc, para);
            return shift;
        };
        auto lineLeftOf = [&]() {
            std::int32_t left = 0;
            if (!storedLines) {
                if (firstLine && indent > 0) left += indent;
                if (!firstLine && indent < 0) left += -indent;
            }
            if (left >= frameRight) left = std::max<std::int32_t>(0, frameRight - 1);
            return left;
        };
        const SpacingKind kind = spacingKindOf(paraShape);
        const int spacingValue = spacingValueOf(paraShape);
        auto pitchOf = [&](int em) { return pitchFor(kind, spacingValue, em); };
        int emptyEm = 0;
        for (const auto& run : para.runs) {
            if (const CharShape* shape = findCharShape(doc, run.charShapeId)) {
                emptyEm = std::max(emptyEm, shape->height);
            }
        }
        const std::int32_t defaultPitch = pitchOf(emptyEm > 0 ? emptyEm : kEm);
        // 저장된 줄이 있으면 글자 폭으로 다시 끊지 않는다. 위치는 lineseg 와 같은 WCHAR 칸이다.
        const bool useLines = frame.useStoredLineHeight && !para.lineSegs.empty();
        const bool deferText = useLines && frame.deferSpans && frame.sectionIndex >= 0;
        struct ReflowTimer {
            bool on = false;
            std::chrono::steady_clock::time_point started{};
            explicit ReflowTimer(bool enable) : on(enable) {
                if (on) started = std::chrono::steady_clock::now();
            }
            ~ReflowTimer() {
                if (!on) return;
                gPageObj.reflowMs += pageCalcMs(started);
                gPageObj.reflowParas += 1;
            }
        } reflowTimer(gPageObjOn && frame.useStoredLineHeight && !useLines);
        (void)reflowTimer;
        AtomOptions atomOptions;
        atomOptions.storedPositions = useLines;
        atomOptions.deferText = deferText;
        atomOptions.sectionIndex = frame.sectionIndex;
        atomOptions.pageParagraph = frame.pageParagraph;
        std::vector<UnseenMark> unseen;
        atomOptions.unseen = &unseen;
        std::vector<Atom> atoms = buildAtoms(doc, para, paraShape, atomOptions);
        const std::vector<std::int32_t> tallest = tallestAfter(atoms);

        // 탭 위치는 본문 왼쪽 끝 기준이다. 0보다 작으면 본문 시작, 본문 폭보다 크면 본문 오른쪽 끝이다.
        const TabRuler tabs(doc, paraShape, marginLeft, contentWidth, column.width, frame.defaultTab);
        // 흐름 조판은 원자를 만들 때 이미 쟀고, 저장된 줄은 폭을 재지 않는다.
        auto lineHost = makeLineHost([](Atom&) {},
                                     [&](const Atom& atom, std::int32_t pen) {
                                         return makeObject(*atom.control, pen, 0, doc, frame.depth, frame.defaultTab,
                                                           frame.useStoredLineHeight);
                                     });

        // 문단 끝 줄바꿈의 빈 줄은 글자 위치가 원자 다음이다. 앞 쪽에서 넘긴 그 줄은 여기서 만든다.
        // 저장된 줄에서는 한글이 그 빈 줄을 lineseg 하나로 남긴다. 그 lineseg 가 있을 때만 만든다.
        const LineSeg* trailingSeg = nullptr;
        if (useLines && !atoms.empty() && atoms.back().hard) {
            const LineSeg* seg = storedLineAt(frame, para, atoms.back().pos + 1);
            if (seg && static_cast<int>(seg->textPos) > atoms.back().pos) trailingSeg = seg;
        }
        const bool trailingBreak = !atoms.empty() && atoms.back().hard && (!useLines || trailingSeg);
        const bool trailingBreakOnly =
            !fresh && atomBegin >= atoms.size() && paragraph.children.empty() && trailingBreak;
        if (!fresh && atomBegin >= atoms.size() && paragraph.children.empty() && !trailingBreakOnly) continue;

        if ((fresh || hasCarry) && atoms.empty()) {
            const int emptySize = emptyEm > 0 ? emptyEm : kEm;
            const LineSeg* cached = storedHeightOf(storedLineAt(frame, para, 0));
            const int lineEm = cached && cached->textHeight > 0 ? cached->textHeight : emptySize;
            const int storedPitch = storedLinePitch(cached);
            const int linePitch = storedPitch >= 0 ? storedPitch : defaultPitch;
            for (int obsGuard = 0; obsGuard < 32; ++obsGuard) {
                // 문단 위 간격만 있고 아직 줄이 없으면 이 쪽이 비어 있는 것이다.
                // 글자처럼 취급한 개체는 쪽보다 커도 이 쪽에 두고, 쪽 나눔은 적용하지 않는다.
                const bool pageStarted = y > 0 || !paragraph.children.empty() || pageHasContent;
                if (stopAtBottom && y + localY + lineEm >= column.height && pageStarted) {
                    paragraph.height = std::max(localY, 0);
                    if (localY > 0) {
                        if (frame.story > 0) stampStory(paragraph, frame.story);
                        column.children.push_back(std::move(paragraph));
                    }
                    resume.paragraph = pi;
                    resume.atom = 0;
                    resume.done = false;
                    if (pageOnOverflow) resume.breakPage = true;
                    return resume;
                }
                const std::int32_t lineLeft = lineLeftOf();
                std::vector<Obstacle> priorObstacles;
                for (const auto& obstacle : obstacles) {
                    if (obstacle.paragraph == pi) continue;
                    Obstacle item = obstacle;
                    item.x -= marginLeft;
                    priorObstacles.push_back(item);
                }
                auto priorGaps = freeGaps(y + localY, linePitch, lineLeft, frameRight, priorObstacles);
                if (!priorGaps.empty()) break;
                std::int32_t below = y + localY;
                for (const auto& obstacle : obstacles) {
                    if (obstacle.paragraph == pi) continue;
                    if (y + localY < obstacle.y + obstacle.height && y + localY + linePitch > obstacle.y) {
                        below = std::max(below, obstacle.y + obstacle.height);
                    }
                }
                if (below <= y + localY) below = y + localY + std::max(linePitch, 1);
                localY += below - (y + localY);
            }
            const std::int32_t lineLeft = lineLeftOf();
            std::vector<Obstacle> localObstacles = obstacles;
            for (auto& item : localObstacles) item.x -= marginLeft;
            auto gaps = freeGaps(y + localY, linePitch, lineLeft, frameRight, localObstacles);
            Node line;
            line.kind = NodeKind::Line;
            line.paragraph = pi;
            line.y = localY;
            line.x = gaps.empty() ? lineLeft : gaps[0].first;
            line.indent = glyphShift();
            if (useLines) line.marginLeft = marginLeft;
            line.align = paragraph.align;
            line.vertAlign = paragraph.vertAlign;
            line.lastLine = true;
            line.width = gaps.empty() ? 0 : std::max<std::int32_t>(0, gaps[0].second - line.x);
            line.charShapeId = !para.runs.empty() ? para.runs.front().charShapeId : 0;
            line.fontSize = emptySize;
            line.textHeight = lineEm;
            line.height = linePitch;
            if (useLines && !para.lineSegs.empty()) {
                line.storedSeg = para.lineSegs[0];
            } else if (frame.useStoredLineHeight) {
                line.storedSeg.spacing = std::max<std::int32_t>(0, line.height - line.marginTop - line.textHeight);
            }
            line.start = 0;
            line.end = 0;
            if (firstLine) attachHeading(line, paragraph, doc, para, !useLines);
            paragraph.children.push_back(std::move(line));
            localY += paragraph.children.back().height;
        }

        std::size_t cursor = atomBegin;
        std::size_t guardCursor = cursor;
        int spins = 0;
        while (cursor < atoms.size()) {
            const std::size_t lineStartCursor = cursor;
            if (cursor != guardCursor) {
                guardCursor = cursor;
                spins = 0;
            } else if (++spins > 3) {
                const Atom& atom = atoms[cursor];
                const LineSeg* forced = storedHeightOf(storedLineAt(frame, para, atom.pos));
                const int forcedStored = storedLinePitch(forced);
                const int forcedPitch = forcedStored >= 0 ? forcedStored : pitchOf(atom.fontSize);
                if (!atom.hard) {
                    Node object;
                    if (atom.control) {
                        object = makeObject(*atom.control, 0, 0, doc, frame.depth, frame.defaultTab,
                                            frame.useStoredLineHeight);
                    }
                    Node line = singleAtomLine(doc, atom, std::move(object));
                    line.paragraph = pi;
                    line.x = lineLeftOf();
                    line.indent = glyphShift();
                    if (useLines) line.marginLeft = marginLeft;
                    line.y = localY;
                    line.align = paragraph.align;
                    line.vertAlign = paragraph.vertAlign;
                    line.height = forcedPitch;
                    line.textHeight = forced && forced->textHeight > 0 ? forced->textHeight : line.fontSize;
                    placeInlineObjects(line, kind, spacingValue);
                    localY += line.height;
                    if (firstLine) attachHeading(line, paragraph, doc, para, !useLines);
                    paragraph.children.push_back(std::move(line));
                    firstLine = false;
                }
                ++cursor;
                guardCursor = cursor;
                spins = 0;
                continue;
            }
            const LineSeg* cached = storedLineAt(frame, para, atoms[cursor].pos);
            const LineSeg* sized = storedHeightOf(cached);
            const int lineEm = sized && sized->textHeight > 0
                                   ? sized->textHeight
                                   : (atoms[cursor].fontSize > 0 ? atoms[cursor].fontSize : kEm);
            const int storedPitch = storedLinePitch(sized);
            const int pitch = storedPitch >= 0 ? storedPitch : pitchOf(lineEm);
            // 쪽 넘김은 글자 높이만 본다. 줄간격은 더하지 않는다.
            // 글자 바닥이 본문 끝에 닿으면 다음 쪽으로 보낸다. 그보다 1만큼 위에 있으면 남긴다.
            // 쪽이 비어 있으면 글자처럼 취급한 개체가 쪽보다 커도 그 쪽에 남긴다.
            const bool pageStarted = y > 0 || !paragraph.children.empty() || pageHasContent;
            if (stopAtBottom && y + localY + lineEm >= column.height && pageStarted) {
                paragraph.height = std::max(localY, 0);
                if (localY > 0) {
                    if (frame.story > 0) stampStory(paragraph, frame.story);
                    column.children.push_back(std::move(paragraph));
                }
                resume.paragraph = pi;
                resume.atom = cursor;
                resume.textPos = cursor < atoms.size() ? atoms[cursor].pos : 0;
                resume.done = false;
                // 평행 다단은 넘치면 옆 단으로 흐르지 않고 다음 쪽으로 간다.
                if (pageOnOverflow) resume.breakPage = true;
                return resume;
            }
            const std::int32_t lineLeft = lineLeftOf();
            bool onlyOccupiesLine = true;
            for (std::size_t k = cursor; k < atoms.size(); ++k) {
                if (!atoms[k].occupiesLine) {
                    onlyOccupiesLine = false;
                    break;
                }
            }
            if (onlyOccupiesLine) {
                std::vector<Obstacle> priorObstacles;
                for (const auto& obstacle : obstacles) {
                    if (obstacle.paragraph == pi) continue;
                    Obstacle item = obstacle;
                    item.x -= marginLeft;
                    priorObstacles.push_back(item);
                }
                auto priorGaps = freeGaps(y + localY, pitch, lineLeft, frameRight, priorObstacles);
                if (priorGaps.empty()) {
                    std::int32_t below = y + localY;
                    for (const auto& obstacle : obstacles) {
                        if (obstacle.paragraph == pi) continue;
                        if (y + localY < obstacle.y + obstacle.height && y + localY + pitch > obstacle.y) {
                            below = std::max(below, obstacle.y + obstacle.height);
                        }
                    }
                    if (below <= y + localY) below = y + localY + std::max(pitch, 1);
                    localY += below - (y + localY);
                    continue;
                }
            }
            std::vector<Obstacle> localObstacles = obstacles;
            for (auto& item : localObstacles) item.x -= marginLeft;
            auto gaps = freeGaps(y + localY, pitch, lineLeft, frameRight, localObstacles);
            if (gaps.empty()) {
                if (onlyOccupiesLine) {
                    gaps.push_back({lineLeft, lineLeft});
                } else {
                    std::int32_t below = y + localY;
                    for (const auto& obstacle : obstacles) {
                        if (y + localY < obstacle.y + obstacle.height && y + localY + pitch > obstacle.y) {
                            below = std::max(below, obstacle.y + obstacle.height);
                        }
                    }
                    if (below <= y + localY) below = y + localY + std::max(pitch, 1);
                    localY += below - (y + localY);
                    continue;
                }
            }
            LineSpec spec;
            spec.doc = &doc;
            spec.paraShape = paraShape;
            spec.tabs = &tabs;
            spec.gaps = std::move(gaps);
            spec.storedBreaks = useLines;
            spec.deferText = deferText;
            spec.keepAdvances = !useLines;
            // 줄 시작은 첫 글자가 아니라 그 줄 lineseg 의 textpos 다.
            // 빈 문단 앞의 구역·단·번호 컨트롤을 빼면 24처럼 뒤로 밀린다.
            if (useLines && cached) spec.firstPos = static_cast<int>(cached->textPos);
            if (useLines) {
                const int startPos = atoms[cursor].pos;
                for (const LineSeg& seg : para.lineSegs) {
                    if (static_cast<int>(seg.textPos) > startPos) {
                        spec.lineLimit = static_cast<int>(seg.textPos);
                        break;
                    }
                }
            }
            LineFill fill;
            fill.line.kind = NodeKind::Line;
            fill.line.paragraph = pi;
            fill.line.y = localY;
            fill.line.vertAlign = paragraph.vertAlign;
            fill.line.charShapeId = atoms[cursor].charShapeId;
            fill.line.font = atoms[cursor].font;
            fill.line.fontSize = atoms[cursor].fontSize > 0 ? atoms[cursor].fontSize : kEm;
            fillLine(spec, atoms, cursor, fill, lineHost);
            // 줄높이 측정은 이 배치와 같은 값이다. 글자 상자는 글자 크기이고, 그림 높이에 줄간격을 곱하지 않는다.
            closeLine(fill);
            Node& line = fill.line;
            // 문단 끝 표시와 조판 부호는 원자가 없다. 그 글자 모양이 더 크면 글자 상자도 그 크기다.
            // 저장된 줄은 lineseg 범위로, 흐름 조판은 다음 줄 첫 원자 앞까지로 그 줄 몫을 정한다.
            {
                const bool head = lineStartCursor == 0;
                int from = head ? std::numeric_limits<int>::min() : atoms[lineStartCursor].pos;
                int to = cursor < atoms.size() ? atoms[cursor].pos : std::numeric_limits<int>::max();
                if (useLines && cached) {
                    if (!head) from = static_cast<int>(cached->textPos);
                    to = spec.lineLimit;
                }
                line.textHeight = std::max<std::int32_t>(line.textHeight, unseenSize(unseen, from, to));
            }
            // 글꼴에 어울리는 줄 높이면 글자 상자가 글꼴 상자로 커진다.
            applyFontBaseline(line, paraShape);
            const std::int32_t charBox = line.textHeight;
            line.indent = glyphShift();
            if (useLines) line.marginLeft = marginLeft;
            line.align = paragraph.align;
            if (useLines && cached) {
                std::int32_t ownText = charBox;
                for (const auto& child : line.children) {
                    if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
                    const std::int32_t outer = child.height + child.marginTop + child.marginBottom;
                    if (outer > ownText) ownText = outer;
                }
                line.auditLine = true;
                line.auditIndex = static_cast<std::int32_t>(cached - para.lineSegs.data());
                line.auditText = ownText;
                line.auditHorz = cached->horzPos;
            }
            // 저장된 줄은 그 너비와 높이를 쓴다. 글자 advance 는 쪽을 쌓을 때 재지 않는다.
            // 줄바꿈은 칸 너비를 그대로 쓴다. 기록하는 horzsize 만 0.2인치보다 좁아지지 않는다.
            const std::int32_t gapWidth = std::max<std::int32_t>(0, spec.gaps[fill.gapIndex].second - line.x);
            const std::int32_t measuredWidth =
                (onlyOccupiesLine && gapWidth == 0) ? 0 : std::max(kMinParagraphWidth, gapWidth);
            if (useLines && cached && cached->horzSize > 0) line.width = cached->horzSize;
            else line.width = gapWidth;
            placeInlineObjects(line, kind, spacingValue);
            if (line.auditLine) {
                std::int32_t contentBottom = line.marginTop + charBox;
                for (const auto& child : line.children) {
                    if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
                    contentBottom = std::max(contentBottom, inlineObjectBottom(child));
                }
                const std::int32_t measuredBox = measuredTextBox(line, charBox);
                // 기준선 맞춤으로 삐져나온 양은 줄 높이에 들지 않는다. 줄 피치까지 넘친 만큼만 비트맵을 늘린다.
                line.auditRise = 0;
                line.auditBelow = std::max(0, contentBottom - line.height);
                line.auditPitch = line.height;
                line.storedSeg = *cached;
                line.measuredSeg.textPos = static_cast<std::uint32_t>(std::max(0, line.start));
                line.measuredSeg.vertPos = y + line.y;
                line.measuredSeg.vertSize = measuredBox;
                line.measuredSeg.textHeight = measuredBox;
                // 글자·그림 85%·수식 높이×백분율 중 큰 값이 이미 줄 기준선이다.
                const std::int32_t glyphBaseline = line.lineBaseline >= 0 ? line.lineBaseline
                                                                          : layout::glyphBaseline(charBox);
                line.measuredSeg.baseline = lineSegBaseline(line, measuredBox, glyphBaseline);
                line.measuredSeg.spacing = line.height - measuredBox;
                line.measuredSeg.horzPos = line.x + line.marginLeft;
                line.measuredSeg.horzSize = measuredWidth;
                std::uint32_t flags = measuredLineFlags(firstLine, indent);
                if (paragraph.head.ready) {
                    if (firstLine) flags |= 1u << 21;
                    else if (paragraph.head.autoIndent) flags |= 1u << 20;
                }
                line.measuredSeg.flags = flags;
            } else if (frame.useStoredLineHeight) {
                line.storedSeg.spacing = std::max<std::int32_t>(0, line.height - line.marginTop - line.textHeight);
            }
            std::int32_t inlineObjBox = 0;
            for (const Node& child : line.children) {
                if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
                inlineObjBox = std::max(inlineObjBox, child.y + child.marginTop + child.height + child.marginBottom);
            }
            if (sized && sized->textHeight > 0 && sized->textHeight != line.textHeight && inlineObjBox <= 0) {
                line.textHeight = sized->textHeight;
                placeInlineObjects(line, kind, spacingValue);
            }
            // 저장된 줄은 개체가 있어도 그 줄 높이를 쓴다. 우리 계산은 lineseg 검사에만 남긴다.
            // 개체 크기를 바꾸면 편집이 그 문단의 lineseg 를 지우므로 그때는 계산한 높이가 된다.
            if (storedPitch >= 0) line.height = storedPitch;
            const std::int32_t finalLineBox =
                sized && sized->textHeight > 0 ? sized->textHeight : lineCharBox(line);
            const bool linePastPage = y > 0 || !paragraph.children.empty() || pageHasContent;
            if (stopAtBottom && y + localY + finalLineBox >= column.height && linePastPage) {
                paragraph.height = std::max(localY, 0);
                if (localY > 0 && !paragraph.children.empty()) {
                    if (frame.story > 0) stampStory(paragraph, frame.story);
                    column.children.push_back(std::move(paragraph));
                }
                resume.paragraph = pi;
                resume.atom = lineStartCursor;
                resume.textPos = fill.start < 0 ? 0 : fill.start;
                resume.done = false;
                if (pageOnOverflow) resume.breakPage = true;
                return resume;
            }
            // 줄 높이(vertsize)는 줄 시작부터 단 폭을 넘칠 때까지 잰다. 다음 줄 앞 글자도 들 수 있다.
            line.measuredSeg.vertSize =
                scannedVertSize(doc, atoms, tallest, lineStartCursor, cursor, lineTextBox(line),
                                lineStartCursor == 0 ? numberingPush(paragraph, doc, para) : 0, column.width);
            // 글자 폭과 개체 가로 위치는 이 줄을 그릴 때 잡는다.
            // 글자처럼 취한 개체와 캡션이 저장된 줄 밖으로 나가면 그 비트맵만 늘린다. 다음 줄 자리는 그대로다.
            markInlineOverflow(line);
            if (useLines) line.deferTabs = true;
            if (firstLine) attachHeading(line, paragraph, doc, para, !useLines);
            paragraph.children.push_back(std::move(line));
            localY += paragraph.children.back().height;
            firstLine = false;
        }

        if (trailingBreak) {
            int emptySize = atoms.back().fontSize > 0 ? atoms.back().fontSize : (emptyEm > 0 ? emptyEm : kEm);
            const LineSeg* trailingSize = storedHeightOf(trailingSeg);
            if (trailingSize && trailingSize->textHeight > 0) emptySize = trailingSize->textHeight;
            // 문단 끝의 빈 줄도 글자 바닥이 본문 끝에 닿으면 다음 쪽으로 보낸다.
            const bool pageStarted = y > 0 || !paragraph.children.empty() || pageHasContent;
            if (stopAtBottom && pageStarted && y + localY + emptySize >= column.height) {
                paragraph.height = std::max(localY, 0);
                if (localY > 0 && !paragraph.children.empty()) {
                    if (frame.story > 0) stampStory(paragraph, frame.story);
                    column.children.push_back(std::move(paragraph));
                }
                resume.paragraph = pi;
                resume.atom = atoms.size();
                resume.textPos = atoms.back().pos + 1;
                resume.done = false;
                if (pageOnOverflow) resume.breakPage = true;
                return resume;
            }
            Node line;
            line.kind = NodeKind::Line;
            line.paragraph = pi;
            line.y = localY;
            line.x = lineLeftOf();
            line.indent = glyphShift();
            line.align = paragraph.align;
            line.vertAlign = paragraph.vertAlign;
            line.lastLine = true;
            line.width = std::max<std::int32_t>(0, frameRight - line.x);
            line.fontSize = emptySize;
            line.textHeight = line.fontSize;
            line.height = pitchOf(line.fontSize);
            if (trailingSeg) {
                const int storedPitch = storedLinePitch(trailingSize);
                if (storedPitch >= 0) line.height = storedPitch;
                line.storedSeg = *trailingSeg;
                line.marginLeft = marginLeft;
            }
            line.start = atoms.back().pos + 1;
            line.end = atoms.back().pos + 1;
            paragraph.children.push_back(std::move(line));
            localY += paragraph.children.back().height;
        }

        for (auto it = paragraph.children.rbegin(); it != paragraph.children.rend(); ++it) {
            if (it->kind != NodeKind::Line) continue;
            it->lastLine = true;
            break;
        }
        if (spacingNext > 0) localY += spacingNext;
        // 글자가 10pt보다 작으면 그 글자 줄 높이만 차지한다. 10pt 줄로 바닥을 깔면 출처처럼 낮은 줄이 벌어진다.
        paragraph.height = std::max(localY, 0);
        if (frame.story > 0) stampStory(paragraph, frame.story);
        column.children.push_back(std::move(paragraph));
        y += column.children.back().height;
        if (!resume.carryTables.empty()) {
            resume.paragraph = pi;
            resume.atom = atoms.size();
            resume.done = false;
            if (pageOnOverflow) resume.breakPage = true;
            return resume;
        }
        if (stopAtBottom && y > column.height) {
            resume.paragraph = pi + 1;
            resume.atom = 0;
            resume.done = resume.paragraph >= static_cast<int>(paragraphs.size());
            if (pageOnOverflow) resume.breakPage = true;
            return resume;
        }
    }
    resume.done = true;
    return resume;
}

void placeFrameObjects(Node& paper, Node* body, Node& column, const Frame& frame,
                       std::vector<Obstacle>& obstacles, int onlyParagraph) {
    const Document& doc = *frame.doc;
    for (int pi = 0; pi < static_cast<int>(frame.paragraphs->size()); ++pi) {
        if (pi != onlyParagraph) continue;
        const auto& para = (*frame.paragraphs)[static_cast<std::size_t>(pi)];
        for (const auto& run : para.runs) {
            for (const auto& item : run.items) {
                if (!visualObject(item.control) || !item.control.placement || item.control.placement->treatAsChar) {
                    continue;
                }
                const Placement& placement = *item.control.placement;
                if (placement.vertRelTo == "PARA" || placement.horzRelTo == "PARA") continue;
                const bool paperRel = placement.vertRelTo == "PAPER" || placement.horzRelTo == "PAPER";
                const std::int32_t originX = paperRel && frame.page ? 0 : column.x + (body ? body->x : 0);
                const std::int32_t originY = paperRel && frame.page ? 0 : column.y + (body ? body->y : 0);
                const std::int32_t refW = paperRel && frame.page ? paper.width : column.width;
                const std::int32_t refH = paperRel && frame.page ? paper.height : column.height;
                const std::int32_t absX = insetX(
                    placement,
                    aligned(placement.horzRelTo == "PAPER" && frame.page ? placement.horzAlign : placement.horzAlign,
                            placement.horzRelTo == "PAPER" && frame.page ? 0 : originX,
                            placement.horzRelTo == "PAPER" && frame.page ? paper.width : refW,
                            static_cast<std::int32_t>(placement.width), placement.horzOffset));
                const std::int32_t absY = insetY(
                    placement, aligned(placement.vertAlign, placement.vertRelTo == "PAPER" && frame.page ? 0 : originY,
                                       placement.vertRelTo == "PAPER" && frame.page ? paper.height : refH,
                                       static_cast<std::int32_t>(placement.height), placement.vertOffset));
                Node* parent = &column;
                std::int32_t localX = absX - column.x;
                std::int32_t localY = absY - column.y;
                if (frame.page && paperRel) {
                    parent = &paper;
                    localX = absX;
                    localY = absY;
                } else if (frame.page && body && placement.vertRelTo != "COLUMN" && placement.horzRelTo != "COLUMN") {
                    parent = body;
                    localX = absX - body->x;
                    localY = absY - body->y;
                }
                parent->children.push_back(
                    makeObject(item.control, localX, localY, doc, frame.depth, frame.defaultTab,
                               frame.useStoredLineHeight));
                if (textAvoids(placement.textWrap)) {
                    const std::int32_t captionDrop = parent->children.back().y - localY;
                    const std::int32_t captionShift = std::max<std::int32_t>(0, parent->children.back().x - localX);
                    Obstacle obstacle;
                    obstacle.x = absX - originX - placement.marginLeft;
                    obstacle.y = absY - originY - placement.marginTop + captionDrop;
                    if (parent != &column) {
                        obstacle.x = absX - (body ? body->x : 0) - column.x - placement.marginLeft;
                        obstacle.y = absY - (body ? body->y : 0) - column.y - placement.marginTop + captionDrop;
                    }
                    obstacle.width = static_cast<std::int32_t>(placement.width) + placement.marginLeft +
                                     placement.marginRight + captionShift;
                    obstacle.height = parent->children.back().height + placement.marginTop +
                                      placement.marginBottom;
                    obstacle.block = placement.textWrap == "TOP_AND_BOTTOM";
                    obstacle.paragraph = pi;
                    obstacles.push_back(obstacle);
                }
            }
        }
    }
}

Node layoutFrame(const Frame& given, int startParagraph, std::size_t startAtom, ColumnResume* resume) {
    // 표 칸·글상자·캡션·머리말·바탕쪽도 글머리표와 문단 번호를 붙인다. 번호는 그 틀 안에서 처음부터 센다.
    HeadingState ownHeadings;
    Frame withHeadings;
    if (!given.headings) {
        withHeadings = given;
        withHeadings.headings = &ownHeadings;
    }
    const Frame& frame = given.headings ? given : withHeadings;
    const PageDef page = frame.pageDef;
    const std::int32_t paperW = static_cast<std::int32_t>(page.width);
    const std::int32_t paperH = static_cast<std::int32_t>(page.height);
    const std::int32_t contentLeft = frame.page ? static_cast<std::int32_t>(page.marginLeft) : 0;
    const std::int32_t contentTop =
        frame.page ? static_cast<std::int32_t>(page.marginTop) + static_cast<std::int32_t>(page.marginHeader) : 0;
    const std::int32_t contentW =
        frame.page ? std::max<std::int32_t>(200, paperW - contentLeft - static_cast<std::int32_t>(page.marginRight))
                   : paperW;
    const std::int32_t contentH =
        frame.page ? std::max<std::int32_t>(200, paperH - contentTop - static_cast<std::int32_t>(page.marginBottom) -
                                                     static_cast<std::int32_t>(page.marginFooter) - frame.bottomReserve)
                   : paperH;

    Node root;
    root.kind = frame.page ? NodeKind::Paper : NodeKind::Body;
    root.width = paperW;
    root.height = paperH;
    if (frame.page) {
        root.marginTop = kDefaultPageMarginTopHu;
        root.marginLeft = kDefaultPageMarginLeftHu;
        root.marginRight = kDefaultPageMarginRightHu;
        root.marginBottom = kDefaultPageMarginBottomHu;
    }

    Node body;
    body.kind = NodeKind::Body;
    body.x = contentLeft;
    body.y = contentTop;
    body.width = contentW;
    body.height = contentH;

    // 글상자는 안의 마지막 단 정의를 따른다. 쪽은 다단 나누기마다 아래쪽에 새 단을 쌓는다.
    const int columnAt = frame.page ? startParagraph : static_cast<int>(frame.paragraphs->size()) - 1;
    Node* bodyPtr = frame.page ? &body : nullptr;
    Node& paper = frame.page ? root : body;
    const int openEnd = std::numeric_limits<int>::max();
    auto columnUsed = [](const Node& column) {
        std::int32_t bottom = 0;
        for (const auto& child : column.children) {
            bottom = std::max(bottom, child.y + child.height);
            if (child.kind == NodeKind::Paragraph) {
                for (const auto& grand : child.children) {
                    if (grand.kind == NodeKind::Object && !grand.treatAsChar) {
                        bottom = std::max(bottom, child.y + grand.y + grand.height);
                    }
                }
            }
        }
        return bottom;
    };
    auto placeBand = [&](ColumnResume step, int y, int bandHeight, int stopParagraph, bool commit,
                         bool pageOnOverflow) {
        const ColumnLayout columns = columnLayoutAt(*frame.paragraphs, step.paragraph);
        const std::vector<ColumnBox> boxes = columnBoxes(columns, contentW, true);
        const int count = static_cast<int>(boxes.size());
        bool pageHasContent = false;
        if (bodyPtr) {
            for (const auto& child : bodyPtr->children) {
                if (child.kind == NodeKind::Column && !child.children.empty()) {
                    pageHasContent = true;
                    break;
                }
            }
        }
        std::vector<Node> cols;
        std::vector<Obstacle> bandObstacles;
        for (int i = 0; i < count; ++i) {
        Node column;
        column.kind = NodeKind::Column;
        column.x = boxes[static_cast<std::size_t>(i)].x;
        column.y = y;
        column.width = boxes[static_cast<std::size_t>(i)].width;
        column.height = bandHeight;
        column.story = frame.story;
            applyColumnRule(column, columns, i + 1 < count);
            if (!step.done && !step.breakPage) {
                std::vector<Obstacle> obstacles;
                for (const auto& obs : bandObstacles) {
                    if (obs.x + obs.width <= column.x || obs.x >= column.x + column.width) continue;
                    Obstacle local = obs;
                    local.x -= column.x;
                    local.y -= column.y;
                    obstacles.push_back(local);
                }
                const std::size_t seeded = obstacles.size();
                const bool stopAtBottom = frame.page || i + 1 < count;
                step = layoutColumn(column, frame, obstacles, step.paragraph, step.atom, paper, bodyPtr,
                                    stopAtBottom, stopParagraph, commit, pageOnOverflow, pageHasContent,
                                    std::move(step.carryTables));
                for (std::size_t oi = seeded; oi < obstacles.size(); ++oi) {
                    Obstacle shared = obstacles[oi];
                    shared.x += column.x;
                    shared.y += column.y;
                    bandObstacles.push_back(shared);
                }
            }
            if (!column.children.empty()) pageHasContent = true;
            cols.push_back(std::move(column));
        }
        return std::pair<ColumnResume, std::vector<Node>>{step, std::move(cols)};
    };
    // 칸·글상자는 잘리지 않게 큰 높이로 쌓고, 단 높이는 글이 끝난 자리로 맞춘다.
    auto fitColumnToContent = [&](Node& column) {
        if (column.kind != NodeKind::Column) return;
        column.height = std::max<std::int32_t>(columnUsed(column), 1);
    };

    ColumnResume step;
    step.paragraph = startParagraph;
    step.atom = startAtom;
    if (resume && !resume->carryTables.empty()) step.carryTables = std::move(resume->carryTables);
    step.done = frame.paragraphs->empty();
    if (!frame.page) {
        const ColumnLayout columns = columnLayoutAt(*frame.paragraphs, columnAt);
        const std::vector<ColumnBox> boxes = columnBoxes(columns, contentW, true);
        const int count = static_cast<int>(boxes.size());
        std::vector<Obstacle> bandObstacles;
        for (int i = 0; i < count; ++i) {
            Node column;
            column.kind = NodeKind::Column;
            column.x = boxes[static_cast<std::size_t>(i)].x;
            column.y = 0;
            column.width = boxes[static_cast<std::size_t>(i)].width;
            column.height = contentH;
            column.story = frame.story;
            applyColumnRule(column, columns, i + 1 < count);
            if (!step.done && !step.breakPage) {
                std::vector<Obstacle> obstacles;
                for (const auto& obs : bandObstacles) {
                    if (obs.x + obs.width <= column.x || obs.x >= column.x + column.width) continue;
                    Obstacle local = obs;
                    local.x -= column.x;
                    local.y -= column.y;
                    obstacles.push_back(local);
                }
                const std::size_t seeded = obstacles.size();
                const bool stopAtBottom = i + 1 < count;
                step = layoutColumn(column, frame, obstacles, step.paragraph, step.atom, paper, bodyPtr,
                                    stopAtBottom, openEnd, true, false, false, std::move(step.carryTables));
                for (std::size_t oi = seeded; oi < obstacles.size(); ++oi) {
                    Obstacle shared = obstacles[oi];
                    shared.x += column.x;
                    shared.y += column.y;
                    bandObstacles.push_back(shared);
                }
            }
            body.children.push_back(std::move(column));
        }
        std::int32_t bottom = 0;
        for (Node& column : body.children) {
            fitColumnToContent(column);
            bottom = std::max(bottom, column.y + column.height);
        }
        body.height = std::max(bottom, 1);
    } else {
        int y = 0;
        for (int guard = 0; !step.done && !step.breakPage && y < contentH && guard < 32; ++guard) {
            const int regionStart = step.paragraph;
            const std::size_t regionAtom = step.atom;
            const ColumnLayout columns = columnLayoutAt(*frame.paragraphs, regionStart);
            const int count = std::max(1, columns.count);
            const bool parallel = parallelColumns(columns.type);
            const bool balance = balancedColumns(columns.type) && count > 1;
            int regionEnd = static_cast<int>(frame.paragraphs->size());
            for (int i = regionStart + 1; i < regionEnd; ++i) {
                const Paragraph& p = (*frame.paragraphs)[static_cast<std::size_t>(i)];
                if (p.layoutBreak || (balance && p.columnBreak)) {
                    regionEnd = i;
                    break;
                }
            }
            if (regionAtom == 0 && regionStart < static_cast<int>(frame.paragraphs->size())) {
                bool pageHas = false;
                for (const auto& child : body.children) {
                    if (child.kind == NodeKind::Column && !child.children.empty()) {
                        pageHas = true;
                        break;
                    }
                }
                if (pageHas && paragraphStartsPage((*frame.paragraphs)[static_cast<std::size_t>(regionStart)],
                                                   *frame.doc)) {
                    step.paragraph = regionStart;
                    step.atom = 0;
                    step.done = false;
                    step.breakPage = true;
                    break;
                }
            }
            const int remaining = contentH - y;
            int bandHeight = remaining;
            if (balance) {
                auto fits = [&](int height) {
                    ColumnResume probe;
                    probe.paragraph = regionStart;
                    probe.atom = regionAtom;
                    probe.done = false;
                    probe = placeBand(probe, y, height, regionEnd, false, false).first;
                    // 이 문단 자체가 쪽으로 넘어가면 이 높이에는 글이 안 들어간 것이다.
                    const bool laterPage = probe.breakPage && probe.paragraph != regionStart;
                    return laterPage || probe.done || probe.paragraph >= regionEnd;
                };
                if (fits(remaining)) {
                    int lo = 1;
                    int hi = remaining;
                    while (lo < hi) {
                        const int mid = lo + (hi - lo) / 2;
                        if (fits(mid)) hi = mid;
                        else lo = mid + 1;
                    }
                    bandHeight = lo;
                }
            }
            auto placed = placeBand(step, y, bandHeight, regionEnd, true, parallel);
            step = placed.first;
            std::int32_t used = 0;
            std::int32_t flow = 0;
            for (const auto& column : placed.second) {
                used = std::max(used, columnUsed(column));
                flow = std::max(flow, columnCharBottom(column));
            }
            if (used == 0 && step.paragraph == regionStart && step.atom == regionAtom && !step.breakPage &&
                !step.done) {
                step.breakPage = true;
                break;
            }
            // 한 단과 평행 다단의 한 줄은 글 끝으로 높이를 줄인다. 다음 줄은 그 아래에 둔다.
            if (count == 1 || parallel) {
                for (auto& column : placed.second) column.height = std::max<std::int32_t>(used, 1);
            }
            for (auto& column : placed.second) body.children.push_back(std::move(column));
            // 평행 다단은 단 나누기가 단 개수만큼 지나면 같은 쪽의 다음 줄에서 왼쪽 단으로 돌아온다.
            if (parallel && !step.breakPage && !step.done && step.paragraph < regionEnd &&
                !(step.paragraph == regionStart && step.atom == regionAtom)) {
                y += std::max<std::int32_t>(used, 1);
                if (y < contentH) continue;
            }
            const bool regionDone = step.done || step.breakPage || step.paragraph >= regionEnd;
            if (!regionDone || step.breakPage) break;
            // 단 설정이 바뀌면 마지막 줄의 글자 높이 아래에 단 간격(기본 4mm)을 두고 다음 단을 놓는다.
            // 그 첫 줄의 글자 높이가 본문 끝에 닿거나 넘으면 다음 쪽으로 보낸다. 줄간격은 더하지 않는다.
            if (!step.done && step.paragraph >= regionEnd &&
                step.paragraph < static_cast<int>(frame.paragraphs->size())) {
                const std::int32_t gap = frame.columnRegionGap > 0 ? frame.columnRegionGap : 1134;
                const std::int32_t nextTop = y + flow + gap;
                const int nextEm = paragraphEm(*frame.doc, (*frame.paragraphs)[static_cast<std::size_t>(step.paragraph)]);
                if (nextTop + nextEm >= contentH) {
                    step.breakPage = true;
                    break;
                }
                y = nextTop;
            } else {
                y += std::max<std::int32_t>(used, 1);
            }
            if (step.paragraph == regionStart && step.atom == regionAtom) break;
        }
    }
    if (resume) *resume = step;
    if (frame.page) {
        root.children.insert(root.children.begin(), std::move(body));
    } else {
        root = std::move(body);
        root.kind = NodeKind::Body;
    }
    return root;
}

bool dateCommand(const std::string& command) {
    return command.find("yy") != std::string::npos || command.find("MM") != std::string::npos ||
           command.find("dd") != std::string::npos;
}

std::tm clockNow() {
    const std::time_t now = std::time(nullptr);
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &now);
#else
    localtime_r(&now, &out);
#endif
    return out;
}

std::tm clockFromText(const std::string& text, const std::tm& fallback) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    const int count = std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
    if (count < 3) {
        const int dateOnly = std::sscanf(text.c_str(), "%d-%d-%d", &year, &month, &day);
        if (dateOnly != 3) return fallback;
    }
    std::tm out = fallback;
    out.tm_year = year - 1900;
    out.tm_mon = month - 1;
    out.tm_mday = day;
    if (count >= 6) {
        out.tm_hour = hour;
        out.tm_min = minute;
        out.tm_sec = second;
    }
    return out;
}

std::string formatDate(const std::string& command, const std::tm& tm) {
    std::string out;
    for (std::size_t i = 0; i < command.size();) {
        auto put = [&](const char* token, int width, int value) {
            const std::size_t n = std::char_traits<char>::length(token);
            if (command.compare(i, n, token) != 0) return false;
            char buf[8];
            if (width == 4) std::snprintf(buf, sizeof(buf), "%04d", value);
            else if (width == 2) std::snprintf(buf, sizeof(buf), "%02d", value);
            else std::snprintf(buf, sizeof(buf), "%d", value);
            out += buf;
            i += n;
            return true;
        };
        if (put("yyyy", 4, tm.tm_year + 1900)) continue;
        if (put("yy", 2, (tm.tm_year + 1900) % 100)) continue;
        if (put("MM", 2, tm.tm_mon + 1)) continue;
        if (put("dd", 2, tm.tm_mday)) continue;
        if (put("HH", 2, tm.tm_hour)) continue;
        if (put("mm", 2, tm.tm_min)) continue;
        if (put("ss", 2, tm.tm_sec)) continue;
        if (command[i] == 'M') {
            out += std::to_string(tm.tm_mon + 1);
            ++i;
            continue;
        }
        if (command[i] == 'd') {
            out += std::to_string(tm.tm_mday);
            ++i;
            continue;
        }
        if (command[i] == 'H') {
            out += std::to_string(tm.tm_hour);
            ++i;
            continue;
        }
        out.push_back(command[i]);
        ++i;
    }
    return out;
}

std::string numberText(const Control& control, int pageNumber, int pageCount) {
    const int value = control.numType == "TOTAL_PAGE" ? pageCount : pageNumber;
    const std::string body = formatNumber(value < 1 ? 1 : value, control.numFormat.empty() ? "DIGIT" : control.numFormat);
    if (control.prefix.empty() && control.suffix.empty()) return body;
    std::string out = control.prefix;
    if (!control.prefix.empty()) out += " ";
    out += body;
    if (!control.suffix.empty()) out += " ";
    out += control.suffix;
    return out;
}

void fillPageFields(Paragraph& para, int pageNumber, int pageCount, bool hideNumber, const std::tm& dateNow,
                    const std::tm& docDate) {
    const bool bare = para.text().find_first_not_of(" \t\r\n") == std::string::npos;
    for (auto& run : para.runs) {
        for (auto& item : run.items) {
            if (item.type != InlineItem::Type::Control) continue;
            const Control& control = item.control;
            if (control.kind == ControlKind::AutoNumber &&
                (control.numType == "PAGE" || control.numType == "TOTAL_PAGE")) {
                const std::string pen = item.markpen;
                item = InlineItem::makeText(hideNumber ? std::string() : numberText(control, pageNumber, pageCount));
                item.ignoreSpacing = true;
                item.markpen = pen;
                continue;
            }
            if (bare && control.kind == ControlKind::Field && dateCommand(control.command) &&
                (control.ctrlId == "%dte" || control.ctrlId == "%ddt")) {
                const std::tm& when = control.ctrlId == "%ddt" ? docDate : dateNow;
                item = InlineItem::makeText(formatDate(control.command, when));
                continue;
            }
            for (auto& list : item.control.subLists) {
                for (auto& child : list.paragraphs) {
                    fillPageFields(child, pageNumber, pageCount, hideNumber, dateNow, docDate);
                }
            }
            if (item.control.caption) {
                for (auto& child : item.control.caption->content.paragraphs) {
                    fillPageFields(child, pageNumber, pageCount, hideNumber, dateNow, docDate);
                }
            }
            if (item.control.table) {
                for (auto& cell : item.control.table->cells) {
                    for (auto& child : cell.content.paragraphs) {
                        fillPageFields(child, pageNumber, pageCount, hideNumber, dateNow, docDate);
                    }
                }
            }
            for (auto& child : item.control.children) {
                for (auto& list : child.subLists) {
                    for (auto& nested : list.paragraphs) {
                        fillPageFields(nested, pageNumber, pageCount, hideNumber, dateNow, docDate);
                    }
                }
            }
        }
    }
}

Paragraph numberedParagraph(const Paragraph& source, int pageNumber, int pageCount, bool hideNumber,
                            const std::tm& dateNow, const std::tm& docDate) {
    Paragraph para = source;
    fillPageFields(para, pageNumber, pageCount, hideNumber, dateNow, docDate);
    return para;
}

int applySlot(const std::string& type) {
    if (type == "EVEN") return 1;
    if (type == "ODD") return 2;
    return 0;
}

const Control* bandFor(const Control* const slots[3], int pageNumber) {
    const int side = pageNumber % 2 == 0 ? 1 : 2;
    if (slots[side]) return slots[side];
    return slots[0];
}

void shiftBand(Node& band, std::int32_t x, std::int32_t top, std::int32_t boxHeight, const std::string& align) {
    const std::int32_t used = rowContentBottom(band);
    const std::int32_t slack = boxHeight - used;
    std::int32_t shift = 0;
    if (align == "BOTTOM") shift = std::max<std::int32_t>(0, slack);
    else if (align == "CENTER") shift = std::max<std::int32_t>(0, slack / 2);
    band.x = x;
    band.y = top + shift;
}

Node layoutBand(const Document& doc, const std::vector<Paragraph>& paragraphs, std::int32_t width,
                const std::string& align, bool useStoredLineHeight) {
    Frame inner;
    inner.doc = &doc;
    inner.paragraphs = &paragraphs;
    inner.pageDef.width = static_cast<std::uint32_t>(std::max<std::int32_t>(200, width));
    inner.pageDef.height = 20000000;
    inner.forceAlign = align;
    inner.useStoredLineHeight = useStoredLineHeight;
    inner.story = takeStory(&paragraphs);
    return layoutFrame(inner);
}

void attachBand(Node& paper, const Control& control, const PageDef& page, const Document& doc, int pageNumber,
                int pageCount, bool hideNumber, const std::tm& dateNow, const std::tm& docDate,
                bool useStoredLineHeight) {
    if (control.subLists.empty()) return;
    const SubList& list = control.subLists.front();
    const std::int32_t paperW = static_cast<std::int32_t>(page.width);
    const std::int32_t paperH = static_cast<std::int32_t>(page.height);
    const std::int32_t left = static_cast<std::int32_t>(page.marginLeft);
    const std::int32_t contentW = std::max<std::int32_t>(200, paperW - left - static_cast<std::int32_t>(page.marginRight));
    const std::int32_t width = list.textWidth > 0 ? static_cast<std::int32_t>(list.textWidth) : contentW;
    const bool header = control.kind == ControlKind::Header;
    const std::int32_t margin = header ? static_cast<std::int32_t>(page.marginHeader)
                                       : static_cast<std::int32_t>(page.marginFooter);
    const std::int32_t edge = header ? static_cast<std::int32_t>(page.marginTop)
                                     : static_cast<std::int32_t>(page.marginBottom);
    std::int32_t boxH = list.textHeight > 0 ? static_cast<std::int32_t>(list.textHeight) : margin;
    if (boxH <= 0) boxH = 1000;
    std::vector<Paragraph> paragraphs;
    for (const Paragraph& para : list.paragraphs) {
        paragraphs.push_back(numberedParagraph(para, pageNumber, pageCount, hideNumber, dateNow, docDate));
    }
    const TempStoryBinding tempBinding(list.paragraphs, paragraphs);
    Node band = layoutBand(doc, paragraphs, width, "", useStoredLineHeight);
    const std::int32_t top = header ? edge : paperH - edge - boxH;
    const std::string align = list.vertAlign.empty() ? (header ? "TOP" : "BOTTOM") : list.vertAlign;
    shiftBand(band, left, top, boxH, align);
    band.pageBand = true;
    paper.children.push_back(std::move(band));
}

bool pageNumAtTop(const std::string& pos) {
    return pos.find("TOP") != std::string::npos || pos.find("Top") != std::string::npos;
}

std::string pageNumAlign(const std::string& pos) {
    if (pos.find("CENTER") != std::string::npos || pos.find("Center") != std::string::npos) return "CENTER";
    if (pos.find("RIGHT") != std::string::npos || pos.find("Right") != std::string::npos ||
        pos.find("OUTSIDE") != std::string::npos || pos.find("Outside") != std::string::npos)
        return "RIGHT";
    return "LEFT";
}

void attachPageNumber(Node& paper, const Control& control, const PageDef& page, const Document& doc, int pageNumber,
                      int pageCount) {
    if (control.pageNumPos.empty() || control.pageNumPos == "NONE" || control.pageNumPos == "None") return;
    Paragraph para;
    Run run;
    InlineItem number = InlineItem::makeText(numberText(control, pageNumber, pageCount));
    number.ignoreSpacing = true;
    run.items.push_back(std::move(number));
    para.runs.push_back(std::move(run));
    const std::int32_t paperW = static_cast<std::int32_t>(page.width);
    const std::int32_t paperH = static_cast<std::int32_t>(page.height);
    const std::int32_t left = static_cast<std::int32_t>(page.marginLeft);
    const std::int32_t width = std::max<std::int32_t>(200, paperW - left - static_cast<std::int32_t>(page.marginRight));
    std::vector<Paragraph> paragraphs;
    paragraphs.push_back(std::move(para));
    Node band = layoutBand(doc, paragraphs, width, pageNumAlign(control.pageNumPos), false);
    const bool top = pageNumAtTop(control.pageNumPos);
    const std::int32_t edge = top ? static_cast<std::int32_t>(page.marginTop)
                                  : static_cast<std::int32_t>(page.marginBottom);
    const std::int32_t bandH = top ? static_cast<std::int32_t>(page.marginHeader)
                                   : static_cast<std::int32_t>(page.marginFooter);
    const std::int32_t boxH = std::max<std::int32_t>(bandH > 0 ? bandH : 1000, rowContentBottom(band));
    const std::int32_t topY = top ? edge : paperH - edge - boxH;
    shiftBand(band, left, topY, boxH, top ? "TOP" : "BOTTOM");
    band.pageBand = true;
    paper.children.push_back(std::move(band));
}

void noteControl(const Control& control, const Control* heads[3], const Control* foots[3], const Control*& pageNum,
                 int& number, bool& hideHeader, bool& hideFooter, bool& hideMaster, bool& hidePageNum) {
    if (control.kind == ControlKind::Header) heads[applySlot(control.applyPageType)] = &control;
    else if (control.kind == ControlKind::Footer) foots[applySlot(control.applyPageType)] = &control;
    else if (control.kind == ControlKind::NewNumber && (control.numType.empty() || control.numType == "PAGE")) {
        if (control.number > 0) number = static_cast<int>(control.number);
    } else if (control.kind == ControlKind::PageHiding) {
        hideHeader = hideHeader || control.hideHeader;
        hideFooter = hideFooter || control.hideFooter;
        hideMaster = hideMaster || control.hideMaster;
        hidePageNum = hidePageNum || control.hidePageNum;
    } else if (control.kind == ControlKind::PageNumPos) {
        pageNum = &control;
    }
}

void markMasterLayer(Node& node, bool onTop) {
    node.fromMaster = true;
    node.masterOnTop = onTop;
    for (Node& child : node.children) markMasterLayer(child, onTop);
}

void attachMaster(Node& paper, const MasterPage& master, const PageDef& page, const Document& doc, int pageNumber,
                  int pageCount, bool hideNumber, const std::tm& dateNow, const std::tm& docDate,
                  bool useStoredLineHeight) {
    std::vector<Paragraph> paragraphs;
    for (const Paragraph& para : master.content.paragraphs) {
        paragraphs.push_back(numberedParagraph(para, pageNumber, pageCount, hideNumber, dateNow, docDate));
    }
    if (paragraphs.empty()) return;
    const TempStoryBinding tempBinding(master.content.paragraphs, paragraphs);
    Frame inner;
    inner.doc = &doc;
    inner.paragraphs = &paragraphs;
    inner.page = true;
    inner.pageDef = page;
    inner.useStoredLineHeight = useStoredLineHeight;
    inner.story = takeStory(&paragraphs);
    Node laid = layoutFrame(inner);
    for (Node& child : laid.children) markMasterLayer(child, master.printOnTop);
    if (master.printOnTop) {
        paper.children.insert(paper.children.end(), std::make_move_iterator(laid.children.begin()),
                              std::make_move_iterator(laid.children.end()));
    } else {
        paper.children.insert(paper.children.begin(), std::make_move_iterator(laid.children.begin()),
                              std::make_move_iterator(laid.children.end()));
    }
}

struct LineRange {
    int paragraph = -1;
    int start = 0;
    int end = 0;
};

void collectBodyLineRanges(const Node& paper, std::vector<LineRange>& out) {
    for (const Node& child : paper.children) {
        if (child.kind != NodeKind::Body) continue;
        for (const Node& column : child.children) {
            if (column.kind != NodeKind::Column) continue;
            for (const Node& para : column.children) {
                if (para.kind != NodeKind::Paragraph) continue;
                for (const Node& line : para.children) {
                    if (line.kind != NodeKind::Line || line.end <= line.start || line.paragraph < 0) continue;
                    out.push_back(LineRange{line.paragraph, line.start, line.end});
                }
            }
        }
        break;
    }
}

bool lineHolds(const std::vector<LineRange>& ranges, int paragraph, int pos) {
    for (const LineRange& range : ranges) {
        if (range.paragraph == paragraph && pos >= range.start && pos < range.end) return true;
    }
    return false;
}

// 문단의 컨트롤을 lineseg 글자 위치와 함께 걷는다. 탭은 8칸, 보조 평면 글자는 2칸이다.
template <typename Visit>
void forEachControlPos(const Paragraph& para, bool storedLines, Visit visit) {
    const bool useLines = storedLines && !para.lineSegs.empty();
    int pos = 0;
    for (const auto& run : para.runs) {
        for (const auto& item : run.items) {
            if (item.type == InlineItem::Type::Text) {
                std::size_t index = 0;
                char32_t cp = 0;
                while (nextCodePoint(item.text, index, cp)) {
                    if (useLines && cp == '\t') pos += 8;
                    else if (useLines && cp > 0xFFFF) pos += 2;
                    else pos += 1;
                }
                continue;
            }
            visit(item.control, pos);
            const bool note = item.control.kind == ControlKind::Footnote || item.control.kind == ControlKind::Endnote;
            const bool asChar = visualObject(item.control) && item.control.placement &&
                                item.control.placement->treatAsChar;
            if (useLines) pos += 8;
            else if (note || asChar) pos += 1;
        }
    }
}

void pushNote(const Control& control, std::vector<const Control*>& footnotes, std::vector<const Control*>& endnotes) {
    if (control.kind == ControlKind::Endnote) endnotes.push_back(&control);
    else if (control.kind == ControlKind::Footnote) footnotes.push_back(&control);
}

// 칸 안 표의 주는 그 표를 든 칸 줄을 따라 한꺼번에 모은다.
void collectNestedNotes(const Control& control, std::vector<const Control*>& footnotes,
                        std::vector<const Control*>& endnotes) {
    if (!control.table) return;
    for (const auto& cell : control.table->cells) {
        for (const auto& para : cell.content.paragraphs) {
            for (const auto& run : para.runs) {
                for (const auto& item : run.items) {
                    if (item.type != InlineItem::Type::Control) continue;
                    pushNote(item.control, footnotes, endnotes);
                    collectNestedNotes(item.control, footnotes, endnotes);
                }
            }
        }
    }
}

struct CellLineRange {
    int paragraph = -1;  // 표를 든 본문 문단
    int row = -1;
    int col = -1;
    LineRange line;
};

// 이 쪽에 놓인 바깥 표 칸의 줄. 표가 쪽을 넘으면 칸 줄도 그 쪽 몫만 있다.
void collectCellLineRanges(const Node& node, int paragraph, int row, int col, std::vector<CellLineRange>& out) {
    if (node.kind == NodeKind::Cell) {
        if (row >= 0) return;
        row = node.row;
        col = node.col;
    }
    if (row >= 0 && node.kind == NodeKind::Line && node.end > node.start && node.paragraph >= 0) {
        out.push_back(CellLineRange{paragraph, row, col, LineRange{node.paragraph, node.start, node.end}});
    }
    for (const Node& child : node.children) {
        int next = paragraph;
        if (row < 0 && (child.kind == NodeKind::Paragraph || child.kind == NodeKind::Object) && child.paragraph >= 0) {
            next = child.paragraph;
        }
        collectCellLineRanges(child, next, row, col, out);
    }
}

bool cellLineHolds(const std::vector<CellLineRange>& ranges, int paragraph, const TableCell& cell, int cellParagraph,
                   int pos) {
    for (const CellLineRange& range : ranges) {
        if (range.paragraph != paragraph || range.row != static_cast<int>(cell.row) ||
            range.col != static_cast<int>(cell.col) || range.line.paragraph != cellParagraph) {
            continue;
        }
        if (pos >= range.line.start && pos < range.line.end) return true;
    }
    return false;
}

void collectSectionNotes(const Node& paper, const Section& section, bool storedLines,
                         std::vector<const Control*>& footnotes, std::vector<const Control*>& endnotes) {
    std::vector<LineRange> ranges;
    collectBodyLineRanges(paper, ranges);
    std::vector<CellLineRange> cellRanges;
    for (const Node& child : paper.children) {
        if (child.kind != NodeKind::Body) continue;
        collectCellLineRanges(child, -1, -1, -1, cellRanges);
        break;
    }
    for (int pi = 0; pi < static_cast<int>(section.paragraphs.size()); ++pi) {
        const Paragraph& para = section.paragraphs[static_cast<std::size_t>(pi)];
        forEachControlPos(para, storedLines, [&](const Control& control, int pos) {
            if (lineHolds(ranges, pi, pos)) pushNote(control, footnotes, endnotes);
            if (!control.table || cellRanges.empty()) return;
            // 표 칸 안의 주는 그 칸 줄이 놓인 쪽의 주다. 쪽을 넘는 표는 칸 줄을 따라 나뉜다.
            for (const auto& cell : control.table->cells) {
                for (int cpi = 0; cpi < static_cast<int>(cell.content.paragraphs.size()); ++cpi) {
                    forEachControlPos(cell.content.paragraphs[static_cast<std::size_t>(cpi)], storedLines,
                                      [&](const Control& inner, int innerPos) {
                                          if (!cellLineHolds(cellRanges, pi, cell, cpi, innerPos)) return;
                                          pushNote(inner, footnotes, endnotes);
                                          collectNestedNotes(inner, footnotes, endnotes);
                                      });
                }
            }
        });
    }
}

void replaceNoteNumber(Paragraph& para, const std::string& label, bool endnote, bool prepend) {
    if (label.empty()) return;
    bool replaced = false;
    for (auto& run : para.runs) {
        for (auto& item : run.items) {
            if (item.type != InlineItem::Type::Control || item.control.kind != ControlKind::AutoNumber) continue;
            const bool match = endnote ? item.control.numType == "ENDNOTE" : item.control.numType == "FOOTNOTE";
            if (!match) continue;
            const std::string pen = item.markpen;
            item = InlineItem::makeText(label);
            item.markpen = pen;
            replaced = true;
        }
    }
    if (!prepend || replaced) return;
    const std::string text = para.text();
    if (text.compare(0, label.size(), label) == 0) return;
    if (para.runs.empty()) {
        Run run;
        run.items.push_back(InlineItem::makeText(label));
        para.runs.push_back(std::move(run));
        return;
    }
    para.runs.front().items.insert(para.runs.front().items.begin(), InlineItem::makeText(label));
}

struct NoteBand {
    Node node;
    std::int32_t height = 0;
};

std::int32_t noteRuleLength(std::int32_t stored, std::int32_t columnWidth) {
    if (stored == 0 || columnWidth <= 0) return 0;
    // 음수는 파일의 -1이다. 각주 기본 구분선이 단의 1/3이라 그 길이로 둔다.
    // 단보다 긴 값은 단 너비로 자른다. 미주 기본 길이도 여기에 들어간다.
    if (stored < 0) return std::max<std::int32_t>(1, columnWidth / 3);
    return std::min(stored, columnWidth);
}

NoteBand makeNoteBand(const Document& doc, const NoteShape& shape, const std::vector<const Control*>& notes,
                      std::int32_t width, bool storedLines, bool endnote, int sectionIndex, int pageParagraph,
                      bool drawSeparator = true) {
    NoteBand band;
    if (notes.empty() || width < 1) return band;
    const std::int32_t rule =
        !drawSeparator || shape.lineType.empty() || shape.lineType == "NONE" ? 0 : noteRuleLength(shape.lineLength, width);
    std::int32_t y = drawSeparator ? shape.above + shape.below : 0;
    if (y < 0) y = 0;
    if (rule > 0) {
        Node line;
        line.kind = NodeKind::Paragraph;
        line.y = std::max<std::int32_t>(0, shape.above);
        line.width = rule;
        line.height = std::max<std::int32_t>(shape.lineWidth, 1);
        line.borderTop.type = shape.lineType;
        line.borderTop.width = shape.lineWidth > 0 ? shape.lineWidth : 34;
        line.borderTop.color = shape.lineColor.empty() || shape.lineColor == "none" ? "#000000" : shape.lineColor;
        band.node.children.push_back(std::move(line));
    }
    bool any = false;
    for (const Control* note : notes) {
        if (!note || note->subLists.empty()) continue;
        std::vector<Paragraph> paragraphs = note->subLists.front().paragraphs;
        if (paragraphs.empty()) continue;
        const std::string label = noteLabel(shape, noteNumber(doc, sectionIndex, pageParagraph, *note));
        for (std::size_t i = 0; i < paragraphs.size(); ++i) {
            replaceNoteNumber(paragraphs[i], label, endnote, i == 0);
        }
        const TempStoryBinding binding(note->subLists.front().paragraphs, paragraphs);
        Node laid = layoutBand(doc, paragraphs, width, "", storedLines);
        const std::int32_t used = std::max(0, columnContentBottom(laid));
        if (used <= 0) continue;
        if (any) y += shape.between;
        laid.x = 0;
        laid.y = y;
        laid.height = used;
        y += used;
        band.node.children.push_back(std::move(laid));
        any = true;
    }
    if (!any) return {};
    band.height = y;
    band.node.kind = NodeKind::Body;
    band.node.role = endnote ? "endnote" : "footnote";
    band.node.width = width;
    band.node.height = y;
    return band;
}

void attachNoteBand(Node& paper, NoteBand band, std::int32_t x, std::int32_t y) {
    if (band.height <= 0) return;
    band.node.x = x;
    band.node.y = y;
    paper.children.push_back(std::move(band.node));
}

std::int32_t pageContentRoom(const PageDef& page);

bool paperHasRole(const Node& node, const char* role) {
    if (node.role == role) return true;
    for (const Node& child : node.children) {
        if (paperHasRole(child, role)) return true;
    }
    return false;
}

Node* pageBody(Node& paper) {
    for (Node& child : paper.children) {
        if (child.kind == NodeKind::Body) return &child;
    }
    return nullptr;
}

Node blankPaper(const PageDef& page, std::int32_t left, std::int32_t top, std::int32_t width, std::int32_t height) {
    Node paperNode;
    paperNode.kind = NodeKind::Paper;
    paperNode.width = static_cast<std::int32_t>(page.width);
    paperNode.height = static_cast<std::int32_t>(page.height);
    paperNode.marginTop = kDefaultPageMarginTopHu;
    paperNode.marginLeft = kDefaultPageMarginLeftHu;
    paperNode.marginRight = kDefaultPageMarginRightHu;
    paperNode.marginBottom = kDefaultPageMarginBottomHu;
    Node body;
    body.kind = NodeKind::Body;
    body.x = left;
    body.y = top;
    body.width = width;
    body.height = height;
    paperNode.children.push_back(std::move(body));
    return paperNode;
}

// 조각의 y는 부모 기준 시작점이다. 자식 y는 그 조각 안에서의 위치라 빈 줄을 한 번만 더한다.
Node sliceNoteBand(const NoteBand& band, std::int32_t origin, std::int32_t span, std::int32_t top) {
    Node slice = band.node;
    slice.children.clear();
    slice.x = 0;
    slice.y = top;
    std::int32_t bottom = 0;
    for (const Node& child : band.node.children) {
        const std::int32_t y0 = child.y;
        if (y0 < origin || y0 >= origin + span) continue;
        Node part = child;
        part.y = y0 - origin;
        bottom = std::max(bottom, part.y + std::max(part.height, 1));
        slice.children.push_back(std::move(part));
    }
    slice.height = std::max(bottom, 1);
    return slice;
}

std::int32_t noteSliceEnd(const NoteBand& band, std::int32_t origin, std::int32_t span) {
    std::int32_t end = origin;
    bool any = false;
    for (const Node& child : band.node.children) {
        if (child.y < origin || child.y >= origin + span) continue;
        end = std::max(end, child.y + std::max(child.height, 1));
        any = true;
    }
    return any ? end : origin;
}

void appendEndnotePages(std::vector<Node>& pages, const PageDef& page, std::int32_t left, std::int32_t top,
                        std::int32_t width, std::int32_t height, const NoteBand& band, std::int32_t origin) {
    for (int guard = 0; origin < band.height && pages.size() < 64 && guard < 64; ++guard) {
        const std::int32_t span = std::max<std::int32_t>(1, height);
        std::int32_t next = noteSliceEnd(band, origin, span);
        if (next <= origin) {
            std::int32_t jump = band.height;
            for (const Node& child : band.node.children) {
                if (child.y >= origin) jump = std::min(jump, child.y);
            }
            if (jump <= origin) break;
            origin = jump;
            continue;
        }
        Node paperNode = blankPaper(page, left, top, width, height);
        Node* held = pageBody(paperNode);
        held->children.push_back(sliceNoteBand(band, origin, next - origin, 0));
        pages.push_back(std::move(paperNode));
        origin = next;
    }
}

// 다단이면 미주도 단 폭으로 쌓는다. 마지막 글이 있는 단의 글 아래에서 시작해 다음 단, 다음 쪽 단으로 잇는다.
void placeEndnoteColumns(std::vector<Node>& pages, const Node& body, const std::vector<const Node*>& columns,
                         const PageDef& page, const NoteBand& band, bool blocked) {
    std::size_t first = 0;
    std::int32_t firstBottom = 0;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const std::int32_t bottom = columnContentBottom(*columns[i]);
        if (bottom > 0) {
            first = i;
            firstBottom = bottom;
        }
    }
    struct Slot {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
    };
    std::vector<Slot> slots;
    for (const Node* column : columns) slots.push_back({column->x, column->y, column->width, column->height});
    std::int32_t origin = 0;
    const auto fill = [&](Node& paper, std::int32_t bodyX, std::int32_t bodyY, std::size_t from, std::int32_t usedTop) {
        for (std::size_t i = from; i < slots.size() && origin < band.height; ++i) {
            const std::int32_t start = i == from ? usedTop : 0;
            const std::int32_t span = std::max<std::int32_t>(0, slots[i].height - start);
            const std::int32_t next = noteSliceEnd(band, origin, span);
            if (next <= origin) continue;
            Node slice = sliceNoteBand(band, origin, next - origin, bodyY + slots[i].y + start);
            slice.x = bodyX + slots[i].x;
            paper.children.push_back(std::move(slice));
            origin = next;
        }
    };
    if (!blocked) fill(pages.back(), body.x, body.y, first, firstBottom);
    for (int guard = 0; origin < band.height && pages.size() < 64 && guard < 64; ++guard) {
        Node paperNode = blankPaper(page, body.x, body.y, body.width, body.height);
        const std::int32_t before = origin;
        fill(paperNode, body.x, body.y, 0, 0);
        if (origin == before) break;
        pages.push_back(std::move(paperNode));
    }
}

// 각주가 쪽 아래를 예약하면 그 빈칸에는 미주를 두지 않는다. 본문이 있는 쪽에는 구분선을 긋고,
// 본문이 없는 이어진 쪽은 구분선 없이 본문 맨 위에서 미주를 시작한다.
void placeEndnoteFlow(std::vector<Node>& pages, const Document& doc, const noting::model::Section& section,
                      const PageDef& page, const std::vector<const Control*>& notes, bool storedLines,
                      int sectionIndex) {
    if (notes.empty() || pages.empty()) return;
    const std::int32_t left = static_cast<std::int32_t>(page.marginLeft);
    const std::int32_t top = static_cast<std::int32_t>(page.marginTop) + static_cast<std::int32_t>(page.marginHeader);
    const std::int32_t width = std::max<std::int32_t>(200, static_cast<std::int32_t>(page.width) - left -
                                                               static_cast<std::int32_t>(page.marginRight));
    const std::int32_t height = pageContentRoom(page);
    Node* body = pageBody(pages.back());
    const bool blocked = body && paperHasRole(pages.back(), "footnote") && !section.footnote.beneathText;
    if (body) {
        std::vector<const Node*> columns;
        for (const Node& child : body->children) {
            if (child.kind == NodeKind::Column) columns.push_back(&child);
        }
        if (columns.size() > 1) {
            const NoteBand band = makeNoteBand(doc, section.endnote, notes, columns.front()->width, storedLines, true,
                                               sectionIndex, 0, true);
            if (band.height > 0) placeEndnoteColumns(pages, *body, columns, page, band, blocked);
            return;
        }
    }
    NoteBand withRule = makeNoteBand(doc, section.endnote, notes, width, storedLines, true, sectionIndex, 0, true);
    if (withRule.height <= 0) return;
    const std::int32_t textBottom = body ? columnContentBottom(*body) : 0;
    const std::int32_t room = body && !blocked ? std::max<std::int32_t>(0, body->height - textBottom) : 0;
    std::int32_t consumed = 0;
    if (body && room > 0) {
        if (withRule.height <= room) {
            attachNoteBand(pages.back(), std::move(withRule), body->x, body->y + textBottom);
            return;
        }
        const std::int32_t fitted = noteSliceEnd(withRule, 0, room);
        if (fitted > 0) {
            Node slice = sliceNoteBand(withRule, 0, fitted, body->y + textBottom);
            slice.x = body->x;
            pages.back().children.push_back(std::move(slice));
            consumed = fitted;
        }
    }
    if (consumed >= withRule.height) return;
    if (consumed <= 0) {
        NoteBand bare = makeNoteBand(doc, section.endnote, notes, width, storedLines, true, sectionIndex, 0, false);
        if (bare.height <= 0) return;
        appendEndnotePages(pages, page, left, top, width, height, bare, 0);
        return;
    }
    appendEndnotePages(pages, page, left, top, width, height, withRule, consumed);
}

std::int32_t pageContentRoom(const PageDef& page) {
    const std::int32_t paperH = static_cast<std::int32_t>(page.height);
    const std::int32_t top = static_cast<std::int32_t>(page.marginTop) + static_cast<std::int32_t>(page.marginHeader);
    const std::int32_t bottom =
        static_cast<std::int32_t>(page.marginBottom) + static_cast<std::int32_t>(page.marginFooter);
    return std::max<std::int32_t>(200, paperH - top - bottom);
}

bool pageChromeRole(const std::string& role) { return role == "pageFill" || role == "pageBorder"; }

bool pageBorderHasInk(const BorderFill* fill) {
    if (!fill) return false;
    auto on = [](const BorderSide& side) { return !side.type.empty() && side.type != "NONE"; };
    if (on(fill->left) || on(fill->right) || on(fill->top) || on(fill->bottom)) return true;
    if (!fill->faceColor.empty() && fill->faceColor != "none") return true;
    if (!fill->gradColor.empty() && fill->gradColor != "none") return true;
    if (!fill->imageId.empty() && fill->imageMode >= 0 && fill->imageMode != 15) return true;
    return false;
}

// 짝수·홀수 칸의 번호가 0이면 양쪽을 쓴다. 번호가 있어도 선과 칠이 없으면 양쪽을 쓴다.
// 홀짝을 구분하지 않은 파일은 짝수·홀수에 빈 1번을 적고, 실제 테두리는 양쪽에 둔다.
const PageBorderFill* pageBorderForNumber(const Document& doc, const std::array<PageBorderFill, 3>& borders,
                                          int pageNumber) {
    if (pageNumber < 1) pageNumber = 1;
    const PageBorderFill& side = (pageNumber & 1) != 0 ? borders[2] : borders[1];
    const PageBorderFill& both = borders[0];
    if (side.borderFillId != 0 && pageBorderHasInk(findBorderFill(doc, side.borderFillId))) return &side;
    if (both.borderFillId != 0) return &both;
    if (side.borderFillId != 0) return &side;
    return nullptr;
}

void clearPageChrome(Node& paper) {
    paper.children.erase(std::remove_if(paper.children.begin(), paper.children.end(),
                                        [](const Node& child) { return pageChromeRole(child.role); }),
                         paper.children.end());
}

// 종이 기준은 용지 맨 왼쪽 위다. 여백은 빼지 않고, 간격만큼 안쪽으로 들어온다.
// 쪽 기준은 위(머리말 여백 포함), 왼쪽·오른쪽 여백, 아래(꼬리말 여백 포함)를 뺀 본문 칸이다.
// 그 칸의 바깥으로 간격만큼 나간다. 머리말·꼬리말 포함은 종이 기준에서만 본다.
void stampPageChrome(Node& paper, const Document& doc, const noting::model::Section& section, int pageNumber) {
    clearPageChrome(paper);
    const PageBorderFill* spec = pageBorderForNumber(doc, section.pageBorders, pageNumber);
    if (!spec) return;
    const noting::model::BorderFill* fill = findBorderFill(doc, spec->borderFillId);
    if (!fill) return;
    const PageDef page = section.page.value_or(PageDef{});
    const std::int32_t paperW = paper.width > 0 ? paper.width : static_cast<std::int32_t>(page.width);
    const std::int32_t paperH = paper.height > 0 ? paper.height : static_cast<std::int32_t>(page.height);
    const std::int32_t bodyX = static_cast<std::int32_t>(page.marginLeft);
    const std::int32_t bodyY =
        static_cast<std::int32_t>(page.marginTop) + static_cast<std::int32_t>(page.marginHeader);
    const std::int32_t bodyW = std::max<std::int32_t>(0, paperW - bodyX - static_cast<std::int32_t>(page.marginRight));
    const std::int32_t bodyH = std::max<std::int32_t>(
        0, paperH - bodyY - static_cast<std::int32_t>(page.marginBottom) - static_cast<std::int32_t>(page.marginFooter));

    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    if (spec->textBorder) {
        left = bodyX - spec->left;
        top = bodyY - spec->top;
        right = bodyX + bodyW + spec->right;
        bottom = bodyY + bodyH + spec->bottom;
    } else {
        left = spec->left;
        top = spec->top;
        right = paperW - spec->right;
        bottom = paperH - spec->bottom;
        if (spec->headerInside) top -= static_cast<std::int32_t>(page.marginHeader);
        if (spec->footerInside) bottom += static_cast<std::int32_t>(page.marginFooter);
    }

    const int area = spec->fillArea < 0 ? 0 : spec->fillArea;
    std::int32_t fillX = 0;
    std::int32_t fillY = 0;
    std::int32_t fillW = paperW;
    std::int32_t fillH = paperH;
    if (area == 1) {
        fillX = bodyX;
        fillY = bodyY;
        fillW = bodyW;
        fillH = bodyH;
    } else if (area >= 2) {
        fillX = left;
        fillY = top;
        fillW = right - left;
        fillH = bottom - top;
    }
    const bool face = !fill->faceColor.empty() && fill->faceColor != "none";
    const bool image = !fill->imageId.empty() && fill->imageMode >= 0 && fill->imageMode != 15;
    if ((face || image) && fillW > 0 && fillH > 0) {
        Node node;
        node.kind = NodeKind::Object;
        node.role = "pageFill";
        node.textWrap = "BEHIND_TEXT";
        node.x = fillX;
        node.y = fillY;
        node.width = fillW;
        node.height = fillH;
        applyBorderFill(node, doc, spec->borderFillId);
        node.borderLeft = node.borderRight = node.borderTop = node.borderBottom = {};
        paper.children.push_back(std::move(node));
    }
    auto sideOn = [](const noting::model::BorderSide& side) {
        return !side.type.empty() && side.type != "NONE";
    };
    if ((sideOn(fill->left) || sideOn(fill->right) || sideOn(fill->top) || sideOn(fill->bottom)) && right > left &&
        bottom > top) {
        Node node;
        node.kind = NodeKind::Object;
        node.role = "pageBorder";
        node.x = left;
        node.y = top;
        node.width = right - left;
        node.height = bottom - top;
        applyBorderFill(node, doc, spec->borderFillId);
        node.fillColor.clear();
        node.gradColor.clear();
        node.binItemId.clear();
        node.imageFillMode = -1;
        paper.children.push_back(std::move(node));
    }
}

struct FlowAt {
    int para = -1;
    int textPos = 0;
    bool ok = false;
};

const Node* paperBody(const Node& paper) {
    for (const Node& child : paper.children) {
        if (child.kind == NodeKind::Body) return &child;
    }
    return nullptr;
}

int bodyColumnCount(const Node& paper) {
    const Node* body = paperBody(paper);
    if (!body) return 0;
    int count = 0;
    for (const Node& child : body->children) {
        if (child.kind == NodeKind::Column) ++count;
    }
    return count;
}

FlowAt pageFlowStart(const Node& paper) {
    FlowAt at;
    const Node* body = paperBody(paper);
    if (!body) return at;
    const Node* best = nullptr;
    const auto walk = [&](const auto& self, const Node& node) -> void {
        if (node.kind == NodeKind::Line) {
            if (!best || node.paragraph < best->paragraph ||
                (node.paragraph == best->paragraph && node.start < best->start)) {
                best = &node;
            }
            return;
        }
        for (const Node& child : node.children) self(self, child);
    };
    walk(walk, *body);
    if (!best || best->paragraph < 0) return at;
    at.ok = true;
    at.para = best->paragraph;
    at.textPos = std::max(0, best->start);
    return at;
}

const Node* firstBodyLine(const Node& paper) {
    const Node* body = paperBody(paper);
    if (!body) return nullptr;
    const Node* best = nullptr;
    const auto walk = [&](const auto& self, const Node& node) -> void {
        if (node.kind == NodeKind::Line) {
            if (!best || node.paragraph < best->paragraph ||
                (node.paragraph == best->paragraph && node.start < best->start)) {
                best = &node;
            }
            return;
        }
        for (const Node& child : node.children) self(self, child);
    };
    walk(walk, *body);
    return best;
}

int bodyUsedBottom(const Node& body) {
    int bottom = 0;
    const auto walk = [&](const auto& self, const Node& node, int origin) -> void {
        if (node.kind == NodeKind::Line) {
            bottom = std::max(bottom, origin + node.y + std::max(node.height, 0));
            return;
        }
        const int next = (node.kind == NodeKind::Paragraph || node.kind == NodeKind::Column) ? origin + node.y : origin;
        for (const Node& child : node.children) self(self, child, next);
    };
    walk(walk, body, 0);
    return bottom;
}

void rewriteExactNumber(Node& node, const std::string& from, const std::string& to) {
    if (node.kind == NodeKind::Line && node.text == from) {
        node.text = to;
        for (Node& child : node.children) {
            if (child.kind == NodeKind::Span && child.text == from) child.text = to;
        }
    }
    for (Node& child : node.children) rewriteExactNumber(child, from, to);
}

std::vector<Node> layoutSection(const Document& doc, const noting::model::Section& section, int& nextNumber,
                                int pageBudget, bool useStoredLineHeight, int sectionIndex, bool deferSpans,
                                std::vector<const Control*>* pendingEndnotes, bool lastSection,
                                const ColumnResume* begin = nullptr,                                 const std::vector<FlowAt>* reuseStarts = nullptr,
                                const std::vector<Node>* reusePages = nullptr, int reuseBase = 0, int reuseEnd = 0,
                                int* reuseFrom = nullptr, int pageIndexBase = 0) {
    std::vector<Node> pages;
    HeadingState headings;
    Frame frame;
    frame.doc = &doc;
    frame.paragraphs = &section.paragraphs;
    frame.page = true;
    frame.headings = &headings;
    if (section.page) frame.pageDef = *section.page;
    if (section.defaultTab > 0) frame.defaultTab = section.defaultTab;
    if (section.columnRegionGap > 0) frame.columnRegionGap = section.columnRegionGap;
    frame.useStoredLineHeight = useStoredLineHeight;
    frame.deferSpans = deferSpans;
    frame.sectionIndex = sectionIndex;
    frame.story = takeStory(&section.paragraphs);
    struct Cut {
        Node paper;
        int begin = 0;
        int end = 0;
    };
    std::vector<Cut> cuts;
    ColumnResume resume;
    if (begin) resume = *begin;
    resume.done = section.paragraphs.empty();
    int from = resume.paragraph;
    std::size_t fromAtom = resume.atom;
    std::vector<const Control*> sectionEndnotes;
    bool placedEndnotes = false;
    const PageDef pageForRoom = section.page ? *section.page : frame.pageDef;
    const std::int32_t room = pageContentRoom(pageForRoom);
    const bool lineLog = gPageCalcLog && useStoredLineHeight;
    gPageObjOn = lineLog;
    while (!resume.done) {
        Cut cut;
        cut.begin = fromAtom == 0 ? from : from + 1;
        const int pageNo = gPageLogIndex + 1;
        const auto pageStarted = std::chrono::steady_clock::now();
        if (lineLog) resetPageObj();
        else if (gPageCalcLog) std::fprintf(stderr, "[noting] 쪽 %d 시작\n", pageNo);
        if (gPageCalcLog) layout::setFontLoadPage(pageNo);
        const ColumnResume pageStart = resume;
        std::int32_t reserve = 0;
        ColumnResume laid = resume;
        for (int attempt = 0; attempt < 4; ++attempt) {
            ColumnResume trial = pageStart;
            frame.bottomReserve = std::min(reserve, std::max<std::int32_t>(0, room - 400));
            frame.pageParagraph = pageStart.paragraph;
            cut.paper = layoutFrame(frame, trial.paragraph, trial.atom, &trial);
            std::vector<const Control*> foots;
            std::vector<const Control*> ends;
            collectSectionNotes(cut.paper, section, useStoredLineHeight, foots, ends);
            const Node* body = nullptr;
            for (const Node& child : cut.paper.children) {
                if (child.kind == NodeKind::Body) {
                    body = &child;
                    break;
                }
            }
            const std::int32_t width = body ? body->width : room;
            const std::int32_t textBottom = body ? columnContentBottom(*body) : 0;
            const std::int32_t bodyH = body ? body->height : 0;
            const NoteBand footBand = makeNoteBand(doc, section.footnote, foots, width, useStoredLineHeight, false,
                                                   sectionIndex, pageStart.paragraph);
            // 본문 바로 아래인 미주만 이 쪽에 붙인다. 나머지는 쪽 배치가 끝난 뒤 본문 아래에 잇는다.
            std::vector<const Control*> endSet;
            if (section.endnote.beneathText && trial.done) {
                endSet = sectionEndnotes;
                endSet.insert(endSet.end(), ends.begin(), ends.end());
            }
            const NoteBand endBand = makeNoteBand(doc, section.endnote, endSet, width, useStoredLineHeight, true,
                                                  sectionIndex, 0);
            int beneath = 0;
            int want = 0;
            if (section.footnote.beneathText) beneath += footBand.height;
            else want += footBand.height;
            if (section.endnote.beneathText) beneath += endBand.height;
            else want += endBand.height;
            if (beneath > 0 && textBottom + beneath > bodyH) want += beneath;
            if (want > room - 400) want = std::max(0, room - 400);
            if (want != reserve && attempt + 1 < 4) {
                reserve = want;
                continue;
            }
            laid = trial;
            if (body && (footBand.height > 0 || endBand.height > 0)) {
                const bool footFits = section.footnote.beneathText && textBottom + footBand.height <= bodyH;
                const bool endFits = section.endnote.beneathText && textBottom + endBand.height <= bodyH &&
                                     textBottom + beneath <= bodyH;
                std::int32_t flow = body->y + textBottom;
                if (footFits) {
                    attachNoteBand(cut.paper, footBand, body->x, flow);
                    flow += footBand.height;
                }
                if (endFits && endBand.height > 0) attachNoteBand(cut.paper, endBand, body->x, flow);
                std::int32_t at = body->y + body->height;
                if (!section.endnote.beneathText && endBand.height > 0) {
                    attachNoteBand(cut.paper, endBand, body->x, at);
                    at += endBand.height;
                } else if (!endFits && endBand.height > 0) {
                    attachNoteBand(cut.paper, endBand, body->x, at);
                    at += endBand.height;
                }
                if (!footFits && footBand.height > 0) attachNoteBand(cut.paper, footBand, body->x, at);
            }
            if (!endSet.empty()) placedEndnotes = true;
            sectionEndnotes.insert(sectionEndnotes.end(), ends.begin(), ends.end());
            break;
        }
        frame.bottomReserve = 0;
        resume = laid;
        if (resume.done) cut.end = static_cast<int>(section.paragraphs.size());
        else cut.end = resume.atom > 0 ? resume.paragraph + 1 : resume.paragraph;
        if (cut.end < cut.begin) cut.end = cut.begin;
        if (lineLog) {
            logLineCachePage(pageNo, cut.begin + 1, cut.end, pageCalcMs(pageStarted));
        } else if (gPageCalcLog) {
            if (cut.end > cut.begin) {
                std::fprintf(stderr, "[noting] 쪽 %d 문단 %d-%d %.1fms\n", pageNo, cut.begin + 1, cut.end,
                             pageCalcMs(pageStarted));
            } else {
                std::fprintf(stderr, "[noting] 쪽 %d %.1fms\n", pageNo, pageCalcMs(pageStarted));
            }
        }
        ++gPageLogIndex;
        cuts.push_back(std::move(cut));
        from = resume.paragraph;
        fromAtom = resume.atom;
        if (reuseFrom && *reuseFrom < 0 && reuseStarts && resume.atom == 0 && resume.textPos == 0 && !resume.done) {
            const int limit = std::min(reuseEnd, static_cast<int>(reuseStarts->size()));
            const Node* newBody = paperBody(cuts.back().paper);
            bool hardFence = false;
            if (resume.paragraph >= 0 && resume.paragraph < static_cast<int>(section.paragraphs.size())) {
                const noting::model::Paragraph& next = section.paragraphs[static_cast<std::size_t>(resume.paragraph)];
                // 쪽 나눔·단 설정 나누기는 앞에 빈칸이 있어도 올라오지 않는다.
                hardFence = next.pageBreak || next.columnBreak || next.layoutBreak || paragraphStartsPage(next, doc);
            }
            for (int j = reuseBase + 1; j < limit; ++j) {
                const FlowAt& at = (*reuseStarts)[static_cast<std::size_t>(j)];
                if (!at.ok || at.textPos != 0 || at.para != resume.paragraph) continue;
                if (!hardFence && reusePages && newBody && j < static_cast<int>(reusePages->size())) {
                    const Node* nextLine = firstBodyLine((*reusePages)[static_cast<std::size_t>(j)]);
                    const int freeRoom = newBody->height - bodyUsedBottom(*newBody);
                    if (nextLine && nextLine->height > 0 && freeRoom >= nextLine->height) continue;
                }
                *reuseFrom = j;
                break;
            }
            if (*reuseFrom >= 0) break;
        }
        // pageBudget 0 은 첫 쪽만. 그 다음 쪽은 배치하지 않는다.
        if (pageBudget >= 0 && static_cast<int>(cuts.size()) > pageBudget) break;
        if (resume.done) break;
    }
    gPageObjOn = false;
    if (gPageCalcLog) layout::setFontLoadPage(0);
    const PageDef page = section.page ? *section.page : frame.pageDef;
    const std::tm dateNow = clockNow();
    const std::tm docDate = clockFromText(doc.metadata.createdAt, dateNow);
    int numberCursor = section.pageStart > 0 ? static_cast<int>(section.pageStart) : nextNumber;
    if (numberCursor < 1) numberCursor = 1;
    const Control* heads[3] = {};
    const Control* foots[3] = {};
    const Control* pageNum = nullptr;
    const int pageCount = static_cast<int>(cuts.size());
    const auto bandStarted = std::chrono::steady_clock::now();
    if (gPageCalcLog && pageCount > 0 && !lineLog) std::fprintf(stderr, "[noting] 머리말 시작\n");
    for (std::size_t index = 0; index < cuts.size(); ++index) {
        int number = numberCursor;
        const bool firstPage = static_cast<int>(index) + pageIndexBase == 0;
        bool hideHeader = section.hideHeader || (firstPage && section.hideFirstHeader);
        bool hideFooter = section.hideFooter || (firstPage && section.hideFirstFooter);
        bool hideMaster = section.hideMaster || (firstPage && section.hideFirstMaster);
        bool hidePageNum = section.hidePageNum || (firstPage && section.hideFirstPageNum);
        const int last = std::min(cuts[index].end, static_cast<int>(section.paragraphs.size()));
        for (int pi = std::max(0, cuts[index].begin); pi < last; ++pi) {
            for (const auto& run : section.paragraphs[static_cast<std::size_t>(pi)].runs) {
                for (const auto& item : run.items) {
                    if (item.type != InlineItem::Type::Control) continue;
                    noteControl(item.control, heads, foots, pageNum, number, hideHeader, hideFooter, hideMaster,
                                hidePageNum);
                }
            }
        }
        numberCursor = number + 1;
        if (!hideHeader) {
            if (const Control* header = bandFor(heads, number)) {
                attachBand(cuts[index].paper, *header, page, doc, number, pageCount, hidePageNum, dateNow, docDate,
                           useStoredLineHeight);
            }
        }
        if (!hideFooter) {
            if (const Control* footer = bandFor(foots, number)) {
                attachBand(cuts[index].paper, *footer, page, doc, number, pageCount, hidePageNum, dateNow, docDate,
                           useStoredLineHeight);
            }
        }
        if (!hidePageNum && pageNum) {
            attachPageNumber(cuts[index].paper, *pageNum, page, doc, number, pageCount);
        }
        if (!hideMaster) {
            const MasterPage* slots[3] = {};
            for (const MasterPage& master : section.masterPages) slots[applySlot(master.applyPageType)] = &master;
            const int side = number % 2 == 0 ? 1 : 2;
            const MasterPage* master = slots[side] ? slots[side] : slots[0];
            if (master) {
                attachMaster(cuts[index].paper, *master, page, doc, number, pageCount, hidePageNum, dateNow, docDate,
                             useStoredLineHeight);
            }
        }
        cuts[index].paper.section = sectionIndex;
        cuts[index].paper.pageNumber = number;
        cuts[index].paper.pageLabel = formatNumber(number, pageNum ? pageNum->numFormat : std::string());
        cuts[index].paper.pageNumberHidden = hidePageNum;
        pages.push_back(std::move(cuts[index].paper));
    }
    if (gPageCalcLog && pageCount > 0) {
        if (lineLog) {
            std::printf("{\"머리말\":%.1f,\"쪽수\":%d}\n", pageCalcMs(bandStarted), pageCount);
            std::fflush(stdout);
        } else {
            std::fprintf(stderr, "[noting] 머리말 %.1fms\n", pageCalcMs(bandStarted));
        }
    }
    nextNumber = numberCursor;
    const bool trail = !section.endnote.beneathText && (section.endnote.place == 1 || lastSection);
    if (trail && (pageBudget < 0 || static_cast<int>(pages.size()) <= pageBudget)) {
        std::vector<const Control*> pile;
        if (lastSection && pendingEndnotes) {
            pile.insert(pile.end(), pendingEndnotes->begin(), pendingEndnotes->end());
        }
        pile.insert(pile.end(), sectionEndnotes.begin(), sectionEndnotes.end());
        placeEndnoteFlow(pages, doc, section, pageForRoom, pile, useStoredLineHeight, sectionIndex);
        if (pendingEndnotes && lastSection) pendingEndnotes->clear();
    } else if (pendingEndnotes) {
        if (placedEndnotes) {
            if (lastSection) pendingEndnotes->clear();
        } else {
            pendingEndnotes->insert(pendingEndnotes->end(), sectionEndnotes.begin(), sectionEndnotes.end());
        }
    }
    int extraNumber = numberCursor;
    for (Node& paperNode : pages) {
        if (paperNode.pageNumber <= 0) {
            paperNode.section = sectionIndex;
            paperNode.pageNumber = extraNumber++;
        }
        stampPageChrome(paperNode, doc, section, paperNode.pageNumber);
    }
    return pages;
}

bool fillDeferredLine(const Document& doc, const Node& paragraph, Node& line) {
    if (!line.spansLater) return false;
    line.spansLater = false;
    if (paragraph.section < 0 || paragraph.section >= static_cast<int>(doc.sections.size())) return false;
    const auto& paras = doc.sections[static_cast<std::size_t>(paragraph.section)].paragraphs;
    if (paragraph.paragraph < 0 || paragraph.paragraph >= static_cast<int>(paras.size())) return false;
    const Paragraph& para = paras[static_cast<std::size_t>(paragraph.paragraph)];
    const int from = std::max(0, line.start);
    const int to = line.end < 0 ? 0 : line.end;

    std::vector<Node> headings;
    std::vector<Node> objects;
    std::vector<Node> notes;
    for (Node& child : line.children) {
        if (child.noteMark) notes.push_back(std::move(child));
        else if (child.headingMark) headings.push_back(std::move(child));
        else if (child.kind == NodeKind::Object) objects.push_back(std::move(child));
    }
    line.children.clear();
    for (Node& head : headings) line.children.push_back(std::move(head));

    CharShape fallback;
    fallback.height = 1000;
    fallback.ratio.fill(100);
    std::size_t objectAt = 0;
    std::size_t noteAt = 0;
    std::string lineText;
    Node span;
    bool open = false;
    auto flush = [&] {
        if (!open) return;
        if (!span.text.empty()) line.children.push_back(std::move(span));
        span = Node{};
        open = false;
    };
    int pos = 0;
    for (const auto& run : para.runs) {
        const CharShape* shape = findCharShape(doc, run.charShapeId);
        if (!shape) shape = &fallback;
        const int fontSize = shape->height > 0 ? shape->height : 1000;
        for (const auto& item : run.items) {
            if (item.type == InlineItem::Type::Text) {
                std::size_t index = 0;
                char32_t cp = 0;
                while (nextCodePoint(item.text, index, cp)) {
                    const int units = cp == '\t' ? 8 : (cp > 0xFFFF ? 2 : 1);
                    const int at = pos;
                    pos += units;
                    if (cp == '\n' || at < from || at >= to) continue;
                    if (cp == '\t' || cp == ' ' || cp == 0x2007) {
                        flush();
                        span.kind = NodeKind::Span;
                        span.charShapeId = run.charShapeId;
                        const GlyphFace gapFace = glyphFace(doc, *shape, cp, false);
                        span.font = gapFace.font;
                        span.fontSize = fontSize;
                        span.ratio = gapFace.ratio > 0 ? gapFace.ratio : 100;
                        span.relSize = gapFace.relSize > 0 ? gapFace.relSize : 100;
                        span.spacing = item.ignoreSpacing ? 0 : gapFace.spacing;
                        const Lang gapLang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
                        const auto gapSlot = static_cast<std::size_t>(gapLang);
                        span.charOffset = gapSlot < shape->offset.size() ? shape->offset[gapSlot] : 0;
                        span.markpen = item.markpen;
                        paintCharStyle(span, doc, run.charShapeId);
                        span.text = cp == '\t' ? "\t" : (cp == ' ' ? " " : "\u2007");
                        open = true;
                        flush();
                        appendUtf8(lineText, cp);
                        continue;
                    }
                    const bool probe = (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) ||
                                       (cp >= 0x100000 && cp <= 0x10FFFD);
                    const GlyphFace drawn = glyphFace(doc, *shape, cp, probe);
                    const Lang lang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
                    const auto slot = static_cast<std::size_t>(lang);
                    const std::int32_t offset = slot < shape->offset.size() ? shape->offset[slot] : 0;
                    const std::int32_t spacing = item.ignoreSpacing ? 0 : drawn.spacing;
                    const bool same = open && span.charShapeId == run.charShapeId && span.font == drawn.font &&
                                      span.fontSize == fontSize && span.ratio == drawn.ratio &&
                                      span.relSize == drawn.relSize && span.spacing == spacing &&
                                      span.charOffset == offset && span.markpen == item.markpen;
                    if (!same) {
                        flush();
                        span.kind = NodeKind::Span;
                        span.charShapeId = run.charShapeId;
                        span.font = drawn.font;
                        span.fontSize = fontSize;
                        span.ratio = drawn.ratio > 0 ? drawn.ratio : 100;
                        span.relSize = drawn.relSize > 0 ? drawn.relSize : 100;
                        span.spacing = spacing;
                        span.charOffset = offset;
                        span.markpen = item.markpen;
                        paintCharStyle(span, doc, run.charShapeId);
                        open = true;
                    }
                    if (cp == '\t') span.text = "\t";
                    else appendUtf8(span.text, cp);
                    appendUtf8(lineText, cp);
                }
                continue;
            }
            const int at = pos;
            pos += 8;
            if (at < from || at >= to) continue;
            if (item.control.kind == ControlKind::Footnote || item.control.kind == ControlKind::Endnote) {
                flush();
                if (noteAt < notes.size()) {
                    lineText += notes[noteAt].text;
                    line.children.push_back(std::move(notes[noteAt++]));
                } else {
                    Atom marker;
                    marker.charShapeId = run.charShapeId;
                    fillNoteAtom(marker, doc, item.control, *shape, fontSize, paragraph.section, 0, false);
                    std::int32_t unused = 0;
                    placeOccupyingNote(line, marker, doc, unused, lineText);
                }
                continue;
            }
            if (visualObject(item.control) && item.control.placement && item.control.placement->treatAsChar) {
                flush();
                if (objectAt < objects.size()) {
                    Node obj = std::move(objects[objectAt++]);
                    obj.charShapeId = run.charShapeId;
                    obj.fontSize = fontSize;
                    line.children.push_back(std::move(obj));
                }
            }
        }
    }
    flush();
    for (; noteAt < notes.size(); ++noteAt) line.children.push_back(std::move(notes[noteAt]));
    for (; objectAt < objects.size(); ++objectAt) line.children.push_back(std::move(objects[objectAt]));
    line.text = std::move(lineText);
    line.deferTabs = true;
    // 배치 때는 글 칸이 없어 수식을 줄 맨 위에 둔다. 글자를 채운 뒤에 기준선으로 다시 앉힌다.
    const std::int32_t keptHeight = line.height;
    const std::int32_t keptMargin = line.marginTop;
    placeInlineObjects(line, SpacingKind::Fixed, keptHeight > 0 ? keptHeight : 1, true);
    if (line.height != keptHeight) line.height = keptHeight;
    const std::int32_t drop = line.marginTop - keptMargin;
    if (drop > 0) line.auditBelow += drop;
    for (const Node& child : line.children) {
        if (child.kind != NodeKind::Object || (!child.treatAsChar && !child.headingMark)) continue;
        const std::int32_t top = child.headingMark ? child.y : child.y + child.marginTop;
        const std::int32_t bottom = top + std::max(child.height, 0);
        if (top < 0) line.auditRise = std::max(line.auditRise, -top);
        if (bottom > line.height) line.auditBelow = std::max(line.auditBelow, bottom - line.height);
        for (const Node& cap : child.children) {
            if (cap.role != "caption") continue;
            const std::int32_t capTop = top + cap.y;
            const std::int32_t capBottom = capTop + std::max(cap.height, 0);
            if (capTop < 0) line.auditRise = std::max(line.auditRise, -capTop);
            if (capBottom > line.height) line.auditBelow = std::max(line.auditBelow, capBottom - line.height);
        }
    }
    return true;
}

int fillDeferredPage(const Document& doc, Node& page) {
    if (!page.spansLater) return 0;
    int filled = 0;
    const auto visit = [&](auto&& self, Node& node, Node* paragraph) -> void {
        Node* current = node.kind == NodeKind::Paragraph ? &node : paragraph;
        if (node.kind == NodeKind::Line && fillDeferredLine(doc, current ? *current : node, node)) ++filled;
        for (Node& child : node.children) self(self, child, current);
    };
    visit(visit, page, nullptr);
    page.spansLater = false;
    return filled;
}

bool pageHasBodyLine(const Node& paper) {
    const Node* body = paperBody(paper);
    if (!body) return false;
    const auto walk = [&](const auto& self, const Node& node) -> bool {
        if (node.kind == NodeKind::Line) return true;
        for (const Node& child : node.children) {
            if (self(self, child)) return true;
        }
        return false;
    };
    return walk(walk, *body);
}

Node* firstColumn(Node& paper) {
    Node* body = nullptr;
    for (Node& child : paper.children) {
        if (child.kind == NodeKind::Body) {
            body = &child;
            break;
        }
    }
    if (!body) return nullptr;
    for (Node& child : body->children) {
        if (child.kind == NodeKind::Column) return &child;
    }
    return nullptr;
}

// 글자처럼 넣은 그림의 줄 높이만 바꾸고, 그 다음 줄 노드를 쪽 사이에 다시 놓는다.
// 줄이면 다음 쪽의 줄이 빈 자리로 돌아오고, 커지면 넘친 줄이 다음 쪽으로 간다.
bool repackTreatAsCharPicture(const Document& doc, std::vector<Node>& pages, int startPage, const std::string& bin, int newW,
                              int newH) {
    if (bin.empty() || newW <= 0 || newH <= 0) return false;
    if (startPage < 0 || startPage >= static_cast<int>(pages.size())) return false;
    const int sectionIndex = pages[static_cast<std::size_t>(startPage)].section;
    int sectionEnd = startPage;
    while (sectionEnd < static_cast<int>(pages.size()) &&
           pages[static_cast<std::size_t>(sectionEnd)].section == sectionIndex) {
        ++sectionEnd;
    }
    Node* picture = nullptr;
    int picturePage = -1;
    Node* pictureLine = nullptr;
    Node* picturePara = nullptr;
    Node* pictureColumn = nullptr;
    const auto locate = [&](const auto& self, Node& node, int page, Node* column, Node* paragraph, Node* line) -> bool {
        if (node.kind == NodeKind::Object && node.treatAsChar && node.role == "picture" && node.binItemId == bin) {
            picture = &node;
            picturePage = page;
            pictureLine = line;
            picturePara = paragraph;
            pictureColumn = column;
            return true;
        }
        Node* nextColumn = node.kind == NodeKind::Column ? &node : column;
        Node* nextPara = node.kind == NodeKind::Paragraph ? &node : paragraph;
        Node* nextLine = node.kind == NodeKind::Line ? &node : line;
        for (Node& child : node.children) {
            if (self(self, child, page, nextColumn, nextPara, nextLine)) return true;
        }
        return false;
    };
    for (int page = startPage; page < sectionEnd; ++page) {
        if (bodyColumnCount(pages[static_cast<std::size_t>(page)]) != 1) return false;
        if (locate(locate, pages[static_cast<std::size_t>(page)], page, nullptr, nullptr, nullptr)) break;
    }
    if (!picture || !pictureLine || !picturePara || !pictureColumn || picturePage < 0) return false;

    const int oldBox = lineCharBox(*pictureLine);
    const int extra = std::max(0, pictureLine->height - oldBox);
    picture->width = newW;
    picture->height = newH;
    placeInlineObjects(*pictureLine, SpacingKind::Between, extra);
    const int fitted = std::max(pictureLine->textHeight, lineCharBox(*pictureLine));
    if (pictureLine->height < fitted) pictureLine->height = fitted;

    struct Carried {
        int paraId = -1;
        int section = -1;
        int story = -1;
        Node shell;
        Node line;
    };
    std::vector<Carried> carried;
    auto flowLimit = [&](int pageIndex, const Node* col) -> int {
        if (pageIndex >= 0 && pageIndex < static_cast<int>(pages.size())) {
            if (const Node* body = paperBody(pages[static_cast<std::size_t>(pageIndex)])) {
                if (body->height > 0) return body->height - (col ? col->y : 0);
            }
        }
        return 0;
    };
    const int pictureTop = picturePara->y + pictureLine->y;
    const int picturePitch = pictureLine->height;
    const int pictureBox = std::max(pictureLine->textHeight, lineCharBox(*pictureLine));
    const int pictureParaId = picturePara->paragraph;
    const int pictureStory = picturePara->story;
    const int pictureLimit = flowLimit(picturePage, pictureColumn);
    // 본문 높이를 모르면 빈 자리에 다 들어간 것으로 두면 안 된다. 쪽 배치로 넘긴다.
    if (pictureLimit <= 0) return false;
    bool lineBefore = false;
    bool seenPicture = false;
    bool earlierInPara = false;
    for (const Node& para : pictureColumn->children) {
        if (para.kind != NodeKind::Paragraph) continue;
        for (const Node& line : para.children) {
            if (line.kind != NodeKind::Line) continue;
            if (&line == pictureLine) {
                seenPicture = true;
                break;
            }
            lineBefore = true;
            if (&para == picturePara) earlierInPara = true;
        }
        if (seenPicture) break;
    }
    // 위에 줄이 있는데 그림 줄의 글자 높이가 본문 끝 이상이면, 그 줄부터 다음 쪽이다.
    const bool moveForward = lineBefore && pictureTop + pictureBox >= pictureLimit;
    bool pullBack = false;
    int pullY = 0;
    int pullParaChild = -1;
    Node* pullColumn = nullptr;
    if (!lineBefore && !moveForward && picturePage > 0 &&
        pages[static_cast<std::size_t>(picturePage - 1)].section == sectionIndex &&
        bodyColumnCount(pages[static_cast<std::size_t>(picturePage - 1)]) == 1) {
        bool startsPage = false;
        if (pictureStory > 0 && pictureParaId >= 0) {
            if (const model::Paragraph* modelPara = storyParagraph(doc, pictureStory, pictureParaId)) {
                startsPage = paragraphStartsPage(*modelPara, doc) || modelPara->columnBreak || modelPara->layoutBreak;
            }
        }
        Node* prevCol = firstColumn(pages[static_cast<std::size_t>(picturePage - 1)]);
        const int prevLimit = flowLimit(picturePage - 1, prevCol);
        if (!startsPage && prevCol && prevLimit > 0) {
            int prevBottom = 0;
            int lastPara = -1;
            for (int i = 0; i < static_cast<int>(prevCol->children.size()); ++i) {
                const Node& para = prevCol->children[static_cast<std::size_t>(i)];
                if (para.kind != NodeKind::Paragraph) continue;
                lastPara = i;
                prevBottom = std::max(prevBottom, para.y + para.height);
                for (const Node& line : para.children) {
                    if (line.kind != NodeKind::Line) continue;
                    prevBottom = std::max(prevBottom, para.y + line.y + line.height);
                }
            }
            if (prevBottom + pictureBox < prevLimit) {
                pullBack = true;
                pullY = prevBottom;
                pullColumn = prevCol;
                if (lastPara >= 0 &&
                    prevCol->children[static_cast<std::size_t>(lastPara)].paragraph == pictureParaId) {
                    pullParaChild = lastPara;
                }
            }
        }
    }
    const bool takePicture = moveForward || pullBack;
    bool after = false;
    for (int page = picturePage; page < sectionEnd; ++page) {
        Node* column = firstColumn(pages[static_cast<std::size_t>(page)]);
        if (!column) return false;
        for (Node& para : column->children) {
            if (para.kind != NodeKind::Paragraph) continue;
            std::vector<int> drop;
            for (int li = 0; li < static_cast<int>(para.children.size()); ++li) {
                Node& line = para.children[static_cast<std::size_t>(li)];
                if (line.kind != NodeKind::Line) continue;
                if (&line == pictureLine) {
                    after = true;
                    if (!takePicture) continue;
                } else if (!after) {
                    continue;
                }
                Carried item;
                item.paraId = para.paragraph;
                item.section = para.section;
                item.story = para.story;
                item.shell = para;
                item.shell.children.clear();
                item.line = std::move(line);
                carried.push_back(std::move(item));
                drop.push_back(li);
            }
            for (auto it = drop.rbegin(); it != drop.rend(); ++it) {
                para.children.erase(para.children.begin() + *it);
            }
        }
    }
    if (!takePicture || earlierInPara) {
        int bottom = 0;
        bool anyLine = false;
        for (const Node& line : picturePara->children) {
            if (line.kind != NodeKind::Line) continue;
            anyLine = true;
            bottom = std::max(bottom, line.y + line.height);
        }
        if (anyLine) picturePara->height = bottom;
    }
    for (int page = picturePage; page < sectionEnd; ++page) {
        Node* column = firstColumn(pages[static_cast<std::size_t>(page)]);
        if (!column) continue;
        column->children.erase(std::remove_if(column->children.begin(), column->children.end(),
                                              [](const Node& child) {
                                                  if (child.kind != NodeKind::Paragraph) return false;
                                                  for (const Node& line : child.children) {
                                                      if (line.kind == NodeKind::Line) return false;
                                                  }
                                                  return true;
                                              }),
                               column->children.end());
    }

    int paraChild = -1;
    int y = pictureTop + picturePitch;
    int page = picturePage;
    Node* column = pictureColumn;
    if (pullBack) {
        page = picturePage - 1;
        y = pullY;
        column = pullColumn;
        paraChild = pullParaChild;
    } else if (!takePicture) {
        for (int i = 0; i < static_cast<int>(pictureColumn->children.size()); ++i) {
            const Node& para = pictureColumn->children[static_cast<std::size_t>(i)];
            if (para.kind != NodeKind::Paragraph || para.paragraph != pictureParaId) continue;
            for (const Node& line : para.children) {
                if (line.kind != NodeKind::Line) continue;
                for (const Node& child : line.children) {
                    if (child.kind == NodeKind::Object && child.binItemId == bin) paraChild = i;
                }
            }
        }
        if (paraChild < 0) return false;
    } else {
        paraChild = -1;
        y = pictureTop;
    }
    const int oldSectionPages = sectionEnd - picturePage;

    for (Carried& item : carried) {
        const int need = std::max(item.line.textHeight, lineCharBox(item.line));
        const int limit = flowLimit(page, column);
        const bool newPara =
            paraChild < 0 || column->children[static_cast<std::size_t>(paraChild)].paragraph != item.paraId;
        bool pageBreak = false;
        if (newPara && item.story > 0 && item.paraId >= 0) {
            if (const model::Paragraph* modelPara = storyParagraph(doc, item.story, item.paraId)) {
                pageBreak = paragraphStartsPage(*modelPara, doc) || modelPara->columnBreak || modelPara->layoutBreak;
            }
        }
        const bool emptyColumn = column->children.empty();
        // 쪽 나눔은 줄 시작에 글자 높이를 더한 값이 본문 끝 이상일 때다. 빈 쪽은 그 줄을 받는다.
        const bool fits = emptyColumn || (!pageBreak && limit > 0 && y + need < limit);
        if (!fits) {
            ++page;
            if (page >= static_cast<int>(pages.size()) ||
                pages[static_cast<std::size_t>(page)].section != sectionIndex) {
                Node created = pages[static_cast<std::size_t>(page - 1)];
                if (Node* body = nullptr; true) {
                    for (Node& child : created.children) {
                        if (child.kind != NodeKind::Body) continue;
                        body = &child;
                        break;
                    }
                    if (body) {
                        for (Node& child : body->children) {
                            if (child.kind == NodeKind::Column) child.children.clear();
                        }
                    }
                }
                const int number = pages[static_cast<std::size_t>(page - 1)].pageNumber;
                created.pageNumber = number > 0 ? number + 1 : page + 1;
                pages.insert(pages.begin() + page, std::move(created));
                ++sectionEnd;
            }
            column = firstColumn(pages[static_cast<std::size_t>(page)]);
            if (!column) return false;
            paraChild = -1;
            y = 0;
        }
        if (paraChild < 0 || column->children[static_cast<std::size_t>(paraChild)].paragraph != item.paraId) {
            Node para = item.shell;
            para.children.clear();
            para.y = y;
            para.height = 0;
            para.paragraph = item.paraId;
            para.section = item.section;
            para.story = item.story;
            column->children.push_back(std::move(para));
            paraChild = static_cast<int>(column->children.size()) - 1;
        }
        Node& para = column->children[static_cast<std::size_t>(paraChild)];
        item.line.y = y - para.y;
        const int height = item.line.height;
        para.children.push_back(std::move(item.line));
        para.height = std::max(para.height, item.line.y + height);
        y += height;
    }

    const int originPicture = picturePage;
    const int originNumber = pages[static_cast<std::size_t>(originPicture)].pageNumber > 0
                                 ? pages[static_cast<std::size_t>(originPicture)].pageNumber
                                 : originPicture + 1;
    for (int i = sectionEnd - 1; i >= originPicture; --i) {
        if (!pageHasBodyLine(pages[static_cast<std::size_t>(i)]) && pages.size() > 1) {
            pages.erase(pages.begin() + i);
            --sectionEnd;
        }
    }
    const int baseNumber = originNumber;
    int cursor = picturePage;
    while (cursor < static_cast<int>(pages.size()) &&
           pages[static_cast<std::size_t>(cursor)].section == sectionIndex) {
        Node& paper = pages[static_cast<std::size_t>(cursor)];
        const int want = baseNumber + (cursor - picturePage);
        if (paper.pageNumber > 0 && paper.pageNumber != want) {
            const std::string from = std::to_string(paper.pageNumber);
            const std::string to = std::to_string(want);
            for (Node& child : paper.children) {
                if (!child.pageBand && !child.fromMaster) continue;
                rewriteExactNumber(child, from, to);
            }
        }
        paper.pageNumber = want;
        paper.huBoundsReady = false;
        ++cursor;
    }
    const int newSectionPages = cursor - picturePage;
    const int delta = newSectionPages - oldSectionPages;
    if (delta != 0) {
        for (int i = cursor; i < static_cast<int>(pages.size()); ++i) {
            Node& paper = pages[static_cast<std::size_t>(i)];
            if (paper.pageNumber <= 0) continue;
            const std::string from = std::to_string(paper.pageNumber);
            paper.pageNumber += delta;
            const std::string to = std::to_string(paper.pageNumber);
            for (Node& child : paper.children) {
                if (!child.pageBand && !child.fromMaster) continue;
                rewriteExactNumber(child, from, to);
            }
        }
    }
    for (int i = picturePage; i < static_cast<int>(pages.size()); ++i) {
        pages[static_cast<std::size_t>(i)].huBoundsReady = false;
        pages[static_cast<std::size_t>(i)].contentHash = 0;
    }
    return true;
}

}  // namespace detail

void setPageCalcLog(bool on) { gPageCalcLog = on; }
void setMeasuredLineHeight(bool on) { gMeasuredLineHeight = on; }

std::recursive_mutex& pageFillMutex() {
    static std::recursive_mutex mu;
    return mu;
}

PageFillGuard::PageFillGuard() { pageFillMutex().lock(); }

PageFillGuard::~PageFillGuard() { pageFillMutex().unlock(); }

int ensurePageSpans(const model::Document& doc, Node& page) {
    if (!page.spansLater && page.huBoundsReady && page.contentHash != 0) return 0;
    PageFillGuard guard;
    if (!page.spansLater && page.huBoundsReady && page.contentHash != 0) return 0;
    const bool wasSpansLater = page.spansLater;
    const int filled = fillDeferredPage(doc, page);
    if (wasSpansLater || filled > 0 || !page.huBoundsReady || page.contentHash == 0) {
        updatePageHuBounds(page, &doc);
    }
    return filled;
}

const model::StoryMap& ensureStoryMap(const model::Document& doc) {
    if (!gStoryMap.matches(doc)) gStoryMap.build(doc);
    return gStoryMap;
}

model::StoryMap& ensureStoryMap(model::Document& doc) {
    if (!gStoryMap.matches(doc)) gStoryMap.build(doc);
    return gStoryMap;
}

void rebuildStoryMap(const model::Document& doc) {
    gStoryMap.build(doc);
}

const model::StorySlot* resolveStory(const model::Document& doc, int story) {
    return ensureStoryMap(doc).slot(story);
}

model::StorySlot* resolveStory(model::Document& doc, int story) {
    return ensureStoryMap(doc).slot(story);
}

const std::vector<model::Paragraph>* storyParagraphs(const model::Document& doc, int story) {
    return ensureStoryMap(doc).paragraphs(story);
}

std::vector<model::Paragraph>* storyParagraphs(model::Document& doc, int story) {
    return ensureStoryMap(doc).paragraphs(story);
}

const model::Paragraph* storyParagraph(const model::Document& doc, int story, int paragraphIndex) {
    return ensureStoryMap(doc).paragraph(story, paragraphIndex);
}

model::Paragraph* storyParagraph(model::Document& doc, int story, int paragraphIndex) {
    return ensureStoryMap(doc).paragraph(story, paragraphIndex);
}

model::CursorTarget resolveCursor(model::Document& doc, model::CursorPos pos) {
    return ensureStoryMap(doc).resolve(pos);
}

std::vector<Node> layoutPages(const Document& doc, int onlyPage, bool useStoredLineHeight, bool deferSpans) {
    // 새 트리는 옛 노드 주소를 다시 쓸 수 있다. 주소로 찾는 캐럿·블록 캐시를 먼저 비운다.
    forgetLayoutNodeCaches();
    // 줄 캐시는 글자 폭을 재지 않는다. 문서 글꼴을 전부 미리 열지 않고, 그 쪽을 그릴 때 연다.
    // 배치는 문단 폭을 워커가 재고, 메인 스레드가 문단 순서대로 받은 뒤 쪽을 쌓는다.
    struct CalcLog {
        bool line = false;
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        explicit CalcLog(bool lineCache) : line(lineCache) {
            if (gPageCalcLog && !line) std::fprintf(stderr, "[noting] 페이지 계산 시작\n");
        }
        ~CalcLog() {
            if (!gPageCalcLog) return;
            if (line) {
                std::printf("{\"페이지계산\":%.1f}\n", pageCalcMs(started));
                std::fflush(stdout);
            } else {
                std::fprintf(stderr, "[noting] 페이지 계산 %.1fms\n", pageCalcMs(started));
            }
        }
    } calcLog(useStoredLineHeight);
    struct ParagraphGlyphs {
        bool on = false;
        ParagraphGlyphs(const Document& document, bool enabled) {
            if (!enabled) return;
            on = true;
            layout::prepareParagraphGlyphs(document);
        }
        ~ParagraphGlyphs() {
            if (on) layout::finishParagraphGlyphs();
        }
        ParagraphGlyphs(const ParagraphGlyphs&) = delete;
        ParagraphGlyphs& operator=(const ParagraphGlyphs&) = delete;
    };
    const auto glyphStarted = std::chrono::steady_clock::now();
    if (gPageCalcLog && !useStoredLineHeight) std::fprintf(stderr, "[noting] 글자 폭 시작\n");
    ParagraphGlyphs glyphs(doc, !useStoredLineHeight);
    if (gPageCalcLog && !useStoredLineHeight) {
        std::fprintf(stderr, "[noting] 글자 폭 %.1fms\n", pageCalcMs(glyphStarted));
    }
    std::vector<Node> pages;
    resetStories(doc);
    int nextNumber = doc.header.beginNumbers.page > 0 ? static_cast<int>(doc.header.beginNumbers.page) : 1;
    gPageLogIndex = 0;
    const auto stackStarted = std::chrono::steady_clock::now();
    if (gPageCalcLog && !useStoredLineHeight) std::fprintf(stderr, "[noting] 쪽 쌓기 시작\n");
    auto logStack = [&]() {
        if (!gPageCalcLog) return;
        if (useStoredLineHeight) {
            std::printf("{\"쪽쌓기\":%.1f,\"쪽수\":%d}\n", pageCalcMs(stackStarted), static_cast<int>(pages.size()));
            std::fflush(stdout);
        } else {
            std::fprintf(stderr, "[noting] 쪽 쌓기 %.1fms\n", pageCalcMs(stackStarted));
        }
    };
    int sectionIndex = 0;
    std::vector<const Control*> pendingEndnotes;
    for (const auto& section : doc.sections) {
        const int budget = onlyPage < 0 ? -1 : onlyPage - static_cast<int>(pages.size());
        if (onlyPage >= 0 && budget < 0) break;
        const bool lastSection = sectionIndex + 1 == static_cast<int>(doc.sections.size());
        auto sectionPages = layoutSection(doc, section, nextNumber, budget, useStoredLineHeight, sectionIndex,
                                          deferSpans, &pendingEndnotes, lastSection);
        if (deferSpans) {
            for (Node& page : sectionPages) page.spansLater = true;
        }
        ++sectionIndex;
        pages.insert(pages.end(), std::make_move_iterator(sectionPages.begin()),
                     std::make_move_iterator(sectionPages.end()));
        if (onlyPage >= 0 && static_cast<int>(pages.size()) > onlyPage) {
            logStack();
            Node page = std::move(pages[static_cast<std::size_t>(onlyPage)]);
            page.marginTop = defaultPageMarginTopHu(static_cast<std::size_t>(onlyPage));
            return {std::move(page)};
        }
    }
    applyDefaultPageMargins(pages);
    logStack();
    if (onlyPage >= 0) {
        if (onlyPage < static_cast<int>(pages.size())) {
            Node page = std::move(pages[static_cast<std::size_t>(onlyPage)]);
            return {std::move(page)};
        }
        return {};
    }
    return pages;
}

namespace {

bool bodyHasParagraph(const Node& paper, int paragraph) {
    const Node* body = paperBody(paper);
    if (!body || paragraph < 0) return false;
    bool found = false;
    const auto walk = [&](const auto& self, const Node& node) -> void {
        if (found) return;
        if (node.kind == NodeKind::Line && node.paragraph == paragraph) {
            found = true;
            return;
        }
        for (const Node& child : node.children) self(self, child);
    };
    walk(walk, *body);
    return found;
}

int rewindToParagraphStart(const std::vector<Node>& pages, int page) {
    if (page < 0 || page >= static_cast<int>(pages.size())) return -1;
    const int section = pages[static_cast<std::size_t>(page)].section;
    for (int i = page; i >= 0; --i) {
        if (pages[static_cast<std::size_t>(i)].section != section) break;
        const FlowAt at = pageFlowStart(pages[static_cast<std::size_t>(i)]);
        if (at.ok && at.textPos == 0) return i;
    }
    return -1;
}

struct PageBox {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t marginLeft = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginBottom = 0;
};

bool samePageBox(const PageBox& box, const Node& page) {
    return box.width == page.width && box.height == page.height && box.marginLeft == page.marginLeft &&
           box.marginRight == page.marginRight && box.marginTop == page.marginTop &&
           box.marginBottom == page.marginBottom;
}

}  // namespace

SectionReflow reflowPagesFrom(const model::Document& doc, std::vector<Node>& pages, int startPage,
                              bool useStoredLineHeight, bool deferSpans) {
    SectionReflow out;
    startPage = rewindToParagraphStart(pages, startPage);
    if (startPage < 0) return out;
    const int sectionIndex = pages[static_cast<std::size_t>(startPage)].section;
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(doc.sections.size())) return out;
    const FlowAt start = pageFlowStart(pages[static_cast<std::size_t>(startPage)]);
    if (!start.ok || start.textPos != 0) return out;
    int sectionEnd = startPage + 1;
    while (sectionEnd < static_cast<int>(pages.size()) &&
           pages[static_cast<std::size_t>(sectionEnd)].section == sectionIndex) {
        ++sectionEnd;
    }
    std::vector<PageBox> oldBoxes(pages.size());
    for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
        const Node& page = pages[static_cast<std::size_t>(i)];
        oldBoxes[static_cast<std::size_t>(i)] =
            PageBox{page.width, page.height, page.marginLeft, page.marginRight, page.marginTop, page.marginBottom};
    }
    std::vector<FlowAt> starts(pages.size());
    for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
        starts[static_cast<std::size_t>(i)] = pageFlowStart(pages[static_cast<std::size_t>(i)]);
    }
    const noting::model::Section& section = doc.sections[static_cast<std::size_t>(sectionIndex)];
    int nextNumber = pages[static_cast<std::size_t>(startPage)].pageNumber > 0
                         ? pages[static_cast<std::size_t>(startPage)].pageNumber
                         : startPage + 1;
    ColumnResume begin;
    begin.paragraph = start.para;
    begin.atom = 0;
    begin.textPos = 0;
    begin.done = false;
    int reuseFrom = -1;
    gPageLogIndex = startPage;
    const bool lastSection = sectionIndex + 1 == static_cast<int>(doc.sections.size());
    std::vector<const Control*> pendingEndnotes;
    std::vector<Node> laid =
        layoutSection(doc, section, nextNumber, -1, useStoredLineHeight, sectionIndex, deferSpans, &pendingEndnotes,
                      lastSection, &begin, &starts, &pages, startPage, sectionEnd, &reuseFrom, startPage);
    if (deferSpans) {
        for (Node& page : laid) page.spansLater = true;
    }
    const int oldTail = sectionEnd - startPage;
    const int delta = reuseFrom >= 0 ? (startPage + static_cast<int>(laid.size())) - reuseFrom
                                     : static_cast<int>(laid.size()) - oldTail;
    std::vector<Node> result;
    result.reserve(static_cast<std::size_t>(startPage + laid.size() + pages.size()));
    for (int i = 0; i < startPage; ++i) result.push_back(std::move(pages[static_cast<std::size_t>(i)]));
    for (Node& page : laid) result.push_back(std::move(page));
    int dirtyEnd = startPage + static_cast<int>(result.size() - static_cast<std::size_t>(startPage));
    const int tailFrom = reuseFrom >= 0 ? reuseFrom : sectionEnd;
    int prevSection = sectionIndex;
    bool freezeNumber = false;
    for (int i = tailFrom; i < static_cast<int>(pages.size()); ++i) {
        Node page = std::move(pages[static_cast<std::size_t>(i)]);
        if (page.section != prevSection) {
            prevSection = page.section;
            if (page.section >= 0 && page.section < static_cast<int>(doc.sections.size()) &&
                doc.sections[static_cast<std::size_t>(page.section)].pageStart > 0) {
                freezeNumber = true;
            }
        }
        if (!freezeNumber && delta != 0 && page.pageNumber > 0) {
            // 쪽 나눔 뒤에서 그대로 붙인 쪽이다. 번호 글자만 고치고 본문 캐시는 비우지 않는다.
            const std::string from = std::to_string(page.pageNumber);
            page.pageNumber += delta;
            const std::string to = std::to_string(page.pageNumber);
            for (Node& child : page.children) {
                if (!child.pageBand && !child.fromMaster) continue;
                rewriteExactNumber(child, from, to);
            }
        }
        result.push_back(std::move(page));
    }
    applyDefaultPageMargins(result);
    bool pageListChanged = static_cast<int>(result.size()) != static_cast<int>(oldBoxes.size()) || delta != 0;
    if (!pageListChanged) {
        const int limit = std::min(static_cast<int>(result.size()), static_cast<int>(oldBoxes.size()));
        for (int i = startPage; i < limit; ++i) {
            if (!samePageBox(oldBoxes[static_cast<std::size_t>(i)], result[static_cast<std::size_t>(i)])) {
                pageListChanged = true;
                break;
            }
        }
    }
    pages = std::move(result);
    out.ok = true;
    out.pageListChanged = pageListChanged;
    out.dirtyBegin = startPage;
    out.dirtyEnd = std::clamp(dirtyEnd, startPage, static_cast<int>(pages.size()));
    out.sectionIndex = sectionIndex;
    out.fromParagraph = start.para;
    out.laidPages = static_cast<int>(laid.size());
    out.reusePage = reuseFrom;
    out.pageShiftFrom = tailFrom;
    out.pageShiftDelta = delta;
    return out;
}

bool paragraphHasNote(const model::Paragraph& para) {
    for (const auto& run : para.runs) {
        for (const auto& item : run.items) {
            if (item.type != model::InlineItem::Type::Control) continue;
            if (item.control.kind == model::ControlKind::Footnote ||
                item.control.kind == model::ControlKind::Endnote) {
                return true;
            }
        }
    }
    return false;
}

SectionReflow reflowEditedPages(const model::Document& doc, std::vector<Node>& pages, const EditRelayoutSpec& spec) {
    SectionReflow out;
    const model::StorySlot* slot = resolveStory(doc, spec.story);
    if (!slot || slot->kind != model::StoryKind::Section || !slot->paragraphs) {
        out.skipReason = "구역본문아님";
        return out;
    }
    if (spec.paraIndex < 0 || spec.paraIndex >= static_cast<int>(slot->paragraphs->size())) {
        out.skipReason = "문단범위밖";
        return out;
    }
    if (paragraphHasNote((*slot->paragraphs)[static_cast<std::size_t>(spec.paraIndex)])) {
        out.skipReason = "각주미주";
        return out;
    }
    const int sectionIndex = slot->sectionIndex;
    int start = spec.hintPage;
    if (start < 0 || start >= static_cast<int>(pages.size()) ||
        pages[static_cast<std::size_t>(start)].section != sectionIndex) {
        start = -1;
        for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
            if (pages[static_cast<std::size_t>(i)].section == sectionIndex &&
                bodyHasParagraph(pages[static_cast<std::size_t>(i)], spec.paraIndex)) {
                start = i;
                break;
            }
        }
    }
    if (start < 0) {
        for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
            if (pages[static_cast<std::size_t>(i)].section == sectionIndex) {
                start = i;
                break;
            }
        }
    }
    if (start < 0) return out;
    return reflowPagesFrom(doc, pages, start, spec.useStoredLineHeight, spec.useStoredLineHeight);
}

bool reflowResizedPages(const model::Document& doc, std::vector<Node>& pages, int startPage, bool useStoredLineHeight,
                        bool deferSpans, const std::string& bin, int newWidth, int newHeight) {
    if (repackTreatAsCharPicture(doc, pages, startPage, bin, newWidth, newHeight)) return true;
    return reflowPagesFrom(doc, pages, startPage, useStoredLineHeight, deferSpans).ok;
}

model::LineSeg lineSegFor(const Node& line, std::int32_t vertPos) {
    model::LineSeg seg;
    const std::int32_t textHeight = line.textHeight > 0 ? line.textHeight : line.fontSize;
    seg.textPos = line.start > 0 ? static_cast<std::uint32_t>(line.start) : 0;
    seg.vertPos = vertPos;
    // 글자 높이와 글자처럼 취한 개체 높이 중 큰 값이다.
    const std::int32_t box = measuredTextBox(line, textHeight);
    seg.textHeight = box;
    // vertsize 는 다음 줄로 넘어간 앞 글자까지 잰 높이라 textheight 보다 클 수 있다.
    seg.vertSize = box;
    if (line.measuredSeg.vertSize > seg.vertSize) seg.vertSize = line.measuredSeg.vertSize;
    // 글자 기준선, 그림은 위에서 85%, 수식은 수식 높이×백분율 중 큰 값이다.
    const std::int32_t glyphBaseline = line.lineBaseline >= 0 ? line.lineBaseline
                                                              : layout::glyphBaseline(textHeight);
    seg.baseline = lineSegBaseline(line, box, glyphBaseline);
    seg.spacing = line.height - textHeight;
    seg.horzPos = line.x;
    seg.horzSize = std::max(kMinParagraphWidth, line.width);
    // 저장된 줄은 글자 이동에 들여쓰기가 남아 있다. 다시 조판한 줄은 측정할 때 이미 플래그를 넣는다.
    seg.flags = line.measuredSeg.flags != 0 ? line.measuredSeg.flags : measuredLineFlags(line.indent > 0, line.indent);
    return seg;
}

namespace {

ParagraphPlace paragraphPlace(const Node& node) {
    ParagraphPlace paragraph;
    paragraph.index = node.paragraph;
    paragraph.x = node.x + node.marginLeft;
    paragraph.y = node.y;
    paragraph.width = node.width;
    paragraph.height = node.height;
    for (const auto& child : node.children) {
        if (child.kind != NodeKind::Line) continue;
        LinePlace line;
        line.y = child.y;
        line.x = child.x;
        line.width = child.width;
        line.height = child.height;
        std::int32_t pen = 0;
        for (const auto& slot : child.children) {
            if (slot.kind == NodeKind::Span) {
                if (!slot.advances.empty()) {
                    for (const std::int32_t width : slot.advances) {
                        line.xs.push_back(pen);
                        pen += width;
                    }
                } else {
                    line.xs.push_back(pen);
                    pen += slot.width;
                }
            } else if (slot.kind == NodeKind::Object) {
                line.xs.push_back(slot.x);
                pen = slot.x + slot.width + slot.marginLeft + slot.marginRight;
            }
        }
        paragraph.lines.push_back(std::move(line));
    }
    return paragraph;
}

void fillScope(const Node& scope, std::int32_t originX, std::int32_t originY,
               std::vector<ColumnPlace>& columns, std::vector<ObjectPlace>& objects);

ObjectPlace objectPlace(const Node& node, std::int32_t originX, std::int32_t originY) {
    ObjectPlace object;
    object.x = originX + node.x;
    object.y = originY + node.y;
    object.width = node.width;
    object.height = node.height;
    object.paragraph = node.paragraph;
    fillScope(node, 0, 0, object.columns, object.objects);
    return object;
}

void fillScope(const Node& scope, std::int32_t originX, std::int32_t originY,
               std::vector<ColumnPlace>& columns, std::vector<ObjectPlace>& objects) {
    for (const auto& child : scope.children) {
        if (child.role == "pageFill" || child.role == "pageBorder") continue;
        if (child.kind == NodeKind::Body) {
            fillScope(child, originX + child.x, originY + child.y, columns, objects);
        } else if (child.kind == NodeKind::Cell) {
            objects.push_back(objectPlace(child, originX, originY));
        } else if (child.kind == NodeKind::Column) {
            ColumnPlace column;
            column.x = originX + child.x;
            column.y = originY + child.y;
            column.width = child.width;
            column.height = child.height;
            for (const auto& item : child.children) {
                if (item.kind == NodeKind::Paragraph) column.paragraphs.push_back(paragraphPlace(item));
                else if (item.kind == NodeKind::Object && !item.treatAsChar) {
                    objects.push_back(objectPlace(item, column.x, column.y));
                }
            }
            columns.push_back(std::move(column));
        } else if (child.kind == NodeKind::Object && !child.treatAsChar) {
            objects.push_back(objectPlace(child, originX, originY));
        }
    }
}

}  // namespace

std::vector<PagePlace> layoutPlaces(const Document& doc) {
    std::vector<PagePlace> pages;
    for (const Node& paper : layoutPages(doc)) {
        PagePlace page;
        page.width = paper.width;
        page.height = paper.height;
        fillScope(paper, 0, 0, page.columns, page.objects);
        pages.push_back(std::move(page));
    }
    return pages;
}

namespace {

// 띠에 든 글 묶음 번호를 앞에서부터 겹치지 않게 모은다. 단·문단·줄·개체 모두 본다.
void bandStories(const Node& node, std::vector<int>& out) {
    if (node.story > 0 && std::find(out.begin(), out.end(), node.story) == out.end()) {
        out.push_back(node.story);
    }
    for (const Node& child : node.children) bandStories(child, out);
}

void renameStories(Node& node, const std::unordered_map<int, int>& names) {
    if (node.story > 0) {
        if (const auto it = names.find(node.story); it != names.end()) node.story = it->second;
    }
    for (Node& child : node.children) renameStories(child, names);
}

// 이 띠가 그 머리말·꼬리말에서 나왔는가. 띠 안 글상자 글 묶음도 바깥으로 올라가 본다.
bool bandFromControl(const Node& band, const Control& control) {
    std::vector<int> stories;
    bandStories(band, stories);
    if (stories.empty()) return false;
    for (const model::StorySlot* slot = gStoryMap.slot(stories.front()); slot;
         slot = slot->parentStory > 0 ? gStoryMap.slot(slot->parentStory) : nullptr) {
        if (slot->control && (slot->control->kind == ControlKind::Header || slot->control->kind == ControlKind::Footer)) {
            return slot->control == &control;
        }
    }
    return false;
}

}  // namespace

bool relayoutPageBands(const Document& doc, std::vector<Node>& pages, const Control& control,
                       bool useStoredLineHeight, std::vector<FrameInvalidation>& invalidations) {
    if (control.kind != ControlKind::Header && control.kind != ControlKind::Footer) return false;
    const std::tm dateNow = clockNow();
    const std::tm docDate = clockFromText(doc.metadata.createdAt, dateNow);
    std::vector<int> sectionPages;
    for (const Node& paper : pages) {
        const int sec = std::max(0, paper.section);
        if (sec >= static_cast<int>(sectionPages.size())) sectionPages.resize(static_cast<std::size_t>(sec) + 1, 0);
        ++sectionPages[static_cast<std::size_t>(sec)];
    }
    std::vector<FrameInvalidation> out;
    for (int pi = 0; pi < static_cast<int>(pages.size()); ++pi) {
        Node& paper = pages[static_cast<std::size_t>(pi)];
        const int sec = std::max(0, paper.section);
        if (sec >= static_cast<int>(doc.sections.size())) continue;
        for (int ci = 0; ci < static_cast<int>(paper.children.size()); ++ci) {
            Node& old = paper.children[static_cast<std::size_t>(ci)];
            if (!old.pageBand || !bandFromControl(old, control)) continue;
            const model::Section& section = doc.sections[static_cast<std::size_t>(sec)];
            if (!section.page) return false;
            Node scratch;
            attachBand(scratch, control, *section.page, doc, paper.pageNumber,
                       sectionPages[static_cast<std::size_t>(sec)], paper.pageNumberHidden, dateNow, docDate,
                       useStoredLineHeight);
            if (scratch.children.empty()) return false;
            Node fresh = std::move(scratch.children.back());
            // 커서는 옛 띠의 글 묶음 번호에 있다. 같은 순서로 이어 붙여 번호를 그대로 둔다.
            std::vector<int> before;
            std::vector<int> after;
            bandStories(old, before);
            bandStories(fresh, after);
            if (before.size() != after.size()) return false;
            std::unordered_map<int, int> names;
            for (std::size_t i = 0; i < after.size(); ++i) names[after[i]] = before[i];
            renameStories(fresh, names);
            old = std::move(fresh);
            out.push_back(FrameInvalidation{pi, {ci}, -1, -1, false});
        }
    }
    if (out.empty()) return false;
    invalidations = std::move(out);
    return true;
}

}  // namespace noting::render
