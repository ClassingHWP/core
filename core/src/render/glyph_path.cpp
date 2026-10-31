#include "noting/render/glyph_path.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace noting::render {
namespace {

int div3(std::int64_t value) { return static_cast<int>(value / 3); }

void sampleAxis(int p0, int c1, int c2, int p3, int out[20]) {
    const int d = c1 - p0;
    const int b = p0 - 2 * c1 + c2;
    const int a = p3 - p0 + 3 * (c1 - c2);
    const auto q = [](std::int64_t n, std::int64_t den) { return static_cast<int>(n / den); };
    int d1 = q(static_cast<std::int64_t>(d) * 0x18000, 10) + q(static_cast<std::int64_t>(b) * 0xC000, 100) +
             q(static_cast<std::int64_t>(a) * 8192, 1000);
    const int d3 = q(static_cast<std::int64_t>(a) * 0xC000, 1000);
    int d2 = q(static_cast<std::int64_t>(b) * 0x18000, 100) + d3;
    std::int64_t f = static_cast<std::int64_t>(p0) << 16;
    out[0] = p0;
    for (int i = 0; i < 18; ++i) {
        f += d1;
        d1 += d2;
        d2 += d3;
        out[i + 1] = static_cast<int>(f >> 16);
    }
    out[19] = p3;
}

void appendCubic(std::vector<GlyphPoint>& pts, int c1x, int c1y, int c2x, int c2y, int x3, int y3) {
    if (pts.empty()) return;
    int xs[20];
    int ys[20];
    sampleAxis(pts.back().x, c1x, c2x, x3, xs);
    sampleAxis(pts.back().y, c1y, c2y, y3, ys);
    pts.reserve(pts.size() + 20);
    for (int i = 0; i < 20; ++i) pts.push_back({xs[i], ys[i]});
}

void appendQuad(std::vector<GlyphPoint>& pts, int cx, int cy, int ex, int ey) {
    if (pts.empty()) return;
    const int x0 = pts.back().x;
    const int y0 = pts.back().y;
    appendCubic(pts, div3(x0 + 2LL * cx), div3(y0 + 2LL * cy), div3(ex + 2LL * cx), div3(ey + 2LL * cy), ex, ey);
}

void appendLine(std::vector<GlyphPoint>& pts, int x1, int y1) {
    if (pts.empty()) {
        pts.push_back({x1, y1});
        return;
    }
    const int x0 = pts.back().x;
    const int y0 = pts.back().y;
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    int steps = std::max(std::abs(dx), std::abs(dy)) / 20;
    if (steps < 2) steps = 2;
    const int denom = steps - 1;
    int accX = 0;
    int accY = 0;
    for (int i = 0; i < steps; ++i) {
        pts.push_back({x0 + accX / denom, y0 + accY / denom});
        accX += dx;
        accY += dy;
    }
}

std::int16_t readI16(const std::uint8_t* p) {
    std::int16_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

std::uint16_t readU16(const std::uint8_t* p) {
    std::uint16_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

std::uint32_t readU32(const std::uint8_t* p) {
    std::uint32_t value = 0;
    std::memcpy(&value, p, sizeof(value));
    return value;
}

void shearCommand(GlyphCommand& command) {
    command.to.x = gdiItalicX(command.to.x, command.to.y);
    command.c1.x = gdiItalicX(command.c1.x, command.c1.y);
    command.c2.x = gdiItalicX(command.c2.x, command.c2.y);
}

std::int64_t mulFix(std::int64_t a, std::int64_t b) {
    int sign = 1;
    if (a < 0) {
        a = -a;
        sign = -1;
    }
    if (b < 0) {
        b = -b;
        sign = -sign;
    }
    const std::int64_t c = (a * b + 0x8000) >> 16;
    return sign > 0 ? c : -c;
}

std::int64_t mulDiv(std::int64_t a, std::int64_t b, std::int64_t c) {
    if (c == 0) return 0;
    int sign = 1;
    if (a < 0) {
        a = -a;
        sign = -1;
    }
    if (b < 0) {
        b = -b;
        sign = -sign;
    }
    if (c < 0) {
        c = -c;
        sign = -sign;
    }
    const std::int64_t d = (a * b + (c >> 1)) / c;
    return sign > 0 ? d : -d;
}

void normLen(std::int64_t& x, std::int64_t& y, std::int64_t& length) {
    const double len = std::hypot(static_cast<double>(x), static_cast<double>(y));
    if (len == 0) {
        length = 0;
        return;
    }
    x = std::llround(static_cast<double>(x) / len * 65536.0);
    y = std::llround(static_cast<double>(y) / len * 65536.0);
    length = std::llround(len);
}

void emboldenFixed(std::vector<std::vector<std::pair<std::int64_t, std::int64_t>>>& contours, std::int64_t xStrength,
                   std::int64_t yStrength, const std::vector<char>& trueType) {
    xStrength /= 2;
    yStrength /= 2;
    if (xStrength == 0 && yStrength == 0) return;
    for (std::size_t contour = 0; contour < contours.size(); ++contour) {
        auto& pts = contours[contour];
        const bool tt = contour < trueType.size() && trueType[contour] != 0;
        const int count = static_cast<int>(pts.size());
        if (count < 2) continue;
        const int last = count - 1;
        std::int64_t inX = 0;
        std::int64_t inY = 0;
        std::int64_t inLen = 0;
        std::int64_t anchorX = 0;
        std::int64_t anchorY = 0;
        std::int64_t anchorLen = 0;
        int i = last;
        int j = 0;
        int k = -1;
        int guard = 0;
        while (j != i && i != k && guard < count * count + 4) {
            ++guard;
            std::int64_t outX = 0;
            std::int64_t outY = 0;
            std::int64_t outLen = 0;
            if (j != k) {
                outX = pts[static_cast<std::size_t>(j)].first - pts[static_cast<std::size_t>(i)].first;
                outY = pts[static_cast<std::size_t>(j)].second - pts[static_cast<std::size_t>(i)].second;
                normLen(outX, outY, outLen);
                if (outLen == 0) {
                    j = j < last ? j + 1 : 0;
                    continue;
                }
            } else {
                outX = anchorX;
                outY = anchorY;
                outLen = anchorLen;
            }
            if (inLen != 0) {
                if (k < 0) {
                    k = i;
                    anchorX = inX;
                    anchorY = inY;
                    anchorLen = inLen;
                }
                std::int64_t dot = mulFix(inX, outX) + mulFix(inY, outY);
                std::int64_t shiftX = 0;
                std::int64_t shiftY = 0;
                if (dot > -0xF000) {
                    dot += 0x10000;
                    shiftX = inY + outY;
                    shiftY = inX + outX;
                    if (tt) shiftX = -shiftX;
                    else shiftY = -shiftY;
                    std::int64_t cross = mulFix(outX, inY) - mulFix(outY, inX);
                    if (tt) cross = -cross;
                    const std::int64_t limit = std::min(inLen, outLen);
                    if (mulFix(xStrength, cross) <= mulFix(limit, dot)) shiftX = dot ? mulDiv(shiftX, xStrength, dot) : 0;
                    else shiftX = cross ? mulDiv(shiftX, limit, cross) : 0;
                    if (mulFix(yStrength, cross) <= mulFix(limit, dot)) shiftY = dot ? mulDiv(shiftY, yStrength, dot) : 0;
                    else shiftY = cross ? mulDiv(shiftY, limit, cross) : 0;
                }
                while (i != j) {
                    pts[static_cast<std::size_t>(i)].first += xStrength + shiftX;
                    pts[static_cast<std::size_t>(i)].second += yStrength + shiftY;
                    i = i < last ? i + 1 : 0;
                }
            } else {
                i = j;
            }
            inX = outX;
            inY = outY;
            inLen = outLen;
            j = j < last ? j + 1 : 0;
        }
    }
}

struct Slot {
    int command = 0;
    int point = 0;
};

GlyphPoint& commandPoint(GlyphCommand& command, int slot) {
    if (slot == 1) return command.c1;
    if (slot == 2) return command.c2;
    return command.to;
}

const GlyphPoint& commandPoint(const GlyphCommand& command, int slot) {
    if (slot == 1) return command.c1;
    if (slot == 2) return command.c2;
    return command.to;
}

std::int64_t contourArea(const std::vector<GlyphCommand>& commands, const std::vector<Slot>& slots) {
    const GlyphPoint& last = commandPoint(commands[static_cast<std::size_t>(slots.back().command)], slots.back().point);
    std::int64_t area = 0;
    std::int64_t prevX = last.x;
    std::int64_t prevY = last.y;
    for (const Slot& slot : slots) {
        const GlyphPoint& point = commandPoint(commands[static_cast<std::size_t>(slot.command)], slot.point);
        area += (static_cast<std::int64_t>(point.y) - prevY) * (static_cast<std::int64_t>(point.x) + prevX);
        prevX = point.x;
        prevY = point.y;
    }
    return area;
}

// 꼭짓점이 다른 윤곽 안에 홀수 번 들어가면 구멍이다. 무게중심은 바깥과 구멍에 동시에 걸릴 수 있다.
bool pointInContour(const std::vector<GlyphCommand>& commands, const std::vector<Slot>& slots, int x, int y) {
    bool inside = false;
    const int count = static_cast<int>(slots.size());
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const GlyphPoint& a = commandPoint(commands[static_cast<std::size_t>(slots[static_cast<std::size_t>(i)].command)],
                                            slots[static_cast<std::size_t>(i)].point);
        const GlyphPoint& b = commandPoint(commands[static_cast<std::size_t>(slots[static_cast<std::size_t>(j)].command)],
                                            slots[static_cast<std::size_t>(j)].point);
        if ((a.y > y) == (b.y > y)) continue;
        const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
        const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
        const std::int64_t cmp = (static_cast<std::int64_t>(x) - b.x) * dy;
        const std::int64_t cross = dx * (static_cast<std::int64_t>(y) - b.y);
        if ((dy > 0 && cmp < cross) || (dy < 0 && cmp > cross)) inside = !inside;
    }
    return inside;
}

}  // namespace

int gdiItalicX(int x, int y) {
    return x + static_cast<int>((static_cast<std::int64_t>(y) * 0x5700) / 65536);
}

void emboldenGdiOutline(std::vector<GlyphCommand>& commands, int ppem) {
    if (ppem <= 0 || commands.empty()) return;
    const int weighted = 20 * ppem - 10;
    const int yPixels = weighted > 0 ? weighted / 1000 : 0;
    const int xPixels = yPixels + 1;
    std::vector<std::vector<Slot>> groups;
    std::vector<Slot> group;
    for (int index = 0; index < static_cast<int>(commands.size()); ++index) {
        const GlyphCommand& command = commands[static_cast<std::size_t>(index)];
        if (command.verb == GlyphVerb::Move) {
            if (group.size() >= 2) groups.push_back(std::move(group));
            group.clear();
            group.push_back({index, 0});
            continue;
        }
        if (group.empty()) continue;
        if (command.verb == GlyphVerb::Quad) group.push_back({index, 1});
        else if (command.verb == GlyphVerb::Cubic) {
            group.push_back({index, 1});
            group.push_back({index, 2});
        }
        group.push_back({index, 0});
    }
    if (group.size() >= 2) groups.push_back(std::move(group));
    if (groups.empty()) return;

    // 글자 전체 넓이로 방향을 하나만 정하면, 반대 방향인 ㅇ 바깥이 안으로 밀려 획이 얇아진다.
    // 바깥은 넓이 부호대로 키우고, 다른 윤곽 안에 있는 구멍은 반대로 줄인다.
    std::vector<std::int64_t> areas;
    areas.reserve(groups.size());
    bool anyArea = false;
    for (const auto& slots : groups) {
        const std::int64_t area = contourArea(commands, slots);
        if (area != 0) anyArea = true;
        areas.push_back(area);
    }
    if (!anyArea) return;

    std::vector<char> trueType(groups.size(), 0);
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const Slot* probe = nullptr;
        for (const Slot& slot : groups[g]) {
            if (slot.point != 0) continue;
            probe = &slot;
            break;
        }
        int inside = 0;
        if (probe != nullptr) {
            const GlyphPoint& at = commandPoint(commands[static_cast<std::size_t>(probe->command)], probe->point);
            for (std::size_t other = 0; other < groups.size(); ++other) {
                if (other == g) continue;
                if (pointInContour(commands, groups[other], at.x, at.y)) ++inside;
            }
        }
        const bool hole = (inside % 2) == 1;
        const bool negative = areas[g] < 0;
        trueType[g] = static_cast<char>(hole ? !negative : negative);
    }

    std::vector<std::vector<std::pair<std::int64_t, std::int64_t>>> fixed;
    fixed.reserve(groups.size());
    for (const auto& slots : groups) {
        std::vector<std::pair<std::int64_t, std::int64_t>> points;
        points.reserve(slots.size());
        for (const Slot& slot : slots) {
            const GlyphPoint& point = commandPoint(commands[static_cast<std::size_t>(slot.command)], slot.point);
            points.push_back({static_cast<std::int64_t>(point.x) * ppem * 64, static_cast<std::int64_t>(point.y) * ppem * 64});
        }
        fixed.push_back(std::move(points));
    }
    constexpr std::int64_t pixel = 64000;
    emboldenFixed(fixed, static_cast<std::int64_t>(xPixels) * pixel, static_cast<std::int64_t>(yPixels) * pixel, trueType);
    if (yPixels != 0) {
        const std::int64_t scaleX = mulDiv(65536, ppem + 1, ppem + xPixels);
        const std::int64_t scaleY = mulDiv(65536, ppem, ppem + yPixels);
        for (auto& points : fixed) {
            for (auto& point : points) {
                point.first = mulFix(point.first, scaleX);
                point.second = mulFix(point.second, scaleY);
            }
        }
    }
    const std::int64_t den = static_cast<std::int64_t>(ppem) * 64;
    const auto back = [&](std::int64_t value) {
        if (value >= 0) return static_cast<int>((value + den / 2) / den);
        return static_cast<int>((value - den / 2) / den);
    };
    for (std::size_t g = 0; g < groups.size(); ++g) {
        for (std::size_t p = 0; p < groups[g].size(); ++p) {
            const Slot& slot = groups[g][p];
            GlyphPoint& point = commandPoint(commands[static_cast<std::size_t>(slot.command)], slot.point);
            point.x = back(fixed[g][p].first);
            point.y = back(fixed[g][p].second);
        }
    }
}

std::vector<GlyphCommand> styleGlyphCommands(std::vector<GlyphCommand> commands, bool italic, int boldPpem) {
    if (italic) {
        for (GlyphCommand& command : commands) shearCommand(command);
    }
    if (boldPpem > 0) emboldenGdiOutline(commands, boldPpem);
    return commands;
}

std::vector<std::vector<GlyphPoint>> flattenHangulGlyph(std::vector<GlyphCommand> commands, bool italic, int boldPpem) {
    commands = styleGlyphCommands(std::move(commands), italic, boldPpem);
    std::vector<std::vector<GlyphPoint>> out;
    std::vector<GlyphPoint> contour;
    auto flush = [&]() {
        if (contour.size() >= 2) out.push_back(std::move(contour));
        contour.clear();
    };
    for (const GlyphCommand& command : commands) {
        switch (command.verb) {
        case GlyphVerb::Move:
            flush();
            contour.push_back(command.to);
            break;
        case GlyphVerb::Line:
            appendLine(contour, command.to.x, command.to.y);
            break;
        case GlyphVerb::Quad:
            appendQuad(contour, command.c1.x, command.c1.y, command.to.x, command.to.y);
            break;
        case GlyphVerb::Cubic:
            appendCubic(contour, command.c1.x, command.c1.y, command.c2.x, command.c2.y, command.to.x, command.to.y);
            break;
        }
    }
    flush();
    return out;
}

std::vector<GlyphCommand> flattenHangulCommands(std::vector<GlyphCommand> commands) {
    const std::vector<std::vector<GlyphPoint>> contours = flattenHangulGlyph(std::move(commands), false, 0);
    std::vector<GlyphCommand> out;
    for (const std::vector<GlyphPoint>& contour : contours) {
        if (contour.empty()) continue;
        out.push_back({GlyphVerb::Move, contour.front(), {}, {}});
        for (std::size_t i = 1; i < contour.size(); ++i) out.push_back({GlyphVerb::Line, contour[i], {}, {}});
    }
    return out;
}

std::vector<GlyphCommand> commandsFromGgoNative(const std::uint8_t* data, int size) {
    std::vector<GlyphCommand> commands;
    if (!data || size < 16) return commands;
    int pos = 0;
    while (pos + 16 <= size) {
        const int cb = static_cast<int>(readU32(data + pos));
        if (cb < 16 || pos + cb > size) break;
        commands.push_back({GlyphVerb::Move, {readI16(data + pos + 10), readI16(data + pos + 14)}, {}, {}});
        int cur = pos + 16;
        int left = cb - 16;
        while (left > 0 && cur + 4 <= pos + cb) {
            const int type = readU16(data + cur);
            const int count = readU16(data + cur + 2);
            const int bytes = count * 8 + 4;
            if (count < 0 || bytes > left || cur + bytes > size) break;
            const std::uint8_t* p = data + cur + 4;
            if (type == 1) {
                for (int i = 0; i < count; ++i) {
                    commands.push_back(
                        {GlyphVerb::Line, {readI16(p + i * 8 + 2), readI16(p + i * 8 + 6)}, {}, {}});
                }
            } else if (type == 2) {
                for (int i = 0; i + 1 < count; ++i) {
                    const int cx = readI16(p + i * 8 + 2);
                    const int cy = readI16(p + i * 8 + 6);
                    int ex = 0;
                    int ey = 0;
                    if (i + 1 == count - 1) {
                        ex = readI16(p + (i + 1) * 8 + 2);
                        ey = readI16(p + (i + 1) * 8 + 6);
                    } else {
                        const int nx = readI16(p + (i + 1) * 8 + 2);
                        const int ny = readI16(p + (i + 1) * 8 + 6);
                        ex = (cx + nx) / 2;
                        ey = (cy + ny) / 2;
                    }
                    commands.push_back({GlyphVerb::Quad, {ex, ey}, {cx, cy}, {}});
                }
            }
            cur += bytes;
            left -= bytes;
        }
        pos += cb;
    }
    return commands;
}

}  // namespace noting::render
