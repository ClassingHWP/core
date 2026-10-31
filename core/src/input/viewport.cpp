#include "noting/input/viewport.hpp"

#include <algorithm>
#include <cmath>

namespace noting::input {
namespace {

bool contains(const render::Scrollbar& bar, double x, double y) {
    return bar.visible && bar.w > 0 && bar.h > 0 && x >= bar.x && y >= bar.y && x < bar.x + bar.w && y < bar.y + bar.h;
}

bool onThumb(const render::Scrollbar& bar, double x, double y) {
    return bar.visible && x >= bar.thumbX && y >= bar.thumbY && x < bar.thumbX + bar.thumbW && y < bar.thumbY + bar.thumbH;
}

int roundToInt(double value) {
    return static_cast<int>(std::llround(value));
}

}  // namespace

Camera ViewportInput::snapshot(const render::PageView& view, bool scrollChanged, bool zoomChanged, bool handled) const {
    const render::ViewportState now = view.state();
    Camera out;
    out.scrollX = now.scrollX;
    out.scrollY = now.scrollY;
    out.maxScrollX = now.maxScrollX;
    out.maxScrollY = now.maxScrollY;
    out.percent = now.percent;
    out.fitPage = now.fitPage;
    out.fitWidth = now.fitWidth;
    out.gridMode = now.gridMode;
    out.gridCols = now.gridCols;
    out.gridRows = now.gridRows;
    out.effectiveCols = now.effectiveCols;
    out.rowStepY = now.rowStepY;
    out.pageStepY = now.pageStepY;
    out.scrollChanged = scrollChanged;
    out.zoomChanged = zoomChanged;
    out.coasting = coast_;
    out.handled = handled;
    out.pinching = pinch_;
    out.draggingBar = barDrag_;
    out.wheeling = wheeling_;
    out.panning = middle_;
    out.rotation = rotation_;
    return out;
}

void ViewportInput::stopCoast() {
    coast_ = false;
    coastX_ = 0;
    coastY_ = 0;
    residualX_ = 0;
    residualY_ = 0;
    momentum_ = false;
    zoomAnim_ = false;
}

void ViewportInput::noteVelocity(double x, double y, double time) {
    if (sampleTime_ > 0 && time > sampleTime_) {
        const double dt = time - sampleTime_;
        if (dt >= 1 && dt < 120) {
            const double instantX = std::clamp((x - lastX_) / dt * 1000.0, -8000.0, 8000.0);
            const double instantY = std::clamp((y - lastY_) / dt * 1000.0, -8000.0, 8000.0);
            if (!sampleVel_) {
                velX_ = instantX;
                velY_ = instantY;
                sampleVel_ = true;
            } else {
                velX_ = velX_ * 0.4 + instantX * 0.6;
                velY_ = velY_ * 0.4 + instantY * 0.6;
            }
        } else {
            velX_ = 0;
            velY_ = 0;
            sampleVel_ = false;
        }
    }
    if (time > 0) sampleTime_ = time;
}

void ViewportInput::dropPointer() {
    tracking_ = false;
    dragged_ = false;
    barDrag_ = false;
    middle_ = false;
    activeId_ = -1;
}

void ViewportInput::releaseTracking() {
    tracking_ = false;
    dragged_ = false;
    activeId_ = -1;
    sampleVel_ = false;
}

void ViewportInput::stop() {
    dropPointer();
    pinch_ = false;
    stopCoast();
    velX_ = 0;
    velY_ = 0;
    sampleTime_ = 0;
    sampleVel_ = false;
    wheeling_ = false;
    wheelResidualX_ = 0;
    wheelResidualY_ = 0;
    zoomExact_ = 0;
}

bool ViewportInput::hitBar(const render::PageView& view, double x, double y, bool& vertical, bool& thumb, double& grab) const {
    render::Scrollbar horizontal;
    render::Scrollbar upright;
    view.scrollbars(horizontal, upright);
    if (contains(upright, x, y)) {
        vertical = true;
        thumb = onThumb(upright, x, y);
        grab = y - upright.thumbY;
        return true;
    }
    if (contains(horizontal, x, y)) {
        vertical = false;
        thumb = onThumb(horizontal, x, y);
        grab = x - horizontal.thumbX;
        return true;
    }
    return false;
}

bool ViewportInput::dragBar(render::PageView& view, double x, double y) {
    render::Scrollbar horizontal;
    render::Scrollbar upright;
    view.scrollbars(horizontal, upright);
    const render::Scrollbar& bar = barVertical_ ? upright : horizontal;
    if (!bar.visible) return false;
    const int span = barVertical_ ? bar.h : bar.w;
    const int thumb = barVertical_ ? bar.thumbH : bar.thumbW;
    const int travel = std::max(1, span - thumb);
    const int origin = barVertical_ ? bar.y : bar.x;
    const double along = (barVertical_ ? y : x) - origin - barGrab_;
    const render::ViewportState now = view.state();
    const int maxScroll = barVertical_ ? now.maxScrollY : now.maxScrollX;
    const int next = roundToInt(along / static_cast<double>(travel) * static_cast<double>(std::max(0, maxScroll)));
    if (barVertical_) view.setScroll(now.scrollX, next);
    else view.setScroll(next, now.scrollY);
    return true;
}

Camera ViewportInput::pointer(render::PageView& view, const PointerEvent& event) {
    const render::ViewportState before = view.state();
    const bool touchOrPen = (event.device == Device::Touch || event.device == Device::Pen);
    if (event.phase == Phase::Down) {
        stopCoast();
        if (event.button == 1 && !touchOrPen) {
            middle_ = true;
            barDrag_ = false;
            tracking_ = false;
            dragged_ = false;
            activeId_ = event.id;
            lastX_ = event.x;
            lastY_ = event.y;
            return snapshot(view, false, false, true);
        }
        bool vertical = false;
        bool thumb = false;
        double grab = 0;
        if (hitBar(view, event.x, event.y, vertical, thumb, grab)) {
            render::Scrollbar horizontal;
            render::Scrollbar upright;
            view.scrollbars(horizontal, upright);
            barDrag_ = true;
            barVertical_ = vertical;
            const int thumbSize = vertical ? upright.thumbH : horizontal.thumbW;
            barGrab_ = thumb ? grab : thumbSize / 2.0;
            activeId_ = event.id;
            tracking_ = false;
            dragged_ = false;
            if (!thumb) dragBar(view, event.x, event.y);
            const render::ViewportState after = view.state();
            const bool moved = after.scrollX != before.scrollX || after.scrollY != before.scrollY;
            return snapshot(view, moved, false, true);
        }
        barDrag_ = false;
        // 마우스로 눈금자를 누르면 그 아래 문서에 닿지 않는다.
        if (!touchOrPen && view.inRuler(event.x, event.y)) {
            dropPointer();
            return snapshot(view, false, false, true);
        }
        if (touchOrPen) {
            tracking_ = true;
            dragged_ = false;
            activeId_ = event.id;
            downX_ = event.x;
            downY_ = event.y;
            lastX_ = event.x;
            lastY_ = event.y;
            velX_ = 0;
            velY_ = 0;
            sampleVel_ = false;
            sampleTime_ = event.time;
            return snapshot(view, false, false, true);
        }
        dropPointer();
        return snapshot(view, false, false, false);
    }

    if (event.id != activeId_) return snapshot(view, false, false, false);
    if (event.phase == Phase::Cancel || event.phase == Phase::Up) {
        const bool owned = barDrag_ || tracking_;
        const bool flick = event.phase == Phase::Up && tracking_ && dragged_ && !barDrag_ && sampleVel_;
        const double flickX = -velX_;
        const double flickY = -velY_;
        dropPointer();
        velX_ = 0;
        velY_ = 0;
        sampleVel_ = false;
        sampleTime_ = 0;
        if (flick) return fling(view, flickX, flickY);
        return snapshot(view, false, false, owned);
    }
    if (middle_) {
        const int dx = roundToInt(event.x - lastX_);
        const int dy = roundToInt(event.y - lastY_);
        lastX_ = event.x;
        lastY_ = event.y;
        if (dx != 0 || dy != 0) view.scrollBy(dx, dy);
        const render::ViewportState after = view.state();
        return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY, false, true);
    }
    if (barDrag_) {
        dragBar(view, event.x, event.y);
        const render::ViewportState after = view.state();
        return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY, false, true);
    }
    if (tracking_) {
        if (!dragged_) {
            if (std::hypot(event.x - downX_, event.y - downY_) <= kTapSlop) {
                return snapshot(view, false, false, true);
            }
            dragged_ = true;
        }
        noteVelocity(event.x, event.y, event.time);
        const int dx = roundToInt(event.x - lastX_);
        const int dy = roundToInt(event.y - lastY_);
        lastX_ = event.x;
        lastY_ = event.y;
        view.scrollBy(-dx, -dy);
        const render::ViewportState after = view.state();
        return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY, false, true);
    }
    return snapshot(view, false, false, false);
}

int takeWheel(double& residual, double delta) {
    residual += delta;
    const int whole = static_cast<int>(std::trunc(residual));
    residual -= whole;
    return whole;
}

Camera ViewportInput::wheel(render::PageView& view, const WheelEvent& event) {
    const render::ViewportState before = view.state();
    dropPointer();
    if (event.phase == WheelPhase::Ended) {
        // 휠이 끊기면 그 자리에서 멈춘다. 이산 휠 줌 보간 중이면 목표 배율까지 tick()이 마무리한다.
        wheeling_ = false;
        wheelResidualX_ = 0;
        wheelResidualY_ = 0;
        if (zoomAnim_) {
            return snapshot(view, false, false, true);
        }
        pinch_ = false;
        zoomExact_ = 0;
        view.clearZoomAnchor();
        stopCoast();
        momentum_ = false;
        return snapshot(view, false, false, true);
    }
    if (event.zoom) {
        const bool wasZoomAnim = zoomAnim_;
        const double prevZoomCurrent = zoomCurrent_;
        const int prevZoomTarget = zoomTarget_;
        int next = before.percent;
        if (event.discrete) {
            int target = wasZoomAnim ? prevZoomTarget : before.percent;
            if (event.dy > 0) {
                target = (target <= 25) ? 10 : std::clamp(((target - 1) / 25) * 25, 10, 800);
            } else if (event.dy < 0) {
                target = (target < 25) ? 25 : std::clamp(((target / 25) + 1) * 25, 10, 800);
            }
            if (!wasZoomAnim && target == before.percent) {
                return snapshot(view, false, false, false);
            }
            wheeling_ = false;
            pinch_ = true;
            stopCoast();
            zoomCurrent_ = wasZoomAnim ? prevZoomCurrent : static_cast<double>(before.percent);
            zoomTarget_ = target;
            zoomExact_ = static_cast<double>(zoomTarget_);
            zoomX_ = event.x;
            zoomY_ = event.y;
            if (zoomTarget_ != before.percent) {
                zoomAnim_ = true;
                coast_ = true;
                zoomCurrent_ += (static_cast<double>(zoomTarget_) - zoomCurrent_) * 0.35;
                next = roundToInt(zoomCurrent_);
                if (next == before.percent) {
                    next = zoomTarget_ > before.percent ? before.percent + 1 : before.percent - 1;
                }
            }
        } else if (event.dy != 0) {
            double exact = zoomExact_;
            if (!(exact > 0) || roundToInt(exact) != before.percent) {
                exact = static_cast<double>(before.percent);
            }
            exact = std::clamp(exact * std::exp(-event.dy / 300.0), 10.0, 800.0);
            next = roundToInt(exact);
            if (!wasZoomAnim && !pinch_ && next == before.percent &&
                ((before.percent <= 10 && event.dy > 0) || (before.percent >= 800 && event.dy < 0))) {
                return snapshot(view, false, false, false);
            }
            wheeling_ = false;
            pinch_ = true;
            stopCoast();
            zoomExact_ = exact;
        } else {
            return snapshot(view, false, false, false);
        }
        view.zoomAbout(next, event.x, event.y);
        const render::ViewportState after = view.state();
        return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY,
                        after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth, true);
    }
    if (pinch_) view.clearZoomAnchor();
    pinch_ = false;
    zoomAnim_ = false;
    wheeling_ = true;
    const int dx = takeWheel(wheelResidualX_, event.dx);
    const int dy = takeWheel(wheelResidualY_, event.dy);
    if (event.phase == WheelPhase::Momentum) {
        stopCoast();
        momentum_ = true;
        view.scrollBy(dx, dy);
    } else {
        momentum_ = false;
        coast_ = false;
        residualX_ = 0;
        residualY_ = 0;
        if (event.discrete) {
            coastX_ = 0;
            coastY_ = 0;
        } else {
            coastX_ = event.vx;
            coastY_ = event.vy;
        }
        view.scrollBy(dx, dy);
    }
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY, false, true);
}

Camera ViewportInput::gesture(render::PageView& view, const GestureEvent& event) {
    const render::ViewportState before = view.state();
    if (event.phase == Phase::Down) {
        dropPointer();
        stopCoast();
        view.clearZoomAnchor();
        pinch_ = event.kind == GestureKind::Pinch;
        pinchPercent_ = before.percent;
        if (event.kind == GestureKind::Swipe) {
            coastX_ = event.dx;
            coastY_ = event.dy;
            coast_ = std::abs(coastX_) + std::abs(coastY_) > 20;
        }
        return snapshot(view, false, false, true);
    }
    if (event.phase == Phase::Cancel) {
        pinch_ = false;
        view.clearZoomAnchor();
        stopCoast();
        return snapshot(view, false, false, true);
    }
    if (event.phase == Phase::Up) {
        pinch_ = false;
        view.clearZoomAnchor();
        if (event.kind == GestureKind::Swipe) {
            coastX_ = event.dx;
            coastY_ = event.dy;
            coast_ = std::abs(coastX_) + std::abs(coastY_) > 20;
        }
        return snapshot(view, false, false, true);
    }
    if (event.kind == GestureKind::Pinch && pinch_) {
        const double scale = event.scale > 0 ? event.scale : 1;
        view.zoomAbout(std::clamp(roundToInt(static_cast<double>(pinchPercent_) * scale), 10, 800), event.x, event.y);
    } else if (event.kind == GestureKind::Scroll) {
        view.scrollBy(roundToInt(event.dx), roundToInt(event.dy));
    } else if (event.kind == GestureKind::Rotate) {
        rotation_ += event.rotation;
    } else if (event.kind == GestureKind::Swipe) {
        coastX_ = event.dx;
        coastY_ = event.dy;
        coast_ = std::abs(coastX_) + std::abs(coastY_) > 20;
    }
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY,
                    after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth, true);
}

Camera ViewportInput::tick(render::PageView& view, double dtMs) {
    const render::ViewportState before = view.state();
    if (!coast_ || !(dtMs > 0)) return snapshot(view, false, false, false);
    if (zoomAnim_) {
        const double alpha = 1.0 - std::exp(-dtMs / 38.0);
        zoomCurrent_ += (static_cast<double>(zoomTarget_) - zoomCurrent_) * alpha;
        if (std::abs(static_cast<double>(zoomTarget_) - zoomCurrent_) < 0.45) {
            zoomCurrent_ = static_cast<double>(zoomTarget_);
            zoomAnim_ = false;
            pinch_ = false;
            coast_ = false;
            zoomExact_ = 0;
            view.zoomAbout(zoomTarget_, zoomX_, zoomY_);
            view.clearZoomAnchor();
        } else {
            int next = roundToInt(zoomCurrent_);
            if (next == before.percent && std::abs(zoomTarget_ - before.percent) >= 1) {
                const double dir = (zoomTarget_ > before.percent) ? 1.0 : -1.0;
                zoomCurrent_ = std::clamp(zoomCurrent_ + dir * 0.25 * (dtMs / 16.0),
                                          static_cast<double>(std::min(before.percent, zoomTarget_)),
                                          static_cast<double>(std::max(before.percent, zoomTarget_)));
                next = roundToInt(zoomCurrent_);
            }
            view.zoomAbout(next, zoomX_, zoomY_);
        }
        const render::ViewportState after = view.state();
        const bool scrollChanged = after.scrollX != before.scrollX || after.scrollY != before.scrollY;
        const bool zoomChanged = after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth;
        return snapshot(view, scrollChanged, zoomChanged, coast_ || scrollChanged || zoomChanged);
    }
    residualX_ += coastX_ * dtMs / 1000.0;
    residualY_ += coastY_ * dtMs / 1000.0;
    const int dx = static_cast<int>(std::trunc(residualX_));
    const int dy = static_cast<int>(std::trunc(residualY_));
    residualX_ -= dx;
    residualY_ -= dy;
    if (dx != 0 || dy != 0) view.scrollBy(dx, dy);
    const double decay = std::exp(-dtMs / 220.0);
    coastX_ *= decay;
    coastY_ *= decay;
    if (std::abs(coastX_) < 8 && std::abs(coastY_) < 8) stopCoast();
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY, false, coast_ || dx != 0 || dy != 0);
}

Camera ViewportInput::fling(render::PageView& view, double vx, double vy) {
    const double speed = std::hypot(vx, vy);
    if (!(speed >= 80)) {
        stopCoast();
        return snapshot(view, false, false, true);
    }
    coastX_ = std::clamp(vx, -8000.0, 8000.0);
    coastY_ = std::clamp(vy, -8000.0, 8000.0);
    residualX_ = 0;
    residualY_ = 0;
    coast_ = true;
    momentum_ = false;
    return snapshot(view, false, false, true);
}

Camera ViewportInput::setScroll(render::PageView& view, int x, int y) {
    stop();
    const render::ViewportState before = view.state();
    view.setScroll(x, y);
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY, false, true);
}

Camera ViewportInput::setZoom(render::PageView& view, int percent) {
    stop();
    const render::ViewportState before = view.state();
    view.setZoomPercent(percent);
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY,
                    after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth, true);
}

Camera ViewportInput::glideZoom(render::PageView& view, int percent) {
    stop();
    const render::ViewportState before = view.state();
    const int target = std::clamp(percent, 10, 800);
    if (target == before.percent || view.viewWidth() <= 1 || view.viewHeight() <= 1) {
        return setZoom(view, target);
    }
    // 휠 칸 배율과 같은 길로 다가간다. 첫 걸음은 tick 이 아니라 여기서 떼어 바로 보이게 한다.
    pinch_ = true;
    zoomAnim_ = true;
    coast_ = true;
    zoomCurrent_ = static_cast<double>(before.percent);
    zoomTarget_ = target;
    zoomExact_ = static_cast<double>(target);
    zoomX_ = static_cast<double>(view.viewWidth()) / 2.0;
    zoomY_ = static_cast<double>(view.viewHeight()) / 2.0;
    zoomCurrent_ += (static_cast<double>(zoomTarget_) - zoomCurrent_) * 0.35;
    int next = roundToInt(zoomCurrent_);
    if (next == before.percent) next = target > before.percent ? before.percent + 1 : before.percent - 1;
    view.zoomAbout(next, zoomX_, zoomY_);
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY,
                    after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth, true);
}

Camera ViewportInput::stepPage(render::PageView& view, int dir) {
    stop();
    const render::ViewportState before = view.state();
    view.stepPage(dir);
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY, false, true);
}

Camera ViewportInput::fitWidth(render::PageView& view) {
    stop();
    const render::ViewportState before = view.state();
    view.fitWidth();
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY,
                    after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth, true);
}

Camera ViewportInput::fitPage(render::PageView& view) {
    stop();
    const render::ViewportState before = view.state();
    view.fitPage();
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY,
                    after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth, true);
}

Camera ViewportInput::setGridMode(render::PageView& view, render::PageGridMode mode, int cols, int rows, bool fit) {
    stop();
    const render::ViewportState before = view.state();
    view.setGridMode(mode, cols, rows, fit);
    const render::ViewportState after = view.state();
    return snapshot(view, after.scrollX != before.scrollX || after.scrollY != before.scrollY,
                    after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth, true);
}

}  // namespace noting::input
