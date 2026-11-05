#pragma once

#include "chwp/render/view.hpp"

namespace chwp::input {

// 셸은 포인터 id, 장치, 단계, 좌표를 그대로 넘긴다. 손가락을 핀치로 묶는 일은 PointerInput 이 한다.
// 코어는 창, 브라우저, 트랙패드 차이를 모른다.
enum class Device : int { Mouse = 0, Touch = 1, Pen = 2 };

// 포인터와 제스처가 같이 쓴다. Down 시작, Move 변화, Up 끝, Cancel 취소.
enum class Phase : int { Down = 1, Move = 2, Up = 3, Cancel = 4 };

// 휠은 포인터 단계와 다르다. 맥 관성 휠은 Momentum 이고, 델타는 더하기만 한다.
// Ended 인데 운영체제가 더 안 보내면 코어가 마지막 속도로 감속한다. 마우스 한 칸은 discrete 라 감속이 없다.
enum class WheelPhase : int { Changed = 0, Momentum = 1, Ended = 2 };

enum class GestureKind : int { Scroll = 0, Pinch = 1, Rotate = 2, Swipe = 3 };

// x, y 는 뷰포트 장치 픽셀이다. 문서 좌표도, CSS 픽셀도 아니다.
// time 은 셸의 단조 시계(밀리초)다. 0 이면 속도를 재지 않아 터치를 놓아도 관성이 없다.
struct PointerEvent {
    Device device = Device::Mouse;
    Phase phase = Phase::Down;
    int id = 0;
    int button = 0;  // 0 왼쪽, 1 휠 버튼, 2 오른쪽
    double x = 0;
    double y = 0;
    double time = 0;
};

// dy 가 양수면 아래로 스크롤한다. zoom 이면 셸이 핀치를 휠로 받은 것이다.
// 크롬·파이어폭스 트랙패드 핀치와, 각이 없는 윈도우 기본 창이 이쪽이다.
// 사파리 제스처는 GestureKind::Pinch 로 온다. 터치 두 손가락의 스크롤과 핀치는 PointerInput 이 가른다.
// vx, vy 는 초당 픽셀이다. 모르면 0 이고, 그러면 Ended 에서 감속을 시작하지 않는다.
struct WheelEvent {
    WheelPhase phase = WheelPhase::Changed;
    double dx = 0;
    double dy = 0;
    double x = 0;
    double y = 0;
    double vx = 0;
    double vy = 0;
    bool zoom = false;
    bool discrete = false;
};

// scale 은 제스처 시작을 1로 하는 누적 배율이다. 플랫폼이 증분을 주면 셸이 곱해서 넣는다.
// rotation 은 이번 이벤트의 각 변화(라디안)다. 플랫폼이 각을 안 주면 0 이다.
// Scroll 의 dx, dy 는 스크롤에 더할 양이다. 손가락 이동을 셸이 이 방향으로 바꾼다.
// Swipe 의 dx, dy 는 초당 픽셀이다.
struct GestureEvent {
    GestureKind kind = GestureKind::Scroll;
    Phase phase = Phase::Down;
    double x = 0;
    double y = 0;
    double dx = 0;
    double dy = 0;
    double scale = 1;
    double rotation = 0;
};

// 값이 바뀌면 이 구조로 셸에 바로 돌려준다. 셸이 배율·스크롤을 넣을 때도 같은 구조다.
// 스크롤 막대 사각형은 PageView 가 계산하고, 화면 캔버스에 코어가 그린다.
struct Camera {
    int scrollX = 0;
    int scrollY = 0;
    int maxScrollX = 0;
    int maxScrollY = 0;
    int percent = 100;
    bool fitPage = false;
    bool fitWidth = false;
    render::PageGridMode gridMode = render::PageGridMode::Auto;
    int gridCols = 1;
    int gridRows = 1;
    int effectiveCols = 1;
    int rowStepY = 0;
    int pageStepY = 0;
    bool scrollChanged = false;
    bool zoomChanged = false;
    bool coasting = false;
    bool handled = false;
    bool pinching = false;
    bool draggingBar = false;
    bool wheeling = false;
    bool panning = false;
    double rotation = 0;
};

// 확대·축소 한 단계. 25% 계단이고 25% 아래는 10% 하나다. 휠 끊어 돌리기와 메뉴의 확대·축소가 같이 쓴다.
int zoomLadderStep(int percent, int dir);

// 스크롤과 핀치 배율은 여기서 PageView 를 움직인다. 타이머는 없다. 셸이 tick 으로 시간을 넣는다.
// 마우스를 끌면 선택이라 스크롤하지 않는다. 터치와 펜은 끌면 스크롤하고 탭하면 커서를 놓는다.
class ViewportInput {
 public:
    static constexpr double kTapSlop = 8.0;
    // 손가락을 뗄 때 마지막 움직임이 이보다 오래되었으면(ms) 멈췄다가 뗀 것이라 관성을 주지 않는다.
    static constexpr double kFlingFreshMs = 80.0;

    Camera pointer(render::PageView& view, const PointerEvent& event);
    Camera wheel(render::PageView& view, const WheelEvent& event);
    Camera gesture(render::PageView& view, const GestureEvent& event);
    Camera tick(render::PageView& view, double dtMs);
    Camera fling(render::PageView& view, double vx, double vy);
    Camera setScroll(render::PageView& view, int x, int y);
    Camera setZoom(render::PageView& view, int percent);
    // 화면 가운데를 붙잡고 percent 까지 Ctrl+휠처럼 부드럽게 다가간다.
    Camera glideZoom(render::PageView& view, int percent);
    Camera stepPage(render::PageView& view, int dir);
    Camera fitWidth(render::PageView& view);
    Camera fitPage(render::PageView& view);
    Camera setGridMode(render::PageView& view, render::PageGridMode mode, int cols = 1, int rows = 1, bool fit = false);
    void stop();
    void releaseTracking();
    bool coasting() const { return coast_; }
    bool pinching() const { return pinch_; }
    bool draggingBar() const { return barDrag_; }
    bool wheeling() const { return wheeling_; }
    bool panning() const { return middle_; }
    bool draggingScroll() const { return tracking_ && dragged_; }
    // 다가가는 중인 목표 배율. 목표 없이 손을 따라가는 중이거나 멈췄으면 0.
    int zoomAim() const { return zoomAnim_ ? zoomTarget_ : 0; }

 private:
    Camera snapshot(const render::PageView& view, bool scrollChanged, bool zoomChanged, bool handled) const;
    void stopCoast();
    void dropPointer();
    void noteVelocity(double x, double y, double time);
    bool dragBar(render::PageView& view, double x, double y);
    bool hitBar(const render::PageView& view, double x, double y, bool& vertical, bool& thumb, double& grab) const;

    int activeId_ = -1;
    bool tracking_ = false;
    bool dragged_ = false;
    double downX_ = 0;
    double downY_ = 0;
    double lastX_ = 0;
    double lastY_ = 0;
    bool barDrag_ = false;
    bool barVertical_ = false;
    double barGrab_ = 0;
    bool pinch_ = false;
    int pinchPercent_ = 100;
    double coastX_ = 0;
    double coastY_ = 0;
    double residualX_ = 0;
    double residualY_ = 0;
    bool coast_ = false;
    double velX_ = 0;
    double velY_ = 0;
    double sampleTime_ = 0;
    bool sampleVel_ = false;
    bool momentum_ = false;
    bool wheeling_ = false;
    bool middle_ = false;
    double wheelResidualX_ = 0;
    double wheelResidualY_ = 0;
    double zoomExact_ = 0;
    bool zoomAnim_ = false;
    double zoomCurrent_ = 0;
    int zoomTarget_ = 100;
    double zoomX_ = 0;
    double zoomY_ = 0;
    double rotation_ = 0;
};

}  // namespace chwp::input
