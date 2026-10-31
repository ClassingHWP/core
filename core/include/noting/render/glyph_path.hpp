#pragma once

#include <cstdint>
#include <vector>

namespace noting::render {

// em 1000 공간. y는 기준선에서 위쪽이다.
// 글자 박스(진행 폭)는 여기 없다. 기울이거나 장평으로 가로가 늘어도 배치 폭은 바꾸지 않는다.
struct GlyphPoint {
    int x = 0;
    int y = 0;
};

enum class GlyphVerb { Move, Line, Quad, Cubic };

// Quad는 제어점 c1, 끝점 to. Cubic은 c1, c2, to.
struct GlyphCommand {
    GlyphVerb verb = GlyphVerb::Move;
    GlyphPoint to{};
    GlyphPoint c1{};
    GlyphPoint c2{};
};

// Windows가 이탤릭 윤곽에 넣는 기울기. x += y * 0x5700 / 65536.
int gdiItalicX(int x, int y);

// 기울임과 굵기를 윤곽 명령에 반영한다. 직선·quad·cubic 은 그대로 둔다. Skia 는 이 명령을 받아 채운다.
std::vector<GlyphCommand> styleGlyphCommands(std::vector<GlyphCommand> commands, bool italic, int boldPpem);

// 굵은 글꼴 파일이 없을 때 윤곽을 미는 픽셀. ppem은 이 윤곽을 그릴 픽셀 크기이고, 0 이하면 그대로 둔다.
// 세로는 (20×ppem−10)/1000, 가로는 그보다 1픽셀 많다. 왼쪽·아래는 그대로 두고 오른쪽·위로 민다.
// 민 뒤 가로를 (ppem+1)/(ppem+가로픽셀), 세로를 ppem/(ppem+세로픽셀)로 줄인다.
// 진행 폭은 바꾸지 않는다. 바깥 윤곽은 키우고, 다른 윤곽 안의 구멍은 줄인다.
void emboldenGdiOutline(std::vector<GlyphCommand>& commands, int ppem);

// 한글이 윤곽을 펴는 방식. 직선은 max(|dx|,|dy|)/20(최소 2), 2차는 3차로 올린 뒤 20점.
// italic이면 펴기 전에 기울인다. boldPpem이 0보다 크면 기울인 뒤에 GDI 굵기를 넣는다.
// 반환은 닫아 채울 다각형뿐이다.
std::vector<std::vector<GlyphPoint>> flattenHangulGlyph(std::vector<GlyphCommand> commands, bool italic,
                                                        int boldPpem = 0);

// 같은 펴기를 직선 명령으로 돌려준다. 기울임·굵기는 호출 전에 반영된 것으로 보고 다시 넣지 않는다.
std::vector<GlyphCommand> flattenHangulCommands(std::vector<GlyphCommand> commands);

// GetGlyphOutline(GGO_NATIVE) 버퍼. 정수부만 읽는다. 이미 굵기·이탤릭이 들어 있으면 italic은 다시 주지 않는다.
std::vector<GlyphCommand> commandsFromGgoNative(const std::uint8_t* data, int size);

}  // namespace noting::render
