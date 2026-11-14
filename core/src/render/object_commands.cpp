// 이름으로 가리킨 개체(글 묶음·문단·문단 안 항목 순번)의 고르기·속성·지우기.
//
// 편집기에서는 개체를 눌러 고르고 개체 속성 대화 상자(기본·여백 탭)나 Delete 로 하는 일이다. agent 는 화면 없이
// 개체 이름으로 같은 일을 한다. 값 이름은 placementFields(개체 놓기 읽기)와 같고 길이는 HWPUNIT 이다.
// 크기를 바꾸면 손잡이 크기 조절(resizeObjectNow)과 같은 규칙이다: 표는 칸 폭·줄 높이를 비율대로 나누고,
// 그 밖의 개체는 resizeControl 이 점·글상자 폭·캡션 폭을 같이 바꾼다.

#include <algorithm>
#include <cstdint>
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
    return out;
}

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
        else {
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

}  // namespace chwp::render
