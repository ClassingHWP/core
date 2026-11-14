#pragma once

// 차례 만들기·새로 고침(한글 2024 차례 만들기 대화 상자, Ctrl+K,U·Ctrl+K,A). 규칙은 toc_commands.cpp 머리 설명.

#include <string>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/paint.hpp"

namespace chwp::render {

// 차례 만들기 설정. 기본값은 한글 대화 상자 처음 값이다.
struct TocOptions {
    bool field = true;       // 필드로 넣기(아니면 문자열로 넣기)
    bool outline = true;     // 개요 문단으로 모으기
    int level = 2;           // 개요 수준(0 이 1 수준). 한글 처음 값은 3 수준
    bool byStyle = false;    // 스타일로 모으기
    std::vector<std::string> styles;
    bool tables = true;      // 표 차례
    bool pictures = true;    // 그림 차례
    bool equations = true;   // 수식 차례
    bool autoTab = false;    // 문단 오른쪽 끝 자동 탭(아니면 오른쪽 탭)
    int leader = 3;          // 채울 모양(탭 채움 번호)
    bool hyperlink = true;   // 하이퍼링크 만들기(스타일로 모으면 만들지 않는다)
};

// 차례 필드 명령(한글과 같은 꼴)과 그 명령에서 읽은 설정.
std::string tocCommand(const TocOptions& options);
TocOptions tocOptionsOf(const std::string& command);
// 대화 상자 값: field·outline·byStyle·tables·pictures·equations·autoTab·hyperlink(참·거짓), level·leader(수),
// styles(줄마다 스타일 이름).
TocOptions tocOptionsOfFields(const FormatFields& fields);
// 탭 채움 번호 → HWPX 이름.
std::string tabLeaderName(int leader);

// 캐럿 문단 뒤에 차례를 넣는다. 되돌리기 한 칸.
EditResult editMakeToc(model::Document& doc, std::vector<Node>& pages, const TocOptions& options, bool useStoredLineHeight,
                       std::string* why);
// 캐럿이 있는 차례(all 이면 문서의 모든 차례)를 그 필드 설정대로 다시 만든다. 되돌리기 한 칸.
EditResult editUpdateToc(model::Document& doc, std::vector<Node>& pages, bool all, bool useStoredLineHeight,
                         std::string* why);

}  // namespace chwp::render
