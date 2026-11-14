#include "render/painter.hpp"

#include <algorithm>
#include <cmath>

#include "chwp/model/memo.hpp"
#include "chwp/model/proofreading.hpp"
#include "export/layout.hpp"
#include "include/core/SkPathBuilder.h"

// Painter: 교정 부호 그림. 모양은 한글 2024 화면(200·300·400%)에서 잰 것이다. 빨간색 RGB(255,0,0).
// 길이는 그 자리 글자 크기 s 의 배수이고, 세로 기준은 글자 기준선 b 다(위가 음수).
//   지움표: 범위 위 두 줄(b−0.47s, b−0.23s)            뺌표: 범위를 두른 둥근 상자 + 오른쪽 위 고리 셋
//   붙임표: 범위 아래 둥근 괄호                          줄 붙임표: 문단 왼쪽 여백의 갈고리 둘
//   톱니표: 범위 아래 톱니                               생각표: 범위 아래 물결 + 시작 아래 '?'
//   칭찬표: 범위 아래 물결 + 시작 아래 '◎'              줄표: 범위 아래 줄
//   고침표: 범위를 긋는 줄과 아래로 벌린 끝 + 아래 고칠 글
//   띄움표(%sig 1): 그 자리 위 V                         줄 바꿈표(2): 앞 글자 아래에서 올라가 뒤 글자 위로 꺾인 줄
//   줄 비움표(3): 줄 왼쪽 여백의 '>'                    부호 넣음표(14): 그 자리 아래 '^' 와 넣을 부호
//   넣음표(15): 그 자리 아래 묶음표와 넣을 글            오른·왼 자리 옮김표: 줄 머리의 '[' · ']'
//   메모 고침표: 범위를 긋는 한 줄(메모 상자는 메모 배치가 그린다)
//   자리 바꿈표: 범위를 S 자로 감는 줄(가른 자리 %spl 1)   줄 서로 바꿈표: 윗줄 위 가로줄 + 아랫줄 상자(%spl 2)
//   자료 연결: 범위 회색 바탕 #D6D6D6 + 시작 아래 표시할 문자열
// 줄 이음표는 한글에서 넣어지는 자리를 찾지 못해 모양을 보지 못했고 그리지 않는다.

namespace chwp::render::paint_detail {

namespace {

const char* const kProofRed = "#FF0000";

}  // namespace

std::vector<Painter::FieldSpan> Painter::lineFieldSpans(const Node& node, const std::vector<Mark>& marks,
                                                        const LinePlacement& place,
                                                        bool (*pick)(const model::Control&)) const {
    std::vector<FieldSpan> out;
    if (!doc || node.kind != NodeKind::Line || node.story <= 0 || node.paragraph < 0 || node.start < 0) return out;
    const model::Paragraph* para = storyParagraph(*doc, node.story, node.paragraph);
    if (!para) return out;
    const bool stored = !para->lineSegs.empty();
    const int lineStart = node.start;
    const int lineEnd = std::max(node.end, node.start);
    const bool lastLine = node.lineData().lastLine;
    std::vector<std::size_t> caretToMark;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        if (!marks[i].synthetic) caretToMark.push_back(i);
    }
    const auto xAt = [&](int caret) -> std::int32_t {
        if (caret < static_cast<int>(caretToMark.size())) {
            const std::size_t m = caretToMark[static_cast<std::size_t>(std::max(caret, 0))];
            return m < place.at.size() ? place.at[m] : place.end;
        }
        return place.end;
    };
    const std::int32_t lineLeft = place.at.empty() ? place.fit.slide : place.at.front();
    struct Open {
        const model::Control* control;
        std::size_t span;  // out 의 칸. 아직 이 줄에 없으면 npos
        std::uint32_t charShapeId;
    };
    std::vector<Open> open;
    int pos = 0;
    int caret = 0;
    bool passedStart = false;
    const auto inLine = [&](int at, bool zeroWidth) {
        if (at >= lineStart && at < lineEnd) return true;
        return zeroWidth && lastLine && at == lineEnd;
    };
    const auto startSpan = [&](Open& o, std::int32_t x, bool head) {
        FieldSpan span;
        span.control = o.control;
        span.lineRight = std::max<std::int32_t>(node.width, place.end);
        span.x0 = x;
        span.x1 = place.end;
        span.head = head;
        span.charShapeId = o.charShapeId;
        span.lineLeft = lineLeft;
        o.span = out.size();
        out.push_back(span);
    };
    for (const model::Run& run : para->runs) {
        for (const model::InlineItem& item : run.items) {
            int units = 0;
            bool caretChar = false;
            if (item.type == model::InlineItem::Type::Text) {
                std::size_t idx = 0;
                char32_t cp = 0;
                while (nextCp(item.text, idx, cp)) {
                    const int u = stored ? (cp == '\t' ? 8 : (cp > 0xFFFF ? 2 : 1)) : 1;
                    if (!passedStart && pos >= lineStart) {
                        passedStart = true;
                        // 앞 줄에서 열린 필드는 이 줄 머리부터 이어진다.
                        for (Open& o : open) {
                            if (o.control && o.span == std::string::npos) startSpan(o, lineLeft, false);
                        }
                    }
                    if (inLine(pos, false) && cp != '\n') ++caret;
                    pos += u;
                }
                continue;
            }
            const model::Control& c = item.control;
            caretChar = isCaretCharControl(c);
            units = stored ? 8 : (caretChar ? 1 : 0);
            if (!passedStart && pos >= lineStart && inLine(pos, true)) {
                passedStart = true;
                for (Open& o : open) {
                    if (o.control && o.span == std::string::npos) startSpan(o, lineLeft, false);
                }
            }
            if (c.kind == model::ControlKind::Field) {
                // 고르지 않은 필드도 짝을 맞추려고 쌓는다(끝 문자는 가장 안쪽 필드를 닫는다).
                Open o{pick(c) ? &c : nullptr, std::string::npos, run.charShapeId};
                if (o.control && inLine(pos, true)) startSpan(o, xAt(caret), true);
                open.push_back(o);
            } else if (c.kind == model::ControlKind::FieldEnd) {
                if (!open.empty()) {
                    const Open o = open.back();
                    open.pop_back();
                    if (o.span != std::string::npos && inLine(pos, true)) {
                        out[o.span].x1 = xAt(caret);
                        out[o.span].tail = true;
                    }
                }
            }
            if (caretChar && inLine(pos, false)) ++caret;
            pos += units;
        }
    }
    // 이 줄에서 시작하지도 끝나지도 않고 지나가는 것은 줄 머리부터 끝까지다(문단 안에서만 본다).
    if (!passedStart) {
        for (Open& o : open) {
            if (o.control && o.span == std::string::npos) startSpan(o, lineLeft, false);
        }
    }
    return out;
}

namespace {

bool isProofField(const model::Control& c) { return model::proofKindOf(c) != model::ProofKind::None; }

// 메모 고침표(%%mr)는 교정 부호로 그린다. 범위 상자는 메모(%%me)만이다.
bool isPlainMemoField(const model::Control& c) { return c.kind == model::ControlKind::Field && c.ctrlId == "%%me"; }

}  // namespace

std::vector<Painter::ProofSpan> Painter::lineProofs(const Node& node, const std::vector<Mark>& marks,
                                                    const LinePlacement& place) const {
    std::vector<ProofSpan> out;
    if (!shownMarks().proofMarks()) return out;
    for (const FieldSpan& field : lineFieldSpans(node, marks, place, &isProofField)) {
        ProofSpan span;
        const model::Control& c = *field.control;
        span.kind = model::proofKindOf(c);
        // %sig 의 번호와 %spl 의 종류(1 자리 바꿈, 2 줄 서로 바꿈)는 명령 끝 수다.
        span.sign = span.kind == model::ProofKind::Sign || span.kind == model::ProofKind::Split
                        ? model::proofSignNumber(c.command)
                        : 0;
        span.text = model::proofText(c.command);
        if (span.kind == model::ProofKind::Hyperlink) {
            // 자료 연결 명령은 "주소|표시할 문자열;1;0;0;" 이다. 보이는 것은 표시할 문자열이다.
            const std::string& cmd = c.command;
            const std::size_t bar = cmd.find('|');
            const std::size_t semi = cmd.find(';', bar == std::string::npos ? 0 : bar);
            span.text = bar == std::string::npos ? std::string()
                                                 : cmd.substr(bar + 1, semi == std::string::npos ? std::string::npos : semi - bar - 1);
        }
        span.lineRight = field.lineRight;
        if (span.kind == model::ProofKind::RightMove || span.kind == model::ProofKind::LeftMove) {
            if (!model::proofMove(c.command, span.moveStart, span.moveGap)) {
                span.moveStart = 1500;
                span.moveGap = 1000;
            }
        }
        span.x0 = field.x0;
        span.x1 = field.x1;
        span.head = field.head;
        span.tail = field.tail;
        span.charShapeId = field.charShapeId;
        span.lineLeft = field.lineLeft;
        out.push_back(std::move(span));
    }
    // 자리 바꿈표는 범위 안 가른 자리(%spl 1)에서 위아래가 바뀐다.
    for (ProofSpan& span : out) {
        if (span.kind != model::ProofKind::Transfer) continue;
        for (const ProofSpan& cut : out) {
            if (cut.kind == model::ProofKind::Split && cut.sign != 2 && cut.x0 > span.x0 && cut.x0 < span.x1) span.split = cut.x0;
        }
    }
    return out;
}

std::vector<Painter::MemoRange> Painter::lineMemoRanges(const Node& node, std::int32_t x, std::int32_t y,
                                                        const std::vector<Mark>& marks, const LinePlacement& place) const {
    std::vector<MemoRange> out;
    if (!doc || !shownMarks().memo || node.story <= 0 || node.paragraph < 0) return out;
    if (node.role == "glyph" || node.role == "reverse" || node.role == "glyphDeco") return out;
    if (!lineHasMemoRange(node, doc)) return out;
    const model::StorySlot* slot = ensureStoryMap(*doc).slot(node.story);
    const int sectionIndex = slot && slot->sectionIndex >= 0 ? slot->sectionIndex : 0;
    if (sectionIndex >= static_cast<int>(doc->sections.size())) return out;
    const model::Section& section = doc->sections[static_cast<std::size_t>(sectionIndex)];
    for (const FieldSpan& span : lineFieldSpans(node, marks, place, &isPlainMemoField)) {
        if (span.x1 <= span.x0) continue;
        const model::MemoShape shape = model::memoShapeFor(*doc, section, *span.control);
        const model::CharShape* charShape = findCharShape(*doc, span.charShapeId);
        const std::int32_t s = charShape && charShape->height > 0 ? charShape->height : 1000;
        const std::int32_t b = y + node.marginTop + glyphBaseIn(node, s, node.font, 0).baseline;
        MemoRange range;
        range.x0 = x + span.x0;
        range.x1 = x + span.x1;
        range.top = b - s * 85 / 100;
        range.bottom = b + s * 15 / 100;
        range.head = span.head;
        range.tail = span.tail;
        range.fill = shape.fillColor.empty() || shape.fillColor == "none" ? std::string("#F0FFE9") : shape.fillColor;
        range.line = shape.lineColor.empty() || shape.lineColor == "none" ? std::string("#B6D7AE") : shape.lineColor;
        out.push_back(std::move(range));
    }
    return out;
}

// 메모 범위(한글 2024 화면 200·300·400% 에서 잼): 범위 글자 상자(기준선 위 0.85s, 아래 0.15s)를 메모 모양 채우기 색으로
// 칠하고, 시작에는 '⟨', 끝에는 '⟩' 모양 테두리를 메모 모양 선 색 1픽셀로 긋는다. 모서리는 0.4mm(113) 깎는다.
// 위아래 변은 긋지 않는다. 여러 줄에 걸치면 줄마다 칠하고, 테두리는 처음 줄 시작과 마지막 줄 끝에만 있다.
// 메모 보이기를 끄면 그리지 않는다.
void Painter::drawMemoRanges(const Node& node, std::int32_t x, std::int32_t y, const LinePlacement& place) {
    if (placesOnly || !showGlyphs()) return;
    const std::vector<MemoRange> ranges = lineMemoRanges(node, x, y, collectMarks(node), place);
    if (ranges.empty()) return;
    SkCanvas* sk = canvas();
    for (const MemoRange& range : ranges) {
        std::uint8_t fr = 0, fg = 0, fb = 0;
        parseHex(range.fill, fr, fg, fb);
        const int px0 = px(range.x0);
        const int py0 = px(range.top);
        const int px1 = px(range.x1);
        const int py1 = px(range.bottom);
        if (px1 > px0 && py1 > py0) fillRect(bitmap, px0, py0, px1 - px0, py1 - py0, fr, fg, fb);
        if (!sk || (!range.head && !range.tail)) continue;
        std::uint8_t lr = 0, lg = 0, lb = 0;
        parseHex(range.line, lr, lg, lb);
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setStrokeWidth(0.f);
        pen.setColor(SkColorSetRGB(lr, lg, lb));
        const std::int32_t cut = std::min<std::int32_t>(kMemoRangeCut, (range.bottom - range.top) / 2);
        const auto at = [&](std::int32_t hx, std::int32_t hy) {
            return SkPoint::Make(static_cast<float>(hx * scale), static_cast<float>(hy * scale));
        };
        const auto edge = [&](std::int32_t ex, std::int32_t inward) {
            SkPathBuilder path;
            path.moveTo(at(ex + inward, range.top));
            path.lineTo(at(ex, range.top + cut));
            path.lineTo(at(ex, range.bottom - cut));
            path.lineTo(at(ex + inward, range.bottom));
            sk->drawPath(path.detach(), pen);
        };
        if (range.head) edge(range.x0, cut);
        if (range.tail) edge(range.x1, -cut);
    }
}

void Painter::drawProofMarks(const Node& node, std::int32_t x, std::int32_t y, const std::vector<Mark>& marks,
                             const LinePlacement& place) {
    if (!(showGlyphs() || placesOnly) || node.role == "glyph" || node.role == "reverse" || node.role == "glyphDeco") return;
    const std::vector<ProofSpan> spans = lineProofs(node, marks, place);
    if (spans.empty() || !doc) return;
    SkCanvas* sk = placesOnly ? nullptr : canvas();
    for (const ProofSpan& span : spans) {
        const model::CharShape* shape = findCharShape(*doc, span.charShapeId);
        const std::int32_t s = shape && shape->height > 0 ? shape->height : 1000;
        const std::int32_t b = y + node.marginTop + glyphBaseIn(node, s, node.font, 0).baseline;
        const std::int32_t x0 = x + span.x0;
        const std::int32_t x1 = x + std::max(span.x1, span.x0);
        const std::int32_t lx = x + span.lineLeft;
        const auto ax = [&](std::int32_t hx, double fs) { return static_cast<float>((hx + fs * s) * scale); };
        const auto ay = [&](double fs) { return static_cast<float>((b + fs * s) * scale); };
        // 글로 쓰는 부분(?, ◎, 고칠 글)은 글자 길로 남긴다. 화면 글자 자리 기록(placesOnly)에도 들어간다.
        const auto text = [&](const std::string& str, std::int32_t cx, double baseFs, double sizeFs, bool center) {
            const int size = std::max(1, static_cast<int>(s * sizeFs));
            std::vector<std::int32_t> adv;
            std::vector<std::int32_t> inks;
            measureText(doc, str, span.charShapeId, size, 100, adv, inks);
            std::int32_t width = 0;
            for (const std::int32_t w : adv) width += w * size / std::max<std::int32_t>(1, s);
            std::int32_t pen = center ? cx - width / 2 : cx;
            std::size_t idx = 0;
            std::size_t nth = 0;
            char32_t cp = 0;
            const std::int32_t base = b + static_cast<std::int32_t>(baseFs * s);
            while (nextCp(str, idx, cp)) {
                glyph(cp, "함초롬바탕", size, 100, false, false, kProofRed, pen, base);
                pen += nth < adv.size() ? adv[nth] * size / std::max<std::int32_t>(1, s) : size;
                ++nth;
            }
        };
        using K = model::ProofKind;
        if (span.kind == K::Thinking && span.head) text("?", x0 + s / 10, 0.62, 0.5, false);
        if (span.kind == K::Praise && span.head) text("\xE2\x97\x8E", x0, 0.75, 0.5, false);  // ◎
        if (span.kind == K::SimpleChange && !span.text.empty()) text(span.text, (x0 + x1) / 2, 0.85, 0.6, true);
        if (span.kind == K::Sign && span.sign == 15 && !span.text.empty()) text(span.text, x0, 0.95, 0.6, true);
        if (span.kind == K::Sign && span.sign == 14 && !span.text.empty()) text(span.text, x0, 0.42, 0.6, true);
        if (span.kind == K::Hyperlink && span.head && !span.text.empty()) text(span.text, x0, 0.55, 0.6, false);
        if (!sk) continue;
        SkPaint pen;
        pen.setAntiAlias(true);
        pen.setStyle(SkPaint::kStroke_Style);
        pen.setColor(SkColorSetRGB(0xFF, 0, 0));
        pen.setStrokeWidth(std::max(1.f, static_cast<float>(s) * scale * 0.045f));
        pen.setStrokeCap(SkPaint::kRound_Cap);
        pen.setStrokeJoin(SkPaint::kRound_Join);
        SkPathBuilder p;
        const float px0 = x0 * scale;
        const float px1 = x1 * scale;
        const float em = static_cast<float>(s) * scale;
        // 범위 아래 물결(생각표·칭찬표)과 톱니(톱니표).
        const auto wave = [&](bool saw) {
            const float y0 = ay(0.12);
            const float step = std::max(2.f, em * (saw ? 0.08f : 0.1f));
            const float amp = std::max(1.f, em * (saw ? 0.05f : 0.035f));
            p.moveTo(px0, y0);
            int k = 0;
            for (float xx = px0 + step / 2; xx <= px1 + 0.1f; xx += step / 2, ++k) {
                const float yy = y0 + ((k % 2 == 0) ? amp : -amp);
                if (saw) p.lineTo(xx, yy);
                else p.quadTo(xx - step / 4, yy, xx, y0);
            }
        };
        switch (span.kind) {
            case K::Delete:
                p.moveTo(px0 - em * 0.05f, ay(-0.47));
                p.lineTo(px1 + em * 0.05f, ay(-0.47));
                p.moveTo(px0 - em * 0.05f, ay(-0.23));
                p.lineTo(px1 + em * 0.05f, ay(-0.23));
                break;
            case K::Change:
                p.moveTo(px0, ay(-0.35));
                p.lineTo(px1, ay(-0.35));
                break;
            case K::Clipping: {
                const SkRect box = SkRect::MakeLTRB(px0 - em * 0.03f, ay(-0.92), px1 + em * 0.03f, ay(0.17));
                p.addRRect(SkRRect::MakeRectXY(box, em * 0.18f, em * 0.18f));
                if (span.tail) {
                    // 오른쪽 위로 올라가는 작은 고리 셋.
                    float cx = box.fRight + em * 0.02f;
                    float cy = box.fTop + em * 0.05f;
                    p.moveTo(cx, cy);
                    for (int k = 0; k < 3; ++k) {
                        p.cubicTo(cx + em * 0.12f, cy - em * 0.05f, cx + em * 0.14f, cy - em * 0.28f, cx + em * 0.07f,
                                  cy - em * 0.22f);
                        p.cubicTo(cx + em * 0.02f, cy - em * 0.17f, cx + em * 0.06f, cy - em * 0.05f,
                                  cx + em * 0.13f, cy - em * 0.04f);
                        cx += em * 0.11f;
                        cy -= em * 0.03f;
                    }
                }
                break;
            }
            case K::Attach: {
                const float top = ay(0.05);
                const float bottom = ay(0.45);
                const float r = em * 0.12f;
                p.moveTo(px0, top);
                p.lineTo(px0, bottom - r);
                p.quadTo(px0, bottom, px0 + r, bottom);
                p.lineTo(px1 - r, bottom);
                p.quadTo(px1, bottom, px1, bottom - r);
                p.lineTo(px1, top);
                break;
            }
            case K::LineAttach:
                if (!span.head) break;
                p.moveTo(ax(lx, -0.5), ay(-0.8));
                p.quadTo(ax(lx, -0.45), ay(-0.5), ax(lx, -0.05), ay(-0.5));
                p.moveTo(ax(lx, -0.05), ay(-0.15));
                p.quadTo(ax(lx, -0.5), ay(-0.15), ax(lx, -0.5), ay(0.15));
                p.lineTo(ax(lx, -0.5), ay(0.75));
                break;
            case K::Sawtooth: wave(true); break;
            case K::Thinking:
            case K::Praise: wave(false); break;
            case K::Line:
                p.moveTo(px0, ay(0.12));
                p.lineTo(px1, ay(0.12));
                break;
            case K::SimpleChange:
                p.moveTo(span.head ? px0 - em * 0.12f : px0, span.head ? ay(0.12) : ay(-0.35));
                p.lineTo(px0, ay(-0.35));
                p.lineTo(px1, ay(-0.35));
                if (span.tail) p.lineTo(px1 + em * 0.12f, ay(0.12));
                break;
            case K::Sign:
                if (span.sign == 1) {
                    p.moveTo(px0 - em * 0.22f, ay(-1.13));
                    p.lineTo(px0, ay(-0.6));
                    p.lineTo(px0 + em * 0.22f, ay(-1.13));
                } else if (span.sign == 2) {
                    p.moveTo(px0 - em, ay(0.15));
                    p.lineTo(px0, ay(0.15));
                    p.lineTo(px0, ay(-0.95));
                    p.lineTo(px0 + em, ay(-0.95));
                } else if (span.sign == 3) {
                    p.moveTo(ax(lx, -0.55), ay(-0.3));
                    p.lineTo(ax(lx, 0.5), ay(0.2));
                    p.lineTo(ax(lx, -0.55), ay(0.7));
                } else if (span.sign == 14) {
                    p.moveTo(px0 - em * 0.2f, ay(0.45));
                    p.lineTo(px0, ay(0.1));
                    p.lineTo(px0 + em * 0.2f, ay(0.45));
                } else if (span.sign == 15) {
                    p.moveTo(px0 - em * 0.4f, ay(0.05));
                    p.quadTo(px0 - em * 0.4f, ay(0.17), px0 - em * 0.2f, ay(0.17));
                    p.quadTo(px0, ay(0.17), px0, ay(0.28));
                    p.quadTo(px0, ay(0.17), px0 + em * 0.2f, ay(0.17));
                    p.quadTo(px0 + em * 0.4f, ay(0.17), px0 + em * 0.4f, ay(0.05));
                }
                break;
            case K::Transfer: {
                // 자리 바꿈표: 앞 조각은 아래로 감싸고 가른 자리에서 올라가 뒤 조각을 위로 감싼다(S 자).
                const float top = ay(-1.05);
                const float bottom = ay(0.12);
                const float r = em * 0.35f;
                const float cut = span.split >= 0 ? (x + span.split) * scale : px1;
                if (span.head) {
                    p.moveTo(px0, top);
                    p.lineTo(px0, bottom - r);
                    p.quadTo(px0, bottom, px0 + r, bottom);
                } else {
                    p.moveTo(px0, bottom);
                }
                p.lineTo(cut - r, bottom);
                if (span.split >= 0) {
                    p.quadTo(cut, bottom, cut, bottom - r);
                    p.lineTo(cut, top + r);
                    p.quadTo(cut, top, cut + r, top);
                    p.lineTo(px1 - r, top);
                    if (span.tail) {
                        p.quadTo(px1, top, px1, top + r);
                        p.lineTo(px1, bottom);
                    } else {
                        p.lineTo(px1, top);
                    }
                }
                break;
            }
            case K::LineTransfer:
                // 줄 서로 바꿈표: 윗줄 위에 줄 끝까지 가로줄. 아랫줄은 %spl 2 가 상자로 두른다.
                if (span.head) {
                    p.moveTo(ax(lx, -0.5), ay(-1.05));
                    p.lineTo(static_cast<float>((x + span.lineRight) * scale), ay(-1.05));
                }
                break;
            case K::Split:
                if (span.sign == 2) {
                    const float l = ax(lx, -0.5);
                    const float rr = static_cast<float>((x + span.lineRight) * scale);
                    p.moveTo(rr, ay(-1.0));
                    p.lineTo(l, ay(-1.0));
                    p.lineTo(l, ay(0.52));
                    p.lineTo(rr, ay(0.52));
                }
                break;
            case K::RightMove:
            case K::LeftMove: {
                // 줄 머리에서 시작 위치에 세로줄, 간격만큼 떨어진 끝은 위아래로 꺾는다. 오른 자리는 '[', 왼 자리는 ']'
                // (세로줄이 시작 + 간격, 꺾임이 시작 자리). 한글 화면(시작 15·20·30pt, 간격 10pt)에서 잰 자리다.
                if (!span.head) break;
                const bool right = span.kind == K::RightMove;
                const std::int32_t nearHu = lx + (right ? span.moveStart : span.moveStart + span.moveGap);
                const std::int32_t farHu = lx + (right ? span.moveStart + span.moveGap : span.moveStart);
                const float nearX = static_cast<float>(nearHu * scale);
                const float farX = static_cast<float>(farHu * scale);
                p.moveTo(farX, ay(-1.2));
                p.lineTo(farX, ay(-0.9));
                p.lineTo(nearX, ay(-0.9));
                p.lineTo(nearX, ay(0.7));
                p.lineTo(farX, ay(0.7));
                p.lineTo(farX, ay(1.0));
                break;
            }
            default: break;
        }
        sk->drawPath(p.detach(), pen);
    }
}

void Painter::drawProofBackgrounds(const Node& node, std::int32_t x, std::int32_t y, const LinePlacement& place) {
    if (placesOnly || !showGlyphs() || !doc || node.role == "glyph" || node.role == "reverse" || node.role == "glyphDeco") return;
    if (!lineHasProofInk(node, doc)) return;
    const std::vector<ProofSpan> spans = lineProofs(node, collectMarks(node), place);
    for (const ProofSpan& span : spans) {
        if (span.kind != model::ProofKind::Hyperlink || span.x1 <= span.x0) continue;
        // 자료 연결: 범위 글자 상자에 회색 #D6D6D6(한글 화면).
        const model::CharShape* shape = findCharShape(*doc, span.charShapeId);
        const std::int32_t s = shape && shape->height > 0 ? shape->height : 1000;
        const std::int32_t b = y + node.marginTop + glyphBaseIn(node, s, node.font, 0).baseline;
        const int x0 = px(x + span.x0);
        const int x1 = px(x + span.x1);
        const int y0 = px(b - s * 87 / 100);
        const int y1 = px(b + s * 8 / 100);
        if (x1 > x0 && y1 > y0) fillRect(bitmap, x0, y0, x1 - x0, y1 - y0, 0xD6, 0xD6, 0xD6);
    }
}

bool lineHasMemoRange(const Node& line, const model::Document* doc) {
    if (!doc || line.story <= 0 || line.paragraph < 0 || !viewMarks().memo) return false;
    const model::Paragraph* para = storyParagraph(*doc, line.story, line.paragraph);
    if (!para) return false;
    for (const model::Run& run : para->runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control && isPlainMemoField(item.control)) return true;
        }
    }
    return false;
}

bool lineHasProofInk(const Node& line, const model::Document* doc) {
    if (!doc || line.story <= 0 || line.paragraph < 0 || !viewMarks().proofMarks()) return false;
    const model::Paragraph* para = storyParagraph(*doc, line.story, line.paragraph);
    if (!para) return false;
    for (const model::Run& run : para->runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control && model::proofKindOf(item.control) != model::ProofKind::None) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace chwp::render::paint_detail
