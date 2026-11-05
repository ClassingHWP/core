#include "chwp/input/wheel_reader.hpp"

#include <algorithm>
#include <cmath>

namespace chwp::input {

namespace {

// 한 줄은 16 CSS 픽셀이다(브라우저 기본).
constexpr double kLinePx = 16;
// 끊어 돌리는 휠 한 칸의 크기 하한(CSS 픽셀).
constexpr double kNotchPx = 40;
// 이만큼 쉬면 휠이 끝났다. 확대·축소는 조금 더 기다린다.
constexpr double kScrollEndMs = 90;
constexpr double kZoomEndMs = 140;
// 속도는 이 간격 안의 휠로만 잰다. 너무 빠르면 한 사건으로 본다.
constexpr double kVelocityMinMs = 1;
constexpr double kVelocityMaxMs = 80;
constexpr double kMaxVelocity = 8000;

bool notch(const RawWheel& raw) {
    if (raw.mode != 0) return true;
    if (raw.dx != 0) return false;
    const double absY = std::abs(raw.dy);
    if (absY < kNotchPx) return false;
    if (raw.zoom) return true;
    // 마우스 휠은 정수 픽셀(또는 장치 픽셀로 정수)로 온다. 트랙패드는 소수가 섞인다.
    const double device = absY * raw.dpr;
    return raw.dy == std::floor(raw.dy) || std::abs(device - std::round(device)) < 0.05;
}

}  // namespace

WheelEvent WheelReader::read(const RawWheel& raw, double now) {
    double dx = raw.dx;
    double dy = raw.dy;
    if (raw.mode == 1) {
        dx *= kLinePx;
        dy *= kLinePx;
    } else if (raw.mode == 2) {
        dx *= std::max(1.0, raw.pageW);
        dy *= std::max(1.0, raw.pageH);
    }
    const double dpr = raw.dpr > 0 ? raw.dpr : 1;
    WheelEvent out;
    out.phase = WheelPhase::Changed;
    out.dx = dx * dpr;
    out.dy = dy * dpr;
    out.x = raw.x;
    out.y = raw.y;
    out.zoom = raw.zoom;
    const bool discrete = notch(raw);
    if (lastAt_ < 0) runDiscrete_ = true;
    runDiscrete_ = runDiscrete_ && discrete;
    out.discrete = runDiscrete_;
    const double dt = lastAt_ >= 0 ? now - lastAt_ : 0;
    if (!out.discrete && dt >= kVelocityMinMs && dt < kVelocityMaxMs) {
        out.vx = std::clamp(out.dx / dt * 1000.0, -kMaxVelocity, kMaxVelocity);
        out.vy = std::clamp(out.dy / dt * 1000.0, -kMaxVelocity, kMaxVelocity);
    }
    lastAt_ = now;
    endAt_ = now + (raw.zoom ? kZoomEndMs : kScrollEndMs);
    return out;
}

std::optional<WheelEvent> WheelReader::endIfDue(double now) {
    if (lastAt_ < 0 || now < endAt_) return std::nullopt;
    WheelEvent out;
    out.phase = WheelPhase::Ended;
    out.discrete = runDiscrete_;
    lastAt_ = -1;
    runDiscrete_ = true;
    return out;
}

}  // namespace chwp::input
