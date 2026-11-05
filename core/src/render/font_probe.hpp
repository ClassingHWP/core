#pragma once

// 그리기 쪽 글꼴 글자를 시험에서 보는 창구. 글자 찾기와 윤곽은 painter 안쪽이라 여기서만 꺼내 준다.

#include <cstdint>
#include <string>
#include <vector>

namespace chwp::render {

// 그 글꼴로 그 글자를 찾은 결과의 주소. 같은 글자를 다시 찾으면 같은 주소다. 없으면 nullptr.
const void* probeGlyph(const std::string& face, char32_t cp);
// 그 글자의 윤곽 좌표(1000 em, 동사·점 차례로 펼친 값). 글자가 없으면 비어 있다.
std::vector<std::int32_t> probeOutline(const std::string& face, char32_t cp, int ppem);

}  // namespace chwp::render
