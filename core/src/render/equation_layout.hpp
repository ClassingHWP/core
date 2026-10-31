#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace noting::render {

// 수식을 닫을 때 한글이 개체 크기로 넣는 값. 단위는 HWPUNIT.
// baseLine 은 상자 위에서 기준선까지의 높이 비율이다. 0 은 쓰지 않는다(파일에서 0 은 85% 라는 뜻).
struct EquationSize {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t baseLine = 85;
};

// 수식 상자 너비에 쓰는 글꼴 수치. TTF를 열지 않는다.
// advanceWidth 는 유니코드에서 hmtx 진행폭(디자인 단위)이다.
// unitsPerEm, xAvgCharWidth, winCell(usWinAscent + usWinDescent)은 그 글꼴의 가로 픽셀 식에 들어간다.
struct EquationFont {
    std::int32_t unitsPerEm = 0;
    std::int32_t xAvgCharWidth = 0;
    std::int32_t winCell = 0;
    std::unordered_map<char32_t, std::int32_t> advanceWidth;
};

// 스크립트와 baseUnit(1000 = 10pt), 글꼴 맵으로 수식 상자를 구한다. 단위는 HWPUNIT.
// symbol 은 수식 글꼴이다. 기울임 문자와 숫자는 PUA, rm 은 그 글꼴의 ASCII advanceWidth 를 쓴다.
// roman 은 따옴표처럼 수식 글꼴로 바뀌지 않는 글자의 진행폭이다. 비어 있으면 symbol 을 쓴다.
EquationSize measureHangulEquation(std::string_view script, std::int32_t baseUnit, const EquationFont& symbol,
                                   const EquationFont& roman = {});

// 스크립트와 baseUnit(1000 = 10pt)으로 수식 상자의 너비, 높이, 기준선 비율을 구한다. 단위는 HWPUNIT.
// 문자와 숫자는 수식 폰트(HYhwpEQ)다. rm 은 ASCII, it 와 맨 글자, 숫자는 PUA다. it 는 너비를 바꾸지 않는다.
// 폭은 hmtx 진행폭에 lfWidth / xAvgCharWidth를 곱한 픽셀에 75를 곱한다.
// 가로·세로 배율이 같으면 26.6을 픽셀 격자에 올리고, 다르면 1/16픽셀 자리에서 픽셀 아래로 자른다.
// 9pt O는 675, 10pt 기울임 x는 600, 96pt 기울임 x는 5475다.
// 맨 글자 높이는 가장 가까운 픽셀에 75를 곱한 값이다. 1픽셀은 2다.
// 256으로 나눈 나머지가 128이면 2픽셀을 더한다. 96pt는 130픽셀(9750)이다.
// rm 과 it 는 그 높이보다 글자 크기가 크면 글자 크기를 쓴다.
// 글자·숫자 기준선은 수식 상자에만 쓴다. 본문 줄 기준선이 아니다.
// 상자를 세로 한가운데에 놓고 글자 크기의 35%를 더한 뒤, 높이로 나눈 값에 1을 더한다.
// 10pt 맨 글자(975)는 86%, 7pt 맨 글자(675)는 87%, 8pt(825)는 84%다.
// ~ 는 본문 띄어쓰기라 10pt는 500이고, 백틱은 그 1/4이라 10pt는 125다.
// 위첨자는 본문의 68%로 잰 상자이고, 그 아래가 본문 상자의 세로 한가운데에 붙는다.
// 10pt x^2 는 높이 1163, 기준선 89%다.
// 아래첨자는 본문 위를 그대로 두고, 첨자 상자 높이의 반에서 첨자 글자 크기의 20%를 뺀 만큼 아래로 늘린다.
// 10pt x_2 는 높이 1177, 기준선 72%다.
EquationSize measureHangulEquation(std::string_view script, std::int32_t baseUnit,
                                   std::string_view symbolFont = "HYhwpEQ");

}  // namespace noting::render
