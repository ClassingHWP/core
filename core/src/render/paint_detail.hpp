#pragma once

// paint.cpp 와 pointer_hold.cpp 가 함께 쓰는 그리기 내부 선언.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/glyph_path.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"

namespace chwp::render::paint_detail {

// 화면이 글자·그림·테두리를 칸 비트맵 대신 직접 그리는가. 셸 Host 값으로 켜고, GPU 를 못 쓰게 되면 꺼진다.
bool screenVectors();
void enableScreenVectors(bool on);
// GPU 화면이 실패해 2D 대체 화면으로 갔다(참), 화면을 다시 연다(거짓).
void noteScreenFailed(bool failed);

// ---------------------------------------------------------------------------
// paint.cpp

bool nextCp(std::string_view text, std::size_t& index, char32_t& out);

struct InlineBorder {
    bool ok = false;
    std::int32_t x = 0;
};

// 글자처럼 넣은 개체 테두리의 왼쪽. 줄 원점 기준이고, 글자 폭을 나중에 잰 펜·정렬과 같다.
bool treatAsCharBorders(const Node& line, const model::Document* doc, std::vector<InlineBorder>& border);

// ---------------------------------------------------------------------------
// pointer_hold.cpp. 누르고 끄는 동안의 캐럿·선택 상태

struct CachedPicture;

struct HeldBox {
    int page = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t pageW = 0;
    std::int32_t pageH = 0;
    bool invert = true;
    std::uint8_t backR = 255;
    std::uint8_t backG = 255;
    std::uint8_t backB = 255;
};

enum class DragMode { Text, Cells, Object };

struct DragBox {
    int page = -1;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;
};

// 드래그는 글자에서 칸으로, 칸에서 개체로만 올라간다. 한 번 올라가면 그 드래그 안에서는 내려가지 않는다.
struct DragHost {
    DragMode mode = DragMode::Text;
    const model::Control* control = nullptr;
    bool table = false;
    int row = -1;
    int col = -1;
    int rowSpan = 1;
    int colSpan = 1;
    int focusRow = -1;
    int focusCol = -1;
    int focusRowSpan = 1;
    int focusColSpan = 1;
    int parentStory = -1;
    int parentPara = -1;
    int parentOff = -1;
    DragBox objectBox;
};

struct PointerHold {
    bool tracking = false;
    int story = 0;
    int anchorPara = 0;
    int anchorOff = 0;
    int focusPara = 0;
    int focusOff = 0;
    DragHost drag;
    CaretPlace caret{};
    int caretPage = -1;
    std::int32_t caretPageW = 0;
    std::int32_t caretPageH = 0;
    int percent = 100;
    std::int32_t preferredX = -1;
    bool composing = false;
    int compStory = 0;
    int compPara = -1;
    int compStart = 0;
    int compEnd = 0;
    int compStartOff = 0;
    int compEndOff = 0;
    bool hasPendingCharShape = false;
    std::uint32_t pendingCharShapeId = 0;
    int pendingCharStory = 0;
    int pendingCharPara = -1;
    int pendingCharOffset = -1;
    std::int64_t caretTouchMs = 0;
    std::vector<HeldBox> boxes;
    std::vector<HeldBox> compUnderlines;
    std::string selectedText;
    std::string eventText;
    std::string property;
    int clickId = -1;
    int clickPage = -1;
    int clickZone = 0;
    int clickCount = 0;
    int clickDevice = 0;
    double clickDownTime = 0;
    double clickUpTime = 0;
    double clickDownX = 0;
    double clickDownY = 0;
    bool clickPending = false;
    bool clickDragged = false;
    const Node* selectedObject = nullptr;
    int editStory = 0;
    // 머리말·꼬리말을 고치는 중이면 그 글 묶음. 0 이면 본문이다. 나갈 때 bandReturn* 의 본문 자리로 커서를 돌린다.
    // 고치는 동안 띠 밖 누르기는 무시하고, Shift+Esc 로만 나간다.
    int bandStory = 0;
    bool bandHeader = false;
    int bandReturnStory = 0;
    int bandReturnPara = 0;
    int bandReturnOff = 0;
    bool pins = false;
    int pinDrag = 0;
    // 마커를 잡은 자리와, 손가락이 그보다 멀리 움직였는지. 안 움직이고 떼면 pinTapped 다.
    double pinDownX = 0;
    double pinDownY = 0;
    bool pinMoved = false;
    bool pinTapped = false;
    bool objectHandles = false;
    // 커서가 든 글상자·표에서 선이 없는 테두리. 한글처럼 빨간 점선으로 보인다. 좌표는 framePage 의 HWPUNIT.
    struct FrameLine {
        std::int32_t x0 = 0;
        std::int32_t y0 = 0;
        std::int32_t x1 = 0;
        std::int32_t y1 = 0;
    };
    std::vector<FrameLine> frameLines;
    int framePage = -1;
    // 머리말·꼬리말을 고치는 동안 쪽마다 그 띠의 칸(HWPUNIT). 띠 밖은 흐리게 덮는다. 띠가 없는 쪽은 너비 0 이다.
    struct BandArea {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
    };
    std::vector<BandArea> bandAreas;
    int resizeHandle = -1;
    // 도형 그리기. drawTool 이 0 이 아니면 다음 끌기 한 번으로 그 도형을 그린다(1 직사각형, 2 타원, 3 직선,
    // 4 글상자, 5 세로 글상자). 다 끌면 drawPending 이 켜지고 editInsertDrawnShape 가 문서에 넣는다.
    int drawTool = 0;
    int drawPendingTool = 0;
    // 고른 띄운 개체를 끌어 옮기는 중. 뗄 때 movePending 이 켜지고 editMoveObject 가 위치를 옮긴다.
    bool moving = false;
    bool movePending = false;
    int movePage = -1;
    std::int32_t moveStartX = 0;
    std::int32_t moveStartY = 0;
    std::int32_t moveDX = 0;
    std::int32_t moveDY = 0;
    int moveStory = -1;
    int moveParagraph = -1;
    int moveItem = -1;
    // 그림이 아닌 개체(표·도형·글상자)의 크기 조절. 뗄 때 resizeObjectPending 이 켜지고 editResizeObject 가 반영한다.
    // 표는 그린 줄 높이·칸 폭(resizeRows·resizeCols)을 비율대로 늘린다.
    bool resizeObjectPending = false;
    int resizeStory = -1;
    int resizeParagraph = -1;
    int resizeItem = -1;
    std::int32_t resizeNewLeft = 0;
    std::int32_t resizeNewTop = 0;
    std::vector<std::int32_t> resizeRows;
    std::vector<std::int32_t> resizeCols;
    // 표 칸 경계 끌기. 1 세로선(칸 폭), 2 가로선(줄 높이). borderIndex 는 그 선 왼쪽(위) 칸·줄 번호다.
    // 뗄 때 borderPending 이 켜지고 editResizeTableBorder 가 칸 폭·줄 높이를 바꾼다. 안 끌고 떼면 표를 고른다.
    int borderDrag = 0;
    bool borderPending = false;
    bool borderMoved = false;
    int borderPage = -1;
    int borderIndex = -1;
    bool borderOuter = false;  // 오른쪽·아래 바깥선이면 표 폭·높이가 바뀐다
    std::int32_t borderStart = 0;
    std::int32_t borderDelta = 0;
    std::int32_t borderMin = 0;  // 끌 수 있는 범위(이웃 칸이 너무 좁아지지 않게)
    std::int32_t borderMax = 0;
    std::int32_t borderX0 = 0;  // 미리보기 선을 그릴 표 상자(쪽 좌표)
    std::int32_t borderY0 = 0;
    std::int32_t borderX1 = 0;
    std::int32_t borderY1 = 0;
    std::int32_t borderAt = 0;  // 처음 선 자리(세로선이면 x, 가로선이면 y)
    int borderStory = -1;
    int borderParagraph = -1;
    int borderItem = -1;
    std::vector<std::int32_t> borderSizes;  // 그린 칸 폭(세로선) 또는 줄 높이(가로선)
    // 개체를 끌어 옮기거나 크기를 바꾸는 동안 보일 그 개체 모습. 누를 때 그 자리를 그려 둔다.
    std::shared_ptr<const CachedPicture> dragSnapshot;
    std::string dragSnapshotId;
    bool drawing = false;
    bool drawPending = false;
    int drawPage = -1;
    std::int32_t drawX0 = 0;
    std::int32_t drawY0 = 0;
    std::int32_t drawX1 = 0;
    std::int32_t drawY1 = 0;
    int resizePage = -1;
    std::int32_t resizeLeft = 0;
    std::int32_t resizeTop = 0;
    std::int32_t resizeRight = 0;
    std::int32_t resizeBottom = 0;
    std::uint32_t resizeOldW = 0;
    std::uint32_t resizeOldH = 0;
    std::uint32_t resizeNewW = 0;
    std::uint32_t resizeNewH = 0;
    std::string resizeRole;
    std::string resizeBin;
    bool resizeCrop = false;
    std::int32_t resizeClipL = 0;
    std::int32_t resizeClipT = 0;
    std::int32_t resizeClipR = 0;
    std::int32_t resizeClipB = 0;
    std::int32_t resizeOriginW = 0;
    std::int32_t resizeOriginH = 0;
    bool resizeCommit = false;
    CaretPlace pinStart{};
    CaretPlace pinEnd{};
    bool edgeActive = false;
    double edgeViewX = 0;
    double edgeViewY = 0;
    double edgeDpr = 1;
    int pendingPage = -1;
    int pendingX = 0;
    int pendingY = 0;
};

void bumpPointerPicture();
void touchCaretBlink(PointerHold& hold);
// 지금 편집 상태(EditState)의 커서·블록·조합. 아무 상태도 잡지 않았으면 프로세스 기본 상태다.
PointerHold& pointerHold();
int currentCaretPageHint();
void clearPendingCharShape(PointerHold& hold);
// 글자 모양만 바꾸고 아직 글자를 넣지 않은 캐럿 자리의 모양.
std::optional<std::uint32_t> pendingCharShapeForCaret(int story, int paragraph, int offset);
void clearComposition(PointerHold& hold);
void placeCaret(PointerHold& hold, const CaretPlace& place, int page, const std::vector<Node>& pages,
                const model::Document* doc = nullptr);
void rememberSelection(const model::Document& doc, const std::vector<Node>& pages);

// ---------------------------------------------------------------------------
// edit_commands.cpp. 화면 글자 위치와 모델 글자 번호를 서로 바꾼다

int offsetToCharIndex(const model::Document& doc, const std::vector<Node>& pages, int story, int paraIdx, int offset);
std::uint32_t charShapeAtParagraphChar(const model::Paragraph& para, int charIndex, bool preferRight);
// 글자 한 칸을 차지하는 컨트롤. 글자처럼 취급한 개체와 각주·미주 번호다. 문단 글자 번호는 이 칸을 하나로 센다.
bool isCaretCharControl(const model::Control& ctrl);
// 문단 글자 수. 글자 코드 포인트와 위 컨트롤 칸의 합이다.
int paragraphCharCount(const model::Paragraph& para);
bool parseHexColorRgb(const std::string& text, std::uint32_t& outRgb);
// 줄에서 글자가 실제로 놓인 가로 범위 안(slack 만큼 넓혀)인가. 줄 상자가 넓어도 빈 곳은 글이 아니다.
// lineX·lineY 는 줄 노드의 자리다. 글자 자리는 그리기·캐럿과 같은 placeLine 으로 잰다.
bool lineInkHas(const model::Document& doc, const Node& line, std::int32_t lineX, std::int32_t lineY, std::int32_t px,
                std::int32_t slack);

// ---------------------------------------------------------------------------
// format_fields.cpp. 대화 상자 값(FormatPatch::fields)을 글자 모양·문단 모양에 넣는다

bool formatFieldsTouchChar(const FormatFields& fields);
bool formatFieldsTouchPara(const FormatFields& fields);
bool isTabKey(const std::string& key);
void applyCharFields(model::CharShape& shape, const FormatFields& fields);
void applyParaFields(model::ParaShape& shape, const FormatFields& fields);
// 테두리/배경("char." · "para." 이름). 바뀐 값의 테두리/배경을 문서에서 찾아 쓰거나 새로 넣고 그 번호를 붙인다.
void applyCharFillFields(model::Document& doc, model::CharShape& shape, const FormatFields& fields);
void applyParaFillFields(model::Document& doc, model::ParaShape& shape, const FormatFields& fields);
// 문단 종류(headingType)를 바꾸면 그 종류가 가리킬 번호·글머리표 정의를 고르거나 만든다.
void applyParaHeadingFields(model::Document& doc, model::ParaShape& shape, const FormatFields& fields);
// 막힌 글꼴 때문에 남겨 둔 개체 넣기를 이어서 한다. 남은 것이 없으면 문서를 바꾸지 않은 결과다.
EditResult resumePendingInsert(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
// 그린 도형만 버린다. dropPendingInsert 가 부른다.
void dropPendingDrawnShape();
// 탭 설정(tabStops · tabAutoLeft · tabAutoRight). 같은 탭 정의를 찾아 쓰거나 새로 넣는다.
void applyParaTabFields(model::Document& doc, model::ParaShape& shape, const FormatFields& fields);
// 구역 기본 탭 간격(sectionDefaultTab). 있으면 그 값을 넣고 참을 돌려준다.
bool sectionDefaultTabOf(const FormatFields& fields, std::int32_t& value);
// 탭 목록 글("위치,종류,채움;...", 위치는 HWPUNIT)과 목록 사이를 오간다. 위치 차례로 두고 같은 위치는 나중 것만 남긴다.
std::vector<model::TabStop> parseTabStops(const std::string& text);
std::string tabStopsTextOf(const std::vector<model::TabStop>& stops);
// 글자·문단 모양 번호로 대화 상자 값을 읽는다. sectionDefaultTab 이 0 이면 넣지 않는다.
FormatFields formatFieldsOf(const model::Document& doc, const CursorStyle& st, std::int32_t sectionDefaultTab);
// 패치를 적용한 글자·문단 모양 번호. 같은 모양이 있으면 그 번호, 없으면 새로 넣은 번호다.
std::uint32_t deriveCharShapeId(model::Document& doc, std::uint32_t baseId, const FormatPatch& patch);
std::uint32_t deriveParaShapeId(model::Document& doc, std::uint32_t baseId, const FormatPatch& patch);

// ---------------------------------------------------------------------------
// edit_commands.cpp. 고친 자리를 다시 놓고 편집 결과를 만든다. 되돌리기(undo.cpp)도 쓴다

int charIndexToOffset(const model::Document& doc, const std::vector<Node>& pages, int story, int paraIdx, int charIndex);
EditRelayoutResult relayoutAfterEdit(model::Document& doc, std::vector<Node>& pages, const EditRelayoutSpec& spec);
EditResult makeEditResult(const model::Document& doc, bool handled, bool changed, int story, int paraIdx,
                          const EditRelayoutResult* relayout = nullptr);

// 다시 짠 쪽에서 붙은 문단·zOrder 로 그 개체를 찾아 다시 고른다. 없으면 거짓이다.
bool reselectAnchoredObject(const model::Document& doc, const std::vector<Node>& pages, int page, int story,
                            int paragraph, int item);
// 개체 노드가 가리키는 문서의 컨트롤. 없으면 nullptr 이다.
model::Control* anchoredControl(model::Document& doc, int story, int paragraph, int item);
// 개체 하나의 크기를 바꾼다(점·글상자 폭·캡션 폭을 같이). 표는 따로 칸을 늘린다.
void resizeControl(model::Control& ctrl, std::uint32_t oldW, std::uint32_t oldH, std::uint32_t newW,
                   std::uint32_t newH);

// 글자 외곽선(글꼴 단위 1000 = emPx)을 쪽 그림과 같은 Painter 로 width x height 판에 칠한 알파. 시험이 쓴다.
std::vector<std::uint8_t> glyphAlpha(const std::vector<GlyphCommand>& commands, float emPx, float penX, float baseY,
                                     int width, int height);

}  // namespace chwp::render::paint_detail

namespace chwp::render {

// 편집 명령 본체. 되돌리기 칸은 남기지 않는다. 공개 함수(undo.cpp)가 앞뒤로 되돌리기 칸을 남긴다.
// editSwapLicenseFonts 의 본체. 되돌리기 기록은 undo.cpp 가 감싼다.
EditResult swapLicenseFontsNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                               const std::vector<std::pair<std::string, std::string>>& chosen);
EditResult insertTextNow(model::Document& doc, std::vector<Node>& pages, const std::string& text, bool composing,
                         bool useStoredLineHeight);
EditResult keyDownNow(model::Document& doc, std::vector<Node>& pages, EditKey key, bool shift, bool ctrl, bool alt,
                      bool useStoredLineHeight);
EditResult applyFormatNow(model::Document& doc, std::vector<Node>& pages, const FormatPatch& patch,
                          bool useStoredLineHeight);
// 칸 블록이 골라져 있으면 그 표를 품은 바깥 문단. 되돌리기는 이 문단을 베껴 칸 전부를 함께 되돌린다.
bool cellBlockHost(int& story, int& paragraph);
// 칸 블록의 칸마다 문단 전체에 서식을 건다(한글 칸 블록 + 글자·문단 모양). 블록은 그대로 둔다.
EditResult applyFormatToCellsNow(model::Document& doc, std::vector<Node>& pages, const FormatPatch& patch,
                                 bool useStoredLineHeight);
EditResult applyPageSetupNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                             bool wholeDocument, bool useStoredLineHeight);
EditResult insertTableNow(model::Document& doc, std::vector<Node>& pages, int rows, int cols, bool useStoredLineHeight);
EditResult insertPictureNow(model::Document& doc, std::vector<Node>& pages, const Bytes& image, bool useStoredLineHeight);
EditResult insertShapeNow(model::Document& doc, std::vector<Node>& pages, int tool, int page, std::int32_t x0,
                          std::int32_t y0, std::int32_t x1, std::int32_t y1, bool useStoredLineHeight);
EditResult insertDrawnShapeNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
EditResult moveObjectNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
EditResult resizeObjectNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
EditResult resizeTableBorderNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
// 도형을 붙일 문단(쪽 page 의 y 자리). 되돌리기 칸을 그 문단 둘레로 잡을 때 쓴다.
bool shapeAnchor(const model::Document& doc, const std::vector<Node>& pages, int page, std::int32_t y, int& story,
                 int& paragraph);

}  // namespace chwp::render
