// 단 설정(한글 2024 단 설정 대화 상자와 단 ▾ 목록).
//
// 계산은 한글 2024 단 설정(MultiColumnPreset)과 같다.
// - 단 사이 간격 처음 값은 8mm(MulDiv(8, 283465, 1000) = 2268)이고, 단이 셋 이상이면 그 값을 (단 수 - 1)로 나눈다.
// - 같은 너비면 (본문 폭 - 간격 합) / 단 수, 왼쪽·오른쪽 모양은 두 단을 1/3 과 나머지로 나눈다.
// - 한 단이 10mm(2835) 보다 좁아지면 간격을 (본문 폭 - 10mm × 단 수) / (단 수 - 1) 로 줄이고, 그래도 모자라면 간격 0·10mm 다.
// - 단 수 최대는 본문 폭 / 10mm(1~255)다.
// 적용 범위는 한글 2024 화면에서 확인했다: 문서 전체는 구역 첫 단 정의를 고치고, 새 단으로는 캐럿에서 문단을 가르고
// 새 문단 앞에 단 정의를, 새 쪽으로는 거기에 쪽 나눔을 더한다. 너비가 다르면 파일에는 32768 을 본문 폭으로 본 비율로 적는다.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "render/paint_detail.hpp"

namespace chwp::render {

using namespace paint_detail;

namespace {

constexpr std::int32_t kMinColumnHu = 2835;  // 10mm
constexpr std::int32_t kGapHu = 2268;        // 8mm
constexpr std::int32_t kRelativeTotal = 32768;

double huToMm(std::int32_t hu) { return std::round(hu * 25.4 / 7200.0 * 10.0) / 10.0; }
std::int32_t mmToHuRound(double mm) { return static_cast<std::int32_t>(std::lround(mm * 7200.0 / 25.4)); }

const FormatValue* fieldOf(const FormatFields& fields, const std::string& key) {
    const FormatValue* found = nullptr;
    for (const auto& [name, value] : fields) {
        if (name == key) found = &value;
    }
    return found;
}
std::string textOf(const FormatFields& fields, const std::string& key, const std::string& fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const std::string* t = std::get_if<std::string>(v)) return *t;
    }
    return fallback;
}
double numberOf(const FormatFields& fields, const std::string& key, double fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const double* d = std::get_if<double>(v)) return *d;
    }
    return fallback;
}
bool flagOf(const FormatFields& fields, const std::string& key, bool fallback) {
    if (const FormatValue* v = fieldOf(fields, key)) {
        if (const bool* b = std::get_if<bool>(v)) return *b;
    }
    return fallback;
}

// 단 계획. 너비·간격은 HWPUNIT 이다.
struct ColumnPlan {
    std::string type = "NEWSPAPER";
    std::string direction = "LEFT";
    int count = 1;
    bool sameSize = true;
    std::int32_t gap = 0;
    std::vector<std::int32_t> widths;
    std::vector<std::int32_t> gaps;
    std::string lineType = "NONE";
    std::int32_t lineWidth = 34;  // 0.12mm
    std::string lineColor = "#000000";
};

// 구역 본문 폭(첫 쪽).
std::int32_t bodyWidth(const model::Document& doc, int section) {
    if (section < 0 || section >= static_cast<int>(doc.sections.size())) return 42520;
    const model::PageDef page = doc.sections[static_cast<std::size_t>(section)].flowPage(1);
    return std::max<std::int32_t>(kMinColumnHu, static_cast<std::int32_t>(page.width) -
                                                    static_cast<std::int32_t>(page.marginLeft) -
                                                    static_cast<std::int32_t>(page.marginRight));
}

// 한글 단 설정 너비 계산. preset 은 "", left, right 다.
void planWidths(ColumnPlan& plan, std::int32_t body, const std::string& preset, std::int32_t askedGap) {
    plan.widths.clear();
    plan.gaps.clear();
    if (plan.count <= 1) {
        plan.count = 1;
        plan.gap = 0;
        plan.widths = {body};
        plan.gaps = {0};
        return;
    }
    const int n = plan.count;
    std::int32_t gap = askedGap >= 0 ? askedGap : (n > 2 ? kGapHu / (n - 1) : kGapHu);
    const bool sided = n == 2 && (preset == "left" || preset == "right");
    std::int32_t usable = body - (n - 1) * gap;
    std::int32_t narrow = sided ? usable / 3 : usable / n;
    std::int32_t wide = sided ? usable - narrow : narrow;
    if (narrow < kMinColumnHu) {
        gap = (body - kMinColumnHu * n) / (n - 1);
        if (gap < 0) {
            gap = 0;
            narrow = kMinColumnHu;
            wide = sided ? body - kMinColumnHu : kMinColumnHu;
        } else {
            usable = body - (n - 1) * gap;
            narrow = sided ? usable / 3 : usable / n;
            wide = sided ? usable - narrow : narrow;
        }
    }
    plan.gap = gap;
    for (int i = 0; i < n; ++i) {
        std::int32_t width = narrow;
        if (sided) width = (preset == "left") == (i == 0) ? narrow : wide;
        plan.widths.push_back(width);
        plan.gaps.push_back(i + 1 < n ? gap : 0);
    }
}

// 캐럿 자리에 걸린 단 정의. 본문 밖이면 구역 첫 것이다.
const model::Control* columnDefAt(const model::Document& doc, int section, int paragraph) {
    if (section < 0 || section >= static_cast<int>(doc.sections.size())) return nullptr;
    const auto& paras = doc.sections[static_cast<std::size_t>(section)].paragraphs;
    const model::Control* found = nullptr;
    const int last = std::min<int>(paragraph < 0 ? 0 : paragraph, static_cast<int>(paras.size()) - 1);
    for (int p = 0; p <= last; ++p) {
        for (const model::Run& run : paras[static_cast<std::size_t>(p)].runs) {
            for (const model::InlineItem& item : run.items) {
                if (item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::ColumnDef) {
                    found = &item.control;
                }
            }
        }
    }
    return found;
}

// 캐럿의 구역과 본문 문단. 본문이 아니면 문단은 -1 이다.
void caretPlace(const model::Document& doc, int& section, int& paragraph) {
    section = std::max(0, cursorSection(doc));
    paragraph = -1;
    const PointerCursor cur = pointerCursor();
    if (!cur.active || cur.story <= 0) return;
    const model::StorySlot* slot = resolveStory(doc, cur.story);
    if (slot && slot->kind == model::StoryKind::Section) paragraph = cur.paragraph;
}

ColumnPlan planFromControl(const model::Control* control, std::int32_t body) {
    ColumnPlan plan;
    if (!control || control->columnCount == 0) {
        planWidths(plan, body, "", -1);
        return plan;
    }
    plan.type = control->columnType.empty() ? "NEWSPAPER" : control->columnType;
    plan.direction = control->columnLayout.empty() ? "LEFT" : control->columnLayout;
    plan.count = static_cast<int>(control->columnCount);
    plan.sameSize = control->columnSameSize || control->columnWidths.size() < control->columnCount;
    if (!control->columnLineType.empty() && control->columnLineType != "NONE") {
        plan.lineType = control->columnLineType;
        if (control->columnLineWidth > 0) plan.lineWidth = control->columnLineWidth;
        if (!control->columnLineColor.empty()) plan.lineColor = control->columnLineColor;
    }
    if (plan.sameSize) {
        planWidths(plan, body, "", plan.count > 1 ? control->columnGap : -1);
        return plan;
    }
    // 파일의 너비·간격은 비율이다(합이 본문 폭).
    std::int64_t total = 0;
    for (const model::ColumnWidth& slice : control->columnWidths) total += slice.width + slice.gap;
    if (total <= 0) total = kRelativeTotal;
    for (std::uint32_t i = 0; i < control->columnCount; ++i) {
        const model::ColumnWidth& slice = control->columnWidths[i];
        plan.widths.push_back(static_cast<std::int32_t>(slice.width * static_cast<std::int64_t>(body) / total));
        plan.gaps.push_back(i + 1 < control->columnCount
                                ? static_cast<std::int32_t>(slice.gap * static_cast<std::int64_t>(body) / total)
                                : 0);
    }
    plan.gap = plan.gaps.empty() ? 0 : plan.gaps.front();
    return plan;
}

// 고른 값을 계획에 얹는다. preset(one·two·three·left·right)이 있으면 그 모양으로 다시 짠다.
ColumnPlan planOf(const model::Document& doc, const FormatFields& asked, std::int32_t& body, int& maxCount) {
    int section = 0;
    int paragraph = -1;
    caretPlace(doc, section, paragraph);
    body = bodyWidth(doc, section);
    maxCount = std::clamp(body / kMinColumnHu, 1, 255);
    ColumnPlan plan = planFromControl(columnDefAt(doc, section, paragraph), body);
    plan.type = textOf(asked, "type", plan.type);
    plan.direction = textOf(asked, "direction", plan.direction);
    if (fieldOf(asked, "line")) plan.lineType = flagOf(asked, "line", false) ? (plan.lineType == "NONE" ? "SOLID" : plan.lineType) : "NONE";
    if (plan.lineType != "NONE") plan.lineType = textOf(asked, "lineType", plan.lineType);
    plan.lineWidth = static_cast<std::int32_t>(numberOf(asked, "lineWidth", plan.lineWidth));
    plan.lineColor = textOf(asked, "lineColor", plan.lineColor);

    const std::string preset = textOf(asked, "preset", "");
    if (!preset.empty()) {
        plan.count = preset == "one" ? 1 : preset == "three" ? 3 : 2;
        plan.sameSize = preset != "left" && preset != "right";
        planWidths(plan, body, preset, -1);
        return plan;
    }
    const int count = std::clamp(static_cast<int>(numberOf(asked, "count", plan.count)), 1, maxCount);
    const bool sameSize = flagOf(asked, "sameSize", plan.sameSize);
    if (count != plan.count || (sameSize && !plan.sameSize)) {
        plan.count = count;
        plan.sameSize = true;
        planWidths(plan, body, "", -1);
        if (!sameSize) plan.sameSize = false;
        return plan;
    }
    plan.sameSize = sameSize;
    if (plan.sameSize) {
        const double gapMm = numberOf(asked, "gapMm", -1);
        planWidths(plan, body, "", gapMm >= 0 ? mmToHuRound(gapMm) : (plan.count > 1 ? plan.gap : -1));
        return plan;
    }
    for (int i = 0; i < plan.count; ++i) {
        const double w = numberOf(asked, "widthMm." + std::to_string(i), -1);
        if (w > 0 && i < static_cast<int>(plan.widths.size())) plan.widths[static_cast<std::size_t>(i)] = mmToHuRound(w);
        const double g = numberOf(asked, "gapMm." + std::to_string(i), -1);
        if (g >= 0 && i + 1 < plan.count && i < static_cast<int>(plan.gaps.size())) {
            plan.gaps[static_cast<std::size_t>(i)] = mmToHuRound(g);
        }
    }
    return plan;
}

// 계획을 단 정의 컨트롤로. 너비가 다르면 32768 비율로 적는다(한글 2024 저장과 같다).
void writePlan(const ColumnPlan& plan, std::int32_t body, model::Control& control) {
    control.kind = model::ControlKind::ColumnDef;
    if (control.ctrlId.empty()) control.ctrlId = "cold";
    control.columnCount = static_cast<std::uint32_t>(plan.count);
    control.columnType = plan.type;
    control.columnLayout = plan.direction;
    control.columnSameSize = plan.sameSize;
    control.columnWidths.clear();
    control.columnGap = plan.count > 1 && plan.sameSize ? plan.gap : 0;
    if (!plan.sameSize && plan.count > 1) {
        for (int i = 0; i < plan.count; ++i) {
            model::ColumnWidth slice;
            slice.width = static_cast<std::int32_t>(static_cast<std::int64_t>(plan.widths[static_cast<std::size_t>(i)]) *
                                                    kRelativeTotal / body);
            slice.gap = i + 1 < plan.count
                            ? static_cast<std::int32_t>(static_cast<std::int64_t>(plan.gaps[static_cast<std::size_t>(i)]) *
                                                        kRelativeTotal / body)
                            : 0;
            control.columnWidths.push_back(slice);
        }
    }
    control.columnLineType = plan.lineType == "NONE" ? std::string() : plan.lineType;
    control.columnLineWidth = plan.lineType == "NONE" ? 0 : plan.lineWidth;
    control.columnLineColor = plan.lineType == "NONE" ? std::string() : plan.lineColor;
}

// 구역 첫 문단의 첫 단 정의. 없으면 맨 앞에 넣는다.
model::Control& leadColumnDef(model::Section& section) {
    model::Paragraph& first = section.paragraphs.front();
    for (model::Run& run : first.runs) {
        for (model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::ColumnDef) {
                return item.control;
            }
        }
    }
    if (first.runs.empty()) first.runs.emplace_back();
    model::Control fresh;
    fresh.kind = model::ControlKind::ColumnDef;
    fresh.ctrlId = "cold";
    auto& items = first.runs.front().items;
    // 구역 정의 바로 뒤에 둔다.
    auto at = items.begin();
    while (at != items.end() && at->type == model::InlineItem::Type::Control &&
           at->control.kind == model::ControlKind::SectionDef) {
        ++at;
    }
    at = items.insert(at, model::InlineItem::makeControl(std::move(fresh)));
    return at->control;
}

}  // namespace

FormatFields columnView(const model::Document& doc, const std::vector<Node>& pages, const FormatFields& asked) {
    (void)pages;
    std::int32_t body = 0;
    int maxCount = 1;
    const ColumnPlan plan = planOf(doc, asked, body, maxCount);
    FormatFields out;
    out.emplace_back("type", plan.type);
    out.emplace_back("count", static_cast<double>(plan.count));
    out.emplace_back("maxCount", static_cast<double>(maxCount));
    out.emplace_back("direction", plan.direction);
    out.emplace_back("sameSize", plan.sameSize);
    out.emplace_back("gapMm", huToMm(plan.count > 1 ? plan.gap : 0));
    for (int i = 0; i < plan.count; ++i) {
        out.emplace_back("widthMm." + std::to_string(i), huToMm(plan.widths[static_cast<std::size_t>(i)]));
        if (i + 1 < plan.count) out.emplace_back("gapMm." + std::to_string(i), huToMm(plan.gaps[static_cast<std::size_t>(i)]));
    }
    out.emplace_back("line", plan.lineType != "NONE");
    out.emplace_back("lineType", plan.lineType == "NONE" ? std::string("SOLID") : plan.lineType);
    out.emplace_back("lineWidth", static_cast<double>(plan.lineWidth));
    out.emplace_back("lineColor", plan.lineColor);
    out.emplace_back("bodyMm", huToMm(body));
    // 지금 모양과 같은 목록 모양(대화 상자가 그 칸을 눌린 것으로 보인다).
    std::string preset;
    if (plan.count == 1) preset = "one";
    else if (plan.sameSize && plan.count == 2) preset = "two";
    else if (plan.sameSize && plan.count == 3) preset = "three";
    else if (!plan.sameSize && plan.count == 2) {
        ColumnPlan left = plan;
        planWidths(left, body, "left", -1);
        ColumnPlan right = plan;
        planWidths(right, body, "right", -1);
        // 파일의 32768 비율에서 되돌린 너비는 몇 HWPUNIT 어긋난다.
        const auto near = [&](const ColumnPlan& other) {
            for (int i = 0; i < 2; ++i) {
                if (std::abs(other.widths[static_cast<std::size_t>(i)] - plan.widths[static_cast<std::size_t>(i)]) > 4) return false;
            }
            return true;
        };
        if (near(left)) preset = "left";
        else if (near(right)) preset = "right";
    }
    out.emplace_back("preset", preset);
    out.emplace_back("applyTo", textOf(asked, "applyTo", "WHOLE"));
    return out;
}

EditResult columnsNow(model::Document& doc, std::vector<Node>& pages, const FormatFields& fields,
                      bool useStoredLineHeight, std::string& why) {
    if (readOnly()) return {};
    std::int32_t body = 0;
    int maxCount = 1;
    const ColumnPlan plan = planOf(doc, fields, body, maxCount);
    const std::string applyTo = textOf(fields, "applyTo", "WHOLE");
    int section = 0;
    int paragraph = -1;
    caretPlace(doc, section, paragraph);
    PointerHold& hold = pointerHold();
    clearComposition(hold);
    if (applyTo == "NEW_COLUMN" || applyTo == "NEW_PAGE") {
        if (paragraph < 0 || hold.bandStory > 0) {
            why = "새 단·새 쪽으로 넣는 단 설정은 본문에서만 합니다.";
            return {};
        }
        // 캐럿에서 문단을 가르고(새 다단 나누기) 그 문단 앞 단 정의를 새 값으로 바꾼다.
        EditResult split = insertBreakNow(doc, pages, BreakKind::ColumnDef, useStoredLineHeight);
        if (!split.documentChanged) {
            why = "단 설정을 넣을 자리가 아닙니다.";
            return {};
        }
        const PointerCursor cur = pointerCursor();
        model::Paragraph* fresh = storyParagraph(doc, cur.story, cur.paragraph);
        if (!fresh) return split;
        model::Control* def = nullptr;
        for (model::Run& run : fresh->runs) {
            for (model::InlineItem& item : run.items) {
                if (!def && item.type == model::InlineItem::Type::Control && item.control.kind == model::ControlKind::ColumnDef) {
                    def = &item.control;
                }
            }
        }
        if (!def) {
            if (fresh->runs.empty()) fresh->runs.emplace_back();
            auto& items = fresh->runs.front().items;
            model::Control blank;
            blank.kind = model::ControlKind::ColumnDef;
            blank.ctrlId = "cold";
            def = &items.insert(items.begin(), model::InlineItem::makeControl(std::move(blank)))->control;
        }
        writePlan(plan, body, *def);
        if (applyTo == "NEW_PAGE") fresh->pageBreak = true;
        fresh->lineSegs.clear();
    } else {
        // 문서 전체: 구역마다 첫 단 정의를 고친다.
        for (model::Section& each : doc.sections) {
            if (each.paragraphs.empty()) continue;
            writePlan(plan, body, leadColumnDef(each));
            each.paragraphs.front().lineSegs.clear();
        }
    }
    const PointerCursor before = pointerCursor();
    model::ensureParagraphIds(doc);
    rebuildStoryMap(doc);
    invalidateLinePlaces({}, true);
    pages = layoutPages(doc, -1, useStoredLineHeight, useStoredLineHeight);
    EditRelayoutResult relayout;
    relayout.fullInvalidation = true;
    relayout.pageListChanged = true;
    if (before.active && before.story > 0) {
        setPointerCursor(doc, pages, model::CursorPos{before.story, before.paragraph, before.offset});
    }
    const PointerCursor now = pointerCursor();
    return makeEditResult(doc, true, true, now.story, now.paragraph, &relayout);
}

}  // namespace chwp::render
