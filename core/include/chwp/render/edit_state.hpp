#pragma once

#include <memory>

namespace chwp::render {

// 문서 하나의 편집 상태: 커서·블록·IME 조합(PointerHold)과 되돌리기 기록.
// 편집 명령(editInsertText 등)은 지금 잡힌 편집 상태에 대고 동작한다. 아무것도 잡지 않으면 프로세스 기본 상태다.
// 편집기 세션과 agent 문서가 각자 하나씩 가지면, 같은 명령·같은 되돌리기를 타면서 서로의 커서와 기록을 건드리지 않는다.
class EditState {
public:
    EditState();
    ~EditState();
    EditState(const EditState&) = delete;
    EditState& operator=(const EditState&) = delete;

    // 커서·블록·조합을 비운다. 되돌리기 기록은 둔다.
    void clearCursor();

    struct Impl;
    Impl& impl() { return *impl_; }

private:
    std::unique_ptr<Impl> impl_;
};

// 살아 있는 동안 state 를 지금 편집 상태로 잡는다. 끝나면 앞 상태로 돌아간다.
class EditScope {
public:
    explicit EditScope(EditState& state);
    ~EditScope();
    EditScope(const EditScope&) = delete;
    EditScope& operator=(const EditScope&) = delete;

private:
    EditState* previous_ = nullptr;
};

}  // namespace chwp::render
