#pragma once

// 단 너비와 간격. 쪽을 쌓는 경로와 줄 캐시 경로가 같은 계산을 쓴다.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "noting/render/scene.hpp"

namespace noting::render {

struct ColumnBox {
    std::int32_t x = 0;
    std::int32_t width = 0;
    std::int32_t gapAfter = 0;
};

struct ColumnLayout {
    int count = 1;
    std::int32_t gap = 0;
    std::string type;  // NEWSPAPER·BalancedNewspaper 는 배분. PARALLEL 은 평행. 그 밖은 일반.
    bool sameSize = true;
    std::vector<std::int32_t> widths;
    std::vector<std::int32_t> gaps;  // 그 단 오른쪽 간격. 마지막 단은 쓰지 않는다.
    BorderEdge rule;
};

inline bool balancedColumns(const std::string& type) {
    return type == "NEWSPAPER" || type == "BALANCED_NEWSPAPER" || type == "BalancedNewspaper" ||
           type == "BALANCEDNEWSPAPER";
}

inline bool parallelColumns(const std::string& type) { return type == "PARALLEL" || type == "Parallel"; }

// 이 문단까지 나온 단 정의 중 마지막 것. 그 앞 쪽은 나중 단 설정의 영향을 받지 않는다.
inline ColumnLayout columnLayoutAt(const std::vector<model::Paragraph>& paragraphs, int index) {
    ColumnLayout layout;
    if (paragraphs.empty()) return layout;
    const int last = std::min(index, static_cast<int>(paragraphs.size()) - 1);
    for (int i = 0; i <= last; ++i) {
        for (const auto& run : paragraphs[static_cast<std::size_t>(i)].runs) {
            for (const auto& item : run.items) {
                if (item.type != model::InlineItem::Type::Control ||
                    item.control.kind != model::ControlKind::ColumnDef) {
                    continue;
                }
                if (item.control.columnCount == 0) continue;
                const model::Control& control = item.control;
                layout.count = static_cast<int>(control.columnCount);
                layout.gap = control.columnGap;
                layout.type = control.columnType;
                layout.sameSize = control.columnSameSize;
                layout.widths.clear();
                layout.gaps.clear();
                for (const model::ColumnWidth& slice : control.columnWidths) {
                    layout.widths.push_back(slice.width);
                    layout.gaps.push_back(slice.gap);
                }
                layout.rule = BorderEdge{};
                layout.rule.type = control.columnLineType;
                layout.rule.color = control.columnLineColor;
                layout.rule.width = control.columnLineWidth;
                if (!layout.rule.type.empty() && layout.rule.type != "NONE") {
                    if (layout.rule.color.empty() || layout.rule.color == "none") layout.rule.color = "#000000";
                    // 구분선 굵기 기본은 0.12mm.
                    if (layout.rule.width <= 0) layout.rule.width = 34;
                }
            }
        }
    }
    return layout;
}

// 너비가 같으면 본문 너비에서 간격을 빼고 단 수로 나눈다.
// 너비가 다르면 적힌 폭과 오른쪽 간격을 본문 너비에 맞춘다. 마지막 단이 나머지를 받는다.
// floor4 이면 줄 너비를 4단위로 내린다. 본문 45354는 lineseg 45352가 된다. 다음 단의 시작은 내리기 전 너비다.
inline std::vector<ColumnBox> columnBoxes(const ColumnLayout& columns, std::int32_t contentW, bool floor4) {
    const int count = std::max(1, columns.count);
    std::vector<ColumnBox> out(static_cast<std::size_t>(count));
    auto equal = [&] {
        const std::int32_t gap = count > 1 ? std::max<std::int32_t>(0, columns.gap) : 0;
        const std::int32_t colW = std::max<std::int32_t>(200, (contentW - gap * (count - 1)) / count);
        for (int i = 0; i < count; ++i) {
            ColumnBox& box = out[static_cast<std::size_t>(i)];
            box.x = i * (colW + gap);
            std::int32_t width = i == count - 1 ? contentW - box.x : colW;
            if (floor4 && width > 0) width -= width % 4;
            box.width = width;
            box.gapAfter = i + 1 < count ? gap : 0;
        }
    };
    const bool custom = !columns.sameSize && static_cast<int>(columns.widths.size()) >= count;
    if (!custom) {
        equal();
        return out;
    }
    bool anyGap = false;
    for (int i = 0; i + 1 < count && i < static_cast<int>(columns.gaps.size()); ++i) {
        if (columns.gaps[static_cast<std::size_t>(i)] > 0) anyGap = true;
    }
    std::vector<std::int32_t> gaps(static_cast<std::size_t>(count), 0);
    for (int i = 0; i + 1 < count; ++i) {
        std::int32_t gap = columns.gap;
        if (anyGap && i < static_cast<int>(columns.gaps.size())) gap = columns.gaps[static_cast<std::size_t>(i)];
        gaps[static_cast<std::size_t>(i)] = std::max<std::int32_t>(0, gap);
    }
    std::int64_t file = 0;
    for (int i = 0; i < count; ++i) {
        file += std::max<std::int32_t>(0, columns.widths[static_cast<std::size_t>(i)]);
        if (i + 1 < count) file += gaps[static_cast<std::size_t>(i)];
    }
    if (file <= 0) {
        equal();
        return out;
    }
    std::int32_t x = 0;
    for (int i = 0; i < count; ++i) {
        ColumnBox& box = out[static_cast<std::size_t>(i)];
        box.x = x;
        const bool last = i + 1 == count;
        std::int32_t raw = 0;
        std::int32_t gap = 0;
        if (last) {
            raw = contentW - x;
        } else {
            const std::int32_t width = std::max<std::int32_t>(0, columns.widths[static_cast<std::size_t>(i)]);
            raw = static_cast<std::int32_t>(static_cast<std::int64_t>(width) * contentW / file);
            gap = static_cast<std::int32_t>(static_cast<std::int64_t>(gaps[static_cast<std::size_t>(i)]) * contentW / file);
        }
        if (raw < 0) raw = 0;
        std::int32_t width = raw;
        if (floor4 && width > 0) width -= width % 4;
        box.width = width;
        box.gapAfter = last ? 0 : gap;
        if (!last) x += raw + gap;
    }
    return out;
}

inline void applyColumnRule(Node& column, const ColumnLayout& layout, bool between) {
    if (!between || layout.rule.type.empty() || layout.rule.type == "NONE") return;
    column.borderRight = layout.rule;
}

}  // namespace noting::render
