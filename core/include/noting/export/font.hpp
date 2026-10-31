#pragma once

#include <string>
#include <vector>

namespace noting::model {
struct Document;
}

namespace noting::layout {

// TTF, OTF, TTC, WOFF2 바이트를 글꼴 이름에 묶는다. WOFF2 는 메모리에서 sfnt 로 푼다.
// 레이아웃은 그 글꼴의 hmtx 진행 폭을 쓴다.
void registerFontFace(std::string face, std::vector<std::uint8_t> bytes);

// 셸이 글꼴 이름을 받는 명령. 주소와 파일 위치는 셸이 알고, 코어는 이름만 보낸다.
// 셸은 바이트가 도착하는 대로 registerFontFace 로 돌려준다.
using FontFaceRequestHandler = void (*)(void* user, const char* const* faces, int count);
void setFontFaceRequestHandler(FontFaceRequestHandler handler, void* user);

// 문서 글꼴 표의 이름과 대체 이름을 셸에 알린다. 핸들이 없으면 아무 일도 하지 않는다.
void requestDocumentFontFaces(const model::Document& doc);

}  // namespace noting::layout
