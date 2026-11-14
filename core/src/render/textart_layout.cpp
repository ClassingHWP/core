#include "render/textart_layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "chwp/common/unicode.hpp"
#include "export/font_metrics.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

namespace {

struct TextArtWarp {
    int shape;
    bool coons;   // 좌·우 경계도 곡선이다(아니면 위·아래 사이를 v 로 곧게 잇는다)
    bool direct;  // 윤곽 점을 나누지 않고 바로 옮긴다(꺾인 자리에만 점을 넣는다)
    const float* top;
    const float* bottom;
    const float* left;
    const float* right;
    const float* topVertices;  // [u, x, y] × count
    int topVertexCount;
    const float* bottomVertices;
    int bottomVertexCount;
};

#include "render/textart_shapes.inc"

const TextArtWarp* warpOf(int shape) {
    for (const TextArtWarp& warp : kTextArtWarps) {
        if (warp.shape == shape) return &warp;
    }
    return nullptr;
}

struct Pt {
    double x = 0, y = 0;
};

// 101 점 경계에서 매개변수 t(0~1) 자리. 범위 밖은 끝 구간을 늘인다.
Pt sampled(const float* samples, double t) {
    const double at = t * 100.0;
    const int i = std::clamp(static_cast<int>(std::floor(at)), 0, 99);
    const double f = at - i;
    return {samples[i * 2] + (samples[i * 2 + 2] - samples[i * 2]) * f,
            samples[i * 2 + 1] + (samples[i * 2 + 3] - samples[i * 2 + 1]) * f};
}

// 꺾은선 [u, x, y] 에서 u 자리.
Pt polyline(const float* vertices, int count, double u) {
    int i = 0;
    while (i + 2 < count && u > vertices[(i + 1) * 3]) ++i;
    const double u0 = vertices[i * 3];
    const double u1 = vertices[(i + 1) * 3];
    const double f = u1 != u0 ? (u - u0) / (u1 - u0) : 0.0;
    return {vertices[i * 3 + 1] + (vertices[(i + 1) * 3 + 1] - vertices[i * 3 + 1]) * f,
            vertices[i * 3 + 2] + (vertices[(i + 1) * 3 + 2] - vertices[i * 3 + 2]) * f};
}

Pt warpAt(const TextArtWarp& w, double u, double v) {
    Pt top;
    Pt bottom;
    if (w.direct && w.topVertices) {
        top = polyline(w.topVertices, w.topVertexCount, u);
        bottom = polyline(w.bottomVertices, w.bottomVertexCount, u);
    } else {
        top = sampled(w.top, u);
        bottom = sampled(w.bottom, u);
    }
    Pt p{(1 - v) * top.x + v * bottom.x, (1 - v) * top.y + v * bottom.y};
    if (w.coons && w.left && w.right) {
        // 쿤스 면: 네 경계를 섞고 모서리 겹침을 뺀다.
        const Pt left = sampled(w.left, v);
        const Pt right = sampled(w.right, v);
        const Pt t0 = sampled(w.top, 0), t1 = sampled(w.top, 1), b0 = sampled(w.bottom, 0), b1 = sampled(w.bottom, 1);
        p.x += (1 - u) * left.x + u * right.x -
               ((1 - u) * (1 - v) * t0.x + u * (1 - v) * t1.x + (1 - u) * v * b0.x + u * v * b1.x);
        p.y += (1 - u) * left.y + u * right.y -
               ((1 - u) * (1 - v) * t0.y + u * (1 - v) * t1.y + (1 - u) * v * b0.y + u * v * b1.y);
    }
    return p;
}

// 글자 하나: 줄, 그 줄 안 펜 자리(자간을 곱한 진행), 진행, 윤곽(1000 em, y 는 위로, 글자 원점 기준).
struct Placed {
    int line = 0;
    double pen = 0;
    double advance = 0;
    std::vector<GlyphCommand> commands;
};

std::vector<std::u32string> splitLines(const std::string& text) {
    std::vector<std::u32string> lines(1);
    const std::u16string wide = utf8ToUtf16(text);
    for (std::size_t i = 0; i < wide.size(); ++i) {
        char32_t cp = wide[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wide.size() && wide[i + 1] >= 0xDC00 && wide[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (wide[i + 1] - 0xDC00);
            ++i;
        }
        // 한글: CR LF 나 CR 은 줄을 나누고 LF 하나는 나누지 않는다.
        if (cp == '\r') {
            if (i + 1 < wide.size() && wide[i + 1] == '\n') ++i;
            lines.emplace_back();
            continue;
        }
        if (cp == '\n') continue;
        lines.back().push_back(cp);
    }
    return lines;
}

template <typename F>
void forEachPoint(const GlyphCommand& c, F&& fn) {
    fn(c.to);
    if (c.verb == GlyphVerb::Quad || c.verb == GlyphVerb::Cubic) fn(c.c1);
    if (c.verb == GlyphVerb::Cubic) fn(c.c2);
}

}  // namespace

namespace {

// 윤곽의 잉크 상자. 곡선은 제어점이 아니라 곡선 위 극점까지 본다.
struct InkBox {
    double x0 = std::numeric_limits<double>::max(), y0 = std::numeric_limits<double>::max();
    double x1 = std::numeric_limits<double>::lowest(), y1 = std::numeric_limits<double>::lowest();
    void add(double x, double y) {
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
};

// 한 축의 2차 곡선 극점(0<t<1).
void quadExtrema(double a, double b, double c, std::vector<double>& ts) {
    const double den = a - 2 * b + c;
    if (std::abs(den) > 1e-12) {
        const double t = (a - b) / den;
        if (t > 0 && t < 1) ts.push_back(t);
    }
}

// 한 축의 3차 곡선 극점(0<t<1): 도함수 3[(b-a)(1-t)² + 2(c-b)(1-t)t + (d-c)t²] = 0.
void cubicExtrema(double a, double b, double c, double d, std::vector<double>& ts) {
    const double p = b - a, q = c - b, r = d - c;
    const double qa = p - 2 * q + r, qb = 2 * (q - p), qc = p;
    if (std::abs(qa) < 1e-12) {
        if (std::abs(qb) > 1e-12) {
            const double t = -qc / qb;
            if (t > 0 && t < 1) ts.push_back(t);
        }
        return;
    }
    const double disc = qb * qb - 4 * qa * qc;
    if (disc < 0) return;
    const double s = std::sqrt(disc);
    for (const double t : {(-qb + s) / (2 * qa), (-qb - s) / (2 * qa)}) {
        if (t > 0 && t < 1) ts.push_back(t);
    }
}

InkBox inkBox(const std::vector<ArtCommand>& outline) {
    InkBox box;
    double px = 0, py = 0;
    std::vector<double> ts;
    for (const ArtCommand& c : outline) {
        if (c.verb == GlyphVerb::Quad) {
            ts.clear();
            quadExtrema(px, c.c1x, c.x, ts);
            quadExtrema(py, c.c1y, c.y, ts);
            for (const double t : ts) {
                const double u = 1 - t;
                box.add(u * u * px + 2 * u * t * c.c1x + t * t * c.x, u * u * py + 2 * u * t * c.c1y + t * t * c.y);
            }
        } else if (c.verb == GlyphVerb::Cubic) {
            ts.clear();
            cubicExtrema(px, c.c1x, c.c2x, c.x, ts);
            cubicExtrema(py, c.c1y, c.c2y, c.y, ts);
            for (const double t : ts) {
                const double u = 1 - t;
                const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
                box.add(w0 * px + w1 * c.c1x + w2 * c.c2x + w3 * c.x, w0 * py + w1 * c.c1y + w2 * c.c2y + w3 * c.y);
            }
        }
        box.add(c.x, c.y);
        px = c.x;
        py = c.y;
    }
    return box;
}

}  // namespace

ArtShadowFit fitTextArtShadow(const std::vector<ArtCommand>& outline, int offsetXPercent, int offsetYPercent) {
    ArtShadowFit fit;
    if (outline.empty()) return fit;
    const InkBox ink = inkBox(outline);
    const double inkW = ink.x1 - ink.x0;
    const double inkH = ink.y1 - ink.y0;
    if (inkW <= 0 || inkH <= 0) return fit;
    const double dx = offsetXPercent / 100.0;
    const double dy = offsetYPercent / 100.0;
    // 글 칸: 그림자가 오른쪽(아래)이면 왼쪽(위)에, 왼쪽(위)이면 그만큼 밀린다.
    const double left = std::max(0.0, -dx), top = std::max(0.0, -dy);
    const double width = 1 - std::abs(dx), height = 1 - std::abs(dy);
    // 100 % 면 글 칸이 없어 아무것도 안 그린다. 넘으면 칸 폭이 음수라 한글처럼 뒤집혀 그려진다.
    if (width == 0 || height == 0) return fit;
    const auto place = [&](double offX, double offY) {
        std::vector<ArtCommand> out = outline;
        const auto map = [&](float& x, float& y) {
            x = static_cast<float>(left + offX + (x - ink.x0) / inkW * width);
            y = static_cast<float>(top + offY + (y - ink.y0) / inkH * height);
        };
        for (ArtCommand& c : out) {
            map(c.x, c.y);
            map(c.c1x, c.c1y);
            map(c.c2x, c.c2y);
        }
        return out;
    };
    fit.text = place(0, 0);
    fit.shadow = place(dx, dy);
    return fit;
}

std::vector<ArtCommand> textArtOutline(const std::string& text, const std::string& faceIn, const model::TextArt& art) {
    std::vector<ArtCommand> out;
    std::string face = faceIn;
    if (layout::fontUnitsPerEm(face) == 0) face = "함초롬바탕";
    const std::uint16_t units = layout::fontUnitsPerEm(face);
    std::int32_t winAscent = 0, winDescent = 0, hheaAscender = 0, hheaDescender = 0;
    if (units == 0 || !layout::fontWinMetrics(face, winAscent, winDescent)) return out;
    if (!layout::fontHheaMetrics(face, hheaAscender, hheaDescender)) {
        hheaAscender = winAscent;
        hheaDescender = -winDescent;
    }
    const double toEm = 1000.0 / units;
    const double spacing = (art.charSpacing > 0 ? art.charSpacing : 100) / 100.0;
    const double lineRatio = (art.lineSpacing > 0 ? art.lineSpacing : 100) / 100.0;

    // 1. 줄마다 글자를 늘어놓는다(진행에 자간을 곱한다).
    const std::vector<std::u32string> lines = splitLines(text);
    std::vector<Placed> glyphs;
    std::vector<double> widths(lines.size(), 0.0);
    for (std::size_t l = 0; l < lines.size(); ++l) {
        for (const char32_t cp : lines[l]) {
            const std::int32_t design = layout::fontGlyphDesignAdvance(face, cp);
            Placed g;
            g.line = static_cast<int>(l);
            g.pen = widths[l];
            g.advance = (design > 0 ? design * toEm : 500.0) * spacing;
            // 윤곽이 없는 글자(빈칸 등)는 자리만 차지한다.
            if (!paint_detail::faceGlyphCommands(face, cp, g.commands)) g.commands.clear();
            widths[l] += g.advance;
            if (!g.commands.empty()) glyphs.push_back(std::move(g));
        }
    }
    if (glyphs.empty()) return out;
    const int L = static_cast<int>(lines.size());
    const auto alignOffset = [&](double width, double widest) {
        if (art.align == "CENTER") return (widest - width) / 2.0;
        if (art.align == "RIGHT") return widest - width;
        return 0.0;
    };
    const int shape = art.shapeNumber >= 0 ? art.shapeNumber : model::textArtShapeCode(art.shape);

    if (shape >= 48 && shape <= 54) {
        // 2. 원 모양: 글자를 통째로 돌려 원호에 얹는다.
        struct Frame {
            double cx, cy, rx, ry, start, sweep;
            bool outward;
        };
        const double pi = 3.14159265358979323846;
        const auto frame = [&](double cx, double cy, double rx, double ry, double startDeg, double sweepDeg, bool outward) {
            return Frame{cx, cy, rx, ry, startDeg * pi / 180.0, sweepDeg * pi / 180.0, outward};
        };
        std::vector<Frame> frames;
        std::vector<std::vector<int>> groups;  // 틀마다 줄 번호
        std::vector<int> all(static_cast<std::size_t>(L));
        for (int l = 0; l < L; ++l) all[static_cast<std::size_t>(l)] = l;
        const Frame archUp = frame(0.5, 1.0, 0.5, 1.0, 180, 180, true);
        if (shape == 48 || ((shape >= 52) && L == 1)) {
            frames = {archUp};
            groups = {all};
        } else if (shape == 49) {
            frames = {frame(0.5, 0.0, 0.5, 1.0, 180, -180, false)};
            groups = {all};
        } else if (shape == 50) {
            frames = {frame(0.5, 0.5, 0.5, 0.5, 180, 360, true)};
            groups = {all};
        } else if (shape == 51) {
            frames = {frame(0.5, 0.5, 0.5, 0.5, 180, -360, false)};
            groups = {all};
        } else {
            // 52·53·54 여러 줄: 위 반원과 아래 반원. 위는 처음 ceil(L/2) 줄이다(54 는 2~4 줄을 쟀다. 52·53 의
            // 3 줄 이상은 가운데 곧은 줄이 있다고 쟀지만 크기를 맞추지 못해 54 처럼 나눈다).
            const int topCount = (L + 1) / 2;
            groups.resize(2);
            for (int l = 0; l < L; ++l) groups[l < topCount ? 0 : 1].push_back(l);
            frames.push_back(frame(0.5, 0.4875, 0.5, 0.4875, 180, 180, true));
            if (shape == 53) frames.push_back(frame(0.5, 0.5125, 0.5, 0.4875, 0, 180, true));
            else frames.push_back(frame(0.5, 0.5125, 0.5, 0.4875, 180, -180, false));
        }
        // 크기: W 가장 긴 줄, H 글 덩어리 잉크 높이, 고리 두께 T, 가로 배율 s_x.
        double inkTop = -std::numeric_limits<double>::max(), inkBottom = std::numeric_limits<double>::max();
        const double pitch = lineRatio * (hheaAscender - hheaDescender) * toEm;
        std::vector<double> lineInkBottom(static_cast<std::size_t>(L), std::numeric_limits<double>::max());
        for (const Placed& g : glyphs) {
            for (const GlyphCommand& c : g.commands) {
                forEachPoint(c, [&](const GlyphPoint& p) {
                    inkTop = std::max(inkTop, p.y - g.line * pitch);
                    inkBottom = std::min(inkBottom, p.y - g.line * pitch);
                    lineInkBottom[static_cast<std::size_t>(g.line)] =
                        std::min(lineInkBottom[static_cast<std::size_t>(g.line)], static_cast<double>(p.y));
                });
            }
        }
        const double W = *std::max_element(widths.begin(), widths.end());
        const double H = inkTop - inkBottom;
        if (W <= 0 || H <= 0) return out;
        const double dphi = std::fabs(frames.front().sweep);
        const double T = dphi / (L * W / H + dphi);
        const double sx = dphi * (1 - T) / W;
        const double ascentEm = winAscent * toEm;
        for (std::size_t f = 0; f < frames.size(); ++f) {
            const Frame& fr = frames[f];
            const std::vector<int>& group = groups[f];
            if (group.empty()) continue;
            const int Lg = static_cast<int>(group.size());
            double groupWidest = 0;
            for (const int l : group) groupWidest = std::max(groupWidest, widths[static_cast<std::size_t>(l)]);
            if (groupWidest <= 0) continue;
            const double h1 = T / (1 + lineRatio * (Lg - 1));
            const double sign = fr.sweep < 0 ? -1.0 : 1.0;
            for (const Placed& g : glyphs) {
                const auto at = std::find(group.begin(), group.end(), g.line);
                if (at == group.end()) continue;
                const int k = static_cast<int>(at - group.begin());
                const double rk = 1 - k * lineRatio * h1;
                const double sy = h1 / std::max(1.0, ascentEm - lineInkBottom[static_cast<std::size_t>(g.line)]);
                const double offset = alignOffset(widths[static_cast<std::size_t>(g.line)], groupWidest);
                const double phi = fr.start + fr.sweep * (g.pen + offset + g.advance / 2.0) / groupWidest;
                double inkLeft = std::numeric_limits<double>::max(), inkRight = -std::numeric_limits<double>::max();
                for (const GlyphCommand& c : g.commands) {
                    forEachPoint(c, [&](const GlyphPoint& p) {
                        inkLeft = std::min(inkLeft, static_cast<double>(p.x));
                        inkRight = std::max(inkRight, static_cast<double>(p.x));
                    });
                }
                const double center = (inkLeft + inkRight) / 2.0;
                const auto place = [&](const GlyphPoint& p) {
                    const double d = (ascentEm - p.y) * sy;
                    const double r = fr.outward ? rk - d : rk - (h1 - d);
                    const double tau = (p.x - center) * sx;
                    const double qx = r * std::cos(phi) + tau * sign * -std::sin(phi);
                    const double qy = r * std::sin(phi) + tau * sign * std::cos(phi);
                    return Pt{fr.cx + fr.rx * qx, fr.cy + fr.ry * qy};
                };
                for (const GlyphCommand& c : g.commands) {
                    ArtCommand a;
                    a.verb = c.verb;
                    const Pt to = place(c.to);
                    a.x = static_cast<float>(to.x);
                    a.y = static_cast<float>(to.y);
                    if (c.verb == GlyphVerb::Quad || c.verb == GlyphVerb::Cubic) {
                        const Pt c1 = place(c.c1);
                        a.c1x = static_cast<float>(c1.x);
                        a.c1y = static_cast<float>(c1.y);
                    }
                    if (c.verb == GlyphVerb::Cubic) {
                        const Pt c2 = place(c.c2);
                        a.c2x = static_cast<float>(c2.x);
                        a.c2y = static_cast<float>(c2.y);
                    }
                    out.push_back(a);
                }
            }
        }
        return out;
    }

    // 3. 그 밖의 모양: 모든 줄을 한 덩어리로 잉크 상자에 맞춰 (u, v) 로 두고 경계 곡선으로 휜다.
    const double W = *std::max_element(widths.begin(), widths.end());
    const double pitch = lineRatio * (hheaAscender - hheaDescender) * toEm;
    const auto blockX = [&](const Placed& g, const GlyphPoint& p) {
        return g.pen + alignOffset(widths[static_cast<std::size_t>(g.line)], W) + p.x;
    };
    const auto blockY = [&](const Placed& g, const GlyphPoint& p) { return p.y - g.line * pitch; };
    double left = std::numeric_limits<double>::max(), right = -left, top = -left, bottom = left;
    for (const Placed& g : glyphs) {
        for (const GlyphCommand& c : g.commands) {
            forEachPoint(c, [&](const GlyphPoint& p) {
                left = std::min(left, blockX(g, p));
                right = std::max(right, blockX(g, p));
                top = std::max(top, blockY(g, p));
                bottom = std::min(bottom, blockY(g, p));
            });
        }
    }
    if (right <= left || top <= bottom) return out;
    const TextArtWarp* warp = warpOf(shape >= 0 && shape <= 47 ? shape : 0);
    const TextArtWarp* identity = warpOf(42);
    if (!warp) warp = identity;
    if (!warp) return out;
    // 꺾은선 모양은 위·아래 경계의 안쪽 꼭짓점 u 에서만 선을 나눈다.
    std::vector<double> kinks;
    if (warp->direct && warp->topVertices) {
        for (int i = 1; i + 1 < warp->topVertexCount; ++i) kinks.push_back(warp->topVertices[i * 3]);
        for (int i = 1; i + 1 < warp->bottomVertexCount; ++i) kinks.push_back(warp->bottomVertices[i * 3]);
        std::sort(kinks.begin(), kinks.end());
    }
    const auto emit = [&](GlyphVerb verb, double u, double v) {
        const Pt p = warpAt(*warp, u, v);
        ArtCommand a;
        a.verb = verb;
        a.x = static_cast<float>(p.x);
        a.y = static_cast<float>(p.y);
        out.push_back(a);
    };
    for (const Placed& g : glyphs) {
        double prevU = 0, prevV = 0;
        const auto uv = [&](const GlyphPoint& p) {
            return std::pair<double, double>{(blockX(g, p) - left) / (right - left), (top - blockY(g, p)) / (top - bottom)};
        };
        std::vector<GlyphCommand> commands = g.commands;
        // 윤곽은 닫힌다. 마지막 점에서 시작점으로 돌아가는 변도 곧은 선으로 나눠 옮긴다.
        {
            std::vector<GlyphCommand> closed;
            GlyphPoint start{};
            GlyphPoint last{};
            bool any = false;
            for (const GlyphCommand& c : commands) {
                if (c.verb == GlyphVerb::Move) {
                    if (any && (last.x != start.x || last.y != start.y)) closed.push_back(GlyphCommand{GlyphVerb::Line, start});
                    start = c.to;
                    any = true;
                }
                closed.push_back(c);
                last = c.to;
            }
            if (any && (last.x != start.x || last.y != start.y)) closed.push_back(GlyphCommand{GlyphVerb::Line, start});
            commands = std::move(closed);
        }
        for (const GlyphCommand& c : commands) {
            const auto [u, v] = uv(c.to);
            if (c.verb == GlyphVerb::Move) {
                emit(GlyphVerb::Move, u, v);
            } else if (c.verb == GlyphVerb::Line) {
                if (warp->direct) {
                    // 지나는 꺾인 자리에 점을 넣는다.
                    const double lo = std::min(prevU, u), hi = std::max(prevU, u);
                    std::vector<double> cuts;
                    for (const double k : kinks) {
                        if (k > lo && k < hi) cuts.push_back(k);
                    }
                    if (u < prevU) std::reverse(cuts.begin(), cuts.end());
                    for (const double k : cuts) {
                        const double f = (k - prevU) / (u - prevU);
                        emit(GlyphVerb::Line, k, prevV + (v - prevV) * f);
                    }
                    emit(GlyphVerb::Line, u, v);
                } else {
                    // 곧은 선은 (u, v) 에서 잘게 나눠 옮긴다(한글: u 0.0055, v 0.00707 꼴).
                    const int n = std::max({1, static_cast<int>(std::ceil(std::fabs(u - prevU) / 0.0055)),
                                            static_cast<int>(std::ceil(std::fabs(v - prevV) / 0.00707))});
                    for (int i = 1; i <= n; ++i) {
                        const double f = static_cast<double>(i) / n;
                        emit(GlyphVerb::Line, prevU + (u - prevU) * f, prevV + (v - prevV) * f);
                    }
                }
            } else {
                // 곡선은 제어점만 옮겨 곡선으로 둔다.
                ArtCommand a;
                a.verb = c.verb;
                const auto [u1, v1] = uv(c.c1);
                const Pt c1 = warpAt(*warp, u1, v1);
                a.c1x = static_cast<float>(c1.x);
                a.c1y = static_cast<float>(c1.y);
                if (c.verb == GlyphVerb::Cubic) {
                    const auto [u2, v2] = uv(c.c2);
                    const Pt c2 = warpAt(*warp, u2, v2);
                    a.c2x = static_cast<float>(c2.x);
                    a.c2y = static_cast<float>(c2.y);
                }
                const Pt to = warpAt(*warp, u, v);
                a.x = static_cast<float>(to.x);
                a.y = static_cast<float>(to.y);
                out.push_back(a);
            }
            prevU = u;
            prevV = v;
        }
    }
    return out;
}

}  // namespace chwp::render
