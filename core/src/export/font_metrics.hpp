#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "noting/export/font.hpp"

namespace noting::model {
struct Document;
}

namespace noting::layout {

// TTF/OTF 바이트를 글꼴 이름에 묶는다. 같은 프로세스에서 레이아웃이 이 폭을 쓴다.
void registerFontFace(std::string face, std::vector<std::uint8_t> bytes);

// em(HWPUNIT)으로 늘린 진행 폭. 글꼴이나 글리프가 없으면 -1.
std::int32_t fontGlyphAdvance(const std::string& face, char32_t codePoint, std::int32_t em);

// hmtx advance. 글꼴 em으로 늘리기 전 값이다. 없으면 -1.
std::int32_t fontGlyphDesignAdvance(const std::string& face, char32_t codePoint);

// 읽어 들인 글꼴 파일의 unitsPerEm. 파일이 없으면 0.
std::uint16_t fontUnitsPerEm(const std::string& face);

// OS/2 xAvgCharWidth. 표가 없거나 0이면 0.
std::int32_t fontXAvgCharWidth(const std::string& face);

// OS/2 usWinAscent + usWinDescent. 표가 없으면 0.
std::int32_t fontWinCell(const std::string& face);

// 글꼴 파일의 디자인 진행폭을 코드 포인트마다 sink 로 넘긴다. 파일이 없으면 거짓.
// unitsPerEm, xAvgCharWidth, winCell 도 채운다. sink 의 advance 는 hmtx 값이고 0 이상이다.
bool fontCopyDesignAdvances(const std::string& face, std::uint16_t& unitsPerEm, std::int32_t& xAvgCharWidth,
                            std::int32_t& winCell, void (*sink)(void* ctx, char32_t cp, std::int32_t advance),
                            void* ctx);

// 글꼴에 어울리는 줄 높이의 기준선. em 글자 상자 위에서 글꼴 어센더까지.
// 반올림은 0에서 멀어지게. 글꼴 파일이 없으면 -1.
std::int32_t fontBaseline(const std::string& face, std::int32_t em);

// 글꼴에 어울리는 줄 높이에서 글자 하나의 상자. 위·아래는 1em = 1024 값에 크기를 곱해 1024로 나누고 반올림한다.
// 1024 값은 font_line_box_table.inc, 표에 없으면 글꼴 파일의 OS/2 win 값, 그것도 없으면 870·154다.
struct FontLineBox {
    std::int32_t ascent = 0;  // 상자 위에서 기준선까지
    std::int32_t height = 0;  // 위 + 아래
};
FontLineBox fontLineBox(const std::string& face, std::int32_t size);

// 웹 한글 글꼴 표의 tVt. 글꼴 파일이 없을 때만 쓴다. 표에 없으면 1024.
std::uint16_t fontTableEm(const std::string& face);

// 그 글꼴 파일에 글리프가 있는지. 다른 글꼴로 바꿔 넣은 경우는 포함하지 않는다.
bool fontHasGlyph(const std::string& face, char32_t codePoint);

// 지정 글꼴에 글리프가 없으면 문서의 대체 글꼴을 쓴다.
// 사용자 영역 기호는 돋움 계열이면 함초롬돋움, 바탕 계열이면 함초롬바탕으로 넘긴다.
std::string fontWithGlyph(const std::string& face, char32_t codePoint, const std::string& subst = {},
                          const std::string& base = {});

// 이미 올라온 글꼴만 본다. 파일을 새로 열지 않는다.
// warmDocumentFonts 가 끝난 뒤, 글꼴 맵을 바꾸는 쪽이 없을 때만 여러 스레드에서 부른다.
struct ReadyGlyph {
    std::string resolved;
    std::int32_t advance = -1;
    std::uint16_t units = 0;
    bool known = false;
};

ReadyGlyph measureReadyGlyph(const std::string& face, char32_t codePoint, const std::string& subst = {},
                             const std::string& base = {});

// 등록됐거나 디스크에 있는 글꼴 파일. 그리기에서 글리프를 찍을 때 쓴다.
bool fontFaceBytes(const std::string& face, std::shared_ptr<const std::vector<std::uint8_t>>& out, int& fontOffset);

// 윤곽을 읽을 글꼴 파일. bold 이고 굵은 파일이 있으면 그 파일을 주고 designedBold 를 참으로 둔다.
// 굵은 파일이 없으면 요청한 파일을 주고 designedBold 는 거짓이다.
bool fontOutlineBytes(const std::string& face, bool bold, std::shared_ptr<const std::vector<std::uint8_t>>& out,
                      int& fontOffset, bool& designedBold);

// 요청한 글꼴이 이미 굵거나, 같은 이름의 굵은 파일이 있으면 참. 파일을 복사하지 않는다.
bool fontDesignedBold(const std::string& face);

// 이미 메모리에 있거나 이 기기의 글꼴 파일로 있는 것만 연다.
// 없는 파일은 셸이 requestDocumentFontFaces 로 받아 registerFontFace 에 넣는다.
void warmDocumentFonts(const model::Document& doc);

// 글꼴 파일을 처음 열 때 로그에 남기는 이유. 이미 열린 글꼴은 다시 적지 않는다.
class FontLoadScope {
public:
    FontLoadScope(const char* why, char32_t cp, const std::string* asked, bool force = true);
    ~FontLoadScope();
    FontLoadScope(const FontLoadScope&) = delete;
    FontLoadScope& operator=(const FontLoadScope&) = delete;

private:
    const char* why_ = nullptr;
    const char* step_ = nullptr;
    char32_t cp_ = 0;
    const std::string* asked_ = nullptr;
    bool active_ = false;
};

// 같은 이유 안에서, 요청 글꼴 대신 다른 파일을 열 때 붙인다.
class FontLoadStep {
public:
    explicit FontLoadStep(const char* step);
    ~FontLoadStep();
    FontLoadStep(const FontLoadStep&) = delete;
    FontLoadStep& operator=(const FontLoadStep&) = delete;

private:
    const char* step_ = nullptr;
    bool active_ = false;
};

// 쪽 쌓기 중에 연 글꼴 로그에 쪽 번호를 붙인다. 0이면 생략한다.
void setFontLoadPage(int page);

// 셸이 가진 글꼴 목록. 이름은 셸 파일 키를 가리키는 JSON 객체다.
// 코어는 이 키로 주소를 만들지 않는다. 같은 파일의 바이트를 이름끼리 나눌 때만 쓴다.
void setFontCatalog(const std::string& json);

// 라이선스 때문에 파일을 올리지 못한 글꼴. 셸 목록에 폭만 있는 이름(metrics/)과 대역 파일을 붙인 이름(standin/)이다.
// 목록에 아예 없는 이름(사용자 컴퓨터에 깔린 글꼴 등)은 여기 들지 않는다.
bool fontLicenseBlocked(const std::string& face);

// 막힌 글꼴을 편집할 때 바꿀 글꼴 표. {"막힌 이름": "바꿀 이름"} JSON 객체다.
void setFontLicenseSwaps(const std::string& json);

// 막힌 글꼴이면 바꿀 글꼴 이름, 아니면 빈 문자열. 표에 없거나 표의 글꼴이 목록에 없으면 함초롬으로 간다.
std::string fontLicenseSwapTarget(const std::string& face);

// 글꼴 고르기 목록에 드는 이름인지. 파일을 올린 이름이다.
bool fontPickable(const std::string& face);

// 글꼴 고르기 목록. 파일을 올린 이름만 한글 가나다순, 그다음 영문 ABC순으로 준다. 막힌 글꼴은 넣지 않는다.
std::vector<std::string> pickableFonts();

struct FontSwap {
    std::string from;
    std::string to;
};

// 문서 글꼴 표에서 막힌 글꼴과 바꿀 글꼴. 비어 있으면 그대로 편집해도 된다.
std::vector<FontSwap> licenseFontSwaps(const model::Document& doc);

}  // namespace noting::layout
