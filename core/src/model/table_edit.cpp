#include "chwp/model/table_edit.hpp"

#include <algorithm>
#include <map>
#include <cstdlib>
#include <numeric>

#include "chwp/unit.hpp"

namespace chwp::model {

namespace {

int span(std::uint32_t value) { return std::max(1, static_cast<int>(value)); }
int lastRow(const TableCell& cell) { return static_cast<int>(cell.row) + span(cell.rowSpan) - 1; }
int lastCol(const TableCell& cell) { return static_cast<int>(cell.col) + span(cell.colSpan) - 1; }

std::int32_t sumRange(const std::vector<std::int32_t>& sizes, int first, int last) {
    std::int32_t total = 0;
    for (int i = std::max(0, first); i <= last && i < static_cast<int>(sizes.size()); ++i) total += sizes[static_cast<std::size_t>(i)];
    return total;
}

bool paragraphEmpty(const Paragraph& para) {
    for (const Run& run : para.runs) {
        for (const InlineItem& item : run.items) {
            if (item.type == InlineItem::Type::Control || !item.text.empty()) return false;
        }
    }
    return true;
}

// 글자도 개체도 없는 칸. 합칠 때 이 칸의 빈 문단은 잇지 않는다(한글 2024).
bool cellEmpty(const TableCell& cell) {
    return std::all_of(cell.content.paragraphs.begin(), cell.content.paragraphs.end(), paragraphEmpty);
}

// ref 칸의 모양(테두리/배경, 여백, 세로 정렬, 첫 문단의 문단·글자 모양)을 따른 빈 칸.
TableCell emptyCellLike(const TableCell& ref) {
    TableCell cell = ref;
    cell.rowSpan = 1;
    cell.colSpan = 1;
    Paragraph para;
    std::uint32_t charShape = 0;
    if (!ref.content.paragraphs.empty()) {
        const Paragraph& first = ref.content.paragraphs.front();
        para.paraShapeId = first.paraShapeId;
        para.styleId = first.styleId;
        if (!first.runs.empty()) charShape = first.runs.front().charShapeId;
    }
    Run run;
    run.charShapeId = charShape;
    para.runs.push_back(std::move(run));
    cell.content.paragraphs.clear();
    cell.content.paragraphs.push_back(std::move(para));
    return cell;
}

void clearLines(TableCell& cell) {
    for (Paragraph& para : cell.content.paragraphs) para.lineSegs.clear();
}

// 고친 뒤 칸 순번이 바뀐다. order[옛 순번] = 새 순번.
void normalize(Table& table, std::vector<int>* order) {
    int rows = 0;
    int cols = 0;
    for (const TableCell& cell : table.cells) {
        rows = std::max(rows, lastRow(cell) + 1);
        cols = std::max(cols, lastCol(cell) + 1);
    }
    // 어느 칸도 그 자리에서 시작하지 않는 경계선은 칸을 나누지 않는다. 없애고 걸친 칸을 줄인다.
    for (int j = cols - 1; j >= 1; --j) {
        const bool used = std::any_of(table.cells.begin(), table.cells.end(),
                                      [j](const TableCell& cell) { return static_cast<int>(cell.col) == j; });
        if (used) continue;
        for (TableCell& cell : table.cells) {
            if (static_cast<int>(cell.col) > j) --cell.col;
            else if (lastCol(cell) >= j) cell.colSpan = static_cast<std::uint32_t>(span(cell.colSpan) - 1);
        }
        --cols;
    }
    for (int i = rows - 1; i >= 1; --i) {
        const bool used = std::any_of(table.cells.begin(), table.cells.end(),
                                      [i](const TableCell& cell) { return static_cast<int>(cell.row) == i; });
        if (used) continue;
        for (TableCell& cell : table.cells) {
            if (static_cast<int>(cell.row) > i) --cell.row;
            else if (lastRow(cell) >= i) cell.rowSpan = static_cast<std::uint32_t>(span(cell.rowSpan) - 1);
        }
        --rows;
    }
    std::vector<int> index(table.cells.size());
    std::iota(index.begin(), index.end(), 0);
    std::stable_sort(index.begin(), index.end(), [&](int a, int b) {
        const TableCell& x = table.cells[static_cast<std::size_t>(a)];
        const TableCell& y = table.cells[static_cast<std::size_t>(b)];
        return x.row != y.row ? x.row < y.row : x.col < y.col;
    });
    std::vector<TableCell> sorted;
    sorted.reserve(table.cells.size());
    if (order) order->assign(table.cells.size(), -1);
    for (std::size_t i = 0; i < index.size(); ++i) {
        if (order) (*order)[static_cast<std::size_t>(index[i])] = static_cast<int>(i);
        sorted.push_back(std::move(table.cells[static_cast<std::size_t>(index[i])]));
    }
    table.cells = std::move(sorted);
    table.rowCount = static_cast<std::uint32_t>(rows);
    table.colCount = static_cast<std::uint32_t>(cols);
}

// 편집 앞 검사. 칸 그림이 깨진 표는 어디가 어떻게 늘고 줄지 정할 수 없어 고치지 않는다.
std::string checkTable(const Table& table, int cell) {
    if (cell < 0 || cell >= static_cast<int>(table.cells.size())) return "표에 그 칸이 없습니다.";
    if (!tableGridValid(table)) return "표 칸이 겹치거나 비어 있는 자리가 있어 구조를 고칠 수 없습니다.";
    return {};
}

// 칸 하나를 이 순번으로 들고 다니다가 정리한 뒤의 순번으로 바꾼다.
int finish(Table& table, int focus) {
    std::vector<int> order;
    normalize(table, &order);
    return focus >= 0 && focus < static_cast<int>(order.size()) ? order[static_cast<std::size_t>(focus)] : -1;
}

}  // namespace

std::vector<std::int32_t> solveSpanSizes(std::size_t count, std::vector<SpanSize> spans) {
    std::vector<std::int32_t> sizes(count, 0);
    // 좁은 병합을 먼저 칸에 넣는다. 넓은 병합이 먼저 칸을 나누면 나중에 더 좁은 칸으로 줄일 수 없다.
    const auto narrowFirst = [](const SpanSize& a, const SpanSize& b) {
        if (a.span != b.span) return a.span < b.span;
        return a.size < b.size;
    };
    std::sort(spans.begin(), spans.end(), narrowFirst);
    std::vector<std::int32_t> prefix(count + 1, -1);
    prefix[0] = 0;
    const auto validPrefix = [&](std::size_t idx, std::int32_t val) {
        if (val < 0) return false;
        for (std::size_t k = 0; k < prefix.size(); ++k) {
            if (prefix[k] < 0) continue;
            if (k < idx && prefix[k] >= val) return false;
            if (k > idx && prefix[k] <= val) return false;
        }
        return true;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& s : spans) {
            if (s.start >= count) continue;
            int n = s.span;
            if (s.start + static_cast<std::size_t>(n) > count) n = static_cast<int>(count - s.start);
            if (n <= 0) continue;
            const std::size_t a = s.start;
            const std::size_t b = s.start + static_cast<std::size_t>(n);
            if (prefix[a] >= 0 && prefix[b] < 0) {
                const std::int32_t cand = prefix[a] + s.size;
                if (validPrefix(b, cand)) {
                    prefix[b] = cand;
                    changed = true;
                }
            } else if (prefix[b] >= 0 && prefix[a] < 0) {
                const std::int32_t cand = prefix[b] - s.size;
                if (validPrefix(a, cand)) {
                    prefix[a] = cand;
                    changed = true;
                }
            }
        }
    }
    std::size_t prevKnown = 0;
    for (std::size_t i = 1; i < prefix.size(); ++i) {
        if (prefix[i] < 0) continue;
        if (i - prevKnown == 1 && prefix[i] > prefix[prevKnown]) {
            sizes[prevKnown] = prefix[i] - prefix[prevKnown];
        } else if (i - prevKnown > 1 && prefix[i] > prefix[prevKnown]) {
            spans.push_back(SpanSize{prevKnown, static_cast<int>(i - prevKnown), prefix[i] - prefix[prevKnown]});
        }
        prevKnown = i;
    }
    std::sort(spans.begin(), spans.end(), narrowFirst);
    for (const auto& s : spans) {
        if (s.start >= count) continue;
        int n = s.span;
        if (s.start + static_cast<std::size_t>(n) > count) n = static_cast<int>(count - s.start);
        if (n <= 0) continue;
        std::int32_t known = 0;
        int unknown = 0;
        for (int i = 0; i < n; ++i) {
            if (sizes[s.start + static_cast<std::size_t>(i)] > 0) known += sizes[s.start + static_cast<std::size_t>(i)];
            else ++unknown;
        }
        if (unknown == 0) {
            if (n == 1 && s.size > known) sizes[s.start] = s.size;
            continue;
        }
        if (s.size <= known) continue;
        const std::int32_t each = (s.size - known) / unknown;
        std::int32_t remain = s.size - known - each * unknown;
        for (int i = 0; i < n; ++i) {
            if (sizes[s.start + static_cast<std::size_t>(i)] > 0) continue;
            const std::int32_t add = each + (remain > 0 ? 1 : 0);
            if (add <= 0) continue;
            sizes[s.start + static_cast<std::size_t>(i)] = add;
            if (remain > 0) --remain;
        }
    }
    return sizes;
}

TableGrid tableGrid(const Table& table) {
    TableGrid grid;
    grid.rows = static_cast<int>(table.rowCount);
    grid.cols = static_cast<int>(table.colCount);
    for (const TableCell& cell : table.cells) {
        grid.rows = std::max(grid.rows, lastRow(cell) + 1);
        grid.cols = std::max(grid.cols, lastCol(cell) + 1);
    }
    grid.owner.assign(static_cast<std::size_t>(grid.rows) * static_cast<std::size_t>(grid.cols), -1);
    for (std::size_t i = 0; i < table.cells.size(); ++i) {
        const TableCell& cell = table.cells[i];
        for (int r = static_cast<int>(cell.row); r <= lastRow(cell); ++r) {
            for (int c = static_cast<int>(cell.col); c <= lastCol(cell); ++c) {
                int& slot = grid.owner[static_cast<std::size_t>(r * grid.cols + c)];
                slot = slot == -1 ? static_cast<int>(i) : -2;
            }
        }
    }
    return grid;
}

bool tableGridValid(const Table& table) {
    if (table.cells.empty()) return false;
    const TableGrid grid = tableGrid(table);
    return std::all_of(grid.owner.begin(), grid.owner.end(), [](int owner) { return owner >= 0; });
}

std::vector<std::int32_t> tableColumnWidths(const Table& table) {
    const TableGrid grid = tableGrid(table);
    std::vector<SpanSize> spans;
    for (const TableCell& cell : table.cells) {
        if (cell.width > 0) spans.push_back(SpanSize{cell.col, span(cell.colSpan), static_cast<std::int32_t>(cell.width)});
    }
    return solveSpanSizes(static_cast<std::size_t>(grid.cols), std::move(spans));
}

std::vector<std::int32_t> tableRowHeights(const Table& table) {
    const TableGrid grid = tableGrid(table);
    std::vector<SpanSize> spans;
    for (const TableCell& cell : table.cells) {
        if (cell.height > 0) spans.push_back(SpanSize{cell.row, span(cell.rowSpan), static_cast<std::int32_t>(cell.height)});
    }
    return solveSpanSizes(static_cast<std::size_t>(grid.rows), std::move(spans));
}

void normalizeTable(Table& table) { normalize(table, nullptr); }

TableEdit insertTableRows(Table& table, int cell, bool below, int count) {
    TableEdit out;
    if (out.error = checkTable(table, cell); !out.ok()) return out;
    if (count < 1) {
        out.error = "넣을 줄 수는 1 이상입니다.";
        return out;
    }
    const TableGrid grid = tableGrid(table);
    const std::vector<std::int32_t> widths = tableColumnWidths(table);
    const std::vector<std::int32_t> heights = tableRowHeights(table);
    const TableCell ref = table.cells[static_cast<std::size_t>(cell)];
    const int refRow = below ? lastRow(ref) : static_cast<int>(ref.row);
    const int at = below ? refRow + 1 : refRow;
    const std::int32_t height = std::max<std::int32_t>(1, heights[static_cast<std::size_t>(refRow)]);
    const auto crosses = [at](const TableCell& c) { return static_cast<int>(c.row) < at && lastRow(c) >= at; };

    std::vector<TableCell> added;
    int focus = -1;
    for (int c = 0; c < grid.cols; ++c) {
        const TableCell& owner = table.cells[static_cast<std::size_t>(grid.at(refRow, c))];
        if (static_cast<int>(owner.col) != c || crosses(owner)) continue;
        for (int k = 0; k < count; ++k) {
            TableCell fresh = emptyCellLike(owner);
            fresh.row = static_cast<std::uint32_t>(at + k);
            fresh.colSpan = owner.colSpan;
            fresh.width = static_cast<std::uint32_t>(sumRange(widths, c, lastCol(owner)));
            fresh.height = static_cast<std::uint32_t>(height);
            if (k == 0 && c <= static_cast<int>(ref.col) && static_cast<int>(ref.col) <= lastCol(owner)) {
                focus = static_cast<int>(table.cells.size() + added.size());
            }
            added.push_back(std::move(fresh));
        }
    }
    for (TableCell& c : table.cells) {
        if (static_cast<int>(c.row) >= at) {
            c.row += static_cast<std::uint32_t>(count);
        } else if (crosses(c)) {
            c.rowSpan = static_cast<std::uint32_t>(span(c.rowSpan) + count);
            c.height += static_cast<std::uint32_t>(height * count);
            clearLines(c);
        }
    }
    for (TableCell& c : added) table.cells.push_back(std::move(c));
    out.heightDelta = height * count;
    out.focusCell = finish(table, focus);
    return out;
}

TableEdit insertTableColumns(Table& table, int cell, bool right, int count) {
    TableEdit out;
    if (out.error = checkTable(table, cell); !out.ok()) return out;
    if (count < 1) {
        out.error = "넣을 칸 수는 1 이상입니다.";
        return out;
    }
    const TableGrid grid = tableGrid(table);
    const std::vector<std::int32_t> widths = tableColumnWidths(table);
    const std::vector<std::int32_t> heights = tableRowHeights(table);
    const TableCell ref = table.cells[static_cast<std::size_t>(cell)];
    const int refCol = right ? lastCol(ref) : static_cast<int>(ref.col);
    const int at = right ? refCol + 1 : refCol;
    const std::int32_t width = std::max<std::int32_t>(1, widths[static_cast<std::size_t>(refCol)]);
    const auto crosses = [at](const TableCell& c) { return static_cast<int>(c.col) < at && lastCol(c) >= at; };

    std::vector<TableCell> added;
    int focus = -1;
    for (int r = 0; r < grid.rows; ++r) {
        const TableCell& owner = table.cells[static_cast<std::size_t>(grid.at(r, refCol))];
        if (static_cast<int>(owner.row) != r || crosses(owner)) continue;
        for (int k = 0; k < count; ++k) {
            TableCell fresh = emptyCellLike(owner);
            fresh.col = static_cast<std::uint32_t>(at + k);
            fresh.rowSpan = owner.rowSpan;
            fresh.width = static_cast<std::uint32_t>(width);
            fresh.height = static_cast<std::uint32_t>(sumRange(heights, r, lastRow(owner)));
            if (k == 0 && r <= static_cast<int>(ref.row) && static_cast<int>(ref.row) <= lastRow(owner)) {
                focus = static_cast<int>(table.cells.size() + added.size());
            }
            added.push_back(std::move(fresh));
        }
    }
    for (TableCell& c : table.cells) {
        if (static_cast<int>(c.col) >= at) {
            c.col += static_cast<std::uint32_t>(count);
        } else if (crosses(c)) {
            c.colSpan = static_cast<std::uint32_t>(span(c.colSpan) + count);
            c.width += static_cast<std::uint32_t>(width * count);
            clearLines(c);
        }
    }
    for (TableCell& c : added) table.cells.push_back(std::move(c));
    out.widthDelta = width * count;
    out.focusCell = finish(table, focus);
    return out;
}

namespace {

// 줄(rows=true)이나 칸을 first~last 만큼 지운다.
TableEdit deleteLines(Table& table, int first, int last, bool rows) {
    TableEdit out;
    if (out.error = checkTable(table, 0); !out.ok()) return out;
    const TableGrid grid = tableGrid(table);
    const int total = rows ? grid.rows : grid.cols;
    if (first > last) std::swap(first, last);
    if (first < 0 || last >= total) {
        out.error = std::string(rows ? "줄" : "칸") + "은 0~" + std::to_string(total - 1) + " 입니다.";
        return out;
    }
    if (first == 0 && last == total - 1) {
        out.removeTable = true;
        return out;
    }
    const std::vector<std::int32_t> sizes = rows ? tableRowHeights(table) : tableColumnWidths(table);
    const int n = last - first + 1;
    std::vector<TableCell> kept;
    int focus = -1;
    for (TableCell& c : table.cells) {
        const int a = rows ? static_cast<int>(c.row) : static_cast<int>(c.col);
        const int b = rows ? lastRow(c) : lastCol(c);
        const int o0 = std::max(a, first);
        const int o1 = std::min(b, last);
        std::uint32_t& start = rows ? c.row : c.col;
        std::uint32_t& count = rows ? c.rowSpan : c.colSpan;
        std::uint32_t& size = rows ? c.height : c.width;
        if (o0 > o1) {
            if (a > last) start -= static_cast<std::uint32_t>(n);
        } else if (a >= first && b <= last) {
            continue;  // 지우는 줄 안에 다 들었다
        } else {
            count = static_cast<std::uint32_t>(span(count) - (o1 - o0 + 1));
            const std::int32_t cut = sumRange(sizes, o0, o1);
            size = static_cast<std::uint32_t>(std::max<std::int32_t>(1, static_cast<std::int32_t>(size) - cut));
            if (a >= first) start = static_cast<std::uint32_t>(first);
            clearLines(c);
        }
        // 캐럿은 지운 자리에 새로 온 줄의 첫 칸에 둔다. 끝 줄을 지웠으면 그 앞 줄이다.
        const int focusLine = first < total - n ? first : first - 1;
        const int other = rows ? static_cast<int>(c.col) : static_cast<int>(c.row);
        const int s = rows ? static_cast<int>(c.row) : static_cast<int>(c.col);
        const int e = s + span(count) - 1;
        if (focus < 0 && other == 0 && s <= focusLine && focusLine <= e) focus = static_cast<int>(kept.size());
        kept.push_back(std::move(c));
    }
    table.cells = std::move(kept);
    const std::int32_t removed = sumRange(sizes, first, last);
    (rows ? out.heightDelta : out.widthDelta) = -removed;
    out.focusCell = finish(table, focus);
    return out;
}

}  // namespace

TableEdit deleteTableRows(Table& table, int first, int last) { return deleteLines(table, first, last, true); }
TableEdit deleteTableColumns(Table& table, int first, int last) { return deleteLines(table, first, last, false); }

TableEdit mergeTableCells(Table& table, int row0, int col0, int row1, int col1) {
    TableEdit out;
    if (out.error = checkTable(table, 0); !out.ok()) return out;
    if (row0 > row1) std::swap(row0, row1);
    if (col0 > col1) std::swap(col0, col1);
    const TableGrid grid = tableGrid(table);
    if (row0 < 0 || col0 < 0 || row1 >= grid.rows || col1 >= grid.cols) {
        out.error = "표는 줄 0~" + std::to_string(grid.rows - 1) + ", 칸 0~" + std::to_string(grid.cols - 1) + " 입니다.";
        return out;
    }
    std::vector<int> inside;
    for (std::size_t i = 0; i < table.cells.size(); ++i) {
        const TableCell& c = table.cells[i];
        const bool overlap = !(lastRow(c) < row0 || static_cast<int>(c.row) > row1 || lastCol(c) < col0 ||
                               static_cast<int>(c.col) > col1);
        if (!overlap) continue;
        if (static_cast<int>(c.row) < row0 || lastRow(c) > row1 || static_cast<int>(c.col) < col0 || lastCol(c) > col1) {
            out.error = "줄 " + std::to_string(c.row) + " 칸 " + std::to_string(c.col) +
                        " 의 합친 칸이 고른 범위 밖으로 걸쳐 있어 합칠 수 없습니다.";
            return out;
        }
        inside.push_back(static_cast<int>(i));
    }
    if (inside.size() < 2) {
        out.error = "합칠 칸이 둘 이상이어야 합니다.";
        return out;
    }
    std::sort(inside.begin(), inside.end(), [&](int a, int b) {
        const TableCell& x = table.cells[static_cast<std::size_t>(a)];
        const TableCell& y = table.cells[static_cast<std::size_t>(b)];
        return x.row != y.row ? x.row < y.row : x.col < y.col;
    });
    const int target = inside.front();
    std::vector<Paragraph> paragraphs;
    for (int i : inside) {
        TableCell& c = table.cells[static_cast<std::size_t>(i)];
        if (cellEmpty(c)) continue;
        for (Paragraph& para : c.content.paragraphs) {
            para.lineSegs.clear();
            paragraphs.push_back(std::move(para));
        }
    }
    const std::vector<std::int32_t> widths = tableColumnWidths(table);
    const std::vector<std::int32_t> heights = tableRowHeights(table);
    TableCell& t = table.cells[static_cast<std::size_t>(target)];
    if (!paragraphs.empty()) t.content.paragraphs = std::move(paragraphs);
    clearLines(t);
    t.rowSpan = static_cast<std::uint32_t>(row1 - row0 + 1);
    t.colSpan = static_cast<std::uint32_t>(col1 - col0 + 1);
    t.width = static_cast<std::uint32_t>(sumRange(widths, col0, col1));
    t.height = static_cast<std::uint32_t>(sumRange(heights, row0, row1));
    std::vector<TableCell> kept;
    int focus = -1;
    for (std::size_t i = 0; i < table.cells.size(); ++i) {
        if (static_cast<int>(i) != target && std::find(inside.begin(), inside.end(), static_cast<int>(i)) != inside.end()) {
            continue;
        }
        if (static_cast<int>(i) == target) focus = static_cast<int>(kept.size());
        kept.push_back(std::move(table.cells[i]));
    }
    table.cells = std::move(kept);
    out.focusCell = finish(table, focus);
    return out;
}

namespace {

// 크기 sizes 의 경계 위치 [0, s0, s0+s1, ...].
std::vector<std::int64_t> edges(const std::vector<std::int32_t>& sizes) {
    std::vector<std::int64_t> out(1, 0);
    for (const std::int32_t s : sizes) out.push_back(out.back() + s);
    return out;
}

// 줄(rows=true)이나 칸 경계를 위치 pos 에 긋는다. 이미 있으면 그 번호를, 새로 그으면 새 번호를 돌려준다.
int addLine(Table& table, std::vector<std::int32_t>& sizes, std::int64_t pos, bool rows) {
    const std::vector<std::int64_t> e = edges(sizes);
    for (std::size_t j = 0; j < e.size(); ++j) {
        if (e[j] == pos) return static_cast<int>(j);
    }
    std::size_t j = 0;
    while (j + 1 < e.size() && e[j + 1] < pos) ++j;
    // 경계 j+1 자리에 새 선. 그 줄(칸) j 가 둘로 나뉜다.
    const auto split = static_cast<int>(j);
    for (TableCell& c : table.cells) {
        std::uint32_t& start = rows ? c.row : c.col;
        std::uint32_t& count = rows ? c.rowSpan : c.colSpan;
        const int a = static_cast<int>(start);
        const int b = a + span(count) - 1;
        if (a > split) ++start;
        else if (b >= split) count = static_cast<std::uint32_t>(span(count) + 1);
    }
    const std::int32_t left = static_cast<std::int32_t>(pos - e[j]);
    sizes.insert(sizes.begin() + split + 1, sizes[static_cast<std::size_t>(split)] - left);
    sizes[static_cast<std::size_t>(split)] = left;
    return split + 1;
}

// whole 을 parts 로 똑같이 나누고 나머지를 마지막에 준다.
std::vector<std::int32_t> evenParts(std::int32_t whole, int parts) {
    std::vector<std::int32_t> out(static_cast<std::size_t>(parts), whole / parts);
    out.back() = whole - (whole / parts) * (parts - 1);
    return out;
}

}  // namespace

namespace {

// 사각형에 걸친 칸이 다 들도록 사각형을 넓힌다.
void coverCells(const Table& table, int& row0, int& col0, int& row1, int& col1) {
    for (bool grew = true; grew;) {
        grew = false;
        for (const TableCell& c : table.cells) {
            const bool overlaps = static_cast<int>(c.row) <= row1 && lastRow(c) >= row0 && static_cast<int>(c.col) <= col1 &&
                                  lastCol(c) >= col0;
            if (!overlaps) continue;
            if (static_cast<int>(c.row) < row0) row0 = static_cast<int>(c.row), grew = true;
            if (static_cast<int>(c.col) < col0) col0 = static_cast<int>(c.col), grew = true;
            if (lastRow(c) > row1) row1 = lastRow(c), grew = true;
            if (lastCol(c) > col1) col1 = lastCol(c), grew = true;
        }
    }
}

std::string checkBlock(const Table& table, int& row0, int& col0, int& row1, int& col1) {
    if (!tableGridValid(table)) return "표 칸이 겹치거나 비어 있는 자리가 있어 고칠 수 없습니다.";
    if (row0 > row1) std::swap(row0, row1);
    if (col0 > col1) std::swap(col0, col1);
    if (row0 < 0 || col0 < 0 || row1 >= static_cast<int>(table.rowCount) || col1 >= static_cast<int>(table.colCount)) {
        return "표에 그 칸이 없습니다.";
    }
    coverCells(table, row0, col0, row1, col1);
    return {};
}

int firstBlockCell(const Table& table, int row0, int col0) {
    for (std::size_t i = 0; i < table.cells.size(); ++i) {
        if (static_cast<int>(table.cells[i].row) == row0 && static_cast<int>(table.cells[i].col) == col0) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

TableEdit setTableCellBorders(Table& table, int row0, int col0, int row1, int col1, CellBorderKind kind,
                              const BorderSide& line, const BorderFillAccess& fills) {
    TableEdit out;
    if (out.error = checkBlock(table, row0, col0, row1, col1); !out.ok()) return out;
    const auto inside = [&](const TableCell& c) {
        return static_cast<int>(c.row) >= row0 && lastRow(c) <= row1 && static_cast<int>(c.col) >= col0 && lastCol(c) <= col1;
    };
    std::vector<BorderFill> fill;
    fill.reserve(table.cells.size());
    for (const TableCell& c : table.cells) fill.push_back(fills.read(c.borderFillId));

    if (kind == CellBorderKind::DiagonalDown || kind == CellBorderKind::DiagonalUp) {
        const bool down = kind == CellBorderKind::DiagonalDown;
        bool allOn = true;
        for (std::size_t i = 0; i < table.cells.size(); ++i) {
            if (!inside(table.cells[i])) continue;
            const DiagonalSpec& d = down ? fill[i].backSlash : fill[i].slash;
            if (d.type == "NONE" || d.type.empty()) allOn = false;
        }
        for (std::size_t i = 0; i < table.cells.size(); ++i) {
            if (!inside(table.cells[i])) continue;
            DiagonalSpec& d = down ? fill[i].backSlash : fill[i].slash;
            d.type = allOn ? "NONE" : "CENTER";
            if (!allOn && (fill[i].diagonal.type == "NONE" || fill[i].diagonal.type.empty())) {
                fill[i].diagonal = BorderSide{"SOLID", line.color, mmToHu(0.1)};
            }
            table.cells[i].borderFillId = fills.intern(fill[i]);
        }
        out.focusCell = firstBlockCell(table, row0, col0);
        return out;
    }

    // 칸 한 변이 놓인 선의 갈래.
    enum Edge { Top, Bottom, Left, Right, InnerH, InnerV, Off };
    const auto edgeOf = [&](const TableCell& c, int side) -> Edge {
        const bool in = inside(c);
        if (side <= 1) {  // 0 위, 1 아래
            if (static_cast<int>(c.col) > col1 || lastCol(c) < col0) return Off;
            const int y = side == 0 ? static_cast<int>(c.row) : lastRow(c) + 1;
            if (y == row0) return (in && side == 0) || (!in && side == 1) ? Top : Off;
            if (y == row1 + 1) return (in && side == 1) || (!in && side == 0) ? Bottom : Off;
            return in ? InnerH : Off;
        }
        if (static_cast<int>(c.row) > row1 || lastRow(c) < row0) return Off;  // 2 왼쪽, 3 오른쪽
        const int x = side == 2 ? static_cast<int>(c.col) : lastCol(c) + 1;
        if (x == col0) return (in && side == 2) || (!in && side == 3) ? Left : Off;
        if (x == col1 + 1) return (in && side == 3) || (!in && side == 2) ? Right : Off;
        return in ? InnerV : Off;
    };
    bool want[6] = {false, false, false, false, false, false};
    switch (kind) {
        case CellBorderKind::All:
        case CellBorderKind::None: std::fill(std::begin(want), std::end(want), true); break;
        case CellBorderKind::Outside: want[Top] = want[Bottom] = want[Left] = want[Right] = true; break;
        case CellBorderKind::Inside: want[InnerH] = want[InnerV] = true; break;
        case CellBorderKind::Top: want[Top] = true; break;
        case CellBorderKind::Bottom: want[Bottom] = true; break;
        case CellBorderKind::Left: want[Left] = true; break;
        case CellBorderKind::Right: want[Right] = true; break;
        case CellBorderKind::InsideHorz: want[InnerH] = true; break;
        case CellBorderKind::InsideVert: want[InnerV] = true; break;
        default: break;
    }
    const auto sideOf = [](BorderFill& f, int side) -> BorderSide& {
        return side == 0 ? f.top : side == 1 ? f.bottom : side == 2 ? f.left : f.right;
    };
    // 블록 칸의 그 선들이 모두 그어져 있으면 지운다(한글 단추는 켜고 끄기다). 없음 단추는 늘 지운다.
    bool any = false;
    bool allOn = true;
    for (std::size_t i = 0; i < table.cells.size(); ++i) {
        if (!inside(table.cells[i])) continue;
        for (int side = 0; side < 4; ++side) {
            const Edge e = edgeOf(table.cells[i], side);
            if (e == Off || !want[e]) continue;
            any = true;
            const BorderSide& s = sideOf(fill[i], side);
            if (s.type == "NONE" || s.type.empty()) allOn = false;
        }
    }
    if (!any) {
        out.error = "고칠 테두리가 없습니다.";
        return out;
    }
    const bool on = kind != CellBorderKind::None && !allOn;
    const BorderSide off{"NONE", "#000000", mmToHu(0.1)};
    for (std::size_t i = 0; i < table.cells.size(); ++i) {
        bool touched = false;
        for (int side = 0; side < 4; ++side) {
            const Edge e = edgeOf(table.cells[i], side);
            if (e == Off || !want[e]) continue;
            sideOf(fill[i], side) = on ? line : off;
            touched = true;
        }
        if (touched) table.cells[i].borderFillId = fills.intern(fill[i]);
    }
    out.focusCell = firstBlockCell(table, row0, col0);
    return out;
}

TableEdit fillTableCells(Table& table, int row0, int col0, int row1, int col1, const std::string& faceColor,
                         const BorderFillAccess& fills) {
    TableEdit out;
    if (out.error = checkBlock(table, row0, col0, row1, col1); !out.ok()) return out;
    for (TableCell& c : table.cells) {
        if (static_cast<int>(c.row) < row0 || lastRow(c) > row1 || static_cast<int>(c.col) < col0 || lastCol(c) > col1) continue;
        BorderFill f = fills.read(c.borderFillId);
        f.faceColor = faceColor.empty() ? "none" : faceColor;
        c.borderFillId = fills.intern(f);
    }
    out.focusCell = firstBlockCell(table, row0, col0);
    return out;
}

TableEdit equalizeTableCells(Table& table, int row0, int col0, int row1, int col1, bool heights,
                             std::vector<std::int32_t> lineSizes) {
    TableEdit out;
    if (out.error = checkBlock(table, row0, col0, row1, col1); !out.ok()) return out;
    std::vector<std::int32_t> sizes = heights ? std::move(lineSizes) : tableColumnWidths(table);
    if (heights && sizes.size() != table.rowCount) sizes = tableRowHeights(table);
    const std::vector<std::int64_t> e = edges(sizes);
    // 크기를 맞추는 쪽(along)과 그와 엇갈린 쪽(across). 높이면 along 은 줄, across 는 칸이다.
    const auto start = [&](const TableCell& c) { return static_cast<int>(heights ? c.row : c.col); };
    const auto finishAt = [&](const TableCell& c) { return heights ? lastRow(c) : lastCol(c); };
    const auto crossStart = [&](const TableCell& c) { return static_cast<int>(heights ? c.col : c.row); };
    const auto crossLast = [&](const TableCell& c) { return heights ? lastCol(c) : lastRow(c); };
    const int a0 = heights ? row0 : col0;
    const int a1 = heights ? row1 : col1;
    const int p0 = heights ? col0 : row0;
    const int p1 = heights ? col1 : row1;
    const std::int64_t blockStart = e[static_cast<std::size_t>(a0)];
    const std::int64_t blockEnd = e[static_cast<std::size_t>(a1) + 1];
    const std::int64_t total = blockEnd - blockStart;
    const std::size_t n = table.cells.size();
    std::vector<std::int64_t> from(n), to(n);
    std::vector<bool> placed(n, false);
    for (std::size_t i = 0; i < n; ++i) {
        from[i] = e[static_cast<std::size_t>(start(table.cells[i]))];
        to[i] = e[static_cast<std::size_t>(finishAt(table.cells[i])) + 1];
    }
    const auto inBlock = [&](const TableCell& c) {
        return start(c) >= a0 && finishAt(c) <= a1 && crossStart(c) >= p0 && crossLast(c) <= p1;
    };
    // 줄(칸)마다 블록 칸이 블록 크기를 똑같이 나눈다. 나머지는 올린다.
    std::vector<std::int64_t> lineEnd;
    std::vector<int> lineLast;
    bool many = false;
    for (int p = p0; p <= p1; ++p) {
        std::vector<int> line;
        for (std::size_t i = 0; i < n; ++i) {
            const TableCell& c = table.cells[i];
            if (inBlock(c) && crossStart(c) <= p && crossLast(c) >= p) line.push_back(static_cast<int>(i));
        }
        std::sort(line.begin(), line.end(), [&](int x, int y) { return from[static_cast<std::size_t>(x)] < from[static_cast<std::size_t>(y)]; });
        if (line.size() > 1) many = true;
        const std::int64_t count = std::max<std::int64_t>(1, static_cast<std::int64_t>(line.size()));
        const std::int64_t part = std::max<std::int64_t>(1, (total + count - 1) / count);
        std::int64_t pos = blockStart;
        int last = -1;
        for (const int i : line) {
            const auto k = static_cast<std::size_t>(i);
            if (placed[k]) {
                pos = to[k];
                last = -1;
                continue;
            }
            from[k] = pos;
            to[k] = pos + part;
            placed[k] = true;
            pos = to[k];
            last = i;
        }
        lineEnd.push_back(pos);
        lineLast.push_back(last);
    }
    if (!many) {
        out.error = "셀을 둘 이상 블록으로 고르세요.";
        return out;
    }
    const std::int64_t target = *std::max_element(lineEnd.begin(), lineEnd.end());
    for (std::size_t k = 0; k < lineEnd.size(); ++k) {
        if (lineEnd[k] < target && lineLast[k] >= 0) to[static_cast<std::size_t>(lineLast[k])] = target;
    }
    const std::int64_t delta = target - blockEnd;
    for (std::size_t i = 0; i < n; ++i) {
        if (placed[i]) continue;
        if (from[i] >= blockEnd) from[i] += delta;
        if (to[i] >= blockEnd) to[i] += delta;
    }
    // 새 경계선으로 칸 자리를 다시 매긴다.
    std::vector<std::int64_t> lines;
    for (std::size_t i = 0; i < n; ++i) {
        lines.push_back(from[i]);
        lines.push_back(to[i]);
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    const auto indexOf = [&](std::int64_t pos) {
        return static_cast<int>(std::lower_bound(lines.begin(), lines.end(), pos) - lines.begin());
    };
    const int focusRow = row0;
    const int focusCol = col0;
    int focus = firstBlockCell(table, focusRow, focusCol);
    for (std::size_t i = 0; i < n; ++i) {
        TableCell& c = table.cells[i];
        const int a = indexOf(from[i]);
        const int b = indexOf(to[i]);
        if (b <= a) {
            out.error = "칸이 너무 작아 크기를 맞출 수 없습니다.";
            return out;
        }
        if (heights) {
            c.row = static_cast<std::uint32_t>(a);
            c.rowSpan = static_cast<std::uint32_t>(b - a);
            if (placed[i]) c.height = static_cast<std::uint32_t>(to[i] - from[i]);
        } else {
            c.col = static_cast<std::uint32_t>(a);
            c.colSpan = static_cast<std::uint32_t>(b - a);
            c.width = static_cast<std::uint32_t>(to[i] - from[i]);
        }
        clearLines(c);
    }
    if (heights) {
        table.rowCount = static_cast<std::uint32_t>(lines.size() - 1);
        out.heightDelta = static_cast<std::int32_t>(delta);
    } else {
        table.colCount = static_cast<std::uint32_t>(lines.size() - 1);
        out.widthDelta = static_cast<std::int32_t>(delta);
    }
    out.focusCell = finish(table, focus);
    return out;
}

TableEdit splitTableCell(Table& table, int cell, int rows, int cols, std::int32_t firstHeight,
                         std::int32_t emptyHeight) {
    TableEdit out;
    if (out.error = checkTable(table, cell); !out.ok()) return out;
    if (rows < 1 || cols < 1 || (rows == 1 && cols == 1)) {
        out.error = "나눌 줄·칸 수는 1 이상이고 둘 다 1 일 수는 없습니다.";
        return out;
    }
    std::vector<std::int32_t> widths = tableColumnWidths(table);
    std::vector<std::int32_t> heights = tableRowHeights(table);
    const TableCell base = table.cells[static_cast<std::size_t>(cell)];
    const int r0 = static_cast<int>(base.row);
    const int c0 = static_cast<int>(base.col);
    const std::int32_t whole = sumRange(widths, c0, lastCol(base));
    const std::int32_t tall = sumRange(heights, r0, lastRow(base));
    if (whole / cols < 1 || tall < 1) {
        out.error = "칸이 너무 작아 나눌 수 없습니다.";
        return out;
    }
    // 칸 나눔은 폭을 똑같이 나눈 자리에 경계를 긋는다. 이미 그 자리에 선이 있으면 그 선을 쓴다.
    const std::vector<std::int32_t> colParts = evenParts(whole, cols);
    std::vector<int> colLines{c0};
    {
        std::int64_t pos = edges(widths)[static_cast<std::size_t>(c0)];
        for (int k = 0; k + 1 < cols; ++k) {
            pos += colParts[static_cast<std::size_t>(k)];
            colLines.push_back(addLine(table, widths, pos, false));
        }
    }
    const TableCell& now = table.cells[static_cast<std::size_t>(cell)];
    colLines.push_back(lastCol(now) + 1);

    // 줄 나눔. 칸이 걸친 줄 수가 나눌 줄 수의 배수면 있는 경계를 쓰고, 아니면 높이를 똑같이 나눈 자리에 긋는다.
    std::vector<int> rowLines{r0};
    const int rowSpan = span(now.rowSpan);
    if (rowSpan % rows == 0) {
        for (int k = 1; k < rows; ++k) rowLines.push_back(r0 + k * (rowSpan / rows));
    } else {
        const std::vector<std::int32_t> rowParts = evenParts(tall, rows);
        std::int64_t pos = edges(heights)[static_cast<std::size_t>(r0)];
        for (int k = 0; k + 1 < rows; ++k) {
            pos += rowParts[static_cast<std::size_t>(k)];
            rowLines.push_back(addLine(table, heights, pos, true));
        }
    }
    rowLines.push_back(lastRow(table.cells[static_cast<std::size_t>(cell)]) + 1);

    // 나눈 칸의 높이 값: 첫 줄은 글 높이, 가운데 줄은 빈 줄 높이, 마지막 줄은 남은 높이(한글 2024).
    const std::int32_t least = base.hasMargin ? base.marginTop + base.marginBottom : table.marginTop + table.marginBottom;
    std::vector<std::int32_t> pieceHeights(static_cast<std::size_t>(rows), 0);
    if (rows > 1) {
        std::int32_t used = 0;
        for (int i = 0; i + 1 < rows; ++i) {
            pieceHeights[static_cast<std::size_t>(i)] = std::max(least, i == 0 ? firstHeight : emptyHeight);
            used += pieceHeights[static_cast<std::size_t>(i)];
        }
        pieceHeights.back() = std::max(least, tall - used);
    } else {
        pieceHeights[0] = static_cast<std::int32_t>(base.height);
    }
    std::vector<TableCell> pieces;
    for (int i = 0; i < rows; ++i) {
        for (int j = 0; j < cols; ++j) {
            TableCell piece = i == 0 && j == 0 ? table.cells[static_cast<std::size_t>(cell)] : emptyCellLike(base);
            piece.row = static_cast<std::uint32_t>(rowLines[static_cast<std::size_t>(i)]);
            piece.rowSpan = static_cast<std::uint32_t>(rowLines[static_cast<std::size_t>(i) + 1] - rowLines[static_cast<std::size_t>(i)]);
            piece.col = static_cast<std::uint32_t>(colLines[static_cast<std::size_t>(j)]);
            piece.colSpan = static_cast<std::uint32_t>(colLines[static_cast<std::size_t>(j) + 1] - colLines[static_cast<std::size_t>(j)]);
            piece.width = static_cast<std::uint32_t>(colParts[static_cast<std::size_t>(j)]);
            piece.height = static_cast<std::uint32_t>(pieceHeights[static_cast<std::size_t>(i)]);
            clearLines(piece);
            pieces.push_back(std::move(piece));
        }
    }
    table.cells.erase(table.cells.begin() + cell);
    const int focus = static_cast<int>(table.cells.size());
    for (TableCell& piece : pieces) table.cells.push_back(std::move(piece));
    out.focusCell = finish(table, focus);
    return out;
}

namespace {

// 블록에 다 든 칸마다 f 를 부른다.
template <typename F>
TableEdit forBlockCells(Table& table, int row0, int col0, int row1, int col1, F&& f) {
    TableEdit out;
    if (out.error = checkBlock(table, row0, col0, row1, col1); !out.ok()) return out;
    for (TableCell& c : table.cells) {
        if (static_cast<int>(c.row) < row0 || lastRow(c) > row1 || static_cast<int>(c.col) < col0 || lastCol(c) > col1) continue;
        f(c);
    }
    out.focusCell = firstBlockCell(table, row0, col0);
    return out;
}

}  // namespace

TableEdit alignTableCells(Table& table, int row0, int col0, int row1, int col1, const std::string& vertAlign) {
    if (vertAlign != "TOP" && vertAlign != "CENTER" && vertAlign != "BOTTOM") {
        TableEdit out;
        out.error = "세로 정렬 " + vertAlign + " 은 모릅니다.";
        return out;
    }
    return forBlockCells(table, row0, col0, row1, col1, [&](TableCell& c) {
        c.vertAlign = vertAlign;
        c.content.vertAlign = vertAlign;
    });
}

TableEdit setTableCellDirection(Table& table, int row0, int col0, int row1, int col1, std::uint8_t direction) {
    return forBlockCells(table, row0, col0, row1, col1, [&](TableCell& c) {
        c.content.textDirection = std::min<std::uint8_t>(direction, 2);
        clearLines(c);
    });
}

TableEdit splitTableRows(Table& table, int row, Table& tail) {
    TableEdit out;
    if (!tableGridValid(table)) {
        out.error = "표 칸이 겹치거나 비어 있는 자리가 있어 나눌 수 없습니다.";
        return out;
    }
    if (row <= 0 || row >= static_cast<int>(table.rowCount)) {
        out.error = "표의 첫 줄에서는 표를 나눌 수 없습니다.";
        return out;
    }
    for (const TableCell& c : table.cells) {
        if (static_cast<int>(c.row) < row && lastRow(c) >= row) {
            out.error = "나눌 자리를 가로지르는 합친 칸이 있어 표를 나눌 수 없습니다.";
            return out;
        }
    }
    tail = table;
    tail.cells.clear();
    std::vector<TableCell> head;
    for (TableCell& c : table.cells) {
        if (static_cast<int>(c.row) >= row) {
            c.row -= static_cast<std::uint32_t>(row);
            tail.cells.push_back(std::move(c));
        } else {
            head.push_back(std::move(c));
        }
    }
    table.cells = std::move(head);
    normalize(table, nullptr);
    normalize(tail, nullptr);
    out.focusCell = 0;
    return out;
}

TableEdit appendTableRows(Table& table, Table next) {
    TableEdit out;
    if (!tableGridValid(table) || !tableGridValid(next)) {
        out.error = "표 칸이 겹치거나 비어 있는 자리가 있어 붙일 수 없습니다.";
        return out;
    }
    const std::vector<std::int64_t> a = edges(tableColumnWidths(table));
    const std::vector<std::int64_t> b = edges(tableColumnWidths(next));
    // 두 표의 경계를 합친다. 가까운 경계는 앞 표의 것을 쓴다.
    constexpr std::int64_t kSnap = 2;
    std::vector<std::int64_t> all = a;
    for (const std::int64_t e : b) {
        if (std::none_of(all.begin(), all.end(), [&](std::int64_t x) { return std::llabs(x - e) <= kSnap; })) all.push_back(e);
    }
    std::sort(all.begin(), all.end());
    const auto indexOf = [&](std::int64_t e) {
        for (std::size_t j = 0; j < all.size(); ++j) {
            if (std::llabs(all[j] - e) <= kSnap) return static_cast<int>(j);
        }
        return 0;
    };
    const auto regrid = [&](Table& t, const std::vector<std::int64_t>& own, std::uint32_t rowBase) {
        for (TableCell& c : t.cells) {
            const int first = indexOf(own[c.col]);
            const int last = indexOf(own[static_cast<std::size_t>(lastCol(c)) + 1]);
            c.col = static_cast<std::uint32_t>(first);
            c.colSpan = static_cast<std::uint32_t>(std::max(1, last - first));
            c.row += rowBase;
        }
    };
    regrid(table, a, 0);
    regrid(next, b, table.rowCount);
    const std::int64_t oldWidth = a.back();
    for (TableCell& c : next.cells) table.cells.push_back(std::move(c));
    normalize(table, nullptr);
    out.widthDelta = static_cast<std::int32_t>(std::max(a.back(), b.back()) - oldWidth);
    out.focusCell = 0;
    return out;
}

namespace {

// 표 뒤집기의 세 바탕 움직임. 회전은 이것들을 잇는다.
enum class FlipStep { Rows, Columns, Diagonal };

// 대각선 모양의 아래·위 덧선(CENTER_BELOW ↔ CENTER_ABOVE). 가운데 선만이거나 모두면 그대로다.
std::string swapDiagonalExtra(const std::string& type) {
    if (type == "CENTER_BELOW") return "CENTER_ABOVE";
    if (type == "CENTER_ABOVE") return "CENTER_BELOW";
    return type;
}

// 한글 2024 TableSwap 결과(HWPX)로 맞춘 테두리/배경 바꾸기. / 는 slash, \ 는 backSlash 이고, counter 는 isCounter 다.
void flipFill(BorderFill& fill, FlipStep step) {
    switch (step) {
        case FlipStep::Rows: {
            std::swap(fill.top, fill.bottom);
            std::swap(fill.slash, fill.backSlash);
            fill.slash.counter = !fill.slash.counter;
            fill.backSlash.counter = !fill.backSlash.counter;
            break;
        }
        case FlipStep::Columns:
            std::swap(fill.left, fill.right);
            std::swap(fill.slash, fill.backSlash);
            break;
        case FlipStep::Diagonal: {
            BorderSide left = fill.left;
            BorderSide right = fill.right;
            fill.left = fill.top;
            fill.right = fill.bottom;
            fill.top = std::move(left);
            fill.bottom = std::move(right);
            fill.slash.type = swapDiagonalExtra(fill.slash.type);
            fill.backSlash.type = swapDiagonalExtra(fill.backSlash.type);
            fill.slash.counter = !fill.slash.counter;
            break;
        }
    }
}

// 왼·오른·위·아래 여백 네 값을 같은 규칙으로 돌린다.
void flipMargins(std::int32_t& left, std::int32_t& right, std::int32_t& top, std::int32_t& bottom, FlipStep step) {
    switch (step) {
        case FlipStep::Rows: std::swap(top, bottom); break;
        case FlipStep::Columns: std::swap(left, right); break;
        case FlipStep::Diagonal:
            std::swap(left, top);
            std::swap(right, bottom);
            break;
    }
}

std::vector<FlipStep> flipSteps(TableFlip flip) {
    switch (flip) {
        case TableFlip::Rows: return {FlipStep::Rows};
        case TableFlip::Columns: return {FlipStep::Columns};
        case TableFlip::Diagonal: return {FlipStep::Diagonal};
        case TableFlip::CounterClockwise: return {FlipStep::Diagonal, FlipStep::Rows};
        case TableFlip::Half: return {FlipStep::Columns, FlipStep::Rows};
        case TableFlip::Clockwise: return {FlipStep::Diagonal, FlipStep::Columns};
    }
    return {};
}

}  // namespace

TableEdit flipTable(Table& table, TableFlip flip, bool swapMargins, const BorderFillAccess& fills, int focusCell) {
    TableEdit out;
    if (table.cells.empty()) {
        out.error = "표에 칸이 없습니다.";
        return out;
    }
    if (!tableGridValid(table)) {
        out.error = "표 칸이 겹치거나 비어 있는 자리가 있어 뒤집을 수 없습니다.";
        return out;
    }
    const auto sum = [](const std::vector<std::int32_t>& sizes) {
        return std::accumulate(sizes.begin(), sizes.end(), std::int32_t{0});
    };
    const std::int32_t oldWidth = sum(tableColumnWidths(table));
    const std::int32_t oldHeight = sum(tableRowHeights(table));
    const std::vector<FlipStep> steps = flipSteps(flip);

    // 같은 테두리/배경을 쓰는 칸은 같은 새 번호를 받는다. 0 은 테두리/배경이 없다는 뜻이라 두다.
    std::map<std::uint32_t, std::uint32_t> flipped;
    const auto flipFillId = [&](std::uint32_t id) {
        if (id == 0 || !fills.read || !fills.intern) return id;
        if (const auto it = flipped.find(id); it != flipped.end()) return it->second;
        BorderFill fill = fills.read(id);
        for (const FlipStep step : steps) flipFill(fill, step);
        const std::uint32_t next = fills.intern(fill);
        flipped.emplace(id, next);
        return next;
    };
    table.borderFillId = flipFillId(table.borderFillId);

    for (const FlipStep step : steps) {
        const int rows = static_cast<int>(table.rowCount);
        const int cols = static_cast<int>(table.colCount);
        for (TableCell& cell : table.cells) {
            switch (step) {
                case FlipStep::Rows: cell.row = static_cast<std::uint32_t>(rows - 1 - lastRow(cell)); break;
                case FlipStep::Columns: cell.col = static_cast<std::uint32_t>(cols - 1 - lastCol(cell)); break;
                case FlipStep::Diagonal:
                    std::swap(cell.row, cell.col);
                    std::swap(cell.rowSpan, cell.colSpan);
                    std::swap(cell.width, cell.height);
                    break;
            }
            if (swapMargins) flipMargins(cell.marginLeft, cell.marginRight, cell.marginTop, cell.marginBottom, step);
        }
        if (step == FlipStep::Diagonal) std::swap(table.rowCount, table.colCount);
        if (swapMargins) flipMargins(table.marginLeft, table.marginRight, table.marginTop, table.marginBottom, step);
    }
    for (TableCell& cell : table.cells) {
        cell.borderFillId = flipFillId(cell.borderFillId);
        clearLines(cell);
    }
    out.focusCell = finish(table, focusCell);
    out.widthDelta = sum(tableColumnWidths(table)) - oldWidth;
    out.heightDelta = sum(tableRowHeights(table)) - oldHeight;
    return out;
}

}  // namespace chwp::model
