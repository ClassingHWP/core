#pragma once

// 글맵시 윤곽. 한글 2024 가 PDF 로 그린 글맵시 55 모양을 재서 맞췄다(모양 0~47 은 경계 곡선으로 휘고,
// 48~54 는 글자를 원호에 통째로 얹는다). 결과는 개체 상자 안 0~1 좌표(y 는 아래로)의 윤곽이다.

#include <string>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/glyph_path.hpp"

namespace chwp::render {

struct ArtCommand {
    GlyphVerb verb = GlyphVerb::Move;
    float x = 0, y = 0;    // 끝점
    float c1x = 0, c1y = 0;  // Quad·Cubic 첫 제어점
    float c2x = 0, c2y = 0;  // Cubic 둘째 제어점
};

// 글(줄은 CR LF 나 CR), 글꼴, 글맵시 값으로 윤곽을 만든다. 글꼴이나 글자 윤곽이 아직 없으면 빈 목록이다.
std::vector<ArtCommand> textArtOutline(const std::string& text, const std::string& face, const model::TextArt& art);

// 글맵시 그림자(한글 2024 를 재서 맞춤). 그림자가 있으면(종류가 NONE 이 아니면, 자리 0 도) 휜 글의 잉크 상자를
// 개체 상자에서 그림자 몫(|offsetX|%, |offsetY|%)을 뺀 칸에 꽉 채운다. 그림자는 그 글을 개체 크기의 offsetX·offsetY %
// 만큼 옮긴 것이다(음수면 글이 반대쪽으로 밀린다). 비연속(DROP)·연속(CONTINUOUS)은 한글이 똑같이 그린다.
struct ArtShadowFit {
    std::vector<ArtCommand> text;
    std::vector<ArtCommand> shadow;
};
ArtShadowFit fitTextArtShadow(const std::vector<ArtCommand>& outline, int offsetXPercent, int offsetYPercent);

}  // namespace chwp::render
