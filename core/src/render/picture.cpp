#include "render/picture.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "chwp/reader.hpp"
#include "chwp/render/cache_budget.hpp"
#include "chwp/render/paint.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#include "include/core/SkColorFilter.h"
#include "include/core/SkPixmap.h"
#include "include/effects/SkColorMatrix.h"

#include "src/core/SkMipmap.h"
#include "src/image/SkImage_Base.h"

#if CHWP_HAS_GPU
#include "include/core/SkCanvas.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/gpu/graphite/Image.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Surface.h"
#include "src/core/SkMipmapBuilder.h"
#endif

namespace chwp::render {

using namespace paint_detail;

namespace {

// ---------------------------------------------------------------------------
// 풀린 그림

std::map<std::string, std::shared_ptr<CachedPicture>> gPictures;
ByteView gPictureFile;
std::mutex gPictureMu;
std::size_t gPictureBytes = 0;  // gPictureMu 로 지킨다

std::string pictureLogId(const std::string& id) {
    std::string out;
    out.push_back('"');
    for (unsigned char c : id) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c >= 0x20) {
            out.push_back(static_cast<char>(c));
        }
    }
    out.push_back('"');
    return out;
}

std::shared_ptr<const CachedPicture> pictureHit(const std::shared_ptr<CachedPicture>& picture) {
    picture->lastFrame.store(cacheFrame(), std::memory_order_relaxed);
    return picture;
}

// 풀어 둔 그림. 지금은 예산이 없고, 셸이 메모리 신호를 주면 오래 안 쓴 것부터 버린다.
// 화면에 보이는 것은 구워 둔 칸이라 그림 원본은 칸을 다시 구울 때만 필요하다.
// 그래서 그리는 스레드가 지금 쥐고 있는 그림만 Visible 로 남기고 나머지는 Recent 다.
// 버린 그림은 다음에 쓸 때 파일에서 다시 푼다.
class PicturePool final : public CachePool {
public:
    const char* name() const override { return "pictures"; }
    std::size_t bytes() const override {
        std::lock_guard lock(gPictureMu);
        return gPictureBytes;
    }
    std::size_t release(CacheTier tier, std::size_t need) override {
        std::vector<std::pair<std::uint64_t, std::string>> order;
        std::lock_guard lock(gPictureMu);
        for (const auto& [id, picture] : gPictures) {
            if (!picture) continue;
            const CacheTier own = picture.use_count() > 1 ? CacheTier::Visible : CacheTier::Recent;
            if (own == tier) order.emplace_back(picture->lastFrame.load(std::memory_order_relaxed), id);
        }
        std::sort(order.begin(), order.end());
        std::size_t freed = 0;
        for (const auto& [last, id] : order) {
            if (freed >= need) break;
            const auto found = gPictures.find(id);
            if (found == gPictures.end()) continue;
            freed += found->second->bytes;
            gPictureBytes -= std::min(gPictureBytes, found->second->bytes);
            gPictures.erase(found);
        }
        return freed;
    }
};

PicturePool& picturePool() {
    static PicturePool pool;
    static const bool registered = (registerCachePool(pool, kNoCacheBudget), true);
    (void)registered;
    return pool;
}

const model::BinItem* binItemOf(const model::Document& doc, const std::string& id) {
    for (const auto& bin : doc.header.binItems) {
        if (bin.id == id) return &bin;
    }
    return nullptr;
}

}  // namespace

namespace paint_detail {

bool imageInfo(const std::uint8_t* bytes, std::size_t size, int& width, int& height, std::string& extension) {
    if (!bytes || size < 8 || size > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    if (bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') extension = "png";
    else if (bytes[0] == 0xFF && bytes[1] == 0xD8) extension = "jpg";
    else if (bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F') extension = "gif";
    else if (bytes[0] == 'B' && bytes[1] == 'M') extension = "bmp";
    else return false;
    int channels = 0;
    if (!stbi_info_from_memory(bytes, static_cast<int>(size), &width, &height, &channels)) return false;
    return width > 0 && height > 0;
}

}  // namespace paint_detail

namespace {

// 풀린 rgba 에 화면용 래스터 이미지와 밉맵 단계를 붙인다.
void finishPicture(CachedPicture& cached, bool opaque) {
    const int w = cached.width;
    const int h = cached.height;
    const SkImageInfo info =
        SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, opaque ? kOpaque_SkAlphaType : kUnpremul_SkAlphaType);
    const SkPixmap pixmap(info, cached.rgba.data(), static_cast<std::size_t>(w) * 4u);
    if (sk_sp<SkImage> base = SkImages::RasterFromPixmap(
            pixmap, [](const void*, void*) {}, nullptr)) {
        sk_sp<SkImage> mipped = base->withDefaultMipmaps();
        if (mipped) cached.bytes = cached.rgba.size() / 3u;
        cached.skImage = mipped ? std::move(mipped) : std::move(base);
    }
    cached.levels.push_back(pixmap);
    if (const sk_sp<SkMipmap> mips = cached.skImage ? as_IB(cached.skImage)->refMips() : nullptr) {
        for (int i = 0; i < mips->countLevels(); ++i) {
            SkMipmap::Level level;
            if (!mips->getLevel(i, &level)) break;
            cached.levels.push_back(level.fPixmap);
        }
    }
    if (cached.skImage) {
        for (const SkPixmap& level : cached.levels) {
            cached.levelSizes.push_back(PictureLevel{level.width(), level.height(), level.computeByteSize()});
        }
    }
    cached.bytes += cached.rgba.size();
}

// 원본 바이트를 RGBA 로 푼다. 실패하면 rgba 가 빈 그림이다.
std::shared_ptr<CachedPicture> decodePicture(const std::uint8_t* bytes, int byteCount) {
    auto cached = std::make_shared<CachedPicture>();
    if (!bytes || byteCount <= 0) return cached;
    int w = 0, h = 0, channels = 0;
    unsigned char* decoded = stbi_load_from_memory(bytes, byteCount, &w, &h, &channels, 4);
    if (!decoded) return cached;
    if (w <= 0 || h <= 0) {
        stbi_image_free(decoded);
        return cached;
    }
    cached->width = w;
    cached->height = h;
    cached->rgba.assign(decoded, decoded + static_cast<std::size_t>(w) * h * 4u);
    stbi_image_free(decoded);
    // 원본에 투명도가 없으면 불투명으로 둔다. 올릴 때 미리 곱한 색으로 바꾸는 일이 없다.
    finishPicture(*cached, channels == 1 || channels == 3);
    return cached;
}

}  // namespace

namespace paint_detail {

std::shared_ptr<CachedPicture> pictureFromRgba(int width, int height, std::vector<std::uint8_t> rgba, bool opaque) {
    auto cached = std::make_shared<CachedPicture>();
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<std::size_t>(width) * height * 4u) return cached;
    cached->width = width;
    cached->height = height;
    cached->rgba = std::move(rgba);
    finishPicture(*cached, opaque);
    return cached;
}

}  // namespace paint_detail

namespace {

// ---------------------------------------------------------------------------
// 화면 텍스처

// 판단(어느 단계를 언제 올릴지, 프레임 한도, 장부와 LRU)은 picture_upload 가 한다. 아래 #if 는 이미지와 표면만 든다.
void dropPictureImage(const std::string& id);
void dropFillSurface(const std::string& id);
void clearPictureGpu();

PictureTextureBook& textureBook() {
    static PictureTextureBook book([](const std::string& id) { dropPictureImage(id); });
    static const bool registered = (registerCachePool(book, kNoCacheBudget), true);
    (void)registered;
    return book;
}

// 아래 둘은 화면 스레드만 쓴다.
PictureUploadBudget gUploadBudget;
PictureFillQueue gFillQueue;

void startUploadFrame() { gUploadBudget.startFrame(cacheFrame()); }

// 칸마다 남긴 그림. 그리는 스레드가 넣고 화면 스레드가 읽는다.
std::unordered_map<int, std::shared_ptr<const std::vector<FramePicture>>> gFramePictures;
std::mutex gFramePictureMu;

#if CHWP_HAS_GPU

std::unordered_map<std::string, sk_sp<SkImage>> gImages;
std::mutex gImageMu;
// 나눠 쓰는 단계의 GPU 표면. 화면 스레드만 쓴다.
std::unordered_map<std::string, sk_sp<SkSurface>> gFillSurfaces;

void dropPictureImage(const std::string& id) {
    std::lock_guard lock(gImageMu);
    gImages.erase(id);
}

void dropFillSurface(const std::string& id) { gFillSurfaces.erase(id); }

void clearPictureGpu() {
    gFillSurfaces.clear();
    std::lock_guard lock(gImageMu);
    gImages.clear();
}

sk_sp<SkImage> imageOf(const std::string& id) {
    std::lock_guard lock(gImageMu);
    const auto found = gImages.find(id);
    return found == gImages.end() ? nullptr : found->second;
}

// level 단계를 원본처럼 쓰는 래스터 이미지. 그 아래 단계를 밉맵으로 붙인다. 픽셀은 그림이 쥔다.
sk_sp<SkImage> levelImage(const CachedPicture& picture, int level) {
    if (level == 0) return picture.skImage;
    const SkPixmap& base = picture.levels[static_cast<std::size_t>(level)];
    sk_sp<SkImage> image = SkImages::RasterFromPixmap(base, [](const void*, void*) {}, nullptr);
    if (!image) return nullptr;
    SkMipmapBuilder builder(base.info());
    if (builder.countLevels() != static_cast<int>(picture.levels.size()) - level - 1) return image;
    for (int i = 0; i < builder.countLevels(); ++i) {
        const SkPixmap& from = picture.levels[static_cast<std::size_t>(level + 1 + i)];
        if (!from.readPixels(builder.level(i))) return image;
    }
    sk_sp<SkImage> mipped = builder.attachTo(image);
    return mipped ? mipped : image;
}

// 장부가 받은(더 고운) 단계일 때만 바꿔 끼운다.
void keepTexture(const std::string& id, sk_sp<SkImage> image, int level, std::size_t bytes) {
    if (!textureBook().keep(id, level, bytes)) return;
    std::lock_guard lock(gImageMu);
    gImages[id] = std::move(image);
}

#else
void dropPictureImage(const std::string&) {}
void dropFillSurface(const std::string&) {}
void clearPictureGpu() {}
#endif

int pictureChannel(int c, const PictureTone& tone) {
    const int scale = tone.contrast + 100;
    int v = (c - 128) * scale / 100 + 128 + tone.bright * 255 / 100;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (tone.effect == 2) v = v >= 128 ? 255 : 0;
    return v;
}

}  // namespace

namespace paint_detail {

std::shared_ptr<const CachedPicture> pictureFor(const model::Document& doc, const std::string& id) {
    if (id.empty()) return nullptr;
    picturePool();
    ByteView file;
    {
        std::lock_guard lock(gPictureMu);
        if (const auto found = gPictures.find(id);
            found != gPictures.end() && found->second && !found->second->failed) {
            return pictureHit(found->second);
        }
        file = gPictureFile;
    }
    const model::BinItem* item = binItemOf(doc, id);
    const std::uint8_t* bytes = nullptr;
    int byteCount = 0;
    Bytes loaded;
    bool fromFile = false;
    const auto started = std::chrono::steady_clock::now();
    if (item && !item->data.empty()) {
        bytes = item->data.data();
        byteCount = static_cast<int>(item->data.size());
    } else if (item && !file.empty()) {
        fromFile = true;
        if (chwp::readEmbeddedBin(file, doc, *item, loaded) && !loaded.empty()) {
            bytes = loaded.data();
            byteCount = static_cast<int>(loaded.size());
        }
    }
    std::shared_ptr<CachedPicture> cached = decodePicture(bytes, byteCount);
    if (fromFile) {
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (cached->rgba.empty()) {
            std::printf("{\"그림\":%s,\"ms\":%.1f,\"결과\":\"없음\"}\n", pictureLogId(id).c_str(), ms);
        } else {
            std::printf("{\"그림\":%s,\"ms\":%.1f}\n", pictureLogId(id).c_str(), ms);
        }
        std::fflush(stdout);
    }
    // 한 번 못 읽은 그림을 실패로 남기면 그 칸이 회색으로 고정된다. 성공만 남긴다.
    std::lock_guard lock(gPictureMu);
    if (const auto found = gPictures.find(id); found != gPictures.end() && found->second && !found->second->failed) {
        return pictureHit(found->second);
    }
    if (cached->rgba.empty()) return nullptr;
    gPictures.insert_or_assign(id, cached);
    gPictureBytes += cached->bytes;
    return pictureHit(cached);
}

std::shared_ptr<const CachedPicture> peekPicture(const std::string& id) {
    std::lock_guard lock(gPictureMu);
    const auto found = gPictures.find(id);
    if (found == gPictures.end() || !found->second || found->second->failed) return nullptr;
    return found->second;
}

void clearPictures() {
    {
        std::lock_guard lock(gPictureMu);
        gPictures.clear();
        gPictureBytes = 0;
    }
    gFillQueue.clear();
    textureBook().clear();
    clearPictureGpu();
}

void keepPictureBudget() {
    // 그림은 그리는 스레드가 풀어 넣는다. 예산은 화면 스레드에서 지킨다.
    enforceCacheBudget(picturePool());
    enforceCacheBudget(textureBook());
    startUploadFrame();
}

#if CHWP_HAS_GPU
PictureTextureView pictureTexture(skgpu::graphite::Recorder* recorder, const std::string& id,
                                  const std::shared_ptr<const CachedPicture>& picture, float wantW, float wantH,
                                  const SkRect& src) {
    PictureTextureView view;
    if (!picture || !picture->skImage || picture->levels.empty() || picture->levelSizes.empty()) return view;
    if (!recorder) {
        // 2D 화면에는 기록기가 없다. 풀어 둔 래스터 이미지를 그대로 그린다.
        view.image = picture->skImage;
        return view;
    }
    startUploadFrame();
    const std::vector<PictureLevel>& levels = picture->levelSizes;
    const int want = wantedPictureLevel(levels, picture->width, picture->height, src.width(), src.height(), wantW, wantH);
    const auto viewOf = [&](int level) {
        PictureTextureView out;
        if (level < 0 || level >= static_cast<int>(levels.size())) return out;
        out.image = imageOf(id);
        if (!out.image) return out;
        const PictureLevel& size = levels[static_cast<std::size_t>(level)];
        out.scaleX = static_cast<float>(size.width) / static_cast<float>(picture->width);
        out.scaleY = static_cast<float>(size.height) / static_cast<float>(picture->height);
        out.exact = level <= want;
        return out;
    };
    const auto uploadNow = [&](int level) {
        const sk_sp<SkImage> source = levelImage(*picture, level);
        sk_sp<SkImage> image =
            source ? SkImages::TextureFromImage(recorder, source.get(), {/*fMipmapped=*/true}) : nullptr;
        if (!image) return;
        const std::size_t bytes = pictureLevelBytes(levels, level);
        gUploadBudget.spend(bytes);
        keepTexture(id, std::move(image), level, bytes);
    };
    int held = textureBook().use(id);
    if (held >= 0 && !imageOf(id)) held = -1;
    const PictureUploadPlan plan = planPictureUpload(levels, held, want, gUploadBudget.room());
    if (plan.fill >= 0 && gFillQueue.queue(id, picture, plan.fill)) dropFillSurface(id);
    if (plan.now >= 0) uploadNow(plan.now);
    return viewOf(textureBook().use(id));
}

void advancePictureUploads(skgpu::graphite::Recorder* recorder) {
    if (!recorder || gFillQueue.empty()) return;
    startUploadFrame();
    std::vector<PictureFill>& fills = gFillQueue.fills();
    for (auto it = fills.begin(); it != fills.end();) {
        if (gUploadBudget.spent()) break;
        PictureFill& fill = *it;
        const SkPixmap& level = fill.picture->levels[static_cast<std::size_t>(fill.level)];
        sk_sp<SkSurface>& surface = gFillSurfaces[fill.id];
        if (!surface) {
            const SkImageInfo info =
                SkImageInfo::Make(level.width(), level.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType);
            surface = SkSurfaces::RenderTarget(recorder, info, skgpu::Mipmapped::kNo);
            if (!surface) {
                gFillSurfaces.erase(fill.id);
                it = fills.erase(it);
                continue;
            }
        }
        const int from = fill.row;
        const int rows = PictureFillQueue::step(fill, level.rowBytes(), level.height(), gUploadBudget);
        SkPixmap band;
        if (rows > 0 && level.extractSubset(&band, SkIRect::MakeXYWH(0, from, level.width(), rows))) {
            surface->writePixels(band, 0, from);
        }
        if (fill.row < level.height()) {
            ++it;
            continue;
        }
        if (sk_sp<SkImage> image = SkSurfaces::AsImageCopy(surface, nullptr, skgpu::Mipmapped::kYes)) {
            keepTexture(fill.id, std::move(image), fill.level, pictureLevelBytes(fill.picture->levelSizes, fill.level));
        }
        gFillSurfaces.erase(fill.id);
        it = fills.erase(it);
    }
}
#endif

bool pictureUploadsWaiting() { return !gFillQueue.empty(); }

std::uint32_t pictureTextureSeq() { return textureBook().seq(); }

void storeFramePictures(int id, std::vector<FramePicture> pictures) {
    std::lock_guard lock(gFramePictureMu);
    if (id == 0 || pictures.empty()) {
        gFramePictures.erase(id);
        return;
    }
    gFramePictures[id] = std::make_shared<const std::vector<FramePicture>>(std::move(pictures));
}

void dropFramePictures(int id) {
    std::lock_guard lock(gFramePictureMu);
    gFramePictures.erase(id);
}

void clearFramePictures() {
    std::lock_guard lock(gFramePictureMu);
    gFramePictures.clear();
}

bool frameHasPictures(int id) {
    std::lock_guard lock(gFramePictureMu);
    return gFramePictures.count(id) > 0;
}

bool cloneFramePictures(int srcId, int dstId) {
    if (srcId == 0 || dstId == 0) return false;
    std::lock_guard lock(gFramePictureMu);
    const auto found = gFramePictures.find(srcId);
    if (found == gFramePictures.end()) return false;
    // 좌표는 칸 안이라 칸이 옮겨 가도 그대로다.
    if (srcId != dstId) gFramePictures[dstId] = found->second;
    return true;
}

std::shared_ptr<const std::vector<FramePicture>> framePictures(int id) {
    std::lock_guard lock(gFramePictureMu);
    const auto found = gFramePictures.find(id);
    return found == gFramePictures.end() ? nullptr : found->second;
}

#if CHWP_HAS_GPU
SkRect skRect(const PictureRect& rect) { return SkRect::MakeLTRB(rect.l, rect.t, rect.r, rect.b); }

bool drawFramePictures(SkCanvas* canvas, skgpu::graphite::Recorder* recorder,
                       const std::vector<FramePicture>& pictures, const SkRect& spriteDst, float sx, float sy,
                       bool dragging, const std::vector<SkRect>& backs, sk_sp<SkColorFilter>& inverted) {
    if (!canvas) return false;
    bool all = true;
    const auto onScreen = [&](const SkRect& local) {
        return SkRect::MakeLTRB(spriteDst.fLeft + local.fLeft * sx, spriteDst.fTop + local.fTop * sy,
                                spriteDst.fLeft + local.fRight * sx, spriteDst.fTop + local.fBottom * sy);
    };
    for (const FramePicture& item : pictures) {
        if (!item.picture || item.src.empty()) continue;
        const SkRect dst = onScreen(skRect(item.dst));
        SkRect clip = onScreen(skRect(item.clip));
        if (dst.isEmpty() || !clip.intersect(spriteDst) || !clip.intersect(dst)) continue;
        if (!clip.intersect(canvas->getLocalClipBounds())) continue;
        const PictureTextureView view =
            pictureTexture(recorder, item.id, item.picture, dst.width(), dst.height(), skRect(item.src));
        if (!view.image) {
            all = false;
            continue;
        }
        if (!view.exact) all = false;
        const sk_sp<SkImage>& image = view.image;
        const SkRect src = SkRect::MakeLTRB(item.src.l * view.scaleX, item.src.t * view.scaleY,
                                            item.src.r * view.scaleX, item.src.b * view.scaleY);
        const bool shrink = dst.width() < src.width() || dst.height() < src.height();
        const SkSamplingOptions sampling =
            shrink ? (image->hasMipmaps() ? SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)
                                          : SkSamplingOptions(SkFilterMode::kLinear))
                   : (dragging ? SkSamplingOptions(SkFilterMode::kLinear)
                               : SkSamplingOptions(SkCubicResampler::CatmullRom()));
        // 잘라 낸 그림은 바깥 픽셀이 번지지 않게 원본 범위 안에서만 집는다.
        const bool whole = item.src.l == 0.f && item.src.t == 0.f &&
                           item.src.r == static_cast<float>(item.picture->width) &&
                           item.src.b == static_cast<float>(item.picture->height);
        const SkCanvas::SrcRectConstraint constraint =
            whole ? SkCanvas::kFast_SrcRectConstraint : SkCanvas::kStrict_SrcRectConstraint;
        SkPaint paint;
        paint.setAntiAlias(false);
        paintPictureTone(paint, &item.tone);
        const auto drawIn = [&](const SkRect& area, const SkPaint& with) {
            canvas->save();
            canvas->clipRect(area);
            if (item.flipH || item.flipV) {
                canvas->translate(item.flipH ? dst.fRight : dst.fLeft, item.flipV ? dst.fBottom : dst.fTop);
                canvas->scale(item.flipH ? -1.f : 1.f, item.flipV ? -1.f : 1.f);
                canvas->drawImageRect(image.get(), src, SkRect::MakeWH(dst.width(), dst.height()), sampling,
                                      &with, constraint);
            } else {
                canvas->drawImageRect(image.get(), src, dst, sampling, &with, constraint);
            }
            canvas->restore();
        };
        drawIn(clip, paint);
        for (const SkRect& back : backs) {
            SkRect overlap = clip;
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
            SkPaint flipped = paint;
            flipped.setColorFilter(paint.getColorFilter() ? SkColorFilters::Compose(inverted, paint.refColorFilter())
                                                          : inverted);
            drawIn(overlap, flipped);
        }
    }
    return all;
}
#endif

PictureCrop pictureCrop(const model::PictureView& view) {
    PictureCrop crop;
    crop.originW = view.originWidth;
    crop.originH = view.originHeight;
    crop.clipL = view.clipLeft;
    crop.clipT = view.clipTop;
    crop.clipR = view.clipRight;
    crop.clipB = view.clipBottom;
    return crop;
}

bool cropSource(const PictureCrop& crop, int imageW, int imageH, PictureSource& out) {
    out = PictureSource{};
    out.srcR = imageW;
    out.srcB = imageH;
    const std::int32_t clipW = crop.clipR - crop.clipL;
    const std::int32_t clipH = crop.clipB - crop.clipT;
    const bool identity = crop.originW <= 0 || crop.originH <= 0 || clipW <= 0 || clipH <= 0 ||
                          (crop.clipL == 0 && crop.clipT == 0 && crop.clipR == crop.originW &&
                           crop.clipB == crop.originH);
    if (identity) return true;
    const double originW = crop.originW;
    const double originH = crop.originH;
    const double clipL = crop.clipL;
    const double clipT = crop.clipT;
    const double insideL = std::max(clipL, 0.0);
    const double insideT = std::max(clipT, 0.0);
    const double insideR = std::min<double>(crop.clipR, originW);
    const double insideB = std::min<double>(crop.clipB, originH);
    if (insideR <= insideL || insideB <= insideT) return false;
    out.fracL = (insideL - clipL) / clipW;
    out.fracT = (insideT - clipT) / clipH;
    out.fracR = (insideR - clipL) / clipW;
    out.fracB = (insideB - clipT) / clipH;
    out.srcL = insideL * imageW / originW;
    out.srcT = insideT * imageH / originH;
    out.srcR = insideR * imageW / originW;
    out.srcB = insideB * imageH / originH;
    return true;
}

PictureTone pictureTone(const model::PictureView& view) {
    PictureTone tone;
    tone.effect = view.effect;
    tone.bright = view.bright;
    tone.contrast = view.contrast;
    tone.transparency = view.transparency;
    return tone;
}

void applyPictureTone(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a, const PictureTone& tone) {
    if (tone.effect == 1 || tone.effect == 2) {
        const int y = (static_cast<int>(r) * 30 + static_cast<int>(g) * 59 + static_cast<int>(b) * 11) / 100;
        const auto v = static_cast<std::uint8_t>(pictureChannel(y, tone));
        r = g = b = v;
    } else if (tone.bright != 0 || tone.contrast != 0) {
        r = static_cast<std::uint8_t>(pictureChannel(r, tone));
        g = static_cast<std::uint8_t>(pictureChannel(g, tone));
        b = static_cast<std::uint8_t>(pictureChannel(b, tone));
    }
    if (tone.transparency > 0) {
        const int t = std::min(tone.transparency, 255);
        a = static_cast<std::uint8_t>(static_cast<int>(a) * (255 - t) / 255);
    }
}

void paintPictureTone(SkPaint& paint, const PictureTone* tone) {
    if (!tone || !tone->on()) return;
    sk_sp<SkColorFilter> filter;
    if (tone->effect == 1 || tone->effect == 2) {
        const SkColorMatrix luma(0.30f, 0.59f, 0.11f, 0, 0, 0.30f, 0.59f, 0.11f, 0, 0, 0.30f, 0.59f, 0.11f, 0, 0, 0,
                                 0, 0, 1, 0);
        filter = SkColorFilters::Matrix(luma);
    }
    const bool table = tone->bright != 0 || tone->contrast != 0 || tone->effect == 2 || tone->transparency > 0;
    if (!table) {
        paint.setColorFilter(std::move(filter));
        return;
    }
    std::uint8_t rgb[256];
    std::uint8_t alpha[256];
    const int keep = 255 - std::min(std::max(tone->transparency, 0), 255);
    for (int i = 0; i < 256; ++i) {
        rgb[i] = static_cast<std::uint8_t>(pictureChannel(i, *tone));
        alpha[i] = static_cast<std::uint8_t>(i * keep / 255);
    }
    sk_sp<SkColorFilter> mapped = SkColorFilters::TableARGB(alpha, rgb, rgb, rgb);
    paint.setColorFilter(filter ? SkColorFilters::Compose(std::move(mapped), std::move(filter)) : std::move(mapped));
}

SkRect pictureSrc(int sw, int sh, const ImageDraw* draw) {
    if (!draw || draw->srcR <= draw->srcL || draw->srcB <= draw->srcT) return SkRect::MakeWH(static_cast<float>(sw), static_cast<float>(sh));
    return SkRect::MakeLTRB(static_cast<float>(draw->srcL), static_cast<float>(draw->srcT), static_cast<float>(draw->srcR),
                            static_cast<float>(draw->srcB));
}

}  // namespace paint_detail

void setPictureFile(ByteView file) {
    std::lock_guard lock(gPictureMu);
    if (gPictureFile.data() != file.data() || gPictureFile.size() != file.size()) {
        chwp::releaseEmbeddedBinCache();
    }
    gPictureFile = file;
}

void warmPicture(const model::Document& doc, const std::string& id) { pictureFor(doc, id); }

bool pictureUploadsPending() { return pictureUploadsWaiting(); }

}  // namespace chwp::render
