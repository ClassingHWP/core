#include "noting/agent.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <utility>

#include "noting/reader.hpp"
#include "noting/render/paint.hpp"
#include "noting/render/scene.hpp"
#include "noting/writer.hpp"
#include "render/paint_detail.hpp"

// 에이전트 API 구현. 판정·조판·편집은 render 쪽 함수를 그대로 부르고, 여기서는 입구 검사와 모양 바꾸기만 한다.

namespace noting::agent {

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
    Bytes source;
    model::Document doc;
    model::SourceFormat format = model::SourceFormat::Unknown;
    std::vector<std::string> warnings;
    std::vector<render::Node> pages;
    std::vector<PageInfo> pageInfos;

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
            info.label = paper.pageLabel.empty() ? std::to_string(paper.pageNumber) : paper.pageLabel;
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
};

AgentDocument::AgentDocument(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AgentDocument::~AgentDocument() {
    // 그림을 그릴 때 읽던 원본 바이트가 사라지므로 연결을 끊는다.
    render::setPictureFile({});
}

Result<std::unique_ptr<AgentDocument>> AgentDocument::open(ByteView bytes) {
    Result<std::unique_ptr<AgentDocument>> out;
    auto impl = std::make_unique<Impl>();
    impl->source.assign(bytes.begin(), bytes.end());
    ParseResult parsed = parseDocument(impl->source);
    if (!parsed.ok) {
        out.status = fail(Code::ParseFailed, parsed.error.empty() ? "문서를 읽지 못했습니다." : parsed.error);
        return out;
    }
    impl->format = detectFormat(impl->source);
    impl->warnings = std::move(parsed.warnings);
    impl->doc = std::move(parsed.document);
    // 이전 문서의 글자 윤곽·그림 캐시를 비우고, 뷰어와 같은 조판(저장된 줄 높이)을 한다.
    render::clearGlyphWarm();
    render::clearPageRenderCache();
    render::setPictureFile(impl->source);
    impl->pages = render::layoutPages(impl->doc, -1, true, false);
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
    std::string error;
    if (!writeDocument(impl_->doc, format, out.value, impl_->source, &error)) {
        out.status = fail(Code::SaveFailed, error.empty() ? "문서를 쓰지 못했습니다." : error);
        out.value.clear();
    }
    return out;
}

model::SourceFormat AgentDocument::sourceFormat() const { return impl_->format; }

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
        }
        out.push_back(std::move(info));
    }
    return out;
}

Result<ParagraphInfo> AgentDocument::paragraph(int story, int paragraph) const {
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
    info.length = at;
    return out;
}

const model::Document& AgentDocument::model() const { return impl_->doc; }

}  // namespace noting::agent
