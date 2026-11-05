#include "render/microtex_draw.hpp"

#include "latex.h"

#include "stb_truetype.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"

namespace {

struct Face {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info{};
    bool ok = false;
};

Face& faceFor(const std::string& path) {
    static std::unordered_map<std::string, Face> cache;
    if (const auto it = cache.find(path); it != cache.end()) return it->second;
    Face face;
    std::ifstream in(path, std::ios::binary);
    if (in) {
        face.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (!face.bytes.empty()) face.ok = stbtt_InitFont(&face.info, face.bytes.data(), 0) != 0;
    }
    auto [pos, inserted] = cache.emplace(path, std::move(face));
    (void)inserted;
    return pos->second;
}

std::string resolveFontPath(const std::string& name) {
    const bool hasSep = name.find('/') != std::string::npos || name.find('\\') != std::string::npos;
    const bool hasExt = name.size() >= 4 && (name.ends_with(".ttf") || name.ends_with(".otf") || name.ends_with(".ttc"));
    if (hasSep || hasExt) {
        const bool absolute = !name.empty() && (name[0] == '/' || name[0] == '\\' || (name.size() > 1 && name[1] == ':'));
        if (absolute || !hasSep) return name;
        return tex::LaTeX::getResRootPath() + "/" + name;
    }
    return tex::LaTeX::getResRootPath() + "/fonts/latin/cmr10.ttf";
}

class StbFont : public tex::Font {
public:
    StbFont(std::string path, float size, int style) : path_(std::move(path)), size_(size), style_(style) {}

    float getSize() const override { return size_; }
    tex::sptr<tex::Font> deriveFont(int style) const override {
        return std::make_shared<StbFont>(path_, size_, style);
    }
    bool operator==(const tex::Font& other) const override {
        const auto* font = dynamic_cast<const StbFont*>(&other);
        return font && font->path_ == path_ && font->size_ == size_ && font->style_ == style_;
    }
    bool operator!=(const tex::Font& other) const override { return !(*this == other); }
    bool isBold() const { return (style_ & tex::BOLD) != 0; }
    bool isItalic() const { return (style_ & tex::ITALIC) != 0; }

    const std::string& path() const { return path_; }

private:
    std::string path_;
    float size_;
    int style_;
};

class StbTextLayout : public tex::TextLayout {
public:
    StbTextLayout(std::wstring text, tex::sptr<tex::Font> font) : text_(std::move(text)), font_(std::move(font)) {}

    void getBounds(tex::Rect& bounds) override {
        float width = 0.f;
        float ascent = 0.8f;
        float descent = 0.2f;
        if (const auto* font = dynamic_cast<const StbFont*>(font_.get())) {
            const float size = std::max(1.f, font->getSize());
            ascent = size * 0.8f;
            descent = size * 0.2f;
            Face& face = faceFor(font->path());
            if (face.ok) {
                const float scale = stbtt_ScaleForMappingEmToPixels(&face.info, size);
                int asc = 0;
                int desc = 0;
                int gap = 0;
                stbtt_GetFontVMetrics(&face.info, &asc, &desc, &gap);
                ascent = asc * scale;
                descent = -desc * scale;
                for (wchar_t ch : text_) {
                    int advance = 0;
                    int lsb = 0;
                    stbtt_GetCodepointHMetrics(&face.info, static_cast<int>(ch), &advance, &lsb);
                    width += advance * scale;
                }
            } else {
                width = size * 0.5f * static_cast<float>(text_.size());
            }
        }
        bounds.x = 0.f;
        bounds.y = -ascent;
        bounds.w = width;
        bounds.h = ascent + descent;
    }

    void draw(tex::Graphics2D& g2, float x, float y) override {
        if (!font_) return;
        g2.setFont(font_.get());
        g2.drawText(text_, x, y);
    }

private:
    std::wstring text_;
    tex::sptr<tex::Font> font_;
};

class StbGraphics : public tex::Graphics2D {
public:
    StbGraphics(std::uint8_t* pixels, int width, int height) : pixels_(pixels), width_(width), height_(height) {}
    StbGraphics(SkCanvas* canvas, float originX, float originY) : canvas_(canvas) {
        e_ = originX;
        f_ = originY;
    }

    void setColor(tex::color c) override { color_ = c; }
    tex::color getColor() const override { return color_; }
    void setStroke(const tex::Stroke& stroke) override { stroke_ = stroke; }
    const tex::Stroke& getStroke() const override { return stroke_; }
    void setStrokeWidth(float w) override { stroke_.lineWidth = w; }
    const tex::Font* getFont() const override { return font_; }
    void setFont(const tex::Font* font) override { font_ = font; }
    void translate(float dx, float dy) override {
        e_ += a_ * dx + c_ * dy;
        f_ += b_ * dx + d_ * dy;
    }
    void scale(float sx, float sy) override {
        a_ *= sx;
        b_ *= sx;
        c_ *= sy;
        d_ *= sy;
    }
    void rotate(float angle) override {
        const float co = std::cos(angle);
        const float si = std::sin(angle);
        const float na = a_ * co + c_ * si;
        const float nb = b_ * co + d_ * si;
        const float nc = c_ * co - a_ * si;
        const float nd = d_ * co - b_ * si;
        a_ = na;
        b_ = nb;
        c_ = nc;
        d_ = nd;
    }
    void rotate(float angle, float px, float py) override {
        translate(px, py);
        rotate(angle);
        translate(-px, -py);
    }
    void reset() override {
        a_ = 1.f;
        b_ = 0.f;
        c_ = 0.f;
        d_ = 1.f;
        e_ = 0.f;
        f_ = 0.f;
    }
    float sx() const override { return std::hypot(a_, b_); }
    float sy() const override { return std::hypot(c_, d_); }

    void drawChar(wchar_t ch, float x, float y) override {
        const auto* font = dynamic_cast<const StbFont*>(font_);
        if (!font) return;
        Face& face = faceFor(font->path());
        if (!face.ok) return;
        if (canvas_) {
            drawCharPath(face, ch, x, y, font->getSize());
            return;
        }
        const float pixel = std::max(1.f, font->getSize() * sy());
        const float scale = stbtt_ScaleForMappingEmToPixels(&face.info, pixel);
        int bw = 0;
        int bh = 0;
        int xoff = 0;
        int yoff = 0;
        unsigned char* bitmap = stbtt_GetCodepointBitmap(&face.info, scale, scale, static_cast<int>(ch), &bw, &bh, &xoff, &yoff);
        if (!bitmap) return;
        float ox = 0.f;
        float oy = 0.f;
        map(x, y, ox, oy);
        const float xSign = a_ >= 0.f ? 1.f : -1.f;
        const float ySign = d_ >= 0.f ? 1.f : -1.f;
        const int cr = static_cast<int>((color_ >> 16) & 255);
        const int cg = static_cast<int>((color_ >> 8) & 255);
        const int cb = static_cast<int>(color_ & 255);
        const int ca = static_cast<int>((color_ >> 24) & 255);
        for (int row = 0; row < bh; ++row) {
            for (int col = 0; col < bw; ++col) {
                const int coverage = bitmap[row * bw + col];
                if (coverage == 0 || ca == 0) continue;
                const int px = static_cast<int>(std::floor(ox + xSign * static_cast<float>(xoff + col)));
                const int py = static_cast<int>(std::floor(oy + ySign * static_cast<float>(yoff + row)));
                blend(px, py, cr, cg, cb, ca * coverage / 255);
            }
        }
        stbtt_FreeBitmap(bitmap, nullptr);
    }

    void drawText(const std::wstring& text, float x, float y) override {
        const auto* font = dynamic_cast<const StbFont*>(font_);
        float pen = x;
        for (wchar_t ch : text) {
            drawChar(ch, pen, y);
            pen += advanceOf(font, ch);
        }
    }

    void drawLine(float x1, float y1, float x2, float y2) override {
        float ax = 0.f;
        float ay = 0.f;
        float bx = 0.f;
        float by = 0.f;
        map(x1, y1, ax, ay);
        map(x2, y2, bx, by);
        if (canvas_) {
            const int ca = static_cast<int>((color_ >> 24) & 255);
            if (ca == 0) return;
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeCap(SkPaint::kButt_Cap);
            paint.setStrokeJoin(SkPaint::kBevel_Join);
            paint.setStrokeWidth(std::max(0.6f, stroke_.lineWidth * std::max(sx(), sy())));
            paint.setColor(skColor());
            canvas_->drawLine(ax, ay, bx, by, paint);
            return;
        }
        const float thick = std::max(1.f, stroke_.lineWidth * std::max(sx(), sy()));
        const int radius = std::max(0, static_cast<int>(thick * 0.5f));
        const int x0 = static_cast<int>(std::lround(ax));
        const int y0 = static_cast<int>(std::lround(ay));
        const int x1i = static_cast<int>(std::lround(bx));
        const int y1i = static_cast<int>(std::lround(by));
        const int dx = x1i - x0;
        const int dy = y1i - y0;
        const int steps = std::max(std::abs(dx), std::abs(dy));
        const int cr = static_cast<int>((color_ >> 16) & 255);
        const int cg = static_cast<int>((color_ >> 8) & 255);
        const int cb = static_cast<int>(color_ & 255);
        const int ca = static_cast<int>((color_ >> 24) & 255);
        if (ca == 0) return;
        for (int i = 0; i <= steps; ++i) {
            const int x = steps == 0 ? x0 : x0 + dx * i / steps;
            const int y = steps == 0 ? y0 : y0 + dy * i / steps;
            stamp(x, y, radius, cr, cg, cb, ca);
        }
    }

    void drawRect(float x, float y, float w, float h) override {
        drawLine(x, y, x + w, y);
        drawLine(x + w, y, x + w, y + h);
        drawLine(x + w, y + h, x, y + h);
        drawLine(x, y + h, x, y);
    }
    void fillRect(float x, float y, float w, float h) override { fillMapped(x, y, w, h); }
    void drawRoundRect(float x, float y, float w, float h, float, float) override { drawRect(x, y, w, h); }
    void fillRoundRect(float x, float y, float w, float h, float, float) override { fillRect(x, y, w, h); }

private:
    void map(float x, float y, float& ox, float& oy) const {
        ox = a_ * x + c_ * y + e_;
        oy = b_ * x + d_ * y + f_;
    }

    float advanceOf(const StbFont* font, wchar_t ch) const {
        if (!font) return 0.f;
        const float size = std::max(1.f, font->getSize());
        Face& face = faceFor(font->path());
        if (!face.ok) return size * 0.5f;
        const float scale = stbtt_ScaleForMappingEmToPixels(&face.info, size);
        int advance = 0;
        int lsb = 0;
        stbtt_GetCodepointHMetrics(&face.info, static_cast<int>(ch), &advance, &lsb);
        return advance * scale;
    }

    void blend(int x, int y, int r, int g, int b, int a) {
        if (a <= 0 || x < 0 || y < 0 || x >= width_ || y >= height_ || !pixels_) return;
        std::uint8_t* p = pixels_ + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)) * 4u;
        const int inv = 255 - a;
        p[0] = static_cast<std::uint8_t>((r * a + p[0] * inv) / 255);
        p[1] = static_cast<std::uint8_t>((g * a + p[1] * inv) / 255);
        p[2] = static_cast<std::uint8_t>((b * a + p[2] * inv) / 255);
        p[3] = static_cast<std::uint8_t>(a + p[3] * inv / 255);
    }

    void stamp(int x, int y, int radius, int r, int g, int b, int a) {
        const int rr = radius * radius;
        for (int dy = -radius; dy <= radius; ++dy) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dy * dy > rr) continue;
                blend(x + dx, y + dy, r, g, b, a);
            }
        }
    }

    void fillMapped(float x, float y, float w, float h) {
        float xs[4];
        float ys[4];
        map(x, y, xs[0], ys[0]);
        map(x + w, y, xs[1], ys[1]);
        map(x, y + h, xs[2], ys[2]);
        map(x + w, y + h, xs[3], ys[3]);
        float minX = xs[0];
        float maxX = xs[0];
        float minY = ys[0];
        float maxY = ys[0];
        for (int i = 1; i < 4; ++i) {
            minX = std::min(minX, xs[i]);
            maxX = std::max(maxX, xs[i]);
            minY = std::min(minY, ys[i]);
            maxY = std::max(maxY, ys[i]);
        }
        const int x0 = static_cast<int>(std::floor(minX));
        const int y0 = static_cast<int>(std::floor(minY));
        const int x1 = static_cast<int>(std::ceil(maxX));
        const int y1 = static_cast<int>(std::ceil(maxY));
        const int cr = static_cast<int>((color_ >> 16) & 255);
        const int cg = static_cast<int>((color_ >> 8) & 255);
        const int cb = static_cast<int>(color_ & 255);
        const int ca = static_cast<int>((color_ >> 24) & 255);
        if (canvas_) {
            if (ca == 0) return;
            SkPathBuilder path;
            path.moveTo(xs[0], ys[0]);
            path.lineTo(xs[1], ys[1]);
            path.lineTo(xs[3], ys[3]);
            path.lineTo(xs[2], ys[2]);
            path.close();
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::kFill_Style);
            paint.setColor(skColor());
            canvas_->drawPath(path.detach(), paint);
            return;
        }
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) blend(px, py, cr, cg, cb, ca);
        }
    }

    SkColor skColor() const {
        return SkColorSetARGB(static_cast<U8CPU>((color_ >> 24) & 255), static_cast<U8CPU>((color_ >> 16) & 255),
                              static_cast<U8CPU>((color_ >> 8) & 255), static_cast<U8CPU>(color_ & 255));
    }

    void drawCharPath(Face& face, wchar_t ch, float x, float y, float fontSize) {
        const int ca = static_cast<int>((color_ >> 24) & 255);
        if (ca == 0 || !canvas_) return;
        const float pixel = std::max(1.f, fontSize * sy());
        const float em = stbtt_ScaleForMappingEmToPixels(&face.info, pixel);
        const float xSign = a_ >= 0.f ? 1.f : -1.f;
        const float ySign = d_ >= 0.f ? 1.f : -1.f;
        float penX = 0.f;
        float penY = 0.f;
        map(x, y, penX, penY);
        stbtt_vertex* verts = nullptr;
        const int count = stbtt_GetCodepointShape(&face.info, static_cast<int>(ch), &verts);
        if (count <= 0 || !verts) return;
        const auto at = [&](int vx, int vy) {
            return SkPoint::Make(penX + xSign * static_cast<float>(vx) * em, penY - ySign * static_cast<float>(vy) * em);
        };
        SkPathBuilder path;
        bool open = false;
        for (int i = 0; i < count; ++i) {
            const stbtt_vertex& v = verts[i];
            if (v.type == STBTT_vmove) {
                if (open) path.close();
                path.moveTo(at(v.x, v.y));
                open = true;
            } else if (v.type == STBTT_vline) {
                path.lineTo(at(v.x, v.y));
            } else if (v.type == STBTT_vcurve) {
                const SkPoint c = at(v.cx, v.cy);
                const SkPoint p = at(v.x, v.y);
                path.quadTo(c.fX, c.fY, p.fX, p.fY);
            } else if (v.type == STBTT_vcubic) {
                const SkPoint c1 = at(v.cx, v.cy);
                const SkPoint c2 = at(v.cx1, v.cy1);
                const SkPoint p = at(v.x, v.y);
                path.cubicTo(c1.fX, c1.fY, c2.fX, c2.fY, p.fX, p.fY);
            }
        }
        if (open) path.close();
        stbtt_FreeShape(&face.info, verts);
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(SkPaint::kFill_Style);
        paint.setColor(skColor());
        canvas_->drawPath(path.detach(), paint);
    }

    std::uint8_t* pixels_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    SkCanvas* canvas_ = nullptr;
    tex::color color_ = tex::black;
    tex::Stroke stroke_{};
    const tex::Font* font_ = nullptr;
    float a_ = 1.f;
    float b_ = 0.f;
    float c_ = 0.f;
    float d_ = 1.f;
    float e_ = 0.f;
    float f_ = 0.f;
};

}  // namespace

tex::Font* tex::Font::create(const std::string& file, float size) {
    return new StbFont(file, size, tex::PLAIN);
}

tex::sptr<tex::Font> tex::Font::_create(const std::string& name, int style, float size) {
    return std::make_shared<StbFont>(resolveFontPath(name), size, style);
}

tex::sptr<tex::TextLayout> tex::TextLayout::create(const std::wstring& src, const tex::sptr<tex::Font>& font) {
    return std::make_shared<StbTextLayout>(src, font);
}

namespace chwp::render {

void drawMicrotex(tex::TeXRender& render, std::uint8_t* pixels, int width, int height, float originX, float originY,
                  float scale) {
    StbGraphics graphics(pixels, width, height);
    graphics.translate(originX, originY);
    if (scale != 1.f) graphics.scale(scale, scale);
    render.draw(graphics, 0, 0);
}

void drawMicrotex(tex::TeXRender& render, SkCanvas& canvas, float originX, float originY, float scale) {
    StbGraphics graphics(&canvas, originX, originY);
    if (scale != 1.f) graphics.scale(scale, scale);
    render.draw(graphics, 0, 0);
}

}  // namespace chwp::render
