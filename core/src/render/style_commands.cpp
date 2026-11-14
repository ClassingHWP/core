// 스타일 대화 상자 명령(한글 2024 스타일 F6). 숨긴 한글에서 잰 동작:
// - 스타일을 지우면 그 스타일을 쓰던 문단은 고른 대신 스타일이 된다. 직접 준 모양은 남는다(스타일 적용과 같다).
//   뒤 스타일 번호는 하나씩 당겨지고, 지운 스타일을 다음 스타일로 쓰던 스타일은 다음 스타일이 자기 자신이 된다.
// - 글자 스타일은 글(hp:t charStyleIDRef, HWP 영역 종류 1)에 걸린다. 해제하면 문단 스타일 글자 모양으로 돌아간다.
// 스타일을 고치면(편집·현재 모양으로 바꾸기) 그 스타일을 쓰는 문단·글이 옛 스타일에서 새 스타일로 바뀐다.
// 되돌리기는 구역째(스타일 표 포함) 한 칸이다(undo.cpp).

#include "chwp/render/style_commands.hpp"

#include <algorithm>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "chwp/render/format_view.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

const char* kNoStyleMessage = "스타일을 찾을 수 없습니다.";

int styleIndex(const model::Document& doc, std::uint32_t id) {
    for (std::size_t i = 0; i < doc.header.styles.size(); ++i) {
        if (doc.header.styles[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

bool isCharStyle(const model::Style& st) { return st.type == "CHAR"; }

// 문서의 모든 글 조각(본문·바탕쪽·표 칸·글상자·머리말·주석).
void forEachTextRun(model::Document& doc, const std::function<void(model::Paragraph&, model::Run&)>& fn) {
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        for (model::Run& run : para.runs) fn(para, run);
    });
}

bool runHasCharStyle(const model::Run& run, std::uint32_t id) {
    for (const model::InlineItem& item : run.items) {
        if (item.type == model::InlineItem::Type::Text && item.charStyleId == id) return true;
    }
    return false;
}

// 문단 스타일 was → now 로 바뀐 스타일을 쓰는 문단을 모두 다시 입힌다.
void restyleParagraphsOf(model::Document& doc, const model::Style& was, const model::Style& now) {
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        if (para.styleId == was.id) restyleParagraph(doc, para, &was, now);
    });
}

// 글자 스타일 was → now 로 바뀐 글을 다시 입힌다. now 가 문단 스타일이면 글자 스타일을 푼다.
void restyleCharTextOf(model::Document& doc, const model::Style& was, const model::Style& now) {
    const bool release = !isCharStyle(now);
    forEachTextRun(doc, [&](model::Paragraph& para, model::Run& run) {
        if (!runHasCharStyle(run, was.id)) return;
        if (release) {
            // 문단 스타일 글자 모양으로 돌린다. 글자 스타일과 다르게 직접 준 속성은 남는다.
            const model::Style* paraStyle = Painter::findStyle(doc, para.styleId);
            if (paraStyle) {
                const model::Style target = *paraStyle;
                run.charShapeId = restyleCharShapeId(doc, run.charShapeId, &was, target);
            }
        } else {
            run.charShapeId = restyleCharShapeId(doc, run.charShapeId, &was, now);
        }
        for (model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text && item.charStyleId == was.id) {
                item.charStyleId = release ? model::kNoCharStyle : now.id;
            }
        }
        para.lineSegs.clear();
    });
}

// 글자 번호 at 에서 글 조각(run)을 둘로 나눈다. 글 한가운데면 글도 나눈다.
void splitRunsAt(model::Paragraph& para, int at) {
    int pos = 0;
    for (std::size_t r = 0; r < para.runs.size(); ++r) {
        model::Run& run = para.runs[r];
        for (std::size_t i = 0; i < run.items.size(); ++i) {
            if (pos == at && i > 0) {
                model::Run tail;
                tail.charShapeId = run.charShapeId;
                tail.items.assign(run.items.begin() + static_cast<std::ptrdiff_t>(i), run.items.end());
                run.items.erase(run.items.begin() + static_cast<std::ptrdiff_t>(i), run.items.end());
                para.runs.insert(para.runs.begin() + static_cast<std::ptrdiff_t>(r) + 1, std::move(tail));
                return;
            }
            model::InlineItem& item = run.items[i];
            int len = 0;
            if (item.type == model::InlineItem::Type::Text) len = utf8CodePointCount(item.text);
            else if (isCaretCharControl(item.control)) len = 1;
            if (item.type == model::InlineItem::Type::Text && at > pos && at < pos + len) {
                model::InlineItem right = item;
                right.text = utf8SliceCodePoints(item.text, at - pos, len - (at - pos));
                item.text = utf8SliceCodePoints(item.text, 0, at - pos);
                model::Run tail;
                tail.charShapeId = run.charShapeId;
                tail.items.push_back(std::move(right));
                tail.items.insert(tail.items.end(), run.items.begin() + static_cast<std::ptrdiff_t>(i) + 1, run.items.end());
                run.items.erase(run.items.begin() + static_cast<std::ptrdiff_t>(i) + 1, run.items.end());
                para.runs.insert(para.runs.begin() + static_cast<std::ptrdiff_t>(r) + 1, std::move(tail));
                return;
            }
            pos += len;
        }
    }
}

// 이웃한 같은 모양 글 조각과 같은 글자 스타일·형광펜 글을 하나로 합친다(글자 스타일을 푼 뒤).
void mergeRuns(model::Paragraph& para) {
    std::vector<model::Run> out;
    for (model::Run& run : para.runs) {
        if (!out.empty() && out.back().charShapeId == run.charShapeId) {
            for (model::InlineItem& item : run.items) out.back().items.push_back(std::move(item));
        } else {
            out.push_back(std::move(run));
        }
    }
    for (model::Run& run : out) {
        std::vector<model::InlineItem> items;
        for (model::InlineItem& item : run.items) {
            if (!items.empty() && item.type == model::InlineItem::Type::Text &&
                items.back().type == model::InlineItem::Type::Text && items.back().markpen == item.markpen &&
                items.back().charStyleId == item.charStyleId && items.back().ignoreSpacing == item.ignoreSpacing &&
                !items.back().synthetic && !item.synthetic) {
                items.back().text += item.text;
            } else {
                items.push_back(std::move(item));
            }
        }
        run.items = std::move(items);
    }
    para.runs = std::move(out);
}

// 스타일 번호를 바꾼다(지우기·차례 바꾸기). map 은 옛 번호 → 새 번호다.
void remapStyleIds(model::Document& doc, const std::function<std::uint32_t(std::uint32_t)>& map) {
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        para.styleId = map(para.styleId);
        for (model::Run& run : para.runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Text && item.charStyleId != model::kNoCharStyle) {
                    item.charStyleId = map(item.charStyleId);
                }
            }
        }
    });
}

// 스타일 표를 바꾼 뒤 문서 전체를 다시 놓는다.
EditResult relayoutAfterStyles(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    const PointerCursor cur = pointerCursor();
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    if (cur.active && cur.story > 0) {
        setPointerCursor(doc, pages, model::CursorPos{cur.story, cur.anchorParagraph, cur.anchorOffset},
                         model::CursorPos{cur.story, cur.focusParagraph, cur.focusOffset});
    }
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    return makeEditResult(doc, true, true, std::max(1, cur.story), std::max(0, cur.focusParagraph), &full);
}

std::string checkedName(const model::Document& doc, const std::string& name, int editing, std::string& why) {
    if (name.empty()) {
        why = "스타일 이름을 넣으십시오.";
        return {};
    }
    for (const model::Style& st : doc.header.styles) {
        if (st.name == name && static_cast<int>(st.id) != editing) {
            why = "같은 이름의 스타일이 있습니다.";
            return {};
        }
    }
    return name;
}

// 다른 문서의 글꼴·테두리·탭·번호·모양을 이 문서 번호로 옮긴다.
struct ShapeCopier {
    const model::Document& src;
    model::Document& dst;

    std::uint32_t font(std::size_t lang, std::uint32_t ref) {
        if (lang >= src.header.fonts.size() || ref >= src.header.fonts[lang].size()) return ref;
        const model::Font& want = src.header.fonts[lang][ref];
        auto& fonts = dst.header.fonts[lang];
        for (std::size_t i = 0; i < fonts.size(); ++i) {
            if (fonts[i].face == want.face) return static_cast<std::uint32_t>(i);
        }
        model::Font copy = want;
        copy.id = static_cast<std::uint32_t>(fonts.size());
        fonts.push_back(std::move(copy));
        return static_cast<std::uint32_t>(fonts.size() - 1);
    }

    std::uint32_t fill(std::uint32_t id) {
        if (id == 0) return 0;
        for (const model::BorderFill& f : src.header.borderFills) {
            if (f.id == id) return internBorderFillOf(dst, f);
        }
        return 0;
    }

    std::uint32_t tab(std::uint32_t id) {
        const model::TabDef* want = nullptr;
        for (const model::TabDef& t : src.header.tabDefs) {
            if (t.id == id) want = &t;
        }
        if (!want) return id;
        std::uint32_t maxId = 0;
        for (const model::TabDef& t : dst.header.tabDefs) {
            maxId = std::max(maxId, t.id);
            if (t.autoTabLeft != want->autoTabLeft || t.autoTabRight != want->autoTabRight ||
                t.stops.size() != want->stops.size()) {
                continue;
            }
            bool same = true;
            for (std::size_t i = 0; i < t.stops.size() && same; ++i) {
                same = t.stops[i].pos == want->stops[i].pos && t.stops[i].type == want->stops[i].type &&
                       t.stops[i].leader == want->stops[i].leader;
            }
            if (same) return t.id;
        }
        model::TabDef copy = *want;
        copy.id = dst.header.tabDefs.empty() ? 0 : maxId + 1;
        dst.header.tabDefs.push_back(copy);
        return copy.id;
    }

    std::uint32_t numbering(std::uint32_t id) {
        const model::Numbering* want = nullptr;
        for (const model::Numbering& n : src.header.numberings) {
            if (n.id == id) want = &n;
        }
        if (!want) return id;
        std::uint32_t maxId = 1;
        for (const model::Numbering& n : dst.header.numberings) {
            maxId = std::max(maxId, n.id);
            if (n.start != want->start || n.levels.size() != want->levels.size()) continue;
            bool same = true;
            for (std::size_t i = 0; i < n.levels.size() && same; ++i) {
                same = n.levels[i].format == want->levels[i].format && n.levels[i].numFormat == want->levels[i].numFormat &&
                       n.levels[i].start == want->levels[i].start;
            }
            if (same) return n.id;
        }
        model::Numbering copy = *want;
        copy.id = maxId + 1;
        dst.header.numberings.push_back(copy);
        return copy.id;
    }

    std::uint32_t bullet(std::uint32_t id) {
        const model::Bullet* want = nullptr;
        for (const model::Bullet& b : src.header.bullets) {
            if (b.id == id) want = &b;
        }
        if (!want) return id;
        std::uint32_t maxId = 0;
        for (const model::Bullet& b : dst.header.bullets) {
            maxId = std::max(maxId, b.id);
            if (b.text == want->text && b.checkedText == want->checkedText && b.imageId == want->imageId) return b.id;
        }
        model::Bullet copy = *want;
        copy.id = maxId + 1;
        dst.header.bullets.push_back(copy);
        return copy.id;
    }

    std::uint32_t charShape(std::uint32_t id) {
        const model::CharShape* want = Painter::findCharShape(src, id);
        if (!want) return 0;
        model::CharShape copy = *want;
        for (std::size_t lang = 0; lang < copy.fontRef.size(); ++lang) copy.fontRef[lang] = font(lang, copy.fontRef[lang]);
        copy.borderFillId = fill(copy.borderFillId);
        return internCharShape(dst, copy);
    }

    std::uint32_t paraShape(std::uint32_t id) {
        const model::ParaShape* want = Painter::findParaShape(src, id);
        if (!want) return 0;
        model::ParaShape copy = *want;
        copy.tabDefId = tab(copy.tabDefId);
        copy.borderFillId = fill(copy.borderFillId);
        if (copy.headingType == "NUMBER") copy.headingIdRef = numbering(copy.headingIdRef);
        else if (copy.headingType == "BULLET") copy.headingIdRef = bullet(copy.headingIdRef);
        return internParaShape(dst, copy);
    }
};

}  // namespace

EditResult paint_detail::importStylesNow(model::Document& doc, std::vector<Node>& pages, const model::Document& source,
                                         const std::vector<std::string>& names, bool useStoredLineHeight,
                                         std::string& why) {
    ShapeCopier copier{source, doc};
    std::vector<std::pair<std::uint32_t, std::string>> copied;  // 이 문서 번호, 원래 다음 스타일 이름
    bool changed = false;
    for (const model::Style& from : source.header.styles) {
        if (!names.empty() && std::find(names.begin(), names.end(), from.name) == names.end()) continue;
        model::Style now = from;
        now.paraShapeId = copier.paraShape(from.paraShapeId);
        now.charShapeId = copier.charShape(from.charShapeId);
        const model::Style* next = Painter::findStyle(source, from.nextStyleId);
        int index = -1;
        for (std::size_t i = 0; i < doc.header.styles.size(); ++i) {
            if (doc.header.styles[i].name == from.name) index = static_cast<int>(i);
        }
        if (index >= 0) {
            const model::Style was = doc.header.styles[static_cast<std::size_t>(index)];
            now.id = was.id;
            // 종류가 다른 같은 이름 스타일은 덮어쓰지 않는다(쓰던 곳의 종류가 어긋난다).
            if (isCharStyle(was) != isCharStyle(now)) continue;
            doc.header.styles[static_cast<std::size_t>(index)] = now;
            if (isCharStyle(was)) restyleCharTextOf(doc, was, now);
            else restyleParagraphsOf(doc, was, now);
        } else {
            now.id = static_cast<std::uint32_t>(doc.header.styles.size());
            doc.header.styles.push_back(now);
        }
        copied.emplace_back(now.id, next ? next->name : std::string());
        changed = true;
    }
    if (!changed) {
        why = "가져올 스타일이 없습니다.";
        return {};
    }
    // 다음 스타일은 이름으로 잇는다. 이 문서에 그 이름이 없으면 자기 자신이다.
    for (const auto& [id, nextName] : copied) {
        model::Style& st = doc.header.styles[static_cast<std::size_t>(styleIndex(doc, id))];
        st.nextStyleId = st.id;
        if (isCharStyle(st)) st.nextStyleId = 0;
        for (const model::Style& other : doc.header.styles) {
            if (!isCharStyle(st) && !nextName.empty() && other.name == nextName && !isCharStyle(other)) st.nextStyleId = other.id;
        }
    }
    return relayoutAfterStyles(doc, pages, useStoredLineHeight);
}

model::Document styleOnlyDocument(const model::Document& doc) {
    model::Document out = model::blankDocument();
    const std::vector<model::Section> sections = out.sections;
    out.header = doc.header;
    out.header.binItems.clear();
    out.header.sectionCount = 1;
    out.sections = sections;
    // 빈 문단은 바탕 스타일 모양이다.
    for (model::Section& section : out.sections) {
        for (model::Paragraph& para : section.paragraphs) {
            para.styleId = out.header.styles.empty() ? 0 : out.header.styles.front().id;
            para.paraShapeId = out.header.styles.empty() ? 0 : out.header.styles.front().paraShapeId;
            for (model::Run& run : para.runs) run.charShapeId = out.header.styles.empty() ? 0 : out.header.styles.front().charShapeId;
            para.lineSegs.clear();
        }
    }
    return out;
}

std::vector<StyleRow> styleRows(const model::Document& doc) {
    std::vector<StyleRow> rows;
    for (std::size_t i = 0; i < doc.header.styles.size(); ++i) {
        const model::Style& st = doc.header.styles[i];
        StyleRow row;
        row.id = st.id;
        row.name = st.name;
        row.engName = st.engName;
        row.type = st.type.empty() ? "PARA" : st.type;
        row.nextStyleId = st.nextStyleId;
        row.shortcut = i < 10 ? static_cast<int>(i) + 1 : 0;
        rows.push_back(std::move(row));
    }
    return rows;
}

FormatFields styleSummary(const model::Document& doc, std::uint32_t styleId) {
    FormatFields out;
    const model::Style* st = Painter::findStyle(doc, styleId);
    if (!st) return out;
    if (const model::ParaShape* ps = Painter::findParaShape(doc, st->paraShapeId)) {
        out.emplace_back("marginLeft", static_cast<double>(ps->marginLeft));
        out.emplace_back("indent", static_cast<double>(ps->indent));
        out.emplace_back("marginRight", static_cast<double>(ps->marginRight));
        out.emplace_back("align", ps->align);
        out.emplace_back("lineSpacingType", ps->lineSpacingType);
        out.emplace_back("lineSpacing", static_cast<double>(ps->lineSpacing));
        out.emplace_back("headingType", ps->headingType.empty() ? std::string("NONE") : ps->headingType);
        out.emplace_back("headingLevel", static_cast<double>(ps->headingLevel));
    }
    if (const model::Style* next = Painter::findStyle(doc, st->nextStyleId)) out.emplace_back("nextStyle", next->name);
    if (const model::CharShape* cs = Painter::findCharShape(doc, st->charShapeId)) {
        const auto& fonts = doc.header.fonts[0];
        const std::uint32_t ref = cs->fontRef[0];
        out.emplace_back("font", ref < fonts.size() ? fonts[ref].face : std::string());
        out.emplace_back("ratio", static_cast<double>(cs->ratio[0] == 0 ? 100 : cs->ratio[0]));
        out.emplace_back("height", static_cast<double>(cs->height));
        out.emplace_back("spacing", static_cast<double>(cs->spacing[0]));
    }
    out.emplace_back("type", st->type.empty() ? std::string("PARA") : st->type);
    return out;
}

StyleDraft beginStyleDraft(const model::Document& doc, const std::vector<Node>& pages, int editing) {
    StyleDraft draft;
    draft.active = true;
    // 머리말만 베낀다. 글자·문단 모양 대화 상자가 여기 모양을 더한다.
    draft.scratch.header = doc.header;
    const model::Style* st = editing >= 0 ? Painter::findStyle(doc, static_cast<std::uint32_t>(editing)) : nullptr;
    if (st) {
        draft.editing = static_cast<int>(st->id);
        draft.name = st->name;
        draft.engName = st->engName;
        draft.type = st->type.empty() ? "PARA" : st->type;
        draft.nextStyleId = st->nextStyleId;
        draft.place.styleId = st->id;
        draft.place.paraShapeId = st->paraShapeId;
        draft.place.charShapeId = st->charShapeId;
    } else {
        // 새 스타일은 캐럿 문단·글자 모양에서 시작하고, 다음 스타일은 새 스타일 자신이다.
        const CursorStyle caret = cursorStyle(doc, pages);
        const model::Style* paraStyle = Painter::findStyle(doc, caret.styleId);
        draft.editing = -1;
        draft.type = "PARA";
        draft.nextStyleId = static_cast<std::uint32_t>(doc.header.styles.size());
        draft.place.styleId = paraStyle && !isCharStyle(*paraStyle) ? paraStyle->id : 0;
        draft.place.paraShapeId = caret.paraShapeId;
        draft.place.charShapeId = caret.charShapeId;
    }
    draft.place.hit = true;
    return draft;
}

FormatFields styleDraftFields(const StyleDraft& draft) {
    return {{"name", draft.name},
            {"engName", draft.engName},
            {"type", draft.type},
            {"nextStyleId", static_cast<double>(draft.nextStyleId)},
            {"editing", static_cast<double>(draft.editing)}};
}

void setStyleDraftFields(StyleDraft& draft, const FormatFields& fields) {
    for (const auto& [key, value] : fields) {
        if (const std::string* t = std::get_if<std::string>(&value)) {
            if (key == "name") draft.name = *t;
            else if (key == "engName") draft.engName = *t;
            else if (key == "type" && (*t == "PARA" || *t == "CHAR")) draft.type = *t;
        } else if (const double* n = std::get_if<double>(&value)) {
            if (key == "nextStyleId" && *n >= 0) draft.nextStyleId = static_cast<std::uint32_t>(*n);
        }
    }
}

void applyStyleDraftFormat(StyleDraft& draft, bool para, const FormatFields& resolved) {
    FormatPatch patch;
    for (const auto& [key, value] : resolved) setFormatField(patch, key, value);
    if (para) {
        draft.place.paraShapeId = deriveParaShapeId(draft.scratch, draft.place.paraShapeId, patch);
    } else {
        draft.place.charShapeId = deriveCharShapeId(draft.scratch, draft.place.charShapeId, patch);
    }
}

EditResult paint_detail::commitStyleNow(model::Document& doc, std::vector<Node>& pages, const StyleDraft& draft,
                                        bool useStoredLineHeight, std::string& why) {
    if (!draft.active) return {};
    const std::string name = checkedName(doc, draft.name, draft.editing, why);
    if (name.empty()) return {};
    // 초안 머리말은 문서 머리말에 덧붙이기만 했다(글꼴·테두리·탭·번호·모양). 그대로 옮기면 번호가 맞는다.
    doc.header.fonts = draft.scratch.header.fonts;
    doc.header.borderFills = draft.scratch.header.borderFills;
    doc.header.borderFillCount = draft.scratch.header.borderFillCount;
    doc.header.charShapes = draft.scratch.header.charShapes;
    doc.header.paraShapes = draft.scratch.header.paraShapes;
    doc.header.tabDefs = draft.scratch.header.tabDefs;
    doc.header.numberings = draft.scratch.header.numberings;
    doc.header.bullets = draft.scratch.header.bullets;
    model::Style now;
    if (draft.editing >= 0) {
        const int index = styleIndex(doc, static_cast<std::uint32_t>(draft.editing));
        if (index < 0) {
            why = kNoStyleMessage;
            return {};
        }
        now = doc.header.styles[static_cast<std::size_t>(index)];
    } else {
        now.id = static_cast<std::uint32_t>(doc.header.styles.size());
        now.langId = doc.header.styles.empty() ? 1042 : doc.header.styles.front().langId;
    }
    now.name = name;
    now.engName = draft.engName;
    now.type = draft.type;
    now.paraShapeId = draft.place.paraShapeId;
    now.charShapeId = draft.place.charShapeId;
    now.nextStyleId = draft.nextStyleId < doc.header.styles.size() + (draft.editing < 0 ? 1 : 0) ? draft.nextStyleId
                                                                                                : now.id;
    if (isCharStyle(now)) now.nextStyleId = 0;
    if (draft.editing < 0) {
        doc.header.styles.push_back(now);
        return relayoutAfterStyles(doc, pages, useStoredLineHeight);
    }
    const int index = styleIndex(doc, now.id);
    const model::Style was = doc.header.styles[static_cast<std::size_t>(index)];
    doc.header.styles[static_cast<std::size_t>(index)] = now;
    if (isCharStyle(was)) {
        restyleCharTextOf(doc, was, now);
    } else if (!isCharStyle(now)) {
        restyleParagraphsOf(doc, was, now);
    }
    return relayoutAfterStyles(doc, pages, useStoredLineHeight);
}

EditResult paint_detail::deleteStyleNow(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId,
                                        std::uint32_t replacement, bool useStoredLineHeight, std::string& why) {
    const int index = styleIndex(doc, styleId);
    if (index < 0) {
        why = kNoStyleMessage;
        return {};
    }
    if (index == 0) {
        why = "바탕 스타일은 지울 수 없습니다.";
        return {};
    }
    int with = styleIndex(doc, replacement);
    if (with < 0 || with == index) with = 0;
    const model::Style gone = doc.header.styles[static_cast<std::size_t>(index)];
    model::Style instead = doc.header.styles[static_cast<std::size_t>(with)];
    // 문단 스타일은 문단 스타일로만 바꾼다. 글자 스타일은 대신이 글자 스타일이 아니면 푼다.
    if (!isCharStyle(gone) && isCharStyle(instead)) instead = doc.header.styles.front();
    if (isCharStyle(gone)) {
        restyleCharTextOf(doc, gone, instead);
    } else {
        restyleParagraphsOf(doc, gone, instead);
    }
    // 지운 스타일을 다음 스타일로 쓰던 스타일은 자기 자신을 다음 스타일로 쓴다.
    for (model::Style& st : doc.header.styles) {
        if (st.nextStyleId == gone.id) st.nextStyleId = st.id;
    }
    doc.header.styles.erase(doc.header.styles.begin() + index);
    const auto shift = [&](std::uint32_t id) { return id > gone.id ? id - 1 : id; };
    for (model::Style& st : doc.header.styles) {
        st.id = shift(st.id);
        st.nextStyleId = shift(st.nextStyleId);
    }
    remapStyleIds(doc, shift);
    return relayoutAfterStyles(doc, pages, useStoredLineHeight);
}

EditResult paint_detail::moveStyleNow(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId, int delta,
                                      bool useStoredLineHeight, std::string& why) {
    const int index = styleIndex(doc, styleId);
    const int target = index + (delta < 0 ? -1 : 1);
    if (index < 0 || target < 0 || target >= static_cast<int>(doc.header.styles.size())) {
        why = index < 0 ? kNoStyleMessage : "더 옮길 수 없습니다.";
        return {};
    }
    auto& styles = doc.header.styles;
    const std::uint32_t a = styles[static_cast<std::size_t>(index)].id;
    const std::uint32_t b = styles[static_cast<std::size_t>(target)].id;
    std::swap(styles[static_cast<std::size_t>(index)], styles[static_cast<std::size_t>(target)]);
    const auto swapId = [&](std::uint32_t id) { return id == a ? b : id == b ? a : id; };
    for (model::Style& st : styles) {
        st.id = swapId(st.id);
        st.nextStyleId = swapId(st.nextStyleId);
    }
    remapStyleIds(doc, swapId);
    return relayoutAfterStyles(doc, pages, useStoredLineHeight);
}

EditResult paint_detail::styleFromCaretNow(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId,
                                           bool useStoredLineHeight, std::string& why) {
    const int index = styleIndex(doc, styleId);
    if (index < 0) {
        why = kNoStyleMessage;
        return {};
    }
    const CursorStyle caret = cursorStyle(doc, pages);
    const model::Style was = doc.header.styles[static_cast<std::size_t>(index)];
    model::Style now = was;
    now.charShapeId = caret.charShapeId;
    if (!isCharStyle(was)) now.paraShapeId = caret.paraShapeId;
    if (now.charShapeId == was.charShapeId && now.paraShapeId == was.paraShapeId) return {};
    doc.header.styles[static_cast<std::size_t>(index)] = now;
    if (isCharStyle(was)) {
        restyleCharTextOf(doc, was, now);
    } else {
        restyleParagraphsOf(doc, was, now);
    }
    return relayoutAfterStyles(doc, pages, useStoredLineHeight);
}

EditResult paint_detail::clearCharStyleNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                                           std::string& why) {
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || paras->empty()) return {};
    const int last = static_cast<int>(paras->size()) - 1;
    int firstPara = cur.paragraph;
    int lastPara = cur.paragraph;
    int from = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    int to = from;
    if (cur.hasSelection) {
        firstPara = std::clamp(cur.startParagraph, 0, last);
        lastPara = std::clamp(cur.endParagraph, firstPara, last);
        from = offsetToCharIndex(doc, pages, cur.story, firstPara, cur.startOffset);
        to = offsetToCharIndex(doc, pages, cur.story, lastPara, cur.endOffset);
    } else {
        // 블록이 없으면 캐럿에 걸어 둔 글자 스타일만 푼다.
        if (hold.hasPendingCharShape && hold.pendingCharStyleId != model::kNoCharStyle) {
            hold.pendingCharStyleId = model::kNoCharStyle;
            const model::Paragraph& para = (*paras)[static_cast<std::size_t>(cur.paragraph)];
            if (const model::Style* paraStyle = Painter::findStyle(doc, para.styleId)) {
                hold.pendingCharShapeId = paraStyle->charShapeId;
            }
            return makeEditResult(doc, true, false, cur.story, cur.paragraph);
        }
        why = "글자 스타일을 풀 글을 블록으로 고르십시오.";
        return {};
    }
    bool changed = false;
    for (int p = firstPara; p <= lastPara; ++p) {
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
        const int len = paragraphCharCount(para);
        const int a = p == firstPara ? std::clamp(from, 0, len) : 0;
        const int b = p == lastPara ? std::clamp(to, 0, len) : len;
        if (b <= a) continue;
        const model::Style* paraStyle = Painter::findStyle(doc, para.styleId);
        if (!paraStyle) continue;
        const model::Style target = *paraStyle;
        // 구간 경계에서 글 조각을 나눈 뒤 구간 안 조각만 푼다.
        splitRunsAt(para, a);
        splitRunsAt(para, b);
        int pos = 0;
        for (model::Run& run : para.runs) {
            int runStart = pos;
            for (const model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Text) pos += utf8CodePointCount(item.text);
                else if (isCaretCharControl(item.control)) ++pos;
            }
            if (runStart < a || pos > b) continue;
            std::uint32_t styleOfRun = model::kNoCharStyle;
            for (model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Text || item.charStyleId == model::kNoCharStyle) continue;
                styleOfRun = item.charStyleId;
                item.charStyleId = model::kNoCharStyle;
            }
            if (styleOfRun == model::kNoCharStyle) continue;
            if (const model::Style* charStyle = Painter::findStyle(doc, styleOfRun)) {
                const model::Style was = *charStyle;
                run.charShapeId = restyleCharShapeId(doc, run.charShapeId, &was, target);
            }
            changed = true;
        }
        mergeRuns(para);
        para.lineSegs.clear();
    }
    if (!changed) {
        why = "블록 안에 글자 스타일이 없습니다.";
        return {};
    }
    return relayoutAfterStyles(doc, pages, useStoredLineHeight);
}

}  // namespace chwp::render
