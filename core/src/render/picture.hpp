#pragma once

// 그림 한 장이 화면에 오기까지의 단계를 한곳에 둔다.
//   원본 바이트 → 풀린 픽셀(CPU) → 화면 텍스처(GPU)
// 풀린 픽셀과 화면 텍스처는 예산 관리자에 따로 올라간다. 텍스처를 버려도 풀린 픽셀에서 다시 올리고,
// 풀린 픽셀을 버려도 다음에 쓸 때 원본에서 다시 푼다.
// 개체 상자 안에서 원본의 어느 부분을 어디에 그릴지(자르기)와 색조도 여기서 정한다.
// 칸에 굽는 Painter 와 화면에 바로 그리는 쪽이 같은 계산을 쓴다.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "noting/model/document.hpp"

#if NOTING_HAS_SKIA
#include "include/core/SkImage.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#endif

#if defined(__EMSCRIPTEN__) && NOTING_HAS_SKIA
class SkCanvas;
class SkColorFilter;
namespace skgpu::graphite {
class Recorder;
}
#endif

namespace noting::render::paint_detail {

// ---------------------------------------------------------------------------
// 풀린 그림 (CPU)

struct CachedPicture {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    bool failed = false;
    std::size_t bytes = 0;  // 예산에 세는 크기. 밉맵을 만들었으면 그만큼 더한다
    // 마지막으로 꺼내 간 캐시 프레임. 버릴 때 오래된 것부터 고르는 데 쓴다.
    mutable std::atomic<std::uint64_t> lastFrame{0};
#if NOTING_HAS_SKIA
    sk_sp<SkImage> skImage;  // rgba 를 빌려 쓰는 래스터 이미지. 밉맵이 붙어 있다
    // 0 은 rgba, 그 뒤는 밉맵 단계. 픽셀은 rgba 와 skImage 의 밉맵이 쥔다.
    std::vector<SkPixmap> levels;
#endif
};

// 없으면 원본에서 푼다. 쓰는 동안 쥐고 있어야 한다. 예산 관리자가 캐시에서 빼도 쥔 쪽이 다 쓸 때까지 남는다.
std::shared_ptr<const CachedPicture> pictureFor(const model::Document& doc, const std::string& id);
// 이미 풀린 그림만 돌려준다. 풀지 않는다.
std::shared_ptr<const CachedPicture> peekPicture(const std::string& id);
// 풀린 그림과 화면 텍스처를 모두 버린다. 문서를 바꿀 때 쓴다.
void clearPictures();
// 화면 루프가 프레임마다 부른다. 그림 캐시를 예산 관리자에 올리고 예산을 지킨다.
// 프레임당 텍스처 업로드 한도도 여기서 새로 센다.
void keepPictureBudget();

// ---------------------------------------------------------------------------
// 화면 텍스처 (GPU)

#if defined(__EMSCRIPTEN__) && NOTING_HAS_SKIA
// 올린 텍스처와, 원본 픽셀 좌표를 그 텍스처 좌표로 바꾸는 배율.
struct PictureTextureView {
    sk_sp<SkImage> image;
    float scaleX = 1.f;
    float scaleY = 1.f;
    bool exact = true;  // 거짓이면 원하는 단계가 아직 안 올라가 거친 단계를 쓴다
};

// 화면 스레드에서만 부른다. 원본의 src 범위를 화면 want 픽셀로 그리는 데 충분한 밉맵 단계를 올린다.
// 화면보다 큰 원본은 다 올리지 않는다. 한 프레임에 올리는 양에 한도가 있어서,
// 넘치면 거친 단계를 먼저 쓰고 다음 프레임에 고운 단계로 바꾼다. 2D 화면이면 래스터 이미지를 준다.
PictureTextureView pictureTexture(skgpu::graphite::Recorder* recorder, const std::string& id,
                                  const std::shared_ptr<const CachedPicture>& picture, float wantW, float wantH,
                                  const SkRect& src);
// 프레임 앞에서 한 번 부른다. 나눠 쓰는 큰 단계를 한도만큼 이어 쓰고, 다 쓴 것은 텍스처로 바꿔 끼운다.
void advancePictureUploads(skgpu::graphite::Recorder* recorder);
// 나눠 쓰는 중인 단계가 있다. 화면은 다음 프레임을 그려야 한다.
bool pictureUploadsWaiting();
// 텍스처를 바꿔 끼울 때마다 는다. 거친 그림으로 합성한 쪽 텍스처를 다시 만들 때 본다.
std::uint32_t pictureTextureSeq();
#endif

// ---------------------------------------------------------------------------
// 자르기와 색조

// 원본 크기와 그 안에서 남길 범위. HU 다.
struct PictureCrop {
    std::int32_t originW = 0;
    std::int32_t originH = 0;
    std::int32_t clipL = 0;
    std::int32_t clipT = 0;
    std::int32_t clipR = 0;
    std::int32_t clipB = 0;
};

PictureCrop pictureCrop(const model::PictureView& view);

// 원본에서 그릴 부분과, 그 부분이 안쪽 상자(여백을 뺀 개체 상자)에서 차지하는 비율.
// 자르기가 원본 밖으로 나가면 그 바깥은 비워 두고 안쪽 부분만 그 자리에 그린다.
struct PictureSource {
    double srcL = 0;
    double srcT = 0;
    double srcR = 0;
    double srcB = 0;
    double fracL = 0;
    double fracT = 0;
    double fracR = 1;
    double fracB = 1;
};

// 거짓이면 남은 부분이 없어 그릴 것이 없다. 자르지 않았으면 원본 전체, 비율 0..1 이다.
bool cropSource(const PictureCrop& crop, int imageW, int imageH, PictureSource& out);

// 밝기·명암은 채널마다, 회색조·흑백은 먼저 밝기로 모은 뒤에 적용한다. 0 은 불투명이다.
struct PictureTone {
    int effect = 0;
    int bright = 0;
    int contrast = 0;
    int transparency = 0;
    bool on() const { return effect == 1 || effect == 2 || bright != 0 || contrast != 0 || transparency != 0; }
};

PictureTone pictureTone(const model::PictureView& view);

void applyPictureTone(std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a, const PictureTone& tone);

#if NOTING_HAS_SKIA
void paintPictureTone(SkPaint& paint, const PictureTone* tone);
#endif

// Painter 가 그림 한 장을 칸에 그릴 때 넘기는 원본 범위와 색조.
struct ImageDraw {
    double srcL = 0;
    double srcT = 0;
    double srcR = 0;
    double srcB = 0;
    const PictureTone* tone = nullptr;
};

#if NOTING_HAS_SKIA
SkRect pictureSrc(int sw, int sh, const ImageDraw* draw);
#endif

// ---------------------------------------------------------------------------
// 화면에서 바로 그리는 그림

#if defined(__EMSCRIPTEN__) && NOTING_HAS_SKIA
// 텍스처 한 변 한도. WebGPU 기본 한도다. 넘는 그림은 예전처럼 칸에 굽는다.
inline constexpr int kMaxPictureTexture = 8192;

// 개체 칸을 구울 때 픽셀로 그리지 않고 남겨 두는 그림 한 장. 좌표는 칸 비트맵 픽셀(구운 배율)이다.
// 화면은 칸 비트맵보다 먼저 이것을 텍스처로 그린다. 칸의 테두리는 비트맵에 남아 그림 위에 얹힌다.
struct FramePicture {
    std::string id;
    std::shared_ptr<const CachedPicture> picture;
    SkRect dst = SkRect::MakeEmpty();   // 그림이 들어갈 자리
    SkRect clip = SkRect::MakeEmpty();  // 조상 개체가 자르는 범위. 자르지 않으면 칸 전체다
    SkRect src = SkRect::MakeEmpty();   // 원본 픽셀
    bool flipH = false;
    bool flipV = false;
    PictureTone tone;
};

// 칸 id 별로 남긴 그림. 칸을 구울 때마다 갈아 끼우고, 칸을 버리면 함께 버린다.
void storeFramePictures(int id, std::vector<FramePicture> pictures);
void dropFramePictures(int id);
void clearFramePictures();
bool frameHasPictures(int id);
// 재활용한 칸이 새 id 를 받을 때 그림도 옮긴다. 옮길 그림이 없으면 거짓.
bool cloneFramePictures(int srcId, int dstId);
std::shared_ptr<const std::vector<FramePicture>> framePictures(int id);

// 칸이 화면의 spriteDst 에 sx, sy 배율로 놓였을 때 그 칸의 그림을 그린다.
// backs 는 선택으로 뒤집은 바탕이다. 겹친 부분은 색을 뒤집어 다시 그린다.
// 텍스처를 하나라도 못 만들었으면 거짓이다.
bool drawFramePictures(SkCanvas* canvas, skgpu::graphite::Recorder* recorder,
                       const std::vector<FramePicture>& pictures, const SkRect& spriteDst, float sx, float sy,
                       bool dragging, const std::vector<SkRect>& backs, sk_sp<SkColorFilter>& inverted);
#endif

}  // namespace noting::render::paint_detail
