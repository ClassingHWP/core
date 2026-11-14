#pragma once

// 변경 내용 추적(한글 2024 검토 > 변경 내용 추적). 규칙은 track_commands.cpp 머리 설명.

#include <string>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/paint.hpp"

namespace chwp::render {

// 기록 중인가(머리 trackchageConfig 1 번 비트).
bool trackRecording(const model::Document& doc);
// 바뀐 글을 기록할 작성자 이름. 비면 "사용자" 다. 셸이 사용자 설정을 넘긴다.
void setTrackAuthorName(const std::string& name);
std::string trackAuthorName();

// 기록을 켜고 끈다. 문서 값만 바꾸고 되돌리기 칸은 만들지 않는다.
EditResult editToggleTrackChanges(model::Document& doc, std::vector<Node>& pages);
// 변경 내용 적용·취소. scope: "caret"(캐럿·블록의 바뀜), "all"(문서 전체). 되돌리기 한 칸이다.
EditResult editResolveChanges(model::Document& doc, std::vector<Node>& pages, bool accept, const std::string& scope,
                              bool useStoredLineHeight, std::string* why = nullptr);
// editResolveChanges 를 되돌리기 한 칸으로 감싼다(undo.cpp).
EditResult editResolveChangesUndo(model::Document& doc, std::vector<Node>& pages, bool accept, const std::string& scope,
                                  bool useStoredLineHeight, std::string* why = nullptr);
// 이전·다음 바뀜으로 가서 그 바뀜을 블록으로 고른다. 없으면 handled 가 거짓이다.
EditResult editMoveToChange(model::Document& doc, std::vector<Node>& pages, bool forward);
// 문서에 바뀜이 있는가(문단 끝 포함).
bool documentHasChanges(const model::Document& doc);

}  // namespace chwp::render
