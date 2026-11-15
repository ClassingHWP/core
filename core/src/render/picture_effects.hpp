#pragma once

// 그림 효과(그림자·네온·반사)가 차지하는 자리. 조판과 그리기가 같은 계산을 쓴다.
//
// 한글은 효과까지 넣은 상자를 개체 크기로 쓴다. 줄 높이·글 흐름·쪽 넘김이 이 상자를 따른다.
// 그림은 상자 안에서 (left, top) 만큼 들어가 놓인다. 바깥 여백은 이 상자 바깥에 더한다.
//
// 계산은 96dpi 화소(75 HWPUNIT)에서 float 로 한다. 차례는 네온 → 그림자 → 반사이고, 각 효과는 그때까지 쌓인
// 상자를 받는다(그림자는 네온까지 든 모양의 그림자다). 정렬 기준점은 늘 그림 사각형에서 잡는다.
//   네온   반경만큼 사방으로 넓힌다.
//   그림자 기준점을 두고 기울이고(cos·sin) 늘인 뒤 방향·거리만큼 옮기고, 흐림 반경을 화소로 올림해 넓힌다.
//          안쪽 그림자와 가로·세로 배율이 0 인 그림자는 상자에 들지 않는다.
//   반사   그림자와 같은 변환에 자리를 화소로 반올림하고, 높이에 끝 위치를 곱한다.
// 마지막에 x·y·너비·높이를 HWPUNIT 으로 되돌려 네 변을 각각 버린다.
// 한글 2024 가 조판한 줄 높이 80가지(그림자 단독·조합, 네온, 반사 거리·방향·끝 위치·정렬, 바깥 여백)로 맞췄다.

#include <cstdint>

#include "chwp/model/document.hpp"

namespace chwp::render {

// 그림 사각형 바깥으로 효과가 넘는 길이(HWPUNIT). 모두 0 이상이다.
struct EffectBox {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    bool empty() const { return left == 0 && top == 0 && right == 0 && bottom == 0; }
};

EffectBox pictureEffectBox(const model::PictureEffects& effects, std::int32_t width, std::int32_t height);

// 그리기용. 그림 왼쪽 위를 원점으로 한 HWPUNIT 아핀 변환 x' = a·x + b·y + c, y' = d·x + e·y + f.
struct EffectAffine {
    double a = 1, b = 0, c = 0, d = 0, e = 1, f = 0;
};

// 그림자·반사 한 겹을 어디에 그릴지. source 는 그 효과가 받는 모양(그림, 네온이 있으면 네온까지)이다.
struct EffectLayer {
    EffectAffine transform;  // source 모양을 옮기는 변환
    double blur = 0;         // 흐림 반경(HWPUNIT, 화소로 올림한 값)
};

EffectLayer shadowLayer(const model::PictureEffects::Shadow& shadow, std::int32_t width, std::int32_t height);
EffectLayer reflectionLayer(const model::PictureEffects::Reflection& reflection, std::int32_t width,
                            std::int32_t height);

}  // namespace chwp::render
