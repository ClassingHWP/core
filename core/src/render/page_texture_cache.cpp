#include "render/page_texture_cache.hpp"

#include <algorithm>

#include "chwp/render/paint.hpp"

namespace chwp::render {

void PageTextureCache::erase(std::unordered_map<int, PageTextureMeta>::iterator it) {
    const int page = it->first;
    bytes_ -= std::min(bytes_, it->second.bytes);
    entries_.erase(it);
    if (onDrop_) onDrop_(page);
}

std::size_t PageTextureCache::release(CacheTier tier, std::size_t need) {
    std::size_t freed = 0;
    while (freed < need) {
        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (tierForFrame(it->second.lastFrame) != tier) continue;
            if (oldest == entries_.end() || it->second.lastUsedTick < oldest->second.lastUsedTick) oldest = it;
        }
        if (oldest == entries_.end()) break;
        freed += oldest->second.bytes;
        erase(oldest);
    }
    return freed;
}

bool PageTextureCache::bakeable(int page, int w, int h, int stamp) {
    return page >= 0 && stamp != 0 && w > 0 && h > 0 && w <= kMaxPageGpuCacheW && h <= kMaxPageGpuCacheH;
}

bool PageTextureCache::usable(int page, const PageTextureWant& want) const {
    if (want.layoutZoom > 100) return false;
    if (!bakeable(page, want.w, want.h, want.stamp)) return false;
    const PageTextureMeta* entry = find(page);
    if (!entry || entry->w <= 0 || entry->h <= 0 || entry->stamp != want.stamp) return false;
    if (want.interacting) return true;
    // 마스크가 덜 된 채 구운 쪽은 손이 멈추면 쓰지 않고 다시 굽는다. 텍스처로 그리는 동안에는 그 쪽 글자의 마스크를
    // 아무도 청하지 않아, 마스크가 더 생기기를 기다리면 빈 글자가 남는다. 다시 구울 때 그 쪽 글자를 그리며 마스크를 청한다.
    if (entry->incomplete) return false;
    if (entry->picturesCoarse && entry->pictureSeq != want.pictureSeq) return false;
    if (entry->w == want.w && entry->h == want.h && entry->layoutZoom == want.layoutZoom &&
        entry->maskZoom == want.maskZoom) {
        return true;
    }
    // 더 큰 배율로 구운 쪽은 밉맵으로 줄여 쓴다. 같은 배율이면 마스크 배율까지 같아야 한다.
    return entry->layoutZoom > want.layoutZoom && entry->layoutZoom <= 100;
}

bool PageTextureCache::scalable(int page, int stamp) const {
    if (page < 0 || stamp == 0) return false;
    const PageTextureMeta* entry = find(page);
    return entry && entry->stamp == stamp && entry->w > 0 && entry->h > 0;
}

int PageTextureCache::zoomOf(int page, int stamp) const {
    return scalable(page, stamp) ? find(page)->layoutZoom : 0;
}

const PageTextureMeta* PageTextureCache::find(int page) const {
    const auto found = entries_.find(page);
    return found == entries_.end() ? nullptr : &found->second;
}

bool PageTextureCache::store(PageTextureMeta meta) {
    if (meta.page < 0 || meta.w <= 0 || meta.h <= 0) return false;
    meta.bytes = static_cast<std::size_t>(meta.w) * static_cast<std::size_t>(meta.h) * 4u;
    if (meta.bytes > kPageTextureBudget) return false;
    if (const auto existing = entries_.find(meta.page); existing != entries_.end()) {
        // 같은 쪽을 갈아 끼운다. 그리는 쪽은 곧 새 이미지를 넣으므로 버림을 알리지 않는다.
        bytes_ -= std::min(bytes_, existing->second.bytes);
        entries_.erase(existing);
    }
    meta.lastUsedTick = ++tick_;
    meta.lastFrame = cacheFrame();
    bytes_ += meta.bytes;
    entries_[meta.page] = meta;
    // 새 쪽은 이번 프레임에 쓴 것이라 남고, 넘치면 오래 안 본 쪽부터 비운다.
    if (registered_) enforceCacheBudget(*this);
    return true;
}

void PageTextureCache::touch(int page) {
    if (const auto found = entries_.find(page); found != entries_.end()) {
        found->second.lastUsedTick = ++tick_;
        found->second.lastFrame = cacheFrame();
    }
}

void PageTextureCache::markFrame(int page) {
    if (const auto found = entries_.find(page); found != entries_.end()) found->second.lastFrame = cacheFrame();
}

void PageTextureCache::setPicturesCoarse(int page, bool coarse) {
    if (const auto found = entries_.find(page); found != entries_.end()) found->second.picturesCoarse = coarse;
}

void PageTextureCache::drop(int page) {
    if (page < 0) {
        clear();
        return;
    }
    if (const auto found = entries_.find(page); found != entries_.end()) erase(found);
}

void PageTextureCache::clear() {
    while (!entries_.empty()) erase(entries_.begin());
    bytes_ = 0;
}

void PageTextureCache::ensureRegistered(std::size_t budget) {
    if (registered_) return;
    registered_ = true;
    registerCachePool(*this, budget);
}

namespace {

bool bakeCandidate(const ScreenSprite& paper) { return paper.nearest && paper.page >= 0 && paper.stamp != 0; }

}  // namespace

PaperPlan planPaper(const PageTextureCache& cache, const ScreenSprite& paper, const PaperFrame& frame,
                    bool pageHasSprites, const PageBakeBudget& budget) {
    PaperPlan plan;
    if (paper.w <= 0 || paper.h <= 0) return plan;
    plan.onScreen = paper.x < frame.viewW && paper.y < frame.viewH && paper.x + paper.w > 0 && paper.y + paper.h > 0;
    plan.cacheW = paper.sw > 0 ? paper.sw : paper.w;
    plan.cacheH = paper.sh > 0 ? paper.sh : paper.h;
    plan.cacheZoom = paper.percent > 0 ? paper.percent : (frame.layoutZoom > 0 ? std::max(frame.layoutZoom, 25) : 100);
    if (!frame.texturesOn) return plan;
    PageTextureWant want;
    want.w = plan.cacheW;
    want.h = plan.cacheH;
    want.layoutZoom = plan.cacheZoom;
    want.maskZoom = frame.maskZoom;
    want.stamp = paper.stamp;
    want.interacting = frame.dragging || frame.interacting;
    want.maskSeq = frame.maskSeq;
    want.pictureSeq = frame.pictureSeq;
    plan.useTexture = cache.usable(paper.page, want);
    plan.bake = !plan.useTexture && bakeCandidate(paper) && pageHasSprites &&
                PageTextureCache::bakeable(paper.page, plan.cacheW, plan.cacheH, paper.stamp) &&
                budget.allows(plan.cacheW * plan.cacheH);
    plan.scaleTexture = !plan.useTexture && plan.cacheZoom <= 100 && cache.scalable(paper.page, paper.stamp);
    return plan;
}

MaskPass maskPassFor(const std::vector<ScreenSprite>& papers, int layoutZoom, int viewW, int viewH) {
    // 핀치 중에는 10% 단위로 화면 안 글리프만 찍고, 멈추면 나머지 마스크까지 찍는다.
    MaskPass pass;
    pass.layoutZoom = layoutZoom > 0 ? std::max(layoutZoom, 25) : layoutZoom;
    bool pageBake = false;
    for (const ScreenSprite& paper : papers) {
        if (!bakeCandidate(paper)) continue;
        pageBake = true;
        const int zoom = paper.percent > 0 ? paper.percent : pass.layoutZoom;
        if (zoom > pass.layoutZoom) pass.layoutZoom = zoom;
    }
    const bool everywhere = pageBake || (layoutZoom > 0 && layoutZoom < 25);
    pass.viewW = everywhere ? 0 : viewW;
    pass.viewH = everywhere ? 0 : viewH;
    return pass;
}

std::unordered_map<int, std::vector<const ScreenSprite*>> spritesByPage(const std::vector<ScreenSprite>& papers,
                                                                        const std::vector<ScreenSprite>& sprites) {
    std::unordered_map<int, std::vector<const ScreenSprite*>> out;
    if (sprites.empty() || std::none_of(papers.begin(), papers.end(), bakeCandidate)) return out;
    for (const ScreenSprite& sprite : sprites) {
        if (sprite.page >= 0) out[sprite.page].push_back(&sprite);
    }
    return out;
}

}  // namespace chwp::render
