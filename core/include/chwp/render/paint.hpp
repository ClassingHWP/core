#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "chwp/common/bytes.hpp"
#include "chwp/model/document.hpp"
#include "chwp/render/scene.hpp"
#include "chwp/render/screen_ops.hpp"
#include "chwp/render/view.hpp"

namespace chwp::render {

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

class FrameCache;
// 쪽 하나를 화면과 같은 GPU(Skia Graphite) 굽기로 그린다. 칸은 frames 에 굽고(화면이면 화면의 칸 캐시),
// 바탕 비트맵·그림·테두리 셰이더·글자 쿼드를 GPU 에서 합친 뒤 읽어 온다.
// GPU 장치를 못 얻었거나 pageImageGpu() 가 꺼져 있으면 빈 그림이다. 부른 쪽은 그때 CPU(renderPageBitmap)로 그린다.
PageBitmap renderPageGpu(const model::Document& doc, std::vector<Node>& pages, int pageIndex, int pixelHeight,
                         FrameCache& frames);
// 쪽 그림을 GPU 로 먼저 그릴지. 기본은 켬. 픽셀까지 같아야 하는 시험 기준선은 끄고 CPU 로 그린다.
void setPageImageGpu(bool on);
bool pageImageGpu();
// 픽셀을 PNG 로 묶는다.
PageImage encodePageImage(const PageBitmap& bitmap);
// view 는 pixelHeight 로 맞춘 쪽 전체 픽셀 좌표. 가로·세로가 0 이면 쪽 전체다.
// background 는 층을 나누지 않을 때의 바탕 밝기(0 검정 ~ 255 흰색)다.
PageBitmap renderPageBitmap(const model::Document& doc, const Node& page, int pixelHeight, int viewX,
                            int viewY, int viewW, int viewH, PaintLayer layer = PaintLayer::All,
                            int pageIndex = -1, std::uint8_t background = 255);

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
    // 세로쓰기 본문 칸. x·y·w·h 는 종이 픽셀이고, huX·huY·huW·huH·originX·originY 는 돌린 틀의 HU 다.
    // 칸은 돌린 틀로 그린 뒤 시계 방향으로 돌려 굽는다.
    bool turned = false;
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
// 100% 넘게 확대해 굽는 개체 칸의 한 변·넓이 한도. 0 이면 한도가 없다. 메모리가 좁은 플랫폼이 Host 로 넘긴다.
void setZoomedFrameLimit(int side, std::int64_t pixels);
PageBitmap renderFrame(const model::Document& doc, const Node& page, const FrameDesc& frame);

// 문서의 런과 글자를 한 번 훑어 전진폭을 남긴다. 같은 글자 모양은 다시 재지 않는다.
void noteDocumentGlyphs(const model::Document& doc);
struct MaskBake {
    int made = 0;
    bool more = false;
};
// 모아 둔 글자를 기준(100%·75%·50%) 및 targetMaskZoom 마스크로 조금 찍는다. 다 찍히면 more 가 거짓이다.
MaskBake bakeDocumentMasks(const model::Document& doc, int budget, int targetMaskZoom = 0);
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
// 문서를 다시 열면 이전 문서의 글자 윤곽과 줄 자리를 버린다. 글꼴이 새로 들어왔을 때도 부른다:
// 없어서 대체 글꼴로 열어 둔 글꼴을 잊어 다음에 다시 연다.
void clearGlyphWarm();
// 문서를 바꿀 때 디코딩해 둔 그림을 버린다.
void clearPageRenderCache();
// 교체/삭제될 서브트리의 줄 캐럿 캐시만 선택적으로 지운다.
void evictNodeMeasures(const Node& root);
// 쪽 트리를 통째로 새로 만들 때 부른다. 노드 주소를 열쇠로 쓰는 캐럿·블록 캐시를 비운다.
void forgetLayoutNodeCaches();
// 글자만 바뀐 줄. 그림 개체 안은 건드리지 않는다.
void evictLineGlyph(const Node& line);

// 글꼴 파일 접기. 글꼴 파일이 쓰는 메모리(푼 TTF + 압축본)가 예산(캐시 이름 "fonts")을 넘으면,
// 문서가 그리는 글자를 다 뽑아 둔 뒤 오래 안 쓴 파일부터 압축본만 남기고 접는다.
// 최근 kFontRecentMs 안에 쓴 글꼴과 커서 글자 모양의 글꼴은 접지 않는다. 접은 글꼴은 새 글자가 필요할 때 다시 연다.
inline constexpr std::size_t kFontFileBudget = 64u * 1024u * 1024u;
inline constexpr std::int64_t kFontRecentMs = 20000;
// 새 문서를 열 때 부른다. 글자 뽑기를 처음부터 다시 한다.
void resetFontFold();
// 쉬는 프레임에서 부른다. budgetMs 안에서 글자를 조금씩 뽑고, 다 뽑았으면 예산을 넘은 만큼 접는다.
// cursorCharShape 는 실제로 접을 때만 부른다.
void foldIdleFonts(const model::Document& doc, const std::function<std::uint32_t()>& cursorCharShape, double budgetMs);
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
    // 세로쓰기 본문의 커서. (x, y)에서 오른쪽으로 height 만큼 누운 막대다. 아니면 아래로 선 막대다.
    bool turned = false;
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
    // 보이는 줄 간격: 퍼센트면 % 값, 아니면 pt 값과 그 단위.
    double lineSpacingValue() const;
    const char* lineSpacingUnit() const;
    std::uint32_t charShapeId = 0;
    std::string font;
    // font 가 라이선스로 막힌 글꼴이면 편집할 때 바뀔 글꼴 이름. 아니면 비어 있다.
    std::string fontSwapTo;
    std::int32_t fontSize = 1000;
    double fontSizePt() const;
    // 글자 크기 칸이 고른 단위(pt, mm, cm, inch, px)로 본 크기. 소수 둘째 자리다.
    double fontSizeIn(const std::string& unit) const;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    std::string underlineType = "NONE";
    bool strikeout = false;
    std::string color = "#000000";
    // 블록 안에서 값이 섞였는가. 섞인 칸은 셸이 비워 둔다(한글처럼 글꼴·크기는 빈칸, 정렬은 아무것도 누르지 않음).
    // 위 값은 블록 첫 글자 자리의 값이다.
    struct Mixed {
        bool style = false;
        bool font = false;
        bool fontSize = false;
        bool bold = false;
        bool italic = false;
        bool underline = false;
        bool strikeout = false;
        bool color = false;
        bool align = false;
        bool lineSpacing = false;
    } mixed;
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
    bool overwrite = false;  // 수정 모드
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
    SelectAll = 12,  // Ctrl+A. 커서가 든 글 묶음 전체를 블록으로 잡는다
    Insert = 13,     // 삽입·수정 바꾸기
};

// 읽기 전용 문서. 글자 넣기·지우기·문단 나누기·탭·서식·개체 크기 조절을 받지 않는다.
// 클릭·끌기 블록, 방향키·Home·End 이동, 블록 글자 읽기(복사)는 그대로다.
void setReadOnly(bool on);
bool readOnly();

// 수정(겹쳐 쓰기) 모드. 블록이 없을 때 친 글자 수만큼 캐럿 뒤 글자를 바꾼다. 문단 끝과 개체·조판 부호는
// 넘지 않고, 그 앞에서는 삽입처럼 넣는다. 조합 중인 글은 처음 넣을 때만 바꾸고 그 뒤로는 조합을 고친다.
void setOverwrite(bool on);
bool overwrite();

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
// 문서에 조합 중 글이 있는지. 다른 편집·키·클릭은 조합을 끝낸다.
bool compositionActive();

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
    // 문단마다 지금 간격 종류의 단위로 읽는다. 퍼센트면 %, 아니면 pt 다.
    bool hasLineSpacingValue = false;
    double lineSpacingValue = 0;
    // 한 단계 올리기(+)·내리기(-). 퍼센트는 10%, 아니면 1pt 씩이다.
    int lineSpacingStep = 0;

    bool hasFont = false;
    std::string font;
    bool hasFontSize = false;
    std::int32_t fontSize = 1000;
    // 글자 크기 칸이 준 값과 단위. 적용할 때 HU 로 바꾼다.
    bool fontSizeValueSet = false;
    double fontSizeValue = 0;
    std::string fontSizeUnit;
    // 글자마다 1pt 씩 올리기(+)·내리기(-).
    int fontSizeStep = 0;
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
    // 켜고 끄기("bold", "italic", "underline", "strikeout"). 지금 고른 글의 모양을 보고 켤지 끌지 core 가 정한다.
    std::vector<std::string> toggles;
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

// 편집 용지: 커서가 있는 구역의 용지·여백·제본·격자 값. 길이는 HWPUNIT 이다.
// paperWidth·paperHeight 는 한글 대화 상자처럼 좁게(세로) 놓은 크기이고, landscape 가 참이면 가로로 눕힌다.
// gutterType 은 0 한쪽, 1 맞쪽, 2 위로. lineGrid·charGrid 는 격자 간격(0 이면 끔), wongoji 는 원고지 정서법.
// textDirection 은 글자 방향 0 가로, 1 세로(영문 눕힘), 2 세로(영문 세움)다.
// section·sectionCount 는 읽기 전용이다.
FormatFields pageSetupAtCursor(const model::Document& doc);

// 위 이름 중 고친 값만 받아 커서 구역(wholeDocument 면 모든 구역)에 적용하고 문서 전체를 다시 짠다.
// 본문 폭이나 글자 격자가 바뀐 구역은 저장된 줄 나눔을 버리고 새 폭으로 다시 나눈다.
EditResult editApplyPageSetup(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool wholeDocument, bool useStoredLineHeight = false);

// 캐럿 자리에 줄 rows·칸 cols 표를 넣는다. 한글 표 만들기(너비 단에 맞춤, 글자처럼 취급)와 같은 표이고,
// 캐럿은 첫 칸으로 간다. 블록이 있으면 지우고 넣는다.
EditResult editInsertTable(model::Document& doc, std::vector<Node>& pages, int rows, int cols,
                           bool useStoredLineHeight = false);

// 캐럿 자리에 그림 파일(png·jpg·gif·bmp)을 글자처럼 넣는다. 한글 그림 넣기처럼 픽셀 × 75 크기이고, 단보다 넓으면
// 단 폭으로 줄인다. 캐럿은 그림 뒤로 간다. 모르는 파일이면 아무것도 하지 않는다.
EditResult editInsertPicture(model::Document& doc, std::vector<Node>& pages, const Bytes& image,
                             bool useStoredLineHeight = false);

// 도형 그리기. tool: 0 끔, 1 직사각형, 2 타원, 3 직선, 4 글상자, 5 세로 글상자.
// 켜면 다음 끌기 한 번이 그 도형 상자가 된다(한글처럼 한 번 그리면 꺼진다). Esc 로 끈다.
void setDrawTool(int tool);
int drawTool();
// 끌기를 마친 도형을 문서에 넣는다. 포인터를 뗀 뒤 부른다. 그릴 것이 없으면 handled 가 거짓이다.
EditResult editInsertDrawnShape(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight = false);
// 막힌 글꼴 때문에 받지 못한 개체 넣기(표·그림·그린 도형)를 버린다. 셸에서 바꾸기를 그만두면 부른다.
// 글꼴을 바꾸면 editSwapLicenseFonts 가 남은 넣기를 이어서 한다.
void dropPendingInsert();
// page 쪽 종이 좌표 (x0, y0)에서 (x1, y1)까지 끈 도형을 넣는다. 한글이 마우스로 그린 도형과 같은 값이다.
EditResult editInsertShape(model::Document& doc, std::vector<Node>& pages, int tool, int page, std::int32_t x0,
                           std::int32_t y0, std::int32_t x1, std::int32_t y1, bool useStoredLineHeight = false);

// 고른 띄운 개체를 끌어 옮겼으면 그 위치를 문서에 반영한다. 포인터를 뗀 뒤 부른다. 없으면 handled 가 거짓이다.
EditResult editMoveObject(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight = false);
// 그림이 아닌 고른 개체(표·도형·글상자)의 손잡이 크기 조절을 반영한다. 포인터를 뗀 뒤 부른다.
EditResult editResizeObject(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight = false);
// 표 칸 경계선을 끌었으면 칸 폭·줄 높이를 바꾼다. 포인터를 뗀 뒤 부른다.
EditResult editResizeTableBorder(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight = false);

// 되돌리기·다시 하기. 위 편집 명령은 문서를 고치면 고치기 전 문단(편집 용지는 구역)을 남긴다.
// 같은 문단에 이어 친 글자와 이어 지운 글자는 한 번에 되돌린다. 캐럿은 그 편집 앞(다시 하기는 뒤) 자리로 간다.
EditResult editUndo(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight = false);
EditResult editRedo(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight = false);
bool canUndo();
bool canRedo();
// 저장한 때를 적는다. 그 뒤 되돌리기·다시 하기로 그 모습에 돌아오면 undoAtSaved 가 참이다(문서를 연 때도 참).
void markUndoSaved();
bool undoAtSaved();
// 새 문서를 열면 비운다.
void clearUndoHistory();
// begin ~ end 사이의 편집을 한 칸으로 묶는다. 되돌리기·다시 하기 한 번에 함께 움직인다(agent 의 apply 한 번 등).
// beginUndoGroup 은 묶음 번호를 돌려준다. topUndoGroup 은 맨 위 되돌리기 칸의 묶음 번호(없거나 안 묶였으면 0)다.
int beginUndoGroup();
void endUndoGroup();
// 묶음이 열려 있는가. 안쪽 편집이 바깥 묶음을 닫지 않게 본다.
bool undoGroupOpen();
int topUndoGroup();
// 다시 하기 기록만 버린다. 실패한 묶음을 되돌린 뒤 다시 하기로 살아나지 않게 한다.
void clearRedoHistory();

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
// 원격 화면: 화면 캔버스가 이 스레드에 없다. 2D 그림 목록(screen_ops.hpp)을 sink 로 넘긴다.
// 화면은 바뀐 프레임만 낸다. sink 가 거짓을 돌려주면 그 프레임은 못 낸 것이다.
void setScreenOpSink(ScreenOpSink sink);
// 렌더 쪽 큰 자료의 바이트(메모리 보고용): 줄 글자 자리, 글자 쿼드, 글자 윤곽, 마스크 사본, 아틀라스 CPU 사본.
void renderMemory(std::vector<std::pair<std::string, std::size_t>>& out);
bool attachRemoteScreen();
// 화면이 글자·그림·표 테두리를 칸 비트맵 대신 직접 그린다. GPU 화면이 있는 셸이 Host 로 켠다.
void setScreenVectors(bool on);
// 웹에서는 브라우저 WebGPU 장치가 나중에 붙는다. 붙기 전에는 false다.
bool screenReady();
void dropScreenSprite(int id);
bool frameHasGlyphs(int id);
// 칸이 들고 있는 화면 글자의 문단 위치(이야기, 문단, 글자). 블록 판단이 쓰는 값이다.
struct FrameGlyphSpot {
    int story = -1;
    int paragraph = -1;
    int offset = -1;
};
std::vector<FrameGlyphSpot> frameGlyphSpots(int id);
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

}  // namespace chwp::render
