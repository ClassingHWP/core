#pragma once

// 플랫폼 휠 값을 core 휠 사건으로 읽는다. 단위 바꾸기, 끊어 돌리는 휠(마우스)과 미끄러지는 휠(트랙패드) 가르기,
// 속도, 휠이 끝났는지 판단을 여기서 한다. 셸은 플랫폼이 준 값을 그대로 넘긴다.

#include <optional>

#include "chwp/input/viewport.hpp"

namespace chwp::input {

// 플랫폼이 준 휠 한 번. 웹이면 WheelEvent 의 deltaX·deltaY·deltaMode 와 Ctrl·Cmd 다.
struct RawWheel {
    double dx = 0;
    double dy = 0;
    int mode = 0;          // 0 픽셀(CSS), 1 줄, 2 쪽
    bool zoom = false;     // Ctrl·Cmd 를 누른 휠은 확대·축소다
    double x = 0;          // 장치 픽셀 자리
    double y = 0;
    double pageW = 0;      // 한 쪽 단위(mode 2)의 CSS 크기. 스크롤 칸 크기다
    double pageH = 0;
    double dpr = 1;
};

class WheelReader {
public:
    // now 는 core 시계(밀리초)다. 플랫폼 시각은 스레드마다 기준이 달라 쓰지 않는다.
    WheelEvent read(const RawWheel& raw, double now);
    // 휠이 멈췄으면 끝 사건을 준다. 프레임마다 또는 틱마다 부른다.
    std::optional<WheelEvent> endIfDue(double now);
    bool pending() const { return lastAt_ >= 0; }

private:
    double lastAt_ = -1;
    double endAt_ = 0;
    bool runDiscrete_ = true;
};

}  // namespace chwp::input
