// 그림 효과 그리기: 반사 → 그림자 → 네온을 그림 아래에 그리고, 부드러운 가장자리는 그림 가장자리를 흐리게 지운다.
// 자리는 조판과 같은 계산(render/picture_effects.hpp)에서 온다. 그리는 픽셀은 한글과 맞추지 않는다.
// 투명도 값은 한글 그대로다(0 불투명, 1 완전 투명).

#include "render/painter.hpp"
#include "render/picture_effects.hpp"

#include <algorithm>
#include <cmath>

#include "include/core/SkBlendMode.h"
#include "include/core/SkBlurTypes.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkClipOp.h"
#include "include/core/SkColor.h"
#include "include/core/SkImage.h"
#include "include/core/SkMaskFilter.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkSurface.h"
#include "include/effects/SkGradient.h"
#include "include/effects/SkImageFilters.h"

namespace chwp::render::paint_detail {
namespace {

SkColor effectColor(std::uint32_t rgb, float transparency) {
    const float opacity = std::clamp(1.0f - transparency, 0.0f, 1.0f);
    return SkColorSetARGB(static_cast<U8CPU>(std::lround(opacity * 255.0f)), (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF,
                          rgb & 0xFF);
}

SkMatrix matrixOf(const EffectAffine& m) {
    return SkMatrix::MakeAll(static_cast<float>(m.a), static_cast<float>(m.b), static_cast<float>(m.c),
                             static_cast<float>(m.d), static_cast<float>(m.e), static_cast<float>(m.f), 0, 0, 1);
}

// 흐림 반경을 가우스 시그마로. 반경 끝(3 시그마)에서 사라져 조판 상자 안에 든다.
float sigmaOf(double radius) { return static_cast<float>(radius / 3.0); }

sk_sp<SkImage> rasterImage(const CachedPicture& image) {
    if (image.skImage) return image.skImage;
    if (image.rgba.empty() || image.width <= 0 || image.height <= 0) return nullptr;
    const SkImageInfo info =
        SkImageInfo::Make(image.width, image.height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
    const SkPixmap pixmap(info, image.rgba.data(), static_cast<std::size_t>(image.width) * 4u);
    return SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
}

// 반사 한 장. 캔버스는 그림 왼쪽 위가 원점인 HWPUNIT 이다.
void drawReflection(SkCanvas* canvas, const model::PictureEffects::Reflection& r, const sk_sp<SkImage>& image,
                    const SkRect& src, float w, float h, bool flipH, bool flipV) {
    const EffectLayer layer = reflectionLayer(r, static_cast<std::int32_t>(w), static_cast<std::int32_t>(h));
    const float end = std::clamp(r.endPos, 0.0f, 1.0f);
    if (end <= 0.0f) return;
    // 반사는 그림의 기준점 쪽 가장자리부터 끝 위치까지만 보인다. 세로 배율이 음수면 아래 가장자리부터다.
    const bool mirrored = r.scaleY < 0.0f;
    const SkRect shown = mirrored ? SkRect::MakeLTRB(0, h * (1.0f - end), w, h) : SkRect::MakeLTRB(0, 0, w, h * end);
    canvas->save();
    canvas->concat(matrixOf(layer.transform));
    SkPaint layerPaint;
    const float sigma = sigmaOf(layer.blur);
    if (sigma > 0.0f) layerPaint.setImageFilter(SkImageFilters::Blur(sigma, sigma, nullptr));
    const SkRect bounds = shown.makeOutset(sigma * 3.0f, sigma * 3.0f);
    canvas->saveLayer(&bounds, &layerPaint);
    canvas->save();
    canvas->clipRect(shown, true);
    if (flipH || flipV) {
        canvas->translate(flipH ? w : 0.0f, flipV ? h : 0.0f);
        canvas->scale(flipH ? -1.0f : 1.0f, flipV ? -1.0f : 1.0f);
    }
    canvas->drawImageRect(image.get(), src, SkRect::MakeWH(w, h), SkSamplingOptions(SkFilterMode::kLinear), nullptr,
                          SkCanvas::kStrict_SrcRectConstraint);
    canvas->restore();
    // 시작 위치까지는 시작 투명도, 끝 위치에서 끝 투명도가 된다.
    const float startPos = std::clamp(r.startPos, 0.0f, end);
    const float startOpacity = std::clamp(1.0f - r.startAlpha, 0.0f, 1.0f);
    const float endOpacity = std::clamp(1.0f - r.endAlpha, 0.0f, 1.0f);
    const SkPoint pts[2] = {SkPoint::Make(0, mirrored ? h : 0.0f), SkPoint::Make(0, mirrored ? 0.0f : h)};
    SkColor4f colors[3] = {{0, 0, 0, startOpacity}, {0, 0, 0, startOpacity}, {0, 0, 0, endOpacity}};
    float pos[3] = {0.0f, startPos, end};
    std::size_t count = 3;
    if (startPos <= 0.0f) {
        colors[1] = colors[2];
        pos[1] = end;
        count = 2;
    }
    const SkGradient gradient(SkGradient::Colors(SkSpan<const SkColor4f>(colors, count), SkSpan<const float>(pos, count),
                                                 SkTileMode::kClamp),
                              SkGradient::Interpolation{});
    SkPaint mask;
    mask.setBlendMode(SkBlendMode::kDstIn);
    mask.setShader(SkShaders::LinearGradient(pts, gradient));
    canvas->drawRect(shown, mask);
    canvas->restore();
    canvas->restore();
}

}  // namespace

void Painter::pictureEffects(const Node& node, std::int32_t x, std::int32_t y) {
    const model::PictureEffects& fx = node.ext().obj().picture.effects;
    const bool shadow = fx.shadow && fx.shadow->style == 0;
    if (!fx.glow && !shadow && !fx.reflection) return;
    if (node.width <= 0 || node.height <= 0) return;
    SkCanvas* canvas = this->canvas();
    if (!canvas) return;
    const float w = static_cast<float>(node.width);
    const float h = static_cast<float>(node.height);
    canvas->save();
    canvas->translate(static_cast<float>(x) * scale, static_cast<float>(y) * scale);
    canvas->scale(scale, scale);
    // 효과는 그림 바깥에만 그린다(한글 PDF 도 그림 자리를 비운다). 화면이 그림을 따로 얹는 칸에서도 그림을 덮지 않는다.
    canvas->clipRect(SkRect::MakeWH(w, h), SkClipOp::kDifference, false);
    if (fx.reflection) {
        const std::shared_ptr<const CachedPicture> image = pictureFor(*doc, node.ext().obj().binItemId);
        PictureSource source;
        if (image && cropSource(pictureCrop(node.ext().obj().picture), image->width, image->height, source)) {
            if (const sk_sp<SkImage> sk = rasterImage(*image)) {
                const SkRect src = SkRect::MakeLTRB(static_cast<float>(source.srcL), static_cast<float>(source.srcT),
                                                    static_cast<float>(source.srcR), static_cast<float>(source.srcB));
                drawReflection(canvas, *fx.reflection, sk, src, w, h, node.flipHorizontal, node.flipVertical);
            }
        }
    }
    if (shadow) {
        const auto& s = *fx.shadow;
        const EffectLayer layer = shadowLayer(s, node.width, node.height);
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(effectColor(s.color, s.alpha));
        const float sigma = sigmaOf(layer.blur);
        if (sigma > 0.0f) paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma));
        canvas->save();
        canvas->concat(matrixOf(layer.transform));
        canvas->drawRect(SkRect::MakeWH(w, h), paint);
        canvas->restore();
    }
    if (fx.glow && fx.glow->radius > 0.0f) {
        const float r = fx.glow->radius;
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setColor(effectColor(fx.glow->color, fx.glow->alpha));
        // 가장자리에서 진하고 반경 끝에서 사라진다. 넓힌 r/4 와 흐림 3 시그마(3r/4)가 반경 안에 든다.
        paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, r / 4.0f));
        canvas->drawRect(SkRect::MakeWH(w, h).makeOutset(r / 4.0f, r / 4.0f), paint);
    }
    canvas->restore();
}

float Painter::softEdgePx(const Node& node) const {
    const auto& soft = node.ext().obj().picture.effects.softEdge;
    if (!soft || *soft <= 0.0f) return 0.0f;
    const float limit = static_cast<float>(std::min(node.width, node.height)) / 2.0f;
    return std::min(*soft, limit) * scale;
}

void softEdgeMask(SkCanvas* canvas, const SkRect& area, float radius) {
    if (radius <= 0.0f) return;
    // 가리개를 따로 그린 층을 DstIn 으로 합친다. 층에서 비어 있는 곳(가장자리 바깥)은 그림도 지운다.
    SkPaint merge;
    merge.setBlendMode(SkBlendMode::kDstIn);
    canvas->saveLayer(&area, &merge);
    SkPaint mask;
    mask.setAntiAlias(true);
    mask.setColor(SK_ColorBLACK);
    mask.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, radius / 4.0f));
    canvas->drawRect(area.makeInset(radius / 2.0f, radius / 2.0f), mask);
    canvas->restore();
}

bool Painter::softEdgeImage(const CachedPicture& image, int dx, int dy, int dw, int dh, bool flipH, bool flipV,
                            const ImageDraw& draw, float radius) {
    SkCanvas* canvas = this->canvas();
    const sk_sp<SkImage> sk = rasterImage(image);
    if (!canvas || !sk || dw <= 0 || dh <= 0) return false;
    // 그림만 따로 그린 면에 가리개를 씌운 뒤 옮긴다. 바로 그리면 아래에 깔린 것까지 지운다.
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(dw, dh));
    if (!surface) return false;
    SkCanvas* off = surface->getCanvas();
    off->clear(SK_ColorTRANSPARENT);
    SkPaint paint;
    paintPictureTone(paint, draw.tone);
    off->save();
    if (flipH || flipV) {
        off->translate(flipH ? static_cast<float>(dw) : 0.0f, flipV ? static_cast<float>(dh) : 0.0f);
        off->scale(flipH ? -1.0f : 1.0f, flipV ? -1.0f : 1.0f);
    }
    off->drawImageRect(sk.get(), pictureSrc(image.width, image.height, &draw),
                       SkRect::MakeWH(static_cast<float>(dw), static_cast<float>(dh)),
                       SkSamplingOptions(SkCubicResampler::CatmullRom()), &paint, SkCanvas::kStrict_SrcRectConstraint);
    off->restore();
    softEdgeMask(off, SkRect::MakeWH(static_cast<float>(dw), static_cast<float>(dh)), radius);
    canvas->drawImage(surface->makeImageSnapshot(), static_cast<float>(dx), static_cast<float>(dy));
    return true;
}

InnerShadow Painter::innerShadowPx(const Node& node) const {
    InnerShadow out;
    const auto& fx = node.ext().obj().picture.effects;
    if (!fx.shadow || fx.shadow->style != 1) return out;
    const auto& s = *fx.shadow;
    const EffectLayer layer = shadowLayer(s, node.width, node.height);
    const double angle = static_cast<double>(s.direction) * 0.017453292519943295;
    out.dx = static_cast<float>(std::cos(angle) * s.distance) * scale;
    out.dy = static_cast<float>(std::sin(angle) * s.distance) * scale;
    out.sigma = sigmaOf(layer.blur) * scale;
    out.argb = effectColor(s.color, s.alpha);
    if (SkColorGetA(out.argb) == 0) out.argb = 0;
    return out;
}

void drawInnerShadow(SkCanvas* canvas, const SkRect& area, const InnerShadow& shadow, float scaleX, float scaleY) {
    if (!canvas || shadow.argb == 0 || area.isEmpty()) return;
    const float dx = shadow.dx * scaleX;
    const float dy = shadow.dy * scaleY;
    const float sigma = shadow.sigma * (scaleX + scaleY) / 2.0f;
    // 그림 밖을 칠한 판에서 그림 자리를 방향의 반대로 옮겨 뚫는다. 그림자는 방향 쪽 가장자리 안쪽에 진다.
    const float pad = std::abs(dx) + std::abs(dy) + sigma * 3.0f + 1.0f;
    SkPathBuilder frame;
    frame.setFillType(SkPathFillType::kEvenOdd);
    frame.addRect(area.makeOutset(pad, pad));
    frame.addRect(area.makeOffset(-dx, -dy));
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor(shadow.argb);
    if (sigma > 0.0f) paint.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, sigma));
    canvas->save();
    canvas->clipRect(area, true);
    canvas->drawPath(frame.detach(), paint);
    canvas->restore();
}

}  // namespace chwp::render::paint_detail
