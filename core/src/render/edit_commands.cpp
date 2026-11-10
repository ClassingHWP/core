#include "chwp/render/paint.hpp"

#include <algorithm>
#include <map>
#include <atomic>
#include <functional>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "chwp/common/unicode.hpp"
#include "chwp/render/format_view.hpp"
#include "chwp/render/paragraph_layout.hpp"
#include "chwp/render/scene.hpp"
#include "chwp/model/table_edit.hpp"
#include "chwp/unit.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"
#include "render/picture.hpp"

// 편집 명령. 글자 넣기·지우기, 키 입력, 조합, 글자·문단 모양 적용, 문단 나누기·합치기.
// 모델을 고친 뒤 relayoutAfterEdit 로 고친 자리만 다시 놓고, 캐럿과 선택을 옮긴다.

namespace chwp::render {

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

// 각주·미주 번호와 덧말·글자 겹치기는 글자 한 칸이다. 앞뒤에 커서가 서고, 지우면 그 컨트롤이 빠진다.
bool isCaretCharControl(const model::Control& ctrl) {
    return isInlineObjectControl(ctrl) || ctrl.kind == model::ControlKind::Footnote ||
           ctrl.kind == model::ControlKind::Endnote || ctrl.kind == model::ControlKind::Dutmal ||
           ctrl.kind == model::ControlKind::Compose;
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
                        lineStartsWithObject(*line.node);
                    const PointerHold& hold = pointerHold();
                    // 세로쓰기 커서는 종이 자리라 돌린 틀의 줄 윗변으로 되돌려 견준다.
                    const std::int32_t caretTop =
                        hold.caret.turned && line.turn.on()
                            ? line.turn.x + line.turn.span - hold.caret.x - hold.caret.height
                            : hold.caret.y;
                    const bool caretWasOnThisLine =
                        hold.caret.hit && std::abs(caretTop - (line.y + line.node->marginTop)) <= 4;
                    if (bestLineStart >= 0 && !prevLineBreak && offset == line.node->start && offset == prevMaxOff &&
                        startsWithObject && !caretWasOnThisLine) {
                        prevLineBreak = line.node->lineData().lineBreak;
                        prevMaxOff = maxOff;
                        continue;
                    }
                    bestLineStart = line.node->start;
                }
                prevLineBreak = line.node->lineData().lineBreak;
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

    // 글 조각 바로 뒤가 필드 끝이면 그 조각 끝에 붙이지 않는다. 하이퍼링크 끝에서 친 글은 필드 밖(필드 끝 뒤)이다.
    const auto fieldEndFollows = [&](std::size_t runIndex, std::size_t itemIndex) {
        for (std::size_t r = runIndex; r < para.runs.size(); ++r) {
            const std::vector<model::InlineItem>& items = para.runs[r].items;
            for (std::size_t k = r == runIndex ? itemIndex + 1 : 0; k < items.size(); ++k) {
                const model::InlineItem& next = items[k];
                if (next.type == model::InlineItem::Type::Control && next.control.kind == model::ControlKind::FieldEnd) {
                    return true;
                }
                return false;
            }
        }
        return false;
    };
    int pos = 0;
    for (std::size_t runIndex = 0; runIndex < para.runs.size(); ++runIndex) {
        model::Run& run = para.runs[runIndex];
        for (std::size_t i = 0; i < run.items.size(); ++i) {
            model::InlineItem& item = run.items[i];
            if (item.type == model::InlineItem::Type::Text) {
                const int len = utf8CodePointCount(item.text);
                const int itemStart = pos;
                const int itemEnd = pos + len;
                pos = itemEnd;
                if (charIndex >= itemStart && charIndex <= itemEnd &&
                    !(charIndex == itemEnd && len > 0 && fieldEndFollows(runIndex, i))) {
                    // 계산식 결과 맨 앞에서 친 글은 결과를 고친 것이 아니라 필드 앞에 쓴 글이다.
                    if (charIndex == itemStart && i > 0 && run.items[i - 1].type == model::InlineItem::Type::Control &&
                        run.items[i - 1].control.kind == model::ControlKind::Field && run.items[i - 1].control.ctrlId == "%fmu") {
                        run.items.insert(run.items.begin() + static_cast<std::ptrdiff_t>(i - 1), model::InlineItem::makeText(utf8Text));
                        para.lineSegs.clear();
                        return insertedCount;
                    }
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

int replaceCharsInParagraph(model::Paragraph& para, int from, int to, const std::string& replacement) {
    std::vector<std::string> chars;
    for (std::size_t i = 0; i < replacement.size();) {
        std::size_t len = 1;
        while (i + len < replacement.size() && (static_cast<unsigned char>(replacement[i + len]) & 0xC0) == 0x80) ++len;
        chars.push_back(replacement.substr(i, len));
        i += len;
    }
    const int oldCount = std::max(0, to - from);
    const int newCount = static_cast<int>(chars.size());
    const int common = std::min(oldCount, newCount);
    // 옛 글자 바로 뒤에 넣으면 그 글자가 든 item 에 붙는다(insertTextInParagraph). 그다음 옛 글자를 지운다.
    for (int i = 0; i < common; ++i) {
        insertTextInParagraph(para, from + i + 1, chars[static_cast<std::size_t>(i)]);
        eraseCharsInParagraph(para, from + i, from + i + 1);
    }
    if (newCount > oldCount) {
        std::string rest;
        for (int i = oldCount; i < newCount; ++i) rest += chars[static_cast<std::size_t>(i)];
        insertTextInParagraph(para, from + oldCount, rest);
    } else if (oldCount > newCount) {
        eraseCharsInParagraph(para, from + newCount, to);
    }
    para.lineSegs.clear();
    return from + newCount;
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

// 구간 글자의 글자 모양을 mapShapeId 로 바꾼다. markpen 을 주면 그 구간의 형광펜도 그 색으로 바꾼다(빈 색은 지움).
// 글자 조각을 나눠도 조각마다 형광펜은 그대로 간다.
template <typename Fn>
void transformParagraphRangeCharShape(model::Paragraph& para, int startChar, int endChar, Fn&& mapShapeId,
                                      const std::string* markpen = nullptr) {
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
                out.back().items.back().type == model::InlineItem::Type::Text &&
                out.back().items.back().markpen == item.markpen) {
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
                // 나눈 조각도 원래 형광펜을 지닌다. 구간 안 조각만 새 형광펜이다.
                const auto slice = [&](int from, int count, bool inside) {
                    model::InlineItem piece = model::InlineItem::makeText(utf8SliceCodePoints(item.text, from, count));
                    piece.markpen = inside && markpen ? *markpen : item.markpen;
                    return piece;
                };
                if (itemEnd <= startChar || itemStart >= endChar) {
                    appendItemToRuns(newRuns, origShapeId, std::move(item));
                } else if (itemStart >= startChar && itemEnd <= endChar) {
                    if (markpen) item.markpen = *markpen;
                    appendItemToRuns(newRuns, mapShapeId(origShapeId), std::move(item));
                } else {
                    const int leftLen = std::max(0, startChar - itemStart);
                    const int midStart = std::max(0, startChar - itemStart);
                    const int midEnd = std::min(len, endChar - itemStart);
                    const int midLen = std::max(0, midEnd - midStart);
                    const int rightStart = midEnd;
                    const int rightLen = std::max(0, len - rightStart);
                    if (leftLen > 0) appendItemToRuns(newRuns, origShapeId, slice(0, leftLen, false));
                    if (midLen > 0) appendItemToRuns(newRuns, mapShapeId(origShapeId), slice(midStart, midLen, true));
                    if (rightLen > 0) appendItemToRuns(newRuns, origShapeId, slice(rightStart, rightLen, false));
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
           a.suppressLineNumbers == b.suppressLineNumbers &&
           a.borderOffsetLeft == b.borderOffsetLeft && a.borderOffsetRight == b.borderOffsetRight &&
           a.borderOffsetTop == b.borderOffsetTop && a.borderOffsetBottom == b.borderOffsetBottom &&
           a.borderConnect == b.borderConnect && a.borderIgnoreMargin == b.borderIgnoreMargin;
}

// 왼쪽·오른쪽 여백 한 단계(한글 2024: 100HU).
constexpr std::int32_t kMarginStepHu = 100;

// 한글 2024 여백 한 단계의 끝값. 줄이면 -(폭 - 1000)(0 아래로는 안 감)까지, 늘리면 폭 - 맞은편 여백 - 1500 까지다.
// boxWidth 는 그 문단이 놓인 단(표 칸이면 칸 안) 폭이다.
std::int32_t steppedMargin(std::int32_t now, std::int32_t other, int step, std::int32_t boxWidth) {
    if (step > 0) return std::min(now + kMarginStepHu, boxWidth - other - 1500);
    return std::max(now - kMarginStepHu, -std::max(0, boxWidth - 1000));
}

std::uint32_t deriveParaShapeId(model::Document& doc, std::uint32_t baseId, const FormatPatch& patch,
                                std::int32_t boxWidth) {
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
    if (patch.indentStep != 0) next.indent += patch.indentStep * 100;
    // 한글 이름 그대로 오른쪽 여백 늘리기(+)는 오른쪽 여백 값을 줄인다(ParagraphShapeIncreaseRightMargin).
    if (patch.marginLeftStep != 0) {
        next.marginLeft = steppedMargin(next.marginLeft, next.marginRight, patch.marginLeftStep, boxWidth);
    }
    if (patch.marginRightStep != 0) {
        next.marginRight = steppedMargin(next.marginRight, next.marginLeft, -patch.marginRightStep, boxWidth);
    }
    if (patch.hasLineSpacingValue || patch.lineSpacingStep != 0) {
        // 문단의 간격 종류 단위로 읽는다. 퍼센트는 그대로, 고정·최소·여백은 pt 다.
        const bool percent = next.lineSpacingType.empty() || next.lineSpacingType == "PERCENT";
        const std::int32_t unit = percent ? 10 : 100;
        std::int32_t value = next.lineSpacing >= 0 ? next.lineSpacing : (percent ? 160 : 0);
        if (patch.hasLineSpacingValue) {
            value = percent ? static_cast<std::int32_t>(std::llround(patch.lineSpacingValue))
                            : chwp::ptToHu(patch.lineSpacingValue);
        }
        if (patch.lineSpacingStep != 0) {
            // 한글 2024: 퍼센트는 10% 씩 0~500%, 아니면 1pt 씩 0 이상이다.
            value += patch.lineSpacingStep * unit;
            next.lineSpacing = percent ? std::clamp(value, 0, 500) : std::max(0, value);
        } else {
            next.lineSpacing = std::max(unit, value);
        }
    }
    applyParaFields(next, patch.fields);
    applyParaFillFields(doc, next, patch.fields);
    applyParaHeadingFields(doc, next, patch.fields);
    applyParaTabFields(doc, next, patch.fields);
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
    if (patch.normalShape) {
        // 한글 2024 CharShapeNormal 이 지우는 항목: 진하게·기울임·양각·음각·위/아래 첨자·
        // 밑줄·외곽선·그림자·취소선·강조점을 끄고, 글자 색 검정, 음영 없음, 밑줄·취소선 실선 검정,
        // 그림자 10%·10%·#B2B2B2 로 둔다. 글꼴·크기·장평·자간·위치·커닝·빈칸·테두리는 둔다.
        next.bold = false;
        next.italic = false;
        next.emboss = false;
        next.engrave = false;
        next.superscript = false;
        next.subscript = false;
        next.underline = "NONE";
        next.outline = 0;
        next.shadow = 0;
        next.textColor = 0u;
        next.shadeColor.reset();
        next.underlineShape = "SOLID";
        next.underlineShapeId = 0;
        next.underlineColor = 0u;
        next.shadowOffsetX = 10;
        next.shadowOffsetY = 10;
        next.shadowColor = 0xB2B2B2u;
        next.strikeout = false;
        next.emphasis = 0;
        next.strikeShape = "SOLID";
        next.strikeShapeId = 0;
        next.strikeColor = 0u;
    }
    if (patch.hasFont && !patch.font.empty()) {
        ensureFontInAllSlots(doc, patch.font, next.fontRef);
    }
    applyCharLangFonts(doc, next, patch.fields);
    if (patch.hasFontSize) {
        const std::int32_t size = patch.fontSizeValueSet
                                      ? chwp::lengthToHu(patch.fontSizeValue, chwp::lengthUnitOf(patch.fontSizeUnit))
                                      : patch.fontSize;
        if (size > 0) next.height = std::clamp<std::int32_t>(size, 100, 50000);
    }
    if (patch.fontSizeStep != 0) {
        // 한글 2024 CharShapeHeightIncrease·Decrease: 1pt 씩 1pt~4096pt.
        next.height = std::clamp<std::int32_t>(next.height + patch.fontSizeStep * 100, 100, 409600);
    }
    // 언어마다 따로 한 단계씩 옮긴다. 범위는 글자 모양 대화 상자와 같다(자간 -50~50%, 장평 50~200%).
    for (std::size_t lang = 0; lang < next.spacing.size(); ++lang) {
        if (patch.spacingStep != 0) next.spacing[lang] = std::clamp(next.spacing[lang] + patch.spacingStep, -50, 50);
        if (patch.ratioStep != 0) next.ratio[lang] = std::clamp(next.ratio[lang] + patch.ratioStep, 50, 200);
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
    applyCharFillFields(doc, next, patch.fields);
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
    leftPara.id = cur.id;  // 앞 문단이 번호를 지킨다. 뒤 문단은 재조판 앞에서 새 번호를 받는다
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
        if (lineStartsWithObject(lineNode)) {
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
        under.pageW = viewWidthOf(page);
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
                lineStartsWithObject(node);
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
        prevLineBreak = node.lineData().lineBreak;
        prevMaxOff = maxOff;
    }
    return byOffset;
}

void logEditLayout(const char* kind, int pageBegin, int pageEnd, int paragraph, int lineBegin, int lineEnd,
                   int reusePage, const char* why, double ms) {
    std::fprintf(stderr, "[ClassingHWP] 조판 %s 쪽 %d-%d 문단 %d", kind, pageBegin, pageEnd, paragraph);
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

namespace {
EditRelayoutResult relayoutEditedPages(model::Document& doc, std::vector<Node>& pages, const EditRelayoutSpec& spec);
}  // namespace

// 편집 뒤 다시 놓기. 줄 번호는 앞쪽 줄 수에 따라 뒤쪽 번호가 모두 바뀌니, 다시 놓은 뒤 처음부터 다시 세고
// 번호가 바뀐 쪽을 다시 그린다(numberPageLines 는 줄 번호를 보이는 구역이 없으면 바로 돌아간다).
EditRelayoutResult relayoutAfterEdit(model::Document& doc, std::vector<Node>& pages, const EditRelayoutSpec& spec) {
    EditRelayoutResult result = relayoutEditedPages(doc, pages, spec);
    std::vector<int> renumbered;
    numberPageLines(doc, pages, 0, &renumbered);
    placeMemos(doc, pages, &renumbered);
    if (!result.fullInvalidation) {
        for (const int page : renumbered) result.invalidations.push_back(FrameInvalidation{page, {}, -1, -1, false});
    }
    return result;
}

namespace {

EditRelayoutResult relayoutEditedPages(model::Document& doc, std::vector<Node>& pages, const EditRelayoutSpec& spec) {
    const auto started = std::chrono::steady_clock::now();
    model::ensureParagraphIds(doc);
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
    // 바탕쪽 글(그 안 표·글상자 포함)은 그 바탕쪽을 쓰는 모든 쪽에 찍힌다. 문서 전체를 다시 놓는다.
    bool masterText = false;
    for (const model::StorySlot* slot = resolveStory(doc, spec.story); slot;
         slot = slot->parentStory > 0 ? resolveStory(doc, slot->parentStory) : nullptr) {
        if (slot->kind == model::StoryKind::MasterPage) {
            masterText = true;
            break;
        }
    }
    // 표·그림·수식 번호가 든 캡션은 번호를 글로 바꾼 사본으로 놓는다(scene attachCaption). 그 문단만 다시 놓으면
    // 번호가 빠지니 개체를 품은 문단째 다시 놓는다.
    bool numberedCaption = false;
    // 각주·미주도 쪽마다 번호를 바꾼 사본으로 놓는다(scene noteArea). 그 문단만 다시 놓으면 번호가 빠진다.
    if (const model::StorySlot* slot = resolveStory(doc, spec.story);
        slot && slot->control &&
        (slot->control->kind == model::ControlKind::Footnote || slot->control->kind == model::ControlKind::Endnote)) {
        numberedCaption = true;
    }
    if (const model::StorySlot* slot = resolveStory(doc, spec.story); slot && slot->kind == model::StoryKind::Caption) {
        for (const model::Paragraph& para : *slot->paragraphs) {
            for (const model::Run& run : para.runs) {
                for (const model::InlineItem& item : run.items) {
                    numberedCaption = numberedCaption ||
                                      (item.type == model::InlineItem::Type::Control &&
                                       item.control.kind == model::ControlKind::AutoNumber &&
                                       (item.control.numType == "TABLE" || item.control.numType == "PICTURE" ||
                                        item.control.numType == "EQUATION"));
                }
            }
        }
    }
    EditRelayoutResult inc;
    if (masterText) {
        inc.widenReason = "바탕쪽";
    } else if (numberedCaption) {
        inc.widenReason = "캡션 번호";
    } else if (band) {
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
    specWithHint.paragraphsShifted = inc.paragraphsShifted;
    const SectionReflow span = band || masterText ? SectionReflow{} : reflowEditedPages(doc, pages, specWithHint);
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
    model::ensureParagraphIds(doc);
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

}  // namespace

EditResult makeEditResult(const model::Document& doc, bool handled, bool changed, int story, int paraIdx,
                          const EditRelayoutResult* relayout) {
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

// 캐럿 글자 번호마다의 글자. 글 조각은 그 글자, 글자처럼 놓인 개체는 0x0B, 각주·미주는 0x11 이다
// (한글 문단 글의 컨트롤 문자와 같다).
std::u32string caretCharsOf(const model::Paragraph& para) {
    std::u32string out;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                // UTF-8 을 코드 포인트로. 이어지는 바이트(10xxxxxx)는 앞 글자에 붙인다.
                for (const unsigned char c : item.text) {
                    if ((c & 0xC0) == 0x80 && !out.empty()) {
                        out.back() = (out.back() << 6) | (c & 0x3F);
                    } else if (c >= 0xF0) {
                        out.push_back(c & 0x07);
                    } else if (c >= 0xE0) {
                        out.push_back(c & 0x0F);
                    } else if (c >= 0xC0) {
                        out.push_back(c & 0x1F);
                    } else {
                        out.push_back(c);
                    }
                }
            } else if (isCaretCharControl(item.control)) {
                const bool note = item.control.kind == model::ControlKind::Footnote ||
                                  item.control.kind == model::ControlKind::Endnote;
                out.push_back(note ? U'\x11' : U'\x0B');
            }
        }
    }
    return out;
}

}  // namespace paint_detail

namespace {

std::atomic<bool> gReadOnly{false};
std::atomic<bool> gOverwrite{false};

// 캐럿 뒤로 바로 이어지는 글자 수. 개체·조판 부호나 문단 끝에서 멈춘다. 수정 모드가 바꿀 수 있는 글자다.
int textCharsAfter(const model::Paragraph& para, int charIndex) {
    int pos = 0;
    int count = 0;
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                const int len = utf8CodePointCount(item.text);
                const int from = std::max(pos, charIndex);
                if (pos + len > from) count += pos + len - from;
                pos += len;
            } else if (isCaretCharControl(item.control)) {
                if (pos >= charIndex) return count;
                pos += 1;
            }
        }
    }
    return count;
}

// 읽기 전용에서도 받는 키. 커서만 옮기고 문서는 고치지 않는다.
bool movesOnly(EditKey key) {
    return key == EditKey::ArrowLeft || key == EditKey::ArrowRight || key == EditKey::ArrowUp ||
           key == EditKey::ArrowDown || key == EditKey::Home || key == EditKey::End || key == EditKey::Escape ||
           key == EditKey::SelectAll || key == EditKey::DocBegin || key == EditKey::DocEnd;
}

// 한글 2024 단어 이동의 글자 나누기: 0x21 아래 글자와 U+3000 은 띄움이다.
// 다만 컨트롤 문자 3·4(필드)·0x11(주석)·0x12(자동 번호)·0x18(하이픈)은 글자다.
bool wordChar(char32_t c) {
    if (c == 3 || c == 4 || c == 0x11 || c == 0x12 || c == 0x18) return true;
    return !(c < 0x21 || c == 0x3000);
}
bool lineStopChar(char32_t c) { return c == U'\t' || c == U'\n' || c == U'\r'; }

// 다음 단어 앞(MoveNextWord). 문단 끝이면 다음 문단 처음이다. 단어 바로 뒤가 탭·줄바꿈이면 그 앞에 선다.
// 한글 2024 에서 확인했다(tmp/word_probe: "abc\tdef" 는 3, 4, "one\ntwo" 는 3, 4).
model::CursorPos nextWordPos(const std::vector<model::Paragraph>& paras, int story, int para, int charIndex) {
    const std::u32string chars = caretCharsOf(paras[static_cast<std::size_t>(para)]);
    const int len = static_cast<int>(chars.size());
    if (charIndex >= len) {
        if (para + 1 < static_cast<int>(paras.size())) return {story, para + 1, 0};
        return {story, para, len};
    }
    int i = charIndex;
    while (i < len && wordChar(chars[static_cast<std::size_t>(i)])) ++i;
    if (i > charIndex && i < len && lineStopChar(chars[static_cast<std::size_t>(i)])) return {story, para, i};
    while (i < len && !wordChar(chars[static_cast<std::size_t>(i)])) ++i;
    return {story, para, i};
}

// 앞 단어 처음(MovePrevWord). 문단 처음이면 앞 문단 끝이다. 거슬러 가며 탭·줄바꿈을 넘으면 그 앞에 서고,
// 문단 처음까지 띄움뿐이면 앞 문단 끝으로 간다(한글 2024: "  lead" 의 lead 에서 앞 문단 끝).
model::CursorPos prevWordPos(const std::vector<model::Paragraph>& paras, int story, int para, int charIndex) {
    const auto prevParaEnd = [&]() -> model::CursorPos {
        if (para <= 0) return {story, para, 0};
        return {story, para - 1,
                static_cast<int>(caretCharsOf(paras[static_cast<std::size_t>(para - 1)]).size())};
    };
    if (charIndex <= 0) return prevParaEnd();
    const std::u32string chars = caretCharsOf(paras[static_cast<std::size_t>(para)]);
    int i = std::min<int>(charIndex, static_cast<int>(chars.size()));
    while (i > 0 && !wordChar(chars[static_cast<std::size_t>(i - 1)])) {
        --i;
        if (lineStopChar(chars[static_cast<std::size_t>(i)])) return {story, para, i};
    }
    if (i == 0) return prevParaEnd();
    while (i > 0 && wordChar(chars[static_cast<std::size_t>(i - 1)])) --i;
    return {story, para, i};
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

void setOverwrite(bool on) { gOverwrite.store(on, std::memory_order_relaxed); }

bool overwrite() { return gOverwrite.load(std::memory_order_relaxed); }

EditResult insertTextNow(model::Document& doc, std::vector<Node>& pages, const std::string& text, bool composing,
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
            // 수정 모드는 친 글자 수만큼 뒤 글자를 바꾼다. 문단을 나누는 글은 바꾸지 않고 넣는다.
            if (overwrite() && text.find_first_of("\r\n") == std::string::npos) {
                model::Paragraph& para = (*paras)[static_cast<std::size_t>(paraIdx)];
                const int replaced = std::min(utf8CodePointCount(text), textCharsAfter(para, charIndex));
                if (replaced > 0) {
                    eraseCharsInParagraph(para, charIndex, charIndex + replaced);
                    editOldEndOff = charIndex + replaced;
                }
            }
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

namespace {

// 바꿀 수 있는 찾은 글인가. 개체(U+FFFC)를 품은 글은 글로 바꾸지 않는다.
bool replaceableHit(const std::string& found) { return found.find("\xEF\xBF\xBC") == std::string::npos; }

bool hasLineEnd(const std::string& text) { return text.find_first_of("\r\n") != std::string::npos; }

// 문단 글자 [from, to) 의 찾기 글.
std::string searchSlice(const model::Paragraph& para, int from, int to) {
    const std::string body = paragraphSearchText(para);
    std::size_t lo = std::string::npos;
    std::size_t hi = body.size();
    int index = 0;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        if (i < body.size() && (static_cast<unsigned char>(body[i]) & 0xC0) == 0x80) continue;
        if (index == from && lo == std::string::npos) lo = i;
        if (index == to) {
            hi = i;
            break;
        }
        ++index;
    }
    if (lo == std::string::npos || hi < lo) return {};
    return body.substr(lo, hi - lo);
}

// 개체 안 글을 고치면 그 개체를 품은 바깥 문단들의 lineSeg 도 낡는다.
void dropParentLineSegs(model::Document& doc, int story) {
    for (const model::StorySlot* slot = resolveStory(doc, story);
         slot && slot->parentStory > 0 && slot->parentParagraph >= 0; slot = resolveStory(doc, slot->parentStory)) {
        if (model::Paragraph* parent = storyParagraph(doc, slot->parentStory, slot->parentParagraph)) {
            parent->lineSegs.clear();
        }
    }
}

}  // namespace

EditResult replaceTextNow(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                          const std::string& replacement, bool matchCase, bool useStoredLineHeight) {
    if (readOnly() || text.empty() || hasLineEnd(replacement)) return {};
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0 || !cur.hasSelection || cur.startParagraph != cur.endParagraph) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.startParagraph < 0 || cur.startParagraph >= static_cast<int>(paras->size())) return {};
    const int story = cur.story;
    const int paraIdx = cur.startParagraph;
    const int from = offsetToCharIndex(doc, pages, story, paraIdx, cur.startOffset);
    const int to = offsetToCharIndex(doc, pages, story, paraIdx, cur.endOffset);
    model::Paragraph& para = (*paras)[static_cast<std::size_t>(paraIdx)];
    const std::string found = searchSlice(para, from, to);
    if (!replaceableHit(found) || !searchTextEqual(found, text, matchCase)) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    clearPendingCharShape(hold);
    hold.preferredX = -1;
    const int end = replaceCharsInParagraph(para, from, to, replacement);
    const EditRelayoutResult relayout = relayoutAfterEdit(
        doc, pages, EditRelayoutSpec{story, paraIdx, 0, from, to, end, useStoredLineHeight});
    setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, charIndexToOffset(doc, pages, story, paraIdx, end)});
    return makeEditResult(doc, true, true, story, paraIdx, &relayout);
}

EditResult replaceAllTextNow(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                             const std::string& replacement, bool matchCase, bool useStoredLineHeight, int& count) {
    count = 0;
    if (readOnly() || text.empty() || hasLineEnd(replacement)) return {};
    const std::vector<TextHit> hits = findText(doc, text, matchCase);
    if (hits.empty()) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    model::StoryMap& map = ensureStoryMap(doc);
    // 찾은 곳은 story·문단·글자 차례다. 뒤에서부터 바꾸면 앞 찾은 곳의 글자 번호가 그대로다.
    for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
        std::vector<model::Paragraph>* paras = map.paragraphs(it->story);
        if (!paras || it->paragraph >= static_cast<int>(paras->size())) continue;
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(it->paragraph)];
        if (!replaceableHit(searchSlice(para, it->from, it->to))) continue;
        replaceCharsInParagraph(para, it->from, it->to, replacement);
        dropParentLineSegs(doc, it->story);
        ++count;
    }
    if (count == 0) return {};
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    clearPendingCharShape(hold);
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    // 캐럿은 그 문단에 둔다. 글자 수가 바뀌었을 수 있으니 문단 끝을 넘지 않게 한다.
    const PointerCursor cur = pointerCursor();
    if (cur.active && cur.story > 0) {
        int index = 0;
        if (const std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
            paras && cur.paragraph >= 0 && cur.paragraph < static_cast<int>(paras->size())) {
            index = paragraphCharCount((*paras)[static_cast<std::size_t>(cur.paragraph)]);
        }
        const int caretIndex = std::min(index, offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset));
        setPointerCursor(doc, pages,
                         model::CursorPos{cur.story, cur.paragraph,
                                          charIndexToOffset(doc, pages, cur.story, cur.paragraph, caretIndex)});
    }
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &full);
}

namespace paint_detail {
bool editBlockedByLicenseFonts(const model::Document& doc, EditResult& out) { return blockedByLicenseFonts(doc, out); }
void dropParentLineSegsOf(model::Document& doc, int story) { dropParentLineSegs(doc, story); }
}  // namespace paint_detail

EditResult swapLicenseFontsNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
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
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    const PointerCursor cur = pointerCursor();
    if (cur.active && cur.story > 0) {
        setPointerCursor(doc, pages, model::CursorPos{cur.story, cur.paragraph, cur.offset});
    }
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    // 글꼴을 묻느라 넣지 못한 개체(표·그림·그린 도형)가 있으면 이제 넣는다.
    // 글꼴을 바꾸며 문서 전체를 다시 놓았으니, 이어 넣은 결과도 전체를 다시 그리게 한다.
    // 넣기의 부분 결과만 넘기면 셸·일꾼이 바꾸기 전 쪽 캐시를 그대로 쓴다.
    if (EditResult resumed = resumePendingInsert(doc, pages, useStoredLineHeight); resumed.documentChanged) {
        resumed.fullInvalidation = true;
        resumed.pageListChanged = true;
        resumed.pagesMoved = false;
        resumed.invalidations.clear();
        return resumed;
    }
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &full);
}

bool compositionActive() { return pointerHold().composing; }

EditResult editCommitComposition(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    const int story = hold.story;
    const int paraIdx = hold.focusPara;
    const bool wasComposing = hold.composing;
    clearComposition(hold);
    if (wasComposing) bumpPointerPicture();
    EditResult result = makeEditResult(doc, wasComposing, false, story, paraIdx);
    if (!wasComposing || paraIdx < 0) return result;
    EditResult committed = result;
    committed.documentChanged = true;
    EditChange change;
    change.story = story;
    change.first = paraIdx;
    change.count = 1;
    bool more = false;
    EditResult out = afterEditCommitted(doc, pages, committed, change, useStoredLineHeight, &more);
    return more ? out : result;
}

namespace {

// 방향키로 표 칸 사이를 옮긴다(한글 2024 MoveLeft·MoveRight·MoveUp·MoveDown 이 칸 끝에서 하는 일).
//   → 칸 글 끝: 오른쪽 이웃 칸 맨 앞. 줄 끝 칸이면 다음 줄 첫 칸. 표 끝이면 표 바로 뒤.
//   ← 칸 글 맨 앞: 왼쪽 이웃 칸 끝. 줄 첫 칸이면 앞 줄 마지막 칸. 표 맨 앞이면 표 바로 앞.
//   ↓·↑ 칸 마지막·첫 줄: 아래·위 줄에서 캐럿 가로 자리 밑의 칸으로(첫·마지막 줄의 같은 가로 자리).
//       표 마지막·첫 줄이면 표를 담은 글의 다음·앞 줄로.
// 이웃은 칸 그림(합친 칸 포함)으로 고르고, 가로 자리는 그린 칸 상자로 고른다.

int cellStoryAt(const model::Document& doc, const model::Control* table, int cell) {
    const model::StoryMap& map = ensureStoryMap(doc);
    for (int s = 1; s <= map.count(); ++s) {
        const model::StorySlot* slot = map.slot(s);
        if (slot && slot->control == table && slot->tableCell && slot->subIndex == cell && map.storyOf(slot->paragraphs) == s) {
            return s;
        }
    }
    return -1;
}

// 이 표의 그린 칸 상자. 캐럿 쪽과 앞뒤 쪽에서 찾는다(표가 쪽을 넘을 수 있다).
std::vector<std::pair<int, LayoutBox>> drawnCells(const model::Document& doc, std::vector<Node>& pages,
                                                  const model::Control* table, int nearPage) {
    std::vector<std::pair<int, LayoutBox>> out;
    const int first = std::max(0, nearPage - 1);
    const int last = std::min(static_cast<int>(pages.size()) - 1, nearPage + 1);
    for (int p = first; p <= last; ++p) {
        for (const LayoutBox& box : describePage(doc, pages, p).boxes) {
            if (box.kind != "cell" || box.story <= 0) continue;
            const model::StorySlot* slot = resolveStory(doc, box.story);
            if (slot && slot->control == table) out.emplace_back(p, box);
        }
    }
    return out;
}

// 글 묶음의 첫(또는 마지막) 줄에서 가로 자리 x 에 캐럿을 둔다.
bool placeOnStoryEdgeLine(const model::Document& doc, const std::vector<Node>& pages, PointerHold& hold, int story,
                          bool firstLine, std::int32_t x) {
    const std::vector<StoryLineOnPage> lines = gatherAllStoryLines(doc, pages, story);
    if (lines.empty()) return false;
    const StoryLineOnPage& line = firstLine ? lines.front() : lines.back();
    Painter painter;
    painter.doc = &doc;
    CaretPlace place = painter.placeOnLine(line.line, x, false, -1, -1);
    if (!place.hit) return false;
    place.page = line.page;
    // 다른 글 묶음으로 옮긴다. 줄이 든 글 묶음이 캐럿의 글 묶음이다.
    hold.story = line.line.node->story;
    hold.focusPara = place.paragraph;
    hold.focusOff = place.offset;
    hold.anchorPara = place.paragraph;
    hold.anchorOff = place.offset;
    placeCaret(hold, place, line.page, pages, &doc);
    rememberSelection(doc, pages);
    return true;
}

// 표를 담은 글에서 그 표 개체가 박힌 글자 순번.
int tableCharIndex(const model::Paragraph& host, const model::Control* table) {
    int at = 0;
    for (const model::Run& run : host.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control && &item.control == table) return at;
            if (item.type == model::InlineItem::Type::Text) at += static_cast<int>(utf8CodePointCount(item.text));
            else if (isCaretCharControl(item.control)) ++at;
        }
    }
    return at;
}

// 표 밖으로 나간다. after 면 표 뒤, 아니면 표 앞. vertical 이면 표가 든 줄의 다음(앞) 줄의 가로 자리 x 로.
bool leaveTable(const model::Document& doc, const std::vector<Node>& pages, PointerHold& hold,
                const model::StorySlot& slot, bool after, bool vertical, std::int32_t x) {
    const int hostStory = slot.parentStory;
    const int hostPara = slot.parentParagraph;
    const model::Paragraph* host = storyParagraph(doc, hostStory, hostPara);
    if (!host) return false;
    const int at = tableCharIndex(*host, slot.control);
    const bool takesChar = slot.control && isCaretCharControl(*slot.control);
    if (!vertical) {
        const int index = after && takesChar ? at + 1 : at;
        setPointerCursor(doc, pages, model::CursorPos{hostStory, hostPara, charIndexToOffset(doc, pages, hostStory, hostPara, index)});
        return pointerCursor().active;
    }
    const std::vector<StoryLineOnPage> lines = gatherAllStoryLines(doc, pages, hostStory);
    const int offset = charIndexToOffset(doc, pages, hostStory, hostPara, at);
    int tableLine = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        const Node& node = *lines[static_cast<std::size_t>(i)].line.node;
        if (node.paragraph == hostPara && node.start <= offset && offset < std::max(node.end, node.start + 1)) {
            tableLine = i;
            if (!after) break;
        }
    }
    if (tableLine < 0) return false;
    const int target = after ? tableLine + 1 : tableLine - 1;
    if (target < 0 || target >= static_cast<int>(lines.size())) return false;
    const StoryLineOnPage& line = lines[static_cast<std::size_t>(target)];
    Painter painter;
    painter.doc = &doc;
    CaretPlace place = painter.placeOnLine(line.line, x, false, -1, -1);
    if (!place.hit) return false;
    place.page = line.page;
    // 다른 글 묶음으로 옮긴다. 줄이 든 글 묶음이 캐럿의 글 묶음이다.
    hold.story = line.line.node->story;
    hold.focusPara = place.paragraph;
    hold.focusOff = place.offset;
    hold.anchorPara = place.paragraph;
    hold.anchorOff = place.offset;
    placeCaret(hold, place, line.page, pages, &doc);
    rememberSelection(doc, pages);
    return true;
}

// 캐럿이 든 칸에서 key 방향 이웃 칸(또는 표 밖)으로 옮긴다. 옮겼으면 참.
bool moveAcrossCells(model::Document& doc, std::vector<Node>& pages, PointerHold& hold, int story, EditKey key) {
    const model::StorySlot* slot = resolveStory(doc, story);
    if (!slot || !slot->tableCell || !slot->control || !slot->control->table || slot->subIndex < 0) return false;
    const model::Control* control = slot->control;
    const model::Table& table = *control->table;
    const model::TableGrid grid = model::tableGrid(table);
    if (!model::tableGridValid(table) || slot->subIndex >= static_cast<int>(table.cells.size())) return false;
    const model::TableCell& cell = table.cells[static_cast<std::size_t>(slot->subIndex)];
    const int top = static_cast<int>(cell.row);
    const int bottom = top + std::max(1, static_cast<int>(cell.rowSpan)) - 1;
    const int left = static_cast<int>(cell.col);
    const int right = left + std::max(1, static_cast<int>(cell.colSpan)) - 1;

    if (key == EditKey::ArrowLeft || key == EditKey::ArrowRight) {
        hold.preferredX = -1;
        int row = hold.cellRowTable == control && hold.cellRow >= top && hold.cellRow <= bottom ? hold.cellRow : top;
        int col = key == EditKey::ArrowRight ? right + 1 : left - 1;
        if (col >= grid.cols) {
            ++row;
            col = 0;
        } else if (col < 0) {
            --row;
            col = grid.cols - 1;
        }
        if (row < 0 || row >= grid.rows) {
            hold.cellRowTable = nullptr;
            return leaveTable(doc, pages, hold, *slot, key == EditKey::ArrowRight, false, 0);
        }
        const int target = grid.at(row, col);
        const int targetStory = target >= 0 ? cellStoryAt(doc, control, target) : -1;
        if (targetStory <= 0) return false;
        hold.cellRowTable = control;
        hold.cellRow = row;
        if (key == EditKey::ArrowRight) {
            setPointerCursor(doc, pages, model::CursorPos{targetStory, 0, 0});
        } else {
            const std::vector<model::Paragraph>* paras = storyParagraphs(doc, targetStory);
            if (!paras || paras->empty()) return false;
            const int last = static_cast<int>(paras->size()) - 1;
            const int length = paragraphCharCount((*paras)[static_cast<std::size_t>(last)]);
            setPointerCursor(doc, pages,
                             model::CursorPos{targetStory, last, charIndexToOffset(doc, pages, targetStory, last, length)});
        }
        return pointerCursor().active;
    }

    const bool down = key == EditKey::ArrowDown;
    hold.cellRowTable = nullptr;
    if (hold.preferredX < 0) hold.preferredX = hold.caret.turned ? hold.caret.y : hold.caret.x;
    const std::int32_t x = hold.preferredX;
    const int row = down ? bottom + 1 : top - 1;
    if (row < 0 || row >= grid.rows) return leaveTable(doc, pages, hold, *slot, down, true, x);
    // 그 줄을 덮는 칸들 가운데 가로 자리 x 밑의 칸. 그린 상자가 없으면 지금 칸의 왼쪽 칸 줄을 쓴다.
    const std::vector<std::pair<int, LayoutBox>> boxes = drawnCells(doc, pages, control, hold.caretPage);
    int best = -1;
    std::int64_t bestGap = -1;
    for (int c = 0; c < grid.cols; ++c) {
        const int owner = grid.at(row, c);
        if (owner < 0) continue;
        const int ownerStory = cellStoryAt(doc, control, owner);
        for (const auto& [page, box] : boxes) {
            if (box.story != ownerStory) continue;
            const std::int64_t gap = x < box.x ? box.x - x : (x >= box.x + box.width ? x - (box.x + box.width) + 1 : 0);
            if (bestGap < 0 || gap < bestGap) {
                bestGap = gap;
                best = owner;
            }
        }
    }
    if (best < 0) best = grid.at(row, left);
    const int targetStory = best >= 0 ? cellStoryAt(doc, control, best) : -1;
    if (targetStory <= 0) return false;
    return placeOnStoryEdgeLine(doc, pages, hold, targetStory, down, x);
}

// 줄을 ↓·↑ 로 옮길 때, 다음 줄이 표가 든 줄이고 가로 자리 x 가 그 표 위면 표 첫(마지막) 줄 칸으로 들어간다.
bool enterTableOnLine(model::Document& doc, std::vector<Node>& pages, PointerHold& hold, const StoryLineOnPage& line,
                      bool down, std::int32_t x) {
    const Node& node = *line.line.node;
    std::vector<LayoutBox> cells;
    for (const LayoutBox& box : describePage(doc, pages, line.page).boxes) {
        if (box.kind != "cell" || box.story <= 0) continue;
        const model::StorySlot* slot = resolveStory(doc, box.story);
        if (!slot || slot->parentStory != node.story || slot->parentParagraph != node.paragraph) continue;
        // 그 줄에 놓인 표만: 칸 상자가 줄 상자와 세로로 겹친다.
        if (box.y + box.height <= line.line.y || box.y >= line.line.y + std::max(node.height, 1)) continue;
        cells.push_back(box);
    }
    if (cells.empty()) return false;
    int edgeRow = down ? INT32_MAX : -1;
    for (const LayoutBox& box : cells) {
        edgeRow = down ? std::min(edgeRow, box.row) : std::max(edgeRow, box.row + box.rowSpan - 1);
    }
    const LayoutBox* best = nullptr;
    std::int64_t bestGap = -1;
    for (const LayoutBox& box : cells) {
        const bool onEdge = down ? box.row == edgeRow : box.row + box.rowSpan - 1 == edgeRow;
        if (!onEdge) continue;
        const std::int64_t gap = x < box.x ? box.x - x : (x >= box.x + box.width ? x - (box.x + box.width) + 1 : 0);
        if (bestGap < 0 || gap < bestGap) {
            bestGap = gap;
            best = &box;
        }
    }
    // 줄에 표만 있으면(한글처럼) 가장 가까운 칸으로 들어간다. 표 옆에 글자가 있으면 표 위일 때만 들어간다.
    const bool tableOnly = node.end - node.start <= 1;
    if (!best || (bestGap > 0 && !tableOnly)) return false;
    hold.cellRowTable = nullptr;
    return placeOnStoryEdgeLine(doc, pages, hold, best->story, down, x);
}

}  // namespace

EditResult keyDownNow(model::Document& doc, std::vector<Node>& pages, EditKey key, bool shift, bool ctrl, bool alt,
                      bool useStoredLineHeight) {
    // 삽입·수정 바꾸기는 문서를 고치지 않는다. 읽기 전용이어도 바꿔 둔다.
    if (key == EditKey::Insert) {
        setOverwrite(!overwrite());
        const PointerHold& hold = pointerHold();
        return makeEditResult(doc, true, false, hold.story, hold.focusPara);
    }
    if (readOnly() && !movesOnly(key)) return {};
    if (EditResult blocked; !movesOnly(key) && blockedByLicenseFonts(doc, blocked)) return blocked;
    PointerHold& hold = pointerHold();
    // 세로쓰기 본문에서는 화살표를 돌린다. ↓↑ 는 줄 안 다음·앞 글자, ←→ 는 다음(왼쪽)·앞(오른쪽) 줄이다.
    if (hold.caret.hit && hold.caret.turned) {
        if (key == EditKey::ArrowDown) key = EditKey::ArrowRight;
        else if (key == EditKey::ArrowUp) key = EditKey::ArrowLeft;
        else if (key == EditKey::ArrowLeft) key = EditKey::ArrowDown;
        else if (key == EditKey::ArrowRight) key = EditKey::ArrowUp;
    }
    // Esc 는 도형 그리기를 그만둔다.
    if (key == EditKey::Escape && (hold.drawTool != 0 || hold.drawing)) {
        hold.drawTool = 0;
        hold.drawing = false;
        bumpPointerPicture();
        return makeEditResult(doc, true, false, hold.story, hold.focusPara);
    }
    // Ctrl+A 모두 선택. 커서가 든 글 묶음(본문·칸·글상자) 전체를 블록으로 잡는다. 커서가 없으면 첫 구역 본문이다.
    if (key == EditKey::SelectAll) {
        if (hold.composing) clearComposition(hold);
        const PointerCursor cur = pointerCursor();
        const int story = cur.active && cur.story > 0 ? cur.story : 1;
        std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
        if (!paras || paras->empty()) return {};
        const int last = static_cast<int>(paras->size()) - 1;
        const int end =
            charIndexToOffset(doc, pages, story, last, paragraphCharCount((*paras)[static_cast<std::size_t>(last)]));
        setPointerCursor(doc, pages, model::CursorPos{story, 0, 0}, model::CursorPos{story, last, end});
        return makeEditResult(doc, true, false, story, last);
    }
    // Shift+Esc 는 머리말·꼬리말 편집을 끝내고 들어오기 전 본문 자리로 커서를 돌린다.
    if (key == EditKey::Escape) {
        if (hold.bandStory <= 0 || !shift) return {};
        if (hold.editStory == hold.bandStory) hold.editStory = 0;
        hold.bandStory = 0;
        if (hold.bandMaster) setMasterView(false);
        hold.bandMaster = false;
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
        return insertTextNow(doc, pages, "\t", false, useStoredLineHeight);
    }

    if (key == EditKey::Enter) {
        if (hold.composing) {
            clearComposition(hold);
        }
        // Shift+Enter 는 문단을 나누지 않고 같은 문단 안에 줄바꿈 문자('\n')만 삽입한다.
        if (shift) {
            return insertTextNow(doc, pages, "\n", false, useStoredLineHeight);
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

    // 단어·문단·문서 처음과 끝으로 가기. 글자 번호로 정하고 블록은 Shift 일 때만 넓힌다.
    const bool wordMove = ctrl && !alt && (key == EditKey::ArrowLeft || key == EditKey::ArrowRight);
    const bool paraMove = (ctrl && !alt && (key == EditKey::ArrowUp || key == EditKey::ArrowDown)) ||
                          (alt && !ctrl && (key == EditKey::Home || key == EditKey::End));
    if (wordMove || paraMove || key == EditKey::DocBegin || key == EditKey::DocEnd) {
        hold.preferredX = -1;
        clearPendingCharShape(hold);
        const int focusChar = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
        const int paraLen = paragraphCharCount((*paras)[static_cast<std::size_t>(paraIdx)]);
        const int lastPara = static_cast<int>(paras->size()) - 1;
        model::CursorPos target{story, paraIdx, focusChar};
        if (key == EditKey::ArrowRight) {
            target = nextWordPos(*paras, story, paraIdx, focusChar);
        } else if (key == EditKey::ArrowLeft) {
            target = prevWordPos(*paras, story, paraIdx, focusChar);
        } else if (key == EditKey::ArrowUp) {
            // 한글 MovePrevParaBegin: 문단 중간이면 그 문단 처음, 처음이면 앞 문단 처음.
            target = {story, focusChar > 0 || paraIdx == 0 ? paraIdx : paraIdx - 1, 0};
        } else if (key == EditKey::ArrowDown) {
            // 한글 MoveNextParaBegin: 다음 문단 처음. 마지막 문단이면 그 끝.
            target = paraIdx < lastPara ? model::CursorPos{story, paraIdx + 1, 0}
                                        : model::CursorPos{story, paraIdx, paraLen};
        } else if (key == EditKey::Home) {
            target = {story, paraIdx, 0};
        } else if (key == EditKey::End) {
            target = {story, paraIdx, paraLen};
        } else {
            // 한글 MoveTopLevelBegin·End: 본문(첫 구역 처음·마지막 구역 끝). 칸·글상자 안에서도 본문으로 간다.
            if (doc.sections.empty()) return makeEditResult(doc, false, false, story, paraIdx);
            const bool begin = key == EditKey::DocBegin;
            const model::Section& sec = begin ? doc.sections.front() : doc.sections.back();
            const int bodyStory = ensureStoryMap(doc).storyOf(&sec.paragraphs);
            if (bodyStory <= 0 || sec.paragraphs.empty()) return makeEditResult(doc, false, false, story, paraIdx);
            const int lastBody = static_cast<int>(sec.paragraphs.size()) - 1;
            target = begin ? model::CursorPos{bodyStory, 0, 0}
                           : model::CursorPos{bodyStory, lastBody,
                                              paragraphCharCount(sec.paragraphs[static_cast<std::size_t>(lastBody)])};
        }
        const model::CursorPos focusPos{target.story, target.paragraph,
                                        charIndexToOffset(doc, pages, target.story, target.paragraph, target.offset)};
        const bool keepAnchor = shift && target.story == story;
        const model::CursorPos anchorPos =
            keepAnchor ? model::CursorPos{story, cur.anchorParagraph, cur.anchorOffset} : focusPos;
        setPointerCursor(doc, pages, anchorPos, focusPos);
        return makeEditResult(doc, true, false, target.story, target.paragraph);
    }

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
            if (!shift && moveAcrossCells(doc, pages, hold, story, key)) {
                const PointerCursor moved = pointerCursor();
                return makeEditResult(doc, true, false, moved.story, moved.paragraph);
            }
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
            if (!shift && moveAcrossCells(doc, pages, hold, story, key)) {
                const PointerCursor moved = pointerCursor();
                return makeEditResult(doc, true, false, moved.story, moved.paragraph);
            }
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
                // 칸 첫·마지막 줄이면 위·아래 칸(또는 표 밖)으로 간다.
                if (!shift && moveAcrossCells(doc, pages, hold, story, key)) {
                    const PointerCursor moved = pointerCursor();
                    return makeEditResult(doc, true, false, moved.story, moved.paragraph);
                }
                return makeEditResult(doc, false, false, story, paraIdx);
            }
        }
        // 줄 방향 자리. 세로쓰기 커서는 종이 Y 가 줄 방향이다.
        if (hold.preferredX < 0) hold.preferredX = hold.caret.turned ? hold.caret.y : hold.caret.x;
        const StoryLineOnPage& target = all[static_cast<std::size_t>(targetLineIdx)];
        // 다음 줄이 표가 든 줄이면 그 표 칸으로 들어간다(한글처럼 표 위 글에서 ↓ 는 첫 줄 칸으로).
        if (!shift && !hold.caret.turned &&
            enterTableOnLine(doc, pages, hold, target, key == EditKey::ArrowDown, hold.preferredX)) {
            const PointerCursor moved = pointerCursor();
            return makeEditResult(doc, true, false, moved.story, moved.paragraph);
        }
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

namespace {

// 그 수준의 개요 스타일. 문단 모양이 그 수준 개요인 문단 스타일이다(한글 기본 스타일 개요 1~7).
const model::Style* outlineStyleOf(const model::Document& doc, int level) {
    for (const model::Style& st : doc.header.styles) {
        if (!st.type.empty() && st.type != "PARA") continue;
        const model::ParaShape* ps = Painter::findParaShape(doc, st.paraShapeId);
        if (ps && ps->headingType == "OUTLINE" && ps->headingLevel == level) return &st;
    }
    return nullptr;
}

void applyParaStyle(model::Paragraph& para, const model::Style& st) {
    para.styleId = st.id;
    para.paraShapeId = st.paraShapeId;
    for (model::Run& run : para.runs) run.charShapeId = st.charShapeId;
}

std::string headingTypeOf(const model::Document& doc, const model::Paragraph& para) {
    const model::ParaShape* ps = Painter::findParaShape(doc, para.paraShapeId);
    return ps && !ps->headingType.empty() ? ps->headingType : std::string("NONE");
}

// 문단 번호·글머리표·개요 단추와 수준 단계(한글 2024 PutParaNumber·PutBullet·PutOutlineNumber·
// ParaNumberBulletLevelUp/Down). 문단을 고쳤으면 참이다.
bool applyHeadingCommand(model::Document& doc, model::Paragraph& para, const std::string& toggle, bool turnOn,
                         int step) {
    constexpr int kLastLevel = 9;  // 1~10 수준(한글 2024 개요 1~10)
    const model::ParaShape* ps = Painter::findParaShape(doc, para.paraShapeId);
    const std::string type = headingTypeOf(doc, para);
    const int level = ps ? ps->headingLevel : 0;
    const std::uint32_t before = para.paraShapeId;
    const std::uint32_t beforeStyle = para.styleId;
    const auto setHeading = [&](const std::string& kind, int lvl) {
        FormatPatch p;
        p.fields.emplace_back("headingType", kind);
        p.fields.emplace_back("headingLevel", static_cast<double>(lvl));
        para.paraShapeId = deriveParaShapeId(doc, para.paraShapeId, p);
    };
    const auto outline = [&](int lvl) {
        if (const model::Style* st = outlineStyleOf(doc, lvl)) applyParaStyle(para, *st);
        else setHeading("OUTLINE", lvl);
    };
    if (toggle == "NUMBER" || toggle == "BULLET") {
        // 번호와 글머리표 사이를 바꿀 때는 수준을 지킨다.
        if (turnOn) setHeading(toggle, type == "NUMBER" || type == "BULLET" ? level : 0);
        else if (type == toggle) setHeading("NONE", 0);
    } else if (toggle == "OUTLINE") {
        if (turnOn) {
            outline(0);
        } else if (type == "OUTLINE") {
            // 끄면 바탕 스타일(첫 문단 스타일)로 돌아간다.
            if (!doc.header.styles.empty()) applyParaStyle(para, doc.header.styles.front());
            else setHeading("NONE", 0);
        }
    }
    if (step != 0) {
        const std::string now = headingTypeOf(doc, para);
        const model::ParaShape* cur = Painter::findParaShape(doc, para.paraShapeId);
        const int at = cur ? cur->headingLevel : 0;
        const int next = std::clamp(at + step, 0, kLastLevel);
        if (now == "OUTLINE") {
            if (next != at) outline(next);
        } else if (now == "NUMBER" || now == "BULLET") {
            if (next != at) setHeading(now, next);
        } else if (step > 0) {
            outline(0);
        }
    }
    return para.paraShapeId != before || para.styleId != beforeStyle;
}

// 그 문단이 놓인 단의 폭(아래에 있다).
std::int32_t columnWidthAt(const model::Document& doc, const std::vector<Node>& pages, int story, int paragraph);

}  // namespace

// 모양 복사(한글 2024 ShapeCopyPaste, 한글 2024 에서 확인).
//   복사: 캐럿 왼쪽 글자의 글자 모양(경계 AAA|aaa 면 AAA 쪽), 캐럿 문단의 문단 모양·문단 스타일을 기억한다.
//   붙이기: 블록 글자에 글자 모양을, 블록이 걸친 문단에 문단 모양을 준다. 문단 스타일은 스타일 적용과 같다.
//   블록 없이 붙이면 아무것도 바꾸지 않는다. 글자 스타일·셀 모양 복사는 아직 없다.
EditResult copyShapeNow(model::Document& doc, std::vector<Node>& pages, const std::string& action, int type,
                        bool useStoredLineHeight, std::string& why) {
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (action == "copy") {
        if (!cur.active || cur.story <= 0) return {};
        const model::Paragraph* para = storyParagraph(doc, cur.story, cur.focusParagraph >= 0 ? cur.focusParagraph : cur.paragraph);
        if (!para) return {};
        if (type != 0 && type != 1 && type != 2 && type != 4) {
            why = "그 모양은 아직 복사하지 못합니다.";
            return {};
        }
        const int at = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
        hold.copiedShape.valid = true;
        hold.copiedShape.type = type;
        hold.copiedShape.charShape = charShapeAtParagraphChar(*para, at, false);
        hold.copiedShape.paraShape = para->paraShapeId;
        hold.copiedShape.style = para->styleId;
        return makeEditResult(doc, true, false, cur.story, cur.paragraph);
    }
    if (action != "paste") return {};
    if (readOnly()) return {};
    if (!hold.copiedShape.valid) {
        why = "복사한 모양이 없습니다.";
        return {};
    }
    if (!cur.active || cur.story <= 0 || !cur.hasSelection) return makeEditResult(doc, true, false, cur.story, cur.paragraph);
    const PointerHold::CopiedShape copied = hold.copiedShape;
    if (copied.type == 4) {
        FormatPatch patch;
        patch.hasStyleId = true;
        patch.styleId = copied.style;
        return applyFormatNow(doc, pages, patch, useStoredLineHeight);
    }
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras) return {};
    const int startChar = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
    const int endChar = offsetToCharIndex(doc, pages, cur.story, cur.endParagraph, cur.endOffset);
    for (int p = cur.startParagraph; p <= cur.endParagraph && p < static_cast<int>(paras->size()); ++p) {
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
        if (copied.type == 1 || copied.type == 2) para.paraShapeId = copied.paraShape;
        if (copied.type == 0 || copied.type == 2) {
            const int len = paragraphCharCount(para);
            const int a = p == cur.startParagraph ? std::clamp(startChar, 0, len) : 0;
            const int b = p == cur.endParagraph ? std::clamp(endChar, 0, len) : len;
            if (b > a) transformParagraphRangeCharShape(para, a, b, [&](std::uint32_t) { return copied.charShape; });
        }
        para.lineSegs.clear();
    }
    clearComposition(hold);
    hold.preferredX = -1;
    dropParentLineSegs(doc, cur.story);
    model::ensureParagraphIds(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    setPointerCursor(doc, pages, model::CursorPos{cur.story, cur.anchorParagraph, charIndexToOffset(doc, pages, cur.story, cur.anchorParagraph,
                                                                                                     offsetToCharIndex(doc, pages, cur.story, cur.anchorParagraph, cur.anchorOffset))},
                     model::CursorPos{cur.story, cur.focusParagraph, charIndexToOffset(doc, pages, cur.story, cur.focusParagraph,
                                                                                      offsetToCharIndex(doc, pages, cur.story, cur.focusParagraph, cur.focusOffset))});
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    return makeEditResult(doc, true, true, cur.story, cur.focusParagraph, &full);
}

bool copiedShapeReady() { return pointerHold().copiedShape.valid; }

EditResult applyFormatNow(model::Document& doc, std::vector<Node>& pages, const FormatPatch& patchIn,
                          bool useStoredLineHeight) {
    if (readOnly()) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    FormatPatch patch = patchIn;
    if (!patch.styleName.empty()) {
        const model::Style* found = nullptr;
        for (const model::Style& st : doc.header.styles) {
            if (st.name == patch.styleName || st.engName == patch.styleName) {
                found = &st;
                break;
            }
        }
        if (!found) return makeEditResult(doc, false, false, pointerHold().story, pointerHold().focusPara);
        patch.hasStyleId = true;
        patch.styleId = found->id;
    }
    const bool paraFields = formatFieldsTouchPara(patch.fields);
    const bool headingCommand = !patch.headingToggle.empty() || patch.headingLevelStep != 0;
    const bool hasParaPatch =
        patch.hasStyleId || patch.hasAlign || patch.hasLineSpacingType || patch.hasLineSpacing ||
        patch.hasLineSpacingValue || patch.lineSpacingStep != 0 || patch.indentStep != 0 ||
        patch.marginLeftStep != 0 || patch.marginRightStep != 0 || paraFields || headingCommand;
    // 형광펜은 고른 글자에만 칠한다. 블록이 없으면 칠할 글자가 없다.
    if (patch.hasMarkpen && !pointerCursor().hasSelection) {
        return makeEditResult(doc, false, false, pointerHold().story, pointerHold().focusPara);
    }
    const bool hasCharPatch = patch.hasMarkpen || patch.hasFont || patch.hasFontSize || patch.fontSizeStep != 0 ||
                              patch.spacingStep != 0 || patch.ratioStep != 0 || patch.normalShape || patch.hasBold ||
                              patch.hasItalic ||
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
    // 문단 종류·수준이 바뀌면 뒤 문단 번호까지 바뀐다. 그때는 문서 전체를 다시 놓는다.
    // 구역 기본 탭 간격도 그 구역 모든 문단에 걸린다.
    bool headingChanged = false;
    if (std::int32_t defaultTab = 0; sectionDefaultTabOf(patch.fields, defaultTab)) {
        if (const model::StorySlot* slot = resolveStory(doc, cur.story)) {
            const int section = std::max(0, slot->sectionIndex);
            if (section < static_cast<int>(doc.sections.size()) &&
                doc.sections[static_cast<std::size_t>(section)].defaultTab != defaultTab) {
                doc.sections[static_cast<std::size_t>(section)].defaultTab = defaultTab;
                for (model::Paragraph& para : doc.sections[static_cast<std::size_t>(section)].paragraphs) {
                    para.lineSegs.clear();
                }
                headingChanged = true;
            }
        }
    }

    // 켜고 끄기는 블록 첫 문단을 보고 정한다. 이미 그 종류면 모두 끈다.
    const bool headingOn = !patch.headingToggle.empty() &&
                           headingTypeOf(doc, (*paras)[static_cast<std::size_t>(startPara)]) != patch.headingToggle;
    if (hasParaPatch) {
        for (int p = startPara; p <= endPara; ++p) {
            model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
            if (headingCommand &&
                applyHeadingCommand(doc, para, patch.headingToggle, headingOn, patch.headingLevelStep)) {
                headingChanged = true;
            }
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
            if (patch.hasAlign || patch.hasLineSpacingType || patch.hasLineSpacing || patch.hasLineSpacingValue ||
                patch.lineSpacingStep != 0 || patch.indentStep != 0 || patch.marginLeftStep != 0 ||
                patch.marginRightStep != 0 || paraFields) {
                const model::ParaShape* was = Painter::findParaShape(doc, para.paraShapeId);
                const std::string wasHeading = was ? was->headingType + "/" + std::to_string(was->headingIdRef) +
                                                         "/" + std::to_string(was->headingLevel)
                                                   : std::string();
                const std::int32_t boxWidth = patch.marginLeftStep != 0 || patch.marginRightStep != 0
                                                  ? columnWidthAt(doc, pages, story, p)
                                                  : 0;
                para.paraShapeId = deriveParaShapeId(doc, para.paraShapeId, patch, boxWidth);
                const model::ParaShape* now = Painter::findParaShape(doc, para.paraShapeId);
                const std::string nowHeading = now ? now->headingType + "/" + std::to_string(now->headingIdRef) +
                                                         "/" + std::to_string(now->headingLevel)
                                                   : std::string();
                if (wasHeading != nowHeading) headingChanged = true;
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
                    transformParagraphRangeCharShape(
                        para, pStart, pEnd, [&](std::uint32_t oldId) { return deriveCharShapeId(doc, oldId, patch); },
                        patch.hasMarkpen ? &patch.markpen : nullptr);
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
    if (documentChanged && headingChanged) {
        model::ensureParagraphIds(doc);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        // 새 트리는 옛 줄 주소를 다시 쓸 수 있다. 주소로 찾는 줄 자리 캐시를 비운다(다른 전체 조판 편집과 같다).
        invalidateLinePlaces({}, true);
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
        relayoutPtr = &relayout;
    } else if (documentChanged) {
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

namespace {

// 칸 블록이 덮는 칸들. 블록 사각형과 겹치는 칸이다(합친 칸은 걸치기만 해도 든다).
std::vector<const model::TableCell*> blockCells(const DragHost& drag) {
    std::vector<const model::TableCell*> out;
    if (drag.mode != DragMode::Cells || !drag.control || !drag.control->table || drag.focusRow < 0 ||
        drag.focusCol < 0) {
        return out;
    }
    const int row0 = std::min(drag.row, drag.focusRow);
    const int col0 = std::min(drag.col, drag.focusCol);
    const int row1 = std::max(drag.row + drag.rowSpan - 1, drag.focusRow + drag.focusRowSpan - 1);
    const int col1 = std::max(drag.col + drag.colSpan - 1, drag.focusCol + drag.focusColSpan - 1);
    for (const model::TableCell& cell : drag.control->table->cells) {
        const int r0 = static_cast<int>(cell.row);
        const int c0 = static_cast<int>(cell.col);
        const int r1 = r0 + static_cast<int>(std::max<std::uint32_t>(1, cell.rowSpan)) - 1;
        const int c1 = c0 + static_cast<int>(std::max<std::uint32_t>(1, cell.colSpan)) - 1;
        if (r1 < row0 || r0 > row1 || c1 < col0 || c0 > col1) continue;
        out.push_back(&cell);
    }
    return out;
}

}  // namespace

bool cellBlockHost(int& story, int& paragraph) {
    const DragHost& drag = pointerHold().drag;
    if (blockCells(drag).empty() || drag.parentStory <= 0 || drag.parentPara < 0) return false;
    story = drag.parentStory;
    paragraph = drag.parentPara;
    return true;
}

EditResult applyFormatToCellsNow(model::Document& doc, std::vector<Node>& pages, const FormatPatch& patch,
                                 bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    const std::vector<const model::TableCell*> cells = blockCells(hold.drag);
    if (cells.empty()) return applyFormatNow(doc, pages, patch, useStoredLineHeight);
    std::vector<int> stories;
    for (const model::TableCell* cell : cells) {
        const int story = ensureStoryMap(doc).storyOf(&cell->content.paragraphs);
        if (story > 0) stories.push_back(story);
    }
    // 칸마다 그 칸 글 전체를 블록으로 잡아 서식을 건다. 블록 잡기가 끌기 상태를 지우므로 칸 블록은 따로 들고 있다.
    const DragHost drag = hold.drag;
    const int keepStory = hold.story;
    const int keepAnchorPara = hold.anchorPara;
    const int keepAnchorOff = hold.anchorOff;
    const int keepFocusPara = hold.focusPara;
    const int keepFocusOff = hold.focusOff;
    hold.drag = DragHost{};
    EditResult merged;
    for (const int story : stories) {
        const std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
        if (!paras || paras->empty()) continue;
        const int last = static_cast<int>(paras->size()) - 1;
        const int endChar = paragraphCharCount((*paras)[static_cast<std::size_t>(last)]);
        setPointerCursor(doc, pages, model::CursorPos{story, 0, 0},
                         model::CursorPos{story, last, charIndexToOffset(doc, pages, story, last, endChar)});
        EditResult step = applyFormatNow(doc, pages, patch, useStoredLineHeight);
        if (!step.handled) continue;
        if (!merged.handled) {
            merged = step;
            continue;
        }
        merged.documentChanged = merged.documentChanged || step.documentChanged;
        merged.fullInvalidation = merged.fullInvalidation || step.fullInvalidation;
        merged.pageListChanged = merged.pageListChanged || step.pageListChanged;
        if (step.pagesMoved) {
            if (merged.pagesMoved) {
                merged.pageShiftFrom = std::min(merged.pageShiftFrom, step.pageShiftFrom);
                merged.pageShiftDelta += step.pageShiftDelta;
            } else {
                merged.pageShiftFrom = step.pageShiftFrom;
                merged.pageShiftDelta = step.pageShiftDelta;
            }
            merged.pagesMoved = true;
        }
        merged.invalidations.insert(merged.invalidations.end(), step.invalidations.begin(), step.invalidations.end());
    }
    hold.story = keepStory;
    hold.anchorPara = keepAnchorPara;
    hold.anchorOff = keepAnchorOff;
    hold.focusPara = keepFocusPara;
    hold.focusOff = keepFocusOff;
    hold.drag = drag;
    rememberSelection(doc, pages);
    merged.cursor = pointerCursor();
    merged.caret = hold.caret;
    return merged;
}

// 커서가 있는 구역. 커서가 없으면 첫 구역이다.
int cursorSection(const model::Document& doc) {
    const PointerCursor cur = pointerCursor();
    if (cur.active && cur.story > 0) {
        if (const model::StorySlot* slot = resolveStory(doc, cur.story)) {
            if (slot->sectionIndex >= 0 && slot->sectionIndex < static_cast<int>(doc.sections.size())) {
                return slot->sectionIndex;
            }
        }
    }
    return doc.sections.empty() ? -1 : 0;
}

namespace {

// 파일에 쓰는 구역 정의 컨트롤. 저장은 이 값을 먼저 보므로 구역과 같이 고친다.
model::Control* sectionDefControl(model::Section& section) {
    if (section.paragraphs.empty()) return nullptr;
    for (model::Run& run : section.paragraphs.front().runs) {
        for (model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::SectionDef) {
                return &item.control;
            }
        }
    }
    return nullptr;
}

double numberOf(const FormatValue& value) {
    if (const double* number = std::get_if<double>(&value)) return *number;
    if (const bool* flag = std::get_if<bool>(&value)) return *flag ? 1 : 0;
    return 0;
}

std::uint32_t hwpLength(const FormatValue& value) {
    return static_cast<std::uint32_t>(std::clamp<long>(std::lround(numberOf(value)), 0L, 1000000L));
}

}  // namespace

FormatFields pageSetupOf(const model::Document& doc, int index);

FormatFields pageSetupAtCursor(const model::Document& doc) {
    const int index = cursorSection(doc);
    if (index < 0) return {};
    return pageSetupOf(doc, index);
}

FormatFields sectionFields(const model::Document& doc, int index) {
    if (index < 0 || index >= static_cast<int>(doc.sections.size())) return {};
    FormatFields out = pageSetupOf(doc, index);
    const model::Section& section = doc.sections[static_cast<std::size_t>(index)];
    // 단: 구역 첫 문단의 첫 단 정의.
    const model::Control* columns = nullptr;
    if (!section.paragraphs.empty()) {
        for (const model::Run& run : section.paragraphs.front().runs) {
            for (const model::InlineItem& item : run.items) {
                if (!columns && item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::ColumnDef) {
                    columns = &item.control;
                }
            }
        }
    }
    out.emplace_back("columnCount", static_cast<double>(columns ? std::max(1u, columns->columnCount) : 1u));
    out.emplace_back("columnGap", static_cast<double>(columns ? columns->columnGap : 0));
    out.emplace_back("columnType", columns && !columns->columnType.empty() ? columns->columnType : std::string("NEWSPAPER"));
    out.emplace_back("columnSameSize", columns ? columns->columnSameSize : true);
    out.emplace_back("columnLineType", columns && !columns->columnLineType.empty() ? columns->columnLineType : std::string("NONE"));
    out.emplace_back("pageStart", static_cast<double>(section.pageStart));
    out.emplace_back("sectionDefaultTab", static_cast<double>(section.defaultTab));
    out.emplace_back("hideHeader", section.hideHeader);
    out.emplace_back("hideFooter", section.hideFooter);
    out.emplace_back("hideMaster", section.hideMaster);
    out.emplace_back("hidePageNum", section.hidePageNum);
    return out;
}

FormatFields pageSetupOf(const model::Document& doc, int index) {
    FormatFields out;
    const model::Section& section = doc.sections[static_cast<std::size_t>(index)];
    const model::PageDef page = section.page.value_or(model::PageDef{});
    // 폭·길이는 한글 대화 상자처럼 좁게 놓은 크기다. 가로는 landscape 로 따로 준다.
    out.emplace_back("paperWidth", static_cast<double>(page.storedWidth()));
    out.emplace_back("paperHeight", static_cast<double>(page.storedHeight()));
    out.emplace_back("landscape", page.landscape);
    out.emplace_back("gutterType", static_cast<double>(page.gutterType));
    out.emplace_back("marginLeft", static_cast<double>(page.marginLeft));
    out.emplace_back("marginRight", static_cast<double>(page.marginRight));
    out.emplace_back("marginTop", static_cast<double>(page.marginTop));
    out.emplace_back("marginBottom", static_cast<double>(page.marginBottom));
    out.emplace_back("marginHeader", static_cast<double>(page.marginHeader));
    out.emplace_back("marginFooter", static_cast<double>(page.marginFooter));
    out.emplace_back("marginGutter", static_cast<double>(page.marginGutter));
    out.emplace_back("lineGrid", static_cast<double>(section.lineGrid));
    out.emplace_back("charGrid", static_cast<double>(section.charGrid));
    out.emplace_back("wongoji", section.wongoji);
    out.emplace_back("textDirection", static_cast<double>(section.textDirection));
    out.emplace_back("section", static_cast<double>(index));
    out.emplace_back("sectionCount", static_cast<double>(doc.sections.size()));
    return out;
}

EditResult applyPageSetupNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                             bool wholeDocument, bool useStoredLineHeight) {
    if (readOnly()) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    const int current = cursorSection(doc);
    if (current < 0 || fields.empty()) return {};
    PointerHold& hold = pointerHold();
    if (hold.composing) clearComposition(hold);

    // 커서는 글자 번호로 들고 있다가 다시 짠 쪽에서 되돌린다.
    const PointerCursor cur = pointerCursor();
    int anchorChar = 0;
    int focusChar = 0;
    if (cur.active && cur.story > 0) {
        anchorChar = offsetToCharIndex(doc, pages, cur.story, cur.anchorParagraph, cur.anchorOffset);
        focusChar = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    }

    bool changed = false;
    const int first = wholeDocument ? 0 : current;
    const int last = wholeDocument ? static_cast<int>(doc.sections.size()) - 1 : current;
    for (int index = first; index <= last; ++index) {
        model::Section& section = doc.sections[static_cast<std::size_t>(index)];
        model::PageDef page = section.page.value_or(model::PageDef{});
        const model::PageDef before = page;
        std::uint32_t paperW = page.storedWidth();
        std::uint32_t paperH = page.storedHeight();
        std::int32_t lineGrid = section.lineGrid;
        std::int32_t charGrid = section.charGrid;
        bool wongoji = section.wongoji;
        std::uint8_t textDirection = section.textDirection;
        for (const auto& [key, value] : fields) {
            if (key == "paperWidth") paperW = std::max<std::uint32_t>(1000, hwpLength(value));
            else if (key == "paperHeight") paperH = std::max<std::uint32_t>(1000, hwpLength(value));
            else if (key == "landscape") page.landscape = numberOf(value) != 0;
            else if (key == "gutterType")
                page.gutterType = static_cast<std::uint8_t>(std::clamp<long>(std::lround(numberOf(value)), 0L, 2L));
            else if (key == "marginLeft") page.marginLeft = hwpLength(value);
            else if (key == "marginRight") page.marginRight = hwpLength(value);
            else if (key == "marginTop") page.marginTop = hwpLength(value);
            else if (key == "marginBottom") page.marginBottom = hwpLength(value);
            else if (key == "marginHeader") page.marginHeader = hwpLength(value);
            else if (key == "marginFooter") page.marginFooter = hwpLength(value);
            else if (key == "marginGutter") page.marginGutter = hwpLength(value);
            else if (key == "lineGrid") lineGrid = static_cast<std::int32_t>(hwpLength(value));
            else if (key == "charGrid") charGrid = static_cast<std::int32_t>(hwpLength(value));
            else if (key == "wongoji") wongoji = numberOf(value) != 0;
            else if (key == "textDirection")
                textDirection = static_cast<std::uint8_t>(std::clamp<long>(std::lround(numberOf(value)), 0L, 2L));
        }
        // 모델은 놓인 크기를 갖는다. 넓게면 긴 변이 가로다.
        page.width = paperW;
        page.height = paperH;
        page.applyOrientation();
        // 본문 폭은 홀수·짝수 쪽이 같으니 홀수 쪽만 견준다. 폭이 그대로면 저장된 줄 나눔을 그대로 쓴다.
        const model::PageDef oldOdd = before.side(1);
        const model::PageDef newOdd = page.side(1);
        const bool widthChanged = page.width != before.width || oldOdd.marginLeft != newOdd.marginLeft ||
                                  oldOdd.marginRight != newOdd.marginRight;
        const bool gridChanged =
            charGrid != section.charGrid || wongoji != section.wongoji || textDirection != section.textDirection;
        const bool pageChanged = widthChanged || page.height != before.height || page.landscape != before.landscape ||
                                 page.gutterType != before.gutterType || page.marginGutter != before.marginGutter ||
                                 page.marginTop != before.marginTop || page.marginBottom != before.marginBottom ||
                                 page.marginHeader != before.marginHeader || page.marginFooter != before.marginFooter ||
                                 page.marginLeft != before.marginLeft || page.marginRight != before.marginRight;
        if (!pageChanged && !gridChanged && lineGrid == section.lineGrid) continue;
        section.page = page;
        section.lineGrid = lineGrid;
        section.charGrid = charGrid;
        section.wongoji = wongoji;
        section.textDirection = textDirection;
        if (model::Control* def = sectionDefControl(section)) {
            def->textDirection = textDirection;
            def->page = page;
            def->lineGrid = lineGrid;
            def->charGrid = charGrid;
            def->wongoji = wongoji;
        }
        if (widthChanged || gridChanged) {
            for (model::Paragraph& para : section.paragraphs) para.lineSegs.clear();
        }
        changed = true;
    }
    if (!changed) return makeEditResult(doc, false, false, hold.story, hold.focusPara);

    hold.preferredX = -1;
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.incremental = false;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    if (cur.active && cur.story > 0) {
        const int anchorOff = charIndexToOffset(doc, pages, cur.story, cur.anchorParagraph, anchorChar);
        const int focusOff = charIndexToOffset(doc, pages, cur.story, cur.paragraph, focusChar);
        setPointerCursor(doc, pages, model::CursorPos{cur.story, cur.anchorParagraph, anchorOff},
                         model::CursorPos{cur.story, cur.paragraph, focusOff});
    }
    return makeEditResult(doc, true, true, cur.story, cur.paragraph, &relayout);
}

namespace {

// 표 칸에 쓰는 실선 테두리(0.12mm). 같은 테두리가 이미 있으면 그것을 쓴다. 한글이 새 표에 쓰는 테두리와 같다.
std::uint32_t solidTableBorder(model::Document& doc) {
    const std::int32_t width = mmToHu(0.12);
    const auto solid = [&](const model::BorderSide& side) {
        return side.type == "SOLID" && side.width == width && side.color == "#000000";
    };
    std::uint32_t maxId = 0;
    for (const model::BorderFill& fill : doc.header.borderFills) {
        maxId = std::max(maxId, fill.id);
        const bool plain = (fill.faceColor.empty() || fill.faceColor == "none") && fill.gradColor.empty() &&
                           fill.imageId.empty() && fill.slash.type == "NONE" && fill.backSlash.type == "NONE" &&
                           (fill.centerLine.empty() || fill.centerLine == "NONE");
        if (plain && solid(fill.left) && solid(fill.right) && solid(fill.top) && solid(fill.bottom)) return fill.id;
    }
    model::BorderFill fill;
    fill.id = maxId + 1;
    for (model::BorderSide* side : {&fill.left, &fill.right, &fill.top, &fill.bottom}) {
        side->type = "SOLID";
        side->width = width;
        side->color = "#000000";
    }
    fill.diagonal.type = "SOLID";
    fill.diagonal.width = mmToHu(0.1);
    doc.header.borderFills.push_back(fill);
    return fill.id;
}

// 그 문단이 놓인 단의 폭. 표 칸 안이면 칸 안 폭이다. 못 찾으면 구역 본문 폭이다.
std::int32_t columnWidthAt(const model::Document& doc, const std::vector<Node>& pages, int story, int paragraph) {
    std::int32_t found = -1;
    std::function<void(const Node&, std::int32_t)> walk = [&](const Node& node, std::int32_t columnWidth) {
        if (found >= 0) return;
        const std::int32_t width = node.kind == NodeKind::Column ? node.width : columnWidth;
        if (node.kind == NodeKind::Paragraph && node.story == story && node.paragraph == paragraph) {
            found = width;
            return;
        }
        for (const Node& child : node.children) walk(child, width);
    };
    for (const Node& page : pages) {
        walk(page, -1);
        if (found > 0) return found;
    }
    if (const model::StorySlot* slot = resolveStory(doc, story)) {
        if (slot->sectionIndex >= 0 && slot->sectionIndex < static_cast<int>(doc.sections.size())) {
            const model::PageDef page = doc.sections[static_cast<std::size_t>(slot->sectionIndex)].flowPage(1);
            return std::max<std::int32_t>(1440, static_cast<std::int32_t>(page.width) -
                                                    static_cast<std::int32_t>(page.marginLeft) -
                                                    static_cast<std::int32_t>(page.marginRight));
        }
    }
    return 42520;
}

}  // namespace

// 한글 2024 표 만들기(너비 단에 맞춤, 높이 자동)와 같은 표. tmp/insert/tbl-*.
// 칸 폭 = (단 폭 − 바깥 좌우 여백 283 + 283) ÷ 칸 수(버림), 표 폭은 칸 폭의 합, 칸 높이 282, 안 여백 510·141, 글자처럼 취급.
// 캐럿 앞 글은 그 문단에 남고 표는 제 문단, 캐럿 뒤 글은 다음 문단이다. 앞 글이 없으면 그 문단에 표를 둔다.
// 캐럿은 첫 칸으로 간다.
EditResult insertTableNow(model::Document& doc, std::vector<Node>& pages, int rows, int cols,
                          bool useStoredLineHeight) {
    if (readOnly()) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    rows = std::clamp(rows, 1, kTableMaxRows);
    cols = std::clamp(cols, 1, kTableMaxCols);
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.paragraph < 0 || cur.paragraph >= static_cast<int>(paras->size())) return {};
    clearComposition(hold);

    const int story = cur.story;
    int paraIdx = cur.paragraph;
    int charIndex = 0;
    int paraDelta = 0;
    if (cur.hasSelection) {
        paraDelta = -(cur.endParagraph - cur.startParagraph);
        deleteSelectionRange(doc, pages, story, cur.startParagraph, cur.startOffset, cur.endParagraph,
                             cur.endOffset, paraIdx, charIndex);
        paras = storyParagraphs(doc, story);
    } else {
        charIndex = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
    }
    const std::int32_t columnWidth = columnWidthAt(doc, pages, story, paraIdx);

    model::Paragraph& host = (*paras)[static_cast<std::size_t>(paraIdx)];
    const std::uint32_t charShape = charShapeAtParagraphChar(host, charIndex, false);
    std::int32_t lineHeight = 1000;
    if (const model::CharShape* shape = Painter::findCharShape(doc, charShape)) {
        if (shape->height > 0) lineHeight = shape->height;
    }

    model::Control control;
    control.kind = model::ControlKind::Table;
    control.ctrlId = "tbl ";
    model::Placement placement;
    placement.treatAsChar = true;
    placement.flowWithText = true;
    placement.allowOverlap = false;
    placement.textWrap = "TOP_AND_BOTTOM";
    placement.vertRelTo = "PARA";
    placement.horzRelTo = "COLUMN";
    placement.marginLeft = 283;
    placement.marginRight = 283;
    placement.marginTop = 283;
    placement.marginBottom = 283;
    const std::int32_t tableWidth = std::max<std::int32_t>(cols * 200, columnWidth - 566);
    const std::int32_t cellWidth = tableWidth / cols;
    const std::int32_t cellHeight = 282;
    // 한글은 짠 뒤 표 폭을 칸 폭의 합으로 적는다(41954 → 41952).
    placement.width = static_cast<std::uint32_t>(cellWidth * cols);
    placement.height = static_cast<std::uint32_t>(rows * (cellHeight + lineHeight));
    control.placement = placement;

    model::Table table;
    table.rowCount = static_cast<std::uint32_t>(rows);
    table.colCount = static_cast<std::uint32_t>(cols);
    table.borderFillId = solidTableBorder(doc);
    table.pageBreak = "CELL";
    table.repeatHeader = true;
    table.marginLeft = 510;
    table.marginRight = 510;
    table.marginTop = 141;
    table.marginBottom = 141;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            model::TableCell cell;
            cell.row = static_cast<std::uint32_t>(r);
            cell.col = static_cast<std::uint32_t>(c);
            cell.width = static_cast<std::uint32_t>(cellWidth);
            cell.height = static_cast<std::uint32_t>(cellHeight);
            cell.borderFillId = table.borderFillId;
            cell.marginLeft = 510;
            cell.marginRight = 510;
            cell.marginTop = 141;
            cell.marginBottom = 141;
            cell.vertAlign = "CENTER";
            cell.content.vertAlign = "CENTER";
            model::Paragraph para;
            para.paraShapeId = host.paraShapeId;
            para.styleId = host.styleId;
            model::Run run;
            run.charShapeId = charShape;
            para.runs.push_back(std::move(run));
            cell.content.paragraphs.push_back(std::move(para));
            table.cells.push_back(std::move(cell));
        }
    }
    control.table = std::move(table);

    model::Paragraph right = splitParagraphAt(host, charIndex);
    const bool leftEmpty = paragraphCharCount(host) == 0;
    int tablePara = paraIdx;
    int added = 1;
    model::Run tableRun;
    tableRun.charShapeId = charShape;
    tableRun.items.push_back(model::InlineItem::makeControl(std::move(control)));
    if (leftEmpty) {
        host.lineSegs.clear();
        host.runs.push_back(std::move(tableRun));
        paras->insert(paras->begin() + paraIdx + 1, std::move(right));
    } else {
        model::Paragraph tableParagraph;
        tableParagraph.paraShapeId = host.paraShapeId;
        tableParagraph.styleId = host.styleId;
        tableParagraph.runs.push_back(std::move(tableRun));
        paras->insert(paras->begin() + paraIdx + 1, std::move(tableParagraph));
        paras->insert(paras->begin() + paraIdx + 2, std::move(right));
        tablePara = paraIdx + 1;
        added = 2;
    }
    rebuildStoryMap(doc);
    hold.preferredX = -1;
    const EditRelayoutResult relayout = relayoutAfterEdit(
        doc, pages,
        EditRelayoutSpec{story, paraIdx, paraDelta + added, charIndex, charIndex, charIndex, useStoredLineHeight});

    // 첫 칸에 캐럿을 둔다.
    int cellStory = -1;
    if (std::vector<model::Paragraph>* now = storyParagraphs(doc, story)) {
        model::Paragraph& placed = (*now)[static_cast<std::size_t>(tablePara)];
        for (model::Run& run : placed.runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Control && item.control.table &&
                    !item.control.table->cells.empty()) {
                    cellStory = ensureStoryMap(doc).storyOf(&item.control.table->cells.front().content.paragraphs);
                }
            }
        }
    }
    if (cellStory > 0) setPointerCursor(doc, pages, model::CursorPos{cellStory, 0, 0});
    return makeEditResult(doc, true, true, story, paraIdx, &relayout);
}

bool breakKindOf(const std::string& name, BreakKind& kind) {
    if (name == "page") kind = BreakKind::Page;
    else if (name == "column") kind = BreakKind::Column;
    else if (name == "columnDef") kind = BreakKind::ColumnDef;
    else if (name == "section") kind = BreakKind::Section;
    else return false;
    return true;
}

EditResult insertBreakNow(model::Document& doc, std::vector<Node>& pages, BreakKind kind,
                                        bool useStoredLineHeight) {
    if (readOnly()) return {};
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    const model::StorySlot* slot = resolveStory(doc, cur.story);
    if (!slot || slot->kind != model::StoryKind::Section || slot->sectionIndex < 0 ||
        slot->sectionIndex >= static_cast<int>(doc.sections.size())) {
        return {};
    }
    const int sectionIndex = slot->sectionIndex;
    const int story = cur.story;
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || cur.paragraph < 0 || cur.paragraph >= static_cast<int>(paras->size())) return {};
    clearComposition(hold);

    int paraIdx = cur.paragraph;
    int charIndex = 0;
    if (cur.hasSelection) {
        deleteSelectionRange(doc, pages, story, cur.startParagraph, cur.startOffset, cur.endParagraph, cur.endOffset,
                             paraIdx, charIndex);
        paras = storyParagraphs(doc, story);
    } else {
        charIndex = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
    }
    model::Paragraph& host = (*paras)[static_cast<std::size_t>(paraIdx)];
    model::Paragraph right = splitParagraphAt(host, charIndex);
    host.lineSegs.clear();
    right.lineSegs.clear();
    right.pageBreak = kind == BreakKind::Page;
    right.columnBreak = kind == BreakKind::Column;
    right.layoutBreak = kind == BreakKind::ColumnDef;

    // 새 단 설정·새 구역 첫 문단에 베껴 넣을 정의 개체. 단 정의는 나누는 자리에서 쓰이던 것이다.
    std::vector<model::InlineItem> carried;
    const auto copyControls = [&](const model::Paragraph& from, bool sectionDef, bool columnDef) {
        for (const model::Run& run : from.runs) {
            for (const model::InlineItem& item : run.items) {
                if (item.type != model::InlineItem::Type::Control) continue;
                if ((sectionDef && item.control.kind == model::ControlKind::SectionDef) ||
                    (columnDef && item.control.kind == model::ControlKind::ColumnDef)) {
                    carried.push_back(item);
                }
            }
        }
    };
    if (kind == BreakKind::ColumnDef) {
        for (int p = paraIdx; p >= 0 && carried.empty(); --p) copyControls((*paras)[static_cast<std::size_t>(p)], false, true);
    } else if (kind == BreakKind::Section && !paras->empty()) {
        copyControls(paras->front(), true, true);
    }
    if (!carried.empty()) {
        if (right.runs.empty()) {
            model::Run run;
            run.charShapeId = host.runs.empty() ? 0 : host.runs.back().charShapeId;
            right.runs.push_back(std::move(run));
        }
        auto& items = right.runs.front().items;
        items.insert(items.begin(), carried.begin(), carried.end());
    }

    int newStory = story;
    int newPara = paraIdx + 1;
    if (kind == BreakKind::Section) {
        model::Section& from = doc.sections[static_cast<std::size_t>(sectionIndex)];
        model::Section next = from;
        next.paragraphs.clear();
        next.pageStart = 0;  // 앞 구역 다음 번호로 잇는다
        next.paragraphs.push_back(std::move(right));
        for (std::size_t p = static_cast<std::size_t>(paraIdx) + 1; p < from.paragraphs.size(); ++p) {
            next.paragraphs.push_back(std::move(from.paragraphs[p]));
        }
        from.paragraphs.resize(static_cast<std::size_t>(paraIdx) + 1);
        doc.sections.insert(doc.sections.begin() + sectionIndex + 1, std::move(next));
        doc.header.sectionCount = static_cast<std::uint32_t>(doc.sections.size());
        newPara = 0;
    } else {
        paras->insert(paras->begin() + paraIdx + 1, std::move(right));
    }
    rebuildStoryMap(doc);
    if (kind == BreakKind::Section) {
        newStory = ensureStoryMap(doc).storyOf(&doc.sections[static_cast<std::size_t>(sectionIndex) + 1].paragraphs);
    }

    // 쪽·단·구역이 바뀌면 뒤 쪽이 모두 달라진다. 문서 전체를 다시 놓는다.
    hold.preferredX = -1;
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    if (newStory > 0) setPointerCursor(doc, pages, model::CursorPos{newStory, newPara, 0});
    return makeEditResult(doc, true, true, newStory, newPara, &relayout);
}

// 그림 넣기. 한글 2024 그림 넣기(원래 크기)와 같다(tmp/insert/pic-*). 크기는 픽셀 × 75(96dpi, 파일의 dpi 는 보지 않는다),
// 글자처럼 취급, 위아래로 비켜 가기, 바깥 여백 0 이다. 단보다 넓으면 비율을 지켜 단 폭으로 줄인다(한글은 줄이지 않는다).
// 그림은 캐럿 자리의 글자가 되고, 캐럿은 그 뒤로 간다.
EditResult insertPictureNow(model::Document& doc, std::vector<Node>& pages, const Bytes& image,
                            bool useStoredLineHeight, std::int32_t wantWidth, std::int32_t wantHeight) {
    if (readOnly()) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    int pixelW = 0;
    int pixelH = 0;
    std::string extension;
    if (!imageInfo(image.data(), image.size(), pixelW, pixelH, extension)) return {};
    PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.paragraph < 0 || cur.paragraph >= static_cast<int>(paras->size())) return {};
    clearComposition(hold);

    const int story = cur.story;
    int paraIdx = cur.paragraph;
    int charIndex = 0;
    int paraDelta = 0;
    int oldEnd = 0;
    if (cur.hasSelection) {
        paraDelta = -(cur.endParagraph - cur.startParagraph);
        const int endChar =
            paraDelta == 0 ? offsetToCharIndex(doc, pages, story, cur.endParagraph, cur.endOffset) : 0;
        deleteSelectionRange(doc, pages, story, cur.startParagraph, cur.startOffset, cur.endParagraph,
                             cur.endOffset, paraIdx, charIndex);
        paras = storyParagraphs(doc, story);
        oldEnd = paraDelta == 0 ? endChar : charIndex;
    } else {
        charIndex = offsetToCharIndex(doc, pages, story, paraIdx, cur.offset);
        oldEnd = charIndex;
    }

    // 그림 파일을 문서에 넣는다. HWP 에서 온 문서는 숫자 번호, HWPX 는 imageN 이다.
    model::BinItem bin;
    const bool numbered = doc.format.format == model::SourceFormat::Hwp;
    for (int seq = static_cast<int>(doc.header.binItems.size()) + 1;; ++seq) {
        const std::string id = numbered ? std::to_string(seq) : "image" + std::to_string(seq);
        const bool used = std::any_of(doc.header.binItems.begin(), doc.header.binItems.end(),
                                      [&](const model::BinItem& item) { return item.id == id; });
        if (!used) {
            bin.id = id;
            break;
        }
    }
    bin.storage = "EMBEDDING";
    bin.extension = extension;
    bin.mediaType = extension == "jpg" ? "image/jpeg" : "image/" + extension;
    if (numbered) {
        char name[32];
        std::snprintf(name, sizeof(name), "BinData/BIN%04X.%s", static_cast<unsigned>(doc.header.binItems.size() + 1),
                      extension.c_str());
        bin.path = name;
    } else {
        bin.path = "BinData/" + bin.id + "." + extension;
    }
    bin.size = image.size();
    bin.data = image;
    const std::string binId = bin.id;
    doc.header.binItems.push_back(std::move(bin));

    std::int64_t width = static_cast<std::int64_t>(pixelW) * 75;
    std::int64_t height = static_cast<std::int64_t>(pixelH) * 75;
    const std::int32_t columnWidth = columnWidthAt(doc, pages, story, paraIdx);
    if (wantWidth > 0 || wantHeight > 0) {
        // 정한 크기. 한쪽만 주면 그림 가로세로 비율을 지킨다. 단 폭으로 줄이지 않는다.
        const std::int64_t baseW = std::max<std::int64_t>(1, width);
        const std::int64_t baseH = std::max<std::int64_t>(1, height);
        width = wantWidth > 0 ? wantWidth : baseW * wantHeight / baseH;
        height = wantHeight > 0 ? wantHeight : baseH * wantWidth / baseW;
    } else if (width > columnWidth && columnWidth > 0) {
        height = height * columnWidth / width;
        width = columnWidth;
    }
    model::Control control;
    control.kind = model::ControlKind::Picture;
    control.ctrlId = "gso ";
    control.shapeType = "pic";
    model::Placement placement;
    placement.treatAsChar = true;
    placement.flowWithText = true;
    placement.allowOverlap = false;
    placement.textWrap = "TOP_AND_BOTTOM";
    placement.vertRelTo = "PARA";
    placement.horzRelTo = "COLUMN";
    placement.width = static_cast<std::uint32_t>(std::max<std::int64_t>(1, width));
    placement.height = static_cast<std::uint32_t>(std::max<std::int64_t>(1, height));
    control.placement = placement;
    control.binItemId = binId;
    // 한글은 크기를 정해 넣은 그림도 원래 크기·그림 상자를 넣은 크기로 적는다(tmp/insert/pic-big-sized).
    control.picture.originWidth = static_cast<std::int32_t>(placement.width);
    control.picture.originHeight = static_cast<std::int32_t>(placement.height);

    // 캐럿 자리에서 문단을 갈라 그 사이에 그림 글자를 끼우고 다시 붙인다.
    model::Paragraph& host = (*paras)[static_cast<std::size_t>(paraIdx)];
    const std::uint32_t charShape = charShapeAtParagraphChar(host, charIndex, false);
    model::Paragraph right = splitParagraphAt(host, charIndex);
    model::Run pictureRun;
    pictureRun.charShapeId = charShape;
    pictureRun.items.push_back(model::InlineItem::makeControl(std::move(control)));
    host.runs.push_back(std::move(pictureRun));
    for (model::Run& run : right.runs) {
        if (!run.items.empty()) host.runs.push_back(std::move(run));
    }
    host.lineSegs.clear();
    rebuildStoryMap(doc);
    hold.preferredX = -1;
    const EditRelayoutResult relayout = relayoutAfterEdit(
        doc, pages, EditRelayoutSpec{story, paraIdx, paraDelta, charIndex, oldEnd, charIndex + 1, useStoredLineHeight});
    const int newOff = charIndexToOffset(doc, pages, story, paraIdx, charIndex + 1);
    setPointerCursor(doc, pages, model::CursorPos{story, paraIdx, newOff});
    return makeEditResult(doc, true, true, story, paraIdx, &relayout);
}

namespace {

// 문단 줄들의 맨 위 자리(쪽 위에서 잰 값)를 모은다. 개체·머리말·바탕쪽 안 줄은 뺀다.
void bodyLineTops(const Node& node, std::int32_t y, int story, std::vector<std::pair<std::int32_t, int>>& out) {
    if (node.pageBand || node.fromMaster || node.kind == NodeKind::Object || node.role == "lineNumbers" || node.role == "memos") return;
    const std::int32_t here = y + node.y;
    if (node.kind == NodeKind::Line) {
        if (node.paragraph >= 0) out.emplace_back(here, node.paragraph);
        return;
    }
    if (node.kind == NodeKind::Paragraph && node.story > 0 && node.story != story) return;
    for (const Node& child : node.children) bodyLineTops(child, here, story, out);
}

// 도형을 붙일 문단. 그 쪽 본문에서 누른 자리 위의 마지막 줄, 없으면 첫 줄의 문단이다.
bool shapeAnchorAt(const model::Document& doc, const std::vector<Node>& pages, int page, std::int32_t y, int& story,
                   int& paragraph) {
    if (page < 0 || page >= static_cast<int>(pages.size())) return false;
    const Node& paper = pages[static_cast<std::size_t>(page)];
    const int section = paper.section >= 0 ? paper.section : 0;
    if (section >= static_cast<int>(doc.sections.size())) return false;
    story = ensureStoryMap(doc).storyOf(&doc.sections[static_cast<std::size_t>(section)].paragraphs);
    if (story <= 0) return false;
    std::vector<std::pair<std::int32_t, int>> tops;
    bodyLineTops(paper, 0, story, tops);
    if (tops.empty()) {
        paragraph = static_cast<int>(doc.sections[static_cast<std::size_t>(section)].paragraphs.size()) - 1;
        return paragraph >= 0;
    }
    std::sort(tops.begin(), tops.end());
    paragraph = tops.front().second;
    for (const auto& [top, para] : tops) {
        if (top <= y) paragraph = para;
    }
    return true;
}

}  // namespace

bool shapeAnchor(const model::Document& doc, const std::vector<Node>& pages, int page, std::int32_t y, int& story,
                 int& paragraph) {
    return shapeAnchorAt(doc, pages, page, y, story, paragraph);
}

// 도형 넣기. 한글 2024 에서 마우스로 그린 도형과 같다(samples/도형 그리기.hwpx).
// 글 앞으로, 종이 기준 자리(끈 상자), 바깥 여백 0, 겹침 허용, 글과 함께 움직이지 않음. 선은 검정 0.12mm(33) 실선,
// 직사각형·타원·글상자는 흰색 채움이고 직선은 채움이 없다. 글상자는 가운데 정렬, 안 여백 283, 빈 문단 하나이고
// 세로 글상자의 글 폭은 도형 높이다. 직선은 끈 방향 그대로 시작·끝 점을 둔다. 도형은 누른 자리 문단의 앞에 붙는다.
EditResult insertShapeNow(model::Document& doc, std::vector<Node>& pages, int tool, int page, std::int32_t x0,
                          std::int32_t y0, std::int32_t x1, std::int32_t y1, bool useStoredLineHeight) {
    if (readOnly() || tool < 1 || tool > 5) return {};
    if (EditResult blocked; blockedByLicenseFonts(doc, blocked)) return blocked;
    const std::int32_t left = std::min(x0, x1);
    const std::int32_t top = std::min(y0, y1);
    const std::int32_t width = std::abs(x1 - x0);
    const std::int32_t height = std::abs(y1 - y0);
    const bool line = tool == 3;
    // 2mm 보다 작게 끌면 그리지 않는다.
    constexpr std::int32_t kMinDraw = 567;
    if (line ? std::max(width, height) < kMinDraw : (width < kMinDraw || height < kMinDraw)) return {};
    int story = -1;
    int paraIdx = -1;
    if (!shapeAnchorAt(doc, pages, page, top, story, paraIdx)) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || paraIdx < 0 || paraIdx >= static_cast<int>(paras->size())) return {};
    PointerHold& hold = pointerHold();
    clearComposition(hold);

    std::int32_t zOrder = 0;
    for (const model::Paragraph& para : *paras) {
        for (const model::Run& run : para.runs) {
            for (const model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Control && item.control.placement) {
                    zOrder = std::max(zOrder, item.control.placement->zOrder + 1);
                }
            }
        }
    }

    model::Control control;
    control.kind = model::ControlKind::Shape;
    control.ctrlId = "gso ";
    control.shapeType = tool == 2 ? "ellipse" : line ? "line" : "rect";
    model::Placement placement;
    placement.treatAsChar = false;
    placement.flowWithText = false;
    placement.allowOverlap = true;
    placement.textWrap = "IN_FRONT_OF_TEXT";
    placement.vertRelTo = "PAPER";
    placement.horzRelTo = "PAPER";
    placement.horzOffset = left;
    placement.vertOffset = top;
    placement.width = static_cast<std::uint32_t>(std::max<std::int32_t>(1, width));
    placement.height = static_cast<std::uint32_t>(std::max<std::int32_t>(1, height));
    placement.zOrder = zOrder;
    control.placement = placement;
    control.strokeType = "SOLID";
    control.strokeColor = "#000000";
    control.strokeWidth = 33;
    control.strokeEnd = 1;
    if (line) {
        control.points.push_back({x0 <= x1 ? 0 : width, y0 <= y1 ? 0 : height});
        control.points.push_back({x0 <= x1 ? width : 0, y0 <= y1 ? height : 0});
    } else {
        control.fillColor = "#FFFFFF";
    }
    if (tool == 4 || tool == 5) {
        model::SubList box;
        box.vertAlign = "CENTER";
        box.textDirection = tool == 5 ? 1 : 0;
        model::Paragraph para;
        para.runs.push_back(model::Run{});
        box.paragraphs.push_back(std::move(para));
        control.subLists.push_back(std::move(box));
        control.boxPadLeft = 283;
        control.boxPadRight = 283;
        control.boxPadTop = 283;
        control.boxPadBottom = 283;
        control.boxTextWidth = static_cast<std::uint32_t>(tool == 5 ? height : width);
    }

    // 문단 앞(구역·단 정의 뒤)에 붙인다. 글자 칸이 아니라 글자 번호는 그대로이고, 저장된 줄 자리는 낡아 지운다.
    model::Paragraph& host = (*paras)[static_cast<std::size_t>(paraIdx)];
    const int length = paragraphCharCount(host);
    const std::uint32_t charShape = charShapeAtParagraphChar(host, 0, true);
    model::Paragraph right = splitParagraphAt(host, 0);
    model::Run shapeRun;
    shapeRun.charShapeId = charShape;
    shapeRun.items.push_back(model::InlineItem::makeControl(std::move(control)));
    host.runs.push_back(std::move(shapeRun));
    for (model::Run& run : right.runs) {
        if (!run.items.empty()) host.runs.push_back(std::move(run));
    }
    host.lineSegs.clear();
    rebuildStoryMap(doc);
    hold.preferredX = -1;
    // 종이 기준 개체는 쪽을 짤 때 놓는다(placeFrameObjects). 문단 하나만 다시 짜면 빠지므로 쪽을 다시 짠다.
    (void)length;
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;

    // 글상자는 그 안에 캐럿을 둔다.
    if (tool == 4 || tool == 5) {
        if (std::vector<model::Paragraph>* now = storyParagraphs(doc, story)) {
            for (model::Run& run : (*now)[static_cast<std::size_t>(paraIdx)].runs) {
                for (model::InlineItem& item : run.items) {
                    if (item.type == model::InlineItem::Type::Control && !item.control.subLists.empty() &&
                        item.control.placement && item.control.placement->zOrder == zOrder) {
                        const int boxStory = ensureStoryMap(doc).storyOf(&item.control.subLists.front().paragraphs);
                        if (boxStory > 0) setPointerCursor(doc, pages, model::CursorPos{boxStory, 0, 0});
                    }
                }
            }
        }
    }
    return makeEditResult(doc, true, true, story, paraIdx, &relayout);
}

// 끌어 옮긴 개체의 위치를 바꾼다. 가로·세로 거리는 맞춤에 따라 더하거나(왼쪽·위·가운데) 뺀다(오른쪽·아래).
// 글을 비켜 가게 하는 개체는 그 글의 저장된 줄을 지워 다시 나눈다. 쪽을 다시 짜고 그 개체를 다시 고른다.
EditResult moveObjectNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    if (!hold.movePending) return {};
    hold.movePending = false;
    if (readOnly()) return {};
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, hold.moveStory);
    model::Control* control = anchoredControl(doc, hold.moveStory, hold.moveParagraph, hold.moveItem);
    if (!paras || !control || !control->placement || control->placement->treatAsChar) return {};
    model::Placement* target = &*control->placement;
    const auto step = [](const std::string& align, std::int32_t delta) {
        return align == "RIGHT" || align == "BOTTOM" || align == "OUTSIDE" ? -delta : delta;
    };
    target->horzOffset += step(target->horzAlign, hold.moveDX);
    target->vertOffset += step(target->vertAlign, hold.moveDY);
    if (target->textWrap == "SQUARE" || target->textWrap == "TOP_AND_BOTTOM") {
        for (model::Paragraph& para : *paras) para.lineSegs.clear();
    }
    const int story = hold.moveStory;
    const int paragraph = hold.moveParagraph;
    const int item = hold.moveItem;
    const int page = hold.movePage;
    hold.selectedObject = nullptr;
    hold.boxes.clear();
    hold.objectHandles = false;
    invalidateLinePlaces({}, true);
    model::ensureParagraphIds(doc);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    reselectAnchoredObject(doc, pages, page, story, paragraph, item);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    return makeEditResult(doc, true, true, story, paragraph, &relayout);
}

namespace {

// 칸 폭·줄 높이를 비율대로 늘리고, 끝수는 마지막 칸이 받아 합이 total 이 되게 한다.
std::vector<std::int32_t> scaledSizes(const std::vector<std::int32_t>& sizes, std::int32_t total, std::int32_t floor) {
    std::int64_t sum = 0;
    for (const std::int32_t size : sizes) sum += std::max<std::int32_t>(0, size);
    std::vector<std::int32_t> out(sizes.size(), 0);
    if (sizes.empty() || sum <= 0) return out;
    std::int64_t used = 0;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const std::int64_t share = i + 1 == sizes.size()
                                       ? total - used
                                       : static_cast<std::int64_t>(std::llround(static_cast<double>(sizes[i]) * total /
                                                                                 static_cast<double>(sum)));
        out[i] = static_cast<std::int32_t>(std::max<std::int64_t>(floor, share));
        used += out[i];
    }
    return out;
}

void clearTableLines(model::Table& table) {
    for (model::TableCell& cell : table.cells) {
        for (model::Paragraph& para : cell.content.paragraphs) para.lineSegs.clear();
    }
}

}  // namespace

// 그림이 아닌 개체의 크기 조절을 반영한다. 표는 그린 줄 높이·칸 폭을 새 크기에 맞춰 비율대로 나누고, 왼쪽·위
// 손잡이로 늘린 띄운 개체는 자리도 그만큼 옮긴다. 쪽을 다시 짜고 그 개체를 다시 고른다.
// 글자처럼 놓인 개체는 크기가 바뀌어도 제 문단의 줄만 바뀐다. 문서 전체를 다시 놓지 않고 그 문단부터 놓아
// 밀린 쪽만 다시 그린다. 떠 있는 개체는 다른 문단 글을 비켜 가게 하므로 전체를 다시 놓는다.
EditRelayoutResult paint_detail::relayoutObjectParagraph(model::Document& doc, std::vector<Node>& pages, int story,
                                                         int paragraph, bool useStoredLineHeight) {
    model::Paragraph* host = storyParagraph(doc, story, paragraph);
    if (host) host->lineSegs.clear();
    const int length = host ? paragraphCharCount(*host) : 0;
    EditRelayoutSpec spec{story, paragraph, 0, 0, length, length, useStoredLineHeight};
    spec.paraShapeChanged = true;
    return relayoutAfterEdit(doc, pages, spec);
}

EditResult resizeObjectNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    if (!hold.resizeObjectPending) return {};
    hold.resizeObjectPending = false;
    if (readOnly()) return {};
    model::Control* control = anchoredControl(doc, hold.resizeStory, hold.resizeParagraph, hold.resizeItem);
    if (!control || !control->placement) return {};
    model::Placement& placement = *control->placement;
    const std::uint32_t oldW = hold.resizeOldW;
    const std::uint32_t oldH = hold.resizeOldH;
    const std::uint32_t newW = hold.resizeNewW;
    const std::uint32_t newH = hold.resizeNewH;
    if (control->table) {
        model::Table& table = *control->table;
        const std::vector<std::int32_t> cols = scaledSizes(hold.resizeCols, static_cast<std::int32_t>(newW), 300);
        const std::vector<std::int32_t> rows = scaledSizes(hold.resizeRows, static_cast<std::int32_t>(newH), 282);
        const auto span = [](const std::vector<std::int32_t>& sizes, std::uint32_t from, std::uint32_t count) {
            std::int32_t total = 0;
            for (std::uint32_t i = from; i < from + std::max<std::uint32_t>(1, count) && i < sizes.size(); ++i) {
                total += sizes[i];
            }
            return total;
        };
        for (model::TableCell& cell : table.cells) {
            if (!cols.empty()) cell.width = static_cast<std::uint32_t>(std::max(1, span(cols, cell.col, cell.colSpan)));
            if (!rows.empty()) cell.height = static_cast<std::uint32_t>(std::max(1, span(rows, cell.row, cell.rowSpan)));
        }
        clearTableLines(table);
        placement.width = newW;
        placement.height = newH;
    } else {
        resizeControl(*control, oldW, oldH, newW, newH);
        for (model::SubList& list : control->subLists) {
            for (model::Paragraph& para : list.paragraphs) para.lineSegs.clear();
        }
    }
    // 왼쪽·위 손잡이로 늘렸으면 띄운 개체의 자리도 옮긴다.
    if (!placement.treatAsChar) {
        const std::int32_t dx = hold.resizeNewLeft - hold.resizeLeft;
        const std::int32_t dy = hold.resizeNewTop - hold.resizeTop;
        const auto step = [](const std::string& align, std::int32_t delta) {
            return align == "RIGHT" || align == "BOTTOM" || align == "OUTSIDE" ? -delta : delta;
        };
        placement.horzOffset += step(placement.horzAlign, dx);
        placement.vertOffset += step(placement.vertAlign, dy);
    }
    const bool inlineObject = placement.treatAsChar;
    if (std::vector<model::Paragraph>* paras = storyParagraphs(doc, hold.resizeStory); paras && !inlineObject) {
        const bool wraps = placement.textWrap == "SQUARE" || placement.textWrap == "TOP_AND_BOTTOM";
        if (wraps) {
            for (model::Paragraph& para : *paras) para.lineSegs.clear();
        }
    }
    const int story = hold.resizeStory;
    const int paragraph = hold.resizeParagraph;
    const int item = hold.resizeItem;
    const int page = hold.resizePage;
    hold.boxes.clear();
    hold.objectHandles = false;
    EditRelayoutResult relayout;
    if (inlineObject) {
        relayout = relayoutObjectParagraph(doc, pages, story, paragraph, useStoredLineHeight);
    } else {
        invalidateLinePlaces({}, true);
        model::ensureParagraphIds(doc);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
    }
    reselectAnchoredObject(doc, pages, page, story, paragraph, item);
    return makeEditResult(doc, true, true, story, paragraph, &relayout);
}

// 표 칸 경계선 끌기를 반영한다. 세로 안쪽선은 그 왼쪽 칸이 늘어난 만큼 오른쪽 칸이 줄어 표 폭이 그대로이고,
// 오른쪽 바깥선은 마지막 칸과 표 폭이 바뀐다. 가로선은 그 위 줄 높이가 바뀐다. 칸 크기는 그린 크기에서 잰다.
EditResult resizeTableBorderNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    if (!hold.borderPending) return {};
    hold.borderPending = false;
    const int kind = hold.borderDrag;
    hold.borderDrag = 0;
    if (readOnly() || kind == 0) return {};
    model::Control* control = anchoredControl(doc, hold.borderStory, hold.borderParagraph, hold.borderItem);
    if (!control || !control->table || !control->placement) return {};
    std::vector<std::int32_t> sizes = hold.borderSizes;
    const int index = hold.borderIndex;
    if (index < 0 || index >= static_cast<int>(sizes.size())) return {};
    sizes[static_cast<std::size_t>(index)] += hold.borderDelta;
    if (kind == 1 && !hold.borderOuter && index + 1 < static_cast<int>(sizes.size())) {
        sizes[static_cast<std::size_t>(index + 1)] -= hold.borderDelta;
    }
    model::Table& table = *control->table;
    std::int32_t total = 0;
    for (const std::int32_t size : sizes) total += size;
    for (model::TableCell& cell : table.cells) {
        const std::uint32_t from = kind == 1 ? cell.col : cell.row;
        const std::uint32_t count = std::max<std::uint32_t>(1, kind == 1 ? cell.colSpan : cell.rowSpan);
        std::int32_t span = 0;
        for (std::uint32_t i = from; i < from + count && i < sizes.size(); ++i) span += sizes[i];
        if (kind == 1) cell.width = static_cast<std::uint32_t>(std::max(1, span));
        else cell.height = static_cast<std::uint32_t>(std::max(1, span));
    }
    if (kind == 1) control->placement->width = static_cast<std::uint32_t>(std::max(1, total));
    else control->placement->height = static_cast<std::uint32_t>(std::max(1, total));
    clearTableLines(table);
    const bool inlineTable = control->placement->treatAsChar;
    if (std::vector<model::Paragraph>* paras = storyParagraphs(doc, hold.borderStory); paras && !inlineTable) {
        for (model::Paragraph& para : *paras) para.lineSegs.clear();
    }
    const PointerCursor cur = pointerCursor();
    int anchorChar = 0;
    int focusChar = 0;
    if (cur.active && cur.story > 0) {
        anchorChar = offsetToCharIndex(doc, pages, cur.story, cur.anchorParagraph, cur.anchorOffset);
        focusChar = offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
    }
    EditRelayoutResult relayout;
    if (inlineTable) {
        relayout = relayoutObjectParagraph(doc, pages, hold.borderStory, hold.borderParagraph, useStoredLineHeight);
    } else {
        invalidateLinePlaces({}, true);
        model::ensureParagraphIds(doc);
        pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
        relayout.fullInvalidation = true;
        relayout.pageListChanged = true;
    }
    if (cur.active && cur.story > 0) {
        setPointerCursor(doc, pages,
                         model::CursorPos{cur.story, cur.anchorParagraph,
                                          charIndexToOffset(doc, pages, cur.story, cur.anchorParagraph, anchorChar)},
                         model::CursorPos{cur.story, cur.paragraph,
                                          charIndexToOffset(doc, pages, cur.story, cur.paragraph, focusChar)});
    }
    return makeEditResult(doc, true, true, hold.borderStory, hold.borderParagraph, &relayout);
}

model::Control* paint_detail::anchoredControl(model::Document& doc, int story, int paragraph, int item) {
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, story);
    if (!paras || paragraph < 0 || paragraph >= static_cast<int>(paras->size()) || item < 0) return nullptr;
    int index = 0;
    for (model::Run& run : (*paras)[static_cast<std::size_t>(paragraph)].runs) {
        for (model::InlineItem& entry : run.items) {
            if (index++ == item) {
                return entry.type == model::InlineItem::Type::Control ? &entry.control : nullptr;
            }
        }
    }
    return nullptr;
}

EditResult insertDrawnShapeNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight) {
    PointerHold& hold = pointerHold();
    if (!hold.drawPending) return {};
    // 막힌 글꼴이 있으면 그린 자리를 남겨 두고 셸에 글꼴을 바꿀지 묻게 한다. 바꾸면 editSwapLicenseFonts 가 이어서 넣고,
    // 그만두면 dropPendingInsert 가 버린다.
    if (EditResult blocked; !readOnly() && blockedByLicenseFonts(doc, blocked)) return blocked;
    hold.drawPending = false;
    bumpPointerPicture();
    return insertShapeNow(doc, pages, hold.drawPendingTool, hold.drawPage, hold.drawX0, hold.drawY0, hold.drawX1,
                          hold.drawY1, useStoredLineHeight);
}

void paint_detail::dropPendingDrawnShape() {
    PointerHold& hold = pointerHold();
    if (!hold.drawPending) return;
    hold.drawPending = false;
    bumpPointerPicture();
}

int fillMissingLineSegs(model::Document& doc, const std::vector<Node>& pages) {
    ParagraphLayouts layouts;
    layouts.collect(pages);
    return fillMissingLineSegs(doc, layouts);
}

int fillMissingLineSegs(model::Document& doc, const ParagraphLayouts& layouts) {
    int filled = 0;
    model::forEachParagraph(doc, [&](model::Paragraph& para) {
        if (!para.lineSegs.empty()) return;
        const ParagraphLayout* layout = layouts.find(para.id);
        if (!layout || layout->lines.empty()) return;
        // 우리 줄 시작은 글자 번호(개체·각주 1칸)다. lineSeg 는 한글 단위(탭·개체 8칸, 확장 글자 2칸)라 바꾼다.
        const std::vector<CharSegEntry> table = buildCharSegTable(para);
        std::vector<model::LineSeg> segs;
        segs.reserve(layout->lines.size());
        for (std::size_t i = 0; i < layout->lines.size(); ++i) {
            model::LineSeg seg = layout->lines[i].seg;
            const auto at = static_cast<std::size_t>(seg.textPos);
            // 첫 줄은 앞에 붙은 구역·단 정의 컨트롤과 상관없이 0 에서 시작한다.
            seg.textPos = i == 0 ? 0u : (at < table.size() ? static_cast<std::uint32_t>(table[at].segPos) : seg.textPos);
            segs.push_back(seg);
        }
        para.lineSegs = std::move(segs);
        ++filled;
    });
    return filled;
}

// 한글 2024 표 1,000 단위 구분 쉼표(TableInsertComma·TableDeleteComma).
// 숫자·쉼표·점이 이어진 덩어리가 세 글자 이상이면 고친다. 빼기는 끝 글자가 아닌 쉼표와 둘째 점부터를 지우고,
// 넣기는 뺀 글의 정수 부분(첫 점·쉼표 앞)을 셋씩 끊어 쉼표를 넣는다. 소수 부분은 그대로다.
bool rewriteNumberCommas(model::Paragraph& para, bool insert) {
    const std::u32string chars = caretCharsOf(para);
    const auto numberChar = [](char32_t c) { return (c >= U'0' && c <= U'9') || c == U',' || c == U'.'; };
    struct Token {
        int start;
        int length;
    };
    std::vector<Token> tokens;
    const int len = static_cast<int>(chars.size());
    for (int i = 0; i < len;) {
        if (!numberChar(chars[static_cast<std::size_t>(i)])) {
            ++i;
            continue;
        }
        int j = i;
        while (j < len && numberChar(chars[static_cast<std::size_t>(j)])) ++j;
        if (j - i >= 3) tokens.push_back({i, j - i});
        i = j;
    }
    bool changed = false;
    // 뒤 덩어리부터 바꿔야 앞 덩어리 자리가 그대로다.
    for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
        const std::u32string token = chars.substr(static_cast<std::size_t>(it->start), static_cast<std::size_t>(it->length));
        std::u32string stripped;
        bool dot = false;
        for (std::size_t k = 0; k < token.size(); ++k) {
            const bool last = k + 1 == token.size();
            if (token[k] == U',' && !last) continue;
            if (token[k] == U'.') {
                if (dot && !last) continue;
                dot = true;
            }
            stripped.push_back(token[k]);
        }
        std::u32string result = stripped;
        if (insert) {
            std::size_t whole = 0;
            while (whole < stripped.size() && stripped[whole] != U'.' && stripped[whole] != U',') ++whole;
            int digits = static_cast<int>(whole);
            int group = digits % 3 == 0 ? 3 : digits % 3;
            result.clear();
            for (const char32_t c : stripped) {
                result.push_back(c);
                --group;
                --digits;
                if (group < 1) {
                    if (digits > 0) result.push_back(U',');
                    group = 3;
                }
            }
        }
        if (result == token) continue;
        std::string utf8;
        for (const char32_t c : result) appendUtf8(utf8, c);
        // 옛 덩어리 끝에 붙여 그 글자 모양을 잇고, 옛 덩어리를 지운다.
        insertTextInParagraph(para, it->start + it->length, utf8);
        eraseCharsInParagraph(para, it->start, it->start + it->length);
        changed = true;
    }
    if (changed) para.lineSegs.clear();
    return changed;
}

}  // namespace chwp::render
