// 글자 바꾸기 → 대문자/소문자 바꾸기, 전각/반각 바꾸기.
//
// 항목 이름은 한글 2024 대화 상자 글이고, 규칙은 한글 2024 에서 같은 글에
// ConvertCase(type 0~5)·ConvertFullHalfWidth(type 0 반각·1 전각, Number·Alpha·Symbol·Gata·HGJamo)를 불러 본 결과다.
//   대문자/소문자: 모두 대문자로, 모두 소문자로, 대/소문자 서로 바꿈, 단어 첫 글자만 대문자로,
//     문장 첫 글자를 대문자로(나머지는 그대로), 문장 첫 글자만 대문자로(나머지 영문자는 소문자로).
//     바꾸는 글자는 ASCII·전각 영문자뿐이다(ï·À 같은 악센트 글자는 그대로).
//   전각/반각: 짝은 유니코드 분해 표의 <wide>·<narrow> 짝(FF00~FFEF 와 U+3000)이고, 갈래·빠지는 글자는
//     한글이 실제로 바꾼 대로다(hangulKind·halfAllowed). 글자는 늘 한 글자가 한 글자로 바뀐다.
// 글자 모양은 글자마다 그대로 둔다(글자가 든 자리를 고치기만 한다). 블록은 바꾼 글 그대로 남긴다.

#include <algorithm>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

const FormatValue* fieldOf(const FormatFields& fields, const std::string& key) {
    const FormatValue* found = nullptr;
    for (const auto& [name, value] : fields) {
        if (name == key) found = &value;
    }
    return found;
}
std::string textOf(const FormatFields& fields, const std::string& key, const std::string& fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const std::string* t = std::get_if<std::string>(v)) return *t;
    }
    return fallback;
}
bool flagOf(const FormatFields& fields, const std::string& key, bool fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const double* d = std::get_if<double>(v)) return *d != 0;
        if (const bool* b = std::get_if<bool>(v)) return *b;
    }
    return fallback;
}

enum class WidthKind { Digit, Latin, Symbol, Kana, Jamo };
struct WidthPair {
    char32_t full;
    char32_t half;
    WidthKind kind;
};
// 유니코드 15 분해 표에서 뽑은 전각·반각 짝. 반각 쪽 차례다.
constexpr WidthPair kWidthPairs[] = {
    {0x3000, 0x0020, WidthKind::Symbol},
    {0xFF01, 0x0021, WidthKind::Symbol},
    {0xFF02, 0x0022, WidthKind::Symbol},
    {0xFF03, 0x0023, WidthKind::Symbol},
    {0xFF04, 0x0024, WidthKind::Symbol},
    {0xFF05, 0x0025, WidthKind::Symbol},
    {0xFF06, 0x0026, WidthKind::Symbol},
    {0xFF07, 0x0027, WidthKind::Symbol},
    {0xFF08, 0x0028, WidthKind::Symbol},
    {0xFF09, 0x0029, WidthKind::Symbol},
    {0xFF0A, 0x002A, WidthKind::Symbol},
    {0xFF0B, 0x002B, WidthKind::Symbol},
    {0xFF0C, 0x002C, WidthKind::Symbol},
    {0xFF0D, 0x002D, WidthKind::Symbol},
    {0xFF0E, 0x002E, WidthKind::Symbol},
    {0xFF0F, 0x002F, WidthKind::Symbol},
    {0xFF10, 0x0030, WidthKind::Digit},
    {0xFF11, 0x0031, WidthKind::Digit},
    {0xFF12, 0x0032, WidthKind::Digit},
    {0xFF13, 0x0033, WidthKind::Digit},
    {0xFF14, 0x0034, WidthKind::Digit},
    {0xFF15, 0x0035, WidthKind::Digit},
    {0xFF16, 0x0036, WidthKind::Digit},
    {0xFF17, 0x0037, WidthKind::Digit},
    {0xFF18, 0x0038, WidthKind::Digit},
    {0xFF19, 0x0039, WidthKind::Digit},
    {0xFF1A, 0x003A, WidthKind::Symbol},
    {0xFF1B, 0x003B, WidthKind::Symbol},
    {0xFF1C, 0x003C, WidthKind::Symbol},
    {0xFF1D, 0x003D, WidthKind::Symbol},
    {0xFF1E, 0x003E, WidthKind::Symbol},
    {0xFF1F, 0x003F, WidthKind::Symbol},
    {0xFF20, 0x0040, WidthKind::Symbol},
    {0xFF21, 0x0041, WidthKind::Latin},
    {0xFF22, 0x0042, WidthKind::Latin},
    {0xFF23, 0x0043, WidthKind::Latin},
    {0xFF24, 0x0044, WidthKind::Latin},
    {0xFF25, 0x0045, WidthKind::Latin},
    {0xFF26, 0x0046, WidthKind::Latin},
    {0xFF27, 0x0047, WidthKind::Latin},
    {0xFF28, 0x0048, WidthKind::Latin},
    {0xFF29, 0x0049, WidthKind::Latin},
    {0xFF2A, 0x004A, WidthKind::Latin},
    {0xFF2B, 0x004B, WidthKind::Latin},
    {0xFF2C, 0x004C, WidthKind::Latin},
    {0xFF2D, 0x004D, WidthKind::Latin},
    {0xFF2E, 0x004E, WidthKind::Latin},
    {0xFF2F, 0x004F, WidthKind::Latin},
    {0xFF30, 0x0050, WidthKind::Latin},
    {0xFF31, 0x0051, WidthKind::Latin},
    {0xFF32, 0x0052, WidthKind::Latin},
    {0xFF33, 0x0053, WidthKind::Latin},
    {0xFF34, 0x0054, WidthKind::Latin},
    {0xFF35, 0x0055, WidthKind::Latin},
    {0xFF36, 0x0056, WidthKind::Latin},
    {0xFF37, 0x0057, WidthKind::Latin},
    {0xFF38, 0x0058, WidthKind::Latin},
    {0xFF39, 0x0059, WidthKind::Latin},
    {0xFF3A, 0x005A, WidthKind::Latin},
    {0xFF3B, 0x005B, WidthKind::Symbol},
    {0xFF3C, 0x005C, WidthKind::Symbol},
    {0xFF3D, 0x005D, WidthKind::Symbol},
    {0xFF3E, 0x005E, WidthKind::Symbol},
    {0xFF3F, 0x005F, WidthKind::Symbol},
    {0xFF40, 0x0060, WidthKind::Symbol},
    {0xFF41, 0x0061, WidthKind::Latin},
    {0xFF42, 0x0062, WidthKind::Latin},
    {0xFF43, 0x0063, WidthKind::Latin},
    {0xFF44, 0x0064, WidthKind::Latin},
    {0xFF45, 0x0065, WidthKind::Latin},
    {0xFF46, 0x0066, WidthKind::Latin},
    {0xFF47, 0x0067, WidthKind::Latin},
    {0xFF48, 0x0068, WidthKind::Latin},
    {0xFF49, 0x0069, WidthKind::Latin},
    {0xFF4A, 0x006A, WidthKind::Latin},
    {0xFF4B, 0x006B, WidthKind::Latin},
    {0xFF4C, 0x006C, WidthKind::Latin},
    {0xFF4D, 0x006D, WidthKind::Latin},
    {0xFF4E, 0x006E, WidthKind::Latin},
    {0xFF4F, 0x006F, WidthKind::Latin},
    {0xFF50, 0x0070, WidthKind::Latin},
    {0xFF51, 0x0071, WidthKind::Latin},
    {0xFF52, 0x0072, WidthKind::Latin},
    {0xFF53, 0x0073, WidthKind::Latin},
    {0xFF54, 0x0074, WidthKind::Latin},
    {0xFF55, 0x0075, WidthKind::Latin},
    {0xFF56, 0x0076, WidthKind::Latin},
    {0xFF57, 0x0077, WidthKind::Latin},
    {0xFF58, 0x0078, WidthKind::Latin},
    {0xFF59, 0x0079, WidthKind::Latin},
    {0xFF5A, 0x007A, WidthKind::Latin},
    {0xFF5B, 0x007B, WidthKind::Symbol},
    {0xFF5C, 0x007C, WidthKind::Symbol},
    {0xFF5D, 0x007D, WidthKind::Symbol},
    {0xFF5E, 0x007E, WidthKind::Symbol},
    {0xFFE0, 0x00A2, WidthKind::Symbol},
    {0xFFE1, 0x00A3, WidthKind::Symbol},
    {0xFFE5, 0x00A5, WidthKind::Symbol},
    {0xFFE4, 0x00A6, WidthKind::Symbol},
    {0xFFE2, 0x00AC, WidthKind::Symbol},
    {0xFFE3, 0x00AF, WidthKind::Symbol},
    {0xFFE6, 0x20A9, WidthKind::Symbol},
    {0xFF5F, 0x2985, WidthKind::Symbol},
    {0xFF60, 0x2986, WidthKind::Symbol},
    {0x3002, 0xFF61, WidthKind::Kana},
    {0x300C, 0xFF62, WidthKind::Kana},
    {0x300D, 0xFF63, WidthKind::Kana},
    {0x3001, 0xFF64, WidthKind::Kana},
    {0x30FB, 0xFF65, WidthKind::Kana},
    {0x30F2, 0xFF66, WidthKind::Kana},
    {0x30A1, 0xFF67, WidthKind::Kana},
    {0x30A3, 0xFF68, WidthKind::Kana},
    {0x30A5, 0xFF69, WidthKind::Kana},
    {0x30A7, 0xFF6A, WidthKind::Kana},
    {0x30A9, 0xFF6B, WidthKind::Kana},
    {0x30E3, 0xFF6C, WidthKind::Kana},
    {0x30E5, 0xFF6D, WidthKind::Kana},
    {0x30E7, 0xFF6E, WidthKind::Kana},
    {0x30C3, 0xFF6F, WidthKind::Kana},
    {0x30FC, 0xFF70, WidthKind::Kana},
    {0x30A2, 0xFF71, WidthKind::Kana},
    {0x30A4, 0xFF72, WidthKind::Kana},
    {0x30A6, 0xFF73, WidthKind::Kana},
    {0x30A8, 0xFF74, WidthKind::Kana},
    {0x30AA, 0xFF75, WidthKind::Kana},
    {0x30AB, 0xFF76, WidthKind::Kana},
    {0x30AD, 0xFF77, WidthKind::Kana},
    {0x30AF, 0xFF78, WidthKind::Kana},
    {0x30B1, 0xFF79, WidthKind::Kana},
    {0x30B3, 0xFF7A, WidthKind::Kana},
    {0x30B5, 0xFF7B, WidthKind::Kana},
    {0x30B7, 0xFF7C, WidthKind::Kana},
    {0x30B9, 0xFF7D, WidthKind::Kana},
    {0x30BB, 0xFF7E, WidthKind::Kana},
    {0x30BD, 0xFF7F, WidthKind::Kana},
    {0x30BF, 0xFF80, WidthKind::Kana},
    {0x30C1, 0xFF81, WidthKind::Kana},
    {0x30C4, 0xFF82, WidthKind::Kana},
    {0x30C6, 0xFF83, WidthKind::Kana},
    {0x30C8, 0xFF84, WidthKind::Kana},
    {0x30CA, 0xFF85, WidthKind::Kana},
    {0x30CB, 0xFF86, WidthKind::Kana},
    {0x30CC, 0xFF87, WidthKind::Kana},
    {0x30CD, 0xFF88, WidthKind::Kana},
    {0x30CE, 0xFF89, WidthKind::Kana},
    {0x30CF, 0xFF8A, WidthKind::Kana},
    {0x30D2, 0xFF8B, WidthKind::Kana},
    {0x30D5, 0xFF8C, WidthKind::Kana},
    {0x30D8, 0xFF8D, WidthKind::Kana},
    {0x30DB, 0xFF8E, WidthKind::Kana},
    {0x30DE, 0xFF8F, WidthKind::Kana},
    {0x30DF, 0xFF90, WidthKind::Kana},
    {0x30E0, 0xFF91, WidthKind::Kana},
    {0x30E1, 0xFF92, WidthKind::Kana},
    {0x30E2, 0xFF93, WidthKind::Kana},
    {0x30E4, 0xFF94, WidthKind::Kana},
    {0x30E6, 0xFF95, WidthKind::Kana},
    {0x30E8, 0xFF96, WidthKind::Kana},
    {0x30E9, 0xFF97, WidthKind::Kana},
    {0x30EA, 0xFF98, WidthKind::Kana},
    {0x30EB, 0xFF99, WidthKind::Kana},
    {0x30EC, 0xFF9A, WidthKind::Kana},
    {0x30ED, 0xFF9B, WidthKind::Kana},
    {0x30EF, 0xFF9C, WidthKind::Kana},
    {0x30F3, 0xFF9D, WidthKind::Kana},
    {0x3099, 0xFF9E, WidthKind::Kana},
    {0x309A, 0xFF9F, WidthKind::Kana},
    {0x3164, 0xFFA0, WidthKind::Jamo},
    {0x3131, 0xFFA1, WidthKind::Jamo},
    {0x3132, 0xFFA2, WidthKind::Jamo},
    {0x3133, 0xFFA3, WidthKind::Jamo},
    {0x3134, 0xFFA4, WidthKind::Jamo},
    {0x3135, 0xFFA5, WidthKind::Jamo},
    {0x3136, 0xFFA6, WidthKind::Jamo},
    {0x3137, 0xFFA7, WidthKind::Jamo},
    {0x3138, 0xFFA8, WidthKind::Jamo},
    {0x3139, 0xFFA9, WidthKind::Jamo},
    {0x313A, 0xFFAA, WidthKind::Jamo},
    {0x313B, 0xFFAB, WidthKind::Jamo},
    {0x313C, 0xFFAC, WidthKind::Jamo},
    {0x313D, 0xFFAD, WidthKind::Jamo},
    {0x313E, 0xFFAE, WidthKind::Jamo},
    {0x313F, 0xFFAF, WidthKind::Jamo},
    {0x3140, 0xFFB0, WidthKind::Jamo},
    {0x3141, 0xFFB1, WidthKind::Jamo},
    {0x3142, 0xFFB2, WidthKind::Jamo},
    {0x3143, 0xFFB3, WidthKind::Jamo},
    {0x3144, 0xFFB4, WidthKind::Jamo},
    {0x3145, 0xFFB5, WidthKind::Jamo},
    {0x3146, 0xFFB6, WidthKind::Jamo},
    {0x3147, 0xFFB7, WidthKind::Jamo},
    {0x3148, 0xFFB8, WidthKind::Jamo},
    {0x3149, 0xFFB9, WidthKind::Jamo},
    {0x314A, 0xFFBA, WidthKind::Jamo},
    {0x314B, 0xFFBB, WidthKind::Jamo},
    {0x314C, 0xFFBC, WidthKind::Jamo},
    {0x314D, 0xFFBD, WidthKind::Jamo},
    {0x314E, 0xFFBE, WidthKind::Jamo},
    {0x314F, 0xFFC2, WidthKind::Jamo},
    {0x3150, 0xFFC3, WidthKind::Jamo},
    {0x3151, 0xFFC4, WidthKind::Jamo},
    {0x3152, 0xFFC5, WidthKind::Jamo},
    {0x3153, 0xFFC6, WidthKind::Jamo},
    {0x3154, 0xFFC7, WidthKind::Jamo},
    {0x3155, 0xFFCA, WidthKind::Jamo},
    {0x3156, 0xFFCB, WidthKind::Jamo},
    {0x3157, 0xFFCC, WidthKind::Jamo},
    {0x3158, 0xFFCD, WidthKind::Jamo},
    {0x3159, 0xFFCE, WidthKind::Jamo},
    {0x315A, 0xFFCF, WidthKind::Jamo},
    {0x315B, 0xFFD2, WidthKind::Jamo},
    {0x315C, 0xFFD3, WidthKind::Jamo},
    {0x315D, 0xFFD4, WidthKind::Jamo},
    {0x315E, 0xFFD5, WidthKind::Jamo},
    {0x315F, 0xFFD6, WidthKind::Jamo},
    {0x3160, 0xFFD7, WidthKind::Jamo},
    {0x3161, 0xFFDA, WidthKind::Jamo},
    {0x3162, 0xFFDB, WidthKind::Jamo},
    {0x3163, 0xFFDC, WidthKind::Jamo},
    {0x2502, 0xFFE8, WidthKind::Symbol},
    {0x2190, 0xFFE9, WidthKind::Symbol},
    {0x2191, 0xFFEA, WidthKind::Symbol},
    {0x2192, 0xFFEB, WidthKind::Symbol},
    {0x2193, 0xFFEC, WidthKind::Symbol},
    {0x25A0, 0xFFED, WidthKind::Symbol},
    {0x25CB, 0xFFEE, WidthKind::Symbol},
};

// 한글 2024 의 바꿀 대상 갈래(ConvertFullHalf 의 Number·Alpha·Symbol·Gata·HGJamo). 유니코드 짝을 이렇게 나눈다.
//   숫자 Number: 0~9.  영문자 Alpha: 영문자와 나머지 ASCII 기호·빈칸(U+0020 ↔ U+3000).
//   기호 Symbol: ¢£¬¯¦¥₩, 반각 꼴 │←↑→↓■○, 반각 가타카나 문장 부호 ｡｢｣､.
//   가타카나 Gata: ･(가운뎃점)와 반각 가타카나·ｰ·탁점.  한글 자모 HGJamo: 반각 한글 자모.
// ⦅⦆(｟｠)는 어느 갈래로도 바뀌지 않는다.
WidthKind hangulKind(const WidthPair& p) {
    if (p.half >= U'0' && p.half <= U'9') return WidthKind::Digit;
    if (p.half >= 0x20 && p.half <= 0x7E) return WidthKind::Latin;
    if (p.half >= 0xFF61 && p.half <= 0xFF64) return WidthKind::Symbol;
    if (p.half >= 0xFF65 && p.half <= 0xFF9F) return WidthKind::Kana;
    if (p.half >= 0xFFA0 && p.half <= 0xFFDC) return WidthKind::Jamo;
    return WidthKind::Symbol;
}
bool pairUsed(const WidthPair& p) { return p.full != 0xFF5F && p.full != 0xFF60; }
// 반각으로 바꿀 때 한글이 그대로 두는 전각 글자: ー·・, ←, ￠~￥, 결합 탁점.
bool halfAllowed(const WidthPair& p) {
    if (p.full == 0x30FC || p.full == 0x30FB || p.full == 0x2190) return false;
    if (p.full >= 0xFFE0 && p.full <= 0xFFE5) return false;
    return p.full != 0x3099 && p.full != 0x309A;
}

const WidthPair* pairOfHalf(char32_t c) {
    for (const WidthPair& p : kWidthPairs) {
        if (p.half == c && pairUsed(p)) return &p;
    }
    return nullptr;
}
const WidthPair* pairOfFull(char32_t c) {
    for (const WidthPair& p : kWidthPairs) {
        if (p.full == c && pairUsed(p) && halfAllowed(p)) return &p;
    }
    return nullptr;
}

bool asciiUpper(char32_t c) { return (c >= U'A' && c <= U'Z') || (c >= 0xFF21 && c <= 0xFF3A); }
bool asciiLower(char32_t c) { return (c >= U'a' && c <= U'z') || (c >= 0xFF41 && c <= 0xFF5A); }
bool latinLetter(char32_t c) { return asciiUpper(c) || asciiLower(c); }
char32_t toUpper(char32_t c) { return asciiLower(c) ? c - 0x20 : c; }
char32_t toLower(char32_t c) { return asciiUpper(c) ? c + 0x20 : c; }
bool space(char32_t c) { return c == U' ' || c == U'\t' || c == 0x3000 || c == 0xA0; }
bool sentenceEnd(char32_t c) { return c == U'.' || c == U'!' || c == U'?'; }
// 단어를 이루는 글자. 한글로 본 것: 영문자, 악센트 글자(ï·À·ü), 한글은 단어 안이고 숫자·'·_·- 는 단어를 끊는다.
bool wordLetter(char32_t c) {
    if (latinLetter(c)) return true;
    if (c >= 0xC0 && c <= 0x24F) return c != 0xD7 && c != 0xF7;  // 라틴 악센트 글자(× ÷ 빼고)
    if (c >= 0x370 && c <= 0x52F) return true;                     // 그리스·키릴 문자
    if ((c >= 0xAC00 && c <= 0xD7A3) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3131 && c <= 0x318E)) return true;
    if ((c >= 0x3041 && c <= 0x30FA) || (c >= 0x4E00 && c <= 0x9FFF)) return true;  // 가나·한자
    return false;
}

// 문단 글자 차례 s 의 [from, to) 를 대문자/소문자 규칙으로 바꾼다(한글 ConvertCase 의 type 0~5).
// 한글로 본 경계: 블록 처음은 문장·단어의 처음이다(블록 앞 글은 보지 않는다). 문장 처음은 블록·문단 처음과
// . ! ? 바로 뒤에 빈칸이 온 다음 자리이고, 그 자리 첫 글자(따옴표·괄호도 글자다)가 영문자일 때만 대문자로 한다.
void convertCase(std::u32string& s, int from, int to, const std::string& mode) {
    bool atSentence = true;
    for (int i = from; i < to; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        const char32_t c = s[k];
        const bool wordStart = latinLetter(c) && (i == from || !wordLetter(s[k - 1]));
        bool sentenceFirst = false;
        if (i > from && space(c) && sentenceEnd(s[k - 1])) atSentence = true;
        if (atSentence && !space(c)) {
            sentenceFirst = latinLetter(c);
            atSentence = false;
        }
        if (!latinLetter(c)) continue;
        char32_t out = c;
        if (mode == "upper") out = toUpper(c);
        else if (mode == "lower") out = toLower(c);
        else if (mode == "toggle") out = asciiUpper(c) ? toLower(c) : toUpper(c);
        else if (mode == "wordCap") out = wordStart ? toUpper(c) : toLower(c);
        else if (mode == "sentenceCap") out = sentenceFirst ? toUpper(c) : c;
        else if (mode == "sentenceCapOnly") out = sentenceFirst ? toUpper(c) : toLower(c);
        s[k] = out;
    }
}

// 바꿀 대상. 처음 값은 한글 GetDefault(ConvertFullHalf)와 같다: 숫자·영문자·기호만 켠다.
struct WidthTargets {
    bool toHalf = true;
    bool digit = true;
    bool latin = true;
    bool symbol = true;
    bool kana = false;
    bool jamo = false;
    [[nodiscard]] bool wants(const WidthPair& p) const {
        switch (hangulKind(p)) {
            case WidthKind::Digit: return digit;
            case WidthKind::Latin: return latin;
            case WidthKind::Symbol: return symbol;
            case WidthKind::Kana: return kana;
            case WidthKind::Jamo: return jamo;
        }
        return false;
    }
};

// 글자 i 를 바꾼 글. 한글로 본 탁점: 전각으로 바꿀 때 반각 ﾞ·ﾟ 는 앞 글자와 합치지 않고 결합 탁점 U+3099·U+309A 로
// 바꾼다(ｶﾞ → カ+U+3099, ｳﾞ → ウ+U+3099). 반각으로 바꿀 때 탁음 가타카나(ガ·ヴ)는 나누지 않고 그대로 둔다.
// 그래서 글자 수가 바뀌지 않는다.
std::vector<std::u32string> convertWidth(const std::u32string& s, int from, int to, const WidthTargets& t) {
    std::vector<std::u32string> out;
    for (int i = from; i < to; ++i) {
        const char32_t c = s[static_cast<std::size_t>(i)];
        std::u32string next(1, c);
        if (t.toHalf) {
            if (const WidthPair* p = pairOfFull(c); p && t.wants(*p)) next = std::u32string(1, p->half);
        } else if (const WidthPair* p = pairOfHalf(c); p && t.wants(*p)) {
            next = std::u32string(1, p->full);
        }
        out.push_back(std::move(next));
    }
    return out;
}

// 글 조각 하나의 UTF-8 을 코드 포인트로.
std::u32string charsOf(const std::string& text) {
    std::u32string out;
    for (const unsigned char c : text) {
        if ((c & 0xC0) == 0x80 && !out.empty()) out.back() = (out.back() << 6) | (c & 0x3F);
        else if (c >= 0xF0) out.push_back(c & 0x07);
        else if (c >= 0xE0) out.push_back(c & 0x0F);
        else if (c >= 0xC0) out.push_back(c & 0x1F);
        else out.push_back(c);
    }
    return out;
}

// 글자 차례 [from, from+news.size()) 를 news 로 바꾼다. 글 조각(item) 안에서 고쳐 글자 모양이 그대로다.
// 돌려주는 값은 바뀐 글자 수 차이다.
int applyChars(model::Paragraph& para, int from, const std::vector<std::u32string>& news) {
    const int to = from + static_cast<int>(news.size());
    int index = 0;
    int delta = 0;
    for (model::Run& run : para.runs) {
        for (model::InlineItem& item : run.items) {
            if (item.type != model::InlineItem::Type::Text) {
                if (isCaretCharControl(item.control)) ++index;
                continue;
            }
            const std::u32string chars = charsOf(item.text);
            std::string text;
            bool changed = false;
            for (const char32_t c : chars) {
                if (index >= from && index < to) {
                    const std::u32string& next = news[static_cast<std::size_t>(index - from)];
                    if (next.size() != 1 || next[0] != c) changed = true;
                    for (const char32_t n : next) appendUtf8(text, n);
                    delta += static_cast<int>(next.size()) - 1;
                } else {
                    appendUtf8(text, c);
                }
                ++index;
            }
            if (changed) item.text = std::move(text);
        }
    }
    return delta;
}

}  // namespace

EditResult convertTextNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0 || !cur.hasSelection) {
        why = "바꿀 부분을 블록으로 정하세요.";
        return {};
    }
    std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras || cur.startParagraph < 0 || cur.endParagraph >= static_cast<int>(paras->size())) return {};
    const std::string kind = textOf(fields, "kind", "case");
    const std::string mode = textOf(fields, "mode", "upper");
    WidthTargets targets;
    targets.toHalf = textOf(fields, "to", "half") != "full";
    targets.digit = flagOf(fields, "digit", true);
    targets.latin = flagOf(fields, "latin", true);
    targets.symbol = flagOf(fields, "symbol", true);
    targets.kana = flagOf(fields, "kana", false);
    targets.jamo = flagOf(fields, "jamo", false);
    if (kind != "case" && kind != "width") return {};

    const int story = cur.story;
    const bool forward = cur.anchorParagraph < cur.focusParagraph ||
                         (cur.anchorParagraph == cur.focusParagraph && cur.anchorOffset <= cur.focusOffset);
    const int startChar = offsetToCharIndex(doc, pages, story, cur.startParagraph, cur.startOffset);
    int endChar = offsetToCharIndex(doc, pages, story, cur.endParagraph, cur.endOffset);
    bool changed = false;
    for (int p = cur.startParagraph; p <= cur.endParagraph; ++p) {
        model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
        std::u32string chars = caretCharsOf(para);
        const int n = static_cast<int>(chars.size());
        const int from = std::clamp(p == cur.startParagraph ? startChar : 0, 0, n);
        const int to = std::clamp(p == cur.endParagraph ? endChar : n, 0, n);
        if (from >= to) continue;
        std::vector<std::u32string> news;
        if (kind == "case") {
            std::u32string next = chars;
            convertCase(next, from, to, mode);
            for (int i = from; i < to; ++i) news.emplace_back(1, next[static_cast<std::size_t>(i)]);
        } else {
            news = convertWidth(chars, from, to, targets);
        }
        bool differs = false;
        for (int i = from; i < to; ++i) {
            const std::u32string& next = news[static_cast<std::size_t>(i - from)];
            if (next.size() != 1 || next[0] != chars[static_cast<std::size_t>(i)]) differs = true;
        }
        if (!differs) continue;
        if (EditResult blocked; !changed && editBlockedByLicenseFonts(doc, blocked)) return blocked;
        const int delta = applyChars(para, from, news);
        para.lineSegs.clear();
        if (p == cur.endParagraph) endChar += delta;
        changed = true;
    }
    if (!changed) {
        why = kind == "case" ? "블록 안에 바꿀 영문자가 없습니다." : "블록 안에 바꿀 전각·반각 글자가 없습니다.";
        return {};
    }
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    clearPendingCharShape(hold);
    hold.preferredX = -1;
    dropParentLineSegsOf(doc, story);
    model::ensureParagraphIds(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    const model::CursorPos start{story, cur.startParagraph,
                                 charIndexToOffset(doc, pages, story, cur.startParagraph, startChar)};
    const model::CursorPos end{story, cur.endParagraph, charIndexToOffset(doc, pages, story, cur.endParagraph, endChar)};
    if (forward) setPointerCursor(doc, pages, start, end);
    else setPointerCursor(doc, pages, end, start);
    EditRelayoutResult full;
    full.fullInvalidation = true;
    full.pageListChanged = true;
    return makeEditResult(doc, true, true, story, cur.focusParagraph, &full);
}

}  // namespace chwp::render
