#include "render/equation_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
#include "export/layout.hpp"

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
// 배율(lfWidth/xAvg, 16.16)은 반올림한다. 진행폭을 곱하고 0x8000을 더한 뒤 12비트 내리고, 픽셀 아래는 버린다.
// 함초롬바탕(xAvg 933) 10pt 一(1000, lfWidth 7)은 배율을 버리면 7.49 라 7 이지만 GDI 는 8 이다(배율 492, 7.507).
// xAvg 512 인 수식 글꼴·한컴바탕은 나누어떨어져 같다(GDI 정사각이 아닌 크기 2만여 칸, 세 글꼴 모두 같다).
std::int32_t equationFastAdvancePx(std::int32_t design, std::int32_t lfWidth, std::int32_t xAvg) {
    if (design <= 0 || lfWidth <= 0 || xAvg <= 0) return 0;
    const std::int64_t scale = ((static_cast<std::int64_t>(lfWidth) << 16) + xAvg / 2) / xAvg;
    std::int64_t rounded = scale * design + 0x8000;
    rounded >>= 12;
    rounded &= ~std::int64_t{0xF};
    return static_cast<std::int32_t>(rounded >> 4);
}

// 세로 장치 픽셀이 1이면 드라이버가 2로 올리고 가로는 같은 비로 늘린다.
// 글자 크기가 1픽셀보다 작으면 그 올림을 하지 않는다. 1pt 본문 100은 올리고, 첨자 68은 가로 1이다.
// 그 뒤 두 배율이 같으면 픽셀 격자, 다르면 선형 곱이다.
// 같을 때 GDI 는 힌팅 명령을 돌리고 내장 비트맵이 있으면 그 폭을 쓴다. 12pt HancomEQN ) 는 식으로 6이지만 7이다.
// 다를 때(홀수 높이 등)는 힌팅이 폭을 바꾸지 않는다.
std::int32_t equationGlyphPx(const EquationFont& font, char32_t cp, std::int32_t design, std::int32_t em) {
    const std::int32_t xAvg = font.xAvgCharWidth;
    const std::int32_t cell = font.winCell;
    const std::int32_t upem = font.unitsPerEm;
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
    if (!isotropic) return equationFastAdvancePx(design, width, avg);
    if (!font.face.empty() && cp != 0 && denom > 0) {
        const auto ppem = static_cast<std::int32_t>(static_cast<std::int64_t>(height) * upem / denom);
        if (const std::int32_t hinted = chwp::layout::fontGridAdvancePx(font.face, cp, ppem); hinted >= 0) return hinted;
    }
    return equationSideBearingPx(design, width, avg);
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

// 그 글꼴을 lfHeight 로 만든 GDI 글꼴의 tmHeight + tmInternalLeading(픽셀). 한글은 이 합을 글자 상자 높이로 쓴다.
// 배율은 round(lfHeight·65536 / (usWinAscent + usWinDescent))이고, 위·아래·em 을 그 배율로 각각 반올림한다.
// tmHeight 는 위 + 아래, 내부 여백은 tmHeight − em 이다. 1픽셀은 2로 만든다(equationHeightPx).
// 함초롬바탕 10pt(13px)는 11 + 2 에 여백 3 이라 16, 12pt(16px)는 20 이다. HYhwpEQ 128px 는 103 + 26 에 1 이라 130 이다.
// (GDI GetTextMetricsW, 함초롬바탕·한컴바탕·HYhwpEQ 1pt~200pt 모두 같다.) 수식 글꼴에서는 equationHeightPx 와 같다.
std::int32_t equationFontBoxPx(const EquationFont& font, std::int32_t em) {
    const std::int32_t cell = font.winAscent + font.winDescent;
    if (cell <= 0 || font.unitsPerEm <= 0) return equationHeightPx(em);
    std::int32_t height = equationFontHeightPx(em);
    if (height == 1) height = 2;
    const std::int64_t scale = ((static_cast<std::int64_t>(height) << 16) + cell / 2) / cell;
    const auto scaled = [&](std::int64_t units) { return static_cast<std::int32_t>((units * scale + 0x8000) >> 16); };
    const std::int32_t tmHeight = scaled(font.winAscent) + scaled(font.winDescent);
    return tmHeight + (tmHeight - scaled(font.unitsPerEm));
}

// rm 은 같은 수식 폰트의 ASCII, 그 외 문자는 PUA, 숫자는 rm 이어도 PUA다.
char32_t equationGlyph(char32_t cp, bool roman) {
    if (cp >= U'0' && cp <= U'9') return cp == U'0' ? 0xE03D : 0xE034 + (cp - U'1');
    if (cp >= U'a' && cp <= U'z') return roman ? cp : static_cast<char32_t>(0xE0E5 + (cp - U'a'));
    if (cp >= U'A' && cp <= U'Z') return roman ? cp : static_cast<char32_t>(0xE000 + (cp - U'A'));
    // 그리스 문자와 DEG(U+2057)는 rm 이어도 수식 글꼴 코드다. 10pt α 는 525, Ω 는 750 이다.
    struct Range {
        char32_t first;
        char32_t last;
        char32_t code;
    };
    static constexpr Range ranges[] = {
        {0x0391, 0x03A1, 0xE085}, {0x03A3, 0x03A9, 0xE096}, {0x03B1, 0x03C1, 0xE09D},
        {0x03C3, 0x03C9, 0xE0AE}, {0x2057, 0x2057, 0xE0C8},
    };
    for (const Range& range : ranges) {
        if (cp >= range.first && cp <= range.last) return range.code + (cp - range.first);
    }
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
    case U'%': return 0xE042;
    // 한글 수식 글꼴이 ASCII 자리에 둔 나머지 기호다. 10pt ? 와 $ 는 525, @ 는 825 다.
    case U'@': return 0xE03F;
    case U'$': return 0xE041;
    case U'?': return 0xE057;
    // { } 는 묶음이라 LBRACE·RBRACE 로만 온다.
    case U'{': return 0xE04B;
    case U'}': return 0xE04C;
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
    // 프라임은 ′·″ 로 잰다. HYhwpEQ 에 없으면 HancomEQN 폭이다(300, 450).
    if (text == "'") return 0x2032;
    if (text == "''") return 0x2033;
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

struct GlyphClass {
    char32_t code;
    int atom;
};

#include "equation_classes.inc"

// 이름으로 부르거나 바로 넣은 기호의 간격 종류. 수식 글꼴 코드로 글자 정보 표를 찾고,
// 없으면 그 코드의 ASCII 글자로 정한다(LANGLE 은 < 라 관계, LPAREN 은 ( 라 여는 울타리). 그 밖은 보통이다.
// 10pt a CDOTS a 는 앞뒤에 관계 간격 210, a PROPTO a 는 간격이 없다.
int symbolAtomClass(char32_t cp) {
    char32_t code = equationGlyph(cp, false);
    if (code == 0) code = equationOperator(cp);
    if (code == 0) code = cp;
    for (const GlyphClass& entry : kGlyphClasses) {
        if (entry.code == code) return entry.atom;
    }
    for (char32_t ascii = 0x21; ascii < 0x7F; ++ascii) {
        if (equationOperator(ascii) == code) return equationAtomClass(ascii);
    }
    return 1;
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
        const char* ops[] = {"<->", "->", "<-", ">>>", "<<<", "<<", ">>", "!=", "==", "<=", ">=", "+-", "-+", "''"};
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

struct Keyword {
    const char* name;
    std::uint32_t value;
};

#include "equation_keywords.inc"

struct KeywordHit {
    int length = 0;
    std::uint32_t value = 0;
};

// 위 비트가 0·1 이면 기호(글자 하나), 3 이면 함수다.
bool keywordIsSymbol(std::uint32_t value) { return (value >> 17) == 0; }
bool keywordIsFunction(std::uint32_t value) { return (value >> 16) == 3; }

template <std::size_t N>
KeywordHit longestKeyword(const Keyword (&table)[N], std::string_view text) {
    KeywordHit hit;
    for (const Keyword& keyword : table) {
        const std::string_view name(keyword.name);
        if (static_cast<int>(name.size()) <= hit.length || name.size() > text.size()) continue;
        if (text.compare(0, name.size(), name) == 0) hit = {static_cast<int>(name.size()), keyword.value};
    }
    return hit;
}

// 글자 덩어리 맨 앞에 있는 낱말. 이름이 앞부분과 같으면 맞고, 더 긴 이름이 이긴다.
// 먼저 대소문자를 가리는 표를 본다(alpha, vert, REAL, IMAG). 없으면 대문자 표를 본다:
// 첫 두 글자가 대문자면 그대로, 두 번째가 소문자면 첫 글자와 이어지는 소문자(모두 10자까지)를 대문자로 바꿔 찾는다.
// Alpha 는 ALPHA, dagger 는 DAGGER 다. aLPHA 와 Prime 은 낱말이 아니다. FROM·TO 는 낱말 전체가 같아야 한다(tox 는 글자).
KeywordHit keywordAt(std::string_view text) {
    if (const KeywordHit hit = longestKeyword(kKeywordsExact, text); hit.length > 0) return hit;
    if (text.size() < 2) return {};
    const auto isUpper = [](char c) { return c >= 'A' && c <= 'Z'; };
    const auto isLower = [](char c) { return c >= 'a' && c <= 'z'; };
    std::string key;
    if (isUpper(text[0]) && isUpper(text[1])) {
        key = std::string(text);
    } else if ((isUpper(text[0]) || isLower(text[0])) && isLower(text[1])) {
        key += static_cast<char>(isLower(text[0]) ? text[0] - 'a' + 'A' : text[0]);
        for (std::size_t i = 1; i < text.size() && key.size() < 10 && isLower(text[i]); ++i) {
            key += static_cast<char>(text[i] - 'a' + 'A');
        }
    } else {
        return {};
    }
    const KeywordHit hit = longestKeyword(kKeywordsUpper, key);
    if ((hit.value == 0x800006 || hit.value == 0x800007) && hit.length != static_cast<int>(key.size())) return {};
    return hit;
}

// 낱말 전체가 기호 이름이면 그 글자 코드.
char32_t symbolCode(std::string_view word) {
    const KeywordHit hit = keywordAt(word);
    if (hit.length != static_cast<int>(word.size()) || !keywordIsSymbol(hit.value)) return 0;
    return hit.value & 0xFFFF;
}

// 낱말 표의 함수 이름(종류 3)이 낱말 앞부분이면 그 길이. 대소문자를 가리고 더 긴 이름이 이긴다
// (sin, sinh, Exp, EXP, Ln, def). "or " 는 뒤에 띄어쓰기가 있어야 함수다(a or b 는 함수, a orb 는 글자).
// 이런 함수는 띄어 쓴 낱말 덩어리를 끊고 그 뒤에서 다시 낱말을 찾는다(tanalpha 는 tan, alpha).
int tableFunctionPrefix(std::string_view word, bool spaceAfter = false) {
    if (spaceAfter) {
        const KeywordHit spaced = keywordAt(std::string(word) + ' ');
        if (keywordIsFunction(spaced.value) && spaced.length == static_cast<int>(word.size()) + 1) {
            return static_cast<int>(word.size());
        }
    }
    const KeywordHit hit = keywordAt(word);
    return keywordIsFunction(hit.value) ? hit.length : 0;
}

// 곧게 선 ASCII 로 쓰는 함수 이름의 길이. 단어 맨 앞에서만 함수로 본다. xsinx 의 sin 은 기울임으로 남는다.
// 낱말 표 함수에 det, arc 를 더한다. det, arc 는 낱말이 아니라서 덩어리를 끊지 않고 그 뒤에서 낱말을 찾지 않는다
// (한글 2024: det 1500 은 곧은 글자, x det 와 a det 는 모두 기울임, detalpha 의 alpha 는 글자).
// loose 가 거짓이면(띄어 쓴 글자 낱말 뒤) 낱말 표 함수만 본다.
int uprightPrefix(std::string_view word, bool spaceAfter = false, bool loose = true) {
    int best = tableFunctionPrefix(word, spaceAfter);
    if (!loose) return best;
    for (const std::string_view name : {std::string_view("det"), std::string_view("arc")}) {
        if (static_cast<int>(name.size()) > best && word.substr(0, name.size()) == name) {
            best = static_cast<int>(name.size());
        }
    }
    return best;
}

constexpr std::uint32_t kFrom = 0x800006;
constexpr std::uint32_t kTo = 0x800007;
constexpr std::uint32_t kAbove = 0x800023;

// 낱말 전체가 표의 이름일 때만 그 값이다. 앞부분만 같으면 0 이다.
std::uint32_t wholeKeyword(std::string_view word) {
    const KeywordHit hit = keywordAt(word);
    return hit.length == static_cast<int>(word.size()) ? hit.value : 0;
}

// 낱말 표에서 값이 같은 명령은 같은 명령이다. COL·CCOL·CPILE 은 PILE, LCOL 은 LPILE, RCOL 은 RPILE,
// INTEGRAL 은 INT, BIGCAP·BIGCUP 은 INTER·UNION 이다(PILE 은 0x810015, COL 은 0x800015 라 종류 맨 아래 비트는 보지 않는다).
// 그 이름들을 우리가 다루는 이름으로 바꾼다.
std::string_view commandAlias(std::string_view word) {
    static constexpr std::string_view kNamed[] = {"pile", "lpile", "rpile", "int", "inter", "union"};
    const std::uint32_t value = wholeKeyword(word);
    if (value == 0 || keywordIsSymbol(value) || keywordIsFunction(value)) return word;
    for (const std::string_view name : kNamed) {
        const std::uint32_t own = wholeKeyword(name);
        if ((own >> 17) == (value >> 17) && (own & 0xFFFF) == (value & 0xFFFF)) return name;
    }
    return word;
}

struct Big {
    std::string_view name;
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

std::optional<Big> bigOp(std::string_view word) {
    static const Big table[] = {
        {"smallsum", 0x2211, true},   {"smallprod", 0x220F, true},  {"smallcoprod", 0x2210, true},
        {"smallunion", 0x222A, true}, {"smallinter", 0x2229, true}, {"int", 0x222B, false},
        {"oint", 0x222E, false},      {"dint", 0x222C, false},      {"tint", 0x222D, false},
        {"odint", 0x222F, false},     {"otint", 0x2230, false},     {"sum", 0x2211, false},
        {"prod", 0x220F, false},      {"coprod", 0x2210, false},    {"union", 0x22C3, false},
        {"inter", 0x22C2, false},
    };
    for (const Big& op : table) {
        if (eqFold(word, op.name)) return op;
    }
    // 낱말 표의 큰 연산자(종류 0xE0) 가운데 사용자 영역 기호가 없는 것(BIGOPLUS, BIGWEDGE …)은 그 글자 코드로 잰다.
    // 한글 2024는 underover 노드 안의 그 글자(BIGOPLUS 는 U+2295)이고, 크기는 SUM 과 같은 규칙이다.
    const std::uint32_t value = wholeKeyword(word);
    if ((value >> 16) == 0xE0) return Big{word, static_cast<char32_t>(value & 0xFFFF), false};
    return std::nullopt;
}

struct Accent {
    char32_t cp = 0;
    bool under = false;
    // bar dyad under 는 글자 너비와 기호 너비 중 큰 값에 기호 너비의 20%를 더한다(vec 은 padContent).
    bool widen = false;
    // 그릴 때의 뜻. bar·vec·dyad 는 같은 코드로 재지만 그림은 선, 화살표, 양쪽 화살표다.
    char32_t meaning = 0;
    // vec 만 20% 를 글자 너비에 더한 뒤 기호 너비와 큰 값을 고른다(한글 2024, 10·14pt:
    // vec{i} 525, vec{b} 555, vec{1} 630. 같은 글자의 bar 는 630, 630, 630).
    bool padContent = false;
};

const Accent* accentOf(std::string_view word) {
    static const Accent hat{0xE063, false, false, 0x02C6};
    static const Accent check{0xE064, false, false, 0x02C7};
    static const Accent arch{0xE065, false, false, 0x2312};
    static const Accent tilde{0xE066, false, false, 0x02DC};
    static const Accent acute{0xE05D, false, false, 0x00B4};
    static const Accent grave{0xE05E, false, false, 0x0060};
    static const Accent dot{0xE05F, false, false, 0x02D9};
    static const Accent ddot{0xE060, false, false, 0x00A8};
    static const Accent bar{0xE06D, false, true, 0x203E};
    static const Accent vec{0xE06D, false, true, 0x2192, true};
    static const Accent dyad{0xE06D, false, true, 0x2194};
    static const Accent under{0xE06D, true, true, 0x203E};
    if (eqFold(word, "hat")) return &hat;
    if (eqFold(word, "check")) return &check;
    if (eqFold(word, "arch")) return &arch;
    if (eqFold(word, "tilde")) return &tilde;
    if (eqFold(word, "acute")) return &acute;
    if (eqFold(word, "grave")) return &grave;
    if (eqFold(word, "dot")) return &dot;
    if (eqFold(word, "ddot")) return &ddot;
    if (eqFold(word, "bar") || eqFold(word, "overline")) return &bar;
    if (eqFold(word, "vec")) return &vec;
    if (eqFold(word, "dyad")) return &dyad;
    if (eqFold(word, "under") || eqFold(word, "underline")) return &under;
    return nullptr;
}

// 그릴 때의 뜻 글자. 프라임은 ' 폭으로 재지만 그림은 ′ 다(한글 2024가 U+2032).
char32_t drawnSymbol(char32_t cp) {
    return cp == 0x27 ? char32_t{0x2032} : cp;
}

// 울타리 글자의 뜻. 재는 코드(fencePiece)와 따로 둔다.
char32_t fenceMeaning(std::string_view text) {
    if (text.empty() || text == ".") return 0;
    if (text == "{" || eqFold(text, "lbrace")) return U'{';
    if (text == "}" || eqFold(text, "rbrace")) return U'}';
    std::size_t i = 0;
    char32_t cp = 0;
    nextCp(text, i, cp);
    return i == text.size() ? cp : 0;
}

// 위로 솟지 않은 글자. 한글이 bar 안을 볼 때 쓰는 목록이다: 기울인 a c e g i j m~z, 바른 a c g
// m~z, α~η, ι~ω, 마침표. 줄의 모든 글자가 이 목록이면 bar 윗줄을 덜 올린다.
bool lowGlyphCode(char32_t code) {
    switch (code) {
    case 0xE0E5: case 0xE0E7: case 0xE0E9: case 0xE0EB: case 0xE0ED: case 0xE0EE:
    case 0xE01A: case 0xE01C: case 0xE020: case 0xE053:
        return true;
    default:
        break;
    }
    return (code >= 0xE0F1 && code <= 0xE0FE) || (code >= 0xE026 && code <= 0xE033) ||
           (code >= 0xE09D && code <= 0xE0A3) || (code >= 0xE0A5 && code <= 0xE0B4);
}

bool isHangulText(char32_t cp) {
    return (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x1100 && cp <= 0x11FF) || (cp >= 0x3130 && cp <= 0x318F);
}

using Item = EquationItem;

// 수식 한 조각. baseline 은 상자 위에서 기준선까지.
// items 는 이 상자 왼쪽 위에서 잰 놓을 것이다. 조각을 합칠 때 크기를 잰 식 그대로 자리를 옮겨 붙인다.
struct Box {
    std::vector<Item> items;
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
    // 이름으로 부른 기호(dagger, INF, CDOTS)로 끝난다. 그 뒤 함수 앞 공백도 얇은 빈칸이다. 여는·닫는 울타리 종류는 아니다.
    bool wordTail = false;
    // 닫는 울타리의 위첨자는 울타리를 늘린 뒤에 붙인다. 늘어나기 전에는 -1이다.
    int heldSup = -1;
    // bigg 기호. 같은 줄의 다른 조각 높이로 늘어난다.
    bool middle = false;
    // 글자와 빈칸만으로 된 줄이고 글자가 모두 키 낮은 글자다(lowGlyphCode). bar 윗줄 높이를 정한다.
    bool lowGlyphs = false;

};

// child 의 놓을 것을 (dx, dy) 만큼 옮겨 out 에 붙인다.
void place(Box& out, const Box& child, std::int32_t dx, std::int32_t dy) {
    for (Item item : child.items) {
        item.x += dx;
        item.y += dy;
        out.items.push_back(item);
    }
}

// 마지막으로 놓인 것이 라틴 글자인지. 함수 앞 얇은 빈칸은 글자 뒤에만 선다.
bool endsWithLetter(const Box& box) {
    for (auto it = box.items.rbegin(); it != box.items.rend(); ++it) {
        if (it->kind != EquationItem::Kind::Glyph) continue;
        return (it->cp >= U'a' && it->cp <= U'z') || (it->cp >= U'A' && it->cp <= U'Z') ||
               (it->cp >= 0x0391 && it->cp <= 0x03C9);
    }
    return false;
}

// 위로 솟지 않는 낮은 글자만 늘어놓은 목록인지. 그 글자는 a c e g i j m~z, α~η, ι~ω(θ 빼고),
// 마침표다. 글꼴 전환(rm, it)은 건너뛰고, 다른 구조가 하나라도 있으면 아니다. 빈 목록은 맞다.
// rm 글자도 한글에서는 기울임 글자와 같은 코드라(rm a 는 U+E0E5) 글자로 본다.
bool lowLetters(const Box& box) {
    if (box.items.empty()) return box.empty || box.width == 0;
    for (const EquationItem& item : box.items) {
        if (item.kind != EquationItem::Kind::Glyph || item.em != box.items.front().em) return false;
        const char32_t c = item.cp;
        const bool latin = c == U'a' || c == U'c' || c == U'e' || c == U'g' || c == U'i' || c == U'j' ||
                           (c >= U'm' && c <= U'z');
        const bool greek = (c >= 0x03B1 && c <= 0x03B7) || (c >= 0x03B9 && c <= 0x03C1) || (c >= 0x03C3 && c <= 0x03C9);
        if (!latin && !greek && c != U'.') return false;
    }
    return true;
}

// 늘어난 울타리는 상자 높이만큼 그린다.
void refitFence(Box& box) {
    for (Item& item : box.items) {
        if (item.kind != Item::Kind::Fence) continue;
        item.y = 0;
        item.height = box.height;
    }
}

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

// 글자 덩어리 앞에서 잘라 내는 명령·기호 낱말의 길이. 함수 이름은 0 이다(uprightPrefix 가 맡는다).
// symbolsOnly 면 기호 낱말만 센다.
int leadingCommand(std::string_view word, bool symbolsOnly = false) {
    const KeywordHit hit = keywordAt(word);
    if (hit.length < 2 || keywordIsFunction(hit.value)) return 0;
    if (symbolsOnly && !keywordIsSymbol(hit.value)) return 0;
    return hit.length;
}

// 띄어 쓴 낱말들은 한 덩어리다. 그 덩어리 맨 앞이 함수 이름이면 공백을 넘어 그 이름을 잇는다
// (한글 2024: s i n, si n, c os, a si n, ar c, sin h 는 함수다).
// 명령·기호·함수로 시작하는 낱말은 새 덩어리를 연다(a sin 은 a 와 sin, a s in 은 as 와 ∈).
// 덩어리 맨 앞이 함수가 아니면 그대로 둔다(y s i n 은 모두 기울임).
bool keywordStart(const std::string& word) {
    return leadingCommand(word) >= 2 || tableFunctionPrefix(word) > 0 || symbolCode(word) != 0;
}

std::vector<Token> mergeSpacedFunctions(std::vector<Token> tokens) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < tokens.size()) {
        if (tokens[i].kind != Kind::Word) {
            out.push_back(std::move(tokens[i++]));
            continue;
        }
        // 덩어리: 낱말 (공백 낱말)* — 뒤 낱말은 명령·기호·함수로 시작하지 않는 것만.
        std::vector<std::size_t> run{i};
        std::size_t j = i + 1;
        while (j + 1 < tokens.size() && tokens[j].kind == Kind::Space && tokens[j + 1].kind == Kind::Word &&
               !keywordStart(tokens[j + 1].text)) {
            run.push_back(j + 1);
            j += 2;
        }
        std::string joined;
        for (std::size_t k : run) joined += tokens[k].text;
        const std::size_t name = static_cast<std::size_t>(uprightPrefix(joined));
        std::size_t covered = 0;
        std::size_t take = 0;
        if (name > tokens[i].text.size()) {
            // 함수 이름이 걸친 낱말까지 하나로 잇는다.
            while (take < run.size() && covered < name) covered += tokens[run[take++]].text.size();
        }
        if (take <= 1) {
            // 덩어리 맨 앞이 함수가 아니면 덩어리 전체를 그대로 둔다(x a n d 의 a n d 는 함수가 아니다).
            const std::size_t end = run.back() + 1;
            while (i < end) out.push_back(std::move(tokens[i++]));
            continue;
        }
        Token merged{Kind::Word, {}};
        for (std::size_t k = 0; k < take; ++k) merged.text += tokens[run[k]].text;
        out.push_back(std::move(merged));
        i = run[take - 1] + 1;
    }
    return out;
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
    // 다음 lines() 하나가 행렬·pile·cases 의 몸통이다. 그 안의 묶음(칸 안의 _{} 같은)은 표가 아니다.
    bool tableBody = false;
    // 중괄호 없는 위첨자를 재는 동안 아래첨자는 그 첨자가 아니라 본문으로 돌아간다. 같은 방향은 겹친다.
    int scriptSide = 0;
    // bold 와 color 는 폭을 바꾸지 않는다. 그 뒤(묶음 안) 놓을 것의 모양에만 붙는다.
    bool boldMode = false;
    std::uint32_t color = 0xFF000000u;
    // 표 칸 가로 맞춤. 0 가운데, -1 왼쪽, 1 오른쪽, 2 eqalign(첫 칸 오른쪽, 나머지 왼쪽).
    int tableAlign = 0;
    const EquationFont* symbol = nullptr;
    const EquationFont* roman = nullptr;
    // 수식 글꼴에 없는 글자를 대신 잴 글꼴. 글자의 언어마다 하나다.
    // 한글은 먼저 대체 표 [Subst Fonts - 언어]를 보고(수식 글꼴 줄은 HancomEQN → HyhwpEQ 하나),
    // 그 글꼴에도 없으면 그 언어의 기본 글꼴로 잰다.
    std::array<const EquationFont*, chwp::model::kLangCount> fallbacks{};
    const EquationFont* substitute = nullptr;
    std::optional<Box> pendingRoot;
    // 본문이 꼭 있어야 하는 명령(UNDEROVER, 중괄호 없는 PILE)에서 해석이 끝났다. 아무것도 못 만들었으면 수식은 1×1 이다
    // (한글 자동화로 만든 수식: underover 와 pile 은 1×1, 홀로 쓴 from 은 빈 목록 500×1000).
    bool aborted = false;
    // 맨 바깥에서 짝 없는 } 를 이미 하나 버렸다.
    bool strayClose = false;
    // NOT·REL 이 글자로 먹은 { 가 열린 묶음 깊이. 짝 맞추기에서는 여는 괄호라, 그 짝인 } 는 묶음을 닫지 않고
    // 짝 없는 } 로 다룬다(한글 2024: {not{=} b} over c 의 분자는 not{, =, b).
    std::vector<int> rawOpens;
    // 닫는 울타리에 붙은 위첨자. 본문 높이로 울타리를 늘린 다음에 다시 붙인다.
    struct HeldScripts {
        Box sub;
        Box sup;
        bool hasSub = false;
        bool hasSup = false;
    };
    std::vector<HeldScripts> heldScripts;

    Parser(std::string_view script, std::int32_t baseUnit, const EquationFont& symbolFont,
           const EquationFont& romanFont, bool distinctRoman, bool aliasRoman)
        : tokens(mergeSpacedFunctions(tokenize(script))),
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

    // 글자 덩어리를 왼쪽부터 본다. 낱말은 덩어리 맨 앞이나 방금 자른 낱말·함수 바로 뒤에서만 찾는다
    // (한글 2024: rmbarAP 는 rm, bar, AP 이고 tanalpha 는 tan, alpha 다. xalpha 와 sint 는 모두 글자다).
    void peelCommand() {
        if (kind() != Kind::Word) return;
        const std::string text = current().text;
        std::vector<std::string> parts;
        std::size_t i = 0;
        while (i < text.size()) {
            const std::string_view rest = std::string_view(text).substr(i);
            const int n = leadingCommand(rest);
            if (n >= 2) {
                parts.emplace_back(rest.substr(0, static_cast<std::size_t>(n)));
                i += static_cast<std::size_t>(n);
                continue;
            }
            const auto fn = static_cast<std::size_t>(tableFunctionPrefix(rest));
            if (fn > 0 && fn < rest.size() && leadingCommand(rest.substr(fn)) >= 2) {
                parts.emplace_back(rest.substr(0, fn));
                i += fn;
                continue;
            }
            parts.emplace_back(rest);
            break;
        }
        // ABOVE 는 # 과 같은 줄 나눔이다(한글 2024: a above b 는 a # b 와 같은 두 줄 목록).
        const auto kindOf = [](const std::string& part) { return wholeKeyword(part) == kAbove ? Kind::Hash : Kind::Word; };
        if (parts.size() <= 1) {
            tokens[index].kind = kindOf(text);
            return;
        }
        tokens[index].text = std::move(parts[0]);
        tokens[index].kind = kindOf(tokens[index].text);
        for (std::size_t k = parts.size(); k-- > 1;) {
            const Kind partKind = kindOf(parts[k]);
            tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(index + 1), Token{partKind, std::move(parts[k])});
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

    // 큰 연산자·lim 바로 뒤의 FROM·TO. 낱말 표 이름이라 from, From, FROM 은 받고 fROM 은 글자다.
    bool takeLimit(bool to) {
        const std::size_t mark = index;
        skipSpace();
        if (kind() == Kind::Word && wholeKeyword(current().text) == (to ? kTo : kFrom)) {
            take();
            return true;
        }
        index = mark;
        return false;
    }

    // 앞에 붙을 연산자 없이 나온 FROM·TO 를 낱말로 받는지. 글자 덩어리를 대문자로 바꾼 것이
    // 이름과 같으면(from, From) 받는다. 대문자로 시작한 FROM·TO 는 그 덩어리를 만들지 않아 같지 않고,
    // 그때는 앞에서 찾은 낱말이 큰 연산자(SUM, INT, lim …)나 FROM 일 때만 받는다(한글 2024:
    // x FROM a 와 alpha FROM a 는 글자, sum x FROM a 는 낱말). 받지 않으면 글자다.
    bool limitWordHere() const {
        const std::string& text = current().text;
        const std::uint32_t value = wholeKeyword(text);
        if (value != kFrom && value != kTo) return false;
        if (text.size() >= 2 && text[1] >= 'a' && text[1] <= 'z') return true;
        for (std::size_t at = index; at-- > 0;) {
            if (tokens[at].kind != Kind::Word) continue;
            const KeywordHit hit = keywordAt(tokens[at].text);
            if (hit.length == 0) continue;
            return (hit.value >> 17) == (0xE0 >> 1) || hit.value == kFrom;
        }
        return false;
    }

    // 지금 자리가 첨자(^ _ SUP SUB LSUB LSUP)로 시작하는지.
    bool startsScript() {
        if (kind() == Kind::Sup || kind() == Kind::Sub) return true;
        if (kind() != Kind::Word) return false;
        peelCommand();
        return wordIs("sup") || wordIs("sub") || wordIs("lsub") || wordIs("lsup");
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

    const EquationFont* fallbackFor(char32_t cp) const {
        const chwp::model::Lang lang = chwp::layout::hancomCharLang(cp, true);
        if (substitute && designOf(*substitute, cp) >= 0) return substitute;
        return fallbacks[static_cast<std::size_t>(lang)];
    }

    // 이 글자를 수식 글꼴 대신 잰 글꼴. 수식 글꼴로 재면 nullptr 이다.
    // 숫자·라틴 글자·연산자는 수식 글꼴 코드로 바꿔 재고, 그 밖에 수식 글꼴에 없는 글자만 대체 글꼴로 간다.
    const EquationFont* borrowedFont(char32_t cp) const {
        if (cp < 0x80 || equationGlyph(cp, false) != 0 || equationOperator(cp) != 0) return nullptr;
        if (symbol->unitsPerEm > 0 && designOf(*symbol, cp) >= 0) return nullptr;
        const EquationFont* font = fallbackFor(cp);
        if (!font || font == symbol || font->unitsPerEm <= 0 || designOf(*font, cp) < 0) return nullptr;
        return font;
    }

    // 수식 글꼴에 없는 글자의 폭. 대신 잴 글꼴에도 없으면 -1.
    std::int32_t fallbackAdvance(char32_t cp) const {
        const EquationFont* font = borrowedFont(cp);
        if (!font || em <= 0) return -1;
        const std::int32_t px = equationGlyphPx(*font, cp, designOf(*font, cp), em);
        return std::max(0, px * kHwpPerPx);
    }

    // 근호·울타리 안이 비면 한글은 폭 em/2, 높이 em 의 빈 목록을 둔다(한글 2024: sqrt {}).
    Box emptySlot() const {
        Box box = styleSwitch();
        box.width = std::max<std::int32_t>(em / 2, 1);
        return box;
    }

    // 엔진은 빈 목록 노드를 늘 em/2 × em 으로 잰다. 구조의 인자 자리(분수의 위·아래, 꾸밈, phantom, color, scale,
    // 행렬 칸)는 목록 노드라 비면 이 크기다(한글 2024: a over {} 의 분모, a hat 의 인자, matrix{a&} 의
    // 둘째 칸은 500×1000). 목록 안의 그냥 묶음 {} 은 노드가 아니라 아무것도 두지 않는다(a {} b 는 a b).
    // 첨자·한계·중괄호 노드는 빈 칸을 빼고 잰다.
    Box slotted(Box box) const { return box.empty ? emptySlot() : box; }

    std::int32_t symbolAdvance(char32_t cp) const {
        if (em <= 0 || symbol->unitsPerEm <= 0) return 0;
        const int design = designOf(*symbol, cp);
        if (design < 0) return std::max<std::int32_t>(fallbackAdvance(cp), 0);
        const std::int32_t px = equationGlyphPx(*symbol, cp, design, em);
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
                const std::int32_t px = equationGlyphPx(*metric, mapped, design, em);
                return std::max(0, px * kHwpPerPx);
            }
        }
        // 수식 글꼴에 없는 글자는 로만 글자 자리여도 대체 글꼴로 잰다(rm 一 도 함초롬바탕 600, 한글 2024).
        if (!onSymbol) {
            if (const std::int32_t borrowed = fallbackAdvance(cp); borrowed >= 0) return borrowed;
        }
        const int design = designOf(face, cp);
        if (design < 0) {
            if (onSymbol && cp >= 0x80) {
                if (const std::int32_t borrowed = fallbackAdvance(cp); borrowed >= 0) return borrowed;
            }
            // 어느 수식 글꼴에도 없는 기호는 .notdef 다. HYhwpEQ 의 .notdef 진행폭은 em 과 같다.
            if (onSymbol && cp >= 0x80 && symbol->unitsPerEm > 0) {
                // .notdef 는 코드가 없어 힌팅 폭을 찾지 않는다.
                const std::int32_t px = equationGlyphPx(*symbol, 0, symbol->unitsPerEm, em);
                return std::max(0, px * kHwpPerPx);
            }
            return std::max(em / 2, 1);
        }
        const double px =
            static_cast<double>(design) * em * kDpi / (static_cast<double>(kHwpPerInch) * face.unitsPerEm);
        const std::int32_t advPx = static_cast<std::int32_t>(std::llround(px));
        return std::max(0, advPx * kHwpPerPx);
    }

    // 분수선·근호 윗줄·테두리 두께. 그리는 쪽이 픽셀에 맞춘다.
    std::int32_t ruleThickness() const { return std::max<std::int32_t>(mulDiv(em, 5, 100), 1); }

    Item makeItem(Item::Kind kind, char32_t cp, char32_t code, std::int32_t x, std::int32_t y, std::int32_t w,
                  std::int32_t h) const {
        Item item;
        item.kind = kind;
        item.cp = cp;
        item.hwpCode = code;
        item.x = x;
        item.y = y;
        item.width = w;
        item.height = h;
        item.em = em;
        item.bold = boldMode;
        item.color = color;
        return item;
    }

    Item rule(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) const {
        return makeItem(Item::Kind::Rule, 0, 0, x, y, std::max(w, 0), std::max(h, 1));
    }

    // 수식 글꼴 글자 하나로 긋는 선. 글꼴 높이는 글자 크기이고 cellTop 에 칸 위를 맞춘다(TA_TOP). 선은 그 글자의
    // 잉크 자리다. 잉크 상자를 모르면 fallbackTop 에서 글자 크기 5% 두께로 긋는다.
    Item glyphRule(char32_t code, std::int32_t x, std::int32_t cellTop, std::int32_t w, std::int32_t fallbackTop) const {
        const std::int32_t cell = symbol->winAscent + symbol->winDescent;
        chwp::layout::FontInkBox ink;
        if (cell > 0 && chwp::layout::fontGlyphInkBox(symbol->face, code, ink) && ink.yMax > ink.yMin) {
            const std::int32_t top = cellTop + mulDiv(em, symbol->winAscent - ink.yMax, cell);
            const std::int32_t bottom = cellTop + mulDiv(em, symbol->winAscent - ink.yMin, cell);
            Item item = rule(x, top, w, bottom - top);
            item.hwpCode = code;
            return item;
        }
        return rule(x, fallbackTop, w, ruleThickness());
    }

    // 글자 하나를 놓을 것으로. 폭을 잰 advanceOf 와 같은 판단으로 코드와 기울임을 정한다.
    Item glyphItem(const EquationFont& face, char32_t cp, std::int32_t x, std::int32_t w, std::int32_t h,
                   Item::Face drawFace) const {
        const bool romanLetters = romanMode || romanAlias || (romanDistinct && &face == roman);
        const bool latin = (cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z');
        char32_t code = equationGlyph(cp, romanLetters);
        if (code == 0) code = equationOperator(cp);
        if (code == 0) code = cp;
        // 프라임은 ' 폭으로 재지만 그림은 ′ 다(한글 2024가 U+2032).
        Item item = makeItem(Item::Kind::Glyph, drawnSymbol(cp), code, x, 0, w, h);
        item.italic = latin && !romanLetters && drawFace == Item::Face::Symbol;
        // rm·함수 이름의 라틴 글자는 한글이 수식 글꼴의 ASCII 글자로 바꿔 긋고 대문자는 글자 크기 6.5%, 소문자는
        // 6.7% 올린다.
        if (latin && code == cp && drawFace == Item::Face::Symbol) {
            item.raise = mulDiv(em, cp <= U'Z' ? 65 : 67, 1000);
        }
        // 한글·한자는 수식 글꼴에 없어 본문 글꼴로 그린다.
        item.face = isHangulText(cp) || borrowedFont(cp) ? Item::Face::Roman : drawFace;
        return item;
    }

    // drawFace 는 그릴 글꼴 자리다. 따옴표 글과 lim 은 본문 글꼴(Roman)이다.
    Box measureText(const EquationFont& face, std::string_view text, Item::Face drawFace = Item::Face::Symbol) {
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
        const std::int32_t glyphH = box.height;
        std::int32_t tallest = box.height;
        while (nextCp(text, i, cp)) {
            // 굵게는 한글이 글자마다 1픽셀 넓힌다(한글 2024: bold Editor).
            const std::int32_t advance = advanceOf(face, cp) + (boldMode ? kHwpPerPx : 0);
            // 대체 글꼴로 잰 글자는 그 글꼴의 상자 높이다. 10pt 一(함초롬바탕)은 1200 이고 축 한가운데에 놓인다.
            const EquationFont* borrowed = borrowedFont(cp);
            const std::int32_t height = borrowed ? equationFontBoxPx(*borrowed, em) * kHwpPerPx : glyphH;
            tallest = std::max(tallest, height);
            // 공백(따옴표 글 안)은 폭만 있고 그릴 것은 없다.
            if (cp != U' ') box.items.push_back(glyphItem(face, cp, box.width, advance, height, drawFace));
            box.width += advance;
            const int cls = equationAtomClass(cp);
            if (!any) head = cls;
            tail = cls;
            any = true;
        }
        if (any) {
            box.headClass = head;
            box.tailClass = tail;
        }
        box.lowGlyphs = std::all_of(box.items.begin(), box.items.end(), [](const Item& item) {
            return item.kind == Item::Kind::Glyph && lowGlyphCode(item.hwpCode);
        });
        if (tallest > box.height) {
            box.height = tallest;
            box.axisBottom = -box.height / 2;
            const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) + box.height / 2;
            box.baseline = std::max<std::int32_t>(1, std::min(fromTop, box.height));
        }
        // 글자마다 제 높이로 상자 세로 한가운데에 놓는다.
        for (Item& item : box.items) item.y = (box.height - item.height) / 2;
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
        std::vector<std::int32_t> xs;
        for (std::size_t i = 0; i < real.size(); ++i) {
            above = std::max(above, real[i].baseline);
            below = std::max(below, real[i].height - real[i].baseline);
            xs.push_back(width);
            width += real[i].width;
            if (i + 1 < real.size()) width += atomGap(real[i], real[i + 1]);
        }
        Box out;
        // 기준선을 맞춰 가로로 붙인다. 높이를 잰 식과 같다.
        for (std::size_t i = 0; i < real.size(); ++i) place(out, real[i], xs[i], above - real[i].baseline);
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
        out.wordTail = real.back().wordTail;
        out.lowGlyphs = std::all_of(real.begin(), real.end(), [](const Box& box) { return box.lowGlyphs; });
        for (const Box& box : real) {
            out.axisBottom = std::min(out.axisBottom, box.axisBottom);
            if (box.quotBaseline) out.quotBaseline = true;
        }
        return out;
    }

    Box gap(std::int32_t width) const {
        Box box = metricsOf();
        box.width = std::max(width, 0);
        box.lowGlyphs = true;
        return box;
    }

    void stretch(std::vector<Box>& items, std::size_t lo, std::size_t hi) {
        // 짝 없는 닫는 울타리(RIGHT)의 안은 그 앞의 것이다. 비었으면 빈 목록이 울타리 앞에 선다
        // (한글 2024: right 노드는 앞 목록을 품는다. 한글 자동화로 만든 수식: right ) 혼자는 500 + 375).
        std::size_t contentStart = lo;
        for (std::size_t i = lo; i < hi; ++i) {
            if (items[i].stretch && !items[i].open) {
                bool any = false;
                for (std::size_t k = contentStart; k < i; ++k) any = any || !items[k].empty;
                if (!any) {
                    const Box slot = emptySlot();
                    for (Item& item : items[i].items) item.x += slot.width;
                    items[i].width += slot.width;
                    items[i].height = slot.height;
                    items[i].baseline = slot.baseline;
                    items[i].axisBottom = slot.axisBottom;
                    refitFence(items[i]);
                }
                contentStart = i + 1;
                continue;
            }
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
            Box packed = hpack(std::vector<Box>(items.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                                items.begin() + static_cast<std::ptrdiff_t>(contentEnd)));
            // 울타리 안이 비어도 빈 목록(폭 em/2)이 선다. 여는 울타리 뒤에 그 폭을 둔다. 짝 없는 LEFT 도 같다
            // (한글 자동화로 만든 수식: left ( 는 375 + 500, a left a 는 a 와 울타리 a 와 빈 목록의 1550).
            if (packed.empty) {
                packed = emptySlot();
                items[i].width += packed.width;
            }
            if (!packed.empty && packed.height > 0) {
                items[i].height = packed.height;
                items[i].baseline = packed.baseline;
                items[i].axisBottom = packed.axisBottom;
                refitFence(items[i]);
                if (depth == 0) {
                    items[j].height = packed.height;
                    items[j].baseline = packed.baseline;
                    items[j].axisBottom = packed.axisBottom;
                    refitFence(items[j]);
                }
            }
            i = depth == 0 ? j : hi - 1;
            contentStart = i + 1;
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

    // left·right 는 재는 코드, leftMeaning·rightMeaning 은 그릴 울타리 글자다. 울타리는 안쪽 높이만큼 늘어난다.
    Box addFences(const Box& inner, char32_t left, char32_t right, char32_t leftMeaning, char32_t rightMeaning) const {
        Box box = inner;
        box.items.clear();
        const std::int32_t leftW = left != 0 ? symbolAdvance(left) : 0;
        const std::int32_t rightW = right != 0 ? symbolAdvance(right) : 0;
        if (left != 0) box.items.push_back(makeItem(Item::Kind::Fence, leftMeaning, left, 0, 0, leftW, inner.height));
        place(box, inner, leftW, 0);
        if (right != 0) {
            box.items.push_back(
                makeItem(Item::Kind::Fence, rightMeaning, right, leftW + inner.width, 0, rightW, inner.height));
        }
        box.width = inner.width + leftW + rightW;
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
            // 아무것도 없는 줄은 빈 목록이라 폭이 글자 크기의 절반이다(한글 자동화로 만든 수식: above 는 500×2100).
            // & 뒤의 빈 칸은 그 줄 목록 안이라 폭이 없다(a & # b 는 725).
            if (row.size() == 1 && row[0].empty) colW[0] = std::max(colW[0], std::max<std::int32_t>(em / 2, 1));
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
        // & 칸 간격은 어디서나 글자 크기의 20%다(한글 2024: & 노드 폭 200. 한글 자동화로 만든 수식: a & b 는 1175).
        const std::int32_t gapX = mulDiv(em, 20, 100);
        const std::int32_t gapY =
            eqAlignRows ? mulDiv(em, 10, 100) : (matrix ? mulDiv(em, 15, 100) : std::max(em / 3, 1));
        Box out;
        for (std::int32_t w : colW) out.width += w;
        if (cols > 1) out.width += gapX * static_cast<std::int32_t>(cols - 1);
        std::vector<std::int32_t> colX;
        for (std::size_t c = 0, x = 0; c < cols; ++c) {
            colX.push_back(static_cast<std::int32_t>(x));
            x += static_cast<std::size_t>(colW[c] + gapX);
        }
        // 행렬 밖의 # 줄(eqalign 포함)은 줄마다 축을 맞춰 쌓는다. 맨 바깥 목록은 10% 간격이고 묶음 안은 붙인다
        // (한글 2024: a + b # c + d 는 975+100+975, {eqalign{a#b}} over z 의 분자는 975+975).
        if (eqAlignRows || !matrix) {
            return packEqAlign(rows, out.width, braceDepth == 0 ? mulDiv(em, 10, 100) : 0, colX, colW);
        }
        // 줄은 위에서부터 쌓고, 칸은 그 줄의 기준선에 맞춘다. 가로는 tableAlign 대로 칸 안에 놓는다.
        std::int32_t rowTop = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            for (std::size_t c = 0; c < rows[r].size(); ++c) {
                const Box& cell = rows[r][c];
                if (cell.empty) continue;
                const std::int32_t slack = colW[c] - cell.width;
                const std::int32_t dx = tableAlign < 0 ? 0 : tableAlign == 1 ? slack : slack / 2;
                place(out, cell, colX[c] + dx, rowTop + rowBase[r] - cell.baseline);
            }
            rowTop += rowH[r] + gapY;
        }
        for (std::int32_t h : rowH) out.height += h;
        if (rowH.size() > 1) out.height += gapY * static_cast<std::int32_t>(rowH.size() - 1);
        if (matrix) return centerMatrix(out);
        out.baseline = rowH.size() == 1 ? rowBase[0] : out.height / 2;
        return out;
    }

    // 첫 줄의 기준선을 유지하고, 다음 줄은 그 위에 글자 크기의 10% 간격으로 쌓는다.
    // 11pt eqalign {a # b} 는 525×2360, 기준선 41이다. a &= b 는 2251×1125다.
    Box packEqAlign(const std::vector<std::vector<Box>>& rows, std::int32_t width, std::int32_t gapY,
                    const std::vector<std::int32_t>& colX, const std::vector<std::int32_t>& colW) const {
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
            // 빈 줄은 빈 목록이라 높이가 글자 크기다(한글 자동화로 만든 수식: x # 는 975+100+1000).
            if (!any) {
                bottom = -em / 2;
                top = bottom + em;
            }
            // & 노드는 높이가 글자 크기다. & 가 있는 줄은 그만큼은 된다(eqalign 의 두 줄은 1000+100+1000).
            if (row.size() > 1) {
                bottom = std::min(bottom, -(em / 2));
                top = std::max(top, em - em / 2);
            }
            rowBottom.push_back(bottom);
            rowH.push_back(top - bottom);
        }
        Box out;
        out.width = width;
        // 줄 안의 칸은 축을 맞춘다. 첫 칸은 오른쪽, 나머지는 왼쪽에 붙인다(& 앞뒤가 맞닿는다).
        std::int32_t rowTop = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            for (std::size_t c = 0; c < rows[r].size() && c < colX.size(); ++c) {
                const Box& cell = rows[r][c];
                if (cell.empty || cell.height <= 0) continue;
                // 칸이 여럿이면 첫 칸은 오른쪽(& 에 붙음), 칸이 하나인 줄은 왼쪽에 붙인다.
                const std::int32_t dx = c == 0 && colX.size() > 1 ? colW[c] - cell.width : 0;
                place(out, cell, colX[c] + dx, rowTop + cell.axisBottom - rowBottom[r]);
            }
            rowTop += rowH[r] + gapY;
        }
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

    Box corners(const Box& given, const Box* sub, const Box* sup, bool capitalKern = true) const {
        // 빈 {} 바탕은 첨자 글자 크기만 한 빈 목록이다(한글 2024: {} _{n} 은 위 끝 -340).
        Box emptyBase;
        if (given.empty || given.height <= 0) {
            const Box* script = sub && !sub->empty ? sub : sup;
            const std::int32_t size = script && script->fontEm > 0 ? script->fontEm : em;
            emptyBase.height = size;
            emptyBase.axisBottom = -(size / 2);
            emptyBase.fontEm = size;
            // 기준선은 바깥 글자의 축 아래 35% 자리다(다른 글자와 축이 맞도록).
            emptyBase.baseline = std::max<std::int32_t>(1, em * 35 / 100 + size / 2);
        }
        const Box& base = given.empty || given.height <= 0 ? emptyBase : given;
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
        // 첨자 위 끝의 자리. 본문 글자 축에서 잰다(아래로 +).
        std::int32_t supTopAxis = 0;
        std::int32_t subTopAxis = 0;
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
            supTopAxis = finalBottom;
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
            subTopAxis = scriptTop - sub->height;
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
            supTopAxis = supFinal;
            subTopAxis = subTop - sub->height;
            top = supEdge - baseAxis;
            bottom = subEdge - baseAxis;
            extra = std::max(sup->width, sub->width);
            outAxis = supEdge;
            outFont = sup->fontEm;
        }
        Box out;
        // 기울임 대문자 뒤 위첨자는 글자 크기의 15% 를 띄운다(한글 2024: A^C, S^2 의 첨자 폭).
        std::int32_t supKern = 0;
        if (capitalKern && hasSup && !base.items.empty()) {
            const Item& last = base.items.back();
            if (last.kind == Item::Kind::Glyph && last.italic && last.cp >= U'A' && last.cp <= U'Z') {
                supKern = mulDiv(base.fontEm > 0 ? base.fontEm : em, 15, 100);
            }
        }
        // 본문은 상자 안 -top 에, 첨자는 본문 오른쪽에 둔다. 첨자 자리는 본문 위 끝(축 baseAxis)에서 잰다.
        place(out, base, 0, -top);
        if (hasSup) place(out, *sup, base.width + supKern, supTopAxis - base.axisBottom - top);
        if (hasSub) place(out, *sub, base.width, subTopAxis - base.axisBottom - top);
        if (hasSup) extra = std::max(extra, sup->width + supKern);
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
        // 첨자가 붙은 늘어난 울타리는 보통 글자처럼 뒤 간격을 둔다(LEFT ( x-p RIGHT )^2 + q 의 + 앞 20%).
        // 첨자 없는 울타리 뒤는 간격이 없다(RIGHT ) + 는 붙는다).
        if ((hasSup || hasSub) && base.stretch && out.tailClass == 0) out.tailClass = 1;
        out.fnHead = base.fnHead;
        // 빈 첨자도 첨자 노드라 뒤 간격은 첨자 붙은 글자의 것이다. 크기는 더하지 않는다
        // (한글 자동화로 만든 수식: x^{} + y 는 x + y 보다 60 넓은 2290, x^{} 는 600×975).
        if (sup || sub) {
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
        // 위 한계, 기호 칸(간격 높이의 한가운데에 기호), 아래 한계를 가운데 맞춰 쌓는다.
        if (realSup) place(out, *sup, (out.width - supW) / 2, 0);
        place(out, glyph, (out.width - glyph.width) / 2, supH + gap / 2 - glyph.height / 2);
        if (realSub) place(out, *sub, (out.width - subW) / 2, supH + gap);
        out.height = gap + subH + supH;
        out.axisBottom = -(supH + gap / 2);
        out.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        out.quotBaseline = true;
        // 적분처럼 앞 글자에게는 보통 글자다. a+sum x 의 + 는 양쪽에 이항 간격을 둔다(한글 자동화로 만든 수식 3505).
        out.headClass = 1;
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
        // 기호 칸 위 끝. 한계를 붙이기 전의 자리다.
        const std::int32_t glyphTop = shift;
        if (subBottom + half < 0) half = -subBottom;
        const std::int32_t supProbe = supH + shift + supBottom;
        if (supProbe > 0) shift -= supProbe;
        const std::int32_t bottom = std::min(supBottom + shift, 0);
        const std::int32_t lower = std::max(subBottom + half + subH, 0);
        Box out;
        out.width = std::max(subW - mulDiv(em, 50, 100), supW + mulDiv(em, 5, 100)) + mulDiv(em, 1, 100) +
                    glyph.width;
        // 기호는 왼쪽, 위 한계는 기호 오른쪽 위, 아래 한계는 기호 아래 끝 밑으로 파고든 오른쪽 아래다.
        // 너비 식의 두 항이 각 한계의 오른쪽 끝이다.
        place(out, glyph, 0, glyphTop - bottom);
        const std::int32_t tail = mulDiv(em, 1, 100);
        if (realSup) place(out, *sup, glyph.width + mulDiv(em, 5, 100) + tail, supBottom + shift - bottom);
        if (realSub) {
            place(out, *sub, std::max<std::int32_t>(0, glyph.width - mulDiv(em, 50, 100) + tail),
                  subBottom + half - bottom);
        }
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
        const std::int32_t column =
            std::max(word.width, std::max(realSub ? sub->width : 0, realSup ? sup->width : 0));
        out.width = column + side;
        // lim 은 축 한가운데, 한계는 그 아래에 가운데 맞춰 쌓고, 본문 { } 는 오른쪽에 축을 맞춰 둔다.
        place(out, word, (column - word.width) / 2, -half - bottom);
        if (realSub) place(out, *sub, (column - sub->width) / 2, half - bottom);
        if (realSup) place(out, *sup, (column - sup->width) / 2, half + subH - bottom);
        if (side > 0) place(out, *slot, column, slot->axisBottom - bottom);
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
        out.items.clear();
        out.limits = false;
        out.prescript = false;
        const bool hasSup = sup && !sup->empty && sup->height > 0;
        const bool hasSub = sub && !sub->empty && sub->height > 0;
        if (hasSup) {
            out.width = std::max(out.width, sup->width);
            out.height += gap + sup->height;
            out.baseline += gap + sup->height;
        }
        if (hasSub) {
            out.width = std::max(out.width, sub->width);
            out.height += gap + sub->height;
        }
        // 위 한계, 본문, 아래 한계를 가운데 맞춰 쌓는다.
        const std::int32_t baseTop = hasSup ? sup->height + gap : 0;
        if (hasSup) place(out, *sup, (out.width - sup->width) / 2, 0);
        place(out, base, (out.width - base.width) / 2, baseTop);
        if (hasSub) place(out, *sub, (out.width - sub->width) / 2, baseTop + base.height + gap);
        return out;
    }

    // 앞첨자는 보통 첨자와 같이 잰 첨자 노드를 바탕 왼쪽에 둔다
    // (한글 2024: x LSUB y 는 x_y 와 같은 1177, x LSUP y 는 x^y 와 같은 1163).
    // 바탕이 빈 첨자 노드. 엔진은 연산자 글자 뒤의 첨자를 그 글자에 붙이지 않고 이렇게 따로 둔다.
    // 앞뒤 글자에게는 보통 글자이고, 뒤 간격은 첨자 붙은 글자의 것이다.
    Box loneScript(const Box* sub, const Box* sup) const {
        Box base = metricsOf();
        base.width = 0;
        Box out = corners(base, sub, sup);
        out.headClass = 1;
        out.tailClass = 1;
        return out;
    }

    // 앞첨자(LSUB·LSUP)는 앞 노드 앞에 따로 선 첨자 노드다. 세로 자리는 그 노드의 첨자처럼 정하고, 첨자 노드와 그 노드
    // 사이에는 첨자 붙은 글자 뒤 간격을 둔다(한글 2024: x + lsub{b} a 는 x, 첨자, +, a 이고 첨자 뒤 + 앞은
    // 200. 한글 자동화로 만든 수식: {a over c} lsub{b} x 는 분수 아래로 202 내려간 2452).
    Box attachPrescript(const Box& base, const Box& script, bool sub) const {
        Box plain = base;
        plain.items.clear();
        Box scriptOnly = script;
        scriptOnly.prescript = false;
        Box out = sub ? corners(plain, &scriptOnly, nullptr, false) : corners(plain, nullptr, &scriptOnly, false);
        // corners 는 바탕을 왼쪽, 첨자를 오른쪽에 놓았다. 첨자를 왼쪽으로, 바탕을 그 오른쪽으로 옮긴다.
        const std::int32_t scriptW = out.width - base.width;
        Box node;
        node.tailClass = 1;
        node.scripted = true;
        node.gapEm = base.fontEm > 0 ? base.fontEm : em;
        node.fontEm = node.gapEm;
        const std::int32_t gap = atomGap(node, base);
        for (Item& item : out.items) item.x -= base.width;
        Box moved;
        place(moved, out, 0, 0);
        const std::int32_t baseY = out.baseline - base.baseline;
        place(moved, base, scriptW + gap, baseY);
        out.items = std::move(moved.items);
        out.width = base.width + scriptW + gap;
        // 바탕 뒤 간격은 바탕 그대로다(x LSUB y + 의 + 앞은 보통 14%). 앞쪽은 보통 글자다.
        out.scripted = false;
        out.headClass = 1;
        out.tailClass = base.tailClass;
        out.fontEm = base.fontEm > 0 ? base.fontEm : em;
        return out;
    }

    // REL·BUILDREL. 위·아래 글은 첨자가 아니라 원래 크기다. 폭은 둘 중 넓은 쪽에 em/2 를 더하고,
    // 높이는 위 + 가운데 + 아래다. 가운데는 화살표면 글자 높이의 35%, 다른 기호(=)면 글자 높이 그대로다.
    // 축 한가운데에 놓고 앞뒤 간격은 없다. BUILDREL 의 아래는 빈 칸이다
    // (한글 2024: A REL <-> {+2} {-5} B 의 rel 은 1990×2292, = 이면 높이 2925).
    Box relation(const Box& arrow, const Box& given, const Box& under) const {
        const Box over = given.empty || given.height <= 0 ? emptySlot() : given;
        const Box below = under.empty || under.height <= 0 ? emptySlot() : under;
        const std::int32_t glyphH = equationHeightPx(std::max(em, 0)) * kHwpPerPx;
        char32_t cp = 0;
        for (const Item& item : arrow.items) {
            if (item.kind == Item::Kind::Glyph) cp = item.cp;
        }
        const bool isArrow = cp >= 0x2190 && cp <= 0x21FF;
        const std::int32_t middle = isArrow ? mulDiv(glyphH, 35, 200) * 2 : glyphH;
        Box out;
        out.width = std::max(over.width, below.width) + em / 2;
        out.height = over.height + middle + below.height;
        place(out, over, (out.width - over.width) / 2, 0);
        place(out, below, (out.width - below.width) / 2, over.height + middle);
        if (cp != 0) {
            Item band = makeItem(Item::Kind::Accent, cp, cp, 0, over.height, out.width, middle);
            out.items.push_back(band);
        }
        out.axisBottom = -(out.height / 2);
        out.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        out.quotBaseline = true;
        // box 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다(rel -> {a}{b} + 1 은 2515).
        out.headClass = 1;
        out.tailClass = 0;
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
        // 분자·분모는 가운데 맞춘다. 분수선은 간격 한가운데에, 너비 더하기의 절반만큼 안으로 들여 긋는다.
        place(out, num, (out.width - num.width) / 2, 0);
        place(out, den, (out.width - den.width) / 2, num.height + gap);
        if (rule) {
            // 한글은 분수선을 U+E06D 글자로 긋는다: 글자 크기 높이·선 길이 폭의 글꼴로
            // 분자 아래 끝에 칸 위를 맞춘다. 선은 글자 크기 10%씩 들여 긋는다.
            const std::int32_t inset = mulDiv(em, 10, 100);
            out.items.push_back(glyphRule(0xE06D, inset, num.height, out.width - inset * 2,
                                          num.height + gap / 2 - ruleThickness() / 2));
        }
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

    Box radical(const Box& given, const Box& degree) {
        const Box inner = given.empty || given.height <= 0 ? emptySlot() : given;
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
        // 지수는 왼쪽 위, 근호는 지수가 넘친 만큼 오른쪽에서 tail 아래로 scaled 높이, 윗줄은 근호 끝에서 오른쪽 끝까지.
        // 안쪽은 근호 오른쪽 끝에 바로 붙이고(17% 는 오른쪽 끝 여백이다), 위에서 tail 과 10% 아래에 둔다
        // (한글이 그린 PDF: a + sqrt b + c 의 b).
        const std::int32_t signX = std::max(overflow, 0);
        const std::int32_t innerX = signX + glyphW;
        if (!degree.empty && degree.width > 0) place(out, degree, signX + glyphW / 2 - degree.width, 0);
        // 근호는 글자 크기 높이를 늘림 비율만큼 키운 글꼴로 긋는다. 칸 높이가 그 글꼴 높이다.
        out.items.push_back(makeItem(Item::Kind::Radical, 0x221A, 0xE05C, signX, tail, glyphW, mulDiv(em, scale, 100)));
        // 윗줄은 U+E06D 글자다. 칸 위가 근호 칸 위보다 글자 크기 18% 위이고, 안쪽 시작보다 2% 왼쪽에서 안쪽 너비에
        // 17%를 더한 길이다.
        out.items.push_back(glyphRule(0xE06D, innerX - mulDiv(em, 2, 100), tail - mulDiv(em, 18, 100),
                                      inner.width + mulDiv(em, 17, 100), tail));
        place(out, inner, innerX, tail + ten);
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
        // hat 계열은 너비가 글자와 기호 중 큰 값이다. bar dyad under 는 거기에 기호 너비의 20%를 더하고,
        // vec 은 글자에 20%를 더한 뒤 기호와 큰 값을 고른다.
        // under 는 같은 여분을 아래로 붙이고 기준선은 본문 글자에 둔다.
        const std::int32_t glyphH = equationHeightPx(std::max(em, 0)) * kHwpPerPx;
        const std::int32_t extra = mulDiv(glyphH, 20, 100);
        // arch 는 아래로도 글자 높이의 1/15 이 늘어난다(한글 2024, 8~20pt: 10pt 는 위 195, 아래 65).
        const std::int32_t below = mark.cp == 0xE065 ? glyphH / 15 : 0;
        const std::int32_t glyphW = symbolAdvance(mark.cp);
        Box out = inner;
        out.items.clear();
        const std::int32_t pad = mark.widen ? mulDiv(glyphW, 20, 100) : 0;
        out.width = mark.padContent ? std::max(inner.width + pad, glyphW) : std::max(inner.width, glyphW) + pad;
        out.height = inner.height + extra + below;
        // 본문은 가운데, 꾸밈은 위(under 는 아래) 여분 띠 전체 너비다.
        const std::int32_t innerY = mark.under ? 0 : extra;
        place(out, inner, (out.width - inner.width) / 2, innerY);
        if (mark.meaning == 0x203E && !mark.under) {
            // bar 는 U+E06D 글자 하나다: 글꼴 높이는 글자 크기, 폭은 안쪽 너비라 선 길이가
            // 안쪽 너비다. 글자 크기 1/20 오른쪽에서, 칸 위를 띠 위에 맞추고 안쪽이 키 낮은 글자만이 아니면
            // 띠 높이(E06D 높이의 20%)의 절반 더 올린다.
            const std::int32_t lift = inner.lowGlyphs ? 0 : extra / 2;
            out.items.push_back(glyphRule(0xE06D, em / 20, -lift, inner.width, extra / 2 - ruleThickness() / 2));
        } else {
            Item band = makeItem(Item::Kind::Accent, mark.meaning, mark.cp, 0, mark.under ? inner.height : 0,
                                 out.width, extra);
            band.under = mark.under;
            out.items.push_back(band);
        }
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
        if (text == ".") {
            box.width = 0;
            return box;
        }
        // 울타리 글자가 없으면 한글은 코드 0 조각을 만든다. 여는 울타리는 폭이 없고,
        // 닫는 울타리는 코드 0 을 글꼴에 없는 글자로 재 평균 글자 폭(xAvgCharWidth)이다
        // (한글 자동화로 만든 수식: a right 는 10pt 525, 14pt 675, 8pt 375 를 더한다).
        // 평균 폭이 없는 폭 정보 파일은 equationGlyphPx 처럼 글꼴 단위의 절반을 쓴다.
        if (text.empty()) {
            const std::int32_t avg = symbol->xAvgCharWidth > 0 ? symbol->xAvgCharWidth : symbol->unitsPerEm / 2;
            box.width = open || avg <= 0 ? 0 : equationGlyphPx(*symbol, 0, avg, em) * kHwpPerPx;
            return box;
        }
        const char32_t piece = fencePiece(text);
        char32_t meaning = fenceMeaning(text);
        char32_t code = piece;
        if (piece != 0) {
            box.width = symbolAdvance(piece);
        } else {
            char32_t cp = symbolCode(text);
            if (cp == 0) {
                std::size_t i = 0;
                nextCp(text, i, cp);
            } else {
                meaning = cp;
            }
            code = equationOperator(cp) != 0 ? equationOperator(cp) : cp;
            box.width = cp == 0 ? 0 : advanceOf(*symbol, cp);
        }
        // 울타리는 늘어난 뒤(stretch, refitFence) 상자 높이만큼 그린다.
        if (box.width > 0) box.items.push_back(makeItem(Item::Kind::Fence, meaning, code, 0, 0, box.width, box.height));
        // 여는 울타리 앞의 = 는 관계 간격을 둔다. 글자 뒤는 간격을 더하지 않는다.
        // 11pt y= LEFT | 는 울타리 앞에 231이 있다. f LEFT ( 는 없다.
        if (open && box.width > 0) box.headClass = 1;
        return box;
    }

    Box takeDelim(bool open) {
        skipSpace();
        std::string text;
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
            refitFence(side);
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
            refitFence(side);
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

    // LONGDIV·LADDER·SLADDER 의 칸 글. # 은 줄, & 는 칸이다. ~ 는 숫자 한 칸 자리로 남긴다.
    std::vector<std::vector<std::string>> readGrid() {
        std::vector<std::vector<std::string>> rows(1, std::vector<std::string>(1));
        skipSpace();
        if (kind() != Kind::LBrace) return rows;
        take();
        int depth = 0;
        while (kind() != Kind::End) {
            if (kind() == Kind::RBrace && depth == 0) {
                take();
                break;
            }
            if (kind() == Kind::LBrace) ++depth;
            if (kind() == Kind::RBrace) --depth;
            if (kind() == Kind::Hash && depth == 0) {
                rows.emplace_back(1);
            } else if (kind() == Kind::Amp && depth == 0) {
                rows.back().emplace_back();
            } else if (kind() == Kind::Wide) {
                rows.back().back() += '~';
            } else if (kind() != Kind::Space && kind() != Kind::Thin && kind() != Kind::LBrace &&
                       kind() != Kind::RBrace) {
                rows.back().back() += current().text;
            }
            take();
        }
        return rows;
    }

    // 칸 글 하나. ~ 는 숫자 폭의 빈 자리다.
    Box gridCell(const std::string& text, std::int32_t digit) {
        Box box = metricsOf();
        box.width = 0;
        std::string run;
        const auto flush = [&] {
            if (run.empty()) return;
            Box part = measureText(textFont(), run);
            place(box, part, box.width, 0);
            box.width += part.width;
            run.clear();
        };
        for (char ch : text) {
            if (ch == '~') {
                flush();
                box.width += digit;
            } else {
                run += ch;
            }
        }
        flush();
        return box;
    }

    // LONGDIV {제수}{몫}{줄#줄…}. 폭은 제수 + max(몫, 몸통) + 글자 크기의 57.5%,
    // 몸통 높이는 G + (줄-1)(G + 15%), 전체는 몸통 + 1.2G, 위 끝은 축 위 1.1G 다(G 는 글자 높이).
    // ~ 가 없으면 몸통 줄은 나눗셈 차례대로 자리 잡는다: 빼는 수 j 의 오른쪽 끝은 (피제수 자리 - 몫 자리) + j 열,
    // 그 나머지는 한 열 더(마지막 열까지). (한글 2024, 8~20pt)
    // 크기는 한글 2024 와 같다: 폭 = max(몫, 몸통) + 나눗셈 기호(U+E10F) 폭 + 제수 + 20%,
    // 높이 = 몫 + 기호 높이의 20% + 몸통. 제수·몫은 목록이라 비면 em/2 × em 이고, 몸통이 없으면 0, {} 이면 빈 목록이다
    // (한글 자동화로 만든 수식: longdiv 혼자 1575×1195, longdiv{6}{42} 2150×1170, longdiv{6}{42}{} 2150×2170).
    Box longDivision() {
        const Box divisor = slotted(bracedArg());
        // 제수와 몫은 중괄호 없이 한 항도 받는다(a longdiv a, longdiv{6} 42). 몸통은 중괄호가 있어야 하고, 없으면
        // 거기서 해석이 끝난다(한글 자동화로 만든 수식: longdiv{6}{42} + 1 은 longdiv{6}{42} 와 같은 2150×1170).
        skipSpace();
        auto quotientRows = readGrid();
        if (quotientRows.size() == 1 && quotientRows.front().size() == 1 && quotientRows.front().front().empty() &&
            (kind() == Kind::Word || kind() == Kind::Number)) {
            quotientRows.front().front() = current().text;
            take();
        }
        skipSpace();
        const bool bodyGiven = kind() == Kind::LBrace;
        if (!bodyGiven) tokens.resize(index);
        const auto lines = readGrid();
        const std::int32_t glyphH = equationHeightPx(std::max(em, 0)) * kHwpPerPx;
        const std::int32_t digit = std::max(measureText(textFont(), "0").width, 1);
        const std::int32_t bracketW = symbolAdvance(0xE10F);
        const std::int32_t extra = bracketW + mulDiv(em, 20, 100);
        const std::int32_t lineGap = mulDiv(em, 15, 100);
        const std::string& quotientText = quotientRows.front().front();
        const Box quotient = quotientText.empty() ? emptySlot() : gridCell(quotientText, digit);
        std::vector<std::string> texts;
        if (bodyGiven) {
            for (const auto& row : lines) texts.push_back(row.front());
        }
        const bool emptyBody = bodyGiven && texts.size() == 1 && texts.front().empty();
        const auto rows = static_cast<std::int32_t>(texts.size());
        std::vector<Box> cells;
        for (const std::string& text : texts) cells.push_back(text.empty() ? emptySlot() : gridCell(text, digit));
        // 몸통 줄 자리. 어느 줄이든 첫 글자가 ~ 나 ` 이면 자동으로 맞추지 않는다.
        // 자동이면 홀수 줄(빼는 수)은 윗줄 첫 숫자가 이 줄 첫 숫자보다 작을 때 한 자리 밀린다. 윗줄을 이 줄 자릿수(+ 그 한 자리)로
        // 자르거나 0 을 채워 정수로 빼고, 차의 자릿수만큼 남고 나머지 자리는 사라진다(차가 0 이면 모두, 음수면 하나도 아님).
        // 다음 짝수 줄(나머지)은 사라진 자리만큼 밀리고, 첫 글자가 0 이면 한 자리 덜 밀린다. 미는 양은 줄마다 쌓인다.
        // 숫자 아닌 글자가 빼기에 끼면 거기서 자동 맞춤을 그만두고 모든 줄을 왼쪽에 둔다. 한 자리는 '1' 의 폭이다.
        const std::int32_t one = std::max(measureText(textFont(), "1").width, 1);
        const auto digitsOnly = [](const std::string& text) {
            return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
        };
        bool automatic = true;
        for (const std::string& text : texts) {
            if (!text.empty() && (text[0] == '~' || text[0] == '`')) automatic = false;
        }
        std::vector<std::int32_t> shiftBy(texts.size(), 0);
        std::int32_t gone = 0;
        for (std::size_t r = 1; automatic && r < texts.size(); ++r) {
            if (r % 2 == 1) {
                const std::string& above = texts[r - 1];
                const std::string& below = texts[r];
                if (!digitsOnly(above) || !digitsOnly(below)) {
                    automatic = false;
                    break;
                }
                gone = 0;
                std::int32_t carry = 0;
                if (!above.empty() && !below.empty()) {
                    carry = above[0] < below[0] ? 1 : 0;
                    std::string top = above;
                    const std::size_t want = below.size() + static_cast<std::size_t>(carry);
                    if (top.size() > want) top.resize(want);
                    while (top.size() < want) top += '0';
                    const std::int64_t rest = std::stoll(top) - std::stoll(below);
                    if (rest == 0) gone = static_cast<std::int32_t>(below.size());
                    if (rest > 0) {
                        gone = static_cast<std::int32_t>(below.size()) -
                               static_cast<std::int32_t>(std::to_string(rest).size());
                    }
                }
                shiftBy[r] = one * carry;
            } else {
                const std::int32_t steps = !texts[r].empty() && texts[r][0] == '0' ? gone - 1 : gone;
                shiftBy[r] = one * steps;
            }
        }
        if (!automatic) std::fill(shiftBy.begin(), shiftBy.end(), 0);
        std::vector<std::int32_t> lineX;
        std::int32_t bodyW = 0;
        std::int32_t bodyH = 0;
        for (std::int32_t r = 0, x = 0; r < rows; ++r) {
            x += shiftBy[static_cast<std::size_t>(r)];
            lineX.push_back(x);
            // 몸통 폭은 쌓기 전 미는 양에 줄 폭을 더한 것 가운데 가장 큰 값이다.
            bodyW = std::max(bodyW, shiftBy[static_cast<std::size_t>(r)] + cells[static_cast<std::size_t>(r)].width);
            bodyH += cells[static_cast<std::size_t>(r)].height + (r > 0 ? lineGap : 0);
        }
        const std::int32_t region = std::max(quotient.width, bodyW);
        const std::int32_t bodyTop = quotient.height + mulDiv(glyphH, 20, 100);
        Box out;
        out.width = divisor.width + region + extra;
        out.height = bodyH + bodyTop;
        // 그릴 자리: 제수는 왼쪽 끝, 몸통과 몫은 제수 + 나눗셈 기호 폭에서 시작하고
        // 몫은 몸통 오른쪽 끝에 맞춘다. 크기의 20% 는 오른쪽 끝 여백이다. 기호는 제수 + 기호 폭의 10%,
        // 피제수 위 줄은 제수 + 기호 폭의 70% 에서 시작한다(한글이 그린 PDF: LONGDIV {66}{4}{24#24#0} 의 24 는 제수 뒤 375).
        const std::int32_t bodyX = divisor.width + bracketW;
        place(out, divisor, 0, bodyTop);
        place(out, quotient, bodyX + region - quotient.width, 0);
        const std::int32_t thick = ruleThickness();
        const std::int32_t barX = divisor.width + mulDiv(bracketW, 70, 100);
        out.items.push_back(makeItem(Item::Kind::Fence, U')', 0xE10F, divisor.width + mulDiv(bracketW, 10, 100), bodyTop,
                                     bracketW, glyphH));
        out.items.push_back(rule(barX, bodyTop - glyphH / 10, bodyX + region - barX, thick));
        for (std::int32_t r = 0, y = bodyTop; r < rows && !emptyBody; ++r) {
            const Box& cell = cells[static_cast<std::size_t>(r)];
            place(out, cell, bodyX + lineX[static_cast<std::size_t>(r)], y);
            // 빼는 수 아래 줄.
            if (r % 2 == 1) {
                out.items.push_back(rule(bodyX + lineX[static_cast<std::size_t>(r)], y + cell.height + lineGap / 2,
                                         cell.width, thick));
            }
            y += cell.height + lineGap;
        }
        // 위 끝은 축 위로 몫 높이와 기호 높이의 10% 다.
        out.axisBottom = -(quotient.height + mulDiv(glyphH, 20, 100) / 2);
        // box 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다(longdiv{6}{42}{4} + 1 은 3640).
        out.fontEm = em;
        // 기준선은 상자 아래 끝보다 아래일 수 있다. 자르지 않는다(a longdiv a 는 1097 + 488 = 1585).
        out.baseline = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.quotBaseline = true;
        out.headClass = 1;
        out.tailClass = 0;
        return out;
    }

    // LADDER(사다리 나눗셈)와 SLADDER(2진수 바꾸기). # 은 줄, & 는 칸이고 칸마다 목록이라 빈 칸은 em/2 × em 이다.
    // 크기는 한글 2024 와 같다:
    // - 줄마다 칸 앞에 글자 크기의 20% 를 두고, 그 칸의 열 폭(앞 줄까지의 가장 넓은 칸)을 더한다. 줄 폭 중 가장 넓은 것이 폭이다.
    // - 첫 줄이 아닌 마지막 줄은 제수가 없어 한 열 밀려 시작하고, 마지막 칸 대신 첫 열 폭을 더한다.
    // - SLADDER 는 열이 셋 이상이면 첫 줄 마지막 칸(나머지)을 재지 않는다.
    // - 열이 둘 이상이면 세로줄(U+E101) 폭을, SLADDER 는 열이 셋 이상·줄이 둘 이상이면 ⋯ 와 20% 를 더한다.
    // - 줄 높이는 그 줄 칸 중 가장 높은 것에 15% 를 더한다. 축 한가운데에 놓는다.
    // (한글 자동화로 만든 수식: ladder{} 700×1150, ladder{2&} 1950, sladder{2&4} 1975)
    Box ladder(bool binary) {
        skipSpace();
        const bool braced = kind() == Kind::LBrace;
        auto rows = readGrid();
        // 중괄호가 없으면 빈 사다리를 두고 거기서 해석이 끝난다(a ladder a 는 a 와 빈 사다리의 1225).
        if (!braced) tokens.resize(index);
        const std::int32_t glyphH = equationHeightPx(std::max(em, 0)) * kHwpPerPx;
        const std::int32_t digit = std::max(measureText(textFont(), "0").width, 1);
        const std::int32_t gap = mulDiv(em, 20, 100);
        const std::int32_t lineGap = mulDiv(em, 15, 100);
        std::size_t cols = 1;
        for (const auto& row : rows) cols = std::max(cols, row.size());
        const auto cellAt = [&](std::size_t r, std::size_t c) {
            const std::string text = c < rows[r].size() ? rows[r][c] : std::string();
            return text.empty() ? emptySlot() : gridCell(text, digit);
        };
        std::vector<std::int32_t> colW(cols, 0);
        std::vector<std::int32_t> rowH(rows.size(), 0);
        std::int32_t width = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const bool shifted = r > 0 && r + 1 == rows.size();
            std::size_t slot = shifted ? 1 : 0;
            std::int32_t x = 0;
            for (std::size_t c = 0; c < cols; ++c) {
                x += gap;
                if (c + 1 == cols) {
                    if (shifted) {
                        x += colW[0];
                        break;
                    }
                    if (binary && r == 0 && cols > 2) break;
                }
                if (slot == cols) break;
                const Box cell = cellAt(r, c);
                rowH[r] = std::max(rowH[r], cell.height);
                colW[slot] = std::max(colW[slot], cell.width);
                x += colW[slot];
                ++slot;
            }
            width = std::max(width, x);
        }
        const std::int32_t barW = cols > 1 ? symbolAdvance(0xE101) : 0;
        const std::int32_t dotsW = binary && cols > 2 && rows.size() > 1 ? advanceOf(*symbol, 0x22EF) : 0;
        const std::int32_t dots = dotsW > 0 ? dotsW + gap : 0;
        width += barW + dots;
        // 세로줄은 첫 열, ⋯ 는 마지막 열 폭에 든다.
        if (cols > 1) colW[0] += barW;
        if (dots > 0) colW[cols - 1] += dots;
        std::vector<std::int32_t> rowY(rows.size(), 0);
        std::int32_t y = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            rowY[r] = y;
            y += rowH[r] + lineGap;
        }
        Box out;
        out.width = width;
        out.height = y;
        // 그릴 자리: 첫 칸은 왼쪽 끝에서, 칸마다 그 열 폭 가운데에 두고 열 폭과 20% 를 넘어간다.
        // 밀린 마지막 줄은 열 폭과 20% 를 먼저 넘어가 다음 열 가운데에 둔다. ⋯ 가 있으면 마지막 열 칸을 그 절반만큼 민다.
        // 크기에서 뺀 칸(밀린 마지막 줄의 끝 칸, SLADDER 첫 줄의 나머지)은 그리지도 않는다.
        // (한글이 그린 PDF: SLADDER {2&7&1} 의 7 은 2 뒤 984, 2 는 세로줄을 품은 첫 열 가운데)
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const bool shifted = r > 0 && r + 1 == rows.size();
            std::int32_t x = 0;
            std::int32_t shiftedX = 0;
            for (std::size_t c = 0; c < cols; ++c) {
                if (c + 1 == cols && (shifted || (binary && r == 0 && cols > 2))) break;
                const Box cell = cellAt(r, c);
                std::int32_t cellX = 0;
                if (shifted) {
                    shiftedX += gap + colW[c];
                    if (dots > 0 && c + 2 == cols) shiftedX += dots / 2;
                    cellX = shiftedX + (colW[c + 1] - cell.width) / 2;
                } else {
                    if (dots > 0 && c + 1 == cols) {
                        // ⋯ 는 글자 노드가 아니라 사다리가 그리는 장식이다. 마지막 열 앞쪽에 둔다.
                        if (r > 0) out.items.push_back(makeItem(Item::Kind::Accent, 0x22EF, 0x22EF, x,
                                                                rowY[r] + (rowH[r] - glyphH) / 2, dotsW, glyphH));
                        x += dots / 2;
                    }
                    cellX = x + (colW[c] - cell.width) / 2;
                }
                x += gap + colW[c];
                place(out, cell, cellX, rowY[r] + (rowH[r] - cell.height) / 2);
            }
        }
        if (cols > 1) {
            const std::int32_t thick = ruleThickness();
            const std::int32_t barX = colW[0] - barW / 2;
            for (std::size_t r = 0; r + 1 < rows.size() || (r == 0 && rows.size() == 1); ++r) {
                // 제수 오른쪽 세로줄과 수 아래 가로줄.
                out.items.push_back(rule(barX, rowY[r], thick, rowH[r] + lineGap));
                out.items.push_back(rule(barX, rowY[r] + rowH[r] + lineGap - thick, width - barX, thick));
            }
        }
        out.axisBottom = -(out.height / 2);
        out.fontEm = em;
        const std::int32_t fromTop = (em > 0 ? em * 35 / 100 : 0) - out.axisBottom;
        out.baseline = std::max<std::int32_t>(1, std::min(fromTop, std::max(out.height, 1)));
        out.quotBaseline = true;
        // box 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다(ladder{2&4} + 1 은 3465).
        out.headClass = 1;
        out.tailClass = 0;
        return out;
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
    Box brace(bool under);
    Box emitWord(std::string_view word, bool spaceAfter = false, bool loose = true, bool functions = true);

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
    const bool savedBold = boldMode;
    ++braceDepth;
    take();
    Box inner = lines();
    if (kind() == Kind::RBrace) take();
    --braceDepth;
    romanMode = savedRoman;
    boldMode = savedBold;
    return inner;
}

Box Parser::bracedArg() {
    skipSpace();
    if (kind() == Kind::LBrace) return readGroup();
    return piece();
}

Box Parser::radicalArg() {
    // 중괄호가 없으면 공백 앞까지가 근호 안이다. sqrty+1 은 y+1 의 근호이고, sqrt y + 1 은 y 의 근호에 1을 더한다.
    // ` 도 근호 안을 끝낸다. ~ 는 끝내지 않는다(한글 자동화로 만든 수식: a sqrt 2`b 의 ` 와 b 는 근호 밖이라 높이 1137,
    // a sqrt 2~b 의 b 는 근호 안).
    skipSpace();
    if (kind() == Kind::LBrace) return readGroup();
    std::vector<Box> parts;
    int guard = 0;
    while (guard++ < 64 && !stopped() && kind() != Kind::Space && kind() != Kind::LBrace && kind() != Kind::Thin &&
           !infix()) {
        if (!parts.empty() && (wordIs("left") || wordIs("right"))) break;
        const std::size_t before = index;
        Box box = piece();
        const bool fence = box.stretch;
        if (!box.empty) parts.push_back(std::move(box));
        if (index == before || fence) break;
    }
    return hpack(parts);
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
        // ^C` 의 ` 는 첨자 밖이다(한글 2024: 첨자 폭은 C 뿐).
        // ~ 는 첨자 안에 남는다(한글 자동화로 만든 수식: x^2~y 의 ~y 는 위첨자라 1690×1168).
        if (!parts.empty() && kind() == Kind::Thin) break;
        // pi^2cos 의 cos 는 위첨자 밖이다. 첨자가 이미 있으면 함수 앞에서 멈춘다.
        if (!parts.empty() && kind() == Kind::Word) {
            peelCommand();
            if (uprightPrefix(current().text) > 0) break;
            // sqrt 같은 구조 명령도 첨자 밖이다(한글 2024: `^3sqrt{…} 의 첨자는 3 뿐이다).
            const std::string& next = current().text;
            if (leadingCommand(next, false) > leadingCommand(next, true)) break;
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
            box.items.push_back(makeItem(Item::Kind::Glyph, op.cp, integral, 0, 0, box.width, box.height));
            return box;
        });
        Box sub;
        Box sup;
        const Box* subPtr = nullptr;
        const Box* supPtr = nullptr;
        if (takeLimit(false)) {
            sub = atScript([&] { return bracedArg(); });
            subPtr = &sub;
        }
        if (takeLimit(true)) {
            sup = atScript([&] { return bracedArg(); });
            supPtr = &sup;
        }
        if (subPtr || supPtr) return intStack(glyph, subPtr, supPtr);
        glyph.intOp = true;
        return glyph;
    }
    // 사용자 영역 기호가 없는 큰 연산자는 그 글자 코드 그대로 SUM 처럼 잰다.
    const char32_t summed = op.small ? 0 : sumGlyph(op.name) != 0 ? sumGlyph(op.name) : op.cp;
    if (summed != 0) {
        const std::int32_t opEm = mulDiv(em, 180, 100);
        Box glyph = atEm(opEm, [&] {
            Box box;
            box.width = symbolAdvance(summed);
            box.height = equationHeightPx(std::max(opEm, 0)) * kHwpPerPx;
            box.items.push_back(makeItem(Item::Kind::Glyph, op.cp, summed, 0, 0, box.width, box.height));
            return box;
        });
        Box sub;
        Box sup;
        const Box* subPtr = nullptr;
        const Box* supPtr = nullptr;
        if (takeLimit(false)) {
            sub = atScript([&] { return bracedArg(); });
            subPtr = &sub;
        }
        if (takeLimit(true)) {
            sup = atScript([&] { return bracedArg(); });
            supPtr = &sup;
        }
        if (subPtr || supPtr) return sumStack(glyph, subPtr, supPtr);
        glyph.sumOp = true;
        glyph.headClass = 1;
        return glyph;
    }
    // SMALLSUM 같은 작은 기호는 한계를 받는 연산자가 아니라 글자 하나다(한글 2024).
    Box box = metricsOf();
    box.width = advanceOf(*symbol, op.cp);
    box.items.push_back(makeItem(Item::Kind::Glyph, op.cp, op.cp, 0, 0, box.width, box.height));
    const int atom = symbolAtomClass(op.cp);
    box.headClass = atom;
    box.tailClass = atom;
    return box;
}

// OVERBRACE{가}{나} 와 UNDERBRACE{가}{나}. 첫 인자는 아래 칸, 둘째 인자는 위 칸이다.
// OVERBRACE 는 둘째(위 글)를, UNDERBRACE 는 첫째(아래 글)를 68%로 잰다. 나머지는 본문 크기다.
// 폭은 두 칸과 괄호 조각 셋(왼쪽 끝, 가운데, 오른쪽 끝) 폭의 합 가운데 큰 값이다. 10pt 조각은 1050씩이라 3150보다 좁지 않다.
// 괄호 높이는 가운데 조각 높이의 35%다. 아래 칸이 낮은 글자만이면(lowLetters) 괄호와 겹친다:
// OVERBRACE 는 글자 크기의 15%, UNDERBRACE 는 5%. 기준선은 본문 크기 칸의 것이다.
// 10pt overbrace{a+b}{n} 은 3150×1991(341+675+975), overbrace{a}{n} 은 150 겹쳐 1841이다.
Box Parser::brace(bool under) {
    const std::int32_t small = mulDiv(em, 68, 100);
    Box low = under ? atEm(small, [&] { return bracedArg(); }) : bracedArg();
    Box high = under ? bracedArg() : atEm(small, [&] { return bracedArg(); });
    const char32_t left = under ? 0xE10A : 0xE106;
    const char32_t middle = under ? 0xE10B : 0xE107;
    const char32_t right = under ? 0xE10D : 0xE109;
    const std::int32_t pieces = symbolAdvance(left) + symbolAdvance(middle) + symbolAdvance(right);
    const std::int32_t braceH = mulDiv(metricsOf().height, 35, 100);
    const std::int32_t overlap = lowLetters(low) ? mulDiv(em, under ? 5 : 15, 100) : 0;
    const std::int32_t highH = high.empty ? 0 : high.height;
    const std::int32_t lowH = low.empty ? 0 : low.height;
    Box out;
    out.width = std::max({pieces, low.width, high.width});
    out.height = highH + braceH - overlap + lowH;
    const std::int32_t lowY = highH + braceH - overlap;
    if (!high.empty) place(out, high, (out.width - high.width) / 2, 0);
    // 아래 칸이 비어 겹친 만큼은 상자 밖이라 띠를 상자 안에서 자른다.
    out.items.push_back(makeItem(Item::Kind::Accent, under ? 0x23DF : 0x23DE, middle, 0, highH, out.width,
                                 std::min(braceH, out.height - highH)));
    if (!low.empty) place(out, low, (out.width - low.width) / 2, lowY);
    // 빈 칸은 높이 없이 기준선만 글자 크기의 35% 자리다(한글 자동화로 만든 수식: a underbrace a 는 1453).
    const std::int32_t emptyBase = em > 0 ? em * 35 / 100 : 0;
    out.baseline = under ? (high.empty ? emptyBase : high.baseline) : lowY + (low.empty ? emptyBase : low.baseline);
    out.axisBottom = (em > 0 ? em * 35 / 100 : 0) - out.baseline;
    out.fontEm = em;
    // box 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다(x + overbrace{a}{n} 4855, overbrace{a}{n} + x 4715).
    out.headClass = 1;
    out.tailClass = 0;
    return out;
}

Box Parser::emitWord(std::string_view word, bool spaceAfter, bool loose, bool functions) {
    std::vector<Box> parts;
    // 함수 이름은 수식 글꼴의 곧게 선 ASCII 다. 바탕이 아니다.
    const auto upright = [&](std::string_view text) {
        const bool saved = romanMode;
        romanMode = true;
        Box box = measureText(*symbol, text);
        romanMode = saved;
        return box;
    };
    // "or " 의 띄어쓰기는 ` 와 같은 얇은 빈칸이다(10pt a or b 는 2200).
    const auto spacedOr = [&](int n) {
        return n == static_cast<int>(word.size()) && tableFunctionPrefix(word) == 0 &&
               tableFunctionPrefix(word, spaceAfter) == n;
    };
    const auto thin = [&] {
        Box box = styleSwitch();
        box.width = equationBlank(em) / 4;
        box.headClass = 1;
        box.tailClass = 1;
        return box;
    };
    // 함수가 설 수 없는 자리(글자 낱말·이름 기호 뒤)의 표 함수는 첫 글자를 기울임 글자로 떼고, 나머지 이름에 빈칸을
    // 붙여 대소문자를 가리는 표에서 함수를 다시 찾는다. 찾으면 그만큼 곧은 글자다(한글 2024:
    // x dagger for 는 f 기울임에 곧은 or, x dagger acos 는 a 와 곧은 cos, x dagger sin 은 모두 기울임).
    if (!functions) {
        const int n = tableFunctionPrefix(word, spaceAfter);
        if (n > 1) {
            parts.push_back(measureText(textFont(), word.substr(0, 1)));
            const std::string rest = std::string(word.substr(1, static_cast<std::size_t>(n - 1))) + ' ';
            const KeywordHit hit = longestKeyword(kKeywordsExact, rest);
            const int m = keywordIsFunction(hit.value) ? std::min(hit.length, n - 1) : 0;
            if (m > 0) parts.push_back(upright(word.substr(1, static_cast<std::size_t>(m))));
            if (n - 1 > m) {
                parts.push_back(measureText(textFont(), word.substr(static_cast<std::size_t>(1 + m),
                                                                    static_cast<std::size_t>(n - 1 - m))));
            }
            if (spacedOr(n)) parts.push_back(thin());
            if (static_cast<int>(word.size()) > n) {
                parts.push_back(measureText(textFont(), word.substr(static_cast<std::size_t>(n))));
            }
            return hpack(parts);
        }
    }
    const int n = functions ? uprightPrefix(word, spaceAfter, loose) : 0;
    if (n > 0) {
        Box box = upright(word.substr(0, static_cast<std::size_t>(n)));
        box.fnHead = true;
        parts.push_back(std::move(box));
        if (spacedOr(n)) parts.push_back(thin());
    }
    if (static_cast<int>(word.size()) > n) parts.push_back(measureText(textFont(), word.substr(static_cast<std::size_t>(n))));
    return hpack(parts);
}

Box Parser::prefix() {
    if (kind() == Kind::LBrace) return readGroup();
    if (kind() == Kind::Thin) {
        take();
        // ` 는 보통 글자다. 11pt 너비는 137이고, 뒤의 : 와는 관계 간격 231을 둔다.
        // 빈칸 노드는 폭 0 의 rm 처럼 높이가 글자 크기다(한글 2024).
        Box box = styleSwitch();
        box.width = equationBlank(em) / 4;
        box.headClass = 1;
        box.tailClass = 1;
        return box;
    }
    if (kind() == Kind::Wide) {
        take();
        // ~ 도 보통 글자다. 11pt 너비는 550이고, 뒤의 : 와는 231을 둔다.
        Box box = styleSwitch();
        box.width = equationBlank(em);
        box.headClass = 1;
        box.tailClass = 1;
        return box;
    }
    if (kind() == Kind::Quote) {
        const std::string text = current().text;
        take();
        // 따옴표 글도 수식 글자처럼 잰다(한글 2024가 기울임 코드다).
        // 안의 공백은 사라지지 않고 ` 와 같은 얇은 빈칸이다. 그 빈칸은 높이가 글자 크기다(10pt "a b" 는 1000).
        // 공백이 없으면 글자 높이 그대로다("가一" 10pt 는 975, 한글 자동화로 만든 수식).
        std::vector<Box> parts;
        std::size_t start = 0;
        while (start <= text.size()) {
            const std::size_t space = text.find(' ', start);
            const std::string_view chunk =
                std::string_view(text).substr(start, space == std::string::npos ? std::string::npos : space - start);
            if (!chunk.empty()) parts.push_back(measureText(textFont(), chunk));
            if (space == std::string::npos) break;
            Box thin = styleSwitch();
            thin.width = equationBlank(em) / 4;
            thin.headClass = 1;
            thin.tailClass = 1;
            parts.push_back(thin);
            start = space + 1;
        }
        return hpack(parts);
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
                box.items.push_back(makeItem(Item::Kind::Glyph, joined, joined, 0, 0, width, box.height));
                // -> 는 rarrow 와 같다. 앞뒤 글자와의 간격은 0이다. 11pt n -> INF 는 2700이다.
                // +- 는 이항이고, 나머지 묶음 연산자는 관계식이다.
                int atom = 4;
                if (text == "+-" || text == "!=" || text == "==") atom = 3;
                else if (text == "->" || text == "<-" || text == "<->" || text == "'" || text == "''") atom = 1;
                box.headClass = atom;
                box.tailClass = atom;
                return box;
            }
        }
        const char32_t mapped = symbolCode(text);
        const EquationFont& face = symbol->unitsPerEm > 0 ? *symbol : textFont();
        // ≤ 처럼 기호를 바로 넣은 글자도 그 기호의 간격 종류를 쓴다.
        if (mapped == 0 && static_cast<unsigned char>(text[0]) >= 0x80) {
            std::size_t at = 0;
            char32_t cp = 0;
            if (nextCp(text, at, cp) && at == text.size() && !isHangulText(cp)) {
                Box box = measureText(face, text);
                const int atom = symbolAtomClass(cp);
                box.headClass = atom;
                box.tailClass = atom;
                return box;
            }
        }
        if (mapped != 0) {
            Box box = metricsOf();
            box.width = advanceOf(face, mapped);
            box.items.push_back(makeItem(Item::Kind::Glyph, drawnSymbol(mapped), mapped, 0, 0, box.width, box.height));
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
        // 바탕도 첨자도 없으면 아무것도 두지 않는다(^{} 혼자는 빈 목록 500×1000).
        if (arg.empty) return arg;
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
    if (kind() != Kind::Word) {
        Box box;
        box.empty = true;
        return box;
    }
    const std::string word(commandAlias(current().text));
    // lim 과 Lim 은 대소문자를 가리는 표에만 있다. LIM 은 글자다(한글 2024).
    const std::uint32_t limValue = wholeKeyword(word);
    if (limValue == 0xE10048 || limValue == 0xE10049) {
        take();
        const bool capital = limValue == 0xE10049;
        const std::int32_t outer = em;
        const std::string_view name = capital ? std::string_view("Lim") : std::string_view("lim");
        Box box = atEm(mulDiv(em, 120, 100), [&] {
            Box measured = measureText(*roman, name, Item::Face::Roman);
            // 크기는 바탕 글꼴 폭이지만 그리기는 수식 글꼴의 곧은 ASCII L·i·m 을 같은 크기로 이어 쓴다
            // (한글이 그린 PDF: 10pt Lim 은 HancomEQN 12pt L, i, m 이 0, 804, 1140 에 선다. 잰 폭은 825, 375).
            if (symbol->unitsPerEm > 0) {
                std::int32_t x = 0;
                for (Item& item : measured.items) {
                    const std::int32_t design = designOf(*symbol, item.cp);
                    if (design < 0) break;
                    const std::int32_t advance = equationGlyphPx(*symbol, item.cp, design, em) * kHwpPerPx;
                    item.x = x;
                    item.width = advance;
                    item.face = Item::Face::Symbol;
                    item.hwpCode = item.cp;
                    item.italic = false;
                    // lim 은 따로 그린다(올리지 않는다). 글꼴 높이가 글자 크기 120% 인 TA_BASELINE 이다.
                    item.raise = 0;
                    x += advance;
                }
            }
            return measured;
        });
        box.limits = true;
        box.fontEm = outer;
        return box;
    }
    if (eqFold(word, "rm")) {
        take();
        romanMode = true;
        // 글꼴 전환은 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다
        // (한글 2024: = it x 는 = 뒤 관계 간격, P it ( 는 간격 없음).
        Box box = styleSwitch();
        box.headClass = 1;
        box.tailClass = 0;
        return box;
    }
    if (eqFold(word, "it")) {
        take();
        romanMode = false;
        // 글꼴 전환은 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다
        // (한글 2024: = it x 는 = 뒤 관계 간격, P it ( 는 간격 없음).
        Box box = styleSwitch();
        box.headClass = 1;
        box.tailClass = 0;
        return box;
    }
    if (eqFold(word, "bold")) {
        take();
        // 폭은 그대로다. 그 뒤(묶음 안) 글자를 굵게 그린다.
        boldMode = true;
        Box box;
        box.empty = true;
        return box;
    }
    // MIT 는 낱말 표에 있지만 노드를 만들지 않는다. 앞의 rm 도 그대로 이어진다
    // (한글 2024: rm abc mit ef 의 e·f 는 곧은 글자 폭).
    if (eqFold(word, "mit")) {
        take();
        Box box;
        box.empty = true;
        return box;
    }
    // UNDEROVER 는 바로 뒤 하나를 본문으로 받고, 문서 상자는 그 본문 크기다. 뒤에 아무것도 없으면 해석이 끝난다
    // (한글 2024: underover{a}{b}{c} 의 underover 노드는 a 를 품는다. 한글 자동화로 만든 수식:
    // underover{a}{b}{c} 는 a b c 의 1425, a underover 는 a, underover 혼자는 1×1).
    if (eqFold(word, "underover")) {
        take();
        skipSpace();
        if (stopped()) {
            tokens.resize(index);
            aborted = true;
            Box box;
            box.empty = true;
            return box;
        }
        // 본문이 비어도 같다(underover from a 는 1×1).
        Box base = bracedArg();
        if (base.empty) aborted = true;
        return base;
    }
    // 앞에 붙을 연산자가 없는 FROM·TO 에서 해석이 끝난다. 묶음 밖까지 그 뒤는 모두 버린다
    // (한글 자동화로 만든 수식: abc from a + b 와 {abc from a} + b 는 abc, x^{from a} 는 x 다).
    if (limitWordHere()) {
        tokens.resize(index);
        Box box;
        box.empty = true;
        return box;
    }
    // BREVE 는 꾸밈 이름이지만 그 글자를 그리지 않는다. 안쪽 그대로이고 간격 종류도 안쪽 것이다
    // (한글 자동화로 만든 수식: breve{a} 는 a 와 같은 525×975, a breve{+} b 는 a+b 와 같은 2080).
    if (eqFold(word, "breve")) {
        take();
        return bracedArg();
    }
    // PHANTOM 은 안쪽 크기만 두고 그리지 않는다. box 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다
    // (한글 자동화로 만든 수식: phantom{x}+y 는 2090, x+phantom{y} 는 x+y 와 같은 2230).
    if (eqFold(word, "phantom")) {
        take();
        Box box = slotted(bracedArg());
        box.items.clear();
        box.empty = false;
        box.headClass = 1;
        box.tailClass = 0;
        return box;
    }
    if (eqFold(word, "overbrace") || eqFold(word, "underbrace")) {
        const bool under = eqFold(word, "underbrace");
        take();
        return brace(under);
    }
    if (const Accent* accent = accentOf(word)) {
        take();
        return this->accent(slotted(bracedArg()), *accent);
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
        const Box inner = slotted(bracedArg());
        // BOX 는 낱말 표의 꾸밈(0xC00076)이라 폭은 bar 와 같다: 안쪽과 막대 기호 중 큰 폭에 기호 폭의 20% 를 더한다
        // (한글 자동화로 만든 수식: box{i} 와 box{a} 는 630, box{} 는 빈 목록 500 이라 630, 14pt box{i} 는 810).
        // 위 여백은 글자 크기의 5% 를 0.1픽셀로 반올림한 t/10 픽셀의 4.5배, 높이는 글자 높이의 20% 를 더한다
        // (한글 2024, 8~20pt: 10pt 는 79, 195).
        const std::int32_t glyphH = equationHeightPx(std::max(em, 0)) * kHwpPerPx;
        const std::int32_t tenths = (std::max(em, 0) + 75) / 150;
        const std::int32_t barW = symbolAdvance(0xE06D);
        const std::int32_t width = std::max(inner.width, barW) + mulDiv(barW, 20, 100);
        const std::int32_t growY = glyphH / 5;
        const std::int32_t padX = (width - inner.width) / 2;
        const std::int32_t padY = (tenths * 45 + 2) / 4;
        Box framed = inner;
        framed.items.clear();
        place(framed, inner, padX, padY);
        framed.width = width;
        framed.height += growY;
        framed.baseline += padY;
        framed.axisBottom = inner.axisBottom - padY;
        framed.empty = false;
        // box 노드는 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다(box{x} + box{~~} 의 + 앞은 붙는다).
        framed.headClass = 1;
        framed.tailClass = 0;
        // 테두리 네 변.
        const std::int32_t thick = ruleThickness();
        framed.items.push_back(rule(0, 0, framed.width, thick));
        framed.items.push_back(rule(0, framed.height - thick, framed.width, thick));
        framed.items.push_back(rule(0, 0, thick, framed.height));
        framed.items.push_back(rule(framed.width - thick, 0, thick, framed.height));
        return framed;
    }
    if (eqFold(word, "not")) {
        take();
        // NOT 은 다음 토큰 하나를 그대로 받는다. { 도 묶음이 아니라 글자 { 이고, 짝 잃은 } 는 버려진다
        // (한글 2024: not{} 의 인자는 U+E04B. 한글 자동화로 만든 수식: not{=} 은 { 와 = 의 1425).
        skipSpace();
        if (kind() == Kind::LBrace) {
            tokens[index].kind = Kind::Symbol;
            tokens[index].text = "{";
            rawOpens.push_back(braceDepth);
        }
        Box arg = slotted(bracedArg());
        Box slash = measureText(*symbol, "/");
        Box out = arg;
        // 크기는 안쪽 그대로다. 사선은 겹쳐 그릴 뿐이다(한글 2024: a not b 는 975, a not 은 1025).
        out.width = arg.width;
        out.height = arg.height;
        // not 노드는 box 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다. 안쪽 기호의 간격을 물려받지 않는다
        // (한글 2024: not = not in 은 붙는다. 한글 자동화로 만든 수식: not{=} 의 { 뒤 = 도 붙어 1425).
        out.headClass = 1;
        out.tailClass = 0;
        // 사선을 글자 위에 겹친다. 글자 노드가 아니라 not 이 그리는 장식이다.
        out.items.clear();
        place(out, arg, (out.width - arg.width) / 2, out.height - arg.height);
        Box stroke = slash;
        for (Item& item : stroke.items) item.kind = Item::Kind::Accent;
        place(out, stroke, (out.width - slash.width) / 2, out.height - slash.height);
        return out;
    }
    if (eqFold(word, "bigg")) {
        take();
        // 폭은 그 기호 그대로이고, 높이는 같은 줄 전체로 늘어난다(한글 2024: 분수 사이 / 는 525×2250).
        // 글자가 없으면 폭이 없다(한글 자동화로 만든 수식: bigg 혼자는 1×1000). 여는 쪽으로 받아 코드 0 폭을 피한다.
        Box side = takeDelim(true);
        side.open = false;
        side.stretch = false;
        side.middle = true;
        // 노드 높이는 글자 크기보다 낮지 않다(한글 2024: bigg ( 는 375×1000, a bigg ( b 도 1000).
        if (side.height < em) {
            const Box tall = styleSwitch();
            side.height = tall.height;
            side.baseline = tall.baseline;
            side.axisBottom = tall.axisBottom;
            refitFence(side);
        }
        side.headClass = 1;
        side.tailClass = 1;
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
        // choose 와 같은 괄호 친 두 줄 행렬이다(한글 2024: binom n r 은 n choose r 과 같은 1350×2100).
        Box table = addFences(packTable({{upper}, {lower}}, true), 0xE07D, 0xE080, U'(', U')');
        table.headClass = 1;
        table.tailClass = 0;
        return table;
    }
    if (eqFold(word, "rel") || eqFold(word, "buildrel")) {
        const bool build = eqFold(word, "buildrel");
        take();
        // 인자가 하나도 없으면 거기서 해석이 끝나고, 아무것도 못 만들었으면 1×1 이다(한글 자동화로 만든 수식: rel 혼자).
        skipSpace();
        if (stopped()) {
            tokens.resize(index);
            aborted = true;
            Box box;
            box.empty = true;
            return box;
        }
        // 화살표 자리는 NOT 처럼 다음 토큰 하나를 그대로 받는다. { 도 글자 { 다(한글 자동화로 만든 수식:
        // rel {a} 는 { 위에 a, 아래 빈 목록의 1025×2950).
        if (kind() == Kind::LBrace) {
            tokens[index].kind = Kind::Symbol;
            tokens[index].text = "{";
            rawOpens.push_back(braceDepth);
        }
        // 위·아래 글 자리에서 맨 바깥의 짝 없는 } 를 만나면 그것을 먹고 해석이 끝난다
        // (rel{->}{a}{b} 는 { 위에 -> 를 얹은 1550×2950 이고 {a}{b} 는 버려진다).
        const auto argument = [&] {
            skipSpace();
            if (braceDepth == 0 && kind() == Kind::RBrace) {
                take();
                tokens.resize(index);
                return emptySlot();
            }
            return bracedArg();
        };
        const Box arrow = bracedArg();
        const Box over = argument();
        const Box under = build ? emptySlot() : argument();
        return relation(arrow, over, under);
    }
    if (eqFold(word, "color")) {
        take();
        skipSpace();
        // {R,G,B} 를 읽어 다음 묶음의 글자색으로 쓴다. 폭은 바꾸지 않는다.
        std::uint32_t next = color;
        if (kind() == Kind::LBrace) {
            take();
            std::uint32_t channels[3] = {0, 0, 0};
            int got = 0;
            while (kind() != Kind::End && kind() != Kind::RBrace) {
                if (kind() == Kind::Number && got < 3) {
                    channels[got++] = static_cast<std::uint32_t>(std::min(255, std::atoi(current().text.c_str())));
                }
                take();
            }
            if (kind() == Kind::RBrace) take();
            if (got == 3) next = (channels[0] << 16) | (channels[1] << 8) | channels[2];
        } else if (kind() == Kind::Word || kind() == Kind::Number || kind() == Kind::Symbol) {
            take();
        }
        const std::uint32_t saved = color;
        color = next;
        Box body = slotted(bracedArg());
        color = saved;
        // color 노드는 box 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다
        // (한글 자동화로 만든 수식: x + color{255,0,0}{a} + y 는 3720, 빈 인자는 빈 목록이라 3695).
        body.headClass = 1;
        body.tailClass = 0;
        return body;
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
        } else {
            // 비율이 없으면 거기서 해석이 끝난다(한글 자동화로 만든 수식: a scale a 는 a).
            tokens.resize(index);
            Box box;
            box.empty = true;
            return box;
        }
        // 한글 2024 는 scale 값을 쓰지 않는다(한글 2024: scale 70, scale 150 모두 원래 크기).
        (void)percent;
        // 인자가 없으면 빈 칸을 두지 않는다(한글 자동화로 만든 수식: a scale 70 {} 는 a 의 525).
        return bracedArg();
    }
    if (eqFold(word, "sup") || eqFold(word, "sub")) {
        const bool sub = eqFold(word, "sub");
        take();
        Box arg = atScript([&] { return bracedArg(); });
        // 바탕도 첨자도 없으면 아무것도 두지 않는다(한글 자동화로 만든 수식: sup 혼자는 빈 목록 500×1000).
        if (arg.empty) return arg;
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
    if (const std::optional<Big> op = bigOp(word)) {
        take();
        return bigOperator(*op);
    }
    if (eqFold(word, "longdiv")) {
        take();
        return longDivision();
    }
    if (eqFold(word, "ladder") || eqFold(word, "sladder")) {
        const bool binary = eqFold(word, "sladder");
        take();
        return ladder(binary);
    }
    // EQALIGN 의 중괄호는 따로 묶지 않는다. 안의 줄을 둘러싼 목록에 그대로 펼치고 그 목록을 eqalign 줄로 만든다.
    // 그래서 뒤에 이어진 글은 마지막 줄에, 앞의 글은 첫 줄에 붙는다(한글 2024:
    // x + eqalign{a#b} + y 는 x + a 와 b + y 두 줄, eqalign{a#b} eqalign{c#d} 는 a, b c, d 세 줄).
    if (eqFold(word, "eqalign")) {
        take();
        skipSpace();
        // 중괄호가 없으면 거기서 해석이 끝난다(한글 자동화로 만든 수식: a eqalign a 는 a).
        if (kind() != Kind::LBrace) {
            tokens.resize(index);
            Box box;
            box.empty = true;
            return box;
        }
        int depth = 0;
        std::size_t close = index;
        for (; close < tokens.size(); ++close) {
            if (tokens[close].kind == Kind::LBrace) ++depth;
            if (tokens[close].kind == Kind::RBrace && --depth == 0) break;
        }
        if (close < tokens.size()) tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(close));
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(index));
        eqAlignRows = true;
        matrixRows = false;
        Box box;
        box.empty = true;
        return box;
    }
    if (eqFold(word, "cases") || eqFold(word, "pile") || eqFold(word, "lpile") || eqFold(word, "rpile") ||
        eqFold(word, "matrix") || eqFold(word, "pmatrix") || eqFold(word, "bmatrix") || eqFold(word, "dmatrix")) {
        const std::string name = word;
        take();
        skipSpace();
        const bool savedMatrix = matrixRows;
        const bool savedAlign = eqAlignRows;
        const int savedTableAlign = tableAlign;
        // 중괄호가 없으면 pile 계열은 노드 없이, 행렬·cases 는 빈 칸 하나로 만든 뒤 거기서 해석이 끝난다
        // (한글 자동화로 만든 수식: a pile a 는 a, a matrix a 는 a 와 빈 목록의 1025, cases 는 1025×1000).
        if (kind() != Kind::LBrace) {
            tokens.resize(index);
            if (eqFold(name, "pile") || eqFold(name, "lpile") || eqFold(name, "rpile")) {
                aborted = true;
                Box box;
                box.empty = true;
                return box;
            }
        }
        tableBody = true;
        eqAlignRows = false;
        // cases 와 lpile 은 왼쪽, rpile 은 오른쪽, 나머지는 가운데 맞춤이다.
        tableAlign = eqFold(name, "cases") || eqFold(name, "lpile") ? -1 : eqFold(name, "rpile") ? 1 : 0;
        Box inner;
        if (kind() == Kind::LBrace) inner = readGroup();
        else inner = bracedArg();
        inner = slotted(inner);
        tableBody = false;
        matrixRows = savedMatrix;
        eqAlignRows = savedAlign;
        tableAlign = savedTableAlign;
        if (eqFold(name, "cases")) {
            // f(x)= {cases ...} 의 = 뒤는 관계 간격이다. 11pt 는 231이다.
            Box box = addFences(inner, 0xE079, 0, U'{', 0);
            box.headClass = 1;
            return box;
        }
        // 행렬·pile 은 box 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다
        // (한글 2024: ) + matrix 의 + 뒤 20%. 한글 자동화로 만든 수식: pile{a#b} + x 는 2090, x + pile{a#b} 는 2230).
        Box table;
        if (eqFold(name, "pmatrix")) table = addFences(inner, 0xE07D, 0xE080, U'(', U')');
        else if (eqFold(name, "bmatrix")) table = addFences(inner, 0xE101, 0xE105, U'[', U']');
        else if (eqFold(name, "dmatrix")) table = addFences(inner, 0xE101, 0xE101, U'|', U'|');
        else table = inner;
        table.headClass = 1;
        table.tailClass = 0;
        return table;
    }
    // 함수 이름 그 자체는 기호보다 먼저다(deg 는 ° 가 아니라 곧은 글자 deg).
    const bool spaceAfter = index + 1 < tokens.size() && tokens[index + 1].kind == Kind::Space;
    // 띄어 쓴 글자 낱말에 이어진 낱말은 한 덩어리라 det, arc 로 시작해도 기울임이다(x det, a det).
    // 글자로 끝나는 낱말이다. Prime 은 함수 Pr 뒤에 글자 ime 가 붙어 글자로 끝난다(한글 2024:
    // VARTHETA Prime dagger forall 의 for 는 함수가 아니다).
    const auto plainWordAt = [&](std::size_t at) {
        const std::string& text = tokens[at].text;
        return tokens[at].kind == Kind::Word && leadingCommand(text) == 0 &&
               uprightPrefix(text) < static_cast<int>(text.size()) && symbolCode(text) == 0;
    };
    const bool loose = !(index >= 2 && tokens[index - 1].kind == Kind::Space && plainWordAt(index - 2));
    // 띄어 쓴 글자 낱말 뒤에 이름 기호가 오면 그 뒤 낱말은 함수가 아니다(한글 2024:
    // x dagger sin 과 x alpha forall 은 기울임이고 얇은 빈칸도 없다. a b sin 과 alpha sin 은 함수다).
    bool functions = true;
    for (std::size_t at = index; at >= 2 && tokens[at - 1].kind == Kind::Space && tokens[at - 2].kind == Kind::Word;
         at -= 2) {
        if (symbolCode(tokens[at - 2].text) != 0) continue;
        functions = !(at != index && plainWordAt(at - 2));
        break;
    }
    if (functions && uprightPrefix(word, spaceAfter, loose) == static_cast<int>(word.size())) {
        take();
        return emitWord(word, spaceAfter, loose);
    }
    if (const char32_t cp = symbolCode(word)) {
        take();
        Box box = metricsOf();
        box.width = advanceOf(*symbol, cp);
        char32_t code = equationGlyph(cp, false);
        if (code == 0) code = equationOperator(cp);
        if (code == 0) code = cp;
        box.items.push_back(makeItem(Item::Kind::Glyph, drawnSymbol(cp), code, 0, 0, box.width, box.height));
        // INF, 그리스 문자는 보통 글자다. = INF 는 11pt 에서 2106이다.
        // LEQ 는 관계, CIRC 는 이항이다. CDOTS 같은 점은 간격을 더하지 않는다.
        const int atom = symbolAtomClass(cp);
        box.headClass = atom;
        box.tailClass = atom;
        box.wordTail = atom != 5 && atom != 6;
        return box;
    }
    // lex 는 <= 와 x, gex 는 >= 와 x 다. 11pt 는 둘 다 1881이다.
    // le 하나만 있어도 <= 다(한글 2024).
    if (word.size() >= 2 && (eqFold(std::string_view(word).substr(0, 2), "le") ||
                            eqFold(std::string_view(word).substr(0, 2), "ge"))) {
        const bool ge = word[0] == 'g' || word[0] == 'G';
        take();
        Box rel = metricsOf();
        rel.width = symbolAdvance(ge ? 0x2265 : 0x2264);
        rel.items.push_back(
            makeItem(Item::Kind::Glyph, ge ? 0x2265 : 0x2264, ge ? 0x2265 : 0x2264, 0, 0, rel.width, rel.height));
        rel.headClass = 4;
        rel.tailClass = 4;
        Box rest = emitWord(std::string_view(word).substr(2));
        return hpack({rel, rest});
    }
    take();
    return emitWord(word, spaceAfter, loose, functions);
}

Box Parser::piece() {
    // `^3sqrt 의 ^3 은 근호 지수가 아니라 앞 빈칸의 위첨자다(한글 2024).
    Box base = prefix();
    Box sub;
    Box sup;
    bool hasSub = false;
    bool hasSup = false;
    int guard = 0;
    while (guard++ < 8) {
        // SUP·SUB 명령도 ^·_ 처럼 같은 첨자 노드로 붙는다(한글 2024: x SUP 2 SUB i).
        const auto opAt = [&](std::size_t i) {
            if (i >= tokens.size()) return 0;
            if (tokens[i].kind == Kind::Sup) return 1;
            if (tokens[i].kind == Kind::Sub) return -1;
            if (tokens[i].kind == Kind::Word && eqFold(tokens[i].text, "sup")) return 2;
            if (tokens[i].kind == Kind::Word && eqFold(tokens[i].text, "sub")) return -2;
            // lim 의 FROM·TO 는 _·^ 와 같다(한글 자동화로 만든 수식: lim from x a 는 lim _{x} a 와 같은 2325×1875,
            // Lim to a b 는 위첨자를 버린 Lim b 2700).
            if (base.limits && tokens[i].kind == Kind::Word && wholeKeyword(tokens[i].text) == kTo) return 2;
            if (base.limits && tokens[i].kind == Kind::Word && wholeKeyword(tokens[i].text) == kFrom) return -2;
            return 0;
        };
        if (kind() == Kind::Space) {
            std::size_t i = index;
            while (i < tokens.size() && tokens[i].kind == Kind::Space) ++i;
            if (opAt(i) != 0) {
                index = i;
            } else {
                break;
            }
        }
        const int op = opAt(index);
        if (op == 0) break;
        if (scriptSide > 0 && op < 0) break;
        if (scriptSide < 0 && op > 0) break;
        const bool isSub = op < 0;
        const bool named = op == 2 || op == -2;
        take();
        const int savedSide = scriptSide;
        scriptSide = isSub ? -1 : 1;
        Box arg = atScript([&] { return named ? bracedArg() : scriptRun(); });
        scriptSide = savedSide;
        if (isSub) {
            sub = std::move(arg);
            hasSub = true;
        } else {
            sup = std::move(arg);
            hasSup = true;
        }
    }
    // 닫는 울타리의 첨자는 울타리가 본문 높이가 된 뒤에 붙는다.
    // LEFT ( ... RIGHT ) ^{4x} 의 4x 는 글자 높이가 아니라 늘어난 괄호 위에 있다. _{a} ^{b} 도 같다.
    if (base.stretch && !base.open && (hasSup || hasSub)) {
        heldScripts.push_back(HeldScripts{std::move(sub), std::move(sup), hasSub, hasSup});
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
                const std::int32_t top = std::min(stacked.axisBottom, slot.axisBottom);
                const std::int32_t bot =
                    std::max(stacked.axisBottom + stacked.height, slot.axisBottom + slot.height);
                // 적분 상자와 본문 묶음을 축에 맞춰 나란히 둔다.
                Box joined;
                place(joined, stacked, 0, stacked.axisBottom - top);
                place(joined, slot, stacked.width, slot.axisBottom - top);
                stacked.items = std::move(joined.items);
                stacked.width += slot.width;
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
        const std::size_t mark = index;
        skipSpace();
        // {a} over {b} 의 묶음은 분모 앞의 분자다. = 앞에 온 묶음만 lim 오른쪽 칸으로 들어간다.
        if (kind() == Kind::LBrace && !groupBeforeInfix()) {
            slot = readGroup();
            slotPtr = &slot;
        } else {
            // 묶음이 없으면 공백을 돌려 둔다. lim_N->inf 1 over N 의 1 은 lim 과 다른 항이다.
            index = mark;
        }
        // lim 의 위첨자는 읽고 버린다(한글 자동화로 만든 수식: lim ^{n} a 는 lim a 와 같은 2325×1200).
        return limBox(base, hasSub ? &sub : nullptr, nullptr, slotPtr);
    }
    if (!hasSub && !hasSup) return base;
    // 이항·관계·쉼표 글자는 첨자를 받지 않는다. 첨자는 그 뒤에 바탕 없이 따로 서고, 비었으면 사라진다
    // (한글 2024: x +_{n} a 의 첨자 노드는 + 와 a 사이. 한글 자동화로 만든 수식: x+_{} a 는 x+a 의 2230).
    const bool operatorGlyph = base.items.size() == 1 && base.items.front().kind == Item::Kind::Glyph &&
                               base.headClass == base.tailClass &&
                               (base.headClass == 3 || base.headClass == 4 || base.headClass == 7);
    if (operatorGlyph) {
        const bool realSub = hasSub && !sub.empty;
        const bool realSup = hasSup && !sup.empty;
        if (!realSub && !realSup) return base;
        return hpack({base, loneScript(realSub ? &sub : nullptr, realSup ? &sup : nullptr)});
    }
    return corners(base, hasSub ? &sub : nullptr, hasSup ? &sup : nullptr);
}

Box Parser::term() {
    // 중괄호 묶음은 그 자체로 한 항이다. ={pi} over {2} 의 분자는 pi 이고 등호는 밖에 남는다.
    // 중괄호가 없으면 a+b over c 처럼 공백 전까지가 분자다.
    if (kind() == Kind::LBrace) return piece();
    std::vector<Box> parts;
    int guard = 0;
    while (guard++ < 64 && !stopped() && kind() != Kind::Space && kind() != Kind::LBrace && !infix()) {
        // 늘어나는 울타리는 따로 한 항이다. 붙여 쓴 LEFT(SUM… 도 울타리를 펴려면 안쪽과 나눠야 한다.
        if (!parts.empty() && (wordIs("left") || wordIs("right"))) break;
        const std::size_t before = index;
        Box box = piece();
        const bool fence = box.stretch;
        if (!box.empty) parts.push_back(std::move(box));
        if (index == before || fence) break;
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
    // 모은 항을 한 상자로 묶는다: over·atop·choose 를 합치고, 울타리를 늘리고, 가로로 붙인다.
    const auto finish = [&](std::vector<Item> items) {
        std::size_t i = 0;
        while (i < items.size()) {
            if (!items[i].op) {
                ++i;
                continue;
            }
            // 앞 항이 없으면 뒤 항까지 사라진다(한글 2024: over a 와 over {a+b} 는 빈 뿌리 목록).
            if (i == 0) {
                const bool right = i + 1 < items.size() && !items[i + 1].op;
                items.erase(items.begin(), items.begin() + (right ? 2 : 1));
                continue;
            }
            if (i + 1 < items.size() && items[i + 1].op) {
                items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            // 뒤 항이 없으면 빈 목록이다(한글 자동화로 만든 수식: a over 는 a over {} 와 같은 1025×2275).
            if (i + 1 >= items.size()) items.push_back(Item{false, {}, emptySlot()});
            items[i - 1].box = slotted(items[i - 1].box);
            items[i + 1].box = slotted(items[i + 1].box);
            Box merged = eqFold(items[i].name, "choose")
                             ? addFences(packTable({{items[i - 1].box}, {items[i + 1].box}}, true), 0xE07D, 0xE080,
                                         U'(', U')')
                             : fraction(items[i - 1].box, items[i + 1].box, !eqFold(items[i].name, "atop"));
            // choose 는 binom 처럼 앞 글자에게는 보통 글자이고 뒤에는 간격을 두지 않는다(1 + a choose b 는 2905).
            if (eqFold(items[i].name, "choose")) {
                merged.headClass = 1;
                merged.tailClass = 0;
            }
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
        if (std::any_of(boxes.begin(), boxes.end(), [](const Box& box) { return box.middle; })) {
            std::vector<Box> others;
            for (const Box& box : boxes) {
                if (!box.middle) others.push_back(box);
            }
            const Box row = hpack(others);
            for (Box& box : boxes) {
                if (!box.middle || row.empty || row.height <= box.height) continue;
                box.height = row.height;
                box.baseline = row.baseline;
                box.axisBottom = row.axisBottom;
                refitFence(box);
            }
        }
        for (Box& box : boxes) {
            if (box.heldSup < 0 || box.heldSup >= static_cast<int>(heldScripts.size())) continue;
            const HeldScripts& held = heldScripts[static_cast<std::size_t>(box.heldSup)];
            box = corners(box, held.hasSub ? &held.sub : nullptr, held.hasSup ? &held.sup : nullptr);
        }
        return hpack(boxes);
    };
    int guard = 0;
    while (guard++ < 256) {
        // x sin x 의 함수 앞 공백은 얇은 빈칸이다. = sin 처럼 간격이 이미 있으면 더하지 않는다.
        const bool sawSpace = kind() == Kind::Space;
        skipSpace();
        // dy}overdx 의 짝 없는 } 는 없는 셈이다. 묶음 안의 } 는 그 묶음을 닫는다.
        // 두 번째 짝 없는 } 에서는 해석이 끝난다(한글 자동화로 만든 수식: a } b } c 는 a b 의 975).
        const bool rawClose = kind() == Kind::RBrace && !rawOpens.empty() && rawOpens.back() == braceDepth;
        if (rawClose) rawOpens.pop_back();
        if (kind() == Kind::RBrace && (braceDepth == 0 || rawClose)) {
            if (strayClose) {
                tokens.resize(index);
                break;
            }
            strayClose = true;
            take();
            // 첫 짝 없는 } 는 그때까지의 항을 한 묶음으로 닫는다(한글 2024: {a } b} over c 의 분자는 a b,
            // dy}overdx 의 분자는 dy).
            if (!items.empty()) {
                Box closed = finish(std::move(items));
                items.clear();
                items.push_back(Item{false, {}, std::move(closed)});
            }
            continue;
        }
        if (stopped()) break;
        if (infix()) {
            items.push_back(Item{true, current().text, {}});
            take();
            continue;
        }
        const std::size_t before = index;
        const bool group = kind() == Kind::LBrace;
        // 목록 맨 앞의 첨자는 붙을 바탕이 없어 거기서 해석이 끝난다(한글 2024: _{x} a 와
        // lsub{b} a 는 빈 뿌리 목록. 한글 자동화로 만든 수식: x + {_{n} a} + y 는 x + 의 1565).
        if (items.empty() && startsScript()) {
            tokens.resize(index);
            break;
        }
        Box box = term();
        // x cos 는 137을 더한다. pi^2 cos 와 | ln 은 더하지 않는다. 2 sin 처럼 숫자 뒤도 더하지 않는다.
        // 이름으로 부른 기호 뒤는 간격 종류와 상관없이 더한다(dagger sin, CDOTS sin 은 관계 간격 210 에 125 를 더한다).
        // ) ! ? @ 처럼 바로 친 기호와 숫자 뒤는 더하지 않는다.
        if (sawSpace && box.fnHead && !box.prescript && !items.empty() && !items.back().op &&
            !items.back().box.scripted &&
            (items.back().box.wordTail || (items.back().box.tailClass == 1 &&
                                           atomGap(items.back().box, box) == 0 && endsWithLetter(items.back().box)))) {
            // 얇은 빈칸은 함수 앞에 선 ` 노드다. 폭은 글자 크기의 1/8, 높이는 글자 크기다(a sin 은 높이 1000).
            Box thin = styleSwitch();
            thin.width = equationBlank(em) / 4;
            const Box fn = box;
            box = hpack({thin, fn});
            box.headClass = fn.headClass;
            box.tailClass = fn.tailClass;
            box.fnHead = true;
        }
        if (box.prescript && !items.empty() && !items.back().op) {
            items.back().box = attachPrescript(items.back().box, box, box.prescriptSub);
        } else if (!box.empty || group) {
            // 빈 묶음도 분수의 위·아래가 될 수 있어 남겨 둔다. 짝이 없으면 아래에서 버린다.
            items.push_back(Item{false, {}, std::move(box)});
        }
        if (index == before) break;
    }
    return finish(std::move(items));
}

Box Parser::lines() {
    // 표 몸통인지는 이 목록에만 걸린다. 안에서 펼친 eqalign 도 이 목록까지만 간다.
    const bool savedMatrix = matrixRows;
    const bool savedAlign = eqAlignRows;
    matrixRows = tableBody;
    tableBody = false;
    eqAlignRows = false;
    std::vector<std::vector<Box>> rows;
    std::vector<Box> row;
    // 행렬·pile·cases 의 칸은 저마다 목록 노드라 비면 빈 목록 크기다.
    const auto cell = [&] { return matrixRows ? slotted(expr()) : expr(); };
    row.push_back(cell());
    while (kind() == Kind::Amp) {
        take();
        row.push_back(cell());
    }
    rows.push_back(std::move(row));
    while (kind() == Kind::Hash) {
        // 줄은 맨 바깥 목록과 행렬·pile·cases·eqalign 목록에서만 나눈다. 다른 묶음 안의 # 에서는 해석이 끝난다
        // (한글 2024: x+{a#b}+y 는 x+a, {a # b} over z 는 a).
        if (braceDepth > 0 && !matrixRows && !eqAlignRows) {
            tokens.resize(index);
            break;
        }
        take();
        std::vector<Box> next;
        next.push_back(cell());
        while (kind() == Kind::Amp) {
            take();
            next.push_back(cell());
        }
        rows.push_back(std::move(next));
    }
    Box out = finishRows(rows);
    matrixRows = savedMatrix;
    eqAlignRows = savedAlign;
    return out;
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

EquationLayout finishLayout(Parser& parser) {
    Box box = parser.lines();
    // 뿌리 목록이 비면 빈 목록 크기다({} 와 mit 는 500×1000). 해석이 중간에 끝나 아무것도 못 만들었으면 1×1 이고,
    // 폭이 0 인 수식도 폭 1 이다(한글 자동화로 만든 수식: underover 는 1×1, rm 은 1×1000).
    if (box.empty || box.height <= 0) {
        if (parser.aborted) {
            box = Box{};
            box.height = 1;
            box.baseline = 1;
        } else {
            box = parser.emptySlot();
        }
    }
    EquationLayout out;
    out.items = std::move(box.items);
    EquationSize& size = out.size;
    size.width = std::max(box.width, 1);
    size.height = std::max(box.height, 1);
    // 글자 상자가 그대로면 버린 뒤 1을 더한다.
    // 위첨자만, 아래첨자만, 분수도 그 몫에 1을 더한다. 기준선은 글자 크기 35%에서 측정 기준선을 뺀 자리다.
    const std::int32_t letterTop = (parser.em > 0 ? parser.em * 35 / 100 : 0) + size.height / 2;
    const std::int32_t letterBaseline = std::max<std::int32_t>(1, std::min(letterTop, size.height));
    size.baseLine = box.baseline == letterBaseline || box.quotBaseline
                        ? letterBaselinePercent(box.baseline, size.height)
                        : baselinePercent(box.baseline, size.height);
    return out;
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
    font->face = face;
    chwp::layout::fontWinMetrics(face, font->winAscent, font->winDescent);
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
    // 수식 글꼴에 없는 글자는 로만 글꼴로 잰다.
    parser.fallbacks.fill(&romanUse);
    return finishLayout(parser).size;
}

std::int32_t equationGlyphCellHeight(std::int32_t em) { return equationHeightPx(std::max(em, 0)) * kHwpPerPx; }

EquationLayout layoutHangulEquation(std::string_view script, std::int32_t baseUnit, std::string_view symbolFont) {
    if (baseUnit <= 0) baseUnit = 1000;
    const std::string symbolName = resolveFace({canonicalSymbol(symbolFont), "HyhwpEQ", "HancomEQN"});
    const std::string romanName =
        resolveFace({"Haansoft Batang", "한컴바탕", "HCR Batang", "함초롬바탕", "바탕", "Batang"});
    const bool alias = romanName.empty() || romanName == symbolName;
    const std::shared_ptr<const EquationFont> symbol = cachedEquationFont(symbolName);
    const std::shared_ptr<const EquationFont> roman = alias ? symbol : cachedEquationFont(romanName);
    Parser parser(script, baseUnit, *symbol, *roman, !alias, alias);
    // 수식 글꼴에 없는 글자는 한글이 일곱 언어 모두 한컴바탕(en-US "Haansoft Batang")으로 잰다. 그래서
    // 모든 언어가 한컴바탕 크기다(一 10pt 1050×975, 漢字 12pt 2400×1200, ″ 12pt 는 한컴바탕 비트맵 폭).
    parser.fallbacks.fill(roman.get());
    // [Subst Fonts - 언어] 의 HancomEQN → HyhwpEQ. 글자가 HancomEQN 에 없으면 HyhwpEQ 를 먼저 본다.
    std::shared_ptr<const EquationFont> substitute;
    if (symbolName == "HancomEQN") {
        const std::string substituteName = resolveFace({"HyhwpEQ"});
        if (!substituteName.empty()) substitute = cachedEquationFont(substituteName);
        parser.substitute = substitute.get();
    }
    return finishLayout(parser);
}

EquationSize measureHangulEquation(std::string_view script, std::int32_t baseUnit, std::string_view symbolFont) {
    return layoutHangulEquation(script, baseUnit, symbolFont).size;
}

std::vector<std::uint8_t> equationGridTable(std::string_view face, std::int32_t maxHeightPx) {
    const std::shared_ptr<const EquationFont> font = cachedEquationFont(std::string(face));
    if (font->unitsPerEm <= 0 || maxHeightPx <= 0) return {};
    const std::int32_t denom = font->winCell > 0 ? font->winCell : font->unitsPerEm;
    const std::int32_t avg = font->xAvgCharWidth > 0 ? font->xAvgCharWidth : font->unitsPerEm / 2;
    if (avg <= 0) return {};
    std::vector<chwp::layout::FontGridCell> cells;
    std::int32_t maxPpem = 0;
    bool measured = false;
    // equationGlyphPx 가 정사각으로 보는 (높이, lfWidth) 만 잰다. 홀수 높이처럼 정사각이 아닌 크기는 힌팅이 폭을 바꾸지 않는다.
    for (std::int32_t height = 1; height <= maxHeightPx; ++height) {
        const std::int64_t scaled = static_cast<std::int64_t>(height) * avg;
        if (scaled % denom != 0) continue;
        const auto width = static_cast<std::int32_t>(scaled / denom);
        const auto ppem = static_cast<std::int32_t>(static_cast<std::int64_t>(height) * font->unitsPerEm / denom);
        maxPpem = std::max(maxPpem, ppem);
        for (const auto& [cp, design] : font->advanceWidth) {
            const std::int32_t hinted = chwp::layout::fontHintedAdvancePx(font->face, cp, ppem);
            if (hinted < 0) continue;
            measured = true;
            if (hinted != equationSideBearingPx(design, width, avg)) cells.push_back({cp, ppem, hinted});
        }
    }
    if (!measured) return {};
    return chwp::layout::fontGridTable(std::move(cells), maxPpem);
}

}  // namespace chwp::render
