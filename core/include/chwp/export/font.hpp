#pragma once

#include <string>
#include <vector>

namespace chwp::model {
struct Document;
}

namespace chwp::layout {

// TTF, OTF, TTC, WOFF2 바이트를 글꼴 이름에 묶는다. WOFF2 는 메모리에서 sfnt 로 푼다.
// 레이아웃은 그 글꼴의 hmtx 진행 폭을 쓴다.
void registerFontFace(std::string face, std::vector<std::uint8_t> bytes);

// 셸이 글꼴 이름을 받는 명령. 주소와 파일 위치는 셸이 알고, 코어는 이름만 보낸다.
// 셸은 바이트가 도착하는 대로 registerFontFace 로 돌려준다.
using FontFaceRequestHandler = void (*)(void* user, const char* const* faces, int count);
void setFontFaceRequestHandler(FontFaceRequestHandler handler, void* user);

// 문서가 쓰는 글꼴을 셸에 알린다. 핸들이 없으면 아무 일도 하지 않는다. 목록은 documentFontFaces 다.
void requestDocumentFontFaces(const model::Document& doc);

// 문서 글자를 그리는 데 쓰는 글꼴 이름. 글꼴 표 전체가 아니라 글자에 실제로 쓰인 글꼴만 넣는다.
// 조판과 같은 칸을 고른다: 본문은 한글 음절이면 한글 칸, 아니면 영문 칸이고, 문단 머리는 hancomCharLang 칸이다.
// 굵게 쓰는 글꼴은 굵은 짝 이름을, 파일이 없는 글꼴이 있으면 폭·그리기를 대신하는 함초롬바탕을,
// 사용자 영역 글자가 있으면 그 기호 대체(같은 계열 함초롬, Wingdings)를 함께 넣는다.
std::vector<std::string> documentFontFaces(const model::Document& doc);
// 수식 하나를 재고 그리는 글꼴(받을 차례). 고른 수식 글꼴(비면 HyhwpEQ), 그 글꼴에 없는 글자를 찾는 HyhwpEQ·HancomEQN,
// 그리고 수식 글꼴에 없는 글자(한글 자모·한자 등)와 로만 글자(숫자 등)를 재는 한컴바탕(Haansoft Batang)이다.
// 한컴바탕은 한글이 수식의 모든 언어 글꼴 칸에 쓰는 이름이다(render/equation_layout).
std::vector<std::string> equationFontFaces(const std::string& chosen);
// 셸 글꼴 목록에 있어 받을 수 있는데 아직 올라오지 않은 글꼴인지. 목록이 없으면(이 기기 글꼴만 쓰면) 기다릴 것이 없다.
bool fontPending(const std::string& face);

// 받은 글꼴에 없는 문서 글자가 있으면 그 자리의 대체 글꼴(문서 대체, 같은 계열 함초롬, 함초롬바탕) 중
// 아직 올라오지 않은 이름. 받은 글꼴로만 판단하고 파일을 새로 열지 않는다. 비면 더 받을 것이 없다.
std::vector<std::string> documentFontGaps(const model::Document& doc);

// 지금 올라온 글꼴로는 그리지 못하는 문서 글자. 그 글꼴(파일이 없거나 폭만 있으면 대신 그리는 함초롬바탕)과
// fontWithGlyph 의 대체 차례 어디에도 그 글자가 없다. 글꼴 차례, 그 안은 글자 차례다.
struct MissingGlyph {
    std::string face;
    char32_t codePoint = 0;
};
std::vector<MissingGlyph> documentMissingGlyphs(const model::Document& doc);

// 이름을 셸에 알려 받게 한다. 편집을 시작할 때 처음 치는 글자의 대체 글꼴을 미리 받는 데 쓴다.
void requestFontFaces(const std::vector<std::string>& faces);

}  // namespace chwp::layout
