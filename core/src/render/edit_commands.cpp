#include "noting/render/paint.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "noting/common/unicode.hpp"
#include "noting/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"

// 편집 명령. 글자 넣기·지우기, 키 입력, 조합, 글자·문단 모양 적용, 문단 나누기·합치기.
// 모델을 고친 뒤 relayoutAfterEdit 로 고친 자리만 다시 놓고, 캐럿과 선택을 옮긴다.

namespace noting::render {

using namespace paint_detail;
namespace paint_detail {

int utf8CodePointCount(const std::string& text) {
    std::size_t idx = 0;
    char32_t cp = 0;
    int count = 0;
    while (nextCp(text, idx, cp)) ++count;
    return count;
}

std::string utf8SliceCodePoints(const std::string& text, int startCp, int countCp) {
    if (countCp <= 0) return {};
    std::size_t idx = 0;
    char32_t cp = 0;
    int cur = 0;
    std::size_t byteStart = text.size();
    std::size_t byteEnd = text.size();
    while (idx < text.size()) {
        const std::size_t before = idx;
        if (!nextCp(text, idx, cp)) break;
        if (cur == startCp) byteStart = before;
        ++cur;
        if (cur == startCp + countCp) {
            byteEnd = idx;
            break;
        }
    }
    if (startCp <= 0) byteStart = 0;
    if (byteStart >= text.size() || byteEnd <= byteStart) return {};
    return text.substr(byteStart, byteEnd - byteStart);
}

bool isInlineObjectControl(const model::Control& ctrl) {
    using model::ControlKind;
    return ctrl.placement && ctrl.placement->treatAsChar &&
           (ctrl.kind == ControlKind::Shape || ctrl.kind == ControlKind::Picture ||
            ctrl.kind == ControlKind::Table || ctrl.kind == ControlKind::Container ||
            ctrl.kind == ControlKind::Equation);
}

// 각주·미주 번호는 글자 한 칸이다. 앞뒤에 커서가 서고, 지우면 주석이 빠진다.
bool isCaretCharControl(const model::Control& ctrl) {
    return isInlineObjectControl(ctrl) || ctrl.kind == model::ControlKind::Footnote ||
           ctrl.kind == model::ControlKind::Endnote;
}

int paragraphCharCount(const model::Paragraph& para) {
    int total = 0;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                total += utf8CodePointCount(item.text);
            } else if (isCaretCharControl(item.control)) {
                ++total;
            }
        }
    }
    return total;
}

bool isSectionLeadControl(const model::Control& ctrl) {
    using model::ControlKind;
    return ctrl.kind == ControlKind::SectionDef || ctrl.kind == ControlKind::ColumnDef ||
           ctrl.kind == ControlKind::Header || ctrl.kind == ControlKind::Footer ||
           ctrl.kind == ControlKind::PageNumPos;
}

void ensureParagraphHasTextRun(model::Paragraph& para, std::uint32_t fallbackCharShapeId) {
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) return;
        }
    }
    if (para.runs.empty()) {
        model::Run run;
        run.charShapeId = fallbackCharShapeId;
        run.items.push_back(model::InlineItem::makeText(""));
        para.runs.push_back(std::move(run));
    } else {
        para.runs.back().items.push_back(model::InlineItem::makeText(""));
    }
}

struct CharSegEntry {
    int segPos = 0;
};

std::vector<CharSegEntry> buildCharSegTable(const model::Paragraph& para) {
    std::vector<CharSegEntry> entries;
    int segPos = 0;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                std::size_t idx = 0;
                char32_t cp = 0;
                while (nextCp(item.text, idx, cp)) {
                    entries.push_back(CharSegEntry{segPos});
                    if (cp == '\t') segPos += 8;
                    else if (cp > 0xFFFF) segPos += 2;
                    else segPos += 1;
                }
            } else {
                if (isCaretCharControl(item.control)) {
                    entries.push_back(CharSegEntry{segPos});
                }
                segPos += 8;
            }
        }
    }
    return entries;
}

int offsetToCharIndex(const model::Document& doc, const std::vector<Node>& pages, int story, int paraIdx, int offset) {
    const model::Paragraph* para = storyParagraph(doc, story, paraIdx);
    if (!para) return std::max(0, offset);
    const int totalChars = paragraphCharCount(*para);
    if (para->lineSegs.empty()) return std::clamp(offset, 0, totalChars);

    const std::vector<CharSegEntry> table = buildCharSegTable(*para);
    Painter painter;
    painter.doc = &doc;
    int bestLineStart = -1;
    bool prevLineBreak = false;
    int prevMaxOff = -1;
    forEachStoryParagraphPage(
        pages, story, paraIdx, paraIdx, currentCaretPageHint(), [&](int, const Node& page) {
            if (page.spansLater) ensurePageSpans(doc, const_cast<Node&>(page));
            std::vector<Painter::StoryLine> lines;
            painter.gatherStoryParagraph(page, 0, 0, story, paraIdx, lines);
            for (const Painter::StoryLine& line : lines) {
                if (!line.node || line.node->paragraph != paraIdx) continue;
                const std::vector<std::int32_t> edges = painter.glyphEdges(*line.node, line.x, line.y, nullptr);
                if (edges.empty()) continue;
                const int maxOff = line.node->start + static_cast<int>(edges.size()) - 1;
                if (offset >= line.node->start && offset <= maxOff) {
                    const bool startsWithObject =
                        !line.node->children.empty() && line.node->children.front().kind == NodeKind::Object;
                    const PointerHold& hold = pointerHold();
                    const bool caretWasOnThisLine =
                        hold.caret.hit &&
                        std::abs(hold.caret.y - (line.y + line.node->marginTop)) <= 4;
                    if (bestLineStart >= 0 && !prevLineBreak && offset == line.node->start && offset == prevMaxOff &&
                        startsWithObject && !caretWasOnThisLine) {
                        prevLineBreak = line.node->lineBreak;
                        prevMaxOff = maxOff;
                        continue;
                    }
                    bestLineStart = line.node->start;
                }
                prevLineBreak = line.node->lineBreak;
                prevMaxOff = maxOff;
            }
        });
    if (bestLineStart < 0) {
        for (int i = 0; i < static_cast<int>(table.size()); ++i) {
            if (table[static_cast<std::size_t>(i)].segPos >= offset) {
                return std::clamp(i, 0, totalChars);
            }
        }
        return totalChars;
    }
    int firstCharIdx = totalChars;
    for (int i = 0; i < static_cast<int>(table.size()); ++i) {
        if (table[static_cast<std::size_t>(i)].segPos >= bestLineStart) {
            firstCharIdx = i;
            break;
        }
    }
    const int delta = std::max(0, offset - bestLineStart);
    return std::clamp(firstCharIdx + delta, 0, totalChars);
}

int charIndexToOffset(const model::Document& doc, const std::vector<Node>& pages, int story, int paraIdx, int charIndex) {
    const model::Paragraph* para = storyParagraph(doc, story, paraIdx);
    if (!para) return std::max(0, charIndex);
    const int totalChars = paragraphCharCount(*para);
    const int clamped = std::clamp(charIndex, 0, totalChars);
    if (para->lineSegs.empty()) return clamped;

    const std::vector<CharSegEntry> table = buildCharSegTable(*para);
    Painter painter;
    painter.doc = &doc;
    int resolvedOffset = -1;
    forEachStoryParagraphPage(
        pages, story, paraIdx, paraIdx, currentCaretPageHint(), [&](int, const Node& page) {
            if (resolvedOffset >= 0) return;
            if (page.spansLater) ensurePageSpans(doc, const_cast<Node&>(page));
            std::vector<Painter::StoryLine> lines;
            painter.gatherStoryParagraph(page, 0, 0, story, paraIdx, lines);
            for (const Painter::StoryLine& line : lines) {
                if (!line.node || line.node->paragraph != paraIdx) continue;
                const std::vector<std::int32_t> edges = painter.glyphEdges(*line.node, line.x, line.y, nullptr);
                if (edges.empty()) continue;
                const int markCount = static_cast<int>(edges.size()) - 1;
                int firstCharIdx = totalChars;
                for (int i = 0; i < static_cast<int>(table.size()); ++i) {
                    if (table[static_cast<std::size_t>(i)].segPos >= line.node->start) {
                        firstCharIdx = i;
                        break;
                    }
                }
                if (clamped >= firstCharIdx && clamped <= firstCharIdx + markCount) {
                    resolvedOffset = line.node->start + (clamped - firstCharIdx);
                    return;
                }
            }
        });
    return resolvedOffset >= 0 ? resolvedOffset : clamped;
}

int insertTextInParagraph(model::Paragraph& para, int charIndex, const std::string& utf8Text) {
    const int insertedCount = utf8CodePointCount(utf8Text);
    const std::uint32_t fallbackShape = para.runs.empty() ? 0 : para.runs.back().charShapeId;
    if (utf8Text.empty()) {
        ensureParagraphHasTextRun(para, fallbackShape);
        para.lineSegs.clear();
        return 0;
    }

    int pos = 0;
    for (model::Run& run : para.runs) {
        for (std::size_t i = 0; i < run.items.size(); ++i) {
            model::InlineItem& item = run.items[i];
            if (item.type == model::InlineItem::Type::Text) {
                const int len = utf8CodePointCount(item.text);
                const int itemStart = pos;
                const int itemEnd = pos + len;
                pos = itemEnd;
                if (charIndex >= itemStart && charIndex <= itemEnd) {
                    const int local = std::clamp(charIndex - itemStart, 0, len);
                    const std::string prefix = utf8SliceCodePoints(item.text, 0, local);
                    const std::string suffix = utf8SliceCodePoints(item.text, local, len - local);
                    item.text = prefix + utf8Text + suffix;
                    para.lineSegs.clear();
                    return insertedCount;
                }
            } else if (isCaretCharControl(item.control)) {
                const int ctrlStart = pos;
                pos += 1;
                if (charIndex == ctrlStart) {
                    run.items.insert(run.items.begin() + static_cast<std::ptrdiff_t>(i),
                                     model::InlineItem::makeText(utf8Text));
                    para.lineSegs.clear();
                    return insertedCount;
                }
                if (charIndex == pos) {
                    if (i + 1 < run.items.size() && run.items[i + 1].type == model::InlineItem::Type::Text) {
                        run.items[i + 1].text = utf8Text + run.items[i + 1].text;
                    } else {
                        run.items.insert(run.items.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                         model::InlineItem::makeText(utf8Text));
                    }
                    para.lineSegs.clear();
                    return insertedCount;
                }
            }
        }
    }
    if (para.runs.empty()) {
        model::Run run;
        run.charShapeId = fallbackShape;
        run.items.push_back(model::InlineItem::makeText(utf8Text));
        para.runs.push_back(std::move(run));
    } else {
        model::Run& backRun = para.runs.back();
        if (!backRun.items.empty() && backRun.items.back().type == model::InlineItem::Type::Text) {
            backRun.items.back().text += utf8Text;
        } else {
            backRun.items.push_back(model::InlineItem::makeText(utf8Text));
        }
    }
    para.lineSegs.clear();
    return insertedCount;
}

void eraseCharsInParagraph(model::Paragraph& para, int startChar, int endChar) {
    if (endChar <= startChar) {
        para.lineSegs.clear();
        return;
    }
    const std::uint32_t fallbackShape = para.runs.empty() ? 0 : para.runs.front().charShapeId;
    int pos = 0;
    for (model::Run& run : para.runs) {
        for (std::size_t i = 0; i < run.items.size();) {
            model::InlineItem& item = run.items[i];
            if (item.type == model::InlineItem::Type::Text) {
                const int len = utf8CodePointCount(item.text);
                const int itemStart = pos;
                const int itemEnd = pos + len;
                pos = itemEnd;
                if (itemEnd > startChar && itemStart < endChar) {
                    const int keepLeft = std::max(0, startChar - itemStart);
                    const int cutEnd = std::min(len, endChar - itemStart);
                    const std::string left = utf8SliceCodePoints(item.text, 0, keepLeft);
                    const std::string right = utf8SliceCodePoints(item.text, cutEnd, len - cutEnd);
                    item.text = left + right;
                }
                ++i;
            } else if (isCaretCharControl(item.control)) {
                const int ctrlStart = pos;
                const int ctrlEnd = pos + 1;
                pos = ctrlEnd;
                if (ctrlStart >= startChar && ctrlEnd <= endChar) {
                    run.items.erase(run.items.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        }
    }
    ensureParagraphHasTextRun(para, fallbackShape);
    para.lineSegs.clear();
}

std::uint32_t charShapeAtParagraphChar(const model::Paragraph& para, int charIndex, bool preferRight) {
    int pos = 0;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            int len = 0;
            if (item.type == model::InlineItem::Type::Text) {
                len = utf8CodePointCount(item.text);
            } else if (isCaretCharControl(item.control)) {
                len = 1;
            }
            if (len <= 0) continue;
            const int itemStart = pos;
            const int itemEnd = pos + len;
            pos = itemEnd;
            if (preferRight) {
                if (charIndex >= itemStart && charIndex < itemEnd) return run.charShapeId;
            } else {
                if (charIndex > itemStart && charIndex <= itemEnd) return run.charShapeId;
            }
        }
    }
    if (!para.runs.empty()) {
        return preferRight ? para.runs.front().charShapeId : para.runs.back().charShapeId;
    }
    return 0;
}

template <typename Fn>
void transformParagraphRangeCharShape(model::Paragraph& para, int startChar, int endChar, Fn&& mapShapeId) {
    const int totalChars = paragraphCharCount(para);
    if (totalChars <= 0) {
        ensureParagraphHasTextRun(para, 0);
        for (model::Run& run : para.runs) {
            run.charShapeId = mapShapeId(run.charShapeId);
        }
        para.lineSegs.clear();
        return;
    }
    if (endChar <= startChar) return;

    auto appendItemToRuns = [](std::vector<model::Run>& out, std::uint32_t shapeId, model::InlineItem item) {
        if (item.type == model::InlineItem::Type::Text && item.text.empty()) return;
        if (!out.empty() && out.back().charShapeId == shapeId) {
            if (item.type == model::InlineItem::Type::Text && !out.back().items.empty() &&
                out.back().items.back().type == model::InlineItem::Type::Text) {
                out.back().items.back().text += item.text;
            } else {
                out.back().items.push_back(std::move(item));
            }
        } else {
            model::Run run;
            run.charShapeId = shapeId;
            run.items.push_back(std::move(item));
            out.push_back(std::move(run));
        }
    };

    std::vector<model::Run> newRuns;
    int pos = 0;
    for (model::Run& run : para.runs) {
        const std::uint32_t origShapeId = run.charShapeId;
        for (model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                const int len = utf8CodePointCount(item.text);
                const int itemStart = pos;
                const int itemEnd = pos + len;
                pos = itemEnd;
                if (len <= 0) continue;
                if (itemEnd <= startChar || itemStart >= endChar) {
                    appendItemToRuns(newRuns, origShapeId, std::move(item));
                } else if (itemStart >= startChar && itemEnd <= endChar) {
                    appendItemToRuns(newRuns, mapShapeId(origShapeId), std::move(item));
                } else {
                    const int leftLen = std::max(0, startChar - itemStart);
                    const int midStart = std::max(0, startChar - itemStart);
                    const int midEnd = std::min(len, endChar - itemStart);
                    const int midLen = std::max(0, midEnd - midStart);
                    const int rightStart = midEnd;
                    const int rightLen = std::max(0, len - rightStart);
                    if (leftLen > 0) {
                        appendItemToRuns(newRuns, origShapeId,
                                         model::InlineItem::makeText(utf8SliceCodePoints(item.text, 0, leftLen)));
                    }
                    if (midLen > 0) {
                        appendItemToRuns(newRuns, mapShapeId(origShapeId),
                                         model::InlineItem::makeText(utf8SliceCodePoints(item.text, midStart, midLen)));
                    }
                    if (rightLen > 0) {
                        appendItemToRuns(newRuns, origShapeId,
                                         model::InlineItem::makeText(utf8SliceCodePoints(item.text, rightStart, rightLen)));
                    }
                }
            } else if (isCaretCharControl(item.control)) {
                const int ctrlStart = pos;
                const int ctrlEnd = pos + 1;
                pos = ctrlEnd;
                const bool inside = (ctrlStart >= startChar && ctrlEnd <= endChar);
                appendItemToRuns(newRuns, inside ? mapShapeId(origShapeId) : origShapeId, std::move(item));
            } else {
                appendItemToRuns(newRuns, origShapeId, std::move(item));
            }
        }
    }
    const std::uint32_t fallbackShape = para.runs.empty() ? 0 : para.runs.front().charShapeId;
    para.runs = std::move(newRuns);
    ensureParagraphHasTextRun(para, fallbackShape);
    para.lineSegs.clear();
}

bool parseHexColorRgb(const std::string& text, std::uint32_t& outRgb) {
    std::size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i < text.size() && text[i] == '#') ++i;
    std::string hex;
    while (i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i]))) {
        hex.push_back(text[i]);
        ++i;
    }
    auto hexDigit = [](char c) -> std::uint32_t {
        if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0');
        if (c >= 'a' && c <= 'f') return static_cast<std::uint32_t>(10 + (c - 'a'));
        if (c >= 'A' && c <= 'F') return static_cast<std::uint32_t>(10 + (c - 'A'));
        return 0;
    };
    if (hex.size() == 6) {
        std::uint32_t val = 0;
        for (char c : hex) val = (val << 4) | hexDigit(c);
        outRgb = val & 0xFFFFFFu;
        return true;
    }
    if (hex.size() == 3) {
        const std::uint32_t r = hexDigit(hex[0]) * 17u;
        const std::uint32_t g = hexDigit(hex[1]) * 17u;
        const std::uint32_t b = hexDigit(hex[2]) * 17u;
        outRgb = (r << 16) | (g << 8) | b;
        return true;
    }
    return false;
}

bool sameParaShapeAttrs(const model::ParaShape& a, const model::ParaShape& b) {
    return a.align == b.align && a.vertAlign == b.vertAlign && a.indent == b.indent &&
           a.marginLeft == b.marginLeft && a.marginRight == b.marginRight &&
           a.spacingPrev == b.spacingPrev && a.spacingNext == b.spacingNext &&
           a.lineSpacingType == b.lineSpacingType && a.lineSpacing == b.lineSpacing &&
           a.breakLatinWord == b.breakLatinWord && a.breakNonLatinWord == b.breakNonLatinWord &&
           a.condense == b.condense && a.widowOrphan == b.widowOrphan && a.keepWithNext == b.keepWithNext &&
           a.keepLines == b.keepLines && a.pageBreakBefore == b.pageBreakBefore &&
           a.headingType == b.headingType && a.headingIdRef == b.headingIdRef &&
           a.headingLevel == b.headingLevel && a.checked == b.checked && a.tabDefId == b.tabDefId &&
           a.borderFillId == b.borderFillId && a.snapToGrid == b.snapToGrid &&
           a.fontLineHeight == b.fontLineHeight && a.autoSpacingEng == b.autoSpacingEng &&
           a.autoSpacingNum == b.autoSpacingNum && a.lineWrap == b.lineWrap &&
           a.borderOffsetLeft == b.borderOffsetLeft && a.borderOffsetRight == b.borderOffsetRight &&
           a.borderOffsetTop == b.borderOffsetTop && a.borderOffsetBottom == b.borderOffsetBottom &&
           a.borderConnect == b.borderConnect && a.borderIgnoreMargin == b.borderIgnoreMargin;
}

std::uint32_t deriveParaShapeId(model::Document& doc, std::uint32_t baseId, const FormatPatch& patch) {
    model::ParaShape next;
    if (const model::ParaShape* base = Painter::findParaShape(doc, baseId)) {
        next = *base;
    } else if (!doc.header.paraShapes.empty()) {
        next = doc.header.paraShapes.front();
    }
    if (patch.hasAlign && !patch.align.empty()) {
        next.align = patch.align;
    }
    if (patch.hasLineSpacingType && !patch.lineSpacingType.empty()) {
        next.lineSpacingType = patch.lineSpacingType;
    }
    if (patch.hasLineSpacing && patch.lineSpacing > 0) {
        next.lineSpacing = patch.lineSpacing;
    }
    applyParaFields(next, patch.fields);
    auto& shapes = doc.header.paraShapes;
    for (std::size_t idx = 0; idx < shapes.size(); ++idx) {
        if (shapes[idx].id == idx && sameParaShapeAttrs(shapes[idx], next)) {
            return static_cast<std::uint32_t>(idx);
        }
    }
    std::uint32_t newId = static_cast<std::uint32_t>(shapes.size());
    for (const auto& ps : shapes) {
        if (ps.id >= newId) newId = ps.id + 1;
    }
    next.id = newId;
    shapes.push_back(std::move(next));
    return newId;
}

bool sameCharShapeAttrs(const model::CharShape& a, const model::CharShape& b) {
    return a.height == b.height && a.fontRef == b.fontRef && a.ratio == b.ratio && a.spacing == b.spacing &&
           a.relSize == b.relSize && a.offset == b.offset && a.textColor == b.textColor &&
           a.shadeColor == b.shadeColor && a.bold == b.bold && a.italic == b.italic &&
           a.underline == b.underline && a.underlineShape == b.underlineShape &&
           a.underlineShapeId == b.underlineShapeId && a.underlineColor == b.underlineColor &&
           a.strikeout == b.strikeout && a.strikeShape == b.strikeShape && a.strikeShapeId == b.strikeShapeId &&
           a.strikeColor == b.strikeColor && a.outline == b.outline && a.shadow == b.shadow &&
           a.shadowOffsetX == b.shadowOffsetX && a.shadowOffsetY == b.shadowOffsetY &&
           a.shadowColor == b.shadowColor && a.emboss == b.emboss && a.engrave == b.engrave &&
           a.emphasis == b.emphasis && a.superscript == b.superscript && a.subscript == b.subscript &&
           a.useFontSpace == b.useFontSpace && a.useKerning == b.useKerning && a.borderFillId == b.borderFillId;
}

void ensureFontInAllSlots(model::Document& doc, const std::string& faceName, model::PerLang<std::uint32_t>& outRefs) {
    for (std::size_t slot = 0; slot < doc.header.fonts.size() && slot < outRefs.size(); ++slot) {
        auto& fonts = doc.header.fonts[slot];
        bool found = false;
        for (std::size_t idx = 0; idx < fonts.size(); ++idx) {
            if (fonts[idx].face == faceName) {
                outRefs[slot] = static_cast<std::uint32_t>(idx);
                found = true;
                break;
            }
        }
        if (!found) {
            model::Font nf;
            nf.id = static_cast<std::uint32_t>(fonts.size());
            nf.face = faceName;
            nf.type = "TTF";
            fonts.push_back(std::move(nf));
            outRefs[slot] = static_cast<std::uint32_t>(fonts.size() - 1);
        }
    }
}

std::uint32_t deriveCharShapeId(model::Document& doc, std::uint32_t baseId, const FormatPatch& patch) {
    model::CharShape next;
    next.ratio.fill(100);
    next.relSize.fill(100);
    if (const model::CharShape* base = Painter::findCharShape(doc, baseId)) {
        next = *base;
    } else if (!doc.header.charShapes.empty()) {
        next = doc.header.charShapes.front();
    }
    for (auto& r : next.ratio) {
        if (r == 0) r = 100;
    }
    for (auto& rel : next.relSize) {
        if (rel == 0) rel = 100;
    }
    if (patch.hasFont && !patch.font.empty()) {
        ensureFontInAllSlots(doc, patch.font, next.fontRef);
    }
    if (patch.hasFontSize && patch.fontSize > 0) {
        next.height = std::clamp<std::int32_t>(patch.fontSize, 100, 50000);
    }
    if (patch.hasBold) {
        next.bold = patch.bold;
    }
    if (patch.hasItalic) {
        next.italic = patch.italic;
    }
    if (patch.hasUnderlineType) {
        next.underline = patch.underlineType.empty() ? "NONE" : patch.underlineType;
    } else if (patch.hasUnderline) {
        if (patch.underline) {
            if (next.underline.empty() || next.underline == "NONE") next.underline = "BOTTOM";
        } else {
            next.underline = "NONE";
        }
    }
    if (patch.hasStrikeout) {
        next.strikeout = patch.strikeout;
    }
    if (patch.hasColor && !patch.color.empty()) {
        std::uint32_t rgb = 0;
        if (parseHexColorRgb(patch.color, rgb)) {
            next.textColor = rgb;
        }
    }
    applyCharFields(next, patch.fields);
    auto& shapes = doc.header.charShapes;
    for (std::size_t idx = 0; idx < shapes.size(); ++idx) {
        if (shapes[idx].id == idx && sameCharShapeAttrs(shapes[idx], next)) {
            return static_cast<std::uint32_t>(idx);
        }
    }
    std::uint32_t newId = static_cast<std::uint32_t>(shapes.size());
    for (const auto& cs : shapes) {
        if (cs.id >= newId) newId = cs.id + 1;
    }
    next.id = newId;
    shapes.push_back(std::move(next));
    return newId;
}

model::Paragraph splitParagraphAt(model::Paragraph& cur, int splitCharIndex) {
    model::Paragraph leftPara;
    leftPara.paraShapeId = cur.paraShapeId;
    leftPara.styleId = cur.styleId;
    leftPara.pageBreak = cur.pageBreak;
    leftPara.columnBreak = cur.columnBreak;
    leftPara.layoutBreak = cur.layoutBreak;

    model::Paragraph rightPara;
    rightPara.paraShapeId = cur.paraShapeId;
    rightPara.styleId = cur.styleId;

    const std::uint32_t firstShape = cur.runs.empty() ? 0 : cur.runs.front().charShapeId;
    std::uint32_t splitShape = firstShape;
    int pos = 0;
    for (const model::Run& run : cur.runs) {
        model::Run leftRun;
        leftRun.charShapeId = run.charShapeId;
        model::Run rightRun;
        rightRun.charShapeId = run.charShapeId;
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control) {
                const int ctrlLen = isCaretCharControl(item.control) ? 1 : 0;
                const int ctrlEnd = pos + ctrlLen;
                pos = ctrlEnd;
                if (isSectionLeadControl(item.control) || ctrlEnd <= splitCharIndex) {
                    leftRun.items.push_back(item);
                    if (ctrlLen > 0) splitShape = run.charShapeId;
                } else {
                    rightRun.items.push_back(item);
                }
                continue;
            }
            const int len = utf8CodePointCount(item.text);
            const int itemStart = pos;
            const int itemEnd = pos + len;
            pos = itemEnd;
            if (itemEnd <= splitCharIndex) {
                leftRun.items.push_back(item);
                splitShape = run.charShapeId;
            } else if (itemStart >= splitCharIndex) {
                rightRun.items.push_back(item);
            } else {
                const int local = splitCharIndex - itemStart;
                model::InlineItem leftItem = item;
                leftItem.text = utf8SliceCodePoints(item.text, 0, local);
                model::InlineItem rightItem = item;
                rightItem.text = utf8SliceCodePoints(item.text, local, len - local);
                leftRun.items.push_back(std::move(leftItem));
                rightRun.items.push_back(std::move(rightItem));
                splitShape = run.charShapeId;
            }
        }
        if (!leftRun.items.empty()) leftPara.runs.push_back(std::move(leftRun));
        if (!rightRun.items.empty()) rightPara.runs.push_back(std::move(rightRun));
    }
    ensureParagraphHasTextRun(leftPara, firstShape);
    ensureParagraphHasTextRun(rightPara, splitShape);
    cur = std::move(leftPara);
    return rightPara;
}

bool splitsBeforeWrappedInlineObject(const model::Document& doc, const std::vector<Node>& pages, int story, int paraIdx,
                                     int splitCharIndex, const model::Paragraph& para) {
    bool nextIsInlineObject = false;
    int pos = 0;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control) {
                const int ctrlLen = isInlineObjectControl(item.control) ? 1 : 0;
                if (ctrlLen == 0) continue;
                if (pos == splitCharIndex) {
                    nextIsInlineObject = true;
                    break;
                }
                if (pos > splitCharIndex) break;
                pos += ctrlLen;
            } else {
                const int len = utf8CodePointCount(item.text);
                if (len == 0) continue;
                if (pos + len > splitCharIndex) break;
                pos += len;
            }
        }
        if (nextIsInlineObject || pos > splitCharIndex) break;
    }
    if (!nextIsInlineObject) return false;

    std::vector<const Node*> paraLines;
    Painter painter;
    painter.doc = &doc;
    forEachStoryParagraphPage(
        pages, story, paraIdx, paraIdx, currentCaretPageHint(), [&](int, const Node& page) {
            if (page.spansLater) ensurePageSpans(doc, const_cast<Node&>(page));
            std::vector<Painter::StoryLine> lines;
            painter.gatherStoryParagraph(page, 0, 0, story, paraIdx, lines);
            for (const Painter::StoryLine& line : lines) {
                if (line.node && line.node->paragraph == paraIdx) {
                    paraLines.push_back(line.node);
                }
            }
        });
    if (paraLines.size() < 2) return false;
    for (std::size_t i = 1; i < paraLines.size(); ++i) {
        const Node& lineNode = *paraLines[i];
        if (!lineNode.children.empty() && lineNode.children.front().kind == NodeKind::Object) {
            return true;
        }
    }
    return false;
}

void mergeParagraphs(model::Paragraph& dst, const model::Paragraph& src) {
    const std::uint32_t fallbackShape = dst.runs.empty() ? (src.runs.empty() ? 0 : src.runs.front().charShapeId)
                                                         : dst.runs.front().charShapeId;
    for (const model::Run& run : src.runs) {
        model::Run filtered;
        filtered.charShapeId = run.charShapeId;
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control && isSectionLeadControl(item.control)) continue;
            if (item.type == model::InlineItem::Type::Text && item.text.empty()) continue;
            filtered.items.push_back(item);
        }
        if (!filtered.items.empty()) {
            if (!dst.runs.empty() && dst.runs.back().charShapeId == filtered.charShapeId) {
                for (model::InlineItem& item : filtered.items) {
                    dst.runs.back().items.push_back(std::move(item));
                }
            } else {
                dst.runs.push_back(std::move(filtered));
            }
        }
    }
    ensureParagraphHasTextRun(dst, fallbackShape);
    dst.lineSegs.clear();
}

bool deleteSelectionRange(model::Document& doc, const std::vector<Node>& pages, int story, int startPara, int startOff,
                          int endPara, int endOff, int& outPara, int& outCharIndex) {
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || startPara < 0 || endPara < startPara || endPara >= static_cast<int>(paras->size())) return false;
    const int startChar = offsetToCharIndex(doc, pages, story, startPara, startOff);
    const int endChar = offsetToCharIndex(doc, pages, story, endPara, endOff);
    outPara = startPara;
    outCharIndex = startChar;
    if (startPara == endPara) {
        eraseCharsInParagraph((*paras)[static_cast<std::size_t>(startPara)], startChar, endChar);
        return true;
    }
    model::Paragraph& firstPara = (*paras)[static_cast<std::size_t>(startPara)];
    model::Paragraph& lastPara = (*paras)[static_cast<std::size_t>(endPara)];
    eraseCharsInParagraph(firstPara, startChar, paragraphCharCount(firstPara));
    eraseCharsInParagraph(lastPara, 0, endChar);
    mergeParagraphs(firstPara, lastPara);
    paras->erase(paras->begin() + startPara + 1, paras->begin() + endPara + 1);
    rebuildStoryMap(doc);
    return true;
}

void rememberCompositionUnderlines(const model::Document& doc, const std::vector<Node>& pages) {
    PointerHold& hold = pointerHold();
    hold.compUnderlines.clear();
    hold.compStartOff = 0;
    hold.compEndOff = 0;
    if (!hold.composing || hold.compStory <= 0 || hold.compPara < 0 || hold.compEnd <= hold.compStart) return;
    const int startOff = charIndexToOffset(doc, pages, hold.compStory, hold.compPara, hold.compStart);
    const int endOff = charIndexToOffset(doc, pages, hold.compStory, hold.compPara, hold.compEnd);
    if (endOff <= startOff) return;
    hold.compStartOff = startOff;
    hold.compEndOff = endOff;
    const std::vector<BlockBox> boxes =
        blockBoxes(doc, pages, hold.compStory, hold.compPara, startOff, hold.compPara, endOff);
    const CaretPlace startCaret = caretForCursor(doc, pages, model::CursorPos{hold.compStory, hold.compPara, endOff});
    for (const BlockBox& box : boxes) {
        if (box.page < 0 || box.page >= static_cast<int>(pages.size()) || box.width <= 0 || box.height <= 0) continue;
        const Node& page = pages[static_cast<std::size_t>(box.page)];
        if (page.width <= 0 || page.height <= 0) continue;
        const std::int32_t rawH =
            startCaret.hit && startCaret.height > 0 ? startCaret.height : box.height;
        const std::int32_t rawTop =
            startCaret.hit && startCaret.page == box.page ? startCaret.y : box.y;
        const std::int32_t charTop = std::clamp(rawTop, box.y, box.y + box.height);
        const std::int32_t charBot = std::clamp(rawTop + rawH, box.y, box.y + box.height);
        const std::int32_t charH = std::max<std::int32_t>(charBot - charTop, 1);
        HeldBox under;
        under.page = box.page;
        under.x = box.x;
        under.y = charTop;
        under.width = box.width;
        under.height = charH;
        under.pageW = page.width;
        under.pageH = page.height;
        under.invert = true;
        under.backR = box.backR;
        under.backG = box.backG;
        under.backB = box.backB;
        hold.compUnderlines.push_back(under);
    }
    touchCaretBlink(hold);
    bumpPointerPicture();
}

struct StoryLineOnPage {
    int page = 0;
    Painter::StoryLine line;
};

std::vector<StoryLineOnPage> gatherStoryLinesInRange(const model::Document& doc, const std::vector<Node>& pages,
                                                     int story, int pStart, int pEnd) {
    std::vector<StoryLineOnPage> all;
    if (story <= 0 || pages.empty()) return all;
    pStart = std::max(0, pStart);
    pEnd = std::min(static_cast<int>(pages.size()) - 1, pEnd);
    Painter painter;
    painter.doc = &doc;
    for (int p = pStart; p <= pEnd; ++p) {
        const Node& curPage = pages[static_cast<std::size_t>(p)];
        if (!Painter::subtreeHasStory(curPage, story)) continue;
        if (curPage.spansLater) ensurePageSpans(doc, const_cast<Node&>(curPage));
        std::vector<Painter::StoryLine> lines;
        painter.gatherStory(curPage, 0, 0, story, lines);
        for (const Painter::StoryLine& line : lines) {
            if (line.node) all.push_back(StoryLineOnPage{p, line});
        }
    }
    return all;
}

std::vector<StoryLineOnPage> gatherAllStoryLines(const model::Document& doc, const std::vector<Node>& pages,
                                                 int story) {
    return gatherStoryLinesInRange(doc, pages, story, 0, static_cast<int>(pages.size()) - 1);
}

int findActiveStoryLineIndex(const model::Document& doc, const std::vector<StoryLineOnPage>& all,
                             const PointerHold& hold) {
    if (all.empty()) return -1;
    Painter painter;
    painter.doc = &doc;
    int byOffset = -1;
    bool prevLineBreak = false;
    int prevMaxOff = -1;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        const StoryLineOnPage& item = all[static_cast<std::size_t>(i)];
        const Node& node = *item.line.node;
        if (node.paragraph != hold.focusPara) continue;
        const std::vector<std::int32_t> edges = painter.glyphEdges(node, item.line.x, item.line.y, nullptr);
        if (edges.empty()) continue;
        const int maxOff = node.start + static_cast<int>(edges.size()) - 1;
        if (hold.focusOff >= node.start && hold.focusOff <= maxOff) {
            const bool startsWithObject =
                !node.children.empty() && node.children.front().kind == NodeKind::Object;
            if (item.page == hold.caretPage &&
                (item.line.y + node.marginTop == hold.caret.y ||
                 painter.placeOnLineAtOffset(item.line, hold.focusOff).y == hold.caret.y)) {
                return i;
            }
            if (!(byOffset >= 0 && !prevLineBreak && hold.focusOff == node.start && hold.focusOff == prevMaxOff &&
                  startsWithObject)) {
                byOffset = i;
            }
        }
        prevLineBreak = node.lineBreak;
        prevMaxOff = maxOff;
    }
    return byOffset;
}

void logEditLayout(const char* kind, int pageBegin, int pageEnd, int paragraph, int lineBegin, int lineEnd,
                   int reusePage, const char* why, double ms) {
    std::fprintf(stderr, "[noting] 조판 %s 쪽 %d-%d 문단 %d", kind, pageBegin, pageEnd, paragraph);
    if (lineBegin >= 0) {
        std::fprintf(stderr, " 줄 %d-", lineBegin);
        if (lineEnd >= 0) std::fprintf(stderr, "%d", lineEnd);
        else std::fprintf(stderr, "끝");
    }
    if (reusePage >= 0) std::fprintf(stderr, " 옛쪽 %d부터 유지", reusePage);
    if (why && why[0]) std::fprintf(stderr, " (%s)", why);
    std::fprintf(stderr, " %.1fms\n", ms);
}

// 증분 조판은 고친 줄의 경계만 지운다. 아래로 밀린 줄은 쪽 경계 캐시에 옛 자리가 남아
// 칸이 옛 자리에 찍히고 새 줄과 겹친다. 무효화한 쪽은 경계를 다시 재게 한다.
void forgetPageBounds(std::vector<Node>& pages, const std::vector<FrameInvalidation>& invalidations) {
    for (const FrameInvalidation& inv : invalidations) {
        if (inv.page < 0) {
            for (Node& page : pages) page.huBoundsReady = false;
            return;
        }
        if (inv.page < static_cast<int>(pages.size())) pages[static_cast<std::size_t>(inv.page)].huBoundsReady = false;
    }
}

EditRelayoutResult relayoutAfterEdit(model::Document& doc, std::vector<Node>& pages, const EditRelayoutSpec& spec) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsedMs = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    };
    // 개체 안 글을 고치면 그 개체를 품은 바깥 문단들의 lineseg 도 낡는다. 지워서 다시 계산하게 한다.
    for (const model::StorySlot* slot = resolveStory(doc, spec.story);
         slot && slot->parentStory > 0 && slot->parentParagraph >= 0; slot = resolveStory(doc, slot->parentStory)) {
        if (model::Paragraph* parent = storyParagraph(doc, slot->parentStory, slot->parentParagraph)) {
            parent->lineSegs.clear();
        }
    }
    EditRelayoutSpec specWithHint = spec;
    if (specWithHint.hintPage < 0) specWithHint.hintPage = currentCaretPageHint();
    // 머리말·꼬리말은 쪽마다 같은 문단을 다시 찍는다. 그 안 글상자 글도 같다.
    // 띠는 본문을 밀지 않으니 그 머리말을 쓰는 쪽의 띠만 다시 놓는다. 못 하면 전체를 다시 놓는다.
    const model::Control* bandControl = nullptr;
    for (const model::StorySlot* slot = resolveStory(doc, spec.story); slot;
         slot = slot->parentStory > 0 ? resolveStory(doc, slot->parentStory) : nullptr) {
        if (slot->control && (slot->control->kind == model::ControlKind::Header ||
                              slot->control->kind == model::ControlKind::Footer)) {
            bandControl = slot->control;
            break;
        }
    }
    const bool band = bandControl != nullptr;
    EditRelayoutResult inc;
    if (band) {
        std::vector<FrameInvalidation> bandInvalidations;
        if (relayoutPageBands(doc, pages, *bandControl, spec.useStoredLineHeight, bandInvalidations)) {
            inc.incremental = true;
            inc.invalidations = std::move(bandInvalidations);
        } else {
            inc.widenReason = "머리말·꼬리말";
        }
    } else {
        inc = relayoutAfterEditIncremental(doc, pages, specWithHint);
    }
    if (inc.incremental && !inc.fullInvalidation) {
        invalidateLinePlaces(inc.invalidations, false);
        forgetPageBounds(pages, inc.invalidations);
        int pageBegin = -1;
        int pageEnd = -1;
        int lineBegin = -1;
        int lineEnd = -1;
        for (const FrameInvalidation& inv : inc.invalidations) {
            if (inv.page < 0) continue;
            const int shown = inv.page + 1;
            if (pageBegin < 0 || shown < pageBegin) pageBegin = shown;
            if (shown > pageEnd) pageEnd = shown;
            if (lineBegin < 0 && (inv.minChild >= 0 || inv.maxChild >= 0)) {
                lineBegin = inv.minChild >= 0 ? inv.minChild + 1 : 1;
                lineEnd = inv.maxChild >= 0 ? inv.maxChild + 1 : -1;
            }
        }
        logEditLayout("증분", pageBegin, pageEnd, spec.paraIndex + 1, lineBegin, lineEnd, -1, nullptr, elapsedMs());
        return inc;
    }
    const SectionReflow span = band ? SectionReflow{} : reflowEditedPages(doc, pages, specWithHint);
    if (span.ok) {
        EditRelayoutResult partial;
        partial.pageListChanged = span.pageListChanged;
        partial.pagesMoved = true;
        partial.pageShiftFrom = span.pageShiftFrom;
        partial.pageShiftDelta = span.pageShiftDelta;
        partial.widenReason = inc.widenReason;
        if (span.dirtyBegin >= 0) {
            for (int page = span.dirtyBegin; page < span.dirtyEnd; ++page) {
                partial.invalidations.push_back(FrameInvalidation{page, {}, -1, -1, false});
            }
        }
        invalidateLinePlaces(partial.invalidations, false);
        if (specWithHint.hintPage >= 0 && specWithHint.hintPage < static_cast<int>(pages.size()) &&
            pages[static_cast<std::size_t>(specWithHint.hintPage)].spansLater) {
            ensurePageSpans(doc, pages[static_cast<std::size_t>(specWithHint.hintPage)]);
        }
        const int reuseShown = span.reusePage >= 0 ? span.reusePage + 1 : -1;
        logEditLayout("구역", span.dirtyBegin + 1, span.dirtyEnd, span.fromParagraph + 1, -1, -1, reuseShown,
                      inc.widenReason, elapsedMs());
        return partial;
    }
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, spec.useStoredLineHeight, spec.useStoredLineHeight);
    if (specWithHint.hintPage >= 0 && specWithHint.hintPage < static_cast<int>(pages.size()) &&
        pages[static_cast<std::size_t>(specWithHint.hintPage)].spansLater) {
        ensurePageSpans(doc, pages[static_cast<std::size_t>(specWithHint.hintPage)]);
    }
    EditRelayoutResult full;
    full.incremental = false;
    full.fullInvalidation = true;
    full.widenReason = inc.widenReason ? inc.widenReason : span.skipReason;
    logEditLayout("문서전체", 1, static_cast<int>(pages.size()), spec.paraIndex + 1, -1, -1, -1, full.widenReason,
                  elapsedMs());
    return full;
}

EditResult makeEditResult(const model::Document& doc, bool handled, bool changed, int story, int paraIdx,
                          const EditRelayoutResult* relayout = nullptr) {
    const PointerHold& hold = pointerHold();
    EditResult res;
    res.handled = handled;
    res.documentChanged = changed;
    res.composing = hold.composing;
    res.dirtyStory = story;
    res.dirtyParagraph = paraIdx;
    if (const model::StorySlot* slot = resolveStory(doc, story)) {
        res.dirtySection = slot->sectionIndex;
    }
    if (relayout) {
        res.fullInvalidation = relayout->fullInvalidation;
        res.pageListChanged = relayout->pageListChanged;
        res.pagesMoved = relayout->pagesMoved;
        res.pageShiftFrom = relayout->pageShiftFrom;
        res.pageShiftDelta = relayout->pageShiftDelta;
        res.invalidations = relayout->invalidations;
    }
    res.cursor = pointerCursor();
    res.caret = hold.caret;
    res.compositionUnderlines = compositionUnderlineMarks();
    return res;
}

}  // namespace paint_detail

namespace {

std::atomic<bool> gReadOnly{false};

// 읽기 전용에서도 받는 키. 커서만 옮기고 문서는 고치지 않는다.
bool movesOnly(EditKey key) {
    return key == EditKey::ArrowLeft || key == EditKey::ArrowRight || key == EditKey::ArrowUp ||
           key == EditKey::ArrowDown || key == EditKey::Home || key == EditKey::End || key == EditKey::Escape;
}

// 막힌 글꼴이 남은 문서는 고치지 않고 셸에 바꾸기를 묻게 한다. 막힌 글꼴로 새 글을 저장하지 않기 위해서다.
bool blockedByLicenseFonts(const model::Document& doc, EditResult& out) {
    if (layout::licenseFontSwaps(doc).empty()) return false;
    out = EditResult{};
    out.handled = true;
    out.needsFontSwap = true;
    out.cursor = pointerCursor();
    out.caret = pointerHold().caret;
    return true;
}

}  // namespace

void setReadOnly(bool on) { gReadOnly.store(on, std::memory_order_relaxed); }

bool readOnly() { return gReadOnly.load(std::memory_order_relaxed); }

EditResult editInsertText(model::Document& doc, std::vector<Node>& pages, const std::string& text, bool composing,
                          bool useStoredLineHeight) {
    if (readOnly()) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.paragraph < 0 || cur.paragraph >= static_cast<int>(paras->size())) return {};

    const int story = cur.story;
    int paraIdx = cur.paragraph;
    int charIndex = 0;
    int paraDelta = 0;
    int editOldEndOff = 0;
    std::optional<std::uint32_t> overrideShapeId;
    if (hold.hasPendingCharShape && hold.pendingCharStory == story && hold.pendingCharPara == paraIdx &&
        (hold.pendingCharOffset == cur.offset ||
         (hold.composing && hold.compStory == story && hold.compPara == paraIdx))) {
        overrideShapeId = hold.pendingCharShapeId;
    }

    if (hold.composing && hold.compStory == story && hold.compPara == paraIdx &&
        hold.compPara < static_cast<int>(paras->size())) {
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(paraIdx)];
        editOldEndOff = hold.compEnd;
        eraseCharsInParagraph(para, hold.compStart, hold.compEnd);
        charIndex = hold.compStart;
    } else {
        clearComposition(hold);
        if (cur.hasSelection) {
            paraDelta = -(cur.endParagraph - cur.startParagraph);
            const int endCharIdx =
                (paraDelta == 0) ? offsetToCharIndex(doc, pages, story, cur.endParagraph, cur.endOffset) : 0;
            deleteSelectionRange(doc, pages, story, cur.startParagraph, cur.startOffset, cur.endParagraph,
                                 cur.endOffset, paraIdx, charIndex);
            editOldEndOff = (paraDelta == 0) ? endCharIdx : charIndex;
        } else {
            charIndex = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
            editOldEndOff = charIndex;
        }
    }

    model::Paragraph& targetPara = (*paras)[static_cast<std::size_t>(paraIdx)];
    const int inserted = insertTextInParagraph(targetPara, charIndex, text);
    const int newCharIndex = charIndex + inserted;
    if (overrideShapeId.has_value() && inserted > 0) {
        const std::uint32_t forcedShape = *overrideShapeId;
        transformParagraphRangeCharShape(targetPara, charIndex, newCharIndex,
                                         [forcedShape](std::uint32_t) { return forcedShape; });
    }

    if (composing && inserted > 0) {
        hold.composing = true;
        hold.compStory = story;
        hold.compPara = paraIdx;
        hold.compStart = charIndex;
        hold.compEnd = newCharIndex;
    } else {
        clearComposition(hold);
    }
    hold.preferredX = -1;

    const EditRelayoutResult relayout = relayoutAfterEdit(
        doc, pages, EditRelayoutSpec{story, paraIdx, paraDelta, charIndex, editOldEndOff, newCharIndex, useStoredLineHeight});
    const int newOff = charIndexToOffset(doc, pages, story, paraIdx, newCharIndex);
    if (overrideShapeId.has_value()) {
        hold.hasPendingCharShape = true;
        hold.pendingCharShapeId = *overrideShapeId;
        hold.pendingCharStory = story;
        hold.pendingCharPara = paraIdx;
        hold.pendingCharOffset = newOff;
    }
    setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
    rememberCompositionUnderlines(doc, pages);
    return makeEditResult(doc, true, true, story, paraIdx, &relayout);
}

EditResult editSwapLicenseFonts(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                                const std::vector<std::pair<std::string, std::string>>& chosen) {
    if (readOnly()) return {};
    const std::vector<layout::FontSwap> swaps = layout::licenseFontSwaps(doc);
    PointerHold& hold = pointerHold();
    if (swaps.empty()) return makeEditResult(doc, false, false, hold.story, hold.focusPara);
    for (const layout::FontSwap& swap : swaps) {
        std::string to = swap.to;
        for (const auto& [from, pick] : chosen) {
            if (from == swap.from && layout::fontPickable(pick)) to = pick;
        }
        model::replaceFontFace(doc, swap.from, to);
    }
    model::StoryMap& map = ensureStoryMap(doc);
    for (int story = 1; story <= map.count(); ++story) {
        std::vector<model::Paragraph>* paras = map.paragraphs(story);
        if (!paras || map.storyOf(paras) != story) continue;
        for (model::Paragraph& para : *paras) para.lineSegs.clear();
    }
    clearComposition(hold);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    const PointerCursor cur = pointerCursor();
    if (cur.active && cur.story > 0) {
        setPointerCursor(doc, pages, model::CursorPos{cur.story, cur.paragraph, cur.offset});
    }
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &full);
}

EditResult editCommitComposition(model::Document& doc, const std::vector<Node>& pages) {
    PointerHold& hold = pointerHold();
    const int story = hold.story;
    const int paraIdx = hold.focusPara;
    const bool wasComposing = hold.composing;
    clearComposition(hold);
    if (wasComposing) bumpPointerPicture();
    (void)pages;
    return makeEditResult(doc, wasComposing, false, story, paraIdx);
}

EditResult editKeyDown(model::Document& doc, std::vector<Node>& pages, EditKey key, bool shift, bool ctrl, bool alt,
                       bool useStoredLineHeight) {
    (void)ctrl;
    (void)alt;
    if (readOnly() && !movesOnly(key)) return {};
    if (EditResult blocked; !movesOnly(key) && blockedByLicenseFonts(doc, blocked)) return blocked;
    PointerHold& hold = pointerHold();
    // Shift+Esc 는 머리말·꼬리말 편집을 끝내고 들어오기 전 본문 자리로 커서를 돌린다.
    if (key == EditKey::Escape) {
        if (hold.bandStory <= 0 || !shift) return {};
        if (hold.editStory == hold.bandStory) hold.editStory = 0;
        hold.bandStory = 0;
        clearComposition(hold);
        const model::CursorPos back{hold.bandReturnStory > 0 ? hold.bandReturnStory : 1,
                                    hold.bandReturnStory > 0 ? hold.bandReturnPara : 0,
                                    hold.bandReturnStory > 0 ? hold.bandReturnOff : 0};
        setPointerCursor(doc, pages, back);
        return makeEditResult(doc, true, false, back.story, back.paragraph);
    }
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.paragraph < 0 || cur.paragraph >= static_cast<int>(paras->size())) return {};

    const int story = cur.story;
    int paraIdx = cur.paragraph;

    if (key == EditKey::Tab) {
        return editInsertText(doc, pages, "\t", false, useStoredLineHeight);
    }

    if (key == EditKey::Enter) {
        if (hold.composing) {
            clearComposition(hold);
        }
        // Shift+Enter 는 문단을 나누지 않고 같은 문단 안에 줄바꿈 문자('\n')만 삽입한다.
        if (shift) {
            return editInsertText(doc, pages, "\n", false, useStoredLineHeight);
        }
        int charIndex = 0;
        int selParaDelta = 0;
        if (cur.hasSelection) {
            selParaDelta = -(cur.endParagraph - cur.startParagraph);
            deleteSelectionRange(doc, pages, story, cur.startParagraph, cur.startOffset, cur.endParagraph,
                                 cur.endOffset, paraIdx, charIndex);
        } else {
            charIndex = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
        }
        const bool insertMidLine = splitsBeforeWrappedInlineObject(
            doc, pages, story, paraIdx, charIndex, (*paras)[static_cast<std::size_t>(paraIdx)]);
        model::Paragraph nextPara = splitParagraphAt((*paras)[static_cast<std::size_t>(paraIdx)], charIndex);
        int insertedParas = 1;
        if (insertMidLine) {
            const model::Paragraph& leftRef = (*paras)[static_cast<std::size_t>(paraIdx)];
            model::Paragraph midPara;
            midPara.paraShapeId = leftRef.paraShapeId;
            midPara.styleId = leftRef.styleId;
            const std::uint32_t midShape = leftRef.runs.empty() ? 0 : leftRef.runs.back().charShapeId;
            ensureParagraphHasTextRun(midPara, midShape);
            paras->insert(paras->begin() + paraIdx + 1, std::move(midPara));
            paras->insert(paras->begin() + paraIdx + 2, std::move(nextPara));
            insertedParas = 2;
        } else {
            paras->insert(paras->begin() + paraIdx + 1, std::move(nextPara));
        }
        rebuildStoryMap(doc);
        hold.preferredX = -1;
        const EditRelayoutResult relayout = relayoutAfterEdit(
            doc, pages,
            EditRelayoutSpec{story, paraIdx, selParaDelta + insertedParas, charIndex, charIndex, charIndex,
                             useStoredLineHeight});
        setPointerCursor(doc, pages, model::CursorPos{story, paraIdx + 1, 0});
        return makeEditResult(doc, true, true, story, paraIdx + 1, &relayout);
    }

    if (key == EditKey::Backspace) {
        if (hold.composing && hold.compStory == story && hold.compPara == paraIdx && hold.compEnd > hold.compStart) {
            model::Paragraph& para = (*paras)[static_cast<std::size_t>(paraIdx)];
            const int oldEnd = hold.compEnd;
            eraseCharsInParagraph(para, hold.compEnd - 1, hold.compEnd);
            --hold.compEnd;
            const int newEnd = hold.compEnd;
            if (hold.compEnd <= hold.compStart) {
                clearComposition(hold);
            }
            hold.preferredX = -1;
            const EditRelayoutResult relayout = relayoutAfterEdit(
                doc, pages, EditRelayoutSpec{story, paraIdx, 0, newEnd, oldEnd, newEnd, useStoredLineHeight});
            const int newOff = charIndexToOffset(doc, pages, story, paraIdx, newEnd);
            setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
            rememberCompositionUnderlines(doc, pages);
            return makeEditResult(doc, true, true, story, paraIdx, &relayout);
        }
        clearComposition(hold);
        hold.preferredX = -1;
        int charIndex = 0;
        if (cur.hasSelection) {
            const int paraDelta = -(cur.endParagraph - cur.startParagraph);
            const int endCharIdx =
                (paraDelta == 0) ? offsetToCharIndex(doc, pages, story, cur.endParagraph, cur.endOffset) : 0;
            deleteSelectionRange(doc, pages, story, cur.startParagraph, cur.startOffset, cur.endParagraph,
                                 cur.endOffset, paraIdx, charIndex);
            const int oldEnd = (paraDelta == 0) ? endCharIdx : charIndex;
            const EditRelayoutResult relayout = relayoutAfterEdit(
                doc, pages, EditRelayoutSpec{story, paraIdx, paraDelta, charIndex, oldEnd, charIndex, useStoredLineHeight});
            const int newOff = charIndexToOffset(doc, pages, story, paraIdx, charIndex);
            setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
            return makeEditResult(doc, true, true, story, paraIdx, &relayout);
        }
        charIndex = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
        if (charIndex > 0) {
            eraseCharsInParagraph((*paras)[static_cast<std::size_t>(paraIdx)], charIndex - 1, charIndex);
            const EditRelayoutResult relayout = relayoutAfterEdit(
                doc, pages, EditRelayoutSpec{story, paraIdx, 0, charIndex - 1, charIndex, charIndex - 1, useStoredLineHeight});
            const int newOff = charIndexToOffset(doc, pages, story, paraIdx, charIndex - 1);
            setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
            return makeEditResult(doc, true, true, story, paraIdx, &relayout);
        }
        if (paraIdx > 0) {
            model::Paragraph& prevPara = (*paras)[static_cast<std::size_t>(paraIdx - 1)];
            const int prevLen = paragraphCharCount(prevPara);
            mergeParagraphs(prevPara, (*paras)[static_cast<std::size_t>(paraIdx)]);
            paras->erase(paras->begin() + paraIdx);
            rebuildStoryMap(doc);
            --paraIdx;
            const EditRelayoutResult relayout = relayoutAfterEdit(
                doc, pages, EditRelayoutSpec{story, paraIdx, -1, prevLen, prevLen, prevLen, useStoredLineHeight});
            const int newOff = charIndexToOffset(doc, pages, story, paraIdx, prevLen);
            setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
            return makeEditResult(doc, true, true, story, paraIdx, &relayout);
        }
        return makeEditResult(doc, false, false, story, paraIdx);
    }

    if (key == EditKey::Delete) {
        clearComposition(hold);
        hold.preferredX = -1;
        int charIndex = 0;
        if (cur.hasSelection) {
            const int paraDelta = -(cur.endParagraph - cur.startParagraph);
            const int endCharIdx =
                (paraDelta == 0) ? offsetToCharIndex(doc, pages, story, cur.endParagraph, cur.endOffset) : 0;
            deleteSelectionRange(doc, pages, story, cur.startParagraph, cur.startOffset, cur.endParagraph,
                                 cur.endOffset, paraIdx, charIndex);
            const int oldEnd = (paraDelta == 0) ? endCharIdx : charIndex;
            const EditRelayoutResult relayout = relayoutAfterEdit(
                doc, pages, EditRelayoutSpec{story, paraIdx, paraDelta, charIndex, oldEnd, charIndex, useStoredLineHeight});
            const int newOff = charIndexToOffset(doc, pages, story, paraIdx, charIndex);
            setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
            return makeEditResult(doc, true, true, story, paraIdx, &relayout);
        }
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(paraIdx)];
        charIndex = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
        const int paraLen = paragraphCharCount(para);
        if (charIndex < paraLen) {
            eraseCharsInParagraph(para, charIndex, charIndex + 1);
            const EditRelayoutResult relayout = relayoutAfterEdit(
                doc, pages, EditRelayoutSpec{story, paraIdx, 0, charIndex, charIndex + 1, charIndex, useStoredLineHeight});
            const int newOff = charIndexToOffset(doc, pages, story, paraIdx, charIndex);
            setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
            return makeEditResult(doc, true, true, story, paraIdx, &relayout);
        }
        if (paraIdx + 1 < static_cast<int>(paras->size())) {
            mergeParagraphs(para, (*paras)[static_cast<std::size_t>(paraIdx + 1)]);
            paras->erase(paras->begin() + paraIdx + 1);
            rebuildStoryMap(doc);
            const EditRelayoutResult relayout = relayoutAfterEdit(
                doc, pages, EditRelayoutSpec{story, paraIdx, -1, paraLen, paraLen, paraLen, useStoredLineHeight});
            const int newOff = charIndexToOffset(doc, pages, story, paraIdx, paraLen);
            setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
            return makeEditResult(doc, true, true, story, paraIdx, &relayout);
        }
        return makeEditResult(doc, false, false, story, paraIdx);
    }

    // 방향키 및 Home/End 이동 시 조합 중 상태를 확정한다.
    clearComposition(hold);

    if (key == EditKey::ArrowLeft) {
        hold.preferredX = -1;
        if (!shift && cur.hasSelection) {
            const model::CursorPos target{story, cur.startParagraph, cur.startOffset};
            setPointerCursor(doc, pages, target, target);
            return makeEditResult(doc, true, false, story, cur.startParagraph);
        }
        int nextPara = paraIdx;
        int nextChar = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
        if (nextChar > 0) {
            --nextChar;
        } else if (nextPara > 0) {
            --nextPara;
            nextChar = paragraphCharCount((*paras)[static_cast<std::size_t>(nextPara)]);
        } else {
            return makeEditResult(doc, false, false, story, paraIdx);
        }
        const int nextOff = charIndexToOffset(doc, pages, story, nextPara, nextChar);
        const model::CursorPos focusPos{story, nextPara, nextOff};
        const model::CursorPos anchorPos =
            shift ? model::CursorPos{story, cur.anchorParagraph, cur.anchorOffset} : focusPos;
        setPointerCursor(doc, pages, anchorPos, focusPos);
        return makeEditResult(doc, true, false, story, nextPara);
    }

    if (key == EditKey::ArrowRight) {
        hold.preferredX = -1;
        if (!shift && cur.hasSelection) {
            const model::CursorPos target{story, cur.endParagraph, cur.endOffset};
            setPointerCursor(doc, pages, target, target);
            return makeEditResult(doc, true, false, story, cur.endParagraph);
        }
        int nextPara = paraIdx;
        int nextChar = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
        const int paraLen = paragraphCharCount((*paras)[static_cast<std::size_t>(paraIdx)]);
        if (nextChar < paraLen) {
            ++nextChar;
        } else if (nextPara + 1 < static_cast<int>(paras->size())) {
            ++nextPara;
            nextChar = 0;
        } else {
            return makeEditResult(doc, false, false, story, paraIdx);
        }
        const int nextOff = charIndexToOffset(doc, pages, story, nextPara, nextChar);
        const model::CursorPos focusPos{story, nextPara, nextOff};
        const model::CursorPos anchorPos =
            shift ? model::CursorPos{story, cur.anchorParagraph, cur.anchorOffset} : focusPos;
        setPointerCursor(doc, pages, anchorPos, focusPos);
        return makeEditResult(doc, true, false, story, nextPara);
    }

    if (key == EditKey::Home || key == EditKey::End) {
        hold.preferredX = -1;
        clearPendingCharShape(hold);
        std::vector<StoryLineOnPage> all;
        int lineIdx = -1;
        if (hold.caretPage >= 0 && hold.caretPage < static_cast<int>(pages.size())) {
            all = gatherStoryLinesInRange(doc, pages, story, hold.caretPage - 1, hold.caretPage + 1);
            lineIdx = findActiveStoryLineIndex(doc, all, hold);
        }
        if (lineIdx < 0) {
            all = gatherAllStoryLines(doc, pages, story);
            lineIdx = findActiveStoryLineIndex(doc, all, hold);
        }
        if (lineIdx < 0) return makeEditResult(doc, false, false, story, paraIdx);
        const StoryLineOnPage& active = all[static_cast<std::size_t>(lineIdx)];
        Painter painter;
        painter.doc = &doc;
        const std::vector<std::int32_t> edges =
            painter.glyphEdges(*active.line.node, active.line.x, active.line.y, nullptr);
        if (edges.empty()) return makeEditResult(doc, false, false, story, paraIdx);
        const int targetOff = key == EditKey::Home
                                  ? active.line.node->start
                                  : active.line.node->start + static_cast<int>(edges.size()) - 1;
        CaretPlace place = painter.placeOnLineAtOffset(active.line, targetOff);
        place.page = active.page;
        hold.focusPara = place.paragraph;
        hold.focusOff = place.offset;
        if (!shift) {
            hold.anchorPara = place.paragraph;
            hold.anchorOff = place.offset;
        }
        placeCaret(hold, place, active.page, pages, &doc);
        rememberSelection(doc, pages);
        return makeEditResult(doc, true, false, story, place.paragraph);
    }

    if (key == EditKey::ArrowUp || key == EditKey::ArrowDown) {
        clearPendingCharShape(hold);
        std::vector<StoryLineOnPage> all;
        int lineIdx = -1;
        int targetLineIdx = -1;
        if (hold.caretPage >= 0 && hold.caretPage < static_cast<int>(pages.size())) {
            all = gatherStoryLinesInRange(doc, pages, story, hold.caretPage - 1, hold.caretPage + 1);
            lineIdx = findActiveStoryLineIndex(doc, all, hold);
            if (lineIdx >= 0) {
                targetLineIdx = key == EditKey::ArrowUp ? lineIdx - 1 : lineIdx + 1;
                if (targetLineIdx < 0 || targetLineIdx >= static_cast<int>(all.size())) {
                    lineIdx = -1;
                }
            }
        }
        if (lineIdx < 0) {
            all = gatherAllStoryLines(doc, pages, story);
            lineIdx = findActiveStoryLineIndex(doc, all, hold);
            if (lineIdx < 0) return makeEditResult(doc, false, false, story, paraIdx);
            targetLineIdx = key == EditKey::ArrowUp ? lineIdx - 1 : lineIdx + 1;
            if (targetLineIdx < 0 || targetLineIdx >= static_cast<int>(all.size())) {
                return makeEditResult(doc, false, false, story, paraIdx);
            }
        }
        if (hold.preferredX < 0) hold.preferredX = hold.caret.x;
        const StoryLineOnPage& target = all[static_cast<std::size_t>(targetLineIdx)];
        Painter painter;
        painter.doc = &doc;
        CaretPlace place = painter.placeOnLine(target.line, hold.preferredX, false, -1, -1);
        if (!place.hit) return makeEditResult(doc, false, false, story, paraIdx);
        place.page = target.page;
        hold.focusPara = place.paragraph;
        hold.focusOff = place.offset;
        if (!shift) {
            hold.anchorPara = place.paragraph;
            hold.anchorOff = place.offset;
        }
        placeCaret(hold, place, target.page, pages, &doc);
        rememberSelection(doc, pages);
        return makeEditResult(doc, true, false, story, place.paragraph);
    }

    return makeEditResult(doc, false, false, story, paraIdx);
}

EditResult editApplyFormat(model::Document& doc, std::vector<Node>& pages, const FormatPatch& patch,
                           bool useStoredLineHeight) {
    if (readOnly()) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    const bool paraFields = formatFieldsTouchPara(patch.fields);
    const bool hasParaPatch =
        patch.hasStyleId || patch.hasAlign || patch.hasLineSpacingType || patch.hasLineSpacing || paraFields;
    const bool hasCharPatch = patch.hasFont || patch.hasFontSize || patch.hasBold || patch.hasItalic ||
                              patch.hasUnderline || patch.hasUnderlineType || patch.hasStrikeout || patch.hasColor ||
                              formatFieldsTouchChar(patch.fields);
    PointerHold& hold = pointerHold();
    if (!hasParaPatch && !hasCharPatch) {
        return makeEditResult(doc, false, false, hold.story, hold.focusPara);
    }

    PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) {
        if (!doc.sections.empty() && !doc.sections.front().paragraphs.empty() && !pages.empty()) {
            setPointerCursor(doc, pages, model::CursorPos{1, 0, 0});
            cur = pointerCursor();
        }
        if (!cur.active || cur.story <= 0) return {};
    }

    if (hold.composing) {
        clearComposition(hold);
    }

    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || paras->empty()) return {};

    const int story = cur.story;
    const int maxParaIdx = static_cast<int>(paras->size()) - 1;
    const int startPara = std::clamp(cur.hasSelection ? cur.startParagraph : cur.paragraph, 0, maxParaIdx);
    const int endPara = std::clamp(cur.hasSelection ? cur.endParagraph : cur.paragraph, startPara, maxParaIdx);
    const int anchorPara = std::clamp(cur.anchorParagraph, 0, maxParaIdx);
    const int focusPara = std::clamp(cur.paragraph, 0, maxParaIdx);
    const int anchorChar = offsetToCharIndex(doc, pages, story, anchorPara, cur.anchorOffset);
    const int focusChar = offsetToCharIndex(doc, pages, story, focusPara, cur.offset);
    const int startChar =
        cur.hasSelection ? offsetToCharIndex(doc, pages, story, startPara, cur.startOffset) : focusChar;
    const int endChar = cur.hasSelection ? offsetToCharIndex(doc, pages, story, endPara, cur.endOffset) : focusChar;

    bool documentChanged = false;
    bool setPendingShape = false;
    std::uint32_t nextPendingShapeId = 0;

    if (hasParaPatch) {
        for (int p = startPara; p <= endPara; ++p) {
            model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
            if (patch.hasStyleId) {
                para.styleId = patch.styleId;
                if (const model::Style* st = Painter::findStyle(doc, patch.styleId)) {
                    if (st->type.empty() || st->type == "PARA") {
                        para.paraShapeId = st->paraShapeId;
                        for (model::Run& run : para.runs) {
                            run.charShapeId = st->charShapeId;
                        }
                        if (!cur.hasSelection && p == focusPara) {
                            setPendingShape = true;
                            nextPendingShapeId = st->charShapeId;
                        }
                    } else if (st->type == "CHAR") {
                        const std::uint32_t targetShape = st->charShapeId;
                        if (cur.hasSelection) {
                            const int paraLen = paragraphCharCount(para);
                            const int pStart = (p == startPara) ? std::clamp(startChar, 0, paraLen) : 0;
                            const int pEnd = (p == endPara) ? std::clamp(endChar, 0, paraLen) : paraLen;
                            if (pEnd > pStart || paraLen == 0) {
                                transformParagraphRangeCharShape(para, pStart, pEnd,
                                                                 [targetShape](std::uint32_t) { return targetShape; });
                            }
                        } else if (p == focusPara) {
                            setPendingShape = true;
                            nextPendingShapeId = targetShape;
                        }
                    }
                }
            }
            if (patch.hasAlign || patch.hasLineSpacingType || patch.hasLineSpacing || paraFields) {
                para.paraShapeId = deriveParaShapeId(doc, para.paraShapeId, patch);
            }
            para.lineSegs.clear();
            documentChanged = true;
        }
    }

    if (hasCharPatch) {
        if (patch.hasFont && !patch.font.empty()) {
            // 새 글꼴을 CharShape 에 등록한 뒤 글꼴 메트릭을 준비한다.
        }
        if (cur.hasSelection) {
            for (int p = startPara; p <= endPara; ++p) {
                model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
                const int paraLen = paragraphCharCount(para);
                const int pStart = (p == startPara) ? std::clamp(startChar, 0, paraLen) : 0;
                const int pEnd = (p == endPara) ? std::clamp(endChar, 0, paraLen) : paraLen;
                if (pEnd > pStart || paraLen == 0) {
                    transformParagraphRangeCharShape(para, pStart, pEnd, [&](std::uint32_t oldId) {
                        return deriveCharShapeId(doc, oldId, patch);
                    });
                    documentChanged = true;
                }
            }
            clearPendingCharShape(hold);
        } else {
            std::uint32_t baseShapeId = 0;
            if (setPendingShape) {
                baseShapeId = nextPendingShapeId;
            } else if (hold.hasPendingCharShape && hold.pendingCharStory == story &&
                       hold.pendingCharPara == focusPara && hold.pendingCharOffset == cur.offset) {
                baseShapeId = hold.pendingCharShapeId;
            } else {
                baseShapeId = cursorStyle(doc, pages).charShapeId;
            }
            const std::uint32_t derivedId = deriveCharShapeId(doc, baseShapeId, patch);
            setPendingShape = true;
            nextPendingShapeId = derivedId;
            model::Paragraph& para = (*paras)[static_cast<std::size_t>(focusPara)];
            if (paragraphCharCount(para) == 0) {
                ensureParagraphHasTextRun(para, derivedId);
                for (model::Run& run : para.runs) {
                    run.charShapeId = derivedId;
                }
                para.lineSegs.clear();
                documentChanged = true;
            }
        }
        if (patch.hasFont && !patch.font.empty()) {
            layout::warmDocumentFonts(doc);
        }
    }

    hold.preferredX = -1;
    EditRelayoutResult relayout{};
    const EditRelayoutResult* relayoutPtr = nullptr;
    if (documentChanged) {
        relayout.incremental = true;
        relayout.fullInvalidation = false;
        for (int p = startPara; p <= endPara; ++p) {
            const int paraLen = paragraphCharCount((*paras)[static_cast<std::size_t>(p)]);
            EditRelayoutSpec spec{story, p, 0, 0, paraLen, paraLen, useStoredLineHeight};
            spec.paraShapeChanged = hasParaPatch;
            EditRelayoutResult step = relayoutAfterEdit(doc, pages, spec);
            if (step.fullInvalidation) {
                relayout = std::move(step);
                break;
            }
            for (FrameInvalidation& inv : step.invalidations) {
                relayout.invalidations.push_back(std::move(inv));
            }
        }
        relayoutPtr = &relayout;
    }

    const int newAnchorOff = charIndexToOffset(doc, pages, story, anchorPara, anchorChar);
    const int newFocusOff = charIndexToOffset(doc, pages, story, focusPara, focusChar);
    if (setPendingShape && !cur.hasSelection) {
        hold.hasPendingCharShape = true;
        hold.pendingCharShapeId = nextPendingShapeId;
        hold.pendingCharStory = story;
        hold.pendingCharPara = focusPara;
        hold.pendingCharOffset = newFocusOff;
    } else if (!cur.hasSelection && hold.hasPendingCharShape && hold.pendingCharStory == story &&
               hold.pendingCharPara == focusPara) {
        hold.pendingCharOffset = newFocusOff;
    }
    setPointerCursor(doc, pages, model::CursorPos{story, anchorPara, newAnchorOff},
                     model::CursorPos{story, focusPara, newFocusOff});
    return makeEditResult(doc, true, documentChanged, story, focusPara, relayoutPtr);
}

}  // namespace noting::render
