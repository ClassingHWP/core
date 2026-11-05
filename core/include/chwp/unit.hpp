#pragma once

#include <cstdint>
#include <string>

namespace chwp {

// mm를 HWPUNIT으로 바꾼다. mm × 283465 / 1000 의 소수점을 버린다.
// 0.25 mm는 70이고, 22.8 mm는 6463이다. 7200 / 25.4 반올림이 아니다.
std::int32_t mmToHu(double mm);

// pt를 HWPUNIT으로 바꾼다. 1 pt는 100이고 소수점을 버린다.
std::int32_t ptToHu(double pt);

// HWPUNIT을 mm로 되돌린다. places는 0부터 2까지이고, 그 자리에서 반올림한다.
double huToMm(std::int32_t hu, int places);

// HWPUNIT을 pt로 되돌린다. places는 0부터 2까지이고, 그 자리에서 반올림한다.
double huToPt(std::int32_t hu, int places);

// 화면 칸이 고르는 길이 단위. cm 는 mm 규칙을 따르고, inch 는 7200, px 는 75(96 dpi)다.
enum class LengthUnit { Pt, Mm, Cm, Inch, Px };
// pt, mm, cm, inch, px. 모르는 이름이면 pt 다.
LengthUnit lengthUnitOf(const std::string& name);
const char* lengthUnitName(LengthUnit unit);
std::int32_t lengthToHu(double value, LengthUnit unit);
// places는 0부터 2까지이고, 그 자리에서 반올림한다.
double huToLength(std::int32_t hu, LengthUnit unit, int places);

}  // namespace chwp
