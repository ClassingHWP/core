#pragma once

// 2D 화면 그림 목록. GPU 화면(WebGPU)을 못 쓰는 곳에서 화면을 플랫폼 2D 그리기(브라우저 Canvas 2D 등)로 그린다.
// 무엇을 어떤 차례로 그릴지, 어떤 그림을 셸에 올려 두고 언제 버릴지는 core 가 정한다.
// 셸은 목록을 차례대로 옮기기만 한다. 칸 그림은 한 번 올리면 번호로 다시 쓰므로 프레임마다 픽셀을 옮기지 않는다.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

#include "chwp/render/cache_budget.hpp"

namespace chwp::render {

enum class ScreenOpKind : std::int32_t {
    Save = 0,
    Restore = 1,
    Clip = 2,       // x,y,w,h 사각으로 자른다
    Fill = 3,       // 화면 전체를 color 로
    Rect = 4,       // x,y,w,h 를 color 로
    RoundRect = 5,  // size 가 모서리 반지름
    Image = 6,      // image 그림의 sx,sy,sw,sh 를 x,y,w,h 에
    Line = 7,       // (x,y)-(w,h) 를 굵기 size 로
    Path = 8,       // path[pathStart..+pathCount] 를 color 로 칠한다
};

// 그림 색을 뒤집는다(블록).
inline constexpr std::int32_t kScreenOpInvert = 1;
// 늘리고 줄일 때 이웃 픽셀을 섞는다. 없으면 가장 가까운 픽셀이다.
inline constexpr std::int32_t kScreenOpSmooth = 2;

// 한 줄 64바이트. 셸이 메모리를 그대로 읽는다(앞 네 칸은 정수, 뒤는 실수).
struct ScreenOp {
    std::int32_t kind = 0;
    std::uint32_t color = 0;  // ARGB, 곧은 알파
    std::int32_t flags = 0;
    std::int32_t image = 0;
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
    float sx = 0.f;
    float sy = 0.f;
    float sw = 0.f;
    float sh = 0.f;
    float size = 0.f;
    std::int32_t pathStart = 0;
    std::int32_t pathCount = 0;
    std::int32_t reserved = 0;
};
static_assert(sizeof(ScreenOp) == 64, "셸이 64바이트씩 읽는다");

// 경로는 실수 목록이다. 명령 하나 뒤에 점 좌표가 붙는다.
// Move x y, Line x y, Quad cx cy x y, Cubic c1x c1y c2x c2y x y, Close.
enum class ScreenPathVerb : int { Move = 0, Line = 1, Quad = 2, Cubic = 3, Close = 4 };

// 셸에 처음 올리는 그림. 픽셀은 곧은 알파 RGBA 다.
struct ScreenUpload {
    int key = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

struct ScreenFrame2D {
    int width = 0;
    int height = 0;
    std::vector<ScreenOp> ops;
    std::vector<float> path;
    // 이 프레임 그리기 전에 올릴 그림과 버릴 그림 번호.
    std::vector<ScreenUpload> uploads;
    std::vector<int> drops;
};

// 다른 스레드나 플랫폼 2D 캔버스가 받아 그린다. 못 그렸으면 거짓이다.
using ScreenOpSink = std::function<bool(const ScreenFrame2D&)>;

// 셸에 올려 둔 그림. 칸 번호(source)와 판(stamp)이 같으면 같은 그림이라 다시 올리지 않는다.
// 예산을 넘으면 이번 프레임에 안 쓴 것부터 오래된 차례로 버린다. 버린 번호는 다음 프레임에 셸에 알린다.
class ScreenBitmaps {
 public:
    ScreenBitmaps();
    ~ScreenBitmaps();
    ScreenBitmaps(const ScreenBitmaps&) = delete;
    ScreenBitmaps& operator=(const ScreenBitmaps&) = delete;

    // 이번 프레임에 쓴다. 처음이거나 판이 바뀌었으면 픽셀(Skia 처럼 미리 곱한 알파)을 곧은 알파로 옮겨 올릴 목록에 넣는다.
    int use(std::uint64_t source, int stamp, int width, int height, const std::uint8_t* premulRgba,
            std::size_t rowBytes);
    // 칸이 사라졌다. 셸에 그 그림을 버리라고 알린다.
    void forget(std::uint64_t source);
    // 모두 버린다. 셸에도 모두 버리라고 알린다.
    void clear();
    // 프레임을 닫는다. 예산을 지키고, 올릴 것·버릴 것을 frame 에 옮긴다.
    void finish(ScreenFrame2D& frame);
    std::size_t bytes() const { return bytes_; }
    std::size_t count() const { return entries_.size(); }

 private:
    struct Entry {
        int key = 0;
        int stamp = 0;
        std::size_t bytes = 0;
        std::uint64_t lastFrame = 0;
    };
    class Pool final : public CachePool {
     public:
        explicit Pool(ScreenBitmaps& owner) : owner_(owner) {}
        const char* name() const override { return "screenBitmaps"; }
        std::size_t bytes() const override { return owner_.bytes_; }
        std::size_t release(CacheTier tier, std::size_t need) override { return owner_.release(tier, need); }

     private:
        ScreenBitmaps& owner_;
    };

    std::size_t release(CacheTier tier, std::size_t need);
    void dropEntry(std::unordered_map<std::uint64_t, Entry>::iterator it);

    std::unordered_map<std::uint64_t, Entry> entries_;
    std::vector<ScreenUpload> uploads_;
    std::vector<int> drops_;
    std::size_t bytes_ = 0;
    int nextKey_ = 1;
    Pool pool_{*this};
};

// 2D 화면 그림 예산. 브라우저가 GPU 텍스처로 들고 있으니 칸 비트맵보다 작게 잡는다.
inline constexpr std::size_t kScreenBitmapBudget = std::size_t{128} << 20;

// 미리 곱한 알파 RGBA 를 곧은 알파로 옮긴다. 셸의 2D 그림(ImageData)은 곧은 알파다.
void unpremultiplyRgba(const std::uint8_t* src, std::size_t rowBytes, int width, int height, std::uint8_t* dst);

}  // namespace chwp::render
