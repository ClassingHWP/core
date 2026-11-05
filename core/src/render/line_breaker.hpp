#pragma once

// 문단 글자를 줄로 나누는 공용 단계.
// 쪽 조판(layoutColumn)과 편집 뒤 문단 조판이 같은 규칙으로 줄을 끊는다.
// 원자 만들기는 글꼴을 고르는 scene.cpp 에 있고, 여기서는 원자를 받아 한 줄씩 채운다.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/scene.hpp"

namespace chwp::render::detail {

inline constexpr std::int32_t kSlack = 0;
inline constexpr std::int32_t kEm = 1000;

bool isHangul(char32_t cp);
// 한글 음절과 자모. 줄은 이 글자마다 끊고, 글꼴 선택은 음절만 본다.
bool isHangulWordChar(char32_t cp);
// 문단 모양의 자동 간격 비트. 1 한글과 영어, 2 한글과 숫자.
int autoSpacingFlags(const model::ParaShape* paraShape);
// 두 글자 사이 자동 간격. 한글과 영어(또는 숫자)가 붙어 있으면 띄어쓰기 폭의 절반 2 × ⌊크기 ÷ 8⌋ 이다
// (한글 2024, tmp/autospace: 7pt 174, 10pt 250, 13pt 324, 20pt 500).
std::int32_t autoSpacingGap(char32_t left, char32_t right, int flags, int size);
bool isCjk(char32_t cp);
bool isLatinWordChar(char32_t cp);
bool isSmallKana(char32_t cp);
// 일반 공백, 전각 공백, U+2000–U+200B. 넘어도 그 글자 앞에서 끊지 않는다.
bool isOverflowGap(char32_t cp);
// 줄 머리에 두면 안 되는 글자. 줄이 이미 꽉 찼으면 넘쳐도 그 줄에 남긴다.
bool lineHeadForbidden(char32_t cp);
void appendUtf8(std::string& out, char32_t cp);

// 글자 모양에서 읽는 그리기 모양. 글자 묶음과 줄 글자 표가 같이 쓴다. 글자 모양이 없으면 모두 비어 있다.
struct CharPaint {
    std::string color;
    std::string shade;
    std::string fillColor;   // 글자 테두리/배경의 면색
    std::string hatchStyle;  // 글자 테두리/배경의 무늬
    std::string hatchColor;
    std::string underline;   // 비면 밑줄 없음
    bool bold = false;
    bool italic = false;
    bool strikeout = false;
    bool superscript = false;
    bool subscript = false;
    bool bitmapDeco = false;  // 밑줄·취소선·강조점·글자 테두리
};

// scene.cpp.
CharPaint charPaint(const model::Document& doc, std::uint32_t charShapeId);

struct Atom {
    char32_t cp = 0;
    std::int32_t width = 0;
    int pos = 0;
    int fontSize = 0;
    std::uint32_t charShapeId = 0;
    std::string font;
    std::int32_t ratio = 100;
    std::int32_t relSize = 100;
    std::int32_t spacing = 0;
    std::int32_t charOffset = 0;
    std::string markpen;
    bool space = false;
    bool tab = false;
    std::string tabLeader;
    bool hard = false;
    bool breakBefore = true;
    // 머리말·꼬리말·쪽 번호. 본문에는 그리지 않고, 줄에 이미 내용이 있게 만든다.
    bool occupiesLine = false;
    std::string noteText;  // 각주·미주 번호. 비어 있으면 본문에 글자를 두지 않는다
    int inkSize = 0;       // 번호를 그리는 크기. 0이면 fontSize
    std::int32_t trail = 0;
    // 한글과 영어·숫자 자동 간격. 이 글자 뒤에 더한다. 폭을 잴 때 width 와 trail 에 같이 넣는다.
    std::int32_t autoGap = 0;
    const model::Control* control = nullptr;
    bool metricsReady = false;
    bool ignoreSpacing = false;
    bool synthetic = false;  // 자동 번호 대신 넣은 글자. 커서 칸이 아니다
    std::int32_t natural = 0;  // 글자 격자로 폭을 칸에 맞췄을 때 원래 글자 폭. 0 이면 width 그대로
    std::int32_t naturalTrail = 0;  // natural 에 든 자간. 세로쓰기는 줄 폭과 자간을 다시 재서 가로 자간을 여기 남긴다
    bool vertical = false;          // 세로쓰기 칸 폭(verticalAtom)을 이미 넣었다
};

// 글자 격자의 한 글자 자리. width 는 칸 수 × 칸 폭, lead 는 그 자리 안에서 글자를 미는 폭이다.
struct GridCell {
    std::int32_t width = 0;
    std::int32_t lead = 0;
};

// 글자 격자로 글자 자리를 나눈다. cps 의 0 은 글자가 아닌 것(개체·탭)이라 폭을 그대로 두고 짝을 끊는다.
// 글자는 폭을 칸 폭의 배수로 올리고 그 가운데에 둔다.
// 원고지 정서법이면 반각(숫자·영문 소문자·반각 문장 부호) 둘이 한 칸을 반씩 쓰고,
// 혼자 남은 문장 부호는 칸 왼쪽 반에 둔다. 영문 대문자와 빈칸은 한 칸씩이다.
std::vector<GridCell> charGridCells(const std::vector<char32_t>& cps, const std::vector<std::int32_t>& widths,
                                    std::int32_t cell, bool wongoji);

// 세로쓰기에서 글자를 세우는지. 세운 글자는 1em 을 쓰고, 눕힌 글자는 가로 폭 그대로 오른쪽으로 눕는다.
// direction 1 은 영문 눕힘, 2 는 영문 세움이다. 빈칸은 늘 눕힌 것처럼 가로 폭이다.
bool verticalUpright(char32_t cp, int direction);
// 세로쓰기에서 바꿔 그리는 글자. 마침표·쉼표는 。、, 둥근 따옴표는 세로 낫표다. 바꾸지 않으면 그대로다.
char32_t verticalForm(char32_t cp);

// 세로쓰기 글자 칸(줄 방향 폭). 한글은 세로쓰기 폭을 모두 4의 배수로 내린다(1 HWPUNIT 경계로 잰 값,
// tools/hwp_vertical_hu_probe.py). 세운 글자는 1em(글자 크기 × 상대 크기, 위·아래 첨자는 16/25)이고 장평을 쓰지 않는다.
// 눕힌 글자와 빈칸은 가로 폭(natural)이다. 자간은 그 폭으로 다시 잰다. spacing 은 자간 퍼센트, 0이면 자간이 없다.
struct VerticalCell {
    std::int32_t ink = 0;
    std::int32_t trail = 0;
    bool upright = false;
};
VerticalCell verticalCell(char32_t cp, int direction, std::int32_t natural, int size, int relSize, bool shrunk,
                          std::int32_t spacing);

// 앞 원자를 보고 이 글자 앞에서 줄을 끊을 수 있는지 정한다.
class BreakClassifier {
public:
    explicit BreakClassifier(const model::ParaShape* paraShape);
    // 글자 원자. atoms 는 아직 이 원자를 넣기 전이다.
    void classify(Atom& atom, const std::vector<Atom>& atoms);
    // 개체·번호처럼 글자가 아닌 원자는 영문 단어를 끊는다.
    void endWord() { latinRun_ = false; }

private:
    bool latinChar_ = false;
    bool hyphen_ = false;
    // 한글 단위 어절. 띄어쓰기 없이 이어진 글자는 한 어절로 묶어 그 앞에서만 끊는다.
    bool nonLatinWord_ = false;
    bool latinRun_ = false;
};

// 줄 글자 표에 같은 꼴 글자를 모아 한 구간으로 바로 짓는다. 글자는 부르는 쪽이 add 뒤에 줄 글자 끝에 붙인다.
class RunBuilder {
public:
    RunBuilder(Node& line, const std::string& text, bool keepAdvances) : line_(line), text_(text), keepAdvances_(keepAdvances) {}
    bool empty() const { return !open_; }
    void add(const Atom& atom);
    void flush();

private:
    Node& line_;
    const std::string& text_;
    bool keepAdvances_ = true;
    bool open_ = false;
    std::size_t begin_ = 0;
    std::vector<std::int32_t> widths_;
    std::vector<std::int32_t> inks_;
    int size_ = 0;
    std::uint32_t shape_ = 0;
    std::string font_;
    std::int32_t ratio_ = 100;
    std::int32_t rel_ = 100;
    std::int32_t offset_ = 0;
    std::int32_t spacing_ = 0;
    std::string markpen_;
    bool synthetic_ = false;
};

// 탭·빈칸처럼 글자 하나로 된 구간을 줄 글자 표에 붙인다. 글자는 [textBegin, textEnd) 에 이미 있다.
void appendGlyphRun(Node& line, const Atom& atom, std::size_t textBegin, std::size_t textEnd);

struct TabMove {
    std::int32_t width = 0;
    std::string leader;
};

// 문단의 탭 정의. 위치는 본문 왼쪽 끝 기준이다.
class TabRuler {
public:
    TabRuler(const model::Document& doc, const model::ParaShape* paraShape, std::int32_t marginLeft,
             std::int32_t contentWidth, std::int32_t bodyRight, std::int32_t defaultTab);

    // at 은 문단 왼쪽 끝 기준 펜이다. 오른쪽·가운데 탭이면 tail() 로 탭 뒤 글 폭을 잰다.
    template <typename Tail>
    TabMove advance(std::int32_t at, Tail&& tail) const {
        TabMove move;
        const std::int32_t fromEdge = at + marginLeft_;
        int align = 0;
        bool found = false;
        std::int32_t target = 0;
        for (const auto& place : stops_) {
            if (place.pos > fromEdge) {
                target = place.pos;
                move.leader = place.leader;
                align = place.align;
                found = true;
                break;
            }
        }
        if (!found) {
            if (stops_.empty()) target = ((fromEdge / step_) + 1) * step_;
            else {
                target = stops_.back().pos;
                while (target <= fromEdge) target += step_;
            }
        }
        if (target < 0) target = 0;
        if (target > bodyRight_) target = bodyRight_;
        move.width = std::max<std::int32_t>(0, target - fromEdge);
        if (found && align != 0) {
            const std::int32_t rest = tail();
            if (align == 1) move.width -= rest;
            else move.width -= rest / 2;
            if (move.width < 0) move.width = 0;
        }
        return move;
    }

    // 저장된 줄의 탭은 그릴 때 정한다. 그때 쓸 탭 정의를 줄에 남긴다.
    void deferTo(Node& line) const;

private:
    struct Stop {
        std::int32_t pos = 0;
        int align = 0;  // 0 왼쪽, 1 오른쪽, 2 가운데
        std::string leader;
    };
    std::vector<Stop> stops_;
    std::int32_t marginLeft_ = 0;
    std::int32_t bodyRight_ = 0;
    std::int32_t step_ = 4000;
};

// 줄 채우기가 부르는 쪽 정보.
class LineHost {
public:
    virtual ~LineHost() = default;
    // 폭을 아직 재지 않은 원자를 잰다.
    virtual void measure(Atom& atom) = 0;
    // 글자처럼 취급한 개체 노드. 가로 위치는 pen 이다.
    virtual Node inlineObject(const Atom& atom, std::int32_t pen) = 0;
};

template <typename Measure, typename Object>
class LineHostFor final : public LineHost {
public:
    LineHostFor(Measure measure, Object object) : measure_(std::move(measure)), object_(std::move(object)) {}
    void measure(Atom& atom) override { measure_(atom); }
    Node inlineObject(const Atom& atom, std::int32_t pen) override { return object_(atom, pen); }

private:
    Measure measure_;
    Object object_;
};

template <typename Measure, typename Object>
LineHostFor<Measure, Object> makeLineHost(Measure measure, Object object) {
    return LineHostFor<Measure, Object>(std::move(measure), std::move(object));
}

using Gap = std::pair<std::int32_t, std::int32_t>;

struct LineSpec {
    const model::Document* doc = nullptr;
    const model::ParaShape* paraShape = nullptr;
    const TabRuler* tabs = nullptr;
    std::vector<Gap> gaps;  // 글을 놓을 수 있는 가로 칸. 하나 이상이다
    // 저장된 줄(lineseg)대로 끊는다. 폭은 재지 않고 lineLimit 앞에서 줄을 끝낸다.
    bool storedBreaks = false;
    int lineLimit = std::numeric_limits<int>::max();
    bool deferText = false;     // 글자 Span 은 그 쪽을 그릴 때 채운다
    bool keepAdvances = true;   // Span 에 글자별 advance 를 남긴다
    int firstPos = -1;          // 줄 시작 글자 위치를 미리 안다면 그 값
    bool vertical = false;      // 세로쓰기 줄. 줄 머리에 못 오는 부호가 안 들어가면 앞 단어째 다음 줄로 간다
};

struct LineFill {
    Node line;
    std::size_t gapIndex = 0;
    std::int32_t lineX = 0;
    int start = -1;
    int end = 0;
    std::int32_t textHeight = 0;
    std::int32_t shapeHeight = 0;  // 개체까지 넣은 그 줄 글자 모양 중 가장 큰 크기. 줄 간격을 정한다
    bool endedByLineBreak = false;
    std::string text;
    // 글이 든 줄이 개체를 건너 다음 칸으로 가야 하면 여기서 줄을 닫는다. 한글은 칸마다 lineseg 를 따로 두고,
    // 같은 줄 높이(vertpos)에서 그 칸부터 다시 채운다. spec.gaps 안의 그 칸 번호다. -1 이면 건너지 않았다.
    int jumpGap = -1;
};

// cursor 부터 한 줄을 채운다. 끝나면 cursor 는 다음 줄 첫 원자다.
// fill.line 은 종류·문단·y·글꼴 같은 머리 값만 채워 넘긴다. 나머지 fill 값은 여기서 정한다.
// 개체 안 조판이 이 함수를 거쳐 다시 들어오므로 Node 를 값으로 주고받지 않는다. WASM 스택은 작다.
void fillLine(const LineSpec& spec, std::vector<Atom>& atoms, std::size_t& cursor, LineFill& fill, LineHost& host);

// 채운 줄에 x·글자 범위·글자 상자를 적는다. 개체 x 는 줄 기준으로 바꾼다. 글자 상자 높이를 돌려준다.
std::int32_t closeLine(LineFill& fill);

// 같은 글자에서 줄을 못 만들고 맴돌 때 그 글자 하나만 담은 줄. 위치·높이는 부르는 쪽이 정한다.
Node singleAtomLine(const model::Document& doc, const Atom& atom, Node object);

// 글꼴에 어울리는 줄 높이. 글자마다 글꼴의 위·아래 상자를 재고,
// 줄의 기준선과 글자 상자는 그 줄 글자 중 가장 큰 값이다. 글자가 없으면 기준선이 음수.
struct SpanFontBox {
    std::int32_t baseline = -1;
    std::int32_t height = 0;
};
// 줄 글자 표로 잰다. 머리 번호는 본문 글자 상자를 키우지 않아 넣지 않는다.
SpanFontBox lineFontBox(const Node& line);
// 켜져 있으면 줄 기준선을 그 값으로 두고, 글자 상자는 글자 크기와 그 상자 중 큰 값으로 올린다.
// 비율 줄간격의 추가분은 이 글자 상자에 비율을 곱하므로 함께 바뀐다.
void applyFontBaseline(Node& line, const model::ParaShape* paraShape);

// 글자처럼 취한 개체와 캡션이 줄 밖으로 나간 만큼 줄 비트맵을 늘린다. 다음 줄 자리는 그대로다.
void markInlineOverflow(Node& line);

// 편집으로 글자 위치가 밀린 줄. 머리 표시는 그대로 둔다.
void shiftSpanOffsets(Node& line, int delta);
void shiftLineOffsets(Node& line, int delta);

// 각주·미주 번호를 줄 글자 표에 붙인다. 글자는 줄 글자 끝에 붙이고, 펜은 원자 폭만큼 민다.
void placeOccupyingNote(Node& line, const Atom& atom, std::int32_t& pen, std::string& lineText);

}  // namespace chwp::render::detail
