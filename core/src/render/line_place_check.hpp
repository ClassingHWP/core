#pragma once

// 글자를 그린 자리와 캐럿·블록이 쓰는 글자 경계가 같은지 견준다.
// 둘 다 Painter::placeLine 에서 나와야 한다. 다른 식이 끼면 이 검사가 잡는다.

#include <cstdint>
#include <string>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/scene.hpp"

namespace chwp::render {

struct LinePlaceCheck {
    long lines = 0;
    long compared = 0;    // 견준 글자 수
    long mismatched = 0;  // 그린 자리와 캐럿 경계가 다른 글자 수
    std::string firstMiss;
};

// 쪽 안 모든 줄(개체 안 줄 포함)에서, 그린 글자의 왼쪽과 그 글자 앞 캐럿 경계를 견준다.
LinePlaceCheck checkLinePlaces(const model::Document& doc, const std::vector<Node>& pages);

// 글자 뒤에 둔 캐럿 상자의 위와, 그 글자를 그린 상자의 위를 견준다. 위·아래 첨자는 그리기만 옮기므로 뺀다.
LinePlaceCheck checkCaretTops(const model::Document& doc, const std::vector<Node>& pages);

// 캐럿 줄 캐시가 돌려주는 경계와, 캐시 없이 새로 잰 경계. 줄 x=0 기준이다.
std::vector<std::int32_t> cachedCaretEdges(const model::Document& doc, const Node& line);
std::vector<std::int32_t> freshCaretEdges(const model::Document& doc, const Node& line);

}  // namespace chwp::render
