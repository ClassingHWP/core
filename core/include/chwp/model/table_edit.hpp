#pragma once

// 표 구조 편집: 줄·칸 넣기와 지우기, 칸 합치기와 나누기.
// 규칙은 한글 2024 와 같다(합친 칸이 있는 표로 맞춘 결과는 tests/test_table_edit.cpp).
// 다시 조판·되돌리기·캐럿은 이 함수를 부르는 편집 명령(render)이 맡는다.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "chwp/model/document.hpp"

namespace chwp::model {

// 칸 크기 하나가 덮는 줄·칸 구간.
struct SpanSize {
    std::size_t start = 0;
    int span = 1;
    std::int32_t size = 0;
};

// 구간 크기들로 count 개 줄(칸)의 크기를 푼다. 좁은 구간부터 경계를 정하고, 남는 구간은 똑같이 나눈다.
// 경계가 어긋나는 구간(한글이 넓힌 뒤 고치지 않고 둔 합친 칸 크기 등)은 버린다. 조판의 칸 폭도 이것으로 정한다.
std::vector<std::int32_t> solveSpanSizes(std::size_t count, std::vector<SpanSize> spans);

// 표의 칸 그림. rows × cols 자리마다 그 자리를 덮는 칸(cells 순번). 덮는 칸이 없으면 -1, 둘이 덮으면 -2.
struct TableGrid {
    int rows = 0;
    int cols = 0;
    std::vector<int> owner;
    [[nodiscard]] int at(int row, int col) const { return owner[static_cast<std::size_t>(row * cols + col)]; }
};
TableGrid tableGrid(const Table& table);
// 모든 자리가 칸 하나에만 덮이는지.
bool tableGridValid(const Table& table);
// 칸 크기 값으로 푼 열 폭과 줄 높이.
std::vector<std::int32_t> tableColumnWidths(const Table& table);
std::vector<std::int32_t> tableRowHeights(const Table& table);

struct TableEdit {
    std::string error;            // 비어 있으면 고쳤다. 아니면 표는 그대로다
    std::int32_t widthDelta = 0;  // 표 바깥 크기가 바뀐 만큼
    std::int32_t heightDelta = 0;
    int focusCell = -1;           // 편집 뒤 캐럿을 둘 칸(cells 순번)
    bool removeTable = false;     // 모든 줄(칸)을 지웠다. 표를 지울 몫은 부른 쪽이다
    [[nodiscard]] bool ok() const { return error.empty(); }
};

// 칸 cell 이 걸친 줄의 아래(below) 또는 위에 count 줄을 넣는다.
// 새 줄 높이는 기준 줄(아래면 칸의 마지막 줄, 위면 첫 줄) 높이다. 새 칸은 기준 줄의 그 자리를 덮던 칸의
// 칸 나눔·테두리/배경·여백을 따르고 비어 있다. 넣는 자리를 가로지르는 합친 칸은 count 만큼 늘어난다.
TableEdit insertTableRows(Table& table, int cell, bool below, int count);
// 같은 규칙으로 칸 cell 이 걸친 칸의 오른쪽(right) 또는 왼쪽에 count 칸을 넣는다. 새 칸 폭은 기준 칸 폭이다.
TableEdit insertTableColumns(Table& table, int cell, bool right, int count);
// first~last 줄(칸)을 지운다. 그 안에 다 든 칸은 사라지고, 걸친 합친 칸은 줄고 글은 남는다.
// 지운 뒤 어느 칸도 나누지 않는 경계선은 없앤다.
TableEdit deleteTableRows(Table& table, int first, int last);
TableEdit deleteTableColumns(Table& table, int first, int last);
// 줄 row0~row1, 칸 col0~col1 사각형의 칸을 하나로 합친다. 사각형 밖으로 삐져나간 합친 칸이 있으면 거절한다.
// 글은 칸 자리 차례로 잇고 빈 칸은 건너뛴다. 테두리/배경은 왼쪽 위 칸 것이다.
TableEdit mergeTableCells(Table& table, int row0, int col0, int row1, int col1);
// 칸 cell 을 rows × cols 로 나눈다. 폭은 똑같이 나누고 나머지는 마지막 칸에 준다. 높이는 첫 줄이 firstHeight
// (칸 글 높이 + 위아래 여백), 가운데 줄이 emptyHeight(빈 줄 높이 + 여백), 마지막 줄이 남은 높이다.
// 그 자리에 경계선이 없으면 새로 긋고, 그 선을 가로지르는 다른 칸은 늘어난다. 글은 첫 칸에 남는다.
TableEdit splitTableCell(Table& table, int cell, int rows, int cols, std::int32_t firstHeight,
                         std::int32_t emptyHeight);

// 어느 칸도 나누지 않는 경계선을 없애고, 칸을 줄·칸 차례로 놓고, 줄·칸 수를 맞춘다.
void normalizeTable(Table& table);

// 아래 칸 모양 편집은 줄 row0~row1, 칸 col0~col1 사각형(셀 블록)에 건다. 사각형에 걸친 합친 칸은 사각형을 넓혀 담는다.

// 셀 테두리 단추(한글 2024 표 탭의 테두리). 블록의 그 선들이 모두 그어져 있으면 지우고, 아니면 line 으로 긋는다.
// None 은 블록의 모든 선을 지운다. 선은 그 선을 사이에 둔 두 칸 모두에 적는다(블록 밖 이웃 칸도).
// 대각선은 블록의 칸마다 건다: DiagonalDown 은 왼쪽 위→오른쪽 아래(backSlash), DiagonalUp 은 slash.
enum class CellBorderKind {
    All, Outside, Inside, Top, Bottom, Left, Right, InsideHorz, InsideVert, None, DiagonalDown, DiagonalUp
};
// 칸이 쓰는 테두리/배경을 읽고, 고친 테두리/배경의 번호를 받는다(같은 것이 있으면 그 번호).
struct BorderFillAccess {
    std::function<BorderFill(std::uint32_t id)> read;
    std::function<std::uint32_t(const BorderFill& fill)> intern;
};
TableEdit setTableCellBorders(Table& table, int row0, int col0, int row1, int col1, CellBorderKind kind,
                              const BorderSide& line, const BorderFillAccess& fills);
// 블록 칸의 배경을 한 색으로 칠한다. faceColor 가 비었거나 "none" 이면 배경을 없앤다.
TableEdit fillTableCells(Table& table, int row0, int col0, int row1, int col1, const std::string& faceColor,
                         const BorderFillAccess& fills);
// 셀 너비(heights 면 높이)를 같게(한글 2024 TableDistributeCellWidth·Height). 블록의 줄(칸)마다 그 줄의 블록 칸이
// 블록 폭(높이)을 똑같이 나눠 갖고 나머지는 올린다(그만큼 표가 커진다). 나눈 자리에 경계선이 없으면 새로 긋는다.
// lineSizes 는 높이를 맞출 때의 줄마다 실제 높이(조판한 높이)다. 비었으면 칸 크기 값으로 푼다.
TableEdit equalizeTableCells(Table& table, int row0, int col0, int row1, int col1, bool heights,
                             std::vector<std::int32_t> lineSizes = {});

// 블록 칸의 세로 정렬(TOP·CENTER·BOTTOM). 한글 TableVAlign*·TableCellAlign*(가로 정렬은 칸 문단의 문단 모양이다).
TableEdit alignTableCells(Table& table, int row0, int col0, int row1, int col1, const std::string& vertAlign);
// 블록 칸의 글자 방향. 0 가로, 1 세로(영문 눕힘), 2 세로(영문 세움). 한글 TableCellTextHorz·Vert·VertAll.
TableEdit setTableCellDirection(Table& table, int row0, int col0, int row1, int col1, std::uint8_t direction);

// 표 뒤집기(한글 2024 TableSwap 의 type 0~5). 줄 기준은 위아래, 칸 기준은 좌우, 줄/칸은 왼쪽 위-오른쪽 아래 대각선
// 기준으로 뒤집는다. 회전은 이 셋을 이은 것이다(반시계 90도 = 줄/칸 뒤 줄 기준, 180도 = 칸 기준 뒤 줄 기준,
// 시계 90도 = 줄/칸 뒤 칸 기준).
enum class TableFlip : std::uint8_t { Rows, Columns, Diagonal, CounterClockwise, Half, Clockwise };
// 칸은 자리·합침·크기·제목 줄 표시·테두리째 옮긴다. 테두리/배경(칸과 표 자신)은 네 변과 대각선을 같이 돌리고,
// 대각선의 시계 방향 표시는 한글과 같게 바꾼다(위아래 뒤집기는 /·\ 를 맞바꾸고 둘 다 뒤집고, 좌우는 맞바꾸기만,
// 대각선 기준은 아래·위 덧선을 맞바꾸고 / 만 뒤집는다). 중심선·무늬·그러데이션·세로 정렬·글자 방향은 그대로다.
// swapMargins(여백 뒤집기)면 표 안쪽 여백과 칸 여백도 같이 돌린다. 바깥 여백은 그대로다.
// focusCell 은 옮긴 뒤 그 칸의 순번으로 돌려준다(TableEdit::focusCell).
TableEdit flipTable(Table& table, TableFlip flip, bool swapMargins, const BorderFillAccess& fills,
                    int focusCell);

// 표 자동 채우기(한글 2024 TableAutoFill). 처음 값(본보기)들의 규칙. 본보기가 하나뿐이거나 규칙이 없으면 Repeat 로
// 본보기 칸을 차례로 되풀이한다. 순수한 수(앞뒤 글 없음)는 등차·등비, 글이 붙은 값은 자동 채우기 목록(요일·달·간지 등,
// 끝 다음은 처음)이 먼저고, 목록에 없고 모두 수가 있으면 수만 늘리고 앞뒤 글은 본보기 차례로 붙인다.
struct AutoFillRule {
    enum class Kind { Repeat, Arithmetic, Geometric, List };
    Kind kind = Kind::Repeat;
    int seedCount = 0;
    double last = 0;  // 마지막 본보기 수
    double step = 0;  // 등차의 차이, 등비의 비
    std::vector<std::string> prefixes;
    std::vector<std::string> suffixes;
    int list = -1;       // 목록 번호
    int listIndex = -1;  // 마지막 본보기의 목록 자리
};
AutoFillRule autoFillRule(const std::vector<std::string>& seeds);
// 처음부터 k 번째(본보기가 0~seedCount-1) 칸에 넣을 글. 비어 있으면 본보기 k % seedCount 칸을 그대로 옮긴다.
std::optional<std::string> autoFillValue(const AutoFillRule& rule, int k);
// 줄 row0~row1, 칸 col0~col1 블록을 채운다. 블록이 넓거나 정사각이면 줄마다 왼쪽에서 오른쪽, 길면 칸마다 위에서 아래다.
// 줄의 첫 칸부터 처음 빈 칸 앞까지가 본보기이고, 그 뒤 칸은 모두 덮어쓴다. 첫 칸이 빈 줄은 앞 줄 규칙을 이어 간다.
// 채운 칸의 문단 모양·글자 모양은 본보기 차례로 돌아가며 쓰고, 칸 테두리/배경은 그 칸 것을 둔다.
// 블록 첫 칸이 비었거나 합친 칸이 들면 고치지 않는다.
TableEdit autoFillTableCells(Table& table, int row0, int col0, int row1, int col1);

// 표 정렬(한글 2024 Sort, 표 칸 블록). 블록의 줄 row0~row1 을 줄째 옮긴다(블록 밖 칸도 같은 줄이면 같이 간다).
// column 은 블록 첫 칸을 1 로 센 기준 칸(블록 밖도 된다). format 0 글자, 1 숫자, 2 날짜, 3 코드.
// 글자는 Windows 한국어 정렬(collate.hpp), 숫자는 수가 글보다 앞, 날짜는 -./ 로 나눈 년월일, 코드는 UTF-16 차례다.
// 같으면 다음 기준을 본다. 차례 정하기는 한글 자체 qsort 를 그대로 따라 같은 키의 차례도 한글과 같다.
// 두 줄이 안 되거나 블록 줄에 여러 줄을 합친 칸이 있으면 하지 않는다.
// keys 는 대화 상자의 기준을 차례대로 다 준다(없음은 column 0). 한글처럼 비교는 첫 기준과 칸이 있는 뒤 기준 수만큼이고,
// i 번째 비교는 i 번째 기준의 형식으로 i 번째부터 처음 칸이 있는 기준의 칸을 견준다(기준 2 가 없음이면 기준 3 칸을
// 기준 2 형식으로 본다).
struct TableSortKey {
    int column = 1;
    int format = 0;
    bool descending = false;
};
struct TableSortOptions {
    bool fromRear = false;   // 뒤에서 앞으로 비교(글자·코드)
    bool extendYear = true;  // 두 자리 연도 기준 적용(날짜)
    int yearBase = 1930;     // 두 자리 연도 범위의 첫 해
    // 고어 정렬(한글 CheckJaso): 글자 형식을 음절마다 첫·가운데·끝 낱자 차례(한글 옛한글 차례)로 견준다.
    bool oldHangul = false;
    // 종성/중성/초성 순으로 비교(CheckJasoReverse): 같은 방식으로 끝·가운데·첫 낱자 차례. 둘 다 켜면 고어 정렬이다.
    bool finalFirst = false;
};
TableEdit sortTableRows(Table& table, int row0, int col0, int row1, int col1, const std::vector<TableSortKey>& keys,
                        const TableSortOptions& options);

// 본문 정렬(한글 Sort, 글 블록). 블록이 걸친 문단(블록 끝이 문단 맨 앞이면 그 문단은 뺀다)을 통째로 옮긴다.
// 키는 블록 안 글(첫 문단은 블록 시작부터, 끝 문단은 블록 끝까지)이다. TableSortKey::column 은 위치다:
// 0 문단 전체, n 필드 n(구분 기호로 나눈 n 번째), kSortWordPosition 단어(문단 대신 단어를 정렬한다).
// 필드는 한글 2024 정렬의 필드 나누기와 같게 나눈다(연속 구분 기호 무시를 끄면 구분 기호 사이 빈 필드도 센다).
// 빈 키(빈 문단, 없는 필드)는 오름차순에서 맨 앞, 내림차순에서 맨 뒤다.
inline constexpr int kSortWordPosition = 0x7fffffff;
struct ParagraphSortOptions : TableSortOptions {
    int delimiter = 0;  // 필드 구분: 0 탭, 1 쉼표, 2 빈칸, 3 기호(delimiterChars 의 글자마다)
    std::u16string delimiterChars;
    bool ignoreMultiDelimiter = true;  // 연속된 구분 기호 무시
    bool eachParagraph = true;         // 단어 정렬을 문단마다 따로(각 문단별 단어 정렬)
};
// 정렬할 문단 하나: HWP 원문(탭·컨트롤은 8 단위 확장 문자) 뒤에 문단 끝 \r, 블록 안 글의 시작과 길이(원문 단위).
struct ParagraphSortText {
    std::u16string text;
    std::size_t from = 0;
    std::size_t length = 0;
};
// 문단의 HWP 원문(문단 끝 없이). charStarts 를 주면 캐럿 글자마다(글 조각의 코드 포인트, caretChar 가 참인 컨트롤)
// 원문 시작 자리와 끝(원문 길이)을 채운다.
std::u16string paragraphSortText(const Paragraph& para, std::vector<std::size_t>* charStarts,
                                 const std::function<bool(const Control&)>& caretChar);
// 정렬한 차례(order[i] = i 번째 자리에 올 문단). 바뀐 것이 없어도 차례를 준다. 하지 못하면 비우고 error 에 까닭.
std::vector<int> paragraphSortOrder(const std::vector<ParagraphSortText>& paras, const std::vector<TableSortKey>& keys,
                                    const ParagraphSortOptions& options, std::string* error);
// 단어 정렬(첫 기준 위치가 kSortWordPosition). 구분 기호로 나눈 단어의 앞뒤 빈칸(0x20 이하·유니코드 빈칸)을 뺀 자리가
// 칸이고, 단어 안 컨트롤을 뺀 글 조각(글자 모양째)을 정렬한 차례대로 문서 차례의 칸에 넣는다(한글처럼 누름틀 같은
// 개체는 옮기지 않는다). eachParagraph 면 문단마다 따로 정렬한다. paras 는 texts 와 같은 차례의 블록 문단이다.
// 차례가 바뀌지 않으면 고치지 않고 거짓이다(한글도 그때 실행 실패다).
bool sortParagraphWords(const std::vector<Paragraph*>& paras, const std::vector<ParagraphSortText>& texts,
                        const std::vector<TableSortKey>& keys, const ParagraphSortOptions& options, std::string* error);
// 블록 문단 가운데 가장 많이 나뉜 필드 수(대화 상자 위치 목록).
int paragraphSortFieldCount(const std::vector<ParagraphSortText>& paras, const ParagraphSortOptions& options);

// 표 나누기(한글 TableSplitTable). row 줄부터 아래를 떼어 tail 로 준다. 첫 줄이거나, 그 경계를 가로지르는
// 합친 칸이 있으면 거절한다. 줄 크기·칸 모양은 그대로이고 tail 의 줄 번호는 0 부터다.
TableEdit splitTableRows(Table& table, int row, Table& tail);
// 표 붙이기(한글 TableMergeTable). next 의 줄을 table 아래에 잇는다. 두 표의 칸 경계를 합쳐 한 칸 그림으로
// 만들고(2 HWPUNIT 안으로 가까운 경계는 같은 선), 그 사이에 든 칸은 늘어난다. 칸 폭은 그대로다.
TableEdit appendTableRows(Table& table, Table next);

}  // namespace chwp::model
