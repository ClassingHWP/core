#pragma once

// 표 테두리 셰이더가 CPU 비트맵과 같은지 native Dawn 에서 픽셀로 맞춰 본다.
// Graphite Dawn 이 든 빌드(CHWP_SKIA_DAWN)에서만 돈다. 아니면 ran 이 거짓이다.

#include <string>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/scene.hpp"

namespace chwp::render {

struct BorderMeshCheck {
    bool ran = false;
    std::string report;
    int frames = 0;
    long pieces = 0;
    long mismatches = 0;      // 대각선 바깥에서 다른 픽셀
    long diagonalPixels = 0;  // 비트맵에 남은 대각선 반투명 픽셀이 색 1~2 단계 다른 곳
};

// 표·쪽 테두리 칸이 있는 쪽을 앞에서부터 pageLimit 쪽까지 percents 배율마다 칸으로 굽고, 표·쪽 테두리 칸을 두 길로 비교한다.
//   CPU: 지금처럼 비트맵에 칠한다.
//   GPU: 테두리를 적어 메시로 그리고, 남은 비트맵(대각선)을 그 위에 얹는다.
// 어댑터(D3D12·Vulkan·소프트웨어)마다 따로 돈다.
BorderMeshCheck checkBorderMeshDawn(const model::Document& doc, const std::vector<Node>& pages, int pageLimit,
                                    const std::vector<int>& percents);

}  // namespace chwp::render
