#pragma once
// 교정 부호(한글 검토-교정 부호). 파일에서는 필드다.
//   - HWP: CTRL_HEADER 는 %unk, 글 쪽 확장 문자에 진짜 id(%%*d, %sig 등, HWP 5.0 규격 표 154)를 둔다. 명령 뒤 u32 필드 id,
//     u32 차례 번호(띄움표 같은 %sig 는 0). 한글 2024 가 저장한 파일로 확인했다.
//   - HWPX: hp:fieldBegin type="PROOFREADING_MARKS_<종류>", fieldid 는 id 네 글자를 정수로 읽은 값, zorder 는 차례 번호
//     (%sig 는 -1), 매개변수 Prop·Command.
//   - 명령: "$Revision<종류>;" 뒤에 값. 띄움표·줄 바꿈표·줄 비움표·부호 넣음표·넣음표는 %sig 이고 "$RevisionSign;1;"처럼
//     번호(1 띄움, 2 줄 바꿈, 3 줄 비움, 14 부호 넣음, 15 넣음)를 둔다. 넣을 글은 "$RevisionSign?글;15;"다.
//     고침표는 "$RevisionSimpleChange?글;", 자리 옮김표는 "$RevisionRightMove;2000;3000;"이다.
//   - 메모 고침표(%%mr, PROOFREADING_MARKS_CHANGE)는 메모와 같은 모양(명령·매개변수·메모 글)이라 메모로 다룬다.
// 범위가 있는 부호(지움표 등)는 필드 시작과 끝 사이 글이고, 자리만 있는 부호(%sig, 자리 옮김표)는 시작 바로 뒤에 끝이 온다.
#include <string>
#include <string_view>

#include "chwp/model/document.hpp"

namespace chwp::model {

enum class ProofKind {
    None,
    Delete,        // 지움표 %%*d
    Attach,        // 붙임표 %%*a
    Clipping,      // 뺌표 %%*C
    Sawtooth,      // 톱니표 %%*S
    Thinking,      // 생각표 %%*T
    Praise,        // 칭찬표 %%*P
    Line,          // 줄표 %%*L
    SimpleChange,  // 고침표 %%*c
    Hyperlink,     // 자료 연결 %%*h
    LineAttach,    // 줄 붙임표 %%*A
    LineLink,      // 줄 이음표 %%*i
    LineTransfer,  // 줄 서로 바꿈표 %%*t
    RightMove,     // 오른 자리 옮김표 %%*r
    LeftMove,      // 왼 자리 옮김표 %%*l
    Transfer,      // 자리 바꿈표 %%*n
    SimpleInsert,  // %%*e
    Split,         // %spl
    Change,        // 메모 고침표 %%mr
    Sign,          // 띄움표·줄 바꿈표·줄 비움표·부호 넣음표·넣음표 %sig
};

// 컨트롤 id(%%*d 등) → 종류. 교정 부호가 아니면 None.
ProofKind proofKindOfId(std::string_view ctrlId);
ProofKind proofKindOf(const Control& control);
// HWPX 필드 종류 이름(PROOFREADING_MARKS_DELETE 등) ↔ 컨트롤 id. 모르는 이름·id 는 빈 값이다.
std::string proofCtrlIdOfType(std::string_view hwpxType);
const char* proofHwpxType(std::string_view ctrlId);
// 컨트롤 id 네 글자를 큰 끝으로 읽은 정수(HWPX fieldid). %%*d → 0x25252A64.
std::uint32_t ctrlIdNumber(std::string_view ctrlId);
// %sig 의 번호(1 띄움표 ...). 명령에서 읽는다. 없으면 0.
int proofSignNumber(const std::string& command);
// 고침표·넣음표·부호 넣음표가 넣을 글. 명령의 '?' 뒤 ';' 앞이다. 없으면 빈 글.
std::string proofText(const std::string& command);
// 자리 옮김표 명령 "$RevisionRightMove;간격;시작;"의 값(HWPUNIT). 명령 값은 1pt = 200 이다(한글 2024 대화 상자
// "시작 위치 0.0pt 부터 10.0pt 간격만큼 옮김"이 "2000;0"). 읽지 못하면 거짓.
bool proofMove(const std::string& command, std::int32_t& start, std::int32_t& gap);
std::string proofMoveCommand(bool right, double startPt, double gapPt);

// 한글 교정 부호 이름(지움표, 띄움표 ...).
std::string proofName(const Control& control);

}  // namespace chwp::model
