#include "chwp/render/view.hpp"

#include "chwp/render/paint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace chwp::render {
namespace {

int roundPositive(double value) {
    return static_cast<int>(std::llround(value));
}

bool intersects(ViewRect a, ViewRect b) {
    return a.w > 0 && a.h > 0 && b.w > 0 && b.h > 0 && a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h &&
           a.y + a.h > b.y;
}

int mapCoord(int value, int from, int to) {
    if (from <= 0) return value;
    return roundPositive(static_cast<double>(value) * static_cast<double>(to) / static_cast<double>(from));
}

long long dist2(ViewRect tile, ViewRect view) {
    const long long cx = tile.x + tile.w / 2;
    const long long cy = tile.y + tile.h / 2;
    const long long vx = view.x + view.w / 2;
    const long long vy = view.y + view.h / 2;
    const long long dx = cx - vx;
    const long long dy = cy - vy;
    return dx * dx + dy * dy;
}

struct Slot {
    int page = 0;
    int col = 0;
    int row = 0;
    int layer = 0;
    int pixelH = 0;
    int pageW = 0;
    int pageH = 0;
    ViewRect local{};
    ViewRect rect{};
};

// 보이는 글부터 칠한다. 글 뒤 면, 글 앞 그림, 테두리는 그 다음이다.
int paintOrder(int layer) {
    if (layer == static_cast<int>(PaintLayer::Text)) return 0;
    if (layer == static_cast<int>(PaintLayer::Behind)) return 1;
    if (layer == static_cast<int>(PaintLayer::Front)) return 2;
    return 3;
}

bool closer(const Slot& a, const Slot& b, ViewRect view) {
    const int oa = paintOrder(a.layer);
    const int ob = paintOrder(b.layer);
    if (oa != ob) return oa < ob;
    const long long da = dist2(a.rect, view);
    const long long db = dist2(b.rect, view);
    if (da != db) return da < db;
    if (a.row != b.row) return a.row < b.row;
    return a.col < b.col;
}

}  // namespace

int pagePixelHeight(int heightHu, int percent) {
    if (heightHu <= 0 || percent <= 0) return 1;
    const double value = (static_cast<double>(heightHu) * 96.0 * static_cast<double>(percent)) / 100.0 / 7200.0;
    return std::max(1, roundPositive(value));
}

int pagePixelWidth(int widthHu, int heightHu, int pixelHeight) {
    if (widthHu <= 0 || heightHu <= 0 || pixelHeight <= 0) return 1;
    const double value = (static_cast<double>(widthHu) * static_cast<double>(pixelHeight)) / static_cast<double>(heightHu);
    return std::max(1, roundPositive(value));
}

namespace {

int marginPixelSize(int marginHu, int refHeightHu, int refPixelH, int percent) {
    if (marginHu <= 0) return 0;
    if (refHeightHu > 0 && refPixelH > 0) {
        return pagePixelWidth(marginHu, refHeightHu, refPixelH);
    }
    return pagePixelHeight(marginHu, percent);
}

void measureSpecAtPercent(const PageSpec& spec, int percent, int& outW, int& outH, int& outML, int& outMR, int& outMT,
                          int& outMB) {
    const int wHu = std::max(0, spec.widthHu);
    const int hHu = std::max(0, spec.heightHu);
    outH = hHu > 0 ? pagePixelHeight(hHu, percent) : 0;
    outW = hHu > 0 ? pagePixelWidth(wHu, hHu, outH) : 0;
    outML = marginPixelSize(spec.marginLeftHu, hHu, outH, percent);
    outMR = marginPixelSize(spec.marginRightHu, hHu, outH, percent);
    outMT = marginPixelSize(spec.marginTopHu, hHu, outH, percent);
    outMB = marginPixelSize(spec.marginBottomHu, hHu, outH, percent);
}

void measureSpecFitPage(const PageSpec& spec, double scale, int slotW, int slotH, int percent, int& outW, int& outH,
                        int& outML, int& outMR, int& outMT, int& outMB) {
    const int wHu = std::max(0, spec.widthHu);
    const int hHu = std::max(0, spec.heightHu);
    const int mlHu = std::max(0, spec.marginLeftHu);
    const int mrHu = std::max(0, spec.marginRightHu);
    const int mtHu = std::max(0, spec.marginTopHu);
    const int mbHu = std::max(0, spec.marginBottomHu);
    const int targetSlotH = std::max(1, slotH);
    outH = hHu > 0 ? std::clamp(roundPositive(static_cast<double>(hHu) * scale), 1, targetSlotH) : 0;
    outW = hHu > 0 ? pagePixelWidth(wHu, hHu, outH) : 0;
    outML = marginPixelSize(mlHu, hHu, outH, percent);
    outMR = marginPixelSize(mrHu, hHu, outH, percent);
    if (slotW > 0 && outML + outW + outMR > slotW) {
        const int overW = (outML + outW + outMR) - slotW;
        const int shrinkW = std::min(overW, std::max(0, outW - 1));
        outW -= shrinkW;
        int remOver = overW - shrinkW;
        if (remOver > 0 && outMR > 0) {
            const int d = std::min(remOver, outMR);
            outMR -= d;
            remOver -= d;
        }
        if (remOver > 0 && outML > 0) {
            const int d = std::min(remOver, outML);
            outML -= d;
        }
    }
    const int remH = std::max(0, targetSlotH - outH);
    const int baseMT = std::min(remH, marginPixelSize(mtHu, hHu, outH, percent));
    const int baseMB = std::min(std::max(0, remH - baseMT), marginPixelSize(mbHu, hHu, outH, percent));
    const int extraH = std::max(0, remH - baseMT - baseMB);
    outMT = baseMT + extraH / 2;
    outMB = remH - outMT;
}

void measureSpecFitWidth(const PageSpec& spec, double scale, int slotW, int percent, int& outW, int& outH, int& outML,
                         int& outMR, int& outMT, int& outMB) {
    const int wHu = std::max(0, spec.widthHu);
    const int hHu = std::max(0, spec.heightHu);
    const int mlHu = std::max(0, spec.marginLeftHu);
    const int mrHu = std::max(0, spec.marginRightHu);
    const int mtHu = std::max(0, spec.marginTopHu);
    const int mbHu = std::max(0, spec.marginBottomHu);
    outH = hHu > 0 ? std::max(1, roundPositive(static_cast<double>(hHu) * scale)) : 0;
    outW = hHu > 0 ? pagePixelWidth(wHu, hHu, outH) : 0;
    outML = marginPixelSize(mlHu, hHu, outH, percent);
    outMR = marginPixelSize(mrHu, hHu, outH, percent);
    outMT = marginPixelSize(mtHu, hHu, outH, percent);
    outMB = marginPixelSize(mbHu, hHu, outH, percent);
    if (slotW > 0 && outML + outW + outMR > slotW) {
        const int overW = (outML + outW + outMR) - slotW;
        const int shrinkW = std::min(overW, std::max(0, outW - 1));
        outW -= shrinkW;
        int remOver = overW - shrinkW;
        if (remOver > 0 && outMR > 0) {
            const int d = std::min(remOver, outMR);
            outMR -= d;
            remOver -= d;
        }
        if (remOver > 0 && outML > 0) {
            const int d = std::min(remOver, outML);
            outML -= d;
        }
    }
}

}  // namespace

const PageView::Sheet* PageView::findSheet(int index) const {
    for (const Sheet& sheet : sheets_) {
        if (sheet.index == index) return &sheet;
    }
    return nullptr;
}

const PageView::Sheet* PageView::findFocusSheet(double focusX, double focusY) const {
    if (sheets_.empty()) return nullptr;
    const Sheet* best = &sheets_.front();
    double bestOuterDist2 = std::numeric_limits<double>::max();
    double bestPaperDist2 = std::numeric_limits<double>::max();
    for (const Sheet& sheet : sheets_) {
        const double oLeft = static_cast<double>(sheet.outerX());
        const double oRight = oLeft + static_cast<double>(sheet.outerW());
        const double oTop = static_cast<double>(sheet.outerY());
        const double oBottom = oTop + static_cast<double>(sheet.outerH());
        double odx = 0.0;
        if (focusX < oLeft) odx = oLeft - focusX;
        else if (focusX > oRight) odx = focusX - oRight;
        double ody = 0.0;
        if (focusY < oTop) ody = oTop - focusY;
        else if (focusY > oBottom) ody = focusY - oBottom;
        const double outerDist2 = odx * odx + ody * ody;

        const double pLeft = static_cast<double>(sheet.x);
        const double pRight = pLeft + static_cast<double>(sheet.w);
        const double pTop = static_cast<double>(sheet.y);
        const double pBottom = pTop + static_cast<double>(sheet.h);
        double pdx = 0.0;
        if (focusX < pLeft) pdx = pLeft - focusX;
        else if (focusX > pRight) pdx = focusX - pRight;
        double pdy = 0.0;
        if (focusY < pTop) pdy = pTop - focusY;
        else if (focusY > pBottom) pdy = focusY - pBottom;
        const double paperDist2 = pdx * pdx + pdy * pdy;

        if (outerDist2 < bestOuterDist2 ||
            (outerDist2 == bestOuterDist2 && paperDist2 < bestPaperDist2)) {
            bestOuterDist2 = outerDist2;
            bestPaperDist2 = paperDist2;
            best = &sheet;
        }
    }
    return best;
}

bool PageView::sameSpecs(const std::vector<PageSpec>& pages) const {
    if (pages.size() != specs_.size()) return false;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (pages[i].index != specs_[i].index || pages[i].widthHu != specs_[i].widthHu ||
            pages[i].heightHu != specs_[i].heightHu || pages[i].marginLeftHu != specs_[i].marginLeftHu ||
            pages[i].marginRightHu != specs_[i].marginRightHu || pages[i].marginTopHu != specs_[i].marginTopHu ||
            pages[i].marginBottomHu != specs_[i].marginBottomHu) {
            return false;
        }
    }
    return true;
}

void PageView::layout() {
    viewW_ = std::max(1, roundPositive(static_cast<double>(cssW_) * dpr_));
    viewH_ = std::max(1, roundPositive(static_cast<double>(cssH_) * dpr_));
    // 눈금자는 뷰포트 위쪽과 왼쪽 띠다. 쪽은 그 띠를 뺀 자리에 맞추고, 다 놓은 뒤 띠만큼 민다.
    rulerPx_ = rulerPixels();
    const int roomW = std::max(1, viewW_ - rulerPx_);
    const int roomH = std::max(1, viewH_ - rulerPx_);
    sheets_.clear();
    rows_.clear();
    if (!stacked_) {
        effectiveCols_ = 1;
        PageSpec singleSpec;
        singleSpec.index = page_;
        singleSpec.widthHu = widthHu_;
        singleSpec.heightHu = heightHu_;
        singleSpec.marginLeftHu = marginLeftHu_;
        singleSpec.marginRightHu = marginRightHu_;
        singleSpec.marginTopHu = marginTopHu_;
        singleSpec.marginBottomHu = marginBottomHu_;
        int w = 0, h = 0, ml = 0, mr = 0, mt = 0, mb = 0;
        if (fitPageActive_ && singleSpec.outerWidthHu() > 0 && singleSpec.outerHeightHu() > 0) {
            const double scaleW = static_cast<double>(roomW) / static_cast<double>(singleSpec.outerWidthHu());
            const double scaleH = static_cast<double>(roomH) / static_cast<double>(singleSpec.outerHeightHu());
            const double scale = std::min(scaleW, scaleH);
            percent_ = std::clamp(roundPositive(scale * 100.0 * 7200.0 / 96.0), 10, 800);
            measureSpecFitPage(singleSpec, scale, roomW, roomH, percent_, w, h, ml, mr, mt, mb);
        } else if (fitWidthActive_ && singleSpec.outerWidthHu() > 0 && singleSpec.outerHeightHu() > 0) {
            const double scale = static_cast<double>(roomW) / static_cast<double>(singleSpec.outerWidthHu());
            percent_ = std::clamp(roundPositive(scale * 100.0 * 7200.0 / 96.0), 10, 800);
            measureSpecFitWidth(singleSpec, scale, roomW, percent_, w, h, ml, mr, mt, mb);
        } else {
            measureSpecAtPercent(singleSpec, percent_, w, h, ml, mr, mt, mb);
        }
        pixelH_ = std::max(1, h);
        contentW_ = heightHu_ > 0 ? (ml + w + mr) : 0;
        contentH_ = heightHu_ > 0 ? (mt + h + mb) : 0;
        rowStepY_ = fitPageActive_ ? roomH : (contentH_ > 0 ? (contentH_ + gapPx_) : 0);
        pageStepY_ = roomH;
        if (page_ >= 0 && w > 0 && h > 0) {
            const int span = std::max(contentW_, roomW);
            const int outerX = (fitPageActive_ || fitWidthActive_) ? std::max(0, (span - contentW_) / 2) : 0;
            Sheet sheet;
            sheet.index = page_;
            sheet.widthHu = widthHu_;
            sheet.heightHu = heightHu_;
            sheet.marginLeftHu = marginLeftHu_;
            sheet.marginRightHu = marginRightHu_;
            sheet.marginTopHu = marginTopHu_;
            sheet.marginBottomHu = marginBottomHu_;
            sheet.x = outerX + ml;
            sheet.y = mt;
            sheet.w = w;
            sheet.h = h;
            sheet.marginLeft = ml;
            sheet.marginRight = mr;
            sheet.marginTop = mt;
            sheet.marginBottom = mb;
            sheet.row = 0;
            sheet.col = 0;
            sheets_.push_back(sheet);
            rows_.push_back(RowBand{0, contentH_, rowStepY_});
            if (fitPageActive_ || fitWidthActive_) contentW_ = span;
        }
    } else {
        struct Sized {
            PageSpec spec;
            int w = 0;
            int h = 0;
            int ml = 0;
            int mr = 0;
            int mt = 0;
            int mb = 0;
            int outerW() const { return ml + w + mr; }
            int outerH() const { return mt + h + mb; }
        };
        std::vector<Sized> sized;
        sized.reserve(specs_.size());
        int maxOuterWidthHu = 0;
        int maxOuterHeightHu = 0;
        for (const PageSpec& spec : specs_) {
            Sized item;
            item.spec = spec;
            item.spec.widthHu = std::max(0, spec.widthHu);
            item.spec.heightHu = std::max(0, spec.heightHu);
            item.spec.marginLeftHu = std::max(0, spec.marginLeftHu);
            item.spec.marginRightHu = std::max(0, spec.marginRightHu);
            item.spec.marginTopHu = std::max(0, spec.marginTopHu);
            item.spec.marginBottomHu = std::max(0, spec.marginBottomHu);
            maxOuterWidthHu = std::max(maxOuterWidthHu, item.spec.outerWidthHu());
            maxOuterHeightHu = std::max(maxOuterHeightHu, item.spec.outerHeightHu());
            sized.push_back(item);
        }
        const int pageCount = static_cast<int>(sized.size());
        if (pageCount <= 0) {
            effectiveCols_ = 1;
            contentW_ = 0;
            contentH_ = 0;
            pixelH_ = 1;
            rowStepY_ = 0;
            pageStepY_ = 0;
        } else if (fitPageActive_ && maxOuterWidthHu > 0 && maxOuterHeightHu > 0) {
            const int stepRows = (gridMode_ == PageGridMode::Grid) ? std::max(1, gridRows_) : 1;
            if (gridMode_ == PageGridMode::Single) {
                effectiveCols_ = 1;
            } else if (gridMode_ == PageGridMode::Two) {
                effectiveCols_ = std::min(2, pageCount);
            } else if (gridMode_ == PageGridMode::Grid) {
                effectiveCols_ = std::clamp(gridCols_, 1, pageCount);
            } else {
                const double scale1 = std::min(static_cast<double>(roomW) / static_cast<double>(maxOuterWidthHu),
                                               static_cast<double>(roomH) / static_cast<double>(maxOuterHeightHu));
                const int autoOuterW = std::max(1, roundPositive(static_cast<double>(maxOuterWidthHu) * scale1));
                const int denom = std::max(1, autoOuterW + gapPx_);
                effectiveCols_ = std::clamp((roomW + gapPx_) / denom, 1, pageCount);
            }
            const int cols = std::max(1, effectiveCols_);
            const int numRows = (pageCount + cols - 1) / cols;
            const int availW = std::max(1, roomW - (cols - 1) * gapPx_);
            const int availH = std::max(1, roomH - (stepRows - 1) * gapPx_);
            const double slotW = static_cast<double>(availW) / static_cast<double>(cols);
            const double slotH = static_cast<double>(availH) / static_cast<double>(stepRows);
            const double scale =
                std::min(slotW / static_cast<double>(maxOuterWidthHu), slotH / static_cast<double>(maxOuterHeightHu));
            percent_ = std::clamp(roundPositive(scale * 100.0 * 7200.0 / 96.0), 10, 800);
            const int slotWInt = std::max(1, availW / cols);

            auto slotTopForRow = [&](int r) {
                return roundPositive(static_cast<double>(static_cast<long long>(r) * roomH) /
                                     static_cast<double>(stepRows));
            };

            struct RowDraft {
                int start = 0;
                int count = 0;
                int y = 0;
                int width = 0;
                int height = 0;
            };
            std::vector<RowDraft> rowDrafts;
            rowDrafts.reserve(static_cast<std::size_t>(numRows));
            int maxRowW = 0;
            for (int r = 0; r < numRows; ++r) {
                RowDraft rd;
                rd.start = r * cols;
                rd.count = std::min(cols, pageCount - rd.start);
                rd.y = slotTopForRow(r);
                const int nextY = slotTopForRow(r + 1);
                rd.height = std::max(1, nextY - rd.y);
                int rowMtHu = 0;
                int rowMbHu = 0;
                for (int c = 0; c < rd.count; ++c) {
                    const Sized& item = sized[static_cast<std::size_t>(rd.start + c)];
                    rowMtHu = std::max(rowMtHu, item.spec.marginTopHu);
                    rowMbHu = std::max(rowMbHu, item.spec.marginBottomHu);
                }
                for (int c = 0; c < rd.count; ++c) {
                    Sized& item = sized[static_cast<std::size_t>(rd.start + c)];
                    PageSpec rowSpec = item.spec;
                    rowSpec.marginTopHu = rowMtHu;
                    rowSpec.marginBottomHu = rowMbHu;
                    measureSpecFitPage(rowSpec, scale, slotWInt, rd.height, percent_, item.w, item.h, item.ml,
                                       item.mr, item.mt, item.mb);
                    rd.width += item.outerW();
                    if (c > 0) rd.width += gapPx_;
                }
                maxRowW = std::max(maxRowW, rd.width);
                rowDrafts.push_back(rd);
            }

            const int span = std::max(maxRowW, roomW);
            const int gridLeft = std::max(0, (span - maxRowW) / 2);
            rows_.reserve(static_cast<std::size_t>(numRows));
            for (int r = 0; r < numRows; ++r) {
                const RowDraft& rd = rowDrafts[static_cast<std::size_t>(r)];
                rows_.push_back(RowBand{rd.y, rd.height, rd.height});
                int x = (cols == 1 || numRows == 1) ? std::max(0, (span - rd.width) / 2) : gridLeft;
                for (int c = 0; c < rd.count; ++c) {
                    const Sized& item = sized[static_cast<std::size_t>(rd.start + c)];
                    const int outerX = (cols == 1) ? std::max(0, (span - item.outerW()) / 2) : x;
                    const int outerY = rd.y;
                    Sheet sheet;
                    sheet.index = item.spec.index;
                    sheet.widthHu = item.spec.widthHu;
                    sheet.heightHu = item.spec.heightHu;
                    sheet.marginLeftHu = item.spec.marginLeftHu;
                    sheet.marginRightHu = item.spec.marginRightHu;
                    sheet.marginTopHu = item.spec.marginTopHu;
                    sheet.marginBottomHu = item.spec.marginBottomHu;
                    sheet.x = outerX + item.ml;
                    sheet.y = outerY + item.mt;
                    sheet.w = item.w;
                    sheet.h = item.h;
                    sheet.marginLeft = item.ml;
                    sheet.marginRight = item.mr;
                    sheet.marginTop = item.mt;
                    sheet.marginBottom = item.mb;
                    sheet.row = r;
                    sheet.col = c;
                    sheets_.push_back(sheet);
                    x += item.outerW() + gapPx_;
                }
            }
            int totalH = rows_.empty() ? 0 : (rows_.back().y + rows_.back().h);
            rowStepY_ = rows_.empty() ? 0 : rows_.front().step;
            pageStepY_ = roomH;
            if (totalH > roomH && !rows_.empty()) {
                const int lastAnchorRow = std::max(0, numRows - stepRows);
                totalH = std::max(totalH, rows_[static_cast<std::size_t>(lastAnchorRow)].y + roomH);
            }
            contentW_ = span;
            contentH_ = totalH;
            pixelH_ = std::max(1, sized.front().h);
        } else {
            int maxOuterW = 0;
            if (fitWidthActive_ && maxOuterWidthHu > 0 && maxOuterHeightHu > 0) {
                int targetCols = 1;
                if (gridMode_ == PageGridMode::Two) targetCols = std::min(2, pageCount);
                else if (gridMode_ == PageGridMode::Grid) targetCols = std::clamp(gridCols_, 1, pageCount);
                const int availW = std::max(1, roomW - (targetCols - 1) * gapPx_);
                const double slotW = static_cast<double>(availW) / static_cast<double>(targetCols);
                const double scale = slotW / static_cast<double>(maxOuterWidthHu);
                percent_ = std::clamp(roundPositive(scale * 100.0 * 7200.0 / 96.0), 10, 800);
                const int slotWInt = std::max(1, availW / targetCols);
                for (Sized& item : sized) {
                    measureSpecFitWidth(item.spec, scale, slotWInt, percent_, item.w, item.h, item.ml, item.mr,
                                        item.mt, item.mb);
                    maxOuterW = std::max(maxOuterW, item.outerW());
                }
                effectiveCols_ = targetCols;
            } else {
                for (Sized& item : sized) {
                    measureSpecAtPercent(item.spec, percent_, item.w, item.h, item.ml, item.mr, item.mt, item.mb);
                    maxOuterW = std::max(maxOuterW, item.outerW());
                }
                if (gridMode_ == PageGridMode::Single) {
                    effectiveCols_ = 1;
                } else if (gridMode_ == PageGridMode::Two) {
                    effectiveCols_ = std::min(2, pageCount);
                } else if (gridMode_ == PageGridMode::Grid) {
                    effectiveCols_ = std::clamp(gridCols_, 1, pageCount);
                } else {
                    const int denom = std::max(1, maxOuterW + gapPx_);
                    effectiveCols_ = std::clamp((roomW + gapPx_) / denom, 1, pageCount);
                }
            }
            const int cols = std::max(1, effectiveCols_);
            const int numRows = (pageCount + cols - 1) / cols;
            struct RowDraft {
                int start = 0;
                int count = 0;
                int width = 0;
                int height = 0;
                int mt = 0;
                int paperH = 0;
                int mb = 0;
            };
            std::vector<RowDraft> rowDrafts;
            rowDrafts.reserve(static_cast<std::size_t>(numRows));
            int maxRowW = 0;
            for (int r = 0; r < numRows; ++r) {
                RowDraft rd;
                rd.start = r * cols;
                rd.count = std::min(cols, pageCount - rd.start);
                for (int c = 0; c < rd.count; ++c) {
                    const Sized& item = sized[static_cast<std::size_t>(rd.start + c)];
                    rd.width += item.outerW();
                    if (c > 0) rd.width += gapPx_;
                    rd.mt = std::max(rd.mt, item.mt);
                    rd.paperH = std::max(rd.paperH, item.h);
                    rd.mb = std::max(rd.mb, item.mb);
                }
                rd.height = rd.mt + rd.paperH + rd.mb;
                maxRowW = std::max(maxRowW, rd.width);
                rowDrafts.push_back(rd);
            }

            const int span = std::max(maxRowW, roomW);
            const int gridLeft = std::max(0, (span - maxRowW) / 2);
            int y = 0;
            rows_.reserve(static_cast<std::size_t>(numRows));
            for (int r = 0; r < numRows; ++r) {
                const RowDraft& rd = rowDrafts[static_cast<std::size_t>(r)];
                const int step = rd.height + gapPx_;
                rows_.push_back(RowBand{y, rd.height, step});
                int x = (cols == 1 || numRows == 1) ? std::max(0, (span - rd.width) / 2) : gridLeft;
                for (int c = 0; c < rd.count; ++c) {
                    const Sized& item = sized[static_cast<std::size_t>(rd.start + c)];
                    const int outerX = (cols == 1) ? std::max(0, (span - item.outerW()) / 2) : x;
                    const int paperOffsetY = (rd.paperH - item.h) / 2;
                    const int effMt = rd.mt + paperOffsetY;
                    const int effMb = std::max(0, rd.height - effMt - item.h);
                    Sheet sheet;
                    sheet.index = item.spec.index;
                    sheet.widthHu = item.spec.widthHu;
                    sheet.heightHu = item.spec.heightHu;
                    sheet.marginLeftHu = item.spec.marginLeftHu;
                    sheet.marginRightHu = item.spec.marginRightHu;
                    sheet.marginTopHu = item.spec.marginTopHu;
                    sheet.marginBottomHu = item.spec.marginBottomHu;
                    sheet.x = outerX + item.ml;
                    sheet.y = y + effMt;
                    sheet.w = item.w;
                    sheet.h = item.h;
                    sheet.marginLeft = item.ml;
                    sheet.marginRight = item.mr;
                    sheet.marginTop = effMt;
                    sheet.marginBottom = effMb;
                    sheet.row = r;
                    sheet.col = c;
                    sheets_.push_back(sheet);
                    x += item.outerW() + gapPx_;
                }
                y += rd.height;
                if (r + 1 < numRows) y += gapPx_;
            }
            rowStepY_ = rows_.empty() ? 0 : rows_.front().step;
            pageStepY_ = roomH;
            contentW_ = span;
            contentH_ = y;
            pixelH_ = std::max(1, sized.front().h);
        }
    }
    // 줄 띠(rows_)는 스크롤 값으로 쓰므로 밀지 않는다. 스크롤 y 가 띠 y 이면 그 줄이 눈금자 바로 아래에 온다.
    if (rulerPx_ > 0) {
        for (Sheet& sheet : sheets_) {
            sheet.x += rulerPx_;
            sheet.y += rulerPx_;
        }
        if (contentW_ > 0) contentW_ += rulerPx_;
        if (contentH_ > 0) contentH_ += rulerPx_;
    }
    maxScrollX_ = std::max(0, contentW_ - viewW_);
    maxScrollY_ = std::max(0, contentH_ - viewH_);
    scrollX_ = std::clamp(scrollX_, 0, maxScrollX_);
    scrollY_ = std::clamp(scrollY_, 0, maxScrollY_);
}

void PageView::dropAt(std::size_t index) {
    drops_.push_back(tiles_[index].id);
    tiles_.erase(tiles_.begin() + static_cast<std::ptrdiff_t>(index));
}

void PageView::reset() {
    for (const Tile& tile : tiles_) drops_.push_back(tile.id);
    std::vector<int> drops = std::move(drops_);
    const bool rulers = rulers_;
    *this = PageView{};
    drops_ = std::move(drops);
    rulers_ = rulers;
}

void PageView::setViewport(int cssW, int cssH, double dpr) {
    cssW = std::max(1, cssW);
    cssH = std::max(1, cssH);
    if (!(dpr > 0)) dpr = 1;
    if (cssW_ == cssW && cssH_ == cssH && dpr_ == dpr) return;
    cssW_ = cssW;
    cssH_ = cssH;
    dpr_ = dpr;
    zoomAnchorValid_ = false;
    if (fitPageActive_ || fitWidthActive_) {
        ++generation_;
        inflight_ = {};
    }
    layout();
}

void PageView::setPage(int page, int widthHu, int heightHu) {
    if (!stacked_ && page_ == page && widthHu_ == widthHu && heightHu_ == heightHu) return;
    for (const Tile& tile : tiles_) drops_.push_back(tile.id);
    tiles_.clear();
    inflight_ = {};
    stacked_ = false;
    gapPx_ = 0;
    specs_.clear();
    page_ = page;
    widthHu_ = std::max(0, widthHu);
    heightHu_ = std::max(0, heightHu);
    scrollX_ = 0;
    scrollY_ = 0;
    lastScrollX_ = 0;
    lastScrollY_ = 0;
    zoomAnchorValid_ = false;
    ++generation_;
    layout();
}

void PageView::setPageMargins(int marginLeftHu, int marginRightHu, int marginTopHu, int marginBottomHu) {
    marginLeftHu = std::max(0, marginLeftHu);
    marginRightHu = std::max(0, marginRightHu);
    marginTopHu = std::max(0, marginTopHu);
    marginBottomHu = std::max(0, marginBottomHu);
    if (marginLeftHu_ == marginLeftHu && marginRightHu_ == marginRightHu && marginTopHu_ == marginTopHu &&
        marginBottomHu_ == marginBottomHu) {
        return;
    }
    marginLeftHu_ = marginLeftHu;
    marginRightHu_ = marginRightHu;
    marginTopHu_ = marginTopHu;
    marginBottomHu_ = marginBottomHu;
    zoomAnchorValid_ = false;
    ++generation_;
    layout();
}

void PageView::setPages(const std::vector<PageSpec>& pages, int gapPx) {
    gapPx = std::max(0, gapPx);
    if (stacked_ && gapPx_ == gapPx && sameSpecs(pages)) return;
    zoomAnchorValid_ = false;
    const bool sameDoc = stacked_ && sameSpecs(pages);
    if (!sameDoc) {
        for (const Tile& tile : tiles_) drops_.push_back(tile.id);
        tiles_.clear();
        inflight_ = {};
        scrollX_ = 0;
        scrollY_ = 0;
        lastScrollX_ = 0;
        lastScrollY_ = 0;
        ++generation_;
    } else {
        const double ry = maxScrollY_ > 0 ? static_cast<double>(scrollY_) / static_cast<double>(maxScrollY_) : 0;
        const double rx = maxScrollX_ > 0 ? static_cast<double>(scrollX_) / static_cast<double>(maxScrollX_) : 0;
        gapPx_ = gapPx;
        specs_.clear();
        for (const PageSpec& spec : pages) {
            PageSpec copy = spec;
            copy.widthHu = std::max(0, spec.widthHu);
            copy.heightHu = std::max(0, spec.heightHu);
            copy.marginLeftHu = std::max(0, spec.marginLeftHu);
            copy.marginRightHu = std::max(0, spec.marginRightHu);
            copy.marginTopHu = std::max(0, spec.marginTopHu);
            copy.marginBottomHu = std::max(0, spec.marginBottomHu);
            specs_.push_back(copy);
        }
        stacked_ = true;
        page_ = specs_.empty() ? -1 : specs_.front().index;
        layout();
        scrollX_ = std::clamp(roundPositive(rx * static_cast<double>(maxScrollX_)), 0, maxScrollX_);
        scrollY_ = std::clamp(roundPositive(ry * static_cast<double>(maxScrollY_)), 0, maxScrollY_);
        lastScrollX_ = scrollX_;
        lastScrollY_ = scrollY_;
        return;
    }
    gapPx_ = gapPx;
    specs_.clear();
    for (const PageSpec& spec : pages) {
        PageSpec copy = spec;
        copy.widthHu = std::max(0, spec.widthHu);
        copy.heightHu = std::max(0, spec.heightHu);
        copy.marginLeftHu = std::max(0, spec.marginLeftHu);
        copy.marginRightHu = std::max(0, spec.marginRightHu);
        copy.marginTopHu = std::max(0, spec.marginTopHu);
        copy.marginBottomHu = std::max(0, spec.marginBottomHu);
        specs_.push_back(copy);
    }
    stacked_ = true;
    page_ = specs_.empty() ? -1 : specs_.front().index;
    widthHu_ = specs_.empty() ? 0 : specs_.front().widthHu;
    heightHu_ = specs_.empty() ? 0 : specs_.front().heightHu;
    marginLeftHu_ = specs_.empty() ? 0 : specs_.front().marginLeftHu;
    marginRightHu_ = specs_.empty() ? 0 : specs_.front().marginRightHu;
    marginTopHu_ = specs_.empty() ? 0 : specs_.front().marginTopHu;
    marginBottomHu_ = specs_.empty() ? 0 : specs_.front().marginBottomHu;
    layout();
}

int PageView::rulerPixels() const {
    return rulers_ ? std::max(1, roundPositive(static_cast<double>(kRulerCss) * dpr_)) : 0;
}

int PageView::fitWidthPercent() const {
    const int vW = std::max(1, roundPositive(static_cast<double>(cssW_) * dpr_) - rulerPixels());
    int cols = 1;
    if (stacked_ && !specs_.empty()) {
        const int pageCount = static_cast<int>(specs_.size());
        if (gridMode_ == PageGridMode::Two) cols = std::min(2, pageCount);
        else if (gridMode_ == PageGridMode::Grid) cols = std::max(1, gridCols_);
    }
    std::vector<PageSpec> targetSpecs;
    if (stacked_ && !specs_.empty()) {
        targetSpecs = specs_;
    } else if (widthHu_ > 0 && heightHu_ > 0) {
        targetSpecs.push_back(PageSpec{page_, widthHu_, heightHu_, marginLeftHu_, marginRightHu_, marginTopHu_, marginBottomHu_});
    }
    if (targetSpecs.empty()) return percent_;
    int maxOuterWidthHu = 0;
    for (const PageSpec& s : targetSpecs) {
        maxOuterWidthHu = std::max(maxOuterWidthHu, s.outerWidthHu());
    }
    if (maxOuterWidthHu <= 0) return percent_;
    const int availW = std::max(1, vW - (cols - 1) * gapPx_);
    const double slotW = static_cast<double>(availW) / static_cast<double>(cols);
    const double rawPercent = (slotW * 100.0 * 7200.0) / (static_cast<double>(maxOuterWidthHu) * 96.0);
    auto fitsAt = [&](int p) {
        int maxOuterW = 0;
        for (const PageSpec& s : targetSpecs) {
            int w = 0, h = 0, ml = 0, mr = 0, mt = 0, mb = 0;
            measureSpecAtPercent(s, p, w, h, ml, mr, mt, mb);
            maxOuterW = std::max(maxOuterW, ml + w + mr);
        }
        return cols * maxOuterW + (cols - 1) * gapPx_ <= vW;
    };
    int p = std::clamp(roundPositive(rawPercent), 10, 800);
    while (p < 800 && fitsAt(p + 1)) ++p;
    while (p > 10 && !fitsAt(p)) --p;
    return p;
}

int PageView::fitPagePercent() const {
    const int vW = std::max(1, roundPositive(static_cast<double>(cssW_) * dpr_) - rulerPixels());
    const int vH = std::max(1, roundPositive(static_cast<double>(cssH_) * dpr_) - rulerPixels());
    int cols = 1;
    int rows = 1;
    if (stacked_ && !specs_.empty()) {
        const int pageCount = static_cast<int>(specs_.size());
        if (gridMode_ == PageGridMode::Two) {
            cols = std::min(2, pageCount);
            rows = 1;
        } else if (gridMode_ == PageGridMode::Grid) {
            cols = std::max(1, gridCols_);
            rows = std::max(1, gridRows_);
        }
    }
    std::vector<PageSpec> targetSpecs;
    if (stacked_ && !specs_.empty()) {
        targetSpecs = specs_;
    } else if (widthHu_ > 0 && heightHu_ > 0) {
        targetSpecs.push_back(PageSpec{page_, widthHu_, heightHu_, marginLeftHu_, marginRightHu_, marginTopHu_, marginBottomHu_});
    }
    if (targetSpecs.empty()) return percent_;
    int maxOuterWidthHu = 0;
    int maxOuterHeightHu = 0;
    for (const PageSpec& s : targetSpecs) {
        maxOuterWidthHu = std::max(maxOuterWidthHu, s.outerWidthHu());
        maxOuterHeightHu = std::max(maxOuterHeightHu, s.outerHeightHu());
    }
    if (maxOuterWidthHu <= 0 || maxOuterHeightHu <= 0) return percent_;
    const int availW = std::max(1, vW - (cols - 1) * gapPx_);
    const int availH = std::max(1, vH - (rows - 1) * gapPx_);
    const double slotW = static_cast<double>(availW) / static_cast<double>(cols);
    const double slotH = static_cast<double>(availH) / static_cast<double>(rows);
    const double widthPercent = (slotW * 100.0 * 7200.0) / (static_cast<double>(maxOuterWidthHu) * 96.0);
    const double heightPercent = (slotH * 100.0 * 7200.0) / (static_cast<double>(maxOuterHeightHu) * 96.0);
    auto fitsAt = [&](int p) {
        int maxOuterW = 0;
        int maxOuterH = 0;
        for (const PageSpec& s : targetSpecs) {
            int w = 0, h = 0, ml = 0, mr = 0, mt = 0, mb = 0;
            measureSpecAtPercent(s, p, w, h, ml, mr, mt, mb);
            maxOuterW = std::max(maxOuterW, ml + w + mr);
            maxOuterH = std::max(maxOuterH, mt + h + mb);
        }
        return (cols * maxOuterW + (cols - 1) * gapPx_ <= vW) && (rows * maxOuterH + (rows - 1) * gapPx_ <= vH);
    };
    int p = std::clamp(roundPositive(std::min(widthPercent, heightPercent)), 10, 800);
    while (p < 800 && fitsAt(p + 1)) ++p;
    while (p > 10 && !fitsAt(p)) --p;
    return p;
}

void PageView::fitWidth() {
    const double focusX = static_cast<double>(scrollX_) + static_cast<double>(viewW_ + rulerPx_) / 2.0;
    const double focusY = static_cast<double>(scrollY_) + static_cast<double>(viewH_ + rulerPx_) / 2.0;
    const Sheet* focus = findFocusSheet(focusX, focusY);
    const int focusPage = focus ? focus->index : -1;
    double localY = 0.5;
    if (focus && focus->outerH() > 0) {
        localY = (focusY - static_cast<double>(focus->outerY())) / static_cast<double>(focus->outerH());
    }
    fitPageActive_ = false;
    fitWidthActive_ = true;
    percent_ = std::clamp(fitWidthPercent(), 10, 800);
    layout();
    scrollX_ = 0;
    const Sheet* placed = focusPage >= 0 ? findSheet(focusPage) : nullptr;
    if (placed && placed->outerH() > 0) {
        const double targetY =
            static_cast<double>(placed->outerY()) + localY * static_cast<double>(placed->outerH()) -
            static_cast<double>(viewH_ + rulerPx_) / 2.0;
        scrollY_ = std::clamp(roundPositive(targetY), 0, maxScrollY_);
    } else {
        scrollY_ = std::clamp(scrollY_, 0, maxScrollY_);
    }
    lastScrollX_ = scrollX_;
    lastScrollY_ = scrollY_;
    zoomAnchorValid_ = false;
    ++generation_;
    inflight_ = {};
}

void PageView::fitPage() {
    const double focusX = static_cast<double>(scrollX_) + static_cast<double>(viewW_ + rulerPx_) / 2.0;
    const double focusY = static_cast<double>(scrollY_) + static_cast<double>(viewH_ + rulerPx_) / 2.0;
    const Sheet* focus = findFocusSheet(focusX, focusY);
    const int focusPage = focus ? focus->index : -1;
    fitWidthActive_ = false;
    fitPageActive_ = true;
    percent_ = std::clamp(fitPagePercent(), 10, 800);
    layout();
    scrollX_ = 0;
    const Sheet* placed = focusPage >= 0 ? findSheet(focusPage) : nullptr;
    if (placed && placed->row >= 0 && placed->row < static_cast<int>(rows_.size())) {
        const int stepRows = (gridMode_ == PageGridMode::Grid) ? std::max(1, gridRows_) : 1;
        const int alignedRow = (placed->row / stepRows) * stepRows;
        scrollY_ = std::clamp(rows_[static_cast<std::size_t>(alignedRow)].y, 0, maxScrollY_);
    } else {
        scrollY_ = std::clamp(scrollY_, 0, maxScrollY_);
    }
    lastScrollX_ = scrollX_;
    lastScrollY_ = scrollY_;
    zoomAnchorValid_ = false;
    ++generation_;
    inflight_ = {};
}

void PageView::setGridMode(PageGridMode mode, int cols, int rows, bool fit) {
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    if (mode == PageGridMode::Single) {
        cols = 1;
        rows = 1;
    } else if (mode == PageGridMode::Two) {
        cols = 2;
        rows = 1;
    }
    const double focusX = static_cast<double>(scrollX_) + static_cast<double>(viewW_ + rulerPx_) / 2.0;
    const double focusY = static_cast<double>(scrollY_) + static_cast<double>(viewH_ + rulerPx_) / 2.0;
    const Sheet* focus = findFocusSheet(focusX, focusY);
    const int focusPage = focus ? focus->index : -1;
    double localY = 0.0;
    if (focus && focus->outerH() > 0) {
        localY = (focusY - static_cast<double>(focus->outerY())) / static_cast<double>(focus->outerH());
    }

    gridMode_ = mode;
    gridCols_ = cols;
    gridRows_ = rows;
    zoomAnchorValid_ = false;

    if (fit) {
        fitPage();
        return;
    }
    fitPageActive_ = false;
    fitWidthActive_ = false;
    layout();
    const Sheet* placed = focusPage >= 0 ? findSheet(focusPage) : nullptr;
    if (placed) {
        const double targetY =
            static_cast<double>(placed->outerY()) + localY * static_cast<double>(placed->outerH()) -
            static_cast<double>(viewH_ + rulerPx_) / 2.0;
        scrollY_ = std::clamp(roundPositive(targetY), 0, maxScrollY_);
        scrollX_ = std::clamp(scrollX_, 0, maxScrollX_);
    }
    lastScrollX_ = scrollX_;
    lastScrollY_ = scrollY_;
    ++generation_;
    inflight_ = {};
}

void PageView::stepPage(int dir) {
    if (dir == 0) return;
    if (sheets_.empty() && (stacked_ ? !specs_.empty() : page_ >= 0)) layout();
    const int step = std::max(1, viewH_ - rulerPx_);
    scrollBy(0, dir > 0 ? step : -step);
}

void PageView::setZoomPercent(int percent) {
    percent = std::clamp(percent, 10, 800);
    if (!fitPageActive_ && !fitWidthActive_ && percent_ == percent && contentW_ > 0) return;
    fitPageActive_ = false;
    fitWidthActive_ = false;
    const int oldMaxX = maxScrollX_;
    const int oldMaxY = maxScrollY_;
    const int oldScrollX = scrollX_;
    const int oldScrollY = scrollY_;
    const int prevCols = effectiveCols_;
    const double viewX = static_cast<double>(viewW_ + rulerPx_) / 2.0;
    const double viewY = static_cast<double>(viewH_ + rulerPx_) / 2.0;
    const double focusX = static_cast<double>(scrollX_) + viewX;
    const double focusY = static_cast<double>(scrollY_) + viewY;
    int focusPage = -1;
    double localX = 0.5;
    double localY = 0.5;
    const Sheet* focus = findFocusSheet(focusX, focusY);
    if (focus && focus->w > 0 && focus->h > 0) {
        focusPage = focus->index;
        localX = (focusX - static_cast<double>(focus->x)) / static_cast<double>(focus->w);
        localY = (focusY - static_cast<double>(focus->y)) / static_cast<double>(focus->h);
    }
    percent_ = percent;
    layout();
    const Sheet* sheet = focusPage >= 0 ? findSheet(focusPage) : nullptr;
    const bool colsChanged = prevCols != effectiveCols_;
    auto anchored = [&](int oldScroll, int oldMax, int newMax, bool horizontal) {
        if (!colsChanged && effectiveCols_ <= 1) {
            if (oldMax <= 0) return newMax / 2;
            if (oldScroll >= oldMax) return newMax;
        }
        if (!sheet) return std::clamp(oldScroll, 0, newMax);
        const double local = horizontal ? localX : localY;
        const double size = horizontal ? static_cast<double>(sheet->w) : static_cast<double>(sheet->h);
        const double origin = horizontal ? static_cast<double>(sheet->x) : static_cast<double>(sheet->y);
        const double anchor = horizontal ? viewX : viewY;
        return std::clamp(roundPositive(origin + local * size - anchor), 0, newMax);
    };
    scrollX_ = anchored(oldScrollX, oldMaxX, maxScrollX_, true);
    scrollY_ = anchored(oldScrollY, oldMaxY, maxScrollY_, false);
    lastScrollX_ = scrollX_;
    lastScrollY_ = scrollY_;
    zoomAnchorValid_ = false;
    ++generation_;
    inflight_ = {};
}

void PageView::clearZoomAnchor() { zoomAnchorValid_ = false; }

void PageView::zoomAbout(int percent, double viewX, double viewY) {
    percent = std::clamp(percent, 10, 800);
    viewX = std::clamp(viewX, 0.0, static_cast<double>(std::max(0, viewW_)));
    viewY = std::clamp(viewY, 0.0, static_cast<double>(std::max(0, viewH_)));
    if (!fitPageActive_ && !fitWidthActive_ && percent_ == percent && contentW_ > 0) return;
    fitPageActive_ = false;
    fitWidthActive_ = false;
    if (!zoomAnchorValid_ || std::abs(viewX - zoomAnchorViewX_) > 0.5 || std::abs(viewY - zoomAnchorViewY_) > 0.5) {
        zoomAnchorOldMaxX_ = maxScrollX_;
        zoomAnchorOldMaxY_ = maxScrollY_;
        zoomAnchorOldScrollX_ = scrollX_;
        zoomAnchorOldScrollY_ = scrollY_;
        const double focusX = static_cast<double>(scrollX_) + viewX;
        const double focusY = static_cast<double>(scrollY_) + viewY;
        zoomAnchorPage_ = -1;
        zoomAnchorLocalX_ = 0.5;
        zoomAnchorLocalY_ = 0.5;
        const Sheet* focus = findFocusSheet(focusX, focusY);
        if (focus && focus->w > 0 && focus->h > 0) {
            zoomAnchorPage_ = focus->index;
            zoomAnchorLocalX_ = (focusX - static_cast<double>(focus->x)) / static_cast<double>(focus->w);
            zoomAnchorLocalY_ = (focusY - static_cast<double>(focus->y)) / static_cast<double>(focus->h);
        }
        zoomAnchorViewX_ = viewX;
        zoomAnchorViewY_ = viewY;
        zoomAnchorValid_ = true;
    }
    const int oldScrollX = zoomAnchorOldScrollX_;
    const int oldScrollY = zoomAnchorOldScrollY_;
    const int focusPage = zoomAnchorPage_;
    const double localX = zoomAnchorLocalX_;
    const double localY = zoomAnchorLocalY_;
    percent_ = percent;
    layout();
    const Sheet* sheet = focusPage >= 0 ? findSheet(focusPage) : nullptr;
    if (sheet && sheet->w > 0 && sheet->h > 0) {
        const double rawX = static_cast<double>(sheet->x) + localX * static_cast<double>(sheet->w) - viewX;
        const double rawY = static_cast<double>(sheet->y) + localY * static_cast<double>(sheet->h) - viewY;
        scrollX_ = std::clamp(roundPositive(rawX), 0, maxScrollX_);
        scrollY_ = std::clamp(roundPositive(rawY), 0, maxScrollY_);
        if (rawX < 0.0 || rawX > static_cast<double>(maxScrollX_)) {
            zoomAnchorLocalX_ =
                (static_cast<double>(scrollX_) + viewX - static_cast<double>(sheet->x)) / static_cast<double>(sheet->w);
        }
        if (rawY < 0.0 || rawY > static_cast<double>(maxScrollY_)) {
            zoomAnchorLocalY_ =
                (static_cast<double>(scrollY_) + viewY - static_cast<double>(sheet->y)) / static_cast<double>(sheet->h);
        }
    } else {
        scrollX_ = std::clamp(oldScrollX, 0, maxScrollX_);
        scrollY_ = std::clamp(oldScrollY, 0, maxScrollY_);
    }
    lastScrollX_ = scrollX_;
    lastScrollY_ = scrollY_;
    ++generation_;
    inflight_ = {};
}

void PageView::scrollBy(int dx, int dy) {
    const long long x = static_cast<long long>(scrollX_) + dx;
    const long long y = static_cast<long long>(scrollY_) + dy;
    const long long clampedX = std::clamp(x, 0ll, static_cast<long long>(std::max(0, maxScrollX_)));
    const long long clampedY = std::clamp(y, 0ll, static_cast<long long>(std::max(0, maxScrollY_)));
    setScroll(static_cast<int>(clampedX), static_cast<int>(clampedY));
}

void layoutScrollbars(Scrollbar& horizontal, Scrollbar& vertical, int viewW, int viewH, double dpr, int scrollX, int scrollY,
                      int maxScrollX, int maxScrollY, int contentW, int contentH, int ruler) {
    horizontal = {};
    ruler = std::max(0, ruler);
    vertical = {};
    if (!(dpr > 0)) dpr = 1;
    const int thickness = std::max(8, roundPositive(12.0 * dpr));
    const bool wantY = maxScrollY > 0 && contentH > viewH;
    const bool wantX = maxScrollX > 0 && contentW > viewW;
    auto place = [&](Scrollbar& bar, bool verticalBar, bool cross) {
        const bool wanted = verticalBar ? wantY : wantX;
        if (!wanted) return;
        bar.visible = true;
        if (verticalBar) {
            bar.w = thickness;
            bar.h = std::max(0, viewH - ruler - (cross ? thickness : 0));
            bar.x = std::max(0, viewW - thickness);
            bar.y = ruler;
            const int track = bar.h;
            int thumb = contentH > 0 ? roundPositive(static_cast<double>(viewH) / static_cast<double>(contentH) * track) : track;
            thumb = std::clamp(thumb, std::min(track, thickness * 2), std::max(0, track));
            const int travel = std::max(0, track - thumb);
            bar.thumbW = bar.w;
            bar.thumbH = thumb;
            bar.thumbX = bar.x;
            bar.thumbY = bar.y + (maxScrollY > 0 ? roundPositive(static_cast<double>(scrollY) / static_cast<double>(maxScrollY) * travel) : 0);
        } else {
            bar.h = thickness;
            bar.w = std::max(0, viewW - ruler - (cross ? thickness : 0));
            bar.x = ruler;
            bar.y = std::max(0, viewH - thickness);
            const int track = bar.w;
            int thumb = contentW > 0 ? roundPositive(static_cast<double>(viewW) / static_cast<double>(contentW) * track) : track;
            thumb = std::clamp(thumb, std::min(track, thickness * 2), std::max(0, track));
            const int travel = std::max(0, track - thumb);
            bar.thumbH = bar.h;
            bar.thumbW = thumb;
            bar.thumbY = bar.y;
            bar.thumbX = bar.x + (maxScrollX > 0 ? roundPositive(static_cast<double>(scrollX) / static_cast<double>(maxScrollX) * travel) : 0);
        }
    };
    place(vertical, true, wantX);
    place(horizontal, false, wantY);
}

void layoutRulers(const std::vector<ViewPage>& pages, int scrollX, int scrollY, int viewW, int viewH, int ruler,
                  std::vector<RulerSpan>& across, std::vector<RulerSpan>& down) {
    across.clear();
    down.clear();
    if (ruler <= 0) return;
    auto seenAcross = [&](const ViewPage& page) {
        const int x = page.x - scrollX;
        return page.w > 0 && page.widthHu > 0 && x < viewW && x + page.w > ruler;
    };
    auto seenDown = [&](const ViewPage& page) {
        const int y = page.y - scrollY;
        return page.h > 0 && page.heightHu > 0 && y < viewH && y + page.h > ruler;
    };
    int firstRow = -1;
    for (const ViewPage& page : pages) {
        if (!seenAcross(page) || !seenDown(page)) continue;
        if (firstRow < 0 || page.row < firstRow) firstRow = page.row;
    }
    int lastRow = -1;
    for (const ViewPage& page : pages) {
        if (page.row == firstRow && seenAcross(page)) {
            across.push_back(RulerSpan{page.x - scrollX, page.w, page.widthHu});
        }
        // 쪽은 줄 순서로 놓여 있다. 줄이 바뀌면 그 줄 첫 쪽을 세로 눈금자로 쓴다.
        if (page.row != lastRow && seenDown(page)) {
            down.push_back(RulerSpan{page.y - scrollY, page.h, page.heightHu});
            lastRow = page.row;
        }
    }
}

void PageView::scrollbars(Scrollbar& horizontal, Scrollbar& vertical) const {
    layoutScrollbars(horizontal, vertical, viewW_, viewH_, dpr_, scrollX_, scrollY_, maxScrollX_, maxScrollY_, contentW_,
                     contentH_, rulerPx_);
}

bool PageView::inRuler(double x, double y) const {
    return rulerPx_ > 0 && (x < static_cast<double>(rulerPx_) || y < static_cast<double>(rulerPx_));
}

void PageView::setRulers(bool on) {
    if (rulers_ == on) return;
    const int oldRuler = rulerPx_;
    rulers_ = on;
    layout();
    // 띠가 생기거나 빠진 만큼 스크롤을 옮겨 보던 자리가 그대로 남게 한다.
    const int shift = rulerPx_ - oldRuler;
    scrollX_ = std::clamp(scrollX_ + (scrollX_ > 0 ? shift : 0), 0, maxScrollX_);
    scrollY_ = std::clamp(scrollY_ + (scrollY_ > 0 ? shift : 0), 0, maxScrollY_);
    lastScrollX_ = scrollX_;
    lastScrollY_ = scrollY_;
    zoomAnchorValid_ = false;
    ++generation_;
    inflight_ = {};
}

ViewportState PageView::state() const {
    ViewportState out;
    out.scrollX = scrollX_;
    out.scrollY = scrollY_;
    out.maxScrollX = maxScrollX_;
    out.maxScrollY = maxScrollY_;
    out.percent = percent_;
    out.fitPage = fitPageActive_;
    out.fitWidth = fitWidthActive_;
    out.gridMode = gridMode_;
    out.gridCols = gridCols_;
    out.gridRows = gridRows_;
    out.effectiveCols = effectiveCols_;
    out.rowStepY = rowStepY_;
    out.pageStepY = pageStepY_;
    return out;
}

void PageView::setScroll(int x, int y) {
    if (sheets_.empty() && (stacked_ ? !specs_.empty() : page_ >= 0)) layout();
    x = std::clamp(x, 0, maxScrollX_);
    y = std::clamp(y, 0, maxScrollY_);
    if (x == scrollX_ && y == scrollY_) return;
    zoomAnchorValid_ = false;
    lastScrollX_ = scrollX_;
    lastScrollY_ = scrollY_;
    scrollX_ = x;
    scrollY_ = y;
    if (inflight_.id == 0) return;
    const Sheet* sheet = findSheet(inflight_.page);
    const ViewRect viewport{scrollX_, scrollY_, viewW_, viewH_};
    const ViewRect pageRect = sheet ? ViewRect{sheet->x, sheet->y, sheet->w, sheet->h} : ViewRect{};
    if (!sheet || inflight_.percent != percent_ || !intersects(pageRect, viewport)) inflight_ = {};
}

void PageView::setCacheLimit(int tiles) {
    cacheLimit_ = std::max(1, tiles);
}

bool PageView::commit(int id) {
    if (id == 0 || inflight_.id != id) return false;
    if (inflight_.generation != generation_) {
        inflight_ = {};
        return false;
    }
    Tile tile;
    tile.id = inflight_.id;
    tile.page = inflight_.page;
    tile.percent = inflight_.percent;
    tile.col = inflight_.col;
    tile.row = inflight_.row;
    tile.x = inflight_.x;
    tile.y = inflight_.y;
    tile.w = inflight_.w;
    tile.h = inflight_.h;
    tile.contentW = inflight_.contentW;
    tile.contentH = inflight_.contentH;
    tile.layer = inflight_.layer;
    tile.stamp = ++clock_;
    tiles_.push_back(tile);
    inflight_ = {};
    return true;
}

ViewPlan PageView::plan(bool includeTiles) {
    ViewPlan out;
    out.generation = generation_;
    out.dpr = dpr_;
    out.viewW = viewW_;
    out.viewH = viewH_;
    out.percent = percent_;
    out.fitPage = fitPageActive_;
    out.fitWidth = fitWidthActive_;
    out.contentW = contentW_;
    out.contentH = contentH_;
    out.scrollX = scrollX_;
    out.scrollY = scrollY_;
    out.maxScrollX = maxScrollX_;
    out.maxScrollY = maxScrollY_;
    out.paperX = -scrollX_;
    out.paperY = -scrollY_;
    out.paperW = contentW_;
    out.paperH = contentH_;
    out.gridMode = gridMode_;
    out.gridCols = gridCols_;
    out.gridRows = gridRows_;
    out.effectiveCols = effectiveCols_;
    out.rowStepY = rowStepY_;
    out.pageStepY = pageStepY_;
    out.ruler = rulerPx_;
    scrollbars(out.barX, out.barY);
    out.pages.reserve(sheets_.size());
    for (const Sheet& sheet : sheets_) {
        ViewPage page;
        page.index = sheet.index;
        page.x = sheet.x;
        page.y = sheet.y;
        page.w = sheet.w;
        page.h = sheet.h;
        page.widthHu = sheet.widthHu;
        page.heightHu = sheet.heightHu;
        page.marginLeft = sheet.marginLeft;
        page.marginRight = sheet.marginRight;
        page.marginTop = sheet.marginTop;
        page.marginBottom = sheet.marginBottom;
        page.row = sheet.row;
        page.col = sheet.col;
        out.pages.push_back(page);
    }
    if (!sheets_.empty()) {
        const double focusX = static_cast<double>(scrollX_) + static_cast<double>(viewW_ + rulerPx_) / 2.0;
        const double focusY = static_cast<double>(scrollY_) + static_cast<double>(viewH_ + rulerPx_) / 2.0;
        const Sheet* focus = findFocusSheet(focusX, focusY);
        out.page = focus ? focus->index : sheets_.front().index;
    }

    if (inflight_.id != 0 && inflight_.generation != generation_) inflight_ = {};
    if (!includeTiles || sheets_.empty() || contentW_ <= 0 || contentH_ <= 0) {
        out.drop = std::move(drops_);
        drops_.clear();
        return out;
    }

    const ViewRect viewport{scrollX_, scrollY_, viewW_, viewH_};
    const ViewRect screen{0, 0, viewW_, viewH_};
    int extraLeft = 0;
    int extraRight = 0;
    int extraTop = 0;
    int extraBottom = 0;
    if (scrollX_ > lastScrollX_) extraRight = kTile;
    else if (scrollX_ < lastScrollX_) extraLeft = kTile;
    if (scrollY_ > lastScrollY_) extraBottom = kTile;
    else if (scrollY_ < lastScrollY_) extraTop = kTile;
    const int margin = kTile * kMarginTiles;
    const ViewRect wanted{
        viewport.x - margin - extraLeft,
        viewport.y - margin - extraTop,
        viewport.w + margin * 2 + extraLeft + extraRight,
        viewport.h + margin * 2 + extraTop + extraBottom,
    };
    if (inflight_.id != 0) {
        const Sheet* sheet = findSheet(inflight_.page);
        const ViewRect content = sheet ? ViewRect{sheet->x + inflight_.x, sheet->y + inflight_.y, inflight_.w,
                                                   inflight_.h}
                                        : ViewRect{};
        if (!sheet || inflight_.percent != percent_ || !intersects(content, wanted)) inflight_ = {};
    }

    std::vector<Slot> visibleMissing;
    std::vector<Slot> marginMissing;
    bool covered = true;
    bool anyVisible = false;
    std::vector<ViewDraw> nearest;

    for (const Sheet& sheet : sheets_) {
        if (sheet.w <= 0 || sheet.h <= 0) continue;
        const ViewRect sheetRect{sheet.x, sheet.y, sheet.w, sheet.h};
        if (!intersects(sheetRect, wanted)) continue;
        for (int y = 0, row = 0; y < sheet.h; y += kTile, ++row) {
            for (int x = 0, col = 0; x < sheet.w; x += kTile, ++col) {
                const ViewRect local{x, y, std::min(kTile, sheet.w - x), std::min(kTile, sheet.h - y)};
                const ViewRect rect{sheet.x + local.x, sheet.y + local.y, local.w, local.h};
                const bool visible = intersects(rect, viewport);
                const bool near = intersects(rect, wanted);
                if (!visible && !near) continue;
                if (visible) anyVisible = true;
                for (int layer = 0; layer < kPaintLayerCount; ++layer) {
                    Tile* found = nullptr;
                    for (Tile& tile : tiles_) {
                        if (tile.page == sheet.index && tile.percent == percent_ && tile.contentW == sheet.w &&
                            tile.contentH == sheet.h && tile.col == col && tile.row == row && tile.layer == layer) {
                            found = &tile;
                            break;
                        }
                    }
                    if (!found) {
                        Slot slot;
                        slot.page = sheet.index;
                        slot.col = col;
                        slot.row = row;
                        slot.layer = layer;
                        slot.pixelH = sheet.h > 0 ? sheet.h : pagePixelHeight(sheet.heightHu, percent_);
                        slot.pageW = sheet.w;
                        slot.pageH = sheet.h;
                        slot.local = local;
                        slot.rect = rect;
                        if (visible) {
                            covered = false;
                            visibleMissing.push_back(slot);
                        } else {
                            marginMissing.push_back(slot);
                        }
                        continue;
                    }
                    found->stamp = ++clock_;
                    if (!visible) continue;
                    ViewDraw draw;
                    draw.id = found->id;
                    draw.x = rect.x - scrollX_;
                    draw.y = rect.y - scrollY_;
                    draw.w = rect.w;
                    draw.h = rect.h;
                    draw.layer = layer;
                    out.draw.push_back(draw);
                }
            }
        }
    }
    if (!anyVisible) covered = false;
    // 그리는 차례: 확대해 늘린 조각(nearest 아님)은 제 배율 조각 위에 얹고, 같은 것끼리는 층 차례다.
    std::stable_sort(out.draw.begin(), out.draw.end(), [](const ViewDraw& a, const ViewDraw& b) {
        if (a.nearest != b.nearest) return a.nearest;
        return a.layer < b.layer;
    });

    if (covered) {
        for (std::size_t i = 0; i < tiles_.size();) {
            const Sheet* sheet = findSheet(tiles_[i].page);
            if (sheet && (tiles_[i].percent != percent_ || tiles_[i].contentW != sheet->w ||
                          tiles_[i].contentH != sheet->h)) {
                dropAt(i);
            } else {
                ++i;
            }
        }
    } else {
        for (Tile& tile : tiles_) {
            const Sheet* sheet = findSheet(tile.page);
            if (!sheet ||
                (tile.percent == percent_ && tile.contentW == sheet->w && tile.contentH == sheet->h) ||
                sheet->w <= 0 || sheet->h <= 0) {
                continue;
            }
            const int left = sheet->x + mapCoord(tile.x, tile.contentW, sheet->w) - scrollX_;
            const int top = sheet->y + mapCoord(tile.y, tile.contentH, sheet->h) - scrollY_;
            const int right = sheet->x + mapCoord(tile.x + tile.w, tile.contentW, sheet->w) - scrollX_;
            const int bottom = sheet->y + mapCoord(tile.y + tile.h, tile.contentH, sheet->h) - scrollY_;
            const ViewRect scaled{left, top, std::max(1, right - left), std::max(1, bottom - top)};
            if (!intersects(scaled, screen)) continue;
            tile.stamp = ++clock_;
            ViewDraw draw;
            draw.id = tile.id;
            draw.x = scaled.x;
            draw.y = scaled.y;
            draw.w = scaled.w;
            draw.h = scaled.h;
            draw.layer = tile.layer;
            draw.nearest = true;
            nearest.push_back(draw);
        }
    }
    if (!nearest.empty()) out.draw.insert(out.draw.begin(), nearest.begin(), nearest.end());
    std::stable_sort(out.draw.begin(), out.draw.end(), [](const ViewDraw& a, const ViewDraw& b) {
        if (a.nearest != b.nearest) return a.nearest;
        return a.layer < b.layer;
    });

    std::vector<int> keep;
    keep.reserve(out.draw.size() + tiles_.size());
    for (const ViewDraw& draw : out.draw) keep.push_back(draw.id);
    for (const Tile& tile : tiles_) {
        const Sheet* sheet = findSheet(tile.page);
        if (!sheet || tile.percent != percent_ || tile.contentW != sheet->w || tile.contentH != sheet->h) continue;
        if (intersects(ViewRect{sheet->x + tile.x, sheet->y + tile.y, tile.w, tile.h}, wanted)) {
            keep.push_back(tile.id);
        }
    }
    while (static_cast<int>(tiles_.size()) > cacheLimit_) {
        int victim = -1;
        int oldest = std::numeric_limits<int>::max();
        for (std::size_t i = 0; i < tiles_.size(); ++i) {
            const int id = tiles_[i].id;
            if (std::find(keep.begin(), keep.end(), id) != keep.end()) continue;
            if (tiles_[i].stamp < oldest) {
                oldest = tiles_[i].stamp;
                victim = static_cast<int>(i);
            }
        }
        if (victim < 0) break;
        dropAt(static_cast<std::size_t>(victim));
    }

    out.drop = std::move(drops_);
    drops_.clear();
    if (inflight_.id != 0) {
        out.job = inflight_;
        return out;
    }
    const std::vector<Slot>* missing = nullptr;
    if (!visibleMissing.empty()) missing = &visibleMissing;
    else if (!marginMissing.empty()) missing = &marginMissing;
    if (!missing) return out;
    const Slot* best = &missing->front();
    for (const Slot& slot : *missing) {
        if (closer(slot, *best, viewport)) best = &slot;
    }
    if (best->pixelH < 32) return out;
    inflight_.id = nextId_++;
    inflight_.generation = generation_;
    inflight_.page = best->page;
    inflight_.percent = percent_;
    inflight_.pixelHeight = best->pixelH;
    inflight_.col = best->col;
    inflight_.row = best->row;
    inflight_.x = best->local.x;
    inflight_.y = best->local.y;
    inflight_.w = best->local.w;
    inflight_.h = best->local.h;
    inflight_.contentW = best->pageW;
    inflight_.contentH = best->pageH;
    inflight_.layer = best->layer;
    out.job = inflight_;
    return out;
}

}  // namespace chwp::render
