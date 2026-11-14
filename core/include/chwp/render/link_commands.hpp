#pragma once

// 하이퍼링크 따라가기와 이전·다음(한글 2024 HyperlinkPrev·HyperlinkNext). 규칙은 link_commands.cpp 머리 설명.

#include <string>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/paint.hpp"

namespace chwp::render {

// 하이퍼링크 명령이 가리키는 곳.
struct LinkTarget {
    enum class Kind { None, Web, DocumentStart, Bookmark, Outline, Table, Figure, Equation, OtherFile };
    Kind kind = Kind::None;
    std::string url;   // Web: 열 주소(풀어 쓴 글). OtherFile: 파일 이름
    std::string name;  // Bookmark: 책갈피 이름
    std::uint32_t id = 0;  // Outline: 문단 id, 표·그림·수식: 개체 id
};
LinkTarget parseLinkCommand(const std::string& command);

// 점(쪽 HU) 아래 하이퍼링크 글자가 있는가. 화면 커서(손 모양)와 누르기가 쓴다.
bool linkUnder(const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y);

// 점 아래 하이퍼링크를 따라간다. 따라간 링크 글은 방문한 색(#800080 글자·밑줄)으로 바뀌고 이것이 되돌리기 한 칸이다.
// 문서 안 대상이면 캐럿을 그리로 옮기고 이력에 넣는다. 웹 주소면 url 에 열 주소를 준다(셸이 Host 로 연다).
EditResult editFollowLinkAt(model::Document& doc, std::vector<Node>& pages, int page, int x, int y,
                            bool useStoredLineHeight, std::string* url, std::string* why);
// 이전(따라오기 전 링크 자리)·다음(다시 따라가기). 이력이 없으면 handled 가 거짓이다.
EditResult editLinkHistory(model::Document& doc, std::vector<Node>& pages, bool forward);

}  // namespace chwp::render
