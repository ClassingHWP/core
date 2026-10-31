#pragma once

#include <cstdint>

namespace tex {
class TeXRender;
}

#if NOTING_HAS_SKIA
class SkCanvas;
#endif

namespace noting::render {

// scale 은 렌더링 크기에 곱한다. origin 은 그 축소 결과의 왼쪽 위다.
void drawMicrotex(tex::TeXRender& render, std::uint8_t* pixels, int width, int height, float originX = 0.f,
                  float originY = 0.f, float scale = 1.f);

#if NOTING_HAS_SKIA
// 수식 윤곽과 줄을 페이지 캔버스에 바로 그린다. origin 은 축소한 수식의 왼쪽 위 픽셀이다.
void drawMicrotex(tex::TeXRender& render, SkCanvas& canvas, float originX, float originY, float scale = 1.f);
#endif

}  // namespace noting::render
