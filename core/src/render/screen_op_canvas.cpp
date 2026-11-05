// Skia 캔버스를 이어받는 클래스는 이 파일에만 둔다. Skia 가 RTTI 없이 빌드되어 이 파일만 -fno-rtti 로 짓는다.
#include "render/screen_op_canvas.hpp"

#if CHWP_HAS_GPU
#include <algorithm>
#include <utility>

#include "include/core/SkImage.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/utils/SkNoDrawCanvas.h"
#include "chwp/render/screen_ops.hpp"

namespace chwp::render {
namespace {

// Skia 그리기를 2D 그림 목록으로 옮겨 적는다. 무엇을 어떤 차례로 그릴지는 presentScreen 이 그대로 정한다.
// 2D 화면에서 쓰는 그리기(사각·둥근 사각·선·경로·그림·자르기)만 옮긴다. 나머지는 2D 화면이 쓰지 않는다.
class ScreenOpCanvas final : public SkNoDrawCanvas {
 public:
    ScreenOpCanvas(int width, int height, ScreenFrame2D& frame, std::function<int(const SkImage*)> imageKey)
        : SkNoDrawCanvas(width, height), frame_(frame), imageKey_(std::move(imageKey)) {}

 protected:
    void willSave() override { push(ScreenOpKind::Save); }
    void willRestore() override { push(ScreenOpKind::Restore); }
    SaveLayerStrategy getSaveLayerStrategy(const SaveLayerRec& rec) override {
        push(ScreenOpKind::Save);
        (void)SkNoDrawCanvas::getSaveLayerStrategy(rec);
        return kNoLayer_SaveLayerStrategy;
    }
    void onClipRect(const SkRect& rect, SkClipOp op, ClipEdgeStyle style) override {
        if (op == SkClipOp::kIntersect) {
            ScreenOp& clip = push(ScreenOpKind::Clip);
            place(clip, this->getTotalMatrix().mapRect(rect));
        }
        SkNoDrawCanvas::onClipRect(rect, op, style);
    }
    void onDrawPaint(const SkPaint& paint) override { push(ScreenOpKind::Fill).color = paint.getColor(); }
    void onDrawRect(const SkRect& rect, const SkPaint& paint) override {
        const SkRect at = this->getTotalMatrix().mapRect(rect);
        if (paint.getStyle() == SkPaint::kFill_Style) {
            ScreenOp& op = push(ScreenOpKind::Rect);
            op.color = paint.getColor();
            place(op, at);
            return;
        }
        const SkPoint corners[5] = {{at.fLeft, at.fTop}, {at.fRight, at.fTop}, {at.fRight, at.fBottom},
                                    {at.fLeft, at.fBottom}, {at.fLeft, at.fTop}};
        for (int i = 0; i < 4; ++i) line(corners[i], corners[i + 1], paint);
    }
    void onDrawRRect(const SkRRect& rrect, const SkPaint& paint) override {
        ScreenOp& op = push(ScreenOpKind::RoundRect);
        op.color = paint.getColor();
        place(op, this->getTotalMatrix().mapRect(rrect.rect()));
        op.size = rrect.getSimpleRadii().fX * this->getTotalMatrix().getScaleX();
    }
    void onDrawPoints(PointMode mode, size_t count, const SkPoint pts[], const SkPaint& paint) override {
        if (!pts || count < 2) return;
        const SkMatrix& m = this->getTotalMatrix();
        const std::size_t step = mode == kLines_PointMode ? 2 : 1;
        for (std::size_t i = 0; i + 1 < count; i += step) line(m.mapPoint(pts[i]), m.mapPoint(pts[i + 1]), paint);
    }
    void onDrawPath(const SkPath& path, const SkPaint& paint) override {
        ScreenOp& op = push(ScreenOpKind::Path);
        op.color = paint.getColor();
        op.pathStart = static_cast<std::int32_t>(frame_.path.size());
        const SkMatrix& m = this->getTotalMatrix();
        const auto point = [&](const SkPoint& p) {
            const SkPoint at = m.mapPoint(p);
            frame_.path.push_back(at.fX);
            frame_.path.push_back(at.fY);
        };
        const auto verb = [&](ScreenPathVerb v) { frame_.path.push_back(static_cast<float>(v)); };
        SkPath::Iter iter(path, false);
        SkPoint pts[4];
        for (SkPath::Verb v = iter.next(pts); v != SkPath::kDone_Verb; v = iter.next(pts)) {
            switch (v) {
                case SkPath::kMove_Verb:
                    verb(ScreenPathVerb::Move);
                    point(pts[0]);
                    break;
                case SkPath::kLine_Verb:
                    verb(ScreenPathVerb::Line);
                    point(pts[1]);
                    break;
                case SkPath::kQuad_Verb:
                    verb(ScreenPathVerb::Quad);
                    point(pts[1]);
                    point(pts[2]);
                    break;
                case SkPath::kCubic_Verb:
                    verb(ScreenPathVerb::Cubic);
                    point(pts[1]);
                    point(pts[2]);
                    point(pts[3]);
                    break;
                case SkPath::kClose_Verb:
                    verb(ScreenPathVerb::Close);
                    break;
                default:
                    break;
            }
        }
        op.pathCount = static_cast<std::int32_t>(frame_.path.size()) - op.pathStart;
    }
    void onDrawImage2(const SkImage* image, SkScalar x, SkScalar y, const SkSamplingOptions& sampling,
                      const SkPaint* paint) override {
        if (!image) return;
        this->onDrawImageRect2(image, SkRect::MakeIWH(image->width(), image->height()),
                               SkRect::MakeXYWH(x, y, static_cast<float>(image->width()),
                                                static_cast<float>(image->height())),
                               sampling, paint, kFast_SrcRectConstraint);
    }
    void onDrawImageRect2(const SkImage* image, const SkRect& src, const SkRect& dst, const SkSamplingOptions& sampling,
                          const SkPaint* paint, SrcRectConstraint) override {
        const int key = image && imageKey_ ? imageKey_(image) : 0;
        if (key <= 0) return;
        ScreenOp& op = push(ScreenOpKind::Image);
        op.image = key;
        place(op, this->getTotalMatrix().mapRect(dst));
        op.sx = src.fLeft;
        op.sy = src.fTop;
        op.sw = src.width();
        op.sh = src.height();
        if (paint && paint->getColorFilter()) op.flags |= kScreenOpInvert;
        if (sampling.useCubic || sampling.filter == SkFilterMode::kLinear) op.flags |= kScreenOpSmooth;
    }

 private:
    ScreenOp& push(ScreenOpKind kind) {
        frame_.ops.emplace_back();
        frame_.ops.back().kind = static_cast<std::int32_t>(kind);
        return frame_.ops.back();
    }
    static void place(ScreenOp& op, const SkRect& at) {
        op.x = at.fLeft;
        op.y = at.fTop;
        op.w = at.width();
        op.h = at.height();
    }
    void line(SkPoint a, SkPoint b, const SkPaint& paint) {
        ScreenOp& op = push(ScreenOpKind::Line);
        op.color = paint.getColor();
        op.x = a.fX;
        op.y = a.fY;
        op.w = b.fX;
        op.h = b.fY;
        op.size = std::max(1.f, paint.getStrokeWidth());
    }
    ScreenFrame2D& frame_;
    std::function<int(const SkImage*)> imageKey_;
};


}  // namespace

std::unique_ptr<SkCanvas> makeScreenOpCanvas(int width, int height, ScreenFrame2D& frame,
                                             std::function<int(const SkImage*)> imageKey) {
    return std::make_unique<ScreenOpCanvas>(width, height, frame, std::move(imageKey));
}

}  // namespace chwp::render
#endif
