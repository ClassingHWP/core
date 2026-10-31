// JavaScript 바인딩. 모든 함수는 Uint8Array 를 받는다.
//
//   import createNotingModule from "./noting.mjs";
//   const noting = await createNotingModule();
//   const result = JSON.parse(noting.parse(new Uint8Array(buffer), false, false));

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/html5.h>
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/threading.h>
#endif
#include <emscripten/val.h>

#include "export/font_metrics.hpp"
#include "export/layout.hpp"
#include "noting/export/font.hpp"
#include "noting/export/json.hpp"
#include "noting/export/text.hpp"
#include "noting/input/pointer.hpp"
#include "noting/reader.hpp"
#include "noting/unit.hpp"
#include "noting/writer.hpp"
#include "noting/render/cache_budget.hpp"
#include "noting/render/frame_cache.hpp"
#include "noting/render/paint.hpp"
#include "noting/render/scene.hpp"
#include "noting/render/view.hpp"

// 코어는 창과 프레임 시계가 없다. 이 선언만 먼저 두고, 프레임 콜백이 쪽 계획을 다시 쓴다.
void useFramePlan(noting::render::ViewPlan& plan);
bool paperPoint(double vx, double vy, bool clampToNearest, int& page, int& x, int& y);

namespace {

noting::Bytes toBytes(const emscripten::val& input) {
    const auto length = input["length"].as<std::size_t>();
    noting::Bytes bytes(length);
    emscripten::val view(emscripten::typed_memory_view(length, bytes.data()));
    view.call<void>("set", input);
    return bytes;
}

std::string detectFormat(const emscripten::val& input) {
    const noting::Bytes bytes = toBytes(input);
    return noting::formatName(noting::detectFormat(bytes));
}

noting::Bytes gParsedBytes;
noting::ParseResult gParsedResult;

std::string parse(const emscripten::val& input, bool pretty, bool includeLineSegs) {
    noting::Bytes bytes = toBytes(input);
    noting::ParseOptions options;
    options.keepLineSegs = includeLineSegs;
    noting::ParseResult result = noting::parseDocument(bytes, options);

    noting::JsonOptions json;
    json.pretty = pretty;
    json.includeLineSegs = includeLineSegs;
    std::string out = noting::parseResultToJson(result, json);
    if (includeLineSegs) {
        gParsedBytes = std::move(bytes);
        gParsedResult = std::move(result);
    }
    return out;
}

std::string extractText(const emscripten::val& input) {
    const noting::Bytes bytes = toBytes(input);
    noting::ParseOptions options;
    options.keepLineSegs = false;
    const noting::ParseResult result = noting::parseDocument(bytes, options);
    return result.ok ? noting::documentToText(result.document) : std::string();
}

std::string extractPages(const emscripten::val& input) {
    const noting::Bytes bytes = toBytes(input);
    noting::ParseOptions options;
    options.keepLineSegs = true;
    const noting::ParseResult result = noting::parseDocument(bytes, options);
    noting::JsonWriter writer(false);
    writer.beginArray();
    if (result.ok) {
        for (const auto& page : noting::documentToPages(result.document)) writer.value(page);
    }
    writer.endArray();
    return writer.take();
}

std::string extractAnchorPages(const emscripten::val& input) {
    const noting::Bytes bytes = toBytes(input);
    noting::ParseOptions options;
    const noting::ParseResult result = noting::parseDocument(bytes, options);
    noting::JsonWriter writer(false);
    writer.beginArray();
    if (result.ok) {
        std::vector<int> anchors;
        noting::layout::paginate(result.document, {}, &anchors);
        for (const int page : anchors) writer.value(page);
    }
    writer.endArray();
    return writer.take();
}

std::string extractInnerLines(const emscripten::val& input) {
    const noting::Bytes bytes = toBytes(input);
    noting::ParseOptions options;
    options.keepLineSegs = true;
    const noting::ParseResult result = noting::parseDocument(bytes, options);
    noting::JsonWriter writer(false);
    writer.beginArray();
    if (result.ok) {
        std::vector<std::vector<std::uint8_t>> inner;
        noting::layout::paginate(result.document, {}, nullptr, &inner);
        for (const auto& page : inner) {
            writer.beginArray();
            for (const std::uint8_t flag : page) writer.value(static_cast<int>(flag));
            writer.endArray();
        }
    }
    writer.endArray();
    return writer.take();
}

std::string extractLayout(const emscripten::val& input) {
    const noting::Bytes bytes = toBytes(input);
    noting::ParseOptions options;
    options.keepLineSegs = true;
    const noting::ParseResult result = noting::parseDocument(bytes, options);
    std::vector<std::string> pages;
    std::vector<int> anchors;
    std::vector<std::vector<std::uint8_t>> inner;
    std::vector<std::vector<int>> paragraphPages;
    std::vector<std::vector<noting::layout::LineRange>> lineRanges;
    if (result.ok) {
        pages = noting::layout::paginate(result.document, {}, &anchors, &inner, &paragraphPages,
                                         &lineRanges);
    }
    noting::JsonWriter writer(false);
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

const char* sceneKind(noting::render::NodeKind kind) {
    switch (kind) {
        case noting::render::NodeKind::Paper: return "paper";
        case noting::render::NodeKind::Body: return "body";
        case noting::render::NodeKind::Column: return "column";
        case noting::render::NodeKind::Paragraph: return "paragraph";
        case noting::render::NodeKind::Line: return "line";
        case noting::render::NodeKind::Span: return "span";
        case noting::render::NodeKind::Object: return "object";
        case noting::render::NodeKind::Cell: return "cell";
    }
    return "node";
}

void writeEdge(noting::JsonWriter& writer, const char* name, const noting::render::BorderEdge& edge) {
    if (edge.type.empty()) return;
    writer.key(name);
    writer.beginObject();
    writer.field("type", edge.type);
    if (!edge.color.empty()) writer.field("color", edge.color);
    writer.field("width", edge.width);
    writer.endObject();
}

void writeScene(noting::JsonWriter& writer, const noting::render::Node& node) {
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
    if (!node.text.empty()) writer.field("text", node.text);
    if (!node.headingType.empty()) {
        writer.field("headingType", node.headingType);
        writer.field("headingLevel", node.headingLevel);
        if (!node.headingFormat.empty()) writer.field("headingFormat", node.headingFormat);
        if (!node.headingText.empty()) writer.field("headingText", node.headingText);
    }
    if (node.fontSize > 0) writer.field("fontSize", node.fontSize);
    if (node.kind == noting::render::NodeKind::Line && node.textHeight > 0) {
        writer.field("textHeight", node.textHeight);
    }
    if (!node.font.empty()) writer.field("font", node.font);
    if (!node.color.empty()) writer.field("color", node.color);
    if (!node.shade.empty()) writer.field("shade", node.shade);
    if (!node.markpen.empty()) writer.field("markpen", node.markpen);
    if (node.bold) writer.field("bold", true);
    if (node.italic) writer.field("italic", true);
    if (!node.underline.empty()) writer.field("underline", node.underline);
    if (node.strikeout) writer.field("strikeout", true);
    if (node.kind == noting::render::NodeKind::Span || node.kind == noting::render::NodeKind::Line) {
        if (node.ratio != 100) writer.field("ratio", node.ratio);
        if (node.relSize != 100) writer.field("relSize", node.relSize);
        if (node.spacing != 0) writer.field("spacing", node.spacing);
        if (node.charOffset != 0) writer.field("charOffset", node.charOffset);
        if (!node.advances.empty()) {
            writer.key("advances");
            writer.beginArray();
            for (const std::int32_t width : node.advances) writer.value(width);
            writer.endArray();
        }
        if (!node.inks.empty()) {
            writer.key("inks");
            writer.beginArray();
            for (const std::int32_t width : node.inks) writer.value(width);
            writer.endArray();
        }
    }
    if (node.kind == noting::render::NodeKind::Line || node.kind == noting::render::NodeKind::Span) {
        writer.field("charShapeId", node.charShapeId);
    }
    if (node.kind == noting::render::NodeKind::Object || node.kind == noting::render::NodeKind::Cell) {
        writer.field("zOrder", node.zOrder);
    }
    if (!node.textWrap.empty()) writer.field("textWrap", node.textWrap);
    if (!node.fillColor.empty()) writer.field("fillColor", node.fillColor);
    const bool hasBorder = !node.borderLeft.type.empty() || !node.borderRight.type.empty() ||
                           !node.borderTop.type.empty() || !node.borderBottom.type.empty();
    if (hasBorder) {
        writer.key("border");
        writer.beginObject();
        writeEdge(writer, "left", node.borderLeft);
        writeEdge(writer, "right", node.borderRight);
        writeEdge(writer, "top", node.borderTop);
        writeEdge(writer, "bottom", node.borderBottom);
        writer.endObject();
    }
    if (node.kind == noting::render::NodeKind::Paragraph) {
        if (node.marginLeft) writer.field("marginLeft", node.marginLeft);
        if (node.marginRight) writer.field("marginRight", node.marginRight);
        if (node.indent) writer.field("indent", node.indent);
        if (node.spacingPrev) writer.field("spacingPrev", node.spacingPrev);
        if (node.spacingNext) writer.field("spacingNext", node.spacingNext);
        if (!node.align.empty()) writer.field("align", node.align);
    }
    if (node.kind == noting::render::NodeKind::Line) {
        if (!node.align.empty()) writer.field("align", node.align);
        if (node.lastLine) writer.field("lastLine", true);
        if (node.lineBreak) writer.field("lineBreak", true);
    }
    if (node.kind == noting::render::NodeKind::Cell) {
        writer.field("row", node.row);
        writer.field("col", node.col);
        writer.field("rowSpan", node.rowSpan);
        writer.field("colSpan", node.colSpan);
        if (!node.vertAlign.empty()) writer.field("vertAlign", node.vertAlign);
        writer.field("marginLeft", node.marginLeft);
        writer.field("marginRight", node.marginRight);
        writer.field("marginTop", node.marginTop);
        writer.field("marginBottom", node.marginBottom);
    }
    if (node.cellSpacing != 0) writer.field("cellSpacing", node.cellSpacing);
    if (node.kind == noting::render::NodeKind::Object || node.kind == noting::render::NodeKind::Paper) {
        if (node.marginLeft) writer.field("marginLeft", node.marginLeft);
        if (node.marginRight) writer.field("marginRight", node.marginRight);
        if (node.marginTop) writer.field("marginTop", node.marginTop);
        if (node.marginBottom) writer.field("marginBottom", node.marginBottom);
    }
    if (!node.role.empty()) writer.field("role", node.role);
    if (!node.script.empty()) writer.field("script", node.script);
    if (!node.binItemId.empty()) writer.field("binItemId", node.binItemId);
    writer.key("children");
    writer.beginArray();
    for (const auto& child : node.children) writeScene(writer, child);
    writer.endArray();
    writer.endObject();
}

std::string layoutScene(const emscripten::val& input) {
    const noting::Bytes bytes = toBytes(input);
    noting::ParseOptions options;
    const noting::ParseResult result = noting::parseDocument(bytes, options);
    noting::JsonWriter writer(false);
    writer.beginArray();
    if (result.ok) {
        for (const auto& page : noting::render::layoutPages(result.document)) writeScene(writer, page);
    }
    writer.endArray();
    return writer.take();
}

void loadFont(const std::string& face, const emscripten::val& input) {
    noting::layout::registerFontFace(face, toBytes(input));
}

// 함초롬처럼 큰 woff2 는 풀고 표를 읽는 데 수백 ms 걸린다. 받은 파일마다 스레드를 두어 함께 푼다.
// 웹은 fontDecodesPending() 이 0 이 될 때까지 기다린 뒤 조판한다.
std::atomic<int> gFontDecodes{0};

// 같은 파일을 쓰는 이름은 첫 이름만 바이트를 넘긴다. 나머지는 등록된 파일 바이트를 함께 쓴다.
void registerFontFaces(const std::vector<std::string>& faces, noting::Bytes bytes) {
    for (std::size_t i = 0; i < faces.size(); ++i) {
        noting::layout::registerFontFace(faces[i], i == 0 ? std::move(bytes) : noting::Bytes{});
    }
}

void decodeFont(const emscripten::val& faces, const emscripten::val& input) {
    std::vector<std::string> names = emscripten::vecFromJSArray<std::string>(faces);
    noting::Bytes bytes = toBytes(input);
    if (names.empty()) return;
#if defined(__EMSCRIPTEN_PTHREADS__)
    gFontDecodes.fetch_add(1);
    std::thread([names = std::move(names), bytes = std::move(bytes)]() mutable {
        try {
            registerFontFaces(names, std::move(bytes));
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "[noting 오류] decodeFont %s: %s\n", names.front().c_str(), ex.what());
        }
        gFontDecodes.fetch_sub(1);
    }).detach();
#else
    registerFontFaces(names, std::move(bytes));
#endif
}

int fontDecodesPending() { return gFontDecodes.load(); }

// 셸이 플랫폼 메모리 신호를 넘기는 통로. 1 가벼움, 2 보통, 3 심각.
void trimCaches(int level) {
    noting::render::trimCaches(static_cast<noting::render::TrimLevel>(std::clamp(level, 1, 3)));
}

// 바이트. 음수면 예산 없음.
bool setCacheBudget(const std::string& name, double bytes) {
    const std::size_t budget = bytes < 0 ? noting::render::kNoCacheBudget : static_cast<std::size_t>(bytes);
    return noting::render::setCacheBudget(name, budget);
}

std::string cacheUsage() {
    noting::JsonWriter writer(false);
    writer.beginArray();
    for (const noting::render::CacheUsage& usage : noting::render::cacheUsage()) {
        writer.beginObject();
        writer.field("name", usage.name);
        writer.field("bytes", static_cast<std::uint64_t>(usage.bytes));
        writer.field("budget", usage.budget == noting::render::kNoCacheBudget
                                     ? std::int64_t{-1}
                                     : static_cast<std::int64_t>(usage.budget));
        writer.endObject();
    }
    writer.endArray();
    return writer.take();
}

noting::model::Document gDocument;

int replaceFontFace(const std::string& from, const std::string& to) {
    return noting::model::replaceFontFace(gDocument, from, to);
}
noting::Bytes gFile;
std::vector<noting::render::Node> gPages;
std::vector<std::vector<std::uint8_t>> gPagePng;
std::vector<noting::render::PageBitmap> gPagePixels;
noting::render::PageBitmap gViewFrame;
noting::render::PageView gCamera;
noting::input::PointerInput gInput;
// 줄 캐시 쪽은 용지 크기만 먼저 둔다. 글자 노드는 그 쪽을 그릴 때 만든다.
std::vector<char> gLineReady;
int gPixelHeight = 0;
bool gUseLineCache = false;
bool gStoredLineHeight = false;
int gLastPageGap = 0;
// 비어 있으면 모든 쪽을 그린다. 웹은 글자를 올린 쪽만 넣는다.
std::unordered_set<int> gReadyPages;

bool gScreenOn = false;
// 셸이 창 크기만 바꿨다. 다음 프레임이 쪽 배치를 다시 잡는다.
bool gViewportDirty = false;
int gShellNoteW = -1;
int gShellNoteH = -1;
int gShellNoteMaxX = -1;
int gShellNoteMaxY = -1;
int gShellNotePercent = -1;
// GPU 가 없을 때 셸이 베끼는 동안만 살아있는 픽셀. 구간 판단은 코어에 있다.
std::vector<noting::render::SelectionImage> gFallbackImages;
noting::render::ViewPlan gLastPlan;
int gLastPlanRevision = -1;
double gScrollHold = 0;
double gLastCameraMoveTime = 0;
double gLastZoomMoveTime = 0;
int gIdleMoveStreak = 0;
bool gPageGpuBakePending = false;
std::mutex gPageSpansMu;

noting::render::FrameCache gFrames;

void stopCrew();
void pauseCrewForEdit();
void resumeCrewAfterEdit(const noting::render::EditResult& res);
void drainDone();
void publishPlan();
void noteCamera(int scrollX, int scrollY, int percent, bool coasting, bool pinching);
void mountCameraFromPages();
bool crewRunning();
void beginGrind();
void ensureScreenFrames();
bool screenFrame(double time, void*);

struct LocalPageSprite {
    int id = 0;
    int stamp = 0;
    int frameX = 0;
    int frameY = 0;
    int frameW = 0;
    int frameH = 0;
    int srcPixelH = 0;
    int localX = 0;
    int localY = 0;
    int w = 0;
    int h = 0;
    int sw = 0;
    int sh = 0;
    int percent = 100;
    bool nearest = false;
    const std::uint8_t* rgba = nullptr;
    int layer = 1;
    std::int64_t z = 0;
    int rank = 1;
    std::int32_t huX = 0;
    std::int32_t huY = 0;
    std::int32_t huW = 0;
    std::int32_t huH = 0;
};

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

void clearPointer() { noting::render::clearPointerSelection(); }

void clearPageSlots() {
    stopCrew();
    clearKeptFrames();
    gPages.clear();
    gPagePng.clear();
    gPagePixels.clear();
    gLineReady.clear();
    gPixelHeight = 0;
}

void resetSessionForDocument() {
    stopCrew();
    noting::render::setPictureFile({});
    gParsedBytes.clear();
    gParsedBytes.shrink_to_fit();
    gParsedResult = noting::ParseResult{};
    gDocument = noting::model::Document{};
    gFile.clear();
    gFile.shrink_to_fit();
    clearPageSlots();
    clearPointer();
    gUseLineCache = false;
    noting::render::clearPageRenderCache();
    gCamera.reset();
    gInput.stop();
}

emscripten::val openedDocumentInfo() {
    emscripten::val out = emscripten::val::object();
    out.set("ok", true);
    out.set("error", std::string());
    emscripten::val faces = emscripten::val::array();
    std::vector<std::string> seen;
    auto addFace = [&](const std::string& name) {
        if (name.empty()) return;
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) return;
        seen.push_back(name);
        faces.call<void>("push", name);
    };
    for (const auto& group : gDocument.header.fonts) {
        for (const auto& font : group) {
            addFace(font.face);
            addFace(font.subst);
        }
    }
    out.set("faces", faces);
    noting::layout::requestDocumentFontFaces(gDocument);
    return out;
}

emscripten::val openDocument(const emscripten::val& input) {
    resetSessionForDocument();

    gFile = toBytes(input);
    noting::ParseOptions options;
    noting::ParseResult result = noting::parseDocument(gFile, options);
    gDocument = result.ok ? std::move(result.document) : noting::model::Document{};
    if (result.ok) {
        noting::render::setPictureFile(gFile);
    } else {
        gFile.clear();
        gFile.shrink_to_fit();
    }

    if (!result.ok) {
        emscripten::val out = emscripten::val::object();
        out.set("ok", false);
        out.set("error", result.error);
        out.set("faces", emscripten::val::array());
        return out;
    }
    return openedDocumentInfo();
}

emscripten::val newDocument() {
    resetSessionForDocument();
    gDocument = noting::model::blankDocument();
    return openedDocumentInfo();
}

void noteFailure(const char* where, const std::exception* ex = nullptr) {
    if (ex && ex->what() && ex->what()[0] != '\0') {
        std::fprintf(stderr, "[noting 오류] %s: %s\n", where, ex->what());
    } else {
        std::fprintf(stderr, "[noting 오류] %s\n", where);
    }
}

void publishFontFaces(void*, const char* const* faces, int count) {
    emscripten::val list = emscripten::val::array();
    for (int i = 0; i < count; ++i) {
        if (!faces || !faces[i] || faces[i][0] == '\0') continue;
        list.call<void>("push", std::string(faces[i]));
    }
    emscripten::val hook = emscripten::val::module_property("onFontFaces");
    if (hook.typeOf().as<std::string>() == "function") hook(list);
}

struct InstallFontFaceHook {
    InstallFontFaceHook() { noting::layout::setFontFaceRequestHandler(&publishFontFaces, nullptr); }
};

InstallFontFaceHook gInstallFontFaceHook;

void rememberLineCache(int page, noting::render::LineCachePage built) {
    const int count = std::max(built.count, 0);
    if (static_cast<int>(gPages.size()) != count) {
        gPages.assign(static_cast<std::size_t>(count), {});
        gPagePng.assign(gPages.size(), {});
        gPagePixels.assign(gPages.size(), {});
    }
    if (page >= 0 && page < count && built.page.width > 0) {
        gPages[static_cast<std::size_t>(page)] = std::move(built.page);
    }
}

void ensureLineCachePage(int page) {
    if (!gUseLineCache || page < 0) return;
    if (page < static_cast<int>(gLineReady.size()) && gLineReady[static_cast<std::size_t>(page)]) return;
    rememberLineCache(page, noting::render::lineCachePage(gDocument, page));
    if (static_cast<int>(gLineReady.size()) != static_cast<int>(gPages.size())) gLineReady.assign(gPages.size(), 0);
    if (page >= 0 && page < static_cast<int>(gPages.size()) && gPages[static_cast<std::size_t>(page)].width > 0) {
        gLineReady[static_cast<std::size_t>(page)] = 1;
    }
}

// 폰트 목록이 끝난 뒤에 부른다.
// 쪽·단·개체는 항상 우리 조판이다. lineseg 가 있으면 그 글자 위치에서 줄을 끊고 그 높이를 쓴다.
// 진행 폭은 쓰고, 글리프 윤곽은 읽지 않는다.
std::string planDocument(bool lineSeg) {
    stopCrew();
    clearPointer();
    clearKeptFrames();
    gPagePng.clear();
    gPagePixels.clear();
    gPixelHeight = 0;
    gUseLineCache = false;
    gStoredLineHeight = lineSeg;
    gLineReady.clear();
    gReadyPages.clear();
    noting::render::clearGlyphWarm();
    noting::JsonWriter writer(false);
    writer.beginObject();
    writer.key("pages");
    writer.beginArray();
    struct PageCalcLog {
        PageCalcLog() { noting::render::setPageCalcLog(true); }
        ~PageCalcLog() { noting::render::setPageCalcLog(false); }
    } pageCalcLog;
    gPages = noting::render::layoutPages(gDocument, -1, lineSeg, lineSeg);
    for (const noting::render::Node& page : gPages) {
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
    writer.field("count", static_cast<int>(gPages.size()));
    writer.field("lineSeg", lineSeg);
    writer.endObject();
    mountCameraFromPages();
    return writer.take();
}

void mountCameraFromPages() {
    std::vector<noting::render::PageSpec> specs;
    specs.reserve(gPages.size());
    for (int i = 0; i < static_cast<int>(gPages.size()); ++i) {
        const noting::render::Node& page = gPages[static_cast<std::size_t>(i)];
        noting::render::PageSpec spec;
        spec.index = i;
        spec.widthHu = page.width;
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

void setFontCatalog(const std::string& json) { noting::layout::setFontCatalog(json); }
void setFontLicenseSwaps(const std::string& json) { noting::layout::setFontLicenseSwaps(json); }

emscripten::val pickableFonts() {
    emscripten::val out = emscripten::val::array();
    for (const std::string& face : noting::layout::pickableFonts()) out.call<void>("push", face);
    return out;
}

void warmFonts() { noting::layout::warmDocumentFonts(gDocument); }

int ensurePageSpans(int page) {
    if (page < 0 || page >= static_cast<int>(gPages.size())) return 0;
    std::lock_guard lock(gPageSpansMu);
    return noting::render::ensurePageSpans(gDocument, gPages[static_cast<std::size_t>(page)]);
}

std::string pageFaces(int page) {
    ensurePageSpans(page);
    noting::JsonWriter writer(false);
    writer.beginArray();
    if (page >= 0 && page < static_cast<int>(gPages.size())) {
        for (const std::string& name : noting::render::pageFaceNames(gPages[static_cast<std::size_t>(page)])) {
            writer.value(name);
        }
    }
    writer.endArray();
    return writer.take();
}

std::string documentFaces() {
    noting::JsonWriter writer(false);
    writer.beginArray();
    for (const std::string& name : noting::render::documentFaceNames(gPages)) writer.value(name);
    writer.endArray();
    return writer.take();
}

std::string warmDocument(int percent) {
    const noting::render::GlyphWarm warm = noting::render::warmDocumentGlyphs(gPages, percent);
    noting::JsonWriter writer(false);
    writer.beginObject();
    writer.field("faces", warm.faces);
    writer.field("glyphs", warm.glyphs);
    writer.endObject();
    return writer.take();
}

std::string warmGlyphs(int page, int percent) {
    noting::render::GlyphWarm warm;
    ensurePageSpans(page);
    if (page >= 0 && page < static_cast<int>(gPages.size())) {
        warm = noting::render::warmPageGlyphs(gPages[static_cast<std::size_t>(page)], page, percent);
    }
    noting::JsonWriter writer(false);
    writer.beginObject();
    writer.field("faces", warm.faces);
    writer.field("glyphs", warm.glyphs);
    writer.endObject();
    return writer.take();
}

emscripten::val preparePage(int page, int percent, int focusY) {
    emscripten::val out = emscripten::val::object();
    if (crewRunning()) {
        out.set("made", 0);
        out.set("done", false);
        return out;
    }
    if (gUseLineCache) ensureLineCachePage(page);
    const noting::render::FramePrepare made = gFrames.prepare(gDocument, gPages, page, percent, focusY);
    out.set("made", made.made);
    out.set("done", made.done);
    return out;
}

void setReadyPages(const emscripten::val& pages) {
    gReadyPages.clear();
    if (pages.isUndefined() || pages.isNull()) return;
    const int count = pages["length"].as<int>();
    for (int i = 0; i < count; ++i) gReadyPages.insert(pages[i].as<int>());
}

// 상태 줄의 단·줄·칸·구역. 캐럿이 없으면 active 만 false 다.
emscripten::val packCaretStatus(const noting::render::CaretStatus& st) {
    emscripten::val out = emscripten::val::object();
    out.set("active", st.active);
    out.set("section", st.section);
    out.set("sectionCount", st.sectionCount);
    out.set("column", st.column);
    out.set("line", st.line);
    out.set("char", st.character);
    out.set("band", st.band);
    return out;
}

emscripten::val packCursorStyle(const noting::render::CursorStyle& st) {
    emscripten::val out = emscripten::val::object();
    out.set("active", st.active);
    out.set("styleId", st.styleId);
    out.set("styleName", st.styleName);
    out.set("styleEngName", st.styleEngName);
    out.set("styleType", st.styleType);
    out.set("paraShapeId", st.paraShapeId);
    out.set("align", st.align);
    out.set("lineSpacingType", st.lineSpacingType);
    out.set("lineSpacing", st.lineSpacing);
    out.set("charShapeId", st.charShapeId);
    out.set("font", st.font);
    out.set("fontSwapTo", st.fontSwapTo);
    out.set("fontSize", st.fontSize);
    out.set("fontSizePt", static_cast<double>(st.fontSize) / 100.0);
    out.set("bold", st.bold);
    out.set("italic", st.italic);
    out.set("underline", st.underline);
    out.set("underlineType", st.underlineType);
    out.set("strikeout", st.strikeout);
    out.set("color", st.color);
    out.set("caret", packCaretStatus(noting::render::caretStatus(gDocument, gPages)));
    return out;
}

// 글자 모양·문단 모양 대화 상자를 열 때 채울 값. 이름은 editStyle 과 같다.
emscripten::val formatFieldsNow() {
    emscripten::val out = emscripten::val::object();
    for (const auto& [key, value] : noting::render::formatFieldsAtCursor(gDocument, gPages)) {
        std::visit([&](const auto& v) { out.set(key, v); }, value);
    }
    return out;
}

emscripten::val cursorStyleNow() {
    return packCursorStyle(noting::render::cursorStyle(gDocument, gPages));
}

emscripten::val caretAt(int page, int x, int y) {
    emscripten::val out = emscripten::val::object();
    out.set("hit", false);
    noting::render::PageFillGuard guard;
    if (gUseLineCache) ensureLineCachePage(page);
    if (page < 0 || page >= static_cast<int>(gPages.size())) return out;
    const noting::render::Node& node = gPages[static_cast<std::size_t>(page)];
    if (node.width <= 0) return out;
    const noting::render::CaretPlace place = noting::render::caretOnPage(gDocument, node, x, y);
    if (!place.hit) return out;
    out.set("hit", true);
    out.set("x", place.x);
    out.set("y", place.y);
    out.set("h", place.height);
    out.set("page", page);
    out.set("story", place.story);
    out.set("para", place.paragraph);
    out.set("offset", place.offset);
    out.set("cursorStyle", packCursorStyle(noting::render::cursorStyleForCaret(gDocument, place)));
    return out;
}

emscripten::val caretInStory(int page, int x, int y, int story) {
    emscripten::val out = emscripten::val::object();
    out.set("hit", false);
    noting::render::PageFillGuard guard;
    if (gUseLineCache) ensureLineCachePage(page);
    const noting::render::CaretPlace place = noting::render::caretInStory(gDocument, gPages, page, x, y, story);
    if (!place.hit) return out;
    out.set("hit", true);
    out.set("x", place.x);
    out.set("y", place.y);
    out.set("h", place.height);
    out.set("page", place.page);
    out.set("story", place.story);
    out.set("para", place.paragraph);
    out.set("offset", place.offset);
    out.set("cursorStyle", packCursorStyle(noting::render::cursorStyleForCaret(gDocument, place)));
    return out;
}

emscripten::val blockMarks(int story, int paragraphA, int offsetA, int paragraphB, int offsetB) {
    noting::render::PageFillGuard guard;
    emscripten::val out = emscripten::val::array();
    const std::vector<noting::render::BlockBox> boxes =
        noting::render::blockBoxes(gDocument, gPages, story, paragraphA, offsetA, paragraphB, offsetB);
    for (const noting::render::BlockBox& box : boxes) {
        emscripten::val item = emscripten::val::object();
        item.set("page", box.page);
        item.set("x", box.x);
        item.set("y", box.y);
        item.set("w", box.width);
        item.set("h", box.height);
        out.call<void>("push", item);
    }
    return out;
}

emscripten::val packImages(const std::vector<noting::render::SelectionImage>& images) {
    emscripten::val out = emscripten::val::array();
    for (const noting::render::SelectionImage& image : images) {
        emscripten::val item = emscripten::val::object();
        item.set("page", image.page);
        item.set("x", image.x);
        item.set("y", image.y);
        item.set("w", image.width);
        item.set("h", image.height);
        item.set("pageW", image.pageW);
        item.set("pageH", image.pageH);
        item.set("caret", image.caret);
        item.set("pixels", emscripten::val(emscripten::typed_memory_view(image.rgba.size(), image.rgba.data())));
        out.call<void>("push", item);
    }
    return out;
}

emscripten::val cursorState() {
    const noting::render::PointerCursor cur = noting::render::pointerCursor();
    const noting::model::CursorTarget target = noting::render::resolvePointerCursor(gDocument);
    emscripten::val out = emscripten::val::object();
    out.set("active", cur.active);
    out.set("caret", cur.caret);
    out.set("hasSelection", cur.hasSelection);
    out.set("page", cur.page);
    out.set("story", cur.story);
    out.set("para", cur.paragraph);
    out.set("offset", cur.offset);
    out.set("anchorPara", cur.anchorParagraph);
    out.set("anchorOff", cur.anchorOffset);
    out.set("focusPara", cur.focusParagraph);
    out.set("focusOff", cur.focusOffset);
    out.set("startPara", cur.startParagraph);
    out.set("startOff", cur.startOffset);
    out.set("endPara", cur.endParagraph);
    out.set("endOff", cur.endOffset);
    out.set("valid", target.valid());
    out.set("storyKind", std::string(noting::model::storyKindName(target.slot ? target.slot->kind : noting::model::StoryKind::None)));
    out.set("sectionIndex", target.slot ? target.slot->sectionIndex : -1);
    out.set("cursorStyle", cursorStyleNow());
    return out;
}

// kind 는 그대로 코어에 넘긴다. 구간과 invert 는 코어가 정한다.
emscripten::val pointerEvent(int kind, int page, int x, int y, int percent) {
    if ((kind == 1 || kind == 2) && page >= 0) {
        if (gUseLineCache) ensureLineCachePage(page);
        else ensurePageSpans(page);
    }
    const noting::render::PointerView view =
        noting::render::applyPointer(gDocument, gPages, kind, page, x, y, percent);
    const std::string event = noting::render::pointerEventText();
    const bool dragUnchanged = (kind == 2 && event.empty());
    if (!dragUnchanged && gScreenOn && noting::render::screenReady()) {
        screenFrame(0, nullptr);
    }
    const noting::render::PointerCursor cur = noting::render::pointerCursor();
    emscripten::val out = emscripten::val::object();
    out.set("active", view.active);
    out.set("story", view.story);
    out.set("caret", view.caret);
    out.set("para", cur.paragraph);
    out.set("offset", cur.offset);
    out.set("hasSelection", cur.hasSelection);
    if (kind != 2) {
        out.set("cursorStyle", cursorStyleNow());
        out.set("text", noting::render::selectionText(gDocument, gPages));
    }
    if (!event.empty()) out.set("event", event);
    gFallbackImages.clear();
    if (!gScreenOn && !dragUnchanged) gFallbackImages = noting::render::pointerImages(gDocument, gPages);
    out.set("images", packImages(gFallbackImages));
    return out;
}

bool ensureCaretVisibleInCamera(const noting::render::EditResult& res, noting::render::ViewPlan& plan) {
    if (!res.caret.hit || res.cursor.page < 0 || res.cursor.page >= static_cast<int>(gPages.size())) return false;
    if (plan.viewW <= 0 || plan.viewH <= 0) return false;
    gInput.stop();
    gScrollHold = 0;

    const noting::render::ViewPage* pageView = nullptr;
    for (const noting::render::ViewPage& vp : plan.pages) {
        if (vp.index == res.cursor.page) {
            pageView = &vp;
            break;
        }
    }
    if (!pageView || pageView->w <= 0 || pageView->h <= 0) return false;

    const noting::render::Node& pageNode = gPages[static_cast<std::size_t>(res.cursor.page)];
    if (pageNode.width <= 0 || pageNode.height <= 0) return false;

    const double scaleX = static_cast<double>(pageView->w) / static_cast<double>(pageNode.width);
    const double scaleY = static_cast<double>(pageView->h) / static_cast<double>(pageNode.height);
    int caretLeft = pageView->x + static_cast<int>(std::floor(static_cast<double>(res.caret.x) * scaleX));
    int caretRight = caretLeft + std::max(2, static_cast<int>(std::ceil(100.0 * scaleX)));
    int caretTop = pageView->y + static_cast<int>(std::floor(static_cast<double>(res.caret.y) * scaleY));
    int caretBottom = caretTop + std::max(2, static_cast<int>(std::ceil(static_cast<double>(std::max(1, res.caret.height)) * scaleY)));

    for (const noting::render::SelectionMark& mark : res.compositionUnderlines) {
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
    if (caretTop - marginY < targetY) {
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

void syncEditedDocument(const noting::render::EditResult& res) {
    drainDone();
    gReadyPages.clear();
        if (res.fullInvalidation) {
            gPageSpriteCaches.clear();
            gPagePng.assign(gPages.size(), {});
            gPagePixels.assign(gPages.size(), {});
            gLineReady.clear();
            gFrames.prepareForFullRelayout();
            noting::render::dropPageGpuCache(-1);
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
            shiftFlat(gLineReady);
        }
        if (gPagePng.size() < gPages.size()) gPagePng.resize(gPages.size());
        if (gPagePixels.size() < gPages.size()) gPagePixels.resize(gPages.size());
        if (gLineReady.size() < gPages.size()) gLineReady.resize(gPages.size());
        for (const auto& inv : res.invalidations) {
            gPageSpriteCaches.erase(inv.page);
            noting::render::dropPageGpuCache(inv.page);
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
            std::vector<noting::render::PageSpec> specs;
            specs.reserve(gPages.size());
            for (int i = 0; i < static_cast<int>(gPages.size()); ++i) {
                const noting::render::Node& p = gPages[static_cast<std::size_t>(i)];
                noting::render::PageSpec spec;
                spec.index = i;
                spec.widthHu = p.width;
                spec.heightHu = p.height;
                spec.marginLeftHu = p.marginLeft;
                spec.marginRightHu = p.marginRight;
                spec.marginTopHu = p.marginTop;
                spec.marginBottomHu = p.marginBottom;
                specs.push_back(spec);
            }
            gCamera.setPages(specs, gLastPageGap);
            gCamera.setScroll(prevCam.scrollX, prevCam.scrollY);
        } else if (gLastPlan.page >= 0 && gLastPlan.page < static_cast<int>(gPages.size())) {
            const noting::render::Node& p = gPages[static_cast<std::size_t>(gLastPlan.page)];
            gCamera.setPage(gLastPlan.page, p.width, p.height);
            gCamera.setPageMargins(p.marginLeft, p.marginRight, p.marginTop, p.marginBottom);
            gCamera.setScroll(prevCam.scrollX, prevCam.scrollY);
        }
    }
    noting::render::ViewPlan plan = gCamera.plan(!gScreenOn);
    ensureCaretVisibleInCamera(res, plan);
    if (gScreenOn) {
        gFrames.bakeVisibleNow(gDocument, gPages, plan);
        useFramePlan(plan);
        gLastPlan = std::move(plan);
        publishPlan();
        ensureScreenFrames();
    } else {
        gLastPlan = std::move(plan);
    }
}

void bringSelectionIntoView() {
    const std::vector<noting::render::SelectionMark> marks = noting::render::selectionMarks();
    if (marks.empty() || marks.front().width <= 0 || marks.front().height <= 0) return;
    const noting::render::SelectionMark& mark = marks.front();
    if (mark.page < 0 || mark.page >= static_cast<int>(gPages.size())) return;
    noting::render::ViewPlan plan = gCamera.plan(!gScreenOn);
    if (plan.viewW <= 0 || plan.viewH <= 0) return;
    const noting::render::ViewPage* pageView = nullptr;
    for (const noting::render::ViewPage& vp : plan.pages) {
        if (vp.index == mark.page) {
            pageView = &vp;
            break;
        }
    }
    if (!pageView || pageView->w <= 0 || pageView->h <= 0) return;
    const noting::render::Node& pageNode = gPages[static_cast<std::size_t>(mark.page)];
    if (pageNode.width <= 0 || pageNode.height <= 0) return;

    const double scaleX = static_cast<double>(pageView->w) / static_cast<double>(pageNode.width);
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
        const noting::render::ViewPlan moved = gCamera.plan(!gScreenOn);
        noteCamera(moved.scrollX, moved.scrollY, moved.percent, false, false);
    }
}

void syncFullRelayout(bool bringSelection) {
    noting::render::invalidateLinePlaces({}, true);
    drainDone();
    gReadyPages.clear();
    gPageSpriteCaches.clear();
    gPagePng.assign(gPages.size(), {});
    gPagePixels.assign(gPages.size(), {});
    gLineReady.clear();
    gUseLineCache = false;
    gFrames.prepareForFullRelayout();
    const auto prevCam = gCamera.state();
    if (!gPages.empty()) {
        std::vector<noting::render::PageSpec> specs;
        specs.reserve(gPages.size());
        for (int i = 0; i < static_cast<int>(gPages.size()); ++i) {
            const noting::render::Node& p = gPages[static_cast<std::size_t>(i)];
            noting::render::PageSpec spec;
            spec.index = i;
            spec.widthHu = p.width;
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
    noting::render::ViewPlan plan = gCamera.plan(!gScreenOn);
    if (gScreenOn) {
        gFrames.bakeVisibleNow(gDocument, gPages, plan);
        useFramePlan(plan);
        gLastPlan = std::move(plan);
        publishPlan();
        ensureScreenFrames();
    } else {
        gLastPlan = std::move(plan);
    }
    gViewportDirty = true;
}

emscripten::val packEditResult(const noting::render::EditResult& res) {
    emscripten::val out = emscripten::val::object();
    const auto cam = gCamera.state();
    out.set("handled", res.handled);
    out.set("documentChanged", res.documentChanged);
    out.set("fullInvalidation", res.fullInvalidation);
    out.set("composing", res.composing);
    out.set("needsFontSwap", res.needsFontSwap);
    out.set("dirtySection", res.dirtySection);
    out.set("dirtyStory", res.dirtyStory);
    out.set("dirtyParagraph", res.dirtyParagraph);
    out.set("active", res.cursor.active);
    out.set("caret", res.cursor.caret);
    out.set("hasSelection", res.cursor.hasSelection);
    out.set("page", res.cursor.page);
    out.set("story", res.cursor.story);
    out.set("para", res.cursor.paragraph);
    out.set("offset", res.cursor.offset);
    out.set("caretX", res.caret.x);
    out.set("caretY", res.caret.y);
    out.set("caretH", res.caret.height);
    out.set("scrollX", cam.scrollX);
    out.set("scrollY", cam.scrollY);
    out.set("cursorStyle", cursorStyleNow());
    out.set("text", noting::render::selectionText(gDocument, gPages));
    emscripten::val underlines = emscripten::val::array();
    for (const noting::render::SelectionMark& mark : res.compositionUnderlines) {
        emscripten::val u = emscripten::val::object();
        u.set("page", mark.page);
        u.set("x", mark.x);
        u.set("y", mark.y);
        u.set("w", mark.width);
        u.set("h", mark.height);
        underlines.call<void>("push", u);
    }
    out.set("compositionUnderlines", underlines);
    emscripten::val pages = emscripten::val::array();
    if (res.documentChanged && res.fullInvalidation) {
        for (const noting::render::Node& p : gPages) {
            emscripten::val item = emscripten::val::object();
            item.set("width", p.width);
            item.set("height", p.height);
            item.set("marginLeft", p.marginLeft);
            item.set("marginRight", p.marginRight);
            item.set("marginTop", p.marginTop);
            item.set("marginBottom", p.marginBottom);
            pages.call<void>("push", item);
        }
    }
    out.set("pages", pages);
    gFallbackImages.clear();
    if (!gScreenOn) gFallbackImages = noting::render::pointerImages(gDocument, gPages);
    out.set("images", packImages(gFallbackImages));
    return out;
}

emscripten::val editText(const std::string& text, bool composing) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    noting::render::PageFillGuard guard;
    gUseLineCache = false;
    const noting::render::EditResult res =
        noting::render::editInsertText(gDocument, gPages, text, composing, gStoredLineHeight);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    if (gScreenOn && noting::render::screenReady()) screenFrame(0, nullptr);
    return packEditResult(res);
}

emscripten::val editCommit() {
    noting::render::PageFillGuard guard;
    const noting::render::EditResult res = noting::render::editCommitComposition(gDocument, gPages);
    if (gScreenOn && noting::render::screenReady()) screenFrame(0, nullptr);
    return packEditResult(res);
}

emscripten::val editKey(int key, bool shift, bool ctrl, bool alt) {
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    noting::render::PageFillGuard guard;
    gUseLineCache = false;
    const noting::render::EditResult res = noting::render::editKeyDown(
        gDocument, gPages, static_cast<noting::render::EditKey>(key), shift, ctrl, alt, gStoredLineHeight);
    if (res.documentChanged) {
        syncEditedDocument(res);
    } else if (res.handled && res.caret.hit) {
        noting::render::ViewPlan plan = gCamera.plan(!gScreenOn);
        if (ensureCaretVisibleInCamera(res, plan)) {
            if (gScreenOn) {
                gFrames.bakeVisibleNow(gDocument, gPages, plan);
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
    if (gScreenOn && noting::render::screenReady()) screenFrame(0, nullptr);
    return packEditResult(res);
}

emscripten::val licenseFontSwaps() {
    emscripten::val out = emscripten::val::array();
    for (const noting::layout::FontSwap& swap : noting::layout::licenseFontSwaps(gDocument)) {
        emscripten::val row = emscripten::val::object();
        row.set("from", swap.from);
        row.set("to", swap.to);
        out.call<void>("push", row);
    }
    return out;
}

// chosenVal 은 셸에서 고른 {막힌 글꼴: 바꿀 글꼴}이다. 고를 수 있는지는 코어가 본다.
emscripten::val editSwapFonts(const emscripten::val& chosenVal) {
    std::vector<std::pair<std::string, std::string>> chosen;
    if (!chosenVal.isUndefined() && !chosenVal.isNull()) {
        const emscripten::val keys = emscripten::val::global("Object").call<emscripten::val>("keys", chosenVal);
        const int count = keys["length"].as<int>();
        for (int i = 0; i < count; ++i) {
            const std::string from = keys[i].as<std::string>();
            const emscripten::val to = chosenVal[from];
            if (to.isString()) chosen.emplace_back(from, to.as<std::string>());
        }
    }
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    noting::render::PageFillGuard guard;
    gUseLineCache = false;
    const noting::render::EditResult res =
        noting::render::editSwapLicenseFonts(gDocument, gPages, gStoredLineHeight, chosen);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    if (gScreenOn && noting::render::screenReady()) screenFrame(0, nullptr);
    return packEditResult(res);
}

// 셸이 준 객체의 이름·값을 그대로 코어에 넘긴다. 이름과 단위는 코어(setFormatField)가 정한다.
emscripten::val editStyle(const emscripten::val& patchVal) {
    noting::render::FormatPatch patch;
    if (!patchVal.isUndefined() && !patchVal.isNull()) {
        const emscripten::val keys = emscripten::val::global("Object").call<emscripten::val>("keys", patchVal);
        const int count = keys["length"].as<int>();
        for (int i = 0; i < count; ++i) {
            const std::string key = keys[i].as<std::string>();
            const emscripten::val value = patchVal[key];
            const std::string type = value.typeOf().as<std::string>();
            if (type == "boolean") {
                noting::render::setFormatField(patch, key, value.as<bool>());
            } else if (type == "number") {
                noting::render::setFormatField(patch, key, value.as<double>());
            } else if (type == "string") {
                noting::render::setFormatField(patch, key, value.as<std::string>());
            }
        }
    }
    const bool wasCrew = crewRunning();
    if (wasCrew) pauseCrewForEdit();
    noting::render::PageFillGuard guard;
    gUseLineCache = false;
    const noting::render::EditResult res =
        noting::render::editApplyFormat(gDocument, gPages, patch, gStoredLineHeight);
    if (res.documentChanged) syncEditedDocument(res);
    if (wasCrew) {
        resumeCrewAfterEdit(res);
    }
    if (gScreenOn && noting::render::screenReady()) screenFrame(0, nullptr);
    return packEditResult(res);
}

void retainViewTile(int id, int page) {
    if (!gScreenOn || id <= 0 || gViewFrame.rgba.empty()) return;
    gFrames.retain(id, page, gViewFrame);
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

bool gScreenLoop = false;
bool gPinchPlanPending = false;
double gFrameClock = 0;
std::atomic<bool> gUserInteracting{false};
bool crewRunning();
void setCrewInteracting(bool interacting);
bool stepSingleThreadForeground();
bool stepSingleThreadIdle();
ShownFrame gShown;
std::vector<ShownTile> gShownDraw;
std::vector<ShownPage> gShownPages;

void collectShownPages(const noting::render::ViewPlan& plan, std::vector<ShownPage>& out) {
    out.clear();
    const int width = plan.viewW;
    const int height = plan.viewH;
    if (width <= 0 || height <= 0) return;
    const int prefetchMargin = std::max(height / 2, 320);
    for (const noting::render::ViewPage& page : plan.pages) {
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
    if (index < 0 || index >= static_cast<int>(gPages.size())) return false;
    const noting::render::Node& page = gPages[static_cast<std::size_t>(index)];
    return page.spansLater || page.contentHash == 0;
}

bool visibleSpansPending(const noting::render::ViewPlan& plan) {
    if (plan.viewW <= 1 || plan.viewH <= 1) return false;
    for (const noting::render::ViewPage& page : plan.pages) {
        if (page.w <= 0 || page.h <= 0) continue;
        if (page.x >= plan.scrollX + plan.viewW || page.x + page.w <= plan.scrollX ||
            page.y >= plan.scrollY + plan.viewH || page.y + page.h <= plan.scrollY) {
            continue;
        }
        if (pageNeedsSpans(page.index)) return true;
    }
    return false;
}

bool sameShownDraw(const noting::render::ViewPlan& plan) {
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
        const noting::render::ViewDraw& draw = plan.draw[i];
        const ShownTile& shown = gShownDraw[i];
        if (draw.id != shown.id || gFrames.stamp(draw.id) != shown.stamp || draw.x != shown.x || draw.y != shown.y ||
            draw.w != shown.w || draw.h != shown.h) {
            return false;
        }
    }
    return true;
}

void rememberShownDraw(const noting::render::ViewPlan& plan) {
    if (gScreenOn) {
        collectShownPages(plan, gShownPages);
    }
    gShownDraw.resize(plan.draw.size());
    for (std::size_t i = 0; i < plan.draw.size(); ++i) {
        const noting::render::ViewDraw& draw = plan.draw[i];
        ShownTile& shown = gShownDraw[i];
        shown.id = draw.id;
        shown.stamp = gFrames.stamp(draw.id);
        shown.x = draw.x;
        shown.y = draw.y;
        shown.w = draw.w;
        shown.h = draw.h;
    }
}

int caretPhaseNow() { return noting::render::caretBlinkPhase(); }

int currentCameraPage(int scrollX, int scrollY, int percent) {
    if (!gLastPlan.pages.empty() && (percent <= 0 || percent == gLastPlan.percent)) {
        const double centerX = static_cast<double>(scrollX) + static_cast<double>(std::max(1, gLastPlan.viewW)) / 2.0;
        const double centerY = static_cast<double>(scrollY) + static_cast<double>(std::max(1, gLastPlan.viewH)) / 2.0;
        int bestPage = gLastPlan.pages.front().index;
        double bestDistSq = std::numeric_limits<double>::infinity();
        for (const noting::render::ViewPage& p : gLastPlan.pages) {
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
    const noting::render::ViewPlan plan = gCamera.plan(false);
    if (plan.page >= 0) return plan.page;
    return gLastPlan.page >= 0 ? gLastPlan.page : 0;
}

void resizeScreen(int width, int height) {
    EM_ASM(
        {
            const canvas = Module["canvas"];
            if (!canvas || $0 <= 0 || $1 <= 0) return;
            if (canvas.width !== $0 || canvas.height !== $1) {
                canvas.width = $0;
                canvas.height = $1;
            }
        },
        width, height);
}

void rememberShellCamera(int percent) {
    const noting::render::ViewportState cam = gCamera.state();
    gShellNoteW = gLastPlan.viewW;
    gShellNoteH = gLastPlan.viewH;
    gShellNoteMaxX = cam.maxScrollX;
    gShellNoteMaxY = cam.maxScrollY;
    gShellNotePercent = percent;
}

bool shellCameraDue(int percent) {
    const noting::render::ViewportState cam = gCamera.state();
    return gLastPlan.viewW != gShellNoteW || gLastPlan.viewH != gShellNoteH || cam.maxScrollX != gShellNoteMaxX ||
           cam.maxScrollY != gShellNoteMaxY || percent != gShellNotePercent;
}

void noteCamera(int scrollX, int scrollY, int percent, bool coasting, bool pinching) {
    const int page = currentCameraPage(scrollX, scrollY, percent);
    const int pageCount = static_cast<int>(gPages.size());
    const int fitPage = gCamera.isFitPage() ? 1 : 0;
    const int fitWidth = gCamera.isFitWidth() ? 1 : 0;
    const noting::render::ViewportState cam = gCamera.state();
    const double dpr = gLastPlan.dpr > 0 ? gLastPlan.dpr : 1;
    EM_ASM(
        {
            const hook = Module["onCamera"];
            if (typeof hook === "function") hook($0, $1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13);
        },
        scrollX, scrollY, percent, coasting ? 1 : 0, pinching ? 1 : 0, page, pageCount, fitPage, fitWidth,
        gLastPlan.viewW, gLastPlan.viewH, cam.maxScrollX, cam.maxScrollY, dpr);
    rememberShellCamera(percent);
}

void noteCameraIf(bool force, int scrollX, int scrollY, int percent, bool coasting, bool pinching) {
    if (!force && !shellCameraDue(percent)) return;
    noteCamera(scrollX, scrollY, percent, coasting, pinching);
}

// requestAnimationFrame 이 넘기는 시각과 같은 기준이다. 스레드 빌드의 emscripten_get_now() 는
// 스레드끼리 맞추려고 기준이 달라서, 프레임 시각과 빼면 음수가 되어 "방금 움직임"이 풀리지 않는다.
double frameNow() { return emscripten_performance_now(); }

bool screenFrame(double time, void*) {
    if (!gScreenOn || !noting::render::screenReady()) {
        drainDone();
        return true;
    }
    if (time <= 0) time = frameNow();

    bool coastMoved = false;
    bool coastZoomed = false;
    bool coastEnded = false;
    if (gInput.coasting() && gFrameClock > 0) {
        double dt = time - gFrameClock;
        if (dt > 48) dt = 48;
        if (dt > 0) {
            const noting::render::ViewportState before = gCamera.state();
            gInput.tick(gCamera, dt);
            const noting::render::ViewportState after = gCamera.state();
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
        if (noting::render::selectionEdgeScroll(edgeDt, gLastPlan.viewW, gLastPlan.viewH, edgeDx, edgeDy)) {
            gCamera.scrollBy(edgeDx, edgeDy);
            double vx = 0;
            double vy = 0;
            noting::render::selectionEdgePoint(vx, vy);
            int edgePage = -1;
            int edgeX = 0;
            int edgeY = 0;
            if (paperPoint(vx, vy, true, edgePage, edgeX, edgeY)) {
                noting::render::PointerSample sample;
                sample.time = time;
                sample.dpr = gLastPlan.dpr > 0 ? gLastPlan.dpr : 1;
                sample.viewX = vx;
                sample.viewY = vy;
                if (edgePage >= 0 && edgePage < static_cast<int>(gLastPlan.pages.size())) {
                    for (const noting::render::ViewPage& sheet : gLastPlan.pages) {
                        if (sheet.index != edgePage || sheet.w <= 0) continue;
                        const noting::render::Node& node = gPages[static_cast<std::size_t>(edgePage)];
                        sample.huPerLogical = node.width > 0 ? (static_cast<double>(node.width) / sheet.w) * sample.dpr : 0;
                        break;
                    }
                }
                noting::render::applyPointer(gDocument, gPages, 2, edgePage, edgeX, edgeY, gCamera.state().percent, sample);
            }
            coastMoved = true;
        }
    }

    // 웹이 view() 로 쪽 배치를 넣기 전에 프레임이 돌 수 있다. 카메라에 쪽이 있으면 여기서 잡는다.
    if (gViewportDirty || gLastPlan.pages.empty() || gLastPlan.viewW <= 1 || gLastPlan.viewH <= 1) {
        noting::render::ViewPlan seeded = gCamera.plan(false);
        if (seeded.pages.empty() || seeded.viewW <= 1 || seeded.viewH <= 1) {
            drainDone();
            return true;
        }
        gViewportDirty = false;
        useFramePlan(seeded);
        gLastPlan = std::move(seeded);
        publishPlan();
    }

    const noting::render::ViewportState camera = gCamera.state();
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
    const bool selecting = gInput.selecting() || noting::render::pointerTracking();
    setCrewInteracting(interacting || selecting);
    if (!selecting) {
        drainDone();
    }

    const int revision = gFrames.revision();
    const int picture = noting::render::pointerPicture();
    const int phase = noting::render::caretShown() ? caretPhaseNow() : -1;

    const bool masks = !selecting && (noting::render::glyphMasksPending() || noting::render::pictureUploadsPending());
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
    const bool notifyCam = coastMoved || coastZoomed || coastEnded || finishInteract;
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
        if (cameraMoved || tilesStale || selectionStale || blink || maskStale || gPageGpuBakePending || filterStale) {
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
            gShown.masks = noting::render::glyphMasksPending() || noting::render::pictureUploadsPending();
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
        if (!crewRunning() && !interacting && !selecting) {
            stepSingleThreadIdle();
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
        noting::render::ViewPlan plan = gCamera.plan(false);
        useFramePlan(plan);
        gLastPlan = std::move(plan);
        if (!crewRunning() && !interacting && !selecting && gLastPlan.job.id != 0 && !gLastPlan.job.background) {
            if (stepSingleThreadForeground()) {
                noting::render::ViewPlan updated = gCamera.plan(false);
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
    gShown.masks = noting::render::glyphMasksPending() || noting::render::pictureUploadsPending();
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
    emscripten_request_animation_frame_loop(screenFrame, nullptr);
}

bool attachScreen() {
    const bool opened = noting::render::resetScreenSurface();
    gScreenOn = opened;
    if (opened) ensureScreenFrames();
    return opened;
}

bool screenReady() {
    const bool ready = noting::render::screenReady();
    gScreenOn = ready;
    return ready;
}

int framePxAt(std::int32_t hu, float scale) {
    return static_cast<int>(hu * scale + (hu < 0 ? -0.5f : 0.5f));
}

bool presentView(int viewW, int viewH, int scrollX, int scrollY, int layoutZoom, bool dragging) {
    if (!gScreenOn) return false;
    const noting::render::ViewPlan& plan = gLastPlan;
    scrollX = std::clamp(scrollX, 0, std::max(0, plan.maxScrollX));
    scrollY = std::clamp(scrollY, 0, std::max(0, plan.maxScrollY));
    const int width = viewW > 0 ? viewW : plan.viewW;
    const int height = viewH > 0 ? viewH : plan.viewH;
    std::vector<noting::render::ScreenSprite> papers;
    std::vector<noting::render::ScreenSprite> sprites;
    std::vector<const noting::render::ViewPage*> visPages;
    std::vector<bool> inViewFlags;
    papers.reserve(plan.pages.size());
    visPages.reserve(32);
    inViewFlags.reserve(32);
    const int prefetchMargin = dragging ? 0 : std::max(height / 2, 320);
    int inViewPageCount = 0;
    for (const noting::render::ViewPage& page : plan.pages) {
        noting::render::ScreenSprite paper;
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
    const bool scaleOnly = noting::render::overviewZoom(inViewPageCount, zoom);
    const int targetPercent = (scaleOnly || zoom < 100) ? 100 : zoom;

    auto betterPercent = [](int cand, int cur, int target) {
        if (cand == cur) return false;
        if (cand == target) return true;
        if (cur == target) return false;
        if (target == 100) return cand < cur;
        if (cand == 100) return false;
        if (cur == 100) return true;
        const int dc = std::abs(cand - target);
        const int d0 = std::abs(cur - target);
        if (dc != d0) return dc < d0;
        return cand > cur;
    };

    std::vector<PageSpriteCache*> visCaches;
    visCaches.reserve(visPages.size());
    std::size_t totalCandidateSprites = 0;
    static std::unordered_map<std::string_view, std::size_t> placedByKey;

    for (const noting::render::ViewPage* pagePtr : visPages) {
        const noting::render::ViewPage& page = *pagePtr;
        const int pageRev = gFrames.pageRevision(page.index);
        PageSpriteCache& pCache = gPageSpriteCaches[page.index];
        const bool needRebuild =
            pCache.pageRevision != pageRev ||
            (pCache.multiPercent && !dragging && pCache.targetPercent != targetPercent);
        if (needRebuild) {
            pCache.sprites.clear();
            placedByKey.clear();
            int firstTilePercent = -1;
            bool multiPercent = false;
            gFrames.forReadyPageTiles(page.index, [&](const noting::render::FrameCache::ReadyTile& ready) {
                if (!ready.desc || !ready.bitmap) return;
                const noting::render::FrameDesc& frame = *ready.desc;
                const int framePercent = ready.percent;
                const bool hasBitmap =
                    ready.bitmap->width > 1 && ready.bitmap->height > 1 && !ready.bitmap->rgba.empty();
                if (!hasBitmap && !ready.hasGlyphs) return;
                if (frame.w <= 0 || frame.h <= 0) return;
                if (firstTilePercent < 0) firstTilePercent = framePercent;
                else if (firstTilePercent != framePercent) multiPercent = true;
                const std::string_view key = ready.key ? std::string_view(*ready.key) : std::string_view{};
                const auto found = key.empty() ? placedByKey.end() : placedByKey.find(key);
                if (found != placedByKey.end()) {
                    if (!betterPercent(framePercent, pCache.sprites[found->second].percent, targetPercent)) return;
                }
                LocalPageSprite item;
                item.id = frame.id;
                item.stamp = ready.stamp;
                item.frameX = frame.x;
                item.frameY = frame.y;
                item.frameW = frame.w;
                item.frameH = frame.h;
                item.srcPixelH = frame.pixelHeight;
                item.localX = frame.x;
                item.localY = frame.y;
                item.w = frame.w;
                item.h = frame.h;
                item.sw = ready.bitmap->width;
                item.sh = ready.bitmap->height;
                item.nearest = frame.kind >= 3;
                item.percent = framePercent;
                item.rgba = hasBitmap ? ready.bitmap->rgba.data() : nullptr;
                item.layer = std::clamp(frame.layer, 0, 3);
                item.z = frame.z;
                item.rank = frame.rank;
                item.huX = frame.huX;
                item.huY = frame.huY;
                item.huW = frame.huW;
                item.huH = frame.huH;
                if (found != placedByKey.end()) {
                    pCache.sprites[found->second] = item;
                } else {
                    if (!key.empty()) placedByKey.emplace(key, pCache.sprites.size());
                    pCache.sprites.push_back(item);
                }
            });
            std::stable_sort(pCache.sprites.begin(), pCache.sprites.end(),
                             [](const LocalPageSprite& a, const LocalPageSprite& b) {
                                 if (a.layer != b.layer) return a.layer < b.layer;
                                 if (a.z != b.z) return a.z < b.z;
                                 if (a.rank != b.rank) return a.rank < b.rank;
                                 if (a.huY != b.huY) return a.huY < b.huY;
                                 if (a.huX != b.huX) return a.huX < b.huX;
                                 if (a.frameY != b.frameY) return a.frameY < b.frameY;
                                 return a.frameX < b.frameX;
                             });
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

    const int pageCacheZoom = noting::render::pageCachePercent(zoom);
    const bool activeZooming = dragging && (gInput.pinchActive() || (gShown.ok && zoom != gShown.percent));
    // 목표로 다가가는 중(휠 칸, 배율 단추)이면 목표 배율 마스크만 굽고, 손을 따라가면 지금 배율 칸을 따라간다.
    // 멈추면 쪽 텍스처에 1:1로 찍히는 배율이다.
    const int zoomAim = gInput.zoomAim();
    const int maskZoom = activeZooming ? noting::render::maskZoomPercent(zoomAim > 0 ? zoomAim : zoom, true)
                                       : noting::render::restingMaskZoom(zoom);
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
        const noting::render::ViewPage& page = *visPages[p];
        int cachePageW = page.w;
        int cachePageH = page.h;
        if (pageCacheZoom != zoom) {
            const bool validPage = page.index >= 0 && page.index < static_cast<int>(gPages.size());
            const int pageWidthHu = validPage ? gPages[static_cast<std::size_t>(page.index)].width : 0;
            const int pageHeightHu = validPage ? gPages[static_cast<std::size_t>(page.index)].height : 0;
            cachePageH =
                pageHeightHu > 0
                    ? noting::render::pagePixelHeight(pageHeightHu, pageCacheZoom)
                    : std::max(1, static_cast<int>(std::lround(static_cast<double>(page.h) *
                                                               static_cast<double>(pageCacheZoom) /
                                                               static_cast<double>(zoom))));
            cachePageW =
                (pageWidthHu > 0 && pageHeightHu > 0)
                    ? noting::render::pagePixelWidth(pageWidthHu, pageHeightHu, cachePageH)
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
            const noting::render::ViewPage& page = *visPages[p];
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
                    noting::render::hasScalablePageGpuTexture(page.index, pCache.pageStamp)) {
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
            const int texZoom = noting::render::pageGpuTextureZoom(page.index, pCache.pageStamp);
            const bool texTooSmall = pass == 0 && texZoom > 0 && texZoom < pageCacheZoom && !pCache.sprites.empty() &&
                                     (!dragging || directSpritesUsed + pCache.sprites.size() <=
                                                       kMaxDirectSpritesWhileDragging);
            if (texTooSmall) drawDirect[p] = 1;
            if (!texTooSmall && !pageNeedsSpans(page.index) &&
                noting::render::canUsePageGpuTexture(page.index, cachePageW, cachePageH, pageCacheZoom, maskZoom,
                                                     pCache.pageStamp, dragging)) {
                pageModes[p] = 2;
                continue;
            }
            const bool spansOpen = pageNeedsSpans(page.index);
            const bool hasScalable = !spansOpen && !texTooSmall &&
                                     noting::render::hasScalablePageGpuTexture(page.index, pCache.pageStamp);
            const bool hasAnyTex = hasScalable;
            if (pass == 0 && hasAnyTex) continue;
            if (pass == 1 && !hasAnyTex) continue;
            const bool canBakePage =
                noting::render::canBakePageGpuTexture(page.index, cachePageW, cachePageH, pCache.pageStamp);
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
        const noting::render::ViewPage& page = *visPages[p];
        PageSpriteCache& pCache = *visCaches[p];
        const int pageHeightHu =
            (page.index >= 0 && page.index < static_cast<int>(gPages.size()))
                ? gPages[static_cast<std::size_t>(page.index)].height
                : 0;
        const int spriteZoom = (mode == 1) ? pageCacheZoom : zoom;
        const int dstPixelH =
            (mode == 1 && papers[p].sh > 0)
                ? papers[p].sh
                : ((spriteZoom == plan.percent && page.h > 0)
                       ? page.h
                       : (pageHeightHu > 0 ? noting::render::pagePixelHeight(pageHeightHu, spriteZoom) : page.h));
        if (pCache.zoom == spriteZoom && pCache.dstPixelH == dstPixelH) continue;
        const float dstScale =
            (pageHeightHu > 0 && dstPixelH > 0)
                ? (static_cast<float>(dstPixelH) / static_cast<float>(pageHeightHu))
                : 0.f;
        for (LocalPageSprite& item : pCache.sprites) {
            int localX = item.frameX;
            int localY = item.frameY;
            int localW = item.frameW;
            int localH = item.frameH;
            const int srcPixelH = item.srcPixelH;
            if (item.percent != spriteZoom || (srcPixelH > 0 && dstPixelH > 0 && dstPixelH != srcPixelH)) {
                if (dstPixelH > 0 && srcPixelH > 0) {
                    int dstX = item.frameX;
                    int dstY = item.frameY;
                    if (dstScale > 0.f) {
                        dstX = framePxAt(item.huX, dstScale) - noting::render::kFramePad;
                        dstY = framePxAt(item.huY, dstScale) - noting::render::kFramePad;
                    } else {
                        const double ratio = static_cast<double>(dstPixelH) / static_cast<double>(srcPixelH);
                        dstX = static_cast<int>(
                                   std::lround(static_cast<double>(item.frameX + noting::render::kFramePad) * ratio)) -
                               noting::render::kFramePad;
                        dstY = static_cast<int>(
                                   std::lround(static_cast<double>(item.frameY + noting::render::kFramePad) * ratio)) -
                               noting::render::kFramePad;
                    }
                    const int srcW = item.sw > 1 ? item.sw : item.frameW;
                    const int srcH = item.sh > 1 ? item.sh : item.frameH;
                    noting::render::placeZoomedFrame(dstX, dstY, dstPixelH, srcPixelH, srcW, srcH, localX, localY,
                                                     localW, localH);
                }
            }
            item.localX = localX;
            item.localY = localY;
            item.w = std::max(1, localW);
            item.h = std::max(1, localH);
        }
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
            const noting::render::ViewPage& page = *visPages[p];
            const PageSpriteCache& pCache = *visCaches[p];
            const int pageScreenX = page.x - scrollX;
            const int pageScreenY = page.y - scrollY;
            const std::size_t begin = pCache.layerOffset[layer];
            const std::size_t end = pCache.layerOffset[layer + 1];
            if (begin >= end) continue;
            if (mode == 1) {
                for (std::size_t i = begin; i < end; ++i) {
                    const LocalPageSprite& item = pCache.sprites[i];
                    noting::render::ScreenSprite s;
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
                noting::render::ScreenSprite s;
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

    noting::render::Scrollbar barX;
    noting::render::Scrollbar barY;
    noting::render::layoutScrollbars(barX, barY, width, height, plan.dpr, scrollX, scrollY, plan.maxScrollX,
                                     plan.maxScrollY, plan.contentW, plan.contentH, plan.ruler);
    return noting::render::presentScreen(width, height, papers, sprites, layoutZoom, maskZoom, plan.pages, scrollX,
                                         scrollY, barX, barY, dragging, plan.ruler, plan.dpr);
}

bool glyphMasksPending() { return noting::render::glyphMasksPending(); }

void useLineCache(bool on) {
    gUseLineCache = on;
    clearPageSlots();
}

std::string lineCacheScene(int pageIndex) {
    gUseLineCache = true;
    auto built = noting::render::lineCachePage(gDocument, pageIndex);
    const int count = std::max(built.count, 0);
    if (static_cast<int>(gPages.size()) != count) {
        gPages.assign(static_cast<std::size_t>(count), {});
        gPagePng.assign(gPages.size(), {});
        gPagePixels.assign(gPages.size(), {});
    }
    noting::JsonWriter writer(false);
    writer.beginObject();
    writer.field("count", count);
    writer.key("page");
    if (pageIndex >= 0 && pageIndex < count && built.page.width > 0) {
        gPages[static_cast<std::size_t>(pageIndex)] = std::move(built.page);
        writeScene(writer, gPages[static_cast<std::size_t>(pageIndex)]);
    } else {
        writer.beginObject();
        writer.endObject();
    }
    writer.endObject();
    return writer.take();
}

void ensurePageCache(int pixelHeight) {
    if (gUseLineCache) {
        if (gPages.empty() && !gDocument.sections.empty()) ensureLineCachePage(0);
        if (pixelHeight != gPixelHeight) {
            gPagePng.assign(gPages.size(), {});
            gPagePixels.assign(gPages.size(), {});
            gPixelHeight = pixelHeight;
        }
        return;
    }
    if (gPages.empty() && !gDocument.sections.empty()) gPages = noting::render::layoutPages(gDocument);
    if (pixelHeight != gPixelHeight) {
        gPagePng.assign(gPages.size(), {});
        gPagePixels.assign(gPages.size(), {});
        gPixelHeight = pixelHeight;
    }
}

emscripten::val renderPage(int page, int pixelHeight) {
    if (gUseLineCache) ensureLineCachePage(page);
    ensurePageCache(pixelHeight);
    if (page < 0 || page >= static_cast<int>(gPages.size())) {
        return emscripten::val(emscripten::typed_memory_view(0, static_cast<const std::uint8_t*>(nullptr)));
    }
    if (gPagePng[static_cast<std::size_t>(page)].empty()) {
        const noting::render::PageImage image =
            noting::render::renderPagePng(gDocument, gPages[static_cast<std::size_t>(page)], pixelHeight);
        gPagePng[static_cast<std::size_t>(page)] = image.png;
    }
    const auto& png = gPagePng[static_cast<std::size_t>(page)];
    return emscripten::val(emscripten::typed_memory_view(png.size(), png.data()));
}

emscripten::val renderPagePixels(int page, int pixelHeight) {
    if (gUseLineCache) ensureLineCachePage(page);
    ensurePageCache(pixelHeight);
    emscripten::val result = emscripten::val::object();
    if (page < 0 || page >= static_cast<int>(gPages.size())) {
        result.set("width", 0);
        result.set("height", 0);
        result.set("pixels", emscripten::val(emscripten::typed_memory_view(0, static_cast<const std::uint8_t*>(nullptr))));
        return result;
    }
    noting::render::PageBitmap& frame = gPagePixels[static_cast<std::size_t>(page)];
    if (frame.rgba.empty()) {
        frame = noting::render::renderPageBitmap(gDocument, gPages[static_cast<std::size_t>(page)], pixelHeight);
    }
    result.set("width", frame.width);
    result.set("height", frame.height);
    result.set("pixels", emscripten::val(emscripten::typed_memory_view(frame.rgba.size(), frame.rgba.data())));
    return result;
}

emscripten::val renderPageView(int page, int pixelHeight, int viewX, int viewY, int viewW, int viewH,
                               int layer) {
    if (gUseLineCache) ensureLineCachePage(page);
    ensurePageCache(pixelHeight);
    emscripten::val result = emscripten::val::object();
    if (page < 0 || page >= static_cast<int>(gPages.size())) {
        result.set("width", 0);
        result.set("height", 0);
        result.set("pixels", emscripten::val(emscripten::typed_memory_view(0, static_cast<const std::uint8_t*>(nullptr))));
        return result;
    }
    const auto pass = layer >= 0 && layer < noting::render::kPaintLayerCount
                          ? static_cast<noting::render::PaintLayer>(layer)
                          : noting::render::PaintLayer::All;
    gViewFrame = noting::render::renderPageBitmap(
        gDocument, gPages[static_cast<std::size_t>(page)], pixelHeight, viewX, viewY, viewW, viewH, pass, page);
    result.set("width", gViewFrame.width);
    result.set("height", gViewFrame.height);
    result.set("pixels", emscripten::val(emscripten::typed_memory_view(gViewFrame.rgba.size(), gViewFrame.rgba.data())));
    return result;
}

}  // namespace

bool defined(const emscripten::val& value) {
    return !value.isUndefined() && !value.isNull();
}

void useFramePlan(noting::render::ViewPlan& plan) {
    int visiblePages = 0;
    int minPageH = 0;
    for (const noting::render::ViewPage& page : plan.pages) {
        if (page.w <= 0 || page.h <= 0) continue;
        if (minPageH <= 0 || page.h < minPageH) minPageH = page.h;
        if (plan.viewW > 0 && plan.viewH > 0 && page.x < plan.scrollX + plan.viewW && page.x + page.w > plan.scrollX &&
            page.y < plan.scrollY + plan.viewH && page.y + page.h > plan.scrollY) {
            ++visiblePages;
        }
    }
    const bool overview = noting::render::overviewZoom(visiblePages, plan.percent);
    const int margin = (overview && minPageH > 0)
                           ? std::clamp(std::min(plan.viewH, std::max(minPageH * 2, 320)), 1, std::max(plan.viewH, 1))
                           : std::max(plan.viewH, 1);
    const bool crewActive = gScreenOn && crewRunning();
    const bool asyncLayoutOnly = crewActive && overview;
    int syncBuilt = 0;
    for (const noting::render::ViewPage& page : plan.pages) {
        const bool near = page.w > 0 && page.h > 0 && plan.viewW > 0 && plan.viewH > 0 &&
                          page.x < plan.scrollX + plan.viewW + margin && page.x + page.w > plan.scrollX - margin &&
                          page.y < plan.scrollY + plan.viewH + margin && page.y + page.h > plan.scrollY - margin;
        if (!near) continue;
        if (page.index < 0 || page.index >= static_cast<int>(gPages.size())) continue;
        const auto& pageNode = gPages[static_cast<std::size_t>(page.index)];
        const bool onScreen = page.x < plan.scrollX + plan.viewW && page.x + page.w > plan.scrollX &&
                              page.y < plan.scrollY + plan.viewH && page.y + page.h > plan.scrollY;
        const bool alreadyLaidOut = !pageNode.children.empty();
        const bool spansReady = !pageNode.spansLater && alreadyLaidOut && pageNode.contentHash != 0;
        if (spansReady) continue;
        // 보이는 쪽은 배경 스레드가 켜져 있어도 글자를 채운 뒤에 그린다.
        // 화면 밖 여유 쪽만 한 프레임에 하나만 채운다.
        if (!onScreen && crewActive && (syncBuilt >= 1 || (!alreadyLaidOut && asyncLayoutOnly))) continue;
        if (onScreen && !alreadyLaidOut && asyncLayoutOnly) continue;
        if (gUseLineCache) ensureLineCachePage(page.index);
        ensurePageSpans(page.index);
        ++syncBuilt;
    }
    gFrames.plan(plan, gPages, gReadyPages, &gDocument, crewActive);
    gLastPlanRevision = gFrames.revision();
}

const char* gridModeName(noting::render::PageGridMode mode) {
    switch (mode) {
        case noting::render::PageGridMode::Two: return "two";
        case noting::render::PageGridMode::Auto: return "auto";
        case noting::render::PageGridMode::Grid: return "grid";
        case noting::render::PageGridMode::Single:
        default: return "single";
    }
}

noting::render::PageGridMode parseGridMode(const std::string& mode) {
    if (mode == "two" || mode == "2") return noting::render::PageGridMode::Two;
    if (mode == "auto") return noting::render::PageGridMode::Auto;
    if (mode == "grid") return noting::render::PageGridMode::Grid;
    return noting::render::PageGridMode::Single;
}

emscripten::val viewApply(const emscripten::val& msg) {
    drainDone();
    if (defined(msg["reset"]) && msg["reset"].as<bool>()) {
        gCamera.reset();
        gInput.stop();
    }
    const emscripten::val viewport = msg["viewport"];
    if (defined(viewport)) {
        gCamera.setViewport(viewport["cssW"].as<int>(), viewport["cssH"].as<int>(), viewport["dpr"].as<double>());
    }
    const emscripten::val pages = msg["pages"];
    const emscripten::val page = msg["page"];
    if (defined(pages)) {
        std::vector<noting::render::PageSpec> specs;
        const int count = pages["length"].as<int>();
        specs.reserve(static_cast<std::size_t>(std::max(0, count)));
        for (int i = 0; i < count; ++i) {
            const emscripten::val item = pages[i];
            noting::render::PageSpec spec;
            spec.index = item["index"].as<int>();
            spec.widthHu = item["widthHu"].as<int>();
            spec.heightHu = item["heightHu"].as<int>();
            const bool validNode = spec.index >= 0 && spec.index < static_cast<int>(gPages.size());
            spec.marginLeftHu = defined(item["marginLeftHu"])
                                    ? item["marginLeftHu"].as<int>()
                                    : (validNode ? gPages[static_cast<std::size_t>(spec.index)].marginLeft
                                                 : noting::render::kDefaultPageMarginLeftHu);
            spec.marginRightHu = defined(item["marginRightHu"])
                                     ? item["marginRightHu"].as<int>()
                                     : (validNode ? gPages[static_cast<std::size_t>(spec.index)].marginRight
                                                  : noting::render::kDefaultPageMarginRightHu);
            spec.marginTopHu = defined(item["marginTopHu"])
                                   ? item["marginTopHu"].as<int>()
                                   : (validNode ? gPages[static_cast<std::size_t>(spec.index)].marginTop
                                                : noting::render::defaultPageMarginTopHu(static_cast<std::size_t>(i)));
            spec.marginBottomHu = defined(item["marginBottomHu"])
                                      ? item["marginBottomHu"].as<int>()
                                      : (validNode ? gPages[static_cast<std::size_t>(spec.index)].marginBottom
                                                   : noting::render::defaultPageMarginBottomHu(
                                                         static_cast<std::size_t>(i),
                                                         static_cast<std::size_t>(std::max(1, count))));
            specs.push_back(spec);
        }
        const int gap = defined(msg["gap"]) ? msg["gap"].as<int>() : 0;
        gLastPageGap = gap;
        gCamera.setPages(specs, gap);
    } else if (defined(page)) {
        const int idx = page["index"].as<int>();
        gCamera.setPage(idx, page["widthHu"].as<int>(), page["heightHu"].as<int>());
        const bool validNode = idx >= 0 && idx < static_cast<int>(gPages.size());
        const int ml = defined(page["marginLeftHu"])
                           ? page["marginLeftHu"].as<int>()
                           : (validNode ? gPages[static_cast<std::size_t>(idx)].marginLeft
                                        : noting::render::kDefaultPageMarginLeftHu);
        const int mr = defined(page["marginRightHu"])
                           ? page["marginRightHu"].as<int>()
                           : (validNode ? gPages[static_cast<std::size_t>(idx)].marginRight
                                        : noting::render::kDefaultPageMarginRightHu);
        const int mt = defined(page["marginTopHu"])
                           ? page["marginTopHu"].as<int>()
                           : (validNode ? gPages[static_cast<std::size_t>(idx)].marginTop
                                        : noting::render::defaultPageMarginTopHu(0));
        const int mb = defined(page["marginBottomHu"])
                           ? page["marginBottomHu"].as<int>()
                           : (validNode ? gPages[static_cast<std::size_t>(idx)].marginBottom
                                        : noting::render::defaultPageMarginBottomHu(0, 1));
        gCamera.setPageMargins(ml, mr, mt, mb);
    }
    bool fitOrStepApplied = false;
    const emscripten::val grid = msg["grid"];
    if (defined(grid)) {
        const std::string modeStr = defined(grid["mode"]) ? grid["mode"].as<std::string>() : std::string("auto");
        const int cols = defined(grid["cols"]) ? grid["cols"].as<int>() : 1;
        const int rows = defined(grid["rows"]) ? grid["rows"].as<int>() : 1;
        const bool fit = defined(grid["fit"]) ? grid["fit"].as<bool>() : (modeStr == "grid");
        gCamera.setGridMode(parseGridMode(modeStr), cols, rows, fit);
        gInput.stop();
        if (fit) fitOrStepApplied = true;
    }
    if (defined(msg["fit"])) {
        const std::string fitMode = msg["fit"].as<std::string>();
        if (fitMode == "width") {
            gCamera.fitWidth();
            gInput.stop();
            fitOrStepApplied = true;
        } else if (fitMode == "page") {
            gCamera.fitPage();
            gInput.stop();
            fitOrStepApplied = true;
        }
    }
    // 손가락, 관성, 휠, 휠 버튼 드래그, 스크롤바 드래그가 스크롤을 갖고 있으면 옛 배율·스크롤로 덮지 않는다.
    // 배율이 실제로 바뀌면 그 앵커만 남기고, 이전 줌의 스크롤 값은 넣지 않는다.
    if (!fitOrStepApplied && (defined(msg["zoom"]) || defined(msg["scroll"])) && !gInput.touchLive() &&
        !gInput.draggingBar() && !gInput.wheeling() && !gInput.panning() && !gInput.pinchActive() &&
        !gInput.coasting()) {
        const int before = gCamera.state().percent;
        const bool beforeFitPage = gCamera.isFitPage();
        const bool beforeFitWidth = gCamera.isFitWidth();
        if (defined(msg["zoom"])) gCamera.setZoomPercent(msg["zoom"].as<int>());
        const bool zoomed = gCamera.state().percent != before || gCamera.isFitPage() != beforeFitPage ||
                            gCamera.isFitWidth() != beforeFitWidth;
        gInput.stop();
        if (!zoomed) {
            const emscripten::val scroll = msg["scroll"];
            if (defined(scroll)) gCamera.setScroll(scroll["x"].as<int>(), scroll["y"].as<int>());
        }
    }
    if (defined(msg["pageStep"])) {
        const int dir = msg["pageStep"].as<int>();
        if (dir != 0) {
            gCamera.stepPage(dir);
            gInput.stop();
        }
    }
    int commitId = 0;
    if (defined(msg["commit"])) commitId = msg["commit"].as<int>();
    bool committed = false;
    if (commitId && gScreenOn) committed = gFrames.contains(commitId);
    else if (commitId) committed = gCamera.commit(commitId);

    noting::render::ViewPlan plan = gCamera.plan(!gScreenOn);
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
    emscripten::val out = emscripten::val::object();
    out.set("generation", plan.generation);
    out.set("dpr", plan.dpr);
    out.set("viewW", plan.viewW);
    out.set("viewH", plan.viewH);
    out.set("percent", plan.percent);
    out.set("fitPage", plan.fitPage);
    out.set("fitWidth", plan.fitWidth);
    out.set("zoomMode", plan.fitPage ? std::string("fitPage")
                                     : (plan.fitWidth ? std::string("fitWidth") : std::string("percent")));
    out.set("zoomLabel", plan.fitPage ? std::string("쪽 맞춤")
                                      : (plan.fitWidth ? std::string("폭 맞춤") : (std::to_string(plan.percent) + "%")));
    out.set("contentW", plan.contentW);
    out.set("contentH", plan.contentH);
    out.set("scrollX", plan.scrollX);
    out.set("scrollY", plan.scrollY);
    out.set("maxScrollX", plan.maxScrollX);
    out.set("maxScrollY", plan.maxScrollY);
    out.set("paperX", plan.paperX);
    out.set("paperY", plan.paperY);
    out.set("paperW", plan.paperW);
    out.set("paperH", plan.paperH);
    out.set("page", plan.page);
    out.set("gridMode", std::string(gridModeName(plan.gridMode)));
    out.set("gridCols", plan.gridCols);
    out.set("gridRows", plan.gridRows);
    out.set("effectiveCols", plan.effectiveCols);
    out.set("rowStepY", plan.rowStepY);
    out.set("pageStepY", plan.pageStepY);
    out.set("ruler", plan.ruler);
    auto packBar = [](const noting::render::Scrollbar& bar) {
        emscripten::val entry = emscripten::val::object();
        entry.set("x", bar.x);
        entry.set("y", bar.y);
        entry.set("w", bar.w);
        entry.set("h", bar.h);
        entry.set("thumbX", bar.thumbX);
        entry.set("thumbY", bar.thumbY);
        entry.set("thumbW", bar.thumbW);
        entry.set("thumbH", bar.thumbH);
        entry.set("visible", bar.visible);
        return entry;
    };
    out.set("barX", packBar(plan.barX));
    out.set("barY", packBar(plan.barY));
    emscripten::val pageBoxes = emscripten::val::array();
    for (const noting::render::ViewPage& item : plan.pages) {
        emscripten::val entry = emscripten::val::object();
        entry.set("index", item.index);
        entry.set("x", item.x);
        entry.set("y", item.y);
        entry.set("w", item.w);
        entry.set("h", item.h);
        entry.set("marginLeft", item.marginLeft);
        entry.set("marginRight", item.marginRight);
        entry.set("marginTop", item.marginTop);
        entry.set("marginBottom", item.marginBottom);
        entry.set("row", item.row);
        entry.set("col", item.col);
        entry.set("outerX", item.outerX());
        entry.set("outerY", item.outerY());
        entry.set("outerW", item.outerW());
        entry.set("outerH", item.outerH());
        pageBoxes.call<void>("push", entry);
    }
    out.set("pages", pageBoxes);
    out.set("committed", committed);
    emscripten::val drop = emscripten::val::array();
    emscripten::val draw = emscripten::val::array();
    if (!gScreenOn) {
        for (const int id : plan.drop) drop.call<void>("push", id);
        for (const noting::render::ViewDraw& item : plan.draw) {
            emscripten::val entry = emscripten::val::object();
            entry.set("id", item.id);
            entry.set("x", item.x);
            entry.set("y", item.y);
            entry.set("w", item.w);
            entry.set("h", item.h);
            entry.set("nearest", item.nearest);
            entry.set("layer", item.layer);
            draw.call<void>("push", entry);
        }
    }
    out.set("drop", drop);
    out.set("draw", draw);
    if (plan.job.id == 0) {
        out.set("job", emscripten::val::null());
    } else {
        emscripten::val job = emscripten::val::object();
        job.set("id", plan.job.id);
        job.set("generation", plan.job.generation);
        job.set("page", plan.job.page);
        job.set("percent", plan.job.percent);
        job.set("pixelHeight", plan.job.pixelHeight);
        job.set("col", plan.job.col);
        job.set("row", plan.job.row);
        job.set("x", plan.job.x);
        job.set("y", plan.job.y);
        job.set("w", plan.job.w);
        job.set("h", plan.job.h);
        job.set("contentW", plan.job.contentW);
        job.set("contentH", plan.job.contentH);
        job.set("layer", plan.job.layer);
        job.set("background", plan.job.background);
        if (gScreenOn && plan.job.id >= 2000000) job.set("frame", plan.job.id);
        out.set("job", job);
    }
    return out;
}

int idleOriginPage(const noting::render::ViewPlan& plan) {
    for (const noting::render::ViewPage& page : plan.pages) {
        if (page.x < plan.scrollX + plan.viewW && page.x + page.w > plan.scrollX && page.y < plan.scrollY + plan.viewH &&
            page.y + page.h > plan.scrollY) {
            return page.index;
        }
    }
    return plan.job.page;
}

bool jobOnScreen(const noting::render::ViewPlan& plan) {
    if (plan.job.id == 0) return false;
    for (const noting::render::ViewPage& page : plan.pages) {
        if (page.index != plan.job.page) continue;
        const int x = page.x + plan.job.x;
        const int y = page.y + plan.job.y;
        return x < plan.scrollX + plan.viewW && x + plan.job.w > plan.scrollX && y < plan.scrollY + plan.viewH &&
               y + plan.job.h > plan.scrollY;
    }
    return false;
}

namespace {

// 화면 스레드는 입력과 프레임만 본다. 나머지 스레드는 줄 자리, 마스크, 칸을 문서 끝까지 굽는다.
struct LineJob {
    int page = 0;
    int pixelH = 0;
    noting::render::FrameDesc frame;
    std::string key;
};

struct BakeJob {
    noting::render::FrameDesc frame;
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
    noting::render::FrameDesc frame;
    int page = 0;
    int percent = 100;
    int total = 0;
    noting::render::PageBitmap bitmap;
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
    std::vector<noting::render::ViewPage> pages;
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
#if defined(__EMSCRIPTEN_PTHREADS__)
    int cores = emscripten_num_logical_cores();
    if (cores <= 0) cores = static_cast<int>(std::thread::hardware_concurrency());
    if (cores < 2) cores = 2;
    return std::clamp(cores - 1, 1, 4);
#else
    return 0;
#endif
}

void setCrewInteracting(bool interacting) {
    noting::render::setRenderInteracting(interacting);
    const bool wasInteracting = gUserInteracting.exchange(interacting, std::memory_order_relaxed);
    if (wasInteracting && !interacting) {
        std::lock_guard<std::mutex> lock(gWorkMu);
        ++gTick;
        gWorkCv.notify_all();
    }
}

std::string zoomBakeKey(const noting::render::FrameDesc& frame) {
    std::string key = std::to_string(frame.page) + "/" + std::to_string(frame.kind);
    for (const int step : frame.path) key += "." + std::to_string(step);
    key += "@" + std::to_string(frame.percent > 0 ? frame.percent : 100);
    return key;
}

std::string lineHoldKey(const noting::render::FrameDesc& frame) {
    std::string key = std::to_string(frame.page);
    key.push_back(':');
    for (const int step : frame.path) {
        key += std::to_string(step);
        key.push_back('.');
    }
    return key;
}

bool pageOnScreen(const noting::render::ViewPage& page, const noting::render::ViewPlan& plan) {
    return page.w > 0 && page.h > 0 && page.x < plan.scrollX + plan.viewW && page.x + page.w > plan.scrollX &&
           page.y < plan.scrollY + plan.viewH && page.y + page.h > plan.scrollY;
}

bool isBigBake(const noting::render::FrameDesc& frame) {
    return frame.kind != 0 &&
           static_cast<std::int64_t>(frame.w) * static_cast<std::int64_t>(frame.h) >= 1024LL * 1024LL;
}

bool isPageHot(int page, const std::vector<int>& visible, int origin) {
    if (!visible.empty()) {
        return std::find(visible.begin(), visible.end(), page) != visible.end();
    }
    return page == origin;
}

void updateBakeMetricsLocked(BakeJob& job) {
    job.hot = isPageHot(job.page, gVisible, gOrigin);
    const noting::render::ViewPage* placed = nullptr;
    if (job.page >= 0 && job.page < static_cast<int>(gWindow.pages.size()) &&
        gWindow.pages[static_cast<std::size_t>(job.page)].index == job.page) {
        placed = &gWindow.pages[static_cast<std::size_t>(job.page)];
    } else {
        for (const noting::render::ViewPage& item : gWindow.pages) {
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
    auto open = [&](int page) { return page >= 0 && page < count && !gClaimed[static_cast<std::size_t>(page)]; };
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
    std::fprintf(stderr, "[noting] 줄 전진폭을 다 적었다\n");
}

void noteBaseLineDoneLocked(int page) {
    const auto found = gLineTally.find(page);
    if (found == gLineTally.end()) return;
    if (found->second.left > 0) --found->second.left;
    if (found->second.left == 0 && found->second.total > 0 && !found->second.told) {
        found->second.told = true;
        std::fprintf(stderr, "[noting] 스레드 %d 줄 %d · %d쪽 %d/%d\n", gCrewSize, found->second.total, page + 1,
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
    const noting::render::ViewPage* placed = nullptr;
    for (const noting::render::ViewPage& item : gWindow.pages) {
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
    if (page < 0 || page >= static_cast<int>(gPages.size())) return;
    ensurePageSpans(page);
    if (page >= static_cast<int>(gPages.size())) return;
    const noting::render::Node& scene = gPages[static_cast<std::size_t>(page)];
    const int pixelH = scene.height > 0 ? noting::render::pagePixelHeight(scene.height, 100) : 0;
    const bool hot = isPageHot(page, visible, origin);
    const int zoom = percent > 0 ? percent : 100;
    const bool lineTileHere = !baseAlreadyScanned || (hot && (scaleOnly || zoom == 100));
    const bool needBase = !baseAlreadyScanned || lineTileHere;
    std::vector<noting::render::FrameDesc> base;
    if (needBase && scene.height > 0 && pixelH >= 32) {
        base = gFrames.baseFrames(page, scene, &gDocument);
    }
    const noting::render::ViewPage* placed = nullptr;
    for (const noting::render::ViewPage& item : window.pages) {
        if (item.index == page) {
            placed = &item;
            break;
        }
    }
    std::vector<noting::render::FrameDesc> zoomFrames;
    if (hot && !scaleOnly && zoom != 100 && placed) {
        zoomFrames = noting::render::framesOnScreen(scene, *placed, zoom, window.scrollX, window.scrollY, window.viewW,
                                                    window.viewH, &gDocument);
        const int focusX = window.scrollX + window.viewW / 2 - placed->x;
        const int focusY = window.scrollY + window.viewH / 2 - placed->y;
        std::stable_sort(zoomFrames.begin(), zoomFrames.end(),
                         [&](const noting::render::FrameDesc& a, const noting::render::FrameDesc& b) {
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
    for (const noting::render::FrameDesc& frame : zoomFrames) {
        if (frame.kind == 0) zoomLines.insert(lineHoldKey(frame));
    }
    std::vector<LineJob> lines;
    int lineTotal = 0;
    for (const noting::render::FrameDesc& frame : base) {
        if (frame.kind != 0) continue;
        ++lineTotal;
        if (lineTileHere || zoomLines.count(lineHoldKey(frame)) || noting::render::linePlaceKept(frame)) continue;
        LineJob job;
        job.page = page;
        job.pixelH = pixelH;
        job.frame = frame;
        job.key = lineHoldKey(job.frame);
        lines.push_back(std::move(job));
    }
    std::vector<BakeJob> bakes;
    auto queueFrame = [&](noting::render::FrameDesc frame, int framePercent, int total, bool frameHot, bool frameLine) {
        frame.percent = framePercent;
        if (!noting::render::canBakeFrame(frame)) return;
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
    for (const noting::render::FrameDesc& frame : base) {
        if (frame.kind != 0) ++baseTotal;
    }
    int zoomTotal = 0;
    int zoomLineTotal = 0;
    for (noting::render::FrameDesc& frame : zoomFrames) {
        frame.percent = zoom;
        if (!noting::render::canBakeFrame(frame)) continue;
        if (frame.kind == 0) ++zoomLineTotal;
        else ++zoomTotal;
    }
    for (noting::render::FrameDesc& frame : zoomFrames) {
        frame.percent = zoom;
        if (!noting::render::canBakeFrame(frame)) continue;
        if (frame.kind == 0) queueFrame(std::move(frame), zoom, zoomLineTotal, true, true);
        else queueFrame(std::move(frame), zoom, zoomTotal, true, false);
    }
    for (const noting::render::FrameDesc& frame : base) {
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
        std::fprintf(stderr, "[noting] 스레드 %d 줄 %d · %d쪽 %d/%d\n", gCrewSize, tally.total, page + 1, tally.total,
                     tally.total);
    }
    maybeTellLinesLocked();
    gWorkCv.notify_all();
}

void crewMain() {
    int seen = 0;
    {
        std::lock_guard lock(gWorkMu);
        seen = gTick;
    }
    for (;;) {
        LineJob line;
        BakeJob bake;
        std::string pictureId;
        int kind = 0;
        {
            std::unique_lock lock(gWorkMu);
            for (;;) {
                if (gCrewStop) return;
                if (gCrewPaused) {
                    gWorkCv.wait(lock, [&] { return gCrewStop || !gCrewPaused; });
                    seen = gTick;
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now < gCrewEditHoldUntil) {
                    gWorkCv.wait_until(lock, gCrewEditHoldUntil, [&] { return gCrewStop || gCrewPaused; });
                    seen = gTick;
                    continue;
                }
                const bool userBusy = gUserInteracting.load(std::memory_order_relaxed);
                if (userBusy) {
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
                        noting::render::noteDocumentGlyphs(gDocument);
                    } catch (const std::exception& ex) {
                        noteFailure("글자 기록", &ex);
                    } catch (...) {
                        noteFailure("글자 기록");
                    }
                    lock.lock();
                    gGlyphsRunning = false;
                    if (gCrewStop) {
                        gWorkCv.notify_all();
                        return;
                    }
                    gGlyphsNoted = true;
                    ++gTick;
                    gWorkCv.notify_all();
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
                    continue;
                }
                maybeTellLinesLocked();
                gWorkCv.wait(lock, [&] { return gCrewStop || gCrewPaused || gTick != seen; });
                seen = gTick;
            }
        }
        if (kind == 1) {
            if (line.page >= 0 && line.page < static_cast<int>(gPages.size())) {
                try {
                    noting::render::writeLinePlace(gDocument, gPages[static_cast<std::size_t>(line.page)], line.frame,
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
                return;
            }
            finishLineLocked(line);
            if (gCrewPaused) gWorkCv.notify_all();
        } else if (kind == 2) {
            int targetMaskZoom = 100;
            {
                std::lock_guard lock(gWorkMu);
                targetMaskZoom = noting::render::restingMaskZoom(gPercent);
            }
            noting::render::MaskBake masks;
            try {
                masks = noting::render::bakeDocumentMasks(gDocument, 48, targetMaskZoom);
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
                return;
            }
            ++gTick;
            gWorkCv.notify_all();
        } else if (kind == 4) {
            if (!pictureId.empty()) {
                try {
                    noting::render::warmPicture(gDocument, pictureId);
                } catch (const std::exception& ex) {
                    std::fprintf(stderr, "[noting 오류] 그림 디코드 %s: %s\n", pictureId.c_str(), ex.what());
                } catch (...) {
                    std::fprintf(stderr, "[noting 오류] 그림 디코드 %s\n", pictureId.c_str());
                }
            }
            std::lock_guard lock(gWorkMu);
            if (gPictureInflight > 0) --gPictureInflight;
            if (gCrewStop) {
                gWorkCv.notify_all();
                return;
            }
            ++gTick;
            gWorkCv.notify_all();
        } else if (kind == 3) {
            const bool big = isBigBake(bake.frame);
            noting::render::PageBitmap bitmap;
            if (bake.page >= 0 && bake.page < static_cast<int>(gPages.size()) &&
                noting::render::canBakeFrame(bake.frame)) {
                try {
                    bitmap =
                        noting::render::renderFrame(gDocument, gPages[static_cast<std::size_t>(bake.page)], bake.frame);
                } catch (const std::exception& ex) {
                    std::fprintf(stderr, "[noting 오류] 칸 그리기 %d쪽 %dx%d: %s\n", bake.page + 1, bake.frame.w,
                                 bake.frame.h, ex.what());
                    bitmap = {};
                } catch (...) {
                    std::fprintf(stderr, "[noting 오류] 칸 그리기 %d쪽 %dx%d\n", bake.page + 1, bake.frame.w,
                                 bake.frame.h);
                    bitmap = {};
                }
            }
            std::lock_guard lock(gWorkMu);
            if (gBakeInflight > 0) --gBakeInflight;
            if (big && gBigBakeInflight > 0) --gBigBakeInflight;
            if (gCrewStop) {
                gWorkCv.notify_all();
                return;
            }
            const bool hot = isPageHot(bake.page, gVisible, gOrigin);
            const bool keep = bake.percent == 100 || (!gScaleOnly && bake.percent == gPercent && hot);
            if (!keep || !noting::render::canBakeFrame(bake.frame)) {
                gTaken.erase(bake.frame.id);
                if (bake.line && bake.percent == 100) noteBaseLineDoneLocked(bake.page);
                if (big || gCrewPaused) {
                    ++gTick;
                    gWorkCv.notify_all();
                }
                continue;
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
    }
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

void resumeCrewAfterEdit(const noting::render::EditResult& res) {
    if (!crewRunning()) return;
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
            gClaimed.assign(gPages.size(), 0);
            gBaseScanned.assign(gPages.size(), 0);
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
            if (gClaimed.size() < gPages.size()) gClaimed.resize(gPages.size(), 0);
            if (gBaseScanned.size() < gPages.size()) gBaseScanned.resize(gPages.size(), 0);
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
    gPercent = 100;
    gScaleOnly = false;
    ++gScanTick;
    ++gTick;
}

void drainDone() {
    std::vector<BakeDone> batch;
    {
        std::lock_guard lock(gWorkMu);
        if (gDone.empty()) return;
        batch.swap(gDone);
        if (gDoneBytes > 0) {
            gDoneBytes = 0;
            ++gTick;
            gWorkCv.notify_all();
        }
    }
    if (batch.empty()) return;
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
    std::string line = "[noting] 스레드 " + std::to_string(gCrewSize) + " 칸 " + std::to_string(cells);
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
    for (const noting::render::ViewPage& page : gLastPlan.pages) {
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
    const bool scale = pauseForPinch || percent < 100 || noting::render::overviewZoom(onScreenCount, percent);
    const int origin = idleOriginPage(gLastPlan);
    std::lock_guard lock(gWorkMu);
    if (gClaimed.size() != gPages.size()) gClaimed.assign(gPages.size(), 0);
    if (gBaseScanned.size() != gPages.size()) gBaseScanned.assign(gPages.size(), 0);
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

bool stepSingleThreadForeground() {
    if (crewRunning() || gPages.empty()) return false;
    if (gLastPlan.job.id == 0 || gLastPlan.job.background) return false;
    const int jobPage = gLastPlan.job.page;
    if (jobPage < 0 || jobPage >= static_cast<int>(gPages.size())) return false;
    const int jobPercent =
        gLastPlan.job.percent > 0 ? gLastPlan.job.percent : (gLastPlan.percent > 0 ? gLastPlan.percent : 100);
    if (gUseLineCache) ensureLineCachePage(jobPage);
    const noting::render::FramePrepare made =
        gFrames.prepare(gDocument, gPages, jobPage, jobPercent, gLastPlan.job.y);
    return made.made > 0;
}

bool stepSingleThreadIdle() {
    if (crewRunning() || gPages.empty()) return false;
    if (stepSingleThreadForeground()) return true;
    noting::render::noteDocumentGlyphs(gDocument);
    if (!(gLastPlan.job.background && !jobOnScreen(gLastPlan))) {
        const noting::render::IdleStep step = gFrames.prepareIdle(gDocument, gPages, gLastPlan);
        if (step.more || step.screen || step.made > 0) return step.more || step.made > 0;
    }
    const noting::render::LineNote lines =
        noting::render::cacheIdleLines(gDocument, gPages, idleOriginPage(gLastPlan), 16);
    if (lines.more) return true;
    const int targetMaskZoom = noting::render::restingMaskZoom(gLastPlan.percent);
    const noting::render::MaskBake masks = noting::render::bakeDocumentMasks(gDocument, 16, targetMaskZoom);
    if (masks.more) return true;
    const noting::render::IdleStep step = gFrames.prepareIdle(gDocument, gPages, gLastPlan);
    return step.more || step.made > 0;
}

void beginGrind() {
    if (gPages.empty()) return;
#if !defined(__EMSCRIPTEN_PTHREADS__)
    (void)crewMain;
    (void)grindThreads;
    std::fprintf(stderr, "[noting] 단일 스레드 폴백 · 화면 스레드 1\n");
    return;
#else
    {
        std::lock_guard lock(gWorkMu);
        if (!gCrew.empty()) {
            ++gTick;
            gWorkCv.notify_all();
            return;
        }
        gCrewStop = false;
        gCrewLive.store(true);
        if (gClaimed.size() != gPages.size()) gClaimed.assign(gPages.size(), 0);
        if (gBaseScanned.size() != gPages.size()) gBaseScanned.assign(gPages.size(), 0);
        gPictureIds.clear();
        gPictureCursor = 0;
        gPictureInflight = 0;
        gPictureIds.reserve(gDocument.header.binItems.size());
        for (const auto& item : gDocument.header.binItems) {
            if (!item.id.empty()) gPictureIds.push_back(item.id);
        }
        gCrewSize = grindThreads();
    }
    std::fprintf(stderr, "[noting] 그리는 스레드 %d · 화면 스레드 1\n", gCrewSize);
    gCrew.reserve(static_cast<std::size_t>(gCrewSize));
    for (int i = 0; i < gCrewSize; ++i) gCrew.emplace_back(crewMain);
#endif
}

}  // namespace

emscripten::val idle(const emscripten::val& msg) {
    emscripten::val out = emscripten::val::object();
    if (crewRunning() && gScreenOn) {
        out.set("view", emscripten::val::null());
        out.set("more", false);
        out.set("screen", false);
        out.set("made", 0);
        return out;
    }
    out.set("view", viewApply(msg));
    if (crewRunning()) {
        out.set("more", false);
        out.set("screen", false);
        out.set("made", 0);
        return out;
    }
    if (gScreenOn && gLastPlan.job.id != 0 && !gLastPlan.job.background) {
        const bool made = stepSingleThreadForeground();
        if (made) {
            noting::render::ViewPlan updated = gCamera.plan(false);
            useFramePlan(updated);
            gLastPlan = std::move(updated);
            out.set("more", true);
            out.set("screen", false);
            out.set("made", 1);
            return out;
        }
    }
    noting::render::noteDocumentGlyphs(gDocument);
    // 화면에 들어온 줄 칸을 먼저 배치한다. 그다음 나머지 줄의 전진폭을 적고, 마스크를 찍는다.
    if (!(gLastPlan.job.background && !jobOnScreen(gLastPlan))) {
        const noting::render::IdleStep step = gFrames.prepareIdle(gDocument, gPages, gLastPlan);
        out.set("more", step.more);
        out.set("screen", step.screen);
        out.set("made", step.made);
        if (step.more || step.screen || step.made > 0) return out;
    }
    const noting::render::LineNote lines = noting::render::cacheIdleLines(gDocument, gPages, idleOriginPage(gLastPlan), 16);
    if (lines.more) {
        out.set("more", true);
        out.set("screen", false);
        out.set("made", lines.made);
        return out;
    }
    const int targetMaskZoom = noting::render::restingMaskZoom(gLastPlan.percent);
    const noting::render::MaskBake masks = noting::render::bakeDocumentMasks(gDocument, 16, targetMaskZoom);
    if (masks.more) {
        out.set("more", true);
        out.set("screen", false);
        out.set("made", masks.made);
        return out;
    }
    const noting::render::IdleStep step = gFrames.prepareIdle(gDocument, gPages, gLastPlan);
    out.set("more", step.more);
    out.set("screen", step.screen);
    out.set("made", step.made);
    return out;
}

emscripten::val renderFrame(int id) {
    emscripten::val result = emscripten::val::object();
    auto fail = [&] {
        result.set("width", 0);
        result.set("height", 0);
        result.set("pixels", emscripten::val(emscripten::typed_memory_view(0, static_cast<const std::uint8_t*>(nullptr))));
        return result;
    };
    const noting::render::FrameDesc* found = gFrames.desc(id);
    if (!found) return fail();
    const noting::render::FrameDesc& desc = *found;
    if (gUseLineCache) ensureLineCachePage(desc.page);
    if (desc.page < 0 || desc.page >= static_cast<int>(gPages.size())) return fail();
    gViewFrame = noting::render::renderFrame(gDocument, gPages[static_cast<std::size_t>(desc.page)], desc);
    if (gViewFrame.rgba.empty() || gViewFrame.width <= 0 || gViewFrame.height <= 0) {
        gViewFrame.width = 1;
        gViewFrame.height = 1;
        gViewFrame.rgba.assign(4, 0);
    }
    result.set("width", gViewFrame.width);
    result.set("height", gViewFrame.height);
    result.set("pixels", emscripten::val(emscripten::typed_memory_view(gViewFrame.rgba.size(), gViewFrame.rgba.data())));
    return result;
}

int inputInt(const emscripten::val& msg, const char* key, int fallback) {
    const emscripten::val value = msg[key];
    if (!defined(value)) return fallback;
    const double d = value.as<double>();
    if (!std::isfinite(d)) return fallback;
    return static_cast<int>(std::lround(d));
}

double inputNumber(const emscripten::val& msg, const char* key, double fallback) {
    const emscripten::val value = msg[key];
    if (!defined(value)) return fallback;
    const double d = value.as<double>();
    return std::isfinite(d) ? d : fallback;
}

bool inputFlag(const emscripten::val& msg, const char* key) {
    const emscripten::val value = msg[key];
    return defined(value) && value.as<bool>();
}

emscripten::val packInput(const noting::input::Camera& camera) {
    noting::render::Scrollbar horizontal;
    noting::render::Scrollbar vertical;
    gCamera.scrollbars(horizontal, vertical);
    auto packBar = [](const noting::render::Scrollbar& bar) {
        emscripten::val entry = emscripten::val::object();
        entry.set("x", bar.x);
        entry.set("y", bar.y);
        entry.set("w", bar.w);
        entry.set("h", bar.h);
        entry.set("thumbX", bar.thumbX);
        entry.set("thumbY", bar.thumbY);
        entry.set("thumbW", bar.thumbW);
        entry.set("thumbH", bar.thumbH);
        entry.set("visible", bar.visible);
        return entry;
    };
    emscripten::val out = emscripten::val::object();
    out.set("scrollX", camera.scrollX);
    out.set("scrollY", camera.scrollY);
    out.set("maxScrollX", camera.maxScrollX);
    out.set("maxScrollY", camera.maxScrollY);
    out.set("percent", camera.percent);
    out.set("fitPage", camera.fitPage);
    out.set("fitWidth", camera.fitWidth);
    out.set("zoomMode", camera.fitPage ? std::string("fitPage")
                                       : (camera.fitWidth ? std::string("fitWidth") : std::string("percent")));
    out.set("zoomLabel",
            camera.fitPage ? std::string("쪽 맞춤")
                           : (camera.fitWidth ? std::string("폭 맞춤") : (std::to_string(camera.percent) + "%")));
    out.set("gridMode", std::string(gridModeName(camera.gridMode)));
    out.set("gridCols", camera.gridCols);
    out.set("gridRows", camera.gridRows);
    out.set("effectiveCols", camera.effectiveCols);
    out.set("rowStepY", camera.rowStepY);
    out.set("pageStepY", camera.pageStepY);
    out.set("page", currentCameraPage(camera.scrollX, camera.scrollY, camera.percent));
    out.set("pageCount", static_cast<int>(gPages.size()));
    out.set("scrollChanged", camera.scrollChanged);
    out.set("zoomChanged", camera.zoomChanged);
    out.set("coasting", camera.coasting);
    out.set("handled", camera.handled);
    out.set("pinching", camera.pinching);
    out.set("draggingBar", camera.draggingBar);
    out.set("wheeling", camera.wheeling);
    out.set("panning", camera.panning);
    out.set("rotation", camera.rotation);
    out.set("barX", packBar(horizontal));
    out.set("barY", packBar(vertical));
    return out;
}

bool paperPoint(double vx, double vy, bool clampToNearest, int& page, int& x, int& y) {
    const noting::render::ViewPlan& plan = gLastPlan;
    const noting::render::ViewportState cam = gCamera.state();
    const int scrollX = cam.percent == plan.percent ? cam.scrollX : plan.scrollX;
    const int scrollY = cam.percent == plan.percent ? cam.scrollY : plan.scrollY;
    auto toHu = [&](int index, double left, double top, int w, int h) {
        if (w <= 0 || h <= 0 || index < 0 || index >= static_cast<int>(gPages.size())) return false;
        const noting::render::Node& node = gPages[static_cast<std::size_t>(index)];
        if (node.width <= 0 || node.height <= 0) return false;
        page = index;
        x = static_cast<int>(std::llround((vx - left) / static_cast<double>(w) * node.width));
        y = static_cast<int>(std::llround((vy - top) / static_cast<double>(h) * node.height));
        return true;
    };
    auto hit = [&](int index, double left, double top, int w, int h) {
        if (vx < left || vy < top || vx > left + w || vy > top + h) return false;
        return toHu(index, left, top, w, h);
    };
    for (const noting::render::ViewPage& sheet : plan.pages) {
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
    const noting::render::ViewPage* nearest = nullptr;
    double bestDist = 0;
    for (const noting::render::ViewPage& sheet : plan.pages) {
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

void attachSelection(emscripten::val& out, const noting::input::PointerOutcome& outcome) {
    if (!outcome.select) return;
    noting::render::PageFillGuard guard;
    int kind = 1;
    if (outcome.tap) kind = 1;
    else if (outcome.phase == noting::input::Phase::Move) kind = 2;
    else if (outcome.phase == noting::input::Phase::Up) kind = 3;
    else if (outcome.phase == noting::input::Phase::Cancel) kind = 4;
    int page = -1;
    int x = 0;
    int y = 0;
    if (kind == 1 || kind == 2 || kind == 3) paperPoint(outcome.x, outcome.y, kind == 2, page, x, y);
    if ((kind == 1 || kind == 2) && page >= 0) {
        if (gUseLineCache) ensureLineCachePage(page);
        else ensurePageSpans(page);
    }
    noting::render::PointerSample sample;
    sample.id = outcome.id;
    sample.device = outcome.device;
    sample.time = outcome.time;
    sample.dpr = gLastPlan.dpr > 0 ? gLastPlan.dpr : 1;
    sample.viewX = outcome.x;
    sample.viewY = outcome.y;
    if (page >= 0 && page < static_cast<int>(gPages.size())) {
        for (const noting::render::ViewPage& sheet : gLastPlan.pages) {
            if (sheet.index != page || sheet.w <= 0) continue;
            const noting::render::Node& node = gPages[static_cast<std::size_t>(page)];
            if (node.width > 0) sample.huPerLogical = (static_cast<double>(node.width) / sheet.w) * sample.dpr;
            break;
        }
    }
    noting::render::PointerView view =
        noting::render::applyPointer(gDocument, gPages, kind, page, x, y, gCamera.state().percent, sample);
    const std::string event = noting::render::pointerEventText();
    if (kind == 2 && event.empty()) {
        return;
    }
    if (outcome.tap) {
        view = noting::render::applyPointer(gDocument, gPages, 3, page, x, y, gCamera.state().percent, sample);
    }
    if ((kind == 3 || outcome.tap) && noting::render::commitObjectResize(gDocument)) {
        const bool wasCrew = crewRunning();
        if (wasCrew) pauseCrewForEdit();
        gUseLineCache = false;
        const noting::render::ResizeCommitInfo resized = noting::render::resizeCommitInfo();
        if (!noting::render::reflowResizedPages(gDocument, gPages, resized.page, gStoredLineHeight, gStoredLineHeight,
                                               resized.bin, static_cast<int>(resized.width),
                                               static_cast<int>(resized.height))) {
            gPages = noting::render::layoutPages(gDocument, -1, gStoredLineHeight, gStoredLineHeight);
        }
        noting::render::restoreResizedObject(gDocument, gPages, resized.page, resized.bin,
                                             static_cast<int>(resized.width), static_cast<int>(resized.height));
        syncFullRelayout(true);
        if (wasCrew) {
            noting::render::EditResult resumed;
            resumed.documentChanged = true;
            resumed.fullInvalidation = true;
            resumeCrewAfterEdit(resumed);
        }
    }
    if (gScreenOn && noting::render::screenReady()) {
        if (!gScreenLoop) {
            screenFrame(frameNow(), nullptr);
        }
    }
    out.set("select", true);
    out.set("selectPhase", outcome.tap ? 1 : static_cast<int>(outcome.phase));
    if (kind == 3 && noting::render::takePinTap()) out.set("pinTap", true);
    out.set("active", view.active);
    out.set("caret", view.caret);
    if (kind != 2) {
        out.set("cursorStyle", cursorStyleNow());
        out.set("text", noting::render::selectionText(gDocument, gPages));
    }
    if (!event.empty()) out.set("event", event);
    gFallbackImages.clear();
    if (!gScreenOn) gFallbackImages = noting::render::pointerImages(gDocument, gPages);
    out.set("images", packImages(gFallbackImages));
}

// 뷰포트 장치 픽셀 한 칸이 그 쪽에서 몇 HWPUNIT 인지. 논리 픽셀 기준이라 dpr 을 곱한다.
double huPerLogicalAt(int page) {
    if (page < 0 || page >= static_cast<int>(gPages.size())) return 0;
    const double dpr = gLastPlan.dpr > 0 ? gLastPlan.dpr : 1;
    for (const noting::render::ViewPage& sheet : gLastPlan.pages) {
        if (sheet.index != page || sheet.w <= 0) continue;
        const noting::render::Node& node = gPages[static_cast<std::size_t>(page)];
        return node.width > 0 ? (static_cast<double>(node.width) / sheet.w) * dpr : 0;
    }
    return 0;
}

// 터치가 블록 마커 위에서 시작하면 스크롤하지 않고 마커를 끈다.
bool touchOnPin(double vx, double vy) {
    int page = -1;
    int x = 0;
    int y = 0;
    if (!paperPoint(vx, vy, false, page, x, y) || page < 0) return false;
    return noting::render::pinUnder(page, x, y, 22.0 * huPerLogicalAt(page));
}

// 마우스가 올라간 자리의 CSS 커서. 눈금자·스크롤 막대·종이 밖은 화살표다.
std::string hoverCursor(double vx, double vy) {
    if (gCamera.inRuler(vx, vy)) return "default";
    noting::render::Scrollbar horizontal;
    noting::render::Scrollbar vertical;
    gCamera.scrollbars(horizontal, vertical);
    auto onBar = [&](const noting::render::Scrollbar& bar) {
        return bar.visible && vx >= bar.x && vy >= bar.y && vx < bar.x + bar.w && vy < bar.y + bar.h;
    };
    if (onBar(horizontal) || onBar(vertical)) return "default";
    int page = -1;
    int x = 0;
    int y = 0;
    if (!paperPoint(vx, vy, false, page, x, y) || page < 0) return "default";
    if (gUseLineCache) ensureLineCachePage(page);
    else ensurePageSpans(page);
    return noting::render::hoverCursor(gDocument, gPages, page, x, y, 10.0 * huPerLogicalAt(page));
}

// type: pointer, wheel, gesture, tick, scroll, zoom.
// 포인터는 id 와 마우스·터치·펜을 그대로 넘긴다. 좌표는 뷰포트 장치 픽셀이다.
emscripten::val inputEvent(const emscripten::val& msg) {
    const emscripten::val typeValue = msg["type"];
    const std::string type = defined(typeValue) ? typeValue.as<std::string>() : std::string();
    noting::input::Camera camera;
    noting::input::PointerOutcome outcome;
    if (type == "pointer") {
        noting::input::PointerEvent event;
        event.device = static_cast<noting::input::Device>(inputInt(msg, "device", 0));
        event.phase = static_cast<noting::input::Phase>(inputInt(msg, "phase", 1));
        event.id = inputInt(msg, "id", 0);
        event.x = inputNumber(msg, "x", 0);
        event.y = inputNumber(msg, "y", 0);
        event.time = inputNumber(msg, "t", 0);
        event.button = inputInt(msg, "button", 0);
        outcome = gInput.pointer(gCamera, event);
        outcome.id = event.id;
        outcome.device = static_cast<int>(event.device);
        outcome.time = event.time;
        if (event.device != noting::input::Device::Mouse && event.phase == noting::input::Phase::Down &&
            gInput.touchCount() == 1 && !gInput.pinchActive() &&
            (touchOnPin(event.x, event.y) ||
             noting::render::repeatedTouchDown(event.x, event.y, event.time, gLastPlan.dpr > 0 ? gLastPlan.dpr : 1,
                                               static_cast<int>(event.device), event.id))) {
            gInput.holdSelection(event.id);
            outcome.select = true;
            outcome.x = event.x;
            outcome.y = event.y;
        }
        camera = outcome.camera;
    } else if (type == "pointers") {
        const emscripten::val items = msg["items"];
        const int len = defined(items) ? items["length"].as<int>() : 0;
        std::vector<noting::input::PointerEvent> events;
        if (len > 0) {
            events.reserve(static_cast<std::size_t>(len));
            for (int i = 0; i < len; ++i) {
                const emscripten::val item = items[i];
                noting::input::PointerEvent event;
                event.device = static_cast<noting::input::Device>(inputInt(item, "device", 0));
                event.phase = static_cast<noting::input::Phase>(inputInt(item, "phase", 2));
                event.id = inputInt(item, "id", 0);
                event.x = inputNumber(item, "x", 0);
                event.y = inputNumber(item, "y", 0);
                event.time = inputNumber(item, "t", 0);
                event.button = inputInt(item, "button", 0);
                events.push_back(event);
            }
        }
        outcome = gInput.pointers(gCamera, events.data(), events.size());
        camera = outcome.camera;
    } else if (type == "wheel") {
        noting::input::WheelEvent event;
        event.phase = static_cast<noting::input::WheelPhase>(inputInt(msg, "phase", 0));
        event.dx = inputNumber(msg, "dx", 0);
        event.dy = inputNumber(msg, "dy", 0);
        event.x = inputNumber(msg, "x", 0);
        event.y = inputNumber(msg, "y", 0);
        event.vx = inputNumber(msg, "vx", 0);
        event.vy = inputNumber(msg, "vy", 0);
        event.zoom = inputFlag(msg, "zoom");
        event.discrete = inputFlag(msg, "discrete");
        camera = gInput.wheel(gCamera, event);
    } else if (type == "gesture") {
        noting::input::GestureEvent event;
        event.kind = static_cast<noting::input::GestureKind>(inputInt(msg, "kind", 0));
        event.phase = static_cast<noting::input::Phase>(inputInt(msg, "phase", 1));
        event.x = inputNumber(msg, "x", 0);
        event.y = inputNumber(msg, "y", 0);
        event.dx = inputNumber(msg, "dx", 0);
        event.dy = inputNumber(msg, "dy", 0);
        event.scale = inputNumber(msg, "scale", 1);
        event.rotation = inputNumber(msg, "rotation", 0);
        camera = gInput.gesture(gCamera, event);
    } else if (type == "tick") {
        camera = gInput.tick(gCamera, inputNumber(msg, "dt", 0));
    } else if (type == "scroll") {
        camera = gInput.setScroll(gCamera, inputInt(msg, "x", 0), inputInt(msg, "y", 0));
    } else if (type == "viewport") {
        const noting::render::ViewportState before = gCamera.state();
        gCamera.setViewport(inputInt(msg, "cssW", 1), inputInt(msg, "cssH", 1), inputNumber(msg, "dpr", 1));
        const noting::render::ViewPlan probe = gCamera.plan(false);
        const noting::render::ViewportState after = gCamera.state();
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
        const int page = inputInt(msg, "page", 0);
        const noting::render::ViewPlan layout = gCamera.plan(false);
        bool found = false;
        for (const noting::render::ViewPage& item : layout.pages) {
            if (item.index != page) continue;
            camera = gInput.setScroll(gCamera, item.outerX() - layout.ruler, item.outerY() - layout.ruler);
            found = true;
            break;
        }
        if (!found) camera = gInput.tick(gCamera, 0);
    } else if (type == "zoom") {
        camera = gInput.glideZoom(gCamera, inputInt(msg, "percent", gCamera.state().percent));
    } else if (type == "pageStep" || type == "pageUp" || type == "pageDown") {
        const int fallbackDir = (type == "pageUp") ? -1 : 1;
        camera = gInput.stepPage(gCamera, inputInt(msg, "dir", fallbackDir));
    } else if (type == "fit") {
        const emscripten::val modeVal = msg["mode"];
        const std::string mode = defined(modeVal) ? modeVal.as<std::string>() : std::string("page");
        camera = (mode == "width") ? gInput.fitWidth(gCamera) : gInput.fitPage(gCamera);
    } else if (type == "grid") {
        const emscripten::val modeVal = msg["mode"];
        const std::string modeStr = defined(modeVal) ? modeVal.as<std::string>() : std::string("auto");
        const int cols = inputInt(msg, "cols", 1);
        const int rows = inputInt(msg, "rows", 1);
        const bool fit = defined(msg["fit"]) ? msg["fit"].as<bool>() : (modeStr == "grid");
        camera = gInput.setGridMode(gCamera, parseGridMode(modeStr), cols, rows, fit);
    } else if (type == "pins") {
        noting::render::setSelectionPins(inputFlag(msg, "on"));
        if (gScreenOn && noting::render::screenReady()) screenFrame(0, nullptr);
        camera = gInput.tick(gCamera, 0);
    } else if (type == "readOnly") {
        noting::render::setReadOnly(inputFlag(msg, "on"));
        camera = gInput.tick(gCamera, 0);
    } else if (type == "rulers") {
        const noting::render::ViewportState before = gCamera.state();
        gCamera.setRulers(inputFlag(msg, "on"));
        const noting::render::ViewportState after = gCamera.state();
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
        if (gScreenOn && noting::render::screenReady() && !gScreenLoop) {
            screenFrame(nowMs, nullptr);
        }
    }
    if ((camera.scrollChanged || camera.zoomChanged) || gInput.selecting() || noting::render::pointerTracking()) {
        setCrewInteracting(true);
    }
    emscripten::val out = packInput(camera);
    if (type == "pointer" || type == "pointers") attachSelection(out, outcome);
    return out;
}

noting::model::SourceFormat parseFormatString(const std::string& formatStr) {
    std::string lower = formatStr;
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lower == "hwp") return noting::model::SourceFormat::Hwp;
    if (lower == "hwpx") return noting::model::SourceFormat::Hwpx;
    return noting::model::SourceFormat::Unknown;
}

emscripten::val toUint8ArrayVal(const noting::Bytes& bytes) {
    emscripten::val Uint8Array = emscripten::val::global("Uint8Array");
    emscripten::val out = Uint8Array.new_(bytes.size());
    if (!bytes.empty()) {
        emscripten::val view(emscripten::typed_memory_view(bytes.size(), bytes.data()));
        out.call<void>("set", view);
    }
    return out;
}

// 현재 열려 있는 gDocument 를 "hwpx" 또는 "hwp" 바이트(Uint8Array)로 내보낸다.
emscripten::val exportDocument(const std::string& formatStr) {
    noting::ExportOptions opts;
    opts.format = parseFormatString(formatStr);
    opts.sourceData = gFile;
    const noting::ExportResult res = noting::writeDocument(gDocument, opts);
    if (!res.ok) return emscripten::val::null();
    return toUint8ArrayVal(res.data);
}

// 입력 파일 바이트(Uint8Array)를 파싱해 지정한 형식("hwpx" 또는 "hwp")의 Uint8Array 로 변환한다.
emscripten::val convertDocument(const emscripten::val& input, const std::string& formatStr) {
    const noting::Bytes srcBytes = toBytes(input);
    noting::ParseOptions parseOpts;
    parseOpts.keepLineSegs = true;
    const noting::ParseResult parsed = noting::parseDocument(srcBytes, parseOpts);
    if (!parsed.ok) return emscripten::val::null();

    noting::ExportOptions exportOpts;
    exportOpts.format = parseFormatString(formatStr);
    exportOpts.sourceData = srcBytes;
    const noting::ExportResult exported = noting::writeDocument(parsed.document, exportOpts);
    if (!exported.ok) return emscripten::val::null();
    return toUint8ArrayVal(exported.data);
}

EMSCRIPTEN_BINDINGS(noting) {
    emscripten::function("detectFormat", &detectFormat);
    emscripten::function("parse", &parse);
    emscripten::function("exportDocument", &exportDocument);
    emscripten::function("convertDocument", &convertDocument);
    emscripten::function("extractText", &extractText);
    emscripten::function("extractPages", &extractPages);
    emscripten::function("extractAnchorPages", &extractAnchorPages);
    emscripten::function("extractInnerLines", &extractInnerLines);
    emscripten::function("extractLayout", &extractLayout);
    emscripten::function("layoutScene", &layoutScene);
    emscripten::function("useLineCache", &useLineCache);
    emscripten::function("lineCacheScene", &lineCacheScene);
    emscripten::function("loadFont", &loadFont);
    emscripten::function("decodeFont", &decodeFont);
    emscripten::function("fontDecodesPending", &fontDecodesPending);
    emscripten::function("trimCaches", &trimCaches);
    emscripten::function("setCacheBudget", &setCacheBudget);
    emscripten::function("cacheUsage", &cacheUsage);
    emscripten::function("replaceFontFace", &replaceFontFace);
    emscripten::function("openDocument", &openDocument);
    emscripten::function("newDocument", &newDocument);
    emscripten::function("setFontCatalog", &setFontCatalog);
    emscripten::function("setFontLicenseSwaps", &setFontLicenseSwaps);
    emscripten::function("pickableFonts", &pickableFonts);
    emscripten::function("licenseFontSwaps", &licenseFontSwaps);
    emscripten::function("editSwapFonts", &editSwapFonts);
    emscripten::function("planDocument", &planDocument);
    emscripten::function("setMeasuredLineHeight", &noting::render::setMeasuredLineHeight);
    emscripten::function("warmFonts", &warmFonts);
    emscripten::function("ensurePageSpans", &ensurePageSpans);
    emscripten::function("pageFaces", &pageFaces);
    emscripten::function("documentFaces", &documentFaces);
    emscripten::function("warmDocument", &warmDocument);
    emscripten::function("warmGlyphs", &warmGlyphs);
    emscripten::function("preparePage", &preparePage);
    emscripten::function("idle", &idle);
    emscripten::function("beginGrind", &beginGrind);
    emscripten::function("setReadyPages", &setReadyPages);
    emscripten::function("renderPage", &renderPage);
    emscripten::function("renderPagePixels", &renderPagePixels);
    emscripten::function("renderPageView", &renderPageView);
    emscripten::function("renderFrame", &renderFrame);
    emscripten::function("viewApply", &viewApply);
    emscripten::function("caretAt", &caretAt);
    emscripten::function("caretInStory", &caretInStory);
    emscripten::function("blockMarks", &blockMarks);
    emscripten::function("pointerEvent", &pointerEvent);
    emscripten::function("cursorState", &cursorState);
    emscripten::function("cursorStyle", &cursorStyleNow);
    emscripten::function("hoverCursor", &hoverCursor);
    emscripten::function("editText", &editText);
    emscripten::function("editCommit", &editCommit);
    emscripten::function("editKey", &editKey);
    emscripten::function("editStyle", &editStyle);
    emscripten::function("formatFields", &formatFieldsNow);
    emscripten::function("inputEvent", &inputEvent);
    emscripten::function("retainViewTile", &retainViewTile);
    emscripten::function("attachScreen", &attachScreen);
    emscripten::function("screenReady", &screenReady);
    emscripten::function("presentView", &presentView);
    emscripten::function("glyphMasksPending", &glyphMasksPending);
    emscripten::function("mmToHu", &noting::mmToHu);
    emscripten::function("ptToHu", &noting::ptToHu);
    emscripten::function("huToMm", &noting::huToMm);
    emscripten::function("huToPt", &noting::huToPt);
}
