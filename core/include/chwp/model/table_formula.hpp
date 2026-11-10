#pragma once

// 표 계산식(한글 2024 계산식 필드 %fmu, HWPX fieldBegin type="FORMULA").
// 규칙은 숨긴 한글에 같은 표를 만들어 TableFormula·TableFormulaSum/Avg/Pro(Auto·Hor·Ver)를 실행해 저장한 결과로 맞췄다
// (tests/test_table_formula.cpp).
//   - 명령은 "식??결과 형식;;마지막 결과"(형식이 없으면 "식;결과")다. 한글 계산식 필드의 기타 속성은 8 이다.
//   - 셀 이름은 칸 A, B, … 와 줄 1, 2, … 이고 합친 칸은 왼쪽 위 주소 하나다. ? 는 식이 든 칸의 줄(A?)·칸(?1)이다.
//   - 셀 값은 셀 글에서 처음 나오는 수다(앞의 글자는 건너뛰고, 쉼표는 무시, 지수·전각 숫자는 읽지 않는다).
//     수가 없어도 계산식이 든 셀은 0 이다. 식의 빈칸은 모두 지우고 읽는다.
//   - 범위와 쉬운 범위는 셀마다 인자 하나다(MOD(A1:B1) = MOD(A1,B1)). 평균의 ABOVE·LEFT 는 바로 위(왼쪽) 소계를 뺀다.
//   - 셀 필드 이름: LEFT:이름 등은 바로 이웃부터 이름이 같은 셀이 이어지는 동안이고(첫 인자로만, 뒤 인자는 버림),
//     ALL:이름 은 표 안의 이름 같은 셀 모두다. 이름은 영문 대소문자를 가리지 않는다.
//   - 결과 형식: %g 는 소수 6 자리에서 반올림(0 에서 먼 쪽)한 뒤 끝 0 을 지우고, %.Nf 는 값×10^N 을 반올림한다.
//     형식 끝의 쉼표는 세 자리마다 쉼표다. 계산하지 못하면 "잘못된 계산식"이다.
// 다시 계산은 한글을 따르지 않는 우리 규칙이다(한글은 키로 칠 때만, 표 차례로, 오류 칸은 0 으로 센다):
//   - 결과는 언제나 지금 칸 값으로 계산한 것과 같다. 어떤 편집이든 끝나면 바뀐 표를 다시 계산한다.
//   - 계산식이 다른 계산식 칸을 쓰면 그 칸을 먼저 계산한다(의존 차례). 서로 물고 돌면 "순환 참조"다.
//   - 오류(잘못된 계산식·순환 참조)인 계산식 칸을 쓰는 계산식도 오류다.
//   - 결과 글을 손으로 고치면 그 계산식은 풀려 보통 글이 된다(unwrapEditedFormulas).

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/model/document.hpp"

namespace chwp::model {

inline constexpr const char* kFormulaErrorText = "잘못된 계산식";
inline constexpr const char* kFormulaCycleText = "순환 참조";
inline constexpr std::uint8_t kFormulaFieldExtra = 8;

struct FormulaCommand {
    std::string formula;  // "=SUM(A1:B2)"
    std::string format;   // "%g,"
    std::string result;   // 마지막 결과 글
    bool hasFormat = false;
};
FormulaCommand parseFormulaCommand(std::string_view command);
std::string formulaCommandText(const FormulaCommand& f);

// 셀 글에서 읽은 수. 수를 이룰 글자(숫자·부호·소수점)가 없으면 없다.
std::optional<double> formulaCellNumber(std::string_view text);
// 셀 값으로 읽는 글: 문단 글을 문단 끝(\r)으로 이은 것.
std::string formulaCellText(const TableCell& cell);
// cell(table.cells 순번) 자리의 식 값. 셀 이름이 틀리거나 계산할 수 없으면 없다.
std::optional<double> evaluateTableFormula(const Table& table, std::size_t cell, std::string_view formula);
std::string formatFormulaValue(double value, std::string_view format);
// 식을 계산해 결과 형식으로 쓴 글(못 하면 "잘못된 계산식").
std::string formulaResultText(const Table& table, std::size_t cell, std::string_view formula, std::string_view format);

// 계산식 필드(%fmu)와 결과 글·필드 끝. 결과 글은 필드 시작과 같은 글자 모양이다.
Run formulaFieldRun(const std::string& command, std::uint32_t charShapeId);
// 표 안 계산식 결과를 의존 차례로 다시 계산해 결과 글과 명령의 마지막 결과를 바꾼다. 다른 계산식 칸을 쓰는 식은
// 그 칸 다음에 계산하고(같으면 칸 차례), 서로 물고 도는 식(과 그 식을 쓰는 식)은 "순환 참조"다.
// 바꾼 칸 문단의 lineSeg 는 버린다. 바뀐 것이 있으면 참.
bool recalcTableFormulas(Table& table);
// 계산식 cell 자리의 식이 값을 읽는 칸들(표 cells 순번, 중복 없음). 칸 이름·범위·쉬운 범위·ALL:이름을 칸으로 푼다.
std::vector<std::size_t> formulaInputCells(const Table& table, std::size_t cell, std::string_view formula);
// 문단의 계산식 필드 가운데 보이는 결과 글이 명령의 마지막 결과와 다른 것(손으로 고친 것)을 풀어 보통 글로 둔다.
// 필드 시작·끝만 빼고 글은 그대로다. 푼 수.
int unwrapEditedFormulas(Paragraph& para);
// 칸(cell) 문단 para 의 runs[run].items[item] 계산식 필드 하나를 다시 계산한다. 바뀌었으면 참.
bool refreshFormulaField(Table& table, std::size_t cell, Paragraph& para, std::size_t run, std::size_t item);
bool tableHasFormulas(const Table& table);

// 블록·쉬운 계산식의 함수.
enum class FormulaFunc { Sum, Average, Product };
// 기본 결과 형식: 평균은 "%.2f,", 나머지는 "%g,".
std::string formulaDefaultFormat(FormulaFunc func);
// 쉬운 계산식(TableFormulaSumHor 등): 가로는 마지막 칸이면 LEFT 아니면 RIGHT, 세로는 마지막 줄이면 ABOVE 아니면 BELOW.
std::string easyFormula(FormulaFunc func, bool horizontal, bool lastLine);
// 블록 계산식(TableFormulaSumAuto 등). 블록 마지막 칸의 셀에 수가 없으면 줄마다 그 칸에 "=SUM(A?:B?)" 를,
// 마지막 줄에 수가 없으면 칸마다 그 줄에 "=SUM(?1:?2)" 를, 둘 다면 모서리에 "=SUM(A1:B2)" 를 칸 맨 앞에 넣는다.
// 넣은 식 수(0 이면 넣을 칸이 없다).
int insertBlockFormulas(Table& table, int row0, int col0, int row1, int col1, FormulaFunc func);

}  // namespace chwp::model
