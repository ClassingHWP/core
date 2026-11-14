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
#include <optional>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "chwp/common/bytes.hpp"
#include "chwp/model/document.hpp"

namespace chwp::agent {

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
    int order = 0;    // 입력 전용. 문서 안 물리 순서(1부터, PageInfo::order). 주면 page·section·nth 는 보지 않는다
};

struct PageInfo {
    PageRef ref;                // 출력에서는 page·section 을 늘 채운다. nth 는 번호가 겹칠 때만 채운다
    std::string label;          // 쪽 번호 형식대로 쓴 글. "79", "iv", "Ⅳ", "라"
    bool numberHidden = false;  // 「쪽 번호 감추기」로 이 쪽에 번호가 찍히지 않는다
    int order = 0;              // 문서 안 물리 순서. 1부터. 쪽을 고를 때 PageRef::order 로 그대로 넘길 수 있다
    std::int32_t width = 0;     // 종이 크기(HU). 가로 문서면 이미 긴 변이 width 다
    std::int32_t height = 0;
    bool landscape = false;
};

// ---------------------------------------------------------------------------
// 글 묶음(story)과 문단
// ---------------------------------------------------------------------------

// 서식 값. 이름은 format op 의 set 이 받는 이름과 같다(읽은 값을 고쳐 그대로 넣을 수 있다). 길이는 HU.
using FieldValue = std::variant<bool, double, std::string>;
using Fields = std::vector<std::pair<std::string, FieldValue>>;

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
    int rowSpan = 1;              // 표 칸이 덮는 줄·칸 수(합친 칸이면 2 이상)
    int colSpan = 1;
};

// 표 칸 하나.
struct TableCellInfo {
    int story = -1;               // 칸 글 묶음. 표를 고치면 번호가 바뀐다
    int row = 0;                  // 칸 왼쪽 위 자리. 0부터
    int col = 0;
    int rowSpan = 1;              // 덮는 줄·칸 수
    int colSpan = 1;
    std::int32_t width = 0;       // 칸 크기(HU). 높이는 최소 높이이고 글이 많으면 늘어난다
    std::int32_t height = 0;
    std::string text;             // 칸 글. 문단은 줄바꿈('\n')으로 잇는다
    // 칸 모양: vertAlign, header, ownMargin, marginLeft/Right/Top/Bottom, "cell." 테두리/배경
    // (cell.borderTopType·cell.borderTopWidth·cell.borderTopColor … cell.fillColor 등. 이름은 format 의 char./para. 와 같은 꼴)
    Fields format;
};

// 표 하나의 칸 그림. 모든 줄·칸 자리는 칸 하나에만 덮인다.
struct TableInfo {
    int parentStory = -1;         // 표가 박힌 story·문단
    int parentParagraph = -1;
    int rows = 0;                 // 줄·칸 수. 표를 지웠으면 0
    int cols = 0;
    std::vector<std::int32_t> columnWidths;  // 칸 폭·줄 높이(HU)
    std::vector<std::int32_t> rowHeights;
    std::vector<TableCellInfo> cells;        // 줄·칸 차례
    // 표 모양: cellSpacing, pageBreak, repeatHeader, 안쪽 여백, "table." 테두리/배경
    Fields format;
    // 표 놓기(ObjectInfo::placement 와 같은 이름)
    Fields placement;
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
    // 놓기: treatAsChar, textWrap(SQUARE·TOP_AND_BOTTOM·BEHIND_TEXT·IN_FRONT_OF_TEXT), textFlow, vertRelTo·horzRelTo,
    // vertAlign·horzAlign, vertOffset·horzOffset, width·height, zOrder, flowWithText, allowOverlap. 크기가 없는 개체는 비었다
    Fields placement;
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
    int pageOrder = 0;  // 첫 글자가 놓인 쪽의 물리 순서(PageInfo::order). 쪽에 놓이지 않는 글(바탕쪽 등)이면 0
    std::vector<FormatRun> runs;
    std::vector<ObjectInfo> objects;
    // detail 로 읽을 때만: 문단 모양 전부(paraShapeFields)와 runs 가 쓰는 글자 모양 전부(번호 → 값).
    Fields format;
    std::vector<std::pair<std::uint32_t, Fields>> charShapes;
};

// 같은 story 안 구간 [from, to). from 이 to 보다 뒤면 바꿔 읽는다.
struct Range {
    Position from;
    Position to;
};

// ---------------------------------------------------------------------------
// 찾기·그림
// ---------------------------------------------------------------------------

struct FindQuery {
    std::string text;        // 찾을 글(UTF-8). 문단 경계는 넘지 않는다
    int story = 0;           // 0 이면 모든 story
    bool matchCase = true;   // 거짓이면 ASCII 대소문자를 가리지 않는다
    int limit = 100;         // 찾은 것을 이만큼에서 멈춘다
};

struct Match {
    Range range;
    PageRef page;            // 구간 첫 글자가 놓인 쪽. 쪽에 놓이지 않은 글(숨은 바탕쪽 등)이면 page 가 0
    std::string context;     // 앞뒤 몇 글자를 붙인 그 문단 글
};

// 쪽 하나의 그려진 배치(layout). 좌표는 쪽 왼쪽 위에서 잰 HU 이고, 그림과 같은 자리 계산이다.
struct LayoutLineInfo {
    Position from;            // 줄 첫 글자. story·paragraph·글자 순번
    int to = 0;               // 줄 끝 글자 순번(그 글자는 다음 줄). 같은 문단이다
    std::int32_t x = 0;       // 줄 상자
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t inkRight = 0;  // 글자가 실제로 끝나는 오른쪽. 줄 상자 오른쪽(x + width)을 넘으면 글이 삐져나왔다
    std::string heading;        // 그 줄 앞에 그려진 문단 번호·글머리("1.", "가)", "●")
    std::string text;           // 줄 글(요청에서 text 를 끄면 비었다)
    bool band = false;          // 머리말·꼬리말·쪽 번호
    bool master = false;        // 바탕쪽
    bool turned = false;        // 세로쓰기 틀 안. 좌표가 돌린 틀 기준이다
    bool overflow = false;      // 본문 줄이 본문 틀 아래로 나갔다
};

struct LayoutBoxInfo {
    std::string kind;           // table, cell, picture, shape, equation, container, column
    int story = -1;             // 칸이면 칸 글 story, 개체면 개체가 박힌 문단의 story
    int paragraph = -1;
    int row = -1;               // 칸 자리
    int col = -1;
    int rowSpan = 1;
    int colSpan = 1;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool inlineChar = false;    // 글자처럼 취급
    bool band = false;
    bool master = false;
    bool turned = false;
};

struct PageLayoutInfo {
    PageRef page;
    std::int32_t width = 0;     // 쪽 크기
    std::int32_t height = 0;
    std::int32_t bodyX = 0;     // 본문 틀(여백 안). 본문 줄은 이 틀 안에 있어야 한다
    std::int32_t bodyY = 0;
    std::int32_t bodyWidth = 0;
    std::int32_t bodyHeight = 0;
    std::vector<LayoutLineInfo> lines;   // 그리는 차례(본문·표 칸·머리말이 섞인다)
    std::vector<LayoutBoxInfo> boxes;
};

struct RenderOptions {
    double dpi = 96;         // 종이 1 인치의 픽셀 수. pixelHeight 를 주면 그것을 쓴다
    int pixelHeight = 0;
};

struct Image {
    PageRef page;
    int width = 0;           // 픽셀
    int height = 0;
    Bytes png;
};

// ---------------------------------------------------------------------------
// 고치기
// ---------------------------------------------------------------------------

// 서식 값 하나. 숫자·참거짓·글. 이름과 단위는 render::setFormatField 와 같다(docs 의 서식 이름표).

enum class OpKind {
    InsertText,   // at 에 text. '\n' 은 문단 나눔
    Delete,       // range 를 지운다
    Replace,      // range 를 text 로 바꾼다
    Format,       // range 에 format 을 입힌다. 글자 모양은 구간 글자에, 문단 모양은 구간이 걸친 문단에
    InsertTable,  // at 에 rows × cols 표(글자처럼 취급, 단 폭). cells[행][열] 글을 채운다
    SetText,      // story 의 글 전체를 text 로 바꾼다(표 칸 채우기 등)
    // 표 구조. story 는 표 칸(아무 칸). 규칙은 한글 2024 와 같다. 고친 뒤 칸 story 번호가 바뀐다(OpReport::table).
    InsertRows,     // story 칸이 걸친 줄의 아래(after) 또는 위에 count 줄. 새 줄은 기준 줄의 높이·칸 나눔·테두리를 따른다
    InsertColumns,  // story 칸이 걸친 칸의 오른쪽(after) 또는 왼쪽에 count 칸. 표 폭이 그만큼 늘어난다
    DeleteRows,     // story 칸(toStory 가 있으면 두 칸을 덮는 사각형)이 걸친 줄을 지운다. 걸친 합친 칸은 줄고 글은 남는다
    DeleteColumns,  // 같은 규칙으로 칸을 지운다. 모든 줄·칸을 지우면 표를 지운다
    MergeCells,     // story·toStory 칸을 덮는 사각형을 한 칸으로. 사각형 밖으로 걸친 합친 칸이 있으면 거절한다
    SplitCell,      // story 칸을 rows × cols 로 나눈다. 글은 첫 칸에 남는다
    InsertBreak,    // at 에서 나눈다. breakKind: page, column, columnDef, section. 구역 본문에서만 된다
    // 문서의 모든 find 를 text 로 바꾼다(한글 모두 바꾸기). i 번째 새 글자는 찾은 i 번째 글자의 모양을 받고,
    // 남는 새 글자는 찾은 마지막 글자의 모양을 받는다. 개체를 품은 곳은 바꾸지 않는다. 바꾼 수는 OpReport::replaced
    ReplaceAll,
    // 셀 모양. story·toStory 두 칸을 덮는 사각형(셀 블록)에 건다. 규칙은 한글 2024 표 탭과 같다.
    CellBorder,   // border 단추: all, outside, inside, top, bottom, left, right, insideHorz, insideVert, none,
                  // diagonalDown, diagonalUp. 그 선이 모두 있으면 지우고 아니면 긋는다(none 은 늘 지운다)
    CellFill,     // 배경색 color("#RRGGBB", "none" 이면 없앰)
    EqualWidth,   // 셀 너비를 같게: 줄마다 블록 칸이 블록 폭을 똑같이 나눈다(나머지는 올림)
    EqualHeight,  // 셀 높이를 같게: 칸마다 블록 칸이 블록의 그려진 높이를 똑같이 나눈다(나머지는 올림)
    // 편집 용지. story 가 아니라 section(1부터)의 용지·여백·제본·격자를 format 으로 고친다(section 읽기와 같은 이름).
    // whole 이면 모든 구역에 건다. 읽기 전용 값(section·sectionCount·단·감추기)은 받지 않는다
    PageSetup,
    // at 에 그림을 글자처럼 넣는다(편집기 그림 넣기와 같다: png·jpg·gif·bmp, 픽셀 × 75 크기, 단보다 넓으면 단 폭).
    // width·height(HWPUNIT)를 주면 그 크기이고, 하나만 주면 그림 가로세로 비율을 지킨다. image 는 파일 바이트다.
    InsertPicture,
    // 편집기 명령 하나(command 이름). 자리는 at(캐럿)·range(블록)·object(개체)·story(표 칸)·section 가운데 명령이 쓰는 것,
    // 값은 format(대화 상자 값, JSON 의 set)과 args(명령 인자). 명령 목록과 값 이름은 skills/chwp-agent/reference.md 다.
    // 판정·규칙은 편집기 명령(render)이 정하고, 여기서는 자리를 커서·고른 개체로 바꿔 부른다.
    Command
};

// 개체 이름. paragraph 의 objects[index] 다(ParagraphInfo::objects).
struct ObjectRef {
    int story = -1;
    int paragraph = -1;
    int index = -1;
};

struct EditOp {
    OpKind kind = OpKind::InsertText;
    Position at;
    Range range;
    std::string text;
    Fields format;
    std::string style;  // Format: 문단 스타일 이름. 비어 있으면 그대로
    int story = -1;     // SetText, 표 구조 op 의 기준 칸
    int toStory = -1;   // DeleteRows·DeleteColumns·MergeCells: 범위의 다른 끝 칸
    bool after = true;  // InsertRows·InsertColumns: 참이면 아래(오른쪽), 거짓이면 위(왼쪽)
    int count = 1;      // InsertRows·InsertColumns
    std::string breakKind = "page";  // InsertBreak
    std::string find;        // ReplaceAll: 찾을 글
    std::string border;      // CellBorder: 단추 이름
    std::string lineType = "SOLID";  // CellBorder: 긋는 선 종류·굵기(HWPUNIT, 0 이면 0.12mm)·색
    std::int32_t lineWidth = 0;
    std::string lineColor = "#000000";
    std::string color;       // CellFill: 배경색
    int section = 0;         // PageSetup: 구역(1부터)
    bool whole = false;      // PageSetup: 모든 구역
    bool matchCase = false;  // ReplaceAll: 참이면 ASCII 대소문자를 가린다
    int rows = 0;       // InsertTable, SplitCell
    int cols = 0;
    std::vector<std::vector<std::string>> cells;
    Bytes image;               // InsertPicture: 그림 파일 바이트
    std::int32_t width = 0;    // InsertPicture: HWPUNIT. 0 이면 그림 크기
    std::int32_t height = 0;
    // Command
    std::string command;       // 명령 이름(insertNote, header, objectProperties ...)
    Fields args;               // 명령 인자(endnote, whole, text, caption, name ...). 길이는 HU 수
    bool hasAt = false;        // at·range·object 를 주었는가
    bool hasRange = false;
    bool hasObject = false;
    ObjectRef object;
    PageRef page;              // insertShape: 쪽
};

struct OpReport {
    Position end;                         // 이 op 뒤 캐럿 자리. 넣은 글의 끝, 지운 자리
    std::vector<std::vector<int>> cells;  // InsertTable: 새 표 칸의 story[행][열]
    TableInfo table;                      // 표 구조 op: 고친 뒤의 표. 표를 지웠으면 rows 가 0
    int replaced = -1;                    // ReplaceAll: 바꾼 수
    ObjectRef object;                     // 개체를 넣은 op(insertPicture·insertShape): 새 개체 이름. 없으면 story -1
};

struct EditReport {
    std::vector<OpReport> ops;
    int pages = 0;  // 편집 뒤 쪽 수
    // 편집 앞에 바꾼 라이선스로 막힌 글꼴 {원래, 바꾼}. 편집기와 같은 정책(editSwapLicenseFonts)이다
    std::vector<std::pair<std::string, std::string>> fontSwaps;
};

// ---------------------------------------------------------------------------
// 글꼴
// ---------------------------------------------------------------------------

// 글꼴 서버 목록을 건다. 웹 뷰어와 같은 index.json, license-swap.json, replace.json 글을 셸이 그대로 넘긴다.
// 프로세스 전체에 걸린다. 걸지 않으면 받을 글꼴이 없고, 그 플랫폼 글꼴 폴더만 쓴다.
// 목록에서 metrics/·standin/ 으로 적힌 글꼴은 라이선스로 막힌 글꼴이라, 편집하면 편집기처럼 대역 원본으로 바꾼다.
void setFontCatalog(const std::string& index, const std::string& licenseSwap, const std::string& replace);

// 받을 글꼴 파일 하나.
struct FontFile {
    int id = 0;
    std::string path;  // 글꼴 서버 기준 "폴더/파일". 셸은 서버 주소 + 폴더 + "/" + 파일(URL 인코딩)에서 받는다
    std::string face;
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
    // 지금 문서를 PDF 로(편집기 인쇄·PDF 로 저장과 같다). 쪽마다 dpi(72~600, 기본 200)로 그린 그림을 종이 크기에 채운다.
    // 실패: SaveFailed(쪽이 없거나 그리지 못함).
    Result<Bytes> exportPdf(int dpi = 200);

    model::SourceFormat sourceFormat() const;
    // 저장할 형식. format 이 "hwp"/"hwpx" 면 그것, 비었으면 경로 끝(.hwp/.hwpx), 그것도 없으면 원래 형식(편집기 저장과 같다).
    // format 이 그 밖의 글이면 nullopt.
    std::optional<model::SourceFormat> saveFormatFor(const std::string& path, const std::string& format = {}) const;
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
    // detail 이면 문단 모양과 글자 모양을 format 의 set 이름으로 모두 붙인다.
    Result<ParagraphInfo> paragraph(int story, int paragraph, bool detail = false) const;
    // 구역(1부터)의 편집 용지·단·쪽 번호·감추기 값.
    Result<Fields> section(int section) const;
    // story 칸이 든 표의 칸 그림.
    // 실패: PositionOutOfRange(없는 story), BadArgument(표 칸이 아님).
    Result<TableInfo> table(int story) const;

    // 글 찾기. stories() 차례, 문단 차례로 찾는다.
    // 실패: BadArgument(빈 글), PositionOutOfRange(없는 story).
    Result<std::vector<Match>> find(const FindQuery& query) const;

    // 쪽 한 장을 PNG 로. 화면과 같은 조판·그리기(크로스플랫폼 CPU 경로)다. 세로 픽셀은 32~8000 으로 자른다.
    // 실패: resolvePage 의 실패, RenderFailed.
    Result<Image> renderPage(const PageRef& page, const RenderOptions& options) const;
    // 쪽 하나의 줄·칸·개체 배치. 그림 없이 줄 나눔·넘침·쪽 나뉨을 본다. withText 면 줄 글을 붙인다.
    Result<PageLayoutInfo> layout(const PageRef& page, bool withText = true) const;

    // 편집 목록을 차례로 적용한다. 각 op 의 위치는 그 앞 op 까지 적용한 문서 기준이다.
    // 하나라도 실패하면 문서를 apply 앞 모습으로 되돌리고 그 op 의 Status 를 돌려준다(message 앞에 op 순번).
    // 성공하면 다시 조판까지 마치고, 한 번의 apply 가 undo() 한 번이다.
    // 실패: BadArgument, PositionOutOfRange, CrossStory, NotEditable(그 자리에 캐럿을 놓을 수 없다), ReadOnly, Unsupported.
    Result<EditReport> apply(const std::vector<EditOp>& ops);
    // Command op 가 받는 값(편집기 대화 상자가 처음 보이는 값). op 의 자리(at·range·object·story·section)에서 읽고
    // 문서는 바꾸지 않는다. 이름은 그 명령의 set 이 받는 이름과 같다(읽은 값을 고쳐 그대로 넣을 수 있다).
    // 실패: BadArgument(값을 읽을 수 없는 명령), PositionOutOfRange, NotEditable.
    Result<Fields> commandView(const EditOp& op);
    // 마지막 apply 를 되돌린다 / 되돌린 것을 다시 한다. 할 것이 없으면 거짓.
    bool undo();
    bool redo();

    // 이 문서를 조판하고 그리는 데 필요한데 아직 넣지 않은 글꼴 파일(글자 대체 글꼴 포함). 무엇을 받을지는 core 가 정한다.
    // 셸은 받은 파일을 fontArrived, 못 받은 파일을 fontMissing 으로 돌려준 뒤 fontsDone 을 부르고,
    // 빈 목록이 나올 때까지 되풀이한다. 한 번 못 받은 파일은 다시 내지 않는다.
    // 편집(apply)이 새 글꼴을 쓰게 할 수 있으니 apply 뒤에도 다시 부른다.
    std::vector<FontFile> fontPlan();
    void fontArrived(int id, Bytes bytes);
    void fontMissing(int id);
    // 들어온 글꼴로 문서 전체를 다시 조판한다. 들어온 것이 없으면 아무것도 하지 않는다.
    void fontsDone();
    // 받지 못해 대체 글꼴로 재고 그리는 글꼴 이름.
    std::vector<std::string> missingFonts() const;
    // 들어온 글꼴과 그 대체 어디에도 없어 그리지 못하는 글자(글꼴 이름, 코드 포인트). 그림에서 비거나 네모로 나온다.
    std::vector<std::pair<std::string, char32_t>> missingGlyphs() const;

    // 고급 사용. 이 API 가 아직 감싸지 않은 기능을 쓸 때만 읽는다. 바꾸지 않는다.
    const model::Document& model() const;

private:
    struct Impl;
    explicit AgentDocument(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// JSON 호출
// ---------------------------------------------------------------------------

// agentCall 이 만든 파일(그림, 저장한 문서). 셸은 path 에 bytes 를 쓰기만 한다.
struct CallFile {
    std::string path;
    Bytes bytes;
};

struct CallResult {
    bool ok = false;
    std::string json;  // 응답 한 줄. {"ok":true,...} 또는 {"ok":false,"error":{"code","message"}}
    std::vector<CallFile> files;
};

// 셸이 하는 파일 읽기(insertPicture 의 file). 읽지 못하면 거짓.
using FileReader = std::function<bool(const std::string& path, Bytes& out)>;

// JSON 요청 하나를 받아 응답 하나를 돌려준다. 모양은 skills/chwp-agent/reference.md 에 있다.
// fileDir 은 그림 파일을 둘 폴더다. 응답의 그림 경로는 fileDir + "/" + 이름이다.
// readFile 이 없으면 insertPicture 는 base64 로만 그림을 받는다.
CallResult agentCall(AgentDocument& doc, std::string_view request, std::string_view fileDir,
                     const FileReader& readFile = {});
// Command op 의 명령 이름 전부.
std::vector<std::string> commandNames();

// Status 를 agentCall 응답과 같은 모양으로. 셸이 문서를 열지 못했을 때 쓴다.
std::string statusJson(const Status& status);


}  // namespace chwp::agent
