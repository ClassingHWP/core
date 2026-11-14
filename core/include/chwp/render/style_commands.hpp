#pragma once

// 스타일 대화 상자(한글 2024 스타일, F6)와 스타일 추가·편집·지우기·차례 바꾸기·현재 모양으로 바꾸기·글자 스타일 해제.
// 스타일을 바꾸면 그 스타일을 쓰는 문단·글이 따라 바뀐다. 옛 스타일과 다르게 직접 준 모양은 남는다(스타일 적용과 같다).

#include <cstdint>
#include <string>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/paint.hpp"

namespace chwp::render {

// 스타일 목록 한 줄. shortcut 은 Ctrl+1~0 번호(1~10)이고 없으면 0 이다.
struct StyleRow {
    std::uint32_t id = 0;
    std::string name;
    std::string engName;
    std::string type = "PARA";
    std::uint32_t nextStyleId = 0;
    int shortcut = 0;
};

std::vector<StyleRow> styleRows(const model::Document& doc);

// 스타일 대화 상자 오른쪽 정보: marginLeft·indent·marginRight(HWPUNIT), align, lineSpacingType, lineSpacing,
// nextStyle(이름), font(대표 글꼴), ratio, height(HWPUNIT), spacing, headingType(NONE·OUTLINE·NUMBER·BULLET).
FormatFields styleSummary(const model::Document& doc, std::uint32_t styleId);

// 추가·편집 중인 스타일. scratch 는 문서 머리말을 베낀 것으로, 글자·문단 모양 대화 상자가 여기 모양을 고친다.
struct StyleDraft {
    bool active = false;
    int editing = -1;  // 고치는 스타일 번호. 새 스타일이면 -1
    std::string name;
    std::string engName;
    std::string type = "PARA";
    std::uint32_t nextStyleId = 0;
    model::Document scratch;
    CaretPlace place;  // scratch 안의 스타일 모양(charShapeId·paraShapeId·styleId)
};

// editing < 0 이면 캐럿 문단·글자 모양으로 새 스타일을 시작한다(다음 스타일은 새 스타일 자신).
StyleDraft beginStyleDraft(const model::Document& doc, const std::vector<Node>& pages, int editing);
// name·engName·type·nextStyleId 칸.
FormatFields styleDraftFields(const StyleDraft& draft);
void setStyleDraftFields(StyleDraft& draft, const FormatFields& fields);
// 글자(char)·문단 모양 대화 상자의 패치를 초안 모양에 넣는다.
void applyStyleDraftFormat(StyleDraft& draft, bool para, const FormatFields& resolved);

// 되돌리기 한 칸씩 기록하는 명령. 못 하면 why 에 까닭을 쓴다.
EditResult editCommitStyle(model::Document& doc, std::vector<Node>& pages, const StyleDraft& draft,
                           bool useStoredLineHeight, std::string* why);
// 지운 스타일을 쓰던 문단·글은 replacement 로 바뀐다. 첫 스타일(바탕글)은 지우지 않는다.
EditResult editDeleteStyle(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId,
                           std::uint32_t replacement, bool useStoredLineHeight, std::string* why);
// 목록에서 한 칸 위(-1)·아래(+1)로. 스타일 번호와 그 번호를 쓰는 곳이 모두 바뀐다.
EditResult editMoveStyle(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId, int delta,
                         bool useStoredLineHeight, std::string* why);
// 캐럿 문단·글자 모양을 그 스타일 모양으로 삼는다(현재 모양으로 바꾸기).
EditResult editStyleFromCaret(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId,
                              bool useStoredLineHeight, std::string* why);
// 다른 문서(source)의 스타일을 이 문서로 베낀다(한글 스타일 가져오기). names 가 비면 모두다. 같은 이름 스타일은
// 덮어쓰고 그 스타일을 쓰는 곳이 따라 바뀐다. 글꼴·테두리·탭·번호는 이름이나 내용이 같은 것을 쓰고 없으면 더한다.
EditResult editImportStyles(model::Document& doc, std::vector<Node>& pages, const model::Document& source,
                            const std::vector<std::string>& names, bool useStoredLineHeight, std::string* why);
// 스타일만 담은 문서(스타일 내보내기). 머리말(글꼴·모양·스타일)은 이 문서 것이고 본문은 빈 문단 하나다.
model::Document styleOnlyDocument(const model::Document& doc);
// 블록(없으면 캐럿 글자 스타일 구간)의 글자 스타일을 풀고 문단 스타일 글자 모양으로 돌린다.
EditResult editClearCharStyle(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                              std::string* why);

}  // namespace chwp::render
