// 블록 계산. 규칙은 block_calc.hpp.
#include "chwp/model/block_calc.hpp"

#include <charconv>
#include <cmath>
#include <string>

namespace chwp::model {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

// text[i] 에서 시작하는 빈칸의 바이트 수(빈칸이 아니면 0).
std::size_t spaceAt(std::string_view text, std::size_t i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f') return 1;
    if (text.compare(i, 2, "\xC2\xA0") == 0) return 2;      // NBSP
    if (text.compare(i, 3, "\xE3\x80\x80") == 0) return 3;  // 전각 빈칸
    return 0;
}

// 소수 decimals 자리로 반올림한 글. 끝 0 과 소수점은 지우고, commas 면 정수 부분에 세 자리 쉼표를 찍는다.
std::string numberText(double value, int decimals, bool commas) {
    const double p = std::pow(10.0, decimals);
    double rounded = std::round(std::fabs(value) * p) / p;
    const bool negative = value < 0 && rounded != 0;
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof(buf), rounded, std::chars_format::fixed, decimals);
    std::string s(buf, r.ptr);
    if (const std::size_t dot = s.find('.'); dot != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (commas) {
        std::size_t end = s.find('.');
        if (end == std::string::npos) end = s.size();
        std::string grouped;
        for (std::size_t i = 0; i < end; ++i) {
            if (i > 0 && (end - i) % 3 == 0) grouped += ',';
            grouped += s[i];
        }
        s = grouped + s.substr(end);
    }
    return negative ? "-" + s : s;
}

}  // namespace

bool blockNumberWord(std::string_view word, double& value, int& decimals, bool& commas) {
    while (!word.empty()) {
        const char c = word.back();
        if (c == ',' || c == ';' || c == ':' || c == '!' || c == '?') word.remove_suffix(1);
        else break;
    }
    if (word.size() > 1 && word.back() == '.' && digit(word[word.size() - 2])) word.remove_suffix(1);
    bool accounting = false;
    if (word.size() >= 3 && word.front() == '(' && word.back() == ')') {
        word = word.substr(1, word.size() - 2);
        accounting = true;
    }
    bool negative = false;
    if (!word.empty() && (word.front() == '+' || word.front() == '-')) {
        negative = word.front() == '-';
        word.remove_prefix(1);
        if (accounting) return false;
    } else if (word.substr(0, 3) == "\xE2\x88\x92") {  // − (U+2212)
        negative = true;
        word.remove_prefix(3);
        if (accounting) return false;
    }
    if (word.empty()) return false;
    std::string plain;
    bool dot = false;
    bool grouped = false;
    int fraction = 0;
    int run = 0;  // 쉼표 뒤로 이어진 정수 숫자 수(쉼표를 썼으면 세 자리씩이어야 한다)
    for (std::size_t i = 0; i < word.size(); ++i) {
        const char c = word[i];
        if (digit(c)) {
            plain += c;
            if (dot) ++fraction;
            else ++run;
        } else if (c == ',' && !dot) {
            // 쉼표는 숫자 사이에만, 첫 묶음 1~3 자리, 다음 묶음은 3 자리.
            if (run == 0 || (grouped ? run != 3 : run > 3) || i + 1 >= word.size() || !digit(word[i + 1])) return false;
            grouped = true;
            run = 0;
        } else if (c == '.' && !dot) {
            if (grouped && run != 3) return false;
            if (i + 1 >= word.size() || !digit(word[i + 1])) return false;
            dot = true;
            plain += '.';
        } else {
            return false;
        }
    }
    if (!dot && grouped && run != 3) return false;
    if (plain.empty() || plain == ".") return false;
    if (plain.front() == '.') plain.insert(plain.begin(), '0');
    double v = 0;
    const auto r = std::from_chars(plain.data(), plain.data() + plain.size(), v);
    if (r.ec != std::errc() || r.ptr != plain.data() + plain.size()) return false;
    value = negative || accounting ? -v : v;
    decimals = fraction;
    commas = grouped;
    return true;
}

BlockNumbers blockNumbers(std::string_view text) {
    BlockNumbers out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (const std::size_t sp = spaceAt(text, i)) {
            i += sp;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && spaceAt(text, j) == 0) ++j;
        double v = 0;
        int decimals = 0;
        bool commas = false;
        if (blockNumberWord(text.substr(i, j - i), v, decimals, commas)) {
            ++out.count;
            out.sum += v;
            if (decimals > out.decimals) out.decimals = decimals;
            out.commas = out.commas || commas;
        }
        i = j;
    }
    return out;
}

std::string blockSumText(const BlockNumbers& numbers) { return numberText(numbers.sum, numbers.decimals, numbers.commas); }

std::string blockAverageText(const BlockNumbers& numbers) {
    if (numbers.count == 0) return {};
    return numberText(numbers.sum / numbers.count, 4, numbers.commas);
}

}  // namespace chwp::model
