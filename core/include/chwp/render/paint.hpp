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
    // 칸을 그릴 때 쓰는 상자(HU). 가로 칸은 huX·huY·huW·huH 와 같고, 세로쓰기 칸은 돌린 틀의 상자다.
    std::int32_t drawHuX = 0;
    std::int32_t drawHuY = 0;
    std::int32_t drawHuW = 0;
    std::int32_t drawHuH = 0;
    // 세로쓰기 본문 칸. x·y·w·h 는 종이 픽셀이고 huX·huY·huW·huH 는 종이 HU 다(다른 배율의 자리·잘라 내기에 쓴다).
    // drawHu*·originX·originY 는 돌린 틀의 HU 다. 칸은 돌린 틀로 그린 뒤 시계 방향으로 돌려 굽는다.
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
    std::string headingType = "NONE";  // NONE, OUTLINE, NUMBER, BULLET
    int headingLevel = 0;              // 0 이 1 수준
    std::string lineSpacingType = "PERCENT";
    std::int32_t lineSpacing = 160;
    // 보이는 줄 간격: 퍼센트면 % 값, 아니면 pt 값과 그 단위.
    double lineSpacingValue() const;
    const char* lineSpacingUnit() const;
    std::uint32_t charShapeId = 0;
    std::string font;
    // font 가 라이선스로 막힌 글꼴이면 편집할 때 바뀔 글꼴 이름. 아니면 비어 있다.
    std::string fontSwapTo;
    // 언어별 글꼴(model::Lang 차례: 한글, 영문, 한자, 일어, 외국어, 기호, 사용자). 서식 막대 언어 칸이 고른 언어의 글꼴이다.
    std::array<std::string, model::kLangCount> langFonts{};
    std::int32_t fontSize = 1000;
    double fontSizePt() const;
    // 글자 크기 칸이 고른 단위(pt, mm, cm, inch, px)로 본 크기. 소수 둘째 자리다.
    double fontSizeIn(const std::string& unit) const;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    std::string underlineType = "NONE";
    bool strikeout = false;
    bool superscript = false;
    bool subscript = false;
    std::string underlineColor = "#000000";
    std::string strikeColor = "#000000";
    // 글자 테두리가 한 변이라도 있는가.
    bool charBorder = false;
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
        bool superscript = false;
        bool subscript = false;
        bool underlineColor = false;
        bool strikeColor = false;
        bool charBorder = false;
        bool color = false;
        bool align = false;
        bool lineSpacing = false;
        std::array<bool, model::kLangCount> langFonts{};
        bool operator==(const Mixed&) const = default;
    } mixed;
    bool operator==(const CursorStyle&) const = default;
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
    bool table = false;      // 캐럿이 표 칸 안이거나 칸 블록을 잡았다(셸은 표 탭을 보인다)
    bool cellBlock = false;  // 칸 블록을 잡았다(블록 단축키 M·S·W·H 가 산다)
    bool cellStart = false;  // 블록 없이 캐럿이 표 칸 맨 앞이다(Ctrl+Backspace 가 줄을 뺀다)
    int note = 0;            // 캐럿이 각주(1)·미주(2) 안이다(셸은 주석 탭을 보인다)
    bool master = false;     // 바탕쪽 편집 중이다(셸은 바탕쪽 탭을 보인다)
    bool operator==(const CaretStatus&) const = default;
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

// 바탕쪽 편집 화면이면 참이다. 그동안 쪽에는 바탕쪽만 그린다(본문·머리말·꼬리말을 감춘다).
// 켜고 끌 때마다 masterViewEpoch 가 오른다. 세션은 그 값이 바뀌면 쪽 그림을 모두 다시 그린다.
bool masterView();
int masterViewEpoch();

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
// 두 번 누르기로 연 고치기 대화 상자 이름(rubyEdit·composeEdit). 한 번 가져가면 비운다.
std::string takeOpenDialog();
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
    DocBegin = 14,   // Ctrl+PageUp. 문서 본문 처음(한글 MoveTopLevelBegin)
    DocEnd = 15,     // Ctrl+PageDown. 문서 본문 끝(한글 MoveTopLevelEnd)
};
// 키 하나에 붙은 이동(한글 2024 단축키 표): Ctrl+←→ 단어(MovePrevWord·MoveNextWord), Ctrl+↑↓ 문단 처음
// (MovePrevParaBegin·MoveNextParaBegin), Alt+Home·End 문단 처음·끝(MoveParaBegin·MoveParaEnd).
// Shift 를 같이 누르면 블록을 넓힌다.

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
// 조합을 끝낸다. 조합 중에는 미뤘던 편집 마무리 알림(계산식 다시 계산 등)을 확정한 글자로 낸다.
EditResult editCommitComposition(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight = false);

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
    // 스타일 이름(styleName). 적용할 때 문서의 스타일에서 찾는다. 없는 이름이면 고치지 않는다.
    std::string styleName;
    // 첫 줄 들여쓰기 한 단계(+)·내어쓰기 한 단계(-). 한 단계는 1pt 다(한글 2024 ParagraphShapeIndentPositive).
    int indentStep = 0;
    // 왼쪽·오른쪽 여백 한 단계(한글 2024 ParagraphShapeIncrease/DecreaseLeftMargin·RightMargin). 한 단계는 1pt(100HU)이고 끝값은 단 폭으로 정한다.
    // marginLeftStep 은 + 가 왼쪽 여백을 늘리고, marginRightStep 은 + 가 오른쪽 여백을 줄인다(한글 이름 그대로:
    // IncreaseRightMargin 은 글 오른쪽 끝을 오른쪽으로 민다).
    int marginLeftStep = 0;
    int marginRightStep = 0;
    // 문단 번호(NUMBER)·글머리표(BULLET)·개요(OUTLINE) 켜고 끄기. 블록 첫 문단이 이미 그 종류면 끄고, 아니면 켠다.
    // 개요는 개요 수준 스타일(개요 1)을 입히고, 끄면 바탕 스타일로 돌린다(한글 2024 PutOutlineNumber).
    std::string headingToggle;
    // 수준 한 단계 아래(+)·위(-). 개요는 그 수준의 개요 스타일을, 번호·글머리표는 수준 값을 바꾼다.
    // 머리가 없는 문단을 아래로 내리면 개요 1 이 된다(한글 2024 ParaNumberBulletLevelDown).
    int headingLevelStep = 0;

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
    // 글자마다 자간 1%·장평 1% 씩 넓히기(+)·좁히기(-)(한글 2024 CharShapeSpacing·Width Increase/Decrease).
    int spacingStep = 0;
    int ratioStep = 0;
    // 보통 모양(한글 2024 CharShapeNormal, Alt+Shift+C). 글꼴·크기·장평·자간·위치는 두고 속성을 모두 끈다.
    bool normalShape = false;
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
    // 밑줄·취소선·글자 테두리 목록에서 고른 값(한글 2024 서식 탭 펼침 목록). 이름은 underlineShape, underlineColor,
    // strikeShape, strikeColor, charBorderType, charBorderColor 이다. 고른 값을 기억하고 그 줄을 켠다.
    std::vector<std::pair<std::string, std::string>> linePicks;
    // 켜고 끄기("bold", "italic", "underline", "strikeout", "superscript", "subscript", "superSub", "charBorder").
    // 지금 고른 글의 모양을 보고 켤지 끌지 core 가 정한다. superSub 는 보통 → 위 첨자 → 아래 첨자 → 보통 차례다.
    std::vector<std::string> toggles;
    bool hasColor = false;
    std::string color;
    // 형광펜(#RRGGBB). 빈 값은 지운다. 고른 글자에만 칠한다.
    bool hasMarkpen = false;
    std::string markpen;

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

// 쪽 하나의 그려진 배치. 좌표는 쪽 왼쪽 위에서 잰 HWPUNIT 이고, 그리기와 같은 자리 계산(placeLine)이다.
// 줄: story·문단·글자 범위 [from, to)(글자 순번), 줄 상자, 글자가 실제로 끝나는 오른쪽(inkRight), 그 줄에 붙은
// 문단 번호·글머리(heading). 상자: 표·칸·그림·도형·수식·묶음·단. 칸은 그 칸 글 story 와 줄·칸 자리를 든다.
// band 는 머리말·꼬리말·쪽 번호, master 는 바탕쪽, turned 는 세로쓰기 틀 안(좌표가 돌린 틀 기준)이다.
struct LayoutLine {
    int story = -1;
    int paragraph = -1;
    int from = 0;
    int to = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t inkRight = 0;
    std::string heading;
    bool band = false;
    bool master = false;
    bool turned = false;
};
struct LayoutBox {
    std::string kind;  // table, cell, picture, shape, equation, container, column, object
    int story = -1;    // 칸: 칸 글 story. 개체: 개체가 박힌 문단의 story
    int paragraph = -1;
    int row = -1;
    int col = -1;
    int rowSpan = 1;
    int colSpan = 1;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    bool inlineChar = false;
    bool band = false;
    bool master = false;
    bool turned = false;
};
struct PageLayout {
    std::int32_t width = 0;   // 쪽 크기
    std::int32_t height = 0;
    std::int32_t bodyX = 0;   // 본문 틀(여백 안)
    std::int32_t bodyY = 0;
    std::int32_t bodyWidth = 0;
    std::int32_t bodyHeight = 0;
    std::vector<LayoutLine> lines;
    std::vector<LayoutBox> boxes;
};
// 쪽 글자를 아직 채우지 않았으면 채운다(그리기 전과 같다).
PageLayout describePage(const model::Document& doc, std::vector<Node>& pages, int pageIndex);

// 모양 값 읽기. 이름은 setFormatField 와 대화 상자가 쓰는 이름과 같다(읽은 값을 고쳐 그대로 넣을 수 있다).
// 글자 모양(font·fontSize·bold … 언어별 값, "char." 테두리/배경)과 문단 모양(align·줄 간격·여백·번호, "para.", 탭).
FormatFields charShapeFields(const model::Document& doc, std::uint32_t charShapeId);
FormatFields paraShapeFields(const model::Document& doc, std::uint32_t paraShapeId);
// 표: 칸 간격·쪽 나눔·제목 줄 반복·안쪽 여백과 "table." 테두리/배경.
FormatFields tableFields(const model::Document& doc, const model::Table& table);
// 표 칸: 세로 정렬·제목 칸·안 여백(칸 여백이 없으면 표 안쪽 여백)과 "cell." 테두리/배경.
FormatFields tableCellFields(const model::Document& doc, const model::Table& table, const model::TableCell& cell);
// 개체 놓기: 글자처럼 취급·본문과의 배치·기준·정렬·오프셋·크기(HWPUNIT).
FormatFields placementFields(const model::Placement& placement);
// 구역 하나(0부터)의 편집 용지 값(pageSetupAtCursor 와 같은 이름)과 단(columnCount·columnGap·columnType·
// columnSameSize·columnLineType), 쪽 번호 시작(pageStart, 0 은 앞 구역에 이어서), 기본 탭(sectionDefaultTab),
// 감추기(hideHeader·hideFooter·hideMaster·hidePageNum).
FormatFields sectionFields(const model::Document& doc, int section);

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

// 표 구조 편집. 칸은 표 칸 story 번호로 가리킨다. 규칙은 한글 2024 와 같다(model/table_edit.hpp).
enum class TableEditKind {
    InsertRows, InsertColumns, DeleteRows, DeleteColumns, Merge, Split,
    CellBorder,   // 셀 테두리 단추(border)
    CellFill,     // 셀 배경색(color)
    EqualWidth,   // 셀 너비를 같게
    EqualHeight,  // 셀 높이를 같게
    CellAlign,    // 내용 정렬: vertAlign(TOP·CENTER·BOTTOM), horzAlign(LEFT·CENTER·RIGHT, 비면 그대로)
    CellDirection,  // 글자 방향: direction 0 가로, 1 세로(영문 눕힘), 2 세로(영문 세움)
    TableMargin,  // 표 여백: margin 은 outer(바깥 여백)·inner(모든 셀의 안 여백), preset 은 normal·wide·narrow·none
    TreatAsChar,  // 글자처럼 취급 켜고 끄기
    Wrap,         // 본문과의 배치: wrap 은 SQUARE·TOP_AND_BOTTOM·IN_FRONT_OF_TEXT·BEHIND_TEXT. 글자처럼 취급이면 거절
    Caption,      // 캡션: caption 은 TOP·BOTTOM·LEFT_TOP·LEFT_CENTER·LEFT_BOTTOM·RIGHT_TOP·RIGHT_CENTER·RIGHT_BOTTOM·NONE(떼기)
    SplitTable,   // 표 나누기: 캐럿 줄부터 아래를 새 표로
    MergeTable,   // 표 붙이기: 바로 다음 문단의 표를 이 표 아래에 잇는다
    Comma,        // 1,000 단위 구분 쉼표: comma 가 insert 면 넣고 delete 면 뺀다. 칸 블록에만 한다
    Flip,         // 표 뒤집기: flip 0 줄 기준, 1 칸 기준, 2 줄/칸, 3 반시계 90도, 4 180도, 5 시계 90도. swapMargin 은 여백 뒤집기
    AutoFill,     // 표 자동 채우기: 칸 블록의 처음 값 규칙으로 나머지 칸을 채운다. 칸 블록에만 한다
    Sort,         // 표 정렬: sortKeys(기준 셋까지), sortFromRear·sortExtendYear·sortYearBase. 두 줄 이상 칸 블록에만 한다
    // 계산식(한글 TableFormula·TableFormulaSum/Avg/Pro + Auto·Hor·Ver). formulaMode 가 block 이면 블록 계산식(칸 블록),
    // horizontal·vertical 이면 쉬운 계산식(칸 블록이면 끝 칸, 아니면 캐럿 자리), custom 이면 formulaText·formulaFormat 을
    // 캐럿 자리에 넣는다(캐럿이 계산식 결과 안이면 그 식을 고친다). formulaFunc 0 합계 1 평균 2 곱
    Formula
};
struct TableEditCommand {
    TableEditKind kind = TableEditKind::InsertRows;
    int story = -1;     // 기준 칸
    int toStory = -1;   // 지우기·합치기·테두리·배경·크기 같게: 범위의 다른 끝 칸. 두 칸을 덮는 사각형이 범위다. -1 이면 기준 칸만
    bool after = true;  // 넣기: 참이면 아래(오른쪽), 거짓이면 위(왼쪽)
    int count = 1;      // 넣기: 줄(칸) 수
    int rows = 1;       // 나누기: 줄 수 × 칸 수
    int cols = 1;
    // 테두리: all, outside, inside, top, bottom, left, right, insideHorz, insideVert, none, diagonalDown, diagonalUp
    std::string border;
    // 테두리 선. 한글 표 탭의 기본 선(실선 0.12mm 검정)이 기본값이다. lineWidth 는 HWPUNIT, 0 이면 0.12mm.
    std::string lineType = "SOLID";
    std::int32_t lineWidth = 0;
    std::string lineColor = "#000000";
    std::string color;  // 배경: "#RRGGBB". 비었거나 "none" 이면 배경을 없앤다
    std::string vertAlign;  // 내용 정렬
    std::string horzAlign;
    int direction = 0;      // 글자 방향
    std::string margin;     // 표 여백: outer, inner
    std::string preset;     // 표 여백: normal, wide, narrow, none
    std::string wrap;       // 배치
    std::string caption;    // 캡션 자리
    std::string comma;      // 1,000 단위 쉼표: insert, delete
    int flip = 0;           // 표 뒤집기 종류(한글 TableSwap type)
    bool swapMargin = false;  // 표 뒤집기: 표 안쪽 여백과 칸 여백도 같이 뒤집는다
    // 표 정렬 기준(한글 Sort 의 KeyOption). column 은 블록 첫 칸이 1, format 0 글자 1 숫자 2 날짜 3 코드.
    struct SortKey {
        int column = 1;
        int format = 0;
        bool descending = false;
    };
    std::vector<SortKey> sortKeys;
    bool sortFromRear = false;   // 뒤에서 앞으로 비교
    bool sortExtendYear = true;  // 두 자리 연도 기준 적용
    int sortYearBase = 1930;
    bool sortOldHangul = false;   // 고어 정렬
    bool sortFinalFirst = false;  // 종성/중성/초성 순으로 비교
    std::string formulaMode;      // 계산식: block, horizontal, vertical, custom
    int formulaFunc = 0;          // 계산식: 0 합계, 1 평균, 2 곱
    std::string formulaText;      // 계산식(custom): "=SUM(A1:B2)"
    std::string formulaFormat;    // 계산식(custom): "%g,", "%.2f" 등
};
const char* tableEditName(TableEditKind kind);
// 이름(insertRows, insertColumns, deleteRows, deleteColumns, mergeCells, splitCell, cellBorder, cellFill,
// equalWidth, equalHeight, cellAlign, cellDirection, tableMargin, treatAsChar, wrap, caption, splitTable,
// mergeTable, comma, flip, autoFill, sort, formula)을 종류로. 모르는 이름이면 거짓.
bool tableEditKindOf(const std::string& name, TableEditKind& kind);
// 편집기 캐럿·칸 블록으로 칸을 채운다. 칸 블록이면 두 끝 칸, 아니면 캐럿이 든 칸. 표 칸이 아니면 거짓.
bool tableCommandAtCursor(const model::Document& doc, TableEditCommand& cmd);
// 명령이 고칠 표를 담은 story·문단.
bool tableEditHost(const model::Document& doc, const TableEditCommand& cmd, int& story, int& paragraph);
// 표를 고치고 다시 놓고, 캐럿을 고친 자리의 칸으로 옮긴다. 되돌리기 한 칸이다.
// 고치지 못하면 handled 가 거짓이고 error 에 이유를 적는다(합친 칸을 자르는 합치기 등).
// 모든 줄·칸을 지우면 표를 지운다.
EditResult editTable(model::Document& doc, std::vector<Node>& pages, const TableEditCommand& cmd,
                     bool useStoredLineHeight = false, std::string* error = nullptr);

// 각주·미주 넣기(한글 InsertFootnote·InsertEndnote). 캐럿 자리에 주를 넣고 캐럿을 주 글 묶음의 번호 뒤로 옮긴다.
// 머리말·꼬리말·주 안이면 넣지 않고 error 에 이유를 적는다. 되돌리기 한 칸이다.
EditResult editInsertNote(model::Document& doc, std::vector<Node>& pages, bool endnote, bool useStoredLineHeight = false,
                          std::string* error = nullptr);

// 캡션(한글 2024 CaptionPos*·TableCaptionPos*·DetachCaption). 개체를 골랐으면 그 개체(그림·도형·수식·글상자·표),
// 아니면 캐럿이 든 표다. caption: TOP·BOTTOM·LEFT_TOP·LEFT_CENTER·LEFT_BOTTOM·RIGHT_TOP·RIGHT_CENTER·RIGHT_BOTTOM·NONE.
EditResult editCaption(model::Document& doc, std::vector<Node>& pages, const std::string& caption,
                       bool useStoredLineHeight = false, std::string* error = nullptr);
// 책갈피(한글 2024 책갈피 대화 상자, bookmark_commands.cpp). 목록은 글 묶음·문단·글자 차례다.
struct BookmarkEntry {
    std::string name;
    int story = -1;
    int paragraph = -1;
    int charIndex = 0;
};
std::vector<BookmarkEntry> bookmarkList(const model::Document& doc);
// 대화 상자의 처음 이름: 캐럿 뒤 낱말, 없으면 앞 낱말.
std::string bookmarkDefaultName(const model::Document& doc, const std::vector<Node>& pages);
// action: insert(캐럿 자리에 넣기, 같은 이름이 있으면 거절), delete(지우기), goto(그 자리로 캐럿, 문서는 그대로),
// rename(name 을 newName 으로. 빈 이름·이미 있는 이름은 거절).
EditResult editBookmark(model::Document& doc, std::vector<Node>& pages, const std::string& action, const std::string& name,
                        bool useStoredLineHeight = false, std::string* error = nullptr, const std::string& newName = {});
// 찾아가기: 쪽(0 부터) 본문 맨 위 줄 맨 앞으로 캐럿을 옮긴다.
EditResult editGoToPage(model::Document& doc, std::vector<Node>& pages, int page, std::string* error = nullptr);
// 구역 찾아가기. section 은 0 부터다. 그 구역 본문 맨 앞으로 캐럿을 옮긴다.
EditResult goToSectionNow(model::Document& doc, std::vector<Node>& pages, int section, std::string& why);
EditResult editGoToSection(model::Document& doc, std::vector<Node>& pages, int section, std::string* error = nullptr);
// 모양 복사(한글 ShapeCopyPaste, Alt+C). action copy(type 0 글자 모양, 1 문단 모양, 2 둘 다, 4 문단 스타일)는 캐럿 자리
// 모양을 기억하고, paste 는 블록에 입힌다(블록이 없으면 그대로). copiedShapeReady 는 기억한 모양이 있는지다.
EditResult copyShapeNow(model::Document& doc, std::vector<Node>& pages, const std::string& action, int type,
                        bool useStoredLineHeight, std::string& why);
EditResult editCopyShape(model::Document& doc, std::vector<Node>& pages, const std::string& action, int type,
                         bool useStoredLineHeight = false, std::string* error = nullptr);
bool copiedShapeReady();
// 표를 문자열로(fields: delimiter tab·comma·space·other, text)와 문자열을 표로(fields: delimiter auto·tab·comma·space·
// char·string, text, keep). 규칙은 table_text.cpp. editTableText 는 fields 의 action(tableToText·textToTable)으로 고른다.
EditResult tableToTextNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight, std::string& why);
EditResult textToTableNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight, std::string& why);
EditResult editTableText(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                         bool useStoredLineHeight = false, std::string* error = nullptr);
// 각주↔미주 바꾸기(한글 ExchangeFootnoteEndnote). mode 0 모든 각주를 미주로, 1 모든 미주를 각주로, 2 서로 바꾸기.
EditResult exchangeNotesNow(model::Document& doc, std::vector<Node>& pages, int mode, bool useStoredLineHeight, std::string& why);
EditResult editExchangeNotes(model::Document& doc, std::vector<Node>& pages, int mode, bool useStoredLineHeight = false,
                             std::string* error = nullptr);
// 찾아가기의 줄·스타일·조판 부호(goto_commands.cpp). fields: kind(line·style·mark)와 line(1부터)·style(스타일 id)·mark(이름).
// goToView 는 lineCount, styles("id\t이름" 줄), marks(조판 부호 이름 줄)다.
FormatFields goToView(const model::Document& doc, const std::vector<Node>& pages);
EditResult goToNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, std::string& why);
// 조판 부호 지우기. fields.marks 는 지울 조판 부호 이름(goToView 의 marks 이름)을 줄로 나눈 것이다.
EditResult deleteMarksNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, bool useStoredLineHeight,
                          std::string& why);
EditResult editDeleteMarks(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                           bool useStoredLineHeight = false, std::string* error = nullptr);
// 글자 바꾸기(한글 대문자/소문자 바꾸기·전각/반각 바꾸기). 블록 안 글을 바꾸고 블록은 그대로 둔다.
// fields: kind(case·width), case 는 mode(upper·lower·toggle·wordCap·sentenceCap·sentenceCapOnly),
// width 는 to(half·full)와 바꿀 대상 digit·latin·symbol·kana·jamo(0·1, 없으면 1).
EditResult convertTextNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight, std::string& why);
EditResult editConvertText(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                           bool useStoredLineHeight, std::string* error = nullptr);
// 본문 정렬(한글 Sort, 글 블록). 블록이 걸친 문단을 기준대로 옮긴다(규칙은 sort_commands.cpp·model::paragraphSortOrder).
// fields: keyNPosition(0 문단, n 필드 n, 2147483647 단어; 2·3 번째의 0 은 없음)·keyNFormat(0 글자 1 숫자 2 날짜 3 코드)·
// keyNDesc(N 은 1~3), delimiter(0 탭 1 쉼표 2 빈칸 3 기호)·delimiterChars·ignoreMulti, fromRear·oldHangul·finalFirst·
// extendYear·yearBase·eachPara. 되돌리기 한 칸이다.
EditResult sortParagraphsNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                             bool useStoredLineHeight, std::string& why);
EditResult editSortParagraphs(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool useStoredLineHeight, std::string* error = nullptr);
// 계산식 대화 상자가 볼 값: inTable(캐럿이 표 칸인지), cell(칸 이름 "B2"), 캐럿이 계산식 결과 안이면 formula·format.
FormatFields tableFormulaView(const model::Document& doc, const std::vector<Node>& pages);
// 정렬 대화 상자가 볼 값: mode(table 칸 블록·text 글 블록·빈 값 못 함), text 면 paragraphs(블록 문단 수)·
// fields(fields 의 구분 기호로 가장 많이 나뉜 필드 수).
FormatFields sortView(const model::Document& doc, const std::vector<Node>& pages, const FormatFields& fields);
// 하이퍼링크 넣기(한글 InsertHyperlink). text 가 비면 주소를 보인다. 고른 글은 바꾼다. 되돌리기 한 칸이다.
EditResult editInsertHyperlink(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                               const std::string& url, bool useStoredLineHeight = false, std::string* error = nullptr);

// 단 설정(한글 2024 단 설정 대화 상자·단 ▾ 목록, column_commands.cpp). 값 이름:
//   type(NEWSPAPER·BALANCED_NEWSPAPER·PARALLEL), count, direction(LEFT·RIGHT·MIRROR), sameSize, gapMm,
//   widthMm.N·gapMm.N(너비가 다를 때 단마다), line·lineType·lineWidth(HWPUNIT)·lineColor(구분선),
//   preset(one·two·three·left·right: 그 모양으로 다시 짠다), applyTo(WHOLE 문서 전체·NEW_PAGE 새 쪽으로·NEW_COLUMN 새 단으로).
// columnView 는 캐럿 자리 단 설정에 고른 값을 얹어 너비·간격을 다시 계산한 값(대화 상자에 보일 값)이다.
// 함께 maxCount(단 수 최대)와 bodyMm(본문 폭), preset(지금 모양과 같은 목록 칸)을 준다.
FormatFields columnView(const model::Document& doc, const std::vector<Node>& pages, const FormatFields& asked);
// 단 설정을 넣는다. 되돌리기 한 칸이다. 하지 못하면 error 에 이유를 적는다.
EditResult editColumns(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                       bool useStoredLineHeight = false, std::string* error = nullptr);

// 구역 설정(한글 2024 구역 설정 대화 상자, section_commands.cpp). 값 이름:
//   startsOn(CONTINUE·EVEN·ODD·USER), pageNumber(USER 일 때), pictureStart·tableStart·equationStart(0 이어서),
//   hideFirstHeaderFooter·hideFirstMaster·hideFirstBorderFill·hideEmptyLine, columnGapPt·defaultTabPt,
//   applyTo(CURRENT·WHOLE·NEW_SECTION). sectionSettingsView 는 캐럿 구역 값과 applyOptions(고를 수 있는 적용 범위)를 준다.
FormatFields sectionSettingsView(const model::Document& doc);
// 표/셀 속성(한글 2024 표/셀 속성 대화 상자, table_props.cpp). 캐럿 표(칸 블록이면 그 칸들) 값. 값 이름:
//   기본 widthMm·heightMm(보기만)·treatAsChar·wrap·horzRelTo·horzAlign·horzOffsetMm·vertRelTo·vertAlign·vertOffsetMm·
//   flowWithText·allowOverlap·numberingType, 여백/캡션 outLeftMm..outBottomMm·caption·captionWidthMm·captionGapMm·
//   captionFullSize, 표 pageBreak(NONE·TABLE·CELL)·repeatHeader·inLeftMm..inBottomMm,
//   셀 cellWidthMm·cellHeightMm(보기만)·cellMargin·cellLeftMm..cellBottomMm·header·direction·cellVertAlign.
FormatFields tablePropertiesView(model::Document& doc);
// 덧말 넣기·글자 겹치기(glyph_group_commands.cpp). 값 이름:
//   kind(ruby·compose), 덧말 mainText·subText·pos(TOP·BOTTOM)·ratio(%)·align(문단 정렬 이름),
//   겹치기 chars·circle(CHAR·SHAPE_CIRCLE…)·size(글자 크기 조절)·type(OVERLAP·SPREAD). 겹쳐 쓸 글자 수 한도는 없다.
//   블록이 한 문단 안 글자면 그 글이 본말·겹칠 글자이고, 넣을 때 블록을 지운다.
FormatFields glyphGroupView(model::Document& doc, const std::vector<Node>& pages);
EditResult glyphGroupNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                         bool useStoredLineHeight, std::string& why);
EditResult editGlyphGroup(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                          bool useStoredLineHeight = false, std::string* error = nullptr);
EditResult editTableProperties(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                               bool useStoredLineHeight = false, std::string* error = nullptr);
// 주석 모양(한글 2024 주석 모양 대화 상자, note_shape_commands.cpp). 값 이름:
//   numFormat·userChar·prefix·suffix, line·lineLength(5CM·2CM·THIRD·COLUMN·USER)·lineLengthMm·lineType·lineWidth·lineColor,
//   aboveMm·betweenMm·belowMm, numbering(CONTINUOUS·ON_SECTION·ON_PAGE)·startNumber, superscript(작게), beneathText,
//   place(각주 EACH_COLUMN·MERGED_COLUMN·RIGHT_MOST_COLUMN, 미주 END_OF_DOCUMENT·END_OF_SECTION).
FormatFields noteShapeView(const model::Document& doc, bool endnote);
// 캐럿이 든 각주·미주를 닫고 그 주 바로 뒤 본문으로 간다(한글 주석 탭 닫기). 문서는 고치지 않는다.
EditResult editLeaveNote(model::Document& doc, std::vector<Node>& pages);
EditResult editNoteShape(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, bool endnote,
                         bool wholeDocument, bool useStoredLineHeight = false, std::string* error = nullptr);
// 쪽 테두리/배경 넣기(대화 상자 값은 FormatViewKind::PageBorder). 되돌리기 한 칸이다.
EditResult editPageBorder(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, bool wholeDocument,
                          bool useStoredLineHeight = false, std::string* error = nullptr);
EditResult editSectionSettings(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                               bool useStoredLineHeight = false, std::string* error = nullptr);

// 쪽 탭 컨트롤. 어디에 넣는지는 한글 2024 와 같다(page_commands.cpp).
enum class PageControlKind {
    Header,      // 머리말(apply: BOTH·ODD·EVEN). 캐럿 문단 맨 앞에 새로 넣고 그 쪽 머리말 편집으로 들어간다
    Footer,      // 꼬리말. 머리말과 같다
    DeleteBand,  // 고치는 머리말·꼬리말을 지우고 본문으로 나간다
    PageNumber,  // 쪽 번호 매기기(pos·format·sideChar). 구역 첫 문단의 것을 고치거나 넣는다
    NewNumber,   // 새 번호로 시작(numType·number). 캐럿 자리에 넣는다
    PageHiding,  // 현재 쪽만 감추기(hideHeader·hideFooter·hideMaster·hideBorder·hideFill·hidePageNum). 모두 끄면 지운다
    InsertPageNum,  // 쪽 번호 넣기(한글 InsertPageNum). 캐럿 자리에 쪽 자동 번호(아라비아 숫자)를 넣는다
    PrevBand,    // 이전 머리말/꼬리말(HeaderFooterToPrev). 문서 차례로 앞 머리말·꼬리말 편집으로 간다
    NextBand,    // 다음 머리말/꼬리말(HeaderFooterToNext)
    // 머리말/꼬리말 대화 상자의 편집(band: header·footer, apply: BOTH·ODD·EVEN). 캐럿 쪽에 보이는 그 머리말·꼬리말,
    // 없으면 앞쪽(가까운 쪽부터), 그다음 뒤쪽에서 처음 보이는 것으로 들어간다
    EditBand,
    // 바탕쪽(한글 2024 MasterPage·MasterPageType*·DeleteMasterPage·MasterPageToPrev/Next·MasterPageFront·
    // DuplicateMasterPage·ExcludeFirstPage). 캐럿 구역의 바탕쪽을 고친다.
    MasterPage,     // type(BOTH·ODD·EVEN·LAST_PAGE·OPTIONAL_PAGE)·pageNumber 의 바탕쪽 편집으로 들어간다. 없으면 만든다.
                    // duplicate·front 를 주면 그 바탕쪽에 준다
    CloseMaster,    // 바탕쪽 편집을 끝내고 들어오기 전 본문 자리로 간다(Shift+Esc 와 같다)
    DeleteMaster,   // scope: current(고치는 바탕쪽)·section(이 구역 모두)·document(문서 모두). 편집을 끝낸다
    PrevMaster,     // 문서 차례로 앞 바탕쪽 편집으로 간다
    NextMaster,
    MasterOptions,  // 고치는 바탕쪽의 front·duplicate, 구역의 excludeFirst(첫 쪽 감추기)
    // 쪽 지우기(한글 2024 DeletePage·현재 쪽 지우기). range: current(캐럿 쪽, 블록이면 블록이 걸친 쪽)·selected(블록이
    // 걸친 쪽)·custom(pages "2-3,5,7-" 쪽 차례, pageNumber 가 참이면 쪽 번호). 그 쪽 첫 글자부터 다음 쪽 첫 글자 앞까지를
    // 지운다(구역 마지막 쪽이면 앞 문단 끝부터 구역 끝까지). 첫 문단의 구역 정의·단 정의는 남긴다.
    DeletePage,
    // 메모(한글 InsertFieldMemo·메모 고치기·DeleteMemo). 캐럿 문단의 블록(없으면 캐럿 자리)에 메모를 건다.
    // text 는 메모 글(줄마다 문단), author 는 작성자. 고치기·지우기는 캐럿이 든 메모(겹치면 안쪽)다.
    InsertMemo,
    EditMemo,
    DeleteMemo,
    // 블록 계산(도구 > 블록 합계·블록 평균, 규칙은 model/block_calc.hpp). 값은 pageControlFields 로 읽고(ok·cells·count·
    // sum·average·canInsert), 넣기는 mode(sum·average)의 결과를 글 블록 바로 뒤에 넣는다(블록이 빈칸으로 끝나지 않으면
    // 빈칸 하나를 띄운다). 칸 블록은 칸 글을 칸 차례로 읽고 넣지는 않는다.
    BlockCalc,
    // 교정 부호 넣기(한글 검토-교정 부호, 파일 모양은 model/proofreading.hpp). kind 는 delete·clipping·attach·lineAttach·
    // sawtooth·thinking·praise·line·simpleChange·change(메모 고침표)·space·lineSeparate·lineInsert·symbol·insert, text 는
    // 고칠·넣을 글이다. 한글 2024 처럼 범위 부호(지움표 ...)는 블록이 있어야 하고 자리 부호(띄움표 ...)는 블록이 없어야 한다.
    // 줄 붙임표는 블록 문단 맨 앞에 빈 범위로, 메모 고침표는 메모처럼 블록(없으면 캐럿 낱말)에 건다(author·text).
    InsertProofMark
};
const char* pageControlName(PageControlKind kind);
// 이름(header, footer, deleteBand, pageNumber, newNumber, pageHiding, insertPageNum, prevBand, nextBand, editBand,
// masterPage, closeMaster, deleteMaster, prevMaster, nextMaster, masterOptions, deletePage, insertMemo, editMemo,
// deleteMemo, blockCalc, insertProofMark)을 종류로.
// 모르는 이름이면 거짓.
bool pageControlKindOf(const std::string& name, PageControlKind& kind);
// 대화 상자를 열 때 채울 값. 이름은 editPageControl 이 받는 이름과 같다.
// pos 는 NONE, TOP_LEFT, TOP_CENTER, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_CENTER, BOTTOM_RIGHT, OUTSIDE_TOP, OUTSIDE_BOTTOM,
// INSIDE_TOP, INSIDE_BOTTOM 이고 format 은 DIGIT, CIRCLED_DIGIT, ROMAN_CAPITAL, ROMAN_SMALL, LATIN_CAPITAL,
// LATIN_SMALL, HANGUL_SYLLABLE … 이다. sideChar 는 번호 앞뒤에 붙는 글(줄표 "-", 없으면 "").
// start 는 시작 번호다. 1 이 아니면 쪽 번호와 같은 문단에 쪽 새 번호를 둔다(한글 PageNumPos 의 NewNumber).
FormatFields pageControlFields(const model::Document& doc, const std::vector<Node>& pages, PageControlKind kind);
// 쪽 탭 컨트롤을 넣거나 고치고 문서를 다시 짠다. 되돌리기 한 칸이다. 하지 못하면 error 에 이유를 적는다.
EditResult editPageControl(model::Document& doc, std::vector<Node>& pages, PageControlKind kind,
                           const FormatFields& fields, bool useStoredLineHeight = false, std::string* error = nullptr);

// 문서 글 찾기. 찾은 구간은 story 안 문단의 글자 순번 [from, to)다. 문단 경계는 넘지 않는다.
// story 가 0 이면 모든 글 묶음을 번호 차례(구역 본문 먼저)로 본다. 글자처럼 놓인 개체는 U+FFFC 한 글자다.
// matchCase 가 거짓이면 ASCII 대소문자를 가리지 않는다. limit 이 0 이면 끝까지 찾는다.
struct TextHit {
    int story = -1;
    int paragraph = -1;
    int from = 0;
    int to = 0;
};
std::vector<TextHit> findText(const model::Document& doc, const std::string& text, bool matchCase, int story = 0,
                              int limit = 0);
// 찾기가 보는 문단 글(개체는 U+FFFC).
std::string paragraphSearchText(const model::Paragraph& para);
// 다음 찾기. 고른 글(없으면 캐럿) 뒤에서(backward 면 앞에서) 처음 맞는 곳을 골라 선택한다.
// 읽는 차례는 구역 본문 문단 차례이고, 문단 안 개체(표 칸·글상자)의 글은 그 문단 글 뒤다.
// 끝에 닿으면 처음부터(뒤로 찾으면 끝부터) 돌고 wrapped 를 참으로 한다. 없으면 handled 가 거짓이다.
EditResult selectNextText(const model::Document& doc, const std::vector<Node>& pages, const std::string& text,
                          bool matchCase, bool backward, bool* wrapped = nullptr);
// 바꾸기. 고른 글이 text 와 같으면 replacement 로 바꾼다. 한글처럼 i 번째 새 글자는 찾은 i 번째 글자의
// 모양을 받고, 남는 새 글자는 찾은 마지막 글자의 모양을 받는다. 고른 글이 다르면 바꾸지 않는다.
// replacement 에 줄 끝(CR·LF)이 있으면 바꾸지 않는다.
EditResult editReplaceText(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                           const std::string& replacement, bool matchCase, bool useStoredLineHeight);
// 모두 바꾸기. 문서의 모든 text 를 바꾼다. 되돌리기 한 칸이다. count 에 바꾼 수를 적는다.
EditResult editReplaceAllText(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                              const std::string& replacement, bool matchCase, bool useStoredLineHeight,
                              int* count = nullptr);

// 나누기(한글 2024 BreakPage·BreakColumn·BreakColDef·BreakSection). 캐럿 자리에서 문단을 나눈다.
//   Page: 뒤 문단을 다음 쪽에서 시작한다. Column: 다음 단에서 시작한다(1단이면 다음 쪽).
//   ColumnDef: 뒤 문단부터 단 설정을 새로 한다(앞 단 정의를 베낀다).
//   Section: 뒤 문단부터 새 구역이다. 구역 설정과 단 정의만 이어받고 머리말·꼬리말은 가져가지 않는다.
// 구역 본문에서만 한다. 표 칸·글상자 안이면 handled 가 거짓이다. 블록이 있으면 지우고 나눈다.
enum class BreakKind { Page, Column, ColumnDef, Section };
bool breakKindOf(const std::string& name, BreakKind& kind);  // page, column, columnDef, section
EditResult editInsertBreak(model::Document& doc, std::vector<Node>& pages, BreakKind kind,
                           bool useStoredLineHeight = false);

// 캐럿 자리에 그림 파일(png·jpg·gif·bmp)을 글자처럼 넣는다. 한글 그림 넣기처럼 픽셀 × 75 크기이고, 단보다 넓으면
// 단 폭으로 줄인다. width·height(HWPUNIT)를 주면 그 크기다. 하나만 주면 그림 가로세로 비율로 다른 쪽을 정한다.
// 캐럿은 그림 뒤로 간다. 모르는 파일이면 아무것도 하지 않는다.
EditResult editInsertPicture(model::Document& doc, std::vector<Node>& pages, const Bytes& image,
                             bool useStoredLineHeight = false, std::int32_t width = 0, std::int32_t height = 0);

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

// 캐럿 자리에 맞춰 머리말·꼬리말·바탕쪽 편집 상태를 맞춘다(캐럿이 머리말 글이면 그 편집, 본문이면 나옴).
// 캐럿을 화면 밖에서 옮긴 뒤(agent) 부른다. 바뀌었으면 참.
bool syncEditBand(const model::Document& doc);

// 이름으로 가리킨 개체(object_commands.cpp). 개체는 글 묶음 story·문단·문단 안 항목 순번 item(글 조각과 컨트롤을
// 차례로 센 번호, 쪽 그림의 anchorItem 과 같다)이다.
// 고른 개체를 놓는다(캐럿·블록은 그대로).
void clearObjectSelection();
// 그 개체를 고른다(화면에서 누른 것과 같은 고르기와 손잡이). 쪽에 그려진 개체가 없으면 거짓.
bool selectAnchoredObject(const model::Document& doc, const std::vector<Node>& pages, int story, int paragraph, int item);
// 개체 속성 값: placementFields 의 이름(treatAsChar, textWrap, textFlow, vertRelTo, horzRelTo, vertAlign, horzAlign,
// vertOffset, horzOffset, width, height, zOrder, flowWithText, allowOverlap)과 바깥 여백 marginLeft·marginRight·
// marginTop·marginBottom. 길이는 HWPUNIT. 크기가 없는 개체(머리말 등)면 빈 값이다.
FormatFields objectPropertiesView(const model::Document& doc, int story, int paragraph, int item);
// 위 이름 가운데 준 값만 고치고 문서를 다시 짠다. 크기는 손잡이 크기 조절과 같은 규칙(표는 칸·줄을 비율대로)이다.
// 모르는 이름·틀린 값이면 고치지 않고 error 에 이유를 적는다. 되돌리기 한 칸이다.
EditResult editObjectProperties(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                                const FormatFields& fields, bool useStoredLineHeight = false, std::string* error = nullptr);
// 개체를 지운다(표면 칸 글까지). 캐럿은 개체가 있던 자리로 간다. 되돌리기 한 칸이다.
EditResult editDeleteObject(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                            bool useStoredLineHeight = false, std::string* error = nullptr);

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
// GPU 파이프라인(그리기 조합마다 하나인 셰이더 묶음) 기록. 장치가 붙으면 화면이 쓸 조합을 미리 만든다.
// drawnFirst 는 미리 만들지 않았는데 그리면서 쓴 조합의 이름이다. 비어 있어야 첫 화면이 컴파일을 기다리지 않는다.
struct PipelineTally {
    int precompiled = 0;
    std::vector<std::string> drawnFirst;
};
PipelineTally pipelineTally();
// 그리다 만든 기록만 비운다. 미리 만든 파이프라인은 장치에 남는다.
void clearDrawnPipelines();
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
