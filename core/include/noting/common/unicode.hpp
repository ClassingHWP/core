#pragma once

#include <string>
#include <string_view>

namespace noting {

void appendUtf8(std::string& out, char32_t codePoint);

// 짝이 맞지 않는 서로게이트는 U+FFFD 로 바꾼다.
std::string utf16ToUtf8(std::u16string_view text);

// UTF-8 을 UTF-16 코드유닛 배열로 변환한다.
std::u16string utf8ToUtf16(std::string_view text);

}  // namespace noting
