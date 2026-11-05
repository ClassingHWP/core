#include "chwp/input/pointer.hpp"

#include <algorithm>
#include <cmath>

namespace chwp::input {
namespace {

bool containsId(const std::vector<int>& ids, int id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

Camera PointerInput::finish(Camera camera) const {
    camera.pinching = pinchActive();
    return camera;
}

Camera PointerInput::quiet(const render::PageView& view, bool handled) const {
    const render::ViewportState now = view.state();
    Camera camera;
    camera.scrollX = now.scrollX;
    camera.scrollY = now.scrollY;
    camera.maxScrollX = now.maxScrollX;
    camera.maxScrollY = now.maxScrollY;
    camera.percent = now.percent;
    camera.fitPage = now.fitPage;
    camera.fitWidth = now.fitWidth;
    camera.gridMode = now.gridMode;
    camera.gridCols = now.gridCols;
    camera.gridRows = now.gridRows;
    camera.effectiveCols = now.effectiveCols;
    camera.rowStepY = now.rowStepY;
    camera.pageStepY = now.pageStepY;
    camera.handled = handled;
    camera.coasting = view_.coasting();
    return finish(camera);
}

int PointerInput::findTouch(int id) const {
    for (int i = 0; i < static_cast<int>(touches_.size()); ++i) {
        if (touches_[static_cast<std::size_t>(i)].first == id) return i;
    }
    return -1;
}

bool PointerInput::touchSpan(double& span, double& x, double& y) const {
    if (touches_.size() < 2) return false;
    const Contact& a = touches_[0].second;
    const Contact& b = touches_[1].second;
    span = std::hypot(a.x - b.x, a.y - b.y);
    x = (a.x + b.x) / 2.0;
    y = (a.y + b.y) / 2.0;
    return span > 0;
}

bool PointerInput::pinchActive() const { return pinching_ || platformPinch_ || view_.pinching(); }

bool PointerInput::touchLive() const { return pinchActive() || pair_ || !touches_.empty(); }

void PointerInput::holdSelection(int id) {
    view_.releaseTracking();
    tapId_ = -1;
    tapMoved_ = false;
    if (!containsId(selecting_, id)) selecting_.push_back(id);
}

void PointerInput::stop() {
    touches_.clear();
    selecting_.clear();
    tapId_ = -1;
    tapX_ = 0;
    tapY_ = 0;
    tapMoved_ = false;
    pair_ = false;
    pinching_ = false;
    platformPinch_ = false;
    pinchSpan_ = 0;
    midX_ = 0;
    midY_ = 0;
    midTime_ = 0;
    midVelX_ = 0;
    midVelY_ = 0;
    midVel_ = false;
    held_ = false;
    view_.stop();
}

void PointerInput::noteMid(double x, double y, double time) {
    if (midTime_ > 0 && time > midTime_) {
        const double dt = time - midTime_;
        if (dt >= 1 && dt < 120) {
            const double instantX = std::clamp((x - midX_) / dt * 1000.0, -8000.0, 8000.0);
            const double instantY = std::clamp((y - midY_) / dt * 1000.0, -8000.0, 8000.0);
            if (!midVel_) {
                midVelX_ = instantX;
                midVelY_ = instantY;
                midVel_ = true;
            } else {
                midVelX_ = midVelX_ * 0.35 + instantX * 0.65;
                midVelY_ = midVelY_ * 0.35 + instantY * 0.65;
            }
        } else {
            midVelX_ = 0;
            midVelY_ = 0;
            midVel_ = false;
        }
    }
    if (time > 0) midTime_ = time;
}

PointerOutcome PointerInput::beginPair(render::PageView& view, const PointerEvent& event) {
    GestureEvent arm;
    arm.kind = GestureKind::Scroll;
    arm.phase = Phase::Down;
    view_.gesture(view, arm);
    double span = 0;
    touchSpan(span, midX_, midY_);
    pinchSpan_ = span;
    pair_ = span > 0;
    pinching_ = false;
    midVelX_ = 0;
    midVelY_ = 0;
    midVel_ = false;
    midTime_ = event.time;
    PointerOutcome out;
    out.camera = quiet(view, true);
    out.phase = Phase::Down;
    return out;
}

PointerOutcome PointerInput::movePair(render::PageView& view, const PointerEvent& event) {
    const render::ViewportState before = view.state();
    double span = 0;
    double x = 0;
    double y = 0;
    PointerOutcome out;
    out.phase = Phase::Move;
    out.x = event.x;
    out.y = event.y;
    if (!touchSpan(span, x, y) || pinchSpan_ < 1) {
        out.camera = quiet(view, true);
        return out;
    }
    noteMid(x, y, event.time);
    const double panX = x - midX_;
    const double panY = y - midY_;
    const double pan = std::hypot(panX, panY);
    const double spanDelta = std::abs(span - pinchSpan_);
    midX_ = x;
    midY_ = y;
    GestureEvent scroll;
    scroll.kind = GestureKind::Scroll;
    scroll.phase = Phase::Move;
    scroll.dx = -panX;
    scroll.dy = -panY;
    scroll.x = x;
    scroll.y = y;
    Camera camera = view_.gesture(view, scroll);
    const double ratio = span / pinchSpan_;
    // 가운데가 움직이는 쪽이면 스크롤이다. 간격 변화가 그보다 클 때만 핀치로 잠근다.
    if (!pinching_ && spanDelta > 24.0 && spanDelta > pan * 1.5 && (ratio >= 1.08 || ratio <= 1.0 / 1.08)) {
        pinching_ = true;
        GestureEvent began;
        began.kind = GestureKind::Pinch;
        began.phase = Phase::Down;
        began.scale = 1;
        began.x = x;
        began.y = y;
        view_.gesture(view, began);
    }
    if (pinching_) {
        GestureEvent pinch;
        pinch.kind = GestureKind::Pinch;
        pinch.phase = Phase::Move;
        pinch.scale = ratio;
        pinch.x = x;
        pinch.y = y;
        camera = view_.gesture(view, pinch);
    }
    const render::ViewportState after = view.state();
    camera.scrollChanged = after.scrollX != before.scrollX || after.scrollY != before.scrollY;
    camera.zoomChanged =
        after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth;
    camera.handled = true;
    out.camera = finish(camera);
    return out;
}

PointerOutcome PointerInput::endPinch(render::PageView& view, Phase phase) {
    double span = 0;
    double x = 0;
    double y = 0;
    touchSpan(span, x, y);
    if (touches_.size() == 1) {
        x = touches_.front().second.x;
        y = touches_.front().second.y;
    }
    GestureEvent gesture;
    gesture.kind = GestureKind::Pinch;
    gesture.phase = phase;
    gesture.scale = 1;
    gesture.x = x;
    gesture.y = y;
    pair_ = false;
    pinching_ = false;
    pinchSpan_ = 0;
    midVel_ = false;
    held_ = !touches_.empty();
    PointerOutcome out;
    out.camera = finish(view_.gesture(view, gesture));
    out.phase = phase;
    return out;
}

PointerOutcome PointerInput::endPair(render::PageView& view, Phase phase, double time) {
    if (pinching_) return endPinch(view, phase);
    pair_ = false;
    held_ = !touches_.empty();
    PointerOutcome out;
    out.phase = phase;
    // 두 손가락을 멈춘 채 있다가 떼면 남은 속도는 지난 것이다. 한 손가락과 같은 기준으로 관성을 주지 않는다.
    const bool fresh = time <= 0 || midTime_ <= 0 || time - midTime_ <= ViewportInput::kFlingFreshMs;
    if (phase == Phase::Up && midVel_ && fresh) out.camera = finish(view_.fling(view, -midVelX_, -midVelY_));
    else out.camera = quiet(view, true);
    midVelX_ = 0;
    midVelY_ = 0;
    midVel_ = false;
    return out;
}

PointerOutcome PointerInput::pointer(render::PageView& view, const PointerEvent& event) {
    PointerOutcome out;
    out.phase = event.phase;
    out.x = event.x;
    out.y = event.y;
    if (event.device == Device::Mouse) {
        const Camera camera = view_.pointer(view, event);
        const bool owned = containsId(selecting_, event.id);
        if (event.phase == Phase::Down) {
            if (!camera.handled) selecting_.push_back(event.id);
        } else if (event.phase == Phase::Up || event.phase == Phase::Cancel) {
            selecting_.erase(std::remove(selecting_.begin(), selecting_.end(), event.id), selecting_.end());
        }
        out.camera = finish(camera);
        out.select = event.phase == Phase::Down ? !camera.handled : owned && !camera.handled;
        if (event.phase == Phase::Up || event.phase == Phase::Cancel) out.select = owned;
        return out;
    }

    if (event.phase == Phase::Down) {
        if (findTouch(event.id) < 0) touches_.push_back({event.id, Contact{event.x, event.y}});
        if (held_ || platformPinch_ || touches_.size() > 2) {
            tapId_ = -1;
            tapMoved_ = true;
            out.camera = quiet(view, true);
            return out;
        }
        if (touches_.size() == 2) {
            tapId_ = -1;
            tapMoved_ = true;
            return beginPair(view, event);
        }
        out.camera = finish(view_.pointer(view, event));
        if (!view_.draggingBar()) {
            tapId_ = event.id;
            tapX_ = event.x;
            tapY_ = event.y;
            tapMoved_ = false;
        } else {
            tapId_ = -1;
            tapMoved_ = true;
        }
        return out;
    }

    const int index = findTouch(event.id);
    if (event.phase == Phase::Move) {
        if (index >= 0) {
            touches_[static_cast<std::size_t>(index)].second.x = event.x;
            touches_[static_cast<std::size_t>(index)].second.y = event.y;
        }
        if (containsId(selecting_, event.id)) {
            out.select = true;
            out.phase = Phase::Move;
            out.x = event.x;
            out.y = event.y;
            out.camera = quiet(view, true);
            return out;
        }
        if (event.id == tapId_ && std::hypot(event.x - tapX_, event.y - tapY_) > ViewportInput::kTapSlop) {
            tapMoved_ = true;
        }
        if (held_ || platformPinch_) {
            tapId_ = -1;
            tapMoved_ = true;
            out.camera = quiet(view, true);
            return out;
        }
        if (pair_) {
            tapId_ = -1;
            tapMoved_ = true;
            return movePair(view, event);
        }
        out.camera = finish(view_.pointer(view, event));
        if (view_.draggingScroll() || view_.draggingBar()) {
            tapMoved_ = true;
        }
        return out;
    }

    if (index >= 0) touches_.erase(touches_.begin() + index);
    if (touches_.empty()) platformPinch_ = false;
    if (containsId(selecting_, event.id)) {
        selecting_.erase(std::remove(selecting_.begin(), selecting_.end(), event.id), selecting_.end());
        out.select = true;
        out.phase = event.phase;
        out.x = event.x;
        out.y = event.y;
        out.camera = quiet(view, true);
        return out;
    }
    if (pair_ && touches_.size() < 2) {
        tapId_ = -1;
        tapMoved_ = true;
        return endPair(view, event.phase, event.time);
    }
    if (held_) {
        tapId_ = -1;
        tapMoved_ = true;
        if (touches_.empty()) held_ = false;
        out.camera = quiet(view, true);
        return out;
    }
    const bool isTap = (event.phase == Phase::Up && event.id == tapId_ && !tapMoved_ && !pair_ && !pinching_ &&
                        !platformPinch_ && touches_.empty() && !view_.draggingBar() && !view_.draggingScroll());
    const double tapX = tapX_;
    const double tapY = tapY_;
    tapId_ = -1;
    tapMoved_ = false;
    out.camera = finish(view_.pointer(view, event));
    if (isTap) {
        out.select = true;
        out.tap = true;
        out.x = tapX;
        out.y = tapY;
    }
    return out;
}

PointerOutcome PointerInput::pointers(render::PageView& view, const PointerEvent* events, std::size_t count) {
    if (!events || count == 0) {
        PointerOutcome out;
        out.camera = quiet(view, false);
        return out;
    }
    if (count == 1) return pointer(view, events[0]);
    const render::ViewportState before = view.state();
    PointerOutcome out;
    bool anyHandled = false;
    for (std::size_t i = 0; i < count; ++i) {
        const PointerEvent& ev = events[i];
        const bool isLast = (i + 1 == count);
        if (!isLast && ev.device != Device::Mouse && ev.phase == Phase::Move && pair_ && !held_ && !platformPinch_) {
            const int idx = findTouch(ev.id);
            if (idx >= 0) {
                touches_[static_cast<std::size_t>(idx)].second.x = ev.x;
                touches_[static_cast<std::size_t>(idx)].second.y = ev.y;
            }
            tapId_ = -1;
            tapMoved_ = true;
            anyHandled = true;
            continue;
        }
        out = pointer(view, ev);
        anyHandled = anyHandled || out.camera.handled;
    }
    const render::ViewportState after = view.state();
    out.camera.scrollChanged =
        out.camera.scrollChanged || after.scrollX != before.scrollX || after.scrollY != before.scrollY;
    out.camera.zoomChanged = out.camera.zoomChanged || after.percent != before.percent ||
                             after.fitPage != before.fitPage || after.fitWidth != before.fitWidth;
    out.camera.handled = anyHandled;
    out.camera = finish(out.camera);
    return out;
}

Camera PointerInput::wheel(render::PageView& view, const WheelEvent& event) {
    return finish(view_.wheel(view, event));
}

Camera PointerInput::gesture(render::PageView& view, const GestureEvent& event) {
    if (event.kind == GestureKind::Pinch && pinching_) return quiet(view, true);
    if (event.kind == GestureKind::Pinch) {
        if (event.phase == Phase::Down) platformPinch_ = true;
        if (event.phase == Phase::Up || event.phase == Phase::Cancel) platformPinch_ = false;
    }
    return finish(view_.gesture(view, event));
}

Camera PointerInput::tick(render::PageView& view, double dtMs) { return finish(view_.tick(view, dtMs)); }

Camera PointerInput::setScroll(render::PageView& view, int x, int y) {
    stop();
    return view_.setScroll(view, x, y);
}

Camera PointerInput::setZoom(render::PageView& view, int percent) {
    stop();
    return view_.setZoom(view, percent);
}

Camera PointerInput::glideZoom(render::PageView& view, int percent) {
    stop();
    return view_.glideZoom(view, percent);
}

Camera PointerInput::stepPage(render::PageView& view, int dir) {
    stop();
    return view_.stepPage(view, dir);
}

Camera PointerInput::fitWidth(render::PageView& view) {
    stop();
    return view_.fitWidth(view);
}

Camera PointerInput::fitPage(render::PageView& view) {
    stop();
    return view_.fitPage(view);
}

Camera PointerInput::setGridMode(render::PageView& view, render::PageGridMode mode, int cols, int rows, bool fit) {
    stop();
    return view_.setGridMode(view, mode, cols, rows, fit);
}

}  // namespace chwp::input
