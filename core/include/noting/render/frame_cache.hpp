#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "noting/model/document.hpp"
#include "noting/render/cache_budget.hpp"
#include "noting/render/paint.hpp"
#include "noting/render/view.hpp"

namespace noting::render {

// 화면에 쪽이 이 수 이상이고 배율이 100보다 작으면, 그 배율 칸은 만들지 않고 100%를 줄여 보여 준다.
constexpr int kOverviewPages = 4;

bool overviewZoom(int visiblePages, int percent);

// 화면과 그 위아래 한 화면 안에 걸친 칸. 100%가 아닌 배율은 이 칸만 굽는다.
std::vector<FrameDesc> framesOnScreen(const Node& scene, const ViewPage& page, int percent, int scrollX, int scrollY,
                                     int viewW, int viewH, const model::Document* doc = nullptr);

// 글자 마스크를 굽는 배율. 50% 아래는 50% 마스크를 줄여 쓰고, 그 위는 10% 칸(100% 넘으면 넓어진다)으로 올려 잡는다.
// 마스크가 그리는 글자보다 조금 크거나 같아서 늘려 흐려지는 일이 없다.
int maskZoomPercent(int percent, bool dragging);

// 100% 이하에서 쪽을 통째로 굽는 GPU 텍스처 배율(50/100). 100%를 넘으면 그 배율이다.
// 언제나 보이는 배율 이상으로 구워 밉맵으로 줄이기만 한다. 작은 글자 점이 늘어나 덩어리로 보이지 않는다.
int pageCachePercent(int percent);

// 멈춰 있을 때 쓰는 마스크 배율. 100% 이하는 쪽 텍스처에 1:1로 찍히도록 그 텍스처 배율에 맞춘다.
int restingMaskZoom(int percent);

struct FramePrepare {
    int made = 0;
    bool done = true;
};

// 유휴 준비 한 번. screen 이면 보이는 칸이 먼저라 캐시를 하지 않았다.
struct IdleStep {
    bool more = false;
    bool screen = false;
    int made = 0;
};

// 줄·개체·표 칸 캐시. 어떤 배율을 남기고 다음에 무엇을 구울지는 여기서 정한다.
class FrameCache {
 public:
    struct ReadyTile {
        int id = 0;
        int stamp = 0;
        int percent = 0;
        bool hasGlyphs = false;
        const PageBitmap* bitmap = nullptr;
        const std::string* key = nullptr;
        const FrameDesc* desc = nullptr;
    };

    FrameCache();
    ~FrameCache();
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    void clear();
    void prepareForFullRelayout();
    // recycle 이면 지운 칸 비트맵을 재활용 칸으로 넘긴다. 내용이 같은 칸은 다시 굽지 않고 입양한다.
    void invalidate(const std::vector<FrameInvalidation>& invalidations, bool recycle = true);
    bool contains(int id) const;
    bool getReadyTile(int id, ReadyTile& out) const;
    const PageBitmap* bitmap(int id) const;
    bool hasGlyphs(int id) const;
    int percent(int id) const;
    int stamp(int id) const;
    const std::string* key(int id) const;
    const FrameDesc* desc(int id) const;
    void retain(int id, int page, PageBitmap bitmap);
    void erase(int id);
    // 키에 칸 번호를 붙인다. 그리는 스레드와 화면 스레드가 같이 부른다.
    int assign(FrameDesc& frame);
    // 구워진 칸을 캐시에 넣는다. 화면 스레드만 부른다.
    void keepBaked(const FrameDesc& frame, int page, PageBitmap bitmap);
    int countOnPage(int page, int percent) const;
    // 구워 둔 칸 비트맵 바이트. 재활용 칸도 센다. 화면 스레드만 부른다.
    std::size_t bitmapBytes() const;
    // 칸이 구워지거나 지워지면 늘어난다. 화면 루프가 다시 그릴지 볼 때 쓴다.
    int revision() const { return stamp_; }
    int pageRevision(int page) const;
    bool hasPendingDrops() const { return !pendingDrop_.empty(); }
    std::vector<FrameDesc> baseFrames(int page, const Node& scene, const model::Document* doc = nullptr);
    // 쪽 벡터를 옮기면 칸 목록이 쥐던 쪽 노드 주소는 버린다. 구워 둔 비트맵은 남긴다.
    void dropPageCatalog();
    // fromPage 이상 쪽 번호를 delta 만큼 민다. 본문 비트맵은 새 번호로 따라간다.
    void shiftPages(int fromPage, int delta);

    // 이미 구워진 칸. 화면 스레드만 부른다. 스크롤 중 계획에 없던 칸을 올릴 때 쓴다.
    template <typename Fn>
    void forReady(Fn&& fn) const {
        for (const auto& [id, desc] : catalog_) {
            const auto tile = tiles_.find(id);
            if (tile == tiles_.end() || !usable(id, tile->second)) continue;
            fn(desc, tile->second.percent);
        }
    }

    template <typename Fn>
    void forReadyPage(int page, Fn&& fn) const {
        const auto found = pageTiles_.find(page);
        if (found == pageTiles_.end()) return;
        for (const int id : found->second) {
            const auto tile = tiles_.find(id);
            if (tile == tiles_.end() || !usable(id, tile->second)) continue;
            if (tile->second.frame && tile->second.desc.id == id) {
                fn(tile->second.desc, tile->second.percent);
                continue;
            }
            const auto desc = catalog_.find(id);
            if (desc == catalog_.end()) continue;
            fn(desc->second, tile->second.percent);
        }
    }

    template <typename Fn>
    void forReadyPageTiles(int page, Fn&& fn) const {
        const auto found = pageTiles_.find(page);
        if (found == pageTiles_.end()) return;
        for (const int id : found->second) {
            const auto tile = tiles_.find(id);
            if (tile == tiles_.end() || !usable(id, tile->second) || !tile->second.frame) continue;
            const Tile& t = tile->second;
            const FrameDesc* d = (t.desc.id == id) ? &t.desc : nullptr;
            if (!d) {
                const auto cat = catalog_.find(id);
                if (cat == catalog_.end()) continue;
                d = &cat->second;
            }
            ReadyTile ready{id, t.stamp, t.percent, t.hasGlyphs, &t.bitmap, t.key.empty() ? nullptr : &t.key, d};
            fn(ready);
        }
    }

    // plan.draw, plan.drop, plan.job 을 칸 캐시 규칙으로 다시 쓴다.
    // ready 가 비어 있으면 모든 쪽을 본다.
    void plan(ViewPlan& plan, const std::vector<Node>& pages, const std::unordered_set<int>& ready,
              const model::Document* doc = nullptr, bool screenCrewFastPath = false);

    // 한 쪽을 배율 percent 로 굽는다. focusY 가 0 이상이면 그 높이에 가까운 칸부터.
    // 한 번에 코어 수만큼 칸을 나누고, 코어가 8보다 적으면 8칸까지 모은다.
    FramePrepare prepare(const model::Document& doc, std::vector<Node>& pages, int page, int percent, int focusY);

    // 유휴 잡이면 가까운 쪽들의 칸을 한 큐에 넣고 스레드가 집어 간다. 화면 잡이거나 잡이 없으면 굽지 않는다.
    IdleStep prepareIdle(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan);

    // 편집 직후 현재 화면에 걸친 줄과 개체를 즉시 구워 깜빡임 없이 화면을 갱신한다.
    void bakeVisibleNow(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan);

 private:
    // 예산 관리자에 올리는 칸 캐시. 지금은 크기만 알리고 버리지는 않는다.
    class Pool final : public CachePool {
    public:
        explicit Pool(const FrameCache& owner) : owner_(owner) {}
        const char* name() const override { return "tiles"; }
        std::size_t bytes() const override { return owner_.bitmapBytes(); }
        std::size_t release(CacheTier, std::size_t) override { return 0; }

    private:
        const FrameCache& owner_;
    };

    struct QueuedFrame {
        FrameDesc frame;
        int page = 0;
        int percent = 100;
        int total = 0;
    };

    FramePrepare bake(const model::Document& doc, const std::vector<Node>& pages, std::vector<QueuedFrame> queued, bool more);
    struct Tile {
        int stamp = 0;
        int page = -1;
        bool frame = false;
        bool hasGlyphs = false;
        int percent = 0;
        std::string key;
        FrameDesc desc;
        PageBitmap bitmap;
    };

    int idFor(const std::string& key, int percent);
    bool usable(int id, const Tile& tile) const;

    struct CachedFrameEntry {
        FrameDesc baseDesc;
        std::string key;
    };

    struct CachedPageFrames {
        const Node* pageNode = nullptr;
        int widthHu = 0;
        int heightHu = 0;
        std::size_t childCount = 0;
        std::size_t firstLineChildren = 0;
        bool spansLater = false;
        std::uint64_t pageContentHash = 0;
        int basePixelH = 0;
        std::vector<CachedFrameEntry> entries;
        std::vector<std::size_t> byYOrder;
        std::vector<std::size_t> byHuYOrder;
        std::vector<std::int32_t> maxHuBottomPrefix;
    };

    struct RecycledTile {
        int oldId = 0;
        Tile tile;
    };

    std::shared_ptr<const CachedPageFrames> ensurePageFrames(int page, const Node& scene, const model::Document* doc);
    bool pageCatalogCurrent(int page, const Node& scene) const;
    void reconcilePageTiles(int page, const CachedPageFrames& built);
    void stashTileForRecycle(int id, Tile&& tile);
    bool tryAdoptRecycledTile(const FrameDesc& frame, int page, int percent);
    void flushRecycledTiles();

    mutable std::mutex idMu_;
    mutable std::mutex pageFramesMu_;

    std::unordered_map<int, Tile> tiles_;
    std::unordered_set<int> tileIds_;
    std::unordered_map<int, FrameDesc> catalog_;
    std::unordered_map<int, std::unordered_set<int>> pageTiles_;
    std::unordered_map<int, int> pageStamps_;
    std::unordered_map<int, std::shared_ptr<const CachedPageFrames>> pageFrames_;
    std::unordered_map<std::uint64_t, std::vector<RecycledTile>> recycledByHash_;
    std::unordered_set<int> recycledOldIds_;
    int recycledAge_ = 0;
    std::unordered_set<int> nonBaseTiles_;
    std::vector<int> transientCatalog_;
    std::vector<int> pendingDrop_;
    std::unordered_map<std::string, int> ids_;
    std::unordered_set<int> baseDone_;
    std::unordered_set<int> ready_;
    int stamp_ = 1;
    int nextId_ = 2000001;
    Pool pool_{*this};
};

}  // namespace noting::render
