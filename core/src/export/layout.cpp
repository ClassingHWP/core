#include "export/font_metrics.hpp"
#include "export/layout.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <thread>

namespace chwp::layout {

namespace {

using namespace chwp::model;

bool nextCodePoint(std::string_view text, std::size_t& index, char32_t& out) {
    if (index >= text.size()) return false;
    const auto c = static_cast<unsigned char>(text[index]);
    if (c < 0x80) {
        out = c;
        index += 1;
        return true;
    }
    if ((c & 0xE0) == 0xC0 && index + 1 < text.size()) {
        out = (c & 0x1F) << 6 | (static_cast<unsigned char>(text[index + 1]) & 0x3F);
        index += 2;
        return true;
    }
    if ((c & 0xF0) == 0xE0 && index + 2 < text.size()) {
        out = (c & 0x0F) << 12 | (static_cast<unsigned char>(text[index + 1]) & 0x3F) << 6 |
              (static_cast<unsigned char>(text[index + 2]) & 0x3F);
        index += 3;
        return true;
    }
    if ((c & 0xF8) == 0xF0 && index + 3 < text.size()) {
        out = (c & 0x07) << 18 | (static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12 |
              (static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6 |
              (static_cast<unsigned char>(text[index + 3]) & 0x3F);
        index += 4;
        return true;
    }
    out = 0xFFFD;
    index += 1;
    return true;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool isHangul(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3);
}

bool isFullwidth(char32_t cp) {
    return isHangul(cp) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xFF01 && cp <= 0xFF60) ||
           (cp >= 0xFFE0 && cp <= 0xFFE6);
}

bool isSmallKana(char32_t cp) {
    switch (cp) {
        case 0x3041:
        case 0x3043:
        case 0x3045:
        case 0x3047:
        case 0x3049:
        case 0x3063:
        case 0x3083:
        case 0x3085:
        case 0x3087:
        case 0x308E:
        case 0x3095:
        case 0x3096:
        case 0x30A1:
        case 0x30A3:
        case 0x30A5:
        case 0x30A7:
        case 0x30A9:
        case 0x30C3:
        case 0x30E3:
        case 0x30E5:
        case 0x30E7:
        case 0x30EE:
        case 0x30F5:
        case 0x30F6:
            return true;
        default:
            return (cp >= 0x31F0 && cp <= 0x31FF) || (cp >= 0xFF67 && cp <= 0xFF6F);
    }
}

bool isOverflowGap(char32_t cp) {
    if (cp == ' ' || cp == 0x3000) return true;
    return cp >= 0x2000 && cp <= 0x200B;
}

bool lineHeadForbidden(char32_t cp) {
    if (isSmallKana(cp)) return true;
    switch (cp) {
        case ')':
        case ']':
        case '}':
        case ',':
        case '.':
        case ':':
        case ';':
        case '?':
        case '!':
        case 0x3001:  // 、
        case 0x3002:  // 。
        case 0xFF0C:  // ，
        case 0xFF0E:  // ．
        case 0xFF09:  // ）
        case 0xFF1A:  // ：
        case 0xFF1B:  // ；
        case 0x2019:
        case 0x201D:
            return true;
        default:
            return false;
    }
}

bool isLatinWordChar(char32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
           (cp >= 0x00C0 && cp <= 0x024F);
}

bool followsPreviousLanguage(char32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\n') return true;
    if (cp >= 0x21 && cp <= 0x2F) return true;
    if (cp >= 0x3A && cp <= 0x40) return true;
    if (cp >= 0x5B && cp <= 0x60) return true;
    if (cp >= 0x7B && cp <= 0x7E) return true;
    return false;
}

Lang languageOf(char32_t cp) {
    if (isHangul(cp) || (cp >= 0xFF01 && cp <= 0xFF60)) return Lang::Hangul;
    if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) return Lang::Hanja;
    if (cp >= 0x3040 && cp <= 0x30FF) return Lang::Japanese;
    if (cp < 0x80) return Lang::Latin;
    return Lang::Other;
}

const CharShape& charShapeAt(const Document& doc, std::uint32_t id) {
    static const CharShape kFallback = [] {
        CharShape shape;
        shape.ratio.fill(100);
        return shape;
    }();
    if (id < doc.header.charShapes.size() && doc.header.charShapes[id].id == id) return doc.header.charShapes[id];
    for (const auto& shape : doc.header.charShapes) {
        if (shape.id == id) return shape;
    }
    return kFallback;
}

const ParaShape& paraShapeAt(const Document& doc, std::uint32_t id) {
    static const ParaShape kFallback{};
    if (id < doc.header.paraShapes.size() && doc.header.paraShapes[id].id == id) return doc.header.paraShapes[id];
    for (const auto& ps : doc.header.paraShapes) {
        if (ps.id == id) return ps;
    }
    return kFallback;
}

std::int32_t emSize(const CharShape& shape) {
    return shape.height > 0 ? shape.height : 1000;
}

// 비율 줄간격의 추가분. 글자 상자(4의 배수)에만 비율을 곱한다.
// 0.5는 0에서 멀어지게 올린 뒤 4를 곱한다. 나머지 1·2·3은 비율을 타지 않는다.
std::int32_t percentSpacingExtra(std::int32_t size, std::int32_t percent) {
    if (size <= 0) size = 1000;
    if (percent < 0) percent = 160;
    const std::int32_t quant = size / 4;
    const std::int64_t numer = static_cast<std::int64_t>(quant) * (percent - 100);
    const std::int64_t rounded = numer >= 0 ? (numer + 50) / 100 : -(((-numer) + 50) / 100);
    return static_cast<std::int32_t>(rounded * 4);
}

std::int32_t linePitch(const ParaShape& para, std::int32_t em) {
    if (em <= 0) em = 1000;
    if (para.lineSpacingType == "FIXED") return para.lineSpacing > 0 ? para.lineSpacing : em;
    if (para.lineSpacingType == "BETWEEN_LINES") return em + std::max<std::int32_t>(0, para.lineSpacing);
    if (para.lineSpacingType == "AT_LEAST") return std::max(em, para.lineSpacing);
    // 0%는 글자 상자만큼 되돌린다. 음수는 비어 있는 값으로 보고 160이다.
    const std::int32_t percent = para.lineSpacing > 0 ? para.lineSpacing : (para.lineSpacing == 0 ? 0 : 160);
    return std::max<std::int32_t>(0, em + percentSpacingExtra(em, percent));
}

struct Obstacle {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    bool blockLine = false;
    std::string textFlow = "BOTH_SIDES";
    int page = 0;
};

bool affectsFlow(const Placement& placement) {
    return !placement.treatAsChar &&
           (placement.textWrap == "SQUARE" || placement.textWrap == "TOP_AND_BOTTOM");
}

// 정렬 기준점에서의 거리. 오른쪽·아래·바깥쪽은 기준 영역의 끝에서 개체 크기와 오프셋을 뺀다.
std::int32_t alignedOffset(const std::string& align, std::int32_t refOrigin, std::int32_t refSize,
                           std::int32_t objSize, std::int32_t offset) {
    if (align == "CENTER") return refOrigin + (refSize - objSize) / 2 + offset;
    if (align == "RIGHT" || align == "BOTTOM" || align == "OUTSIDE") {
        return refOrigin + refSize - objSize - offset;
    }
    return refOrigin + offset;
}

Obstacle obstacleFrom(const Placement& placement, std::int32_t anchorY, int page,
                      std::int32_t contentLeft, std::int32_t contentTop, std::int32_t contentWidth,
                      std::int32_t contentHeight, std::int32_t pageWidth, std::int32_t pageHeight) {
    // 쪽·단·문단은 본문 영역 기준이다. 종이는 종이 모서리 기준이라 본문 시작만큼 되돌린다.
    // 본문 시작의 세로는 위 여백 + 머리말 여백이다.
    const bool paperX = placement.horzRelTo == "PAPER";
    const bool paperY = placement.vertRelTo == "PAPER";
    const bool paraY = placement.vertRelTo == "PARA";
    const std::int32_t refLeft = paperX ? -contentLeft : 0;
    const std::int32_t refWidth = paperX ? pageWidth : contentWidth;
    const std::int32_t refTop = paraY ? anchorY : (paperY ? -contentTop : 0);
    const std::int32_t refHeight = paraY ? 0 : (paperY ? pageHeight : contentHeight);
    const std::int32_t width = static_cast<std::int32_t>(placement.width);
    const std::int32_t height = static_cast<std::int32_t>(placement.height);
    const std::int32_t x =
        alignedOffset(placement.horzAlign, refLeft, refWidth, width, placement.horzOffset);
    const std::int32_t y = paraY ? anchorY + placement.vertOffset
                                 : alignedOffset(placement.vertAlign, refTop, refHeight, height,
                                                 placement.vertOffset);
    Obstacle obstacle;
    obstacle.left = x - placement.marginLeft;
    obstacle.top = y - placement.marginTop;
    obstacle.right = x + width + placement.marginRight;
    obstacle.bottom = y + height + placement.marginBottom;
    obstacle.blockLine = placement.textWrap == "TOP_AND_BOTTOM";
    obstacle.textFlow = placement.textFlow.empty() ? "BOTH_SIDES" : placement.textFlow;
    obstacle.page = page;
    return obstacle;
}

std::int32_t innerWidth(const Control& control, std::int32_t fallback) {
    if (!control.placement || control.placement->width == 0) return fallback;
    const std::int32_t width = static_cast<std::int32_t>(control.placement->width) -
                                control.boxPadLeft - control.boxPadRight;
    return width > 0 ? width : static_cast<std::int32_t>(control.placement->width);
}

struct Flow {
    const Document* doc = nullptr;
    const TextOptions* options = nullptr;
    std::string* current = nullptr;
    std::vector<std::string>* pages = nullptr;
    std::int32_t contentWidth = 42520;
    std::int32_t contentHeight = 65600;
    std::int32_t contentLeft = 0;
    std::int32_t contentTop = 0;
    std::int32_t pageWidth = 59528;
    std::int32_t pageHeight = 84188;
    std::int32_t y = 0;
    int page = 0;
    int objectDepth = 0;  // 글상자·표 안을 조판 중이면 0보다 크다
    int paragraphIndex = -1;
    bool paragraphLanded = false;
    int layoutDepth = 0;
    std::vector<std::vector<LineRange>>* lineRanges = nullptr;
    std::vector<std::vector<std::uint8_t>>* innerOut = nullptr;
    std::vector<std::uint8_t> innerCurrent;
    std::vector<std::vector<int>>* paragraphPages = nullptr;

    void noteParagraph() {
        if (!paragraphPages || !pages || paragraphIndex < 0) return;
        const std::size_t index = pages->size();
        if (paragraphPages->size() <= index) paragraphPages->resize(index + 1);
        auto& list = (*paragraphPages)[index];
        if (list.empty() || list.back() != paragraphIndex) list.push_back(paragraphIndex);
        paragraphLanded = true;
    }

    void startPage() {
        const bool pushed = current && !current->empty();
        if (pushed) {
            pages->push_back(std::move(*current));
            if (innerOut) innerOut->push_back(std::move(innerCurrent));
        }
        if (current) current->clear();
        innerCurrent.clear();
        y = 0;
        ++page;
        if (pushed) noteParagraph();
    }

    void addLine(std::string line, std::int32_t pitch, int start = -1, int end = -1) {
        if (y > 0 && y + pitch > contentHeight) startPage();
        noteParagraph();
        if (lineRanges && layoutDepth == 1 && objectDepth == 0 && start >= 0 && paragraphIndex >= 0 &&
            pages) {
            const std::size_t index = pages->size();
            if (lineRanges->size() <= index) lineRanges->resize(index + 1);
            (*lineRanges)[index].push_back(LineRange{paragraphIndex, start, end});
        }
        *current += line;
        *current += '\n';
        // 수식처럼 줄 안에 개행이 있으면 미리보기는 그 개행마다 한 줄로 자른다.
        if (innerOut) {
            const auto pieces = std::count(line.begin(), line.end(), '\n') + 1;
            const auto flag = static_cast<std::uint8_t>(objectDepth > 0 ? 1 : 0);
            for (std::ptrdiff_t i = 0; i < pieces; ++i) innerCurrent.push_back(flag);
        }
        y += pitch;
    }

    // 글자가 들어갈 틈이 없으면 빈 줄은 만들지 않고 세로 위치만 내린다.
    bool skipBand(std::int32_t pitch) {
        const std::int32_t beforeY = y;
        const int beforePage = page;
        if (y > 0 && y + pitch > contentHeight) startPage();
        else y += pitch;
        return y != beforeY || page != beforePage;
    }
};

struct Interval {
    std::int32_t left = 0;
    std::int32_t right = 0;
};

// 이 줄의 세로 범위와 겹치는 어울림 개체를 빼고, 글자가 들어갈 가로 구간을 왼쪽부터 돌려준다.
std::vector<Interval> freeIntervals(const Flow& flow, std::int32_t y, std::int32_t pitch,
                                    std::int32_t left, std::int32_t right,
                                    const std::vector<Obstacle>& obstacles) {
    std::vector<Interval> free{{left, right}};
    for (const auto& obstacle : obstacles) {
        if (obstacle.page != flow.page) continue;
        if (y + pitch <= obstacle.top || y >= obstacle.bottom) continue;
        if (obstacle.blockLine) return {};
        std::vector<Interval> next;
        for (const auto& interval : free) {
            if (obstacle.right <= interval.left || obstacle.left >= interval.right) {
                next.push_back(interval);
                continue;
            }
            const std::int32_t leftWidth =
                obstacle.left > interval.left ? obstacle.left - interval.left : 0;
            const std::int32_t rightWidth =
                obstacle.right < interval.right ? interval.right - obstacle.right : 0;
            bool keepLeft = true;
            bool keepRight = true;
            if (obstacle.textFlow == "LEFT_ONLY") keepRight = false;
            else if (obstacle.textFlow == "RIGHT_ONLY") keepLeft = false;
            else if (obstacle.textFlow == "LARGEST_ONLY") {
                if (leftWidth > rightWidth) keepRight = false;
                else if (rightWidth > leftWidth) keepLeft = false;
            }
            if (keepLeft && leftWidth > 0) next.push_back({interval.left, obstacle.left});
            if (keepRight && rightWidth > 0) next.push_back({obstacle.right, interval.right});
        }
        free = std::move(next);
    }
    free.erase(std::remove_if(free.begin(), free.end(),
                              [](const Interval& interval) { return interval.right <= interval.left; }),
               free.end());
    return free;
}

struct LineBuilder {
    std::string text;
    std::int32_t pen = 0;
    bool firstLine = true;
    int rangeStart = -1;
    int rangeEnd = 0;
};

std::vector<Obstacle> obstaclesIn(const Paragraph& para, const Flow& flow);

class Layouter {
public:
    Layouter(Flow& flow, std::vector<Obstacle> obstacles)
        : flow_(flow), obstacles_(std::move(obstacles)) {}

    void paragraph(const Paragraph& para, std::int32_t width) {
        const ParaShape& shape = paraShapeAt(*flow_.doc, para.paraShapeId);
        const std::int32_t em = emSize(charShapeAt(*flow_.doc, firstCharShape(para)));
        const std::int32_t pitch = linePitch(shape, em);
        LineBuilder line;
        Lang previous = Lang::Hangul;
        const std::int32_t frameLeft = std::max<std::int32_t>(0, shape.marginLeft);
        const std::int32_t frameRight = std::max<std::int32_t>(frameLeft + 1, width - shape.marginRight);
        std::vector<Interval> gaps;
        std::size_t gapIndex = 0;
        std::int32_t pen = 0;
        auto loadGaps = [&] {
            std::int32_t left = frameLeft;
            if (line.firstLine && shape.indent > 0) left += shape.indent;
            if (!line.firstLine && shape.indent < 0) left += -shape.indent;
            gaps = freeIntervals(flow_, flow_.y, pitch, left, frameRight, obstacles_);
            gaps.erase(std::remove_if(gaps.begin(), gaps.end(),
                                      [&](const Interval& interval) {
                                          return interval.right - interval.left < em;
                                      }),
                       gaps.end());
            gapIndex = 0;
            pen = gaps.empty() ? 0 : gaps.front().left;
        };
        bool solid = false;
        std::int32_t spaceSum = 0;
        std::int32_t spaceBase = 0;
        auto finish = [&] {
            flow_.addLine(std::move(line.text), pitch, line.rangeStart, line.rangeEnd);
            line = {};
            line.firstLine = false;
            solid = false;
            spaceSum = 0;
            spaceBase = 0;
            loadGaps();
        };
        loadGaps();
        // 지금까지의 합 + 이번 글자가 줄 너비보다 크면 그 글자 앞에서 끊는다. 같으면 그 줄에 남긴다.
        constexpr std::int32_t kSlack = 0;

        struct Atom {
            std::string text;
            char32_t cp = 0;
            int pos = 0;
            std::int32_t width = 0;
            std::int32_t trail = 0;
            bool space = false;
            bool tab = false;
            bool hard = false;
            bool breakBefore = true;
        };
        auto trailOf = [&](char32_t cp, const CharShape& face, Lang lang) {
            return glyphTrail(cp, face, lang, flow_.doc);
        };
        auto sameWord = [](const Atom& atom, bool first) {
            return first || (!atom.breakBefore && !atom.space && !atom.tab && !atom.hard);
        };
        auto ensureGap = [&] {
            if (!gaps.empty()) return true;
            std::int32_t jump = flow_.y;
            for (const auto& obstacle : obstacles_) {
                if (obstacle.page != flow_.page) continue;
                if (flow_.y < obstacle.bottom && flow_.y + pitch > obstacle.top) {
                    jump = std::max(jump, obstacle.bottom);
                }
            }
            if (jump > flow_.y) {
                flow_.y = jump;
                if (flow_.y >= flow_.contentHeight) flow_.startPage();
                loadGaps();
            }
            return !gaps.empty();
        };
        auto nextGap = [&] {
            ++gapIndex;
            if (gapIndex >= gaps.size()) {
                if (!line.text.empty()) finish();
                else {
                    flow_.y += pitch;
                    if (flow_.y >= flow_.contentHeight) flow_.startPage();
                    loadGaps();
                }
                return;
            }
            pen = gaps[gapIndex].left;
        };
        int textPos = 0;
        auto cover = [&](int pos) {
            if (line.rangeStart < 0 || pos < line.rangeStart) line.rangeStart = pos;
            if (pos + 1 > line.rangeEnd) line.rangeEnd = pos + 1;
        };
        ++flow_.layoutDepth;

        for (const auto& run : para.runs) {
            const CharShape& face = charShapeAt(*flow_.doc, run.charShapeId);
            for (const auto& item : run.items) {
                if (item.type == InlineItem::Type::Control) {
                    emitControl(item.control, width, line, pitch, finish);
                    continue;
                }
                const bool latinChar = shape.breakLatinWord == "BREAK_WORD";
                const bool hyphen = shape.breakLatinWord == "HYPHENATION";
                std::vector<Atom> atoms;
                std::size_t index = 0;
                bool latinRun = false;
                while (index < item.text.size()) {
                    char32_t cp = 0;
                    if (!nextCodePoint(item.text, index, cp)) break;
                    Atom atom;
                    atom.cp = cp;
                    atom.pos = textPos++;
                    appendUtf8(atom.text, cp);
                    if (cp == '\n') {
                        atom.hard = true;
                        latinRun = false;
                        atoms.push_back(std::move(atom));
                        continue;
                    }
                    if (cp == ' ' || cp == 0x2007) {
                        atom.space = true;
                        atom.width = glyphAdvance(cp, face, Lang::Latin, flow_.doc);
                        atom.trail = trailOf(cp, face, Lang::Latin);
                        latinRun = false;
                        atoms.push_back(std::move(atom));
                        continue;
                    }
                    if (cp == '\t') {
                        atom.tab = true;
                        latinRun = false;
                        atoms.push_back(std::move(atom));
                        continue;
                    }
                    Lang lang = languageOf(cp);
                    if (followsPreviousLanguage(cp)) lang = previous;
                    else previous = lang;
                    atom.width = glyphAdvance(cp, face, lang, flow_.doc);
                    atom.trail = trailOf(cp, face, lang);
                    const bool hangul = lang == Lang::Hangul && isHangul(cp);
                    const bool cjk = lang == Lang::Hanja || lang == Lang::Japanese;
                    const bool latin = isLatinWordChar(cp) || cp == '-';
                    if (lineHeadForbidden(cp) && !atoms.empty() && !atoms.back().space &&
                        !atoms.back().tab && !atoms.back().hard) {
                        atom.breakBefore = false;
                        latinRun = false;
                    } else if (cjk || hangul) {
                        atom.breakBefore = true;
                        latinRun = false;
                    } else if (latin && !latinChar) {
                        atom.breakBefore = !latinRun || (hyphen && atoms.empty());
                        if (hyphen && !atoms.empty() && atoms.back().text == "-") atom.breakBefore = true;
                        latinRun = true;
                        if (hyphen && cp == '-') latinRun = false;
                    } else {
                        atom.breakBefore = true;
                        latinRun = false;
                    }
                    atoms.push_back(std::move(atom));
                }

                std::size_t cursor = 0;
                while (cursor < atoms.size()) {
                    const Atom& atom = atoms[cursor];
                    if (atom.hard) {
                        cover(atom.pos);
                        finish();
                        ++cursor;
                        continue;
                    }
                    if (shape.lineWrap == "KEEP") {
                        std::int32_t advance = atom.width;
                        if (atom.tab) {
                            advance = std::max<std::int32_t>(0, ((pen / kDefaultTab) + 1) * kDefaultTab - pen);
                        }
                        cover(atom.pos);
                        line.text += atom.text;
                        pen += advance;
                        solid = !atom.space;
                        line.pen = pen;
                        ++cursor;
                        continue;
                    }
                    if (!ensureGap()) {
                        cover(atom.pos);
                        line.text += atom.text;
                        pen += std::max<std::int32_t>(atom.width, 1);
                        solid = !atom.space;
                        finish();
                        ++cursor;
                        continue;
                    }
                    if (atom.space) {
                        if (!solid) {
                            cover(atom.pos);
                            ++cursor;
                            continue;
                        }
                        cover(atom.pos);
                        line.text += atom.text;
                        pen += atom.width;
                        if (atom.text != "\u2007") {
                            spaceSum += atom.width;
                            spaceBase += atom.width;
                        }
                        line.pen = pen;
                        ++cursor;
                        continue;
                    }
                    if (atom.tab) {
                        const std::int32_t advance =
                            std::max<std::int32_t>(0, ((pen / kDefaultTab) + 1) * kDefaultTab - pen);
                        cover(atom.pos);
                        line.text += atom.text;
                        pen += advance;
                        solid = true;
                        line.pen = pen;
                        ++cursor;
                        continue;
                    }
                    if (isOverflowGap(atom.cp) && !atom.space) {
                        cover(atom.pos);
                        line.text += atom.text;
                        pen += atom.width;
                        solid = true;
                        line.pen = pen;
                        ++cursor;
                        continue;
                    }
                    if (lineHeadForbidden(atom.cp)) {
                        const std::int32_t one = atom.width - atom.trail;
                        const bool fits = pen + one <= gaps[gapIndex].right + kSlack;
                        const bool lineFull = pen >= gaps[gapIndex].right + kSlack;
                        if (fits || (lineFull && solid)) {
                            cover(atom.pos);
                            line.text += atom.text;
                            pen += atom.width;
                            solid = true;
                            line.pen = pen;
                            ++cursor;
                            continue;
                        }
                        if (shape.condense > 0 && solid && spaceBase > 0) {
                            const std::int32_t limit = std::min<std::int32_t>(shape.condense, 75);
                            const std::int32_t used = spaceBase - spaceSum;
                            const std::int32_t budget = static_cast<std::int32_t>(
                                                            static_cast<std::int64_t>(spaceBase) * limit / 100) -
                                                        used;
                            if (budget > 0 && pen - budget + one <= gaps[gapIndex].right + kSlack) {
                                pen -= budget;
                                spaceSum -= budget;
                                cover(atom.pos);
                                line.text += atom.text;
                                pen += atom.width;
                                solid = true;
                                line.pen = pen;
                                ++cursor;
                                continue;
                            }
                        }
                    }

                    const std::int32_t gapWidth = gaps[gapIndex].right - gaps[gapIndex].left;
                    std::size_t end = cursor;
                    std::int32_t wordWidth = 0;
                    std::int32_t wordFit = 0;
                    bool longer = false;
                    while (end < atoms.size() && sameWord(atoms[end], end == cursor)) {
                        if (end != cursor && lineHeadForbidden(atoms[end].cp)) break;
                        wordWidth += atoms[end].width;
                        wordFit = wordWidth - atoms[end].trail;
                        ++end;
                        if (wordFit > gapWidth + kSlack) {
                            longer = true;
                            break;
                        }
                    }
                    if (!longer && pen + wordFit <= gaps[gapIndex].right + kSlack) {
                        for (; cursor < end; ++cursor) {
                            cover(atoms[cursor].pos);
                            line.text += atoms[cursor].text;
                            pen += atoms[cursor].width;
                        }
                        solid = true;
                        line.pen = pen;
                        continue;
                    }
                    if (!longer && shape.condense > 0 && solid && spaceBase > 0) {
                        const std::int32_t limit = std::min<std::int32_t>(shape.condense, 75);
                        const std::int32_t used = spaceBase - spaceSum;
                        const std::int32_t budget = static_cast<std::int32_t>(
                                                       static_cast<std::int64_t>(spaceBase) * limit / 100) -
                                                   used;
                        if (budget > 0 && pen - budget + wordFit <= gaps[gapIndex].right + kSlack) {
                            pen -= budget;
                            spaceSum -= budget;
                            for (; cursor < end; ++cursor) {
                                cover(atoms[cursor].pos);
                                line.text += atoms[cursor].text;
                                pen += atoms[cursor].width;
                            }
                            solid = true;
                            line.pen = pen;
                            continue;
                        }
                    }
                    if (!longer) {
                        if (solid || gapIndex + 1 < gaps.size()) nextGap();
                        else finish();
                        continue;
                    }
                    if (pen + atoms[cursor].width - atoms[cursor].trail > gaps[gapIndex].right + kSlack) {
                        if (solid || gapIndex + 1 < gaps.size()) nextGap();
                        else {
                            cover(atoms[cursor].pos);
                            line.text += atoms[cursor].text;
                            pen += std::max<std::int32_t>(atoms[cursor].width, 1);
                            solid = true;
                            line.pen = pen;
                            ++cursor;
                            finish();
                        }
                        continue;
                    }
                    cover(atoms[cursor].pos);
                    line.text += atoms[cursor].text;
                    pen += atoms[cursor].width;
                    solid = true;
                    line.pen = pen;
                    ++cursor;
                }
            }
        }
        if (!line.text.empty() || para.runs.empty()) {
            const int start = line.rangeStart < 0 ? 0 : line.rangeStart;
            const int end = line.rangeStart < 0 ? 0 : line.rangeEnd;
            flow_.addLine(std::move(line.text), pitch, start, end);
        }
        --flow_.layoutDepth;
    }

private:
    static std::uint32_t firstCharShape(const Paragraph& para) {
        for (const auto& run : para.runs) {
            if (!run.items.empty()) return run.charShapeId;
        }
        return 0;
    }

    void emitControl(const Control& control, std::int32_t width, LineBuilder& line,
                     std::int32_t pitch, const auto& finish) {
        const bool shape = control.kind == ControlKind::Shape || control.kind == ControlKind::Picture ||
                           control.kind == ControlKind::Container;
        if (shape && flow_.options->includeShapeText) {
            if (!line.text.empty()) finish();
            const std::int32_t savedY = flow_.y;
            const int savedPage = flow_.page;
            flow_.y = 0;
            const std::int32_t box = innerWidth(control, width);
            if (flow_.options->includeCaptions && control.caption) {
                for (const auto& para : control.caption->content.paragraphs) paragraph(para, box);
            }
            std::vector<Obstacle> nested;
            for (const auto& sub : control.subLists) {
                for (const auto& para : sub.paragraphs) {
                    for (const auto& obstacle : obstaclesIn(para, flow_)) nested.push_back(obstacle);
                }
            }
            ++flow_.objectDepth;
            Layouter inner(flow_, std::move(nested));
            for (const auto& sub : control.subLists) {
                for (const auto& para : sub.paragraphs) inner.paragraph(para, box);
            }
            for (const auto& child : control.children) {
                LineBuilder ignored;
                emitControl(child, box, ignored, pitch, finish);
            }
            --flow_.objectDepth;
            flow_.y = savedY;
            flow_.page = savedPage;
            return;
        }
        if (flow_.options->includeCaptions && control.caption) {
            if (!line.text.empty()) finish();
            for (const auto& para : control.caption->content.paragraphs) paragraph(para, width);
        }
        if (control.kind == ControlKind::Table && control.table) {
            if (!line.text.empty()) finish();
            ++flow_.objectDepth;
            for (const auto& cell : control.table->cells) {
                const std::int32_t cellWidth =
                    cell.width > 0 ? static_cast<std::int32_t>(cell.width) : width;
                for (const auto& para : cell.content.paragraphs) paragraph(para, cellWidth);
            }
            --flow_.objectDepth;
            return;
        }
        if (control.kind == ControlKind::Dutmal || control.kind == ControlKind::Compose) {
            line.text += control.text;
            return;
        }
        if (control.kind == ControlKind::Equation && flow_.options->includeEquations &&
            !control.script.empty()) {
            if (line.pen > 0) finish();
            flow_.addLine(control.script, pitch);
            return;
        }
        if ((control.kind == ControlKind::Header || control.kind == ControlKind::Footer) &&
            !flow_.options->includeHeaderFooter) {
            return;
        }
        if ((control.kind == ControlKind::Footnote || control.kind == ControlKind::Endnote) &&
            !flow_.options->includeNotes) {
            return;
        }
        if (shape) return;
        for (const auto& sub : control.subLists) {
            for (const auto& para : sub.paragraphs) paragraph(para, width);
        }
    }

    Flow& flow_;
    std::vector<Obstacle> obstacles_;
};

std::vector<Obstacle> obstaclesIn(const Paragraph& para, const Flow& flow) {
    std::vector<Obstacle> obstacles;
    for (const auto& run : para.runs) {
        for (const auto& item : run.items) {
            if (item.type != InlineItem::Type::Control || !item.control.placement) continue;
            const Placement& placement = *item.control.placement;
            if (!affectsFlow(placement)) continue;
            Obstacle obs = obstacleFrom(placement, 0, flow.page, flow.contentLeft, flow.contentTop,
                                        flow.contentWidth, flow.contentHeight, flow.pageWidth,
                                        flow.pageHeight);
            if (placement.vertRelTo == "PARA" && placement.flowWithText) {
                const std::int32_t spanH = obs.bottom - obs.top;
                if (obs.top < 0) {
                    obs.top = 0;
                    obs.bottom = spanH;
                }
                for (std::size_t guard = 0; guard <= obstacles.size(); ++guard) {
                    bool moved = false;
                    for (const auto& prev : obstacles) {
                        if (prev.page != obs.page) continue;
                        const bool xOverlap =
                            obs.blockLine || prev.blockLine || (obs.left < prev.right && obs.right > prev.left);
                        if (!xOverlap) continue;
                        if (obs.top < prev.bottom && obs.bottom > prev.top && prev.bottom > obs.top) {
                            obs.top = prev.bottom;
                            obs.bottom = obs.top + spanH;
                            moved = true;
                            break;
                        }
                    }
                    if (!moved) break;
                }
            }
            obstacles.push_back(obs);
        }
    }
    return obstacles;
}

}  // namespace

struct GlyphScale {
    std::int32_t scaled = 0;
    std::int32_t spacing = 0;
};

// 0.5는 0에서 멀어지는 쪽으로 올린다.
std::int32_t roundAway(std::int64_t numer, std::int64_t denom) {
    if (denom < 0) {
        numer = -numer;
        denom = -denom;
    }
    if (numer >= 0) return static_cast<std::int32_t>((numer + denom / 2) / denom);
    return -static_cast<std::int32_t>(((-numer) + denom / 2) / denom);
}

// 장평 100: 4 × 반올림(advance × ⌊크기 ÷ 4⌋ ÷ tVt)
// 장평이 100이 아니면 장평을 분자에 넣고 나눗셈을 버린다.
std::int32_t steppedWidth(std::int32_t advance, std::int32_t em, std::int32_t units, std::int32_t ratio) {
    if (advance <= 0 || em <= 0 || units <= 0) return 0;
    const std::int32_t quant = em / 4;
    if (ratio == 100) {
        const std::int64_t numer = static_cast<std::int64_t>(advance) * quant;
        return static_cast<std::int32_t>(((numer + units / 2) / units) * 4);
    }
    const std::int64_t numer = static_cast<std::int64_t>(advance) * quant * ratio;
    const std::int64_t denom = static_cast<std::int64_t>(100) * units;
    return static_cast<std::int32_t>((numer / denom) * 4);
}

std::int32_t widthSize(const CharShape& shape, Lang lang) {
    std::int32_t em = shape.height > 0 ? shape.height : 1000;
    const std::size_t slot = static_cast<std::size_t>(lang);
    std::int32_t rel = slot < shape.relSize.size() ? shape.relSize[slot] : 100;
    if (rel == 0) rel = 100;
    if (rel != 100) em = roundAway(static_cast<std::int64_t>(em) * rel, 100);
    if (shape.superscript || shape.subscript) em = roundAway(static_cast<std::int64_t>(em) * 16, 25);
    return em;
}

std::int32_t langNumber(const PerLang<std::int32_t>& values, Lang lang) {
    const std::size_t slot = static_cast<std::size_t>(lang);
    return slot < values.size() ? values[slot] : 0;
}

// 자간은 4 × 반올림((폭 ÷ 4) × 자간 ÷ 100). 0.5는 0에서 멀어진다.
std::int32_t letterSpacing(std::int32_t width, std::int32_t percent) {
    if (percent == 0) return 0;
    return roundAway((static_cast<std::int64_t>(width) / 4) * percent, 100) * 4;
}

// 장평 100이면 4 × 몫이다. 그 밖은 몫에 장평을 곱해 반올림한 뒤 4를 곱한다.
std::int32_t blankWidth(std::int32_t quant, std::int32_t ratio) {
    if (ratio == 0) ratio = 100;
    if (ratio == 100) return quant * 4;
    return roundAway(static_cast<std::int64_t>(quant) * ratio, 100) * 4;
}

struct ScaleKey {
    char32_t cp = 0;
    std::uint8_t lang = 0;
    std::uint8_t flags = 0;
    std::int32_t height = 0;
    std::int32_t relH = 0;
    std::int32_t relL = 0;
    std::int32_t ratioH = 0;
    std::int32_t ratioL = 0;
    std::int32_t spaceH = 0;
    std::int32_t spaceL = 0;
    std::string face;
    std::string subst;
    std::string base;
};

struct ScaleView {
    char32_t cp = 0;
    std::uint8_t lang = 0;
    std::uint8_t flags = 0;
    std::int32_t height = 0;
    std::int32_t relH = 0;
    std::int32_t relL = 0;
    std::int32_t ratioH = 0;
    std::int32_t ratioL = 0;
    std::int32_t spaceH = 0;
    std::int32_t spaceL = 0;
    std::string_view face;
    std::string_view subst;
    std::string_view base;
};

struct ScaleHash {
    using is_transparent = void;
    template <typename T>
    std::size_t operator()(const T& key) const noexcept {
        std::size_t h = std::hash<char32_t>{}(key.cp);
        auto mix = [&](std::size_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); };
        mix(key.lang);
        mix(key.flags);
        mix(static_cast<std::uint32_t>(key.height));
        mix(static_cast<std::uint32_t>(key.relH));
        mix(static_cast<std::uint32_t>(key.relL));
        mix(static_cast<std::uint32_t>(key.ratioH));
        mix(static_cast<std::uint32_t>(key.ratioL));
        mix(static_cast<std::uint32_t>(key.spaceH));
        mix(static_cast<std::uint32_t>(key.spaceL));
        mix(std::hash<std::string_view>{}(key.face));
        mix(std::hash<std::string_view>{}(key.subst));
        mix(std::hash<std::string_view>{}(key.base));
        return h;
    }
};

struct ScaleEq {
    using is_transparent = void;
    template <typename A, typename B>
    bool operator()(const A& a, const B& b) const noexcept {
        return a.cp == b.cp && a.lang == b.lang && a.flags == b.flags && a.height == b.height && a.relH == b.relH &&
               a.relL == b.relL && a.ratioH == b.ratioH && a.ratioL == b.ratioL && a.spaceH == b.spaceH &&
               a.spaceL == b.spaceL && a.face == b.face && a.subst == b.subst && a.base == b.base;
    }
};

using ScaleMap = std::unordered_map<ScaleKey, GlyphScale, ScaleHash, ScaleEq>;

ScaleMap gScaleCache;
std::shared_mutex gScaleMu;
std::atomic<bool> gScaleCacheOn{false};

std::int32_t langRaw(const PerLang<std::int32_t>& values, Lang lang) {
    const std::size_t slot = static_cast<std::size_t>(lang);
    return slot < values.size() ? values[slot] : 0;
}

template <typename String>
auto makeScaleKey(const CharShape& shape, const Document* doc, char32_t cp, Lang lang) {
    struct Box {
        char32_t cp = 0;
        std::uint8_t lang = 0;
        std::uint8_t flags = 0;
        std::int32_t height = 0;
        std::int32_t relH = 0;
        std::int32_t relL = 0;
        std::int32_t ratioH = 0;
        std::int32_t ratioL = 0;
        std::int32_t spaceH = 0;
        std::int32_t spaceL = 0;
        String face;
        String subst;
        String base;
    };
    static const std::string empty;
    const std::string* face = &empty;
    const std::string* subst = &empty;
    const std::string* base = &empty;
    const std::size_t slot = static_cast<std::size_t>(lang);
    if (doc && slot < doc->header.fonts.size() && slot < shape.fontRef.size()) {
        const auto& fonts = doc->header.fonts[slot];
        const std::uint32_t id = shape.fontRef[slot];
        if (id < fonts.size()) {
            face = &fonts[id].face;
            subst = &fonts[id].subst;
            base = &fonts[id].base;
        }
    }
    Box key;
    key.cp = cp;
    key.lang = static_cast<std::uint8_t>(lang);
    key.flags = static_cast<std::uint8_t>((shape.useFontSpace ? 1 : 0) | (shape.superscript ? 2 : 0) |
                                          (shape.subscript ? 4 : 0));
    key.height = shape.height;
    key.relH = langRaw(shape.relSize, Lang::Hangul);
    key.relL = langRaw(shape.relSize, Lang::Latin);
    key.ratioH = langRaw(shape.ratio, Lang::Hangul);
    key.ratioL = langRaw(shape.ratio, Lang::Latin);
    key.spaceH = langRaw(shape.spacing, Lang::Hangul);
    key.spaceL = langRaw(shape.spacing, Lang::Latin);
    key.face = *face;
    key.subst = *subst;
    key.base = *base;
    return key;
}

ScaleKey ownedKey(const CharShape& shape, const Document* doc, char32_t cp, Lang lang) {
    const auto view = makeScaleKey<std::string>(shape, doc, cp, lang);
    ScaleKey key;
    key.cp = view.cp;
    key.lang = view.lang;
    key.flags = view.flags;
    key.height = view.height;
    key.relH = view.relH;
    key.relL = view.relL;
    key.ratioH = view.ratioH;
    key.ratioL = view.ratioL;
    key.spaceH = view.spaceH;
    key.spaceL = view.spaceL;
    key.face = view.face;
    key.subst = view.subst;
    key.base = view.base;
    return key;
}

ScaleView viewKey(const CharShape& shape, const Document* doc, char32_t cp, Lang lang) {
    const auto view = makeScaleKey<std::string_view>(shape, doc, cp, lang);
    ScaleView key;
    key.cp = view.cp;
    key.lang = view.lang;
    key.flags = view.flags;
    key.height = view.height;
    key.relH = view.relH;
    key.relL = view.relL;
    key.ratioH = view.ratioH;
    key.ratioL = view.ratioL;
    key.spaceH = view.spaceH;
    key.spaceL = view.spaceL;
    key.face = view.face;
    key.subst = view.subst;
    key.base = view.base;
    return key;
}

std::optional<GlyphScale> scaleIfReady(char32_t codePoint, const CharShape& shape, Lang lang, const Document* doc) {
    if ((codePoint == ' ' || codePoint == 0x00A0) && shape.useFontSpace) lang = Lang::Latin;
    if (((codePoint == ' ' || codePoint == 0x00A0) && !shape.useFontSpace) || codePoint == 0x2007) {
        const std::int32_t em = widthSize(shape, Lang::Hangul);
        const std::int32_t quant = (codePoint == ' ' || codePoint == 0x00A0) ? em / 8 : (em / 4) / 4;
        GlyphScale out;
        out.scaled = blankWidth(quant, langNumber(shape.ratio, Lang::Hangul));
        const std::int32_t spacing = langNumber(shape.spacing, Lang::Latin);
        if (codePoint >= 31 && spacing != 0) out.spacing = letterSpacing(out.scaled, spacing);
        return out;
    }
    const std::int32_t em = widthSize(shape, lang);
    const std::size_t slot = static_cast<std::size_t>(lang);
    std::int32_t ratio = slot < shape.ratio.size() ? shape.ratio[slot] : 100;
    const std::int32_t spacing = slot < shape.spacing.size() ? shape.spacing[slot] : 0;
    if (ratio == 0) ratio = 100;
    std::string faceName;
    std::string subst;
    std::string baseFont;
    if (doc && slot < doc->header.fonts.size()) {
        const auto& fonts = doc->header.fonts[slot];
        const std::uint32_t id = slot < shape.fontRef.size() ? shape.fontRef[slot] : 0;
        if (id < fonts.size()) {
            faceName = fonts[id].face;
            subst = fonts[id].subst;
            baseFont = fonts[id].base;
        }
    }
    auto finish = [&](std::int32_t advance, std::int32_t units) {
        GlyphScale out;
        out.scaled = steppedWidth(advance, em, units, ratio);
        if (codePoint >= 31 && spacing != 0) out.spacing = letterSpacing(out.scaled, spacing);
        return out;
    };
    if (faceName.empty()) {
        const std::int32_t units = 1024;
        const std::int32_t advance = isFullwidth(codePoint) ? units : units / 2;
        return finish(advance, units);
    }
    const ReadyGlyph measured = measureReadyGlyph(faceName, codePoint, subst, baseFont);
    if (!measured.known) return std::nullopt;
    std::int32_t advance = measured.advance;
    std::int32_t units = 1024;
    faceName = measured.resolved;
    if (advance >= 0) units = measured.units > 0 ? measured.units : static_cast<std::int32_t>(fontTableEm(faceName));
    if (advance < 0 && faceName != "함초롬바탕") {
        const ReadyGlyph ham = measureReadyGlyph("함초롬바탕", codePoint, {}, {});
        if (!ham.known) return std::nullopt;
        advance = ham.advance;
        if (advance >= 0) units = ham.units > 0 ? ham.units : static_cast<std::int32_t>(fontTableEm("함초롬바탕"));
    }
    if (advance < 0) {
        units = 1024;
        advance = isFullwidth(codePoint) ? units : units / 2;
    }
    return finish(advance, units);
}

void rememberScale(const CharShape& shape, const Document* doc, char32_t cp, Lang lang, const GlyphScale& scale) {
    if (!gScaleCacheOn.load()) return;
    std::unique_lock lock(gScaleMu);
    if (!gScaleCacheOn.load()) return;
    gScaleCache.emplace(ownedKey(shape, doc, cp, lang), scale);
}

GlyphScale glyphScale(char32_t codePoint, const CharShape& shape, Lang lang, const Document* doc) {
    // 글꼴에 어울리는 빈칸이 켜지면 띄어쓰기만 영문 글꼴의 일반 폭을 쓴다.
    if (codePoint == ' ' && shape.useFontSpace) lang = Lang::Latin;
    if (gScaleCacheOn.load()) {
        const ScaleView view = viewKey(shape, doc, codePoint, lang);
        std::shared_lock lock(gScaleMu);
        if (const auto found = gScaleCache.find(view); found != gScaleCache.end()) return found->second;
    }
    if (const auto ready = scaleIfReady(codePoint, shape, lang, doc)) {
        rememberScale(shape, doc, codePoint, lang, *ready);
        return *ready;
    }
    if (((codePoint == ' ' || codePoint == 0x00A0) && !shape.useFontSpace) || codePoint == 0x2007) {
        const std::int32_t em = widthSize(shape, Lang::Hangul);
        const std::int32_t quant = (codePoint == ' ' || codePoint == 0x00A0) ? em / 8 : (em / 4) / 4;
        GlyphScale out;
        out.scaled = blankWidth(quant, langNumber(shape.ratio, Lang::Hangul));
        const std::int32_t spacing = langNumber(shape.spacing, Lang::Latin);
        if (codePoint >= 31 && spacing != 0) out.spacing = letterSpacing(out.scaled, spacing);
        rememberScale(shape, doc, codePoint, lang, out);
        return out;
    }
    const std::int32_t em = widthSize(shape, lang);
    const std::size_t slot = static_cast<std::size_t>(lang);
    std::int32_t ratio = slot < shape.ratio.size() ? shape.ratio[slot] : 100;
    const std::int32_t spacing = slot < shape.spacing.size() ? shape.spacing[slot] : 0;
    if (ratio == 0) ratio = 100;
    std::int32_t advance = -1;
    std::int32_t units = 1024;
    std::string faceName;
    if (doc && slot < doc->header.fonts.size()) {
        const auto& fonts = doc->header.fonts[slot];
        const std::uint32_t id = slot < shape.fontRef.size() ? shape.fontRef[slot] : 0;
        if (id < fonts.size()) faceName = fonts[id].face;
    }
    const FontLoadScope fontWhy("글자폭", codePoint, faceName.empty() ? nullptr : &faceName);
    if (advance < 0 && !faceName.empty()) {
        std::string subst;
        std::string baseFont;
        if (doc && slot < doc->header.fonts.size()) {
            const auto& fonts = doc->header.fonts[slot];
            const std::uint32_t id = slot < shape.fontRef.size() ? shape.fontRef[slot] : 0;
            if (id < fonts.size()) {
                subst = fonts[id].subst;
                baseFont = fonts[id].base;
            }
        }
        // 글꼴에 글자가 없으면 문서의 대체 글꼴, 없으면 같은 계열 한컴 글꼴로 폭을 잰다.
        faceName = fontWithGlyph(faceName, codePoint, subst, baseFont);
        advance = fontGlyphDesignAdvance(faceName, codePoint);
        if (advance >= 0) {
            const int upm = fontUnitsPerEm(faceName);
            units = upm > 0 ? upm : static_cast<int>(fontTableEm(faceName));
        }
    }
    if (advance < 0 && !faceName.empty() && faceName != "함초롬바탕") {
        const FontLoadStep ham("없는글꼴대체");
        advance = fontGlyphDesignAdvance("함초롬바탕", codePoint);
        (void)ham;
        if (advance >= 0) {
            const int upm = fontUnitsPerEm("함초롬바탕");
            units = upm > 0 ? upm : static_cast<int>(fontTableEm("함초롬바탕"));
        }
    }
    if (advance < 0) {
        units = 1024;
        advance = isFullwidth(codePoint) ? units : units / 2;
    }
    GlyphScale out;
    out.scaled = steppedWidth(advance, em, units, ratio);
    // 코드 31 미만이거나 자간이 0이면 더하지 않는다.
    if (codePoint >= 31 && spacing != 0) out.spacing = letterSpacing(out.scaled, spacing);
    rememberScale(shape, doc, codePoint, lang, out);
    return out;
}

std::int32_t glyphAdvance(char32_t codePoint, const CharShape& shape, Lang lang,
                           const Document* doc) {
    const GlyphScale part = glyphScale(codePoint, shape, lang, doc);
    return part.scaled + part.spacing;
}

std::int32_t glyphTrail(char32_t codePoint, const CharShape& shape, Lang lang,
                        const Document* doc) {
    return glyphScale(codePoint, shape, lang, doc).spacing;
}

const CharShape* shapeById(const Document& doc, std::uint32_t id) {
    const auto& shapes = doc.header.charShapes;
    if (id < shapes.size() && shapes[id].id == id) return &shapes[id];
    for (const auto& shape : shapes) {
        if (shape.id == id) return &shape;
    }
    return nullptr;
}

Lang shapeLang(const CharShape& shape, char32_t cp) {
    Lang lang = isHangul(cp) ? Lang::Hangul : Lang::Latin;
    if (cp == ' ' && shape.useFontSpace) lang = Lang::Latin;
    return lang;
}

void measureParagraph(const Document& doc, const Paragraph& para, ScaleMap& seen, ScaleMap* fresh);

void measureControl(const Document& doc, const Control& control, ScaleMap& seen, ScaleMap* fresh) {
    if (control.table) {
        for (const auto& cell : control.table->cells) {
            for (const auto& nested : cell.content.paragraphs) measureParagraph(doc, nested, seen, fresh);
        }
    }
    if (control.caption) {
        for (const auto& nested : control.caption->content.paragraphs) measureParagraph(doc, nested, seen, fresh);
    }
    for (const auto& sub : control.subLists) {
        for (const auto& nested : sub.paragraphs) measureParagraph(doc, nested, seen, fresh);
    }
    for (const auto& child : control.children) measureControl(doc, child, seen, fresh);
}

void measureParagraph(const Document& doc, const Paragraph& para, ScaleMap& seen, ScaleMap* fresh) {
    CharShape fallback;
    fallback.height = 1000;
    fallback.ratio.fill(100);
    for (const auto& run : para.runs) {
        const CharShape* shape = shapeById(doc, run.charShapeId);
        if (!shape) shape = &fallback;
        for (const auto& item : run.items) {
            if (item.type == InlineItem::Type::Control) {
                measureControl(doc, item.control, seen, fresh);
                continue;
            }
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
                if (cp == '\n') continue;
                const Lang lang = shapeLang(*measure, cp);
                const ScaleView view = viewKey(*measure, &doc, cp, lang);
                if (seen.find(view) != seen.end()) continue;
                const auto ready = scaleIfReady(cp, *measure, lang, &doc);
                if (!ready) continue;
                ScaleKey key = ownedKey(*measure, &doc, cp, lang);
                seen.emplace(key, *ready);
                if (fresh) fresh->emplace(std::move(key), *ready);
            }
        }
    }
}

namespace {
// 셸 Host 가 정적 초기화 중에 넣을 수 있으니 상수로 초기화되는 원자 값이다.
std::atomic<int> gWorkerCores{-1};
std::atomic<int> gWorkerMin{8};
std::atomic<int> gWorkerMax{0};
}  // namespace

void setWorkerLimits(const WorkerLimits& limits) {
    gWorkerCores.store(limits.cores);
    gWorkerMin.store(std::max(1, limits.minWorkers));
    gWorkerMax.store(std::max(0, limits.maxWorkers));
}

int workerCount(int jobs) {
    if (jobs <= 1) return 1;
    int cores = gWorkerCores.load();
    if (cores == 0) return 1;
    if (cores < 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        cores = hardware == 0 ? 8 : static_cast<int>(hardware);
    }
    int workers = std::max(gWorkerMin.load(), cores - 1);
    if (const int most = gWorkerMax.load(); most > 0) workers = std::min(workers, most);
    return std::min(workers, jobs);
}

void prepareParagraphGlyphs(const Document& doc) {
    gScaleCacheOn.store(false);
    {
        std::unique_lock lock(gScaleMu);
        gScaleCache.clear();
    }
    warmDocumentFonts(doc);
    std::vector<const Paragraph*> jobs;
    for (const auto& section : doc.sections) {
        for (const auto& para : section.paragraphs) jobs.push_back(&para);
        for (const auto& master : section.masterPages) {
            for (const auto& para : master.content.paragraphs) jobs.push_back(&para);
        }
    }
    const int count = static_cast<int>(jobs.size());
    if (count <= 0) {
        gScaleCacheOn.store(true);
        return;
    }
    const int threads = workerCount(count);
    if (threads <= 1) {
        ScaleMap seen;
        for (const Paragraph* para : jobs) measureParagraph(doc, *para, seen, nullptr);
        {
            std::unique_lock lock(gScaleMu);
            gScaleCache = std::move(seen);
        }
        gScaleCacheOn.store(true);
        return;
    }
    std::vector<ScaleMap> workerMaps(static_cast<std::size_t>(threads));
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(threads));
    const int chunk = (count + threads - 1) / threads;
    for (int t = 0; t < threads; ++t) {
        const int begin = t * chunk;
        const int end = std::min(count, begin + chunk);
        if (begin >= end) break;
        pool.emplace_back([&, t, begin, end] {
            ScaleMap seen;
            for (int i = begin; i < end; ++i) {
                try {
                    measureParagraph(doc, *jobs[static_cast<std::size_t>(i)], seen, nullptr);
                } catch (...) {
                }
            }
            workerMaps[static_cast<std::size_t>(t)] = std::move(seen);
        });
    }
    for (auto& worker : pool) {
        if (worker.joinable()) worker.join();
    }
    {
        std::unique_lock lock(gScaleMu);
        for (auto& map : workerMaps) {
            for (auto& item : map) gScaleCache.emplace(std::move(item.first), item.second);
        }
    }
    gScaleCacheOn.store(true);
}

void finishParagraphGlyphs() {
    gScaleCacheOn.store(false);
    std::unique_lock lock(gScaleMu);
    gScaleCache.clear();
}

void retainGlyphScaleCache() { gScaleCacheOn.store(true); }

void clearGlyphScaleCache() {
    gScaleCacheOn.store(false);
    std::unique_lock lock(gScaleMu);
    gScaleCache.clear();
}

std::vector<std::string> paginate(const Document& doc, const TextOptions& options,
                                  std::vector<int>* anchorPages,
                                  std::vector<std::vector<std::uint8_t>>* innerLines,
                                  std::vector<std::vector<int>>* paragraphPages,
                                  std::vector<std::vector<LineRange>>* lineRanges) {
    warmDocumentFonts(doc);
    std::vector<std::string> pages;
    std::string current;
    Flow flow;
    flow.doc = &doc;
    flow.options = &options;
    flow.current = &current;
    flow.pages = &pages;
    flow.innerOut = innerLines;
    flow.paragraphPages = paragraphPages;
    flow.lineRanges = lineRanges;
    int ordinal = 0;
    for (const auto& section : doc.sections) {
        const PageDef page = section.flowPage(1);
        flow.pageWidth = static_cast<std::int32_t>(page.width);
        flow.pageHeight = static_cast<std::int32_t>(page.height);
        flow.contentWidth = flow.pageWidth - static_cast<std::int32_t>(page.marginLeft) -
                            static_cast<std::int32_t>(page.marginRight);
        // 쪽(본문)의 위쪽 시작은 종이 끝에서 위 여백과 머리말 여백을 더한 곳이다.
        flow.contentLeft = static_cast<std::int32_t>(page.marginLeft);
        flow.contentTop = static_cast<std::int32_t>(page.marginTop) +
                          static_cast<std::int32_t>(page.marginHeader);
        const std::int32_t body = flow.pageHeight - flow.contentTop -
                                  static_cast<std::int32_t>(page.marginBottom) -
                                  static_cast<std::int32_t>(page.marginFooter);
        flow.contentHeight = body > 0 ? body : flow.pageHeight;
        if (flow.contentWidth <= 0) flow.contentWidth = 42520;
        flow.y = 0;
        flow.page = 0;
        for (const auto& para : section.paragraphs) {
            flow.paragraphIndex = ordinal;
            ++ordinal;
            flow.paragraphLanded = false;
            if (para.pageBreak && !current.empty()) flow.startPage();
            if (anchorPages) {
                for (const auto& run : para.runs) {
                    for (const auto& item : run.items) {
                        if (item.type != InlineItem::Type::Control || !item.control.placement) continue;
                        if (item.control.placement->treatAsChar && item.control.kind != ControlKind::Picture &&
                            item.control.kind != ControlKind::Table && item.control.kind != ControlKind::Shape) {
                            continue;
                        }
                        anchorPages->push_back(flow.page);
                    }
                }
            }
            Layouter layouter(flow, obstaclesIn(para, flow));
            layouter.paragraph(para, flow.contentWidth);
            if (!flow.paragraphLanded) flow.noteParagraph();
        }
        if (!current.empty()) {
            pages.push_back(std::move(current));
            current.clear();
            if (innerLines) innerLines->push_back(std::move(flow.innerCurrent));
        }
    }
    if (pages.empty()) pages.emplace_back();
    if (paragraphPages && paragraphPages->size() < pages.size()) paragraphPages->resize(pages.size());
    if (lineRanges && lineRanges->size() < pages.size()) lineRanges->resize(pages.size());
    return pages;
}

// 한글이 글자마다 고르는 언어를 BMP 전체에서 잰 구간이다. 시작 코드, 그리기용이 아닐 때와 그리기용일 때의 언어다.
model::Lang hancomCharLang(char32_t codePoint, bool drawing) {
    struct Range {
        char32_t first;
        std::uint8_t plain;
        std::uint8_t drawn;
    };
    static constexpr Range kRanges[] = {
        {0x0000, 1, 1}, {0x02B0, 5, 5}, {0x0590, 4, 4}, {0x0700, 5, 5}, {0x1100, 0, 0}, {0x1200, 1, 1},
        {0x2000, 5, 5}, {0x2018, 1, 1}, {0x2020, 5, 5}, {0x3005, 3, 3}, {0x3007, 5, 5}, {0x3012, 3, 3},
        {0x3014, 5, 5}, {0x301E, 3, 3}, {0x3020, 5, 5}, {0x3040, 3, 3}, {0x3100, 2, 2}, {0x3130, 0, 0},
        {0x3190, 5, 5}, {0x3231, 3, 3}, {0x3233, 5, 5}, {0x3239, 3, 3}, {0x323A, 5, 5}, {0x32A4, 3, 3},
        {0x32A9, 5, 5}, {0x32D0, 3, 3}, {0x3380, 5, 5}, {0x33CD, 3, 3}, {0x33CE, 5, 5}, {0x3400, 2, 2},
        {0xA000, 5, 5}, {0xA960, 0, 0}, {0xA980, 5, 5}, {0xAC00, 0, 0}, {0xD800, 1, 1}, {0xD840, 2, 2},
        {0xD880, 5, 5}, {0xE000, 6, 6}, {0xE0BC, 6, 0}, {0xF000, 6, 5}, {0xF100, 6, 0}, {0xF670, 6, 6},
        {0xF780, 6, 0}, {0xF900, 2, 2}, {0xFB00, 4, 4}, {0xFE00, 5, 5}, {0xFE70, 4, 4}, {0xFF00, 5, 5}
    };
    char32_t unit = codePoint;
    // 한글은 UTF-16 한 칸을 넘긴다. BMP 밖 글자는 앞 대리 칸으로 본다.
    if (codePoint > 0xFFFF) unit = 0xD800 + ((codePoint - 0x10000) >> 10);
    const Range* found = &kRanges[0];
    for (const Range& range : kRanges) {
        if (range.first > unit) break;
        found = &range;
    }
    return static_cast<model::Lang>(drawing ? found->drawn : found->plain);
}

}  // namespace chwp::layout
