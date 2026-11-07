#include "render/equation_draw.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "export/font_metrics.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {
namespace {

using Item = EquationItem;

// 수식 상자 안 HWPUNIT 사각형. y 는 아래로 자란다.
struct HuBox {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    double width() const { return right - left; }
    double height() const { return bottom - top; }
};

bool hasFace(const std::string& face) { return chwp::layout::fontGlyphDesignAdvance(face, U'P') >= 0; }

// 한글 수식 글꼴 이름. HancomEQN 에 없는 글자는 한글도 HyhwpEQ 에서 찾는다([Subst Fonts] HancomEQN → HyhwpEQ).
std::vector<std::string> symbolFaces(std::string_view symbolFace) {
    std::vector<std::string> faces;
    const auto add = [&](std::string face) {
        if (face.empty() || std::find(faces.begin(), faces.end(), face) != faces.end() || !hasFace(face)) return;
        faces.push_back(std::move(face));
    };
    const bool hy = symbolFace.empty() || symbolFace == "HYhwpEQ" || symbolFace == "HyhwpEQ";
    add(hy ? std::string("HyhwpEQ") : std::string(symbolFace));
    add("HyhwpEQ");
    add("HancomEQN");
    return faces;
}

// 울타리를 그리는 글자, 한 글자로 늘릴 수 있는 높이(글자 칸 높이의 배수),
// 가로로 옮기는 몫(종류 3·11 은 글자 크기 1/8 오른쪽, 4·12 는 왼쪽). 표에 없는 뜻은 그 글자를
// 그대로 쓰고 늘리지 않는다(종류 19).
struct FenceDraw {
    char32_t code = 0;
    int cells = 1;
    int shift = 0;  // 글자 크기의 1/8 단위
};

FenceDraw fenceDraw(char32_t meaning) {
    switch (meaning) {
    case U'(': return {0xE044, 3, 0};
    case U')': return {0xE045, 3, 0};
    case U'[': return {0xE049, 2, 1};
    case U']': return {0xE04A, 2, 1};
    case U'{': return {0xE04B, 3, 0};
    case U'}': return {0xE04C, 3, 0};
    case U'|': return {0xE101, 1, 0};
    case U'<': case 0x2329: case 0x27E8: return {0x2329, 1, -1};
    case U'>': case 0x232A: case 0x27E9: return {0x232A, 1, -1};
    default: return {meaning, 1, 0};
    }
}

// 윤곽 점(1000 em, y 위)의 상자.
bool commandBox(const std::vector<GlyphCommand>& commands, double& x0, double& y0, double& x1, double& y1) {
    bool any = false;
    const auto take = [&](const GlyphPoint& p) {
        if (!any) {
            x0 = x1 = p.x;
            y0 = y1 = p.y;
            any = true;
            return;
        }
        x0 = std::min<double>(x0, p.x);
        x1 = std::max<double>(x1, p.x);
        y0 = std::min<double>(y0, p.y);
        y1 = std::max<double>(y1, p.y);
    };
    for (const GlyphCommand& command : commands) {
        take(command.to);
        if (command.verb == GlyphVerb::Quad) take(command.c1);
        if (command.verb == GlyphVerb::Cubic) {
            take(command.c1);
            take(command.c2);
        }
    }
    return any;
}

GlyphPoint inkPoint(double hx, double hy) {
    return {static_cast<int>(std::lround(hx * kEquationInkUnit)), static_cast<int>(std::lround(-hy * kEquationInkUnit))};
}

// 윤곽을 상자 target 에 맞춰 늘린다. 한쪽이 납작한 글자(가로줄·세로줄)는 그 쪽을 다른 쪽 배율로 두고 가운데에 놓는다.
std::vector<GlyphCommand> fitCommands(const std::vector<GlyphCommand>& commands, const HuBox& target) {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::vector<GlyphCommand> out;
    if (!commandBox(commands, x0, y0, x1, y1)) return out;
    const double sw = x1 - x0;
    const double sh = y1 - y0;
    double kx = sw > 1 ? target.width() / sw : 0;
    double ky = sh > 1 ? target.height() / sh : 0;
    if (kx <= 0 && ky <= 0) return out;
    if (kx <= 0) kx = ky;
    if (ky <= 0) ky = kx;
    const double cx = (target.left + target.right) / 2;
    const double cy = (target.top + target.bottom) / 2;
    const double mx = (x0 + x1) / 2;
    const double my = (y0 + y1) / 2;
    const auto map = [&](const GlyphPoint& p) { return inkPoint(cx + (p.x - mx) * kx, cy - (p.y - my) * ky); };
    out.reserve(commands.size());
    for (const GlyphCommand& command : commands) {
        GlyphCommand moved = command;
        moved.to = map(command.to);
        if (command.verb == GlyphVerb::Quad || command.verb == GlyphVerb::Cubic) moved.c1 = map(command.c1);
        if (command.verb == GlyphVerb::Cubic) moved.c2 = map(command.c2);
        out.push_back(moved);
    }
    return out;
}

// 1000 em 윤곽을 기준선 (x, baseline)에서 가로 kx, 세로 ky(1000 em 하나의 HWPUNIT)로 옮긴다.
std::vector<GlyphCommand> placeCommands(const std::vector<GlyphCommand>& commands, double x, double baseline,
                                        double kx, double ky) {
    std::vector<GlyphCommand> out;
    out.reserve(commands.size());
    const auto map = [&](const GlyphPoint& p) { return inkPoint(x + p.x * kx, baseline - p.y * ky); };
    for (const GlyphCommand& command : commands) {
        GlyphCommand moved = command;
        moved.to = map(command.to);
        if (command.verb == GlyphVerb::Quad || command.verb == GlyphVerb::Cubic) moved.c1 = map(command.c1);
        if (command.verb == GlyphVerb::Cubic) moved.c2 = map(command.c2);
        out.push_back(moved);
    }
    return out;
}

// 글자 하나를 한글 수식 글꼴(설치된 원래 글꼴이나 그 자리의 폴백 글꼴)로 긋는다. 한글 2024 와 같다.
// 글꼴은 높이가 글자 크기, 폭(lfWidth)이 그 절반이다. 가로 배율은 폭 ÷ 평균 글자 폭이다.
// U+E044~U+E084·U+E100~U+E105(괄호·선·늘이기 조각)는 칸 위에 맞춰(TA_TOP) 긋고, 그 밖은 칸 위에서 글자 크기
// 85% 아래를 기준선으로 긋는다(TA_BASELINE).
// 울타리는 칸 높이가 한 글자로 늘릴 수 있는 높이보다 낮으면 그 높이의 글꼴로, 아니면 글자 크기 그대로 긋는다.
// 근호는 칸 높이(글자 크기 × 늘림 비율)의 글꼴로 긋는다. 둘 다 폭은 글자 크기 절반 그대로라 세로로만 늘어난다.
// 글꼴에 그 코드가 없으면 Skip, 코드는 있는데 윤곽이 아직 없으면(폭 정보 파일만 왔으면) NotReady 다.
enum class Drawn { Done, Skip, NotReady };

Drawn symbolGlyph(const std::vector<std::string>& faces, char32_t code, const Item& item, double shiftX,
                  std::vector<GlyphCommand>& out, double& fontHeight) {
    for (const std::string& face : faces) {
        if (chwp::layout::fontGlyphDesignAdvance(face, code) < 0) continue;
        std::int32_t ascent = 0;
        std::int32_t descent = 0;
        const std::uint16_t units = chwp::layout::fontUnitsPerEm(face);
        if (!chwp::layout::fontWinMetrics(face, ascent, descent) || ascent + descent <= 0 || units == 0) continue;
        std::vector<GlyphCommand> commands;
        if (!paint_detail::faceGlyphCommands(face, code, commands)) return Drawn::NotReady;
        if (commands.empty()) return Drawn::Skip;  // 공백
        if (item.kind == Item::Kind::Accent) {
            // 장식 그리기는 아직 한글과 맞추지 않았다. 그 글자를 장식 칸에 맞춰 늘린다.
            out = fitCommands(commands, HuBox{static_cast<double>(item.x), static_cast<double>(item.y),
                                              static_cast<double>(item.x + item.width),
                                              static_cast<double>(item.y + item.height)});
            return Drawn::Done;
        }
        const std::int32_t xAvg = chwp::layout::fontXAvgCharWidth(face);
        double fontH = item.em;
        if (item.kind == Item::Kind::Radical) fontH = item.height;
        if (item.kind == Item::Kind::Fence) {
            const std::int32_t limit = fenceDraw(item.cp).cells * equationGlyphCellHeight(item.em);
            if (item.height < limit) fontH = item.height;
        }
        fontHeight = fontH;
        const double ky = fontH / (ascent + descent);
        const double kx = xAvg > 0 ? item.em / 2.0 / xAvg : ky;
        const bool topAligned = (code >= 0xE044 && code <= 0xE084) || (code >= 0xE100 && code <= 0xE105);
        const double baseline = topAligned ? item.y + ascent * ky : item.y + item.em * 0.85 - item.raise;
        // 윤곽은 1000 em 이다. 디자인 단위 배율을 1000 em 배율로 바꾼다.
        const double perEm = units / 1000.0;
        out = placeCommands(commands, item.x + shiftX, baseline, kx * perEm, ky * perEm);
        return Drawn::Done;
    }
    return Drawn::Skip;
}

// 굵게. 한글은 bold 안을 글꼴 굵기 700 으로 긋는다. 수식 글꼴에는 굵은 판이 없어 한글 PDF 는 같은 글꼴을 채우고 윤곽 둘레에 글꼴 높이 2% 굵기의
// 선을 함께 긋는다(2 Tr 1.660 w, 글꼴 크기 83). 그 PDF 는 가로를 42/43(97.67%)로 줄이기도 하는데, 600dpi 장치에서
// 굵은 글꼴 평균 폭이 1 늘어난 몫이라 장치마다 달라 따르지 않는다.
float boldStroke(double fontHeight) { return static_cast<float>(fontHeight * 0.02); }

std::uint32_t inkColor(const Item& item) {
    return (item.color & 0xFF000000u) ? (0xFF000000u | (item.color & 0x00FFFFFFu)) : 0xFF000000u;
}

// 본문 글꼴 글자(한글·한자·따옴표 글·lim). 수식 엔진이 잰 글꼴의 윤곽을 그대로 쓴다.
bool romanInk(const Item& item, EquationInk& ink) {
    static const std::initializer_list<const char*> faces = {"Haansoft Batang", "한컴바탕", "HCR Batang", "함초롬바탕"};
    for (const char* name : faces) {
        const std::string face(name);
        if (!paint_detail::faceGlyphCommands(face, item.cp, ink.commands) || ink.commands.empty()) continue;
        std::int32_t ascent = 0;
        std::int32_t descent = 0;
        const std::uint16_t units = chwp::layout::fontUnitsPerEm(face);
        if (!chwp::layout::fontWinMetrics(face, ascent, descent) || ascent + descent <= 0 || units == 0) continue;
        const double perUnit = static_cast<double>(item.height) / (ascent + descent);
        const std::int32_t advance = chwp::layout::fontGlyphDesignAdvance(face, item.cp);
        const double perUnitX = advance > 0 ? static_cast<double>(item.width) / advance : perUnit;
        // 윤곽은 1000 em 이다. 디자인 단위 배율을 1000 em 배율로 바꾼다.
        const double em = units / 1000.0;
        ink.commands = placeCommands(ink.commands, item.x, item.y + ascent * perUnit, perUnitX * em, perUnit * em);
        return true;
    }
    return false;
}

Drawn appendInk(const Item& item, const std::vector<std::string>& faces, std::vector<EquationInk>& out) {
    EquationInk ink;
    ink.color = inkColor(item);
    if (item.kind == Item::Kind::Rule) {
        ink.kind = EquationInk::Kind::Rect;
        ink.x = static_cast<float>(item.x);
        ink.y = static_cast<float>(item.y);
        ink.width = static_cast<float>(item.width);
        ink.height = static_cast<float>(item.height);
        out.push_back(std::move(ink));
        return Drawn::Done;
    }
    if (item.cp == 0 || item.width <= 0 || item.height <= 0) return Drawn::Skip;
    if (item.kind == Item::Kind::Glyph && item.face == Item::Face::Roman) {
        // 본문 글꼴 글자도 글꼴 높이는 그 칸 높이다(romanInk).
        if (item.bold) ink.stroke = boldStroke(item.height);
        if (romanInk(item, ink)) out.push_back(std::move(ink));
        return Drawn::Done;
    }
    // 울타리는 재는 코드가 아니라 뜻 글자의 그리기 코드다.
    char32_t code = item.hwpCode ? item.hwpCode : item.cp;
    double shiftX = 0;
    if (item.kind == Item::Kind::Fence) {
        const FenceDraw fence = fenceDraw(item.cp);
        code = fence.code;
        shiftX = fence.shift * item.em / 8.0;
    }
    double fontHeight = item.em;
    const Drawn drawn = symbolGlyph(faces, code, item, shiftX, ink.commands, fontHeight);
    if (drawn != Drawn::Done) return drawn;
    if (item.bold) ink.stroke = boldStroke(fontHeight);
    if (!ink.commands.empty()) out.push_back(std::move(ink));
    return Drawn::Done;
}

std::mutex gInkMu;
struct CachedInk {
    EquationSize size;
    std::shared_ptr<const std::vector<EquationInk>> ink;
};
std::list<std::string> gInkOrder;
std::unordered_map<std::string, std::pair<CachedInk, std::list<std::string>::iterator>> gInk;
constexpr std::size_t kInkCacheEntries = 4096;

}  // namespace

std::vector<EquationInk> equationInk(const EquationLayout& layout, std::string_view symbolFace) {
    std::vector<EquationInk> out;
    const std::vector<std::string> faces = symbolFaces(symbolFace);
    for (const Item& item : layout.items) {
        // 수식 글꼴 윤곽이 아직 없으면(웹에서 폭 정보 파일만 왔으면) 그리지 않는다. 글자를 빼먹고 선만 긋지 않는다.
        if (appendInk(item, faces, out) == Drawn::NotReady) return {};
    }
    return out;
}

std::shared_ptr<const std::vector<EquationInk>> cachedEquationInk(const std::string& script, std::int32_t baseUnit,
                                                                  const std::string& symbolFace, EquationSize& size) {
    const std::string key = symbolFace + '\n' + std::to_string(baseUnit) + '\n' + script;
    {
        std::lock_guard lock(gInkMu);
        if (const auto found = gInk.find(key); found != gInk.end()) {
            gInkOrder.splice(gInkOrder.begin(), gInkOrder, found->second.second);
            size = found->second.first.size;
            return found->second.first.ink;
        }
    }
    const EquationLayout layout = layoutHangulEquation(script, baseUnit, symbolFace.empty() ? "HYhwpEQ" : symbolFace);
    CachedInk made{layout.size, std::make_shared<const std::vector<EquationInk>>(equationInk(layout, symbolFace))};
    std::lock_guard lock(gInkMu);
    if (const auto found = gInk.find(key); found != gInk.end()) {
        size = found->second.first.size;
        return found->second.first.ink;
    }
    gInkOrder.push_front(key);
    auto [it, _] = gInk.emplace(key, std::make_pair(std::move(made), gInkOrder.begin()));
    while (gInk.size() > kInkCacheEntries) {
        gInk.erase(gInkOrder.back());
        gInkOrder.pop_back();
    }
    size = it->second.first.size;
    return it->second.first.ink;
}

void clearEquationInkCache() {
    std::lock_guard lock(gInkMu);
    gInk.clear();
    gInkOrder.clear();
}

}  // namespace chwp::render
