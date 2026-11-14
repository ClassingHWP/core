#include "render/painter.hpp"

#include "chwp/render/paint.hpp"

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "chwp/common/unicode.hpp"
#include "chwp/reader.hpp"
#include "chwp/render/view.hpp"
#include "render/line_breaker.hpp"
#include "render/scene_detail.hpp"

// Painter: 줄 그리기: 글자 표시, 글자 꾸밈, 문단 꾸밈, 조판 보고, 트리 순회

namespace chwp::render::paint_detail {

const model::Style* Painter::findStyle(const model::Document& doc, std::uint32_t id) {
    const auto& styles = doc.header.styles;
    if (id < styles.size() && styles[id].id == id) return &styles[id];
    for (const auto& st : styles) {
        if (st.id == id) return &st;
    }
    return nullptr;
}

const model::ParaShape* Painter::findParaShape(const model::Document& doc, std::uint32_t id) {
    const auto& shapes = doc.header.paraShapes;
    if (id < shapes.size() && shapes[id].id == id) return &shapes[id];
    for (const auto& ps : shapes) {
        if (ps.id == id) return &ps;
    }
    return nullptr;
}

const model::CharShape* Painter::findCharShape(const model::Document& doc, std::uint32_t id) {
    const auto& shapes = doc.header.charShapes;
    if (id < shapes.size() && shapes[id].id == id) return &shapes[id];
    for (const auto& cs : shapes) {
        if (cs.id == id) return &cs;
    }
    return nullptr;
}

std::string Painter::resolveCharShapeFontName(const model::Document& doc, const model::CharShape& shape) {
    for (std::size_t slot = 0; slot < shape.fontRef.size() && slot < doc.header.fonts.size(); ++slot) {
        const auto& fonts = doc.header.fonts[slot];
        if (fonts.empty()) continue;
        const std::uint32_t id = shape.fontRef[slot];
        if (id < fonts.size() && fonts[id].id == id && !fonts[id].face.empty()) return fonts[id].face;
        for (const auto& f : fonts) {
            if (f.id == id && !f.face.empty()) return f.face;
        }
        if (id < fonts.size() && !fonts[id].face.empty()) return fonts[id].face;
    }
    return {};
}

bool Painter::markPads(const Mark& mark) {
    if (mark.object) return false;
    return mark.cp == 0 || mark.cp == ' ' || mark.cp == 0x2007 || mark.cp == 0x00A0 || mark.cp == '\t';
}

Painter::LineFit Painter::fitLine(const Node& node, const std::vector<Mark>& marks) const {
    LineFit fit;
    fit.contentCount = static_cast<int>(marks.size());
    while (fit.contentCount > 0 && marks[static_cast<std::size_t>(fit.contentCount - 1)].adjustable) --fit.contentCount;
    while (fit.firstGlyph < fit.contentCount && markPads(marks[static_cast<std::size_t>(fit.firstGlyph)]))
        ++fit.firstGlyph;
    std::int32_t contentEnd = 0;
    int gapsBetween = 0;
    std::int32_t spaceTotal = 0;
    for (int i = 0; i < fit.contentCount; ++i) {
        const Mark& mark = marks[static_cast<std::size_t>(i)];
        contentEnd = std::max(contentEnd, mark.x + mark.width);
        if (mark.adjustable && i >= fit.firstGlyph) {
            ++gapsBetween;
            spaceTotal += std::max<std::int32_t>(0, mark.width);
        }
    }
    fit.slide = std::max<std::int32_t>(0, node.indent);
    const std::int32_t rawRoom = node.width - fit.slide - contentEnd;
    const std::int32_t room = std::max<std::int32_t>(0, rawRoom);
    const std::string& align = node.align;
    const bool justifySpaces = gapsBetween > 0 && !node.lineData().lineBreak &&
                               (align == "DISTRIBUTE_SPACE" ||
                                ((align.empty() || align == "JUSTIFY") && !node.lineData().lastLine));
    if (rawRoom < 0 && gapsBetween > 0 && spaceTotal > 0) {
        const std::int32_t deficit = std::min<std::int32_t>(-rawRoom, spaceTotal);
        fit.perSpace = -(deficit / gapsBetween);
        fit.spaceLeft = -(deficit % gapsBetween);
    } else if (align == "CENTER") fit.shift = room / 2;
    else if (align == "RIGHT") fit.shift = room;
    else if (align == "DISTRIBUTE" && marks.size() > 1)
        fit.perGap = room / static_cast<std::int32_t>(marks.size() - 1);
    else if (justifySpaces) {
        fit.perSpace = room / gapsBetween;
        fit.spaceLeft = room % gapsBetween;
    } else if (gapsBetween == 0 && room > 0 && !node.lineData().lineBreak && !node.lineData().lastLine &&
               (align.empty() || align == "JUSTIFY")) {
        // 한글 2024: 띄어쓰기가 없으면 남은 폭을 글자 뒤마다 똑같이 나눈다. 줄 마지막 글자와 글자처럼 넣은
        // 개체 뒤에는 주지 않는다("가나다라"는 세 칸, "가나[그림]다라"는 가·나·다 뒤, tmp/insert/j-*).
        int charGaps = 0;
        for (std::size_t i = 0; i < marks.size(); ++i) {
            if (takesCharGap(marks, i, fit)) ++charGaps;
        }
        if (charGaps > 0) {
            fit.perChar = room / charGaps;
            fit.charLeft = room % charGaps;
        }
    }
    return fit;
}

bool Painter::takesCharGap(const std::vector<Mark>& marks, std::size_t index, const LineFit& fit) {
    if (static_cast<int>(index) < fit.firstGlyph || static_cast<int>(index) + 1 >= fit.contentCount) return false;
    const Mark& mark = marks[index];
    return !mark.object && mark.cp != 0 && mark.cp != U'\t';
}

Painter::GlyphBase Painter::glyphBaseIn(const Node& line, std::int32_t size, const std::string& font,
                                        int charOffset) {
    // 글꼴 기준이면 글자 크기가 달라도 같은 기준선을 쓴다.
    // 줄 기준선은 글자 기준선, 그림은 위에서 85%, 수식은 수식 높이×백분율 중 큰 값이다.
    // 위·가운데·아래는 각 글자 상자를 그 줄 글자 상자의 그 자리에 맞춘다.
    GlyphBase out;
    const std::int32_t box = line.lineData().textHeight > 0 ? line.lineData().textHeight : size;
    out.ownBase = chwp::layout::glyphBaseline(size);
    if (line.lineData().matchFontBaseline) {
        out.ownBase = chwp::layout::fontLineBox(font, size).ascent;
    }
    const std::string& mode = line.vertAlign;
    out.baseline = line.lineData().lineBaseline >= 0 ? line.lineData().lineBaseline : chwp::layout::glyphBaseline(box);
    if (mode == "TOP") out.baseline = out.ownBase;
    else if (mode == "CENTER") out.baseline = (box - size) / 2 + out.ownBase;
    else if (mode == "BOTTOM") out.baseline = box - size + chwp::layout::glyphBaseline(size);
    // 글자 위치는 그 글자 크기의 백분율. 양수는 아래로, 음수는 위로. 줄 높이는 바꾸지 않는다.
    out.baseline += size * charOffset / 100;
    return out;
}

Painter::LinePlacement Painter::placeLine(const Node& node, const std::vector<Mark>& marks) const {
    LinePlacement place;
    place.fit = fitLine(node, marks);
    const LineFit& fit = place.fit;
    place.at.reserve(marks.size());
    // 조판 부호 이름표는 정렬과 줄 나눔을 그대로 두고, 놓인 자리부터 뒤 글자를 그 폭만큼 민다(한글 화면에서 확인).
    place.labels = lineLabels(node, marks);
    place.labelAt.reserve(place.labels.size());
    std::size_t nextLabel = 0;
    std::int32_t labelShift = 0;
    std::int32_t extra = 0;
    std::int32_t spaceLeft = fit.spaceLeft;
    std::int32_t charLeft = fit.charLeft;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const Mark& mark = marks[i];
        // 빈칸을 대신하는 이름표는 그 빈칸 자리에 그리고, 빈칸 폭 대신 이름표 폭만큼 뒤 마크를 민다.
        std::int32_t replaceShift = 0;
        while (nextLabel < place.labels.size() && place.labels[nextLabel].before <= i) {
            const LineLabel& label = place.labels[nextLabel];
            place.labelAt.push_back(fit.slide + mark.x + fit.shift + extra + labelShift);
            if (label.replaces && label.before == i) replaceShift += label.width - mark.width;
            else labelShift += label.width;
            ++nextLabel;
        }
        place.at.push_back(fit.slide + mark.x + fit.shift + extra + labelShift);
        labelShift += replaceShift;
        // 늘리거나 줄인 공백은 그 공백 다음 마크부터 민다. 나머지 1 HU 는 앞 공백부터 하나씩 나눈다.
        if (mark.adjustable && static_cast<int>(i) >= fit.firstGlyph && static_cast<int>(i) < fit.contentCount) {
            extra += fit.perSpace;
            if (spaceLeft > 0) {
                extra += 1;
                --spaceLeft;
            } else if (spaceLeft < 0) {
                extra -= 1;
                ++spaceLeft;
            }
        }
        if (i + 1 < marks.size()) extra += fit.perGap;
        if ((fit.perChar > 0 || charLeft > 0) && takesCharGap(marks, i, fit)) {
            extra += fit.perChar;
            if (charLeft > 0) {
                extra += 1;
                --charLeft;
            }
        }
    }
    if (marks.empty()) {
        place.end = fit.slide;
    } else {
        const Mark& last = marks.back();
        place.end = fit.slide + last.x + last.width + fit.shift + extra;
    }
    place.end += labelShift;
    for (; nextLabel < place.labels.size(); ++nextLabel) {
        place.labelAt.push_back(place.end);
        place.end += place.labels[nextLabel].width;
    }
    return place;
}

ViewMarks Painter::shownMarks() const { return viewMarks(); }

namespace {

const char* numberKindName(const std::string& numType) {
    if (numType == "PAGE") return "쪽";
    if (numType == "FOOTNOTE") return "각주";
    if (numType == "ENDNOTE") return "미주";
    if (numType == "PICTURE") return "그림";
    if (numType == "TABLE") return "표";
    if (numType == "EQUATION") return "수식";
    return nullptr;
}

// 필드 이름. 하이퍼링크·누름틀·메모·계산식·날짜 코드·사용자 정보·책갈피 영역·상호 참조·파일 경로·문서 정보는 한글
// 화면에서 [이름 시작]·[이름 끝]을 확인했다. 메일 머지는 한글 조판 부호 목록 이름이다.
const char* fieldMarkName(const std::string& ctrlId) {
    if (ctrlId == "%hlk") return "하이퍼링크";
    if (ctrlId == "%clk") return "누름틀";
    if (ctrlId == "%%me") return "메모";
    if (ctrlId == "%xrf") return "상호 참조";
    if (ctrlId == "%dte" || ctrlId == "%ddt") return "날짜 코드";
    if (ctrlId == "%pat") return "파일 경로";
    if (ctrlId == "%smr") return "문서 정보";
    if (ctrlId == "%usr") return "사용자 정보";
    if (ctrlId == "%mmg") return "메일 머지";
    if (ctrlId == "%fmu") return "계산식";
    if (ctrlId == "%bmk") return "책갈피 영역";
    return "필드";
}

// 필드 이름표. 교정 부호는 한글 교정 부호 이름이다([지움표 시작] 등, 한글 화면에서 확인).
std::string fieldLabelName(const model::Control& c) {
    if (model::proofKindOf(c) != model::ProofKind::None) return model::proofName(c);
    return fieldMarkName(c.ctrlId);
}

const char* pageSideName(const std::string& applyPageType) {
    // 한글 2024 화면: 양쪽은 붙여 쓰고 홀수·짝수는 띄어 쓴다.
    if (applyPageType == "EVEN") return "(짝수 쪽)";
    if (applyPageType == "ODD") return "(홀수 쪽)";
    return "(양쪽)";
}

// 컨트롤의 조판 부호 이름. 비면 보이지 않는다. 한글 2024 화면에서 확인한 이름: 쪽 번호 위치, 머리말(양쪽),
// 꼬리말(양쪽), 머리말·꼬리말(홀수 쪽·짝수 쪽), 새 쪽 번호, 쪽 번호, 감추기, 책갈피, 글자 겹치기, 덧말, 각주, 미주,
// 미주 번호, 표, 그림, 수식, 숨은 설명, 사각형, 타원, 선, 그리기(묶음 개체).
// 구역 정의·단 정의는 구역 첫머리에서 보이지 않는다. 나머지는 한글 조판 부호 목록 이름을 쓴다.
std::string controlMarkName(const model::Control& c) {
    using K = model::ControlKind;
    switch (c.kind) {
        case K::Table: return "표";
        case K::Picture: return "그림";
        case K::Equation: return "수식";
        case K::Shape: {
            // 도형 종류 이름. 사각형(글상자도)·타원·선은 한글 화면에서 확인했다.
            if (c.shapeType == "rect") return "사각형";
            if (c.shapeType == "ellipse") return "타원";
            if (c.shapeType == "line") return "선";
            if (c.shapeType == "arc") return "호";
            if (c.shapeType == "polygon") return "다각형";
            if (c.shapeType == "curve") return "곡선";
            if (c.shapeType == "textart") return "글맵시";
            return "그리기";
        }
        case K::Container: return "그리기";
        case K::Ole: return "OLE 개체";
        case K::Header: return std::string("머리말") + pageSideName(c.applyPageType);
        case K::Footer: return std::string("꼬리말") + pageSideName(c.applyPageType);
        case K::Footnote: return "각주";
        case K::Endnote: return "미주";
        case K::PageHiding: return "감추기";
        case K::PageNumPos: return "쪽 번호 위치";
        case K::Bookmark: return "책갈피";
        case K::Dutmal: return "덧말";
        case K::Compose: return "글자 겹치기";
        case K::HiddenComment: return c.ctrlId == "tcmt" ? "숨은 설명" : "";
        case K::AutoNumber: {
            if (c.numType == "TOTAL_PAGE") return "전체 쪽수";
            const char* kind = numberKindName(c.numType);
            return kind ? std::string(kind) + " 번호" : "";
        }
        case K::NewNumber: {
            const char* kind = numberKindName(c.numType);
            return kind ? std::string("새 ") + kind + " 번호" : "";
        }
        default: return "";
    }
}

}  // namespace

std::vector<Painter::LineLabel> Painter::lineLabels(const Node& node, const std::vector<Mark>& marks) const {
    std::vector<LineLabel> out;
    if (!doc || node.kind != NodeKind::Line || node.story <= 0 || node.paragraph < 0 || node.start < 0) return out;
    if (!shownMarks().control) return out;
    const model::Paragraph* para = storyParagraph(*doc, node.story, node.paragraph);
    if (!para) return out;
    // 줄 자리(start·end)는 저장된 줄 정보로 놓은 문단이면 lineseg 칸(탭·컨트롤 8칸), 아니면 커서 칸이다
    // (buildAtoms·offsetToCharIndex 와 같은 셈).
    const bool stored = !para->lineSegs.empty();
    const int lineStart = node.start;
    const int lineEnd = std::max(node.end, node.start);
    const bool lastLine = node.lineData().lastLine;
    // 커서 칸 k 앞 → 마크 번호. 자동 번호 대신 넣은 글자(synthetic)는 커서 칸이 없어 그 뒤에 둔다.
    std::vector<std::size_t> caretToMark;
    caretToMark.reserve(marks.size() + 1);
    for (std::size_t i = 0; i < marks.size(); ++i) {
        if (!marks[i].synthetic) caretToMark.push_back(i);
    }
    caretToMark.push_back(marks.size());
    const auto markBefore = [&](int caret) {
        const int last = static_cast<int>(caretToMark.size()) - 1;
        return caretToMark[static_cast<std::size_t>(std::clamp(caret, 0, last))];
    };
    const auto add = [&](std::size_t before, const std::string& name, std::uint32_t charShapeId, bool replaces = false) {
        if (name.empty()) return;
        LineLabel label;
        label.before = before;
        label.replaces = replaces;
        label.text = "[" + name + "]";
        label.charShapeId = charShapeId;
        const model::CharShape* shape = findCharShape(*doc, charShapeId);
        const int size = shape && shape->height > 0 ? shape->height : 1000;
        std::vector<std::int32_t> inks;
        measureText(doc, label.text, charShapeId, size, 100, label.advances, inks);
        for (const std::int32_t w : label.advances) label.width += w;
        out.push_back(std::move(label));
    };
    // 열린 필드. 끝 이름표는 짝인 시작의 이름을 쓴다. 앞 문단에서 시작한 필드는 이름을 몰라 "필드"다.
    std::vector<const model::Control*> openFields;
    int pos = 0;
    int caret = 0;  // 이 줄 안에서 지난 커서 칸 수
    const auto inLine = [&](int at, bool zeroWidth) {
        if (at >= lineStart && at < lineEnd) return true;
        return zeroWidth && lastLine && at == lineEnd;
    };
    for (const model::Run& run : para->runs) {
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                std::size_t idx = 0;
                char32_t cp = 0;
                while (nextCp(item.text, idx, cp)) {
                    const int units = stored ? (cp == '\t' ? 8 : (cp > 0xFFFF ? 2 : 1)) : 1;
                    if (inLine(pos, false) && cp != '\n') {
                        // 고정폭·묶음 빈 칸은 이름표가 그 빈칸을 대신한다(한글 화면: 앞뒤에 빈칸 틈이 없다).
                        if (cp == 0x2007) add(markBefore(caret), "고정폭 빈 칸", run.charShapeId, true);
                        if (cp == 0x00A0) add(markBefore(caret), "묶음 빈 칸", run.charShapeId, true);
                        ++caret;
                    }
                    pos += units;
                }
                continue;
            }
            const model::Control& c = item.control;
            const bool caretChar = isCaretCharControl(c);
            const int units = stored ? 8 : (caretChar ? 1 : 0);
            std::string name;
            if (c.kind == model::ControlKind::Field) {
                openFields.push_back(&c);
                name = fieldLabelName(c) + " 시작";
            } else if (c.kind == model::ControlKind::FieldEnd) {
                const model::Control* begin = openFields.empty() ? nullptr : openFields.back();
                if (!openFields.empty()) openFields.pop_back();
                name = (begin ? fieldLabelName(*begin) : std::string("필드")) + " 끝";
            } else {
                name = controlMarkName(c);
            }
            if (caretChar) {
                if (inLine(pos, false)) {
                    ++caret;
                    add(markBefore(caret), name, run.charShapeId);
                }
            } else if (inLine(pos, true)) {
                add(markBefore(caret), name, run.charShapeId);
            }
            pos += units;
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const LineLabel& a, const LineLabel& b) { return a.before < b.before; });
    return out;
}

void Painter::appendLineSeg(std::string& out, const model::LineSeg& seg, std::int32_t rise, std::int32_t below,
                            bool measured) {
    const std::int32_t pitch = seg.textHeight + seg.spacing + (measured ? rise + below : 0);
    out += "{\"textpos\":" + std::to_string(seg.textPos);
    out += ",\"vertpos\":" + std::to_string(seg.vertPos);
    out += ",\"vertsize\":" + std::to_string(seg.vertSize);
    out += ",\"textheight\":" + std::to_string(seg.textHeight);
    out += ",\"baseline\":" + std::to_string(seg.baseline);
    out += ",\"spacing\":" + std::to_string(seg.spacing);
    out += ",\"horzpos\":" + std::to_string(seg.horzPos);
    out += ",\"horzsize\":" + std::to_string(seg.horzSize);
    out += ",\"flags\":" + std::to_string(seg.flags);
    out += ",\"줄높이\":" + std::to_string(pitch);
    if (measured) {
        out += ",\"윗여백\":" + std::to_string(rise);
        out += ",\"아래돌출\":" + std::to_string(below);
    }
    out += "}";
}

void Painter::quoteJson(std::string& out, const std::string& text) {
    out.push_back('"');
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
}

void Painter::appendQuad(std::string& out, std::int32_t left, std::int32_t right, std::int32_t top,
                         std::int32_t bottom) {
    out += "[" + std::to_string(left) + "," + std::to_string(right) + "," + std::to_string(top) + "," +
           std::to_string(bottom) + "]";
}

const char* Painter::objectLabel(const Node& node) {
    if (node.kind == NodeKind::Cell) return "칸";
    if (node.role == "picture") return "그림";
    if (node.role == "table") return "표";
    if (node.role == "container") return "묶음";
    if (node.role == "equation") return "수식";
    if (node.ext().obj().shapeType == "rect") return "사각형";
    if (node.ext().obj().shapeType == "ellipse") return "타원";
    if (node.ext().obj().shapeType == "line") return "선";
    if (node.ext().obj().shapeType == "polygon") return "다각형";
    if (node.ext().obj().shapeType == "curve") return "곡선";
    if (node.ext().obj().shapeType == "arc") return "호";
    if (node.ext().obj().shapeType == "ole") return "OLE";
    return "도형";
}

void Painter::reportObject(const Node& node, std::int32_t pageX, std::int32_t pageY) {
    if (pass != PaintLayer::All && pass != PaintLayer::Text) return;
    if (node.kind != NodeKind::Object && node.kind != NodeKind::Cell) return;
    if (node.width <= 0 || node.height <= 0) return;
    const std::string key = "o:" + std::to_string(pageIndex) + ":" + std::to_string(pageX) + ":" +
                            std::to_string(pageY) + ":" + std::to_string(node.width) + ":" +
                            std::to_string(node.height) + ":" + std::to_string(node.ext().row) + ":" +
                            std::to_string(node.ext().col) + ":" + std::to_string(node.ext().obj().zOrder) + ":" + node.role +
                            ":" + node.ext().obj().shapeType;
    if (!claimLineWarn(key)) return;
    std::string diff;
    auto mark = [&](const char* name, bool bad) {
        if (!bad) return;
        if (!diff.empty()) diff += ',';
        diff += '"';
        diff += name;
        diff += '"';
    };
    if (node.fileAudit) {
        if (node.role == "equation" && node.ext().obj().eqBase >= 0) {
            mark("너비", node.ext().obj().fileWidth != node.ext().obj().eqWidth);
            mark("높이", node.ext().obj().fileHeight != node.ext().obj().eqHeight);
            int storedBase = node.ext().obj().fileEqBase >= 0 ? node.ext().obj().fileEqBase : node.ext().obj().baseLine;
            if (storedBase == 0) storedBase = 85;
            if (storedBase > 0) mark("기준선", storedBase != node.ext().obj().eqBase);
        } else {
            mark("너비", node.ext().obj().fileWidth != node.width);
            mark("높이", node.ext().obj().fileHeight != node.height);
        }
        if (node.kind == NodeKind::Cell) {
            mark("안여백", node.ext().obj().filePadLeft != node.marginLeft || node.ext().obj().filePadRight != node.marginRight ||
                             node.ext().obj().filePadTop != node.marginTop || node.ext().obj().filePadBottom != node.marginBottom);
        }
    }
    std::string out = "{\"요소\":";
    quoteJson(out, objectLabel(node));
    if (!diff.empty()) out += ",\"경고\":\"개체\"";
    if (pageIndex >= 0) out += ",\"쪽\":" + std::to_string(pageIndex + 1);
    if (node.kind == NodeKind::Cell) {
        out += ",\"행\":" + std::to_string(node.ext().row + 1);
        out += ",\"열\":" + std::to_string(node.ext().col + 1);
        if (node.ext().rowSpan > 1) out += ",\"행합\":" + std::to_string(node.ext().rowSpan);
        if (node.ext().colSpan > 1) out += ",\"열합\":" + std::to_string(node.ext().colSpan);
    }
    if (!node.ext().obj().shapeType.empty() && node.role != "picture" && node.role != "table" && node.role != "equation" &&
        node.role != "container") {
        out += ",\"종류\":";
        quoteJson(out, node.ext().obj().shapeType);
    }
    out += ",\"다른필드\":[" + diff + "]";
    out += ",\"저장\":{";
    bool storedField = false;
    auto storedNum = [&](const char* name, std::int64_t value) {
        if (storedField) out += ',';
        storedField = true;
        out += '"';
        out += name;
        out += "\":";
        out += std::to_string(value);
    };
    auto storedText = [&](const char* name, const std::string& value) {
        if (value.empty()) return;
        if (storedField) out += ',';
        storedField = true;
        out += '"';
        out += name;
        out += "\":";
        quoteJson(out, value);
    };
    if (node.fileAudit) {
        storedNum("너비", node.ext().obj().fileWidth);
        storedNum("높이", node.ext().obj().fileHeight);
        storedText("가로기준", node.ext().obj().fileHorzRel);
        storedText("세로기준", node.ext().obj().fileVertRel);
        storedText("가로맞춤", node.ext().obj().fileHorzAlign);
        storedText("세로맞춤", node.vertAlign);
        storedNum("가로오프셋", node.ext().obj().fileX);
        storedNum("세로오프셋", node.ext().obj().fileY);
        if (node.ext().obj().fileGroupX != 0 || node.ext().obj().fileGroupY != 0) {
            storedNum("묶음x", node.ext().obj().fileGroupX);
            storedNum("묶음y", node.ext().obj().fileGroupY);
        }
        auto storedReal = [&](const char* name, double value) {
            if (storedField) out += ',';
            storedField = true;
            out += '"';
            out += name;
            out += "\":";
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.8g", value);
            out += buf;
        };
        const bool scaled = std::fabs(node.ext().obj().fileScaleX - 1.0) > 1e-9 || std::fabs(node.ext().obj().fileScaleY - 1.0) > 1e-9;
        if (scaled) {
            storedReal("배율x", node.ext().obj().fileScaleX);
            storedReal("배율y", node.ext().obj().fileScaleY);
        }
        if (node.ext().obj().fileMatrixX != 0 || node.ext().obj().fileMatrixY != 0) {
            storedNum("행렬이동x", node.ext().obj().fileMatrixX);
            storedNum("행렬이동y", node.ext().obj().fileMatrixY);
        }
        if (storedField) out += ',';
        storedField = true;
        out += node.kind == NodeKind::Cell ? "\"안여백\":" : "\"글상자안여백\":";
        appendQuad(out, node.ext().obj().filePadLeft, node.ext().obj().filePadRight, node.ext().obj().filePadTop, node.ext().obj().filePadBottom);
        if (node.kind == NodeKind::Cell && node.ext().obj().fileOwnMargin >= 0) storedNum("자체여백", node.ext().obj().fileOwnMargin);
        if (node.ext().obj().fileRows > 0) storedNum("행", node.ext().obj().fileRows);
        if (node.ext().obj().fileCols > 0) storedNum("열", node.ext().obj().fileCols);
    }
    if (node.kind == NodeKind::Object) {
        storedNum("바깥여백왼쪽", node.marginLeft);
        storedNum("바깥여백오른쪽", node.marginRight);
        storedNum("바깥여백위", node.marginTop);
        storedNum("바깥여백아래", node.marginBottom);
        storedNum("글자처럼", node.treatAsChar ? 1 : 0);
        storedText("배치", node.ext().obj().textWrap);
        storedNum("z", node.ext().obj().zOrder);
        if (node.ext().cellSpacing > 0) storedNum("셀간격", node.ext().cellSpacing);
        storedText("그림", node.ext().obj().binItemId);
        if (node.ext().obj().picture.clipRight > node.ext().obj().picture.clipLeft && node.ext().obj().picture.clipBottom > node.ext().obj().picture.clipTop) {
            storedNum("자르기왼쪽", node.ext().obj().picture.clipLeft);
            storedNum("자르기위", node.ext().obj().picture.clipTop);
            storedNum("자르기오른쪽", node.ext().obj().picture.clipRight);
            storedNum("자르기아래", node.ext().obj().picture.clipBottom);
        }
        if (node.ext().obj().picture.originWidth > 0) storedNum("원본너비", node.ext().obj().picture.originWidth);
        if (node.ext().obj().picture.originHeight > 0) storedNum("원본높이", node.ext().obj().picture.originHeight);
        if (node.ext().obj().picture.effect != 0) storedNum("효과", node.ext().obj().picture.effect);
        if (node.ext().obj().picture.bright != 0) storedNum("밝기", node.ext().obj().picture.bright);
        if (node.ext().obj().picture.contrast != 0) storedNum("명암", node.ext().obj().picture.contrast);
        if (node.ext().obj().picture.transparency != 0) storedNum("투명도", node.ext().obj().picture.transparency);
        storedText("수식", node.ext().obj().script);
        if (node.role == "equation" && node.ext().obj().eqBase >= 0) {
            const int storedBase = node.ext().obj().fileEqBase >= 0 ? node.ext().obj().fileEqBase : node.ext().obj().baseLine;
            if (storedBase >= 0) storedNum("기준선", storedBase);
        }
    }
    const std::int32_t measureW = node.role == "equation" && node.ext().obj().eqBase >= 0 ? node.ext().obj().eqWidth : node.width;
    const std::int32_t measureH = node.role == "equation" && node.ext().obj().eqBase >= 0 ? node.ext().obj().eqHeight : node.height;
    out += "},\"측정\":{\"x\":" + std::to_string(pageX);
    out += ",\"y\":" + std::to_string(pageY);
    out += ",\"너비\":" + std::to_string(measureW);
    out += ",\"높이\":" + std::to_string(measureH);
    if (node.role == "equation" && node.ext().obj().eqBase >= 0) out += ",\"기준선\":" + std::to_string(node.ext().obj().eqBase);
    if (node.kind == NodeKind::Cell) {
        out += ",\"안여백\":";
        appendQuad(out, node.marginLeft, node.marginRight, node.marginTop, node.marginBottom);
    }
    out += "},\"영역\":[" + std::to_string(pageX) + "," + std::to_string(pageY) + "," +
           std::to_string(node.width) + "," + std::to_string(node.height) + "]}";
    out += "\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

void Painter::reportCaption(const Node& node, std::int32_t pageX, std::int32_t pageY) {
    if (pass != PaintLayer::All && pass != PaintLayer::Text) return;
    if (node.role != "caption" || node.width <= 0 || node.height <= 0) return;
    const std::string key = "c:" + std::to_string(pageIndex) + ":" + std::to_string(pageX) + ":" +
                            std::to_string(pageY) + ":" + std::to_string(node.width) + ":" +
                            std::to_string(node.height);
    if (!claimLineWarn(key)) return;
    std::string out = "{\"요소\":\"캡션\"";
    if (pageIndex >= 0) out += ",\"쪽\":" + std::to_string(pageIndex + 1);
    if (!node.ext().obj().fileHorzRel.empty()) {
        out += ",\"방향\":";
        quoteJson(out, node.ext().obj().fileHorzRel);
    }
    out += ",\"저장\":{";
    out += "\"간격\":" + std::to_string(node.ext().obj().fileX);
    out += ",\"너비\":" + std::to_string(node.ext().obj().fileWidth);
    out += ",\"높이\":" + std::to_string(node.ext().obj().fileHeight);
    out += "},\"측정\":{\"x\":" + std::to_string(pageX);
    out += ",\"y\":" + std::to_string(pageY);
    out += ",\"너비\":" + std::to_string(node.width);
    out += ",\"높이\":" + std::to_string(node.height);
    out += "},\"다른필드\":[],\"영역\":[" + std::to_string(pageX) + "," + std::to_string(pageY) + "," +
           std::to_string(node.width) + "," + std::to_string(node.height) + "]}";
    out += "\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

void Painter::reportLineSeg(const Node& node, std::int32_t pageX, std::int32_t pageY, bool force) {
    // lineseg 와 측정에 찍힌 값이 하나라도 다르면 경고한다.
    const model::LineSeg& stored = node.lineData().storedSeg;
    const model::LineSeg& measured = node.lineData().measuredSeg;
    const std::int32_t storedPitch = stored.textHeight + stored.spacing;
    // 기준선 맞춤으로 삐져나온 양은 줄 피치에 들지 않는다. auditRise·auditBelow 는 그리는 넘침만 뜻한다.
    const std::int32_t measuredPitch = measured.textHeight + measured.spacing;
    std::string diff;
    auto mark = [&](const char* name, bool bad) {
        if (!bad) return;
        if (!diff.empty()) diff += ',';
        diff += '"';
        diff += name;
        diff += '"';
    };
    mark("textpos", stored.textPos != measured.textPos);
    mark("vertpos", stored.vertPos != measured.vertPos);
    mark("vertsize", stored.vertSize != measured.vertSize);
    mark("textheight", stored.textHeight != measured.textHeight);
    mark("baseline", stored.baseline != measured.baseline);
    mark("spacing", stored.spacing != measured.spacing);
    mark("horzpos", stored.horzPos != measured.horzPos);
    mark("horzsize", stored.horzSize != measured.horzSize);
    mark("flags", stored.flags != measured.flags);
    mark("줄높이", storedPitch != measuredPitch);
    if (diff.empty() && !force) return;
    // 글상자 안 첫 줄은 문단 번호와 줄 좌표가 본문과 겹친다. 쪽 좌표로 가른다.
    const std::string key = std::to_string(pageIndex) + ":" + std::to_string(pageX) + ":" +
                            std::to_string(pageY) + ":" + std::to_string(node.start) + ":" +
                            std::to_string(node.width);
    if (!claimLineWarn(key)) return;
    std::string out = "{\"요소\":\"줄\"";
    if (!diff.empty()) out += ",\"경고\":\"lineseg\"";
    if (pageIndex >= 0) out += ",\"쪽\":" + std::to_string(pageIndex + 1);
    if (node.paragraph >= 0) out += ",\"문단\":" + std::to_string(node.paragraph + 1);
    if (node.lineData().auditIndex >= 0) out += ",\"줄\":" + std::to_string(node.lineData().auditIndex + 1);
    out += ",\"글자\":" + std::to_string(std::max(0, node.start));
    out += ",\"다른필드\":[" + diff + "]";
    out += ",\"lineseg\":";
    appendLineSeg(out, stored, 0, 0, false);
    out += ",\"측정\":";
    appendLineSeg(out, measured, node.lineData().auditRise, node.lineData().auditBelow, true);
    const std::int32_t boxW = node.width;
    const std::int32_t boxH = node.lineData().textHeight > 0 ? node.lineData().textHeight : node.height;
    if (boxW > 0 && boxH > 0) {
        out += ",\"영역\":[" + std::to_string(pageX) + "," + std::to_string(pageY) + "," +
               std::to_string(boxW) + "," + std::to_string(boxH) + "]";
    }
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

std::vector<Painter::Mark> Painter::collectMarks(const Node& node) { return marksFromRuns(node, lineGlyphs(node)); }

std::vector<Painter::Mark> Painter::marksFromRuns(const Node& node, const LineGlyphs& glyphs) {
    std::vector<Mark> marks;
    std::int32_t pen = 0;
    const int lineSize = node.fontSize > 0 ? node.fontSize : 1000;
    const auto textOf = [&](const LineRun& run) {
        return std::string_view(node.text().data() + run.textBegin, run.textEnd - run.textBegin);
    };
    // 그리기 모양은 글자 모양에서 읽는다. 글자 모양이 없으면 비어 있어 줄 값을 쓴다.
    const auto paintOf = [&](const LineRun& run) {
        return doc ? detail::charPaint(*doc, run.charShapeId) : detail::CharPaint{};
    };
    const auto styled = [&](Mark& mark, const LineRun& run, const detail::CharPaint& paint) {
        mark.size = run.style->fontSize > 0 ? run.style->fontSize : lineSize;
        mark.ratio = run.style->ratio > 0 ? run.style->ratio : 100;
        mark.relSize = run.style->relSize > 0 ? run.style->relSize : 100;
        mark.charOffset = run.style->charOffset;
        mark.charShapeId = run.charShapeId;
        mark.bold = paint.bold;
        mark.italic = paint.italic;
        mark.superscript = paint.superscript;
        mark.subscript = paint.subscript;
        mark.font = run.style->font ? *run.style->font : node.font.str();
        mark.color = paint.color;
        mark.background = paint.fillColor;
        mark.hatchStyle = paint.hatchStyle;
        mark.hatchColor = paint.hatchColor;
        mark.shade = paint.shade;
        mark.markpen = run.style->markpen ? *run.style->markpen : std::string{};
        mark.track = run.style->track;
        // 한글 변경 내용(최종본 및 변경 내용): 바뀐 글은 빨간 글자다.
        if (mark.track != 0) mark.color = "#FF0000";
        mark.synthetic = run.synthetic;
    };
    for (std::size_t runIndex = 0; runIndex < glyphs.runs.size(); ++runIndex) {
        const LineRun& run = glyphs.runs[runIndex];
        const std::string_view text = textOf(run);
        if (run.kind == RunKind::Note) {
            // 번호와 뒤 장식은 한 칸이다. 1 과 ) 사이에는 커서가 서지 않는다.
            Mark mark;
            mark.run = static_cast<int>(runIndex);
            mark.x = pen;
            mark.noteText = std::string(text);
            std::int32_t width = run.width;
            if (run.advanceCount > 0) {
                const auto first = glyphs.advances.begin() + run.advanceAt;
                mark.noteAdvances.assign(first, first + run.advanceCount);
                const auto ink = glyphs.inks.begin() + run.inkAt;
                mark.noteInks.assign(ink, ink + run.inkCount);
                width = 0;
                for (const std::int32_t advance : mark.noteAdvances) width += advance;
            } else if (!text.empty() && doc) {
                // 번호는 늘 그릴 때 잰다. 폭이 따로 없을 때만 잰 폭을 칸 폭으로 쓴다.
                measureText(doc, text, run.charShapeId, run.style->fontSize > 0 ? run.style->fontSize : 1000,
                            run.style->ratio > 0 ? run.style->ratio : 100, mark.noteAdvances, mark.noteInks);
                if (run.width == 0 && !mark.noteAdvances.empty()) {
                    width = 0;
                    for (const std::int32_t advance : mark.noteAdvances) width += advance;
                }
            }
            mark.width = width;
            mark.band = width;
            styled(mark, run, paintOf(run));
            mark.synthetic = false;
            std::size_t index = 0;
            if (!nextCp(text, index, mark.cp)) mark.cp = 0;
            marks.push_back(std::move(mark));
            pen += width;
        } else if (run.kind == RunKind::Object) {
            const Node& slot = node.children[run.slot];
            // 저장된 줄은 탭 뒤를 펜으로 잇는다. 왼쪽 캡션은 그 펜보다 개체 x가 오른쪽이라, 그 자리를 쓴다.
            bool hasLeftCaption = false;
            for (const Node& sub : slot.children) {
                if (sub.role == "caption" && sub.x < 0) {
                    hasLeftCaption = true;
                    break;
                }
            }
            if (slot.treatAsChar && hasLeftCaption && slot.x > pen) pen = slot.x;
            Mark mark;
            mark.x = slot.treatAsChar ? pen : slot.x;
            mark.width = slot.width + slot.marginLeft + slot.marginRight;
            mark.size = slot.fontSize > 0 ? slot.fontSize : lineSize;
            mark.charShapeId = slot.charShapeId;
            mark.font = slot.font.empty() ? node.font : slot.font;
            mark.object = &slot;
            mark.slot = static_cast<int>(run.slot);
            mark.run = static_cast<int>(runIndex);
            marks.push_back(mark);
            pen = mark.x + mark.width;
        } else if (run.kind == RunKind::Tab) {
            Mark mark;
            mark.run = static_cast<int>(runIndex);
            mark.cp = '\t';
            mark.x = pen;
            std::int32_t width = run.width;
            std::string leader = run.style->tabLeader ? *run.style->tabLeader : std::string{};
            if (node.lineData().deferTabs) {
                const TabGap gap = tabGapAtRun(node, glyphs, pen, runIndex, doc);
                width = gap.width;
                leader = gap.leader;
            }
            const detail::CharPaint paint = paintOf(run);
            mark.width = width;
            mark.size = run.style->fontSize > 0 ? run.style->fontSize : lineSize;
            mark.charShapeId = run.charShapeId;
            mark.font = run.style->font ? *run.style->font : node.font.str();
            mark.color = paint.color;
            mark.leader = std::move(leader);
            mark.synthetic = run.synthetic;
            marks.push_back(std::move(mark));
            pen += width;
        } else {
            const detail::CharPaint paint = paintOf(run);
            const std::int32_t* advances = run.advanceCount > 0 ? glyphs.advances.data() + run.advanceAt : nullptr;
            std::uint32_t advanceCount = run.advanceCount;
            const std::int32_t* inks = run.inkCount > 0 ? glyphs.inks.data() + run.inkAt : nullptr;
            std::uint32_t inkCount = run.inkCount;
            // 줄 캐시로 가른 줄은 글자 폭을 비워 둔다. 그릴 때 재고, 저장된 줄만 자동 간격을 여기서 더한다.
            std::vector<std::int32_t> measuredAdvances;
            std::vector<std::int32_t> measuredInks;
            bool gapHere = false;
            if (advanceCount == 0 && run.width == 0 && !text.empty() && doc) {
                measureText(doc, text, run.charShapeId, run.style->fontSize > 0 ? run.style->fontSize : 1000,
                            run.style->ratio > 0 ? run.style->ratio : 100, measuredAdvances, measuredInks);
                advances = measuredAdvances.data();
                advanceCount = static_cast<std::uint32_t>(measuredAdvances.size());
                inks = measuredInks.data();
                inkCount = static_cast<std::uint32_t>(measuredInks.size());
                gapHere = node.lineData().autoSpacing != 0;
            }
            std::size_t index = 0;
            if (advanceCount > 0) {
                for (std::uint32_t glyph = 0; glyph < advanceCount; ++glyph) {
                    const std::int32_t width = advances[glyph];
                    Mark mark;
                    mark.run = static_cast<int>(runIndex);
                    if (!nextCp(text, index, mark.cp)) mark.cp = 0;
                    styled(mark, run, paint);
                    if (gapHere && !marks.empty() && marks.back().cp != ' ' && marks.back().cp != '\t') {
                        const std::int32_t gap = detail::autoSpacingGap(marks.back().cp, mark.cp, node.lineData().autoSpacing,
                                                                        marks.back().size > 0 ? marks.back().size
                                                                                              : mark.size);
                        marks.back().width += gap;
                        pen += gap;
                    }
                    mark.x = pen;
                    mark.width = width;
                    mark.band = glyph < inkCount ? inks[glyph] : width;
                    // 일반 공백만 양쪽 정렬로 늘린다. 고정폭 빈칸은 그대로 둔다.
                    mark.adjustable = mark.cp == ' ';
                    marks.push_back(std::move(mark));
                    pen += width;
                }
            } else if (char32_t single = 0; nextCp(text, index, single) && index == text.size()) {
                // 띄어쓰기와 고정폭 빈칸은 한 글자짜리 구간이고 폭만 있다. 한 칸으로 세어야 커서가 모델과 맞는다.
                Mark mark;
                mark.run = static_cast<int>(runIndex);
                mark.cp = single;
                mark.x = pen;
                mark.width = run.width;
                mark.band = inkCount == 0 ? run.width : inks[0];
                styled(mark, run, paint);
                mark.adjustable = single == ' ';
                marks.push_back(std::move(mark));
                pen += run.width;
            } else {
                pen += run.width;
            }
        }
    }
    if (node.lineData().vertical != 0) applyVertical(marks, node.lineData().vertical);
    if (node.lineData().charGrid > 0) applyCharGrid(marks, node.lineData().charGrid, node.lineData().wongoji);
    return marks;
}

void Painter::applyVertical(std::vector<Mark>& marks, int direction) const {
    std::int32_t pen = marks.empty() ? 0 : marks.front().x;
    for (Mark& mark : marks) {
        if (mark.object && !mark.object->treatAsChar) {
            pen = mark.x + mark.width;
            continue;
        }
        mark.x = pen;
        if (!mark.object && mark.noteText.empty() && mark.cp != 0 && mark.cp != '\t') {
            // 줄을 나눌 때와 같은 칸(verticalCell)이다. 자간은 그 칸 폭으로 다시 잰다.
            std::int32_t spacing = 0;
            if (mark.width != mark.band && doc) {
                if (const model::CharShape* shape = findCharShape(*doc, mark.charShapeId)) {
                    const std::size_t slot =
                        static_cast<std::size_t>(detail::isHangul(mark.cp) ? model::Lang::Hangul : model::Lang::Latin);
                    if (slot < shape->spacing.size()) spacing = shape->spacing[slot];
                }
            }
            const detail::VerticalCell cell = detail::verticalCell(mark.cp, direction, mark.band, mark.size, mark.relSize,
                                                                   mark.superscript || mark.subscript, spacing);
            mark.width = cell.ink + cell.trail;
            if (cell.upright) {
                // 바꿔 그리는 。、﹃ 따위는 1em 글자라 칸을 그대로 쓴다. 그 밖의 글자는 가로 폭을 칸 가운데에 둔다.
                const bool swapped = detail::verticalForm(mark.cp) != mark.cp;
                mark.lead = swapped ? 0 : std::max<std::int32_t>(0, (cell.ink - mark.band) / 2);
                mark.band = cell.ink;
                mark.upright = true;
            }
        }
        pen += mark.width;
    }
}

// 줄을 나눌 때와 같은 charGridCells 로 글자마다 칸 자리를 잡는다. 줄 앞에서 짝을 새로 센다.
void Painter::applyCharGrid(std::vector<Mark>& marks, std::int32_t cell, bool wongoji) {
    std::vector<char32_t> cps(marks.size(), 0);
    std::vector<std::int32_t> widths(marks.size(), 0);
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const Mark& mark = marks[i];
        widths[i] = mark.width;
        if (!mark.object && mark.noteText.empty() && mark.cp != '\t' && mark.cp != 0) cps[i] = mark.cp;
    }
    const std::vector<detail::GridCell> cells = detail::charGridCells(cps, widths, cell, wongoji);
    std::int32_t pen = marks.empty() ? 0 : marks.front().x;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        Mark& mark = marks[i];
        if (mark.object && !mark.object->treatAsChar) {
            pen = mark.x + mark.width;
            continue;
        }
        mark.x = pen;
        if (cps[i] != 0) {
            mark.width = cells[i].width;
            mark.lead = cells[i].lead;
            mark.band = mark.width;
            // 칸에 맞춘 줄은 양쪽 정렬로 늘리거나 줄이지 않는다.
            mark.adjustable = false;
        }
        pen += mark.width;
    }
}

std::vector<Painter::Mark> Painter::caretMarks(const Node& node) {
    std::vector<Mark> marks = collectMarks(node);
    marks.erase(std::remove_if(marks.begin(), marks.end(), [](const Mark& mark) { return mark.synthetic; }),
                marks.end());
    return marks;
}

int Painter::relativeDrawSize(int size, int rel, bool superOrSub) {
    if (size <= 0) return size;
    int out = (rel <= 0 || rel == 100)
                  ? size
                  : static_cast<int>((static_cast<std::int64_t>(size) * rel + 50) / 100);
    if (superOrSub) {
        out = static_cast<int>((static_cast<std::int64_t>(out) * 16 + 12) / 25);
    }
    return out;
}

std::int32_t Painter::scriptBaselineShift(int size, bool super, bool sub, const std::string& vertAlign,
                                          bool firstByteZero) {
    if (size <= 0 || (!super && !sub)) return 0;
    int pct = 0;
    if (vertAlign == "TOP") {
        pct = super ? -13 : 42;
    } else if (vertAlign == "CENTER") {
        pct = super ? -32 : 24;
    } else if (vertAlign == "BOTTOM") {
        pct = super ? -49 : 6;
    } else if (firstByteZero) {
        pct = super ? -44 : 12;
    } else {
        pct = super ? -32 : 24;
    }
    const std::int64_t numer = static_cast<std::int64_t>(size) * std::abs(pct) + 50;
    const std::int32_t mag = static_cast<std::int32_t>(numer / 100);
    return pct < 0 ? -mag : mag;
}

std::string Painter::hexOf(model::Color color, const std::string& fallback) {
    if (!color) return fallback;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(*color & 0xFFFFFFu));
    return buf;
}

const model::BorderFill* Painter::borderFillOf(std::uint32_t id) const {
    if (!doc || id == 0) return nullptr;
    const auto& fills = doc->header.borderFills;
    if (id - 1 < fills.size() && fills[id - 1].id == id) return &fills[id - 1];
    if (id < fills.size() && fills[id].id == id) return &fills[id];
    for (const auto& fill : fills) {
        if (fill.id == id) return &fill;
    }
    return nullptr;
}

void Painter::paintRule(std::int32_t huX, std::int32_t huW, std::int32_t huY, const std::string& type,
                        const std::string& color, std::int32_t thickHu) {
    if (placesOnly || huW <= 0) return;
    BorderEdge edge;
    edge.type = type.empty() ? "SOLID" : type;
    if (edge.type == "NONE") return;
    edge.color = color.empty() ? "#000000" : color;
    edge.width = thickHu > 0 ? thickHu : 33;
    const int t = std::max(1, px(edge.width));
    const int x0 = px(huX);
    const int x1 = px(huX + huW);
    strokeEdge(edge, x0, px(huY) - t / 2, std::max(1, x1 - x0), t);
}

void Painter::paintEmphasisMark(int kind, int cx, int cy, int rad, const std::string& color) {
    if (placesOnly || kind <= 0 || rad <= 0) return;
    std::uint8_t r, g, b;
    parseHex(color.empty() ? "#000000" : color, r, g, b);
    auto disk = [&](int x, int y, int radius, bool ring) {
        if (radius <= 0) return;
        const int outer = radius * radius;
        const int innerRad = ring ? std::max(0, radius - std::max(1, radius / 3)) : 0;
        const int inner = innerRad * innerRad;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int d2 = dx * dx + dy * dy;
                if (d2 > outer || (ring && d2 < inner)) continue;
                fillRect(bitmap, x + dx, y + dy, 1, 1, r, g, b);
            }
        }
    };
    if (kind == 2) disk(cx, cy, rad, true);
    else if (kind == 5) disk(cx, cy, std::max(1, rad / 2), false);
    else if (kind == 6) {
        const int dot = std::max(1, rad / 3);
        disk(cx, cy - rad / 2, dot, false);
        disk(cx, cy + rad / 2, dot, false);
    } else if (kind == 3) {
        for (int i = 0; i <= rad; ++i) {
            fillRect(bitmap, cx - rad + i, cy - rad / 2 + i / 2, 2, 2, r, g, b);
            fillRect(bitmap, cx + rad - i, cy - rad / 2 + i / 2, 2, 2, r, g, b);
        }
    } else if (kind == 4) {
        const int thick = std::max(1, rad / 5);
        fillRect(bitmap, cx - rad, cy, rad, thick, r, g, b);
        fillRect(bitmap, cx, cy - rad / 3, rad, thick, r, g, b);
    } else {
        disk(cx, cy, rad, false);
    }
}

void Painter::paintCharBorderBox(const model::BorderFill& fill, std::int32_t x, std::int32_t y, std::int32_t w,
                                 std::int32_t h) {
    if (placesOnly || w <= 0 || h <= 0) return;
    auto edgeOf = [](BorderEdge& edge, const model::BorderSide& side) {
        if (side.type.empty() || side.type == "NONE") return;
        edge.type = side.type;
        edge.color = side.color.empty() ? "#000000" : side.color;
        edge.width = side.width;
    };
    BorderEdge left, right, top, bottom;
    edgeOf(left, fill.left);
    edgeOf(right, fill.right);
    edgeOf(top, fill.top);
    edgeOf(bottom, fill.bottom);
    const int x0 = px(x);
    const int y0 = px(y);
    const int x1 = px(x + w);
    const int y1 = px(y + h);
    const int tw = std::max(0, x1 - x0);
    const int th = std::max(0, y1 - y0);
    if (const int t = edgeThickPx(top)) strokeEdge(top, x0 - t / 2, y0 - t / 2, tw + t, t);
    if (const int t = edgeThickPx(bottom)) strokeEdge(bottom, x0 - t / 2, y1 - t / 2, tw + t, t);
    if (const int t = edgeThickPx(left)) strokeEdge(left, x0 - t / 2, y0 - t / 2, t, th + t);
    if (const int t = edgeThickPx(right)) strokeEdge(right, x1 - t / 2, y0 - t / 2, t, th + t);
}

void Painter::paintGlyphEffects(const model::CharShape& cs, const Mark& mark, std::int32_t at,
                                std::int32_t glyphBaseline, int drawSize, int story, int paragraph, int offset) {
    if (mark.cp == 0 || mark.cp == ' ' || mark.cp == '\t' || mark.cp == '\n') return;
    const std::string color = mark.color.empty() ? "#000000" : mark.color;
    auto drawAt = [&](std::int32_t dx, std::int32_t dy, const std::string& col) {
        glyph(mark.cp, mark.font, drawSize, mark.ratio, mark.bold, mark.italic, col, at + dx, glyphBaseline + dy,
              story, paragraph, offset);
    };
    if (cs.shadow != 0) {
        const std::int32_t dx = mark.size * static_cast<int>(cs.shadowOffsetX) / 100;
        const std::int32_t dy = mark.size * static_cast<int>(cs.shadowOffsetY) / 100;
        const std::string shadow = hexOf(cs.shadowColor, "#B2B2B2");
        if (cs.shadow == 2) {
            for (int step = 1; step <= 4; ++step) drawAt(dx * step / 4, dy * step / 4, shadow);
        } else {
            drawAt(dx, dy, shadow);
        }
    }
    const std::int32_t lift = std::max<std::int32_t>(mark.size / 20, 10);
    if (cs.emboss) {
        drawAt(-lift, -lift, "#FFFFFF");
        drawAt(lift, lift, "#808080");
    }
    if (cs.engrave) {
        drawAt(-lift, -lift, "#808080");
        drawAt(lift, lift, "#FFFFFF");
    }
    if (cs.outline >= 1 && cs.outline <= 6) {
        const int rad = cs.outline == 3 ? std::max(mark.size / 10, 20) : std::max(mark.size / 18, 12);
        static const int kDir[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
        for (int i = 0; i < 8; ++i) {
            if (cs.outline == 2 && (i % 2) == 0) continue;
            if (cs.outline == 4 && (i % 2) == 1) continue;
            drawAt(kDir[i][0] * rad, kDir[i][1] * rad, color);
        }
    }
}

void Painter::paintBitmapDeco(const model::CharShape& shape, const Mark& mark, std::int32_t at, std::int32_t lineTop,
                              int lineBox, std::int32_t glyphBaseline, int drawSize) {
    if (placesOnly) return;
    // 변경 추적: 넣은 글은 빨간 실선 밑줄, 지운 글은 빨간 실선 취소선을 더한다(한글 화면).
    model::CharShape tracked;
    if (mark.track != 0) {
        tracked = shape;
        if (mark.track == model::kTrackInsert) {
            tracked.underline = "BOTTOM";
            tracked.underlineShape = "SOLID";
            tracked.underlineShapeId = 0;
            tracked.underlineColor = 0xFF0000u;
        } else {
            tracked.strikeout = true;
            tracked.strikeShape = "SOLID";
            tracked.strikeShapeId = 0;
            tracked.strikeColor = 0xFF0000u;
        }
    }
    const model::CharShape& cs = mark.track != 0 ? tracked : shape;
    const std::int32_t span = mark.width > 0 ? mark.width : mark.band;
    const std::string color = mark.color.empty() ? "#000000" : mark.color;
    int position = 0;
    // 밑줄 위치 2는 쪽 본문에서 그리지 않는다. 1은 글자 상자 아래, 3은 위다.
    if (cs.underline == "BOTTOM") position = 1;
    else if (cs.underline == "TOP") position = 3;
    const std::int32_t anchor = lineTop + mark.size * mark.charOffset / 100;
    if (position != 0 && span > 0 && decorationLineVisible(cs.underlineShapeId)) {
        paintRule(at, span, anchor + decorationLineY(lineBox, position, cs.underlineShapeId),
                  decorationLineType(cs.underlineShapeId, cs.underlineShape.c_str()),
                  hexOf(cs.underlineColor, color), decorationStrokeHu(cs.underlineShapeId));
    }
    if (cs.strikeout && span > 0 && decorationLineVisible(cs.strikeShapeId)) {
        paintRule(at, span, glyphBaseline + strikeLineY(mark.size),
                  decorationLineType(cs.strikeShapeId, cs.strikeShape.c_str()), hexOf(cs.strikeColor, color),
                  decorationStrokeHu(cs.strikeShapeId));
    }
    if (cs.emphasis != 0 && mark.cp != 0 && mark.cp != ' ' && mark.cp != '\t') {
        const int rad = std::max(2, px(std::max(mark.size / 10, 40)));
        const std::int32_t band = mark.band > 0 ? mark.band : span;
        const std::int32_t glyphTop = glyphBaseline - chwp::layout::glyphBaseline(drawSize);
        paintEmphasisMark(cs.emphasis, px(at + band / 2), px(glyphTop) - rad - 1, rad, color);
    }
    if (const model::BorderFill* fill = borderFillOf(cs.borderFillId)) {
        const std::int32_t band = mark.band > 0 ? mark.band : span;
        if (band > 0 && drawSize > 0) {
            paintCharBorderBox(*fill, at, glyphBaseline - chwp::layout::glyphBaseline(drawSize), band, drawSize);
        }
    }
}

void Painter::paintParagraphChrome(const Node& node, std::int32_t ox, std::int32_t oy, int join, bool fill, bool stroke) {
    if (placesOnly) return;
    const bool fillPass = fill && (pass == PaintLayer::All || pass == PaintLayer::Behind);
    const bool strokePass = stroke && show(frameStroke());
    if (!fillPass && !strokePass) return;
    auto inked = [](const BorderEdge& edge) { return !edge.type.empty() && edge.type != "NONE"; };
    const bool anySide =
        inked(node.ext().borderLeft) || inked(node.ext().borderRight) || inked(node.ext().borderTop) || inked(node.ext().borderBottom);
    const bool face = !node.ext().fillColor.empty() && node.ext().fillColor != "none";
    const bool hatch = !node.ext().hatchStyle.empty();
    if (!anySide && !face && !hatch) return;
    std::int32_t left = 0, top = 0, right = 0, bottom = 0;
    paragraphBorderBox(node, ox, oy, left, top, right, bottom);
    if (right <= left || bottom <= top) return;
    const int x0 = px(left);
    const int y0 = px(top);
    const int x1 = px(right);
    const int y1 = px(bottom);
    const int tw = std::max(0, x1 - x0);
    const int th = std::max(0, y1 - y0);
    if (fillPass && face && tw > 0 && th > 0) {
        std::uint8_t r, g, b;
        parseHex(node.ext().fillColor, r, g, b);
        fillRect(bitmap, x0, y0, tw, th, r, g, b);
    }
    if (fillPass && hatch) hatchRect(x0, y0, tw, th, node.ext().hatchStyle, node.ext().hatchColor);
    if (!strokePass) return;
    if (!(join & 1)) {
        if (const int t = edgeThickPx(node.ext().borderTop))
            strokeEdge(node.ext().borderTop, x0 - t / 2, y0 - t / 2, tw + t, t);
    }
    if (!(join & 2)) {
        if (const int t = edgeThickPx(node.ext().borderBottom))
            strokeEdge(node.ext().borderBottom, x0 - t / 2, y1 - t / 2, tw + t, t);
    }
    if (const int t = edgeThickPx(node.ext().borderLeft)) strokeEdge(node.ext().borderLeft, x0 - t / 2, y0 - t / 2, t, th + t);
    if (const int t = edgeThickPx(node.ext().borderRight))
        strokeEdge(node.ext().borderRight, x1 - t / 2, y0 - t / 2, t, th + t);
}

void Painter::drawHeading(const Node& line, std::int32_t x, std::int32_t y, std::int32_t shift, std::int32_t slide) {
    for (const auto& slot : line.children) {
        // 글머리 그림은 글자처럼 넣은 개체와 달리 따로 칸이 없다. 줄 칸에 그린다.
        if (!slot.headingMark || slot.kind != NodeKind::Object || placesOnly) continue;
        objectBox(slot, x + slide + slot.x + shift, y + slot.y);
    }
    const HeadingRun* head = lineGlyphs(line).heading.get();
    if (!head) return;
    const std::int32_t at = x + slide + head->x + shift;
    const int size = head->fontSize > 0 ? head->fontSize : 1000;
    const int box = line.lineData().textHeight > 0 ? line.lineData().textHeight : size;
    const std::string mode = line.vertAlign.empty() ? "BASELINE" : line.vertAlign;
    int baseline = line.lineData().lineBaseline >= 0 ? line.lineData().lineBaseline : chwp::layout::glyphBaseline(box);
    baseline += scriptBaselineShift(size, head->superscript, head->subscript, mode, head->text.empty());
    std::int32_t pen = at;
    std::size_t index = 0;
    std::size_t nth = 0;
    char32_t cp = 0;
    while (nextCp(head->text, index, cp)) {
        const std::int32_t advance = nth < head->advances.size() ? head->advances[nth] : size;
        ++nth;
        glyph(cp, head->font, relativeDrawSize(size, head->relSize, head->superscript || head->subscript),
              head->ratio > 0 ? head->ratio : 100, head->bold, head->italic,
              head->color.empty() ? "#000000" : head->color, pen, y + line.marginTop + baseline);
        pen += advance;
    }
}

void Painter::line(const Node& node, std::int32_t x, std::int32_t y, bool inObject) {
    // 덧말·글자 겹치기 묶음 안 줄(glyph_group.cpp). 글자 줄은 글자·효과·강조점만, 장식 줄은 밑줄·취소선·음영·테두리만 그린다.
    const bool glyphOnly = node.role == "glyph" || node.role == "reverse";
    const bool decoOnly = node.role == "glyphDeco";
    std::vector<Mark> marks = collectMarks(node);
    const LinePlacement place = placeLine(node, marks);
    if (!placesOnly && node.lineData().auditLine && (pass == PaintLayer::All || pass == PaintLayer::Text)) {
        reportLineSeg(node, x + originX, y + originY, inObject);
    }

    drawHeading(node, x, y, place.fit.shift, place.fit.slide);
    drawProofBackgrounds(node, x, y, place);
    drawMemoRanges(node, x, y, place);

    // 글자 자리 번호는 커서 칸이다. 자동 번호 대신 넣은 글자는 칸을 세지 않고, 번호 없이(-1) 그린다.
    int caretIndex = 0;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const Mark& mark = marks[i];
        const int slotIndex = caretIndex;
        if (!mark.synthetic) ++caretIndex;
        const std::int32_t at = x + place.at[i];
        if (mark.object && !skipInlineObjects && !placesOnly) {
            // 글자처럼 넣은 개체의 바깥 여백은 테두리 밖에 있다. 왼쪽·위 여백만큼 테두리를 민다.
            const std::int32_t ox = at + mark.object->marginLeft;
            const std::int32_t oy = y + mark.object->y + mark.object->marginTop;
            reportObject(*mark.object, ox + originX, oy + originY);
            objectBox(*mark.object, ox, oy);
            const CoverScope cover(*this, coversText(*mark.object));
            const bool clipBox = clipsOwnBox(*mark.object);
            if (clipBox) {
                const std::int32_t bleed = tableBorderBleed(*mark.object);
                pushClip(ox - bleed, oy - bleed, mark.object->width + bleed * 2,
                         mark.object->height + bleed * 2);
            }
            // 글자처럼 취급한 묶음도 자식은 zOrder가 큰 쪽이 위다.
            // 이 개체의 문단 칸이 블록에 있으면 안쪽 글자도 그 선택으로 뒤집는다.
            const ObjectHostScope host(*this, node.story, node.paragraph, node.start + slotIndex);
            paintStackedChildren(*mark.object, ox, oy, true);
            if (mark.object->role == "table") paintTableBorders(*mark.object, ox, oy);
            if (clipBox) popClip();
            // 캡션은 개체 밖이다. 개체 상자 클립을 벗긴 뒤에 그린다.
            paintCaptions(*mark.object, ox, oy, true);
        }
        else if (showGlyphs() || placesOnly) {
            // 세로 자리는 캐럿과 같은 glyphBaseIn 이다. marginTop은 개체가 글자 위로 나간 만큼이다.
            // 줄 간격은 상자 아래에 남는다.
            const GlyphBase base = glyphBaseIn(node, mark.size, mark.font, mark.charOffset);
            const std::int32_t baseline = base.baseline;
            const std::int32_t ownBase = base.ownBase;
            const std::string mode = node.vertAlign.empty() ? "BASELINE" : node.vertAlign;
            // 글자 배경과 형광펜은 장평까지 반영한 글자 상자만 칠한다. 자간과 줄 간격은 넣지 않는다.
            const auto fillBand = [&](const std::string& color) {
                if (placesOnly || color.empty() || color == "none" || mark.band <= 0 || mark.size <= 0) return;
                std::uint8_t br, bg, bb;
                parseHex(color, br, bg, bb);
                const std::int32_t top = y + node.marginTop + baseline - ownBase;
                const int x0 = px(at);
                const int y0 = px(top);
                const int x1 = px(at + mark.band);
                const int y1 = px(top + mark.size);
                if (x1 > x0 && y1 > y0) fillRect(bitmap, x0, y0, x1 - x0, y1 - y0, br, bg, bb);
            };
            if (!glyphOnly) fillBand(mark.background);
            if (!glyphOnly && !placesOnly && !mark.hatchStyle.empty() && mark.band > 0 && mark.size > 0) {
                const std::int32_t top = y + node.marginTop + baseline - ownBase;
                const int x0 = px(at);
                const int y0 = px(top);
                hatchRect(x0, y0, px(at + mark.band) - x0, px(top + mark.size) - y0, mark.hatchStyle, mark.hatchColor);
            }
            if (!glyphOnly) fillBand(mark.shade);
            if (!glyphOnly) fillBand(mark.markpen);
            if (mark.cp == '\t' && !placesOnly) {
                const std::string leader = canonicalLeader(mark.leader);
                const int slotHu = std::max(figureSlot(mark.size), 4);
                if (!leader.empty() && leader != "NONE" && mark.width > slotHu * 2) {
                    // 기준선은 글자 상자 위에서 85%다. 채움 선은 그 상자의 세로 중앙에 둔다.
                    const std::int32_t midY = y + node.marginTop + baseline - mark.size * 35 / 100;
                    std::uint8_t lr, lg, lb;
                    parseHex(mark.color.empty() ? "#000000" : mark.color, lr, lg, lb);
                    const int slotPx = std::max(1, px(slotHu));
                    const int step = std::max(1, slotPx / 3);
                    if (leader == "DOT") {
                        const int cy = px(midY) - step / 2;
                        for (std::int32_t hu = slotHu; hu + slotHu * 2 <= mark.width; hu += slotHu) {
                            fillRect(bitmap, px(at + hu), cy, step, step, lr, lg, lb);
                        }
                    } else {
                        const int x0 = px(at + slotHu);
                        const int x1 = px(at + mark.width - slotHu);
                        if (x1 > x0) fillLeader(bitmap, leader, x0, px(midY), x1 - x0, slotPx, lr, lg, lb);
                    }
                }
                if (const model::CharShape* deco = doc ? findCharShape(*doc, mark.charShapeId) : nullptr) {
                    const int lineBox = node.lineData().textHeight > 0 ? node.lineData().textHeight : mark.size;
                    const std::int32_t script =
                        scriptBaselineShift(mark.size, mark.superscript, mark.subscript, mode, mark.cp == 0);
                    paintBitmapDeco(*deco, mark, at, y + node.marginTop, lineBox,
                                    y + node.marginTop + baseline + script, mark.size);
                }
            } else {
                const bool note = !mark.noteText.empty();
                const bool noteMeasured = note && doc;
                std::size_t textIndex = 0;
                std::size_t nth = 0;
                char32_t cp = mark.cp;
                std::int32_t pen = at + mark.lead;
                while (true) {
                    if (note && !nextCp(mark.noteText, textIndex, cp)) break;
                    Mark glyphMark = mark;
                    glyphMark.noteText.clear();
                    glyphMark.noteAdvances.clear();
                    glyphMark.noteInks.clear();
                    glyphMark.cp = cp;
                    if (noteMeasured) {
                        const std::int32_t width =
                            nth < mark.noteAdvances.size() ? mark.noteAdvances[nth] : glyphMark.size;
                        glyphMark.width = width;
                        glyphMark.band = nth < mark.noteInks.size() ? mark.noteInks[nth] : width;
                    }
                    const std::int32_t drawShift =
                        scriptBaselineShift(glyphMark.size, glyphMark.superscript, glyphMark.subscript, mode,
                                            glyphMark.cp == 0);
                    const int drawSize = relativeDrawSize(glyphMark.size, glyphMark.relSize,
                                                          glyphMark.superscript || glyphMark.subscript);
                    const std::int32_t glyphBase = y + node.marginTop + baseline + drawShift;
                    const model::CharShape* deco = doc ? findCharShape(*doc, glyphMark.charShapeId) : nullptr;
                    std::string glyphColor = glyphMark.color.empty() ? "#000000" : glyphMark.color;
                    // 반전 모양 글자 겹치기(●■▲◆)의 안 글자는 흰색이다.
                    if (node.role == "reverse") glyphColor = "#FFFFFF";
                    if (deco && deco->outline != 0) {
                        if (!glyphMark.shade.empty() && glyphMark.shade != "none") glyphColor = glyphMark.shade;
                        else if (!glyphMark.background.empty() && glyphMark.background != "none")
                            glyphColor = glyphMark.background;
                        else glyphColor = "#FFFFFF";
                    }
                    const int slot = mark.synthetic ? -1 : node.start + slotIndex;
                    // 세로쓰기에서 세우는 글자는 칸 가운데를 축으로 돌린다. 。、 는 칸 오른쪽 위로 반 em 민다.
                    if (glyphMark.upright) {
                        glyphMark.cp = detail::verticalForm(glyphMark.cp);
                        const auto pxAt = [&](std::int32_t hu) {
                            return framePadHu > 0 ? glyphPx(hu) : static_cast<float>(px(hu));
                        };
                        const std::int32_t boxTop = glyphBase - chwp::layout::glyphBaseline(drawSize);
                        glyphTurn.on = true;
                        glyphTurn.cx = pxAt(at + mark.band / 2);
                        glyphTurn.cy = pxAt(boxTop + drawSize / 2);
                        const bool corner = glyphMark.cp == 0x3001 || glyphMark.cp == 0x3002;
                        glyphTurn.dx = corner ? -static_cast<float>(drawSize) * scale / 2.f : 0.f;
                        glyphTurn.dy = glyphTurn.dx;
                    }
                    if (deco && !decoOnly) {
                        paintGlyphEffects(*deco, glyphMark, pen, glyphBase, drawSize, node.story, node.paragraph,
                                          slot);
                    }
                    glyph(glyphMark.cp, glyphMark.font, drawSize, glyphMark.ratio, glyphMark.bold,
                          glyphMark.italic, glyphColor, pen, glyphBase, node.story, node.paragraph, slot);
                    glyphTurn.on = false;
                    if (deco) {
                        const int lineBox = node.lineData().textHeight > 0 ? node.lineData().textHeight : glyphMark.size;
                        if (glyphOnly || decoOnly) {
                            model::CharShape part = *deco;
                            if (glyphOnly) {
                                part.underline = "NONE";
                                part.strikeout = false;
                                part.borderFillId = 0;
                            } else {
                                part.emphasis = 0;
                            }
                            paintBitmapDeco(part, glyphMark, pen, y + node.marginTop, lineBox, glyphBase, drawSize);
                        } else {
                            paintBitmapDeco(*deco, glyphMark, pen, y + node.marginTop, lineBox, glyphBase, drawSize);
                        }
                    }
                    if (!note) break;
                    pen += glyphMark.width;
                    ++nth;
                }
            }
        }
    }
    drawLineMarks(node, x, y, marks, place);
    drawProofMarks(node, x, y, marks, place);
}

void Painter::drawLineMarks(const Node& node, std::int32_t x, std::int32_t y, const std::vector<Mark>& marks,
                            const LinePlacement& place) {
    // 화면 글자는 자리만 남겼다가(placesOnly·placeSink) 다시 그린다. 부호 글자도 같은 길로 남긴다.
    if (!(showGlyphs() || placesOnly) || node.role == "glyph" || node.role == "reverse" || node.role == "glyphDeco") return;
    const ViewMarks shown = shownMarks();
    if (!shown.paragraphMarks()) return;
    // 부호 글자는 함초롬바탕(늘 들고 있는 글꼴)의 ↵ ↓ ⇥ ∨ 다. 크기는 본문 글자의 60%다.
    // 색은 한글 화면에서 잰 파란색(문단 부호·빈칸·탭)과 주황색(이름표)이다.
    static const std::string kFace = "함초롬바탕";
    static const std::string kBlue = "#4169E1";
    static const std::string kOrange = "#FF6F00";
    // 글자처럼 넣은 개체만 있는 줄은 글자 기준선이 비어 있다. 그 줄은 lineseg 기준선(개체 높이의 85%)에 둔다.
    bool objectLine = false;
    for (const Node& child : node.children) objectLine = objectLine || (child.kind == NodeKind::Object && child.treatAsChar);
    const LineContent& lineData = node.lineData();
    const std::int32_t storedBase =
        objectLine && lineData.lineBaseline < 0 && lineData.storedSeg.textHeight > 0 ? lineData.storedSeg.baseline : 0;
    const auto baseOf = [&](int size, const std::string& font, int offset) {
        return y + node.marginTop + std::max(glyphBaseIn(node, size, font, offset).baseline, storedBase);
    };
    const auto markSize = [](int size) { return std::max(1, (size > 0 ? size : 1000) * 6 / 10); };
    const auto sign = [&](char32_t cp, std::int32_t at, int size, const std::string& font, int offset) {
        glyph(cp, kFace, markSize(size), 100, false, false, kBlue, at, baseOf(size, font, offset));
    };
    if (shown.control) {
        for (std::size_t i = 0; i < marks.size() && i < place.at.size(); ++i) {
            const Mark& mark = marks[i];
            if (mark.object || mark.synthetic) continue;
            if (mark.cp == U' ' || mark.cp == 0x3000) {
                sign(0x2228, x + place.at[i] + mark.width / 10, mark.size, mark.font, mark.charOffset);
            } else if (mark.cp == U'\t') {
                sign(0x21E5, x + place.at[i], mark.size, mark.font, mark.charOffset);
            }
        }
        for (std::size_t k = 0; k < place.labels.size() && k < place.labelAt.size() && doc; ++k) {
            const LineLabel& label = place.labels[k];
            const model::CharShape* shape = findCharShape(*doc, label.charShapeId);
            const model::CharShape& face = shape ? *shape : detail::fallbackCharShape();
            const int size = face.height > 0 ? face.height : 1000;
            std::int32_t pen = x + place.labelAt[k];
            std::size_t index = 0;
            std::size_t nth = 0;
            char32_t cp = 0;
            while (nextCp(label.text, index, cp)) {
                const detail::GlyphFace drawn = detail::glyphFace(*doc, face, cp, false, true);
                glyph(cp, drawn.font, size, drawn.ratio > 0 ? drawn.ratio : 100, false, false, kOrange, pen,
                      baseOf(size, drawn.font, 0));
                pen += nth < label.advances.size() ? label.advances[nth] : size;
                ++nth;
            }
        }
    }
    // 문단 끝 ↵, 강제 줄 바꿈 ↓. 줄의 마지막 글자(이름표 포함) 바로 뒤다.
    const LineContent& data = lineData;
    if (!data.lastLine && !data.lineBreak) return;
    int size = node.fontSize;
    std::string font = node.font;
    int offset = 0;
    if (!marks.empty()) {
        size = marks.back().size;
        font = marks.back().font;
        offset = marks.back().charOffset;
    }
    sign(data.lineBreak ? 0x2193 : 0x21B5, x + place.end, size, font, offset);
}

void Painter::strokePageBorderNode(const Node& node, std::int32_t x, std::int32_t y) {
    if (node.width <= 0 || node.height <= 0) return;
    strokeTableBorder(node.ext().borderLeft, x, y, x, y + node.height, 1);
    strokeTableBorder(node.ext().borderRight, x + node.width, y, x + node.width, y + node.height, -1);
    strokeTableBorder(node.ext().borderTop, x, y, x + node.width, y, 1);
    strokeTableBorder(node.ext().borderBottom, x, y + node.height, x + node.width, y + node.height, -1);
    auto joinCorner = [&](const BorderEdge& horizontal, const BorderEdge& vertical, std::int32_t hx,
                          std::int32_t hy, int hInside, int vInside) {
        if (tableLineKind(horizontal.type) == 0 || tableLineKind(horizontal.type) != tableLineKind(vertical.type) ||
            borderInkPixels(horizontal) != borderInkPixels(vertical) || horizontal.color != vertical.color) {
            return;
        }
        mitreTableCorner(hx, hy, hInside, vInside, horizontal);
    };
    joinCorner(node.ext().borderTop, node.ext().borderLeft, x, y, 1, 1);
    joinCorner(node.ext().borderTop, node.ext().borderRight, x + node.width, y, 1, -1);
    joinCorner(node.ext().borderBottom, node.ext().borderLeft, x, y + node.height, -1, 1);
    joinCorner(node.ext().borderBottom, node.ext().borderRight, x + node.width, y + node.height, -1, -1);
}

void Painter::paintPiece(const Node& node, int kind, std::int32_t x, std::int32_t y, bool inObject,
                         std::int32_t stripY0, std::int32_t stripY1, std::int32_t stripX0, std::int32_t stripX1) {
    if (node.role == "pageFill") {
        cellChrome(node, x, y, false);
        return;
    }
    if (node.role == "pageBorder") {
        if (show(PaintLayer::Border)) strokePageBorderNode(node, x, y);
        return;
    }
    if (kind == 0) line(node, x, y, inObject);
    else if (kind == 1) objectBox(node, x, y);
    else if (kind >= 100 && kind < 104 && node.kind == NodeKind::Paragraph)
        paintParagraphChrome(node, x, y, kind - 100, true, true);
    else if (kind >= 3 && node.kind == NodeKind::Column) paintColumnRuleSpan(node.ext().borderRight, x, y, stripY1 - stripY0);
    else if (kind >= 3) paintTableBorders(node, x, y, stripY0, stripY1, stripX0, stripX1);
    else {
        cellChrome(node, x, y, false);
    }
}

void Painter::paintPageFill(const Node& paper, std::int32_t ox, std::int32_t oy) {
    if (placesOnly || !show(PaintLayer::Behind)) return;
    for (const Node& child : paper.children) {
        if (child.role != "pageFill") continue;
        cellChrome(child, ox + child.x, oy + child.y, false);
    }
}

void Painter::paintPageBorder(const Node& paper, std::int32_t ox, std::int32_t oy) {
    if (placesOnly || !show(PaintLayer::Border)) return;
    for (const Node& child : paper.children) {
        if (child.role != "pageBorder") continue;
        strokePageBorderNode(child, ox + child.x, oy + child.y);
    }
}

void Painter::walkTurned(const Node& node, std::int32_t ox, std::int32_t oy, bool inObject, int borderJoin) {
    // 돌린 틀의 가로 폭. 쪽 본문은 종이 폭, 글상자·표 칸은 그 상자 폭이다.
    const std::int32_t paperW = node.turnSpan > 0 ? node.turnSpan : (pageNode ? pageNode->width : 0);
    const int bw = bitmap.width;
    const int bh = bitmap.height;
    if (paperW <= 0 || bw <= 0 || bh <= 0 || scale <= 0.f) return;
    ensureBitmap(bitmap);
    // 쪽 비트맵 (bx, by) 는 돌린 틀 비트맵 (by, bw − 1 − bx) 다. 픽셀을 옮기기만 해서 흐려지지 않는다.
    Bitmap turned;
    turned.width = bh;
    turned.height = bw;
    turned.px.resize(bitmap.px.size());
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const std::uint8_t* from = bitmap.px.data() + (static_cast<std::size_t>(by) * bw + bx) * 4u;
            std::uint8_t* to = turned.px.data() + (static_cast<std::size_t>(bw - 1 - bx) * bh + by) * 4u;
            std::memcpy(to, from, 4);
        }
    }
    turned.clipR = turned.width;
    turned.clipB = turned.height;
    std::swap(bitmap, turned);
    // 바깥 자르기는 돌린 비트맵 자리로 옮겨 그대로 건다. 글상자·표 칸의 세로 줄은 그 상자 폭 밖으로 넘치면 잘린다.
    Clip turnedClip{0, 0, bh, bw};
    if (!clips.empty()) turnedClip = {clips.back().t, bw - clips.back().r, clips.back().b, bw - clips.back().l};
    if (node.turnSpan > 0) {
        turnedClip.t = std::max(turnedClip.t, bw - px(ox + paperW));
        turnedClip.b = std::min(turnedClip.b, bw - px(ox));
        if (turnedClip.b < turnedClip.t) turnedClip.b = turnedClip.t;
    }
    std::vector<Clip> outerClips;
    std::swap(clips, outerClips);
    sk_sp<SkSurface> outerSurface = std::move(skiaSurface);
    SkCanvas* outerCanvas = skiaCanvas;
    const int outerSaves = skiaSaves;
    skiaCanvas = nullptr;
    skiaSaves = 0;
    const float outerRight = viewRightHu;
    const float outerBottom = viewBottomHu;
    viewRightHu = static_cast<float>(bh) / scale;
    viewBottomHu = static_cast<float>(bw) / scale;
    // 틀의 (x, y) 는 종이의 (종이 폭 − y, x) 다. 쪽 비트맵 왼쪽 끝이 종이 ox 만큼 밀려 있으면 틀은 위에서 그만큼 잘린다.
    const std::int32_t bitmapHu = static_cast<std::int32_t>(std::lround(static_cast<double>(bw) / scale));
    const std::int32_t turnedOx = oy;
    const std::int32_t turnedOy = bitmapHu - paperW - ox;
    std::vector<UprightLater> outerLater;
    std::swap(uprightLater, outerLater);
    const std::int32_t outerBitmapHu = turnedBitmapHu;
    turnedBitmapHu = bitmapHu;
    clips.push_back(turnedClip);
    applyCpuClip();
    turning = true;
    walk(node, turnedOx, turnedOy, inObject, borderJoin);
    turning = false;
    turnedBitmapHu = outerBitmapHu;
    std::vector<UprightLater> later;
    std::swap(uprightLater, later);
    std::swap(uprightLater, outerLater);
    if (skiaCanvas) {
        while (skiaSaves > 0) {
            skiaCanvas->restore();
            --skiaSaves;
        }
    }
    skiaSurface = std::move(outerSurface);
    skiaCanvas = outerCanvas;
    skiaSaves = outerSaves;
    viewRightHu = outerRight;
    viewBottomHu = outerBottom;
    std::swap(clips, outerClips);
    std::swap(bitmap, turned);
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const std::uint8_t* from = turned.px.data() + (static_cast<std::size_t>(bw - 1 - bx) * bh + by) * 4u;
            std::uint8_t* to = bitmap.px.data() + (static_cast<std::size_t>(by) * bw + bx) * 4u;
            std::memcpy(to, from, 4);
        }
    }
    // 세운 개체는 종이로 돌려 놓은 뒤 그 종이 자리에 그대로 그린다.
    for (const UprightLater& item : later) walk(*item.node, item.x, item.y, item.inObject);
}

void Painter::walk(const Node& node, std::int32_t ox, std::int32_t oy, bool inObject, int borderJoin) {
    if (node.role == "pageFill" || node.role == "pageBorder") return;
    if (node.turned != 0 && !turning) {
        walkTurned(node, ox, oy, inObject, borderJoin);
        return;
    }
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    const std::int32_t boxH = node.height > 0 ? node.height : node.lineData().textHeight;
    const bool fileBox = keepsFileHeight(node);
    const std::int32_t viewH = fileBox ? std::max(boxH, paintedExtent(node)) : boxH;
    if (missesView(x, y, node.width, viewH)) return;
    if (node.kind == NodeKind::Object || node.kind == NodeKind::Cell) {
        reportObject(node, x + originX, y + originY);
    }
    if (node.kind == NodeKind::Line) {
        line(node, x, y, inObject);
        return;
    }
    const BandScope band(*this, node.pageBand);
    if (node.kind == NodeKind::Paper) paintPageFill(node, x, y);
    if (node.kind == NodeKind::Paragraph) paintParagraphChrome(node, ox, oy, borderJoin, true, false);
    const bool boxed = node.kind == NodeKind::Object || node.kind == NodeKind::Cell;
    if (node.kind == NodeKind::Cell) cellChrome(node, x, y, false);
    else if (node.kind == NodeKind::Object && !node.treatAsChar) objectBox(node, x, y);
    if (node.treatAsChar && node.kind == NodeKind::Object) return;
    const CoverScope cover(*this, node.kind == NodeKind::Object && coversText(node));
    // 칸·표·묶음은 적힌 높이로 테두리를 두고, 넘치는 글과 그림은 잘라내지 않는다.
    // 묶음 너비는 자식보다 좁게 적혀 있을 수 있다. 그릴 개체는 자식의 위치와 너비다.
    if (boxed && clipsOwnBox(node)) {
        const std::int32_t bleed = node.kind == NodeKind::Object ? tableBorderBleed(node) : 0;
        pushClip(x - bleed, y - bleed, node.width + bleed * 2, node.height + bleed * 2);
    }
    // 개체 안 글은 그 개체와 같은 층이다. 그림이 글 층 위로 올라와 묶음 글씨를 덮지 않는다.
    const ObjectLayerScope layers(*this, node.kind == NodeKind::Object, objectPlace(node));
    paintStackedChildren(node, x, y, inObject || node.kind == NodeKind::Object || node.kind == NodeKind::Cell);
    if (node.kind == NodeKind::Paper) paintPageBorder(node, x, y);
    if (node.kind == NodeKind::Paragraph) paintParagraphChrome(node, ox, oy, borderJoin, false, true);
    if (node.kind == NodeKind::Object && node.role == "table") paintTableBorders(node, x, y);
    if (boxed && clipsOwnBox(node)) popClip();
    if (node.kind == NodeKind::Object && !node.treatAsChar) {
        for (const Node& child : node.children) {
            if (child.role != "caption") continue;
            reportCaption(child, x + child.x + originX, y + child.y + originY);
            if (floatCaptions) walk(child, x, y, true);
        }
    }
}

void Painter::paintStackedChildren(const Node& node, std::int32_t x, std::int32_t y, bool inObject) {
    // 돌린 틀 안의 세운 개체 상자. 틀의 (x, y, 너비, 높이) 는 종이에서 (비트맵 폭 − y − 높이, x) 부터다.
    if (node.upright && turning) {
        for (const auto& child : node.children) {
            uprightLater.push_back({&child, turnedBitmapHu - (y + node.height), x, inObject});
        }
        return;
    }
    std::vector<const Node*> behind;
    std::vector<const Node*> front;
    std::vector<const Node*> bands;
    std::vector<const Node*> flow;
    const bool masterOnly = node.kind == NodeKind::Paper && masterView();
    for (const auto& child : node.children) {
        if (child.role == "caption" || child.role == "pageFill" || child.role == "pageBorder") continue;
        if (masterOnly && !child.fromMaster) continue;
        if (child.pageBand) {
            bands.push_back(&child);
            continue;
        }
        if (child.kind == NodeKind::Object && !child.treatAsChar) {
            // 바탕쪽은 글 앞으로여도 본문 뒤에 깐다. 인쇄 시 맨 위면 본문보다 위에 올린다.
            const bool masterBack = child.fromMaster && !child.masterOnTop;
            const bool masterFront = child.fromMaster && child.masterOnTop;
            if (masterFront || (!masterBack && child.ext().obj().textWrap != "BEHIND_TEXT")) front.push_back(&child);
            else behind.push_back(&child);
        } else {
            flow.push_back(&child);
        }
    }
    const auto byZ = [](const Node* a, const Node* b) { return a->ext().obj().zOrder < b->ext().obj().zOrder; };
    std::stable_sort(behind.begin(), behind.end(), byZ);
    std::stable_sort(front.begin(), front.end(), byZ);
    for (const Node* child : behind) walk(*child, x, y, inObject);
    for (const Node* child : bands) walk(*child, x, y, inObject);
    for (const Node* child : flow) {
        int join = 0;
        if (child->kind == NodeKind::Paragraph) {
            for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
                if (&node.children[static_cast<std::size_t>(i)] == child) {
                    join = paragraphBorderJoin(node, i);
                    break;
                }
            }
        }
        walk(*child, x, y, inObject, join);
    }
    paintColumnRules(node, x, y);
    for (const Node* child : front) walk(*child, x, y, inObject);
}

void Painter::paintCaptions(const Node& node, std::int32_t x, std::int32_t y, bool inObject) {
    for (const Node& child : node.children) {
        if (child.role != "caption") continue;
        reportCaption(child, x + child.x + originX, y + child.y + originY);
        walk(child, x, y, inObject);
    }
}

void Painter::expandObjectBandBounds(const Node& node, std::int32_t ox, std::int32_t oy, std::int32_t& x0,
                                     std::int32_t& y0, std::int32_t& x1, std::int32_t& y1, bool& any) {
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    const std::int32_t w = node.width;
    const std::int32_t h = keepsFileHeight(node) ? std::max(node.height, paintedExtent(node)) : node.height;
    if (w > 0 && h > 0) {
        const std::int32_t top = y - (node.kind == NodeKind::Line ? std::max<std::int32_t>(0, node.lineData().auditRise) : 0);
        const std::int32_t bottom =
            y + h + (node.kind == NodeKind::Line ? std::max<std::int32_t>(0, node.lineData().auditBelow) : 0);
        if (!any) {
            x0 = x;
            y0 = top;
            x1 = x + w;
            y1 = bottom;
            any = true;
        } else {
            x0 = std::min(x0, x);
            y0 = std::min(y0, top);
            x1 = std::max(x1, x + w);
            y1 = std::max(y1, bottom);
        }
    }
    if (node.kind == NodeKind::Line) {
        enterPaintedObjects(
            node, x, y, [&](const Node& child, std::int32_t cx, std::int32_t cy, const Node* painted) {
                if (painted && painted->width > 0 && painted->height > 0) {
                    const std::int32_t ph = keepsFileHeight(*painted)
                                                ? std::max(painted->height, paintedExtent(*painted))
                                                : painted->height;
                    if (!any) {
                        x0 = cx;
                        y0 = cy;
                        x1 = cx + painted->width;
                        y1 = cy + ph;
                        any = true;
                    } else {
                        x0 = std::min(x0, cx);
                        y0 = std::min(y0, cy);
                        x1 = std::max(x1, cx + painted->width);
                        y1 = std::max(y1, cy + ph);
                    }
                }
                expandObjectBandBounds(child, cx, cy, x0, y0, x1, y1, any);
            });
        return;
    }
    for (const Node& child : node.children) {
        expandObjectBandBounds(child, x, y, x0, y0, x1, y1, any);
    }
}

}  // namespace chwp::render::paint_detail
