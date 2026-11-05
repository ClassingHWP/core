#pragma once

// 쪽 합성 텍스처를 쓸지·구울지·버릴지 정하는 판단. 텍스처 자체(GPU 이미지)는 그리는 쪽이 같은 쪽 번호로 들고,
// 여기서 항목을 버리면 onDrop 으로 알려 준다. GPU 없이도 돌아서 네이티브 시험이 같은 판단을 탄다.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "chwp/render/cache_budget.hpp"
#include "chwp/render/paint.hpp"

namespace chwp::render {

struct PageTextureMeta {
    int page = -1;
    int w = 0;
    int h = 0;
    int layoutZoom = 0;
    int maskZoom = 0;
    int stamp = 0;
    // 글자 마스크가 덜 된 채 구웠다. 손이 멈추면 다시 굽는다.
    bool incomplete = false;
    std::uint32_t builtSeq = 0;
    // 그림을 거친 밉맵 단계로 합성했다. 그 뒤 그림 텍스처가 바뀌면 다시 합성한다.
    bool picturesCoarse = false;
    std::uint32_t pictureSeq = 0;
    std::size_t bytes = 0;
    std::uint64_t lastUsedTick = 0;
    std::uint64_t lastFrame = 0;
};

// 쓰려는 쪽의 지금 모양.
struct PageTextureWant {
    int w = 0;
    int h = 0;
    int layoutZoom = 0;
    int maskZoom = 0;
    int stamp = 0;
    // 끌기·확대 중이면 덜 된 텍스처라도 그대로 쓴다.
    bool interacting = false;
    std::uint32_t maskSeq = 0;
    std::uint32_t pictureSeq = 0;
};

inline constexpr std::size_t kPageTextureBudget = 192u * 1024u * 1024u;

class PageTextureCache final : public CachePool {
public:
    explicit PageTextureCache(std::function<void(int page)> onDrop = {}) : onDrop_(std::move(onDrop)) {}

    const char* name() const override { return "pageGpu"; }
    std::size_t bytes() const override { return bytes_; }
    std::size_t release(CacheTier tier, std::size_t need) override;

    // 이 크기·내용이면 쪽 텍스처로 구울 수 있는지. 너무 크거나 내용 번호가 없으면 굽지 않는다.
    static bool bakeable(int page, int w, int h, int stamp);
    // 남은 텍스처를 이번 모양에 그대로(또는 밉맵으로 줄여) 쓸 수 있는지.
    bool usable(int page, const PageTextureWant& want) const;
    // 배율은 달라도 내용이 같은 텍스처가 있는지. 확대·축소 중 늘려 쓴다.
    bool scalable(int page, int stamp) const;
    // scalable 텍스처를 구운 배율. 없으면 0.
    int zoomOf(int page, int stamp) const;
    bool has(int page) const { return entries_.count(page) != 0; }
    const PageTextureMeta* find(int page) const;

    // 새로 구운 텍스처를 넣는다. 같은 쪽의 옛 항목은 버린다. 예산을 넘으면 오래 안 본 쪽부터 비운다.
    // 한 장이 예산보다 크면 넣지 않고 거짓을 돌려준다(그리는 쪽은 그 이미지를 남기지 않는다).
    bool store(PageTextureMeta meta);
    // 이번 프레임에 썼다.
    void touch(int page);
    // 화면에 걸쳐 있어 이번 프레임 것으로 둔다. 예산이 버리지 않는다.
    void markFrame(int page);
    void setPicturesCoarse(int page, bool coarse);
    void drop(int page);
    void clear();
    // 예산 관리자에 이 캐시를 올린다. budget 은 처음 한 번만 쓴다.
    void ensureRegistered(std::size_t budget = kPageTextureBudget);

private:
    void erase(std::unordered_map<int, PageTextureMeta>::iterator it);

    std::unordered_map<int, PageTextureMeta> entries_;
    std::size_t bytes_ = 0;
    std::uint64_t tick_ = 0;
    bool registered_ = false;
    std::function<void(int)> onDrop_;
};

// 한 프레임에 쪽 텍스처를 새로 굽는 한도. 첫 장은 한도와 상관없이 굽는다. 끌기 중에는 덜 굽는다.
class PageBakeBudget {
public:
    explicit PageBakeBudget(bool dragging)
        : maxBakes_(dragging ? 24 : 64), maxPixels_(dragging ? 1800000 : 5400000) {}
    bool allows(int pixels) const { return bakes_ == 0 || (bakes_ < maxBakes_ && pixels_ + pixels <= maxPixels_); }
    void spend(int pixels) {
        ++bakes_;
        pixels_ += pixels;
    }

private:
    int bakes_ = 0;
    int pixels_ = 0;
    int maxBakes_ = 0;
    int maxPixels_ = 0;
};

// 이번 프레임 화면의 모양.
struct PaperFrame {
    int viewW = 0;
    int viewH = 0;
    int layoutZoom = 0;
    int maskZoom = 0;
    bool dragging = false;
    // GPU 표면이 쪽 텍스처를 쓸 수 있다. 2D 대체 화면은 칸을 바로 그린다.
    bool texturesOn = false;
    bool interacting = false;
    std::uint32_t maskSeq = 0;
    std::uint32_t pictureSeq = 0;
};

// 쪽 종이 한 장을 이번 프레임에 어떻게 그릴지. 위에서부터 해 보고, 못 하면 다음 것으로 간다.
struct PaperPlan {
    bool onScreen = false;
    int cacheW = 0;
    int cacheH = 0;
    int cacheZoom = 100;
    bool useTexture = false;    // 남은 쪽 텍스처를 그대로(또는 줄여) 그린다
    bool bake = false;          // 쪽 텍스처를 새로 굽는다
    bool scaleTexture = false;  // 배율이 다른 같은 내용 텍스처를 늘려 쓴다
};

PaperPlan planPaper(const PageTextureCache& cache, const ScreenSprite& paper, const PaperFrame& frame,
                    bool pageHasSprites, const PageBakeBudget& budget);

// 글자 마스크를 굽는 배율과 범위. 쪽을 통째로 굽는 프레임이면 화면 밖 글자도 굽는다(범위 0).
struct MaskPass {
    int layoutZoom = 0;
    int viewW = 0;
    int viewH = 0;
};
MaskPass maskPassFor(const std::vector<ScreenSprite>& papers, int layoutZoom, int viewW, int viewH);

// 쪽 텍스처로 구울 칸을 쪽마다 모은다. 구울 쪽이 없으면 비어 있다.
std::unordered_map<int, std::vector<const ScreenSprite*>> spritesByPage(const std::vector<ScreenSprite>& papers,
                                                                        const std::vector<ScreenSprite>& sprites);

}  // namespace chwp::render
