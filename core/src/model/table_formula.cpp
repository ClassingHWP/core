// 표 계산식(한글 2024 계산식). 규칙은 table_formula.hpp, 정답은 tests/test_table_formula.cpp.
#include "chwp/model/table_formula.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "chwp/model/table_edit.hpp"

namespace chwp::model {

namespace {

constexpr double kPi = 3.14159265358979323846;

bool digit(char c) { return c >= '0' && c <= '9'; }
bool letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

std::string upperOf(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = upper(c);
    return out;
}

// 셀 필드 이름 비교: 한글은 _wcsicmp 로 비교한다(영문 대소문자만 같게 본다).
bool sameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (upper(a[i]) != upper(b[i])) return false;
    }
    return true;
}

const Control* firstFormulaField(const TableCell& cell) {
    for (const Paragraph& p : cell.content.paragraphs) {
        for (const Run& r : p.runs) {
            for (const InlineItem& item : r.items) {
                if (item.type == InlineItem::Type::Control && item.control.kind == ControlKind::Field &&
                    item.control.ctrlId == "%fmu") {
                    return &item.control;
                }
            }
        }
    }
    return nullptr;
}

bool isFormulaBegin(const InlineItem& item) {
    return item.type == InlineItem::Type::Control && item.control.kind == ControlKind::Field && item.control.ctrlId == "%fmu";
}

// 칸의 계산식 필드마다 보이는 결과 글(필드 시작과 끝 사이 글).
std::vector<std::string> formulaResults(const TableCell& cell) {
    std::vector<std::string> out;
    for (const Paragraph& p : cell.content.paragraphs) {
        bool open = false;
        std::string text;
        for (const Run& r : p.runs) {
            for (const InlineItem& item : r.items) {
                if (isFormulaBegin(item)) {
                    open = true;
                    text.clear();
                } else if (open && item.type == InlineItem::Type::Text) {
                    text += item.text;
                } else if (open && item.type == InlineItem::Type::Control && item.control.kind == ControlKind::FieldEnd) {
                    out.push_back(text);
                    open = false;
                }
            }
        }
    }
    return out;
}

// 셀 값: 셀 글에서 처음 나오는 수. 수가 없어도 계산식이 든 셀은 0 이다(숨긴 한글로 확인).
// 우리 규칙: 결과가 오류(잘못된 계산식·순환 참조)인 계산식이 든 칸은 오류다(한글은 0 으로 센다).
struct CellRead {
    bool error = false;
    std::optional<double> value;
};

CellRead cellValue(const TableCell& cell) {
    bool formula = false;
    for (const std::string& result : formulaResults(cell)) {
        formula = true;
        if (result == kFormulaErrorText || result == kFormulaCycleText) return {true, std::nullopt};
    }
    if (const std::optional<double> v = formulaCellNumber(formulaCellText(cell))) return {false, v};
    if (formula || firstFormulaField(cell)) return {false, 0.0};
    return {};
}

// 평균(AVG·AVERAGE)의 ABOVE·LEFT 범위는 바로 위(왼쪽) 셀이 그 방향 소계면 뺀다(한글 2024 와 같다).
// 소계는 셀 첫 계산식(앞 '=' 하나 뺌)에서 처음 나오는 '(' 뒤로 짝 맞는 ')' 가 있고, 그 '(' 부터가
// "(ABOVE)"·"(LEFT)" 로 시작하는 식이다. 대소문자·빈칸은 그대로 비교한다.
bool directionSubtotal(const TableCell& cell, std::string_view dir) {
    const Control* field = firstFormulaField(cell);
    if (!field) return false;
    std::string f = parseFormulaCommand(field->command).formula;
    if (!f.empty() && f.front() == '=') f.erase(f.begin());
    const std::size_t open = f.find('(');
    if (open == std::string::npos) return false;
    int depth = 1;
    std::size_t i = open + 1;
    for (; i < f.size(); ++i) {
        if (f[i] == '(') ++depth;
        else if (f[i] == ')' && --depth == 0) break;
    }
    if (i >= f.size()) return false;
    const std::string want = "(" + std::string(dir) + ")";
    return f.compare(open, want.size(), want) == 0;
}

// 값의 정확한 십진 전개(부호 없이). to_chars 는 주어진 자릿수가 전개보다 길면 정확한 값을 쓴다.
std::string exactDigits(double v) {
    std::array<char, 1500> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), std::fabs(v), std::chars_format::fixed, 1100);
    std::string s(buf.data(), r.ptr);
    // 끝 0 은 지운다(소수점은 남긴다)
    if (const std::size_t dot = s.find('.'); dot != std::string::npos) {
        while (s.size() > dot + 1 && s.back() == '0') s.pop_back();
    }
    return s;
}

// 소수 n 자리에서 0 에서 먼 쪽으로 반올림한 글(정확한 값 기준, 한글 printf %.nf 와 같다).
std::string fixedAway(double v, int n) {
    std::string s = exactDigits(v);
    std::string ip = s.substr(0, s.find('.'));
    std::string fp = s.find('.') == std::string::npos ? std::string() : s.substr(s.find('.') + 1);
    if (static_cast<int>(fp.size()) < n + 1) fp.append(static_cast<std::size_t>(n + 1) - fp.size(), '0');
    const bool up = fp[static_cast<std::size_t>(n)] >= '5';
    fp.resize(static_cast<std::size_t>(n));
    std::string all = ip + fp;
    if (up) {
        int i = static_cast<int>(all.size()) - 1;
        while (i >= 0 && all[static_cast<std::size_t>(i)] == '9') all[static_cast<std::size_t>(i--)] = '0';
        if (i < 0) all.insert(all.begin(), '1');
        else ++all[static_cast<std::size_t>(i)];
    }
    std::string out = all.substr(0, all.size() - static_cast<std::size_t>(n));
    if (out.empty()) out = "0";
    if (n > 0) out += "." + all.substr(all.size() - static_cast<std::size_t>(n));
    if (std::signbit(v)) out.insert(out.begin(), '-');
    return out;
}

std::string withCommas(const std::string& s) {
    std::size_t start = s.empty() || s[0] != '-' ? 0 : 1;
    std::size_t end = s.find('.');
    if (end == std::string::npos) end = s.size();
    std::string ip = s.substr(start, end - start);
    std::string grouped;
    for (std::size_t i = 0; i < ip.size(); ++i) {
        if (i > 0 && (ip.size() - i) % 3 == 0) grouped += ',';
        grouped += ip[i];
    }
    return s.substr(0, start) + grouped + s.substr(end);
}

double pow10(int n) {
    double p = 1.0;
    for (int i = 0; i < n; ++i) p *= 10.0;
    return p;
}

// ROUND(x, n): 유효 숫자 15 자리 십진으로 바꾼 뒤 소수 n 자리에서 0 에서 먼 쪽으로 반올림한다.
double roundDecimal(double x, int n) {
    if (!std::isfinite(x)) return x;
    std::array<char, 64> buf{};
    const auto r = std::to_chars(buf.data(), buf.data() + buf.size(), std::fabs(x), std::chars_format::scientific, 14);
    const std::string s(buf.data(), r.ptr);
    const std::size_t e = s.find('e');
    std::string mant;
    for (std::size_t i = 0; i < e; ++i) {
        if (digit(s[i])) mant += s[i];
    }
    const int exp10 = std::atoi(s.c_str() + e + 1);
    // 값 = 0.mant × 10^(exp10+1). 남길 자리 수 = exp10 + 1 + n
    const int keep = exp10 + 1 + n;
    std::string digits = mant;
    int pointPos = exp10 + 1;  // digits 앞에서 소수점까지 자리
    if (keep < 0) return std::signbit(x) ? -0.0 : 0.0;
    if (keep < static_cast<int>(digits.size())) {
        const bool up = digits[static_cast<std::size_t>(keep)] >= '5';
        digits.resize(static_cast<std::size_t>(keep));
        if (up) {
            int i = keep - 1;
            while (i >= 0 && digits[static_cast<std::size_t>(i)] == '9') digits[static_cast<std::size_t>(i--)] = '0';
            if (i < 0) {
                digits.insert(digits.begin(), '1');
                ++pointPos;
            } else {
                ++digits[static_cast<std::size_t>(i)];
            }
        }
    }
    if (digits.empty()) digits = "0";
    const std::string text = "0." + digits + "e" + std::to_string(pointPos);
    double out = 0;
    std::from_chars(text.data(), text.data() + text.size(), out);
    return std::signbit(x) ? -out : out;
}

// ---- 식 해석 ----

struct CellAddr {
    int col = 0;
    int row = 0;
    bool colQ = false;  // ? 면 식 칸의 칸
    bool rowQ = false;  // ? 면 식 칸의 줄
};

// Dir 은 쉬운 범위 LEFT·RIGHT·ABOVE·BELOW, All 은 ALL:이름. 쌍점이 붙으면(LEFT:이름) named 이고 field 가 이름이다.
enum class Tok { End, Num, Ref, Func, Dir, All, Plus, Minus, Mul, Div, LParen, RParen, Comma, Colon, Bad };

struct Token {
    Tok kind = Tok::End;
    double num = 0;
    CellAddr ref;
    std::string name;
    bool named = false;
    std::string field;
};

std::vector<Token> tokenize(std::string_view raw) {
    // 한글은 식의 빈칸을 모두 지우고 읽는다("=1 2+1" 은 13, "LEFT: x" 는 LEFT:x).
    std::string compact;
    for (const char c : raw) {
        if (c != ' ' && c != '\t') compact += c;
    }
    const std::string_view s = compact;
    std::vector<Token> out;
    std::size_t i = 0;
    const auto space = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; };
    space();
    while (i < s.size() && (s[i] == '=' || s[i] == '@')) {
        ++i;
        space();
    }
    while (true) {
        space();
        if (i >= s.size()) break;
        const char c = s[i];
        Token t;
        if (digit(c)) {
            std::size_t j = i;
            while (j < s.size() && digit(s[j])) ++j;
            if (j < s.size() && s[j] == '.') {
                ++j;
                while (j < s.size() && digit(s[j])) ++j;
            }
            const std::string text(s.substr(i, j - i));
            t.kind = Tok::Num;
            std::from_chars(text.data(), text.data() + text.size() - (text.back() == '.' ? 1 : 0), t.num);
            i = j;
        } else if (letter(c) || c == '?') {
            std::size_t j = i;
            while (j < s.size() && letter(s[j])) ++j;
            const std::string word = upperOf(s.substr(i, j - i));
            std::size_t k = j;
            while (k < s.size() && digit(s[k])) ++k;
            bool question = false;
            if (k == j && k < s.size() && s[k] == '?') {
                question = true;
                ++k;
            }
            if (c == '?' && j == i) {
                // ?1 처럼 칸이 ? 인 셀
                std::size_t m = i + 1;
                std::size_t d = m;
                while (d < s.size() && digit(s[d])) ++d;
                if (d == m) {
                    t.kind = Tok::Bad;
                    i = d;
                } else {
                    t.kind = Tok::Ref;
                    t.ref.colQ = true;
                    t.ref.row = std::atoi(std::string(s.substr(m, d - m)).c_str()) - 1;
                    t.name = "?";
                    i = d;
                }
            } else if (k > j && !word.empty()) {
                int col = 0;
                for (const char ch : word) col = col * 26 + (ch - 'A' + 1);
                t.kind = Tok::Ref;
                t.ref.col = col - 1;
                t.ref.rowQ = question;
                t.ref.row = question ? 0 : std::atoi(std::string(s.substr(j, k - j)).c_str()) - 1;
                i = k;
            } else if ((word == "LEFT" || word == "RIGHT" || word == "ABOVE" || word == "BELOW" || word == "ALL") &&
                       j < s.size() && s[j] == ':') {
                // 필드 이름 범위: 쌍점 뒤 이름은 쉼표·닫는 괄호 앞까지다.
                std::size_t e = j + 1;
                while (e < s.size() && s[e] != ',' && s[e] != ')') ++e;
                t.kind = word == "ALL" ? Tok::All : Tok::Dir;
                t.name = word;
                t.named = true;
                t.field = std::string(s.substr(j + 1, e - j - 1));
                i = e;
            } else if (word == "LEFT" || word == "RIGHT" || word == "ABOVE" || word == "BELOW") {
                t.kind = Tok::Dir;
                t.name = word;
                i = j;
            } else if (!word.empty()) {
                t.kind = Tok::Func;
                t.name = word;
                i = j;
            } else {
                t.kind = Tok::Bad;
                i = j + 1;
            }
        } else {
            switch (c) {
                case '+': t.kind = Tok::Plus; break;
                case '-': t.kind = Tok::Minus; break;
                case '*': t.kind = Tok::Mul; break;
                case '/': t.kind = Tok::Div; break;
                case '(': t.kind = Tok::LParen; break;
                case ')': t.kind = Tok::RParen; break;
                case ',': t.kind = Tok::Comma; break;
                case ':': t.kind = Tok::Colon; break;
                default: t.kind = Tok::Bad; break;
            }
            ++i;
        }
        out.push_back(std::move(t));
    }
    // 이항 연산자 바로 뒤에 * / 가 오면 앞 연산자는 버린다(한글: A1+*B1 = A1*B1).
    std::vector<Token> merged;
    for (Token& t : out) {
        const bool binary = t.kind == Tok::Mul || t.kind == Tok::Div;
        if (binary && !merged.empty()) {
            const Tok prev = merged.back().kind;
            if (prev == Tok::Plus || prev == Tok::Minus || prev == Tok::Mul || prev == Tok::Div) merged.pop_back();
        }
        merged.push_back(std::move(t));
    }
    merged.push_back(Token{});
    return merged;
}

class Evaluator {
public:
    Evaluator(const Table& table, std::size_t cell, std::vector<Token> tokens)
        : table_(table), self_(table.cells[cell]), toks_(std::move(tokens)) {}

    std::optional<double> run() {
        if (toks_.front().kind == Tok::End) return std::nullopt;
        const std::optional<double> v = expr();
        if (!v || peek() != Tok::End || !std::isfinite(*v)) return std::nullopt;
        return v;
    }

    // 식이 값을 읽는 칸들. 계산과 같은 풀이(셀 하나·범위·쉬운 범위·ALL:이름)를 식 전체에서 모은다. 계산은 오류에서
    // 멈추지만 여기는 끝까지 본다.
    void inputs(std::vector<const TableCell*>& out) const {
        std::vector<std::string> funcs;
        for (std::size_t i = 0; i < toks_.size(); ++i) {
            const Token& t = toks_[i];
            switch (t.kind) {
                case Tok::LParen:
                    funcs.push_back(i > 0 && toks_[i - 1].kind == Tok::Func ? toks_[i - 1].name : std::string());
                    break;
                case Tok::RParen:
                    if (!funcs.empty()) funcs.pop_back();
                    break;
                case Tok::Dir: {
                    const bool average = !funcs.empty() && (funcs.back() == "AVG" || funcs.back() == "AVERAGE");
                    directionCells(t, average, out);
                    break;
                }
                case Tok::All: namedCells(t.field, out); break;
                case Tok::Ref:
                    if (i + 2 < toks_.size() && toks_[i + 1].kind == Tok::Colon && toks_[i + 2].kind == Tok::Ref) {
                        rangeCells(t.ref, toks_[i + 2].ref, out);
                        i += 2;
                    } else if (!t.ref.colQ) {
                        if (const TableCell* c = cellAt(t.ref)) out.push_back(c);
                    }
                    break;
                default: break;
            }
        }
    }

private:
    Tok peek() const { return toks_[pos_].kind; }
    const Token& take() { return toks_[pos_ < toks_.size() - 1 ? pos_++ : pos_]; }

    std::optional<double> expr() {
        std::optional<double> v = term();
        while (v && (peek() == Tok::Plus || peek() == Tok::Minus)) {
            const bool plus = take().kind == Tok::Plus;
            // 끝에 남은 연산자는 0 과 셈한다(한글: A1+ = A1)
            const std::optional<double> r = peek() == Tok::End ? std::optional<double>(0.0) : term();
            if (!r) return std::nullopt;
            v = plus ? *v + *r : *v - *r;
        }
        return v;
    }

    std::optional<double> term() {
        std::optional<double> v = unary();
        while (v && (peek() == Tok::Mul || peek() == Tok::Div)) {
            const bool mul = take().kind == Tok::Mul;
            const std::optional<double> r = peek() == Tok::End ? std::optional<double>(0.0) : unary();
            if (!r) return std::nullopt;
            if (!mul && *r == 0) return std::nullopt;
            v = mul ? *v * *r : *v / *r;
        }
        return v;
    }

    std::optional<double> unary() {
        if (peek() == Tok::Plus) {
            take();
            return unary();
        }
        if (peek() == Tok::Minus) {
            take();
            const std::optional<double> v = unary();
            if (!v) return std::nullopt;
            return -*v;
        }
        return primary();
    }

    std::optional<double> primary() {
        const Token& t = take();
        switch (t.kind) {
            case Tok::Num: return t.num;
            case Tok::Ref: {
                if (t.ref.colQ) return std::nullopt;  // ?1 은 범위 끝에만 쓴다
                const TableCell* cell = cellAt(t.ref);
                if (!cell || cell == &self_) return std::nullopt;
                const CellRead read = cellValue(*cell);
                if (read.error) return std::nullopt;
                return read.value.value_or(0.0);
            }
            case Tok::LParen: {
                const std::optional<double> v = expr();
                if (!v || take().kind != Tok::RParen) return std::nullopt;
                return v;
            }
            case Tok::Func: return function(t.name);
            default: return std::nullopt;
        }
    }

    const TableCell* cellAt(CellAddr a) const {
        const int row = a.rowQ ? static_cast<int>(self_.row) : a.row;
        const int col = a.colQ ? static_cast<int>(self_.col) : a.col;
        for (const TableCell& c : table_.cells) {
            if (static_cast<int>(c.row) == row && static_cast<int>(c.col) == col) return &c;
        }
        return nullptr;
    }

    // 범위 안(주소 기준) 셀을 표 차례대로 모은다. 셀이 없거나 식 칸이 들면 거짓.
    bool rangeCells(int r0, int c0, int r1, int c1, std::vector<const TableCell*>& out) const {
        if (r0 > r1 || c0 > c1) return false;
        bool any = false;
        for (const TableCell& c : table_.cells) {
            const int row = static_cast<int>(c.row);
            const int col = static_cast<int>(c.col);
            if (row < r0 || row > r1 || col < c0 || col > c1) continue;
            if (&c == &self_) return false;
            any = true;
            out.push_back(&c);
        }
        return any;
    }

    bool rangeCells(CellAddr a, CellAddr b, std::vector<const TableCell*>& out) const {
        const int r0 = a.rowQ ? static_cast<int>(self_.row) : a.row;
        const int c0 = a.colQ ? static_cast<int>(self_.col) : a.col;
        const int r1 = b.rowQ ? static_cast<int>(self_.row) : b.row;
        const int c1 = b.colQ ? static_cast<int>(self_.col) : b.col;
        return rangeCells(r0, c0, r1, c1, out);
    }

    // 쉬운 범위는 한글이 셀 범위로 바꿔 계산한다(한글 2024 와 같다).
    //   - 이름 없음: ABOVE 는 첫 줄부터 바로 위 줄까지, BELOW 는 아래 줄부터 끝 줄까지(LEFT·RIGHT 도 같다).
    //     평균이면 바로 위(왼쪽) 셀이 그 방향 소계일 때 그 셀을 뺀다(바로 위가 첫 줄이면 빼지 않는다).
    //   - 이름 있음: 바로 이웃 셀부터 바깥으로, 그 주소에 셀이 있고 이름이 같은 동안 잇는다(합친 셀에 덮인 자리에서 끝).
    bool directionCells(const Token& dir, bool average, std::vector<const TableCell*>& out) const {
        const int row = static_cast<int>(self_.row);
        const int col = static_cast<int>(self_.col);
        const int rows = static_cast<int>(table_.rowCount);
        const int cols = static_cast<int>(table_.colCount);
        const bool vertical = dir.name == "ABOVE" || dir.name == "BELOW";
        const int step = dir.name == "ABOVE" || dir.name == "LEFT" ? -1 : 1;
        const int self = vertical ? row : col;
        const int size = vertical ? rows : cols;
        const auto at = [&](int k) { return cellAt(CellAddr{vertical ? col : k, vertical ? k : row}); };
        const auto range = [&](int lo, int hi) {
            return vertical ? rangeCells(lo, col, hi, col, out) : rangeCells(row, lo, row, hi, out);
        };
        if (dir.named && !dir.field.empty()) {
            int first = -1;
            int last = -1;
            for (int k = self + step; k >= 0 && k < size; k += step) {
                const TableCell* c = at(k);
                if (!c || !sameName(c->name, dir.field)) break;
                if (first < 0) first = k;
                last = k;
            }
            if (first < 0) return false;
            return range(std::min(first, last), std::max(first, last));
        }
        if (step > 0) return range(self + 1, size - 1);
        int hi = self - 1;
        if (average && hi > 0) {
            const TableCell* next = at(hi);
            if (next && directionSubtotal(*next, dir.name)) --hi;
        }
        return range(0, hi);
    }

    // ALL:이름 은 표 안에서 이름이 같은 셀 모두다(다른 표는 보지 않는다). 식 칸이 들면 거짓.
    bool namedCells(const std::string& field, std::vector<const TableCell*>& out) const {
        if (field.empty()) return false;
        bool any = false;
        for (const TableCell& c : table_.cells) {
            if (!sameName(c.name, field)) continue;
            if (&c == &self_) return false;
            any = true;
            out.push_back(&c);
        }
        return any;
    }

    // 범위는 셀마다 인자 하나다(한글: MOD(A1:B1) = MOD(A1,B1)). 수가 없는 셀은 값에서 뺀다. 오류 칸이 들면 거짓.
    static bool addCells(const std::vector<const TableCell*>& cells, std::vector<double>& values, int& count) {
        for (const TableCell* c : cells) {
            const CellRead read = cellValue(*c);
            if (read.error) return false;
            if (read.value) values.push_back(*read.value);
        }
        count += static_cast<int>(cells.size());
        return true;
    }

    // 함수 인자.
    //   - 쉬운 범위(LEFT)는 혼자 써야 한다.
    //   - 이름 붙은 쉬운 범위(LEFT:이름, LEFT:)는 첫 인자로만 쓰고, 같은 괄호 안의 뒤 인자는 버린다.
    //   - ALL:이름, 범위 A1:B2, 셀 하나(수가 없으면 뺀다), 식은 섞어 쓴다.
    bool arguments(const std::string& func, std::vector<double>& values, int& count) {
        if (take().kind != Tok::LParen) return false;
        count = 0;
        if (peek() == Tok::Dir) {
            const Token dir = take();
            std::vector<const TableCell*> cells;
            if (!directionCells(dir, func == "AVG" || func == "AVERAGE", cells)) return false;
            if (dir.named) {
                int depth = 0;
                while (true) {
                    const Tok k = take().kind;
                    if (k == Tok::End) return false;
                    if (k == Tok::LParen) ++depth;
                    if (k == Tok::RParen && depth-- == 0) break;
                }
            } else if (take().kind != Tok::RParen) {
                return false;
            }
            return addCells(cells, values, count);
        }
        while (true) {
            if (peek() == Tok::All) {
                std::vector<const TableCell*> cells;
                if (!namedCells(take().field, cells) || !addCells(cells, values, count)) return false;
            } else if (peek() == Tok::Ref && toks_[pos_ + 1].kind == Tok::Colon) {
                const CellAddr a = take().ref;
                take();
                if (peek() != Tok::Ref) return false;
                const CellAddr b = take().ref;
                std::vector<const TableCell*> cells;
                if (!rangeCells(a, b, cells) || !addCells(cells, values, count)) return false;
            } else if (peek() == Tok::Ref && (toks_[pos_ + 1].kind == Tok::Comma || toks_[pos_ + 1].kind == Tok::RParen)) {
                const CellAddr a = take().ref;
                if (a.colQ) return false;
                const TableCell* cell = cellAt(a);
                if (!cell || cell == &self_ || !addCells({cell}, values, count)) return false;
            } else {
                const std::optional<double> v = expr();
                if (!v) return false;
                values.push_back(*v);
                ++count;
            }
            const Tok next = take().kind;
            if (next == Tok::RParen) return true;
            if (next != Tok::Comma) return false;
        }
    }

    std::optional<double> function(const std::string& name) {
        std::vector<double> v;
        int count = 0;
        if (!arguments(name, v, count) || count == 0) return std::nullopt;
        const auto one = [&](double (*f)(double)) -> std::optional<double> {
            if (count != 1 || v.size() != 1) return count == 1 && v.empty() ? std::optional<double>(f(0.0)) : std::nullopt;
            return f(v[0]);
        };
        if (name == "SUM") {
            double s = 0;
            for (const double x : v) s += x;
            return s;
        }
        if (name == "AVG" || name == "AVERAGE") {
            if (v.empty()) return 0.0;
            double s = 0;
            for (const double x : v) s += x;
            return s / static_cast<double>(v.size());
        }
        if (name == "PRODUCT") {
            if (v.empty()) return 0.0;
            double p = 1;
            for (const double x : v) p *= x;
            return p;
        }
        if (name == "MIN") return v.empty() ? 0.0 : *std::min_element(v.begin(), v.end());
        if (name == "MAX") return v.empty() ? 0.0 : *std::max_element(v.begin(), v.end());
        if (name == "COUNT") return static_cast<double>(v.size());
        if (name == "ROUND" || name == "MOD") {
            if (count != 2 || v.size() != 2) return std::nullopt;
            if (name == "MOD") {
                if (v[1] == 0) return std::nullopt;
                return std::fmod(v[0], v[1]);
            }
            return roundDecimal(v[0], static_cast<int>(v[1]));
        }
        if (name == "COS") return one([](double x) { return std::cos(x); });
        if (name == "SIN") return one([](double x) { return std::sin(x); });
        if (name == "TAN") return one([](double x) { return std::tan(x); });
        if (name == "ACOS") return one([](double x) { return std::acos(x); });
        if (name == "ASIN") return one([](double x) { return std::asin(x); });
        if (name == "ATAN") return one([](double x) { return std::atan(x); });
        if (name == "ABS") return one([](double x) { return std::fabs(x); });
        if (name == "EXP") return one([](double x) { return std::exp(x); });
        if (name == "LN") return one([](double x) { return x > 0 ? std::log(x) : NAN; });
        if (name == "LOG") return one([](double x) { return x > 0 ? std::log10(x) : NAN; });
        if (name == "SQRT") return one([](double x) { return x >= 0 ? std::sqrt(x) : NAN; });
        if (name == "DEGTORAD" || name == "RADIAN") return one([](double x) { return x * kPi / 180.0; });
        if (name == "RADTODEG") return one([](double x) { return x * 180.0 / kPi; });
        if (name == "SIGN") return one([](double x) { return x > 0 ? 1.0 : (x < 0 ? -1.0 : 0.0); });
        if (name == "CEILING") return one([](double x) { return std::ceil(x); });
        if (name == "FLOOR" || name == "INT") return one([](double x) { return std::floor(x); });
        return std::nullopt;
    }

    const Table& table_;
    const TableCell& self_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
};

const char* funcName(FormulaFunc func) {
    switch (func) {
        case FormulaFunc::Average: return "AVG";
        case FormulaFunc::Product: return "PRODUCT";
        default: return "SUM";
    }
}

std::string colName(int col) {
    std::string out;
    for (int c = col + 1; c > 0; c = (c - 1) / 26) out.insert(out.begin(), static_cast<char>('A' + (c - 1) % 26));
    return out;
}

}  // namespace

FormulaCommand parseFormulaCommand(std::string_view command) {
    FormulaCommand f;
    // 식과 형식은 마지막 결과(;; 뒤) 앞의 마지막 ?? 로 나눈다(한글: "=a???%g," 의 식은 "=a?").
    const std::size_t results = command.find(";;");
    const std::size_t q = command.substr(0, results).rfind("??");
    if (q == std::string_view::npos) {
        const std::size_t semi = command.find(';');
        f.formula = std::string(command.substr(0, semi));
        if (semi != std::string_view::npos) f.result = std::string(command.substr(semi + 1));
        return f;
    }
    f.hasFormat = true;
    f.formula = std::string(command.substr(0, q));
    const std::string_view rest = command.substr(q + 2);
    const std::size_t semi = rest.find(";;");
    f.format = std::string(rest.substr(0, semi));
    if (semi != std::string_view::npos) f.result = std::string(rest.substr(semi + 2));
    else if (!f.format.empty() && f.format.back() == ';') f.format.pop_back();
    return f;
}

std::string formulaCommandText(const FormulaCommand& f) {
    if (!f.hasFormat) return f.formula + ";" + f.result;
    return f.formula + "??" + f.format + ";;" + f.result;
}

std::optional<double> formulaCellNumber(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size() && !digit(text[i]) && text[i] != '+' && text[i] != '-' && text[i] != '.') ++i;
    if (i >= text.size()) return std::nullopt;
    std::string num;
    if (text[i] == '+' || text[i] == '-') {
        if (text[i] == '-') num += '-';
        ++i;
    }
    bool dot = false;
    bool any = false;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (digit(c)) {
            num += c;
            any = true;
        } else if (c == ',') {
            continue;
        } else if (c == '.' && !dot) {
            dot = true;
            num += '.';
        } else {
            break;
        }
    }
    if (!any) return 0.0;
    if (num.back() == '.') num.pop_back();
    if (num.size() > 1 && num[0] == '-' && num[1] == '.') num.insert(1, "0");
    if (!num.empty() && num[0] == '.') num.insert(0, "0");
    double v = 0;
    const char* begin = num.data() + (num[0] == '-' ? 1 : 0);
    std::from_chars(begin, num.data() + num.size(), v);
    return num[0] == '-' ? -v : v;
}

std::string formulaCellText(const TableCell& cell) {
    std::string out;
    for (std::size_t i = 0; i < cell.content.paragraphs.size(); ++i) {
        if (i) out += '\r';
        out += cell.content.paragraphs[i].text();
    }
    return out;
}

std::optional<double> evaluateTableFormula(const Table& table, std::size_t cell, std::string_view formula) {
    if (cell >= table.cells.size()) return std::nullopt;
    Evaluator e(table, cell, tokenize(formula));
    return e.run();
}

std::string formatFormulaValue(double value, std::string_view format) {
    const bool commas = format.find(',') != std::string_view::npos;
    int decimals = -1;  // -1 은 %g
    if (format.size() >= 3 && format[0] == '%' && format[1] == '.') {
        std::size_t i = 2;
        int n = 0;
        while (i < format.size() && digit(format[i])) n = n * 10 + (format[i++] - '0');
        if (i < format.size() && format[i] == 'f' && i > 2) decimals = n;
    }
    std::string out;
    if (decimals < 0) {
        out = fixedAway(value, 6);
        if (out.find('.') != std::string::npos) {
            while (out.back() == '0') out.pop_back();
            if (out.back() == '.') out.pop_back();
        }
    } else {
        // 값×10^N 을 0.5 더해 내리고(절댓값) 다시 나눈 값을 N 자리로 쓴다.
        const double p = pow10(decimals);
        const double r = std::floor(std::fabs(value * p) + 0.5);
        out = fixedAway(std::copysign(r, value) / p, decimals);
    }
    return commas ? withCommas(out) : out;
}

std::string formulaResultText(const Table& table, std::size_t cell, std::string_view formula, std::string_view format) {
    const std::optional<double> v = evaluateTableFormula(table, cell, formula);
    return v ? formatFormulaValue(*v, format) : std::string(kFormulaErrorText);
}

Run formulaFieldRun(const std::string& command, std::uint32_t charShapeId) {
    Control begin;
    begin.kind = ControlKind::Field;
    begin.ctrlId = "%fmu";
    begin.command = command;
    begin.fieldExtra = kFormulaFieldExtra;
    Control end;
    end.kind = ControlKind::FieldEnd;
    end.ctrlId = "fend";
    Run run;
    run.charShapeId = charShapeId;
    run.items.push_back(InlineItem::makeControl(std::move(begin)));
    run.items.push_back(InlineItem::makeText(parseFormulaCommand(command).result));
    run.items.push_back(InlineItem::makeControl(std::move(end)));
    return run;
}

bool tableHasFormulas(const Table& table) {
    for (const TableCell& cell : table.cells) {
        for (const Paragraph& p : cell.content.paragraphs) {
            for (const Run& run : p.runs) {
                for (const InlineItem& item : run.items) {
                    if (item.type == InlineItem::Type::Control && item.control.kind == ControlKind::Field &&
                        item.control.ctrlId == "%fmu") {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

namespace {

// 계산식 필드 runs[r].items[k] 의 결과 글과 명령의 마지막 결과를 text 로 바꾼다. 바뀌었으면 참.
bool setFormulaResult(Paragraph& para, std::size_t r, std::size_t k, const std::string& text) {
    if (r >= para.runs.size() || k >= para.runs[r].items.size()) return false;
    FormulaCommand f = parseFormulaCommand(para.runs[r].items[k].control.command);
    // 필드 끝까지의 결과 글을 새 결과 하나로 바꾼다.
    std::string old;
    bool placed = false;
    std::size_t rr = r;
    std::size_t kk = k + 1;
    while (rr < para.runs.size()) {
        std::vector<InlineItem>& items = para.runs[rr].items;
        bool ended = false;
        while (kk < items.size()) {
            InlineItem& it = items[kk];
            if (it.type == InlineItem::Type::Control && it.control.kind == ControlKind::FieldEnd) {
                ended = true;
                break;
            }
            if (it.type == InlineItem::Type::Text) {
                old += it.text;
                if (!placed) {
                    it.text = text;
                    placed = true;
                    ++kk;
                } else {
                    items.erase(items.begin() + static_cast<std::ptrdiff_t>(kk));
                }
                continue;
            }
            ++kk;
        }
        if (ended) break;
        ++rr;
        kk = 0;
    }
    if (!placed) {
        std::vector<InlineItem>& items = para.runs[r].items;
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(k + 1), InlineItem::makeText(text));
    }
    f.result = text;
    const std::string command = formulaCommandText(f);
    InlineItem& field = para.runs[r].items[k];
    const bool changed = old != text || field.control.command != command;
    field.control.command = command;
    if (changed) para.lineSegs.clear();
    return changed;
}

// 칸의 계산식 필드마다 fn(para, run, item) 을 부른다.
template <typename Fn>
void forEachFormulaField(TableCell& cell, Fn&& fn) {
    for (Paragraph& para : cell.content.paragraphs) {
        for (std::size_t r = 0; r < para.runs.size(); ++r) {
            for (std::size_t k = 0; k < para.runs[r].items.size(); ++k) {
                if (isFormulaBegin(para.runs[r].items[k])) fn(para, r, k);
            }
        }
    }
}

}  // namespace

bool refreshFormulaField(Table& table, std::size_t cell, Paragraph& para, std::size_t r, std::size_t k) {
    if (r >= para.runs.size() || k >= para.runs[r].items.size()) return false;
    const FormulaCommand f = parseFormulaCommand(para.runs[r].items[k].control.command);
    return setFormulaResult(para, r, k, formulaResultText(table, cell, f.formula, f.hasFormat ? f.format : "%g"));
}

std::vector<std::size_t> formulaInputCells(const Table& table, std::size_t cell, std::string_view formula) {
    std::vector<std::size_t> out;
    if (cell >= table.cells.size()) return out;
    const Evaluator e(table, cell, tokenize(formula));
    std::vector<const TableCell*> cells;
    e.inputs(cells);
    for (const TableCell* c : cells) {
        const std::size_t index = static_cast<std::size_t>(c - table.cells.data());
        if (index < table.cells.size() && std::find(out.begin(), out.end(), index) == out.end()) out.push_back(index);
    }
    return out;
}

bool recalcTableFormulas(Table& table) {
    const std::size_t n = table.cells.size();
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const TableCell& x = table.cells[a];
        const TableCell& y = table.cells[b];
        return x.row != y.row ? x.row < y.row : x.col < y.col;
    });
    std::vector<char> formula(n, 0);
    for (std::size_t i = 0; i < n; ++i) formula[i] = firstFormulaField(table.cells[i]) != nullptr;
    // 계산식 칸마다 먼저 계산할 계산식 칸(그 칸이 읽는 계산식 칸).
    std::vector<std::vector<std::size_t>> needs(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (!formula[i]) continue;
        forEachFormulaField(table.cells[i], [&](Paragraph& para, std::size_t r, std::size_t k) {
            const FormulaCommand f = parseFormulaCommand(para.runs[r].items[k].control.command);
            for (const std::size_t j : formulaInputCells(table, i, f.formula)) {
                if (j != i && formula[j] && std::find(needs[i].begin(), needs[i].end(), j) == needs[i].end()) {
                    needs[i].push_back(j);
                }
            }
        });
    }
    bool changed = false;
    std::vector<char> done(n, 0);
    // 먼저 계산할 칸이 모두 끝난 칸을 칸 차례로 계산한다. 더 고를 칸이 없는데 남은 칸은 순환에 걸렸다.
    for (bool progress = true; progress;) {
        progress = false;
        for (const std::size_t ci : order) {
            if (!formula[ci] || done[ci]) continue;
            bool ready = true;
            for (const std::size_t j : needs[ci]) ready = ready && done[j];
            if (!ready) continue;
            forEachFormulaField(table.cells[ci], [&](Paragraph& para, std::size_t r, std::size_t k) {
                changed = refreshFormulaField(table, ci, para, r, k) || changed;
            });
            done[ci] = 1;
            progress = true;
        }
    }
    for (const std::size_t ci : order) {
        if (!formula[ci] || done[ci]) continue;
        forEachFormulaField(table.cells[ci], [&](Paragraph& para, std::size_t r, std::size_t k) {
            changed = setFormulaResult(para, r, k, kFormulaCycleText) || changed;
        });
    }
    return changed;
}

int unwrapEditedFormulas(Paragraph& para) {
    int unwrapped = 0;
    for (std::size_t r = 0; r < para.runs.size(); ++r) {
        std::size_t k = 0;
        while (k < para.runs[r].items.size()) {
            if (!isFormulaBegin(para.runs[r].items[k])) {
                ++k;
                continue;
            }
            // 필드 끝을 찾으며 보이는 결과 글을 모은다.
            std::string shown;
            std::size_t er = r;
            std::size_t ek = k + 1;
            bool ended = false;
            while (er < para.runs.size()) {
                const std::vector<InlineItem>& items = para.runs[er].items;
                for (; ek < items.size(); ++ek) {
                    const InlineItem& it = items[ek];
                    if (it.type == InlineItem::Type::Control && it.control.kind == ControlKind::FieldEnd) {
                        ended = true;
                        break;
                    }
                    if (it.type == InlineItem::Type::Text) shown += it.text;
                }
                if (ended) break;
                ++er;
                ek = 0;
            }
            if (!ended || shown == parseFormulaCommand(para.runs[r].items[k].control.command).result) {
                ++k;
                continue;
            }
            // 끝을 먼저 빼야 시작 자리가 그대로다. 시작을 빼면 같은 k 를 다시 본다.
            para.runs[er].items.erase(para.runs[er].items.begin() + static_cast<std::ptrdiff_t>(ek));
            para.runs[r].items.erase(para.runs[r].items.begin() + static_cast<std::ptrdiff_t>(k));
            para.lineSegs.clear();
            ++unwrapped;
        }
    }
    return unwrapped;
}

std::string formulaDefaultFormat(FormulaFunc func) { return func == FormulaFunc::Average ? "%.2f," : "%g,"; }

std::string easyFormula(FormulaFunc func, bool horizontal, bool lastLine) {
    const char* dir = horizontal ? (lastLine ? "LEFT" : "RIGHT") : (lastLine ? "ABOVE" : "BELOW");
    return std::string("=") + funcName(func) + "(" + dir + ")";
}

int insertBlockFormulas(Table& table, int row0, int col0, int row1, int col1, FormulaFunc func) {
    if (row0 > row1 || col0 > col1) return 0;
    const auto cellAt = [&](int row, int col) -> TableCell* {
        for (TableCell& c : table.cells) {
            if (static_cast<int>(c.row) == row && static_cast<int>(c.col) == col) return &c;
        }
        return nullptr;
    };
    // 블록 안에서 그 칸(줄)의 셀에 수가 하나도 없으면 결과 칸(줄)이다.
    const auto numberFree = [&](bool column, int line) {
        for (const TableCell& c : table.cells) {
            const int row = static_cast<int>(c.row);
            const int col = static_cast<int>(c.col);
            if (row < row0 || row > row1 || col < col0 || col > col1) continue;
            if ((column ? col : row) != line) continue;
            if (formulaCellNumber(formulaCellText(c))) return false;
        }
        return true;
    };
    const bool sumColumn = numberFree(true, col1);
    const bool sumRow = numberFree(false, row1);
    if (!sumColumn && !sumRow) return 0;
    const int lastRow = sumRow ? row1 - 1 : row1;
    const int lastCol = sumColumn ? col1 - 1 : col1;
    const std::string format = formulaDefaultFormat(func);
    const std::string name = funcName(func);
    struct Place {
        int row;
        int col;
        std::string formula;
    };
    std::vector<Place> places;
    if (sumColumn && lastCol >= col0) {
        for (int r = row0; r <= lastRow; ++r) {
            places.push_back({r, col1, "=" + name + "(" + colName(col0) + "?:" + colName(lastCol) + "?)"});
        }
    }
    if (sumRow && lastRow >= row0) {
        for (int c = col0; c <= lastCol; ++c) {
            places.push_back({row1, c, "=" + name + "(?" + std::to_string(row0 + 1) + ":?" + std::to_string(lastRow + 1) + ")"});
        }
    }
    if (sumColumn && sumRow && lastRow >= row0 && lastCol >= col0) {
        places.push_back({row1, col1, "=" + name + "(" + colName(col0) + std::to_string(row0 + 1) + ":" + colName(lastCol) +
                                          std::to_string(lastRow + 1) + ")"});
    }
    int made = 0;
    for (const Place& place : places) {
        TableCell* cell = cellAt(place.row, place.col);
        if (!cell) continue;
        if (cell->content.paragraphs.empty()) cell->content.paragraphs.emplace_back();
        Paragraph& para = cell->content.paragraphs.front();
        const std::size_t index = static_cast<std::size_t>(cell - table.cells.data());
        FormulaCommand f;
        f.formula = place.formula;
        f.format = format;
        f.hasFormat = true;
        f.result = formulaResultText(table, index, f.formula, f.format);
        const std::uint32_t shape = para.runs.empty() ? 0 : para.runs.front().charShapeId;
        para.runs.insert(para.runs.begin(), formulaFieldRun(formulaCommandText(f), shape));
        para.lineSegs.clear();
        ++made;
    }
    return made;
}

}  // namespace chwp::model
