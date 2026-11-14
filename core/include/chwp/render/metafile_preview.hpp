#pragma once

// OLE 개체·차트·메타파일 그림의 미리 보기 비트맵. 한글은 OLE 묶음의 \x02OlePres000 에 미리 그려 둔 그림
// (WMF·EMF 안의 DIB, 또는 DIB)을 둔다. 차트 OLE 의 미리 보기는 한글이 그린 차트 그대로다.
// 벡터로만 된 메타파일은 아직 그리지 않는다(비트맵 레코드가 없으면 거짓).

#include "chwp/common/bytes.hpp"

namespace chwp::render {

// OLE 묶음 바이트(HWP·HWPX 의 BinData. 앞 4 바이트 길이가 붙어 있을 수 있다)에서 미리 보기를 BMP 파일로 꺼낸다.
bool olePreviewBitmap(ByteView ole, Bytes& bmp);

// WMF(자리 머리 0x9AC6CDD7 이 있든 없든)·EMF 바이트에서 그림을 덮는 비트맵 레코드를 BMP 파일로 꺼낸다.
bool metafileBitmap(ByteView metafile, Bytes& bmp);

}  // namespace chwp::render
