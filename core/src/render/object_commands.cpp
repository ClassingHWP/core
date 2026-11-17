// 이름으로 가리킨 개체(글 묶음·문단·문단 안 항목 순번)의 고르기·속성·지우기.
//
// 편집기에서는 개체를 눌러 고르고 개체 속성 대화 상자(기본·여백 탭)나 Delete 로 하는 일이다. agent 는 화면 없이
// 개체 이름으로 같은 일을 한다. 값 이름은 placementFields(개체 놓기 읽기)와 같고 길이는 HWPUNIT 이다.
// 크기를 바꾸면 손잡이 크기 조절(resizeObjectNow)과 같은 규칙이다: 표는 칸 폭·줄 높이를 비율대로 나누고,
// 그 밖의 개체는 resizeControl 이 점·글상자 폭·캡션 폭을 같이 바꾼다.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "chwp/model/table_edit.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

const FormatValue* fieldOf(const FormatFields& fields, const std::string& key) {
    const FormatValue* found = nullptr;
    for (const auto& [name, value] : fields) {
        if (name == key) found = &value;
    }
    return found;
}

const model::Control* findControl(const model::Document& doc, int story, int paragraph, int item) {
    const std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || paragraph < 0 || paragraph >= static_cast<int>(paras->size()) || item < 0) return nullptr;
    int index = 0;
    for (const model::Run& run : (*paras)[static_cast<std::size_t>(paragraph)].runs) {
        for (const model::InlineItem& entry : run.items) {
            if (index++ == item) return entry.type == model::InlineItem::Type::Control ? &entry.control : nullptr;
        }
    }
    return nullptr;
}

// sizes 를 합이 total 이 되게 비율대로 늘리거나 줄인다. 끝 자리를 반올림해 나눠 합이 꼭 total 이다.
std::vector<std::int32_t> rescale(const std::vector<std::int32_t>& sizes, std::int32_t total) {
    std::int64_t sum = 0;
    for (std::int32_t s : sizes) sum += std::max(0, s);
    if (sum <= 0 || total <= 0) return sizes;
    std::vector<std::int32_t> out(sizes.size());
    std::int64_t acc = 0;
    std::int64_t prev = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        acc += std::max(0, sizes[i]);
        const std::int64_t edge = (acc * total + sum / 2) / sum;
        out[i] = static_cast<std::int32_t>(std::max<std::int64_t>(1, edge - prev));
        prev = edge;
    }
    return out;
}

std::int32_t spanOf(const std::vector<std::int32_t>& sizes, std::uint32_t from, std::uint32_t count) {
    std::int32_t total = 0;
    for (std::uint32_t i = from; i < from + std::max<std::uint32_t>(1, count) && i < sizes.size(); ++i) total += sizes[i];
    return total;
}

bool wrapsText(const model::Placement& p) {
    return !p.treatAsChar && (p.textWrap.empty() || p.textWrap == "SQUARE" || p.textWrap == "TOP_AND_BOTTOM");
}

bool oneOf(const std::string& value, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        if (value == name) return true;
    }
    return false;
}

// 개체를 고친 뒤: 고른 개체·조합을 비우고 문서 전체를 다시 짠다.
EditResult relayoutAfterObjectEdit(model::Document& doc, std::vector<Node>& pages, int story, int paragraph,
                                   bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    hold.selectedObject = nullptr;
    hold.objectHandles = false;
    hold.resizeHandle = -1;
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    return makeEditResult(doc, true, true, story, paragraph, &relayout);
}

}  // namespace

void clearObjectSelection() {
    PointerHold& hold = pointerHold();
    hold.multiPicked.clear();
    if (!hold.selectedObject && !hold.objectHandles) return;
    hold.selectedObject = nullptr;
    hold.objectHandles = false;
    hold.resizeHandle = -1;
    bumpPointerPicture();
}

bool selectAnchoredObject(const model::Document& doc, const std::vector<Node>& pages, int story, int paragraph,
                          int item) {
    if (!findControl(doc, story, paragraph, item)) return false;
    for (int page = 0; page < static_cast<int>(pages.size()); ++page) {
        if (reselectAnchoredObject(doc, pages, page, story, paragraph, item)) return true;
    }
    return false;
}

FormatFields objectPropertiesView(const model::Document& doc, int story, int paragraph, int item) {
    const model::Control* control = findControl(doc, story, paragraph, item);
    if (!control || !control->placement) return {};
    const model::Placement& p = *control->placement;
    FormatFields out = placementFields(p);
    out.emplace_back("marginLeft", static_cast<double>(p.marginLeft));
    out.emplace_back("marginRight", static_cast<double>(p.marginRight));
    out.emplace_back("marginTop", static_cast<double>(p.marginTop));
    out.emplace_back("marginBottom", static_cast<double>(p.marginBottom));
    out.emplace_back("kind", std::string(control->kind == model::ControlKind::Picture   ? "picture"
                                         : control->kind == model::ControlKind::Table    ? "table"
                                         : control->kind == model::ControlKind::Equation ? "equation"
                                                                                         : "shape"));
    // 그림 밝기·대비(-100..100)와 효과(0 원래, 1 회색조, 2 흑백). 한글 그림 탭·개체 속성 [그림] 의 값이다.
    // 도형 선(종류·색·굵기 HU)과 채우기 색("none" 은 채우기 없음). 한글 도형 탭의 선·채우기다.
    if (control->kind == model::ControlKind::Shape) {
        out.emplace_back("lineType", control->strokeType.empty() ? std::string("SOLID") : control->strokeType);
        out.emplace_back("lineColor", control->strokeColor.empty() ? std::string("#000000") : control->strokeColor);
        out.emplace_back("lineWidth", static_cast<double>(control->strokeWidth));
        out.emplace_back("fillColor", control->fillColor.empty() ? std::string("none") : control->fillColor);
    }
    out.emplace_back("flipHorizontal", p.flipHorizontal);
    out.emplace_back("flipVertical", p.flipVertical);
    out.emplace_back("rotation", static_cast<double>(p.rotation % 360));
    out.emplace_back("skewX", static_cast<double>(p.skewX));
    out.emplace_back("skewY", static_cast<double>(p.skewY));
    if (control->kind == model::ControlKind::Picture) {
        out.emplace_back("bright", static_cast<double>(control->picture.bright));
        out.emplace_back("contrast", static_cast<double>(control->picture.contrast));
        out.emplace_back("effect", static_cast<double>(control->picture.effect));
        // 자르기(한글 개체 속성 그림 자르기 SkipLeft·Top·Right·Bottom, 숨긴 한글 2024: 원본 그림 단위로 imgClip 의
        // 왼쪽·위는 그 값, 오른쪽·아래는 원본 크기에서 뺀 값이다).
        const model::PictureView& pv = control->picture;
        const std::int32_t ow = pv.originWidth > 0 ? pv.originWidth : pv.clipRight;
        const std::int32_t oh = pv.originHeight > 0 ? pv.originHeight : pv.clipBottom;
        out.emplace_back("cropLeft", static_cast<double>(pv.clipLeft));
        out.emplace_back("cropTop", static_cast<double>(pv.clipTop));
        out.emplace_back("cropRight", static_cast<double>(pv.clipRight > 0 ? std::max(0, ow - pv.clipRight) : 0));
        out.emplace_back("cropBottom", static_cast<double>(pv.clipBottom > 0 ? std::max(0, oh - pv.clipBottom) : 0));
    }
    return out;
}

std::string selectedObjectKind(const model::Document& doc) {
    const Node* node = pointerHold().selectedObject;
    if (!node || node->kind != NodeKind::Object) return {};
    const ObjectExtra& o = node->ext().obj();
    const model::Control* ctrl = findControl(doc, o.anchorStory, o.anchorParagraph, o.anchorItem);
    if (!ctrl) return "shape";
    switch (ctrl->kind) {
        case model::ControlKind::Picture: return "picture";
        case model::ControlKind::Table: return "table";
        case model::ControlKind::Equation: return "equation";
        default: return "shape";
    }
}

bool selectedObjectAnchor(int& story, int& paragraph, int& item) {
    const Node* node = pointerHold().selectedObject;
    if (!node || node->kind != NodeKind::Object) return false;
    const ObjectExtra& o = node->ext().obj();
    if (o.anchorStory <= 0 || o.anchorItem < 0) return false;
    story = o.anchorStory;
    paragraph = o.anchorParagraph;
    item = o.anchorItem;
    return true;
}

namespace {

// 개체 순서(한글 ShapeObjBringToFront·BringForward·SendToBack·SendBack, 숨긴 한글 2024 로 확인).
// 같은 구역의 자리를 가진 개체(띄운 개체·표·그림·도형, 칸·글상자 안 포함)의 zOrder 를 본다.
// front: 고른 개체가 가장 큰 순서가 되고 그 위에 있던 개체는 한 칸씩 내려간다. back 은 그 반대다.
// forward·backward: 바로 위(아래) 순서의 개체와 자리를 바꾼다. 이미 맨 위(아래)면 그대로다.
void collectPlaced(std::vector<model::Paragraph>& paras, std::vector<model::Placement*>& out) {
    for (model::Paragraph& para : paras) {
        for (model::Run& run : para.runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Control) continue;
                model::Control& c = item.control;
                if (c.placement) out.push_back(&*c.placement);
                if (c.table) {
                    for (model::TableCell& cell : c.table->cells) collectPlaced(cell.content.paragraphs, out);
                }
                for (model::SubList& list : c.subLists) collectPlaced(list.paragraphs, out);
            }
        }
    }
}

void reorderObject(model::Document& doc, int story, model::Control& control, const std::string& order) {
    if (!control.placement) return;
    const model::StorySlot* slot = resolveStory(doc, story);
    int sectionIndex = slot ? slot->sectionIndex : -1;
    if (sectionIndex < 0) {
        const model::StoryMap& map = ensureStoryMap(doc);
        if (const model::StorySlot* root = map.slot(map.rootSectionStory(story))) sectionIndex = root->sectionIndex;
    }
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(doc.sections.size())) return;
    std::vector<model::Placement*> all;
    collectPlaced(doc.sections[static_cast<std::size_t>(sectionIndex)].paragraphs, all);
    model::Placement* self = &*control.placement;
    const std::int32_t z = self->zOrder;
    if (order == "front" || order == "back") {
        const bool front = order == "front";
        std::int32_t edge = z;
        for (model::Placement* p : all) {
            if (p == self) continue;
            if (front ? p->zOrder > z : p->zOrder < z) {
                edge = front ? std::max(edge, p->zOrder) : std::min(edge, p->zOrder);
                p->zOrder += front ? -1 : 1;
            }
        }
        self->zOrder = edge;
        return;
    }
    const bool up = order == "forward";
    model::Placement* next = nullptr;
    for (model::Placement* p : all) {
        if (p == self) continue;
        if (up ? p->zOrder > z : p->zOrder < z) {
            if (!next || (up ? p->zOrder < next->zOrder : p->zOrder > next->zOrder)) next = p;
        }
    }
    if (next) std::swap(self->zOrder, next->zOrder);
}

}  // namespace

namespace paint_detail {

EditResult objectPropertiesNow(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                               const FormatFields& fields, bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    model::Control* control = anchoredControl(doc, story, paragraph, item);
    if (!control) {
        why = "그 자리에 개체가 없습니다.";
        return {};
    }
    if (!control->placement) {
        why = "크기·위치가 없는 개체(머리말·꼬리말·각주 등)입니다.";
        return {};
    }
    model::Placement next = *control->placement;
    model::PictureView pictureLook = control->picture;
    model::Control shapeLook;
    shapeLook.strokeType = control->strokeType;
    shapeLook.strokeColor = control->strokeColor;
    shapeLook.strokeWidth = control->strokeWidth;
    shapeLook.fillColor = control->fillColor;
    std::string order;
    std::string flip;
    int turnTo = -1;  // 회전각을 이 값으로
    std::optional<std::int32_t> skewX;
    std::optional<std::int32_t> skewY;
    int turnBy = 0;   // 회전각에 더할 값(왼쪽 -90, 오른쪽 +90)
    const auto text = [&](const FormatValue& v, std::string& out) {
        if (const auto* s = std::get_if<std::string>(&v)) {
            out = *s;
            return true;
        }
        return false;
    };
    const auto number = [&](const FormatValue& v, double& out) {
        if (const auto* d = std::get_if<double>(&v)) {
            out = *d;
            return true;
        }
        return false;
    };
    const auto flag = [&](const FormatValue& v, bool& out) {
        if (const auto* b = std::get_if<bool>(&v)) {
            out = *b;
            return true;
        }
        return false;
    };
    for (const auto& [key, value] : fields) {
        bool ok = false;
        std::string s;
        double d = 0;
        if (key == "treatAsChar") ok = flag(value, next.treatAsChar);
        else if (key == "flowWithText") ok = flag(value, next.flowWithText);
        else if (key == "allowOverlap") ok = flag(value, next.allowOverlap);
        else if (key == "textWrap") ok = text(value, s) && oneOf(s, {"SQUARE", "TOP_AND_BOTTOM", "BEHIND_TEXT", "IN_FRONT_OF_TEXT"}) && (next.textWrap = s, true);
        else if (key == "textFlow") ok = text(value, s) && oneOf(s, {"BOTH_SIDES", "LEFT_ONLY", "RIGHT_ONLY", "LARGEST_ONLY"}) && (next.textFlow = s, true);
        else if (key == "vertRelTo") ok = text(value, s) && oneOf(s, {"PAPER", "PAGE", "PARA"}) && (next.vertRelTo = s, true);
        else if (key == "horzRelTo") ok = text(value, s) && oneOf(s, {"PAPER", "PAGE", "COLUMN", "PARA"}) && (next.horzRelTo = s, true);
        else if (key == "vertAlign") ok = text(value, s) && oneOf(s, {"TOP", "CENTER", "BOTTOM", "INSIDE", "OUTSIDE"}) && (next.vertAlign = s, true);
        else if (key == "horzAlign") ok = text(value, s) && oneOf(s, {"LEFT", "CENTER", "RIGHT", "INSIDE", "OUTSIDE"}) && (next.horzAlign = s, true);
        else if (key == "vertOffset") ok = number(value, d) && (next.vertOffset = static_cast<std::int32_t>(d), true);
        else if (key == "horzOffset") ok = number(value, d) && (next.horzOffset = static_cast<std::int32_t>(d), true);
        else if (key == "zOrder") ok = number(value, d) && (next.zOrder = static_cast<std::int32_t>(d), true);
        else if (key == "width") ok = number(value, d) && d >= 1 && (next.width = static_cast<std::uint32_t>(d), true);
        else if (key == "height") ok = number(value, d) && d >= 1 && (next.height = static_cast<std::uint32_t>(d), true);
        else if (key == "marginLeft") ok = number(value, d) && d >= 0 && (next.marginLeft = static_cast<std::int32_t>(d), true);
        else if (key == "marginRight") ok = number(value, d) && d >= 0 && (next.marginRight = static_cast<std::int32_t>(d), true);
        else if (key == "marginTop") ok = number(value, d) && d >= 0 && (next.marginTop = static_cast<std::int32_t>(d), true);
        else if (key == "marginBottom") ok = number(value, d) && d >= 0 && (next.marginBottom = static_cast<std::int32_t>(d), true);
        else if (key == "flip") {
            ok = text(value, s) && oneOf(s, {"horizontal", "vertical"}) && (flip = s, true);
        } else if (key == "skewX" || key == "skewY") {
            if (control->kind == model::ControlKind::Table || control->kind == model::ControlKind::Equation) {
                why = "표와 수식은 기울일 수 없습니다.";
                return {};
            }
            ok = number(value, d) && d > -90 && d < 90 &&
                 ((key == "skewX" ? skewX : skewY) = static_cast<std::int32_t>(std::lround(d)), true);
        } else if (key == "rotation" || key == "rotate") {
            // 회전(한글 개체 속성 회전각, 회전 단추의 왼쪽·오른쪽 90도 회전). 표·수식은 돌리지 않는다.
            if (control->kind == model::ControlKind::Table || control->kind == model::ControlKind::Equation) {
                why = "표와 수식은 돌릴 수 없습니다.";
                return {};
            }
            if (key == "rotation") {
                ok = number(value, d) && d > -360 && d < 360 && (turnTo = (static_cast<int>(std::lround(d)) % 360 + 360) % 360, true);
            } else {
                ok = text(value, s) && oneOf(s, {"left", "right"}) && (turnBy = s == "right" ? 90 : -90, true);
            }
        } else if (key == "order") {
            ok = text(value, s) && oneOf(s, {"front", "forward", "back", "backward"}) && (order = s, true);
        } else if (key == "lineType" || key == "lineColor" || key == "lineWidth" || key == "fillColor") {
            if (control->kind != model::ControlKind::Shape) {
                why = "개체 속성 " + key + " 은 도형에만 있습니다.";
                return {};
            }
            if (key == "lineWidth") {
                ok = number(value, d) && d >= 0 && (shapeLook.strokeWidth = static_cast<std::int32_t>(d), true);
            } else if (key == "lineType") {
                ok = text(value, s) && !s.empty() && (shapeLook.strokeType = s, true);
            } else if (key == "lineColor") {
                ok = text(value, s) && s.size() == 7 && s[0] == '#' && (shapeLook.strokeColor = s, true);
            } else {
                ok = text(value, s) && (s == "none" || (s.size() == 7 && s[0] == '#')) && (shapeLook.fillColor = s, true);
            }
        } else if (key == "cropLeft" || key == "cropTop" || key == "cropRight" || key == "cropBottom") {
            if (control->kind != model::ControlKind::Picture) {
                why = "개체 속성 " + key + " 은 그림에만 있습니다.";
                return {};
            }
            const std::int32_t ow = pictureLook.originWidth > 0 ? pictureLook.originWidth : pictureLook.clipRight;
            const std::int32_t oh = pictureLook.originHeight > 0 ? pictureLook.originHeight : pictureLook.clipBottom;
            const bool across = key == "cropLeft" || key == "cropRight";
            const std::int32_t whole = across ? ow : oh;
            ok = number(value, d) && d >= 0 && whole > 0 && d < whole;
            if (ok) {
                // 자르기가 없던 그림은 오른쪽·아래가 0 이다. 원본 크기로 채운 뒤 고친다.
                if (pictureLook.clipRight <= 0) pictureLook.clipRight = ow;
                if (pictureLook.clipBottom <= 0) pictureLook.clipBottom = oh;
                const auto v = static_cast<std::int32_t>(d);
                if (key == "cropLeft") pictureLook.clipLeft = v;
                else if (key == "cropTop") pictureLook.clipTop = v;
                else if (key == "cropRight") pictureLook.clipRight = ow - v;
                else pictureLook.clipBottom = oh - v;
                if (pictureLook.clipRight <= pictureLook.clipLeft || pictureLook.clipBottom <= pictureLook.clipTop) {
                    why = "자르고 남는 그림이 없습니다.";
                    return {};
                }
            }
        } else if (key == "bright" || key == "contrast" || key == "effect") {
            if (control->kind != model::ControlKind::Picture) {
                why = "개체 속성 " + key + " 은 그림에만 있습니다.";
                return {};
            }
            if (key == "effect") {
                ok = number(value, d) && d >= 0 && d <= 2 && (pictureLook.effect = static_cast<int>(d), true);
            } else {
                int& slot = key == "bright" ? pictureLook.bright : pictureLook.contrast;
                ok = number(value, d) && d >= -100 && d <= 100 && (slot = static_cast<int>(d), true);
            }
        } else {
            why = "개체 속성 이름 " + key + " 은 모릅니다.";
            return {};
        }
        if (!ok) {
            why = "개체 속성 " + key + " 의 값이 틀렸습니다.";
            return {};
        }
    }
    const model::Placement before = *control->placement;
    // 크기: 손잡이 크기 조절과 같다. 표는 칸 폭·줄 높이를 비율대로, 그 밖은 점·글상자·캡션을 같이 바꾼다.
    if (next.width != before.width || next.height != before.height) {
        if (control->table) {
            model::Table& table = *control->table;
            const std::vector<std::int32_t> cols =
                rescale(model::tableColumnWidths(table), static_cast<std::int32_t>(next.width));
            const std::vector<std::int32_t> rows =
                rescale(model::tableRowHeights(table), static_cast<std::int32_t>(next.height));
            for (model::TableCell& cell : table.cells) {
                if (!cols.empty()) cell.width = static_cast<std::uint32_t>(std::max(1, spanOf(cols, cell.col, cell.colSpan)));
                if (!rows.empty()) cell.height = static_cast<std::uint32_t>(std::max(1, spanOf(rows, cell.row, cell.rowSpan)));
                for (model::Paragraph& para : cell.content.paragraphs) para.lineSegs.clear();
            }
        } else {
            resizeControl(*control, before.width, before.height, next.width, next.height);
            for (model::SubList& list : control->subLists) {
                for (model::Paragraph& para : list.paragraphs) para.lineSegs.clear();
            }
        }
        // resizeControl 이 바꾼 크기 위에 나머지 값을 얹는다.
        next.groupX = control->placement->groupX;
        next.groupY = control->placement->groupY;
        next.scaleX = control->placement->scaleX;
        next.scaleY = control->placement->scaleY;
        next.matrixX = control->placement->matrixX;
        next.matrixY = control->placement->matrixY;
    }
    *control->placement = next;
    if (control->kind == model::ControlKind::Shape) {
        control->strokeType = shapeLook.strokeType;
        control->strokeColor = shapeLook.strokeColor;
        control->strokeWidth = shapeLook.strokeWidth;
        if (shapeLook.fillColor != control->fillColor) {
            control->fillColor = shapeLook.fillColor == "none" ? std::string() : shapeLook.fillColor;
            control->gradColor.clear();
        }
    }
    if (!order.empty()) reorderObject(doc, story, *control, order);
    // 회전(숨긴 한글 2024: 90도·30도로 확인). 돌린 상자의 가운데가 그대로다. 왼쪽(위) 맞춤이면 자리는
    // (전 상자 − 새 상자) / 2 만큼 옮긴다(0도 40×30mm 를 90도로: 8504 → 9921, 8504 → 7087).
    if (turnTo >= 0 || turnBy != 0) {
        model::Placement& place = *control->placement;
        const int now = ((place.rotation % 360) + 360) % 360;
        const int target = turnTo >= 0 ? turnTo : ((now + turnBy) % 360 + 360) % 360;
        if (target != now) {
            // 기울인 개체도 차지하는 상자의 가운데(상자/2 정수)를 지킨다(숨긴 한글 2024: 기울이기 20·15 에 30도 → 17008 → 15089).
            const model::ObjectGeometry before = model::objectGeometry(place.width, place.height, now, place.skewX, place.skewY,
                                                                       place.flipHorizontal, place.flipVertical);
            const model::ObjectGeometry after = model::objectGeometry(place.width, place.height, target, place.skewX,
                                                                      place.skewY, place.flipHorizontal, place.flipVertical);
            const std::int32_t dx = static_cast<std::int32_t>(before.boxWidth / 2) - static_cast<std::int32_t>(after.boxWidth / 2);
            const std::int32_t dy = static_cast<std::int32_t>(before.boxHeight / 2) - static_cast<std::int32_t>(after.boxHeight / 2);
            const auto sign = [](const std::string& align) {
                return align == "RIGHT" || align == "BOTTOM" || align == "OUTSIDE" ? -1 : align == "CENTER" ? 0 : 1;
            };
            if (!place.treatAsChar) {
                place.horzOffset += sign(place.horzAlign) * dx;
                place.vertOffset += sign(place.vertAlign) * dy;
            }
            place.rotation = target;
        }
    }
    // 기울이기(한글 개체 속성 기울이기 가로·세로, 도). 자리(왼쪽 위)는 그대로다(숨긴 한글 2024: 20도에 17008 그대로).
    if (skewX) control->placement->skewX = *skewX;
    if (skewY) control->placement->skewY = *skewY;
    // 좌우·상하 대칭(한글 ShapeObjHorzFlip·VertFlip). 누를 때마다 켜고 끈다. 점은 보이는 자리라 상자 안에서 뒤집는다.
    if (!flip.empty()) {
        const bool horizontal = flip == "horizontal";
        bool& flag = horizontal ? control->placement->flipHorizontal : control->placement->flipVertical;
        flag = !flag;
        // 돌린 개체를 뒤집으면 회전각이 반대가 되고 상자·자리는 그대로다(숨긴 한글 2024: 30도 → 330, 45도 → 315).
        model::Placement& place = *control->placement;
        place.rotation = (360 - ((place.rotation % 360) + 360) % 360) % 360;
        const std::int32_t boxW = static_cast<std::int32_t>(control->placement->width);
        const std::int32_t boxH = static_cast<std::int32_t>(control->placement->height);
        for (model::ShapePoint& point : control->points) {
            if (horizontal) point.x = boxW - point.x;
            else point.y = boxH - point.y;
        }
    }
    if (control->kind == model::ControlKind::Picture) {
        // 그림 자르기(숨긴 한글 2024 개체 속성 [그림] 그림 자르기: 30000×15000 그림 왼쪽 10mm → imgClip left 2834, 개체 너비
        // 27166, orgSz 30000 그대로, 배율 0.905533). 보이는 배율을 지키고 잘라 낸 만큼 개체가 줄어든다.
        const auto spanOf = [](std::int32_t from, std::int32_t to, std::int32_t whole) {
            return (to > 0 ? to : whole) - from;
        };
        const std::int32_t ow = control->picture.originWidth > 0 ? control->picture.originWidth : control->picture.clipRight;
        const std::int32_t oh = control->picture.originHeight > 0 ? control->picture.originHeight : control->picture.clipBottom;
        const std::int32_t oldSpanX = spanOf(control->picture.clipLeft, control->picture.clipRight, ow);
        const std::int32_t oldSpanY = spanOf(control->picture.clipTop, control->picture.clipBottom, oh);
        const std::int32_t newSpanX = spanOf(pictureLook.clipLeft, pictureLook.clipRight, ow);
        const std::int32_t newSpanY = spanOf(pictureLook.clipTop, pictureLook.clipBottom, oh);
        model::Placement& place = *control->placement;
        const auto refit = [](std::uint32_t& size, double& scale, std::int32_t oldSpan, std::int32_t newSpan) {
            if (oldSpan <= 0 || newSpan <= 0 || oldSpan == newSpan || size == 0) return;
            const auto next = static_cast<std::uint32_t>(
                std::max<long long>(1, std::llround(static_cast<double>(size) * newSpan / static_cast<double>(oldSpan))));
            scale = (scale > 1e-9 ? scale : 1.0) * static_cast<double>(next) / static_cast<double>(size);
            size = next;
        };
        refit(place.width, place.scaleX, oldSpanX, newSpanX);
        refit(place.height, place.scaleY, oldSpanY, newSpanY);
        control->picture.bright = pictureLook.bright;
        control->picture.contrast = pictureLook.contrast;
        control->picture.effect = pictureLook.effect;
        control->picture.clipLeft = pictureLook.clipLeft;
        control->picture.clipTop = pictureLook.clipTop;
        control->picture.clipRight = pictureLook.clipRight;
        control->picture.clipBottom = pictureLook.clipBottom;
    }
    // 글자처럼 취급을 바꾸면 문단 글자 수가 바뀌고, 글을 비켜 가는 개체는 그 글 묶음의 줄을 바꾼다. 저장된 줄은 낡았다.
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (paras) {
        if (wrapsText(before) || wrapsText(next)) {
            for (model::Paragraph& para : *paras) para.lineSegs.clear();
        } else {
            (*paras)[static_cast<std::size_t>(paragraph)].lineSegs.clear();
        }
    }
    return relayoutAfterObjectEdit(doc, pages, story, paragraph, useStoredLineHeight);
}

EditResult deleteObjectNow(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                           bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || !anchoredControl(doc, story, paragraph, item)) {
        why = "그 자리에 개체가 없습니다.";
        return {};
    }
    model::Paragraph& host = (*paras)[static_cast<std::size_t>(paragraph)];
    // 개체 앞 글자 수(캐럿 자리). 지운 뒤 캐럿을 그 자리에 둔다.
    int charIndex = 0;
    int index = 0;
    bool wrapped = false;
    for (std::size_t r = 0; r < host.runs.size(); ++r) {
        model::Run& run = host.runs[r];
        for (std::size_t i = 0; i < run.items.size(); ++i, ++index) {
            const model::InlineItem& entry = run.items[i];
            if (index != item) {
                if (entry.type == model::InlineItem::Type::Text) charIndex += static_cast<int>(utf8CodePointCount(entry.text));
                else if (isCaretCharControl(entry.control)) ++charIndex;
                continue;
            }
            wrapped = entry.control.placement && wrapsText(*entry.control.placement);
            run.items.erase(run.items.begin() + static_cast<std::ptrdiff_t>(i));
            if (run.items.empty() && host.runs.size() > 1) host.runs.erase(host.runs.begin() + static_cast<std::ptrdiff_t>(r));
            r = host.runs.size();
            break;
        }
    }
    if (wrapped) {
        for (model::Paragraph& para : *paras) para.lineSegs.clear();
    } else {
        host.lineSegs.clear();
    }
    EditResult result = relayoutAfterObjectEdit(doc, pages, story, paragraph, useStoredLineHeight);
    setPointerCursor(doc, pages, model::CursorPos{story, paragraph, charIndexToOffset(doc, pages, story, paragraph, charIndex)});
    const PointerCursor now = pointerCursor();
    result.cursor = now;
    return result;
}

EditResult deleteSelectedObjectsNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    if (readOnly() || !hold.selectedObject || !hold.objectHandles) return {};
    struct Ref {
        int story;
        int paragraph;
        int item;
    };
    std::vector<Ref> refs;
    const Node* first = hold.selectedObject;
    if (first->ext().obj().anchorStory > 0 && first->ext().obj().anchorItem >= 0) {
        refs.push_back({first->ext().obj().anchorStory, first->ext().obj().anchorParagraph, first->ext().obj().anchorItem});
    }
    for (const PointerHold::PickedObject& picked : hold.multiPicked) refs.push_back({picked.story, picked.paragraph, picked.item});
    if (refs.empty()) return {};
    // 뒤 항목부터 지운다. 앞 항목을 먼저 지우면 같은 문단 뒤 항목의 순번이 당겨진다.
    std::sort(refs.begin(), refs.end(), [](const Ref& a, const Ref& b) {
        if (a.story != b.story) return a.story > b.story;
        if (a.paragraph != b.paragraph) return a.paragraph > b.paragraph;
        return a.item > b.item;
    });
    refs.erase(std::unique(refs.begin(), refs.end(),
                           [](const Ref& a, const Ref& b) {
                               return a.story == b.story && a.paragraph == b.paragraph && a.item == b.item;
                           }),
               refs.end());
    hold.multiPicked.clear();
    EditResult result;
    for (const Ref& ref : refs) {
        std::string why;
        EditResult one = deleteObjectNow(doc, pages, ref.story, ref.paragraph, ref.item, useStoredLineHeight, why);
        if (one.documentChanged) result = one;
    }
    return result;
}

}  // namespace paint_detail

// 개체 회전을 켜고 끈다. 고른 개체가 없으면 끈다. 켰으면 참.
bool toggleObjectRotateMode() {
    PointerHold& hold = pointerHold();
    const bool canRotate = hold.selectedObject && hold.objectHandles && hold.selectedObject->role != "table";
    hold.rotateMode = canRotate && !hold.rotateMode;
    bumpPointerPicture();
    return hold.rotateMode;
}

namespace paint_detail {

// 끌어 돌린 각을 넣는다(objectProperties rotation 과 같은 규칙: 차지하는 상자의 가운데를 지킨다). 다시 고르고 회전을 켜 둔다.
EditResult rotateObjectNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    if (!hold.rotatePending) return {};
    hold.rotatePending = false;
    model::Control* control = anchoredControl(doc, hold.rotateStory, hold.rotateParagraph, hold.rotateItem);
    if (!control || !control->placement) return {};
    const int target = (((control->placement->rotation + hold.rotateDelta) % 360) + 360) % 360;
    const int story = hold.rotateStory;
    const int paragraph = hold.rotateParagraph;
    const int item = hold.rotateItem;
    std::string why;
    EditResult result = objectPropertiesNow(doc, pages, story, paragraph, item,
                                            FormatFields{{"rotation", static_cast<double>(target)}}, useStoredLineHeight, why);
    if (result.documentChanged && selectAnchoredObject(doc, pages, story, paragraph, item)) pointerHold().rotateMode = true;
    return result;
}

std::vector<ObjectSpot> selectedObjectSpots() {
    const PointerHold& hold = pointerHold();
    std::vector<ObjectSpot> spots;
    const Node* first = hold.selectedObject;
    if (!first || !hold.objectHandles) return spots;
    if (first->ext().obj().anchorStory > 0 && first->ext().obj().anchorItem >= 0) {
        spots.push_back({first->ext().obj().anchorStory, first->ext().obj().anchorParagraph, first->ext().obj().anchorItem});
    }
    for (const PointerHold::PickedObject& picked : hold.multiPicked) spots.push_back({picked.story, picked.paragraph, picked.item});
    return spots;
}

namespace {

// 개체를 쪽에서 (fromX, fromW) 에 그리던 놓기 값(offset·align)으로 (toX, toW) 에 그릴 offset.
// 왼쪽(위) 맞춤은 왼쪽(위) 끝, 오른쪽(아래) 맞춤은 오른쪽(아래) 끝, 가운데 맞춤은 가운데가 옮긴 만큼이다.
std::int32_t movedOffset(const std::string& align, std::int32_t offset, std::int32_t fromX, std::int32_t fromW,
                         std::int32_t toX, std::int32_t toW) {
    if (align == "RIGHT" || align == "BOTTOM" || align == "OUTSIDE") return offset + (fromX + fromW) - (toX + toW);
    if (align == "CENTER") return offset + (toX + toW / 2) - (fromX + fromW / 2);
    return offset + (toX - fromX);
}

// 묶음 안 개체 좌표는 가장 바깥 묶음 기준이다(HWPX·HWP 읽기와 같은 groupInRoot). 묶음 아래 모든 개체를 옮긴다.
void shiftGroupChildren(model::Control& control, std::int32_t dx, std::int32_t dy) {
    for (model::Control& child : control.children) {
        if (child.placement) {
            child.placement->groupX += dx;
            child.placement->groupY += dy;
        }
        shiftGroupChildren(child, dx, dy);
    }
}

bool groupable(const model::Control& control) {
    return control.placement && !control.placement->treatAsChar &&
           (control.kind == model::ControlKind::Shape || control.kind == model::ControlKind::Picture ||
            control.kind == model::ControlKind::Container || control.kind == model::ControlKind::Ole);
}

int sectionOfStory(model::Document& doc, int story) {
    const model::StorySlot* slot = resolveStory(doc, story);
    int sectionIndex = slot ? slot->sectionIndex : -1;
    if (sectionIndex < 0) {
        const model::StoryMap& map = ensureStoryMap(doc);
        if (const model::StorySlot* root = map.slot(map.rootSectionStory(story))) sectionIndex = root->sectionIndex;
    }
    return sectionIndex >= 0 && sectionIndex < static_cast<int>(doc.sections.size()) ? sectionIndex : -1;
}

// 문단의 항목 순번 자리에 있는 InlineItem(run 과 run 안 순번).
bool itemAt(model::Paragraph& para, int item, std::size_t& runIndex, std::size_t& itemIndex) {
    int index = 0;
    for (std::size_t r = 0; r < para.runs.size(); ++r) {
        for (std::size_t i = 0; i < para.runs[r].items.size(); ++i, ++index) {
            if (index == item) {
                runIndex = r;
                itemIndex = i;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// 개체 묶기(한글 Group, 숨긴 한글 2024 의 개체 묶기로 확인. 띄운 사각형 둘을 묶음):
// - 묶음은 문서 차례로 첫 개체의 자리에 놓이고 그 개체의 놓기 값(기준·본문과의 배치)을 받는다. 다른 개체는 제 자리에서 빠진다.
// - 묶음 상자는 개체들을 감싸는 상자이고 순서(zOrder)는 개체 가운데 가장 큰 값이다. 다른 개체의 순서는 그대로다.
// - 개체는 묶음 왼쪽 위에서 잰 자리(offset·이동 행렬)를 갖고, 본문과의 배치 자리 차지·순서 0 이다. 차례는 순서대로다.
// 글자처럼 취급한 개체·표·수식은 묶지 않고, 같은 쪽·같은 글 묶음의 개체만 묶는다.
EditResult groupObjectsNow(model::Document& doc, std::vector<Node>& pages, std::vector<ObjectSpot> spots,
                           bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    std::sort(spots.begin(), spots.end(), [](const ObjectSpot& a, const ObjectSpot& b) {
        if (a.story != b.story) return a.story < b.story;
        if (a.paragraph != b.paragraph) return a.paragraph < b.paragraph;
        return a.item < b.item;
    });
    spots.erase(std::unique(spots.begin(), spots.end(),
                            [](const ObjectSpot& a, const ObjectSpot& b) {
                                return a.story == b.story && a.paragraph == b.paragraph && a.item == b.item;
                            }),
                spots.end());
    if (spots.size() < 2) {
        why = "묶을 개체를 둘 이상 고르세요.";
        return {};
    }
    struct Member {
        ObjectSpot spot;
        PlacedBox box;
        model::Control* control = nullptr;
    };
    std::vector<Member> members;
    for (const ObjectSpot& spot : spots) {
        Member m{spot, {}, anchoredControl(doc, spot.story, spot.paragraph, spot.item)};
        if (!m.control) {
            why = "그 자리에 개체가 없습니다.";
            return {};
        }
        if (!groupable(*m.control)) {
            why = "글자처럼 취급한 개체나 표·수식은 묶을 수 없습니다.";
            return {};
        }
        if (!placedObjectBox(doc, pages, spot.story, spot.paragraph, spot.item, m.box)) {
            why = "그려진 개체가 아닙니다.";
            return {};
        }
        members.push_back(m);
    }
    for (const Member& m : members) {
        if (m.spot.story != members.front().spot.story || m.box.page != members.front().box.page) {
            why = "같은 쪽의 개체만 묶을 수 있습니다.";
            return {};
        }
    }
    std::int32_t left = members.front().box.x, top = members.front().box.y;
    std::int32_t right = left + members.front().box.width, bottom = top + members.front().box.height;
    std::int32_t zTop = members.front().control->placement->zOrder;
    bool wraps = false;
    for (const Member& m : members) {
        left = std::min(left, m.box.x);
        top = std::min(top, m.box.y);
        right = std::max(right, m.box.x + m.box.width);
        bottom = std::max(bottom, m.box.y + m.box.height);
        zTop = std::max(zTop, m.control->placement->zOrder);
        wraps = wraps || wrapsText(*m.control->placement);
    }
    const Member& first = members.front();
    const model::Placement base = *first.control->placement;

    model::Control group;
    group.kind = model::ControlKind::Container;
    group.ctrlId = "gso ";
    group.shapeType = "container";
    model::Placement place = base;
    place.width = static_cast<std::uint32_t>(right - left);
    place.height = static_cast<std::uint32_t>(bottom - top);
    place.horzOffset = movedOffset(base.horzAlign, base.horzOffset, first.box.x, first.box.width, left, right - left);
    place.vertOffset = movedOffset(base.vertAlign, base.vertOffset, first.box.y, first.box.height, top, bottom - top);
    place.zOrder = zTop;
    place.instanceId = 0;
    place.numberingType = "NONE";
    place.groupX = place.groupY = place.matrixX = place.matrixY = 0;
    place.scaleX = place.scaleY = 1;
    place.groupInRoot = false;
    place.flipHorizontal = place.flipVertical = false;
    group.placement = place;

    std::vector<const Member*> byZ;
    for (const Member& m : members) byZ.push_back(&m);
    std::stable_sort(byZ.begin(), byZ.end(), [](const Member* a, const Member* b) {
        return a->control->placement->zOrder < b->control->placement->zOrder;
    });
    for (const Member* m : byZ) {
        model::Control child = *m->control;
        model::Placement& cp = *child.placement;
        const std::int32_t dx = m->box.x - left;
        const std::int32_t dy = m->box.y - top;
        // 묶음이던 개체는 그 아래 개체의 기준이 새 묶음이 된다.
        shiftGroupChildren(child, dx, dy);
        cp.groupX = dx;
        cp.groupY = dy;
        cp.matrixX = cp.matrixY = 0;
        cp.groupInRoot = true;
        cp.treatAsChar = false;
        cp.textWrap = "TOP_AND_BOTTOM";
        cp.zOrder = 0;
        cp.horzRelTo = base.horzRelTo;
        cp.vertRelTo = base.vertRelTo;
        cp.horzOffset = cp.vertOffset = 0;
        child.caption.reset();
        group.children.push_back(std::move(child));
    }

    // 뒤 개체부터 빼고, 첫 개체 자리에 묶음을 둔다(첫 개체가 문서 차례로 가장 앞이라 순번이 그대로다).
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, first.spot.story);
    if (!paras) return {};
    const ObjectSpot firstSpot = first.spot;
    for (std::size_t i = members.size(); i-- > 1;) {
        model::Paragraph& host = (*paras)[static_cast<std::size_t>(members[i].spot.paragraph)];
        std::size_t r = 0, at = 0;
        if (!itemAt(host, members[i].spot.item, r, at)) continue;
        host.runs[r].items.erase(host.runs[r].items.begin() + static_cast<std::ptrdiff_t>(at));
        if (host.runs[r].items.empty() && host.runs.size() > 1) host.runs.erase(host.runs.begin() + static_cast<std::ptrdiff_t>(r));
        host.lineSegs.clear();
    }
    model::Paragraph& host = (*paras)[static_cast<std::size_t>(firstSpot.paragraph)];
    std::size_t r = 0, at = 0;
    if (!itemAt(host, firstSpot.item, r, at)) return {};
    host.runs[r].items[at].control = std::move(group);
    host.lineSegs.clear();
    if (wraps) {
        for (model::Paragraph& para : *paras) para.lineSegs.clear();
    }
    EditResult result = relayoutAfterObjectEdit(doc, pages, firstSpot.story, firstSpot.paragraph, useStoredLineHeight);
    selectAnchoredObject(doc, pages, firstSpot.story, firstSpot.paragraph, firstSpot.item);
    return result;
}

// 개체 풀기(한글 Ungroup, 숨긴 한글 2024 로 확인): 묶음 자리에 그 안 개체를 차례대로 꺼낸다.
// - 개체는 묶음의 놓기 값(기준·본문과의 배치)을 받고, 묶음 안에서 그리던 쪽 자리에 그대로 놓인다.
// - 순서는 구역의 개체를 순서대로 0 부터 다시 매기고, 묶음 순서 자리에 꺼낸 개체가 차례로 들어간다.
// - 꺼낸 개체를 모두 고른다.
EditResult ungroupObjectNow(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                            bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    model::Control* control = anchoredControl(doc, story, paragraph, item);
    if (!control || control->kind != model::ControlKind::Container || !control->placement || control->children.empty()) {
        why = "고른 개체가 묶음이 아닙니다.";
        return {};
    }
    if (control->placement->treatAsChar) {
        why = "글자처럼 취급한 묶음은 풀 수 없습니다.";
        return {};
    }
    PlacedBox box;
    if (!placedObjectBox(doc, pages, story, paragraph, item, box)) {
        why = "그려진 개체가 아닙니다.";
        return {};
    }
    const int sectionIndex = sectionOfStory(doc, story);
    const model::Placement gp = *control->placement;
    std::vector<model::Control> out;
    for (const model::Control& source : control->children) {
        if (!source.placement) continue;
        model::Control c = source;
        model::Placement& cp = *c.placement;
        const std::int32_t x = box.x + cp.groupX;
        const std::int32_t y = box.y + cp.groupY;
        const std::int32_t w = static_cast<std::int32_t>(cp.width);
        const std::int32_t h = static_cast<std::int32_t>(cp.height);
        // 묶음이던 개체는 그 아래 개체의 기준이 이 개체가 된다.
        shiftGroupChildren(c, -cp.groupX, -cp.groupY);
        model::Placement next = gp;
        next.width = cp.width;
        next.height = cp.height;
        next.horzOffset = movedOffset(gp.horzAlign, gp.horzOffset, box.x, box.width, x, w);
        next.vertOffset = movedOffset(gp.vertAlign, gp.vertOffset, box.y, box.height, y, h);
        next.instanceId = cp.instanceId;
        next.numberingType = cp.numberingType;
        next.scaleX = cp.scaleX;
        next.scaleY = cp.scaleY;
        next.flipHorizontal = cp.flipHorizontal;
        next.flipVertical = cp.flipVertical;
        next.attrKeep = cp.attrKeep;
        next.groupX = next.groupY = next.matrixX = next.matrixY = 0;
        next.groupInRoot = false;
        cp = next;
        out.push_back(std::move(c));
    }
    const std::int32_t groupZ = gp.zOrder;
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras) return {};
    model::Paragraph& host = (*paras)[static_cast<std::size_t>(paragraph)];
    std::size_t r = 0, at = 0;
    if (!itemAt(host, item, r, at)) return {};
    const std::size_t count = out.size();
    std::vector<model::InlineItem>& items = host.runs[r].items;
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(at));
    for (std::size_t i = 0; i < count; ++i) {
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(at + i), model::InlineItem::makeControl(std::move(out[i])));
    }
    // 순서를 다시 매긴다: 묶음 순서 자리에 꺼낸 개체가 차례로 들어간다.
    if (sectionIndex >= 0) {
        std::vector<model::Placement*> all;
        collectPlaced(doc.sections[static_cast<std::size_t>(sectionIndex)].paragraphs, all);
        std::vector<model::Placement*> taken;
        for (std::size_t i = 0; i < count; ++i) {
            model::Control& c = host.runs[r].items[at + i].control;
            if (c.placement) taken.push_back(&*c.placement);
        }
        std::vector<std::pair<std::pair<std::int32_t, int>, model::Placement*>> order;
        for (model::Placement* p : all) {
            const auto found = std::find(taken.begin(), taken.end(), p);
            if (found != taken.end()) order.push_back({{groupZ, 1 + static_cast<int>(found - taken.begin())}, p});
            else order.push_back({{p->zOrder, 0}, p});
        }
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t i = 0; i < order.size(); ++i) order[i].second->zOrder = static_cast<std::int32_t>(i);
    }
    if (wrapsText(gp)) {
        for (model::Paragraph& para : *paras) para.lineSegs.clear();
    }
    host.lineSegs.clear();
    EditResult result = relayoutAfterObjectEdit(doc, pages, story, paragraph, useStoredLineHeight);
    // 꺼낸 개체는 그 문단의 item, item+1, ... 자리다. 모두 고른다.
    if (selectAnchoredObject(doc, pages, story, paragraph, item)) {
        pointerHold().multiPicked.clear();
        for (std::size_t i = 1; i < count; ++i) {
            const int next = item + static_cast<int>(i);
            PlacedBox placed;
            if (placedObjectBox(doc, pages, story, paragraph, next, placed)) {
                pickAnchoredObject(doc, pages, placed.page, story, paragraph, next);
            }
        }
    }
    return result;
}

}  // namespace paint_detail

}  // namespace chwp::render
