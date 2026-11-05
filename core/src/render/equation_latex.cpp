#include "render/equation_latex.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace chwp::render {
namespace {

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

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return out;
}

std::string escapeText(std::string_view text) {
    std::string out;
    for (char ch : text) {
        if (ch == '\\' || ch == '{' || ch == '}' || ch == '$' || ch == '&' || ch == '#' || ch == '%' || ch == '_')
            out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

const std::unordered_map<std::string, std::string>& exactSymbols() {
    static const std::unordered_map<std::string, std::string> table = {
        {"alpha", "\\alpha"},       {"beta", "\\beta"},         {"gamma", "\\gamma"},
        {"delta", "\\delta"},       {"epsilon", "\\epsilon"},   {"zeta", "\\zeta"},
        {"eta", "\\eta"},           {"theta", "\\theta"},       {"iota", "\\iota"},
        {"kappa", "\\kappa"},       {"lambda", "\\lambda"},     {"mu", "\\mu"},
        {"nu", "\\nu"},             {"xi", "\\xi"},             {"omicron", "o"},
        {"pi", "\\pi"},             {"rho", "\\rho"},           {"sigma", "\\sigma"},
        {"tau", "\\tau"},           {"upsilon", "\\upsilon"},   {"phi", "\\phi"},
        {"chi", "\\chi"},           {"psi", "\\psi"},           {"omega", "\\omega"},
        {"Alpha", "A"},             {"ALPHA", "A"},             {"Beta", "B"},
        {"BETA", "B"},              {"Gamma", "\\Gamma"},       {"GAMMA", "\\Gamma"},
        {"Delta", "\\Delta"},       {"DELTA", "\\Delta"},       {"Epsilon", "E"},
        {"EPSILON", "E"},           {"Zeta", "Z"},              {"ZETA", "Z"},
        {"Eta", "H"},               {"ETA", "H"},               {"Theta", "\\Theta"},
        {"THETA", "\\Theta"},       {"Iota", "I"},              {"IOTA", "I"},
        {"Kappa", "K"},             {"KAPPA", "K"},             {"Lambda", "\\Lambda"},
        {"LAMBDA", "\\Lambda"},     {"Mu", "M"},                {"MU", "M"},
        {"Nu", "N"},                {"NU", "N"},                {"Xi", "\\Xi"},
        {"XI", "\\Xi"},             {"Omicron", "O"},           {"OMICRON", "O"},
        {"Pi", "\\Pi"},             {"PI", "\\Pi"},             {"Rho", "P"},
        {"RHO", "P"},               {"Sigma", "\\Sigma"},       {"SIGMA", "\\Sigma"},
        {"Tau", "T"},               {"TAU", "T"},               {"Upsilon", "\\Upsilon"},
        {"UPSILON", "\\Upsilon"},   {"Phi", "\\Phi"},           {"PHI", "\\Phi"},
        {"Chi", "X"},               {"CHI", "X"},               {"Psi", "\\Psi"},
        {"PSI", "\\Psi"},           {"Omega", "\\Omega"},       {"OMEGA", "\\Omega"},
        {"larrow", "\\leftarrow"},  {"rarrow", "\\rightarrow"}, {"uparrow", "\\uparrow"},
        {"downarrow", "\\downarrow"}, {"LARROW", "\\Leftarrow"}, {"RARROW", "\\Rightarrow"},
        {"UPARROW", "\\Uparrow"},   {"DOWNARROW", "\\Downarrow"}, {"udarrow", "\\updownarrow"},
        {"UDARROW", "\\Updownarrow"}, {"lrarrow", "\\leftrightarrow"}, {"LRARROW", "\\Leftrightarrow"},
        {"nwarrow", "\\nwarrow"},   {"nearrow", "\\nearrow"},   {"swarrow", "\\swarrow"},
        {"searrow", "\\searrow"},           {"mapsto", "\\mapsto"},     {"hookleft", "\\hookleftarrow"},
        {"hookright", "\\hookrightarrow"}, {"vert", "|"},       {"VERT", "\\|"},
        {"DEG", "^{\\circ}"},
    };
    return table;
}

const std::unordered_map<std::string, std::string>& foldedSymbols() {
    static const std::unordered_map<std::string, std::string> table = {
        {"times", "\\times"},       {"div", "\\div"},           {"divide", "\\div"},
        {"cdot", "\\cdot"},         {"plusminus", "\\pm"},
        {"minusplus", "\\mp"},              {"circ", "\\circ"},         {"bullet", "\\bullet"},
        {"ast", "\\ast"},           {"star", "\\star"},
        {"emptyset", "\\emptyset"}, {"therefore", "\\therefore"}, {"because", "\\because"},
        {"exist", "\\exists"},      {"neq", "\\neq"},           {"sim", "\\sim"},
        {"approx", "\\approx"},     {"simeq", "\\simeq"},       {"cong", "\\cong"},
        {"equiv", "\\equiv"},       {"propto", "\\propto"},     {"infty", "\\infty"},
        {"inf", "\\infty"},         {"partial", "\\partial"},   {"forall", "\\forall"},
        {"prime", "\\prime"},       {"angle", "\\angle"},       {"bot", "\\perp"},
        {"top", "\\top"},           {"ldots", "\\ldots"},       {"cdots", "\\cdots"},
        {"vdots", "\\vdots"},       {"ddots", "\\ddots"},       {"leq", "\\leq"},
        {"geq", "\\geq"},           {"in", "\\in"},             {"notin", "\\notin"},
        {"subset", "\\subset"},     {"supset", "\\supset"},     {"subseteq", "\\subseteq"},
        {"supseteq", "\\supseteq"}, {"cap", "\\cap"},           {"cup", "\\cup"},
        {"wedge", "\\wedge"},       {"vee", "\\vee"},           {"oplus", "\\oplus"},
        {"ominus", "\\ominus"},     {"otimes", "\\otimes"},     {"odot", "\\odot"},
        {"oslash", "\\oslash"},     {"owns", "\\ni"},           {"prec", "\\prec"},
        {"succ", "\\succ"},         {"aleph", "\\aleph"},       {"hbar", "\\hbar"},
        {"imath", "\\imath"},       {"jmath", "\\jmath"},       {"ell", "\\ell"},
        {"liter", "\\ell"},         {"ohm", "\\Omega"},         {"wp", "\\wp"},
        {"vartheta", "\\vartheta"}, {"varpi", "\\varpi"},       {"varsigma", "\\varsigma"},
        {"varphi", "\\varphi"},     {"varepsilon", "\\varepsilon"}, {"triangle", "\\triangle"},
        {"msangle", "\\measuredangle"}, {"sangle", "\\sphericalangle"}, {"vdash", "\\vdash"},
        {"models", "\\models"},     {"centigrade", "^{\\circ}\\mathrm{C}"},
        {"fahrenheit", "^{\\circ}\\mathrm{F}"},
    };
    return table;
}

// 스펙 3.10 기본 함수, 3.11 예약어. 대소문자를 가린다. 긴 이름이 이긴다.
// sinh 는 sin 이 아니다. 낱말 맨 앞에서만 자른다. xsin 의 sin 은 기울임으로 남는다.
int romanPrefix(std::string_view word) {
    static const char* names[] = {
        "cosec", "arcsin", "arccos", "arctan", "coth", "sinh", "cosh", "tanh", "asin", "acos", "atan",
        "sin",   "cos",    "log",    "tan",    "cot",  "sec",  "max",  "min",  "csc",  "exp",  "Exp",
        "arc",   "det",    "gcd",    "mod",    "lcm",  "ln",   "lg",   "for",  "and",  "hom",  "ker",
        "deg",   "arg",    "dim",    "if",     "Pr",
    };
    int best = 0;
    for (const char* name : names) {
        const int n = static_cast<int>(std::char_traits<char>::length(name));
        if (n <= best || static_cast<int>(word.size()) < n) continue;
        if (word.compare(0, static_cast<std::size_t>(n), name) == 0) best = n;
    }
    return best;
}

bool limitsName(std::string_view word) {
    return word == "max" || word == "min" || word == "det" || word == "gcd" || word == "lcm";
}

bool latinLetter(char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

// \circ 다음에 f 가 붙으면 명령 이름이 circf 가 된다. 글자 명령 뒤에만 공백을 넣는다.
bool endsWithControlWord(std::string_view text) {
    if (text.size() < 2) return false;
    std::size_t i = text.size();
    while (i > 0 && latinLetter(text[i - 1])) --i;
    if (i == 0 || i == text.size() || text[i - 1] != '\\') return false;
    if (i >= 2 && text[i - 2] == '\\') return false;
    return true;
}

void appendLatex(std::string& out, std::string_view next) {
    if (!out.empty() && !next.empty() && latinLetter(next[0]) && endsWithControlWord(out)) out.push_back(' ');
    out.append(next);
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
    for (const auto& entry : exactSymbols()) consider(entry.first);
    for (const auto& entry : foldedSymbols()) consider(entry.first);
    return best;
}

const char* symbolOf(std::string_view word) {
    const auto& exact = exactSymbols();
    if (const auto it = exact.find(std::string(word)); it != exact.end()) return it->second.c_str();
    const auto& folded = foldedSymbols();
    if (const auto it = folded.find(lower(word)); it != folded.end()) return it->second.c_str();
    return nullptr;
}

std::string mapCharSymbol(std::string_view text) {
    if (text == "<->") return "\\leftrightarrow";
    if (text == "->") return "\\rightarrow";
    if (text == "<-") return "\\leftarrow";
    if (text == ">>>") return "\\ggg";
    if (text == "<<<") return "\\lll";
    if (text == "<<") return "\\ll";
    if (text == ">>") return "\\gg";
    if (text == "!=") return "\\neq";
    if (text == "==") return "\\equiv";
    if (text == "<=") return "\\leq";
    if (text == ">=") return "\\geq";
    if (text == "∠") return "\\angle";
    if (text == "△") return "\\triangle";
    if (text == "□") return "\\square";
    if (text == "⊥") return "\\perp";
    if (text == "°") return "^{\\circ}";
    if (text == "×") return "\\times";
    if (text == "÷") return "\\div";
    if (text == "≤") return "\\leq";
    if (text == "≥") return "\\geq";
    if (text == "≠") return "\\neq";
    if (text == "≡") return "\\equiv";
    if (text == "∞") return "\\infty";
    if (text == "→") return "\\rightarrow";
    if (text == "←") return "\\leftarrow";
    if (text == "↔") return "\\leftrightarrow";
    if (text == "+-" || text == "±") return "\\pm";
    if (text == "∓") return "\\mp";
    if (text == "·" || text == "∙") return "\\cdot";
    if (text == "π") return "\\pi";
    if (text == "α") return "\\alpha";
    if (text == "β") return "\\beta";
    if (text == "γ") return "\\gamma";
    if (text == "θ") return "\\theta";
    // % 는 라텍스 주석이다. 그대로 넘기면 45% 가 45 만 남는다.
    if (text == "%") return "\\%";
    if (text.size() == 1) return std::string(text);
    return "\\text{" + escapeText(text) + "}";
}

const char* accentOf(std::string_view word) {
    if (eqFold(word, "bar") || eqFold(word, "overline")) return "\\overline";
    if (eqFold(word, "under") || eqFold(word, "underline")) return "\\underline";
    if (eqFold(word, "hat")) return "\\widehat";
    if (eqFold(word, "check")) return "\\check";
    if (eqFold(word, "tilde")) return "\\widetilde";
    if (eqFold(word, "acute")) return "\\acute";
    if (eqFold(word, "grave")) return "\\grave";
    if (eqFold(word, "dot")) return "\\dot";
    if (eqFold(word, "ddot")) return "\\ddot";
    if (eqFold(word, "vec")) return "\\overrightarrow";
    if (eqFold(word, "dyad")) return "\\overleftrightarrow";
    return nullptr;
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
        const char* ops[] = {"<->", "->", "<-", ">>>", "<<<", "<<", ">>", "!=", "==", "<=", ">=", "+-"};
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

struct Parser {
    std::vector<Token> tokens;
    std::size_t index = 0;
    bool roman = false;
    bool bold = false;
    int braceDepth = 0;

    explicit Parser(std::string_view script) : tokens(tokenize(script)) {}

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

    // 공백 뒤의 lim _ 도 같은 첨자다. 첨자가 있으면 \limits 로 글자 아래에 둔다.
    bool scriptAhead() const {
        std::size_t i = index + 1;
        while (i < tokens.size() && tokens[i].kind == Kind::Space) ++i;
        if (i >= tokens.size()) return false;
        const Kind k = tokens[i].kind;
        return k == Kind::Sup || k == Kind::Sub;
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

    std::string styleWrap(const std::string& text) const {
        if (text.empty()) return text;
        if (bold && roman) return "\\mathbf{" + text + "}";
        if (bold) return "\\boldsymbol{" + text + "}";
        if (roman) return "\\mathrm{" + text + "}";
        return text;
    }

    std::string emitWord(std::string_view word) const {
        std::string out;
        std::size_t i = 0;
        // 맨 앞에서만 함수를 자른다. sin 다음의 cos 도 함수다. xsin 은 통째로 글자다.
        while (i < word.size()) {
            const int n = romanPrefix(word.substr(i));
            if (n <= 0) break;
            const std::string name(word.substr(i, static_cast<std::size_t>(n)));
            if (bold && roman) out += "\\mathbf{" + name + "}";
            else if (bold) out += "\\boldsymbol{" + name + "}";
            else out += "\\mathrm{" + name + "}";
            i += static_cast<std::size_t>(n);
        }
        if (i < word.size()) out += styleWrap(std::string(word.substr(i)));
        return out;
    }

    std::string lines();
    std::string row();
    std::string expr();
    std::string term();
    std::string piece();
    std::string prefix();
    std::string scriptOf();
    std::string readGroupInner();
    std::string bracedArg();
    std::string delimiter();

    // 공백은 항 구분자다. 뒤 단어가 기대하는 이름일 때만 공백을 먹는다.
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

    std::string bigOp(const char* cmd, bool small) {
        take();
        std::string sub;
        std::string sup;
        if (takeNamed("from")) sub = bracedArg();
        if (takeNamed("to")) sup = bracedArg();
        std::string out = small ? std::string("{\\textstyle") + cmd + "}" : cmd;
        if (!sub.empty()) out += "_{" + sub + "}";
        if (!sup.empty()) out += "^{" + sup + "}";
        return out;
    }
};

std::string Parser::readGroupInner() {
    const bool savedRoman = roman;
    const bool savedBold = bold;
    ++braceDepth;
    take();
    std::string inner = lines();
    if (kind() == Kind::RBrace) take();
    --braceDepth;
    roman = savedRoman;
    bold = savedBold;
    return inner;
}

std::string Parser::bracedArg() {
    skipSpace();
    if (kind() == Kind::LBrace) return readGroupInner();
    return piece();
}

std::string Parser::delimiter() {
    skipSpace();
    if (kind() == Kind::LBrace) {
        take();
        return "\\{";
    }
    if (kind() == Kind::RBrace) {
        take();
        return "\\}";
    }
    if (kind() == Kind::Symbol) {
        const std::string text = mapCharSymbol(current().text);
        take();
        return text;
    }
    if (kind() == Kind::Word) {
        const std::string text = current().text;
        take();
        if (eqFold(text, "vert")) return "|";
        if (text == "VERT") return "\\|";
        if (const char* sym = symbolOf(text)) return sym;
        return text;
    }
    return ".";
}

std::string Parser::scriptOf() {
    // lim _ {n} 처럼 첨자 기호와 중괄호 사이의 공백은 첨자 본문 앞의 공백이다.
    skipSpace();
    if (kind() == Kind::LBrace) return readGroupInner();
    std::string out;
    int guard = 0;
    while (guard++ < 64) {
        const Kind k = kind();
        if (k == Kind::End || k == Kind::Space || k == Kind::Sup || k == Kind::Sub || k == Kind::RBrace ||
            k == Kind::Hash || k == Kind::Amp)
            break;
        const std::size_t before = index;
        appendLatex(out, prefix());
        if (index == before) break;
    }
    return out;
}

std::string Parser::prefix() {
    const std::size_t before = index;
    if (kind() == Kind::LBrace) return "{" + readGroupInner() + "}";
    if (kind() == Kind::Thin) {
        take();
        return "\\,";
    }
    if (kind() == Kind::Wide) {
        take();
        return "\\ ";
    }
    if (kind() == Kind::Quote) {
        const std::string text = current().text;
        take();
        return "\\text{" + escapeText(text) + "}";
    }
    if (kind() == Kind::Number) {
        const std::string text = current().text;
        take();
        if (bold && roman) return "\\mathbf{" + text + "}";
        if (bold) return "\\boldsymbol{" + text + "}";
        return text;
    }
    if (kind() == Kind::Symbol) {
        const std::string text = mapCharSymbol(current().text);
        take();
        return text;
    }
    if (kind() == Kind::Sup || kind() == Kind::Sub) {
        const bool sub = kind() == Kind::Sub;
        take();
        return std::string(sub ? "_{" : "^{") + scriptOf() + "}";
    }
    if (kind() != Kind::Word) {
        if (index == before && index < tokens.size()) take();
        return "";
    }

    peelCommand();
    const std::string word = current().text;
    if (eqFold(word, "lim")) {
        const bool under = scriptAhead();
        take();
        const bool capital = !word.empty() && word[0] == 'L';
        if (capital) return under ? "\\operatorname{Lim}\\limits" : "\\operatorname{Lim} ";
        return under ? "\\lim\\limits" : "\\lim ";
    }
    if (scriptAhead() && limitsName(word)) {
        take();
        return "\\mathop{\\mathrm{" + word + "}}\\limits";
    }
    if (eqFold(word, "rm")) {
        take();
        roman = true;
        return "";
    }
    if (eqFold(word, "it")) {
        take();
        roman = false;
        return "";
    }
    if (eqFold(word, "bold")) {
        take();
        bold = true;
        return "";
    }
    if (const char* accent = accentOf(word)) {
        take();
        return std::string(accent) + "{" + bracedArg() + "}";
    }
    if (eqFold(word, "arch")) {
        take();
        return "\\overset{\\frown}{" + bracedArg() + "}";
    }
    if (eqFold(word, "sqrt")) {
        take();
        return "\\sqrt{" + bracedArg() + "}";
    }
    if (eqFold(word, "root")) {
        take();
        const std::string first = bracedArg();
        if (takeNamed("of")) return "\\sqrt[" + first + "]{" + bracedArg() + "}";
        return "\\sqrt{" + first + "}";
    }
    if (eqFold(word, "box")) {
        take();
        return "\\boxed{" + bracedArg() + "}";
    }
    if (eqFold(word, "not")) {
        take();
        const std::string arg = bracedArg();
        if (arg == "=") return "\\neq";
        if (arg == "\\in") return "\\notin";
        return "\\not " + arg;
    }
    if (eqFold(word, "bigg")) {
        take();
        return "\\bigg " + bracedArg();
    }
    if (eqFold(word, "left") || eqFold(word, "right")) {
        const bool left = eqFold(word, "left");
        take();
        return std::string(left ? "\\left" : "\\right") + delimiter();
    }
    if (eqFold(word, "binom")) {
        take();
        const std::string a = bracedArg();
        const std::string b = bracedArg();
        return "\\binom{" + a + "}{" + b + "}";
    }
    if (eqFold(word, "rel")) {
        take();
        const std::string arrow = bracedArg();
        const std::string over = bracedArg();
        const std::string under = bracedArg();
        return "\\overset{" + over + "}{\\underset{" + under + "}{" + arrow + "}}";
    }
    if (eqFold(word, "buildrel")) {
        take();
        const std::string arrow = bracedArg();
        const std::string over = bracedArg();
        return "\\overset{" + over + "}{" + arrow + "}";
    }
    if (eqFold(word, "color")) {
        take();
        skipSpace();
        if (kind() == Kind::LBrace) readGroupInner();
        return bracedArg();
    }
    if (eqFold(word, "scale")) {
        take();
        skipSpace();
        if (kind() == Kind::Number) take();
        return bracedArg();
    }
    if (eqFold(word, "sup") || eqFold(word, "sub")) {
        const bool sub = eqFold(word, "sub");
        take();
        return std::string(sub ? "_{" : "^{") + bracedArg() + "}";
    }
    if (eqFold(word, "lsub") || eqFold(word, "lsup")) {
        const bool sub = eqFold(word, "lsub");
        take();
        return std::string(sub ? "{}_{" : "{}^{") + bracedArg() + "}";
    }
    if (wordIs("int")) return bigOp("\\int", false);
    if (wordIs("oint")) return bigOp("\\oint", false);
    if (wordIs("dint")) return bigOp("\\iint", false);
    if (wordIs("tint")) return bigOp("\\iiint", false);
    if (wordIs("odint")) return bigOp("\\oiint", false);
    if (wordIs("otint")) return bigOp("\\oiiint", false);
    if (wordIs("sum")) return bigOp("\\sum", false);
    if (wordIs("prod")) return bigOp("\\prod", false);
    if (wordIs("coprod")) return bigOp("\\coprod", false);
    if (wordIs("union")) return bigOp("\\bigcup", false);
    if (wordIs("inter")) return bigOp("\\bigcap", false);
    if (wordIs("smallsum")) return bigOp("\\sum", true);
    if (wordIs("smallprod")) return bigOp("\\prod", true);
    if (wordIs("smallunion")) return bigOp("\\cup", true);
    if (wordIs("smallinter")) return bigOp("\\cap", true);
    if (wordIs("cases")) {
        take();
        return "\\begin{cases}" + bracedArg() + "\\end{cases}";
    }
    if (wordIs("pile") || wordIs("lpile") || wordIs("rpile")) {
        const char* col = "c";
        if (wordIs("lpile")) col = "l";
        if (wordIs("rpile")) col = "r";
        take();
        return "\\begin{array}{" + std::string(col) + "}" + bracedArg() + "\\end{array}";
    }
    if (wordIs("eqalign")) {
        take();
        return "\\begin{array}{rl}" + bracedArg() + "\\end{array}";
    }
    if (wordIs("matrix") || wordIs("pmatrix") || wordIs("bmatrix") || wordIs("dmatrix")) {
        std::string env = "matrix";
        if (wordIs("pmatrix")) env = "pmatrix";
        else if (wordIs("bmatrix")) env = "bmatrix";
        else if (wordIs("dmatrix")) env = "vmatrix";
        take();
        return "\\begin{" + env + "}" + bracedArg() + "\\end{" + env + "}";
    }
    if (wordIs("from") || wordIs("to")) {
        take();
        return "";
    }
    if (const char* sym = symbolOf(word)) {
        take();
        return sym;
    }
    // lex 는 ≤ 와 x, gex 는 ≥ 와 x 다. left, leq 는 더 긴 이름이라 여기서 만나지 않는다.
    if (word.size() > 2 && (eqFold(std::string_view(word).substr(0, 2), "le") ||
                            eqFold(std::string_view(word).substr(0, 2), "ge"))) {
        const bool ge = word[0] == 'g' || word[0] == 'G';
        take();
        return std::string(ge ? "\\geq " : "\\leq ") + emitWord(std::string_view(word).substr(2));
    }
    take();
    return emitWord(word);
}

std::string Parser::piece() {
    std::string base = prefix();
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
        const bool sub = kind() == Kind::Sub;
        take();
        base += sub ? "_{" : "^{";
        base += scriptOf();
        base += "}";
    }
    return base;
}

std::string Parser::term() {
    // 중괄호 묶음은 그 자체로 한 항이다. ={pi} over {2} 의 분자는 pi 이고 등호는 밖에 남는다.
    // 중괄호가 없으면 10a^3 over b^2 처럼 공백 전까지가 한 항이다.
    if (kind() == Kind::LBrace) return piece();
    std::string out;
    int guard = 0;
    while (guard++ < 64 && !stopped() && kind() != Kind::Space && kind() != Kind::LBrace && !infix()) {
        const std::size_t before = index;
        appendLatex(out, piece());
        if (index == before) break;
    }
    return out;
}

std::string Parser::expr() {
    struct Item {
        bool op = false;
        std::string text;
    };
    std::vector<Item> items;
    int guard = 0;
    while (guard++ < 256) {
        skipSpace();
        // dy}overdx 의 짝 없는 } 는 없는 셈이다. 묶음 안의 } 는 그 묶음을 닫는다.
        if (braceDepth == 0 && kind() == Kind::RBrace) {
            take();
            continue;
        }
        if (stopped()) break;
        if (infix()) {
            items.push_back(Item{true, lower(current().text)});
            take();
            continue;
        }
        const std::size_t before = index;
        std::string text = term();
        if (!text.empty()) items.push_back(Item{false, std::move(text)});
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
        const std::string& op = items[i].text;
        const std::string left = items[i - 1].text;
        const std::string right = items[i + 1].text;
        std::string merged;
        if (op == "atop") merged = "{" + left + " \\atop " + right + "}";
        else if (op == "choose") merged = "\\binom{" + left + "}{" + right + "}";
        else merged = "\\frac{" + left + "}{" + right + "}";
        items.erase(items.begin() + static_cast<std::ptrdiff_t>(i - 1),
                    items.begin() + static_cast<std::ptrdiff_t>(i + 2));
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(i - 1), Item{false, std::move(merged)});
        i = 0;
    }
    std::string out;
    for (const Item& item : items)
        if (!item.op) appendLatex(out, item.text);
    return out;
}

std::string Parser::row() {
    std::string out = expr();
    while (kind() == Kind::Amp) {
        take();
        out += "&";
        out += expr();
    }
    return out;
}

std::string Parser::lines() {
    std::string out = row();
    while (kind() == Kind::Hash) {
        take();
        out += "\\\\";
        out += row();
    }
    return out;
}

}  // namespace

std::string hangulEquationToLatex(std::string_view script) {
    Parser parser(script);
    return parser.lines();
}

}  // namespace chwp::render
