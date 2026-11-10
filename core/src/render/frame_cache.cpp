#include "chwp/render/frame_cache.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <thread>

#include "export/layout.hpp"

namespace chwp::render {
namespace {

constexpr int kPrepareBudget = 8;

bool hits(int x, int y, int w, int h, int vx, int vy, int vw, int vh) {
    return w > 0 && h > 0 && vw > 0 && vh > 0 && x < vx + vw && x + w > vx && y < vy + vh && y + h > vy;
}

// 기울임은 글자 높이의 약 0.34배가 줄 상자 밖으로 나간다. 합성 굵기까지 포함해 판정 사각형을 이만큼 키운다.
constexpr std::int32_t kInkOutsetHu = 8192;

FrameCull pageCull(const ViewPage& page, int pageHuH, int pixelH, int scrollX, int scrollY, int viewW, int viewH,
                   int margin) {
    FrameCull cull;
    if (pixelH <= 0 || pageHuH <= 0 || viewW <= 0 || viewH <= 0) return cull;
    const int screenH = page.h > 0 ? page.h : pixelH;
    const double huPerScreenPx = static_cast<double>(pageHuH) / static_cast<double>(screenH);
    const double huPerTilePx = static_cast<double>(pageHuH) / static_cast<double>(pixelH);
    const double pad = std::ceil(static_cast<double>(kFramePad + 2) * huPerTilePx) + static_cast<double>(kInkOutsetHu);
    const double left = std::floor((static_cast<double>(scrollX - margin - page.x) * huPerScreenPx) - pad);
    const double top = std::floor((static_cast<double>(scrollY - margin - page.y) * huPerScreenPx) - pad);
    const double right = std::ceil((static_cast<double>(scrollX + viewW + margin - page.x) * huPerScreenPx) + pad);
    const double bottom = std::ceil((static_cast<double>(scrollY + viewH + margin - page.y) * huPerScreenPx) + pad);
    cull.on = true;
    cull.x = static_cast<std::int32_t>(left);
    cull.y = static_cast<std::int32_t>(top);
    cull.w = std::max<std::int32_t>(1, static_cast<std::int32_t>(right) - cull.x);
    cull.h = std::max<std::int32_t>(1, static_cast<std::int32_t>(bottom) - cull.y);
    return cull;
}

int outsideGap(int x, int y, int w, int h, int vx, int vy, int vw, int vh) {
    int gap = 0;
    if (y + h < vy) gap += vy - (y + h);
    else if (y > vy + vh) gap += y - (vy + vh);
    if (x + w < vx) gap += vx - (x + w);
    else if (x > vx + vw) gap += x - (vx + vw);
    return gap;
}

std::string frameKey(const FrameDesc& frame) {
    std::string key = std::to_string(frame.page) + "/" + std::to_string(frame.kind);
    for (const int index : frame.path) key += "." + std::to_string(index);
    return key;
}

int framePxFromHu(std::int32_t hu, float scale) {
    return static_cast<int>(hu * scale + (hu < 0 ? -0.5f : 0.5f));
}

constexpr std::int32_t kBleedGuardHu = 2048;

bool missesCull(const FrameDesc& base, const FrameCull& cull) {
    if (!cull.on || base.huW <= 0 || base.huH <= 0) return false;
    return base.huX - kBleedGuardHu >= cull.x + cull.w || base.huY - kBleedGuardHu >= cull.y + cull.h ||
           base.huX + base.huW + kBleedGuardHu <= cull.x || base.huY + base.huH + kBleedGuardHu <= cull.y;
}

std::uint64_t makeRecycleHashKey(const FrameDesc& desc, int percent) {
    if (desc.contentHash == 0) return 0;
    std::uint64_t h = desc.contentHash;
    h ^= static_cast<std::uint64_t>(desc.kind) * 0x9e3779b97f4a7c15ULL;
    h ^= static_cast<std::uint64_t>(percent > 0 ? percent : 100) * 0xbf58476d1ce4e5b9ULL;
    return h == 0 ? 1ULL : h;
}

FrameDesc scaleCachedFrame(const FrameDesc& base, int percent, int pixelHeight, float scale) {
    FrameDesc frame = base;
    frame.percent = percent;
    if (percent == 100 && pixelHeight == base.pixelHeight) return frame;
    const int left = framePxFromHu(base.huX, scale) - kFramePad;
    const int top = framePxFromHu(base.huY, scale) - kFramePad;
    const int right = framePxFromHu(base.huX + base.huW, scale) + kFramePad;
    const int bottom = framePxFromHu(base.huY + base.huH, scale) + kFramePad;
    frame.x = left;
    frame.y = top;
    frame.w = std::max(1, right - left);
    frame.h = std::max(1, bottom - top);
    frame.pixelHeight = pixelHeight;
    return frame;
}

std::size_t findFirstLineChildCount(const Node& node) {
    if (node.kind == NodeKind::Line) return node.children.size() + 1;
    for (const Node& child : node.children) {
        const std::size_t found = findFirstLineChildCount(child);
        if (found > 0) return found;
    }
    return 0;
}

}  // namespace

bool overviewZoom(int visiblePages, int percent) {
    return visiblePages >= kOverviewPages && percent > 0 && percent < 100;
}

std::vector<FrameDesc> framesOnScreen(const Node& scene, const ViewPage& page, int percent, int scrollX, int scrollY,
                                     int viewW, int viewH, const model::Document* doc) {
    std::vector<FrameDesc> out;
    if (page.index < 0 || scene.height <= 0 || viewW <= 0 || viewH <= 0) return out;
    const int zoom = percent > 0 ? percent : 100;
    const int pageMargin = page.h > 0 ? std::max(page.h * 2, 320) : viewH;
    const int margin = (zoom < 100 && page.h > 0) ? std::clamp(std::min(viewH, pageMargin), 1, std::max(viewH, 1))
                                                  : std::max(viewH, 1);
    if (!hits(page.x, page.y, page.w, page.h, scrollX - margin, scrollY - margin, viewW + margin * 2, viewH + margin * 2))
        return out;
    const int pixelH = pagePixelHeight(scene.height, zoom);
    if (pixelH < 32) return out;
    const FrameCull cull = pageCull(page, scene.height, pixelH, scrollX, scrollY, viewW, viewH, margin);
    std::vector<FrameDesc> frames = collectFrames(scene, page.index, pixelH, cull, doc);
    const int viewX = scrollX - margin;
    const int viewY = scrollY - margin;
    const int viewWidth = viewW + margin * 2;
    const int viewHeight = viewH + margin * 2;
    for (FrameDesc& frame : frames) {
        frame.percent = zoom;
        if (!hits(page.x + frame.x, page.y + frame.y, frame.w, frame.h, viewX, viewY, viewWidth, viewHeight)) continue;
        out.push_back(std::move(frame));
    }
    return out;
}

int maskZoomPercent(int percent, bool dragging) {
    (void)dragging;
    if (percent <= 0) percent = 100;
    percent = std::clamp(percent, 10, 800);
    if (percent <= 50) return 50;
    if (percent <= 100) return (percent + 9) / 10 * 10;
    const int band = std::clamp(percent / 100, 1, 7);
    const int base = band * 100;
    const int nextBase = std::min(800, base + 100);
    const int step = band * 10;
    const int steps = (percent - base + step - 1) / step;
    return std::min(nextBase, base + steps * step);
}

int pageCachePercent(int percent) {
    if (percent <= 0) percent = 100;
    if (percent <= 50) return 50;
    if (percent <= 100) return 100;
    return percent;
}

int restingMaskZoom(int percent) { return maskZoomPercent(pageCachePercent(percent), false); }

FrameCache::FrameCache() { registerCachePool(pool_, kTileCacheBudget); }

FrameCache::FrameCache(int firstId) : FrameCache() { nextId_ = firstId; }

FrameCache::~FrameCache() { unregisterCachePool(pool_); }

std::size_t FrameCache::bitmapBytes() const {
    std::size_t total = 0;
    for (const auto& [id, tile] : tiles_) total += tile.bitmap.rgba.size();
    for (const auto& [hash, bucket] : recycledByHash_) {
        for (const RecycledTile& recycled : bucket) total += recycled.tile.bitmap.rgba.size();
    }
    return total;
}

std::size_t FrameCache::catalogBytes() const {
    const auto descBytes = [](const FrameDesc& d) { return sizeof(FrameDesc) + d.path.capacity() * sizeof(int); };
    std::size_t total = 0;
    for (const auto& [id, desc] : catalog_) total += descBytes(desc) + 32;
    for (const auto& [id, tile] : tiles_) total += sizeof(Tile) + tile.key.capacity() + tile.desc.path.capacity() * sizeof(int) + 32;
    {
        std::lock_guard lock(idMu_);
        for (const auto& [key, id] : ids_) total += key.capacity() + sizeof(int) + 48;
    }
    std::lock_guard lock(pageFramesMu_);
    for (const auto& [page, frames] : pageFrames_) {
        if (!frames) continue;
        for (const CachedFrameEntry& entry : frames->entries) total += descBytes(entry.baseDesc) + entry.key.capacity() + 32;
        total += (frames->byYOrder.capacity() + frames->byHuYOrder.capacity()) * sizeof(std::size_t) +
                 frames->maxHuBottomPrefix.capacity() * sizeof(std::int32_t);
    }
    return total;
}

void FrameCache::setFocus(int firstPage, int lastPage) {
    focusFirst_ = firstPage;
    focusLast_ = std::max(firstPage, lastPage);
}

int FrameCache::focusGap(int page) const {
    if (focusFirst_ < 0) return 0;
    if (page < focusFirst_) return focusFirst_ - page;
    if (page > focusLast_) return page - focusLast_;
    return 0;
}

std::size_t FrameCache::evictPage(int page) {
    const auto found = pageTiles_.find(page);
    if (found == pageTiles_.end()) return 0;
    std::size_t freed = 0;
    {
        std::lock_guard lock(idMu_);
        for (const int id : found->second) {
            const auto tile = tiles_.find(id);
            if (tile == tiles_.end()) continue;
            freed += tile->second.bitmap.rgba.size();
            tiles_.erase(tile);
            tileIds_.erase(id);
            nonBaseTiles_.erase(id);
            dropScreenSprite(id);
        }
    }
    pageTiles_.erase(found);
    baseDone_.erase(page);
    pageStamps_[page] = ++stamp_;
    evictedPages_.push_back(page);
    return freed;
}

std::size_t FrameCache::releaseTiles(CacheTier tier, std::size_t need) {
    if (tier == CacheTier::Spare) {
        std::size_t freed = 0;
        for (const auto& [hash, bucket] : recycledByHash_) {
            for (const RecycledTile& recycled : bucket) freed += recycled.tile.bitmap.rgba.size();
        }
        flushRecycledTiles();
        return freed;
    }
    std::vector<std::pair<int, int>> far;
    for (const auto& [page, ids] : pageTiles_) {
        if (!ids.empty()) far.emplace_back(focusGap(page), page);
    }
    std::sort(far.begin(), far.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second > b.second;
    });
    // 보이는 쪽과 바로 옆 쪽은 스크롤하면 곧 들어오니 남긴다.
    constexpr int kMinKeepRadius = 1;
    std::size_t freed = 0;
    for (const auto& [gap, page] : far) {
        if (freed >= need || gap <= kMinKeepRadius) break;
        if (tier == CacheTier::Recent) {
            if (keepRadius_ < 0 || gap <= keepRadius_) break;
        } else {
            keepRadius_ = keepRadius_ < 0 ? gap - 1 : std::min(keepRadius_, gap - 1);
        }
        freed += evictPage(page);
    }
    return freed;
}

bool FrameCache::keepWithinBudget() {
    const int before = keepRadius_;
    enforceCacheBudget(pool_);
    const std::size_t budget = cacheBudget(pool_);
    // 편집·배율 바꿈으로 칸이 많이 비면 다시 끝까지 미리 굽는다. 넘으면 그때 또 줄인다.
    if (keepRadius_ >= 0 && keepRadius_ == before && (budget == kNoCacheBudget || bitmapBytes() < budget / 2)) {
        keepRadius_ = -1;
    }
    return keepRadius_ != before;
}

std::vector<int> FrameCache::takeEvictedPages() {
    std::vector<int> pages = std::move(evictedPages_);
    evictedPages_.clear();
    return pages;
}

void FrameCache::flushRecycledTiles() {
    for (const int oldId : recycledOldIds_) {
        if (!tiles_.count(oldId)) {
            dropScreenSprite(oldId);
            pendingDrop_.push_back(oldId);
        }
    }
    recycledOldIds_.clear();
    recycledByHash_.clear();
    recycledAge_ = 0;
}

void FrameCache::stashTileForRecycle(int id, Tile&& tile) {
    if (!usable(id, tile) || tile.desc.contentHash == 0 || recycledOldIds_.size() > 4096) {
        dropScreenSprite(id);
        pendingDrop_.push_back(id);
        return;
    }
    const std::uint64_t rKey = makeRecycleHashKey(tile.desc, tile.percent);
    if (rKey == 0) {
        dropScreenSprite(id);
        pendingDrop_.push_back(id);
        return;
    }
    recycledOldIds_.insert(id);
    recycledByHash_[rKey].push_back(RecycledTile{id, std::move(tile)});
    recycledAge_ = 0;
}

bool FrameCache::tryAdoptRecycledTile(const FrameDesc& frame, int page, int percent) {
    if (recycledByHash_.empty() || frame.contentHash == 0 || frame.id <= 0) return false;
    if (tiles_.count(frame.id)) return true;
    const int p = percent > 0 ? percent : (frame.percent > 0 ? frame.percent : 100);
    const std::uint64_t rKey = makeRecycleHashKey(frame, p);
    if (rKey == 0) return false;
    auto it = recycledByHash_.find(rKey);
    if (it == recycledByHash_.end() || it->second.empty()) return false;

    auto& bucket = it->second;
    std::size_t pickIdx = bucket.size();
    for (std::size_t i = 0; i < bucket.size(); ++i) {
        if (bucket[i].oldId == frame.id) {
            pickIdx = i;
            break;
        }
    }
    if (pickIdx == bucket.size()) {
        for (std::size_t i = 0; i < bucket.size(); ++i) {
            if (bucket[i].tile.page == page) {
                pickIdx = i;
                break;
            }
        }
    }
    // 다른 쪽의 비트맵은 글자가 같아도 붙이지 않는다. 쪽 위 위치가 다르면 다른 그림이다.
    if (pickIdx == bucket.size()) return false;

    RecycledTile picked = std::move(bucket[pickIdx]);
    bucket.erase(bucket.begin() + static_cast<std::ptrdiff_t>(pickIdx));
    if (bucket.empty()) recycledByHash_.erase(it);

    const FrameDesc& oldDesc = picked.tile.desc;
    auto dropPicked = [&]() {
        dropScreenSprite(picked.oldId);
        pendingDrop_.push_back(picked.oldId);
        recycledOldIds_.erase(picked.oldId);
    };
    if (std::abs(oldDesc.w - frame.w) > 1 || std::abs(oldDesc.h - frame.h) > 1) {
        dropPicked();
        return false;
    }
    if (picked.tile.hasGlyphs) {
        const int deltaOff =
            (frame.lineStart >= 0 && oldDesc.lineStart >= 0) ? (frame.lineStart - oldDesc.lineStart) : 0;
        const std::int32_t deltaOriginX = frame.originX - oldDesc.originX;
        const std::int32_t deltaOriginY = frame.originY - oldDesc.originY;
        if (!cloneAndAdjustFrameGlyphs(picked.oldId, frame.id, frame.lineStory, frame.lineParagraph, deltaOff,
                                       deltaOriginX, deltaOriginY)) {
            dropPicked();
            return false;
        }
    }

    Tile adopted = std::move(picked.tile);
    if (picked.oldId != frame.id) {
        adopted.stamp = ++stamp_;
    } else {
        ++stamp_;
    }
    adopted.page = page;
    adopted.frame = true;
    adopted.percent = p;
    adopted.key = frameKey(frame);
    adopted.desc = frame;
    adopted.desc.percent = p;
    const bool isBase = adopted.frame && adopted.percent == 100;
    tiles_[frame.id] = std::move(adopted);
    {
        std::lock_guard lock(idMu_);
        tileIds_.insert(frame.id);
    }
    if (isBase) nonBaseTiles_.erase(frame.id);
    else nonBaseTiles_.insert(frame.id);
    catalog_[frame.id] = frame;
    if (page >= 0) {
        pageTiles_[page].insert(frame.id);
        pageStamps_[page] = stamp_;
    }
    if (picked.oldId == frame.id) {
        recycledOldIds_.erase(picked.oldId);
    }
    return true;
}

void FrameCache::clear() {
    flushRecycledTiles();
    for (const auto& item : tiles_) dropScreenSprite(item.first);
    tiles_.clear();
    catalog_.clear();
    pageTiles_.clear();
    pageStamps_.clear();
    {
        std::lock_guard lock(pageFramesMu_);
        pageFrames_.clear();
    }
    nonBaseTiles_.clear();
    transientCatalog_.clear();
    pendingDrop_.clear();
    {
        std::lock_guard lock(idMu_);
        ids_.clear();
        tileIds_.clear();
    }
    baseDone_.clear();
    ready_.clear();
    keepRadius_ = -1;
    evictedPages_.clear();
    ++stamp_;
}

void FrameCache::dropPageCatalog() {
    std::lock_guard lock(pageFramesMu_);
    pageFrames_.clear();
}

int pageIndexInKey(const std::string& key) {
    const std::size_t slash = key.find('/');
    if (slash == std::string::npos || slash == 0) return -1;
    try {
        return std::stoi(key.substr(0, slash));
    } catch (...) {
        return -1;
    }
}

std::string shiftPageKey(const std::string& key, int fromPage, int delta) {
    const int page = pageIndexInKey(key);
    if (page < fromPage) return key;
    const std::size_t slash = key.find('/');
    return std::to_string(page + delta) + key.substr(slash);
}

void FrameCache::shiftPages(int fromPage, int delta) {
    if (fromPage < 0 || delta == 0) {
        dropPageCatalog();
        return;
    }
    std::scoped_lock lock(idMu_, pageFramesMu_);
    pageFrames_.clear();
    std::unordered_map<std::string, int> shiftedIds;
    shiftedIds.reserve(ids_.size());
    for (const auto& [key, id] : ids_) shiftedIds.emplace(shiftPageKey(key, fromPage, delta), id);
    ids_ = std::move(shiftedIds);
    for (auto& [id, desc] : catalog_) {
        (void)id;
        if (desc.page >= fromPage) desc.page += delta;
    }
    for (auto& [id, tile] : tiles_) {
        (void)id;
        if (tile.page < fromPage) continue;
        tile.page += delta;
        tile.desc.page += delta;
        if (!tile.key.empty()) tile.key = shiftPageKey(tile.key, fromPage, delta);
    }
    std::unordered_map<int, std::unordered_set<int>> shiftedTiles;
    for (auto& [page, ids] : pageTiles_) {
        const int next = page >= fromPage ? page + delta : page;
        if (next < 0) continue;
        shiftedTiles[next].insert(ids.begin(), ids.end());
    }
    pageTiles_ = std::move(shiftedTiles);
    std::unordered_map<int, int> shiftedStamps;
    for (const auto& [page, stamp] : pageStamps_) {
        const int next = page >= fromPage ? page + delta : page;
        if (next >= 0) shiftedStamps[next] = stamp;
    }
    pageStamps_ = std::move(shiftedStamps);
    std::unordered_set<int> shiftedBase;
    for (const int page : baseDone_) {
        const int next = page >= fromPage ? page + delta : page;
        if (next >= 0) shiftedBase.insert(next);
    }
    baseDone_ = std::move(shiftedBase);
}

// 칸 번호는 키로 정해져서, 문서를 통째로 다시 놓으면 같은 개체·줄이 같은 번호를 다시 받는다.
// 옛 칸을 버리며 적어 둔 번호가 지금 칸 목록이나 캐시에 다시 쓰이면 빼고 넘긴다.
// 넘기면 셸·일꾼이 그 번호의 굽기 일감을 버려 새 칸(그림·도형)이 끝내 구워지지 않는다.
std::vector<int> FrameCache::takePendingDrops() {
    std::vector<int> drop = std::move(pendingDrop_);
    pendingDrop_.clear();
    if (drop.empty()) return drop;
    std::unordered_set<int> live;
    for (const auto& [id, tile] : tiles_) live.insert(id);
    for (const auto& [id, desc] : catalog_) live.insert(id);
    {
        std::lock_guard lock(pageFramesMu_);
        for (const auto& [page, frames] : pageFrames_) {
            if (!frames) continue;
            for (const CachedFrameEntry& entry : frames->entries) live.insert(entry.baseDesc.id);
        }
    }
    std::vector<int> stale;
    stale.reserve(drop.size());
    std::unordered_set<int> seen;
    for (const int id : drop) {
        if (live.count(id) || !seen.insert(id).second) continue;
        stale.push_back(id);
    }
    return stale;
}

void FrameCache::prepareForFullRelayout() {
    flushRecycledTiles();
    for (auto& [id, tile] : tiles_) {
        stashTileForRecycle(id, std::move(tile));
    }
    tiles_.clear();
    {
        std::lock_guard lock(idMu_);
        tileIds_.clear();
    }
    catalog_.clear();
    pageTiles_.clear();
    {
        std::lock_guard lock(pageFramesMu_);
        pageFrames_.clear();
    }
    nonBaseTiles_.clear();
    transientCatalog_.clear();
    baseDone_.clear();
    ready_.clear();
    ++stamp_;
    for (auto& [p, s] : pageStamps_) s = stamp_;
}

int FrameCache::pageRevision(int page) const {
    const auto found = pageStamps_.find(page);
    return found == pageStamps_.end() ? 0 : found->second;
}

std::shared_ptr<const FrameCache::CachedPageFrames> FrameCache::ensurePageFrames(int page, const Node& scene,
                                                                                 const model::Document* doc) {
    static const auto kEmpty = std::make_shared<const CachedPageFrames>();
    if (page < 0 || scene.width <= 0 || scene.height <= 0) return kEmpty;
    const int basePixelH = pagePixelHeight(scene.height, 100);
    if (basePixelH < 32) return kEmpty;
    ensurePageHuBounds(scene, doc);
    const std::size_t firstLineChildren = findFirstLineChildCount(scene);
    {
        std::lock_guard lock(pageFramesMu_);
        const auto found = pageFrames_.find(page);
        if (found != pageFrames_.end() && found->second) {
            const CachedPageFrames& cached = *found->second;
            if (cached.pageNode == &scene && cached.widthHu == scene.width && cached.heightHu == scene.height &&
                cached.childCount == scene.children.size() && cached.firstLineChildren == firstLineChildren &&
                cached.spansLater == scene.spansLater && cached.pageContentHash == scene.contentHash &&
                cached.basePixelH == basePixelH) {
                return found->second;
            }
        }
    }

    std::vector<FrameDesc> raw = collectFrames(scene, page, basePixelH, {}, doc);
    std::stable_sort(raw.begin(), raw.end(), [](const FrameDesc& a, const FrameDesc& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.z != b.z) return a.z < b.z;
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });

    auto built = std::make_shared<CachedPageFrames>();
    built->pageNode = &scene;
    built->widthHu = scene.width;
    built->heightHu = scene.height;
    built->childCount = scene.children.size();
    built->firstLineChildren = firstLineChildren;
    built->spansLater = scene.spansLater;
    built->pageContentHash = scene.contentHash;
    built->basePixelH = basePixelH;
    built->entries.reserve(raw.size());
    for (FrameDesc& desc : raw) {
        desc.percent = 100;
        std::string key = frameKey(desc);
        desc.id = idFor(key, 100);
        built->entries.push_back(CachedFrameEntry{std::move(desc), std::move(key)});
    }
    built->byYOrder.resize(built->entries.size());
    for (std::size_t i = 0; i < built->entries.size(); ++i) built->byYOrder[i] = i;
    std::stable_sort(built->byYOrder.begin(), built->byYOrder.end(), [&](std::size_t ia, std::size_t ib) {
        const FrameDesc& a = built->entries[ia].baseDesc;
        const FrameDesc& b = built->entries[ib].baseDesc;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    built->byHuYOrder.resize(built->entries.size());
    for (std::size_t i = 0; i < built->entries.size(); ++i) built->byHuYOrder[i] = i;
    std::stable_sort(built->byHuYOrder.begin(), built->byHuYOrder.end(), [&](std::size_t ia, std::size_t ib) {
        const FrameDesc& a = built->entries[ia].baseDesc;
        const FrameDesc& b = built->entries[ib].baseDesc;
        if (a.huY != b.huY) return a.huY < b.huY;
        return a.huX < b.huX;
    });
    built->maxHuBottomPrefix.resize(built->byHuYOrder.size());
    std::int32_t runningMaxBottom = std::numeric_limits<std::int32_t>::min();
    for (std::size_t i = 0; i < built->byHuYOrder.size(); ++i) {
        const FrameDesc& d = built->entries[built->byHuYOrder[i]].baseDesc;
        runningMaxBottom = std::max(runningMaxBottom, d.huY + d.huH);
        built->maxHuBottomPrefix[i] = runningMaxBottom;
    }

    {
        std::lock_guard lock(pageFramesMu_);
        pageFrames_[page] = built;
    }
    reconcilePageTiles(page, *built);
    return built;
}

bool FrameCache::pageCatalogCurrent(int page, const Node& scene) const {
    if (page < 0 || scene.width <= 0 || scene.height <= 0) return true;
    const int basePixelH = pagePixelHeight(scene.height, 100);
    if (basePixelH < 32) return true;
    const std::size_t firstLineChildren = findFirstLineChildCount(scene);
    std::lock_guard lock(pageFramesMu_);
    const auto found = pageFrames_.find(page);
    if (found == pageFrames_.end() || !found->second) return false;
    const CachedPageFrames& cached = *found->second;
    return cached.pageNode == &scene && cached.widthHu == scene.width && cached.heightHu == scene.height &&
           cached.childCount == scene.children.size() && cached.firstLineChildren == firstLineChildren &&
           cached.spansLater == scene.spansLater && cached.pageContentHash == scene.contentHash &&
           cached.basePixelH == basePixelH;
}

void FrameCache::reconcilePageTiles(int page, const CachedPageFrames& built) {
    const auto pageSet = pageTiles_.find(page);
    if (pageSet == pageTiles_.end() || pageSet->second.empty()) return;
    std::unordered_map<std::string, const FrameDesc*> live;
    live.reserve(built.entries.size());
    for (const CachedFrameEntry& entry : built.entries) live.emplace(entry.key, &entry.baseDesc);
    const std::vector<int> ids(pageSet->second.begin(), pageSet->second.end());
    bool changed = false;
    for (const int id : ids) {
        const auto tileIt = tiles_.find(id);
        if (tileIt == tiles_.end()) {
            pageTiles_[page].erase(id);
            changed = true;
            continue;
        }
        Tile& tile = tileIt->second;
        const auto found = live.find(tile.key);
        const FrameDesc* now = found == live.end() ? nullptr : found->second;
        const bool sameContent = now && now->contentHash != 0 && tile.desc.contentHash == now->contentHash;
        if (!sameContent || (tile.percent == 100 && now &&
                             (std::abs(tile.desc.w - now->w) > 1 || std::abs(tile.desc.h - now->h) > 1))) {
            Tile taken = std::move(tile);
            tiles_.erase(tileIt);
            {
                std::lock_guard lock(idMu_);
                tileIds_.erase(id);
            }
            nonBaseTiles_.erase(id);
            catalog_.erase(id);
            pageTiles_[page].erase(id);
            baseDone_.erase(page);
            if (taken.desc.contentHash != 0) stashTileForRecycle(id, std::move(taken));
            else {
                dropScreenSprite(id);
                pendingDrop_.push_back(id);
            }
            changed = true;
            continue;
        }
        // 글자가 같은 줄이 다른 문단·시작 위치로 옮겨 가도 칸은 그대로 쓴다. 글자의 문단 위치만 지금 줄로 옮긴다.
        if (now->lineStory != tile.desc.lineStory || now->lineParagraph != tile.desc.lineParagraph ||
            now->lineStart != tile.desc.lineStart) {
            const int deltaOff =
                (now->lineStart >= 0 && tile.desc.lineStart >= 0) ? (now->lineStart - tile.desc.lineStart) : 0;
            if (tile.hasGlyphs) cloneAndAdjustFrameGlyphs(id, id, now->lineStory, now->lineParagraph, deltaOff, 0, 0);
            tile.desc.lineStory = now->lineStory;
            tile.desc.lineParagraph = now->lineParagraph;
            tile.desc.lineStart = now->lineStart;
            if (const auto listed = catalog_.find(id); listed != catalog_.end()) {
                listed->second.lineStory = now->lineStory;
                listed->second.lineParagraph = now->lineParagraph;
                listed->second.lineStart = now->lineStart;
            }
        }
        if (tile.percent != 100) continue;
        if (tile.desc.x == now->x && tile.desc.y == now->y && tile.desc.huX == now->huX && tile.desc.huY == now->huY &&
            tile.desc.huW == now->huW && tile.desc.huH == now->huH && tile.desc.drawHuX == now->drawHuX &&
            tile.desc.drawHuY == now->drawHuY && tile.desc.drawHuW == now->drawHuW && tile.desc.drawHuH == now->drawHuH) {
            continue;
        }
        tile.desc.x = now->x;
        tile.desc.y = now->y;
        tile.desc.w = now->w;
        tile.desc.h = now->h;
        tile.desc.huX = now->huX;
        tile.desc.huY = now->huY;
        tile.desc.huW = now->huW;
        tile.desc.huH = now->huH;
        tile.desc.drawHuX = now->drawHuX;
        tile.desc.drawHuY = now->drawHuY;
        tile.desc.drawHuW = now->drawHuW;
        tile.desc.drawHuH = now->drawHuH;
        tile.desc.originX = now->originX;
        tile.desc.originY = now->originY;
        tile.stamp = ++stamp_;
        catalog_[id] = tile.desc;
        changed = true;
    }
    if (changed) pageStamps_[page] = ++stamp_;
}

std::vector<FrameDesc> FrameCache::baseFrames(int page, const Node& scene, const model::Document* doc) {
    const auto cachedPtr = ensurePageFrames(page, scene, doc);
    const CachedPageFrames& cached = *cachedPtr;
    std::vector<FrameDesc> out;
    out.reserve(cached.entries.size());
    for (const std::size_t idx : cached.byYOrder) {
        out.push_back(cached.entries[idx].baseDesc);
    }
    return out;
}

namespace {

bool descMatchesInvalidation(const FrameDesc& desc, const FrameInvalidation& inv) {
    if (inv.page >= 0 && desc.page != inv.page) return false;
    if (desc.path.size() < inv.pathPrefix.size()) return false;
    for (std::size_t i = 0; i < inv.pathPrefix.size(); ++i) {
        if (desc.path[i] != inv.pathPrefix[i]) return false;
    }
    if (inv.selfOnly) {
        return desc.path.size() == inv.pathPrefix.size();
    }
    if (inv.minChild >= 0) {
        if (desc.path.size() <= inv.pathPrefix.size()) return false;
        const int childIdx = desc.path[inv.pathPrefix.size()];
        if (childIdx < inv.minChild) return false;
        if (inv.maxChild >= 0 && childIdx > inv.maxChild) return false;
    }
    return true;
}

bool descMatchesAnyInvalidation(const FrameDesc& desc, const std::vector<FrameInvalidation>& invalidations) {
    for (const FrameInvalidation& inv : invalidations) {
        if (descMatchesInvalidation(desc, inv)) return true;
    }
    return false;
}

}  // namespace

void FrameCache::invalidate(const std::vector<FrameInvalidation>& invalidations, bool recycle) {
    if (invalidations.empty()) return;
    ++stamp_;
    {
        std::lock_guard lock(pageFramesMu_);
        for (const FrameInvalidation& inv : invalidations) {
            if (inv.page < 0) {
                pageFrames_.clear();
                break;
            }
            pageFrames_.erase(inv.page);
        }
    }
    for (const FrameInvalidation& inv : invalidations) {
        if (inv.page < 0) {
            for (auto& [p, s] : pageStamps_) s = stamp_;
        } else {
            pageStamps_[inv.page] = stamp_;
        }
    }
    std::unordered_set<int> dropPages;
    bool dropAll = false;
    for (const FrameInvalidation& inv : invalidations) {
        if (inv.page < 0) dropAll = true;
        else dropPages.insert(inv.page);
    }
    std::unordered_set<int> removeIds;
    std::unordered_set<std::string> deadKeys;
    for (const auto& [id, desc] : catalog_) {
        // 줄 하나만 고쳐도 그 쪽의 옛 그림을 남기면 새 글자와 겹친다.
        if (dropAll || dropPages.count(desc.page) || descMatchesAnyInvalidation(desc, invalidations)) {
            removeIds.insert(id);
            deadKeys.insert(frameKey(desc));
        }
    }
    if (dropAll) {
        for (const auto& [page, ids] : pageTiles_) {
            for (const int id : ids) removeIds.insert(id);
        }
    } else {
        for (const int page : dropPages) {
            const auto pageSet = pageTiles_.find(page);
            if (pageSet == pageTiles_.end()) continue;
            for (const int id : pageSet->second) removeIds.insert(id);
        }
    }
    auto purgeRecycledPage = [&](int page) {
        for (auto it = recycledByHash_.begin(); it != recycledByHash_.end();) {
            auto& bucket = it->second;
            for (auto tileIt = bucket.begin(); tileIt != bucket.end();) {
                if (page >= 0 && tileIt->tile.page != page) {
                    ++tileIt;
                    continue;
                }
                dropScreenSprite(tileIt->oldId);
                pendingDrop_.push_back(tileIt->oldId);
                recycledOldIds_.erase(tileIt->oldId);
                tileIt = bucket.erase(tileIt);
            }
            if (bucket.empty()) it = recycledByHash_.erase(it);
            else ++it;
        }
    };
    if (dropAll) purgeRecycledPage(-1);
    else {
        for (const int page : dropPages) purgeRecycledPage(page);
    }
    for (const int id : removeIds) {
        if (const auto tileIt = tiles_.find(id); tileIt != tiles_.end()) {
            if (!tileIt->second.key.empty()) deadKeys.insert(tileIt->second.key);
            const int page = tileIt->second.page;
            if (page >= 0) {
                if (const auto pageSet = pageTiles_.find(page); pageSet != pageTiles_.end()) pageSet->second.erase(id);
                baseDone_.erase(page);
                pageStamps_[page] = stamp_;
            }
            // 화면 목록에서는 빼서 옛 줄이 새 글자 아래 남지 않게 한다. 비트맵은 재활용 칸으로 넘겨
            // 내용이 같은 칸(밀리기만 한 줄, 그림)은 다시 굽지 않고 새 자리로 입양한다.
            if (recycle) {
                stashTileForRecycle(id, std::move(tileIt->second));
            } else {
                dropScreenSprite(id);
                pendingDrop_.push_back(id);
            }
            tiles_.erase(tileIt);
            {
                std::lock_guard lock(idMu_);
                tileIds_.erase(id);
            }
        } else {
            dropScreenSprite(id);
            pendingDrop_.push_back(id);
        }
        nonBaseTiles_.erase(id);
        catalog_.erase(id);
    }
    if (!deadKeys.empty()) {
        std::lock_guard lock(idMu_);
        for (auto it = ids_.begin(); it != ids_.end();) {
            const std::size_t atPos = it->first.rfind('@');
            const std::string baseKey = atPos == std::string::npos ? it->first : it->first.substr(0, atPos);
            if (deadKeys.count(baseKey)) {
                it = ids_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

bool FrameCache::contains(int id) const {
    std::lock_guard lock(idMu_);
    return tileIds_.count(id) > 0;
}

bool FrameCache::getReadyTile(int id, ReadyTile& out) const {
    const auto found = tiles_.find(id);
    if (found == tiles_.end() || !usable(id, found->second)) return false;
    const Tile& t = found->second;
    out.id = id;
    out.stamp = t.stamp;
    out.percent = t.percent;
    out.hasGlyphs = t.hasGlyphs;
    out.bitmap = &t.bitmap;
    out.key = t.key.empty() ? nullptr : &t.key;
    if (t.frame && t.desc.id == id) {
        out.desc = &t.desc;
    } else {
        const auto cat = catalog_.find(id);
        out.desc = (cat != catalog_.end()) ? &cat->second : nullptr;
    }
    return true;
}

const PageBitmap* FrameCache::bitmap(int id) const {
    const auto found = tiles_.find(id);
    if (found == tiles_.end()) return nullptr;
    return &found->second.bitmap;
}

bool FrameCache::hasGlyphs(int id) const {
    const auto found = tiles_.find(id);
    if (found == tiles_.end()) return false;
    return found->second.hasGlyphs;
}

int FrameCache::percent(int id) const {
    const auto found = tiles_.find(id);
    if (found == tiles_.end()) return 0;
    return found->second.percent;
}

int FrameCache::stamp(int id) const {
    const auto found = tiles_.find(id);
    if (found == tiles_.end()) return 0;
    return found->second.stamp;
}

const std::string* FrameCache::key(int id) const {
    const auto found = tiles_.find(id);
    if (found == tiles_.end() || found->second.key.empty()) return nullptr;
    return &found->second.key;
}

const FrameDesc* FrameCache::desc(int id) const {
    const auto tileIt = tiles_.find(id);
    if (tileIt != tiles_.end() && tileIt->second.frame && tileIt->second.desc.id == id) {
        return &tileIt->second.desc;
    }
    const auto found = catalog_.find(id);
    if (found == catalog_.end()) return nullptr;
    return &found->second;
}

void FrameCache::retain(int id, int page, PageBitmap bitmap) {
    if (id <= 0 || bitmap.rgba.empty()) return;
    Tile kept;
    kept.stamp = ++stamp_;
    kept.page = page;
    kept.bitmap = std::move(bitmap);
    const auto described = catalog_.find(id);
    if (described != catalog_.end()) {
        kept.frame = true;
        kept.percent = described->second.percent;
        kept.key = frameKey(described->second);
        kept.desc = described->second;
    }
    const bool isBase = kept.frame && kept.percent == 100;
    tiles_[id] = std::move(kept);
    {
        std::lock_guard lock(idMu_);
        tileIds_.insert(id);
    }
    if (isBase) nonBaseTiles_.erase(id);
    else nonBaseTiles_.insert(id);
    if (page >= 0) {
        pageTiles_[page].insert(id);
        pageStamps_[page] = stamp_;
    }
}

void FrameCache::erase(int id) {
    const auto found = tiles_.find(id);
    if (found == tiles_.end()) return;
    const int page = found->second.page;
    tiles_.erase(found);
    {
        std::lock_guard lock(idMu_);
        tileIds_.erase(id);
    }
    nonBaseTiles_.erase(id);
    ++stamp_;
    if (page >= 0) {
        if (const auto pageSet = pageTiles_.find(page); pageSet != pageTiles_.end()) pageSet->second.erase(id);
        pageStamps_[page] = stamp_;
    }
    catalog_.erase(id);
}

int FrameCache::assign(FrameDesc& frame) {
    frame.id = idFor(frameKey(frame), frame.percent > 0 ? frame.percent : 100);
    return frame.id;
}

void FrameCache::keepBaked(const FrameDesc& frame, int page, PageBitmap bitmap) {
    const bool glyphs = frameHasGlyphs(frame.id);
    if (bitmap.width <= 0 || bitmap.height <= 0 || (bitmap.rgba.empty() && !glyphs)) {
        bitmap.width = 1;
        bitmap.height = 1;
        bitmap.rgba.assign(4, 0);
    }
    Tile kept;
    kept.stamp = ++stamp_;
    kept.page = page;
    kept.frame = true;
    kept.hasGlyphs = glyphs;
    kept.percent = frame.percent;
    kept.key = frameKey(frame);
    kept.desc = frame;
    kept.bitmap = std::move(bitmap);
    const bool isBase = kept.frame && kept.percent == 100;
    tiles_[frame.id] = std::move(kept);
    {
        std::lock_guard lock(idMu_);
        tileIds_.insert(frame.id);
    }
    if (isBase) nonBaseTiles_.erase(frame.id);
    else nonBaseTiles_.insert(frame.id);
    catalog_[frame.id] = frame;
    if (page >= 0) {
        pageTiles_[page].insert(frame.id);
        pageStamps_[page] = stamp_;
    }
}

int FrameCache::countOnPage(int page, int percent) const {
    const auto pageSet = pageTiles_.find(page);
    if (pageSet == pageTiles_.end()) return 0;
    int filled = 0;
    for (const int id : pageSet->second) {
        const auto item = tiles_.find(id);
        if (item == tiles_.end() || !item->second.frame || item->second.percent != percent) continue;
        const auto described = catalog_.find(id);
        if (described != catalog_.end() && described->second.kind == 0) continue;
        ++filled;
    }
    return filled;
}

int FrameCache::idFor(const std::string& key, int percent) {
    std::lock_guard lock(idMu_);
    const std::string idKey = key + "@" + std::to_string(percent);
    const auto found = ids_.find(idKey);
    if (found != ids_.end()) return found->second;
    const int id = nextId_++;
    ids_.emplace(idKey, id);
    return id;
}

bool FrameCache::usable(int id, const Tile& tile) const {
    (void)id;
    if (!tile.frame) return false;
    if (tile.hasGlyphs) return true;
    return tile.bitmap.width > 1 && tile.bitmap.height > 1 && !tile.bitmap.rgba.empty();
}

void FrameCache::plan(ViewPlan& plan, const std::vector<Node>& pages, const std::unordered_set<int>& ready,
                      const model::Document* doc, bool screenCrewFastPath) {
    ready_ = ready;
    const int percent = plan.percent > 0 ? plan.percent : 100;
    int visiblePages = 0;
    int firstVisible = -1;
    int lastVisible = -1;
    for (const ViewPage& page : plan.pages) {
        if (!hits(page.x, page.y, page.w, page.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH)) continue;
        ++visiblePages;
        if (firstVisible < 0) firstVisible = page.index;
        lastVisible = page.index;
    }
    const bool scaleOnly = overviewZoom(visiblePages, percent);
    bool catalogsCurrent = true;
    if (screenCrewFastPath && (scaleOnly || percent <= 100) && recycledByHash_.empty() && recycledOldIds_.empty()) {
        for (const ViewPage& page : plan.pages) {
            if (page.index < 0 || page.index >= static_cast<int>(pages.size())) continue;
            if (!hits(page.x, page.y, page.w, page.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH)) continue;
            if (!pageCatalogCurrent(page.index, pages[static_cast<std::size_t>(page.index)])) {
                catalogsCurrent = false;
                break;
            }
        }
    }
    if (catalogsCurrent && screenCrewFastPath && (scaleOnly || percent <= 100) && recycledByHash_.empty() &&
        recycledOldIds_.empty()) {
        std::vector<int> drop = takePendingDrops();
        for (auto it = nonBaseTiles_.begin(); it != nonBaseTiles_.end();) {
            const int id = *it;
            const auto tileIt = tiles_.find(id);
            if (tileIt == tiles_.end()) {
                it = nonBaseTiles_.erase(it);
                continue;
            }
            const Tile& tile = tileIt->second;
            if (tile.frame && tile.percent == 100) {
                it = nonBaseTiles_.erase(it);
                continue;
            }
            const int page = tile.page;
            drop.push_back(id);
            dropScreenSprite(id);
            if (page >= 0) {
                if (const auto pageSet = pageTiles_.find(page); pageSet != pageTiles_.end()) pageSet->second.erase(id);
                pageStamps_[page] = ++stamp_;
            }
            catalog_.erase(id);
            tiles_.erase(tileIt);
            {
                std::lock_guard lock(idMu_);
                tileIds_.erase(id);
            }
            it = nonBaseTiles_.erase(it);
        }
        for (const int id : transientCatalog_) {
            if (tiles_.count(id)) continue;
            catalog_.erase(id);
        }
        transientCatalog_.clear();
        plan.drop = std::move(drop);
        plan.draw.clear();
        plan.job = {};
        return;
    }

    struct WantedItem {
        FrameDesc frame;
        const std::string* key = nullptr;
        int baseId = 0;
        int pageX = 0;
        int pageY = 0;
        int pageW = 0;
        int pageH = 0;
    };
    std::vector< std::shared_ptr<const CachedPageFrames> > keptPageFrames;
    std::vector<WantedItem> wanted;
    std::vector<int> wantedPages;
    for (const ViewPage& page : plan.pages) {
        if (page.index < 0 || page.index >= static_cast<int>(pages.size())) continue;
        if (!ready.empty() && !ready.count(page.index)) continue;
        const Node& scene = pages[static_cast<std::size_t>(page.index)];
        if (scene.height <= 0 || plan.viewW <= 0 || plan.viewH <= 0) continue;
        const int pageMargin = page.h > 0 ? std::max(page.h * 2, 320) : plan.viewH;
        const int margin = (percent < 100 && page.h > 0)
                               ? std::clamp(std::min(plan.viewH, pageMargin), 1, std::max(plan.viewH, 1))
                               : std::max(plan.viewH, 1);
        if (!hits(page.x, page.y, page.w, page.h, plan.scrollX - margin, plan.scrollY - margin,
                  plan.viewW + margin * 2, plan.viewH + margin * 2)) {
            continue;
        }
        const int pixelH = pagePixelHeight(scene.height, percent);
        if (pixelH < 32) continue;
        const auto cachedPtr = ensurePageFrames(page.index, scene, doc);
        const CachedPageFrames& cached = *cachedPtr;
        if (cached.entries.empty()) continue;
        keptPageFrames.push_back(cachedPtr);
        const FrameCull cull =
            pageCull(page, scene.height, pixelH, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH, margin);
        const float scale = static_cast<float>(pixelH) / static_cast<float>(scene.height);
        const int viewX = plan.scrollX - margin;
        const int viewY = plan.scrollY - margin;
        const int viewWidth = plan.viewW + margin * 2;
        const int viewHeight = plan.viewH + margin * 2;
        bool pageAdded = false;
        const auto visitEntry = [&](const CachedFrameEntry& entry) {
            if (missesCull(entry.baseDesc, cull)) return;
            FrameDesc frame = scaleCachedFrame(entry.baseDesc, percent, pixelH, scale);
            if (!hits(page.x + frame.x, page.y + frame.y, frame.w, frame.h, viewX, viewY, viewWidth, viewHeight)) {
                return;
            }
            if (percent == 100 || scaleOnly) {
                frame.id = entry.baseDesc.id;
            } else {
                frame.id = idFor(entry.key, percent);
            }
            if (!recycledByHash_.empty()) {
                if (!tiles_.count(entry.baseDesc.id)) {
                    tryAdoptRecycledTile(entry.baseDesc, page.index, 100);
                }
                if (frame.id != entry.baseDesc.id && !tiles_.count(frame.id)) {
                    tryAdoptRecycledTile(frame, page.index, percent);
                }
            }
            wanted.push_back(WantedItem{std::move(frame), &entry.key, entry.baseDesc.id, page.x, page.y, page.w, page.h});
            pageAdded = true;
        };
        if (cull.on && !cached.byHuYOrder.empty() && cached.maxHuBottomPrefix.size() == cached.byHuYOrder.size()) {
            const std::int32_t minBottom = cull.y - kBleedGuardHu;
            const std::int32_t maxTop = cull.y + cull.h + kBleedGuardHu;
            const auto firstIt =
                std::upper_bound(cached.maxHuBottomPrefix.begin(), cached.maxHuBottomPrefix.end(), minBottom);
            const std::size_t startIdx = static_cast<std::size_t>(firstIt - cached.maxHuBottomPrefix.begin());
            for (std::size_t k = startIdx; k < cached.byHuYOrder.size(); ++k) {
                const CachedFrameEntry& entry = cached.entries[cached.byHuYOrder[k]];
                if (entry.baseDesc.huY >= maxTop) break;
                visitEntry(entry);
            }
        } else {
            for (const CachedFrameEntry& entry : cached.entries) {
                visitEntry(entry);
            }
        }
        if (pageAdded) wantedPages.push_back(page.index);
    }

    if (!recycledByHash_.empty() || !recycledOldIds_.empty()) {
        if (++recycledAge_ >= 2) {
            flushRecycledTiles();
        }
    }

    std::sort(wanted.begin(), wanted.end(), [](const WantedItem& a, const WantedItem& b) {
        if (a.frame.layer != b.frame.layer) return a.frame.layer < b.frame.layer;
        if (a.frame.z != b.frame.z) return a.frame.z < b.frame.z;
        if (a.frame.rank != b.frame.rank) return a.frame.rank < b.frame.rank;
        const int ay = a.pageY + a.frame.y;
        const int by = b.pageY + b.frame.y;
        if (ay != by) return ay < by;
        return a.pageX + a.frame.x < b.pageX + b.frame.x;
    });

    std::vector<int> drop = takePendingDrops();
    if (!nonBaseTiles_.empty() || !transientCatalog_.empty()) {
        std::unordered_set<std::string> wantedKeys;
        std::unordered_set<std::string> readyNow;
        std::unordered_map<std::string, int> currentIds;
        wantedKeys.reserve(wanted.size());
        readyNow.reserve(wanted.size());
        currentIds.reserve(wanted.size());
        for (const WantedItem& item : wanted) {
            wantedKeys.insert(*item.key);
            if (tiles_.count(item.frame.id)) {
                readyNow.insert(*item.key);
                currentIds.emplace(*item.key, item.frame.id);
            }
        }
        std::unordered_map<std::string, std::pair<int, int>> fallback;
        for (const int id : nonBaseTiles_) {
            const auto item = tiles_.find(id);
            if (item == tiles_.end() || !item->second.frame || item->second.percent == 100 ||
                item->second.percent == percent) {
                continue;
            }
            if (!wantedKeys.count(item->second.key) || readyNow.count(item->second.key)) continue;
            const int dist = std::abs(item->second.percent - percent);
            const auto found = fallback.find(item->second.key);
            if (found == fallback.end() || dist < std::abs(found->second.second - percent) ||
                (dist == std::abs(found->second.second - percent) && item->second.percent > found->second.second)) {
                fallback[item->second.key] = {item->first, item->second.percent};
            }
        }
        std::unordered_set<int> keepFallback;
        for (const auto& item : fallback) keepFallback.insert(item.second.first);
        for (auto it = nonBaseTiles_.begin(); it != nonBaseTiles_.end();) {
            const int id = *it;
            const auto tileIt = tiles_.find(id);
            if (tileIt == tiles_.end()) {
                it = nonBaseTiles_.erase(it);
                continue;
            }
            const Tile& tile = tileIt->second;
            if (tile.frame && tile.percent == 100) {
                it = nonBaseTiles_.erase(it);
                continue;
            }
            const auto cur = currentIds.find(tile.key);
            const bool replacedByCurrent = tile.frame && cur != currentIds.end() && cur->second != id;
            const bool current = !scaleOnly && tile.frame && tile.percent == percent && wantedKeys.count(tile.key);
            if (!replacedByCurrent && (current || keepFallback.count(id))) {
                ++it;
                continue;
            }
            const int page = tile.page;
            drop.push_back(id);
            dropScreenSprite(id);
            if (page >= 0) {
                if (const auto pageSet = pageTiles_.find(page); pageSet != pageTiles_.end()) pageSet->second.erase(id);
                pageStamps_[page] = ++stamp_;
            }
            catalog_.erase(id);
            tiles_.erase(tileIt);
            {
                std::lock_guard lock(idMu_);
                tileIds_.erase(id);
            }
            it = nonBaseTiles_.erase(it);
        }
        for (const int id : transientCatalog_) {
            if (tiles_.count(id)) continue;
            const auto catIt = catalog_.find(id);
            if (catIt == catalog_.end()) continue;
            const std::string key = frameKey(catIt->second);
            if (currentIds.count(key) || wantedKeys.count(key)) continue;
            catalog_.erase(catIt);
        }
        transientCatalog_.clear();
    }
    if (!scaleOnly) {
        std::vector<int> nextTransient;
        nextTransient.reserve(wanted.size());
        for (const WantedItem& item : wanted) {
            if (!tiles_.count(item.frame.id)) {
                nextTransient.push_back(item.frame.id);
                catalog_[item.frame.id] = item.frame;
            }
        }
        transientCatalog_ = std::move(nextTransient);
    }
    plan.drop = std::move(drop);
    plan.draw.clear();
    plan.job = {};

    struct ScaleTile {
        int id = 0;
        int percent = 0;
    };
    std::unordered_map<std::string, ScaleTile> scaledFrom;
    if (!nonBaseTiles_.empty()) {
        for (const int page : wantedPages) {
            const auto pageSet = pageTiles_.find(page);
            if (pageSet == pageTiles_.end()) continue;
            for (const int id : pageSet->second) {
                const auto item = tiles_.find(id);
                if (item == tiles_.end() || !usable(item->first, item->second)) continue;
                const int tilePercent = item->second.percent;
                if (tilePercent == percent || tilePercent == 100) continue;
                const std::string& key = item->second.key;
                const int dist = std::abs(tilePercent - percent);
                const auto found = scaledFrom.find(key);
                if (found == scaledFrom.end() || dist < std::abs(found->second.percent - percent) ||
                    (dist == std::abs(found->second.percent - percent) && tilePercent > found->second.percent)) {
                    scaledFrom[key] = ScaleTile{item->first, tilePercent};
                }
            }
        }
    }
    for (const WantedItem& item : wanted) {
        const FrameDesc& frame = item.frame;
        int srcId = 0;
        int srcPercent = 0;
        const auto baseIt = tiles_.find(item.baseId);
        const bool hasBase = baseIt != tiles_.end() && usable(item.baseId, baseIt->second);
        if ((scaleOnly || percent < 100) && hasBase) {
            srcId = item.baseId;
            srcPercent = 100;
        } else {
            const auto exact = (frame.id == item.baseId) ? baseIt : tiles_.find(frame.id);
            if (exact != tiles_.end() && usable(frame.id, exact->second) && exact->second.percent == percent) {
                srcId = frame.id;
                srcPercent = percent;
            } else if (!scaledFrom.empty()) {
                const auto other = scaledFrom.find(*item.key);
                if (other != scaledFrom.end()) {
                    srcId = other->second.id;
                    srcPercent = other->second.percent;
                }
            }
            if (!srcId && hasBase) {
                srcId = item.baseId;
                srcPercent = 100;
            }
        }
        const bool nearest = frame.kind >= 3;
        if (!srcId) continue;
        int localX = frame.x;
        int localY = frame.y;
        int localW = frame.w;
        int localH = frame.h;
        const int dstPageH = item.pageH > 0 ? item.pageH : frame.pixelHeight;
        if (srcPercent > 0 && frame.page >= 0 &&
            frame.page < static_cast<int>(pages.size()) && pages[static_cast<std::size_t>(frame.page)].height > 0) {
            const int pageHeightHu = pages[static_cast<std::size_t>(frame.page)].height;
            const auto tile = (srcId == item.baseId) ? baseIt : tiles_.find(srcId);
            const int srcPixels = tile == tiles_.end() ? 0 : tile->second.bitmap.width;
            const int srcPixelsH = tile == tiles_.end() ? 0 : tile->second.bitmap.height;
            const int srcPageH = pagePixelHeight(pageHeightHu, srcPercent);
            if (srcPixels > 1 && srcPixelsH > 1 && srcPageH > 0 && dstPageH > 0 &&
                (srcPercent != percent || dstPageH != srcPageH)) {
                int dstX = frame.x;
                int dstY = frame.y;
                if (dstPageH != frame.pixelHeight && pageHeightHu > 0) {
                    const float dstScale = static_cast<float>(dstPageH) / static_cast<float>(pageHeightHu);
                    dstX = framePxFromHu(frame.huX, dstScale) - kFramePad;
                    dstY = framePxFromHu(frame.huY, dstScale) - kFramePad;
                }
                placeZoomedFrame(dstX, dstY, dstPageH, srcPageH, srcPixels, srcPixelsH, localX, localY, localW,
                                 localH);
            }
        }
        // 이번 프레임에는 화면에 걸친 칸만 올린다. 바로 바깥 칸은 캐시에 두고 제출하지 않는다.
        if (!hits(item.pageX + localX, item.pageY + localY, localW, localH, plan.scrollX, plan.scrollY, plan.viewW,
                  plan.viewH)) {
            continue;
        }
        ViewDraw draw;
        draw.id = srcId;
        draw.x = item.pageX - plan.scrollX + localX;
        draw.y = item.pageY - plan.scrollY + localY;
        draw.w = localW;
        draw.h = localH;
        draw.layer = frame.layer;
        draw.nearest = nearest;
        plan.draw.push_back(draw);
    }

    const WantedItem* open = nullptr;
    int openY = 0;
    for (const WantedItem& item : wanted) {
        const FrameDesc& frame = item.frame;
        const int absY = item.pageY + frame.y;
        if (!hits(item.pageX + frame.x, absY, frame.w, frame.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH))
            continue;
        const bool useBase = scaleOnly || !canBakeFrame(frame);
        if (useBase || percent < 100) {
            if (tiles_.count(item.baseId)) continue;
        }
        if (!useBase && tiles_.count(frame.id)) {
            continue;
        }
        const bool aObj = frame.kind != 0;
        const bool bObj = open && open->frame.kind != 0;
        if (!open || (aObj != bObj ? aObj : absY < openY)) {
            open = &item;
            openY = absY;
        }
    }
    auto finish = [&](const FrameDesc& frame, const std::string& key, int baseId, int jobPercent, int jobPixelH,
                      int jobYpx, bool background) {
        const int jobId = (jobPercent == 100 && baseId > 0) ? baseId : idFor(key, jobPercent);
        plan.job.id = jobId;
        plan.job.generation = plan.generation;
        plan.job.page = frame.page;
        plan.job.percent = jobPercent;
        plan.job.pixelHeight = jobPixelH;
        plan.job.x = frame.x;
        plan.job.y = jobYpx;
        plan.job.w = frame.w;
        plan.job.h = frame.h;
        plan.job.layer = frame.layer;
        plan.job.background = background;
        if (!tiles_.count(jobId)) {
            FrameDesc jobDesc = frame;
            if (jobPercent != frame.percent && frame.page >= 0 && frame.page < static_cast<int>(pages.size())) {
                const Node& scene = pages[static_cast<std::size_t>(frame.page)];
                if (scene.height > 0 && jobPixelH > 0) {
                    const float jobScale = static_cast<float>(jobPixelH) / static_cast<float>(scene.height);
                    jobDesc = scaleCachedFrame(frame, jobPercent, jobPixelH, jobScale);
                }
            }
            jobDesc.id = jobId;
            jobDesc.percent = jobPercent;
            catalog_[jobId] = std::move(jobDesc);
        }
    };
    auto placeJob = [&](const WantedItem& item, bool background) {
        const FrameDesc& frame = item.frame;
        const bool useBase = scaleOnly || !canBakeFrame(frame);
        const int jobPercent = useBase ? 100 : percent;
        int jobPixelH = frame.pixelHeight;
        int jobYpx = frame.y;
        if (useBase && frame.page >= 0 && frame.page < static_cast<int>(pages.size())) {
            const Node& scene = pages[static_cast<std::size_t>(frame.page)];
            jobPixelH = pagePixelHeight(scene.height, 100);
            if (frame.pixelHeight > 0 && jobPixelH > 0) {
                jobYpx = static_cast<int>(std::lround(static_cast<double>(frame.y) * static_cast<double>(jobPixelH) /
                                                      static_cast<double>(frame.pixelHeight)));
            }
        }
        finish(frame, *item.key, item.baseId, jobPercent, jobPixelH, jobYpx, background);
    };
    if (open) {
        // 줄 자리는 워커를 붙잡는 준비 루프에 넣지 않는다. 유휴가 적고, 그 사이에 화면과 선택이 돈다.
        placeJob(*open, open->frame.kind == 0);
        return;
    }
    if (!scaleOnly) {
        const WantedItem* near = nullptr;
        int nearGap = 0;
        for (const WantedItem& item : wanted) {
            const FrameDesc& frame = item.frame;
            // 줄 칸은 화면에 들어온 것만 연다. 화면 밖 줄의 전진폭과 자리는 유휴가 따로 적는다.
            if (frame.kind == 0) continue;
            if (!canBakeFrame(frame) || percent < 100) {
                if (tiles_.count(item.baseId)) continue;
            }
            if (canBakeFrame(frame) && tiles_.count(frame.id)) {
                continue;
            }
            const int absX = item.pageX + frame.x;
            const int absY = item.pageY + frame.y;
            if (hits(absX, absY, frame.w, frame.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH)) continue;
            const int gap = outsideGap(absX, absY, frame.w, frame.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH);
            if (!near || gap < nearGap) {
                near = &item;
                nearGap = gap;
            }
        }
        if (near) {
            placeJob(*near, true);
            return;
        }
    }

    if (pages.empty()) return;
    const int origin = firstVisible >= 0 ? firstVisible : 0;
    const int end = lastVisible >= 0 ? lastVisible : origin;
    std::vector<int> order;
    const int count = static_cast<int>(pages.size());
    for (int step = 1; step < count; ++step) {
        if (end + step < count) order.push_back(end + step);
        if (origin - step >= 0) order.push_back(origin - step);
    }
    for (const ViewPage& page : plan.pages) {
        if (hits(page.x, page.y, page.w, page.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH)) order.push_back(page.index);
    }
    int checkedBasePages = 0;
    for (const int page : order) {
        if (page < 0 || page >= static_cast<int>(pages.size())) continue;
        if (!ready.empty() && !ready.count(page)) continue;
        if (baseDone_.count(page)) continue;
        const Node& scene = pages[static_cast<std::size_t>(page)];
        const auto cachedPtr = ensurePageFrames(page, scene, doc);
        const CachedPageFrames& cached = *cachedPtr;
        const CachedFrameEntry* missing = nullptr;
        for (const std::size_t idx : cached.byYOrder) {
            const CachedFrameEntry& entry = cached.entries[idx];
            // 화면 밖 줄 칸은 여기 두지 않는다. 전진폭과 자리는 유휴가 적는다.
            if (entry.baseDesc.kind == 0) continue;
            if (tiles_.count(entry.baseDesc.id)) continue;
            if (!recycledByHash_.empty() && tryAdoptRecycledTile(entry.baseDesc, page, 100)) continue;
            missing = &entry;
            break;
        }
        if (!missing) {
            baseDone_.insert(page);
            if (++checkedBasePages >= 2) break;
            continue;
        }
        finish(missing->baseDesc, missing->key, missing->baseDesc.id, 100, cached.basePixelH, missing->baseDesc.y, true);
        return;
    }
}

FramePrepare FrameCache::prepare(const model::Document& doc, std::vector<Node>& pages, int page, int percent,
                                 int focusY) {
    FramePrepare out;
    if (page < 0 || page >= static_cast<int>(pages.size())) return out;
    if (percent <= 0) percent = 100;
    ensurePageSpans(doc, pages[static_cast<std::size_t>(page)]);
    const Node& scene = pages[static_cast<std::size_t>(page)];
    const int pixelH = pagePixelHeight(scene.height, percent);
    if (pixelH < 32 || scene.height <= 0) return out;
    const auto cachedPtr = ensurePageFrames(page, scene, &doc);
    const CachedPageFrames& cached = *cachedPtr;
    const float scale = static_cast<float>(pixelH) / static_cast<float>(scene.height);
    struct PrepCand {
        FrameDesc frame;
        std::string key;
    };
    std::vector<PrepCand> frames;
    frames.reserve(cached.entries.size());
    for (const CachedFrameEntry& entry : cached.entries) {
        FrameDesc frame = scaleCachedFrame(entry.baseDesc, percent, pixelH, scale);
        if (percent == 100) frame.id = entry.baseDesc.id;
        frames.push_back(PrepCand{std::move(frame), entry.key});
    }
    const int focus = focusY;
    std::stable_sort(frames.begin(), frames.end(), [&](const PrepCand& ca, const PrepCand& cb) {
        const FrameDesc& a = ca.frame;
        const FrameDesc& b = cb.frame;
        if (focus >= 0) {
            const int da = std::abs((a.y + a.h / 2) - focus);
            const int db = std::abs((b.y + b.h / 2) - focus);
            if (da != db) return da < db;
        }
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    const int budget = std::max(kPrepareBudget, layout::workerCount(256));
    std::vector<QueuedFrame> queued;
    bool more = false;
    int total = 0;
    for (const PrepCand& cand : frames) {
        if (cand.frame.kind != 0) ++total;
    }
    for (PrepCand& cand : frames) {
        FrameDesc& frame = cand.frame;
        // 글 줄 자리는 여기서 걷지 않는다. 유휴가 메모리에 적는다.
        if (frame.kind == 0) continue;
        frame.percent = percent;
        if (!canBakeFrame(frame)) continue;
        if (percent != 100) frame.id = idFor(cand.key, percent);
        if (tiles_.count(frame.id)) continue;
        if (!recycledByHash_.empty() && tryAdoptRecycledTile(frame, page, percent)) continue;
        if (static_cast<int>(queued.size()) >= budget) {
            more = true;
            break;
        }
        queued.push_back(QueuedFrame{std::move(frame), page, percent, total});
    }
    return bake(doc, pages, std::move(queued), more);
}

FramePrepare FrameCache::bake(const model::Document& doc, const std::vector<Node>& pages, std::vector<QueuedFrame> queued, bool more) {
    FramePrepare out;
    if (queued.empty()) return out;
    std::vector<PageBitmap> bitmaps(queued.size());
    int usedThreads = 1;
    const int count = static_cast<int>(queued.size());
    const int threads = layout::workerCount(count);
    if (threads <= 1) {
        for (int i = 0; i < count; ++i) {
            const QueuedFrame& work = queued[static_cast<std::size_t>(i)];
            bitmaps[static_cast<std::size_t>(i)] = renderFrame(doc, pages[static_cast<std::size_t>(work.page)], work.frame);
        }
    } else {
        usedThreads = threads;
        std::atomic<int> next{0};
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));
        while (static_cast<int>(pool.size()) < threads) {
            pool.emplace_back([&] {
                for (;;) {
                    const int i = next.fetch_add(1, std::memory_order_relaxed);
                    if (i >= count) return;
                    const QueuedFrame& work = queued[static_cast<std::size_t>(i)];
                    bitmaps[static_cast<std::size_t>(i)] = renderFrame(doc, pages[static_cast<std::size_t>(work.page)], work.frame);
                }
            });
        }
        for (std::thread& worker : pool) worker.join();
    }
    for (int i = 0; i < count; ++i) {
        QueuedFrame& work = queued[static_cast<std::size_t>(i)];
        PageBitmap bitmap = std::move(bitmaps[static_cast<std::size_t>(i)]);
        const bool glyphs = frameHasGlyphs(work.frame.id);
        if (bitmap.width <= 0 || bitmap.height <= 0 || (bitmap.rgba.empty() && !glyphs)) {
            bitmap.width = 1;
            bitmap.height = 1;
            bitmap.rgba.assign(4, 0);
        }
        Tile kept;
        kept.stamp = ++stamp_;
        kept.page = work.page;
        kept.frame = true;
        kept.hasGlyphs = glyphs;
        kept.percent = work.percent;
        kept.key = frameKey(work.frame);
        kept.desc = work.frame;
        kept.bitmap = std::move(bitmap);
        const bool isBase = kept.frame && kept.percent == 100;
        tiles_[work.frame.id] = std::move(kept);
        {
            std::lock_guard lock(idMu_);
            tileIds_.insert(work.frame.id);
        }
        if (isBase) nonBaseTiles_.erase(work.frame.id);
        else nonBaseTiles_.insert(work.frame.id);
        catalog_[work.frame.id] = work.frame;
        if (work.page >= 0) {
            pageTiles_[work.page].insert(work.frame.id);
            pageStamps_[work.page] = stamp_;
        }
    }
    out.made = count;
    out.done = !more;
    std::string line = "[ClassingHWP] 스레드 " + std::to_string(usedThreads) + " 칸 " + std::to_string(count);
    std::vector<int> seen;
    for (const QueuedFrame& work : queued) {
        if (std::find(seen.begin(), seen.end(), work.page) != seen.end()) continue;
        seen.push_back(work.page);
        int filled = 0;
        if (const auto pageSet = pageTiles_.find(work.page); pageSet != pageTiles_.end()) {
            for (const int id : pageSet->second) {
                const auto item = tiles_.find(id);
                if (item != tiles_.end() && item->second.frame && item->second.percent == work.percent) ++filled;
            }
        }
        line += " · " + std::to_string(work.page + 1) + "쪽 " + std::to_string(filled) + "/" + std::to_string(work.total);
    }
    std::fprintf(stderr, "%s\n", line.c_str());
    return out;
}

void FrameCache::queueNear(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan, int percent,
                           bool baseNear, int margin, int budget, std::vector<QueuedFrame>& queued, bool& more) {
    for (const ViewPage& page : plan.pages) {
        if (page.index < 0 || page.index >= static_cast<int>(pages.size())) continue;
        if (!ready_.empty() && !ready_.count(page.index)) continue;
        if (!hits(page.x, page.y, page.w, page.h, plan.scrollX - margin, plan.scrollY - margin,
                  plan.viewW + margin * 2, plan.viewH + margin * 2)) {
            continue;
        }
        ensurePageSpans(doc, pages[static_cast<std::size_t>(page.index)]);
        const Node& scene = pages[static_cast<std::size_t>(page.index)];
        const int pixelH = pagePixelHeight(scene.height, percent);
        if (pixelH < 32 || scene.height <= 0) continue;
        const auto cachedPtr = ensurePageFrames(page.index, scene, &doc);
        const CachedPageFrames& cached = *cachedPtr;
        const FrameCull cull =
            pageCull(page, scene.height, pixelH, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH, margin);
        const float scale = static_cast<float>(pixelH) / static_cast<float>(scene.height);
        struct IdleCand {
            FrameDesc frame;
            std::string key;
            const FrameDesc* base = nullptr;
        };
        std::vector<IdleCand> frames;
        for (const CachedFrameEntry& entry : cached.entries) {
            if (missesCull(entry.baseDesc, cull)) continue;
            FrameDesc frame = scaleCachedFrame(entry.baseDesc, percent, pixelH, scale);
            if (percent == 100) frame.id = entry.baseDesc.id;
            frames.push_back(IdleCand{std::move(frame), entry.key, &entry.baseDesc});
        }
        const int total = static_cast<int>(frames.size());
        std::stable_sort(frames.begin(), frames.end(), [&](const IdleCand& ca, const IdleCand& cb) {
            const FrameDesc& a = ca.frame;
            const FrameDesc& b = cb.frame;
            const int ga = outsideGap(page.x + a.x, page.y + a.y, a.w, a.h, plan.scrollX, plan.scrollY, plan.viewW,
                                      plan.viewH);
            const int gb = outsideGap(page.x + b.x, page.y + b.y, b.w, b.h, plan.scrollX, plan.scrollY, plan.viewW,
                                      plan.viewH);
            if (ga != gb) return ga < gb;
            if (ga == 0) {
                const int bandA = std::max(0, page.y + a.y - plan.scrollY) / 160;
                const int bandB = std::max(0, page.y + b.y - plan.scrollY) / 160;
                if (bandA != bandB) return bandA < bandB;
                const bool aObj = a.kind != 0;
                const bool bObj = b.kind != 0;
                if (aObj != bObj) return aObj;
            }
            if (a.y != b.y) return a.y < b.y;
            return a.x < b.x;
        });
        for (IdleCand& cand : frames) {
            FrameDesc& frame = cand.frame;
            // 화면 밖 줄 칸은 여기서 굽지 않는다. 전진폭과 자리는 유휴가 적는다.
            if (frame.kind == 0 &&
                !hits(page.x + frame.x, page.y + frame.y, frame.w, frame.h, plan.scrollX, plan.scrollY, plan.viewW,
                      plan.viewH)) {
                continue;
            }
            if (baseNear) {
                // 화면 자리로 고르고, 굽는 것은 100% 칸이다.
                if (tiles_.count(cand.base->id)) continue;
                if (!recycledByHash_.empty() && tryAdoptRecycledTile(*cand.base, page.index, 100)) continue;
                if (static_cast<int>(queued.size()) >= budget) {
                    more = true;
                    break;
                }
                queued.push_back(QueuedFrame{*cand.base, page.index, 100, total});
                continue;
            }
            frame.percent = percent;
            if (!canBakeFrame(frame)) continue;
            if (percent != 100) frame.id = idFor(cand.key, percent);
            if (tiles_.count(frame.id)) continue;
            if (!recycledByHash_.empty() && tryAdoptRecycledTile(frame, page.index, percent)) continue;
            if (static_cast<int>(queued.size()) >= budget) {
                more = true;
                break;
            }
            queued.push_back(QueuedFrame{std::move(frame), page.index, percent, total});
        }
        if (more) break;
    }
}

FramePrepare FrameCache::prepareView(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan,
                                     int maxTiles) {
    if (plan.viewW <= 0 || plan.viewH <= 0) return {};
    const int percent = plan.percent > 0 ? plan.percent : 100;
    int visiblePages = 0;
    for (const ViewPage& page : plan.pages) {
        if (hits(page.x, page.y, page.w, page.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH)) ++visiblePages;
    }
    // 여러 쪽 축소 보기는 plan 처럼 100% 칸을 줄여 쓴다.
    const bool baseNear = overviewZoom(visiblePages, percent);
    std::vector<QueuedFrame> queued;
    bool more = false;
    queueNear(doc, pages, plan, percent, baseNear, 0, std::max(1, maxTiles), queued, more);
    return bake(doc, pages, std::move(queued), more);
}

IdleStep FrameCache::prepareIdle(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan) {
    IdleStep step;
    if (plan.job.id == 0 || !plan.job.background) {
        step.screen = plan.job.id != 0;
        return step;
    }
    const int percent = plan.percent > 0 ? plan.percent : 100;
    const int margin = std::max(plan.viewH, 1);
    // 여러 쪽 축소 보기는 100% 칸을 줄여 쓰므로 작업 배율이 100이다. 그때도 화면 쪽을 먼저 굽는다.
    const bool baseNear = plan.job.percent == 100 && percent < 100;
    bool nearPage = false;
    if (plan.job.percent == percent || baseNear) {
        for (const ViewPage& page : plan.pages) {
            if (page.index != plan.job.page) continue;
            nearPage = hits(page.x, page.y, page.w, page.h, plan.scrollX - margin, plan.scrollY - margin,
                            plan.viewW + margin * 2, plan.viewH + margin * 2);
        }
    }
    if (nearPage) {
        const int budget = std::max(kPrepareBudget, layout::workerCount(256));
        std::vector<QueuedFrame> queued;
        bool more = false;
        queueNear(doc, pages, plan, percent, baseNear, margin, budget, queued, more);
        if (!queued.empty()) {
            const FramePrepare baked = bake(doc, pages, std::move(queued), more);
            if (baked.made > 0) {
                step.made = baked.made;
                step.more = true;
                return step;
            }
        }
    }
    const int budget = std::max(kPrepareBudget, layout::workerCount(256));
    const int pageCount = static_cast<int>(pages.size());
    int firstVisible = -1;
    int lastVisible = -1;
    for (const ViewPage& page : plan.pages) {
        if (!hits(page.x, page.y, page.w, page.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH)) continue;
        if (firstVisible < 0) firstVisible = page.index;
        lastVisible = page.index;
    }
    const int origin = firstVisible >= 0 ? firstVisible : 0;
    const int end = lastVisible >= 0 ? lastVisible : origin;
    std::vector<int> order;
    for (int stepIndex = 1; stepIndex < pageCount; ++stepIndex) {
        if (end + stepIndex < pageCount) order.push_back(end + stepIndex);
        if (origin - stepIndex >= 0) order.push_back(origin - stepIndex);
    }
    for (const ViewPage& page : plan.pages) {
        if (hits(page.x, page.y, page.w, page.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH)) order.push_back(page.index);
    }
    std::vector<QueuedFrame> queued;
    bool more = false;
    for (const int page : order) {
        if (page < 0 || page >= pageCount) continue;
        if (!ready_.empty() && !ready_.count(page)) continue;
        if (baseDone_.count(page) || !withinKeep(page)) continue;
        ensurePageSpans(doc, pages[static_cast<std::size_t>(page)]);
        const Node& scene = pages[static_cast<std::size_t>(page)];
        const auto cachedPtr = ensurePageFrames(page, scene, &doc);
        const CachedPageFrames& cached = *cachedPtr;
        if (cached.entries.empty()) continue;
        const int focus = page == plan.job.page ? plan.job.y : -1;
        std::vector<std::size_t> indices = cached.byYOrder;
        if (focus >= 0) {
            std::stable_sort(indices.begin(), indices.end(), [&](std::size_t ia, std::size_t ib) {
                const FrameDesc& a = cached.entries[ia].baseDesc;
                const FrameDesc& b = cached.entries[ib].baseDesc;
                const int da = std::abs((a.y + a.h / 2) - focus);
                const int db = std::abs((b.y + b.h / 2) - focus);
                if (da != db) return da < db;
                if (a.y != b.y) return a.y < b.y;
                return a.x < b.x;
            });
        }
        const int total = static_cast<int>(cached.entries.size());
        bool pageDone = true;
        for (const std::size_t idx : indices) {
            const FrameDesc& frame = cached.entries[idx].baseDesc;
            // 화면 밖 줄 칸은 개체 준비에 넣지 않는다. 전진폭과 자리는 유휴가 적는다.
            if (frame.kind == 0) continue;
            if (tiles_.count(frame.id)) continue;
            if (!recycledByHash_.empty() && tryAdoptRecycledTile(frame, page, 100)) continue;
            pageDone = false;
            if (static_cast<int>(queued.size()) >= budget) {
                more = true;
                break;
            }
            queued.push_back(QueuedFrame{frame, page, 100, total});
        }
        if (pageDone) baseDone_.insert(page);
        if (more) break;
    }
    const FramePrepare baked = bake(doc, pages, std::move(queued), more);
    step.made = baked.made;
    step.more = baked.made > 0;
    return step;
}

void FrameCache::bakeVisibleNow(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan) {
    if (pages.empty() || plan.viewW <= 0 || plan.viewH <= 0) return;
    int visiblePages = 0;
    for (const ViewPage& page : plan.pages) {
        if (hits(page.x, page.y, page.w, page.h, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH)) {
            ++visiblePages;
        }
    }
    const int planPercent = plan.percent > 0 ? plan.percent : 100;
    const bool scaleOnly = overviewZoom(visiblePages, planPercent);
    const int percent = (scaleOnly || planPercent < 100) ? 100 : planPercent;
    const int margin = 64;

    for (const ViewPage& page : plan.pages) {
        if (page.index < 0 || page.index >= static_cast<int>(pages.size())) continue;
        if (!hits(page.x, page.y, page.w, page.h, plan.scrollX - margin, plan.scrollY - margin, plan.viewW + margin * 2,
                  plan.viewH + margin * 2)) {
            continue;
        }
        ensurePageSpans(doc, pages[static_cast<std::size_t>(page.index)]);
        const Node& scene = pages[static_cast<std::size_t>(page.index)];
        const int pixelH = pagePixelHeight(scene.height, percent);
        if (pixelH < 32 || scene.height <= 0) continue;
        const auto cachedPtr = ensurePageFrames(page.index, scene, &doc);
        const CachedPageFrames& cached = *cachedPtr;
        if (cached.entries.empty()) continue;
        const FrameCull cull =
            pageCull(page, scene.height, pixelH, plan.scrollX, plan.scrollY, plan.viewW, plan.viewH, margin);
        const float scale = static_cast<float>(pixelH) / static_cast<float>(scene.height);
        const auto bakeEntry = [&](const CachedFrameEntry& entry) {
            if (missesCull(entry.baseDesc, cull)) return;
            FrameDesc frame = scaleCachedFrame(entry.baseDesc, percent, pixelH, scale);
            int screenFX = frame.x;
            int screenFY = frame.y;
            int screenFW = frame.w;
            int screenFH = frame.h;
            if (page.h > 0 && pixelH > 0 && page.h != pixelH) {
                placeZoomedFrame(frame.x, frame.y, page.h, pixelH, frame.w, frame.h, screenFX, screenFY, screenFW,
                                 screenFH);
            }
            if (!hits(page.x + screenFX, page.y + screenFY, screenFW, screenFH, plan.scrollX - margin,
                      plan.scrollY - margin, plan.viewW + margin * 2, plan.viewH + margin * 2)) {
                return;
            }
            frame.percent = percent;
            if (canBakeFrame(frame)) {
                frame.id = (percent == 100) ? entry.baseDesc.id : idFor(entry.key, percent);
                if (tiles_.count(frame.id)) return;
                if (!recycledByHash_.empty() && tryAdoptRecycledTile(frame, page.index, percent)) return;
                PageBitmap bitmap = renderFrame(doc, scene, frame);
                keepBaked(frame, page.index, std::move(bitmap));
                return;
            }
            if (percent == 100) return;
            const FrameDesc& baseFrame = entry.baseDesc;
            if (!canBakeFrame(baseFrame)) return;
            if (!tiles_.count(baseFrame.id)) {
                if (!recycledByHash_.empty() && tryAdoptRecycledTile(baseFrame, page.index, 100)) return;
                PageBitmap bitmap = renderFrame(doc, scene, baseFrame);
                keepBaked(baseFrame, page.index, std::move(bitmap));
            }
        };
        if (cull.on && !cached.byHuYOrder.empty() && cached.maxHuBottomPrefix.size() == cached.byHuYOrder.size()) {
            const std::int32_t minBottom = cull.y - kBleedGuardHu;
            const std::int32_t maxTop = cull.y + cull.h + kBleedGuardHu;
            const auto firstIt =
                std::upper_bound(cached.maxHuBottomPrefix.begin(), cached.maxHuBottomPrefix.end(), minBottom);
            const std::size_t startIdx = static_cast<std::size_t>(firstIt - cached.maxHuBottomPrefix.begin());
            for (std::size_t k = startIdx; k < cached.byHuYOrder.size(); ++k) {
                const CachedFrameEntry& entry = cached.entries[cached.byHuYOrder[k]];
                if (entry.baseDesc.huY >= maxTop) break;
                bakeEntry(entry);
            }
        } else {
            for (const CachedFrameEntry& entry : cached.entries) {
                bakeEntry(entry);
            }
        }
    }
}

namespace {

// 같은 칸의 두 배율 중 cand 가 cur 보다 목표에 맞는가.
bool betterPercent(int cand, int cur, int target) {
    if (cand == cur) return false;
    if (cand == target) return true;
    if (cur == target) return false;
    if (target == 100) return cand < cur;
    if (cand == 100) return false;
    if (cur == 100) return true;
    const int dc = std::abs(cand - target);
    const int d0 = std::abs(cur - target);
    if (dc != d0) return dc < d0;
    return cand > cur;
}

}  // namespace

std::vector<PageSprite> readyPageSprites(const FrameCache& frames, int page, int targetPercent, bool* multiPercent) {
    std::vector<PageSprite> sprites;
    std::unordered_map<std::string_view, std::size_t> placedByKey;
    int firstTilePercent = -1;
    bool mixed = false;
    frames.forReadyPageTiles(page, [&](const FrameCache::ReadyTile& ready) {
        if (!ready.desc || !ready.bitmap) return;
        const FrameDesc& frame = *ready.desc;
        const int framePercent = ready.percent;
        const bool hasBitmap = ready.bitmap->width > 1 && ready.bitmap->height > 1 && !ready.bitmap->rgba.empty();
        if (!hasBitmap && !ready.hasGlyphs) return;
        if (frame.w <= 0 || frame.h <= 0) return;
        if (firstTilePercent < 0) firstTilePercent = framePercent;
        else if (firstTilePercent != framePercent) mixed = true;
        const std::string_view key = ready.key ? std::string_view(*ready.key) : std::string_view{};
        const auto found = key.empty() ? placedByKey.end() : placedByKey.find(key);
        if (found != placedByKey.end()) {
            if (!betterPercent(framePercent, sprites[found->second].percent, targetPercent)) return;
        }
        PageSprite item;
        item.id = frame.id;
        item.stamp = ready.stamp;
        item.frameX = frame.x;
        item.frameY = frame.y;
        item.frameW = frame.w;
        item.frameH = frame.h;
        item.srcPixelH = frame.pixelHeight;
        item.localX = frame.x;
        item.localY = frame.y;
        item.w = frame.w;
        item.h = frame.h;
        item.sw = ready.bitmap->width;
        item.sh = ready.bitmap->height;
        item.nearest = frame.kind >= 3;
        item.percent = framePercent;
        item.rgba = hasBitmap ? ready.bitmap->rgba.data() : nullptr;
        item.layer = std::clamp(frame.layer, 0, 3);
        item.z = frame.z;
        item.rank = frame.rank;
        item.huX = frame.huX;
        item.huY = frame.huY;
        item.huW = frame.huW;
        item.huH = frame.huH;
        if (found != placedByKey.end()) {
            sprites[found->second] = item;
        } else {
            if (!key.empty()) placedByKey.emplace(key, sprites.size());
            sprites.push_back(item);
        }
    });
    std::stable_sort(sprites.begin(), sprites.end(), [](const PageSprite& a, const PageSprite& b) {
        if (a.layer != b.layer) return a.layer < b.layer;
        if (a.z != b.z) return a.z < b.z;
        if (a.rank != b.rank) return a.rank < b.rank;
        if (a.huY != b.huY) return a.huY < b.huY;
        if (a.huX != b.huX) return a.huX < b.huX;
        if (a.frameY != b.frameY) return a.frameY < b.frameY;
        return a.frameX < b.frameX;
    });
    if (multiPercent) *multiPercent = mixed;
    return sprites;
}

void placePageSprites(std::vector<PageSprite>& sprites, int spriteZoom, int dstPixelH, std::int32_t pageHeightHu) {
    const auto framePxAt = [](std::int32_t hu, float scale) {
        return static_cast<int>(hu * scale + (hu < 0 ? -0.5f : 0.5f));
    };
    const float dstScale = (pageHeightHu > 0 && dstPixelH > 0)
                               ? (static_cast<float>(dstPixelH) / static_cast<float>(pageHeightHu))
                               : 0.f;
    for (PageSprite& item : sprites) {
        int localX = item.frameX;
        int localY = item.frameY;
        int localW = item.frameW;
        int localH = item.frameH;
        const int srcPixelH = item.srcPixelH;
        if (item.percent != spriteZoom || (srcPixelH > 0 && dstPixelH > 0 && dstPixelH != srcPixelH)) {
            if (dstPixelH > 0 && srcPixelH > 0) {
                int dstX = item.frameX;
                int dstY = item.frameY;
                if (dstScale > 0.f) {
                    dstX = framePxAt(item.huX, dstScale) - kFramePad;
                    dstY = framePxAt(item.huY, dstScale) - kFramePad;
                } else {
                    const double ratio = static_cast<double>(dstPixelH) / static_cast<double>(srcPixelH);
                    dstX = static_cast<int>(std::lround(static_cast<double>(item.frameX + kFramePad) * ratio)) -
                           kFramePad;
                    dstY = static_cast<int>(std::lround(static_cast<double>(item.frameY + kFramePad) * ratio)) -
                           kFramePad;
                }
                const int srcW = item.sw > 1 ? item.sw : item.frameW;
                const int srcH = item.sh > 1 ? item.sh : item.frameH;
                placeZoomedFrame(dstX, dstY, dstPixelH, srcPixelH, srcW, srcH, localX, localY, localW, localH);
            }
        }
        item.localX = localX;
        item.localY = localY;
        item.w = std::max(1, localW);
        item.h = std::max(1, localH);
    }
}

}  // namespace chwp::render
