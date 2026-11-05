#include "chwp/render/screen_ops.hpp"

#include <algorithm>
#include <utility>

namespace chwp::render {

void unpremultiplyRgba(const std::uint8_t* src, std::size_t rowBytes, int width, int height, std::uint8_t* dst) {
    if (!src || !dst || width <= 0 || height <= 0) return;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* in = src + static_cast<std::size_t>(y) * rowBytes;
        std::uint8_t* out = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
        for (int x = 0; x < width; ++x, in += 4, out += 4) {
            const unsigned a = in[3];
            if (a == 255 || a == 0) {
                out[0] = a ? in[0] : 0;
                out[1] = a ? in[1] : 0;
                out[2] = a ? in[2] : 0;
                out[3] = static_cast<std::uint8_t>(a);
                continue;
            }
            out[0] = static_cast<std::uint8_t>(std::min(255u, (in[0] * 255u + a / 2) / a));
            out[1] = static_cast<std::uint8_t>(std::min(255u, (in[1] * 255u + a / 2) / a));
            out[2] = static_cast<std::uint8_t>(std::min(255u, (in[2] * 255u + a / 2) / a));
            out[3] = static_cast<std::uint8_t>(a);
        }
    }
}

ScreenBitmaps::ScreenBitmaps() { registerCachePool(pool_, kScreenBitmapBudget); }

ScreenBitmaps::~ScreenBitmaps() { unregisterCachePool(pool_); }

int ScreenBitmaps::use(std::uint64_t source, int stamp, int width, int height, const std::uint8_t* premulRgba,
                       std::size_t rowBytes) {
    if (width <= 0 || height <= 0 || !premulRgba) return 0;
    auto found = entries_.find(source);
    if (found != entries_.end() && found->second.stamp != stamp) {
        dropEntry(found);
        found = entries_.end();
    }
    if (found != entries_.end()) {
        found->second.lastFrame = cacheFrame();
        return found->second.key;
    }
    Entry entry;
    entry.key = nextKey_++;
    entry.stamp = stamp;
    entry.bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    entry.lastFrame = cacheFrame();
    ScreenUpload upload;
    upload.key = entry.key;
    upload.width = width;
    upload.height = height;
    upload.rgba.resize(entry.bytes);
    unpremultiplyRgba(premulRgba, rowBytes, width, height, upload.rgba.data());
    uploads_.push_back(std::move(upload));
    bytes_ += entry.bytes;
    entries_.emplace(source, entry);
    return entry.key;
}

void ScreenBitmaps::dropEntry(std::unordered_map<std::uint64_t, Entry>::iterator it) {
    const int key = it->second.key;
    bytes_ -= std::min(bytes_, it->second.bytes);
    entries_.erase(it);
    // 아직 셸에 안 간 그림이면 올리지 않는다.
    const auto pending = std::find_if(uploads_.begin(), uploads_.end(), [&](const ScreenUpload& u) { return u.key == key; });
    if (pending != uploads_.end()) {
        uploads_.erase(pending);
        return;
    }
    drops_.push_back(key);
}

void ScreenBitmaps::forget(std::uint64_t source) {
    const auto found = entries_.find(source);
    if (found != entries_.end()) dropEntry(found);
}

void ScreenBitmaps::clear() {
    while (!entries_.empty()) dropEntry(entries_.begin());
}

std::size_t ScreenBitmaps::release(CacheTier tier, std::size_t need) {
    (void)tier;
    const std::uint64_t now = cacheFrame();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> idle;
    for (const auto& [source, entry] : entries_) {
        if (entry.lastFrame != now) idle.emplace_back(entry.lastFrame, source);
    }
    std::sort(idle.begin(), idle.end());
    std::size_t freed = 0;
    for (const auto& [lastFrame, source] : idle) {
        if (freed >= need) break;
        const auto found = entries_.find(source);
        if (found == entries_.end()) continue;
        freed += found->second.bytes;
        dropEntry(found);
    }
    return freed;
}

void ScreenBitmaps::finish(ScreenFrame2D& frame) {
    enforceCacheBudget(pool_);
    frame.uploads = std::move(uploads_);
    frame.drops = std::move(drops_);
    uploads_.clear();
    drops_.clear();
}

}  // namespace chwp::render
