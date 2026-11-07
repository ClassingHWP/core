#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/render/glyph_path.hpp"
#include "render/equation_layout.hpp"

namespace chwp::render {

// 수식을 칠할 것 하나. 좌표는 수식 상자 왼쪽 위에서 잰 HWPUNIT 이고 y 는 아래로 자란다.
struct EquationInk {
    enum class Kind { Path, Rect };
    Kind kind = Kind::Path;
    // Path: 윤곽. 점 하나는 kInkUnit 분의 1 HWPUNIT 이고 y 는 위로 자란다(GlyphCommand 와 같다).
    std::vector<GlyphCommand> commands;
    // Rect: 칠할 사각형.
    float x = 0.f;
    float y = 0.f;
    float width = 0.f;
    float height = 0.f;
    std::uint32_t color = 0xFF000000u;  // 0xAARRGGBB
    // Path: 굵게(bold)일 때 윤곽 둘레에 함께 긋는 선의 굵기(HWPUNIT). 0 이면 칠만 한다.
    float stroke = 0.f;
};

// EquationInk 윤곽 점의 단위. 1 HWPUNIT = 16.
constexpr int kEquationInkUnit = 16;

// 놓을 것(layoutHangulEquation)을 칠할 것으로 바꾼다. 글자는 한글 수식 글꼴(symbolFace) 자리의 윤곽을 한글이
// 그리는 크기·기준선 그대로 긋는다. 원래 글꼴이 없으면 그 자리에는 폴백 글꼴(font/fallback/ClassingHWP Equation)이
// 이어진다. 본문 글꼴 글자(한글·한자·따옴표 글)는 본문 글꼴 윤곽을 쓴다.
// 수식 글꼴 윤곽이 아직 없으면(폭 정보 파일만 있으면) 빈 값이다.
std::vector<EquationInk> equationInk(const EquationLayout& layout, std::string_view symbolFace);

// 같은 수식을 여러 번 그릴 때 놓을 것을 다시 재지 않는다. 결과는 공유된다. size 는 놓을 것을 잰 수식 크기다.
std::shared_ptr<const std::vector<EquationInk>> cachedEquationInk(const std::string& script, std::int32_t baseUnit,
                                                                  const std::string& symbolFace, EquationSize& size);

void clearEquationInkCache();

}  // namespace chwp::render
