// 표 정렬(한글 2024 Sort, 표 칸 블록). 한글 2024 의 정렬 규칙을 그대로 따른다.
//  - 줄마다 기준 칸의 첫 문단 글(HWP 원문: 탭·개체는 8 단위 확장 문자) 뒤에 문단 끝 \r 을 붙인 것이 키다.
//  - 비교는 형식마다 글자·숫자·날짜·코드 비교, 같으면 다음 기준. 내림차순은 결과 부호를 뒤집는다.
//  - 차례 정하기는 한글 자체 qsort(8개 이하는 가장 큰 것을 끝으로 보내기, 그보다 크면 가운데 값 분할)다.
// 결과는 한글로 같은 표를 정렬해 저장한 HWPX 로 맞췄다(tests/test_table_sort.cpp).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "chwp/common/unicode.hpp"
#include "chwp/model/collate.hpp"
#include "chwp/model/table_edit.hpp"

namespace chwp::model {

namespace collate_detail {
extern const std::uint16_t kPuaSyllableJamo[16665];
extern const std::uint16_t kPuaJamoToJamo[380];
extern const std::uint16_t kJamoToPuaJamo[361];
}  // namespace collate_detail

namespace {

// HWP 원문에서 8 단위를 차지하는 확장 문자(1~31 가운데 줄바꿈 10·문단 끝 13·0 을 뺀 것, 한글 0xFFDBFE 표).
bool extendedChar(char16_t c) { return c < 0x20 && ((0xFFDBFEu >> c) & 1u) != 0; }
std::size_t unitsOf(char16_t c) { return extendedChar(c) ? 8 : 1; }

char16_t controlChar(const Control& control) {
    switch (control.kind) {
        case ControlKind::SectionDef:
        case ControlKind::ColumnDef: return 2;
        case ControlKind::HiddenComment: return 15;
        case ControlKind::Header:
        case ControlKind::Footer:
        case ControlKind::Bookmark:
        case ControlKind::Dutmal:
        case ControlKind::Compose: return 16;
        case ControlKind::Footnote:
        case ControlKind::Endnote: return 17;
        case ControlKind::AutoNumber:
        case ControlKind::NewNumber: return 18;
        case ControlKind::PageHiding:
        case ControlKind::PageNumPos: return 21;
        case ControlKind::Field: return 3;
        case ControlKind::FieldEnd: return 4;
        default: return 11;
    }
}

void appendExtended(std::u16string& out, char16_t code) {
    out += code;
    out.append(6, u'\0');
    out += code;
}

// 칸 첫 문단의 HWP 원문 + 문단 끝(\r).
std::u16string keyText(const TableCell* cell) {
    std::u16string out;
    if (cell && !cell->content.paragraphs.empty()) out = paragraphSortText(cell->content.paragraphs.front(), nullptr, {});
    out += u'\r';
    return out;
}

void reverseUnits(std::u16string& s) { std::reverse(s.begin(), s.end()); }

// 글자 형식: 키 길이만큼(0 문자 앞까지) 빼기표·하이픈(0x18)·탭을 지우고, 확장 문자는 첫 단위만
// 남긴 뒤 로캘 비교 → 대소문자 무시 비교 → 코드 차례.
std::u16string charForm(const std::u16string& s, int len, bool rear) {
    std::u16string out;
    int left = len;
    for (std::size_t i = 0; i < s.size() && left > 0;) {
        const char16_t c = s[i];
        if (c == 0) break;
        if (c != u'-' && c != 0x18 && c != 9) out += c;
        i += unitsOf(c);
        left -= static_cast<int>(unitsOf(c));
    }
    if (rear) reverseUnits(out);
    return out;
}

// 고어 정렬·종성/중성/초성 순 비교. 음절(현대·사용자 영역 옛한글)과 첫가끝·사용자 영역 낱자 묶음을
// 첫·가운데·끝 낱자로 나눠 한글 낱자 차례 값으로 견주고, 둘 다 낱자가 아니면 글자 코드로 견준다.
// 길이는 형식에 맞춰 바꾸기 전 키 길이를 쓰고, 끝나는 조건(B 글자 값 > B 남은 길이)과 남은 길이 비교까지 한글과 같다.
std::uint16_t jamoToPua(char16_t c) {
    if (c >= 0x1100 && c <= 0x11FF) return collate_detail::kJamoToPuaJamo[c - 0x1100];
    if (c >= 0xA960 && c <= 0xA97C) return collate_detail::kJamoToPuaJamo[0x100 + (c - 0xA960)];
    if (c >= 0xD7B0 && c <= 0xD7FB) return collate_detail::kJamoToPuaJamo[0x100 + 0x1D + (c - 0xD7B0)];
    return 0;
}

char16_t puaJamoToJamo(char16_t c) {
    return c >= 0xF784 && c <= 0xF8FF ? static_cast<char16_t>(collate_detail::kPuaJamoToJamo[c - 0xF784]) : char16_t{0};
}

bool jamoL(char16_t c) { return (c >= 0x1100 && c <= 0x115F) || (c >= 0xA960 && c <= 0xA97C); }
bool jamoV(char16_t c) { return (c >= 0x1160 && c <= 0x11A7) || (c >= 0xD7B0 && c <= 0xD7C6); }
bool jamoT(char16_t c) { return (c >= 0x11A8 && c <= 0x11FF) || (c >= 0xD7CB && c <= 0xD7FB); }

struct Syllable {
    int used = 1;
    std::array<char16_t, 3> jamo{};
    bool hangul = false;
};

Syllable splitSyllable(const std::u16string& s, std::size_t i, int n) {
    const auto at = [&](std::size_t k) { return k < s.size() ? s[k] : char16_t{0}; };
    Syllable out;
    const char16_t c = at(i);
    out.jamo = {c, 0, 0};
    out.hangul = true;
    if (c >= 0xAC00 && c <= 0xD7A3) {
        const int k = c - 0xAC00;
        const int t = k % 28;
        out.jamo = {static_cast<char16_t>(0x1100 + k / 588), static_cast<char16_t>(0x1161 + (k % 588) / 28),
                    static_cast<char16_t>(t ? 0x11A7 + t : 0)};
        return out;
    }
    if (c >= 0xE0BC && c <= 0xF66E && !(c >= 0xF000 && c <= 0xF0FF)) {
        const std::uint16_t* j = collate_detail::kPuaSyllableJamo + (c - 0xE0BC) * 3;
        out.jamo = {puaJamoToJamo(j[0]), puaJamoToJamo(j[1]), puaJamoToJamo(j[2])};
        return out;
    }
    const char16_t b = n >= 2 ? at(i + 1) : 0;
    const char16_t d = n >= 3 ? at(i + 2) : 0;
    if (jamoL(c) || jamoV(c) || jamoT(c)) {
        if (jamoL(c) && jamoV(b) && jamoT(d)) {
            out.used = 3;
            out.jamo = {c, b, d};
        } else if (jamoL(c) && jamoV(b)) {
            out.used = 2;
            out.jamo = {c, b, 0x11A7};
        } else if (jamoV(c) && jamoT(b)) {
            out.used = 2;
            out.jamo = {0x115F, c, b};
        }
        return out;
    }
    if (c >= 0xF784 && c <= 0xF8FF) {
        const auto pl = [](char16_t x) { return x >= 0xF784 && x <= 0xF805; };
        const auto pv = [](char16_t x) { return x >= 0xF806 && x <= 0xF869; };
        const auto pt = [](char16_t x) { return x >= 0xF86A && x <= 0xF8FF; };
        if (pl(c) && pv(b) && pt(d)) {
            out.used = 3;
            out.jamo = {puaJamoToJamo(c), puaJamoToJamo(b), puaJamoToJamo(d)};
        } else if (pl(c) && pv(b)) {
            out.used = 2;
            out.jamo = {puaJamoToJamo(c), puaJamoToJamo(b), 0x11A7};
        } else if (pv(c) && pt(b)) {
            out.used = 2;
            out.jamo = {0x115F, puaJamoToJamo(c), puaJamoToJamo(b)};
        } else {
            out.jamo[0] = puaJamoToJamo(c);
        }
        return out;
    }
    out.hangul = c >= 0x3130 && c <= 0x318F;
    return out;
}

// 8 단위 확장 문자를 통째로 뺀다. 확장 문자가 있을 때만 부른다.
std::u16string withoutExtended(const std::u16string& s, int& n) {
    std::u16string out;
    int left = n;
    for (std::size_t i = 0; i < s.size() && s[i] != 0 && left > 0;) {
        if (extendedChar(s[i])) {
            i += 8;
            left -= 8;
        } else {
            out += s[i++];
            left -= 1;
        }
    }
    n = static_cast<int>(out.size());
    return out;
}

bool hasExtended(const std::u16string& s, int n) {
    for (std::size_t i = 0; i < s.size() && static_cast<int>(i) < n && s[i] != 0; ++i) {
        if (extendedChar(s[i])) return true;
    }
    return false;
}

int compareJamo(const std::u16string& a0, int lenA, const std::u16string& b0, int lenB, bool finalFirst) {
    lenA = std::max(lenA, 0);
    lenB = std::max(lenB, 0);
    if (lenA == 0) return lenB == 0 ? 0 : -1;
    if (lenB == 0) return 1;
    std::u16string a = a0;
    std::u16string b = b0;
    if (hasExtended(a, lenA)) a = withoutExtended(a, lenA);
    if (hasExtended(b, lenB)) b = withoutExtended(b, lenB);
    const auto at = [](const std::u16string& s, std::size_t k) { return k < s.size() ? s[k] : char16_t{0}; };
    std::size_t ia = 0, ib = 0;
    int result = 0;
    while (at(a, ia) != 0 && lenA > 0 && static_cast<int>(at(b, ib)) > lenB) {
        const Syllable x = splitSyllable(a, ia, lenA);
        const Syllable y = splitSyllable(b, ib, lenB);
        if (x.hangul && y.hangul) {
            for (int k = 0; k < 3 && result == 0; ++k) {
                const int idx = finalFirst ? 2 - k : k;
                const std::uint16_t p = jamoToPua(x.jamo[static_cast<std::size_t>(idx)]);
                const std::uint16_t q = jamoToPua(y.jamo[static_cast<std::size_t>(idx)]);
                if (p != q) result = p < q ? -1 : 1;
            }
        } else if (x.jamo[0] != y.jamo[0]) {
            result = x.jamo[0] < y.jamo[0] ? -1 : 1;
        }
        lenA -= x.used;
        lenB -= y.used;
        ia += static_cast<std::size_t>(x.used);
        ib += static_cast<std::size_t>(y.used);
        if (result) break;
    }
    if (result == 0 && lenA != lenB) result = lenA < lenB ? 1 : -1;
    return result;
}

int compareChar(const std::u16string& a, const std::u16string& b) {
    if (const int r = windowsCompare(a, b, CollateFlags::IgnoreCaseAccentKanaWidth)) return r;
    if (const int r = windowsCompare(a, b, CollateFlags::IgnoreCaseString)) return r;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}

// 숫자 형식. 앞뒤 공백 사이에 부호와 숫자·소수점·쉼표만 있으면 수다(빈 칸은 0).
// 키 길이·문단 끝(\r)·0 문자 가운데 먼저 오는 곳까지 본다. 부호는 길이를 보지 않고 읽는다.
bool spaceLike(char16_t c) {
    return c <= 0x20 || (c >= 0x2000 && c <= 0x200A) || c == 0xA0 || c == 0x200B || c == 0x3000 || c == 0xFEFF;
}
bool digit(char16_t c) { return c >= u'0' && c <= u'9'; }
char16_t charAt(const std::u16string& s, std::size_t i) { return i < s.size() ? s[i] : char16_t{0}; }

bool isNumber(const std::u16string& s, int len) {
    std::size_t i = 0;
    int left = len;
    while (charAt(s, i) != u'\r' && left > 0) {
        const char16_t c = charAt(s, i);
        if (digit(c) || c == u'+' || c == u'-') break;
        if (!spaceLike(c)) return false;
        i += unitsOf(c);
        left -= static_cast<int>(unitsOf(c));
    }
    if (charAt(s, i) == u'+' || charAt(s, i) == u'-') {
        i += 1;
        left -= 1;
    }
    bool space = false;
    while (left > 0 && charAt(s, i) != 0 && charAt(s, i) != u'\r') {
        const char16_t c = charAt(s, i);
        if (digit(c) || c == u'.' || c == u',') {
            if (space) return false;
        } else if (spaceLike(c)) {
            space = true;
        } else {
            return false;
        }
        i += unitsOf(c);
        left -= static_cast<int>(unitsOf(c));
    }
    return true;
}

// 부호와 정수 39자리(오른쪽 맞춤)·소수 39자리(왼쪽 맞춤)를 '0' 으로 채운 80 바이트.
struct NumberKey {
    char sign = '+';
    std::string digits;
};

NumberKey numberKey(const std::u16string& s, int len) {
    NumberKey k;
    std::size_t i = 0;
    int left = len;
    while (charAt(s, i) != u'\r' && left > 0 && !digit(charAt(s, i)) && charAt(s, i) != u'+' && charAt(s, i) != u'-') {
        left -= static_cast<int>(unitsOf(charAt(s, i)));
        i += unitsOf(charAt(s, i));
    }
    if (charAt(s, i) == u'+' || charAt(s, i) == u'-') {
        k.sign = static_cast<char>(charAt(s, i));
        i += 1;
        left -= 1;
    }
    std::string ip;
    std::string fp;
    bool fraction = false;
    while (left > 0 && charAt(s, i) != 0 && charAt(s, i) != u'\r') {
        const char16_t c = charAt(s, i);
        if (!fraction) {
            if (digit(c)) {
                // 정수 자리가 39 개를 넘으면 한글은 거기서 읽기를 멈춘다(소수는 읽지 않는다)
                if (ip.size() >= 39) break;
                ip += static_cast<char>(c);
            } else if (c == u'.') {
                fraction = true;
            } else if (c != u',') {
                break;
            }
        } else {
            if (!digit(c) || fp.size() >= 39) break;
            fp += static_cast<char>(c);
        }
        i += unitsOf(c);
        left -= static_cast<int>(unitsOf(c));
    }
    k.digits = std::string(40 - ip.size(), '0') + ip + fp + std::string(40 - fp.size(), '0');
    return k;
}

std::u16string untilNul(const std::u16string& s) {
    const std::size_t z = s.find(u'\0');
    return z == std::u16string::npos ? s : s.substr(0, z);
}

int compareNumber(const std::u16string& a, int la, const std::u16string& b, int lb) {
    const bool na = isNumber(a, la);
    const bool nb = isNumber(b, lb);
    if (na && nb) {
        const NumberKey x = numberKey(a, la);
        const NumberKey y = numberKey(b, lb);
        if (x.sign != y.sign) return x.sign == '+' ? 1 : -1;
        const int r = x.digits.compare(y.digits);
        const int s = r < 0 ? -1 : (r > 0 ? 1 : 0);
        return x.sign == '-' ? -s : s;
    }
    if (na) return -1;
    if (nb) return 1;
    // 둘 다 수가 아니면 wcscoll(사용자 로캘, SORT_STRINGSORT). 0 문자에서 끝나는 글이다.
    return windowsCompare(untilNul(a), untilNul(b), CollateFlags::String);
}

// 날짜 형식. 앞 30 단위를 -./ 로 나눠(이어진 구분자는 하나) 년월일 자리로 옮긴
// 29 자 글을 wcsncmp 로 견준다. 자리 옮기기의 wcsncpy_s 끝 0 이 남는 버릇(한 자리 월이면 년만 남음)까지 같다.
struct Token {
    std::size_t at = 0;
    std::size_t units = 0;
};

std::vector<Token> dateTokens(const std::u16string& s, std::size_t len) {
    std::vector<Token> out;
    std::size_t i = 0;
    const auto delim = [](char16_t c) { return c == u'-' || c == u'.' || c == u'/'; };
    while (i < len && s[i] != 0 && out.size() < 10) {
        while (i < len && s[i] != 0 && delim(s[i])) i += unitsOf(s[i]);
        if (i >= len || s[i] == 0) break;
        Token t;
        t.at = i;
        while (i < len && s[i] != 0 && !delim(s[i])) {
            t.units += unitsOf(s[i]);
            i += unitsOf(s[i]);
        }
        out.push_back(t);
    }
    return out;
}

// len 은 키 길이와 30 가운데 작은 것. 글은 키 시작부터 원문 끝까지라 키 밖 글도 옮겨질 수 있다.
std::u16string dateForm(const std::u16string& s, int keyLen, bool extendYear, int yearBase) {
    std::u16string buf(30, u'0');
    buf[0] = u' ';
    buf[1] = u' ';
    buf[29] = 0;
    const std::size_t len = static_cast<std::size_t>(std::clamp(keyLen, 0, 30));
    // wcsncpy_s(dest, size, src, count): count 가 size 보다 작으면 count 자를, 아니면 size-1 자를 옮기고 0 을 붙인다.
    const auto put = [&](std::size_t at, std::size_t from, std::size_t count, std::size_t size) {
        if (size <= count) count = size - 1;
        std::size_t n = 0;
        for (; n < count && from + n < s.size() && s[from + n] != 0 && at + n < 30; ++n) buf[at + n] = s[from + n];
        if (at + n < 30) buf[at + n] = 0;
    };
    const auto fmt = [&](const Token* y, const Token* m, const Token* d) {
        if (y) put(y->units <= 4 ? 4 - y->units : 0, y->at, y->units, y->units <= 4 ? y->units + 0x1A : 0x1E);
        if (m) put(m->units <= 2 ? 6 - m->units : 4, m->at, m->units, m->units <= 2 ? m->units + 0x18 : 0x1A);
        if (d) put(d->units <= 2 ? 8 - d->units : 6, d->at, d->units, d->units <= 2 ? d->units + 0x16 : 0x18);
    };
    const std::vector<Token> t = dateTokens(s, len);
    if (t.size() >= 3) {
        const std::size_t l0 = t[0].units, l1 = t[1].units, l2 = t[2].units;
        if (l0 == 4) fmt(&t[0], &t[1], &t[2]);
        else if (l2 == 4) fmt(&t[2], &t[0], &t[1]);
        else if (l0 == 1 && l1 == 1) fmt(&t[2], &t[0], &t[1]);
        else if (l0 == 1) fmt(&t[0], &t[1], &t[2]);
        else if ((l1 == 1 && l2 == 1) || (l0 == 2 && l1 == 2 && l2 == 2) || l1 == 1 || l2 == 1) fmt(&t[0], &t[1], &t[2]);
        else {
            // 형식에 안 맞으면 글을 셋째 자리부터 옮긴다(한글은 여기서 0 문자까지 읽어 같은 글끼리도 차례가 매번
            // 달라진다. 0 문자 뒤를 읽지 않고 같은 글은 같다고 본다).
            put(2, 0, 27, 28);
            buf[29] = 0;
            // 주민등록번호 꼴(YYMMDD-G…)은 일곱째 자리로 세기를 정한다(0·9 1800, 1·2 1900, 3·4 2000, 5·6 2100, 7 2200).
            if (((l0 == 6 && l1 == 7 && s.size() > 2 && s[2] == u'0') || (s.size() > 2 && s[2] == u'1')) && len >= 8 &&
                digit(s[7])) {
                static constexpr int kCentury[10] = {0, 1, 1, 2, 2, 3, 3, 4, 0, 0};
                const int v = kCentury[s[7] - u'0'] + 18;
                buf[0] = static_cast<char16_t>(u'0' + v / 10);
                buf[1] = static_cast<char16_t>(u'0' + v % 10);
            }
        }
    } else if (t.size() == 2) {
        fmt(&t[0], &t[1], nullptr);
    } else {
        const std::size_t m = std::min<std::size_t>(len, 29);
        if (m <= 4) put(4 - m, 0, m, m + 0x1A);
        else put(0, 0, m, 0x1E);
    }
    if (extendYear && buf[0] == u' ' && buf[1] == u' ' && buf[2] != u' ') {
        const int yy = buf[2] * 10 + buf[3] - 0x210;
        const int c = yearBase / 100 + (yy >= yearBase % 100 ? 0 : 1);
        buf[0] = static_cast<char16_t>(u'0' + c / 10);
        buf[1] = static_cast<char16_t>(u'0' + c % 10);
    }
    return buf;
}

int compareDate(const std::u16string& a, const std::u16string& b) {
    for (std::size_t i = 0; i < 29; ++i) {
        const char16_t x = i < a.size() ? a[i] : 0;
        const char16_t y = i < b.size() ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
        if (x == 0) return 0;
    }
    return 0;
}

// 코드 형식: 키 시작부터 0 문자까지를 베끼고(뒤부터 비교면 그 글 전체를
// 뒤집는다: 필드 키면 문단 끝까지 뒤집혀 문단 끝 쪽부터 견준다), 짧은 키 길이만큼 글자를(확장 문자는 8 단위 건너)
// 코드로 견주고, 같으면 긴 키가 크다.
int compareCode(const std::u16string& a, int la, const std::u16string& b, int lb) {
    const int count = std::max(0, std::min(la, lb));
    std::size_t i = 0, j = 0;
    for (int n = 0; n < count; ++n) {
        const char16_t x = charAt(a, i);
        const char16_t y = charAt(b, j);
        if (x != y) return x < y ? -1 : 1;
        i += unitsOf(x);
        j += unitsOf(y);
    }
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

// 한글 정렬의 qsort. less 가 아니라 비교값을 쓴다.
template <class Cmp>
void hangulQsort(std::vector<int>& a, Cmp cmp) {
    if (a.size() < 2) return;
    std::vector<std::pair<int, int>> stack;
    int lo = 0;
    int hi = static_cast<int>(a.size()) - 1;
    for (;;) {
        const int size = hi - lo + 1;
        if (size <= 8) {
            // 가장 큰 것(같으면 뒤의 것)을 끝으로 보낸다
            for (int h = hi; h > lo; --h) {
                int mx = lo;
                for (int p = lo + 1; p <= h; ++p) {
                    if (cmp(a[static_cast<std::size_t>(p)], a[static_cast<std::size_t>(mx)]) >= 0) mx = p;
                }
                std::swap(a[static_cast<std::size_t>(mx)], a[static_cast<std::size_t>(h)]);
            }
        } else {
            const int mid = lo + size / 2;
            std::swap(a[static_cast<std::size_t>(mid)], a[static_cast<std::size_t>(lo)]);
            int loguy = lo;
            int higuy = hi + 1;
            for (;;) {
                for (++loguy; loguy <= hi && cmp(a[static_cast<std::size_t>(loguy)], a[static_cast<std::size_t>(lo)]) <= 0; ++loguy) {
                }
                for (--higuy; higuy > lo && cmp(a[static_cast<std::size_t>(higuy)], a[static_cast<std::size_t>(lo)]) >= 0; --higuy) {
                }
                if (higuy < loguy) break;
                if (higuy != loguy) std::swap(a[static_cast<std::size_t>(loguy)], a[static_cast<std::size_t>(higuy)]);
            }
            std::swap(a[static_cast<std::size_t>(lo)], a[static_cast<std::size_t>(higuy)]);
            if (higuy - lo > hi - loguy) {
                if (lo + 1 < higuy) stack.emplace_back(lo, higuy - 1);
                if (loguy < hi) {
                    lo = loguy;
                    continue;
                }
            } else {
                if (loguy < hi) stack.emplace_back(loguy, hi);
                if (lo + 1 < higuy) {
                    hi = higuy - 1;
                    continue;
                }
            }
        }
        if (stack.empty()) return;
        lo = stack.back().first;
        hi = stack.back().second;
        stack.pop_back();
    }
}

// 비교 하나. KeyOption 의 형식×2+내림이 0~7 을 벗어나면 한글은 그 비교를 만들지 않고
// 건너뛴다(뒤 비교가 앞 키와 짝지어진다).
struct SortMethod {
    int format = 0;
    bool descending = false;
};

// 비교 수: 첫 기준과, 뒤 기준 가운데 칸(위치)이 0 보다 큰 것. 키 묶음 수도 같다.
std::size_t sortRecordCount(const std::vector<TableSortKey>& keys) {
    if (keys.empty()) return 0;
    std::size_t count = 1;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (keys[i].column > 0) ++count;
    }
    return count;
}

std::vector<SortMethod> sortMethods(const std::vector<TableSortKey>& keys) {
    std::vector<SortMethod> out;
    const std::size_t count = sortRecordCount(keys);
    for (std::size_t i = 0; i < count; ++i) {
        const int code = keys[i].format * 2 + (keys[i].descending ? 1 : 0);
        if (code < 0 || code > 7) continue;
        out.push_back(SortMethod{code / 2, code % 2 != 0});
    }
    return out;
}

// 키 하나: 키 시작부터 원문 끝까지(문단 끝 \r 포함)와 키 길이. 빈 키(present 거짓)는 어느 키보다 작다.
struct SortField {
    bool present = false;
    std::u16string tail;
    int len = 0;
    std::u16string form;  // 형식에 맞춰 미리 바꾼 글
};

void prepareField(SortField& f, const SortMethod& m, const TableSortOptions& options) {
    if (!f.present) return;
    switch (m.format) {
        case 0: f.form = charForm(f.tail, f.len, options.fromRear); break;
        case 2: f.form = dateForm(f.tail, f.len, options.extendYear, options.yearBase); break;
        case 3:
            f.form = untilNul(f.tail);
            if (options.fromRear) reverseUnits(f.form);
            break;
        default: f.form = f.tail; break;
    }
}

int compareField(const SortField& a, const SortField& b, const SortMethod& m, const TableSortOptions& options) {
    if (!a.present || !b.present) return a.present == b.present ? 0 : (a.present ? 1 : -1);
    switch (m.format) {
        case 0:
            // 한글은 고어 정렬(bit2)을 먼저 본다. 길이는 바꾸기 전 키 길이다
            if (options.oldHangul || options.finalFirst) return compareJamo(a.form, a.len, b.form, b.len, !options.oldHangul);
            return compareChar(a.form, b.form);
        case 1: return compareNumber(a.tail, a.len, b.tail, b.len);
        case 2: return compareDate(a.form, b.form);
        default: return compareCode(a.form, a.len, b.form, b.len);
    }
}

// 줄(문단)마다 키 묶음을 한글 qsort 로 정렬한 차례. 내림차순은 결과 부호를 뒤집고, 같으면 다음 비교다.
std::vector<int> sortRecords(std::vector<std::vector<SortField>>& records, const std::vector<SortMethod>& methods,
                             const TableSortOptions& options) {
    for (std::vector<SortField>& row : records) {
        for (std::size_t k = 0; k < methods.size() && k < row.size(); ++k) prepareField(row[k], methods[k], options);
    }
    const auto cmp = [&](int x, int y) {
        const std::vector<SortField>& rx = records[static_cast<std::size_t>(x)];
        const std::vector<SortField>& ry = records[static_cast<std::size_t>(y)];
        for (std::size_t k = 0; k < methods.size() && k < rx.size(); ++k) {
            int r = compareField(rx[k], ry[k], methods[k], options);
            if (r && methods[k].descending) r = -r;
            if (r) return r;
        }
        return 0;
    };
    std::vector<int> order(records.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    hangulQsort(order, cmp);
    return order;
}

// 필드 구분 기호. 기호는 앞 39 글자까지다.
std::u16string delimiterSet(const ParagraphSortOptions& options) {
    switch (options.delimiter) {
        case 0: return u"\t";
        case 1: return u",";
        case 2: return u" ";
        case 3: {
            std::u16string s = untilNul(options.delimiterChars);
            if (s.size() > 39) s.resize(39);
            return s;
        }
        default: return {};
    }
}

struct FieldToken {
    std::size_t at = 0;
    int len = 0;
};

// 한글의 필드 나누기와 같다. 구분 기호를 지나면 남은 길이를
// 다음 글자의 단위(확장 문자 8)로 줄이는 버릇까지 같다.
std::vector<FieldToken> fieldTokens(const std::u16string& s, std::size_t from, int len, const std::u16string& delims,
                                    bool ignoreMulti, int max) {
    std::vector<FieldToken> out;
    const auto delim = [&](char16_t c) { return c != 0 && delims.find(c) != std::u16string::npos; };
    std::size_t p = from;
    int left = len;
    if (charAt(s, p) == 0) return out;
    while (left > 0) {
        FieldToken t;
        if (!ignoreMulti && delim(charAt(s, p))) {
            t.at = p;
            t.len = 0;
            p += unitsOf(charAt(s, p));
            left -= static_cast<int>(unitsOf(charAt(s, p)));
        } else {
            while (charAt(s, p) != 0 && left > 0 && delim(charAt(s, p))) {
                p += unitsOf(charAt(s, p));
                left -= static_cast<int>(unitsOf(charAt(s, p)));
            }
            const std::size_t start = p;
            while (charAt(s, p) != 0 && left > 0) {
                const char16_t c = charAt(s, p);
                if (delim(c)) {
                    p += unitsOf(c);
                    left -= static_cast<int>(unitsOf(charAt(s, p)));
                    break;
                }
                p += unitsOf(c);
                left -= static_cast<int>(unitsOf(c));
                t.len += static_cast<int>(unitsOf(c));
            }
            if (start == p) break;
            t.at = start;
        }
        if (static_cast<int>(out.size()) >= max) break;
        out.push_back(t);
        if (charAt(s, p) == 0) break;
    }
    return out;
}

SortField fieldOf(const std::u16string& text, std::size_t at, int len) {
    SortField f;
    if (len <= 0) return f;
    f.present = true;
    f.tail = text.substr(std::min(at, text.size()));
    f.len = len;
    return f;
}

}  // namespace

TableEdit sortTableRows(Table& table, int row0, int col0, int row1, int col1, const std::vector<TableSortKey>& keys,
                        const TableSortOptions& options) {
    TableEdit out;
    (void)col1;
    if (!tableGridValid(table)) {
        out.error = "표 칸이 겹치거나 비어 있는 자리가 있어 정렬할 수 없습니다.";
        return out;
    }
    if (keys.empty()) {
        out.error = "정렬 기준이 없습니다.";
        return out;
    }
    const std::vector<SortMethod> methods = sortMethods(keys);
    if (methods.empty()) {
        out.error = "모르는 정렬 형식입니다.";
        return out;
    }
    const TableGrid grid = tableGrid(table);
    row0 = std::max(0, row0);
    row1 = std::min(grid.rows - 1, row1);
    if (row1 - row0 < 1) {
        out.error = "두 줄 이상을 칸 블록으로 잡아야 정렬합니다.";
        return out;
    }
    // 줄째 옮기므로 블록 줄에 걸친 합친 칸(줄을 둘 이상 덮거나 블록 밖 줄로 이어진 칸)이 있으면 하지 않는다.
    for (const TableCell& cell : table.cells) {
        const int first = static_cast<int>(cell.row);
        const int last = first + std::max(1, static_cast<int>(cell.rowSpan)) - 1;
        if (last < row0 || first > row1) continue;
        if (first != last) {
            out.error = "여러 줄을 합친 칸이 있는 블록은 정렬하지 않습니다.";
            return out;
        }
    }
    const int rows = row1 - row0 + 1;
    // 키 묶음 i 의 칸: i 번째부터 처음 칸 번호가 0 이 아닌 기준의 칸(블록 첫 칸이 1). 첫 기준 칸이 0 이면 표 첫 칸이다.
    const std::size_t count = sortRecordCount(keys);
    std::vector<int> columns(count, -1);
    for (std::size_t i = 0; i < count; ++i) {
        for (std::size_t j = i; j < keys.size(); ++j) {
            if (keys[j].column != 0) {
                columns[i] = col0 - 1 + keys[j].column;
                break;
            }
            if (j == 0) {
                columns[i] = 0;
                break;
            }
        }
    }
    std::vector<std::vector<SortField>> records(static_cast<std::size_t>(rows));
    for (int r = 0; r < rows; ++r) {
        for (std::size_t i = 0; i < count; ++i) {
            const int c = columns[i];
            const TableCell* cell = nullptr;
            if (c >= 0 && c < grid.cols) {
                const int owner = grid.at(row0 + r, c);
                if (owner >= 0) cell = &table.cells[static_cast<std::size_t>(owner)];
            }
            const std::u16string raw = keyText(cell);
            records[static_cast<std::size_t>(r)].push_back(fieldOf(raw, 0, static_cast<int>(raw.size())));
        }
    }
    const std::vector<int> order = sortRecords(records, methods, options);
    std::vector<int> newRow(static_cast<std::size_t>(rows));
    bool moved = false;
    for (int i = 0; i < rows; ++i) {
        newRow[static_cast<std::size_t>(order[static_cast<std::size_t>(i)])] = i;
        moved = moved || order[static_cast<std::size_t>(i)] != i;
    }
    if (moved) {
        for (TableCell& cell : table.cells) {
            const int r = static_cast<int>(cell.row);
            if (r < row0 || r > row1) continue;
            cell.row = static_cast<std::uint32_t>(row0 + newRow[static_cast<std::size_t>(r - row0)]);
            for (Paragraph& para : cell.content.paragraphs) para.lineSegs.clear();
        }
        normalizeTable(table);
    }
    out.focusCell = tableGrid(table).at(row0, std::max(0, col0));
    return out;
}

std::u16string paragraphSortText(const Paragraph& para, std::vector<std::size_t>* charStarts,
                                 const std::function<bool(const Control&)>& caretChar) {
    std::u16string out;
    if (charStarts) charStarts->clear();
    for (const Run& run : para.runs) {
        for (const InlineItem& item : run.items) {
            if (item.synthetic) continue;
            if (item.type == InlineItem::Type::Control) {
                if (charStarts && caretChar && caretChar(item.control)) charStarts->push_back(out.size());
                appendExtended(out, controlChar(item.control));
                continue;
            }
            const std::u16string units = utf8ToUtf16(item.text);
            for (std::size_t i = 0; i < units.size(); ++i) {
                const char16_t c = units[i];
                // 낮은 대리 글자는 앞 글자(코드 포인트 하나)에 붙는다
                const bool low = c >= 0xDC00 && c <= 0xDFFF && i > 0 && units[i - 1] >= 0xD800 && units[i - 1] <= 0xDBFF;
                if (charStarts && !low) charStarts->push_back(out.size());
                if (c == u'\t') appendExtended(out, 9);
                else if (c == 0xA0) out += char16_t{30};
                else if (c == 0x2007) out += char16_t{31};
                else out += c;
            }
        }
    }
    if (charStarts) charStarts->push_back(out.size());
    return out;
}

std::vector<int> paragraphSortOrder(const std::vector<ParagraphSortText>& paras, const std::vector<TableSortKey>& keys,
                                    const ParagraphSortOptions& options, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return std::vector<int>{};
    };
    if (paras.size() < 2) return fail("글자가 입력된 두 문단 이상을 블록으로 설정하여 정렬하세요.");
    if (keys.empty()) return fail("정렬 기준이 없습니다.");
    const std::vector<SortMethod> methods = sortMethods(keys);
    if (methods.empty()) return fail("모르는 정렬 형식입니다.");
    const std::size_t count = sortRecordCount(keys);
    // 첫 기준이 문단이면 키는 블록 안 글 하나다(뒤 기준은 비어 같다). 아니면 가장 큰 필드 번호까지 나눈다.
    // 필드 번호 200 이 넘는 기준은 키 묶음을 채우지 않는다.
    int maxField = 0;
    if (keys[0].column == 0) {
        maxField = -1;
    } else {
        for (const TableSortKey& k : keys) maxField = std::max(maxField, k.column);
    }
    const std::u16string delims = delimiterSet(options);
    std::vector<std::vector<SortField>> records(paras.size(), std::vector<SortField>(count));
    for (std::size_t p = 0; p < paras.size(); ++p) {
        const ParagraphSortText& s = paras[p];
        const int length = static_cast<int>(s.length);
        if (maxField < 0) {
            records[p][0] = fieldOf(s.text, s.from, length);
            continue;
        }
        const std::vector<FieldToken> tokens =
            fieldTokens(s.text, s.from, length, delims, options.ignoreMultiDelimiter, maxField <= 200 ? maxField : 199);
        std::size_t slot = 0;
        int column = 0;
        for (std::size_t i = 0; i < count && slot < count; ++i) {
            for (std::size_t j = i; j < keys.size(); ++j) {
                column = keys[j].column;
                if (column > 0) break;
            }
            const int field = column - 1;
            if (field < 0 || field >= 200) continue;
            if (field < static_cast<int>(tokens.size())) {
                const FieldToken& t = tokens[static_cast<std::size_t>(field)];
                records[p][slot] = fieldOf(s.text, t.at, t.len);
            }
            ++slot;
        }
    }
    return sortRecords(records, methods, options);
}

int paragraphSortFieldCount(const std::vector<ParagraphSortText>& paras, const ParagraphSortOptions& options) {
    const std::u16string delims = delimiterSet(options);
    int most = 0;
    for (const ParagraphSortText& s : paras) {
        const std::vector<FieldToken> tokens =
            fieldTokens(s.text, s.from, static_cast<int>(s.length), delims, options.ignoreMultiDelimiter, 200);
        most = std::max(most, static_cast<int>(tokens.size()));
    }
    return most;
}

namespace {

// 단어 정렬에서 단어 앞뒤로 빼는 글자: 0x20 이하(탭·컨트롤의 확장 문자도), 유니코드 빈칸.
bool wordSpace(char16_t c) { return spaceLike(c) || c == u'\r' || c == u'\n'; }

struct WordSegment {
    std::size_t at = 0;
    std::size_t units = 0;
};

// 단어를 확장 문자(컨트롤·탭)에서 나눈 글 조각. 문단 끝·줄바꿈에서 끝난다.
std::vector<WordSegment> wordSegments(const std::u16string& s, std::size_t at, std::size_t len) {
    std::vector<WordSegment> out;
    std::size_t i = 0;
    std::size_t start = 0;
    bool stopped = false;
    while (i < len) {
        const char16_t c = charAt(s, at + i);
        if (c == u'\r' || c == u'\n') {
            stopped = true;
            break;
        }
        if (extendedChar(c)) {
            if (i > start) out.push_back(WordSegment{at + start, i - start});
            i += 8;
            start = i;
        } else {
            i += 1;
        }
    }
    if ((stopped || i == len) && i > start) out.push_back(WordSegment{at + start, i - start});
    return out;
}

struct SortWord {
    std::size_t paragraph = 0;
    std::vector<WordSegment> segments;
};

// 문단 조각: 원문 자리와 단위, 그 자리의 글자(코드 포인트 하나) 또는 컨트롤.
struct Piece {
    std::size_t raw = 0;
    std::size_t units = 0;
    std::uint32_t charShapeId = 0;
    InlineItem item;
};

std::vector<Piece> piecesOf(const Paragraph& para) {
    std::vector<Piece> out;
    std::size_t raw = 0;
    for (const Run& run : para.runs) {
        for (const InlineItem& item : run.items) {
            if (item.synthetic) continue;
            if (item.type == InlineItem::Type::Control) {
                out.push_back(Piece{raw, 8, run.charShapeId, item});
                raw += 8;
                continue;
            }
            const std::string& t = item.text;
            for (std::size_t i = 0; i < t.size();) {
                std::size_t n = 1;
                const unsigned char b = static_cast<unsigned char>(t[i]);
                if (b >= 0xF0) n = 4;
                else if (b >= 0xE0) n = 3;
                else if (b >= 0xC0) n = 2;
                Piece p{raw, 1, run.charShapeId, item};
                p.item.text = t.substr(i, n);
                if (p.item.text == "\t") p.units = 8;
                else if (n == 4) p.units = 2;
                out.push_back(std::move(p));
                raw += out.back().units;
                i += n;
            }
        }
    }
    return out;
}

// 조각을 다시 문단 run 으로 묶는다(같은 글자 모양·형광펜·자간 무시인 이웃 글자는 한 글 조각).
void rebuildRuns(Paragraph& para, const std::vector<Piece>& pieces) {
    const std::uint32_t fallback = para.runs.empty() ? 0 : para.runs.front().charShapeId;
    para.runs.clear();
    for (const Piece& p : pieces) {
        if (para.runs.empty() || para.runs.back().charShapeId != p.charShapeId) para.runs.push_back(Run{p.charShapeId, {}});
        std::vector<InlineItem>& items = para.runs.back().items;
        if (p.item.type == InlineItem::Type::Text && !items.empty() && items.back().type == InlineItem::Type::Text &&
            items.back().markpen == p.item.markpen && items.back().ignoreSpacing == p.item.ignoreSpacing &&
            items.back().charStyleId == p.item.charStyleId) {
            items.back().text += p.item.text;
        } else {
            items.push_back(p.item);
        }
    }
    if (para.runs.empty()) para.runs.push_back(Run{fallback, {}});
    para.lineSegs.clear();
}

}  // namespace

bool sortParagraphWords(const std::vector<Paragraph*>& paras, const std::vector<ParagraphSortText>& texts,
                        const std::vector<TableSortKey>& keys, const ParagraphSortOptions& options, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (paras.size() != texts.size() || keys.empty()) return false;
    const std::vector<SortMethod> methods = sortMethods(keys);
    if (methods.empty()) return fail("모르는 정렬 형식입니다.");
    const std::u16string delims = delimiterSet(options);
    // 단어 모으기: 문단마다 구분 기호로 나누고, 앞뒤 빈칸을 뺀 자리를 칸으로, 그 안 글 조각을 키로.
    std::vector<SortWord> words;
    std::vector<std::vector<SortField>> records;
    for (std::size_t p = 0; p < texts.size(); ++p) {
        const ParagraphSortText& s = texts[p];
        const int length = static_cast<int>(s.length);
        if (length <= 0) continue;
        for (const FieldToken& t : fieldTokens(s.text, s.from, length, delims, options.ignoreMultiDelimiter, length)) {
            if (t.len <= 0) continue;
            int first = -1;
            int last = -1;
            for (int i = 0; i < t.len;) {
                const char16_t c = charAt(s.text, t.at + static_cast<std::size_t>(i));
                if (!wordSpace(c)) {
                    if (first < 0) first = i;
                    last = i;
                }
                i += static_cast<int>(unitsOf(c));
            }
            if (first < 0) continue;
            const std::size_t at = t.at + static_cast<std::size_t>(first);
            const std::size_t len = static_cast<std::size_t>(last - first + 1);
            std::vector<WordSegment> segments = wordSegments(s.text, at, len);
            if (segments.empty()) continue;
            SortField f;
            f.present = true;
            if (segments.size() == 1) {
                f.tail = s.text.substr(at);
                f.len = static_cast<int>(len);
            } else {
                for (const WordSegment& g : segments) f.tail += s.text.substr(g.at, g.units);
                f.len = static_cast<int>(f.tail.size());
            }
            words.push_back(SortWord{p, std::move(segments)});
            records.push_back({std::move(f)});
        }
    }
    if (words.empty()) return false;
    // 각 문단별이면 같은 문단 단어끼리, 아니면 블록 단어 모두를 정렬한다.
    std::vector<int> order(words.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    for (std::size_t begin = 0; begin < words.size();) {
        std::size_t end = words.size();
        if (options.eachParagraph) {
            end = begin;
            while (end < words.size() && words[end].paragraph == words[begin].paragraph) ++end;
        }
        std::vector<std::vector<SortField>> group(records.begin() + static_cast<std::ptrdiff_t>(begin),
                                                  records.begin() + static_cast<std::ptrdiff_t>(end));
        const std::vector<int> local = sortRecords(group, methods, options);
        for (std::size_t i = 0; i < local.size(); ++i) order[begin + i] = static_cast<int>(begin) + local[i];
        begin = end;
    }
    bool moved = false;
    for (std::size_t i = 0; i < order.size(); ++i) moved = moved || order[i] != static_cast<int>(i);
    if (!moved) return false;
    // 칸 i 의 글 조각을 지우고, 그 첫 조각 자리에 정렬한 i 번째 단어의 글 조각(글자 모양째)을 넣는다.
    std::vector<std::vector<Piece>> pieces(paras.size());
    for (std::size_t p = 0; p < paras.size(); ++p) pieces[p] = piecesOf(*paras[p]);
    const auto copyOf = [&](const SortWord& w) {
        std::vector<Piece> out;
        for (const WordSegment& g : w.segments) {
            for (const Piece& piece : pieces[w.paragraph]) {
                if (piece.raw >= g.at && piece.raw < g.at + g.units) out.push_back(piece);
            }
        }
        return out;
    };
    std::vector<std::vector<Piece>> next = pieces;
    for (std::size_t p = 0; p < paras.size(); ++p) {
        bool touched = false;
        for (std::size_t i = 0; i < words.size(); ++i) touched = touched || (words[i].paragraph == p && order[i] != static_cast<int>(i));
        if (!touched) continue;
        std::vector<Piece> out;
        for (const Piece& piece : pieces[p]) {
            bool inside = false;
            for (std::size_t i = 0; i < words.size() && !inside; ++i) {
                if (words[i].paragraph != p) continue;
                const SortWord& w = words[i];
                for (const WordSegment& g : w.segments) {
                    if (piece.raw >= g.at && piece.raw < g.at + g.units) {
                        inside = true;
                        if (piece.raw == w.segments.front().at) {
                            const std::vector<Piece> word = copyOf(words[static_cast<std::size_t>(order[i])]);
                            out.insert(out.end(), word.begin(), word.end());
                        }
                        break;
                    }
                }
            }
            if (!inside) out.push_back(piece);
        }
        next[p] = std::move(out);
    }
    for (std::size_t p = 0; p < paras.size(); ++p) {
        bool touched = false;
        for (std::size_t i = 0; i < words.size(); ++i) touched = touched || (words[i].paragraph == p && order[i] != static_cast<int>(i));
        if (touched) rebuildRuns(*paras[p], next[p]);
    }
    return true;
}

}  // namespace chwp::model
