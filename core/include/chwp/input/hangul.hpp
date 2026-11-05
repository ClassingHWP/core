#pragma once

// 낱자모 한글 조합기(두벌식). 입력기가 조합하지 않고 낱자모(ㄱ, ㅏ …)를 하나씩 넣는 플랫폼이 있다.
// 아이패드 외장 키보드(WebKit)는 조합 사건 없이 'ㅎ', 'ㅏ', 'ㄴ' 을 따로 넣는다. 코어가 이것을 음절로 묶는다.
// 조합기는 글자만 다룬다. 문서에 조합 중 글로 넣고 확정하는 일은 세션이 한다.

#include <string>

namespace chwp::input {

class HangulComposer {
public:
    // 한글 호환 낱자모(ㄱ U+3131 ~ ㅣ U+3163)인지.
    static bool isJamo(char32_t cp);

    struct Step {
        std::u32string done;     // 이번에 확정된 글
        std::u32string preedit;  // 아직 조합 중인 글
    };

    // 낱자모 하나를 넣는다. isJamo 가 참인 글자만 넣는다.
    Step feed(char32_t jamo);
    // 조합 중인 글에서 마지막 낱자모를 뺀다. 빼고 남은 조합 중 글을 돌려준다. 조합 중이 아니면 아무것도 안 한다.
    std::u32string backspace();
    // 조합 중인 글을 버린다(이미 문서에 넣은 글은 그대로 둔다).
    void reset();
    [[nodiscard]] bool active() const { return !keys_.empty(); }
    [[nodiscard]] std::u32string preedit() const;

private:
    void start(char32_t key);
    std::u32string keys_;  // 지금 음절에 넣은 낱자모 차례. 지울 때 다시 넣어 본다
    char32_t cho_ = 0;     // 첫소리(호환 낱자모). 없으면 0
    char32_t jung_ = 0;    // 가운뎃소리
    char32_t jong_ = 0;    // 끝소리
};

}  // namespace chwp::input
