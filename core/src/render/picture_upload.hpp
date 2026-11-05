#pragma once

// 그림 화면 텍스처를 언제·어느 단계로 올릴지 정하는 판단. 텍스처와 표면(GPU)은 그리는 쪽이 같은 그림 id 로 들고,
// 여기서 장부 항목을 버리면 onDrop 으로 알려 준다. GPU 없이도 돌아서 네이티브 시험이 같은 판단을 탄다.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "chwp/render/cache_budget.hpp"

namespace chwp::render::paint_detail {

struct CachedPicture;

// 밉맵 단계 하나. 0 이 원본이다.
struct PictureLevel {
    int width = 0;
    int height = 0;
    std::size_t bytes = 0;
};

// 한 프레임에 올리는 그림 바이트. 재 보니 1MB 를 올리는 데 1ms 남짓 든다.
inline constexpr std::size_t kPictureUploadBudget = 4u << 20;
// 처음 보이는 그림은 한도를 넘어도 이만큼 작은 단계는 바로 올린다. 빈칸으로 두지 않는다.
inline constexpr std::size_t kPictureFirstLevelBytes = 1u << 20;
// 나눠 쓸 때 한 번에 쓰는 줄 수의 하한.
inline constexpr int kPictureFillMinRows = 16;

// 원본 src 범위(srcW×srcH 원본 픽셀)를 화면 want 픽셀 이상으로 그릴 수 있는 가장 거친 단계.
int wantedPictureLevel(const std::vector<PictureLevel>& levels, int pictureW, int pictureH, float srcW, float srcH,
                       float wantW, float wantH);
// level 단계부터 끝까지, 그 단계를 원본으로 올릴 때의 바이트.
std::size_t pictureLevelBytes(const std::vector<PictureLevel>& levels, int level);

struct PictureUploadPlan {
    int now = -1;   // 이번 프레임에 바로 올릴 단계. 없으면 -1
    int fill = -1;  // 프레임마다 나눠 쓸 단계. 없으면 -1
};

// 올린 단계(held, 없으면 -1)와 원하는 단계, 이번 프레임에 남은 바이트로 무엇을 올릴지 정한다.
// 한도에 들면 바로 올리고, 크면 나눠 쓰도록 세운다. 아무것도 안 올라간 그림은 1MB 안쪽의 가장 고운 단계를
// 한도와 상관없이 먼저 올린다.
PictureUploadPlan planPictureUpload(const std::vector<PictureLevel>& levels, int held, int want, std::size_t room);

// 프레임마다 새로 세는 업로드 한도.
class PictureUploadBudget {
public:
    void startFrame(std::uint64_t frame);
    std::size_t room() const { return used_ >= kPictureUploadBudget ? 0 : kPictureUploadBudget - used_; }
    bool spent() const { return used_ >= kPictureUploadBudget; }
    void spend(std::size_t bytes) { used_ += bytes; }

private:
    std::uint64_t frame_ = 0;
    std::size_t used_ = 0;
};

// 나눠 쓰는 단계 하나. 표면은 그리는 쪽이 같은 id 로 든다.
struct PictureFill {
    std::string id;
    std::shared_ptr<const CachedPicture> picture;
    int level = 0;
    int row = 0;
};

// 나눠 쓰기 차례. 같은 그림은 더 고운 단계로만 다시 세운다.
class PictureFillQueue {
public:
    // 새로 세웠거나 다시 세웠으면 참. 다시 세운 것은 쓰던 표면을 버려야 한다.
    bool queue(const std::string& id, const std::shared_ptr<const CachedPicture>& picture, int level);
    // 이번에 쓸 줄 수를 정하고 fill.row 와 예산을 앞으로 민다.
    static int step(PictureFill& fill, std::size_t rowBytes, int height, PictureUploadBudget& budget);
    std::vector<PictureFill>& fills() { return fills_; }
    bool empty() const { return fills_.empty(); }
    void clear() { fills_.clear(); }

private:
    std::vector<PictureFill> fills_;
};

// 화면에 올린 그림 텍스처의 장부. 이번 프레임에 쓴 것은 Visible, 나머지는 Recent 다.
class PictureTextureBook final : public CachePool {
public:
    explicit PictureTextureBook(std::function<void(const std::string&)> onDrop = {}) : onDrop_(std::move(onDrop)) {}

    const char* name() const override { return "pictureGpu"; }
    std::size_t bytes() const override;
    std::size_t release(CacheTier tier, std::size_t need) override;

    // 더 고운 단계일 때만 받는다. 받았으면 참이고, 그리는 쪽은 그 텍스처로 갈아 끼운다.
    bool keep(const std::string& id, int level, std::size_t bytes);
    // 올린 단계. 없으면 -1. 찾으면 이번 프레임에 쓴 것으로 둔다.
    int use(const std::string& id);
    // 텍스처를 바꿔 끼울 때마다 는다. 거친 그림으로 합성한 쪽 텍스처를 다시 만들 때 본다.
    std::uint32_t seq() const;
    void clear();

private:
    struct Entry {
        int level = 0;
        std::size_t bytes = 0;
        std::uint64_t lastFrame = 0;
    };
    mutable std::mutex mu_;
    std::unordered_map<std::string, Entry> entries_;
    std::size_t bytes_ = 0;
    std::uint32_t seq_ = 1;
    std::function<void(const std::string&)> onDrop_;
};

}  // namespace chwp::render::paint_detail
