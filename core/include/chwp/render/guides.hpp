#pragma once
// 보기 격자와 안내선(한글 2024 보기-격자, 보기-안내선). 문서에 저장하지 않는 편집 화면 설정이고 인쇄·쪽 그림에도 나오지 않는다.
// 무엇을 어디에 그릴지, 개체를 어디에 붙일지는 여기서 정한다. 화면(paint)과 끌기(pointer_hold)가 같은 값을 쓴다.
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chwp/render/paint.hpp"

namespace chwp::render {

struct Node;

// 격자 모양(한글 격자 설정의 점·가로선·세로선·가로/세로선).
enum class GridKind : std::uint8_t { Dots, Horizontal, Vertical, Both };
// 격자 방식. Free 는 격자를 눈금자처럼 보기만 하고, Magnet 은 자석 범위 안이면 붙이고, Only 는 늘 붙인다.
enum class GridSnap : std::uint8_t { Free, Magnet, Only };

// 길이는 0.01mm 단위다(대화 상자가 mm 둘째 자리까지 받는다).
// 간격 5mm·자석 범위 2mm·시작점 0mm 는 한글 도움말(격자 설정)에 적힌 처음값이다. 모양·위치·방식은 도움말에 없어
// 이 PC 한글 대화 상자에서 본 값(가로/세로선, 글 뒤, 격자에 상관없이)을 쓴다. 한글은 이 값을 실행 사이에 저장해 두므로
// 대화 상자 값이 공장 처음값이라는 보장은 없다.
struct GridSettings {
    bool show = false;
    GridKind kind = GridKind::Both;
    bool front = false;  // 글 앞. 아니면 글 뒤
    GridSnap snap = GridSnap::Free;
    int spanX = 500;
    int spanY = 500;
    // 시작점의 기준. 쪽이면 본문 영역 왼쪽 위, 종이면 종이 왼쪽 위다. 격자는 쪽 기준이면 본문 영역에만, 종이 기준이면 종이 전체에 깐다.
    bool fromPaper = false;
    int originX = 0;
    int originY = 0;
    int magnetX = 200;
    int magnetY = 200;
};

// 안내선. 처음값은 한글 2024 개체 이동 안내선 설정 대화 상자에서 확인한 값이다(보기 꺼짐, 실선, 다른 개체에 맞춰 이동,
// 종이 기준, 자석 효과·위치 풍선·크기 풍선 켬). 그리기 안내선은 처음 켜면 종이 한가운데에 놓인다.
struct GuideSettings {
    bool drawShow = false;
    // 끌어 옮긴 그리기 안내선 자리(종이 왼쪽 위에서 HU). 옮기기 전에는 종이 한가운데다.
    std::optional<std::int32_t> drawX;
    std::optional<std::int32_t> drawY;
    bool moveShow = false;
    bool dashed = false;
    bool alignOthers = true;
    bool fromPaper = true;
    bool magnet = true;
    bool tipPosition = true;
    bool tipSize = true;
};

// 보기 표시(한글 보기-표시/숨기기). 문서에 저장하지 않는 편집 화면 설정이다. 셸만 켜므로 agent 의 쪽 그림에는 나오지 않는다.
// 한글 2024 도움말(조판 부호·문단 부호·투명 선·메모)과 한글 화면에서 확인한 동작:
//   - 문단 부호: 문단 끝 ↵, 강제 줄 바꿈 ↓. 파란색.
//   - 조판 부호: 문단 부호도 함께 보인다. 빈칸 ∨, 탭 ⇥(파란색)과 컨트롤 자리의 [이름](주황 RGB(255,111,0)).
//     이름표는 본문 글자 모양으로 그리고, 줄을 다시 나누지 않은 채 그 뒤 글자를 민다(편집 영역 밖으로
//     나갈 수 있다). 문단 부호를 먼저 켰으면 조판 부호를 끄고 켜도 문단 부호는 그대로다. 조판 부호가 켜진 채 문단 부호를
//     누르면 둘 다 꺼진다(숨긴 한글 ViewProperties.OptionFlag 로 확인).
//   - 투명 선: 표·글상자에서 선 종류가 없음인 테두리를 빨간 점선으로 보인다.
//   - 메모: 끄면 메모 영역 띠와 메모 상자를 그리지 않고 쪽을 종이 폭으로 둔다.
// 한글은 조판 부호를 켜면 메모 상자를 감추지만(도움말 memo.htm), 우리는 메모 보이기로만 정한다.
struct ViewMarks {
    bool paragraph = false;
    bool control = false;
    bool transparent = false;
    bool memo = true;
    // 교정 부호. 한글은 처음부터 보인다(보기 OptionFlag 0x2000 이 켜지면 감춘다). 조판 부호를 켜면 그림 대신 이름표다.
    bool proof = true;
    bool operator==(const ViewMarks&) const = default;
    // 문단 끝·줄 바꿈 표시를 그리는가. 조판 부호는 문단 부호를 품는다.
    [[nodiscard]] bool paragraphMarks() const { return paragraph || control; }
    // 교정 부호 그림을 그리는가.
    [[nodiscard]] bool proofMarks() const { return proof && !control; }
};

struct ViewGuides {
    GridSettings grid;
    GuideSettings guide;
    ViewMarks marks;
};

// 지금 설정의 사본. 화면 스레드와 셸 호출이 같이 읽는다.
ViewGuides viewGuides();
// 격자 모양이 바뀔 때마다 는다. 쪽 텍스처처럼 격자를 품고 구운 그림은 이 값이 바뀌면 다시 굽는다.
// 안내선은 텍스처 위에 그리므로 이 값을 바꾸지 않는다.
std::uint32_t viewGuidesSeq();

// 지금 보기 표시(viewGuides().marks).
ViewMarks viewMarks();
// 셸 값: paraMarks ctrlMarks transparentLines memos proofMarks(참·거짓). paraMarks 는 조판 부호가 켜져 있으면 참으로 알린다.
// paraMarksOwn 은 조판 부호와 상관없이 문단 부호를 켰는지다(저장했다가 다시 넘기는 값).
FormatFields viewMarkFields(const ViewMarks& marks);
// toggle(paraMarks|ctrlMarks|transparentLines|memos|proofMarks)은 그 표시를 뒤집는다(조판 부호가 켜진 채 paraMarks 를 뒤집으면
// 둘 다 끈다). 이름 그대로의 참·거짓 값은 그 표시를 그 값으로 둔다.
ViewMarks patchViewMarks(const ViewMarks& base, const FormatFields& patch);

// 0.01mm ↔ HU.
double hundredthMmToHu(int value);
int huToHundredthMm(double hu);

// 종이 한 장의 크기와 본문 영역(HU, 종이 왼쪽 위가 0).
struct PaperBox {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t bodyLeft = 0;
    std::int32_t bodyTop = 0;
    std::int32_t bodyRight = 0;
    std::int32_t bodyBottom = 0;
};
// 쪽 노드의 종이·본문 영역. 본문 노드가 없으면 쪽 여백으로 본다.
PaperBox paperBoxOf(const Node& paper);

// 설정 ↔ 대화 상자 값. 길이는 mm(소수 둘째 자리), 이름은 아래와 같다.
//   gridShow gridKind(DOTS|HORIZONTAL|VERTICAL|BOTH) gridFront gridSnap(FREE|MAGNET|ONLY) gridSpanX gridSpanY
//   gridBase(PAGE|PAPER) gridOriginX gridOriginY gridMagnetX gridMagnetY
//   drawGuides moveGuides moveDashed moveOthers moveBase(PAGE|PAPER) moveMagnet moveTipPosition moveTipSize
//   보기 표시 paraMarks ctrlMarks transparentLines memos(viewMarkFields)
FormatFields viewGuideFields(const ViewGuides& guides);
// patch 를 얹은 설정. 값은 한글 대화 상자의 범위로 자른다(간격 1~25mm, 자석 범위 0~25mm, 시작점 쪽 ±557.99mm·종이 0~557.99mm).
// toggle(grid|drawGuides|moveGuides)은 그 보기를 뒤집는다. 기준(gridBase)만 바꾸고 시작점을 주지 않으면, 같은 자리를
// 새 기준으로 다시 적는다(box 의 본문 영역 왼쪽 위로 옮겨 잰다). box 가 없으면 시작점 값을 그대로 둔다.
ViewGuides patchViewGuides(const ViewGuides& base, const FormatFields& patch, const PaperBox* box);
// 설정을 바꾼다. 화면에 보이는 것이 바뀌었으면 참이다.
bool setViewGuides(const ViewGuides& next);

// 한 쪽에 깔 격자. HU, 종이 왼쪽 위가 0. 선(점)은 origin + k*span 자리에 area 안에서만 놓인다.
struct GridPlan {
    bool show = false;
    GridKind kind = GridKind::Both;
    bool front = false;
    double originX = 0;
    double originY = 0;
    double spanX = 0;
    double spanY = 0;
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
};
GridPlan gridPlanFor(const GridSettings& grid, const PaperBox& box);
// from~to 안의 격자 자리(origin + k*span). 간격이 0 이하면 빈 목록이다.
std::vector<double> gridStops(double origin, double span, double from, double to);

// 개체를 그리거나 옮기거나 키울 때 쓸 자리(HU, 종이 왼쪽 위가 0). 격자가 보이고 격자 방식이 붙이기일 때만 붙인다.
// 자석 효과는 자석 범위 안이면 가장 가까운 격자에 붙인다. 자석 범위가 간격의 절반을 넘으면 늘 붙는다(한글과 같다).
std::int32_t snapToGrid(const GridSettings& grid, const PaperBox& box, std::int32_t value, bool horizontal);

// 이번 프레임 화면에 걸친 쪽들의 종이·본문 영역. 세션이 그리기 전에 넣고 화면 합성이 읽는다.
void setScreenPaperBoxes(std::vector<std::pair<int, PaperBox>> boxes);
std::optional<PaperBox> screenPaperBox(int page);

}  // namespace chwp::render
