#pragma once

// 한글이 저장한 줄 정보 없이, 글자 폭과 문단 폭으로 줄을 나눈다.
// 글자 폭은 advance와 글꼴 파일의 unitsPerEm, 장평으로 만들고 자간은 그 다음에 더한다.
// 글꼴 파일이 없으면 웹 한글 글꼴 표의 tVt를 쓰고, 표에도 없으면 1024다.

#include <cstdint>
#include <string>
#include <vector>

#include "noting/export/text.hpp"
#include "noting/model/document.hpp"

namespace noting::layout {

// 글꼴 파일이 있으면 hmtx advance를 그 파일의 unitsPerEm으로 나눈다.
// 크기는 상대 크기를 먼저 반영하고, 위·아래 첨자는 이어서 16/25다. 줄 높이는 이 크기를 쓰지 않는다.
// 장평 100은 4 × 반올림(advance × ⌊크기 ÷ 4⌋ ÷ tVt)다. 함초롬바탕 가는 10pt에서 972다.
// 장평이 100이 아니면 4 × 버림(advance × ⌊크기 ÷ 4⌋ × 장평 ÷ (100 × tVt))다. 90% 가는 872다.
// 자간은 코드가 31 이상이고 0이 아닐 때만 4 × 반올림((폭 ÷ 4) × 자간 ÷ 100)을 더한다. 0.5는 0에서 멀어진다.
// 띄어쓰기는 4 × ⌊글자크기 ÷ 8⌋이다. 10pt는 500, 5pt는 248이다. 고정폭 빈칸은 4 × ⌊⌊글자크기 ÷ 4⌋ ÷ 4⌋라 10pt는 248이다.
// 둘 다 장평은 한글 칸, 자간은 영문 칸이다. 장평이 100이 아니면 몫에 장평을 곱해 반올림한 뒤 4를 곱한다.
// 글꼴에 어울리는 빈칸이 켜지면 띄어쓰기만 영문 글꼴 advance와 영문 장평의 일반 폭을 쓴다.
// 줄바꿈 비교에서 이미 들어간 글자는 자간을 포함하고, 지금 보는 글자는 폭만 쓴다.
// 그 글꼴에 글자가 없으면 함초롬바탕의 폭을 쓴다. 거기에도 없으면 라틴은 0.5em, 한글·한자·전각은 1em 이다.
// 공백·문장부호는 호출하는 쪽에서 앞 글자 언어를 넘긴다.
std::int32_t glyphAdvance(char32_t codePoint, const model::CharShape& shape, model::Lang lang,
                          const model::Document* doc = nullptr);
// 폭 뒤에 더하는 자간. 줄에 넣을지는 이 값을 뺀 폭으로 본다.
std::int32_t glyphTrail(char32_t codePoint, const model::CharShape& shape, model::Lang lang,
                        const model::Document* doc = nullptr);

// 한글이 글자마다 정하는 언어. 0 한글부터 6 사용자까지 model::Lang 과 같은 번호다.
// drawing 은 둘째 인자 1이다. 글자를 그리고 잴 때 이 값을 넘기고, 글머리표 칸 너비만 0을 넘긴다.
// 둘째 인자는 사용자 영역 E0BC..F7FF 에서만 다르다. 0이면 사용자, 1이면 F000..F0FF 는 기호, 나머지는 한글이다.
model::Lang hancomCharLang(char32_t codePoint, bool drawing);

// 기본 탭 간격. 파일의 8000 은 2배로 저장된 값이므로 실제 간격은 4000(40pt)이다.
inline constexpr std::int32_t kDefaultTab = 4000;

// innerLines 는 쪽마다 줄이 개체 안 글이면 1이다. 본문에 다시 그리지 않기 위한 표시다.
// 우리 줄나눔에서 한 줄이 차지하는 문단 글자 범위. end 는 마지막 글자의 다음 위치다.
struct LineRange {
    int paragraph = 0;
    int start = 0;
    int end = 0;
};

// 글자 크기의 85% 기준선. 한글은 크기 × 85 / 100 을 반올림한다. 1131은 961, 1004는 853이다.
inline std::int32_t glyphBaseline(std::int32_t size) {
    if (size <= 0) return 0;
    return static_cast<std::int32_t>((static_cast<std::int64_t>(size) * 85 + 50) / 100);
}

// 작업 수와 코어 수 중 작은 값. 스레드 없는 wasm 빌드에서는 1이다.
int workerCount(int jobs);

// 문단마다 글자 폭을 워커가 재고, 메인 스레드가 문단 순서대로 받아 둔다.
// 쪽 나눔은 앞 문단 위치와 떠 있는 개체에 기대므로 여기서 나누지 않는다.
void prepareParagraphGlyphs(const model::Document& doc);
void finishParagraphGlyphs();
// 줄 캐시로 연 문서는 조판 때 글자 폭을 재지 않는다. 그리기에서 처음 잰 폭은 같은 글자에 다시 쓴다.
void retainGlyphScaleCache();
void clearGlyphScaleCache();

// paragraphPages 는 쪽마다, 그 쪽에 걸친 문단의 문서 전체 순번이다.
// lineRanges 는 그 쪽의 본문 줄이다. 글상자·표 안 줄은 넣지 않는다.
std::vector<std::string> paginate(const model::Document& doc, const TextOptions& options,
                                  std::vector<int>* anchorPages = nullptr,
                                  std::vector<std::vector<std::uint8_t>>* innerLines = nullptr,
                                  std::vector<std::vector<int>>* paragraphPages = nullptr,
                                  std::vector<std::vector<LineRange>>* lineRanges = nullptr);

}  // namespace noting::layout
