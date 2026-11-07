#include "render/painter.hpp"

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "chwp/common/unicode.hpp"
#include "chwp/reader.hpp"
#include "chwp/render/view.hpp"

// Painter: 그리기 바탕: 잘라내기, 글리프, 상자, 그림

namespace chwp::render::paint_detail {

bool Painter::showGlyphs() const {
    if (pass == PaintLayer::All) return true;
    if (!objectLayers.empty()) return pass == objectLayers.back();
    if (pass == PaintLayer::Text) return true;
    return pass == PaintLayer::Front && coverDepth > 0;
}

void Painter::stampHosts(FrameGlyph& glyph) const {
    glyph.hostFirst = 0;
    glyph.hostCount = static_cast<std::uint8_t>(objectHostCount);
    if (objectHostCount == 0 || !glyphSink) return;
    std::vector<TextHost>& hosts = glyphSink->hosts;
    if (!glyphSink->glyphs.empty()) {
        const FrameGlyph& last = glyphSink->glyphs.back();
        bool same = last.hostCount == objectHostCount;
        for (int i = 0; same && i < objectHostCount; ++i) {
            const TextHost& a = hosts[static_cast<std::size_t>(last.hostFirst + i)];
            same = a.story == objectHosts[i].story && a.paragraph == objectHosts[i].paragraph &&
                   a.offset == objectHosts[i].offset;
        }
        if (same) {
            glyph.hostFirst = last.hostFirst;
            return;
        }
    }
    glyph.hostFirst = static_cast<std::int32_t>(hosts.size());
    hosts.insert(hosts.end(), objectHosts, objectHosts + objectHostCount);
}

Painter::ClipBounds Painter::intersectClip(const ClipBounds& a, std::int32_t x, std::int32_t y, std::int32_t w,
                                           std::int32_t h) {
    if (w <= 0 || h <= 0) return a;
    ClipBounds out;
    out.x0 = std::max(a.x0, x);
    out.y0 = std::max(a.y0, y);
    out.x1 = std::min(a.x1, x + w);
    out.y1 = std::min(a.y1, y + h);
    return out;
}

Painter::ClipBounds Painter::childClipOf(const Node& parent, std::int32_t px, std::int32_t py, const Node& child,
                                         const ClipBounds& cur) {
    if (parent.kind == NodeKind::Paper) {
        return intersectClip(cur, px, py, parent.width, parent.height);
    }
    if (parent.kind == NodeKind::Cell) {
        return intersectClip(cur, px, py, parent.width, parent.height);
    }
    if (parent.kind == NodeKind::Object) {
        if (child.role == "caption") {
            return intersectClip(cur, px + child.x, py + child.y, child.width, child.height);
        }
        return intersectClip(cur, px, py, parent.width, parent.height);
    }
    return cur;
}

void Painter::seedObjectHosts(const Node& page, const std::vector<int>& path) {
    objectHostCount = 0;
    const Node* cur = &page;
    for (const int childIndex : path) {
        if (childIndex < 0 || childIndex >= static_cast<int>(cur->children.size())) return;
        const Node& child = cur->children[static_cast<std::size_t>(childIndex)];
        if (cur->kind == NodeKind::Line && child.kind == NodeKind::Object && !child.headingMark &&
            objectHostCount < kObjectHostDepth) {
            const std::vector<Mark> marks = caretMarks(*cur);
            for (std::size_t m = 0; m < marks.size(); ++m) {
                if (marks[m].slot == childIndex) {
                    objectHosts[objectHostCount++] =
                        TextHost{cur->story, cur->paragraph, cur->start + static_cast<int>(m)};
                    break;
                }
            }
        }
        cur = &child;
    }
}

bool Painter::seedAncestorClips(const Node& page, const std::vector<int>& path, std::int32_t padHu,
                                std::int32_t frameHuX, std::int32_t frameHuY, std::int32_t frameHuW,
                                std::int32_t frameHuH) {
    if (path.empty()) return false;
    ClipBounds clip;
    if (page.width > 0 && page.height > 0) {
        clip = intersectClip(clip, 0, 0, page.width, page.height);
    }
    bool hasBoxClip = false;
    const Node* cur = &page;
    std::int32_t ox = 0;
    std::int32_t oy = 0;
    // 세로쓰기 틀은 FrameWalk::visit 과 같이 바꾼다. 돌린 틀은 부모 자리가 원점이고, 세운 개체 상자 안은 종이 자리다.
    bool turned = false;
    std::int32_t turnX = 0;
    std::int32_t turnY = 0;
    std::int32_t turnSpan = 0;
    for (std::size_t step = 0; step < path.size(); ++step) {
        const int childIndex = path[step];
        if (childIndex < 0 || childIndex >= static_cast<int>(cur->children.size())) break;
        // 모은 자르기도 틀 좌표로 옮긴다. 종이 (X, Y) 는 틀의 (Y − 원점 Y, 원점 X + 폭 − X) 다.
        const auto clipToTurn = [&]() {
            if (clip.x1 == std::numeric_limits<std::int32_t>::max() || clip.y1 == std::numeric_limits<std::int32_t>::max()) {
                return;
            }
            ClipBounds turnedClip;
            turnedClip.x0 = clip.y0 - turnY;
            turnedClip.x1 = clip.y1 - turnY;
            turnedClip.y0 = turnX + turnSpan - clip.x1;
            turnedClip.y1 = turnX + turnSpan - clip.x0;
            clip = turnedClip;
        };
        const auto clipToPaper = [&]() {
            if (clip.x1 == std::numeric_limits<std::int32_t>::max() || clip.y1 == std::numeric_limits<std::int32_t>::max()) {
                return;
            }
            ClipBounds paperClip;
            paperClip.x0 = turnX + turnSpan - clip.y1;
            paperClip.x1 = turnX + turnSpan - clip.y0;
            paperClip.y0 = turnY + clip.x0;
            paperClip.y1 = turnY + clip.x1;
            clip = paperClip;
        };
        if (cur->turned != 0) {
            turned = true;
            turnX = ox;
            turnY = oy;
            turnSpan = cur->turnSpan > 0 ? cur->turnSpan : page.width;
            ox = 0;
            oy = 0;
            clipToTurn();
        }
        if (cur->upright && turned) {
            const std::int32_t paperX = turnX + turnSpan - (oy + cur->y) - cur->height;
            const std::int32_t paperY = turnY + ox + cur->x;
            clipToPaper();
            turned = false;
            ox = paperX;
            oy = paperY;
            cur = &cur->children[static_cast<std::size_t>(childIndex)];
            continue;
        }
        const std::int32_t x = ox + cur->x + (cur->kind == NodeKind::Paragraph ? cur->marginLeft : 0);
        const std::int32_t y = oy + cur->y;
        const Node& child = cur->children[static_cast<std::size_t>(childIndex)];
        if (cur->kind == NodeKind::Object) {
            if (child.role == "caption") {
                clip = intersectClip(clip, x + child.x, y + child.y, child.width, child.height);
                hasBoxClip = true;
            } else if (clipsOwnBox(*cur)) {
                const std::int32_t bleed = tableBorderBleed(*cur);
                clip = intersectClip(clip, x - bleed, y - bleed, cur->width + bleed * 2,
                                     cur->height + bleed * 2);
                hasBoxClip = true;
            }
        } else if (cur->kind == NodeKind::Cell) {
            clip = intersectClip(clip, x, y, cur->width, cur->height);
            hasBoxClip = true;
        }
        if (cur->kind == NodeKind::Line && child.kind == NodeKind::Object && child.treatAsChar) {
            std::vector<NestedCaret> nested;
            glyphEdges(*cur, x, y, &nested);
            std::int32_t childOx = x + child.marginLeft;
            std::int32_t childOy = y + child.marginTop;
            for (const NestedCaret& item : nested) {
                if (item.object == &child) {
                    childOx = item.x - child.x;
                    childOy = item.y - child.y;
                    break;
                }
            }
            ox = childOx;
            oy = childOy;
        } else {
            ox = x;
            oy = y;
        }
        cur = &child;
    }
    if (!hasBoxClip) return false;
    const std::int32_t cw = std::max<std::int32_t>(0, clip.x1 - clip.x0);
    const std::int32_t ch = std::max<std::int32_t>(0, clip.y1 - clip.y0);
    pushClip(padHu + clip.x0 - frameHuX, padHu + clip.y0 - frameHuY, cw, ch);
    return frameHuX < clip.x0 || frameHuY < clip.y0 || frameHuX + frameHuW > clip.x1 ||
           frameHuY + frameHuH > clip.y1;
}

bool Painter::behindOf(const Node& node) {
    const bool masterBack = node.fromMaster && !node.masterOnTop;
    const bool masterFront = node.fromMaster && node.masterOnTop;
    if (masterFront) return false;
    if (masterBack) return true;
    return node.ext().obj().textWrap == "BEHIND_TEXT";
}

PaintLayer Painter::objectPlace(const Node& node) const {
    if (pageBandDepth > 0) return PaintLayer::Behind;
    if (node.treatAsChar || !behindOf(node)) return PaintLayer::Front;
    return PaintLayer::Behind;
}

bool Painter::coversText(const Node& node) const {
    if (objectPlace(node) != PaintLayer::Front) return false;
    if (node.role == "equation") return true;
    if (node.role == "picture" && !node.ext().obj().binItemId.empty()) return true;
    return !node.ext().fillColor.empty() && node.ext().fillColor != "none";
}

std::int32_t Painter::paintedExtent(const Node& node) {
    std::int32_t bottom = std::max(node.height, node.lineData().textHeight);
    for (const Node& child : node.children) {
        bottom = std::max(bottom, child.y + child.marginTop + paintedExtent(child) + child.marginBottom);
    }
    return bottom;
}

bool Painter::missesView(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) const {
    if (!limitView || w <= 0 || h <= 0) return false;
    constexpr std::int32_t margin = 6000;
    if (x + w < -margin || y + h < -margin) return true;
    if (static_cast<float>(x) > viewRightHu + margin || static_cast<float>(y) > viewBottomHu + margin) return true;
    return false;
}

void Painter::applyCpuClip() {
    if (clips.empty()) {
        bitmap.clipL = 0;
        bitmap.clipT = 0;
        bitmap.clipR = bitmap.width;
        bitmap.clipB = bitmap.height;
        return;
    }
    bitmap.clipL = clips.back().l;
    bitmap.clipT = clips.back().t;
    bitmap.clipR = clips.back().r;
    bitmap.clipB = clips.back().b;
}

void Painter::pushClip(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) {
    Clip next;
    next.l = px(x);
    next.t = px(y);
    next.r = px(x + w);
    next.b = px(y + h);
    const int boundL = clips.empty() ? 0 : clips.back().l;
    const int boundT = clips.empty() ? 0 : clips.back().t;
    const int boundR = clips.empty() ? bitmap.width : clips.back().r;
    const int boundB = clips.empty() ? bitmap.height : clips.back().b;
    next.l = std::max(boundL, next.l);
    next.t = std::max(boundT, next.t);
    next.r = std::min(boundR, next.r);
    next.b = std::min(boundB, next.b);
    if (next.r < next.l) next.r = next.l;
    if (next.b < next.t) next.b = next.t;
    clips.push_back(next);
    applyCpuClip();
    if (skiaCanvas) {
        skiaCanvas->save();
        skiaCanvas->clipRect(SkRect::MakeLTRB(static_cast<float>(next.l), static_cast<float>(next.t),
                                              static_cast<float>(next.r), static_cast<float>(next.b)),
                             false);
        ++skiaSaves;
    }
}

void Painter::popClip() {
    if (!clips.empty()) clips.pop_back();
    applyCpuClip();
    if (skiaCanvas && skiaSaves > 0) {
        skiaCanvas->restore();
        --skiaSaves;
    }
}

SkCanvas* Painter::canvas() {
    if (skiaCanvas) return skiaCanvas;
    if (bitmap.width <= 0 || bitmap.height <= 0) return nullptr;
    ensureBitmap(bitmap);
    if (bitmap.px.empty()) return nullptr;
    const SkImageInfo info = SkImageInfo::Make(bitmap.width, bitmap.height, kRGBA_8888_SkColorType,
                                                kPremul_SkAlphaType);
    skiaSurface = SkSurfaces::WrapPixels(info, bitmap.px.data(), static_cast<std::size_t>(bitmap.width) * 4u);
    if (skiaSurface) {
        skiaCanvas = skiaSurface->getCanvas();
        for (const Clip& clip : clips) {
            skiaCanvas->save();
            skiaCanvas->clipRect(SkRect::MakeLTRB(static_cast<float>(clip.l), static_cast<float>(clip.t),
                                                  static_cast<float>(clip.r), static_cast<float>(clip.b)),
                                 false);
            ++skiaSaves;
        }
    }
    return skiaCanvas;
}

void Painter::paintGlyph(const std::vector<GlyphCommand>& commands, float emPx, int ratio, int fitX, int fitY, float penX,
                         float baseY, std::uint8_t r, std::uint8_t g, std::uint8_t b, float strokePx) {
    if (commands.empty() || emPx <= 0.f) return;
    const float unit = emPx / 1000.f;
    const float sy = unit * static_cast<float>(std::max(1, fitY)) / 1000.f;
    const float sx = unit * static_cast<float>(std::max(1, ratio)) / 100.f * static_cast<float>(std::max(1, fitX)) / 1000.f;
    const auto pxOf = [&](const GlyphPoint& point) {
        float x = penX + static_cast<float>(point.x) * sx;
        float y = baseY - static_cast<float>(point.y) * sy;
        turnGlyphPoint(&glyphTurn, x, y);
        return std::pair<float, float>{x, y};
    };
    if (SkCanvas* canvas = this->canvas()) {
        SkPathBuilder path;
        for (const GlyphCommand& command : commands) {
            const auto [x, y] = pxOf(command.to);
            if (command.verb == GlyphVerb::Move) path.moveTo(x, y);
            else if (command.verb == GlyphVerb::Line) path.lineTo(x, y);
            else if (command.verb == GlyphVerb::Quad) {
                const auto [cx, cy] = pxOf(command.c1);
                path.quadTo(cx, cy, x, y);
            } else if (command.verb == GlyphVerb::Cubic) {
                const auto [c1x, c1y] = pxOf(command.c1);
                const auto [c2x, c2y] = pxOf(command.c2);
                path.cubicTo(c1x, c1y, c2x, c2y, x, y);
            }
        }
        SkPaint paint;
        paint.setAntiAlias(true);
        paint.setStyle(strokePx > 0.f ? SkPaint::kStrokeAndFill_Style : SkPaint::kFill_Style);
        if (strokePx > 0.f) paint.setStrokeWidth(strokePx);
        paint.setColor(SkColorSetARGB(255, r, g, b));
        canvas->drawPath(path.detach(), paint);
    }
}

float Painter::glyphPx(std::int32_t hu) const {
    if (framePadHu > 0) return static_cast<float>(kFramePad) + static_cast<float>(hu - framePadHu) * scale;
    return static_cast<float>(hu) * scale;
}

void Painter::blit(int x, int y, int w, int h, const std::uint8_t* src, int channels, std::uint8_t r, std::uint8_t g,
                   std::uint8_t b, bool colored) {
    for (int row = 0; row < h; ++row) {
        for (int col = 0; col < w; ++col) {
            const std::uint8_t* s = src + (static_cast<std::size_t>(row) * w + col) * channels;
            if (colored) blend(bitmap, x + col, y + row, s[0], s[1], s[2], channels > 3 ? s[3] : 255);
            else blend(bitmap, x + col, y + row, r, g, b, s[0]);
        }
    }
}

void Painter::replaySpots(const std::vector<GlyphSpot>& spots) {
    for (const GlyphSpot& spot : spots) {
        if (!spot.mem || !spot.mem->face || spot.fontSize <= 0) continue;
        const float pixel = std::max(1.f, static_cast<float>(spot.fontSize) * scale);
        if (glyphSink) {
            const int ppem = std::max(1, static_cast<int>(std::lround(pixel)));
            FrameGlyph drawn{spot.mem, ppem, spot.ratio, spot.bold, spot.italic,
                             glyphPx(spot.huX), glyphPx(spot.baseline),
                             opaqueArgb(spot.r, spot.g, spot.b), spot.story, spot.paragraph,
                             spot.offset, pixel};
            stampHosts(drawn);
            glyphSink->glyphs.push_back(drawn);
            continue;
        }
        const int ppem = std::max(1, static_cast<int>(std::lround(pixel)));
        const auto& commands = hangulCommands(spot.mem, spot.bold, spot.italic, ppem);
        const int fitX = spot.mem->fitX > 0 ? spot.mem->fitX : 1000;
        const int fitY = spot.mem->fitY > 0 ? spot.mem->fitY : 1000;
        const float gx = framePadHu > 0 ? glyphPx(spot.huX) : static_cast<float>(px(spot.huX));
        const float gy = framePadHu > 0 ? glyphPx(spot.baseline) : static_cast<float>(px(spot.baseline));
        paintGlyph(commands, pixel, spot.ratio, fitX, fitY, gx, gy, spot.r, spot.g, spot.b);
    }
}

void Painter::glyph(char32_t cp, const std::string& font, int fontSize, int ratio, bool bold, bool italic,
                    const std::string& color, std::int32_t huX, std::int32_t baseline, int story,
                    int paragraph, int offset) {
    if (skipGlyphs && !placeSink) return;
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == 0x2007 || cp == 0x00A0) return;
    // 지정 글꼴에 글자가 없으면 함초롬 계열에서 그린다. 없으면 .notdef 빈 상자만 남는다.
    GlyphMem* mem = glyphMemory(font, cp);
    if (!mem || !mem->face || fontSize <= 0) return;
    const float pixel = std::max(1.f, static_cast<float>(fontSize) * scale);
    std::uint8_t r, g, b;
    parseHex(color, r, g, b);
    if (placeSink) {
        placeSink->push_back(GlyphSpot{mem, bold, italic, fontSize, ratio, huX, baseline, r, g, b, story, paragraph, offset});
    }
    if (skipGlyphs) return;
    if (glyphSink) {
        const int ppem = std::max(1, static_cast<int>(std::lround(pixel)));
        FrameGlyph drawn{mem, ppem, ratio, bold, italic, glyphPx(huX),
                         glyphPx(baseline), opaqueArgb(r, g, b), story, paragraph,
                         offset, pixel};
        stampHosts(drawn);
        if (glyphTurn.on) {
            drawn.upright = true;
            drawn.turnCx = glyphTurn.cx;
            drawn.turnCy = glyphTurn.cy;
            drawn.turnDx = glyphTurn.dx;
            drawn.turnDy = glyphTurn.dy;
        }
        glyphSink->glyphs.push_back(drawn);
        return;
    }
    const int ppem = std::max(1, static_cast<int>(std::lround(pixel)));
    const auto& commands = hangulCommands(mem, bold, italic, ppem);
    const int fitX = mem->fitX > 0 ? mem->fitX : 1000;
    const int fitY = mem->fitY > 0 ? mem->fitY : 1000;
    const float gx = framePadHu > 0 ? glyphPx(huX) : static_cast<float>(px(huX));
    const float gy = framePadHu > 0 ? glyphPx(baseline) : static_cast<float>(px(baseline));
    paintGlyph(commands, pixel, ratio, fitX, fitY, gx, gy, r, g, b);
}

void Painter::box(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, const std::string& fill,
                  const BorderEdge& edge, const std::string& role, PaintLayer place) {
    if (show(place) && !fill.empty() && fill != "none") {
        std::uint8_t r, g, b;
        parseHex(fill, r, g, b);
        fillRect(bitmap, px(x), px(y), std::max(1, px(x + w) - px(x)), std::max(1, px(y + h) - px(y)), r, g, b);
    }
    if (!show(frameStroke())) return;
    const bool equation = role == "equation";
    if (edge.type.empty() || edge.type == "NONE") {
        if (!equation) return;
        std::uint8_t r, g, b;
        parseHex(edge.color.empty() ? "#c45c26" : edge.color, r, g, b);
        const int thick = std::max(1, px(edge.width > 0 ? edge.width : 28));
        strokeRect(bitmap, px(x), px(y), std::max(1, px(x + w) - px(x)), std::max(1, px(y + h) - px(y)), thick, r,
                   g, b);
        return;
    }
    const int x0 = px(x);
    const int y0 = px(y);
    const int x1 = px(x + w);
    const int y1 = px(y + h);
    const int tw = std::max(0, x1 - x0);
    const int th = std::max(0, y1 - y0);
    float intervals[8];
    const int dashes = gdiDash(edge, intervals);
    if (dashes >= 2 && tw > 0 && th > 0) {
        if (SkCanvas* canvas = this->canvas()) {
            std::uint8_t r, g, b;
            parseHex(edge.color.empty() ? "#000000" : edge.color, r, g, b);
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(penPx(edge));
            const bool round = edge.end == 0 || edge.type == "CIRCLE";
            paint.setStrokeCap(round ? SkPaint::kRound_Cap : SkPaint::kButt_Cap);
            paint.setStrokeJoin(round ? SkPaint::kRound_Join : SkPaint::kMiter_Join);
            paint.setColor(SkColorSetARGB(255, r, g, b));
            paint.setPathEffect(SkDashPathEffect::Make(
                SkSpan<const SkScalar>(intervals, static_cast<std::size_t>(dashes)), 0));
            canvas->drawRect(SkRect::MakeXYWH(static_cast<float>(x0), static_cast<float>(y0),
                                               static_cast<float>(tw), static_cast<float>(th)),
                              paint);
            return;
        }
    }
    const int t = std::max(1, px(edge.width > 0 ? edge.width : 28));
    const ClipSuspend open(bitmap);
    strokeEdge(edge, x0 - t / 2, y0 - t / 2, tw + t, t);
    strokeEdge(edge, x0 - t / 2, y1 - t / 2, tw + t, t);
    strokeEdge(edge, x0 - t / 2, y0 - t / 2, t, th + t);
    strokeEdge(edge, x1 - t / 2, y0 - t / 2, t, th + t);
}

void Painter::imageFlip(SkCanvas* canvas, int dw, int dh, bool flipH, bool flipV) const {
    if (!flipH && !flipV) return;
    canvas->translate(flipH ? static_cast<float>(dw) : 0.f, flipV ? static_cast<float>(dh) : 0.f);
    canvas->scale(flipH ? -1.f : 1.f, flipV ? -1.f : 1.f);
}

bool Painter::nearestImage(const std::uint8_t* rgba, int sw, int sh, int dx, int dy, int dw, int dh, bool flipH, bool flipV,
                           const ImageDraw* draw, const CachedPicture* cached) {
    SkCanvas* canvas = this->canvas();
    if (!canvas || !rgba) return false;
    sk_sp<SkImage> sk = cached ? cached->skImage : nullptr;
    if (!sk) {
        const SkImageInfo info = SkImageInfo::Make(sw, sh, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
        const SkPixmap pixmap(info, rgba, static_cast<std::size_t>(sw) * 4u);
        sk = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
    }
    if (!sk) return false;
    SkPaint paint;
    paint.setAntiAlias(false);
    paintPictureTone(paint, draw ? draw->tone : nullptr);
    canvas->save();
    canvas->translate(static_cast<float>(dx), static_cast<float>(dy));
    imageFlip(canvas, dw, dh, flipH, flipV);
    canvas->drawImageRect(sk.get(), pictureSrc(sw, sh, draw), SkRect::MakeWH(static_cast<float>(dw), static_cast<float>(dh)),
                           SkSamplingOptions(SkFilterMode::kNearest), &paint, SkCanvas::kStrict_SrcRectConstraint);
    canvas->restore();
    return true;
}

bool Painter::paintAreaImage(const std::uint8_t* src, int sw, int sh, int dx, int dy, int dw, int dh, bool flipH,
                             bool flipV, const ImageDraw* draw) {
    PixelSpan vis;
    if (!visibleSpan(bitmap, dx, dy, dw, dh, vis)) return true;
    double sL = 0;
    double sT = 0;
    double sR = sw;
    double sB = sh;
    if (draw && draw->srcR > draw->srcL && draw->srcB > draw->srcT) {
        sL = draw->srcL;
        sT = draw->srcT;
        sR = draw->srcR;
        sB = draw->srcB;
    }
    const int visW = vis.r - vis.l;
    const int visH = vis.b - vis.t;
    if (visW <= 0 || visH <= 0 || sR <= sL || sB <= sT) return true;

    std::vector<ResampleSpan> xSpans;
    std::vector<ResampleTap> xTaps;
    int minSrcX = 0, maxSrcX = -1, maxTapX = 0;
    buildResampleAxis(vis.l, vis.r, dx, dw, flipH, sL, sR, sw, xSpans, xTaps, minSrcX, maxSrcX, maxTapX);

    std::vector<ResampleSpan> ySpans;
    std::vector<ResampleTap> yTaps;
    int minSrcY = 0, maxSrcY = -1, maxTapY = 0;
    buildResampleAxis(vis.t, vis.b, dy, dh, flipV, sT, sB, sh, ySpans, yTaps, minSrcY, maxSrcY, maxTapY);
    if (maxSrcX < minSrcX || maxSrcY < minSrcY) return true;

    const int cacheRows = std::clamp(maxTapY + 4, 8, std::max(8, maxSrcY - minSrcY + 1));
    std::vector<int> cachedRowTag(static_cast<std::size_t>(cacheRows), -1);
    std::vector<PremulRgba> cachedRowPixels(static_cast<std::size_t>(cacheRows) * static_cast<std::size_t>(visW));

    auto getHorizRow = [&](int sy) -> const PremulRgba* {
        const int slot = (sy - minSrcY) % cacheRows;
        PremulRgba* dstRow = cachedRowPixels.data() + static_cast<std::size_t>(slot) * static_cast<std::size_t>(visW);
        if (cachedRowTag[static_cast<std::size_t>(slot)] == sy) return dstRow;
        cachedRowTag[static_cast<std::size_t>(slot)] = sy;
        const std::uint8_t* srcRow = src + static_cast<std::size_t>(sy) * static_cast<std::size_t>(sw) * 4u;
        for (int ox = 0; ox < visW; ++ox) {
            const ResampleSpan span = xSpans[static_cast<std::size_t>(ox)];
            float ar = 0.f, ag = 0.f, ab = 0.f, aa = 0.f;
            float minR = 1e9f, minG = 1e9f, minB = 1e9f, minA = 1e9f;
            float maxR = -1e9f, maxG = -1e9f, maxB = -1e9f, maxA = -1e9f;
            for (int t = 0; t < span.count; ++t) {
                const ResampleTap& tap = xTaps[static_cast<std::size_t>(span.offset + t)];
                const std::uint8_t* px = srcRow + static_cast<std::size_t>(tap.index) * 4u;
                const float pa = static_cast<float>(px[3]) * (1.f / 255.f);
                const float pr = static_cast<float>(px[0]) * pa;
                const float pg = static_cast<float>(px[1]) * pa;
                const float pb = static_cast<float>(px[2]) * pa;
                ar += pr * tap.weight;
                ag += pg * tap.weight;
                ab += pb * tap.weight;
                aa += pa * tap.weight;
                if (tap.core) {
                    minR = std::min(minR, pr);
                    maxR = std::max(maxR, pr);
                    minG = std::min(minG, pg);
                    maxG = std::max(maxG, pg);
                    minB = std::min(minB, pb);
                    maxB = std::max(maxB, pb);
                    minA = std::min(minA, pa);
                    maxA = std::max(maxA, pa);
                }
            }
            dstRow[static_cast<std::size_t>(ox)] = PremulRgba{
                std::clamp(ar, minR, maxR),
                std::clamp(ag, minG, maxG),
                std::clamp(ab, minB, maxB),
                std::clamp(aa, minA, maxA),
            };
        }
        return dstRow;
    };

    std::vector<const PremulRgba*> tapRows(static_cast<std::size_t>(std::max(1, maxTapY)));
    for (int y = vis.t; y < vis.b; ++y) {
        const ResampleSpan ySpan = ySpans[static_cast<std::size_t>(y - vis.t)];
        for (int t = 0; t < ySpan.count; ++t) {
            const ResampleTap& tap = yTaps[static_cast<std::size_t>(ySpan.offset + t)];
            tapRows[static_cast<std::size_t>(t)] = getHorizRow(tap.index);
        }
        for (int ox = 0; ox < visW; ++ox) {
            float ar = 0.f, ag = 0.f, ab = 0.f, aa = 0.f;
            float minR = 1e9f, minG = 1e9f, minB = 1e9f, minA = 1e9f;
            float maxR = -1e9f, maxG = -1e9f, maxB = -1e9f, maxA = -1e9f;
            for (int t = 0; t < ySpan.count; ++t) {
                const ResampleTap& tap = yTaps[static_cast<std::size_t>(ySpan.offset + t)];
                const PremulRgba& p = tapRows[static_cast<std::size_t>(t)][static_cast<std::size_t>(ox)];
                ar += p.r * tap.weight;
                ag += p.g * tap.weight;
                ab += p.b * tap.weight;
                aa += p.a * tap.weight;
                if (tap.core) {
                    minR = std::min(minR, p.r);
                    maxR = std::max(maxR, p.r);
                    minG = std::min(minG, p.g);
                    maxG = std::max(maxG, p.g);
                    minB = std::min(minB, p.b);
                    maxB = std::max(maxB, p.b);
                    minA = std::min(minA, p.a);
                    maxA = std::max(maxA, p.a);
                }
            }
            ar = std::clamp(ar, minR, maxR);
            ag = std::clamp(ag, minG, maxG);
            ab = std::clamp(ab, minB, maxB);
            aa = std::clamp(aa, std::max(0.f, minA), std::min(1.f, maxA));
            std::uint8_t r = 0, g = 0, b = 0, a = 0;
            if (aa > 1e-5f) {
                const float invA = 1.f / aa;
                r = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(ar * invA)), 0, 255));
                g = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(ag * invA)), 0, 255));
                b = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(ab * invA)), 0, 255));
                a = static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(aa * 255.f)), 0, 255));
            }
            if (draw && draw->tone && draw->tone->on()) applyPictureTone(r, g, b, a, *draw->tone);
            blendPicture(bitmap, vis.l + ox, y, r, g, b, a);
        }
    }
    return true;
}

bool Painter::mitchellCpu(const std::uint8_t* rgba, int sw, int sh, int dx, int dy, int dw, int dh, bool flipH, bool flipV,
                          const ImageDraw* draw, const CachedPicture* cached) {
    SkCanvas* canvas = this->canvas();
    if (!canvas || !rgba) return false;
    sk_sp<SkImage> sk = cached ? cached->skImage : nullptr;
    if (!sk) {
        const SkImageInfo info = SkImageInfo::Make(sw, sh, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType);
        const SkPixmap pixmap(info, rgba, static_cast<std::size_t>(sw) * 4u);
        sk = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
    }
    if (!sk) return false;
    SkPaint paint;
    paint.setAntiAlias(false);
    paintPictureTone(paint, draw ? draw->tone : nullptr);
    canvas->save();
    canvas->translate(static_cast<float>(dx), static_cast<float>(dy));
    imageFlip(canvas, dw, dh, flipH, flipV);
    canvas->drawImageRect(sk.get(), pictureSrc(sw, sh, draw), SkRect::MakeWH(static_cast<float>(dw), static_cast<float>(dh)),
                           SkSamplingOptions(SkCubicResampler::CatmullRom()), &paint, SkCanvas::kStrict_SrcRectConstraint);
    canvas->restore();
    return true;
}

bool Painter::mitchellImage(const std::uint8_t* rgba, int sw, int sh, int dx, int dy, int dw, int dh, bool flipH, bool flipV,
                            const CachedPicture* key, const ImageDraw* draw) {
    PixelSpan vis;
    if (!visibleSpan(bitmap, dx, dy, dw, dh, vis)) return true;
    return mitchellCpu(rgba, sw, sh, dx, dy, dw, dh, flipH, flipV, draw, key);
}

bool Painter::skiaImage(const CachedPicture& image, int dx, int dy, int dw, int dh, bool flipH, bool flipV, bool filter,
                        const ImageDraw* draw) {
    if (image.rgba.empty() || image.width <= 0 || image.height <= 0 || dw <= 0 || dh <= 0) return false;
    double spanW = image.width;
    double spanH = image.height;
    if (draw && draw->srcR > draw->srcL && draw->srcB > draw->srcT) {
        spanW = draw->srcR - draw->srcL;
        spanH = draw->srcB - draw->srcT;
    }
    const bool shrink = dw < spanW || dh < spanH;
    const bool grow = dw > spanW || dh > spanH;
    if (filter && shrink) {
        return paintAreaImage(image.rgba.data(), image.width, image.height, dx, dy, dw, dh, flipH, flipV, draw);
    }
    if (filter && grow) {
        if (mitchellImage(image.rgba.data(), image.width, image.height, dx, dy, dw, dh, flipH, flipV, &image, draw)) {
            return true;
        }
        return paintAreaImage(image.rgba.data(), image.width, image.height, dx, dy, dw, dh, flipH, flipV, draw);
    }
    return nearestImage(image.rgba.data(), image.width, image.height, dx, dy, dw, dh, flipH, flipV, draw, &image);
}

bool Painter::skiaTile(const CachedPicture& image, int x, int y, int w, int h) {
    SkCanvas* canvas = this->canvas();
    if (!canvas || image.rgba.empty() || w <= 0 || h <= 0) return false;
    sk_sp<SkImage> sk = image.skImage;
    if (!sk) {
        const SkImageInfo info = SkImageInfo::Make(image.width, image.height, kRGBA_8888_SkColorType,
                                                    kUnpremul_SkAlphaType);
        const SkPixmap pixmap(info, image.rgba.data(), static_cast<std::size_t>(image.width) * 4u);
        sk = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
    }
    if (!sk) return false;
    const SkMatrix origin = SkMatrix::Translate(static_cast<float>(x), static_cast<float>(y));
    SkPaint paint;
    paint.setAntiAlias(false);
    paint.setShader(SkShaders::Image(sk, SkTileMode::kRepeat, SkTileMode::kRepeat,
                                     SkSamplingOptions(SkFilterMode::kNearest), &origin));
    canvas->save();
    canvas->clipRect(SkRect::MakeXYWH(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                                       static_cast<float>(h)),
                     false);
    canvas->drawRect(SkRect::MakeXYWH(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                                       static_cast<float>(h)),
                     paint);
    canvas->restore();
    return true;
}

void Painter::picture(const Node& node, std::int32_t x, std::int32_t y) {
    const std::shared_ptr<const CachedPicture> image = pictureFor(*doc, node.ext().obj().binItemId);
    const int dx = px(x);
    const int dy = px(y);
    const int dw = std::max(1, px(x + node.width) - dx);
    const int dh = std::max(1, px(y + node.height) - dy);
    if (!image) {
        pictureMissed = true;
        fillRect(bitmap, dx, dy, dw, dh, 0xE7, 0xED, 0xF2);
        return;
    }
    int x0 = dx;
    int y0 = dy;
    int x1 = dx + dw;
    int y1 = dy + dh;
    const model::PictureView& pic = node.ext().obj().picture;
    if (pic.padLeft || pic.padRight || pic.padTop || pic.padBottom) {
        const int ix0 = px(x + pic.padLeft);
        const int iy0 = px(y + pic.padTop);
        const int ix1 = px(x + node.width - pic.padRight);
        const int iy1 = px(y + node.height - pic.padBottom);
        if (ix1 <= ix0 || iy1 <= iy0) return;
        x0 = ix0;
        y0 = iy0;
        x1 = ix1;
        y1 = iy1;
    }
    PictureSource source;
    if (!cropSource(pictureCrop(pic), image->width, image->height, source)) return;
    if (source.fracL != 0 || source.fracT != 0 || source.fracR != 1 || source.fracB != 1) {
        const int boxW = x1 - x0;
        const int boxH = y1 - y0;
        const auto along = [](double frac, int size) {
            return std::clamp(static_cast<int>(std::lround(frac * size)), 0, size);
        };
        const int left = along(source.fracL, boxW);
        const int top = along(source.fracT, boxH);
        const int right = along(source.fracR, boxW);
        const int bottom = along(source.fracB, boxH);
        if (right <= left || bottom <= top) return;
        x0 += left;
        y0 += top;
        x1 = x0 + (right - left);
        y1 = y0 + (bottom - top);
    }
    ImageDraw draw;
    draw.srcL = source.srcL;
    draw.srcT = source.srcT;
    draw.srcR = source.srcR;
    draw.srcB = source.srcB;
    const PictureTone tone = pictureTone(pic);
    if (tone.on()) draw.tone = &tone;
    const int drawW = x1 - x0;
    const int drawH = y1 - y0;
    if (drawW <= 0 || drawH <= 0) return;
    if (pictureSink && !image->levelSizes.empty() && image->width <= kMaxPictureTexture &&
        image->height <= kMaxPictureTexture) {
        // 확대해도 다시 굽지 않으므로 자리는 픽셀로 반올림하지 않는다.
        const auto at = [&](std::int32_t hu) { return static_cast<float>(hu) * scale; };
        float boxL = at(x);
        float boxT = at(y);
        float boxR = at(x + node.width);
        float boxB = at(y + node.height);
        if (pic.padLeft || pic.padRight || pic.padTop || pic.padBottom) {
            boxL = at(x + pic.padLeft);
            boxT = at(y + pic.padTop);
            boxR = at(x + node.width - pic.padRight);
            boxB = at(y + node.height - pic.padBottom);
        }
        const float boxW = boxR - boxL;
        const float boxH = boxB - boxT;
        FramePicture item;
        item.id = node.ext().obj().binItemId;
        item.picture = image;
        item.dst = PictureRect{boxL + static_cast<float>(source.fracL) * boxW,
                               boxT + static_cast<float>(source.fracT) * boxH,
                               boxL + static_cast<float>(source.fracR) * boxW,
                               boxT + static_cast<float>(source.fracB) * boxH};
        item.clip = PictureRect{static_cast<float>(bitmap.clipL), static_cast<float>(bitmap.clipT),
                                static_cast<float>(bitmap.clipR), static_cast<float>(bitmap.clipB)};
        item.src = PictureRect{static_cast<float>(source.srcL), static_cast<float>(source.srcT),
                               static_cast<float>(source.srcR), static_cast<float>(source.srcB)};
        item.flipH = node.flipHorizontal;
        item.flipV = node.flipVertical;
        item.tone = tone;
        pictureSink->push_back(std::move(item));
        return;
    }
    if (skiaImage(*image, x0, y0, drawW, drawH, node.flipHorizontal, node.flipVertical, true, &draw)) return;
    const double spanW = draw.srcR - draw.srcL;
    const double spanH = draw.srcB - draw.srcT;
    for (int row = 0; row < drawH; ++row) {
        int iy = row;
        if (node.flipVertical) iy = drawH - 1 - iy;
        const double sy0 = draw.srcT + static_cast<double>(iy) * spanH / drawH;
        const double sy1 = draw.srcT + static_cast<double>(iy + 1) * spanH / drawH;
        for (int col = 0; col < drawW; ++col) {
            int ix = col;
            if (node.flipHorizontal) ix = drawW - 1 - ix;
            const double sx0 = draw.srcL + static_cast<double>(ix) * spanW / drawW;
            const double sx1 = draw.srcL + static_cast<double>(ix + 1) * spanW / drawW;
            std::uint8_t r, g, b, a;
            sampleArea(image->rgba.data(), image->width, image->height, sx0, sx1, sy0, sy1, r, g, b, a);
            if (draw.tone) applyPictureTone(r, g, b, a, *draw.tone);
            blendPicture(bitmap, x0 + col, y0 + row, r, g, b, a);
        }
    }
}

}  // namespace chwp::render::paint_detail
