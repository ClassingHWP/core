#pragma once

#include "render/gpu_config.hpp"

// 2D 화면 그림 목록을 받아 적는 Skia 캔버스. presentScreen 이 이 캔버스에 그리면 그 그리기가 목록이 된다.

#if CHWP_HAS_GPU
#include <functional>
#include <memory>

class SkCanvas;
class SkImage;

namespace chwp::render {

struct ScreenFrame2D;

// imageKey 는 그림을 셸에 올린 번호로 바꾼다. 0 이면 그 그림은 건너뛴다.
std::unique_ptr<SkCanvas> makeScreenOpCanvas(int width, int height, ScreenFrame2D& frame,
                                             std::function<int(const SkImage*)> imageKey);

}  // namespace chwp::render
#endif
