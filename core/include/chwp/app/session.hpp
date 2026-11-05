#pragma once

// 편집기 세션. 문서·조판된 쪽·칸 캐시·카메라·입력·배경 굽기 일정·화면 판단을 모두 코어가 들고 결정한다.
// 셸(웹 바인딩, 네이티브 창)은 값을 바꿔 넘기고, 여기서 돌려준 값을 포장하기만 한다.
// 창·프레임 시계·코어 수처럼 플랫폼만 아는 것은 Host 로 받는다.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chwp/input/pointer.hpp"
#include "chwp/input/text_field.hpp"
#include "chwp/input/wheel_reader.hpp"
#include "chwp/model/document.hpp"
#include "chwp/render/format_view.hpp"
#include "chwp/render/frame_cache.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "chwp/render/view.hpp"
#include "chwp/common/bytes.hpp"

namespace chwp {
class JsonWriter;
}

namespace chwp::app {

// 카메라가 바뀌었을 때 셸에 알리는 값.
struct CameraNote {
    int scrollX = 0;
    int scrollY = 0;
    int percent = 100;
    bool coasting = false;
    bool pinching = false;
    int page = 0;
    int pageCount = 0;
    bool fitPage = false;
    bool fitWidth = false;
    int viewW = 0;
    int viewH = 0;
    int maxScrollX = 0;
    int maxScrollY = 0;
    double dpr = 1;
    bool ownsScroll = false;
    bool settled = true;
};

// 플랫폼이 채운다. 비어 있으면 코어 기본값(스레드 없음, steady_clock)을 쓴다.
struct Host {
    // 프레임 시각과 같은 기준의 밀리초.
    std::function<double()> nowMs;
    // 일꾼 스레드를 띄울 수 있는 논리 코어 수. 0 이면 스레드 없이 화면 스레드가 쉬는 틈에 굽는다.
    std::function<int()> logicalCores;
    // 한 번에 띄울 일꾼 스레드 수의 하한과 상한(0 은 한도 없음). 웹은 스레드 풀 크기가 정해져 있어 상한을 둔다.
    int minWorkers = 8;
    int maxWorkers = 0;
    // 100% 넘게 확대해 굽는 개체 칸의 한 변·넓이 한도(0 은 한도 없음).
    // 휴대폰·태블릿처럼 메모리가 좁은 기기인가. 열어 둘 글꼴 파일 예산 같은 캐시 정책은 core 가 이 값으로 정한다.
    bool smallMemory = false;
    int zoomedFrameSide = 0;
    std::int64_t zoomedFramePixels = 0;
    // 화면(GPU)이 글자·그림·표 테두리를 칸 비트맵 대신 직접 그린다. 칸에는 그 자료만 남긴다.
    bool screenVectors = false;
    // 화면 프레임 루프를 건다. 루프는 매 프레임 screenFrame(시각) 을 부른다. 한 번만 부른다.
    std::function<void()> startFrameLoop;
    // 화면 그림판 픽셀 크기를 맞춘다.
    std::function<void(int, int)> resizeSurface;
    std::function<void(const CameraNote&)> cameraChanged;
    // GPU 화면을 못 쓸 때 2D 그림 목록을 그 플랫폼 2D 그리기로 옮긴다(이 스레드나 다른 스레드의 캔버스). 못 그리면 거짓이다.
    std::function<bool(const render::ScreenFrame2D&)> presentScreenOps;
    // 일꾼 스레드에서 끝난 일의 뒷일을 셸 스레드(core 를 부르는 스레드)에서 하게 맡긴다.
    // 없으면 core 는 일꾼 스레드에 맡기지 않고 셸 스레드에서 바로 한다.
    std::function<void(std::function<void()>)> post;
    // 한 묶음(fontFetchPlan 의 batch)의 글꼴을 다 받아 풀고 문서에 적용했다. 셸 스레드에서 부른다.
    std::function<void(int batch)> fontsSettled;
};

void setHost(Host host);

// ---- 상태 없는 형식 함수
std::string detectFormat(const Bytes& bytes);
std::string parse(const Bytes& bytes, bool pretty, bool includeLineSegs);
std::string extractText(const Bytes& bytes);
std::string extractPages(const Bytes& bytes);
std::string extractAnchorPages(const Bytes& bytes);
std::string extractInnerLines(const Bytes& bytes);
std::string extractLayout(const Bytes& bytes);
std::string layoutScene(const Bytes& bytes);
void writeScene(JsonWriter& writer, const render::Node& node);
// 형식 이름(hwp, hwpx)으로 바꾼다. 못 바꾸면 비어 있다.
std::optional<Bytes> convertDocument(const Bytes& bytes, const std::string& format);
std::optional<Bytes> exportDocument(const std::string& format);
// 열린 문서를 저장한다. 형식은 이름 끝(.hwp/.hwpx), 없으면 연 파일의 형식이다(agent 저장과 같은 DocSession 규칙).
// 이름에 그 형식의 끝이 없으면 붙이고, 이름이 비면 "문서" 로 한다. 실패하면 bytes 가 없다.
struct SavedDocument {
    std::optional<Bytes> bytes;
    std::string name;
    std::string format;
    std::string error;     // 실패한 까닭
    std::string docState;  // 저장한 뒤 문서 상태
};
SavedDocument saveDocument(const std::string& name);

// ---- 글꼴·캐시
// 셸이 받을 글꼴 파일. 셸은 글꼴 폴더 아래 folder/file 을 받아 fontFetched(id, 바이트) 로 넘기고,
// 못 받았으면 fontFetchFailed(id) 를 부른다. 한 묶음을 다 받아 풀고 대신 받은 글꼴 이름까지 문서에 적용하면
// core 가 Host::fontsSettled(batch) 로 알린다. 셸은 그때까지 기다릴 일만 기다린다.
struct FontFetchJob {
    int id = 0;
    int batch = 0;
    std::string folder;
    std::string file;
    std::string face;
};
// 넘긴 글꼴 중 아직 없는 것을 파일마다 하나로 묶는다. 대신 받을 글꼴(replace.json)도 여기서 고른다.
// 한 번 부를 때 나온 일감은 같은 batch 다. 받을 것이 없으면 빈 목록이고 알림도 없다.
std::vector<FontFetchJob> fontFetchPlan(const std::vector<std::string>& faces);
// 받은 글꼴에 없는 문서 글자를 그릴 대체 글꼴 중 아직 없는 이름. 셸은 한 묶음이 끝나면 이것을 묻고,
// 비어 있지 않으면 그 이름으로 한 번 더 받은 뒤 조판한다.
std::vector<std::string> fontGapFaces();
void fontFetched(int id, Bytes bytes);
void fontFetchFailed(int id);
// 사용자가 넣은 글꼴. 아직 없는 이름만 등록하고 등록한 이름 수를 돌려준다.
int installFont(const std::vector<std::string>& faces, Bytes bytes);
int replaceFontFace(const std::string& from, const std::string& to);
void trimCaches(int level);
bool setCacheBudget(const std::string& name, double bytes);
std::string cacheUsage();
// 큰 자료가 쓰는 메모리(JSON, 바이트). 캐시 예산 밖의 메모리를 찾으려고 둔다.
std::string memoryReport();
// 지난번 부른 뒤로 모은 화면 프레임 통계(JSON). 부르면 비운다.
std::string frameStats();

// ---- 문서 열기·계획
struct OpenResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> faces;
    std::string docState;  // DocSession::documentState
    std::string name;      // 새 문서의 이름(저장할 때 형식 끝이 붙는다). 연 문서는 비어 있다(파일 이름을 쓴다)
};
OpenResult openDocument(Bytes bytes);
OpenResult newDocument();
std::string planDocument(bool lineSeg);
// 저장된 줄 높이를 쓸지는 core 기본값(DocSession::kUseStoredLineHeight)으로 정한다. 셸은 이것을 부른다.
std::string planDocument();
// 받지 못해 대체 글꼴로 재고 그리는 문서 글꼴 이름(agent 의 missingFonts 와 같다).
std::vector<std::string> missingFonts();
int ensurePageSpans(int page);

// ---- 조회
struct StyleReport {
    render::CursorStyle style;
    render::CaretStatus status;
};
StyleReport cursorStyleNow();
// 문서 스타일 이름(스타일 표 차례). 리본 스타일 목록이 쓴다.
std::vector<std::string> styleNames();
// 글꼴 고르기 목록: 받을 수 있는 글꼴과 지금 문서에 쓰인 글꼴. 라이선스로 막힌 글꼴은 뺀다.
std::vector<std::string> pickableFonts();
render::FormatFields formatFieldsNow();
render::FormatFields pageSetupFieldsNow();
struct CursorReport {
    render::PointerCursor cursor;
    bool valid = false;
    std::string storyKind;
    int sectionIndex = -1;
    StyleReport style;
};
CursorReport cursorState();
struct FontSwapRow {
    std::string from;
    std::string to;
};
// to 는 사용자가 전에 고른 글꼴(아직 고를 수 있으면) 또는 기본 짝이다.
std::vector<FontSwapRow> licenseFontSwaps();
// 막힌 글꼴마다 사용자가 기본 짝과 다르게 고른 글꼴. 셸은 저장해 둔 JSON 을 넘기고,
// 바꾸기를 한 뒤 fontSwapPreferences() 를 다시 저장한다(저장소는 플랫폼 일이다).
void setFontSwapPreferences(const std::string& json);
std::string fontSwapPreferences();
// 최근에 고른 색(#rrggbb, 새 것 먼저, 10개까지)과 글자 색 단추의 처음 색. 셸은 저장해 둔 글을 넘기고,
// 색을 고를 때마다 rememberColor 를 부른 뒤 saved 를 다시 저장한다(저장소는 플랫폼 일이다).
struct ColorMemory {
    std::vector<std::string> recent;
    std::string pen;    // 저장해 둔 글을 읽을 때 정한다. 최근 색 첫째, 없으면 기본 빨강
    std::string saved;  // 저장할 글
};
ColorMemory setRecentColors(const std::string& saved);
ColorMemory rememberColor(const std::string& hex);
// GPU 화면이 없을 때 셸이 베끼는 선택 그림. 다음 보고까지 살아 있다.
const std::vector<render::SelectionImage>& fallbackImages();

// ---- 옛 포인터 경로(쪽 좌표)
struct PointerReport {
    render::PointerView view;
    render::PointerCursor cursor;
    std::string event;
    bool styled = false;  // 끌기 중이 아니면 스타일·글자를 채운다
    StyleReport style;
    std::string text;
};
PointerReport pointerEvent(int kind, int page, int x, int y, int percent);

// ---- 편집
struct PageSize {
    int width = 0;
    int height = 0;
    int marginLeft = 0;
    int marginRight = 0;
    int marginTop = 0;
    int marginBottom = 0;
};
struct EditReport {
    render::EditResult result;
    int scrollX = 0;
    int scrollY = 0;
    bool canUndo = false;
    bool canRedo = false;
    StyleReport style;
    std::string text;
    std::vector<PageSize> pages;  // 문서 전체를 다시 놓았을 때만
    std::string docState;         // DocSession::documentState
};
EditReport editText(const std::string& text, bool composing);
EditReport editCommit();
EditReport editKey(int key, bool shift, bool ctrl, bool alt);
// 고른 글을 지운다(오려두기·블록 지우기). 고른 글이 없으면 아무것도 하지 않는다. 클립보드에 넣는 일은 셸이 한다.
EditReport editDeleteSelection();
// 입력기(IME) 사건. 셸은 플랫폼 조합 사건을 종류로 바꿔 넘긴다.
//   start, update(조합 중 글), end(끝 글), settle(끝 바로 뒤 다음 조합이 없으면), insert(조합 아닌 글), enter(조합 중 Enter)
// 끝 뒤에 바로 다음 조합이 오면 끝 글은 버리고 확정만 한다. 끝 글이 마지막 조합 글과 같으면 확정, 다르면 바꿔 넣는다.
EditReport editIme(const std::string& kind, const std::string& text, bool shift);
// 입력칸에서 커서 앞 글자 removed 개가 지워지고 text 가 들어왔다(입력기가 앞 글자를 바꿔 넣을 때). 한 번의 편집으로 하고 한 번 그린다.
EditReport editImeReplace(int removed, const std::string& text);
// 편집 입력칸 사건. 셸은 플랫폼 입력칸 사건을 그대로 바꿔 넘기고, 돌아온 field 대로 입력칸 앞 글을 떼거나 글자를 넣고
// 깨우기 타이머를 건다. 사건 그 자리에서 할 일은 input::fieldRules() 표대로 한다.
// 무엇을 문서에 넣고 지울지, 언제 입력칸을 비울지는 코어(input::TextField)가 정한다.
struct FieldResult {
    bool edited = false;  // report 가 있다(문서·캐럿이 바뀌었을 수 있다)
    EditReport report;
    chwp::input::FieldReply field;  // edits 는 비운다. 이미 report 로 실행했다
};
FieldResult editField(const chwp::input::FieldEvent& event);
EditReport editUndo();
EditReport editRedo();
EditReport editInsertTable(int rows, int cols);
EditReport editInsertPicture(const Bytes& image);
EditReport editStyle(const render::FormatPatch& patch);
EditReport editPageSetup(const render::FormatFields& fields, bool wholeDocument);
// 서식 대화 상자(0 글자 모양, 1 문단 모양, 2 편집 용지, 3 표 넣기). 셸은 칸 이름과 칸에 친 값을 그대로 넘긴다
// (chwp/render/format_view.hpp). 보기는 패치를 적용했을 때의 값이고 문서는 바꾸지 않는다.
render::FormatView formatDialogView(int kind, const render::FormatFields& patch);
EditReport editFormatDialog(int kind, const render::FormatFields& patch, bool wholeDocument);
const render::PageBitmap* formatDialogPreview(int kind, const render::FormatFields& patch, int width, int height);
EditReport editSwapFonts(const std::vector<std::pair<std::string, std::string>>& chosen);

// ---- 화면 배치
struct PageSpecPatch {
    int index = 0;
    int widthHu = 0;
    int heightHu = 0;
    std::optional<int> marginLeftHu;
    std::optional<int> marginRightHu;
    std::optional<int> marginTopHu;
    std::optional<int> marginBottomHu;
};
struct ViewPatch {
    bool reset = false;
    struct Viewport {
        int cssW = 1;
        int cssH = 1;
        double dpr = 1;
    };
    std::optional<Viewport> viewport;
    std::optional<std::vector<PageSpecPatch>> pages;
    int gap = 0;
    std::optional<PageSpecPatch> page;
    struct Grid {
        std::string mode = "auto";
        int cols = 1;
        int rows = 1;
        std::optional<bool> fit;
    };
    std::optional<Grid> grid;
    std::optional<std::string> fit;
    std::optional<int> zoom;
    std::optional<std::pair<int, int>> scroll;
    int pageStep = 0;
    int commit = 0;
};
struct ViewApplied {
    // GPU 화면이 있으면 그리기·버리기 목록은 비어 있다. 화면이 칸을 들고 직접 그린다.
    render::ViewPlan plan;
    bool committed = false;
    bool screenOn = false;
    // 남은 잡이 쪽 조각이 아니라 칸(FrameCache) 하나다.
    bool frameJob = false;
};
ViewApplied applyView(const ViewPatch& patch);
const char* gridModeName(render::PageGridMode mode);
// 배율 방식 이름(fitPage, fitWidth, percent)과 화면에 보일 배율 글("쪽 맞춤", "폭 맞춤", "N%").
const char* zoomModeName(bool fitPage, bool fitWidth);
std::string zoomLabel(bool fitPage, bool fitWidth, int percent);
render::PageGridMode parseGridMode(const std::string& mode);

void beginGrind();

// 쪽 한 장을 그림으로. 실패하면 nullptr. 다음 호출까지 살아 있다.
const std::vector<std::uint8_t>* renderPagePng(int page, int pixelHeight);
const render::PageBitmap* renderPagePixels(int page, int pixelHeight);

// ---- GPU 화면
// remote 면 화면 캔버스가 이 스레드에 없다. core 가 2D 그림 목록을 정해 Host::presentScreenOps 로 넘긴다.
bool attachScreen(bool remote = false);
bool screenReady();
// 화면 붙이기 진행. 셸은 Pending 인 동안만 기다린다. 장치가 끝내 안 오면 core 가 Failed 로 정한다.
enum class ScreenAttach : int { Pending = 0, Ready = 1, Failed = 2 };
ScreenAttach screenAttachState();
bool screenFrame(double time);
bool presentView(int viewW, int viewH, int scrollX, int scrollY, int layoutZoom, bool dragging);

// ---- 입력
// 셸이 주지 않은 값은 비워 둔다. 기본값은 core 가 메시지 종류를 보고 정한다.
struct InputPointer {
    int device = 0;
    std::optional<int> phase;
    int id = 0;
    double x = 0;
    double y = 0;
    double t = 0;
    int button = 0;
};
// type: pointer, pointers, wheel, wheelRaw, gesture, tick, scroll, viewport, revealPage, zoom, zoomStep, pageStep, pageUp, pageDown,
// fit, grid, pins, readOnly, rulers, embed, embedOpen. 각 종류는 쓰는 칸만 읽는다.
struct InputMessage {
    std::string type;
    InputPointer pointer;
    std::vector<InputPointer> items;
    std::optional<int> phase;
    int kind = 0;
    double x = 0;
    double y = 0;
    double dx = 0;
    double dy = 0;
    double vx = 0;
    double vy = 0;
    double scale = 1;
    double rotation = 0;
    bool zoom = false;
    bool discrete = false;
    double dt = 0;
    int scrollX = 0;
    int scrollY = 0;
    int cssW = 1;
    int cssH = 1;
    double dpr = 1;
    // wheelRaw: 플랫폼 휠 값 그대로. dx·dy 는 플랫폼 단위, deltaMode 는 0 픽셀·1 줄·2 쪽, pageW·pageH 는 쪽 단위 크기다.
    int deltaMode = 0;
    double pageW = 0;
    double pageH = 0;
    int page = 0;
    std::optional<int> percent;
    std::optional<int> dir;
    std::string mode;
    // zoomDialog: 배율 고르기("page" 쪽 맞춤, "width" 폭 맞춤, 그 밖은 percent).
    std::string choice;
    int cols = 1;
    int rows = 1;
    std::optional<bool> fit;
    bool on = false;
};
// 포인터가 글을 고르거나 개체를 다룬 결과.
struct SelectionReport {
    bool attached = false;  // 거짓이면 아래는 비어 있다
    bool needsFontSwap = false;
    bool edited = false;
    std::string docState;  // edited 일 때 문서 상태
    int selectPhase = 0;
    bool pinTap = false;
    bool active = false;
    bool caret = false;
    bool styled = false;
    StyleReport style;
    std::string text;
    std::string event;
};
struct InputReport {
    input::Camera camera;
    render::Scrollbar barX;
    render::Scrollbar barY;
    int page = 0;
    int pageCount = 0;
    // 입력(휠·관성·끌기·핀치·막대·터치)이 스크롤을 쥐고 있다. 셸은 그동안 제 스크롤 막대를 덮어쓰지 않는다.
    bool ownsScroll = false;
    // 움직임이 멈췄다. 셸은 이때 제 스크롤 막대를 카메라 자리로 맞춘다.
    bool settled = true;
    // 시간이 흘러야 끝나는 일(관성, 휠 끝 판정)이 남았다. 프레임 루프가 없으면 셸이 다음 프레임에 tick 을 보낸다.
    bool needsTick = false;
    // 탭과 끌기를 가르는 거리(장치 픽셀). 손이 떨어지는 그 순간 판단해야 하는 셸(화면 자판 초점)이 같은 기준을 쓴다.
    double tapSlop = input::ViewportInput::kTapSlop;
    // 사용자가 켠 눈금자 값(처음은 켬). 보기 전용이면 이 값과 달리 숨긴다. 셸은 이 값을 메뉴에 보이고 저장한다.
    bool rulersWanted = true;
    bool pointer = false;  // pointer·pointers 면 selection 을 붙인다
    SelectionReport selection;
};
InputReport input(const InputMessage& msg);

// 단축키 표. 키를 누른 그 순간 기본 동작을 막아야 하는 셸이 이 표를 받아 두고 찾아본다.
// key 는 플랫폼 키 이름이다(웹: 글자 키는 KeyboardEvent.code 인 KeyZ, 나머지는 key 인 ArrowLeft·F7).
struct KeyBinding {
    std::string key;
    int shift = -1;  // -1 상관없음, 0 안 누름, 1 누름
    bool ctrl = false;  // Ctrl 또는 Cmd
    bool alt = false;
    // editKey(editKey 번호로 editKey 를 부른다), undo, redo, copy, cut, paste, open, save, pageUp, pageDown,
    // charDialog, paraDialog, pageDialog. 메뉴의 단축키 표시도 이 표에서 읽는다.
    std::string command;
    int editKey = 0;
    bool editorOnly = false;  // 편집 입력칸에 있을 때만(대화 상자의 Esc 는 두려고)
};
const std::vector<KeyBinding>& keyBindings();
// 배율 대화 상자의 값들(JSON): 미리 정한 배율, 사용자 배율 한 걸음, 가장 작은 배율, 여러 쪽의 가장 큰 칸 수,
// 처음 칸 수. 적용은 input "zoomDialog" 하나로 보낸다(쪽 모양과 배율을 함께 core 가 정한다).
std::string zoomDialogSpec();
std::string hoverCursor(double vx, double vy);

// ---- 임베드
// 다른 페이지에 넣은 보기(웹 iframe 등)가 받은 명령. 셸은 받은 글을 그대로 넘기고, 뜻과 범위는 core 가 정한다.
//   zoom: "150", "150%", "page"(쪽 맞춤), "width"(폭 맞춤), "in", "out"
//   page: 쪽 번호(1부터, 넘치면 끝 쪽), "next", "prev"(쪽 한 줄씩), "first", "last"
//   view: "auto", "single", "two", "grid"(cols·rows 를 함께 준다)
// 알 수 없는 명령이나 값이면 kind 가 None 이다. 적용은 input "embed"(mode 명령, choice 값, cols·rows)로 보낸다.
struct EmbedCommand {
    enum class Kind { None, Zoom, FitPage, FitWidth, ZoomStep, Page, PageStep, View };
    Kind kind = Kind::None;
    int value = 0;  // Zoom: 배율, ZoomStep·PageStep: 방향(±1), Page: 0부터 센 쪽
    render::PageGridMode view = render::PageGridMode::Auto;
    int cols = 1;
    int rows = 1;
};
EmbedCommand parseEmbedCommand(const std::string& command, const std::string& value, int cols, int rows, int pageCount);
// 처음 열 때의 주소 인자("?file=…&view=two&zoom=150&page=3"). 쪽 모양, 배율, 쪽 차례로 적용할 명령을 준다.
// 모르는 인자는 건너뛴다. 적용은 input "embedOpen"(choice 에 인자 글)으로 보낸다.
std::vector<EmbedCommand> parseEmbedQuery(const std::string& query, int pageCount);

}  // namespace chwp::app
