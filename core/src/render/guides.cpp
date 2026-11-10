#include "chwp/render/guides.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <variant>

#include "chwp/render/scene.hpp"

namespace chwp::render {

namespace {

constexpr double kHuPerMm = 7200.0 / 25.4;
// 한글 격자 설정 대화 상자의 범위(0.01mm).
constexpr int kSpanMin = 100;
constexpr int kSpanMax = 2500;
constexpr int kMagnetMax = 2500;
constexpr int kOriginMax = 55799;

struct GuideState {
    std::mutex mu;
    ViewGuides value;
    std::uint32_t seq = 1;
};

GuideState& guideState() {
    static GuideState state;
    return state;
}

const FormatValue* fieldOf(const FormatFields& fields, const char* key) {
    for (const auto& [name, value] : fields) {
        if (name == key) return &value;
    }
    return nullptr;
}

std::optional<bool> boolOf(const FormatFields& fields, const char* key) {
    const FormatValue* value = fieldOf(fields, key);
    if (!value) return std::nullopt;
    if (const bool* b = std::get_if<bool>(value)) return *b;
    if (const double* d = std::get_if<double>(value)) return *d != 0;
    if (const std::string* s = std::get_if<std::string>(value)) return *s == "true" || *s == "1";
    return std::nullopt;
}

std::optional<double> numberOf(const FormatFields& fields, const char* key) {
    const FormatValue* value = fieldOf(fields, key);
    if (!value) return std::nullopt;
    if (const double* d = std::get_if<double>(value)) return std::isfinite(*d) ? std::optional<double>(*d) : std::nullopt;
    if (const std::string* s = std::get_if<std::string>(value)) {
        try {
            std::size_t used = 0;
            const double d = std::stod(*s, &used);
            if (used > 0 && std::isfinite(d)) return d;
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::optional<std::string> textOf(const FormatFields& fields, const char* key) {
    const FormatValue* value = fieldOf(fields, key);
    if (!value) return std::nullopt;
    if (const std::string* s = std::get_if<std::string>(value)) return *s;
    return std::nullopt;
}

// mm 값을 0.01mm 로. 대화 상자 둘째 자리에서 반올림한다.
int hundredths(double mm) { return static_cast<int>(std::lround(mm * 100.0)); }

const char* kindName(GridKind kind) {
    switch (kind) {
        case GridKind::Dots: return "DOTS";
        case GridKind::Horizontal: return "HORIZONTAL";
        case GridKind::Vertical: return "VERTICAL";
        case GridKind::Both: return "BOTH";
    }
    return "BOTH";
}

const char* snapName(GridSnap snap) {
    switch (snap) {
        case GridSnap::Free: return "FREE";
        case GridSnap::Magnet: return "MAGNET";
        case GridSnap::Only: return "ONLY";
    }
    return "FREE";
}

// 쪽 텍스처가 품는 격자 모양만 견준다. 안내선은 텍스처 위에 그려 견주지 않는다.
bool gridDrawsSame(const GridSettings& g, const GridSettings& h) {
    return g.show == h.show && g.kind == h.kind && g.front == h.front && g.spanX == h.spanX && g.spanY == h.spanY &&
           g.fromPaper == h.fromPaper && g.originX == h.originX && g.originY == h.originY;
}

bool sameSettings(const ViewGuides& a, const ViewGuides& b) {
    const GridSettings& g = a.grid;
    const GridSettings& h = b.grid;
    const GuideSettings& p = a.guide;
    const GuideSettings& q = b.guide;
    return gridDrawsSame(g, h) && g.snap == h.snap && g.magnetX == h.magnetX && g.magnetY == h.magnetY &&
           p.drawShow == q.drawShow && p.drawX == q.drawX && p.drawY == q.drawY && p.moveShow == q.moveShow &&
           p.dashed == q.dashed && p.alignOthers == q.alignOthers && p.fromPaper == q.fromPaper &&
           p.magnet == q.magnet && p.tipPosition == q.tipPosition && p.tipSize == q.tipSize && a.marks == b.marks;
}

}  // namespace

ViewGuides viewGuides() {
    GuideState& state = guideState();
    std::lock_guard lock(state.mu);
    return state.value;
}

std::uint32_t viewGuidesSeq() {
    GuideState& state = guideState();
    std::lock_guard lock(state.mu);
    return state.seq;
}

ViewMarks viewMarks() {
    GuideState& state = guideState();
    std::lock_guard lock(state.mu);
    return state.value.marks;
}

FormatFields viewMarkFields(const ViewMarks& marks) {
    return {
        {"paraMarks", marks.paragraphMarks()},
        {"paraMarksOwn", marks.paragraph},
        {"ctrlMarks", marks.control},
        {"transparentLines", marks.transparent},
        {"memos", marks.memo},
        {"proofMarks", marks.proof},
    };
}

ViewMarks patchViewMarks(const ViewMarks& base, const FormatFields& patch) {
    ViewMarks out = base;
    if (const auto toggle = textOf(patch, "toggle")) {
        if (*toggle == "paraMarks") {
            if (out.control) {
                out.control = false;
                out.paragraph = false;
            } else {
                out.paragraph = !out.paragraph;
            }
        } else if (*toggle == "ctrlMarks") {
            out.control = !out.control;
        } else if (*toggle == "transparentLines") {
            out.transparent = !out.transparent;
        } else if (*toggle == "memos") {
            out.memo = !out.memo;
        } else if (*toggle == "proofMarks") {
            out.proof = !out.proof;
        }
    }
    if (const auto v = boolOf(patch, "paraMarks")) out.paragraph = *v;
    if (const auto v = boolOf(patch, "paraMarksOwn")) out.paragraph = *v;
    if (const auto v = boolOf(patch, "ctrlMarks")) out.control = *v;
    if (const auto v = boolOf(patch, "transparentLines")) out.transparent = *v;
    if (const auto v = boolOf(patch, "memos")) out.memo = *v;
    if (const auto v = boolOf(patch, "proofMarks")) out.proof = *v;
    return out;
}

double hundredthMmToHu(int value) { return static_cast<double>(value) / 100.0 * kHuPerMm; }

int huToHundredthMm(double hu) { return static_cast<int>(std::lround(hu / kHuPerMm * 100.0)); }

PaperBox paperBoxOf(const Node& paper) {
    PaperBox box;
    box.width = paper.width;
    box.height = paper.height;
    box.bodyLeft = paper.marginLeft;
    box.bodyTop = paper.marginTop;
    box.bodyRight = paper.width - paper.marginRight;
    box.bodyBottom = paper.height - paper.marginBottom;
    // 본문 노드(머리말·꼬리말·바탕쪽이 아닌 것)가 본문 영역이다.
    for (const Node& child : paper.children) {
        if (child.kind != NodeKind::Body || child.pageBand || child.fromMaster || !child.role.empty()) continue;
        box.bodyLeft = child.x;
        box.bodyTop = child.y;
        box.bodyRight = child.x + child.width;
        box.bodyBottom = child.y + child.height;
        break;
    }
    return box;
}

FormatFields viewGuideFields(const ViewGuides& guides) {
    const GridSettings& g = guides.grid;
    const GuideSettings& p = guides.guide;
    const auto mm = [](int value) { return static_cast<double>(value) / 100.0; };
    FormatFields out = {
        {"gridShow", g.show},
        {"gridKind", std::string(kindName(g.kind))},
        {"gridFront", g.front},
        {"gridSnap", std::string(snapName(g.snap))},
        {"gridSpanX", mm(g.spanX)},
        {"gridSpanY", mm(g.spanY)},
        {"gridBase", std::string(g.fromPaper ? "PAPER" : "PAGE")},
        {"gridOriginX", mm(g.originX)},
        {"gridOriginY", mm(g.originY)},
        {"gridMagnetX", mm(g.magnetX)},
        {"gridMagnetY", mm(g.magnetY)},
        {"drawGuides", p.drawShow},
        {"moveGuides", p.moveShow},
        {"moveDashed", p.dashed},
        {"moveOthers", p.alignOthers},
        {"moveBase", std::string(p.fromPaper ? "PAPER" : "PAGE")},
        {"moveMagnet", p.magnet},
        {"moveTipPosition", p.tipPosition},
        {"moveTipSize", p.tipSize},
    };
    for (auto& field : viewMarkFields(guides.marks)) out.push_back(std::move(field));
    return out;
}

ViewGuides patchViewGuides(const ViewGuides& base, const FormatFields& patch, const PaperBox* box) {
    ViewGuides out = base;
    GridSettings& g = out.grid;
    GuideSettings& p = out.guide;
    if (const auto toggle = textOf(patch, "toggle")) {
        if (*toggle == "grid") g.show = !g.show;
        else if (*toggle == "drawGuides") p.drawShow = !p.drawShow;
        else if (*toggle == "moveGuides") p.moveShow = !p.moveShow;
    }
    if (const auto v = boolOf(patch, "gridShow")) g.show = *v;
    if (const auto v = textOf(patch, "gridKind")) {
        if (*v == "DOTS") g.kind = GridKind::Dots;
        else if (*v == "HORIZONTAL") g.kind = GridKind::Horizontal;
        else if (*v == "VERTICAL") g.kind = GridKind::Vertical;
        else if (*v == "BOTH") g.kind = GridKind::Both;
    }
    if (const auto v = boolOf(patch, "gridFront")) g.front = *v;
    if (const auto v = textOf(patch, "gridSnap")) {
        if (*v == "FREE") g.snap = GridSnap::Free;
        else if (*v == "MAGNET") g.snap = GridSnap::Magnet;
        else if (*v == "ONLY") g.snap = GridSnap::Only;
    }
    if (const auto v = numberOf(patch, "gridSpanX")) g.spanX = std::clamp(hundredths(*v), kSpanMin, kSpanMax);
    if (const auto v = numberOf(patch, "gridSpanY")) g.spanY = std::clamp(hundredths(*v), kSpanMin, kSpanMax);
    const auto originX = numberOf(patch, "gridOriginX");
    const auto originY = numberOf(patch, "gridOriginY");
    if (const auto v = textOf(patch, "gridBase")) {
        const bool paper = *v == "PAPER";
        if (paper != g.fromPaper && box && !originX && !originY) {
            // 같은 시작점을 새 기준으로 다시 잰다(한글은 쪽 0mm 를 종이로 바꾸면 본문 왼쪽 위 자리를 적는다).
            const int bodyX = huToHundredthMm(box->bodyLeft);
            const int bodyY = huToHundredthMm(box->bodyTop);
            g.originX += paper ? bodyX : -bodyX;
            g.originY += paper ? bodyY : -bodyY;
        }
        g.fromPaper = paper;
    }
    if (originX) g.originX = hundredths(*originX);
    if (originY) g.originY = hundredths(*originY);
    g.originX = std::clamp(g.originX, g.fromPaper ? 0 : -kOriginMax, kOriginMax);
    g.originY = std::clamp(g.originY, g.fromPaper ? 0 : -kOriginMax, kOriginMax);
    if (const auto v = numberOf(patch, "gridMagnetX")) g.magnetX = std::clamp(hundredths(*v), 0, kMagnetMax);
    if (const auto v = numberOf(patch, "gridMagnetY")) g.magnetY = std::clamp(hundredths(*v), 0, kMagnetMax);
    if (const auto v = boolOf(patch, "drawGuides")) p.drawShow = *v;
    if (const auto v = boolOf(patch, "moveGuides")) p.moveShow = *v;
    if (const auto v = boolOf(patch, "moveDashed")) p.dashed = *v;
    if (const auto v = boolOf(patch, "moveOthers")) p.alignOthers = *v;
    if (const auto v = textOf(patch, "moveBase")) p.fromPaper = *v == "PAPER";
    if (const auto v = boolOf(patch, "moveMagnet")) p.magnet = *v;
    if (const auto v = boolOf(patch, "moveTipPosition")) p.tipPosition = *v;
    if (const auto v = boolOf(patch, "moveTipSize")) p.tipSize = *v;
    out.marks = patchViewMarks(base.marks, patch);
    return out;
}

bool setViewGuides(const ViewGuides& next) {
    GuideState& state = guideState();
    std::lock_guard lock(state.mu);
    if (sameSettings(state.value, next)) return false;
    const bool regrid = !gridDrawsSame(state.value.grid, next.grid);
    state.value = next;
    if (regrid) ++state.seq;
    return true;
}

GridPlan gridPlanFor(const GridSettings& grid, const PaperBox& box) {
    GridPlan plan;
    plan.show = grid.show && box.width > 0 && box.height > 0;
    plan.kind = grid.kind;
    plan.front = grid.front;
    plan.spanX = hundredthMmToHu(grid.spanX);
    plan.spanY = hundredthMmToHu(grid.spanY);
    const double baseX = grid.fromPaper ? 0.0 : static_cast<double>(box.bodyLeft);
    const double baseY = grid.fromPaper ? 0.0 : static_cast<double>(box.bodyTop);
    plan.originX = baseX + hundredthMmToHu(grid.originX);
    plan.originY = baseY + hundredthMmToHu(grid.originY);
    if (grid.fromPaper) {
        plan.left = 0;
        plan.top = 0;
        plan.right = box.width;
        plan.bottom = box.height;
    } else {
        // 쪽 기준은 본문 영역에 깐다. 시작점을 여백으로 옮겼으면(음수) 그 자리부터 깐다.
        plan.left = std::min<std::int32_t>(box.bodyLeft, static_cast<std::int32_t>(std::floor(plan.originX)));
        plan.top = std::min<std::int32_t>(box.bodyTop, static_cast<std::int32_t>(std::floor(plan.originY)));
        plan.left = std::max<std::int32_t>(plan.left, 0);
        plan.top = std::max<std::int32_t>(plan.top, 0);
        plan.right = box.bodyRight;
        plan.bottom = box.bodyBottom;
    }
    if (plan.right <= plan.left || plan.bottom <= plan.top || plan.spanX <= 0 || plan.spanY <= 0) plan.show = false;
    return plan;
}

std::vector<double> gridStops(double origin, double span, double from, double to) {
    std::vector<double> out;
    if (span <= 0 || to < from) return out;
    // origin + k*span 이 from 이상인 가장 작은 k 부터.
    const double first = std::ceil((from - origin) / span - 1e-9);
    for (double k = first;; k += 1.0) {
        const double at = origin + k * span;
        if (at > to + 1e-6) break;
        if (at >= from - 1e-6) out.push_back(at);
        if (out.size() > 100000) break;
    }
    return out;
}

std::int32_t snapToGrid(const GridSettings& grid, const PaperBox& box, std::int32_t value, bool horizontal) {
    if (!grid.show || grid.snap == GridSnap::Free) return value;
    const GridPlan plan = gridPlanFor(grid, box);
    const double span = horizontal ? plan.spanX : plan.spanY;
    const double origin = horizontal ? plan.originX : plan.originY;
    if (span <= 0) return value;
    const double k = std::round((static_cast<double>(value) - origin) / span);
    const double nearest = origin + k * span;
    if (grid.snap == GridSnap::Magnet) {
        const double range = hundredthMmToHu(horizontal ? grid.magnetX : grid.magnetY);
        if (std::abs(nearest - static_cast<double>(value)) > range) return value;
    }
    return static_cast<std::int32_t>(std::lround(nearest));
}

namespace {
struct PaperBoxState {
    std::mutex mu;
    std::vector<std::pair<int, PaperBox>> boxes;
};
PaperBoxState& paperBoxState() {
    static PaperBoxState state;
    return state;
}
}  // namespace

void setScreenPaperBoxes(std::vector<std::pair<int, PaperBox>> boxes) {
    PaperBoxState& state = paperBoxState();
    std::lock_guard lock(state.mu);
    state.boxes = std::move(boxes);
}

std::optional<PaperBox> screenPaperBox(int page) {
    PaperBoxState& state = paperBoxState();
    std::lock_guard lock(state.mu);
    for (const auto& [index, box] : state.boxes) {
        if (index == page) return box;
    }
    return std::nullopt;
}

}  // namespace chwp::render
