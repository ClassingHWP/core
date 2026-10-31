#pragma once

#include <cstdint>
#include <string>
#include <vector>

#if NOTING_HAS_SKIA
class SkCanvas;
#endif

namespace noting::render {

struct EquationRaster {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// 한글 수식을 LaTeX로 바꾼 뒤 MicroTeX로 그린다. 결과는 target 상자 안에 들어가는 크기.
// baselinePercent 는 상자 위에서 기준선까지의 백분율이다. 0보다 작으면 윤곽을 가운데에 둔다.
// 실패하면 빈 래스터. 호출하는 쪽이 주황 자리표시를 유지한다.
EquationRaster rasterHangulEquation(const std::string& script, int targetWidth, int targetHeight,
                                    int baselinePercent = -1);

#if NOTING_HAS_SKIA
// 수식을 비트맵으로 굽지 않고 캔버스에 그린다. 글자 윤곽과 분수선은 Skia가 안티앨리어싱한다.
bool paintHangulEquation(::SkCanvas& canvas, const std::string& script, int x, int y, int w, int h,
                         int baselinePercent = -1);
#endif

void clearEquationRasterCache();

}  // namespace noting::render
