#include "render/painter.hpp"

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "chwp/common/unicode.hpp"
#include "chwp/reader.hpp"
#include "chwp/render/view.hpp"
#include "render/equation_draw.hpp"
#include "render/textart_layout.hpp"

// Painter: 도형과 수식: 경로, 둥근 상자, 타원

namespace chwp::render::paint_detail {

void Painter::stamp(int x, int y, int radius, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int rr = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy > rr) continue;
            blend(bitmap, x + dx, y + dy, r, g, b, 255);
        }
    }
}

void Painter::thickLine(int x0, int y0, int x1, int y1, int thick, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int radius = std::max(0, thick / 2);
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps == 0) {
        stamp(x0, y0, radius, r, g, b);
        return;
    }
    for (int i = 0; i <= steps; ++i) {
        stamp(x0 + dx * i / steps, y0 + dy * i / steps, radius, r, g, b);
    }
}

namespace {

// 한글 2024 곡선(hp:curve CURVE 마디): 점들을 지나는 3차 베지어. 안쪽 점의 접선은 (다음 점 − 앞 점)/2, 끝점의 접선은
// (3·(이웃 점 − 끝점) − 이웃 점의 접선)/2 이고, 마디마다 16 조각의 꺾은선으로 긋는다. 한글이 만든 PDF 의 곡선 꺾은선과
// 같다(점 (0,0)(4500,4500)(9000,0)(13500,4500) 의 마디 가운데가 (2254,3092)(6752,2253)(11250,1402)).
std::vector<model::ShapePoint> hangulCurvePoints(const std::vector<model::ShapePoint>& points) {
    const std::size_t n = points.size();
    if (n < 3) return points;
    std::vector<double> tx(n);
    std::vector<double> ty(n);
    for (std::size_t i = 1; i + 1 < n; ++i) {
        tx[i] = (points[i + 1].x - points[i - 1].x) / 2.0;
        ty[i] = (points[i + 1].y - points[i - 1].y) / 2.0;
    }
    tx[0] = (3.0 * (points[1].x - points[0].x) - tx[1]) / 2.0;
    ty[0] = (3.0 * (points[1].y - points[0].y) - ty[1]) / 2.0;
    tx[n - 1] = (3.0 * (points[n - 1].x - points[n - 2].x) - tx[n - 2]) / 2.0;
    ty[n - 1] = (3.0 * (points[n - 1].y - points[n - 2].y) - ty[n - 2]) / 2.0;
    std::vector<model::ShapePoint> out;
    out.push_back(points.front());
    constexpr int kSteps = 16;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        const double x0 = points[i].x;
        const double y0 = points[i].y;
        const double x3 = points[i + 1].x;
        const double y3 = points[i + 1].y;
        const double x1 = x0 + tx[i] / 3.0;
        const double y1 = y0 + ty[i] / 3.0;
        const double x2 = x3 - tx[i + 1] / 3.0;
        const double y2 = y3 - ty[i + 1] / 3.0;
        for (int s = 1; s <= kSteps; ++s) {
            const double t = static_cast<double>(s) / kSteps;
            const double u = 1.0 - t;
            const double bx = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
            const double by = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
            out.push_back(model::ShapePoint{static_cast<std::int32_t>(std::lround(bx)), static_cast<std::int32_t>(std::lround(by))});
        }
    }
    return out;
}

// 한글 2024 호(hp:arc NORMAL): 중심에서 ax2 끝을 지나 ax1 끝까지의 사분 타원. 한글 PDF 는 한 3차 베지어로 긋는다
// (조절점 ax2 + k·(ax1 − 중심), ax1 + k·(ax2 − 중심), k = 0.5523). 여기서는 32 조각으로 편다.
std::vector<model::ShapePoint> hangulArcPoints(const std::vector<model::ShapePoint>& points) {
    std::vector<model::ShapePoint> out;
    if (points.size() < 3) return out;
    const model::ShapePoint c = points[0];
    const model::ShapePoint a1 = points[1];
    const model::ShapePoint a2 = points[2];
    constexpr double k = 0.5523;
    const double x0 = a2.x;
    const double y0 = a2.y;
    const double x1 = a2.x + k * (a1.x - c.x);
    const double y1 = a2.y + k * (a1.y - c.y);
    const double x2 = a1.x + k * (a2.x - c.x);
    const double y2 = a1.y + k * (a2.y - c.y);
    const double x3 = a1.x;
    const double y3 = a1.y;
    constexpr int kSteps = 32;
    for (int s = 0; s <= kSteps; ++s) {
        const double t = static_cast<double>(s) / kSteps;
        const double u = 1.0 - t;
        const double bx = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        const double by = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        out.push_back(model::ShapePoint{static_cast<std::int32_t>(std::lround(bx)), static_cast<std::int32_t>(std::lround(by))});
    }
    return out;
}

}  // namespace

void Painter::drawPath(const Node& node, std::int32_t x, std::int32_t y, PaintLayer place) {
    if (node.ext().obj().shapeType == "curve") {
        drawPoints(node, hangulCurvePoints(node.ext().obj().points), x, y, place);
        return;
    }
    drawPoints(node, node.ext().obj().points, x, y, place);
}

// 선·다각형·곡선·호·자유선. 채우기와 테두리를 같은 float 경로로 그린다. 점은 픽셀로 반올림하지 않는다.
// 테두리는 한글처럼 윤곽 가운데에 긋는다(한글 2024 PDF: 굵기 1000 의 다각형·선 모두 윤곽이 선 가운데).
// 채우기는 점을 이어 닫은 면이고, 테두리는 적힌 점만 잇는다(닫힌 도형은 파일이 첫 점을 끝에 한 번 더 적는다).
void Painter::drawPoints(const Node& node, const std::vector<model::ShapePoint>& points, std::int32_t x, std::int32_t y,
                         PaintLayer place) {
    if (points.size() < 2) return;
    SkCanvas* canvas = this->canvas();
    if (!canvas) return;
    SkPathBuilder builder;
    builder.moveTo(glyphPx(x + points[0].x), glyphPx(y + points[0].y));
    for (std::size_t i = 1; i < points.size(); ++i) builder.lineTo(glyphPx(x + points[i].x), glyphPx(y + points[i].y));
    const SkPath path = builder.detach();
    const bool close = !node.ext().fillColor.empty() && node.ext().fillColor != "none";
    if (show(place) && close) {
        std::uint8_t r, g, b;
        parseHex(node.ext().fillColor, r, g, b);
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kFill_Style);
        paint.setColor(SkColorSetARGB(255, r, g, b));
        canvas->drawPath(path, paint);
    }
    const BorderEdge& edge = node.ext().borderLeft;
    if (!show(frameStroke()) || edge.type.empty() || edge.type == "NONE") return;
    SkPaint paint;
    strokePaint(edge, paint);
    canvas->drawPath(path, paint);
}

void Painter::equation(const Node& node, std::int32_t x, std::int32_t y, PaintLayer) {
    // 놓을 것은 한글 수식 엔진과 같은 자리다. 잰 크기를 개체 상자에 맞춰 늘린다(크기가 같으면 그대로).
    // 수식 글꼴 윤곽이 아직 없으면(웹에서 글꼴을 받는 중이면) 그리지 않는다. 글꼴이 오면 다시 그린다.
    EquationSize measured;
    const auto inks = cachedEquationInk(node.ext().obj().script, node.ext().obj().eqBaseUnit, node.ext().obj().eqFont,
                                        measured);
    SkCanvas* canvas = this->canvas();
    if (!inks || inks->empty() || measured.width <= 0 || measured.height <= 0 || !canvas) return;
    const float ox = glyphPx(x);
    const float oy = glyphPx(y);
    const float kx = (glyphPx(x + node.width) - ox) / static_cast<float>(measured.width);
    const float ky = (glyphPx(y + node.height) - oy) / static_cast<float>(measured.height);
    for (const EquationInk& ink : *inks) {
        const std::uint8_t r = static_cast<std::uint8_t>(ink.color >> 16);
        const std::uint8_t g = static_cast<std::uint8_t>(ink.color >> 8);
        const std::uint8_t b = static_cast<std::uint8_t>(ink.color);
        if (ink.kind == EquationInk::Kind::Rect) {
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(SkColorSetARGB(255, r, g, b));
            canvas->drawRect(SkRect::MakeXYWH(ox + ink.x * kx, oy + ink.y * ky, ink.width * kx,
                                              std::max(ink.height * ky, 1.f)),
                             paint);
            continue;
        }
        // 윤곽 점 하나는 1/kEquationInkUnit HWPUNIT 이다. 가로 배율을 em 으로, 세로는 fitY 로 준다.
        const float unitX = kx / static_cast<float>(kEquationInkUnit);
        const int fitY = static_cast<int>(std::lround(1000.f * ky / kx));
        paintGlyph(ink.commands, unitX * 1000.f, 100, 1000, fitY, ox, oy, r, g, b, ink.stroke * (kx + ky) / 2.f);
    }
}

bool Painter::roundContains(int col, int row, int width, int height, int radius) {
    if (col < 0 || row < 0 || col >= width || row >= height) return false;
    int dx = 0;
    int dy = 0;
    if (radius > 0) {
        if (col < radius) dx = radius - col;
        else if (col >= width - radius) dx = col - (width - radius - 1);
        if (row < radius) dy = radius - row;
        else if (row >= height - radius) dy = row - (height - radius - 1);
    }
    return dx * dx + dy * dy <= radius * radius;
}

void Painter::fillRoundRect(int x, int y, int w, int h, int rad, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (w <= 0 || h <= 0) return;
    rad = std::max(0, std::min(rad, std::min(w, h) / 2));
    for (int row = 0; row < h; ++row) {
        int run = -1;
        for (int col = 0; col <= w; ++col) {
            const bool on = col < w && roundContains(col, row, w, h, rad);
            if (on && run < 0) run = col;
            if (!on && run >= 0) {
                fillRect(bitmap, x + run, y + row, col - run, 1, r, g, b);
                run = -1;
            }
        }
    }
}

void Painter::strokePaint(const BorderEdge& edge, SkPaint& paint) const {
    std::uint8_t r, g, b;
    parseHex(edge.color.empty() ? "#000000" : edge.color, r, g, b);
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(penPx(edge));
    const bool round = edge.end == 0 || edge.type == "CIRCLE";
    paint.setStrokeCap(round ? SkPaint::kRound_Cap : SkPaint::kButt_Cap);
    paint.setStrokeJoin(round ? SkPaint::kRound_Join : SkPaint::kMiter_Join);
    paint.setStrokeMiter(10.f);  // GDI·PDF 기본 꺾임 한도. 한글 PDF 의 뾰족한 꼭짓점이 잘리지 않는다
    paint.setColor(SkColorSetARGB(255, r, g, b));
    float intervals[8];
    const int count = gdiDash(edge, intervals);
    if (count >= 2) {
        paint.setPathEffect(SkDashPathEffect::Make(SkSpan<const SkScalar>(intervals, static_cast<std::size_t>(count)), 0));
    }
}

float Painter::penPx(const BorderEdge& edge) const {
    const std::int32_t widthHu = edge.width > 0 ? edge.width : 28;
    return std::max(1.f, std::fabs(static_cast<float>(widthHu) * scale));
}

int Painter::gdiDash(const BorderEdge& edge, float* out) const {
    const std::string& type = edge.type;
    if (type.empty() || type == "NONE" || type == "SOLID" || type.find("DOUBLE") != std::string::npos ||
        type.find("SLIM") != std::string::npos) {
        return 0;
    }
    const float w = penPx(edge);
    int count = 0;
    auto push = [&](float on, float off) {
        if (count + 2 > 8) return;
        out[count++] = on;
        out[count++] = off;
    };
    if (type == "DASH") push(7.5f * w, 4.5f * w);
    else if (type == "LONG_DASH") push(15.f * w, 4.5f * w);
    else if (type == "DOT") push(1.5f * w, 2.25f * w);
    else if (type == "CIRCLE") push(1.f * w, 1.25f * w);
    else if (type == "DASH_DOT" || type == "DASH_DOT_DOT") {
        push(15.f * w, 4.5f * w);
        push(1.5f * w, 4.5f * w);
        if (type == "DASH_DOT_DOT") push(1.5f * w, 4.5f * w);
    }
    return count;
}

int Painter::dashPattern(const BorderEdge& edge, int* out) const {
    const std::string& type = edge.type;
    if (type.empty() || type == "NONE" || type == "SOLID" || type.find("DOUBLE") != std::string::npos ||
        type.find("SLIM") != std::string::npos) {
        return 0;
    }
    const std::int32_t widthHu = edge.width > 0 ? edge.width : 28;
    const double widthPx = std::fabs(static_cast<double>(widthHu) * static_cast<double>(scale));
    auto unit = [&](double ratio) { return std::max(1, static_cast<int>(std::lround(ratio * widthPx))); };
    int count = 0;
    auto push = [&](int on, int off) {
        if (on <= 0 || count + 2 > 8) return;
        out[count++] = on;
        out[count++] = std::max(1, off);
    };
    if (type == "DASH") push(unit(7.5), unit(4.5));
    else if (type == "LONG_DASH") push(unit(15.0), unit(4.5));
    else if (type == "DOT") push(unit(1.5), unit(2.25));
    else if (type == "CIRCLE") push(unit(1.0), unit(1.25));
    else if (type == "DASH_DOT" || type == "DASH_DOT_DOT") {
        push(unit(15.0), unit(4.5));
        push(unit(1.5), unit(4.5));
        if (type == "DASH_DOT_DOT") push(unit(1.5), unit(4.5));
    }
    return count;
}

void Painter::roundBox(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, int ratio, const std::string& fill,
                       const BorderEdge& edge, PaintLayer place) {
    if (SkCanvas* canvas = this->canvas()) {
        const float x0 = static_cast<float>(px(x));
        const float y0 = static_cast<float>(px(y));
        const float pw = static_cast<float>(px(x + w) - px(x));
        const float ph = static_cast<float>(px(y + h) - px(y));
        if (pw <= 0.f || ph <= 0.f) return;
        const float rad = std::min(pw, ph) * static_cast<float>(std::max(0, ratio)) / 100.f;
        const SkRRect rrect = SkRRect::MakeRectXY(SkRect::MakeXYWH(x0, y0, pw, ph), rad, rad);
        if (show(place) && !fill.empty() && fill != "none") {
            std::uint8_t r, g, b;
            parseHex(fill, r, g, b);
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::kFill_Style);
            paint.setColor(SkColorSetARGB(255, r, g, b));
            canvas->drawRRect(rrect, paint);
        }
        if (show(frameStroke()) && !edge.type.empty() && edge.type != "NONE") {
            SkPaint paint;
            strokePaint(edge, paint);
            canvas->drawRRect(rrect, paint);
        }
        return;
    }
    const int x0 = px(x);
    const int y0 = px(y);
    const int pw = px(x + w) - x0;
    const int ph = px(y + h) - y0;
    if (pw <= 0 || ph <= 0) return;
    const int rad = std::min(pw, ph) * std::max(0, ratio) / 100;
    const bool paintFill = show(place) && !fill.empty() && fill != "none";
    if (paintFill) {
        std::uint8_t r, g, b;
        parseHex(fill, r, g, b);
        fillRoundRect(x0, y0, pw, ph, rad, r, g, b);
    }
    if (!show(frameStroke()) || edge.type.empty() || edge.type == "NONE") return;
    const int thick = std::max(1, px(edge.width > 0 ? edge.width : 28));
    const int out = thick / 2;
    const int in = thick - out;
    std::uint8_t r, g, b;
    parseHex(edge.color.empty() ? "#000000" : edge.color, r, g, b);
    const int ox = x0 - out;
    const int oy = y0 - out;
    const int ow = pw + out * 2;
    const int oh = ph + out * 2;
    const int orad = rad + out;
    const int iw = pw - in * 2;
    const int ih = ph - in * 2;
    const int irad = std::max(0, rad - in);
    const int inset = out + in;
    const ClipSuspend open(bitmap);
    for (int row = 0; row < oh; ++row) {
        int run = -1;
        for (int col = 0; col <= ow; ++col) {
            const bool outer = col < ow && roundContains(col, row, ow, oh, orad);
            const bool inner = iw > 0 && ih > 0 && roundContains(col - inset, row - inset, iw, ih, irad);
            const bool on = outer && !inner;
            if (on && run < 0) run = col;
            if (!on && run >= 0) {
                fillRect(bitmap, ox + run, oy + row, col - run, 1, r, g, b);
                run = -1;
            }
        }
    }
}

void Painter::fillOval(int x0, int y0, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (w <= 1 || h <= 1) {
        fillRect(bitmap, x0, y0, std::max(1, w), std::max(1, h), r, g, b);
        return;
    }
    const double rx = w * 0.5;
    const double ry = h * 0.5;
    for (int row = 0; row < h; ++row) {
        const double ny = (row + 0.5 - ry) / ry;
        const double span = 1.0 - ny * ny;
        if (span <= 0) continue;
        const double half = rx * std::sqrt(span);
        const int left = static_cast<int>(std::ceil(x0 + rx - half));
        const int right = static_cast<int>(std::floor(x0 + rx + half));
        if (right >= left) fillRect(bitmap, left, y0 + row, right - left + 1, 1, r, g, b);
    }
}

void Painter::ellipseBox(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, const std::string& fill,
                         const BorderEdge& edge, PaintLayer place) {
    const int x0 = px(x);
    const int y0 = px(y);
    const int pw = std::max(1, px(x + w) - x0);
    const int ph = std::max(1, px(y + h) - y0);
    if (show(place) && !fill.empty() && fill != "none") {
        std::uint8_t r, g, b;
        parseHex(fill, r, g, b);
        if (SkCanvas* canvas = this->canvas()) {
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(SkColorSetARGB(255, r, g, b));
            canvas->drawOval(SkRect::MakeLTRB(glyphPx(x), glyphPx(y), glyphPx(x + w), glyphPx(y + h)), paint);
        } else
            fillOval(x0, y0, pw, ph, r, g, b);
    }
    if (!show(frameStroke()) || edge.type.empty() || edge.type == "NONE") return;
    std::uint8_t r, g, b;
    parseHex(edge.color.empty() ? "#000000" : edge.color, r, g, b);
    const int thick = std::max(1, px(edge.width > 0 ? edge.width : 28));
    if (SkCanvas* canvas = this->canvas()) {
        // 한글은 타원 테두리도 윤곽 가운데에 긋는다(한글 2024 PDF). 안쪽으로 들이지 않는다.
        SkPaint paint;
        strokePaint(edge, paint);
        canvas->drawOval(SkRect::MakeLTRB(glyphPx(x), glyphPx(y), glyphPx(x + w), glyphPx(y + h)), paint);
        return;
    }
    const int grow = thick / 2;
    const int ox = x0 - grow;
    const int oy = y0 - grow;
    const int ow = pw + thick;
    const int oh = ph + thick;
    const double rx = ow * 0.5;
    const double ry = oh * 0.5;
    const double inRx = std::max(0.0, rx - thick);
    const double inRy = std::max(0.0, ry - thick);
    for (int row = 0; row < oh; ++row) {
        const double ny = ry <= 0 ? 0 : (row + 0.5 - ry) / ry;
        const double outerSpan = 1.0 - ny * ny;
        if (outerSpan <= 0) continue;
        const double outerHalf = rx * std::sqrt(outerSpan);
        const int left = static_cast<int>(std::ceil(ox + rx - outerHalf));
        const int right = static_cast<int>(std::floor(ox + rx + outerHalf));
        int inLeft = left;
        int inRight = left - 1;
        if (inRy > 0 && inRx > 0) {
            const double iny = (row + 0.5 - ry) / inRy;
            const double innerSpan = 1.0 - iny * iny;
            if (innerSpan > 0) {
                const double innerHalf = inRx * std::sqrt(innerSpan);
                inLeft = static_cast<int>(std::ceil(ox + rx - innerHalf));
                inRight = static_cast<int>(std::floor(ox + rx + innerHalf));
            }
        }
        if (inLeft > left) fillRect(bitmap, left, oy + row, inLeft - left, 1, r, g, b);
        if (right > inRight) fillRect(bitmap, inRight + 1, oy + row, right - inRight, 1, r, g, b);
    }
}

namespace {
// 글이 든 개체(글상자). 안쪽에 글 묶음 줄이 있다.
bool holdsText(const Node& node) {
    for (const Node& child : node.children) {
        if (child.kind == NodeKind::Object) continue;
        if ((child.kind == NodeKind::Line || child.kind == NodeKind::Paragraph) && child.story > 0) return true;
        if (holdsText(child)) return true;
    }
    return false;
}
}  // namespace

// 글맵시: 한글 2024 를 재서 맞춘 모양(textart_layout)으로 글자 윤곽을 휘어 개체 상자에 두고, 개체 채움색으로 칠하고
// 선으로 두른다. 그림자가 있으면 글을 그림자 몫을 뺀 칸에 채우고 그림자를 먼저 그림자 색으로 칠한다(선은 두르지 않는다).
void Painter::textArt(const Node& node, std::int32_t x, std::int32_t y, PaintLayer) {
    SkCanvas* canvas = this->canvas();
    const ObjectExtra& obj = node.ext().obj();
    const std::string& text = obj.artText;
    if (!canvas || text.empty() || node.width <= 0 || node.height <= 0) return;
    model::TextArt art;
    art.shapeNumber = obj.artShape;
    art.lineSpacing = obj.artLineSpacing;
    art.charSpacing = obj.artCharSpacing;
    art.align = obj.artAlign;
    std::vector<ArtCommand> outline = textArtOutline(text, obj.artFont, art);
    std::vector<ArtCommand> shadow;
    if (obj.artShadow) {
        ArtShadowFit fit = fitTextArtShadow(outline, obj.artShadowX, obj.artShadowY);
        outline = std::move(fit.text);
        shadow = std::move(fit.shadow);
    }
    if (outline.empty()) return;
    const float left = glyphPx(x);
    const float top = glyphPx(y);
    const float w = glyphPx(x + node.width) - left;
    const float h = glyphPx(y + node.height) - top;
    const auto at = [&](float nx, float ny) { return SkPoint::Make(left + nx * w, top + ny * h); };
    const auto build = [&](const std::vector<ArtCommand>& commands) {
        SkPathBuilder path;
        bool started = false;
        for (const ArtCommand& c : commands) {
            if (c.verb == GlyphVerb::Move) {
                if (started) path.close();
                path.moveTo(at(c.x, c.y));
                started = true;
            } else if (c.verb == GlyphVerb::Line) {
                path.lineTo(at(c.x, c.y));
            } else if (c.verb == GlyphVerb::Quad) {
                path.quadTo(at(c.c1x, c.c1y), at(c.x, c.y));
            } else {
                path.cubicTo(at(c.c1x, c.c1y), at(c.c2x, c.c2y), at(c.x, c.y));
            }
        }
        if (started) path.close();
        return path.detach();
    };
    const auto fillWith = [&](const SkPath& shape, const std::string& color) {
        SkPaint fill;
        fill.setAntiAlias(true);
        std::uint8_t r = 0, g = 0, b = 0;
        if (!color.empty() && color != "none") parseHex(color, r, g, b);
        // 한글은 그림자 투명도(alpha)를 그리지 않는다(쪽 그림·PDF 모두 불투명).
        fill.setColor(SkColorSetARGB(255, r, g, b));
        canvas->drawPath(shape, fill);
    };
    if (!shadow.empty()) fillWith(build(shadow), obj.artShadowColor.empty() ? "#000000" : obj.artShadowColor);
    const SkPath shape = build(outline);
    fillWith(shape, node.ext().fillColor);
    const BorderEdge& edge = node.ext().borderLeft;
    if (!edge.type.empty() && edge.type != "NONE" && edge.width > 0) {
        SkPaint stroke;
        stroke.setAntiAlias(true);
        stroke.setStyle(SkPaint::kStroke_Style);
        stroke.setStrokeWidth(std::max(1.f, static_cast<float>(edge.width) * scale));
        std::uint8_t sr = 0, sg = 0, sb = 0;
        parseHex(edge.color.empty() ? "#000000" : edge.color, sr, sg, sb);
        stroke.setColor(SkColorSetARGB(255, sr, sg, sb));
        canvas->drawPath(shape, stroke);
    }
}

void Painter::objectBox(const Node& node, std::int32_t x, std::int32_t y) {
    const PaintLayer place = objectPlace(node);
    if (node.ext().obj().shapeType == "textart") {
        if (show(place)) textArt(node, x, y, place);
        return;
    }
    // 호: 채움은 개체 상자 전체, 선은 사분 타원이다(한글 2024 가 그린 PDF 와 같다).
    if (node.ext().obj().shapeType == "arc" && node.ext().obj().points.size() >= 3) {
        if (show(place) && !node.ext().fillColor.empty() && node.ext().fillColor != "none") {
            std::uint8_t fr, fg, fb;
            parseHex(node.ext().fillColor, fr, fg, fb);
            fillRect(bitmap, px(x), px(y), std::max(1, px(x + node.width) - px(x)), std::max(1, px(y + node.height) - px(y)),
                     fr, fg, fb);
        }
        Node outline = node;
        outline.extMut().fillColor.clear();
        drawPoints(outline, hangulArcPoints(node.ext().obj().points), x, y, place);
        return;
    }
    if (node.ext().obj().shapeType == "ellipse" || node.ext().obj().shapeType == "arc") {
        ellipseBox(x, y, node.width, node.height, node.ext().fillColor, node.ext().borderLeft, place);
        return;
    }
    const int ratio = node.ext().obj().cornerRatio > 50 ? 0 : node.ext().obj().cornerRatio;
    if (ratio > 0 && node.ext().obj().points.size() < 2) {
        roundBox(x, y, node.width, node.height, ratio, node.ext().fillColor, node.ext().borderLeft, place);
        return;
    }
    if (node.ext().obj().points.size() >= 2) {
        drawPath(node, x, y, place);
        return;
    }
    if (node.role == "equation") {
        if (show(place)) equation(node, x, y, place);
        return;
    }
    if (node.role == "picture" && !node.ext().obj().binItemId.empty()) {
        if (show(place)) {
            pictureEffects(node, x, y);
            picture(node, x, y);
        }
        if (!node.ext().borderLeft.type.empty() && node.ext().borderLeft.type != "NONE" && node.ext().borderLeft.width > 0) {
            box(x, y, node.width, node.height, "", node.ext().borderLeft, node.role, place);
        }
        return;
    }
    box(x, y, node.width, node.height, node.ext().fillColor, node.ext().borderLeft, node.role, place);
    // 투명 선: 선이 없는 글상자(글이 든 도형) 테두리.
    const BorderEdge& edge = node.ext().borderLeft;
    if (node.role == "shape" && (edge.type.empty() || edge.type == "NONE") && shownMarks().transparent && !placesOnly &&
        show(frameStroke()) && holdsText(node)) {
        strokeTransparent(x, y, x + node.width, y);
        strokeTransparent(x, y + node.height, x + node.width, y + node.height);
        strokeTransparent(x, y, x, y + node.height);
        strokeTransparent(x + node.width, y, x + node.width, y + node.height);
    }
}

}  // namespace chwp::render::paint_detail
