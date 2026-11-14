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

namespace chwp::render {
struct StyleDraft;
struct TocOptions;
}

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

// 하이퍼링크로 옮긴 한 번(이전·다음 이력). 자리는 글 묶음·문단·글자 번호다.
struct LinkJump {
    int fromStory = 0;
    int fromPara = 0;
    int fromChar = 0;
    int toStory = 0;
    int toPara = 0;
    int toChar = 0;
};

// 점 아래 하이퍼링크 글자(pointer_hold.cpp). 링크 안 글자 번호다.
struct LinkSpot {
    bool found = false;
    int story = -1;
    int paragraph = -1;
    int charIndex = -1;
};

// 숨긴 누름틀 안내문(한글 2024: 블록 없는 캐럿이 손대지 않은 누름틀 안에 있는 동안 안내문을 감추고 뒤 글을 당긴다).
// 문단에서 안내문을 빼 두고, 캐럿이 나가면 숨기기 전 문단(줄 정보까지)으로 되돌린다. story 가 -1 이면 없다.
struct HiddenClickGuide {
    int story = -1;
    int para = -1;
    int openPos = 0;     // 누름틀 시작 글자 번호
    int chars = 0;       // 뺀 안내문 글자 수
    std::string text;    // 뺀 안내문
    std::uint32_t shape = 0;  // 안내문 글자 모양
    bool useStoredLineHeight = false;
    std::optional<model::Paragraph> before;  // 숨기기 전 문단
    std::optional<model::Paragraph> hidden;  // 숨긴 뒤 문단(그 사이 다른 편집이 고쳤는지 견준다)
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
    // 칸 사이를 가로로 옮길 때 지나는 줄. 여러 줄을 합친 칸을 지나도 같은 줄을 따라가게 한다(그 표·그 줄을 덮는 칸에서만 쓴다).
    const model::Control* cellRowTable = nullptr;
    int cellRow = -1;
    bool composing = false;
    int compStory = 0;
    int compPara = -1;
    int compStart = 0;
    int compEnd = 0;
    int compStartOff = 0;
    int compEndOff = 0;
    bool hasPendingCharShape = false;
    std::uint32_t pendingCharShapeId = 0;
    // 캐럿이 누름틀 "안" 자리다(누름틀 앞과 글자 번호가 같아 따로 든다). clickHereStory 가 -1 이면 없다.
    int clickHereStory = -1;
    int clickHerePara = -1;
    int clickHereChar = -1;
    HiddenClickGuide hiddenGuide;
    // 걸어 둔 글자 모양과 함께 친 글에 걸 글자 스타일. 없으면 kNoCharStyle 이다.
    std::uint32_t pendingCharStyleId = 0xFFFFFFFFu;
    // 하이퍼링크 이력. linkJumpAt 앞은 이전으로 돌아갈 수 있고, 뒤는 다음으로 다시 갈 수 있다.
    std::vector<LinkJump> linkJumps;
    std::size_t linkJumpAt = 0;
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
    // bandStory 가 바탕쪽 글이다(바탕쪽 편집). 본문·머리말을 감추고 바탕쪽만 그리며, 띠 덮개는 없다.
    bool bandMaster = false;
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
    // 두 번 눌러 고를 덧말·글자 겹치기의 고치기 대화 상자(rubyEdit·composeEdit). 셸이 한 번 가져간다.
    std::string openDialog;
    // 메모 필드 시작·끝 문자 자리(한글은 캐럿이 한 칸씩 멈춘다). 같은 글자 번호에 놓인 메모 시작·끝 사이 어디에 캐럿이
    // 있는지(앞에서 지난 표시 수). 그 글자 자리(story·문단·글자 번호)에서만 쓴다. 다른 자리면 기본값(memoMarkerSlot)이다.
    int markerSlot = 0;
    int markerStory = -1;
    int markerPara = -1;
    int markerChar = -1;
    int compMarkerSlot = -1;  // 조합 중인 글을 넣은 표시 자리. 조합 글을 다시 넣을 때 같은 자리에 넣는다
    // 지우기가 메모 시작·끝 표시에 닿아 지울 메모. 그 캐럿 자리에서만 메모 지우기가 이 메모를 고른다.
    const model::Control* pendingMemo = nullptr;
    int pendingMemoStory = -1;
    int pendingMemoPara = -1;
    int pendingMemoOffset = -1;
    // 모양 복사(한글 ShapeCopyPaste, Alt+C)로 기억한 모양. type 0 글자 모양, 1 문단 모양, 2 둘 다, 4 문단 스타일.
    struct CopiedShape {
        bool valid = false;
        int type = 0;
        std::uint32_t charShape = 0;
        std::uint32_t paraShape = 0;
        std::uint32_t style = 0;
    } copiedShape;
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
    // 4 글상자, 5 세로 글상자, 6 호, 9 자유선). 7 다각형·8 곡선은 누를 때마다 점을 더하고 두 번 누르면 끝난다
    // (한글 2024 도형 갤러리와 같다). 다 그리면 drawPending 이 켜지고 editInsertDrawnShape 가 문서에 넣는다.
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
    // 여러 개체 고르기(한글 2024: Shift 를 누른 채 개체를 누르면 더하거나 뺀다). selectedObject 가 첫 개체이고, 여기는
    // 함께 고른 다른 개체의 자리(글 묶음·문단·항목 순번)와 쪽 상자다. 끌면 같이 옮기고 Delete 로 같이 지운다.
    struct PickedObject {
        int page = -1;
        int story = -1;
        int paragraph = -1;
        int item = -1;
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
    };
    std::vector<PickedObject> multiPicked;
    bool shiftDown = false;  // 지금 누른 포인터가 Shift 와 함께인지(셸이 준 값)
    bool drawing = false;
    bool drawPending = false;
    std::vector<model::ShapePoint> drawPoints;  // 다각형·곡선·자유선의 점(쪽 좌표)
    double drawLastDown = 0;                    // 다각형·곡선: 마지막으로 누른 때(두 번 누르기를 가린다)
    int drawPage = -1;
    std::int32_t drawX0 = 0;
    std::int32_t drawY0 = 0;
    std::int32_t drawX1 = 0;
    std::int32_t drawY1 = 0;
    // 그리기 안내선을 끄는 중이다. 1 세로선(가로 자리를 옮김), 2 가로선(세로 자리를 옮김).
    int guideDrag = 0;
    // 옮기는 개체가 그리기 안내선에 붙었다. 붙은 안내선은 실선으로 그린다.
    bool guideSnapX = false;
    bool guideSnapY = false;
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
// 바탕쪽 편집 화면을 켜고 끈다(masterView). 바뀌면 masterViewEpoch 가 오른다.
void setMasterView(bool on);
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
// 그 자리 글의 글자 스타일 번호. 없으면 model::kNoCharStyle 이다.
std::uint32_t charStyleAtParagraphChar(const model::Paragraph& para, int charIndex, bool preferRight);
// 글자 한 칸을 차지하는 컨트롤. 글자처럼 취급한 개체와 각주·미주 번호다. 문단 글자 번호는 이 칸을 하나로 센다.
bool isCaretCharControl(const model::Control& ctrl);
// 문단 글자 번호 charIndex 에 놓인 메모 필드 시작·끝 표시(글 차례). 메모 시작은 begin, 짝인 끝은 !begin 이고
// memo 는 그 메모 시작 컨트롤이다. 한글은 이 표시마다 캐럿이 한 칸 멈춘다(숨긴 한글 GetPos 로 확인).
struct MemoMarker {
    const model::Control* control = nullptr;
    const model::Control* memo = nullptr;
    bool begin = false;
};
std::vector<MemoMarker> memoMarkersAt(const model::Paragraph& para, int charIndex);
// 캐럿이 그 자리 표시 몇 개 뒤에 있는가. 정한 적 없는 자리면 앞쪽 끝 표시를 다 지난 자리(모든 메모 밖)다.
int memoMarkerSlot(const PointerHold& hold, int story, int paragraph, int charIndex, const std::vector<MemoMarker>& markers);
void setMemoMarkerSlot(PointerHold& hold, int story, int paragraph, int charIndex, int slot);
// 표시 앞(after 거짓)이나 뒤에 글을 넣는다. 넣은 글자 수를 돌려준다.
int insertTextAtMemoMarker(model::Paragraph& para, const model::Control* marker, bool after, const std::string& text);
// 지우기(Delete·Backspace)가 캐럿 자리 메모 표시에 닿으면 그 메모. 닿지 않으면 nullptr 이다.
const model::Control* memoMarkerToErase(const model::Document& doc, const std::vector<Node>& pages, EditKey key);
// 캐럿 자리 메모(pendingMemo 가 있으면 그것)를 지운다. 본문 글은 남는다(page_commands.cpp).
EditResult deleteMemoNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
// 막힌 글꼴이 남은 문서면 고치지 않고 out 에 글꼴 바꾸기를 묻는 결과를 둔다(edit_commands.cpp).
bool editBlockedByLicenseFonts(const model::Document& doc, EditResult& out);
// 개체 안 글을 고쳤을 때 그 개체를 품은 바깥 문단들의 lineSeg 를 버린다.
void dropParentLineSegsOf(model::Document& doc, int story);
// 캐럿 글자 번호마다의 글자. 글자처럼 놓인 개체는 0x0B, 각주·미주는 0x11 이다.
std::u32string caretCharsOf(const model::Paragraph& para);
// 글 코드 포인트 수와 코드 포인트 범위 자르기.
int utf8CodePointCount(const std::string& text);
// 고른 범위를 지우고 캐럿이 갈 문단·글자 번호를 준다.
bool deleteSelectionRange(model::Document& doc, const std::vector<Node>& pages, int story, int startPara, int startOff,
                          int endPara, int endOff, int& outPara, int& outCharIndex);
// 문단을 글자 번호에서 가른다. 앞은 cur 에 남고 뒤 문단을 돌려준다.
model::Paragraph splitParagraphAt(model::Paragraph& cur, int splitCharIndex);
std::string utf8SliceCodePoints(const std::string& text, int startCp, int countCp);
// 문단 글자 [from, to) 를 replacement 로 바꾼다. i 번째 새 글자는 i 번째 옛 글자의 자리(같은 run·item)에 들고,
// 남는 새 글자는 마지막 옛 글자 뒤에 붙는다. 옛 글자가 남으면 지운다. 바꾼 뒤 끝 글자 번호를 돌려준다.
int replaceCharsInParagraph(model::Paragraph& para, int from, int to, const std::string& replacement);
// find_text.cpp. 찾기와 같은 대소문자 규칙으로 두 글이 같은가.
bool searchTextEqual(const std::string& a, const std::string& b, bool matchCase);
// 문단 글자 수. 글자 코드 포인트와 위 컨트롤 칸의 합이다.
int paragraphCharCount(const model::Paragraph& para);
// 이 줄 문단에 그릴 교정 부호가 있는가(교정 부호 보기가 켜져 있을 때). 있으면 줄 칸 비트맵을 그린다.
bool lineHasProofInk(const Node& line, const model::Document* doc);
// 이 줄 문단에 메모 범위(%%me)가 있는가(메모 보이기가 켜져 있을 때). 있으면 줄 칸 비트맵에 범위 상자를 그린다.
bool lineHasMemoRange(const Node& line, const model::Document* doc);
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
// 언어별 글꼴(font.HANGUL 등)을 문서 글꼴 표에 넣고 그 언어 칸에 건다.
void applyCharLangFonts(model::Document& doc, model::CharShape& shape, const FormatFields& fields);
void applyParaFields(model::ParaShape& shape, const FormatFields& fields);
// 테두리/배경("char." · "para." 이름). 바뀐 값의 테두리/배경을 문서에서 찾아 쓰거나 새로 넣고 그 번호를 붙인다.
void applyCharFillFields(model::Document& doc, model::CharShape& shape, const FormatFields& fields);
void applyParaFillFields(model::Document& doc, model::ParaShape& shape, const FormatFields& fields);
// 테두리/배경 번호로 읽기(없으면 빈 테두리/배경)와, 같은 것이 있으면 그 번호·없으면 새로 넣은 번호.
model::BorderFill borderFillOf(const model::Document& doc, std::uint32_t id);
std::uint32_t internBorderFillOf(model::Document& doc, const model::BorderFill& fill);
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
void appendCharShapeFields(const model::Document& doc, const model::CharShape& cs, FormatFields& out);
void appendParaShapeFields(const model::Document& doc, const model::ParaShape& ps, FormatFields& out);
// 테두리/배경 값을 prefix("char." 등)를 붙인 이름으로.
void readFillFieldsOf(const model::Document& doc, std::uint32_t id, const char* prefix, FormatFields& out);
// 접두(prefix) 붙은 테두리/배경 값을 current 테두리/배경에 얹어 그 번호를 돌려준다. 고칠 값이 없으면 그대로다.
std::uint32_t applyFillFieldsOf(model::Document& doc, std::uint32_t current, const FormatFields& fields, const char* prefix);
// 쪽 테두리/배경 대화 상자의 값(캐럿 구역, which 는 BOTH·ODD·EVEN). 이름은 page_commands.cpp 의 pageBorderNow 와 같다.
FormatFields pageBorderValues(const model::Document& doc, const std::string& which);
EditResult pageBorderNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, bool wholeDocument,
                         bool useStoredLineHeight, std::string& why);
// 패치를 적용한 글자·문단 모양 번호. 같은 모양이 있으면 그 번호, 없으면 새로 넣은 번호다.
std::uint32_t deriveCharShapeId(model::Document& doc, std::uint32_t baseId, const FormatPatch& patch);
// boxWidth 는 여백 한 단계(marginLeftStep·marginRightStep)의 끝값을 정하는 문단 폭이다. 그 밖에는 쓰지 않는다.
std::uint32_t deriveParaShapeId(model::Document& doc, std::uint32_t baseId, const FormatPatch& patch,
                                std::int32_t boxWidth = 0);
// 같은 모양이 있으면 그 번호, 없으면 새로 넣은 번호다.
std::uint32_t internParaShape(model::Document& doc, model::ParaShape next);
std::uint32_t internCharShape(model::Document& doc, model::CharShape next);
// 문단 스타일 was → now 로 바꿀 때 글자·문단 모양. 옛 스타일과 다르게 직접 준 속성만 남긴다. was 가 없으면 스타일
// 모양 그대로다(같은 스타일을 다시 입힐 때).
std::uint32_t restyleCharShapeId(model::Document& doc, std::uint32_t curId, const model::Style* was,
                                 const model::Style& now);
std::uint32_t restyleParaShapeId(model::Document& doc, std::uint32_t curId, const model::Style* was,
                                 const model::Style& now);
// 문단을 was 에서 now 로 바꾼다(문단 모양·글자 모양·스타일 번호, 줄 나눔 값은 비운다).
void restyleParagraph(model::Document& doc, model::Paragraph& para, const model::Style* was, const model::Style& now);
// 문단 스타일 styleId 에서 now 로 갈 때의 옛 스타일. 같은 스타일이거나 글자 스타일이면 nullptr(스타일 모양 그대로)이다.
const model::Style* restyleFrom(const model::Document& doc, std::uint32_t styleId, const model::Style& now);
// style_commands.cpp. 스타일 표 명령(되돌리기 없이).
EditResult commitStyleNow(model::Document& doc, std::vector<Node>& pages, const StyleDraft& draft,
                          bool useStoredLineHeight, std::string& why);
EditResult deleteStyleNow(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId, std::uint32_t replacement,
                          bool useStoredLineHeight, std::string& why);
EditResult moveStyleNow(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId, int delta,
                        bool useStoredLineHeight, std::string& why);
EditResult styleFromCaretNow(model::Document& doc, std::vector<Node>& pages, std::uint32_t styleId,
                             bool useStoredLineHeight, std::string& why);
EditResult clearCharStyleNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight, std::string& why);
// 변경 내용 추적(track_commands.cpp). 기록 중 Delete·Backspace 를 지운 글 표시로 바꾼다. 했으면 참이다.
bool trackDeleteNow(model::Document& doc, std::vector<Node>& pages, EditKey key, bool useStoredLineHeight, EditResult& out);
// 친 글(paraFrom:charFrom ~ 캐럿)을 기록 중이면 넣은 글로, 아니면 표시 없는 글로 둔다. splitParagraphs 면 사이 문단 끝도.
void trackMarkInserted(model::Document& doc, std::vector<Node>& pages, int story, int paraFrom, int charFrom,
                       bool splitParagraphs);
void mergeParagraphs(model::Paragraph& dst, const model::Paragraph& src);
// 하이퍼링크(link_commands.cpp, pointer_hold.cpp, edit_commands.cpp).
LinkSpot linkSpotAt(const model::Document& doc, const std::vector<Node>& pages, int page, int x, int y);
// para 의 글자 번호 charIndex 를 감싼 하이퍼링크 필드. 없으면 nullptr 이고, 있으면 begin·end 는 링크 글자 구간이다.
model::Control* hyperlinkAround(model::Paragraph& para, int charIndex, int& begin, int& end);
// 링크 글 [startChar, endChar) 를 방문한 색(#800080 글자·밑줄)으로. 바뀌었으면 참이다.
bool markLinkVisited(model::Document& doc, model::Paragraph& para, int startChar, int endChar);
EditResult followLinkNow(model::Document& doc, std::vector<Node>& pages, int page, int x, int y, bool useStoredLineHeight,
                         std::string& url, std::string& why);
EditResult importStylesNow(model::Document& doc, std::vector<Node>& pages, const model::Document& source,
                           const std::vector<std::string>& names, bool useStoredLineHeight, std::string& why);
// toc_commands.cpp. 차례 만들기·새로 고침(되돌리기 없이).
EditResult makeTocNow(model::Document& doc, std::vector<Node>& pages, const TocOptions& options,
                      bool useStoredLineHeight, std::string& why);
EditResult updateTocNow(model::Document& doc, std::vector<Node>& pages, bool all, bool useStoredLineHeight,
                        std::string& why);
// 문단에 문단 스타일을 입힌다(한글 스타일 적용: 직접 준 문단·글자 속성은 남고, 같은 스타일이면 모두 지운다).
void applyParaStyle(model::Document& doc, model::Paragraph& para, const model::Style& st);

// ---------------------------------------------------------------------------
// edit_commands.cpp. 고친 자리를 다시 놓고 편집 결과를 만든다. 되돌리기(undo.cpp)도 쓴다

int charIndexToOffset(const model::Document& doc, const std::vector<Node>& pages, int story, int paraIdx, int charIndex);
EditRelayoutResult relayoutAfterEdit(model::Document& doc, std::vector<Node>& pages, const EditRelayoutSpec& spec);
// 개체를 담은 문단부터 다시 놓는다. 글자처럼 놓인 개체의 크기·모양이 바뀌었을 때 쓴다.
EditRelayoutResult relayoutObjectParagraph(model::Document& doc, std::vector<Node>& pages, int story, int paragraph,
                                           bool useStoredLineHeight);
// table_commands.cpp. 표 구조 편집. 고치지 못하면 error 에 이유를 적고 문서를 그대로 둔다.
// 캡션 넣기·옮기기·떼기(caption: TOP·BOTTOM·LEFT_*·RIGHT_*·NONE). 표·그림·도형·수식이 같은 규칙이다.
enum class CaptionChange { Refused, Same, Changed };
CaptionChange setObjectCaption(model::Document& doc, model::Control& control, const std::string& caption, int hostStory,
                               int hostParagraph, bool& created, std::string& error);
EditResult objectCaptionNow(model::Document& doc, std::vector<Node>& pages, const std::string& caption,
                            bool useStoredLineHeight, std::string& why);
EditResult tableEditNow(model::Document& doc, std::vector<Node>& pages, const TableEditCommand& cmd,
                        bool useStoredLineHeight, std::string& error);
// formula_commands.cpp. 칸(cell, 그 story 는 cellStory)에 계산식을 넣는다: 캐럿이 그 칸에 있으면 캐럿 자리, 아니면 칸
// 맨 앞. 캐럿이 계산식 결과 안이면 그 식을 고친다. 하지 못하면 error.
bool placeTableFormula(model::Document& doc, const std::vector<Node>& pages, model::Table& table, int cell, int cellStory,
                       const std::string& formula, const std::string& format, std::string& error);

// 캐럿을 문단 고유 번호와 글자 번호로 들고 있다가, 글 묶음 번호·글자 자리가 바뀐 뒤에 되돌린다(page_commands.cpp).
struct HeldCaret {
    bool active = false;
    std::uint32_t anchorId = 0;
    std::uint32_t focusId = 0;
    int anchorChar = 0;
    int focusChar = 0;
};
HeldCaret holdCaret(model::Document& doc, const std::vector<Node>& pages);
void placeHeldCaret(const model::Document& doc, const std::vector<Node>& pages, const HeldCaret& held);
// 문서 전체를 다시 짠다. 머리말·쪽 번호는 쪽마다 붙으니 고친 문단만 다시 놓아서는 안 된다.
EditRelayoutResult relayoutWhole(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);

// ---- 편집 마무리 알림(edit_events.cpp) ----
// 편집 명령 하나가 문서를 고친 범위. 되돌리기 기록(UndoTrack)이 편집 앞뒤를 견줘 만든다.
//   whole: 어디가 바뀌었는지 모른다(구역 기록·되돌리기·다시 하기). 문서 전체를 바뀐 것으로 본다.
//   story·first·count: 그 글 묶음의 [first, first + count) 문단이 바뀌었다(count 0 이면 문단만 빠졌다).
struct EditChange {
    bool whole = false;
    int story = -1;
    int first = 0;
    int count = 0;
};
// 바뀐 범위를 듣는 쪽(계산식 다시 계산 등)에 알리고, 듣는 쪽이 문서를 더 고쳤으면 그 자리를 다시 짜서 결과를 합친다.
// 글자 조합 중이거나 문서가 바뀌지 않았으면 result 그대로다.
// changedMore 가 있으면 듣는 쪽이 문서를 더 고쳤는지 적는다.
EditResult afterEditCommitted(model::Document& doc, std::vector<Node>& pages, const EditResult& result,
                              const EditChange& change, bool useStoredLineHeight, bool* changedMore = nullptr);
EditResult makeEditResult(const model::Document& doc, bool handled, bool changed, int story, int paraIdx,
                          const EditRelayoutResult* relayout = nullptr);

// object_commands.cpp. 이름으로 가리킨 개체의 속성·지우기(되돌리기 없이).
EditResult objectPropertiesNow(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                               const FormatFields& fields, bool useStoredLineHeight, std::string& why);
EditResult insertEquationNow(model::Document& doc, std::vector<Node>& pages, const std::string& script,
                             std::int32_t baseUnit, bool useStoredLineHeight, std::string& why);
EditResult editEquationNow(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                           const std::string& script, std::int32_t baseUnit, bool useStoredLineHeight, std::string& why);
EditResult deleteObjectNow(model::Document& doc, std::vector<Node>& pages, int story, int paragraph, int item,
                           bool useStoredLineHeight, std::string& why);

// 다시 짠 쪽에서 붙은 문단·zOrder 로 그 개체를 찾아 다시 고른다. 없으면 거짓이다.
bool pickAnchoredObject(const model::Document& doc, const std::vector<Node>& pages, int page, int story, int paragraph,
                        int item);
// 고른 개체(첫 개체와 여러 개체 고르기로 더한 개체)를 모두 지운다. 고른 개체가 없으면 handled 가 거짓이다.
EditResult deleteSelectedObjectsNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
bool reselectAnchoredObject(const model::Document& doc, const std::vector<Node>& pages, int page, int story,
                            int paragraph, int item);
// 개체 노드가 가리키는 문서의 컨트롤. 없으면 nullptr 이다.
model::Control* anchoredControl(model::Document& doc, int story, int paragraph, int item);
// 개체 하나의 크기를 바꾼다(점·글상자 폭·캡션 폭을 같이). 표는 따로 칸을 늘린다.
void resizeControl(model::Control& ctrl, std::uint32_t oldW, std::uint32_t oldH, std::uint32_t newW,
                   std::uint32_t newH);

// 이름으로 고른 글꼴 파일에서 글자 하나의 윤곽(1000 em, 기울임·굵기 없음). 글꼴이나 글자가 없으면 거짓.
bool faceGlyphCommands(const std::string& face, char32_t cp, std::vector<GlyphCommand>& commands);

// 글자 외곽선(글꼴 단위 1000 = emPx)을 쪽 그림과 같은 Painter 로 width x height 판에 칠한 알파. 시험이 쓴다.
std::vector<std::uint8_t> glyphAlpha(const std::vector<GlyphCommand>& commands, float emPx, float penX, float baseY,
                                     int width, int height);

}  // namespace chwp::render::paint_detail

namespace chwp::render {

// 커서가 있는 구역. 커서가 없으면 첫 구역이다.
int cursorSection(const model::Document& doc);

// 편집 명령 본체. 되돌리기 칸은 남기지 않는다. 공개 함수(undo.cpp)가 앞뒤로 되돌리기 칸을 남긴다.
// editSwapLicenseFonts 의 본체. 되돌리기 기록은 undo.cpp 가 감싼다.
EditResult swapLicenseFontsNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                               const std::vector<std::pair<std::string, std::string>>& chosen);
EditResult insertTextNow(model::Document& doc, std::vector<Node>& pages, const std::string& text, bool composing,
                         bool useStoredLineHeight);
// 표 칸 문단의 1,000 단위 쉼표 넣기·빼기(한글 TableInsertComma·TableDeleteComma). 바꿨으면 참이다.
bool rewriteNumberCommas(model::Paragraph& para, bool insert);
EditResult noteShapeNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields, bool endnote,
                        bool wholeDocument, bool useStoredLineHeight, std::string& why);
EditResult bookmarkNow(model::Document& doc, std::vector<Node>& pages, const std::string& action,
                       const std::string& name, bool useStoredLineHeight, std::string& why,
                       const std::string& newName = {});
EditResult goToPageNow(model::Document& doc, std::vector<Node>& pages, int page, std::string& why);
EditResult tablePropertiesNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool useStoredLineHeight, std::string& why);
EditResult sectionSettingsNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool useStoredLineHeight, std::string& why);
EditResult columnsNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                      bool useStoredLineHeight, std::string& why);
// 캐럿이 든 각주·미주를 닫고 그 주 바로 뒤로 간다(한글 주석 탭 닫기). 주 밖이면 아무것도 하지 않는다.
EditResult leaveNoteNow(model::Document& doc, std::vector<Node>& pages);
// 누름틀 넣기(한글 누름틀 고치기 대화 상자의 안내문·메모·필드 이름). 캐럿 자리에 손대지 않은 누름틀을 넣는다.
EditResult insertClickHereNow(model::Document& doc, std::vector<Node>& pages, const std::string& direction,
                              const std::string& memo, const std::string& name, bool useStoredLineHeight,
                              std::string& error);
// 캐럿이 손대지 않은 누름틀(안내문만 든 누름틀) 안이면 안내문을 지우고 그 누름틀을 고친 것으로 둔다. 캐럿은 누름틀
// 시작이고, 이어 칠 글자 모양은 누름틀 앞 글자 모양이다(한글: 안내문 자리에 치면 안내문이 사라진다). 했으면 참.
bool clearClickHereGuideAtCaret(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
// 캐럿 문단의 빈(고친) 누름틀에 안내문을 다시 넣고 손대지 않은 것으로 되돌린다(한글: 다 지우면 안내문이 보인다).
bool restoreClickHereGuides(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
// 손대지 않은 누름틀 안(또는 바로 앞의 Delete, 바로 뒤의 BackSpace)에서 지우면 누름틀을 통째로 지운다.
bool deleteClickHereAtCaret(model::Document& doc, std::vector<Node>& pages, EditKey key, bool useStoredLineHeight);
// ←·→ 가 손대지 않은 누름틀을 한 칸으로 건넌다(앞 → 안 → 뒤). 했으면 참.
bool moveOverClickHere(const model::Document& doc, const std::vector<Node>& pages, EditKey key);
// 클릭한 캐럿이 안내문 글자 사이면 누름틀 안 자리로 옮긴다.
void snapCaretIntoClickHere(const model::Document& doc, const std::vector<Node>& pages);
// 누름틀 고치기 값(found, direction, memo, name, editable)과 고치기.
FormatFields clickHereView(model::Document& doc, const std::vector<Node>& pages);
EditResult modifyClickHereNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                              bool useStoredLineHeight, std::string& error);
// 누름틀 안내문 숨기기(한글 2024): 블록 없는 캐럿이 손대지 않은 누름틀 안이면 안내문을 감추고(뒤 글이 당겨진다),
// 캐럿이 나가면 다시 보인다. 편집이 아니라 보기 상태라 되돌리기 기록·고친 표시를 남기지 않는다.
// 지금 캐럿에 맞추려면 바꿀 것이 있는지(가벼운 판단).
bool clickHereGuideStale(const model::Document& doc, const std::vector<Node>& pages);
// 캐럿에 맞춘다. 바꿨으면 참이고 relayout 에 다시 놓은 쪽이 든다.
bool syncClickHereGuide(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight,
                        EditRelayoutResult& relayout);
// 숨긴 안내문을 되돌린다(편집·이동을 숨기지 않은 문서에서 하도록 그 앞에). 캐럿은 같은 논리 자리다.
bool revealClickHereGuide(model::Document& doc, std::vector<Node>& pages, EditRelayoutResult& relayout);
// 숨긴 안내문을 되돌린 문단(문서는 그대로). 숨긴 문단이 아니면 para 그대로다. 저장본·되돌리기 기록이 쓴다.
model::Paragraph shownClickHereParagraph(const model::Paragraph& para, const paint_detail::HiddenClickGuide& hidden);
// 문단 자체가 같은가(undo.cpp. 줄 정보는 보지 않는다).
bool paragraphsAlike(const model::Paragraph& a, const model::Paragraph& b);
// 누름틀 안내문·메모(필드 매개변수나 명령에서).
std::string clickHereGuide(const model::Control& field);
std::string clickHereMemo(const model::Control& field);
EditResult insertHyperlinkNow(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                             const std::string& url, bool useStoredLineHeight, std::string& error);
EditResult insertNoteNow(model::Document& doc, std::vector<Node>& pages, bool endnote, bool useStoredLineHeight,
                         std::string& error);
EditResult pageControlNow(model::Document& doc, std::vector<Node>& pages, PageControlKind kind,
                          const FormatFields& fields, bool useStoredLineHeight, std::string* error);
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
EditResult insertBreakNow(model::Document& doc, std::vector<Node>& pages, BreakKind kind, bool useStoredLineHeight);
EditResult replaceTextNow(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                          const std::string& replacement, bool matchCase, bool useStoredLineHeight);
EditResult replaceAllTextNow(model::Document& doc, std::vector<Node>& pages, const std::string& text,
                             const std::string& replacement, bool matchCase, bool useStoredLineHeight, int& count);
EditResult insertPictureNow(model::Document& doc, std::vector<Node>& pages, const Bytes& image, bool useStoredLineHeight,
                            std::int32_t width = 0, std::int32_t height = 0);
EditResult insertShapeNow(model::Document& doc, std::vector<Node>& pages, int tool, int page, std::int32_t x0,
                          std::int32_t y0, std::int32_t x1, std::int32_t y1, bool useStoredLineHeight,
                          const std::vector<model::ShapePoint>* points = nullptr);
EditResult insertDrawnShapeNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
EditResult moveObjectNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
EditResult resizeObjectNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
EditResult resizeTableBorderNow(model::Document& doc, std::vector<Node>& pages, bool useStoredLineHeight);
// 도형을 붙일 문단(쪽 page 의 y 자리). 되돌리기 칸을 그 문단 둘레로 잡을 때 쓴다.
bool shapeAnchor(const model::Document& doc, const std::vector<Node>& pages, int page, std::int32_t y, int& story,
                 int& paragraph);

}  // namespace chwp::render
