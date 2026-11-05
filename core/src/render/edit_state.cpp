#include "chwp/render/edit_state.hpp"

#include <atomic>

#include "render/paint_detail.hpp"
#include "render/undo_history.hpp"

namespace chwp::render {

struct EditState::Impl {
    paint_detail::PointerHold hold;
    paint_detail::UndoHistory history;
};

EditState::EditState() : impl_(std::make_unique<Impl>()) {}

EditState::~EditState() = default;

void EditState::clearCursor() { impl_->hold = paint_detail::PointerHold{}; }

namespace {

EditState& defaultState() {
    static EditState state;
    return state;
}

std::atomic<EditState*> gActive{nullptr};

EditState& activeState() {
    EditState* state = gActive.load(std::memory_order_acquire);
    return state ? *state : defaultState();
}

}  // namespace

EditScope::EditScope(EditState& state) : previous_(gActive.exchange(&state, std::memory_order_acq_rel)) {}

EditScope::~EditScope() { gActive.store(previous_, std::memory_order_release); }

namespace paint_detail {

PointerHold& pointerHold() { return activeState().impl().hold; }

UndoHistory& undoHistory() { return activeState().impl().history; }

}  // namespace paint_detail

}  // namespace chwp::render
