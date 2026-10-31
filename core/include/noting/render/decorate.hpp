#pragma once

#include <algorithm>
#include <cstdint>

#include "noting/render/scene.hpp"

namespace noting::render {

// 그리는 번호는 저장값+1이다. 표 칸 선과 같다. 12·13은 이중실선이고, 14 이상은 그리지 않는다.
inline bool decorationLineVisible(int storedShape) {
    const int kind = storedShape + 1;
    return kind >= 1 && kind <= 13;
}

inline const char* decorationLineType(int storedShape, const char* name) {
    const int kind = storedShape + 1;
    if (kind == 12 || kind == 13) return "DOUBLE_SLIM";
    return name && name[0] ? name : "SOLID";
}

// 밑줄·취소선 굵기. 저장값 0부터 5는 33, 6은 84, 7과 12는 141, 8부터 10은 708, 11은 283.
inline int decorationStrokeHu(int storedShape) {
    if (storedShape >= 8 && storedShape <= 10) return 708;
    if (storedShape == 11) return 283;
    if (storedShape == 7 || storedShape == 12) return 141;
    if (storedShape == 6) return 84;
    return 33;
}

// 양수에서는 (a × b + c / 2) / c. c / 2는 0 쪽으로 자른 절반이다.
inline std::int32_t mulDivRound(int a, int b, int c) {
    if (c == 0) return 0;
    const std::int64_t prod = static_cast<std::int64_t>(a) * b;
    const std::int64_t half = c / 2;
    if (prod >= 0) return static_cast<std::int32_t>((prod + half) / c);
    return static_cast<std::int32_t>((prod - half) / c);
}

// 쪽 본문 밑줄. 0은 줄 글자 상자 위다. 위치 1은 상자 높이를 더하고, 3은 더하지 않는다.
// 펜 중심은 그 점에서 굵기 절반만큼, 아래는 더 내려가고 위는 더 올라간다.
inline std::int32_t decorationLineY(int boxH, int position, int storedShape) {
    if (boxH <= 0) boxH = 1000;
    const int half = decorationStrokeHu(storedShape) / 2;
    if (position == 3) return -half;
    return boxH + half;
}

// 쪽 본문 취소선. 글자 기준선에서 이 값만큼 더한다. 아래로 갈수록 크다.
// 한글의 취소선 위치는 크기의 85%에서 시작한다. 글꼴 기준에서는 그 기준선보다 35% 위인 글자 상자 한가운데를 돌려준다.
inline std::int32_t strikeLineY(int fontSize) {
    if (fontSize <= 0) fontSize = 1000;
    return -mulDivRound(fontSize, 35, 100);
}

// 문단 테두리 상자. ox, oy는 문단 부모의 원점. 간격은 글과 테두리 사이이고, 문단 위·아래 간격은 상자 밖에 둔다.
inline void paragraphBorderBox(const Node& node, std::int32_t ox, std::int32_t oy, std::int32_t& left,
                               std::int32_t& top, std::int32_t& right, std::int32_t& bottom) {
    std::int32_t contentTop = 0;
    std::int32_t contentBottom = node.height;
    bool lined = false;
    for (const Node& child : node.children) {
        if (child.kind != NodeKind::Line) continue;
        const std::int32_t y0 = child.y;
        const std::int32_t y1 = child.y + std::max(child.height, child.textHeight);
        if (!lined) {
            contentTop = y0;
            contentBottom = y1;
            lined = true;
        } else {
            contentTop = std::min(contentTop, y0);
            contentBottom = std::max(contentBottom, y1);
        }
    }
    if (node.borderIgnoreMargin) {
        left = ox + node.x;
        right = left + node.marginLeft + node.width + node.marginRight;
    } else {
        left = ox + node.x + node.marginLeft;
        right = left + node.width;
    }
    top = oy + node.y + contentTop;
    bottom = oy + node.y + contentBottom;
    left -= node.borderOffsetLeft;
    right += node.borderOffsetRight;
    top -= node.borderOffsetTop;
    bottom += node.borderOffsetBottom;
}

inline bool sameParagraphBorder(const Node& a, const Node& b) {
    if (!a.borderConnect || !b.borderConnect) return false;
    auto eq = [](const BorderEdge& x, const BorderEdge& y) {
        return x.type == y.type && x.color == y.color && x.width == y.width;
    };
    return eq(a.borderLeft, b.borderLeft) && eq(a.borderRight, b.borderRight) && eq(a.borderTop, b.borderTop) &&
           eq(a.borderBottom, b.borderBottom);
}

// 1 위 선 생략, 2 아래 선 생략. 테두리 연결이 켜진 이웃 문단과 맞닿은 변이다.
inline int paragraphBorderJoin(const Node& parent, int index) {
    if (index < 0 || index >= static_cast<int>(parent.children.size())) return 0;
    const Node& node = parent.children[static_cast<std::size_t>(index)];
    if (node.kind != NodeKind::Paragraph || !node.borderConnect) return 0;
    int join = 0;
    if (index > 0 && sameParagraphBorder(parent.children[static_cast<std::size_t>(index - 1)], node)) join |= 1;
    if (index + 1 < static_cast<int>(parent.children.size()) &&
        sameParagraphBorder(node, parent.children[static_cast<std::size_t>(index + 1)])) {
        join |= 2;
    }
    return join;
}

}  // namespace noting::render
