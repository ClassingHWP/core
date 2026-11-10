#pragma once

#include "render/gpu_config.hpp"

// 쪽 노드 트리를 비트맵이나 Skia 캔버스에 그리고, 캐럿·선택 칸을 찾는 Painter.
// 메서드 정의는 painter_draw.cpp, painter_shape.cpp, painter_table.cpp, painter_text.cpp, painter_caret.cpp 에 있다.
// 이 파일의 도우미 함수와 전역 값의 정의는 paint.cpp 에 있다.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "chwp/render/decorate.hpp"
#include "chwp/render/glyph_path.hpp"
#include "chwp/render/guides.hpp"
#include "chwp/model/proofreading.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"
#include "render/border_mesh.hpp"
#include "render/picture.hpp"
#include "stb_truetype.h"

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/effects/SkDashPathEffect.h"
#include "include/core/SkData.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRSXform.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkShader.h"
#include "include/core/SkString.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTileMode.h"
#include "include/core/SkVertices.h"
#include "include/effects/SkColorMatrix.h"
#include "include/effects/SkRuntimeEffect.h"

#if CHWP_HAS_GPU
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#else
#include "dawn/dawn_proc.h"
#include "dawn/native/DawnNative.h"
#endif

#include <webgpu/webgpu_cpp.h>

#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "src/gpu/graphite/RecorderPriv.h"
#include "src/gpu/graphite/RendererProvider.h"
#endif

namespace chwp::render::paint_detail {

struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> px;
    int clipL = 0;
    int clipT = 0;
    int clipR = 0;
    int clipB = 0;
};

struct Face {
    const std::string* name = nullptr;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    stbtt_fontinfo info{};
    bool ok = false;
    bool fallbackOutline = false;
    // 접힌 글꼴. bytes 와 info 를 놓았다. 쓸 때 reopenFaceLocked 가 다시 연다. 객체는 남아 GlyphMem 이 가리킨다.
    bool folded = false;
    int offset = 0;
};

// 한 번 찾은 글꼴 글자. 자리는 이 노드를 가리키고, 글자 코드로 다시 찾지 않는다.
struct GlyphMem {
    Face* face = nullptr;
    int index = 0;
    char32_t cp = 0;
    // 1000 이 1배. 폴백 윤곽만 원래 글꼴의 진행 폭·어센더에 맞춘다.
    int fitX = 1000;
    int fitY = 1000;
    // 글꼴 파일의 진행 폭(1000 em 기준). 찾을 때 한 번 읽어 두어, 글꼴을 접어도 쓴다.
    float advance1000 = 0.f;
};

void ensureBitmap(Bitmap& bitmap);

void blend(Bitmap& bitmap, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);

// 그림 칸은 투명에서 시작한다. 알파를 255로 덮으면 옅은 가장자리가 검게 뭉친다.
void blendPicture(Bitmap& bitmap, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);

struct PixelSpan {
    int l = 0;
    int t = 0;
    int r = 0;
    int b = 0;
};

bool visibleSpan(const Bitmap& bitmap, int dx, int dy, int dw, int dh, PixelSpan& out);

struct ResampleTap {
    int index = 0;
    float weight = 0.f;
    bool core = false;
};

struct ResampleSpan {
    int offset = 0;
    int count = 0;
};

struct PremulRgba {
    float r = 0.f;
    float g = 0.f;
    float b = 0.f;
    float a = 0.f;
};

void buildResampleAxis(int visStart, int visEnd, int dstOrigin, int dstLen, bool flip, double srcStart, double srcEnd,
                       int srcLimit, std::vector<ResampleSpan>& spans, std::vector<ResampleTap>& taps, int& minNeeded,
                       int& maxNeeded, int& maxTapCount);

// 목적지 픽셀이 덮는 원본 사각형을 평균한다. 투명도는 미리 곱한 색으로 모은 뒤 다시 나눈다.
void sampleArea(const std::uint8_t* src, int srcW, int srcH, double x0, double x1, double y0, double y1,
                std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a);

void fillRect(Bitmap& bitmap, int x, int y, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b);

// 고정폭 빈칸. 10pt면 248이다.
int figureSlot(int size);

// HWPX는 leader를 1, 2, 3처럼 숫자로 둔다. 2는 파선, 3은 점선이다.
std::string canonicalLeader(const std::string& raw);

// 탭 채움. x,w는 양쪽 고정폭 빈칸을 뺀 구간이고, y는 글자 상자의 세로 중앙이다.
// 칸의 1/3이 미리보기에서 1픽셀이다. 파선은 5칸 그리고 2칸 비운다. 긴 파선과 쇄선의 긴 획은 10칸이다.
void fillLeader(Bitmap& bitmap, const std::string& raw, int x, int baseline, int w, int slot, std::uint8_t r,
                std::uint8_t g, std::uint8_t b);

// 선 굵기의 절반은 상자 안, 절반은 밖이다. 그리는 동안은 개체 자르기를 끈다.
struct ClipSuspend {
    Bitmap& bitmap;
    int l;
    int t;
    int r;
    int b;
    explicit ClipSuspend(Bitmap& bitmap)
        : bitmap(bitmap), l(bitmap.clipL), t(bitmap.clipT), r(bitmap.clipR), b(bitmap.clipB) {
        bitmap.clipL = 0;
        bitmap.clipT = 0;
        bitmap.clipR = bitmap.width;
        bitmap.clipB = bitmap.height;
    }
    ~ClipSuspend() {
        bitmap.clipL = l;
        bitmap.clipT = t;
        bitmap.clipR = r;
        bitmap.clipB = b;
    }
};

void strokeRect(Bitmap& bitmap, int x, int y, int w, int h, int thick, std::uint8_t r, std::uint8_t g,
                std::uint8_t b);

void parseHex(const std::string& text, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b);

const std::vector<GlyphCommand>& hangulCommands(const GlyphMem* mem, bool bold, bool italic, int ppem);

// 세로쓰기에서 세우는 글자. 돌린 틀에 그린 뒤 쪽째 시계 방향으로 돌리므로, 글자는 칸 가운데(cx, cy)를 축으로
// 반시계 방향 90° 돌려 둔다. dx·dy 는 돌린 뒤 미는 픽셀이다(。、 를 칸 오른쪽 위로).
struct GlyphTurn {
    bool on = false;
    float cx = 0.f;
    float cy = 0.f;
    float dx = 0.f;
    float dy = 0.f;
};

inline void turnGlyphPoint(const GlyphTurn* turn, float& x, float& y) {
    if (!turn || !turn->on) return;
    const float rx = turn->cx + (y - turn->cy);
    const float ry = turn->cy - (x - turn->cx);
    x = rx + turn->dx;
    y = ry + turn->dy;
}

// 칸 선은 경계 한가운데라 바깥 절반이 표 상자 밖에 있다. 픽셀로 반올림해도 잘리지 않게 선 굵기만큼 여유를 둔다.
std::int32_t tableBorderBleed(const Node& node);

// 글자처럼 넣은 개체의 문단 위치. 그 개체를 블록에 넣으면 안쪽 글자도 같은 선택이다.
struct TextHost {
    int story = -1;
    int paragraph = -1;
    int offset = -1;
};

constexpr int kObjectHostDepth = 8;

// 불투명 ARGB 색. 화면 쪽 자료는 Skia 없이 이 값으로 색을 들고, 그리는 쪽이 그대로 SkColor 로 쓴다.
inline std::uint32_t opaqueArgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
}

// 화면이 칸 비트맵 대신 그리는 글자 하나. 글꼴 글자는 GlyphMem 을 가리키고, 색과 위치만 칸에 둔다.
struct FrameGlyph {
    GlyphMem* mem = nullptr;
    int ppem = 0;
    int ratio = 100;
    bool bold = false;
    bool italic = false;
    float penX = 0.f;
    float penY = 0.f;
    std::uint32_t color = 0xFF000000u;
    int story = -1;
    int paragraph = -1;
    int offset = -1;
    float exactPpem = 0.f;
    // 개체 안 글자의 바깥 개체 자리. 칸의 FrameGlyphs::hosts[hostFirst, hostFirst + hostCount) 이다.
    std::int32_t hostFirst = 0;
    std::uint8_t hostCount = 0;
    // 1 이면 펜을 축으로 마스크를 시계 방향 90° 돌려 찍는다(세로쓰기에서 눕힌 글자).
    std::uint8_t turn = 0;
    // 세로쓰기에서 세운 글자. 돌린 틀의 칸 가운데(turnCx, turnCy)와 미는 픽셀. 칸을 종이로 돌릴 때 펜을 다시 잡는다.
    bool upright = false;
    float turnCx = 0.f;
    float turnCy = 0.f;
    float turnDx = 0.f;
    float turnDy = 0.f;
    mutable int cachedWant = 0;
    mutable int cachedDrawnPpem = 0;
    mutable std::uint32_t cachedGen = 0;
    mutable std::uint32_t cachedBuiltSeq = 0;
    mutable const void* cachedMask = nullptr;
};

// 줄 자리. 글꼴 글자는 GlyphMem 을 가리킨다. 윤곽은 그 포인터로 읽는다.
struct GlyphSpot {
    GlyphMem* mem = nullptr;
    bool bold = false;
    bool italic = false;
    int fontSize = 0;
    int ratio = 100;
    std::int32_t huX = 0;
    std::int32_t baseline = 0;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    int story = -1;
    int paragraph = -1;
    int offset = -1;
};

// 한 칸의 화면 글자. 같은 칸 글자는 대개 바깥 개체 자리가 같아서, 그 자리는 글자마다 두지 않고 여기 한 번만 둔다.
struct FrameGlyphs {
    std::vector<FrameGlyph> glyphs;
    std::vector<TextHost> hosts;
};

GlyphMem* glyphMemory(const std::string& font, char32_t cp);

extern std::shared_mutex gSpanMu;

// 글자마다 폭(다음 글자까지, 자간 포함)과 잉크 폭을 잰다. 잰 값은 글자 모양·글자별로 남겨 다시 쓴다.
void measureText(const model::Document* doc, std::string_view text, std::uint32_t charShapeId, int size, int ratio,
                 std::vector<std::int32_t>& advances, std::vector<std::int32_t>& inks);

struct LineBlockFullRun {
    std::int32_t relLeft = 0;
    std::int32_t relRight = 0;
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    bool hasOwnBg = false;
};

struct LineBlockBand {
    std::int32_t relX = 0;
    std::int32_t relY = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    int index = 0;
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    bool hasOwnBg = false;
};

struct LineBlockNested {
    std::int32_t relX = 0;
    std::int32_t relY = 0;
    const Node* object = nullptr;
};

struct LineBlockCacheEntry {
    int story = 0;
    int paragraph = -1;
    int start = -1;
    int end = -1;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t textHeight = 0;
    std::int32_t fontSize = 0;
    std::int32_t indent = 0;
    std::size_t childCount = 0;
    std::int32_t firstChildW = -1;
    std::size_t firstChildTextLen = 0;
    std::uint64_t childrenSig = 0;
    int endExclusive = 0;
    std::int32_t boxHeight = 1;
    bool bandsBuilt = false;
    bool nestedBuilt = false;
    std::vector<std::int32_t> relEdges;
    std::vector<std::uint8_t> markHasOwnBg;
    std::vector<std::uint8_t> markR;
    std::vector<std::uint8_t> markG;
    std::vector<std::uint8_t> markB;
    std::vector<LineBlockFullRun> fullRuns;
    std::vector<LineBlockBand> bands;
    std::vector<LineBlockNested> relNested;
};

std::unordered_map<const Node*, LineBlockCacheEntry>& lineBlockCache();

struct TabGap {
    std::int32_t width = 0;
    std::string leader;
};

// 탭 위치는 본문 왼쪽 끝 기준이다. pen 은 줄 원점에서의 글자 폭이고, 줄의 x 와 문단 왼쪽 여백을 더한다.
// 줄 글자 표로 같은 탭 자리를 정한다. runIndex 는 그 탭 구간이다.
TabGap tabGapAtRun(const Node& line, const LineGlyphs& glyphs, std::int32_t pen, std::size_t runIndex,
                   const model::Document* doc);

bool claimLineWarn(const std::string& key);

std::vector<std::pair<std::int32_t, std::int32_t>> columnRuleSpans(const Node* page, const Node& parent,
                                                                   std::int32_t ox, std::int32_t oy,
                                                                   std::int32_t mid, std::int32_t halfW,
                                                                   std::int32_t top, std::int32_t bottom);

struct Painter {
    const model::Document* doc = nullptr;
    const Node* pageNode = nullptr;
    int pageIndex = -1;
    std::int32_t originX = 0;
    std::int32_t originY = 0;
    Bitmap bitmap;
    float scale = 1;
    bool limitView = false;
    // 쪽을 통째로 그릴 때는 띄운 개체의 캡션 글도 이 걸음에서 그린다. 줄 칸으로 그릴 때는 캡션 줄이 따로 칸이 된다.
    bool floatCaptions = false;
    float viewRightHu = 0;
    float viewBottomHu = 0;
    PaintLayer pass = PaintLayer::All;
    // 켜져 있으면 글자 픽셀 대신 제어점 경로를 담는다.
    FrameGlyphs* glyphSink = nullptr;
    // 켜져 있으면 그림 픽셀 대신 화면에서 그릴 자리와 원본 범위를 담는다.
    std::vector<FramePicture>* pictureSink = nullptr;
    // 켜져 있으면 표 테두리 선분과 모서리 잇기를 칠하지 않고 HU 로 적는다. 화면 셰이더가 그린다.
    // 비트맵이 잘려 있을 때는 CPU 로 칠한다. 화면은 칸 범위로만 자른다.
    std::vector<BorderPiece>* borderSink = nullptr;
    // 글 앞 개체의 면은 글 층보다 위에 합쳐진다. 그 면 안쪽 글은 같은 층에서 면 다음에 다시 그린다.
    int coverDepth = 0;
    // 떠 있는 개체 안쪽 글은 본문 글 층이 아니라 그 개체의 층에 둔다. 묶음 자식은 zOrder로 쌓인다.
    std::vector<PaintLayer> objectLayers;
    // 지금 그리는 글자를 감싼 글자처럼 개체. 바깥이 블록에 있으면 안쪽 글자도 뒤집는다.
    TextHost objectHosts[kObjectHostDepth]{};
    int objectHostCount = 0;
    // 머리말·꼬리말·쪽 번호. 채우기와 선 모두 본문 그림 아래다.
    int pageBandDepth = 0;

    bool show(PaintLayer layer) const { return pass == PaintLayer::All || pass == layer; }

    bool showGlyphs() const;

    struct ObjectLayerScope {
        Painter& painter;
        bool on = false;
        ObjectLayerScope(Painter& painter, bool on, PaintLayer layer) : painter(painter), on(on) {
            if (on) painter.objectLayers.push_back(layer);
        }
        ~ObjectLayerScope() {
            if (on) painter.objectLayers.pop_back();
        }
        ObjectLayerScope(const ObjectLayerScope&) = delete;
        ObjectLayerScope& operator=(const ObjectLayerScope&) = delete;
    };

    struct ObjectHostScope {
        Painter& painter;
        bool on = false;
        ObjectHostScope(Painter& painter, int story, int paragraph, int offset) : painter(painter) {
            if (painter.objectHostCount >= kObjectHostDepth) return;
            painter.objectHosts[painter.objectHostCount] = TextHost{story, paragraph, offset};
            ++painter.objectHostCount;
            on = true;
        }
        ~ObjectHostScope() {
            if (on) --painter.objectHostCount;
        }
        ObjectHostScope(const ObjectHostScope&) = delete;
        ObjectHostScope& operator=(const ObjectHostScope&) = delete;
    };

    // 지금 그리는 바깥 개체 자리를 글자에 붙인다. 앞 글자와 같으면 그 자리를 같이 쓴다.
    void stampHosts(FrameGlyph& glyph) const;

    struct NestedCaret {
        std::int32_t x = 0;
        std::int32_t y = 0;
        const Node* object = nullptr;
    };

    struct ObjectBand {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        int index = 0;
    };

    struct ClipBounds {
        std::int32_t x0;
        std::int32_t y0;
        std::int32_t x1;
        std::int32_t y1;
        ClipBounds()
            : x0(0),
              y0(0),
              x1(std::numeric_limits<std::int32_t>::max()),
              y1(std::numeric_limits<std::int32_t>::max()) {}
    };

    static ClipBounds intersectClip(const ClipBounds& a, std::int32_t x, std::int32_t y, std::int32_t w,
                                    std::int32_t h);

    static ClipBounds childClipOf(const Node& parent, std::int32_t px, std::int32_t py, const Node& child,
                                  const ClipBounds& cur);

    void seedObjectHosts(const Node& page, const std::vector<int>& path);

    bool seedAncestorClips(const Node& page, const std::vector<int>& path, std::int32_t padHu,
                           std::int32_t frameHuX, std::int32_t frameHuY, std::int32_t frameHuW,
                           std::int32_t frameHuH);

    struct CoverScope {
        Painter& painter;
        bool on = false;
        explicit CoverScope(Painter& painter, bool on) : painter(painter), on(on) {
            if (on) ++painter.coverDepth;
        }
        ~CoverScope() {
            if (on) --painter.coverDepth;
        }
        CoverScope(const CoverScope&) = delete;
        CoverScope& operator=(const CoverScope&) = delete;
    };

    static bool behindOf(const Node& node);

    PaintLayer objectPlace(const Node& node) const;

    PaintLayer frameStroke() const {
        return pageBandDepth > 0 ? PaintLayer::Behind : PaintLayer::Border;
    }

    struct BandScope {
        Painter& painter;
        bool on = false;
        BandScope(Painter& painter, bool on) : painter(painter), on(on) {
            if (on) ++painter.pageBandDepth;
        }
        ~BandScope() {
            if (on) --painter.pageBandDepth;
        }
        BandScope(const BandScope&) = delete;
        BandScope& operator=(const BandScope&) = delete;
    };

    bool coversText(const Node& node) const;

    // 칸·표에 적힌 높이 아래로 나간 내용까지. 배정 높이와 따로 그린다.
    static std::int32_t paintedExtent(const Node& node);

    static bool keepsFileHeight(const Node& node) {
        return node.kind == NodeKind::Cell || (node.kind == NodeKind::Object && node.role == "table");
    }

    // 묶음은 자식 개체의 위치와 너비를 모은 것이다. 적힌 묶음 너비로 자식을 자르지 않는다.
    // 덧말·글자 겹치기 묶음은 자르지 않는다. 본말보다 긴 덧말은 한글처럼 오른쪽으로 넘친다.
    static bool clipsOwnBox(const Node& node) {
        return node.kind == NodeKind::Object && node.role != "container" && node.role != "glyphGroup" &&
               !keepsFileHeight(node);
    }

    bool missesView(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) const;
    struct Clip {
        int l = 0;
        int t = 0;
        int r = 0;
        int b = 0;
    };
    std::vector<Clip> clips;
    int skiaSaves = 0;

    void applyCpuClip();

    // 글상자·칸 안쪽만 남긴다. 테두리는 이 자르기 전에 그린다.
    void pushClip(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h);

    void popClip();
    sk_sp<SkSurface> skiaSurface;
    SkCanvas* skiaCanvas = nullptr;

    SkCanvas* canvas();

    // 윤곽은 이미 한글 방식으로 편 직선이다. Skia 가 있으면 그 직선을 채우고, 없으면 우리 스캔라인이 채운다.
    // strokePx 가 0 보다 크면 윤곽 둘레에 그 굵기의 선을 함께 긋는다(굵게 흉내).
    void paintGlyph(const std::vector<GlyphCommand>& commands, float emPx, int ratio, int fitX, int fitY, float penX,
                    float baseY, std::uint8_t r, std::uint8_t g, std::uint8_t b, float strokePx = 0.f);

    int px(std::int32_t hu) const { return static_cast<int>(hu * scale + (hu < 0 ? -0.5f : 0.5f)); }

    float glyphPx(std::int32_t hu) const;

    void blit(int x, int y, int w, int h, const std::uint8_t* src, int channels, std::uint8_t r, std::uint8_t g,
              std::uint8_t b, bool colored);

    bool skipGlyphs = false;
    GlyphTurn glyphTurn;  // 지금 그리는 글자를 세로쓰기로 세운다
    // 자리만 남길 때는 칠·안내선·개체 그림을 그리지 않고 펜만 옮긴다.
    bool placesOnly = false;
    // 칸으로 나눌 때 글자처럼 넣은 개체는 줄 비트맵에 그리지 않는다. 그 개체 칸이 따로 있다.
    bool skipInlineObjects = false;
    bool pictureMissed = false;
    std::int32_t framePadHu = 0;
    std::vector<GlyphSpot>* placeSink = nullptr;

    void replaySpots(const std::vector<GlyphSpot>& spots);

    void glyph(char32_t cp, const std::string& font, int fontSize, int ratio, bool bold, bool italic,
               const std::string& color, std::int32_t huX, std::int32_t baseline, int story = -1,
               int paragraph = -1, int offset = -1);

    void box(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, const std::string& fill,
             const BorderEdge& edge, const std::string& role, PaintLayer place);
    void imageFlip(SkCanvas* canvas, int dw, int dh, bool flipH, bool flipV) const;

    // 같은 크기이거나 바둑판은 원본 픽셀 하나를 집는다.
    bool nearestImage(const std::uint8_t* rgba, int sw, int sh, int dx, int dy, int dw, int dh, bool flipH, bool flipV,
                      const ImageDraw* draw = nullptr, const CachedPicture* cached = nullptr);

    // 축소. 2-Pass 분리형 Scaled Catmull-Rom + 로컬 극값 클램핑(Anti-ringing)으로 선명도와 안티앨리어싱을 함께 유지한다.
    bool paintAreaImage(const std::uint8_t* src, int sw, int sh, int dx, int dy, int dw, int dh, bool flipH,
                        bool flipV, const ImageDraw* draw = nullptr);
    bool mitchellCpu(const std::uint8_t* rgba, int sw, int sh, int dx, int dy, int dw, int dh, bool flipH, bool flipV,
                     const ImageDraw* draw = nullptr, const CachedPicture* cached = nullptr);

    // 확대는 Catmull-Rom. CPU Skia가 그린다. 글자 폭은 조판 advance를 그대로 쓴다.
    bool mitchellImage(const std::uint8_t* rgba, int sw, int sh, int dx, int dy, int dw, int dh, bool flipH, bool flipV,
                       const CachedPicture* key, const ImageDraw* draw = nullptr);

    // 축소가 포함되면 분리형 Scaled Catmull-Rom 리샘플러로 선명하게 줄이고, 순수 확대는 Catmull-Rom 큐빅으로 그린다.
    bool skiaImage(const CachedPicture& image, int dx, int dy, int dw, int dh, bool flipH, bool flipV, bool filter,
                   const ImageDraw* draw = nullptr);

    bool skiaTile(const CachedPicture& image, int x, int y, int w, int h);

    void picture(const Node& node, std::int32_t x, std::int32_t y);

    void stamp(int x, int y, int radius, std::uint8_t r, std::uint8_t g, std::uint8_t b);

    void thickLine(int x0, int y0, int x1, int y1, int thick, std::uint8_t r, std::uint8_t g, std::uint8_t b);

    void drawPath(const Node& node, std::int32_t x, std::int32_t y, PaintLayer place);

    void equation(const Node& node, std::int32_t x, std::int32_t y, PaintLayer place);

    bool roundContains(int col, int row, int width, int height, int radius);

    void fillRoundRect(int x, int y, int w, int h, int rad, std::uint8_t r, std::uint8_t g, std::uint8_t b);

    // 도형 펜 굵기. 1픽셀보다 가늘면 1로 올린 뒤 그 굵기의 배수로 점선을 낸다.
    float penPx(const BorderEdge& edge) const;

    // GDI+ 도형 테두리. 표 칸의 정수 막대와 달리 굵기의 실수 배다. 0이면 실선이다.
    int gdiDash(const BorderEdge& edge, float* out) const;

    // 도형 DrawLine의 GDI+ 대시를 정수로 만든 것. 표 칸 선은 strokeTableBorder다.
    int dashPattern(const BorderEdge& edge, int* out) const;

    void roundBox(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, int ratio, const std::string& fill,
                  const BorderEdge& edge, PaintLayer place);

    void fillOval(int x0, int y0, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b);

    void ellipseBox(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, const std::string& fill,
                    const BorderEdge& edge, PaintLayer place);

    void objectBox(const Node& node, std::int32_t x, std::int32_t y);

    int edgeThickPx(const BorderEdge& edge) const;

    // 표 칸 선 번호. 0과 14 이상은 그리지 않는다. 12·13은 이중실선과 같다.
    static int tableLineKind(const std::string& type);

    // 겹선이 실제로 차지하는 픽셀 굵기. 이웃 모서리에서 이 절반까지만 닿는다.
    int borderInkPixels(const BorderEdge& edge) const;

    // 칸 모서리 HU가 중심선의 끝이다. insideSign 이 양수면 좌표가 커지는 쪽이 칸 안이다.
    // 한글은 굵고 얇은 이중선의 굵은 획을 항상 작은 좌표(왼쪽·위)에 둔다. 오른쪽·아래는 굵은 획이 칸 안이 된다.
    // 여기서는 칸 바깥을 굵게, 안쪽을 얇게 둔다.
    // 끝은 자르거나 뒤집지 않는다. 변은 중심선 길이 그대로 두고, 같은 선이 모서리에서 만나면 미터로 잇는다.
    // 기하 펜 PS_JOIN_MITER | PS_ENDCAP_FLAT 과 같다. 안티앨리어싱으로 구우면 칸을 늘릴 때 흐려진다.
    struct StrokeBand {
        float offset = 0;
        float width = 0;
    };
    int collectStrokeBands(const BorderEdge& edge, StrokeBand* bands) const;
    void fillBandSpan(bool horizontal, float along0, float along1, float crossCenter, float widthPx, std::uint8_t cr,
                      std::uint8_t cg, std::uint8_t cb);
    // 같은 선이 직각으로 만나면 각 획의 중심을 교점까지 밀고, 꺾임 바깥에 미터 사각을 채운다.
    void mitreTableCorner(std::int32_t hx, std::int32_t hy, int hInside, int vInside, const BorderEdge& edge);
    bool sinksBorder() const;
    void strokeTableBorder(const BorderEdge& edge, std::int32_t hx0, std::int32_t hy0, std::int32_t hx1,
                           std::int32_t hy1, int insideSign);

    void strokeEdge(const BorderEdge& edge, int ex, int ey, int ew, int eh);

    // 단 구분선은 두 단 사이 간격의 한가운데에, 그 단 높이만큼 그린다.
    void paintColumnRuleSpan(const BorderEdge& edge, std::int32_t mid, std::int32_t top, std::int32_t span);

    void paintColumnRules(const Node& parent, std::int32_t ox, std::int32_t oy);

    // 칸은 변마다 선이 다르다. 없는 변은 그리지 않고, 있는 변은 칸 경계에 맞춘다.
    // 채우기는 내용보다 먼저, 선은 내용 뒤에 그린다. 글이 선 안쪽을 덮지 않게 한다.
    void cellChrome(const Node& node, std::int32_t x, std::int32_t y, bool borders);
    // 테두리/배경 무늬. 비트맵 픽셀 상자에 1픽셀 줄을 긋는다. 간격은 한글처럼 1/12 인치(600)이고,
    // 칸이 갈려도 줄이 이어지게 쪽 원점에 맞춘다(한글 2024, tmp/hatch).
    void hatchRect(int x0, int y0, int w, int h, const std::string& style, const std::string& color);

    // 맞닿은 변은 칸마다 적혀 있고, 그릴 때는 하나만 남긴다.
    // 없는 선은 진다. 종류 번호가 작은 쪽이 이긴다. 실선 1, 파선 2, 점선 3, … 삼중선 11.
    // 번호가 같으면 굵기가 큰 쪽이 이긴다. 종류와 굵기가 같으면 아래가 위보다, 오른쪽이 왼쪽보다 우선이다.
    static bool edgeWins(const BorderEdge& cand, bool candPrimary, const BorderEdge& cur, bool curPrimary);

    void drawDiagonalSegment(const BorderEdge& edge, int x0, int y0, int x1, int y1);

    void cellDiagonals(const Node& node, std::int32_t x, std::int32_t y);

    // strip 범위는 표 HU. 띠가 겹치는 여백에서 같은 선을 두 띠가 서로 다른 반올림으로 긋지 않게,
    // 가로선은 Y 띠, 세로선은 X 띠 하나만 맡는다. -1 이면 자르지 않는다.
    void paintTableBorders(const Node& tableNode, std::int32_t ox, std::int32_t oy, std::int32_t stripY0 = 0,
                           std::int32_t stripY1 = -1, std::int32_t stripX0 = 0, std::int32_t stripX1 = -1);

    struct Mark {
        char32_t cp = 0;
        std::int32_t x = 0;
        std::int32_t width = 0;
        std::int32_t band = 0;
        int size = 1000;
        int ratio = 100;
        int relSize = 100;
        int charOffset = 0;
        std::uint32_t charShapeId = 0;
        bool bold = false;
        bool italic = false;
        bool superscript = false;
        bool subscript = false;
        bool adjustable = false;
        bool synthetic = false;  // 자동 번호 대신 넣은 글자. 그리기만 하고 커서 칸에서는 뺀다
        std::int32_t lead = 0;   // 글자 격자 칸 안에서 글자를 오른쪽으로 미는 폭. 캐럿은 칸 경계에 선다
    bool upright = false;    // 세로쓰기에서 세우는 글자. 칸 가운데를 축으로 돌려 그린다
        std::string font;
        std::string color;
        std::string background;
        std::string hatchStyle;
        std::string hatchColor;
        std::string shade;
        std::string markpen;
        std::string leader;
        const Node* object = nullptr;
        // 각주·미주 번호 전체와 글자마다의 폭. 1) 은 한 마크다. 비어 있으면 번호가 아니다.
        std::string noteText;
        std::vector<std::int32_t> noteAdvances;
        std::vector<std::int32_t> noteInks;
        int slot = -1;
        int run = -1;  // 줄 글자 표에서 만든 마크의 구간 번호
    };

    static const model::Style* findStyle(const model::Document& doc, std::uint32_t id);

    static const model::ParaShape* findParaShape(const model::Document& doc, std::uint32_t id);

    static const model::CharShape* findCharShape(const model::Document& doc, std::uint32_t id);

    static std::string resolveCharShapeFontName(const model::Document& doc, const model::CharShape& shape);

    struct LineFit {
        std::int32_t slide = 0;
        std::int32_t shift = 0;
        std::int32_t perSpace = 0;
        std::int32_t spaceLeft = 0;
        std::int32_t perGap = 0;
        // 띄어쓰기 없는 양쪽 정렬 줄은 남은 폭을 글자 뒤마다 나눈다(줄 마지막 글자와 개체 뒤는 뺀다).
        std::int32_t perChar = 0;
        std::int32_t charLeft = 0;
        int contentCount = 0;
        int firstGlyph = 0;
    };

    static bool markPads(const Mark& mark);
    // 띄어쓰기 없는 양쪽 정렬 줄에서 이 마크 뒤에 남은 폭을 나눠 주는가.
    static bool takesCharGap(const std::vector<Mark>& marks, std::size_t index, const LineFit& fit);

    // 글자 폭을 뺀 뒤의 들여쓰기와 정렬. 줄 비트맵과 글자처럼 넣은 개체 칸이 같은 값을 쓴다.
    LineFit fitLine(const Node& node, const std::vector<Mark>& marks) const;

    // 줄 안 마크마다 실제로 그려지는 가로 자리. 줄 x 에서 잰 HU 다.
    // 들여쓰기, 가운데·오른쪽 밀기, 양쪽·배분 정렬로 늘린 공백까지 들어 있다.
    // 글자를 그리는 쪽, 캐럿·블록·클릭, 글자처럼 넣은 개체와 그 테두리는 모두 이 값만 쓴다.
    // 같은 식을 다른 곳에 다시 쓰면 캐럿과 글자가 어긋난다.
    // 조판 부호 이름표 하나([각주], [메모 시작] 등). before 번째 마크 앞에 그 폭만큼 자리를 차지한다.
    // before 가 마크 수면 줄 끝이다. 글자 모양은 그 자리 본문 글자 모양이다(한글 도움말 control_code).
    struct LineLabel {
        std::size_t before = 0;
        std::string text;
        std::uint32_t charShapeId = 0;
        std::int32_t width = 0;
        std::vector<std::int32_t> advances;
        // 빈칸 글자(고정폭·묶음 빈 칸)를 대신한다. before 번째 마크 자리에 그리고 그 마크 폭 대신 이름표 폭을 쓴다.
        bool replaces = false;
    };
    // 지금 보기 표시. 셸이 켜는 편집 화면 설정이라 agent 의 쪽 그림에는 늘 꺼져 있다.
    ViewMarks shownMarks() const;
    // 줄 안 조판 부호 이름표. 조판 부호를 켜지 않았으면 비어 있다. 줄은 다시 나누지 않는다.
    std::vector<LineLabel> lineLabels(const Node& node, const std::vector<Mark>& marks) const;

    struct LinePlacement {
        LineFit fit;
        std::vector<std::int32_t> at;  // 마크 왼쪽
        std::int32_t end = 0;          // 마지막 마크 오른쪽. 마크가 없으면 들여쓰기 자리
        // 조판 부호 이름표와 그 왼쪽(줄 x 에서 잰 HU). 이름표 폭은 뒤 마크와 end 를 민다.
        std::vector<LineLabel> labels;
        std::vector<std::int32_t> labelAt;
    };
    LinePlacement placeLine(const Node& node, const std::vector<Mark>& marks) const;

    // 줄 안 글자 하나의 세로 자리. 줄 y + marginTop 에서 잰 기준선과, 그 글자 상자 위에서 기준선까지(ownBase).
    // 글자 상자 위 = 줄 y + marginTop + baseline - ownBase 다. 문단 세로 정렬과 글자 위치(%)가 들어 있다.
    // 글자를 그리는 쪽과 캐럿 높이는 모두 이 값을 쓴다. 위·첨자의 작은 글자 이동은 그리기만 더한다.
    struct GlyphBase {
        std::int32_t baseline = 0;
        std::int32_t ownBase = 0;
    };
    static GlyphBase glyphBaseIn(const Node& line, std::int32_t size, const std::string& font, int charOffset);

    void appendLineSeg(std::string& out, const model::LineSeg& seg, std::int32_t rise, std::int32_t below,
                       bool measured);

    static void quoteJson(std::string& out, const std::string& text);

    static void appendQuad(std::string& out, std::int32_t left, std::int32_t right, std::int32_t top,
                           std::int32_t bottom);

    static const char* objectLabel(const Node& node);

    void reportObject(const Node& node, std::int32_t pageX, std::int32_t pageY);

    void reportCaption(const Node& node, std::int32_t pageX, std::int32_t pageY);

    void reportLineSeg(const Node& node, std::int32_t pageX, std::int32_t pageY, bool force);

    // 줄의 글자 칸. 줄 글자 표(lineGlyphs)로 만든다.
    std::vector<Mark> collectMarks(const Node& node);
    // 줄 글자 표로 collectMarks 와 같은 마크를 만든다. 노드 주소로 찾는 캐시를 쓰지 않는다.
    std::vector<Mark> marksFromRuns(const Node& node, const LineGlyphs& glyphs);
    // 커서가 서는 글자만. 커서 칸 i 가 이 목록의 i 번째 글자다.
    std::vector<Mark> caretMarks(const Node& node);
    static void applyCharGrid(std::vector<Mark>& marks, std::int32_t cell, bool wongoji);
    // 세로쓰기: 세우는 글자는 1em 칸을 쓰고 그 칸 가운데에 선다. 줄을 나눌 때와 같은 verticalUpright 로 가른다.
    void applyVertical(std::vector<Mark>& marks, int direction) const;

    // 상대 크기는 그리는 글자만 바꾼다. 크기 × 상대 크기 / 100 을 반올림한다. 100이면 그대로다.
    // 위첨자·아래첨자는 그 글자 상자를 다시 크기 × 16 / 25 로 줄인다.
    // 줄의 textHeight와 spacing은 이 크기를 쓰지 않는다.
    static int relativeDrawSize(int size, int rel, bool superOrSub = false);

    // 위첨자·아래첨자의 기준선 이동. 줄어든 16/25 상자가 아니라 원래 글자 크기에 퍼센트를 곱해 그릴 때 Y에만 더한다.
    // 글꼴 기준이고 묶음 첫 바이트가 0이 아니면 가운데와 같은 −32/+24다. 첫 바이트가 0일 때만 −44/+12다.
    static std::int32_t scriptBaselineShift(int size, bool super, bool sub, const std::string& vertAlign,
                                            bool firstByteZero = true);

    static std::string hexOf(model::Color color, const std::string& fallback);

    const model::BorderFill* borderFillOf(std::uint32_t id) const;

    void paintRule(std::int32_t huX, std::int32_t huW, std::int32_t huY, const std::string& type,
                   const std::string& color, std::int32_t thickHu);

    void paintEmphasisMark(int kind, int cx, int cy, int rad, const std::string& color);

    void paintCharBorderBox(const model::BorderFill& fill, std::int32_t x, std::int32_t y, std::int32_t w,
                            std::int32_t h);

    // 그림자·외곽선·양각·음각은 글자보다 먼저 찍는다. 폭에는 더하지 않는다.
    void paintGlyphEffects(const model::CharShape& cs, const Mark& mark, std::int32_t at,
                           std::int32_t glyphBaseline, int drawSize, int story, int paragraph, int offset);

    void paintBitmapDeco(const model::CharShape& cs, const Mark& mark, std::int32_t at, std::int32_t lineTop,
                         int lineBox, std::int32_t glyphBaseline, int drawSize);

    void paintParagraphChrome(const Node& node, std::int32_t ox, std::int32_t oy, int join, bool fill, bool stroke);

    void drawHeading(const Node& line, std::int32_t x, std::int32_t y, std::int32_t shift, std::int32_t slide);

    void line(const Node& node, std::int32_t x, std::int32_t y, bool inObject);
    // 투명 선(보기 표시) 한 줄. 표 칸·글상자의 선 없는 변이다.
    void strokeTransparent(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1);
    // 교정 부호 한 줄 몫. x0·x1 은 줄 x 에서 잰 HU(범위 시작·끝, 자리만 있는 부호는 x0 하나)이고, head·tail 은 범위의
    // 처음·끝이 이 줄에 있는지다. lineLeft 는 줄 머리(첫 마크 자리)다.
    struct ProofSpan {
        model::ProofKind kind = model::ProofKind::None;
        int sign = 0;
        std::string text;
        std::int32_t x0 = 0;
        std::int32_t x1 = 0;
        std::int32_t lineLeft = 0;
        bool head = false;
        bool tail = false;
        std::uint32_t charShapeId = 0;
        std::int32_t split = -1;     // 자리 바꿈표: 이 줄 안 가른 자리(%spl). 없으면 -1
        std::int32_t lineRight = 0;  // 줄 오른쪽 끝(줄 x 기준). 줄 서로 바꿈표가 쓴다
        std::int32_t moveStart = 0;  // 자리 옮김표: 시작 위치·간격(HWPUNIT, 줄 머리에서)
        std::int32_t moveGap = 0;
    };
    std::vector<ProofSpan> lineProofs(const Node& node, const std::vector<Mark>& marks, const LinePlacement& place) const;
    void drawProofMarks(const Node& node, std::int32_t x, std::int32_t y, const std::vector<Mark>& marks,
                        const LinePlacement& place);
    // 글 뒤에 까는 교정 부호 바탕(자료 연결의 회색). 글자보다 먼저 그린다.
    void drawProofBackgrounds(const Node& node, std::int32_t x, std::int32_t y, const LinePlacement& place);
    // 보기 표시: 문단 부호(↵ ↓)와 조판 부호(빈칸 ∨, 탭 ⇥, [이름표]). 화면에만 그린다.
    void drawLineMarks(const Node& node, std::int32_t x, std::int32_t y, const std::vector<Mark>& marks,
                       const LinePlacement& place);

    void strokePageBorderNode(const Node& node, std::int32_t x, std::int32_t y);

    void paintPiece(const Node& node, int kind, std::int32_t x, std::int32_t y, bool inObject,
                    std::int32_t stripY0 = 0, std::int32_t stripY1 = -1, std::int32_t stripX0 = 0,
                    std::int32_t stripX1 = -1);

    void paintPageFill(const Node& paper, std::int32_t ox, std::int32_t oy);

    // 쪽 테두리 함수는 채운 다음 선을 긋는다. 글자와의 순서는 확인하지 못했다.
    // 글이 선을 덮지 않게 선은 본문 앞에 그린다.
    void paintPageBorder(const Node& paper, std::int32_t ox, std::int32_t oy);

    void walk(const Node& node, std::int32_t ox, std::int32_t oy, bool inObject = false, int borderJoin = 0);
    // 세로쓰기 본문(turned). 돌린 틀의 비트맵에 그린 뒤 시계 방향으로 돌려 붙인다.
    void walkTurned(const Node& node, std::int32_t ox, std::int32_t oy, bool inObject, int borderJoin);
    bool turning = false;
    // 돌린 틀 안에서 만난 세운 개체. 틀을 종이로 돌려 놓은 뒤 종이 자리(x, y)에 그대로 그린다.
    struct UprightLater {
        const Node* node = nullptr;
        std::int32_t x = 0;
        std::int32_t y = 0;
        bool inObject = false;
    };
    std::vector<UprightLater> uprightLater;
    std::int32_t turnedBitmapHu = 0;  // 돌린 틀로 그리는 동안 종이 비트맵 폭(HU)

    // 글 뒤 개체를 먼저 깔고, 글자를 그린 다음, 글 앞 개체를 올린다. 같은 층은 zOrder가 클수록 위다.
    void paintStackedChildren(const Node& node, std::int32_t x, std::int32_t y, bool inObject);

    // 캡션은 개체 자식이지만 상자 밖에 있다. 글자처럼 넣은 개체는 줄 비트맵에 그리고,
    // 떠 있는 개체는 캡션 줄이 자기 프레임을 가진다.
    void paintCaptions(const Node& node, std::int32_t x, std::int32_t y, bool inObject);

    void expandObjectBandBounds(const Node& node, std::int32_t ox, std::int32_t oy, std::int32_t& x0,
                                std::int32_t& y0, std::int32_t& x1, std::int32_t& y1, bool& any);

    // 그리기와 같은 글자 경계. 빈 줄은 들여쓴 시작 한 곳이다.
    std::vector<std::int32_t> glyphEdgesWithMarks(const Node& node, const std::vector<Mark>& marks,
                                                  std::int32_t lineX, std::int32_t lineY,
                                                  std::vector<NestedCaret>* nested,
                                                  std::vector<ObjectBand>* bands = nullptr);

    std::vector<std::int32_t> glyphEdges(const Node& node, std::int32_t lineX, std::int32_t lineY,
                                         std::vector<NestedCaret>* nested, std::vector<ObjectBand>* bands = nullptr);

    // 처음 클릭은 글자 한가운데. 드래그 중과 끝은 그 글자의 60%를 넘어야 넘어간 것이다.
    void chooseCaretEdge(const Node& node, const std::vector<std::int32_t>& edges, std::int32_t px, bool drag,
                         int anchorPara, int anchorOff, int& chosenIndex, std::int32_t& chosen) const;

    static bool lineHasObjectChild(const Node& line);

    static bool subtreeHasStory(const Node& node, int story);

    static bool subtreeHasStoryRange(const Node& node, int story, int fromParagraph, int toParagraph);

    void hitLine(const Node& node, std::int32_t lineX, std::int32_t lineY, std::int32_t px, std::int32_t py,
                 CaretPlace& best, bool requireInside, bool drag, int anchorPara, int anchorOff);

    void walkCaret(const Node& node, std::int32_t ox, std::int32_t oy, std::int32_t px, std::int32_t py,
                   CaretPlace& best, bool drag, int anchorPara, int anchorOff);

    // 세로쓰기 틀. 원점은 종이의 (x, y), 가로 폭은 span 이다. 틀의 (lx, ly) 는 종이의 (x + span − ly, y + lx) 다.
    // span 이 0 이면 돌리지 않은 종이 그대로다.
    struct TurnFrame {
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t span = 0;
        bool on() const { return span > 0; }
    };
    // x, y 는 줄이 든 틀의 좌표다. turn 이 켜져 있으면 세로쓰기 틀의 줄이라 돌린 틀 좌표다.
    struct StoryLine {
        const Node* node = nullptr;
        std::int32_t x = 0;
        std::int32_t y = 0;
        TurnFrame turn;
    };
    // 종이 점을 줄 방향 좌표로. 세로쓰기 줄은 종이 Y 가 줄 방향이다.
    static std::int32_t alongLine(const StoryLine& line, std::int32_t x, std::int32_t y) {
        return line.turn.on() ? y - line.turn.y : x;
    }
    // 돌린 틀에서 잡은 커서를 종이 자리로 바꾼다. 막대는 눕는다.
    static void turnCaretToPaper(CaretPlace& place, const TurnFrame& turn) {
        if (!turn.on() || !place.hit) return;
        const std::int32_t along = place.x;
        place.x = turn.x + turn.span - (place.y + place.height);
        place.y = turn.y + along;
        place.turned = true;
    }
    // 종이 점 → 돌린 틀 점.
    static void paperToTurn(const TurnFrame& turn, std::int32_t& x, std::int32_t& y) {
        if (!turn.on()) return;
        const std::int32_t lx = y - turn.y;
        y = turn.x + turn.span - x;
        x = lx;
    }
    // 줄·커서·블록을 모을 때 지금 든 종이 폭과 세로쓰기 틀. 돌린 틀 노드에 들어갈 때 하나씩 바꾼다.
    std::int32_t caretPaperW = 0;
    TurnFrame caretTurn;
    const Node* caretTurnNode = nullptr;
    // 돌린 틀(turned)이나 세운 개체 상자(upright)에 들어갈 때 틀을 바꾸고, 나올 때 되돌린다.
    // turned: 원점은 부모 자리(ox, oy)고 그 아래 좌표는 (0, 0)부터다. upright: 틀의 상자를 종이 자리로 돌려 그 자리가 원점이다.
    struct CaretTurnScope {
        Painter& painter;
        TurnFrame saved;
        const Node* savedNode;
        explicit CaretTurnScope(Painter& p) : painter(p), saved(p.caretTurn), savedNode(p.caretTurnNode) {}
        ~CaretTurnScope() {
            painter.caretTurn = saved;
            painter.caretTurnNode = savedNode;
        }
    };
    // node 가 돌린 틀이면 틀을 켜고 (ox, oy) 를 (0, 0) 으로, 세운 상자면 틀을 끄고 (ox, oy) 를 그 종이 자리로 바꾼다.
    // 바꿨으면 true 다. 자식은 바뀐 (ox, oy) 에 node.x·node.y 를 더하지 않고 놓아야 하는 세운 상자만 uprightBox 를 켠다.
    // 글자처럼 넣은 세운 개체 상자의 자식 원점. 틀의 상자 원점 (x, y) 를 종이 자리로 바꾸고 틀을 끈다.
    void enterUprightObject(const Node& object, std::int32_t& x, std::int32_t& y) {
        if (!object.upright || !caretTurn.on()) return;
        const std::int32_t paperX = caretTurn.x + caretTurn.span - y - object.height;
        const std::int32_t paperY = caretTurn.y + x;
        caretTurn = {};
        x = paperX;
        y = paperY;
    }
    bool enterCaretFrame(const Node& node, std::int32_t& ox, std::int32_t& oy, bool& uprightBox) {
        uprightBox = false;
        if (node.kind == NodeKind::Paper) caretPaperW = node.width;
        if (node.turned != 0 && caretTurnNode != &node) {
            caretTurn = {ox, oy, node.turnSpan > 0 ? node.turnSpan : caretPaperW};
            caretTurnNode = &node;
            ox = 0;
            oy = 0;
            return true;
        }
        if (node.upright && caretTurn.on()) {
            const std::int32_t bx = ox + node.x;
            const std::int32_t by = oy + node.y;
            const std::int32_t paperX = caretTurn.x + caretTurn.span - by - node.height;
            const std::int32_t paperY = caretTurn.y + bx;
            caretTurn = {};
            ox = paperX;
            oy = paperY;
            uprightBox = true;
            return true;
        }
        return false;
    }

    static bool posBefore(int paragraph, int offset, int otherParagraph, int otherOffset) {
        return paragraph < otherParagraph || (paragraph == otherParagraph && offset < otherOffset);
    }

    // 줄에 앉힌 개체는 그 줄의 들여쓰기·정렬·자간만큼 밀린 자리에 그린다.
    // 드래그 커서와 블록 칸도 그 자리의 자식으로 들어간다.
    template <typename Visit>
    void enterPaintedObjects(const Node& line, std::int32_t lineX, std::int32_t lineY, Visit&& visit) {
        bool hasObject = false;
        for (const Node& child : line.children) {
            if (child.kind == NodeKind::Object) {
                hasObject = true;
                break;
            }
        }
        if (!hasObject) return;
        const LineBlockCacheEntry& cached = ensureLineBlockCache(line, false, true);
        std::unordered_set<const Node*> entered;
        for (const LineBlockNested& item : cached.relNested) {
            if (!item.object) continue;
            entered.insert(item.object);
            const CaretTurnScope scope(*this);
            std::int32_t cx = lineX + item.relX;
            std::int32_t cy = lineY + item.relY;
            enterUprightObject(*item.object, cx, cy);
            for (const Node& child : item.object->children) {
                visit(child, cx, cy, item.object);
            }
        }
        for (const Node& child : line.children) {
            if (child.kind != NodeKind::Object || entered.find(&child) != entered.end()) continue;
            visit(child, lineX, lineY, static_cast<const Node*>(nullptr));
        }
    }

    void gatherStory(const Node& node, std::int32_t ox, std::int32_t oy, int story, std::vector<StoryLine>& out);

    void gatherStoryParagraph(const Node& node, std::int32_t ox, std::int32_t oy, int story, int paraIdx,
                              std::vector<StoryLine>& out);

    CaretPlace placeOnLine(const StoryLine& line, std::int32_t px, bool drag, int anchorPara, int anchorOff);

    void applyCaretCharMetrics(const Node& node, std::int32_t lineY, const std::vector<Mark>& marks, int chosenIndex,
                               CaretPlace& best) const;

    CaretPlace placeOnLineAtOffset(const StoryLine& line, int offset);

    // 세로는 더 가까운 줄이 이긴다. 같은 높이의 단이 여러 개면 누른 가로가 들어 있는 줄을 고른다.
    const StoryLine* nearestLine(const std::vector<StoryLine>& lines, std::int32_t x, std::int32_t y) const;

    const StoryLine* edgeLine(const std::vector<StoryLine>& lines, bool atStart) const;

    static bool paintColor(const std::string& color) { return !color.empty() && color != "none"; }

    // 글자 바로 뒤의 색. 형광펜이 글자 배경 위에 있고, 둘 다 없으면 칸이나 종이다.
    static std::string markBackground(const Mark* mark, const std::string& inherited);

    void pushBlockRgb(std::vector<BlockBox>& out, int page, std::int32_t left, std::int32_t right, std::int32_t top,
                      std::int32_t height, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                      const ClipBounds& clip) const;

    void pushBlock(std::vector<BlockBox>& out, int page, std::int32_t left, std::int32_t right, std::int32_t top,
                   std::int32_t height, const std::string& color, const ClipBounds& clip) const;

    const LineBlockCacheEntry& ensureLineBlockCache(const Node& node, bool needBands, bool needNested);

    void appendBlock(const Node& node, std::int32_t ox, std::int32_t oy, int page, int story, int fromParagraph,
                     int fromOffset, int toParagraph, int toOffset, std::vector<BlockBox>& out,
                     const std::string& inheritedFill, const ClipBounds& clip = ClipBounds{});

    // 고른 구간의 글자만 모은다. 문단이 바뀌면 줄을 바꾸고, 글자처럼 넣은 개체는 뺀다.
    std::string copySelection(const std::vector<Node>& pages, int story, int fromParagraph, int fromOffset,
                              int toParagraph, int toOffset);
};


// 그 글 묶음의 문단이 있는 쪽마다 fn(쪽 번호, 쪽) 을 부른다. hintPage 근처부터 찾는다.
template <typename Fn>
void forEachStoryParagraphPage(const std::vector<Node>& pages, int story, int fromParagraph, int toParagraph,
                               int hintPage, Fn&& fn) {
    const int n = static_cast<int>(pages.size());
    if (n <= 0 || story <= 0) return;
    if (fromParagraph <= toParagraph && hintPage >= 0 && hintPage < n) {
        int seed = -1;
        if (Painter::subtreeHasStoryRange(pages[static_cast<std::size_t>(hintPage)], story, fromParagraph,
                                          toParagraph)) {
            seed = hintPage;
        } else if (hintPage > 0 &&
                   Painter::subtreeHasStoryRange(pages[static_cast<std::size_t>(hintPage - 1)], story, fromParagraph,
                                                 toParagraph)) {
            seed = hintPage - 1;
        } else if (hintPage + 1 < n &&
                   Painter::subtreeHasStoryRange(pages[static_cast<std::size_t>(hintPage + 1)], story, fromParagraph,
                                                 toParagraph)) {
            seed = hintPage + 1;
        }
        if (seed >= 0) {
            int pStart = seed;
            while (pStart > 0) {
                if (Painter::subtreeHasStoryRange(pages[static_cast<std::size_t>(pStart - 1)], story, fromParagraph,
                                                  toParagraph)) {
                    --pStart;
                } else if (pStart > 1 &&
                           Painter::subtreeHasStoryRange(pages[static_cast<std::size_t>(pStart - 2)], story,
                                                         fromParagraph, toParagraph)) {
                    pStart -= 2;
                } else {
                    break;
                }
            }
            int pEnd = seed;
            while (pEnd + 1 < n) {
                if (Painter::subtreeHasStoryRange(pages[static_cast<std::size_t>(pEnd + 1)], story, fromParagraph,
                                                  toParagraph)) {
                    ++pEnd;
                } else if (pEnd + 2 < n &&
                           Painter::subtreeHasStoryRange(pages[static_cast<std::size_t>(pEnd + 2)], story,
                                                         fromParagraph, toParagraph)) {
                    pEnd += 2;
                } else {
                    break;
                }
            }
            for (int p = pStart; p <= pEnd; ++p) {
                const Node& curPage = pages[static_cast<std::size_t>(p)];
                if (!Painter::subtreeHasStoryRange(curPage, story, fromParagraph, toParagraph)) continue;
                fn(p, curPage);
            }
            return;
        }
    }
    for (int p = 0; p < n; ++p) {
        const Node& curPage = pages[static_cast<std::size_t>(p)];
        if (!Painter::subtreeHasStoryRange(curPage, story, fromParagraph, toParagraph)) continue;
        fn(p, curPage);
    }
}

}  // namespace chwp::render::paint_detail
