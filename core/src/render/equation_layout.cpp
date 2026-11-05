#include "render/equation_layout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "export/font_metrics.hpp"

namespace chwp::render {
namespace {

// 1인치 = 7200 HWPUNIT. 화면 96dpi에서 1픽셀은 75 HWPUNIT이다.
// 글자 진행 폭은 픽셀 수를 이 값으로 곱해 HWPUNIT로 낸다.
constexpr int kDpi = 96;
constexpr int kHwpPerInch = 7200;
constexpr int kHwpPerPx = kHwpPerInch / kDpi;

// ~ 는 글자 크기의 절반이다. 10pt는 500, 9pt는 450, 11pt는 550이다.
// ` 는 그 값을 4로 나눈다. 10pt는 125, 9pt는 112, 11pt는 137이다.
std::int32_t equationBlank(std::int32_t em) {
    if (em <= 0) return 0;
    return em / 2;
}

std::int32_t mulDiv(std::int64_t a, std::int64_t b, std::int64_t c) {
    if (a <= 0 || b <= 0 || c <= 0) return 0;
    return static_cast<std::int32_t>((a * b + c / 2) / c);
}

// 글자 폭 글꼴의 높이. 글자 크기의 가장 가까운 픽셀이다. 9pt는 12, 8pt는 11, 10pt는 13이다.
std::int32_t equationFontHeightPx(std::int32_t em) {
    if (em <= 0) return 1;
    return std::max(1, static_cast<std::int32_t>(
        (static_cast<std::int64_t>(em) * kDpi + kHwpPerInch / 2) / kHwpPerInch));
}

// 글자 폭에 쓰는 lfWidth. 화면 크기의 절반을 가장 가까운 픽셀로 맞춘다.
// 딱 n.5이면 아래 픽셀이다. 9pt는 6, 10pt의 6.666은 7, 96pt는 64다. 9525는 63.5라 63이다.
std::int32_t equationFontWidthPx(std::int32_t em) {
    if (em <= 0) return 1;
    return std::max(1, static_cast<std::int32_t>(
        (static_cast<std::int64_t>(em) * (kDpi / 2) + kHwpPerInch / 2 - 1) / kHwpPerInch));
}

// 가로 배율과 세로 배율이 같을 때의 진행폭. 픽셀 격자에 맞춘다.
// 노션 폭에 lfWidth를 16.16으로 곱하고 OS/2 xAvgCharWidth로 나눈 뒤 26.6으로 바꾸고,
// (26.6 + 0x20) & ~0x3F 로 픽셀에 올린다. 9pt O(725, lfWidth 6, xAvg 512)는 9다.
std::int32_t equationSideBearingPx(std::int32_t design, std::int32_t lfWidth, std::int32_t xAvg) {
    if (design <= 0 || lfWidth <= 0 || xAvg <= 0) return 0;
    const std::int64_t scaled = static_cast<std::int64_t>(design) * (static_cast<std::int64_t>(lfWidth) << 16);
    const std::int64_t quot = (scaled + xAvg / 2) / xAvg;
    const std::int64_t f26 = (quot + 0x200) >> 10;
    const std::uint32_t snapped = (static_cast<std::uint32_t>(f26) + 0x20u) & ~0x3Fu;
    return static_cast<std::int32_t>(snapped >> 6);
}

// 가로 배율과 세로 배율이 다를 때의 진행폭. 격자 맞춤 없이 선형으로 곱한다.
// 배율(lfWidth/xAvg, 16.16)에 진행폭을 곱하고 0x8000을 더한 뒤 12비트 내리고, 픽셀 아래는 버린다.
std::int32_t equationFastAdvancePx(std::int32_t design, std::int32_t lfWidth, std::int32_t xAvg) {
    if (design <= 0 || lfWidth <= 0 || xAvg <= 0) return 0;
    const std::int64_t scale = (static_cast<std::int64_t>(lfWidth) << 16) / xAvg;
    std::int64_t rounded = scale * design + 0x8000;
    rounded >>= 12;
    rounded &= ~std::int64_t{0xF};
    return static_cast<std::int32_t>(rounded >> 4);
}

// 세로 장치 픽셀이 1이면 드라이버가 2로 올리고 가로는 같은 비로 늘린다.
// 글자 크기가 1픽셀보다 작으면 그 올림을 하지 않는다. 1pt 본문 100은 올리고, 첨자 68은 가로 1이다.
// 그 뒤 두 배율이 같으면 픽셀 격자, 다르면 선형 곱이다.
std::int32_t equationGlyphPx(std::int32_t design, std::int32_t em, std::int32_t xAvg, std::int32_t cell,
                             std::int32_t upem) {
    std::int32_t height = equationFontHeightPx(em);
    std::int32_t width = equationFontWidthPx(em);
    const std::int32_t denom = cell > 0 ? cell : upem;
    if (denom > 0 && upem > 0) {
        const std::int32_t yPx = static_cast<std::int32_t>(static_cast<std::int64_t>(height) * upem / denom);
        if (yPx == 1 && static_cast<std::int64_t>(em) * kDpi >= kHwpPerInch) {
            width *= 2;
            height *= 2;
        }
    }
    const std::int32_t avg = xAvg > 0 ? xAvg : (upem > 0 ? upem / 2 : 0);
    if (avg <= 0) return 0;
    const bool isotropic = denom > 0 && static_cast<std::int64_t>(height) * avg == static_cast<std::int64_t>(width) * denom;
    return isotropic ? equationSideBearingPx(design, width, avg) : equationFastAdvancePx(design, width, avg);
}

// 글자 높이의 픽셀. 확장 호출의 cy를 75로 나눈 값이다.
// 가장 가까운 픽셀이다. 10pt는 13(975), 8pt는 11(825), 6pt는 8(600).
// 1픽셀은 측정 칸이 2다. 256으로 나눈 나머지가 128이면 2픽셀을 더한다.
// GetOutlineTextMetricsW가 1픽셀은 tmHeight+tmInternalLeading 합 2를, 그 나머지 픽셀은 tmHeight를 1 키우고 내부 여백 1을 돌려준다. 한글은 그 합을 글자 높이로 쓴다.
// 96pt는 130(9750), 288pt는 386, 480pt는 642다.
std::int32_t equationHeightPx(std::int32_t em) {
    if (em <= 0) return 1;
    std::int32_t px = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(em) * kDpi + kHwpPerInch / 2) / kHwpPerInch);
    if (px < 1) px = 1;
    if (px == 1) return 2;
    if (px % 256 == 128) return px + 2;
    return px;
}

// rm 은 같은 수식 폰트의 ASCII, 그 외 문자는 PUA, 숫자는 rm 이어도 PUA다.
char32_t equationGlyph(char32_t cp, bool roman) {
    if (cp >= U'0' && cp <= U'9') return cp == U'0' ? 0xE03D : 0xE034 + (cp - U'1');
    if (cp >= U'a' && cp <= U'z') return roman ? cp : static_cast<char32_t>(0xE0E5 + (cp - U'a'));
    if (cp >= U'A' && cp <= U'Z') return roman ? cp : static_cast<char32_t>(0xE000 + (cp - U'A'));
    return 0;
}

// 수식 글꼴이 연산자를 넣는 코드. '=' 는 U+003D 가 아니라 U+E047 이다.
char32_t equationOperator(char32_t cp) {
    switch (cp) {
    case U'+': return 0xE048;
    case U'*': return 0xE043;
    case U'(': return 0xE044;
    case U')': return 0xE045;
    case U'=': return 0xE047;
    case U'[': return 0xE049;
    case U']': return 0xE04A;
    case U';': return 0xE04E;
    case U':': return 0xE04F;
    case U',': return 0xE052;
    case U'.': return 0xE053;
    case U'/': return 0xE054;
    case U'-': return 0xE046;
    case U'<': return 0xE055;
    case U'>': return 0xE056;
    case U'!': return 0xE03E;
    // 세로줄은 U+E04D 다. 11pt 는 3픽셀(225)이다.
    case U'|': return 0xE04D;
    default: return 0;
    }
}

// 두 글자 이상으로 묶인 연산자. 한글은 이것을 글리프 하나로 잰다.
char32_t equationOperatorToken(std::string_view text) {
    if (text == "!=") return 0x2260;
    if (text == "==") return 0x2261;
    if (text == "<=") return 0x2264;
    if (text == ">=") return 0x2265;
    if (text == "->") return 0x2192;
    if (text == "<-") return 0x2190;
    if (text == "<->") return 0x2194;
    if (text == "<<") return 0x226A;
    if (text == ">>") return 0x226B;
    if (text == "+-") return 0x00B1;
    return 0;
}

// 가로로 맞닿은 글자의 간격 표에 쓰는 종류. 0 은 간격을 더하지 않는다.
// 1 보통, 3 이항, 4 관계, 5 여는 울타리, 6 닫는 울타리, 7 곱·쉼표.
int equationAtomClass(char32_t cp) {
    if ((cp >= U'0' && cp <= U'9') || (cp >= U'A' && cp <= U'Z') || (cp >= U'a' && cp <= U'z')) return 1;
    switch (cp) {
    case U'+':
    case U'-': return 3;
    case U'=':
    case U'<':
    case U'>':
    case U':': return 4;
    case U'(':
    case U'[':
    case U'{': return 5;
    case U')':
    case U']':
    case U'}': return 6;
    case U'*':
    case U',': return 7;
    default: return 1;
    }
}

// 이름으로 부르는 기호의 간격 종류. 그리스 문자와 INF, prime 은 보통 글자다.
// LEQ 같은 관계는 4, CIRC 와 != 은 이항 3, 점(…)은 0이다.
int symbolAtomClass(char32_t cp) {
    switch (cp) {
    case 0x22EF:
    case 0x2026:
    case 0x22EE:
    case 0x22F1:
    case 0x22F0:
        return 0;
    case 0x2264:
    case 0x2265:
    case 0x226A:
    case 0x226B:
    case 0x22D8:
    case 0x22D9:
    case 0x2261:
    case 0x2262:
    case 0x223C:
    case 0x2243:
    case 0x2245:
    case 0x2248:
    case 0x224D:
    case 0x224E:
    case 0x2250:
    case 0x2252:
    case 0x2253:
    case 0x2282:
    case 0x2283:
    case 0x2284:
    case 0x2285:
    case 0x2286:
    case 0x2287:
    case 0x2288:
    case 0x2289:
    case 0x220A:
    case 0x220D:
    case 0x2209:
    case 0x220C:
    case 0x227A:
    case 0x227B:
    case 0x228F:
    case 0x2290:
    case 0x2291:
    case 0x2292:
    case 0x2234:
    case 0x2235:
    case 0x221D:
    case 0x2237:
    case 0x22A2:
    case 0x22A3:
    case 0x22A4:
    case 0x22A5:
    case 0x22A8:
        return 4;
    case 0x2218:
    case 0x2260:
    case 0x00B1:
    case 0x2213:
    case 0x00D7:
    case 0x00F7:
    case 0x2295:
    case 0x2296:
    case 0x2297:
    case 0x2298:
    case 0x2299:
    case 0x228E:
    case 0x2227:
    case 0x2228:
    case 0x2214:
    case 0x22C5:
    case 0x2219:
    case 0x2217:
    case 0x22C6:
    case 0x22C4:
    case 0x2293:
    case 0x2294:
    case 0x2229:
    case 0x222A:
    case 0x22BB:
        return 3;
    default:
        return 1;
    }
}

enum class Kind { End, Space, Word, Number, Symbol, Thin, Wide, LBrace, RBrace, Hash, Amp, Sup, Sub, Quote };

struct Token {
    Kind kind = Kind::End;
    std::string text;
};

bool eqFold(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<unsigned char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<unsigned char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::vector<Token> tokenize(std::string_view script) {
    std::vector<Token> tokens;
    const auto push = [&](Kind kind, std::string text) { tokens.push_back(Token{kind, std::move(text)}); };
    std::size_t i = 0;
    while (i < script.size()) {
        const unsigned char ch = static_cast<unsigned char>(script[i]);
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            if (tokens.empty() || tokens.back().kind != Kind::Space) push(Kind::Space, " ");
            ++i;
            continue;
        }
        if (ch == '"') {
            const std::size_t start = ++i;
            while (i < script.size() && script[i] != '"') ++i;
            push(Kind::Quote, std::string(script.substr(start, i - start)));
            if (i < script.size()) ++i;
            continue;
        }
        if (ch == '`') {
            push(Kind::Thin, "`");
            ++i;
            continue;
        }
        if (ch == '~') {
            push(Kind::Wide, "~");
            ++i;
            continue;
        }
        if (ch == '{') {
            push(Kind::LBrace, "{");
            ++i;
            continue;
        }
        if (ch == '}') {
            push(Kind::RBrace, "}");
            ++i;
            continue;
        }
        if (ch == '#') {
            push(Kind::Hash, "#");
            ++i;
            continue;
        }
        if (ch == '&') {
            push(Kind::Amp, "&");
            ++i;
            continue;
        }
        if (ch == '^') {
            push(Kind::Sup, "^");
            ++i;
            continue;
        }
        if (ch == '_') {
            push(Kind::Sub, "_");
            ++i;
            continue;
        }
        const std::string_view rest = script.substr(i);
        const char* ops[] = {"<->", "->", "<-", ">>>", "<<<", "<<", ">>", "!=", "==", "<=", ">=", "+-", "-+"};
        bool matched = false;
        for (const char* op : ops) {
            const std::size_t n = std::char_traits<char>::length(op);
            if (rest.size() >= n && rest.compare(0, n, op) == 0) {
                push(Kind::Symbol, op);
                i += n;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        if (ch >= '0' && ch <= '9') {
            const std::size_t start = i++;
            while (i < script.size()) {
                const unsigned char n = static_cast<unsigned char>(script[i]);
                if (n >= '0' && n <= '9') {
                    ++i;
                    continue;
                }
                if (n == '.' && i + 1 < script.size() && script[i + 1] >= '0' && script[i + 1] <= '9') {
                    i += 2;
                    while (i < script.size() && script[i] >= '0' && script[i] <= '9') ++i;
                    break;
                }
                break;
            }
            push(Kind::Number, std::string(script.substr(start, i - start)));
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) {
            const std::size_t start = i++;
            while (i < script.size()) {
                const unsigned char n = static_cast<unsigned char>(script[i]);
                if ((n >= 'A' && n <= 'Z') || (n >= 'a' && n <= 'z')) ++i;
                else break;
            }
            push(Kind::Word, std::string(script.substr(start, i - start)));
            continue;
        }
        if (ch < 0x80) {
            push(Kind::Symbol, std::string(1, static_cast<char>(ch)));
            ++i;
            continue;
        }
        std::size_t n = 1;
        if ((ch & 0xE0) == 0xC0) n = 2;
        else if ((ch & 0xF0) == 0xE0) n = 3;
        else if ((ch & 0xF8) == 0xF0) n = 4;
        if (i + n > script.size()) n = 1;
        push(Kind::Symbol, std::string(script.substr(i, n)));
        i += n;
    }
    return tokens;
}

bool nextCp(std::string_view text, std::size_t& i, char32_t& cp) {
    if (i >= text.size()) return false;
    const auto c0 = static_cast<unsigned char>(text[i]);
    if (c0 < 0x80) {
        cp = c0;
        ++i;
        return true;
    }
    const std::size_t need = c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : 4;
    if (i + need > text.size()) {
        cp = c0;
        ++i;
        return true;
    }
    char32_t value = c0 & (need == 2 ? 0x1F : need == 3 ? 0x0F : 0x07);
    for (std::size_t k = 1; k < need; ++k) {
        const auto cx = static_cast<unsigned char>(text[i + k]);
        if ((cx & 0xC0) != 0x80) {
            cp = c0;
            ++i;
            return true;
        }
        value = (value << 6) | (cx & 0x3F);
    }
    cp = value;
    i += need;
    return true;
}

struct Symbol {
    const char* name;
    char32_t cp;
};

const Symbol kSymbols[] = {
#include "equation_symbols.inc"
};

char32_t symbolCode(std::string_view word) {
    for (const Symbol& symbol : kSymbols) {
        if (word == symbol.name) return symbol.cp;
    }
    char32_t found = 0;
    bool any = false;
    bool clash = false;
    for (const Symbol& symbol : kSymbols) {
        if (!eqFold(word, symbol.name)) continue;
        if (any && symbol.cp != found) clash = true;
        found = symbol.cp;
        any = true;
    }
    return any && !clash ? found : 0;
}

// 단어 맨 앞에서만 함수로 본다. sin 은 곧게 선 ASCII 다. xsinx 의 sin 은 기울임으로 남는다.
// sinh 는 sin 과 h 다. 더 긴 이름이 이긴다.
int uprightPrefix(std::string_view word) {
    static const char* names[] = {"csc", "sec", "cot", "cos", "sin", "tan", "log", "exp", "det", "mod", "gcd", "ln"};
    int best = 0;
    for (const char* name : names) {
        const int n = static_cast<int>(std::char_traits<char>::length(name));
        if (n <= best || static_cast<int>(word.size()) < n) continue;
        if (word.compare(0, static_cast<std::size_t>(n), name) == 0) best = n;
    }
    return best;
}

struct Big {
    const char* name;
    char32_t cp;
    bool small;
};

char32_t fencePiece(std::string_view text) {
    if (text == "(") return 0xE07D;
    if (text == ")") return 0xE080;
    if (text == "[") return 0xE101;
    if (text == "]") return 0xE105;
    if (text == "{") return 0xE079;
    if (text == "}") return 0xE083;
    if (text == "|") return 0xE101;
    return 0;
}

char32_t intGlyph(std::string_view name) {
    if (eqFold(name, "int")) return 0xE05B;
    if (eqFold(name, "dint")) return 0xE073;
    if (eqFold(name, "tint")) return 0xE074;
    if (eqFold(name, "oint")) return 0xE075;
    if (eqFold(name, "odint")) return 0xE076;
    if (eqFold(name, "otint")) return 0xE077;
    return 0;
}

char32_t sumGlyph(std::string_view name) {
    if (eqFold(name, "sum")) return 0xE067;
    if (eqFold(name, "prod")) return 0xE068;
    if (eqFold(name, "coprod")) return 0xE069;
    if (eqFold(name, "inter")) return 0xE06A;
    if (eqFold(name, "union")) return 0xE06B;
    return 0;
}

const Big* bigOp(std::string_view word) {
    static const Big table[] = {
        {"smallsum", 0x2211, true},   {"smallprod", 0x220F, true},  {"smallcoprod", 0x2210, true},
        {"smallunion", 0x222A, true}, {"smallinter", 0x2229, true}, {"int", 0x222B, false},
        {"oint", 0x222E, false},      {"dint", 0x222C, false},      {"tint", 0x222D, false},
        {"odint", 0x222F, false},     {"otint", 0x2230, false},     {"sum", 0x2211, false},
        {"prod", 0x220F, false},      {"coprod", 0x2210, false},    {"union", 0x22C3, false},
        {"inter", 0x22C2, false},
    };
    for (const Big& op : table) {
        if (eqFold(word, op.name)) return &op;
    }
    return nullptr;
}

struct Accent {
    char32_t cp = 0;
    bool under = false;
    // bar vec dyad under 는 글자 너비와 기호 너비 중 큰 값에 기호 너비의 20%를 더한다.
    bool widen = false;
};

const Accent* accentOf(std::string_view word) {
    static const Accent hat{0xE063, false, false};
    static const Accent check{0xE064, false, false};
    static const Accent arch{0xE065, false, false};
    static const Accent tilde{0xE066, false, false};
    static const Accent acute{0xE05D, false, false};
    static const Accent grave{0xE05E, false, false};
    static const Accent dot{0xE05F, false, false};
    static const Accent ddot{0xE060, false, false};
    static const Accent bar{0xE06D, false, true};
    static const Accent under{0xE06D, true, true};
    if (eqFold(word, "hat")) return &hat;
    if (eqFold(word, "check")) return &check;
    if (eqFold(word, "arch")) return &arch;
    if (eqFold(word, "tilde")) return &tilde;
    if (eqFold(word, "acute")) return &acute;
    if (eqFold(word, "grave")) return &grave;
    if (eqFold(word, "dot")) return &dot;
    if (eqFold(word, "ddot")) return &ddot;
    if (eqFold(word, "bar") || eqFold(word, "overline") || eqFold(word, "vec") || eqFold(word, "dyad")) return &bar;
    if (eqFold(word, "under") || eqFold(word, "underline")) return &under;
    return nullptr;
}

// 수식 한 조각. baseline 은 상자 위에서 기준선까지.
struct Box {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t baseline = 0;
    bool empty = false;
    bool stretch = false;
    bool open = false;
    bool limits = false;
    // sum prod coprod inter union. 한계는 68%로 재고, 빈 한계는 높이에 넣지 않는다.
    bool sumOp = false;
    // int dint tint oint odint otint. 기호는 200%이고 빈 한계도 68% 칸으로 높이에 들어간다.
    bool intOp = false;
    bool prescript = false;
    bool prescriptSub = false;
    // 위첨자만, 또는 아래첨자만 있으면 저장 퍼센트는 기준선을 높이로 나눈 몫에 1을 더한다.
    bool quotBaseline = false;
    // 측정한 글자 크기. 겹친 위첨자의 25%는 이 값을 쓴다.
    std::int32_t fontEm = 0;
    // 수식 축에서 잰 상자 아래 끝. 글자 상자는 높이의 절반을 축 아래로 둔다.
    std::int32_t axisBottom = 0;
    // 가로로 붙은 글자 간격. 앞 글자는 이 상자의 첫 종류를, 이 상자는 끝 종류로 뒤를 본다.
    int headClass = 0;
    int tailClass = 0;
    // 위첨자나 아래첨자가 붙으면 뒤 간격은 본문 글자 크기의 표값×10%다. 7배가 아니다.
    bool scripted = false;
    std::int32_t gapEm = 0;
    // 단어 맨 앞의 sin, ln 같은 함수. 바로 앞 공백은 보통 글자 뒤에서만 얇은 빈칸을 더한다.
    bool fnHead = false;
    // 닫는 울타리의 위첨자는 울타리를 늘린 뒤에 붙인다. 늘어나기 전에는 -1이다.
    int heldSup = -1;
};

std::string canonicalSymbol(std::string_view font) {
    if (eqFold(font, "HYhwpEQ") || eqFold(font, "HyhwpEQ")) return "HyhwpEQ";
    return std::string(font);
}

std::string resolveFace(std::initializer_list<std::string_view> names) {
    for (std::string_view name : names) {
        if (name.empty()) continue;
        std::string face(name);
        if (chwp::layout::fontGlyphDesignAdvance(face, U'P') >= 0) return face;
    }
    return {};
}

// 글자 덩어리 앞에서 잘라 내는 명령. 긴 이름이 이긴다. rmbarAP 는 rm, bar, AP 다.
int leadingCommand(std::string_view word, bool symbolsOnly = false) {
    static const char* names[] = {
        "smallinter", "smallunion", "smallprod", "smallsum", "underline", "overline", "buildrel",
        "eqalign",    "bmatrix",    "pmatrix",   "dmatrix",  "coprod",     "choose",   "matrix",
        "cases",      "lpile",      "rpile",     "pile",     "binom",      "union",    "inter",
        "odint",      "otint",      "oint",      "dint",     "tint",       "prod",     "sqrt",
        "root",       "left",       "right",     "bold",     "over",       "atop",     "sum",
        "int",        "bar",        "hat",       "vec",      "dyad",       "under",    "check",
        "tilde",      "acute",      "grave",     "ddot",     "dot",        "arch",       "box",      "not",
        "rel",
        "bigg",       "color",      "scale",     "lsub",     "lsup",       "sup",      "sub",
        "lim",        "rm",         "it",
    };
    int best = 0;
    const auto consider = [&](std::string_view name) {
        if (name.size() < 2 || static_cast<int>(name.size()) <= best || name.size() > word.size()) return;
        if (name.size() < 3 && !eqFold(name, "rm") && !eqFold(name, "it") && !eqFold(name, "pi") &&
            !eqFold(name, "mu") && !eqFold(name, "nu") && !eqFold(name, "xi"))
            return;
        if (eqFold(word.substr(0, name.size()), name)) best = static_cast<int>(name.size());
    };
    if (!symbolsOnly) {
        for (const char* name : names) consider(name);
    }
    for (const Symbol& symbol : kSymbols) consider(symbol.name);
    return best;
}

struct Parser {
    std::vector<Token> tokens;
    std::size_t index = 0;
    std::int32_t em = 1000;
    bool romanMode = false;
    int braceDepth = 0;
    // 로만 글꼴이 수식 글꼴과 다르면 그 글꼴로 잰 글자는 ASCII 폭을 쓴다.
    // 둘을 같은 글꼴로 묶은 이름 측정은 글자 폭을 모두 ASCII 로 본다.
    bool romanDistinct = false;
    bool romanAlias = false;
    // matrix choose cases pile 의 줄 간격은 글자 크기의 15%, 칸 간격은 20%다.
    bool matrixRows = false;
    // eqalign 의 & 는 20% 칸이고, 다음 줄은 10% 간격으로 첫 줄 위에 쌓인다.
    bool eqAlignRows = false;
    // 중괄호 없는 위첨자를 재는 동안 아래첨자는 그 첨자가 아니라 본문으로 돌아간다. 같은 방향은 겹친다.
    int scriptSide = 0;
    const EquationFont* symbol = nullptr;
    const EquationFont* roman = nullptr;
    std::optional<Box> pendingRoot;
    // 닫는 울타리에 붙은 위첨자. 본문 높이로 울타리를 늘린 다음에 다시 붙인다.
    std::vector<Box> heldScripts;

    Parser(std::string_view script, std::int32_t baseUnit, const EquationFont& symbolFont,
           const EquationFont& romanFont, bool distinctRoman, bool aliasRoman)
        : tokens(tokenize(script)),
          em(baseUnit > 0 ? baseUnit : 1000),
          romanDistinct(distinctRoman),
          romanAlias(aliasRoman),
          symbol(&symbolFont),
          roman(&romanFont) {}

    const Token& current() const {
        static const Token end;
        return index < tokens.size() ? tokens[index] : end;
    }
    Kind kind() const { return current().kind; }
    bool wordIs(std::string_view name) const { return kind() == Kind::Word && eqFold(current().text, name); }
    void take() {
        if (index < tokens.size()) ++index;
    }
    void skipSpace() {
        while (kind() == Kind::Space) take();
    }
    bool stopped() const {
        const Kind k = kind();
        return k == Kind::End || k == Kind::RBrace || k == Kind::Hash || k == Kind::Amp;
    }
    bool infix() {
        peelCommand();
        return wordIs("over") || wordIs("atop") || wordIs("choose");
    }

    // 묶음 바로 뒤가 over, atop, choose 이면 그 묶음은 lim 의 본문이 아니다.
    bool groupBeforeInfix() const {
        if (kind() != Kind::LBrace) return false;
        int depth = 0;
        bool closed = false;
        std::size_t i = index;
        for (; i < tokens.size(); ++i) {
            if (tokens[i].kind == Kind::LBrace) {
                ++depth;
            } else if (tokens[i].kind == Kind::RBrace) {
                --depth;
                if (depth == 0) {
                    ++i;
                    closed = true;
                    break;
                }
            }
        }
        if (!closed) return false;
        while (i < tokens.size() && tokens[i].kind == Kind::Space) ++i;
        if (i >= tokens.size() || tokens[i].kind != Kind::Word) return false;
        const int n = leadingCommand(tokens[i].text, false);
        if (n < 4) return false;
        const std::string_view name(tokens[i].text.data(), static_cast<std::size_t>(n));
        return eqFold(name, "over") || eqFold(name, "atop") || eqFold(name, "choose");
    }

    // 글자 덩어리를 왼쪽부터 본다. rmbarAP 는 rm, bar 이고 tanalpha 는 tan 다음의 alpha 다.
    void peelCommand() {
        if (kind() != Kind::Word) return;
        const std::string text = current().text;
        std::vector<std::string> parts;
        // int 같은 구조 명령은 덩어리 맨 앞이나 방금 자른 명령 뒤에서만 자른다.
        // sint 의 int 는 글자이고, tanalpha 의 alpha 는 가운데 있어도 기호다.
        std::size_t i = 0;
        bool boundary = true;
        while (i < text.size()) {
            const int n = leadingCommand(std::string_view(text).substr(i), !boundary);
            if (n >= 2) {
                parts.emplace_back(text.substr(i, static_cast<std::size_t>(n)));
                i += static_cast<std::size_t>(n);
                boundary = true;
                continue;
            }
            const std::size_t start = i++;
            boundary = false;
            while (i < text.size() && leadingCommand(std::string_view(text).substr(i), true) < 2) ++i;
            parts.emplace_back(text.substr(start, i - start));
        }
        if (parts.size() <= 1) return;
        tokens[index].text = std::move(parts[0]);
        for (std::size_t k = parts.size(); k-- > 1;) {
            tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(index + 1),
                          Token{Kind::Word, std::move(parts[k])});
        }
    }
    const EquationFont& textFont() const {
        return romanMode && (romanDistinct || romanAlias) ? *roman : *symbol;
    }

    static std::int32_t designOf(const EquationFont& font, char32_t cp) {
        const auto found = font.advanceWidth.find(cp);
        if (found == font.advanceWidth.end()) return -1;
        return found->second;
    }

    bool takeNamed(std::string_view name) {
        const std::size_t mark = index;
        skipSpace();
        if (wordIs(name)) {
            take();
            return true;
        }
        index = mark;
        return false;
    }

    Box metricsOf() const {
        // 상자 높이는 글자 크기를 가장 가까운 픽셀로 맞춘 값이다. 10pt 맨 글자는 13픽셀(975).
        // rm 과 it 가 줄을 글자 크기까지 키우는 것은 너비 0인 그 명령 상자다. 글자 높이는 그대로다.
        const std::int32_t snapped = equationHeightPx(em) * kHwpPerPx;
        Box box;
        box.height = snapped;
        box.axisBottom = -box.height / 2;
        box.fontEm = em;
        // 글자·숫자 상자만의 기준선이다. 본문 줄에는 쓰지 않는다.
        // 상자는 세로 한가운데에 놓이므로 위에서 가운데까지가 높이의 절반이다.
        // 거기에 글자 크기의 35%를 더한다. 나눗셈은 버린다.
        // 10pt 맨 글자(975)는 350+487=837, 7pt 맨 글자(675)는 245+337=582, 8pt(825)는 280+412=692.
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) + box.height / 2;
        box.baseline = std::max<std::int32_t>(1, std::min(fromTop, box.height));
        return box;
    }

    // rm 과 it 는 너비 0, 높이는 글자 크기. 10pt 에서는 글자 975보다 큰 1000이라 줄 높이가 1000이 된다.
    // rm bar PA 는 막대 1170과 이 상자가 겹쳐 1182다.
    Box styleSwitch() const {
        Box box;
        box.width = 0;
        box.height = std::max(em, 0);
        box.axisBottom = -box.height / 2;
        box.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) + box.height / 2;
        box.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(box.height, 1)));
        return box;
    }

    std::int32_t symbolAdvance(char32_t cp) const {
        if (em <= 0 || symbol->unitsPerEm <= 0) return 0;
        const int design = designOf(*symbol, cp);
        if (design < 0) return 0;
        const std::int32_t px =
            equationGlyphPx(design, em, symbol->xAvgCharWidth, symbol->winCell, symbol->unitsPerEm);
        return std::max(0, px * kHwpPerPx);
    }

    std::int32_t advanceOf(const EquationFont& face, char32_t cp) const {
        if (em <= 0) return 0;
        if (face.unitsPerEm <= 0) return std::max(em / 2, 1);
        const bool romanLetters = romanMode || romanAlias || (romanDistinct && &face == roman);
        const bool onSymbol = symbol->unitsPerEm > 0 && &face == symbol;
        char32_t mapped = equationGlyph(cp, romanLetters);
        const EquationFont* metric = nullptr;
        if (mapped != 0) metric = symbol->unitsPerEm > 0 ? symbol : &face;
        else if (onSymbol) {
            if (const char32_t op = equationOperator(cp)) {
                mapped = op;
                metric = symbol;
            } else if (cp >= 0x80) {
                mapped = cp;
                metric = symbol;
            }
        }
        if (mapped != 0 && metric != nullptr && metric->unitsPerEm > 0) {
            const int design = designOf(*metric, mapped);
            if (design >= 0) {
                const std::int32_t px = equationGlyphPx(design, em, metric->xAvgCharWidth, metric->winCell,
                                                        metric->unitsPerEm);
                return std::max(0, px * kHwpPerPx);
            }
        }
        const int design = designOf(face, cp);
        if (design < 0) {
            // 글꼴에 없는 기호는 .notdef 다. HYhwpEQ 의 .notdef 진행폭은 em 과 같다.
            if (onSymbol && cp >= 0x80 && symbol->unitsPerEm > 0) {
                const std::int32_t px = equationGlyphPx(symbol->unitsPerEm, em, symbol->xAvgCharWidth, symbol->winCell,
                                                        symbol->unitsPerEm);
                return std::max(0, px * kHwpPerPx);
            }
            return std::max(em / 2, 1);
        }
        const double px =
            static_cast<double>(design) * em * kDpi / (static_cast<double>(kHwpPerInch) * face.unitsPerEm);
        const std::int32_t advPx = static_cast<std::int32_t>(std::llround(px));
        return std::max(0, advPx * kHwpPerPx);
    }

    Box measureText(const EquationFont& face, std::string_view text) {
        if (text.empty()) {
            Box box;
            box.empty = true;
            return box;
        }
        Box box = metricsOf();
        std::size_t i = 0;
        char32_t cp = 0;
        int head = 0;
        int tail = 0;
        bool any = false;
        while (nextCp(text, i, cp)) {
            box.width += advanceOf(face, cp);
            const int cls = equationAtomClass(cp);
            if (!any) head = cls;
            tail = cls;
            any = true;
        }
        if (any) {
            box.headClass = head;
            box.tailClass = tail;
        }
        return box;
    }

    // 맞닿은 두 글자 사이의 간격. 표 값의 7배다. 10pt 관계식은 210이다.
    std::int32_t atomGap(const Box& left, const Box& right) const {
        const int a = left.tailClass;
        const int b = right.headClass;
        if (a < 1 || a > 9 || b < 1 || b > 9 || left.fontEm <= 0) return 0;
        static constexpr int table[10][10] = {
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 1, 2, 3, 1, 1, 0, 1, 0}, {0, 1, 3, 2, 3, 0, 0, 0, 1, 0},
            {0, 2, 0, 0, 0, 2, 0, 0, 2, 0}, {0, 3, 3, 0, 0, 3, 0, 0, 3, 0}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
            {0, 0, 1, 2, 3, 0, 0, 0, 1, 0}, {0, 1, 1, 0, 1, 1, 1, 1, 1, 0}, {0, 1, 1, 2, 3, 1, 0, 1, 1, 0},
            {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        };
        const int pct = table[a][b];
        if (pct == 0) return 0;
        // 첨자가 붙은 글자 뒤는 본문 글자 크기에 표값의 10배를 쓴다. 11pt x^2 -2 는 220, x^2 =1 은 330이다.
        // 첨자가 없으면 표값의 7배다. 11pt x-2 는 154다.
        if (left.scripted && left.gapEm > 0) return mulDiv(left.gapEm, pct * 10, 100);
        return 7 * mulDiv(left.fontEm, pct, 100);
    }

    Box hpack(const std::vector<Box>& items) const {
        std::vector<Box> real;
        for (const Box& box : items) {
            if (!box.empty) real.push_back(box);
        }
        if (real.empty()) {
            Box box;
            box.empty = true;
            return box;
        }
        if (real.size() == 1) return real[0];
        std::int32_t above = 0;
        std::int32_t below = 0;
        std::int32_t width = 0;
        for (std::size_t i = 0; i < real.size(); ++i) {
            above = std::max(above, real[i].baseline);
            below = std::max(below, real[i].height - real[i].baseline);
            width += real[i].width;
            if (i + 1 < real.size()) width += atomGap(real[i], real[i + 1]);
        }
        Box out;
        out.width = width;
        out.height = above + below;
        out.baseline = above;
        out.axisBottom = real[0].axisBottom;
        out.fontEm = real.back().fontEm;
        out.headClass = real.front().headClass;
        out.tailClass = real.back().tailClass;
        out.scripted = real.back().scripted;
        out.gapEm = real.back().gapEm;
        out.fnHead = real.front().fnHead;
        for (const Box& box : real) {
            out.axisBottom = std::min(out.axisBottom, box.axisBottom);
            if (box.quotBaseline) out.quotBaseline = true;
        }
        return out;
    }

    Box gap(std::int32_t width) const {
        Box box = metricsOf();
        box.width = std::max(width, 0);
        return box;
    }

    void stretch(std::vector<Box>& items, std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo; i < hi; ++i) {
            if (!items[i].stretch || !items[i].open) continue;
            int depth = 1;
            std::size_t j = i + 1;
            for (; j < hi; ++j) {
                if (!items[j].stretch) continue;
                depth += items[j].open ? 1 : -1;
                if (depth == 0) break;
            }
            const std::size_t contentEnd = depth == 0 ? j : hi;
            stretch(items, i + 1, contentEnd);
            const Box packed = hpack(std::vector<Box>(items.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                                      items.begin() + static_cast<std::ptrdiff_t>(contentEnd)));
            if (!packed.empty && packed.height > 0) {
                items[i].height = packed.height;
                items[i].baseline = packed.baseline;
                items[i].axisBottom = packed.axisBottom;
                if (depth == 0) {
                    items[j].height = packed.height;
                    items[j].baseline = packed.baseline;
                    items[j].axisBottom = packed.axisBottom;
                }
            }
            i = depth == 0 ? j : hi - 1;
        }
    }

    Box centerMatrix(Box box) const {
        if (box.empty || box.height <= 0) return box;
        box.axisBottom = -(box.height / 2);
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - box.axisBottom;
        box.baseline = std::max<std::int32_t>(1, std::min(fromTop, box.height));
        box.quotBaseline = true;
        box.fontEm = em;
        return box;
    }

    Box addFences(Box box, char32_t left, char32_t right) const {
        if (left != 0) box.width += symbolAdvance(left);
        if (right != 0) box.width += symbolAdvance(right);
        return box;
    }

    Box packTable(const std::vector<std::vector<Box>>& rows, bool matrix = false) {
        if (rows.empty()) {
            Box box;
            box.empty = true;
            return box;
        }
        std::size_t cols = 1;
        for (const auto& row : rows) cols = std::max(cols, row.size());
        std::vector<std::int32_t> colW(cols, 0);
        std::vector<std::int32_t> rowH;
        std::vector<std::int32_t> rowBase;
        for (const auto& row : rows) {
            std::int32_t above = 0;
            std::int32_t below = 0;
            for (std::size_t c = 0; c < row.size(); ++c) {
                colW[c] = std::max(colW[c], row[c].width);
                above = std::max(above, row[c].baseline);
                below = std::max(below, row[c].height - row[c].baseline);
            }
            if (above == 0 && below == 0) {
                above = em * 86 / 100;
                below = em - above;
            }
            rowH.push_back(above + below);
            rowBase.push_back(above);
        }
        const std::int32_t gapX = matrix || eqAlignRows ? mulDiv(em, 20, 100) : std::max(em / 2, 1);
        const std::int32_t gapY =
            eqAlignRows ? mulDiv(em, 10, 100) : (matrix ? mulDiv(em, 15, 100) : std::max(em / 3, 1));
        Box out;
        for (std::int32_t w : colW) out.width += w;
        if (cols > 1) out.width += gapX * static_cast<std::int32_t>(cols - 1);
        if (eqAlignRows) return packEqAlign(rows, out.width, gapY);
        for (std::int32_t h : rowH) out.height += h;
        if (rowH.size() > 1) out.height += gapY * static_cast<std::int32_t>(rowH.size() - 1);
        if (matrix) return centerMatrix(out);
        out.baseline = rowH.size() == 1 ? rowBase[0] : out.height / 2;
        return out;
    }

    // 첫 줄의 기준선을 유지하고, 다음 줄은 그 위에 글자 크기의 10% 간격으로 쌓는다.
    // 11pt eqalign {a # b} 는 525×2360, 기준선 41이다. a &= b 는 2251×1125다.
    Box packEqAlign(const std::vector<std::vector<Box>>& rows, std::int32_t width, std::int32_t gapY) const {
        std::vector<std::int32_t> rowH;
        std::vector<std::int32_t> rowBottom;
        for (const auto& row : rows) {
            bool any = false;
            std::int32_t bottom = 0;
            std::int32_t top = 0;
            for (const Box& cell : row) {
                if (cell.empty || cell.height <= 0) continue;
                const std::int32_t cellTop = cell.axisBottom + cell.height;
                if (!any) {
                    bottom = cell.axisBottom;
                    top = cellTop;
                    any = true;
                } else {
                    bottom = std::min(bottom, cell.axisBottom);
                    top = std::max(top, cellTop);
                }
            }
            if (!any) {
                const std::int32_t snapped = equationHeightPx(em) * kHwpPerPx;
                bottom = -snapped / 2;
                top = bottom + snapped;
            }
            rowBottom.push_back(bottom);
            rowH.push_back(top - bottom);
        }
        Box out;
        out.width = width;
        for (std::int32_t h : rowH) out.height += h;
        if (rowH.size() > 1) out.height += gapY * static_cast<std::int32_t>(rowH.size() - 1);
        out.axisBottom = rowBottom.empty() ? 0 : rowBottom.front();
        out.fontEm = em;
        out.quotBaseline = true;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        return out;
    }

    Box finishRows(const std::vector<std::vector<Box>>& rows) {
        if (rows.size() == 1 && rows[0].size() <= 1) {
            if (rows[0].empty() || rows[0][0].empty) {
                Box box;
                box.empty = true;
                return box;
            }
            return matrixRows ? centerMatrix(rows[0][0]) : rows[0][0];
        }
        return packTable(rows, matrixRows);
    }

    Box corners(const Box& base, const Box* sub, const Box* sup) const {
        // 위첨자만 있으면 첨자 목록의 기준선과, 첨자를 잰 뒤 남은 글자 크기의 25%로 높이를 정한다.
        // 글자 한 개의 목록 기준선은 높이의 절반을 아래로 둔 값이다. 10pt x^2 는 1163, 17pt는 1998이다.
        // 겹치면 안쪽 상자의 기준선과 그 안쪽에 남은 글자 크기를 쓴다. 10pt x^2^2 는 1309다.
        // 아래첨자만 있으면 첨자 목록의 기준선과, 첨자를 잰 뒤 남은 글자 크기의 20%로 높이를 정한다.
        // 늘어난 높이는 글자 아래에 붙이고 기준선은 본문 글자에 둔다.
        // 10pt x_2 는 975에 202를 더해 1177, 10pt x_2_2 는 1354다. 저장 퍼센트는 그 기준선을 높이로 나눈 몫에 1을 더한다.
        const bool hasSup = sup && !sup->empty && sup->height > 0;
        const bool hasSub = sub && !sub->empty && sub->height > 0;
        std::int32_t top = 0;
        std::int32_t bottom = base.height;
        std::int32_t extra = 0;
        std::int32_t outAxis = base.axisBottom;
        std::int32_t outFont = base.fontEm;
        if (hasSup && !hasSub) {
            // 25%의 피제수는 첨자를 잰 뒤 폰트에 남은 글자 크기다. 한 겹은 본문의 68%이고,
            // x^2^2 의 바깥은 안쪽 첨자를 잰 뒤 남은 크기라 한 단계 더 68%다. 반올림은 MulDiv다.
            // 첨자 목록의 기준선이 글자 기준선보다 아래면 그 거리를 높이에 남긴다.
            // 늘어난 높이는 글자 위에 붙이고, 글자 아래는 그대로 둔다. 10pt x^2 는 1163, 10pt x^2^2 는 1309다.
            const std::int32_t baseAxis = base.axisBottom;
            const std::int32_t scriptAxis = sup->axisBottom;
            const std::int32_t gap25 = mulDiv(sup->fontEm, 25, 100);
            const std::int32_t shift = gap25 + baseAxis;
            const std::int32_t probe = sup->height + scriptAxis + shift;
            const std::int32_t finalBottom = probe > 0 ? -sup->height : scriptAxis + shift;
            const std::int32_t listBottom = std::min(baseAxis, finalBottom);
            const std::int32_t listHeight = (base.height + baseAxis) - listBottom;
            top = base.height - listHeight;
            bottom = base.height;
            extra = sup->width;
            outAxis = listBottom;
            outFont = sup->fontEm;
        }
        if (hasSub && !hasSup) {
            // 20%의 피제수는 첨자를 잰 뒤 폰트에 남은 글자 크기다. 한 겹은 본문의 68%이고,
            // x_2_2 의 바깥은 안쪽 첨자를 잰 뒤 남은 크기라 한 단계 더 68%다.
            // 첨자 목록 기준선에 본문 위쪽을 더한 값이 20%보다 크면 그 20%를 빼서 위로 올린다.
            // 글자 한 개면 첨자 높이의 반(반올림)에서 20%를 뺀 만큼 아래로 늘어난다.
            // 10pt x_2 는 1177, 10pt x_2_2 는 1354다. 기준선은 본문 글자 자리에 남는다.
            const std::int32_t baseAxis = base.axisBottom;
            const std::int32_t scriptAxis = sub->axisBottom;
            const std::int32_t gap20 = mulDiv(sub->fontEm, 20, 100);
            const std::int32_t letterTop = base.height + baseAxis;
            const std::int32_t shift = letterTop - gap20;
            const std::int32_t probe = scriptAxis + shift;
            const std::int32_t scriptTop = probe < 0 ? sub->height : scriptAxis + shift + sub->height;
            const std::int32_t listTop = std::max(letterTop, scriptTop);
            bottom = listTop - baseAxis;
            extra = sub->width;
            outAxis = baseAxis;
            outFont = sub->fontEm;
        }
        if (hasSup && hasSub) {
            // 위첨자와 아래첨자는 각각 한 방향 식을 그대로 쓰고 상자를 합친다.
            // 위쪽 끝은 위첨자 기준선이고 아래쪽 끝은 아래첨자 끝이다. 10pt x^2_2 는 1365, 기준선 76이다.
            const std::int32_t baseAxis = base.axisBottom;
            const std::int32_t gap25 = mulDiv(sup->fontEm, 25, 100);
            const std::int32_t supShift = gap25 + baseAxis;
            const std::int32_t supProbe = sup->height + sup->axisBottom + supShift;
            const std::int32_t supFinal = supProbe > 0 ? -sup->height : sup->axisBottom + supShift;
            const std::int32_t supEdge = std::min(baseAxis, supFinal);
            const std::int32_t gap20 = mulDiv(sub->fontEm, 20, 100);
            const std::int32_t letterTop = base.height + baseAxis;
            const std::int32_t subShift = letterTop - gap20;
            const std::int32_t subProbe = sub->axisBottom + subShift;
            const std::int32_t subTop = subProbe < 0 ? sub->height : sub->axisBottom + subShift + sub->height;
            const std::int32_t subEdge = std::max(letterTop, subTop);
            top = supEdge - baseAxis;
            bottom = subEdge - baseAxis;
            extra = std::max(sup->width, sub->width);
            outAxis = supEdge;
            outFont = sup->fontEm;
        }
        Box out;
        out.width = base.width + extra;
        out.height = bottom - top;
        out.baseline = base.baseline - top;
        out.axisBottom = outAxis;
        out.fontEm = outFont;
        // 위첨자나 아래첨자가 있으면 글자 기준선 공식을 그대로 쓰고, 나눈 몫에 1을 더해 저장한다.
        out.quotBaseline = hasSup || hasSub;
        // 첨자가 붙어도 앞뒤 간격은 본문 글자 종류를 따른다. t=t_0 의 = 뒤는 21%다.
        // 간격의 기준 크기는 첨자로 줄어들기 전의 본문이다. 뒤 연산자는 그 크기의 표값×10%를 둔다.
        out.headClass = base.headClass;
        out.tailClass = base.tailClass;
        out.fnHead = base.fnHead;
        if (hasSup || hasSub) {
            out.scripted = true;
            out.gapEm = base.fontEm > 0 ? base.fontEm : em;
        }
        return out;
    }

    Box sumStack(const Box& glyph, const Box* sub, const Box* sup) const {
        // 합·곱·교집합·합집합은 기호를 글자 크기의 180%로 재고, 그 높이의 75%를 간격으로 둔다.
        // 한계는 68%다. 첨자가 없으면 그 칸은 높이에 넣지 않는다.
        // 10pt sum 은 1275×1350, 기준선 76이다. sum_a 는 2025/51, sum_a^b 는 2700/63이다.
        const bool realSub = sub && !sub->empty && sub->height > 0;
        const bool realSup = sup && !sup->empty && sup->height > 0;
        const std::int32_t subH = realSub ? sub->height : 0;
        const std::int32_t supH = realSup ? sup->height : 0;
        const std::int32_t subW = realSub ? sub->width : 0;
        const std::int32_t supW = realSup ? sup->width : 0;
        const std::int32_t gap = mulDiv(glyph.height, 75, 100);
        Box out;
        out.width = std::max(glyph.width, std::max(subW, supW));
        out.height = gap + subH + supH;
        out.axisBottom = -(supH + gap / 2);
        out.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        out.quotBaseline = true;
        return out;
    }

    Box intStack(const Box& glyph, const Box* sub, const Box* sup) const {
        // 적분 여섯은 기호를 글자 크기의 200%로 잰다.
        // 입력창의 빈 한계는 네모지만, 파일에 넣는 상자에서는 그 너비와 높이를 뺀다.
        // 68%의 6분의 1만 기호를 옮긴다. 11pt 빈 적분은 높이 2050, 기준선 66이다.
        // 오른쪽 빈 칸의 절반도 넣지 않는다. 뒤의 묶음이 있으면 그 너비를 더한다.
        const std::int32_t slotH = mulDiv(em, 68, 100);
        const bool realSub = sub && !sub->empty && sub->height > 0;
        const bool realSup = sup && !sup->empty && sup->height > 0;
        const std::int32_t subH = realSub ? sub->height : 0;
        const std::int32_t supH = realSup ? sup->height : 0;
        const std::int32_t subW = realSub ? sub->width : 0;
        const std::int32_t supW = realSup ? sup->width : 0;
        const std::int32_t subBottom = realSub ? sub->axisBottom : 0;
        const std::int32_t supBottom = realSup ? sup->axisBottom : 0;
        std::int32_t half = glyph.height / 2;
        std::int32_t shift = slotH / 6 - half;
        if (subBottom + half < 0) half = -subBottom;
        const std::int32_t supProbe = supH + shift + supBottom;
        if (supProbe > 0) shift -= supProbe;
        const std::int32_t bottom = std::min(supBottom + shift, 0);
        const std::int32_t lower = std::max(subBottom + half + subH, 0);
        Box out;
        out.width = std::max(subW - mulDiv(em, 50, 100), supW + mulDiv(em, 5, 100)) + mulDiv(em, 1, 100) +
                    glyph.width;
        out.height = lower - bottom;
        out.axisBottom = bottom;
        out.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        out.quotBaseline = true;
        // = int 는 관계 간격을 둔다. int x 는 간격을 더하지 않는다.
        out.headClass = 1;
        return out;
    }

    // lim 은 글자 크기의 120%로 l,i,m 을 잰다.
    // 빈 아래첨자와 오른쪽 빈 칸은 파일 상자에서 뺀다. 내용이 있는 { } 만 그 너비를 더한다.
    // 11pt lim _{n}{a_n b_n}=0 은 5406×2100 이다.
    Box limBox(const Box& word, const Box* sub, const Box* sup, const Box* slot) const {
        const bool realSub = sub && !sub->empty && sub->height > 0;
        const bool realSup = sup && !sup->empty && sup->height > 0;
        const std::int32_t subH = realSub ? sub->height : 0;
        const std::int32_t supH = realSup ? sup->height : 0;
        const std::int32_t half = word.height / 2;
        std::int32_t bottom = -half;
        std::int32_t top = half + subH + supH;
        // 빈 {} 는 칸이 아니다. 내용이 있는 묶음만 너비에 더하고, 더 크면 높이도 늘린다.
        std::int32_t side = 0;
        if (slot && !slot->empty && slot->height > 0) {
            side = slot->width;
            bottom = std::min(bottom, slot->axisBottom);
            top = std::max(top, slot->axisBottom + slot->height);
        }
        Box out;
        out.width = std::max(word.width, std::max(realSub ? sub->width : 0, realSup ? sup->width : 0)) + side;
        out.height = top - bottom;
        out.axisBottom = bottom;
        out.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - bottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        out.quotBaseline = true;
        // = lim 은 관계 간격을 둔다. lim = 는 간격을 더하지 않는다.
        out.headClass = 1;
        return out;
    }

    Box limitsOf(const Box& base, const Box* sub, const Box* sup) const {
        const std::int32_t gap = std::max(em / 10, 1);
        Box out = base;
        out.limits = false;
        out.prescript = false;
        if (sup && !sup->empty && sup->height > 0) {
            out.width = std::max(out.width, sup->width);
            out.height += gap + sup->height;
            out.baseline += gap + sup->height;
        }
        if (sub && !sub->empty && sub->height > 0) {
            out.width = std::max(out.width, sub->width);
            out.height += gap + sub->height;
        }
        return out;
    }

    Box attachPrescript(const Box& base, const Box& script, bool sub) const {
        const std::int32_t scriptEm = std::max<std::int32_t>(em * 68 / 100, 1);
        const std::int32_t raise = scriptEm * 25 / 100;
        const std::int32_t tuck = scriptEm * 70 / 100;
        const std::int32_t scriptTop = sub ? base.height - tuck : -raise;
        const std::int32_t top = std::min(0, scriptTop);
        const std::int32_t bottom = std::max(base.height, scriptTop + scriptEm);
        Box out;
        out.width = script.width + base.width;
        out.height = bottom - top;
        out.baseline = base.baseline - top;
        return out;
    }

    Box fraction(const Box& num, const Box& den, bool rule) const {
        // over: 간격은 글자크기의 30%, 너비 더하기는 50%.
        // 너비는 분자·분모·글자크기 절반 중 큰 값에 50%를 더한다. 절반은 0 쪽으로 자른다.
        // 기준선은 분자 높이에 간격의 절반을 더한 곳이다. 10pt a over b 는 1025×2250, 기준은 한가운데.
        // 10pt b over b 는 글자가 절반보다 좁아 1000이다.
        // atop: 막대 글자 U+E05B의 높이를 10으로 자르고, 그 값의 5배를 너비에 더한다.
        // 너비 바닥은 그 막대 글자의 가로다. 8pt a atop b 는 785×1732, 10pt는 간격 97이다.
        std::int32_t gap = 0;
        std::int32_t pad = 0;
        std::int32_t floor = 0;
        if (rule) {
            gap = mulDiv(em, 30, 100);
            pad = mulDiv(em, 50, 100);
            floor = em > 0 ? em / 2 : 0;
        } else {
            const std::int32_t barH = equationHeightPx(std::max(em, 0)) * kHwpPerPx;
            gap = barH / 10;
            pad = mulDiv(gap, 500, 100);
            floor = symbolAdvance(0xE05B);
        }
        Box out;
        out.width = std::max(num.width, std::max(den.width, floor)) + pad;
        out.height = num.height + den.height + gap;
        // 분자 쪽이 음수 방향(위)이다. 저장 기준선은 그 끝에서 글자 크기 35%를 더한 자리다.
        // 10pt a over b 는 1475/2250 을 버린 뒤 1을 더해 66이다.
        out.axisBottom = -(num.height + gap / 2);
        out.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        out.quotBaseline = true;
        // 분수 앞의 = 나 - 는 보통 글자와의 간격을 둔다. 분수 뒤는 간격을 더하지 않는다.
        out.headClass = 1;
        return out;
    }

    Box radical(const Box& inner, const Box& degree) {
        // sqrt. 근호 U+E05C의 세로가 나눗셈의 제수다. 그 세로는 글자 높이와 같다.
        // 몫이 100보다 작으면 100으로 올리고, 그 몫으로 세로를 다시 곱한다.
        // 지수가 그 52%보다 5% 이상 크면 그만큼 더하고, 아니면 5%만 더한다.
        // 가로는 본문 너비에 17%와 근호 가로를 더한다. 지수가 근호 가로의 절반보다 넓으면 그 차이도 더한다.
        // 10pt sqrt x 는 높이 1123, 기준선 88. 지수가 x^2 이면 1153이다.
        const std::int32_t glyphH = equationHeightPx(std::max(em, 0)) * kHwpPerPx;
        const std::int32_t glyphW = symbolAdvance(0xE05C);
        const std::int32_t ten = mulDiv(em, 10, 100);
        const std::int32_t five = mulDiv(em, 5, 100);
        std::int32_t scale = 100;
        if (glyphH > 0) {
            const std::int32_t quot = static_cast<std::int32_t>(
                (static_cast<std::int64_t>(inner.height) + ten) * 100 / glyphH);
            if (quot > scale) scale = quot;
        }
        const std::int32_t scaled = mulDiv(glyphH, scale, 100);
        std::int32_t tail = degree.height - mulDiv(scaled, 52, 100);
        if (tail <= five) tail = five;
        const std::int32_t overflow = degree.width - glyphW / 2;
        Box out;
        out.width = inner.width + mulDiv(em, 17, 100) + glyphW + std::max(overflow, 0);
        out.height = scaled + tail;
        out.axisBottom = inner.axisBottom - ten - tail;
        out.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        out.quotBaseline = true;
        // 근호 앞의 = 는 보통 글자와의 간격을 둔다. 근호 뒤의 ) 에는 간격을 더하지 않는다.
        out.headClass = 1;
        return out;
    }

    Box accent(const Box& inner, const Accent& mark) const {
        // 세로 여분은 악센트 글리프 높이의 20%다. 10pt 는 195라 hat x 가 1170, 기준선 89다.
        // hat 계열은 너비가 글자와 기호 중 큰 값이다. bar vec dyad under 는 거기에 기호 너비의 20%를 더한다.
        // under 는 같은 여분을 아래로 붙이고 기준선은 본문 글자에 둔다.
        const std::int32_t glyphH = equationHeightPx(std::max(em, 0)) * kHwpPerPx;
        const std::int32_t extra = mulDiv(glyphH, 20, 100);
        const std::int32_t glyphW = symbolAdvance(mark.cp);
        Box out = inner;
        out.width = std::max(inner.width, glyphW);
        if (mark.widen) out.width += mulDiv(glyphW, 20, 100);
        out.height = inner.height + extra;
        out.fontEm = inner.fontEm;
        if (mark.under) {
            out.axisBottom = inner.axisBottom;
            out.baseline = inner.baseline;
        } else {
            out.axisBottom = inner.axisBottom - extra;
            out.baseline = inner.baseline + extra;
        }
        out.quotBaseline = true;
        // 앞 연산자는 막대를 보통 글자로 보고 간격을 둔다. 막대 자신은 다음 글자에 간격을 더하지 않는다.
        out.headClass = 1;
        out.tailClass = 0;
        return out;
    }

    Box delimiter(std::string_view text, bool open) {
        Box box = metricsOf();
        box.stretch = true;
        box.open = open;
        if (text.empty() || text == ".") {
            box.width = 0;
            return box;
        }
        const char32_t piece = fencePiece(text);
        if (piece != 0) {
            box.width = symbolAdvance(piece);
        } else {
            char32_t cp = symbolCode(text);
            if (cp == 0) {
                std::size_t i = 0;
                nextCp(text, i, cp);
            }
            box.width = cp == 0 ? 0 : advanceOf(*symbol, cp);
        }
        // 여는 울타리 앞의 = 는 관계 간격을 둔다. 글자 뒤는 간격을 더하지 않는다.
        // 11pt y= LEFT | 는 울타리 앞에 231이 있다. f LEFT ( 는 없다.
        if (open && box.width > 0) box.headClass = 1;
        return box;
    }

    Box takeDelim(bool open) {
        skipSpace();
        std::string text = ".";
        if (kind() == Kind::LBrace || kind() == Kind::RBrace || kind() == Kind::Symbol || kind() == Kind::Word ||
            kind() == Kind::Number) {
            text = current().text;
            take();
        }
        return delimiter(text, open);
    }

    Box wrapDelim(Box inner, const char* left, const char* right) {
        std::vector<Box> parts;
        const std::int32_t pad = std::max(em / 8, 1);
        if (left && left[0] != '\0') {
            Box side = delimiter(left, true);
            side.height = inner.height;
            side.baseline = inner.baseline;
            parts.push_back(side);
            Box padBox = gap(pad);
            padBox.height = inner.height;
            padBox.baseline = inner.baseline;
            parts.push_back(padBox);
        }
        parts.push_back(inner);
        if (right && right[0] != '\0') {
            Box padBox = gap(pad);
            padBox.height = inner.height;
            padBox.baseline = inner.baseline;
            parts.push_back(padBox);
            Box side = delimiter(right, false);
            side.height = inner.height;
            side.baseline = inner.baseline;
            parts.push_back(side);
        }
        return hpack(parts);
    }

    template <class F>
    Box atEm(std::int32_t next, F fn) {
        const std::int32_t saved = em;
        em = std::max<std::int32_t>(1, next);
        Box box = fn();
        em = saved;
        return box;
    }

    // 첨자 글자 크기는 본문의 68%를 MulDiv로 반올림한 값이다. 254pt 위첨자 블록은 230픽셀(17250)이다.
    template <class F>
    Box atScript(F fn) {
        return atEm(mulDiv(em, 68, 100), fn);
    }

    Box lines();
    Box expr();
    Box term();
    Box piece();
    Box prefix();
    Box scriptRun();
    Box readGroup();
    Box bracedArg();
    Box radicalArg();
    Box bigOperator(const Big& op);
    Box emitWord(std::string_view word);

    bool rootIndexAhead() {
        if (kind() != Kind::Sup) return false;
        const std::size_t mark = index;
        take();
        if (kind() != Kind::Number) {
            index = mark;
            return false;
        }
        const std::string digits = current().text;
        take();
        if (!wordIs("sqrt") && !wordIs("root")) {
            index = mark;
            return false;
        }
        pendingRoot = atEm(mulDiv(em, 50, 100), [&] { return measureText(textFont(), digits); });
        return true;
    }

    bool rootHasOf() const {
        int depth = 0;
        for (std::size_t i = index; i < tokens.size(); ++i) {
            const Kind k = tokens[i].kind;
            if (k == Kind::LBrace) {
                ++depth;
                continue;
            }
            if (k == Kind::RBrace) {
                if (depth == 0) break;
                --depth;
                continue;
            }
            if (depth == 0 && k == Kind::Word && eqFold(tokens[i].text, "of")) return true;
            if (depth == 0 && (k == Kind::Hash || k == Kind::Amp)) break;
        }
        return false;
    }
};

Box Parser::readGroup() {
    const bool savedRoman = romanMode;
    ++braceDepth;
    take();
    Box inner = lines();
    if (kind() == Kind::RBrace) take();
    --braceDepth;
    romanMode = savedRoman;
    return inner;
}

Box Parser::bracedArg() {
    skipSpace();
    if (kind() == Kind::LBrace) return readGroup();
    return piece();
}

Box Parser::radicalArg() {
    // 중괄호가 없으면 공백 앞까지가 근호 안이다. sqrty+1 은 y+1 의 근호이고, sqrt y + 1 은 y 의 근호에 1을 더한다.
    skipSpace();
    if (kind() == Kind::LBrace) return readGroup();
    return term();
}

Box Parser::scriptRun() {
    // lim _ {n} 처럼 첨자 기호와 중괄호 사이의 공백은 첨자 본문 앞의 공백이다.
    skipSpace();
    if (kind() == Kind::LBrace) {
        const int saved = scriptSide;
        scriptSide = 0;
        Box group = readGroup();
        scriptSide = saved;
        return group;
    }
    // 첨자 본문은 바깥과 같은 식 해석을 다시 탄다. x^2^2 의 두 번째 ^ 는 첫 첨자 2 의 위첨자다.
    // 글자 크기는 들어갈 때마다 68%다. 반대 방향 첨자는 이 첨자가 아니라 본문에 붙는다.
    std::vector<Box> parts;
    int guard = 0;
    while (guard++ < 64 && !stopped() && kind() != Kind::Space && !infix()) {
        if (scriptSide > 0 && kind() == Kind::Sub) break;
        if (scriptSide < 0 && kind() == Kind::Sup) break;
        // pi^2cos 의 cos 는 위첨자 밖이다. 첨자가 이미 있으면 함수 앞에서 멈춘다.
        if (!parts.empty() && kind() == Kind::Word) {
            peelCommand();
            if (uprightPrefix(current().text) > 0) break;
        }
        const std::size_t before = index;
        Box box = piece();
        if (!box.empty) parts.push_back(std::move(box));
        if (index == before) break;
    }
    return hpack(parts);
}

Box Parser::bigOperator(const Big& op) {
    const char32_t integral = op.small ? 0 : intGlyph(op.name);
    if (integral != 0) {
        const std::int32_t opEm = mulDiv(em, 200, 100);
        Box glyph = atEm(opEm, [&] {
            Box box;
            box.width = symbolAdvance(integral);
            box.height = equationHeightPx(std::max(opEm, 0)) * kHwpPerPx;
            return box;
        });
        Box sub;
        Box sup;
        const Box* subPtr = nullptr;
        const Box* supPtr = nullptr;
        if (takeNamed("from")) {
            sub = atScript([&] { return bracedArg(); });
            subPtr = &sub;
        }
        if (takeNamed("to")) {
            sup = atScript([&] { return bracedArg(); });
            supPtr = &sup;
        }
        if (subPtr || supPtr) return intStack(glyph, subPtr, supPtr);
        glyph.intOp = true;
        return glyph;
    }
    const char32_t summed = op.small ? 0 : sumGlyph(op.name);
    if (summed != 0) {
        const std::int32_t opEm = mulDiv(em, 180, 100);
        Box glyph = atEm(opEm, [&] {
            Box box;
            box.width = symbolAdvance(summed);
            box.height = equationHeightPx(std::max(opEm, 0)) * kHwpPerPx;
            return box;
        });
        Box sub;
        Box sup;
        const Box* subPtr = nullptr;
        const Box* supPtr = nullptr;
        if (takeNamed("from")) {
            sub = atScript([&] { return bracedArg(); });
            subPtr = &sub;
        }
        if (takeNamed("to")) {
            sup = atScript([&] { return bracedArg(); });
            supPtr = &sup;
        }
        if (subPtr || supPtr) return sumStack(glyph, subPtr, supPtr);
        glyph.sumOp = true;
        return glyph;
    }
    Box body = atEm(op.small ? em : em * 8 / 5, [&] {
        Box box = metricsOf();
        box.height = em;
        box.baseline = em / 2;
        const bool hasGlyph = symbol->unitsPerEm > 0 && designOf(*symbol, op.cp) >= 0;
        box.width = hasGlyph ? advanceOf(*symbol, op.cp) : em;
        box.limits = true;
        return box;
    });
    const Box* subPtr = nullptr;
    const Box* supPtr = nullptr;
    Box sub;
    Box sup;
    if (takeNamed("from")) {
        sub = atScript([&] { return bracedArg(); });
        subPtr = &sub;
    }
    if (takeNamed("to")) {
        sup = atScript([&] { return bracedArg(); });
        supPtr = &sup;
    }
    if (!subPtr && !supPtr) return body;
    Box stacked = limitsOf(body, subPtr, supPtr);
    stacked.limits = true;
    return stacked;
}

Box Parser::emitWord(std::string_view word) {
    std::vector<Box> parts;
    const int n = uprightPrefix(word);
    if (n > 0) {
        // 함수 이름은 수식 글꼴의 곧게 선 ASCII 다. 바탕이 아니다.
        const bool saved = romanMode;
        romanMode = true;
        Box box = measureText(*symbol, word.substr(0, static_cast<std::size_t>(n)));
        romanMode = saved;
        box.fnHead = true;
        parts.push_back(std::move(box));
    }
    if (static_cast<int>(word.size()) > n) parts.push_back(measureText(textFont(), word.substr(static_cast<std::size_t>(n))));
    return hpack(parts);
}

Box Parser::prefix() {
    if (kind() == Kind::LBrace) return readGroup();
    if (kind() == Kind::Thin) {
        take();
        // ` 는 보통 글자다. 11pt 너비는 137이고, 뒤의 : 와는 관계 간격 231을 둔다.
        Box box = gap(equationBlank(em) / 4);
        box.headClass = 1;
        box.tailClass = 1;
        return box;
    }
    if (kind() == Kind::Wide) {
        take();
        // ~ 도 보통 글자다. 11pt 너비는 550이고, 뒤의 : 와는 231을 둔다.
        Box box = gap(equationBlank(em));
        box.headClass = 1;
        box.tailClass = 1;
        return box;
    }
    if (kind() == Kind::Quote) {
        const std::string text = current().text;
        take();
        return measureText(*roman, text);
    }
    if (kind() == Kind::Number) {
        const std::string text = current().text;
        take();
        return measureText(textFont(), text);
    }
    if (kind() == Kind::Symbol) {
        const std::string text = current().text;
        take();
        if (const char32_t joined = equationOperatorToken(text)) {
            const std::int32_t width = symbolAdvance(joined);
            if (width > 0) {
                Box box = metricsOf();
                box.width = width;
                // -> 는 rarrow 와 같다. 앞뒤 글자와의 간격은 0이다. 11pt n -> INF 는 2700이다.
                // +- 는 이항이고, 나머지 묶음 연산자는 관계식이다.
                int atom = 4;
                if (text == "+-" || text == "!=") atom = 3;
                else if (text == "->" || text == "<-" || text == "<->") atom = 1;
                box.headClass = atom;
                box.tailClass = atom;
                return box;
            }
        }
        const char32_t mapped = symbolCode(text);
        const EquationFont& face = symbol->unitsPerEm > 0 ? *symbol : textFont();
        if (mapped != 0) {
            Box box = metricsOf();
            box.width = advanceOf(face, mapped);
            const int atom = symbolAtomClass(mapped);
            box.headClass = atom;
            box.tailClass = atom;
            return box;
        }
        return measureText(face, text);
    }
    if (kind() == Kind::Sup || kind() == Kind::Sub) {
        const bool sub = kind() == Kind::Sub;
        take();
        Box arg = atScript([&] { return scriptRun(); });
        Box base = metricsOf();
        base.width = 0;
        return sub ? corners(base, &arg, nullptr) : corners(base, nullptr, &arg);
    }
    if (kind() != Kind::Word) {
        if (index < tokens.size()) take();
        Box box;
        box.empty = true;
        return box;
    }

    peelCommand();
    const std::string word = current().text;
    if (eqFold(word, "lim")) {
        take();
        const bool capital = !word.empty() && word[0] == 'L';
        const std::int32_t outer = em;
        Box box = atEm(mulDiv(em, 120, 100), [&] {
            return measureText(*roman, capital ? std::string_view("Lim") : std::string_view("lim"));
        });
        box.limits = true;
        box.fontEm = outer;
        return box;
    }
    if (eqFold(word, "rm")) {
        take();
        romanMode = true;
        return styleSwitch();
    }
    if (eqFold(word, "it")) {
        take();
        romanMode = false;
        return styleSwitch();
    }
    if (eqFold(word, "bold")) {
        take();
        Box box;
        box.empty = true;
        return box;
    }
    if (const Accent* accent = accentOf(word)) {
        take();
        return this->accent(bracedArg(), *accent);
    }
    if (eqFold(word, "sqrt")) {
        take();
        Box degree;
        if (pendingRoot) {
            degree = *pendingRoot;
            pendingRoot.reset();
        }
        return radical(radicalArg(), degree);
    }
    if (eqFold(word, "root")) {
        take();
        if (rootHasOf()) {
            Box degree = atEm(mulDiv(em, 50, 100), [&] { return bracedArg(); });
            takeNamed("of");
            return radical(radicalArg(), degree);
        }
        Box degree;
        if (pendingRoot) {
            degree = *pendingRoot;
            pendingRoot.reset();
        }
        return radical(radicalArg(), degree);
    }
    if (eqFold(word, "box")) {
        take();
        Box inner = bracedArg();
        const std::int32_t padX = std::max(em / 6, 1);
        const std::int32_t padY = std::max(em / 8, 1);
        inner.width += padX * 2;
        inner.height += padY * 2;
        inner.baseline += padY;
        inner.empty = false;
        return inner;
    }
    if (eqFold(word, "not")) {
        take();
        Box arg = bracedArg();
        Box slash = measureText(*symbol, "/");
        Box out = arg;
        out.width = std::max(arg.width, slash.width);
        out.height = std::max(arg.height, slash.height);
        return out;
    }
    if (eqFold(word, "bigg")) {
        take();
        Box side = atEm(em * 2, [&] { return takeDelim(false); });
        side.stretch = false;
        side.height = em * 2;
        side.baseline = side.height / 2;
        return side;
    }
    if (eqFold(word, "left") || eqFold(word, "right")) {
        const bool open = eqFold(word, "left");
        take();
        return takeDelim(open);
    }
    if (eqFold(word, "binom")) {
        take();
        const Box upper = bracedArg();
        const Box lower = bracedArg();
        return wrapDelim(fraction(upper, lower, false), "(", ")");
    }
    if (eqFold(word, "rel")) {
        take();
        const Box arrow = bracedArg();
        const Box over = atScript([&] { return bracedArg(); });
        const Box under = atScript([&] { return bracedArg(); });
        return limitsOf(arrow, &under, &over);
    }
    if (eqFold(word, "buildrel")) {
        take();
        const Box arrow = bracedArg();
        const Box over = atScript([&] { return bracedArg(); });
        return limitsOf(arrow, nullptr, &over);
    }
    if (eqFold(word, "color")) {
        take();
        skipSpace();
        if (kind() == Kind::LBrace) readGroup();
        else if (kind() == Kind::Word || kind() == Kind::Number || kind() == Kind::Symbol) take();
        return bracedArg();
    }
    if (eqFold(word, "scale")) {
        take();
        skipSpace();
        std::int32_t percent = 100;
        if (kind() == Kind::Number) {
            percent = 0;
            for (char ch : current().text) {
                if (ch < '0' || ch > '9') break;
                percent = percent * 10 + (ch - '0');
            }
            take();
        }
        if (percent < 1) percent = 1;
        return atEm(em * percent / 100, [&] { return bracedArg(); });
    }
    if (eqFold(word, "sup") || eqFold(word, "sub")) {
        const bool sub = eqFold(word, "sub");
        take();
        Box arg = atScript([&] { return bracedArg(); });
        Box base = metricsOf();
        base.width = 0;
        return sub ? corners(base, &arg, nullptr) : corners(base, nullptr, &arg);
    }
    if (eqFold(word, "lsub") || eqFold(word, "lsup")) {
        const bool sub = eqFold(word, "lsub");
        take();
        Box script = atScript([&] { return bracedArg(); });
        script.prescript = true;
        script.prescriptSub = sub;
        script.empty = false;
        return script;
    }
    if (const Big* op = bigOp(word)) {
        take();
        return bigOperator(*op);
    }
    if (wordIs("cases") || wordIs("pile") || wordIs("lpile") || wordIs("rpile") || wordIs("eqalign") ||
        wordIs("matrix") || wordIs("pmatrix") || wordIs("bmatrix") || wordIs("dmatrix")) {
        const std::string name = word;
        take();
        skipSpace();
        const bool savedMatrix = matrixRows;
        const bool savedAlign = eqAlignRows;
        matrixRows = eqFold(name, "matrix") || eqFold(name, "pmatrix") || eqFold(name, "bmatrix") ||
                     eqFold(name, "dmatrix") || eqFold(name, "cases") || eqFold(name, "pile") ||
                     eqFold(name, "lpile") || eqFold(name, "rpile");
        eqAlignRows = eqFold(name, "eqalign");
        Box inner;
        if (kind() == Kind::LBrace) inner = readGroup();
        else inner = bracedArg();
        matrixRows = savedMatrix;
        eqAlignRows = savedAlign;
        if (eqFold(name, "cases")) {
            // f(x)= {cases ...} 의 = 뒤는 관계 간격이다. 11pt 는 231이다.
            Box box = addFences(inner, 0xE079, 0);
            box.headClass = 1;
            return box;
        }
        if (eqFold(name, "pmatrix")) return addFences(inner, 0xE07D, 0xE080);
        if (eqFold(name, "bmatrix")) return addFences(inner, 0xE101, 0xE105);
        if (eqFold(name, "dmatrix")) return addFences(inner, 0xE101, 0xE101);
        return inner;
    }
    if (wordIs("from") || wordIs("to")) {
        take();
        Box box;
        box.empty = true;
        return box;
    }
    if (const char32_t cp = symbolCode(word)) {
        take();
        Box box = metricsOf();
        box.width = advanceOf(*symbol, cp);
        // INF, 그리스 문자는 보통 글자다. = INF 는 11pt 에서 2106이다.
        // LEQ 는 관계, CIRC 는 이항이다. CDOTS 같은 점은 간격을 더하지 않는다.
        const int atom = symbolAtomClass(cp);
        box.headClass = atom;
        box.tailClass = atom;
        return box;
    }
    // lex 는 <= 와 x, gex 는 >= 와 x 다. 11pt 는 둘 다 1881이다.
    if (word.size() > 2 && (eqFold(std::string_view(word).substr(0, 2), "le") ||
                            eqFold(std::string_view(word).substr(0, 2), "ge"))) {
        const bool ge = word[0] == 'g' || word[0] == 'G';
        take();
        Box rel = metricsOf();
        rel.width = symbolAdvance(ge ? 0x2265 : 0x2264);
        rel.headClass = 4;
        rel.tailClass = 4;
        Box rest = emitWord(std::string_view(word).substr(2));
        return hpack({rel, rest});
    }
    take();
    return emitWord(word);
}

Box Parser::piece() {
    if (rootIndexAhead()) return piece();
    Box base = prefix();
    Box sub;
    Box sup;
    bool hasSub = false;
    bool hasSup = false;
    int guard = 0;
    while (guard++ < 8) {
        if (kind() == Kind::Space) {
            std::size_t i = index;
            while (i < tokens.size() && tokens[i].kind == Kind::Space) ++i;
            if (i < tokens.size() && (tokens[i].kind == Kind::Sup || tokens[i].kind == Kind::Sub)) {
                index = i;
            } else {
                break;
            }
        }
        if (kind() != Kind::Sup && kind() != Kind::Sub) break;
        if (kind() == Kind::Sup && rootIndexAhead()) break;
        if (scriptSide > 0 && kind() == Kind::Sub) break;
        if (scriptSide < 0 && kind() == Kind::Sup) break;
        const bool isSub = kind() == Kind::Sub;
        take();
        const int savedSide = scriptSide;
        scriptSide = isSub ? -1 : 1;
        Box arg = atScript([&] { return scriptRun(); });
        scriptSide = savedSide;
        if (isSub) {
            sub = std::move(arg);
            hasSub = true;
        } else {
            sup = std::move(arg);
            hasSup = true;
        }
    }
    // 닫는 울타리의 위첨자는 울타리가 본문 높이가 된 뒤에 붙는다.
    // LEFT ( ... RIGHT ) ^{4x} 의 4x 는 글자 높이가 아니라 늘어난 괄호 위에 있다.
    if (base.stretch && !base.open && hasSup && !hasSub) {
        heldScripts.push_back(std::move(sup));
        base.heldSup = static_cast<int>(heldScripts.size()) - 1;
        return base;
    }
    if (base.intOp) {
        Box stacked = intStack(base, hasSub ? &sub : nullptr, hasSup ? &sup : nullptr);
        // 적분 뒤의 빈 칸은 파일 너비에 넣지 않는다. 내용이 있는 { } 만 그 너비를 더한다.
        const std::size_t mark = index;
        skipSpace();
        if (kind() == Kind::LBrace && !groupBeforeInfix()) {
            Box slot = readGroup();
            if (!slot.empty && slot.height > 0) {
                stacked.width += slot.width;
                const std::int32_t top = std::min(stacked.axisBottom, slot.axisBottom);
                const std::int32_t bot =
                    std::max(stacked.axisBottom + stacked.height, slot.axisBottom + slot.height);
                stacked.axisBottom = top;
                stacked.height = bot - top;
                const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - stacked.axisBottom;
                stacked.baseline =
                    std::max<std::int32_t>(1, std::min(fromTop, std::max(stacked.height, 1)));
            }
        } else {
            index = mark;
        }
        return stacked;
    }
    if (base.sumOp) return sumStack(base, hasSub ? &sub : nullptr, hasSup ? &sup : nullptr);
    if (base.limits) {
        Box slot;
        const Box* slotPtr = nullptr;
        skipSpace();
        // {a} over {b} 의 묶음은 분모 앞의 분자다. = 앞에 온 묶음만 lim 오른쪽 칸으로 들어간다.
        if (kind() == Kind::LBrace && !groupBeforeInfix()) {
            slot = readGroup();
            slotPtr = &slot;
        }
        return limBox(base, hasSub ? &sub : nullptr, hasSup ? &sup : nullptr, slotPtr);
    }
    if (!hasSub && !hasSup) return base;
    return corners(base, hasSub ? &sub : nullptr, hasSup ? &sup : nullptr);
}

Box Parser::term() {
    // 중괄호 묶음은 그 자체로 한 항이다. ={pi} over {2} 의 분자는 pi 이고 등호는 밖에 남는다.
    // 중괄호가 없으면 a+b over c 처럼 공백 전까지가 분자다.
    if (kind() == Kind::LBrace) return piece();
    std::vector<Box> parts;
    int guard = 0;
    while (guard++ < 64 && !stopped() && kind() != Kind::Space && kind() != Kind::LBrace && !infix()) {
        const std::size_t before = index;
        Box box = piece();
        if (!box.empty) parts.push_back(std::move(box));
        if (index == before) break;
    }
    return hpack(parts);
}

Box Parser::expr() {
    struct Item {
        bool op = false;
        std::string name;
        Box box;
    };
    std::vector<Item> items;
    int guard = 0;
    while (guard++ < 256) {
        // x sin x 의 함수 앞 공백은 얇은 빈칸이다. = sin 처럼 간격이 이미 있으면 더하지 않는다.
        const bool sawSpace = kind() == Kind::Space;
        skipSpace();
        // dy}overdx 의 짝 없는 } 는 없는 셈이다. 묶음 안의 } 는 그 묶음을 닫는다.
        if (braceDepth == 0 && kind() == Kind::RBrace) {
            take();
            continue;
        }
        if (stopped()) break;
        if (infix()) {
            items.push_back(Item{true, current().text, {}});
            take();
            continue;
        }
        const std::size_t before = index;
        Box box = term();
        // x cos 는 137을 더한다. pi^2 cos 와 | ln 은 더하지 않는다.
        if (sawSpace && box.fnHead && !box.prescript && !items.empty() && !items.back().op &&
            !items.back().box.scripted && items.back().box.tailClass == 1 &&
            atomGap(items.back().box, box) == 0) {
            box.width += equationBlank(em) / 4;
        }
        if (box.prescript && !items.empty() && !items.back().op) {
            items.back().box = attachPrescript(items.back().box, box, box.prescriptSub);
        } else if (!box.empty) {
            items.push_back(Item{false, {}, std::move(box)});
        }
        if (index == before) break;
    }
    std::size_t i = 0;
    while (i < items.size()) {
        if (!items[i].op) {
            ++i;
            continue;
        }
        if (i == 0 || i + 1 >= items.size() || items[i + 1].op) {
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        Box merged = eqFold(items[i].name, "choose")
                         ? addFences(packTable({{items[i - 1].box}, {items[i + 1].box}}, true), 0xE07D, 0xE080)
                         : fraction(items[i - 1].box, items[i + 1].box, !eqFold(items[i].name, "atop"));
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(i - 1),
                    items.begin() + static_cast<std::ptrdiff_t>(i + 2));
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(i - 1), Item{false, {}, std::move(merged)});
        i = 0;
    }
    std::vector<Box> boxes;
    for (const Item& item : items) {
        if (!item.op && !item.box.empty) boxes.push_back(item.box);
    }
    stretch(boxes, 0, boxes.size());
    for (Box& box : boxes) {
        if (box.heldSup < 0 || box.heldSup >= static_cast<int>(heldScripts.size())) continue;
        const Box& sup = heldScripts[static_cast<std::size_t>(box.heldSup)];
        box = corners(box, nullptr, &sup);
    }
    return hpack(boxes);
}

Box Parser::lines() {
    std::vector<std::vector<Box>> rows;
    std::vector<Box> row;
    row.push_back(expr());
    while (kind() == Kind::Amp) {
        take();
        row.push_back(expr());
    }
    rows.push_back(std::move(row));
    while (kind() == Kind::Hash) {
        take();
        std::vector<Box> next;
        next.push_back(expr());
        while (kind() == Kind::Amp) {
            take();
            next.push_back(expr());
        }
        rows.push_back(std::move(next));
    }
    return finishRows(rows);
}

std::int32_t baselinePercent(std::int32_t baseline, std::int32_t height) {
    if (height <= 0) return 85;
    std::int32_t percent = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(baseline) * 100 + height / 2) / height);
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    return percent;
}

// 글자·숫자가 그대로 한 상자로 끝났을 때의 저장 퍼센트.
// 위에서 기준선까지를 높이로 나누고 버린 뒤 1을 더한다.
// 10pt 로만(1000)은 850/1000 이 딱 85라 반올림으로는 85가 되지만, 저장값은 86이다.
// 7pt 맨 글자(675)는 86에 1을 더해 87이다.
std::int32_t letterBaselinePercent(std::int32_t baseline, std::int32_t height) {
    if (height <= 0) return 85;
    std::int32_t percent = static_cast<std::int32_t>(static_cast<std::int64_t>(baseline) * 100 / height) + 1;
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    return percent;
}

EquationSize finishMeasure(Parser& parser) {
    Box box = parser.lines();
    if (box.empty || box.height <= 0) {
        box = parser.metricsOf();
        box.width = 0;
    }
    EquationSize size;
    size.width = std::max(box.width, 0);
    size.height = std::max(box.height, 1);
    // 글자 상자가 그대로면 버린 뒤 1을 더한다.
    // 위첨자만, 아래첨자만, 분수도 그 몫에 1을 더한다. 기준선은 글자 크기 35%에서 측정 기준선을 뺀 자리다.
    const std::int32_t letterTop = (parser.em > 0 ? parser.em * 35 / 100 : 0) + size.height / 2;
    const std::int32_t letterBaseline = std::max<std::int32_t>(1, std::min(letterTop, size.height));
    size.baseLine = box.baseline == letterBaseline || box.quotBaseline
                        ? letterBaselinePercent(box.baseline, size.height)
                        : baselinePercent(box.baseline, size.height);
    return size;
}

std::shared_ptr<const EquationFont> cachedEquationFont(const std::string& face) {
    static const std::shared_ptr<const EquationFont> empty = std::make_shared<EquationFont>();
    if (face.empty()) return empty;
    static std::mutex gate;
    static std::unordered_map<std::string, std::shared_ptr<EquationFont>> fonts;
    std::lock_guard lock(gate);
    if (const auto found = fonts.find(face); found != fonts.end()) return found->second;
    auto font = std::make_shared<EquationFont>();
    std::uint16_t units = 0;
    std::int32_t xAvg = 0;
    std::int32_t cell = 0;
    auto sink = [](void* ctx, char32_t cp, std::int32_t advance) {
        static_cast<EquationFont*>(ctx)->advanceWidth.emplace(cp, advance);
    };
    if (!chwp::layout::fontCopyDesignAdvances(face, units, xAvg, cell, sink, font.get())) return empty;
    font->unitsPerEm = units;
    font->xAvgCharWidth = xAvg;
    font->winCell = cell;
    fonts.emplace(face, font);
    return font;
}

}  // namespace

EquationSize measureHangulEquation(std::string_view script, std::int32_t baseUnit, const EquationFont& symbol,
                                   const EquationFont& roman) {
    if (baseUnit <= 0) baseUnit = 1000;
    const bool distinct = roman.unitsPerEm > 0 && &roman != &symbol;
    const EquationFont& romanUse = distinct ? roman : symbol;
    Parser parser(script, baseUnit, symbol, romanUse, distinct, false);
    return finishMeasure(parser);
}

EquationSize measureHangulEquation(std::string_view script, std::int32_t baseUnit, std::string_view symbolFont) {
    if (baseUnit <= 0) baseUnit = 1000;
    const std::string symbolName = resolveFace({canonicalSymbol(symbolFont), "HyhwpEQ", "HancomEQN"});
    const std::string romanName =
        resolveFace({"Haansoft Batang", "한컴바탕", "HCR Batang", "함초롬바탕", "바탕", "Batang"});
    const bool alias = romanName.empty() || romanName == symbolName;
    const std::shared_ptr<const EquationFont> symbol = cachedEquationFont(symbolName);
    const std::shared_ptr<const EquationFont> roman = alias ? symbol : cachedEquationFont(romanName);
    Parser parser(script, baseUnit, *symbol, *roman, !alias, alias);
    return finishMeasure(parser);
}

}  // namespace chwp::render
