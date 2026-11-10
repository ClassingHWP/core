#pragma once

// HWP(바이너리) 와 HWPX(XML) 를 같은 구조로 담는 통합 문서 모델.
// 개념과 열거값 이름은 HWPX(OWPML) 를 따른다.
// 길이 단위는 모두 HWPUNIT (1 inch = 7200) 이다.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "chwp/common/bytes.hpp"

namespace chwp::model {

enum class SourceFormat { Unknown, Hwp, Hwpx };

// 언어별 글꼴 슬롯 (한글 고유: 7개 언어마다 글꼴/장평/자간을 따로 가진다)
enum class Lang : std::uint8_t { Hangul, Latin, Hanja, Japanese, Other, Symbol, User };
inline constexpr std::size_t kLangCount = 7;
const char* langName(Lang lang);

template <typename T>
using PerLang = std::array<T, kLangCount>;

// 0xRRGGBB. std::nullopt 은 "색 없음"
using Color = std::optional<std::uint32_t>;

struct FormatInfo {
    SourceFormat format = SourceFormat::Unknown;
    std::string version;  // 예: "5.1.1.0"
    std::string application;
    bool compressed = false;
    bool encrypted = false;
    bool distribution = false;  // 배포용 문서
};

struct Metadata {
    std::string title;
    std::string subject;
    std::string author;
    std::string keywords;
    std::string comments;
    std::string lastSavedBy;
    std::string createdAt;
    std::string modifiedAt;
    std::string language;
};

struct BeginNumbers {
    std::uint32_t page = 1;
    std::uint32_t footnote = 1;
    std::uint32_t endnote = 1;
    std::uint32_t picture = 1;
    std::uint32_t table = 1;
    std::uint32_t equation = 1;
};

struct Font {
    std::uint32_t id = 0;
    std::string face;
    std::string type;   // TTF, HFT ...
    std::string subst;  // 대체 글꼴. 글꼴 파일이 없을 때
    std::string base;   // 기본 글꼴
    Bytes typeInfo;     // 글꼴 유형 정보 10바이트. 없으면 비어 있다
};

// 규격에 필드 배치가 없는 레코드. 태그와 원본 바이트를 그대로 둔다.
struct KeptRecord {
    std::uint16_t tag = 0;
    Bytes data;
};

// 문단 영역 태그. 끝은 포함하지 않는다. 종류 2는 형광펜이고 data 하위 24비트가 색이다.
struct TextRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::uint32_t kind = 0;
    std::uint32_t data = 0;
};

struct CharShape {
    std::uint32_t id = 0;
    std::int32_t height = 1000;  // 1000 = 10pt
    PerLang<std::uint32_t> fontRef{};
    PerLang<std::int32_t> ratio{};    // 장평 %
    PerLang<std::int32_t> spacing{};  // 자간 %
    PerLang<std::int32_t> relSize{};  // 상대 크기 %
    PerLang<std::int32_t> offset{};   // 글자 위치 %
    Color textColor = 0;
    Color shadeColor;
    bool bold = false;
    bool italic = false;
    std::string underline = "NONE";  // NONE, BOTTOM, TOP. 대화상자에 중간은 없다.
    // 밑줄·취소선 모양. 저장값에 1을 더한 표 칸 선 이름. 0은 실선. 굵기는 저장값으로 고른다.
    std::string underlineShape = "SOLID";
    std::uint8_t underlineShapeId = 0;
    Color underlineColor;
    bool strikeout = false;
    std::string strikeShape = "SOLID";
    std::uint8_t strikeShapeId = 0;
    Color strikeColor;
    std::uint8_t outline = 0;  // 0 없음, 1 실선, 2 점선, 3 굵은 실선, 4 파선, 5 일점쇄선, 6 이점쇄선
    std::uint8_t shadow = 0;   // 0 없음, 1 비연속, 2 연속
    std::int8_t shadowOffsetX = 10;
    std::int8_t shadowOffsetY = 10;
    Color shadowColor = 0xB2B2B2u;
    bool emboss = false;
    bool engrave = false;
    std::uint8_t emphasis = 0;  // 0 없음, 1 채운 동그라미, 2 빈 동그라미, 3 ˇ, 4 ˜, 5 ·, 6 :
    bool superscript = false;
    bool subscript = false;
    bool useFontSpace = false;  // 글꼴에 어울리는 빈칸. 띄어쓰기만 영문 글꼴 폭을 쓴다.
    // 커닝 비트. 줄 폭에는 더하지 않는다. 한글도 이 비트로 줄 폭이나 글자 간격을 바꾸지 않는다.
    bool useKerning = false;
    std::uint32_t borderFillId = 0;
};

struct ParaShape {
    std::uint32_t id = 0;
    std::string align = "JUSTIFY";  // JUSTIFY, LEFT, RIGHT, CENTER, DISTRIBUTE, DISTRIBUTE_SPACE
    std::string vertAlign = "BASELINE";  // BASELINE(글꼴 기준), TOP, CENTER, BOTTOM
    std::int32_t indent = 0;        // 첫 줄 (음수 = 내어쓰기)
    std::int32_t marginLeft = 0;
    std::int32_t marginRight = 0;
    std::int32_t spacingPrev = 0;
    std::int32_t spacingNext = 0;
    std::string lineSpacingType = "PERCENT";  // PERCENT, FIXED, BETWEEN_LINES, AT_LEAST
    std::int32_t lineSpacing = 160;
    std::string breakLatinWord = "KEEP_WORD";     // KEEP_WORD, HYPHENATION, BREAK_WORD
    std::string breakNonLatinWord = "KEEP_WORD";  // 이름과 뜻이 반대다. KEEP_WORD 가 글자, BREAK_WORD 가 어절(한글 2024)
    std::int32_t condense = 0;  // 공백 최소값 0~75. 20이면 빈칸을 원래의 80%까지 줄여 그 줄에 더 넣는다.
    bool widowOrphan = false;
    bool keepWithNext = false;
    bool keepLines = false;
    bool pageBreakBefore = false;
    std::string headingType = "NONE";  // NONE, OUTLINE, NUMBER, BULLET
    std::uint32_t headingIdRef = 0;
    std::uint32_t headingLevel = 0;
    bool checked = false;  // 체크된 글머리표. 켜지면 체크 문자를 쓴다
    std::uint32_t tabDefId = 0;
    std::uint32_t borderFillId = 0;
    bool snapToGrid = false;       // 편집 용지 줄 격자. 조판에는 아직 쓰지 않는다
    bool fontLineHeight = false;   // 글꼴에 어울리는 줄 높이. 기준선을 글꼴 어센더에 둔다
    bool autoSpacingEng = false;   // 한글과 영어 사이 자동 간격
    bool autoSpacingNum = false;   // 한글과 숫자 사이 자동 간격
    bool suppressLineNumbers = false;  // 줄 번호 숨기기(한글 SuppressLineNumbers). 이 문단 줄은 번호를 쓰지도 세지도 않는다
    std::string lineWrap = "BREAK";
    std::int32_t borderOffsetLeft = 0;
    std::int32_t borderOffsetRight = 0;
    std::int32_t borderOffsetTop = 0;
    std::int32_t borderOffsetBottom = 0;
    bool borderConnect = false;
    bool borderIgnoreMargin = false;
};

struct Style {
    std::uint32_t id = 0;
    std::string type = "PARA";  // PARA, CHAR
    std::string name;
    std::string engName;
    std::uint32_t paraShapeId = 0;
    std::uint32_t charShapeId = 0;
    std::uint32_t nextStyleId = 0;
    std::int32_t langId = 0;
};

struct BinItem {
    std::string id;         // Control::binItemId 가 참조하는 키 (HWP: "1","2"..., HWPX: "image1"...)
    std::string storage;    // EMBEDDING, LINK, STORAGE
    std::string path;       // 컨테이너 내부 경로 또는 링크 경로
    std::string extension;  // bmp, png ...
    std::string mediaType;
    std::uint64_t size = 0; // HWP 는 loadBinData 가 아니면 압축된 저장 크기일 수 있다
    std::uint32_t compression = 0;  // HWP: 0 문서 설정, 1 압축, 2 비압축
    Bytes data;             // ParseOptions::loadBinData 일 때만 채워진다. 비어 있으면 그릴 때 읽는다
};

struct BorderSide {
    std::string type = "NONE";
    std::string color = "#000000";
    std::int32_t width = 0;  // HWPUNIT
};

struct DiagonalSpec {
    std::string type = "NONE";  // NONE, CENTER, CENTER_BELOW, CENTER_ABOVE, ALL
    bool crooked = false;
    bool counter = false;
};

struct BorderFill {
    std::uint32_t id = 0;
    BorderSide left, right, top, bottom;
    DiagonalSpec slash;
    DiagonalSpec backSlash;
    BorderSide diagonal;
    std::string centerLine = "NONE";  // NONE, VERTICAL, HORIZONTAL, CROSS
    bool breakCellSeparateLine = false;
    std::string faceColor;   // 단색. "none" 이면 칠하지 않음
    // 무늬. 비어 있으면 없다. HORIZONTAL, VERTICAL, SLASH(\ 모양), BACK_SLASH(/ 모양), CROSS, CROSS_DIAGONAL.
    // HWP 무늬 번호는 이 차례대로 0~5, 없으면 -1 이다(한글 2024, tmp/hatch).
    std::string hatchStyle;
    std::string hatchColor;  // 무늬 색
    std::string gradColor;   // 그러데이션 끝 색
    std::int32_t gradAngle = 0;  // 0 은 아래에서 위, 90 은 왼쪽에서 오른쪽
    std::string imageId;     // 그림 채우기. 비어 있으면 없다
    int imageMode = -1;      // 0 바둑판, 5 크기에 맞춤, 6 가운데. -1 이면 그림 없음
};

// 번호·개요 한 수준의 형식. format 의 ^1 은 그 수준 번호, ^n 은 상위부터의 경로다.
struct ParaHead {
    int level = 1;
    int start = 1;
    std::string align = "LEFT";
    std::string numFormat = "DIGIT";
    std::string format;
    std::uint32_t charShapeId = 0xFFFFFFFFu;
    bool useInstWidth = true;
    bool autoIndent = true;
    std::int32_t widthAdjust = 0;
    std::int32_t textOffset = 50;
    std::string textOffsetType = "PERCENT";
};

struct Numbering {
    std::uint32_t id = 0;
    int start = 0;
    std::vector<ParaHead> levels;
};

struct Bullet {
    std::uint32_t id = 0;
    std::string text;
    std::string checkedText;  // 체크된 문단일 때 대신 그리는 문자
    std::string imageId;      // 비어 있으면 문자 글머리표. 있으면 그림 글머리표
    int bright = 0;
    int contrast = 0;
    int effect = 0;  // 0 원래, 1 회색조, 2 흑백
    ParaHead head;
};

// 문단이 참조하는 탭 위치. pos 는 문단 왼쪽 여백에서 잰 HWPUNIT.
struct TabStop {
    std::int32_t pos = 0;
    std::string type = "LEFT";    // LEFT, RIGHT, CENTER, DECIMAL
    std::string leader = "NONE";
};

struct TabDef {
    std::uint32_t id = 0;
    bool autoTabLeft = false;
    bool autoTabRight = false;
    std::vector<TabStop> stops;
};

// 메모 모양(HWP DocInfo MEMO_SHAPE, HWPX hh:memoPr). 기본값은 한글 새 메모 모양이다.
struct MemoShape {
    std::uint32_t width = 15591;      // HWPUNIT. 메모 상자 너비(55mm)
    std::uint8_t lineType = 1;        // 선 종류 번호(0 없음, 1 실선, 2 점선, 3 파선 …)
    std::uint8_t lineWidth = 1;       // 선 굵기 번호
    std::string lineColor = "#B6D7AE";
    std::string fillColor = "#F0FFE9";
    std::string activeColor = "#CFF1C7";  // 고른 메모 채우기
    std::uint32_t memoType = 0;       // 0 NOMAL, 1 USER_INSERT … 높은 비트는 파일 그대로
    bool operator==(const MemoShape&) const = default;
};

struct DocHeader {
    std::uint32_t sectionCount = 0;
    BeginNumbers beginNumbers;
    PerLang<std::vector<Font>> fonts;
    std::size_t borderFillCount = 0;
    std::vector<BorderFill> borderFills;
    std::vector<CharShape> charShapes;
    std::vector<ParaShape> paraShapes;
    std::vector<Style> styles;
    std::vector<MemoShape> memoShapes;  // 1 부터 센다(구역·메모 필드의 모양 번호)
    std::vector<Numbering> numberings;
    std::vector<Bullet> bullets;
    std::vector<TabDef> tabDefs;
    std::vector<BinItem> binItems;
    std::vector<std::string> forbiddenWords;
    std::vector<KeptRecord> kept;  // 메모 모양, 변경 추적, 금칙, 호환 문서 등 아직 필드가 없는 레코드
};

// ---------------------------------------------------------------------------
// 본문
// ---------------------------------------------------------------------------

struct PageDef {
    std::uint32_t width = 59528;  // A4
    std::uint32_t height = 84188;
    bool landscape = false;  // 넓게(가로). 저장된 가로가 세로보다 짧으면 긴 변을 가로로 바꾼다.

    void applyOrientation() {
        if (landscape && width < height) std::swap(width, height);
    }
    std::uint32_t marginLeft = 0;
    std::uint32_t marginRight = 0;
    std::uint32_t marginTop = 0;
    std::uint32_t marginBottom = 0;
    std::uint32_t marginHeader = 0;
    std::uint32_t marginFooter = 0;
    std::uint32_t marginGutter = 0;
    // 제본 방식. 0 한쪽(제본 여백은 늘 왼쪽), 1 맞쪽(짝수 쪽은 좌우 여백을 바꾸고 제본 여백이 오른쪽),
    // 2 위로(홀수 쪽은 위, 짝수 쪽은 아래에 제본 여백)
    std::uint8_t gutterType = 0;

    // 파일에는 넓게여도 좁게 놓은 크기(세로가 긴 쪽)를 쓴다. 모델은 놓인 그대로 갖고 있다.
    std::uint32_t storedWidth() const { return landscape && width > height ? height : width; }
    std::uint32_t storedHeight() const { return landscape && width > height ? width : height; }

    // 그 쪽 번호에 놓이는 여백. 제본 여백을 알맞은 쪽 여백에 더하고 제본 여백은 0 으로 둔다.
    // 본문 폭·높이는 홀수 쪽과 짝수 쪽이 같다. 홀수 쪽(1)으로 줄을 나누고, 쪽마다 자리만 옮긴다.
    PageDef side(int pageNumber) const {
        PageDef out = *this;
        const bool even = pageNumber % 2 == 0;
        if (gutterType == 1 && even) {
            out.marginLeft = marginRight;
            out.marginRight = marginLeft + marginGutter;
        } else if (gutterType == 2) {
            if (even) out.marginBottom = marginBottom + marginGutter;
            else out.marginTop = marginTop + marginGutter;
        } else {
            out.marginLeft = marginLeft + marginGutter;
        }
        out.marginGutter = 0;
        return out;
    }
    // 세로쓰기 쪽의 틀과 제본 여백. 제본 여백은 종이에 놓인 그대로 붙는다(한글 2024 PDF·lineseg, vert-gutter-*).
    // 한쪽은 늘 종이 왼쪽(틀의 아래)이다. 맞쪽은 홀수 쪽이 그와 같고, 짝수 쪽은 종이 좌우가 뒤집혀 오른쪽이 아래 여백 +
    // 제본, 왼쪽이 위 여백이다. 위로는 줄 길이에서 빠져 홀수 쪽은 줄 시작(종이 위), 짝수 쪽은 줄 끝(종이 아래)이다.
    PageDef turnedSide(int pageNumber) const {
        PageDef plain = *this;
        plain.marginGutter = 0;
        PageDef out = plain.turned();
        const bool even = pageNumber % 2 == 0;
        if (gutterType == 2) {
            if (even) out.marginRight += marginGutter;
            else out.marginLeft += marginGutter;
        } else if (gutterType == 1 && even) {
            out.marginTop = marginBottom + marginGutter;
            out.marginBottom = marginTop;
        } else {
            out.marginBottom += marginGutter;
        }
        return out;
    }
    // 세로쓰기 본문의 틀. 종이를 오른쪽으로 90° 돌려 가로처럼 줄을 나눈다. 줄은 종이 위에서 아래로,
    // 다음 줄은 왼쪽이다. 줄 시작은 왼쪽 + 머리말 여백, 줄 끝은 오른쪽 + 꼬리말 여백, 첫 줄은 위 여백,
    // 마지막 줄은 아래 여백까지 간다(한글 PDF·lineseg 로 잼). 머리말·꼬리말은 가로 그대로 종이에 놓는다.
    // 돌린 틀의 (x, y) 는 종이의 (종이 폭 − y, x) 다.
    PageDef turned() const {
        PageDef out = *this;
        out.width = height;
        out.height = width;
        out.marginLeft = marginLeft + marginHeader;
        out.marginRight = marginRight + marginFooter;
        out.marginTop = marginTop;
        out.marginBottom = marginBottom;
        out.marginHeader = 0;
        out.marginFooter = 0;
        return out;
    }
    // 홀수 쪽과 짝수 쪽의 본문 자리가 다른가
    bool sidesDiffer() const {
        const PageDef odd = side(1);
        const PageDef even = side(2);
        return odd.marginLeft != even.marginLeft || odd.marginTop != even.marginTop;
    }
};

// 조판 결과 캐시 (HWP: PARA_LINE_SEG, HWPX: hp:lineseg)
struct LineSeg {
    std::uint32_t textPos = 0;
    std::int32_t vertPos = 0;
    std::int32_t vertSize = 0;
    std::int32_t textHeight = 0;
    std::int32_t baseline = 0;
    std::int32_t spacing = 0;
    std::int32_t horzPos = 0;
    std::int32_t horzSize = 0;
    std::uint32_t flags = 0;

    bool operator==(const LineSeg&) const = default;
};

// 개체 공통 배치 속성
struct Placement {
    bool treatAsChar = false;           // 글자처럼 취급
    bool flowWithText = true;           // 세로 기준이 문단(PARA)일 때 본문 영역으로 제한
    bool allowOverlap = true;           // 다른 개체와 겹침 허용 (본문 영역 제한이면 무시됨)
    bool relativeWidth = false;         // 폭 기준이 절대값(ABSOLUTE)이 아닌 상대 비율일 때 true
    bool relativeHeight = false;        // 높이 기준이 절대값(ABSOLUTE)이 아닌 상대 비율일 때 true
    std::string textWrap;               // SQUARE, TOP_AND_BOTTOM, BEHIND_TEXT, IN_FRONT_OF_TEXT
    std::string textFlow = "BOTH_SIDES";  // BOTH_SIDES, LEFT_ONLY, RIGHT_ONLY, LARGEST_ONLY
    // 개체 번호 범주: NONE, PICTURE, TABLE, EQUATION. 캡션의 자동 번호가 이 범주를 센다.
    // 비어 있으면(새 개체) 개체 종류로 정한다(objectNumberingType).
    std::string numberingType;
    // HWP 개체 공통 속성 중 모델이 풀지 않는 비트(크기 보호 등). 읽은 그대로 다시 쓴다.
    std::uint32_t attrKeep = 0;
    std::string vertRelTo;              // PAPER, PAGE, PARA
    std::string horzRelTo;              // PAPER, PAGE, COLUMN, PARA
    std::string vertAlign = "TOP";      // TOP, CENTER, BOTTOM, INSIDE, OUTSIDE
    std::string horzAlign = "LEFT";     // LEFT, CENTER, RIGHT, INSIDE, OUTSIDE
    std::int32_t vertOffset = 0;
    std::int32_t horzOffset = 0;
    std::int32_t groupX = 0;  // 묶음 안 X. 그룹으로 묶인 자식만 이 좌표를 쓴다.
    std::int32_t groupY = 0;
    // 배율·회전 행렬을 뒤에서부터 원점에 적용한 이동. groupX/groupY 에는 이미 더해져 있다.
    std::int32_t matrixX = 0;
    std::int32_t matrixY = 0;
    double scaleX = 1;  // 배율 행렬 e1, e5 의 곱. 1이면 배율이 없다.
    double scaleY = 1;
    // HWP 묶음 자식은 가장 바깥 묶음 기준이다. 안쪽 묶음에 넣을 때 그 묶음 위치를 뺀다.
    bool groupInRoot = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t zOrder = 0;
    std::int32_t marginLeft = 0;    // 바깥 여백. 본문이 개체를 피하는 영역이 이만큼 더 커진다.
    std::int32_t marginRight = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginBottom = 0;
    bool flipHorizontal = false;  // hp:flip. 상자 안에서 좌우를 뒤집는다.
    bool flipVertical = false;
};

struct Paragraph;

// 문단 리스트: 셀, 머리말/꼬리말, 각주/미주, 글상자, 캡션이 공통으로 쓴다.
// 줄 번호(한글 2024 구역 설정의 줄 번호). 숨긴 한글 2024 의 PDF 로 확인한 그리기:
//   - 번호는 단 왼쪽에서 distance 만큼 떨어진 자리에 오른쪽 끝을 맞추고, 그 줄과 기준선을 같게 둔다(문단 여백·정렬과 무관).
//   - 본문 줄마다 하나씩 센다(빈 줄, 표가 든 줄 포함, 표 칸·글상자 안 줄은 세지 않는다). 단은 차례대로 잇는다.
//   - countBy 의 배수인 번호만 쓴다. 시작 번호 0 은 1 이다. 글자는 글자 모양 0 번이다.
//   - restart: 0 문서 끝까지 이어서, 1 쪽마다 새로, 2 구역마다 새로.
// HWP 는 구역 정의 속성 비트 24(보이기)와 레코드 38 바이트 restart(u8), countBy(u16), distance(u32), start(u16)다.
struct LineNumberShape {
    bool show = false;
    std::uint8_t restart = 0;
    std::uint16_t countBy = 0;
    std::uint32_t distance = 0;  // HWPUNIT
    std::uint16_t start = 0;
    bool operator==(const LineNumberShape&) const = default;
};

struct SubList {
    std::string vertAlign;  // TOP, CENTER, BOTTOM. 비어 있으면 위
    std::uint8_t textDirection = 0;  // 글상자·칸 글자 방향. 0 가로, 1 세로(영문 눕힘), 2 세로(영문 세움)
    std::string applyPageType;     // 바탕쪽: BOTH, EVEN, ODD, LAST_PAGE, OPTIONAL_PAGE
    // 바탕쪽(HWP 읽기): 임의 쪽 차례, 겹치게 하기, 인쇄 시 맨 위.
    std::uint32_t masterPageNumber = 0;
    bool masterDuplicate = false;
    bool masterFront = false;
    std::uint32_t textWidth = 0;   // 머리말/꼬리말 글 영역. 0이면 본문 너비
    std::uint32_t textHeight = 0;  // 머리말/꼬리말 글 영역. 0이면 머리말·꼬리말 여백
    std::vector<Paragraph> paragraphs;
};

struct TableCell {
    std::uint32_t row = 0;
    std::uint32_t col = 0;
    std::uint32_t rowSpan = 1;
    std::uint32_t colSpan = 1;
    std::uint32_t width = 0;   // 최소 너비. 내용과 좌우 여백이 더 넓으면 늘어난다.
    std::uint32_t height = 0;  // 최소 높이. 내용과 상하 여백이 더 높으면 늘어난다.
    std::uint32_t borderFillId = 0;
    bool hasMargin = false;  // 참이면 아래 여백, 거짓이면 표 안쪽 여백
    bool header = false;     // 제목 줄 칸 여부
    std::int32_t marginLeft = 0;
    std::int32_t marginRight = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginBottom = 0;
    std::string vertAlign;
    std::string name;  // 셀 필드 이름(표/셀 속성 셀 탭). 계산식 LEFT:이름·ALL:이름 이 이 이름으로 셀을 고른다
    SubList content;
};

struct Table {
    std::uint32_t rowCount = 0;
    std::uint32_t colCount = 0;
    std::uint32_t cellSpacing = 0;
    std::uint32_t borderFillId = 0;
    // 쪽 경계에서: CELL 나눔(HWP 2), TABLE 셀 단위로 나눔(HWP 1), NONE 나누지 않음(HWP 0). 한글 2024 표/셀 속성 [표] 탭으로 확인.
    std::string pageBreak = "CELL";
    bool repeatHeader = false;
    bool noAdjust = false;
    std::int32_t marginLeft = 0;  // 칸이 자체 여백을 쓰지 않을 때의 안쪽 여백
    std::int32_t marginRight = 0;
    std::int32_t marginTop = 0;
    std::int32_t marginBottom = 0;
    std::vector<TableCell> cells;
};

// 표/그림/도형/수식의 캡션
struct Caption {
    std::string side = "BOTTOM";  // LEFT, RIGHT, TOP, BOTTOM
    bool fullSize = false;        // 캡션 폭에 개체 바깥 여백 포함 (가로 방향일 때)
    std::uint32_t width = 0;      // 캡션 폭 (세로 방향일 때)
    std::int32_t gap = 0;         // 틀과의 간격. 음수면 그 거리만큼 개체와 겹친다.
    std::uint32_t lastWidth = 0;  // 저장 당시 텍스트 최대 길이
    std::uint32_t attr = 0;       // 방향·여백 포함 원본 비트
    SubList content;
};

enum class ControlKind {
    Unknown,
    SectionDef,   // secd : 구역 정의
    ColumnDef,    // cold : 단 정의
    Table,        // tbl
    Picture,      // gso / $pic
    Shape,        // gso / $rec, $ell, $lin, $arc, $pol, $cur ...
    Container,    // gso / $con : 묶음 개체
    Ole,          // gso / $ole
    Equation,     // eqed
    Header,       // head
    Footer,       // foot
    Footnote,     // fn
    Endnote,      // en
    AutoNumber,   // atno
    NewNumber,    // nwno
    PageHiding,   // pghd
    PageNumPos,   // pgnp
    Bookmark,     // bokm
    Field,        // %hlk, %clk ...
    HiddenComment,// tcmt
    Dutmal,       // tdut : 덧말
    Compose,      // tcps : 글자 겹침
    FieldEnd      // 필드 끝. 앞의 Field(하이퍼링크 등)의 범위가 여기서 끝난다. 글자 칸은 없다
};
const char* controlKindName(ControlKind kind);
// 덧말·글자 겹치기 값의 HWP 번호 ↔ HWPX 이름. 번호 차례는 HWP 5.0 문서 형식(덧말 자리 0 위·1 아래·2 가운데,
// 정렬은 문단 정렬 차례)과 OWPML 열거 차례다. 글자 겹치기 CHAR 0·SHAPE_CIRCLE 1 은 한글 2024 파일로 확인했다.
const char* dutmalPosName(std::uint32_t code);
std::uint32_t dutmalPosCode(const std::string& name);
const char* dutmalAlignName(std::uint32_t code);
std::uint32_t dutmalAlignCode(const std::string& name);
const char* composeCircleName(std::uint32_t code);
std::uint32_t composeCircleCode(const std::string& name);
// 개체 번호 범주. 읽은 값이 있으면 그대로, 없으면 표 TABLE, 수식 EQUATION, 그림 PICTURE, 그 밖 NONE.
// 한글은 캡션 자동 번호와 범주가 맞지 않는 HWP 를 열지 않는다.
const char* objectNumberingType(ControlKind kind, const std::string& stored);

// 도형 점. 다각형은 원본 좌표를 현재 크기에 맞춘 값이다.
struct ShapePoint {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

// 그림 자르기·색. 자르기 좌표는 최초 이미지 크기 안이다.
// 자르기가 그 크기와 같거나 없으면 비트맵 전체를 상자에 넣는다.
// rect 는 처음 넣은 그림 사각형(imgRect)이다. 0이면 원본 크기와 같다.
// 점을 끌어 키워도 원본·자르기·이 사각형은 그대로 두고, 배율만 새 상자에 맞춘다.
struct PictureView {
    std::int32_t clipLeft = 0;
    std::int32_t clipTop = 0;
    std::int32_t clipRight = 0;
    std::int32_t clipBottom = 0;
    std::int32_t originWidth = 0;
    std::int32_t originHeight = 0;
    std::int32_t rectWidth = 0;
    std::int32_t rectHeight = 0;
    std::int32_t padLeft = 0;
    std::int32_t padRight = 0;
    std::int32_t padTop = 0;
    std::int32_t padBottom = 0;
    int bright = 0;         // -100..100
    int contrast = 0;       // -100..100
    int effect = 0;         // 0 원래, 1 회색조, 2 흑백
    int transparency = 0;   // 0 불투명, 255 완전 투명
};

// 구역의 각주·미주 모양. 번호 다시 시작과 미주를 모으는 곳은 파일 속성이다.
struct NoteShape {
    std::string numFormat = "DIGIT";
    std::string userChar;
    std::string prefix;
    std::string suffix = ")";
    bool contentSuperscript = false;  // 주 내용의 번호. 글자 크기는 바꾸지 않는다
    bool beneathText = false;         // 본문 바로 아래. 꺼져 있으면 쪽 아래
    int place = 0;       // 각주 0 단마다, 1 통단, 2 오른쪽 단. 미주 0 문서 끝, 1 구역 끝
    int numbering = 0;   // 0 이어서, 1 구역마다, 2 쪽마다
    std::uint16_t startNumber = 1;
    // 0 선 없음, -1 5cm, -2 2cm, -3 단 너비의 1/3, -4 단 너비(한글 2024 목록), 양수는 HWPUNIT
    std::int32_t lineLength = -1;
    std::string lineType = "SOLID";
    std::int32_t lineWidth = 34;  // 0.12mm. 메뉴의 첫 굵기
    std::string lineColor = "#000000";
    std::int32_t above = 850;    // 구분선 위
    std::int32_t below = 567;    // 구분선 아래
    std::int32_t between = 283;  // 주와 주 사이. 마지막 뒤에는 더하지 않는다
};

// 단 하나의 폭과, 그 단 오른쪽에 두는 간격. 마지막 단의 간격은 0이다.
struct ColumnWidth {
    std::int32_t width = 0;
    std::int32_t gap = 0;
};

// 쪽 테두리/배경 한 칸. 구역마다 양쪽, 짝수, 홀수 세 칸이다.
// textBorder 가 켜지면 쪽이다. hwpx에는 PAPER, hwp 비트 0은 1이다.
// 꺼지면 종이다. hwpx에는 CONTENT, hwp 비트 0은 0이다.
// fillArea 는 0 종이, 1 쪽, 2 테두리. borderFillId 가 0이면 그리지 않는다.
struct PageBorderFill {
    std::string type = "BOTH";
    bool textBorder = true;
    bool headerInside = false;
    bool footerInside = false;
    int fillArea = 0;
    std::int16_t left = 1417;
    std::int16_t right = 1417;
    std::int16_t top = 1417;
    std::int16_t bottom = 1417;
    std::uint16_t borderFillId = 1;
};

inline std::array<PageBorderFill, 3> defaultPageBorders() {
    std::array<PageBorderFill, 3> out;
    out[0].type = "BOTH";
    out[1].type = "EVEN";
    out[2].type = "ODD";
    return out;
}

struct Control {
    ControlKind kind = ControlKind::Unknown;
    std::string ctrlId;     // HWP 4글자 컨트롤 ID (HWPX 는 대응되는 ID 로 변환)
    std::string shapeType;  // Shape/Picture/Container: pic, rect, ellipse, line, arc, polygon, curve, container, ole
    std::optional<Placement> placement;

    std::optional<PageDef> page;  // SectionDef
    std::uint32_t columnCount = 0;  // ColumnDef
    std::int32_t columnGap = 0;     // 단 사이 간격. 같은 너비일 때 sameGap
    std::int32_t columnRegionGap = 1134;  // 같은 쪽에서 단 설정이 바뀔 때 띄우는 간격. 4mm
    std::int32_t defaultTab = 0;    // 구역 기본 탭 간격. 탭 정의에 위치가 없을 때 쓴다
    std::uint8_t textDirection = 0;  // 구역 글자 방향. 0 가로, 1 세로(영문 눕힘), 2 세로(영문 세움)
    // HWP 구역 정의 속성 중 모델이 풀지 않는 비트. 읽은 그대로 다시 쓴다(한글은 이 비트가 빠지면 파일을 열지 않는다).
    std::optional<std::uint32_t> sectionAttrKeep;
    std::int32_t lineGrid = 0;      // 구역 줄 격자 간격. 0 이면 끔
    std::int32_t charGrid = 0;      // 구역 글자 격자 간격. 0 이면 끔
    bool wongoji = false;           // 원고지 정서법
    bool columnSameSize = true;
    std::vector<ColumnWidth> columnWidths;  // 너비가 다를 때 단마다 폭·간격
    std::string columnLineType;             // 단 구분선. 비어 있거나 NONE 이면 그리지 않는다
    std::int32_t columnLineWidth = 0;
    std::string columnLineColor;
    std::string columnType;    // HWPX 이름: NEWSPAPER(일반), BALANCED_NEWSPAPER(배분), PARALLEL(평행)
    std::string columnLayout;  // LEFT, RIGHT, BOTH
    std::optional<Table> table;   // Table
    std::string binItemId;        // Picture
    PictureView picture;
    std::string script;           // Equation (한글 수식 스크립트)
    int baseLine = -1;            // 수식 상자 높이의 퍼센트. 0은 85. 없으면 -1. 조판에 쓴다
    std::int32_t baseUnit = 1000; // 수식 글자 크기. 1000 = 10pt. 측정에만 쓴다
    std::string equationFont;     // 수식 글꼴. 비어 있으면 HYhwpEQ. 재기와 그리기에 쓴다
    int fileBaseLine = -1;        // 파일 기준선. 조판 baseLine과 따로 요소 비교에만 쓴다
    std::string applyPageType;    // Header/Footer: BOTH, EVEN, ODD
    std::string numType;          // AutoNumber/NewNumber: PAGE, FOOTNOTE ...
    std::string numFormat;        // DIGIT, ROMAN_CAPITAL ... 비어 있으면 아라비아 숫자
    std::string prefix;           // 번호 앞 장식
    std::string suffix;           // 번호 뒤 장식
    std::string pageNumPos;       // PageNumPos: TOP_LEFT, BOTTOM_CENTER ...
    bool hideHeader = false;      // 감추기, 또는 구역 전체 머리말 감춤
    bool hideFooter = false;
    bool hideMaster = false;      // 바탕쪽 감춤
    bool hidePageNum = false;
    bool hideBorder = false;      // PageHiding: 쪽 테두리 감춤
    bool hideFill = false;        // PageHiding: 쪽 배경 감춤
    bool hideFirstHeader = false;  // 구역 첫 쪽만
    bool hideFirstFooter = false;
    bool hideFirstMaster = false;
    bool hideFirstPageNum = false;
    // SectionDef: 구역 설정(한글 2024 구역 설정 대화 상자). 시작 쪽 번호 종류: 0 이어서, 1 짝수, 2 홀수, 3 사용자(쪽 번호 값).
    std::uint8_t startsOn = 0;
    std::uint16_t pictureStart = 0;   // 그림 시작 번호. 0 은 이어서
    std::uint16_t tableStart = 0;     // 표 시작 번호
    std::uint16_t equationStart = 0;  // 수식 시작 번호
    std::uint16_t memoShapeId = 0;    // 구역 메모 모양(1 부터, 0 은 기본 모양)
    // 쪽 테두리·배경을 보일 쪽: SHOW_ALL, HIDE_FIRST(첫 쪽만 감춤), SHOW_FIRST(첫 쪽만 보임).
    std::string borderShow = "SHOW_ALL";
    std::string fillShow = "SHOW_ALL";
    bool hideFirstEmptyLine = false;  // 빈 줄 감추기
    LineNumberShape lineNumbers;
    std::uint32_t number = 0;
    std::string name;             // Bookmark/Field 이름
    std::string command;          // Field 명령 (하이퍼링크 URL 등)
    // Field 속성(HWP CTRL_HEADER 의 속성 u32: bit0 고칠 수 있음, bit15 내용이 고쳐짐)과 기타 속성 u8(HWPX
    // parameters 의 Prop). 한글 계산식은 기타 속성이 8 이다.
    std::uint32_t fieldProps = 0;
    std::uint8_t fieldExtra = 0;
    // 필드 고유 id(HWP CTRL_HEADER 명령 뒤 u32, HWPX fieldBegin@id). 0 이면 저장할 때 매긴다.
    std::uint32_t fieldId = 0;
    // 메모 필드(%%me): 메모 번호(HWP MEMO_LIST 짝, 저장할 때 다시 매김), 답글이면 원 메모의 필드 고유 id.
    std::uint32_t memoNumber = 0;
    std::uint32_t memoParentId = 0;
    std::string text;             // Dutmal 본말, Compose 겹칠 글자
    std::string subText;          // Dutmal 덧말
    // 덧말(tdut) 모양. HWP 레코드 차례(본말, 덧말, 자리 u32, 크기 비율 u32, option u32, 스타일 u32, 정렬 u32)와
    // HWPX hp:dutmal 속성(posType, szRatio, option, styleIDRef, align)이 같은 값이다.
    std::string dutmalPos = "TOP";       // TOP, BOTTOM, CENTER
    std::uint32_t dutmalSizeRatio = 0;   // 덧말 글자 크기 %(0 은 한글 기본)
    std::uint32_t dutmalOption = 0;
    std::uint32_t dutmalStyle = 0;
    std::string dutmalAlign = "JUSTIFY"; // 문단 정렬 이름(JUSTIFY, LEFT, RIGHT, CENTER, DISTRIBUTE, DISTRIBUTE_SPACE)
    // 글자 겹치기(tcps) 모양. HWP 레코드: 겹칠 글자, 테두리 u8, 글자 크기 i8, 겹치기 u8, 글자 모양 수 u8, 글자 모양 u32×수.
    // HWPX hp:compose circleType·charSz·composeType·charPrCnt·hp:charPr. 한글 2024 파일로 확인:
    // CHAR 0, SHAPE_CIRCLE 1, OVERLAP 1, 글자 모양은 10 칸(쓰지 않는 칸은 0xFFFFFFFF).
    std::string composeCircle = "CHAR";
    std::int8_t composeCharSize = 0;
    std::string composeType = "OVERLAP";  // OVERLAP, SPREAD
    std::vector<std::uint32_t> composeCharShapes;
    std::optional<Caption> caption;
    std::int32_t boxPadLeft = 0;    // 글상자 안쪽 여백. 글상자 안 줄 폭에서 빼는 값.
    std::int32_t boxPadRight = 0;
    std::int32_t boxPadTop = 0;
    std::int32_t boxPadBottom = 0;
    // 글상자 글 폭(drawText lastWidth). 한글은 여기서 안쪽 여백을 빼 줄 폭을 잡는다. 0이면 개체 폭이다.
    std::uint32_t boxTextWidth = 0;
    std::string strokeType;          // 도형 테두리. NONE 이면 선 없음
    std::string strokeColor;
    std::int32_t strokeWidth = 0;    // HWPUNIT
    int strokeEnd = 1;               // 선 끝. 0 둥근 끝, 그 외 평평
    std::string fillColor;
    std::string gradColor;
    std::int32_t gradAngle = 0;
    int cornerRatio = 0;  // 사각형 모서리 곡률 %. 0 은 직각, 20 은 둥근 모서리, 50 은 반원
    std::vector<ShapePoint> points;  // 다각형·곡선·선·타원. 현재 크기 안의 좌표
    std::vector<KeptRecord> kept;    // 글맵시·양식·차트·비디오 등 원본 레코드

    NoteShape footnoteShape;  // 구역 정의의 각주 모양
    NoteShape endnoteShape;   // 구역 정의의 미주 모양
    std::array<PageBorderFill, 3> pageBorders = defaultPageBorders();

    std::vector<SubList> subLists;  // 머리말/각주/글상자 내용
    std::vector<Control> children;  // Container 자식 개체
    std::vector<std::string> masterPageRefs;  // 구역 정의가 가리키는 바탕쪽 id
};

// 인라인 항목: 텍스트 또는 컨트롤(텍스트 흐름 안의 위치 = 앵커)
struct InlineItem {
    enum class Type { Text, Control };
    Type type = Type::Text;
    std::string text;  // UTF-8. 탭 '\t', 줄바꿈 '\n', 묶음 빈칸 U+00A0, 고정폭 빈칸 U+2007
    std::string markpen;  // 형광펜 #RRGGBB. 비어 있으면 없다. 글자 배경과 따로 둔다
    bool ignoreSpacing = false;  // 쪽 번호. 장평은 적용하고 자간은 더하지 않는다
    // 조판 때 자동 번호 대신 넣은 글자. 원래 문서에는 없어서 커서가 서는 칸이 아니다.
    // 줄 위치로는 바꾼 컨트롤의 칸(replacedUnits)만 차지한다.
    bool synthetic = false;
    int replacedUnits = 0;
    Control control;

    static InlineItem makeText(std::string t);
    static InlineItem makeControl(Control c);
};

// 같은 글자 모양이 적용된 연속 구간
struct Run {
    std::uint32_t charShapeId = 0;
    std::vector<InlineItem> items;
};

struct Paragraph {
    std::uint32_t paraShapeId = 0;
    std::uint32_t styleId = 0;
    bool pageBreak = false;
    bool columnBreak = false;   // 단 나누기. 같은 다단 안에서 다음 단으로 넘긴다
    bool layoutBreak = false;   // 다단 나누기. 이 문단부터 단 개수가 새로 시작된다
    std::vector<Run> runs;
    std::vector<LineSeg> lineSegs;
    std::vector<TextRange> ranges;
    std::vector<KeptRecord> kept;  // 문단 자식 중 아직 필드가 없는 레코드
    // 문단 고유 번호. 앞뒤에 문단이 생기거나 빠져도 그대로다. 파일에는 쓰지 않는다.
    // 0 이면 아직 없다. ensureParagraphIds 가 매긴다.
    std::uint32_t id = 0;

    // 컨트롤 내부를 제외한 문단 자체의 텍스트
    [[nodiscard]] std::string text() const;
};

// Paragraph 가 완전한 타입이 된 뒤에 정의해야 std::vector<Paragraph> 멤버를 안전하게 쓸 수 있다.
inline InlineItem InlineItem::makeText(std::string t) {
    InlineItem item;
    item.text = std::move(t);
    return item;
}

inline InlineItem InlineItem::makeControl(Control c) {
    InlineItem item;
    item.type = Type::Control;
    item.control = std::move(c);
    return item;
}

// 바탕쪽. 화면에서는 본문 뒤에 깔린다. applyPageType 은 BOTH, EVEN, ODD, LAST_PAGE(구역 마지막 쪽),
// OPTIONAL_PAGE(구역 안 pageNumber 번째 쪽, 1 부터).
// printOnTop 은 한글의 "인쇄 시 맨 위"다. 켜지면 인쇄할 때 본문보다 위에 그린다.
// duplicate 는 "기존 바탕쪽과 겹치게 하기"다. 마지막 쪽·임의 쪽 바탕쪽이 켜면 그 쪽의 양쪽·홀수·짝수 바탕쪽 위에 겹친다.
// 쪽마다 고르는 규칙은 숨긴 한글 2024 의 PDF 로 확인했다(scene.cpp masterPagesFor).
struct MasterPage {
    std::string applyPageType = "BOTH";
    std::uint32_t pageNumber = 0;
    bool duplicate = false;
    bool printOnTop = false;
    SubList content;
};

struct Section {
    std::optional<PageDef> page;
    std::int32_t defaultTab = 4000;  // 탭 위치가 없을 때의 간격. HWPUNIT
    std::int32_t columnRegionGap = 1134;  // 단 설정 나누기 뒤의 간격. 기본 4mm
    // 글자 방향. 0 가로, 1 세로(영문 눕힘), 2 세로(영문 세움). 한글 2024 의 값과 같다(HWPX HORIZONTAL·VERTICAL·VERTICALALL).
    std::uint8_t textDirection = 0;
    // HWP 구역 정의 속성 중 모델이 풀지 않는 비트(Control::sectionAttrKeep). 없으면 비운다.
    std::optional<std::uint32_t> sectionAttrKeep;
    // 줄 격자 간격. 0 이면 끈다. 켜면 줄은 격자 칸의 정수배를 차지하고 그 칸 가운데에 선다.
    std::int32_t lineGrid = 0;
    // 글자 격자 간격. 0 이면 끈다. 켜면 글자마다 칸 하나에 가운데 맞춰 놓는다.
    std::int32_t charGrid = 0;
    bool wongoji = false;  // 원고지 정서법. 반각 둘을 한 칸에, 줄 앞 빈칸을 뺀다
    std::uint32_t pageStart = 0;     // 0이면 앞 구역 다음 번호. 양수면 이 구역 첫 쪽 번호
    bool hideHeader = false;
    bool hideFooter = false;
    bool hideMaster = false;
    bool hidePageNum = false;
    bool hideFirstHeader = false;
    bool hideFirstFooter = false;
    bool hideFirstMaster = false;
    bool hideFirstPageNum = false;
    // 구역 설정(한글 2024 구역 설정 대화 상자). 시작 쪽 번호 종류: 0 이어서, 1 짝수, 2 홀수, 3 사용자(쪽 번호 값).
    std::uint8_t startsOn = 0;
    std::uint16_t pictureStart = 0;   // 그림 시작 번호. 0 은 이어서
    std::uint16_t tableStart = 0;     // 표 시작 번호
    std::uint16_t equationStart = 0;  // 수식 시작 번호
    std::uint16_t memoShapeId = 0;    // 구역 메모 모양(1 부터, 0 은 기본 모양)
    // 쪽 테두리·배경을 보일 쪽: SHOW_ALL, HIDE_FIRST(첫 쪽만 감춤), SHOW_FIRST(첫 쪽만 보임).
    std::string borderShow = "SHOW_ALL";
    std::string fillShow = "SHOW_ALL";
    bool hideFirstEmptyLine = false;  // 빈 줄 감추기
    LineNumberShape lineNumbers;
    std::vector<MasterPage> masterPages;
    NoteShape footnote;
    NoteShape endnote;
    std::array<PageBorderFill, 3> pageBorders = defaultPageBorders();
    std::vector<Paragraph> paragraphs;

    bool vertical() const { return textDirection != 0; }
    // 본문을 짜는 틀. 그 쪽 번호의 여백이고, 세로쓰기면 돌린 틀이다(PageDef::turned).
    PageDef flowPage(int pageNumber) const {
        const PageDef plain = page.value_or(PageDef{});
        return vertical() ? plain.turnedSide(pageNumber) : plain.side(pageNumber);
    }
};

struct Document {
    FormatInfo format;
    Metadata metadata;
    DocHeader header;
    std::vector<Section> sections;
    std::string previewText;
};

struct DocumentStats {
    std::size_t sections = 0;
    std::size_t paragraphs = 0;  // 셀/글상자 등 중첩 포함
    std::size_t characters = 0;  // UTF-8 코드 포인트 수
    std::size_t tables = 0;
    std::size_t tableCells = 0;
    std::size_t pictures = 0;
    std::size_t shapes = 0;
    std::size_t equations = 0;
    std::size_t headersFooters = 0;
    std::size_t notes = 0;
    std::size_t otherControls = 0;
};

DocumentStats computeStats(const Document& doc);

// 번호가 없거나(0) 문서 차례로 앞 문단과 겹치는 문단에 새 번호를 준다. 문단을 나누면 뒤 문단이 앞 문단을
// 복사해 번호까지 같아지므로, 먼저 나온 쪽이 번호를 지키고 뒤쪽이 새 번호를 받는다. 번호는 프로그램 전체에서
// 한 번만 쓴다(되돌리기로 옛 문단이 돌아와도 다른 문단과 겹치지 않는다). 새로 준 문단 수를 돌려준다.
int ensureParagraphIds(Document& doc);
// 문서의 모든 문단(본문, 바탕쪽, 머리말·각주·글상자, 표 칸, 캡션)을 문서 차례로 돈다.
void forEachParagraph(Document& doc, const std::function<void(Paragraph&)>& fn);

// samples/빈 문서.hwpx 와 같은 빈 문서. 용지 세로는 84186 이다.
Document blankDocument();

// 글자 모양이 가리키는 글꼴 표의 이름을 바꾼다. 스타일의 글꼴 번호는 그대로다.
int replaceFontFace(Document& doc, const std::string& from, const std::string& to);

// ---------------------------------------------------------------------------
// 편집용 Story 매핑 및 커서 역추적 (story ID <-> std::vector<Paragraph>& / Paragraph*)
// ---------------------------------------------------------------------------

enum class StoryKind : std::uint8_t {
    None,
    Section,     // 구역 본문 (Section::paragraphs)
    TableCell,   // 표 셀 (TableCell::content.paragraphs)
    SubList,     // 글상자·머리말·꼬리말·각주·미주 등 (Control::subLists[i].paragraphs)
    Caption,     // 개체 캡션 (Caption::content.paragraphs)
    MasterPage   // 바탕쪽 (MasterPage::content.paragraphs)
};
const char* storyKindName(StoryKind kind);

// story 번호 하나가 가리키는 문서 안 실제 문단 컨테이너와 소유자 정보.
struct StorySlot {
    int story = -1;
    StoryKind kind = StoryKind::None;
    std::vector<Paragraph>* paragraphs = nullptr;
    int sectionIndex = -1;
    int parentStory = -1;       // 이 개체를 담고 있는 바깥 story 번호 (구역 본문·바탕쪽이면 -1)
    int parentParagraph = -1;   // 바깥 story 안에서 이 개체가 들어 있는 문단 번호
    Control* control = nullptr; // 표, 글상자, 머리말/꼬리말, 각주/미주, 캡션의 소유 Control
    TableCell* tableCell = nullptr;
    SubList* subList = nullptr;
    Caption* caption = nullptr;
    MasterPage* masterPage = nullptr;
    int subIndex = -1;          // cells[i], subLists[i], masterPages[i] 안에서의 인덱스
};

// 편집 커서의 논리 좌표 (story 번호, 그 안의 문단 번호, 문단 안 글자 오프셋).
struct CursorPos {
    int story = -1;
    int paragraph = -1;
    int offset = -1;
};

// CursorPos 를 실제 수정 가능한 문서 모델 포인터로 역추적한 결과.
struct CursorTarget {
    StorySlot* slot = nullptr;
    std::vector<Paragraph>* paragraphs = nullptr;
    Paragraph* paragraph = nullptr;
    int story = -1;
    int paragraphIndex = -1;
    int offset = -1;

    [[nodiscard]] bool valid() const { return paragraphs != nullptr && paragraph != nullptr; }
};

// story 번호만으로 Document 안의 std::vector<Paragraph>& 와 Paragraph* 를 O(1)에 찾아가는 매핑 테이블.
class StoryMap {
public:
    StoryMap() = default;
    explicit StoryMap(Document& doc) { build(doc); }
    explicit StoryMap(const Document& doc) { build(doc); }

    void clear();
    void build(Document& doc);
    void build(const Document& doc);

    [[nodiscard]] bool matches(const Document& doc) const { return doc_ == &doc && slots_.size() > 1; }
    [[nodiscard]] int count() const { return slots_.empty() ? 0 : static_cast<int>(slots_.size()) - 1; }
    [[nodiscard]] bool valid(int story) const {
        return story > 0 && story < static_cast<int>(slots_.size()) &&
               slots_[static_cast<std::size_t>(story)].paragraphs != nullptr;
    }

    // O(1) 조회: story 번호 -> StorySlot / std::vector<Paragraph>* / Paragraph*
    [[nodiscard]] const StorySlot* slot(int story) const;
    [[nodiscard]] StorySlot* slot(int story);
    [[nodiscard]] const std::vector<Paragraph>* paragraphs(int story) const;
    [[nodiscard]] std::vector<Paragraph>* paragraphs(int story);
    [[nodiscard]] const Paragraph* paragraph(int story, int paragraphIndex) const;
    [[nodiscard]] Paragraph* paragraph(int story, int paragraphIndex);
    [[nodiscard]] CursorTarget resolve(CursorPos pos);

    // O(1) 역조회: &paragraphs 포인터 -> 기본 story 번호 (없으면 -1)
    [[nodiscard]] int storyOf(const std::vector<Paragraph>* paragraphs) const;

    // 머리말·꼬리말·바탕쪽·반복 제목줄처럼 여러 쪽에 복제되어 찍히는 요소에
    // 화면 블록 선택 범위만 분리된 별칭 story 번호를 부여하되,
    // 수정 대상(StorySlot)은 원본 std::vector<Paragraph>* 를 그대로 가리키게 한다.
    int aliasStory(int baseStory);

    // 문서 모델에 없는 합성 줄(예: 쪽 번호 표시)에 빈 story 번호를 배정한다.
    int reserveSynthetic();

    // 조판 중 번호 치환 등으로 만든 임시 문단 배열 포인터를 별칭 story 에 잠시 연결·해제한다.
    void bindTempPointer(const std::vector<Paragraph>* tempPtr, int story);
    void unbindTempPointer(const std::vector<Paragraph>* tempPtr);

    // 중첩된 표·글상자·캡션이 속한 최상위 구역 본문 story 번호와 그 구역 안의 문단 번호를 찾는다.
    [[nodiscard]] int rootSectionStory(int story) const;
    [[nodiscard]] int rootSectionParagraph(int story) const;

    // O(1) 조회: 문단 고유 번호(Paragraph::id) -> 그 문단의 글 묶음 번호와 문단 번호. 없으면 둘 다 -1.
    // 문단이 늘거나 줄면 번호가 밀리므로, 편집 뒤 다시 짓는(rebuild) 지도에서만 맞다.
    struct ParagraphAt {
        int story = -1;
        int paragraph = -1;
    };
    [[nodiscard]] ParagraphAt findParagraph(std::uint32_t id) const;

private:
    const Document* doc_ = nullptr;
    std::vector<StorySlot> slots_;
    std::unordered_map<const std::vector<Paragraph>*, int> byPtr_;
    std::unordered_map<std::uint32_t, ParagraphAt> byId_;
};

}  // namespace chwp::model
