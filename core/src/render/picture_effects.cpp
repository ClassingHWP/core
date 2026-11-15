#include "render/picture_effects.hpp"

#include <algorithm>
#include <cmath>

// 상자 계산은 한글과 같은 float 차례를 따른다. 한 식에 곱셈과 덧셈을 섞으면 컴파일러가 한 번에 묶어(FMA)
// 끝자리가 달라질 수 있어 단계마다 이름 붙인 float 에 담는다.

namespace chwp::render {
namespace {

constexpr float kPixel = 75.0f;  // 96dpi 한 화소의 HWPUNIT

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
};

// 0 이상 360 미만으로 접은 각도. 한글은 정수 몫으로 접는다.
float foldDegree(float degree) {
    if (degree < 0.0f || degree > 360.0f) {
        const int turns = static_cast<int>(degree) / 360;
        const float whole = static_cast<float>(turns) * 360.0f;
        degree = degree - whole;
        if (degree < 0.0f) degree = degree + 360.0f;
    }
    return degree;
}

float radians(float degree) {
    return static_cast<float>(static_cast<double>(foldDegree(degree)) * 0.017453292519943295);
}

float cosOf(float degree) { return static_cast<float>(std::cos(static_cast<double>(radians(degree)))); }
float sinOf(float degree) { return static_cast<float>(std::sin(static_cast<double>(radians(degree)))); }

struct Point {
    float x = 0, y = 0;
};

// 정렬 0~8 에 따른 그림 사각형의 기준점.
Point anchorOf(const RectF& r, int align) {
    Point p{r.x, r.y};
    if (align == 1 || align == 4 || align == 7) {
        const float half = r.w * 0.5f;
        p.x = half + r.x;
    } else if (align == 2 || align == 5 || align == 8) {
        p.x = r.w + r.x;
    }
    if (align == 3 || align == 4 || align == 5) {
        const float half = r.h * 0.5f;
        p.y = half + r.y;
    } else if (align == 6 || align == 7 || align == 8) {
        p.y = r.h + r.y;
    }
    return p;
}

// 기준점을 두고 기울인 뒤 늘이는 변환. 각도는 효과 값의 반대 방향이다.
struct Affine {
    float a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
};

Affine skewScale(float skewX, float skewY, float scaleX, float scaleY, Point at) {
    const float ca = cosOf(-skewX);
    const float sa = sinOf(-skewX);
    const float cb = cosOf(-skewY);
    const float sb = sinOf(-skewY);
    // 기울이기
    Affine k;
    k.a = cb;
    k.b = -sa;
    const float kc1 = (1.0f - cb) * at.x;
    const float kc2 = at.y * sa;
    k.c = kc1 + kc2;
    k.d = sb;
    k.e = ca;
    const float kf1 = (1.0f - ca) * at.y;
    const float kf2 = at.x * sb;
    k.f = kf1 - kf2;
    // 늘이기를 뒤에 곱한다
    const float sc = (1.0f - scaleX) * at.x;
    const float sf = (1.0f - scaleY) * at.y;
    Affine m;
    m.a = scaleX * k.a;
    m.b = scaleX * k.b;
    const float mc = scaleX * k.c;
    m.c = mc + sc;
    m.d = scaleY * k.d;
    m.e = scaleY * k.e;
    const float mf = scaleY * k.f;
    m.f = mf + sf;
    return m;
}

Point apply(const Affine& m, float x, float y) {
    const float ax = m.a * x;
    const float by = m.b * y;
    const float dx = m.d * x;
    const float ey = m.e * y;
    const float px = ax + by;
    const float py = dx + ey;
    return Point{px + m.c, py + m.f};
}

RectF bounds(const Affine& m, const RectF& r) {
    const float right = r.x + r.w;
    const float bottom = r.y + r.h;
    const Point p[4] = {apply(m, r.x, r.y), apply(m, right, r.y), apply(m, r.x, bottom), apply(m, right, bottom)};
    float x0 = p[0].x, x1 = p[0].x, y0 = p[0].y, y1 = p[0].y;
    for (const Point& q : p) {
        x0 = std::min(x0, q.x);
        x1 = std::max(x1, q.x);
        y0 = std::min(y0, q.y);
        y1 = std::max(y1, q.y);
    }
    return RectF{x0, y0, x1 - x0, y1 - y0};
}

struct Spec {
    int align = 0;
    float skewX = 0, skewY = 0, scaleX = 1, scaleY = 1;
    float direction = 0, distance = 0, blur = 0;
};

float blurPixels(float radius) { return std::ceil(radius / kPixel); }

// 쌓인 상자 box 를 효과 하나로 옮긴 자리. shape 는 그림 사각형(기준점용)이다.
RectF effectRect(const RectF& box, const RectF& shape, const Spec& s) {
    const Affine m = skewScale(s.skewX, s.skewY, s.scaleX, s.scaleY, anchorOf(shape, s.align));
    RectF r = bounds(m, box);
    const float distance = s.distance / kPixel;
    if (distance != 0.0f) {
        const float cx = cosOf(s.direction) * distance;
        const float sy = sinOf(s.direction) * distance;
        r.x = r.x + cx;
        r.y = r.y + sy;
    }
    const float blur = blurPixels(s.blur);
    const float grow = blur + blur;
    r.x = r.x - blur;
    r.y = r.y - blur;
    r.w = r.w + grow;
    r.h = r.h + grow;
    return r;
}

RectF unite(const RectF& a, const RectF& b) {
    const float x0 = std::min(a.x, b.x);
    const float y0 = std::min(a.y, b.y);
    const float ar = a.x + a.w;
    const float br = b.x + b.w;
    const float ab = a.y + a.h;
    const float bb = b.y + b.h;
    return RectF{x0, y0, std::max(ar, br) - x0, std::max(ab, bb) - y0};
}

bool shadowCounts(const model::PictureEffects::Shadow& s) {
    return s.style == 0 && s.scaleX != 0.0f && s.scaleY != 0.0f;
}

Spec specOf(const model::PictureEffects::Shadow& s) {
    return Spec{s.align, s.skewX, s.skewY, s.scaleX, s.scaleY, s.direction, s.distance, s.radius};
}

Spec specOf(const model::PictureEffects::Reflection& r) {
    return Spec{r.align, r.skewX, r.skewY, r.scaleX, r.scaleY, r.direction, r.distance, r.radius};
}

EffectLayer layerOf(const Spec& s, std::int32_t width, std::int32_t height) {
    const RectF shape{0, 0, static_cast<float>(width) / kPixel, static_cast<float>(height) / kPixel};
    const Affine m = skewScale(s.skewX, s.skewY, s.scaleX, s.scaleY, anchorOf(shape, s.align));
    const double distance = s.distance;
    const double angle = static_cast<double>(radians(s.direction));
    EffectLayer layer;
    // 화소 단위 변환을 HWPUNIT 으로 옮긴다. 기울기·배율은 그대로, 옮기는 값만 75 배다.
    layer.transform.a = m.a;
    layer.transform.b = m.b;
    layer.transform.c = static_cast<double>(m.c) * kPixel + std::cos(angle) * distance;
    layer.transform.d = m.d;
    layer.transform.e = m.e;
    layer.transform.f = static_cast<double>(m.f) * kPixel + std::sin(angle) * distance;
    layer.blur = static_cast<double>(blurPixels(s.blur)) * kPixel;
    return layer;
}

}  // namespace

EffectBox pictureEffectBox(const model::PictureEffects& effects, std::int32_t width, std::int32_t height) {
    if (width <= 0 || height <= 0) return {};
    const bool shadow = effects.shadow && shadowCounts(*effects.shadow);
    if (!effects.glow && !shadow && !effects.reflection) return {};
    const RectF shape{0, 0, static_cast<float>(width) / kPixel, static_cast<float>(height) / kPixel};
    RectF box = shape;
    if (effects.glow) {
        const float g = effects.glow->radius / kPixel;
        const float grow = g + g;
        box.x = box.x - g;
        box.y = box.y - g;
        box.w = box.w + grow;
        box.h = box.h + grow;
    }
    if (shadow) box = unite(box, effectRect(box, shape, specOf(*effects.shadow)));
    if (effects.reflection) {
        RectF r = effectRect(box, shape, specOf(*effects.reflection));
        r.x = std::round(r.x);
        r.y = std::round(r.y);
        r.h = r.h * effects.reflection->endPos;
        box = unite(box, r);
    }
    // HWPUNIT 으로 되돌려 네 변을 버린다.
    const float x = box.x * kPixel;
    const float y = box.y * kPixel;
    const float w = box.w * kPixel;
    const float h = box.h * kPixel;
    const float right = x + w;
    const float bottom = y + h;
    EffectBox out;
    out.left = std::max(0, -static_cast<std::int32_t>(x));
    out.top = std::max(0, -static_cast<std::int32_t>(y));
    out.right = std::max(0, static_cast<std::int32_t>(right) - width);
    out.bottom = std::max(0, static_cast<std::int32_t>(bottom) - height);
    return out;
}

EffectLayer shadowLayer(const model::PictureEffects::Shadow& shadow, std::int32_t width, std::int32_t height) {
    return layerOf(specOf(shadow), width, height);
}

EffectLayer reflectionLayer(const model::PictureEffects::Reflection& reflection, std::int32_t width,
                            std::int32_t height) {
    return layerOf(specOf(reflection), width, height);
}

}  // namespace chwp::render
