#include "render/picture_upload.hpp"

#include <algorithm>
#include <utility>

namespace chwp::render::paint_detail {

int wantedPictureLevel(const std::vector<PictureLevel>& levels, int pictureW, int pictureH, float srcW, float srcH,
                       float wantW, float wantH) {
    if (pictureW <= 0 || pictureH <= 0) return 0;
    int best = 0;
    for (int i = 1; i < static_cast<int>(levels.size()); ++i) {
        const PictureLevel& level = levels[static_cast<std::size_t>(i)];
        const float spanW = srcW * static_cast<float>(level.width) / static_cast<float>(pictureW);
        const float spanH = srcH * static_cast<float>(level.height) / static_cast<float>(pictureH);
        if (spanW + 0.5f < wantW || spanH + 0.5f < wantH) break;
        best = i;
    }
    return best;
}

std::size_t pictureLevelBytes(const std::vector<PictureLevel>& levels, int level) {
    std::size_t total = 0;
    for (std::size_t i = static_cast<std::size_t>(std::max(0, level)); i < levels.size(); ++i) total += levels[i].bytes;
    return total;
}

PictureUploadPlan planPictureUpload(const std::vector<PictureLevel>& levels, int held, int want, std::size_t room) {
    PictureUploadPlan plan;
    if (levels.empty()) return plan;
    if (held >= 0 && held <= want) return plan;
    if (pictureLevelBytes(levels, want) <= room) {
        plan.now = want;
        return plan;
    }
    plan.fill = want;
    if (held < 0) {
        int coarse = static_cast<int>(levels.size()) - 1;
        for (int i = want; i < static_cast<int>(levels.size()); ++i) {
            if (pictureLevelBytes(levels, i) <= kPictureFirstLevelBytes) {
                coarse = i;
                break;
            }
        }
        plan.now = coarse;
    }
    return plan;
}

void PictureUploadBudget::startFrame(std::uint64_t frame) {
    if (frame == frame_) return;
    frame_ = frame;
    used_ = 0;
}

bool PictureFillQueue::queue(const std::string& id, const std::shared_ptr<const CachedPicture>& picture, int level) {
    for (PictureFill& fill : fills_) {
        if (fill.id != id) continue;
        if (fill.level <= level) return false;
        fill = PictureFill{id, picture, level, 0};
        return true;
    }
    fills_.push_back(PictureFill{id, picture, level, 0});
    return true;
}

int PictureFillQueue::step(PictureFill& fill, std::size_t rowBytes, int height, PictureUploadBudget& budget) {
    const int left = height - fill.row;
    if (left <= 0) return 0;
    const int room = static_cast<int>(budget.room() / std::max<std::size_t>(rowBytes, 1));
    const int rows = std::min(std::max(room, kPictureFillMinRows), left);
    fill.row += rows;
    budget.spend(static_cast<std::size_t>(rows) * rowBytes);
    return rows;
}

std::size_t PictureTextureBook::bytes() const {
    std::lock_guard lock(mu_);
    return bytes_;
}

std::size_t PictureTextureBook::release(CacheTier tier, std::size_t need) {
    std::vector<std::string> dropped;
    std::size_t freed = 0;
    {
        std::vector<std::pair<std::uint64_t, std::string>> order;
        std::lock_guard lock(mu_);
        for (const auto& [id, entry] : entries_) {
            if (tierForFrame(entry.lastFrame) == tier) order.emplace_back(entry.lastFrame, id);
        }
        std::sort(order.begin(), order.end());
        for (const auto& [last, id] : order) {
            if (freed >= need) break;
            const auto found = entries_.find(id);
            if (found == entries_.end()) continue;
            freed += found->second.bytes;
            bytes_ -= std::min(bytes_, found->second.bytes);
            entries_.erase(found);
            dropped.push_back(id);
        }
    }
    if (onDrop_) {
        for (const std::string& id : dropped) onDrop_(id);
    }
    return freed;
}

bool PictureTextureBook::keep(const std::string& id, int level, std::size_t bytes) {
    std::lock_guard lock(mu_);
    const auto found = entries_.find(id);
    if (found != entries_.end() && found->second.level <= level) return false;
    Entry& entry = entries_[id];
    bytes_ -= std::min(bytes_, entry.bytes);
    entry.level = level;
    entry.bytes = bytes;
    entry.lastFrame = cacheFrame();
    bytes_ += bytes;
    ++seq_;
    return true;
}

int PictureTextureBook::use(const std::string& id) {
    std::lock_guard lock(mu_);
    const auto found = entries_.find(id);
    if (found == entries_.end()) return -1;
    found->second.lastFrame = cacheFrame();
    return found->second.level;
}

std::uint32_t PictureTextureBook::seq() const {
    std::lock_guard lock(mu_);
    return seq_;
}

void PictureTextureBook::clear() {
    std::vector<std::string> dropped;
    {
        std::lock_guard lock(mu_);
        for (const auto& item : entries_) dropped.push_back(item.first);
        entries_.clear();
        bytes_ = 0;
    }
    if (onDrop_) {
        for (const std::string& id : dropped) onDrop_(id);
    }
}

}  // namespace chwp::render::paint_detail
