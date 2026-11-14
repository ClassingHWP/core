#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace chwp::render {

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
// face 는 정사각 크기에서 힌팅·내장 비트맵 폭(layout::fontGridAdvancePx)을 찾을 글꼴 이름이다. 비면 식만 쓴다.
struct EquationFont {
    std::string face;
    std::int32_t unitsPerEm = 0;
    std::int32_t xAvgCharWidth = 0;
    std::int32_t winCell = 0;
    // usWinAscent·usWinDescent. 대체 글꼴로 잰 글자의 상자 높이(GDI tmHeight + tmInternalLeading)에 쓴다.
    std::int32_t winAscent = 0;
    std::int32_t winDescent = 0;
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
// 이름별 수식 글꼴 폭 표를 비운다. 글꼴이 들어오면 부른다(받기 전에 대체 글꼴 폭으로 채운 표가 남지 않게).
void clearEquationFontCache();

// 글꼴 원본으로 정사각 크기(높이 1~maxHeightPx)마다 힌팅·내장 비트맵이 바꾼 글자 진행폭을 모아
// 폭 정보 파일에 넣을 CHgw 표를 만든다. 힌팅 없는 식과 같은 칸은 넣지 않는다. 원본이 없으면 빈 값이다.
std::vector<std::uint8_t> equationGridTable(std::string_view face, std::int32_t maxHeightPx);

// 수식 안에 놓을 것 하나. 자리는 수식 상자 왼쪽 위에서 잰 HWPUNIT 이고 y 는 아래로 자란다.
// 크기를 잰 판단(measureHangulEquation)이 그대로 낸 자리다. 무엇으로 어떻게 그릴지는 그리는 쪽이 정한다.
struct EquationItem {
    enum class Kind {
        Glyph,    // 글자 하나. x·y·width·height 는 그 글자 칸(진행폭 × 글자 높이)이다
        Rule,     // 선. 분수선, 근호 윗줄, box 테두리. 칸이 곧 칠할 사각형이다
        Fence,    // 늘어나는 울타리. 칸 높이만큼 늘려 그린다. cp 가 0 이면 그리지 않는다
        Radical,  // 근호 기호. 칸 높이만큼 늘려 그린다. 윗줄은 따로 Rule 이다
        Accent,   // 구조가 그리는 장식. 글자 위(아래) 꾸밈 띠(bar·under 는 선, vec·dyad 는 화살표, 그 밖은 기호), not 의 사선, 사다리의 ⋯, 중괄호
    };
    enum class Face {
        Symbol,  // 수식 글꼴(HancomEQN·HYhwpEQ 자리)
        Roman,   // 본문 글꼴. 따옴표 글, 한글, lim
    };
    Kind kind = Kind::Glyph;
    Face face = Face::Symbol;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t em = 0;      // 그 자리의 글자 크기(HWPUNIT, 1000 = 10pt)
    char32_t cp = 0;          // 뜻으로 본 유니코드 글자(기울임 a 도 'a' 다)
    char32_t hwpCode = 0;     // 한글 수식 글꼴에서 잰 코드(사용자 영역일 수 있다). 그릴 때도 이 코드의 윤곽을 쓴다
    bool italic = false;
    bool bold = false;
    bool under = false;       // Accent 가 글자 아래에 붙는다
    std::int32_t raise = 0;   // Glyph: 그릴 때 기준선을 이만큼(HWPUNIT) 올린다
    std::uint32_t color = 0xFF000000u;  // 0xRRGGBB. 맨 위 바이트가 있으면 수식 글자색을 쓴다
};

struct EquationLayout {
    EquationSize size;
    std::vector<EquationItem> items;
};

// 글자 크기 em 에서 수식 글자 칸의 높이(HWPUNIT). 한글이 재는 정수 픽셀 높이다(10pt 975).
std::int32_t equationGlyphCellHeight(std::int32_t em);

// 크기와 놓을 것을 함께 낸다. size 는 measureHangulEquation 과 같다.
EquationLayout layoutHangulEquation(std::string_view script, std::int32_t baseUnit,
                                    std::string_view symbolFont = "HYhwpEQ");

}  // namespace chwp::render
