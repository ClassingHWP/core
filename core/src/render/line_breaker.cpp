#include "render/line_breaker.hpp"

#include "export/font_metrics.hpp"

namespace noting::render::detail {

using model::Document;
using model::ParaShape;

bool isHangul(char32_t cp) {
    return cp >= 0xAC00 && cp <= 0xD7A3;
}

bool isHangulWordChar(char32_t cp) {
    return (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F) ||
           (cp >= 0xAC00 && cp <= 0xD7A3);
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
      hyphen_(paraShape && paraShape->breakLatinWord == "HYPHENATION") {}

void BreakClassifier::classify(Atom& atom, const std::vector<Atom>& atoms) {
    const char32_t cp = atom.cp;
    if (atom.hard || atom.space || atom.tab) {
        latinRun_ = false;
    } else if (lineHeadForbidden(cp) && !atoms.empty() && !atoms.back().space && !atoms.back().tab &&
               !atoms.back().hard && !atoms.back().control) {
        atom.breakBefore = false;
        latinRun_ = false;
    } else if (isCjk(cp) || isHangulWordChar(cp)) {
        atom.breakBefore = true;
        latinRun_ = false;
    } else if (!latinChar_ && (isLatinWordChar(cp) || cp == '-')) {
        atom.breakBefore = !latinRun_;
        if (hyphen_ && !atoms.empty() && atoms.back().cp == '-' && !atoms.back().control) {
            atom.breakBefore = true;
        }
        latinRun_ = true;
        if (hyphen_ && cp == '-') latinRun_ = false;
    } else {
        if (atoms.empty() || atoms.back().space || atoms.back().tab || atoms.back().hard || atoms.back().control) {
            atom.breakBefore = true;
        }
        latinRun_ = false;
    }
}

// ---------------------------------------------------------------------------
// Span

void SpanBuilder::add(const Atom& atom) {
    if (!text_.empty() && (size_ != atom.fontSize || shape_ != atom.charShapeId || font_ != atom.font ||
                           ratio_ != atom.ratio || spacing_ != atom.spacing || offset_ != atom.charOffset ||
                           rel_ != atom.relSize || markpen_ != atom.markpen)) {
        flush();
    }
    if (text_.empty()) {
        size_ = atom.fontSize;
        shape_ = atom.charShapeId;
        font_ = atom.font;
        ratio_ = atom.ratio;
        rel_ = atom.relSize;
        offset_ = atom.charOffset;
        spacing_ = atom.spacing;
        markpen_ = atom.markpen;
    }
    appendUtf8(text_, atom.cp);
    if (keepAdvances_) {
        widths_.push_back(atom.width);
        inks_.push_back(atom.width - atom.trail);
    }
}

void SpanBuilder::flush() {
    if (text_.empty()) return;
    Node span;
    span.kind = NodeKind::Span;
    span.text = std::move(text_);
    span.advances = std::move(widths_);
    span.inks = std::move(inks_);
    span.width = 0;
    for (const std::int32_t width : span.advances) span.width += width;
    span.fontSize = size_;
    span.charShapeId = shape_;
    span.font = font_;
    span.ratio = ratio_;
    span.spacing = spacing_;
    span.relSize = rel_;
    span.charOffset = offset_;
    span.markpen = markpen_;
    paintCharStyle(span, doc_, shape_);
    text_.clear();
    widths_.clear();
    inks_.clear();
    out_.push_back(std::move(span));
}

Node glyphSpan(const Document& doc, const Atom& atom, const std::string& text) {
    Node span;
    span.kind = NodeKind::Span;
    span.text = text;
    span.width = atom.width;
    span.inks.push_back(atom.width - atom.trail);
    span.fontSize = atom.fontSize;
    span.charShapeId = atom.charShapeId;
    span.font = atom.font;
    span.ratio = atom.ratio;
    span.spacing = atom.spacing;
    span.relSize = atom.relSize;
    span.charOffset = atom.charOffset;
    span.markpen = atom.markpen;
    span.tabLeader = atom.tabLeader;
    paintCharStyle(span, doc, atom.charShapeId);
    return span;
}

void placeOccupyingNote(Node& line, const Atom& atom, const Document& doc, std::int32_t& pen, std::string& lineText) {
    if (atom.noteText.empty()) return;
    Node span;
    span.kind = NodeKind::Span;
    span.noteMark = true;
    span.text = atom.noteText;
    span.fontSize = atom.inkSize > 0 ? atom.inkSize : atom.fontSize;
    span.charShapeId = atom.charShapeId;
    span.font = atom.font;
    span.ratio = atom.ratio > 0 ? atom.ratio : 100;
    span.relSize = atom.relSize > 0 ? atom.relSize : 100;
    span.spacing = atom.spacing;
    span.charOffset = atom.charOffset;
    paintCharStyle(span, doc, atom.charShapeId);
    line.children.push_back(std::move(span));
    pen += atom.width;
    lineText += atom.noteText;
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
    line.deferTabs = true;
    line.tabMargin = marginLeft_;
    line.tabBodyRight = bodyRight_;
    line.tabStep = step_;
    line.tabStops.reserve(stops_.size());
    for (const Stop& place : stops_) {
        LineTab slot;
        slot.pos = place.pos;
        slot.align = place.align;
        slot.leader = place.leader;
        line.tabStops.push_back(std::move(slot));
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
        SpanBuilder spans(doc_, fill_.line.children, spec_.keepAdvances);
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
    bool hasContent() const { return fill_.start >= 0 || !spans_->empty() || !fill_.line.children.empty(); }

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
            line.children.push_back(std::move(child));
            return;
        }
        if (atom.tab) {
            spans_->flush();
            line.children.push_back(glyphSpan(doc_, atom, "\t"));
            appendUtf8(fill_.text, atom.cp);
            pen_ += atom.width;
            return;
        }
        if (atom.space) {
            // 띄어쓰기는 4 × ⌊크기 ÷ 8⌋, 고정폭 빈칸은 4 × ⌊⌊크기 ÷ 4⌋ ÷ 4⌋다. 고정폭 빈칸은 양쪽 정렬에서 뺀다.
            spans_->flush();
            std::string gap;
            appendUtf8(gap, atom.cp);
            line.children.push_back(glyphSpan(doc_, atom, gap));
            appendUtf8(fill_.text, atom.cp);
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
            placeOccupyingNote(fill_.line, atom, doc_, pen_, fill_.text);
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
        auto& children = fill_.line.children;
        std::int32_t left = shrink;
        std::size_t lastIdx = children.size();
        for (std::size_t idx = 0; idx < children.size(); ++idx) {
            auto& child = children[idx];
            if (child.kind != NodeKind::Span || child.text != " " || child.width <= 0) continue;
            std::int32_t cut = static_cast<std::int32_t>(static_cast<std::int64_t>(child.width) * shrink / spaceSum_);
            if (cut > left) cut = left;
            if (cut > child.width) cut = child.width;
            child.width -= cut;
            left -= cut;
            lastIdx = idx;
            if (cut > 0) {
                for (std::size_t k = idx + 1; k < children.size(); ++k) children[k].x -= cut;
            }
        }
        if (lastIdx < children.size() && left > 0) {
            auto& last = children[lastIdx];
            const std::int32_t cut = std::min(last.width, left);
            last.width -= cut;
            left -= cut;
            if (cut > 0) {
                for (std::size_t k = lastIdx + 1; k < children.size(); ++k) children[k].x -= cut;
            }
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
        if (atom.tab && !fill_.line.deferTabs) spec_.tabs->deferTo(fill_.line);
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
        if (!word.longer && pen_ + fitCheckWidth <= gap.second + kSlack) {
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
    SpanBuilder* spans_ = nullptr;
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
    line.lineBreak = fill.endedByLineBreak;
    // 글자 없이 글자처럼 넣은 개체만 있으면 글자 상자가 없다. 개체의 글자 모양은 줄 간격에만 든다.
    bool objectOnly = fill.textHeight <= 0;
    if (objectOnly) {
        objectOnly = false;
        for (const auto& child : line.children) {
            if (child.kind == NodeKind::Object && child.treatAsChar) objectOnly = true;
        }
    }
    const std::int32_t charBox = fill.textHeight > 0 ? fill.textHeight : (objectOnly ? 0 : line.fontSize);
    line.textHeight = charBox;
    line.spacingBox = fill.shapeHeight;
    line.start = fill.start < 0 ? 0 : fill.start;
    line.end = fill.end;
    line.text = std::move(fill.text);
    for (auto& child : line.children) {
        if (child.kind == NodeKind::Object) child.x -= line.x;
    }
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
        line.children.push_back(std::move(object));
        line.width = atom.width;
        return line;
    }
    appendUtf8(line.text, atom.cp);
    Node span;
    span.kind = NodeKind::Span;
    span.text = line.text;
    span.fontSize = line.fontSize;
    span.charShapeId = atom.charShapeId;
    span.charOffset = atom.charOffset;
    span.markpen = atom.markpen;
    span.tabLeader = atom.tabLeader;
    paintCharStyle(span, doc, atom.charShapeId);
    line.children.push_back(std::move(span));
    line.width = atom.width;
    return line;
}

SpanFontBox spanFontBox(const std::vector<Node>& spans) {
    SpanFontBox out;
    for (const Node& child : spans) {
        // 문단 번호는 본문 글자 상자를 키우지 않는다.
        if (child.headingMark) continue;
        if (child.kind != NodeKind::Span || child.text.empty() || child.fontSize <= 0) continue;
        const auto box = noting::layout::fontLineBox(child.font, child.fontSize);
        out.baseline = std::max(out.baseline, box.ascent);
        out.height = std::max(out.height, box.height);
    }
    return out;
}

void applyFontBaseline(Node& line, const ParaShape* paraShape) {
    line.matchFontBaseline = paraShape && paraShape->fontLineHeight;
    if (!line.matchFontBaseline) return;
    const SpanFontBox box = spanFontBox(line.children);
    if (box.baseline < 0) return;
    line.lineBaseline = box.baseline;
    line.textHeight = std::max(line.textHeight, box.height);
}

void markInlineOverflow(Node& line) {
    std::int32_t capTop = 0;
    std::int32_t capBottom = line.height;
    for (const Node& child : line.children) {
        if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
        const std::int32_t top = child.y + child.marginTop;
        const std::int32_t bottom = top + std::max(child.height, 0);
        if (top < 0) line.auditRise = std::max(line.auditRise, -top);
        if (bottom > line.height) line.auditBelow = std::max(line.auditBelow, bottom - line.height);
        for (const Node& cap : child.children) {
            if (cap.role != "caption") continue;
            capTop = std::min(capTop, top + cap.y);
            capBottom = std::max(capBottom, top + cap.y + std::max(cap.height, 0));
        }
    }
    if (capTop < 0) line.auditRise = std::max(line.auditRise, -capTop);
    if (capBottom > line.height) line.auditBelow = std::max(line.auditBelow, capBottom - line.height);
}

void shiftSpanOffsets(Node& line, int delta) {
    if (delta == 0) return;
    for (Node& ch : line.children) {
        if ((ch.kind == NodeKind::Span || ch.kind == NodeKind::Object) && !ch.headingMark) {
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

}  // namespace noting::render::detail
