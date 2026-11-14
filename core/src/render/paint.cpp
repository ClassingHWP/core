#include "chwp/render/cache_budget.hpp"
#include "chwp/render/decorate.hpp"
#include "chwp/render/glyph_path.hpp"
#include "chwp/render/frame_cache.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/frame_phase.hpp"
#include "chwp/render/screen_ops.hpp"
#include "render/screen_op_canvas.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "miniz.h"
#include "chwp/common/unicode.hpp"
#include "chwp/reader.hpp"
#include "chwp/unit.hpp"
#include "chwp/render/scene.hpp"
#include "chwp/render/view.hpp"
#include "chwp/render/guides.hpp"
#include "render/equation_draw.hpp"
#include "render/line_breaker.hpp"
#include "render/page_texture_cache.hpp"
#include "render/paint_detail.hpp"
#include "render/painter.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
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
#include "include/gpu/graphite/PrecompileContext.h"
#include "include/gpu/graphite/precompile/PaintOptions.h"
#include "include/gpu/graphite/precompile/Precompile.h"
#include "include/gpu/graphite/precompile/PrecompileShader.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "src/gpu/graphite/RecorderPriv.h"
#include "src/gpu/graphite/RendererProvider.h"
#endif

namespace chwp::render {

using namespace paint_detail;

namespace paint_detail {



bool openFace(Face& face, int offset) {
    if (!face.bytes || face.bytes->empty()) return false;
    if (offset < 0) offset = 0;
    if (static_cast<std::size_t>(offset) >= face.bytes->size()) return false;
    if (stbtt_InitFont(&face.info, face.bytes->data(), offset) != 0) return true;
    // Wingdings 같은 기호 글꼴은 기호 표(3, 0)만 있어 stb 가 거절한다. 그 표를 글자 표로 쓴다.
    if (!face.info.head || !face.info.glyf || !face.info.loca) return false;
    stbtt_uint8* data = face.info.data;
    const stbtt_uint32 cmap = stbtt__find_table(data, static_cast<stbtt_uint32>(offset), "cmap");
    if (!cmap) return false;
    const int tables = ttUSHORT(data + cmap + 2);
    for (int i = 0; i < tables; ++i) {
        const stbtt_uint32 record = cmap + 4 + 8 * static_cast<stbtt_uint32>(i);
        if (ttUSHORT(data + record) != STBTT_PLATFORM_ID_MICROSOFT || ttUSHORT(data + record + 2) != STBTT_MS_EID_SYMBOL) continue;
        face.info.index_map = static_cast<int>(cmap + ttULONG(data + record + 4));
        face.info.indexToLocFormat = ttUSHORT(data + face.info.head + 50);
        return true;
    }
    return false;
}

// 연 글꼴은 프로세스에 둔다. 리스트 노드는 옮기지 않아서 stb 포인터가 유지된다.
std::list<Face> gOpenFaces;
std::unordered_map<std::string, Face*> gOpenByName;
std::shared_mutex gFaceMu;

// 접힌 글꼴을 다시 연다. gFaceMu 를 단독으로 잡고 부른다.
bool reopenFaceLocked(Face& face) {
    if (!face.ok) return false;
    if (!face.folded) return true;
    const std::string asked = face.fallbackOutline ? std::string("함초롬바탕") : (face.name ? *face.name : std::string());
    int offset = 0;
    if (asked.empty() || !layout::fontFaceBytes(asked, face.bytes, offset)) return false;
    face.info = stbtt_fontinfo{};
    if (!openFace(face, offset)) {
        face.bytes.reset();
        return false;
    }
    face.offset = offset;
    face.folded = false;
    return true;
}

Face* openCachedFaceBody(const std::string& key) {
    if (const auto found = gOpenByName.find(key); found != gOpenByName.end()) {
        Face* face = found->second;
        if (face->ok && face->folded && !reopenFaceLocked(*face)) return nullptr;
        return face->ok ? face : nullptr;
    }
    Face face;
    int offset = 0;
    const layout::FontLoadScope fontWhy("그리기", 0, &key, false);
    const bool have = layout::fontFaceBytes(key, face.bytes, offset);
    gOpenFaces.push_back(std::move(face));
    Face* stored = &gOpenFaces.back();
    stored->ok = have && openFace(*stored, offset);
    stored->offset = offset;
    if (!stored->ok && key != "함초롬바탕") {
        const layout::FontLoadStep ham("없는글꼴대체");
        (void)ham;
        Face alt;
        int altOffset = 0;
        if (layout::fontFaceBytes("함초롬바탕", alt.bytes, altOffset)) {
            *stored = std::move(alt);
            stored->ok = openFace(*stored, altOffset);
            stored->offset = altOffset;
            stored->fallbackOutline = stored->ok;
        }
    }
    const auto [it, inserted] = gOpenByName.emplace(key, stored);
    (void)inserted;
    stored->name = &it->first;
    return stored->ok ? stored : nullptr;
}

// 잊은 이름. 옛 Face 는 GlyphMem 이 계속 가리키므로 이름 글을 여기 옮겨 살려 둔다.
std::list<std::string> gRetiredFaceNames;

// 글꼴이 없어 대체 글꼴로 열었거나 열지 못한 이름을 잊는다. 그 글꼴이 나중에 들어오면 다시 연다.
// 글꼴이 없을 때 레이아웃은 그 이름에 대체 글꼴 바이트를 주므로, 지금 받는 바이트와 다르면 대체로 연 것이다.
// 옛 Face 는 리스트에 남겨 둔다(이미 구운 글자가 가리킨다). gFaceMu 를 단독으로 잡고 부른다.
void forgetMissingFacesLocked() {
    for (auto it = gOpenByName.begin(); it != gOpenByName.end();) {
        Face* face = it->second;
        // 지금 그 이름으로 받는 바이트가 열어 둔 것과 다를 때만 잊는다. 그대로 없는 글꼴은 남겨 둬서
        // 지울 때마다 새로 열어 쌓이지 않게 한다.
        bool stale = false;
        if (!face->folded) {
            std::shared_ptr<const std::vector<std::uint8_t>> now;
            int offset = 0;
            const bool have = layout::fontFaceBytes(it->first, now, offset);
            stale = face->ok ? (!have || now != face->bytes) : have;
        }
        if (!stale) {
            ++it;
            continue;
        }
        gRetiredFaceNames.push_back(it->first);
        face->name = &gRetiredFaceNames.back();
        it = gOpenByName.erase(it);
    }
}


std::list<GlyphMem> gGlyphMem;

void putBe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void pngChunk(std::vector<std::uint8_t>& png, const char type[4], const std::uint8_t* data, std::size_t size) {
    putBe32(png, static_cast<std::uint32_t>(size));
    const std::size_t mark = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data, data + size);
    const mz_ulong crc = mz_crc32(mz_crc32(0, nullptr, 0), png.data() + mark, png.size() - mark);
    putBe32(png, static_cast<std::uint32_t>(crc));
}

std::vector<std::uint8_t> encodePng(const Bitmap& bitmap) {
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(bitmap.height) * (1u + static_cast<std::size_t>(bitmap.width) * 4u));
    const std::size_t stride = 1u + static_cast<std::size_t>(bitmap.width) * 4u;
    for (int y = 0; y < bitmap.height; ++y) {
        raw[static_cast<std::size_t>(y) * stride] = 0;
        std::memcpy(raw.data() + static_cast<std::size_t>(y) * stride + 1,
                    bitmap.px.data() + static_cast<std::size_t>(y) * bitmap.width * 4,
                    static_cast<std::size_t>(bitmap.width) * 4u);
    }
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<std::uint8_t> idat(bound);
    mz_compress(idat.data(), &bound, raw.data(), static_cast<mz_ulong>(raw.size()));
    idat.resize(bound);

    std::uint8_t ihdr[13];
    auto be = [](std::uint8_t* p, std::uint32_t v) {
        p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
        p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        p[3] = static_cast<std::uint8_t>(v & 0xFF);
    };
    be(ihdr, static_cast<std::uint32_t>(bitmap.width));
    be(ihdr + 4, static_cast<std::uint32_t>(bitmap.height));
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;

    std::vector<std::uint8_t> png;
    const std::uint8_t sig[] = {137, 80, 78, 71, 13, 10, 26, 10};
    png.insert(png.end(), sig, sig + 8);
    pngChunk(png, "IHDR", ihdr, sizeof(ihdr));
    pngChunk(png, "IDAT", idat.data(), idat.size());
    pngChunk(png, "IEND", nullptr, 0);
    return png;
}

int clampi(int value, int lo, int hi) { return std::max(lo, std::min(value, hi)); }

void ensureBitmap(Bitmap& bitmap) {
    if (bitmap.width <= 0 || bitmap.height <= 0) return;
    const std::size_t need = static_cast<std::size_t>(bitmap.width) * static_cast<std::size_t>(bitmap.height) * 4u;
    if (bitmap.px.size() == need) return;
    bitmap.px.assign(need, 0);
}

void blend(Bitmap& bitmap, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (x < bitmap.clipL || y < bitmap.clipT || x >= bitmap.clipR || y >= bitmap.clipB || a == 0) return;
    if (x < 0 || y < 0 || x >= bitmap.width || y >= bitmap.height) return;
    ensureBitmap(bitmap);
    std::uint8_t* p = bitmap.px.data() + (static_cast<std::size_t>(y) * bitmap.width + x) * 4u;
    const int inv = 255 - a;
    p[0] = static_cast<std::uint8_t>((r * a + p[0] * inv) / 255);
    p[1] = static_cast<std::uint8_t>((g * a + p[1] * inv) / 255);
    p[2] = static_cast<std::uint8_t>((b * a + p[2] * inv) / 255);
    p[3] = 255;
}

// 그림 칸은 투명에서 시작한다. 알파를 255로 덮으면 옅은 가장자리가 검게 뭉친다.
void blendPicture(Bitmap& bitmap, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    if (x < bitmap.clipL || y < bitmap.clipT || x >= bitmap.clipR || y >= bitmap.clipB || a == 0) return;
    if (x < 0 || y < 0 || x >= bitmap.width || y >= bitmap.height) return;
    ensureBitmap(bitmap);
    std::uint8_t* p = bitmap.px.data() + (static_cast<std::size_t>(y) * bitmap.width + x) * 4u;
    const int inv = 255 - a;
    p[0] = static_cast<std::uint8_t>((r * a + p[0] * inv) / 255);
    p[1] = static_cast<std::uint8_t>((g * a + p[1] * inv) / 255);
    p[2] = static_cast<std::uint8_t>((b * a + p[2] * inv) / 255);
    p[3] = static_cast<std::uint8_t>((a * 255 + p[3] * inv) / 255);
}


bool visibleSpan(const Bitmap& bitmap, int dx, int dy, int dw, int dh, PixelSpan& out) {
    out.l = std::max(dx, std::max(bitmap.clipL, 0));
    out.t = std::max(dy, std::max(bitmap.clipT, 0));
    out.r = std::min(dx + dw, std::min(bitmap.clipR, bitmap.width));
    out.b = std::min(dy + dh, std::min(bitmap.clipB, bitmap.height));
    return out.l < out.r && out.t < out.b;
}

double catmullRomWeight(double x) {
    x = std::abs(x);
    if (x < 1.0) return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0) return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}




void buildResampleAxis(int visStart, int visEnd, int dstOrigin, int dstLen, bool flip, double srcStart, double srcEnd,
                       int srcLimit, std::vector<ResampleSpan>& spans, std::vector<ResampleTap>& taps, int& minNeeded,
                       int& maxNeeded, int& maxTapCount) {
    const int outCount = std::max(0, visEnd - visStart);
    spans.assign(static_cast<std::size_t>(outCount), {});
    taps.clear();
    maxTapCount = 0;
    if (outCount <= 0 || dstLen <= 0 || srcLimit <= 0 || srcEnd <= srcStart) {
        minNeeded = 0;
        maxNeeded = -1;
        return;
    }
    const double srcSpan = srcEnd - srcStart;
    const int minSrc = std::clamp(static_cast<int>(std::floor(srcStart + 1e-6)), 0, srcLimit - 1);
    const int maxSrc = std::clamp(static_cast<int>(std::ceil(srcEnd - 1e-6)) - 1, minSrc, srcLimit - 1);
    const double scale = srcSpan / static_cast<double>(dstLen);
    const double filterScale = std::max(1.0, scale);
    const double support = 2.0 * filterScale;
    const double coreRadius = std::max(1.0, filterScale * 1.05);
    taps.reserve(static_cast<std::size_t>(outCount) * static_cast<std::size_t>(std::ceil(support * 2.0) + 2));
    minNeeded = maxSrc;
    maxNeeded = minSrc;

    for (int pos = visStart; pos < visEnd; ++pos) {
        int idx = pos - dstOrigin;
        if (flip) idx = dstLen - 1 - idx;
        const double center = srcStart + (static_cast<double>(idx) + 0.5) * scale - 0.5;
        const int k0 = static_cast<int>(std::ceil(center - support));
        const int k1 = static_cast<int>(std::floor(center + support));
        const int tapStart = static_cast<int>(taps.size());
        double sumW = 0.0;
        for (int k = k0; k <= k1; ++k) {
            const double dist = std::abs(static_cast<double>(k) - center);
            const double w = catmullRomWeight(dist / filterScale);
            if (std::abs(w) < 1e-6) continue;
            const int clamped = std::clamp(k, minSrc, maxSrc);
            const bool isCore = dist <= coreRadius;
            minNeeded = std::min(minNeeded, clamped);
            maxNeeded = std::max(maxNeeded, clamped);
            if (static_cast<int>(taps.size()) > tapStart && taps.back().index == clamped) {
                taps.back().weight += static_cast<float>(w);
                taps.back().core = taps.back().core || isCore;
            } else {
                taps.push_back(ResampleTap{clamped, static_cast<float>(w), isCore});
            }
            sumW += w;
        }
        int tapCount = static_cast<int>(taps.size()) - tapStart;
        if (tapCount <= 0 || std::abs(sumW) < 1e-6) {
            taps.resize(static_cast<std::size_t>(tapStart));
            const int nearest = std::clamp(static_cast<int>(std::lround(center)), minSrc, maxSrc);
            minNeeded = std::min(minNeeded, nearest);
            maxNeeded = std::max(maxNeeded, nearest);
            taps.push_back(ResampleTap{nearest, 1.f, true});
            tapCount = 1;
        } else {
            const float invSum = static_cast<float>(1.0 / sumW);
            bool anyCore = false;
            for (int t = 0; t < tapCount; ++t) {
                ResampleTap& tap = taps[static_cast<std::size_t>(tapStart + t)];
                tap.weight *= invSum;
                if (tap.core) anyCore = true;
            }
            if (!anyCore) {
                for (int t = 0; t < tapCount; ++t) {
                    taps[static_cast<std::size_t>(tapStart + t)].core = true;
                }
            }
        }
        maxTapCount = std::max(maxTapCount, tapCount);
        spans[static_cast<std::size_t>(pos - visStart)] = ResampleSpan{tapStart, tapCount};
    }
}

// 목적지 픽셀이 덮는 원본 사각형을 평균한다. 투명도는 미리 곱한 색으로 모은 뒤 다시 나눈다.
void sampleArea(const std::uint8_t* src, int srcW, int srcH, double x0, double x1, double y0, double y1,
                std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a) {
    r = g = b = a = 0;
    if (!src || srcW <= 0 || srcH <= 0 || x1 <= x0 || y1 <= y0) return;
    const int xs = std::max(0, static_cast<int>(std::floor(x0)));
    const int xe = std::min(srcW, static_cast<int>(std::ceil(x1)));
    const int ys = std::max(0, static_cast<int>(std::floor(y0)));
    const int ye = std::min(srcH, static_cast<int>(std::ceil(y1)));
    double ar = 0;
    double ag = 0;
    double ab = 0;
    double aa = 0;
    double weight = 0;
    for (int sy = ys; sy < ye; ++sy) {
        const double covY = std::min(y1, sy + 1.0) - std::max(y0, static_cast<double>(sy));
        if (covY <= 0) continue;
        const std::uint8_t* row = src + (static_cast<std::size_t>(sy) * static_cast<std::size_t>(srcW)) * 4u;
        for (int sx = xs; sx < xe; ++sx) {
            const double covX = std::min(x1, sx + 1.0) - std::max(x0, static_cast<double>(sx));
            if (covX <= 0) continue;
            const double cov = covX * covY;
            const std::uint8_t* px = row + static_cast<std::size_t>(sx) * 4u;
            const double pa = px[3] / 255.0;
            ar += px[0] * pa * cov;
            ag += px[1] * pa * cov;
            ab += px[2] * pa * cov;
            aa += pa * cov;
            weight += cov;
        }
    }
    if (weight <= 0 || aa <= 0) return;
    const auto channel = [](double value) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(value)), 0, 255));
    };
    r = channel(ar / aa);
    g = channel(ag / aa);
    b = channel(ab / aa);
    a = channel(aa / weight * 255.0);
}

[[maybe_unused]] bool areaScaleRgba(const std::uint8_t* src, int srcW, int srcH, std::vector<std::uint8_t>& dst, int dstW, int dstH) {
    if (!src || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return false;
    if (static_cast<long long>(dstW) * dstH > 16000000) return false;
    dst.assign(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4u, 0);
    for (int y = 0; y < dstH; ++y) {
        const double y0 = static_cast<double>(y) * srcH / dstH;
        const double y1 = static_cast<double>(y + 1) * srcH / dstH;
        for (int x = 0; x < dstW; ++x) {
            const double x0 = static_cast<double>(x) * srcW / dstW;
            const double x1 = static_cast<double>(x + 1) * srcW / dstW;
            std::uint8_t r, g, b, a;
            sampleArea(src, srcW, srcH, x0, x1, y0, y1, r, g, b, a);
            std::uint8_t* out = dst.data() + (static_cast<std::size_t>(y) * dstW + x) * 4u;
            out[0] = r;
            out[1] = g;
            out[2] = b;
            out[3] = a;
        }
    }
    return true;
}

void fillRect(Bitmap& bitmap, int x, int y, int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const int x0 = clampi(std::max(x, bitmap.clipL), 0, bitmap.width);
    const int y0 = clampi(std::max(y, bitmap.clipT), 0, bitmap.height);
    const int x1 = clampi(std::min(x + w, bitmap.clipR), 0, bitmap.width);
    const int y1 = clampi(std::min(y + h, bitmap.clipB), 0, bitmap.height);
    if (x0 >= x1 || y0 >= y1) return;
    ensureBitmap(bitmap);
    for (int yy = y0; yy < y1; ++yy) {
        std::uint8_t* row = bitmap.px.data() + (static_cast<std::size_t>(yy) * bitmap.width + x0) * 4u;
        for (int xx = x0; xx < x1; ++xx) {
            row[0] = r;
            row[1] = g;
            row[2] = b;
            row[3] = 255;
            row += 4;
        }
    }
}

// 고정폭 빈칸. 10pt면 248이다.
int figureSlot(int size) {
    if (size <= 0) return 0;
    return 4 * ((size / 4) / 4);
}

// HWPX는 leader를 1, 2, 3처럼 숫자로 둔다. 2는 파선, 3은 점선이다.
std::string canonicalLeader(const std::string& raw) {
    bool digits = !raw.empty();
    int number = 0;
    for (unsigned char ch : raw) {
        if (ch < '0' || ch > '9') {
            digits = false;
            break;
        }
        number = number * 10 + (ch - '0');
    }
    if (digits) {
        static const char* names[] = {"NONE",
                                      "SOLID",
                                      "DASH",
                                      "DOT",
                                      "DASHDOT",
                                      "DASHDOTDOT",
                                      "LONGDASH",
                                      "CIRCLE",
                                      "DOUBLESLIM",
                                      "SLIMTHICK",
                                      "THICKSLIM",
                                      "SLIMTHICKSLIM",
                                      "WAVE",
                                      "DOUBLEWAVE",
                                      "THICK3D",
                                      "THICK3DREVERSELIGHTING",
                                      "3D",
                                      "3DREVERSELIGHTING"};
        if (number >= 0 && number < static_cast<int>(std::size(names))) return names[number];
        return "SOLID";
    }
    std::string type;
    for (unsigned char ch : raw) {
        if (ch == '_' || ch == '-' || ch == ' ') continue;
        if (ch >= 'a' && ch <= 'z') ch = static_cast<unsigned char>(ch - 'a' + 'A');
        type.push_back(static_cast<char>(ch));
    }
    return type;
}

// 탭 채움. x,w는 양쪽 고정폭 빈칸을 뺀 구간이고, y는 글자 상자의 세로 중앙이다.
// 칸의 1/3이 미리보기에서 1픽셀이다. 파선은 5칸 그리고 2칸 비운다. 긴 파선과 쇄선의 긴 획은 10칸이다.
void fillLeader(Bitmap& bitmap, const std::string& raw, int x, int baseline, int w, int slot, std::uint8_t r,
                std::uint8_t g, std::uint8_t b) {
    const std::string type = canonicalLeader(raw);
    if (type.empty() || type == "NONE" || type == "DOT" || w <= 0 || slot <= 0) return;
    const int step = std::max(1, slot / 3);
    auto solid = [&](int sx, int sy, int sw, int sh) {
        if (sw > 0 && sh > 0) fillRect(bitmap, sx, sy, sw, sh, r, g, b);
    };
    auto rule = [&](int yTop, int thick, const int* pattern, int cycle) {
        if (thick <= 0) return;
        if (!pattern || cycle <= 0) {
            solid(x, yTop, w, thick);
            return;
        }
        int cursor = 0;
        int index = 0;
        while (cursor < w) {
            const int n = pattern[index % cycle];
            if (n <= 0) break;
            const int draw = std::min(n, w - cursor);
            if (index % 2 == 0) solid(x + cursor, yTop, draw, thick);
            cursor += draw;
            ++index;
        }
    };
    auto stack = [&](const int* thicks, const int* gaps, int count, const int* pattern, int cycle) {
        int total = 0;
        for (int i = 0; i < count; ++i) total += thicks[i];
        for (int i = 0; i + 1 < count; ++i) total += gaps[i];
        int y = baseline - total / 2;
        for (int i = 0; i < count; ++i) {
            rule(y, thicks[i], pattern, cycle);
            y += thicks[i];
            if (i + 1 < count) y += gaps[i];
        }
    };
    const int hair[] = {step};
    if (type == "DASH") {
        const int pattern[] = {5 * step, 2 * step};
        stack(hair, nullptr, 1, pattern, 2);
    } else if (type == "LONGDASH") {
        const int pattern[] = {10 * step, 2 * step};
        stack(hair, nullptr, 1, pattern, 2);
    } else if (type == "DASHDOT") {
        const int pattern[] = {10 * step, 2 * step, step, 2 * step};
        stack(hair, nullptr, 1, pattern, 4);
    } else if (type == "DASHDOTDOT") {
        const int pattern[] = {10 * step, 2 * step, step, 2 * step, step, 2 * step};
        stack(hair, nullptr, 1, pattern, 6);
    } else if (type == "CIRCLE") {
        const int rad = std::max(1, step);
        for (int p = rad; p + rad <= w; p += slot) {
            for (int yy = -rad; yy <= rad; ++yy) {
                for (int xx = -rad; xx <= rad; ++xx) {
                    if (xx * xx + yy * yy <= rad * rad) solid(x + p + xx, baseline + yy, 1, 1);
                }
            }
        }
    } else if (type == "DOUBLESLIM") {
        const int thicks[] = {step, step};
        const int gaps[] = {2 * step};
        stack(thicks, gaps, 2, nullptr, 0);
    } else if (type == "SLIMTHICK") {
        const int thicks[] = {step, 3 * step};
        const int gaps[] = {step};
        stack(thicks, gaps, 2, nullptr, 0);
    } else if (type == "THICKSLIM") {
        const int thicks[] = {3 * step, step};
        const int gaps[] = {step};
        stack(thicks, gaps, 2, nullptr, 0);
    } else if (type == "SLIMTHICKSLIM") {
        const int thicks[] = {step, 2 * step, step};
        const int gaps[] = {step, step};
        stack(thicks, gaps, 3, nullptr, 0);
    } else if (type == "WAVE" || type == "DOUBLEWAVE") {
        const int thicks[] = {step, step};
        const int gaps[] = {2 * step};
        stack(thicks, gaps, 2, nullptr, 0);
    } else if (type == "THICK3D" || type == "THICK3DREVERSELIGHTING" || type == "3D" || type == "3DREVERSELIGHTING") {
        return;
    } else {
        stack(hair, nullptr, 1, nullptr, 0);
    }
}


void strokeRect(Bitmap& bitmap, int x, int y, int w, int h, int thick, std::uint8_t r, std::uint8_t g,
                std::uint8_t b) {
    thick = std::max(1, thick);
    const int out = thick / 2;
    const int left = x - out;
    const int top = y - out;
    const int spanW = w + thick;
    const int spanH = h + thick;
    const ClipSuspend open(bitmap);
    fillRect(bitmap, left, top, spanW, thick, r, g, b);
    fillRect(bitmap, left, top + spanH - thick, spanW, thick, r, g, b);
    fillRect(bitmap, left, top, thick, spanH, r, g, b);
    fillRect(bitmap, left + spanW - thick, top, thick, spanH, r, g, b);
}

void parseHex(const std::string& text, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    r = g = b = 0x11;
    if (text.size() >= 7 && text[0] == '#') {
        r = static_cast<std::uint8_t>((hex(text[1]) << 4) | hex(text[2]));
        g = static_cast<std::uint8_t>((hex(text[3]) << 4) | hex(text[4]));
        b = static_cast<std::uint8_t>((hex(text[5]) << 4) | hex(text[6]));
    }
}

bool nextCp(std::string_view text, std::size_t& index, char32_t& out) {
    if (index >= text.size()) return false;
    const auto c = static_cast<unsigned char>(text[index]);
    if (c < 0x80) {
        out = c;
        index += 1;
        return true;
    }
    if ((c & 0xE0) == 0xC0 && index + 1 < text.size()) {
        out = ((c & 0x1F) << 6) | (text[index + 1] & 0x3F);
        index += 2;
        return true;
    }
    if ((c & 0xF0) == 0xE0 && index + 2 < text.size()) {
        out = ((c & 0x0F) << 12) | ((text[index + 1] & 0x3F) << 6) | (text[index + 2] & 0x3F);
        index += 3;
        return true;
    }
    if ((c & 0xF8) == 0xF0 && index + 3 < text.size()) {
        out = ((c & 0x07) << 18) | ((static_cast<unsigned char>(text[index + 1]) & 0x3F) << 12) |
              ((static_cast<unsigned char>(text[index + 2]) & 0x3F) << 6) |
              (static_cast<unsigned char>(text[index + 3]) & 0x3F);
        index += 4;
        return true;
    }
    out = c;
    index += 1;
    return true;
}



std::vector<GlyphCommand> commandsFromGlyph(const stbtt_fontinfo* info, int glyph) {
    std::vector<GlyphCommand> commands;
    if (!info || glyph <= 0) return commands;
    stbtt_vertex* verts = nullptr;
    const int count = stbtt_GetGlyphShape(info, glyph, &verts);
    if (count <= 0 || !verts) return commands;
    const float scale = stbtt_ScaleForMappingEmToPixels(info, 1000.f);
    const auto unit = [&](int v) { return static_cast<int>(static_cast<float>(v) * scale); };
    for (int i = 0; i < count; ++i) {
        const stbtt_vertex& v = verts[i];
        const GlyphPoint to{unit(v.x), unit(v.y)};
        if (v.type == STBTT_vmove) commands.push_back({GlyphVerb::Move, to, {}, {}});
        else if (v.type == STBTT_vline) commands.push_back({GlyphVerb::Line, to, {}, {}});
        else if (v.type == STBTT_vcurve)
            commands.push_back({GlyphVerb::Quad, to, {unit(v.cx), unit(v.cy)}, {}});
        else if (v.type == STBTT_vcubic)
            commands.push_back({GlyphVerb::Cubic, to, {unit(v.cx), unit(v.cy)}, {unit(v.cx1), unit(v.cy1)}});
    }
    stbtt_FreeShape(info, verts);
    return commands;
}

struct FtSlot {
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    FT_Face face = nullptr;
};

std::mutex gFtMutex;
FT_Library gFt = nullptr;
std::unordered_map<std::string, FtSlot> gFtFaces;

int ftCollectionIndex(const std::uint8_t* bytes, std::size_t size, int fontOffset) {
    if (fontOffset <= 0 || size < 12 || std::memcmp(bytes, "ttcf", 4) != 0) return 0;
    const auto be32 = [](const std::uint8_t* p) {
        return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
               (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    };
    const std::uint32_t count = be32(bytes + 8);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (12u + (i + 1u) * 4u > size) break;
        if (be32(bytes + 12 + i * 4) == static_cast<std::uint32_t>(fontOffset)) return static_cast<int>(i);
    }
    return 0;
}

int ftToEm(FT_Pos value, int upem) {
    if (upem <= 0) upem = 1000;
    const long long scaled = static_cast<long long>(value) * 1000;
    const long long half = upem / 2;
    const long long rounded = scaled >= 0 ? (scaled + half) / upem : (scaled - half) / upem;
    return static_cast<int>(rounded);
}

struct FtSink {
    std::vector<GlyphCommand>* commands = nullptr;
    int upem = 1000;
};

extern "C" int chwpFtMove(const FT_Vector* to, void* user) {
    auto* sink = static_cast<FtSink*>(user);
    sink->commands->push_back(
        {GlyphVerb::Move, {ftToEm(to->x, sink->upem), ftToEm(to->y, sink->upem)}, {}, {}});
    return 0;
}

extern "C" int chwpFtLine(const FT_Vector* to, void* user) {
    auto* sink = static_cast<FtSink*>(user);
    sink->commands->push_back(
        {GlyphVerb::Line, {ftToEm(to->x, sink->upem), ftToEm(to->y, sink->upem)}, {}, {}});
    return 0;
}

extern "C" int chwpFtConic(const FT_Vector* control, const FT_Vector* to, void* user) {
    auto* sink = static_cast<FtSink*>(user);
    sink->commands->push_back({GlyphVerb::Quad,
                               {ftToEm(to->x, sink->upem), ftToEm(to->y, sink->upem)},
                               {ftToEm(control->x, sink->upem), ftToEm(control->y, sink->upem)},
                               {}});
    return 0;
}

extern "C" int chwpFtCubic(const FT_Vector* control1, const FT_Vector* control2, const FT_Vector* to, void* user) {
    auto* sink = static_cast<FtSink*>(user);
    sink->commands->push_back({GlyphVerb::Cubic,
                               {ftToEm(to->x, sink->upem), ftToEm(to->y, sink->upem)},
                               {ftToEm(control1->x, sink->upem), ftToEm(control1->y, sink->upem)},
                               {ftToEm(control2->x, sink->upem), ftToEm(control2->y, sink->upem)}});
    return 0;
}

bool ftDecompose(FtSlot& slot, char32_t cp, std::vector<GlyphCommand>& commands) {
    if (!slot.face) return false;
    const FT_UInt glyph = FT_Get_Char_Index(slot.face, static_cast<FT_ULong>(cp));
    if (glyph == 0) return false;
    if (FT_Load_Glyph(slot.face, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return false;
    if (slot.face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;
    FtSink sink{&commands, static_cast<int>(slot.face->units_per_EM)};
    FT_Outline_Funcs funcs{};
    funcs.move_to = chwpFtMove;
    funcs.line_to = chwpFtLine;
    funcs.conic_to = chwpFtConic;
    funcs.cubic_to = chwpFtCubic;
    if (FT_Outline_Decompose(&slot.face->glyph->outline, &funcs, &sink) != 0) {
        commands.clear();
        return false;
    }
    return !commands.empty();
}

bool ftCommands(const std::string& face, char32_t cp, bool bold, bool designedBold,
                std::vector<GlyphCommand>& commands) {
    if (face.empty()) return false;
    std::string slotKey = face;
    slotKey.push_back(bold && designedBold ? 'B' : 'R');
    {
        std::lock_guard lock(gFtMutex);
        if (const auto found = gFtFaces.find(slotKey); found != gFtFaces.end())
            return ftDecompose(found->second, cp, commands);
    }
    bool fileBold = false;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    int fontOffset = 0;
    if (!layout::fontOutlineBytes(face, bold && designedBold, bytes, fontOffset, fileBold) || !bytes || bytes->empty())
        return false;
    std::lock_guard lock(gFtMutex);
    if (!gFt && FT_Init_FreeType(&gFt) != 0) return false;
    if (const auto found = gFtFaces.find(slotKey); found != gFtFaces.end())
        return ftDecompose(found->second, cp, commands);
    const auto [it, inserted] = gFtFaces.emplace(slotKey, FtSlot{});
    (void)inserted;
    it->second.bytes = std::move(bytes);
    const int index = ftCollectionIndex(it->second.bytes->data(), it->second.bytes->size(), fontOffset);
    if (FT_New_Memory_Face(gFt, it->second.bytes->data(), static_cast<FT_Long>(it->second.bytes->size()), index,
                           &it->second.face) != 0) {
        gFtFaces.erase(it);
        return false;
    }
    return ftDecompose(it->second, cp, commands);
}

bool faceGlyphCommands(const std::string& face, char32_t cp, std::vector<GlyphCommand>& commands) {
    commands.clear();
    return ftCommands(face, cp, false, false, commands);
}

struct OutlineKey {
    const Face* face = nullptr;
    int index = 0;
    bool bold = false;
    bool italic = false;
    int ppem = 0;
    bool operator==(const OutlineKey& other) const {
        return face == other.face && index == other.index && bold == other.bold && italic == other.italic &&
               ppem == other.ppem;
    }
};

bool operator<(const OutlineKey& left, const OutlineKey& right) {
    if (left.face != right.face) return left.face < right.face;
    if (left.index != right.index) return left.index < right.index;
    if (left.bold != right.bold) return !left.bold && right.bold;
    if (left.italic != right.italic) return !left.italic && right.italic;
    return left.ppem < right.ppem;
}

std::map<OutlineKey, std::vector<GlyphCommand>> gCommands;
std::unordered_map<std::string, bool> gBoldFace;
std::mutex gBoldMu;
std::shared_mutex gOutlineMu;

const std::string& glyphFaceName(const GlyphMem* mem) {
    static const std::string fallback = "함초롬바탕";
    if (mem && mem->face && mem->face->name && !mem->face->name->empty()) return *mem->face->name;
    return fallback;
}

bool designedBoldFace(const std::string& face, bool bold) {
    if (!bold) return false;
    std::lock_guard lock(gBoldMu);
    if (const auto known = gBoldFace.find(face); known != gBoldFace.end()) return known->second;
    const bool designed = layout::fontDesignedBold(face);
    gBoldFace.emplace(face, designed);
    return designed;
}

// 기울임·합성 굵기를 입히기 전 윤곽(1000 em). 글꼴 파일에서 한 번만 뽑고, 글꼴을 접어도 여기서 스타일을 입힌다.
struct BaseOutlineKey {
    const Face* face = nullptr;
    char32_t cp = 0;
    int index = 0;
    bool heavy = false;  // 굵은 파일에서 뽑은 윤곽
    bool operator<(const BaseOutlineKey& other) const {
        if (face != other.face) return face < other.face;
        if (cp != other.cp) return cp < other.cp;
        if (index != other.index) return index < other.index;
        return !heavy && other.heavy;
    }
};

std::map<BaseOutlineKey, std::vector<GlyphCommand>> gBaseOutlines;

std::vector<GlyphCommand> baseGlyphCommands(const GlyphMem* mem, bool bold, bool designedBold) {
    const BaseOutlineKey key{mem->face, mem->cp, mem->index, bold && designedBold};
    {
        std::shared_lock lock(gOutlineMu);
        if (const auto found = gBaseOutlines.find(key); found != gBaseOutlines.end()) return found->second;
    }
    std::vector<GlyphCommand> commands;
    {
        // 뽑는 동안 글꼴이 접히지 않게 잡는다. 접혀 있으면 먼저 다시 연다.
        std::shared_lock faceLock(gFaceMu);
        if (mem->face->folded) {
            faceLock.unlock();
            {
                std::unique_lock reopen(gFaceMu);
                reopenFaceLocked(*mem->face);
            }
            faceLock.lock();
        }
        const std::string& face = glyphFaceName(mem);
        if (!ftCommands(face, mem->cp, bold, designedBold, commands) && !mem->face->folded) {
            commands = commandsFromGlyph(&mem->face->info, mem->index);
        }
    }
    std::unique_lock lock(gOutlineMu);
    const auto [it, _] = gBaseOutlines.emplace(key, std::move(commands));
    return it->second;
}

std::vector<GlyphCommand> styledGlyphCommands(const GlyphMem* mem, bool bold, bool italic, int ppem) {
    std::vector<GlyphCommand> commands;
    if (!mem || !mem->face) return commands;
    const std::string& face = glyphFaceName(mem);
    const bool designedBold = designedBoldFace(face, bold);
    const int syntheticPpem = bold && !designedBold && ppem > 0 ? ppem : 0;
    return styleGlyphCommands(baseGlyphCommands(mem, bold, designedBold), italic, syntheticPpem);
}

const std::vector<GlyphCommand>& hangulCommands(const GlyphMem* mem, bool bold, bool italic, int ppem) {
    static const std::vector<GlyphCommand> empty;
    if (!mem || !mem->face) return empty;
    const int syntheticPpem = bold && !designedBoldFace(glyphFaceName(mem), bold) && ppem > 0 ? ppem : 0;
    const OutlineKey key{mem->face, mem->index, bold, italic, syntheticPpem};
    {
        std::shared_lock lock(gOutlineMu);
        if (const auto found = gCommands.find(key); found != gCommands.end()) return found->second;
    }
    // 캐시에 넣는 외곽선은 한글이 곡선을 펴는 방식의 직선이다. 기울임과 굵기는 펴기 전에 반영한다.
    std::vector<GlyphCommand> commands = flattenHangulCommands(styledGlyphCommands(mem, bold, italic, ppem));
    std::unique_lock lock(gOutlineMu);
    if (const auto found = gCommands.find(key); found != gCommands.end()) return found->second;
    const auto [it, _] = gCommands.emplace(key, std::move(commands));
    return it->second;
}

void clearHangulCommands() {
    std::unique_lock lock(gOutlineMu);
    gCommands.clear();
}

std::map<OutlineKey, std::vector<GlyphCommand>> gCurves;

// GPU 경로는 곡선을 펴지 않는다. 키는 글꼴, 글리프, 굵기, 기울임, 합성 굵기 픽셀 크기다.
const std::vector<GlyphCommand>& hangulCurves(const GlyphMem* mem, bool bold, bool italic, int ppem) {
    static const std::vector<GlyphCommand> empty;
    if (!mem || !mem->face) return empty;
    const int syntheticPpem = bold && !designedBoldFace(glyphFaceName(mem), bold) && ppem > 0 ? ppem : 0;
    const OutlineKey key{mem->face, mem->index, bold, italic, syntheticPpem};
    {
        std::shared_lock lock(gOutlineMu);
        if (const auto found = gCurves.find(key); found != gCurves.end()) return found->second;
    }
    std::vector<GlyphCommand> commands = styledGlyphCommands(mem, bold, italic, ppem);
    std::unique_lock lock(gOutlineMu);
    if (const auto found = gCurves.find(key); found != gCurves.end()) return found->second;
    const auto [it, _] = gCurves.emplace(key, std::move(commands));
    return it->second;
}

void clearHangulCurves() {
    std::unique_lock lock(gOutlineMu);
    gCurves.clear();
}

SkPath glyphPath(const std::vector<GlyphCommand>& commands, float emPx, int ratio, float penX, float baseY, int fitX,
                 int fitY) {
    const float unit = emPx / 1000.f;
    const float sy = unit * static_cast<float>(std::max(1, fitY)) / 1000.f;
    const float sx = unit * static_cast<float>(std::max(1, ratio)) / 100.f * static_cast<float>(std::max(1, fitX)) / 1000.f;
    SkPathBuilder path;
    for (const GlyphCommand& command : commands) {
        const float x = penX + static_cast<float>(command.to.x) * sx;
        const float y = baseY - static_cast<float>(command.to.y) * sy;
        if (command.verb == GlyphVerb::Move) path.moveTo(x, y);
        else if (command.verb == GlyphVerb::Line) path.lineTo(x, y);
        else if (command.verb == GlyphVerb::Quad) {
            path.quadTo(penX + static_cast<float>(command.c1.x) * sx, baseY - static_cast<float>(command.c1.y) * sy, x,
                        y);
        } else if (command.verb == GlyphVerb::Cubic) {
            path.cubicTo(penX + static_cast<float>(command.c1.x) * sx, baseY - static_cast<float>(command.c1.y) * sy,
                         penX + static_cast<float>(command.c2.x) * sx, baseY - static_cast<float>(command.c2.y) * sy, x,
                         y);
        }
    }
    return path.detach();
}

std::vector<std::uint8_t> glyphAlpha(const std::vector<GlyphCommand>& commands, float emPx, float penX, float baseY,
                                     int width, int height) {
    Painter painter;
    painter.bitmap.width = width;
    painter.bitmap.height = height;
    painter.bitmap.clipR = width;
    painter.bitmap.clipB = height;
    // 흰 바탕에 검게 칠해 어두워진 만큼을 알파로 읽는다.
    painter.bitmap.px.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 255);
    painter.paintGlyph(commands, emPx, 100, 1000, 1000, penX, baseY, 0, 0, 0);
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t i = 0; i < alpha.size(); ++i) {
        alpha[i] = static_cast<std::uint8_t>(255 - painter.bitmap.px[i * 4u]);
    }
    return alpha;
}

// 칸 선은 경계 한가운데라 바깥 절반이 표 상자 밖에 있다. 픽셀로 반올림해도 잘리지 않게 선 굵기만큼 여유를 둔다.
std::int32_t tableBorderBleed(const Node& node) {
    std::int32_t bleed = 0;
    auto take = [&](const BorderEdge& edge) {
        if (edge.type.empty() || edge.type == "NONE") return;
        const std::int32_t width = edge.width > 0 ? edge.width : 28;
        if (width > bleed) bleed = width;
    };
    for (const Node& child : node.children) {
        if (child.kind != NodeKind::Cell) continue;
        take(child.ext().borderLeft);
        take(child.ext().borderRight);
        take(child.ext().borderTop);
        take(child.ext().borderBottom);
    }
    return bleed;
}

// Graphite 가 파이프라인을 만들 때마다 여기에 적는다. 컴파일 스레드에서도 부를 수 있다.
struct PipelineTallyState {
    std::mutex mu;
    PipelineTally tally;
    // 미리 만든(또는 미리 그려 만든) 파이프라인 이름.
    std::unordered_set<std::string> prepared;
};

PipelineTallyState& pipelineTallyState() {
    static PipelineTallyState state;
    return state;
}

// 미리 만들기가 직접 그려서 만드는 동안(경로·글자 쿼드·표 테두리) 켠다. 그때 만든 것도 미리 만든 것이다.
std::atomic<bool> gWarmingPipelines{false};

// Graphite 가 파이프라인을 만들거나(made) 캐시에서 찾을(found) 때 부른다. 미리 만들지 않은 것을 쓰면 적는다.
void notePipelineUse(const std::string& label, bool made, bool precompiled) {
    PipelineTallyState& state = pipelineTallyState();
    std::lock_guard lock(state.mu);
    if (precompiled || (made && gWarmingPipelines.load(std::memory_order_relaxed))) {
        if (made && state.prepared.insert(label).second) ++state.tally.precompiled;
        return;
    }
    if (state.prepared.count(label)) return;
    if (std::find(state.tally.drawnFirst.begin(), state.tally.drawnFirst.end(), label) != state.tally.drawnFirst.end()) {
        return;
    }
    state.tally.drawnFirst.push_back(label);
    if (made) std::fprintf(stderr, "[ClassingHWP] 그리다 만든 파이프라인 %s\n", label.c_str());
}

// 첫 화면이 쓰는 그리기 조합. 장치가 붙으면 문서를 기다리지 않고 이 표대로 파이프라인을 만든다.
// GPU 셰이더 컴파일은 처음 한 번 수백 ms 걸리고, 그동안 화면 내보내기가 기다린다.
// 표에 없는 조합은 그릴 때 만든다(pipelineTally().drawnFirst 에 남는다).
enum class PipeTarget : std::uint8_t {
    Screen,      // 화면 표면. 경로가 없는 프레임(깊이만, 표본 1)
    ScreenPath,  // 화면 표면. 경로를 그리는 프레임(깊이·스텐실, 표본 4)
    Offscreen,   // 화면 밖 RGBA8 그림판(칸 텍스처, 쪽 그림)
};
// 경로·글자 쿼드·표 테두리는 그리는 방식(경로 나누기, 정점 꼴, 메시 셰이더)이 여럿이라 Precompile 로 적으면
// 쓰지 않는 조합까지 수십 개를 만든다. 이 셋은 화면과 같은 그리기 함수로 작은 그림판에 한 번 그린다.
enum class PipeShape : std::uint8_t {
    FillRect,
    RRect,
    EdgeAAQuad,
    RulerPath,   // 눈금자 숫자 윤곽(안티에일리어싱 경로)
    GlyphQuads,  // 글자 마스크 쿼드(drawVertices)
    BorderMesh,  // 표 테두리 메시
};
enum class PipeInk : std::uint8_t {
    Solid,       // 단색
    Image,       // RGBA8 그림, 하드웨어 자르기
    ClampImage,  // RGBA8 그림, 셰이더 자르기
    CubicImage,  // RGBA8 그림, 3차 보간(1:1 이 아닌 그림: 고배율 화면, 쪽 그림)
    AlphaImage,  // A8 글자 마스크를 칠 색으로 물들인 것
};
enum class PipeBlend : std::uint8_t { Src, SrcOver };

struct PipeCombo {
    PipeTarget target;
    PipeShape shape;
    PipeInk ink;
    PipeBlend blend;
};

// 첫 화면이 쓰는 조합. 순서가 컴파일 순서다. 칸을 합치는 화면 밖 그림판부터 두고,
// 1:1 이 아닐 때만 쓰는 3차 보간 그림은 맨 뒤에 둔다.
constexpr PipeCombo kFirstScreenPipes[] = {
    {PipeTarget::Offscreen, PipeShape::FillRect, PipeInk::Image, PipeBlend::Src},
    {PipeTarget::Offscreen, PipeShape::FillRect, PipeInk::ClampImage, PipeBlend::Src},
    {PipeTarget::Offscreen, PipeShape::FillRect, PipeInk::Image, PipeBlend::SrcOver},
    {PipeTarget::Offscreen, PipeShape::GlyphQuads, PipeInk::AlphaImage, PipeBlend::SrcOver},
    {PipeTarget::Offscreen, PipeShape::EdgeAAQuad, PipeInk::Image, PipeBlend::SrcOver},
    {PipeTarget::Offscreen, PipeShape::BorderMesh, PipeInk::Solid, PipeBlend::SrcOver},
    {PipeTarget::Screen, PipeShape::FillRect, PipeInk::Solid, PipeBlend::Src},
    {PipeTarget::Screen, PipeShape::FillRect, PipeInk::Image, PipeBlend::SrcOver},
    {PipeTarget::Screen, PipeShape::RRect, PipeInk::Solid, PipeBlend::SrcOver},
    {PipeTarget::ScreenPath, PipeShape::FillRect, PipeInk::Solid, PipeBlend::Src},
    {PipeTarget::ScreenPath, PipeShape::FillRect, PipeInk::Image, PipeBlend::SrcOver},
    {PipeTarget::ScreenPath, PipeShape::RRect, PipeInk::Solid, PipeBlend::SrcOver},
    {PipeTarget::ScreenPath, PipeShape::RulerPath, PipeInk::Solid, PipeBlend::SrcOver},
    {PipeTarget::Offscreen, PipeShape::FillRect, PipeInk::CubicImage, PipeBlend::Src},
    {PipeTarget::Offscreen, PipeShape::FillRect, PipeInk::CubicImage, PipeBlend::SrcOver},
    {PipeTarget::Offscreen, PipeShape::EdgeAAQuad, PipeInk::CubicImage, PipeBlend::SrcOver},
};

// 그려서 만드는 모양인가. 나머지는 Precompile 로 만든다.
constexpr bool drawnToWarm(PipeShape shape) {
    return shape == PipeShape::RulerPath || shape == PipeShape::GlyphQuads || shape == PipeShape::BorderMesh;
}

#if CHWP_HAS_GPU
// GPU 장치는 Graphite 에 넘기는 WebGPU 장치 하나다. 웹은 브라우저 WebGPU, 네이티브는 Dawn(D3D12·Metal·Vulkan)이 준다.
// 플랫폼마다 다른 것은 아래 gpu_platform 의 호출뿐이고, 장치를 고르고 Graphite 를 세우는 차례는 같다.
// 글자 진행 폭은 여기 없다. 줄 비트맵은 조판 advance로 이미 그려져 있다.
struct GpuSlot {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Surface surface;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    int surfaceW = 0;
    int surfaceH = 0;
    std::unique_ptr<skgpu::graphite::Context> context;
    std::unique_ptr<skgpu::graphite::Recorder> recorder;
    bool tried = false;
    bool ready = false;
    bool failed = false;
    bool raster = false;
    // 경로 테셀레이션이 될 때만 글자 제어점을 GPU에 올린다. 아니면 줄 비트맵이다.
    bool vectors = false;
};

GpuSlot& gpuSlot() {
    static GpuSlot slot;
    return slot;
}

namespace gpu_platform {

#if defined(__EMSCRIPTEN__)
// 화면은 캔버스 표면에 그린다. 표면이 없으면 GPU 화면을 쓰지 않는다.
constexpr bool kScreenNeedsSurface = true;

bool available() {
    EM_ASM({
        if (typeof specialHTMLTargets !== "undefined" && Module["canvas"]) {
            specialHTMLTargets["!canvas"] = Module["canvas"];
        }
    });
    return EM_ASM_INT({
               return (typeof navigator !== "undefined" && !!navigator.gpu &&
                       typeof navigator.gpu.requestAdapter === "function")
                          ? 1
                          : 0;
           }) != 0;
}

wgpu::Instance makeInstance() { return wgpu::CreateInstance(nullptr); }

wgpu::Surface makeSurface(const wgpu::Instance& instance) {
    wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasDesc;
    canvasDesc.selector = "!canvas";
    wgpu::SurfaceDescriptor surfaceDesc;
    surfaceDesc.nextInChain = &canvasDesc;
    return instance.CreateSurface(&surfaceDesc);
}

// 2D 폴백 그림판(캔버스 2d)이 있는가.
bool rasterTarget() {
    return EM_ASM_INT({
               const canvas = Module["canvas"];
               if (!canvas || typeof canvas.getContext !== "function") return 0;
               let ctx = canvas._chwp2dCtx;
               if (!ctx) {
                   try {
                       ctx = canvas.getContext("2d");
                   } catch (e) {
                       ctx = null;
                   }
                   if (ctx) canvas._chwp2dCtx = ctx;
               }
               return ctx ? 1 : 0;
           }) != 0;
}

// 브라우저가 콜백을 스스로 부른다.
void pump(const wgpu::Instance&) {}
#else
// 창 표면은 셸이 창을 줄 때 붙인다. 창이 없어도(시험·agent) 장치만으로 화면 밖 표면에 그린다.
constexpr bool kScreenNeedsSurface = false;

bool available() { return true; }

wgpu::Instance makeInstance() {
    static const bool procsSet = [] {
        static const DawnProcTable procs = dawn::native::GetProcs();
        dawnProcSetProcs(&procs);
        return true;
    }();
    (void)procsSet;
    static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor desc{};
    desc.requiredFeatureCount = 1;
    desc.requiredFeatures = &kTimedWaitAny;
    return wgpu::CreateInstance(&desc);
}

wgpu::Surface makeSurface(const wgpu::Instance&) { return nullptr; }

// 2D 화면 그림 목록은 셸이 받는다(screenOpSink). 받을 곳은 presentScreen 이 본다.
bool rasterTarget() { return true; }

// Dawn 네이티브는 이벤트를 돌려야 콜백이 온다.
void pump(const wgpu::Instance& instance) { instance.ProcessEvents(); }
#endif

}  // namespace gpu_platform

bool enableRasterFallback(GpuSlot& slot, const char* reason) {
    slot.recorder.reset();
    slot.context.reset();
    slot.surface = nullptr;
    slot.queue = nullptr;
    slot.device = nullptr;
    slot.adapter = nullptr;
    slot.instance = nullptr;
    slot.format = wgpu::TextureFormat::Undefined;
    slot.surfaceW = 0;
    slot.surfaceH = 0;
    slot.vectors = false;
    slot.failed = true;
    noteScreenFailed(true);
    slot.raster = gpu_platform::rasterTarget();
    slot.ready = slot.raster;
    std::fprintf(stderr, "[ClassingHWP] %s -> 2d fallback %s\n", reason ? reason : "Skia Dawn off",
                 slot.raster ? "on" : "off");
    return slot.raster;
}

// 그려서 만드는 모양(drawnToWarm)을 화면과 같은 칠로 작은 그림판에 그린다. 내용은 상관없고 조합만 같으면 된다.
void warmByDrawing(GpuSlot& slot, PipeShape shape, SkColorType target) {
    sk_sp<SkSurface> surface =
        SkSurfaces::RenderTarget(slot.recorder.get(), SkImageInfo::Make(32, 32, target, kPremul_SkAlphaType));
    if (!surface) return;
    SkCanvas* canvas = surface->getCanvas();
    switch (shape) {
        case PipeShape::RulerPath: {
            // drawRulerNumber 와 같은 칠: 안티에일리어싱, 불투명 단색. 숫자 0 처럼 곡선 윤곽 두 개.
            SkPathBuilder path;
            path.moveTo(16, 4).quadTo(26, 4, 26, 16).quadTo(26, 28, 16, 28).quadTo(6, 28, 6, 16).quadTo(6, 4, 16, 4).close();
            path.moveTo(16, 8).quadTo(10, 8, 10, 16).quadTo(10, 24, 16, 24).quadTo(22, 24, 22, 16).quadTo(22, 8, 16, 8).close();
            SkPaint ink;
            ink.setAntiAlias(true);
            ink.setColor(SkColorSetARGB(255, 92, 98, 108));
            canvas->drawPath(path.detach(), ink);
            break;
        }
        case PipeShape::GlyphQuads: {
            // 글자 아틀라스와 같은 A8 그림을 칠 셰이더로 두고, 쿼드 정점 색과 DstIn 으로 섞는다.
            std::array<std::uint8_t, 16> alpha{};
            alpha.fill(255);
            const SkPixmap pixmap(SkImageInfo::Make(4, 4, kAlpha_8_SkColorType, kPremul_SkAlphaType), alpha.data(), 4);
            const sk_sp<SkImage> raster = SkImages::RasterFromPixmapCopy(pixmap);
            const sk_sp<SkImage> atlas = raster ? SkImages::TextureFromImage(slot.recorder.get(), raster.get()) : nullptr;
            if (!atlas) return;
            SkPaint paint;
            paint.setShader(atlas->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, SkSamplingOptions()));
            SkVertices::Builder builder(SkVertices::kTriangles_VertexMode, 4, 6,
                                        SkVertices::kHasTexCoords_BuilderFlag | SkVertices::kHasColors_BuilderFlag);
            if (!builder.isValid()) return;
            const SkPoint corners[4] = {{0, 0}, {4, 0}, {4, 4}, {0, 4}};
            const std::uint16_t order[6] = {0, 1, 2, 0, 2, 3};
            for (int i = 0; i < 4; ++i) {
                builder.positions()[i] = corners[i];
                builder.texCoords()[i] = corners[i];
                builder.colors()[i] = SK_ColorBLACK;
            }
            std::copy(std::begin(order), std::end(order), builder.indices());
            canvas->drawVertices(builder.detach().get(), SkBlendMode::kDstIn, paint);
            break;
        }
        case PipeShape::BorderMesh: {
            std::vector<paint_detail::BorderPiece> pieces(1);
            pieces[0].x1 = 100;
            pieces[0].widthHu = 10;
            const paint_detail::BorderMesh mesh = paint_detail::makeBorderMesh(pieces);
            if (mesh.chunks.empty()) return;
            paint_detail::drawBorderMesh(canvas, mesh, 1.f, 1.f, 1.f, SkPoint::Make(0, 0), SkRect::MakeWH(32, 32));
            break;
        }
        default: break;
    }
}

// kFirstScreenPipes 를 Graphite 미리 만들기로 옮긴다. 무엇을 만들지는 표가 정한다.
// 화면 표면의 색 형식은 장치가 준 값(slot.format)이다.
void precompileFirstScreen(GpuSlot& slot) {
    namespace gr = skgpu::graphite;
    const auto started = std::chrono::steady_clock::now();
    std::unique_ptr<gr::PrecompileContext> precompile = slot.context->makePrecompileContext();
    if (!precompile) return;
    const SkColorType screenType =
        slot.format == wgpu::TextureFormat::BGRA8Unorm ? kBGRA_8888_SkColorType : kRGBA_8888_SkColorType;
    const gr::RenderPassProperties screen{gr::DepthStencilFlags::kDepth, screenType, nullptr, false};
    const gr::RenderPassProperties screenPath{gr::DepthStencilFlags::kDepthStencil, screenType, nullptr, true};
    const gr::RenderPassProperties offscreen{gr::DepthStencilFlags::kDepth, kRGBA_8888_SkColorType, nullptr, false};
    const SkColorInfo premul(kRGBA_8888_SkColorType, kPremul_SkAlphaType, nullptr);
    const SkTileMode clamp = SkTileMode::kClamp;
    gWarmingPipelines.store(true, std::memory_order_relaxed);
    bool drew = false;
    // 화면 표면이 없으면(시험·agent 의 쪽 그림만) 화면 표면 조합은 쓰지 않는다.
    const bool hasScreen = static_cast<bool>(slot.surface);
    for (const PipeCombo& combo : kFirstScreenPipes) {
        if (combo.target != PipeTarget::Offscreen && !hasScreen) continue;
        if (drawnToWarm(combo.shape)) {
            warmByDrawing(slot, combo.shape, combo.target == PipeTarget::Offscreen ? kRGBA_8888_SkColorType : screenType);
            drew = true;
            continue;
        }
        const gr::RenderPassProperties& pass = combo.target == PipeTarget::Screen       ? screen
                                               : combo.target == PipeTarget::ScreenPath ? screenPath
                                                                                        : offscreen;
        gr::PaintOptions paint;
        const SkBlendMode mode = combo.blend == PipeBlend::Src ? SkBlendMode::kSrc : SkBlendMode::kSrcOver;
        paint.setBlendModes({&mode, 1});
        if (combo.ink == PipeInk::Image || combo.ink == PipeInk::ClampImage || combo.ink == PipeInk::CubicImage) {
            // 자르기 방식을 주지 않으면 하드웨어 자르기 하나만, 주면 그 방식과 하드웨어 자르기를 만든다.
            // 3차 보간을 켜면 하드웨어 자르기와 3차 보간을 만든다. 이미 만든 것은 다시 만들지 않는다.
            const sk_sp<gr::PrecompileShader> image = gr::PrecompileShaders::Image(
                combo.ink == PipeInk::CubicImage ? gr::PrecompileShaders::ImageShaderFlags::kCubicSampling
                                                 : gr::PrecompileShaders::ImageShaderFlags::kNone,
                {&premul, 1},
                combo.ink == PipeInk::ClampImage ? SkSpan<const SkTileMode>(&clamp, 1) : SkSpan<const SkTileMode>());
            paint.setShaders({&image, 1});
        }
        gr::DrawTypeFlags draw = gr::DrawTypeFlags::kNonAAFillRect;
        if (combo.shape == PipeShape::RRect) draw = gr::DrawTypeFlags::kAnalyticRRect;
        if (combo.shape == PipeShape::EdgeAAQuad) draw = gr::DrawTypeFlags::kPerEdgeAAQuad;
        gr::Precompile(precompile.get(), paint, draw, {&pass, 1});
    }
    if (drew) {
        std::unique_ptr<gr::Recording> recording = slot.recorder->snap();
        if (recording) {
            gr::InsertRecordingInfo info;
            info.fRecording = recording.get();
            if (slot.context->insertRecording(info) == gr::InsertStatus::kSuccess) slot.context->submit(gr::SyncToCpu::kNo);
        }
    }
    gWarmingPipelines.store(false, std::memory_order_relaxed);
    std::fprintf(stderr, "[ClassingHWP] 첫 화면 파이프라인 %d 개를 미리 만들었다 (%.0fms)\n", pipelineTally().precompiled,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
}

void finishDawnDevice(wgpu::Device device) {
    GpuSlot& slot = gpuSlot();
    if (!device) {
        enableRasterFallback(slot, "Skia Dawn device off");
        return;
    }
    slot.device = std::move(device);
    slot.queue = slot.device.GetQueue();
    skgpu::graphite::DawnBackendContext backend;
    backend.fInstance = slot.instance;
    backend.fDevice = slot.device;
    backend.fQueue = slot.queue;
    skgpu::graphite::ContextOptions options;
    options.fPipelineCachingCallback = [](void*, skgpu::graphite::ContextOptions::PipelineCacheOp op,
                                          const std::string& label, uint32_t, bool fromPrecompile, sk_sp<SkData>) {
        notePipelineUse(label, op == skgpu::graphite::ContextOptions::PipelineCacheOp::kAddingPipeline,
                        fromPrecompile);
    };
    slot.context = skgpu::graphite::ContextFactory::MakeDawn(backend, options);
    if (!slot.context) {
        enableRasterFallback(slot, "Skia Dawn off");
        return;
    }
    slot.recorder = slot.context->makeRecorder();
    if (!slot.recorder) {
        enableRasterFallback(slot, "Skia Dawn recorder off");
        return;
    }
    slot.surface = gpu_platform::makeSurface(slot.instance);
    if (slot.surface) {
        wgpu::SurfaceCapabilities caps;
        slot.surface.GetCapabilities(slot.adapter, &caps);
        slot.format = caps.formatCount > 0 ? caps.formats[0] : wgpu::TextureFormat::BGRA8Unorm;
    } else if (gpu_platform::kScreenNeedsSurface) {
        enableRasterFallback(slot, "Skia Dawn surface off");
        return;
    }
    slot.raster = false;
    slot.vectors = true;
    slot.ready = true;
    std::fprintf(stderr, "[ClassingHWP] Skia Dawn glyph atlas\n");
    precompileFirstScreen(slot);
}

bool beginDawnSurface() {
    GpuSlot& slot = gpuSlot();
    if (slot.tried) return slot.ready || !slot.failed;
    slot.tried = true;
    if (!gpu_platform::available()) {
        return enableRasterFallback(slot, "Skia Dawn navigator.gpu off");
    }
    slot.instance = gpu_platform::makeInstance();
    if (!slot.instance) {
        return enableRasterFallback(slot, "Skia Dawn instance off");
    }
    wgpu::RequestAdapterOptions adapterOptions;
    adapterOptions.powerPreference = wgpu::PowerPreference::HighPerformance;
    slot.instance.RequestAdapter(
        &adapterOptions, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView message) {
            GpuSlot& slot = gpuSlot();
            if (status != wgpu::RequestAdapterStatus::Success || !adapter) {
                std::fprintf(stderr, "[ClassingHWP] Skia Dawn adapter off (status=%d, msg=%.*s)\n",
                             static_cast<int>(status), static_cast<int>(message.length),
                             message.data ? message.data : "");
                enableRasterFallback(slot, "Skia Dawn adapter off");
                return;
            }
            // Null 은 그리지 않는 장치다(Vulkan 드라이버를 못 연 리눅스 등). GPU 가 없는 것으로 보고 CPU 로 그린다.
            wgpu::AdapterInfo adapterInfo;
            adapter.GetInfo(&adapterInfo);
            if (adapterInfo.backendType == wgpu::BackendType::Null) {
                enableRasterFallback(slot, "Skia Dawn null adapter");
                return;
            }
            slot.adapter = std::move(adapter);
            wgpu::DeviceDescriptor deviceDesc;
            deviceDesc.SetUncapturedErrorCallback(
                [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView errMsg) {
                    std::fprintf(stderr, "[ClassingHWP] WebGPU uncaptured error (type=%d): %.*s\n",
                                 static_cast<int>(type), static_cast<int>(errMsg.length),
                                 errMsg.data ? errMsg.data : "");
                });
            deviceDesc.SetDeviceLostCallback(
                wgpu::CallbackMode::AllowSpontaneous,
                [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView lostMsg) {
                    std::fprintf(stderr, "[ClassingHWP] WebGPU device lost (reason=%d): %.*s\n",
                                 static_cast<int>(reason), static_cast<int>(lostMsg.length),
                                 lostMsg.data ? lostMsg.data : "");
                });
            slot.adapter.RequestDevice(
                &deviceDesc, wgpu::CallbackMode::AllowSpontaneous,
                [](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView devMsg) {
                    if (status != wgpu::RequestDeviceStatus::Success) {
                        std::fprintf(stderr, "[ClassingHWP] Skia Dawn device off (status=%d, msg=%.*s)\n",
                                     static_cast<int>(status), static_cast<int>(devMsg.length),
                                     devMsg.data ? devMsg.data : "");
                        enableRasterFallback(gpuSlot(), "Skia Dawn device off");
                        return;
                    }
                    finishDawnDevice(std::move(device));
                });
        });
    // 네이티브는 콜백이 끝날 때까지 이벤트를 돌린다. 웹은 브라우저가 나중에 부르므로 곧바로 돌아간다.
    for (int spin = 0; spin < 2000 && !slot.ready && !slot.failed && !gpu_platform::kScreenNeedsSurface; ++spin) {
        gpu_platform::pump(slot.instance);
    }
    return true;
}

void clearDawnSlot() {
    GpuSlot& slot = gpuSlot();
    slot.recorder.reset();
    slot.context.reset();
    slot.surface = nullptr;
    slot.queue = nullptr;
    slot.device = nullptr;
    slot.adapter = nullptr;
    slot.instance = nullptr;
    slot.format = wgpu::TextureFormat::Undefined;
    slot.surfaceW = 0;
    slot.surfaceH = 0;
    slot.tried = false;
    slot.ready = false;
    slot.failed = false;
    slot.raster = false;
    slot.vectors = false;
    noteScreenFailed(false);
}
#endif

// 화면 사각형. 그리는 쪽은 SkRect 로 옮겨 쓴다.
struct GlyphRect {
    float l = 0.f;
    float t = 0.f;
    float r = 0.f;
    float b = 0.f;
    static GlyphRect xywh(float x, float y, float w, float h) { return GlyphRect{x, y, x + w, y + h}; }
    float width() const { return r - l; }
    float height() const { return b - t; }
    bool empty() const { return !(l < r && t < b); }
    bool intersects(float left, float top, float right, float bottom) const {
        return !empty() && l < right && left < r && t < bottom && top < b;
    }
    void join(const GlyphRect& other) {
        if (other.empty()) return;
        if (empty()) {
            *this = other;
            return;
        }
        l = std::min(l, other.l);
        t = std::min(t, other.t);
        r = std::max(r, other.r);
        b = std::max(b, other.b);
    }
};

struct SpriteMaskState {
    int zoom = 0;
    int spritePercent = 0;
    std::uint32_t gen = 0;
};

struct LocalGlyphQuad {
    float relX = 0.f;
    float relY = 0.f;
    float scale = 1.f;
    float rawPenX = 0.f;
    float rawPenY = 0.f;
    float basePpem = 1.f;
    float invDrawnPpem = 1.f;
    float bearingX = 0.f;
    float bearingY = 0.f;
    int drawnPpem = 0;
    int unclampedWant = 0;
    GlyphRect tex;
    std::uint32_t color = 0;
    int atlasPage = 0;
    std::uint64_t seq = 0;
    std::uint32_t glyphIndex = 0;
    bool stretched = false;
    bool turn = false;  // 세로쓰기에서 눕힌 글자. 펜을 축으로 시계 방향 90° 돌려 찍는다
    float penRelX = 0.f;
    float penRelY = 0.f;
};

struct CachedSpriteQuads {
    int w = 0;
    int h = 0;
    int sw = 0;
    int sh = 0;
    int percent = 0;
    int effLayout = 0;
    int effMask = 0;
    std::uint32_t maskGen = 0;
    std::uint32_t builtSeq = 0;
    float minBasePpem = 1000.f;
    bool allExactMask = false;
    bool hasAnyDot = false;
    std::vector<LocalGlyphQuad> quads;
};

void clearScreenGlyphs();

std::unordered_map<int, FrameGlyphs>& frameGlyphs() {
    static std::unordered_map<int, FrameGlyphs> glyphs;
    return glyphs;
}

std::unordered_map<int, SpriteMaskState>& spriteMaskReady() {
    static std::unordered_map<int, SpriteMaskState> ready;
    return ready;
}

std::unordered_map<int, CachedSpriteQuads>& cachedSpriteQuads() {
    static std::unordered_map<int, CachedSpriteQuads> cache;
    return cache;
}

std::uint32_t& maskCacheGen() {
    static std::uint32_t gen = 1;
    return gen;
}

std::uint32_t& maskBuiltSeq() {
    static std::uint32_t seq = 1;
    return seq;
}

std::mutex& frameGlyphMutex() {
    static std::mutex mu;
    return mu;
}

namespace {
// 화면이 글자·그림·테두리를 칸 비트맵 대신 직접 그리는가. 셸이 Host 로 켜고, GPU 를 못 쓰게 되면 끈다.
std::atomic<bool> gScreenVectors{false};
std::atomic<bool> gScreenFailed{false};
}  // namespace

bool screenVectors() { return gScreenVectors.load() && !gScreenFailed.load(); }

ScreenOpSink& screenOpSink() {
    static ScreenOpSink sink;
    return sink;
}
void enableScreenVectors(bool on) { gScreenVectors.store(on); }
void noteScreenFailed(bool failed) { gScreenFailed.store(failed); }

void storeFrameGlyphs(int id, FrameGlyphs glyphs) {
    std::lock_guard lock(frameGlyphMutex());
    spriteMaskReady().erase(id);
    cachedSpriteQuads().erase(id);
    if (id == 0 || glyphs.glyphs.empty()) {
        frameGlyphs().erase(id);
        return;
    }
    frameGlyphs()[id] = std::move(glyphs);
}

void dropFrameGlyphs(int id) {
    std::lock_guard lock(frameGlyphMutex());
    spriteMaskReady().erase(id);
    cachedSpriteQuads().erase(id);
    frameGlyphs().erase(id);
}

void clearFrameGlyphs() {
    {
        std::lock_guard lock(frameGlyphMutex());
        spriteMaskReady().clear();
        cachedSpriteQuads().clear();
        frameGlyphs().clear();
    }
    clearFramePictures();
    clearFrameBorders();
}


std::string linePlaceKey(const FrameDesc& frame) {
    std::string key = std::to_string(frame.page) + "/" + std::to_string(frame.kind);
    for (const int index : frame.path) key += "." + std::to_string(index);
    return key;
}

// 한 줄 칸에 적어 둔 글자 자리. 글자 offset 은 lineStart 에서 시작한 줄의 문단 안 위치다.
// 칸 키와 내용 해시 두 표가 같은 자리를 함께 가리킨다.
struct LinePlaces {
    int lineStart = -1;
    std::vector<GlyphSpot> spots;
};
using SharedLinePlaces = std::shared_ptr<const LinePlaces>;

std::unordered_map<std::uint64_t, SharedLinePlaces>& linePlacesByHash() {
    static std::unordered_map<std::uint64_t, SharedLinePlaces> byHash;
    return byHash;
}

std::unordered_set<std::string>& linePlaceSeen() {
    static std::unordered_set<std::string> seen;
    return seen;
}

std::unordered_map<std::string, SharedLinePlaces>& linePlaces() {
    static std::unordered_map<std::string, SharedLinePlaces> places;
    return places;
}

std::unordered_map<int, std::unordered_set<std::string>>& linePlacePages() {
    static std::unordered_map<int, std::unordered_set<std::string>> pages;
    return pages;
}



std::unordered_map<std::string, std::unordered_map<char32_t, GlyphMem*>>& resolvedFaces() {
    static std::unordered_map<std::string, std::unordered_map<char32_t, GlyphMem*>> faces;
    return faces;
}

int fitPermille(std::int64_t numer, std::int64_t denom) {
    if (numer <= 0 || denom <= 0) return 1000;
    std::int64_t value = (numer * 1000 + denom / 2) / denom;
    if (value < 50) value = 50;
    if (value > 8000) value = 8000;
    return static_cast<int>(value);
}

void fallbackFit(const std::string& asked, char32_t cp, int& fitX, int& fitY) {
    fitX = 1000;
    fitY = 1000;
    if (asked.empty() || asked == "함초롬바탕") return;
    const int askedBase = layout::fontBaseline(asked, 1000000);
    const int hamBase = layout::fontBaseline("함초롬바탕", 1000000);
    if (askedBase > 0 && hamBase > 0) fitY = fitPermille(askedBase, hamBase);
    const int askedAdv = layout::fontGlyphDesignAdvance(asked, cp);
    const int askedUpm = layout::fontUnitsPerEm(asked);
    const int hamAdv = layout::fontGlyphDesignAdvance("함초롬바탕", cp);
    const int hamUpm = layout::fontUnitsPerEm("함초롬바탕");
    if (askedAdv > 0 && askedUpm > 0 && hamAdv > 0 && hamUpm > 0) {
        fitX = fitPermille(static_cast<std::int64_t>(askedAdv) * hamUpm, static_cast<std::int64_t>(hamAdv) * askedUpm);
    }
}

GlyphMem* glyphMemory(const std::string& font, char32_t cp) {
    {
        std::shared_lock lock(gFaceMu);
        const auto table = resolvedFaces().find(font);
        if (table != resolvedFaces().end()) {
            const auto cached = table->second.find(cp);
            if (cached != table->second.end()) return cached->second;
        }
    }
    std::unique_lock lock(gFaceMu);
    auto& known = resolvedFaces()[font];
    if (const auto cached = known.find(cp); cached != known.end()) return cached->second;
    auto has = [&font](Face* face, char32_t code, int& index) {
        if (face && face->folded) {
            // 접은 글꼴을 그리기가 다시 연다. 미리 뽑지 못한 글자라 어떤 글자인지 남긴다.
            std::printf("{\"글꼴다시열기그리기\":\"%s\",\"코드\":%u}\n", font.c_str(), static_cast<unsigned>(code));
            std::fflush(stdout);
        }
        if (!face || !reopenFaceLocked(*face)) return false;
        index = stbtt_FindGlyphIndex(&face->info, static_cast<int>(code));
        return index != 0;
    };
    const layout::FontLoadScope fontWhy("그리기", cp, &font);
    const std::string resolved = layout::fontWithGlyph(font, cp);
    const std::string& asked = resolved.empty() ? font : resolved;
    Face* face = openCachedFaceBody(asked.empty() ? std::string("함초롬바탕") : asked);
    int index = 0;
    const bool haveGlyph = has(face, cp, index);
    const bool substituted = face && face->fallbackOutline;
    int fitX = 1000;
    int fitY = 1000;
    if (substituted || (!haveGlyph && asked != "함초롬바탕")) {
        const layout::FontLoadStep ham("없는글꼴대체");
        (void)ham;
        Face* alt = openCachedFaceBody("함초롬바탕");
        int altIndex = 0;
        if (has(alt, cp, altIndex)) {
            face = alt;
            index = altIndex;
            fallbackFit(asked, cp, fitX, fitY);
        }
    }
    if (!face) {
        known.emplace(cp, nullptr);
        return nullptr;
    }
    float advance1000 = 0.f;
    if (reopenFaceLocked(*face)) {
        int adv = 0;
        int lsb = 0;
        stbtt_GetGlyphHMetrics(&face->info, index, &adv, &lsb);
        advance1000 = static_cast<float>(adv) * stbtt_ScaleForMappingEmToPixels(&face->info, 1000.f);
    }
    gGlyphMem.push_back(GlyphMem{face, index, cp, fitX, fitY, advance1000});
    GlyphMem* mem = &gGlyphMem.back();
    known.emplace(cp, mem);
    return mem;
}

// 글자 모양과 코드의 전진폭. 런을 한 번 훑을 때 채우고, 보이는 줄은 이 표만 읽는다.
struct AdvanceKey {
    std::uint32_t shape = 0;
    char32_t cp = 0;
    bool operator==(const AdvanceKey& other) const { return shape == other.shape && cp == other.cp; }
};

struct AdvanceKeyHash {
    std::size_t operator()(const AdvanceKey& key) const noexcept {
        return (static_cast<std::size_t>(key.shape) * 1315423911u) ^ static_cast<std::size_t>(key.cp);
    }
};

struct AdvanceRec {
    std::int32_t width = 0;
    std::int32_t ink = 0;
};

std::shared_mutex gAdvanceMu;

std::unordered_map<AdvanceKey, AdvanceRec, AdvanceKeyHash>& textAdvances() {
    static std::unordered_map<AdvanceKey, AdvanceRec, AdvanceKeyHash> table;
    return table;
}

bool takeTextAdvance(std::uint32_t shape, char32_t cp, std::int32_t& width, std::int32_t& ink) {
    std::shared_lock lock(gAdvanceMu);
    const auto found = textAdvances().find(AdvanceKey{shape, cp});
    if (found == textAdvances().end()) return false;
    width = found->second.width;
    ink = found->second.ink;
    return true;
}

void keepTextAdvance(std::uint32_t shape, char32_t cp, std::int32_t width, std::int32_t ink) {
    std::unique_lock lock(gAdvanceMu);
    textAdvances().insert_or_assign(AdvanceKey{shape, cp}, AdvanceRec{width, ink});
}

struct MaskJob {
    std::string font;
    char32_t cp = 0;
    bool bold = false;
    bool italic = false;
    int ratio = 100;
    int ppem = 1;
    int zoom = 100;
    float exactPpem = 0.f;
};

std::mutex& glyphAtlasMutex() {
    static std::mutex mu;
    return mu;
}

std::vector<MaskJob>& maskJobs() {
    static std::vector<MaskJob> jobs;
    return jobs;
}

std::size_t& baseMaskJobCount() {
    static std::size_t count = 0;
    return count;
}

int& maskZoomJobTarget() {
    static int target = 0;
    return target;
}

std::size_t& maskZoomJobCursor() {
    static std::size_t cursor = 0;
    return cursor;
}

bool& documentGlyphsNoted() {
    static bool noted = false;
    return noted;
}

std::size_t& maskJobCursor() {
    static std::size_t cursor = 0;
    return cursor;
}

void clearDocumentGlyphs() {
    {
        std::unique_lock lock(gAdvanceMu);
        textAdvances().clear();
    }
    std::lock_guard atlasLock(glyphAtlasMutex());
    maskJobs().clear();
    baseMaskJobCount() = 0;
    maskZoomJobTarget() = 0;
    maskZoomJobCursor() = 0;
    maskJobCursor() = 0;
    documentGlyphsNoted() = false;
}

// 줄 캐시로 가른 글자 폭. 쪽을 쌓을 때는 비어 있고, 처음 그릴 때 재서 여기 남긴다.
std::shared_mutex gSpanMu;

void measureText(const model::Document* doc, std::string_view text, std::uint32_t charShapeId, int size, int ratio,
                 std::vector<std::int32_t>& advances, std::vector<std::int32_t>& inks) {
    layout::retainGlyphScaleCache();
    const model::CharShape* shape = nullptr;
    if (charShapeId < doc->header.charShapes.size() && doc->header.charShapes[charShapeId].id == charShapeId) {
        shape = &doc->header.charShapes[charShapeId];
    } else {
        for (const auto& item : doc->header.charShapes) {
            if (item.id == charShapeId) {
                shape = &item;
                break;
            }
        }
    }
    model::CharShape fallback;
    fallback.height = size;
    fallback.ratio.fill(ratio);
    const model::CharShape* measure = shape ? shape : &fallback;
    std::size_t scan = 0;
    char32_t code = 0;
    while (nextCp(text, scan, code)) {
        std::int32_t width = 0;
        std::int32_t ink = 0;
        if (takeTextAdvance(charShapeId, code, width, ink)) {
            advances.push_back(width);
            inks.push_back(ink);
            continue;
        }
        const bool hangul = code >= 0xAC00 && code <= 0xD7A3;
        const model::Lang lang = hangul ? model::Lang::Hangul : model::Lang::Latin;
        const std::int32_t advance = chwp::layout::glyphAdvance(code, *measure, lang, doc);
        const std::int32_t trail = chwp::layout::glyphTrail(code, *measure, lang, doc);
        width = advance > 0 ? advance : 0;
        ink = std::max<std::int32_t>(0, width - trail);
        keepTextAdvance(charShapeId, code, width, ink);
        advances.push_back(width);
        inks.push_back(ink);
    }
}

std::shared_mutex gPlaceMu;

struct IdleLineNote {
    std::vector<char> done;
    bool told = false;
    int recorded = 0;
};

IdleLineNote& idleLineNote() {
    static IdleLineNote note;
    return note;
}





std::unordered_map<const Node*, LineBlockCacheEntry>& lineBlockCache() {
    static std::unordered_map<const Node*, LineBlockCacheEntry> cache;
    return cache;
}

void clearLinePlaces() {
    std::scoped_lock lock(gPlaceMu, gSpanMu, gFaceMu);
    linePlaceSeen().clear();
    linePlaces().clear();
    linePlacesByHash().clear();
    linePlacePages().clear();
    lineBlockCache().clear();
    resolvedFaces().clear();
    layout::clearGlyphScaleCache();
    idleLineNote() = {};
}

void evictNodeMeasuresUnlocked(const Node& node) {
    if (node.kind == NodeKind::Line) lineBlockCache().erase(&node);
    for (const Node& child : node.children) {
        evictNodeMeasuresUnlocked(child);
    }
}

bool parseLinePlaceKey(const std::string& key, int& outPage, int& outKind, std::vector<int>& outPath) {
    outPath.clear();
    const std::size_t slash = key.find('/');
    if (slash == std::string::npos || slash == 0) return false;
    try {
        outPage = std::stoi(key.substr(0, slash));
    } catch (...) {
        return false;
    }
    const std::size_t firstDot = key.find('.', slash + 1);
    const std::string kindStr =
        firstDot == std::string::npos ? key.substr(slash + 1) : key.substr(slash + 1, firstDot - (slash + 1));
    try {
        outKind = std::stoi(kindStr);
    } catch (...) {
        return false;
    }
    if (firstDot == std::string::npos) return true;
    std::size_t pos = firstDot + 1;
    while (pos <= key.size()) {
        const std::size_t nextDot = key.find('.', pos);
        const std::string part = nextDot == std::string::npos ? key.substr(pos) : key.substr(pos, nextDot - pos);
        if (!part.empty()) {
            try {
                outPath.push_back(std::stoi(part));
            } catch (...) {
                return false;
            }
        }
        if (nextDot == std::string::npos) break;
        pos = nextDot + 1;
    }
    return true;
}

bool matchesInvalidationPath(int page, int kind, const std::vector<int>& path, const FrameInvalidation& inv) {
    (void)kind;
    if (inv.page >= 0 && page != inv.page) return false;
    if (path.size() < inv.pathPrefix.size()) return false;
    for (std::size_t i = 0; i < inv.pathPrefix.size(); ++i) {
        if (path[i] != inv.pathPrefix[i]) return false;
    }
    if (inv.selfOnly) {
        return path.size() == inv.pathPrefix.size();
    }
    if (inv.minChild >= 0) {
        if (path.size() <= inv.pathPrefix.size()) return false;
        const int childIdx = path[inv.pathPrefix.size()];
        if (childIdx < inv.minChild) return false;
        if (inv.maxChild >= 0 && childIdx > inv.maxChild) return false;
    }
    return true;
}

bool matchesAnyInvalidationKey(const std::string& key, const std::vector<FrameInvalidation>& invalidations) {
    int page = -1;
    int kind = 0;
    std::vector<int> path;
    if (!parseLinePlaceKey(key, page, kind, path)) return false;
    for (const FrameInvalidation& inv : invalidations) {
        if (matchesInvalidationPath(page, kind, path, inv)) return true;
    }
    return false;
}

// 이 칸에 적어 둔 글자 자리를 찾는다. 칸 키로 적었으면 비어 있어도 찾은 것이다.
bool findLinePlaces(const FrameDesc& frame, SharedLinePlaces& out) {
    if (frame.kind != 0) return false;
    const std::string key = linePlaceKey(frame);
    std::shared_lock lock(gPlaceMu);
    if (linePlaceSeen().count(key)) {
        const auto found = linePlaces().find(key);
        out = found == linePlaces().end() ? nullptr : found->second;
        return true;
    }
    if (frame.contentHash != 0) {
        const auto byHash = linePlacesByHash().find(frame.contentHash);
        if (byHash != linePlacesByHash().end()) {
            out = byHash->second;
            return true;
        }
    }
    return false;
}

// 적어 둔 자리를 지금 칸의 줄로 옮긴다. 글자가 같은 줄이 문단·시작 위치만 바뀌어도 블록 판단이 맞게 한다.
bool takeLinePlaces(const FrameDesc& frame, std::vector<GlyphSpot>& out) {
    SharedLinePlaces places;
    if (!findLinePlaces(frame, places)) return false;
    out = places ? places->spots : std::vector<GlyphSpot>{};
    const int deltaOff =
        (places && frame.lineStart >= 0 && places->lineStart >= 0) ? (frame.lineStart - places->lineStart) : 0;
    for (GlyphSpot& spot : out) {
        if (frame.lineStory >= 0) spot.story = frame.lineStory;
        if (frame.lineParagraph >= 0) spot.paragraph = frame.lineParagraph;
        if (deltaOff != 0 && spot.offset >= 0) spot.offset += deltaOff;
    }
    return true;
}

void keepLinePlaces(const FrameDesc& frame, std::vector<GlyphSpot> spots) {
    if (frame.kind != 0) return;
    const std::string key = linePlaceKey(frame);
    const bool empty = spots.empty();
    const SharedLinePlaces places = std::make_shared<const LinePlaces>(LinePlaces{frame.lineStart, std::move(spots)});
    std::unique_lock lock(gPlaceMu);
    linePlaceSeen().insert(key);
    if (empty) linePlaces().erase(key);
    else linePlaces()[key] = places;
    int placePage = -1;
    int placeKind = 0;
    std::vector<int> placePath;
    if (parseLinePlaceKey(key, placePage, placeKind, placePath)) linePlacePages()[placePage].insert(key);
    if (frame.contentHash != 0) {
        if (linePlacesByHash().size() > 32768) linePlacesByHash().clear();
        linePlacesByHash()[frame.contentHash] = places;
    }
}

// 걸을 때 좌표는 프레임 왼쪽 위보다 여백만큼 안쪽이다. 캐시에는 프레임 왼쪽 위 기준 상대 HU 좌표만 남긴다.
void spotsToPage(std::vector<GlyphSpot>& spots, const FrameDesc&, std::int32_t padHu) {
    for (GlyphSpot& spot : spots) {
        spot.huX = spot.huX - padHu;
        spot.baseline = spot.baseline - padHu;
    }
}

std::vector<GlyphSpot> spotsInFrame(const std::vector<GlyphSpot>& spots, const FrameDesc& frame, std::int32_t padHu) {
    std::vector<GlyphSpot> local = spots;
    for (GlyphSpot& spot : local) {
        spot.huX = spot.huX + padHu;
        spot.baseline = spot.baseline + padHu;
        if (frame.lineStory >= 0) spot.story = frame.lineStory;
        if (frame.lineParagraph >= 0) spot.paragraph = frame.lineParagraph;
    }
    return local;
}

void releaseGpuImages() {}

// 줄 글자 표 한 구간의 글자 폭 합. 탭은 0 이다. 폭을 안 잰 구간은 재서 더한다.
std::int32_t runContentWidth(const Node& line, const LineGlyphs& glyphs, const LineRun& run,
                             const model::Document* doc) {
    if (run.kind == RunKind::Object) {
        const Node& slot = line.children[run.slot];
        return slot.width + slot.marginLeft + slot.marginRight;
    }
    if (run.kind == RunKind::Tab || run.kind == RunKind::Note) return 0;
    if (run.advanceCount > 0) {
        std::int32_t sum = 0;
        for (std::uint32_t i = 0; i < run.advanceCount; ++i) sum += glyphs.advances[run.advanceAt + i];
        return sum;
    }
    if (run.width > 0 || run.textEnd == run.textBegin || !doc) return run.width;
    std::vector<std::int32_t> advances;
    std::vector<std::int32_t> inks;
    const std::string_view text(line.text().data() + run.textBegin, run.textEnd - run.textBegin);
    measureText(doc, text, run.charShapeId, run.style->fontSize > 0 ? run.style->fontSize : 1000,
                run.style->ratio > 0 ? run.style->ratio : 100,
                advances, inks);
    std::int32_t sum = 0;
    for (const std::int32_t width : advances) sum += width;
    return sum;
}

namespace {

// 탭 자리. 칸이 정해지면 그 칸까지의 폭과 채움을 준다. tail 은 오른쪽·가운데 탭이 미는 뒤 글 폭을 늦게 잰다.
template <typename Tail>
TabGap tabGapFor(const Node& line, std::int32_t pen, Tail&& tail) {
    TabGap gap;
    const std::int32_t fromEdge = pen + line.x + line.indent + line.lineData().tabMargin;
    int align = 0;
    bool found = false;
    std::int32_t target = 0;
    for (const LineTab& place : line.lineData().tabStops) {
        if (place.pos > fromEdge) {
            target = place.pos;
            gap.leader = place.leader;
            align = place.align;
            found = true;
            break;
        }
    }
    const std::int32_t step = line.lineData().tabStep > 0 ? line.lineData().tabStep : 4000;
    if (!found) {
        if (line.lineData().tabStops.empty()) target = ((fromEdge / step) + 1) * step;
        else {
            target = line.lineData().tabStops.back().pos;
            while (target <= fromEdge) target += step;
        }
    }
    if (target < 0) target = 0;
    if (target > line.lineData().tabBodyRight) target = line.lineData().tabBodyRight;
    gap.width = std::max<std::int32_t>(0, target - fromEdge);
    if (found && align != 0) {
        const std::int32_t behind = tail();
        if (align == 1) gap.width -= behind;
        else gap.width -= behind / 2;
        if (gap.width < 0) gap.width = 0;
    }
    return gap;
}

}  // namespace

TabGap tabGapAtRun(const Node& line, const LineGlyphs& glyphs, std::int32_t pen, std::size_t runIndex,
                   const model::Document* doc) {
    return tabGapFor(line, pen, [&] {
        std::int32_t tail = 0;
        for (std::size_t i = runIndex + 1; i < glyphs.runs.size(); ++i) {
            const LineRun& next = glyphs.runs[i];
            if (next.kind == RunKind::Tab) break;
            tail += runContentWidth(line, glyphs, next, doc);
        }
        return tail;
    });
}


std::unordered_set<std::string> gLineWarnSeen;
std::mutex gLineWarnMu;

bool claimLineWarn(const std::string& key) {
    std::lock_guard lock(gLineWarnMu);
    return gLineWarnSeen.insert(key).second;
}




std::vector<std::pair<std::int32_t, std::int32_t>> columnRuleSpans(const Node* page, const Node& parent,
                                                                   std::int32_t ox, std::int32_t oy,
                                                                   std::int32_t mid, std::int32_t halfW,
                                                                   std::int32_t top, std::int32_t bottom) {
    std::vector<std::pair<std::int32_t, std::int32_t>> spans;
    if (bottom <= top) return spans;
    spans.push_back({top, bottom});
    const std::int32_t lineL = mid - halfW;
    const std::int32_t lineR = mid + halfW;
    auto subtractObject = [&](const Node& obj, std::int32_t baseX, std::int32_t baseY) {
        // 쪽 테두리는 본문 전체를 두르는 선이라 단 구분선을 끊지 않는다.
        if (obj.kind != NodeKind::Object || obj.treatAsChar || obj.role == "caption" || obj.role == "pageBorder") return;
        if (obj.ext().obj().textWrap == "BEHIND_TEXT" || obj.ext().obj().textWrap == "IN_FRONT_OF_TEXT") return;
        const std::int32_t boxL = baseX + obj.x - obj.marginLeft;
        const std::int32_t boxR = baseX + obj.x + obj.width + obj.marginRight;
        if (boxR <= lineL || boxL >= lineR) return;
        const std::int32_t boxT = baseY + obj.y - obj.marginTop;
        const std::int32_t boxB = baseY + obj.y + obj.height + obj.marginBottom;
        if (boxB <= boxT) return;
        std::vector<std::pair<std::int32_t, std::int32_t>> next;
        for (const auto& seg : spans) {
            if (boxB <= seg.first || boxT >= seg.second) {
                next.push_back(seg);
            } else {
                if (boxT > seg.first) next.push_back({seg.first, boxT});
                if (boxB < seg.second) next.push_back({boxB, seg.second});
            }
        }
        spans = std::move(next);
    };
    if (page && parent.kind == NodeKind::Body) {
        const std::int32_t pageOx = ox - parent.x;
        const std::int32_t pageOy = oy - parent.y;
        for (const Node& child : page->children) subtractObject(child, pageOx, pageOy);
    }
    for (const Node& child : parent.children) {
        subtractObject(child, ox, oy);
        if (child.kind != NodeKind::Column) continue;
        const std::int32_t colOx = ox + child.x;
        const std::int32_t colOy = oy + child.y;
        for (const Node& item : child.children) {
            subtractObject(item, colOx, colOy);
            if (item.kind != NodeKind::Paragraph) continue;
            const std::int32_t paraOx = colOx + item.x + item.marginLeft;
            const std::int32_t paraOy = colOy + item.y;
            for (const Node& grand : item.children) {
                subtractObject(grand, paraOx, paraOy);
            }
        }
    }
    return spans;
}

}  // namespace paint_detail

PipelineTally pipelineTally() {
    PipelineTallyState& state = paint_detail::pipelineTallyState();
    std::lock_guard lock(state.mu);
    return state.tally;
}

void clearDrawnPipelines() {
    PipelineTallyState& state = paint_detail::pipelineTallyState();
    std::lock_guard lock(state.mu);
    state.tally.drawnFirst.clear();
}

namespace {

// 글자처럼 넣은 개체 테두리의 왼쪽. 줄 원점 기준이고, 글자 폭을 나중에 잰 펜·정렬과 같다.
}  // namespace

bool paint_detail::treatAsCharBorders(const Node& line, const model::Document* doc, std::vector<InlineBorder>& border) {
    border.clear();
    if (!doc || line.kind != NodeKind::Line || line.spansLater) return false;
    bool any = false;
    for (const Node& child : line.children) {
        if (child.kind == NodeKind::Object && child.treatAsChar) {
            any = true;
            break;
        }
    }
    if (!any) return false;
    Painter painter;
    painter.doc = doc;
    const std::vector<Painter::Mark> marks = painter.collectMarks(line);
    // 개체 자리는 글자를 그리는 자리와 같은 placeLine 에서 읽는다.
    const Painter::LinePlacement placed = painter.placeLine(line, marks);
    border.assign(line.children.size(), {});
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const Painter::Mark& mark = marks[i];
        if (mark.object && mark.slot >= 0 && mark.slot < static_cast<int>(border.size())) {
            InlineBorder place;
            place.ok = true;
            place.x = placed.at[i];
            border[static_cast<std::size_t>(mark.slot)] = place;
        }
    }
    return true;
}

namespace {

void clearHangulOutlineCache() {
    clearHangulCommands();
    clearLinePlaces();
    clearHangulCurves();
}

bool drawnCode(char32_t cp) {
    return cp != 0 && cp != ' ' && cp != '\t' && cp != '\n' && cp != 0x2007 && cp != 0x00A0;
}

std::uint32_t pageHeightOf(const model::Section& section) {
    if (section.page && section.page->height > 0) return section.page->height;
    return 84188;
}

const model::CharShape* shapeFrom(const std::unordered_map<std::uint32_t, const model::CharShape*>& shapes,
                                  std::uint32_t id) {
    const auto found = shapes.find(id);
    return found == shapes.end() ? nullptr : found->second;
}

int langSlot(char32_t cp) {
    return (cp >= 0xAC00 && cp <= 0xD7A3) ? static_cast<int>(model::Lang::Hangul) : static_cast<int>(model::Lang::Latin);
}

int langValue(const model::PerLang<std::int32_t>& values, int slot, int fallback) {
    if (slot < 0 || slot >= static_cast<int>(values.size())) return fallback;
    return values[static_cast<std::size_t>(slot)] != 0 ? values[static_cast<std::size_t>(slot)] : fallback;
}

std::string faceOf(const model::Document& doc, const model::CharShape& shape, int slot) {
    if (slot >= 0 && slot < static_cast<int>(doc.header.fonts.size()) && slot < static_cast<int>(shape.fontRef.size())) {
        const auto& fonts = doc.header.fonts[static_cast<std::size_t>(slot)];
        const std::uint32_t id = shape.fontRef[static_cast<std::size_t>(slot)];
        if (id < fonts.size() && !fonts[id].face.empty()) return fonts[id].face;
    }
    return "함초롬바탕";
}

float maskExactPpemOf(const model::CharShape& shape, int rel, std::uint32_t pageH) {
    int size = shape.height > 0 ? shape.height : 1000;
    if (rel > 0 && rel != 100) size = static_cast<int>((static_cast<std::int64_t>(size) * rel + 50) / 100);
    if (shape.superscript || shape.subscript) {
        size = static_cast<int>((static_cast<std::int64_t>(size) * 16 + 12) / 25);
    }
    if (pageH == 0) pageH = 84188;
    const int pixelH = pagePixelHeight(static_cast<int>(pageH), 100);
    return std::max(1.f, static_cast<float>(size) * static_cast<float>(pixelH) / static_cast<float>(pageH));
}

std::string maskSeenKey(const std::string& face, char32_t cp, bool bold, bool italic, int ratio, int ppem) {
    std::string key = face;
    key.push_back('\n');
    key += std::to_string(static_cast<std::uint32_t>(cp));
    key.push_back(bold ? 'b' : 'r');
    key.push_back(italic ? 'i' : 'n');
    key.push_back('\n');
    key += std::to_string(ratio);
    key.push_back('\n');
    key += std::to_string(ppem);
    return key;
}

void noteCode(const model::Document& doc, const model::CharShape& shape, std::uint32_t shapeId, char32_t cp,
              std::uint32_t pageH, std::unordered_set<std::string>& seenMasks, std::vector<MaskJob>& localJobs) {
    if (cp == 0 || cp == '\n') return;
    const int slot = langSlot(cp);
    const model::Lang lang = slot == static_cast<int>(model::Lang::Hangul) ? model::Lang::Hangul : model::Lang::Latin;
    std::int32_t width = 0;
    std::int32_t ink = 0;
    if (!takeTextAdvance(shapeId, cp, width, ink)) {
        const std::int32_t advance = layout::glyphAdvance(cp, shape, lang, &doc);
        const std::int32_t trail = layout::glyphTrail(cp, shape, lang, &doc);
        width = advance > 0 ? advance : 0;
        ink = std::max<std::int32_t>(0, width - trail);
        keepTextAdvance(shapeId, cp, width, ink);
    }
    if (!drawnCode(cp)) return;
    const int ratio = langValue(shape.ratio, slot, 100);
    const int rel = langValue(shape.relSize, slot, 100);
    const float exact = maskExactPpemOf(shape, rel, pageH);
    const int ppem = std::max(1, static_cast<int>(std::lround(exact)));
    const std::string face = faceOf(doc, shape, slot);
    const std::string key = maskSeenKey(face, cp, shape.bold, shape.italic, ratio, ppem);
    if (!seenMasks.insert(key).second) return;
    localJobs.push_back(MaskJob{face, cp, shape.bold, shape.italic, ratio, ppem, 100, exact});
}

void noteText(const model::Document& doc, const model::CharShape& shape, std::uint32_t shapeId, std::string_view text,
              std::uint32_t pageH, std::unordered_set<std::string>& seenMasks, std::vector<MaskJob>& localJobs) {
    std::size_t index = 0;
    char32_t cp = 0;
    while (nextCp(text, index, cp)) noteCode(doc, shape, shapeId, cp, pageH, seenMasks, localJobs);
}

void noteControl(const model::Document& doc, const model::Control& control, std::uint32_t pageH,
                 const std::unordered_map<std::uint32_t, const model::CharShape*>& shapes,
                 std::unordered_set<std::string>& seenMasks, std::vector<MaskJob>& localJobs);

void noteParagraphs(const model::Document& doc, const std::vector<model::Paragraph>& paragraphs, std::uint32_t pageH,
                    const std::unordered_map<std::uint32_t, const model::CharShape*>& shapes,
                    std::unordered_set<std::string>& seenMasks, std::vector<MaskJob>& localJobs) {
    for (const model::Paragraph& para : paragraphs) {
        const model::ParaShape* paraShape = nullptr;
        if (para.paraShapeId < doc.header.paraShapes.size() &&
            doc.header.paraShapes[para.paraShapeId].id == para.paraShapeId) {
            paraShape = &doc.header.paraShapes[para.paraShapeId];
        } else {
            for (const model::ParaShape& item : doc.header.paraShapes) {
                if (item.id == para.paraShapeId) {
                    paraShape = &item;
                    break;
                }
            }
        }
        // 머리 글자 모양이 없으면 조판처럼 문단 끝 글자의 모양을 쓴다.
        const model::CharShape* ending = nullptr;
        std::uint32_t endingId = 0;
        for (const model::Run& run : para.runs) {
            if (const model::CharShape* shape = shapeFrom(shapes, run.charShapeId)) {
                ending = shape;
                endingId = run.charShapeId;
            }
        }
        if (paraShape && paraShape->headingType == "BULLET") {
            for (const model::Bullet& bullet : doc.header.bullets) {
                if (bullet.id != paraShape->headingIdRef) continue;
                const model::CharShape* shaped = shapeFrom(shapes, bullet.head.charShapeId);
                std::uint32_t shapedId = bullet.head.charShapeId;
                if (!shaped) {
                    shaped = ending;
                    shapedId = endingId;
                }
                if (!shaped) break;
                noteText(doc, *shaped, shapedId, bullet.text, pageH, seenMasks, localJobs);
                noteText(doc, *shaped, shapedId, bullet.checkedText, pageH, seenMasks, localJobs);
                break;
            }
        } else if (paraShape && (paraShape->headingType == "NUMBER" || paraShape->headingType == "OUTLINE")) {
            for (const model::Numbering& numbering : doc.header.numberings) {
                if (numbering.id != paraShape->headingIdRef) continue;
                for (const model::ParaHead& head : numbering.levels) {
                    const model::CharShape* shaped = shapeFrom(shapes, head.charShapeId);
                    std::uint32_t shapedId = head.charShapeId;
                    if (!shaped) {
                        shaped = ending;
                        shapedId = endingId;
                    }
                    if (!shaped) continue;
                    std::size_t index = 0;
                    char32_t cp = 0;
                    while (nextCp(head.format, index, cp)) {
                        if (cp != '^') {
                            noteCode(doc, *shaped, shapedId, cp, pageH, seenMasks, localJobs);
                            continue;
                        }
                        if (!nextCp(head.format, index, cp)) break;
                    }
                    const char* alphabet = "0123456789";
                    if (head.numFormat == "ROMAN_CAPITAL") alphabet = "IVXLCDM";
                    else if (head.numFormat == "ROMAN_SMALL") alphabet = "ivxlcdm";
                    else if (head.numFormat == "HANGUL_SYLLABLE") alphabet = "가나다라마바사아자차카타파하";
                    else if (head.numFormat == "HANGUL_JAMO") alphabet = "ㄱㄴㄷㄹㅁㅂㅅㅇㅈㅊㅋㅌㅍㅎ";
                    else if (head.numFormat == "LATIN_CAPITAL") alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
                    else if (head.numFormat == "LATIN_SMALL") alphabet = "abcdefghijklmnopqrstuvwxyz";
                    noteText(doc, *shaped, shapedId, alphabet, pageH, seenMasks, localJobs);
                }
                break;
            }
        }
        for (const model::Run& run : para.runs) {
            const model::CharShape* shape = shapeFrom(shapes, run.charShapeId);
            model::CharShape fallback;
            fallback.height = 1000;
            fallback.ratio.fill(100);
            const model::CharShape* measure = shape ? shape : &fallback;
            for (const model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Text) {
                    noteText(doc, *measure, run.charShapeId, item.text, pageH, seenMasks, localJobs);
                } else {
                    noteControl(doc, item.control, pageH, shapes, seenMasks, localJobs);
                }
            }
        }
    }
}

void noteControl(const model::Document& doc, const model::Control& control, std::uint32_t pageH,
                 const std::unordered_map<std::uint32_t, const model::CharShape*>& shapes,
                 std::unordered_set<std::string>& seenMasks, std::vector<MaskJob>& localJobs) {
    if (control.table) {
        for (const model::TableCell& cell : control.table->cells) {
            noteParagraphs(doc, cell.content.paragraphs, pageH, shapes, seenMasks, localJobs);
        }
    }
    if (control.caption) noteParagraphs(doc, control.caption->content.paragraphs, pageH, shapes, seenMasks, localJobs);
    for (const model::SubList& sub : control.subLists) noteParagraphs(doc, sub.paragraphs, pageH, shapes, seenMasks, localJobs);
    for (const model::Control& child : control.children) noteControl(doc, child, pageH, shapes, seenMasks, localJobs);
}

void gatherDocumentGlyphs(const model::Document& doc) {
    {
        std::lock_guard lock(glyphAtlasMutex());
        if (documentGlyphsNoted()) return;
    }
    layout::warmDocumentFonts(doc);
    std::unordered_map<std::uint32_t, const model::CharShape*> shapes;
    for (const model::CharShape& shape : doc.header.charShapes) shapes.emplace(shape.id, &shape);
    std::unordered_set<std::string> seenMasks;
    std::vector<MaskJob> localJobs;
    for (const model::Section& section : doc.sections) {
        const std::uint32_t pageH = pageHeightOf(section);
        noteParagraphs(doc, section.paragraphs, pageH, shapes, seenMasks, localJobs);
        for (const model::MasterPage& master : section.masterPages) {
            noteParagraphs(doc, master.content.paragraphs, pageH, shapes, seenMasks, localJobs);
        }
    }
    // 100% 기준 마스크만 큐에 넣고, 축소(<= 100%) 시에는 100% 마스크와 도트 마스크를 공유한다.
    const std::size_t baseJobCount = localJobs.size();
    int maskCount = 0;
    {
        std::lock_guard lock(glyphAtlasMutex());
        if (!documentGlyphsNoted()) {
            baseMaskJobCount() = baseJobCount;
            maskJobs() = std::move(localJobs);
            maskJobCursor() = 0;
            maskZoomJobCursor() = 0;
            documentGlyphsNoted() = true;
        }
        maskCount = static_cast<int>(maskJobs().size());
    }
    int advanceCount = 0;
    {
        std::shared_lock lock(gAdvanceMu);
        advanceCount = static_cast<int>(textAdvances().size());
    }
    std::fprintf(stderr, "[ClassingHWP] 글자 전진폭 %d 마스크 %d\n", advanceCount, maskCount);
}

}  // namespace

PageBitmap renderPageBitmap(const model::Document& doc, const Node& page, int pixelHeight, int viewX, int viewY,
                            int viewW, int viewH, PaintLayer layer, int pageIndex, std::uint8_t background) {
    PageBitmap bitmap;
    if (pixelHeight < 32 || page.width <= 0 || page.height <= 0) return bitmap;
    Painter painter;
    painter.doc = &doc;
    painter.pageNode = &page;
    painter.pageIndex = pageIndex;
    painter.pass = layer;
    painter.floatCaptions = true;
    painter.scale = static_cast<float>(pixelHeight) / static_cast<float>(page.height);
    const int fullW = std::max(1, static_cast<int>(viewWidthOf(page) * painter.scale + 0.5f));
    const int fullH = std::max(1, pixelHeight);
    if (viewW <= 0 || viewH <= 0) {
        viewX = 0;
        viewY = 0;
        viewW = fullW;
        viewH = fullH;
    } else {
        viewX = std::clamp(viewX, 0, fullW - 1);
        viewY = std::clamp(viewY, 0, fullH - 1);
        viewW = std::clamp(viewW, 1, fullW - viewX);
        viewH = std::clamp(viewH, 1, fullH - viewY);
    }
    const bool partial = viewX != 0 || viewY != 0 || viewW != fullW || viewH != fullH;
    painter.limitView = partial;
    painter.viewRightHu = static_cast<float>(viewW) / painter.scale;
    painter.viewBottomHu = static_cast<float>(viewH) / painter.scale;
    const auto shiftX = static_cast<std::int32_t>(std::lround(static_cast<double>(viewX) / painter.scale));
    const auto shiftY = static_cast<std::int32_t>(std::lround(static_cast<double>(viewY) / painter.scale));
    painter.originX = shiftX;
    painter.originY = shiftY;
    painter.bitmap.width = std::max(1, viewW);
    painter.bitmap.height = std::max(1, viewH);
    const bool layered = layer != PaintLayer::All;
    painter.bitmap.px.assign(static_cast<std::size_t>(painter.bitmap.width) * painter.bitmap.height * 4u,
                             layered ? 0 : background);
    if (!layered && background != 255) {
        for (std::size_t i = 3; i < painter.bitmap.px.size(); i += 4) painter.bitmap.px[i] = 255;
    }
    painter.bitmap.clipR = painter.bitmap.width;
    painter.bitmap.clipB = painter.bitmap.height;
    painter.walk(page, -shiftX, -shiftY);
    if (layered) {
        bool ink = false;
        const std::vector<std::uint8_t>& px = painter.bitmap.px;
        for (std::size_t i = 3; i < px.size(); i += 4) {
            if (px[i] != 0) {
                ink = true;
                break;
            }
        }
        if (!ink) return {};
    }
    bitmap.width = painter.bitmap.width;
    bitmap.height = painter.bitmap.height;
    bitmap.rgba = std::move(painter.bitmap.px);
    return bitmap;
}

namespace {

int framePx(std::int32_t hu, float scale) {
    return static_cast<int>(hu * scale + (hu < 0 ? -0.5f : 0.5f));
}

bool frameBehind(const Node& node) {
    const bool masterBack = node.fromMaster && !node.masterOnTop;
    const bool masterFront = node.fromMaster && node.masterOnTop;
    if (masterFront) return false;
    if (masterBack) return true;
    return node.ext().obj().textWrap == "BEHIND_TEXT";
}

// 쪽 개체는 zOrder가 큰 쪽이 위다. 같은 값이면 뒤에 나온 쪽이 위다.
// 한 칸 안에 묶음 자식을 넣는다. 자식이 깊어져도 다음 형제 칸을 넘지 않는다.
constexpr std::int64_t kFrameZSlot = 1LL << 16;
constexpr std::int64_t kFrameZGap = kFrameZSlot * 100000LL;
constexpr std::int32_t kTableBorderStripHu = 16000;

bool edgeHasStroke(const BorderEdge& edge) {
    return !edge.type.empty() && edge.type != "NONE";
}

bool cellHasDiagonalStroke(const Node& cell) {
    if (!edgeHasStroke(cell.ext().borderDiagonal)) return false;
    const bool hasSlash = !cell.ext().slash.type.empty() && cell.ext().slash.type != "NONE";
    const bool hasBackSlash = !cell.ext().backSlash.type.empty() && cell.ext().backSlash.type != "NONE";
    const bool hasCenter = !cell.ext().centerLine.empty() && cell.ext().centerLine != "NONE";
    return hasSlash || hasBackSlash || hasCenter;
}

bool cellHasAnyBorder(const Node& cell) {
    return edgeHasStroke(cell.ext().borderTop) || edgeHasStroke(cell.ext().borderBottom) ||
           edgeHasStroke(cell.ext().borderLeft) || edgeHasStroke(cell.ext().borderRight) ||
           cellHasDiagonalStroke(cell);
}

bool cellHasFillInk(const Node& cell) {
    if (!cell.ext().fillColor.empty() && cell.ext().fillColor != "none") return true;
    if (!cell.ext().hatchStyle.empty()) return true;
    if (cell.ext().obj().imageFillMode >= 0 && !cell.ext().obj().binItemId.empty()) return true;
    return false;
}

bool tableBoxHasInk(const Node& table) {
    if (!table.ext().fillColor.empty() && table.ext().fillColor != "none") return true;
    if (edgeHasStroke(table.ext().borderLeft)) return true;
    return false;
}

bool pictureFitsBaseFrame(const Node& node, float baseScale) {
    if (node.role != "picture" || node.ext().obj().binItemId.empty()) return false;
    if (edgeHasStroke(node.ext().borderLeft)) return false;
    if (!node.ext().fillColor.empty() && node.ext().fillColor != "none") return false;
    const int baseW = std::max(1, framePx(node.x + node.width, baseScale) - framePx(node.x, baseScale));
    const int baseH = std::max(1, framePx(node.y + node.height, baseScale) - framePx(node.y, baseScale));
    const std::shared_ptr<const CachedPicture> picture = peekPicture(node.ext().obj().binItemId);
    if (!picture) return false;
    return picture->width > 0 && picture->height > 0 && picture->width <= baseW && picture->height <= baseH;
}

// 테두리 없는 그림 개체는 칸에 픽셀이 없고 화면이 텍스처로 그린다. 확대해도 다시 구울 것이 없다.
// objectBox 가 그림으로 그리는 모양만 해당한다. 둥근 모서리나 다각형은 예전처럼 굽는다.
bool pictureDrawsOnScreen(const Node& node) {
    if (node.role != "picture" || node.ext().obj().binItemId.empty() || !screenVectors()) return false;
    if (node.ext().obj().shapeType == "ellipse" || node.ext().obj().shapeType == "arc" || node.ext().obj().points.size() >= 2) return false;
    if (node.ext().obj().cornerRatio > 0 && node.ext().obj().cornerRatio <= 50) return false;
    if (edgeHasStroke(node.ext().borderLeft)) return false;
    const std::shared_ptr<const CachedPicture> picture = peekPicture(node.ext().obj().binItemId);
    return !picture || (picture->width <= kMaxPictureTexture && picture->height <= kMaxPictureTexture);
}

inline std::uint64_t fnvMix64(std::uint64_t h, std::uint64_t v) {
    h ^= v;
    h *= 1099511628211ULL;
    return h;
}

inline std::uint64_t fnvMixStr64(std::uint64_t h, const std::string& s) {
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return fnvMix64(h, static_cast<std::uint64_t>(s.size()));
}

inline std::uint64_t fnvMixEdge64(std::uint64_t h, const BorderEdge& e) {
    if (e.type.empty() || e.type == "NONE") return fnvMix64(h, 0);
    h = fnvMixStr64(h, e.type);
    h = fnvMixStr64(h, e.color);
    h = fnvMix64(h, static_cast<std::uint64_t>(e.width));
    return fnvMix64(h, static_cast<std::uint64_t>(e.end));
}

inline void unionHuBox(std::int32_t& x, std::int32_t& y, std::int32_t& w, std::int32_t& h,
                       std::int32_t bx, std::int32_t by, std::int32_t bw, std::int32_t bh) {
    if (bw <= 0 || bh <= 0) return;
    if (w <= 0 || h <= 0) {
        x = bx;
        y = by;
        w = bw;
        h = bh;
        return;
    }
    const std::int32_t right = std::max(x + w, bx + bw);
    const std::int32_t bottom = std::max(y + h, by + bh);
    x = std::min(x, bx);
    y = std::min(y, by);
    w = std::max<std::int32_t>(0, right - x);
    h = std::max<std::int32_t>(0, bottom - y);
}

// 쪽 칸 상자를 잡는 동안의 세로쓰기 틀. FrameWalk::visit 과 같이 돌린 틀은 부모 자리를 원점으로 (0, 0) 부터 재고,
// 세운 개체 상자 안은 종이 자리로 잰다.
struct HuTurn {
    bool on = false;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t span = 0;
};
thread_local HuTurn gHuTurn;
thread_local std::int32_t gHuPaperW = 0;

// 줄 글자 표를 줄 내용 해시에 섞는다. 그리기 모양은 글자 모양에서 읽어 함께 섞는다.
std::uint64_t mixLineGlyphs(std::uint64_t h, const Node& line, const model::Document* doc) {
    const LineGlyphs& glyphs = lineGlyphs(line);
    if (const HeadingRun* head = glyphs.heading.get()) {
        h = fnvMix64(h, 0x48454144ULL);
        h = fnvMix64(h, static_cast<std::uint64_t>(head->x));
        h = fnvMix64(h, static_cast<std::uint64_t>(head->width));
        h = fnvMix64(h, static_cast<std::uint64_t>(head->charShapeId));
        h = fnvMix64(h, static_cast<std::uint64_t>(head->fontSize));
        h = fnvMix64(h, static_cast<std::uint64_t>(static_cast<std::uint16_t>(head->ratio)) |
                            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(head->relSize)) << 16));
        h = fnvMix64(h, static_cast<std::uint64_t>((head->bold ? 1 : 0) | (head->italic ? 2 : 0) |
                                                   (head->superscript ? 4 : 0) | (head->subscript ? 8 : 0)));
        h = fnvMixStr64(h, head->text);
        h = fnvMixStr64(h, head->font);
        h = fnvMixStr64(h, head->color);
        for (const std::int32_t advance : head->advances) h = fnvMix64(h, static_cast<std::uint64_t>(advance));
    }
    for (const LineRun& run : glyphs.runs) {
        h = fnvMix64(h, static_cast<std::uint64_t>(run.kind) | (run.synthetic ? 16ULL : 0ULL));
        if (run.kind == RunKind::Object) {
            h = fnvMix64(h, run.slot);
            continue;
        }
        h = fnvMixStr64(h, std::string(line.text(), run.textBegin, run.textEnd - run.textBegin));
        h = fnvMix64(h, static_cast<std::uint64_t>(run.width));
        h = fnvMix64(h, static_cast<std::uint64_t>(run.charShapeId));
        h = fnvMix64(h, static_cast<std::uint64_t>(run.style->fontSize));
        h = fnvMix64(h, static_cast<std::uint64_t>(static_cast<std::uint16_t>(run.style->ratio)) |
                            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(run.style->relSize)) << 16) |
                            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(run.style->spacing)) << 32) |
                            (static_cast<std::uint64_t>(static_cast<std::uint16_t>(run.style->charOffset)) << 48));
        h = fnvMixStr64(h, run.style->font ? *run.style->font : std::string{});
        h = fnvMixStr64(h, run.style->markpen ? *run.style->markpen : std::string{});
        h = fnvMixStr64(h, run.style->tabLeader ? *run.style->tabLeader : std::string{});
        h = fnvMix64(h, run.style->track);
        for (std::uint32_t i = 0; i < run.advanceCount; ++i) {
            h = fnvMix64(h, static_cast<std::uint64_t>(glyphs.advances[run.advanceAt + i]));
        }
        if (doc) {
            const detail::CharPaint paint = detail::charPaint(*doc, run.charShapeId);
            h = fnvMixStr64(h, paint.color);
            h = fnvMixStr64(h, paint.shade);
            h = fnvMixStr64(h, paint.fillColor);
            h = fnvMixStr64(h, paint.hatchStyle);
            h = fnvMixStr64(h, paint.hatchColor);
            h = fnvMixStr64(h, paint.underline);
            h = fnvMix64(h, static_cast<std::uint64_t>((paint.bold ? 1 : 0) | (paint.italic ? 2 : 0) |
                                                       (paint.strikeout ? 4 : 0) | (paint.superscript ? 16 : 0) |
                                                       (paint.subscript ? 32 : 0) | (paint.bitmapDeco ? 64 : 0)));
        }
    }
    return h;
}

void computeNodeHuBounds(Node& node, std::int32_t ox, std::int32_t oy, bool inObject, bool isPage,
                         const model::Document* doc, std::int32_t& pageSerial) {
    if (isPage) gHuPaperW = node.width;
    struct TurnRestore {
        HuTurn saved = gHuTurn;
        ~TurnRestore() { gHuTurn = saved; }
    } turnRestore;
    if (node.turned != 0) {
        gHuTurn = {true, ox, oy, node.turnSpan > 0 ? node.turnSpan : gHuPaperW};
        ox = 0;
        oy = 0;
    }
    if (node.upright && gHuTurn.on) {
        const std::int32_t paperX = gHuTurn.x + gHuTurn.span - (oy + node.y) - node.height;
        const std::int32_t paperY = gHuTurn.y + ox + node.x;
        gHuTurn = {};
        for (Node& child : node.children) computeNodeHuBounds(child, paperX, paperY, true, false, doc, pageSerial);
        node.huX = ox + node.x;
        node.huY = oy + node.y;
        node.huW = node.width;
        node.huH = node.height;
        node.huBoundsReady = true;
        return;
    }
    const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
    const std::int32_t y = oy + node.y;

    if (!isPage && node.kind == NodeKind::Line) {
        std::int32_t top = y;
        std::int32_t height = node.height > 0 ? node.height : node.lineData().textHeight;
        if (node.lineData().auditRise > 0) {
            top -= node.lineData().auditRise;
            height += node.lineData().auditRise;
        }
        height += std::max<std::int32_t>(0, node.editContent().auditBelow);
        std::vector<InlineBorder> borders;
        const bool seated = treatAsCharBorders(node, doc, borders);
        std::int32_t extraLeft = 0;
        std::int32_t extraRight = 0;
        for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
            const Node& child = node.children[static_cast<std::size_t>(i)];
            if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
            const std::int32_t moved =
                seated && borders[static_cast<std::size_t>(i)].ok ? borders[static_cast<std::size_t>(i)].x - child.x : 0;
            const std::int32_t objL = child.x + moved + child.marginLeft;
            const std::int32_t objR = objL + child.width + child.marginRight;
            if (objL < 0) extraLeft = std::max(extraLeft, -objL);
            if (objR > node.width) extraRight = std::max(extraRight, objR - node.width);
            for (const Node& cap : child.children) {
                if (cap.role != "caption") continue;
                const std::int32_t capL = objL + cap.x;
                const std::int32_t capR = capL + cap.width;
                if (capL < 0) extraLeft = std::max(extraLeft, -capL);
                if (capR > node.width) extraRight = std::max(extraRight, capR - node.width);
            }
        }
        const std::int32_t slide = std::max<std::int32_t>(0, node.indent);
        std::int32_t coveredAbove = std::max<std::int32_t>(0, node.editContent().auditRise);
        std::int32_t coveredBelow =
            (node.height > 0 ? node.height : node.editContent().textHeight) + std::max<std::int32_t>(0, node.editContent().auditBelow);
        if (const HeadingRun* head = lineGlyphs(node).heading.get()) {
            const std::int32_t left = slide + head->x;
            std::int32_t right = left + std::max(head->width, 0);
            if (!head->advances.empty()) {
                right = left;
                for (const std::int32_t width : head->advances) right += width;
            }
            if (left < 0) extraLeft = std::max(extraLeft, -left);
            if (right > node.width) extraRight = std::max(extraRight, right - node.width);
        }
        for (const Node& child : node.children) {
            if (!child.headingMark || child.kind != NodeKind::Object) continue;
            const std::int32_t left = slide + child.x;
            const std::int32_t right = left + child.width;
            if (left < 0) extraLeft = std::max(extraLeft, -left);
            if (right > node.width) extraRight = std::max(extraRight, right - node.width);
            if (child.y < -coveredAbove) {
                const std::int32_t more = -child.y - coveredAbove;
                top -= more;
                height += more;
                coveredAbove += more;
            }
            const std::int32_t bottom = child.y + std::max(child.height, 0);
            if (bottom > coveredBelow) {
                height += bottom - coveredBelow;
                coveredBelow = bottom;
            }
        }
        node.editContent().lineBoxX = x - extraLeft;
        node.editContent().lineBoxY = top;
        node.editContent().lineBoxW = std::max<std::int32_t>(0, node.width + extraLeft + extraRight);
        node.editContent().lineBoxH = std::max<std::int32_t>(0, height);
        node.huX = node.lineData().lineBoxX;
        node.huY = node.lineData().lineBoxY;
        node.huW = node.lineData().lineBoxW;
        node.huH = node.lineData().lineBoxH;

        std::uint64_t h = 14695981039346656037ULL;
        h = fnvMix64(h, 1001ULL);
        h = fnvMix64(h, static_cast<std::uint64_t>(node.width));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.height));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.lineData().textHeight));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.lineData().lineBaseline));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.marginTop));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.indent));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.lineData().auditRise));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.lineData().auditBelow));
        h = fnvMix64(h, static_cast<std::uint64_t>((node.lineData().lastLine ? 1 : 0) | (node.lineData().lineBreak ? 2 : 0) |
                                                   (node.lineData().deferTabs ? 4 : 0) | (node.lineData().matchFontBaseline ? 8 : 0)));
        h = fnvMixStr64(h, node.align);
        h = fnvMixStr64(h, node.vertAlign);
        // 반전 글자 겹치기 안 글자 줄(reverse)은 같은 글자라도 흰색이다. 글자 자리를 같은 해시로 나눠 쓰지 않는다.
        h = fnvMixStr64(h, node.role);

        for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
            Node& child = node.children[static_cast<std::size_t>(i)];
            // 본문 글자와 머리 번호 글자는 줄 글자 표로 섞는다(아래 mixLineGlyphs).
            if (child.kind == NodeKind::Object && child.treatAsChar) {
                std::int32_t childOx = x + child.marginLeft;
                std::int32_t effX = child.x;
                if (seated && borders[static_cast<std::size_t>(i)].ok) {
                    effX = borders[static_cast<std::size_t>(i)].x;
                    childOx = x + effX + child.marginLeft - child.x;
                }
                computeNodeHuBounds(child, childOx, y + child.marginTop, true, false, doc, pageSerial);
                unionHuBox(node.huX, node.huY, node.huW, node.huH, child.huX, child.huY, child.huW, child.huH);
                h = fnvMix64(h, static_cast<std::uint64_t>(effX));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.y));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.width));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.height));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.marginLeft));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.marginRight));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.marginTop));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.marginBottom));
                h = fnvMix64(h, child.contentHash);
            } else if (child.headingMark && child.kind == NodeKind::Object) {
                h = fnvMix64(h, static_cast<std::uint64_t>(child.x));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.y));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.width));
                h = fnvMix64(h, static_cast<std::uint64_t>(child.height));
                h = fnvMixStr64(h, child.ext().obj().binItemId);
            }
        }
        h = mixLineGlyphs(h, node, doc);
        // 메모 범위 상자는 글자 밖 모델(필드)에서 온다. 글자가 같아도 메모를 넣고 지우면 다른 그림이다.
        if (doc && paint_detail::lineHasMemoRange(node, doc)) {
            Painter painter;
            painter.doc = doc;
            const std::vector<Painter::Mark> marks = painter.collectMarks(node);
            const Painter::LinePlacement place = painter.placeLine(node, marks);
            for (const Painter::MemoRange& range : painter.lineMemoRanges(node, 0, 0, marks, place)) {
                h = fnvMix64(h, static_cast<std::uint64_t>(range.x0));
                h = fnvMix64(h, static_cast<std::uint64_t>(range.x1));
                h = fnvMix64(h, static_cast<std::uint64_t>((range.head ? 1 : 0) | (range.tail ? 2 : 0)));
                h = fnvMixStr64(h, range.fill);
                h = fnvMixStr64(h, range.line);
            }
        }
        node.contentHash = node.spansLater ? 0ULL : (h == 0ULL ? 1ULL : h);
        node.huBoundsReady = true;
        return;
    }

    if (!isPage && node.kind == NodeKind::Object) {
        if (!inObject && !node.treatAsChar) {
            node.extMut().objMut().objectPageSerial = pageSerial++;
        } else {
            node.extMut().objMut().objectPageSerial = -1;
        }
        const std::int32_t bleed = tableBorderBleed(node);
        node.huX = x - bleed;
        node.huY = y - bleed;
        node.huW = std::max<std::int32_t>(0, node.width + bleed * 2);
        node.huH = std::max<std::int32_t>(0, node.height + bleed * 2);

        std::uint64_t h = 14695981039346656037ULL;
        h = fnvMix64(h, 2002ULL);
        h = fnvMix64(h, static_cast<std::uint64_t>(node.width));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.height));
        h = fnvMixStr64(h, node.role);
        h = fnvMixStr64(h, node.ext().obj().shapeType);
        h = fnvMixStr64(h, node.ext().fillColor);
        h = fnvMixStr64(h, node.ext().gradColor);
        h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().gradAngle));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().obj().cornerRatio));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().obj().imageFillMode));
        h = fnvMixStr64(h, node.ext().obj().binItemId);
        h = fnvMixStr64(h, node.ext().obj().script);
        // 글맵시: 글·글꼴·모양·간격·정렬·그림자가 바뀌면 다시 그린다.
        h = fnvMixStr64(h, node.ext().obj().artText);
        h = fnvMixStr64(h, node.ext().obj().artFont);
        h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().obj().artShape));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().obj().artLineSpacing));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().obj().artCharSpacing));
        h = fnvMixStr64(h, node.ext().obj().artAlign);
        h = fnvMix64(h, node.ext().obj().artShadow ? 1ULL : 0ULL);
        h = fnvMixStr64(h, node.ext().obj().artShadowColor);
        h = fnvMix64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(node.ext().obj().artShadowX)));
        h = fnvMix64(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(node.ext().obj().artShadowY)));
        h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().cellSpacing));
        h = fnvMix64(h, static_cast<std::uint64_t>((node.flipHorizontal ? 1 : 0) | (node.flipVertical ? 2 : 0)));
        h = fnvMixEdge64(h, node.ext().borderLeft);
        h = fnvMixEdge64(h, node.ext().borderRight);
        h = fnvMixEdge64(h, node.ext().borderTop);
        h = fnvMixEdge64(h, node.ext().borderBottom);
        for (const auto& pt : node.ext().obj().points) {
            h = fnvMix64(h, static_cast<std::uint64_t>(pt.x));
            h = fnvMix64(h, static_cast<std::uint64_t>(pt.y));
        }

        std::int32_t maxY = node.height;
        for (Node& child : node.children) {
            computeNodeHuBounds(child, x, y, true, false, doc, pageSerial);
            unionHuBox(node.huX, node.huY, node.huW, node.huH, child.huX, child.huY, child.huW, child.huH);
            if (node.role == "table" && child.kind == NodeKind::Cell) {
                maxY = std::max(maxY, std::max<std::int32_t>(0, child.y + child.height));
            }
            h = fnvMix64(h, static_cast<std::uint64_t>(child.x));
            h = fnvMix64(h, static_cast<std::uint64_t>(child.y));
            h = fnvMix64(h, child.contentHash);
        }
        if (node.role == "table" && maxY > 0 && node.width > 0) {
            unionHuBox(node.huX, node.huY, node.huW, node.huH, x - bleed, y - bleed,
                       node.width + bleed * 2, maxY + bleed * 2);
        }
        node.contentHash = node.spansLater ? 0ULL : (h == 0ULL ? 1ULL : h);
        node.huBoundsReady = true;
        return;
    }

    node.huX = x;
    node.huY = y;
    node.huW = std::max<std::int32_t>(0, node.width);
    node.huH = std::max<std::int32_t>(0, node.height);

    std::uint64_t h = 14695981039346656037ULL;
    h = fnvMix64(h, static_cast<std::uint64_t>(node.kind));
    h = fnvMix64(h, static_cast<std::uint64_t>(node.width));
    h = fnvMix64(h, static_cast<std::uint64_t>(node.height));
    if (node.kind == NodeKind::Cell || node.kind == NodeKind::Paragraph) {
        h = fnvMixStr64(h, node.ext().fillColor);
        h = fnvMixStr64(h, node.ext().hatchStyle);
        h = fnvMixStr64(h, node.ext().hatchColor);
        h = fnvMixStr64(h, node.ext().gradColor);
        h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().obj().imageFillMode));
        h = fnvMixStr64(h, node.ext().obj().binItemId);
        h = fnvMixEdge64(h, node.ext().borderLeft);
        h = fnvMixEdge64(h, node.ext().borderRight);
        h = fnvMixEdge64(h, node.ext().borderTop);
        h = fnvMixEdge64(h, node.ext().borderBottom);
        h = fnvMixEdge64(h, node.ext().borderDiagonal);
        if (node.kind == NodeKind::Paragraph) {
            h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().borderOffsetLeft));
            h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().borderOffsetRight));
            h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().borderOffsetTop));
            h = fnvMix64(h, static_cast<std::uint64_t>(node.ext().borderOffsetBottom));
            h = fnvMix64(h, static_cast<std::uint64_t>((node.ext().borderConnect ? 1 : 0) | (node.ext().borderIgnoreMargin ? 2 : 0)));
            const bool chrome = (!node.ext().fillColor.empty() && node.ext().fillColor != "none") || !node.ext().hatchStyle.empty() ||
                                edgeHasStroke(node.ext().borderLeft) || edgeHasStroke(node.ext().borderRight) ||
                                edgeHasStroke(node.ext().borderTop) || edgeHasStroke(node.ext().borderBottom);
            if (chrome) {
                const std::int32_t padL = std::max<std::int32_t>(0, node.ext().borderOffsetLeft) +
                                          (node.ext().borderIgnoreMargin ? node.marginLeft : 0) + 120;
                const std::int32_t padR = std::max<std::int32_t>(0, node.ext().borderOffsetRight) +
                                          (node.ext().borderIgnoreMargin ? node.marginRight : 0) + 120;
                const std::int32_t padT = std::max<std::int32_t>(0, node.ext().borderOffsetTop) + 120;
                const std::int32_t padB = std::max<std::int32_t>(0, node.ext().borderOffsetBottom) + 120;
                unionHuBox(node.huX, node.huY, node.huW, node.huH, x - padL, y - padT, node.width + padL + padR,
                           std::max(node.height, 0) + padT + padB);
            }
        }
    }

    const bool childInBase = (node.kind == NodeKind::Cell) ? true : inObject;
    const int count = static_cast<int>(node.children.size());
    for (int i = 0; i < count; ++i) {
        Node& child = node.children[static_cast<std::size_t>(i)];
        const bool childIn = (childInBase && child.kind == NodeKind::Object && !child.treatAsChar) ? true : childInBase;
        computeNodeHuBounds(child, x, y, childIn, false, doc, pageSerial);
        unionHuBox(node.huX, node.huY, node.huW, node.huH, child.huX, child.huY, child.huW, child.huH);
        h = fnvMix64(h, static_cast<std::uint64_t>(child.x));
        h = fnvMix64(h, static_cast<std::uint64_t>(child.y));
        h = fnvMix64(h, child.contentHash);
    }
    for (int i = 0; i < count; ++i) {
        const Node& column = node.children[static_cast<std::size_t>(i)];
        if (column.kind != NodeKind::Column || !edgeHasStroke(column.ext().borderRight)) continue;
        const Node* next = nullptr;
        for (int j = i + 1; j < count; ++j) {
            const Node& other = node.children[static_cast<std::size_t>(j)];
            if (other.kind != NodeKind::Column || other.y != column.y || other.x <= column.x) continue;
            next = &other;
            break;
        }
        if (!next) continue;
        const std::int32_t left = column.x + column.width;
        const std::int32_t right = next->x;
        if (right < left) continue;
        const std::int32_t mid = x + (left + right) / 2;
        const std::int32_t top = y + column.y;
        const std::int32_t span = std::max(column.height, next->height);
        if (span <= 0) continue;
        const std::int32_t bleed = std::max<std::int32_t>(column.ext().borderRight.width, 100);
        unionHuBox(node.huX, node.huY, node.huW, node.huH, mid - bleed, top - bleed, bleed * 2, span + bleed * 2);
    }
    node.contentHash = node.spansLater ? 0ULL : (h == 0ULL ? 1ULL : h);
    node.huBoundsReady = true;
}

// 보기 표시가 줄 칸 밖으로 나가는 몫(HU). 칸이 그 그림을 잘라 내지 않게 늘린다.
struct ViewInkPad {
    std::int32_t left = 0;
    std::int32_t right = 0;
    std::int32_t top = 0;
    std::int32_t bottom = 0;
};

ViewInkPad viewInkPad(const Node& line, const model::Document* doc) {
    ViewInkPad pad;
    if (!doc) return pad;
    const std::int32_t em = std::max<std::int32_t>(line.fontSize > 0 ? line.fontSize : 1000, line.lineData().textHeight);
    if (paint_detail::lineHasProofInk(line, doc)) {
        // 줄 붙임표·줄 비움표는 왼쪽 여백, 자리 옮김표는 줄 위아래로 1.2em 까지 나간다.
        pad.left = em;
        pad.right = em / 2;
        pad.top = em * 13 / 10;
        pad.bottom = em;
    }
    if (viewMarks().control && line.story > 0) {
        Painter painter;
        painter.doc = doc;
        std::int32_t labels = 0;
        for (const Painter::LineLabel& label : painter.lineLabels(line, painter.collectMarks(line))) labels += label.width;
        pad.right = std::max(pad.right, labels + em);
    }
    if (viewMarks().paragraphMarks()) pad.right = std::max(pad.right, em);
    return pad;
}

struct FrameWalk {
    int pageIndex = 0;
    int pixelHeight = 0;
    float scale = 1;
    float baseScale = 1;
    FrameCull cull;
    const model::Document* doc = nullptr;
    const Node* pageNode = nullptr;
    std::vector<FrameDesc>* out = nullptr;
    std::int64_t pageSerial = 0;
    // 머리말·꼬리말 띠 안을 지나는 중이다. 한글처럼 그 안 개체는 글 뒤 층이고, 본문 글 뒤 개체보다는 위다.
    int bandDepth = 0;
    // 세로쓰기 틀(turned) 안이다. 노드 좌표는 돌린 틀이라 종이 좌표로 바꿔 자르고 칸을 놓는다.
    // 틀 원점은 종이의 (turnX, turnY), 가로 폭은 turnSpan 이다. 틀의 (x, y) 는 종이의 (turnX + turnSpan − y, turnY + x) 다.
    bool turned = false;
    std::int32_t turnX = 0;
    std::int32_t turnY = 0;
    std::int32_t turnSpan = 0;

    // 돌린 틀의 상자 → 종이 상자.
    void toPaper(std::int32_t& x, std::int32_t& y, std::int32_t& w, std::int32_t& h) const {
        if (!turned) return;
        const std::int32_t paperX = turnX + turnSpan - y - h;
        y = turnY + x;
        x = paperX;
        std::swap(w, h);
    }

    // 크기를 모르는 노드는 건너뛰지 않는다. 자식이 그 상자 밖에 있을 수 있다.
    bool misses(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h) const {
        if (!cull.on || w <= 0 || h <= 0) return false;
        toPaper(x, y, w, h);
        return x >= cull.x + cull.w || y >= cull.y + cull.h || x + w <= cull.x || y + h <= cull.y;
    }

    void emit(const Node& owner, int kind, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
              std::int32_t originX, std::int32_t originY, int layer, std::int64_t z, const std::vector<int>& path,
              bool inObject, bool baseOnly = false) {
        if (w <= 0 || h <= 0 || !out) return;
        std::int32_t paperX = x, paperY = y, paperW = w, paperH = h;
        toPaper(paperX, paperY, paperW, paperH);
        const int left = framePx(paperX, scale) - kFramePad;
        const int top = framePx(paperY, scale) - kFramePad;
        const int right = framePx(paperX + paperW, scale) + kFramePad;
        const int bottom = framePx(paperY + paperH, scale) + kFramePad;
        FrameDesc frame;
        frame.page = pageIndex;
        frame.layer = layer;
        const bool paraChrome = kind >= 100 && kind < 104;
        frame.rank = paraChrome ? 0 : (kind >= 3 ? 2 : (kind == 0 ? 1 : 0));
        frame.z = z;
        frame.kind = kind;
        frame.x = left;
        frame.y = top;
        frame.w = std::max(1, right - left);
        frame.h = std::max(1, bottom - top);
        frame.pixelHeight = pixelHeight;
        frame.inObject = inObject;
        frame.baseOnly = baseOnly;
        // 화면 자리를 다시 셀 때(다른 배율, 잘라 내기)는 종이 상자를 쓰고, 칸을 그릴 때는 틀 상자를 쓴다.
        frame.huX = paperX;
        frame.huY = paperY;
        frame.huW = paperW;
        frame.huH = paperH;
        frame.drawHuX = x;
        frame.drawHuY = y;
        frame.drawHuW = w;
        frame.drawHuH = h;
        frame.originX = originX;
        frame.originY = originY;
        frame.turned = turned;
        if (owner.contentHash != 0) {
            frame.contentHash = (kind >= 3) ? fnvMix64(owner.contentHash, static_cast<std::uint64_t>(kind))
                                            : owner.contentHash;
        }
        if (kind == 0) {
            frame.lineStory = owner.story;
            frame.lineParagraph = owner.paragraph;
            frame.lineStart = owner.start;
        }
        frame.path = path;
        if (frame.w > 65536 || frame.h > 65536) return;
        out->push_back(std::move(frame));
    }

    // 글 뒤는 글보다 아래, 글 앞은 글보다 위. 둘 다 이 개체의 칠보다는 위고, 다음 형제보다는 아래다.
    void visitKids(const Node& node, std::int32_t x, std::int32_t y, std::vector<int>& path, int layer, std::int64_t z,
                   std::int64_t step, bool inObject) {
        const int count = static_cast<int>(node.children.size());
        std::vector<std::int64_t> childZ(static_cast<std::size_t>(count), z);
        std::vector<std::int64_t> childStep(static_cast<std::size_t>(count), step > 0 ? step : 1);
        std::vector<char> nested(static_cast<std::size_t>(count), 0);
        if (inObject && step > 1) {
            std::vector<int> behind;
            std::vector<int> front;
            std::vector<int> captions;
            for (int i = 0; i < count; ++i) {
                const Node& child = node.children[static_cast<std::size_t>(i)];
                if (child.kind != NodeKind::Object || child.treatAsChar) continue;
                if (child.role == "caption") captions.push_back(i);
                else if (frameBehind(child)) behind.push_back(i);
                else front.push_back(i);
            }
            const auto byZ = [&](int a, int b) {
                return node.children[static_cast<std::size_t>(a)].ext().obj().zOrder <
                       node.children[static_cast<std::size_t>(b)].ext().obj().zOrder;
            };
            std::stable_sort(behind.begin(), behind.end(), byZ);
            std::stable_sort(front.begin(), front.end(), byZ);
            std::stable_sort(captions.begin(), captions.end(), byZ);
            const int parts = static_cast<int>(behind.size() + front.size() + captions.size()) + 1;
            std::int64_t unit = (step - 1) / parts;
            if (unit < 1) unit = 1;
            const std::int64_t slot = unit > 1 ? unit - 1 : 1;
            for (int r = 0; r < static_cast<int>(behind.size()); ++r) {
                const int index = behind[static_cast<std::size_t>(r)];
                childZ[static_cast<std::size_t>(index)] = z + 1 + static_cast<std::int64_t>(r) * unit;
                childStep[static_cast<std::size_t>(index)] = slot;
                nested[static_cast<std::size_t>(index)] = 1;
            }
            const std::int64_t textAt = z + 1 + static_cast<std::int64_t>(behind.size()) * unit;
            for (int i = 0; i < count; ++i) {
                if (nested[static_cast<std::size_t>(i)]) continue;
                childZ[static_cast<std::size_t>(i)] = textAt;
                childStep[static_cast<std::size_t>(i)] = slot;
            }
            for (int r = 0; r < static_cast<int>(front.size()); ++r) {
                const int index = front[static_cast<std::size_t>(r)];
                childZ[static_cast<std::size_t>(index)] = textAt + unit * static_cast<std::int64_t>(r + 1);
                childStep[static_cast<std::size_t>(index)] = slot;
                nested[static_cast<std::size_t>(index)] = 1;
            }
            const std::int64_t captionAt = textAt + unit * static_cast<std::int64_t>(front.size() + 1);
            for (int r = 0; r < static_cast<int>(captions.size()); ++r) {
                const int index = captions[static_cast<std::size_t>(r)];
                childZ[static_cast<std::size_t>(index)] = captionAt + unit * static_cast<std::int64_t>(r);
                childStep[static_cast<std::size_t>(index)] = slot;
                nested[static_cast<std::size_t>(index)] = 1;
            }
        }
        for (int i = 0; i < count; ++i) {
            path.push_back(i);
            const bool childIn = nested[static_cast<std::size_t>(i)] ? true : inObject;
            visit(node.children[static_cast<std::size_t>(i)], x, y, path, layer, childZ[static_cast<std::size_t>(i)],
                  childStep[static_cast<std::size_t>(i)], childIn, false);
            path.pop_back();
        }
        for (int i = 0; i < count; ++i) {
            const Node& column = node.children[static_cast<std::size_t>(i)];
            if (column.kind != NodeKind::Column || !edgeHasStroke(column.ext().borderRight)) continue;
            const Node* next = nullptr;
            for (int j = i + 1; j < count; ++j) {
                const Node& other = node.children[static_cast<std::size_t>(j)];
                if (other.kind != NodeKind::Column || other.y != column.y || other.x <= column.x) continue;
                next = &other;
                break;
            }
            if (!next) continue;
            const std::int32_t left = column.x + column.width;
            const std::int32_t right = next->x;
            if (right < left) continue;
            const std::int32_t mid = x + (left + right) / 2;
            const std::int32_t top = y + column.y;
            const std::int32_t span = std::max(column.height, next->height);
            if (span <= 0) continue;
            const std::int32_t bleed = std::max<std::int32_t>(column.ext().borderRight.width, 100);
            const std::int32_t bw = bleed * 2;
            const std::int32_t halfW = std::max<std::int32_t>(column.ext().borderRight.width / 2, 1);
            path.push_back(i);
            int stripIdx = 0;
            for (const auto& seg : columnRuleSpans(pageNode, node, x, y, mid, halfW, top, top + span)) {
                std::int32_t s0 = seg.first;
                while (s0 < seg.second) {
                    const std::int32_t s1 = std::min(seg.second, s0 + kTableBorderStripHu);
                    const std::int32_t bx = mid - bleed;
                    const std::int32_t by = s0 - bleed;
                    const std::int32_t bh = (s1 - s0) + bleed * 2;
                    if (!misses(bx, by, bw, bh)) {
                        emit(column, 3 + stripIdx, bx, by, bw, bh, mid, s0, layer,
                             childZ[static_cast<std::size_t>(i)] + childStep[static_cast<std::size_t>(i)] - 1, path,
                             inObject);
                    }
                    s0 = s1;
                    ++stripIdx;
                }
            }
            path.pop_back();
        }
    }

    void visit(const Node& node, std::int32_t ox, std::int32_t oy, std::vector<int>& path, int layer, std::int64_t z,
               std::int64_t step, bool inObject, bool isPage, bool force = false) {
        // 틀을 바꾸면 나올 때 되돌린다. 돌린 틀은 부모 자리가 원점이고, 세운 개체 상자는 다시 종이 그대로다.
        struct TurnScope {
            FrameWalk& walk;
            bool turned;
            std::int32_t x, y, span;
            explicit TurnScope(FrameWalk& w)
                : walk(w), turned(w.turned), x(w.turnX), y(w.turnY), span(w.turnSpan) {}
            ~TurnScope() {
                walk.turned = turned;
                walk.turnX = x;
                walk.turnY = y;
                walk.turnSpan = span;
            }
        } turnScope(*this);
        if (node.turned != 0) {
            turned = true;
            turnX = ox;
            turnY = oy;
            turnSpan = node.turnSpan > 0 ? node.turnSpan : (pageNode ? pageNode->width : 0);
            ox = 0;
            oy = 0;
        }
        if (node.upright && turned) {
            // 세운 개체. 틀의 상자 (x, y, 너비, 높이) 는 종이에서 (turnX + turnSpan − y − 높이, turnY + x) 부터다.
            const std::int32_t bx = ox + node.x;
            const std::int32_t by = oy + node.y;
            const std::int32_t paperX = turnX + turnSpan - by - node.height;
            const std::int32_t paperY = turnY + bx;
            turned = false;
            for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
                path.push_back(i);
                visit(node.children[static_cast<std::size_t>(i)], paperX, paperY, path, layer, z, step, true, false,
                      force);
                path.pop_back();
            }
            return;
        }
        if (!isPage && !force && node.huBoundsReady && misses(node.huX, node.huY, node.huW, node.huH)) {
            return;
        }
        struct BandScope {
            int& depth;
            bool on;
            BandScope(int& d, bool o) : depth(d), on(o) { if (on) ++depth; }
            ~BandScope() { if (on) --depth; }
        } bandScope(bandDepth, node.pageBand);
        const std::int32_t x = ox + node.x + (node.kind == NodeKind::Paragraph ? node.marginLeft : 0);
        const std::int32_t y = oy + node.y;
        int nextLayer = layer;
        std::int64_t nextZ = z;
        std::int64_t nextStep = step > 0 ? step : 1;
        bool nextIn = inObject;
        if (!isPage && node.kind == NodeKind::Line) {
            // 보기 표시가 줄 밖으로 나가는 몫: 교정 부호(여백의 괄호·줄 위아래 표시)와 조판 부호 이름표(줄 끝 넘침).
            const ViewInkPad pad = viewInkPad(node, doc);
            if (node.huBoundsReady) {
                const LineContent& box = node.lineData();
                const std::int32_t bx = box.lineBoxX - pad.left;
                const std::int32_t by = box.lineBoxY - pad.top;
                const std::int32_t bw = box.lineBoxW + pad.left + pad.right;
                const std::int32_t bh = box.lineBoxH + pad.top + pad.bottom;
                if (!misses(bx, by, bw, bh)) emit(node, 0, bx, by, bw, bh, x, y, layer, z, path, inObject);
                bool hasInlineObj = false;
                for (const Node& child : node.children) {
                    if (child.kind == NodeKind::Object && child.treatAsChar) {
                        hasInlineObj = true;
                        break;
                    }
                }
                if (!hasInlineObj) return;
                std::vector<InlineBorder> borders;
                const bool seated = treatAsCharBorders(node, doc, borders);
                for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
                    const Node& child = node.children[static_cast<std::size_t>(i)];
                    if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
                    std::int32_t childOx = x + child.marginLeft;
                    if (seated && borders[static_cast<std::size_t>(i)].ok)
                        childOx = x + borders[static_cast<std::size_t>(i)].x + child.marginLeft - child.x;
                    path.push_back(i);
                    visit(child, childOx, y + child.marginTop, path, layer, z, step, true, false, true);
                    path.pop_back();
                }
                return;
            }
            std::int32_t top = y;
            std::int32_t height = node.height > 0 ? node.height : node.lineData().textHeight;
            if (node.lineData().auditRise > 0) {
                top -= node.lineData().auditRise;
                height += node.lineData().auditRise;
            }
            height += std::max<std::int32_t>(0, node.lineData().auditBelow);
            std::vector<InlineBorder> borders;
            const bool seated = treatAsCharBorders(node, doc, borders);
            std::int32_t extraLeft = 0;
            std::int32_t extraRight = 0;
            for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
                const Node& child = node.children[static_cast<std::size_t>(i)];
                if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
                const std::int32_t moved =
                    seated && borders[static_cast<std::size_t>(i)].ok ? borders[static_cast<std::size_t>(i)].x - child.x : 0;
                const std::int32_t objL = child.x + moved + child.marginLeft;
                const std::int32_t objR = objL + child.width + child.marginRight;
                if (objL < 0) extraLeft = std::max(extraLeft, -objL);
                if (objR > node.width) extraRight = std::max(extraRight, objR - node.width);
                for (const Node& cap : child.children) {
                    if (cap.role != "caption") continue;
                    const std::int32_t capL = objL + cap.x;
                    const std::int32_t capR = capL + cap.width;
                    if (capL < 0) extraLeft = std::max(extraLeft, -capL);
                    if (capR > node.width) extraRight = std::max(extraRight, capR - node.width);
                }
            }
            // 문단 번호·글머리는 줄 왼쪽 밖에 매달린다. 칸이 그 글자를 잘라 내지 않게 늘린다.
            const std::int32_t slide = std::max<std::int32_t>(0, node.indent);
            std::int32_t coveredAbove = std::max<std::int32_t>(0, node.lineData().auditRise);
            std::int32_t coveredBelow =
                (node.height > 0 ? node.height : node.lineData().textHeight) + std::max<std::int32_t>(0, node.lineData().auditBelow);
            if (const HeadingRun* head = lineGlyphs(node).heading.get()) {
                const std::int32_t left = slide + head->x;
                std::int32_t right = left + std::max(head->width, 0);
                if (!head->advances.empty()) {
                    right = left;
                    for (const std::int32_t width : head->advances) right += width;
                }
                if (left < 0) extraLeft = std::max(extraLeft, -left);
                if (right > node.width) extraRight = std::max(extraRight, right - node.width);
            }
            for (const Node& child : node.children) {
                if (!child.headingMark || child.kind != NodeKind::Object) continue;
                const std::int32_t left = slide + child.x;
                const std::int32_t right = left + child.width;
                if (left < 0) extraLeft = std::max(extraLeft, -left);
                if (right > node.width) extraRight = std::max(extraRight, right - node.width);
                if (child.y < -coveredAbove) {
                    const std::int32_t more = -child.y - coveredAbove;
                    top -= more;
                    height += more;
                    coveredAbove += more;
                }
                const std::int32_t bottom = child.y + std::max(child.height, 0);
                if (bottom > coveredBelow) {
                    height += bottom - coveredBelow;
                    coveredBelow = bottom;
                }
            }
            extraLeft = std::max(extraLeft, pad.left);
            extraRight = std::max(extraRight, pad.right);
            top -= pad.top;
            height += pad.top + pad.bottom;
            if (misses(x - extraLeft, top, node.width + extraLeft + extraRight, height)) return;
            emit(node, 0, x - extraLeft, top, node.width + extraLeft + extraRight, height, x, y, layer, z, path,
                 inObject);
            // 글자처럼 넣은 개체는 줄과 칸을 나눈다. 펜은 바깥 여백의 바깥이고, 테두리는 그 안쪽이다.
            for (int i = 0; i < static_cast<int>(node.children.size()); ++i) {
                const Node& child = node.children[static_cast<std::size_t>(i)];
                if (child.kind != NodeKind::Object || !child.treatAsChar) continue;
                std::int32_t childOx = x + child.marginLeft;
                if (seated && borders[static_cast<std::size_t>(i)].ok)
                    childOx = x + borders[static_cast<std::size_t>(i)].x + child.marginLeft - child.x;
                path.push_back(i);
                visit(child, childOx, y + child.marginTop, path, layer, z, step, true, false, true);
                path.pop_back();
            }
            return;
        }
        if (!isPage && node.kind == NodeKind::Object) {
            if (node.role == "pageFill" || node.role == "pageBorder") {
                // 미리보기는 쪽 전체가 아니라 이 칸을 구워 붙인다. 쪽만 그리면 테두리가 빠진다.
                const bool fill = node.role == "pageFill";
                std::int32_t bleed = 0;
                if (!fill) {
                    bleed = std::max({node.ext().borderLeft.width, node.ext().borderRight.width, node.ext().borderTop.width,
                                      node.ext().borderBottom.width, static_cast<std::int32_t>(0)});
                    bleed = bleed / 2 + 40;
                }
                const std::int32_t boxW = node.width + bleed * 2;
                const std::int32_t boxH = node.height + bleed * 2;
                const std::int32_t boxX = x - bleed;
                const std::int32_t boxY = y - bleed;
                const int chromeLayer = fill ? 0 : static_cast<int>(PaintLayer::Border);
                const std::int64_t chromeZ = fill ? 0 : (kFrameZSlot - 1);
                int strip = 0;
                for (std::int32_t sy = 0; sy < boxH; sy += kTableBorderStripHu) {
                    const std::int32_t sh = std::min(kTableBorderStripHu, boxH - sy);
                    for (std::int32_t sx = 0; sx < boxW; sx += kTableBorderStripHu) {
                        const std::int32_t sw = std::min(kTableBorderStripHu, boxW - sx);
                        if (misses(boxX + sx, boxY + sy, sw, sh)) continue;
                        const int kind = (fill ? 500 : 700) + strip;
                        emit(node, kind, boxX + sx, boxY + sy, sw, sh, x, y, chromeLayer, chromeZ, path, false);
                        ++strip;
                    }
                }
                return;
            }
            const std::int32_t bleed = tableBorderBleed(node);
            if (!force && !node.huBoundsReady &&
                misses(x - bleed, y - bleed, node.width + bleed * 2, node.height + bleed * 2)) {
                return;
            }
            std::int64_t myZ = z;
            std::int64_t myStep = step > 0 ? step : 1;
            // 쪽 위의 개체만 문서 zOrder를 쓴다. 묶음 안에서는 부모가 준 칸이 순서다.
            if (!inObject && !node.treatAsChar) {
                const std::int64_t zo = node.ext().obj().zOrder;
                const std::int64_t clamped = zo < -1000000 ? -1000000 : (zo > 1000000 ? 1000000 : zo);
                const std::int64_t serial = node.ext().obj().objectPageSerial >= 0 ? node.ext().obj().objectPageSerial : pageSerial++;
                myZ = (clamped + 1000000LL) * kFrameZGap + serial * kFrameZSlot;
                // 띠 개체는 글 뒤 층의 맨 위 칸을 쓴다. 문서 zOrder 가 커도 본문 글 앞 개체를 덮지 않는다.
                if (bandDepth > 0) myZ = (2000001LL + clamped + 1000000LL) * kFrameZGap + serial * kFrameZSlot;
                myStep = kFrameZSlot;
            }
            nextLayer = (bandDepth > 0 || frameBehind(node)) ? 0 : 2;
            if (node.role != "table" || tableBoxHasInk(node)) {
                if (force || !misses(x - bleed, y - bleed, node.width + bleed * 2, node.height + bleed * 2)) {
                    emit(node, 1, x, y, node.width, node.height, x, y, nextLayer, myZ, path, inObject,
                         pictureFitsBaseFrame(node, baseScale) || pictureDrawsOnScreen(node));
                }
            }
            visitKids(node, x, y, path, nextLayer, myZ, myStep, true);
            if (node.role == "table") {
                bool anyBorder = false;
                std::int32_t maxY = node.height;
                std::vector<std::int32_t> rowYs = {0, node.height};
                for (const Node& child : node.children) {
                    if (child.kind != NodeKind::Cell) continue;
                    if (cellHasAnyBorder(child)) anyBorder = true;
                    const std::int32_t cy0 = std::max<std::int32_t>(0, child.y);
                    const std::int32_t cy1 = std::max<std::int32_t>(0, child.y + child.height);
                    maxY = std::max(maxY, cy1);
                    rowYs.push_back(cy0);
                    rowYs.push_back(cy1);
                }
                if (anyBorder && maxY > 0 && node.width > 0) {
                    rowYs.push_back(maxY);
                    for (std::int32_t sy = kTableBorderStripHu; sy < maxY; sy += kTableBorderStripHu) {
                        rowYs.push_back(sy);
                    }
                    std::sort(rowYs.begin(), rowYs.end());
                    rowYs.erase(std::unique(rowYs.begin(), rowYs.end()), rowYs.end());
                    // 가로는 표 너비 그대로 두면 확대했을 때 칸이 2048을 넘어 다시 굽지 못한다.
                    // 그러면 100%에 실선으로 뭉갠 칸을 늘려 보여 주고 끝난다. 세로와 같이 잘라 화면 칸만 다시 그린다.
                    std::vector<std::int32_t> colXs = {0, node.width};
                    for (std::int32_t sx = kTableBorderStripHu; sx < node.width; sx += kTableBorderStripHu) {
                        colXs.push_back(sx);
                    }
                    std::sort(colXs.begin(), colXs.end());
                    colXs.erase(std::unique(colXs.begin(), colXs.end()), colXs.end());
                    const std::int64_t borderZ = myZ + (myStep > 1 ? myStep - 1 : 0);
                    std::int32_t s0 = 0;
                    int stripIdx = 0;
                    for (std::size_t ri = 1; ri < rowYs.size(); ++ri) {
                        const std::int32_t cand = rowYs[ri];
                        const bool isEnd = (ri + 1 == rowYs.size());
                        if (!isEnd && cand - s0 < kTableBorderStripHu) continue;
                        if (!isEnd) {
                            bool straddles = false;
                            for (const Node& child : node.children) {
                                if (child.kind != NodeKind::Cell) continue;
                                if (node.ext().cellSpacing <= 0 && !cellHasDiagonalStroke(child)) continue;
                                if (child.y < cand && child.y + child.height > cand) {
                                    straddles = true;
                                    break;
                                }
                            }
                            if (straddles) continue;
                        }
                        const std::int32_t s1 = cand;
                        const std::int32_t by = y + s0 - bleed;
                        const std::int32_t bh = (s1 - s0) + bleed * 2;
                        std::int32_t x0 = 0;
                        for (std::size_t ci = 1; ci < colXs.size(); ++ci) {
                            const std::int32_t x1 = colXs[ci];
                            const std::int32_t bx = x + x0 - bleed;
                            const std::int32_t bw = (x1 - x0) + bleed * 2;
                            if (!misses(bx, by, bw, bh)) {
                                emit(node, 3 + stripIdx, bx, by, bw, bh, x, y, nextLayer, borderZ, path, inObject);
                            }
                            x0 = x1;
                            ++stripIdx;
                        }
                        s0 = s1;
                    }
                }
            }
            return;
        }
        if (!isPage && node.kind == NodeKind::Cell) {
            if (!node.huBoundsReady && misses(x, y, node.width, node.height)) return;
            nextIn = true;
            if (cellHasFillInk(node) && !misses(x, y, node.width, node.height)) {
                emit(node, 2, x, y, node.width, node.height, x, y, layer, z, path, inObject);
            }
        } else if (!isPage && !node.huBoundsReady && misses(x, y, node.width, node.height)) {
            return;
        }
        if (!isPage && node.kind == NodeKind::Paragraph) {
            std::int32_t left = 0, top = 0, right = 0, bottom = 0;
            paragraphBorderBox(node, ox, oy, left, top, right, bottom);
            const bool face = (!node.ext().fillColor.empty() && node.ext().fillColor != "none") || !node.ext().hatchStyle.empty();
            const bool side = !node.ext().borderLeft.type.empty() && node.ext().borderLeft.type != "NONE";
            const bool any = face || side ||
                             (!node.ext().borderRight.type.empty() && node.ext().borderRight.type != "NONE") ||
                             (!node.ext().borderTop.type.empty() && node.ext().borderTop.type != "NONE") ||
                             (!node.ext().borderBottom.type.empty() && node.ext().borderBottom.type != "NONE");
            if (any && right > left && bottom > top) {
                const std::int32_t bleed =
                    std::max({node.ext().borderLeft.width, node.ext().borderRight.width, node.ext().borderTop.width,
                              node.ext().borderBottom.width, static_cast<std::int32_t>(80)}) +
                    40;
                int join = 0;
                if (!path.empty() && pageNode) {
                    const Node* parent = pageNode;
                    bool ok = true;
                    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
                        const int idx = path[i];
                        if (idx < 0 || idx >= static_cast<int>(parent->children.size())) {
                            ok = false;
                            break;
                        }
                        parent = &parent->children[static_cast<std::size_t>(idx)];
                    }
                    if (ok) join = paragraphBorderJoin(*parent, path.back());
                }
                if (!misses(left - bleed, top - bleed, (right - left) + bleed * 2, (bottom - top) + bleed * 2)) {
                    emit(node, 100 + join, left - bleed, top - bleed, (right - left) + bleed * 2,
                         (bottom - top) + bleed * 2, ox, oy, layer, z, path, inObject);
                }
            }
        }
        visitKids(node, x, y, path, nextLayer, nextZ, nextStep, nextIn);
    }
};

const Node* frameNode(const Node& page, const std::vector<int>& path) {
    const Node* node = &page;
    for (const int index : path) {
        if (index < 0 || index >= static_cast<int>(node->children.size())) return nullptr;
        node = &node->children[static_cast<std::size_t>(index)];
    }
    return node;
}

bool lineHasGlyphSource(const Node& line) {
    if (lineGlyphs(line).heading && !lineGlyphs(line).heading->text.empty()) return true;
    for (const Node& child : line.children) {
        if (child.kind == NodeKind::Object) return true;
    }
    for (const LineRun& run : lineGlyphs(line).runs) {
        if (run.kind == RunKind::Object || run.textEnd > run.textBegin) return true;
    }
    return false;
}

bool lineHasBitmapInk(const Node& line, const model::Document* doc) {
    if (line.lineData().auditLine) return true;
    if (paint_detail::lineHasProofInk(line, doc)) return true;
    if (paint_detail::lineHasMemoRange(line, doc)) return true;
    const auto set = [](const std::string* name, const char* none) { return name && *name != none; };
    for (const LineRun& run : lineGlyphs(line).runs) {
        if (run.kind == RunKind::Object) continue;
        if (set(run.style->markpen, "none")) return true;
        if (run.style->track != 0) return true;  // 변경 추적 밑줄·취소선
        if (run.kind == RunKind::Tab && (line.lineData().deferTabs || set(run.style->tabLeader, "NONE"))) return true;
        if (!doc) continue;
        const detail::CharPaint paint = detail::charPaint(*doc, run.charShapeId);
        if (!paint.fillColor.empty() || !paint.hatchStyle.empty()) return true;
        if (!paint.shade.empty() && paint.shade != "none") return true;
        if (paint.bitmapDeco || !paint.underline.empty() || paint.strikeout) return true;
    }
    for (const Node& slot : line.children) {
        if (slot.headingMark && slot.kind == NodeKind::Object) return true;
        if (!slot.ext().fillColor.empty() && slot.ext().fillColor != "none") return true;
        if (!slot.ext().hatchStyle.empty()) return true;
    }
    return false;
}

}  // namespace

namespace {
std::atomic<int> gZoomedFrameSide{0};
std::atomic<std::int64_t> gZoomedFramePixels{0};
}  // namespace

void setZoomedFrameLimit(int side, std::int64_t pixels) {
    gZoomedFrameSide.store(std::max(0, side));
    gZoomedFramePixels.store(std::max<std::int64_t>(0, pixels));
}

bool canBakeFrame(const FrameDesc& frame) {
    if (frame.w <= 0 || frame.h <= 0 || frame.w > 8192 || frame.h <= 0 || frame.h > 8192) return false;
    if (frame.baseOnly && frame.percent > 100) return false;
    if (frame.percent > 100 && frame.kind != 0) {
        const int side = gZoomedFrameSide.load();
        if (side > 0 && (frame.w > side || frame.h > side)) return false;
        const std::int64_t pixels = gZoomedFramePixels.load();
        if (pixels > 0 && static_cast<std::int64_t>(frame.w) * static_cast<std::int64_t>(frame.h) > pixels) return false;
    }
    return true;
}

void placeZoomedFrame(int dstX, int dstY, int dstPixelHeight, int srcPixelHeight, int srcW, int srcH, int& x, int& y,
                      int& w, int& h) {
    x = dstX;
    y = dstY;
    w = std::max(1, srcW);
    h = std::max(1, srcH);
    if (dstPixelHeight <= 0 || srcPixelHeight <= 0 || srcW <= 0 || srcH <= 0) return;
    const double scale = static_cast<double>(dstPixelHeight) / static_cast<double>(srcPixelHeight);
    w = std::max(1, static_cast<int>(std::lround(static_cast<double>(srcW) * scale)));
    h = std::max(1, static_cast<int>(std::lround(static_cast<double>(srcH) * scale)));
    const double sx = static_cast<double>(w) / static_cast<double>(srcW);
    const double sy = static_cast<double>(h) / static_cast<double>(srcH);
    x = static_cast<int>(std::lround(static_cast<double>(dstX + kFramePad) - static_cast<double>(kFramePad) * sx));
    y = static_cast<int>(std::lround(static_cast<double>(dstY + kFramePad) - static_cast<double>(kFramePad) * sy));
}

void updatePageHuBounds(Node& page, const model::Document* doc) {
    std::int32_t pageSerial = 0;
    computeNodeHuBounds(page, 0, 0, false, true, doc, pageSerial);
}

void ensurePageHuBounds(const Node& page, const model::Document* doc) {
    if (page.huBoundsReady && (page.spansLater || page.contentHash != 0)) return;
    PageFillGuard guard;
    if (page.huBoundsReady && (page.spansLater || page.contentHash != 0)) return;
    updatePageHuBounds(const_cast<Node&>(page), doc);
}

std::vector<FrameDesc> collectFrames(const Node& page, int pageIndex, int pixelHeight, const FrameCull& cull,
                                   const model::Document* doc) {
    PageFillGuard guard;
    std::vector<FrameDesc> frames;
    if (pixelHeight < 32 || page.width <= 0 || page.height <= 0) return frames;
    ensurePageHuBounds(page, doc);
    FrameWalk walk;
    walk.pageIndex = pageIndex;
    walk.pixelHeight = pixelHeight;
    walk.scale = static_cast<float>(pixelHeight) / static_cast<float>(page.height);
    const int basePixelHeight = pagePixelHeight(page.height, 100);
    walk.baseScale = static_cast<float>(basePixelHeight) / static_cast<float>(page.height);
    walk.cull = cull;
    walk.doc = doc;
    walk.pageNode = &page;
    walk.out = &frames;
    std::vector<int> path;
    walk.visit(page, 0, 0, path, 1, 0, kFrameZSlot, false, true);
    return frames;
}

namespace {

// 돌린 틀의 비트맵을 시계 방향으로 돌린다. 종이 칸 (x, y) 는 틀의 (y, w − 1 − x) 다.
std::vector<std::uint8_t> turnedPixels(const Bitmap& source, int w, int h) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0);
    if (source.px.size() < out.size() || source.width != h || source.height != w) return out;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* from = source.px.data() + (static_cast<std::size_t>(w - 1 - x) * h + y) * 4u;
            std::memcpy(out.data() + (static_cast<std::size_t>(y) * w + x) * 4u, from, 4);
        }
    }
    return out;
}

// 돌린 틀에서 적은 글자 자리를 종이 칸으로 옮긴다. 틀의 점 (x, y) 는 칸의 (w − y, x) 다.
// 눕힌 글자는 펜을 옮기고 마스크를 돌려 찍게 한다. 세운 글자는 틀에서 돌린 만큼 다시 돌아와 마스크를 그대로 찍는다.
void turnFrameGlyphs(std::vector<FrameGlyph>& glyphs, int w) {
    const float right = static_cast<float>(w);
    for (FrameGlyph& glyph : glyphs) {
        if (glyph.upright) {
            const float cx = right - glyph.turnCy;
            const float cy = glyph.turnCx;
            const float penX = cx + (glyph.penX - glyph.turnCx) - glyph.turnDy;
            const float penY = cy + (glyph.penY - glyph.turnCy) + glyph.turnDx;
            glyph.penX = penX;
            glyph.penY = penY;
            glyph.turn = 0;
        } else {
            const float penX = right - glyph.penY;
            glyph.penY = glyph.penX;
            glyph.penX = penX;
            glyph.turn = 1;
        }
    }
}

}  // namespace

PageBitmap renderFrame(const model::Document& doc, const Node& page, const FrameDesc& frame) {
    PageBitmap bitmap;
    const Node* node = frameNode(page, frame.path);
    if (!node || !canBakeFrame(frame) || frame.pixelHeight < 32 || page.height <= 0) return bitmap;
    Painter painter;
    painter.doc = &doc;
    painter.pageIndex = frame.page;
    painter.pass = PaintLayer::All;
    painter.scale = static_cast<float>(frame.pixelHeight) / static_cast<float>(page.height);
    // 세로쓰기 칸은 돌린 틀(가로 = 종이 세로)로 그린 뒤 시계 방향으로 돌린다. 줄 자리 캐시는 쓰지 않는다.
    const bool turned = frame.turned;
    painter.bitmap.width = turned ? frame.h : frame.w;
    painter.bitmap.height = turned ? frame.w : frame.h;
    painter.bitmap.clipR = painter.bitmap.width;
    painter.bitmap.clipB = painter.bitmap.height;
    const std::int32_t padHu = static_cast<std::int32_t>(std::lround(static_cast<double>(kFramePad) / painter.scale));
    painter.framePadHu = padHu;
    std::vector<GlyphSpot> spots;
    bool placed = !turned && takeLinePlaces(frame, spots);
    // 글자를 다 지운 줄은 이전에 적어 둔 글자 자리를 다시 그리지 않는다.
    if (placed && frame.kind == 0 && !lineHasGlyphSource(*node)) {
        placed = false;
        spots.clear();
    }
    std::vector<GlyphSpot> recorded;
    if (!placed && !turned) painter.placeSink = &recorded;
    if (placed) painter.skipGlyphs = true;
    painter.skipInlineObjects = true;
    painter.seedObjectHosts(page, frame.path);
    const bool clippedByAncestor =
        painter.seedAncestorClips(page, frame.path, padHu, frame.drawHuX, frame.drawHuY, frame.drawHuW,
                                 frame.drawHuH);
    FrameGlyphs captured;
    if (!placed && screenVectors() && !clippedByAncestor) painter.glyphSink = &captured;
    // 개체 칸의 그림은 굽지 않고 화면이 텍스처로 그린다. 조상이 자르는 범위는 그림마다 적어 둔다.
    std::vector<FramePicture> pictures;
    if (frame.kind == 1 && screenVectors() && !turned) painter.pictureSink = &pictures;
    // 표와 쪽 테두리 칸은 선을 칠하지 않고 적어 두었다가 화면 셰이더가 그린다. 대각선은 비트맵에 남는다.
    std::vector<BorderPiece> borders;
    const bool tableStroke = (frame.kind >= 3 && frame.kind < 100 && node->kind != NodeKind::Column) || frame.kind >= 700;
    if (tableStroke && bordersOnScreen() && !turned) painter.borderSink = &borders;
    const bool needPaintPiece = !placed || frame.kind != 0 || lineHasBitmapInk(*node, &doc) || clippedByAncestor;
    if (needPaintPiece) {
        std::int32_t stripY0 = 0;
        std::int32_t stripY1 = -1;
        std::int32_t stripX0 = 0;
        std::int32_t stripX1 = -1;
        if (frame.kind >= 3 && node->role == "table") {
            const std::int32_t bleed = tableBorderBleed(*node);
            stripY0 = std::max<std::int32_t>(0, (frame.drawHuY - frame.originY) + bleed);
            stripY1 = stripY0 + std::max<std::int32_t>(0, frame.drawHuH - bleed * 2);
            stripX0 = std::max<std::int32_t>(0, (frame.drawHuX - frame.originX) + bleed);
            stripX1 = stripX0 + std::max<std::int32_t>(0, frame.drawHuW - bleed * 2);
        } else if (frame.kind >= 3 && node->kind == NodeKind::Column) {
            const std::int32_t bleed = std::max<std::int32_t>(node->ext().borderRight.width, 100);
            stripY0 = 0;
            stripY1 = std::max<std::int32_t>(0, frame.drawHuH - bleed * 2);
        }
        painter.paintPiece(*node, frame.kind, padHu + frame.originX - frame.drawHuX, padHu + frame.originY - frame.drawHuY,
                           frame.inObject, stripY0, stripY1, stripX0, stripX1);
    }
    if (!placed && !turned) {
        spotsToPage(recorded, frame, padHu);
        keepLinePlaces(frame, std::move(recorded));
    }
    std::vector<GlyphSpot> local = placed ? spotsInFrame(spots, frame, padHu) : std::vector<GlyphSpot>{};
    if (placed && screenVectors() && !clippedByAncestor) {
        painter.skipGlyphs = false;
        painter.glyphSink = &captured;
        painter.replaySpots(local);
        painter.glyphSink = nullptr;
        storeFrameGlyphs(frame.id, std::move(captured));
    } else if (placed) {
        painter.skipGlyphs = false;
        painter.replaySpots(local);
        dropFrameGlyphs(frame.id);
    } else if (painter.glyphSink) {
        painter.glyphSink = nullptr;
        if (turned) turnFrameGlyphs(captured.glyphs, frame.w);
        storeFrameGlyphs(frame.id, std::move(captured));
    } else {
        dropFrameGlyphs(frame.id);
    }
    painter.pictureSink = nullptr;
    const bool hasPictures = !pictures.empty();
    storeFramePictures(frame.id, std::move(pictures));
    painter.borderSink = nullptr;
    storeFrameBorders(frame.id, painter.scale, std::move(borders));
    bitmap.width = frame.w;
    bitmap.height = frame.h;
    if (turned) {
        bitmap.rgba = turnedPixels(painter.bitmap, frame.w, frame.h);
    } else {
        bitmap.rgba = std::move(painter.bitmap.px);
    }
    bitmap.incomplete = painter.pictureMissed;
    // 그림만 있는 칸은 비트맵이 다 투명하다. 올리지 않게 비운다. 크기는 남겨 화면 배율을 잰다.
    if (hasPictures) {
        bool ink = false;
        for (std::size_t i = 3; i < bitmap.rgba.size(); i += 4) {
            if (bitmap.rgba[i] != 0) {
                ink = true;
                break;
            }
        }
        if (!ink) {
            bitmap.rgba.clear();
            bitmap.rgba.shrink_to_fit();
        }
    }
    return bitmap;
}

// 윤곽은 굽지 않고, 이 줄의 전진폭과 글자 자리만 남긴다.
void writeLinePlace(const model::Document& doc, const Node& page, const FrameDesc& frame, int pixelH) {
    const Node* node = frameNode(page, frame.path);
    if (!node || frame.w <= 0 || frame.h <= 0 || pixelH < 32 || page.height <= 0) {
        keepLinePlaces(frame, {});
        return;
    }
    // 글 칸을 채우기 전에 자리를 남기면 본문과 문단 번호가 그 줄에 고정된다.
    if (node->spansLater) return;
    Painter painter;
    painter.doc = &doc;
    painter.pageIndex = frame.page;
    painter.pass = PaintLayer::All;
    painter.scale = static_cast<float>(pixelH) / static_cast<float>(page.height);
    painter.bitmap.width = 1;
    painter.bitmap.height = 1;
    painter.bitmap.px.assign(4, 0);
    painter.bitmap.clipR = 1;
    painter.bitmap.clipB = 1;
    painter.skipGlyphs = true;
    painter.placesOnly = true;
    painter.skipInlineObjects = true;
    painter.seedObjectHosts(page, frame.path);
    std::vector<GlyphSpot> spots;
    painter.placeSink = &spots;
    const std::int32_t padHu = static_cast<std::int32_t>(std::lround(static_cast<double>(kFramePad) / painter.scale));
    painter.paintPiece(*node, frame.kind, padHu + frame.originX - frame.drawHuX, padHu + frame.originY - frame.drawHuY,
                       frame.inObject);
    spotsToPage(spots, frame, padHu);
    keepLinePlaces(frame, std::move(spots));
}

bool linePlaceKept(const FrameDesc& frame) {
    SharedLinePlaces places;
    return findLinePlaces(frame, places);
}

int cachePageLines(const model::Document& doc, const Node& page, int pageIndex, int percent) {
    if (page.height <= 0) return 0;
    const int zoom = percent > 0 ? percent : 100;
    const int pixelH = pagePixelHeight(page.height, zoom);
    if (pixelH < 32) return 0;
    std::vector<FrameDesc> frames = collectFrames(page, pageIndex, pixelH, {}, &doc);
    int kept = 0;
    for (FrameDesc& frame : frames) {
        if (frame.kind != 0) continue;
        std::vector<GlyphSpot> existing;
        if (!takeLinePlaces(frame, existing)) writeLinePlace(doc, page, frame, pixelH);
        ++kept;
    }
    std::fprintf(stderr, "[ClassingHWP] %d쪽 줄 자리 %d\n", pageIndex + 1, kept);
    return kept;
}

LineNote cacheIdleLines(const model::Document& doc, std::vector<Node>& pages, int origin, int budget) {
    LineNote out;
    if (budget < 1) budget = 16;
    IdleLineNote& note = idleLineNote();
    if (note.done.size() != pages.size()) {
        note.done.assign(pages.size(), 0);
        note.told = false;
        note.recorded = 0;
    }
    if (note.told || pages.empty()) {
        note.told = true;
        return out;
    }
    const int count = static_cast<int>(pages.size());
    const int threadsWanted = std::max(1, layout::workerCount(256));
    const int batch = std::max(budget, threadsWanted * 8);
    std::vector<char> hold(static_cast<std::size_t>(count), 0);
    auto pending = [&](int start) {
        if (start < 0 || start >= count) start = 0;
        for (int step = 0; step < count; ++step) {
            const int page = (start + step) % count;
            if (!note.done[static_cast<std::size_t>(page)] && !hold[static_cast<std::size_t>(page)]) return page;
        }
        return -1;
    };
    struct LineJob {
        int page = 0;
        int pixelH = 0;
        FrameDesc frame;
    };
    struct PageTally {
        int page = 0;
        int placed = 0;
        int lines = 0;
        bool done = false;
    };
    std::vector<LineJob> jobs;
    std::vector<PageTally> tallies;
    while (static_cast<int>(jobs.size()) < batch) {
        const int pageIndex = pending(origin);
        if (pageIndex < 0) break;
        hold[static_cast<std::size_t>(pageIndex)] = 1;
        Node& page = pages[static_cast<std::size_t>(pageIndex)];
        ensurePageSpans(doc, page);
        const int pixelH = page.height > 0 ? pagePixelHeight(page.height, 100) : 0;
        if (page.height <= 0 || pixelH < 32) {
            note.done[static_cast<std::size_t>(pageIndex)] = 1;
            origin = pageIndex + 1;
            continue;
        }
        std::vector<FrameDesc> frames = collectFrames(page, pageIndex, pixelH, {}, &doc);
        PageTally tally;
        tally.page = pageIndex;
        std::vector<FrameDesc> todo;
        for (FrameDesc& frame : frames) {
            if (frame.kind != 0) continue;
            ++tally.lines;
            std::vector<GlyphSpot> existing;
            if (takeLinePlaces(frame, existing)) ++tally.placed;
            else todo.push_back(std::move(frame));
        }
        const int room = batch - static_cast<int>(jobs.size());
        const int take = std::min(room, static_cast<int>(todo.size()));
        for (int i = 0; i < take; ++i) {
            jobs.push_back(LineJob{pageIndex, pixelH, std::move(todo[static_cast<std::size_t>(i)])});
        }
        tally.placed += take;
        tally.done = tally.placed >= tally.lines;
        tallies.push_back(tally);
        if (!tally.done) break;
        origin = pageIndex + 1;
    }
    const int jobCount = static_cast<int>(jobs.size());
    int usedThreads = 1;
    if (jobCount > 0) {
        const int threads = layout::workerCount(jobCount);
        if (threads <= 1) {
            for (const LineJob& job : jobs) {
                writeLinePlace(doc, pages[static_cast<std::size_t>(job.page)], job.frame, job.pixelH);
            }
        } else {
            usedThreads = threads;
            std::atomic<int> next{0};
            std::vector<std::thread> pool;
            pool.reserve(static_cast<std::size_t>(threads));
            while (static_cast<int>(pool.size()) < threads) {
                pool.emplace_back([&] {
                    for (;;) {
                        const int index = next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= jobCount) return;
                        const LineJob& job = jobs[static_cast<std::size_t>(index)];
                        writeLinePlace(doc, pages[static_cast<std::size_t>(job.page)], job.frame, job.pixelH);
                    }
                });
            }
            for (std::thread& worker : pool) worker.join();
        }
        note.recorded += jobCount;
        out.made = jobCount;
        std::string line = "[ClassingHWP] 스레드 " + std::to_string(usedThreads) + " 줄 " + std::to_string(jobCount);
        for (const PageTally& tally : tallies) {
            if (tally.lines <= 0 || tally.placed <= 0) continue;
            line += " · " + std::to_string(tally.page + 1) + "쪽 " + std::to_string(tally.placed) + "/" +
                    std::to_string(tally.lines);
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    for (const PageTally& tally : tallies) {
        if (tally.done) note.done[static_cast<std::size_t>(tally.page)] = 1;
    }
    bool remain = false;
    for (int page = 0; page < count; ++page) {
        if (!note.done[static_cast<std::size_t>(page)]) {
            remain = true;
            break;
        }
    }
    if (!remain) {
        note.told = true;
        if (note.recorded > 0) std::fprintf(stderr, "[ClassingHWP] 줄 전진폭을 다 적었다\n");
    } else {
        out.more = true;
    }
    return out;
}

void noteDocumentGlyphs(const model::Document& doc) { gatherDocumentGlyphs(doc); }

void clearGlyphWarm() {
    clearDocumentGlyphs();
    clearHangulOutlineCache();
    // 수식 폭 표와 그린 수식도 글꼴 없을 때의 대체 폭을 품었을 수 있다.
    clearEquationFontCache();
    clearEquationInkCache();
    clearScreenGlyphs();
    // 그사이 들어온 글꼴이 있을 수 있다. 없어서 대체로 열어 둔 글꼴과 굵기 판단을 다시 본다.
    {
        std::unique_lock lock(gFaceMu);
        forgetMissingFacesLocked();
        // 이름별로 고른 글자(대체 글자 포함)도 다시 고른다. 글자 자체는 gGlyphMem 에 남아 다시 쓴다.
        resolvedFaces().clear();
    }
    {
        // FreeType 윤곽도 글꼴 이름으로 연다. 대체 글꼴로 열어 둔 이름이 있을 수 있어 모두 닫고 쓸 때 다시 연다.
        std::lock_guard ft(gFtMutex);
        for (auto& [key, slot] : gFtFaces) {
            if (slot.face) FT_Done_Face(slot.face);
        }
        gFtFaces.clear();
    }
    std::lock_guard lock(gBoldMu);
    gBoldFace.clear();
}

PageBitmap renderPageBitmap(const model::Document& doc, const Node& page, int pixelHeight) {
    return renderPageBitmap(doc, page, pixelHeight, 0, 0, 0, 0);
}

namespace {
std::atomic<bool> gPageImageGpu{true};
}  // namespace

void setPageImageGpu(bool on) { gPageImageGpu.store(on, std::memory_order_relaxed); }

bool pageImageGpu() { return gPageImageGpu.load(std::memory_order_relaxed); }

PageImage encodePageImage(const PageBitmap& bitmap) {
    PageImage image;
    if (bitmap.rgba.empty()) return image;
    image.width = bitmap.width;
    image.height = bitmap.height;
    Bitmap raw;
    raw.width = bitmap.width;
    raw.height = bitmap.height;
    raw.px = bitmap.rgba;
    image.png = encodePng(raw);
    return image;
}

PageImage renderPagePng(const model::Document& doc, const Node& page, int pixelHeight) {
    PageImage image;
    PageBitmap bitmap = renderPageBitmap(doc, page, pixelHeight);
    if (bitmap.rgba.empty()) return image;
    image.width = bitmap.width;
    image.height = bitmap.height;
    Bitmap raw;
    raw.width = bitmap.width;
    raw.height = bitmap.height;
    raw.px = std::move(bitmap.rgba);
    image.png = encodePng(raw);
    return image;
}

PageImage renderPagePng(const model::Document& doc, int pageIndex, int pixelHeight) {
    if (pageIndex < 0 || pixelHeight < 32) return {};
    const std::vector<Node> pages = layoutPages(doc);
    if (pageIndex >= static_cast<int>(pages.size())) return {};
    return renderPagePng(doc, pages[static_cast<std::size_t>(pageIndex)], pixelHeight);
}

namespace {

struct SelectionSheet {
    int percent = -1;
    int stamp = 0;
    PageBitmap bitmap;
};

struct PageStoryLinesSlot {
    const model::Document* docPtr = nullptr;
    const Node* pagePtr = nullptr;
    std::size_t childCount = 0;
    int width = 0;
    int height = 0;
    int story = 0;
    const Node* firstLinePtr = nullptr;
    int firstLineEnd = -1;
    int minPara = std::numeric_limits<int>::max();
    int maxPara = -1;
    std::vector<Painter::StoryLine> lines;
};

struct PageBlockSelectionSlot {
    const model::Document* docPtr = nullptr;
    const Node* pagePtr = nullptr;
    std::size_t childCount = 0;
    const Node* firstLinePtr = nullptr;
    int firstLineEnd = -1;
    int pageIndex = -1;
    int story = 0;
    int effFromPara = -1;
    int effFromOff = -1;
    int effToPara = -1;
    int effToOff = -1;
    std::vector<BlockBox> boxes;
};

std::unordered_map<std::uint64_t, PageStoryLinesSlot> gPageStoryLinesCache;
std::unordered_map<std::uint64_t, PageBlockSelectionSlot> gPageBlockSelectionCache;

std::vector<SelectionSheet> gSelectionSheets;
int gSelectionStamp = 0;

void clearSelectionSheets() {
    gSelectionSheets.clear();
    gSelectionStamp = 0;
    gPageStoryLinesCache.clear();
    gPageBlockSelectionCache.clear();
    lineBlockCache().clear();
}

PageBitmap& selectionSheet(const model::Document& doc, const Node& page, int pageIndex, int percent, int pixelHeight) {
    if (static_cast<int>(gSelectionSheets.size()) <= pageIndex) {
        gSelectionSheets.resize(static_cast<std::size_t>(pageIndex) + 1);
    }
    SelectionSheet& slot = gSelectionSheets[static_cast<std::size_t>(pageIndex)];
    if (slot.percent != percent || slot.bitmap.width <= 0 || slot.bitmap.rgba.empty()) {
        slot.bitmap = renderPageBitmap(doc, page, pixelHeight);
        slot.percent = percent;
    }
    slot.stamp = ++gSelectionStamp;
    return slot.bitmap;
}

void trimSelectionSheets(const std::vector<char>& used) {
    int live = 0;
    for (const SelectionSheet& slot : gSelectionSheets) {
        if (!slot.bitmap.rgba.empty()) ++live;
    }
    while (live > 8) {
        int oldest = -1;
        int oldestStamp = 0;
        for (int i = 0; i < static_cast<int>(gSelectionSheets.size()); ++i) {
            const SelectionSheet& slot = gSelectionSheets[static_cast<std::size_t>(i)];
            if (slot.bitmap.rgba.empty()) continue;
            if (i < static_cast<int>(used.size()) && used[static_cast<std::size_t>(i)]) continue;
            if (oldest < 0 || slot.stamp < oldestStamp) {
                oldest = i;
                oldestStamp = slot.stamp;
            }
        }
        if (oldest < 0) break;
        gSelectionSheets[static_cast<std::size_t>(oldest)] = {};
        --live;
    }
}

}  // namespace

void clearPageRenderCache() {
    releaseGpuImages();
    clearPictures();
    {
        std::unique_lock lock(gOutlineMu);
        gCommands.clear();
    }
    {
        std::lock_guard lock(gLineWarnMu);
        gLineWarnSeen.clear();
    }
    clearEquationInkCache();
    clearSelectionSheets();
}

CaretPlace caretOnPage(const model::Document& doc, const Node& page, std::int32_t x, std::int32_t y) {
    CaretPlace place;
    if (page.width <= 0 || page.height <= 0) return place;
    if (page.spansLater) ensurePageSpans(doc, const_cast<Node&>(page));
    Painter painter;
    painter.doc = &doc;
    painter.walkCaret(page, 0, 0, x, y, place, false, -1, -1);
    return place;
}

const PageStoryLinesSlot& ensurePageStoryLines(Painter& painter, const model::Document& doc, const Node& page,
                                               int pageIndex, int story) {
    const std::uint64_t key =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pageIndex)) << 32) |
        static_cast<std::uint64_t>(static_cast<std::uint32_t>(story));
    PageStoryLinesSlot& slot = gPageStoryLinesCache[key];
    const bool valid =
        slot.docPtr == &doc && slot.pagePtr == &page && slot.childCount == page.children.size() &&
        slot.width == page.width && slot.height == page.height && slot.story == story &&
        (slot.lines.empty() ||
         (slot.firstLinePtr == slot.lines.front().node && slot.firstLineEnd == slot.lines.front().node->end));
    if (valid) return slot;
    slot.docPtr = &doc;
    slot.pagePtr = &page;
    slot.childCount = page.children.size();
    slot.width = page.width;
    slot.height = page.height;
    slot.story = story;
    slot.lines.clear();
    painter.gatherStory(page, 0, 0, story, slot.lines);
    slot.firstLinePtr = slot.lines.empty() ? nullptr : slot.lines.front().node;
    slot.firstLineEnd = slot.lines.empty() ? -1 : slot.lines.front().node->end;
    slot.minPara = std::numeric_limits<int>::max();
    slot.maxPara = -1;
    for (const Painter::StoryLine& sl : slot.lines) {
        if (!sl.node) continue;
        slot.minPara = std::min(slot.minPara, sl.node->paragraph);
        slot.maxPara = std::max(slot.maxPara, sl.node->paragraph);
    }
    return slot;
}

namespace paint_detail {

bool lineInkHas(const model::Document& doc, const Node& line, std::int32_t lineX, std::int32_t lineY, std::int32_t px,
                std::int32_t slack) {
    if (line.kind != NodeKind::Line || line.end <= line.start) return false;
    Painter painter;
    painter.doc = &doc;
    const Painter::StoryLine story{&line, lineX, lineY};
    const CaretPlace first = painter.placeOnLineAtOffset(story, line.start);
    const CaretPlace last = painter.placeOnLineAtOffset(story, line.end);
    if (!first.hit || !last.hit) return true;
    const std::int32_t left = std::min(first.x, last.x);
    const std::int32_t right = std::max(first.x, last.x);
    return px >= left - slack && px <= right + slack;
}

}  // namespace paint_detail

CaretPlace caretInStory(const model::Document& doc, const std::vector<Node>& pages, int page, std::int32_t x,
                        std::int32_t y, int story, bool drag, int anchorParagraph, int anchorOffset) {
    CaretPlace place;
    if (story <= 0 || pages.empty()) return place;
    Painter painter;
    painter.doc = &doc;
    if (page >= 0 && page < static_cast<int>(pages.size())) {
        const Node& targetPage = pages[static_cast<std::size_t>(page)];
        if (targetPage.spansLater) ensurePageSpans(doc, const_cast<Node&>(targetPage));
        const PageStoryLinesSlot& slot = ensurePageStoryLines(painter, doc, targetPage, page, story);
        if (const Painter::StoryLine* line = painter.nearestLine(slot.lines, x, y)) {
            place = painter.placeOnLine(*line, Painter::alongLine(*line, x, y), drag, anchorParagraph, anchorOffset);
            if (place.hit) place.page = page;
            return place;
        }
    }
    int first = -1;
    int last = -1;
    for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
        const Node& curPage = pages[static_cast<std::size_t>(i)];
        if (!Painter::subtreeHasStory(curPage, story)) continue;
        if (curPage.spansLater) ensurePageSpans(doc, const_cast<Node&>(curPage));
        const PageStoryLinesSlot& slot = ensurePageStoryLines(painter, doc, curPage, i, story);
        if (slot.lines.empty()) continue;
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0) return place;
    const int target = page <= first ? first : last;
    const PageStoryLinesSlot& targetSlot =
        ensurePageStoryLines(painter, doc, pages[static_cast<std::size_t>(target)], target, story);
    const bool atStart = page <= first;
    const Painter::StoryLine* line = painter.edgeLine(targetSlot.lines, atStart);
    if (!line) return place;
    const std::int32_t probe =
        atStart ? line->x - 1000000 : line->x + std::max<std::int32_t>(line->node->width, 1) + 1000000;
    place = painter.placeOnLine(*line, probe, drag, anchorParagraph, anchorOffset);
    if (place.hit) place.page = target;
    return place;
}

std::vector<BlockBox> blockBoxes(const model::Document& doc, const std::vector<Node>& pages, int story, int paragraphA,
                                 int offsetA, int paragraphB, int offsetB) {
    std::vector<BlockBox> boxes;
    if (story <= 0) return boxes;
    int fromParagraph = paragraphA;
    int fromOffset = offsetA;
    int toParagraph = paragraphB;
    int toOffset = offsetB;
    if (toParagraph < fromParagraph || (toParagraph == fromParagraph && toOffset < fromOffset)) {
        std::swap(fromParagraph, toParagraph);
        std::swap(fromOffset, toOffset);
    }
    Painter painter;
    painter.doc = &doc;
    forEachStoryParagraphPage(pages, story, fromParagraph, toParagraph, currentCaretPageHint(),
                              [&](int i, const Node& curPage) {
                                  if (curPage.spansLater) ensurePageSpans(doc, const_cast<Node&>(curPage));
                                  const PageStoryLinesSlot& sSlot =
                                      ensurePageStoryLines(painter, doc, curPage, i, story);
                                  if (sSlot.lines.empty() || sSlot.maxPara < fromParagraph ||
                                      sSlot.minPara > toParagraph) {
                                      return;
                                  }
                                  const int effFromPara =
                                      fromParagraph <= sSlot.minPara ? sSlot.minPara : fromParagraph;
                                  const int effFromOff = fromParagraph < sSlot.minPara ? -1 : fromOffset;
                                  const int effToPara = toParagraph >= sSlot.maxPara ? sSlot.maxPara : toParagraph;
                                  const int effToOff =
                                      toParagraph > sSlot.maxPara ? std::numeric_limits<int>::max() : toOffset;
                                  const std::uint64_t bKey =
                                      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32) |
                                      static_cast<std::uint64_t>(static_cast<std::uint32_t>(story));
                                  PageBlockSelectionSlot& bSlot = gPageBlockSelectionCache[bKey];
                                  if (bSlot.docPtr == &doc && bSlot.pagePtr == &curPage &&
                                      bSlot.childCount == curPage.children.size() &&
                                      bSlot.firstLinePtr == sSlot.firstLinePtr &&
                                      bSlot.firstLineEnd == sSlot.firstLineEnd &&
                                      bSlot.pageIndex == i && bSlot.story == story &&
                                      bSlot.effFromPara == effFromPara && bSlot.effFromOff == effFromOff &&
                                      bSlot.effToPara == effToPara && bSlot.effToOff == effToOff) {
                                      boxes.insert(boxes.end(), bSlot.boxes.begin(), bSlot.boxes.end());
                                      return;
                                  }
                                  bSlot.docPtr = &doc;
                                  bSlot.pagePtr = &curPage;
                                  bSlot.childCount = curPage.children.size();
                                  bSlot.firstLinePtr = sSlot.firstLinePtr;
                                  bSlot.firstLineEnd = sSlot.firstLineEnd;
                                  bSlot.pageIndex = i;
                                  bSlot.story = story;
                                  bSlot.effFromPara = effFromPara;
                                  bSlot.effFromOff = effFromOff;
                                  bSlot.effToPara = effToPara;
                                  bSlot.effToOff = effToOff;
                                  bSlot.boxes.clear();
                                  painter.appendBlock(curPage, 0, 0, i, story, effFromPara, effFromOff, effToPara,
                                                      effToOff, bSlot.boxes, "");
                                  boxes.insert(boxes.end(), bSlot.boxes.begin(), bSlot.boxes.end());
                              });
    return boxes;
}

void invertOpaqueColors(std::vector<std::uint8_t>& rgba, int width, int height) {
    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    if (width <= 0 || height <= 0 || rgba.size() < bytes) return;
    std::vector<std::uint8_t> src = rgba;
    const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    const SkPixmap pixmap(info, src.data(), static_cast<std::size_t>(width) * 4u);
    const sk_sp<SkImage> image = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
    const sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, rgba.data(), static_cast<std::size_t>(width) * 4u);
    if (image && surface) {
        SkPaint paint;
        const float matrix[20] = {
            -1.f, 0.f, 0.f, 0.f, 1.f, 0.f, -1.f, 0.f, 0.f, 1.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f,
        };
        paint.setColorFilter(SkColorFilters::Matrix(matrix));
        surface->getCanvas()->drawImage(image.get(), 0, 0, SkSamplingOptions(), &paint);
        return;
    }
    for (std::size_t i = 0; i + 3 < bytes; i += 4) {
        rgba[i] = static_cast<std::uint8_t>(255 - rgba[i]);
        rgba[i + 1] = static_cast<std::uint8_t>(255 - rgba[i + 1]);
        rgba[i + 2] = static_cast<std::uint8_t>(255 - rgba[i + 2]);
    }
}

namespace {

std::int32_t scaleHu(std::int32_t hu, std::int32_t pageHu, int pixel) {
    if (pageHu <= 0 || pixel <= 0) return 0;
    return static_cast<std::int32_t>(std::lround(static_cast<double>(hu) * static_cast<double>(pixel) /
                                                 static_cast<double>(pageHu)));
}

void fillCaret(std::vector<std::uint8_t>& rgba, int width, int height, bool light) {
    const std::size_t bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    rgba.assign(bytes, 0);
    if (width <= 0 || height <= 0) return;
    const SkImageInfo info = SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    const sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, rgba.data(), static_cast<std::size_t>(width) * 4u);
    if (surface) {
        SkPaint paint;
        paint.setColor(light ? SK_ColorWHITE : SK_ColorBLACK);
        surface->getCanvas()->drawRect(SkRect::MakeWH(static_cast<float>(width), static_cast<float>(height)), paint);
        return;
    }
    const std::uint8_t value = light ? 255 : 0;
    for (std::size_t i = 0; i + 3 < bytes; i += 4) {
        rgba[i] = value;
        rgba[i + 1] = value;
        rgba[i + 2] = value;
        rgba[i + 3] = 255;
    }
}

struct PixelBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

}  // namespace

std::vector<SelectionImage> paintSelection(const model::Document& doc, const std::vector<Node>& pages,
                                           const std::vector<BlockBox>& boxes, const CaretPlace& caret, int caretPage,
                                           int percent) {
    std::vector<SelectionImage> images;
    if (percent <= 0) percent = 100;
    std::vector<char> usedSheets(pages.size(), 0);
    for (int pageIndex = 0; pageIndex < static_cast<int>(pages.size()); ++pageIndex) {
        const Node& page = pages[static_cast<std::size_t>(pageIndex)];
        if (page.width <= 0 || page.height <= 0) continue;
        int pixelH = pagePixelHeight(page.height, percent);
        int pixelW = pagePixelWidth(viewWidthOf(page), page.height, pixelH);
        if (pixelH < 32) continue;
        bool hasBox = false;
        for (const BlockBox& box : boxes) {
            if (box.page == pageIndex && box.width > 0 && box.height > 0) hasBox = true;
        }
        const PageBitmap* sheet = nullptr;
        if (hasBox) {
            const PageBitmap& cached = selectionSheet(doc, page, pageIndex, percent, pixelH);
            if (cached.width > 0 && cached.height > 0 && !cached.rgba.empty()) {
                sheet = &cached;
                pixelW = cached.width;
                pixelH = cached.height;
                usedSheets[static_cast<std::size_t>(pageIndex)] = 1;
            }
        }
        std::vector<PixelBox> local;
        int left = pixelW;
        int top = pixelH;
        int right = 0;
        int bottom = 0;
        for (const BlockBox& box : boxes) {
            if (box.page != pageIndex || box.width <= 0 || box.height <= 0) continue;
            PixelBox pixel;
            pixel.x = scaleHu(box.x, viewWidthOf(page), pixelW);
            pixel.y = scaleHu(box.y, page.height, pixelH);
            const int edgeX = scaleHu(box.x + box.width, viewWidthOf(page), pixelW);
            const int edgeY = scaleHu(box.y + box.height, page.height, pixelH);
            pixel.w = std::max(1, edgeX - pixel.x);
            pixel.h = std::max(1, edgeY - pixel.y);
            local.push_back(pixel);
            left = std::min(left, pixel.x);
            top = std::min(top, pixel.y);
            right = std::max(right, pixel.x + pixel.w);
            bottom = std::max(bottom, pixel.y + pixel.h);
        }
        if (!local.empty() && right > left && bottom > top) {
            left = std::clamp(left, 0, pixelW - 1);
            top = std::clamp(top, 0, pixelH - 1);
            const int viewW = std::clamp(right - left, 1, pixelW - left);
            const int viewH = std::clamp(bottom - top, 1, pixelH - top);
            PageBitmap bitmap;
            if (sheet && left + viewW <= sheet->width && top + viewH <= sheet->height) {
                bitmap.width = viewW;
                bitmap.height = viewH;
                bitmap.rgba.resize(static_cast<std::size_t>(viewW) * static_cast<std::size_t>(viewH) * 4u);
                for (int y = 0; y < viewH; ++y) {
                    const std::uint8_t* src = sheet->rgba.data() +
                                              (static_cast<std::size_t>(top + y) * static_cast<std::size_t>(sheet->width) + left) * 4u;
                    std::memcpy(bitmap.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(viewW) * 4u, src,
                                static_cast<std::size_t>(viewW) * 4u);
                }
            }
            if (bitmap.width > 0 && bitmap.height > 0 && !bitmap.rgba.empty()) {
                invertOpaqueColors(bitmap.rgba, bitmap.width, bitmap.height);
                for (int y = 0; y < bitmap.height; ++y) {
                    for (int x = 0; x < bitmap.width; ++x) {
                        const int pageX = left + x;
                        const int pageY = top + y;
                        bool inside = false;
                        for (const PixelBox& pixel : local) {
                            if (pageX >= pixel.x && pageY >= pixel.y && pageX < pixel.x + pixel.w &&
                                pageY < pixel.y + pixel.h) {
                                inside = true;
                                break;
                            }
                        }
                        if (inside) continue;
                        std::uint8_t* px = bitmap.rgba.data() + (static_cast<std::size_t>(y) * bitmap.width + x) * 4u;
                        px[0] = px[1] = px[2] = px[3] = 0;
                    }
                }
                SelectionImage image;
                image.page = pageIndex;
                image.x = left;
                image.y = top;
                image.width = bitmap.width;
                image.height = bitmap.height;
                image.pageW = pixelW;
                image.pageH = pixelH;
                image.rgba = std::move(bitmap.rgba);
                images.push_back(std::move(image));
            }
        }
        if (!caret.hit || caretPage != pageIndex || caret.height <= 0) continue;
        SelectionImage cursor;
        cursor.caret = true;
        cursor.page = pageIndex;
        cursor.pageW = pixelW;
        cursor.pageH = pixelH;
        cursor.x = std::clamp(scaleHu(caret.x, viewWidthOf(page), pixelW), 0, std::max(0, pixelW - 1));
        cursor.y = std::clamp(scaleHu(caret.y, page.height, pixelH), 0, std::max(0, pixelH - 1));
        if (caret.turned) {
            // 세로쓰기 커서는 누운 막대다.
            const int cursorW = std::max(1, scaleHu(caret.height, viewWidthOf(page), pixelW));
            cursor.width = std::min(cursorW, std::max(1, pixelW - cursor.x));
            cursor.height = 1;
        } else {
            cursor.width = 1;
            const int cursorH = std::max(1, scaleHu(caret.height, page.height, pixelH));
            cursor.height = std::min(cursorH, std::max(1, pixelH - cursor.y));
        }
        fillCaret(cursor.rgba, cursor.width, cursor.height, !local.empty());
        images.push_back(std::move(cursor));
    }
    trimSelectionSheets(usedSheets);
    return images;
}

std::string selectionText(const model::Document& doc, const std::vector<Node>& pages) {
    PointerHold& hold = pointerHold();
    if (hold.story <= 0 || (hold.anchorPara == hold.focusPara && hold.anchorOff == hold.focusOff)) return {};
    if (!hold.selectedText.empty()) return hold.selectedText;
    Painter painter;
    painter.doc = &doc;
    hold.selectedText =
        painter.copySelection(pages, hold.story, hold.anchorPara, hold.anchorOff, hold.focusPara, hold.focusOff);
    return hold.selectedText;
}

CaretPlace caretForCursor(const model::Document& doc, const std::vector<Node>& pages, model::CursorPos pos) {
    CaretPlace best;
    if (pos.story <= 0 || pos.paragraph < 0 || pages.empty()) return best;
    Painter painter;
    painter.doc = &doc;
    int fallbackPage = -1;
    Painter::StoryLine fallbackLine{};
    bool hasFallback = false;
    bool prevLineBreak = false;
    int prevMaxOff = -1;
    forEachStoryParagraphPage(
        pages, pos.story, pos.paragraph, pos.paragraph, currentCaretPageHint(),
        [&](int pageIndex, const Node& curPage) {
            if (curPage.spansLater) ensurePageSpans(doc, const_cast<Node&>(curPage));
            std::vector<Painter::StoryLine> lines;
            painter.gatherStoryParagraph(curPage, 0, 0, pos.story, pos.paragraph, lines);
            for (const Painter::StoryLine& line : lines) {
                if (!line.node || line.node->paragraph != pos.paragraph) continue;
                const std::vector<std::int32_t> edges = painter.glyphEdges(*line.node, line.x, line.y, nullptr);
                if (edges.empty()) continue;
                const int maxOff = line.node->start + static_cast<int>(edges.size()) - 1;
                fallbackPage = pageIndex;
                fallbackLine = line;
                hasFallback = true;
                if (pos.offset >= line.node->start && pos.offset <= maxOff) {
                    const bool startsWithObject =
                        lineStartsWithObject(*line.node);
                    const PointerHold& hold = pointerHold();
                    const bool caretWasOnThisLine =
                        hold.caret.hit && hold.caretPage == pageIndex &&
                        std::abs(hold.caret.y - (line.y + line.node->marginTop)) <= 4;
                    if (best.hit && !prevLineBreak && pos.offset == line.node->start && pos.offset == prevMaxOff &&
                        startsWithObject && !caretWasOnThisLine) {
                        prevLineBreak = line.node->lineData().lineBreak;
                        prevMaxOff = maxOff;
                        continue;
                    }
                    best = painter.placeOnLineAtOffset(line, pos.offset);
                    best.page = pageIndex;
                }
                prevLineBreak = line.node->lineData().lineBreak;
                prevMaxOff = maxOff;
            }
        });
    if (best.hit) return best;
    if (hasFallback) {
        best = painter.placeOnLineAtOffset(fallbackLine, pos.offset);
        best.page = fallbackPage;
    }
    return best;
}

CursorStyle cursorStyleForCaret(const model::Document& doc, const CaretPlace& caret) {
    CursorStyle st;
    st.active = caret.hit;
    std::uint32_t styleId = caret.styleId;
    std::uint32_t paraShapeId = caret.paraShapeId;
    std::uint32_t charShapeId = caret.charShapeId;
    if (caret.story > 0 && caret.paragraph >= 0) {
        if (const model::Paragraph* para = storyParagraph(doc, caret.story, caret.paragraph)) {
            styleId = para->styleId;
            paraShapeId = para->paraShapeId;
            if (charShapeId == 0 && !para->runs.empty()) {
                charShapeId = para->runs.front().charShapeId;
            }
        }
        if (const auto pending = pendingCharShapeForCaret(caret.story, caret.paragraph, caret.offset)) {
            charShapeId = *pending;
        }
    }

    const model::Style* style = Painter::findStyle(doc, styleId);
    if (!style && !doc.header.styles.empty()) style = &doc.header.styles.front();
    if (style) {
        st.styleId = style->id;
        st.styleName = style->name;
        st.styleEngName = style->engName;
        st.styleType = style->type.empty() ? "PARA" : style->type;
    } else {
        st.styleId = styleId;
        st.styleName = "바탕글";
        st.styleEngName = "Normal";
        st.styleType = "PARA";
    }

    const model::ParaShape* ps = Painter::findParaShape(doc, paraShapeId);
    if (!ps && style) ps = Painter::findParaShape(doc, style->paraShapeId);
    if (!ps && !doc.header.paraShapes.empty()) ps = &doc.header.paraShapes.front();
    if (ps) {
        st.paraShapeId = ps->id;
        st.align = ps->align.empty() ? "JUSTIFY" : ps->align;
        st.headingType = ps->headingType.empty() ? "NONE" : ps->headingType;
        st.headingLevel = ps->headingLevel;
        st.lineSpacingType = ps->lineSpacingType.empty() ? "PERCENT" : ps->lineSpacingType;
        st.lineSpacing = ps->lineSpacing > 0 ? ps->lineSpacing : 160;
    } else {
        st.paraShapeId = paraShapeId;
    }

    const model::CharShape* cs = Painter::findCharShape(doc, charShapeId);
    if (!cs && style) cs = Painter::findCharShape(doc, style->charShapeId);
    if (!cs && !doc.header.charShapes.empty()) cs = &doc.header.charShapes.front();
    if (cs) {
        st.charShapeId = cs->id;
        st.font = Painter::resolveCharShapeFontName(doc, *cs);
        for (std::size_t slot = 0; slot < model::kLangCount && slot < doc.header.fonts.size(); ++slot) {
            for (const model::Font& f : doc.header.fonts[slot]) {
                if (f.id == cs->fontRef[slot]) st.langFonts[slot] = f.face;
            }
        }
        st.fontSize = cs->height > 0 ? cs->height : 1000;
        st.bold = cs->bold;
        st.italic = cs->italic;
        st.underlineType = cs->underline.empty() ? "NONE" : cs->underline;
        st.underline = st.underlineType != "NONE";
        st.strikeout = cs->strikeout;
        st.superscript = cs->superscript;
        st.subscript = cs->subscript;
        const auto hex = [](const model::Color& c) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(c.value_or(0) & 0xFFFFFFu));
            return std::string(buf);
        };
        st.underlineColor = hex(cs->underlineColor);
        st.strikeColor = hex(cs->strikeColor);
        st.charBorder = false;
        for (const model::BorderFill& fill : doc.header.borderFills) {
            if (fill.id != cs->borderFillId) continue;
            for (const model::BorderSide* side : {&fill.left, &fill.right, &fill.top, &fill.bottom}) {
                if (!side->type.empty() && side->type != "NONE") st.charBorder = true;
            }
        }
        if (cs->textColor) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "#%06x", static_cast<unsigned>(*cs->textColor & 0xFFFFFFu));
            st.color = buf;
        } else {
            st.color = "#000000";
        }
    } else {
        st.charShapeId = charShapeId;
    }
    if (st.font.empty() && !doc.header.fonts[0].empty()) {
        st.font = doc.header.fonts[0].front().face;
    }
    st.fontSwapTo = layout::fontLicenseSwapTarget(st.font);
    return st;
}

namespace {

// 블록이 덮는 문단과 글자 구간을 훑어 값이 섞였는지 적는다. 문단 값(스타일·정렬·줄 간격)은 블록이 지나는 문단,
// 글자 값은 블록 안 글자가 있는 구간만 본다. 블록이 다음 문단 맨 앞에서 끝나면 그 문단은 넣지 않는다.
void markMixed(const model::Document& doc, const std::vector<Node>& pages, const PointerCursor& cur, CursorStyle& st) {
    const std::vector<model::Paragraph>* paras = storyParagraphs(doc, cur.story);
    if (!paras) return;
    const int last = static_cast<int>(paras->size()) - 1;
    const int firstPara = std::clamp(cur.startParagraph, 0, last);
    const int lastPara = std::clamp(cur.endParagraph, 0, last);
    const int startChar = offsetToCharIndex(doc, pages, cur.story, firstPara, cur.startOffset);
    const int endChar = offsetToCharIndex(doc, pages, cur.story, lastPara, cur.endOffset);
    std::unordered_map<std::uint32_t, CursorStyle> charStyles;
    const auto charStyle = [&](std::uint32_t id) -> const CursorStyle& {
        auto found = charStyles.find(id);
        if (found == charStyles.end()) {
            CaretPlace probe;
            probe.charShapeId = id;
            found = charStyles.emplace(id, cursorStyleForCaret(doc, probe)).first;
        }
        return found->second;
    };
    const auto codePoints = [](const std::string& text) {
        int count = 0;
        for (unsigned char c : text) {
            if ((c & 0xC0) != 0x80) ++count;
        }
        return count;
    };
    for (int p = firstPara; p <= lastPara; ++p) {
        const model::Paragraph& para = (*paras)[static_cast<std::size_t>(p)];
        const int from = p == firstPara ? startChar : 0;
        const int to = p == lastPara ? endChar : std::numeric_limits<int>::max();
        if (p == lastPara && p != firstPara && endChar <= 0) break;
        CaretPlace paraProbe;
        paraProbe.styleId = para.styleId;
        paraProbe.paraShapeId = para.paraShapeId;
        const CursorStyle paraStyle = cursorStyleForCaret(doc, paraProbe);
        st.mixed.style = st.mixed.style || paraStyle.styleId != st.styleId;
        st.mixed.align = st.mixed.align || paraStyle.align != st.align;
        st.mixed.lineSpacing = st.mixed.lineSpacing || paraStyle.lineSpacing != st.lineSpacing ||
                               paraStyle.lineSpacingType != st.lineSpacingType;
        int pos = 0;
        for (const model::Run& run : para.runs) {
            int length = 0;
            for (const model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Text) length += codePoints(item.text);
                else if (isCaretCharControl(item.control)) ++length;
            }
            const int runFrom = pos;
            pos += length;
            if (length <= 0 || pos <= from || runFrom >= to) continue;
            const CursorStyle& cs = charStyle(run.charShapeId);
            st.mixed.font = st.mixed.font || cs.font != st.font;
            for (std::size_t slot = 0; slot < model::kLangCount; ++slot) {
                st.mixed.langFonts[slot] = st.mixed.langFonts[slot] || cs.langFonts[slot] != st.langFonts[slot];
            }
            st.mixed.fontSize = st.mixed.fontSize || cs.fontSize != st.fontSize;
            st.mixed.bold = st.mixed.bold || cs.bold != st.bold;
            st.mixed.italic = st.mixed.italic || cs.italic != st.italic;
            st.mixed.underline = st.mixed.underline || cs.underlineType != st.underlineType;
            st.mixed.strikeout = st.mixed.strikeout || cs.strikeout != st.strikeout;
            st.mixed.superscript = st.mixed.superscript || cs.superscript != st.superscript;
            st.mixed.subscript = st.mixed.subscript || cs.subscript != st.subscript;
            st.mixed.underlineColor = st.mixed.underlineColor || cs.underlineColor != st.underlineColor;
            st.mixed.strikeColor = st.mixed.strikeColor || cs.strikeColor != st.strikeColor;
            st.mixed.charBorder = st.mixed.charBorder || cs.charBorder != st.charBorder;
            st.mixed.color = st.mixed.color || cs.color != st.color;
        }
    }
}

}  // namespace

double CursorStyle::lineSpacingValue() const {
    const bool percent = lineSpacingType.empty() || lineSpacingType == "PERCENT";
    return percent ? static_cast<double>(lineSpacing) : huToPt(lineSpacing, 1);
}

const char* CursorStyle::lineSpacingUnit() const {
    return lineSpacingType.empty() || lineSpacingType == "PERCENT" ? "%" : "pt";
}

double CursorStyle::fontSizePt() const { return huToPt(fontSize, 1); }

double CursorStyle::fontSizeIn(const std::string& unit) const {
    return huToLength(fontSize, lengthUnitOf(unit), 2);
}

namespace {

CursorStyle paragraphCursorStyle(const model::Document& doc, const std::vector<Node>& pages);

}  // namespace

// 캐럿(블록이면 블록 처음) 글에 글자 스타일이 걸려 있으면 그 스타일을 보인다(숨긴 한글: 글자 스타일 글 안·끝의 캐럿
// 스타일은 그 글자 스타일이다). 블록 없는 캐럿에 걸어 둔 글자 스타일도 같다.
CursorStyle cursorStyle(const model::Document& doc, const std::vector<Node>& pages) {
    CursorStyle st = paragraphCursorStyle(doc, pages);
    const PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return st;
    const int paragraph = cur.hasSelection ? cur.startParagraph : cur.paragraph;
    const int offset = cur.hasSelection ? cur.startOffset : cur.offset;
    const model::Paragraph* para = storyParagraph(doc, cur.story, paragraph);
    if (!para) return st;
    std::uint32_t charStyle = model::kNoCharStyle;
    if (!cur.hasSelection && hold.hasPendingCharShape && hold.pendingCharStory == cur.story &&
        hold.pendingCharPara == paragraph && hold.pendingCharOffset == offset) {
        charStyle = hold.pendingCharStyleId;
    } else {
        const int charIndex = offsetToCharIndex(doc, pages, cur.story, paragraph, offset);
        charStyle = charStyleAtParagraphChar(*para, charIndex, cur.hasSelection || charIndex == 0);
    }
    const model::Style* style = charStyle != model::kNoCharStyle ? Painter::findStyle(doc, charStyle) : nullptr;
    if (style && style->type == "CHAR") {
        st.styleId = style->id;
        st.styleName = style->name;
        st.styleEngName = style->engName;
        st.styleType = "CHAR";
    }
    return st;
}

namespace {

CursorStyle paragraphCursorStyle(const model::Document& doc, const std::vector<Node>& pages) {
    const PointerHold& hold = pointerHold();
    const PointerCursor cur = pointerCursor();
    if (cur.active && cur.hasSelection && cur.story > 0 && cur.startParagraph >= 0) {
        if (const model::Paragraph* startPara = storyParagraph(doc, cur.story, cur.startParagraph)) {
            const int startChar = offsetToCharIndex(doc, pages, cur.story, cur.startParagraph, cur.startOffset);
            CaretPlace selCaret = hold.caret;
            selCaret.hit = true;
            selCaret.story = cur.story;
            selCaret.paragraph = cur.startParagraph;
            selCaret.offset = cur.startOffset;
            selCaret.styleId = startPara->styleId;
            selCaret.paraShapeId = startPara->paraShapeId;
            selCaret.charShapeId = charShapeAtParagraphChar(*startPara, startChar, true);
            CursorStyle st = cursorStyleForCaret(doc, selCaret);
            markMixed(doc, pages, cur, st);
            return st;
        }
    }
    if (hold.caret.hit) {
        return cursorStyleForCaret(doc, hold.caret);
    }
    if (hold.story > 0 && hold.focusPara >= 0) {
        const CaretPlace focusCaret =
            caretForCursor(doc, pages, model::CursorPos{hold.story, hold.focusPara, hold.focusOff});
        if (focusCaret.hit) {
            return cursorStyleForCaret(doc, focusCaret);
        }
    }
    CaretPlace fallback;
    if (!doc.sections.empty() && !doc.sections.front().paragraphs.empty()) {
        const auto& p0 = doc.sections.front().paragraphs.front();
        fallback.styleId = p0.styleId;
        fallback.paraShapeId = p0.paraShapeId;
        if (!p0.runs.empty()) fallback.charShapeId = p0.runs.front().charShapeId;
    }
    return cursorStyleForCaret(doc, fallback);
}

}  // namespace

namespace {

// 캐럿 줄을 찾으며 지나온 줄을 센다. 단은 본문 단, 줄 묶음은 가장 가까운 단·칸·개체다.
struct CaretLineWalk {
    int story = -1;
    int paragraph = -1;
    int charIndex = -1;
    int columns = 0;
    const Node* found = nullptr;
    const Node* foundBox = nullptr;
    int foundColumn = 0;
    std::unordered_map<const Node*, int> counts;
    int foundLine = 0;

    void walk(const Node& node, const Node* box, int column) {
        if (node.kind == NodeKind::Column && column == 0) {
            column = ++columns;
            box = &node;
        } else if (node.kind == NodeKind::Column || node.kind == NodeKind::Cell || node.kind == NodeKind::Object) {
            box = &node;
        }
        if (node.kind == NodeKind::Line && node.story == story) {
            const int n = ++counts[box];
            // 같은 문단에서 캐럿 글자가 줄 시작 뒤에 있으면 그 줄이다. 줄 끝 캐럿은 다음 줄이 이어받는다.
            if (node.paragraph == paragraph && node.start <= charIndex && charIndex <= node.end) {
                found = &node;
                foundBox = box;
                foundColumn = column;
                foundLine = n;
            }
            return;
        }
        for (const Node& child : node.children) walk(child, box, column);
    }
};

}  // namespace

CaretStatus caretStatus(const model::Document& doc, const std::vector<Node>& pages) {
    CaretStatus st;
    st.sectionCount = static_cast<int>(doc.sections.size());
    st.overwrite = overwrite();
    // 블록이면 끄는 쪽 끝(focus)을 센다. 커서가 없으면 마지막으로 놓은 캐럿을 쓴다.
    const PointerCursor cur = pointerCursor();
    const PointerHold& hold = pointerHold();
    if (hold.bandStory > 0 && !hold.bandMaster) st.band = hold.bandHeader ? 1 : 2;
    st.master = hold.bandStory > 0 && hold.bandMaster;
    if (hold.drag.mode == DragMode::Cells && hold.drag.control) {
        st.table = true;
        st.cellBlock = true;
    } else if (cur.active && cur.story > 0) {
        const model::StorySlot* slot = resolveStory(doc, cur.story);
        st.table = slot && slot->tableCell;
        if (slot && slot->kind == model::StoryKind::SubList && slot->control) {
            if (slot->control->kind == model::ControlKind::Footnote) st.note = 1;
            if (slot->control->kind == model::ControlKind::Endnote) st.note = 2;
        }
        st.cellStart = st.table && !cur.hasSelection && cur.paragraph == 0 &&
                       paint_detail::offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset) == 0;
    }
    CaretPlace caret;
    if (cur.active && cur.story > 0) {
        const int para = cur.focusParagraph >= 0 ? cur.focusParagraph : cur.paragraph;
        const int off = cur.focusParagraph >= 0 ? cur.focusOffset : cur.offset;
        if (para >= 0) caret = caretForCursor(doc, pages, model::CursorPos{cur.story, para, off});
    }
    if (!caret.hit) caret = hold.caret;
    if (!caret.hit || caret.story <= 0 || caret.paragraph < 0 || caret.page < 0 ||
        caret.page >= static_cast<int>(pages.size())) {
        return st;
    }

    const model::StoryMap& map = ensureStoryMap(doc);
    const model::StorySlot* slot = map.slot(caret.story);
    int sectionIndex = slot ? slot->sectionIndex : -1;
    if (sectionIndex < 0) {
        if (const model::StorySlot* root = map.slot(map.rootSectionStory(caret.story))) sectionIndex = root->sectionIndex;
    }

    CaretLineWalk walk;
    walk.story = caret.story;
    walk.paragraph = caret.paragraph;
    walk.charIndex = offsetToCharIndex(doc, pages, caret.story, caret.paragraph, caret.offset);
    walk.walk(pages[static_cast<std::size_t>(caret.page)], nullptr, 0);
    if (!walk.found) return st;

    st.active = true;
    st.section = std::max(0, sectionIndex) + 1;
    st.column = std::max(1, walk.foundColumn);
    st.line = walk.foundLine;
    st.character = walk.charIndex - walk.found->start + 1;
    return st;
}

void evictNodeMeasures(const Node& root) {
    std::unique_lock lock(gSpanMu);
    evictNodeMeasuresUnlocked(root);
}

void forgetLayoutNodeCaches() {
    // 새 트리의 노드가 옛 노드 주소에 앉으면 캐럿 캐시가 옛 개체 포인터와 옛 자리를 그대로 돌려준다.
    // 글자 측정은 내용으로 다시 확인하므로 남긴다.
    std::unique_lock lock(gSpanMu);
    lineBlockCache().clear();
    gPageStoryLinesCache.clear();
    gPageBlockSelectionCache.clear();
}

namespace {

// 문서가 그리는 글자를 접기 전에 뽑아 두는 일. 쉬는 프레임마다 조금씩 한다.
struct FontFoldPrep {
    const model::Document* doc = nullptr;
    std::vector<layout::DrawnGlyph> glyphs;
    std::size_t next = 0;
    bool listed = false;
    bool ready = false;
};

FontFoldPrep& fontFoldPrep() {
    static FontFoldPrep prep;
    return prep;
}

std::mutex gFontFoldMu;

// 접힌 파일의 TTF 를 그리기 쪽에서도 놓는다. gFaceMu·gFtMutex 를 잡고 부른다.
void releaseFontBytesLocked(const void* data) {
    for (Face& face : gOpenFaces) {
        if (face.bytes.get() != data) continue;
        face.bytes.reset();
        face.info = stbtt_fontinfo{};
        face.folded = true;
    }
    for (auto it = gFtFaces.begin(); it != gFtFaces.end();) {
        if (it->second.bytes.get() == data) {
            if (it->second.face) FT_Done_Face(it->second.face);
            it = gFtFaces.erase(it);
        } else {
            ++it;
        }
    }
}

// 오래 안 쓴 파일부터 need 바이트만큼 접는다. minIdleMs 안에 쓴 파일과 pinned 파일은 남긴다.
std::size_t foldFontFiles(std::size_t need, std::int64_t minIdleMs, const std::vector<const void*>& pinned) {
    if (!fontFoldPrep().ready) return 0;
    std::vector<layout::OpenFontFile> files = layout::openFontFiles();
    // 오래 안 쓴 것부터 접는다. 같은 초 안에 쓴 것끼리는 큰 파일부터 접어 적게 접고도 예산에 든다.
    std::sort(files.begin(), files.end(), [](const layout::OpenFontFile& a, const layout::OpenFontFile& b) {
        const std::int64_t aSecond = a.lastUseMs / 1000;
        const std::int64_t bSecond = b.lastUseMs / 1000;
        if (aSecond != bSecond) return aSecond < bSecond;
        return a.bytes > b.bytes;
    });
    const std::int64_t now = layout::fontClockMs();
    std::size_t released = 0;
    for (const layout::OpenFontFile& file : files) {
        if (released >= need) break;
        if (now - file.lastUseMs < minIdleMs) continue;
        if (std::find(pinned.begin(), pinned.end(), file.key) != pinned.end()) continue;
        std::unique_lock faceLock(gFaceMu);
        std::lock_guard ftLock(gFtMutex);
        const void* data = layout::foldFontFile(file.key);
        if (!data) continue;
        releaseFontBytesLocked(data);
        released += file.bytes;
        std::printf("{\"글꼴접기\":\"%s\",\"MB\":%.1f,\"쉰초\":%.0f}\n", file.faces.empty() ? "" : file.faces.front().c_str(),
                    static_cast<double>(file.bytes) / 1048576.0, static_cast<double>(now - file.lastUseMs) / 1000.0);
    }
    if (released > 0) std::fflush(stdout);
    return released;
}

class FontFilePool final : public CachePool {
public:
    const char* name() const override { return "fonts"; }
    std::size_t bytes() const override {
        std::size_t open = 0;
        for (const layout::OpenFontFile& file : layout::openFontFiles()) open += file.bytes;
        return open + layout::packedFontBytes();
    }
    // 메모리 신호로 비울 때: Spare 는 오래 안 쓴 글꼴, Recent 는 잠깐이라도 안 쓴 글꼴, Near 는 커서 글꼴 말고 모두.
    std::size_t release(CacheTier tier, std::size_t need) override {
        std::lock_guard lock(gFontFoldMu);
        const std::int64_t idle = tier == CacheTier::Spare ? kFontRecentMs : (tier == CacheTier::Recent ? 2000 : 0);
        return foldFontFiles(need, idle, pinnedFiles_);
    }
    std::vector<const void*> pinnedFiles_;
};

FontFilePool& fontFilePool() {
    static FontFilePool pool;
    static const bool registered = (registerCachePool(pool, kFontFileBudget), true);
    (void)registered;
    return pool;
}

}  // namespace

void resetFontFold() {
    std::lock_guard lock(gFontFoldMu);
    fontFoldPrep() = FontFoldPrep{};
}

void foldIdleFonts(const model::Document& doc, const std::function<std::uint32_t()>& cursorCharShape, double budgetMs) {
    FontFilePool& pool = fontFilePool();
    std::unique_lock lock(gFontFoldMu);
    FontFoldPrep& prep = fontFoldPrep();
    if (prep.doc != &doc) {
        prep = FontFoldPrep{};
        prep.doc = &doc;
    }
    if (!prep.ready) {
        // 예산 안이면 뽑을 일도 없다. 넘을 때만 문서 글자를 다 뽑아 두고 접는다.
        if (pool.bytes() <= cacheBudget(pool)) return;
        const auto started = std::chrono::steady_clock::now();
        const auto spent = [&] {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        };
        if (!prep.listed) {
            prep.glyphs = layout::documentDrawnGlyphs(doc);
            prep.listed = true;
        }
        lock.unlock();
        while (prep.next < prep.glyphs.size() && spent() < budgetMs) {
            const layout::DrawnGlyph& glyph = prep.glyphs[prep.next++];
            const GlyphMem* mem = glyphMemory(glyph.face, glyph.cp);
            if (!mem || !mem->face) continue;
            const std::string& face = glyphFaceName(mem);
            baseGlyphCommands(mem, false, false);
            if (glyph.bold) baseGlyphCommands(mem, true, designedBoldFace(face, true));
        }
        lock.lock();
        if (prep.next < prep.glyphs.size()) return;
        std::printf("{\"글꼴접기준비\":%zu}\n", prep.glyphs.size());
        std::fflush(stdout);
        prep.ready = true;
        prep.glyphs.clear();
        prep.glyphs.shrink_to_fit();
    }
    const std::size_t total = pool.bytes();
    const std::size_t budget = cacheBudget(pool);
    if (total <= budget) return;
    pool.pinnedFiles_.clear();
    for (const std::string& face : layout::charShapeFaces(doc, cursorCharShape ? cursorCharShape() : 0)) {
        if (const void* key = layout::fontFileKey(face)) pool.pinnedFiles_.push_back(key);
    }
    foldFontFiles(total - budget, kFontRecentMs, pool.pinnedFiles_);
}

void evictLineGlyph(const Node& line) {
    std::unique_lock lock(gSpanMu);
    lineBlockCache().erase(&line);
}

void invalidateLinePlaces(const std::vector<FrameInvalidation>& invalidations, bool fullInvalidation) {
    clearSelectionSheets();
    if (fullInvalidation) {
        std::scoped_lock lock(gPlaceMu, gSpanMu);
        linePlaceSeen().clear();
        linePlaces().clear();
        linePlacesByHash().clear();
        linePlacePages().clear();
        idleLineNote() = {};
        return;
    }
    if (invalidations.empty()) return;
    std::unique_lock lock(gPlaceMu);
    bool allPages = false;
    std::unordered_set<int> pages;
    for (const FrameInvalidation& inv : invalidations) {
        if (inv.page < 0) allPages = true;
        else pages.insert(inv.page);
    }
    auto dropKey = [&](const std::string& key) {
        linePlaceSeen().erase(key);
        linePlaces().erase(key);
        int page = -1;
        int kind = 0;
        std::vector<int> path;
        if (parseLinePlaceKey(key, page, kind, path)) {
            const auto found = linePlacePages().find(page);
            if (found != linePlacePages().end()) found->second.erase(key);
        }
    };
    if (allPages) {
        std::vector<std::string> drop;
        for (const std::string& key : linePlaceSeen()) {
            if (matchesAnyInvalidationKey(key, invalidations)) drop.push_back(key);
        }
        for (const std::string& key : drop) dropKey(key);
        return;
    }
    for (const int page : pages) {
        const auto found = linePlacePages().find(page);
        if (found == linePlacePages().end()) continue;
        std::vector<std::string> drop;
        for (const std::string& key : found->second) {
            if (matchesAnyInvalidationKey(key, invalidations)) drop.push_back(key);
        }
        for (const std::string& key : drop) dropKey(key);
    }
}

// ---------------------------------------------------------------------------
// 화면 글자·쪽 텍스처 판단. GPU 없이 돌아서 네이티브 시험이 같은 판단을 탄다.
// GPU 호출(텍스처 올리기·그리기)은 아래 #if 안에 있고, GPU 화면이 없는 빌드는 빈 함수다.

namespace {

void dropScreenTexture(int id);
void clearAtlasTextures();
void dropPageImage(int page);
void clearPageImages();
bool pageTexturesOn();

// 글자 크기마다 알파 마스크 한 장. Skia 스트라이크처럼 글꼴·글자·픽셀 크기·장평·굵기·기울임이 키다.
constexpr int kAtlasDim = 2048;
// 한 프레임에 마스크를 굽는 시간. 움직이는 중에는 화면 글자만 짧게 굽는다.
constexpr int kMaskMotionMicros = 3000;
constexpr int kMaskRestMicros = 8000;
// 움직이는 중 아틀라스 장 수 한도(장당 4MB). 정리는 멈춘 뒤에만 한다.
constexpr std::size_t kMotionAtlasPages = 16;

struct MaskLoc {
    int page = -1;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int bearingX = 0;
    int bearingY = 0;
    int ppem = 0;
    int zoom = 0;
    std::uint64_t seq = 0;
    bool missing = false;
    std::vector<std::uint8_t> alpha;
};

struct AtlasPage {
    std::vector<std::uint8_t> alpha;
    int cx = 0;
    int cy = 0;
    int rowH = 0;
    std::uint64_t seq = 0;
    std::uint64_t uploadedSeq = 0;
    bool needUpload = false;
    // 화면 텍스처로 올린 적이 있다. 텍스처 자체는 GPU 쪽이 같은 번호로 들고 있다.
    bool uploaded = false;
};

struct MaskFamily {
    const GlyphMem* mem = nullptr;
    int ratio = 100;
    int fitX = 1000;
    int fitY = 1000;
    bool bold = false;
    bool italic = false;
    bool operator==(const MaskFamily& other) const {
        return mem == other.mem && ratio == other.ratio && fitX == other.fitX && fitY == other.fitY &&
               bold == other.bold && italic == other.italic;
    }
};

struct MaskFamilyHash {
    std::size_t operator()(const MaskFamily& key) const noexcept {
        std::size_t h = reinterpret_cast<std::uintptr_t>(key.mem);
        h ^= static_cast<std::size_t>(key.ratio) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(key.fitX) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= static_cast<std::size_t>(key.fitY) + 0x9e3779b9u + (h << 6) + (h >> 2);
        if (key.bold) h ^= 0x85ebca6bu;
        if (key.italic) h ^= 0xc2b2ae35u;
        return h;
    }
};

struct MaskId {
    MaskFamily family;
    int ppem = 0;
    bool operator==(const MaskId& other) const { return ppem == other.ppem && family == other.family; }
};

struct MaskIdHash {
    std::size_t operator()(const MaskId& key) const noexcept {
        std::size_t h = MaskFamilyHash{}(key.family);
        h ^= static_cast<std::size_t>(key.ppem) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

MaskFamily maskFamily(const FrameGlyph& glyph) {
    const int fitX = glyph.mem && glyph.mem->fitX > 0 ? glyph.mem->fitX : 1000;
    const int fitY = glyph.mem && glyph.mem->fitY > 0 ? glyph.mem->fitY : 1000;
    return MaskFamily{glyph.mem, glyph.ratio, fitX, fitY, glyph.bold, glyph.italic};
}

MaskId maskId(const FrameGlyph& glyph, int ppem) { return MaskId{maskFamily(glyph), ppem}; }

std::unordered_map<MaskId, MaskLoc, MaskIdHash>& glyphMasks() {
    static std::unordered_map<MaskId, MaskLoc, MaskIdHash> masks;
    return masks;
}

std::unordered_map<MaskFamily, std::vector<int>, MaskFamilyHash>& glyphFamilies() {
    static std::unordered_map<MaskFamily, std::vector<int>, MaskFamilyHash> families;
    return families;
}

std::vector<AtlasPage>& glyphPages() {
    static std::vector<AtlasPage> pages;
    return pages;
}

void collectRenderMemory(std::vector<std::pair<std::string, std::size_t>>& out) {
    std::size_t glyphCount = 0;
    std::size_t glyphBytes = 0;
    std::size_t quadBytes = 0;
    {
        std::lock_guard lock(frameGlyphMutex());
        for (const auto& [id, frame] : frameGlyphs()) {
            glyphCount += frame.glyphs.size();
            glyphBytes += frame.glyphs.capacity() * sizeof(FrameGlyph) + frame.hosts.capacity() * sizeof(TextHost);
        }
        for (const auto& [id, cached] : cachedSpriteQuads()) quadBytes += cached.quads.capacity() * sizeof(LocalGlyphQuad);
    }
    std::size_t outlineBytes = 0;
    {
        std::shared_lock lock(gOutlineMu);
        for (const auto& [key, commands] : gCommands) outlineBytes += commands.capacity() * sizeof(GlyphCommand);
        for (const auto& [key, commands] : gCurves) outlineBytes += commands.capacity() * sizeof(GlyphCommand);
    }
    std::size_t maskBytes = 0;
    std::size_t atlasBytes = 0;
    {
        std::lock_guard lock(glyphAtlasMutex());
        for (const auto& [key, loc] : glyphMasks()) maskBytes += loc.alpha.capacity();
        for (const AtlasPage& page : glyphPages()) atlasBytes += page.alpha.capacity();
    }
    std::size_t placeBytes = 0;
    std::size_t placeHashBytes = 0;
    std::size_t placeCount = 0;
    {
        std::shared_lock lock(gPlaceMu);
        // 두 표가 같은 자리를 가리키면 한 번만 센다. 해시 표 몫은 그 표에만 있는 자리다.
        std::unordered_set<const LinePlaces*> counted;
        for (const auto& [key, places] : linePlaces()) {
            placeBytes += key.capacity() + 48;
            if (!places || !counted.insert(places.get()).second) continue;
            placeBytes += places->spots.capacity() * sizeof(GlyphSpot) + sizeof(LinePlaces);
            placeCount += places->spots.size();
        }
        for (const auto& [hash, places] : linePlacesByHash()) {
            placeHashBytes += 32;
            if (!places || !counted.insert(places.get()).second) continue;
            placeHashBytes += places->spots.capacity() * sizeof(GlyphSpot) + sizeof(LinePlaces);
        }
    }
    out.emplace_back("linePlaces", placeBytes);
    out.emplace_back("linePlacesByHash", placeHashBytes);
    out.emplace_back("linePlaceGlyphCount", placeCount);
    out.emplace_back("frameGlyphs", glyphBytes);
    out.emplace_back("frameGlyphCount", glyphCount);
    out.emplace_back("glyphQuads", quadBytes);
    out.emplace_back("outlines", outlineBytes);
    out.emplace_back("maskCopies", maskBytes);
    out.emplace_back("atlasCpu", atlasBytes);
}

// 글자 마스크 아틀라스. 지금은 크기만 알리고 버리지는 않는다.
class AtlasPool final : public CachePool {
public:
    const char* name() const override { return "glyphAtlas"; }
    std::size_t bytes() const override {
        std::lock_guard lock(glyphAtlasMutex());
        std::size_t total = 0;
        for (const AtlasPage& page : glyphPages()) total += page.alpha.size();
        return total;
    }
    std::size_t release(CacheTier, std::size_t) override { return 0; }
};

void registerAtlasPool() {
    static AtlasPool pool;
    static const bool registered = (registerCachePool(pool, kNoCacheBudget), true);
    (void)registered;
}

bool& glyphMaskPending() {
    static bool pending = false;
    return pending;
}

constexpr float kDotMaxPpem = 3.5f;
constexpr int kDotSizes = 3;
constexpr int kDotVariants = 4;

float screenPpemFor(const FrameGlyph& glyph, int framePercent, int layoutZoom) {
    const float exact = glyph.exactPpem > 0.f ? glyph.exactPpem : static_cast<float>(glyph.ppem);
    if (exact <= 0.f) return 1.f;
    if (layoutZoom <= 0 || framePercent <= 0 || layoutZoom == framePercent) return exact;
    return exact * static_cast<float>(layoutZoom) / static_cast<float>(framePercent);
}

std::array<std::array<MaskLoc, kDotVariants>, kDotSizes>& dotMasks() {
    static std::array<std::array<MaskLoc, kDotVariants>, kDotSizes> dots;
    return dots;
}

std::uint32_t& dotMaskGen() {
    static std::uint32_t gen = 0;
    return gen;
}

int basePpem(const FrameGlyph& glyph, int framePercent) {
    const float exact = glyph.exactPpem > 0.f ? glyph.exactPpem : static_cast<float>(glyph.ppem);
    if (exact <= 0.f) return 1;
    if (framePercent <= 0 || framePercent == 100) return std::max(1, glyph.ppem);
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(exact) * 100.0 / static_cast<double>(framePercent))));
}

int maskUnclampedPpemFor(const FrameGlyph& glyph, int framePercent, int layoutZoom, int maskZoom) {
    const float exact = glyph.exactPpem > 0.f ? glyph.exactPpem : static_cast<float>(glyph.ppem);
    if (exact <= 0.f) return 1;
    if (maskZoom <= 0 || framePercent <= 0) return std::max(1, glyph.ppem);
    const int useZoom = maskZoom > 0 ? maskZoom : (layoutZoom > 0 ? layoutZoom : framePercent);
    if (useZoom == framePercent) return std::max(1, glyph.ppem);
    const double scaled = static_cast<double>(exact) * static_cast<double>(useZoom) / static_cast<double>(framePercent);
    return std::max(1, static_cast<int>(std::lround(scaled)));
}

int maskPpemFor(const FrameGlyph& glyph, int framePercent, int layoutZoom, int maskZoom) {
    return glyphMaskPpem(maskUnclampedPpemFor(glyph, framePercent, layoutZoom, maskZoom));
}

// 글자 하나를 알파 마스크로 찍는다. 자리 잡기·크기 고르기는 이 함수 밖에서 정한다.
bool rasterGlyphAlpha(const FrameGlyph& glyph, int ppem, MaskLoc& loc) {
    if (!glyph.mem || !glyph.mem->face || ppem <= 0) return false;
    const int fitX = glyph.mem->fitX > 0 ? glyph.mem->fitX : 1000;
    const int fitY = glyph.mem->fitY > 0 ? glyph.mem->fitY : 1000;
    constexpr int pad = 2;
    const auto& curves = hangulCurves(glyph.mem, glyph.bold, glyph.italic, ppem);
    SkPath path = glyphPath(curves, static_cast<float>(ppem), glyph.ratio, 0.f, 0.f, fitX, fitY);
    if (path.isEmpty()) return false;
    const SkRect bounds = path.computeTightBounds();
    if (bounds.isEmpty()) return false;
    const int x0 = static_cast<int>(std::floor(bounds.fLeft)) - pad;
    const int y0 = static_cast<int>(std::floor(bounds.fTop)) - pad;
    const int x1 = static_cast<int>(std::ceil(bounds.fRight)) + pad;
    const int y1 = static_cast<int>(std::ceil(bounds.fBottom)) + pad;
    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w <= 0 || h <= 0 || w > kAtlasDim || h > kAtlasDim) return false;
    const SkImageInfo info = SkImageInfo::MakeA8(w, h);
    sk_sp<SkSurface> surface = SkSurfaces::Raster(info);
    if (!surface) return false;
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    canvas->translate(static_cast<float>(-x0), static_cast<float>(-y0));
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kFill_Style);
    paint.setColor(SK_ColorBLACK);
    canvas->drawPath(path, paint);
    loc.alpha.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    if (!surface->readPixels(info, loc.alpha.data(), w, 0, 0)) return false;
    loc.w = w;
    loc.h = h;
    loc.bearingX = x0;
    loc.bearingY = y0;
    loc.ppem = ppem;
    return true;
}

void blitMask(AtlasPage& page, MaskLoc& loc) {
    if (page.alpha.empty()) page.alpha.assign(static_cast<std::size_t>(kAtlasDim) * kAtlasDim, 0);
    for (int row = 0; row < loc.h; ++row) {
        std::memcpy(page.alpha.data() + static_cast<std::size_t>(loc.y + row) * kAtlasDim + loc.x,
                    loc.alpha.data() + static_cast<std::size_t>(row) * loc.w, static_cast<std::size_t>(loc.w));
    }
    loc.seq = ++page.seq;
}

bool placeOnPage(AtlasPage& page, MaskLoc& loc) {
    constexpr int gap = 1;
    const int needW = loc.w + gap;
    const int needH = loc.h + gap;
    if (needW > kAtlasDim || needH > kAtlasDim) return false;
    int x = page.cx;
    int y = page.cy;
    int rowH = page.rowH;
    if (x + needW > kAtlasDim) {
        y += rowH;
        x = 0;
        rowH = 0;
    }
    if (y + needH > kAtlasDim) return false;
    loc.x = x;
    loc.y = y;
    page.cx = x + needW;
    page.cy = y;
    page.rowH = std::max(rowH, needH);
    blitMask(page, loc);
    return true;
}

bool packMask(MaskLoc& loc) {
    std::vector<AtlasPage>& pages = glyphPages();
    for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
        if (!placeOnPage(pages[static_cast<std::size_t>(i)], loc)) continue;
        loc.page = i;
        return true;
    }
    pages.emplace_back();
    if (!placeOnPage(pages.back(), loc)) {
        pages.pop_back();
        return false;
    }
    loc.page = static_cast<int>(pages.size()) - 1;
    return true;
}

void rememberFamily(const MaskFamily& family, int ppem) {
    std::vector<int>& sizes = glyphFamilies()[family];
    if (std::find(sizes.begin(), sizes.end(), ppem) == sizes.end()) sizes.push_back(ppem);
}

void ensureDotMasks() {
    const std::uint32_t gen = maskCacheGen();
    if (dotMaskGen() == gen && !glyphPages().empty() && dotMasks()[0][0].page >= 0) return;
    dotMaskGen() = gen;
    static constexpr std::uint8_t kDot1[kDotVariants][1] = {
        {195},
        {220},
        {175},
        {205},
    };
    static constexpr std::uint8_t kDot2[kDotVariants][4] = {
        {215, 165, 175, 200},
        {195, 205, 215, 185},
        {220, 150, 185, 165},
        {175, 215, 200, 180},
    };
    static constexpr std::uint8_t kDot3[kDotVariants][6] = {
        {210, 175, 185, 220, 145, 195},
        {195, 210, 220, 175, 190, 205},
        {220, 185, 160, 210, 110, 165},
        {180, 205, 210, 190, 200, 215},
    };
    for (int v = 0; v < kDotVariants; ++v) {
        MaskLoc& d1 = dotMasks()[0][static_cast<std::size_t>(v)];
        d1 = MaskLoc{};
        d1.w = 1;
        d1.h = 1;
        d1.bearingX = 0;
        d1.bearingY = -1;
        d1.ppem = 1;
        d1.zoom = 100;
        d1.alpha.assign(kDot1[v], kDot1[v] + 1);
        packMask(d1);

        MaskLoc& d2 = dotMasks()[1][static_cast<std::size_t>(v)];
        d2 = MaskLoc{};
        d2.w = 2;
        d2.h = 2;
        d2.bearingX = 0;
        d2.bearingY = -2;
        d2.ppem = 2;
        d2.zoom = 100;
        d2.alpha.assign(kDot2[v], kDot2[v] + 4);
        packMask(d2);

        MaskLoc& d3 = dotMasks()[2][static_cast<std::size_t>(v)];
        d3 = MaskLoc{};
        d3.w = 2;
        d3.h = 3;
        d3.bearingX = 0;
        d3.bearingY = -3;
        d3.ppem = 3;
        d3.zoom = 100;
        d3.alpha.assign(kDot3[v], kDot3[v] + 6);
        packMask(d3);
    }
}

void rebuildAtlas() {
    glyphPages().clear();
    dotMaskGen() = 0;
    ensureDotMasks();
    for (auto& item : glyphMasks()) {
        MaskLoc& loc = item.second;
        loc.page = -1;
        if (loc.missing || loc.alpha.empty()) continue;
        packMask(loc);
    }
}

void reindexMaskFamilies() {
    glyphFamilies().clear();
    for (const auto& item : glyphMasks()) rememberFamily(item.first.family, item.second.ppem);
}

const MaskLoc* readyMask(const MaskId& key);
const MaskLoc* maskFor(const FrameGlyph& glyph, int want, int* drawnPpem, bool* outMissing = nullptr);

void releaseUnusedZoomMasks(const std::vector<ScreenSprite>& sprites, int layoutZoom, int maskZoom) {
    // 아틀라스 페이지가 6장(24MB) 이하이면 줌 마스크를 매 프레임 지우거나 재패킹하지 않는다.
    if (sprites.empty() || glyphPages().size() <= 6) return;
    std::unordered_set<MaskId, MaskIdHash> used;
    for (const ScreenSprite& sprite : sprites) {
        const auto found = frameGlyphs().find(sprite.id);
        if (found == frameGlyphs().end()) continue;
        const int effLayout = layoutZoom > 0 ? layoutZoom : (sprite.percent > 0 ? sprite.percent : 100);
        for (const FrameGlyph& glyph : found->second.glyphs) {
            if (screenPpemFor(glyph, sprite.percent, effLayout) <= kDotMaxPpem) continue;
            const int want = maskPpemFor(glyph, sprite.percent, layoutZoom, maskZoom);
            used.insert(maskId(glyph, want));
            if (readyMask(maskId(glyph, want))) continue;
            int drawn = 0;
            if (const MaskLoc* fallback = maskFor(glyph, want, &drawn)) used.insert(maskId(glyph, fallback->ppem));
        }
    }
    const int activeZoom = maskZoom > 0 ? maskZoom : layoutZoom;
    const int targetZoom = maskZoomJobTarget();
    bool dropped = false;
    for (auto it = glyphMasks().begin(); it != glyphMasks().end();) {
        if (it->second.zoom == 100 || it->second.zoom == activeZoom ||
            (targetZoom > 0 && it->second.zoom == targetZoom) || used.count(it->first)) {
            ++it;
            continue;
        }
        dropped = true;
        it = glyphMasks().erase(it);
    }
    if (!dropped) return;
    ++maskCacheGen();
    ++maskBuiltSeq();
    spriteMaskReady().clear();
    cachedSpriteQuads().clear();
    reindexMaskFamilies();
    rebuildAtlas();
}

const MaskLoc* readyMask(const MaskId& key) {
    const auto found = glyphMasks().find(key);
    if (found == glyphMasks().end() || found->second.missing || found->second.page < 0) return nullptr;
    return &found->second;
}

const MaskLoc* maskFor(const FrameGlyph& glyph, int want, int* drawnPpem, bool* outMissing) {
    if (outMissing) *outMissing = false;
    if (!glyph.mem) {
        if (outMissing) *outMissing = true;
        return nullptr;
    }
    const std::uint32_t gen = maskCacheGen();
    const std::uint32_t builtSeq = maskBuiltSeq();
    if (glyph.cachedGen == gen && glyph.cachedWant == want && glyph.cachedMask) {
        const auto* cached = static_cast<const MaskLoc*>(glyph.cachedMask);
        if (cached->missing) {
            if (outMissing) *outMissing = true;
            return nullptr;
        }
        if (cached->page >= 0 &&
            (cached->ppem == want || glyph.cachedBuiltSeq == builtSeq)) {
            if (drawnPpem) *drawnPpem = glyph.cachedDrawnPpem;
            return cached;
        }
    }
    if (const auto exactIt = glyphMasks().find(maskId(glyph, want)); exactIt != glyphMasks().end()) {
        if (exactIt->second.missing) {
            glyph.cachedWant = want;
            glyph.cachedDrawnPpem = exactIt->second.ppem;
            glyph.cachedGen = gen;
            glyph.cachedBuiltSeq = builtSeq;
            glyph.cachedMask = &exactIt->second;
            if (outMissing) *outMissing = true;
            return nullptr;
        }
        if (exactIt->second.page >= 0) {
            glyph.cachedWant = want;
            glyph.cachedDrawnPpem = exactIt->second.ppem;
            glyph.cachedGen = gen;
            glyph.cachedBuiltSeq = builtSeq;
            glyph.cachedMask = &exactIt->second;
            if (drawnPpem) *drawnPpem = exactIt->second.ppem;
            return &exactIt->second;
        }
    }
    const std::vector<int>* sizes = nullptr;
    if (const auto family = glyphFamilies().find(maskFamily(glyph)); family != glyphFamilies().end()) sizes = &family->second;
    if (!sizes || sizes->empty()) return nullptr;
    // 원하는 크기가 아직 없으면 그보다 큰 것 중 가장 작은 것(두 배 안쪽)을 줄여 쓴다. 줄일 때는 네 점을 고르게 읽어
    // 획이 남고, 늘리면 흐려진다. 그런 것이 없을 때만 가장 가까운 것을 쓴다.
    const MaskLoc* best = nullptr;
    const MaskLoc* larger = nullptr;
    int dist = 1 << 30;
    for (const int ppem : *sizes) {
        const auto found = glyphMasks().find(maskId(glyph, ppem));
        if (found == glyphMasks().end() || found->second.missing || found->second.page < 0) continue;
        if (ppem >= want && ppem <= want * 2 && (!larger || ppem < larger->ppem)) larger = &found->second;
        const int gap = std::abs(ppem - want);
        if (gap < dist) {
            dist = gap;
            best = &found->second;
        }
    }
    if (larger) best = larger;
    if (!best) return nullptr;
    glyph.cachedWant = want;
    glyph.cachedDrawnPpem = best->ppem;
    glyph.cachedGen = gen;
    glyph.cachedBuiltSeq = builtSeq;
    glyph.cachedMask = best;
    if (drawnPpem) *drawnPpem = best->ppem;
    return best;
}

void ensureGlyphMasks(const std::vector<ScreenSprite>& sprites, int layoutZoom, int maskZoom, int viewW, int viewH,
                      bool dragging) {
    ensureDotMasks();
    if (!dragging) releaseUnusedZoomMasks(sprites, layoutZoom, maskZoom);
    // 굽는 양은 개수가 아니라 시간으로 끊는다. 글자 크기에 따라 한 장 굽는 값이 크게 다르다.
    // 움직이는 중에는 아틀라스를 정리하지 않으니, 장 수가 한도에 닿으면 굽기를 멈추고 가까운 마스크로 그린다.
    int made = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::microseconds(dragging ? kMaskMotionMicros : kMaskRestMicros);
    const bool atlasFull = dragging && glyphPages().size() >= kMotionAtlasPages;
    bool more = false;
    const int zoom = maskZoom > 0 ? maskZoom : layoutZoom;
    const std::uint32_t gen = maskCacheGen();

    auto bakePass = [&](bool visibleOnly) {
        for (const ScreenSprite& sprite : sprites) {
            if (visibleOnly && viewW > 0 && viewH > 0) {
                if (sprite.x >= viewW || sprite.y >= viewH || sprite.x + sprite.w <= 0 || sprite.y + sprite.h <= 0) {
                    continue;
                }
            }
            const auto readyIt = spriteMaskReady().find(sprite.id);
            if (readyIt != spriteMaskReady().end() && readyIt->second.gen == gen &&
                readyIt->second.spritePercent == sprite.percent) {
                if (readyIt->second.zoom == zoom) {
                    continue;
                }
            }
            const auto found = frameGlyphs().find(sprite.id);
            if (found == frameGlyphs().end()) continue;
            const int effLayout = layoutZoom > 0 ? layoutZoom : (sprite.percent > 0 ? sprite.percent : 100);
            const float frameX = sprite.sw > 0 ? static_cast<float>(sprite.w) / static_cast<float>(sprite.sw) : 1.f;
            const float frameY = sprite.sh > 0 ? static_cast<float>(sprite.h) / static_cast<float>(sprite.sh) : 1.f;
            bool spriteAllReady = true;
            for (const FrameGlyph& glyph : found->second.glyphs) {
                if (!glyph.mem) continue;
                if (screenPpemFor(glyph, sprite.percent, effLayout) <= kDotMaxPpem) continue;
                const int want = maskPpemFor(glyph, sprite.percent, layoutZoom, maskZoom);
                if (glyph.cachedGen == gen && glyph.cachedWant == want && glyph.cachedMask) {
                    const auto* cached = static_cast<const MaskLoc*>(glyph.cachedMask);
                    if (cached->ppem == want && (cached->missing || cached->page >= 0)) continue;
                }
                bool onScreen = true;
                if (viewW > 0 && viewH > 0) {
                    const float penX = std::round(static_cast<float>(sprite.x) + glyph.penX * frameX);
                    const float penY = std::round(static_cast<float>(sprite.y) + glyph.penY * frameY);
                    const float margin = static_cast<float>(std::max(want, 8));
                    onScreen = penX >= -margin && penX <= static_cast<float>(viewW) + margin &&
                               penY >= -margin && penY <= static_cast<float>(viewH) + margin;
                }
                if (visibleOnly != onScreen) {
                    const MaskId key = maskId(glyph, want);
                    if (const auto ex = glyphMasks().find(key); ex != glyphMasks().end()) {
                        glyph.cachedWant = want;
                        glyph.cachedDrawnPpem = ex->second.ppem;
                        glyph.cachedGen = gen;
                        glyph.cachedBuiltSeq = maskBuiltSeq();
                        glyph.cachedMask = &ex->second;
                    } else {
                        spriteAllReady = false;
                    }
                    continue;
                }
                const MaskId key = maskId(glyph, want);
                if (const auto ex = glyphMasks().find(key); ex != glyphMasks().end()) {
                    glyph.cachedWant = want;
                    glyph.cachedDrawnPpem = ex->second.ppem;
                    glyph.cachedGen = gen;
                    glyph.cachedBuiltSeq = maskBuiltSeq();
                    glyph.cachedMask = &ex->second;
                    continue;
                }
                // 움직이는 중에도 지금 배율 마스크를 굽는다. 시간이나 아틀라스가 다하면 남은 글자는 가까운 마스크로 그린다.
                if (atlasFull || (made > 0 && std::chrono::steady_clock::now() >= deadline)) {
                    more = true;
                    return;
                }
                MaskLoc loc;
                loc.zoom = want == basePpem(glyph, sprite.percent) ? 100 : zoom;
                loc.ppem = want;
                if (!rasterGlyphAlpha(glyph, want, loc) || !packMask(loc)) {
                    loc.missing = true;
                    loc.alpha.clear();
                    loc.page = -1;
                }
                rememberFamily(maskFamily(glyph), want);
                auto [insIt, _] = glyphMasks().emplace(key, std::move(loc));
                const std::uint32_t newBuiltSeq = ++maskBuiltSeq();
                glyph.cachedWant = want;
                glyph.cachedDrawnPpem = insIt->second.ppem;
                glyph.cachedGen = gen;
                glyph.cachedBuiltSeq = newBuiltSeq;
                glyph.cachedMask = &insIt->second;
                ++made;
            }
            if (spriteAllReady) {
                spriteMaskReady()[sprite.id] = SpriteMaskState{zoom, sprite.percent, gen};
            }
        }
    };

    // 확대·축소가 끝나면 화면에 보이는 글리프부터 먼저 정밀 래스터화하고, 다 끝나면 화면 밖 마스크도 구워놓는다.
    bakePass(true);
    if (!dragging && !more) {
        bakePass(false);
    }
    glyphMaskPending() = more;
}

// 화면에 찍을 글자 마스크 한 장. 펜 자리 (x, y) 에 마스크를 scale 배로 놓는다.
struct GlyphQuad {
    float scale = 1.f;
    float x = 0.f;
    float y = 0.f;
    GlyphRect tex;
    std::uint32_t color = 0;
    bool turn = false;  // 펜(penX, penY)을 축으로 시계 방향 90° 돌린다
    float penX = 0.f;
    float penY = 0.f;
};

// ARGB 에서 알파는 두고 색만 뒤집는다.
std::uint32_t invertColor(std::uint32_t color) { return (color & 0xFF000000u) | (~color & 0x00FFFFFFu); }

bool hasActiveHold() {
    const PointerHold& hold = pointerHold();
    if (hold.composing && hold.compStory > 0 && hold.compPara >= 0 && hold.compEndOff > hold.compStartOff &&
        caretBlinkPhase() == 0) {
        return true;
    }
    if (hold.story <= 0) return false;
    return hold.anchorPara != hold.focusPara || hold.anchorOff != hold.focusOff;
}

// 고른 구간과 조합 중인 글자를 뒤집는다. 잉크가 칸에 닿는지만 보면 다음 줄이 따라 뒤집힌다.
bool offsetInHold(int story, int paragraph, int offset) {
    const PointerHold& hold = pointerHold();
    if (offset < 0 || paragraph < 0 || story <= 0) return false;
    if (hold.composing && hold.compStory == story && hold.compPara == paragraph &&
        hold.compEndOff > hold.compStartOff && offset >= hold.compStartOff && offset < hold.compEndOff) {
        return caretBlinkPhase() == 0;
    }
    if (hold.story != story) return false;
    int fromParagraph = hold.anchorPara;
    int fromOffset = hold.anchorOff;
    int toParagraph = hold.focusPara;
    int toOffset = hold.focusOff;
    if (toParagraph < fromParagraph || (toParagraph == fromParagraph && toOffset < fromOffset)) {
        std::swap(fromParagraph, toParagraph);
        std::swap(fromOffset, toOffset);
    }
    if (fromParagraph == toParagraph && fromOffset == toOffset) return false;
    const bool afterStart = paragraph > fromParagraph || (paragraph == fromParagraph && offset >= fromOffset);
    const bool beforeEnd = paragraph < toParagraph || (paragraph == toParagraph && offset < toOffset);
    return afterStart && beforeEnd;
}

bool glyphInSelection(const FrameGlyphs& frame, const FrameGlyph& glyph) {
    if (offsetInHold(glyph.story, glyph.paragraph, glyph.offset)) return true;
    // 수식은 칸 픽셀이라 블록에 걸리면 뒤집힌다. 개체 안 글자는 다른 이야기라 그 개체의 칸으로 본다.
    for (int i = 0; i < glyph.hostCount; ++i) {
        const std::size_t at = static_cast<std::size_t>(glyph.hostFirst + i);
        if (at >= frame.hosts.size()) break;
        const TextHost& host = frame.hosts[at];
        if (offsetInHold(host.story, host.paragraph, host.offset)) return true;
    }
    return false;
}

void appendGlyphQuads(const ScreenSprite& sprite, int layoutZoom, int maskZoom, int viewW, int viewH,
                      bool activeHold, std::vector<std::vector<GlyphQuad>>& sharp,
                      std::vector<std::vector<GlyphQuad>>& scaled, GlyphRect& batchBounds, bool& hasPendingGlyphs,
                      bool* outAllExact = nullptr) {
    const auto found = frameGlyphs().find(sprite.id);
    if (found == frameGlyphs().end() || sprite.sw <= 0 || sprite.sh <= 0) {
        if (outAllExact) *outAllExact = true;
        return;
    }
    if (viewW > 0 && viewH > 0) {
        if (sprite.x >= viewW || sprite.y >= viewH || sprite.x + sprite.w <= 0 || sprite.y + sprite.h <= 0) {
            if (outAllExact) *outAllExact = true;
            return;
        }
    }
    const FrameGlyphs& frameGlyphSet = found->second;
    const std::vector<FrameGlyph>& glyphs = frameGlyphSet.glyphs;
    const int effLayout = layoutZoom > 0 ? layoutZoom : (sprite.percent > 0 ? sprite.percent : 100);
    const int effMask = maskZoom > 0 ? maskZoom : effLayout;
    const std::uint32_t gen = maskCacheGen();
    const std::uint32_t curBuiltSeq = maskBuiltSeq();
    CachedSpriteQuads& cached = cachedSpriteQuads()[sprite.id];
    const bool maskHit = (cached.sw == sprite.sw && cached.sh == sprite.sh &&
                          cached.percent == sprite.percent &&
                          cached.effMask == effMask &&
                          cached.maskGen == gen &&
                          (cached.allExactMask || cached.builtSeq == curBuiltSeq));
    const bool layoutHit = maskHit && (cached.w == sprite.w && cached.h == sprite.h && cached.effLayout == effLayout);
    const float spritePctF = static_cast<float>(std::max(1, sprite.percent));
    const bool canRescaleInPlace =
        maskHit && !layoutHit && !cached.hasAnyDot &&
        (cached.minBasePpem * static_cast<float>(effLayout) > kDotMaxPpem * spritePctF);

    if (canRescaleInPlace) {
        cached.w = sprite.w;
        cached.h = sprite.h;
        cached.effLayout = effLayout;
        const float frameX = static_cast<float>(sprite.w) / static_cast<float>(sprite.sw);
        const float frameY = static_cast<float>(sprite.h) / static_cast<float>(sprite.sh);
        const float ppemRatio =
            (layoutZoom <= 0 || sprite.percent <= 0 || layoutZoom == sprite.percent)
                ? 1.f
                : (static_cast<float>(layoutZoom) / static_cast<float>(sprite.percent));
        for (LocalGlyphQuad& lq : cached.quads) {
            const float sPpem = lq.basePpem * ppemRatio;
            const float scale = (lq.drawnPpem == lq.unclampedWant && effLayout == effMask)
                                    ? 1.f
                                    : (sPpem * lq.invDrawnPpem);
            const float localPenX = std::round(lq.rawPenX * frameX);
            const float localPenY = std::round(lq.rawPenY * frameY);
            lq.relX = localPenX + lq.bearingX * scale;
            lq.relY = localPenY + lq.bearingY * scale;
            lq.penRelX = localPenX;
            lq.penRelY = localPenY;
            lq.scale = scale;
            lq.stretched = std::abs(scale - 1.f) > 0.005f;
        }
    } else if (!layoutHit) {
        cached.w = sprite.w;
        cached.h = sprite.h;
        cached.sw = sprite.sw;
        cached.sh = sprite.sh;
        cached.percent = sprite.percent;
        cached.effLayout = effLayout;
        cached.effMask = effMask;
        cached.maskGen = gen;
        cached.builtSeq = curBuiltSeq;
        cached.minBasePpem = 1000.f;
        cached.allExactMask = true;
        cached.hasAnyDot = false;
        cached.quads.clear();
        cached.quads.reserve(glyphs.size());

        const float frameX = static_cast<float>(sprite.w) / static_cast<float>(sprite.sw);
        const float frameY = static_cast<float>(sprite.h) / static_cast<float>(sprite.sh);
        int lastDotX = -100000;
        int lastDotY = -100000;
        int lastDotPara = -2;
        std::uint32_t lastDotColor = 0;

        for (std::size_t gi = 0; gi < glyphs.size(); ++gi) {
            const FrameGlyph& glyph = glyphs[gi];
            const float bPpem =
                glyph.exactPpem > 0.f ? glyph.exactPpem : std::max(1.f, static_cast<float>(glyph.ppem));
            if (bPpem < cached.minBasePpem) cached.minBasePpem = bPpem;
            const float sPpem = screenPpemFor(glyph, sprite.percent, effLayout);
            if (sPpem <= kDotMaxPpem) {
                cached.hasAnyDot = true;
                const float localPenX = std::round(glyph.penX * frameX);
                const float localPenY = std::round(glyph.penY * frameY);
                const int dotIdx = sPpem <= 1.65f ? 0 : (sPpem <= 2.55f ? 1 : 2);
                const int dotH = dotIdx + 1;
                const int ix = static_cast<int>(localPenX);
                const int iy = static_cast<int>(localPenY) - dotH;
                const int minStep = (dotIdx == 2) ? 1 : 0;
                if (glyph.paragraph == lastDotPara && iy == lastDotY && glyph.color == lastDotColor && ix >= lastDotX &&
                    ix <= lastDotX + minStep) {
                    continue;
                }
                lastDotX = ix;
                lastDotY = iy;
                lastDotPara = glyph.paragraph;
                lastDotColor = glyph.color;
                const std::size_t h = (reinterpret_cast<std::uintptr_t>(glyph.mem) >> 4) ^
                                      static_cast<std::size_t>(glyph.offset * 131 + glyph.paragraph * 17);
                const MaskLoc& dot = dotMasks()[static_cast<std::size_t>(dotIdx)][h & 3u];
                if (dot.page < 0 || dot.page >= static_cast<int>(glyphPages().size())) continue;
                LocalGlyphQuad lq;
                lq.relX = static_cast<float>(ix);
                lq.relY = static_cast<float>(iy);
                lq.scale = 1.f;
                lq.rawPenX = glyph.penX;
                lq.rawPenY = glyph.penY;
                lq.basePpem = bPpem;
                lq.tex = GlyphRect::xywh(static_cast<float>(dot.x), static_cast<float>(dot.y),
                                         static_cast<float>(dot.w), static_cast<float>(dot.h));
                lq.color = glyph.color;
                lq.atlasPage = dot.page;
                lq.seq = dot.seq;
                lq.glyphIndex = static_cast<std::uint32_t>(gi);
                lq.stretched = false;
                cached.quads.push_back(lq);
                continue;
            }
            const int unclampedWant = maskUnclampedPpemFor(glyph, sprite.percent, layoutZoom, maskZoom);
            const int want = glyphMaskPpem(unclampedWant);
            int drawnPpem = want;
            bool isMissing = false;
            const MaskLoc* mask = maskFor(glyph, want, &drawnPpem, &isMissing);
            if (!mask || drawnPpem <= 0 || want <= 0) {
                if (!isMissing) cached.allExactMask = false;
                continue;
            }
            // 다른 크기로 대신 그렸으면 제 크기가 구워질 때 다시 짠다.
            if (drawnPpem != want) {
                cached.allExactMask = false;
            }
            const float invDrawn = 1.f / static_cast<float>(drawnPpem);
            const float scale = (drawnPpem == unclampedWant && effLayout == effMask)
                                    ? 1.f
                                    : (sPpem * invDrawn);
            const float localPenX = std::round(glyph.penX * frameX);
            const float localPenY = std::round(glyph.penY * frameY);
            if (mask->page < 0 || mask->page >= static_cast<int>(glyphPages().size())) continue;
            LocalGlyphQuad lq;
            lq.rawPenX = glyph.penX;
            lq.rawPenY = glyph.penY;
            lq.basePpem = bPpem;
            lq.invDrawnPpem = invDrawn;
            lq.bearingX = static_cast<float>(mask->bearingX);
            lq.bearingY = static_cast<float>(mask->bearingY);
            lq.drawnPpem = drawnPpem;
            lq.unclampedWant = unclampedWant;
            lq.relX = localPenX + lq.bearingX * scale;
            lq.relY = localPenY + lq.bearingY * scale;
            lq.penRelX = localPenX;
            lq.penRelY = localPenY;
            lq.scale = scale;
            lq.tex = GlyphRect::xywh(static_cast<float>(mask->x), static_cast<float>(mask->y),
                                     static_cast<float>(mask->w), static_cast<float>(mask->h));
            lq.color = glyph.color;
            lq.atlasPage = mask->page;
            lq.seq = mask->seq;
            lq.glyphIndex = static_cast<std::uint32_t>(gi);
            lq.stretched = std::abs(scale - 1.f) > 0.005f;
            lq.turn = glyph.turn != 0;
            cached.quads.push_back(lq);
        }
    }
    if (outAllExact) *outAllExact = cached.allExactMask;
    if (cached.quads.empty()) return;

    const float sx = static_cast<float>(sprite.x);
    const float sy = static_cast<float>(sprite.y);
    const int numPages = static_cast<int>(glyphPages().size());
    const bool clipPerGlyph =
        (viewW > 0 && viewH > 0) &&
        (sprite.x < 0 || sprite.y < 0 || sprite.x + sprite.w > viewW || sprite.y + sprite.h > viewH);
    bool addedAny = false;
    for (const LocalGlyphQuad& lq : cached.quads) {
        if (lq.atlasPage < 0 || lq.atlasPage >= numPages) continue;
        const float x = sx + lq.relX;
        const float y = sy + lq.relY;
        if (clipPerGlyph) {
            const float dw = lq.tex.width() * lq.scale;
            const float dh = lq.tex.height() * lq.scale;
            if (x + dw < 0.f || y + dh < 0.f || x > static_cast<float>(viewW) || y > static_cast<float>(viewH)) {
                continue;
            }
        }
        AtlasPage& atlasPage = glyphPages()[static_cast<std::size_t>(lq.atlasPage)];
        if (!atlasPage.uploaded || lq.seq > atlasPage.uploadedSeq) {
            atlasPage.needUpload = true;
        }
        const std::uint32_t drawColor =
            (activeHold && lq.glyphIndex < glyphs.size() && glyphInSelection(frameGlyphSet, glyphs[lq.glyphIndex]))
                ? invertColor(lq.color)
                : lq.color;
        GlyphQuad quad;
        quad.scale = lq.scale;
        quad.x = x;
        quad.y = y;
        quad.tex = lq.tex;
        quad.color = drawColor;
        quad.turn = lq.turn;
        quad.penX = sx + lq.penRelX;
        quad.penY = sy + lq.penRelY;
        std::vector<std::vector<GlyphQuad>>& bucket = lq.stretched ? scaled : sharp;
        if (static_cast<int>(bucket.size()) <= lq.atlasPage) bucket.resize(static_cast<std::size_t>(numPages));
        bucket[static_cast<std::size_t>(lq.atlasPage)].push_back(quad);
        addedAny = true;
    }
    if (addedAny) {
        const GlyphRect spriteRect = GlyphRect::xywh(sx, sy, static_cast<float>(sprite.w), static_cast<float>(sprite.h));
        if (!hasPendingGlyphs) {
            batchBounds = spriteRect;
            hasPendingGlyphs = true;
        } else {
            batchBounds.join(spriteRect);
        }
    }
}

// 쪽 텍스처를 쓸지·구울지·버릴지는 pageTextures() 가 정한다. 버리면 GPU 쪽 이미지도 지운다.
// 쪽 텍스처는 화면 스레드만 만진다. 예산 관리자도 화면 스레드에서 부른다.
PageTextureCache& pageTextures() {
    static PageTextureCache cache([](int page) { dropPageImage(page); });
    return cache;
}

void clearPageGpuCache() {
    pageTextures().clear();
    clearPageImages();
}

bool pageHasActiveHold(int page) {
    if (page < 0) return false;
    const PointerHold& hold = pointerHold();
    for (const HeldBox& box : hold.boxes) {
        if (box.page == page && box.invert && box.width > 0 && box.height > 0) return true;
    }
    if (caretBlinkPhase() == 0) {
        for (const HeldBox& box : hold.compUnderlines) {
            if (box.page == page && box.invert && box.width > 0 && box.height > 0) return true;
        }
    }
    return false;
}

std::atomic<bool> gRenderInteracting{false};

int rasterQueuedMasks(int budget, int targetMaskZoom) {
    if (budget < 1) budget = 1;
    int made = 0;
    while (made < budget) {
        if (gRenderInteracting.load(std::memory_order_relaxed)) break;
        MaskJob job;
        bool hasJob = false;
        {
            std::lock_guard lock(glyphAtlasMutex());
            std::vector<MaskJob>& jobs = maskJobs();
            const std::size_t baseCount = std::min(baseMaskJobCount(), jobs.size());
            if (targetMaskZoom > 0 && targetMaskZoom != 100 && baseCount > 0) {
                if (maskZoomJobTarget() != targetMaskZoom) {
                    maskZoomJobTarget() = targetMaskZoom;
                    maskZoomJobCursor() = 0;
                }
                std::size_t& zCursor = maskZoomJobCursor();
                if (zCursor < baseCount) {
                    const MaskJob& base = jobs[zCursor++];
                    const float exact = base.exactPpem > 0.f ? base.exactPpem : static_cast<float>(base.ppem);
                    const int scaledPpem = glyphMaskPpem(static_cast<int>(
                        std::lround(static_cast<double>(exact) * static_cast<double>(targetMaskZoom) / 100.0)));
                    job = MaskJob{base.font, base.cp, base.bold, base.italic, base.ratio,
                                  scaledPpem, targetMaskZoom, exact};
                    hasJob = true;
                }
            }
            if (!hasJob) {
                std::size_t& cursor = maskJobCursor();
                if (cursor < jobs.size()) {
                    job = jobs[cursor++];
                    hasJob = true;
                }
            }
        }
        if (!hasJob) break;
        GlyphMem* mem = glyphMemory(job.font, job.cp);
        if (!mem || !mem->face || job.ppem <= 0) continue;
        FrameGlyph glyph;
        glyph.mem = mem;
        glyph.ppem = job.ppem;
        glyph.ratio = job.ratio > 0 ? job.ratio : 100;
        glyph.bold = job.bold;
        glyph.italic = job.italic;
        const MaskId key = maskId(glyph, job.ppem);
        {
            std::lock_guard lock(glyphAtlasMutex());
            if (glyphMasks().count(key)) continue;
        }
        MaskLoc loc;
        loc.zoom = job.zoom > 0 ? job.zoom : 100;
        loc.ppem = job.ppem;
        const bool drawn = rasterGlyphAlpha(glyph, job.ppem, loc);
        {
            std::lock_guard lock(glyphAtlasMutex());
            if (glyphMasks().count(key)) continue;
            if (!drawn || !packMask(loc)) {
                loc.missing = true;
                loc.alpha.clear();
                loc.page = -1;
            }
            rememberFamily(maskFamily(glyph), job.ppem);
            glyphMasks().emplace(key, std::move(loc));
            ++maskBuiltSeq();
        }
        ++made;
    }
    return made;
}

}  // namespace

namespace {
std::atomic<int> gScreenGlyphEpoch{0};
}  // namespace

int screenGlyphEpoch() { return gScreenGlyphEpoch.load(std::memory_order_relaxed); }

// 선언은 앞쪽 paint_detail 에 있다. 이름 공간이 같아야 같은 함수다.
void paint_detail::clearScreenGlyphs() {
    // 칸마다 모아 둔 글자를 지운다. 세션이 들고 있는 칸은 이 값이 바뀐 것을 보고 다시 굽는다.
    gScreenGlyphEpoch.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(glyphAtlasMutex());
        ++maskCacheGen();
        ++maskBuiltSeq();
        dotMaskGen() = 0;
        glyphMasks().clear();
        glyphFamilies().clear();
        glyphPages().clear();
        glyphMaskPending() = false;
        maskJobCursor() = 0;
        maskZoomJobCursor() = 0;
    }
    clearAtlasTextures();
    clearFrameGlyphs();
    clearPageGpuCache();
}

void dropScreenSprite(int id) {
    dropScreenTexture(id);
    dropFrameGlyphs(id);
    dropFramePictures(id);
    dropFrameBorders(id);
}

void dropPageGpuCache(int page) {
    if (page < 0) clearPageGpuCache();
    else pageTextures().drop(page);
}

bool glyphMasksPending() {
    std::lock_guard lock(glyphAtlasMutex());
    return glyphMaskPending();
}

// 화면이 칸 비트맵 말고 따로 그릴 것(글자, 그림)이 있는지. 있으면 빈 비트맵 칸도 쓴다.
int framesHoldingGlyphs() {
    std::lock_guard lock(frameGlyphMutex());
    int count = 0;
    for (const auto& [id, held] : frameGlyphs()) {
        if (!held.glyphs.empty()) ++count;
    }
    return count;
}

bool frameHasGlyphs(int id) {
    {
        std::lock_guard lock(frameGlyphMutex());
        const auto found = frameGlyphs().find(id);
        if (found != frameGlyphs().end() && !found->second.glyphs.empty()) return true;
    }
    return frameHasPictures(id) || frameHasBorders(id);
}

std::vector<FrameGlyphSpot> frameGlyphSpots(int id) {
    std::vector<FrameGlyphSpot> out;
    std::lock_guard lock(frameGlyphMutex());
    const auto found = frameGlyphs().find(id);
    if (found == frameGlyphs().end()) return out;
    out.reserve(found->second.glyphs.size());
    for (const FrameGlyph& glyph : found->second.glyphs) out.push_back(FrameGlyphSpot{glyph.story, glyph.paragraph, glyph.offset});
    return out;
}

bool cloneAndAdjustFrameGlyphs(int srcFrameId, int dstFrameId, int newStory, int newParagraph, int deltaOffset,
                               std::int32_t deltaOriginX, std::int32_t deltaOriginY) {
    (void)deltaOriginX;
    (void)deltaOriginY;
    if (srcFrameId == 0 || dstFrameId == 0) return false;
    const bool pictures = cloneFramePictures(srcFrameId, dstFrameId);
    const bool borders = cloneFrameBorders(srcFrameId, dstFrameId);
    std::lock_guard lock(frameGlyphMutex());
    const auto found = frameGlyphs().find(srcFrameId);
    if (found == frameGlyphs().end() || found->second.glyphs.empty()) return pictures || borders;
    if (srcFrameId != dstFrameId) {
        FrameGlyphs copy = found->second;
        for (FrameGlyph& g : copy.glyphs) {
            if (newStory >= 0) g.story = newStory;
            if (newParagraph >= 0) g.paragraph = newParagraph;
            if (deltaOffset != 0 && g.offset >= 0) g.offset += deltaOffset;
        }
        frameGlyphs()[dstFrameId] = std::move(copy);
        if (const auto srIt = spriteMaskReady().find(srcFrameId); srIt != spriteMaskReady().end()) {
            spriteMaskReady()[dstFrameId] = srIt->second;
        }
        cachedSpriteQuads().erase(dstFrameId);
    } else {
        for (FrameGlyph& g : found->second.glyphs) {
            if (newStory >= 0) g.story = newStory;
            if (newParagraph >= 0) g.paragraph = newParagraph;
            if (deltaOffset != 0 && g.offset >= 0) g.offset += deltaOffset;
        }
    }
    return true;
}

bool canUsePageGpuTexture(int page, int pageW, int pageH, int layoutZoom, int maskZoom, int pageStamp, bool dragging) {
    if (!pageTexturesOn()) return false;
    PageTextureWant want;
    want.w = pageW;
    want.h = pageH;
    want.layoutZoom = layoutZoom;
    want.maskZoom = maskZoom;
    want.stamp = pageStamp;
    want.interacting = dragging || gRenderInteracting.load(std::memory_order_relaxed);
    want.maskSeq = maskBuiltSeq();
    want.pictureSeq = pictureTextureSeq();
    return pageTextures().usable(page, want);
}

bool hasScalablePageGpuTexture(int page, int pageStamp) {
    return pageTexturesOn() && pageTextures().scalable(page, pageStamp);
}

int pageGpuTextureZoom(int page, int pageStamp) {
    return pageTexturesOn() ? pageTextures().zoomOf(page, pageStamp) : 0;
}

bool hasAnyPageGpuTexture(int page) { return pageTexturesOn() && pageTextures().has(page); }

bool canBakePageGpuTexture(int page, int pageW, int pageH, int pageStamp) {
    return pageTexturesOn() && PageTextureCache::bakeable(page, pageW, pageH, pageStamp);
}

void setRenderInteracting(bool interacting) {
    gRenderInteracting.store(interacting, std::memory_order_relaxed);
}

void setScreenVectors(bool on) { enableScreenVectors(on); }

void setScreenOpSink(ScreenOpSink sink) { screenOpSink() = std::move(sink); }

MaskBake bakeDocumentMasks(const model::Document& doc, int budget, int targetMaskZoom) {
    noteDocumentGlyphs(doc);
    // 마스크는 화면이 글자를 직접 그릴 때만 굽는다.
    if (!screenVectors()) return {};
    MaskBake out;
    out.made = rasterQueuedMasks(budget, targetMaskZoom);
    {
        std::lock_guard lock(glyphAtlasMutex());
        const std::size_t baseCount = std::min(baseMaskJobCount(), maskJobs().size());
        const bool zoomMore =
            targetMaskZoom > 0 && targetMaskZoom != 100 && baseCount > 0 &&
            (maskZoomJobTarget() != targetMaskZoom || maskZoomJobCursor() < baseCount);
        out.more = zoomMore || (maskJobCursor() < maskJobs().size());
    }
    if (!out.more && out.made > 0) {
        std::fprintf(stderr, "[ClassingHWP] %d%% 마스크를 다 찍었다\n", targetMaskZoom > 0 ? targetMaskZoom : 100);
    }
    return out;
}
#if CHWP_HAS_GPU
namespace {

struct ScreenTexture {
    int stamp = -1;
    bool mipmapped = false;
    sk_sp<SkImage> image;
};

std::unordered_map<int, ScreenTexture>& screenTextures() {
    static std::unordered_map<int, ScreenTexture> textures;
    return textures;
}

// 2D 화면에서 셸에 올려 둔 칸 그림.
ScreenBitmaps& screenBitmaps() {
    static ScreenBitmaps bitmaps;
    return bitmaps;
}

// 이번 프레임에 칸 픽셀로 만든 래스터 그림이 어느 칸의 어느 판인지. 2D 그림 목록이 같은 칸을 같은 번호로 쓴다.
std::unordered_map<std::uint32_t, std::pair<int, int>>& rasterImageSources() {
    static std::unordered_map<std::uint32_t, std::pair<int, int>> sources;
    return sources;
}

void dropScreenTexture(int id) {
    screenTextures().erase(id);
    screenBitmaps().forget(static_cast<std::uint32_t>(id));
}

// 글자 아틀라스 장마다 올린 텍스처. 장 번호가 같다.
std::vector<sk_sp<SkImage>>& atlasTextures() {
    static std::vector<sk_sp<SkImage>> textures;
    return textures;
}

void clearAtlasTextures() { atlasTextures().clear(); }

sk_sp<SkRuntimeEffect> sdfGlyphEffect() {
    static sk_sp<SkRuntimeEffect> effect = []() -> sk_sp<SkRuntimeEffect> {
        static constexpr char kSkSL[] =
            "uniform shader atlas;"
            "uniform float lo;"
            "uniform float hi;"
            "half4 main(float2 uv) {"
            "    half a = atlas.eval(uv).a;"
            "    half cov = smoothstep(half(lo), half(hi), a);"
            "    return half4(cov);"
            "}";
        return SkRuntimeEffect::MakeForShader(SkString(kSkSL)).effect;
    }();
    return effect;
}

sk_sp<SkRuntimeEffect> shrinkGlyphEffect() {
    static sk_sp<SkRuntimeEffect> effect = []() -> sk_sp<SkRuntimeEffect> {
        static constexpr char kSkSL[] =
            "uniform shader atlas;"
            "uniform float reach;"
            "half4 main(float2 uv) {"
            "    half a = atlas.eval(uv + float2(-reach, -reach)).a + atlas.eval(uv + float2(reach, -reach)).a +"
            "             atlas.eval(uv + float2(-reach, reach)).a + atlas.eval(uv + float2(reach, reach)).a;"
            "    return half4(a * 0.25);"
            "}";
        return SkRuntimeEffect::MakeForShader(SkString(kSkSL)).effect;
    }();
    return effect;
}

void drawGlyphQuads(SkCanvas* canvas, skgpu::graphite::Recorder* recorder,
                    const std::vector<std::vector<GlyphQuad>>& pages, SkFilterMode filter) {
    constexpr std::size_t kMaxQuadsPerBatch = 16380;
    for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
        const std::vector<GlyphQuad>& quads = pages[static_cast<std::size_t>(i)];
        if (quads.empty() || i >= static_cast<int>(glyphPages().size())) continue;
        AtlasPage& atlasPage = glyphPages()[static_cast<std::size_t>(i)];
        std::vector<sk_sp<SkImage>>& textures = atlasTextures();
        if (textures.size() <= static_cast<std::size_t>(i)) textures.resize(static_cast<std::size_t>(i) + 1);
        sk_sp<SkImage>& atlasImage = textures[static_cast<std::size_t>(i)];
        if ((atlasPage.needUpload || !atlasPage.uploaded) && !atlasPage.alpha.empty()) {
            const SkImageInfo info = SkImageInfo::Make(kAtlasDim, kAtlasDim, kAlpha_8_SkColorType, kPremul_SkAlphaType);
            const SkPixmap pixmap(info, atlasPage.alpha.data(), kAtlasDim);
            if (const sk_sp<SkImage> raster = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr)) {
                if (recorder) {
                    if (sk_sp<SkImage> gpuImg = SkImages::TextureFromImage(recorder, raster.get())) {
                        atlasImage = std::move(gpuImg);
                        atlasPage.uploaded = true;
                        atlasPage.uploadedSeq = atlasPage.seq;
                        atlasPage.needUpload = false;
                    } else {
                        std::fprintf(stderr, "[ClassingHWP] 글자 아틀라스 업로드 실패\n");
                    }
                } else {
                    atlasImage = raster;
                    atlasPage.uploaded = true;
                    atlasPage.uploadedSeq = atlasPage.seq;
                    atlasPage.needUpload = false;
                }
            }
        }
        if (!atlasPage.uploaded || !atlasImage) continue;

        const sk_sp<SkShader> atlasShader =
            atlasImage->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, SkSamplingOptions(filter));
        if (!atlasShader) continue;
        auto drawGroup = [&](const std::vector<GlyphQuad>& group, sk_sp<SkShader> shader) {
            if (group.empty() || !shader) return;
            SkPaint paint;
            paint.setShader(std::move(shader));
            for (std::size_t start = 0; start < group.size(); start += kMaxQuadsPerBatch) {
                const std::size_t count = std::min(kMaxQuadsPerBatch, group.size() - start);
                SkVertices::Builder builder(
                    SkVertices::kTriangles_VertexMode,
                    static_cast<int>(count * 4),
                    static_cast<int>(count * 6),
                    SkVertices::kHasTexCoords_BuilderFlag | SkVertices::kHasColors_BuilderFlag);
                if (!builder.isValid()) break;
                SkPoint* pos = builder.positions();
                SkPoint* tex = builder.texCoords();
                SkColor* col = builder.colors();
                std::uint16_t* idx = builder.indices();
                for (std::size_t q = 0; q < count; ++q) {
                    const GlyphQuad& quad = group[start + q];
                    const float x0 = quad.x;
                    const float y0 = quad.y;
                    const float s = quad.scale;
                    const float x1 = x0 + quad.tex.width() * s;
                    const float y1 = y0 + quad.tex.height() * s;
                    const std::uint16_t v = static_cast<std::uint16_t>(q * 4);
                    pos[v + 0] = {x0, y0};
                    pos[v + 1] = {x1, y0};
                    pos[v + 2] = {x1, y1};
                    pos[v + 3] = {x0, y1};
                    if (quad.turn) {
                        // 세로쓰기에서 눕힌 글자. 펜에서 (dx, dy) 떨어진 점은 (−dy, dx) 로 간다.
                        for (int k = 0; k < 4; ++k) {
                            const SkPoint at = pos[v + k];
                            pos[v + k] = {quad.penX - (at.fY - quad.penY), quad.penY + (at.fX - quad.penX)};
                        }
                    }
                    tex[v + 0] = {quad.tex.l, quad.tex.t};
                    tex[v + 1] = {quad.tex.r, quad.tex.t};
                    tex[v + 2] = {quad.tex.r, quad.tex.b};
                    tex[v + 3] = {quad.tex.l, quad.tex.b};
                    col[v + 0] = quad.color;
                    col[v + 1] = quad.color;
                    col[v + 2] = quad.color;
                    col[v + 3] = quad.color;
                    const std::size_t iBase = q * 6;
                    idx[iBase + 0] = v + 0;
                    idx[iBase + 1] = v + 1;
                    idx[iBase + 2] = v + 2;
                    idx[iBase + 3] = v + 0;
                    idx[iBase + 4] = v + 2;
                    idx[iBase + 5] = v + 3;
                }
                canvas->drawVertices(builder.detach().get(), SkBlendMode::kDstIn, paint);
            }
        };
        if (filter != SkFilterMode::kLinear) {
            drawGroup(quads, atlasShader);
            continue;
        }
        // 늘린 글자는 가장자리를 세우고, 많이 줄인 글자는 네 점을 고르게 읽어 획이 빠지지 않게 한다.
        // 조금 줄인 것은 한 점 쌍선형으로도 획이 남는다. 네 점을 읽으면 오히려 흐려진다.
        constexpr float kShrinkBlend = 0.8f;
        const bool allEven = std::all_of(quads.begin(), quads.end(), [](const GlyphQuad& quad) {
            return quad.scale >= kShrinkBlend && quad.scale <= 1.05f;
        });
        if (allEven) {
            drawGroup(quads, atlasShader);
            continue;
        }
        std::vector<GlyphQuad> grown;
        std::vector<GlyphQuad> shrunk;
        std::vector<GlyphQuad> even;
        float maxScale = 1.f;
        float minScale = 1.f;
        for (const GlyphQuad& quad : quads) {
            const float s = quad.scale;
            if (s > 1.05f) {
                grown.push_back(quad);
                maxScale = std::max(maxScale, s);
            } else if (s < kShrinkBlend) {
                shrunk.push_back(quad);
                minScale = std::min(minScale, s);
            } else {
                even.push_back(quad);
            }
        }
        drawGroup(even, atlasShader);
        if (!grown.empty()) {
            sk_sp<SkShader> shader = atlasShader;
            if (const sk_sp<SkRuntimeEffect> effect = sdfGlyphEffect()) {
                const float s = std::clamp(maxScale, 1.05f, 8.0f);
                const float t = (s - 1.0f) / s;
                constexpr float kEdgeCenter = 0.44f;
                struct SdfUniforms {
                    float lo;
                    float hi;
                } uniforms{
                    kEdgeCenter * t,
                    1.0f - (1.0f - kEdgeCenter) * t,
                };
                sk_sp<SkShader> children[1] = {atlasShader};
                if (sk_sp<SkShader> sdfShader =
                        effect->makeShader(SkData::MakeWithCopy(&uniforms, sizeof(uniforms)), children, 1)) {
                    shader = std::move(sdfShader);
                }
            }
            drawGroup(grown, std::move(shader));
        }
        if (!shrunk.empty()) {
            sk_sp<SkShader> shader = atlasShader;
            if (const sk_sp<SkRuntimeEffect> effect = shrinkGlyphEffect()) {
                // 화면 한 픽셀이 덮는 마스크 폭의 4분의 1만큼 비켜 네 번 읽는다. 마스크 둘레 여백(2px) 안이다.
                const float reach = std::clamp(0.25f / std::max(minScale, 0.25f), 0.25f, 1.0f);
                sk_sp<SkShader> children[1] = {atlasShader};
                if (sk_sp<SkShader> shrinkShader =
                        effect->makeShader(SkData::MakeWithCopy(&reach, sizeof(reach)), children, 1)) {
                    shader = std::move(shrinkShader);
                }
            }
            drawGroup(shrunk, std::move(shader));
        }
    }
}

// 쪽 텍스처 이미지. 쓸지·구울지·버릴지는 pageTextures() 가 정하고, 버리면 여기서도 지운다.
// 쪽 텍스처는 화면 스레드만 만진다. 예산 관리자도 화면 스레드에서 부른다.
std::unordered_map<int, sk_sp<SkImage>>& pageGpuImages() {
    static std::unordered_map<int, sk_sp<SkImage>> images;
    return images;
}

SkImage* pageGpuImage(int page) {
    const auto found = pageGpuImages().find(page);
    return found == pageGpuImages().end() ? nullptr : found->second.get();
}

void dropPageImage(int page) { pageGpuImages().erase(page); }

void clearPageImages() { pageGpuImages().clear(); }

void storePageGpuCache(int page, int w, int h, int layoutZoom, int maskZoom, int stamp, bool incomplete,
                       sk_sp<SkImage> image) {
    if (!image) return;
    PageTextureMeta meta;
    meta.page = page;
    meta.w = w;
    meta.h = h;
    meta.layoutZoom = layoutZoom;
    meta.maskZoom = maskZoom;
    meta.stamp = stamp;
    meta.incomplete = incomplete;
    meta.builtSeq = maskBuiltSeq();
    meta.pictureSeq = pictureTextureSeq();
    if (pageTextures().store(meta)) pageGpuImages()[page] = std::move(image);
}

bool pageTexturesOn() {
    const GpuSlot& slot = gpuSlot();
    return slot.ready && !slot.raster && slot.recorder;
}

sk_sp<SkImage> screenTexture(skgpu::graphite::Recorder* recorder, const ScreenSprite& sprite, bool needMipmap,
                             bool dragging, bool& outMipmapped, int& mipmapUpgrades) {
    outMipmapped = false;
    // 1x1 더미 투명 비트맵은 텍스처를 만들거나 글리프 배칭을 끊지 않는다.
    if (!sprite.rgba || sprite.sw <= 1 || sprite.sh <= 1 || sprite.id == 0) return nullptr;
    if (!recorder) {
        const SkImageInfo info = SkImageInfo::Make(sprite.sw, sprite.sh, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        const SkPixmap pixmap(info, sprite.rgba, static_cast<std::size_t>(sprite.sw) * 4u);
        sk_sp<SkImage> image = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
        if (image) rasterImageSources()[image->uniqueID()] = {sprite.id, sprite.stamp};
        return image;
    }
    ScreenTexture& slot = screenTextures()[sprite.id];
    if (slot.image && slot.stamp == sprite.stamp) {
        if (!needMipmap || slot.mipmapped) {
            outMipmapped = slot.mipmapped;
            return slot.image;
        }
        if (dragging || mipmapUpgrades >= 6) {
            outMipmapped = slot.mipmapped;
            return slot.image;
        }
        ++mipmapUpgrades;
    }
    const SkImageInfo info = SkImageInfo::Make(sprite.sw, sprite.sh, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    const SkPixmap pixmap(info, sprite.rgba, static_cast<std::size_t>(sprite.sw) * 4u);
    const sk_sp<SkImage> raster = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
    if (!raster) return nullptr;
    bool uploadMipmap = needMipmap || sprite.percent == 100;
    sk_sp<SkImage> uploaded = SkImages::TextureFromImage(recorder, raster.get(), {uploadMipmap});
    if (!uploaded && uploadMipmap) {
        uploaded = SkImages::TextureFromImage(recorder, raster.get());
        uploadMipmap = false;
    }
    if (!uploaded) return nullptr;
    slot.image = std::move(uploaded);
    slot.stamp = sprite.stamp;
    slot.mipmapped = uploadMipmap;
    outMipmapped = uploadMipmap;
    return slot.image;
}

bool configureDawnSurface(GpuSlot& slot, int viewW, int viewH) {
    if (slot.surfaceW == viewW && slot.surfaceH == viewH) return true;
    wgpu::SurfaceConfiguration config;
    config.device = slot.device;
    config.format = slot.format;
    config.width = static_cast<std::uint32_t>(viewW);
    config.height = static_cast<std::uint32_t>(viewH);
    slot.surface.Configure(&config);
    slot.surfaceW = viewW;
    slot.surfaceH = viewH;
    return true;
}

const ViewPage* sheetOf(const std::vector<ViewPage>& pages, int index) {
    for (const ViewPage& item : pages) {
        if (item.index == index) return &item;
    }
    return nullptr;
}

void appendMergedRect(std::vector<SkRect>& rects, const SkRect& rect) {
    if (rect.isEmpty()) return;
    if (!rects.empty()) {
        SkRect& prev = rects.back();
        if (std::abs(prev.fLeft - rect.fLeft) <= 0.5f && std::abs(prev.fRight - rect.fRight) <= 0.5f &&
            rect.fTop <= prev.fBottom + 0.5f && rect.fBottom >= prev.fTop - 0.5f) {
            prev.fTop = std::min(prev.fTop, rect.fTop);
            prev.fBottom = std::max(prev.fBottom, rect.fBottom);
            return;
        }
        if (std::abs(prev.fTop - rect.fTop) <= 0.5f && std::abs(prev.fBottom - rect.fBottom) <= 0.5f &&
            rect.fLeft <= prev.fRight + 0.5f && rect.fRight >= prev.fLeft - 0.5f) {
            prev.fLeft = std::min(prev.fLeft, rect.fLeft);
            prev.fRight = std::max(prev.fRight, rect.fRight);
            return;
        }
    }
    rects.push_back(rect);
}

std::vector<SkRect> placeInvertBacks(SkCanvas* canvas, const std::vector<ViewPage>& pages, int scrollX, int scrollY,
                                     int viewW, int viewH, const std::unordered_set<int>& handledPages) {
    std::vector<SkRect> backs;
    const PointerHold& hold = pointerHold();
    if ((hold.boxes.empty() && hold.compUnderlines.empty()) || !canvas) return backs;
    SkPaint paint;
    paint.setAntiAlias(false);
    const SkRect screenBounds = SkRect::MakeWH(static_cast<float>(viewW), static_cast<float>(viewH));
    const auto drawBoxes = [&](const std::vector<HeldBox>& list) {
        SkColor lastColor = 0;
        bool hasColor = false;
        std::vector<SkRect> batch;
        const auto flushBatch = [&]() {
            if (batch.empty()) return;
            paint.setColor(lastColor);
            for (const SkRect& r : batch) {
                backs.push_back(r);
                canvas->drawRect(r, paint);
            }
            batch.clear();
        };
        for (const HeldBox& box : list) {
            if (!box.invert || box.pageW <= 0 || box.pageH <= 0 || box.width <= 0 || box.height <= 0) continue;
            if (handledPages.count(box.page)) continue;
            const ViewPage* page = sheetOf(pages, box.page);
            if (!page || page->w <= 0 || page->h <= 0) continue;
            const SkColor color = invertColor(SkColorSetARGB(255, box.backR, box.backG, box.backB));
            if (!hasColor || color != lastColor) {
                flushBatch();
                lastColor = color;
                hasColor = true;
            }
            const float sx = static_cast<float>(page->w) / static_cast<float>(box.pageW);
            const float sy = static_cast<float>(page->h) / static_cast<float>(box.pageH);
            const float x = static_cast<float>(page->x - scrollX) + static_cast<float>(box.x) * sx;
            const float y = static_cast<float>(page->y - scrollY) + static_cast<float>(box.y) * sy;
            const float w = std::max(1.f, static_cast<float>(box.width) * sx);
            const float h = std::max(1.f, static_cast<float>(box.height) * sy);
            SkRect rect = SkRect::MakeXYWH(x, y, w, h);
            if (!rect.intersect(screenBounds)) continue;
            appendMergedRect(batch, rect);
        }
        flushBatch();
    };
    if (hold.resizeHandle < 0) drawBoxes(hold.boxes);
    if (caretBlinkPhase() == 0) {
        drawBoxes(hold.compUnderlines);
    }
    return backs;
}

void drawPageHoldInversion(SkCanvas* canvas, const SkImage* pageImg, const ScreenSprite& quad,
                           const SkSamplingOptions& pageSampling, int viewW, int viewH,
                           sk_sp<SkColorFilter>& inverted) {
    if (!canvas || !pageImg || quad.page < 0 || quad.w <= 0 || quad.h <= 0) return;
    if (!pageHasActiveHold(quad.page)) return;
    const PointerHold& hold = pointerHold();
    if (!inverted) {
        const float matrix[20] = {
            -1.f, 0.f,  0.f,  1.f, 0.f,
            0.f,  -1.f, 0.f,  1.f, 0.f,
            0.f,  0.f,  -1.f, 1.f, 0.f,
            0.f,  0.f,  0.f,  1.f, 0.f,
        };
        inverted = SkColorFilters::Matrix(matrix);
    }
    SkPaint paint;
    paint.setAntiAlias(false);
    paint.setColorFilter(inverted);
    const SkRect dst = SkRect::MakeXYWH(static_cast<float>(quad.x), static_cast<float>(quad.y),
                                        static_cast<float>(quad.w), static_cast<float>(quad.h));
    const SkRect screenBounds = SkRect::MakeWH(static_cast<float>(viewW), static_cast<float>(viewH));
    SkRect visiblePageDst = dst;
    if (!visiblePageDst.intersect(screenBounds)) return;
    const float imgScaleX = static_cast<float>(pageImg->width()) / static_cast<float>(quad.w);
    const float imgScaleY = static_cast<float>(pageImg->height()) / static_cast<float>(quad.h);
    const bool exactSize = (pageImg->width() == quad.w && pageImg->height() == quad.h);
    const SkSamplingOptions sampling = exactSize ? SkSamplingOptions() : pageSampling;
    std::vector<SkRect> mergedRects;
    const auto collectBoxes = [&](const std::vector<HeldBox>& list) {
        for (const HeldBox& box : list) {
            if (box.page != quad.page || !box.invert || box.pageW <= 0 || box.pageH <= 0 || box.width <= 0 ||
                box.height <= 0) {
                continue;
            }
            const float sx = static_cast<float>(quad.w) / static_cast<float>(box.pageW);
            const float sy = static_cast<float>(quad.h) / static_cast<float>(box.pageH);
            const float x = static_cast<float>(quad.x) + static_cast<float>(box.x) * sx;
            const float y = static_cast<float>(quad.y) + static_cast<float>(box.y) * sy;
            const float w = std::max(1.f, static_cast<float>(box.width) * sx);
            const float h = std::max(1.f, static_cast<float>(box.height) * sy);
            SkRect rect = SkRect::MakeXYWH(x, y, w, h);
            if (!rect.intersect(visiblePageDst)) continue;
            appendMergedRect(mergedRects, rect);
        }
    };
    if (hold.resizeHandle < 0) collectBoxes(hold.boxes);
    if (caretBlinkPhase() == 0) {
        collectBoxes(hold.compUnderlines);
    }
    for (const SkRect& rect : mergedRects) {
        const SkRect srcRect = SkRect::MakeLTRB((rect.fLeft - dst.fLeft) * imgScaleX,
                                                (rect.fTop - dst.fTop) * imgScaleY,
                                                (rect.fRight - dst.fLeft) * imgScaleX,
                                                (rect.fBottom - dst.fTop) * imgScaleY);
        canvas->drawImageRect(pageImg, srcRect, rect, sampling, &paint, SkCanvas::kFast_SrcRectConstraint);
    }
}

void drawCaret(SkCanvas* canvas, skgpu::graphite::Recorder* recorder, const std::vector<ViewPage>& pages, int scrollX,
              int scrollY, bool onSelection, double dpr) {
    const PointerHold& hold = pointerHold();
    if (!canvas) return;
    // 머리말·꼬리말을 고치는 동안 띠 밖은 반투명 흰색으로 덮는다. 쪽 타일은 그대로 두고 덮개만 그린다.
    if (hold.bandStory > 0) {
        SkPaint veilPaint;
        veilPaint.setAntiAlias(false);
        veilPaint.setColor(SkColorSetARGB(150, 255, 255, 255));
        for (const ViewPage& page : pages) {
            const BandVeil veil = bandVeil(page.index);
            if (!veil.veiled || page.w <= 0 || page.h <= 0) continue;
            const float left = static_cast<float>(page.x - scrollX);
            const float top = static_cast<float>(page.y - scrollY);
            const float right = left + static_cast<float>(page.w);
            const float bottom = top + static_cast<float>(page.h);
            if (veil.width <= 0 || veil.height <= 0 || page.widthHu <= 0 || page.heightHu <= 0) {
                canvas->drawRect(SkRect::MakeLTRB(left, top, right, bottom), veilPaint);
                continue;
            }
            const float sx = static_cast<float>(page.w) / static_cast<float>(page.widthHu);
            const float sy = static_cast<float>(page.h) / static_cast<float>(page.heightHu);
            const float x0 = std::round(left + static_cast<float>(veil.x) * sx);
            const float y0 = std::round(top + static_cast<float>(veil.y) * sy);
            const float x1 = std::round(left + static_cast<float>(veil.x + veil.width) * sx);
            const float y1 = std::round(top + static_cast<float>(veil.y + veil.height) * sy);
            canvas->drawRect(SkRect::MakeLTRB(left, top, right, y0), veilPaint);
            canvas->drawRect(SkRect::MakeLTRB(left, y1, right, bottom), veilPaint);
            canvas->drawRect(SkRect::MakeLTRB(left, y0, x0, y1), veilPaint);
            canvas->drawRect(SkRect::MakeLTRB(x1, y0, right, y1), veilPaint);
        }
    }
    if (hold.pins) {
        // 마커는 배율과 상관없이 화면에서 같은 크기다. 블록 끝 세로선 아래에 손가락으로 잡는 동그라미를 단다.
        const float unit = static_cast<float>(dpr > 0 ? dpr : 1);
        auto drawPin = [&](const CaretPlace& pin) {
            if (!pin.hit || hold.caretPageW <= 0) return;
            const ViewPage* page = sheetOf(pages, pin.page);
            if (!page || page->w <= 0 || page->h <= 0) return;
            const float sx = static_cast<float>(page->w) / static_cast<float>(hold.caretPageW);
            const float sy = static_cast<float>(page->h) / static_cast<float>(std::max<std::int32_t>(hold.caretPageH, 1));
            const float x = static_cast<float>(page->x - scrollX) + static_cast<float>(pin.x) * sx;
            const float top = static_cast<float>(page->y - scrollY) + static_cast<float>(pin.y) * sy;
            const float bottom =
                static_cast<float>(page->y - scrollY) + static_cast<float>(pin.y + std::max<std::int32_t>(pin.height, 1)) * sy;
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(SkColorSetARGB(255, 26, 115, 232));
            paint.setStrokeWidth(2.f * unit);
            const float radius = 8.f * unit;
            if (pin.turned) {
                // 세로쓰기는 막대가 누워 있다. 동그라미는 막대 왼쪽 끝 밖에 단다.
                const float right = static_cast<float>(page->x - scrollX) +
                                    static_cast<float>(pin.x + std::max<std::int32_t>(pin.height, 1)) * sx;
                canvas->drawLine(x, top, right, top, paint);
                canvas->drawCircle(x - radius, top, radius, paint);
                return;
            }
            canvas->drawLine(x, top, x, bottom, paint);
            canvas->drawCircle(x, bottom + radius, radius, paint);
        };
        drawPin(hold.pinStart);
        drawPin(hold.pinEnd);
    }
    // 표 칸 경계선을 끄는 동안 새 선 자리를 점선으로 보인다.
    if (hold.borderDrag != 0 && hold.borderPage >= 0) {
        if (const ViewPage* page = sheetOf(pages, hold.borderPage);
            page && page->w > 0 && page->h > 0 && page->widthHu > 0 && page->heightHu > 0) {
            const float sx = static_cast<float>(page->w) / static_cast<float>(page->widthHu);
            const float sy = static_cast<float>(page->h) / static_cast<float>(page->heightHu);
            const float ox = static_cast<float>(page->x - scrollX);
            const float oy = static_cast<float>(page->y - scrollY);
            SkPaint pen;
            pen.setAntiAlias(true);
            pen.setStyle(SkPaint::kStroke_Style);
            pen.setStrokeWidth(1.f);
            pen.setColor(SK_ColorBLACK);
            const SkScalar intervals[2] = {4.f, 3.f};
            pen.setPathEffect(SkDashPathEffect::Make(intervals, 0));
            const std::int32_t at = hold.borderAt + hold.borderDelta;
            if (hold.borderDrag == 1) {
                const float lx = ox + static_cast<float>(at) * sx;
                canvas->drawLine(lx, oy + static_cast<float>(hold.borderY0) * sy, lx,
                                 oy + static_cast<float>(hold.borderY1) * sy, pen);
            } else {
                const float ly = oy + static_cast<float>(at) * sy;
                canvas->drawLine(ox + static_cast<float>(hold.borderX0) * sx, ly,
                                 ox + static_cast<float>(hold.borderX1) * sx, ly, pen);
            }
        }
    }
    // 개체를 옮기거나(그림이 아닌 개체는) 크기를 바꾸는 동안: 원래 자리는 반투명하게 덮어 흐리게 두고, 새 자리에
    // 누를 때 떼어 둔 그 개체 모습을 실제 모양 그대로 늘려 그린다.
    const bool reshaping = hold.resizeHandle >= 0 && hold.resizeBin.empty();
    if ((hold.moving || reshaping) && !hold.boxes.empty()) {
        const HeldBox& box = hold.boxes.front();
        if (const ViewPage* page = sheetOf(pages, box.page);
            page && page->w > 0 && page->h > 0 && page->widthHu > 0 && page->heightHu > 0) {
            const float sx = static_cast<float>(page->w) / static_cast<float>(page->widthHu);
            const float sy = static_cast<float>(page->h) / static_cast<float>(page->heightHu);
            const float ox = static_cast<float>(page->x - scrollX);
            const float oy = static_cast<float>(page->y - scrollY);
            const auto rectOf = [&](std::int32_t l, std::int32_t t, std::int32_t r, std::int32_t b) {
                return SkRect::MakeLTRB(ox + static_cast<float>(l) * sx, oy + static_cast<float>(t) * sy,
                                        ox + static_cast<float>(r) * sx, oy + static_cast<float>(b) * sy);
            };
            const SkRect from = hold.moving ? rectOf(box.x, box.y, box.x + box.width, box.y + box.height)
                                            : rectOf(hold.resizeLeft, hold.resizeTop, hold.resizeRight, hold.resizeBottom);
            const SkRect to = hold.moving ? rectOf(box.x + hold.moveDX, box.y + hold.moveDY, box.x + box.width + hold.moveDX,
                                                   box.y + box.height + hold.moveDY)
                                          : rectOf(box.x, box.y, box.x + box.width, box.y + box.height);
            SkPaint veil;
            veil.setAntiAlias(false);
            veil.setColor(SkColorSetARGB(170, 255, 255, 255));
            canvas->drawRect(from, veil);
            // 글이 든 개체(글상자·표)는 크기를 바꿀 때 글을 늘려 보이지 않는다(한글: 글은 놓은 뒤 다시 나뉜다).
            // 떼어 둔 모습을 원래 크기 그대로 새 상자 왼쪽 위에 두고 새 상자 안만 보이며, 새 상자는 점선으로 둘러 보인다.
            bool holdsText = false;
            if (reshaping && hold.selectedObject) {
                const auto hasLine = [](const auto& self, const Node& node) -> bool {
                    if (node.kind == NodeKind::Line || node.kind == NodeKind::Cell) return true;
                    for (const Node& child : node.children) {
                        if (self(self, child)) return true;
                    }
                    return false;
                };
                holdsText = hold.selectedObject->role == "table" || hasLine(hasLine, *hold.selectedObject);
            }
            if (holdsText && hold.dragSnapshot && hold.dragSnapshot->width > 0) {
                SkRect src = SkRect::MakeWH(static_cast<float>(hold.dragSnapshot->width),
                                            static_cast<float>(hold.dragSnapshot->height));
                const SkRect natural = SkRect::MakeXYWH(to.left(), to.top(), from.width(), from.height());
                const PictureTextureView texture =
                    pictureTexture(recorder, hold.dragSnapshotId, hold.dragSnapshot, natural.width(), natural.height(), src);
                if (texture.image) {
                    src = SkRect::MakeLTRB(src.fLeft * texture.scaleX, src.fTop * texture.scaleY,
                                           src.fRight * texture.scaleX, src.fBottom * texture.scaleY);
                    canvas->save();
                    canvas->clipRect(to);
                    canvas->drawImageRect(texture.image.get(), src, natural,
                                          SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), nullptr,
                                          SkCanvas::kFast_SrcRectConstraint);
                    canvas->restore();
                }
                SkPaint outline;
                outline.setAntiAlias(true);
                outline.setStyle(SkPaint::kStroke_Style);
                outline.setStrokeWidth(1.f);
                outline.setColor(SK_ColorBLACK);
                const SkScalar dashes[2] = {4.f, 3.f};
                outline.setPathEffect(SkDashPathEffect::Make(dashes, 0));
                canvas->drawRect(to, outline);
            } else if (hold.dragSnapshot && hold.dragSnapshot->width > 0) {
                SkRect src = SkRect::MakeWH(static_cast<float>(hold.dragSnapshot->width),
                                            static_cast<float>(hold.dragSnapshot->height));
                const PictureTextureView texture =
                    pictureTexture(recorder, hold.dragSnapshotId, hold.dragSnapshot, to.width(), to.height(), src);
                if (texture.image) {
                    src = SkRect::MakeLTRB(src.fLeft * texture.scaleX, src.fTop * texture.scaleY,
                                           src.fRight * texture.scaleX, src.fBottom * texture.scaleY);
                    canvas->drawImageRect(texture.image.get(), src, to,
                                          SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear), nullptr,
                                          SkCanvas::kFast_SrcRectConstraint);
                }
            }
        }
    }
    if (hold.objectHandles && !hold.boxes.empty() && hold.caretPageW > 0) {
        const HeldBox& box = hold.boxes.front();
        const ViewPage* page = sheetOf(pages, box.page);
        if (page && page->w > 0 && page->h > 0 && hold.resizeHandle >= 0 && !hold.resizeBin.empty()) {
            const std::shared_ptr<const CachedPicture> pic = peekPicture(hold.resizeBin);
            if (pic) {
                const float sx = static_cast<float>(page->w) / static_cast<float>(hold.caretPageW);
                const float sy =
                    static_cast<float>(page->h) / static_cast<float>(std::max<std::int32_t>(hold.caretPageH, 1));
                const auto screenX = [&](std::int32_t hu) {
                    return static_cast<float>(page->x - scrollX) + static_cast<float>(hu) * sx;
                };
                const auto screenY = [&](std::int32_t hu) {
                    return static_cast<float>(page->y - scrollY) + static_cast<float>(hu) * sy;
                };
                SkPaint cover;
                cover.setAntiAlias(false);
                // 원래 자리는 반투명하게 덮어 흐리게 남긴다.
                cover.setColor(SkColorSetARGB(170, 255, 255, 255));
                canvas->drawRect(SkRect::MakeLTRB(screenX(hold.resizeLeft), screenY(hold.resizeTop),
                                                  screenX(hold.resizeRight), screenY(hold.resizeBottom)),
                                 cover);
                const float nx0 = screenX(box.x);
                const float ny0 = screenY(box.y);
                const float nx1 = screenX(box.x + box.width);
                const float ny1 = screenY(box.y + box.height);
                const bool flipH = hold.selectedObject && hold.selectedObject->flipHorizontal;
                const bool flipV = hold.selectedObject && hold.selectedObject->flipVertical;
                SkRect src = SkRect::MakeWH(static_cast<float>(pic->width), static_cast<float>(pic->height));
                PictureSource cropped;
                if (hold.resizeCrop &&
                    cropSource(PictureCrop{hold.resizeOriginW, hold.resizeOriginH, hold.resizeClipL, hold.resizeClipT,
                                           hold.resizeClipR, hold.resizeClipB},
                               pic->width, pic->height, cropped)) {
                    src = SkRect::MakeLTRB(static_cast<float>(cropped.srcL), static_cast<float>(cropped.srcT),
                                           static_cast<float>(cropped.srcR), static_cast<float>(cropped.srcB));
                }
                const PictureTextureView texture =
                    pictureTexture(recorder, hold.resizeBin, pic, std::abs(nx1 - nx0), std::abs(ny1 - ny0), src);
                src = SkRect::MakeLTRB(src.fLeft * texture.scaleX, src.fTop * texture.scaleY,
                                       src.fRight * texture.scaleX, src.fBottom * texture.scaleY);
                const SkSamplingOptions sampling(SkFilterMode::kLinear, SkMipmapMode::kLinear);
                canvas->save();
                if (flipH || flipV) {
                    canvas->translate(flipH ? nx1 : nx0, flipV ? ny1 : ny0);
                    canvas->scale(flipH ? -1.f : 1.f, flipV ? -1.f : 1.f);
                    canvas->drawImageRect(texture.image.get(), src,
                                          SkRect::MakeWH(std::abs(nx1 - nx0), std::abs(ny1 - ny0)), sampling, nullptr,
                                          SkCanvas::kFast_SrcRectConstraint);
                } else {
                    canvas->drawImageRect(texture.image.get(), src, SkRect::MakeLTRB(nx0, ny0, nx1, ny1),
                                          sampling, nullptr, SkCanvas::kFast_SrcRectConstraint);
                }
                canvas->restore();
            }
        }
        if (page && page->w > 0 && page->h > 0) {
            const float sx = static_cast<float>(page->w) / static_cast<float>(hold.caretPageW);
            const float sy = static_cast<float>(page->h) / static_cast<float>(std::max<std::int32_t>(hold.caretPageH, 1));
            const float x0 = static_cast<float>(page->x - scrollX) + static_cast<float>(box.x) * sx;
            const float y0 = static_cast<float>(page->y - scrollY) + static_cast<float>(box.y) * sy;
            const float x1 = x0 + static_cast<float>(box.width) * sx;
            const float y1 = y0 + static_cast<float>(box.height) * sy;
            const float xm = (x0 + x1) * 0.5f;
            const float ym = (y0 + y1) * 0.5f;
            const float pts[8][2] = {{x0, y0}, {xm, y0}, {x1, y0}, {x0, ym}, {x1, ym}, {x0, y1}, {xm, y1}, {x1, y1}};
            SkPaint paint;
            paint.setAntiAlias(true);
            paint.setColor(SK_ColorWHITE);
            SkPaint edge;
            edge.setAntiAlias(true);
            edge.setStyle(SkPaint::kStroke_Style);
            edge.setStrokeWidth(1.f);
            edge.setColor(SK_ColorBLACK);
            const float rad = std::max(3.5f, 4.f * sx);
            for (const auto& pt : pts) {
                canvas->drawCircle(pt[0], pt[1], rad, paint);
                canvas->drawCircle(pt[0], pt[1], rad, edge);
            }
        }
        // 여러 개체 고르기로 더한 개체에도 손잡이를 둔다. 끄는 동안은 끈 만큼 옮긴 자리에 테두리만 보인다.
        for (const PointerHold::PickedObject& picked : hold.multiPicked) {
            const ViewPage* other = sheetOf(pages, picked.page);
            if (!other || other->w <= 0 || other->h <= 0 || other->widthHu <= 0 || other->heightHu <= 0) continue;
            const float ox = static_cast<float>(other->w) / static_cast<float>(other->widthHu);
            const float oy = static_cast<float>(other->h) / static_cast<float>(other->heightHu);
            const std::int32_t dx = hold.moving ? hold.moveDX : 0;
            const std::int32_t dy = hold.moving ? hold.moveDY : 0;
            const float x0 = static_cast<float>(other->x - scrollX) + static_cast<float>(picked.x + dx) * ox;
            const float y0 = static_cast<float>(other->y - scrollY) + static_cast<float>(picked.y + dy) * oy;
            const float x1 = x0 + static_cast<float>(picked.width) * ox;
            const float y1 = y0 + static_cast<float>(picked.height) * oy;
            SkPaint outline;
            outline.setAntiAlias(true);
            outline.setStyle(SkPaint::kStroke_Style);
            outline.setStrokeWidth(1.f);
            outline.setColor(SkColorSetARGB(255, 60, 110, 220));
            canvas->drawRect(SkRect::MakeLTRB(x0, y0, x1, y1), outline);
            if (hold.moving) continue;
            const float xm = (x0 + x1) * 0.5f;
            const float ym = (y0 + y1) * 0.5f;
            const float corners[8][2] = {{x0, y0}, {xm, y0}, {x1, y0}, {x0, ym}, {x1, ym}, {x0, y1}, {xm, y1}, {x1, y1}};
            SkPaint fillDot;
            fillDot.setAntiAlias(true);
            fillDot.setColor(SK_ColorWHITE);
            SkPaint edgeDot;
            edgeDot.setAntiAlias(true);
            edgeDot.setStyle(SkPaint::kStroke_Style);
            edgeDot.setStrokeWidth(1.f);
            edgeDot.setColor(SK_ColorBLACK);
            const float rad = std::max(3.f, 3.5f * ox);
            for (const auto& pt : corners) {
                canvas->drawCircle(pt[0], pt[1], rad, fillDot);
                canvas->drawCircle(pt[0], pt[1], rad, edgeDot);
            }
        }
    }
    // 도형을 끄는 동안 새로 생길 모양 그대로 보인다. 넣을 때와 같이 검은 실선 33 HU 에 흰 채움이고, 선은 채움이 없다
    // (insertShapeNow).
    if (hold.drawing && hold.drawPage >= 0) {
        if (const ViewPage* page = sheetOf(pages, hold.drawPage);
            page && page->w > 0 && page->h > 0 && page->widthHu > 0 && page->heightHu > 0) {
            const float sx = static_cast<float>(page->w) / static_cast<float>(page->widthHu);
            const float sy = static_cast<float>(page->h) / static_cast<float>(page->heightHu);
            const float left = static_cast<float>(page->x - scrollX);
            const float top = static_cast<float>(page->y - scrollY);
            const float x0 = left + static_cast<float>(hold.drawX0) * sx;
            const float y0 = top + static_cast<float>(hold.drawY0) * sy;
            const float x1 = left + static_cast<float>(hold.drawX1) * sx;
            const float y1 = top + static_cast<float>(hold.drawY1) * sy;
            SkPaint pen;
            pen.setAntiAlias(true);
            pen.setStyle(SkPaint::kStroke_Style);
            pen.setStrokeWidth(std::max(1.f, 33.f * sx));
            pen.setColor(SK_ColorBLACK);
            SkPaint face;
            face.setAntiAlias(true);
            face.setColor(SK_ColorWHITE);
            const SkRect box = SkRect::MakeLTRB(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
            if (hold.drawTool == 7 || hold.drawTool == 8 || hold.drawTool == 9) {
                // 다각형·곡선·자유선: 지금까지의 점을 잇는다. 점마다 작은 네모를 둔다(자유선은 두지 않는다).
                SkPathBuilder trail;
                for (std::size_t i = 0; i < hold.drawPoints.size(); ++i) {
                    const float px = left + static_cast<float>(hold.drawPoints[i].x) * sx;
                    const float py = top + static_cast<float>(hold.drawPoints[i].y) * sy;
                    if (i == 0) trail.moveTo(px, py);
                    else trail.lineTo(px, py);
                }
                canvas->drawPath(trail.detach(), pen);
                if (hold.drawTool != 9) {
                    SkPaint dot;
                    dot.setColor(SK_ColorBLACK);
                    for (const model::ShapePoint& pt : hold.drawPoints) {
                        const float px = left + static_cast<float>(pt.x) * sx;
                        const float py = top + static_cast<float>(pt.y) * sy;
                        canvas->drawRect(SkRect::MakeXYWH(px - 2.5f, py - 2.5f, 5.f, 5.f), dot);
                    }
                }
            } else if (hold.drawTool == 6) {
                // 호: 중심이 끈 상자 왼쪽 위인 사분 호(insertShapeNow).
                const float w = box.width();
                const float h = box.height();
                const SkRect oval = SkRect::MakeLTRB(box.left() - w, box.top() - h, box.left() + w, box.top() + h);
                SkPathBuilder arc;
                arc.moveTo(box.left(), box.top());
                arc.arcTo(oval, 0.f, 90.f, false);
                arc.close();
                const SkPath wedge = arc.detach();
                canvas->drawPath(wedge, face);
                SkPathBuilder rim;
                rim.arcTo(oval, 0.f, 90.f, true);
                canvas->drawPath(rim.detach(), pen);
            } else if (hold.drawTool == 3) {
                canvas->drawLine(x0, y0, x1, y1, pen);
            } else if (hold.drawTool == 2) {
                canvas->drawOval(box, face);
                canvas->drawOval(box, pen);
            } else {
                canvas->drawRect(box, face);
                canvas->drawRect(box, pen);
            }
        }
    }
    // 커서가 든 글상자·표에서 선이 없는 테두리. 한글처럼 빨간 점선이고, 커서처럼 깜빡이지 않는다.
    if (hold.caret.hit && !hold.frameLines.empty() && hold.framePage == hold.caretPage && hold.caretPageW > 0 &&
        hold.caretPageH > 0) {
        if (const ViewPage* page = sheetOf(pages, hold.framePage); page && page->w > 0 && page->h > 0) {
            const float sx = static_cast<float>(page->w) / static_cast<float>(hold.caretPageW);
            const float sy = static_cast<float>(page->h) / static_cast<float>(hold.caretPageH);
            const float left = static_cast<float>(page->x - scrollX);
            const float top = static_cast<float>(page->y - scrollY);
            SkPaint dash;
            dash.setAntiAlias(false);
            dash.setStyle(SkPaint::kStroke_Style);
            dash.setStrokeWidth(1.f);
            dash.setColor(SkColorSetRGB(0xFF, 0x00, 0x00));
            const SkScalar intervals[2] = {2.f, 2.f};
            // 1픽셀 선은 픽셀 가운데에 두어야 번지지 않는다.
            const auto snap = [](float v) { return std::floor(v) + 0.5f; };
            for (const PointerHold::FrameLine& line : hold.frameLines) {
                const float x0 = snap(left + static_cast<float>(line.x0) * sx);
                const float y0 = snap(top + static_cast<float>(line.y0) * sy);
                const float x1 = snap(left + static_cast<float>(line.x1) * sx);
                const float y1 = snap(top + static_cast<float>(line.y1) * sy);
                // 점선 박자는 선 시작이 아니라 쪽 왼쪽 위에 맞춘다. 따로 그은 선이 이어지거나 겹쳐도 점이 같은 자리에 온다.
                const float run = line.y0 == line.y1 ? x0 - left : y0 - top;
                const float phase = std::fmod(std::fmod(run, 4.f) + 4.f, 4.f);
                dash.setPathEffect(SkDashPathEffect::Make(intervals, phase));
                canvas->drawLine(x0, y0, x1, y1, dash);
            }
        }
    }
    if (hold.composing && !hold.compUnderlines.empty()) return;
    if (!hold.caret.hit || hold.caretPageW <= 0 || hold.caretPageH <= 0 || hold.caret.height <= 0) return;
    if (caretBlinkPhase() != 0) return;
    const ViewPage* page = sheetOf(pages, hold.caretPage);
    if (!page || page->w <= 0 || page->h <= 0) return;
    const float sx = static_cast<float>(page->w) / static_cast<float>(hold.caretPageW);
    const float sy = static_cast<float>(page->h) / static_cast<float>(hold.caretPageH);
    const float x = static_cast<float>(page->x - scrollX) + static_cast<float>(hold.caret.x) * sx;
    const float y = static_cast<float>(page->y - scrollY) + static_cast<float>(hold.caret.y) * sy;
    SkPaint paint;
    paint.setAntiAlias(false);
    paint.setColor(onSelection ? SK_ColorWHITE : SK_ColorBLACK);
    if (hold.caret.turned) {
        // 세로쓰기 커서는 줄을 가로지르는 누운 막대다.
        const float w = std::max(1.f, static_cast<float>(hold.caret.height) * sx);
        canvas->drawRect(SkRect::MakeXYWH(x, y, w, std::max(1.f, sy)), paint);
        return;
    }
    const float h = std::max(1.f, static_cast<float>(hold.caret.height) * sy);
    canvas->drawRect(SkRect::MakeXYWH(x, y, std::max(1.f, sx), h), paint);
}

}  // namespace

bool screenReady() { return gpuSlot().ready; }

bool resetScreenSurface() {
    screenTextures().clear();
    clearScreenGlyphs();
    clearPageGpuCache();
    releaseGpuImages();
    if (gpuSlot().ready) return true;
    clearDawnSlot();
    return beginDawnSurface();
}

// 캔버스 없이 2D 로만 합성한다. 글자는 칸 비트맵에 굽는다(GPU 화면이 없을 때와 같다).
bool attachRemoteScreen() {
    screenTextures().clear();
    screenBitmaps().clear();
    clearScreenGlyphs();
    clearPageGpuCache();
    releaseGpuImages();
    clearDawnSlot();
    GpuSlot& slot = gpuSlot();
    slot.tried = true;
    slot.failed = true;
    slot.raster = true;
    slot.ready = true;
    slot.vectors = false;
    noteScreenFailed(true);
    return true;
}

// 개체·그림·칠은 글자 마스크가 아니라 칸 픽셀이다. 블록에 걸린 부분만 색을 뒤집는다.
// 블록마다 차이 클립을 화면에 쌓으면 클립이 마스크가 되고, 브라우저는 스왑체인을 텍스처로 읽다 실패한다.
// 보이는 블록만 칸 텍스처 안에서 뒤집고, 화면에는 그 결과를 한 장으로 올린다.
// 칸의 표 테두리를 셰이더로 그린다. 칸 비트맵(대각선)보다 먼저다. 선택 바탕과 겹친 곳은 색을 뒤집어 다시 그린다.
// 칸 비트맵 (0,0) 이 dst 왼쪽 위이고, 비트맵 sw×sh 가 dst 크기로 놓인다.
void drawSpriteBorders(SkCanvas* canvas, int id, const SkRect& dst, int sw, int sh, const std::vector<SkRect>& backs,
                       sk_sp<SkColorFilter>& inverted) {
    if (!canvas || sw <= 0 || sh <= 0 || dst.isEmpty()) return;
    const std::shared_ptr<const FrameBorders> borders = frameBorders(id);
    if (!borders || borders->mesh.chunks.empty()) return;
    const float sx = dst.width() / static_cast<float>(sw);
    const float sy = dst.height() / static_cast<float>(sh);
    const SkPoint origin = SkPoint::Make(dst.fLeft, dst.fTop);
    drawBorderMesh(canvas, borders->mesh, borders->scale, sx, sy, origin, dst);
    for (const SkRect& back : backs) {
        SkRect overlap = dst;
        if (!overlap.intersect(back)) continue;
        if (!inverted) {
            const float matrix[20] = {
                -1.f, 0.f,  0.f,  1.f, 0.f,
                0.f,  -1.f, 0.f,  1.f, 0.f,
                0.f,  0.f,  -1.f, 1.f, 0.f,
                0.f,  0.f,  0.f,  1.f, 0.f,
            };
            inverted = SkColorFilters::Matrix(matrix);
        }
        drawBorderMesh(canvas, borders->mesh, borders->scale, sx, sy, origin, overlap, inverted);
    }
}

void drawSpriteImage(SkCanvas* canvas, skgpu::graphite::Recorder* recorder, const SkImage* image, const SkRect& dst,
                     const SkSamplingOptions& sampling, const std::vector<SkRect>& backs, sk_sp<SkColorFilter>& inverted,
                     float viewW, float viewH) {
    (void)recorder;
    if (!canvas || !image || dst.isEmpty() || !(viewW > 0.f) || !(viewH > 0.f)) return;
    // 확대된 그림은 칸 전체가 화면보다 크다. 화면에 걸린 조각만 원본에서 집어 그린다.
    SkRect visible = dst;
    if (!visible.intersect(SkRect::MakeWH(viewW, viewH))) return;
    const float imgScaleX = static_cast<float>(image->width()) / dst.width();
    const float imgScaleY = static_cast<float>(image->height()) / dst.height();
    const SkRect visibleSrc = SkRect::MakeLTRB((visible.fLeft - dst.fLeft) * imgScaleX, (visible.fTop - dst.fTop) * imgScaleY,
                                               (visible.fRight - dst.fLeft) * imgScaleX,
                                               (visible.fBottom - dst.fTop) * imgScaleY);
    canvas->drawImageRect(image, visibleSrc, visible, sampling, nullptr, SkCanvas::kFast_SrcRectConstraint);
    if (backs.empty()) return;
    bool anyHit = false;
    for (const SkRect& back : backs) {
        if (SkRect::Intersects(visible, back)) {
            anyHit = true;
            break;
        }
    }
    if (!anyHit) return;
    if (!inverted) {
        const float matrix[20] = {
            -1.f, 0.f,  0.f,  1.f, 0.f,
            0.f,  -1.f, 0.f,  1.f, 0.f,
            0.f,  0.f,  -1.f, 1.f, 0.f,
            0.f,  0.f,  0.f,  1.f, 0.f,
        };
        inverted = SkColorFilters::Matrix(matrix);
    }
    SkPaint paint;
    paint.setAntiAlias(false);
    paint.setColorFilter(inverted);
    for (const SkRect& back : backs) {
        SkRect overlap = visible;
        if (!overlap.intersect(back)) continue;
        const SkRect srcRect = SkRect::MakeLTRB((overlap.fLeft - dst.fLeft) * imgScaleX,
                                                (overlap.fTop - dst.fTop) * imgScaleY,
                                                (overlap.fRight - dst.fLeft) * imgScaleX,
                                                (overlap.fBottom - dst.fTop) * imgScaleY);
        canvas->drawImageRect(image, srcRect, overlap, sampling, &paint, SkCanvas::kFast_SrcRectConstraint);
    }
}

void drawScrollbar(SkCanvas* canvas, const Scrollbar& bar) {
    if (!canvas || !bar.visible || bar.w <= 1 || bar.h <= 1) return;
    const float inset = 2.f;
    SkPaint track;
    track.setAntiAlias(true);
    track.setColor(SkColorSetARGB(48, 32, 36, 44));
    canvas->drawRoundRect(SkRect::MakeXYWH(static_cast<float>(bar.x), static_cast<float>(bar.y),
                                            static_cast<float>(bar.w), static_cast<float>(bar.h)),
                          6.f, 6.f, track);
    const float thumbW = std::max(1.f, static_cast<float>(bar.thumbW) - inset * 2.f);
    const float thumbH = std::max(1.f, static_cast<float>(bar.thumbH) - inset * 2.f);
    const SkRect thumb = SkRect::MakeXYWH(static_cast<float>(bar.thumbX) + inset, static_cast<float>(bar.thumbY) + inset,
                                          thumbW, thumbH);
    SkPaint thumbPaint;
    thumbPaint.setAntiAlias(true);
    thumbPaint.setColor(SkColorSetARGB(190, 72, 78, 88));
    const float radius = std::min(thumbW, thumbH) * 0.5f;
    canvas->drawRoundRect(thumb, radius, radius, thumbPaint);
}

// 눈금자 숫자 0~9 의 윤곽. 처음 그릴 때 함초롬돋움에서 한 번 떠 두고 계속 쓴다(몇 KB).
// 글꼴 예산이 함초롬돋움을 접은 뒤에 새 숫자가 나와도 그리는 도중에 글꼴 파일을 다시 열지 않는다.
struct RulerDigit {
    bool ready = false;
    std::vector<GlyphCommand> commands;
    float advance1000 = 500.f;
    int fitX = 1000;
    int fitY = 1000;
};

// 아직 못 뜬 숫자(글꼴을 받기 전)는 다음에 다시 뜬다. 한 번 뜬 숫자는 글꼴을 다시 찾지 않는다.
const RulerDigit& rulerDigit(int d) {
    static std::array<RulerDigit, 10> digits;
    static std::mutex mu;
    std::lock_guard lock(mu);
    RulerDigit& digit = digits[static_cast<std::size_t>(d)];
    if (digit.ready) return digit;
    static const std::string face = "함초롬돋움";
    const GlyphMem* mem = glyphMemory(face, static_cast<char32_t>('0' + d));
    if (!mem || !mem->face) return digit;
    digit.commands = hangulCurves(mem, false, false, 0);
    digit.advance1000 = mem->advance1000;
    digit.fitX = mem->fitX;
    digit.fitY = mem->fitY;
    digit.ready = true;
    return digit;
}

// 눈금자 숫자. 함초롬돋움 윤곽을 그대로 칠한다. 가운데 x, 기준선 y 에 맞춘다.
void drawRulerNumber(SkCanvas* canvas, int value, float centerX, float baseY, float emPx, const SkPaint& ink) {
    const std::string digits = std::to_string(value);
    float total = 0.f;
    for (char c : digits) {
        if (c < '0' || c > '9') continue;
        total += rulerDigit(c - '0').advance1000 * emPx / 1000.f;
    }
    float pen = centerX - total / 2.f;
    for (char c : digits) {
        if (c < '0' || c > '9') continue;
        const RulerDigit& digit = rulerDigit(c - '0');
        if (!digit.commands.empty()) {
            canvas->drawPath(glyphPath(digit.commands, emPx, 100, pen, baseY, digit.fitX, digit.fitY), ink);
        }
        pen += digit.advance1000 * emPx / 1000.f;
    }
}

// 안내선 색. 그리기 안내선은 한글 2024 화면에서 잰 하늘색 점선(6화소 그림·6화소 빔, 종이 밖으로 7화소)이고, 개체가 붙으면
// 실선이 된다. 개체 이동 안내선과 손잡이는 한글 도움말 그림의 남색이다. 풍선은 한글 크기 풍선의 연노랑이다.
constexpr SkColor kDrawGuideColor = SkColorSetARGB(255, 143, 222, 255);
constexpr SkColor kDrawGuideHeldColor = SkColorSetARGB(255, 0, 150, 255);
constexpr SkColor kMoveGuideColor = SkColorSetARGB(255, 71, 71, 162);
constexpr SkColor kGuideTipFill = SkColorSetARGB(255, 252, 244, 206);
constexpr SkColor kGuideTipEdge = SkColorSetARGB(255, 222, 208, 150);
constexpr SkColor kGuideTipInk = SkColorSetARGB(255, 51, 51, 51);
constexpr float kDrawGuideDash = 6.f;
constexpr float kDrawGuideOverhang = 7.f;

// 풍선 글자의 윤곽(함초롬돋움, 없으면 받아 둔 기본 글꼴). 한 번 뜬 글자는 다시 찾지 않는다. 글꼴이 아직 없으면 다음에 다시 뜬다.
const RulerDigit& uiGlyph(char32_t cp) {
    static std::unordered_map<char32_t, RulerDigit> glyphs;
    static std::mutex mu;
    std::lock_guard lock(mu);
    RulerDigit& glyph = glyphs[cp];
    if (glyph.ready) return glyph;
    // 문서가 함초롬돋움을 받지 않았으면 받아 둔 기본 글꼴로 그린다.
    const GlyphMem* mem = nullptr;
    for (const char* face : {"함초롬돋움", "함초롬바탕", "맑은 고딕", "바탕", "돋움"}) {
        mem = glyphMemory(face, cp);
        if (mem && mem->face) break;
    }
    if (!mem || !mem->face) return glyph;
    glyph.commands = hangulCurves(mem, false, false, 0);
    glyph.advance1000 = mem->advance1000;
    glyph.fitX = mem->fitX;
    glyph.fitY = mem->fitY;
    glyph.ready = true;
    return glyph;
}

float uiTextWidth(const std::string& text, float emPx) {
    float total = 0.f;
    std::size_t index = 0;
    char32_t cp = 0;
    while (nextCp(text, index, cp)) total += uiGlyph(cp).advance1000 * emPx / 1000.f;
    return total;
}

void drawUiText(SkCanvas* canvas, const std::string& text, float x, float baseY, float emPx, const SkPaint& ink) {
    std::size_t index = 0;
    char32_t cp = 0;
    float pen = x;
    while (nextCp(text, index, cp)) {
        const RulerDigit& glyph = uiGlyph(cp);
        if (!glyph.commands.empty()) {
            canvas->drawPath(glyphPath(glyph.commands, emPx, 100, pen, baseY, glyph.fitX, glyph.fitY), ink);
        }
        pen += glyph.advance1000 * emPx / 1000.f;
    }
}

// HU 를 한글 풍선처럼 cm 둘째 자리로.
std::string guideCm(double hu) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2fcm", hu * 2.54 / 7200.0);
    return buf;
}

// 축에 나란한 1화소 선. dash 가 0 이면 실선이다.
void guideLine(SkCanvas* canvas, bool vertical, float at, float from, float to, float width, float dash,
               const SkPaint& paint) {
    if (to <= from) return;
    const float pos = std::round(at);
    if (dash <= 0) {
        canvas->drawRect(vertical ? SkRect::MakeXYWH(pos, from, width, to - from)
                                  : SkRect::MakeXYWH(from, pos, to - from, width),
                         paint);
        return;
    }
    for (float t = from; t < to; t += dash * 2.f) {
        const float end = std::min(to, t + dash);
        canvas->drawRect(vertical ? SkRect::MakeXYWH(pos, t, width, end - t) : SkRect::MakeXYWH(t, pos, end - t, width),
                         paint);
    }
}

// 상자 (x, y) 바로 위에 풍선을 띄운다. 화면 밖으로 나가지 않게 민다.
void drawGuideTip(SkCanvas* canvas, const std::string& text, float x, float y, float dpr, int viewW, int viewH) {
    const float em = 12.f * dpr;
    const float pad = 4.f * dpr;
    const float w = uiTextWidth(text, em) + pad * 2.f;
    const float h = em * 1.6f;
    float left = x;
    float top = y - h - 4.f * dpr;
    if (top < 0) top = y + 4.f * dpr;
    left = std::clamp(left, 0.f, std::max(0.f, static_cast<float>(viewW) - w));
    top = std::clamp(top, 0.f, std::max(0.f, static_cast<float>(viewH) - h));
    SkPaint fill;
    fill.setColor(kGuideTipFill);
    canvas->drawRect(SkRect::MakeXYWH(left, top, w, h), fill);
    SkPaint edge;
    edge.setColor(kGuideTipEdge);
    const float line = std::max(1.f, std::round(dpr));
    canvas->drawRect(SkRect::MakeXYWH(left, top, w, line), edge);
    canvas->drawRect(SkRect::MakeXYWH(left, top + h - line, w, line), edge);
    canvas->drawRect(SkRect::MakeXYWH(left, top, line, h), edge);
    canvas->drawRect(SkRect::MakeXYWH(left + w - line, top, line, h), edge);
    SkPaint ink;
    ink.setAntiAlias(true);
    ink.setColor(kGuideTipInk);
    drawUiText(canvas, text, left + pad, top + h * 0.5f + em * 0.36f, em, ink);
}

// 그리기 안내선과 개체 이동 안내선. 선은 쪽 그림 위·캐럿 아래에(tips 거짓), 풍선은 옮기는 개체 미리보기 위에(tips 참) 그린다.
void drawViewGuides(SkCanvas* canvas, const std::vector<ViewPage>& pages, int scrollX, int scrollY, int viewW,
                    int viewH, double dpr, bool tips) {
    const ViewGuides guides = viewGuides();
    const GuideSettings& guide = guides.guide;
    const PointerHold& hold = pointerHold();
    const float scale = static_cast<float>(dpr > 0 ? dpr : 1.0);
    const float px = std::max(1.f, std::round(scale));
    // 쪽 HU → 화면 화소.
    struct Mapped {
        bool ok = false;
        float left = 0, top = 0, sx = 0, sy = 0;
        PaperBox box;
    };
    const auto mapPage = [&](int index) {
        Mapped m;
        const ViewPage* page = sheetOf(pages, index);
        if (!page || page->w <= 0 || page->h <= 0) return m;
        const std::optional<PaperBox> box = screenPaperBox(index);
        if (!box || box->width <= 0 || box->height <= 0) return m;
        m.ok = true;
        m.box = *box;
        m.left = static_cast<float>(page->x - scrollX);
        m.top = static_cast<float>(page->y - scrollY);
        m.sx = static_cast<float>(page->w) / static_cast<float>(box->width);
        m.sy = static_cast<float>(page->h) / static_cast<float>(box->height);
        return m;
    };
    if (guide.drawShow && !tips) {
        for (const ViewPage& page : pages) {
            const Mapped m = mapPage(page.index);
            if (!m.ok) continue;
            const float w = static_cast<float>(m.box.width) * m.sx;
            const float h = static_cast<float>(m.box.height) * m.sy;
            const float over = kDrawGuideOverhang * scale;
            const float gx = m.left + static_cast<float>(guide.drawX.value_or(m.box.width / 2)) * m.sx;
            const float gy = m.top + static_cast<float>(guide.drawY.value_or(m.box.height / 2)) * m.sy;
            const bool here = (hold.moving && hold.movePage == page.index) || (hold.guideDrag != 0 && hold.movePage == page.index);
            const bool heldX = here && (hold.guideSnapX || hold.guideDrag == 1);
            const bool heldY = here && (hold.guideSnapY || hold.guideDrag == 2);
            SkPaint plain;
            plain.setColor(kDrawGuideColor);
            SkPaint held;
            held.setColor(kDrawGuideHeldColor);
            guideLine(canvas, true, gx, m.top - over, m.top + h + over, px, heldX ? 0.f : kDrawGuideDash * scale,
                      heldX ? held : plain);
            guideLine(canvas, false, gy, m.left - over, m.left + w + over, px, heldY ? 0.f : kDrawGuideDash * scale,
                      heldY ? held : plain);
        }
    }
    if (!guide.moveShow) return;
    // 옮기는 개체: 3×3 안내선, 손잡이, 위치 풍선.
    if (hold.moving && !hold.boxes.empty()) {
        const Mapped m = mapPage(hold.movePage);
        if (m.ok) {
            const HeldBox& held = hold.boxes.front();
            const std::int32_t leftHu = held.x + hold.moveDX;
            const std::int32_t topHu = held.y + hold.moveDY;
            const float x0 = m.left + static_cast<float>(leftHu) * m.sx;
            const float y0 = m.top + static_cast<float>(topHu) * m.sy;
            const float x1 = x0 + static_cast<float>(held.width) * m.sx;
            const float y1 = y0 + static_cast<float>(held.height) * m.sy;
            const float xm = (x0 + x1) * 0.5f;
            const float ym = (y0 + y1) * 0.5f;
            SkPaint line;
            line.setColor(kMoveGuideColor);
            const float dash = guide.dashed ? 3.f * scale : 0.f;
            if (!tips) {
                for (const float x : {x0, xm, x1}) {
                    guideLine(canvas, true, x, 0.f, static_cast<float>(viewH), px, dash, line);
                }
                for (const float y : {y0, ym, y1}) {
                    guideLine(canvas, false, y, 0.f, static_cast<float>(viewW), px, dash, line);
                }
                const float knob = 3.f * scale;
                for (const float x : {x0, xm, x1}) {
                    for (const float y : {y0, ym, y1}) {
                        if (x == xm && y == ym) continue;
                        canvas->drawRect(SkRect::MakeXYWH(x - knob, y - knob, knob * 2.f, knob * 2.f), line);
                    }
                }
            }
            if (tips && guide.tipPosition) {
                const double baseX = guide.fromPaper ? 0.0 : static_cast<double>(m.box.bodyLeft);
                const double baseY = guide.fromPaper ? 0.0 : static_cast<double>(m.box.bodyTop);
                const std::string text = "가로 = " + guideCm(leftHu - baseX) + ", 세로 = " + guideCm(topHu - baseY);
                drawGuideTip(canvas, text, x0, y0, scale, viewW, viewH);
            }
        }
    }
    if (!tips || !guide.tipSize) return;
    // 키우는 개체와 그리는 도형: 크기 풍선(한글: 너비 = 2.80cm, 높이 = 4.55cm).
    if (hold.resizeHandle >= 0 && !hold.boxes.empty()) {
        const Mapped m = mapPage(hold.resizePage);
        if (m.ok) {
            const HeldBox& held = hold.boxes.front();
            const std::string text = "너비 = " + guideCm(held.width) + ", 높이 = " + guideCm(held.height);
            drawGuideTip(canvas, text, m.left + static_cast<float>(held.x) * m.sx, m.top + static_cast<float>(held.y) * m.sy,
                         scale, viewW, viewH);
        }
    }
    if (hold.drawing && hold.drawPage >= 0) {
        const Mapped m = mapPage(hold.drawPage);
        if (m.ok) {
            const std::int32_t w = std::abs(hold.drawX1 - hold.drawX0);
            const std::int32_t h = std::abs(hold.drawY1 - hold.drawY0);
            const std::string text = "너비 = " + guideCm(w) + ", 높이 = " + guideCm(h);
            drawGuideTip(canvas, text, m.left + static_cast<float>(std::min(hold.drawX0, hold.drawX1)) * m.sx,
                         m.top + static_cast<float>(std::min(hold.drawY0, hold.drawY1)) * m.sy, scale, viewW, viewH);
        }
    }
}

// 눈금 간격은 화면에 3픽셀 이상 벌어지는 가장 작은 mm 다. 숫자는 서로 닿지 않는 가장 작은 간격마다 붙인다.
void rulerSteps(double pxPerMm, double unit, int& minor, int& label) {
    static constexpr int kSteps[] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000};
    minor = kSteps[std::size(kSteps) - 1];
    for (int step : kSteps) {
        if (step * pxPerMm >= 3.0 * unit) {
            minor = step;
            break;
        }
    }
    label = kSteps[std::size(kSteps) - 1];
    for (int step : kSteps) {
        if (step >= 10 && step * pxPerMm >= 24.0 * unit) {
            label = step;
            break;
        }
    }
}

// 한 구간의 눈금. from..to 는 그릴 화면 범위다.
// 가로 눈금자는 아래 변에서, 세로 눈금자는 오른쪽 변에서 눈금이 자란다.
void drawRulerSpan(SkCanvas* canvas, const RulerSpan& span, bool across, int ruler, int from, int to) {
    if (span.length <= 0 || span.lengthHu <= 0) return;
    const float thick = static_cast<float>(ruler);
    const float unit = thick / static_cast<float>(PageView::kRulerCss);
    const double huPerMm = 7200.0 / 25.4;
    const double totalMm = static_cast<double>(span.lengthHu) / huPerMm;
    const double pxPerMm = static_cast<double>(span.length) / totalMm;
    SkPaint paper;
    paper.setColor(SK_ColorWHITE);
    const float a0 = static_cast<float>(span.start);
    const float a1 = static_cast<float>(span.start + span.length);
    canvas->drawRect(across ? SkRect::MakeLTRB(a0, 0.f, a1, thick - 1.f) : SkRect::MakeLTRB(0.f, a0, thick - 1.f, a1),
                     paper);
    int minor = 1;
    int label = 10;
    rulerSteps(pxPerMm, unit, minor, label);
    const int lastMm = static_cast<int>(std::floor(totalMm + 1e-6));
    const int firstMm = std::max(0, static_cast<int>(std::floor((from - span.start) / pxPerMm)) - label);
    const int endMm = std::min(lastMm, static_cast<int>(std::ceil((to - span.start) / pxPerMm)) + label);
    SkPaint ink;
    ink.setAntiAlias(true);
    ink.setColor(SkColorSetARGB(255, 92, 98, 108));
    SkPaint tick;
    tick.setColor(SkColorSetARGB(255, 120, 126, 136));
    tick.setStrokeWidth(std::max(1.f, std::round(unit)));
    const float emPx = std::round(9.f * unit);
    for (int mm = firstMm - firstMm % minor; mm <= endMm; mm += minor) {
        const float at = std::round(static_cast<float>(span.start + mm * pxPerMm)) + 0.5f;
        float len = thick * 0.18f;
        if (mm % label == 0) len = thick * 0.3f;
        else if (mm % 10 == 0) len = thick * 0.38f;
        else if (mm % 5 == 0 && minor < 5) len = thick * 0.28f;
        if (across) canvas->drawLine(at, thick - len, at, thick, tick);
        else canvas->drawLine(thick - len, at, thick, at, tick);
        if (mm == 0 || mm % label != 0) continue;
        if (across) drawRulerNumber(canvas, mm, at, thick * 0.6f, emPx, ink);
        else drawRulerNumber(canvas, mm, (thick - len) * 0.5f, at + emPx * 0.35f, emPx * 0.9f, ink);
    }
}

void drawRulers(SkCanvas* canvas, int viewW, int viewH, int ruler, const std::vector<ViewPage>& pages, int scrollX,
                int scrollY) {
    if (!canvas || ruler <= 0) return;
    std::vector<RulerSpan> across;
    std::vector<RulerSpan> down;
    layoutRulers(pages, scrollX, scrollY, viewW, viewH, ruler, across, down);
    const float thick = static_cast<float>(ruler);
    SkPaint band;
    band.setColor(SkColorSetARGB(255, 236, 238, 241));
    canvas->drawRect(SkRect::MakeLTRB(0.f, 0.f, static_cast<float>(viewW), thick), band);
    canvas->drawRect(SkRect::MakeLTRB(0.f, thick, thick, static_cast<float>(viewH)), band);
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(thick, 0.f, static_cast<float>(viewW), thick));
    for (const RulerSpan& span : across) drawRulerSpan(canvas, span, true, ruler, ruler, viewW);
    canvas->restore();
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(0.f, thick, thick, static_cast<float>(viewH)));
    for (const RulerSpan& span : down) drawRulerSpan(canvas, span, false, ruler, ruler, viewH);
    canvas->restore();
    SkPaint edge;
    edge.setColor(SkColorSetARGB(255, 178, 184, 194));
    canvas->drawRect(SkRect::MakeLTRB(0.f, thick - 1.f, static_cast<float>(viewW), thick), edge);
    canvas->drawRect(SkRect::MakeLTRB(thick - 1.f, 0.f, thick, static_cast<float>(viewH)), edge);
}

// Skia Graphite 가 GPU 에 쥔 자원 캐시(중간 텍스처·버퍼 등). 우리 캐시와 따로 GPU 메모리를 쓰니 크기를 같이 보인다.
// 지금은 재기만 한다. 예산은 Graphite 기본값을 쓴다.
class SkiaGpuPool final : public CachePool {
 public:
    const char* name() const override { return "skiaGpu"; }
    std::size_t bytes() const override {
        const GpuSlot& slot = gpuSlot();
        std::size_t total = 0;
        if (slot.context) total += slot.context->currentBudgetedBytes();
        if (slot.recorder) total += slot.recorder->currentBudgetedBytes();
        return total;
    }
    std::size_t release(CacheTier, std::size_t) override { return 0; }
};

void registerSkiaGpuPool() {
    static SkiaGpuPool pool;
    static const bool registered = (registerCachePool(pool, kNoCacheBudget), true);
    (void)registered;
}

// 2D 그림 목록의 그림 번호. 칸 그림은 칸 번호와 판으로 셸에 한 번만 올린다. 칸이 아닌 그림은 그림 번호로 올린다.
int screenImageKey(const SkImage* image) {
    if (!image) return 0;
    SkPixmap pixmap;
    std::vector<std::uint8_t> copied;
    if (!image->peekPixels(&pixmap) || pixmap.colorType() != kRGBA_8888_SkColorType) {
        const SkImageInfo info =
            SkImageInfo::Make(image->width(), image->height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType);
        copied.resize(info.computeMinByteSize());
        if (copied.empty() || !image->readPixels(nullptr, info, copied.data(), info.minRowBytes(), 0, 0)) return 0;
        pixmap = SkPixmap(info, copied.data(), info.minRowBytes());
    }
    std::uint64_t source = (std::uint64_t{1} << 40) | image->uniqueID();
    int stamp = 0;
    if (const auto found = rasterImageSources().find(image->uniqueID()); found != rasterImageSources().end()) {
        source = static_cast<std::uint32_t>(found->second.first);
        stamp = found->second.second;
    }
    return screenBitmaps().use(source, stamp, pixmap.width(), pixmap.height(),
                               static_cast<const std::uint8_t*>(pixmap.addr()), pixmap.rowBytes());
}

// 격자 색(한글 2024 화면에서 잰 값): 선 격자는 회색, 점 격자는 남색 1화소 점이다.
constexpr SkColor kGridLineColor = SkColorSetARGB(255, 192, 192, 192);
constexpr SkColor kGridDotColor = SkColorSetARGB(255, 0, 0, 128);
// 한 칸이 이보다 좁으면 격자가 면처럼 뭉개져 그리지 않는다(장치 화소).
constexpr float kGridMinSpanPx = 2.f;
// 2D 화면에서 점 격자를 사각형으로 찍는 한도. 넘으면 그 쪽 점은 찍지 않는다.
constexpr std::size_t kGridMaxDots2D = 40000;

// 격자 셰이더. 화소 p 가 격자 자리(origin + k*span 을 반올림한 화소)에 들면 칠한다.
// kind: 0 점, 1 가로선, 2 세로선, 3 가로/세로선.
const sk_sp<SkRuntimeEffect>& gridEffect() {
    static const sk_sp<SkRuntimeEffect> effect = [] {
        const char* sksl =
            "uniform float2 origin;\n"
            "uniform float2 span;\n"
            "uniform float kind;\n"
            "uniform half4 color;\n"
            "half4 main(float2 p) {\n"
            "    float2 d = p - origin + 0.5;\n"
            "    float2 m = d - span * floor(d / span);\n"
            "    bool onX = m.x < 1.0;\n"
            "    bool onY = m.y < 1.0;\n"
            "    bool on = kind < 0.5 ? (onX && onY) : (kind < 1.5 ? onY : (kind < 2.5 ? onX : (onX || onY)));\n"
            "    return on ? color : half4(0);\n"
            "}\n";
        auto made = SkRuntimeEffect::MakeForShader(SkString(sksl));
        if (!made.effect) std::fprintf(stderr, "[ClassingHWP] 격자 셰이더 실패: %s\n", made.errorText.c_str());
        return made.effect;
    }();
    return effect;
}

// 한 쪽의 격자를 그린다. paperX·paperY 는 그 쪽 종이 왼쪽 위 화소, scaleX·scaleY 는 HU 당 화소, clip 은 그릴 수 있는 범위다.
// 셰이더를 못 쓰는 화면(2D 그림 목록)은 선과 점을 1화소 사각형으로 찍는다.
void drawPaperGrid(SkCanvas* canvas, const GridPlan& plan, float paperX, float paperY, float scaleX, float scaleY,
                   const SkRect& clip, bool useShader) {
    if (!canvas || !plan.show || scaleX <= 0 || scaleY <= 0) return;
    const float spanX = static_cast<float>(plan.spanX) * scaleX;
    const float spanY = static_cast<float>(plan.spanY) * scaleY;
    if (spanX < kGridMinSpanPx || spanY < kGridMinSpanPx) return;
    SkRect area = SkRect::MakeLTRB(paperX + static_cast<float>(plan.left) * scaleX,
                                   paperY + static_cast<float>(plan.top) * scaleY,
                                   paperX + static_cast<float>(plan.right) * scaleX,
                                   paperY + static_cast<float>(plan.bottom) * scaleY);
    area.roundOut(&area);
    if (!area.intersect(clip)) return;
    const float originX = paperX + static_cast<float>(plan.originX) * scaleX;
    const float originY = paperY + static_cast<float>(plan.originY) * scaleY;
    const SkColor color = plan.kind == GridKind::Dots ? kGridDotColor : kGridLineColor;
    if (useShader && gridEffect()) {
        const float kind = plan.kind == GridKind::Dots         ? 0.f
                           : plan.kind == GridKind::Horizontal ? 1.f
                           : plan.kind == GridKind::Vertical   ? 2.f
                                                               : 3.f;
        SkRuntimeShaderBuilder builder(gridEffect());
        builder.uniform("origin") = SkV2{originX, originY};
        builder.uniform("span") = SkV2{spanX, spanY};
        builder.uniform("kind") = kind;
        builder.uniform("color") = SkColor4f::FromColor(color).premul();
        SkPaint paint;
        paint.setShader(builder.makeShader());
        canvas->drawRect(area, paint);
        return;
    }
    SkPaint paint;
    paint.setColor(color);
    // 화면 화소를 HU 로 되돌려 그 범위의 격자 자리만 고른다.
    const auto huX = [&](float px) { return static_cast<double>((px - paperX) / scaleX); };
    const auto huY = [&](float py) { return static_cast<double>((py - paperY) / scaleY); };
    const std::vector<double> xs = gridStops(plan.originX, plan.spanX, huX(area.fLeft), huX(area.fRight));
    const std::vector<double> ys = gridStops(plan.originY, plan.spanY, huY(area.fTop), huY(area.fBottom));
    const auto pxX = [&](double hu) { return std::round(paperX + static_cast<float>(hu) * scaleX); };
    const auto pxY = [&](double hu) { return std::round(paperY + static_cast<float>(hu) * scaleY); };
    if (plan.kind == GridKind::Dots) {
        if (xs.size() * ys.size() > kGridMaxDots2D) return;
        for (const double y : ys) {
            for (const double x : xs) canvas->drawRect(SkRect::MakeXYWH(pxX(x), pxY(y), 1.f, 1.f), paint);
        }
        return;
    }
    if (plan.kind == GridKind::Vertical || plan.kind == GridKind::Both) {
        for (const double x : xs) canvas->drawRect(SkRect::MakeXYWH(pxX(x), area.fTop, 1.f, area.height()), paint);
    }
    if (plan.kind == GridKind::Horizontal || plan.kind == GridKind::Both) {
        for (const double y : ys) canvas->drawRect(SkRect::MakeXYWH(area.fLeft, pxY(y), area.width(), 1.f), paint);
    }
}

// 화면에 놓인 쪽 종이(quad)의 격자. 그 쪽 종이·본문 영역은 세션이 프레임마다 넣는다.
void drawQuadGrid(SkCanvas* canvas, const GridSettings& grid, const ScreenSprite& quad, const SkRect& clip,
                  bool useShader) {
    if (!grid.show || quad.page < 0 || quad.w <= 0 || quad.h <= 0) return;
    const std::optional<PaperBox> box = screenPaperBox(quad.page);
    if (!box || box->width <= 0 || box->height <= 0) return;
    drawPaperGrid(canvas, gridPlanFor(grid, *box), static_cast<float>(quad.x), static_cast<float>(quad.y),
                  static_cast<float>(quad.w) / static_cast<float>(box->width),
                  static_cast<float>(quad.h) / static_cast<float>(box->height), clip, useShader);
}

// 한 쪽의 칸 조각(바탕 비트맵·그림·테두리·글자 쿼드)을 GPU 캔버스 pageCanvas 에 차례대로 굽는다.
// 화면의 쪽 텍스처와 쪽 그림(renderPageGpu)이 같이 쓴다. origin 은 조각 좌표에서 뺄 쪽 왼쪽 위다.
void bakePageSprites(SkCanvas* pageCanvas, GpuSlot& slot, const std::vector<const ScreenSprite*>& pageSprites,
                     int originX, int originY, int bakeW, int bakeH, int cacheZoom, int maskZoom, int cacheW,
                     int cacheH, bool dragging, int& mipmapUpgrades, bool& pageIncomplete, bool& pagePicturesCoarse,
                     int gridPage = -1) {
    pageCanvas->clear(SK_ColorWHITE);
    // 화면 쪽 텍스처는 글 뒤 격자를 종이 위·칸 아래에 품는다. 쪽 그림(gridPage < 0)에는 넣지 않는다.
    if (gridPage >= 0) {
        const GridSettings grid = viewGuides().grid;
        if (grid.show && !grid.front) {
            ScreenSprite quad;
            quad.page = gridPage;
            quad.w = bakeW;
            quad.h = bakeH;
            drawQuadGrid(pageCanvas, grid, quad, SkRect::MakeWH(static_cast<float>(bakeW), static_cast<float>(bakeH)),
                         !slot.raster);
        }
    }
    std::vector<std::vector<GlyphQuad>> pSharp;
    std::vector<std::vector<GlyphQuad>> pScaled;
    GlyphRect pBatchBounds;
    bool pHasPending = false;
    auto flushPageGlyphs = [&]() {
        if (!pHasPending) return;
        drawGlyphQuads(pageCanvas, slot.recorder.get(), pSharp, SkFilterMode::kNearest);
        drawGlyphQuads(pageCanvas, slot.recorder.get(), pScaled, SkFilterMode::kLinear);
        for (auto& pq : pSharp) pq.clear();
        for (auto& pq : pScaled) pq.clear();
        pBatchBounds = GlyphRect{};
        pHasPending = false;
    };
    const std::vector<SkRect> emptyBacks;
    sk_sp<SkColorFilter> emptyInverted;
    for (const ScreenSprite* spritePtr : pageSprites) {
        const ScreenSprite& sprite = *spritePtr;
        ScreenSprite localSprite = sprite;
        localSprite.x = sprite.x - originX;
        localSprite.y = sprite.y - originY;
        if (const auto pictures = framePictures(sprite.id); pictures && localSprite.sw > 0 &&
                                                             localSprite.sh > 0) {
            const SkRect dst = SkRect::MakeXYWH(
                static_cast<float>(localSprite.x), static_cast<float>(localSprite.y),
                static_cast<float>(localSprite.w), static_cast<float>(localSprite.h));
            if (pHasPending && pBatchBounds.intersects(dst.fLeft, dst.fTop, dst.fRight, dst.fBottom)) flushPageGlyphs();
            if (!drawFramePictures(pageCanvas, slot.recorder.get(), *pictures, dst,
                                   static_cast<float>(localSprite.w) / static_cast<float>(localSprite.sw),
                                   static_cast<float>(localSprite.h) / static_cast<float>(localSprite.sh),
                                   dragging, emptyBacks, emptyInverted)) {
                pagePicturesCoarse = true;
            }
        }
        if (frameHasBorders(sprite.id) && localSprite.sw > 0 && localSprite.sh > 0) {
            const SkRect dst = SkRect::MakeXYWH(
                static_cast<float>(localSprite.x), static_cast<float>(localSprite.y),
                static_cast<float>(localSprite.w), static_cast<float>(localSprite.h));
            if (pHasPending && pBatchBounds.intersects(dst.fLeft, dst.fTop, dst.fRight, dst.fBottom)) flushPageGlyphs();
            drawSpriteBorders(pageCanvas, sprite.id, dst, localSprite.sw, localSprite.sh, emptyBacks,
                              emptyInverted);
        }
        if (localSprite.rgba && localSprite.sw > 1 && localSprite.sh > 1) {
            const bool exactSize = localSprite.w == localSprite.sw && localSprite.h == localSprite.sh;
            const bool heavyShrink =
                !localSprite.nearest &&
                (localSprite.w * 2 < localSprite.sw || localSprite.h * 2 < localSprite.sh);
            bool mipmapped = false;
            const sk_sp<SkImage> image = screenTexture(slot.recorder.get(), localSprite, heavyShrink,
                                                       dragging, mipmapped, mipmapUpgrades);
            if (image) {
                const SkRect dst = SkRect::MakeXYWH(static_cast<float>(localSprite.x),
                                                    static_cast<float>(localSprite.y),
                                                    static_cast<float>(localSprite.w),
                                                    static_cast<float>(localSprite.h));
                if (pHasPending && pBatchBounds.intersects(dst.fLeft, dst.fTop, dst.fRight, dst.fBottom)) {
                    flushPageGlyphs();
                }
                const SkSamplingOptions sampling =
                    (localSprite.nearest || exactSize)
                        ? SkSamplingOptions(SkFilterMode::kNearest)
                        : (heavyShrink || cacheZoom <= 75
                               ? (mipmapped
                                      ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                                      : SkSamplingOptions(SkFilterMode::kLinear))
                               : SkSamplingOptions(SkCubicResampler::CatmullRom()));
                drawSpriteImage(pageCanvas, slot.recorder.get(), image.get(), dst, sampling, emptyBacks,
                                emptyInverted, static_cast<float>(bakeW), static_cast<float>(bakeH));
            }
        }
        if (!slot.vectors) continue;
        bool spriteExact = true;
        appendGlyphQuads(localSprite, cacheZoom, maskZoom, cacheW, cacheH, false, pSharp, pScaled,
                         pBatchBounds, pHasPending, &spriteExact);
        if (!spriteExact) pageIncomplete = true;
    }
    flushPageGlyphs();
}

namespace {

struct PageReadBox {
    std::unique_ptr<const SkImage::AsyncReadResult> result;
};

void onPageRead(void* context, std::unique_ptr<const SkImage::AsyncReadResult> result) {
    static_cast<PageReadBox*>(context)->result = std::move(result);
}

bool submitPage(GpuSlot& slot) {
    std::unique_ptr<skgpu::graphite::Recording> recording = slot.recorder->snap();
    if (!recording) return false;
    skgpu::graphite::InsertRecordingInfo info;
    info.fRecording = recording.get();
    if (slot.context->insertRecording(info) != skgpu::graphite::InsertStatus::kSuccess) return false;
    return slot.context->submit(skgpu::graphite::SyncToCpu::kYes);
}

}  // namespace

PageBitmap renderPageGpu(const model::Document& doc, std::vector<Node>& pages, int pageIndex, int pixelHeight,
                         FrameCache& frames) {
    PageBitmap out;
    if (!pageImageGpu() || pageIndex < 0 || pageIndex >= static_cast<int>(pages.size()) || pixelHeight < 32) {
        return out;
    }
    GpuSlot& slot = gpuSlot();
    if (!slot.tried) beginDawnSurface();
    if (!slot.ready || slot.raster || !slot.context || !slot.recorder) return out;
    const Node& scene = pages[static_cast<std::size_t>(pageIndex)];
    if (scene.height <= 0 || scene.width <= 0) return out;
    const int pixelW = pagePixelWidth(viewWidthOf(scene), scene.height, pixelHeight);
    const int fullH = pagePixelHeight(scene.height, 100);
    const int percent = std::max(1, static_cast<int>(std::lround(100.0 * pixelHeight / std::max(1, fullH))));
    // 화면과 같이 칸을 굽는다. 100% 보다 작으면 100% 칸을 줄여 놓는다.
    // 칸을 고르는 범위는 구운 배율의 쪽 크기다. 100% 보다 작게 그려도 100% 칸 전부가 들어오게 넓힌다.
    ViewPlan plan;
    plan.viewW = std::max(pixelW, pagePixelWidth(viewWidthOf(scene), scene.height, fullH));
    plan.viewH = std::max(pixelHeight, fullH);
    plan.percent = percent;
    ViewPage view;
    view.index = pageIndex;
    view.w = pixelW;
    view.h = pixelHeight;
    view.widthHu = viewWidthOf(scene);
    view.heightHu = scene.height;
    plan.pages.push_back(view);
    frames.bakeVisibleNow(doc, pages, plan);
    const int bakePercent = percent < 100 ? 100 : percent;
    std::vector<PageSprite> placed = readyPageSprites(frames, pageIndex, bakePercent);
    placePageSprites(placed, percent, pixelHeight, scene.height);
    std::vector<ScreenSprite> sprites;
    sprites.reserve(placed.size());
    for (const PageSprite& item : placed) {
        ScreenSprite sprite;
        sprite.id = item.id;
        sprite.stamp = item.stamp;
        sprite.x = item.localX;
        sprite.y = item.localY;
        sprite.w = item.w;
        sprite.h = item.h;
        sprite.sw = item.sw;
        sprite.sh = item.sh;
        sprite.percent = item.percent;
        sprite.page = pageIndex;
        sprite.layer = item.layer;
        sprite.nearest = item.nearest;
        sprite.rgba = item.rgba;
        sprites.push_back(sprite);
    }
    std::vector<const ScreenSprite*> pageSprites;
    pageSprites.reserve(sprites.size());
    for (const ScreenSprite& sprite : sprites) pageSprites.push_back(&sprite);
    registerAtlasPool();
    registerSkiaGpuPool();
    const int maskZoom = restingMaskZoom(percent);
    const SkImageInfo info = SkImageInfo::Make(pixelW, pixelHeight, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface;
    // 그림은 GPU 로 올리는 데 몇 차례가 걸린다. 덜 올라간 그림이 있으면 다 올린 뒤 다시 굽는다.
    for (int round = 0; round < 64; ++round) {
        std::unique_lock<std::mutex> glyphLock(frameGlyphMutex());
        std::unique_lock<std::mutex> atlasLock(glyphAtlasMutex());
        if (slot.vectors) ensureGlyphMasks(sprites, percent, maskZoom, pixelW, pixelHeight, false);
        surface = SkSurfaces::RenderTarget(slot.recorder.get(), info, skgpu::Mipmapped::kNo);
        if (!surface) return out;
        int mipmapUpgrades = 0;
        bool incomplete = false;
        bool coarse = false;
        bakePageSprites(surface->getCanvas(), slot, pageSprites, 0, 0, pixelW, pixelHeight, percent, maskZoom, pixelW,
                        pixelHeight, false, mipmapUpgrades, incomplete, coarse);
        atlasLock.unlock();
        glyphLock.unlock();
        if (!coarse || !pictureUploadsWaiting()) break;
        while (pictureUploadsWaiting()) {
            advancePictureUploads(slot.recorder.get());
            if (!submitPage(slot)) return out;
        }
    }
    if (!submitPage(slot)) return out;
    PageReadBox box;
    slot.context->asyncRescaleAndReadPixels(surface.get(), info, SkIRect::MakeWH(pixelW, pixelHeight),
                                            SkImage::RescaleGamma::kSrc, SkImage::RescaleMode::kNearest, onPageRead,
                                            &box);
    slot.context->submit(skgpu::graphite::SyncToCpu::kYes);
    for (int spin = 0; !box.result && spin < 1000; ++spin) slot.context->checkAsyncWorkCompletion();
    if (!box.result) return out;
    const auto* src = static_cast<const std::uint8_t*>(box.result->data(0));
    const std::size_t rowBytes = box.result->rowBytes(0);
    out.width = pixelW;
    out.height = pixelHeight;
    out.rgba.resize(static_cast<std::size_t>(pixelW) * static_cast<std::size_t>(pixelHeight) * 4u);
    for (int y = 0; y < pixelHeight; ++y) {
        std::memcpy(out.rgba.data() + static_cast<std::size_t>(y) * pixelW * 4u,
                    src + static_cast<std::size_t>(y) * rowBytes, static_cast<std::size_t>(pixelW) * 4u);
    }
    return out;
}

bool presentFailed(const char* why) {
    static const char* last = nullptr;
    static int quiet = 0;
    if (last && why && std::strcmp(last, why) == 0) {
        if (quiet > 0) {
            --quiet;
            return false;
        }
    }
    last = why;
    quiet = 30;
    std::fprintf(stderr, "[ClassingHWP 오류] 화면: %s\n", why ? why : "알 수 없음");
    return false;
}

bool presentScreen(int viewW, int viewH, const std::vector<ScreenSprite>& papers, const std::vector<ScreenSprite>& sprites,
                   int layoutZoom, int maskZoom, const std::vector<ViewPage>& pages, int scrollX, int scrollY,
                   const Scrollbar& barX, const Scrollbar& barY, bool dragging, int ruler, double dpr) {
    beginCacheFrame();
    registerAtlasPool();
    registerSkiaGpuPool();
    pageTextures().ensureRegistered();
    // 격자·안내선. 격자 모양이 바뀌면 세션이 쪽 텍스처를 먼저 버린다(applyViewGuides).
    const ViewGuides guides = viewGuides();
    keepPictureBudget();
    if (viewW <= 0 || viewH <= 0) return presentFailed("화면 크기가 없습니다");
    // 화면에 걸친 쪽의 텍스처를 그리기 전에 모두 이번 프레임 것으로 표시한다.
    // 셸은 텍스처를 쓰기로 한 쪽의 조각을 보내지 않는다. 아래에서 다른 쪽을 구워 넣다 예산을 넘길 때
    // 아직 차례가 안 온 보이는 쪽 텍스처를 버리면 그 쪽은 흰 종이만 남는다. 이번 프레임 것은 예산이 버리지 않는다.
    // 보이는 쪽만으로 예산을 넘으면 그 프레임은 잠시 예산을 넘긴다. 화면을 벗어난 뒤 다음 저장 때 비운다.
    for (const ScreenSprite& quad : papers) {
        if (quad.page < 0 || quad.w <= 0 || quad.h <= 0) continue;
        if (quad.x >= viewW || quad.y >= viewH || quad.x + quad.w <= 0 || quad.y + quad.h <= 0) continue;
        pageTextures().markFrame(quad.page);
    }
    GpuSlot& slot = gpuSlot();
    if (!slot.ready) return presentFailed("GPU가 준비되지 않았습니다");
    if (!slot.raster) {
        const FramePhaseTimer timer(FramePhase::Pictures);
        advancePictureUploads(slot.recorder.get());
    }
    sk_sp<SkSurface> surface;
    ScreenFrame2D frame2d;
    std::unique_ptr<SkCanvas> opCanvas;
    rasterImageSources().clear();
    if (slot.raster && screenOpSink()) {
        // 2D 화면: 픽셀을 합성하지 않고 그림 목록을 낸다. 셸이 플랫폼 2D 그리기로 옮긴다.
        opCanvas = makeScreenOpCanvas(viewW, viewH, frame2d, &screenImageKey);
    } else if (slot.raster) {
        return presentFailed("2D 화면 그림 목록을 받을 곳이 없습니다");
    } else {
        if (!slot.context || !slot.recorder) return presentFailed("GPU 기록기가 없습니다");
        const FramePhaseTimer timer(FramePhase::Acquire);
        if (!configureDawnSurface(slot, viewW, viewH)) return presentFailed("화면 표면을 맞추지 못했습니다");
    }
    // 스프라이트가 없는 순수 페이지 GPU 텍스처 합성 프레임은 글자·아틀라스 뮤텍스를 잠그지 않는다.
    std::unique_lock<std::mutex> glyphLock(frameGlyphMutex(), std::defer_lock);
    std::unique_lock<std::mutex> atlasLock(glyphAtlasMutex(), std::defer_lock);
    if (!sprites.empty() || slot.raster) {
        glyphLock.lock();
        atlasLock.lock();
        const MaskPass pass = maskPassFor(papers, layoutZoom, viewW, viewH);
        if (slot.vectors && maskZoom > 0) {
            const FramePhaseTimer timer(FramePhase::Masks);
            ensureGlyphMasks(sprites, pass.layoutZoom, maskZoom, pass.viewW, pass.viewH, dragging);
        }
    }
    if (!slot.raster) {
        const FramePhaseTimer timer(FramePhase::Acquire);
        wgpu::SurfaceTexture surfaceTexture;
        slot.surface.GetCurrentTexture(&surfaceTexture);
        if (!surfaceTexture.texture) return presentFailed("GPU 텍스처를 받지 못했습니다");
        const skgpu::graphite::BackendTexture backend =
            skgpu::graphite::BackendTextures::MakeDawn(surfaceTexture.texture.Get());
        surface = SkSurfaces::WrapBackendTexture(slot.recorder.get(), backend, nullptr, nullptr);
        if (!surface) return presentFailed("GPU 화면을 감싸지 못했습니다");
    }
    SkCanvas* canvas = opCanvas ? opCanvas.get() : surface->getCanvas();
    canvas->clear(SkColorSetARGB(255, 213, 219, 227));
    // 걸친 칸과 글자 쿼드는 통째로 두고, 캔버스 밖으로 나간 픽셀만 자른다.
    canvas->clipRect(SkRect::MakeWH(static_cast<float>(viewW), static_cast<float>(viewH)));
    SkPaint paper;
    paper.setColor(SK_ColorWHITE);
    std::unordered_set<int> handledPages;
    int mipmapUpgrades = 0;
    // 쪽마다 무엇을 그릴지는 planPaper 가 정한다. 여기서는 그 차례대로 해 보고, GPU 가 못 하면 다음으로 간다.
    PageBakeBudget bakeBudget(dragging);
    PaperFrame paperFrame;
    paperFrame.viewW = viewW;
    paperFrame.viewH = viewH;
    paperFrame.layoutZoom = layoutZoom;
    paperFrame.maskZoom = maskZoom;
    paperFrame.dragging = dragging;
    paperFrame.texturesOn = pageTexturesOn();
    paperFrame.interacting = gRenderInteracting.load(std::memory_order_relaxed);
    paperFrame.maskSeq = maskBuiltSeq();
    paperFrame.pictureSeq = pictureTextureSeq();
    const std::unordered_map<int, std::vector<const ScreenSprite*>> bakeSpritesByPage =
        paperFrame.texturesOn ? spritesByPage(papers, sprites)
                              : std::unordered_map<int, std::vector<const ScreenSprite*>>{};

    sk_sp<SkColorFilter> inverted;
    FramePhaseTimer papersTimer(FramePhase::Papers);
    for (const ScreenSprite& quad : papers) {
        if (quad.w <= 0 || quad.h <= 0) continue;
        const auto pageSpritesIt = bakeSpritesByPage.find(quad.page);
        const bool pageHasSprites = pageSpritesIt != bakeSpritesByPage.end() && !pageSpritesIt->second.empty();
        const PaperPlan plan = planPaper(pageTextures(), quad, paperFrame, pageHasSprites, bakeBudget);
        const bool onScreen = plan.onScreen;
        const int cacheW = plan.cacheW;
        const int cacheH = plan.cacheH;
        const int cacheZoom = plan.cacheZoom;
        SkImage* const cachedImage = pageGpuImage(quad.page);
        if (cachedImage && plan.useTexture) {
            pageTextures().touch(quad.page);
            if (onScreen) {
                const SkSamplingOptions pageSampling =
                    cachedImage->hasMipmaps()
                        ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                        : SkSamplingOptions(SkFilterMode::kLinear);
                const PageTextureMeta* entry = pageTextures().find(quad.page);
                if (entry && entry->w == quad.w && entry->h == quad.h) {
                    canvas->drawImage(cachedImage, static_cast<float>(quad.x), static_cast<float>(quad.y));
                } else {
                    const SkRect dst = SkRect::MakeXYWH(static_cast<float>(quad.x), static_cast<float>(quad.y),
                                                        static_cast<float>(quad.w), static_cast<float>(quad.h));
                    canvas->drawImageRect(cachedImage, dst, pageSampling, nullptr);
                }
                drawPageHoldInversion(canvas, cachedImage, quad, pageSampling, viewW, viewH, inverted);
            }
            handledPages.insert(quad.page);
            continue;
        }
        const int bakeW = cacheW;
        const int bakeH = cacheH;
        const int quadPixels = bakeW * bakeH;
        if (plan.bake) {
            const FramePhaseTimer bakeTimer(FramePhase::PageBake);
            const std::vector<const ScreenSprite*>& pageSprites = pageSpritesIt->second;
            const SkImageInfo pageInfo =
                SkImageInfo::Make(bakeW, bakeH, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
            sk_sp<SkSurface> pageSurface =
                SkSurfaces::RenderTarget(slot.recorder.get(), pageInfo, skgpu::Mipmapped::kYes);
            if (!pageSurface) {
                pageSurface = SkSurfaces::RenderTarget(slot.recorder.get(), pageInfo, skgpu::Mipmapped::kNo);
            }
            if (pageSurface) {
                SkCanvas* pageCanvas = pageSurface->getCanvas();
                bool pageIncomplete = false;
                bool pagePicturesCoarse = false;
                bakePageSprites(pageCanvas, slot, pageSprites, quad.x, quad.y, bakeW, bakeH, cacheZoom, maskZoom,
                                cacheW, cacheH, dragging, mipmapUpgrades, pageIncomplete, pagePicturesCoarse, quad.page);
                if (sk_sp<SkImage> pageImg = SkSurfaces::AsImage(pageSurface)) {
                    if (onScreen) {
                        const SkSamplingOptions pageSampling =
                            pageImg->hasMipmaps()
                                ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                                : SkSamplingOptions(SkFilterMode::kLinear);
                        if (bakeW == quad.w && bakeH == quad.h) {
                            canvas->drawImage(pageImg.get(), static_cast<float>(quad.x), static_cast<float>(quad.y));
                        } else {
                            const SkRect dst =
                                SkRect::MakeXYWH(static_cast<float>(quad.x), static_cast<float>(quad.y),
                                                 static_cast<float>(quad.w), static_cast<float>(quad.h));
                            canvas->drawImageRect(pageImg.get(), dst, pageSampling, nullptr);
                        }
                        drawPageHoldInversion(canvas, pageImg.get(), quad, pageSampling, viewW, viewH, inverted);
                    }
                    storePageGpuCache(quad.page, bakeW, bakeH, cacheZoom, maskZoom, quad.stamp, pageIncomplete,
                                      std::move(pageImg));
                    pageTextures().setPicturesCoarse(quad.page, pagePicturesCoarse);
                    handledPages.insert(quad.page);
                    bakeBudget.spend(quadPixels);
                    continue;
                }
            }
        }
        SkImage* const scaledImage = pageGpuImage(quad.page);
        if (scaledImage && plan.scaleTexture) {
            pageTextures().touch(quad.page);
            if (onScreen) {
                const SkRect dst = SkRect::MakeXYWH(static_cast<float>(quad.x), static_cast<float>(quad.y),
                                                    static_cast<float>(quad.w), static_cast<float>(quad.h));
                const SkSamplingOptions pageSampling =
                    scaledImage->hasMipmaps()
                        ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                        : SkSamplingOptions(SkFilterMode::kLinear);
                canvas->drawImageRect(scaledImage, dst, pageSampling, nullptr);
                drawPageHoldInversion(canvas, scaledImage, quad, pageSampling, viewW, viewH, inverted);
            }
            handledPages.insert(quad.page);
            continue;
        }
        if (onScreen) {
            canvas->drawRect(SkRect::MakeXYWH(static_cast<float>(quad.x), static_cast<float>(quad.y),
                                               static_cast<float>(quad.w), static_cast<float>(quad.h)),
                             paper);
        } else {
            handledPages.insert(quad.page);
        }
    }
    papersTimer.stop();
    const SkRect viewClip = SkRect::MakeWH(static_cast<float>(viewW), static_cast<float>(viewH));
    // 글 뒤 격자: 쪽 텍스처로 그린 쪽은 텍스처가 품었다. 흰 종이 위에 칸을 바로 얹는 쪽만 여기서 깐다.
    if (guides.grid.show && !guides.grid.front) {
        for (const ScreenSprite& quad : papers) {
            if (handledPages.count(quad.page)) continue;
            drawQuadGrid(canvas, guides.grid, quad, viewClip, !slot.raster);
        }
    }
    FramePhaseTimer backsTimer(FramePhase::Overlay);
    const std::vector<SkRect> invertBacks =
        placeInvertBacks(canvas, pages, scrollX, scrollY, viewW, viewH, handledPages);
    backsTimer.stop();
    std::vector<std::vector<GlyphQuad>> sharp;
    std::vector<std::vector<GlyphQuad>> scaled;
    GlyphRect glyphBatchBounds;
    bool hasPendingGlyphs = false;
    auto flushGlyphBatches = [&]() {
        if (!hasPendingGlyphs) return;
        const FramePhaseTimer timer(FramePhase::Glyphs);
        drawGlyphQuads(canvas, slot.recorder.get(), sharp, SkFilterMode::kNearest);
        drawGlyphQuads(canvas, slot.recorder.get(), scaled, SkFilterMode::kLinear);
        for (auto& pageQuads : sharp) pageQuads.clear();
        for (auto& pageQuads : scaled) pageQuads.clear();
        glyphBatchBounds = GlyphRect{};
        hasPendingGlyphs = false;
    };
    const bool activeHold = hasActiveHold();
    FramePhaseTimer spritesTimer(FramePhase::Sprites);
    for (const ScreenSprite& sprite : sprites) {
        if (sprite.page >= 0 && handledPages.count(sprite.page)) continue;
        if (sprite.x >= viewW || sprite.y >= viewH || sprite.x + sprite.w <= 0 || sprite.y + sprite.h <= 0) continue;
        // 칸의 그림이 먼저다. 칸 비트맵의 테두리와 글자가 그 위에 얹힌다.
        if (const auto pictures = framePictures(sprite.id); pictures && sprite.sw > 0 && sprite.sh > 0) {
            const SkRect dst = SkRect::MakeXYWH(static_cast<float>(sprite.x), static_cast<float>(sprite.y),
                                                static_cast<float>(sprite.w), static_cast<float>(sprite.h));
            if (hasPendingGlyphs && glyphBatchBounds.intersects(dst.fLeft, dst.fTop, dst.fRight, dst.fBottom)) flushGlyphBatches();
            drawFramePictures(canvas, slot.recorder.get(), *pictures, dst,
                              static_cast<float>(sprite.w) / static_cast<float>(sprite.sw),
                              static_cast<float>(sprite.h) / static_cast<float>(sprite.sh), dragging, invertBacks,
                              inverted);
        }
        if (frameHasBorders(sprite.id) && sprite.sw > 0 && sprite.sh > 0) {
            const SkRect dst = SkRect::MakeXYWH(static_cast<float>(sprite.x), static_cast<float>(sprite.y),
                                                static_cast<float>(sprite.w), static_cast<float>(sprite.h));
            if (hasPendingGlyphs && glyphBatchBounds.intersects(dst.fLeft, dst.fTop, dst.fRight, dst.fBottom)) flushGlyphBatches();
            drawSpriteBorders(canvas, sprite.id, dst, sprite.sw, sprite.sh, invertBacks, inverted);
        }
        const bool exactSize = sprite.w == sprite.sw && sprite.h == sprite.sh;
        const bool heavyShrink = !sprite.nearest && (sprite.w * 2 < sprite.sw || sprite.h * 2 < sprite.sh);
        bool mipmapped = false;
        const sk_sp<SkImage> image =
            screenTexture(slot.recorder.get(), sprite, heavyShrink, dragging, mipmapped, mipmapUpgrades);
        if (image) {
            const SkRect dst = SkRect::MakeXYWH(static_cast<float>(sprite.x), static_cast<float>(sprite.y),
                                                static_cast<float>(sprite.w), static_cast<float>(sprite.h));
            if (hasPendingGlyphs && glyphBatchBounds.intersects(dst.fLeft, dst.fTop, dst.fRight, dst.fBottom)) {
                flushGlyphBatches();
            }
            // 드래그 중 확대 칸은 Linear로 붙인다. Catmull-Rom은 손이 멈춘 뒤에 쓴다.
            const SkSamplingOptions sampling =
                (sprite.nearest || exactSize)
                    ? SkSamplingOptions(SkFilterMode::kNearest)
                    : (slot.raster
                           ? SkSamplingOptions((dragging && heavyShrink) ? SkFilterMode::kNearest : SkFilterMode::kLinear)
                           : (heavyShrink
                                  ? (mipmapped ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                                               : SkSamplingOptions(SkFilterMode::kLinear))
                                  : (dragging ? SkSamplingOptions(SkFilterMode::kLinear)
                                              : SkSamplingOptions(SkCubicResampler::CatmullRom()))));
            drawSpriteImage(canvas, slot.recorder.get(), image.get(), dst, sampling, invertBacks, inverted,
                            static_cast<float>(viewW), static_cast<float>(viewH));
        }
        if (!slot.vectors) continue;
        const FramePhaseTimer glyphTimer(FramePhase::Glyphs);
        appendGlyphQuads(sprite, layoutZoom, maskZoom, viewW, viewH, activeHold, sharp, scaled, glyphBatchBounds,
                         hasPendingGlyphs);
    }
    flushGlyphBatches();
    spritesTimer.stop();
    {
        const FramePhaseTimer timer(FramePhase::Overlay);
        // 글 앞 격자는 글자·개체 위에 깐다.
        if (guides.grid.show && guides.grid.front) {
            for (const ScreenSprite& quad : papers) drawQuadGrid(canvas, guides.grid, quad, viewClip, !slot.raster);
        }
        drawViewGuides(canvas, pages, scrollX, scrollY, viewW, viewH, dpr, false);
        drawCaret(canvas, slot.recorder.get(), pages, scrollX, scrollY, !invertBacks.empty(), dpr);
        drawViewGuides(canvas, pages, scrollX, scrollY, viewW, viewH, dpr, true);
        drawRulers(canvas, viewW, viewH, ruler, pages, scrollX, scrollY);
        drawScrollbar(canvas, barX);
        drawScrollbar(canvas, barY);
    }
    if (opCanvas) {
        frame2d.width = viewW;
        frame2d.height = viewH;
        const FramePhaseTimer timer(FramePhase::Submit);
        screenBitmaps().finish(frame2d);
        if (screenOpSink()(frame2d)) return true;
        return presentFailed("2D 화면 그림 목록을 넘기지 못했습니다");
    }
    const FramePhaseTimer submitTimer(FramePhase::Submit);
    std::unique_ptr<skgpu::graphite::Recording> recording = slot.recorder->snap();
    if (!recording) return presentFailed("GPU 녹화를 끝내지 못했습니다");
    skgpu::graphite::InsertRecordingInfo info;
    info.fRecording = recording.get();
    if (slot.context->insertRecording(info) != skgpu::graphite::InsertStatus::kSuccess) {
        return presentFailed("GPU 녹화를 넣지 못했습니다");
    }
    if (!slot.context->submit()) return presentFailed("GPU 제출에 실패했습니다");
    // 브라우저 WebGPU 는 surface.Present 가 없다. 워커가 이 턴을 끝내면 캔버스가 화면에 올라간다.
    return true;
}

#else
namespace {
void dropScreenTexture(int) {}
void clearAtlasTextures() {}
void dropPageImage(int) {}
void clearPageImages() {}
bool pageTexturesOn() { return false; }
}  // namespace

bool screenReady() { return false; }

bool resetScreenSurface() { return false; }

bool attachRemoteScreen() { return false; }

bool presentScreen(int, int, const std::vector<ScreenSprite>&, const std::vector<ScreenSprite>&, int, int,
                   const std::vector<ViewPage>&, int, int, const Scrollbar&, const Scrollbar&, bool, int, double) {
    return false;
}

PageBitmap renderPageGpu(const model::Document&, std::vector<Node>&, int, int, FrameCache&) { return {}; }
#endif

void renderMemory(std::vector<std::pair<std::string, std::size_t>>& out) { collectRenderMemory(out); }

}  // namespace chwp::render
