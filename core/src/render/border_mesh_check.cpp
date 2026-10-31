#include "render/border_mesh_check.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "noting/render/paint.hpp"
#include "noting/render/view.hpp"
#include "render/border_mesh.hpp"
#include "render/painter.hpp"

#if NOTING_HAS_DAWN
#include <memory>

#include "dawn/dawn_proc.h"
#include "dawn/native/DawnNative.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkImage.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/GraphiteTypes.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#endif

namespace noting::render {

#if NOTING_HAS_DAWN
namespace {

using paint_detail::frameBorders;
using paint_detail::forceBordersOnScreen;

const char* backendName(wgpu::BackendType type) {
    switch (type) {
        case wgpu::BackendType::D3D11: return "D3D11";
        case wgpu::BackendType::D3D12: return "D3D12";
        case wgpu::BackendType::Vulkan: return "Vulkan";
        case wgpu::BackendType::Metal: return "Metal";
        case wgpu::BackendType::OpenGL: return "OpenGL";
        case wgpu::BackendType::OpenGLES: return "OpenGLES";
        default: return "other";
    }
}

struct ReadBox {
    std::unique_ptr<const SkImage::AsyncReadResult> result;
};

void onRead(void* context, std::unique_ptr<const SkImage::AsyncReadResult> result) {
    static_cast<ReadBox*>(context)->result = std::move(result);
}

bool submit(skgpu::graphite::Context* context, skgpu::graphite::Recorder* recorder) {
    std::unique_ptr<skgpu::graphite::Recording> recording = recorder->snap();
    if (!recording) return false;
    skgpu::graphite::InsertRecordingInfo info;
    info.fRecording = recording.get();
    if (context->insertRecording(info) != skgpu::graphite::InsertStatus::kSuccess) return false;
    return context->submit(skgpu::graphite::SyncToCpu::kYes);
}

// 테두리 메시를 그리고 남은 비트맵(rest)을 얹어 cpu 비트맵과 비교한다. 다른 픽셀 수를 돌려준다. 못 그리면 -1.
long compareOnGpu(skgpu::graphite::Context* context, skgpu::graphite::Recorder* recorder,
                  const paint_detail::BorderMesh& mesh, float scale, int w, int h, const std::vector<std::uint8_t>& cpu,
                  const std::vector<std::uint8_t>& rest, BorderMeshCheck& out) {
    if (w <= 0 || h <= 0) return -1;
    // 화면처럼 칸을 표면 한가운데 정수 자리에 놓는다. (0,0) 에만 놓으면 자리 계산 실수를 못 잡는다.
    constexpr int kAtX = 37;
    constexpr int kAtY = 53;
    const SkImageInfo info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    const SkImageInfo whole = SkImageInfo::Make(w + kAtX * 2, h + kAtY * 2, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    sk_sp<SkSurface> surface = SkSurfaces::RenderTarget(recorder, whole);
    if (!surface) return -1;
    SkCanvas* canvas = surface->getCanvas();
    canvas->clear(SK_ColorTRANSPARENT);
    const SkRect dst = SkRect::MakeXYWH(kAtX, kAtY, static_cast<float>(w), static_cast<float>(h));
    paint_detail::drawBorderMesh(canvas, mesh, scale, 1.f, 1.f, SkPoint::Make(kAtX, kAtY), dst);
    if (!rest.empty()) {
        const SkPixmap pixmap(info, rest.data(), static_cast<std::size_t>(w) * 4u);
        sk_sp<SkImage> image = SkImages::TextureFromImage(recorder, SkImages::RasterFromPixmapCopy(pixmap).get());
        if (image) canvas->drawImage(image.get(), kAtX, kAtY);
    }
    if (!submit(context, recorder)) return -1;
    ReadBox box;
    // 칸 밖까지 읽는다. 셰이더가 clip 을 넘어 칠하면 그 픽셀도 틀린 것이다.
    context->asyncRescaleAndReadPixels(surface.get(), whole, SkIRect::MakeWH(whole.width(), whole.height()),
                                       SkImage::RescaleGamma::kSrc, SkImage::RescaleMode::kNearest, onRead, &box);
    context->submit(skgpu::graphite::SyncToCpu::kYes);
    for (int spin = 0; !box.result && spin < 1000; ++spin) context->checkAsyncWorkCompletion();
    if (!box.result) return -1;
    const auto* got = static_cast<const std::uint8_t*>(box.result->data(0));
    const std::size_t rowBytes = box.result->rowBytes(0);
    long miss = 0;
    for (int y = 0; y < whole.height(); ++y) {
        for (int x = 0; x < whole.width(); ++x) {
            if (x < kAtX || y < kAtY || x >= kAtX + w || y >= kAtY + h) {
                if (got[static_cast<std::size_t>(y) * rowBytes + static_cast<std::size_t>(x) * 4u + 3] != 0) ++miss;
            }
        }
    }
    got += static_cast<std::size_t>(kAtY) * rowBytes + static_cast<std::size_t>(kAtX) * 4u;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * w + x) * 4u;
            const std::uint8_t* g = got + static_cast<std::size_t>(y) * rowBytes + static_cast<std::size_t>(x) * 4u;
            std::uint8_t want[4] = {0, 0, 0, 0};
            if (at + 3 < cpu.size()) std::copy(cpu.begin() + at, cpu.begin() + at + 4, want);
            if (want[0] == g[0] && want[1] == g[1] && want[2] == g[2] && want[3] == g[3]) continue;
            // 대각선은 안티앨리어싱으로 테두리 위에 얹힌다. 섞는 순서가 달라 1~2 단계 다를 수 있다.
            const bool soft = at + 3 < rest.size() && rest[at + 3] > 0 && rest[at + 3] < 255;
            bool near = true;
            for (int c = 0; c < 4; ++c) near = near && std::abs(int(want[c]) - int(g[c])) <= 2;
            if (soft && near) {
                ++out.diagonalPixels;
                continue;
            }
            ++miss;
        }
    }
    out.mismatches += miss;
    return miss;
}

// 한 칸을 CPU 와 GPU 로 그려 비교한다. 테두리를 적지 않은 칸이면 거짓이다.
bool checkFrame(skgpu::graphite::Context* context, skgpu::graphite::Recorder* recorder, const model::Document& doc,
                const Node& page, FrameDesc frame, BorderMeshCheck& out) {
    forceBordersOnScreen(false);
    const PageBitmap cpu = renderFrame(doc, page, frame);
    forceBordersOnScreen(true);
    const PageBitmap rest = renderFrame(doc, page, frame);
    forceBordersOnScreen(false);
    const auto borders = frameBorders(frame.id);
    paint_detail::dropFrameBorders(frame.id);
    if (!borders || borders->pieces.empty()) return false;
    const long miss =
        compareOnGpu(context, recorder, borders->mesh, borders->scale, rest.width, rest.height, cpu.rgba, rest.rgba, out);
    if (miss < 0) return false;
    ++out.frames;
    out.pieces += static_cast<long>(borders->pieces.size());
    if (miss > 0) {
        char line[200];
        std::snprintf(line, sizeof(line), "    page %d kind %d %dx%d @%d%%: %ld px differ\n", frame.page, frame.kind,
                      rest.width, rest.height, frame.percent, miss);
        out.report += line;
    }
    return true;
}

// 선 종류 11가지 × 굵기 10가지를 가로·세로로 긋고, 끝마다 모서리 잇기를 붙인다.
// 시작점과 길이를 조금씩 비틀어 반올림 경계를 건드린다.
void checkAllLineKinds(skgpu::graphite::Context* context, skgpu::graphite::Recorder* recorder,
                       const std::vector<int>& percents, BorderMeshCheck& out) {
    static const char* kTypes[] = {"SOLID",     "DASH",        "DOT",        "DASH_DOT",   "DASH_DOT_DOT",
                                   "LONG_DASH", "CIRCLE",      "DOUBLE_SLIM", "SLIM_THICK", "THICK_SLIM",
                                   "SLIM_THICK_SLIM"};
    static const int kWidths[] = {28, 34, 57, 85, 113, 142, 198, 283, 397, 567};
    constexpr int kRowHu = 1000;
    constexpr int kLengthHu = 20000;
    constexpr int kWidthCount = static_cast<int>(std::size(kWidths));
    const int count = static_cast<int>(std::size(kTypes)) * kWidthCount;
    for (int percent : percents) {
        const float scale = 96.f / 7200.f * static_cast<float>(percent) / 100.f;
        for (bool vertical : {false, true}) {
            const int along = static_cast<int>((1200 + kLengthHu + 1500) * scale) + 2;
            const int across = static_cast<int>((1500 + count * kRowHu + 1500) * scale) + 2;
            const int w = vertical ? across : along;
            const int h = vertical ? along : across;
            paint_detail::Painter cpu;
            paint_detail::Painter cap;
            std::vector<paint_detail::BorderPiece> pieces;
            for (paint_detail::Painter* painter : {&cpu, &cap}) {
                painter->scale = scale;
                painter->bitmap.width = w;
                painter->bitmap.height = h;
                painter->bitmap.clipR = w;
                painter->bitmap.clipB = h;
            }
            cap.borderSink = &pieces;
            for (int i = 0; i < count; ++i) {
                BorderEdge edge;
                edge.type = kTypes[i / kWidthCount];
                edge.width = kWidths[i % kWidthCount];
                edge.color = "#203040";
                const std::int32_t a0 = 1003 + (i * 37) % 113;
                const std::int32_t a1 = a0 + kLengthHu - (i * 53) % 211;
                const std::int32_t c = 1500 + i * kRowHu + (i * 29) % 75;
                const int inside = i % 2 == 0 ? 1 : -1;
                for (paint_detail::Painter* painter : {&cpu, &cap}) {
                    if (vertical) {
                        painter->strokeTableBorder(edge, c, a0, c, a1, inside);
                        painter->mitreTableCorner(c, a1, -inside, inside, edge);
                    } else {
                        painter->strokeTableBorder(edge, a0, c, a1, c, inside);
                        painter->mitreTableCorner(a1, c, inside, -inside, edge);
                    }
                }
            }
            const paint_detail::BorderMesh mesh = paint_detail::makeBorderMesh(pieces);
            const long miss = compareOnGpu(context, recorder, mesh, scale, w, h, cpu.bitmap.px, cap.bitmap.px, out);
            ++out.frames;
            out.pieces += static_cast<long>(pieces.size());
            if (miss != 0) {
                char line[200];
                std::snprintf(line, sizeof(line), "    all kinds %s @%d%%: %ld px differ\n", vertical ? "V" : "H",
                              percent, miss);
                out.report += line;
            }
        }
    }
}

}  // namespace

BorderMeshCheck checkBorderMeshDawn(const model::Document& doc, const std::vector<Node>& pages, int pageLimit,
                                    const std::vector<int>& percents) {
    BorderMeshCheck out;
    const char* specError = nullptr;
    if (!paint_detail::borderMeshSpec(&specError)) {
        out.report = std::string("spec failed: ") + (specError ? specError : "");
        return out;
    }
    DawnProcTable procs = dawn::native::GetProcs();
    dawnProcSetProcs(&procs);
    wgpu::InstanceDescriptor desc{};
    static const auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
    desc.requiredFeatureCount = 1;
    desc.requiredFeatures = &kTimedWaitAny;
    dawn::native::Instance instance(&desc);
    wgpu::RequestAdapterOptions options;
    std::vector<dawn::native::Adapter> adapters = instance.EnumerateAdapters(&options);
    int nextId = 1 << 24;
    for (dawn::native::Adapter& native : adapters) {
        wgpu::Adapter adapter = native.Get();
        wgpu::AdapterInfo info;
        adapter.GetInfo(&info);
        if (info.backendType == wgpu::BackendType::Null) continue;
        wgpu::DeviceDescriptor deviceDesc;
        deviceDesc.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
            std::fprintf(stderr, "[border-check] device error: %.*s\n", static_cast<int>(message.length),
                         message.data);
        });
        wgpu::Device device = adapter.CreateDevice(&deviceDesc);
        if (!device) continue;
        skgpu::graphite::DawnBackendContext backend;
        backend.fInstance = wgpu::Instance(instance.Get());
        backend.fDevice = device;
        backend.fQueue = device.GetQueue();
        std::unique_ptr<skgpu::graphite::Context> context = skgpu::graphite::ContextFactory::MakeDawn(backend, {});
        if (!context) continue;
        std::unique_ptr<skgpu::graphite::Recorder> recorder = context->makeRecorder();
        BorderMeshCheck one;
        checkAllLineKinds(context.get(), recorder.get(), {50, 75, 100, 125, 150, 200, 350}, one);
        int checkedPages = 0;
        for (int p = 0; p < static_cast<int>(pages.size()) && checkedPages < pageLimit; ++p) {
            bool any = false;
            for (int percent : percents) {
                const int pixelHeight = pagePixelHeight(pages[p].height, percent);
                for (FrameDesc frame : collectFrames(pages[p], p, pixelHeight, {}, &doc)) {
                    const bool stroke = (frame.kind >= 3 && frame.kind < 100) || frame.kind >= 700;
                    if (!stroke) continue;
                    frame.id = ++nextId;
                    frame.percent = percent;
                    if (checkFrame(context.get(), recorder.get(), doc, pages[p], frame, one)) any = true;
                }
                if (!any) break;
            }
            if (any) ++checkedPages;
        }
        char head[256];
        std::snprintf(head, sizeof(head), "%s %.*s%s: frames=%d pieces=%ld differ=%ld diagonalBlend=%ld\n",
                      backendName(info.backendType), static_cast<int>(info.device.length), info.device.data,
                      info.adapterType == wgpu::AdapterType::CPU ? " (cpu)" : "", one.frames, one.pieces,
                      one.mismatches, one.diagonalPixels);
        out.report += head + one.report;
        out.ran = true;
        out.frames += one.frames;
        out.pieces += one.pieces;
        out.mismatches += one.mismatches;
        out.diagonalPixels += one.diagonalPixels;
    }
    if (!out.ran && out.report.empty()) out.report = "no adapters";
    return out;
}
#else
BorderMeshCheck checkBorderMeshDawn(const model::Document&, const std::vector<Node>&, int, const std::vector<int>&) {
    BorderMeshCheck out;
    out.report = "no dawn";
    return out;
}
#endif

}  // namespace noting::render
