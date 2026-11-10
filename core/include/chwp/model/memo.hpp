#pragma once

// 메모(한글 2024 메모 필드 %%me).
//   - 메모는 필드다(Control kind Field, ctrlId "%%me"). 본문 범위는 필드 시작과 끝(FieldEnd) 사이고, 메모 글은 subLists[0] 이다.
//   - 명령은 "MEMO/<모양 번호>/<메모 번호>/<FILETIME 아래 32비트>/<위 32비트>/<작성자>/<나머지>" 다.
//     모양 번호 65535 는 구역 메모 모양을 따른다. 메모 번호는 저장할 때 문서 차례로 1 부터 다시 매긴다.
//   - 답글은 원 메모 범위 안에 겹친 메모이고, memoParentId 가 원 메모의 필드 고유 id 다.

#include <cstdint>
#include <string>
#include <vector>

#include "chwp/model/document.hpp"

namespace chwp::model {

inline constexpr std::uint32_t kMemoShapeFollowSection = 0xFFFF;

struct MemoCommand {
    std::string kind = "MEMO";  // MEMO 또는 MEMO$U
    std::uint32_t shapeRef = kMemoShapeFollowSection;
    std::uint32_t number = 0;
    std::uint64_t created = 0;  // FILETIME(UTC, 100ns 단위)
    std::string author;
    std::string rest = "\\;;";  // 작성자 뒤 나머지 조각(한글이 쓴 그대로)
};

bool isMemoField(const Control& control);
MemoCommand parseMemoCommand(const std::string& command);
std::string memoCommandText(const MemoCommand& memo);

// 한글 기본 메모 모양: 너비 55mm, 실선 굵기 1, 선 #B6D7AE, 채우기 #F0FFE9, 활성 #CFF1C7.
MemoShape defaultMemoShape();
// 메모가 쓰는 모양: 필드 모양 번호(1 부터), 아니면 구역 모양 번호, 아니면 기본값.
MemoShape memoShapeFor(const Document& doc, const Section& section, const Control& memo);

// 문서 차례(구역, 문단, 표 칸·글상자 안까지)의 메모 필드. 답글도 든다.
std::vector<Control*> collectMemos(Document& doc);
std::vector<const Control*> collectMemos(const Document& doc);

// FILETIME(UTC) ↔ 현지 시각 글. 머리글은 "yyyy/MM/dd HH:mm", HWPX CreateDateTime 은 "yyyy-MM-ddTHH:mm:ssZ"(현지 시각에 Z).
std::string memoHeadDate(std::uint64_t filetime);
std::string memoCreateDateTime(std::uint64_t filetime);
// 지금 시각의 FILETIME(UTC).
std::uint64_t filetimeNow();

}  // namespace chwp::model
