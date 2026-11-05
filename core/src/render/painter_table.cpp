#include "render/painter.hpp"

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "chwp/common/unicode.hpp"
#include "chwp/reader.hpp"
#include "chwp/render/view.hpp"
#include "render/equation_raster.hpp"

// Painter: 표 테두리와 칸 꾸밈, 대각선

namespace chwp::render::paint_detail {

int Painter::edgeThickPx(const BorderEdge& edge) const {
    if (edge.type.empty() || edge.type == "NONE") return 0;
    return std::max(1, px(edge.width > 0 ? edge.width : 28));
}

int Painter::tableLineKind(const std::string& type) {
    if (type == "SOLID") return 1;
    if (type == "DASH") return 2;
    if (type == "DOT") return 3;
    if (type == "DASH_DOT") return 4;
    if (type == "DASH_DOT_DOT") return 5;
    if (type == "LONG_DASH") return 6;
    if (type == "CIRCLE") return 7;
    if (type == "DOUBLE_SLIM" || type == "WAVE" || type == "DOUBLE_WAVE") return 8;
    if (type == "SLIM_THICK") return 9;
    if (type == "THICK_SLIM") return 10;
    if (type == "SLIM_THICK_SLIM") return 11;
    return 0;
}

int Painter::borderInkPixels(const BorderEdge& edge) const {
    const int kind = tableLineKind(edge.type);
    if (kind == 0) return 0;
    const float widthHu = static_cast<float>(edge.width > 0 ? edge.width : 28);
    const int W = std::max(1, static_cast<int>(std::lround(widthHu * scale)));
    if (kind == 8 || (kind == 11 && W <= 2)) {
        if (W < 2) return W;
        int snapped = W;
        const int rem = snapped & 3;
        if (rem == 1) snapped -= 1;
        else if (rem == 2) snapped += 2;
        else if (rem == 3) snapped += 1;
        return std::max(1, snapped);
    }
    if (kind == 9 || kind == 10) {
        if (W <= 2) return W;
        if (W == 3 || W == 4) return 5;
        return W;
    }
    if (kind == 11) {
        if (W == 3) return 5;
        if (W == 4 || W == 5) return 6;
        return W;
    }
    return W;
}

int Painter::collectStrokeBands(const BorderEdge& edge, StrokeBand* bands) const {
    const int kind = tableLineKind(edge.type);
    if (kind == 0) return 0;
    const float widthHu = static_cast<float>(edge.width > 0 ? edge.width : 28);
    const float stroke = widthHu * scale;
    if (stroke <= 0.f) return 0;
    int count = 0;
    auto add = [&](float offset, float width) {
        if (width <= 0.f || count >= 3) return;
        bands[count++] = StrokeBand{offset, width};
    };
    const int W = std::max(1, static_cast<int>(std::lround(stroke)));
    if (kind == 8 || (kind == 11 && W <= 2)) {
        if (W < 2) {
            add(0.f, static_cast<float>(W));
        } else {
            int snapped = W;
            const int rem = snapped & 3;
            if (rem == 1) snapped -= 1;
            else if (rem == 2) snapped += 2;
            else if (rem == 3) snapped += 1;
            const float s = static_cast<float>(std::max(1, snapped / 4));
            add(-1.5f * s, s);
            add(1.5f * s, s);
        }
    } else if (kind == 9 || kind == 10) {
        if (W <= 2) {
            add(0.f, static_cast<float>(W));
        } else {
            int w = W;
            if (w == 3 || w == 4) w = 5;
            const float thin = static_cast<float>(std::max(w / 4, 1));
            const float thick = static_cast<float>(w) - 2.f * thin;
            const float outerW = kind == 10 ? thick : thin;
            const float innerW = kind == 10 ? thin : thick;
            const float half = static_cast<float>(w) * 0.5f;
            add(-(half - outerW * 0.5f), outerW);
            add(half - innerW * 0.5f, innerW);
        }
    } else if (kind == 11) {
        int w = W;
        if (w == 3) w = 5;
        else if (w == 4 || w == 5) w = 6;
        const float outer = static_cast<float>(std::max(w / 6, 1));
        const float mid = static_cast<float>(w) - 4.f * outer;
        const float half = static_cast<float>(w) * 0.5f;
        add(-(half - outer * 0.5f), outer);
        add(-(half - 2.f * outer - mid * 0.5f), mid);
        add(half - outer * 0.5f, outer);
    } else {
        add(0.f, static_cast<float>(W));
    }
    return count;
}

void Painter::fillBandSpan(bool horizontal, float along0, float along1, float crossCenter, float widthPx, std::uint8_t cr,
                           std::uint8_t cg, std::uint8_t cb) {
    if (std::fabs(along1 - along0) < 0.5f || widthPx <= 0.f) return;
    const int thick = std::max(1, static_cast<int>(std::lround(widthPx)));
    const int a0 = static_cast<int>(std::lround(std::min(along0, along1)));
    const int a1 = static_cast<int>(std::lround(std::max(along0, along1)));
    const int cross = static_cast<int>(std::lround(crossCenter - static_cast<float>(thick) * 0.5f));
    const int span = std::max(1, a1 - a0);
    if (horizontal) fillRect(bitmap, a0, cross, span, thick, cr, cg, cb);
    else fillRect(bitmap, cross, a0, thick, span, cr, cg, cb);
}

bool Painter::sinksBorder() const {
    return borderSink && bitmap.clipL <= 0 && bitmap.clipT <= 0 && bitmap.clipR >= bitmap.width &&
           bitmap.clipB >= bitmap.height;
}

void Painter::mitreTableCorner(std::int32_t hx, std::int32_t hy, int hInside, int vInside, const BorderEdge& edge) {
    const int kind = tableLineKind(edge.type);
    if (kind == 0 || (kind >= 2 && kind <= 7)) return;
    if (sinksBorder()) {
        BorderPiece piece;
        piece.x0 = hx;
        piece.y0 = hy;
        piece.widthHu = edge.width > 0 ? edge.width : 28;
        piece.kind = static_cast<std::uint8_t>(kind);
        piece.inside = static_cast<std::int8_t>(hInside >= 0 ? 1 : -1);
        piece.vInside = static_cast<std::int8_t>(vInside >= 0 ? 1 : -1);
        piece.corner = true;
        parseHex(edge.color.empty() ? "#000000" : edge.color, piece.r, piece.g, piece.b);
        borderSink->push_back(piece);
        return;
    }
    const float cx = static_cast<float>(hx) * scale;
    const float cy = static_cast<float>(hy) * scale;
    StrokeBand bands[3];
    const int count = collectStrokeBands(edge, bands);
    if (count == 0) return;
    std::uint8_t cr, cg, cb;
    parseHex(edge.color.empty() ? "#000000" : edge.color, cr, cg, cb);
    const float hi = static_cast<float>(hInside);
    const float vi = static_cast<float>(vInside);
    for (int i = 0; i < count; ++i) {
        const float offset = bands[i].offset;
        const float half = bands[i].width * 0.5f;
        const float px = cx + vi * offset;
        const float py = cy + hi * offset;
        fillBandSpan(true, cx, px, py, bands[i].width, cr, cg, cb);
        fillBandSpan(false, cy, py, px, bands[i].width, cr, cg, cb);
        const float sx = px - vi * half;
        const float sy = py - hi * half;
        const int left = static_cast<int>(std::lround(std::min(px, sx)));
        const int top = static_cast<int>(std::lround(std::min(py, sy)));
        const int right = static_cast<int>(std::lround(std::max(px, sx)));
        const int bottom = static_cast<int>(std::lround(std::max(py, sy)));
        if (right > left && bottom > top) fillRect(bitmap, left, top, right - left, bottom - top, cr, cg, cb);
    }
}

void Painter::strokeTableBorder(const BorderEdge& edge, std::int32_t hx0, std::int32_t hy0, std::int32_t hx1,
                                std::int32_t hy1, int insideSign) {
    const int kind = tableLineKind(edge.type);
    if (kind == 0 || (hx0 == hx1 && hy0 == hy1)) return;
    if (sinksBorder()) {
        BorderPiece piece;
        piece.x0 = hx0;
        piece.y0 = hy0;
        piece.x1 = hx1;
        piece.y1 = hy1;
        piece.widthHu = edge.width > 0 ? edge.width : 28;
        piece.kind = static_cast<std::uint8_t>(kind);
        piece.inside = static_cast<std::int8_t>(insideSign >= 0 ? 1 : -1);
        parseHex(edge.color.empty() ? "#000000" : edge.color, piece.r, piece.g, piece.b);
        borderSink->push_back(piece);
        return;
    }
    const float widthHu = static_cast<float>(edge.width > 0 ? edge.width : 28);
    const float stroke = widthHu * scale;
    if (stroke <= 0.f) return;
    const float x0 = static_cast<float>(hx0) * scale;
    const float y0 = static_cast<float>(hy0) * scale;
    const float x1 = static_cast<float>(hx1) * scale;
    const float y1 = static_cast<float>(hy1) * scale;
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    if (dx == 0.f && dy == 0.f) return;
    std::uint8_t cr, cg, cb;
    parseHex(edge.color.empty() ? "#000000" : edge.color, cr, cg, cb);
    StrokeBand bands[3];
    const int count = collectStrokeBands(edge, bands);
    if (count == 0) return;
    float intervals[8];
    int dashes = 0;
    const bool dashed = kind >= 2 && kind <= 7;
    if (dashed && kind != 7) {
        BorderEdge probe = edge;
        probe.width = static_cast<std::int32_t>(widthHu);
        dashes = gdiDash(probe, intervals);
    }
    const bool horizontal = std::fabs(dx) >= std::fabs(dy);
    const float mid = horizontal ? (y0 + y1) * 0.5f : (x0 + x1) * 0.5f;
    const float origin = horizontal ? std::min(x0, x1) : std::min(y0, y1);
    const float span = horizontal ? std::fabs(dx) : std::fabs(dy);
    const float insideDir = insideSign >= 0 ? 1.f : -1.f;
    // 마디 시작은 더해 가지 않고 (주기 번호 × 주기 + 주기 안 앞마디 합)으로 바로 잰다.
    // 화면 셰이더가 픽셀마다 같은 식으로 마디를 찾는다. 더해 가면 오차가 쌓여 둘이 갈린다.
    float prefix[8] = {};
    float period = 0.f;
    for (int s = 0; s < dashes; ++s) {
        prefix[s] = period;
        period += intervals[s];
    }
    for (int i = 0; i < count; ++i) {
        const float cross = mid + bands[i].offset * insideDir;
        if (dashes < 2 || !(period > 0.f)) {
            fillBandSpan(horizontal, origin, origin + span, cross, bands[i].width, cr, cg, cb);
            continue;
        }
        for (int k = 0; k < 100000; ++k) {
            const float base = static_cast<float>(k) * period;
            if (base >= span) break;
            for (int s = 0; s < dashes; s += 2) {
                const float cursor = base + prefix[s];
                if (cursor >= span) break;
                const float draw = std::min(intervals[s], span - cursor);
                if (draw > 0.f) {
                    fillBandSpan(horizontal, origin + cursor, origin + cursor + draw, cross, bands[i].width, cr, cg,
                                 cb);
                }
            }
        }
    }
}

void Painter::strokeEdge(const BorderEdge& edge, int ex, int ey, int ew, int eh) {
    // 번호 0과 14 이상은 그리지 않는다. 12·13(물결)은 8번 이중실선이다.
    const int kind = tableLineKind(edge.type);
    if (kind == 0 || ew <= 0 || eh <= 0) return;
    std::uint8_t r, g, b;
    parseHex(edge.color.empty() ? "#000000" : edge.color, r, g, b);
    const std::string& type = edge.type;
    const bool doubled = kind == 8 || type.find("DOUBLE") != std::string::npos || type.find("SLIM") != std::string::npos;
    auto solid = [&](int sx, int sy, int sw, int sh) {
        if (sw > 0 && sh > 0) fillRect(bitmap, sx, sy, sw, sh, r, g, b);
    };
    if (doubled && (ew >= 3 || eh >= 3)) {
        if (ew >= eh) {
            const int band = std::max(1, eh / 3);
            solid(ex, ey, ew, band);
            solid(ex, ey + eh - band, ew, band);
        } else {
            const int band = std::max(1, ew / 3);
            solid(ex, ey, band, eh);
            solid(ex + ew - band, ey, band, eh);
        }
        return;
    }
    int pattern[8] = {};
    const int count = dashPattern(edge, pattern);
    if (count == 0) {
        solid(ex, ey, ew, eh);
        return;
    }
    const bool horizontal = ew >= eh;
    const int length = horizontal ? ew : eh;
    int cursor = 0;
    int slot = 0;
    while (cursor < length && count > 0) {
        const int n = pattern[slot % count];
        const int draw = std::min(n, length - cursor);
        if (slot % 2 == 0) {
            if (horizontal) solid(ex + cursor, ey, draw, eh);
            else solid(ex, ey + cursor, ew, draw);
        }
        cursor += draw;
        ++slot;
    }
}

void Painter::paintColumnRuleSpan(const BorderEdge& edge, std::int32_t mid, std::int32_t top, std::int32_t span) {
    if (!show(frameStroke()) || span <= 0) return;
    if (edge.type.empty() || edge.type == "NONE") return;
    const int thick = edgeThickPx(edge);
    if (thick <= 0) return;
    const int x0 = px(mid) - thick / 2;
    const int y0 = px(top);
    const int y1 = px(top + span);
    strokeEdge(edge, x0, y0, thick, std::max(0, y1 - y0));
}

void Painter::paintColumnRules(const Node& parent, std::int32_t ox, std::int32_t oy) {
    if (!show(frameStroke())) return;
    const auto& kids = parent.children;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        const Node& column = kids[i];
        if (column.kind != NodeKind::Column) continue;
        if (column.ext().borderRight.type.empty() || column.ext().borderRight.type == "NONE") continue;
        const Node* next = nullptr;
        for (std::size_t j = i + 1; j < kids.size(); ++j) {
            const Node& other = kids[j];
            if (other.kind != NodeKind::Column || other.y != column.y || other.x <= column.x) continue;
            next = &other;
            break;
        }
        if (!next) continue;
        const std::int32_t left = column.x + column.width;
        const std::int32_t right = next->x;
        if (right < left) continue;
        const std::int32_t mid = ox + (left + right) / 2;
        const std::int32_t top = oy + column.y;
        const std::int32_t span = std::max(column.height, next->height);
        const std::int32_t halfW = std::max<std::int32_t>(column.ext().borderRight.width / 2, 1);
        for (const auto& seg : columnRuleSpans(pageNode, parent, ox, oy, mid, halfW, top, top + span)) {
            paintColumnRuleSpan(column.ext().borderRight, mid, seg.first, seg.second - seg.first);
        }
    }
}

void Painter::hatchRect(int x0, int y0, int w, int h, const std::string& style, const std::string& color) {
    if (w <= 0 || h <= 0 || style.empty() || style == "NONE") return;
    std::uint8_t r = 0, g = 0, b = 0;
    if (!color.empty()) parseHex(color, r, g, b);
    const int period = std::max(4, px(600));
    const int ax = px(originX);
    const int ay = px(originY);
    const auto mod = [period](int v) { return ((v % period) + period) % period; };
    const bool rows = style == "HORIZONTAL" || style == "CROSS";
    const bool cols = style == "VERTICAL" || style == "CROSS";
    // SLASH 는 왼쪽 위에서 오른쪽 아래로, BACK_SLASH 는 왼쪽 아래에서 오른쪽 위로 긋는다. 이름과 반대다.
    const bool down = style == "SLASH" || style == "CROSS_DIAGONAL";
    const bool up = style == "BACK_SLASH" || style == "CROSS_DIAGONAL";
    if (rows) {
        for (int yy = y0; yy < y0 + h; ++yy) {
            if (mod(yy + ay) == 0) fillRect(bitmap, x0, yy, w, 1, r, g, b);
        }
    }
    if (cols) {
        for (int xx = x0; xx < x0 + w; ++xx) {
            if (mod(xx + ax) == 0) fillRect(bitmap, xx, y0, 1, h, r, g, b);
        }
    }
    if (down || up) {
        for (int yy = y0; yy < y0 + h; ++yy) {
            const int gy = yy + ay;
            for (int xx = x0; xx < x0 + w; ++xx) {
                const int gx = xx + ax;
                if ((down && mod(gx - gy) == 0) || (up && mod(gx + gy) == 0)) fillRect(bitmap, xx, yy, 1, 1, r, g, b);
            }
        }
    }
}

void Painter::cellChrome(const Node& node, std::int32_t x, std::int32_t y, bool borders) {
    if (borders) {
        if (!show(frameStroke())) return;
    } else if (!show(PaintLayer::Behind)) {
        return;
    }
    const int x0 = px(x);
    const int y0 = px(y);
    const int x1 = px(x + node.width);
    const int y1 = px(y + node.height);
    const int tw = std::max(0, x1 - x0);
    const int th = std::max(0, y1 - y0);
    if (!borders) {
    const bool flat = !node.ext().fillColor.empty() && node.ext().fillColor != "none";
    const bool grad = flat && !node.ext().gradColor.empty() && node.ext().gradColor != "none";
    if (grad && tw > 0 && th > 0) {
        std::uint8_t r0, g0, b0, r1, g1, b1;
        parseHex(node.ext().fillColor, r0, g0, b0);
        parseHex(node.ext().gradColor, r1, g1, b1);
        // 0도는 아래에서 위, 시계 방향. 90도는 왼쪽에서 오른쪽이다.
        const int deg = ((node.ext().gradAngle % 360) + 360) % 360;
        const auto mix = [](int a, int b, int t) {
            return static_cast<std::uint8_t>(a + (b - a) * t / 255);
        };
        if (deg == 0 || deg == 180) {
            for (int row = 0; row < th; ++row) {
                int t = th == 1 ? 0 : row * 255 / (th - 1);
                if (deg == 0) t = 255 - t;
                fillRect(bitmap, x0, y0 + row, tw, 1, mix(r0, r1, t), mix(g0, g1, t), mix(b0, b1, t));
            }
        } else if (deg == 90 || deg == 270) {
            for (int col = 0; col < tw; ++col) {
                int t = tw == 1 ? 0 : col * 255 / (tw - 1);
                if (deg == 270) t = 255 - t;
                fillRect(bitmap, x0 + col, y0, 1, th, mix(r0, r1, t), mix(g0, g1, t), mix(b0, b1, t));
            }
        } else {
            const double rad = deg * 3.141592653589793 / 180.0;
            const double dx = std::sin(rad);
            const double dy = -std::cos(rad);
            double minP = 0;
            double maxP = 0;
            const double xs[2] = {0, static_cast<double>(std::max(0, tw - 1))};
            const double ys[2] = {0, static_cast<double>(std::max(0, th - 1))};
            bool first = true;
            for (double cx : xs) {
                for (double cy : ys) {
                    const double p = cx * dx + cy * dy;
                    if (first || p < minP) minP = p;
                    if (first || p > maxP) maxP = p;
                    first = false;
                }
            }
            const double span = maxP - minP;
            for (int row = 0; row < th; ++row) {
                for (int col = 0; col < tw; ++col) {
                    const double p = col * dx + row * dy;
                    int t = span <= 0 ? 0 : static_cast<int>((p - minP) / span * 255.0);
                    if (t < 0) t = 0;
                    if (t > 255) t = 255;
                    fillRect(bitmap, x0 + col, y0 + row, 1, 1, mix(r0, r1, t), mix(g0, g1, t), mix(b0, b1, t));
                }
            }
        }
    } else if (flat && tw > 0 && th > 0) {
        std::uint8_t r, g, b;
        parseHex(node.ext().fillColor, r, g, b);
        fillRect(bitmap, x0, y0, tw, th, r, g, b);
    }
    if (!node.ext().hatchStyle.empty()) hatchRect(x0, y0, tw, th, node.ext().hatchStyle, node.ext().hatchColor);
    if (node.ext().obj().imageFillMode >= 0 && !node.ext().obj().binItemId.empty() && tw > 0 && th > 0) {
        if (const std::shared_ptr<const CachedPicture> image = pictureFor(*doc, node.ext().obj().binItemId)) {
            const int mode = node.ext().obj().imageFillMode;
            const int iw = image->width;
            const int ih = image->height;
            if (iw > 0 && ih > 0 && mode == 5) {
                if (!skiaImage(*image, x0, y0, tw, th, false, false, true)) {
                    for (int row = 0; row < th; ++row) {
                        const int sy = std::min(ih - 1, row * ih / th);
                        for (int col = 0; col < tw; ++col) {
                            const int sx = std::min(iw - 1, col * iw / tw);
                            const unsigned char* s =
                                image->rgba.data() + (static_cast<std::size_t>(sy) * iw + sx) * 4u;
                            blend(bitmap, x0 + col, y0 + row, s[0], s[1], s[2], s[3]);
                        }
                    }
                }
            } else if (iw > 0 && ih > 0) {
                const bool tile = mode >= 0 && mode <= 4;
                int ox = 0;
                int oy = 0;
                if (mode == 6 || mode == 7 || mode == 8) ox = (tw - iw) / 2;
                else if (mode == 12 || mode == 13 || mode == 14) ox = tw - iw;
                if (mode == 6 || mode == 9 || mode == 12) oy = (th - ih) / 2;
                else if (mode == 2 || mode == 8 || mode == 11 || mode == 14) oy = th - ih;
                if (tile) {
                    if (!skiaTile(*image, x0, y0, tw, th)) {
                        for (int row = 0; row < th; ++row) {
                            int sy = row - oy;
                            sy %= ih;
                            if (sy < 0) sy += ih;
                            for (int col = 0; col < tw; ++col) {
                                int sx = col - ox;
                                sx %= iw;
                                if (sx < 0) sx += iw;
                                const unsigned char* s =
                                    image->rgba.data() + (static_cast<std::size_t>(sy) * iw + sx) * 4u;
                                blend(bitmap, x0 + col, y0 + row, s[0], s[1], s[2], s[3]);
                            }
                        }
                    }
                } else {
                    pushClip(x, y, node.width, node.height);
                    const bool drew = skiaImage(*image, x0 + ox, y0 + oy, iw, ih, false, false, false);
                    popClip();
                    if (!drew) {
                        for (int row = 0; row < ih; ++row) {
                            const int dy = y0 + oy + row;
                            if (dy < y0 || dy >= y0 + th) continue;
                            for (int col = 0; col < iw; ++col) {
                                const int dx = x0 + ox + col;
                                if (dx < x0 || dx >= x0 + tw) continue;
                                const unsigned char* s =
                                    image->rgba.data() + (static_cast<std::size_t>(row) * iw + col) * 4u;
                                blend(bitmap, dx, dy, s[0], s[1], s[2], s[3]);
                            }
                        }
                    }
                }
            }
        }
    }
    return;
    }
    const ClipSuspend open(bitmap);
    // 왼쪽, 오른쪽, 위, 아래. 같은 선이 모서리에서 만나면 미터로 잇는다.
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
    cellDiagonals(node, x, y);
}

bool Painter::edgeWins(const BorderEdge& cand, bool candPrimary, const BorderEdge& cur, bool curPrimary) {
    const int candKind = tableLineKind(cand.type);
    const int curKind = tableLineKind(cur.type);
    if (candKind == 0) return false;
    if (curKind == 0) return true;
    if (candKind != curKind) return candKind < curKind;
    const std::int32_t wCand = cand.width > 0 ? cand.width : 28;
    const std::int32_t wCur = cur.width > 0 ? cur.width : 28;
    if (wCand != wCur) return wCand > wCur;
    if (candPrimary != curPrimary) return candPrimary;
    return false;
}

void Painter::drawDiagonalSegment(const BorderEdge& edge, int x0, int y0, int x1, int y1) {
    if (edge.type.empty() || edge.type == "NONE") return;
    std::uint8_t r, g, b;
    parseHex(edge.color.empty() ? "#000000" : edge.color, r, g, b);
    const int thick = edgeThickPx(edge);
    float intervals[8];
    const int dashes = gdiDash(edge, intervals);
    if (SkCanvas* canvas = this->canvas()) {
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kStroke_Style);
        paint.setStrokeWidth(penPx(edge));
        paint.setColor(SkColorSetARGB(255, r, g, b));
        if (dashes >= 2) {
            paint.setPathEffect(SkDashPathEffect::Make(
                SkSpan<const SkScalar>(intervals, static_cast<std::size_t>(dashes)), 0));
        }
        canvas->drawLine(static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1),
                         static_cast<float>(y1), paint);
        return;
    }
    if (dashes < 2) {
        thickLine(x0, y0, x1, y1, thick, r, g, b);
        return;
    }
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int steps = std::max(std::abs(dx), std::abs(dy));
    if (steps <= 0) return;
    int pattern[8];
    for (int i = 0; i < dashes; ++i) pattern[i] = std::max(1, static_cast<int>(std::lround(intervals[i])));
    int cursor = 0;
    int slot = 0;
    while (cursor < steps) {
        const int n = pattern[slot % dashes];
        const int draw = std::min(n, steps - cursor);
        if (slot % 2 == 0) {
            const int ax = x0 + dx * cursor / steps;
            const int ay = y0 + dy * cursor / steps;
            const int bx = x0 + dx * (cursor + draw) / steps;
            const int by = y0 + dy * (cursor + draw) / steps;
            thickLine(ax, ay, bx, by, thick, r, g, b);
        }
        cursor += draw;
        ++slot;
    }
}

void Painter::cellDiagonals(const Node& node, std::int32_t x, std::int32_t y) {
    const BorderEdge& edge = node.ext().borderDiagonal;
    if (edge.type.empty() || edge.type == "NONE") return;
    const bool hasSlash = !node.ext().slash.type.empty() && node.ext().slash.type != "NONE";
    const bool hasBackSlash = !node.ext().backSlash.type.empty() && node.ext().backSlash.type != "NONE";
    const bool hasCenter = !node.ext().centerLine.empty() && node.ext().centerLine != "NONE";
    if (!hasSlash && !hasBackSlash && !hasCenter) return;

    const int x0 = px(x);
    const int y0 = px(y);
    const int x1 = px(x + node.width);
    const int y1 = px(y + node.height);
    if (x1 <= x0 || y1 <= y0) return;
    const int xm = (x0 + x1) / 2;
    const int ym = (y0 + y1) / 2;
    const ClipSuspend open(bitmap);

    auto paintSpec = [&](const model::DiagonalSpec& spec, bool isSlash) {
        if (spec.type.empty() || spec.type == "NONE") return;
        const bool mainLine =
            spec.type == "CENTER" || spec.type == "CENTER_BELOW" || spec.type == "CENTER_ABOVE" || spec.type == "ALL";
        const bool below = spec.type == "CENTER_BELOW" || spec.type == "ALL";
        const bool above = spec.type == "CENTER_ABOVE" || spec.type == "ALL";

        if (mainLine) {
            if (spec.crooked) {
                if (isSlash) {
                    if (!spec.counter) {
                        drawDiagonalSegment(edge, x0, y1, xm, y1);
                        drawDiagonalSegment(edge, xm, y1, x1, y0);
                    } else {
                        drawDiagonalSegment(edge, x0, y1, x1, ym);
                        drawDiagonalSegment(edge, x1, ym, x1, y0);
                    }
                } else {
                    if (!spec.counter) {
                        drawDiagonalSegment(edge, x0, y0, xm, y0);
                        drawDiagonalSegment(edge, xm, y0, x1, y1);
                    } else {
                        drawDiagonalSegment(edge, x0, y0, x0, ym);
                        drawDiagonalSegment(edge, x0, ym, x1, y1);
                    }
                }
            } else {
                if (isSlash) drawDiagonalSegment(edge, x0, y1, x1, y0);
                else drawDiagonalSegment(edge, x0, y0, x1, y1);
            }
        }

        if (below || above) {
            std::vector<int> rSplits;
            std::vector<int> cSplits;
            for (std::int32_t dy : node.ext().obj().spanRowSplits) rSplits.push_back(px(y + dy));
            for (std::int32_t dx : node.ext().obj().spanColSplits) cSplits.push_back(px(x + dx));
            if (rSplits.empty()) rSplits.push_back(ym);
            if (cSplits.empty()) cSplits.push_back(xm);

            if (isSlash) {
                if (above) {
                    for (int sy : rSplits) drawDiagonalSegment(edge, x0, sy, x1, y0);
                    for (int sx : cSplits) drawDiagonalSegment(edge, x0, y1, sx, y0);
                }
                if (below) {
                    for (int sy : rSplits) drawDiagonalSegment(edge, x0, y1, x1, sy);
                    for (int sx : cSplits) drawDiagonalSegment(edge, sx, y1, x1, y0);
                }
            } else {
                if (below) {
                    for (int sy : rSplits) drawDiagonalSegment(edge, x0, sy, x1, y1);
                    for (int sx : cSplits) drawDiagonalSegment(edge, x0, y0, sx, y1);
                }
                if (above) {
                    for (int sy : rSplits) drawDiagonalSegment(edge, x0, y0, x1, sy);
                    for (int sx : cSplits) drawDiagonalSegment(edge, sx, y0, x1, y1);
                }
            }
        }
    };

    paintSpec(node.ext().slash, true);
    paintSpec(node.ext().backSlash, false);

    if (hasCenter) {
        if (node.ext().centerLine == "HORIZONTAL" || node.ext().centerLine == "CROSS") {
            drawDiagonalSegment(edge, x0, ym, x1, ym);
        }
        if (node.ext().centerLine == "VERTICAL" || node.ext().centerLine == "CROSS") {
            drawDiagonalSegment(edge, xm, y0, xm, y1);
        }
    }
}

void Painter::paintTableBorders(const Node& tableNode, std::int32_t ox, std::int32_t oy, std::int32_t stripY0,
                                std::int32_t stripY1, std::int32_t stripX0, std::int32_t stripX1) {
    if (!show(frameStroke())) return;
    std::vector<const Node*> cells;
    std::int32_t maxCellY = tableNode.height;
    std::int32_t maxCellX = tableNode.width;
    for (const Node& child : tableNode.children) {
        if (child.kind == NodeKind::Cell) {
            cells.push_back(&child);
            maxCellY = std::max(maxCellY, child.y + child.height);
            maxCellX = std::max(maxCellX, child.x + child.width);
        }
    }
    if (cells.empty()) return;
    const bool striped = stripY1 >= 0;
    const bool isLastStrip = !striped || stripY1 >= maxCellY;
    const bool stripedX = stripX1 >= 0;
    const bool isLastStripX = !stripedX || stripX1 >= maxCellX;
    // 세로선은 그 x 가 이 띠 안일 때만 긋는다. 이웃 띠 여백에 걸친 선은 그 띠가 긋는다.
    const auto ownsX = [&](std::int32_t x) {
        return !stripedX || (x >= stripX0 && (isLastStripX ? x <= stripX1 : x < stripX1));
    };

    if (tableNode.ext().cellSpacing > 0) {
        for (const Node* c : cells) {
            if (striped && (c->y < stripY0 || (isLastStrip ? (c->y > stripY1) : (c->y >= stripY1)))) continue;
            cellChrome(*c, ox + c->x, oy + c->y, true);
        }
        return;
    }

    std::vector<std::int32_t> xs;
    std::vector<std::int32_t> ys;
    xs.reserve(cells.size() * 2);
    ys.reserve(cells.size() * 2 + 2);
    for (const Node* c : cells) {
        xs.push_back(c->x);
        xs.push_back(c->x + c->width);
        ys.push_back(c->y);
        ys.push_back(c->y + c->height);
    }
    if (striped) {
        ys.push_back(stripY0);
        ys.push_back(stripY1);
    }
    std::sort(xs.begin(), xs.end());
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end()), ys.end());

    struct Seg {
        std::int32_t fixed = 0;
        std::int32_t start = 0;
        std::int32_t end = 0;
        BorderEdge edge;
        bool primary = false;
    };

    std::vector<Seg> hSegs;
    std::vector<Seg> vSegs;

    auto addH = [&](std::int32_t yFixed, std::int32_t xStart, std::int32_t xEnd, const BorderEdge& e,
                    bool isBottom) {
        if (e.type.empty() || e.type == "NONE" || xEnd <= xStart) return;
        if (striped && (yFixed < stripY0 || (isLastStrip ? (yFixed > stripY1) : (yFixed >= stripY1)))) return;
        auto it0 = std::lower_bound(xs.begin(), xs.end(), xStart);
        auto it1 = std::lower_bound(xs.begin(), xs.end(), xEnd);
        for (auto it = it0; it != it1 && (it + 1) != xs.end(); ++it) {
            const std::int32_t s = *it;
            const std::int32_t t = *(it + 1);
            if (t > s) hSegs.push_back(Seg{yFixed, s, t, e, isBottom});
        }
    };

    auto addV = [&](std::int32_t xFixed, std::int32_t yStart, std::int32_t yEnd, const BorderEdge& e,
                    bool isRight) {
        if (e.type.empty() || e.type == "NONE" || yEnd <= yStart || !ownsX(xFixed)) return;
        if (striped) {
            yStart = std::max(yStart, stripY0);
            yEnd = std::min(yEnd, stripY1);
            if (yEnd <= yStart) return;
        }
        auto it0 = std::lower_bound(ys.begin(), ys.end(), yStart);
        auto it1 = std::lower_bound(ys.begin(), ys.end(), yEnd);
        for (auto it = it0; it != it1 && (it + 1) != ys.end(); ++it) {
            const std::int32_t s = *it;
            const std::int32_t t = *(it + 1);
            if (t > s) vSegs.push_back(Seg{xFixed, s, t, e, isRight});
        }
    };

    for (const Node* c : cells) {
        addH(c->y, c->x, c->x + c->width, c->ext().borderTop, false);
        addH(c->y + c->height, c->x, c->x + c->width, c->ext().borderBottom, true);
        addV(c->x, c->y, c->y + c->height, c->ext().borderLeft, false);
        addV(c->x + c->width, c->y, c->y + c->height, c->ext().borderRight, true);
    }

    auto resolveAndMerge = [&](std::vector<Seg>& segs) {
        if (segs.empty()) return;
        std::stable_sort(segs.begin(), segs.end(), [](const Seg& a, const Seg& b) {
            if (a.fixed != b.fixed) return a.fixed < b.fixed;
            if (a.start != b.start) return a.start < b.start;
            return a.end < b.end;
        });
        std::vector<Seg> resolved;
        resolved.reserve(segs.size());
        for (const Seg& s : segs) {
            if (!resolved.empty() && resolved.back().fixed == s.fixed && resolved.back().start == s.start &&
                resolved.back().end == s.end) {
                if (edgeWins(s.edge, s.primary, resolved.back().edge, resolved.back().primary)) {
                    resolved.back() = s;
                }
            } else {
                resolved.push_back(s);
            }
        }
        std::vector<Seg> merged;
        merged.reserve(resolved.size());
        for (const Seg& s : resolved) {
            if (!merged.empty() && merged.back().fixed == s.fixed && merged.back().end == s.start &&
                merged.back().edge.type == s.edge.type && merged.back().edge.width == s.edge.width &&
                merged.back().edge.color == s.edge.color) {
                merged.back().end = s.end;
            } else {
                merged.push_back(s);
            }
        }
        segs = std::move(merged);
    };

    resolveAndMerge(hSegs);
    resolveAndMerge(vSegs);

    const ClipSuspend open(bitmap);
    // 세로를 먼저 긋고 가로가 모서리를 덮는다. 좌표는 칸 모서리 HU다.
    // 가운데로 들어오는 선 때문에 겹선을 자르지 않는다. 관통선은 그대로 두고 나중에 그린 선이 교차를 덮는다.
    for (const Seg& s : vSegs) {
        strokeTableBorder(s.edge, ox + s.fixed, oy + s.start, ox + s.fixed, oy + s.end, s.primary ? -1 : 1);
    }
    for (const Seg& s : hSegs) {
        strokeTableBorder(s.edge, ox + s.start, oy + s.fixed, ox + s.end, oy + s.fixed, s.primary ? -1 : 1);
    }
    for (const Seg& h : hSegs) {
        for (const Seg& v : vSegs) {
            const bool atH = v.fixed == h.start || v.fixed == h.end;
            const bool atV = h.fixed == v.start || h.fixed == v.end;
            if (!atH || !atV) continue;
            if (tableLineKind(h.edge.type) == 0 || tableLineKind(h.edge.type) != tableLineKind(v.edge.type) ||
                borderInkPixels(h.edge) != borderInkPixels(v.edge) || h.edge.color != v.edge.color) {
                continue;
            }
            mitreTableCorner(ox + v.fixed, oy + h.fixed, h.primary ? -1 : 1, v.primary ? -1 : 1, h.edge);
        }
    }
    for (const Node* c : cells) {
        if (striped && (c->y < stripY0 || (isLastStrip ? (c->y > stripY1) : (c->y >= stripY1)))) continue;
        cellDiagonals(*c, ox + c->x, oy + c->y);
    }
}

}  // namespace chwp::render::paint_detail
