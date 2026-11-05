#include "render/line_breaker.hpp"

#include "export/font_metrics.hpp"

namespace chwp::render::detail {

using model::Document;
using model::ParaShape;

bool isHangul(char32_t cp) {
    return cp >= 0xAC00 && cp <= 0xD7A3;
}

bool isHangulWordChar(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3);
}

int autoSpacingFlags(const ParaShape* paraShape) {
    if (!paraShape) return 0;
    return (paraShape->autoSpacingEng ? 1 : 0) | (paraShape->autoSpacingNum ? 2 : 0);
}

std::int32_t autoSpacingGap(char32_t left, char32_t right, int flags, int size) {
    if (flags == 0) return 0;
    const bool leftHangul = isHangulWordChar(left);
    if (leftHangul == isHangulWordChar(right)) return 0;
    const char32_t other = leftHangul ? right : left;
    const bool latin = (other >= 'A' && other <= 'Z') || (other >= 'a' && other <= 'z');
    const bool digit = other >= '0' && other <= '9';
    if (((flags & 1) && latin) || ((flags & 2) && digit)) return 2 * (std::max(size, 0) / 8);
    return 0;
}

bool isCjk(char32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x3040 && cp <= 0x30FF);
}

bool isLatinWordChar(char32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
           (cp >= 0x00C0 && cp <= 0x024F);
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
        case 0x3001:
        case 0x3002:
        case 0xFF0C:
        case 0xFF0E:
        case 0xFF09:
        case 0xFF1A:
        case 0xFF1B:
        case 0x2019:
        case 0x201D:
            return true;
        default:
            return false;
    }
}

namespace {

bool wongojiHalf(char32_t cp) {
    if (cp <= 0x20 || cp >= 0x7F) return false;
    return !(cp >= 'A' && cp <= 'Z');
}

bool wongojiMark(char32_t cp) {
    return cp == '.' || cp == ',' || cp == '!' || cp == '?' || cp == ':' || cp == ';' || cp == U'\x27' || cp == U'\x22';
}

}  // namespace

// 세로쓰기 글꼴로 찍을 때 바꾸는 문장 부호 여섯 쌍. 한글 2024 와 같다.
char32_t verticalForm(char32_t cp) {
    switch (cp) {
        case '.': return 0x3002;     // 。
        case ',': return 0x3001;     // 、
        case 0x201C: return 0xFE43;  // “ → ﹃
        case 0x201D: return 0xFE44;  // ” → ﹄
        case 0x2018: return 0xFE41;  // ‘ → ﹁
        case 0x2019: return 0xFE42;  // ’ → ﹂
        default: return cp;
    }
}

namespace {

// 세로쓰기에서 글자를 눕히는지. 참이면 눕힌다. latinSideways 는 영문 눕힘(방향 1)이다.
// 한글은 UTF-16 단위로 묻는다. 보조 평면 글자는 대리 쌍(D800–DFFF)이라 세운다.
bool verticalSideways(char32_t cp, bool latinSideways) {
    switch (cp) {
        // 두 방식 모두 눕힌다.
        case ':': case '(': case ')': case '{': case '}': case '<': case '>': case '[': case ']':
        case '-': case '~': case 0x223C: case 0x30FC: case 0xFF62: case 0xFF63:
            return true;
        default:
            break;
    }
    if ((cp >= 0x2010 && cp <= 0x2015) || (cp >= 0x3008 && cp <= 0x3011) || (cp >= 0x3014 && cp <= 0x301B)) return true;
    if (!latinSideways) return false;
    if ((cp >= 0x20 && cp <= 0x24F) || (cp >= 0x1E00 && cp <= 0x1EFF) || (cp >= 0xFB00 && cp <= 0xFB06) ||
        (cp >= 0xFF60 && cp <= 0xFFEF)) {
        return true;
    }
    // 한글·한자·일본어·사용자 정의 글자는 세운다.
    // 아래 범위 밖에서 그 값이 갈리는 글자는 3012·3013(일본어)뿐이다.
    if (cp == 0x3012 || cp == 0x3013) return false;
    switch (cp) {
        case 0x2016: case 0x2018: case 0x2019: case 0x201C: case 0x201D: case 0x2020: case 0x2021:
        case 0x2030: case 0x2031: case 0x21D2: case 0x21D4:
            return false;
        default:
            break;
    }
    if (cp >= 0x1100 && cp <= 0x11FF) return false;
    if (cp >= 0x2100 && cp <= 0x214F) return cp == 0x2122;
    if ((cp >= 0x2160 && cp <= 0x218F) || (cp >= 0x2300 && cp <= 0x24FF)) return false;
    if (cp >= 0x2580 && cp <= 0x27BF) return cp >= 0x2794;
    if (cp >= 0x3000 && cp <= 0xFAFF) return cp <= 0x3015 && cp >= 0x3008;
    if (cp >= 0xFE30 && cp <= 0xFE6F) return false;
    if (cp >= 0xFF00) return false;
    return true;
}

}  // namespace

bool verticalUpright(char32_t cp, int direction) {
    if (cp == ' ' || cp == 0x2007 || cp == '\t') return false;
    // . , “ ” ‘ ’ 은 세로 꼴(。 、 ﹃ ﹄ ﹁ ﹂)로 바꾼 글자로 묻는다.
    return !verticalSideways(verticalForm(cp), direction == 1);
}

VerticalCell verticalCell(char32_t cp, int direction, std::int32_t natural, int size, int relSize, bool shrunk,
                          std::int32_t spacing) {
    // 곱해 나눌 때 0.5는 0에서 멀어지게 올린다(한글 2024 와 같은 반올림).
    auto mulDiv = [](std::int64_t a, std::int64_t b, std::int64_t c) {
        const std::int64_t n = a * b;
        return static_cast<std::int32_t>(n >= 0 ? (n + c / 2) / c : -((-n + c / 2) / c));
    };
    VerticalCell out;
    out.upright = verticalUpright(cp, direction);
    std::int32_t width = natural;
    if (out.upright) {
        width = size > 0 ? size : 1000;
        if (relSize > 0 && relSize != 100) width = mulDiv(width, relSize, 100);
        if (shrunk) width = mulDiv(width, 16, 25);
    }
    out.ink = std::max<std::int32_t>(0, width) / 4 * 4;
    // 자간은 코드 31 이상만이다.
    if (spacing != 0 && cp >= 31) out.trail = mulDiv(out.ink / 4, spacing, 100) * 4;
    return out;
}

std::vector<GridCell> charGridCells(const std::vector<char32_t>& cps, const std::vector<std::int32_t>& widths,
                                    std::int32_t cell, bool wongoji) {
    std::vector<GridCell> out(cps.size());
    if (cell <= 0) {
        for (std::size_t i = 0; i < cps.size(); ++i) out[i].width = i < widths.size() ? widths[i] : 0;
        return out;
    }
    int open = -1;  // 반을 쓰고 남은 칸의 첫 글자
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const char32_t cp = cps[i];
        const std::int32_t natural = i < widths.size() ? std::max<std::int32_t>(0, widths[i]) : 0;
        if (cp == 0) {
            out[i].width = natural;
            open = -1;
            continue;
        }
        if (wongoji && wongojiHalf(cp)) {
            if (open >= 0) {
                // 앞 반각과 한 칸을 나눈다. 앞 글자는 왼쪽 반, 이 글자는 오른쪽 반 가운데다.
                // 두 글자를 붙여 한 덩어리로 칸 가운데에 둔다. 캐럿은 두 글자 사이에 선다.
                const std::size_t first = static_cast<std::size_t>(open);
                const std::int32_t firstNatural = first < widths.size() ? std::max<std::int32_t>(0, widths[first]) : 0;
                const std::int32_t lead = std::max<std::int32_t>(0, (cell - firstNatural - natural) / 2);
                out[first].width = std::min(cell, lead + firstNatural);
                out[first].lead = lead;
                out[i].width = cell - out[first].width;
                out[i].lead = 0;
                open = -1;
                continue;
            }
            out[i].width = cell;
            // 혼자인 문장 부호는 칸 왼쪽에서 칸 폭의 15% 에, 숫자·글자는 칸 가운데에 둔다(한글 2024 PDF, 10pt·칸 2000).
            // 다음 반각이 오면 위에서 짝으로 다시 나눈다.
            out[i].lead = wongojiMark(cp) ? cell * 3 / 20 : std::max<std::int32_t>(0, (cell - natural) / 2);
            open = static_cast<int>(i);
            continue;
        }
        open = -1;
        const std::int32_t cells = std::max<std::int32_t>(1, (natural + cell - 1) / cell);
        out[i].width = cells * cell;
        out[i].lead = std::max<std::int32_t>(0, (out[i].width - natural) / 2);
    }
    return out;
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

// ---------------------------------------------------------------------------
// 끊을 자리

BreakClassifier::BreakClassifier(const ParaShape* paraShape)
    : latinChar_(paraShape && paraShape->breakLatinWord == "BREAK_WORD"),
      hyphen_(paraShape && paraShape->breakLatinWord == "HYPHENATION"),
      // HWPX 이름과 뜻이 반대다. BREAK_WORD 가 어절, KEEP_WORD 가 글자다(한글 2024, tmp/wordbreak).
      nonLatinWord_(paraShape && paraShape->breakNonLatinWord == "BREAK_WORD") {}

void BreakClassifier::classify(Atom& atom, const std::vector<Atom>& atoms) {
    const char32_t cp = atom.cp;
    // 어절 단위면 띄어쓰기·탭·줄바꿈·개체 바로 뒤가 아닌 글자는 앞 글자와 한 어절이다.
    const bool joins = nonLatinWord_ && !atoms.empty() && !atoms.back().space && !atoms.back().tab &&
                       !atoms.back().hard && !atoms.back().control;
    if (atom.hard || atom.space || atom.tab) {
        latinRun_ = false;
    } else if (lineHeadForbidden(cp) && !atoms.empty() && !atoms.back().space && !atoms.back().tab &&
               !atoms.back().hard && !atoms.back().control) {
        atom.breakBefore = false;
        latinRun_ = false;
    } else if (isCjk(cp) || isHangulWordChar(cp)) {
        atom.breakBefore = !joins;
        latinRun_ = false;
    } else if (!latinChar_ && (isLatinWordChar(cp) || cp == '-')) {
        atom.breakBefore = !latinRun_ && !joins;
        if (hyphen_ && !atoms.empty() && atoms.back().cp == '-' && !atoms.back().control) {
            atom.breakBefore = true;
        }
        latinRun_ = true;
        if (hyphen_ && cp == '-') latinRun_ = false;
    } else {
        if (atoms.empty() || atoms.back().space || atoms.back().tab || atoms.back().hard || atoms.back().control) {
            atom.breakBefore = true;
        } else if (joins) {
            atom.breakBefore = false;  // 어절 안 부호. "비교⋅평가하기"는 한 어절이다.
        }
        latinRun_ = false;
    }
}

// ---------------------------------------------------------------------------
// Span

void RunBuilder::add(const Atom& atom) {
    if (open_ && (size_ != atom.fontSize || shape_ != atom.charShapeId || font_ != atom.font || ratio_ != atom.ratio ||
                  spacing_ != atom.spacing || offset_ != atom.charOffset || rel_ != atom.relSize ||
                  markpen_ != atom.markpen || synthetic_ != atom.synthetic)) {
        flush();
    }
    if (!open_) {
        open_ = true;
        begin_ = text_.size();
        size_ = atom.fontSize;
        shape_ = atom.charShapeId;
        font_ = atom.font;
        ratio_ = atom.ratio;
        rel_ = atom.relSize;
        offset_ = atom.charOffset;
        spacing_ = atom.spacing;
        markpen_ = atom.markpen;
        synthetic_ = atom.synthetic;
    }
    if (keepAdvances_) {
        // 글자 격자에서는 원래 글자 폭을 남긴다. 칸 자리는 그릴 때 같은 식(charGridCells)으로 다시 잡는다.
        const std::int32_t width = atom.natural > 0 ? atom.natural : atom.width;
        widths_.push_back(width);
        inks_.push_back(width - (atom.natural > 0 ? atom.naturalTrail : atom.trail));
    }
}

void RunBuilder::flush() {
    if (!open_) return;
    open_ = false;
    if (text_.size() <= begin_) return;
    LineRun run;
    run.synthetic = synthetic_;
    run.textBegin = static_cast<std::uint16_t>(begin_);
    run.textEnd = static_cast<std::uint16_t>(text_.size());
    for (const std::int32_t width : widths_) run.width += width;
    run.charShapeId = shape_;
    run.style = runStyleOf(size_, font_, markpen_, std::string{}, ratio_, rel_, spacing_, offset_);
    appendLineRun(line_.glyphsMut(), run, widths_.data(), widths_.size(), inks_.data(), inks_.size());
    widths_.clear();
    inks_.clear();
}

void appendGlyphRun(Node& line, const Atom& atom, std::size_t textBegin, std::size_t textEnd) {
    LineRun run;
    run.synthetic = atom.synthetic;
    run.textBegin = static_cast<std::uint16_t>(textBegin);
    run.textEnd = static_cast<std::uint16_t>(textEnd);
    run.width = atom.width;
    run.charShapeId = atom.charShapeId;
    run.style = runStyleOf(atom.fontSize, atom.font, atom.markpen, atom.tabLeader, atom.ratio, atom.relSize,
                           atom.spacing, atom.charOffset);
    const std::int32_t ink = atom.width - atom.trail;
    appendLineRun(line.glyphsMut(), run, nullptr, 0, &ink, 1);
}

void placeOccupyingNote(Node& line, const Atom& atom, std::int32_t& pen, std::string& lineText) {
    if (atom.noteText.empty()) return;
    LineRun run;
    run.kind = RunKind::Note;
    run.textBegin = static_cast<std::uint16_t>(lineText.size());
    lineText += atom.noteText;
    run.textEnd = static_cast<std::uint16_t>(lineText.size());
    run.charShapeId = atom.charShapeId;
    run.style = runStyleOf(atom.inkSize > 0 ? atom.inkSize : atom.fontSize, atom.font, std::string{}, std::string{},
                           atom.ratio > 0 ? atom.ratio : 100, atom.relSize > 0 ? atom.relSize : 100, atom.spacing,
                           atom.charOffset);
    appendLineRun(line.glyphsMut(), run);
    pen += atom.width;
}

// ---------------------------------------------------------------------------
// 탭

TabRuler::TabRuler(const Document& doc, const ParaShape* paraShape, std::int32_t marginLeft,
                   std::int32_t contentWidth, std::int32_t bodyRight, std::int32_t defaultTab)
    : marginLeft_(marginLeft), bodyRight_(std::max<std::int32_t>(0, bodyRight)),
      step_(defaultTab > 0 ? defaultTab : 4000) {
    bool autoTabRight = false;
    if (paraShape) {
        for (const auto& def : doc.header.tabDefs) {
            if (def.id != paraShape->tabDefId) continue;
            autoTabRight = def.autoTabRight;
            for (const auto& stop : def.stops) {
                int align = 0;
                if (stop.type == "RIGHT") align = 1;
                else if (stop.type == "CENTER") align = 2;
                const std::int32_t tabPos = std::max<std::int32_t>(0, std::min(stop.pos, bodyRight_));
                stops_.push_back(Stop{tabPos, align, stop.leader});
            }
            break;
        }
    }
    // 문단 오른쪽 끝 자동 탭. 탭 뒤 글의 끝이 내용 오른쪽 끝에 닿는다.
    if (autoTabRight) stops_.push_back(Stop{marginLeft + contentWidth, 1, {}});
    std::sort(stops_.begin(), stops_.end(), [](const Stop& a, const Stop& b) { return a.pos < b.pos; });
}

void TabRuler::deferTo(Node& line) const {
    line.editContent().deferTabs = true;
    line.editContent().tabMargin = marginLeft_;
    line.editContent().tabBodyRight = bodyRight_;
    line.editContent().tabStep = step_;
    line.editContent().tabStops.reserve(stops_.size());
    for (const Stop& place : stops_) {
        LineTab slot;
        slot.pos = place.pos;
        slot.align = place.align;
        slot.leader = place.leader;
        line.editContent().tabStops.push_back(std::move(slot));
    }
}

// ---------------------------------------------------------------------------
// 한 줄 채우기

namespace {

// 영문 단어가 칸에 들어가면 통째로 두고, 칸보다 길 때만 글자로 자른다.
// 한글은 글자마다 끊어 줄에 들어가는 만큼 채운다.
struct WordFit {
    std::size_t end = 0;
    std::int32_t width = 0;
    bool longer = false;
};

class LineFiller {
public:
    LineFiller(const LineSpec& spec, std::vector<Atom>& atoms, std::size_t& cursor, LineFill& fill, LineHost& host)
        : spec_(spec), doc_(*spec.doc), atoms_(atoms), cursor_(cursor), host_(host), fill_(fill) {
        pen_ = spec.gaps[0].first;
        fill_.gapIndex = 0;
        fill_.lineX = pen_;
        fill_.start = spec.firstPos;
        fill_.end = 0;
        fill_.textHeight = 0;
        fill_.shapeHeight = 0;
        fill_.endedByLineBreak = false;
        fill_.text.clear();
    }

    void run() {
        RunBuilder spans(fill_.line, fill_.text, spec_.keepAdvances);
        spans_ = &spans;
        while (cursor_ < atoms_.size()) {
            if (spec_.storedBreaks) {
                if (!storedStep()) break;
                continue;
            }
            if (!flowStep()) break;
        }
        spans.flush();
        spans_ = nullptr;
    }

private:
    bool hasContent() const {
        return fill_.start >= 0 || !spans_->empty() || !fill_.line.glyphs().runs.empty() || !fill_.line.children.empty();
    }

    void open(const Atom& atom) {
        if (fill_.start >= 0) return;
        fill_.start = atom.pos;
        fill_.lineX = pen_;
    }

    void place(const Atom& atom) {
        fill_.shapeHeight = std::max(fill_.shapeHeight, atom.fontSize);
        if (spec_.deferText && !atom.control) {
            open(atom);
            const int units = atom.tab ? 8 : (atom.cp > 0xFFFF ? 2 : 1);
            fill_.end = atom.pos + units;
            if (atom.fontSize > fill_.textHeight) fill_.textHeight = atom.fontSize;
            fill_.line.spansLater = true;
            return;
        }
        open(atom);
        fill_.end = atom.pos + 1;
        if (!atom.control && atom.fontSize > fill_.textHeight) fill_.textHeight = atom.fontSize;
        Node& line = fill_.line;
        if (atom.control) {
            spans_->flush();
            Node child = host_.inlineObject(atom, pen_);
            child.charShapeId = atom.charShapeId;
            child.fontSize = atom.fontSize > 0 ? atom.fontSize : kEm;
            child.font = atom.font;
            pen_ += atom.width;
            appendLineObject(line, std::move(child), fill_.text.size());
            return;
        }
        if (atom.tab) {
            spans_->flush();
            const std::size_t at = fill_.text.size();
            fill_.text += '\t';
            appendGlyphRun(line, atom, at, fill_.text.size());
            line.glyphsMut().runs.back().kind = RunKind::Tab;
            pen_ += atom.width;
            return;
        }
        if (atom.space) {
            // 띄어쓰기는 4 × ⌊크기 ÷ 8⌋, 고정폭 빈칸은 4 × ⌊⌊크기 ÷ 4⌋ ÷ 4⌋다. 고정폭 빈칸은 양쪽 정렬에서 뺀다.
            spans_->flush();
            const std::size_t at = fill_.text.size();
            appendUtf8(fill_.text, atom.cp);
            appendGlyphRun(line, atom, at, fill_.text.size());
            pen_ += atom.width;
            if (atom.cp != 0x2007) {
                spaceSum_ += atom.width;
                spaceBase_ += atom.width;
            }
            return;
        }
        spans_->add(atom);
        appendUtf8(fill_.text, atom.cp);
        pen_ += atom.width;
    }

    // 각주·미주 번호와 머리말 같은 줄 차지 원자. 폭은 번호 글자만큼이다.
    void placeOccupying(const Atom& atom, int units) {
        open(atom);
        if (!atom.noteText.empty()) {
            spans_->flush();
            fill_.end = std::max(fill_.end, atom.pos + units);
            placeOccupyingNote(fill_.line, atom, pen_, fill_.text);
        } else {
            fill_.end = atom.pos;
        }
        if (atom.fontSize > fill_.textHeight) fill_.textHeight = atom.fontSize;
        fill_.shapeHeight = std::max(fill_.shapeHeight, atom.fontSize);
    }

    WordFit measureWord(std::int32_t gapWidth) {
        WordFit fit;
        fit.end = cursor_;
        std::int32_t sum = 0;
        while (fit.end < atoms_.size()) {
            host_.measure(atoms_[fit.end]);
            const Atom& next = atoms_[fit.end];
            if (fit.end != cursor_ && lineHeadForbidden(next.cp)) break;
            const bool same =
                fit.end == cursor_ || (!next.breakBefore && !next.space && !next.tab && !next.hard && !next.control);
            if (!same) break;
            sum += next.width;
            // 이미 더해진 글자는 자간을 포함하고, 지금 보는 글자는 폭만 비교한다.
            fit.width = sum - next.trail;
            ++fit.end;
            if (fit.width > gapWidth + kSlack) {
                fit.longer = true;
                break;
            }
        }
        return fit;
    }

    // 탭 뒤로 다음 탭이나 줄바꿈까지의 글 폭.
    std::int32_t followingWidth(std::size_t index) {
        std::int32_t sum = 0;
        for (std::size_t i = index + 1; i < atoms_.size(); ++i) {
            if (atoms_[i].tab || atoms_[i].hard) break;
            host_.measure(atoms_[i]);
            sum += atoms_[i].width;
        }
        return sum;
    }

    void resolveTab() {
        Atom& atom = atoms_[cursor_];
        if (!atom.tab) return;
        const std::size_t index = cursor_;
        const TabMove move = spec_.tabs->advance(pen_, [&] { return followingWidth(index); });
        atom.width = move.width;
        atom.tabLeader = move.leader;
    }

    void shrinkSpaces(std::int32_t shrink) {
        if (shrink <= 0 || spaceSum_ <= 0) return;
        auto& runs = fill_.line.glyphsMut().runs;
        auto& children = fill_.line.children;
        const auto pullAfter = [&](std::size_t idx, std::int32_t cut) {
            for (std::size_t k = idx + 1; k < runs.size(); ++k) {
                if (runs[k].kind == RunKind::Object && runs[k].slot < children.size()) children[runs[k].slot].x -= cut;
            }
        };
        std::int32_t left = shrink;
        std::size_t lastIdx = runs.size();
        for (std::size_t idx = 0; idx < runs.size(); ++idx) {
            LineRun& run = runs[idx];
            if (run.kind != RunKind::Text || run.textEnd != run.textBegin + 1 || fill_.text[run.textBegin] != ' ' ||
                run.width <= 0) {
                continue;
            }
            std::int32_t cut = static_cast<std::int32_t>(static_cast<std::int64_t>(run.width) * shrink / spaceSum_);
            if (cut > left) cut = left;
            if (cut > run.width) cut = run.width;
            run.width -= cut;
            left -= cut;
            lastIdx = idx;
            if (cut > 0) pullAfter(idx, cut);
        }
        if (lastIdx < runs.size() && left > 0) {
            LineRun& last = runs[lastIdx];
            const std::int32_t cut = std::min(last.width, left);
            last.width -= cut;
            left -= cut;
            if (cut > 0) pullAfter(lastIdx, cut);
        }
        const std::int32_t applied = shrink - left;
        pen_ -= applied;
        spaceSum_ -= applied;
    }

    // 공백 최소값만큼 빈칸을 줄일 수 있는 남은 폭. 20이면 원래 너비의 80%가 하한이고, 그 아래로 다시 줄이지 않는다.
    std::int32_t condenseBudget() const {
        const ParaShape* paraShape = spec_.paraShape;
        if (!paraShape || paraShape->condense <= 0 || spaceBase_ <= 0) return 0;
        const std::int32_t limit = std::min<std::int32_t>(paraShape->condense, 75);
        const std::int32_t used = spaceBase_ - spaceSum_;
        return static_cast<std::int32_t>(static_cast<std::int64_t>(spaceBase_) * limit / 100) - used;
    }

    // 다음 빈칸 중 width 가 들어가는 첫 칸으로 펜을 옮긴다.
    bool moveToGapFitting(std::int32_t width) {
        const auto& gaps = spec_.gaps;
        for (std::size_t next = fill_.gapIndex + 1; next < gaps.size(); ++next) {
            if (width <= gaps[next].second - gaps[next].first + kSlack) {
                // 글이 든 줄은 여기서 닫고, 같은 높이에서 그 칸부터 새 줄로 잇는다.
                if (hasContent()) {
                    fill_.jumpGap = static_cast<int>(next);
                    return false;
                }
                fill_.gapIndex = next;
                pen_ = gaps[next].first;
                return true;
            }
        }
        return false;
    }

    // 저장된 줄: 글자 폭으로 다시 나누지 않고 다음 줄 첫 글자 위치에서 끊는다.
    bool storedStep() {
        const Atom& atom = atoms_[cursor_];
        if (atom.pos >= spec_.lineLimit) return false;
        if (atom.occupiesLine) {
            placeOccupying(atom, 8);
            ++cursor_;
            return true;
        }
        // 탭 칸은 글자 폭이 있어야 정해진다. 쪽 쌓기에는 줄 높이만 쓰고, 칸은 그릴 때 정한다.
        if (atom.tab && !fill_.line.lineData().deferTabs) spec_.tabs->deferTo(fill_.line);
        // 줄바꿈 문자는 그리지 않는다. 여기서 줄을 끝내고 다음 글자는 다음 줄이다.
        if (atom.hard) {
            fill_.endedByLineBreak = true;
            ++cursor_;
            return false;
        }
        place(atoms_[cursor_]);
        ++cursor_;
        return true;
    }

    // 글자 폭으로 끊는다. false 면 이 줄은 끝이다.
    bool flowStep() {
        host_.measure(atoms_[cursor_]);
        const Atom& atom = atoms_[cursor_];
        if (atom.occupiesLine) {
            placeOccupying(atom, 1);
            ++cursor_;
            return true;
        }
        if (atom.hard) {
            fill_.endedByLineBreak = true;
            ++cursor_;
            return false;
        }
        // 한 줄로 유지는 폭 비교를 타지 않는다.
        // 공백·전각 공백·U+2000–U+200B·탭은 넘어도 그 자리에서 끊지 않는다.
        const bool keepLine = spec_.paraShape && spec_.paraShape->lineWrap == "KEEP";
        if (keepLine || atom.tab || isOverflowGap(atom.cp)) {
            resolveTab();
            place(atoms_[cursor_]);
            ++cursor_;
            return true;
        }
        const Gap& gap = spec_.gaps[fill_.gapIndex];
        const std::int32_t gapWidth = gap.second - gap.first;
        if (atom.control) {
            // 글자처럼 취급한 개체는 빈 줄이면 칸보다 넓어도 그 줄 맨 위에 둔다.
            // 맞지 않는다고 줄 간격만큼 내려 가면 생각 열기 그림처럼 칸 위에서 떨어진다.
            const std::int32_t one = atom.width - atom.trail;
            const bool fitted = pen_ + one <= gap.second + kSlack || moveToGapFitting(one);
            if (!fitted && hasContent()) return false;
            place(atom);
            ++cursor_;
            return true;
        }
        // 닫는 문장부호와 일본어 작은 가나는 줄 앞에 두지 않는다.
        // 폭이 되면 두고, 줄이 이미 꽉 찼으면 넘쳐도 그 줄에 남긴다.
        // 남은 칸이 있는데 그 글자가 안 들어가면 다음 줄로 보낸다.
        if (lineHeadForbidden(atom.cp)) {
            const std::int32_t one = atom.width - atom.trail;
            const std::int32_t right = spec_.gaps[fill_.gapIndex].second;
            const bool fits = pen_ + one <= right + kSlack;
            const bool lineFull = pen_ >= right + kSlack;
            if (fits || (lineFull && hasContent())) {
                place(atom);
                ++cursor_;
                return true;
            }
            if (hasContent()) {
                const std::int32_t budget = condenseBudget();
                if (budget > 0 && pen_ - budget + one <= right + kSlack) {
                    shrinkSpaces(budget);
                    place(atom);
                    ++cursor_;
                    return true;
                }
            }
        }
        const WordFit word = measureWord(gapWidth);
        std::int32_t wordWithTail = word.width;
        if (word.end > cursor_ && word.end < atoms_.size() && lineHeadForbidden(atoms_[word.end].cp)) {
            std::int32_t sum = word.width + atoms_[word.end - 1].trail;
            std::size_t idx = word.end;
            while (idx < atoms_.size() && lineHeadForbidden(atoms_[idx].cp)) {
                sum += atoms_[idx].width;
                wordWithTail = sum - atoms_[idx].trail;
                ++idx;
            }
        }
        const std::int32_t fitCheckWidth = (spaceSum_ < spaceBase_) ? wordWithTail : word.width;
        // 세로쓰기에서 단어 뒤에 줄 머리에 못 오는 부호가 붙으면 그 부호도 이 줄에 남아야 단어를 둔다. 부호가
        // 들어가거나, 단어로 줄이 꽉 차 부호가 넘쳐 남거나, 빈칸을 줄여 들어가면 남는다. 아니면 단어째 다음 줄로 간다.
        // 세로쓰기 20pt 의 1em 마침표: "키웁니|다." 로 나눈다(한글 2024 vert-1-landscape).
        // 가로쓰기는 그 부호만 다음 줄 앞으로 보낸다(줄바뀜 테스트의 "?", 글자 격자의 ".").
        const std::int32_t tailRight = pen_ + wordWithTail;
        const bool tailStays = !spec_.vertical || wordWithTail == word.width || tailRight <= gap.second + kSlack ||
                               pen_ + word.width >= gap.second + kSlack ||
                               tailRight - condenseBudget() <= gap.second + kSlack;
        if (!word.longer && pen_ + fitCheckWidth <= gap.second + kSlack && (tailStays || !hasContent())) {
            while (cursor_ < word.end) place(atoms_[cursor_++]);
            return true;
        }
        if (!word.longer && atom.breakBefore && hasContent()) {
            const std::int32_t budget = condenseBudget();
            if (budget > 0 && pen_ - budget + wordWithTail <= gap.second + kSlack) {
                shrinkSpaces(budget);
                while (cursor_ < word.end) place(atoms_[cursor_++]);
                return true;
            }
        }
        // 단어가 칸에는 들어가는데 이 자리에는 안 들어가면 통째로 넘긴다.
        // 이미 그 단어를 글자로 자르기 시작했다면 들어가는 만큼 더 둔다.
        if (!word.longer && atom.breakBefore) {
            const auto& gaps = spec_.gaps;
            for (std::size_t next = fill_.gapIndex + 1; next < gaps.size(); ++next) {
                const std::int32_t nextWidth = gaps[next].second - gaps[next].first;
                const WordFit alt = measureWord(nextWidth);
                if (!alt.longer && alt.width <= nextWidth + kSlack) {
                    if (hasContent()) {
                        fill_.jumpGap = static_cast<int>(next);
                        return false;
                    }
                    fill_.gapIndex = next;
                    pen_ = gaps[next].first;
                    return true;
                }
            }
            if (hasContent()) return false;
        }
        const std::int32_t one = atom.width - atom.trail;
        const bool fitted = pen_ + one <= spec_.gaps[fill_.gapIndex].second + kSlack || moveToGapFitting(one);
        // 빈 줄인데 글자 하나가 칸보다 넓으면 그 글자는 그래도 둔다.
        if (!fitted && hasContent()) return false;
        place(atom);
        ++cursor_;
        return true;
    }

    const LineSpec& spec_;
    const Document& doc_;
    std::vector<Atom>& atoms_;
    std::size_t& cursor_;
    LineHost& host_;
    RunBuilder* spans_ = nullptr;
    LineFill& fill_;
    std::int32_t pen_ = 0;
    std::int32_t spaceSum_ = 0;
    std::int32_t spaceBase_ = 0;
};

}  // namespace

void fillLine(const LineSpec& spec, std::vector<Atom>& atoms, std::size_t& cursor, LineFill& fill, LineHost& host) {
    LineFiller(spec, atoms, cursor, fill, host).run();
}

std::int32_t closeLine(LineFill& fill) {
    Node& line = fill.line;
    line.x = fill.lineX;
    line.editContent().lineBreak = fill.endedByLineBreak;
    // 글자 없이 글자처럼 넣은 개체만 있으면 글자 상자가 없다. 개체의 글자 모양은 줄 간격에만 든다.
    bool objectOnly = fill.textHeight <= 0;
    if (objectOnly) {
        objectOnly = false;
        for (const auto& child : line.children) {
            if (child.kind == NodeKind::Object && child.treatAsChar) objectOnly = true;
        }
    }
    const std::int32_t charBox = fill.textHeight > 0 ? fill.textHeight : (objectOnly ? 0 : line.fontSize);
    line.editContent().textHeight = charBox;
    line.editContent().spacingBox = fill.shapeHeight;
    line.start = fill.start < 0 ? 0 : fill.start;
    line.end = fill.end;
    line.textMut() = std::move(fill.text);
    for (auto& child : line.children) {
        if (child.kind == NodeKind::Object) child.x -= line.x;
    }
    trimLineGlyphs(line.glyphsMut());
    return charBox;
}

Node singleAtomLine(const Document& doc, const Atom& atom, Node object) {
    Node line;
    line.kind = NodeKind::Line;
    line.charShapeId = atom.charShapeId;
    line.font = atom.font;
    line.fontSize = atom.fontSize > 0 ? atom.fontSize : kEm;
    line.start = atom.pos;
    line.end = atom.pos + 1;
    if (atom.control) {
        object.charShapeId = atom.charShapeId;
        object.fontSize = line.fontSize;
        object.font = atom.font;
        appendLineObject(line, std::move(object), 0);
        line.width = atom.width;
        trimLineGlyphs(line.glyphsMut());
        return line;
    }
    appendUtf8(line.textMut(), atom.cp);
    LineRun run;
    run.kind = atom.cp == '\t' ? RunKind::Tab : RunKind::Text;
    run.textEnd = static_cast<std::uint16_t>(line.text().size());
    run.charShapeId = atom.charShapeId;
    run.style = runStyleOf(line.fontSize, std::string{}, atom.markpen, atom.tabLeader, 100, 100, 0, atom.charOffset);
    appendLineRun(line.glyphsMut(), run);
    line.width = atom.width;
    trimLineGlyphs(line.glyphsMut());
    return line;
}

SpanFontBox lineFontBox(const Node& line) {
    SpanFontBox out;
    for (const LineRun& run : lineGlyphs(line).runs) {
        if (run.kind == RunKind::Object || run.textEnd == run.textBegin || run.style->fontSize <= 0) continue;
        const RunStyle& style = *run.style;
        const auto box = chwp::layout::fontLineBox(style.font ? *style.font : std::string{}, style.fontSize);
        out.baseline = std::max(out.baseline, box.ascent);
        out.height = std::max(out.height, box.height);
    }
    return out;
}

void applyFontBaseline(Node& line, const ParaShape* paraShape) {
    line.editContent().matchFontBaseline = paraShape && paraShape->fontLineHeight;
    if (!line.lineData().matchFontBaseline) return;
    const SpanFontBox box = lineFontBox(line);
    if (box.baseline < 0) return;
    line.editContent().lineBaseline = box.baseline;
    line.editContent().textHeight = std::max(line.lineData().textHeight, box.height);
}

void markInlineOverflow(Node& line) {
    std::int32_t capTop = 0;
    std::int32_t capBottom = line.height;
    for (const Node& child : line.children) {
        if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
        const std::int32_t top = child.y + child.marginTop;
        const std::int32_t bottom = top + std::max(child.height, 0);
        if (top < 0) line.editContent().auditRise = std::max(line.lineData().auditRise, -top);
        if (bottom > line.height) line.editContent().auditBelow = std::max(line.lineData().auditBelow, bottom - line.height);
        for (const Node& cap : child.children) {
            if (cap.role != "caption") continue;
            capTop = std::min(capTop, top + cap.y);
            capBottom = std::max(capBottom, top + cap.y + std::max(cap.height, 0));
        }
    }
    if (capTop < 0) line.editContent().auditRise = std::max(line.lineData().auditRise, -capTop);
    if (capBottom > line.height) line.editContent().auditBelow = std::max(line.lineData().auditBelow, capBottom - line.height);
}

void shiftSpanOffsets(Node& line, int delta) {
    if (delta == 0) return;
    for (Node& ch : line.children) {
        if (ch.kind == NodeKind::Object && !ch.headingMark) {
            ch.start += delta;
            ch.end += delta;
        }
    }
}

void shiftLineOffsets(Node& line, int delta) {
    line.start += delta;
    line.end += delta;
    shiftSpanOffsets(line, delta);
}

}  // namespace chwp::render::detail
