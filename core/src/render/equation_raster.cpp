#include "render/equation_raster.hpp"

#include "render/equation_latex.hpp"
#include "render/microtex_draw.hpp"

#include "latex.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#if NOTING_HAS_SKIA
#include "include/core/SkCanvas.h"
#include "include/core/SkRect.h"
#endif

#ifndef NOTING_MICROTEX_RES
#define NOTING_MICROTEX_RES "res"
#endif

namespace noting::render {
namespace {

std::wstring widen(const std::string& text) {
    std::wstring out;
    out.reserve(text.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t i = 0;
    while (i < text.size()) {
        char32_t cp = bytes[i];
        if (cp < 0x80) {
            ++i;
        } else if ((cp & 0xE0) == 0xC0 && i + 1 < text.size()) {
            cp = ((cp & 0x1F) << 6) | (bytes[i + 1] & 0x3F);
            i += 2;
        } else if ((cp & 0xF0) == 0xE0 && i + 2 < text.size()) {
            cp = ((cp & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
            i += 3;
        } else if ((cp & 0xF8) == 0xF0 && i + 3 < text.size()) {
            cp = ((cp & 0x07) << 18) | ((bytes[i + 1] & 0x3F) << 12) | ((bytes[i + 2] & 0x3F) << 6) | (bytes[i + 3] & 0x3F);
            i += 4;
        } else {
            ++i;
            continue;
        }
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0x10000) {
                cp -= 0x10000;
                out.push_back(static_cast<wchar_t>(0xD800 + ((cp >> 10) & 0x3FF)));
                out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            } else if (cp < 0xD800 || cp > 0xDFFF) {
                out.push_back(static_cast<wchar_t>(cp));
            }
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

struct Engine {
    bool ready = false;
    bool failed = false;

    bool ensure() {
        if (ready) return true;
        if (failed) return false;
        try {
            tex::LaTeX::init(NOTING_MICROTEX_RES);
            ready = true;
            return true;
        } catch (const std::exception&) {
            failed = true;
            return false;
        } catch (...) {
            failed = true;
            return false;
        }
    }
};

Engine& engine() {
    static Engine value;
    return value;
}

// microtex 는 전역 파서와 글꼴을 쓴다. 조판 스레드가 동시에 들어가면 free 가 힙 밖을 읽는다.
std::mutex& microtexMu() {
    static std::mutex mu;
    return mu;
}

bool hasInk(const EquationRaster& image) {
    for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
        if (image.rgba[i] != 0) return true;
    }
    return false;
}

std::unordered_map<std::string, EquationRaster>& cache() {
    static std::unordered_map<std::string, EquationRaster> images;
    return images;
}

std::unordered_map<std::string, std::unique_ptr<tex::TeXRender>>& renders() {
    static std::unordered_map<std::string, std::unique_ptr<tex::TeXRender>> items;
    return items;
}

tex::TeXRender* renderFor(const std::string& latex) {
    if (const auto it = renders().find(latex); it != renders().end()) return it->second.get();
    if (!engine().ensure()) return nullptr;
    tex::TeXRender* render = nullptr;
    try {
        render = tex::LaTeX::parse(widen(latex), 100000, 10.f, 10.f / 3.f, tex::black);
    } catch (const std::exception&) {
        render = nullptr;
    } catch (...) {
        render = nullptr;
    }
    if (!render) return nullptr;
    if (renders().size() > 128) renders().clear();
    const auto [it, inserted] = renders().emplace(latex, std::unique_ptr<tex::TeXRender>(render));
    (void)inserted;
    return it->second.get();
}

struct Fitted {
    tex::TeXRender* render = nullptr;
    float scale = 1.f;
    float padX = 0.f;
    float padY = 0.f;
};

// getWidth/getHeight 는 글꼴 메트릭이다. 숫자 5의 깊이는 0이지만 윤곽은 기준선 아래로 나간다.
// 그 비율만큼 더 줄여야 블록 안에 남는다.
struct Spill {
    float left = 0.f;
    float top = 0.f;
    float right = 0.f;
    float bottom = 0.f;
};

std::unordered_map<std::string, Spill>& spills() {
    static std::unordered_map<std::string, Spill> items;
    return items;
}

Spill measureSpill(tex::TeXRender& render) {
    constexpr float kSample = 48.f;
    constexpr int kPad = 24;
    render.setTextSize(kSample);
    const int metricW = std::max(1, render.getWidth());
    const int metricH = std::max(1, render.getHeight());
    if (metricW + kPad * 2 > 1024 || metricH + kPad * 2 > 1024) return {};
    const int width = metricW + kPad * 2;
    const int height = metricH + kPad * 2;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0);
    drawMicrotex(render, pixels.data(), width, height, static_cast<float>(kPad), static_cast<float>(kPad), 1.f);
    int minX = width;
    int minY = height;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (pixels[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 4u + 3] == 0)
                continue;
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < 0) return {};
    Spill spill;
    const float originX = static_cast<float>(kPad);
    const float originY = static_cast<float>(kPad);
    spill.left = std::max(0.f, originX - static_cast<float>(minX)) / static_cast<float>(metricW);
    spill.top = std::max(0.f, originY - static_cast<float>(minY)) / static_cast<float>(metricH);
    spill.right = std::max(0.f, static_cast<float>(maxX + 1) - originX - static_cast<float>(metricW)) / static_cast<float>(metricW);
    spill.bottom = std::max(0.f, static_cast<float>(maxY + 1) - originY - static_cast<float>(metricH)) / static_cast<float>(metricH);
    return spill;
}

Spill spillFor(const std::string& latex, tex::TeXRender& render) {
    if (const auto it = spills().find(latex); it != spills().end()) return it->second;
    const Spill spill = measureSpill(render);
    if (spills().size() > 128) spills().clear();
    spills()[latex] = spill;
    return spill;
}

// 글자 크기를 상자에 가깝게 올린 뒤, 윤곽이 넘친 비율까지 넣어 줄인다.
// 기준선은 남는 세로 여백 안에서만 파일 퍼센트에 맞춘다. 맞추려고 배율을 더 줄이지 않는다.
Fitted fitEquation(const std::string& script, int targetWidth, int targetHeight, int baselinePercent) {
    Fitted fit;
    if (targetWidth <= 0 || targetHeight <= 0) return fit;
    const std::string latex = hangulEquationToLatex(script);
    if (latex.empty()) return fit;
    tex::TeXRender* render = renderFor(latex);
    if (!render) return fit;
    render->setTextSize(10.f);
    const int naturalH = std::max(1, render->getHeight());
    const int naturalW = std::max(1, render->getWidth());
    float size = 10.f * static_cast<float>(targetHeight) / static_cast<float>(naturalH);
    const float widthAt = static_cast<float>(naturalW) * (size / 10.f);
    if (widthAt > static_cast<float>(targetWidth)) size *= static_cast<float>(targetWidth) / widthAt;
    size = std::clamp(size, 4.f, 128.f);
    const Spill spill = spillFor(latex, *render);
    render->setTextSize(size);
    const float renderW = static_cast<float>(std::max(1, render->getWidth()));
    const float renderH = static_cast<float>(std::max(1, render->getHeight()));
    const float inkW = renderW * (1.f + spill.left + spill.right);
    const float inkH = renderH * (1.f + spill.top + spill.bottom);
    const float baseline = render->getBaseline();
    const bool align = baselinePercent > 0 && baselinePercent <= 100 && baseline > 0.f && baseline < 1.f;
    const float scale = std::min(static_cast<float>(targetWidth) / inkW, static_cast<float>(targetHeight) / inkH);
    if (!(scale > 0.f)) return fit;
    const float drawnW = inkW * scale;
    fit.render = render;
    fit.scale = scale;
    fit.padX = (static_cast<float>(targetWidth) - drawnW) * 0.5f + spill.left * renderW * scale;
    const float drawnH = inkH * scale;
    const float centered = (static_cast<float>(targetHeight) - drawnH) * 0.5f + spill.top * renderH * scale;
    if (!align) {
        fit.padY = centered;
    } else {
        const float anchor = static_cast<float>(targetHeight) * (static_cast<float>(baselinePercent) / 100.f);
        const float minPad = spill.top * renderH * scale;
        const float maxPad = static_cast<float>(targetHeight) - renderH * scale * (1.f + spill.bottom);
        const float ideal = anchor - baseline * renderH * scale;
        fit.padY = maxPad >= minPad ? std::clamp(ideal, minPad, maxPad) : centered;
    }
    return fit;
}

}  // namespace

void clearEquationRasterCache() {
    std::lock_guard lock(microtexMu());
    cache().clear();
    renders().clear();
    spills().clear();
}

EquationRaster rasterHangulEquation(const std::string& script, int targetWidth, int targetHeight,
                                    int baselinePercent) {
    std::lock_guard lock(microtexMu());
    const std::string latex = hangulEquationToLatex(script);
    if (latex.empty()) return {};
    const std::string key = latex + "\n" + std::to_string(targetWidth) + "x" + std::to_string(targetHeight) + "@" +
                            std::to_string(baselinePercent);
    if (const auto it = cache().find(key); it != cache().end()) return it->second;

    EquationRaster image;
    const Fitted fit = fitEquation(script, targetWidth, targetHeight, baselinePercent);
    if (fit.render && targetWidth <= 4096 && targetHeight <= 4096) {
        image.width = targetWidth;
        image.height = targetHeight;
        image.rgba.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 4u, 0);
        drawMicrotex(*fit.render, image.rgba.data(), image.width, image.height, fit.padX, fit.padY, fit.scale);
        if (!hasInk(image)) image = {};
    }
    if (cache().size() > 128) cache().clear();
    cache()[key] = image;
    return image;
}

#if NOTING_HAS_SKIA
bool paintHangulEquation(SkCanvas& canvas, const std::string& script, int x, int y, int w, int h,
                         int baselinePercent) {
    std::lock_guard lock(microtexMu());
    if (w <= 0 || h <= 0) return false;
    const Fitted fit = fitEquation(script, w, h, baselinePercent);
    if (!fit.render) return false;
    canvas.save();
    canvas.clipRect(SkRect::MakeXYWH(static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                                     static_cast<float>(h)));
    drawMicrotex(*fit.render, canvas, static_cast<float>(x) + fit.padX, static_cast<float>(y) + fit.padY, fit.scale);
    canvas.restore();
    return true;
}
#endif

}  // namespace noting::render
