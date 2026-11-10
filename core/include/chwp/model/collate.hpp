#pragma once

// Windows 한국어(로캘 0x412) 정렬 비교. 한글 2024 표 정렬은 Windows 한국어 정렬 차례로 글을 견주므로, 같은 결과를
// 내도록 LCMapStringW(LCMAP_SORTKEY) 와 같은 정렬 키를 만든다. 키를 바이트 차례로 견주면 CompareStringW 결과와 같다.
// 글자마다의 키 조각과 옛한글 낱자 합침 표는 Windows 정렬 키에서 뽑아 넣었다.

#include <string>
#include <string_view>

namespace chwp::model {

enum class CollateFlags {
    // NORM_IGNORECASE | NORM_IGNORENONSPACE | NORM_IGNOREKANATYPE | NORM_IGNOREWIDTH (0x30003). 한글 글자 정렬 1차.
    IgnoreCaseAccentKanaWidth,
    // NORM_IGNORECASE | SORT_STRINGSORT (0x1001). 한글 글자 정렬 2차(_wcsicoll).
    IgnoreCaseString,
    // SORT_STRINGSORT (0x1000). 한글 숫자 정렬에서 수가 아닌 글끼리(wcscoll).
    String,
};

// text 의 정렬 키(LCMapStringW 와 같은 바이트, 끝의 0 포함).
std::string windowsSortKey(std::u16string_view text, CollateFlags flags);
// CompareStringW 와 같은 비교. 음수·0·양수.
int windowsCompare(std::u16string_view a, std::u16string_view b, CollateFlags flags);

}  // namespace chwp::model
