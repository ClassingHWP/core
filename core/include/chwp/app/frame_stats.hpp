#pragma once

// 화면 프레임 통계. 손이 움직이는 동안(끌기·관성·핀치) 프레임 간격과 한 프레임 처리 시간을 모은다.
// 셸은 꺼내서 보내기만 한다. 기기에서 부드러운지 재는 기준을 core 한 곳에 둔다.

#include <string>
#include <vector>

#include "chwp/render/frame_phase.hpp"

namespace chwp::app {

struct FrameSummary {
    int frames = 0;
    // 손이 움직이는 동안의 프레임 수.
    int moving = 0;
    // 움직이는 동안 한 프레임 처리 시간(ms).
    double costP50 = 0;
    double costP90 = 0;
    double costP99 = 0;
    double costMax = 0;
    // 움직이는 동안 이어진 두 프레임 사이 간격(ms).
    double gapP50 = 0;
    double gapP90 = 0;
    double gapP99 = 0;
    double gapMax = 0;
    // 보통 간격(p50)의 1.5배를 넘은 간격 수. 눈에 띄게 끊긴 프레임이다.
    int longGaps = 0;
    // 쉬는 동안 가장 긴 프레임(ms). 쉴 때 굽는 일이 다음 손놀림을 막는지 본다.
    double idleCostMax = 0;
    double seconds = 0;
    // 움직이는 동안 단계마다 가장 긴 시간과 평균(ms).
    render::FramePhases phaseMax{};
    render::FramePhases phaseAvg{};
    // 가장 느린 프레임이 어느 단계에 시간을 썼는지. 움직이는 동안과 쉬는 동안 따로.
    render::FramePhases worstMoving{};
    render::FramePhases worstIdle{};
};

class FrameStats {
 public:
    // time 은 프레임 시각(ms), costMs 는 그 프레임에 쓴 시간이다.
    void record(double time, double costMs, bool moving, const render::FramePhases& phases = {});
    // 모은 것을 요약해 돌려주고 비운다.
    FrameSummary take();

 private:
    static constexpr std::size_t kMaxSamples = 4096;
    std::vector<double> costs_;
    std::vector<double> gaps_;
    double firstTime_ = -1;
    double lastTime_ = -1;
    bool lastMoving_ = false;
    int frames_ = 0;
    int moving_ = 0;
    double idleCostMax_ = 0;
    double movingCostMax_ = 0;
    render::FramePhases phaseMax_{};
    render::FramePhases phaseSum_{};
    render::FramePhases worstMoving_{};
    render::FramePhases worstIdle_{};
};

std::string frameSummaryJson(const FrameSummary& summary);

}  // namespace chwp::app
