#include "chwp/input/hangul.hpp"

namespace chwp::input {

namespace {

// 첫소리 19개와 끝소리 27개(없음 빼고)를 호환 낱자모로 적은 차례. 음절 번호를 셀 때 쓴다.
constexpr char32_t kInitials[] = {0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
                                  0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E};
constexpr char32_t kFinals[] = {0,      0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136, 0x3137, 0x3139, 0x313A,
                                0x313B, 0x313C, 0x313D, 0x313E, 0x313F, 0x3140, 0x3141, 0x3142, 0x3144, 0x3145,
                                0x3146, 0x3147, 0x3148, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E};

struct Pair {
    char32_t a;
    char32_t b;
    char32_t joined;
};

// 겹홀소리: ㅘ ㅙ ㅚ ㅝ ㅞ ㅟ ㅢ
constexpr Pair kVowelPairs[] = {{0x3157, 0x314F, 0x3158}, {0x3157, 0x3150, 0x3159}, {0x3157, 0x3163, 0x315A},
                                {0x315C, 0x3153, 0x315D}, {0x315C, 0x3154, 0x315E}, {0x315C, 0x3163, 0x315F},
                                {0x3161, 0x3163, 0x3162}};
// 겹받침: ㄳ ㄵ ㄶ ㄺ ㄻ ㄼ ㄽ ㄾ ㄿ ㅀ ㅄ
constexpr Pair kFinalPairs[] = {{0x3131, 0x3145, 0x3133}, {0x3134, 0x3148, 0x3135}, {0x3134, 0x314E, 0x3136},
                                {0x3139, 0x3131, 0x313A}, {0x3139, 0x3141, 0x313B}, {0x3139, 0x3142, 0x313C},
                                {0x3139, 0x3145, 0x313D}, {0x3139, 0x314C, 0x313E}, {0x3139, 0x314D, 0x313F},
                                {0x3139, 0x314E, 0x3140}, {0x3142, 0x3145, 0x3144}};

bool isVowel(char32_t cp) { return cp >= 0x314F && cp <= 0x3163; }

int indexOf(const char32_t* table, int count, char32_t cp) {
    for (int i = 0; i < count; ++i) {
        if (table[i] == cp) return i;
    }
    return -1;
}

int initialIndex(char32_t cp) { return indexOf(kInitials, 19, cp); }
int finalIndex(char32_t cp) { return cp == 0 ? 0 : indexOf(kFinals, 28, cp); }

char32_t joined(const Pair* pairs, int count, char32_t a, char32_t b) {
    for (int i = 0; i < count; ++i) {
        if (pairs[i].a == a && pairs[i].b == b) return pairs[i].joined;
    }
    return 0;
}

// 겹받침을 앞뒤로 나눈다. 겹받침이 아니면 앞은 0, 뒤는 그 받침이다.
void splitFinal(char32_t jong, char32_t& keep, char32_t& moved) {
    for (const Pair& pair : kFinalPairs) {
        if (pair.joined == jong) {
            keep = pair.a;
            moved = pair.b;
            return;
        }
    }
    keep = 0;
    moved = jong;
}

std::u32string compose(char32_t cho, char32_t jung, char32_t jong) {
    if (cho != 0 && jung != 0) {
        const int l = initialIndex(cho);
        const int v = static_cast<int>(jung - 0x314F);
        const int t = finalIndex(jong);
        if (l >= 0 && t >= 0) return std::u32string(1, static_cast<char32_t>(0xAC00 + (l * 21 + v) * 28 + t));
    }
    std::u32string out;
    if (cho != 0) out += cho;
    if (jung != 0) out += jung;
    if (jong != 0) out += jong;
    return out;
}

}  // namespace

bool HangulComposer::isJamo(char32_t cp) { return cp >= 0x3131 && cp <= 0x3163; }

std::u32string HangulComposer::preedit() const { return compose(cho_, jung_, jong_); }

void HangulComposer::reset() {
    keys_.clear();
    cho_ = 0;
    jung_ = 0;
    jong_ = 0;
}

void HangulComposer::start(char32_t key) {
    reset();
    keys_ = std::u32string(1, key);
    if (isVowel(key)) jung_ = key;
    else cho_ = key;
}

HangulComposer::Step HangulComposer::feed(char32_t key) {
    Step step;
    if (!isJamo(key)) return step;
    if (keys_.empty()) {
        start(key);
        step.preedit = preedit();
        return step;
    }
    if (!isVowel(key)) {
        if (cho_ != 0 && jung_ != 0 && jong_ == 0 && finalIndex(key) > 0) {
            // 받침이 될 수 없는 ㄸ ㅃ ㅉ 는 다음 음절 첫소리다.
            jong_ = key;
            keys_ += key;
        } else if (jong_ != 0 && joined(kFinalPairs, 11, jong_, key) != 0) {
            jong_ = joined(kFinalPairs, 11, jong_, key);
            keys_ += key;
        } else {
            step.done = preedit();
            start(key);
        }
        step.preedit = preedit();
        return step;
    }
    if (cho_ != 0 && jung_ == 0) {
        jung_ = key;
        keys_ += key;
    } else if (jung_ != 0 && jong_ == 0 && joined(kVowelPairs, 7, jung_, key) != 0) {
        jung_ = joined(kVowelPairs, 7, jung_, key);
        keys_ += key;
    } else if (jong_ != 0) {
        // 받침 뒤 홀소리: 받침(겹받침이면 뒤 낱자)이 다음 음절 첫소리로 넘어간다. 각+ㅏ → 가 + 가
        char32_t keep = 0;
        char32_t moved = 0;
        splitFinal(jong_, keep, moved);
        step.done = compose(cho_, jung_, keep);
        start(moved);
        jung_ = key;
        keys_ += key;
    } else {
        step.done = preedit();
        start(key);
    }
    step.preedit = preedit();
    return step;
}

std::u32string HangulComposer::backspace() {
    if (keys_.empty()) return {};
    std::u32string keys = keys_;
    keys.pop_back();
    reset();
    // 남은 낱자모를 다시 넣는다. 한 음절 안에서 쌓인 것이라 확정되는 글이 없다.
    for (const char32_t key : keys) feed(key);
    return preedit();
}

}  // namespace chwp::input
