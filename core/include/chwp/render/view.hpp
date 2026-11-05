#pragma once

#include <vector>

namespace chwp::render {

// 100% 는 96dpi. 가로 픽셀은 쪽 비율로 맞춘다.
int pagePixelHeight(int heightHu, int percent);
int pagePixelWidth(int widthHu, int heightHu, int pixelHeight);

struct ViewRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// 캔버스에 붙일 칸. 좌표는 뷰포트 장치 픽셀이고, nearest 면 다른 배율 칸을 그대로 늘린 것이다.
struct ViewDraw {
    int id = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int layer = 0;
    bool nearest = false;
};

// 다음에 그릴 칸 하나. 픽셀은 셸이 받아서 id 로 보관한다.
struct ViewJob {
    int id = 0;
    int generation = 0;
    int page = 0;
    int percent = 0;
    int pixelHeight = 0;
    int col = 0;
    int row = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int contentW = 0;
    int contentH = 0;
    int layer = 0;
    // 화면에 없는 쪽의 100% 캐시. 유휴이면 코어가 이 잡을 직접 굽고, 화면 잡이 있으면 굽지 않는다.
    bool background = false;
};

enum class PageGridMode : int {
    Single = 0,  // "한쪽": 배율과 무관하게 항상 가로 1열 고정
    Two = 1,     // "두쪽": 배율과 무관하게 항상 가로 2열 고정
    Auto = 2,    // "자동": 확대·축소에 따라 가로로 들어갈 수 있는 페이지만큼 배치
    Grid = 3,    // "기타 n * m": 가로 n열 고정 + 세로 m행 기준 쪽 맞춤
};

// 이어 붙일 쪽 하나. 크기는 HWPUNIT. 바깥 여백(margin*Hu)은 페이지 양옆 메모 등 외곽 영역이다.
struct PageSpec {
    int index = 0;
    int widthHu = 0;
    int heightHu = 0;
    int marginLeftHu = 0;
    int marginRightHu = 0;
    int marginTopHu = 0;
    int marginBottomHu = 0;

    int outerWidthHu() const {
        return (marginLeftHu > 0 ? marginLeftHu : 0) + (widthHu > 0 ? widthHu : 0) +
               (marginRightHu > 0 ? marginRightHu : 0);
    }
    int outerHeightHu() const {
        return (marginTopHu > 0 ? marginTopHu : 0) + (heightHu > 0 ? heightHu : 0) +
               (marginBottomHu > 0 ? marginBottomHu : 0);
    }
};

// 스크롤 막대. 좌표는 뷰포트 장치 픽셀이다. 화면 캔버스에 코어가 그린다.
struct Scrollbar {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int thumbX = 0;
    int thumbY = 0;
    int thumbW = 0;
    int thumbH = 0;
    bool visible = false;
};

// ruler 는 눈금자 띠 두께다. 막대는 그 띠 아래·오른쪽에서 시작한다.
void layoutScrollbars(Scrollbar& horizontal, Scrollbar& vertical, int viewW, int viewH, double dpr, int scrollX,
                      int scrollY, int maxScrollX, int maxScrollY, int contentW, int contentH, int ruler = 0);

struct ViewportState {
    int scrollX = 0;
    int scrollY = 0;
    int maxScrollX = 0;
    int maxScrollY = 0;
    int percent = 100;
    bool fitPage = false;
    bool fitWidth = false;
    PageGridMode gridMode = PageGridMode::Auto;
    int gridCols = 1;
    int gridRows = 1;
    int effectiveCols = 1;
    int rowStepY = 0;
    int pageStepY = 0;
};

// 문서 스크롤 좌표의 쪽 사각형. 셸이 흰 종이와 클릭을 여기에 맞춘다.
// margin* 은 종이 바깥 여백(메모 등)의 장치 픽셀 크기다.
struct ViewPage {
    int index = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    // 종이 크기. 눈금자가 장치 픽셀을 mm 로 바꿀 때 쓴다.
    int widthHu = 0;
    int heightHu = 0;
    int marginLeft = 0;
    int marginRight = 0;
    int marginTop = 0;
    int marginBottom = 0;
    int row = 0;
    int col = 0;

    int outerX() const { return x - marginLeft; }
    int outerY() const { return y - marginTop; }
    int outerW() const { return marginLeft + w + marginRight; }
    int outerH() const { return marginTop + h + marginBottom; }
};

// 눈금자 한 구간. 종이 왼쪽(위쪽) 끝이 0mm 이다. start 는 그 끝의 뷰포트 장치 픽셀이다.
struct RulerSpan {
    int start = 0;
    int length = 0;
    int lengthHu = 0;
};

// 가로 눈금자는 눈금자 아래에 보이는 첫 줄의 쪽마다 한 구간을 낸다. 여러 쪽이 가로로 놓이면 쪽마다 0mm 부터 다시 센다.
// 세로 눈금자는 보이는 줄마다 그 줄 첫 쪽으로 한 구간을 낸다.
void layoutRulers(const std::vector<ViewPage>& pages, int scrollX, int scrollY, int viewW, int viewH, int ruler,
                  std::vector<RulerSpan>& across, std::vector<RulerSpan>& down);

struct ViewPlan {
    int generation = 0;
    double dpr = 1;
    int viewW = 1;
    int viewH = 1;
    int percent = 100;
    bool fitPage = false;
    bool fitWidth = false;
    int contentW = 0;
    int contentH = 0;
    int scrollX = 0;
    int scrollY = 0;
    int maxScrollX = 0;
    int maxScrollY = 0;
    int paperX = 0;
    int paperY = 0;
    int paperW = 0;
    int paperH = 0;
    int page = -1;
    PageGridMode gridMode = PageGridMode::Auto;
    int gridCols = 1;
    int gridRows = 1;
    int effectiveCols = 1;
    int rowStepY = 0;
    int pageStepY = 0;
    // 위쪽·왼쪽 눈금자 띠 두께(장치 픽셀). 0 이면 눈금자가 없다.
    int ruler = 0;
    bool committed = false;
    Scrollbar barX;
    Scrollbar barY;
    std::vector<ViewPage> pages;
    std::vector<int> drop;
    std::vector<ViewDraw> draw;
    ViewJob job;
};

// 쪽 비트맵을 512 칸으로 나누고, 뷰포트와 한 칸 여유만 남긴다.
// 스크롤 위치와 배율은 여기 있다. 셸은 입력을 넘기고 draw 를 그린다.
// 스크롤 막대도 여기 계산하고, 화면 캔버스에 코어가 그린다.
// setPages 는 쪽 모양(자동/한쪽/두쪽/n*m)에 따라 격자로 배치한다. 칸은 화면에 걸친 쪽만 고른다.
class PageView {
 public:
    static constexpr int kTile = 512;
    static constexpr int kMarginTiles = 1;
    static constexpr int kCacheTiles = 256;

    void reset();
    void setViewport(int cssW, int cssH, double dpr);
    void setPage(int page, int widthHu, int heightHu);
    void setPageMargins(int marginLeftHu, int marginRightHu, int marginTopHu, int marginBottomHu);
    void setPages(const std::vector<PageSpec>& pages, int gapPx);
    void setGridMode(PageGridMode mode, int cols = 1, int rows = 1, bool fit = false);
    // 위쪽·왼쪽 눈금자. 켜면 쪽은 띠 아래·오른쪽 자리에 맞춰 놓이고, 띠 위 점은 문서에 닿지 않는다.
    static constexpr int kRulerCss = 18;
    void setRulers(bool on);
    bool rulers() const { return rulers_; }
    int rulerPixels() const;
    bool inRuler(double x, double y) const;
    PageGridMode gridMode() const { return gridMode_; }
    int gridCols() const { return gridCols_; }
    int gridRows() const { return gridRows_; }
    int effectiveCols() const { return effectiveCols_; }
    int fitWidthPercent() const;
    int fitPagePercent() const;
    bool isFitPage() const { return fitPageActive_; }
    bool isFitWidth() const { return fitWidthActive_; }
    void fitWidth();
    void fitPage();
    void stepPage(int dir);
    int rowStepY() const { return rowStepY_; }
    int pageStepY() const { return pageStepY_; }
    int viewWidth() const { return viewW_; }
    int viewHeight() const { return viewH_; }
    void setZoomPercent(int percent);
    // viewX, viewY 는 뷰포트 장치 픽셀. 그 아래 문서 점이 배율 뒤에도 같은 픽셀에 남는다.
    void zoomAbout(int percent, double viewX, double viewY);
    void clearZoomAnchor();
    void setScroll(int x, int y);
    void scrollBy(int dx, int dy);
    void scrollbars(Scrollbar& horizontal, Scrollbar& vertical) const;
    ViewportState state() const;
    void setCacheLimit(int tiles);
    bool commit(int id);
    ViewPlan plan(bool includeTiles = true);

 private:
    struct Tile {
        int id = 0;
        int page = -1;
        int percent = 0;
        int col = 0;
        int row = 0;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int contentW = 0;
        int contentH = 0;
        int layer = 0;
        int stamp = 0;
    };

    struct Sheet {
        int index = 0;
        int widthHu = 0;
        int heightHu = 0;
        int marginLeftHu = 0;
        int marginRightHu = 0;
        int marginTopHu = 0;
        int marginBottomHu = 0;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int marginLeft = 0;
        int marginRight = 0;
        int marginTop = 0;
        int marginBottom = 0;
        int row = 0;
        int col = 0;

        int outerX() const { return x - marginLeft; }
        int outerY() const { return y - marginTop; }
        int outerW() const { return marginLeft + w + marginRight; }
        int outerH() const { return marginTop + h + marginBottom; }
    };

    struct RowBand {
        int y = 0;
        int h = 0;
        int step = 0;
    };

    void layout();
    void dropAt(std::size_t index);
    const Sheet* findSheet(int index) const;
    const Sheet* findFocusSheet(double focusX, double focusY) const;
    bool sameSpecs(const std::vector<PageSpec>& pages) const;

    int page_ = -1;
    bool stacked_ = false;
    int gapPx_ = 0;
    PageGridMode gridMode_ = PageGridMode::Auto;
    int gridCols_ = 1;
    int gridRows_ = 1;
    int effectiveCols_ = 1;
    bool fitPageActive_ = false;
    bool fitWidthActive_ = false;
    int rowStepY_ = 0;
    int pageStepY_ = 0;
    bool rulers_ = false;
    int rulerPx_ = 0;
    std::vector<PageSpec> specs_;
    std::vector<Sheet> sheets_;
    std::vector<RowBand> rows_;
    int widthHu_ = 0;
    int heightHu_ = 0;
    int marginLeftHu_ = 0;
    int marginRightHu_ = 0;
    int marginTopHu_ = 0;
    int marginBottomHu_ = 0;
    int cssW_ = 1;
    int cssH_ = 1;
    double dpr_ = 1;
    int viewW_ = 1;
    int viewH_ = 1;
    int percent_ = 100;
    int pixelH_ = 1;
    int contentW_ = 0;
    int contentH_ = 0;
    int scrollX_ = 0;
    int scrollY_ = 0;
    int maxScrollX_ = 0;
    int maxScrollY_ = 0;
    int lastScrollX_ = 0;
    int lastScrollY_ = 0;
    int generation_ = 0;
    int nextId_ = 1;
    int clock_ = 0;
    int cacheLimit_ = kCacheTiles;
    ViewJob inflight_{};
    std::vector<Tile> tiles_;
    std::vector<int> drops_;
    bool zoomAnchorValid_ = false;
    int zoomAnchorPage_ = -1;
    double zoomAnchorLocalX_ = 0.5;
    double zoomAnchorLocalY_ = 0.5;
    double zoomAnchorViewX_ = -1;
    double zoomAnchorViewY_ = -1;
    int zoomAnchorOldMaxX_ = 0;
    int zoomAnchorOldMaxY_ = 0;
    int zoomAnchorOldScrollX_ = 0;
    int zoomAnchorOldScrollY_ = 0;
};

}  // namespace chwp::render
