#include "chwp/agent.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "chwp/app/doc_session.hpp"
#include "chwp/app/session.hpp"
#include "chwp/export/font.hpp"
#include "chwp/model/table_edit.hpp"
#include "chwp/reader.hpp"
#include "chwp/render/edit_state.hpp"
#include "chwp/render/frame_cache.hpp"
#include "chwp/render/format_view.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "chwp/writer.hpp"
#include "export/font_metrics.hpp"
#include "render/paint_detail.hpp"
#include "render/picture.hpp"

// 에이전트 API 구현. 판정·조판·편집은 render 쪽 함수를 그대로 부르고, 여기서는 입구 검사와 모양 바꾸기만 한다.

namespace chwp::agent {

namespace {

using model::ControlKind;

Status fail(Code code, std::string message) {
    Status status;
    status.code = code;
    status.message = std::move(message);
    return status;
}

// 코드 포인트 수. 이어지는 바이트(10xxxxxx)만 빼고 센다. 편집기의 글자 번호와 같은 셈이다.
int codePoints(const std::string& text) {
    int n = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

std::string colorText(const model::Color& color) {
    if (!color) return {};
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned>(*color & 0xFFFFFFu));
    return buf;
}

std::string fontFace(const model::Document& doc, model::Lang lang, std::uint32_t ref) {
    const auto& fonts = doc.header.fonts[static_cast<std::size_t>(lang)];
    return ref < fonts.size() ? fonts[ref].face : std::string();
}

TextFormat formatOf(const model::Document& doc, std::uint32_t charShapeId) {
    TextFormat out;
    if (charShapeId >= doc.header.charShapes.size()) return out;
    const model::CharShape& shape = doc.header.charShapes[charShapeId];
    out.font = fontFace(doc, model::Lang::Hangul, shape.fontRef[static_cast<std::size_t>(model::Lang::Hangul)]);
    out.latinFont = fontFace(doc, model::Lang::Latin, shape.fontRef[static_cast<std::size_t>(model::Lang::Latin)]);
    out.size = shape.height;
    out.bold = shape.bold;
    out.italic = shape.italic;
    out.underline = shape.underline;
    out.strikeout = shape.strikeout;
    out.color = colorText(shape.textColor);
    return out;
}

const model::Style* styleById(const model::Document& doc, std::uint32_t id) {
    const auto& styles = doc.header.styles;
    if (id < styles.size() && styles[id].id == id) return &styles[id];
    for (const model::Style& style : styles) {
        if (style.id == id) return &style;
    }
    return nullptr;
}

// 개체로 내보이는 컨트롤. 구역 정의·쪽 번호 위치·책갈피처럼 보이지 않는 조판 지시는 넣지 않는다.
const char* objectKind(const model::Control& ctrl) {
    switch (ctrl.kind) {
        case ControlKind::Table: return "table";
        case ControlKind::Picture: return "picture";
        case ControlKind::Shape: return "shape";
        case ControlKind::Container: return "container";
        case ControlKind::Ole: return "ole";
        case ControlKind::Equation: return "equation";
        case ControlKind::Footnote: return "footnote";
        case ControlKind::Endnote: return "endnote";
        case ControlKind::Header: return "header";
        case ControlKind::Footer: return "footer";
        default: return nullptr;
    }
}

std::string ownerOf(const model::StorySlot& slot) {
    switch (slot.kind) {
        case model::StoryKind::Section: return "body";
        case model::StoryKind::TableCell: return "table";
        case model::StoryKind::Caption: return "caption";
        case model::StoryKind::MasterPage: return "masterPage";
        case model::StoryKind::SubList:
            if (!slot.control) return "subList";
            switch (slot.control->kind) {
                case ControlKind::Header: return "header";
                case ControlKind::Footer: return "footer";
                case ControlKind::Footnote: return "footnote";
                case ControlKind::Endnote: return "endnote";
                default: return "textBox";
            }
        default: return "unknown";
    }
}

std::string storyKindText(model::StoryKind kind) {
    switch (kind) {
        case model::StoryKind::Section: return "section";
        case model::StoryKind::TableCell: return "tableCell";
        case model::StoryKind::SubList: return "subList";
        case model::StoryKind::Caption: return "caption";
        case model::StoryKind::MasterPage: return "masterPage";
        default: return "none";
    }
}

bool sameRef(const PageRef& a, const PageRef& b) {
    return a.page == b.page && a.section == b.section && a.nth == b.nth;
}

// 코드 포인트마다 시작 바이트. 끝에 글 길이를 하나 더 둔다.
std::vector<std::size_t> codePointStarts(const std::string& text) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) starts.push_back(i);
    }
    starts.push_back(text.size());
    return starts;
}

std::string positionText(const Position& p) {
    return "{story " + std::to_string(p.story) + ", 문단 " + std::to_string(p.paragraph) + ", 글자 " +
           std::to_string(p.index) + "}";
}

// 앞이 먼저 오게 둔다.
Range ordered(Range range) {
    const Position& a = range.from;
    const Position& b = range.to;
    if (b.paragraph < a.paragraph || (b.paragraph == a.paragraph && b.index < a.index)) std::swap(range.from, range.to);
    return range;
}

bool sameSpot(const Position& a, const Position& b) {
    return a.story == b.story && a.paragraph == b.paragraph && a.index == b.index;
}

}  // namespace

const char* codeName(Code code) {
    switch (code) {
        case Code::Ok: return "Ok";
        case Code::BadArgument: return "BadArgument";
        case Code::PageNotFound: return "PageNotFound";
        case Code::AmbiguousPage: return "AmbiguousPage";
        case Code::PositionOutOfRange: return "PositionOutOfRange";
        case Code::CrossStory: return "CrossStory";
        case Code::ReadOnly: return "ReadOnly";
        case Code::NotEditable: return "NotEditable";
        case Code::Unsupported: return "Unsupported";
        case Code::ParseFailed: return "ParseFailed";
        case Code::SaveFailed: return "SaveFailed";
        case Code::RenderFailed: return "RenderFailed";
        case Code::Internal: return "Internal";
    }
    return "Internal";
}

Result<std::int32_t> parseLength(std::string_view text) {
    Result<std::int32_t> out;
    std::string raw(text);
    raw.erase(std::remove_if(raw.begin(), raw.end(), [](unsigned char c) { return c == ' ' || c == '\t'; }), raw.end());
    if (raw.empty()) {
        out.status = fail(Code::BadArgument, "길이가 비어 있습니다. 예: \"50mm\", \"12pt\", \"1234\"");
        return out;
    }
    const char* begin = raw.c_str();
    char* end = nullptr;
    errno = 0;
    const double number = std::strtod(begin, &end);
    if (end == begin || errno == ERANGE || !std::isfinite(number)) {
        out.status = fail(Code::BadArgument, "길이 \"" + raw + "\" 의 숫자를 읽지 못했습니다.");
        return out;
    }
    std::string unit(end);
    std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    double perUnit = 0;
    if (unit.empty() || unit == "hu") perUnit = 1;
    else if (unit == "mm") perUnit = 7200.0 / 25.4;
    else if (unit == "cm") perUnit = 72000.0 / 25.4;
    else if (unit == "in") perUnit = 7200.0;
    else if (unit == "pt") perUnit = 100.0;
    else {
        out.status = fail(Code::BadArgument, "길이 \"" + raw + "\" 의 단위 \"" + unit + "\" 를 모릅니다. mm, cm, in, pt, hu 를 씁니다.");
        return out;
    }
    const double hu = std::round(number * perUnit);
    if (hu < std::numeric_limits<std::int32_t>::min() || hu > std::numeric_limits<std::int32_t>::max()) {
        out.status = fail(Code::BadArgument, "길이 \"" + raw + "\" 가 너무 큽니다.");
        return out;
    }
    out.value = static_cast<std::int32_t>(hu);
    return out;
}

double huToMm(std::int32_t hu) {
    return static_cast<double>(hu) * 25.4 / 7200.0;
}

// ---------------------------------------------------------------------------

struct AgentDocument::Impl {
    // 문서·쪽·편집 상태는 편집기와 같은 문서 세션이 갖는다. 여기서는 위치를 커서로 바꿔 그 명령을 부른다.
    app::DocSession session;
    Bytes& source = session.file;
    model::Document& doc = session.doc;
    std::vector<render::Node>& pages = session.pages;
    model::SourceFormat format = model::SourceFormat::Unknown;
    std::vector<std::string> warnings;
    std::vector<PageInfo> pageInfos;
    // 쪽 그림을 GPU 로 구울 때 쓰는 칸 캐시. 그릴 때마다 비우고 그 쪽만 굽는다.
    render::FrameCache frames{render::kAgentFirstFrameId};

    void indexPages() {
        pageInfos.clear();
        pageInfos.reserve(pages.size());
        // 구역 안에서 같은 번호가 몇 번째인지 센다. 겹치는 번호에만 nth 를 남긴다.
        std::map<std::pair<int, int>, int> seen;
        std::map<std::pair<int, int>, int> total;
        for (const render::Node& paper : pages) ++total[{paper.section + 1, paper.pageNumber}];
        int order = 0;
        for (const render::Node& paper : pages) {
            PageInfo info;
            info.ref.page = paper.pageNumber;
            info.ref.section = paper.section + 1;
            const int count = ++seen[{info.ref.section, info.ref.page}];
            if (total[{info.ref.section, info.ref.page}] > 1) info.ref.nth = count;
            info.label = paper.ext().pageLabel.empty() ? std::to_string(paper.pageNumber) : paper.ext().pageLabel;
            info.numberHidden = paper.pageNumberHidden;
            info.order = ++order;
            info.width = paper.width;
            info.height = paper.height;
            const auto section = static_cast<std::size_t>(std::max(0, paper.section));
            info.landscape = section < doc.sections.size() && doc.sections[section].page &&
                             doc.sections[section].page->landscape;
            pageInfos.push_back(std::move(info));
        }
    }

    // ---- 편집. 판정은 편집기 명령(render)이 하고, 여기서는 위치를 커서로 바꿔 넘긴다.
    // apply 하나가 되돌리기 한 칸(묶음)이다.

    // ---- 글꼴. 무엇을 받을지는 편집기와 같은 app::fontFetchPlan 이 정한다.
    std::map<int, FontFile> fontJobs;     // 셸에 내준 뒤 아직 돌아오지 않은 파일
    std::set<std::string> fontFailed;     // 못 받은 파일 경로. 다시 내지 않는다
    std::set<std::string> fontFetched;    // 받은 파일 경로. 다시 내지 않는다(되묻기가 반드시 끝난다)
    std::vector<std::string> landedFaces; // fontsDone 앞에 들어온 글꼴 이름

    void relayoutAll() {
        session.relayoutAll();
        indexPages();
    }

    const std::vector<model::Paragraph>* storyParas(int story) const {
        const model::StoryMap& map = render::ensureStoryMap(doc);
        const model::StorySlot* slot = map.valid(story) ? map.slot(story) : nullptr;
        return slot ? slot->paragraphs : nullptr;
    }

    // 화면 별칭 번호(여러 쪽에 찍히는 머리말 등)를 stories() 의 번호로.
    int canonicalStory(int story) const {
        const model::StoryMap& map = render::ensureStoryMap(doc);
        const model::StorySlot* slot = map.valid(story) ? map.slot(story) : nullptr;
        return slot && slot->paragraphs ? map.storyOf(slot->paragraphs) : story;
    }

    Status check(const Position& p) const {
        const std::vector<model::Paragraph>* paras = storyParas(p.story);
        if (!paras) return fail(Code::PositionOutOfRange, "story " + std::to_string(p.story) + " 은 없습니다.");
        const int count = static_cast<int>(paras->size());
        if (p.paragraph < 0 || p.paragraph >= count) {
            return fail(Code::PositionOutOfRange, positionText(p) + ": 문단은 0~" + std::to_string(count - 1) + " 입니다.");
        }
        const int length = render::paint_detail::paragraphCharCount((*paras)[static_cast<std::size_t>(p.paragraph)]);
        if (p.index < 0 || p.index > length) {
            return fail(Code::PositionOutOfRange, positionText(p) + ": 글자는 0~" + std::to_string(length) + " 입니다.");
        }
        return {};
    }

    Status checkRange(const Range& range) const {
        if (Status s = check(range.from); !s.ok()) return s;
        if (Status s = check(range.to); !s.ok()) return s;
        if (range.from.story != range.to.story) {
            return fail(Code::CrossStory, "구간의 두 끝이 다른 story 입니다(" + std::to_string(range.from.story) + ", " +
                                              std::to_string(range.to.story) + ").");
        }
        return {};
    }

    // anchor 부터 focus 까지 블록(같으면 캐럿)을 잡는다.
    Status place(const Position& anchor, const Position& focus) {
        using render::paint_detail::charIndexToOffset;
        const int anchorOff = charIndexToOffset(doc, pages, anchor.story, anchor.paragraph, anchor.index);
        const int focusOff = charIndexToOffset(doc, pages, focus.story, focus.paragraph, focus.index);
        render::setPointerCursor(doc, pages, model::CursorPos{anchor.story, anchor.paragraph, anchorOff},
                                 model::CursorPos{focus.story, focus.paragraph, focusOff});
        // 앞 op 가 머리말·바탕쪽 편집에 들어갔어도 이 자리에 맞춘다(편집기의 그 자리 누르기와 같다).
        render::syncEditBand(doc);
        const render::PointerCursor cur = render::pointerCursor();
        if (!cur.active || canonicalStory(cur.story) != canonicalStory(anchor.story)) {
            return fail(Code::NotEditable, positionText(focus) + " 에 캐럿을 놓을 수 없습니다. 쪽에 놓이지 않은 글입니다.");
        }
        return {};
    }

    Position caret() const {
        const render::PointerCursor cur = render::pointerCursor();
        Position p;
        if (!cur.active) return p;
        p.story = canonicalStory(cur.story);
        p.paragraph = cur.paragraph;
        p.index = render::paint_detail::offsetToCharIndex(doc, pages, cur.story, cur.paragraph, cur.offset);
        return p;
    }

    static Status refused(const render::EditResult& result, const char* what) {
        if (result.needsFontSwap) {
            return fail(Code::Unsupported, std::string(what) + ": 라이선스로 막힌 글꼴이 있어 편집기가 받지 않았습니다.");
        }
        return fail(Code::NotEditable, std::string(what) + ": 그 자리는 고칠 수 없습니다.");
    }

    // 캐럿 자리에 글을 넣는다. 줄 끝은 문단 나눔, U+2028 은 문단 안 줄바꿈이다(편집기 붙여 넣기와 같은 DocSession 명령).
    Status typeText(const std::string& text) {
        for (const render::EditResult& r : session.insertPlainText(text)) {
            if (!r.documentChanged) return refused(r, "insertText");
        }
        return {};
    }

    Status insertText(const Position& at, const std::string& text) {
        if (Status s = check(at); !s.ok()) return s;
        if (Status s = place(at, at); !s.ok()) return s;
        return typeText(text);
    }

    Status erase(Range range) {
        if (Status s = checkRange(range); !s.ok()) return s;
        range = ordered(range);
        if (Status s = place(range.from, range.to); !s.ok()) return s;
        if (sameSpot(range.from, range.to)) return {};
        const render::EditResult r = session.keyDown(render::EditKey::Delete);
        if (!r.documentChanged) return refused(r, "delete");
        return {};
    }

    Status restyle(Range range, const Fields& fields, const std::string& styleName) {
        if (Status s = checkRange(range); !s.ok()) return s;
        range = ordered(range);
        render::FormatPatch patch;
        for (const auto& [key, value] : fields) {
            render::FormatValue v;
            std::visit([&](const auto& x) { v = x; }, value);
            if (!render::setFormatField(patch, key, v)) {
                return fail(Code::BadArgument, "서식 이름 " + key + " 을 모르거나 값의 꼴이 틀렸습니다.");
            }
        }
        if (!styleName.empty()) {
            const model::Style* found = nullptr;
            for (const model::Style& style : doc.header.styles) {
                if (style.name == styleName || style.engName == styleName) found = &style;
                if (found) break;
            }
            if (!found) return fail(Code::BadArgument, "스타일 " + styleName + " 은 이 문서에 없습니다.");
            patch.hasStyleId = true;
            patch.styleId = found->id;
        }
        if (Status s = place(range.from, range.to); !s.ok()) return s;
        const render::EditResult r = session.applyFormat(patch);
        if (!r.handled) return refused(r, "format");
        return {};
    }

    Status insertTable(const EditOp& op, OpReport& report) {
        // 편집기 표 넣기와 같은 상한(core 값)이다. 넘으면 core 가 잘라 넣어 칸 글이 빠지므로 미리 거절한다.
        if (op.rows < 1 || op.cols < 1 || op.rows > render::kTableMaxRows || op.cols > render::kTableMaxCols) {
            return fail(Code::BadArgument, "표는 1~" + std::to_string(render::kTableMaxRows) + " 줄, 1~" +
                                               std::to_string(render::kTableMaxCols) + " 칸입니다.");
        }
        if (Status s = check(op.at); !s.ok()) return s;
        if (Status s = place(op.at, op.at); !s.ok()) return s;
        const render::EditResult r = session.insertTable(op.rows, op.cols);
        if (!r.documentChanged) return refused(r, "insertTable");
        render::rebuildStoryMap(doc);
        // 캐럿이 든 첫 칸의 표를 찾아 칸 story 를 줄·칸 자리에 놓는다.
        const model::StoryMap& map = render::ensureStoryMap(doc);
        const model::StorySlot* first = map.slot(render::pointerCursor().story);
        if (!first || !first->control || !first->tableCell) {
            return fail(Code::Internal, "insertTable: 넣은 표의 칸을 찾지 못했습니다.");
        }
        report.cells.assign(static_cast<std::size_t>(op.rows), std::vector<int>(static_cast<std::size_t>(op.cols), -1));
        for (int s = 1; s <= map.count(); ++s) {
            const model::StorySlot* slot = map.slot(s);
            if (!slot || slot->control != first->control || !slot->tableCell || map.storyOf(slot->paragraphs) != s) continue;
            const auto row = static_cast<std::size_t>(slot->tableCell->row);
            const auto col = static_cast<std::size_t>(slot->tableCell->col);
            if (row < report.cells.size() && col < report.cells[row].size()) report.cells[row][col] = s;
        }
        for (std::size_t row = 0; row < op.cells.size() && row < report.cells.size(); ++row) {
            for (std::size_t col = 0; col < op.cells[row].size() && col < report.cells[row].size(); ++col) {
                if (op.cells[row][col].empty() || report.cells[row][col] < 0) continue;
                if (Status s = insertText({report.cells[row][col], 0, 0}, op.cells[row][col]); !s.ok()) return s;
            }
        }
        return {};
    }

    Status insertPicture(const EditOp& op, OpReport& report) {
        int pixelW = 0;
        int pixelH = 0;
        std::string extension;
        if (!render::paint_detail::imageInfo(op.image.data(), op.image.size(), pixelW, pixelH, extension)) {
            return fail(Code::BadArgument, "insertPicture: png·jpg·gif·bmp 그림 파일이 아닙니다.");
        }
        if (op.width < 0 || op.height < 0) return fail(Code::BadArgument, "insertPicture: width·height 는 0 이상입니다.");
        if (Status s = check(op.at); !s.ok()) return s;
        if (Status s = place(op.at, op.at); !s.ok()) return s;
        const render::EditResult r = session.insertPicture(op.image, op.width, op.height);
        if (!r.documentChanged) return refused(r, "insertPicture");
        render::rebuildStoryMap(doc);
        // 캐럿은 그림 바로 뒤다. 그 앞 글자(U+FFFC)인 그림의 개체 이름을 돌려준다.
        const Position end = caret();
        const std::vector<model::Paragraph>* paras = storyParas(end.story);
        if (paras && end.paragraph >= 0 && end.paragraph < static_cast<int>(paras->size()) && end.index > 0) {
            int at = 0;
            int ordinal = 0;
            for (const model::Run& run : (*paras)[static_cast<std::size_t>(end.paragraph)].runs) {
                for (const model::InlineItem& entry : run.items) {
                    if (entry.type == model::InlineItem::Type::Text) {
                        at += codePoints(entry.text);
                    } else if (render::paint_detail::isCaretCharControl(entry.control)) {
                        if (at == end.index - 1 && entry.control.kind == ControlKind::Picture) {
                            report.object = objectRefOf(end.story, end.paragraph, ordinal);
                        }
                        ++at;
                    }
                    ++ordinal;
                }
            }
        }
        return {};
    }

    // story 칸이 든 표의 칸 그림.
    Status tableOf(int story, TableInfo& out) const {
        const model::StoryMap& map = render::ensureStoryMap(doc);
        const model::StorySlot* slot = map.valid(story) ? map.slot(story) : nullptr;
        if (!slot || !slot->paragraphs) return fail(Code::PositionOutOfRange, "story " + std::to_string(story) + " 은 없습니다.");
        if (!slot->tableCell || !slot->control || !slot->control->table) {
            return fail(Code::BadArgument, "story " + std::to_string(story) + " 은 표 칸이 아닙니다.");
        }
        const model::Table& table = *slot->control->table;
        out = TableInfo{};
        out.parentStory = canonicalStory(slot->parentStory);
        out.parentParagraph = slot->parentParagraph;
        const model::TableGrid grid = model::tableGrid(table);
        out.rows = grid.rows;
        out.cols = grid.cols;
        out.columnWidths = model::tableColumnWidths(table);
        out.rowHeights = model::tableRowHeights(table);
        out.format = render::tableFields(doc, table);
        if (slot->control->placement) out.placement = render::placementFields(*slot->control->placement);
        out.cells.resize(table.cells.size());
        for (std::size_t i = 0; i < table.cells.size(); ++i) {
            const model::TableCell& cell = table.cells[i];
            TableCellInfo& info = out.cells[i];
            info.row = static_cast<int>(cell.row);
            info.col = static_cast<int>(cell.col);
            info.rowSpan = std::max(1, static_cast<int>(cell.rowSpan));
            info.colSpan = std::max(1, static_cast<int>(cell.colSpan));
            info.width = static_cast<std::int32_t>(cell.width);
            info.height = static_cast<std::int32_t>(cell.height);
            info.format = render::tableCellFields(doc, table, cell);
            for (std::size_t p = 0; p < cell.content.paragraphs.size(); ++p) {
                if (p > 0) info.text += '\n';
                info.text += cell.content.paragraphs[p].text();
            }
        }
        for (int s = 1; s <= map.count(); ++s) {
            const model::StorySlot* other = map.slot(s);
            if (!other || other->control != slot->control || !other->tableCell || other->subIndex < 0 ||
                map.storyOf(other->paragraphs) != s) {
                continue;
            }
            if (other->subIndex < static_cast<int>(out.cells.size())) out.cells[static_cast<std::size_t>(other->subIndex)].story = s;
        }
        return {};
    }

    // 표 구조 op. 규칙과 거절은 core 표 편집(model/table_edit)이 정한다.
    Status tableOp(const EditOp& op, OpReport& report) {
        render::TableEditCommand cmd;
        const char* name = "table";
        switch (op.kind) {
            case OpKind::InsertRows: cmd.kind = render::TableEditKind::InsertRows; break;
            case OpKind::InsertColumns: cmd.kind = render::TableEditKind::InsertColumns; break;
            case OpKind::DeleteRows: cmd.kind = render::TableEditKind::DeleteRows; break;
            case OpKind::DeleteColumns: cmd.kind = render::TableEditKind::DeleteColumns; break;
            case OpKind::MergeCells: cmd.kind = render::TableEditKind::Merge; break;
            case OpKind::SplitCell: cmd.kind = render::TableEditKind::Split; break;
            case OpKind::CellBorder: cmd.kind = render::TableEditKind::CellBorder; break;
            case OpKind::CellFill: cmd.kind = render::TableEditKind::CellFill; break;
            case OpKind::EqualWidth: cmd.kind = render::TableEditKind::EqualWidth; break;
            case OpKind::EqualHeight: cmd.kind = render::TableEditKind::EqualHeight; break;
            default: return fail(Code::Internal, "표 op 가 아닙니다.");
        }
        cmd.border = op.border;
        cmd.lineType = op.lineType;
        cmd.lineWidth = op.lineWidth;
        cmd.lineColor = op.lineColor;
        cmd.color = op.color;
        name = render::tableEditName(cmd.kind);
        TableInfo before;
        if (Status s = tableOf(op.story, before); !s.ok()) return s;
        if (op.toStory >= 0) {
            TableInfo other;
            if (Status s = tableOf(op.toStory, other); !s.ok()) return s;
            if (other.parentStory != before.parentStory || other.parentParagraph != before.parentParagraph) {
                return fail(Code::BadArgument, std::string(name) + ": story " + std::to_string(op.story) + " 와 " +
                                                   std::to_string(op.toStory) + " 은 다른 표의 칸입니다.");
            }
        }
        cmd.story = op.story;
        cmd.toStory = op.toStory;
        cmd.after = op.after;
        cmd.count = op.count;
        cmd.rows = op.rows;
        cmd.cols = op.cols;
        if ((op.kind == OpKind::InsertRows && (op.count < 1 || before.rows + op.count > render::kTableMaxRows)) ||
            (op.kind == OpKind::InsertColumns && (op.count < 1 || before.cols + op.count > render::kTableMaxCols))) {
            return fail(Code::BadArgument, std::string(name) + ": 표는 1~" + std::to_string(render::kTableMaxRows) +
                                               " 줄, 1~" + std::to_string(render::kTableMaxCols) + " 칸입니다.");
        }
        if (op.kind == OpKind::SplitCell &&
            (op.rows < 1 || op.cols < 1 || before.rows + op.rows - 1 > render::kTableMaxRows ||
             before.cols + op.cols - 1 > render::kTableMaxCols)) {
            return fail(Code::BadArgument, std::string(name) + ": 나눌 줄·칸 수가 표 한도를 넘거나 1 보다 작습니다.");
        }
        std::string why;
        const render::EditResult r = session.editTable(cmd, &why);
        if (!r.documentChanged) {
            return fail(Code::BadArgument, std::string(name) + ": " + (why.empty() ? "표를 고치지 못했습니다." : why));
        }
        render::rebuildStoryMap(doc);
        const render::PointerCursor cur = render::pointerCursor();
        if (cur.active && cur.story > 0) {
            if (Status s = tableOf(cur.story, report.table); !s.ok()) report.table = TableInfo{};
        }
        if (report.table.rows == 0) {
            report.table.parentStory = before.parentStory;
            report.table.parentParagraph = before.parentParagraph;
        }
        return {};
    }

    Status insertBreak(const EditOp& op) {
        render::BreakKind kind = render::BreakKind::Page;
        if (!render::breakKindOf(op.breakKind, kind)) {
            return fail(Code::BadArgument, "insertBreak: kind 는 page, column, columnDef, section 중 하나입니다.");
        }
        if (Status s = check(op.at); !s.ok()) return s;
        if (Status s = place(op.at, op.at); !s.ok()) return s;
        const render::EditResult r = session.insertBreak(kind);
        if (!r.documentChanged) return fail(Code::NotEditable, "insertBreak: 나누기는 구역 본문에서만 할 수 있습니다.");
        render::rebuildStoryMap(doc);
        return {};
    }

    Status replaceAll(const EditOp& op, OpReport& report) {
        if (op.find.empty()) return fail(Code::BadArgument, "replaceAll: find 가 비어 있습니다.");
        if (op.text.find_first_of("\r\n") != std::string::npos) {
            return fail(Code::BadArgument, "replaceAll: text 에 줄 끝을 넣을 수 없습니다. 문단은 나누지 않습니다.");
        }
        int count = 0;
        const render::EditResult r = session.replaceAll(op.find, op.text, op.matchCase, &count);
        if (r.needsFontSwap) return refused(r, "replaceAll");
        report.replaced = count;
        return {};
    }

    Status pageSetup(const EditOp& op) {
        const int count = static_cast<int>(doc.sections.size());
        if (op.section < 1 || op.section > count) {
            return fail(Code::BadArgument, "pageSetup: 구역 " + std::to_string(op.section) + " 은 없습니다. 구역은 1~" +
                                               std::to_string(count) + " 입니다.");
        }
        render::FormatFields fields;
        for (const auto& [key, value] : op.format) fields.emplace_back(key, value);
        if (fields.empty()) return fail(Code::BadArgument, "pageSetup: set 이 비어 있습니다.");
        // 그 구역 본문 첫 자리에 캐럿을 두면 편집기와 같은 명령이 그 구역을 고친다.
        const int story = render::ensureStoryMap(doc).storyOf(&doc.sections[static_cast<std::size_t>(op.section - 1)].paragraphs);
        if (Status s = place({story, 0, 0}, {story, 0, 0}); !s.ok()) return s;
        const render::EditResult r = session.applyPageSetup(fields, op.whole);
        if (r.needsFontSwap) return refused(r, "pageSetup");
        if (!r.documentChanged) return fail(Code::BadArgument, "pageSetup: 바꿀 값이 없거나 이름을 모릅니다.");
        return {};
    }

    Status setText(int story, const std::string& text) {
        const std::vector<model::Paragraph>* paras = storyParas(story);
        if (!paras || paras->empty()) return fail(Code::PositionOutOfRange, "story " + std::to_string(story) + " 은 없습니다.");
        const int last = static_cast<int>(paras->size()) - 1;
        const int length = render::paint_detail::paragraphCharCount(paras->back());
        if (Status s = erase({{story, 0, 0}, {story, last, length}}); !s.ok()) return s;
        return insertText({story, 0, 0}, text);
    }

    // ---- 편집기 명령(Command). 자리를 커서·고른 개체로 바꾸고 DocSession 명령을 부른다.

    int shapePage = -1;  // insertShape: apply 가 미리 고른 쪽 순번

    static const FieldValue* argOf(const EditOp& op, const char* name) {
        const FieldValue* found = nullptr;
        for (const auto& [key, value] : op.args) {
            if (key == name) found = &value;
        }
        return found;
    }
    static bool argFlag(const EditOp& op, const char* name, bool fallback) {
        const FieldValue* v = argOf(op, name);
        const bool* b = v ? std::get_if<bool>(v) : nullptr;
        return b ? *b : fallback;
    }
    static double argNumber(const EditOp& op, const char* name, double fallback) {
        const FieldValue* v = argOf(op, name);
        const double* d = v ? std::get_if<double>(v) : nullptr;
        return d ? *d : fallback;
    }
    static std::string argText(const EditOp& op, const char* name, const std::string& fallback = {}) {
        const FieldValue* v = argOf(op, name);
        const std::string* s = v ? std::get_if<std::string>(v) : nullptr;
        return s ? *s : fallback;
    }
    static render::FormatFields setOf(const EditOp& op) {
        render::FormatFields fields;
        for (const auto& [key, value] : op.format) {
            render::FormatValue v;
            std::visit([&](const auto& x) { v = x; }, value);
            fields.emplace_back(key, std::move(v));
        }
        return fields;
    }

    // 개체 이름 {story, paragraph, index} → 문단 항목 순번(글 조각·컨트롤을 차례로 센 번호).
    Status objectItem(const ObjectRef& ref, int& item) const {
        const std::vector<model::Paragraph>* paras = storyParas(ref.story);
        if (!paras) return fail(Code::PositionOutOfRange, "story " + std::to_string(ref.story) + " 은 없습니다.");
        if (ref.paragraph < 0 || ref.paragraph >= static_cast<int>(paras->size())) {
            return fail(Code::PositionOutOfRange, "object: 문단은 0~" + std::to_string(paras->size() - 1) + " 입니다.");
        }
        int ordinal = 0;
        int count = 0;
        for (const model::Run& run : (*paras)[static_cast<std::size_t>(ref.paragraph)].runs) {
            for (const model::InlineItem& entry : run.items) {
                if (entry.type == model::InlineItem::Type::Control && objectKind(entry.control)) {
                    if (count == ref.index) {
                        item = ordinal;
                        return {};
                    }
                    ++count;
                }
                ++ordinal;
            }
        }
        return fail(Code::PositionOutOfRange, "object: 그 문단의 개체는 " + std::to_string(count) + " 개입니다.");
    }

    // 문단 항목 순번 → 개체 이름.
    ObjectRef objectRefOf(int story, int paragraph, int item) const {
        ObjectRef ref;
        const std::vector<model::Paragraph>* paras = storyParas(story);
        if (!paras || paragraph < 0 || paragraph >= static_cast<int>(paras->size())) return ref;
        int ordinal = 0;
        int count = 0;
        for (const model::Run& run : (*paras)[static_cast<std::size_t>(paragraph)].runs) {
            for (const model::InlineItem& entry : run.items) {
                const bool object = entry.type == model::InlineItem::Type::Control && objectKind(entry.control);
                if (ordinal == item) {
                    if (object) ref = ObjectRef{canonicalStory(story), paragraph, count};
                    return ref;
                }
                if (object) ++count;
                ++ordinal;
            }
        }
        return ref;
    }

    // 명령 자리. range(블록) > at(캐럿) > story(그 글 묶음 첫 자리) > section(구역 본문 첫 자리) 가운데 처음 것에 커서를
    // 두고, object 가 있으면 그 개체를 고른다. 아무 자리도 없으면 placed 가 거짓이다.
    Status placeCommand(const EditOp& op, bool& placed) {
        render::clearObjectSelection();
        placed = true;
        if (op.hasRange) {
            if (Status s = checkRange(op.range); !s.ok()) return s;
            const Range r = ordered(op.range);
            if (Status s = place(r.from, r.to); !s.ok()) return s;
        } else if (op.hasAt) {
            if (Status s = check(op.at); !s.ok()) return s;
            if (Status s = place(op.at, op.at); !s.ok()) return s;
        } else if (op.story >= 0) {
            if (!storyParas(op.story)) return fail(Code::PositionOutOfRange, "story " + std::to_string(op.story) + " 은 없습니다.");
            if (Status s = place({op.story, 0, 0}, {op.story, 0, 0}); !s.ok()) return s;
        } else if (op.section > 0) {
            const int count = static_cast<int>(doc.sections.size());
            if (op.section > count) {
                return fail(Code::BadArgument, "구역 " + std::to_string(op.section) + " 은 없습니다. 구역은 1~" +
                                                   std::to_string(count) + " 입니다.");
            }
            const int story =
                render::ensureStoryMap(doc).storyOf(&doc.sections[static_cast<std::size_t>(op.section - 1)].paragraphs);
            if (Status s = place({story, 0, 0}, {story, 0, 0}); !s.ok()) return s;
        } else {
            placed = op.hasObject;
            // 자리 없이 문서에 거는 명령(도형 넣기 등)은 앞 op 의 캐럿을 이어 쓰지 않는다.
            if (!placed) {
                render::setPointerCursor(doc, pages, model::CursorPos{}, model::CursorPos{});
                render::syncEditBand(doc);
            }
        }
        if (op.hasObject) {
            int item = -1;
            if (Status s = objectItem(op.object, item); !s.ok()) return s;
            if (!session.selectObject(op.object.story, op.object.paragraph, item)) {
                return fail(Code::NotEditable, "object: 그 개체는 쪽에 그려지지 않아 고를 수 없습니다.");
            }
        }
        return {};
    }

    static Status commandFailed(const std::string& name, const render::EditResult& r, const std::string& why) {
        if (r.needsFontSwap) return refused(r, name.c_str());
        return fail(Code::BadArgument, name + ": " + (why.empty() ? std::string("하지 못했습니다.") : why));
    }

    Status command(const EditOp& op, OpReport& report) {
        const std::string& name = op.command;
        bool placed = false;
        if (Status s = placeCommand(op, placed); !s.ok()) return s;
        // 문서 전체에 거는 명령 말고는 자리가 있어야 한다(agent 는 커서를 들고 있지 않는다).
        const bool anywhere = name == "exchangeNotes" || name == "insertShape";
        if (!placed && !anywhere) {
            return fail(Code::BadArgument, name + ": 자리(at·range·object·story·section)가 없습니다.");
        }
        const render::FormatFields set = setOf(op);
        std::string why;
        render::EditResult r;
        bool typeAfter = false;  // 끝에 text 인자를 캐럿 자리에 친다(머리말·주·캡션·글상자 글)
        render::PageControlKind kind{};
        if (render::pageControlKindOf(name, kind)) {
            r = session.pageControl(kind, set, &why);
            typeAfter = kind == render::PageControlKind::Header || kind == render::PageControlKind::Footer ||
                        kind == render::PageControlKind::EditBand || kind == render::PageControlKind::MasterPage;
        } else if (name == "insertNote") {
            r = session.insertNote(argFlag(op, "endnote", false), &why);
            typeAfter = true;
        } else if (name == "exchangeNotes") {
            const int mode = static_cast<int>(argNumber(op, "mode", 2));
            if (mode < 0 || mode > 2) {
                return fail(Code::BadArgument, "exchangeNotes: mode 는 0(각주→미주), 1(미주→각주), 2(서로) 입니다.");
            }
            r = session.exchangeNotes(mode, &why);
        } else if (name == "noteShape") {
            r = session.noteShape(set, argFlag(op, "endnote", false), argFlag(op, "whole", false), &why);
        } else if (name == "pageBorder") {
            r = session.pageBorder(set, argFlag(op, "whole", false), &why);
        } else if (name == "sectionSettings") {
            r = session.sectionSettings(set, &why);
        } else if (name == "columns") {
            r = session.columns(set, &why);
        } else if (name == "caption") {
            r = session.caption(argText(op, "caption", "BOTTOM"), &why);
            typeAfter = true;
        } else if (name == "bookmark") {
            r = session.bookmark(argText(op, "action", "insert"), argText(op, "name"), &why, argText(op, "newName"));
        } else if (name == "tableToText" || name == "textToTable") {
            render::FormatFields fields = set;
            fields.emplace_back("action", name);
            r = session.tableText(fields, &why);
        } else if (name == "deleteMarks") {
            r = session.deleteMarks(set, &why);
        } else if (name == "tableProperties") {
            r = session.tableProperties(set, &why);
        } else if (name == "glyphGroup") {
            r = session.glyphGroup(set, &why);
        } else if (name == "convertText") {
            r = session.convertText(set, &why);
        } else if (name == "sortParagraphs") {
            r = session.sortParagraphs(set, &why);
        } else if (name == "insertHyperlink") {
            const std::string url = argText(op, "url");
            if (url.empty()) return fail(Code::BadArgument, "insertHyperlink: url 이 없습니다.");
            r = session.insertHyperlink(argText(op, "text"), url, &why);
        } else if (name == "copyShape") {
            // at 자리의 모양을 기억해 range 블록에 입힌다.
            if (!op.hasAt || !op.hasRange) {
                return fail(Code::BadArgument, "copyShape: at(모양을 가져올 자리)과 range(입힐 블록)가 있어야 합니다.");
            }
            const int type = static_cast<int>(argNumber(op, "type", 2));
            if (type != 0 && type != 1 && type != 2 && type != 4) {
                return fail(Code::BadArgument, "copyShape: type 은 0(글자), 1(문단), 2(둘 다), 4(스타일) 입니다.");
            }
            if (Status s = check(op.at); !s.ok()) return s;
            if (Status s = place(op.at, op.at); !s.ok()) return s;
            r = session.copyShape("copy", type, &why);
            if (!r.handled) return commandFailed(name, r, why);
            const Range target = ordered(op.range);
            if (Status s = place(target.from, target.to); !s.ok()) return s;
            r = session.copyShape("paste", type, &why);
        } else if (name == "insertShape") {
            static const std::pair<const char*, int> kTools[] = {
                {"rect", 1}, {"ellipse", 2}, {"line", 3}, {"textBox", 4}, {"verticalTextBox", 5}};
            const std::string toolName = argText(op, "tool", "rect");
            int tool = 0;
            for (const auto& [n, t] : kTools) {
                if (toolName == n) tool = t;
            }
            if (tool == 0) {
                return fail(Code::BadArgument, "insertShape: tool 은 rect, ellipse, line, textBox, verticalTextBox 입니다.");
            }
            if (shapePage < 0) return fail(Code::BadArgument, "insertShape: page 가 없습니다.");
            const auto x = static_cast<std::int32_t>(argNumber(op, "x", 0));
            const auto y = static_cast<std::int32_t>(argNumber(op, "y", 0));
            const auto w = static_cast<std::int32_t>(argNumber(op, "width", 0));
            const auto h = static_cast<std::int32_t>(argNumber(op, "height", 0));
            // 편집기는 2mm 보다 작게 끈 도형을 그리지 않는다.
            if (tool != 3 && (w < 567 || h < 567)) {
                return fail(Code::BadArgument, "insertShape: width·height 는 2mm(567) 이상입니다.");
            }
            if (tool == 3 && std::max(std::abs(w), std::abs(h)) < 567) {
                return fail(Code::BadArgument, "insertShape: 선 길이는 2mm(567) 이상입니다.");
            }
            r = session.insertShape(tool, shapePage, x, y, x + w, y + h);
            if (r.documentChanged) {
                // 새 도형은 그 글 묶음에서 가장 앞(zOrder 가 가장 큰) 개체다.
                const std::vector<model::Paragraph>* paras = storyParas(r.dirtyStory);
                if (paras && r.dirtyParagraph >= 0 && r.dirtyParagraph < static_cast<int>(paras->size())) {
                    int ordinal = 0;
                    int best = -1;
                    std::int32_t bestZ = std::numeric_limits<std::int32_t>::min();
                    for (const model::Run& run : (*paras)[static_cast<std::size_t>(r.dirtyParagraph)].runs) {
                        for (const model::InlineItem& entry : run.items) {
                            if (entry.type == model::InlineItem::Type::Control && entry.control.kind == ControlKind::Shape &&
                                entry.control.placement && entry.control.placement->zOrder >= bestZ) {
                                bestZ = entry.control.placement->zOrder;
                                best = ordinal;
                            }
                            ++ordinal;
                        }
                    }
                    if (best >= 0) report.object = objectRefOf(r.dirtyStory, r.dirtyParagraph, best);
                }
            }
            typeAfter = tool >= 4;
        } else if (name == "objectProperties" || name == "deleteObject") {
            if (!op.hasObject) return fail(Code::BadArgument, name + ": object 가 없습니다.");
            int item = -1;
            if (Status s = objectItem(op.object, item); !s.ok()) return s;
            r = name == "deleteObject" ? session.deleteObject(op.object.story, op.object.paragraph, item, &why)
                                       : session.objectProperties(op.object.story, op.object.paragraph, item, set, &why);
        } else {
            return fail(Code::BadArgument, "명령 " + name + " 은 모릅니다.");
        }
        if (!r.handled && !r.documentChanged) return commandFailed(name, r, why);
        // 편집 명령이 글 묶음 표를 다시 만들고 쪽을 다시 짰다. 여기서 다시 만들면 쪽이 붙인 별칭 번호(머리말 등)가 빠진다.
        const std::string text = argText(op, "text");
        if (typeAfter && !text.empty()) {
            // 명령이 캐럿을 새 글 묶음(머리말·주·캡션·글상자)에 넣었을 때만 친다. 이미 있던 캡션처럼 들어가지 않았으면 거절한다.
            const render::PointerCursor cur = render::pointerCursor();
            const model::StorySlot* slot = cur.active ? render::resolveStory(doc, cur.story) : nullptr;
            if (!slot || slot->kind == model::StoryKind::Section || slot->kind == model::StoryKind::TableCell) {
                return fail(Code::BadArgument, name + ": text 를 칠 새 글 묶음이 없습니다(이미 있는 캡션 등). insertText 로 넣으세요.");
            }
            if (Status s = typeText(text); !s.ok()) return s;
        }
        return {};
    }

    // Command op 의 대화 상자 값. 자리를 잡고 읽는다.
    Status view(const EditOp& op, Fields& out) {
        const std::string& name = op.command;
        render::PageControlKind kind{};
        const bool known = render::pageControlKindOf(name, kind) || name == "noteShape" || name == "pageBorder" ||
                           name == "sectionSettings" || name == "columns" || name == "tableProperties" ||
                           name == "glyphGroup" || name == "sortParagraphs" || name == "bookmark" ||
                           name == "objectProperties";
        if (!known) return fail(Code::BadArgument, "view: " + name + " 은 읽을 값이 없습니다.");
        bool placed = false;
        if (Status s = placeCommand(op, placed); !s.ok()) return s;
        if (!placed) return fail(Code::BadArgument, name + ": 자리(at·range·object·story·section)가 없습니다.");
        render::FormatFields fields;
        if (render::pageControlKindOf(name, kind)) {
            fields = session.pageControlFields(kind);
        } else if (name == "noteShape") {
            fields = session.noteShapeView(argFlag(op, "endnote", false));
        } else if (name == "pageBorder") {
            fields = render::formatView(doc, pages, render::FormatViewKind::PageBorder, {}).values;
        } else if (name == "sectionSettings") {
            fields = session.sectionSettingsView();
        } else if (name == "columns") {
            fields = session.columnView(setOf(op));
        } else if (name == "tableProperties") {
            fields = session.tablePropertiesView();
        } else if (name == "glyphGroup") {
            fields = session.glyphGroupView();
        } else if (name == "sortParagraphs") {
            fields = session.sortView(setOf(op));
        } else if (name == "bookmark") {
            fields = session.bookmarkView();
        } else {
            if (!op.hasObject) return fail(Code::BadArgument, name + ": object 가 없습니다.");
            int item = -1;
            if (Status s = objectItem(op.object, item); !s.ok()) return s;
            fields = session.objectPropertiesView(op.object.story, op.object.paragraph, item);
            if (fields.empty()) return fail(Code::BadArgument, name + ": 크기·위치가 없는 개체입니다.");
        }
        out.clear();
        for (const auto& [key, value] : fields) {
            FieldValue v;
            std::visit([&](const auto& x) { v = x; }, value);
            out.emplace_back(key, std::move(v));
        }
        return {};
    }

    Status run(const EditOp& op, OpReport& report) {
        Status status;
        switch (op.kind) {
            case OpKind::InsertText: status = insertText(op.at, op.text); break;
            case OpKind::Delete: status = erase(op.range); break;
            case OpKind::Replace:
                status = erase(op.range);
                if (status.ok()) status = insertText(ordered(op.range).from, op.text);
                break;
            case OpKind::Format: status = restyle(op.range, op.format, op.style); break;
            case OpKind::InsertTable: status = insertTable(op, report); break;
            case OpKind::SetText: status = setText(op.story, op.text); break;
            case OpKind::InsertRows:
            case OpKind::InsertColumns:
            case OpKind::DeleteRows:
            case OpKind::DeleteColumns:
            case OpKind::MergeCells:
            case OpKind::SplitCell:
            case OpKind::CellBorder:
            case OpKind::CellFill:
            case OpKind::EqualWidth:
            case OpKind::EqualHeight: status = tableOp(op, report); break;
            case OpKind::InsertBreak: status = insertBreak(op); break;
            case OpKind::ReplaceAll: status = replaceAll(op, report); break;
            case OpKind::PageSetup: status = pageSetup(op); break;
            case OpKind::InsertPicture: status = insertPicture(op, report); break;
            case OpKind::Command: status = command(op, report); break;
        }
        if (status.ok()) report.end = caret();
        return status;
    }
};

AgentDocument::AgentDocument(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AgentDocument::~AgentDocument() {
    // 그림을 그릴 때 읽던 원본 바이트가 사라지므로 연결을 끊는다.
    render::setPictureFile({});
}

Result<std::unique_ptr<AgentDocument>> AgentDocument::open(ByteView bytes) {
    Result<std::unique_ptr<AgentDocument>> out;
    auto impl = std::make_unique<Impl>();
    std::string error;
    if (!impl->session.open(Bytes(bytes.begin(), bytes.end()), &error, &impl->warnings)) {
        out.status = fail(Code::ParseFailed, error.empty() ? "문서를 읽지 못했습니다." : error);
        return out;
    }
    impl->format = detectFormat(impl->source);
    // 이전 문서의 글자 윤곽·그림 캐시를 비우고, 편집기와 같은 조판(저장된 줄 높이)을 한다.
    render::clearGlyphWarm();
    render::clearPageRenderCache();
    impl->session.layout();
    impl->indexPages();
    out.value = std::unique_ptr<AgentDocument>(new AgentDocument(std::move(impl)));
    return out;
}

Result<Bytes> AgentDocument::save(model::SourceFormat format) const {
    Result<Bytes> out;
    if (format == model::SourceFormat::Unknown) {
        out.status = fail(Code::BadArgument, "저장 형식을 정하지 않았습니다. HWP 또는 HWPX 를 고르세요.");
        return out;
    }
    // 편집기와 같이 고친 문단의 줄 나눔(lineSeg)을 조판 결과로 적어 저장한다.
    std::string error;
    std::optional<Bytes> saved = impl_->session.save(format, &error);
    if (!saved) {
        out.status = fail(Code::SaveFailed, error.empty() ? "문서를 쓰지 못했습니다." : error);
        return out;
    }
    out.value = std::move(*saved);
    return out;
}

model::SourceFormat AgentDocument::sourceFormat() const { return impl_->format; }

std::optional<model::SourceFormat> AgentDocument::saveFormatFor(const std::string& path, const std::string& format) const {
    return impl_->session.saveFormatFor(path, format);
}

std::vector<std::string> AgentDocument::warnings() const { return impl_->warnings; }

std::vector<PageInfo> AgentDocument::pages() const { return impl_->pageInfos; }

Result<int> AgentDocument::resolvePage(const PageRef& ref) const {
    Result<int> out;
    const int sectionCount = static_cast<int>(impl_->doc.sections.size());
    if (ref.section < 0 || ref.section > sectionCount) {
        out.status = fail(Code::BadArgument, "구역 " + std::to_string(ref.section) + " 은 없습니다. 구역은 1~" +
                                                 std::to_string(sectionCount) + " 입니다.");
        return out;
    }
    if (ref.nth < 0) {
        out.status = fail(Code::BadArgument, "nth 는 1 이상이어야 합니다.");
        return out;
    }
    // 물리 순서로 고르면 번호가 겹쳐도 하나로 정해진다.
    if (ref.order != 0) {
        const int count = static_cast<int>(impl_->pageInfos.size());
        if (ref.order < 1 || ref.order > count) {
            out.status = fail(Code::PageNotFound, std::to_string(ref.order) + "번째 쪽은 없습니다. 쪽은 1~" +
                                                      std::to_string(count) + "번째까지 있습니다.");
            return out;
        }
        out.value = ref.order - 1;
        return out;
    }
    std::vector<int> hits;
    for (std::size_t i = 0; i < impl_->pageInfos.size(); ++i) {
        const PageRef& have = impl_->pageInfos[i].ref;
        if (have.page != ref.page) continue;
        if (ref.section > 0 && have.section != ref.section) continue;
        if (ref.nth > 0 && have.nth != ref.nth && !(have.nth == 0 && ref.nth == 1)) continue;
        hits.push_back(static_cast<int>(i));
    }
    if (hits.size() == 1) {
        out.value = hits.front();
        return out;
    }
    if (hits.empty() && ref.nth > 0) {
        // 번호는 있는데 그만큼 겹치지 않는다. 몇 개인지 알려 준다.
        int have = 0;
        for (const PageInfo& info : impl_->pageInfos) {
            if (info.ref.page == ref.page && (ref.section == 0 || info.ref.section == ref.section)) ++have;
        }
        if (have > 0) {
            out.status = fail(Code::PageNotFound, std::to_string(ref.page) + "쪽은 " + std::to_string(have) +
                                                      "개뿐입니다. nth " + std::to_string(ref.nth) + " 은 없습니다.");
            return out;
        }
    }
    if (hits.empty()) {
        // 구역마다 쪽 번호 범위를 적어 준다.
        std::string ranges;
        for (int s = 1; s <= sectionCount; ++s) {
            if (ref.section > 0 && s != ref.section) continue;
            int lo = std::numeric_limits<int>::max();
            int hi = std::numeric_limits<int>::min();
            for (const PageInfo& info : impl_->pageInfos) {
                if (info.ref.section != s) continue;
                lo = std::min(lo, info.ref.page);
                hi = std::max(hi, info.ref.page);
            }
            if (lo > hi) continue;
            if (!ranges.empty()) ranges += ", ";
            ranges += "구역 " + std::to_string(s) + " 은 " + std::to_string(lo) + "~" + std::to_string(hi) + "쪽";
        }
        std::string where = ref.section > 0 ? "구역 " + std::to_string(ref.section) + "에 " : std::string();
        out.status = fail(Code::PageNotFound, where + std::to_string(ref.page) + "쪽이 없습니다." +
                                                  (ranges.empty() ? std::string() : " " + ranges + "입니다."));
        return out;
    }
    Status status = fail(Code::AmbiguousPage, std::string());
    std::string sections;
    for (int index : hits) {
        const PageRef& have = impl_->pageInfos[static_cast<std::size_t>(index)].ref;
        if (std::none_of(status.candidates.begin(), status.candidates.end(),
                         [&](const PageRef& c) { return sameRef(c, have); })) {
            status.candidates.push_back(have);
        }
        const std::string name = "구역 " + std::to_string(have.section);
        if (sections.find(name) == std::string::npos) sections += (sections.empty() ? "" : ", ") + name;
    }
    const bool needNth = ref.section > 0 || std::all_of(status.candidates.begin(), status.candidates.end(),
                                                         [&](const PageRef& c) {
                                                             return c.section == status.candidates.front().section;
                                                         });
    status.message = std::to_string(ref.page) + "쪽이 " + std::to_string(hits.size()) + "개 있습니다(" + sections +
                     "). " + (needNth ? "nth 로 몇 번째인지 주세요." : "section 을 주세요.");
    out.status = std::move(status);
    return out;
}

std::vector<StoryInfo> AgentDocument::stories() const {
    std::vector<StoryInfo> out;
    const model::StoryMap& map = render::ensureStoryMap(impl_->doc);
    for (int story = 1; story <= map.count(); ++story) {
        const model::StorySlot* slot = map.slot(story);
        if (!slot || !slot->paragraphs) continue;
        // 머리말·바탕쪽처럼 여러 쪽에 찍히는 글은 화면용 별칭 번호가 더 있다. 원래 번호 하나만 낸다.
        if (map.storyOf(slot->paragraphs) != story) continue;
        StoryInfo info;
        info.story = story;
        info.kind = storyKindText(slot->kind);
        info.owner = ownerOf(*slot);
        info.section = slot->sectionIndex + 1;
        info.paragraphs = static_cast<int>(slot->paragraphs->size());
        info.parentStory = slot->parentStory;
        info.parentParagraph = slot->parentParagraph;
        if (slot->tableCell) {
            info.row = static_cast<int>(slot->tableCell->row);
            info.col = static_cast<int>(slot->tableCell->col);
            info.rowSpan = std::max(1, static_cast<int>(slot->tableCell->rowSpan));
            info.colSpan = std::max(1, static_cast<int>(slot->tableCell->colSpan));
        }
        out.push_back(std::move(info));
    }
    return out;
}

Result<TableInfo> AgentDocument::table(int story) const {
    Result<TableInfo> out;
    out.status = impl_->tableOf(story, out.value);
    return out;
}

Result<ParagraphInfo> AgentDocument::paragraph(int story, int paragraph, bool detail) const {
    Result<ParagraphInfo> out;
    const model::StoryMap& map = render::ensureStoryMap(impl_->doc);
    const model::StorySlot* slot = map.valid(story) ? map.slot(story) : nullptr;
    if (!slot || !slot->paragraphs) {
        out.status = fail(Code::PositionOutOfRange, "story " + std::to_string(story) + " 은 없습니다. stories() 로 번호를 확인하세요.");
        return out;
    }
    const int count = static_cast<int>(slot->paragraphs->size());
    if (paragraph < 0 || paragraph >= count) {
        out.status = fail(Code::PositionOutOfRange, "story " + std::to_string(story) + " 의 문단은 0~" +
                                                        std::to_string(count - 1) + " 입니다. " +
                                                        std::to_string(paragraph) + " 은 없습니다.");
        return out;
    }
    const model::Document& doc = impl_->doc;
    const model::Paragraph& para = (*slot->paragraphs)[static_cast<std::size_t>(paragraph)];
    ParagraphInfo& info = out.value;
    info.story = story;
    info.paragraph = paragraph;
    if (const model::Style* style = styleById(doc, para.styleId)) info.style = style->name;
    if (para.paraShapeId < doc.header.paraShapes.size()) {
        const model::ParaShape& shape = doc.header.paraShapes[para.paraShapeId];
        info.align = shape.align;
        info.lineSpacingType = shape.lineSpacingType;
        info.lineSpacing = shape.lineSpacing;
        info.indent = shape.indent;
        info.spacingPrev = shape.spacingPrev;
        info.spacingNext = shape.spacingNext;
    }

    // 개체가 담은 글 묶음. 칸·글상자·머리말 글의 story 를 개체에 붙인다.
    std::map<const model::Control*, std::vector<int>> inner;
    for (int s = 1; s <= map.count(); ++s) {
        const model::StorySlot* other = map.slot(s);
        if (!other || !other->paragraphs || !other->control) continue;
        if (map.storyOf(other->paragraphs) != s) continue;
        inner[other->control].push_back(s);
    }

    int at = 0;
    auto addRun = [&](int from, int to, std::uint32_t charShapeId) {
        if (to <= from) return;
        if (!info.runs.empty() && info.runs.back().to == from && info.runs.back().charShapeId == charShapeId) {
            info.runs.back().to = to;
            return;
        }
        FormatRun run;
        run.from = from;
        run.to = to;
        run.charShapeId = charShapeId;
        run.format = formatOf(doc, charShapeId);
        info.runs.push_back(std::move(run));
    };
    for (const model::Run& run : para.runs) {
        const int runFrom = at;
        for (const model::InlineItem& item : run.items) {
            if (item.type == model::InlineItem::Type::Text) {
                info.text += item.text;
                at += codePoints(item.text);
                continue;
            }
            const model::Control& ctrl = item.control;
            const bool takesChar = render::paint_detail::isCaretCharControl(ctrl);
            if (const char* kind = objectKind(ctrl)) {
                ObjectInfo object;
                object.index = static_cast<int>(info.objects.size());
                object.at = at;
                object.kind = kind;
                object.inlineChar = takesChar;
                if (ctrl.placement) {
                    object.width = static_cast<std::int32_t>(ctrl.placement->width);
                    object.height = static_cast<std::int32_t>(ctrl.placement->height);
                }
                if (ctrl.kind == ControlKind::Equation) object.script = ctrl.script;
                if (ctrl.placement) object.placement = render::placementFields(*ctrl.placement);
                if (auto found = inner.find(&ctrl); found != inner.end()) object.stories = found->second;
                info.objects.push_back(std::move(object));
            }
            if (takesChar) {
                info.text += "\xEF\xBF\xBC";  // U+FFFC
                ++at;
            }
        }
        addRun(runFrom, at, run.charShapeId);
    }
    // 글자 수는 편집기가 캐럿 번호에 쓰는 셈(paragraphCharCount) 하나로 낸다.
    info.length = render::paint_detail::paragraphCharCount(para);
    // 첫 글자가 놓인 쪽. find 와 같이 캐럿 자리로 고른다.
    const int offset = render::paint_detail::charIndexToOffset(doc, impl_->pages, story, paragraph, 0);
    const render::CaretPlace place =
        render::caretForCursor(doc, impl_->pages, model::CursorPos{story, paragraph, offset});
    if (place.hit && place.page >= 0 && place.page < static_cast<int>(impl_->pageInfos.size())) {
        info.pageOrder = impl_->pageInfos[static_cast<std::size_t>(place.page)].order;
    }
    if (detail) {
        info.format = render::paraShapeFields(doc, para.paraShapeId);
        for (const FormatRun& run : info.runs) {
            const bool seen = std::any_of(info.charShapes.begin(), info.charShapes.end(),
                                          [&](const auto& entry) { return entry.first == run.charShapeId; });
            if (!seen) info.charShapes.emplace_back(run.charShapeId, render::charShapeFields(doc, run.charShapeId));
        }
    }
    return out;
}

Result<Fields> AgentDocument::section(int section) const {
    Result<Fields> out;
    const int count = static_cast<int>(impl_->doc.sections.size());
    if (section < 1 || section > count) {
        out.status = fail(Code::BadArgument, "구역 " + std::to_string(section) + " 은 없습니다. 구역은 1~" +
                                                 std::to_string(count) + " 입니다.");
        return out;
    }
    out.value = render::sectionFields(impl_->doc, section - 1);
    return out;
}

Result<std::vector<Match>> AgentDocument::find(const FindQuery& query) const {
    Result<std::vector<Match>> out;
    if (query.text.empty()) {
        out.status = fail(Code::BadArgument, "찾을 글이 비어 있습니다.");
        return out;
    }
    if (query.story != 0 && !impl_->storyParas(query.story)) {
        out.status = fail(Code::PositionOutOfRange, "story " + std::to_string(query.story) + " 은 없습니다.");
        return out;
    }
    const int limit = query.limit > 0 ? query.limit : 100;
    const model::Document& doc = impl_->doc;
    for (const render::TextHit& hit : render::findText(doc, query.text, query.matchCase, query.story, limit)) {
        Match match;
        match.range = {{hit.story, hit.paragraph, hit.from}, {hit.story, hit.paragraph, hit.to}};
        const int offset = render::paint_detail::charIndexToOffset(doc, impl_->pages, hit.story, hit.paragraph, hit.from);
        const render::CaretPlace place =
            render::caretForCursor(doc, impl_->pages, model::CursorPos{hit.story, hit.paragraph, offset});
        if (place.hit && place.page >= 0 && place.page < static_cast<int>(impl_->pageInfos.size())) {
            match.page = impl_->pageInfos[static_cast<std::size_t>(place.page)].ref;
        }
        const std::string text = render::paragraphSearchText((*impl_->storyParas(hit.story))[static_cast<std::size_t>(hit.paragraph)]);
        const std::vector<std::size_t> starts = codePointStarts(text);
        constexpr int kAround = 20;
        const int lo = std::max(0, hit.from - kAround);
        const int hi = std::min(static_cast<int>(starts.size()) - 1, hit.to + kAround);
        match.context = text.substr(starts[static_cast<std::size_t>(lo)],
                                    starts[static_cast<std::size_t>(hi)] - starts[static_cast<std::size_t>(lo)]);
        out.value.push_back(std::move(match));
    }
    return out;
}

Result<PageLayoutInfo> AgentDocument::layout(const PageRef& page, bool withText) const {
    Result<PageLayoutInfo> out;
    const Result<int> index = resolvePage(page);
    if (!index.ok()) {
        out.status = index.status;
        return out;
    }
    const render::PageLayout drawn = render::describePage(impl_->doc, impl_->pages, index.value);
    PageLayoutInfo& info = out.value;
    info.page = impl_->pageInfos[static_cast<std::size_t>(index.value)].ref;
    info.width = drawn.width;
    info.height = drawn.height;
    info.bodyX = drawn.bodyX;
    info.bodyY = drawn.bodyY;
    info.bodyWidth = drawn.bodyWidth;
    info.bodyHeight = drawn.bodyHeight;
    const std::int32_t bodyBottom = drawn.bodyY + drawn.bodyHeight;
    const model::StoryMap& map = render::ensureStoryMap(impl_->doc);
    const auto canonical = [&](int story) {
        const model::StorySlot* slot = story > 0 && map.valid(story) ? map.slot(story) : nullptr;
        return slot && slot->paragraphs ? map.storyOf(slot->paragraphs) : story;
    };
    for (const render::LayoutLine& l : drawn.lines) {
        LayoutLineInfo line;
        line.from = {canonical(l.story), l.paragraph, l.from};
        line.to = l.to;
        line.x = l.x;
        line.y = l.y;
        line.width = l.width;
        line.height = l.height;
        line.inkRight = l.inkRight;
        line.heading = l.heading;
        line.band = l.band;
        line.master = l.master;
        line.turned = l.turned;
        const model::StorySlot* slot = l.story > 0 && map.valid(l.story) ? map.slot(l.story) : nullptr;
        const bool body = slot && !slot->control && !l.band && !l.master && !l.turned;
        line.overflow = body && drawn.bodyHeight > 0 && l.y + l.height > bodyBottom;
        if (withText && slot && slot->paragraphs && l.paragraph >= 0 &&
            l.paragraph < static_cast<int>(slot->paragraphs->size())) {
            const std::string text = render::paragraphSearchText((*slot->paragraphs)[static_cast<std::size_t>(l.paragraph)]);
            const std::vector<std::size_t> starts = codePointStarts(text);
            const int last = static_cast<int>(starts.size()) - 1;
            const int a = std::clamp(l.from, 0, last);
            const int b = std::clamp(l.to, a, last);
            line.text = text.substr(starts[static_cast<std::size_t>(a)], starts[static_cast<std::size_t>(b)] - starts[static_cast<std::size_t>(a)]);
        }
        info.lines.push_back(std::move(line));
    }
    for (const render::LayoutBox& b : drawn.boxes) {
        LayoutBoxInfo box;
        box.kind = b.kind;
        box.story = canonical(b.story);
        box.paragraph = b.paragraph;
        box.row = b.row;
        box.col = b.col;
        box.rowSpan = b.rowSpan;
        box.colSpan = b.colSpan;
        box.x = b.x;
        box.y = b.y;
        box.width = b.width;
        box.height = b.height;
        box.inlineChar = b.inlineChar;
        box.band = b.band;
        box.master = b.master;
        box.turned = b.turned;
        info.boxes.push_back(std::move(box));
    }
    return out;
}

Result<Image> AgentDocument::renderPage(const PageRef& page, const RenderOptions& options) const {
    Result<Image> out;
    const Result<int> index = resolvePage(page);
    if (!index.ok()) {
        out.status = index.status;
        return out;
    }
    const render::Node& paper = impl_->pages[static_cast<std::size_t>(index.value)];
    int pixelHeight = options.pixelHeight;
    if (pixelHeight <= 0) pixelHeight = static_cast<int>(std::lround(paper.height * options.dpi / 7200.0));
    // 쪽 그리기는 세로 32px 보다 작은 그림을 만들지 않는다. 그보다 작게 물으면 32px 로 그린다.
    pixelHeight = std::clamp(pixelHeight, 32, 8000);
    // 편집기와 같은 쪽 그림(GPU 먼저, 못 쓰면 CPU). 칸 캐시는 이 그림에만 쓴다.
    impl_->frames.clear();
    render::PageImage image = impl_->session.renderPage(index.value, pixelHeight, impl_->frames);
    impl_->frames.clear();
    if (image.png.empty()) {
        // 파일 쓰기와는 상관없다(쓰기 실패는 SaveFailed). 어느 쪽을 어떤 크기로 그리다 멈췄는지 적는다.
        out.status = fail(Code::RenderFailed, std::to_string(index.value + 1) + "번째 쪽을 세로 " +
                                                  std::to_string(pixelHeight) +
                                                  "px 로 그리지 못했습니다. 그리기가 빈 그림을 돌려줬습니다.");
        return out;
    }
    out.value.page = impl_->pageInfos[static_cast<std::size_t>(index.value)].ref;
    out.value.width = image.width;
    out.value.height = image.height;
    out.value.png = std::move(image.png);
    return out;
}

Result<EditReport> AgentDocument::apply(const std::vector<EditOp>& ops) {
    Result<EditReport> out;
    if (render::readOnly()) {
        out.status = fail(Code::ReadOnly, "읽기 전용 문서입니다.");
        return out;
    }
    if (ops.empty()) {
        out.value.pages = static_cast<int>(impl_->pages.size());
        return out;
    }
    // 편집기와 같은 편집 명령·되돌리기를 이 문서의 편집 상태로 탄다. op 들은 되돌리기 한 묶음이다.
    app::DocSession& session = impl_->session;
    const render::EditScope scope = session.bind();
    // 라이선스로 막힌 글꼴은 편집 앞에 바꾼다. 바꿀 글꼴과 되돌리기 한 칸인 것은 편집기와 같다(DocSession).
    // 편집기는 사용자에게 묻고, agent 는 묻지 않고 그 처음 값(전에 고른 글꼴, 없으면 기본 짝)으로 바꾼다.
    out.value.fontSwaps = session.licenseSwapTargets();
    const bool swapped = !out.value.fontSwaps.empty() && session.swapLicenseFonts().documentChanged;
    const int group = session.beginEditGroup();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        OpReport report;
        Status status;
        impl_->shapePage = -1;
        if (ops[i].kind == OpKind::Command && ops[i].command == "insertShape") {
            const Result<int> page = resolvePage(ops[i].page);
            if (page.ok()) impl_->shapePage = page.value;
            else status = page.status;
        }
        if (status.ok()) status = impl_->run(ops[i], report);
        if (!status.ok()) {
            // 앞 op 들까지 묶음째 되돌리고, 다시 하기로 살아나지 않게 버린다. apply 는 전부 아니면 없음이라
            // 이 apply 가 한 글꼴 바꾸기도 되돌린다.
            session.endEditGroup();
            if (session.topEditGroup() == group) session.undo();
            if (swapped) session.undo();
            session.dropRedo();
            session.edit.clearCursor();
            render::syncEditBand(impl_->doc);
            impl_->indexPages();
            status.message = "op " + std::to_string(i) + ": " + status.message;
            out.status = std::move(status);
            out.value = {};
            return out;
        }
        out.value.ops.push_back(std::move(report));
    }
    session.endEditGroup();
    // 위치는 요청마다 받으므로 커서·블록은 남기지 않는다. 머리말·바탕쪽 편집 상태도 나온다.
    session.edit.clearCursor();
    render::syncEditBand(impl_->doc);
    impl_->indexPages();
    out.value.pages = static_cast<int>(impl_->pages.size());
    return out;
}

Result<Fields> AgentDocument::commandView(const EditOp& op) {
    Result<Fields> out;
    if (op.kind != OpKind::Command) {
        out.status = fail(Code::BadArgument, "view 는 명령(Command) op 만 읽습니다.");
        return out;
    }
    const render::EditScope scope = impl_->session.bind();
    out.status = impl_->view(op, out.value);
    render::clearObjectSelection();
    impl_->session.edit.clearCursor();
    render::syncEditBand(impl_->doc);
    if (!out.ok()) out.value.clear();
    return out;
}

std::vector<std::string> commandNames() {
    std::vector<std::string> names = {"insertNote", "exchangeNotes", "noteShape", "pageBorder", "sectionSettings",
                                      "columns", "caption", "bookmark", "tableToText", "textToTable", "deleteMarks",
                                      "tableProperties", "glyphGroup", "convertText", "sortParagraphs", "insertHyperlink",
                                      "copyShape", "insertShape", "objectProperties", "deleteObject"};
    // 쪽 탭 컨트롤(머리말·꼬리말·쪽 번호·바탕쪽·메모·교정 부호 ...)은 편집기 이름 그대로다.
    for (int k = 0; k <= static_cast<int>(render::PageControlKind::InsertProofMark); ++k) {
        names.emplace_back(render::pageControlName(static_cast<render::PageControlKind>(k)));
    }
    return names;
}

bool AgentDocument::undo() {
    if (!impl_->session.canUndo()) return false;
    impl_->session.undo();
    impl_->session.edit.clearCursor();
    render::syncEditBand(impl_->doc);
    impl_->indexPages();
    return true;
}

bool AgentDocument::redo() {
    if (!impl_->session.canRedo()) return false;
    impl_->session.redo();
    impl_->session.edit.clearCursor();
    render::syncEditBand(impl_->doc);
    impl_->indexPages();
    return true;
}

void setFontCatalog(const std::string& index, const std::string& licenseSwap, const std::string& replace) {
    layout::setFontCatalog(index.empty() ? "{}" : index);
    layout::setFontLicenseSwaps(licenseSwap.empty() ? "{}" : licenseSwap);
    layout::setFontReplacements(replace.empty() ? "{}" : replace);
}


std::vector<FontFile> AgentDocument::fontPlan() {
    // 편집기와 같은 목록이다: 문서 글자에 쓰인 글꼴, 편집이 부른 글꼴(DocSession::takeFontWants),
    // 받은 글꼴에 없는 글자의 대체 글꼴(DocSession::fontGaps, 되묻는 차례는 core 가 센다).
    // 막힌 글꼴의 대역은 편집기처럼 바꾼 뒤(편집이 부른 글꼴로) 받는다.
    // 한 번 낸 파일은 받았든 못 받았든 다시 내지 않으므로 셸의 되묻기가 반드시 끝난다.
    std::vector<std::string> faces = impl_->session.takeFontWants();
    for (std::string& face : layout::documentFontFaces(impl_->doc)) faces.push_back(std::move(face));
    for (std::string& face : impl_->session.fontGaps()) faces.push_back(std::move(face));
    std::vector<FontFile> out;
    for (const app::FontFetchJob& job : app::fontFetchPlan(faces)) {
        FontFile file{job.id, job.folder + "/" + job.file, job.face};
        if (impl_->fontFailed.count(file.path) || impl_->fontFetched.count(file.path)) {
            app::fontFetchFailed(job.id);
            continue;
        }
        impl_->fontJobs[file.id] = file;
        out.push_back(std::move(file));
    }
    return out;
}

void AgentDocument::fontArrived(int id, Bytes bytes) {
    const auto found = impl_->fontJobs.find(id);
    if (found == impl_->fontJobs.end()) return;
    impl_->fontFetched.insert(found->second.path);
    impl_->landedFaces.push_back(found->second.face);
    impl_->fontJobs.erase(found);
    // 스레드 없는 셸(Host 없음)이면 그 자리에서 풀고 등록한다.
    app::fontFetched(id, std::move(bytes));
}

void AgentDocument::fontMissing(int id) {
    const auto found = impl_->fontJobs.find(id);
    if (found == impl_->fontJobs.end()) return;
    impl_->fontFailed.insert(found->second.path);
    impl_->fontJobs.erase(found);
    app::fontFetchFailed(id);
}

void AgentDocument::fontsDone() {
    if (impl_->landedFaces.empty()) return;
    // 이름 바꾸기(replace.json)와 다시 놓을지는 편집기와 같은 DocSession 판단이다.
    if (impl_->session.fontsLanded(std::exchange(impl_->landedFaces, {}))) impl_->indexPages();
}

// 편집기와 같은 DocSession 판단이다.
std::vector<std::string> AgentDocument::missingFonts() const { return impl_->session.missingFonts(); }

std::vector<std::pair<std::string, char32_t>> AgentDocument::missingGlyphs() const {
    std::vector<std::pair<std::string, char32_t>> out;
    for (layout::MissingGlyph& glyph : layout::documentMissingGlyphs(impl_->doc)) {
        out.emplace_back(std::move(glyph.face), glyph.codePoint);
    }
    return out;
}

const model::Document& AgentDocument::model() const { return impl_->doc; }

}  // namespace chwp::agent
