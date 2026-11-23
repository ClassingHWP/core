// 개체 보호 해제, 문단 첫 글자 장식, 글상자 연결, 양식 개체(숨긴 한글 2024 로 본 동작).
//
// - 모든 보호 개체 해제하기(UnLockShapeObjectAll): 문서의 모든 개체 보호(lock)를 끈다.
// - 문단 첫 글자 장식(DropCap): 캐럿 문단의 첫 글자를 뺀 글상자(사각형)를 그 자리에 넣는다. 선·채우기 없음, 개체 보호,
//   크기 보호, 문단 기준 어울림(오른쪽만), 바깥 여백 오른쪽 = 간격(850), 글자 모양은 첫 글자 모양에 크기 = 상자 크기,
//   문단 모양은 가운데 정렬. 상자 크기는 글자 크기 + 줄 간격 × (줄 수 − 1)(10pt·160% 에서 두 줄 2600, 세 줄 4200).
//   여백 장식은 세 줄 크기로 왼쪽 여백에 둔다(가로 자리 = −(크기 + 간격)).
// - 글상자 연결(LinkTextBox): 앞 상자의 다음 번호를 뒤 상자 번호로 잇는다. 뒤 상자의 글은 앞 상자 글 끝에 붙인다.
// - 양식 개체(InsertFormCheck 등): 캐럿 자리에 글자처럼 넣는다. 이름은 종류마다 번호를 매기고, 글자 모양은 캐럿 글자
//   모양에 크기 9pt 다. 양식 편집 상태가 꺼져 있으면 선택 상자·라디오 단추를 눌러 고른다.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/picture.hpp"

namespace chwp::render {

// 그림 파일을 문서에 넣고 그 이름을 준다(edit_commands.cpp).
std::string addImageBin(model::Document& doc, const Bytes& image, const std::string& extension);

using namespace paint_detail;

namespace {

EditResult relayoutAllObjects(model::Document& doc, std::vector<Node>& pages, int story, int paragraph,
                         bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    hold.selectedObject = nullptr;
    hold.objectHandles = false;
    hold.resizeHandle = -1;
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    return makeEditResult(doc, true, true, story, paragraph, &relayout);
}

template <typename Fn>
void eachControl(std::vector<model::Paragraph>& paragraphs, const Fn& fn, int depth = 0) {
    if (depth > 8) return;
    for (model::Paragraph& para : paragraphs) {
        for (model::Run& run : para.runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Control) continue;
                fn(item.control);
                for (model::SubList& list : item.control.subLists) eachControl(list.paragraphs, fn, depth + 1);
                if (item.control.table) {
                    for (model::TableCell& cell : item.control.table->cells) eachControl(cell.content.paragraphs, fn, depth + 1);
                }
                for (model::Control& child : item.control.children) fn(child);
            }
        }
    }
}

template <typename Fn>
void eachDocControl(model::Document& doc, const Fn& fn) {
    for (model::Section& section : doc.sections) eachControl(section.paragraphs, fn);
}

const model::CharShape* charShapeOf(const model::Document& doc, std::uint32_t id) {
    if (id < doc.header.charShapes.size() && doc.header.charShapes[id].id == id) return &doc.header.charShapes[id];
    for (const auto& cs : doc.header.charShapes) {
        if (cs.id == id) return &cs;
    }
    return nullptr;
}

const model::ParaShape* paraShapeOf(const model::Document& doc, std::uint32_t id) {
    if (id < doc.header.paraShapes.size() && doc.header.paraShapes[id].id == id) return &doc.header.paraShapes[id];
    for (const auto& ps : doc.header.paraShapes) {
        if (ps.id == id) return &ps;
    }
    return nullptr;
}

std::int32_t maxZOrder(model::Document& doc) {
    std::int32_t z = -1;
    eachDocControl(doc, [&](model::Control& c) {
        if (c.placement) z = std::max(z, c.placement->zOrder);
    });
    return z;
}

// 첫 글자 장식 상자 크기: 글자 크기 + 줄 간격 × (줄 수 − 1). 줄 간격은 문단의 줄 간격 비율로 잰다.
std::int32_t dropCapSize(std::int32_t charHeight, const model::ParaShape* ps, int lines) {
    std::int32_t pitch = charHeight * 160 / 100;
    if (ps) {
        if (ps->lineSpacingType == "PERCENT") pitch = charHeight * ps->lineSpacing / 100;
        else if (ps->lineSpacingType == "FIXED") pitch = ps->lineSpacing;
        else if (ps->lineSpacingType == "BETWEEN_LINES") pitch = charHeight + ps->lineSpacing;
        else if (ps->lineSpacingType == "AT_LEAST") pitch = std::max(charHeight, ps->lineSpacing);
    }
    return charHeight + pitch * (lines - 1);
}

}  // namespace

namespace paint_detail {

EditResult unlockAllObjectsNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    if (readOnly()) return {};
    bool changed = false;
    eachDocControl(doc, [&](model::Control& c) {
        if (c.placement && c.placement->locked) {
            c.placement->locked = false;
            changed = true;
        }
    });
    if (!changed) return {};
    const PointerCursor cur = pointerCursor();
    return relayoutAllObjects(doc, pages, cur.story, cur.paragraph, useStoredLineHeight);
}

EditResult dropCapNow(model::Document& doc, std::vector<Node>& pages, int style, std::int32_t spacing,
                      bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) {
        why = "캐럿이 없습니다.";
        return {};
    }
    model::Paragraph* para = storyParagraph(doc, cur.story, cur.paragraph);
    if (!para) return {};
    // 이미 장식이 있으면 글자를 문단 앞에 되돌리고 상자를 뺀다.
    std::string restored;
    std::uint32_t restoredShape = model::kNoCharStyle;
    for (model::Run& run : para->runs) {
        for (std::size_t i = 0; i < run.items.size(); ++i) {
            model::InlineItem& item = run.items[i];
            if (item.type != model::InlineItem::Type::Control || !item.control.placement ||
                item.control.placement->dropCap.empty()) {
                continue;
            }
            if (!item.control.subLists.empty()) {
                for (const model::Paragraph& inner : item.control.subLists.front().paragraphs) {
                    for (const model::Run& r : inner.runs) {
                        for (const model::InlineItem& t : r.items) {
                            if (t.type == model::InlineItem::Type::Text) {
                                restored += t.text;
                                if (restoredShape == model::kNoCharStyle) restoredShape = r.charShapeId;
                            }
                        }
                    }
                }
            }
            run.items.erase(run.items.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        if (!restored.empty()) break;
    }
    // 되돌린 글자는 원래 글자 모양(장식 글자 모양의 크기만 키운 것)이 아니라 다음 글자 모양을 따른다.
    if (!restored.empty()) {
        for (model::Run& run : para->runs) {
            for (std::size_t i = 0; i < run.items.size(); ++i) {
                if (run.items[i].type != model::InlineItem::Type::Text) continue;
                run.items[i].text = restored + run.items[i].text;
                restored.clear();
                break;
            }
            if (restored.empty()) break;
        }
        if (!restored.empty()) {
            model::Run run;
            run.charShapeId = para->runs.empty() ? 0 : para->runs.back().charShapeId;
            run.items.push_back(model::InlineItem::makeText(restored));
            para->runs.push_back(std::move(run));
        }
    }
    if (style <= 0) {
        para->lineSegs.clear();
        return relayoutAllObjects(doc, pages, cur.story, cur.paragraph, useStoredLineHeight);
    }
    // 첫 글자: 문단 앞 컨트롤 뒤의 첫 글.
    model::Run* hostRun = nullptr;
    std::size_t hostIndex = 0;
    for (model::Run& run : para->runs) {
        for (std::size_t i = 0; i < run.items.size(); ++i) {
            if (run.items[i].type == model::InlineItem::Type::Text && !run.items[i].text.empty()) {
                hostRun = &run;
                hostIndex = i;
                break;
            }
        }
        if (hostRun) break;
    }
    if (!hostRun) {
        why = "장식할 글자가 없습니다.";
        return {};
    }
    std::string& text = hostRun->items[hostIndex].text;
    std::size_t len = 1;
    const auto lead = static_cast<unsigned char>(text[0]);
    if (lead >= 0xF0) len = 4;
    else if (lead >= 0xE0) len = 3;
    else if (lead >= 0xC0) len = 2;
    len = std::min(len, text.size());
    const std::string first = text.substr(0, len);
    if (first == " " || first == "\t" || first == "\n") {
        why = "문단 첫 글자가 빈칸입니다.";
        return {};
    }
    text.erase(0, len);
    const std::uint32_t baseShape = hostRun->charShapeId;
    const model::CharShape* base = charShapeOf(doc, baseShape);
    const std::int32_t charHeight = base ? base->height : 1000;
    const int lines = style == 1 ? 2 : 3;
    const std::int32_t size = dropCapSize(charHeight, paraShapeOf(doc, para->paraShapeId), lines);
    model::CharShape bigShape = base ? *base : model::CharShape{};
    bigShape.height = size;
    const std::uint32_t bigId = internCharShape(doc, bigShape);
    model::ParaShape centered = paraShapeOf(doc, para->paraShapeId) ? *paraShapeOf(doc, para->paraShapeId) : model::ParaShape{};
    centered.align = "CENTER";
    centered.indent = 0;
    centered.marginLeft = 0;
    centered.marginRight = 0;
    centered.spacingPrev = 0;
    centered.spacingNext = 0;
    centered.headingType = "NONE";
    centered.borderFillId = 0;
    const std::uint32_t centeredId = internParaShape(doc, centered);

    model::Control box;
    box.kind = model::ControlKind::Shape;
    box.shapeType = "rect";
    box.strokeType = "NONE";
    box.strokeWidth = 0;
    box.strokeEnd = 0;
    model::Placement p;
    p.treatAsChar = false;
    p.flowWithText = true;
    p.allowOverlap = true;
    p.textWrap = "SQUARE";
    p.textFlow = "RIGHT_ONLY";
    p.vertRelTo = "PARA";
    p.horzRelTo = "PARA";
    p.vertAlign = "TOP";
    p.horzAlign = "LEFT";
    p.width = static_cast<std::uint32_t>(size);
    p.height = static_cast<std::uint32_t>(size);
    p.marginRight = spacing;
    p.horzOffset = style == 3 ? -(size + spacing) : 0;
    p.zOrder = maxZOrder(doc) + 1;
    p.locked = true;
    p.sizeProtect = true;
    p.dropCap = style == 1 ? "DoubleLine" : style == 2 ? "TripleLine" : "Margin";
    box.placement = p;
    model::SubList list;
    list.vertAlign = "CENTER";
    model::Paragraph inner;
    inner.paraShapeId = centeredId;
    model::Run innerRun;
    innerRun.charShapeId = bigId;
    innerRun.items.push_back(model::InlineItem::makeText(first));
    inner.runs.push_back(std::move(innerRun));
    list.paragraphs.push_back(std::move(inner));
    box.subLists.push_back(std::move(list));
    box.boxTextWidth = 0xFFFFFFFFu;
    hostRun->items.insert(hostRun->items.begin() + static_cast<std::ptrdiff_t>(hostIndex),
                          model::InlineItem::makeControl(std::move(box)));
    para->lineSegs.clear();
    return relayoutAllObjects(doc, pages, cur.story, cur.paragraph, useStoredLineHeight);
}

EditResult linkTextBoxNow(model::Document& doc, std::vector<Node>& pages, const ObjectSpot& from, const ObjectSpot& to,
                          bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    model::Control* a = anchoredControl(doc, from.story, from.paragraph, from.item);
    model::Control* b = anchoredControl(doc, to.story, to.paragraph, to.item);
    if (!a || !b || a == b) {
        why = "연결할 두 글상자를 고르십시오.";
        return {};
    }
    if (a->subLists.empty() || b->subLists.empty() || a->kind != model::ControlKind::Shape ||
        b->kind != model::ControlKind::Shape) {
        why = "글상자끼리만 연결합니다.";
        return {};
    }
    model::SubList& la = a->subLists.front();
    model::SubList& lb = b->subLists.front();
    if (la.linkNextId != 0) {
        why = "이미 다음 글상자가 연결된 글상자입니다.";
        return {};
    }
    if (lb.linkId != 0) {
        bool hasPrev = false;
        eachDocControl(doc, [&](model::Control& c) {
            if (!c.subLists.empty() && c.subLists.front().linkNextId == lb.linkId) hasPrev = true;
        });
        if (hasPrev) {
            why = "이미 앞 글상자가 연결된 글상자입니다.";
            return {};
        }
    }
    std::uint32_t top = 1;
    eachDocControl(doc, [&](model::Control& c) {
        for (const model::SubList& l : c.subLists) top = std::max({top, l.linkId, l.linkNextId});
    });
    if (la.linkId == 0) la.linkId = ++top;
    if (lb.linkId == 0) lb.linkId = ++top;
    la.linkNextId = lb.linkId;
    // 연결한 글은 모두 앞 상자에 있다(한글 2024). 뒤 상자에 글이 있었으면 앞 상자 끝에 붙인다.
    const auto blank = [](const model::SubList& l) {
        for (const model::Paragraph& para : l.paragraphs) {
            if (!para.text().empty()) return false;
        }
        return true;
    };
    if (!blank(lb)) {
        for (model::Paragraph& para : lb.paragraphs) {
            para.lineSegs.clear();
            la.paragraphs.push_back(std::move(para));
        }
    }
    model::Paragraph empty;
    empty.paraShapeId = lb.paragraphs.empty() ? 0 : lb.paragraphs.front().paraShapeId;
    model::Run run;
    run.charShapeId = la.paragraphs.empty() || la.paragraphs.back().runs.empty() ? 0 : la.paragraphs.back().runs.back().charShapeId;
    empty.runs.push_back(std::move(run));
    lb.paragraphs.clear();
    lb.paragraphs.push_back(std::move(empty));
    for (model::Paragraph& para : la.paragraphs) para.lineSegs.clear();
    // 연결한 앞 상자는 크기를 고정한다(한글: 크기 고정).
    if (a->placement) a->placement->sizeProtect = true;
    return relayoutAllObjects(doc, pages, from.story, from.paragraph, useStoredLineHeight);
}

EditResult unlinkTextBoxNow(model::Document& doc, std::vector<Node>& pages, const ObjectSpot& spot,
                            bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    model::Control* a = anchoredControl(doc, spot.story, spot.paragraph, spot.item);
    if (!a || a->subLists.empty() || a->subLists.front().linkNextId == 0) {
        why = "다음 글상자가 연결된 글상자가 아닙니다.";
        return {};
    }
    // 연결을 끊으면 뒤 상자부터의 글은 앞 상자에 남는다(넘치는 글은 앞 상자 아래로 숨는다).
    a->subLists.front().linkNextId = 0;
    for (model::Paragraph& para : a->subLists.front().paragraphs) para.lineSegs.clear();
    return relayoutAllObjects(doc, pages, spot.story, spot.paragraph, useStoredLineHeight);
}

EditResult insertFormNow(model::Document& doc, std::vector<Node>& pages, const std::string& type,
                         bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    static const char* const kTypes[] = {"btn", "checkBtn", "comboBox", "radioBtn", "edit"};
    if (std::find_if(std::begin(kTypes), std::end(kTypes), [&](const char* t) { return type == t; }) == std::end(kTypes)) {
        why = "양식 개체 종류는 btn·checkBtn·comboBox·radioBtn·edit 입니다.";
        return {};
    }
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) {
        why = "캐럿이 없습니다.";
        return {};
    }
    if (!storyParagraph(doc, cur.story, cur.paragraph)) return {};
    clearComposition(hold);
    int paraIdx = cur.paragraph;
    int charIndex = 0;
    if (cur.hasSelection) {
        deleteSelectionRange(doc, pages, cur.story, cur.startParagraph, cur.startOffset, cur.endParagraph,
                             cur.endOffset, paraIdx, charIndex);
    } else {
        charIndex = offsetToCharIndex(doc, pages, cur.story, paraIdx, cur.offset);
    }
    model::Paragraph* host = storyParagraph(doc, cur.story, paraIdx);
    if (!host) return {};
    const std::uint32_t baseShape = charShapeAtParagraphChar(*host, charIndex, false);
    // 이름 번호: 같은 종류의 개수 + 1. 탭 차례는 모든 양식 개체 수 + 1(숨긴 한글 2024).
    int sameKind = 0;
    int forms = 0;
    eachDocControl(doc, [&](model::Control& c) {
        if (c.kind != model::ControlKind::Form || !c.form) return;
        ++forms;
        if (c.form->type == type) ++sameKind;
    });
    model::FormObject f;
    f.type = type;
    const std::string n = std::to_string(sameKind + 1);
    std::uint32_t width = 7087;
    if (type == "btn") {
        f.name = "PushButton" + n;
        f.caption = "명령 단추" + n;
        f.borderType = 4;
    } else if (type == "checkBtn") {
        f.name = "CheckBox" + n;
        f.caption = "선택 상자" + n;
        f.backColor = "#FFFFFF";
        width = 9921;
    } else if (type == "radioBtn") {
        f.name = "RadioButton" + n;
        f.caption = "라디오 단추" + n;
        f.backColor = "#FFFFFF";
        width = 8504;
    } else if (type == "comboBox") {
        f.name = "ComboBox" + n;
        f.borderType = 5;
        width = 9921;
    } else {
        f.name = "Edit" + n;
        f.borderType = 5;
    }
    f.tabOrder = forms + 1;
    model::CharShape small = charShapeOf(doc, baseShape) ? *charShapeOf(doc, baseShape) : model::CharShape{};
    small.height = 900;
    f.charShapeId = internCharShape(doc, small);
    model::Control form;
    form.kind = model::ControlKind::Form;
    form.ctrlId = "form";
    model::Placement p;
    p.treatAsChar = true;
    p.flowWithText = true;
    p.allowOverlap = true;
    p.textWrap = "TOP_AND_BOTTOM";
    p.vertRelTo = "PARA";
    p.horzRelTo = "COLUMN";
    p.width = width;
    p.height = 1984;
    form.placement = p;
    form.form = std::move(f);

    model::Paragraph right = splitParagraphAt(*host, charIndex);
    model::Run run;
    run.charShapeId = baseShape;
    run.items.push_back(model::InlineItem::makeControl(std::move(form)));
    host->runs.push_back(std::move(run));
    for (model::Run& r : right.runs) {
        if (!r.items.empty()) host->runs.push_back(std::move(r));
    }
    host->lineSegs.clear();
    hold.preferredX = -1;
    EditResult result = relayoutAllObjects(doc, pages, cur.story, paraIdx, useStoredLineHeight);
    const int afterOff = charIndexToOffset(doc, pages, cur.story, paraIdx, charIndex + 1);
    setPointerCursor(doc, pages, model::CursorPos{cur.story, paraIdx, afterOff});
    return result;
}

EditResult pressFormNow(model::Document& doc, std::vector<Node>& pages, const ObjectSpot& spot,
                        bool useStoredLineHeight) {
    if (readOnly()) return {};
    model::Control* c = anchoredControl(doc, spot.story, spot.paragraph, spot.item);
    if (!c || c->kind != model::ControlKind::Form || !c->form || !c->form->enabled) return {};
    model::FormObject& f = *c->form;
    if (f.type == "checkBtn") {
        f.value = f.triState ? (f.value + 1) % 3 : (f.value == 0 ? 1 : 0);
    } else if (f.type == "radioBtn") {
        if (f.value == 1) return {};
        // 같은 라디오 묶음(radioGroupName)의 다른 단추는 푼다.
        const std::string group = f.radioGroupName;
        eachDocControl(doc, [&](model::Control& other) {
            if (&other != c && other.kind == model::ControlKind::Form && other.form && other.form->type == "radioBtn" &&
                other.form->radioGroupName == group) {
                other.form->value = 0;
            }
        });
        f.value = 1;
    } else {
        return {};
    }
    return relayoutAllObjects(doc, pages, spot.story, spot.paragraph, useStoredLineHeight);
}

// 여러 개체 맞춤·배분·크기 같게(한글 도형 탭 맞춤·너비를 같게·높이를 같게·너비 및 높이를 같게).
// 숨긴 한글 2024 로 확인한 규칙:
// - 맞춤(left·center·right·top·middle·bottom)과 크기는 마지막에 고른 개체(한글이 초록 손잡이로 보이는 개체)가 기준이다.
//   가운데는 기준 왼쪽 + (기준 폭 − 폭)/2 를 반올림한다(22677 + 1417.5 → 24095).
// - 배분(hspace·vspace)은 자리 차례로 늘어놓아 양 끝 개체는 두고 사이 간격을 같게 한다.
// - 크기는 왼쪽 위를 두고 바꾼다(개체 속성 크기와 같다).
// 글자처럼 취급한 개체는 옮기지 않는다. 같은 쪽의 개체만 맞춘다.
EditResult alignObjectsNow(model::Document& doc, std::vector<Node>& pages, const std::vector<ObjectSpot>& spots,
                           const std::string& mode, bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    const bool sizing = mode == "width" || mode == "height" || mode == "size";
    const bool spacing = mode == "hspace" || mode == "vspace";
    const bool aligning = mode == "left" || mode == "center" || mode == "right" || mode == "top" || mode == "middle" ||
                          mode == "bottom";
    if (!sizing && !spacing && !aligning) {
        why = "맞춤 종류는 left·center·right·top·middle·bottom·hspace·vspace·width·height·size 입니다.";
        return {};
    }
    struct Member {
        ObjectSpot spot;
        PlacedBox box;
        model::Placement place;
    };
    std::vector<Member> members;
    for (const ObjectSpot& spot : spots) {
        const bool seen = std::any_of(members.begin(), members.end(), [&](const Member& m) {
            return m.spot.story == spot.story && m.spot.paragraph == spot.paragraph && m.spot.item == spot.item;
        });
        if (seen) continue;
        const model::Control* control = anchoredControl(doc, spot.story, spot.paragraph, spot.item);
        Member m{spot, {}, {}};
        if (!control || !control->placement || !placedObjectBox(doc, pages, spot.story, spot.paragraph, spot.item, m.box)) {
            why = "그 자리에 그려진 개체가 없습니다.";
            return {};
        }
        m.place = *control->placement;
        members.push_back(m);
    }
    if (members.size() < (spacing ? 3u : 2u)) {
        why = spacing ? "간격을 같게 할 개체를 셋 이상 고르세요." : "맞출 개체를 둘 이상 고르세요.";
        return {};
    }
    for (const Member& m : members) {
        if (m.box.page != members.front().box.page) {
            why = "같은 쪽의 개체만 맞출 수 있습니다.";
            return {};
        }
    }
    struct Change {
        ObjectSpot spot;
        FormatFields fields;
    };
    std::vector<Change> changes;
    const Member& ref = members.back();
    if (sizing) {
        for (std::size_t i = 0; i + 1 < members.size(); ++i) {
            FormatFields fields;
            if (mode != "height" && members[i].place.width != ref.place.width) {
                fields.emplace_back("width", static_cast<double>(ref.place.width));
            }
            if (mode != "width" && members[i].place.height != ref.place.height) {
                fields.emplace_back("height", static_cast<double>(ref.place.height));
            }
            if (!fields.empty()) changes.push_back({members[i].spot, std::move(fields)});
        }
    } else {
        const bool across = mode == "left" || mode == "center" || mode == "right" || mode == "hspace";
        const auto pos = [&](const Member& m) { return across ? m.box.x : m.box.y; };
        const auto len = [&](const Member& m) { return across ? m.box.width : m.box.height; };
        std::vector<std::pair<const Member*, std::int32_t>> targets;
        if (aligning) {
            for (std::size_t i = 0; i + 1 < members.size(); ++i) {
                const Member& m = members[i];
                std::int32_t to = pos(ref);
                if (mode == "center" || mode == "middle") {
                    to = pos(ref) + static_cast<std::int32_t>(std::lround((len(ref) - len(m)) / 2.0));
                } else if (mode == "right" || mode == "bottom") {
                    to = pos(ref) + len(ref) - len(m);
                }
                targets.emplace_back(&m, to);
            }
        } else {
            std::vector<const Member*> order;
            for (const Member& m : members) order.push_back(&m);
            std::stable_sort(order.begin(), order.end(), [&](const Member* a, const Member* b) { return pos(*a) < pos(*b); });
            const Member& first = *order.front();
            const Member& last = *order.back();
            std::int64_t sum = 0;
            for (const Member* m : order) sum += len(*m);
            const double gap = static_cast<double>(pos(last) + len(last) - pos(first) - sum) / static_cast<double>(order.size() - 1);
            double at = pos(first) + len(first);
            for (std::size_t i = 1; i + 1 < order.size(); ++i) {
                at += gap;
                targets.emplace_back(order[i], static_cast<std::int32_t>(std::lround(at)));
                at += len(*order[i]);
            }
        }
        for (const auto& [m, to] : targets) {
            if (m->place.treatAsChar || to == pos(*m)) continue;
            const std::string& align = across ? m->place.horzAlign : m->place.vertAlign;
            const std::int32_t offset = across ? m->place.horzOffset : m->place.vertOffset;
            std::int32_t next = offset + (to - pos(*m));
            if (align == "RIGHT" || align == "BOTTOM" || align == "OUTSIDE") next = offset - (to - pos(*m));
            changes.push_back({m->spot, FormatFields{{across ? "horzOffset" : "vertOffset", static_cast<double>(next)}}});
        }
    }
    if (changes.empty()) {
        why = "이미 맞춰져 있습니다.";
        return {};
    }
    EditResult result;
    for (const Change& change : changes) {
        std::string err;
        EditResult one = objectPropertiesNow(doc, pages, change.spot.story, change.spot.paragraph, change.spot.item,
                                             change.fields, useStoredLineHeight, err);
        if (!one.documentChanged) {
            if (!err.empty()) why = err;
            continue;
        }
        result = one;
    }
    // 맞춘 개체들을 그대로 다시 고른다(한글: 맞춘 뒤에도 여러 개체가 골라져 있어 이어서 맞출 수 있다). 고른 차례도
    // 그대로라 기준 개체(마지막에 고른 것)가 바뀌지 않는다.
    if (result.documentChanged) {
        const int pageCount = static_cast<int>(pages.size());
        for (int p = 0; p < pageCount; ++p) {
            if (reselectAnchoredObject(doc, pages, p, spots.front().story, spots.front().paragraph, spots.front().item)) break;
        }
        for (std::size_t i = 1; i < spots.size(); ++i) {
            for (int p = 0; p < pageCount; ++p) {
                if (pickAnchoredObject(doc, pages, p, spots[i].story, spots[i].paragraph, spots[i].item)) break;
            }
        }
    }
    return result;
}

// 그림 바꾸기(한글 ChangePictureDlg). 숨긴 한글 2024: 개체 크기·자리·밝기·대비·효과·그림자·자르기 왼쪽·위는 그대로 두고,
// 그림 파일과 원래 그림 크기(imgDim)만 새 그림으로 바꾼다. 자르기 오른쪽·아래는 새 그림 크기가 된다.
// (원래 그림 크기는 그림 넣기와 같이 픽셀 × 75 로 잡는다. 한글은 150 픽셀 폭을 11280 으로 적었다.)
EditResult replacePictureNow(model::Document& doc, std::vector<Node>& pages, const ObjectSpot& spot, const Bytes& image,
                             bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    model::Control* c = anchoredControl(doc, spot.story, spot.paragraph, spot.item);
    if (!c || c->kind != model::ControlKind::Picture) {
        why = "바꿀 그림을 고르십시오.";
        return {};
    }
    int pixelW = 0;
    int pixelH = 0;
    std::string extension;
    if (!imageInfo(image.data(), image.size(), pixelW, pixelH, extension)) {
        why = "png·jpg·gif·bmp 그림 파일이 아닙니다.";
        return {};
    }
    const std::string oldBin = c->binItemId;
    c->binItemId = addImageBin(doc, image, extension);
    const std::int32_t w = static_cast<std::int32_t>(std::min<std::int64_t>(0x7FFFFFFF, std::int64_t{pixelW} * 75));
    const std::int32_t h = static_cast<std::int32_t>(std::min<std::int64_t>(0x7FFFFFFF, std::int64_t{pixelH} * 75));
    c->picture.originWidth = w;
    c->picture.originHeight = h;
    c->picture.clipRight = w;
    c->picture.clipBottom = h;
    // 다른 그림이 쓰지 않는 옛 그림 파일은 빼지 않는다(되돌리기가 문서 전체를 되살리고, 저장은 쓰는 것만 담는다).
    (void)oldBin;
    EditResult result = relayoutAllObjects(doc, pages, spot.story, spot.paragraph, useStoredLineHeight);
    // 바꾼 그림을 그대로 골라 둔다(이어서 그림 탭 명령을 쓸 수 있다).
    for (int p = 0; p < static_cast<int>(pages.size()); ++p) {
        if (reselectAnchoredObject(doc, pages, p, spot.story, spot.paragraph, spot.item)) break;
    }
    return result;
}

}  // namespace paint_detail

}  // namespace chwp::render
