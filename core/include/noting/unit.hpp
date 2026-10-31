#pragma once

#include <cstdint>

namespace noting {

// mm를 HWPUNIT으로 바꾼다. mm × 283465 / 1000 의 소수점을 버린다.
// 0.25 mm는 70이고, 22.8 mm는 6463이다. 7200 / 25.4 반올림이 아니다.
std::int32_t mmToHu(double mm);

// pt를 HWPUNIT으로 바꾼다. 1 pt는 100이고 소수점을 버린다.
std::int32_t ptToHu(double pt);

// HWPUNIT을 mm로 되돌린다. places는 0부터 2까지이고, 그 자리에서 반올림한다.
double huToMm(std::int32_t hu, int places);

// HWPUNIT을 pt로 되돌린다. places는 0부터 2까지이고, 그 자리에서 반올림한다.
double huToPt(std::int32_t hu, int places);

}  // namespace noting
