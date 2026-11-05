#pragma once

// 화면 프레임 한 장을 단계로 나눠 각 단계에 쓴 시간을 잰다. 어느 단계가 프레임을 늦추는지 기기에서 보려는 것이다.
// 화면 스레드에서만 쓴다(스레드마다 따로 센다).

#include <array>
#include <chrono>

namespace chwp::render {

enum class FramePhase : int {
    Drain = 0,  // 일꾼이 끝낸 칸 받기
    Crew,       // 일꾼 스레드가 없을 때 화면 스레드가 밟는 일꾼 걸음
    Plan,       // 쪽 배치·칸 계획
    ViewBake,   // 스크롤 중 화면에 들어온 빈 칸 굽기
    View,       // 화면 그리기 전체(아래를 모두 포함한다)
    Gather,     // 화면에 올릴 쪽·칸 모으기
    Acquire,    // GPU 화면 표면 맞추기와 이번 프레임 텍스처 받기
    Masks,      // 글자 마스크 굽기
    Pictures,   // 그림을 GPU 에 올리기
    Papers,     // 쪽 그리기(쪽 텍스처 굽기를 포함한다)
    PageBake,   // 쪽 텍스처 굽기
    Sprites,    // 칸 그리기(글자 쿼드를 포함한다)
    Glyphs,     // 화면 글자 쿼드 만들기와 그리기
    Overlay,    // 블록 바탕, 캐럿, 눈금자, 스크롤바
    Submit,     // GPU 제출, 2D 그림 목록 넘기기
    Count,
};

inline constexpr int kFramePhaseCount = static_cast<int>(FramePhase::Count);
using FramePhases = std::array<double, kFramePhaseCount>;

const char* framePhaseName(FramePhase phase);
// 이번 프레임에 모은 단계 시간(ms). 프레임을 시작할 때 비운다.
FramePhases& framePhases();
void resetFramePhases();
void addFramePhase(FramePhase phase, double ms);

// 범위를 벗어날 때 그 단계에 걸린 시간을 더한다.
class FramePhaseTimer {
 public:
    explicit FramePhaseTimer(FramePhase phase) : phase_(phase), start_(std::chrono::steady_clock::now()) {}
    ~FramePhaseTimer() { stop(); }
    // 범위가 끝나기 전에 멈춘다. 한 번만 더한다.
    void stop() {
        if (stopped_) return;
        stopped_ = true;
        addFramePhase(phase_, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count());
    }
    FramePhaseTimer(const FramePhaseTimer&) = delete;
    FramePhaseTimer& operator=(const FramePhaseTimer&) = delete;

 private:
    FramePhase phase_;
    std::chrono::steady_clock::time_point start_;
    bool stopped_ = false;
};

}  // namespace chwp::render
