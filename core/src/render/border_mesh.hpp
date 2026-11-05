#pragma once

// 표 테두리를 화면 GPU가 셰이더로 그린다.
//   칸을 구울 때: Painter 가 선분과 모서리 잇기를 픽셀로 칠하지 않고 HU 정수로 적는다(BorderPiece).
//   화면에서: 그 목록을 메시로 묶어 두고, 배율과 자리만 유니폼으로 바꿔 그린다.
// 조각 셰이더가 픽셀마다 strokeTableBorder·mitreTableCorner 와 같은 식으로 칠할지 정한다.
// 구운 배율과 같은 배율이면 CPU 비트맵과 픽셀까지 같다. 다른 배율이면 그 배율로 규칙을 다시 잰다.
// 메시는 칸마다 한 번 만들고 다시 만들지 않는다.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "include/core/SkMesh.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"

class SkCanvas;
class SkColorFilter;

namespace chwp::render::paint_detail {

// 테두리 한 조각. 좌표는 칸 비트맵의 HU 다(픽셀 = HU × 구운 배율).
// 선분은 (x0,y0)-(x1,y1) 중심선이고 inside 가 안쪽 방향이다.
// 모서리 잇기는 (x0,y0) 가 교점이고 inside 는 가로선, vInside 는 세로선 안쪽이다.
struct BorderPiece {
    std::int32_t x0 = 0;
    std::int32_t y0 = 0;
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
    std::int32_t widthHu = 0;
    std::uint8_t kind = 0;  // Painter::tableLineKind
    std::int8_t inside = 1;
    std::int8_t vInside = 1;
    bool corner = false;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// 셰이더가 읽는 정점 묶음. 16비트 색인 한도에 맞춰 조각을 나눈다.
struct BorderMesh {
    struct Chunk {
        sk_sp<SkMesh::VertexBuffer> vertices;
        sk_sp<SkMesh::IndexBuffer> indices;
        std::size_t vertexCount = 0;
        std::size_t indexCount = 0;
    };
    std::vector<Chunk> chunks;
    std::size_t bytes = 0;
    // 조각 중심선이 닿는 HU 범위와 가장 굵은 선. 그릴 때 화면 경계를 잰다.
    // 경계를 clip 으로 주면 Graphite 가 자르기를 건너뛰어 칸 밖으로 샌다.
    SkRect huBounds = SkRect::MakeEmpty();
    std::int32_t maxWidthHu = 0;
};

// 셰이더 컴파일은 처음 한 번이다. 실패하면 널이고 이유는 error 에 남는다.
sk_sp<SkMeshSpecification> borderMeshSpec(const char** error = nullptr);

BorderMesh makeBorderMesh(const std::vector<BorderPiece>& pieces);

// 칸 비트맵 픽셀 (0,0) 이 화면 origin 에 오고, 구운 배율 bakeScale 의 칸이 sx·sy 배로 놓였을 때 그린다.
// clip 은 칸이 차지하는 화면 범위다. CPU 가 비트맵 밖을 잘랐듯 그 밖은 그리지 않는다.
// filter 가 있으면 그 색 거르개를 얹는다(선택 반전).
void drawBorderMesh(SkCanvas* canvas, const BorderMesh& mesh, float bakeScale, float sx, float sy, SkPoint origin,
                    const SkRect& clip, sk_sp<SkColorFilter> filter = nullptr);

// 칸 id 별로 남긴 테두리. 칸을 구울 때마다 갈아 끼우고, 칸을 버리면 함께 버린다.
struct FrameBorders {
    float scale = 0;  // 구운 배율(픽셀/HU)
    std::vector<BorderPiece> pieces;
    BorderMesh mesh;
};

void storeFrameBorders(int id, float scale, std::vector<BorderPiece> pieces);
void dropFrameBorders(int id);
void clearFrameBorders();
bool frameHasBorders(int id);
// 재활용한 칸이 새 id 를 받을 때 테두리도 옮긴다. 옮길 것이 없으면 거짓.
bool cloneFrameBorders(int srcId, int dstId);
std::shared_ptr<const FrameBorders> frameBorders(int id);

// 칸을 구울 때 표 테두리를 적어 둘지. 웹 GPU 화면이면 켜진다. native 시험은 직접 켠다.
bool bordersOnScreen();
void forceBordersOnScreen(bool on);

}  // namespace chwp::render::paint_detail
