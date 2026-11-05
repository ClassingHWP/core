#include "chwp/render/frame_phase.hpp"

namespace chwp::render {

const char* framePhaseName(FramePhase phase) {
    switch (phase) {
        case FramePhase::Drain: return "drain";
        case FramePhase::Crew: return "crew";
        case FramePhase::Plan: return "plan";
        case FramePhase::ViewBake: return "viewBake";
        case FramePhase::View: return "view";
        case FramePhase::Gather: return "gather";
        case FramePhase::Acquire: return "acquire";
        case FramePhase::Masks: return "masks";
        case FramePhase::Pictures: return "pictures";
        case FramePhase::Papers: return "papers";
        case FramePhase::PageBake: return "pageBake";
        case FramePhase::Sprites: return "sprites";
        case FramePhase::Glyphs: return "glyphs";
        case FramePhase::Overlay: return "overlay";
        case FramePhase::Submit: return "submit";
        case FramePhase::Count: break;
    }
    return "?";
}

FramePhases& framePhases() {
    thread_local FramePhases phases{};
    return phases;
}

void resetFramePhases() { framePhases().fill(0.0); }

void addFramePhase(FramePhase phase, double ms) {
    const int at = static_cast<int>(phase);
    if (at < 0 || at >= kFramePhaseCount) return;
    framePhases()[static_cast<std::size_t>(at)] += ms;
}

}  // namespace chwp::render
