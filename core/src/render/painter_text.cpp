#include "render/painter.hpp"

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "noting/common/unicode.hpp"
#include "noting/reader.hpp"
#include "noting/render/view.hpp"
#include "render/equation_raster.hpp"

// Painter: 줄 그리기: 글자 표시, 글자 꾸밈, 문단 꾸밈, 조판 보고, 트리 순회

namespace noting::render::paint_detail {

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
    const bool justifySpaces = gapsBetween > 0 && !node.lineBreak &&
                               (align == "DISTRIBUTE_SPACE" ||
                                ((align.empty() || align == "JUSTIFY") && !node.lastLine));
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
    }
    return fit;
}

Painter::GlyphBase Painter::glyphBaseIn(const Node& line, std::int32_t size, const std::string& font,
                                        int charOffset) {
    // 글꼴 기준이면 글자 크기가 달라도 같은 기준선을 쓴다.
    // 줄 기준선은 글자 기준선, 그림은 위에서 85%, 수식은 수식 높이×백분율 중 큰 값이다.
    // 위·가운데·아래는 각 글자 상자를 그 줄 글자 상자의 그 자리에 맞춘다.
    GlyphBase out;
    const std::int32_t box = line.textHeight > 0 ? line.textHeight : size;
    out.ownBase = noting::layout::glyphBaseline(size);
    if (line.matchFontBaseline) {
        out.ownBase = noting::layout::fontLineBox(font, size).ascent;
    }
    const std::string& mode = line.vertAlign;
    out.baseline = line.lineBaseline >= 0 ? line.lineBaseline : noting::layout::glyphBaseline(box);
    if (mode == "TOP") out.baseline = out.ownBase;
    else if (mode == "CENTER") out.baseline = (box - size) / 2 + out.ownBase;
    else if (mode == "BOTTOM") out.baseline = box - size + noting::layout::glyphBaseline(size);
    // 글자 위치는 그 글자 크기의 백분율. 양수는 아래로, 음수는 위로. 줄 높이는 바꾸지 않는다.
    out.baseline += size * charOffset / 100;
    return out;
}

Painter::LinePlacement Painter::placeLine(const Node& node, const std::vector<Mark>& marks) const {
    LinePlacement place;
    place.fit = fitLine(node, marks);
    const LineFit& fit = place.fit;
    place.at.reserve(marks.size());
    std::int32_t extra = 0;
    std::int32_t spaceLeft = fit.spaceLeft;
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const Mark& mark = marks[i];
        place.at.push_back(fit.slide + mark.x + fit.shift + extra);
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
    }
    if (marks.empty()) {
        place.end = fit.slide;
    } else {
        const Mark& last = marks.back();
        place.end = fit.slide + last.x + last.width + fit.shift + extra;
    }
    return place;
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
    if (node.shapeType == "rect") return "사각형";
    if (node.shapeType == "ellipse") return "타원";
    if (node.shapeType == "line") return "선";
    if (node.shapeType == "polygon") return "다각형";
    if (node.shapeType == "curve") return "곡선";
    if (node.shapeType == "arc") return "호";
    if (node.shapeType == "ole") return "OLE";
    return "도형";
}

void Painter::reportObject(const Node& node, std::int32_t pageX, std::int32_t pageY) {
    if (pass != PaintLayer::All && pass != PaintLayer::Text) return;
    if (node.kind != NodeKind::Object && node.kind != NodeKind::Cell) return;
    if (node.width <= 0 || node.height <= 0) return;
    const std::string key = "o:" + std::to_string(pageIndex) + ":" + std::to_string(pageX) + ":" +
                            std::to_string(pageY) + ":" + std::to_string(node.width) + ":" +
                            std::to_string(node.height) + ":" + std::to_string(node.row) + ":" +
                            std::to_string(node.col) + ":" + std::to_string(node.zOrder) + ":" + node.role +
                            ":" + node.shapeType;
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
        if (node.role == "equation" && node.eqBase >= 0) {
            mark("너비", node.fileWidth != node.eqWidth);
            mark("높이", node.fileHeight != node.eqHeight);
            int storedBase = node.fileEqBase >= 0 ? node.fileEqBase : node.baseLine;
            if (storedBase == 0) storedBase = 85;
            if (storedBase > 0) mark("기준선", storedBase != node.eqBase);
        } else {
            mark("너비", node.fileWidth != node.width);
            mark("높이", node.fileHeight != node.height);
        }
        if (node.kind == NodeKind::Cell) {
            mark("안여백", node.filePadLeft != node.marginLeft || node.filePadRight != node.marginRight ||
                             node.filePadTop != node.marginTop || node.filePadBottom != node.marginBottom);
        }
    }
    std::string out = "{\"요소\":";
    quoteJson(out, objectLabel(node));
    if (!diff.empty()) out += ",\"경고\":\"개체\"";
    if (pageIndex >= 0) out += ",\"쪽\":" + std::to_string(pageIndex + 1);
    if (node.kind == NodeKind::Cell) {
        out += ",\"행\":" + std::to_string(node.row + 1);
        out += ",\"열\":" + std::to_string(node.col + 1);
        if (node.rowSpan > 1) out += ",\"행합\":" + std::to_string(node.rowSpan);
        if (node.colSpan > 1) out += ",\"열합\":" + std::to_string(node.colSpan);
    }
    if (!node.shapeType.empty() && node.role != "picture" && node.role != "table" && node.role != "equation" &&
        node.role != "container") {
        out += ",\"종류\":";
        quoteJson(out, node.shapeType);
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
        storedNum("너비", node.fileWidth);
        storedNum("높이", node.fileHeight);
        storedText("가로기준", node.fileHorzRel);
        storedText("세로기준", node.fileVertRel);
        storedText("가로맞춤", node.fileHorzAlign);
        storedText("세로맞춤", node.vertAlign);
        storedNum("가로오프셋", node.fileX);
        storedNum("세로오프셋", node.fileY);
        if (node.fileGroupX != 0 || node.fileGroupY != 0) {
            storedNum("묶음x", node.fileGroupX);
            storedNum("묶음y", node.fileGroupY);
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
        const bool scaled = std::fabs(node.fileScaleX - 1.0) > 1e-9 || std::fabs(node.fileScaleY - 1.0) > 1e-9;
        if (scaled) {
            storedReal("배율x", node.fileScaleX);
            storedReal("배율y", node.fileScaleY);
        }
        if (node.fileMatrixX != 0 || node.fileMatrixY != 0) {
            storedNum("행렬이동x", node.fileMatrixX);
            storedNum("행렬이동y", node.fileMatrixY);
        }
        if (storedField) out += ',';
        storedField = true;
        out += node.kind == NodeKind::Cell ? "\"안여백\":" : "\"글상자안여백\":";
        appendQuad(out, node.filePadLeft, node.filePadRight, node.filePadTop, node.filePadBottom);
        if (node.kind == NodeKind::Cell && node.fileOwnMargin >= 0) storedNum("자체여백", node.fileOwnMargin);
        if (node.fileRows > 0) storedNum("행", node.fileRows);
        if (node.fileCols > 0) storedNum("열", node.fileCols);
    }
    if (node.kind == NodeKind::Object) {
        storedNum("바깥여백왼쪽", node.marginLeft);
        storedNum("바깥여백오른쪽", node.marginRight);
        storedNum("바깥여백위", node.marginTop);
        storedNum("바깥여백아래", node.marginBottom);
        storedNum("글자처럼", node.treatAsChar ? 1 : 0);
        storedText("배치", node.textWrap);
        storedNum("z", node.zOrder);
        if (node.cellSpacing > 0) storedNum("셀간격", node.cellSpacing);
        storedText("그림", node.binItemId);
        if (node.picture.clipRight > node.picture.clipLeft && node.picture.clipBottom > node.picture.clipTop) {
            storedNum("자르기왼쪽", node.picture.clipLeft);
            storedNum("자르기위", node.picture.clipTop);
            storedNum("자르기오른쪽", node.picture.clipRight);
            storedNum("자르기아래", node.picture.clipBottom);
        }
        if (node.picture.originWidth > 0) storedNum("원본너비", node.picture.originWidth);
        if (node.picture.originHeight > 0) storedNum("원본높이", node.picture.originHeight);
        if (node.picture.effect != 0) storedNum("효과", node.picture.effect);
        if (node.picture.bright != 0) storedNum("밝기", node.picture.bright);
        if (node.picture.contrast != 0) storedNum("명암", node.picture.contrast);
        if (node.picture.transparency != 0) storedNum("투명도", node.picture.transparency);
        storedText("수식", node.script);
        if (node.role == "equation" && node.eqBase >= 0) {
            const int storedBase = node.fileEqBase >= 0 ? node.fileEqBase : node.baseLine;
            if (storedBase >= 0) storedNum("기준선", storedBase);
        }
    }
    const std::int32_t measureW = node.role == "equation" && node.eqBase >= 0 ? node.eqWidth : node.width;
    const std::int32_t measureH = node.role == "equation" && node.eqBase >= 0 ? node.eqHeight : node.height;
    out += "},\"측정\":{\"x\":" + std::to_string(pageX);
    out += ",\"y\":" + std::to_string(pageY);
    out += ",\"너비\":" + std::to_string(measureW);
    out += ",\"높이\":" + std::to_string(measureH);
    if (node.role == "equation" && node.eqBase >= 0) out += ",\"기준선\":" + std::to_string(node.eqBase);
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
    if (!node.fileHorzRel.empty()) {
        out += ",\"방향\":";
        quoteJson(out, node.fileHorzRel);
    }
    out += ",\"저장\":{";
    out += "\"간격\":" + std::to_string(node.fileX);
    out += ",\"너비\":" + std::to_string(node.fileWidth);
    out += ",\"높이\":" + std::to_string(node.fileHeight);
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
    const model::LineSeg& stored = node.storedSeg;
    const model::LineSeg& measured = node.measuredSeg;
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
    if (node.auditIndex >= 0) out += ",\"줄\":" + std::to_string(node.auditIndex + 1);
    out += ",\"글자\":" + std::to_string(std::max(0, node.start));
    out += ",\"다른필드\":[" + diff + "]";
    out += ",\"lineseg\":";
    appendLineSeg(out, stored, 0, 0, false);
    out += ",\"측정\":";
    appendLineSeg(out, measured, node.auditRise, node.auditBelow, true);
    const std::int32_t boxW = node.width;
    const std::int32_t boxH = node.textHeight > 0 ? node.textHeight : node.height;
    if (boxW > 0 && boxH > 0) {
        out += ",\"영역\":[" + std::to_string(pageX) + "," + std::to_string(pageY) + "," +
               std::to_string(boxW) + "," + std::to_string(boxH) + "]";
    }
    out += "}\n";
    std::fputs(out.c_str(), stdout);
    std::fflush(stdout);
}

std::vector<Painter::Mark> Painter::collectMarks(const Node& node) {
    std::vector<Mark> marks;
    std::int32_t pen = 0;
    for (std::size_t slotIndex = 0; slotIndex < node.children.size(); ++slotIndex) {
        const auto& slot = node.children[slotIndex];
        // 개요·번호·글머리는 본문 왼쪽에 매단다. 펜과 정렬 폭에는 넣지 않는다.
        if (slot.headingMark) continue;
        if (slot.kind == NodeKind::Span && slot.noteMark) {
            // 번호와 뒤 장식은 한 칸이다. 1 과 ) 사이에는 커서가 서지 않는다.
            Mark mark;
            mark.noteSpan = &slot;
            mark.x = pen;
            const std::vector<std::int32_t>* advances = &slot.advances;
            if (slot.advances.empty() && slot.width == 0 && !slot.text.empty() && doc) {
                advances = &rememberSpanMeasure(slot, doc).advances;
            }
            std::int32_t width = slot.width;
            if (!advances->empty()) {
                width = 0;
                for (const std::int32_t advance : *advances) width += advance;
            }
            mark.width = width;
            mark.band = width;
            const int size = slot.fontSize > 0 ? slot.fontSize : (node.fontSize > 0 ? node.fontSize : 1000);
            mark.size = size;
            mark.ratio = slot.ratio > 0 ? slot.ratio : 100;
            mark.relSize = slot.relSize > 0 ? slot.relSize : 100;
            mark.charOffset = slot.charOffset;
            mark.charShapeId = slot.charShapeId;
            mark.bold = slot.bold;
            mark.italic = slot.italic;
            mark.superscript = slot.superscript;
            mark.subscript = slot.subscript;
            mark.font = slot.font.empty() ? node.font : slot.font;
            mark.color = slot.color;
            mark.background = slot.fillColor;
            mark.shade = slot.shade;
            mark.markpen = slot.markpen;
            std::size_t index = 0;
            if (!nextCp(slot.text, index, mark.cp)) mark.cp = 0;
            marks.push_back(std::move(mark));
            pen += width;
        } else if (slot.kind == NodeKind::Span) {
            const std::string text = slot.text;
            std::size_t index = 0;
            const int size = slot.fontSize > 0 ? slot.fontSize : (node.fontSize > 0 ? node.fontSize : 1000);
            const int ratio = slot.ratio > 0 ? slot.ratio : 100;
            const std::string& font = slot.font.empty() ? node.font : slot.font;
            // 줄 캐시로 가른 줄은 글자 폭을 비워 둔다. 처음 그릴 때 재서 메모리에 남긴다.
            const std::vector<std::int32_t>* advances = &slot.advances;
            const std::vector<std::int32_t>* inks = &slot.inks;
            if (slot.advances.empty() && slot.width == 0 && !slot.text.empty() && slot.text != "\t" && doc) {
                const SpanMeasure& measured = rememberSpanMeasure(slot, doc);
                advances = &measured.advances;
                inks = &measured.inks;
            }
            if (!advances->empty()) {
                std::size_t glyphIndex = 0;
                for (const std::int32_t width : *advances) {
                    Mark mark;
                    if (!nextCp(text, index, mark.cp)) mark.cp = 0;
                    mark.x = pen;
                    mark.width = width;
                    mark.band = glyphIndex < inks->size() ? (*inks)[glyphIndex] : width;
                    ++glyphIndex;
                    mark.size = size;
                    mark.ratio = ratio;
                    mark.relSize = slot.relSize > 0 ? slot.relSize : 100;
                    mark.charOffset = slot.charOffset;
                    mark.charShapeId = slot.charShapeId;
                    mark.bold = slot.bold;
                    mark.italic = slot.italic;
                    mark.superscript = slot.superscript;
                    mark.subscript = slot.subscript;
                    mark.font = font;
                    mark.color = slot.color;
                    mark.background = slot.fillColor;
                    mark.shade = slot.shade;
                    mark.markpen = slot.markpen;
                    // 일반 공백만 양쪽 정렬로 늘린다. 고정폭 빈칸은 그대로 둔다.
                    mark.adjustable = mark.cp == ' ';
                    marks.push_back(std::move(mark));
                    pen += width;
                }
            } else if (slot.text == "\t") {
                Mark mark;
                mark.cp = '\t';
                mark.x = pen;
                std::int32_t width = slot.width;
                std::string leader = slot.tabLeader;
                if (node.deferTabs) {
                    bool cached = false;
                    {
                        std::shared_lock lock(gSpanMu);
                        const auto found = tabMeasures().find(&slot);
                        if (found != tabMeasures().end() && found->second.pen == pen) {
                            width = found->second.width;
                            leader = found->second.leader;
                            cached = true;
                        }
                    }
                    if (!cached) {
                        const TabGap gap = tabGapAt(node, pen, slotIndex, doc);
                        width = gap.width;
                        leader = gap.leader;
                        std::unique_lock lock(gSpanMu);
                        const auto found = tabMeasures().find(&slot);
                        if (found != tabMeasures().end() && found->second.pen == pen) {
                            width = found->second.width;
                            leader = found->second.leader;
                        } else {
                            tabMeasures().insert_or_assign(&slot, TabMeasure{pen, width, leader});
                        }
                    }
                }
                mark.width = width;
                mark.size = size;
                mark.charShapeId = slot.charShapeId;
                mark.font = font;
                mark.color = slot.color.empty() ? node.color : slot.color;
                mark.leader = std::move(leader);
                marks.push_back(std::move(mark));
                pen += width;
            } else if (slot.text == " ") {
                Mark mark;
                mark.cp = ' ';
                mark.x = pen;
                mark.width = slot.width;
                mark.band = slot.inks.empty() ? slot.width : slot.inks.front();
                mark.size = size;
                mark.ratio = ratio;
                mark.relSize = slot.relSize > 0 ? slot.relSize : 100;
                mark.charOffset = slot.charOffset;
                mark.charShapeId = slot.charShapeId;
                mark.bold = slot.bold;
                mark.italic = slot.italic;
                mark.superscript = slot.superscript;
                mark.subscript = slot.subscript;
                mark.font = font;
                mark.color = slot.color;
                mark.background = slot.fillColor;
                mark.shade = slot.shade;
                mark.markpen = slot.markpen;
                mark.adjustable = true;
                marks.push_back(std::move(mark));
                pen += slot.width;
            } else {
                pen += slot.width;
            }
        } else if (slot.kind == NodeKind::Object) {
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
            mark.size = slot.fontSize > 0 ? slot.fontSize : (node.fontSize > 0 ? node.fontSize : 1000);
            mark.charShapeId = slot.charShapeId;
            mark.font = slot.font.empty() ? node.font : slot.font;
            mark.object = &slot;
            mark.slot = static_cast<int>(slotIndex);
            marks.push_back(mark);
            pen = mark.x + mark.width;
        }
    }
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

void Painter::paintBitmapDeco(const model::CharShape& cs, const Mark& mark, std::int32_t at, std::int32_t lineTop,
                              int lineBox, std::int32_t glyphBaseline, int drawSize) {
    if (placesOnly) return;
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
        const std::int32_t glyphTop = glyphBaseline - noting::layout::glyphBaseline(drawSize);
        paintEmphasisMark(cs.emphasis, px(at + band / 2), px(glyphTop) - rad - 1, rad, color);
    }
    if (const model::BorderFill* fill = borderFillOf(cs.borderFillId)) {
        const std::int32_t band = mark.band > 0 ? mark.band : span;
        if (band > 0 && drawSize > 0) {
            paintCharBorderBox(*fill, at, glyphBaseline - noting::layout::glyphBaseline(drawSize), band, drawSize);
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
        inked(node.borderLeft) || inked(node.borderRight) || inked(node.borderTop) || inked(node.borderBottom);
    const bool face = !node.fillColor.empty() && node.fillColor != "none";
    if (!anySide && !face) return;
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
        parseHex(node.fillColor, r, g, b);
        fillRect(bitmap, x0, y0, tw, th, r, g, b);
    }
    if (!strokePass) return;
    if (!(join & 1)) {
        if (const int t = edgeThickPx(node.borderTop))
            strokeEdge(node.borderTop, x0 - t / 2, y0 - t / 2, tw + t, t);
    }
    if (!(join & 2)) {
        if (const int t = edgeThickPx(node.borderBottom))
            strokeEdge(node.borderBottom, x0 - t / 2, y1 - t / 2, tw + t, t);
    }
    if (const int t = edgeThickPx(node.borderLeft)) strokeEdge(node.borderLeft, x0 - t / 2, y0 - t / 2, t, th + t);
    if (const int t = edgeThickPx(node.borderRight))
        strokeEdge(node.borderRight, x1 - t / 2, y0 - t / 2, t, th + t);
}

void Painter::drawHeading(const Node& line, std::int32_t x, std::int32_t y, std::int32_t shift, std::int32_t slide) {
    for (const auto& slot : line.children) {
        if (!slot.headingMark) continue;
        const std::int32_t at = x + slide + slot.x + shift;
        if (slot.kind == NodeKind::Object) {
            // 글머리 그림은 글자처럼 넣은 개체와 달리 따로 칸이 없다. 줄 칸에 그린다.
            if (placesOnly) continue;
            objectBox(slot, at, y + slot.y);
            continue;
        }
        const int size = slot.fontSize > 0 ? slot.fontSize : 1000;
        const int box = line.textHeight > 0 ? line.textHeight : size;
        const std::string mode = line.vertAlign.empty() ? "BASELINE" : line.vertAlign;
        int baseline = line.lineBaseline >= 0 ? line.lineBaseline : noting::layout::glyphBaseline(box);
        baseline += scriptBaselineShift(size, slot.superscript, slot.subscript, mode, slot.text.empty());
        std::int32_t pen = at;
        std::size_t index = 0;
        std::size_t nth = 0;
        char32_t cp = 0;
        while (nextCp(slot.text, index, cp)) {
            const std::int32_t advance = nth < slot.advances.size() ? slot.advances[nth] : size;
            ++nth;
            glyph(cp, slot.font, relativeDrawSize(size, slot.relSize, slot.superscript || slot.subscript),
                  slot.ratio > 0 ? slot.ratio : 100,
                  slot.bold, slot.italic, slot.color.empty() ? "#000000" : slot.color, pen,
                  y + line.marginTop + baseline);
            pen += advance;
        }
    }
}

void Painter::line(const Node& node, std::int32_t x, std::int32_t y, bool inObject) {
    std::vector<Mark> marks = collectMarks(node);
    const LinePlacement place = placeLine(node, marks);
    if (!placesOnly && node.auditLine && (pass == PaintLayer::All || pass == PaintLayer::Text)) {
        reportLineSeg(node, x + originX, y + originY, inObject);
    }

    drawHeading(node, x, y, place.fit.shift, place.fit.slide);

    for (std::size_t i = 0; i < marks.size(); ++i) {
        const Mark& mark = marks[i];
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
            const ObjectHostScope host(*this, node.story, node.paragraph, node.start + static_cast<int>(i));
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
            fillBand(mark.background);
            fillBand(mark.shade);
            fillBand(mark.markpen);
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
                    const int lineBox = node.textHeight > 0 ? node.textHeight : mark.size;
                    const std::int32_t script =
                        scriptBaselineShift(mark.size, mark.superscript, mark.subscript, mode, mark.cp == 0);
                    paintBitmapDeco(*deco, mark, at, y + node.marginTop, lineBox,
                                    y + node.marginTop + baseline + script, mark.size);
                }
            } else {
                const std::vector<std::int32_t>* advances = nullptr;
                const std::vector<std::int32_t>* inks = nullptr;
                if (mark.noteSpan && doc) {
                    advances = &mark.noteSpan->advances;
                    inks = &mark.noteSpan->inks;
                    if (advances->empty()) {
                        const SpanMeasure& measured = rememberSpanMeasure(*mark.noteSpan, doc);
                        advances = &measured.advances;
                        inks = &measured.inks;
                    }
                }
                std::size_t textIndex = 0;
                std::size_t nth = 0;
                char32_t cp = mark.cp;
                std::int32_t pen = at;
                while (true) {
                    if (mark.noteSpan && !nextCp(mark.noteSpan->text, textIndex, cp)) break;
                    Mark glyphMark = mark;
                    glyphMark.noteSpan = nullptr;
                    glyphMark.cp = cp;
                    if (mark.noteSpan && advances) {
                        const std::int32_t width = nth < advances->size() ? (*advances)[nth] : glyphMark.size;
                        glyphMark.width = width;
                        glyphMark.band = inks && nth < inks->size() ? (*inks)[nth] : width;
                    }
                    const std::int32_t drawShift =
                        scriptBaselineShift(glyphMark.size, glyphMark.superscript, glyphMark.subscript, mode,
                                            glyphMark.cp == 0);
                    const int drawSize = relativeDrawSize(glyphMark.size, glyphMark.relSize,
                                                          glyphMark.superscript || glyphMark.subscript);
                    const std::int32_t glyphBase = y + node.marginTop + baseline + drawShift;
                    const model::CharShape* deco = doc ? findCharShape(*doc, glyphMark.charShapeId) : nullptr;
                    std::string glyphColor = glyphMark.color.empty() ? "#000000" : glyphMark.color;
                    if (deco && deco->outline != 0) {
                        if (!glyphMark.shade.empty() && glyphMark.shade != "none") glyphColor = glyphMark.shade;
                        else if (!glyphMark.background.empty() && glyphMark.background != "none")
                            glyphColor = glyphMark.background;
                        else glyphColor = "#FFFFFF";
                    }
                    const int slot = node.start + static_cast<int>(i);
                    if (deco) {
                        paintGlyphEffects(*deco, glyphMark, pen, glyphBase, drawSize, node.story, node.paragraph,
                                          slot);
                    }
                    glyph(glyphMark.cp, glyphMark.font, drawSize, glyphMark.ratio, glyphMark.bold,
                          glyphMark.italic, glyphColor, pen, glyphBase, node.story, node.paragraph, slot);
                    if (deco) {
                        const int lineBox = node.textHeight > 0 ? node.textHeight : glyphMark.size;
                        paintBitmapDeco(*deco, glyphMark, pen, y + node.marginTop, lineBox, glyphBase, drawSize);
                    }
                    if (!mark.noteSpan) break;
                    pen += glyphMark.width;
                    ++nth;
                }
            }
        }
    }
}

void Painter::strokePageBorderNode(const Node& node, std::int32_t x, std::int32_t y) {
    if (node.width <= 0 || node.height <= 0) return;
    strokeTableBorder(node.borderLeft, x, y, x, y + node.height, 1);
    strokeTableBorder(node.borderRight, x + node.width, y, x + node.width, y + node.height, -1);
    strokeTableBorder(node.borderTop, x, y, x + node.width, y, 1);
    strokeTableBorder(node.borderBottom, x, y + node.height, x + node.width, y + node.height, -1);
    auto joinCorner = [&](const BorderEdge& horizontal, const BorderEdge& vertical, std::int32_t hx,
                          std::int32_t hy, int hInside, int vInside) {
        if (tableLineKind(horizontal.type) == 0 || tableLineKind(horizontal.type) != tableLineKind(vertical.type) ||
            borderInkPixels(horizontal) != borderInkPixels(vertical) || horizontal.color != vertical.color) {
            return;
        }
        mitreTableCorner(hx, hy, hInside, vInside, horizontal);
    };
    joinCorner(node.borderTop, node.borderLeft, x, y, 1, 1);
    joinCorner(node.borderTop, node.borderRight, x + node.width, y, 1, -1);
    joinCorner(node.borderBottom, node.borderLeft, x, y + node.height, -1, 1);
    joinCorner(node.borderBottom, node.borderRight, x + node.width, y + node.height, -1, -1);
}

void Painter::paintPiece(const Node& node, int kind, std::int32_t x, std::int32_t y, bool inObject,
                         std::int32_t stripY0, std::int32_t stripY1) {
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
    else if (kind >= 3 && node.kind == NodeKind::Column) paintColumnRuleSpan(node.borderRight, x, y, stripY1 - stripY0);
    else if (kind >= 3) paintTableBorders(node, x, y, stripY0, stripY1);
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

void Painter::walk(const Node& node, std::int32_t ox, std::int32_t oy, bool inObject, int borderJoin) {
    if (node.role == "pageFill" || node.role == "pageBorder") return;
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;
    const std::int32_t boxH = node.height > 0 ? node.height : node.textHeight;
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
        }
    }
}

void Painter::paintStackedChildren(const Node& node, std::int32_t x, std::int32_t y, bool inObject) {
    std::vector<const Node*> behind;
    std::vector<const Node*> front;
    std::vector<const Node*> bands;
    std::vector<const Node*> flow;
    for (const auto& child : node.children) {
        if (child.role == "caption" || child.role == "pageFill" || child.role == "pageBorder") continue;
        if (child.pageBand) {
            bands.push_back(&child);
            continue;
        }
        if (child.kind == NodeKind::Object && !child.treatAsChar) {
            // 바탕쪽은 글 앞으로여도 본문 뒤에 깐다. 인쇄 시 맨 위면 본문보다 위에 올린다.
            const bool masterBack = child.fromMaster && !child.masterOnTop;
            const bool masterFront = child.fromMaster && child.masterOnTop;
            if (masterFront || (!masterBack && child.textWrap != "BEHIND_TEXT")) front.push_back(&child);
            else behind.push_back(&child);
        } else {
            flow.push_back(&child);
        }
    }
    const auto byZ = [](const Node* a, const Node* b) { return a->zOrder < b->zOrder; };
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
        const std::int32_t top = y - (node.kind == NodeKind::Line ? std::max<std::int32_t>(0, node.auditRise) : 0);
        const std::int32_t bottom =
            y + h + (node.kind == NodeKind::Line ? std::max<std::int32_t>(0, node.auditBelow) : 0);
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

}  // namespace noting::render::paint_detail
