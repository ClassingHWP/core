#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "noting/common/bytes.hpp"
#include "noting/model/document.hpp"
#include "noting/render/scene.hpp"
#include "noting/render/view.hpp"

namespace noting::render {

// 한 쪽을 래스터로 그린 PNG. 뷰어와 편집이 같은 그림을 쓴다.
struct PageImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> png;
};

// PNG로 압축하기 전의 픽셀. RGBA, 위에서 아래로.
struct PageBitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    // 그림을 아직 못 읽었다. 이 칸을 남기면 회색이 고정된다.
    bool incomplete = false;
};

// 한 칸을 층으로 나눈다. 글이 먼저 나오고, 그림은 그 다음에 올린다.
// 글 뒤(0) → 글(1) → 글 앞(2) → 테두리(3). All 은 층을 나누지 않고 한 장으로 그린다.
enum class PaintLayer : int { Behind = 0, Text = 1, Front = 2, Border = 3, All = -1 };
inline constexpr int kPaintLayerCount = 4;

// pixelHeight 는 결과 그림의 세로 픽셀. 가로는 종이 비율에 맞춘다.
PageImage renderPagePng(const model::Document& doc, int pageIndex, int pixelHeight);
// 이미 배치한 쪽만 그린다. 쪽을 넘길 때 문서 전체를 다시 배치하지 않기 위함이다.
PageImage renderPagePng(const model::Document& doc, const Node& page, int pixelHeight);
PageBitmap renderPageBitmap(const model::Document& doc, const Node& page, int pixelHeight);
// view 는 pixelHeight 로 맞춘 쪽 전체 픽셀 좌표. 가로·세로가 0 이면 쪽 전체다.
PageBitmap renderPageBitmap(const model::Document& doc, const Node& page, int pixelHeight, int viewX,
                            int viewY, int viewW, int viewH, PaintLayer layer = PaintLayer::All,
                            int pageIndex = -1);

// 화면에 올릴 줄 또는 개체 하나. 좌표는 그 쪽의 픽셀이고, 여백 패드를 포함한다.
// kind: 0 줄, 1 개체, 2 칸. 100~103은 문단 테두리(100 + 연결로 생략할 변). path 는 쪽 루트에서 이 노드까지의 자식 번호다.
struct FrameDesc {
    int id = 0;
    int page = 0;
    int layer = 0;
    int rank = 0;
    // 쪽 zOrder와 묶음 안 순서를 한 값으로 잇는다. 자식 묶음이 다음 형제보다 위로 새지 않게 칸을 넓혀 둔다.
    std::int64_t z = 0;
    int kind = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int pixelHeight = 0;
    int percent = 0;
    bool inObject = false;
    bool baseOnly = false;
    std::int32_t huX = 0;
    std::int32_t huY = 0;
    std::int32_t huW = 0;
    std::int32_t huH = 0;
    std::int32_t originX = 0;
    std::int32_t originY = 0;
    std::uint64_t contentHash = 0;
    int lineStory = -1;
    int lineParagraph = -1;
    int lineStart = -1;
    std::vector<int> path;
};

// 칸 테두리 여백. 픽셀로 고정이라 배율에 따라 커지지 않는다.
constexpr int kFramePad = 8;

// 다른 배율로 그린 칸을 지금 배율의 내용 상자에 맞춘다. 여백까지 같이 늘리지 않는다.
void placeZoomedFrame(int dstX, int dstY, int dstPixelHeight, int srcPixelHeight, int srcW, int srcH, int& x, int& y,
                      int& w, int& h);

// 쪽 왼쪽 위 기준 HU. on 이면 이 사각형과 겹치지 않는 줄·개체는 칸으로 만들지 않는다.
struct FrameCull {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
    bool on = false;
};

std::vector<FrameDesc> collectFrames(const Node& page, int pageIndex, int pixelHeight, const FrameCull& cull = {},
                                   const model::Document* doc = nullptr);
bool canBakeFrame(const FrameDesc& frame);
PageBitmap renderFrame(const model::Document& doc, const Node& page, const FrameDesc& frame);

// 그 쪽에 실제로 찍히는 글꼴. 굵은 글이면 굵은 파일 이름도 넣는다. 안 쓰는 글꼴은 넣지 않는다.
std::vector<std::string> pageFaceNames(const Node& page);
struct GlyphWarm {
    int faces = 0;
    int glyphs = 0;
};
// 그 쪽에 찍히는 글자의 윤곽만 메모리에 둔다. 글꼴 파일 전체는 올리지 않는다.
GlyphWarm warmPageGlyphs(const Node& page, int pageIndex, int percent);
// 문서의 런과 글자를 한 번 훑어 전진폭을 남긴다. 같은 글자 모양은 다시 재지 않는다.
void noteDocumentGlyphs(const model::Document& doc);
struct MaskBake {
    int made = 0;
    bool more = false;
};
// 모아 둔 글자를 기준(100%·75%·50%) 및 targetMaskZoom 마스크로 조금 찍는다. 다 찍히면 more 가 거짓이다.
MaskBake bakeDocumentMasks(const model::Document& doc, int budget, int targetMaskZoom = 0);
// 문서에 쓰인 글꼴과 그 글자의 윤곽을 한 번만 올린다. 쪽마다 다시 세지 않는다.
std::vector<std::string> documentFaceNames(const std::vector<Node>& pages);
GlyphWarm warmDocumentGlyphs(const std::vector<Node>& pages, int percent);
// 줄 캐시. 글자 윤곽은 넣지 않고, 어느 글자가 어디에 어떤 크기로 있는지만 남긴다.
int cachePageLines(const model::Document& doc, const Node& page, int pageIndex, int percent);
// 윤곽은 굽지 않고, 이 줄의 전진폭과 글자 자리만 남긴다.
void writeLinePlace(const model::Document& doc, const Node& page, const FrameDesc& frame, int pixelH);
// 이 줄의 글자 자리를 이미 적어 두었으면 참이다.
bool linePlaceKept(const FrameDesc& frame);
struct LineNote {
    int made = 0;
    bool more = false;
};
// 유휴가 줄마다 전진폭과 글자 자리를 적는다. 스레드 수만큼 나누어 적고, 남으면 more 다.
LineNote cacheIdleLines(const model::Document& doc, std::vector<Node>& pages, int origin, int budget);
// 문서를 다시 열면 이전 문서의 글자 윤곽과 줄 자리를 버린다.
void clearGlyphWarm();
// 문서를 바꿀 때 디코딩해 둔 그림을 버린다.
void clearPageRenderCache();
// 교체/삭제될 서브트리의 Span 측정 캐시만 선택적으로 지운다.
void evictNodeMeasures(const Node& root);
// 쪽 트리를 통째로 새로 만들 때 부른다. 노드 주소를 열쇠로 쓰는 캐럿·블록·탭 캐시를 비운다.
void forgetLayoutNodeCaches();
// 글자만 바뀐 줄. 그림 개체 안은 건드리지 않는다.
void evictLineGlyph(const Node& line, const Node& span);
// 부분 무효화 범위에 해당하는 줄 배치 캐시(linePlaces)만 선택적으로 지운다.
void invalidateLinePlaces(const std::vector<FrameInvalidation>& invalidations, bool fullInvalidation);
// 그림 바이트가 문서에 없으면 그릴 때 이 파일에서 해당 항목만 읽는다. 빈 뷰는 캐시를 놓는다.
void setPictureFile(ByteView file);
// 칸을 굽기 전에 그림 한 장을 풀어 둔다. 이미 풀렸으면 바로 돌아온다.
void warmPicture(const model::Document& doc, const std::string& id);

// 종이 왼쪽 위 기준. 커서는 그 줄의 글자 높이다. 글자가 없는 곳이면 hit 가 아니다.
// story 는 블록 범위다. 같은 섹션 본문끼리, 또는 같은 개체 안 문단끼리만 같다.
struct CaretPlace {
    bool hit = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t height = 0;
    int page = -1;
    int story = -1;
    int paragraph = -1;
    int offset = -1;
    std::uint32_t styleId = 0;
    std::uint32_t paraShapeId = 0;
    std::uint32_t charShapeId = 0;
    // 커서 바로 앞·뒤 글자. 띄어쓰기와 탭도 경계에 들어 있다.
    bool hasBefore = false;
    bool hasAfter = false;
    char32_t before = 0;
    char32_t after = 0;
};

// 현재 커서 위치의 스타일·문단 속성·글자 모양.
struct CursorStyle {
    bool active = false;
    std::uint32_t styleId = 0;
    std::string styleName;
    std::string styleEngName;
    std::string styleType = "PARA";
    std::uint32_t paraShapeId = 0;
    std::string align = "JUSTIFY";
    std::string lineSpacingType = "PERCENT";
    std::int32_t lineSpacing = 160;
    std::uint32_t charShapeId = 0;
    std::string font;
    // font 가 라이선스로 막힌 글꼴이면 편집할 때 바뀔 글꼴 이름. 아니면 비어 있다.
    std::string fontSwapTo;
    std::int32_t fontSize = 1000;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    std::string underlineType = "NONE";
    bool strikeout = false;
    std::string color = "#000000";
};

CursorStyle cursorStyle(const model::Document& doc, const std::vector<Node>& pages);
CursorStyle cursorStyleForCaret(const model::Document& doc, const CaretPlace& caret);

// 상태 줄에 보이는 캐럿 자리. 한글처럼 1부터 센다.
// 줄은 캐럿이 든 단(표 칸·글상자 안이면 그 칸)의 위에서부터, 칸은 그 줄의 첫 글자부터다.
struct CaretStatus {
    bool active = false;
    int section = 0;
    int sectionCount = 0;
    int column = 0;
    int line = 0;
    int character = 0;
    // 머리말·꼬리말 편집 중이면 1(머리말)·2(꼬리말), 본문이면 0.
    int band = 0;
};

CaretStatus caretStatus(const model::Document& doc, const std::vector<Node>& pages);

// 머리말·꼬리말을 고치는 동안 흐리게 덮을 쪽과, 그 안에서 비워 둘 띠 칸(HWPUNIT).
// 덮지 않는 쪽이면 veiled 가 false 고, 띠가 없는 쪽이면 칸 너비가 0 이라 쪽 전체를 덮는다.
struct BandVeil {
    bool veiled = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
};

BandVeil bandVeil(int page);

// 블록으로 칠할 칸. 좌표는 그 쪽의 종이 왼쪽 위 기준이다.
// 세로는 글자 상자에 그 줄의 줄 간격을 더한 값이다. 다음 줄 글자는 이 칸에 넣지 않는다.
// back 은 그 칸의 배경색이다. 형광펜, 글자 배경, 칸 채우기, 종이 순으로 고른다.
struct BlockBox {
    int page = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint8_t backR = 255;
    std::uint8_t backG = 255;
    std::uint8_t backB = 255;
};

CaretPlace caretOnPage(const model::Document& doc, const Node& page, std::int32_t x, std::int32_t y);
// 포인터가 다른 글 묶음 위에 있어도 story 안의 가장 가까운 글자 경계로 붙인다.
// drag 면 글자의 60%를 넘어야 그 글자를 넘어간 것으로 본다. 처음 클릭은 한가운데다.
CaretPlace caretInStory(const model::Document& doc, const std::vector<Node>& pages, int page, std::int32_t x,
                        std::int32_t y, int story, bool drag = false, int anchorParagraph = -1,
                        int anchorOffset = -1);
// 두 위치 사이에 있는 story 의 줄만 돌려준다. 순서는 앞뒤를 가리지 않는다.
std::vector<BlockBox> blockBoxes(const model::Document& doc, const std::vector<Node>& pages, int story, int paragraphA,
                                 int offsetA, int paragraphB, int offsetB);

// 포인터가 고른 구간. 좌표는 그 쪽의 HWPUNIT. invert 면 GPU 가 종이색을 뒤집은 배경을 글자 뒤에 칠하고, 그 위 글자색도 뒤집는다.
struct SelectionMark {
    int page = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool invert = false;
};

// 셸에 알릴 커서 유무. 구간을 어디부터 어디까지인지는 코어가 갖고, 그림은 GPU 가 그린다.
struct PointerView {
    bool active = false;
    int story = 0;
    bool caret = false;
};

// 포인터 시각과 화면 배율. time 이 0 이면 클릭 횟수를 세지 않는다.
// 좌표 viewX, viewY 는 뷰포트 장치 픽셀이다. huPerLogical 은 논리 픽셀 하나가 쪽 좌표로 몇인지다.
struct PointerSample {
    int id = 0;
    int device = 0;
    double time = 0;
    double dpr = 1;
    double viewX = 0;
    double viewY = 0;
    double huPerLogical = 0;
};

// kind: 0 배율, 1 누름, 2 누른 채 이동, 3 뗌, 4 해제.
PointerView applyPointer(const model::Document& doc, const std::vector<Node>& pages, int kind, int page, int x, int y,
                         int percent, PointerSample sample = {});
// 블록 양 끝 마커. 켜면 마우스·터치 블록 모두 끝에 마커가 나오고 끌어서 넓히거나 좁힌다. 끄면 나오지 않는다.
void setSelectionPins(bool on);
bool selectionPins();
// (page, x, y)가 마커 위면 true. 셸이 터치를 스크롤 대신 마커 끌기로 보낼 때 쓴다. radius 는 HWPUNIT 이다.
bool pinUnder(int page, int x, int y, double radius);
// 마커를 끌지 않고 눌렀다 뗐으면 한 번만 true 다. 셸은 이때 복사·잘라내기 메뉴를 띄운다.
bool takePinTap();
// 마우스가 올라간 자리의 CSS 커서 이름. 개체 손잡이면 크기 화살표, 글줄 위면 text, 나머지는 default 다.
// 읽기 전용이면 손잡이 화살표를 내지 않는다. handleRadius 는 HWPUNIT 이다.
std::string hoverCursor(const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y,
                        double handleRadius);
// 터치의 두 번째 누르기면 true. 시계는 바꾸지 않는다.
bool repeatedTouchDown(double viewX, double viewY, double timeMs, double dpr, int device, int id);
// 블록을 끄는 동안 화면 가장자리면 스크롤할 장치 픽셀. 없으면 false.
bool selectionEdgeScroll(double dtMs, int viewW, int viewH, int& dx, int& dy);
void selectionEdgePoint(double& viewX, double& viewY);
std::string pointerPropertyText();
// 개체 크기 조절을 문서에 반영한다. 바뀐 것이 있으면 true.
bool commitObjectResize(model::Document& doc);
// 처음 크기는 두고 배율만 새 상자에 맞춘다. 배율 1이면 지금 크기가 처음 크기다.
void applyObjectResize(model::Placement& place, std::uint32_t newWidth, std::uint32_t newHeight);

struct ResizeCommitInfo {
    int page = -1;
    std::string bin;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

ResizeCommitInfo resizeCommitInfo();
// 크기를 바꾼 그림을 다시 고른다. 좌표는 글자처럼 넣은 개체의 그린 자리다.
bool restoreResizedObject(const model::Document& doc, const std::vector<Node>& pages, int pageHint,
                          const std::string& bin, int width, int height);
// 이번 포인터에서 디버그 로그에 남길 문장. 좌표와 구간은 코어가 정한 값이다. 없으면 빈 문자열.
std::string pointerEventText();
// 커서·블록이 바뀌면 늘어난다. 화면 루프가 다시 그릴지 볼 때 쓴다.
int pointerPicture();
// 깜빡일 커서가 있으면 true다.
bool caretShown();
// 커서·조합 상자의 현재 깜빡임 주기(0: 켜짐, 1: 꺼짐, -1: 없음).
int caretBlinkPhase();
// 포인터 드래그(블록 선택) 중이면 true다.
bool pointerTracking();
// 고른 구간의 글자. 문단 사이는 줄바꿈이고, 커서만 있으면 빈 문자열이다.
std::string selectionText(const model::Document& doc, const std::vector<Node>& pages);
void clearPointerSelection();
std::vector<SelectionMark> selectionMarks();

// 현재 포인터/캐럿이 가리키는 편집 커서와 블록 선택 상태.
struct PointerCursor {
    bool active = false;
    bool caret = false;
    bool hasSelection = false;
    int page = -1;
    int story = -1;
    int paragraph = -1;
    int offset = -1;
    int anchorParagraph = -1;
    int anchorOffset = -1;
    int focusParagraph = -1;
    int focusOffset = -1;
    int startParagraph = -1;
    int startOffset = -1;
    int endParagraph = -1;
    int endOffset = -1;
};

PointerCursor pointerCursor();
// 현재 포인터 커서를 Document 안의 실제 std::vector<Paragraph>* 와 Paragraph* 로 O(1) 역추적한다.
model::CursorTarget resolvePointerCursor(model::Document& doc);
// 논리 커서 좌표(story, paragraph, offset)에 해당하는 화면 CaretPlace 를 찾는다.
CaretPlace caretForCursor(const model::Document& doc, const std::vector<Node>& pages, model::CursorPos pos);
// 논리 커서/선택 구간으로 현재 포인터 캐럿과 블록 하이라이트를 갱신한다.
PointerView setPointerCursor(const model::Document& doc, const std::vector<Node>& pages, model::CursorPos anchor,
                             model::CursorPos focus);
inline PointerView setPointerCursor(const model::Document& doc, const std::vector<Node>& pages, model::CursorPos pos) {
    return setPointerCursor(doc, pages, pos, pos);
}

// 편집/이동 키 종류.
enum class EditKey : int {
    Backspace = 1,
    Delete = 2,
    Enter = 3,       // 문단 나눔 (Shift+Enter 는 Shift 플래그로 같은 문단 내 줄바꿈 '\n' 삽입)
    ArrowLeft = 4,
    ArrowRight = 5,
    ArrowUp = 6,
    ArrowDown = 7,
    Home = 8,
    End = 9,
    Tab = 10,
    Escape = 11,  // 머리말·꼬리말 편집에서 본문으로 나간다
};

// 읽기 전용 문서. 글자 넣기·지우기·문단 나누기·탭·서식·개체 크기 조절을 받지 않는다.
// 클릭·끌기 블록, 방향키·Home·End 이동, 블록 글자 읽기(복사)는 그대로다.
void setReadOnly(bool on);
bool readOnly();

// 편집 및 키 입력 처리 결과.
struct EditResult {
    bool handled = false;
    bool documentChanged = false;
    bool composing = false;
    int dirtySection = -1;
    int dirtyStory = -1;
    int dirtyParagraph = -1;
    bool fullInvalidation = false;
    bool pageListChanged = false;
    bool pagesMoved = false;
    int pageShiftFrom = -1;
    int pageShiftDelta = 0;
    std::vector<FrameInvalidation> invalidations;
    PointerCursor cursor;
    CaretPlace caret;
    std::vector<SelectionMark> compositionUnderlines;
    // 라이선스로 막힌 글꼴이 문서에 있어 편집을 받지 않았다. 셸은 안내한 뒤 editSwapLicenseFonts 를 부른다.
    bool needsFontSwap = false;
};

// 현재 커서 위치(또는 선택 구간 대체)에 UTF-8 텍스트를 삽입하고 수정된 문단을 재조판한다.
// composing 이 true 이면 한글 IME 조합 중 상태로 기록하고 조합 글자 박스를 반전 표시한다.
EditResult editInsertText(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                          bool composing = false, bool useStoredLineHeight = false);

// 문서의 막힌 글꼴(layout::licenseFontSwaps)을 한꺼번에 바꾸고 문서 전체를 다시 조판한다.
// 저장된 줄 정보는 옛 글꼴로 잰 것이라 지운다. 줄 간격과 쪽 나눔이 달라질 수 있다.
// chosen 은 셸에서 고른 {막힌 글꼴, 바꿀 글꼴}이다. 고를 수 있는 글꼴이 아니거나 없는 이름은 기본 짝을 쓴다.
EditResult editSwapLicenseFonts(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight = false,
                                const std::vector<std::pair<std::string, std::string>>& chosen = {});

// 진행 중인 한글 IME 조합을 확정한다(조합 반전 해제).
EditResult editCommitComposition(model::Document& doc, const std::vector<Node>& pages);

// 키보드 편집/이동 키(Backspace, Delete, Enter, Shift+Enter, 방향키, Home, End, Tab)를 처리한다.
EditResult editKeyDown(model::Document& doc, std::vector<Node>& pages, EditKey key, bool shift = false,
                       bool ctrl = false, bool alt = false, bool useStoredLineHeight = false);

// 현재 커서/선택 영역에 적용할 스타일·문단 속성·글자 모양 변경 패치.
// 글자 모양·문단 모양 대화 상자의 값 하나. 숫자는 double 로 오가고, 색은 #rrggbb(빈 문자열은 색 없음)다.
using FormatValue = std::variant<bool, double, std::string>;
using FormatFields = std::vector<std::pair<std::string, FormatValue>>;

struct FormatPatch {
    bool hasStyleId = false;
    std::uint32_t styleId = 0;

    bool hasAlign = false;
    std::string align;
    bool hasLineSpacingType = false;
    std::string lineSpacingType;
    bool hasLineSpacing = false;
    std::int32_t lineSpacing = 160;

    bool hasFont = false;
    std::string font;
    bool hasFontSize = false;
    std::int32_t fontSize = 1000;
    bool hasBold = false;
    bool bold = false;
    bool hasItalic = false;
    bool italic = false;
    bool hasUnderline = false;
    bool underline = false;
    bool hasUnderlineType = false;
    std::string underlineType;
    bool hasStrikeout = false;
    bool strikeout = false;
    bool hasColor = false;
    std::string color;

    // 위에 없는 나머지 글자 모양·문단 모양 값. 고친 값만 들어 있다. 이름과 단위는 setFormatField 가 정한다.
    FormatFields fields;
};

// 이름 하나를 패치에 넣는다. 모르는 이름이거나 값 꼴이 틀리면 false.
// 길이는 HWPUNIT(1pt = 100), 비율은 %, 글자 크기는 fontSize(HWPUNIT) 또는 fontSizePt 다.
bool setFormatField(FormatPatch& patch, const std::string& key, const FormatValue& value);

// 커서(블록이면 시작) 자리의 글자 모양·문단 모양을 setFormatField 와 같은 이름·단위로 돌려준다.
FormatFields formatFieldsAtCursor(const model::Document& doc, const std::vector<Node>& pages);

// 현재 커서 또는 블록 선택 구간에 스타일·문단 속성·글자 모양 변경을 적용하고 재조판한다.
EditResult editApplyFormat(model::Document& doc, std::vector<Node>& pages, const FormatPatch& patch,
                           bool useStoredLineHeight = false);

// 현재 한글 IME 조합 중 반전 박스 영역(종이 왼쪽 위 기준 HWPUNIT)을 돌려준다.
std::vector<SelectionMark> compositionUnderlineMarks();

// 블록과 커서 그림. 좌표는 그 쪽의 픽셀이고, 줄 그림에는 굽지 않는다.
// caret 가 참이면 깜빡이는 커서다. 블록은 그 칸의 색을 반전한 그림이다.
struct SelectionImage {
    int page = -1;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t pageW = 0;
    std::int32_t pageH = 0;
    bool caret = false;
    std::vector<std::uint8_t> rgba;
};

// percent 는 화면 배율이다. 블록이 없으면 커서만 돌려준다.
std::vector<SelectionImage> paintSelection(const model::Document& doc, const std::vector<Node>& pages,
                                           const std::vector<BlockBox>& boxes, const CaretPlace& caret, int caretPage,
                                           int percent);
// 화면 GPU 가 없을 때만 셸이 이 픽셀을 그린다. 있으면 present 가 invert 배경을 직접 칠한다.
std::vector<SelectionImage> pointerImages(const model::Document& doc, const std::vector<Node>& pages);

// 불투명 RGBA 의 빨강·초록·파랑만 뒤집는다. 알파는 그대로 둔다.
void invertOpaqueColors(std::vector<std::uint8_t>& rgba, int width, int height);

// 창에 바로 올릴 그림. rgba 가 없으면 흰 종이 사각형이다.
struct ScreenSprite {
    int id = 0;
    int stamp = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int sw = 0;
    int sh = 0;
    bool nearest = false;
    int percent = 0;
    int page = -1;
    int layer = 1;
    const std::uint8_t* rgba = nullptr;
};

// 페이지 GPU 합성 텍스처 캐시 대상 크기 한도.
inline constexpr int kMaxPageGpuCacheW = 2048;
inline constexpr int kMaxPageGpuCacheH = 2800;

// 화면 캔버스가 바뀌면 GPU 표면을 다시 연다.
bool resetScreenSurface();
// 웹에서는 브라우저 WebGPU 장치가 나중에 붙는다. 붙기 전에는 false다.
bool screenReady();
void dropScreenSprite(int id);
bool frameHasGlyphs(int id);
bool cloneAndAdjustFrameGlyphs(int srcFrameId, int dstFrameId, int newStory, int newParagraph, int deltaOffset,
                               std::int32_t deltaOriginX, std::int32_t deltaOriginY);
// 해당 페이지가 이미 유효한 GPU 합성 텍스처로 캐시되어 있어 개별 스프라이트 전달을 생략할 수 있는지 확인한다.
bool canUsePageGpuTexture(int page, int pageW, int pageH, int layoutZoom, int maskZoom, int pageStamp,
                          bool dragging = false);
// 배율은 다르지만 같은 내용(pageStamp)의 GPU 합성 텍스처가 캐시되어 있어 확대·축소 스케일링에 즉시 쓸 수 있는지 확인한다.
bool hasScalablePageGpuTexture(int page, int pageStamp);
// 그 쪽 GPU 합성 텍스처를 구운 배율. 없거나 내용이 다르면 0.
int pageGpuTextureZoom(int page, int pageStamp);
bool hasAnyPageGpuTexture(int page);
// 글자가 바뀐 쪽의 옛 합성 그림은 새 글자와 겹치므로 지운다.
void dropPageGpuCache(int page);
bool canBakePageGpuTexture(int page, int pageW, int pageH, int pageStamp);
void setRenderInteracting(bool interacting);
// 워커가 넘겨받은 창에 종이와 스프라이트를 그린다. 픽셀은 다시 읽지 않는다.
// layoutZoom 은 칸을 만든 배율, maskZoom 은 글자 마스크를 찍는 배율이다. 둘이 다르면 있는 마스크를 칸에 맞춰 늘린다.
// pages 와 스크롤은 쪽 위치다. 블록 배경은 코어가 여기 맞춰 글자 뒤에 칠한다.
// 스크롤 막대도 이 캔버스에 그린다. ruler 가 0 보다 크면 위쪽·왼쪽 눈금자를 그 두께로 그린다.
bool presentScreen(int viewW, int viewH, const std::vector<ScreenSprite>& papers, const std::vector<ScreenSprite>& sprites,
                   int layoutZoom = 0, int maskZoom = 0, const std::vector<ViewPage>& pages = {}, int scrollX = 0,
                   int scrollY = 0, const Scrollbar& barX = {}, const Scrollbar& barY = {}, bool dragging = false,
                   int ruler = 0, double dpr = 1);
// 이번 화면에 아직 없는 글자 크기 마스크가 남아 있으면 true다.
bool glyphMasksPending();
// 프레임당 한도 때문에 아직 거친 단계로 그린 그림이 있으면 true다. 화면은 다음 프레임을 그린다.
bool pictureUploadsPending();

}  // namespace noting::render
