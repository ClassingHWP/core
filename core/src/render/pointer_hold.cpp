#include "chwp/render/paint.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/render/guides.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/scene_detail.hpp"
#include "render/picture.hpp"

// 포인터를 누르고 끄는 동안의 상태. 캐럿, 범위 선택, 개체 선택과 크기 조절, 터치 제스처가 여기 있다.
// 화면에 그리는 일은 paint.cpp 가 맡는다.

namespace chwp::render {

using namespace paint_detail;

namespace paint_detail {

int pointerPictureClock = 1;

void bumpPointerPicture() { ++pointerPictureClock; }

std::int64_t nowCaretMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void touchCaretBlink(PointerHold& hold) { hold.caretTouchMs = nowCaretMs(); }

int currentCaretPageHint() { return pointerHold().caretPage; }

void clearPendingCharShape(PointerHold& hold) {
    hold.hasPendingCharShape = false;
    hold.pendingCharShapeId = 0;
    hold.pendingCharStory = 0;
    hold.pendingCharPara = -1;
    hold.pendingCharOffset = -1;
}

std::optional<std::uint32_t> pendingCharShapeForCaret(int story, int paragraph, int offset) {
    const PointerHold& hold = pointerHold();
    if (!hold.hasPendingCharShape || story <= 0) return std::nullopt;
    if (hold.anchorPara != hold.focusPara || hold.anchorOff != hold.focusOff) return std::nullopt;
    if (hold.pendingCharStory == story && hold.pendingCharPara == paragraph && hold.pendingCharOffset == offset) {
        return hold.pendingCharShapeId;
    }
    return std::nullopt;
}

void clearComposition(PointerHold& hold) {
    hold.composing = false;
    hold.compStory = 0;
    hold.compPara = -1;
    hold.compStart = 0;
    hold.compEnd = 0;
    hold.compStartOff = 0;
    hold.compEndOff = 0;
    hold.compUnderlines.clear();
}

void caretFrameLines(const Node& paper, const model::Document* doc, int story,
                     std::vector<PointerHold::FrameLine>& out);
void fillBandAreas(PointerHold& hold, const std::vector<Node>& pages);

void placeCaret(PointerHold& hold, const CaretPlace& place, int page, const std::vector<Node>& pages,
                const model::Document* doc) {
    bumpPointerPicture();
    touchCaretBlink(hold);
    hold.caret = place;
    hold.caretPage = page;
    hold.caretPageW = 0;
    hold.caretPageH = 0;
    hold.frameLines.clear();
    hold.framePage = -1;
    fillBandAreas(hold, pages);
    if (page < 0 || page >= static_cast<int>(pages.size())) return;
    hold.caretPageW = viewWidthOf(pages[static_cast<std::size_t>(page)]);
    hold.caretPageH = pages[static_cast<std::size_t>(page)].height;
    if (place.hit && place.story > 0) {
        caretFrameLines(pages[static_cast<std::size_t>(page)], doc, place.story, hold.frameLines);
        hold.framePage = page;
    }
}

bool dragBoxContains(const DragBox& box, int x, int y) {
    return box.page >= 0 && box.w > 0 && box.h > 0 && x >= box.x && y >= box.y && x < box.x + box.w && y < box.y + box.h;
}

bool inlineObjectControl(const model::Control& ctrl) {
    using model::ControlKind;
    return ctrl.placement && ctrl.placement->treatAsChar &&
           (ctrl.kind == ControlKind::Shape || ctrl.kind == ControlKind::Picture || ctrl.kind == ControlKind::Table ||
            ctrl.kind == ControlKind::Container || ctrl.kind == ControlKind::Equation);
}

int controlCharOffset(const model::Paragraph& para, const model::Control* control) {
    if (!control) return -1;
    int total = 0;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                std::size_t idx = 0;
                char32_t cp = 0;
                while (nextCp(item.text, idx, cp)) ++total;
            } else if (inlineObjectControl(item.control)) {
                if (&item.control == control) return total;
                ++total;
            }
        }
    }
    return -1;
}

struct ContainerHit {
    bool inCell = false;
    const model::Control* control = nullptr;
    int row = -1;
    int col = -1;
    int rowSpan = 1;
    int colSpan = 1;
    DragBox objectBox;
    DragBox cellBox;
};

int cellContentStory(const Node& node) {
    if ((node.kind == NodeKind::Paragraph || node.kind == NodeKind::Line || node.kind == NodeKind::Column) &&
        node.story > 0) {
        return node.story;
    }
    for (const Node& child : node.children) {
        if (child.kind == NodeKind::Object) continue;
        if (const int found = cellContentStory(child)) return found;
    }
    return 0;
}

// 글자처럼 넣은 개체는 줄 칸이 아니라, 그릴 때 문단 정렬만큼 밀린 자리에 있다.
template <typename Visit>
void walkPlacedChildren(const Node& node, std::int32_t x, std::int32_t y, const model::Document* doc, Visit&& visit) {
    if (node.kind == NodeKind::Line) {
        std::vector<InlineBorder> borders;
        const bool seated = treatAsCharBorders(node, doc, borders);
        const int count = static_cast<int>(node.children.size());
        for (int i = 0; i < count; ++i) {
            const Node& child = node.children[static_cast<std::size_t>(i)];
            if (child.kind == NodeKind::Object && child.treatAsChar) {
                std::int32_t ox = x + child.marginLeft;
                if (seated && i < static_cast<int>(borders.size()) && borders[static_cast<std::size_t>(i)].ok) {
                    ox = x + borders[static_cast<std::size_t>(i)].x + child.marginLeft - child.x;
                }
                visit(child, ox, y + child.marginTop);
            } else {
                visit(child, x, y);
            }
        }
        return;
    }
    for (const Node& child : node.children) visit(child, x, y);
}

// 쪽의 노드를 그린 자리(쪽 좌표 왼쪽 위)와 함께 찾아 간다. 줄 속 글자처럼 넣은 개체도 그린 자리다.
// visit 이 false 를 돌려주면 그 아래는 보지 않는다.
template <typename Visit>
void walkPlacedNodes(const model::Document& doc, const Node& node, std::int32_t ox, std::int32_t oy, Visit& visit) {
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    if (!visit(node, x, y)) return;
    walkPlacedChildren(node, x, y, &doc, [&](const Node& child, std::int32_t cx, std::int32_t cy) {
        walkPlacedNodes(doc, child, cx, cy, visit);
    });
}

void walkDragHit(const model::Document& doc, const Node& node, std::int32_t ox, std::int32_t oy, int page, int px,
                 int py, ContainerHit& hit) {
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Object && node.role != "caption" && node.width > 0 && node.height > 0 && px >= x &&
        py >= y && px < x + node.width && py < y + node.height) {
        hit.objectBox = DragBox{page, x, y, node.width, node.height};
    }
    if (node.kind == NodeKind::Cell && node.width > 0 && node.height > 0 && px >= x && py >= y &&
        px < x + node.width && py < y + node.height) {
        const int story = cellContentStory(node);
        if (const model::StorySlot* slot = story > 0 ? resolveStory(doc, story) : nullptr) {
            if (slot->kind == model::StoryKind::TableCell && slot->control) {
                hit.inCell = true;
                hit.control = slot->control;
                hit.row = node.ext().row;
                hit.col = node.ext().col;
                hit.rowSpan = std::max(1, node.ext().rowSpan);
                hit.colSpan = std::max(1, node.ext().colSpan);
                hit.cellBox = DragBox{page, x, y, node.width, node.height};
            }
        }
    }
    walkPlacedChildren(node, x, y, &doc, [&](const Node& child, std::int32_t cx, std::int32_t cy) {
        walkDragHit(doc, child, cx, cy, page, px, py, hit);
    });
}

ContainerHit dragHitAt(const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y) {
    ContainerHit hit;
    if (page < 0 || page >= static_cast<int>(pages.size())) return hit;
    walkDragHit(doc, pages[static_cast<std::size_t>(page)], 0, 0, page, x, y, hit);
    return hit;
}

void armDragHost(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y,
                 const CaretPlace& place) {
    hold.drag = {};
    if (!place.hit || place.story <= 0) return;
    const model::StorySlot* slot = resolveStory(doc, place.story);
    if (!slot || !slot->control || slot->parentStory <= 0 || slot->parentParagraph < 0) return;
    const ContainerHit hit = dragHitAt(doc, pages, page, x, y);
    const model::Paragraph* parent = storyParagraph(doc, slot->parentStory, slot->parentParagraph);
    const int parentOff = parent ? controlCharOffset(*parent, slot->control) : -1;
    if (parentOff < 0) return;
    if (slot->kind == model::StoryKind::TableCell && hit.inCell && hit.control == slot->control) {
        hold.drag.control = slot->control;
        hold.drag.table = true;
        hold.drag.row = hit.row;
        hold.drag.col = hit.col;
        hold.drag.rowSpan = hit.rowSpan;
        hold.drag.colSpan = hit.colSpan;
        hold.drag.focusRow = hit.row;
        hold.drag.focusCol = hit.col;
        hold.drag.focusRowSpan = hit.rowSpan;
        hold.drag.focusColSpan = hit.colSpan;
        hold.drag.parentStory = slot->parentStory;
        hold.drag.parentPara = slot->parentParagraph;
        hold.drag.parentOff = parentOff;
        hold.drag.objectBox = hit.objectBox.w > 0 ? hit.objectBox : hit.cellBox;
        return;
    }
    if (hit.objectBox.w > 0) {
        hold.drag.control = slot->control;
        hold.drag.parentStory = slot->parentStory;
        hold.drag.parentPara = slot->parentParagraph;
        hold.drag.parentOff = parentOff;
        hold.drag.objectBox = hit.objectBox;
    }
}

bool cellIntersects(int row, int col, int rowSpan, int colSpan, int row0, int row1, int col0, int col1) {
    const int rowEnd = row + std::max(1, rowSpan) - 1;
    const int colEnd = col + std::max(1, colSpan) - 1;
    return row <= row1 && rowEnd >= row0 && col <= col1 && colEnd >= col0;
}

void appendHeldBox(PointerHold& hold, const std::vector<Node>& pages, const DragBox& box) {
    if (box.page < 0 || box.page >= static_cast<int>(pages.size()) || box.w <= 0 || box.h <= 0) return;
    const Node& page = pages[static_cast<std::size_t>(box.page)];
    HeldBox held;
    held.page = box.page;
    held.x = box.x;
    held.y = box.y;
    held.width = box.w;
    held.height = box.h;
    held.pageW = viewWidthOf(page);
    held.pageH = page.height;
    held.invert = true;
    hold.boxes.push_back(held);
}

void collectCellBoxes(const model::Document& doc, const Node& node, std::int32_t ox, std::int32_t oy, int page,
                      const model::Control* control, int row0, int row1, int col0, int col1, PointerHold& hold,
                      const std::vector<Node>& pages) {
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Cell && node.width > 0 && node.height > 0) {
        const int story = cellContentStory(node);
        if (const model::StorySlot* slot = story > 0 ? resolveStory(doc, story) : nullptr) {
            if (slot->control == control &&
                cellIntersects(node.ext().row, node.ext().col, node.ext().rowSpan, node.ext().colSpan, row0, row1, col0, col1)) {
                appendHeldBox(hold, pages, DragBox{page, x, y, node.width, node.height});
            }
        }
    }
    walkPlacedChildren(node, x, y, &doc, [&](const Node& child, std::int32_t cx, std::int32_t cy) {
        collectCellBoxes(doc, child, cx, cy, page, control, row0, row1, col0, col1, hold, pages);
    });
}

void updateDragMode(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages, int page, int x,
                    int y, bool& moved) {
    if (!hold.drag.control || hold.drag.mode == DragMode::Object) return;
    if (!dragBoxContains(hold.drag.objectBox, x, y)) {
        if (hold.drag.parentStory > 0 && hold.drag.parentOff >= 0) {
            hold.drag.mode = DragMode::Object;
            hold.story = hold.drag.parentStory;
            hold.anchorPara = hold.drag.parentPara;
            hold.anchorOff = hold.drag.parentOff;
            moved = true;
        }
        return;
    }
    if (!hold.drag.table) return;
    const ContainerHit hit = dragHitAt(doc, pages, page, x, y);
    if (!hit.inCell || hit.control != hold.drag.control) return;
    if (hold.drag.mode == DragMode::Text && hit.row == hold.drag.row && hit.col == hold.drag.col) return;
    const bool changed = hold.drag.mode != DragMode::Cells || hit.row != hold.drag.focusRow ||
                         hit.col != hold.drag.focusCol;
    hold.drag.mode = DragMode::Cells;
    hold.drag.focusRow = hit.row;
    hold.drag.focusCol = hit.col;
    hold.drag.focusRowSpan = hit.rowSpan;
    hold.drag.focusColSpan = hit.colSpan;
    if (changed) moved = true;
}

void focusObjectSelection(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages, int page,
                          int x, int y, bool& moved) {
    const int parentStory = hold.drag.parentStory;
    const int parentPara = hold.drag.parentPara;
    const int parentOff = hold.drag.parentOff;
    hold.story = parentStory;
    hold.anchorPara = parentPara;
    hold.anchorOff = parentOff;
    int focusPara = parentPara;
    int focusOff = parentOff + 1;
    const CaretPlace place = caretInStory(doc, pages, page, x, y, parentStory, true, parentPara, parentOff);
    if (place.hit && place.story == parentStory) {
        focusPara = place.paragraph;
        focusOff = place.offset;
        const bool before = focusPara < parentPara || (focusPara == parentPara && focusOff <= parentOff);
        if (before) {
            focusPara = parentPara;
            focusOff = parentOff + 1;
        }
        const int caretPage = place.page >= 0 ? place.page : page;
        if (focusPara != hold.focusPara || focusOff != hold.focusOff || caretPage != hold.caretPage) moved = true;
        hold.focusPara = focusPara;
        hold.focusOff = focusOff;
        placeCaret(hold, place, caretPage, pages, &doc);
        return;
    }
    if (hold.focusPara != focusPara || hold.focusOff != focusOff) moved = true;
    hold.focusPara = focusPara;
    hold.focusOff = focusOff;
    hold.caret.hit = false;
}

void refreshPins(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages);

namespace {

std::atomic<bool> gSelectionPins{false};

}  // namespace

void rememberSelection(const model::Document& doc, const std::vector<Node>& pages) {
    bumpPointerPicture();
    PointerHold& hold = pointerHold();
    // 방향키나 끌기로 블록이 바뀌어도 마커가 새 끝을 따라간다.
    struct FollowPins {
        PointerHold& hold;
        const model::Document& doc;
        const std::vector<Node>& pages;
        ~FollowPins() {
            if (hold.pins && hold.pinDrag == 0) refreshPins(hold, doc, pages);
        }
    } follow{hold, doc, pages};
    hold.boxes.clear();
    hold.selectedText.clear();
    if (hold.drag.mode == DragMode::Cells && hold.drag.control && hold.drag.focusRow >= 0 && hold.drag.focusCol >= 0) {
        const int row0 = std::min(hold.drag.row, hold.drag.focusRow);
        const int col0 = std::min(hold.drag.col, hold.drag.focusCol);
        const int row1 = std::max(hold.drag.row + hold.drag.rowSpan - 1, hold.drag.focusRow + hold.drag.focusRowSpan - 1);
        const int col1 = std::max(hold.drag.col + hold.drag.colSpan - 1, hold.drag.focusCol + hold.drag.focusColSpan - 1);
        for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
            collectCellBoxes(doc, pages[static_cast<std::size_t>(i)], 0, 0, i, hold.drag.control, row0, row1, col0,
                             col1, hold, pages);
        }
        hold.caret.hit = false;
        return;
    }
    const bool same = hold.anchorPara == hold.focusPara && hold.anchorOff == hold.focusOff;
    if (hold.story <= 0 || same) return;
    if (hold.drag.mode != DragMode::Object && !hold.caret.hit) return;
    const std::vector<BlockBox> boxes = blockBoxes(doc, pages, hold.story, hold.anchorPara, hold.anchorOff, hold.focusPara,
                                                    hold.focusOff);
    hold.boxes.reserve(boxes.size());
    for (const BlockBox& box : boxes) {
        if (box.page < 0 || box.page >= static_cast<int>(pages.size()) || box.width <= 0 || box.height <= 0) continue;
        const Node& page = pages[static_cast<std::size_t>(box.page)];
        if (page.width <= 0 || page.height <= 0) continue;
        HeldBox held;
        held.page = box.page;
        held.x = box.x;
        held.y = box.y;
        held.width = box.width;
        held.height = box.height;
        held.pageW = viewWidthOf(page);
        held.pageH = page.height;
        held.invert = true;
        held.backR = box.backR;
        held.backG = box.backG;
        held.backB = box.backB;
        hold.boxes.push_back(held);
    }
}

PointerView pointerViewOf(const PointerHold& hold) {
    PointerView view;
    view.active = hold.caret.hit;
    view.story = hold.story;
    view.caret = hold.caret.hit;
    return view;
}

std::string codeLabel(bool has, char32_t cp) {
    if (!has) return "없음";
    if (cp == U' ') return "띄어쓰기";
    if (cp == U'\t') return "탭";
    if (cp == U'\n' || cp == U'\r') return "줄바꿈";
    if (cp == 0x00A0) return "묶음빈칸";
    if (cp == 0x2007) return "고정폭빈칸";
    if (cp < 0x20 || cp == 0x7F) return "제어 " + std::to_string(static_cast<unsigned>(cp));
    std::string out;
    appendUtf8(out, cp);
    return out;
}

std::string pageNo(int page) {
    if (page < 0) return "없음";
    return std::to_string(page + 1);
}

std::string pointText(int page, int x, int y) {
    return "쪽 " + pageNo(page) + " 좌표 " + std::to_string(x) + "," + std::to_string(y);
}

std::string gapText(int offset) {
    return "오프셋 " + std::to_string(offset) + "과 " + std::to_string(offset + 1) + " 사이";
}

std::string caretSpot(const CaretPlace& place, int page) {
    const int shown = place.page >= 0 ? place.page : page;
    return "쪽 " + pageNo(shown) + " " + std::to_string(place.x) + "," + std::to_string(place.y) + " 문단 " +
           std::to_string(place.paragraph) + " " + gapText(place.offset) + " 앞 " +
           codeLabel(place.hasBefore, place.before) + " 뒤 " + codeLabel(place.hasAfter, place.after);
}

std::string rangeText(const PointerHold& hold) {
    if (hold.story <= 0 || (hold.anchorPara == hold.focusPara && hold.anchorOff == hold.focusOff)) return "선택 없음";
    std::string text = "시작 문단 " + std::to_string(hold.anchorPara) + " " + gapText(hold.anchorOff) + " 끝 문단 " +
                       std::to_string(hold.focusPara) + " " + gapText(hold.focusOff);
    if (hold.anchorPara == hold.focusPara) {
        const int first = std::min(hold.anchorOff, hold.focusOff) + 1;
        const int last = std::max(hold.anchorOff, hold.focusOff);
        text += " " + std::to_string(first) + "번째부터 " + std::to_string(last) + "번째";
    }
    return text;
}

enum class ClickZone { None = 0, Left = 1, Line = 2, Top = 3, Right = 4, Bottom = 5, Object = 6, Header = 7, Footer = 8 };

struct LineRef {
    const Node* node = nullptr;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    bool turned = false;  // 세로쓰기 본문의 줄. 상자는 종이 자리로 돌려 두었다
};

struct ObjRef {
    const Node* node = nullptr;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
};

struct PaperHit {
    ClickZone zone = ClickZone::None;
    LineRef line;
    std::vector<ObjRef> objects;
    int pick = -1;  // 고를 개체. objects 안의 번호. -1 이면 바깥 개체부터 차례로 들어간다
    int bandStory = 0;
    bool header = false;
};

int lineBoxH(const Node& node) {
    if (node.height > 0) return node.height;
    if (node.lineData().textHeight > 0) return node.lineData().textHeight;
    return std::max<std::int32_t>(node.fontSize, 1);
}

bool boxHas(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, int px, int py) {
    return w > 0 && h > 0 && px >= x && py >= y && px < x + w && py < y + h;
}

std::int32_t childX(const Node& node, std::int32_t ox) {
    return ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
}

int firstStory(const Node& node) {
    if (node.story > 0 && (node.kind == NodeKind::Line || node.kind == NodeKind::Column || node.kind == NodeKind::Paragraph)) {
        return node.story;
    }
    for (const Node& child : node.children) {
        const int found = firstStory(child);
        if (found > 0) return found;
    }
    return node.story > 0 ? node.story : 0;
}

bool lineUnder(const Node& node, std::int32_t ox, std::int32_t oy, int px, int py, LineRef& out) {
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Line) {
        const std::int32_t w = std::max<std::int32_t>(node.width, 1);
        const std::int32_t h = lineBoxH(node);
        if (!boxHas(x, y, w, h, px, py)) return false;
        out = {&node, x, y, w, h};
        return true;
    }
    for (int i = static_cast<int>(node.children.size()) - 1; i >= 0; --i) {
        if (lineUnder(node.children[static_cast<std::size_t>(i)], x, y, px, py, out)) return true;
    }
    return false;
}

// 세로쓰기 틀을 지나는 동안의 틀 원점·폭과 종이 점. 틀 안은 돌린 점으로 재고, 세운 개체를 만나면 종이로 돌아간다.
struct HitTurn {
    const Node* node = nullptr;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t span = 0;
    int px = 0;
    int py = 0;
};
thread_local HitTurn gHitTurn;
thread_local std::int32_t gHitPaperW = 0;
struct HitTurnScope {
    HitTurn saved = gHitTurn;
    ~HitTurnScope() { gHitTurn = saved; }
};

bool objectUnder(const Node& node, std::int32_t ox, std::int32_t oy, int px, int py, bool allowBehind,
                 const model::Document* doc, std::vector<ObjRef>& chain, LineRef& line);

// 돌린 틀 안의 세운 개체 상자(틀 자리 bx, by). 실제 개체를 종이 자리와 종이 점으로 고른다.
bool uprightUnder(const Node& box, std::int32_t bx, std::int32_t by, bool allowBehind, const model::Document* doc,
                  std::vector<ObjRef>& chain, LineRef& line) {
    const HitTurnScope scope;
    const HitTurn turn = gHitTurn;
    const std::int32_t paperX = turn.x + turn.span - by - box.height;
    const std::int32_t paperY = turn.y + bx;
    gHitTurn = {};
    for (int i = static_cast<int>(box.children.size()) - 1; i >= 0; --i) {
        if (objectUnder(box.children[static_cast<std::size_t>(i)], paperX, paperY, turn.px, turn.py, allowBehind, doc,
                        chain, line)) {
            return true;
        }
    }
    return false;
}

bool objectUnder(const Node& node, std::int32_t ox, std::int32_t oy, int px, int py, bool allowBehind,
                 const model::Document* doc, std::vector<ObjRef>& chain, LineRef& line) {
    if (node.kind == NodeKind::Paper) gHitPaperW = node.width;
    if (node.turned != 0 && gHitTurn.node != &node) {
        const HitTurnScope scope;
        const std::int32_t span = node.turnSpan > 0 ? node.turnSpan : gHitPaperW;
        gHitTurn = {&node, ox, oy, span, px, py};
        return objectUnder(node, 0, 0, py - oy, ox + span - px, allowBehind, doc, chain, line);
    }
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.pageBand || node.role == "memos" || node.role == "lineNumbers" || node.role == "memos") return false;
    if (node.upright && gHitTurn.node) {
        if (!boxHas(x, y, node.width, node.height, px, py)) return false;
        return uprightUnder(node, x, y, allowBehind, doc, chain, line);
    }
    if (node.kind == NodeKind::Line) {
        // 글자처럼 넣은 개체는 줄의 x가 화면 위치가 아니다. 그리는 walkPlacedChildren 과 같은 자리를 쓴다.
        struct PlacedChild {
            const Node* node = nullptr;
            std::int32_t x = 0;
            std::int32_t y = 0;
        };
        std::vector<PlacedChild> placed;
        walkPlacedChildren(node, x, y, doc, [&](const Node& child, std::int32_t cx, std::int32_t cy) {
            placed.push_back({&child, cx, cy});
        });
        for (int i = static_cast<int>(placed.size()) - 1; i >= 0; --i) {
            const Node& child = *placed[static_cast<std::size_t>(i)].node;
            // 덧말·글자 겹치기는 글자 칸이다. 누르면 개체를 고르지 않고 캐럿을 둔다.
            if (child.kind != NodeKind::Object || child.role == "caption" || child.role == "pageBorder" ||
                child.role == "pageFill" || child.role == "glyphGroup") {
                continue;
            }
            if (child.ext().obj().textWrap == "BEHIND_TEXT" && !allowBehind) continue;
            const std::int32_t hx = placed[static_cast<std::size_t>(i)].x + child.x;
            const std::int32_t hy = placed[static_cast<std::size_t>(i)].y + child.y;
            if (!boxHas(hx, hy, child.width, child.height, px, py)) continue;
            if (child.upright && gHitTurn.node) {
                if (uprightUnder(child, hx, hy, true, doc, chain, line)) return true;
                continue;
            }
            std::vector<ObjRef> nested;
            LineRef nestedLine;
            for (int j = static_cast<int>(child.children.size()) - 1; j >= 0; --j) {
                if (objectUnder(child.children[static_cast<std::size_t>(j)], hx, hy, px, py, true, doc, nested,
                                nestedLine)) {
                    chain.push_back({&child, hx, hy, child.width, child.height});
                    chain.insert(chain.end(), nested.begin(), nested.end());
                    line = nestedLine;
                    return true;
                }
            }
            chain.push_back({&child, hx, hy, child.width, child.height});
            for (int j = static_cast<int>(child.children.size()) - 1; j >= 0; --j) {
                if (lineUnder(child.children[static_cast<std::size_t>(j)], hx, hy, px, py, line)) break;
            }
            return true;
        }
        return false;
    }
    if (node.kind == NodeKind::Object && node.role != "caption" && node.role != "pageBorder" && node.role != "pageFill") {
        const bool behind = node.ext().obj().textWrap == "BEHIND_TEXT";
        if (behind && !allowBehind) return false;
        if (!boxHas(x, y, node.width, node.height, px, py)) return false;
        for (int i = static_cast<int>(node.children.size()) - 1; i >= 0; --i) {
            std::vector<ObjRef> child;
            if (objectUnder(node.children[static_cast<std::size_t>(i)], x, y, px, py, true, doc, child, line)) {
                chain.push_back({&node, x, y, node.width, node.height});
                chain.insert(chain.end(), child.begin(), child.end());
                return true;
            }
        }
        chain.push_back({&node, x, y, node.width, node.height});
        lineUnder(node, ox, oy, px, py, line);
        return true;
    }
    for (int i = static_cast<int>(node.children.size()) - 1; i >= 0; --i) {
        if (objectUnder(node.children[static_cast<std::size_t>(i)], x, y, px, py, allowBehind, doc, chain, line)) return true;
    }
    return false;
}

// 글이 들어가는 개체. 표와 글상자는 테두리 띠에서만 개체를 고르고, 안쪽을 누르면 글에 커서를 둔다.
bool textHolder(const Node& node) {
    if (node.role == "table") return true;
    return node.role == "shape" && cellContentStory(node) > 0;
}

bool edgeVisible(const BorderEdge& edge) { return !edge.type.empty(); }

// 0 은 그 글 묶음이 없다, 1 은 줄을 찾았지만 담은 상자를 아직 못 정했다, 2 는 정했다.
int storyFrame(const Node& node, std::int32_t ox, std::int32_t oy, const model::Document* doc, int story,
               std::vector<PointerHold::FrameLine>& out) {
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Line && node.story == story) return 1;
    int found = 0;
    walkPlacedChildren(node, x, y, doc, [&](const Node& child, std::int32_t cx, std::int32_t cy) {
        if (!found) found = storyFrame(child, cx, cy, doc, story, out);
    });
    // 머리말·꼬리말 띠. 고치는 중인 영역을 점선 상자로 보인다.
    if (found == 1 && node.pageBand) {
        out.push_back({x, y, x + node.width, y});
        out.push_back({x, y + node.height, x + node.width, y + node.height});
        out.push_back({x, y, x, y + node.height});
        out.push_back({x + node.width, y, x + node.width, y + node.height});
        return 2;
    }
    if (found != 1 || node.kind != NodeKind::Object) return found;
    if (node.role == "caption") return 2;
    if (node.role == "shape" && textHolder(node)) {
        if (edgeVisible(node.ext().borderTop) || edgeVisible(node.ext().borderLeft)) return 2;
        out.push_back({x, y, x + node.width, y});
        out.push_back({x, y + node.height, x + node.width, y + node.height});
        out.push_back({x, y, x, y + node.height});
        out.push_back({x + node.width, y, x + node.width, y + node.height});
        return 2;
    }
    if (node.role != "table") return 1;
    // 칸마다 선이 없는 변을 긋는다. 맞닿은 칸이 그 변에 선을 그리면 그 선이 보이므로 긋지 않는다.
    struct CellRect {
        const Node* cell;
        std::int32_t l, t, r, b;
    };
    std::vector<CellRect> cells;
    for (const Node& child : node.children) {
        if (child.kind != NodeKind::Cell) continue;
        const std::int32_t cx = childX(child, x);
        const std::int32_t cy = y + child.y;
        cells.push_back({&child, cx, cy, cx + child.width, cy + child.height});
    }
    const auto overlaps = [](std::int32_t a0, std::int32_t a1, std::int32_t b0, std::int32_t b1) {
        return std::min(a1, b1) - std::max(a0, b0) > 0;
    };
    const auto shown = [&](const CellRect& self, bool vertical, std::int32_t at, std::int32_t from, std::int32_t to) {
        for (const CellRect& other : cells) {
            if (other.cell == self.cell) continue;
            if (vertical) {
                if (!overlaps(from, to, other.t, other.b)) continue;
                if (std::abs(other.r - at) <= 1 && edgeVisible(other.cell->ext().borderRight)) return true;
                if (std::abs(other.l - at) <= 1 && edgeVisible(other.cell->ext().borderLeft)) return true;
            } else {
                if (!overlaps(from, to, other.l, other.r)) continue;
                if (std::abs(other.b - at) <= 1 && edgeVisible(other.cell->ext().borderBottom)) return true;
                if (std::abs(other.t - at) <= 1 && edgeVisible(other.cell->ext().borderTop)) return true;
            }
        }
        return false;
    };
    for (const CellRect& c : cells) {
        if (!edgeVisible(c.cell->ext().borderTop) && !shown(c, false, c.t, c.l, c.r)) out.push_back({c.l, c.t, c.r, c.t});
        if (!edgeVisible(c.cell->ext().borderBottom) && !shown(c, false, c.b, c.l, c.r)) out.push_back({c.l, c.b, c.r, c.b});
        if (!edgeVisible(c.cell->ext().borderLeft) && !shown(c, true, c.l, c.t, c.b)) out.push_back({c.l, c.t, c.l, c.b});
        if (!edgeVisible(c.cell->ext().borderRight) && !shown(c, true, c.r, c.t, c.b)) out.push_back({c.r, c.t, c.r, c.b});
    }
    return 2;
}

void caretFrameLines(const Node& paper, const model::Document* doc, int story,
                     std::vector<PointerHold::FrameLine>& out) {
    storyFrame(paper, 0, 0, doc, story, out);
    // 맞닿은 칸은 같은 변을 따로 긋는다. 두 점선이 겹치면 점과 틈이 엇갈려 실선처럼 보이므로 한 줄로 합친다.
    using Line = PointerHold::FrameLine;
    std::vector<Line> horizontal;
    std::vector<Line> vertical;
    for (const Line& line : out) (line.y0 == line.y1 ? horizontal : vertical).push_back(line);
    const auto merge = [](std::vector<Line>& lines, bool across) {
        // across 가 참이면 가로선이다. 같은 높이에서 x 구간이 닿거나 겹치면 합친다.
        const auto at = [&](const Line& l) { return across ? l.y0 : l.x0; };
        const auto from = [&](const Line& l) { return across ? l.x0 : l.y0; };
        std::sort(lines.begin(), lines.end(), [&](const Line& a, const Line& b) {
            return at(a) != at(b) ? at(a) < at(b) : from(a) < from(b);
        });
        std::vector<Line> joined;
        for (const Line& line : lines) {
            if (!joined.empty()) {
                Line& last = joined.back();
                if (at(last) == at(line) && from(line) <= (across ? last.x1 : last.y1)) {
                    if (across) last.x1 = std::max(last.x1, line.x1);
                    else last.y1 = std::max(last.y1, line.y1);
                    continue;
                }
            }
            joined.push_back(line);
        }
        lines = std::move(joined);
    };
    merge(horizontal, true);
    merge(vertical, false);
    out = std::move(horizontal);
    out.insert(out.end(), vertical.begin(), vertical.end());
}

bool inEdgeBand(const ObjRef& obj, int px, int py, double band) {
    if (band <= 0) return false;
    return px - obj.x < band || obj.x + obj.w - px <= band || py - obj.y < band || obj.y + obj.h - py <= band;
}

void gatherBodyLines(const Node& node, std::int32_t ox, std::int32_t oy, std::vector<LineRef>& out,
                     std::int32_t paperW = 0, std::int32_t turnW = 0);
LineRef nearestLine(const std::vector<LineRef>& lines, int px, int py);

// 표·글상자 안쪽에서 누른 자리에 가까운 줄. 표는 누른 칸의 줄만 본다.
LineRef holderLine(const ObjRef& obj, int px, int py) {
    std::vector<LineRef> lines;
    const Node& node = *obj.node;
    if (node.role == "table") {
        for (const Node& cell : node.children) {
            if (cell.kind != NodeKind::Cell) continue;
            if (boxHas(childX(cell, obj.x), obj.y + cell.y, cell.width, cell.height, px, py)) {
                gatherBodyLines(cell, obj.x, obj.y, lines);
                break;
            }
        }
    }
    if (lines.empty()) {
        for (const Node& child : node.children) gatherBodyLines(child, obj.x, obj.y, lines);
    }
    return nearestLine(lines, px, py);
}

// 개체 위를 누른 자리를 정한다. 가장 안쪽 개체를 본다.
// 그림·도형·수식은 상자 어디를 눌러도 고른다. 표와 글상자는 테두리 띠에서만 고르고, 안쪽은 가까운 줄에 커서를 둔다.
// 글자 줄 위는 띠 안이라도 글이다. 띠는 칸 안여백처럼 줄이 없는 테두리 곁에서만 개체를 고른다.
void settleObjectHit(PaperHit& hit, int px, int py, double band, const model::Document& doc) {
    if (hit.objects.empty() || !hit.objects.back().node) return;
    const ObjRef& inner = hit.objects.back();
    const int last = static_cast<int>(hit.objects.size()) - 1;
    // 글상자는 줄 상자가 폭을 다 차지해도 글자가 없는 테두리 곁이 개체를 고르는 띠다.
    // 표는 칸 안 여백도 글이라 줄 위면 커서다. 표는 테두리선 위를 눌러 고른다(tableBorderAt).
    const bool edge = inEdgeBand(inner, px, py, band);
    const bool onInk = hit.line.node &&
                       (inner.node->role == "table" ||
                        lineInkHas(doc, *hit.line.node, hit.line.x, hit.line.y, px, static_cast<std::int32_t>(band)));
    if (!textHolder(*inner.node) || (edge && !onInk)) {
        hit.zone = ClickZone::Object;
        hit.pick = last;
        hit.line = {};
        return;
    }
    if (!hit.line.node) hit.line = holderLine(inner, px, py);
    hit.zone = hit.line.node ? ClickZone::Line : ClickZone::Object;
    if (!hit.line.node) hit.pick = last;
}

// 세로쓰기 본문(turned)의 줄 상자는 돌린 틀 좌표다. 종이 (X, Y) 는 틀의 (Y, 종이 폭 − X) 라 종이 상자로 돌린다.
LineRef paperLineRef(const LineRef& line, std::int32_t turnW) {
    if (turnW <= 0) return line;
    return {line.node, turnW - (line.y + line.h), line.x, line.h, line.w, true};
}

bool bodyLineAt(const Node& node, std::int32_t ox, std::int32_t oy, int px, int py, LineRef& out,
                std::int32_t paperW = 0, std::int32_t turnW = 0) {
    if (node.pageBand || node.kind == NodeKind::Object || node.role == "lineNumbers" || node.role == "memos") return false;
    if (node.kind == NodeKind::Paper) paperW = node.width;
    if (node.turned != 0 && turnW == 0 && paperW > 0) {
        LineRef inner;
        if (!bodyLineAt(node, oy, -ox, py, paperW - px, inner, paperW, paperW)) return false;
        out = paperLineRef(inner, paperW);
        return true;
    }
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Line) {
        const std::int32_t w = std::max<std::int32_t>(node.width, 1);
        const std::int32_t h = lineBoxH(node);
        if (!boxHas(x, y, w, h, px, py)) return false;
        out = {&node, x, y, w, h};
        return true;
    }
    for (int i = static_cast<int>(node.children.size()) - 1; i >= 0; --i) {
        if (bodyLineAt(node.children[static_cast<std::size_t>(i)], x, y, px, py, out, paperW, turnW)) return true;
    }
    return false;
}

void gatherBodyLines(const Node& node, std::int32_t ox, std::int32_t oy, std::vector<LineRef>& out,
                     std::int32_t paperW, std::int32_t turnW) {
    if (node.pageBand || node.kind == NodeKind::Object || node.role == "lineNumbers" || node.role == "memos") return;
    if (node.kind == NodeKind::Paper) paperW = node.width;
    if (node.turned != 0 && turnW == 0 && paperW > 0) {
        gatherBodyLines(node, oy, -ox, out, paperW, paperW);
        return;
    }
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Line) {
        out.push_back(paperLineRef({&node, x, y, std::max<std::int32_t>(node.width, 1), lineBoxH(node)}, turnW));
        return;
    }
    for (const Node& child : node.children) gatherBodyLines(child, x, y, out, paperW, turnW);
}

bool bandAt(const Node& node, std::int32_t ox, std::int32_t oy, int px, int py, int paperH, int& story, bool& header,
            ObjRef& box) {
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.pageBand && boxHas(x, y, std::max<std::int32_t>(node.width, 1), std::max<std::int32_t>(node.height, lineBoxH(node)), px, py)) {
        story = firstStory(node);
        header = y + std::max<std::int32_t>(node.height, 1) / 2 < paperH / 2;
        box = {&node, x, y, node.width, node.height};
        return true;
    }
    if (node.kind == NodeKind::Object) return false;
    for (const Node& child : node.children) {
        if (bandAt(child, x, y, px, py, paperH, story, header, box)) return true;
    }
    return false;
}

// 띠는 종이의 바로 아래 자식이다. 고치는 띠와 같은 글 묶음인 띠를 쪽마다 찾는다.
void fillBandAreas(PointerHold& hold, const std::vector<Node>& pages) {
    hold.bandAreas.clear();
    if (hold.bandStory <= 0 || hold.bandMaster) return;
    hold.bandAreas.resize(pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        for (const Node& child : pages[i].children) {
            if (!child.pageBand || firstStory(child) != hold.bandStory) continue;
            hold.bandAreas[i] = {childX(child, 0), child.y, child.width, std::max<std::int32_t>(child.height, lineBoxH(child))};
            break;
        }
    }
}

LineRef nearestLine(const std::vector<LineRef>& lines, int px, int py) {
    LineRef best;
    std::int64_t bestDist = 0;
    for (const LineRef& line : lines) {
        const std::int32_t midY = line.y + line.h / 2;
        const std::int32_t dy = py >= line.y && py < line.y + line.h ? 0 : std::abs(py - midY);
        const std::int32_t midX = line.x + line.w / 2;
        const std::int64_t dist = static_cast<std::int64_t>(dy) * 100000 + std::abs(px - midX);
        if (!best.node || dist < bestDist) {
            best = line;
            bestDist = dist;
        }
    }
    return best;
}

void paragraphChars(const model::Document& doc, int story, int paragraph, std::vector<char32_t>& out) {
    const model::StorySlot* slot = resolveStory(doc, story);
    if (!slot || !slot->paragraphs || paragraph < 0 || paragraph >= static_cast<int>(slot->paragraphs->size())) return;
    const model::Paragraph& para = (*slot->paragraphs)[static_cast<std::size_t>(paragraph)];
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type != model::InlineItem::Type::Text) {
                out.push_back(0xFFFC);
                continue;
            }
            std::size_t index = 0;
            char32_t cp = 0;
            while (nextCp(item.text, index, cp)) out.push_back(cp);
        }
    }
}

bool gapChar(char32_t cp) { return cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' || cp == 0x00A0; }

void wordBounds(const std::vector<char32_t>& text, int offset, int& begin, int& end) {
    begin = 0;
    end = 0;
    if (text.empty()) return;
    int index = std::clamp(offset, 0, static_cast<int>(text.size()) - 1);
    if (offset >= static_cast<int>(text.size()) && offset > 0) index = offset - 1;
    const bool gap = gapChar(text[static_cast<std::size_t>(index)]);
    begin = index;
    while (begin > 0 && gapChar(text[static_cast<std::size_t>(begin - 1)]) == gap) --begin;
    end = index + 1;
    while (end < static_cast<int>(text.size()) && gapChar(text[static_cast<std::size_t>(end)]) == gap) ++end;
}

void rangeOf(const std::vector<Node>& pages, int story, int paragraph, bool wholeStory, int& beginPara, int& beginOff, int& endPara, int& endOff) {
    beginPara = paragraph;
    beginOff = 0;
    endPara = paragraph;
    endOff = 0;
    bool any = false;
    const auto walk = [&](const auto& self, const Node& node) -> void {
        if (node.kind == NodeKind::Line && node.story == story && (wholeStory || node.paragraph == paragraph)) {
            if (!any || node.paragraph < beginPara || (node.paragraph == beginPara && node.start < beginOff)) {
                beginPara = node.paragraph;
                beginOff = std::max(0, node.start);
            }
            if (!any || node.paragraph > endPara || (node.paragraph == endPara && node.end > endOff)) {
                endPara = node.paragraph;
                endOff = std::max(endOff, node.end);
            }
            any = true;
        }
        for (const Node& child : node.children) self(self, child);
    };
    for (const Node& page : pages) walk(walk, page);
}

bool cornerOf(const ObjRef& obj, int px, int py, double radius) {
    if (radius <= 0 || obj.w <= 0 || obj.h <= 0) return false;
    const double r = radius;
    auto near = [&](std::int32_t cx, std::int32_t cy) {
        return std::abs(px - cx) <= r && std::abs(py - cy) <= r;
    };
    return near(obj.x, obj.y) || near(obj.x + obj.w, obj.y) || near(obj.x, obj.y + obj.h) || near(obj.x + obj.w, obj.y + obj.h);
}

void showObject(PointerHold& hold, const std::vector<Node>& pages, int page, const ObjRef& obj) {
    hold.boxes.clear();
    hold.selectedText.clear();
    hold.pins = false;
    hold.pinDrag = 0;
    hold.story = 0;
    hold.caret.hit = false;
    hold.selectedObject = obj.node;
    hold.objectHandles = true;
    hold.resizeHandle = -1;
    hold.tracking = false;
    if (page >= 0 && page < static_cast<int>(pages.size())) {
        hold.caretPage = page;
        hold.caretPageW = viewWidthOf(pages[static_cast<std::size_t>(page)]);
        hold.caretPageH = pages[static_cast<std::size_t>(page)].height;
    }
    if (page >= 0 && page < static_cast<int>(pages.size())) {
        HeldBox held;
        held.page = page;
        held.x = obj.x;
        held.y = obj.y;
        held.width = obj.w;
        held.height = obj.h;
        held.pageW = viewWidthOf(pages[static_cast<std::size_t>(page)]);
        held.pageH = pages[static_cast<std::size_t>(page)].height;
        // 고른 개체는 색을 뒤집지 않는다. 상자 둘레의 손잡이 여덟 개로만 보인다.
        held.invert = false;
        hold.boxes.push_back(held);
    }
    bumpPointerPicture();
}

bool reselectAnchoredObject(const model::Document& doc, const std::vector<Node>& pages, int page, int story,
                            int paragraph, int item) {
    if (page < 0 || page >= static_cast<int>(pages.size())) return false;
    ObjRef found;
    auto visit = [&](const Node& node, std::int32_t x, std::int32_t y) {
        if (found.node || node.turned != 0) return false;
        if (node.kind == NodeKind::Object && node.ext().obj().anchorStory == story && node.ext().obj().anchorParagraph == paragraph &&
            node.ext().obj().anchorItem == item) {
            found = ObjRef{&node, x, y, node.width, node.height};
            return false;
        }
        return true;
    };
    walkPlacedNodes(doc, pages[static_cast<std::size_t>(page)], 0, 0, visit);
    if (!found.node) return false;
    showObject(pointerHold(), pages, page, found);
    return true;
}

void refreshPins(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages) {
    const PointerCursor cur = [&] {
        PointerCursor out;
        if (hold.story <= 0 || !hold.caret.hit) return out;
        out.startParagraph = hold.anchorPara;
        out.startOffset = hold.anchorOff;
        out.endParagraph = hold.focusPara;
        out.endOffset = hold.focusOff;
        if (out.endParagraph < out.startParagraph ||
            (out.endParagraph == out.startParagraph && out.endOffset < out.startOffset)) {
            std::swap(out.startParagraph, out.endParagraph);
            std::swap(out.startOffset, out.endOffset);
        }
        out.story = hold.story;
        out.hasSelection = out.startParagraph != out.endParagraph || out.startOffset != out.endOffset;
        return out;
    }();
    if (!cur.hasSelection) {
        hold.pins = false;
        return;
    }
    hold.pinStart = caretForCursor(doc, pages, model::CursorPos{hold.story, cur.startParagraph, cur.startOffset});
    hold.pinEnd = caretForCursor(doc, pages, model::CursorPos{hold.story, cur.endParagraph, cur.endOffset});
}

void selectCaret(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages, int page, const CaretPlace& place) {
    hold.selectedObject = nullptr;
    hold.objectHandles = false;
    hold.resizeHandle = -1;
    hold.pins = false;
    hold.pinDrag = 0;
    hold.tracking = true;
    hold.preferredX = -1;
    hold.story = place.story;
    hold.anchorPara = place.paragraph;
    hold.anchorOff = place.offset;
    hold.focusPara = place.paragraph;
    hold.focusOff = place.offset;
    hold.selectedText.clear();
    clearComposition(hold);
    armDragHost(hold, doc, pages, page, place.x, place.y, place);
    placeCaret(hold, place, place.page >= 0 ? place.page : page, pages, &doc);
    rememberSelection(doc, pages);
}

void selectSpan(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages, int page, int story, int paraA, int offA, int paraB, int offB) {
    hold.selectedObject = nullptr;
    hold.objectHandles = false;
    hold.resizeHandle = -1;
    hold.pinDrag = 0;
    hold.tracking = true;
    hold.preferredX = -1;
    hold.story = story;
    hold.anchorPara = paraA;
    hold.anchorOff = offA;
    hold.focusPara = paraB;
    hold.focusOff = offB;
    hold.selectedText.clear();
    clearComposition(hold);
    const CaretPlace place = caretForCursor(doc, pages, model::CursorPos{story, paraB, offB});
    if (place.hit) placeCaret(hold, place, place.page >= 0 ? place.page : page, pages, &doc);
    rememberSelection(doc, pages);
    if (gSelectionPins.load(std::memory_order_relaxed)) {
        hold.pins = paraA != paraB || offA != offB;
        if (hold.pins) refreshPins(hold, doc, pages);
    }
}

// 머리말·꼬리말 띠 안의 모든 줄. 띠 안 개체(묶음·글상자·표) 속 줄도 넣는다.
// 머리말 문단은 비어 있고 글이 글상자 안에만 있는 문서가 많다.
void gatherBandLines(const Node& node, std::int32_t ox, std::int32_t oy, const model::Document* doc,
                     std::vector<LineRef>& out) {
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Line) {
        if (node.story > 0) out.push_back({&node, x, y, std::max<std::int32_t>(node.width, 1), lineBoxH(node)});
        // 글자처럼 넣은 개체는 그리는 자리(walkPlacedChildren)에서 그 안 줄을 찾는다.
        walkPlacedChildren(node, x, y, doc, [&](const Node& child, std::int32_t cx, std::int32_t cy) {
            if (child.kind == NodeKind::Object) gatherBandLines(child, cx, cy, doc, out);
        });
        return;
    }
    for (const Node& child : node.children) gatherBandLines(child, x, y, doc, out);
}

// 띠 안에서 누른 줄. 누른 자리를 품은 줄 중 가장 작은 것(가장 안쪽 글상자)을 먼저 고르고, 없으면 가까운 줄이다.
LineRef bandLineAt(const ObjRef& band, int px, int py, const model::Document* doc) {
    std::vector<LineRef> lines;
    if (!band.node) return {};
    for (const Node& child : band.node->children) gatherBandLines(child, band.x, band.y, doc, lines);
    const LineRef* inside = nullptr;
    for (const LineRef& line : lines) {
        if (!boxHas(line.x, line.y, line.w, line.h, px, py)) continue;
        if (!inside || static_cast<std::int64_t>(line.w) * line.h <
                           static_cast<std::int64_t>(inside->w) * inside->h) {
            inside = &line;
        }
    }
    if (inside) return *inside;
    return nearestLine(lines, px, py);
}

// 그 쪽에서 story 띠의 글 줄 가운데 누른 자리에 가장 가까운 것. 띠 안 글상자 속 줄은 고르지 않는다.
LineRef bandStoryLineNear(const Node& paper, int story, int px, int py, const model::Document* doc) {
    std::vector<LineRef> lines;
    for (const Node& child : paper.children) {
        if (!child.pageBand || firstStory(child) != story) continue;
        std::vector<LineRef> all;
        gatherBandLines(child, 0, 0, doc, all);
        for (const LineRef& line : all) {
            if (line.node && line.node->story == story) lines.push_back(line);
        }
    }
    return nearestLine(lines, px, py);
}

// 바탕쪽 편집 중 누른 자리: 바탕쪽 개체(위에서부터), 아니면 그 바탕쪽 글 줄 가운데 가장 가까운 것이다.
PaperHit hitMaster(const model::Document& doc, const Node& paper, int px, int py, int story, double edgeBand) {
    PaperHit hit;
    for (int i = static_cast<int>(paper.children.size()) - 1; i >= 0; --i) {
        const Node& child = paper.children[static_cast<std::size_t>(i)];
        if (!child.fromMaster || child.kind != NodeKind::Object) continue;
        std::vector<ObjRef> objs;
        LineRef objLine;
        if (objectUnder(child, 0, 0, px, py, true, &doc, objs, objLine)) {
            if (objs.empty()) continue;
            hit.objects = std::move(objs);
            hit.line = objLine;
            settleObjectHit(hit, px, py, edgeBand, doc);
            hit.bandStory = story;
            return hit;
        }
    }
    // 바탕쪽은 쪽마다 별칭 글 묶음이다. 고치는 바탕쪽과 같은 바탕쪽의 글 줄만 고른다(그 안 글상자·표 글은 빼고
    // 바탕쪽 글 자체의 줄). 겹친 다른 바탕쪽 글은 고르지 않는다.
    const model::StorySlot* edited = resolveStory(doc, story);
    const model::MasterPage* master = edited ? edited->masterPage : nullptr;
    std::vector<LineRef> own;
    for (const Node& child : paper.children) {
        if (!child.fromMaster) continue;
        std::vector<LineRef> all;
        gatherBandLines(child, 0, 0, &doc, all);
        for (const LineRef& line : all) {
            const model::StorySlot* slot = line.node && line.node->story > 0 ? resolveStory(doc, line.node->story) : nullptr;
            if (slot && slot->kind == model::StoryKind::MasterPage && slot->masterPage == master) own.push_back(line);
        }
    }
    const LineRef* inside = nullptr;
    for (const LineRef& line : own) {
        if (!boxHas(line.x, line.y, line.w, line.h, px, py)) continue;
        if (!inside || static_cast<std::int64_t>(line.w) * line.h < static_cast<std::int64_t>(inside->w) * inside->h) {
            inside = &line;
        }
    }
    hit.line = inside ? *inside : nearestLine(own, px, py);
    if (hit.line.node) {
        hit.zone = ClickZone::Line;
        hit.bandStory = story;
    }
    return hit;
}

PaperHit hitPaper(const model::Document& doc, const Node& paper, int px, int py, int editBand, double edgeBand) {
    if (editBand > 0 && pointerHold().bandMaster) return hitMaster(doc, paper, px, py, editBand, edgeBand);
    PaperHit hit;
    std::vector<ObjRef> front;
    LineRef frontLine;
    objectUnder(paper, 0, 0, px, py, false, &doc, front, frontLine);
    LineRef body;
    const bool onBody = bodyLineAt(paper, 0, 0, px, py, body);
    int bandStory = 0;
    bool header = false;
    ObjRef band;
    const bool onBand = bandAt(paper, 0, 0, px, py, paper.height, bandStory, header, band);
    // 머리말·꼬리말을 고치는 중이면 그 띠 안은 글이다. 가장 가까운 줄에 커서를 둔다.
    if (onBand && editBand > 0 && bandStory == editBand && band.node) {
        // 띠 안 개체는 본문 개체처럼 고른다. 테두리 띠는 개체, 글자 줄 위는 커서다.
        std::vector<ObjRef> inBand;
        LineRef inBandLine;
        for (int i = static_cast<int>(band.node->children.size()) - 1; i >= 0; --i) {
            if (objectUnder(band.node->children[static_cast<std::size_t>(i)], band.x, band.y, px, py, true, &doc,
                            inBand, inBandLine)) {
                break;
            }
        }
        if (!inBand.empty()) {
            hit.objects = std::move(inBand);
            hit.line = inBandLine;
            settleObjectHit(hit, px, py, edgeBand, doc);
            hit.bandStory = bandStory;
            hit.header = header;
            return hit;
        }
        hit.line = bandLineAt(band, px, py, &doc);
        if (hit.line.node) {
            hit.zone = ClickZone::Line;
            hit.bandStory = bandStory;
            hit.header = header;
            return hit;
        }
    }
    if (!front.empty()) {
        hit.objects = std::move(front);
        hit.line = frontLine;
        settleObjectHit(hit, px, py, edgeBand, doc);
        return hit;
    }
    if (onBody) {
        hit.line = body;
        hit.zone = ClickZone::Line;
        return hit;
    }
    std::int32_t bodyX = paper.marginLeft;
    std::int32_t bodyY = paper.marginTop;
    std::int32_t bodyR = paper.width - paper.marginRight;
    std::int32_t bodyB = paper.height - paper.marginBottom;
    for (const Node& child : paper.children) {
        if (child.kind != NodeKind::Body) continue;
        bodyX = child.x;
        bodyY = child.y;
        bodyR = child.x + child.width;
        bodyB = child.y + child.height;
        break;
    }
    if (px < bodyX) {
        hit.zone = ClickZone::Left;
        return hit;
    }
    if (onBand) {
        hit.bandStory = bandStory;
        hit.header = header;
        hit.zone = header ? ClickZone::Header : ClickZone::Footer;
        // 두 번 눌러 들어갈 때 누른 줄(글상자 속 줄일 수 있다)에 커서를 둔다.
        hit.line = bandLineAt(band, px, py, &doc);
        return hit;
    }
    std::vector<ObjRef> behind;
    LineRef behindLine;
    if (objectUnder(paper, 0, 0, px, py, true, &doc, behind, behindLine) && !behind.empty() && behind.front().node &&
        behind.front().node->ext().obj().textWrap == "BEHIND_TEXT") {
        hit.objects = std::move(behind);
        hit.line = behindLine;
        settleObjectHit(hit, px, py, edgeBand, doc);
        return hit;
    }
    if (py < bodyY) hit.zone = ClickZone::Top;
    else if (px >= bodyR) hit.zone = ClickZone::Right;
    else if (py >= bodyB) hit.zone = ClickZone::Bottom;
    else hit.zone = ClickZone::Bottom;
    return hit;
}

bool pinHit(const PointerHold& hold, int px, int py, double radius, int& which) {
    if (!hold.pins || radius <= 0) return false;
    auto near = [&](const CaretPlace& pin) {
        if (!pin.hit) return false;
        const double cy = pin.y + std::max<std::int32_t>(pin.height, 1) / 2.0;
        return std::abs(px - pin.x) <= radius && std::abs(py - cy) <= radius + pin.height;
    };
    if (near(hold.pinStart)) {
        which = 1;
        return true;
    }
    if (near(hold.pinEnd)) {
        which = 2;
        return true;
    }
    return false;
}

const ObjRef* enteredNext(const std::vector<ObjRef>& objects, const Node* entered) {
    if (!entered) return objects.empty() ? nullptr : &objects.front();
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].node == entered) {
            if (i + 1 < objects.size()) return &objects[i + 1];
            return &objects[i];
        }
    }
    return objects.empty() ? nullptr : &objects.front();
}

void applyZone(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages, int page, int px, int py,
               int count, const PaperHit& hit, double cornerR) {
    const Node& paper = pages[static_cast<std::size_t>(page)];
    std::vector<LineRef> lines;
    gatherBodyLines(paper, 0, 0, lines);
    const auto edgeCaret = [&](ClickZone zone) {
        const LineRef line = nearestLine(lines, px, py);
        if (!line.node) return;
        bool start = zone == ClickZone::Top || zone == ClickZone::Left || zone == ClickZone::Header;
        if (zone == ClickZone::Right) start = false;
        if (zone == ClickZone::Bottom || zone == ClickZone::Footer) start = px < line.x + line.w / 2;
        const int off = start ? std::max(0, line.node->start) : std::max(0, line.node->end);
        const CaretPlace place = caretForCursor(doc, pages, model::CursorPos{line.node->story, line.node->paragraph, off});
        if (!place.hit) return;
        selectCaret(hold, doc, pages, page, place);
        hold.eventText = start ? "줄 맨 앞" : "줄 맨 뒤";
    };
    if (count == 1 && hit.zone == ClickZone::Object && !hit.objects.empty()) {
        const ObjRef* target = hit.pick >= 0 && hit.pick < static_cast<int>(hit.objects.size())
                                   ? &hit.objects[static_cast<std::size_t>(hit.pick)]
                                   : enteredNext(hit.objects, hold.selectedObject);
        if (hold.selectedObject && target && target->node == hold.selectedObject && hit.line.node) {
            const CaretPlace place = caretOnPage(doc, paper, px, py);
            if (place.hit) {
                selectCaret(hold, doc, pages, page, place);
                hold.editStory = place.story;
                hold.eventText = "커서";
                return;
            }
        }
        if (target) {
            showObject(hold, pages, page, *target);
            hold.eventText = "개체 " + target->node->role;
        }
        return;
    }
    if (hit.zone == ClickZone::Line && hit.line.node) {
        if (count <= 1) {
            CaretPlace place = caretInStory(doc, pages, page, px, py, hit.line.node->story);
            if (!place.hit) place = caretOnPage(doc, paper, px, py);
            if (place.hit) {
                selectCaret(hold, doc, pages, page, place);
                hold.eventText = "커서";
            }
            return;
        }
        // 덧말·글자 겹치기 위를 두 번 누르면 그 글자 하나를 고르고 고치기 대화 상자를 연다(한글 고치기-덧말).
        if (count == 2 && !hit.line.turned) {
            const Node* group = nullptr;
            walkPlacedChildren(*hit.line.node, hit.line.x, hit.line.y, &doc,
                               [&](const Node& child, std::int32_t cx, std::int32_t cy) {
                                   if (child.kind == NodeKind::Object && child.role == "glyphGroup" &&
                                       boxHas(cx + child.x, cy + child.y, child.width, child.height, px, py)) {
                                       group = &child;
                                   }
                               });
            const model::Paragraph* para =
                group ? storyParagraph(doc, hit.line.node->story, hit.line.node->paragraph) : nullptr;
            const int item = group ? group->ext().obj().anchorItem : -1;
            if (para && item >= 0) {
                int ordinal = 0;
                int charIndex = 0;
                const model::Control* found = nullptr;
                for (const model::Run& run : para->runs) {
                    for (const model::InlineItem& entry : run.items) {
                        if (ordinal++ == item) {
                            if (entry.type == model::InlineItem::Type::Control) found = &entry.control;
                            break;
                        }
                        if (entry.type == model::InlineItem::Type::Text) charIndex += utf8CodePointCount(entry.text);
                        else if (isCaretCharControl(entry.control)) ++charIndex;
                    }
                    if (found || ordinal > item) break;
                }
                if (found && (found->kind == model::ControlKind::Dutmal || found->kind == model::ControlKind::Compose)) {
                    const int story = hit.line.node->story;
                    const int paraIdx = hit.line.node->paragraph;
                    selectSpan(hold, doc, pages, page, story, paraIdx,
                               charIndexToOffset(doc, pages, story, paraIdx, charIndex), paraIdx,
                               charIndexToOffset(doc, pages, story, paraIdx, charIndex + 1));
                    hold.openDialog = found->kind == model::ControlKind::Dutmal ? "rubyEdit" : "composeEdit";
                    hold.eventText = "덧말·겹치기 고치기";
                    return;
                }
            }
        }
        std::vector<char32_t> chars;
        paragraphChars(doc, hit.line.node->story, hit.line.node->paragraph, chars);
        if (count == 2) {
            int begin = 0;
            int end = 0;
            wordBounds(chars, std::max(0, hit.line.node->start), begin, end);
            const CaretPlace probe = caretOnPage(doc, paper, px, py);
            if (probe.hit) wordBounds(chars, probe.offset > 0 && probe.offset >= static_cast<int>(chars.size()) ? probe.offset - 1 : probe.offset, begin, end);
            selectSpan(hold, doc, pages, page, hit.line.node->story, hit.line.node->paragraph, begin, hit.line.node->paragraph, end);
            hold.eventText = "어절";
            return;
        }
        int paraA = 0, offA = 0, paraB = 0, offB = 0;
        rangeOf(pages, hit.line.node->story, hit.line.node->paragraph, false, paraA, offA, paraB, offB);
        selectSpan(hold, doc, pages, page, hit.line.node->story, paraA, offA, paraB, offB);
        hold.eventText = "문단";
        return;
    }
    if (hit.zone == ClickZone::Left) {
        const LineRef line = nearestLine(lines, px, py);
        if (!line.node) return;
        if (count >= 3) {
            int paraA = 0, offA = 0, paraB = 0, offB = 0;
            rangeOf(pages, line.node->story, line.node->paragraph, true, paraA, offA, paraB, offB);
            selectSpan(hold, doc, pages, page, line.node->story, paraA, offA, paraB, offB);
            hold.eventText = "본문";
            return;
        }
        if (count == 2) {
            int paraA = 0, offA = 0, paraB = 0, offB = 0;
            rangeOf(pages, line.node->story, line.node->paragraph, false, paraA, offA, paraB, offB);
            selectSpan(hold, doc, pages, page, line.node->story, paraA, offA, paraB, offB);
            hold.eventText = "문단";
            return;
        }
        selectSpan(hold, doc, pages, page, line.node->story, line.node->paragraph, std::max(0, line.node->start),
                   line.node->paragraph, std::max(0, line.node->end));
        hold.eventText = "줄";
        return;
    }
    if ((hit.zone == ClickZone::Header || hit.zone == ClickZone::Footer) && count >= 2 && hit.bandStory > 0) {
        // 머리말·꼬리말 편집으로 들어간다. 첫 클릭이 놓은 본문 자리는 나올 때 돌아갈 곳이다.
        // 띠 안 글상자를 눌렀어도 커서는 띠 글에 둔다. 개체는 들어온 뒤에 고른다.
        const PointerCursor before = pointerCursor();
        const CaretPlace place = caretInStory(doc, pages, page, px, py, hit.bandStory);
        if (!place.hit) return;
        if (hold.bandStory <= 0 && before.active && before.story > 0) {
            hold.bandReturnStory = before.story;
            hold.bandReturnPara = before.paragraph;
            hold.bandReturnOff = before.offset;
        }
        hold.editStory = hit.bandStory;
        hold.bandStory = hit.bandStory;
        hold.bandHeader = hit.header;
        selectCaret(hold, doc, pages, page, place);
        hold.eventText = hit.header ? "머리말" : "꼬리말";
        return;
    }
    if (count >= 2 && !hit.objects.empty()) {
        const ObjRef* corner = nullptr;
        for (auto it = hit.objects.rbegin(); it != hit.objects.rend(); ++it) {
            if (cornerOf(*it, px, py, cornerR)) {
                corner = &(*it);
                break;
            }
        }
        if (corner && corner->node) {
            showObject(hold, pages, page, *corner);
            hold.property = corner->node->role.empty() ? "개체" : corner->node->role;
            hold.eventText = "개체 속성 " + hold.property;
            return;
        }
    }
    edgeCaret(hit.zone == ClickZone::None ? ClickZone::Bottom : hit.zone);
}

int objectHandleAt(const PointerHold& hold, int px, int py, double radius) {
    if (!hold.objectHandles || hold.boxes.empty() || radius <= 0) return -1;
    const HeldBox& box = hold.boxes.front();
    const std::int32_t xs[3] = {box.x, box.x + box.width / 2, box.x + box.width};
    const std::int32_t ys[3] = {box.y, box.y + box.height / 2, box.y + box.height};
    const int order[8][2] = {{0, 0}, {1, 0}, {2, 0}, {0, 1}, {2, 1}, {0, 2}, {1, 2}, {2, 2}};
    for (int i = 0; i < 8; ++i) {
        if (std::abs(px - xs[order[i][0]]) <= radius && std::abs(py - ys[order[i][1]]) <= radius) return i;
    }
    return -1;
}

void resizeObjectBox(PointerHold& hold, int px, int py) {
    const int handle = hold.resizeHandle;
    const bool corner = handle == 0 || handle == 2 || handle == 5 || handle == 7;
    std::int32_t left = hold.resizeLeft;
    std::int32_t top = hold.resizeTop;
    std::int32_t right = hold.resizeRight;
    std::int32_t bottom = hold.resizeBottom;
    if (corner && hold.resizeOldW > 0 && hold.resizeOldH > 0) {
        const double ow = static_cast<double>(hold.resizeOldW);
        const double oh = static_cast<double>(hold.resizeOldH);
        const double ratio = ow / oh;
        const std::int32_t ax = (handle == 0 || handle == 5) ? right : left;
        const std::int32_t ay = (handle == 0 || handle == 2) ? bottom : top;
        double dw = std::abs(static_cast<double>(px - ax));
        double dh = std::abs(static_cast<double>(py - ay));
        if (dw / ow >= dh / oh) {
            dw = std::max(400.0, dw);
            dh = dw / ratio;
        } else {
            dh = std::max(400.0, dh);
            dw = dh * ratio;
        }
        if (dw < 400.0) {
            dw = 400.0;
            dh = dw / ratio;
        }
        if (dh < 400.0) {
            dh = 400.0;
            dw = dh * ratio;
        }
        const auto w = std::max<std::int32_t>(400, static_cast<std::int32_t>(std::lround(dw)));
        const auto h = std::max<std::int32_t>(400, static_cast<std::int32_t>(std::lround(dh)));
        if (handle == 0 || handle == 5) {
            right = ax;
            left = ax - w;
        } else {
            left = ax;
            right = ax + w;
        }
        if (handle == 0 || handle == 2) {
            bottom = ay;
            top = ay - h;
        } else {
            top = ay;
            bottom = ay + h;
        }
    } else {
        switch (handle) {
            case 0: left = px; top = py; break;
            case 1: top = py; break;
            case 2: right = px; top = py; break;
            case 3: left = px; break;
            case 4: right = px; break;
            case 5: left = px; bottom = py; break;
            case 6: bottom = py; break;
            default: right = px; bottom = py; break;
        }
        if (right < left + 400) {
            if (handle == 0 || handle == 3 || handle == 5) left = right - 400;
            else right = left + 400;
        }
        if (bottom < top + 400) {
            if (handle == 0 || handle == 1 || handle == 2) top = bottom - 400;
            else bottom = top + 400;
        }
    }
    if (hold.boxes.empty()) return;
    hold.boxes.front().x = left;
    hold.boxes.front().y = top;
    hold.boxes.front().width = right - left;
    hold.boxes.front().height = bottom - top;
    hold.resizeNewW = static_cast<std::uint32_t>(right - left);
    hold.resizeNewH = static_cast<std::uint32_t>(bottom - top);
    bumpPointerPicture();
}

// 표 칸 경계선 찾기. 가장 안쪽 표의 선이다. 왼쪽·위 바깥선은 번호 -1 로, 끌지 않고 표를 고르는 자리다.
struct TableBorderHit {
    const Node* table = nullptr;
    int kind = 0;  // 1 세로선, 2 가로선
    int index = -1;
    bool outer = false;
    std::int32_t at = 0;
    std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<std::int32_t> sizes;
    std::vector<std::int32_t> least;  // 칸(줄)마다 끌어 줄일 수 있는 최소 크기
};

bool tableBorderAt(const model::Document& doc, const std::vector<Node>& pages, int page, int px, int py, double radius,
                   TableBorderHit& out) {
    if (page < 0 || page >= static_cast<int>(pages.size()) || radius <= 0) return false;
    bool found = false;
    auto visit = [&](const Node& node, std::int32_t x, std::int32_t y) {
        if (node.turned != 0 || node.pageBand || node.upright) return false;
        if (node.kind == NodeKind::Object && node.role == "table" && node.ext().obj().anchorItem >= 0 &&
            px >= x - radius && px <= x + node.width + radius && py >= y - radius && py <= y + node.height + radius) {
            std::vector<std::int32_t> cols;
            std::vector<std::int32_t> rows;
            std::vector<std::int32_t> rightEdge;
            std::vector<std::int32_t> bottomEdge;
            for (const Node& cell : node.children) {
                if (cell.kind != NodeKind::Cell || cell.ext().row < 0 || cell.ext().col < 0) continue;
                const auto c = static_cast<std::size_t>(cell.ext().col + std::max(1, cell.ext().colSpan) - 1);
                const auto r = static_cast<std::size_t>(cell.ext().row + std::max(1, cell.ext().rowSpan) - 1);
                if (rightEdge.size() <= c) rightEdge.resize(c + 1, 0);
                if (bottomEdge.size() <= r) bottomEdge.resize(r + 1, 0);
                rightEdge[c] = std::max(rightEdge[c], cell.x + cell.width);
                bottomEdge[r] = std::max(bottomEdge[r], cell.y + cell.height);
            }
            // 한글처럼 칸 폭은 좌우 안쪽 여백 + 1mm, 줄 높이는 칸 내용 높이보다 작게 줄이지 않는다
            // (한글 2024 칸 크기 줄이기, tmp/minsize). 여러 칸에 걸친 칸은 넣지 않는다.
            std::vector<std::int32_t> colLeast(rightEdge.size(), 283);
            std::vector<std::int32_t> rowLeast(bottomEdge.size(), 282);
            for (const Node& cell : node.children) {
                if (cell.kind != NodeKind::Cell || cell.ext().row < 0 || cell.ext().col < 0) continue;
                const auto c = static_cast<std::size_t>(cell.ext().col);
                const auto r = static_cast<std::size_t>(cell.ext().row);
                if (cell.ext().colSpan <= 1 && c < colLeast.size()) {
                    colLeast[c] = std::max(colLeast[c], cell.marginLeft + cell.marginRight + 283);
                }
                if (cell.ext().rowSpan <= 1 && r < rowLeast.size()) {
                    std::int32_t used = 0;
                    for (const Node& content : cell.children) {
                        if (content.turned == 0) used = std::max(used, detail::rowContentBottom(content));
                    }
                    rowLeast[r] = std::max(rowLeast[r], cell.marginTop + cell.marginBottom + used);
                }
            }
            std::int32_t prev = 0;
            for (const std::int32_t edge : rightEdge) {
                cols.push_back(std::max<std::int32_t>(0, edge - prev));
                prev = edge;
            }
            prev = 0;
            for (const std::int32_t edge : bottomEdge) {
                rows.push_back(std::max<std::int32_t>(0, edge - prev));
                prev = edge;
            }
            TableBorderHit best;
            double bestDist = radius;
            // 왼쪽·위 바깥선(번호 -1)은 끌지 않고 표만 고른다.
            if (py >= y && py <= y + node.height && std::abs(px - x) <= bestDist) {
                bestDist = std::abs(px - x);
                best.kind = 1;
                best.index = -1;
                best.outer = true;
                best.at = x;
                best.sizes = cols;
                best.least = colLeast;
            }
            if (px >= x && px <= x + node.width && std::abs(py - y) <= bestDist) {
                bestDist = std::abs(py - y);
                best.kind = 2;
                best.index = -1;
                best.outer = true;
                best.at = y;
                best.sizes = rows;
                best.least = rowLeast;
            }
            if (py >= y && py <= y + node.height) {
                for (std::size_t c = 0; c < rightEdge.size(); ++c) {
                    const double dist = std::abs(px - (x + rightEdge[c]));
                    if (dist <= bestDist) {
                        bestDist = dist;
                        best.kind = 1;
                        best.index = static_cast<int>(c);
                        best.outer = c + 1 == rightEdge.size();
                        best.at = x + rightEdge[c];
                        best.sizes = cols;
                        best.least = colLeast;
                    }
                }
            }
            if (px >= x && px <= x + node.width) {
                for (std::size_t r = 0; r < bottomEdge.size(); ++r) {
                    const double dist = std::abs(py - (y + bottomEdge[r]));
                    if (dist <= bestDist) {
                        bestDist = dist;
                        best.kind = 2;
                        best.index = static_cast<int>(r);
                        best.outer = r + 1 == bottomEdge.size();
                        best.at = y + bottomEdge[r];
                        best.sizes = rows;
                        best.least = rowLeast;
                    }
                }
            }
            if (best.kind != 0) {
                best.table = &node;
                best.x0 = x;
                best.y0 = y;
                best.x1 = x + node.width;
                best.y1 = y + node.height;
                out = std::move(best);
                found = true;
            }
        }
        return true;
    };
    walkPlacedNodes(doc, pages[static_cast<std::size_t>(page)], 0, 0, visit);
    return found;
}

// 개체 모습을 그려 둔다. 긴 변이 800 픽셀쯤 되게 그린다. 고른 개체만 빈 쪽에 올려 그려서
// 위에 겹친 도형이나 밑의 글은 들어가지 않는다. 흰 바탕과 검은 바탕에 한 번씩 그려 두 색 차이로 투명도를 얻는다.
// 그래서 새 자리에 실제 모양 그대로 얹어도 채우지 않은 곳으로 밑의 글이 보인다.
void snapshotHeldObject(PointerHold& hold, const model::Document& doc, const std::vector<Node>& pages, int page) {
    hold.dragSnapshot.reset();
    if (hold.boxes.empty() || page < 0 || page >= static_cast<int>(pages.size())) return;
    const HeldBox& box = hold.boxes.front();
    const Node& sheet = pages[static_cast<std::size_t>(page)];
    if (box.width <= 0 || box.height <= 0 || sheet.height <= 0) return;
    Node solo;
    if (hold.selectedObject) {
        solo.kind = sheet.kind;
        solo.width = sheet.width;
        solo.height = sheet.height;
        Node object = *hold.selectedObject;
        object.x = box.x;
        object.y = box.y;
        object.treatAsChar = false;
        solo.children.push_back(std::move(object));
    }
    const Node& paper = hold.selectedObject ? solo : sheet;
    const double scale = std::clamp(800.0 / std::max(box.width, box.height), 0.01, 0.12);
    const int pixelHeight = std::max(32, static_cast<int>(std::lround(paper.height * scale)));
    const double s = static_cast<double>(pixelHeight) / static_cast<double>(paper.height);
    const int vx = static_cast<int>(std::floor(box.x * s));
    const int vy = static_cast<int>(std::floor(box.y * s));
    const int vw = std::max(1, static_cast<int>(std::ceil((box.x + box.width) * s)) - vx);
    const int vh = std::max(1, static_cast<int>(std::ceil((box.y + box.height) * s)) - vy);
    PageBitmap bitmap = renderPageBitmap(doc, paper, pixelHeight, vx, vy, vw, vh);
    if (bitmap.rgba.empty()) return;
    const PageBitmap dark = renderPageBitmap(doc, paper, pixelHeight, vx, vy, vw, vh, PaintLayer::All, -1, 0);
    if (dark.rgba.size() == bitmap.rgba.size()) {
        for (std::size_t i = 0; i + 3 < bitmap.rgba.size(); i += 4) {
            int spread = 0;
            for (int c = 0; c < 3; ++c) spread += bitmap.rgba[i + c] - dark.rgba[i + c];
            const int alpha = std::clamp(255 - spread / 3, 0, 255);
            for (int c = 0; c < 3; ++c) {
                bitmap.rgba[i + c] = static_cast<std::uint8_t>(alpha > 0 ? std::min(255, dark.rgba[i + c] * 255 / alpha) : 0);
            }
            bitmap.rgba[i + 3] = static_cast<std::uint8_t>(alpha);
        }
    }
    static int serial = 0;
    hold.dragSnapshotId = "@drag:" + std::to_string(++serial);
    hold.dragSnapshot = pictureFromRgba(bitmap.width, bitmap.height, std::move(bitmap.rgba), false);
}

int drawGuideAt(const std::vector<Node>& pages, int page, std::int32_t x, std::int32_t y, double reach);

PointerView applyTimedDown(const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y, const PointerSample& sample) {
    PointerHold& hold = pointerHold();
    if (page < 0 || page >= static_cast<int>(pages.size())) {
        // 머리말·꼬리말 편집 중에는 종이 밖을 눌러도 그대로 둔다.
        if (hold.bandStory > 0) {
            hold.clickPending = false;
            return pointerViewOf(hold);
        }
        const int kept = hold.percent;
        const int id = hold.clickId;
        hold = {};
        hold.percent = kept;
        hold.clickId = id;
        hold.eventText = "선택 해제";
        bumpPointerPicture();
        return pointerViewOf(hold);
    }
    const double dpr = sample.dpr > 0 ? sample.dpr : 1;
    const double handleRadius = sample.huPerLogical > 0 ? 10.0 * sample.huPerLogical : 400;
    // 그리기 안내선 위를 누르면 그 선을 끌어 옮긴다(한글: 마우스 포인터가 화살표로 바뀌는 자리).
    if (!readOnly()) {
        if (const int grab = drawGuideAt(pages, page, x, y, handleRadius * 0.4); grab != 0) {
            hold.guideDrag = grab;
            hold.movePage = page;
            hold.clickPending = false;
            hold.tracking = true;
            hold.eventText = "안내선";
            return pointerViewOf(hold);
        }
    }
    // 읽기 전용이면 개체 손잡이를 잡지 않는다. 그 자리 누르기는 보통 클릭이 된다.
    const int handle = readOnly() ? -1 : objectHandleAt(hold, x, y, handleRadius);
    if (handle >= 0 && !hold.boxes.empty()) {
        const HeldBox& box = hold.boxes.front();
        hold.resizeHandle = handle;
        hold.resizePage = page;
        hold.resizeLeft = box.x;
        hold.resizeTop = box.y;
        hold.resizeRight = box.x + box.width;
        hold.resizeBottom = box.y + box.height;
        hold.resizeOldW = static_cast<std::uint32_t>(box.width);
        hold.resizeOldH = static_cast<std::uint32_t>(box.height);
        hold.resizeNewW = hold.resizeOldW;
        hold.resizeNewH = hold.resizeOldH;
        hold.resizeRole = hold.selectedObject ? hold.selectedObject->role.str() : std::string();
        // 표는 그린 줄 높이·칸 폭을 잡아 둔다. 늘릴 때 그 비율로 나눈다.
        hold.resizeRows.clear();
        hold.resizeCols.clear();
        if (hold.selectedObject) {
            const Node* table = hold.selectedObject;
            if (table->upright && !table->children.empty()) table = &table->children.front();
            for (const Node& cell : table->children) {
                if (cell.kind != NodeKind::Cell || cell.ext().row < 0 || cell.ext().col < 0) continue;
                const auto r = static_cast<std::size_t>(cell.ext().row);
                const auto c = static_cast<std::size_t>(cell.ext().col);
                if (cell.ext().rowSpan <= 1) {
                    if (hold.resizeRows.size() <= r) hold.resizeRows.resize(r + 1, 0);
                    hold.resizeRows[r] = std::max(hold.resizeRows[r], cell.height);
                }
                if (cell.ext().colSpan <= 1) {
                    if (hold.resizeCols.size() <= c) hold.resizeCols.resize(c + 1, 0);
                    hold.resizeCols[c] = std::max(hold.resizeCols[c], cell.width);
                }
            }
        }
        hold.resizeBin = hold.selectedObject ? hold.selectedObject->ext().obj().binItemId.str() : std::string();
        hold.resizeCrop = false;
        if (hold.selectedObject && hold.selectedObject->role == "picture") {
            const model::PictureView& pic = hold.selectedObject->ext().obj().picture;
            const std::int32_t clipW = pic.clipRight - pic.clipLeft;
            const std::int32_t clipH = pic.clipBottom - pic.clipTop;
            const bool identity = pic.originWidth <= 0 || pic.originHeight <= 0 || clipW <= 0 || clipH <= 0 ||
                                  (pic.clipLeft == 0 && pic.clipTop == 0 && pic.clipRight == pic.originWidth &&
                                   pic.clipBottom == pic.originHeight);
            if (!identity) {
                hold.resizeCrop = true;
                hold.resizeClipL = pic.clipLeft;
                hold.resizeClipT = pic.clipTop;
                hold.resizeClipR = pic.clipRight;
                hold.resizeClipB = pic.clipBottom;
                hold.resizeOriginW = pic.originWidth;
                hold.resizeOriginH = pic.originHeight;
            }
        }
        hold.tracking = true;
        hold.edgeActive = true;
        hold.edgeViewX = sample.viewX;
        hold.edgeViewY = sample.viewY;
        hold.edgeDpr = dpr;
        hold.eventText = "크기";
        if (hold.resizeRole != "picture") snapshotHeldObject(hold, doc, pages, page);
        return pointerViewOf(hold);
    }
    // 표 칸 경계선을 누르면 끌어 칸 폭·줄 높이를 바꾼다.
    if (!readOnly()) {
        TableBorderHit border;
        if (tableBorderAt(doc, pages, page, x, y, handleRadius * 0.3, border)) {
            hold.borderDrag = border.kind;
            hold.borderMoved = false;
            hold.borderPage = page;
            hold.borderIndex = border.index;
            hold.borderOuter = border.outer;
            hold.borderStart = border.kind == 1 ? x : y;
            hold.borderDelta = 0;
            hold.borderAt = border.at;
            hold.borderX0 = border.x0;
            hold.borderY0 = border.y0;
            hold.borderX1 = border.x1;
            hold.borderY1 = border.y1;
            hold.borderStory = border.table->ext().obj().anchorStory;
            hold.borderParagraph = border.table->ext().obj().anchorParagraph;
            hold.borderItem = border.table->ext().obj().anchorItem;
            hold.borderSizes = border.sizes;
            // 끌 수 있는 범위. 이 칸(줄)은 최소 크기, 세로 안쪽선은 오른쪽 이웃 칸도 최소 크기를 남긴다.
            // 이미 최소보다 작으면 더 줄이지만 않는다.
            const auto leastAt = [&](int i) {
                return i >= 0 && i < static_cast<int>(border.least.size()) ? border.least[static_cast<std::size_t>(i)] : 283;
            };
            const auto sizeAt = [&](int i) {
                return i >= 0 && i < static_cast<int>(border.sizes.size()) ? border.sizes[static_cast<std::size_t>(i)] : 0;
            };
            hold.borderMin = std::min(0, leastAt(border.index) - sizeAt(border.index));
            hold.borderMax = 1000000;
            if (border.kind == 1 && !border.outer && border.index + 1 < static_cast<int>(border.sizes.size())) {
                hold.borderMax = std::max(0, sizeAt(border.index + 1) - leastAt(border.index + 1));
            }
            if (border.index < 0) hold.borderMin = hold.borderMax = 0;
            hold.tracking = true;
            hold.eventText = border.kind == 1 ? "칸 폭" : "줄 높이";
            bumpPointerPicture();
            return pointerViewOf(hold);
        }
    }
    // 고른 띄운 개체 안을 누르면 끌어 옮긴다. 글자처럼 취급한 개체는 옮기지 않는다.
    if (!readOnly() && hold.objectHandles && hold.selectedObject && !hold.selectedObject->treatAsChar &&
        hold.selectedObject->ext().obj().anchorStory > 0 && hold.selectedObject->ext().obj().anchorItem >= 0 && !hold.boxes.empty()) {
        const HeldBox& box = hold.boxes.front();
        if (box.page == page && x >= box.x && x <= box.x + box.width && y >= box.y && y <= box.y + box.height) {
            hold.moving = true;
            hold.movePage = page;
            hold.moveStartX = x;
            hold.moveStartY = y;
            hold.moveDX = 0;
            hold.moveDY = 0;
            hold.moveStory = hold.selectedObject->ext().obj().anchorStory;
            hold.moveParagraph = hold.selectedObject->ext().obj().anchorParagraph;
            hold.moveItem = hold.selectedObject->ext().obj().anchorItem;
            snapshotHeldObject(hold, doc, pages, page);
            hold.tracking = true;
            hold.eventText = "옮기기";
            return pointerViewOf(hold);
        }
    }
    int which = 0;
    if (pinHit(hold, x, y, 22.0 * sample.huPerLogical, which)) {
        hold.pinDrag = which;
        hold.pinDownX = sample.viewX;
        hold.pinDownY = sample.viewY;
        hold.pinMoved = false;
        hold.pinTapped = false;
        hold.tracking = true;
        hold.edgeActive = true;
        hold.edgeViewX = sample.viewX;
        hold.edgeViewY = sample.viewY;
        hold.edgeDpr = dpr;
        hold.eventText = "핀";
        return pointerViewOf(hold);
    }
    const Node& paper = pages[static_cast<std::size_t>(page)];
    // 표·글상자 테두리 안쪽으로 이만큼은 개체를 고르는 띠다.
    PaperHit hit = hitPaper(doc, paper, x, y, hold.bandStory, 3.0 * sample.huPerLogical);
    if (!hit.objects.empty() && hit.line.node && hit.objects.back().node == hold.selectedObject) hit.zone = ClickZone::Line;
    // 머리말·꼬리말 편집 중 그 띠 안(띠 안 글상자 포함)을 누른 것이다.
    const bool inBand = hold.bandStory > 0 && hit.bandStory == hold.bandStory &&
                        ((hit.zone == ClickZone::Line && hit.line.node) || hit.zone == ClickZone::Object);
    if (hold.bandStory <= 0 && hold.editStory > 0 && hit.zone == ClickZone::Line && hit.line.node &&
        hit.line.node->story != hold.editStory) {
        hold.editStory = 0;
    }
    const double chain = (sample.device == 1 ? 20.0 : 12.0) * dpr;
    const bool close = hold.clickDownTime > 0 && std::hypot(sample.viewX - hold.clickDownX, sample.viewY - hold.clickDownY) <= chain;
    const bool timeOk = hold.clickUpTime > 0 && sample.time - hold.clickUpTime <= 500.0 &&
                        hold.clickUpTime - hold.clickDownTime <= 500.0;
    const bool sameZone = hold.clickZone == static_cast<int>(hit.zone) || close;
    const bool sameFinger = hold.clickId == sample.id || (sample.device == 1 && hold.clickDevice == 1);
    const bool same = sameFinger && hold.clickPage == page && sameZone;
    const int count = (timeOk && close && same && hold.clickCount > 0) ? hold.clickCount + 1 : 1;
    hold.clickId = sample.id;
    hold.clickPage = page;
    hold.clickZone = static_cast<int>(hit.zone);
    hold.clickCount = count;
    hold.clickDevice = sample.device;
    hold.clickDownTime = sample.time;
    hold.clickDownX = sample.viewX;
    hold.clickDownY = sample.viewY;
    hold.clickDragged = false;
    hold.pendingPage = page;
    hold.pendingX = x;
    hold.pendingY = y;
    // 머리말·꼬리말 편집 중에는 본문을 고르지 않는다. 띠 밖을 누르면 본문 여백을 누를 때처럼 그 쪽 띠에서
    // 가장 가까운 줄로 간다. 세로는 그 줄 안으로 당겨, 가로 자리에 맞는 글자 앞에 커서를 둔다.
    // 다른 머리말·꼬리말을 두 번 누르면 그리로 옮긴다.
    int zoneY = y;
    if (hold.bandStory > 0 && !inBand) {
        const bool otherBand = (hit.zone == ClickZone::Header || hit.zone == ClickZone::Footer) && hit.bandStory > 0;
        if (!(otherBand && count >= 2)) {
            const LineRef near = bandStoryLineNear(paper, hold.bandStory, x, y, &doc);
            if (!near.node) {
                hold.clickPending = false;
                hold.eventText = hold.bandHeader ? "머리말 편집 중" : "꼬리말 편집 중";
                return pointerViewOf(hold);
            }
            hit = PaperHit{};
            hit.zone = ClickZone::Line;
            hit.line = near;
            hit.bandStory = hold.bandStory;
            hit.header = hold.bandHeader;
            zoneY = std::clamp(y, near.y, near.y + std::max(near.h, 1) - 1);
        }
    }
    hold.clickPending = count >= 2;
    applyZone(hold, doc, pages, page, x, zoneY, count, hit, 10.0 * sample.huPerLogical);
    return pointerViewOf(hold);
}

// 격자에 붙인 자리(쪽 HU). 격자가 보이고 붙이기 방식일 때만 바뀐다. 한글처럼 마우스로 끌 때만 붙인다.
void snapPointToGrid(const std::vector<Node>& pages, int page, std::int32_t& x, std::int32_t& y) {
    if (page < 0 || page >= static_cast<int>(pages.size())) return;
    const GridSettings grid = viewGuides().grid;
    if (!grid.show || grid.snap == GridSnap::Free) return;
    const PaperBox box = paperBoxOf(pages[static_cast<std::size_t>(page)]);
    x = snapToGrid(grid, box, x, true);
    y = snapToGrid(grid, box, y, false);
}

// 쪽에 띄운 개체들의 상자(쪽 HU). 개체 안은 들어가지 않는다. 돌린 틀 안은 자리가 달라 건너뛴다.
void gatherFloatingBoxes(const Node& node, std::int32_t ox, std::int32_t oy, std::vector<HeldBox>& out) {
    if (node.turned != 0 || node.upright || node.pageBand) return;
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Object && !node.treatAsChar && !node.role.empty()) {
        HeldBox box;
        box.x = x;
        box.y = y;
        box.width = node.width;
        box.height = node.height;
        out.push_back(box);
        return;
    }
    for (const Node& child : node.children) gatherFloatingBoxes(child, x, y, out);
}

// 논리 화소 하나가 쪽 HU 로 얼마인지(화면 배율 percent).
double huPerLogicalAt(int percent) { return 7500.0 / static_cast<double>(percent > 0 ? percent : 100); }
// 안내선 자석이 붙는 거리(논리 화소).
constexpr double kGuideMagnetLogical = 5.0;

// 옮기는 상자의 왼쪽·가운데·오른쪽(위·가운데·아래)을 가장 가까운 기준에 붙인다. 붙였으면 그 기준 번호를 돌려준다.
int magnetAxis(std::int32_t& start, std::int32_t size, const std::vector<std::int32_t>& marks, double reach) {
    double best = reach + 1;
    std::int32_t shift = 0;
    int which = -1;
    const std::int32_t edges[3] = {start, start + size / 2, start + size};
    for (std::size_t m = 0; m < marks.size(); ++m) {
        for (const std::int32_t edge : edges) {
            const double d = std::abs(static_cast<double>(marks[m] - edge));
            if (d <= reach && d < best) {
                best = d;
                shift = marks[m] - edge;
                which = static_cast<int>(m);
            }
        }
    }
    if (which >= 0) start += shift;
    return which;
}

// 옮기는 개체를 붙인다. 먼저 격자(왼쪽 위)에, 그다음 그리기 안내선과 다른 개체(개체 이동 안내선의 다른 개체에 맞춰 이동)에
// 자석으로 붙인다. 자석 효과는 개체 이동 안내선 설정을 따른다.
void snapMove(PointerHold& hold, const std::vector<Node>& pages) {
    hold.guideSnapX = false;
    hold.guideSnapY = false;
    if (hold.boxes.empty() || hold.movePage < 0 || hold.movePage >= static_cast<int>(pages.size())) return;
    const HeldBox& box = hold.boxes.front();
    std::int32_t left = box.x + hold.moveDX;
    std::int32_t top = box.y + hold.moveDY;
    snapPointToGrid(pages, hold.movePage, left, top);
    const GuideSettings guide = viewGuides().guide;
    if (guide.magnet && (guide.drawShow || (guide.moveShow && guide.alignOthers))) {
        const Node& paper = pages[static_cast<std::size_t>(hold.movePage)];
        const PaperBox paperBox = paperBoxOf(paper);
        std::vector<std::int32_t> xs;
        std::vector<std::int32_t> ys;
        if (guide.drawShow) {
            xs.push_back(guide.drawX.value_or(paperBox.width / 2));
            ys.push_back(guide.drawY.value_or(paperBox.height / 2));
        }
        if (guide.moveShow && guide.alignOthers) {
            std::vector<HeldBox> others;
            for (const Node& child : paper.children) gatherFloatingBoxes(child, 0, 0, others);
            for (const HeldBox& other : others) {
                if (other.x == box.x && other.y == box.y && other.width == box.width && other.height == box.height) continue;
                xs.insert(xs.end(), {other.x, other.x + other.width / 2, other.x + other.width});
                ys.insert(ys.end(), {other.y, other.y + other.height / 2, other.y + other.height});
            }
        }
        const double reach = kGuideMagnetLogical * huPerLogicalAt(hold.percent);
        const int hitX = magnetAxis(left, box.width, xs, reach);
        const int hitY = magnetAxis(top, box.height, ys, reach);
        hold.guideSnapX = guide.drawShow && hitX == 0;
        hold.guideSnapY = guide.drawShow && hitY == 0;
    }
    hold.moveDX = left - box.x;
    hold.moveDY = top - box.y;
}

// (x, y) 가까이 그리기 안내선이 있으면 1(세로선) 또는 2(가로선). reach 는 HU 다.
int drawGuideAt(const std::vector<Node>& pages, int page, std::int32_t x, std::int32_t y, double reach) {
    if (page < 0 || page >= static_cast<int>(pages.size())) return 0;
    const GuideSettings guide = viewGuides().guide;
    if (!guide.drawShow) return 0;
    const PaperBox box = paperBoxOf(pages[static_cast<std::size_t>(page)]);
    const std::int32_t gx = guide.drawX.value_or(box.width / 2);
    const std::int32_t gy = guide.drawY.value_or(box.height / 2);
    if (std::abs(static_cast<double>(x - gx)) <= reach) return 1;
    if (std::abs(static_cast<double>(y - gy)) <= reach) return 2;
    return 0;
}

// 끄는 그리기 안내선을 (x, y) 로 옮긴다. 자리는 종이 안이다.
void moveDrawGuide(const std::vector<Node>& pages, int page, int which, std::int32_t x, std::int32_t y) {
    if (page < 0 || page >= static_cast<int>(pages.size()) || which == 0) return;
    const PaperBox box = paperBoxOf(pages[static_cast<std::size_t>(page)]);
    ViewGuides next = viewGuides();
    if (which == 1) next.guide.drawX = std::clamp<std::int32_t>(x, 0, box.width);
    else next.guide.drawY = std::clamp<std::int32_t>(y, 0, box.height);
    setViewGuides(next);
}

PointerView applyTimedMove(const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y,
                          const PointerSample& sample) {
    PointerHold& hold = pointerHold();
    const double dpr = sample.dpr > 0 ? sample.dpr : 1;
    hold.edgeViewX = sample.viewX;
    hold.edgeViewY = sample.viewY;
    hold.edgeDpr = dpr;
    hold.edgeActive = true;
    if (hold.guideDrag != 0) {
        if (page == hold.movePage) moveDrawGuide(pages, page, hold.guideDrag, x, y);
        hold.eventText = "안내선";
        bumpPointerPicture();
        return pointerViewOf(hold);
    }
    if (hold.resizeHandle >= 0) {
        std::int32_t sx = x;
        std::int32_t sy = y;
        snapPointToGrid(pages, hold.resizePage, sx, sy);
        resizeObjectBox(hold, sx, sy);
        hold.eventText = "크기";
        return pointerViewOf(hold);
    }
    if (hold.moving) {
        if (page == hold.movePage) {
            hold.moveDX = x - hold.moveStartX;
            hold.moveDY = y - hold.moveStartY;
            snapMove(hold, pages);
        }
        hold.eventText = "옮기기";
        bumpPointerPicture();
        return pointerViewOf(hold);
    }
    if (hold.borderDrag != 0) {
        if (page == hold.borderPage) {
            const std::int32_t delta = (hold.borderDrag == 1 ? x : y) - hold.borderStart;
            hold.borderDelta = std::clamp(delta, hold.borderMin, std::max(hold.borderMin, hold.borderMax));
            if (hold.borderDelta != 0) hold.borderMoved = true;
        }
        hold.eventText = hold.borderDrag == 1 ? "칸 폭" : "줄 높이";
        bumpPointerPicture();
        return pointerViewOf(hold);
    }
    if (hold.pinDrag != 0 && hold.story > 0) {
        // 손가락 떨림으로 블록 끝이 한 글자씩 움직이지 않게, 8픽셀을 넘긴 뒤부터 따라간다.
        if (!hold.pinMoved) {
            if (std::hypot(sample.viewX - hold.pinDownX, sample.viewY - hold.pinDownY) <= 8.0 * dpr) {
                return pointerViewOf(hold);
            }
            hold.pinMoved = true;
        }
        const CaretPlace place = caretInStory(doc, pages, page, x, y, hold.story, true, hold.anchorPara, hold.anchorOff);
        if (place.hit) {
            int paraA = hold.pinDrag == 1 ? place.paragraph : hold.anchorPara;
            int offA = hold.pinDrag == 1 ? place.offset : hold.anchorOff;
            int paraB = hold.pinDrag == 2 ? place.paragraph : hold.focusPara;
            int offB = hold.pinDrag == 2 ? place.offset : hold.focusOff;
            if (hold.pinDrag == 1) {
                paraB = hold.focusPara;
                offB = hold.focusOff;
            }
            hold.anchorPara = paraA;
            hold.anchorOff = offA;
            hold.focusPara = paraB;
            hold.focusOff = offB;
            placeCaret(hold, place, place.page >= 0 ? place.page : page, pages, &doc);
            hold.selectedText.clear();
            rememberSelection(doc, pages);
            refreshPins(hold, doc, pages);
        }
        return pointerViewOf(hold);
    }
    if (!hold.clickPending) return pointerViewOf(hold);
    if (std::hypot(sample.viewX - hold.clickDownX, sample.viewY - hold.clickDownY) <= 8.0 * dpr) return pointerViewOf(hold);
    hold.clickDragged = true;
    hold.clickPending = false;
    hold.pins = false;
    const int originPage = hold.pendingPage >= 0 ? hold.pendingPage : page;
    CaretPlace start = originPage >= 0 && originPage < static_cast<int>(pages.size())
                           ? caretOnPage(doc, pages[static_cast<std::size_t>(originPage)], hold.pendingX, hold.pendingY)
                           : CaretPlace{};
    if (!start.hit && originPage >= 0 && originPage < static_cast<int>(pages.size())) {
        std::vector<LineRef> lines;
        gatherBodyLines(pages[static_cast<std::size_t>(originPage)], 0, 0, lines);
        const LineRef line = nearestLine(lines, hold.pendingX, hold.pendingY);
        if (line.node) {
            start = caretForCursor(doc, pages, model::CursorPos{line.node->story, line.node->paragraph, std::max(0, line.node->start)});
        }
    }
    if (start.hit) {
        hold.tracking = true;
        hold.story = start.story;
        hold.anchorPara = start.paragraph;
        hold.anchorOff = start.offset;
        hold.focusPara = start.paragraph;
        hold.focusOff = start.offset;
        placeCaret(hold, start, start.page >= 0 ? start.page : originPage, pages, &doc);
    }
    if (hold.tracking && hold.story > 0) {
        const CaretPlace place = caretInStory(doc, pages, page, x, y, hold.story, true, hold.anchorPara, hold.anchorOff);
        if (place.hit) {
            hold.focusPara = place.paragraph;
            hold.focusOff = place.offset;
            placeCaret(hold, place, place.page >= 0 ? place.page : page, pages, &doc);
            hold.selectedText.clear();
            rememberSelection(doc, pages);
            hold.eventText = "블록";
        }
    }
    return pointerViewOf(hold);
}

}  // namespace paint_detail

PointerView applyPointer(const model::Document& doc, const std::vector<Node>& pages, int kind, int page, int x, int y,
                         int percent, PointerSample sample) {
    PointerHold& hold = pointerHold();
    hold.eventText.clear();
    hold.property.clear();
    if (percent > 0) hold.percent = percent;
    // 도형 그리기: 누른 자리부터 끈 자리까지가 도형 상자다. 그 쪽 밖으로 나가면 쪽 끝에서 멈춘다.
    if (hold.drawTool != 0 && !readOnly()) {
        const auto clampToPage = [&](std::int32_t& px, std::int32_t& py) {
            if (hold.drawPage < 0 || hold.drawPage >= static_cast<int>(pages.size())) return;
            const Node& paper = pages[static_cast<std::size_t>(hold.drawPage)];
            paint_detail::snapPointToGrid(pages, hold.drawPage, px, py);
            px = std::clamp<std::int32_t>(px, 0, std::max<std::int32_t>(0, paper.width));
            py = std::clamp<std::int32_t>(py, 0, std::max<std::int32_t>(0, paper.height));
        };
        if (kind == 1 && page >= 0 && page < static_cast<int>(pages.size())) {
            hold.drawing = true;
            hold.drawPage = page;
            hold.drawX0 = hold.drawX1 = x;
            hold.drawY0 = hold.drawY1 = y;
            clampToPage(hold.drawX0, hold.drawY0);
            clampToPage(hold.drawX1, hold.drawY1);
        } else if ((kind == 2 || kind == 3) && hold.drawing) {
            if (page == hold.drawPage) {
                hold.drawX1 = x;
                hold.drawY1 = y;
                clampToPage(hold.drawX1, hold.drawY1);
            }
            if (kind == 3) {
                hold.drawing = false;
                hold.drawPending = true;
                hold.drawPendingTool = hold.drawTool;
                hold.drawTool = 0;
            }
        } else if (kind == 4) {
            hold.drawing = false;
        }
        bumpPointerPicture();
        return pointerViewOf(hold);
    }
    if (sample.time > 0 && kind == 1) return applyTimedDown(doc, pages, page, x, y, sample);
    if (sample.time > 0 && kind == 2 &&
        (hold.clickPending || hold.pinDrag != 0 || hold.resizeHandle >= 0 || hold.moving || hold.borderDrag != 0 ||
         hold.guideDrag != 0)) {
        return applyTimedMove(doc, pages, page, x, y, sample);
    }
    if (sample.time > 0 && kind == 3 && hold.clickPending && !hold.clickDragged) {
        hold.clickPending = false;
        hold.clickUpTime = sample.time;
        hold.edgeActive = false;
        hold.pinDrag = 0;
        if (gSelectionPins.load(std::memory_order_relaxed) && hold.story > 0 &&
            (hold.anchorPara != hold.focusPara || hold.anchorOff != hold.focusOff)) {
            hold.pins = true;
            refreshPins(hold, doc, pages);
        }
        return pointerViewOf(hold);
    }
    if (sample.time > 0 && kind == 3 && hold.guideDrag != 0) {
        if (page == hold.movePage) paint_detail::moveDrawGuide(pages, page, hold.guideDrag, x, y);
        hold.guideDrag = 0;
        hold.tracking = false;
        bumpPointerPicture();
        return pointerViewOf(hold);
    }
    if (sample.time > 0 && kind == 3) {
        hold.dragSnapshot.reset();
        if (hold.borderDrag != 0) {
            if (page == hold.borderPage) {
                const std::int32_t delta = (hold.borderDrag == 1 ? x : y) - hold.borderStart;
                hold.borderDelta = std::clamp(delta, hold.borderMin, std::max(hold.borderMin, hold.borderMax));
            }
            hold.borderPending = hold.borderMoved && hold.borderDelta != 0;
            if (!hold.borderPending) hold.borderDrag = 0;
            hold.tracking = false;
            hold.edgeActive = false;
            // 끌지 않고 뗐으면 그 표를 고른다.
            if (!hold.borderPending) reselectAnchoredObject(doc, pages, hold.borderPage, hold.borderStory,
                                                            hold.borderParagraph, hold.borderItem);
            bumpPointerPicture();
            return pointerViewOf(hold);
        }
        if (hold.moving) {
            if (page == hold.movePage) {
                hold.moveDX = x - hold.moveStartX;
                hold.moveDY = y - hold.moveStartY;
                snapMove(hold, pages);
            }
            hold.moving = false;
            hold.guideSnapX = false;
            hold.guideSnapY = false;
            hold.movePending = hold.moveDX != 0 || hold.moveDY != 0;
            hold.tracking = false;
            hold.edgeActive = false;
            bumpPointerPicture();
            return pointerViewOf(hold);
        }
        if (hold.resizeHandle >= 0) {
            // 뗀 자리까지 크기에 넣는다.
            if (!hold.boxes.empty() && hold.boxes.front().page == page) {
                std::int32_t sx = x;
                std::int32_t sy = y;
                snapPointToGrid(pages, hold.resizePage, sx, sy);
                resizeObjectBox(hold, sx, sy);
            }
            hold.resizeCommit = hold.resizeNewW > 0 &&
                                (hold.resizeNewW != hold.resizeOldW || hold.resizeNewH != hold.resizeOldH);
            hold.resizeHandle = -1;
            // 그림이 아닌 개체는 문서의 그 컨트롤을 찾아 바꾼다(표는 칸까지). 그림은 원래 길(commitObjectResize)이다.
            const Node* picked = hold.selectedObject;
            if (hold.resizeCommit && picked && picked->role != "picture" && picked->ext().obj().anchorItem >= 0) {
                hold.resizeCommit = false;
                hold.resizeObjectPending = true;
                hold.resizeStory = picked->ext().obj().anchorStory;
                hold.resizeParagraph = picked->ext().obj().anchorParagraph;
                hold.resizeItem = picked->ext().obj().anchorItem;
                if (!hold.boxes.empty()) {
                    hold.resizeNewLeft = hold.boxes.front().x;
                    hold.resizeNewTop = hold.boxes.front().y;
                }
            }
            if (hold.resizeCommit || hold.resizeObjectPending) hold.selectedObject = nullptr;
        }
        if (hold.clickDragged) {
            hold.clickCount = 0;
            hold.clickUpTime = 0;
            hold.clickDragged = false;
        } else {
            hold.clickUpTime = sample.time;
        }
        hold.edgeActive = false;
        if (hold.pinDrag != 0 && !hold.pinMoved) hold.pinTapped = true;
        hold.pinDrag = 0;
        if (gSelectionPins.load(std::memory_order_relaxed) && hold.story > 0 &&
            (hold.anchorPara != hold.focusPara || hold.anchorOff != hold.focusOff)) {
            hold.pins = true;
            refreshPins(hold, doc, pages);
        }
    }
    if (kind == 4 || (kind == 1 && page < 0)) {
        const int kept = hold.percent;
        const bool had = hold.caret.hit || !hold.boxes.empty();
        hold = {};
        hold.percent = kept;
        if (had) {
            hold.eventText = "선택 해제";
            bumpPointerPicture();
        }
        return pointerViewOf(hold);
    }
    bool moved = kind == 1;
    if (kind == 1) {
        clearPendingCharShape(hold);
        CaretPlace place{};
        if (page >= 0 && page < static_cast<int>(pages.size()) && pages[static_cast<std::size_t>(page)].width > 0) {
            place = caretOnPage(doc, pages[static_cast<std::size_t>(page)], x, y);
        }
        if (!place.hit || place.story <= 0) {
            const int kept = hold.percent;
            const bool had = hold.caret.hit || !hold.boxes.empty();
            hold = {};
            hold.percent = kept;
            hold.eventText = "클릭 " + pointText(page, x, y) + " → 글자 없음";
            if (had) bumpPointerPicture();
            return pointerViewOf(hold);
        }
        hold.tracking = true;
        clearComposition(hold);
        hold.preferredX = -1;
        hold.story = place.story;
        hold.anchorPara = place.paragraph;
        hold.anchorOff = place.offset;
        hold.focusPara = place.paragraph;
        hold.focusOff = place.offset;
        armDragHost(hold, doc, pages, page, x, y, place);
        placeCaret(hold, place, page, pages, &doc);
        hold.eventText = "클릭 " + pointText(page, x, y) + " → 커서 " + caretSpot(hold.caret, hold.caretPage);
    } else if (kind == 2 && hold.tracking && hold.story > 0) {
        if (sample.time > 0) {
            hold.edgeActive = true;
            hold.edgeViewX = sample.viewX;
            hold.edgeViewY = sample.viewY;
            hold.edgeDpr = sample.dpr > 0 ? sample.dpr : 1;
        }
        clearPendingCharShape(hold);
        clearComposition(hold);
        hold.preferredX = -1;
        updateDragMode(hold, doc, pages, page, x, y, moved);
        if (hold.drag.mode == DragMode::Cells) {
            hold.caret.hit = false;
            if (moved) {
                hold.eventText = "드래그 " + pointText(page, x, y) + " → 칸 " + std::to_string(hold.drag.row) + "," +
                                 std::to_string(hold.drag.col) + "–" + std::to_string(hold.drag.focusRow) + "," +
                                 std::to_string(hold.drag.focusCol);
            }
        } else if (hold.drag.mode == DragMode::Object) {
            focusObjectSelection(hold, doc, pages, page, x, y, moved);
            if (moved) {
                hold.eventText = "드래그 " + pointText(page, x, y) + " → 개체 " + rangeText(hold);
            }
        } else {
            const CaretPlace place =
                caretInStory(doc, pages, page, x, y, hold.story, true, hold.anchorPara, hold.anchorOff);
            if (place.hit && place.story == hold.story) {
                const int caretPage = place.page >= 0 ? place.page : page;
                moved = place.paragraph != hold.focusPara || place.offset != hold.focusOff || caretPage != hold.caretPage;
                if (moved) {
                    hold.focusPara = place.paragraph;
                    hold.focusOff = place.offset;
                    placeCaret(hold, place, caretPage, pages, &doc);
                    hold.eventText = "드래그 " + pointText(page, x, y) + " → " + rangeText(hold) + " 커서 " +
                                     caretSpot(hold.caret, hold.caretPage);
                }
            }
        }
    } else if (kind == 3) {
        hold.tracking = false;
        if (hold.caret.hit) {
            hold.eventText = "뗌 " + rangeText(hold) + " 커서 " + caretSpot(hold.caret, hold.caretPage);
        }
    }
    if (moved) rememberSelection(doc, pages);
    return pointerViewOf(hold);
}

bool repeatedTouchDown(double viewX, double viewY, double timeMs, double dpr, int device, int id) {
    const PointerHold& hold = pointerHold();
    if (hold.clickUpTime <= 0 || hold.clickCount < 1) return false;
    if (hold.clickId != id && !(device == 1 && hold.clickDevice == 1)) return false;
    if (timeMs - hold.clickUpTime > 500.0 || hold.clickUpTime - hold.clickDownTime > 500.0) return false;
    const double scale = dpr > 0 ? dpr : 1;
    const double chain = (device == 1 ? 20.0 : 12.0) * scale;
    return std::hypot(viewX - hold.clickDownX, viewY - hold.clickDownY) <= chain;
}

bool selectionEdgeScroll(double dtMs, int viewW, int viewH, int& dx, int& dy) {
    dx = 0;
    dy = 0;
    const PointerHold& hold = pointerHold();
    if (!hold.edgeActive || dtMs <= 0 || viewW <= 0 || viewH <= 0) return false;
    const double dpr = hold.edgeDpr > 0 ? hold.edgeDpr : 1;
    const double band = 32.0 * dpr;
    const auto push = [&](double depth) {
        const double t = std::min(1.0, std::max(0.0, depth / band));
        return 700.0 * dpr * t * dtMs / 1000.0;
    };
    if (hold.edgeViewX < band) dx = -static_cast<int>(std::round(push(band - hold.edgeViewX)));
    else if (hold.edgeViewX > viewW - band) dx = static_cast<int>(std::round(push(hold.edgeViewX - (viewW - band))));
    if (hold.edgeViewY < band) dy = -static_cast<int>(std::round(push(band - hold.edgeViewY)));
    else if (hold.edgeViewY > viewH - band) dy = static_cast<int>(std::round(push(hold.edgeViewY - (viewH - band))));
    return dx != 0 || dy != 0;
}

void selectionEdgePoint(double& viewX, double& viewY) {
    const PointerHold& hold = pointerHold();
    viewX = hold.edgeViewX;
    viewY = hold.edgeViewY;
}

std::string pointerPropertyText() { return pointerHold().property; }

ResizeCommitInfo resizeCommitInfo() {
    const PointerHold& hold = pointerHold();
    ResizeCommitInfo info;
    info.page = hold.resizePage;
    info.bin = hold.resizeBin;
    info.width = hold.resizeNewW;
    info.height = hold.resizeNewH;
    return info;
}

void applyObjectResize(model::Placement& place, std::uint32_t newWidth, std::uint32_t newHeight) {
    auto axis = [](std::uint32_t oldSize, std::uint32_t newSize, double scale) {
        if (!(scale > 1e-12)) scale = 1.0;
        if (oldSize == 0 || newSize == 0) return scale;
        const double org = static_cast<double>(oldSize) / scale;
        if (!(org > 1e-6)) return scale;
        return static_cast<double>(newSize) / org;
    };
    place.scaleX = axis(place.width, newWidth, place.scaleX);
    place.scaleY = axis(place.height, newHeight, place.scaleY);
    place.width = newWidth;
    place.height = newHeight;
}

void setSelectionPins(bool on) {
    gSelectionPins.store(on, std::memory_order_relaxed);
    PointerHold& hold = pointerHold();
    if (!on && hold.pins) {
        hold.pins = false;
        hold.pinDrag = 0;
        bumpPointerPicture();
    }
}

bool selectionPins() { return gSelectionPins.load(std::memory_order_relaxed); }

bool pinUnder(int page, int x, int y, double radius) {
    const PointerHold& hold = pointerHold();
    int which = 0;
    if (!pinHit(hold, x, y, radius, which)) return false;
    const CaretPlace& pin = which == 1 ? hold.pinStart : hold.pinEnd;
    return pin.page < 0 || pin.page == page;
}

std::string takeOpenDialog() {
    PointerHold& hold = pointerHold();
    std::string name = std::move(hold.openDialog);
    hold.openDialog.clear();
    return name;
}

bool takePinTap() {
    PointerHold& hold = pointerHold();
    const bool tapped = hold.pinTapped;
    hold.pinTapped = false;
    return tapped;
}

std::string hoverCursor(const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y,
                        double handleRadius) {
    if (page < 0 || page >= static_cast<int>(pages.size())) return "default";
    const PointerHold& hold = pointerHold();
    if (hold.drawTool != 0 && !readOnly()) return "crosshair";
    if (!readOnly()) {
        const int guide = paint_detail::drawGuideAt(pages, page, x, y, handleRadius * 0.4);
        if (guide == 1) return "ew-resize";
        if (guide == 2) return "ns-resize";
    }
    if (!readOnly() && (hold.boxes.empty() || hold.boxes.front().page == page)) {
        // 손잡이 번호는 왼쪽 위부터 0 1 2 / 3 4 / 5 6 7 이다.
        static const char* const kHandleCursor[8] = {"nwse-resize", "ns-resize", "nesw-resize", "ew-resize",
                                                     "ew-resize",   "nesw-resize", "ns-resize", "nwse-resize"};
        const int handle = objectHandleAt(hold, x, y, handleRadius);
        if (handle >= 0) return kHandleCursor[handle];
        // 표 칸 경계선. 누를 때처럼 손잡이 다음이다.
        TableBorderHit border;
        if (tableBorderAt(doc, pages, page, x, y, handleRadius * 0.3, border)) {
            if (border.index < 0) return "pointer";
            return border.kind == 1 ? "col-resize" : "row-resize";
        }
        // 고른 띄운 개체 안은 끌어 옮기는 자리다.
        if (hold.objectHandles && hold.selectedObject && !hold.selectedObject->treatAsChar &&
            hold.selectedObject->ext().obj().anchorItem >= 0 && !hold.boxes.empty()) {
            const HeldBox& box = hold.boxes.front();
            if (box.page == page && x >= box.x && x <= box.x + box.width && y >= box.y && y <= box.y + box.height) {
                return "move";
            }
        }
    }
    const PaperHit hit = hitPaper(doc, pages[static_cast<std::size_t>(page)], x, y, hold.bandStory, handleRadius * 0.3);
    // 머리말·꼬리말 편집 중에는 본문 줄 위도 글 커서를 보이지 않는다.
    if (hold.bandStory > 0 && hit.bandStory != hold.bandStory) return "default";
    if (hit.zone != ClickZone::Line) return "default";
    return hit.line.turned ? "vertical-text" : "text";
}

void paint_detail::resizeControl(model::Control& ctrl, std::uint32_t oldW, std::uint32_t oldH, std::uint32_t newW,
                   std::uint32_t newH) {
    if (!ctrl.placement) return;
    const double rx = oldW > 0 ? static_cast<double>(newW) / static_cast<double>(oldW) : 1.0;
    const double ry = oldH > 0 ? static_cast<double>(newH) / static_cast<double>(oldH) : 1.0;
    for (model::ShapePoint& pt : ctrl.points) {
        pt.x = static_cast<std::int32_t>(std::llround(static_cast<double>(pt.x) * rx));
        pt.y = static_cast<std::int32_t>(std::llround(static_cast<double>(pt.y) * ry));
    }
    applyObjectResize(*ctrl.placement, newW, newH);
    // 저장된 글상자·캡션 폭은 새 크기로 맞춘다. 남겨 두면 줄 폭이 옛 크기로 잡힌다.
    if (ctrl.boxTextWidth > 0) ctrl.boxTextWidth = ctrl.subLists.empty() || ctrl.subLists.front().textDirection == 0
                                                       ? newW
                                                       : newH;
    if (ctrl.caption && (ctrl.caption->side == "TOP" || ctrl.caption->side == "BOTTOM") && ctrl.caption->lastWidth > 0) {
        ctrl.caption->lastWidth =
            newW + (ctrl.caption->fullSize ? static_cast<std::uint32_t>(std::max(0, ctrl.placement->marginLeft + ctrl.placement->marginRight)) : 0u);
    }
}

bool commitObjectResize(model::Document& doc) {
    PointerHold& hold = pointerHold();
    if (!hold.resizeCommit) return false;
    if (readOnly()) {
        hold.resizeCommit = false;
        return false;
    }
    hold.resizeCommit = false;
    const std::uint32_t oldW = hold.resizeOldW;
    const std::uint32_t oldH = hold.resizeOldH;
    const std::uint32_t newW = hold.resizeNewW;
    const std::uint32_t newH = hold.resizeNewH;
    if (oldW == 0 || newW == 0 || newH == 0) return false;
    const std::string role = hold.resizeRole;
    const std::string bin = hold.resizeBin;
    bool found = false;
    const auto visit = [&](const auto& self, model::Control& ctrl) -> void {
        if (found) return;
        for (model::Control& child : ctrl.children) self(self, child);
        if (ctrl.table) {
            for (model::TableCell& cell : ctrl.table->cells) {
                for (model::Paragraph& para : cell.content.paragraphs) {
                    const bool before = found;
                    for (model::Run& run : para.runs) {
                        for (model::InlineItem& item : run.items) {
                            if (item.type == model::InlineItem::Type::Control) self(self, item.control);
                        }
                    }
                    if (!before && found) para.lineSegs.clear();
                }
            }
        }
        for (model::SubList& list : ctrl.subLists) {
            for (model::Paragraph& para : list.paragraphs) {
                const bool before = found;
                for (model::Run& run : para.runs) {
                    for (model::InlineItem& item : run.items) {
                        if (item.type == model::InlineItem::Type::Control) self(self, item.control);
                    }
                }
                if (!before && found) para.lineSegs.clear();
            }
        }
        if (!ctrl.placement || ctrl.placement->width != oldW || ctrl.placement->height != oldH) return;
        if (!bin.empty() && ctrl.binItemId != bin) return;
        if (!role.empty() && std::string(model::controlKindName(ctrl.kind)) != role) return;
        resizeControl(ctrl, oldW, oldH, newW, newH);
        found = true;
    };
    for (model::Section& section : doc.sections) {
        for (model::Paragraph& para : section.paragraphs) {
            // 크기를 바꾼 개체를 품은 문단은 저장된 줄 정보가 낡는다. 바깥 문단까지 지워 다시 계산하게 한다.
            const bool before = found;
            for (model::Run& run : para.runs) {
                for (model::InlineItem& item : run.items) {
                    if (item.type == model::InlineItem::Type::Control) visit(visit, item.control);
                }
            }
            if (!before && found) para.lineSegs.clear();
        }
    }
    return found;
}

std::string pointerEventText() { return pointerHold().eventText; }

int pointerPicture() { return pointerPictureClock; }

bool caretShown() {
    const PointerHold& hold = pointerHold();
    if (!hold.boxes.empty()) return false;
    return hold.caret.hit || (hold.composing && !hold.compUnderlines.empty());
}

int caretBlinkPhase() {
    if (!caretShown()) return -1;
    const PointerHold& hold = pointerHold();
    const std::int64_t elapsed = std::max<std::int64_t>(0, nowCaretMs() - hold.caretTouchMs);
    return static_cast<int>((elapsed / 530) % 2);
}

bool pointerTracking() { return pointerHold().tracking; }

BandVeil bandVeil(int page) {
    const PointerHold& hold = pointerHold();
    BandVeil veil;
    if (hold.bandStory <= 0 || hold.bandMaster || page < 0 || page >= static_cast<int>(hold.bandAreas.size())) return veil;
    const PointerHold::BandArea& area = hold.bandAreas[static_cast<std::size_t>(page)];
    veil.veiled = true;
    veil.x = area.x;
    veil.y = area.y;
    veil.width = area.width;
    veil.height = area.height;
    return veil;
}


namespace {
std::atomic<bool> gMasterView{false};
std::atomic<int> gMasterViewEpoch{0};
}  // namespace

void paint_detail::setMasterView(bool on) {
    if (gMasterView.exchange(on, std::memory_order_relaxed) != on) gMasterViewEpoch.fetch_add(1, std::memory_order_relaxed);
}

bool masterView() { return gMasterView.load(std::memory_order_relaxed); }

int masterViewEpoch() { return gMasterViewEpoch.load(std::memory_order_relaxed); }

void clearPointerSelection() {
    setMasterView(false);
    pointerHold() = {};
    bumpPointerPicture();
}

bool findPaintedBin(const model::Document& doc, const Node& node, std::int32_t ox, std::int32_t oy,
                    const std::string& bin, int width, int height, ObjRef& out) {
    const std::int32_t x = childX(node, ox);
    const std::int32_t y = oy + node.y;
    if (node.kind == NodeKind::Line) {
        struct Placed {
            const Node* node = nullptr;
            std::int32_t x = 0;
            std::int32_t y = 0;
        };
        std::vector<Placed> placed;
        walkPlacedChildren(node, x, y, &doc, [&](const Node& child, std::int32_t cx, std::int32_t cy) {
            placed.push_back({&child, cx, cy});
        });
        for (const Placed& item : placed) {
            const Node& child = *item.node;
            if (child.kind != NodeKind::Object || child.role == "caption" || child.role == "pageBorder" ||
                child.role == "pageFill") {
                continue;
            }
            const std::int32_t hx = item.x + child.x;
            const std::int32_t hy = item.y + child.y;
            if (child.ext().obj().binItemId == bin && child.width == width && child.height == height) {
                out = {&child, hx, hy, child.width, child.height};
                return true;
            }
            if (findPaintedBin(doc, child, hx - child.x, hy - child.y, bin, width, height, out)) return true;
        }
        return false;
    }
    if (node.kind == NodeKind::Object && node.role != "caption" && node.role != "pageBorder" &&
        node.role != "pageFill" && node.ext().obj().binItemId == bin && node.width == width && node.height == height) {
        out = {&node, x, y, node.width, node.height};
        return true;
    }
    for (const Node& child : node.children) {
        if (findPaintedBin(doc, child, x, y, bin, width, height, out)) return true;
    }
    return false;
}

bool restoreResizedObject(const model::Document& doc, const std::vector<Node>& pages, int pageHint,
                          const std::string& bin, int width, int height) {
    if (bin.empty() || width <= 0 || height <= 0 || pages.empty()) return false;
    const int count = static_cast<int>(pages.size());
    const int hint = std::clamp(pageHint, 0, count - 1);
    for (int dist = 0; dist < count; ++dist) {
        const int order[2] = {hint + dist, hint - dist};
        const int n = dist == 0 ? 1 : 2;
        for (int step = 0; step < n; ++step) {
            const int page = order[step];
            if (page < 0 || page >= count) continue;
            Node& paper = const_cast<Node&>(pages[static_cast<std::size_t>(page)]);
            if (paper.spansLater) ensurePageSpans(doc, paper);
            ObjRef box;
            if (!findPaintedBin(doc, paper, 0, 0, bin, width, height, box)) continue;
            showObject(pointerHold(), pages, page, box);
            return true;
        }
    }
    return false;
}

PointerCursor pointerCursor() {
    const PointerHold& hold = pointerHold();
    PointerCursor out;
    if (hold.story <= 0 || !hold.caret.hit) return out;
    out.active = true;
    out.caret = hold.caret.hit;
    out.page = hold.caretPage;
    out.story = hold.story;
    out.paragraph = hold.focusPara;
    out.offset = hold.focusOff;
    out.anchorParagraph = hold.anchorPara;
    out.anchorOffset = hold.anchorOff;
    out.focusParagraph = hold.focusPara;
    out.focusOffset = hold.focusOff;
    out.startParagraph = hold.anchorPara;
    out.startOffset = hold.anchorOff;
    out.endParagraph = hold.focusPara;
    out.endOffset = hold.focusOff;
    if (out.endParagraph < out.startParagraph ||
        (out.endParagraph == out.startParagraph && out.endOffset < out.startOffset)) {
        std::swap(out.startParagraph, out.endParagraph);
        std::swap(out.startOffset, out.endOffset);
    }
    out.hasSelection = out.startParagraph != out.endParagraph || out.startOffset != out.endOffset;
    return out;
}

model::CursorTarget resolvePointerCursor(model::Document& doc) {
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    return resolveCursor(doc, model::CursorPos{cur.story, cur.paragraph, cur.offset});
}

PointerView setPointerCursor(const model::Document& doc, const std::vector<Node>& pages, model::CursorPos anchor,
                             model::CursorPos focus) {
    PointerHold& hold = pointerHold();
    hold.eventText.clear();
    // 캐럿을 새로 놓으면 ↑↓ 가 따라갈 가로 자리도 새로 잰다(클릭과 같다).
    hold.preferredX = -1;
    if (focus.story <= 0 || anchor.story <= 0 || focus.story != anchor.story) {
        const int kept = hold.percent;
        hold = {};
        hold.percent = kept;
        bumpPointerPicture();
        return pointerViewOf(hold);
    }
    const int reqAnchorPara = anchor.paragraph >= 0 ? anchor.paragraph : focus.paragraph;
    const int reqAnchorOff = anchor.offset >= 0 ? anchor.offset : focus.offset;
    if (reqAnchorPara != focus.paragraph || reqAnchorOff != focus.offset ||
        focus.story != hold.pendingCharStory || focus.paragraph != hold.pendingCharPara ||
        focus.offset != hold.pendingCharOffset) {
        clearPendingCharShape(hold);
    }
    const CaretPlace place = caretForCursor(doc, pages, focus);
    if (!place.hit || place.story <= 0) {
        const int kept = hold.percent;
        hold = {};
        hold.percent = kept;
        bumpPointerPicture();
        return pointerViewOf(hold);
    }
    hold.tracking = false;
    hold.story = place.story;
    hold.anchorPara = anchor.paragraph >= 0 ? anchor.paragraph : place.paragraph;
    hold.anchorOff = anchor.offset >= 0 ? anchor.offset : place.offset;
    hold.focusPara = place.paragraph;
    hold.focusOff = place.offset;
    if (hold.anchorPara != hold.focusPara || hold.anchorOff != hold.focusOff ||
        hold.story != hold.pendingCharStory || hold.focusPara != hold.pendingCharPara ||
        hold.focusOff != hold.pendingCharOffset) {
        clearPendingCharShape(hold);
    }
    placeCaret(hold, place, place.page, pages, &doc);
    rememberSelection(doc, pages);
    return pointerViewOf(hold);
}

std::vector<SelectionMark> selectionMarks() {
    std::vector<SelectionMark> marks;
    const PointerHold& hold = pointerHold();
    marks.reserve(hold.boxes.size());
    for (const HeldBox& box : hold.boxes) {
        SelectionMark mark;
        mark.page = box.page;
        mark.x = box.x;
        mark.y = box.y;
        mark.width = box.width;
        mark.height = box.height;
        mark.invert = box.invert;
        marks.push_back(mark);
    }
    return marks;
}

std::vector<SelectionMark> compositionUnderlineMarks() {
    std::vector<SelectionMark> marks;
    const PointerHold& hold = pointerHold();
    marks.reserve(hold.compUnderlines.size());
    for (const HeldBox& box : hold.compUnderlines) {
        SelectionMark mark;
        mark.page = box.page;
        mark.x = box.x;
        mark.y = box.y;
        mark.width = box.width;
        mark.height = box.height;
        mark.invert = box.invert;
        marks.push_back(mark);
    }
    return marks;
}

std::vector<SelectionImage> pointerImages(const model::Document& doc, const std::vector<Node>& pages) {
    const PointerHold& hold = pointerHold();
    std::vector<BlockBox> boxes;
    boxes.reserve(hold.boxes.size() + hold.compUnderlines.size());
    const auto appendBoxes = [&](const std::vector<HeldBox>& src) {
        for (const HeldBox& box : src) {
            BlockBox mark;
            mark.page = box.page;
            mark.x = box.x;
            mark.y = box.y;
            mark.width = box.width;
            mark.height = box.height;
            mark.backR = box.backR;
            mark.backG = box.backG;
            mark.backB = box.backB;
            boxes.push_back(mark);
        }
    };
    appendBoxes(hold.boxes);
    appendBoxes(hold.compUnderlines);
    const CaretPlace caret = (hold.composing && !hold.compUnderlines.empty()) ? CaretPlace{} : hold.caret;
    return paintSelection(doc, pages, boxes, caret, hold.caretPage, hold.percent);
}

}  // namespace chwp::render
