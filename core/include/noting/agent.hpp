#pragma once

// 에이전트 API. Agent Skill·CLI·WASM 이 문서를 읽고, 좌표로 찾고, 그림으로 뽑고, 고칠 때 쓰는 공개 면이다.
// 설계와 단위 규칙은 docs/agent-api.md 에 있다. 이 헤더의 주석이 계약이고, 문서는 그 해설이다.
//
// 공통 규칙
// - 상태 없음: 화면 커서·블록·조합(pointer_hold 의 전역 상태)을 읽지도 바꾸지도 않는다. 위치는 늘 인자로 받는다.
// - 한 번에 한 문서: 글꼴·줄 캐시가 아직 전역이라, AgentDocument 는 한 프로세스에서 하나만 열어 쓴다.
// - 실패는 값으로: 예외를 던지지 않는다. 모든 결과는 Status 를 갖고, 실패하면 value 는 쓰지 않는다.
// - 길이 단위: HWPUNIT(HU). 1 인치 = 7200 HU, 1 mm = 7200 / 25.4 HU, 1 pt = 100 HU.
// - 쪽 좌표: 종이 왼쪽 위가 (0, 0), 오른쪽이 +x, 아래가 +y.
// - 구역: 1부터 센다(한글 상태 표시줄의 「1/3 구역」과 같다). 모델의 sections[] 순번은 1을 뺀 값이다.
// - 글자 순번: 문단 글자열(ParagraphInfo::text)의 유니코드 코드 포인트 순번. 0부터.
//   글자처럼 취급한 개체와 각주·미주 번호는 U+FFFC 한 글자다. 떠 있는 개체는 글자를 차지하지 않는다.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "noting/common/bytes.hpp"
#include "noting/model/document.hpp"

namespace noting::agent {

// ---------------------------------------------------------------------------
// 오류
// ---------------------------------------------------------------------------

enum class Code {
    Ok,
    BadArgument,         // 형식이 틀린 인자(길이 문자열, 음수 크기 등)
    PageNotFound,        // 그런 쪽 번호가 없다
    AmbiguousPage,       // 쪽 번호가 여러 쪽에 붙어 있다. Status::candidates 에서 골라 다시 부른다
    PositionOutOfRange,  // 없는 story·문단·글자
    CrossStory,          // 구간의 두 끝이 서로 다른 story 다
    ReadOnly,            // 문서가 읽기 전용이다
    NotEditable,         // 그 자리는 고칠 수 없다(쪽 번호 등 조판이 만든 글)
    Unsupported,         // 아직 못 하는 일
    ParseFailed,         // 파일을 읽지 못했다
    SaveFailed,          // 파일로 쓰지 못했다
    RenderFailed,        // 그리지 못했다
    Internal             // 코어 버그. 테스트에서 나오면 안 된다
};
const char* codeName(Code code);

struct PageRef;

struct Status {
    Code code = Code::Ok;
    std::string message;              // 한국어 한 문장. 어떤 인자가 왜 틀렸는지
    std::vector<PageRef> candidates;  // AmbiguousPage 일 때 고를 수 있는 쪽

    [[nodiscard]] bool ok() const { return code == Code::Ok; }
};

template <class T>
struct Result {
    Status status;
    T value{};

    [[nodiscard]] bool ok() const { return status.ok(); }
};

// ---------------------------------------------------------------------------
// 단위
// ---------------------------------------------------------------------------

// "50mm", "12.5pt", "3cm", "1in", "1234hu", "1234" 를 HU 로 바꾼다. 단위가 없으면 HU 다.
// 실패: BadArgument(숫자가 아니거나 모르는 단위, int32 를 넘는 값).
Result<std::int32_t> parseLength(std::string_view text);
double huToMm(std::int32_t hu);

// ---------------------------------------------------------------------------
// 쪽
// ---------------------------------------------------------------------------

// 쪽을 가리키는 값. 물리 순서가 아니라 한글처럼 쪽 번호와 구역으로 가리킨다(docs/agent-api.md 2.1).
struct PageRef {
    int page = 0;     // 인쇄 쪽 번호. 그 쪽에 번호를 감췄어도 한글이 매긴 번호다
    int section = 0;  // 구역. 1부터. 0이면 생략(번호가 문서 전체에서 하나일 때만 된다)
    int nth = 0;      // 한 구역 안에서 「새 번호로 시작」으로 번호가 겹칠 때 몇 번째인지. 1부터. 0이면 생략
};

struct PageInfo {
    PageRef ref;                // 출력에서는 page·section 을 늘 채운다. nth 는 번호가 겹칠 때만 채운다
    std::string label;          // 쪽 번호 형식대로 쓴 글. "79", "iv", "Ⅳ", "라"
    bool numberHidden = false;  // 「쪽 번호 감추기」로 이 쪽에 번호가 찍히지 않는다
    int order = 0;              // 문서 안 물리 순서. 1부터. 출력 전용이고 입력으로는 받지 않는다
    std::int32_t width = 0;     // 종이 크기(HU). 가로 문서면 이미 긴 변이 width 다
    std::int32_t height = 0;
    bool landscape = false;
};

// ---------------------------------------------------------------------------
// 글 묶음(story)과 문단
// ---------------------------------------------------------------------------

struct StoryInfo {
    int story = -1;               // 위치에 쓰는 번호. 같은 문서를 다시 열면 같은 번호다. 편집 뒤에는 바뀔 수 있다
    std::string kind;             // section, tableCell, subList, caption, masterPage
    std::string owner;            // 담고 있는 개체: body, table, header, footer, footnote, endnote, textBox, caption, masterPage
    int section = 0;              // 속한 구역. 1부터
    int paragraphs = 0;           // 문단 수
    int parentStory = -1;         // 이 묶음을 담은 개체가 있는 바깥 story. 구역 본문·바탕쪽이면 -1
    int parentParagraph = -1;     // 바깥 story 안에서 그 개체가 박힌 문단
    int row = -1;                 // 표 칸이면 행·열. 0부터
    int col = -1;
};

// 문단 안 글자 위치. 같은 story 안에서만 뜻이 있다.
struct Position {
    int story = -1;
    int paragraph = -1;  // story 안 문단 순번. 0부터
    int index = -1;      // 글자 순번. 0 이 문단 맨 앞, 문단 글자 수가 맨 끝(마지막 글자 뒤)
};

struct TextFormat {
    std::string font;        // 한글 글꼴 이름
    std::string latinFont;   // 영문 글꼴 이름
    std::int32_t size = 0;   // 글자 크기. 1000 = 10pt
    bool bold = false;
    bool italic = false;
    std::string underline;   // NONE, BOTTOM, TOP
    bool strikeout = false;
    std::string color;       // #RRGGBB. 비어 있으면 기본색
};

// 같은 글자 모양이 이어지는 구간 [from, to). 글자 순번.
struct FormatRun {
    int from = 0;
    int to = 0;
    std::uint32_t charShapeId = 0;
    TextFormat format;
};

struct ObjectInfo {
    int index = 0;              // 이 문단 안 개체 순번. 0부터. 개체 이름은 {story, paragraph, index}
    int at = 0;                 // 개체가 박힌 글자 순번. 글자처럼 취급한 개체는 그 U+FFFC 의 순번
    std::string kind;           // table, picture, shape, container, ole, equation, footnote, endnote, header, footer
    bool inlineChar = false;    // 글자처럼 취급(본문 흐름 안에 한 글자로 앉음)
    std::int32_t width = 0;     // 파일에 적힌 크기(HU). 머리말·각주처럼 크기가 없으면 0
    std::int32_t height = 0;
    std::string script;         // 수식 스크립트
    std::vector<int> stories;   // 안에 담은 글 묶음(표 칸, 글상자 글, 머리말 글 등)
};

struct ParagraphInfo {
    int story = -1;
    int paragraph = -1;
    std::string text;           // UTF-8. 탭 '\t', 줄바꿈 '\n', 글자 개체 U+FFFC
    int length = 0;             // text 의 코드 포인트 수
    std::string style;          // 스타일 이름
    std::string align;          // JUSTIFY, LEFT, RIGHT, CENTER, DISTRIBUTE, DISTRIBUTE_SPACE
    std::string lineSpacingType;  // PERCENT, FIXED, BETWEEN_LINES, AT_LEAST
    std::int32_t lineSpacing = 0;   // PERCENT 면 %, 나머지는 HU
    std::int32_t indent = 0;        // 첫 줄. 양수 들여쓰기, 음수 내어쓰기(HU)
    std::int32_t spacingPrev = 0;   // 문단 위·아래 간격(HU)
    std::int32_t spacingNext = 0;
    std::vector<FormatRun> runs;
    std::vector<ObjectInfo> objects;
};

// ---------------------------------------------------------------------------
// 문서
// ---------------------------------------------------------------------------

// 열린 문서 하나. 문서 모델과 조판 결과를 함께 갖는다.
class AgentDocument {
public:
    ~AgentDocument();
    AgentDocument(const AgentDocument&) = delete;
    AgentDocument& operator=(const AgentDocument&) = delete;

    // HWP·HWPX 바이트를 읽고 전체를 조판한다. 바이트는 복사해 둔다(그림을 그릴 때 다시 읽는다).
    // 실패: ParseFailed(형식을 모르거나 깨진 파일, 암호 문서 등. 이유를 message 에 적는다).
    static Result<std::unique_ptr<AgentDocument>> open(ByteView bytes);

    // 지금 문서를 HWP 또는 HWPX 로 쓴다. 문서는 바뀌지 않는다.
    // 실패: BadArgument(형식이 Unknown), SaveFailed.
    Result<Bytes> save(model::SourceFormat format) const;

    model::SourceFormat sourceFormat() const;
    std::vector<std::string> warnings() const;  // 읽으면서 남긴 경고

    // 문서 순서대로 모든 쪽.
    std::vector<PageInfo> pages() const;
    // 쪽 하나를 고른다. 돌려주는 값은 pages() 의 순번(0부터)이다. 다른 함수의 입구에서 쓴다.
    // 실패: PageNotFound(번호가 없음. 그 구역의 번호 범위를 적는다),
    //       AmbiguousPage(번호가 겹침. candidates 를 채운다), BadArgument(없는 구역).
    Result<int> resolvePage(const PageRef& ref) const;

    // 글 묶음 전부. 구역 본문이 먼저, 그 안의 개체 묶음이 뒤에 온다.
    std::vector<StoryInfo> stories() const;
    // 문단 하나의 글자열, 문단 모양, 글자 모양 구간, 개체.
    // 실패: PositionOutOfRange(없는 story·문단).
    Result<ParagraphInfo> paragraph(int story, int paragraph) const;

    // 고급 사용. 이 API 가 아직 감싸지 않은 기능을 쓸 때만 읽는다. 바꾸지 않는다.
    const model::Document& model() const;

private:
    struct Impl;
    explicit AgentDocument(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace noting::agent
