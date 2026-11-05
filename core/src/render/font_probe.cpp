#include "render/font_probe.hpp"

#include "render/painter.hpp"

namespace chwp::render {

const void* probeGlyph(const std::string& face, char32_t cp) { return paint_detail::glyphMemory(face, cp); }

std::vector<std::int32_t> probeOutline(const std::string& face, char32_t cp, int ppem) {
    std::vector<std::int32_t> out;
    const paint_detail::GlyphMem* mem = paint_detail::glyphMemory(face, cp);
    if (!mem) return out;
    for (const GlyphCommand& command : paint_detail::hangulCommands(mem, false, false, ppem)) {
        out.push_back(static_cast<std::int32_t>(command.verb));
        for (const GlyphPoint& point : {command.to, command.c1, command.c2}) {
            out.push_back(point.x);
            out.push_back(point.y);
        }
    }
    return out;
}

}  // namespace chwp::render
