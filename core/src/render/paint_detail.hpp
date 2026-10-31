#pragma once

// paint.cpp 와 pointer_hold.cpp 가 함께 쓰는 그리기 내부 선언.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "noting/model/document.hpp"
#include "noting/render/paint.hpp"
#include "noting/render/scene.hpp"

namespace noting::render::paint_detail {

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
void applyCharFields(model::CharShape& shape, const FormatFields& fields);
void applyParaFields(model::ParaShape& shape, const FormatFields& fields);

}  // namespace noting::render::paint_detail
