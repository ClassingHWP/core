#pragma once

// 서식 대화 상자(글자 모양·문단 모양·편집 용지)가 보이는 값과 그 값을 고치는 법.
// 셸은 칸 이름과 칸에 친 값을 그대로 넘기고, 받은 값을 그대로 보인다. 단위 바꾸기, 범위, 기본값,
// 고를 수 있는 값, 함께 바뀌는 값(양각·음각, 용지 종류, 격자 줄 수)은 모두 여기서 정한다.
//
// 칸 이름:
//   이름@pt, 이름@mm  길이 칸. 화면 단위 값이다. 문서에는 HWPUNIT 으로 들어간다.
//   그 밖              문서 값 이름(format_fields) 또는 아래 보기 전용 이름.
// 보기 전용 이름(글자·문단): plain, underlineOn, outlineOn, shadowOn, strikeView, indentMode,
//   indentValue@pt, lineSpacingView, condenseMin, hangulBreak, latinBreak, oneLine, tabList, tabAdd, tabRemove,
//   tabClear, penType, penWidth, penColor(char.·para. 접두).
// 보기 전용 이름(편집 용지): paperKind, gridMode, lineCount, cellCount, marginPreset, wholeDefault, sheet*.

#include <string>
#include <utility>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"

namespace chwp::render {

// 칸 하나의 설명. 숫자 칸은 단위·범위·한 칸·자릿수, 고르는 칸은 고를 수 있는 값이다.
struct FieldSpec {
    std::string key;
    std::string unit;  // pt, mm, %, 빈 글(단위 없음)
    double min = 0;
    double max = 0;
    double step = 1;
    int digits = 0;
    // 고를 수 있는 값과 보일 이름. 이름이 비어 있으면 셸이 값 이름으로 이름을 붙인다.
    std::vector<std::pair<std::string, std::string>> options;
};

struct FormatView {
    FormatFields values;
    std::vector<FieldSpec> specs;
};

// PageBorder 는 쪽 테두리/배경(한글 2024 쪽 테두리/배경 대화 상자)이다.
enum class FormatViewKind { Char, Para, Page, Table, PageBorder };

// 표 넣기 한도와 처음 값. 표 넣기(editInsertTable)도 이 한도로 자른다.
inline constexpr int kTableMaxRows = 500;
inline constexpr int kTableMaxCols = 64;
inline constexpr int kTableStartRows = 5;
inline constexpr int kTableStartCols = 5;

// base 를 주면 글자·문단 값은 캐럿 대신 그 모양(스타일 대화 상자가 고치는 스타일 모양)에서 읽는다.
// 대화 상자 칸 패치를 문서 값 이름으로 푼다. 칸 이름이 아닌 것은 버린다.
FormatFields resolveFormatView(const model::Document& doc, const std::vector<Node>& pages, FormatViewKind kind,
                               const FormatFields& viewPatch, const CaretPlace* base = nullptr);
// 패치를 적용했을 때 대화 상자가 보일 값. 문서는 바꾸지 않는다.
FormatView formatView(const model::Document& doc, const std::vector<Node>& pages, FormatViewKind kind,
                      const FormatFields& viewPatch, const CaretPlace* base = nullptr);
// 글자·문단 모양 미리보기. 패치를 적용한 글자·문단 모양으로 견본 글을 그린다.
PageBitmap renderFormatPreview(const model::Document& doc, const std::vector<Node>& pages, FormatViewKind kind,
                               const FormatFields& viewPatch, int width, int height, const CaretPlace* base = nullptr);

}  // namespace chwp::render
