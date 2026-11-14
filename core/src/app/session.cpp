// 편집기 세션. 웹 바인딩에 있던 판단을 모두 옮겼다. 문서·쪽·칸 캐시·카메라·입력·배경 굽기 일정·화면 판단이 여기 있다.
// 셸은 값을 바꿔 넘기고 돌려받은 값을 포장하기만 한다. 창·프레임 시계·코어 수는 Host 로 받는다.

#include "chwp/app/session.hpp"
#include "chwp/app/doc_session.hpp"
#include "chwp/app/frame_stats.hpp"
#include "chwp/render/frame_phase.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "render/line_breaker.hpp"
#include "render/scene_detail.hpp"
#include "chwp/input/hangul.hpp"
#include "chwp/export/font.hpp"
#include "chwp/export/json.hpp"
#include "chwp/export/text.hpp"
#include "chwp/input/pointer.hpp"
#include "chwp/reader.hpp"
#include "chwp/unit.hpp"
#include "chwp/writer.hpp"
#include "chwp/render/cache_budget.hpp"
#include "chwp/render/frame_cache.hpp"
#include "chwp/render/link_commands.hpp"
#include "chwp/render/track_commands.hpp"
#include "chwp/render/paint.hpp"
#include "chwp/render/scene.hpp"
#include "chwp/render/view.hpp"
#include "chwp/render/guides.hpp"

namespace chwp::app {

namespace {
// 셸의 정적 초기화가 setHost 를 먼저 부를 수 있다. 전역 객체로 두면 이 파일의 초기화가 뒤에 와서 빈 값으로 덮는다.
Host& hostSlot() {
    static Host host;
    return host;
}
}  // namespace

constexpr std::size_t kSmallMemoryFontBudget = 24u * 1024u * 1024u;

void setHost(Host host) {
    hostSlot() = std::move(host);
    const Host& slot = hostSlot();
    chwp::layout::WorkerLimits limits;
    limits.cores = slot.logicalCores ? slot.logicalCores() : -1;
    limits.minWorkers = slot.minWorkers;
    limits.maxWorkers = slot.maxWorkers;
    chwp::layout::setWorkerLimits(limits);
    chwp::render::setZoomedFrameLimit(slot.zoomedFrameSide, slot.zoomedFramePixels);
    // 메모리가 좁은 기기는 열어 둘 글꼴 파일(압축본 포함)을 24MB 로 줄인다. 아니면 core 기본값이다.
    if (slot.smallMemory) chwp::render::setCacheBudget("fonts", kSmallMemoryFontBudget);
    chwp::render::setScreenVectors(slot.screenVectors);
}

// 셸의 프레임 시각과 같은 기준이다. 셸이 시계를 주지 않으면 steady_clock 이다.
// (웹 스레드 빌드의 emscripten_get_now() 는 기준이 달라 프레임 시각과 빼면 음수가 된다. 셸이 맞는 시계를 준다.)
double frameNow() {
    if (hostSlot().nowMs) return hostSlot().nowMs();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void useFramePlan(chwp::render::ViewPlan& plan);
int replaceFontFace(const std::string& from, const std::string& to);
bool paperPoint(double vx, double vy, bool clampToNearest, int& page, int& x, int& y);

std::string detectFormat(const Bytes& bytes) { return chwp::formatName(chwp::detectFormat(bytes)); }

std::string parse(const Bytes& bytes, bool pretty, bool includeLineSegs) {
    chwp::ParseOptions options;
    options.keepLineSegs = includeLineSegs;
    const chwp::ParseResult result = chwp::parseDocument(bytes, options);
    chwp::JsonOptions json;
    json.pretty = pretty;
    json.includeLineSegs = includeLineSegs;
    return chwp::parseResultToJson(result, json);
}

std::string extractText(const Bytes& bytes) {
    chwp::ParseOptions options;
    options.keepLineSegs = false;
    const chwp::ParseResult result = chwp::parseDocument(bytes, options);
    return result.ok ? chwp::documentToText(result.document) : std::string();
}

std::string extractPages(const Bytes& bytes) {
    chwp::ParseOptions options;
    options.keepLineSegs = true;
    const chwp::ParseResult result = chwp::parseDocument(bytes, options);
    chwp::JsonWriter writer(false);
    writer.beginArray();
    if (result.ok) {
        for (const auto& page : chwp::documentToPages(result.document)) writer.value(page);
    }
    writer.endArray();
    return writer.take();
}

std::string extractAnchorPages(const Bytes& bytes) {
    chwp::ParseOptions options;
    const chwp::ParseResult result = chwp::parseDocument(bytes, options);
    chwp::JsonWriter writer(false);
    writer.beginArray();
    if (result.ok) {
        std::vector<int> anchors;
        chwp::layout::paginate(result.document, {}, &anchors);
        for (const int page : anchors) writer.value(page);
    }
    writer.endArray();
    return writer.take();
}

std::string extractInnerLines(const Bytes& bytes) {
    chwp::ParseOptions options;
    options.keepLineSegs = true;
    const chwp::ParseResult result = chwp::parseDocument(bytes, options);
    chwp::JsonWriter writer(false);
    writer.beginArray();
    if (result.ok) {
        std::vector<std::vector<std::uint8_t>> inner;
        chwp::layout::paginate(result.document, {}, nullptr, &inner);
        for (const auto& page : inner) {
            writer.beginArray();
            for (const std::uint8_t flag : page) writer.value(static_cast<int>(flag));
            writer.endArray();
        }
    }
    writer.endArray();
    return writer.take();
}

std::string extractLayout(const Bytes& bytes) {
    chwp::ParseOptions options;
    options.keepLineSegs = true;
    const chwp::ParseResult result = chwp::parseDocument(bytes, options);
    std::vector<std::string> pages;
    std::vector<int> anchors;
    std::vector<std::vector<std::uint8_t>> inner;
    std::vector<std::vector<int>> paragraphPages;
    std::vector<std::vector<chwp::layout::LineRange>> lineRanges;
    if (result.ok) {
        pages = chwp::layout::paginate(result.document, {}, &anchors, &inner, &paragraphPages,
                                         &lineRanges);
    }
    chwp::JsonWriter writer(false);
    writer.beginObject();
    writer.key("pages");
    writer.beginArray();
    for (const auto& page : pages) writer.value(page);
    writer.endArray();
    writer.key("inner");
    writer.beginArray();
    for (const auto& flags : inner) {
        writer.beginArray();
        for (const std::uint8_t flag : flags) writer.value(static_cast<int>(flag));
        writer.endArray();
    }
    writer.endArray();
    writer.key("anchors");
    writer.beginArray();
    for (const int page : anchors) writer.value(page);
    writer.endArray();
    writer.key("paragraphs");
    writer.beginArray();
    for (const auto& indexes : paragraphPages) {
        writer.beginArray();
        for (const int index : indexes) writer.value(index);
        writer.endArray();
    }
    writer.endArray();
    writer.key("lines");
    writer.beginArray();
    for (const auto& page : lineRanges) {
        writer.beginArray();
        for (const auto& line : page) {
            writer.beginObject();
            writer.field("paragraph", line.paragraph);
            writer.field("start", line.start);
            writer.field("end", line.end);
            writer.endObject();
        }
        writer.endArray();
    }
    writer.endArray();
    writer.endObject();
    return writer.take();
}

const char* sceneKind(chwp::render::NodeKind kind) {
    switch (kind) {
        case chwp::render::NodeKind::Paper: return "paper";
        case chwp::render::NodeKind::Body: return "body";
        case chwp::render::NodeKind::Column: return "column";
        case chwp::render::NodeKind::Paragraph: return "paragraph";
        case chwp::render::NodeKind::Line: return "line";
        case chwp::render::NodeKind::Object: return "object";
        case chwp::render::NodeKind::Cell: return "cell";
    }
    return "node";
}

void writeEdge(chwp::JsonWriter& writer, const char* name, const chwp::render::BorderEdge& edge) {
    if (edge.type.empty()) return;
    writer.key(name);
    writer.beginObject();
    writer.field("type", edge.type);
    if (!edge.color.empty()) writer.field("color", edge.color);
    writer.field("width", edge.width);
    writer.endObject();
}

void writeWidths(chwp::JsonWriter& writer, const char* name, const std::int32_t* widths, std::size_t count) {
    if (count == 0) return;
    writer.key(name);
    writer.beginArray();
    for (std::size_t i = 0; i < count; ++i) writer.value(widths[i]);
    writer.endArray();
}

// 줄 글자 표의 한 구간(또는 머리 번호)을 장면 JSON 의 글자 묶음("span")으로 쓴다. 꾸밈은 글자 모양에서 읽는다.
void writeSpan(chwp::JsonWriter& writer, const chwp::model::Document& doc, std::string_view text, std::int32_t x,
               std::int32_t width, std::uint32_t charShapeId, const chwp::render::RunStyle& style,
               const std::string& font, const std::int32_t* advances, std::size_t advanceCount,
               const std::int32_t* inks, std::size_t inkCount, bool heading) {
    const chwp::render::detail::CharPaint paint = chwp::render::detail::charPaint(doc, charShapeId);
    writer.beginObject();
    writer.field("kind", "span");
    writer.field("x", x);
    writer.field("y", 0);
    writer.field("width", width);
    writer.field("height", 0);
    if (!text.empty()) writer.field("text", std::string(text));
    if (heading) writer.field("headingMark", true);
    if (style.fontSize > 0) writer.field("fontSize", style.fontSize);
    if (!font.empty()) writer.field("font", font);
    if (!paint.color.empty()) writer.field("color", paint.color);
    if (!paint.shade.empty()) writer.field("shade", paint.shade);
    if (style.markpen && !style.markpen->empty()) writer.field("markpen", *style.markpen);
    if (paint.bold) writer.field("bold", true);
    if (paint.italic) writer.field("italic", true);
    if (!paint.underline.empty()) writer.field("underline", paint.underline);
    if (paint.strikeout) writer.field("strikeout", true);
    if (style.ratio != 100) writer.field("ratio", style.ratio);
    if (style.relSize != 100) writer.field("relSize", style.relSize);
    if (style.spacing != 0) writer.field("spacing", style.spacing);
    if (style.charOffset != 0) writer.field("charOffset", style.charOffset);
    writeWidths(writer, "advances", advances, advanceCount);
    writeWidths(writer, "inks", inks, inkCount);
    writer.field("charShapeId", charShapeId);
    if (!paint.fillColor.empty()) writer.field("fillColor", paint.fillColor);
    writer.key("children");
    writer.beginArray();
    writer.endArray();
    writer.endObject();
}

void writeScene(chwp::JsonWriter& writer, const chwp::model::Document& doc, const chwp::render::Node& node) {
    writer.beginObject();
    writer.field("kind", sceneKind(node.kind));
    writer.field("x", node.x);
    writer.field("y", node.y);
    writer.field("width", node.width);
    writer.field("height", node.height);
    writer.field("paragraph", node.paragraph);
    writer.field("start", node.start);
    writer.field("end", node.end);
    writer.field("treatAsChar", node.treatAsChar);
    if (!node.text().empty()) writer.field("text", node.text());
    if (!node.ext().headingType.empty()) {
        writer.field("headingType", node.ext().headingType);
        writer.field("headingLevel", node.headingLevel);
        if (!node.ext().headingFormat.empty()) writer.field("headingFormat", node.ext().headingFormat);
        if (!node.ext().headingText.empty()) writer.field("headingText", node.ext().headingText);
    }
    if (node.fontSize > 0) writer.field("fontSize", node.fontSize);
    if (node.kind == chwp::render::NodeKind::Line && node.lineData().textHeight > 0) {
        writer.field("textHeight", node.lineData().textHeight);
    }
    if (!node.font.empty()) writer.field("font", node.font);
    if (node.kind == chwp::render::NodeKind::Line) {
        writer.field("charShapeId", node.charShapeId);
    }
    if (node.kind == chwp::render::NodeKind::Object || node.kind == chwp::render::NodeKind::Cell) {
        writer.field("zOrder", node.ext().obj().zOrder);
    }
    if (!node.ext().obj().textWrap.empty()) writer.field("textWrap", node.ext().obj().textWrap);
    if (!node.ext().fillColor.empty()) writer.field("fillColor", node.ext().fillColor);
    const bool hasBorder = !node.ext().borderLeft.type.empty() || !node.ext().borderRight.type.empty() ||
                           !node.ext().borderTop.type.empty() || !node.ext().borderBottom.type.empty();
    if (hasBorder) {
        writer.key("border");
        writer.beginObject();
        writeEdge(writer, "left", node.ext().borderLeft);
        writeEdge(writer, "right", node.ext().borderRight);
        writeEdge(writer, "top", node.ext().borderTop);
        writeEdge(writer, "bottom", node.ext().borderBottom);
        writer.endObject();
    }
    if (node.kind == chwp::render::NodeKind::Paragraph) {
        if (node.marginLeft) writer.field("marginLeft", node.marginLeft);
        if (node.marginRight) writer.field("marginRight", node.marginRight);
        if (node.indent) writer.field("indent", node.indent);
        if (node.spacingPrev) writer.field("spacingPrev", node.spacingPrev);
        if (node.spacingNext) writer.field("spacingNext", node.spacingNext);
        if (!node.align.empty()) writer.field("align", node.align);
    }
    if (node.kind == chwp::render::NodeKind::Line) {
        if (!node.align.empty()) writer.field("align", node.align);
        if (node.lineData().lastLine) writer.field("lastLine", true);
        if (node.lineData().lineBreak) writer.field("lineBreak", true);
    }
    if (node.kind == chwp::render::NodeKind::Cell) {
        writer.field("row", node.ext().row);
        writer.field("col", node.ext().col);
        writer.field("rowSpan", node.ext().rowSpan);
        writer.field("colSpan", node.ext().colSpan);
        if (!node.vertAlign.empty()) writer.field("vertAlign", node.vertAlign);
        writer.field("marginLeft", node.marginLeft);
        writer.field("marginRight", node.marginRight);
        writer.field("marginTop", node.marginTop);
        writer.field("marginBottom", node.marginBottom);
    }
    if (node.ext().cellSpacing != 0) writer.field("cellSpacing", node.ext().cellSpacing);
    if (node.kind == chwp::render::NodeKind::Object || node.kind == chwp::render::NodeKind::Paper) {
        if (node.marginLeft) writer.field("marginLeft", node.marginLeft);
        if (node.marginRight) writer.field("marginRight", node.marginRight);
        if (node.marginTop) writer.field("marginTop", node.marginTop);
        if (node.marginBottom) writer.field("marginBottom", node.marginBottom);
    }
    if (!node.role.empty()) writer.field("role", node.role);
    if (!node.ext().obj().script.empty()) writer.field("script", node.ext().obj().script);
    if (!node.ext().obj().binItemId.empty()) writer.field("binItemId", node.ext().obj().binItemId);
    writer.key("children");
    writer.beginArray();
    // 줄 글자는 글자 표에 있다. 장면 JSON 은 머리 번호, 그다음 표 차례로 글자 묶음과 개체를 쓴다.
    if (node.kind == chwp::render::NodeKind::Line) {
        const chwp::render::LineGlyphs& glyphs = node.glyphs();
        if (const chwp::render::HeadingRun* head = glyphs.heading.get()) {
            chwp::render::RunStyle style;
            style.fontSize = head->fontSize;
            style.ratio = head->ratio;
            style.relSize = head->relSize;
            writeSpan(writer, doc, head->text, head->x, head->width, head->charShapeId, style, head->font,
                      head->advances.data(), head->advances.size(), nullptr, 0, true);
        }
        for (const auto& child : node.children) {
            if (child.headingMark) writeScene(writer, doc, child);
        }
        for (const chwp::render::LineRun& run : glyphs.runs) {
            if (run.kind == chwp::render::RunKind::Object) {
                if (run.slot < node.children.size()) writeScene(writer, doc, node.children[run.slot]);
                continue;
            }
            const std::string_view text = std::string_view(node.text()).substr(run.textBegin, run.textEnd - run.textBegin);
            writeSpan(writer, doc, text, 0, run.width, run.charShapeId, *run.style,
                      run.style->font ? *run.style->font : std::string{}, glyphs.advances.data() + run.advanceAt,
                      run.advanceCount, glyphs.inks.data() + run.inkAt, run.inkCount, false);
        }
    } else {
        for (const auto& child : node.children) writeScene(writer, doc, child);
    }
    writer.endArray();
    writer.endObject();
}

std::string layoutScene(const Bytes& bytes) {
    chwp::ParseOptions options;
    const chwp::ParseResult result = chwp::parseDocument(bytes, options);
    chwp::JsonWriter writer(false);
    writer.beginArray();
    if (result.ok) {
        for (const auto& page : chwp::render::layoutPages(result.document)) writeScene(writer, result.document, page);
    }
    writer.endArray();
    return writer.take();
}

chwp::model::SourceFormat parseFormatString(const std::string& formatStr) {
    std::string lower = formatStr;
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lower == "hwp") return chwp::model::SourceFormat::Hwp;
    if (lower == "hwpx") return chwp::model::SourceFormat::Hwpx;
    return chwp::model::SourceFormat::Unknown;
}

std::optional<Bytes> convertDocument(const Bytes& srcBytes, const std::string& formatStr) {
    chwp::ParseOptions parseOpts;
    parseOpts.keepLineSegs = true;
    const chwp::ParseResult parsed = chwp::parseDocument(srcBytes, parseOpts);
    if (!parsed.ok) return std::nullopt;
    chwp::ExportOptions exportOpts;
    exportOpts.format = parseFormatString(formatStr);
    exportOpts.sourceData = srcBytes;
    const chwp::ExportResult exported = chwp::writeDocument(parsed.document, exportOpts);
    if (!exported.ok) return std::nullopt;
    return exported.data;
}

void registerFontFaces(const std::vector<std::string>& faces, chwp::Bytes bytes) {
    for (std::size_t i = 0; i < faces.size(); ++i) {
        chwp::layout::registerFontFace(faces[i], i == 0 ? std::move(bytes) : chwp::Bytes{});
    }
}

void relayoutForLandedFonts(const std::vector<std::string>& faces);

namespace {

struct PlannedFont {
    int batch = 0;
    std::vector<std::string> faces;
    std::vector<std::pair<std::string, std::string>> replacements;
    bool installed = false;
    bool landed = false;
};

std::mutex gFontPlanMu;
std::unordered_map<int, PlannedFont> gFontPlans;
// 묶음마다 아직 받거나 풀지 못한 파일 수.
std::unordered_map<int, int> gFontBatchWaiting;
int gNextFontJob = 1;
int gNextFontBatch = 1;
// 이미 넘긴 글꼴 이름. 문서를 바꿔도 등록한 글꼴은 남으니 지우지 않는다.
std::unordered_set<std::string> gFontLoaded;

// 셸 목록 경로를 받을 폴더와 파일로. metrics/ 는 폭만 잰 파일, full/ 은 글꼴 파일, standin/ 은 막힌 글꼴의
// 대역이며 파일은 full 폴더에 같이 있다.
bool fetchPathOf(const std::string& path, std::string& folder, std::string& file) {
    const auto take = [&](const char* prefix, const char* into) {
        const std::string p(prefix);
        if (path.rfind(p, 0) != 0) return false;
        folder = into;
        file = path.substr(p.size());
        return !file.empty();
    };
    return take("metrics/", "metrics") || take("full/", "full") || take("standin/", "full");
}

// 받은(또는 못 받은) 파일 하나가 끝났다. 셸 스레드에서 부른다. 묶음이 다 끝나면 대신 받은 글꼴 이름을
// 문서에 적용하고 셸에 알린다. 편집이 부른 글꼴이 문서에 쓰이면 문서를 다시 놓는다.
void fontJobLanded(int id) {
    int batch = 0;
    std::vector<std::string> landedFaces;
    {
        std::lock_guard lock(gFontPlanMu);
        const auto found = gFontPlans.find(id);
        if (found == gFontPlans.end() || found->second.landed) return;
        found->second.landed = true;
        batch = found->second.batch;
        int& waiting = gFontBatchWaiting[batch];
        if (--waiting > 0) return;
        gFontBatchWaiting.erase(batch);
        for (auto it = gFontPlans.begin(); it != gFontPlans.end();) {
            if (it->second.batch != batch) {
                ++it;
                continue;
            }
            if (it->second.installed) {
                landedFaces.insert(landedFaces.end(), it->second.faces.begin(), it->second.faces.end());
            }
            it = gFontPlans.erase(it);
        }
    }
    relayoutForLandedFonts(landedFaces);
    if (hostSlot().fontsSettled) hostSlot().fontsSettled(batch);
}

// 함초롬처럼 큰 woff2 는 풀고 표를 읽는 데 수백 ms 걸린다. 스레드를 쓸 수 있고 셸 스레드로 돌려보낼 수
// 있으면 받은 파일마다 스레드를 두어 함께 풀고, 끝나면 셸 스레드에서 뒷일을 한다.
void decodeFont(int id, std::vector<std::string> names, Bytes bytes) {
    const std::function<void(std::function<void()>)> post = hostSlot().post;
    const bool threads = post && hostSlot().logicalCores && hostSlot().logicalCores() > 0;
    if (!threads) {
        registerFontFaces(names, std::move(bytes));
        fontJobLanded(id);
        return;
    }
    std::thread([id, post, names = std::move(names), bytes = std::move(bytes)]() mutable {
        try {
            registerFontFaces(names, std::move(bytes));
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[ClassingHWP 오류] decodeFont %s: %s\n", names.front().c_str(), ex.what());
        }
        post([id] { fontJobLanded(id); });
    }).detach();
}

}  // namespace

std::vector<FontFetchJob> fontFetchPlan(const std::vector<std::string>& faces) {
    std::lock_guard lock(gFontPlanMu);
    std::vector<FontFetchJob> jobs;
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, int> byPath;
    // 무엇이 필요한지는 core 가 정한 목록(documentFontFaces, documentFontGaps, 편집 대체)이다. 여기서 덧붙이지 않는다.
    const std::vector<std::string>& requested = faces;
    const int batch = gNextFontBatch;
    for (const std::string& face : requested) {
        if (face.empty() || seen.count(face)) continue;
        const std::string next = chwp::layout::fontReplacement(face);
        const bool replaced = !next.empty() && next != face;
        const std::string fetchFace = replaced ? next : face;
        if (!replaced && gFontLoaded.count(face)) continue;
        seen.insert(face);
        std::string folder;
        std::string file;
        if (!fetchPathOf(chwp::layout::fontCatalogPath(fetchFace), folder, file)) continue;
        const std::string key = folder + "/" + file;
        int id = 0;
        if (const auto found = byPath.find(key); found != byPath.end()) {
            id = found->second;
            std::vector<std::string>& listed = gFontPlans[id].faces;
            if (std::find(listed.begin(), listed.end(), fetchFace) == listed.end()) listed.push_back(fetchFace);
        } else {
            id = gNextFontJob++;
            gFontPlans[id].batch = batch;
            gFontPlans[id].faces.push_back(fetchFace);
            byPath[key] = id;
            jobs.push_back(FontFetchJob{id, batch, folder, file, fetchFace});
        }
        if (replaced) gFontPlans[id].replacements.emplace_back(face, next);
    }
    if (!jobs.empty()) {
        gFontBatchWaiting[batch] = static_cast<int>(jobs.size());
        ++gNextFontBatch;
    }
    return jobs;
}

void fontFetched(int id, Bytes bytes) {
    if (bytes.size() < 12) {
        fontFetchFailed(id);
        return;
    }
    std::vector<std::string> faces;
    {
        std::lock_guard lock(gFontPlanMu);
        const auto found = gFontPlans.find(id);
        if (found == gFontPlans.end() || found->second.landed) return;
        for (const std::string& face : found->second.faces) {
            if (face.empty() || gFontLoaded.count(face)) continue;
            faces.push_back(face);
            gFontLoaded.insert(face);
        }
        found->second.installed = true;
    }
    if (faces.empty()) {
        fontJobLanded(id);
        return;
    }
    decodeFont(id, std::move(faces), std::move(bytes));
}

void fontFetchFailed(int id) { fontJobLanded(id); }

int installFont(const std::vector<std::string>& faces, Bytes bytes) {
    if (faces.empty() || bytes.size() < 12) return 0;
    std::vector<std::string> fresh;
    {
        std::lock_guard lock(gFontPlanMu);
        for (const std::string& face : faces) {
            if (face.empty() || gFontLoaded.count(face)) continue;
            fresh.push_back(face);
            gFontLoaded.insert(face);
        }
    }
    if (!fresh.empty()) registerFontFaces(fresh, std::move(bytes));
    return static_cast<int>(fresh.size());
}

// 셸이 플랫폼 메모리 신호를 넘기는 통로. 1 가벼움, 2 보통, 3 심각.
void trimCaches(int level) {
    chwp::render::trimCaches(static_cast<chwp::render::TrimLevel>(std::clamp(level, 1, 3)));
}

// 바이트. 음수면 예산 없음.
bool setCacheBudget(const std::string& name, double bytes) {
    const std::size_t budget = bytes < 0 ? chwp::render::kNoCacheBudget : static_cast<std::size_t>(bytes);
    return chwp::render::setCacheBudget(name, budget);
}

std::string cacheUsage() {
    chwp::JsonWriter writer(false);
    writer.beginArray();
    for (const chwp::render::CacheUsage& usage : chwp::render::cacheUsage()) {
        writer.beginObject();
        writer.field("name", usage.name);
        writer.field("bytes", static_cast<std::uint64_t>(usage.bytes));
        writer.field("budget", usage.budget == chwp::render::kNoCacheBudget
                                     ? std::int64_t{-1}
                                     : static_cast<std::int64_t>(usage.budget));
        writer.endObject();
    }
    writer.endArray();
    return writer.take();
}

// 화면이 붙은 문서. 열기·조판·편집 명령·저장은 agent 문서와 같은 DocSession 을 탄다.
// 이 문서의 편집 상태를 프로세스 기본으로 잡아, 커서를 읽는 화면·입력 코드도 같은 상태를 본다.
chwp::app::DocSession gDoc;
const chwp::render::EditScope gDocEditScope(gDoc.edit);

int replaceFontFace(const std::string& from, const std::string& to) {
    return chwp::model::replaceFontFace(gDoc.doc, from, to);
}
std::vector<std::vector<std::uint8_t>> gPagePng;
std::vector<chwp::render::PageBitmap> gPagePixels;
chwp::render::PageView gCamera;
chwp::input::PointerInput gInput;
// 플랫폼 휠을 읽고 휠 끝을 정한다. 끝은 프레임마다(화면) 또는 tick 마다(화면 없음) 본다.
chwp::input::WheelReader gWheel;
// 사용자가 켠 눈금자. 셸이 저장해 둔 값을 넘기고, 없으면 켠다.
bool gRulersWanted = true;
// 셸이 켠 보기 전용. 배포용 문서는 이것과 상관없이 늘 읽기 전용이다.
bool gShellReadOnly = false;
// 보기 화면의 편집 단추. 넣은 페이지가 임베드 명령으로 켠다.
bool gEditButton = false;
// 쪽 그림(renderPagePng·renderPagePixels)을 그린 높이. 바뀌면 그린 그림을 버린다.
int gPixelHeight = 0;
int gLastPageGap = 0;

bool gScreenOn = false;
// 셸이 창 크기만 바꿨다. 다음 프레임이 쪽 배치를 다시 잡는다.
bool gViewportDirty = false;
// 격자·안내선 보기가 바뀌었다. 문서·카메라는 그대로여도 다음 프레임을 다시 내보낸다.
bool gGuidesDirty = false;
int gShellNoteW = -1;
int gShellNoteH = -1;
int gShellNoteMaxX = -1;
int gShellNoteMaxY = -1;
int gShellNotePercent = -1;
// GPU 가 없을 때 셸이 베끼는 동안만 살아있는 픽셀. 구간 판단은 코어에 있다.
std::vector<chwp::render::SelectionImage> gFallbackImages;
chwp::render::ViewPlan gLastPlan;
int gLastPlanRevision = -1;
double gScrollHold = 0;
double gLastCameraMoveTime = 0;
double gLastZoomMoveTime = 0;
int gIdleMoveStreak = 0;
bool gPageGpuBakePending = false;

chwp::render::FrameCache gFrames;

void stopCrew();
void pauseCrewForEdit();
void resumeCrewAfterEdit(const chwp::render::EditResult& res);
chwp::render::EditResult syncClickHereGuidePaused();
bool syncClickHereGuideNow();
void revealClickHereGuideNow();
void drainDone();
void publishPlan();
void noteCamera(int scrollX, int scrollY, int percent, bool coasting, bool pinching);
void mountCameraFromPages();
bool crewRunning();
// 일을 집는 일꾼 스레드 수. 0 이면 화면 스레드가 일꾼 걸음을 대신 밟는다.
int crewThreads();
void runSoloCrew(double budgetMs);
void beginGrind();
void ensureScreenFrames();
bool screenFrame(double time, void*);

// 편집 하나가 끝나면 화면을 그린다. 여러 편집을 묶어 할 때는 다 하고 한 번만 그린다(gHeldFrames).
int gHeldFrames = 0;
void frameAfterEdit();
int gMasterViewSeen = 0;
int gScreenGlyphSeen = 0;
int currentCameraPage(int scrollX, int scrollY, int percent);

using LocalPageSprite = chwp::render::PageSprite;

struct PageSpriteCache {
    int generation = -1;
    int pageRevision = -1;
    int zoom = -1;
    int targetPercent = -1;
    int dstPixelH = -1;
    int pageStamp = 0;
    bool multiPercent = false;
    std::vector<LocalPageSprite> sprites;
    std::vector<int> maxLocalBottomPrefix;
    std::vector<int> minLocalYSuffix;
    std::size_t layerOffset[5] = {0, 0, 0, 0, 0};
};

std::unordered_map<int, PageSpriteCache> gPageSpriteCaches;

void clearKeptFrames() {
    gFrames.clear();
    gLastPlanRevision = -1;
    gPageSpriteCaches.clear();
    gPageGpuBakePending = false;
}

void clearPointer() { chwp::render::clearPointerSelection(); }

void clearPageSlots() {
    stopCrew();
    clearKeptFrames();
    gDoc.pages.clear();
    gPagePng.clear();
    gPagePixels.clear();
    gPixelHeight = 0;
}

std::vector<std::string> fontGapFaces() { return gDoc.fontGaps(); }

void syncFullRelayout(bool bringSelection);

// 글꼴이 들어왔다. 이름 바꾸기(replace.json)와 다시 놓을지는 DocSession 이 정한다(agent 와 같은 판단).
// 다시 놓았으면 화면과 일꾼을 처음부터 맞춘다.
void relayoutForLandedFonts(const std::vector<std::string>& faces) {
    const bool wasCrew = !gDoc.pages.empty() && crewRunning();
    if (wasCrew) pauseCrewForEdit();
    const bool relaid = gDoc.fontsLanded(faces);
    if (relaid) syncFullRelayout(false);
    if (wasCrew) {
        chwp::render::EditResult resumed;
        resumed.documentChanged = relaid;
        resumed.fullInvalidation = relaid;
        resumeCrewAfterEdit(resumed);
    }
    if (relaid) frameAfterEdit();
}

// 셸에 마지막으로 보낸 모양. 문서를 새로 열면 비운다.
std::optional<StyleReport> gSentStyle;

void resetSessionForDocument() {
    gSentStyle.reset();
    stopCrew();
    chwp::render::resetFontFold();
    gDoc.close();
    clearPageSlots();
    clearPointer();
    chwp::render::clearPageRenderCache();
    gCamera.reset();
    gInput.stop();
}

std::vector<std::string> documentFaceList() {
    std::vector<std::string> seen;
    auto addFace = [&](const std::string& name) {
        if (name.empty()) return;
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) return;
        seen.push_back(name);
    };
    for (const auto& group : gDoc.doc.header.fonts) {
        for (const auto& font : group) {
            addFace(font.face);
            addFace(font.subst);
        }
    }
    return seen;
}

// 고른 글을 복사할 글로 돌려준다. 복사를 막은 배포용 문서면 비운다(셸은 빈 글을 복사하지 않는다).
std::string copyableSelectionText() {
    if (gDoc.copyBlocked()) return {};
    return chwp::render::selectionText(gDoc.doc, gDoc.pages);
}

OpenResult openedDocumentInfo() {
    OpenResult out;
    out.ok = true;
    // 배포용 문서는 고칠 수 없다(한글도 배포용 문서는 편집을 막는다). 복사·인쇄 막기는 문서가 정한다.
    chwp::render::setReadOnly(gShellReadOnly || gDoc.locked());
    out.locked = gDoc.locked();
    if (gDoc.locked()) {
        out.notice = "배포용 문서라 고칠 수 없습니다.";
        if (gDoc.copyBlocked() && gDoc.printBlocked()) out.notice += " 복사와 인쇄도 막혀 있습니다.";
        else if (gDoc.copyBlocked()) out.notice += " 복사도 막혀 있습니다.";
        else if (gDoc.printBlocked()) out.notice += " 인쇄도 막혀 있습니다.";
    }
    out.faces = documentFaceList();
    out.docState = gDoc.documentState();
    chwp::layout::requestFontFaces(gDoc.openFontFaces());
    return out;
}

// 새 문서는 쪽을 놓은 뒤 첫 문단에 커서를 둔다. 커서가 있어야 바로 글을 치고 표·그림을 넣을 수 있다.
bool gCaretAtStart = false;

OpenResult openDocument(Bytes bytes) {
    resetSessionForDocument();
    gCaretAtStart = false;

    std::string error;
    if (!gDoc.open(std::move(bytes), &error)) {
        OpenResult out;
        out.error = error;
        out.needPassword = gDoc.needsPassword();
        out.wrongPassword = gDoc.wrongPassword();
        return out;
    }
    return openedDocumentInfo();
}

OpenResult unlockDocument(const std::string& password) {
    // 문서는 openDocument 가 이미 비웠다. 들고 있는 잠긴 바이트만 남아 있다.
    gCaretAtStart = false;
    std::string error;
    if (!gDoc.unlock(password, &error)) {
        OpenResult out;
        out.error = error;
        out.needPassword = gDoc.needsPassword();
        out.wrongPassword = gDoc.wrongPassword();
        return out;
    }
    return openedDocumentInfo();
}

OpenResult newDocument() {
    resetSessionForDocument();
    gDoc.openBlank();
    gCaretAtStart = true;
    OpenResult out = openedDocumentInfo();
    out.name = "빈 문서";
    return out;
}

std::optional<Bytes> exportDocument(const std::string& formatStr) {
    // 고친 문단과 lineSeg 없이 들어온 문단은 우리 조판 결과를 lineSeg 로 적어 저장한다(agent 저장과 같다).
    gDoc.ensureFullLayout();
    // 숨긴 누름틀 안내문은 보기 상태다. 안내문이 보이는 문서로 저장하고 다시 캐럿에 맞춘다.
    std::optional<Bytes> bytes;
    {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrewForEdit();
        chwp::render::PageFillGuard guard;
        revealClickHereGuideNow();
        bytes = gDoc.save(parseFormatString(formatStr));
        if (wasCrew) resumeCrewAfterEdit({});
        else syncClickHereGuidePaused();
    }
    return bytes;
}

SavedDocument saveDocument(const std::string& name) {
    SavedDocument out;
    // 조합 끝이 와서 확정을 기다리는 글이 있으면 먼저 확정한다(입력칸을 놓으며 저장을 누르면 끝이 바로 온다).
    editIme("settle", "", false);
    const chwp::model::SourceFormat format = gDoc.saveFormatFor(name).value_or(chwp::model::SourceFormat::Hwpx);
    const std::string ext = format == chwp::model::SourceFormat::Hwp ? ".hwp" : ".hwpx";
    out.format = ext.substr(1);
    out.name = name.empty() ? "문서" : name;
    std::string lower = out.name;
    for (char& c : lower) c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (lower.size() < ext.size() || lower.compare(lower.size() - ext.size(), ext.size(), ext) != 0) out.name += ext;
    gDoc.ensureFullLayout();
    {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrewForEdit();
        chwp::render::PageFillGuard guard;
        revealClickHereGuideNow();
        out.bytes = gDoc.save(format, &out.error);
        if (wasCrew) resumeCrewAfterEdit({});
        else syncClickHereGuidePaused();
    }
    if (out.bytes) gDoc.markSaved();
    out.docState = gDoc.documentState();
    return out;
}

void noteFailure(const char* where, const std::exception* ex = nullptr) {
    if (ex && ex->what() && ex->what()[0] != '\0') {
        std::fprintf(stderr, "[ClassingHWP 오류] %s: %s\n", where, ex->what());
    } else {
        std::fprintf(stderr, "[ClassingHWP 오류] %s\n", where);
    }
}

// 폰트 목록이 끝난 뒤에 부른다.
// 쪽·단·개체는 항상 우리 조판이다. lineseg 가 있으면 그 글자 위치에서 줄을 끊고 그 높이를 쓴다.
// 진행 폭은 쓰고, 글리프 윤곽은 읽지 않는다.
std::vector<std::string> missingFonts() { return gDoc.missingFonts(); }

// 열 때 먼저 쌓는 쪽 수. 두 쪽·여러 쪽 보기의 첫 화면을 채운다.
constexpr int kOpenPages = 4;
// 쪽 명령(page=N, 다음 쪽)이 가리키는 쪽 뒤로 더 쌓아 둘 쪽 수. 그 쪽부터 한 화면을 채운다.
constexpr int kPageLookahead = 3;
// 프레임마다 남은 쪽을 쌓는 시간(ms). 손이 움직이는 동안은 짧게 쌓는다.
constexpr double kLayoutIdleMs = 8.0;
constexpr double kLayoutBusyMs = 2.0;
// 일꾼이 그림을 푸는 중이면 쌓기를 미룬다. 그래도 이만큼 프레임을 미뤘으면 쌓는다.
constexpr int kLayoutWaitFrames = 6;

void completeLayoutOnScreen();
void syncLaidPages();
void layoutThroughPage(int index);
void continueLayout(bool busy);
bool crewPausedNow();
bool crewBusyNow();
void resumeCrewQuietly();
// 쌓은 쪽이 늘어 셸에 쪽 수를 다시 알려야 한다. 다음 프레임이 카메라 알림을 보낸다.
bool gCameraNoteDue = false;

std::string planDocument() { return planDocument(DocSession::kUseStoredLineHeight); }

std::string planDocument(bool lineSeg) {
    stopCrew();
    // 다시 짜도 커서는 그 글자 자리에 남긴다. 새 문서는 첫 문단 맨 앞이다.
    const chwp::render::PointerCursor keep = chwp::render::pointerCursor();
    clearPointer();
    clearKeptFrames();
    gPagePng.clear();
    gPagePixels.clear();
    gPixelHeight = 0;
    chwp::render::clearGlyphWarm();
    // 칸은 위에서 모두 비웠다. 지운 글자 때문에 다시 구울 칸이 없다.
    gScreenGlyphSeen = chwp::render::screenGlyphEpoch();
    chwp::JsonWriter writer(false);
    writer.beginObject();
    writer.key("pages");
    writer.beginArray();
    struct PageCalcLog {
        PageCalcLog() { chwp::render::setPageCalcLog(true); }
        ~PageCalcLog() { chwp::render::setPageCalcLog(false); }
    } pageCalcLog;
    // 보일 쪽만 먼저 쌓고 첫 화면을 그린다. 남은 쪽은 프레임마다 조금씩 쌓는다(continueLayout).
    // 가운데 쪽을 여는 주소(page=N)는 embedOpen 이 그 쪽까지 더 쌓는다.
    gDoc.finishLayoutHook = [] { completeLayoutOnScreen(); };
    gDoc.layoutBegin(lineSeg);
    gDoc.layoutUntil(kOpenPages);
    gDoc.takePagesMoved();
    if (!gDoc.pages.empty()) {
        if (gCaretAtStart) {
            chwp::render::setPointerCursor(gDoc.doc, gDoc.pages, chwp::model::CursorPos{1, 0, 0});
        } else if (keep.active && keep.story > 0) {
            chwp::render::setPointerCursor(gDoc.doc, gDoc.pages,
                                             chwp::model::CursorPos{keep.story, keep.paragraph, keep.offset});
        }
    }
    gCaretAtStart = false;
    for (const chwp::render::Node& page : gDoc.pages) {
        writer.beginObject();
        writer.field("width", page.width);
        writer.field("height", page.height);
        writer.field("marginLeft", page.marginLeft);
        writer.field("marginRight", page.marginRight);
        writer.field("marginTop", page.marginTop);
        writer.field("marginBottom", page.marginBottom);
        writer.endObject();
    }
    writer.endArray();
    writer.field("count", static_cast<int>(gDoc.pages.size()));
    writer.field("counting", !gDoc.layoutDone());
    writer.field("lineSeg", lineSeg);
    writer.endObject();
    mountCameraFromPages();
    return writer.take();
}

void mountCameraFromPages() {
    std::vector<chwp::render::PageSpec> specs;
    specs.reserve(gDoc.pages.size());
    for (int i = 0; i < static_cast<int>(gDoc.pages.size()); ++i) {
        const chwp::render::Node& page = gDoc.pages[static_cast<std::size_t>(i)];
        chwp::render::PageSpec spec;
        spec.index = i;
        spec.widthHu = chwp::render::viewWidthOf(page);
        spec.heightHu = page.height;
        spec.marginLeftHu = page.marginLeft;
        spec.marginRightHu = page.marginRight;
        spec.marginTopHu = page.marginTop;
        spec.marginBottomHu = page.marginBottom;
        specs.push_back(spec);
    }
    gCamera.setPages(specs, gLastPageGap);
    gViewportDirty = true;
}


int ensurePageSpans(int page) {
    if (page < 0 || page >= static_cast<int>(gDoc.pages.size())) return 0;
    // 코어의 ensurePageSpans 가 쪽 채우기 잠금을 잡는다. 따로 잠그면 일꾼과 잠그는 차례가 엇갈려 묶일 수 있다.
    return chwp::render::ensurePageSpans(gDoc.doc, gDoc.pages[static_cast<std::size_t>(page)]);
}

StyleReport cursorStyleNow() {
    StyleReport out;
    out.style = chwp::render::cursorStyle(gDoc.doc, gDoc.pages);
    out.status = chwp::render::caretStatus(gDoc.doc, gDoc.pages);
    return out;
}

StyleReport styleToSend(StyleReport report) {
    if (!gSentStyle) {
        report.styleChanged = true;
        report.statusChanged = true;
        gSentStyle = report;
        return report;
    }
    report.styleChanged = report.style.active && !(report.style == gSentStyle->style);
    report.statusChanged = report.status.active && !(report.status == gSentStyle->status);
    if (report.styleChanged) gSentStyle->style = report.style;
    if (report.statusChanged) gSentStyle->status = report.status;
    return report;
}

// 글자 모양·문단 모양 대화 상자를 열 때 채울 값. 이름은 editStyle 과 같다.
render::FormatFields formatFieldsNow() { return chwp::render::formatFieldsAtCursor(gDoc.doc, gDoc.pages); }

// 편집 용지 대화 상자를 열 때 채울 값. 이름과 단위(HWPUNIT)는 editPageSetup 과 같다.
std::vector<std::string> pickableFonts() {
    std::vector<std::string> out = chwp::layout::pickableFonts();
    std::unordered_set<std::string> seen(out.begin(), out.end());
    for (const auto& faces : gDoc.doc.header.fonts) {
        for (const chwp::model::Font& font : faces) {
            if (font.face.empty() || seen.count(font.face)) continue;
            if (!chwp::layout::fontLicenseSwapTarget(font.face).empty()) continue;
            seen.insert(font.face);
            out.push_back(font.face);
        }
    }
    return out;
}

std::vector<std::string> styleNames() {
    std::vector<std::string> out;
    for (const chwp::model::Style& style : gDoc.doc.header.styles) {
        if (!style.name.empty()) out.push_back(style.name);
    }
    return out;
}

render::FormatFields pageSetupFieldsNow() { return chwp::render::pageSetupAtCursor(gDoc.doc); }

CursorReport cursorState() {
    CursorReport out;
    out.cursor = chwp::render::pointerCursor();
    const chwp::model::CursorTarget target = chwp::render::resolvePointerCursor(gDoc.doc);
    out.valid = target.valid();
    out.storyKind = chwp::model::storyKindName(target.slot ? target.slot->kind : chwp::model::StoryKind::None);
    out.sectionIndex = target.slot ? target.slot->sectionIndex : -1;
    out.style = cursorStyleNow();
    return out;
}

// 막힌 글꼴 대체 기억은 편집기·agent 가 함께 쓰는 DocSession 쪽 기억이다.
void setFontSwapPreferences(const std::string& json) { setLicenseSwapPreferences(json); }

std::string fontSwapPreferences() { return licenseSwapPreferences(); }

namespace {

constexpr std::size_t kRecentColors = 10;
constexpr const char* kDefaultPen = "#c42b1c";
std::vector<std::string> gRecentColors;
std::string gPenColor = kDefaultPen;

// #rgb·#rrggbb 를 소문자 #rrggbb 로. 아니면 빈 글이다.
std::string colorHex(std::string_view text) {
    const auto hexDigit = [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; };
    std::string out;
    if (text.size() == 4 && text[0] == '#') {
        out = std::string{'#', text[1], text[1], text[2], text[2], text[3], text[3]};
    } else if (text.size() == 7 && text[0] == '#') {
        out = std::string(text);
    } else {
        return {};
    }
    for (std::size_t i = 1; i < out.size(); ++i) {
        if (!hexDigit(out[i])) return {};
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

ColorMemory colorMemoryNow() {
    ColorMemory out;
    out.recent = gRecentColors;
    out.pen = gPenColor;
    chwp::JsonWriter writer(false);
    writer.beginArray();
    for (const std::string& hex : gRecentColors) writer.value(hex);
    writer.endArray();
    out.saved = writer.take();
    return out;
}

void pushRecentColor(const std::string& hex) {
    gRecentColors.erase(std::remove(gRecentColors.begin(), gRecentColors.end(), hex), gRecentColors.end());
    gRecentColors.insert(gRecentColors.begin(), hex);
    if (gRecentColors.size() > kRecentColors) gRecentColors.resize(kRecentColors);
}

}  // namespace

ColorMemory setRecentColors(const std::string& saved) {
    // 저장해 둔 글에서 #rrggbb 를 차례로 읽는다. 앞이 새 것이다.
    gRecentColors.clear();
    for (std::size_t at = saved.find('#'); at != std::string::npos; at = saved.find('#', at + 1)) {
        const std::string hex = colorHex(std::string_view(saved).substr(at, 7));
        if (hex.empty() || std::find(gRecentColors.begin(), gRecentColors.end(), hex) != gRecentColors.end()) continue;
        gRecentColors.push_back(hex);
        if (gRecentColors.size() == kRecentColors) break;
    }
    gPenColor = gRecentColors.empty() ? kDefaultPen : gRecentColors.front();
    return colorMemoryNow();
}

ColorMemory rememberColor(const std::string& hex) {
    const std::string normal = colorHex(hex);
    if (!normal.empty()) pushRecentColor(normal);
    return colorMemoryNow();
}

std::vector<FontSwapRow> licenseFontSwaps() {
    // 처음 값은 전에 고른 글꼴(아직 고를 수 있으면), 아니면 기본 짝이다. agent 가 묻지 않고 바꾸는 글꼴과 같다.
    std::vector<FontSwapRow> out;
    for (const auto& [from, to] : gDoc.licenseSwapTargets()) out.push_back(FontSwapRow{from, to});
    return out;
}

const std::vector<render::SelectionImage>& fallbackImages() {
    if (!gScreenOn) return gFallbackImages;
    // GPU 화면은 선택을 직접 그린다. 셸에는 캐럿 그림만 준다.
    static std::vector<render::SelectionImage> carets;
    carets.clear();
    for (const render::SelectionImage& image : gFallbackImages) {
        if (image.caret) carets.push_back(image);
    }
    return carets;
}

// kind 는 그대로 코어에 넘긴다. 구간과 invert 는 코어가 정한다.
PointerReport pointerEvent(int kind, int page, int x, int y, int percent) {
    if ((kind == 1 || kind == 2) && page >= 0) {
        ensurePageSpans(page);
    }
    PointerReport out;
    out.view = chwp::render::applyPointer(gDoc.doc, gDoc.pages, kind, page, x, y, percent);
    out.event = chwp::render::pointerEventText();
    const bool dragUnchanged = (kind == 2 && out.event.empty());
    if (kind != 2) syncClickHereGuideNow();
    if (!dragUnchanged && gScreenOn && chwp::render::screenReady()) {
        screenFrame(0, nullptr);
    }
    out.cursor = chwp::render::pointerCursor();
    if (kind != 2) {
        out.styled = true;
        out.style = cursorStyleNow();
        out.text = copyableSelectionText();
    }
    gFallbackImages.clear();
    if (!gScreenOn && !dragUnchanged) gFallbackImages = chwp::render::pointerImages(gDoc.doc, gDoc.pages);
    return out;
}

// 다음 캐럿 보이기는 캐럿 줄을 화면 세로 가운데에 둔다(한글 하이퍼링크 이동). 한 번 쓰면 꺼진다.
bool gCenterCaretOnce = false;

bool ensureCaretVisibleInCamera(const chwp::render::EditResult& res, chwp::render::ViewPlan& plan) {
    const bool center = gCenterCaretOnce;
    gCenterCaretOnce = false;
    if (!res.caret.hit || res.cursor.page < 0 || res.cursor.page >= static_cast<int>(gDoc.pages.size())) return false;
    if (plan.viewW <= 0 || plan.viewH <= 0) return false;
    gInput.stop();
    gScrollHold = 0;

    const chwp::render::ViewPage* pageView = nullptr;
    for (const chwp::render::ViewPage& vp : plan.pages) {
        if (vp.index == res.cursor.page) {
            pageView = &vp;
            break;
        }
    }
    if (!pageView || pageView->w <= 0 || pageView->h <= 0) return false;

    const chwp::render::Node& pageNode = gDoc.pages[static_cast<std::size_t>(res.cursor.page)];
    if (pageNode.width <= 0 || pageNode.height <= 0) return false;

    const double scaleX = static_cast<double>(pageView->w) / static_cast<double>(chwp::render::viewWidthOf(pageNode));
    const double scaleY = static_cast<double>(pageView->h) / static_cast<double>(pageNode.height);
    // 세로쓰기 커서는 누운 막대라 길이가 가로로 간다.
    const double caretAcross = static_cast<double>(std::max(1, res.caret.height));
    int caretLeft = pageView->x + static_cast<int>(std::floor(static_cast<double>(res.caret.x) * scaleX));
    int caretRight = caretLeft + std::max(2, static_cast<int>(std::ceil((res.caret.turned ? caretAcross : 100.0) * scaleX)));
    int caretTop = pageView->y + static_cast<int>(std::floor(static_cast<double>(res.caret.y) * scaleY));
    int caretBottom = caretTop + std::max(2, static_cast<int>(std::ceil((res.caret.turned ? 100.0 : caretAcross) * scaleY)));

    for (const chwp::render::SelectionMark& mark : res.compositionUnderlines) {
        if (mark.page != res.cursor.page || mark.width <= 0) continue;
        const int ulLeft = pageView->x + static_cast<int>(std::floor(static_cast<double>(mark.x) * scaleX));
        const int ulRight =
            pageView->x + static_cast<int>(std::ceil(static_cast<double>(mark.x + mark.width) * scaleX));
        caretLeft = std::min(caretLeft, ulLeft);
        caretRight = std::max(caretRight, ulRight);
    }

    const int marginX = std::min(std::max(24, static_cast<int>(std::round(36.0 * std::max(1.0, plan.dpr)))),
                                 std::max(1, plan.viewW / 4));
    const int marginY = std::min(std::max(24, static_cast<int>(std::round(32.0 * std::max(1.0, plan.dpr)))),
                                 std::max(1, plan.viewH / 4));

    int targetX = plan.scrollX;
    int targetY = plan.scrollY;
    if (caretLeft - marginX < targetX) {
        targetX = caretLeft - marginX;
    } else if (caretRight + marginX > targetX + plan.viewW) {
        targetX = caretRight + marginX - plan.viewW;
    }
    if (center) {
        targetY = (caretTop + caretBottom) / 2 - plan.viewH / 2;
    } else if (caretTop - marginY < targetY) {
        targetY = caretTop - marginY;
    } else if (caretBottom + marginY > targetY + plan.viewH) {
        targetY = caretBottom + marginY - plan.viewH;
    }

    const int maxScrollX = std::max(0, plan.maxScrollX);
    const int maxScrollY = std::max(0, plan.maxScrollY);
    targetX = std::clamp(targetX, 0, maxScrollX);
    targetY = std::clamp(targetY, 0, maxScrollY);

    if (targetX == plan.scrollX && targetY == plan.scrollY) return false;
    gCamera.setScroll(targetX, targetY);
    plan = gCamera.plan(!gScreenOn);
    if (gScreenOn) {
        noteCamera(plan.scrollX, plan.scrollY, plan.percent, false, false);
    }
    return true;
}

// 문서를 바꾸지 않은 캐럿 이동을 화면에 보인다.
void revealCaretNow(const chwp::render::EditResult& res) {
    if (!res.handled || !res.caret.hit) return;
    chwp::render::ViewPlan plan = gCamera.plan(!gScreenOn);
    if (!ensureCaretVisibleInCamera(res, plan)) return;
    if (gScreenOn) {
        gFrames.bakeVisibleNow(gDoc.doc, gDoc.pages, plan);
        useFramePlan(plan);
        gLastPlan = std::move(plan);
        publishPlan();
        ensureScreenFrames();
    } else {
        gLastPlan = std::move(plan);
    }
}

void syncEditedDocument(const chwp::render::EditResult& res) {
    drainDone();
        if (res.fullInvalidation) {
            gPageSpriteCaches.clear();
            gPagePng.assign(gDoc.pages.size(), {});
            gPagePixels.assign(gDoc.pages.size(), {});
            gFrames.prepareForFullRelayout();
            chwp::render::dropPageGpuCache(-1);
        } else {
        if (res.pagesMoved) gFrames.dropPageCatalog();
        if (res.pageShiftDelta != 0 && res.pageShiftFrom >= 0) {
            gFrames.shiftPages(res.pageShiftFrom, res.pageShiftDelta);
            auto shiftIndexMap = [&](auto& map) {
                using Map = std::decay_t<decltype(map)>;
                Map shifted;
                for (auto& [page, value] : map) {
                    const int next = page >= res.pageShiftFrom ? page + res.pageShiftDelta : page;
                    if (next >= 0) shifted.emplace(next, std::move(value));
                }
                map = std::move(shifted);
            };
            shiftIndexMap(gPageSpriteCaches);
            auto shiftFlat = [&](auto& rows) {
                if (res.pageShiftDelta > 0) {
                    const int from = std::min(res.pageShiftFrom, static_cast<int>(rows.size()));
                    rows.insert(rows.begin() + from, static_cast<std::size_t>(res.pageShiftDelta), {});
                } else {
                    const int from = std::min(res.pageShiftFrom, static_cast<int>(rows.size()));
                    const int count = std::min(-res.pageShiftDelta, static_cast<int>(rows.size()) - from);
                    if (count > 0) rows.erase(rows.begin() + from, rows.begin() + from + count);
                }
            };
            shiftFlat(gPagePng);
            shiftFlat(gPagePixels);
        }
        if (gPagePng.size() < gDoc.pages.size()) gPagePng.resize(gDoc.pages.size());
        if (gPagePixels.size() < gDoc.pages.size()) gPagePixels.resize(gDoc.pages.size());
        for (const auto& inv : res.invalidations) {
            gPageSpriteCaches.erase(inv.page);
            chwp::render::dropPageGpuCache(inv.page);
            if (inv.page >= 0 && inv.page < static_cast<int>(gPagePng.size())) {
                gPagePng[static_cast<std::size_t>(inv.page)].clear();
                gPagePixels[static_cast<std::size_t>(inv.page)] = {};
            }
        }
        // 쪽이 옮겨 간 편집(구역 조판)은 재활용을 아직 검증하지 않아 예전처럼 지운다.
        gFrames.invalidate(res.invalidations, !res.pagesMoved);
    }
    if (res.fullInvalidation || res.pageListChanged) {
        const auto prevCam = gCamera.state();
        if (!gLastPlan.pages.empty()) {
            std::vector<chwp::render::PageSpec> specs;
            specs.reserve(gDoc.pages.size());
            for (int i = 0; i < static_cast<int>(gDoc.pages.size()); ++i) {
                const chwp::render::Node& p = gDoc.pages[static_cast<std::size_t>(i)];
                chwp::render::PageSpec spec;
                spec.index = i;
                spec.widthHu = chwp::render::viewWidthOf(p);
                spec.heightHu = p.height;
                spec.marginLeftHu = p.marginLeft;
                spec.marginRightHu = p.marginRight;
                spec.marginTopHu = p.marginTop;
                spec.marginBottomHu = p.marginBottom;
                specs.push_back(spec);
            }
            gCamera.setPages(specs, gLastPageGap);
            gCamera.setScroll(prevCam.scrollX, prevCam.scrollY);
        } else if (gLastPlan.page >= 0 && gLastPlan.page < static_cast<int>(gDoc.pages.size())) {
            const chwp::render::Node& p = gDoc.pages[static_cast<std::size_t>(gLastPlan.page)];
            gCamera.setPage(gLastPlan.page, chwp::render::viewWidthOf(p), p.height);
            gCamera.setPageMargins(p.marginLeft, p.marginRight, p.marginTop, p.marginBottom);
            gCamera.setScroll(prevCam.scrollX, prevCam.scrollY);
        }
    }
    chwp::render::ViewPlan plan = gCamera.plan(!gScreenOn);
    ensureCaretVisibleInCamera(res, plan);
    if (gScreenOn) {
        gFrames.bakeVisibleNow(gDoc.doc, gDoc.pages, plan);
        useFramePlan(plan);
        gLastPlan = std::move(plan);
        publishPlan();
        ensureScreenFrames();
    } else {
        gLastPlan = std::move(plan);
    }
}

void bringSelectionIntoView() {
    const std::vector<chwp::render::SelectionMark> marks = chwp::render::selectionMarks();
    if (marks.empty() || marks.front().width <= 0 || marks.front().height <= 0) return;
    const chwp::render::SelectionMark& mark = marks.front();
    if (mark.page < 0 || mark.page >= static_cast<int>(gDoc.pages.size())) return;
    chwp::render::ViewPlan plan = gCamera.plan(!gScreenOn);
    if (plan.viewW <= 0 || plan.viewH <= 0) return;
    const chwp::render::ViewPage* pageView = nullptr;
    for (const chwp::render::ViewPage& vp : plan.pages) {
        if (vp.index == mark.page) {
            pageView = &vp;
            break;
        }
    }
    if (!pageView || pageView->w <= 0 || pageView->h <= 0) return;
    const chwp::render::Node& pageNode = gDoc.pages[static_cast<std::size_t>(mark.page)];
    if (pageNode.width <= 0 || pageNode.height <= 0) return;

    const double scaleX = static_cast<double>(pageView->w) / static_cast<double>(chwp::render::viewWidthOf(pageNode));
    const double scaleY = static_cast<double>(pageView->h) / static_cast<double>(pageNode.height);
    const int left = pageView->x + static_cast<int>(std::floor(static_cast<double>(mark.x) * scaleX));
    const int top = pageView->y + static_cast<int>(std::floor(static_cast<double>(mark.y) * scaleY));
    const int right = pageView->x + static_cast<int>(std::ceil(static_cast<double>(mark.x + mark.width) * scaleX));
    const int bottom = pageView->y + static_cast<int>(std::ceil(static_cast<double>(mark.y + mark.height) * scaleY));
    const int marginX = std::min(std::max(24, static_cast<int>(std::round(36.0 * std::max(1.0, plan.dpr)))),
                                 std::max(1, plan.viewW / 4));
    const int marginY = std::min(std::max(24, static_cast<int>(std::round(32.0 * std::max(1.0, plan.dpr)))),
                                 std::max(1, plan.viewH / 4));
    int targetX = plan.scrollX;
    int targetY = plan.scrollY;
    if (right - left > plan.viewW - marginX * 2) {
        targetX = left - marginX;
    } else if (left - marginX < targetX) {
        targetX = left - marginX;
    } else if (right + marginX > targetX + plan.viewW) {
        targetX = right + marginX - plan.viewW;
    }
    if (bottom - top > plan.viewH - marginY * 2) {
        targetY = top - marginY;
    } else if (top - marginY < targetY) {
        targetY = top - marginY;
    } else if (bottom + marginY > targetY + plan.viewH) {
        targetY = bottom + marginY - plan.viewH;
    }
    targetX = std::clamp(targetX, 0, std::max(0, plan.maxScrollX));
    targetY = std::clamp(targetY, 0, std::max(0, plan.maxScrollY));
    if (targetX == plan.scrollX && targetY == plan.scrollY) return;
    gInput.stop();
    gScrollHold = 0;
    gCamera.setScroll(targetX, targetY);
    if (gScreenOn) {
        const chwp::render::ViewPlan moved = gCamera.plan(!gScreenOn);
        noteCamera(moved.scrollX, moved.scrollY, moved.percent, false, false);
    }
}

void syncFullRelayout(bool bringSelection) {
    chwp::render::invalidateLinePlaces({}, true);
    drainDone();
    gPageSpriteCaches.clear();
    gPagePng.assign(gDoc.pages.size(), {});
    gPagePixels.assign(gDoc.pages.size(), {});
    gFrames.prepareForFullRelayout();
    const auto prevCam = gCamera.state();
    if (!gDoc.pages.empty()) {
        std::vector<chwp::render::PageSpec> specs;
        specs.reserve(gDoc.pages.size());
        for (int i = 0; i < static_cast<int>(gDoc.pages.size()); ++i) {
            const chwp::render::Node& p = gDoc.pages[static_cast<std::size_t>(i)];
            chwp::render::PageSpec spec;
            spec.index = i;
            spec.widthHu = chwp::render::viewWidthOf(p);
            spec.heightHu = p.height;
            spec.marginLeftHu = p.marginLeft;
            spec.marginRightHu = p.marginRight;
            spec.marginTopHu = p.marginTop;
            spec.marginBottomHu = p.marginBottom;
            specs.push_back(spec);
        }
        gCamera.setPages(specs, gLastPageGap);
        gCamera.setScroll(prevCam.scrollX, prevCam.scrollY);
    }
    if (bringSelection) bringSelectionIntoView();
    chwp::render::ViewPlan plan = gCamera.plan(!gScreenOn);
    if (gScreenOn) {
        gFrames.bakeVisibleNow(gDoc.doc, gDoc.pages, plan);
        useFramePlan(plan);
        gLastPlan = std::move(plan);
        publishPlan();
        ensureScreenFrames();
    } else {
        gLastPlan = std::move(plan);
    }
    gViewportDirty = true;
}

// 누름틀 안내문(한글 2024): 블록 없는 캐럿이 손대지 않은 누름틀 안에 있는 동안 감추고 뒤 글을 당긴다. 보기 상태라
// 되돌리기·고친 표시가 없고, 무엇을 감출지는 코어(click_here_commands)가 정한다. 문단을 다시 놓았으면 화면만 맞춘다.
// 일꾼이 멈췄거나 없을 때 캐럿에 맞춘다. 일꾼이 있으면 편집 끝(resumeCrewAfterEdit)이 부른다.
chwp::render::EditResult syncClickHereGuidePaused() {
    if (gDoc.pages.empty()) return {};
    chwp::render::PageFillGuard guard;
    chwp::render::EditResult res = gDoc.syncClickHereGuide();
    if (res.documentChanged) syncEditedDocument(res);
    return res;
}

// 포인터 하나가 끝날 때(쪽 채우기 잠금을 잡지 않은 자리). 바꿨으면 참.
bool syncClickHereGuideNow() {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    const chwp::render::EditResult res = syncClickHereGuidePaused();
    if (wasCrew) resumeCrewAfterEdit(res);
    return res.documentChanged;
}

// 편집·이동·저장 앞에 숨긴 안내문을 되돌린다(일꾼은 부르는 쪽이 멈춰 둔다).
void revealClickHereGuideNow() {
    const chwp::render::EditResult res = gDoc.revealClickHereGuide();
    if (res.documentChanged) syncEditedDocument(res);
}

EditReport makeEditReport(const chwp::render::EditResult& res) {
    // 편집으로 새로 쓰게 된 글꼴(처음 고칠 때의 대체 글꼴, 고른 글꼴, 막힌 글꼴의 대역)은 core 가 정한다.
    // 셸이 받아 넣으면 문서를 다시 놓는다(fontJobLanded).
    if (std::vector<std::string> wants = gDoc.takeFontWants(); !wants.empty()) chwp::layout::requestFontFaces(wants);
    EditReport out;
    out.result = res;
    const auto cam = gCamera.state();
    out.scrollX = cam.scrollX;
    out.scrollY = cam.scrollY;
    out.canUndo = chwp::render::canUndo();
    out.canRedo = chwp::render::canRedo();
    out.docState = gDoc.documentState();
    out.style = cursorStyleNow();
    out.text = copyableSelectionText();
    if (res.documentChanged && res.fullInvalidation) {
        for (const chwp::render::Node& p : gDoc.pages) {
            out.pages.push_back(PageSize{p.width, p.height, p.marginLeft, p.marginRight, p.marginTop, p.marginBottom});
        }
    }
    gFallbackImages.clear();
    if (!gScreenOn) gFallbackImages = chwp::render::pointerImages(gDoc.doc, gDoc.pages);
    return out;
}

EditReport editText(const std::string& text, bool composing) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    revealClickHereGuideNow();
    // 조합 글이 아니면 줄 끝을 문단 나눔으로 넣는다(붙여 넣기). agent 글 넣기와 같은 DocSession 명령이다.
    std::vector<chwp::render::EditResult> steps;
    if (composing) steps.push_back(gDoc.insertText(text, true));
    else steps = gDoc.insertPlainText(text);
    chwp::render::EditResult res;
    int changed = 0;
    for (const chwp::render::EditResult& step : steps) {
        if (step.documentChanged) ++changed;
        if (step.documentChanged || !res.documentChanged) res = step;
    }
    // 편집이 여럿(여러 줄 붙여 넣기)이면 쪽 밀림이 겹친다. 화면·일꾼·쪽 목록을 한 번에 처음부터 맞춘다.
    if (changed > 1) res.fullInvalidation = true;
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    return makeEditReport(res);
}

// 되돌리기·다시 하기. 무엇을 남기고 어떻게 되돌리는지는 코어(undo.cpp)가 정한다.
EditReport editHistory(bool backward) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    revealClickHereGuideNow();
    const chwp::render::EditResult res = backward
                                               ? gDoc.undo()
                                               : gDoc.redo();
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    return makeEditReport(res);
}

namespace {

struct ImeState {
    std::string composed;     // 마지막으로 넣은 조합 중 글
    bool endPending = false;  // 끝이 왔고 아직 확정하지 않았다
    std::string endText;
};
ImeState gIme;
// 입력기가 조합하지 않고 낱자모를 하나씩 넣을 때 코어가 묶는 조합기. 문서의 조합이 끝나면 버린다.
chwp::input::HangulComposer gHangul;

std::string utf8Of(const std::u32string& text) {
    std::string out;
    for (const char32_t cp : text) chwp::render::detail::appendUtf8(out, cp);
    return out;
}

// 낱자모 하나를 조합기에 넣고, 확정된 글은 확정하고 조합 중 글은 조합 중으로 문서에 둔다.
EditReport feedJamo(char32_t jamo) {
    if (!chwp::render::compositionActive()) gHangul.reset();
    const chwp::input::HangulComposer::Step step = gHangul.feed(jamo);
    EditReport out;
    if (!step.done.empty()) {
        editText(utf8Of(step.done), true);
        out = editCommit();
    }
    if (!step.preedit.empty()) out = editText(utf8Of(step.preedit), true);
    return out;
}

}  // namespace

EditReport editIme(const std::string& kind, const std::string& text, bool shift) {
    // 입력기 조합 사건이 오면 낱자모 조합은 끝이다. 문서에 넣은 글은 그 사건 처리가 확정하거나 바꾼다.
    if (kind != "insert" && kind != "delete") gHangul.reset();
    // 입력칸에서 글자 하나가 지워졌다(입력기가 앞 글자를 바꿔 넣을 때). 낱자모를 묶는 중이면 낱자모 하나다.
    if (kind == "delete") return editKey(static_cast<int>(chwp::render::EditKey::Backspace), false, false, false);
    if (kind == "start") {
        // 끝 바로 뒤에 다음 조합이 시작됐다. 끝 글은 버리고 조합 중이던 글로 확정한다.
        EditReport out;
        if (gIme.endPending) {
            gIme.endPending = false;
            out = editCommit();
        }
        gIme.composed.clear();
        return out;
    }
    if (kind == "end") {
        gIme.endPending = true;
        gIme.endText = text;
        return {};
    }
    if (kind == "settle") {
        if (!gIme.endPending) return {};
        gIme.endPending = false;
        const bool same = gIme.endText == gIme.composed;
        gIme.composed.clear();
        return same ? editCommit() : editText(gIme.endText, false);
    }
    if (kind == "update") {
        // 끝 바로 뒤에 온 다음 조합이다. 앞 글자는 끝 글이 아니라 조합 중이던 글로 확정한다.
        if (gIme.endPending) {
            gIme.endPending = false;
            editCommit();
        }
        gIme.composed = text;
        return editText(text, true);
    }
    if (kind == "insert") {
        if (gIme.endPending) {
            gIme.endPending = false;
            editCommit();
        }
        gIme.composed.clear();
        if (text.empty()) return {};
        // 낱자모 한 글자는 코어가 음절로 묶는다(아이패드 외장 키보드처럼 입력기가 조합하지 않는 플랫폼).
        std::size_t at = 0;
        char32_t cp = 0;
        if (chwp::render::detail::nextCodePoint(text, at, cp) && at == text.size() &&
            chwp::input::HangulComposer::isJamo(cp)) {
            return feedJamo(cp);
        }
        if (gHangul.active()) {
            gHangul.reset();
            if (chwp::render::compositionActive()) editCommit();
        }
        return editText(text, false);
    }
    if (kind == "enter") {
        gIme.endPending = false;
        gIme.composed.clear();
        editCommit();
        return editKey(static_cast<int>(chwp::render::EditKey::Enter), shift, false, false);
    }
    return {};
}

EditReport editImeReplace(int removed, const std::string& text) {
    // 지우기와 넣기는 한 번의 입력이다. 사이에 화면을 그리면 아이패드에서 글자 하나에 화면을 두 번 그린다.
    EditReport out;
    ++gHeldFrames;
    for (int i = 0; i < removed; ++i) out = editIme("delete", "", false);
    if (!text.empty()) out = editIme("insert", text, false);
    --gHeldFrames;
    frameAfterEdit();
    return out;
}

namespace {
chwp::input::TextField gField;

// 입력칸 글 바꾸기 하나(뒤에서 removed 글자를 지우고 text 를 넣는다). composeTail 이면 끝 글자를 조합 글로 넣어
// 조합 블록을 보인다. 조합 사건 없이 입력칸 앞 글자를 바꿔 조합하는 입력기(아이패드 한글)는 그 글자가 아직 조합 중이다.
// 입력칸 글과 문서 글이 글자마다 맞아야 하므로 낱자모를 코어가 묶지 않는다.
EditReport editFieldReplace(int removed, const std::string& text, bool composeTail) {
    EditReport out;
    ++gHeldFrames;
    gHangul.reset();
    if (gIme.endPending) {
        gIme.endPending = false;
        editCommit();
    }
    gIme.composed.clear();
    // 조합 글은 입력칸 끝 글자라 지우기가 먼저 지운다.
    for (int i = 0; i < removed; ++i) out = editKey(static_cast<int>(chwp::render::EditKey::Backspace), false, false, false);
    // 조합 글 뒤에 이어 쳤다. 그 글자는 끝났다.
    if (chwp::render::compositionActive()) out = editCommit();
    std::size_t cut = text.size();
    if (composeTail && cut > 0) {
        --cut;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    }
    if (cut > 0) out = editText(text.substr(0, cut), false);
    if (cut < text.size()) out = editText(text.substr(cut), true);
    --gHeldFrames;
    frameAfterEdit();
    return out;
}
}  // namespace

FieldResult editField(const chwp::input::FieldEvent& event) {
    FieldResult out;
    out.field = gField.handle(event);
    for (const chwp::input::FieldEdit& edit : out.field.edits) {
        using Kind = chwp::input::FieldEditKind;
        switch (edit.kind) {
            case Kind::ImeStart: out.report = editIme("start", "", false); break;
            case Kind::ImeUpdate: out.report = editIme("update", edit.text, false); break;
            case Kind::ImeEnd: out.report = editIme("end", edit.text, false); break;
            case Kind::ImeSettle: out.report = editIme("settle", "", false); break;
            case Kind::Enter: out.report = editIme("enter", "", edit.shift); break;
            case Kind::Replace: out.report = editFieldReplace(edit.removed, edit.text, edit.composeTail); break;
            case Kind::Paste: out.report = editText(edit.text, false); break;
        }
        out.edited = true;
    }
    out.field.edits.clear();
    return out;
}

EditReport editUndo() { return editHistory(true); }

// 그림 넣기. 파일 바이트만 받고, 종류·크기·자리는 코어가 정한다.
EditReport editInsertPicture(const Bytes& image) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    const chwp::render::EditResult res =
        gDoc.insertPicture(image);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    return makeEditReport(res);
}

EditReport editInsertBreak(const std::string& kind) {
    chwp::render::BreakKind breakKind = chwp::render::BreakKind::Page;
    if (!chwp::render::breakKindOf(kind, breakKind)) return makeEditReport(chwp::render::EditResult{});
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    const chwp::render::EditResult res = gDoc.insertBreak(breakKind);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = "나누기는 본문에서만 할 수 있습니다.";
    return report;
}

EditReport editPageControl(const std::string& kind, const render::FormatFields& fields) {
    chwp::render::PageControlKind which = chwp::render::PageControlKind::Header;
    if (!chwp::render::pageControlKindOf(kind, which)) return makeEditReport(chwp::render::EditResult{});
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.pageControl(which, fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

EditReport editNoteShape(const render::FormatFields& fields, bool endnote, bool wholeDocument) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.noteShape(fields, endnote, wholeDocument, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

render::FormatFields noteShapeViewNow(bool endnote) { return gDoc.noteShapeView(endnote); }

EditReport editCaption(const std::string& caption) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.caption(caption, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

EditReport editBookmark(const std::string& action, const std::string& name, const std::string& newName) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.bookmark(action, name, &why, newName);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.handled || (!res.documentChanged && action != "goto")) report.error = why;
    return report;
}

EditReport editGoTo(const render::FormatFields& fields) {
    std::string why;
    const chwp::render::EditResult res = gDoc.goTo(fields, &why);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.handled) report.error = why;
    return report;
}

render::FormatFields goToViewNow() { return gDoc.goToView(); }

EditReport editDeleteMarks(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.deleteMarks(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

EditReport editTableText(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.tableText(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

EditReport editCopyShape(const std::string& action, int type) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.copyShape(action, type, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.handled || (!res.documentChanged && action == "paste")) report.error = why;
    return report;
}

EditReport editExchangeNotes(int mode) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.exchangeNotes(mode, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

EditReport editGoToSection(int section) {
    std::string why;
    const chwp::render::EditResult res = gDoc.goToSection(section, &why);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.handled) report.error = why;
    return report;
}

EditReport editGoToPage(int page) {
    std::string why;
    const chwp::render::EditResult res = gDoc.goToPage(page, &why);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.handled) report.error = why;
    return report;
}

render::FormatFields bookmarkViewNow() { return gDoc.bookmarkView(); }

render::FormatFields styleViewNow(int styleId) {
    render::FormatFields out;
    std::string rows;
    for (const chwp::render::StyleRow& row : chwp::render::styleRows(gDoc.doc)) {
        if (!rows.empty()) rows += '\n';
        rows += std::to_string(row.id) + '\t' + row.name + '\t' + row.engName + '\t' + row.type + '\t' +
                std::to_string(row.nextStyleId) + '\t' + std::to_string(row.shortcut);
    }
    out.emplace_back("rows", rows);
    {
        const chwp::render::EditScope scope = gDoc.bind();
        out.emplace_back("caretStyleId", static_cast<double>(chwp::render::cursorStyle(gDoc.doc, gDoc.pages).styleId));
    }
    if (styleId >= 0) {
        for (const auto& [key, value] : chwp::render::styleSummary(gDoc.doc, static_cast<std::uint32_t>(styleId))) {
            // 길이는 pt 로 준다(HWPUNIT 100 = 1pt).
            const double* n = std::get_if<double>(&value);
            if (n && (key == "marginLeft" || key == "indent" || key == "marginRight" || key == "height")) {
                out.emplace_back("info." + key, *n / 100.0);
            } else {
                out.emplace_back("info." + key, value);
            }
        }
    }
    for (const auto& [key, value] : gDoc.styleDraftFields()) out.emplace_back("draft." + key, value);
    out.emplace_back("draft.active", gDoc.styleDraft.active);
    return out;
}

namespace {

// 스타일 가져오기 대화 상자가 연 문서. 그 문서의 머리말만 쓴다.
std::optional<chwp::model::Document> gStyleSource;

}  // namespace

EditReport editToc(const std::string& action, const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    chwp::render::EditResult res;
    if (action == "make") res = gDoc.makeToc(chwp::render::tocOptionsOfFields(fields), &why);
    else if (action == "update") res = gDoc.updateToc(false, &why);
    else if (action == "updateAll") res = gDoc.updateToc(true, &why);
    else why = "모르는 차례 명령입니다.";
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

render::FormatFields openStyleSource(const Bytes& bytes) {
    render::FormatFields out;
    chwp::ParseResult parsed = chwp::parseDocument(bytes);
    if (parsed.document.header.styles.empty()) {
        gStyleSource.reset();
        out.emplace_back("error", std::string("스타일을 읽을 수 없는 파일입니다."));
        return out;
    }
    std::string rows;
    for (const chwp::render::StyleRow& row : chwp::render::styleRows(parsed.document)) {
        if (!rows.empty()) rows += '\n';
        rows += std::to_string(row.id) + '\t' + row.name + '\t' + row.engName + '\t' + row.type + '\t' +
                std::to_string(row.nextStyleId) + '\t' + std::to_string(row.shortcut);
    }
    out.emplace_back("rows", rows);
    gStyleSource = std::move(parsed.document);
    return out;
}

std::optional<Bytes> exportStyles() {
    Bytes bytes;
    std::string error;
    if (!chwp::writeDocument(chwp::render::styleOnlyDocument(gDoc.doc), chwp::model::SourceFormat::Hwpx, bytes, {},
                             &error)) {
        return std::nullopt;
    }
    return bytes;
}

EditReport editStyleCommand(const std::string& action, const render::FormatFields& fields) {
    const auto number = [&](const char* key, double fallback) {
        for (const auto& [k, v] : fields) {
            if (k != key) continue;
            if (const double* n = std::get_if<double>(&v)) return *n;
        }
        return fallback;
    };
    if (action == "begin") {
        gDoc.beginStyleDraft(static_cast<int>(number("editing", -1)));
        return makeEditReport(chwp::render::EditResult{});
    }
    if (action == "set") {
        gDoc.setStyleDraftFields(fields);
        return makeEditReport(chwp::render::EditResult{});
    }
    if (action == "end") {
        gDoc.endStyleDraft();
        return makeEditReport(chwp::render::EditResult{});
    }
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    chwp::render::EditResult res;
    const auto id = static_cast<std::uint32_t>(std::max(0.0, number("styleId", 0)));
    if (action == "commit") {
        res = gDoc.commitStyleDraft(&why);
    } else if (action == "delete") {
        res = gDoc.deleteStyle(id, static_cast<std::uint32_t>(std::max(0.0, number("replacement", 0))), &why);
    } else if (action == "move") {
        res = gDoc.moveStyle(id, number("delta", 0) < 0 ? -1 : 1, &why);
    } else if (action == "fromCaret") {
        res = gDoc.styleFromCaret(id, &why);
    } else if (action == "clearChar") {
        res = gDoc.clearCharStyle(&why);
    } else if (action == "import") {
        std::vector<std::string> names;
        for (const auto& [k, v] : fields) {
            const std::string* text = std::get_if<std::string>(&v);
            if (k != "names" || !text) continue;
            std::size_t at = 0;
            while (at <= text->size()) {
                const std::size_t end = std::min(text->find('\n', at), text->size());
                if (end > at) names.push_back(text->substr(at, end - at));
                at = end + 1;
            }
        }
        if (gStyleSource) res = gDoc.importStyles(*gStyleSource, names, &why);
        else why = "스타일을 가져올 파일을 먼저 고르십시오.";
    } else {
        why = "모르는 스타일 명령입니다.";
    }
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged && !res.handled) report.error = why;
    return report;
}

EditReport editTableProperties(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.tableProperties(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

render::FormatFields tablePropertiesViewNow() { return gDoc.tablePropertiesView(); }

EditReport editGlyphGroup(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.glyphGroup(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

render::FormatFields glyphGroupViewNow() { return gDoc.glyphGroupView(); }

EditReport editConvertText(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.convertText(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

EditReport editSortParagraphs(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.sortParagraphs(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

render::FormatFields sortViewNow(const render::FormatFields& fields) { return gDoc.sortView(fields); }

render::FormatFields tableFormulaViewNow() { return gDoc.tableFormulaView(); }

EditReport editSectionSettings(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.sectionSettings(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

render::FormatFields sectionSettingsViewNow() { return gDoc.sectionSettingsView(); }

EditReport editColumns(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.columns(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

render::FormatFields columnViewNow(const render::FormatFields& asked) { return gDoc.columnView(asked); }

namespace {
// 기준 위치를 바꿀 때 시작점을 다시 잴 쪽. 화면 가운데 쪽이다.
std::optional<chwp::render::PaperBox> guidePaperBox() {
    if (gDoc.pages.empty()) return std::nullopt;
    const chwp::render::ViewportState camera = gCamera.state();
    int page = currentCameraPage(camera.scrollX, camera.scrollY, camera.percent);
    if (page < 0 || page >= static_cast<int>(gDoc.pages.size())) page = 0;
    return chwp::render::paperBoxOf(gDoc.pages[static_cast<std::size_t>(page)]);
}
}  // namespace

namespace {
// 보기 표시(문단 부호·조판 부호·투명 선·메모)를 바꾼 뒤. 조판 부호 이름표는 그 뒤 글자를 밀고, 메모를 끄고 켜면
// 쪽 폭이 바뀐다. 쪽 그림과 줄 자리 기록을 처음부터 다시 그리고, 커서·블록은 같은 글자 자리에 다시 놓는다.
void redrawForViewMarks(const chwp::render::ViewMarks& before) {
    if (gDoc.pages.empty()) return;
    // 부호 글자(↵ ↓ ⇥ ∨)는 함초롬바탕으로 그린다. 아직 받지 않았으면 받는다(들어오면 글꼴 도착으로 다시 그린다).
    if (chwp::render::viewMarks().paragraphMarks()) chwp::layout::requestFontFaces({"함초롬바탕"});
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    gDoc.viewMarksChanged(before);
    // 칸 그림은 노드 내용이 같으면 다시 쓴다(재활용). 노드는 그대로이고 그리는 것만 바뀌었으니 칸과 쪽 텍스처를 다 버린다.
    gFrames.clear();
    chwp::render::dropPageGpuCache(-1);
    syncFullRelayout(false);
    const chwp::render::PointerCursor cursor = chwp::render::pointerCursor();
    if (cursor.active && cursor.story > 0) {
        const chwp::model::CursorPos focus{cursor.story, cursor.hasSelection ? cursor.focusParagraph : cursor.paragraph,
                                           cursor.hasSelection ? cursor.focusOffset : cursor.offset};
        const chwp::model::CursorPos anchor =
            cursor.hasSelection ? chwp::model::CursorPos{cursor.story, cursor.anchorParagraph, cursor.anchorOffset} : focus;
        chwp::render::setPointerCursor(gDoc.doc, gDoc.pages, anchor, focus);
    }
    if (wasCrew) {
        chwp::render::EditResult resumed;
        resumed.fullInvalidation = true;
        resumeCrewAfterEdit(resumed);
    }
    gGuidesDirty = true;
    frameAfterEdit();
}
}  // namespace

render::FormatFields viewGuidesNow(const render::FormatFields& asked) {
    const std::optional<chwp::render::PaperBox> box = guidePaperBox();
    return chwp::render::viewGuideFields(
        chwp::render::patchViewGuides(chwp::render::viewGuides(), asked, box ? &*box : nullptr));
}

render::FormatFields applyViewGuides(const render::FormatFields& patch) {
    const std::optional<chwp::render::PaperBox> box = guidePaperBox();
    const chwp::render::ViewGuides next =
        chwp::render::patchViewGuides(chwp::render::viewGuides(), patch, box ? &*box : nullptr);
    const std::uint32_t gridBefore = chwp::render::viewGuidesSeq();
    const chwp::render::ViewMarks marksBefore = chwp::render::viewMarks();
    if (chwp::render::setViewGuides(next)) {
        if (!(next.marks == marksBefore)) {
            redrawForViewMarks(marksBefore);
            return chwp::render::viewGuideFields(chwp::render::viewGuides());
        }
        // 글 뒤 격자는 쪽 텍스처가 품는다. 격자 모양이 바뀌면 칸 조각을 고르기 전에 텍스처를 버려 다시 굽게 한다.
        if (chwp::render::viewGuidesSeq() != gridBefore) chwp::render::dropPageGpuCache(-1);
        // 문서와 카메라는 그대로라 화면이 이번 프레임을 건너뛰지 않게 한다.
        gGuidesDirty = true;
        frameAfterEdit();
    }
    return chwp::render::viewGuideFields(chwp::render::viewGuides());
}

EditReport editInsertNote(bool endnote) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.insertNote(endnote, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

EditReport editInsertHyperlink(const std::string& text, const std::string& url) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.insertHyperlink(text, url, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

render::FormatFields equationViewNow() {
    // 미리 보기를 그릴 수식 글꼴을 편집기를 열 때 받는다(이미 있으면 받지 않는다).
    std::vector<std::string> wants;
    for (const std::string& face : chwp::layout::equationFontFaces("HancomEQN")) {
        if (!chwp::layout::fontFaceAvailable(face)) wants.push_back(face);
    }
    if (!wants.empty()) chwp::layout::requestFontFaces(wants);
    return gDoc.equationView();
}

EditReport editEquationSubmit(const std::string& script, std::int32_t baseUnit) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    int story = -1;
    int paragraph = -1;
    int item = -1;
    const bool editing = chwp::render::selectedEquationAt(gDoc.doc, story, paragraph, item);
    const chwp::render::EditResult res =
        editing ? gDoc.editEquation(script, baseUnit, &why) : gDoc.insertEquation(script, baseUnit, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

std::string equationPreviewNow(const std::string& script, std::int32_t baseUnit, bool* ready) {
    return chwp::render::equationSvg(script, baseUnit, ready);
}

render::FormatFields pageControlFieldsNow(const std::string& kind) {
    chwp::render::PageControlKind which = chwp::render::PageControlKind::Header;
    if (!chwp::render::pageControlKindOf(kind, which)) return {};
    return gDoc.pageControlFields(which);
}

namespace {
// 마지막 찾기(다시 찾기 Ctrl+L 이 쓴다).
std::string gLastFind;
bool gLastFindCase = false;
}  // namespace

EditReport editFindReplace(const std::string& mode, const std::string& text, const std::string& replacement,
                           bool matchCase) {
    if (!text.empty()) {
        gLastFind = text;
        gLastFindCase = matchCase;
    }
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    chwp::render::EditResult res;
    bool found = false;
    bool wrapped = false;
    int count = 0;
    std::string notice;
    if (mode == "replaceAll") {
        res = gDoc.replaceAll(text, replacement, matchCase, &count);
        notice = count > 0 ? std::to_string(count) + "개를 바꿨습니다." : "찾는 글이 없습니다.";
    } else if (mode == "replace") {
        bool replaced = false;
        res = gDoc.replaceNext(text, replacement, matchCase, &replaced, &found);
        count = replaced ? 1 : 0;
        if (!found) notice = replaced ? "더 찾는 글이 없습니다." : "찾는 글이 없습니다.";
    } else {
        res = gDoc.findNext(text, matchCase, mode == "findBack", &wrapped);
        found = res.handled;
        if (!found) {
            notice = "찾는 글이 없습니다.";
        } else if (wrapped) {
            notice = mode == "findBack" ? "문서 끝부터 다시 찾았습니다." : "문서 처음부터 다시 찾았습니다.";
        }
    }
    // 막힌 글꼴 때문에 바꾸지 못했으면 알림 대신 셸이 글꼴 바꾸기를 묻는다.
    if (res.needsFontSwap) notice.clear();
    if (res.documentChanged) {
        syncEditedDocument(res);
    } else if (res.handled && res.caret.hit) {
        chwp::render::ViewPlan plan = gCamera.plan(!gScreenOn);
        if (ensureCaretVisibleInCamera(res, plan)) {
            if (gScreenOn) {
                gFrames.bakeVisibleNow(gDoc.doc, gDoc.pages, plan);
                useFramePlan(plan);
                gLastPlan = std::move(plan);
                publishPlan();
                ensureScreenFrames();
            } else {
                gLastPlan = std::move(plan);
            }
        }
    }
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    report.notice = notice;
    report.count = count;
    report.found = found;
    return report;
}

namespace {

// 서식 단축키 명령과 그 서식 값(setFormatField 이름). 한글 2024 단축키 표의 같은 명령이다.
const std::vector<std::pair<std::string, render::FormatFields>>& formatShortcuts() {
    using render::FormatValue;
    static const std::vector<std::pair<std::string, render::FormatFields>> table = {
        {"bold", {{"toggle", FormatValue{std::string("bold")}}}},
        {"italic", {{"toggle", FormatValue{std::string("italic")}}}},
        {"underline", {{"toggle", FormatValue{std::string("underline")}}}},
        {"superscript", {{"toggle", FormatValue{std::string("superscript")}}}},
        {"subscript", {{"toggle", FormatValue{std::string("subscript")}}}},
        {"superSub", {{"toggle", FormatValue{std::string("superSub")}}}},
        {"fontSizeUp", {{"fontSizeStep", FormatValue{1.0}}}},
        {"fontSizeDown", {{"fontSizeStep", FormatValue{-1.0}}}},
        {"ratioUp", {{"ratioStep", FormatValue{1.0}}}},
        {"ratioDown", {{"ratioStep", FormatValue{-1.0}}}},
        {"spacingUp", {{"spacingStep", FormatValue{1.0}}}},
        {"spacingDown", {{"spacingStep", FormatValue{-1.0}}}},
        {"normalShape", {{"normalShape", FormatValue{true}}}},
        {"alignJustify", {{"align", FormatValue{std::string("JUSTIFY")}}}},
        {"alignLeft", {{"align", FormatValue{std::string("LEFT")}}}},
        {"alignRight", {{"align", FormatValue{std::string("RIGHT")}}}},
        {"alignCenter", {{"align", FormatValue{std::string("CENTER")}}}},
        {"alignDistribute", {{"align", FormatValue{std::string("DISTRIBUTE")}}}},
        {"marginLeftUp", {{"marginLeftStep", FormatValue{1.0}}}},
        {"marginLeftDown", {{"marginLeftStep", FormatValue{-1.0}}}},
        {"marginRightUp", {{"marginRightStep", FormatValue{1.0}}}},
        {"marginRightDown", {{"marginRightStep", FormatValue{-1.0}}}},
        {"indentUp", {{"indentStep", FormatValue{1.0}}}},
        {"indentDown", {{"indentStep", FormatValue{-1.0}}}},
        {"lineSpacingUp", {{"lineSpacingStep", FormatValue{1.0}}}},
        {"lineSpacingDown", {{"lineSpacingStep", FormatValue{-1.0}}}},
    };
    return table;
}

}  // namespace

render::FormatFields clickHereViewNow() { return gDoc.clickHereView(); }

EditReport editClickHere(const render::FormatFields& fields) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    revealClickHereGuideNow();
    std::string why;
    const chwp::render::EditResult res = gDoc.modifyClickHere(fields, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) resumeCrewAfterEdit(res);
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.handled) report.error = why;
    return report;
}

EditReport editShortcut(const std::string& command) {
    // Hangul StyleShortcut1..10 (Ctrl+1..0, Ctrl+Alt+1..0): the Nth style in the document style list.
    if (command.rfind("styleShortcut", 0) == 0) {
        const int index = std::atoi(command.c_str() + 13) - 1;
        const auto& styles = gDoc.doc.header.styles;
        if (index < 0 || index >= static_cast<int>(styles.size())) return makeEditReport(chwp::render::EditResult{});
        render::FormatPatch patch;
        render::setFormatField(patch, "styleId", static_cast<double>(styles[static_cast<std::size_t>(index)].id));
        return editStyle(patch);
    }
    for (const auto& [name, fields] : formatShortcuts()) {
        if (name != command) continue;
        render::FormatPatch patch;
        for (const auto& [key, value] : fields) render::setFormatField(patch, key, value);
        return editStyle(patch);
    }
    if (command == "clickHereEdit") {
        EditReport report = makeEditReport(chwp::render::EditResult{});
        report.dialog = "clickHere";
        return report;
    }
    // 누름틀 넣기(한글 입력 탭 누름틀, InsertFieldCtrl). 한글 기본 안내문으로 바로 넣는다.
    if (command == "insertClickHere") {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrewForEdit();
        chwp::render::PageFillGuard guard;
        std::string why;
        const chwp::render::EditResult res = gDoc.insertClickHere({}, {}, {}, &why);
        if (res.documentChanged) syncEditedDocument(res);
        if (wasCrew) resumeCrewAfterEdit(res);
        frameAfterEdit();
        EditReport report = makeEditReport(res);
        if (!res.handled) report.error = why;
        return report;
    }
    // 변경 내용 추적(한글 검토 탭). 켜고 끄기는 알림 글로 지금 상태를 알린다.
    if (command == "trackChanges") {
        chwp::render::PageFillGuard guard;
        const chwp::render::EditResult res = gDoc.toggleTrackChanges();
        frameAfterEdit();
        EditReport report = makeEditReport(res);
        report.notice = chwp::render::trackRecording(gDoc.doc) ? "변경 내용 기록을 시작합니다." : "변경 내용 기록을 멈춥니다.";
        return report;
    }
    if (command == "acceptChange" || command == "rejectChange" || command == "acceptAllChanges" ||
        command == "rejectAllChanges" || command == "acceptAndMove" || command == "rejectAndMove") {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrewForEdit();
        chwp::render::PageFillGuard guard;
        std::string why;
        const bool accept = command.rfind("accept", 0) == 0;
        const char* scope = command.find("All") != std::string::npos ? "all"
                            : command.find("AndMove") != std::string::npos ? "move"
                                                                           : "caret";
        const chwp::render::EditResult res = gDoc.resolveChanges(accept, scope, &why);
        if (res.documentChanged) syncEditedDocument(res);
        else revealCaretNow(res);
        if (wasCrew) resumeCrewAfterEdit(res);
        frameAfterEdit();
        EditReport report = makeEditReport(res);
        if (!res.handled) report.error = why;
        return report;
    }
    if (command == "nextChange" || command == "prevChange") {
        chwp::render::PageFillGuard guard;
        const chwp::render::EditResult res = gDoc.moveToChange(command == "nextChange");
        revealCaretNow(res);
        frameAfterEdit();
        EditReport report = makeEditReport(res);
        if (!res.handled) report.error = command == "nextChange" ? "다음 바뀐 내용이 없습니다." : "이전 바뀐 내용이 없습니다.";
        return report;
    }
    if (command == "linkBack" || command == "linkForward") {
        // 한글 HyperlinkPrev·HyperlinkNext: 링크 이력의 앞·뒤. 대상 줄을 화면 가운데에 둔다.
        chwp::render::PageFillGuard guard;
        const chwp::render::EditResult res = gDoc.linkHistory(command == "linkForward");
        gCenterCaretOnce = true;
        revealCaretNow(res);
        gCenterCaretOnce = false;
        frameAfterEdit();
        EditReport report = makeEditReport(res);
        if (!res.handled) report.error = command == "linkBack" ? "돌아갈 하이퍼링크 자리가 없습니다." : "다시 갈 하이퍼링크 자리가 없습니다.";
        return report;
    }
    if (command == "updateToc") return editToc("update", {});
    if (command == "updateAllToc") return editToc("updateAll", {});
    if (command == "breakPage") return editInsertBreak("page");
    if (command == "leaveNote") {
        // 주석 탭 닫기: 주 바로 뒤 본문으로 간다. 문서는 그대로다.
        const chwp::render::EditResult res = gDoc.leaveNote();
        frameAfterEdit();
        return makeEditReport(res);
    }
    if (command == "breakColumn") return editInsertBreak("column");
    if (command == "footnote" || command == "endnote") return editInsertNote(command == "endnote");
    if (command == "captionToggle") return editCaption("TOGGLE");
    if (command == "copyShape") {
        // 모양 복사(Alt+C): 블록이 있고 기억한 모양이 있으면 붙이고, 아니면 모양 복사 대화 상자를 연다.
        const chwp::render::PointerCursor cur = chwp::render::pointerCursor();
        if (cur.hasSelection && gDoc.copiedShapeReady()) return editCopyShape("paste", 0);
        EditReport report = makeEditReport(chwp::render::EditResult{});
        report.dialog = "copyShape";
        return report;
    }
    if (command == "modify") {
        // 고치기(Ctrl+N,K): 고른 수식이면 수식 편집기, 캐럿 뒤(또는 고른 한 글자)의 덧말·글자 겹치기면 그 편집 대화 상자다.
        // 캐럿이 누름틀 안이면 누름틀 고치기다.
        EditReport report = makeEditReport(chwp::render::EditResult{});
        int story = -1;
        int paragraph = -1;
        int item = -1;
        if (chwp::render::selectedEquationAt(gDoc.doc, story, paragraph, item)) {
            report.dialog = "equation";
            return report;
        }
        for (const auto& [name, value] : gDoc.clickHereView()) {
            if (name == "found" && std::get_if<bool>(&value) && *std::get_if<bool>(&value)) {
                report.dialog = "clickHere";
                return report;
            }
        }
        for (const auto& [name, value] : gDoc.glyphGroupView()) {
            if (name != "editKind") continue;
            if (const std::string* kind = std::get_if<std::string>(&value); kind && !kind->empty()) {
                report.dialog = *kind + "Edit";
            }
        }
        return report;
    }
    if (command == "breakColumnDef") return editInsertBreak("columnDef");
    if (command == "breakSection") return editInsertBreak("section");
    if (command == "repeatFind") {
        if (gLastFind.empty()) {
            EditReport report = makeEditReport(chwp::render::EditResult{});
            report.dialog = "find";
            return report;
        }
        return editFindReplace("find", gLastFind, "", gLastFindCase);
    }
    const chwp::render::CaretStatus status = chwp::render::caretStatus(gDoc.doc, gDoc.pages);
    // 한글 TableAppendRow: 칸 블록이 있으면 하지 않고, 없으면 캐럿 줄 아래에 줄 하나.
    if (command == "appendRow") {
        if (!status.table || status.cellBlock) return makeEditReport(chwp::render::EditResult{});
        return editTable("insertRows", true, 1, 1, 1);
    }
    // 한글 TableSubtractRow: 캐럿이 칸 맨 앞이면 그 줄을 지운다.
    if (command == "subtractRow") {
        if (!status.cellStart) return makeEditReport(chwp::render::EditResult{});
        return editTable("deleteRows", true, 1, 1, 1);
    }
    if (command == "mergeCells" || command == "equalWidth" || command == "equalHeight") {
        if (!status.cellBlock) return makeEditReport(chwp::render::EditResult{});
        return editTable(command, true, 1, 1, 1);
    }
    return makeEditReport(chwp::render::EditResult{});
}

EditReport editTable(const std::string& kind, bool after, int count, int rows, int cols, const std::string& border,
                     const std::string& color, const std::string& lineType, int lineWidth, const std::string& lineColor) {
    chwp::render::TableEditCommand cmd;
    if (!chwp::render::tableEditKindOf(kind, cmd.kind)) return makeEditReport(chwp::render::EditResult{});
    cmd.after = after;
    cmd.count = count;
    cmd.rows = rows;
    cmd.cols = cols;
    cmd.border = border;
    cmd.color = color;
    if (!lineType.empty()) cmd.lineType = lineType;
    cmd.lineWidth = lineWidth;
    if (!lineColor.empty()) cmd.lineColor = lineColor;
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.editTableAtCursor(cmd, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

EditReport editTableFields(const std::string& kind, const render::FormatFields& fields) {
    const auto text = [&](const char* key) {
        for (const auto& [name, value] : fields) {
            if (name == key) {
                if (const std::string* s = std::get_if<std::string>(&value)) return *s;
            }
        }
        return std::string();
    };
    const auto number = [&](const char* key, double fallback) {
        for (const auto& [name, value] : fields) {
            if (name != key) continue;
            if (const double* d = std::get_if<double>(&value)) return *d;
            if (const bool* b = std::get_if<bool>(&value)) return *b ? 1.0 : 0.0;
        }
        return fallback;
    };
    chwp::render::TableEditCommand cmd;
    if (!chwp::render::tableEditKindOf(kind, cmd.kind)) return makeEditReport(chwp::render::EditResult{});
    cmd.after = number("after", 1) != 0;
    cmd.count = static_cast<int>(number("count", 1));
    cmd.rows = static_cast<int>(number("rows", 1));
    cmd.cols = static_cast<int>(number("cols", 1));
    cmd.border = text("border");
    cmd.color = text("color");
    if (!text("lineType").empty()) cmd.lineType = text("lineType");
    cmd.lineWidth = static_cast<std::int32_t>(number("lineWidth", 0));
    if (!text("lineColor").empty()) cmd.lineColor = text("lineColor");
    cmd.vertAlign = text("vertAlign");
    cmd.horzAlign = text("horzAlign");
    cmd.direction = static_cast<int>(number("direction", 0));
    cmd.margin = text("margin");
    cmd.preset = text("preset");
    cmd.wrap = text("wrap");
    cmd.caption = text("caption");
    cmd.comma = text("comma");
    cmd.flip = static_cast<int>(number("flip", 0));
    cmd.swapMargin = number("swapMargin", 0) != 0;
    // 정렬 기준 셋: keyNColumn·keyNFormat·keyNDesc(N 은 1~3, 없음은 칸 0). 한글 KeyOption 처럼 차례대로 다 넘기고,
    // 기준과 칸을 짝짓는 규칙은 코어(sortTableRows)가 정한다.
    for (int n = 1; n <= 3; ++n) {
        const std::string prefix = "key" + std::to_string(n);
        const int column = static_cast<int>(number((prefix + "Column").c_str(), n == 1 ? 1 : 0));
        cmd.sortKeys.push_back({column, static_cast<int>(number((prefix + "Format").c_str(), 0)),
                                number((prefix + "Desc").c_str(), 0) != 0});
    }
    cmd.sortFromRear = number("fromRear", 0) != 0;
    cmd.sortExtendYear = number("extendYear", 1) != 0;
    cmd.sortYearBase = static_cast<int>(number("yearBase", 1930));
    cmd.sortOldHangul = number("oldHangul", 0) != 0;
    cmd.sortFinalFirst = number("finalFirst", 0) != 0;
    cmd.formulaMode = text("formulaMode");
    cmd.formulaFunc = static_cast<int>(number("formulaFunc", 0));
    cmd.formulaText = text("formula");
    cmd.formulaFormat = text("format");
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    std::string why;
    const chwp::render::EditResult res = gDoc.editTableAtCursor(cmd, &why);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    EditReport report = makeEditReport(res);
    if (!res.documentChanged) report.error = why;
    return report;
}

// 표 넣기. 줄·칸 개수만 받고 표 모양은 코어가 정한다.
EditReport editInsertTable(int rows, int cols) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    const chwp::render::EditResult res =
        gDoc.insertTable(rows, cols);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    return makeEditReport(res);
}

EditReport editRedo() { return editHistory(false); }

EditReport editDeleteSelection() {
    if (!chwp::render::pointerCursor().hasSelection) return makeEditReport({});
    return editKey(static_cast<int>(chwp::render::EditKey::Backspace), false, false, false);
}

EditReport editCommit() {
    chwp::render::PageFillGuard guard;
    const chwp::render::EditResult res = gDoc.commitComposition();
    frameAfterEdit();
    return makeEditReport(res);
}

// 지금 화면에 보이는 쪽마다의 세로 범위(쪽 HU).
std::vector<chwp::render::ViewBand> visibleBands() {
    std::vector<chwp::render::ViewBand> out;
    const chwp::render::ViewPlan plan = gCamera.plan(false);
    for (const chwp::render::ViewPage& vp : plan.pages) {
        if (vp.index < 0 || vp.index >= static_cast<int>(gDoc.pages.size()) || vp.h <= 0) continue;
        const chwp::render::Node& page = gDoc.pages[static_cast<std::size_t>(vp.index)];
        if (page.height <= 0) continue;
        const double perPixel = static_cast<double>(page.height) / static_cast<double>(vp.h);
        const double top = (static_cast<double>(plan.scrollY) - vp.y) * perPixel;
        const double bottom = (static_cast<double>(plan.scrollY + plan.viewH) - vp.y) * perPixel;
        if (bottom <= 0 || top >= page.height) continue;
        out.push_back(chwp::render::ViewBand{vp.index, static_cast<std::int32_t>(std::ceil(top)),
                                            static_cast<std::int32_t>(std::floor(bottom))});
    }
    return out;
}

EditReport editKey(int key, bool shift, bool ctrl, bool alt) {
    // 낱자모를 묶는 중이면 지우기는 마지막 낱자모만 뺀다. 다른 키는 조합을 끝낸다(편집 명령이 확정한다).
    if (gHangul.active() && chwp::render::compositionActive() &&
        key == static_cast<int>(chwp::render::EditKey::Backspace) && !shift && !ctrl && !alt) {
        const std::u32string left = gHangul.backspace();
        return editText(utf8Of(left), true);
    }
    gHangul.reset();
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    revealClickHereGuideNow();
    // Ctrl+Home·End 는 한글 MoveViewBegin·End: 화면에 온전히 보이는 첫·마지막 줄. 보이는 범위는 카메라에서 셈한다.
    const bool viewMove = ctrl && !alt &&
                          (key == static_cast<int>(chwp::render::EditKey::Home) ||
                           key == static_cast<int>(chwp::render::EditKey::End));
    const chwp::render::EditResult res =
        viewMove ? gDoc.moveToView(visibleBands(), key == static_cast<int>(chwp::render::EditKey::End), shift)
                 : gDoc.keyDown(static_cast<chwp::render::EditKey>(key), shift, ctrl, alt);
    if (res.documentChanged) {
        syncEditedDocument(res);
    } else if (res.handled && res.caret.hit) {
        chwp::render::ViewPlan plan = gCamera.plan(!gScreenOn);
        if (ensureCaretVisibleInCamera(res, plan)) {
            if (gScreenOn) {
                gFrames.bakeVisibleNow(gDoc.doc, gDoc.pages, plan);
                useFramePlan(plan);
                gLastPlan = std::move(plan);
                publishPlan();
                ensureScreenFrames();
            } else {
                gLastPlan = std::move(plan);
            }
        }
    }
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    return makeEditReport(res);
}

// chosen 은 셸에서 고른 {막힌 글꼴, 바꿀 글꼴}이다. 고를 수 있는지는 코어가 본다.
EditReport editSwapFonts(const std::vector<std::pair<std::string, std::string>>& chosen) {
    // 고른 글꼴 기억과 고르지 않은 글꼴의 바꿀 글꼴은 DocSession 이 정한다(agent 와 같다).
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    const chwp::render::EditResult res =
        gDoc.swapLicenseFonts(chosen);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    return makeEditReport(res);
}

EditReport editStyle(const render::FormatPatch& patch) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    const chwp::render::EditResult res =
        gDoc.applyFormat(patch);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    return makeEditReport(res);
}

EditReport editPageSetup(const render::FormatFields& fields, bool wholeDocument) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::PageFillGuard guard;
    const chwp::render::EditResult res =
        gDoc.applyPageSetup(fields, wholeDocument);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    frameAfterEdit();
    return makeEditReport(res);
}

namespace {

chwp::render::FormatViewKind dialogKind(int kind) {
    if (kind == 2) return chwp::render::FormatViewKind::Page;
    if (kind == 3) return chwp::render::FormatViewKind::Table;
    if (kind == 4) return chwp::render::FormatViewKind::PageBorder;
    return kind == 1 ? chwp::render::FormatViewKind::Para : chwp::render::FormatViewKind::Char;
}

chwp::render::PageBitmap gDialogPreview;

}  // namespace

render::FormatView formatDialogView(int kind, const render::FormatFields& patch) {
    if (kind == 5 || kind == 6) return gDoc.styleDraftFormatView(kind == 6, patch);
    return chwp::render::formatView(gDoc.doc, gDoc.pages, dialogKind(kind), patch);
}

EditReport editFormatDialog(int kind, const render::FormatFields& patch, bool wholeDocument) {
    if (kind == 5 || kind == 6) {
        // 스타일 초안 모양만 고친다. 문서는 스타일 대화 상자에서 맺을 때 바뀐다.
        gDoc.applyStyleDraftFormat(kind == 6, patch);
        return makeEditReport(chwp::render::EditResult{});
    }
    const chwp::render::FormatViewKind which = dialogKind(kind);
    const chwp::render::FormatFields resolved = chwp::render::resolveFormatView(gDoc.doc, gDoc.pages, which, patch);
    if (which == chwp::render::FormatViewKind::Page) return editPageSetup(resolved, wholeDocument);
    if (which == chwp::render::FormatViewKind::PageBorder) {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrewForEdit();
        chwp::render::PageFillGuard guard;
        std::string why;
        const chwp::render::EditResult res = gDoc.pageBorder(resolved, wholeDocument, &why);
        if (res.documentChanged) syncEditedDocument(res);
        if (wasCrew) resumeCrewAfterEdit(res);
        frameAfterEdit();
        EditReport report = makeEditReport(res);
        if (!res.documentChanged) report.error = why;
        return report;
    }
    chwp::render::FormatPatch style;
    for (const auto& [key, value] : resolved) chwp::render::setFormatField(style, key, value);
    return editStyle(style);
}

const render::PageBitmap* formatDialogPreview(int kind, const render::FormatFields& patch, int width, int height) {
    if (kind == 5 || kind == 6) {
        gDialogPreview = gDoc.styleDraftPreview(kind == 6, patch, width, height);
        return gDialogPreview.width > 0 ? &gDialogPreview : nullptr;
    }
    gDialogPreview = chwp::render::renderFormatPreview(gDoc.doc, gDoc.pages, dialogKind(kind), patch, width, height);
    return gDialogPreview.width > 0 ? &gDialogPreview : nullptr;
}

bool presentView(int viewW, int viewH, int scrollX, int scrollY, int layoutZoom, bool dragging);

// 워커 rAF 한 번에 최종 화면 한 장. 플링은 여기서 진행한다.
// 스크롤만 바뀌면 있는 칸을 밀어 그린다. 칸 목록은 반 화면이 넘어가거나 멈춘 뒤에 고친다.
struct ShownFrame {
    bool ok = false;
    int scrollX = 0;
    int scrollY = 0;
    int percent = 0;
    int viewW = 0;
    int viewH = 0;
    int generation = -1;
    int revision = 0;
    int picture = -1;
    int caretPhase = -2;
    bool masks = false;
    bool pinching = false;
    bool fastMotion = false;
};

struct ShownTile {
    int id = 0;
    int stamp = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct ShownPage {
    int index = -1;
    int revision = 0;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool inView = false;
};

// 일꾼 스레드가 없을 때 화면 스레드가 한 프레임에 쓰는 준비 시간(ms).
// 스크롤 중에는 화면에 들어온 빈 칸만 짧게, 쉴 때는 일꾼 걸음을 길게 이어 간다.
constexpr double kSoloScrollBudgetMs = 4;
constexpr double kSoloIdleBudgetMs = 8;
// 스크롤 중 한 번에 굽는 칸 수. 예산을 넘기지 않게 작게 나눈다.
constexpr int kSoloViewTiles = 2;

bool gScreenLoop = false;
bool gPinchPlanPending = false;
double gFrameClock = 0;
std::atomic<bool> gUserInteracting{false};
bool crewRunning();
void setCrewInteracting(bool interacting);
bool stepSingleThreadForeground();
bool stepSingleThreadIdle();
bool stepSingleThreadView(double budgetMs);
void runSingleThreadIdle(double budgetMs);
ShownFrame gShown;
std::vector<ShownTile> gShownDraw;
std::vector<ShownPage> gShownPages;

void collectShownPages(const chwp::render::ViewPlan& plan, std::vector<ShownPage>& out) {
    out.clear();
    const int width = plan.viewW;
    const int height = plan.viewH;
    if (width <= 0 || height <= 0) return;
    const int prefetchMargin = std::max(height / 2, 320);
    for (const chwp::render::ViewPage& page : plan.pages) {
        if (page.w <= 0 || page.h <= 0) continue;
        const int x = page.x - plan.scrollX;
        const int y = page.y - plan.scrollY;
        const bool inView = !(x >= width || y >= height || x + page.w <= 0 || y + page.h <= 0);
        const bool inPrefetch =
            !inView && prefetchMargin > 0 &&
            !(x >= width + prefetchMargin || y >= height + prefetchMargin ||
              x + page.w <= -prefetchMargin || y + page.h <= -prefetchMargin);
        if (!inView && !inPrefetch) continue;
        ShownPage sp;
        sp.index = page.index;
        sp.revision = gFrames.pageRevision(page.index);
        sp.x = x;
        sp.y = y;
        sp.w = page.w;
        sp.h = page.h;
        sp.inView = inView;
        out.push_back(sp);
    }
}

bool pageNeedsSpans(int index) {
    if (index < 0 || index >= static_cast<int>(gDoc.pages.size())) return false;
    const chwp::render::Node& page = gDoc.pages[static_cast<std::size_t>(index)];
    return page.spansLater || page.contentHash == 0;
}

bool visibleSpansPending(const chwp::render::ViewPlan& plan) {
    if (plan.viewW <= 1 || plan.viewH <= 1) return false;
    for (const chwp::render::ViewPage& page : plan.pages) {
        if (page.w <= 0 || page.h <= 0) continue;
        if (page.x >= plan.scrollX + plan.viewW || page.x + page.w <= plan.scrollX ||
            page.y >= plan.scrollY + plan.viewH || page.y + page.h <= plan.scrollY) {
            continue;
        }
        if (pageNeedsSpans(page.index)) return true;
    }
    return false;
}

bool sameShownDraw(const chwp::render::ViewPlan& plan) {
    if (gPageGpuBakePending) return false;
    if (visibleSpansPending(plan)) return false;
    if (gScreenOn) {
        if (gShownPages.empty()) return false;
        std::vector<ShownPage> current;
        collectShownPages(plan, current);
        if (current.empty() || current.size() != gShownPages.size()) return false;
        for (std::size_t i = 0; i < current.size(); ++i) {
            const ShownPage& a = current[i];
            const ShownPage& b = gShownPages[i];
            if (a.inView && a.revision == 0) return false;
            if (a.index != b.index || a.revision != b.revision || a.x != b.x || a.y != b.y || a.w != b.w ||
                a.h != b.h || a.inView != b.inView) {
                return false;
            }
        }
        return true;
    }
    if (plan.draw.size() != gShownDraw.size()) return false;
    for (std::size_t i = 0; i < plan.draw.size(); ++i) {
        const chwp::render::ViewDraw& draw = plan.draw[i];
        const ShownTile& shown = gShownDraw[i];
        if (draw.id != shown.id || gFrames.stamp(draw.id) != shown.stamp || draw.x != shown.x || draw.y != shown.y ||
            draw.w != shown.w || draw.h != shown.h) {
            return false;
        }
    }
    return true;
}

void rememberShownDraw(const chwp::render::ViewPlan& plan) {
    if (gScreenOn) {
        collectShownPages(plan, gShownPages);
    }
    gShownDraw.resize(plan.draw.size());
    for (std::size_t i = 0; i < plan.draw.size(); ++i) {
        const chwp::render::ViewDraw& draw = plan.draw[i];
        ShownTile& shown = gShownDraw[i];
        shown.id = draw.id;
        shown.stamp = gFrames.stamp(draw.id);
        shown.x = draw.x;
        shown.y = draw.y;
        shown.w = draw.w;
        shown.h = draw.h;
    }
}

int caretPhaseNow() { return chwp::render::caretBlinkPhase(); }

int currentCameraPage(int scrollX, int scrollY, int percent) {
    if (!gLastPlan.pages.empty() && (percent <= 0 || percent == gLastPlan.percent)) {
        const double centerX = static_cast<double>(scrollX) + static_cast<double>(std::max(1, gLastPlan.viewW)) / 2.0;
        const double centerY = static_cast<double>(scrollY) + static_cast<double>(std::max(1, gLastPlan.viewH)) / 2.0;
        int bestPage = gLastPlan.pages.front().index;
        double bestDistSq = std::numeric_limits<double>::infinity();
        for (const chwp::render::ViewPage& p : gLastPlan.pages) {
            const double left = static_cast<double>(p.x);
            const double right = static_cast<double>(p.x + std::max(0, p.w));
            const double top = static_cast<double>(p.y);
            const double bottom = static_cast<double>(p.y + std::max(0, p.h));
            const double dx = centerX < left ? (left - centerX) : (centerX > right ? (centerX - right) : 0.0);
            const double dy = centerY < top ? (top - centerY) : (centerY > bottom ? (centerY - bottom) : 0.0);
            const double distSq = dx * dx + dy * dy;
            if (distSq == 0.0) return p.index;
            if (distSq < bestDistSq) {
                bestDistSq = distSq;
                bestPage = p.index;
            }
        }
        return bestPage;
    }
    const chwp::render::ViewPlan plan = gCamera.plan(false);
    if (plan.page >= 0) return plan.page;
    return gLastPlan.page >= 0 ? gLastPlan.page : 0;
}

void resizeScreen(int width, int height) {
    if (hostSlot().resizeSurface) hostSlot().resizeSurface(width, height);
}

void rememberShellCamera(int percent) {
    const chwp::render::ViewportState cam = gCamera.state();
    gShellNoteW = gLastPlan.viewW;
    gShellNoteH = gLastPlan.viewH;
    gShellNoteMaxX = cam.maxScrollX;
    gShellNoteMaxY = cam.maxScrollY;
    gShellNotePercent = percent;
}

bool shellCameraDue(int percent) {
    const chwp::render::ViewportState cam = gCamera.state();
    return gLastPlan.viewW != gShellNoteW || gLastPlan.viewH != gShellNoteH || cam.maxScrollX != gShellNoteMaxX ||
           cam.maxScrollY != gShellNoteMaxY || percent != gShellNotePercent;
}

// 입력이 스크롤을 쥐고 있는가, 움직임이 멈췄는가. 셸은 이 둘로 제 스크롤 막대를 덮어쓸지 정한다.
bool inputOwnsScroll() {
    return gInput.touchLive() || gInput.draggingBar() || gInput.wheeling() || gInput.panning() ||
           gInput.pinchActive() || gInput.coasting();
}

bool inputSettled() { return !gInput.pinchActive() && !gInput.coasting() && !gInput.panning() && !gInput.wheeling(); }

void noteCamera(int scrollX, int scrollY, int percent, bool coasting, bool pinching) {
    gCameraNoteDue = false;
    const chwp::render::ViewportState cam = gCamera.state();
    CameraNote note;
    note.scrollX = scrollX;
    note.scrollY = scrollY;
    note.percent = percent;
    note.coasting = coasting;
    note.pinching = pinching;
    note.ownsScroll = inputOwnsScroll();
    note.settled = inputSettled();
    note.page = currentCameraPage(scrollX, scrollY, percent);
    note.pageCount = static_cast<int>(gDoc.pages.size());
    note.pageCounting = !gDoc.layoutDone();
    note.fitPage = gCamera.isFitPage();
    note.fitWidth = gCamera.isFitWidth();
    note.viewW = gLastPlan.viewW;
    note.viewH = gLastPlan.viewH;
    note.maxScrollX = cam.maxScrollX;
    note.maxScrollY = cam.maxScrollY;
    note.dpr = gLastPlan.dpr > 0 ? gLastPlan.dpr : 1;
    if (hostSlot().cameraChanged) hostSlot().cameraChanged(note);
    rememberShellCamera(percent);
}

void noteCameraIf(bool force, int scrollX, int scrollY, int percent, bool coasting, bool pinching) {
    if (!force && !shellCameraDue(percent)) return;
    noteCamera(scrollX, scrollY, percent, coasting, pinching);
}


void frameAfterEdit() {
    // 일꾼이 없으면 여기서 누름틀 안내문을 맞춘다(일꾼이 있으면 편집 끝에서 이미 맞췄다).
    if (gHeldFrames == 0 && !crewRunning()) syncClickHereGuidePaused();
    if (gHeldFrames == 0 && gScreenOn && chwp::render::screenReady()) screenFrame(0, nullptr);
}

// 칸마다 모은 화면 글자를 지웠다(글꼴이 들어와 글자를 다시 고를 때. 문서를 다시 놓지 않아도 지운다).
// 들고 있는 칸과 일꾼이 다 구운 칸은 글자 없는 그림만 남으니 모두 버리고 다시 굽는다. 화면이 아직 없어도
// 일꾼이 미리 구운 칸이 있으니 같은 판단을 한다.
void rebakeFramesWithoutGlyphs() {
    const int epoch = chwp::render::screenGlyphEpoch();
    if (epoch == gScreenGlyphSeen) return;
    gScreenGlyphSeen = epoch;
    if (gDoc.pages.empty()) return;
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    chwp::render::EditResult redraw;
    redraw.fullInvalidation = true;
    syncEditedDocument(redraw);
    if (wasCrew) resumeCrewAfterEdit(redraw);
}

bool screenFrame(double time, void*) {
    rebakeFramesWithoutGlyphs();
    if (!gScreenOn || !chwp::render::screenReady()) {
        // 화면이 없으면 매끄럽게 지킬 프레임이 없다. 입력이 켜 둔 '손이 움직이는 중'을 풀어 일꾼이 쉬지 않게 한다.
        setCrewInteracting(false);
        runSoloCrew(kSoloIdleBudgetMs);
        drainDone();
        return true;
    }
    if (time <= 0) time = frameNow();
    // 바탕쪽 편집 화면을 켜고 끄면 쪽에 그리는 것이 바뀐다(본문을 감춘다). 쪽 그림을 모두 다시 그린다.
    if (const int epoch = chwp::render::masterViewEpoch(); epoch != gMasterViewSeen) {
        gMasterViewSeen = epoch;
        chwp::render::EditResult redraw;
        redraw.fullInvalidation = true;
        syncEditedDocument(redraw);
    }
    if (const auto end = gWheel.endIfDue(frameNow())) gInput.wheel(gCamera, *end);

    bool coastMoved = false;
    bool coastZoomed = false;
    bool coastEnded = false;
    if (gInput.coasting() && gFrameClock > 0) {
        double dt = time - gFrameClock;
        if (dt > 48) dt = 48;
        if (dt > 0) {
            const chwp::render::ViewportState before = gCamera.state();
            gInput.tick(gCamera, dt);
            const chwp::render::ViewportState after = gCamera.state();
            coastMoved = after.scrollX != before.scrollX || after.scrollY != before.scrollY;
            coastZoomed =
                after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth;
            coastEnded = !gInput.coasting();
        }
    }
    const double frameDt = gFrameClock > 0 ? time - gFrameClock : 16;
    if (time > 0) gFrameClock = time;

    {
        double edgeDt = frameDt;
        if (edgeDt <= 0 || edgeDt > 48) edgeDt = 16;
        int edgeDx = 0;
        int edgeDy = 0;
        if (chwp::render::selectionEdgeScroll(edgeDt, gLastPlan.viewW, gLastPlan.viewH, edgeDx, edgeDy)) {
            gCamera.scrollBy(edgeDx, edgeDy);
            double vx = 0;
            double vy = 0;
            chwp::render::selectionEdgePoint(vx, vy);
            int edgePage = -1;
            int edgeX = 0;
            int edgeY = 0;
            if (paperPoint(vx, vy, true, edgePage, edgeX, edgeY)) {
                chwp::render::PointerSample sample;
                sample.time = time;
                sample.dpr = gLastPlan.dpr > 0 ? gLastPlan.dpr : 1;
                sample.viewX = vx;
                sample.viewY = vy;
                if (edgePage >= 0 && edgePage < static_cast<int>(gLastPlan.pages.size())) {
                    for (const chwp::render::ViewPage& sheet : gLastPlan.pages) {
                        if (sheet.index != edgePage || sheet.w <= 0) continue;
                        const chwp::render::Node& node = gDoc.pages[static_cast<std::size_t>(edgePage)];
                        sample.huPerLogical =
                            node.width > 0 ? (static_cast<double>(chwp::render::viewWidthOf(node)) / sheet.w) * sample.dpr : 0;
                        break;
                    }
                }
                chwp::render::applyPointer(gDoc.doc, gDoc.pages, 2, edgePage, edgeX, edgeY, gCamera.state().percent, sample);
            }
            coastMoved = true;
        }
    }

    // 웹이 view() 로 쪽 배치를 넣기 전에 프레임이 돌 수 있다. 카메라에 쪽이 있으면 여기서 잡는다.
    if (gViewportDirty || gLastPlan.pages.empty() || gLastPlan.viewW <= 1 || gLastPlan.viewH <= 1) {
        chwp::render::ViewPlan seeded = gCamera.plan(false);
        if (seeded.pages.empty() || seeded.viewW <= 1 || seeded.viewH <= 1) {
            drainDone();
            return true;
        }
        gViewportDirty = false;
        useFramePlan(seeded);
        gLastPlan = std::move(seeded);
        publishPlan();
    }

    if (gGuidesDirty) {
        gGuidesDirty = false;
        gShown.ok = false;
    }
    const chwp::render::ViewportState camera = gCamera.state();
    const bool pinching = gInput.pinchActive();
    const bool scrollMoved = gShown.ok && (camera.scrollX != gShown.scrollX || camera.scrollY != gShown.scrollY);
    const bool zoomMoved = gShown.ok && (camera.percent != gShown.percent);
    const bool movedNow = scrollMoved || zoomMoved;
    if (zoomMoved) {
        gScrollHold = 0;
        if (time > 0) gLastZoomMoveTime = time;
    } else if (pinching) {
        gScrollHold = 0;
    } else if (scrollMoved && time > 0) {
        gScrollHold = time;
    }
    if (movedNow) {
        gIdleMoveStreak = 0;
        if (time > 0) gLastCameraMoveTime = time;
    } else if (time > 0) {
        ++gIdleMoveStreak;
    }
    const bool holding = time > 0 && gScrollHold > 0 && time - gScrollHold < 260;
    const bool zoomHolding = time > 0 && gLastZoomMoveTime > 0 && (time - gLastZoomMoveTime) < 180.0;
    const bool gliding = holding || zoomHolding || gInput.wheeling() || gInput.coasting() || gInput.panning() ||
                         gInput.draggingBar() || gInput.touchLive();
    const bool rawInteracting = gliding || pinching;
    const bool recentlyMoved =
        movedNow || gIdleMoveStreak < 2 || zoomHolding ||
        (time > 0 && gLastCameraMoveTime > 0 && (time - gLastCameraMoveTime) < 60.0);
    const bool interacting = rawInteracting && recentlyMoved;
    const bool selecting = gInput.selecting() || chwp::render::pointerTracking();
    setCrewInteracting(interacting || selecting);
    continueLayout(interacting || selecting);
    if (!selecting) {
        runSoloCrew(kSoloIdleBudgetMs);
        drainDone();
    }

    const int revision = gFrames.revision();
    const int picture = chwp::render::pointerPicture();
    const int phase = chwp::render::caretShown() ? caretPhaseNow() : -1;

    const bool masks = !selecting && (chwp::render::glyphMasksPending() || chwp::render::pictureUploadsPending());
    const bool cameraMoved = !gShown.ok || movedNow;
    const bool fastMotion = movedNow || zoomHolding || pinching || interacting || selecting;
    const bool filterStale = gShown.ok && gShown.fastMotion && !fastMotion;
    const bool cameraStale = camera.scrollX != gLastPlan.scrollX || camera.scrollY != gLastPlan.scrollY ||
                             camera.percent != gLastPlan.percent;
    const bool tilesStale = !gShown.ok || revision != gShown.revision;
    const bool planStale = !gShown.ok || gLastPlan.generation != gShown.generation || gLastPlan.viewW != gShown.viewW ||
                           gLastPlan.viewH != gShown.viewH || gLastPlan.scrollX != gShown.scrollX ||
                           gLastPlan.scrollY != gShown.scrollY || gLastPlan.percent != gShown.percent;
    const bool selectionStale = picture != gShown.picture;
    const bool blink = phase != gShown.caretPhase;
    const bool maskStale = masks || (!selecting && masks != gShown.masks) || (gShown.pinching && !interacting);
    const bool finishInteract = gPinchPlanPending && !interacting;
    if (finishInteract) gPinchPlanPending = false;
    const bool spansPending = visibleSpansPending(gLastPlan);
    const bool notifyCam = coastMoved || coastZoomed || coastEnded || finishInteract || gCameraNoteDue;
    const bool activePinchOrZoom = pinching || zoomHolding || (gInput.coasting() && coastZoomed);

    if (!spansPending && !gViewportDirty && interacting && gShown.ok && gLastPlan.viewW == gShown.viewW &&
        gLastPlan.viewH == gShown.viewH) {
        // 스크롤·핀치 중에는 무거운 useFramePlan 은 미루되, 스크롤 위치는 알려 보이는 쪽을 바로 준비시킨다.
        if (camera.percent != gLastPlan.percent) {
            gPinchPlanPending = true;
            gLastPlan = gCamera.plan(false);
        } else if (camera.scrollX != gLastPlan.scrollX || camera.scrollY != gLastPlan.scrollY) {
            gPinchPlanPending = true;
            gLastPlan.scrollX = camera.scrollX;
            gLastPlan.scrollY = camera.scrollY;
            if (!pinching) publishPlan();
        }
        // 일꾼은 손이 움직이는 동안 쉰다. 일꾼이 없으면 미리 구워 둔 쪽이 적으니, 화면에 들어온 빈 칸을 짧게 굽는다.
        // 확대 중에는 배율이 계속 바뀌니 굽지 않는다.
        const bool bakedView = !pinching && !zoomHolding && !selecting && stepSingleThreadView(kSoloScrollBudgetMs);
        if (cameraMoved || tilesStale || bakedView || selectionStale || blink || maskStale || gPageGpuBakePending ||
            filterStale) {
            resizeScreen(gLastPlan.viewW, gLastPlan.viewH);
            if (!presentView(gLastPlan.viewW, gLastPlan.viewH, camera.scrollX, camera.scrollY, camera.percent,
                             fastMotion)) {
                return true;
            }
            gShown.scrollX = camera.scrollX;
            gShown.scrollY = camera.scrollY;
            gShown.percent = camera.percent;
            gShown.generation = gLastPlan.generation;
            gShown.revision = gFrames.revision();
            gShown.picture = picture;
            gShown.caretPhase = phase;
            gShown.masks = chwp::render::glyphMasksPending() || chwp::render::pictureUploadsPending();
            gShown.pinching = true;
            gShown.fastMotion = fastMotion;
        }
        noteCameraIf(notifyCam, camera.scrollX, camera.scrollY, camera.percent, gInput.coasting() || zoomHolding,
                     activePinchOrZoom);
        return true;
    }

    const bool fgPending = !crewRunning() && !interacting && !selecting && gLastPlan.job.id != 0 &&
                           !gLastPlan.job.background;

    if (!spansPending && !gViewportDirty && !finishInteract && gShown.ok && !cameraStale && !tilesStale && !planStale &&
        !selectionStale && !blink && !maskStale && !gPageGpuBakePending && !fgPending && !filterStale) {
        if (!interacting && !selecting) {
            runSingleThreadIdle(kSoloIdleBudgetMs);
            // 화면이 가라앉으면 예산을 넘은 글꼴 파일을 접는다. 커서 글자 모양은 실제로 접을 때만 구한다.
            chwp::render::foldIdleFonts(
                gDoc.doc, [] { return chwp::render::cursorStyle(gDoc.doc, gDoc.pages).charShapeId; }, 4.0);
        }
        noteCameraIf(notifyCam, camera.scrollX, camera.scrollY, camera.percent, gInput.coasting(), activePinchOrZoom);
        return true;
    }

    const bool cacheOnly = !spansPending && !gViewportDirty && !interacting && !finishInteract && gShown.ok && tilesStale &&
                           !cameraStale && !planStale && !selectionStale && !blink && !maskStale && !gPageGpuBakePending &&
                           !fgPending;
    if (spansPending || finishInteract || gViewportDirty || cameraStale || gFrames.revision() != gLastPlanRevision ||
        fgPending) {
        gViewportDirty = false;
        const chwp::render::FramePhaseTimer planTimer(chwp::render::FramePhase::Plan);
        chwp::render::ViewPlan plan = gCamera.plan(false);
        useFramePlan(plan);
        gLastPlan = std::move(plan);
        if (!crewRunning() && !interacting && !selecting && gLastPlan.job.id != 0 && !gLastPlan.job.background) {
            if (stepSingleThreadForeground()) {
                chwp::render::ViewPlan updated = gCamera.plan(false);
                useFramePlan(updated);
                gLastPlan = std::move(updated);
            }
        }
        publishPlan();
    }
    // 화면 밖 칸만 준비됐으면 그리지 않는다. 보이는 칸이 바뀐 뒤에 그린다.
    if (cacheOnly && !filterStale && sameShownDraw(gLastPlan)) {
        gShown.revision = gFrames.revision();
        noteCameraIf(notifyCam, camera.scrollX, camera.scrollY, camera.percent, gInput.coasting(), activePinchOrZoom);
        return true;
    }
    if (gLastPlan.viewW <= 0 || gLastPlan.viewH <= 0) return true;

    resizeScreen(gLastPlan.viewW, gLastPlan.viewH);
    if (!presentView(gLastPlan.viewW, gLastPlan.viewH, gLastPlan.scrollX, gLastPlan.scrollY, gLastPlan.percent,
                     fastMotion)) {
        return true;
    }
    gShown.ok = true;
    gShown.scrollX = gLastPlan.scrollX;
    gShown.scrollY = gLastPlan.scrollY;
    gShown.percent = gLastPlan.percent;
    gShown.viewW = gLastPlan.viewW;
    gShown.viewH = gLastPlan.viewH;
    gShown.generation = gLastPlan.generation;
    gShown.revision = gFrames.revision();
    gShown.picture = picture;
    gShown.caretPhase = phase;
    gShown.masks = chwp::render::glyphMasksPending() || chwp::render::pictureUploadsPending();
    gShown.pinching = interacting;
    gShown.fastMotion = fastMotion;
    rememberShownDraw(gLastPlan);
    noteCameraIf(notifyCam, camera.scrollX, camera.scrollY, camera.percent, gInput.coasting(), activePinchOrZoom);
    return true;
}

void ensureScreenFrames() {
    gShown.ok = false;
    gShownDraw.clear();
    gShownPages.clear();
    gPinchPlanPending = false;
    gPageGpuBakePending = false;
    if (gScreenLoop) return;
    gScreenLoop = true;
    if (hostSlot().startFrameLoop) hostSlot().startFrameLoop();
}

namespace {
// 브라우저 WebGPU 장치는 나중에 붙는다. 이만큼 기다려도 안 오면 화면 없이 간다.
constexpr double kScreenAttachMs = 8000;
double gScreenAttachAt = -1;
}  // namespace

bool attachScreen(bool remote) {
    // GPU 화면을 못 열면 2D 화면이다. 캔버스가 이 스레드에 있든(remote 아님) 없든 2D 그림 목록으로 그린다.
    chwp::render::setScreenOpSink(chwp::render::ScreenOpSink(hostSlot().presentScreenOps));
    const bool opened = remote ? chwp::render::attachRemoteScreen() : chwp::render::resetScreenSurface();
    gScreenOn = opened;
    gScreenAttachAt = opened ? frameNow() : -1;
    if (opened) ensureScreenFrames();
    return opened;
}

ScreenAttach screenAttachState() {
    if (gScreenAttachAt < 0) return ScreenAttach::Failed;
    if (screenReady()) return ScreenAttach::Ready;
    if (frameNow() - gScreenAttachAt > kScreenAttachMs) {
        gScreenAttachAt = -1;
        gScreenOn = false;
        return ScreenAttach::Failed;
    }
    return ScreenAttach::Pending;
}

bool screenReady() {
    const bool ready = chwp::render::screenReady();
    gScreenOn = ready;
    return ready;
}

bool presentView(int viewW, int viewH, int scrollX, int scrollY, int layoutZoom, bool dragging) {
    const chwp::render::FramePhaseTimer phaseTimer(chwp::render::FramePhase::View);
    chwp::render::FramePhaseTimer gatherTimer(chwp::render::FramePhase::Gather);
    if (!gScreenOn) return false;
    const chwp::render::ViewPlan& plan = gLastPlan;
    scrollX = std::clamp(scrollX, 0, std::max(0, plan.maxScrollX));
    scrollY = std::clamp(scrollY, 0, std::max(0, plan.maxScrollY));
    const int width = viewW > 0 ? viewW : plan.viewW;
    const int height = viewH > 0 ? viewH : plan.viewH;
    std::vector<chwp::render::ScreenSprite> papers;
    std::vector<chwp::render::ScreenSprite> sprites;
    std::vector<const chwp::render::ViewPage*> visPages;
    std::vector<bool> inViewFlags;
    papers.reserve(plan.pages.size());
    visPages.reserve(32);
    inViewFlags.reserve(32);
    const int prefetchMargin = dragging ? 0 : std::max(height / 2, 320);
    int inViewPageCount = 0;
    for (const chwp::render::ViewPage& page : plan.pages) {
        chwp::render::ScreenSprite paper;
        paper.page = page.index;
        paper.stamp = gFrames.pageRevision(page.index);
        paper.x = page.x - scrollX;
        paper.y = page.y - scrollY;
        paper.w = page.w;
        paper.h = page.h;
        if (paper.w <= 0 || paper.h <= 0) continue;
        const bool inView =
            !(paper.x >= width || paper.y >= height || paper.x + paper.w <= 0 || paper.y + paper.h <= 0);
        const bool inPrefetch =
            !inView && prefetchMargin > 0 &&
            !(paper.x >= width + prefetchMargin || paper.y >= height + prefetchMargin ||
              paper.x + paper.w <= -prefetchMargin || paper.y + paper.h <= -prefetchMargin);
        if (!inView && !inPrefetch) continue;
        if (inView) ++inViewPageCount;
        papers.push_back(paper);
        visPages.push_back(&page);
        inViewFlags.push_back(inView);
    }
    const int zoom = layoutZoom > 0 ? layoutZoom : 100;
    const bool scaleOnly = chwp::render::overviewZoom(inViewPageCount, zoom);
    const int targetPercent = (scaleOnly || zoom < 100) ? 100 : zoom;

    std::vector<PageSpriteCache*> visCaches;
    visCaches.reserve(visPages.size());
    std::size_t totalCandidateSprites = 0;

    for (const chwp::render::ViewPage* pagePtr : visPages) {
        const chwp::render::ViewPage& page = *pagePtr;
        const int pageRev = gFrames.pageRevision(page.index);
        PageSpriteCache& pCache = gPageSpriteCaches[page.index];
        const bool needRebuild =
            pCache.pageRevision != pageRev ||
            (pCache.multiPercent && !dragging && pCache.targetPercent != targetPercent);
        if (needRebuild) {
            bool multiPercent = false;
            pCache.sprites = chwp::render::readyPageSprites(gFrames, page.index, targetPercent, &multiPercent);
            std::size_t idx = 0;
            for (int layer = 0; layer < 4; ++layer) {
                pCache.layerOffset[layer] = idx;
                while (idx < pCache.sprites.size() && pCache.sprites[idx].layer == layer) {
                    ++idx;
                }
            }
            pCache.layerOffset[4] = pCache.sprites.size();
            if (pCache.sprites.empty()) {
                pCache.pageStamp = 0;
            } else {
                std::uint32_t h = 2166136261u;
                auto mix32 = [&](std::uint32_t v) {
                    h ^= v;
                    h *= 16777619u;
                };
                mix32(static_cast<std::uint32_t>(pageRev));
                for (const LocalPageSprite& sp : pCache.sprites) {
                    mix32(static_cast<std::uint32_t>(sp.id));
                    mix32(static_cast<std::uint32_t>(sp.stamp));
                    mix32(static_cast<std::uint32_t>(sp.percent));
                }
                pCache.pageStamp = static_cast<int>(h & 0x7fffffffu);
                if (pCache.pageStamp == 0) pCache.pageStamp = 1;
            }
            pCache.generation = plan.generation;
            pCache.pageRevision = pageRev;
            pCache.targetPercent = targetPercent;
            pCache.multiPercent = multiPercent;
            pCache.zoom = -1;
            pCache.dstPixelH = -1;
        }
        visCaches.push_back(&pCache);
    }

    const int pageCacheZoom = chwp::render::pageCachePercent(zoom);
    const bool activeZooming = dragging && (gInput.pinchActive() || (gShown.ok && zoom != gShown.percent));
    // 목표로 다가가는 중(휠 칸, 배율 단추)이면 목표 배율 마스크만 굽고, 손을 따라가면 지금 배율 칸을 따라간다.
    // 멈추면 쪽 텍스처에 1:1로 찍히는 배율이다.
    const int zoomAim = gInput.zoomAim();
    const int maskZoom = activeZooming ? chwp::render::maskZoomPercent(zoomAim > 0 ? zoomAim : zoom, true)
                                       : chwp::render::restingMaskZoom(zoom);
    std::vector<int> pageModes(visPages.size(), 0);
    std::vector<char> drawDirect(visPages.size(), 0);
    int bakeCandidates = 0;
    int bakeCandidatePixels = 0;
    std::size_t directSpritesUsed = 0;
    constexpr std::size_t kMaxDirectSpritesWhileDragging = 1200;
    const int maxBakeCount = dragging ? 24 : 64;
    const int maxBakePixels = dragging ? 1800000 : 5400000;
    bool hasUnbakedCandidates = false;

    for (std::size_t p = 0; p < visPages.size(); ++p) {
        const chwp::render::ViewPage& page = *visPages[p];
        int cachePageW = page.w;
        int cachePageH = page.h;
        if (pageCacheZoom != zoom) {
            const bool validPage = page.index >= 0 && page.index < static_cast<int>(gDoc.pages.size());
            const int pageWidthHu = validPage ? chwp::render::viewWidthOf(gDoc.pages[static_cast<std::size_t>(page.index)]) : 0;
            const int pageHeightHu = validPage ? gDoc.pages[static_cast<std::size_t>(page.index)].height : 0;
            cachePageH =
                pageHeightHu > 0
                    ? chwp::render::pagePixelHeight(pageHeightHu, pageCacheZoom)
                    : std::max(1, static_cast<int>(std::lround(static_cast<double>(page.h) *
                                                               static_cast<double>(pageCacheZoom) /
                                                               static_cast<double>(zoom))));
            cachePageW =
                (pageWidthHu > 0 && pageHeightHu > 0)
                    ? chwp::render::pagePixelWidth(pageWidthHu, pageHeightHu, cachePageH)
                    : std::max(1, static_cast<int>(std::lround(static_cast<double>(page.w) *
                                                               static_cast<double>(cachePageH) /
                                                               static_cast<double>(std::max(1, page.h)))));
        }
        papers[p].sw = cachePageW;
        papers[p].sh = cachePageH;
        papers[p].percent = pageCacheZoom;
    }

    for (int pass = 0; pass < 3; ++pass) {
        for (std::size_t p = 0; p < visPages.size(); ++p) {
            const bool inView = inViewFlags[p];
            if (pass < 2 && !inView) continue;
            if (pass == 2 && inView) continue;
            const chwp::render::ViewPage& page = *visPages[p];
            const PageSpriteCache& pCache = *visCaches[p];
            const int cachePageW = papers[p].sw > 0 ? papers[p].sw : page.w;
            const int cachePageH = papers[p].sh > 0 ? papers[p].sh : page.h;
            papers[p].stamp = pCache.pageStamp;
            if (pass == 0) papers[p].nearest = false;
            if (zoom > 100) {
                if (pass == 1) continue;
                if (!inView) {
                    pageModes[p] = 3;
                    continue;
                }
                if (!pageNeedsSpans(page.index) && pCache.sprites.empty() &&
                    chwp::render::hasScalablePageGpuTexture(page.index, pCache.pageStamp)) {
                    papers[p].percent = 100;
                    pageModes[p] = 2;
                    continue;
                }
                directSpritesUsed += pCache.sprites.size();
                totalCandidateSprites += pCache.sprites.size();
                continue;
            }
            // 지금보다 작은 배율로 구운 쪽 텍스처를 늘리면 작은 글자 점이 덩어리로 커진다.
            // 확대 중이든 스크롤 중이든 새 텍스처를 굽기 전이든, 칸이 있고 직접 그릴 여유가 있으면 글자 쿼드로 그린다.
            // 한 번 정하면 다음 단계에서 텍스처로 되돌리지 않는다.
            if (drawDirect[p]) continue;
            const int texZoom = chwp::render::pageGpuTextureZoom(page.index, pCache.pageStamp);
            const bool texTooSmall = pass == 0 && texZoom > 0 && texZoom < pageCacheZoom && !pCache.sprites.empty() &&
                                     (!dragging || directSpritesUsed + pCache.sprites.size() <=
                                                       kMaxDirectSpritesWhileDragging);
            if (texTooSmall) drawDirect[p] = 1;
            if (!texTooSmall && !pageNeedsSpans(page.index) &&
                chwp::render::canUsePageGpuTexture(page.index, cachePageW, cachePageH, pageCacheZoom, maskZoom,
                                                     pCache.pageStamp, dragging)) {
                pageModes[p] = 2;
                continue;
            }
            const bool spansOpen = pageNeedsSpans(page.index);
            const bool hasScalable = !spansOpen && !texTooSmall &&
                                     chwp::render::hasScalablePageGpuTexture(page.index, pCache.pageStamp);
            const bool hasAnyTex = hasScalable;
            if (pass == 0 && hasAnyTex) continue;
            if (pass == 1 && !hasAnyTex) continue;
            const bool canBakePage =
                chwp::render::canBakePageGpuTexture(page.index, cachePageW, cachePageH, pCache.pageStamp);
            if (dragging && hasAnyTex) {
                pageModes[p] = 2;
                if (canBakePage && !hasScalable) hasUnbakedCandidates = true;
                continue;
            }
            // 핀치 줌 도중에는 오프스크린 페이지 렌더타겟과 밉맵을 새로 굽지 않고 글리프 쿼드를 직접 스케일링한다.
            if (activeZooming && canBakePage) {
                if (!inView) {
                    pageModes[p] = 3;
                    continue;
                }
                directSpritesUsed += pCache.sprites.size();
                totalCandidateSprites += pCache.sprites.size();
                continue;
            }
            if (canBakePage) {
                const int pagePixels = cachePageW * cachePageH;
                const bool withinBudget =
                    bakeCandidates == 0 ||
                    (bakeCandidates < maxBakeCount && bakeCandidatePixels + pagePixels <= maxBakePixels);
                if (withinBudget) {
                    pageModes[p] = 1;
                    papers[p].nearest = true;
                    ++bakeCandidates;
                    bakeCandidatePixels += pagePixels;
                    totalCandidateSprites += pCache.sprites.size();
                    continue;
                }
                hasUnbakedCandidates = true;
                if (hasAnyTex) {
                    pageModes[p] = 2;
                    continue;
                }
                if (!inView) {
                    pageModes[p] = 3;
                    continue;
                }
            } else if (hasAnyTex) {
                pageModes[p] = 2;
                continue;
            }
            if (!inView) {
                pageModes[p] = 3;
                continue;
            }
            if (dragging && directSpritesUsed > 0 &&
                directSpritesUsed + pCache.sprites.size() > kMaxDirectSpritesWhileDragging) {
                pageModes[p] = 3;
                hasUnbakedCandidates = true;
                continue;
            }
            directSpritesUsed += pCache.sprites.size();
            totalCandidateSprites += pCache.sprites.size();
        }
    }
    gPageGpuBakePending = hasUnbakedCandidates;

    for (std::size_t p = 0; p < visPages.size(); ++p) {
        const int mode = pageModes[p];
        if (mode == 2 || mode == 3) continue;
        if (mode == 0 && !inViewFlags[p]) continue;
        const chwp::render::ViewPage& page = *visPages[p];
        PageSpriteCache& pCache = *visCaches[p];
        const int pageHeightHu =
            (page.index >= 0 && page.index < static_cast<int>(gDoc.pages.size()))
                ? gDoc.pages[static_cast<std::size_t>(page.index)].height
                : 0;
        const int spriteZoom = (mode == 1) ? pageCacheZoom : zoom;
        const int dstPixelH =
            (mode == 1 && papers[p].sh > 0)
                ? papers[p].sh
                : ((spriteZoom == plan.percent && page.h > 0)
                       ? page.h
                       : (pageHeightHu > 0 ? chwp::render::pagePixelHeight(pageHeightHu, spriteZoom) : page.h));
        if (pCache.zoom == spriteZoom && pCache.dstPixelH == dstPixelH) continue;
        chwp::render::placePageSprites(pCache.sprites, spriteZoom, dstPixelH, pageHeightHu);
        pCache.maxLocalBottomPrefix.resize(pCache.sprites.size());
        pCache.minLocalYSuffix.resize(pCache.sprites.size());
        for (int layer = 0; layer < 4; ++layer) {
            const std::size_t lBegin = pCache.layerOffset[layer];
            const std::size_t lEnd = pCache.layerOffset[layer + 1];
            int runMaxBot = std::numeric_limits<int>::min();
            for (std::size_t k = lBegin; k < lEnd; ++k) {
                runMaxBot = std::max(runMaxBot, pCache.sprites[k].localY + pCache.sprites[k].h);
                pCache.maxLocalBottomPrefix[k] = runMaxBot;
            }
            int runMinTop = std::numeric_limits<int>::max();
            for (std::size_t k = lEnd; k > lBegin; --k) {
                runMinTop = std::min(runMinTop, pCache.sprites[k - 1].localY);
                pCache.minLocalYSuffix[k - 1] = runMinTop;
            }
        }
        pCache.zoom = spriteZoom;
        pCache.dstPixelH = dstPixelH;
    }

    sprites.reserve(totalCandidateSprites);
    for (int layer = 0; layer < 4; ++layer) {
        for (std::size_t p = 0; p < visPages.size(); ++p) {
            const int mode = pageModes[p];
            if (mode == 2 || mode == 3) continue;
            if (mode == 0 && !inViewFlags[p]) continue;
            const chwp::render::ViewPage& page = *visPages[p];
            const PageSpriteCache& pCache = *visCaches[p];
            const int pageScreenX = page.x - scrollX;
            const int pageScreenY = page.y - scrollY;
            const std::size_t begin = pCache.layerOffset[layer];
            const std::size_t end = pCache.layerOffset[layer + 1];
            if (begin >= end) continue;
            if (mode == 1) {
                for (std::size_t i = begin; i < end; ++i) {
                    const LocalPageSprite& item = pCache.sprites[i];
                    chwp::render::ScreenSprite s;
                    s.id = item.id;
                    s.stamp = item.stamp;
                    s.x = pageScreenX + item.localX;
                    s.y = pageScreenY + item.localY;
                    s.w = item.w;
                    s.h = item.h;
                    s.sw = item.sw;
                    s.sh = item.sh;
                    s.percent = item.percent;
                    s.page = page.index;
                    s.layer = item.layer;
                    s.nearest = item.nearest;
                    s.rgba = item.rgba;
                    sprites.push_back(s);
                }
                continue;
            }
            const int viewLocalTop = -pageScreenY;
            const int viewLocalBottom = height - pageScreenY;
            const auto firstIt = std::upper_bound(pCache.maxLocalBottomPrefix.begin() + static_cast<std::ptrdiff_t>(begin),
                                                  pCache.maxLocalBottomPrefix.begin() + static_cast<std::ptrdiff_t>(end),
                                                  viewLocalTop);
            const std::size_t startIdx = static_cast<std::size_t>(firstIt - pCache.maxLocalBottomPrefix.begin());
            for (std::size_t i = startIdx; i < end; ++i) {
                if (pCache.minLocalYSuffix[i] >= viewLocalBottom) break;
                const LocalPageSprite& item = pCache.sprites[i];
                const int sx = pageScreenX + item.localX;
                const int sy = pageScreenY + item.localY;
                if (sx >= width || sy >= height || sx + item.w <= 0 || sy + item.h <= 0) continue;
                chwp::render::ScreenSprite s;
                s.id = item.id;
                s.stamp = item.stamp;
                s.x = sx;
                s.y = sy;
                s.w = item.w;
                s.h = item.h;
                s.sw = item.sw;
                s.sh = item.sh;
                s.percent = item.percent;
                s.page = page.index;
                s.layer = item.layer;
                s.nearest = item.nearest;
                s.rgba = item.rgba;
                sprites.push_back(s);
            }
        }
    }

    chwp::render::Scrollbar barX;
    chwp::render::Scrollbar barY;
    chwp::render::layoutScrollbars(barX, barY, width, height, plan.dpr, scrollX, scrollY, plan.maxScrollX,
                                     plan.maxScrollY, plan.contentW, plan.contentH, plan.ruler);
    gatherTimer.stop();
    // 격자·안내선은 쪽마다 종이·본문 영역으로 그린다. 화면에 걸친 쪽만 넣는다.
    {
        std::vector<std::pair<int, chwp::render::PaperBox>> boxes;
        boxes.reserve(papers.size());
        for (const chwp::render::ScreenSprite& quad : papers) {
            if (quad.page < 0 || quad.page >= static_cast<int>(gDoc.pages.size())) continue;
            boxes.emplace_back(quad.page, chwp::render::paperBoxOf(gDoc.pages[static_cast<std::size_t>(quad.page)]));
        }
        chwp::render::setScreenPaperBoxes(std::move(boxes));
    }
    return chwp::render::presentScreen(width, height, papers, sprites, layoutZoom, maskZoom, plan.pages, scrollX,
                                         scrollY, barX, barY, dragging, plan.ruler, plan.dpr);
}

// 프레임마다 처리 시간과 손이 움직이는 중인지를 적는다. 기기에서 부드러운지는 이 통계로 본다.
chwp::app::FrameStats gFrameStats;

bool screenFrame(double time) {
    const double start = frameNow();
    chwp::render::resetFramePhases();
    const bool keep = screenFrame(time, nullptr);
    gFrameStats.record(time > 0 ? time : start, frameNow() - start, gUserInteracting.load(std::memory_order_relaxed),
                       chwp::render::framePhases());
    return keep;
}

std::string frameStats() { return chwp::app::frameSummaryJson(gFrameStats.take()); }

std::string memoryReport() {
    std::vector<std::pair<std::string, std::size_t>> items;
    std::size_t fontFile = 0;
    std::size_t fontTable = 0;
    int faces = 0;
    chwp::layout::fontMemory(fontFile, fontTable, faces);
    items.emplace_back("fontFiles", fontFile);
    items.emplace_back("fontPacked", chwp::layout::packedFontBytes());
    items.emplace_back("fontTables", fontTable);
    items.emplace_back("fontFaces", static_cast<std::size_t>(faces));
    chwp::render::renderMemory(items);
    items.emplace_back("frameCatalog", gFrames.catalogBytes());
    std::size_t nodes = 0;
    items.emplace_back("sceneNodes", chwp::render::sceneMemory(gDoc.pages, nodes));
    items.emplace_back("sceneNodeCount", nodes);
    static const char* kKindNames[] = {"Paper", "Body", "Column", "Paragraph", "Line", "Object", "Cell"};
    for (int k = 0; k < 7; ++k) {
        items.emplace_back(std::string("sceneCount") + kKindNames[k], chwp::render::sceneKindCount(k));
    }
    items.emplace_back("sceneCountPagesSpansLater", chwp::render::sceneKindCount(8));
    items.emplace_back("sceneCountExtras", chwp::render::sceneKindCount(9));
    static const char* kPartNames[] = {"Heads", "Tails", "Lines", "Runs", "Widths", "ChildSlack", "Other", "RunCount"};
    for (int part = 0; part < chwp::render::kScenePartCount; ++part) {
        items.emplace_back(std::string("scenePart") + kPartNames[part],
                           chwp::render::scenePart(static_cast<chwp::render::ScenePart>(part)));
    }
    items.emplace_back("sizeofNode", sizeof(chwp::render::Node));
    items.emplace_back("sizeofNodeExtra", sizeof(chwp::render::NodeExtra));
    for (const chwp::render::CacheUsage& usage : chwp::render::cacheUsage()) items.emplace_back(usage.name, usage.bytes);
    std::string out = "{";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += ",";
        out += "\"" + items[i].first + "\":" + std::to_string(items[i].second);
    }
    return out + "}";
}

void ensurePageCache(int pixelHeight) {
    // 아직 쪽을 놓지 않았으면 열 때와 같은 core 기본값으로 놓는다.
    if (gDoc.pages.empty() && !gDoc.doc.sections.empty()) gDoc.layout();
    if (pixelHeight != gPixelHeight) {
        gPagePng.assign(gDoc.pages.size(), {});
        gPagePixels.assign(gDoc.pages.size(), {});
        gPixelHeight = pixelHeight;
    }
}

const std::vector<std::uint8_t>* renderPagePng(int page, int pixelHeight) {
    ensurePageCache(pixelHeight);
    if (page < 0 || page >= static_cast<int>(gDoc.pages.size())) return nullptr;
    if (gPagePng[static_cast<std::size_t>(page)].empty()) {
        // agent 와 같은 쪽 그림(GPU 먼저, 못 쓰면 CPU). 칸은 화면의 칸 캐시에 굽는다.
        gPagePng[static_cast<std::size_t>(page)] = gDoc.renderPage(page, pixelHeight, gFrames).png;
    }
    return &gPagePng[static_cast<std::size_t>(page)];
}

std::string printBlockedReason() {
    return gDoc.printBlocked() ? "배포용 문서라 인쇄와 PDF 저장이 막혀 있습니다." : std::string();
}

Bytes exportPdfNow(int dpi, int from, int to) {
    if (gDoc.printBlocked()) return {};
    if (gDoc.pages.empty() && !gDoc.doc.sections.empty()) gDoc.layout();
    // 쪽 글자를 채우는 일꾼과 같은 쪽을 만지지 않게 멈춘 뒤 그린다.
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    Bytes out;
    {
        chwp::render::PageFillGuard guard;
        out = gDoc.exportPdf(dpi, gFrames, from, to);
    }
    if (wasCrew) resumeCrewAfterEdit(chwp::render::EditResult{});
    return out;
}

const render::PageBitmap* renderPagePixels(int page, int pixelHeight) {
    ensurePageCache(pixelHeight);
    if (page < 0 || page >= static_cast<int>(gDoc.pages.size())) return nullptr;
    chwp::render::PageBitmap& frame = gPagePixels[static_cast<std::size_t>(page)];
    if (frame.rgba.empty()) {
        frame = chwp::render::renderPageGpu(gDoc.doc, gDoc.pages, page, pixelHeight, gFrames);
        if (frame.rgba.empty()) {
            frame = chwp::render::renderPageBitmap(gDoc.doc, gDoc.pages[static_cast<std::size_t>(page)], pixelHeight);
        }
    }
    return &frame;
}

void useFramePlan(chwp::render::ViewPlan& plan) {
    int visiblePages = 0;
    int minPageH = 0;
    for (const chwp::render::ViewPage& page : plan.pages) {
        if (page.w <= 0 || page.h <= 0) continue;
        if (minPageH <= 0 || page.h < minPageH) minPageH = page.h;
        if (plan.viewW > 0 && plan.viewH > 0 && page.x < plan.scrollX + plan.viewW && page.x + page.w > plan.scrollX &&
            page.y < plan.scrollY + plan.viewH && page.y + page.h > plan.scrollY) {
            ++visiblePages;
        }
    }
    const bool overview = chwp::render::overviewZoom(visiblePages, plan.percent);
    const int margin = (overview && minPageH > 0)
                           ? std::clamp(std::min(plan.viewH, std::max(minPageH * 2, 320)), 1, std::max(plan.viewH, 1))
                           : std::max(plan.viewH, 1);
    const bool crewActive = gScreenOn && crewRunning();
    const bool asyncLayoutOnly = crewActive && overview;
    int syncBuilt = 0;
    for (const chwp::render::ViewPage& page : plan.pages) {
        const bool near = page.w > 0 && page.h > 0 && plan.viewW > 0 && plan.viewH > 0 &&
                          page.x < plan.scrollX + plan.viewW + margin && page.x + page.w > plan.scrollX - margin &&
                          page.y < plan.scrollY + plan.viewH + margin && page.y + page.h > plan.scrollY - margin;
        if (!near) continue;
        if (page.index < 0 || page.index >= static_cast<int>(gDoc.pages.size())) continue;
        const auto& pageNode = gDoc.pages[static_cast<std::size_t>(page.index)];
        const bool onScreen = page.x < plan.scrollX + plan.viewW && page.x + page.w > plan.scrollX &&
                              page.y < plan.scrollY + plan.viewH && page.y + page.h > plan.scrollY;
        const bool alreadyLaidOut = !pageNode.children.empty();
        const bool spansReady = !pageNode.spansLater && alreadyLaidOut && pageNode.contentHash != 0;
        if (spansReady) continue;
        // 보이는 쪽은 배경 스레드가 켜져 있어도 글자를 채운 뒤에 그린다.
        // 화면 밖 여유 쪽만 한 프레임에 하나만 채운다.
        if (!onScreen && crewActive && (syncBuilt >= 1 || (!alreadyLaidOut && asyncLayoutOnly))) continue;
        if (onScreen && !alreadyLaidOut && asyncLayoutOnly) continue;
        ensurePageSpans(page.index);
        ++syncBuilt;
    }
    gFrames.plan(plan, gDoc.pages, {}, &gDoc.doc, crewActive);
    gLastPlanRevision = gFrames.revision();
}

const char* gridModeName(chwp::render::PageGridMode mode) {
    switch (mode) {
        case chwp::render::PageGridMode::Two: return "two";
        case chwp::render::PageGridMode::Auto: return "auto";
        case chwp::render::PageGridMode::Grid: return "grid";
        case chwp::render::PageGridMode::Single:
        default: return "single";
    }
}

const char* zoomModeName(bool fitPage, bool fitWidth) {
    return fitPage ? "fitPage" : (fitWidth ? "fitWidth" : "percent");
}

std::string zoomLabel(bool fitPage, bool fitWidth, int percent) {
    if (fitPage) return "쪽 맞춤";
    if (fitWidth) return "폭 맞춤";
    return std::to_string(percent) + "%";
}

chwp::render::PageGridMode parseGridMode(const std::string& mode) {
    if (mode == "two" || mode == "2") return chwp::render::PageGridMode::Two;
    if (mode == "auto") return chwp::render::PageGridMode::Auto;
    if (mode == "grid") return chwp::render::PageGridMode::Grid;
    return chwp::render::PageGridMode::Single;
}

ViewApplied applyView(const ViewPatch& patch) {
    drainDone();
    if (patch.reset) {
        gCamera.reset();
        gInput.stop();
    }
    if (patch.viewport) gCamera.setViewport(patch.viewport->cssW, patch.viewport->cssH, patch.viewport->dpr);
    if (patch.pages) {
        std::vector<chwp::render::PageSpec> specs;
        const int count = static_cast<int>(patch.pages->size());
        specs.reserve(static_cast<std::size_t>(std::max(0, count)));
        for (int i = 0; i < count; ++i) {
            const PageSpecPatch& item = (*patch.pages)[static_cast<std::size_t>(i)];
            chwp::render::PageSpec spec;
            spec.index = item.index;
            spec.widthHu = item.widthHu;
            spec.heightHu = item.heightHu;
            const bool validNode = spec.index >= 0 && spec.index < static_cast<int>(gDoc.pages.size());
            const chwp::render::Node* node = validNode ? &gDoc.pages[static_cast<std::size_t>(spec.index)] : nullptr;
            spec.marginLeftHu = item.marginLeftHu ? *item.marginLeftHu
                                                  : (node ? node->marginLeft : chwp::render::kDefaultPageMarginLeftHu);
            spec.marginRightHu = item.marginRightHu
                                     ? *item.marginRightHu
                                     : (node ? node->marginRight : chwp::render::kDefaultPageMarginRightHu);
            spec.marginTopHu = item.marginTopHu
                                   ? *item.marginTopHu
                                   : (node ? node->marginTop
                                           : chwp::render::defaultPageMarginTopHu(static_cast<std::size_t>(i)));
            spec.marginBottomHu =
                item.marginBottomHu
                    ? *item.marginBottomHu
                    : (node ? node->marginBottom
                            : chwp::render::defaultPageMarginBottomHu(static_cast<std::size_t>(i),
                                                                        static_cast<std::size_t>(std::max(1, count))));
            specs.push_back(spec);
        }
        gLastPageGap = patch.gap;
        gCamera.setPages(specs, patch.gap);
    } else if (patch.page) {
        const PageSpecPatch& page = *patch.page;
        const int idx = page.index;
        gCamera.setPage(idx, page.widthHu, page.heightHu);
        const bool validNode = idx >= 0 && idx < static_cast<int>(gDoc.pages.size());
        const chwp::render::Node* node = validNode ? &gDoc.pages[static_cast<std::size_t>(idx)] : nullptr;
        const int ml = page.marginLeftHu ? *page.marginLeftHu
                                         : (node ? node->marginLeft : chwp::render::kDefaultPageMarginLeftHu);
        const int mr = page.marginRightHu ? *page.marginRightHu
                                          : (node ? node->marginRight : chwp::render::kDefaultPageMarginRightHu);
        const int mt = page.marginTopHu ? *page.marginTopHu
                                        : (node ? node->marginTop : chwp::render::defaultPageMarginTopHu(0));
        const int mb = page.marginBottomHu ? *page.marginBottomHu
                                           : (node ? node->marginBottom : chwp::render::defaultPageMarginBottomHu(0, 1));
        gCamera.setPageMargins(ml, mr, mt, mb);
    }
    bool fitOrStepApplied = false;
    if (patch.grid) {
        const ViewPatch::Grid& grid = *patch.grid;
        const bool fit = grid.fit ? *grid.fit : (grid.mode == "grid");
        gCamera.setGridMode(parseGridMode(grid.mode), grid.cols, grid.rows, fit);
        gInput.stop();
        if (fit) fitOrStepApplied = true;
    }
    if (patch.fit) {
        if (*patch.fit == "width") {
            gCamera.fitWidth();
            gInput.stop();
            fitOrStepApplied = true;
        } else if (*patch.fit == "page") {
            gCamera.fitPage();
            gInput.stop();
            fitOrStepApplied = true;
        }
    }
    // 손가락, 관성, 휠, 휠 버튼 드래그, 스크롤바 드래그가 스크롤을 갖고 있으면 옛 배율·스크롤로 덮지 않는다.
    // 배율이 실제로 바뀌면 그 앵커만 남기고, 이전 줌의 스크롤 값은 넣지 않는다.
    if (!fitOrStepApplied && (patch.zoom || patch.scroll) && !gInput.touchLive() && !gInput.draggingBar() &&
        !gInput.wheeling() && !gInput.panning() && !gInput.pinchActive() && !gInput.coasting()) {
        const int before = gCamera.state().percent;
        const bool beforeFitPage = gCamera.isFitPage();
        const bool beforeFitWidth = gCamera.isFitWidth();
        if (patch.zoom) gCamera.setZoomPercent(*patch.zoom);
        const bool zoomed = gCamera.state().percent != before || gCamera.isFitPage() != beforeFitPage ||
                            gCamera.isFitWidth() != beforeFitWidth;
        gInput.stop();
        if (!zoomed && patch.scroll) gCamera.setScroll(patch.scroll->first, patch.scroll->second);
    }
    if (patch.pageStep != 0) {
        gCamera.stepPage(patch.pageStep);
        gInput.stop();
    }
    const int commitId = patch.commit;
    bool committed = false;
    if (commitId && gScreenOn) committed = gFrames.contains(commitId);
    else if (commitId) committed = gCamera.commit(commitId);

    chwp::render::ViewPlan plan = gCamera.plan(!gScreenOn);
    if (gScreenOn) {
        const bool alreadyPlanned = !gPinchPlanPending && !committed &&
                                    (!gLastPlan.draw.empty() || !gShownPages.empty()) && gShown.ok &&
                                    gShown.revision == gFrames.revision() &&
                                    plan.generation == gLastPlan.generation && plan.viewW == gLastPlan.viewW &&
                                    plan.viewH == gLastPlan.viewH && plan.scrollX == gLastPlan.scrollX &&
                                    plan.scrollY == gLastPlan.scrollY && plan.percent == gLastPlan.percent &&
                                    plan.fitPage == gLastPlan.fitPage && plan.fitWidth == gLastPlan.fitWidth &&
                                    plan.pages.size() == gLastPlan.pages.size();
        gPinchPlanPending = false;
        if (!alreadyPlanned) {
            useFramePlan(plan);
            gLastPlan = plan;
            publishPlan();
        }
    } else {
        gLastPlan = plan;
        publishPlan();
    }
    ViewApplied out;
    out.plan = std::move(plan);
    out.committed = committed;
    out.screenOn = gScreenOn;
    out.frameJob = gScreenOn && out.plan.job.id >= chwp::render::kFirstFrameId;
    if (gScreenOn) {
        out.plan.drop.clear();
        out.plan.draw.clear();
    }
    return out;
}

int idleOriginPage(const chwp::render::ViewPlan& plan) {
    for (const chwp::render::ViewPage& page : plan.pages) {
        if (page.x < plan.scrollX + plan.viewW && page.x + page.w > plan.scrollX && page.y < plan.scrollY + plan.viewH &&
            page.y + page.h > plan.scrollY) {
            return page.index;
        }
    }
    return plan.job.page;
}

bool jobOnScreen(const chwp::render::ViewPlan& plan) {
    if (plan.job.id == 0) return false;
    for (const chwp::render::ViewPage& page : plan.pages) {
        if (page.index != plan.job.page) continue;
        const int x = page.x + plan.job.x;
        const int y = page.y + plan.job.y;
        return x < plan.scrollX + plan.viewW && x + plan.job.w > plan.scrollX && y < plan.scrollY + plan.viewH &&
               y + plan.job.h > plan.scrollY;
    }
    return false;
}


// 화면 스레드는 입력과 프레임만 본다. 나머지 스레드는 줄 자리, 마스크, 칸을 문서 끝까지 굽는다.
struct LineJob {
    int page = 0;
    int pixelH = 0;
    chwp::render::FrameDesc frame;
    std::string key;
};

struct BakeJob {
    chwp::render::FrameDesc frame;
    int page = 0;
    int percent = 100;
    int total = 0;
    bool hot = false;
    bool onScreen = false;
    bool line = false;
    int gap = 0;
    int screenX = 0;
    int screenY = 0;
    std::string key;
};

struct BakeDone {
    chwp::render::FrameDesc frame;
    int page = 0;
    int percent = 100;
    int total = 0;
    chwp::render::PageBitmap bitmap;
};

struct Taken {
    int percent = 100;
    int page = 0;
    std::string key;
};

struct ScreenWindow {
    int scrollX = 0;
    int scrollY = 0;
    int viewW = 0;
    int viewH = 0;
    std::vector<chwp::render::ViewPage> pages;
};

struct LineTally {
    int total = 0;
    int left = 0;
    bool told = false;
};

std::mutex gWorkMu;
std::condition_variable gWorkCv;
std::atomic<bool> gCrewLive{false};
bool gCrewStop = false;
bool gCrewPaused = false;
std::chrono::steady_clock::time_point gCrewEditHoldUntil{};
int gScanInflight = 0;
int gTick = 0;
int gScanTick = 0;
int gOrigin = 0;
// 칸 예산이 정한 미리 굽기 범위. 보이는 쪽 gKeepFirst..gKeepLast 에서 gKeepRadius 쪽까지. 음수면 끝이 없다.
int gKeepFirst = -1;
int gKeepLast = -1;
int gKeepRadius = -1;
int gPercent = 100;
bool gScaleOnly = false;
int gCrewSize = 1;
int gLineInflight = 0;
int gBakeInflight = 0;
int gBigBakeInflight = 0;
std::size_t gDoneBytes = 0;
bool gMaskBusy = false;
bool gMasksDone = false;
bool gGlyphsNoted = false;
bool gGlyphsRunning = false;
bool gLineTold = false;
int gTurn = 0;
int gPictureCursor = 0;
int gPictureInflight = 0;
std::vector<std::string> gPictureIds;
std::vector<std::thread> gCrew;
std::vector<char> gClaimed;
std::vector<char> gBaseScanned;
std::vector<int> gVisible;
std::deque<LineJob> gLineJobs;
std::deque<BakeJob> gBakeJobs;
std::vector<BakeDone> gDone;
std::unordered_map<int, Taken> gTaken;
// 그림을 못 읽어 회색이 된 칸은 한 번만 다시 굽는다.
std::unordered_set<int> gPictureRetry;
// 화면 밖으로 빠져 지운 배율 칸. 스크롤이나 배율이 바뀌기 전에는 다시 굽지 않는다.
std::unordered_set<std::string> gZoomSkip;
ScreenWindow gWindow;
std::unordered_set<std::string> gLineHold;
std::unordered_map<int, LineTally> gLineTally;

int grindThreads() {
    int cores = hostSlot().logicalCores ? hostSlot().logicalCores() : 0;
    if (cores <= 0) return 0;
    if (cores < 2) cores = 2;
    return std::clamp(cores - 1, 1, 4);
}

void setCrewInteracting(bool interacting) {
    chwp::render::setRenderInteracting(interacting);
    const bool wasInteracting = gUserInteracting.exchange(interacting, std::memory_order_relaxed);
    if (wasInteracting && !interacting) {
        std::lock_guard<std::mutex> lock(gWorkMu);
        ++gTick;
        gWorkCv.notify_all();
    }
}

std::string zoomBakeKey(const chwp::render::FrameDesc& frame) {
    std::string key = std::to_string(frame.page) + "/" + std::to_string(frame.kind);
    for (const int step : frame.path) key += "." + std::to_string(step);
    key += "@" + std::to_string(frame.percent > 0 ? frame.percent : 100);
    return key;
}

std::string lineHoldKey(const chwp::render::FrameDesc& frame) {
    std::string key = std::to_string(frame.page);
    key.push_back(':');
    for (const int step : frame.path) {
        key += std::to_string(step);
        key.push_back('.');
    }
    return key;
}

bool pageOnScreen(const chwp::render::ViewPage& page, const chwp::render::ViewPlan& plan) {
    return page.w > 0 && page.h > 0 && page.x < plan.scrollX + plan.viewW && page.x + page.w > plan.scrollX &&
           page.y < plan.scrollY + plan.viewH && page.y + page.h > plan.scrollY;
}

bool isBigBake(const chwp::render::FrameDesc& frame) {
    return frame.kind != 0 &&
           static_cast<std::int64_t>(frame.w) * static_cast<std::int64_t>(frame.h) >= 1024LL * 1024LL;
}

bool keepPageLocked(int page) {
    if (gKeepRadius < 0 || gKeepFirst < 0) return true;
    const int gap = page < gKeepFirst ? gKeepFirst - page : (page > gKeepLast ? page - gKeepLast : 0);
    return gap <= gKeepRadius;
}

bool isPageHot(int page, const std::vector<int>& visible, int origin) {
    if (!visible.empty()) {
        return std::find(visible.begin(), visible.end(), page) != visible.end();
    }
    return page == origin;
}

void updateBakeMetricsLocked(BakeJob& job) {
    job.hot = isPageHot(job.page, gVisible, gOrigin);
    const chwp::render::ViewPage* placed = nullptr;
    if (job.page >= 0 && job.page < static_cast<int>(gWindow.pages.size()) &&
        gWindow.pages[static_cast<std::size_t>(job.page)].index == job.page) {
        placed = &gWindow.pages[static_cast<std::size_t>(job.page)];
    } else {
        for (const chwp::render::ViewPage& item : gWindow.pages) {
            if (item.index == job.page) {
                placed = &item;
                break;
            }
        }
    }
    if (placed && gWindow.viewW > 0 && gWindow.viewH > 0) {
        const double scale =
            (job.frame.pixelHeight > 0 && placed->h > 0 && job.percent != gPercent)
                ? (static_cast<double>(placed->h) / static_cast<double>(job.frame.pixelHeight))
                : 1.0;
        const int fx = placed->x + static_cast<int>(std::lround(static_cast<double>(job.frame.x) * scale));
        const int fy = placed->y + static_cast<int>(std::lround(static_cast<double>(job.frame.y) * scale));
        const int fw = std::max(1, static_cast<int>(std::lround(static_cast<double>(job.frame.w) * scale)));
        const int fh = std::max(1, static_cast<int>(std::lround(static_cast<double>(job.frame.h) * scale)));
        int gap = 0;
        if (fy + fh <= gWindow.scrollY) gap += gWindow.scrollY - (fy + fh);
        else if (fy >= gWindow.scrollY + gWindow.viewH) gap += fy - (gWindow.scrollY + gWindow.viewH);
        if (fx + fw <= gWindow.scrollX) gap += gWindow.scrollX - (fx + fw);
        else if (fx >= gWindow.scrollX + gWindow.viewW) gap += fx - (gWindow.scrollX + gWindow.viewW);
        job.gap = gap;
        job.onScreen = job.hot && (gap == 0);
        job.screenX = fx;
        job.screenY = (gap == 0) ? std::max(fy, gWindow.scrollY) : fy;
        return;
    }
    job.gap = 0;
    job.onScreen = job.hot;
    job.screenX = job.frame.x;
    job.screenY = job.frame.y;
}

void reprioritizeBakesLocked() {
    for (BakeJob& job : gBakeJobs) updateBakeMetricsLocked(job);
    const int targetPercent = gScaleOnly ? 100 : (gPercent > 0 ? gPercent : 100);
    const int bandBase = (gWindow.viewW > 0 && gWindow.viewH > 0) ? gWindow.scrollY : 0;
    std::stable_sort(gBakeJobs.begin(), gBakeJobs.end(), [&](const BakeJob& a, const BakeJob& b) {
        if (a.hot != b.hot) return a.hot;
        if (a.hot) {
            const bool aTarget = a.percent == targetPercent;
            const bool bTarget = b.percent == targetPercent;
            if (aTarget != bTarget) return aTarget;
            if (a.onScreen != b.onScreen) return a.onScreen;
            if (a.onScreen) {
                if (a.line != b.line) return a.line;
                const int bandA = std::max(0, a.screenY - bandBase) / 160;
                const int bandB = std::max(0, b.screenY - bandBase) / 160;
                if (bandA != bandB) return bandA < bandB;
                if (!a.line && !b.line) {
                    if (a.frame.layer != b.frame.layer) return a.frame.layer < b.frame.layer;
                    if (a.frame.z != b.frame.z) return a.frame.z < b.frame.z;
                    if (a.frame.rank != b.frame.rank) return a.frame.rank < b.frame.rank;
                }
                if (a.screenY != b.screenY) return a.screenY < b.screenY;
                return a.screenX < b.screenX;
            }
            if (a.line != b.line) return a.line;
            const int gapBandA = a.gap / 160;
            const int gapBandB = b.gap / 160;
            if (gapBandA != gapBandB) return gapBandA < gapBandB;
            if (a.gap != b.gap) return a.gap < b.gap;
            if (a.screenY != b.screenY) return a.screenY < b.screenY;
            return a.screenX < b.screenX;
        }
        const int da = std::abs(a.page - gOrigin);
        const int db = std::abs(b.page - gOrigin);
        if (a.line != b.line) return a.line;
        if (da != db) return da < db;
        if (a.frame.y != b.frame.y) return a.frame.y < b.frame.y;
        return a.frame.x < b.frame.x;
    });
}

bool takeHotBakeLocked(BakeJob& out, bool onScreenOnly) {
    const bool allowNonLine = gDoneBytes < 64u * 1024u * 1024u;
    for (auto it = gBakeJobs.begin(); it != gBakeJobs.end(); ++it) {
        if (!it->hot) continue;
        if (onScreenOnly && !it->onScreen) continue;
        if (!it->line && !allowNonLine) continue;
        const bool big = isBigBake(it->frame);
        if (big && gBigBakeInflight >= 2) continue;
        out = std::move(*it);
        gBakeJobs.erase(it);
        ++gBakeInflight;
        if (big) ++gBigBakeInflight;
        return true;
    }
    return false;
}

bool takeAnyBakeLocked(BakeJob& out) {
    const bool allowNonLine = gDoneBytes < 64u * 1024u * 1024u;
    for (auto it = gBakeJobs.begin(); it != gBakeJobs.end(); ++it) {
        if (!it->line && !allowNonLine) continue;
        if (!keepPageLocked(it->page)) continue;
        const bool big = isBigBake(it->frame);
        if (big && gBigBakeInflight >= 2) continue;
        out = std::move(*it);
        gBakeJobs.erase(it);
        ++gBakeInflight;
        if (big) ++gBigBakeInflight;
        return true;
    }
    return false;
}

int pickHotPageLocked() {
    const int count = static_cast<int>(gClaimed.size());
    auto open = [&](int page) { return page >= 0 && page < count && !gClaimed[static_cast<std::size_t>(page)]; };
    if (gVisible.empty()) {
        if (open(gOrigin)) return gOrigin;
        return -1;
    }
    for (const int page : gVisible) {
        if (open(page)) return page;
    }
    return -1;
}

int pickPageLocked() {
    const int count = static_cast<int>(gClaimed.size());
    auto open = [&](int page) {
        return page >= 0 && page < count && !gClaimed[static_cast<std::size_t>(page)] && keepPageLocked(page);
    };
    if (open(gOrigin)) return gOrigin;
    for (const int page : gVisible) {
        if (open(page)) return page;
    }
    for (int step = 1; step < count; ++step) {
        if (open(gOrigin + step)) return gOrigin + step;
        if (open(gOrigin - step)) return gOrigin - step;
    }
    return -1;
}

void maybeTellLinesLocked() {
    if (gLineTold || gClaimed.empty() || !gLineJobs.empty() || gLineInflight > 0 || gScanInflight > 0) return;
    if (std::find(gClaimed.begin(), gClaimed.end(), 0) != gClaimed.end()) return;
    if (!gBaseScanned.empty() && std::find(gBaseScanned.begin(), gBaseScanned.end(), 0) != gBaseScanned.end()) return;
    for (const auto& item : gLineTally) {
        if (item.second.left > 0) return;
    }
    gLineTold = true;
    std::fprintf(stderr, "[ClassingHWP] 줄 전진폭을 다 적었다\n");
}

void noteBaseLineDoneLocked(int page) {
    const auto found = gLineTally.find(page);
    if (found == gLineTally.end()) return;
    if (found->second.left > 0) --found->second.left;
    if (found->second.left == 0 && found->second.total > 0 && !found->second.told) {
        found->second.told = true;
        std::fprintf(stderr, "[ClassingHWP] 스레드 %d 줄 %d · %d쪽 %d/%d\n", gCrewSize, found->second.total, page + 1,
                     found->second.total, found->second.total);
    }
    maybeTellLinesLocked();
}

void finishLineLocked(const LineJob& job) {
    gLineHold.erase(job.key);
    if (gLineInflight > 0) --gLineInflight;
    noteBaseLineDoneLocked(job.page);
}

bool jobInWindowLocked(const BakeJob& job) {
    if (job.percent == 100) return true;
    if (gScaleOnly || job.percent != gPercent) return false;
    if (gWindow.viewW <= 0 || gWindow.viewH <= 0) return false;
    const chwp::render::ViewPage* placed = nullptr;
    for (const chwp::render::ViewPage& item : gWindow.pages) {
        if (item.index == job.page) {
            placed = &item;
            break;
        }
    }
    if (!placed) return false;
    const int marginX = std::max(gWindow.viewW, 1);
    const int marginY = std::max(gWindow.viewH, 1);
    const int x = placed->x + job.frame.x;
    const int y = placed->y + job.frame.y;
    return x < gWindow.scrollX + gWindow.viewW + marginX && x + job.frame.w > gWindow.scrollX - marginX &&
           y < gWindow.scrollY + gWindow.viewH + marginY && y + job.frame.h > gWindow.scrollY - marginY;
}

void pruneZoomJobsLocked() {
    std::deque<BakeJob> keep;
    for (BakeJob& job : gBakeJobs) {
        if (jobInWindowLocked(job)) {
            keep.push_back(std::move(job));
        } else {
            gTaken.erase(job.frame.id);
        }
    }
    gBakeJobs.swap(keep);
}

void scanPage(int page, int percent, bool scaleOnly, bool baseAlreadyScanned, const std::vector<int>& visible,
              int origin, const ScreenWindow& window, int scanTick) {
    if (page < 0 || page >= static_cast<int>(gDoc.pages.size())) return;
    ensurePageSpans(page);
    if (page >= static_cast<int>(gDoc.pages.size())) return;
    const chwp::render::Node& scene = gDoc.pages[static_cast<std::size_t>(page)];
    const int pixelH = scene.height > 0 ? chwp::render::pagePixelHeight(scene.height, 100) : 0;
    const bool hot = isPageHot(page, visible, origin);
    const int zoom = percent > 0 ? percent : 100;
    const bool lineTileHere = !baseAlreadyScanned || (hot && (scaleOnly || zoom == 100));
    const bool needBase = !baseAlreadyScanned || lineTileHere;
    std::vector<chwp::render::FrameDesc> base;
    if (needBase && scene.height > 0 && pixelH >= 32) {
        base = gFrames.baseFrames(page, scene, &gDoc.doc);
    }
    const chwp::render::ViewPage* placed = nullptr;
    for (const chwp::render::ViewPage& item : window.pages) {
        if (item.index == page) {
            placed = &item;
            break;
        }
    }
    std::vector<chwp::render::FrameDesc> zoomFrames;
    if (hot && !scaleOnly && zoom != 100 && placed) {
        zoomFrames = chwp::render::framesOnScreen(scene, *placed, zoom, window.scrollX, window.scrollY, window.viewW,
                                                    window.viewH, &gDoc.doc);
        const int focusX = window.scrollX + window.viewW / 2 - placed->x;
        const int focusY = window.scrollY + window.viewH / 2 - placed->y;
        std::stable_sort(zoomFrames.begin(), zoomFrames.end(),
                         [&](const chwp::render::FrameDesc& a, const chwp::render::FrameDesc& b) {
                             const int aOn =
                                 (placed->x + a.x < window.scrollX + window.viewW && placed->x + a.x + a.w > window.scrollX &&
                                  placed->y + a.y < window.scrollY + window.viewH && placed->y + a.y + a.h > window.scrollY)
                                     ? 0
                                     : 1;
                             const int bOn =
                                 (placed->x + b.x < window.scrollX + window.viewW && placed->x + b.x + b.w > window.scrollX &&
                                  placed->y + b.y < window.scrollY + window.viewH && placed->y + b.y + b.h > window.scrollY)
                                     ? 0
                                     : 1;
                             if (aOn != bOn) return aOn < bOn;
                             const int da = std::abs((a.y + a.h / 2) - focusY) + std::abs((a.x + a.w / 2) - focusX) / 4;
                             const int db = std::abs((b.y + b.h / 2) - focusY) + std::abs((b.x + b.w / 2) - focusX) / 4;
                             return da < db;
                         });
    }
    std::unordered_set<std::string> zoomLines;
    for (const chwp::render::FrameDesc& frame : zoomFrames) {
        if (frame.kind == 0) zoomLines.insert(lineHoldKey(frame));
    }
    std::vector<LineJob> lines;
    int lineTotal = 0;
    for (const chwp::render::FrameDesc& frame : base) {
        if (frame.kind != 0) continue;
        ++lineTotal;
        if (lineTileHere || zoomLines.count(lineHoldKey(frame)) || chwp::render::linePlaceKept(frame)) continue;
        LineJob job;
        job.page = page;
        job.pixelH = pixelH;
        job.frame = frame;
        job.key = lineHoldKey(job.frame);
        lines.push_back(std::move(job));
    }
    std::vector<BakeJob> bakes;
    auto queueFrame = [&](chwp::render::FrameDesc frame, int framePercent, int total, bool frameHot, bool frameLine) {
        frame.percent = framePercent;
        if (!chwp::render::canBakeFrame(frame)) return;
        const std::string key = zoomBakeKey(frame);
        gFrames.assign(frame);
        if (gFrames.contains(frame.id)) return;
        BakeJob job;
        job.frame = std::move(frame);
        job.page = page;
        job.percent = framePercent;
        job.total = total;
        job.hot = frameHot;
        job.line = frameLine;
        job.key = key;
        bakes.push_back(std::move(job));
    };
    int baseTotal = 0;
    for (const chwp::render::FrameDesc& frame : base) {
        if (frame.kind != 0) ++baseTotal;
    }
    int zoomTotal = 0;
    int zoomLineTotal = 0;
    for (chwp::render::FrameDesc& frame : zoomFrames) {
        frame.percent = zoom;
        if (!chwp::render::canBakeFrame(frame)) continue;
        if (frame.kind == 0) ++zoomLineTotal;
        else ++zoomTotal;
    }
    for (chwp::render::FrameDesc& frame : zoomFrames) {
        frame.percent = zoom;
        if (!chwp::render::canBakeFrame(frame)) continue;
        if (frame.kind == 0) queueFrame(std::move(frame), zoom, zoomLineTotal, true, true);
        else queueFrame(std::move(frame), zoom, zoomTotal, true, false);
    }
    for (const chwp::render::FrameDesc& frame : base) {
        if (frame.kind == 0) {
            if (lineTileHere) queueFrame(frame, 100, lineTotal, hot, true);
            continue;
        }
        if (!baseAlreadyScanned) {
            queueFrame(frame, 100, baseTotal, hot, false);
        }
    }
    std::lock_guard lock(gWorkMu);
    if (gCrewStop || scanTick != gScanTick) {
        if (page >= 0 && page < static_cast<int>(gClaimed.size()) &&
            (page >= static_cast<int>(gBaseScanned.size()) || !gBaseScanned[static_cast<std::size_t>(page)])) {
            gClaimed[static_cast<std::size_t>(page)] = 0;
        }
        return;
    }
    if (needBase && page >= 0 && page < static_cast<int>(gBaseScanned.size())) {
        gBaseScanned[static_cast<std::size_t>(page)] = 1;
    }
    if (!placed && isPageHot(page, gVisible, gOrigin) && !gScaleOnly && gPercent != 100 && gWindow.viewW > 0 &&
        gWindow.viewH > 0 && page >= 0 && page < static_cast<int>(gClaimed.size())) {
        gClaimed[static_cast<std::size_t>(page)] = 0;
    }
    LineTally& tally = gLineTally[page];
    if (tally.total < lineTotal) tally.total = lineTotal;
    for (LineJob& job : lines) {
        if (!gLineHold.insert(job.key).second) continue;
        ++tally.left;
        gLineJobs.push_back(std::move(job));
    }
    bool addedBake = false;
    for (BakeJob& job : bakes) {
        if (job.percent != 100 && gZoomSkip.count(job.key)) continue;
        if (gTaken.count(job.frame.id)) continue;
        if (job.line && job.percent == 100) ++tally.left;
        gTaken[job.frame.id] = Taken{job.percent, job.page, job.key};
        gBakeJobs.push_back(std::move(job));
        addedBake = true;
    }
    if (addedBake) reprioritizeBakesLocked();
    if (tally.left == 0 && tally.total > 0 && !tally.told) {
        tally.told = true;
        std::fprintf(stderr, "[ClassingHWP] 스레드 %d 줄 %d · %d쪽 %d/%d\n", gCrewSize, tally.total, page + 1, tally.total,
                     tally.total);
    }
    maybeTellLinesLocked();
    gWorkCv.notify_all();
}

enum class CrewStep { Did, Idle, Stop };

// 일꾼 한 걸음: 일을 하나 골라 한다. 일꾼 스레드는 이 걸음을 되풀이하고, 할 일이 없으면 잔다.
// 일꾼 스레드가 없으면(solo) 화면 스레드가 프레임마다 예산만큼 부른다. 기다릴 때는 자지 않고 바로 돌아온다.
// 무엇을 어떤 차례로 할지는 스레드가 있든 없든 이 한 갈래로 정한다.
CrewStep crewStep(int& seen, bool solo) {
    LineJob line;
    BakeJob bake;
    std::string pictureId;
    int kind = 0;
    {
        std::unique_lock lock(gWorkMu);
        for (;;) {
            if (gCrewStop) return CrewStep::Stop;
            if (gCrewPaused) {
                if (solo) return CrewStep::Idle;
                gWorkCv.wait(lock, [&] { return gCrewStop || !gCrewPaused; });
                seen = gTick;
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now < gCrewEditHoldUntil) {
                if (solo) return CrewStep::Idle;
                gWorkCv.wait_until(lock, gCrewEditHoldUntil, [&] { return gCrewStop || gCrewPaused; });
                seen = gTick;
                continue;
            }
            const bool userBusy = gUserInteracting.load(std::memory_order_relaxed);
            if (userBusy) {
                if (solo) return CrewStep::Idle;
                gWorkCv.wait(lock, [&] {
                    return gCrewStop || gCrewPaused || !gUserInteracting.load(std::memory_order_relaxed) ||
                           gTick != seen;
                });
                seen = gTick;
                continue;
            }
            if (takeHotBakeLocked(bake, true)) {
                kind = 3;
                break;
            }
            const int hotPage = pickHotPageLocked();
            if (hotPage >= 0) {
                gClaimed[static_cast<std::size_t>(hotPage)] = 1;
                const bool baseDone = hotPage < static_cast<int>(gBaseScanned.size()) &&
                                      gBaseScanned[static_cast<std::size_t>(hotPage)] != 0;
                const int percent = gPercent;
                const bool scale = gScaleOnly;
                const int scanTick = gScanTick;
                const std::vector<int> visible = gVisible;
                const int origin = gOrigin;
                const ScreenWindow window = gWindow;
                ++gScanInflight;
                lock.unlock();
                try {
                    scanPage(hotPage, percent, scale, baseDone, visible, origin, window, scanTick);
                } catch (const std::exception& ex) {
                    noteFailure("보이는 쪽 준비", &ex);
                } catch (...) {
                    noteFailure("보이는 쪽 준비");
                }
                lock.lock();
                if (gScanInflight > 0) --gScanInflight;
                if (gCrewPaused || gCrewStop) gWorkCv.notify_all();
                if (solo) return CrewStep::Did;
                continue;
            }
            if (takeHotBakeLocked(bake, false)) {
                kind = 3;
                break;
            }
            if (!gGlyphsNoted && !gGlyphsRunning) {
                gGlyphsRunning = true;
                lock.unlock();
                try {
                    chwp::render::noteDocumentGlyphs(gDoc.doc);
                } catch (const std::exception& ex) {
                    noteFailure("글자 기록", &ex);
                } catch (...) {
                    noteFailure("글자 기록");
                }
                lock.lock();
                gGlyphsRunning = false;
                if (gCrewStop) {
                    gWorkCv.notify_all();
                    return CrewStep::Stop;
                }
                gGlyphsNoted = true;
                ++gTick;
                gWorkCv.notify_all();
                if (solo) return CrewStep::Did;
                continue;
            }
            const bool picturesLeft = gPictureCursor < static_cast<int>(gPictureIds.size());
            int scanCandidate = -1;
            bool took = false;
            for (int step = 0; step < 4 && !took; ++step) {
                const int which = (gTurn + step) % 4;
                if (which == 0 && picturesLeft) {
                    pictureId = gPictureIds[static_cast<std::size_t>(gPictureCursor++)];
                    ++gPictureInflight;
                    kind = 4;
                    took = true;
                } else if (which == 1) {
                    if (!gLineJobs.empty()) {
                        line = std::move(gLineJobs.front());
                        gLineJobs.pop_front();
                        ++gLineInflight;
                        kind = 1;
                        took = true;
                    } else {
                        scanCandidate = pickPageLocked();
                        if (scanCandidate >= 0) {
                            took = true;
                        }
                    }
                } else if (which == 2 && gGlyphsNoted && !gMasksDone && !gMaskBusy) {
                    gMaskBusy = true;
                    kind = 2;
                    took = true;
                } else if (which == 3 && takeAnyBakeLocked(bake)) {
                    kind = 3;
                    took = true;
                }
            }
            if (took && scanCandidate >= 0) {
                ++gTurn;
                gClaimed[static_cast<std::size_t>(scanCandidate)] = 1;
                const bool baseDone = scanCandidate < static_cast<int>(gBaseScanned.size()) &&
                                      gBaseScanned[static_cast<std::size_t>(scanCandidate)] != 0;
                const int percent = gPercent;
                const bool scale = gScaleOnly;
                const int scanTick = gScanTick;
                const std::vector<int> visible = gVisible;
                const int origin = gOrigin;
                const ScreenWindow window = gWindow;
                ++gScanInflight;
                lock.unlock();
                try {
                    scanPage(scanCandidate, percent, scale, baseDone, visible, origin, window, scanTick);
                } catch (const std::exception& ex) {
                    noteFailure("쪽 준비", &ex);
                } catch (...) {
                    noteFailure("쪽 준비");
                }
                lock.lock();
                if (gScanInflight > 0) --gScanInflight;
                if (gCrewPaused || gCrewStop) gWorkCv.notify_all();
                if (solo) return CrewStep::Did;
                continue;
            }
            if (took) {
                ++gTurn;
                break;
            }
            const int page = pickPageLocked();
            if (page >= 0) {
                gClaimed[static_cast<std::size_t>(page)] = 1;
                const bool baseDone = page < static_cast<int>(gBaseScanned.size()) &&
                                      gBaseScanned[static_cast<std::size_t>(page)] != 0;
                const int percent = gPercent;
                const bool scale = gScaleOnly;
                const int scanTick = gScanTick;
                const std::vector<int> visible = gVisible;
                const int origin = gOrigin;
                const ScreenWindow window = gWindow;
                ++gScanInflight;
                lock.unlock();
                try {
                    scanPage(page, percent, scale, baseDone, visible, origin, window, scanTick);
                } catch (const std::exception& ex) {
                    noteFailure("쪽 준비", &ex);
                } catch (...) {
                    noteFailure("쪽 준비");
                }
                lock.lock();
                if (gScanInflight > 0) --gScanInflight;
                if (gCrewPaused || gCrewStop) gWorkCv.notify_all();
                if (solo) return CrewStep::Did;
                continue;
            }
            maybeTellLinesLocked();
            if (solo) return CrewStep::Idle;
            gWorkCv.wait(lock, [&] { return gCrewStop || gCrewPaused || gTick != seen; });
            seen = gTick;
        }
    }
    if (kind == 1) {
        if (line.page >= 0 && line.page < static_cast<int>(gDoc.pages.size())) {
            try {
                chwp::render::writeLinePlace(gDoc.doc, gDoc.pages[static_cast<std::size_t>(line.page)], line.frame,
                                               line.pixelH);
            } catch (const std::exception& ex) {
                noteFailure("줄 자리", &ex);
            } catch (...) {
                noteFailure("줄 자리");
            }
        }
        std::lock_guard lock(gWorkMu);
        if (gCrewStop) {
            if (gLineInflight > 0) --gLineInflight;
            gWorkCv.notify_all();
            return CrewStep::Stop;
        }
        finishLineLocked(line);
        if (gCrewPaused) gWorkCv.notify_all();
    } else if (kind == 2) {
        int targetMaskZoom = 100;
        {
            std::lock_guard lock(gWorkMu);
            targetMaskZoom = chwp::render::restingMaskZoom(gPercent);
        }
        chwp::render::MaskBake masks;
        try {
            masks = chwp::render::bakeDocumentMasks(gDoc.doc, 48, targetMaskZoom);
        } catch (const std::exception& ex) {
            noteFailure("글자 마스크", &ex);
        } catch (...) {
            noteFailure("글자 마스크");
        }
        std::lock_guard lock(gWorkMu);
        gMaskBusy = false;
        if (!masks.more) gMasksDone = true;
        if (gCrewStop) {
            gWorkCv.notify_all();
            return CrewStep::Stop;
        }
        ++gTick;
        gWorkCv.notify_all();
    } else if (kind == 4) {
        if (!pictureId.empty()) {
            try {
                chwp::render::warmPicture(gDoc.doc, pictureId);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "[ClassingHWP 오류] 그림 디코드 %s: %s\n", pictureId.c_str(), ex.what());
            } catch (...) {
                std::fprintf(stderr, "[ClassingHWP 오류] 그림 디코드 %s\n", pictureId.c_str());
            }
        }
        std::lock_guard lock(gWorkMu);
        if (gPictureInflight > 0) --gPictureInflight;
        if (gCrewStop) {
            gWorkCv.notify_all();
            return CrewStep::Stop;
        }
        ++gTick;
        gWorkCv.notify_all();
    } else if (kind == 3) {
        const bool big = isBigBake(bake.frame);
        chwp::render::PageBitmap bitmap;
        if (bake.page >= 0 && bake.page < static_cast<int>(gDoc.pages.size()) &&
            chwp::render::canBakeFrame(bake.frame)) {
            try {
                bitmap =
                    chwp::render::renderFrame(gDoc.doc, gDoc.pages[static_cast<std::size_t>(bake.page)], bake.frame);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "[ClassingHWP 오류] 칸 그리기 %d쪽 %dx%d: %s\n", bake.page + 1, bake.frame.w,
                             bake.frame.h, ex.what());
                bitmap = {};
            } catch (...) {
                std::fprintf(stderr, "[ClassingHWP 오류] 칸 그리기 %d쪽 %dx%d\n", bake.page + 1, bake.frame.w,
                             bake.frame.h);
                bitmap = {};
            }
        }
        std::lock_guard lock(gWorkMu);
        if (gBakeInflight > 0) --gBakeInflight;
        if (big && gBigBakeInflight > 0) --gBigBakeInflight;
        if (gCrewStop) {
            gWorkCv.notify_all();
            return CrewStep::Stop;
        }
        const bool hot = isPageHot(bake.page, gVisible, gOrigin);
        const bool keep = bake.percent == 100 || (!gScaleOnly && bake.percent == gPercent && hot);
        if (!keep || !chwp::render::canBakeFrame(bake.frame)) {
            gTaken.erase(bake.frame.id);
            if (bake.line && bake.percent == 100) noteBaseLineDoneLocked(bake.page);
            if (big || gCrewPaused) {
                ++gTick;
                gWorkCv.notify_all();
            }
            return CrewStep::Did;
        }
        if (bake.line && bake.percent == 100) noteBaseLineDoneLocked(bake.page);
        gDoneBytes += bitmap.rgba.size();
        BakeDone done;
        done.frame = std::move(bake.frame);
        done.page = bake.page;
        done.percent = bake.percent;
        done.total = bake.total;
        done.bitmap = std::move(bitmap);
        gDone.push_back(std::move(done));
        if (big || gCrewPaused) {
            ++gTick;
            gWorkCv.notify_all();
        }
    }
    return CrewStep::Did;
}

void crewMain() {
    int seen = 0;
    {
        std::lock_guard lock(gWorkMu);
        seen = gTick;
    }
    while (crewStep(seen, false) != CrewStep::Stop) {
    }
}

bool crewPausedNow() {
    std::lock_guard lock(gWorkMu);
    return gCrewPaused;
}

// 일꾼이 오래 걸리는 일(그림 풀기, 글자 마스크)을 하는 중인가.
bool crewBusyNow() {
    std::lock_guard lock(gWorkMu);
    return gPictureInflight > 0 || gMaskBusy || gGlyphsRunning;
}

// 쪽만 붙인 뒤 일꾼을 다시 돌린다. 지운 칸은 없다.
void resumeCrewQuietly() { resumeCrewAfterEdit(chwp::render::EditResult{}); }

// 쌓기가 붙인 쪽을 화면 쪽에 알린다. 일꾼이 멈춘 동안 부른다. 쪽 주소가 바뀌었으면 처음부터 맞춘다.
void syncLaidPages() {
    if (gDoc.takePagesMoved()) {
        syncFullRelayout(false);
        return;
    }
    const std::size_t count = gDoc.pages.size();
    if (gPagePng.size() < count) gPagePng.resize(count);
    if (gPagePixels.size() < count) gPagePixels.resize(count);
    {
        std::lock_guard lock(gWorkMu);
        if (gClaimed.size() < count) gClaimed.resize(count, 0);
        if (gBaseScanned.size() < count) gBaseScanned.resize(count, 0);
    }
    // 앞의 끝 쪽 아래 여백이 바뀌니 쪽 배치를 다시 넣는다. 보던 자리는 그대로 둔다.
    const chwp::render::ViewportState prev = gCamera.state();
    mountCameraFromPages();
    gCamera.setScroll(prev.scrollX, prev.scrollY);
    gCameraNoteDue = true;
}

// 남은 쪽을 다 쌓고 화면에 알린다. 문서 전체 쪽이 필요한 일 앞에서 DocSession 이 부른다.
// 이미 일꾼을 멈춘 편집 안에서 불리면 멈춤·다시 돌리기는 그 편집에 맡긴다.
void completeLayoutOnScreen() {
    if (gDoc.layoutDone()) return;
    const bool pause = crewRunning() && !crewPausedNow();
    if (pause) pauseCrewForEdit();
    gDoc.finishLayout();
    syncLaidPages();
    if (pause) resumeCrewQuietly();
}

// 쪽 index 부터 한 화면이 쌓여 있게 한다. 쪽 명령(page=N, 다음 쪽)이 아직 쌓지 않은 쪽을 가리킬 때 쓴다.
void layoutThroughPage(int index) {
    if (gDoc.layoutDone() || index < 0) return;
    const int want = index + 1 + kPageLookahead;
    if (static_cast<int>(gDoc.pages.size()) >= want) return;
    const bool pause = crewRunning() && !crewPausedNow();
    if (pause) pauseCrewForEdit();
    gDoc.layoutUntil(want);
    syncLaidPages();
    if (pause) resumeCrewQuietly();
}

// 열 때 다 쌓지 못한 쪽을 프레임마다 조금씩 쌓는다. 일꾼이 쪽을 읽으니 멈춘 동안 쌓는다.
// 일꾼이 그림을 푸는 중이면 기다리지 않고 다음 프레임으로 미룬다(몇 프레임 넘게 밀리면 기다려 쌓는다).
void continueLayout(bool busy) {
    if (gDoc.layoutDone()) return;
    static int waited = 0;
    if (crewRunning() && crewBusyNow() && waited < kLayoutWaitFrames) {
        ++waited;
        return;
    }
    waited = 0;
    const bool pause = crewRunning() && !crewPausedNow();
    if (pause) pauseCrewForEdit();
    gDoc.layoutStep(busy ? kLayoutBusyMs : kLayoutIdleMs);
    syncLaidPages();
    if (pause) resumeCrewQuietly();
}


void pauseCrewForEdit() {
    if (!crewRunning()) return;
    std::unique_lock lock(gWorkMu);
    gCrewPaused = true;
    ++gScanTick;
    ++gTick;
    gWorkCv.notify_all();
    gWorkCv.wait(lock, [&] {
        return gScanInflight == 0 && gLineInflight == 0 && gBakeInflight == 0 &&
               gPictureInflight == 0 && !gMaskBusy && !gGlyphsRunning;
    });
}

void resumeCrewAfterEdit(const chwp::render::EditResult& edited) {
    if (!crewRunning()) return;
    // 편집 뒤 캐럿에 맞춰 누름틀 안내문을 숨기거나 보인다. 일꾼이 멈춘 지금 한다. 그랬으면 일감을 처음부터 다시 본다.
    chwp::render::EditResult res = edited;
    if (syncClickHereGuidePaused().documentChanged) {
        res.documentChanged = true;
        res.fullInvalidation = true;
    }
    {
        std::lock_guard lock(gWorkMu);
        if (res.fullInvalidation) {
            gBakeJobs.clear();
            gLineJobs.clear();
            gDone.clear();
            gTaken.clear();
            gPictureRetry.clear();
            gZoomSkip.clear();
            gLineHold.clear();
            gLineTally.clear();
            gClaimed.assign(gDoc.pages.size(), 0);
            gBaseScanned.assign(gDoc.pages.size(), 0);
            gDoneBytes = 0;
            gLineTold = false;
            ++gScanTick;
        } else if (res.documentChanged) {
            if (res.pagesMoved) {
                gBakeJobs.clear();
                gLineJobs.clear();
                gDone.clear();
                gTaken.clear();
                gLineHold.clear();
                gPictureRetry.clear();
                gZoomSkip.clear();
                gDoneBytes = 0;
            }
            if (res.pageShiftDelta != 0 && res.pageShiftFrom >= 0) {
                auto shiftFlat = [&](auto& rows) {
                    if (res.pageShiftDelta > 0) {
                        const int from = std::min(res.pageShiftFrom, static_cast<int>(rows.size()));
                        rows.insert(rows.begin() + from, static_cast<std::size_t>(res.pageShiftDelta),
                                    typename std::decay_t<decltype(rows)>::value_type{});
                    } else {
                        const int from = std::min(res.pageShiftFrom, static_cast<int>(rows.size()));
                        const int count = std::min(-res.pageShiftDelta, static_cast<int>(rows.size()) - from);
                        if (count > 0) rows.erase(rows.begin() + from, rows.begin() + from + count);
                    }
                };
                shiftFlat(gClaimed);
                shiftFlat(gBaseScanned);
            }
            if (gClaimed.size() < gDoc.pages.size()) gClaimed.resize(gDoc.pages.size(), 0);
            if (gBaseScanned.size() < gDoc.pages.size()) gBaseScanned.resize(gDoc.pages.size(), 0);
            if (res.pageShiftFrom >= 0) {
                for (int i = res.pageShiftFrom; i < static_cast<int>(gClaimed.size()); ++i) {
                    gClaimed[static_cast<std::size_t>(i)] = 0;
                }
                for (int i = res.pageShiftFrom; i < static_cast<int>(gBaseScanned.size()); ++i) {
                    gBaseScanned[static_cast<std::size_t>(i)] = 0;
                }
            }
            std::unordered_set<int> dirtyPages;
            bool allPagesDirty = false;
            for (const auto& inv : res.invalidations) {
                if (inv.page < 0) {
                    allPagesDirty = true;
                } else {
                    dirtyPages.insert(inv.page);
                }
            }
            if (allPagesDirty) {
                std::fill(gClaimed.begin(), gClaimed.end(), 0);
                std::fill(gBaseScanned.begin(), gBaseScanned.end(), 0);
            } else {
                for (int p : dirtyPages) {
                    if (p >= 0 && p < static_cast<int>(gClaimed.size())) {
                        gClaimed[static_cast<std::size_t>(p)] = 0;
                    }
                    if (p >= 0 && p < static_cast<int>(gBaseScanned.size())) {
                        gBaseScanned[static_cast<std::size_t>(p)] = 0;
                    }
                }
            }
            gZoomSkip.clear();
            std::deque<LineJob> keptLines;
            for (LineJob& job : gLineJobs) {
                if (!allPagesDirty && !dirtyPages.count(job.page)) {
                    keptLines.push_back(std::move(job));
                } else {
                    gLineHold.erase(job.key);
                }
            }
            gLineJobs.swap(keptLines);
            std::vector<BakeDone> keptDone;
            gDoneBytes = 0;
            for (BakeDone& done : gDone) {
                if (!allPagesDirty && !dirtyPages.count(done.page)) {
                    gDoneBytes += done.bitmap.rgba.size();
                    keptDone.push_back(std::move(done));
                } else {
                    gTaken.erase(done.frame.id);
                }
            }
            gDone.swap(keptDone);
            std::deque<BakeJob> keptBakes;
            std::unordered_set<int> queuedIds;
            for (BakeJob& job : gBakeJobs) {
                if (!allPagesDirty && !dirtyPages.count(job.page)) {
                    queuedIds.insert(job.frame.id);
                    keptBakes.push_back(std::move(job));
                }
            }
            gBakeJobs.swap(keptBakes);
            for (auto it = gTaken.begin(); it != gTaken.end();) {
                if (!gFrames.contains(it->first) && !queuedIds.count(it->first)) {
                    it = gTaken.erase(it);
                } else {
                    ++it;
                }
            }
            ++gScanTick;
        }
        gCrewPaused = false;
        if (res.documentChanged || res.composing) {
            gCrewEditHoldUntil =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(res.composing ? 360 : 220);
        }
        ++gTick;
    }
    gWorkCv.notify_all();
}

void stopCrew() {
    {
        std::lock_guard lock(gWorkMu);
        gCrewStop = true;
        gCrewLive.store(false);
    }
    gWorkCv.notify_all();
    for (std::thread& worker : gCrew) {
        if (worker.joinable()) worker.join();
    }
    gCrew.clear();
    std::lock_guard lock(gWorkMu);
    gCrewStop = false;
    gCrewPaused = false;
    gCrewEditHoldUntil = {};
    gScanInflight = 0;
    gBakeJobs.clear();
    gLineJobs.clear();
    gDone.clear();
    gTaken.clear();
    gPictureRetry.clear();
    gZoomSkip.clear();
    gWindow = {};
    gLineHold.clear();
    gLineTally.clear();
    gClaimed.clear();
    gBaseScanned.clear();
    gVisible.clear();
    gLineInflight = 0;
    gBakeInflight = 0;
    gBigBakeInflight = 0;
    gDoneBytes = 0;
    gMaskBusy = false;
    gMasksDone = false;
    gGlyphsNoted = false;
    gGlyphsRunning = false;
    gLineTold = false;
    gPictureIds.clear();
    gPictureCursor = 0;
    gPictureInflight = 0;
    gTurn = 0;
    gOrigin = 0;
    gKeepFirst = -1;
    gKeepLast = -1;
    gKeepRadius = -1;
    gPercent = 100;
    gScaleOnly = false;
    ++gScanTick;
    ++gTick;
}

// 칸 예산을 지킨다. 칸을 버린 쪽은 일꾼이 처음처럼 다시 훑고, 반경이 바뀌면 일꾼을 깨운다.
void applyTileBudget(bool baked) {
    const bool radiusChanged = baked && gFrames.keepWithinBudget();
    const std::vector<int> evicted = gFrames.takeEvictedPages();
    if (!radiusChanged && evicted.empty()) return;
    std::lock_guard lock(gWorkMu);
    if (!evicted.empty()) {
        const std::unordered_set<int> pages(evicted.begin(), evicted.end());
        for (const int page : evicted) {
            if (page >= 0 && page < static_cast<int>(gClaimed.size())) gClaimed[static_cast<std::size_t>(page)] = 0;
            if (page >= 0 && page < static_cast<int>(gBaseScanned.size())) {
                gBaseScanned[static_cast<std::size_t>(page)] = 0;
            }
        }
        // 구워 둔 칸의 번호는 남아 있어 다시 훑어도 일감을 안 만든다. 아직 줄 선 일감만 남긴다.
        std::unordered_set<int> queued;
        for (const BakeJob& job : gBakeJobs) queued.insert(job.frame.id);
        for (auto it = gTaken.begin(); it != gTaken.end();) {
            if (pages.count(it->second.page) && !queued.count(it->first)) it = gTaken.erase(it);
            else ++it;
        }
    }
    gKeepRadius = gFrames.keepRadius();
    ++gTick;
    gWorkCv.notify_all();
}

void drainDone() {
    const chwp::render::FramePhaseTimer phaseTimer(chwp::render::FramePhase::Drain);
    std::vector<BakeDone> batch;
    {
        std::lock_guard lock(gWorkMu);
        batch.swap(gDone);
        if (gDoneBytes > 0) {
            gDoneBytes = 0;
            ++gTick;
            gWorkCv.notify_all();
        }
    }
    if (batch.empty()) {
        applyTileBudget(false);
        return;
    }
    int cells = 0;
    std::vector<BakeDone> lines;
    std::vector<BakeDone> again;
    for (BakeDone& item : batch) {
        if (item.bitmap.incomplete && gPictureRetry.insert(item.frame.id).second) {
            again.push_back(std::move(item));
            continue;
        }
        if (item.frame.kind == 0) {
            lines.push_back(std::move(item));
            continue;
        }
        ++cells;
        gFrames.keepBaked(item.frame, item.page, std::move(item.bitmap));
    }
    for (BakeDone& item : lines) gFrames.keepBaked(item.frame, item.page, std::move(item.bitmap));
    applyTileBudget(true);
    if (!again.empty()) {
        std::lock_guard lock(gWorkMu);
        for (BakeDone& item : again) {
            BakeJob job;
            job.frame = std::move(item.frame);
            job.page = item.page;
            job.percent = item.percent;
            job.total = item.total;
            job.hot = isPageHot(job.page, gVisible, gOrigin);
            job.line = job.frame.kind == 0;
            job.key = zoomBakeKey(job.frame);
            gBakeJobs.push_front(std::move(job));
        }
        reprioritizeBakesLocked();
        ++gTick;
        gWorkCv.notify_all();
    }
    if (cells == 0) return;
    std::string line = "[ClassingHWP] 스레드 " + std::to_string(gCrewSize) + " 칸 " + std::to_string(cells);
    std::vector<int> seenPages;
    std::vector<int> seenPercents;
    for (const BakeDone& item : batch) {
        if (item.frame.kind == 0) continue;
        bool seen = false;
        for (int i = 0; i < static_cast<int>(seenPages.size()); ++i) {
            if (seenPages[static_cast<std::size_t>(i)] == item.page && seenPercents[static_cast<std::size_t>(i)] == item.percent) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        seenPages.push_back(item.page);
        seenPercents.push_back(item.percent);
        line += " · " + std::to_string(item.page + 1) + "쪽 " + std::to_string(gFrames.countOnPage(item.page, item.percent)) +
                "/" + std::to_string(item.total);
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}

void publishPlan() {
    std::vector<int> visible;
    std::vector<int> prefetch;
    int onScreenCount = 0;
    const int prefetchMargin = std::max(gLastPlan.viewH / 2, 320);
    for (const chwp::render::ViewPage& page : gLastPlan.pages) {
        if (pageOnScreen(page, gLastPlan)) {
            visible.push_back(page.index);
            ++onScreenCount;
            continue;
        }
        if (prefetchMargin > 0 && page.w > 0 && page.h > 0 &&
            page.x < gLastPlan.scrollX + gLastPlan.viewW + prefetchMargin &&
            page.x + page.w > gLastPlan.scrollX - prefetchMargin &&
            page.y < gLastPlan.scrollY + gLastPlan.viewH + prefetchMargin &&
            page.y + page.h > gLastPlan.scrollY - prefetchMargin) {
            prefetch.push_back(page.index);
        }
    }
    std::sort(visible.begin(), visible.end());
    visible.erase(std::unique(visible.begin(), visible.end()), visible.end());
    std::sort(prefetch.begin(), prefetch.end());
    prefetch.erase(std::unique(prefetch.begin(), prefetch.end()), prefetch.end());
    for (const int p : prefetch) {
        if (std::find(visible.begin(), visible.end(), p) == visible.end()) {
            visible.push_back(p);
        }
    }
    const bool pinching = gInput.pinchActive();
    const bool activelyMoving =
        gIdleMoveStreak < 2 ||
        (gFrameClock > 0 && gLastCameraMoveTime > 0 && (gFrameClock - gLastCameraMoveTime) < 36.0);
    const bool pauseForPinch = pinching && activelyMoving;
    const int percent = pauseForPinch ? 100 : (gLastPlan.percent > 0 ? gLastPlan.percent : 100);
    const bool scale = pauseForPinch || percent < 100 || chwp::render::overviewZoom(onScreenCount, percent);
    const int origin = idleOriginPage(gLastPlan);
    int firstShown = -1;
    int lastShown = -1;
    for (const chwp::render::ViewPage& page : gLastPlan.pages) {
        if (!pageOnScreen(page, gLastPlan)) continue;
        firstShown = firstShown < 0 ? page.index : std::min(firstShown, page.index);
        lastShown = std::max(lastShown, page.index);
    }
    gFrames.setFocus(firstShown, lastShown);
    std::lock_guard lock(gWorkMu);
    gKeepFirst = firstShown;
    gKeepLast = lastShown;
    gKeepRadius = gFrames.keepRadius();
    if (gClaimed.size() != gDoc.pages.size()) gClaimed.assign(gDoc.pages.size(), 0);
    if (gBaseScanned.size() != gDoc.pages.size()) gBaseScanned.assign(gDoc.pages.size(), 0);
    const bool percentChanged = percent != gPercent || scale != gScaleOnly;
    const bool visibleChanged = visible != gVisible;
    const bool viewMoved = gLastPlan.scrollX != gWindow.scrollX || gLastPlan.scrollY != gWindow.scrollY ||
                           gLastPlan.viewW != gWindow.viewW || gLastPlan.viewH != gWindow.viewH;
    gWindow.scrollX = gLastPlan.scrollX;
    gWindow.scrollY = gLastPlan.scrollY;
    gWindow.viewW = gLastPlan.viewW;
    gWindow.viewH = gLastPlan.viewH;
    gWindow.pages = gLastPlan.pages;
    gOrigin = origin;
    gPercent = percent;
    gScaleOnly = scale;
    if (viewMoved || percentChanged) gZoomSkip.clear();
    for (const int id : gLastPlan.drop) {
        const auto found = gTaken.find(id);
        if (found == gTaken.end()) continue;
        const int page = found->second.page;
        if (!viewMoved && !percentChanged && found->second.percent != 100 && !found->second.key.empty()) {
            gZoomSkip.insert(found->second.key);
        }
        gTaken.erase(found);
        if (page >= 0 && page < static_cast<int>(gClaimed.size())) {
            const bool baseDone =
                page < static_cast<int>(gBaseScanned.size()) && gBaseScanned[static_cast<std::size_t>(page)] != 0;
            if (!baseDone || (!gScaleOnly && gPercent != 100)) {
                gClaimed[static_cast<std::size_t>(page)] = 0;
            }
        }
    }
    if (!gLastPlan.drop.empty()) {
        std::deque<BakeJob> keep;
        for (BakeJob& job : gBakeJobs) {
            if (gTaken.count(job.frame.id)) keep.push_back(std::move(job));
        }
        gBakeJobs.swap(keep);
    }
    if (percentChanged) {
        gMasksDone = false;
        std::deque<BakeJob> keep;
        for (BakeJob& job : gBakeJobs) {
            if (job.percent == 100 || (!scale && job.percent == percent)) keep.push_back(std::move(job));
            else gTaken.erase(job.frame.id);
        }
        gBakeJobs.swap(keep);
        for (auto it = gTaken.begin(); it != gTaken.end();) {
            if (it->second.percent != 100 && (scale || it->second.percent != percent)) it = gTaken.erase(it);
            else ++it;
        }
        if (pauseForPinch || scale || percent == 100) {
            gClaimed = gBaseScanned;
        } else {
            std::fill(gClaimed.begin(), gClaimed.end(), 0);
        }
        ++gScanTick;
    } else if (visibleChanged || viewMoved) {
        if (!pauseForPinch) {
            pruneZoomJobsLocked();
            for (const int page : visible) {
                if (page < 0 || page >= static_cast<int>(gClaimed.size())) continue;
                const bool baseDone =
                    page < static_cast<int>(gBaseScanned.size()) && gBaseScanned[static_cast<std::size_t>(page)] != 0;
                if (baseDone && (gScaleOnly || gPercent == 100)) continue;
                gClaimed[static_cast<std::size_t>(page)] = 0;
            }
        }
    }
    gVisible = std::move(visible);
    reprioritizeBakesLocked();
    ++gTick;
    gWorkCv.notify_all();
}

bool crewRunning() { return gCrewLive.load(); }

int crewThreads() { return crewRunning() ? gCrewSize : 0; }

// 일꾼 스레드가 없을 때 화면 스레드가 일꾼 걸음을 예산만큼 밟는다. 손이 움직이는 동안은 일꾼처럼 쉰다.
void runSoloCrew(double budgetMs) {
    const chwp::render::FramePhaseTimer phaseTimer(chwp::render::FramePhase::Crew);
    if (!crewRunning() || gCrewSize > 0) return;
    const double start = frameNow();
    int seen = 0;
    while (crewStep(seen, true) == CrewStep::Did && frameNow() - start < budgetMs) {
    }
}

bool stepSingleThreadForeground() {
    if (crewRunning() || gDoc.pages.empty()) return false;
    if (gLastPlan.job.id == 0 || gLastPlan.job.background) return false;
    const int jobPage = gLastPlan.job.page;
    if (jobPage < 0 || jobPage >= static_cast<int>(gDoc.pages.size())) return false;
    const int jobPercent =
        gLastPlan.job.percent > 0 ? gLastPlan.job.percent : (gLastPlan.percent > 0 ? gLastPlan.percent : 100);
    const chwp::render::FramePrepare made =
        gFrames.prepare(gDoc.doc, gDoc.pages, jobPage, jobPercent, gLastPlan.job.y);
    return made.made > 0;
}

// 화면에 걸친 칸 중 없는 것을 예산 안에서 굽는다. 하나라도 구웠으면 참이다.
bool stepSingleThreadView(double budgetMs) {
    const chwp::render::FramePhaseTimer phaseTimer(chwp::render::FramePhase::ViewBake);
    if (crewThreads() > 0 || gDoc.pages.empty()) return false;
    const double start = frameNow();
    bool made = false;
    do {
        if (gFrames.prepareView(gDoc.doc, gDoc.pages, gLastPlan, kSoloViewTiles).made <= 0) break;
        made = true;
    } while (frameNow() - start < budgetMs);
    return made;
}

void runSingleThreadIdle(double budgetMs) {
    const chwp::render::FramePhaseTimer phaseTimer(chwp::render::FramePhase::Crew);
    if (crewRunning()) return;
    const double start = frameNow();
    while (stepSingleThreadIdle() && frameNow() - start < budgetMs) {
    }
}

bool stepSingleThreadIdle() {
    if (crewRunning() || gDoc.pages.empty()) return false;
    if (stepSingleThreadForeground()) return true;
    chwp::render::noteDocumentGlyphs(gDoc.doc);
    if (!(gLastPlan.job.background && !jobOnScreen(gLastPlan))) {
        const chwp::render::IdleStep step = gFrames.prepareIdle(gDoc.doc, gDoc.pages, gLastPlan);
        if (step.more || step.screen || step.made > 0) return step.more || step.made > 0;
    }
    const chwp::render::LineNote lines =
        chwp::render::cacheIdleLines(gDoc.doc, gDoc.pages, idleOriginPage(gLastPlan), 16);
    if (lines.more) return true;
    const int targetMaskZoom = chwp::render::restingMaskZoom(gLastPlan.percent);
    const chwp::render::MaskBake masks = chwp::render::bakeDocumentMasks(gDoc.doc, 16, targetMaskZoom);
    if (masks.more) return true;
    const chwp::render::IdleStep step = gFrames.prepareIdle(gDoc.doc, gDoc.pages, gLastPlan);
    return step.more || step.made > 0;
}

void beginGrind() {
    if (gDoc.pages.empty()) return;
    {
        std::lock_guard lock(gWorkMu);
        if (gCrewLive.load()) {
            ++gTick;
            gWorkCv.notify_all();
            return;
        }
        gCrewStop = false;
        gCrewLive.store(true);
        if (gClaimed.size() != gDoc.pages.size()) gClaimed.assign(gDoc.pages.size(), 0);
        if (gBaseScanned.size() != gDoc.pages.size()) gBaseScanned.assign(gDoc.pages.size(), 0);
        gPictureIds.clear();
        gPictureCursor = 0;
        gPictureInflight = 0;
        gPictureIds.reserve(gDoc.doc.header.binItems.size());
        for (const auto& item : gDoc.doc.header.binItems) {
            if (!item.id.empty()) gPictureIds.push_back(item.id);
        }
        gCrewSize = grindThreads();
    }
    if (gCrewSize <= 0) {
        std::fprintf(stderr, "[ClassingHWP] 그리는 스레드 없음 · 화면 스레드가 프레임마다 나눠 굽는다\n");
        return;
    }
    std::fprintf(stderr, "[ClassingHWP] 그리는 스레드 %d · 화면 스레드 1\n", gCrewSize);
    gCrew.reserve(static_cast<std::size_t>(gCrewSize));
    for (int i = 0; i < gCrewSize; ++i) gCrew.emplace_back(crewMain);
}


bool paperPoint(double vx, double vy, bool clampToNearest, int& page, int& x, int& y) {
    const chwp::render::ViewPlan& plan = gLastPlan;
    const chwp::render::ViewportState cam = gCamera.state();
    const int scrollX = cam.percent == plan.percent ? cam.scrollX : plan.scrollX;
    const int scrollY = cam.percent == plan.percent ? cam.scrollY : plan.scrollY;
    auto toHu = [&](int index, double left, double top, int w, int h) {
        if (w <= 0 || h <= 0 || index < 0 || index >= static_cast<int>(gDoc.pages.size())) return false;
        const chwp::render::Node& node = gDoc.pages[static_cast<std::size_t>(index)];
        if (node.width <= 0 || node.height <= 0) return false;
        page = index;
        x = static_cast<int>(std::llround((vx - left) / static_cast<double>(w) * chwp::render::viewWidthOf(node)));
        y = static_cast<int>(std::llround((vy - top) / static_cast<double>(h) * node.height));
        return true;
    };
    auto hit = [&](int index, double left, double top, int w, int h) {
        if (vx < left || vy < top || vx > left + w || vy > top + h) return false;
        return toHu(index, left, top, w, h);
    };
    for (const chwp::render::ViewPage& sheet : plan.pages) {
        if (hit(sheet.index, static_cast<double>(sheet.x - scrollX), static_cast<double>(sheet.y - scrollY), sheet.w,
                sheet.h)) {
            return true;
        }
    }
    if (plan.pages.empty() && plan.page >= 0) {
        if (hit(plan.page, plan.paperX, plan.paperY, plan.paperW, plan.paperH)) return true;
        if (clampToNearest) return toHu(plan.page, plan.paperX, plan.paperY, plan.paperW, plan.paperH);
    }
    if (!clampToNearest || plan.pages.empty()) return false;
    const chwp::render::ViewPage* nearest = nullptr;
    double bestDist = 0;
    for (const chwp::render::ViewPage& sheet : plan.pages) {
        if (sheet.w <= 0 || sheet.h <= 0) continue;
        const double left = static_cast<double>(sheet.x - scrollX);
        const double top = static_cast<double>(sheet.y - scrollY);
        const double cx = std::clamp(vx, left, left + sheet.w);
        const double cy = std::clamp(vy, top, top + sheet.h);
        const double dx = vx - cx;
        const double dy = vy - cy;
        const double dist = dx * dx + dy * dy;
        if (!nearest || dist < bestDist) {
            nearest = &sheet;
            bestDist = dist;
        }
    }
    if (!nearest) return false;
    return toHu(nearest->index, static_cast<double>(nearest->x - scrollX), static_cast<double>(nearest->y - scrollY),
                nearest->w, nearest->h);
}

// 마지막으로 누른 포인터가 Shift 와 함께였는지(InputPointer::shift).
bool gPointerShift = false;

SelectionReport attachSelection(const chwp::input::PointerOutcome& outcome) {
    SelectionReport out;
    if (!outcome.select) return out;
    std::optional<chwp::render::PageFillGuard> guard;
    guard.emplace();
    // 편집 앞에 쪽 일꾼을 멈출 때는 쪽 채우기 잠금을 놓는다. 일꾼의 칸 모으기가 이 잠금을 기다리면
    // 멈추기가 그 일꾼을 기다리느라 서로 묶인다(다른 편집 입구는 멈춘 뒤에 잠근다).
    const auto pauseCrew = [&] {
        guard.reset();
        pauseCrewForEdit();
        guard.emplace();
    };
    int kind = 1;
    if (outcome.tap) kind = 1;
    else if (outcome.phase == chwp::input::Phase::Move) kind = 2;
    else if (outcome.phase == chwp::input::Phase::Up) kind = 3;
    else if (outcome.phase == chwp::input::Phase::Cancel) kind = 4;
    int page = -1;
    int x = 0;
    int y = 0;
    if (kind == 1 || kind == 2 || kind == 3) paperPoint(outcome.x, outcome.y, kind == 2, page, x, y);
    if ((kind == 1 || kind == 2) && page >= 0) {
        ensurePageSpans(page);
    }
    chwp::render::PointerSample sample;
    sample.id = outcome.id;
    sample.device = outcome.device;
    sample.shift = gPointerShift;
    sample.time = outcome.time;
    sample.dpr = gLastPlan.dpr > 0 ? gLastPlan.dpr : 1;
    sample.viewX = outcome.x;
    sample.viewY = outcome.y;
    if (page >= 0 && page < static_cast<int>(gDoc.pages.size())) {
        for (const chwp::render::ViewPage& sheet : gLastPlan.pages) {
            if (sheet.index != page || sheet.w <= 0) continue;
            const chwp::render::Node& node = gDoc.pages[static_cast<std::size_t>(page)];
            if (node.width > 0) sample.huPerLogical = (static_cast<double>(chwp::render::viewWidthOf(node)) / sheet.w) * sample.dpr;
            break;
        }
    }
    chwp::render::PointerView view =
        chwp::render::applyPointer(gDoc.doc, gDoc.pages, kind, page, x, y, gCamera.state().percent, sample);
    const std::string event = chwp::render::pointerEventText();
    if (kind == 2 && event.empty()) {
        return out;
    }
    if (outcome.tap) {
        view = chwp::render::applyPointer(gDoc.doc, gDoc.pages, 3, page, x, y, gCamera.state().percent, sample);
    }
    // 누름틀 안을 누르면 안내문을 숨기고, 밖을 누르면 다시 보인다(한글).
    if (kind != 2 || outcome.tap) syncClickHereGuideNow();
    // 도형 그리기를 마쳤으면 그 도형을 문서에 넣는다. 무엇을 어떻게 넣을지는 코어가 정한다.
    if (kind == 3 || outcome.tap) {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrew();
        chwp::render::EditResult drawn =
            gDoc.insertDrawnShape();
        // 막힌 글꼴 때문에 도형을 넣지 못했다. 셸이 바꿀지 묻고, 바꾸면 그 도형을 이어서 넣는다.
        // 뒤의 옮기기·크기 결과로 덮지 않는다.
        const bool askSwap = drawn.needsFontSwap;
        if (askSwap) out.needsFontSwap = true;
        if (!drawn.documentChanged && !askSwap) drawn = gDoc.moveObject();
        if (!drawn.documentChanged && !askSwap) drawn = gDoc.resizeObject();
        if (!drawn.documentChanged && !askSwap) drawn = gDoc.resizeTableBorder();
        if (drawn.documentChanged) {
            syncEditedDocument(drawn);
            out.edited = true;
            // 글상자를 그리면 캐럿이 그 안에 든다. 셸이 입력칸에 초점을 주도록 지금 커서를 알린다.
            const chwp::render::PointerCursor cursor = chwp::render::pointerCursor();
            view.active = cursor.active;
            view.caret = cursor.caret;
        }
        if (wasCrew) resumeCrewAfterEdit(drawn);
    }
    if ((kind == 3 || outcome.tap) && chwp::render::commitObjectResize(gDoc.doc)) {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrew();
        const chwp::render::ResizeCommitInfo resized = chwp::render::resizeCommitInfo();
        if (!chwp::render::reflowResizedPages(gDoc.doc, gDoc.pages, resized.page, gDoc.storedLineHeight, gDoc.storedLineHeight,
                                               resized.bin, static_cast<int>(resized.width),
                                               static_cast<int>(resized.height))) {
            gDoc.layout(gDoc.storedLineHeight);
        }
        chwp::render::restoreResizedObject(gDoc.doc, gDoc.pages, resized.page, resized.bin,
                                             static_cast<int>(resized.width), static_cast<int>(resized.height));
        syncFullRelayout(true);
        if (wasCrew) {
            chwp::render::EditResult resumed;
            resumed.documentChanged = true;
            resumed.fullInvalidation = true;
            resumeCrewAfterEdit(resumed);
        }
    }
    // 링크 글을 끌지 않고 한 번 누르면 따라간다(한글과 같다). Shift 누르기·블록 고르기는 따라가지 않는다.
    if ((kind == 3 || outcome.tap) && !out.edited && !gPointerShift && page >= 0 &&
        !chwp::render::pointerCursor().hasSelection && chwp::render::linkUnder(gDoc.doc, gDoc.pages, page, x, y)) {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrew();
        std::string url;
        std::string why;
        const chwp::render::EditResult followed = gDoc.followLinkAt(page, x, y, &url, &why);
        if (followed.handled) {
            gCenterCaretOnce = true;
            if (followed.documentChanged) {
                syncEditedDocument(followed);
                out.edited = true;
            } else {
                revealCaretNow(followed);
            }
            gCenterCaretOnce = false;
            const chwp::render::PointerCursor cursor = chwp::render::pointerCursor();
            view.active = cursor.active;
            view.caret = cursor.caret;
            if (!url.empty() && hostSlot().openUrl) hostSlot().openUrl(url);
        } else if (!why.empty()) {
            out.notice = why;
        }
        if (wasCrew) resumeCrewAfterEdit(followed);
    }
    if (gScreenOn && chwp::render::screenReady()) {
        if (!gScreenLoop) {
            screenFrame(frameNow(), nullptr);
        }
    }
    if (out.edited) out.docState = gDoc.documentState();
    out.attached = true;
    out.selectPhase = outcome.tap ? 1 : static_cast<int>(outcome.phase);
    if (kind == 3 && chwp::render::takePinTap()) out.pinTap = true;
    if (kind == 3) out.dialog = chwp::render::takeOpenDialog();
    out.active = view.active;
    out.caret = view.caret;
    if (kind != 2) {
        out.styled = true;
        out.style = cursorStyleNow();
        out.text = copyableSelectionText();
    }
    out.event = event;
    gFallbackImages.clear();
    if (!gScreenOn) gFallbackImages = chwp::render::pointerImages(gDoc.doc, gDoc.pages);
    return out;
}

// 뷰포트 장치 픽셀 한 칸이 그 쪽에서 몇 HWPUNIT 인지. 논리 픽셀 기준이라 dpr 을 곱한다.
double huPerLogicalAt(int page) {
    if (page < 0 || page >= static_cast<int>(gDoc.pages.size())) return 0;
    const double dpr = gLastPlan.dpr > 0 ? gLastPlan.dpr : 1;
    for (const chwp::render::ViewPage& sheet : gLastPlan.pages) {
        if (sheet.index != page || sheet.w <= 0) continue;
        const chwp::render::Node& node = gDoc.pages[static_cast<std::size_t>(page)];
        return node.width > 0 ? (static_cast<double>(chwp::render::viewWidthOf(node)) / sheet.w) * dpr : 0;
    }
    return 0;
}

// 터치가 블록 마커 위에서 시작하면 스크롤하지 않고 마커를 끈다.
bool touchOnPin(double vx, double vy) {
    int page = -1;
    int x = 0;
    int y = 0;
    if (!paperPoint(vx, vy, false, page, x, y) || page < 0) return false;
    return chwp::render::pinUnder(page, x, y, 22.0 * huPerLogicalAt(page));
}

// 마우스가 올라간 자리의 CSS 커서. 눈금자·스크롤 막대·종이 밖은 화살표다.
std::string hoverCursor(double vx, double vy) {
    if (gCamera.inRuler(vx, vy)) return "default";
    chwp::render::Scrollbar horizontal;
    chwp::render::Scrollbar vertical;
    gCamera.scrollbars(horizontal, vertical);
    auto onBar = [&](const chwp::render::Scrollbar& bar) {
        return bar.visible && vx >= bar.x && vy >= bar.y && vx < bar.x + bar.w && vy < bar.y + bar.h;
    };
    if (onBar(horizontal) || onBar(vertical)) return "default";
    int page = -1;
    int x = 0;
    int y = 0;
    if (!paperPoint(vx, vy, false, page, x, y) || page < 0) return "default";
    ensurePageSpans(page);
    return chwp::render::hoverCursor(gDoc.doc, gDoc.pages, page, x, y, 10.0 * huPerLogicalAt(page));
}

// type: pointer, wheel, gesture, tick, scroll, zoom 등.
// 포인터는 id 와 마우스·터치·펜을 그대로 넘긴다. 좌표는 뷰포트 장치 픽셀이다.
// 배율 대화 상자: 사용자 배율 한 걸음, 여러 쪽의 가장 큰 칸 수.
constexpr int kZoomDialogStep = 10;
constexpr int kZoomDialogMaxGrid = 8;
// 여러 쪽의 처음 칸 수. 배율 대화 상자와 임베드의 view=grid 가 같이 쓴다.
constexpr int kGridCols = 4;
constexpr int kGridRows = 3;

std::string zoomDialogSpec() {
    JsonWriter writer(false);
    writer.beginObject();
    writer.key("presets");
    writer.beginArray();
    for (const int percent : {100, 125, 150, 200, 300, 500}) writer.value(percent);
    writer.endArray();
    writer.field("step", kZoomDialogStep);
    writer.field("minPercent", 1);
    writer.field("maxGrid", kZoomDialogMaxGrid);
    writer.field("cols", kGridCols);
    writer.field("rows", kGridRows);
    writer.endObject();
    return writer.take();
}

const std::vector<KeyBinding>& keyBindings() {
    // 편집 키 번호는 render::EditKey 차례다.
    static const std::vector<KeyBinding> table = [] {
        std::vector<KeyBinding> out;
        const std::pair<const char*, int> edit[] = {{"Backspace", 1}, {"Delete", 2}, {"Enter", 3}, {"ArrowLeft", 4},
                                                   {"ArrowRight", 5}, {"ArrowUp", 6}, {"ArrowDown", 7}, {"Home", 8},
                                                   {"End", 9}, {"Tab", 10}, {"Insert", 13}};
        for (const auto& [key, code] : edit) out.push_back(KeyBinding{key, -1, false, false, "editKey", code, false});
        // 한글 2024 이동 단축키: Ctrl+←→ 단어, Ctrl+↑↓ 문단 처음, Alt+Home·End 문단 처음·끝,
        // Ctrl+PageUp·PageDown 문서 처음·끝. Shift 를 같이 누르면 블록을 넓힌다.
        for (const auto& [key, code] : {std::pair<const char*, int>{"ArrowLeft", 4}, {"ArrowRight", 5}, {"ArrowUp", 6},
                                        {"ArrowDown", 7}}) {
            out.push_back(KeyBinding{key, -1, true, false, "editKey", code, false});
        }
        out.push_back(KeyBinding{"Home", -1, false, true, "editKey", 8, false});
        out.push_back(KeyBinding{"End", -1, false, true, "editKey", 9, false});
        // Ctrl+Home·End: 화면 처음·끝(한글 MoveViewBegin·MoveViewEnd).
        out.push_back(KeyBinding{"Home", -1, true, false, "editKey", 8, false});
        out.push_back(KeyBinding{"End", -1, true, false, "editKey", 9, false});
        out.push_back(KeyBinding{"PageUp", -1, true, false, "editKey", 14, false});
        out.push_back(KeyBinding{"PageDown", -1, true, false, "editKey", 15, false});
        // Esc 는 편집 입력칸에서만 코어로 보낸다(Shift+Esc 는 머리말·꼬리말 편집 끝내기).
        out.push_back(KeyBinding{"Escape", -1, false, false, "editKey", 11, true});
        out.push_back(KeyBinding{"KeyA", 0, true, false, "editKey", 12, false});
        out.push_back(KeyBinding{"KeyZ", 0, true, false, "undo", 0, false});
        out.push_back(KeyBinding{"KeyZ", 1, true, false, "redo", 0, false});
        // Ctrl+Shift+C 는 가운데 정렬이라 복사는 Shift 없이만.
        out.push_back(KeyBinding{"KeyC", 0, true, false, "copy", 0, false});
        out.push_back(KeyBinding{"KeyC", 0, false, true, "copyShape", 0, false});  // 모양 복사(한글 Alt+C)
        out.push_back(KeyBinding{"KeyX", -1, true, false, "cut", 0, false});
        // 붙여 넣기는 플랫폼이 클립보드 글을 입력칸 붙여 넣기 사건으로 준다. 표에는 메뉴 표시와 같은 차례로 둔다.
        out.push_back(KeyBinding{"KeyV", -1, true, false, "paste", 0, false});
        out.push_back(KeyBinding{"KeyO", 0, true, false, "open", 0, false});
        out.push_back(KeyBinding{"KeyS", 0, true, false, "save", 0, false});
        out.push_back(KeyBinding{"KeyP", 0, true, false, "print", 0, false});
        out.push_back(KeyBinding{"PageDown", 0, false, false, "pageDown", 0, false});
        out.push_back(KeyBinding{"PageUp", 0, false, false, "pageUp", 0, false});
        // 한글처럼 Alt+L 글자 모양, Alt+T 문단 모양, F7 편집 용지.
        out.push_back(KeyBinding{"KeyL", 0, false, true, "charDialog", 0, false});
        out.push_back(KeyBinding{"KeyT", 0, false, true, "paraDialog", 0, false});
        out.push_back(KeyBinding{"F7", 0, false, false, "pageDialog", 0, false});
        // 나누기(한글 2024). 표 칸의 Ctrl+Enter 는 줄 추가라 그 줄이 앞이다.
        out.push_back(KeyBinding{"Enter", 0, true, false, "appendRow", 0, false, "table"});
        out.push_back(KeyBinding{"Enter", 0, true, false, "breakPage", 0, false});
        out.push_back(KeyBinding{"KeyJ", 0, true, false, "breakPage", 0, false});
        out.push_back(KeyBinding{"Enter", 1, true, false, "breakColumn", 0, false});
        out.push_back(KeyBinding{"Enter", 0, true, true, "breakColumnDef", 0, false});
        out.push_back(KeyBinding{"Enter", 1, false, true, "breakSection", 0, false});
        out.push_back(KeyBinding{"Backspace", 0, true, false, "subtractRow", 0, false, "cellStart"});
        // 찾기
        out.push_back(KeyBinding{"KeyF", 0, true, false, "findDialog", 0, false});
        out.push_back(KeyBinding{"KeyH", 0, true, false, "replaceDialog", 0, false});
        out.push_back(KeyBinding{"F2", 0, true, false, "replaceDialog", 0, false});
        out.push_back(KeyBinding{"KeyL", 0, true, false, "repeatFind", 0, false});
        // 찾아가기(한글 GotoDlg, Alt+G).
        out.push_back(KeyBinding{"KeyG", 0, false, true, "goToDialog", 0, false});
        // 칸 블록
        out.push_back(KeyBinding{"KeyM", 0, false, false, "mergeCells", 0, false, "cellBlock"});
        out.push_back(KeyBinding{"KeyS", 0, false, false, "splitCellDialog", 0, false, "cellBlock"});
        out.push_back(KeyBinding{"KeyW", 0, false, false, "equalWidth", 0, false, "cellBlock"});
        out.push_back(KeyBinding{"KeyH", 0, false, false, "equalHeight", 0, false, "cellBlock"});
        // 글자 서식(한글 2024). Ctrl+B·I·U 와 Alt+Shift 짝.
        const auto both = [&](const char* key, const char* command) {
            out.push_back(KeyBinding{key, 0, true, false, command, 0, false});
            out.push_back(KeyBinding{key, 1, false, true, command, 0, false});
        };
        both("KeyB", "bold");
        both("KeyI", "italic");
        both("KeyU", "underline");
        const auto altShift = [&](const char* key, const char* command) {
            out.push_back(KeyBinding{key, 1, false, true, command, 0, false});
        };
        const auto ctrlShift = [&](const char* key, const char* command) {
            out.push_back(KeyBinding{key, 1, true, false, command, 0, false});
        };
        altShift("KeyE", "fontSizeUp");
        altShift("KeyR", "fontSizeDown");
        out.push_back(KeyBinding{"BracketRight", 0, true, false, "fontSizeUp", 0, false});
        out.push_back(KeyBinding{"BracketLeft", 0, true, false, "fontSizeDown", 0, false});
        altShift("KeyK", "ratioUp");
        altShift("KeyJ", "ratioDown");
        altShift("KeyW", "spacingUp");
        altShift("KeyN", "spacingDown");
        altShift("KeyC", "normalShape");
        altShift("KeyP", "superscript");
        altShift("KeyS", "subscript");
        out.push_back(KeyBinding{"KeyA", 0, true, true, "superSub", 0, false});
        // 문단 서식(한글 2024).
        ctrlShift("KeyM", "alignJustify");
        ctrlShift("KeyL", "alignLeft");
        ctrlShift("KeyR", "alignRight");
        ctrlShift("KeyC", "alignCenter");
        ctrlShift("KeyT", "alignDistribute");
        ctrlShift("KeyE", "marginLeftDown");
        ctrlShift("KeyG", "marginLeftUp");
        ctrlShift("KeyD", "marginRightUp");
        ctrlShift("KeyF", "marginRightDown");
        // Style shortcuts (Hangul 2024 StyleShortcut1..10 on Ctrl+1..0 and Ctrl+Alt+1..0) and the style dialog (F6).
        for (int n = 1; n <= 10; ++n) {
            const std::string key = "Digit" + std::to_string(n % 10);
            const std::string command = "styleShortcut" + std::to_string(n);
            out.push_back(KeyBinding{key, 0, true, false, command, 0, false});
            out.push_back(KeyBinding{key, 0, true, true, command, 0, false});
        }
        out.push_back(KeyBinding{"F6", 0, false, false, "styleDialog", 0, false});
        out.push_back(KeyBinding{"F5", 0, true, true, "marginLeftDown", 0, false});
        out.push_back(KeyBinding{"F6", 0, true, true, "marginLeftUp", 0, false});
        out.push_back(KeyBinding{"F7", 0, true, true, "marginRightUp", 0, false});
        out.push_back(KeyBinding{"F8", 0, true, true, "marginRightDown", 0, false});
        out.push_back(KeyBinding{"F5", 0, true, false, "indentDown", 0, false});
        out.push_back(KeyBinding{"F6", 0, true, false, "indentUp", 0, false});
        ctrlShift("KeyO", "indentDown");
        ctrlShift("KeyI", "indentUp");
        altShift("KeyA", "lineSpacingDown");
        altShift("KeyZ", "lineSpacingUp");
        ctrlShift("KeyQ", "lineSpacingDown");
        ctrlShift("KeyW", "lineSpacingUp");
        // 두 번 누르는 단축키(한글 2024 hwp_shortcuts.xml 의 prevKey·nextKey). 있는 기능만 둔다.
        const auto chord = [&](const char* key, const char* next, const char* command) {
            out.push_back(KeyBinding{key, 0, true, false, command, 0, false, "", next});
        };
        chord("KeyK", "KeyB", "bookmarkDialog");   // BookmarkDlg
        chord("KeyK", "KeyU", "updateToc");        // UpdateTOC
        chord("KeyQ", "KeyB", "linkBack");         // HyperlinkPrev
        chord("KeyQ", "KeyR", "linkForward");      // HyperlinkNext
        chord("KeyK", "KeyA", "updateAllToc");     // UpdateAllTOC
        chord("KeyK", "KeyH", "hyperlinkDialog");  // HyperlinkDlg
        chord("KeyN", "KeyN", "footnote");         // Footnotes
        chord("KeyN", "KeyE", "endnote");          // Endnotes
        chord("KeyN", "KeyT", "tableDialog");      // InsertTableDlg
        chord("KeyN", "KeyI", "pictureDialog");    // InsertPictureDlg
        chord("KeyN", "KeyG", "sectionDialog");    // SectionDlg
        chord("KeyN", "KeyH", "headerFooterDialog");  // HeaderFooterDlg
        chord("KeyN", "KeyC", "captionToggle");    // InsertCaption(캡션 넣기·없애기)
        chord("KeyN", "KeyM", "equationDialog");
        chord("KeyN", "KeyK", "modify");           // ModifyDlg(고치기)
        chord("KeyQ", "KeyF", "findDialog");       // FindDlg
        chord("KeyQ", "KeyA", "replaceDialog");    // FindReplaceDlg
        return out;
    }();
    return table;
}

namespace {

// 앞뒤 빈칸을 떼고 작은 글자로.
std::string embedWord(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    std::string out(text.substr(begin, end - begin));
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// "150", "150%", "12.5" 같은 수. 끝의 % 는 percent 가 참일 때만 받는다.
std::optional<double> embedNumber(std::string word, bool percent) {
    if (percent && !word.empty() && word.back() == '%') {
        word.pop_back();
        while (!word.empty() && std::isspace(static_cast<unsigned char>(word.back()))) word.pop_back();
    }
    if (word.empty() || !(std::isdigit(static_cast<unsigned char>(word[0])) || word[0] == '.')) return std::nullopt;
    char* end = nullptr;
    const double n = std::strtod(word.c_str(), &end);
    if (end != word.c_str() + word.size() || !std::isfinite(n)) return std::nullopt;
    return n;
}

// 주소 인자 글의 %XX 와 + 를 푼다.
std::string decodeQueryPart(std::string_view text) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '+') {
            out.push_back(' ');
        } else if (c == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0) {
            out.push_back(static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2])));
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

EmbedCommand parseEmbedCommand(const std::string& command, const std::string& value, int cols, int rows, int pageCount) {
    EmbedCommand out;
    const std::string name = embedWord(command);
    const std::string word = embedWord(value);
    if (name == "zoom") {
        if (word == "page") {
            out.kind = EmbedCommand::Kind::FitPage;
        } else if (word == "width") {
            out.kind = EmbedCommand::Kind::FitWidth;
        } else if (word == "in" || word == "out") {
            out.kind = EmbedCommand::Kind::ZoomStep;
            out.value = word == "in" ? 1 : -1;
        } else if (const auto n = embedNumber(word, true); n && *n >= 1) {
            // 범위(10~800%)는 화면(PageView)이 자른다.
            out.kind = EmbedCommand::Kind::Zoom;
            out.value = static_cast<int>(std::lround(std::min(*n, 100000.0)));
        }
    } else if (name == "page") {
        if (pageCount <= 0) return out;
        if (word == "next" || word == "prev") {
            out.kind = EmbedCommand::Kind::PageStep;
            out.value = word == "next" ? 1 : -1;
        } else if (word == "first" || word == "last") {
            out.kind = EmbedCommand::Kind::Page;
            out.value = word == "first" ? 0 : pageCount - 1;
        } else if (const auto n = embedNumber(word, false); n && *n >= 1 && *n == std::floor(*n)) {
            out.kind = EmbedCommand::Kind::Page;
            out.value = static_cast<int>(std::min(*n, static_cast<double>(pageCount))) - 1;
        }
    } else if (name == "view") {
        if (word == "auto" || word == "single" || word == "two" || word == "grid") {
            out.kind = EmbedCommand::Kind::View;
            out.view = parseGridMode(word);
            if (out.view == chwp::render::PageGridMode::Grid) {
                out.cols = cols > 0 ? std::min(cols, kZoomDialogMaxGrid) : kGridCols;
                out.rows = rows > 0 ? std::min(rows, kZoomDialogMaxGrid) : kGridRows;
            }
        }
    } else if (name == "edit") {
        if (word == "true" || word == "1" || word == "show") {
            out.kind = EmbedCommand::Kind::EditButton;
            out.value = 1;
        } else if (word == "false" || word == "0" || word == "hide") {
            out.kind = EmbedCommand::Kind::EditButton;
            out.value = 0;
        }
    }
    return out;
}

// 주소 인자 하나의 값. 없으면 빈 글이다.
std::string embedQueryValue(const std::string& query, const std::string& wanted) {
    std::string_view rest(query);
    if (!rest.empty() && rest.front() == '?') rest.remove_prefix(1);
    std::string found;
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string_view part = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        const std::size_t eq = part.find('=');
        if (eq == std::string_view::npos) continue;
        if (decodeQueryPart(part.substr(0, eq)) == wanted) found = decodeQueryPart(part.substr(eq + 1));
    }
    return found;
}

// 쪽 명령이 가리키는 쪽까지 쌓는다. 끝 쪽은 끝까지, N쪽은 N쪽부터 한 화면, 다음 쪽은 지금 쪽 다음 줄까지.
void layoutForPageWord(const std::string& value) {
    if (gDoc.layoutDone() || value.empty()) return;
    const std::string word = embedWord(value);
    if (word == "last") {
        completeLayoutOnScreen();
    } else if (word == "next") {
        const chwp::render::ViewportState now = gCamera.state();
        const int here = currentCameraPage(now.scrollX, now.scrollY, now.percent);
        layoutThroughPage(here + std::max(1, gCamera.effectiveCols()));
    } else if (const auto n = embedNumber(word, false); n && *n >= 1 && *n == std::floor(*n)) {
        layoutThroughPage(static_cast<int>(std::min(*n, 1e9)) - 1);
    }
}

std::vector<EmbedCommand> parseEmbedQuery(const std::string& query, int pageCount) {
    std::string_view rest(query);
    if (!rest.empty() && rest.front() == '?') rest.remove_prefix(1);
    std::string zoom, view, page, edit;
    int cols = 0;
    int rows = 0;
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string_view part = rest.substr(0, amp);
        rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
        const std::size_t eq = part.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string key = decodeQueryPart(part.substr(0, eq));
        const std::string value = decodeQueryPart(part.substr(eq + 1));
        if (key == "zoom") {
            zoom = value;
        } else if (key == "view") {
            view = value;
        } else if (key == "page") {
            page = value;
        } else if (key == "edit") {
            edit = value;
        } else if (key == "cols" || key == "rows") {
            const auto n = embedNumber(embedWord(value), false);
            (key == "cols" ? cols : rows) = n && *n >= 1 ? static_cast<int>(std::min(*n, 1000.0)) : 0;
        }
    }
    // 쪽 모양이 맞춤 배율을 바꾸고, 배율이 쪽 자리를 바꾸므로 이 차례로 놓는다.
    std::vector<EmbedCommand> out;
    for (const EmbedCommand& command : {parseEmbedCommand("edit", edit, 0, 0, pageCount),
                                        parseEmbedCommand("view", view, cols, rows, pageCount),
                                        parseEmbedCommand("zoom", zoom, 0, 0, pageCount),
                                        parseEmbedCommand("page", page, 0, 0, pageCount)}) {
        if (command.kind != EmbedCommand::Kind::None) out.push_back(command);
    }
    return out;
}

namespace {

// 쪽 왼쪽 위(눈금자 포함)가 화면 왼쪽 위에 오게 한다.
chwp::input::Camera revealPageCamera(int page) {
    const chwp::render::ViewPlan layout = gCamera.plan(false);
    for (const chwp::render::ViewPage& item : layout.pages) {
        if (item.index != page) continue;
        return gInput.setScroll(gCamera, item.outerX() - layout.ruler, item.outerY() - layout.ruler);
    }
    return gInput.tick(gCamera, 0);
}

// glide 면 배율이 Ctrl+휠처럼 다가가고, 아니면(처음 열 때) 바로 바뀐다.
chwp::input::Camera applyEmbed(const EmbedCommand& command, bool glide) {
    switch (command.kind) {
        case EmbedCommand::Kind::Zoom:
            return glide ? gInput.glideZoom(gCamera, command.value) : gInput.setZoom(gCamera, command.value);
        case EmbedCommand::Kind::FitPage: return gInput.fitPage(gCamera);
        case EmbedCommand::Kind::FitWidth: return gInput.fitWidth(gCamera);
        case EmbedCommand::Kind::ZoomStep: {
            const int target = chwp::input::zoomLadderStep(gCamera.state().percent, command.value);
            return glide ? gInput.glideZoom(gCamera, target) : gInput.setZoom(gCamera, target);
        }
        case EmbedCommand::Kind::Page: return revealPageCamera(command.value);
        case EmbedCommand::Kind::PageStep: {
            // 화면 한 장(PageDown)이 아니라 쪽 한 줄씩 간다. 한 줄에 여러 쪽이면 그 수만큼 넘는다.
            const chwp::render::ViewportState now = gCamera.state();
            const int last = static_cast<int>(gDoc.pages.size()) - 1;
            const int here = currentCameraPage(now.scrollX, now.scrollY, now.percent);
            const int step = std::max(1, gCamera.effectiveCols());
            return revealPageCamera(std::clamp(here + command.value * step, 0, std::max(0, last)));
        }
        case EmbedCommand::Kind::View:
            return gInput.setGridMode(gCamera, command.view, command.cols, command.rows,
                                      command.view == chwp::render::PageGridMode::Grid);
        case EmbedCommand::Kind::EditButton:
            gEditButton = command.value != 0;
            return gInput.tick(gCamera, 0);
        case EmbedCommand::Kind::None:
        default: return gInput.tick(gCamera, 0);
    }
}

}  // namespace

InputReport input(const InputMessage& msg) {
    const std::string& type = msg.type;
    chwp::input::Camera camera;
    chwp::input::PointerOutcome outcome;
    if (type == "pointer") {
        // 누를 때의 Shift 는 이 포인터의 고르기 판단(여러 개체 고르기)에 쓴다.
        if (msg.pointer.phase.value_or(1) == 1) gPointerShift = msg.pointer.shift;
        chwp::input::PointerEvent event;
        event.device = static_cast<chwp::input::Device>(msg.pointer.device);
        event.phase = static_cast<chwp::input::Phase>(msg.pointer.phase.value_or(1));
        event.id = msg.pointer.id;
        event.x = msg.pointer.x;
        event.y = msg.pointer.y;
        event.time = msg.pointer.t;
        event.button = msg.pointer.button;
        outcome = gInput.pointer(gCamera, event);
        outcome.id = event.id;
        outcome.device = static_cast<int>(event.device);
        outcome.time = event.time;
        if (event.device != chwp::input::Device::Mouse && event.phase == chwp::input::Phase::Down &&
            gInput.touchCount() == 1 && !gInput.pinchActive() &&
            (touchOnPin(event.x, event.y) || chwp::render::drawTool() != 0 ||
             chwp::render::repeatedTouchDown(event.x, event.y, event.time, gLastPlan.dpr > 0 ? gLastPlan.dpr : 1,
                                               static_cast<int>(event.device), event.id))) {
            gInput.holdSelection(event.id);
            outcome.select = true;
            outcome.x = event.x;
            outcome.y = event.y;
        }
        camera = outcome.camera;
    } else if (type == "pointers") {
        std::vector<chwp::input::PointerEvent> events;
        events.reserve(msg.items.size());
        for (const InputPointer& item : msg.items) {
            chwp::input::PointerEvent event;
            event.device = static_cast<chwp::input::Device>(item.device);
            event.phase = static_cast<chwp::input::Phase>(item.phase.value_or(2));
            event.id = item.id;
            event.x = item.x;
            event.y = item.y;
            event.time = item.t;
            event.button = item.button;
            events.push_back(event);
        }
        outcome = gInput.pointers(gCamera, events.data(), events.size());
        camera = outcome.camera;
    } else if (type == "wheel") {
        chwp::input::WheelEvent event;
        event.phase = static_cast<chwp::input::WheelPhase>(msg.phase.value_or(0));
        event.dx = msg.dx;
        event.dy = msg.dy;
        event.x = msg.x;
        event.y = msg.y;
        event.vx = msg.vx;
        event.vy = msg.vy;
        event.zoom = msg.zoom;
        event.discrete = msg.discrete;
        camera = gInput.wheel(gCamera, event);
    } else if (type == "gesture") {
        chwp::input::GestureEvent event;
        event.kind = static_cast<chwp::input::GestureKind>(msg.kind);
        event.phase = static_cast<chwp::input::Phase>(msg.phase.value_or(1));
        event.x = msg.x;
        event.y = msg.y;
        event.dx = msg.dx;
        event.dy = msg.dy;
        event.scale = msg.scale;
        event.rotation = msg.rotation;
        camera = gInput.gesture(gCamera, event);
    } else if (type == "tick") {
        camera = gInput.tick(gCamera, msg.dt);
        if (const auto end = gWheel.endIfDue(frameNow())) camera = gInput.wheel(gCamera, *end);
    } else if (type == "wheelRaw") {
        chwp::input::RawWheel raw;
        raw.dx = msg.dx;
        raw.dy = msg.dy;
        raw.mode = msg.deltaMode;
        raw.zoom = msg.zoom;
        raw.x = msg.x;
        raw.y = msg.y;
        raw.pageW = msg.pageW;
        raw.pageH = msg.pageH;
        raw.dpr = msg.dpr;
        camera = gInput.wheel(gCamera, gWheel.read(raw, frameNow()));
    } else if (type == "scroll") {
        camera = gInput.setScroll(gCamera, msg.scrollX, msg.scrollY);
    } else if (type == "viewport") {
        const chwp::render::ViewportState before = gCamera.state();
        gCamera.setViewport(msg.cssW, msg.cssH, msg.dpr);
        const chwp::render::ViewPlan probe = gCamera.plan(false);
        const chwp::render::ViewportState after = gCamera.state();
        if (probe.viewW != gLastPlan.viewW || probe.viewH != gLastPlan.viewH || probe.percent != gLastPlan.percent ||
            probe.scrollX != gLastPlan.scrollX || probe.scrollY != gLastPlan.scrollY || after.fitPage != before.fitPage ||
            after.fitWidth != before.fitWidth) {
            gViewportDirty = true;
        }
        camera = gInput.tick(gCamera, 0);
        camera.scrollChanged = after.scrollX != before.scrollX || after.scrollY != before.scrollY;
        camera.zoomChanged = after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth;
        camera.handled = true;
    } else if (type == "revealPage") {
        camera = revealPageCamera(msg.page);
    } else if (type == "embed") {
        if (embedWord(msg.mode) == "page") layoutForPageWord(msg.choice);
        const int pageCount = static_cast<int>(gDoc.pages.size());
        camera = applyEmbed(parseEmbedCommand(msg.mode, msg.choice, msg.cols, msg.rows, pageCount), true);
    } else if (type == "embedOpen") {
        // 여러 명령의 바뀜을 모아 한 번에 알린다.
        const chwp::render::ViewportState before = gCamera.state();
        camera = gInput.tick(gCamera, 0);
        layoutForPageWord(embedQueryValue(msg.choice, "page"));
        for (const EmbedCommand& command : parseEmbedQuery(msg.choice, static_cast<int>(gDoc.pages.size()))) {
            camera = applyEmbed(command, false);
        }
        const chwp::render::ViewportState after = gCamera.state();
        camera.scrollChanged = after.scrollX != before.scrollX || after.scrollY != before.scrollY;
        camera.zoomChanged = after.percent != before.percent || after.fitPage != before.fitPage || after.fitWidth != before.fitWidth;
        camera.handled = true;
    } else if (type == "zoom") {
        camera = gInput.glideZoom(gCamera, msg.percent ? *msg.percent : gCamera.state().percent);
    } else if (type == "zoomStep") {
        camera = gInput.glideZoom(gCamera, chwp::input::zoomLadderStep(gCamera.state().percent, msg.dir ? *msg.dir : 1));
    } else if (type == "pageStep" || type == "pageUp" || type == "pageDown") {
        const int fallbackDir = (type == "pageUp") ? -1 : 1;
        camera = gInput.stepPage(gCamera, msg.dir ? *msg.dir : fallbackDir);
    } else if (type == "fit") {
        const std::string mode = msg.mode.empty() ? std::string("page") : msg.mode;
        camera = (mode == "width") ? gInput.fitWidth(gCamera) : gInput.fitPage(gCamera);
    } else if (type == "zoomDialog") {
        // 배율 대화 상자의 적용. 쪽 모양 "맞춤"은 한 쪽을 화면에 맞추므로 배율은 두지 않는다.
        const std::string shape = msg.mode.empty() ? std::string("auto") : msg.mode;
        const bool fitShape = shape == "fit";
        const std::string gridMode = fitShape ? std::string("single") : shape;
        const int cols = gridMode == "grid" ? std::clamp(msg.cols, 1, kZoomDialogMaxGrid) : 1;
        const int rows = gridMode == "grid" ? std::clamp(msg.rows, 1, kZoomDialogMaxGrid) : 1;
        camera = gInput.setGridMode(gCamera, parseGridMode(gridMode), cols, rows, fitShape);
        if (!fitShape) {
            if (msg.choice == "page") camera = gInput.fitPage(gCamera);
            else if (msg.choice == "width") camera = gInput.fitWidth(gCamera);
            else if (msg.percent) camera = gInput.glideZoom(gCamera, *msg.percent);
        }
    } else if (type == "grid") {
        const std::string modeStr = msg.mode.empty() ? std::string("auto") : msg.mode;
        const int cols = msg.cols;
        const int rows = msg.rows;
        const bool fit = msg.fit ? *msg.fit : (modeStr == "grid");
        camera = gInput.setGridMode(gCamera, parseGridMode(modeStr), cols, rows, fit);
    } else if (type == "pins") {
        chwp::render::setSelectionPins(msg.on);
        frameAfterEdit();
        camera = gInput.tick(gCamera, 0);
    } else if (type == "readOnly" || type == "rulers" || type == "rulersToggle") {
        // 눈금자는 사용자가 켠 값(처음은 켬)이고, 보기 전용이면 숨긴다. 켠 값은 그대로 둔다.
        if (type == "readOnly") {
            gShellReadOnly = msg.on;
            chwp::render::setReadOnly(msg.on || gDoc.locked());
        }
        else if (type == "rulers") gRulersWanted = msg.on;
        else gRulersWanted = !gRulersWanted;
        const chwp::render::ViewportState before = gCamera.state();
        gCamera.setRulers(gRulersWanted && !chwp::render::readOnly());
        const chwp::render::ViewportState after = gCamera.state();
        gViewportDirty = true;
        camera = gInput.tick(gCamera, 0);
        camera.scrollChanged = after.scrollX != before.scrollX || after.scrollY != before.scrollY;
        camera.zoomChanged = after.percent != before.percent;
        camera.handled = true;
    } else {
        camera = gInput.tick(gCamera, 0);
    }
    if (camera.scrollChanged || camera.zoomChanged) {
        const double nowMs = frameNow();
        gLastCameraMoveTime = nowMs;
        gIdleMoveStreak = 0;
        if (camera.zoomChanged) {
            gLastZoomMoveTime = nowMs;
            gScrollHold = 0;
        } else if (camera.scrollChanged && !gInput.pinchActive()) {
            gScrollHold = nowMs;
        }
        if (gScreenOn && chwp::render::screenReady() && !gScreenLoop) {
            screenFrame(nowMs, nullptr);
        }
    }
    if ((camera.scrollChanged || camera.zoomChanged) || gInput.selecting() || chwp::render::pointerTracking()) {
        setCrewInteracting(true);
    }
    InputReport out;
    out.camera = camera;
    gCamera.scrollbars(out.barX, out.barY);
    out.page = currentCameraPage(camera.scrollX, camera.scrollY, camera.percent);
    out.pageCount = static_cast<int>(gDoc.pages.size());
    out.pageCounting = !gDoc.layoutDone();
    out.ownsScroll = inputOwnsScroll();
    out.settled = inputSettled();
    out.rulersWanted = gRulersWanted;
    out.editButton = gEditButton;
    out.needsTick = gInput.coasting() || gWheel.pending();
    if (type == "pointer" || type == "pointers") {
        out.pointer = true;
        out.selection = attachSelection(outcome);
    }
    return out;
}

}  // namespace chwp::app
