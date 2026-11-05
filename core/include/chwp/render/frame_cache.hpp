#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "chwp/model/document.hpp"
#include "chwp/render/cache_budget.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/view.hpp"

namespace chwp::render {

// 칸 번호는 여기서부터 준다. 화면 조각(ViewCamera) 번호와 겹치지 않는다.
inline constexpr int kFirstFrameId = 2000001;
// 화면 칸 캐시와 같은 프로세스에 있을 수 있는 다른 칸 캐시(agent 쪽 그림)의 첫 번호. 칸 번호로 찾는 전역 저장소가 겹치지 않게 띄운다.
inline constexpr int kAgentFirstFrameId = 1 << 28;

// 칸 비트맵 예산. GPU 화면은 글자를 칸에 굽지 않아 문서 하나가 이보다 훨씬 작다.
// 2D 화면은 글자까지 칸에 구워 긴 문서는 넘는다. 넘으면 화면에서 먼 쪽부터 버린다.
inline constexpr std::size_t kTileCacheBudget = std::size_t{192} << 20;

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

// 글자 마스크를 찍는 픽셀 크기(ppem). 화면에 보이는 크기 그대로 찍고, 아틀라스 한 장(2048) 안에 들도록
// 1024 에서 멈춘다. 큰 제목을 크게 확대하면 256 을 넘는데, 거기서 멈추면 작은 마스크를 늘려 그려 글자가 흐리고 깨진다.
inline constexpr int kMaxGlyphMaskPpem = 1024;
inline int glyphMaskPpem(int ppem) { return ppem < 1 ? 1 : (ppem > kMaxGlyphMaskPpem ? kMaxGlyphMaskPpem : ppem); }

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
    explicit FrameCache(int firstId);
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
    // 칸 목록·번호표·쪽별 칸 목록이 쓰는 바이트(어림값). 메모리 보고용.
    std::size_t catalogBytes() const;
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

    // 지금 화면에 걸친 칸(줄·개체) 중 아직 없는 것을 화면 위쪽부터 maxTiles 개까지 굽는다.
    // 잡과 상관없이 plan 의 스크롤 자리만 본다. 스크롤 중에 새로 들어온 쪽을 채울 때 쓴다.
    FramePrepare prepareView(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan, int maxTiles);

    // 편집 직후 현재 화면에 걸친 줄과 개체를 즉시 구워 깜빡임 없이 화면을 갱신한다.
    void bakeVisibleNow(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan);

    // 칸 예산. 화면에 보이는 쪽(first..last)에서 먼 쪽부터 그 쪽 칸을 통째로 버린다.
    // 예산을 넘으면 미리 굽는 반경을 줄이고, 쓰는 바이트가 예산의 절반 아래로 내려가면 반경을 다시 푼다.
    void setFocus(int firstPage, int lastPage);
    // 보이는 쪽에서 page 까지 떨어진 쪽 수. 보이는 쪽이면 0.
    int focusGap(int page) const;
    // 미리 굽는 반경. 음수면 끝이 없다.
    int keepRadius() const { return keepRadius_; }
    bool withinKeep(int page) const { return keepRadius_ < 0 || focusGap(page) <= keepRadius_; }
    // 예산을 지킨다. 반경이 바뀌면 참이다.
    bool keepWithinBudget();
    // 예산 때문에 칸을 버린 쪽. 일꾼이 그 쪽을 다시 훑게 한다.
    std::vector<int> takeEvictedPages();

 private:
    // 예산 관리자에 올리는 칸 캐시. Spare 는 재활용 칸, Recent 는 반경 밖 쪽,
    // Near 는 반경을 줄여 가며 먼 쪽을 버린다. 보이는 쪽과 바로 옆 쪽은 남긴다.
    class Pool final : public CachePool {
    public:
        explicit Pool(FrameCache& owner) : owner_(owner) {}
        const char* name() const override { return "tiles"; }
        std::size_t bytes() const override { return owner_.bitmapBytes(); }
        std::size_t release(CacheTier tier, std::size_t need) override { return owner_.releaseTiles(tier, need); }

    private:
        FrameCache& owner_;
    };

    std::size_t releaseTiles(CacheTier tier, std::size_t need);
    std::size_t evictPage(int page);

    struct QueuedFrame {
        FrameDesc frame;
        int page = 0;
        int percent = 100;
        int total = 0;
    };

    FramePrepare bake(const model::Document& doc, const std::vector<Node>& pages, std::vector<QueuedFrame> queued, bool more);
    // 화면과 그 둘레 margin 안의 쪽에서 없는 칸을 화면에 가까운 차례로 budget 개까지 모은다.
    // 화면 밖 줄 칸은 넣지 않는다. baseNear 면 100% 칸을 모은다.
    void queueNear(const model::Document& doc, std::vector<Node>& pages, const ViewPlan& plan, int percent, bool baseNear,
                   int margin, int budget, std::vector<QueuedFrame>& queued, bool& more);
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
    std::vector<int> takePendingDrops();
    std::unordered_map<std::string, int> ids_;
    std::unordered_set<int> baseDone_;
    std::unordered_set<int> ready_;
    int stamp_ = 1;
    int nextId_ = kFirstFrameId;
    int focusFirst_ = -1;
    int focusLast_ = -1;
    int keepRadius_ = -1;
    std::vector<int> evictedPages_;
    Pool pool_{*this};
};

// 쪽 하나에 구워 둔 칸 하나를 쪽 픽셀 좌표로 둔 것. 화면(세션)과 쪽 그림(renderPageGpu)이 같은 목록을 쓴다.
struct PageSprite {
    int id = 0;
    int stamp = 0;
    int frameX = 0;
    int frameY = 0;
    int frameW = 0;
    int frameH = 0;
    int srcPixelH = 0;
    int localX = 0;
    int localY = 0;
    int w = 0;
    int h = 0;
    int sw = 0;
    int sh = 0;
    int percent = 100;
    bool nearest = false;
    const std::uint8_t* rgba = nullptr;
    int layer = 1;
    std::int64_t z = 0;
    int rank = 1;
    std::int32_t huX = 0;
    std::int32_t huY = 0;
    std::int32_t huW = 0;
    std::int32_t huH = 0;
};

// 그 쪽에 구운 칸을 그릴 차례(층·z·순번·자리)로 낸다. 같은 칸이 여러 배율로 있으면 targetPercent 에 가까운 것 하나다.
// multiPercent 가 있으면 배율이 섞였는지 적는다.
std::vector<PageSprite> readyPageSprites(const FrameCache& frames, int page, int targetPercent,
                                         bool* multiPercent = nullptr);
// 칸 조각을 spriteZoom 배율, 쪽 높이 dstPixelH 픽셀에 놓는다(localX·localY·w·h). 구운 배율이 다르면 HU 자리로 다시 잰다.
void placePageSprites(std::vector<PageSprite>& sprites, int spriteZoom, int dstPixelH, std::int32_t pageHeightHu);

}  // namespace chwp::render
