#include "render/border_mesh.hpp"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace noting::render::paint_detail {

namespace {

std::unordered_map<int, std::shared_ptr<const FrameBorders>> gFrameBorders;
std::mutex gFrameBorderMu;
std::atomic<bool> gForcedOnScreen{false};

}  // namespace

void storeFrameBorders(int id, float scale, std::vector<BorderPiece> pieces) {
    if (id == 0 || pieces.empty()) {
        dropFrameBorders(id);
        return;
    }
    auto made = std::make_shared<FrameBorders>();
    made->scale = scale;
#if NOTING_HAS_SKIA
    made->mesh = makeBorderMesh(pieces);
#endif
    made->pieces = std::move(pieces);
    std::lock_guard lock(gFrameBorderMu);
    gFrameBorders[id] = std::move(made);
}

void dropFrameBorders(int id) {
    std::lock_guard lock(gFrameBorderMu);
    gFrameBorders.erase(id);
}

void clearFrameBorders() {
    std::lock_guard lock(gFrameBorderMu);
    gFrameBorders.clear();
}

bool frameHasBorders(int id) {
    std::lock_guard lock(gFrameBorderMu);
    return gFrameBorders.count(id) > 0;
}

bool cloneFrameBorders(int srcId, int dstId) {
    if (srcId == 0 || dstId == 0) return false;
    std::lock_guard lock(gFrameBorderMu);
    const auto found = gFrameBorders.find(srcId);
    if (found == gFrameBorders.end()) return false;
    // 좌표는 칸 안이라 칸이 옮겨 가도 그대로다.
    if (srcId != dstId) gFrameBorders[dstId] = found->second;
    return true;
}

std::shared_ptr<const FrameBorders> frameBorders(int id) {
    std::lock_guard lock(gFrameBorderMu);
    const auto found = gFrameBorders.find(id);
    return found == gFrameBorders.end() ? nullptr : found->second;
}

void forceBordersOnScreen(bool on) { gForcedOnScreen = on; }

#if defined(__EMSCRIPTEN__) && NOTING_HAS_SKIA
bool screenVectors();  // paint.cpp
#endif

bool bordersOnScreen() {
#if defined(__EMSCRIPTEN__) && NOTING_HAS_SKIA
    if (screenVectors()) return true;
#endif
    return gForcedOnScreen;
}

}  // namespace noting::render::paint_detail

#if NOTING_HAS_SKIA
#include <algorithm>
#include <cstring>
#include <utility>

#include "include/core/SkBlender.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkPaint.h"
#include "include/core/SkString.h"

namespace noting::render::paint_detail {

namespace {

// 정점마다 조각 전체를 싣는다. 정점 셰이더는 조각을 덮는 사각만 내고, 칠할 픽셀은 조각 셰이더가 정한다.
//   seg   = 선분 양 끝 HU (모서리는 교점만)
//   style = 굵기 HU, 선 번호, 안쪽(모서리는 가로선 안쪽), 꼭짓점 번호 + 4×모서리 + 8×세로선 안쪽이 양수
struct Vertex {
    float seg[4];
    float style[4];
    std::uint8_t color[4];
};
static_assert(sizeof(Vertex) == 36);

constexpr std::size_t kPiecesPerChunk = 65536 / 4;

// Skia 가 셰이더 앞에 구조체를 붙여 #version 300 을 못 쓴다. 엄격한 ES2 라서
// 비트 연산·나머지·정수 min/max·동적 배열 색인이 없다. 정수 값은 실수 varying 으로 넘겨 반올림한다.
constexpr const char* kShared = R"(
uniform float2 scale;
uniform float wscale;
uniform float2 origin;

int lr(float v) { return v >= 0 ? int(floor(v + 0.5)) : -int(floor(-v + 0.5)); }
int ni(float v) { return int(floor(v + 0.5)); }
int imax(int a, int b) { return a > b ? a : b; }
)";

constexpr const char* kVertex = R"(
Varyings main(const Attributes a) {
    Varyings v;
    v.vSeg = a.seg;
    v.vStyle = a.style;
    v.vColor = a.color;
    int W = imax(1, lr(a.style.x * wscale));
    float reach = float(imax(W, 6)) + 3.0;
    float code = a.style.w;
    float vInside = floor(code / 8.0);
    float isCorner = floor((code - 8.0 * vInside) / 4.0);
    int corner = int(code - 8.0 * vInside - 4.0 * isCorner + 0.5);
    float x0 = a.seg.x * scale.x;
    float y0 = a.seg.y * scale.y;
    float x1 = a.seg.z * scale.x;
    float y1 = a.seg.w * scale.y;
    float l; float t; float r; float b;
    if (isCorner > 0.5) {
        l = x0 - reach; r = x0 + reach; t = y0 - reach; b = y0 + reach;
    } else if (abs(x1 - x0) >= abs(y1 - y0)) {
        l = min(x0, x1) - 2.0; r = max(x0, x1) + 2.0;
        float mid = (y0 + y1) * 0.5;
        t = mid - reach; b = mid + reach;
    } else {
        t = min(y0, y1) - 2.0; b = max(y0, y1) + 2.0;
        float mid = (x0 + x1) * 0.5;
        l = mid - reach; r = mid + reach;
    }
    float px = (corner == 1 || corner == 3) ? r : l;
    float py = corner >= 2 ? b : t;
    v.position = float2(floor(px), floor(py)) + origin;
    return v;
}
)";

constexpr const char* kFragment = R"(
// Painter::collectStrokeBands
void bandsOf(int kind, int W, out int count, out float o0, out float w0, out float o1, out float w1, out float o2,
             out float w2) {
    count = 1; o0 = 0.0; w0 = float(W); o1 = 0.0; w1 = 0.0; o2 = 0.0; w2 = 0.0;
    if (kind == 8 || (kind == 11 && W <= 2)) {
        if (W >= 2) {
            int snapped = W;
            int rem = snapped - (snapped / 4) * 4;
            if (rem == 1) snapped -= 1;
            else if (rem == 2) snapped += 2;
            else if (rem == 3) snapped += 1;
            float s = float(imax(1, snapped / 4));
            o0 = -1.5 * s; w0 = s; o1 = 1.5 * s; w1 = s; count = 2;
        }
    } else if (kind == 9 || kind == 10) {
        if (W > 2) {
            int w = W;
            if (w == 3 || w == 4) w = 5;
            float thin = float(imax(w / 4, 1));
            float thick = float(w) - 2.0 * thin;
            float outerW = kind == 10 ? thick : thin;
            float innerW = kind == 10 ? thin : thick;
            float hw = float(w) * 0.5;
            o0 = -(hw - outerW * 0.5); w0 = outerW;
            o1 = hw - innerW * 0.5; w1 = innerW;
            count = 2;
        }
    } else if (kind == 11) {
        int w = W;
        if (w == 3) w = 5;
        else if (w == 4 || w == 5) w = 6;
        float outer = float(imax(w / 6, 1));
        float midW = float(w) - 4.0 * outer;
        float hw = float(w) * 0.5;
        o0 = -(hw - outer * 0.5); w0 = outer;
        o1 = -(hw - 2.0 * outer - midW * 0.5); w1 = midW;
        o2 = hw - outer * 0.5; w2 = outer;
        count = 3;
    }
}

// Painter::fillBandSpan 이 칠하는 사각 안인지. pa 는 길이 방향, pc 는 가로지르는 방향 픽셀이다.
bool inSpan(int pa, int pc, float along0, float along1, float crossCenter, float widthPx) {
    if (abs(along1 - along0) < 0.5 || widthPx <= 0.0) return false;
    int thick = imax(1, lr(widthPx));
    int a0 = lr(min(along0, along1));
    int a1 = lr(max(along0, along1));
    int top = lr(crossCenter - float(thick) * 0.5);
    return pa >= a0 && pa < a0 + imax(1, a1 - a0) && pc >= top && pc < top + thick;
}

// Painter::strokeTableBorder 의 한 띠. 점선 마디는 (주기 번호 × 주기 + 앞마디 합)으로 바로 잰다.
// cross 는 WGSL 내장 함수 이름이라 Skia 가 이름을 반만 바꾼다. 변수 이름으로 쓰지 않는다.
bool inBand(int pa, int pc, float org, float span, float center, float wid, int dashes, float period, float i0,
            float i2, float i4, float p2, float p4) {
    if (dashes < 2 || period <= 0.0) return inSpan(pa, pc, org, org + span, center, wid);
    int k0 = imax(0, int(floor((float(pa) - org) / period)) - 1);
    for (int j = 0; j < 3; ++j) {
        float base = float(k0 + j) * period;
        if (base >= span) break;
        for (int s = 0; s < 3; ++s) {
            if (s * 2 >= dashes) break;
            float n = s == 0 ? i0 : (s == 1 ? i2 : i4);
            float cursor = base + (s == 0 ? 0.0 : (s == 1 ? p2 : p4));
            if (cursor >= span) break;
            float draw = min(n, span - cursor);
            if (draw > 0.0 && inSpan(pa, pc, org + cursor, org + cursor + draw, center, wid)) return true;
        }
    }
    return false;
}

float2 main(const Varyings v, out half4 color) {
    color = half4(0);
    float widthHu = float(ni(v.vStyle.x));
    int kind = ni(v.vStyle.y);
    float inside = v.vStyle.z >= 0.0 ? 1.0 : -1.0;
    float code = float(ni(v.vStyle.w));
    float vInside = floor(code / 8.0) > 0.5 ? 1.0 : -1.0;
    bool isCorner = floor((code - 8.0 * floor(code / 8.0)) / 4.0) > 0.5;
    float2 local = v.position - origin;
    int px = int(floor(local.x));
    int py = int(floor(local.y));
    int W = imax(1, lr(widthHu * wscale));
    int count; float o0; float w0; float o1; float w1; float o2; float w2;
    bandsOf(kind, W, count, o0, w0, o1, w1, o2, w2);
    bool hit = false;
    if (isCorner) {
        // Painter::mitreTableCorner
        if (kind == 0 || (kind >= 2 && kind <= 7)) return v.position;
        float cx = float(ni(v.vSeg.x)) * scale.x;
        float cy = float(ni(v.vSeg.y)) * scale.y;
        for (int i = 0; i < 3; ++i) {
            if (i >= count) break;
            float off = i == 0 ? o0 : (i == 1 ? o1 : o2);
            float wid = i == 0 ? w0 : (i == 1 ? w1 : w2);
            float hw = wid * 0.5;
            float bx = cx + vInside * off;
            float by = cy + inside * off;
            if (inSpan(px, py, cx, bx, by, wid) || inSpan(py, px, cy, by, bx, wid)) hit = true;
            float sx = bx - vInside * hw;
            float sy = by - inside * hw;
            int l = lr(min(bx, sx));
            int t = lr(min(by, sy));
            int r = lr(max(bx, sx));
            int b = lr(max(by, sy));
            if (r > l && b > t && px >= l && px < r && py >= t && py < b) hit = true;
        }
    } else {
        float x0 = float(ni(v.vSeg.x)) * scale.x;
        float y0 = float(ni(v.vSeg.y)) * scale.y;
        float x1 = float(ni(v.vSeg.z)) * scale.x;
        float y1 = float(ni(v.vSeg.w)) * scale.y;
        float dx = x1 - x0;
        float dy = y1 - y0;
        bool horizontal = abs(dx) >= abs(dy);
        float mid = horizontal ? (y0 + y1) * 0.5 : (x0 + x1) * 0.5;
        float org = horizontal ? min(x0, x1) : min(y0, y1);
        float span = horizontal ? abs(dx) : abs(dy);
        int pa = horizontal ? px : py;
        int pc = horizontal ? py : px;
        // Painter::gdiDash. 펜 굵기는 1픽셀보다 가늘면 1이다.
        float pen = max(1.0, abs(widthHu * wscale));
        int dashes = 0;
        float i0 = 0; float i1 = 0; float i2 = 0; float i3 = 0; float i4 = 0; float i5 = 0;
        if (kind == 2) { i0 = 7.5 * pen; i1 = 4.5 * pen; dashes = 2; }
        else if (kind == 6) { i0 = 15.0 * pen; i1 = 4.5 * pen; dashes = 2; }
        else if (kind == 3) { i0 = 1.5 * pen; i1 = 2.25 * pen; dashes = 2; }
        else if (kind == 4 || kind == 5) {
            i0 = 15.0 * pen; i1 = 4.5 * pen; i2 = 1.5 * pen; i3 = 4.5 * pen; dashes = 4;
            if (kind == 5) { i4 = 1.5 * pen; i5 = 4.5 * pen; dashes = 6; }
        }
        float p1 = i0;
        float p2 = p1 + i1;
        float p3 = p2 + i2;
        float p4 = p3 + i3;
        float p5 = p4 + i4;
        float period = dashes == 2 ? p2 : (dashes == 4 ? p4 : p5 + i5);
        for (int i = 0; i < 3; ++i) {
            if (i >= count) break;
            float off = i == 0 ? o0 : (i == 1 ? o1 : o2);
            float wid = i == 0 ? w0 : (i == 1 ? w1 : w2);
            if (inBand(pa, pc, org, span, mid + off * inside, wid, dashes, period, i0, i2, i4, p2, p4)) hit = true;
        }
    }
    if (hit) color = v.vColor;
    return v.position;
}
)";

sk_sp<SkMeshSpecification> compileSpec(SkString& error) {
    using Attr = SkMeshSpecification::Attribute;
    using Vary = SkMeshSpecification::Varying;
    const Attr attrs[] = {
        {Attr::Type::kFloat4, 0, SkString("seg")},
        {Attr::Type::kFloat4, 16, SkString("style")},
        {Attr::Type::kUByte4_unorm, 32, SkString("color")},
    };
    const Vary varys[] = {
        {Vary::Type::kFloat4, SkString("vSeg")},
        {Vary::Type::kFloat4, SkString("vStyle")},
        {Vary::Type::kHalf4, SkString("vColor")},
    };
    SkString vs(kShared);
    vs.append(kVertex);
    SkString fs(kShared);
    fs.append(kFragment);
    auto result = SkMeshSpecification::Make(attrs, sizeof(Vertex), varys, vs, fs, SkColorSpace::MakeSRGB(),
                                            kPremul_SkAlphaType);
    error = result.error;
    return result.specification;
}

struct SpecSlot {
    sk_sp<SkMeshSpecification> spec;
    SkString error;
};

const SpecSlot& specSlot() {
    static const SpecSlot slot = [] {
        SpecSlot made;
        made.spec = compileSpec(made.error);
        return made;
    }();
    return slot;
}

sk_sp<SkMesh::IndexBuffer> sharedIndices() {
    static const sk_sp<SkMesh::IndexBuffer> indices = [] {
        std::vector<std::uint16_t> data;
        data.reserve(kPiecesPerChunk * 6);
        for (std::size_t i = 0; i < kPiecesPerChunk; ++i) {
            const auto base = static_cast<std::uint16_t>(i * 4);
            for (std::uint16_t corner : {0, 1, 2, 2, 1, 3}) data.push_back(static_cast<std::uint16_t>(base + corner));
        }
        return SkMeshes::MakeIndexBuffer(data.data(), data.size() * sizeof(std::uint16_t));
    }();
    return indices;
}

}  // namespace

sk_sp<SkMeshSpecification> borderMeshSpec(const char** error) {
    const SpecSlot& slot = specSlot();
    if (error) *error = slot.error.c_str();
    return slot.spec;
}

BorderMesh makeBorderMesh(const std::vector<BorderPiece>& pieces) {
    BorderMesh mesh;
    if (pieces.empty()) return mesh;
    float l = pieces.front().x0, t = pieces.front().y0, r = l, b = t;
    for (const BorderPiece& piece : pieces) {
        for (const auto& [x, y] : {std::pair{piece.x0, piece.y0}, std::pair{piece.corner ? piece.x0 : piece.x1,
                                                                             piece.corner ? piece.y0 : piece.y1}}) {
            l = std::min(l, static_cast<float>(x));
            t = std::min(t, static_cast<float>(y));
            r = std::max(r, static_cast<float>(x));
            b = std::max(b, static_cast<float>(y));
        }
        mesh.maxWidthHu = std::max(mesh.maxWidthHu, piece.widthHu);
    }
    mesh.huBounds = SkRect::MakeLTRB(l, t, r, b);
    std::vector<Vertex> vertices;
    for (std::size_t start = 0; start < pieces.size(); start += kPiecesPerChunk) {
        const std::size_t end = std::min(pieces.size(), start + kPiecesPerChunk);
        vertices.clear();
        vertices.reserve((end - start) * 4);
        for (std::size_t i = start; i < end; ++i) {
            const BorderPiece& piece = pieces[i];
            const float flags = (piece.corner ? 4.f : 0.f) + (piece.vInside > 0 ? 8.f : 0.f);
            for (int corner = 0; corner < 4; ++corner) {
                Vertex v{};
                v.seg[0] = static_cast<float>(piece.x0);
                v.seg[1] = static_cast<float>(piece.y0);
                v.seg[2] = static_cast<float>(piece.x1);
                v.seg[3] = static_cast<float>(piece.y1);
                v.style[0] = static_cast<float>(piece.widthHu);
                v.style[1] = static_cast<float>(piece.kind);
                v.style[2] = static_cast<float>(piece.inside);
                v.style[3] = static_cast<float>(corner) + flags;
                v.color[0] = piece.r;
                v.color[1] = piece.g;
                v.color[2] = piece.b;
                v.color[3] = 255;
                vertices.push_back(v);
            }
        }
        BorderMesh::Chunk chunk;
        chunk.vertexCount = vertices.size();
        chunk.indexCount = (end - start) * 6;
        chunk.vertices = SkMeshes::MakeVertexBuffer(vertices.data(), vertices.size() * sizeof(Vertex));
        chunk.indices = sharedIndices();
        if (!chunk.vertices || !chunk.indices) return {};
        mesh.bytes += vertices.size() * sizeof(Vertex);
        mesh.chunks.push_back(std::move(chunk));
    }
    return mesh;
}

void drawBorderMesh(SkCanvas* canvas, const BorderMesh& mesh, float bakeScale, float sx, float sy, SkPoint origin,
                    const SkRect& clip, sk_sp<SkColorFilter> filter) {
    if (!canvas || mesh.chunks.empty() || clip.isEmpty()) return;
    const sk_sp<SkMeshSpecification> spec = borderMeshSpec();
    if (!spec) return;
    // 구운 배율 그대로면 굵기·자리 배율이 모두 같아 CPU 비트맵과 같은 픽셀이다.
    const float scale[2] = {bakeScale * sx, bakeScale * sy};
    const float wscale = bakeScale * sx;
    const float at[2] = {origin.fX, origin.fY};
    std::vector<std::uint8_t> uniforms(spec->uniformSize(), 0);
    if (const auto* u = spec->findUniform("scale")) std::memcpy(uniforms.data() + u->offset, scale, sizeof(scale));
    if (const auto* u = spec->findUniform("wscale")) std::memcpy(uniforms.data() + u->offset, &wscale, sizeof(float));
    if (const auto* u = spec->findUniform("origin")) std::memcpy(uniforms.data() + u->offset, at, sizeof(at));
    const sk_sp<const SkData> data = SkData::MakeWithCopy(uniforms.data(), uniforms.size());
    // 정점이 실제로 닿는 화면 범위. 정점 셰이더가 선 굵기와 반올림만큼 사각을 넓힌다.
    const float reach = std::max(static_cast<float>(mesh.maxWidthHu) * wscale, 6.f) + 6.f;
    const SkRect bounds = SkRect::MakeLTRB(origin.fX + mesh.huBounds.fLeft * scale[0] - reach,
                                           origin.fY + mesh.huBounds.fTop * scale[1] - reach,
                                           origin.fX + mesh.huBounds.fRight * scale[0] + reach,
                                           origin.fY + mesh.huBounds.fBottom * scale[1] + reach);
    SkPaint paint;
    paint.setColorFilter(std::move(filter));
    canvas->save();
    canvas->clipRect(clip);
    for (const BorderMesh::Chunk& chunk : mesh.chunks) {
        auto made = SkMesh::MakeIndexed(spec, SkMesh::Mode::kTriangles, chunk.vertices, chunk.vertexCount, 0,
                                        chunk.indices, chunk.indexCount, 0, data, {}, bounds);
        if (!made.mesh.isValid()) continue;
        canvas->drawMesh(made.mesh, SkBlender::Mode(SkBlendMode::kDst), paint);
    }
    canvas->restore();
}

}  // namespace noting::render::paint_detail
#endif
