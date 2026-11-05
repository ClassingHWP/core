#pragma once

#include "chwp/input/viewport.hpp"

#include <utility>
#include <vector>

namespace chwp::input {

// 셸이 올린 포인터 하나. select 면 마우스 선택이거나 터치·펜 탭이라서 글자 선택/커서로 넘긴다.
struct PointerOutcome {
    Camera camera;
    bool select = false;
    bool tap = false;
    Phase phase = Phase::Down;
    double x = 0;
    double y = 0;
    int id = 0;
    int device = 0;
    double time = 0;
};

// 포인터 id 와 마우스·터치·펜, 그리고 플랫폼 제스처를 여기서 가른다.
// 터치와 펜은 끌면 스크롤이고 탭하면 커서를 놓는다. 둘이면 가운데가 움직이면 스크롤이고, 간격이 벌어지면 핀치다.
// 스크롤하다 손을 떼면 마지막 속도로 감속한다. 마우스를 끌면 선택이다.
class PointerInput {
 public:
    PointerOutcome pointer(render::PageView& view, const PointerEvent& event);
    PointerOutcome pointers(render::PageView& view, const PointerEvent* events, std::size_t count);
    Camera wheel(render::PageView& view, const WheelEvent& event);
    Camera gesture(render::PageView& view, const GestureEvent& event);
    Camera tick(render::PageView& view, double dtMs);
    Camera setScroll(render::PageView& view, int x, int y);
    Camera setZoom(render::PageView& view, int percent);
    Camera glideZoom(render::PageView& view, int percent);
    Camera stepPage(render::PageView& view, int dir);
    Camera fitWidth(render::PageView& view);
    Camera fitPage(render::PageView& view);
    Camera setGridMode(render::PageView& view, render::PageGridMode mode, int cols = 1, int rows = 1, bool fit = false);
    void stop();
    void holdSelection(int id);
    int touchCount() const { return static_cast<int>(touches_.size()); }
    bool pinchActive() const;
    bool touchLive() const;
    bool coasting() const { return view_.coasting(); }
    bool draggingBar() const { return view_.draggingBar(); }
    bool wheeling() const { return view_.wheeling(); }
    bool panning() const { return view_.panning(); }
    int zoomAim() const { return view_.zoomAim(); }
    bool selecting() const { return !selecting_.empty(); }

 private:
    struct Contact {
        double x = 0;
        double y = 0;
    };

    Camera finish(Camera camera) const;
    Camera quiet(const render::PageView& view, bool handled) const;
    int findTouch(int id) const;
    bool touchSpan(double& span, double& x, double& y) const;
    void noteMid(double x, double y, double time);
    PointerOutcome beginPair(render::PageView& view, const PointerEvent& event);
    PointerOutcome movePair(render::PageView& view, const PointerEvent& event);
    PointerOutcome endPair(render::PageView& view, Phase phase, double time);
    PointerOutcome endPinch(render::PageView& view, Phase phase);

    ViewportInput view_;
    std::vector<std::pair<int, Contact>> touches_;
    std::vector<int> selecting_;
    int tapId_ = -1;
    double tapX_ = 0;
    double tapY_ = 0;
    bool tapMoved_ = false;
    bool pair_ = false;
    bool pinching_ = false;
    bool platformPinch_ = false;
    double pinchSpan_ = 0;
    double midX_ = 0;
    double midY_ = 0;
    double midTime_ = 0;
    double midVelX_ = 0;
    double midVelY_ = 0;
    bool midVel_ = false;
    bool held_ = false;
};

}  // namespace chwp::input
