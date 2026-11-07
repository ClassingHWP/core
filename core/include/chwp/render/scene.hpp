#pragma once

// 렌더링은 파서가 만든 문서를 받아, 각 요소를 어디에 둘지 계산한다.
// 에디터(스튜디오)는 이 결과를 나중에 고친다. 여기에는 넣지 않는다.
//
// 한 프레임의 순서:
//   1. 종이, 그 안의 본문(쪽), 그 안의 단
//   2. 글자처럼 취급하지 않는 개체
//   3. 단마다 글자가 들어갈 가로 칸
//   4. 그 칸에 문단과 줄
// 글상자·표·칸처럼 안에 문단을 담는 개체는 같은 계산을 다시 탄다.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/model/document.hpp"

namespace chwp::render {

enum class NodeKind : std::uint8_t {
    Paper,
    Body,
    Column,
    Paragraph,
    Line,
    Object,
    Cell
};

// 줄 캐시의 탭 위치. 칸 너비는 쪽을 쌓을 때 정하지 않고, 그 줄을 그릴 때 정한다.
struct LineTab {
    std::int32_t pos = 0;
    int align = 0;  // 0 왼쪽, 1 오른쪽, 2 가운데
    std::string leader;
};

// 줄 글자 표의 이름(글꼴, 형광펜 색, 탭 채움). 같은 이름은 한 번만 두고 그 자리를 가리킨다.
// 가리키는 자리는 프로그램이 끝날 때까지 그대로다. 빈 이름은 nullptr 이다.
const std::string* nameRef(const std::string& name);

// 줄 글자 표의 한 구간. 글자 묶음 노드 대신 줄이 든다.
// Text 는 line.text 의 [textBegin, textEnd) 바이트가 같은 글자 모양·글꼴·크기·장평·상대 크기·자간·글자 위치·
// 형광펜으로 이어진 곳이다. 색·굵기·밑줄·배경 같은 그리기 모양은 문서의 글자 모양(charShapeId)에서 읽는다.
// Tab 은 탭 한 칸, Note 는 각주·미주 번호(한 칸), Object 는 줄의 글자처럼 넣은 개체(line.children[slot])다.
enum class RunKind : std::uint8_t { Text, Tab, Note, Object };

// 줄 구간의 글자 꼴(크기·글꼴·형광펜·탭 채움·장평·상대 크기·자간·글자 위치). 문서 전체에 같은 꼴이 몇 개 없어서
// 한 번만 두고 구간은 그 자리를 가리킨다(runStyleRef). 자리는 프로그램이 끝날 때까지 그대로다.
struct RunStyle {
    std::int32_t fontSize = 0;
    const std::string* font = nullptr;  // 비면 줄 글꼴
    const std::string* markpen = nullptr;
    const std::string* tabLeader = nullptr;
    std::int16_t ratio = 100;
    std::int16_t relSize = 100;
    std::int16_t spacing = 0;
    std::int16_t charOffset = 0;

    bool operator==(const RunStyle&) const = default;
};

const RunStyle* runStyleRef(const RunStyle& style);
const RunStyle* defaultRunStyle();
// 꼴 값으로 공용 꼴 자리를 얻는다. 빈 이름은 없음이다.
const RunStyle* runStyleOf(std::int32_t fontSize, const std::string& font, const std::string& markpen,
                           const std::string& tabLeader, std::int32_t ratio, std::int32_t relSize, std::int32_t spacing,
                           std::int32_t charOffset);

struct LineRun {
    RunKind kind = RunKind::Text;
    bool synthetic = false;  // 자동 번호 대신 넣은 글자. 그리기만 하고 커서 칸은 아니다
    std::uint16_t slot = 0;  // Object: line.children 번호
    // 이 구간 글자 폭과 잉크 폭이 LineGlyphs::advances / inks 의 어디부터 몇 칸인지. 0 칸이면 그리거나 칠 때 잰다.
    std::uint16_t advanceCount = 0;
    std::uint16_t inkCount = 0;
    // 줄 글자 안 자리와 폭 표 안 자리. 한 줄은 쪽 너비 안에 놓이므로 65535 바이트·칸을 넘지 않는다(가장 작은 1pt 글자도 수백 자).
    std::uint16_t textBegin = 0;
    std::uint16_t textEnd = 0;
    std::uint16_t advanceAt = 0;
    std::uint16_t inkAt = 0;
    std::int32_t width = 0;  // 글자 폭을 따로 들지 않는 구간의 폭(흐름 조판의 탭·빈칸 한 칸)
    std::uint32_t charShapeId = 0;
    const RunStyle* style = defaultRunStyle();

    bool operator==(const LineRun&) const = default;
};

// 문단 첫 줄 앞에 매다는 개요·번호·글머리 글자. 줄 글자(line.text)에도 커서 칸에도 들지 않는다.
// x 는 줄 왼쪽에서 잰 자리(내어 쓰면 음수)다. 글머리 그림은 이것 대신 줄 자식 개체(headingMark)로 둔다.
struct HeadingRun {
    std::string text;
    std::vector<std::int32_t> advances;  // 다음 글자까지
    std::int32_t x = 0;
    std::int32_t width = 0;
    std::int32_t fontSize = 0;
    std::uint32_t charShapeId = 0;
    std::string font;
    std::string color;
    std::int16_t ratio = 100;
    std::int16_t relSize = 100;
    bool bold = false;
    bool italic = false;
    bool superscript = false;
    bool subscript = false;
};

// 한 줄의 글자 표. 구간과 글자마다의 폭(다음 글자까지, 자간 포함)과 잉크 폭(자간을 뺀 폭)이다.
// 머리 번호 글자는 문단 첫 줄에만 있어 따로 든다. 바뀌지 않으므로 사본끼리 같이 쓴다.
struct LineGlyphs {
    std::vector<LineRun> runs;
    std::vector<std::int32_t> advances;
    std::vector<std::int32_t> inks;
    std::shared_ptr<const HeadingRun> heading;
};

// 줄 내용: 줄 하나의 배치. 찍을 글자와 그 글자 표, 줄 상자 치수, 탭, lineSeg(읽은 값과 잰 값)다.
// 줄 노드는 자리(x, y, 너비, 높이)와 이것만 든다. 노드 사본과 문단 배치 표가 같은 것을 나눠 들고,
// 고칠 때는 Node::editContent 로 혼자 든 것만 고친다. 줄이 아닌 노드는 들지 않는다(읽으면 기본값).
struct LineContent {
    std::string text;  // 개요·번호 머리는 넣지 않는다
    LineGlyphs glyphs;
    int autoSpacing = 0;          // 한글과 영어(1)·숫자(2) 자동 간격. 그릴 때 재는 글자 폭에 더한다
    std::int32_t textHeight = 0;  // 글자를 놓는 상자. 그 줄의 가장 큰 글자 크기. 줄 간격은 넣지 않는다
    std::int32_t spacingBox = 0;  // 줄 간격 비율을 곱하는 크기. 글자처럼 넣은 개체의 글자 모양도 든다. 0이면 textHeight
    std::int32_t lineBaseline = -1;  // 줄 기준선. 글자, 그림 85%, 수식 높이×백분율 중 큰 값
    bool matchFontBaseline = false;  // 글꼴에 어울리는 줄 높이. 글자 자신의 기준선도 어센더다
    bool lastLine = false;   // 문단의 마지막 줄. 양쪽 정렬은 이 줄을 늘리지 않는다
    bool lineBreak = false;  // Shift+Enter로 끝난 줄. 양쪽 정렬로 빈칸을 늘리지 않는다
    // 줄 캐시는 글자 advance 를 비워 둔다. 글자 폭, 탭 칸, 개체 가로 위치는 그릴 때 정한다.
    bool deferTabs = false;
    bool wongoji = false;        // 원고지 정서법
    std::uint8_t vertical = 0;   // 세로쓰기 글자 방향. 0 이면 가로. 세우는 글자는 1em 칸에 돌려 그린다
    std::int32_t charGrid = 0;   // 글자 격자 간격. 0 이면 끔
    // lineseg 로 놓은 줄. 줄높이 측정은 글자 상자로 줄을 쌓은 높이다.
    bool auditLine = false;
    std::int32_t auditRise = 0;   // 줄 위로 나가 그리는 높이. 줄 피치에는 들지 않는다
    std::int32_t auditBelow = 0;  // 줄 피치 아래로 나가 그리는 높이. 줄 피치에는 들지 않는다
    std::int32_t tabMargin = 0;
    std::int32_t tabBodyRight = 0;
    std::int32_t tabStep = 0;
    std::vector<LineTab> tabStops;
    // 줄 자체 프레임 HU 상자(글자처럼 취급한 자식 개체는 뺀 순수 줄 프레임 영역)
    std::int32_t lineBoxX = 0;
    std::int32_t lineBoxY = 0;
    std::int32_t lineBoxW = 0;
    std::int32_t lineBoxH = 0;
    // lineSeg: 파일에서 읽은 값(storedSeg)과 우리 조판으로 잰 값(measuredSeg).
    std::int32_t auditIndex = -1;  // 문단 lineseg 안에서의 줄 번호. 0 이 첫 줄
    std::int32_t auditText = 0;
    std::int32_t auditPitch = 0;
    std::int32_t auditHorz = 0;
    model::LineSeg storedSeg;
    model::LineSeg measuredSeg;  // spacing 은 글자 상자 아래 추가분. 줄 피치는 textheight + spacing 이다
};

// 노드에 붙는 짧은 이름 값(역할·글꼴·색·정렬 따위). 같은 값은 프로그램에 한 번만 두고 노드는 그 자리를 가리킨다.
// 문자열처럼 쓰지만 8바이트다. 자리는 프로그램이 끝날 때까지 그대로라 잠그지 않고 읽는다.
class Name {
public:
    Name() = default;
    Name(std::string_view value) : value_(intern(value)) {}
    Name(const std::string& value) : value_(intern(value)) {}
    Name(const char* value) : value_(intern(value ? std::string_view(value) : std::string_view{})) {}
    Name& operator=(std::string_view value) {
        value_ = intern(value);
        return *this;
    }
    Name& operator=(const std::string& value) { return *this = std::string_view(value); }
    Name& operator=(const char* value) { return *this = (value ? std::string_view(value) : std::string_view{}); }

    [[nodiscard]] const std::string& str() const { return *value_; }
    operator const std::string&() const { return *value_; }
    [[nodiscard]] bool empty() const { return value_->empty(); }
    [[nodiscard]] std::size_t size() const { return value_->size(); }
    [[nodiscard]] const char* c_str() const { return value_->c_str(); }
    [[nodiscard]] const char* data() const { return value_->data(); }
    [[nodiscard]] std::size_t find(std::string_view what, std::size_t at = 0) const { return value_->find(what, at); }
    [[nodiscard]] std::size_t find(char what, std::size_t at = 0) const { return value_->find(what, at); }
    [[nodiscard]] std::string substr(std::size_t at, std::size_t count = std::string::npos) const {
        return value_->substr(at, count);
    }
    [[nodiscard]] char front() const { return value_->front(); }
    [[nodiscard]] char operator[](std::size_t i) const { return (*value_)[i]; }
    [[nodiscard]] std::string::const_iterator begin() const { return value_->begin(); }
    [[nodiscard]] std::string::const_iterator end() const { return value_->end(); }
    void clear() { value_ = empty_(); }

    friend bool operator==(const Name& a, const Name& b) { return a.value_ == b.value_; }
    friend bool operator==(const Name& a, std::string_view b) { return *a.value_ == b; }
    friend bool operator==(const Name& a, const std::string& b) { return *a.value_ == b; }
    friend bool operator==(const Name& a, const char* b) { return *a.value_ == b; }
    friend std::string operator+(const Name& a, std::string_view b) { return *a.value_ + std::string(b); }
    friend std::string operator+(std::string_view a, const Name& b) { return std::string(a) + *b.value_; }
    friend std::string operator+(const Name& a, const char* b) { return *a.value_ + b; }
    friend std::string operator+(const char* a, const Name& b) { return a + *b.value_; }
    friend std::string operator+(const Name& a, const std::string& b) { return *a.value_ + b; }
    friend std::string operator+(const std::string& a, const Name& b) { return a + *b.value_; }

private:
    static const std::string* empty_();
    static const std::string* intern(std::string_view value);
    const std::string* value_ = empty_();
};

template <typename Stream>
auto operator<<(Stream& out, const Name& name) -> decltype(out << name.str()) {
    return out << name.str();
}

// 좌표는 부모의 왼쪽 위가 원점이다. 단위는 HWPUNIT.
// 개요·번호·글머리표가 차지하는 칸. 문단 노드에만 채운다.
// 테두리 한 변. type 이 비어 있으면 그 변은 그리지 않는다.
struct BorderEdge {
    Name type;
    Name color;
    std::int32_t width = 0;
    int end = 1;  // 0 이면 둥근 끝. 도형 테두리에서만 쓴다
};

struct HeadingPlace {
    bool ready = false;
    bool autoIndent = true;
    bool useInstWidth = true;
    bool halfSlot = false;  // 영문 글머리표. 칸이 글자 크기의 절반이다
    std::int32_t widthAdjust = 0;
    std::int32_t textOffset = 50;
    std::string offsetType = "PERCENT";
    std::string align = "LEFT";
    std::uint32_t charShapeId = 0xFFFFFFFFu;
    std::string imageId;
    int bright = 0;
    int contrast = 0;
    int effect = 0;
};

// 페이지(NodeKind::Paper) 바깥 기본 여백(HWPUNIT).
// 기본 위·아래 마진은 900 HU, 첫 페이지는 top에 절반(450 HU) 더 주고, 마지막 페이지는 bottom에 절반(450 HU) 더 준다.
inline constexpr std::int32_t kDefaultPageMarginTopHu = 900;
inline constexpr std::int32_t kDefaultPageMarginBottomHu = 900;
inline constexpr std::int32_t kDefaultPageMarginEdgeExtraHu = 450;
inline constexpr std::int32_t kDefaultPageMarginLeftHu = 1800;
inline constexpr std::int32_t kDefaultPageMarginRightHu = 1800;

inline constexpr std::int32_t defaultPageMarginTopHu(std::size_t pageIndex) {
    return kDefaultPageMarginTopHu + (pageIndex == 0 ? kDefaultPageMarginEdgeExtraHu : 0);
}

inline constexpr std::int32_t defaultPageMarginBottomHu(std::size_t pageIndex, std::size_t pageCount) {
    return kDefaultPageMarginBottomHu +
           (pageCount > 0 && pageIndex + 1 == pageCount ? kDefaultPageMarginEdgeExtraHu : 0);
}

// 개체·칸·쪽·머리 번호·테두리 값. 글자 묶음과 줄은 쓰지 않아서, 필요한 노드에만 만든다.
// 노드는 읽을 때 ext(), 쓸 때 extMut() 로 닿는다. 없는 노드의 ext() 는 기본값을 돌려준다.
// 노드를 복사하면 같이 복사하는 꼬리 칸. 없으면 비어 있고, 읽으면 기본값이다.
template <typename T>
class TailBox {
public:
    TailBox() = default;
    TailBox(const TailBox& other) : value_(other.value_ ? std::make_unique<T>(*other.value_) : nullptr) {}
    TailBox& operator=(const TailBox& other) {
        if (this != &other) value_ = other.value_ ? std::make_unique<T>(*other.value_) : nullptr;
        return *this;
    }
    TailBox(TailBox&&) noexcept = default;
    TailBox& operator=(TailBox&&) noexcept = default;

    [[nodiscard]] const T* get() const { return value_.get(); }
    [[nodiscard]] const T& value() const {
        static const T none;
        return value_ ? *value_ : none;
    }
    T& make() {
        if (!value_) value_ = std::make_unique<T>();
        return *value_;
    }

private:
    std::unique_ptr<T> value_;
};

// 개체(그림·도형·수식·글상자·표)와 칸에만 쓰는 꼬리 값. 문단·종이 꼬리에는 없다.
struct ObjectExtra {
    // 띄운 개체가 붙은 문단(글 묶음 번호, 문단 번호). 개체를 끌어 옮길 때 문서의 그 개체를 찾는다.
    int anchorStory = -1;
    int anchorParagraph = -1;
    int anchorItem = -1;  // 그 문단 runs 의 항목을 차례로 센 번호. 문서의 그 컨트롤이다
    std::string script;     // 수식 스크립트. 그리기는 layoutHangulEquation 이 놓은 자리를 상자 크기에 맞춘다
    Name binItemId;  // 그림 파일 참조
    model::PictureView picture;
    std::int32_t zOrder = 0;
    Name textWrap;
    int cornerRatio = 0;           // 사각형 모서리 곡률 %. 0 은 직각, 50 은 짧은 변의 반원
    int imageFillMode = -1;        // 칸 그림 채우기. -1 이면 없다. binItemId 가 그림이다
    bool splitTable = false;
    bool splitTail = false;  // 앞 쪽에서 이어진 표 조각. 이 쪽은 문단 첫머리부터 시작하지 않는다
    std::vector<std::int32_t> spanRowSplits;
    std::vector<std::int32_t> spanColSplits;
    std::vector<model::ShapePoint> points;  // 다각형. 이 개체 상자 안의 좌표
    int baseLine = -1;             // 수식. 상자 높이의 이 퍼센트가 글자 기준선에 맞는다. 조판은 파일 값
    std::int32_t eqWidth = 0;      // measureHangulEquation 너비. 조판 상자는 width 그대로다
    std::int32_t eqHeight = 0;
    int eqBase = -1;               // 측정 기준선 퍼센트. 재지 않았으면 -1
    std::int32_t eqBaseUnit = 1000;  // 수식 글자 크기(1000 = 10pt). 그릴 때 다시 놓는다
    std::string eqFont;              // 수식 글꼴. 비어 있으면 HYhwpEQ
    int fileEqBase = -1;           // 파일 기준선. 요소 비교용이고 조판에는 안 쓴다
    std::int32_t fileWidth = 0;
    std::int32_t fileHeight = 0;
    std::int32_t fileX = 0;       // 가로 오프셋. 묶음 자식은 묶음 안 좌표
    std::int32_t fileY = 0;
    std::int32_t fileGroupX = 0;
    std::int32_t fileGroupY = 0;
    std::int32_t fileMatrixX = 0;  // 행렬 이동. 묶음 좌표에 이미 포함된다.
    std::int32_t fileMatrixY = 0;
    double fileScaleX = 1;
    double fileScaleY = 1;
    std::int32_t filePadLeft = 0;  // 글상자 안여백 또는 칸에 적힌 셀 안여백
    std::int32_t filePadRight = 0;
    std::int32_t filePadTop = 0;
    std::int32_t filePadBottom = 0;
    std::uint32_t fileRows = 0;
    std::uint32_t fileCols = 0;
    int fileOwnMargin = -1;  // 칸이 자체 안여백을 쓰는지. 칸이 아니면 -1
    Name fileHorzRel;
    Name fileVertRel;
    Name fileHorzAlign;
    Name shapeType;
    // 쪽/단 위 부유 개체의 페이지 내 일련번호 (트리 가지치기 시에도 z 순서가 고정되도록 유지)
    std::int32_t objectPageSerial = -1;
};

struct NodeExtra {
    Name headingType;    // OUTLINE, NUMBER, BULLET. 없으면 빈 문자열
    std::string headingFormat;  // ^1. 같은 형식 문자열
    std::string headingText;    // 그 문단에 실제로 붙는 머리. 예: 1.  가.
    HeadingPlace head;
    Name fillColor;
    Name hatchStyle;  // 테두리/배경 무늬. 비어 있으면 없다
    Name hatchColor;
    Name gradColor;         // 채우기 끝 색. 있으면 fillColor에서 이 색으로 번진다
    std::int32_t gradAngle = 0;    // 0 은 아래에서 위, 90 은 왼쪽에서 오른쪽
    BorderEdge borderLeft, borderRight, borderTop, borderBottom;
    model::DiagonalSpec slash;
    model::DiagonalSpec backSlash;
    BorderEdge borderDiagonal;
    Name centerLine = "NONE";
    bool breakCellSeparateLine = false;
    int row = -1;
    int col = -1;
    int rowSpan = 1;
    int colSpan = 1;
    std::int32_t borderOffsetLeft = 0;
    std::int32_t borderOffsetRight = 0;
    std::int32_t borderOffsetTop = 0;
    std::int32_t borderOffsetBottom = 0;
    bool borderConnect = false;
    bool borderIgnoreMargin = false;
    std::int32_t cellSpacing = 0;
    std::string pageLabel;          // 종이 노드. 쪽 번호 형식대로 쓴 번호("79", "iv"). 비면 pageNumber 그대로
    TailBox<ObjectExtra> object;

    // 개체·칸 값. 읽기는 obj(), 쓰기는 objMut().
    [[nodiscard]] const ObjectExtra& obj() const { return object.value(); }
    ObjectExtra& objMut() { return object.make(); }
};

const NodeExtra& noNodeExtra();

// 노드를 복사하면 꼬리도 따로 복사한다.
class NodeExtraBox {
public:
    NodeExtraBox() = default;
    NodeExtraBox(const NodeExtraBox& other) : value_(other.value_ ? std::make_unique<NodeExtra>(*other.value_) : nullptr) {}
    NodeExtraBox& operator=(const NodeExtraBox& other) {
        if (this != &other) value_ = other.value_ ? std::make_unique<NodeExtra>(*other.value_) : nullptr;
        return *this;
    }
    NodeExtraBox(NodeExtraBox&&) noexcept = default;
    NodeExtraBox& operator=(NodeExtraBox&&) noexcept = default;

    [[nodiscard]] const NodeExtra* get() const { return value_.get(); }
    NodeExtra& make() {
        if (!value_) value_ = std::make_unique<NodeExtra>();
        return *value_;
    }

private:
    std::unique_ptr<NodeExtra> value_;
};

struct Node {
    // 칸 순서는 크기대로다(4바이트 값, 이름, 8바이트 값, 배열·참조, 1바이트 값). 끼워 넣는 빈칸이 생기지 않게 한다.
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    int paragraph = -1;     // 이 프레임 문단 목록에서의 번호
    std::uint32_t paragraphId = 0;  // 문단 노드: 문서 문단의 고유 번호(model::Paragraph::id)
    int story = -1;         // 블록이 머무는 글 묶음. 섹션 본문과 개체 안 문단은 서로 다르다
    int start = -1;         // 줄이 차지하는 글자 시작. end 는 마지막 글자 다음
    int end = -1;
    int section = -1;
    int headingLevel = -1;      // 문단 모양의 단계. 0 이 첫 단계
    std::int32_t fontSize = 0;  // 글자 크기. 1000 = 10pt
    std::uint32_t charShapeId = 0;
    std::int32_t turnSpan = 0;  // 돌린 틀의 가로(종이 쪽) 폭. 0 이면 종이 폭이다
    std::int32_t marginLeft = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginBottom = 0;
    std::int32_t indent = 0;       // 첫 줄. 양수면 들여쓰기, 음수면 내어쓰기
    std::int32_t spacingPrev = 0;  // 문단 위 간격
    std::int32_t spacingNext = 0;  // 문단 아래 간격
    int pageNumber = 0;  // 종이 노드의 쪽 번호. 1부터. 홀짝 테두리를 고를 때 쓴다
    // 페이지 왼쪽 위(0,0) 기준 절대 HU 경계 상자 (자식·여백·수식 돌출·테두리 블리드 모두 포함)
    std::int32_t huX = 0;
    std::int32_t huY = 0;
    std::int32_t huW = 0;
    std::int32_t huH = 0;
    Name role;       // picture, shape, table, container, equation
    Name font;           // 글꼴 이름
    Name vertAlign;  // 줄: 문단 세로 정렬 BASELINE, TOP, CENTER, BOTTOM. 칸·개체는 각자 맞춤
    Name align;             // LEFT, CENTER, RIGHT, JUSTIFY, DISTRIBUTE, DISTRIBUTE_SPACE
    // 위치(x, y, page, path, paragraph, start)에 독립적인 노드 모양/내용 해시
    std::uint64_t contentHash = 0;
    std::vector<Node> children;
    // 줄: 글자와 본문 글자 표(LineContent). 자식에는 머리 그림과 글자처럼 넣은 개체만 남는다. 줄이 아니면 비어 있다.
    std::shared_ptr<LineContent> content;
    // 드물게 쓰는 값(개체·칸·쪽·머리 번호·테두리). 읽기는 ext(), 쓰기는 extMut().
    NodeExtraBox extra;
    NodeKind kind = NodeKind::Paper;
    // 세로쓰기 본문·각주·글상자·표 칸의 글자 방향(1 영문 눕힘, 2 영문 세움). 0 이 아니면 이 노드와 그 아래 좌표는
    // 돌린 틀에 있다. 틀 원점은 부모 자리이고, 틀의 (x, y) 는 부모 자리에서 (turnSpan − y, x) 다.
    std::uint8_t turned = 0;
    bool treatAsChar = false;
    bool headingMark = false;  // 첫 줄에 붙인 머리. 본문 펜은 밀지 않는다
    bool fromMaster = false;    // 바탕쪽. 화면에서는 본문 뒤에 깐다
    bool masterOnTop = false;   // 인쇄 시 맨 위. 이 쪽을 인쇄할 때 본문보다 위에 그린다
    bool pageBand = false;      // 머리말·꼬리말·쪽 번호. 본문 그림보다 아래에 깐다
    // 세로쓰기 틀 안의 개체 자리. 너비·높이·여백은 돌린 틀 기준(가로·세로가 바뀜)이고, 자식(실제 개체)은
    // 이 상자의 종이 자리에서 세운 그대로 그린다. 한글은 세로쓰기에서도 표·그림·글상자를 돌리지 않는다.
    bool upright = false;
    bool flipHorizontal = false;   // 그림을 그릴 때 좌우를 뒤집는다. 도형 점은 파서에서 이미 뒤집는다
    bool flipVertical = false;
    // 쪽 계산은 줄 위치만 남긴다. 글자열과 글꼴 이름은 그 쪽을 그리기 전에 채운다.
    bool spansLater = false;
    bool pageNumberHidden = false;  // 종이 노드. 「쪽 번호 감추기」로 이 쪽에 번호가 찍히지 않는다
    // 파일에 적힌 개체 크기. 조판이 칸을 키운 뒤의 width/height 와 비교한다.
    bool fileAudit = false;
    bool huBoundsReady = false;
    [[nodiscard]] const NodeExtra& ext() const { return extra.get() ? *extra.get() : noNodeExtra(); }
    NodeExtra& extMut() { return extra.make(); }

    // 줄 내용. 없으면 기본값이다.
    [[nodiscard]] const LineContent& lineData() const;
    [[nodiscard]] const std::string& text() const { return lineData().text; }
    [[nodiscard]] const LineGlyphs& glyphs() const { return lineData().glyphs; }
    // 고칠 줄 내용. 없으면 만들고, 다른 노드(사본)나 문단 배치 표와 나눠 들고 있으면 떼어 내 혼자 든다.
    LineContent& editContent();
    std::string& textMut() { return editContent().text; }
    LineGlyphs& glyphsMut() { return editContent().glyphs; }
};

// 줄 글자 표. 줄 글자를 읽는 곳은 모두 이것을 쓴다.
inline const LineGlyphs& lineGlyphs(const Node& line) { return line.glyphs(); }

// 줄 글자 표를 차례로 짓는다. 구간의 글자는 이미 줄 글자 끝에 붙어 있다. 폭·잉크 칸 자리는 여기서 정한다.
void appendLineRun(LineGlyphs& glyphs, LineRun run, const std::int32_t* advances = nullptr,
                   std::size_t advanceCount = 0, const std::int32_t* inks = nullptr, std::size_t inkCount = 0);
// 개체를 줄 자식 끝에 넣고 그 자리를 가리키는 구간을 붙인다. textAt 은 지금까지 지은 줄 글자 길이다.
void appendLineObject(Node& line, Node object, std::size_t textAt);
// 다 지은 줄 글자 표의 남는 자리를 돌려준다.
void trimLineGlyphs(LineGlyphs& glyphs);
// 줄이 개체로 시작하는지(머리 번호가 있으면 그 머리가 개체인지).
bool lineStartsWithObject(const Node& line);

// 쪽 조판 결과가 쓰는 바이트(어림값)와 노드 수. 메모리 보고용.
std::size_t sceneMemory(const std::vector<Node>& pages, std::size_t& nodes);
// 마지막 sceneMemory 가 센 종류별 노드 수. 8 은 아직 조각을 안 채운 쪽 수, 9 는 꼬리(ext)가 있는 노드 수.
std::size_t sceneKindCount(int kind);
// 마지막 sceneMemory 가 센 쪽 계산 바이트 내역. 줄 구간 수(kScenePartRunCount)만 개수다.
enum ScenePart {
    kScenePartHeads,       // 노드 머리
    kScenePartTails,       // 노드 꼬리(NodeExtra)
    kScenePartLines,       // 줄 내용(LineContent) 몸체
    kScenePartRuns,        // 줄 글자 구간
    kScenePartWidths,      // 글자 폭·잉크 폭
    kScenePartChildSlack,  // 자식 배열의 빈자리
    kScenePartOther,       // 줄 글자, 탭, 도형 점
    kScenePartRunCount,
    kScenePartCount
};
std::size_t scenePart(ScenePart part);

inline void applyDefaultPageMargins(std::vector<Node>& pages) {
    const std::size_t count = pages.size();
    for (std::size_t i = 0; i < count; ++i) {
        pages[i].marginTop = defaultPageMarginTopHu(i);
        pages[i].marginBottom = defaultPageMarginBottomHu(i, count);
        pages[i].marginLeft = kDefaultPageMarginLeftHu;
        pages[i].marginRight = kDefaultPageMarginRightHu;
    }
}

// 쪽 내 모든 노드의 페이지 절대 HU 경계 상자(huX, huY, huW, huH)와 contentHash를 갱신한다.
void updatePageHuBounds(Node& page, const model::Document* doc = nullptr);
void ensurePageHuBounds(const Node& page, const model::Document* doc = nullptr);

// 쪽마다 종이 노드 하나. 종이는 본문을, 본문은 단을 담는다.
// onlyPage 가 0 이상이면 그 쪽까지만 계산하고 그 쪽 노드만 돌려준다.
// 그 경로는 미리보기에서 부르지 않는다. 한글 조판과 맞추려면 문서 전체를 배치해야 한다.
// useStoredLineHeight 가 켜지면 문단 lineseg 의 글자 위치에서 줄을 끊고,
// 글자 높이에 줄간격을 더한 값으로 세로를 쌓는다.
// 글자 폭과 탭 칸은 그때 재지 않는다. 그 쪽을 그릴 때 잰다. 단, 개체, 글상자는 항상 이 조판이 놓는다.
// 미리보기에서 페이지를 계산하는 동안 쪽마다 걸린 시간을 콘솔에 남긴다.
void setPageCalcLog(bool on);
// 켜면 lineseg 의 줄 경계는 그대로 쓰고, 줄 높이·줄 간격은 우리 측정값으로 쌓는다. 한글과 모양을 견줄 때 쓴다.
void setMeasuredLineHeight(bool on);

std::vector<Node> layoutPages(const model::Document& doc, int onlyPage, bool useStoredLineHeight = false,
                             bool deferSpans = false);

// 그림 크기가 바뀐 쪽부터 다시 놓는다. 뒤쪽 본문이 예전 쪽의 문단 처음과 같으면
// 그 종이 노드를 옮기고, 쪽 번호만 고친다. 문단 중간에서 쪽이 시작하거나 단이 여러 개면 false.
bool reflowResizedPages(const model::Document& doc, std::vector<Node>& pages, int startPage,
                        bool useStoredLineHeight, bool deferSpans, const std::string& bin, int newWidth,
                        int newHeight);
inline std::vector<Node> layoutPages(const model::Document& doc) { return layoutPages(doc, -1, false); }

// 구역의 쪽 테두리 세 칸을 바꾸고, 첫 문단의 구역 정의에도 같은 값을 넣는다.
// pages 가 있으면 그 구역의 종이에 채우기와 테두리를 다시 붙인다.
void assignPageBorders(model::Document& doc, int sectionIndex, std::array<model::PageBorderFill, 3> borders,
                       std::vector<Node>* pages = nullptr);

// 편집 후 부분 무효화할 타일/줄 캐시 범위.
struct FrameInvalidation {
    int page = -1;               // -1 이면 모든 쪽
    std::vector<int> pathPrefix; // 비어 있으면 해당 쪽의 모든 노드
    int minChild = -1;           // >= 0 이면 pathPrefix 바로 아래 자식의 최소 인덱스
    int maxChild = -1;           // >= 0 이면 pathPrefix 바로 아래 자식의 최대 인덱스 (-1 이면 끝까지)
    bool selfOnly = false;       // true 이면 pathPrefix 노드 자체 타일(표 테두리/셀 배경 등)만 지우고 자손은 유지
};

// 편집 연산의 범위와 상향식(Bottom-Up) 증분 재조판 입력 명세.
struct EditRelayoutSpec {
    int story = -1;
    int paraIndex = -1;
    int paraDelta = 0;       // 0: 같은 문단 내 편집, +1: 문단 분리(Enter), <0: 문단 병합/다중문단 삭제
    int editStartOff = 0;    // paraIndex 문단 내 편집 시작 오프셋
    int editOldEndOff = 0;   // 편집 전 paraIndex 문단 내 편집 끝 오프셋
    int editNewEndOff = 0;   // 편집 후 paraIndex 문단 내 편집 끝 오프셋
    bool useStoredLineHeight = false;
    int hintPage = -1;       // >= 0 이면 해당 페이지 근처를 먼저 탐색
    // 문단 모양(여백·정렬·간격)을 바꿨다. 줄 하나만 고치지 않고, 줄 글자가 같아도 문단 전체를 다시 그린다.
    bool paraShapeChanged = false;
    // 증분 조판이 뒤 문단 번호를 이미 paraDelta 만큼 밀었다. 아니면 구역 다시 놓기가 옛 쪽 번호를 새 번호로 바꿔 본다.
    bool paragraphsShifted = false;
};

struct EditRelayoutResult {
    bool incremental = false;
    bool fullInvalidation = false;
    bool pageListChanged = false;  // 쪽 수·쪽 크기가 바뀌면 카메라 쪽 목록만 고친다. 캐시는 invalidations 만 비운다
    const char* widenReason = nullptr;  // 증분을 포기한 이유. 로그용
    bool lineBoxSame = false;  // 줄 상자는 그대로다. 글자만 바뀌었으면 쪽 경계 상자를 다시 계산하지 않는다
    bool pagesMoved = false;   // 쪽 벡터를 다시 만들었다. 칸 목록이 쥐고 있던 쪽 노드는 무효다
    int pageShiftFrom = -1;    // 이 옛 쪽부터 번호가 pageShiftDelta 만큼 밀린다
    int pageShiftDelta = 0;
    // 뒤 문단 번호를 paraDelta 만큼 밀어 둔 채 끝났다(증분을 포기했어도 그대로다).
    bool paragraphsShifted = false;
    std::vector<FrameInvalidation> invalidations;
};

// 머리말·꼬리말 하나를 고친 뒤 그 띠만 쪽마다 다시 놓는다. 띠는 본문을 밀지 않아 본문은 그대로다.
// 다시 놓은 띠는 invalidations 에 담는다. 띠 구조가 바뀌어 글 묶음 번호를 이을 수 없으면 false 이고 아무것도 바꾸지 않은 쪽이 남을 수 있다.
bool relayoutPageBands(const model::Document& doc, std::vector<Node>& pages, const model::Control& control,
                       bool useStoredLineHeight, std::vector<FrameInvalidation>& invalidations);


// 고친 쪽부터 그 구역을 다시 놓는다. 쪽 나눔에서 옛 쪽 시작과 맞으면 뒤를 붙인다.
// 앞 구역은 그대로 둔다. 뒤 구역은 쪽 수가 바뀌면 쪽 번호만 고친다. pageStart 가 고정인 구역부터는 번호를 유지한다.
// 쪽 처음이 문단 중간이면 같은 구역에서 문단 처음으로 시작한 쪽까지 거슬러 올라간다.
struct SectionReflow {
    bool ok = false;
    bool pageListChanged = false;
    int dirtyBegin = -1;
    int dirtyEnd = -1;  // 이 쪽 미만을 캐시에서 비운다
    int sectionIndex = -1;
    int fromParagraph = -1;
    int laidPages = 0;
    int reusePage = -1;  // 이 옛 쪽부터 종이를 유지. 없으면 -1
    int pageShiftFrom = -1;
    int pageShiftDelta = 0;
    const char* skipReason = nullptr;
};

// 문단을 넣거나 지운 편집이면 shiftStory 구역 글의 shiftAfter 번 뒤 옛 문단 번호가 shiftDelta 만큼 밀린다.
// 옛 쪽과 맞춰 볼 때와 그대로 붙이는 옛 쪽에 새 번호를 쓴다. 지운 문단(shiftDelta < 0)으로 시작한 옛 쪽은 맞추지 않는다.
SectionReflow reflowPagesFrom(const model::Document& doc, std::vector<Node>& pages, int startPage,
                              bool useStoredLineHeight, bool deferSpans, int shiftStory = -1, int shiftAfter = -1,
                              int shiftDelta = 0);

// 구역 본문 편집이 단·쪽을 넘기면 그 쪽부터 다시 놓는다. 표 칸·글상자·각주·미주는 여기서 다루지 않는다.
SectionReflow reflowEditedPages(const model::Document& doc, std::vector<Node>& pages, const EditRelayoutSpec& spec);

EditRelayoutResult relayoutAfterEditIncremental(const model::Document& doc, std::vector<Node>& pages,
                                                const EditRelayoutSpec& spec);

// 마지막 조판(layoutPages)에서 구축된 story 매핑 테이블을 돌려준다.
// 문서가 바뀌었거나 아직 구축되지 않았으면 doc 기준으로 즉시 구축한다.
const model::StoryMap& ensureStoryMap(const model::Document& doc);
model::StoryMap& ensureStoryMap(model::Document& doc);
void rebuildStoryMap(const model::Document& doc);

// story 번호와 커서 좌표를 Document 안의 실제 std::vector<Paragraph>* / Paragraph* 로 O(1) 역추적한다.
const model::StorySlot* resolveStory(const model::Document& doc, int story);
model::StorySlot* resolveStory(model::Document& doc, int story);
const std::vector<model::Paragraph>* storyParagraphs(const model::Document& doc, int story);
std::vector<model::Paragraph>* storyParagraphs(model::Document& doc, int story);
const model::Paragraph* storyParagraph(const model::Document& doc, int story, int paragraphIndex);
model::Paragraph* storyParagraph(model::Document& doc, int story, int paragraphIndex);
model::CursorTarget resolveCursor(model::Document& doc, model::CursorPos pos);

// 쪽 글자 노드를 채우는 동안 그 쪽을 읽지 않게 막는다.
struct PageFillGuard {
    PageFillGuard();
    ~PageFillGuard();
    PageFillGuard(const PageFillGuard&) = delete;
    PageFillGuard& operator=(const PageFillGuard&) = delete;
};

// 쪽 계산이 미뤄 둔 글 칸에 글자열과 글꼴 이름을 넣는다. 글자 폭은 그릴 때 잰다.
int ensurePageSpans(const model::Document& doc, Node& page);

// 배치가 끝난 줄을 줄 캐시 한 칸으로 옮긴다.
// vertPos 는 단 맨 위에서 그 줄까지다. 문단 y 에 줄 y 를 더한 값이다.
// 나중에 문서에 lineseg 를 기록할 때 이 칸을 그대로 쓴다.
model::LineSeg lineSegFor(const Node& line, std::int32_t vertPos);
// 저장할 문서 사본에 쓴다. lineSeg 가 없는 문단(고친 문단, lineSeg 없이 들어온 문단)에 우리가 조판한 줄을 lineSeg 로
// 적는다. 화면 문서에는 쓰지 않는다. lineSeg 가 생기면 그 문단의 커서 위치 단위가 바뀌기 때문이다. 적은 문단 수를 돌려준다.
int fillMissingLineSegs(model::Document& doc, const std::vector<Node>& pages);

// 위치만 기억한다. 글꼴 이름이나 글자열은 문서 모델에 있다.
// 문단은 단 안의 자리, 줄은 문단 안의 세로와 놓을 수 있는 너비,
// 글자와 글자처럼 취급하는 개체는 그 줄 안의 가로만 가진다.
struct LinePlace {
    std::int32_t y = 0;
    std::int32_t x = 0;
    std::int32_t width = 0;   // 이 줄에 놓을 수 있는 너비
    std::int32_t height = 0;  // 글자 높이와 글자처럼 취급한 개체 높이 중 큰 값
    std::vector<std::int32_t> xs;
};

struct ParagraphPlace {
    int index = -1;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<LinePlace> lines;
};

struct ColumnPlace {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<ParagraphPlace> paragraphs;
};

struct ObjectPlace {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    int paragraph = -1;
    std::vector<ColumnPlace> columns;
    std::vector<ObjectPlace> objects;
};

struct PagePlace {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::vector<ColumnPlace> columns;
    std::vector<ObjectPlace> objects;
};

// 스크롤과 커서. 배치 좌표와 같은 HWPUNIT 이다.
struct ViewState {
    int page = 0;
    std::int32_t scrollX = 0;
    std::int32_t scrollY = 0;
    int paragraph = 0;
    int cursor = 0;
};

std::vector<PagePlace> layoutPlaces(const model::Document& doc);

// 파싱한 문서와 그 위치, 스크롤, 커서를 한곳에 둔다.
struct Session {
    model::Document document;
    ViewState view;
    std::vector<PagePlace> pages;
};

}  // namespace chwp::render
