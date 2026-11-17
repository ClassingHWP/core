#pragma once

// 되돌리기 기록. 문서 하나의 편집 상태(EditState)가 커서와 함께 갖는다. 기록하고 되돌리는 차례는 undo.cpp 에 있다.

#include <cstddef>
#include <vector>

#include "chwp/model/document.hpp"

namespace chwp::render::paint_detail {

enum class UndoKind { Typing, Deleting, Other, Sections };

// 캐럿을 글자 번호로 들고 있는다. 화면 자리(offset)는 다시 놓은 쪽에서 바꾼다.
struct CaretMark {
    int story = -1;
    int anchorParagraph = -1;
    int anchorChar = 0;
    int paragraph = -1;
    int focusChar = 0;
    // 편집할 때 골라 둔 개체(글 묶음·문단·문단 안 항목 순번). 되돌리면 한글처럼 그 개체를 다시 고른다.
    // 편집 앞 모습에만 적는다(숨긴 한글 2024: 다시 하기는 개체를 고르지 않는다).
    int objectStory = -1;
    int objectParagraph = -1;
    int objectItem = -1;
    bool operator==(const CaretMark& other) const {
        return story == other.story && anchorParagraph == other.anchorParagraph && anchorChar == other.anchorChar &&
               paragraph == other.paragraph && focusChar == other.focusChar && objectStory == other.objectStory &&
               objectParagraph == other.objectParagraph && objectItem == other.objectItem;
    }
};

struct UndoRecord {
    UndoKind kind = UndoKind::Other;
    // 문단 칸: story 의 first 부터 count 문단을 paragraphs 로 바꾸면 이 편집 앞(되돌린 뒤) 모습이다.
    int story = -1;
    int first = 0;
    int count = 0;
    std::vector<model::Paragraph> paragraphs;
    // 구역 칸(편집 용지·막힌 글꼴 바꾸기): 구역 전체와 글꼴 표를 맞바꾼다.
    std::vector<model::Section> sections;
    model::PerLang<std::vector<model::Font>> fonts;
    std::vector<model::Style> styles;
    CaretMark caretBefore;  // 이 칸을 적용한 뒤 캐럿
    CaretMark caretAfter;   // 이 칸을 적용하기 전 캐럿(이어 치기 판단)
    bool closed = false;    // 되돌리기·다시 하기를 거쳤으면 더 잇지 않는다
    // 0 이 아니면 같은 번호의 칸을 한 번에 되돌린다(beginUndoGroup ~ endUndoGroup).
    int group = 0;
    // 칸 번호. 되돌리기·다시 하기로 오가도 같다. 저장한 때의 맨 위 칸과 견주어 문서가 바뀌었는지 본다.
    int id = 0;
};

inline constexpr std::size_t kUndoLimit = 200;

struct UndoHistory {
    const model::Document* doc = nullptr;
    std::vector<UndoRecord> undo;
    std::vector<UndoRecord> redo;
    // 지금 열린 묶음 번호. 0 이면 묶지 않는다.
    int openGroup = 0;
    int nextGroup = 1;
    int nextId = 1;
    // 저장(또는 열기)한 때의 맨 위 칸 번호. 칸이 없었으면 0 이다.
    int savedId = 0;
};

// 지금 편집 상태의 되돌리기 기록.
UndoHistory& undoHistory();

}  // namespace chwp::render::paint_detail
