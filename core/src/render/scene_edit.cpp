#include "chwp/render/scene.hpp"
#include "chwp/render/paint.hpp"

#include "render/line_breaker.hpp"
#include "render/scene_detail.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "export/font_metrics.hpp"
#include "export/layout.hpp"

// 편집 뒤 다시 놓기. 고친 문단만 다시 조판하고, 줄·표 칸·쪽을 되도록 그대로 둔다.
// 단·쪽을 넘는 변화는 scene.cpp 의 reflowEditedPages 가 맡는다.

namespace chwp::render {

using namespace detail;

namespace {

bool canIncrementalRelayoutParagraph(const Document& doc, const Paragraph& para, bool sameParagraphEdit = false,
                                     bool isFirstInColumn = false) {
    if (para.columnBreak || para.pageBreak || para.layoutBreak) {
        if (!sameParagraphEdit || !isFirstInColumn) return false;
    }
    if (paragraphStartsPage(para, doc)) {
        if (!sameParagraphEdit || !isFirstInColumn) return false;
    }
    for (const auto& run : para.runs) {
        for (const auto& item : run.items) {
            if (item.type != InlineItem::Type::Control) continue;
            if (item.control.kind == ControlKind::Footnote || item.control.kind == ControlKind::Endnote) {
                return false;
            }
            // 덧말·글자 겹치기는 글자 칸 개체다. 줄을 통째로 다시 짠다.
            if (glyphGroupControl(item.control)) return false;
            if (!sameParagraphEdit &&
                (item.control.kind == ControlKind::ColumnDef || item.control.kind == ControlKind::SectionDef)) {
                return false;
            }
            if (visualObject(item.control) && (!item.control.placement || !item.control.placement->treatAsChar)) {
                return false;
            }
        }
    }
    return true;
}

bool columnHasFloatingObjects(const Node& col) {
    for (const Node& child : col.children) {
        if (child.kind == NodeKind::Object && !child.treatAsChar) return true;
        if (child.kind != NodeKind::Paragraph) return true;
        for (const Node& pChild : child.children) {
            if (pChild.kind == NodeKind::Object && !pChild.treatAsChar) return true;
        }
    }
    return false;
}

bool paraLinesUseFullColumnWidth(const Node& paraNode) {
    for (const Node& child : paraNode.children) {
        if (child.kind != NodeKind::Line) continue;
        const std::int32_t expectedW = std::max<std::int32_t>(0, paraNode.width - child.x);
        if (std::abs(child.width - expectedW) > 2) return false;
    }
    return true;
}

int columnCountInBody(const Node& body) {
    int count = 0;
    for (const Node& child : body.children) {
        if (child.kind == NodeKind::Column) ++count;
    }
    return count;
}

bool bodyHasFloatingObjects(const Node& body, const Node& col) {
    if (columnHasFloatingObjects(col)) return true;
    for (const Node& child : body.children) {
        if (child.kind == NodeKind::Object && !child.treatAsChar) return true;
    }
    return false;
}

bool columnFloatingBlocksParagraphShift(const Node& col, int targetParaChildIdx) {
    if (targetParaChildIdx < 0 || targetParaChildIdx >= static_cast<int>(col.children.size())) return true;
    const std::int32_t targetParaY = col.children[static_cast<std::size_t>(targetParaChildIdx)].y;
    for (int i = 0; i < static_cast<int>(col.children.size()); ++i) {
        const Node& child = col.children[static_cast<std::size_t>(i)];
        if (child.kind == NodeKind::Object && !child.treatAsChar) {
            if (child.y + child.height > targetParaY) return true;
            continue;
        }
        if (child.kind != NodeKind::Paragraph) return true;
        if (i >= targetParaChildIdx && !paraLinesUseFullColumnWidth(child)) return true;
        for (const Node& pChild : child.children) {
            if (pChild.kind == NodeKind::Object && !pChild.treatAsChar) {
                if (i >= targetParaChildIdx || child.y + pChild.y + pChild.height > targetParaY) return true;
            }
            if (pChild.kind == NodeKind::Line) {
                for (const Node& lChild : pChild.children) {
                    if (lChild.kind == NodeKind::Object && !lChild.treatAsChar) {
                        if (i >= targetParaChildIdx ||
                            child.y + pChild.y + lChild.y + lChild.height > targetParaY) {
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

bool bodyFloatingBlocksParagraphShift(const Node& body, const Node& col, int targetParaChildIdx) {
    if (columnCountInBody(body) != 1) return true;
    if (columnFloatingBlocksParagraphShift(col, targetParaChildIdx)) return true;
    const Node& targetPara = col.children[static_cast<std::size_t>(targetParaChildIdx)];
    const std::int32_t targetParaYInBody = col.y + targetPara.y;
    for (const Node& child : body.children) {
        if (child.kind == NodeKind::Object && !child.treatAsChar) {
            if (child.paragraph >= targetPara.paragraph) return true;
            if (child.y + child.height > targetParaYInBody) return true;
        }
    }
    return false;
}

bool sameEdge(const BorderEdge& a, const BorderEdge& b) {
    return a.type == b.type && a.color == b.color && a.width == b.width && a.end == b.end;
}

// 개체 안쪽까지 같은지. 표 칸 경계만 옮기면 표 상자는 그대로라, 칸 자리·테두리·글까지 봐야 옛 노드를 쓸 수 있다.
bool sameNodeShape(const Node& a, const Node& b) {
    if (a.kind != b.kind || a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height ||
        a.text() != b.text() || a.children.size() != b.children.size() || a.ext().fillColor != b.ext().fillColor ||
        a.ext().gradColor != b.ext().gradColor || a.ext().hatchStyle != b.ext().hatchStyle || a.ext().hatchColor != b.ext().hatchColor) {
        return false;
    }
    if (!sameEdge(a.ext().borderLeft, b.ext().borderLeft) || !sameEdge(a.ext().borderRight, b.ext().borderRight) ||
        !sameEdge(a.ext().borderTop, b.ext().borderTop) || !sameEdge(a.ext().borderBottom, b.ext().borderBottom)) {
        return false;
    }
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!sameNodeShape(a.children[i], b.children[i])) return false;
    }
    return true;
}

bool sameLineChildVisual(const Node& a, const Node& b) {
    if (a.kind != b.kind) return false;
    if (a.kind == NodeKind::Object) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
               a.role == b.role && a.treatAsChar == b.treatAsChar && a.headingMark == b.headingMark &&
               a.ext().obj().binItemId == b.ext().obj().binItemId && a.ext().obj().script == b.ext().obj().script && sameNodeShape(a, b);
    }
    return false;
}

bool sameHeading(const HeadingRun* a, const HeadingRun* b) {
    if (!a || !b) return a == b;
    return a->text == b->text && a->advances == b->advances && a->x == b->x && a->width == b->width &&
           a->fontSize == b->fontSize && a->charShapeId == b->charShapeId && a->font == b->font &&
           a->color == b->color && a->ratio == b->ratio && a->relSize == b->relSize && a->bold == b->bold &&
           a->italic == b->italic && a->superscript == b->superscript && a->subscript == b->subscript;
}

bool sameGlyphs(const LineGlyphs& a, const LineGlyphs& b) {
    if (a.runs.size() != b.runs.size() || a.advances != b.advances || a.inks != b.inks) return false;
    if (!sameHeading(a.heading.get(), b.heading.get())) return false;
    for (std::size_t i = 0; i < a.runs.size(); ++i) {
        const LineRun& x = a.runs[i];
        const LineRun& y = b.runs[i];
        if (x.kind != y.kind || x.synthetic != y.synthetic || x.slot != y.slot || x.textBegin != y.textBegin ||
            x.textEnd != y.textEnd || x.advanceAt != y.advanceAt || x.advanceCount != y.advanceCount ||
            x.inkAt != y.inkAt || x.inkCount != y.inkCount || x.width != y.width || x.charShapeId != y.charShapeId ||
            x.style != y.style) {
            return false;
        }
    }
    return true;
}

bool sameLineVisual(const Node& a, const Node& b) {
    if (a.kind != NodeKind::Line || b.kind != NodeKind::Line) return false;
    if (a.paragraph != b.paragraph || a.story != b.story) return false;
    if (a.lineData().auditLine != b.lineData().auditLine || a.lineData().deferTabs != b.lineData().deferTabs || a.spansLater != b.spansLater) return false;
    if (a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height) return false;
    if (a.indent != b.indent || a.marginLeft != b.marginLeft || a.marginTop != b.marginTop) return false;
    if (a.fontSize != b.fontSize || a.lineData().textHeight != b.lineData().textHeight || a.lineData().lineBaseline != b.lineData().lineBaseline) return false;
    if (a.charShapeId != b.charShapeId || a.font != b.font) return false;
    if (a.lineData().lastLine != b.lineData().lastLine || a.lineData().lineBreak != b.lineData().lineBreak) return false;
    if (a.align != b.align || a.vertAlign != b.vertAlign || a.text() != b.text()) return false;
    if (!sameGlyphs(a.glyphs(), b.glyphs())) return false;
    if (a.children.size() != b.children.size()) return false;
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!sameLineChildVisual(a.children[i], b.children[i])) return false;
    }
    return true;
}

struct SingleParaLayoutResult {
    bool ok = false;
    Node paragraph;
    int firstDirtyLine = 0;
    int lastDirtyLine = -1;
};

SingleParaLayoutResult layoutSingleParagraphIncremental(const Document& doc, const Paragraph& para, int pi,
                                                        int sectionIndex, int story, int depth,
                                                        std::int32_t defaultTab, std::int32_t columnWidth,
                                                        std::int32_t paraY, Node* existingPara,
                                                        bool allowLineConvergence, int editStartOff,
                                                        int editOldEndOff, int editNewEndOff,
                                                        bool useStoredLineHeight = false,
                                                        bool isFirstInColumn = false) {
    SingleParaLayoutResult result;
    if (!canIncrementalRelayoutParagraph(doc, para, allowLineConvergence, isFirstInColumn)) return result;
    // 세로쓰기 구역 본문은 돌린 틀 좌표 그대로 다시 짠다. 글자 칸과 세운 개체는 조판과 같다(verticalAtom, uprightObject).
    int direction = 0;
    if (const model::StorySlot* own = resolveStory(doc, story);
        own && own->kind == model::StoryKind::Section && sectionIndex >= 0 &&
        sectionIndex < static_cast<int>(doc.sections.size())) {
        direction = doc.sections[static_cast<std::size_t>(sectionIndex)].textDirection;
    }
    if (existingPara && !existingPara->children.empty()) {
        const Node& firstChild = existingPara->children.front();
        if (firstChild.kind == NodeKind::Line && firstChild.start > 0) return result;
    }

    Node paragraph;
    paragraph.kind = NodeKind::Paragraph;
    paragraph.paragraph = pi;
    paragraph.paragraphId = para.id;
    paragraph.section = sectionIndex;
    paragraph.story = story;
    paragraph.x = 0;
    paragraph.y = paraY;
    if (existingPara) {
        paragraph.extMut().head = existingPara->ext().head;
        paragraph.extMut().headingType = existingPara->ext().headingType;
        paragraph.extMut().headingText = existingPara->ext().headingText;
    } else {
        HeadingState dummyHeadings{};
        applyHeading(paragraph, para, doc, dummyHeadings);
    }

    std::int32_t localY = 0;
    const ParaShape* paraShape = findParaShape(doc, para.paraShapeId);
    const ParagraphSpan span = paragraphSpan(columnWidth, paraShape);
    const std::int32_t marginLeft = span.marginLeft;
    const std::int32_t marginRight = paraShape ? paraShape->marginRight : 0;
    const std::int32_t contentWidth = span.width;
    paragraph.width = contentWidth;

    const std::int32_t indent = paraShape ? paraShape->indent : 0;
    const std::int32_t spacingPrev = paraShape ? paraShape->spacingPrev : 0;
    const std::int32_t spacingNext = paraShape ? paraShape->spacingNext : 0;
    const std::int32_t frameRight = contentWidth;
    paragraph.marginLeft = marginLeft;
    paragraph.marginRight = marginRight;
    paragraph.align = paraShape && !paraShape->align.empty() ? paraShape->align : "JUSTIFY";
    paragraph.vertAlign = paraShape && !paraShape->vertAlign.empty() ? paraShape->vertAlign : "BASELINE";
    applyParagraphBorder(paragraph, doc, paraShape);
    paragraph.indent = indent;
    paragraph.spacingPrev = spacingPrev;
    paragraph.spacingNext = spacingNext;
    if (spacingPrev > 0) localY += spacingPrev;

    bool firstLine = true;
    auto glyphShift = [&]() -> std::int32_t { return numberingPush(paragraph, doc, para); };
    auto lineLeftOf = [&]() {
        std::int32_t left = 0;
        if (firstLine && indent > 0) left += indent;
        if (!firstLine && indent < 0) left += -indent;
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

    AtomOptions atomOptions;
    atomOptions.lazyMetrics = true;
    atomOptions.sectionIndex = sectionIndex;
    std::vector<UnseenMark> unseen;
    atomOptions.unseen = &unseen;
    std::vector<Atom> atoms = buildAtoms(doc, para, paraShape, atomOptions);
    const std::vector<std::int32_t> tallest = tallestAfter(atoms);
    const TabRuler tabs(doc, paraShape, marginLeft, contentWidth, columnWidth, defaultTab);

    bool canConverge = allowLineConvergence && existingPara && !existingPara->children.empty() && !atoms.empty();
    if (canConverge) {
        for (const Node& oldL : existingPara->children) {
            if (oldL.kind != NodeKind::Line || oldL.lineData().auditLine || oldL.lineData().deferTabs || oldL.spansLater) {
                canConverge = false;
                break;
            }
        }
    }

    int startLineIdx = 0;
    int recalcFromLine = 0;
    std::size_t cursor = 0;
    if (canConverge) {
        const int oldCount = static_cast<int>(existingPara->children.size());
        for (int i = 0; i < oldCount; ++i) {
            const Node& oldL = existingPara->children[static_cast<std::size_t>(i)];
            if (editStartOff <= oldL.end || i + 1 == oldCount) {
                startLineIdx = i;
                break;
            }
        }
        recalcFromLine = std::max(0, startLineIdx - 1);
        if (recalcFromLine > 0) {
            const int targetStartPos = existingPara->children[static_cast<std::size_t>(recalcFromLine)].start;
            bool foundCursor = false;
            for (std::size_t aIdx = 0; aIdx < atoms.size(); ++aIdx) {
                if (atoms[aIdx].pos == targetStartPos) {
                    cursor = aIdx;
                    foundCursor = true;
                    break;
                }
            }
            if (foundCursor) {
                for (int i = 0; i < recalcFromLine; ++i) {
                    paragraph.children.push_back(existingPara->children[static_cast<std::size_t>(i)]);
                }
                localY = existingPara->children[static_cast<std::size_t>(recalcFromLine)].y;
                firstLine = false;
            } else {
                recalcFromLine = 0;
                cursor = 0;
            }
        }
    }

    if (atoms.empty()) {
        Node line;
        line.kind = NodeKind::Line;
        line.paragraph = pi;
        line.y = localY;
        line.x = lineLeftOf();
        line.indent = glyphShift();
        line.align = paragraph.align;
        line.vertAlign = paragraph.vertAlign;
        line.editContent().lastLine = true;
        line.width = std::max<std::int32_t>(0, frameRight - line.x);
        const int emptySize = emptyEm > 0 ? emptyEm : kEm;
        line.charShapeId = !para.runs.empty() ? para.runs.front().charShapeId : 0;
        line.fontSize = emptySize;
        line.editContent().textHeight = line.fontSize;
        line.height = defaultPitch;
        if (useStoredLineHeight) {
            line.editContent().storedSeg.spacing = std::max<std::int32_t>(0, line.height - line.marginTop - line.editContent().textHeight);
        }
        line.start = 0;
        line.end = 0;
        if (firstLine) attachHeading(line, paragraph, doc, para, true);
        paragraph.children.push_back(std::move(line));
        localY += paragraph.children.back().height;
    }

    bool converged = false;
    int convergedOldIdx = -1;
    const int deltaOff = editNewEndOff - editOldEndOff;
    // 편집 범위 밖의 글자처럼 취급한 개체는 옛 줄의 노드를 그대로 쓴다. 그림을 다시 읽지 않는다.
    auto reusedObject = [&](const Atom& atom) -> const Node* {
        if (!existingPara || !atom.control || !atom.control->placement) return nullptr;
        int oldPos = -1;
        if (atom.pos < editStartOff) oldPos = atom.pos;
        else if (atom.pos >= editNewEndOff) oldPos = atom.pos - deltaOff;
        if (oldPos < 0) return nullptr;
        const std::int32_t wantW = static_cast<std::int32_t>(atom.control->placement->width);
        const std::int32_t wantH = static_cast<std::int32_t>(atom.control->placement->height);
        const std::string& bin = atom.control->binItemId;
        for (const Node& oldLine : existingPara->children) {
            if (oldLine.kind != NodeKind::Line || oldPos < oldLine.start || oldPos >= oldLine.end) continue;
            const Node* found = nullptr;
            int hits = 0;
            for (const Node& ch : oldLine.children) {
                if (ch.kind != NodeKind::Object || !ch.treatAsChar || ch.headingMark) continue;
                if (ch.width != wantW || ch.height != wantH) continue;
                if (!bin.empty() && ch.ext().obj().binItemId != bin) continue;
                found = &ch;
                ++hits;
            }
            return hits == 1 ? found : nullptr;
        }
        return nullptr;
    };
    // 전체 조판(scene.cpp lineHost)처럼 개체에 제 문단 자리를 적는다. 표 경계선 끌기·개체 고르기가 이 값으로 찾는다.
    auto anchored = [&](Node object, const Atom& atom) {
        object.extMut().objMut().anchorStory = story;
        object.extMut().objMut().anchorParagraph = pi;
        object.extMut().objMut().anchorItem = atom.control ? itemOrdinal(para, atom.control) : -1;
        return object;
    };
    auto lineHost = makeLineHost(
        [&](Atom& atom) {
            measureLazyAtom(doc, atom);
            verticalAtom(doc, atom, direction);
        },
        [&](const Atom& atom, std::int32_t pen) {
            if (const Node* reused = reusedObject(atom)) {
                Node child = *reused;
                child.x = pen;
                return anchored(std::move(child), atom);
            }
            Node built = atomObject(atom, pen, doc, depth, defaultTab, useStoredLineHeight);
            return anchored(direction != 0 ? uprightObject(std::move(built)) : std::move(built), atom);
        });
    std::size_t guardCursor = cursor;
    int spins = 0;
    while (cursor < atoms.size()) {
        if (cursor != guardCursor) {
            guardCursor = cursor;
            spins = 0;
        } else if (++spins > 3) {
            measureLazyAtom(doc, atoms[cursor]);
            const Atom& atom = atoms[cursor];
            const int forcedPitch = pitchOf(atom.fontSize);
            if (!atom.hard) {
                Node object;
                if (atom.control) {
                    object = atomObject(atom, 0, doc, depth, defaultTab, useStoredLineHeight);
                    if (direction != 0) object = uprightObject(std::move(object));
                    object = anchored(std::move(object), atom);
                }
                Node line = singleAtomLine(doc, atom, std::move(object));
                line.paragraph = pi;
                line.editContent().vertical = static_cast<std::uint8_t>(direction);
                line.x = lineLeftOf();
                line.indent = glyphShift();
                line.y = localY;
                line.align = paragraph.align;
                line.vertAlign = paragraph.vertAlign;
                line.height = forcedPitch;
                line.editContent().textHeight = line.fontSize;
                placeInlineObjects(line, kind, spacingValue);
                if (useStoredLineHeight) {
                    line.editContent().storedSeg.spacing = std::max<std::int32_t>(0, line.height - line.marginTop - line.editContent().textHeight);
                }
                localY += line.height;
                if (firstLine) attachHeading(line, paragraph, doc, para, true);
                paragraph.children.push_back(std::move(line));
                firstLine = false;
            }
            ++cursor;
            guardCursor = cursor;
            spins = 0;
            continue;
        }
        const int lineEm = atoms[cursor].fontSize > 0 ? atoms[cursor].fontSize : kEm;
        LineSpec spec;
        spec.doc = &doc;
        spec.paraShape = paraShape;
        spec.tabs = &tabs;
        spec.vertical = direction != 0;
        spec.gaps.push_back({lineLeftOf(), frameRight});
        LineFill fill;
        fill.line.kind = NodeKind::Line;
        fill.line.editContent().autoSpacing = autoSpacingFlags(paraShape);
        fill.line.paragraph = pi;
        fill.line.y = localY;
        fill.line.vertAlign = paragraph.vertAlign;
        fill.line.charShapeId = atoms[cursor].charShapeId;
        fill.line.font = atoms[cursor].font;
        fill.line.fontSize = lineEm;
        const std::size_t lineStart = cursor;
        fillLine(spec, atoms, cursor, fill, lineHost);
        closeLine(fill);
        Node& line = fill.line;
        // 문단 끝 표시와 조판 부호의 글자 모양도 글자 상자에 넣는다. 조판과 같은 범위다.
        line.editContent().textHeight = std::max<std::int32_t>(
            line.lineData().textHeight,
            unseenSize(unseen, lineStart == 0 ? std::numeric_limits<int>::min() : atoms[lineStart].pos,
                       cursor < atoms.size() ? atoms[cursor].pos : std::numeric_limits<int>::max()));
        line.indent = glyphShift();
        line.align = paragraph.align;
        line.editContent().vertical = static_cast<std::uint8_t>(direction);
        line.width = std::max<std::int32_t>(0, frameRight - line.x);
        applyFontBaseline(line, paraShape);
        placeInlineObjects(line, kind, spacingValue);
        if (useStoredLineHeight) {
            line.editContent().storedSeg.spacing = std::max<std::int32_t>(0, line.height - line.marginTop - line.editContent().textHeight);
        }
        // 줄 높이(vertsize)는 조판과 같이 줄 시작부터 단 폭을 넘칠 때까지 잰다.
        line.editContent().measuredSeg.vertSize = scannedVertSize(doc, atoms, tallest, lineStart, cursor, lineTextBox(line),
                                                    lineStart == 0 ? numberingPush(paragraph, doc, para) : 0,
                                                    columnWidth);
        markInlineOverflow(line);
        if (firstLine) attachHeading(line, paragraph, doc, para, true);
        paragraph.children.push_back(std::move(line));
        localY += paragraph.children.back().height;
        firstLine = false;

        // 편집 뒤 줄 시작이 옛 줄 시작과 다시 맞으면 나머지 옛 줄을 그대로 붙인다.
        if (canConverge && cursor < atoms.size()) {
            const int nextPos = atoms[cursor].pos;
            const int builtLineIdx = static_cast<int>(paragraph.children.size()) - 1;
            if (builtLineIdx >= startLineIdx && nextPos >= editNewEndOff) {
                for (int oldIdx = std::max(1, startLineIdx + 1);
                     oldIdx < static_cast<int>(existingPara->children.size()); ++oldIdx) {
                    const Node& cand = existingPara->children[static_cast<std::size_t>(oldIdx)];
                    if (cand.start >= editOldEndOff && cand.start + deltaOff == nextPos) {
                        converged = true;
                        convergedOldIdx = oldIdx;
                        break;
                    }
                }
                if (converged) {
                    for (std::size_t rem = static_cast<std::size_t>(convergedOldIdx);
                         rem < existingPara->children.size(); ++rem) {
                        Node remLine = existingPara->children[rem];
                        remLine.y = localY;
                        shiftLineOffsets(remLine, deltaOff);
                        localY += remLine.height;
                        paragraph.children.push_back(std::move(remLine));
                    }
                    break;
                }
            }
        }
    }

    if (!converged && !atoms.empty() && atoms.back().hard) {
        Node line;
        line.kind = NodeKind::Line;
        line.paragraph = pi;
        line.y = localY;
        line.x = lineLeftOf();
        line.indent = glyphShift();
        line.align = paragraph.align;
        line.vertAlign = paragraph.vertAlign;
        line.editContent().lastLine = true;
        line.width = std::max<std::int32_t>(0, frameRight - line.x);
        const int emptySize = atoms.back().fontSize > 0 ? atoms.back().fontSize : (emptyEm > 0 ? emptyEm : kEm);
        line.fontSize = emptySize;
        line.editContent().textHeight = line.fontSize;
        line.height = pitchOf(line.fontSize);
        line.start = atoms.back().pos + 1;
        line.end = atoms.back().pos + 1;
        paragraph.children.push_back(std::move(line));
        localY += paragraph.children.back().height;
    }

    for (auto& child : paragraph.children) {
        if (child.kind == NodeKind::Line) child.editContent().lastLine = false;
    }
    for (auto it = paragraph.children.rbegin(); it != paragraph.children.rend(); ++it) {
        if (it->kind != NodeKind::Line) continue;
        it->editContent().lastLine = true;
        break;
    }
    if (spacingNext > 0) localY += spacingNext;
    paragraph.height = std::max(localY, 0);
    if (story > 0) stampStory(paragraph, story);

    result.ok = true;
    result.firstDirtyLine = 0;
    result.lastDirtyLine = -1;
    if (existingPara) {
        const int oldN = static_cast<int>(existingPara->children.size());
        const int newN = static_cast<int>(paragraph.children.size());
        int firstDiff = 0;
        while (firstDiff < oldN && firstDiff < newN &&
               sameLineVisual(existingPara->children[static_cast<std::size_t>(firstDiff)],
                              paragraph.children[static_cast<std::size_t>(firstDiff)])) {
            paragraph.children[static_cast<std::size_t>(firstDiff)].children =
                std::move(existingPara->children[static_cast<std::size_t>(firstDiff)].children);
            ++firstDiff;
        }
        result.firstDirtyLine = firstDiff;
        if (oldN == newN) {
            int lastDiff = newN - 1;
            while (lastDiff >= firstDiff &&
                   sameLineVisual(existingPara->children[static_cast<std::size_t>(lastDiff)],
                                  paragraph.children[static_cast<std::size_t>(lastDiff)])) {
                Node& kept = paragraph.children[static_cast<std::size_t>(lastDiff)];
                const int dOff = kept.start - existingPara->children[static_cast<std::size_t>(lastDiff)].start;
                kept.children = std::move(existingPara->children[static_cast<std::size_t>(lastDiff)].children);
                shiftSpanOffsets(kept, dOff);
                --lastDiff;
            }
            result.lastDirtyLine = lastDiff;
        } else {
            result.lastDirtyLine = -1;
        }
        evictNodeMeasures(*existingPara);
    }
    result.paragraph = std::move(paragraph);
    return result;
}

bool updateTableAfterCellEdit(Node& tableNode, int editedCellChildIdx, int pageIndex,
                              const std::vector<int>& pathToTable,
                              std::vector<FrameInvalidation>& invalidations,
                              std::int32_t& outTableHeightDelta) {
    outTableHeightDelta = 0;
    const bool isSplitTable = tableNode.ext().obj().splitTable;
    std::uint32_t rowCount = tableNode.ext().obj().fileRows;
    for (const Node& child : tableNode.children) {
        if (child.kind != NodeKind::Cell) continue;
        const std::uint32_t rEnd = child.ext().row + static_cast<std::uint32_t>(std::max(1, child.ext().rowSpan));
        if (rEnd > rowCount) rowCount = rEnd;
    }
    if (rowCount == 0) return false;

    std::vector<std::int32_t> rowH(rowCount, 0);
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

    struct CellRef {
        int childIdx = -1;
        Node* cell = nullptr;
        std::int32_t used = 0;
    };
    std::vector<CellRef> cellRefs;
    std::vector<CellRef> spanRefs;
    for (int i = 0; i < static_cast<int>(tableNode.children.size()); ++i) {
        Node& c = tableNode.children[static_cast<std::size_t>(i)];
        if (c.kind != NodeKind::Cell) continue;
        const int rSpan = std::max(1, static_cast<int>(c.ext().rowSpan));
        if (rSpan == 1 && c.ext().row >= 0 && static_cast<std::size_t>(c.ext().row) < rowH.size()) {
            rowH[c.ext().row] = std::max(rowH[c.ext().row], c.ext().obj().fileHeight);
        }
        const std::int32_t used = c.children.empty() ? 0 : rowContentBottom(c.children[0]);
        CellRef ref{i, &c, used};
        cellRefs.push_back(ref);
        if (rSpan == 1) {
            cover(rowH, c.ext().row, 1, c.marginTop + c.marginBottom + used);
        } else {
            spanRefs.push_back(ref);
        }
    }
    std::sort(spanRefs.begin(), spanRefs.end(), [](const CellRef& a, const CellRef& b) {
        return a.cell->ext().rowSpan < b.cell->ext().rowSpan;
    });
    for (const CellRef& ref : spanRefs) {
        const Node& c = *ref.cell;
        const int rSpan = std::max(1, static_cast<int>(c.ext().rowSpan));
        const std::int32_t need = std::max(c.ext().obj().fileHeight, c.marginTop + c.marginBottom + ref.used);
        cover(rowH, c.ext().row, rSpan, need);
    }

    const std::int32_t spacing = tableNode.ext().cellSpacing;
    std::int32_t tableH = 0;
    for (const std::int32_t h : rowH) tableH += h;
    if (!rowH.empty()) tableH += spacing * static_cast<std::int32_t>(rowH.size() + 1);
    const std::int32_t newTableH = std::max(tableNode.ext().obj().fileHeight, tableH);

    bool anyCellBoxChanged = (newTableH != tableNode.height);
    for (const CellRef& ref : cellRefs) {
        const Node& c = *ref.cell;
        const int rSpan = std::max(1, static_cast<int>(c.ext().rowSpan));
        std::int32_t cy = spacing * static_cast<std::int32_t>(c.ext().row + 1);
        for (int r = 0; r < c.ext().row && static_cast<std::size_t>(r) < rowH.size(); ++r) {
            cy += rowH[static_cast<std::size_t>(r)];
        }
        std::int32_t ch = 0;
        for (int i = 0; i < rSpan && c.ext().row + static_cast<std::size_t>(i) < rowH.size(); ++i) {
            ch += rowH[c.ext().row + static_cast<std::size_t>(i)];
        }
        if (rSpan > 1) ch += spacing * (rSpan - 1);
        if (cy != c.y || ch != c.height) {
            anyCellBoxChanged = true;
            break;
        }
    }

    if (!anyCellBoxChanged) {
        if (editedCellChildIdx >= 0 && editedCellChildIdx < static_cast<int>(tableNode.children.size())) {
            Node& editedCell = tableNode.children[static_cast<std::size_t>(editedCellChildIdx)];
            if (editedCell.kind == NodeKind::Cell && !editedCell.children.empty()) {
                Node& cellBody = editedCell.children[0];
                const std::int32_t used = rowContentBottom(cellBody);
                const std::int32_t innerH =
                    std::max<std::int32_t>(0, editedCell.height - editedCell.marginTop - editedCell.marginBottom);
                std::int32_t contentY = editedCell.marginTop;
                if (editedCell.vertAlign == "CENTER" && used < innerH) {
                    contentY = editedCell.marginTop + (innerH - used) / 2;
                } else if (editedCell.vertAlign == "BOTTOM" && used < innerH) {
                    contentY = editedCell.marginTop + (innerH - used);
                }
                if (contentY != cellBody.y) {
                    cellBody.y = contentY;
                    std::vector<int> cellBodyPath = pathToTable;
                    cellBodyPath.push_back(editedCellChildIdx);
                    cellBodyPath.push_back(0);
                    invalidations.push_back(FrameInvalidation{pageIndex, cellBodyPath, 0, -1, false});
                }
            }
        }
        return true;
    }

    if (isSplitTable) return false;

    for (const CellRef& ref : cellRefs) {
        Node& c = *ref.cell;
        const int rSpan = std::max(1, static_cast<int>(c.ext().rowSpan));
        std::int32_t cy = spacing * static_cast<std::int32_t>(c.ext().row + 1);
        for (int r = 0; r < c.ext().row && static_cast<std::size_t>(r) < rowH.size(); ++r) {
            cy += rowH[static_cast<std::size_t>(r)];
        }
        std::int32_t ch = 0;
        for (int i = 0; i < rSpan && c.ext().row + static_cast<std::size_t>(i) < rowH.size(); ++i) {
            ch += rowH[c.ext().row + static_cast<std::size_t>(i)];
        }
        if (rSpan > 1) ch += spacing * (rSpan - 1);
        const bool movedY = (cy != c.y);
        const bool changedH = (ch != c.height);
        c.y = cy;
        c.height = ch;
        c.extMut().objMut().spanRowSplits.clear();
        if (rSpan > 1 && spacing == 0) {
            std::int32_t acc = 0;
            for (int i = 0; i + 1 < rSpan && c.ext().row + static_cast<std::size_t>(i) < rowH.size(); ++i) {
                acc += rowH[c.ext().row + static_cast<std::size_t>(i)];
                if (acc > 0 && acc < ch) c.extMut().objMut().spanRowSplits.push_back(acc);
            }
        }
        bool innerShifted = false;
        if (!c.children.empty()) {
            Node& cellBody = c.children[0];
            const std::int32_t innerH = std::max<std::int32_t>(0, ch - c.marginTop - c.marginBottom);
            std::int32_t contentY = c.marginTop;
            if (c.vertAlign == "CENTER" && ref.used < innerH) {
                contentY = c.marginTop + (innerH - ref.used) / 2;
            } else if (c.vertAlign == "BOTTOM" && ref.used < innerH) {
                contentY = c.marginTop + (innerH - ref.used);
            }
            if (cellBody.y != contentY) innerShifted = true;
            cellBody.y = contentY;
        }
        if (movedY || innerShifted) {
            std::vector<int> cellPath = pathToTable;
            cellPath.push_back(ref.childIdx);
            invalidations.push_back(FrameInvalidation{pageIndex, std::move(cellPath), -1, -1, false});
        } else if (changedH) {
            std::vector<int> cellPath = pathToTable;
            cellPath.push_back(ref.childIdx);
            invalidations.push_back(FrameInvalidation{pageIndex, std::move(cellPath), -1, -1, true});
        }
    }

    const std::int32_t oldTableH = tableNode.height;
    outTableHeightDelta = newTableH - oldTableH;
    tableNode.height = newTableH;
    for (int ci = 0; ci < static_cast<int>(tableNode.children.size()); ++ci) {
        Node& child = tableNode.children[static_cast<std::size_t>(ci)];
        if (child.role == "caption" && child.y >= oldTableH && outTableHeightDelta != 0) {
            child.y += outTableHeightDelta;
            std::vector<int> capPath = pathToTable;
            capPath.push_back(ci);
            invalidations.push_back(FrameInvalidation{pageIndex, std::move(capPath), -1, -1, false});
        }
    }
    invalidations.push_back(FrameInvalidation{pageIndex, pathToTable, -1, -1, true});
    return true;
}

struct AncestorStep {
    Node* parent = nullptr;
    int childIndex = -1;
};

struct StoryColumnLoc {
    int pageIndex = -1;
    Node* column = nullptr;
    std::vector<AncestorStep> ancestors;
};

void findStoryColumns(Node& node, int pageIndex, int targetStory, std::vector<AncestorStep>& stack,
                      std::vector<StoryColumnLoc>& out) {
    if (node.kind == NodeKind::Column) {
        bool matches = (node.story == targetStory);
        if (!matches) {
            for (const Node& child : node.children) {
                if (child.kind == NodeKind::Paragraph && child.story == targetStory) {
                    matches = true;
                    break;
                }
            }
        }
        if (matches) {
            StoryColumnLoc loc;
            loc.pageIndex = pageIndex;
            loc.column = &node;
            loc.ancestors = stack;
            out.push_back(std::move(loc));
            return;
        }
    }
    if (node.kind == NodeKind::Paragraph || node.kind == NodeKind::Line) {
        for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
            Node& child = node.children[static_cast<std::size_t>(i)];
            if (child.kind != NodeKind::Line && child.kind != NodeKind::Object) continue;
            if (child.kind == NodeKind::Line) {
                bool hasObj = false;
                for (const Node& gc : child.children) {
                    if (gc.kind == NodeKind::Object) {
                        hasObj = true;
                        break;
                    }
                }
                if (!hasObj) continue;
            }
            stack.push_back(AncestorStep{&node, i});
            findStoryColumns(child, pageIndex, targetStory, stack, out);
            stack.pop_back();
        }
        return;
    }
    for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
        stack.push_back(AncestorStep{&node, i});
        findStoryColumns(node.children[static_cast<std::size_t>(i)], pageIndex, targetStory, stack, out);
        stack.pop_back();
    }
}

void shiftColumnParagraphIndices(Node& col, int delta) {
    if (delta == 0) return;
    for (Node& sib : col.children) {
        if (sib.kind == NodeKind::Paragraph) {
            sib.paragraph += delta;
            for (Node& line : sib.children) {
                if (line.kind == NodeKind::Line) {
                    line.paragraph = sib.paragraph;
                    for (Node& lc : line.children) {
                        if (lc.kind == NodeKind::Object) lc.paragraph = sib.paragraph;
                    }
                } else if (line.kind == NodeKind::Object) {
                    line.paragraph = sib.paragraph;
                }
            }
        } else if (sib.kind == NodeKind::Object) {
            sib.paragraph += delta;
        }
    }
}

bool nextColumnCanStayPutOnShrink(const Document& doc, const Node& curCol, const Node& nextCol,
                                  std::int32_t availHeight) {
    if (availHeight <= 0) return true;
    int curSection = curCol.section;
    for (const Node& p : curCol.children) {
        if (p.kind == NodeKind::Paragraph) {
            curSection = p.section;
            break;
        }
    }
    for (const Node& child : nextCol.children) {
        if (child.kind != NodeKind::Paragraph) return false;
        if (child.section != curSection) return true;
        const std::vector<model::Paragraph>* paras = storyParagraphs(doc, child.story);
        if (paras && child.paragraph >= 0 && child.paragraph < static_cast<int>(paras->size())) {
            const model::Paragraph& mp = (*paras)[static_cast<std::size_t>(child.paragraph)];
            if (mp.pageBreak || mp.columnBreak || mp.layoutBreak || paragraphStartsPage(mp, doc)) {
                return true;
            }
        }
        for (const Node& line : child.children) {
            if (line.kind != NodeKind::Line) continue;
            const std::int32_t needed = (line.start > 0 ? 0 : child.spacingPrev) + line.y + lineCharBox(line);
            return needed > availHeight;
        }
        return false;
    }
    return true;
}

std::vector<int> buildPathPrefix(const std::vector<AncestorStep>& ancestors, std::size_t count) {
    std::vector<int> path;
    const std::size_t limit = std::min(count, ancestors.size());
    path.reserve(limit);
    for (std::size_t i = 0; i < limit; ++i) {
        path.push_back(ancestors[i].childIndex);
    }
    return path;
}

bool flowFence(const Document& doc, const Node& paraNode) {
    if (paraNode.kind != NodeKind::Paragraph) return false;
    const model::Paragraph* model = storyParagraph(doc, paraNode.story, paraNode.paragraph);
    if (!model) return false;
    return model->pageBreak || model->columnBreak || model->layoutBreak || paragraphStartsPage(*model, doc);
}

// 줄 글자 표를 글자 하나씩 편 것. 편집 빠른 길이 고칠 자리를 찾고 표를 다시 짓는 데 쓴다.
struct LineUnit {
    bool object = false;
    int run = -1;  // 이 글자·개체가 든 구간 번호
    char32_t cp = 0;
    std::int32_t adv = 0;
    std::int32_t ink = 0;
    std::int32_t glyphBefore = 0;  // 앞 글자 폭 합(개체는 빼고)
};

// 머리 번호·탭·줄바꿈 없이 글자와 글자처럼 놓인 개체만 있는 줄을 편다. 글자마다 폭이 있어야 한다.
// 폭을 따로 들지 않는 구간은 글자 하나일 때만 받고, needWidth 면 그 폭이 0보다 커야 한다.
// pen 은 글자 폭과 개체 폭(여백 포함)의 합이다.
bool lineUnits(const Node& line, bool needWidth, std::vector<LineUnit>& units, std::int32_t& pen) {
    if (line.glyphs().heading) return false;
    for (const Node& child : line.children) {
        if (child.headingMark) return false;
    }
    const LineGlyphs& glyphs = line.glyphs();
    std::int32_t glyphRun = 0;
    for (int ri = 0; ri < static_cast<int>(glyphs.runs.size()); ++ri) {
        const LineRun& run = glyphs.runs[static_cast<std::size_t>(ri)];
        if (run.kind == RunKind::Object) {
            if (run.slot >= line.children.size()) return false;
            const Node& obj = line.children[run.slot];
            if (!obj.treatAsChar) return false;
            LineUnit unit;
            unit.object = true;
            unit.run = ri;
            unit.adv = obj.width + obj.marginLeft + obj.marginRight;
            unit.glyphBefore = glyphRun;
            units.push_back(unit);
            pen += unit.adv;
            continue;
        }
        if (run.kind == RunKind::Tab) return false;
        const std::size_t first = units.size();
        std::size_t idx = run.textBegin;
        char32_t cp = 0;
        while (idx < run.textEnd && nextCodePoint(line.text(), idx, cp)) {
            if (cp == '\n' || cp == '\t') return false;
            LineUnit unit;
            unit.run = ri;
            unit.cp = cp;
            units.push_back(unit);
        }
        const std::size_t count = units.size() - first;
        if (count == 0) continue;
        if (run.advanceCount != 0 && run.advanceCount != count) return false;
        if (run.advanceCount == 0 && (count != 1 || (needWidth && run.width <= 0))) return false;
        for (std::size_t g = 0; g < count; ++g) {
            LineUnit& unit = units[first + g];
            unit.adv = run.advanceCount == 0 ? run.width : glyphs.advances[run.advanceAt + g];
            unit.ink = g < run.inkCount ? glyphs.inks[run.inkAt + g] : unit.adv;
            unit.glyphBefore = glyphRun;
            glyphRun += unit.adv;
            pen += unit.adv;
        }
    }
    return true;
}

// 글자 크기가 그대로이고, 줄에 있는 폭 합에 새 글자 폭을 더해도 줄 너비 안이면 조판하지 않는다.
// 넘치면 저장된 폭으로만 줄을 나누고, 쪽 나눔·단 나눔 뒤는 다시 나누지 않는다.
// 1 이면 줄 상자가 그대로다. 2 이면 줄이 늘었다. 0 이면 이 길이 아니다.
int placeSameSizeGlyph(const Document& doc, const Paragraph& para, const EditRelayoutSpec& spec, Node& targetLine,
                       const Node* nextLine) {
    if (targetLine.lineData().auditLine || targetLine.lineData().deferTabs || targetLine.spansLater) return 0;
    if (targetLine.width <= 0) return 0;
    if (spec.editStartOff < targetLine.start || spec.editOldEndOff > targetLine.end) return 0;
    if (spec.editNewEndOff < spec.editStartOff) return 0;
    if (nextLine && !targetLine.lineData().lineBreak && spec.editNewEndOff < spec.editOldEndOff) return 0;

    std::vector<LineUnit> glyphs;
    std::int32_t used = 0;
    if (!lineUnits(targetLine, true, glyphs, used)) return 0;
    if (static_cast<int>(glyphs.size()) != targetLine.end - targetLine.start) return 0;
    const int localStart = spec.editStartOff - targetLine.start;
    const int localOldEnd = spec.editOldEndOff - targetLine.start;
    if (localStart < 0 || localOldEnd < localStart || localOldEnd > static_cast<int>(glyphs.size())) return 0;
    for (int i = localStart; i < localOldEnd; ++i) {
        if (glyphs[static_cast<std::size_t>(i)].object) return 0;
    }

    int hostRun = -1;
    if (localStart < static_cast<int>(glyphs.size()) && !glyphs[static_cast<std::size_t>(localStart)].object) {
        hostRun = glyphs[static_cast<std::size_t>(localStart)].run;
    } else {
        for (int i = localStart - 1; i >= 0; --i) {
            if (!glyphs[static_cast<std::size_t>(i)].object) {
                hostRun = glyphs[static_cast<std::size_t>(i)].run;
                break;
            }
        }
    }
    if (hostRun < 0) return 0;
    const LineRun& host = targetLine.glyphs().runs[static_cast<std::size_t>(hostRun)];
    const int freshCount = spec.editNewEndOff - spec.editStartOff;
    struct Fresh {
        char32_t cp = 0;
        std::int32_t adv = 0;
        std::int32_t ink = 0;
    };
    std::vector<Fresh> fresh;
    if (freshCount > 0) {
        std::uint32_t shapeId = host.charShapeId;
        int pos = 0;
        bool saw = false;
        for (const auto& run : para.runs) {
            for (const auto& item : run.items) {
                if (item.type == InlineItem::Type::Text) {
                    std::size_t idx = 0;
                    char32_t cp = 0;
                    while (nextCodePoint(item.text, idx, cp)) {
                        if (pos >= spec.editStartOff && pos < spec.editNewEndOff) {
                            if (cp == '\n' || cp == '\t' || run.charShapeId != shapeId) return 0;
                            fresh.push_back(Fresh{cp, 0, 0});
                            saw = true;
                        }
                        ++pos;
                    }
                } else if ((visualObject(item.control) && item.control.placement &&
                            item.control.placement->treatAsChar) ||
                           (item.type == InlineItem::Type::Control && glyphGroupControl(item.control))) {
                    if (pos >= spec.editStartOff && pos < spec.editNewEndOff) return 0;
                    ++pos;
                } else if (item.type == InlineItem::Type::Control) {
                    if (pos >= spec.editStartOff && pos < spec.editNewEndOff) return 0;
                    if (item.control.kind == ControlKind::Footnote || item.control.kind == ControlKind::Endnote) ++pos;
                }
            }
        }
        if (!saw || static_cast<int>(fresh.size()) != freshCount) return 0;
        const CharShape* shape = findCharShape(doc, shapeId);
        CharShape fallback;
        fallback.height = kEm;
        fallback.ratio.fill(100);
        if (!shape) shape = &fallback;
        const int em = shape->height > 0 ? shape->height : static_cast<int>(kEm);
        int have = host.style->fontSize > 0 ? host.style->fontSize : targetLine.fontSize;
        if (have <= 0) have = targetLine.lineData().textHeight;
        if (have <= 0 || em != have) return 0;
        const std::string* hostFont = host.style->font;
        for (Fresh& glyph : fresh) {
            const GlyphFace drawn = glyphFace(doc, *shape, glyph.cp, true, true);
            if (hostFont && !hostFont->empty() && drawn.font != *hostFont) return 0;
            const std::int32_t width = glyphWidth(doc, glyph.cp, *shape);
            const Lang trailLang = isHangul(glyph.cp) ? Lang::Hangul : Lang::Latin;
            const std::int32_t trail = layout::glyphTrail(glyph.cp, *shape, trailLang, &doc);
            glyph.adv = width;
            glyph.ink = width - trail;
        }
    }

    std::int32_t removed = 0;
    for (int i = localStart; i < localOldEnd; ++i) removed += glyphs[static_cast<std::size_t>(i)].adv;
    std::int32_t added = 0;
    for (const Fresh& glyph : fresh) added += glyph.adv;
    std::int32_t lastTrail = 0;
    if (localOldEnd < static_cast<int>(glyphs.size())) {
        const LineUnit& tail = glyphs.back();
        if (!tail.object) lastTrail = tail.adv - tail.ink;
    } else if (!fresh.empty()) {
        lastTrail = fresh.back().adv - fresh.back().ink;
    } else if (localStart > 0) {
        const LineUnit& tail = glyphs[static_cast<std::size_t>(localStart - 1)];
        if (!tail.object) lastTrail = tail.adv - tail.ink;
    }
    const std::int32_t nextPen = used - removed + added;
    if (nextPen - lastTrail > targetLine.width + kSlack) return 0;

    int spanBegin = -1;
    int spanCount = 0;
    for (int i = 0; i < static_cast<int>(glyphs.size()); ++i) {
        if (glyphs[static_cast<std::size_t>(i)].run != hostRun || glyphs[static_cast<std::size_t>(i)].object) continue;
        if (spanBegin < 0) spanBegin = i;
        ++spanCount;
    }
    if (spanBegin < 0 || localStart < spanBegin || localOldEnd > spanBegin + spanCount) return 0;

    // 고친 구간만 새 글자로 바꾸고 나머지 구간은 그대로 옮겨 표를 다시 짓는다.
    const LineGlyphs& old = targetLine.glyphs();
    LineGlyphs next;
    next.heading = old.heading;
    std::string text;
    text.reserve(targetLine.text().size() + fresh.size() * 3);
    for (int ri = 0; ri < static_cast<int>(old.runs.size()); ++ri) {
        LineRun run = old.runs[static_cast<std::size_t>(ri)];
        const std::size_t at = text.size();
        if (ri != hostRun) {
            text.append(targetLine.text(), run.textBegin, run.textEnd - run.textBegin);
            run.textBegin = static_cast<std::uint16_t>(at);
            run.textEnd = static_cast<std::uint16_t>(text.size());
            appendLineRun(next, run, old.advances.data() + run.advanceAt, run.advanceCount,
                          old.inks.data() + run.inkAt, run.inkCount);
            continue;
        }
        std::vector<std::int32_t> advs;
        std::vector<std::int32_t> inks;
        auto pushOld = [&](const LineUnit& unit) {
            appendUtf8(text, unit.cp);
            advs.push_back(unit.adv);
            inks.push_back(unit.ink);
        };
        for (int i = spanBegin; i < localStart; ++i) pushOld(glyphs[static_cast<std::size_t>(i)]);
        for (const Fresh& glyph : fresh) {
            appendUtf8(text, glyph.cp);
            advs.push_back(glyph.adv);
            inks.push_back(glyph.ink);
        }
        for (int i = localOldEnd; i < spanBegin + spanCount; ++i) pushOld(glyphs[static_cast<std::size_t>(i)]);
        run.textBegin = static_cast<std::uint16_t>(at);
        run.textEnd = static_cast<std::uint16_t>(text.size());
        run.width = 0;
        for (const std::int32_t adv : advs) run.width += adv;
        appendLineRun(next, run, advs.data(), advs.size(), inks.data(), inks.size());
    }
    const std::int32_t dx = added - removed;
    if (dx != 0) {
        for (int i = localOldEnd; i < static_cast<int>(glyphs.size()); ++i) {
            const LineUnit& glyph = glyphs[static_cast<std::size_t>(i)];
            if (!glyph.object) continue;
            targetLine.children[old.runs[static_cast<std::size_t>(glyph.run)].slot].x += dx;
        }
    }
    evictLineGlyph(targetLine);
    trimLineGlyphs(next);
    targetLine.glyphsMut() = std::move(next);
    targetLine.textMut() = std::move(text);
    targetLine.end += spec.editNewEndOff - spec.editOldEndOff;
    targetLine.contentHash = 0;
    targetLine.huBoundsReady = false;
    return 1;
}

// 한 글자는 그 줄의 폭만 바꾼다. 같은 글꼴이면 새 글자만 재고, 그림 개체는 그대로 둔다.
bool spliceEditedLine(const Document& doc, const Paragraph& para, const EditRelayoutSpec& spec, Node& targetLine,
                      const Node* nextLine) {
    if (targetLine.lineData().auditLine || targetLine.lineData().deferTabs || targetLine.spansLater) return false;
    if (spec.editStartOff < targetLine.start || spec.editOldEndOff > targetLine.end) return false;
    if (spec.editNewEndOff < spec.editStartOff) return false;
    const bool shrunk = spec.editNewEndOff < spec.editOldEndOff;
    if (nextLine && !targetLine.lineData().lineBreak && shrunk) return false;

    std::vector<LineUnit> units;
    std::int32_t oldPen = 0;
    if (!lineUnits(targetLine, false, units, oldPen)) return false;
    for (const Node& obj : targetLine.children) {
        for (const Node& cap : obj.children) {
            if (cap.role == "caption" && cap.x < 0) return false;
        }
    }
    if (static_cast<int>(units.size()) != targetLine.end - targetLine.start) return false;
    const int localStart = spec.editStartOff - targetLine.start;
    const int localOldEnd = spec.editOldEndOff - targetLine.start;
    if (localStart < 0 || localOldEnd < localStart || localOldEnd > static_cast<int>(units.size())) return false;
    for (int i = localStart; i < localOldEnd; ++i) {
        if (units[static_cast<std::size_t>(i)].object) return false;
    }

    struct Fresh {
        char32_t cp = 0;
        std::int32_t adv = 0;
        std::int32_t ink = 0;
    };
    std::vector<Fresh> fresh;
    int hostRun = -1;
    if (localStart < static_cast<int>(units.size()) && !units[static_cast<std::size_t>(localStart)].object) {
        hostRun = units[static_cast<std::size_t>(localStart)].run;
    } else {
        for (int i = localStart - 1; i >= 0; --i) {
            if (!units[static_cast<std::size_t>(i)].object) {
                hostRun = units[static_cast<std::size_t>(i)].run;
                break;
            }
        }
        if (hostRun < 0) {
            for (int i = localStart; i < static_cast<int>(units.size()); ++i) {
                if (!units[static_cast<std::size_t>(i)].object) {
                    hostRun = units[static_cast<std::size_t>(i)].run;
                    break;
                }
            }
        }
    }
    const LineGlyphs& old = targetLine.glyphs();
    const LineRun* host = hostRun >= 0 ? &old.runs[static_cast<std::size_t>(hostRun)] : nullptr;
    // 줄에 글자가 없으면 새 글자는 제 꼴로 새 구간을 연다.
    LineRun freshRun;
    const int freshCount = spec.editNewEndOff - spec.editStartOff;
    if (freshCount > 0) {
        std::uint32_t shapeId = host ? host->charShapeId : 0;
        bool shapeSet = host != nullptr;
        int pos = 0;
        for (const auto& run : para.runs) {
            for (const auto& item : run.items) {
                if (item.type == InlineItem::Type::Text) {
                    std::size_t idx = 0;
                    char32_t cp = 0;
                    while (nextCodePoint(item.text, idx, cp)) {
                        if (pos >= spec.editStartOff && pos < spec.editNewEndOff) {
                            if (cp == '\n' || cp == '\t') return false;
                            if (!shapeSet) {
                                shapeId = run.charShapeId;
                                shapeSet = true;
                            } else if (run.charShapeId != shapeId) {
                                return false;
                            }
                            fresh.push_back(Fresh{cp, 0, 0});
                        }
                        ++pos;
                    }
                } else if ((visualObject(item.control) && item.control.placement &&
                            item.control.placement->treatAsChar) ||
                           (item.type == InlineItem::Type::Control && glyphGroupControl(item.control))) {
                    if (pos >= spec.editStartOff && pos < spec.editNewEndOff) return false;
                    ++pos;
                } else if (item.type == InlineItem::Type::Control && occupiesBodyLine(item.control)) {
                    if (pos >= spec.editStartOff && pos < spec.editNewEndOff) return false;
                    const bool note = item.control.kind == ControlKind::Footnote ||
                                      item.control.kind == ControlKind::Endnote;
                    if (note) ++pos;
                } else if (item.type == InlineItem::Type::Control && pos >= spec.editStartOff &&
                           pos < spec.editNewEndOff) {
                    return false;
                }
            }
        }
        if (static_cast<int>(fresh.size()) != freshCount || !shapeSet) return false;
        const CharShape* shape = findCharShape(doc, shapeId);
        CharShape fallback;
        fallback.height = kEm;
        fallback.ratio.fill(100);
        if (!shape) shape = &fallback;
        const int fontSize = shape->height > 0 ? shape->height : kEm;
        if (host && host->style->fontSize > 0 && host->style->fontSize != fontSize) return false;
        const std::string* hostFont = host ? host->style->font : nullptr;
        bool styled = false;
        for (Fresh& glyph : fresh) {
            const GlyphFace drawn = glyphFace(doc, *shape, glyph.cp, true, true);
            if (hostFont && !hostFont->empty() && drawn.font != *hostFont) return false;
            const std::int32_t width = glyphWidth(doc, glyph.cp, *shape);
            const Lang trailLang = isHangul(glyph.cp) ? Lang::Hangul : Lang::Latin;
            const std::int32_t trail = layout::glyphTrail(glyph.cp, *shape, trailLang, &doc);
            glyph.adv = width;
            glyph.ink = width - trail;
            if (!host && !styled) {
                const Lang offsetLang = isHangul(glyph.cp) ? Lang::Hangul : Lang::Latin;
                const std::size_t offsetSlot = static_cast<std::size_t>(offsetLang);
                const std::int32_t offset = offsetSlot < shape->offset.size() ? shape->offset[offsetSlot] : 0;
                freshRun.charShapeId = shapeId;
                freshRun.style = runStyleOf(fontSize, drawn.font, std::string{}, std::string{}, drawn.ratio,
                                            drawn.relSize, drawn.spacing, offset);
                styled = true;
            }
        }
    }

    std::int32_t replaced = 0;
    for (int i = localStart; i < localOldEnd; ++i) replaced += units[static_cast<std::size_t>(i)].adv;
    std::int32_t added = 0;
    for (const Fresh& glyph : fresh) added += glyph.adv;

    // 글자를 차례로 놓아 표를 다시 짓는다. 같은 원본 구간에서 온 글자가 이어지면 한 구간으로 묶는다.
    LineGlyphs built;
    std::string lineText;
    std::vector<std::pair<std::uint16_t, std::int32_t>> moves;  // 개체 자식 번호와 옮길 폭
    std::int32_t lastTrail = 0;
    std::int32_t emitted = 0;
    int openKey = -999;
    LineRun open;
    std::vector<std::int32_t> openAdvs;
    std::vector<std::int32_t> openInks;
    auto flush = [&]() {
        if (openKey == -999) return;
        open.textEnd = static_cast<std::uint16_t>(lineText.size());
        appendLineRun(built, open, openAdvs.data(), openAdvs.size(), openInks.data(), openInks.size());
        openAdvs.clear();
        openInks.clear();
        openKey = -999;
    };
    auto addGlyph = [&](int key, char32_t cp, std::int32_t adv, std::int32_t ink) {
        if (openKey != key) {
            flush();
            openKey = key;
            open = key >= 0 ? old.runs[static_cast<std::size_t>(key)] : freshRun;
            open.textBegin = static_cast<std::uint16_t>(lineText.size());
            open.width = 0;
        }
        appendUtf8(lineText, cp);
        openAdvs.push_back(adv);
        openInks.push_back(ink);
        open.width += adv;
        emitted += adv;
        lastTrail = adv - ink;
    };
    auto emitUnit = [&](const LineUnit& unit) {
        if (unit.object) {
            flush();
            LineRun run = old.runs[static_cast<std::size_t>(unit.run)];
            run.textBegin = static_cast<std::uint16_t>(lineText.size());
            run.textEnd = run.textBegin;
            appendLineRun(built, run);
            moves.emplace_back(run.slot, emitted - unit.glyphBefore);
            lastTrail = 0;
            return;
        }
        addGlyph(unit.run, unit.cp, unit.adv, unit.ink);
    };
    for (int i = 0; i < localStart; ++i) emitUnit(units[static_cast<std::size_t>(i)]);
    for (const Fresh& glyph : fresh) addGlyph(hostRun, glyph.cp, glyph.adv, glyph.ink);
    for (int i = localOldEnd; i < static_cast<int>(units.size()); ++i) emitUnit(units[static_cast<std::size_t>(i)]);
    flush();
    if (built.runs.empty()) return false;
    const std::int32_t newPen = oldPen - replaced + added;
    if (newPen - lastTrail > targetLine.width + kSlack) return false;

    evictNodeMeasures(targetLine);
    for (const auto& [slot, dx] : moves) {
        Node& obj = targetLine.children[slot];
        obj.x += dx;
        obj.huBoundsReady = false;
    }
    trimLineGlyphs(built);
    targetLine.glyphsMut() = std::move(built);
    targetLine.textMut() = std::move(lineText);
    targetLine.end += spec.editNewEndOff - spec.editOldEndOff;
    targetLine.contentHash = 0;
    targetLine.huBoundsReady = false;
    return true;
}

bool tryUpdateSingleLineInPlace(const Document& doc, const Paragraph& para, const EditRelayoutSpec& spec,
                                std::vector<StoryColumnLoc>& storyCols, std::vector<Node>& pages,
                                EditRelayoutResult& out) {
    if (spec.paraDelta != 0 || storyCols.empty()) return false;
    struct ParaLineLoc {
        int colListIdx = -1;
        int paraChildIdx = -1;
        int lineChildIdx = -1;
        Node* line = nullptr;
    };
    std::vector<ParaLineLoc> paraLines;
    for (int cIdx = 0; cIdx < static_cast<int>(storyCols.size()); ++cIdx) {
        StoryColumnLoc& loc = storyCols[static_cast<std::size_t>(cIdx)];
        if (loc.pageIndex >= 0 && loc.pageIndex < static_cast<int>(pages.size()) &&
            pages[static_cast<std::size_t>(loc.pageIndex)].spansLater) {
            ensurePageSpans(doc, pages[static_cast<std::size_t>(loc.pageIndex)]);
        }
        Node& col = *loc.column;
        for (int pIdx = 0; pIdx < static_cast<int>(col.children.size()); ++pIdx) {
            Node& pNode = col.children[static_cast<std::size_t>(pIdx)];
            if (pNode.kind != NodeKind::Paragraph || pNode.paragraph != spec.paraIndex) continue;
            for (int lIdx = 0; lIdx < static_cast<int>(pNode.children.size()); ++lIdx) {
                Node& lNode = pNode.children[static_cast<std::size_t>(lIdx)];
                if (lNode.kind != NodeKind::Line) continue;
                if (lNode.lineData().auditLine || lNode.lineData().deferTabs || lNode.spansLater) return false;
                paraLines.push_back(ParaLineLoc{cIdx, pIdx, lIdx, &lNode});
            }
        }
    }
    if (paraLines.empty()) return false;

    int targetIdx = -1;
    for (int i = 0; i < static_cast<int>(paraLines.size()); ++i) {
        const Node& lNode = *paraLines[static_cast<std::size_t>(i)].line;
        if (lNode.children.empty() && lNode.start == lNode.end && i + 1 < static_cast<int>(paraLines.size())) {
            continue;
        }
        if (spec.editStartOff >= lNode.start && spec.editOldEndOff <= lNode.end) {
            targetIdx = i;
            break;
        }
    }
    if (targetIdx < 0) return false;

    const ParaLineLoc& located = paraLines[static_cast<std::size_t>(targetIdx)];
    StoryColumnLoc& locatedCol = storyCols[static_cast<std::size_t>(located.colListIdx)];
    Node& targetLine = *located.line;
    const Node* nextForSplice = nullptr;
    if (targetIdx + 1 < static_cast<int>(paraLines.size())) {
        nextForSplice = paraLines[static_cast<std::size_t>(targetIdx + 1)].line;
    }
    const int placed = placeSameSizeGlyph(doc, para, spec, targetLine, nextForSplice);
    if (placed == 1 || placed == 2) {
        const int deltaOff = spec.editNewEndOff - spec.editOldEndOff;
        if (placed == 1 && deltaOff != 0) {
            for (int i = targetIdx + 1; i < static_cast<int>(paraLines.size()); ++i) {
                shiftLineOffsets(*paraLines[static_cast<std::size_t>(i)].line, deltaOff);
            }
        }
        if (placed == 1) {
            out.invalidations.push_back(FrameInvalidation{locatedCol.pageIndex, {}, -1, -1, false});
        } else {
            out.invalidations.push_back(FrameInvalidation{locatedCol.pageIndex, {}, -1, -1, false});
            const int nextPage = locatedCol.pageIndex + 1;
            if (nextPage < static_cast<int>(pages.size()) &&
                pages[static_cast<std::size_t>(nextPage)].section == pages[static_cast<std::size_t>(locatedCol.pageIndex)].section) {
                out.invalidations.push_back(FrameInvalidation{nextPage, {}, -1, -1, false});
            }
        }
        out.incremental = true;
        out.fullInvalidation = false;
        return true;
    }
    if (spliceEditedLine(doc, para, spec, targetLine, nextForSplice)) {
        if (spec.story > 0) stampStory(targetLine, spec.story);
        const int deltaOff = spec.editNewEndOff - spec.editOldEndOff;
        if (deltaOff != 0) {
            for (int i = targetIdx + 1; i < static_cast<int>(paraLines.size()); ++i) {
                shiftLineOffsets(*paraLines[static_cast<std::size_t>(i)].line, deltaOff);
            }
        }
        const ParaLineLoc& tLoc = paraLines[static_cast<std::size_t>(targetIdx)];
        const StoryColumnLoc& colLoc = storyCols[static_cast<std::size_t>(tLoc.colListIdx)];
        std::vector<int> paraPath = buildPathPrefix(colLoc.ancestors, colLoc.ancestors.size());
        paraPath.push_back(tLoc.paraChildIdx);
        out.invalidations.push_back(
            FrameInvalidation{colLoc.pageIndex, std::move(paraPath), tLoc.lineChildIdx, tLoc.lineChildIdx, false});
        out.incremental = true;
        out.fullInvalidation = false;
        return true;
    }

    if (targetIdx > 0) {
        const Node& prevLine = *paraLines[static_cast<std::size_t>(targetIdx - 1)].line;
        if (!prevLine.lineData().lineBreak && (!prevLine.children.empty() || !prevLine.glyphs().runs.empty())) return false;
    }
    std::int32_t nextObjWidth = 0;
    bool hasNextLine = (targetIdx + 1 < static_cast<int>(paraLines.size()));
    if (hasNextLine) {
        const Node& nextLine = *paraLines[static_cast<std::size_t>(targetIdx + 1)].line;
        if (!targetLine.lineData().lineBreak) {
            if (!lineStartsWithObject(nextLine)) return false;
            const bool headed = nextLine.glyphs().heading ||
                                (!nextLine.children.empty() && nextLine.children.front().headingMark);
            const Node& obj = headed ? nextLine.children.front() : nextLine.children[nextLine.glyphs().runs.front().slot];
            nextObjWidth = obj.width + obj.marginLeft + obj.marginRight;
        }
    }
    // 머리·개체·탭 없이 글자만 있는 줄이다.
    if (!targetLine.children.empty() || targetLine.glyphs().heading) return false;
    for (const LineRun& run : targetLine.glyphs().runs) {
        if (run.kind == RunKind::Object || run.kind == RunKind::Tab) return false;
    }

    const int deltaOff = spec.editNewEndOff - spec.editOldEndOff;
    const int newLineEnd = targetLine.end + deltaOff;
    if (newLineEnd <= targetLine.start) {
        // 줄의 마지막 글자까지 지우면 그 줄 칸을 비운다. 이전 글자 그림을 그대로 두면 화면에 남는다.
        if (targetLine.start != 0 || paraLines.size() != 1) return false;
        evictNodeMeasures(targetLine);
        targetLine.end = targetLine.start;
        targetLine.textMut().clear();
        targetLine.children.clear();
        targetLine.glyphsMut() = {};
        targetLine.contentHash = 0;
        targetLine.huBoundsReady = false;
        const ParaLineLoc& tLoc = paraLines[static_cast<std::size_t>(targetIdx)];
        const StoryColumnLoc& colLoc = storyCols[static_cast<std::size_t>(tLoc.colListIdx)];
        std::vector<int> paraPath = buildPathPrefix(colLoc.ancestors, colLoc.ancestors.size());
        paraPath.push_back(tLoc.paraChildIdx);
        out.invalidations.push_back(
            FrameInvalidation{colLoc.pageIndex, std::move(paraPath), tLoc.lineChildIdx, tLoc.lineChildIdx, false});
        out.incremental = true;
        out.fullInvalidation = false;
        return true;
    }

    Node built;
    std::string lineText;
    RunBuilder spans(built, lineText, true);

    int pos = 0;
    int matchedCount = 0;
    std::int32_t pen = targetLine.indent;
    std::int32_t lastTrail = 0;
    int firstFontSize = 0;
    std::uint32_t firstShape = 0;
    std::string firstFont;
    int maxTextHeight = 0;

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
                std::size_t idx = 0;
                char32_t cp = 0;
                while (nextCodePoint(item.text, idx, cp)) {
                    const int curPos = pos++;
                    if (curPos < targetLine.start || curPos >= newLineEnd) continue;
                    if (cp == '\n' || cp == '\t') return false;
                    ++matchedCount;
                    const GlyphFace drawn = glyphFace(doc, *shape, cp, true, true);
                    Atom atom;
                    atom.cp = cp;
                    atom.fontSize = fontSize;
                    atom.charShapeId = run.charShapeId;
                    atom.font = drawn.font;
                    atom.ratio = drawn.ratio;
                    atom.relSize = drawn.relSize;
                    atom.spacing = item.ignoreSpacing ? 0 : drawn.spacing;
                    const Lang offsetLang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
                    const std::size_t offsetSlot = static_cast<std::size_t>(offsetLang);
                    atom.charOffset = offsetSlot < shape->offset.size() ? shape->offset[offsetSlot] : 0;
                    atom.markpen = item.markpen;
                    atom.width = glyphWidth(doc, cp, *measure);
                    const Lang trailLang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
                    atom.trail = layout::glyphTrail(cp, *measure, trailLang, &doc);
                    if (matchedCount == 1) {
                        firstFontSize = fontSize;
                        firstShape = run.charShapeId;
                        firstFont = drawn.font;
                    }
                    if (fontSize > maxTextHeight) maxTextHeight = fontSize;
                    if (cp == ' ' || cp == 0x2007) {
                        spans.flush();
                        const std::size_t at = lineText.size();
                        appendUtf8(lineText, cp);
                        appendGlyphRun(built, atom, at, lineText.size());
                    } else {
                        spans.add(atom);
                        appendUtf8(lineText, cp);
                    }
                    pen += atom.width;
                    lastTrail = atom.trail;
                }
            } else if ((visualObject(item.control) && item.control.placement &&
                        item.control.placement->treatAsChar) ||
                       (item.type == InlineItem::Type::Control && glyphGroupControl(item.control))) {
                const int curPos = pos++;
                if (curPos >= targetLine.start && curPos < newLineEnd) return false;
            } else if (item.type == InlineItem::Type::Control && occupiesBodyLine(item.control)) {
                if (pos >= targetLine.start && pos < newLineEnd) return false;
            }
        }
    }
    spans.flush();
    if (matchedCount != newLineEnd - targetLine.start) return false;
    if (pen - lastTrail > targetLine.width + kSlack) return false;
    if (hasNextLine && !targetLine.lineData().lineBreak && nextObjWidth > 0 && pen + nextObjWidth <= targetLine.width + kSlack) {
        return false;
    }

    const ParaShape* paraShape = findParaShape(doc, para.paraShapeId);
    const SpacingKind kind = spacingKindOf(paraShape);
    const int spacingValue = spacingValueOf(paraShape);
    const int lineEm = firstFontSize > 0 ? firstFontSize : kEm;
    std::int32_t charBox = maxTextHeight > 0 ? maxTextHeight : lineEm;
    std::int32_t newLineBaseline = -1;
    std::int32_t pitch = pitchFor(kind, spacingValue, lineEm);
    // 글꼴에 어울리는 줄 높이면 글자 상자와 줄간격 추가분이 글꼴 상자에서 나온다.
    if (targetLine.lineData().matchFontBaseline) {
        const SpanFontBox box = lineFontBox(built);
        if (box.baseline >= 0) {
            newLineBaseline = box.baseline;
            charBox = std::max(charBox, box.height);
            pitch = pitchFor(kind, spacingValue, charBox);
        }
    }
    if (pitch != targetLine.height || charBox != targetLine.lineData().textHeight) return false;
    if (newLineBaseline != targetLine.lineData().lineBaseline) return false;

    evictNodeMeasures(targetLine);
    targetLine.end = newLineEnd;
    targetLine.charShapeId = firstShape;
    targetLine.font = firstFont;
    targetLine.fontSize = lineEm;
    targetLine.textMut() = std::move(lineText);
    trimLineGlyphs(built.glyphsMut());
    targetLine.glyphsMut() = std::move(built.glyphs());
    if (spec.story > 0) stampStory(targetLine, spec.story);

    if (deltaOff != 0) {
        for (int i = targetIdx + 1; i < static_cast<int>(paraLines.size()); ++i) {
            shiftLineOffsets(*paraLines[static_cast<std::size_t>(i)].line, deltaOff);
        }
    }

    const ParaLineLoc& tLoc = paraLines[static_cast<std::size_t>(targetIdx)];
    const StoryColumnLoc& colLoc = storyCols[static_cast<std::size_t>(tLoc.colListIdx)];
    std::vector<int> paraPath = buildPathPrefix(colLoc.ancestors, colLoc.ancestors.size());
    paraPath.push_back(tLoc.paraChildIdx);
    out.invalidations.push_back(
        FrameInvalidation{colLoc.pageIndex, std::move(paraPath), tLoc.lineChildIdx, tLoc.lineChildIdx, false});
    out.incremental = true;
    out.fullInvalidation = false;
    return true;
}

}  // namespace

bool laidParagraphHasNoteMark(const std::vector<Node>& pages, int story, int paraIndex, int hintPage) {
    auto scanPage = [&](int page, bool& saw) {
        saw = false;
        if (page < 0 || page >= static_cast<int>(pages.size())) return false;
        std::vector<const Node*> stack;
        stack.push_back(&pages[static_cast<std::size_t>(page)]);
        while (!stack.empty()) {
            const Node* node = stack.back();
            stack.pop_back();
            if (node->kind == NodeKind::Line && node->story == story && node->paragraph == paraIndex) {
                saw = true;
                for (const LineRun& run : node->glyphs().runs) {
                    if (run.kind == RunKind::Note) return true;
                }
                continue;
            }
            for (const Node& child : node->children) stack.push_back(&child);
        }
        return false;
    };
    if (hintPage >= 0 && !pages.empty()) {
        const int begin = std::max(0, hintPage - 1);
        const int end = std::min(static_cast<int>(pages.size()) - 1, hintPage + 1);
        bool saw = false;
        int last = -1;
        for (int page = begin; page <= end; ++page) {
            bool onPage = false;
            if (scanPage(page, onPage)) return true;
            if (onPage) {
                saw = true;
                last = page;
            }
        }
        if (saw && last < end) return false;
    }
    bool ignored = false;
    for (int page = 0; page < static_cast<int>(pages.size()); ++page) {
        if (scanPage(page, ignored)) return true;
    }
    return false;
}

bool paragraphKeepsNote(const model::Paragraph& para) {
    for (const model::Run& run : para.runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type != model::InlineItem::Type::Control) continue;
            if (item.control.kind == model::ControlKind::Footnote ||
                item.control.kind == model::ControlKind::Endnote) {
                return true;
            }
        }
    }
    return false;
}

void assignPageBorders(model::Document& doc, int sectionIndex, std::array<model::PageBorderFill, 3> borders,
                       std::vector<Node>* pages) {
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(doc.sections.size())) return;
    model::Section& section = doc.sections[static_cast<std::size_t>(sectionIndex)];
    section.pageBorders = borders;
    if (!section.paragraphs.empty()) {
        for (model::Run& run : section.paragraphs.front().runs) {
            for (model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Control &&
                    item.control.kind == model::ControlKind::SectionDef) {
                    item.control.pageBorders = borders;
                }
            }
        }
    }
    if (!pages) return;
    for (Node& paper : *pages) {
        if (paper.kind != NodeKind::Paper || paper.section != sectionIndex) continue;
        stampPageChrome(paper, doc, section, paper.pageNumber);
    }
}

EditRelayoutResult relayoutAfterEditIncremental(const model::Document& doc, std::vector<Node>& pages,
                                                const EditRelayoutSpec& spec) {
    EditRelayoutResult out;
    struct HuBoundsGuard {
        const model::Document& doc;
        std::vector<Node>& pages;
        EditRelayoutResult& out;
        ~HuBoundsGuard() {
            if (!out.incremental || out.fullInvalidation || out.lineBoxSame) return;
            applyDefaultPageMargins(pages);
            std::unordered_set<int> touched;
            for (const FrameInvalidation& inv : out.invalidations) {
                if (inv.page >= 0 && inv.page < static_cast<int>(pages.size())) {
                    touched.insert(inv.page);
                }
            }
            for (const int p : touched) {
                updatePageHuBounds(pages[static_cast<std::size_t>(p)], &doc);
            }
        }
    } huGuard{doc, pages, out};
    auto giveUp = [&](const char* why) {
        out.fullInvalidation = true;
        out.widenReason = why;
        return out;
    };
    if (spec.story <= 0 || spec.paraIndex < 0 || pages.empty()) {
        return giveUp("편집위치없음");
    }
    const model::StorySlot* slot = resolveStory(doc, spec.story);
    const std::vector<model::Paragraph>* paras = storyParagraphs(doc, spec.story);
    if (!slot || !paras || spec.paraIndex >= static_cast<int>(paras->size())) {
        return giveUp("스토리없음");
    }
    // 격자 구역의 본문은 줄 윗변을 격자에 맞춰 쌓는다. 문단 하나만 다시 짜는 길은 그 규칙이 없어 구역째 다시 놓는다.
    if (slot->kind == model::StoryKind::Section && slot->sectionIndex >= 0 &&
        slot->sectionIndex < static_cast<int>(doc.sections.size())) {
        const model::Section& sec = doc.sections[static_cast<std::size_t>(slot->sectionIndex)];
        if (sec.lineGrid > 0 || sec.charGrid > 0) return giveUp("격자");
    }
    // 세로쓰기 글상자·표 칸은 줄 길이가 상자 높이라 상자·줄 높이 계산이 가로와 다르다. 그 개체째 다시 놓는다.
    if ((slot->subList && slot->subList->textDirection != 0) ||
        (slot->tableCell && slot->tableCell->content.textDirection != 0)) {
        return giveUp("세로쓰기 상자");
    }
    // 번호만 문단에서 빼면 줄은 그대로여도 쪽 아래 각주·미주가 남는다. 그때는 쪽을 다시 놓는다.
    if (!paragraphKeepsNote((*paras)[static_cast<std::size_t>(spec.paraIndex)]) &&
        laidParagraphHasNoteMark(pages, spec.story, spec.paraIndex, spec.hintPage)) {
        return giveUp("각주미주");
    }

    std::vector<StoryColumnLoc> storyCols;
    int targetColListIdx = -1;
    int targetParaChildIdx = -1;
    int paraOccurrences = 0;

    auto locateTargetInStoryCols = [&]() {
        targetColListIdx = -1;
        targetParaChildIdx = -1;
        paraOccurrences = 0;
        for (int cIdx = 0; cIdx < static_cast<int>(storyCols.size()); ++cIdx) {
            Node& col = *storyCols[static_cast<std::size_t>(cIdx)].column;
            for (int i = 0; i < static_cast<int>(col.children.size()); ++i) {
                const Node& child = col.children[static_cast<std::size_t>(i)];
                if (child.kind == NodeKind::Paragraph && child.paragraph == spec.paraIndex) {
                    targetColListIdx = cIdx;
                    targetParaChildIdx = i;
                    ++paraOccurrences;
                }
            }
        }
    };

    if (spec.paraDelta == 0 && spec.hintPage >= 0 && spec.hintPage < static_cast<int>(pages.size())) {
        const int pStart = std::max(0, spec.hintPage - 1);
        const int pEnd = std::min(static_cast<int>(pages.size()) - 1, spec.hintPage + 1);
        for (int p = pStart; p <= pEnd; ++p) {
            std::vector<AncestorStep> stack;
            findStoryColumns(pages[static_cast<std::size_t>(p)], p, spec.story, stack, storyCols);
        }
        locateTargetInStoryCols();
        const bool foundCleanlyInWindow =
            paraOccurrences == 1 && targetColListIdx >= 0 &&
            storyCols[static_cast<std::size_t>(targetColListIdx)].pageIndex == spec.hintPage;
        if (!foundCleanlyInWindow) {
            storyCols.clear();
        }
    }

    if (storyCols.empty()) {
        for (int p = 0; p < static_cast<int>(pages.size()); ++p) {
            std::vector<AncestorStep> stack;
            findStoryColumns(pages[static_cast<std::size_t>(p)], p, spec.story, stack, storyCols);
        }
        if (storyCols.empty()) {
            return giveUp("단을못찾음");
        }
        locateTargetInStoryCols();
    }
    if (paraOccurrences != 1 || targetColListIdx < 0 || targetParaChildIdx < 0) {
        if (spec.paraDelta == 0 && paraOccurrences > 1 &&
            tryUpdateSingleLineInPlace(doc, (*paras)[static_cast<std::size_t>(spec.paraIndex)], spec, storyCols, pages,
                                       out)) {
            return out;
        }
        return giveUp("문단이여러단에걸침");
    }

    StoryColumnLoc& loc = storyCols[static_cast<std::size_t>(targetColListIdx)];
    if (loc.pageIndex >= 0 && loc.pageIndex < static_cast<int>(pages.size()) &&
        pages[static_cast<std::size_t>(loc.pageIndex)].spansLater) {
        ensurePageSpans(doc, pages[static_cast<std::size_t>(loc.pageIndex)]);
    }
    Node& col = *loc.column;
    const Node* parentBodyNode =
        !loc.ancestors.empty() && loc.ancestors.back().parent && loc.ancestors.back().parent->kind == NodeKind::Body
            ? loc.ancestors.back().parent
            : nullptr;
    const bool hasFloatingInCol =
        parentBodyNode ? bodyHasFloatingObjects(*parentBodyNode, col) : columnHasFloatingObjects(col);
    const bool floatingBlocksShift =
        hasFloatingInCol && (parentBodyNode ? bodyFloatingBlocksParagraphShift(*parentBodyNode, col, targetParaChildIdx)
                                            : columnFloatingBlocksParagraphShift(col, targetParaChildIdx));
    if (floatingBlocksShift && spec.paraDelta != 0) {
        return giveUp("떠있는개체가문단을가림");
    }

    auto hasSplitBoundaryWithNextCol = [&](const std::vector<StoryColumnLoc>& cols, int colIdx) {
        if (colIdx + 1 >= static_cast<int>(cols.size())) return false;
        const Node& c0 = *cols[static_cast<std::size_t>(colIdx)].column;
        const Node& c1 = *cols[static_cast<std::size_t>(colIdx + 1)].column;
        int lastPara = -1;
        for (auto it = c0.children.rbegin(); it != c0.children.rend(); ++it) {
            if (it->kind == NodeKind::Paragraph) {
                lastPara = it->paragraph;
                break;
            }
        }
        for (const Node& child : c1.children) {
            if (child.kind == NodeKind::Paragraph) {
                return lastPara >= 0 && child.paragraph == lastPara;
            }
        }
        return false;
    };

    int depth = 0;
    for (const AncestorStep& step : loc.ancestors) {
        if (step.parent && step.parent->kind == NodeKind::Object) ++depth;
    }
    std::int32_t defaultTab = 4000;
    if (slot->sectionIndex >= 0 && slot->sectionIndex < static_cast<int>(doc.sections.size())) {
        const model::Section& sec = doc.sections[static_cast<std::size_t>(slot->sectionIndex)];
        if (sec.defaultTab > 0) defaultTab = sec.defaultTab;
    }

    const std::vector<int> pathToColumn = buildPathPrefix(loc.ancestors, loc.ancestors.size());
    std::int32_t colHeightDelta = 0;

    if (spec.paraDelta == 0) {
        if (!spec.paraShapeChanged &&
            tryUpdateSingleLineInPlace(doc, (*paras)[static_cast<std::size_t>(spec.paraIndex)], spec, storyCols, pages,
                                       out)) {
            return out;
        }
        Node& oldPara = col.children[static_cast<std::size_t>(targetParaChildIdx)];
        if (hasFloatingInCol && !paraLinesUseFullColumnWidth(oldPara)) {
            return giveUp("줄너비가단과다르고떠있는개체");
        }
        SingleParaLayoutResult pRes = layoutSingleParagraphIncremental(
            doc, (*paras)[static_cast<std::size_t>(spec.paraIndex)], spec.paraIndex, oldPara.section, spec.story,
            depth, defaultTab, col.width, oldPara.y, &oldPara, true, spec.editStartOff, spec.editOldEndOff,
            spec.editNewEndOff, spec.useStoredLineHeight, targetParaChildIdx == 0);
        if (!pRes.ok) {
            return giveUp("문단증분실패");
        }
        if (spec.paraShapeChanged) {
            pRes.firstDirtyLine = 0;
            pRes.lastDirtyLine = -1;
        }
        colHeightDelta = pRes.paragraph.height - oldPara.height;
        if (colHeightDelta != 0 && floatingBlocksShift) {
            return giveUp("높이변화와떠있는개체");
        }
        if (colHeightDelta != 0 && hasSplitBoundaryWithNextCol(storyCols, targetColListIdx)) {
            return giveUp("문단이다음단까지걸침");
        }
        oldPara = std::move(pRes.paragraph);

        if (colHeightDelta == 0) {
            if (pRes.lastDirtyLine == -1 || pRes.firstDirtyLine <= pRes.lastDirtyLine) {
                std::vector<int> paraPath = pathToColumn;
                paraPath.push_back(targetParaChildIdx);
                out.invalidations.push_back(
                    FrameInvalidation{loc.pageIndex, std::move(paraPath), pRes.firstDirtyLine, pRes.lastDirtyLine, false});
            }
            out.incremental = true;
            return out;
        }

        std::vector<int> paraPath = pathToColumn;
        paraPath.push_back(targetParaChildIdx);
        out.invalidations.push_back(
            FrameInvalidation{loc.pageIndex, std::move(paraPath), pRes.firstDirtyLine, -1, false});
        if (targetParaChildIdx + 1 < static_cast<int>(col.children.size())) {
            for (int i = targetParaChildIdx + 1; i < static_cast<int>(col.children.size()); ++i) {
                col.children[static_cast<std::size_t>(i)].y += colHeightDelta;
            }
            out.invalidations.push_back(
                FrameInvalidation{loc.pageIndex, pathToColumn, targetParaChildIdx + 1, -1, false});
        }
    } else if (spec.paraDelta == 1) {
        if (spec.paraIndex + 1 >= static_cast<int>(paras->size()) ||
            hasSplitBoundaryWithNextCol(storyCols, targetColListIdx)) {
            return giveUp("문단나누기가다음단까지");
        }
        Node& oldPara = col.children[static_cast<std::size_t>(targetParaChildIdx)];
        if (oldPara.ext().headingType == "NUMBER" || oldPara.ext().headingType == "OUTLINE") {
            return giveUp("개요번호문단나누기");
        }
        SingleParaLayoutResult pRes0 = layoutSingleParagraphIncremental(
            doc, (*paras)[static_cast<std::size_t>(spec.paraIndex)], spec.paraIndex, oldPara.section, spec.story,
            depth, defaultTab, col.width, oldPara.y, nullptr, false, 0, 0, 0, spec.useStoredLineHeight,
            targetParaChildIdx == 0);
        if (!pRes0.ok) {
            return giveUp("나뉜앞문단실패");
        }
        SingleParaLayoutResult pRes1 = layoutSingleParagraphIncremental(
            doc, (*paras)[static_cast<std::size_t>(spec.paraIndex + 1)], spec.paraIndex + 1, oldPara.section,
            spec.story, depth, defaultTab, col.width, oldPara.y + pRes0.paragraph.height, nullptr, false, 0, 0, 0,
            spec.useStoredLineHeight, false);
        if (!pRes1.ok) {
            return giveUp("나뉜뒷문단실패");
        }
        evictNodeMeasures(oldPara);
        colHeightDelta = (pRes0.paragraph.height + pRes1.paragraph.height) - oldPara.height;
        oldPara = std::move(pRes0.paragraph);
        col.children.insert(col.children.begin() + targetParaChildIdx + 1, std::move(pRes1.paragraph));
        out.paragraphsShifted = true;
        for (int i = targetParaChildIdx + 2; i < static_cast<int>(col.children.size()); ++i) {
            Node& sib = col.children[static_cast<std::size_t>(i)];
            sib.paragraph += 1;
            sib.y += colHeightDelta;
            for (Node& line : sib.children) {
                if (line.kind == NodeKind::Line) line.paragraph = sib.paragraph;
            }
        }
        for (int cIdx = targetColListIdx + 1; cIdx < static_cast<int>(storyCols.size()); ++cIdx) {
            const StoryColumnLoc& nextLoc = storyCols[static_cast<std::size_t>(cIdx)];
            shiftColumnParagraphIndices(*nextLoc.column, 1);
            out.invalidations.push_back(FrameInvalidation{
                nextLoc.pageIndex, buildPathPrefix(nextLoc.ancestors, nextLoc.ancestors.size()), 0, -1, false});
        }
        out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToColumn, targetParaChildIdx, -1, false});
    } else if (spec.paraDelta < 0) {
        const int removedCount = -spec.paraDelta;
        if (targetParaChildIdx + removedCount >= static_cast<int>(col.children.size()) ||
            hasSplitBoundaryWithNextCol(storyCols, targetColListIdx)) {
            return giveUp("문단합치기가다음단까지");
        }
        for (int k = 0; k <= removedCount; ++k) {
            const Node& victim = col.children[static_cast<std::size_t>(targetParaChildIdx + k)];
            if (victim.kind != NodeKind::Paragraph || victim.ext().headingType == "NUMBER" ||
                victim.ext().headingType == "OUTLINE") {
                return giveUp("개요번호문단합치기");
            }
        }
        Node& firstOldPara = col.children[static_cast<std::size_t>(targetParaChildIdx)];
        const Node& lastOldPara = col.children[static_cast<std::size_t>(targetParaChildIdx + removedCount)];
        const std::int32_t oldCombinedHeight = (lastOldPara.y + lastOldPara.height) - firstOldPara.y;
        SingleParaLayoutResult pRes = layoutSingleParagraphIncremental(
            doc, (*paras)[static_cast<std::size_t>(spec.paraIndex)], spec.paraIndex, firstOldPara.section, spec.story,
            depth, defaultTab, col.width, firstOldPara.y, nullptr, false, 0, 0, 0, spec.useStoredLineHeight,
            targetParaChildIdx == 0);
        if (!pRes.ok) {
            return giveUp("합친문단실패");
        }
        for (int k = 0; k <= removedCount; ++k) {
            evictNodeMeasures(col.children[static_cast<std::size_t>(targetParaChildIdx + k)]);
        }
        colHeightDelta = pRes.paragraph.height - oldCombinedHeight;
        firstOldPara = std::move(pRes.paragraph);
        col.children.erase(col.children.begin() + targetParaChildIdx + 1,
                           col.children.begin() + targetParaChildIdx + 1 + removedCount);
        out.paragraphsShifted = true;
        for (int i = targetParaChildIdx + 1; i < static_cast<int>(col.children.size()); ++i) {
            Node& sib = col.children[static_cast<std::size_t>(i)];
            sib.paragraph += spec.paraDelta;
            sib.y += colHeightDelta;
            for (Node& line : sib.children) {
                if (line.kind == NodeKind::Line) line.paragraph = sib.paragraph;
            }
        }
        for (int cIdx = targetColListIdx + 1; cIdx < static_cast<int>(storyCols.size()); ++cIdx) {
            const StoryColumnLoc& nextLoc = storyCols[static_cast<std::size_t>(cIdx)];
            shiftColumnParagraphIndices(*nextLoc.column, spec.paraDelta);
            out.invalidations.push_back(FrameInvalidation{
                nextLoc.pageIndex, buildPathPrefix(nextLoc.ancestors, nextLoc.ancestors.size()), 0, -1, false});
        }
        out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToColumn, targetParaChildIdx, -1, false});
    } else {
        return giveUp("문단개수변화");
    }

    // 상향식(Bottom-Up) 높이 변화 전파: Column -> Body -> (Cell -> Table / Object / Page)
    std::size_t curColAncIdx = loc.ancestors.size(); // loc.ancestors[curColAncIdx - 1].parent is Body
    Node* curCol = &col;
    std::int32_t curColHeightDelta = colHeightDelta;
    while (true) {
        if (curColAncIdx < 2) {
            return giveUp("쪽조상없음");
        }
        Node& parentBody = *loc.ancestors[curColAncIdx - 1].parent;
        if (parentBody.kind != NodeKind::Body) {
            return giveUp("본문아님");
        }

        // 1. 페이지 직속 본문 Column 인 경우 (Page -> Body -> Column)
        if (curColAncIdx == 2) {
            if (columnCountInBody(parentBody) != 1) {
                return giveUp("단이둘이상");
            }
            const std::int32_t charBot = columnCharBottom(*curCol);
            if (curCol->y + charBot >= parentBody.height) {
                return giveUp("쪽끝");
            }
            int colStory = curCol->story;
            if (colStory <= 0 && !curCol->children.empty()) colStory = curCol->children[0].story;
            if (colStory > 0) {
                const std::vector<StoryColumnLoc>* colsPtr = &storyCols;
                std::vector<StoryColumnLoc> hostCols;
                if (colStory != spec.story) {
                    for (int p = 0; p < static_cast<int>(pages.size()); ++p) {
                        std::vector<AncestorStep> st;
                        findStoryColumns(pages[static_cast<std::size_t>(p)], p, colStory, st, hostCols);
                    }
                    colsPtr = &hostCols;
                }
                int curHostIdx = -1;
                for (int cIdx = 0; cIdx < static_cast<int>(colsPtr->size()); ++cIdx) {
                    if ((*colsPtr)[static_cast<std::size_t>(cIdx)].column == curCol) {
                        curHostIdx = cIdx;
                        break;
                    }
                }
                if (curColHeightDelta < 0 && curHostIdx > 0) {
                    const StoryColumnLoc& prevLoc = (*colsPtr)[static_cast<std::size_t>(curHostIdx - 1)];
                    const Node& prevCol = *prevLoc.column;
                    const Node* prevBody =
                        (!prevLoc.ancestors.empty() && prevLoc.ancestors.back().parent &&
                         prevLoc.ancestors.back().parent->kind == NodeKind::Body)
                            ? prevLoc.ancestors.back().parent
                            : nullptr;
                    if (prevBody && prevBody->height > 0) {
                        const std::int32_t prevUsedBottom = prevCol.y + columnContentBottom(prevCol);
                        const std::int32_t prevAvailH = prevBody->height - prevUsedBottom;
                        if (!nextColumnCanStayPutOnShrink(doc, prevCol, *curCol, prevAvailH)) {
                            return giveUp("앞단으로당김");
                        }
                    } else if (hasSplitBoundaryWithNextCol(*colsPtr, curHostIdx - 1)) {
                        return giveUp("앞단과문단이걸침");
                    }
                }
                if (curHostIdx >= 0 && curHostIdx + 1 < static_cast<int>(colsPtr->size())) {
                    if (curColHeightDelta > 0 && hasSplitBoundaryWithNextCol(*colsPtr, curHostIdx)) {
                        return giveUp("다음단으로넘침");
                    }
                    if (curColHeightDelta < 0) {
                        const std::int32_t usedNow = columnContentBottom(*curCol);
                        const std::int32_t availH = parentBody.height - (curCol->y + usedNow);
                        const Node& nextCol = *(*colsPtr)[static_cast<std::size_t>(curHostIdx + 1)].column;
                        if (!nextColumnCanStayPutOnShrink(doc, *curCol, nextCol, availH)) {
                            return giveUp("다음단을당김");
                        }
                    }
                }
            }
            std::int32_t used = 0;
            for (const Node& child : curCol->children) {
                used = std::max(used, child.y + child.height);
            }
            curCol->height = std::max<std::int32_t>(used, 1);
            out.incremental = true;
            return out;
        }

        // 2. 표 셀 내부 Column 인 경우 (... -> Object(table) -> Cell -> Body -> Column)
        Node& grandParent = *loc.ancestors[curColAncIdx - 2].parent;
        if (grandParent.kind == NodeKind::Cell && curColAncIdx >= 3) {
            Node& tableNode = *loc.ancestors[curColAncIdx - 3].parent;
            if (tableNode.kind != NodeKind::Object || tableNode.role != "table") {
                return giveUp("표아님");
            }
            const std::int32_t newUsed = columnContentBottom(*curCol);
            curCol->height = newUsed;
            parentBody.height = newUsed;

            const int editedCellChildIdx = loc.ancestors[curColAncIdx - 3].childIndex;
            const std::vector<int> pathToTable = buildPathPrefix(loc.ancestors, curColAncIdx - 3);
            std::int32_t tableHDelta = 0;
            if (!updateTableAfterCellEdit(tableNode, editedCellChildIdx, loc.pageIndex, pathToTable,
                                          out.invalidations, tableHDelta)) {
                return giveUp("표칸높이");
            }
            if (tableHDelta == 0) {
                out.incremental = true;
                return out;
            }

            // 표 높이가 변한 경우: 표가 글자처럼 취급(treatAsChar)되어 상위 Line -> Paragraph -> Column 에 속해 있는지 확인
            const std::size_t tableAncIdx = curColAncIdx - 3;
            if (!tableNode.treatAsChar || tableAncIdx < 3) {
                return giveUp("글자처럼취한표아님");
            }
            Node& hostLine = *loc.ancestors[tableAncIdx - 1].parent;
            Node& hostPara = *loc.ancestors[tableAncIdx - 2].parent;
            Node& hostCol = *loc.ancestors[tableAncIdx - 3].parent;
            if (hostLine.kind != NodeKind::Line || hostPara.kind != NodeKind::Paragraph ||
                hostCol.kind != NodeKind::Column || columnHasFloatingObjects(hostCol)) {
                return giveUp("표줄위치");
            }
            const std::int32_t oldLineH = hostLine.height;
            const std::int32_t newCharBox = lineCharBox(hostLine);
            hostLine.editContent().textHeight = std::max(hostLine.fontSize, newCharBox);
            hostLine.height = std::max(hostLine.lineData().textHeight, hostLine.height + tableHDelta);
            const std::int32_t lineHDelta = hostLine.height - oldLineH;
            const std::int32_t propagateDelta = (lineHDelta != 0) ? lineHDelta : tableHDelta;
            const int hostLineIdx = loc.ancestors[tableAncIdx - 2].childIndex;
            const int hostParaIdx = loc.ancestors[tableAncIdx - 3].childIndex;
            for (int i = hostLineIdx + 1; i < static_cast<int>(hostPara.children.size()); ++i) {
                hostPara.children[static_cast<std::size_t>(i)].y += lineHDelta;
            }
            hostPara.height += lineHDelta;
            for (int i = hostParaIdx + 1; i < static_cast<int>(hostCol.children.size()); ++i) {
                hostCol.children[static_cast<std::size_t>(i)].y += lineHDelta;
            }
            const std::vector<int> pathToHostLine = buildPathPrefix(loc.ancestors, tableAncIdx - 1);
            out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToHostLine, -1, -1, true});
            if (hostLineIdx + 1 < static_cast<int>(hostPara.children.size())) {
                const std::vector<int> pathToHostPara = buildPathPrefix(loc.ancestors, tableAncIdx - 2);
                out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToHostPara, hostLineIdx + 1, -1, false});
            }
            if (hostParaIdx + 1 < static_cast<int>(hostCol.children.size())) {
                const std::vector<int> pathToHostCol = buildPathPrefix(loc.ancestors, tableAncIdx - 3);
                out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToHostCol, hostParaIdx + 1, -1, false});
            }

            curCol = &hostCol;
            curColAncIdx = tableAncIdx - 3;
            curColHeightDelta = propagateDelta;
            continue;
        }

        // 3. 글상자(Object -> Body -> Column) 내부인 경우
        if (grandParent.kind == NodeKind::Object && parentBody.role != "caption") {
            const std::int32_t newUsed = columnContentBottom(*curCol);
            curCol->height = std::max<std::int32_t>(newUsed, 1);
            parentBody.height = std::max<std::int32_t>(newUsed, 1);
            const std::int32_t need = grandParent.ext().obj().filePadTop + grandParent.ext().obj().filePadBottom + newUsed;
            const std::int32_t newObjH = std::max(grandParent.ext().obj().fileHeight, need);
            const std::int32_t innerH =
                std::max<std::int32_t>(200, newObjH - grandParent.ext().obj().filePadTop - grandParent.ext().obj().filePadBottom);
            std::int32_t contentY = grandParent.ext().obj().filePadTop;
            if (parentBody.vertAlign == "CENTER" && newUsed < innerH) {
                contentY = grandParent.ext().obj().filePadTop + (innerH - newUsed) / 2;
            } else if (parentBody.vertAlign == "BOTTOM" && newUsed < innerH) {
                contentY = grandParent.ext().obj().filePadTop + (innerH - newUsed);
            }
            if (contentY != parentBody.y) {
                parentBody.y = contentY;
                const std::vector<int> bodyPath = buildPathPrefix(loc.ancestors, curColAncIdx - 1);
                out.invalidations.push_back(FrameInvalidation{loc.pageIndex, bodyPath, 0, -1, false});
            }
            const std::int32_t objHDelta = newObjH - grandParent.height;
            if (objHDelta == 0) {
                out.incremental = true;
                return out;
            }
            const std::int32_t oldObjH = grandParent.height;
            grandParent.height = newObjH;
            const std::size_t objAncIdx = curColAncIdx - 2;
            const std::vector<int> pathToObj = buildPathPrefix(loc.ancestors, objAncIdx);
            out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToObj, -1, -1, false});
            for (int cIdx = 0; cIdx < static_cast<int>(grandParent.children.size()); ++cIdx) {
                Node& ch = grandParent.children[static_cast<std::size_t>(cIdx)];
                if (ch.role == "caption" && ch.y >= oldObjH) {
                    ch.y += objHDelta;
                }
            }
            if (!grandParent.treatAsChar) {
                if (!textAvoids(grandParent.ext().obj().textWrap) && objAncIdx >= 1) {
                    const Node& objHost = *loc.ancestors[objAncIdx - 1].parent;
                    if (objHost.kind != NodeKind::Object) {
                        out.incremental = true;
                        return out;
                    }
                }
                return giveUp("떠있는글상자");
            }
            if (objAncIdx < 3) {
                return giveUp("글상자조상부족");
            }
            Node& hostLine = *loc.ancestors[objAncIdx - 1].parent;
            Node& hostPara = *loc.ancestors[objAncIdx - 2].parent;
            Node& hostCol = *loc.ancestors[objAncIdx - 3].parent;
            if (hostLine.kind != NodeKind::Line || hostPara.kind != NodeKind::Paragraph ||
                hostCol.kind != NodeKind::Column || columnHasFloatingObjects(hostCol)) {
                return giveUp("글상자줄위치");
            }
            const std::int32_t oldLineH = hostLine.height;
            const std::int32_t newCharBox = lineCharBox(hostLine);
            hostLine.editContent().textHeight = std::max(hostLine.fontSize, newCharBox);
            hostLine.height = std::max(hostLine.height + objHDelta, hostLine.lineData().textHeight);
            const std::int32_t lineHDelta = hostLine.height - oldLineH;
            const int hostLineIdx = loc.ancestors[objAncIdx - 2].childIndex;
            const int hostParaIdx = loc.ancestors[objAncIdx - 3].childIndex;
            for (int i = hostLineIdx + 1; i < static_cast<int>(hostPara.children.size()); ++i) {
                hostPara.children[static_cast<std::size_t>(i)].y += lineHDelta;
            }
            hostPara.height += lineHDelta;
            for (int i = hostParaIdx + 1; i < static_cast<int>(hostCol.children.size()); ++i) {
                hostCol.children[static_cast<std::size_t>(i)].y += lineHDelta;
            }
            const std::vector<int> pathToHostLine = buildPathPrefix(loc.ancestors, objAncIdx - 1);
            out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToHostLine, -1, -1, true});
            if (hostLineIdx + 1 < static_cast<int>(hostPara.children.size())) {
                const std::vector<int> pathToHostPara = buildPathPrefix(loc.ancestors, objAncIdx - 2);
                out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToHostPara, hostLineIdx + 1, -1, false});
            }
            if (hostParaIdx + 1 < static_cast<int>(hostCol.children.size())) {
                const std::vector<int> pathToHostCol = buildPathPrefix(loc.ancestors, objAncIdx - 3);
                out.invalidations.push_back(FrameInvalidation{loc.pageIndex, pathToHostCol, hostParaIdx + 1, -1, false});
            }

            curCol = &hostCol;
            curColAncIdx = objAncIdx - 3;
            curColHeightDelta = lineHDelta;
            continue;
        }

        return giveUp("높이전파불가");
    }
}

}  // namespace chwp::render
