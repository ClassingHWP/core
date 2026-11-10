// 메모 상자 배치(화면). 한글 2024 의 메모 배치와 같다.
// 좌표는 HWPUNIT 이고, 세로 쌓기는 쪽 본문 영역 위를 0 으로 한다.
//
// - 가로: R(오른쪽 여백) ≥ 14mm 이고 W(메모 너비) < R − 14mm 이면 상자 오른쪽이 쪽 오른쪽 − 6mm, 아니면 본문 오른쪽 + 10mm.
//   답글은 10mm 들여 놓고 폭이 10mm 좁다.
// - 상자: 머리글 1700(작성자·날짜) + 본문 높이(메모 글 조판 높이 + 위아래 1mm). 상자 사이는 1mm.
// - 세로: 쪽 메모 높이 합이 본문 높이보다 크면 긴 상자를 줄이고, 그래도 크면 위에서부터 차례로 쌓는다.
//   아니면 앵커(필드가 든 줄 아래 끝)가 같은 상자끼리 묶어 앵커 가운데에 두고, 앞 묶음과 겹치면 반씩 나눠 비키고,
//   쪽 아래를 넘으면 올린다.
// - 연결선: 앵커 + 0.5mm → 상자 왼쪽 − 10mm(앵커 높이) → 상자 왼쪽(본문 위 + min(본문 높이/3, 3mm)). 고르지 않은 메모는 파선.
//   답글은 원 메모 상자 왼쪽 + 5mm 에 원 메모 본문 아래부터 답글 본문 아래까지 세로선(#D6DBE9)을 긋는다.

#include <algorithm>
#include <string>
#include <vector>

#include "chwp/model/memo.hpp"
#include "chwp/render/guides.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"
#include "render/scene_detail.hpp"

namespace chwp::render {

namespace detail {
Node layoutBand(const model::Document& doc, const std::vector<model::Paragraph>& paragraphs, std::int32_t width,
                const std::string& align, bool useStoredLineHeight);
}  // namespace detail

namespace {

constexpr std::int32_t kMm = 283;          // 1mm (283465 ÷ 1000 을 반올림)
constexpr std::int32_t kHeadHeight = 1700;  // 머리글 6mm
constexpr std::int32_t kGap = kMm;          // 상자 사이

using paint_detail::PointerHold;

struct MemoBox {
    const model::Control* memo = nullptr;
    const model::Control* parent = nullptr;
    int number = 0;
    int page = -1;
    std::int32_t anchorX = 0;
    std::int32_t anchorY = 0;  // 쪽 기준(줄 아래 끝)
    std::int32_t x = 0;
    std::int32_t width = 0;
    std::int32_t bodyH = 0;  // 머리글 뺀 높이
    std::int32_t y = 0;      // 본문 위(쪽 본문 영역 위 기준)
    Node text;               // 메모 글 조판
    model::MemoShape shape;
    std::uint64_t created = 0;
    std::string author;
};

std::int32_t block(const MemoBox& b) { return kHeadHeight + b.bodyH + kGap; }

const Node* bodyOf(const Node& paper) {
    for (const Node& child : paper.children) {
        if (child.kind == NodeKind::Body && !child.pageBand && !child.fromMaster && child.role.empty()) return &child;
    }
    return nullptr;
}

// 메모 필드 시작의 문단·글자 번호.
bool memoPosition(const model::Document& doc, const model::Control* memo, int& story, int& paragraph, int& charIndex) {
    const model::StoryMap& map = ensureStoryMap(doc);
    for (int s = 1; s <= map.count(); ++s) {
        const model::StorySlot* slot = map.slot(s);
        if (!slot || !slot->paragraphs || map.storyOf(slot->paragraphs) != s) continue;
        const std::vector<model::Paragraph>& paras = *slot->paragraphs;
        for (std::size_t p = 0; p < paras.size(); ++p) {
            int chars = 0;
            for (const model::Run& run : paras[p].runs) {
                for (const model::InlineItem& item : run.items) {
                    if (item.type == model::InlineItem::Type::Control && &item.control == memo) {
                        story = s;
                        paragraph = static_cast<int>(p);
                        charIndex = chars;
                        return true;
                    }
                    if (item.type == model::InlineItem::Type::Text) {
                        chars += paint_detail::utf8CodePointCount(item.text);
                    } else if (paint_detail::isCaretCharControl(item.control)) {
                        ++chars;
                    }
                }
            }
        }
    }
    return false;
}

// 메모 필드 시작 글자의 자리(쪽, x, 줄 위, 줄 높이). 포인터 상태를 보지 않는다(쪽 쌓기는 일꾼 스레드에서도 돈다).
bool anchorOf(const model::Document& doc, const std::vector<Node>& pages, int story, int paragraph, int offset,
              int& page, std::int32_t& x, std::int32_t& y, std::int32_t& height) {
    paint_detail::Painter painter;
    painter.doc = &doc;
    bool found = false;
    paint_detail::forEachStoryParagraphPage(pages, story, paragraph, paragraph, -1, [&](int pageIndex, const Node& curPage) {
        if (found) return;
        std::vector<paint_detail::Painter::StoryLine> lines;
        painter.gatherStoryParagraph(curPage, 0, 0, story, paragraph, lines);
        for (const paint_detail::Painter::StoryLine& line : lines) {
            if (!line.node || line.node->paragraph != paragraph) continue;
            if (offset < line.node->start || (line.node->end > line.node->start && offset >= line.node->end && !line.node->lineData().lastLine)) {
                continue;
            }
            const CaretPlace place = painter.placeOnLineAtOffset(line, offset);
            if (!place.hit) continue;
            page = pageIndex;
            x = place.x;
            y = place.y;
            height = place.height;
            found = true;
            return;
        }
    });
    return found;
}

// 같은 쪽 메모를 한글 차례로 놓는다(넘치면 줄여 위부터, 아니면 묶어 가운데, 겹치면 반씩, 쪽 아래면 올림).
void stack(std::vector<MemoBox*>& boxes, std::int32_t bodyH, std::int32_t topMargin, std::int32_t bottomMargin) {
    if (boxes.empty()) return;
    std::int32_t sum = 0;
    for (const MemoBox* b : boxes) sum += block(*b);
    if (sum > bodyH) {
        const std::int32_t share = std::max<std::int32_t>(bodyH / static_cast<std::int32_t>(boxes.size()), 2 * kMm + 1000);
        for (MemoBox* b : boxes) {
            if (kHeadHeight + b->bodyH > share) b->bodyH = std::max(0, share - kHeadHeight);
        }
        sum = 0;
        for (const MemoBox* b : boxes) sum += block(*b);
    }
    if (sum > bodyH) {
        const std::int32_t over = sum - bodyH;
        const std::int32_t margins = topMargin + bottomMargin;
        std::int32_t y = over < margins && margins > 0
                             ? static_cast<std::int32_t>(-static_cast<std::int64_t>(over) * topMargin / margins)
                             : -(topMargin + kMm);
        for (MemoBox* b : boxes) {
            b->y = y;
            y += block(*b);
        }
        return;
    }
    // 묶음: 앵커 높이가 같은 상자들(원 메모와 답글 포함).
    struct Group {
        std::size_t first = 0;
        std::size_t last = 0;
    };
    std::vector<Group> groups;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (!groups.empty() && boxes[groups.back().first]->anchorY == boxes[i]->anchorY) {
            groups.back().last = i;
        } else {
            groups.push_back({i, i});
        }
    }
    const auto place = [&](const Group& g, std::int32_t firstY) {
        std::int32_t y = firstY;
        for (std::size_t i = g.first; i <= g.last; ++i) {
            boxes[i]->y = y;
            y += block(*boxes[i]);
        }
    };
    const auto next = [&](const Group& g) { return boxes[g.last]->y + block(*boxes[g.last]); };
    const auto first = [&](const Group& g) { return boxes[g.first]->y; };
    const std::int32_t limit = bodyH + bottomMargin;
    for (std::size_t gi = 0; gi < groups.size(); ++gi) {
        const Group& g = groups[gi];
        std::int32_t height = 0;
        for (std::size_t i = g.first; i <= g.last; ++i) height += kHeadHeight + boxes[i]->bodyH;
        height += static_cast<std::int32_t>(g.last - g.first) * kGap;
        // 앵커 y 는 본문 영역 위 기준이다(boxes 의 anchorY 는 이미 그 기준으로 넘어온다).
        std::int32_t y = std::max(0, boxes[g.first]->anchorY - (height + kMm) / 2);
        place(g, y);
        if (gi > 0) {
            const Group& prev = groups[gi - 1];
            const std::int32_t overlap = next(prev) - first(g);
            if (overlap > 0) {
                const std::int32_t half = overlap / 2;
                const std::int32_t floor = gi >= 2 ? next(groups[gi - 2]) : 0;
                const std::int32_t up = std::min(half, std::max(0, first(prev) - floor));
                place(prev, first(prev) - up);
                place(g, first(g) + (overlap - up));
            }
        }
        // 쪽 아래를 넘으면 올리고, 닿는 앞 묶음도 같이 올린다(첫 묶음은 0 까지).
        const std::int32_t below = next(g) - limit;
        if (below > 0) {
            place(g, first(g) - below);
            for (std::size_t k = gi; k > 0; --k) {
                const Group& cur = groups[k];
                const Group& prev = groups[k - 1];
                const std::int32_t clash = next(prev) - first(cur);
                if (clash <= 0) break;
                const std::int32_t floor = k >= 2 ? next(groups[k - 2]) : 0;
                place(prev, std::max(floor, first(prev) - clash));
            }
            if (first(groups.front()) < 0) {
                const std::int32_t push = -first(groups.front());
                for (std::size_t k = 0; k <= gi; ++k) place(groups[k], first(groups[k]) + push);
            }
        }
    }
}

Node solidBox(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, const std::string& fill,
              const std::string& line, std::int32_t lineWidth, const char* role) {
    Node box;
    box.kind = NodeKind::Object;
    box.role = role;
    box.x = x;
    box.y = y;
    box.width = std::max(1, w);
    box.height = std::max(1, h);
    box.story = 0;
    NodeExtra& ext = box.extMut();
    ext.fillColor = fill;
    ext.borderLeft = BorderEdge{"SOLID", line, lineWidth, 1};
    ext.objMut().textWrap = "IN_FRONT_OF_TEXT";
    return box;
}

Node polyline(const std::vector<std::pair<std::int32_t, std::int32_t>>& points, const std::string& color,
              const char* type, std::int32_t width) {
    Node line;
    line.kind = NodeKind::Object;
    line.role = "memoLine";
    line.story = 0;
    std::int32_t minX = points.front().first, minY = points.front().second, maxX = minX, maxY = minY;
    for (const auto& [px, py] : points) {
        minX = std::min(minX, px);
        minY = std::min(minY, py);
        maxX = std::max(maxX, px);
        maxY = std::max(maxY, py);
    }
    line.x = minX;
    line.y = minY;
    line.width = std::max(1, maxX - minX);
    line.height = std::max(1, maxY - minY);
    NodeExtra& ext = line.extMut();
    ext.borderLeft = BorderEdge{type, color, width, 1};
    ext.objMut().textWrap = "IN_FRONT_OF_TEXT";
    ext.objMut().shapeType = "polygon";
    for (const auto& [px, py] : points) ext.objMut().points.push_back(model::ShapePoint{px - minX, py - minY});
    return line;
}

// 머리글 글 한 줄(글자 모양 0 번). 오른쪽 맞춤이면 오른쪽 끝을 right 에 둔다.
Node headText(const model::Document& doc, const std::string& text, std::int32_t width, const char* align) {
    std::vector<model::Paragraph> paragraphs(1);
    model::Run run;
    run.charShapeId = 0;
    if (!text.empty()) run.items.push_back(model::InlineItem::makeText(text));
    paragraphs.front().runs.push_back(std::move(run));
    Node band = detail::layoutBand(doc, paragraphs, width, align, false);
    band.kind = NodeKind::Column;
    const auto clear = [](const auto& self, Node& n) -> void {
        n.story = 0;
        n.paragraph = -1;
        for (Node& c : n.children) self(self, c);
    };
    clear(clear, band);
    return band;
}

}  // namespace

void placeMemos(const model::Document& doc, std::vector<Node>& pages, std::vector<int>* changed) {
    // 보기에서 메모를 끄면 메모가 없는 것처럼 둔다(메모 영역 띠·상자를 그리지 않고 쪽을 종이 폭으로 둔다).
    const std::vector<const model::Control*> memos =
        viewMarks().memo ? model::collectMemos(doc) : std::vector<const model::Control*>{};
    // 지난 배치를 지운다. 메모가 없으면 여기서 끝난다.
    std::vector<bool> had(pages.size(), false);
    for (std::size_t i = 0; i < pages.size(); ++i) {
        auto& kids = pages[i].children;
        const auto before = kids.size();
        kids.erase(std::remove_if(kids.begin(), kids.end(), [](const Node& n) { return n.role == "memos"; }), kids.end());
        had[i] = kids.size() != before;
        if (pages[i].ext().memoExtra != 0) {
            pages[i].extMut().memoExtra = 0;
            had[i] = true;
        }
    }
    std::vector<std::vector<MemoBox>> perPage(pages.size());
    if (!memos.empty()) {
        int number = 0;
        int revisions = 0;
        for (const model::Control* memo : memos) {
            ++number;
            const bool revision = memo->ctrlId == "%%mr";
            if (revision) ++revisions;
            int story = 0, paragraph = 0, charIndex = 0;
            if (!memoPosition(doc, memo, story, paragraph, charIndex)) continue;
            const int offset = paint_detail::charIndexToOffset(doc, pages, story, paragraph, charIndex);
            CaretPlace place;
            if (!anchorOf(doc, pages, story, paragraph, offset, place.page, place.x, place.y, place.height)) continue;
            if (place.page < 0 || place.page >= static_cast<int>(pages.size())) continue;
            const Node& paper = pages[static_cast<std::size_t>(place.page)];
            if (paper.section < 0 || paper.section >= static_cast<int>(doc.sections.size())) continue;
            MemoBox box;
            box.memo = memo;
            box.number = number;
            box.page = place.page;
            box.anchorX = place.x;
            box.anchorY = place.y + std::max<std::int32_t>(place.height, 1);
            box.shape = model::memoShapeFor(doc, doc.sections[static_cast<std::size_t>(paper.section)], *memo);
            const model::MemoCommand cmd = model::parseMemoCommand(memo->command);
            box.author = cmd.author.empty() ? "noname" : cmd.author;
            // 메모 고침표(교정 부호): 주황 상자 #FFECDB·#F2834E, 머리글 "[고침표:차례] 작성자"(한글 2024 화면에서 잼).
            if (revision) {
                box.shape.fillColor = "#FFECDB";
                box.shape.lineColor = "#F2834E";
                box.author = "[고침표:" + std::to_string(revisions) + "] " + box.author;
            }
            box.created = cmd.created;
            for (const model::Control* other : memos) {
                if (memo->memoParentId != 0 && other->fieldId == memo->memoParentId) box.parent = other;
            }
            perPage[static_cast<std::size_t>(place.page)].push_back(std::move(box));
        }
    }
    for (std::size_t pi = 0; pi < pages.size(); ++pi) {
        std::vector<MemoBox>& boxes = perPage[pi];
        Node& paper = pages[pi];
        if (boxes.empty()) {
            if (had[pi] && changed) changed->push_back(static_cast<int>(pi));
            continue;
        }
        const Node* body = bodyOf(paper);
        const std::int32_t bodyX = body ? body->x : paper.marginLeft;
        const std::int32_t bodyY = body ? body->y : paper.marginTop;
        const std::int32_t bodyW = body ? body->width : paper.width - paper.marginLeft - paper.marginRight;
        const std::int32_t bodyH = body ? body->height : paper.height - paper.marginTop - paper.marginBottom;
        const std::int32_t bodyRight = bodyX + bodyW;
        const std::int32_t right = paper.width - bodyRight;
        // 메모 글 조판과 가로 자리. 들어가지 않으면 화면에서 쪽을 W + 14mm − R 만큼 넓힌다.
        std::int32_t extra = 0;
        std::int32_t gapUsed = 10 * kMm;
        std::int32_t widest = 0;
        for (MemoBox& b : boxes) {
            const std::int32_t w = static_cast<std::int32_t>(b.shape.width);
            const bool fits = right >= 14 * kMm && w < right - 14 * kMm;
            b.x = fits ? bodyRight + (right - w - 6 * kMm) : bodyRight + 10 * kMm;
            if (!fits) extra = std::max(extra, w + 14 * kMm - right);
            gapUsed = fits ? right - w - 6 * kMm : 10 * kMm;
            widest = std::max(widest, w);
            b.width = w;
            if (b.parent) {
                b.x += 10 * kMm;
                b.width = std::max(kMm, w - 10 * kMm);
            }
            static const std::vector<model::Paragraph> kEmpty(1);
            const std::vector<model::Paragraph>& paras =
                b.memo->subLists.empty() || b.memo->subLists.front().paragraphs.empty() ? kEmpty
                                                                                        : b.memo->subLists.front().paragraphs;
            b.text = detail::layoutBand(doc, paras, std::max<std::int32_t>(kMm, b.width - 2 * kMm), "", false);
            // 메모 글 높이는 마지막 줄의 글자 상자 아래까지다(마지막 줄 아래 줄 간격은 넣지 않는다).
            // 한 줄 9pt 메모의 본문이 900 + 위아래 1mm = 1466 이다(한글 화면 측정).
            std::int32_t textH = 0;
            const auto lastLine = [&](const auto& self, const Node& n, std::int32_t oy) -> void {
                if (n.kind == NodeKind::Line) {
                    const std::int32_t box = n.lineData().textHeight > 0 ? n.lineData().textHeight : n.fontSize;
                    textH = std::max(textH, oy + n.y + n.marginTop + box);
                    return;
                }
                for (const Node& c : n.children) self(self, c, n.kind == NodeKind::Body ? oy : oy + n.y);
            };
            lastLine(lastLine, b.text, 0);
            b.bodyH = std::min(bodyH, textH + 2 * kMm);
            b.anchorY -= bodyY;  // 본문 영역 위 기준
        }
        // 한글 차례: 앵커 y, 앵커 x, 답글은 원 메모 뒤.
        std::vector<MemoBox*> order;
        for (MemoBox& b : boxes) order.push_back(&b);
        std::stable_sort(order.begin(), order.end(), [](const MemoBox* a, const MemoBox* b) {
            const std::int32_t ay = a->parent ? a->anchorY : a->anchorY;
            if (ay != b->anchorY) return ay < b->anchorY;
            if (a->anchorX != b->anchorX) return a->anchorX < b->anchorX;
            return a->number < b->number;
        });
        stack(order, bodyH, bodyY, paper.height - bodyY - bodyH);

        Node memosNode;
        memosNode.kind = NodeKind::Column;
        memosNode.role = "memos";
        memosNode.width = paper.width + extra;
        memosNode.height = paper.height;
        memosNode.story = 0;
        // 메모 영역 띠: 넓힌 쪽 오른쪽 − W − 띄움/2 부터 넓힌 쪽 오른쪽까지, 회색 #F4F4F4, 경계 #DFDFDF.
        {
            const std::int32_t widened = paper.width + extra;
            const std::int32_t bandLeft = widened - widest - gapUsed / 2;
            memosNode.children.push_back(
                solidBox(bandLeft, 0, widened - bandLeft, paper.height, "#F4F4F4", "#DFDFDF", 28, "memoArea"));
        }
        if (extra != 0) paper.extMut().memoExtra = extra;
        for (const MemoBox* b : order) {
            const std::int32_t top = bodyY + b->y - kHeadHeight;  // 보이는 상자 위(쪽 기준)
            const std::int32_t bodyTop = bodyY + b->y;
            if (!b->parent) {
                const std::int32_t knee = b->x - 10 * kMm;
                const std::int32_t end = bodyTop + std::min(b->bodyH / 3, 3 * kMm);
                const std::int32_t anchorY = bodyY + b->anchorY;
                memosNode.children.push_back(polyline(
                    {{std::min(b->anchorX + kMm / 2, knee), anchorY}, {knee, anchorY}, {b->x, end}}, b->shape.lineColor,
                    "DASH", 28));
            } else {
                for (const MemoBox* p : order) {
                    if (p->memo != b->parent) continue;
                    const std::int32_t lineX = p->x + 5 * kMm;
                    memosNode.children.push_back(polyline({{lineX, bodyY + p->y + p->bodyH}, {lineX, bodyTop + b->bodyH}},
                                                          "#D6DBE9", "SOLID", 28));
                }
            }
            Node box = solidBox(b->x, top, b->width, kHeadHeight + b->bodyH, b->shape.fillColor, b->shape.lineColor, 28,
                                "memoBox");
            // 머리글: 왼쪽 작성자, 오른쪽 날짜(답글 상자가 아니면 그 뒤 "답글").
            const std::int32_t inner = std::max<std::int32_t>(kMm, b->width - 2 * kMm);
            Node author = headText(doc, b->author, inner, "LEFT");
            author.x = kMm;
            author.y = (kHeadHeight - 1000) / 2 - 200;
            Node date = headText(doc, model::memoHeadDate(b->created), inner, "RIGHT");
            date.x = kMm;
            date.y = author.y;
            Node text = b->text;
            text.kind = NodeKind::Column;
            text.x = kMm;
            text.y = kHeadHeight + kMm;
            box.children.push_back(std::move(author));
            box.children.push_back(std::move(date));
            box.children.push_back(std::move(text));
            memosNode.children.push_back(std::move(box));
        }
        paper.children.push_back(std::move(memosNode));
        if (changed) changed->push_back(static_cast<int>(pi));
    }
}

}  // namespace chwp::render
