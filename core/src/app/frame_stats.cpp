#include "chwp/app/frame_stats.hpp"

#include <algorithm>
#include <cstdio>

namespace chwp::app {
namespace {

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0;
    const std::size_t at = std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())));
    return sorted[at];
}

}  // namespace

void FrameStats::record(double time, double costMs, bool moving, const render::FramePhases& phases) {
    ++frames_;
    if (firstTime_ < 0) firstTime_ = time;
    if (moving) {
        ++moving_;
        for (std::size_t i = 0; i < phases.size(); ++i) {
            phaseSum_[i] += phases[i];
            phaseMax_[i] = std::max(phaseMax_[i], phases[i]);
        }
        if (costMs >= movingCostMax_) {
            movingCostMax_ = costMs;
            worstMoving_ = phases;
        }
        if (costs_.size() < kMaxSamples) costs_.push_back(costMs);
        // 간격은 움직이는 프레임이 이어질 때만 센다. 쉬다 움직이기 시작한 첫 간격은 끊김이 아니다.
        if (lastMoving_ && lastTime_ >= 0 && time > lastTime_ && gaps_.size() < kMaxSamples) {
            gaps_.push_back(time - lastTime_);
        }
    } else if (costMs >= idleCostMax_) {
        idleCostMax_ = costMs;
        worstIdle_ = phases;
    }
    lastTime_ = time;
    lastMoving_ = moving;
}

FrameSummary FrameStats::take() {
    FrameSummary out;
    out.frames = frames_;
    out.moving = moving_;
    out.idleCostMax = idleCostMax_;
    out.seconds = firstTime_ >= 0 && lastTime_ > firstTime_ ? (lastTime_ - firstTime_) / 1000.0 : 0;
    std::sort(costs_.begin(), costs_.end());
    std::sort(gaps_.begin(), gaps_.end());
    out.costP50 = percentile(costs_, 0.5);
    out.costP90 = percentile(costs_, 0.9);
    out.costP99 = percentile(costs_, 0.99);
    out.costMax = costs_.empty() ? 0 : costs_.back();
    out.gapP50 = percentile(gaps_, 0.5);
    out.gapP90 = percentile(gaps_, 0.9);
    out.gapP99 = percentile(gaps_, 0.99);
    out.gapMax = gaps_.empty() ? 0 : gaps_.back();
    for (const double gap : gaps_) {
        if (gap > out.gapP50 * 1.5) ++out.longGaps;
    }
    out.phaseMax = phaseMax_;
    for (std::size_t i = 0; i < phaseSum_.size(); ++i) out.phaseAvg[i] = moving_ > 0 ? phaseSum_[i] / moving_ : 0;
    out.worstMoving = worstMoving_;
    out.worstIdle = worstIdle_;
    phaseMax_.fill(0);
    phaseSum_.fill(0);
    worstMoving_.fill(0);
    worstIdle_.fill(0);
    movingCostMax_ = 0;
    costs_.clear();
    gaps_.clear();
    frames_ = 0;
    moving_ = 0;
    idleCostMax_ = 0;
    firstTime_ = lastTime_;
    return out;
}

namespace {

std::string phasesJson(const render::FramePhases& phases) {
    std::string out = "{";
    for (int i = 0; i < render::kFramePhaseCount; ++i) {
        char item[64];
        std::snprintf(item, sizeof(item), "%s\"%s\":%.2f", i ? "," : "",
                      render::framePhaseName(static_cast<render::FramePhase>(i)), phases[static_cast<std::size_t>(i)]);
        out += item;
    }
    return out + "}";
}

}  // namespace

std::string frameSummaryJson(const FrameSummary& s) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"frames\":%d,\"moving\":%d,\"seconds\":%.2f,\"costP50\":%.2f,\"costP90\":%.2f,\"costP99\":%.2f,"
                  "\"costMax\":%.2f,\"gapP50\":%.2f,\"gapP90\":%.2f,\"gapP99\":%.2f,\"gapMax\":%.2f,\"longGaps\":%d,"
                  "\"idleCostMax\":%.2f}",
                  s.frames, s.moving, s.seconds, s.costP50, s.costP90, s.costP99, s.costMax, s.gapP50, s.gapP90,
                  s.gapP99, s.gapMax, s.longGaps, s.idleCostMax);
    std::string out = buf;
    out.pop_back();
    out += ",\"phaseMax\":" + phasesJson(s.phaseMax) + ",\"phaseAvg\":" + phasesJson(s.phaseAvg) +
           ",\"worstMoving\":" + phasesJson(s.worstMoving) + ",\"worstIdle\":" + phasesJson(s.worstIdle) + "}";
    return out;
}

}  // namespace chwp::app
