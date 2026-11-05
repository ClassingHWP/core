// JavaScript 바인딩. JS 값을 코어 값으로 바꿔 넘기고, 코어가 돌려준 값을 JS 객체로 포장한다.
// 판단은 모두 코어(chwp/app/session.hpp)에 있다. 여기 남는 것은 플랫폼 일(시계, 프레임 루프, 그림판 크기,
// 카메라·글꼴 알림, 코어 수)과 값 변환뿐이다.
//
//   import createChwpModule from "./chwp.mjs";
//   const chwp = await createChwpModule();
//   const result = JSON.parse(chwp.parse(new Uint8Array(buffer), false, false));

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <emscripten.h>
#include <emscripten/bind.h>
#include <emscripten/heap.h>
#include <malloc.h>
#include <emscripten/html5.h>
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#endif
#include <emscripten/val.h>

#include "export/font_metrics.hpp"
#include "chwp/app/session.hpp"
#include "chwp/export/font.hpp"

namespace {

namespace app = chwp::app;
using emscripten::val;

// ---- 값 변환

chwp::Bytes toBytes(const val& input) {
    const auto length = input["length"].as<std::size_t>();
    chwp::Bytes bytes(length);
    val view(emscripten::typed_memory_view(length, bytes.data()));
    view.call<void>("set", input);
    return bytes;
}

val toUint8ArrayVal(const chwp::Bytes& bytes) {
    val Uint8Array = val::global("Uint8Array");
    val out = Uint8Array.new_(bytes.size());
    if (!bytes.empty()) {
        val view(emscripten::typed_memory_view(bytes.size(), bytes.data()));
        out.call<void>("set", view);
    }
    return out;
}

bool defined(const val& value) { return !value.isUndefined() && !value.isNull(); }

int inputInt(const val& msg, const char* key, int fallback) {
    const val value = msg[key];
    if (!defined(value)) return fallback;
    const double d = value.as<double>();
    if (!std::isfinite(d)) return fallback;
    return static_cast<int>(std::lround(d));
}

std::optional<int> inputOptInt(const val& msg, const char* key) {
    const val value = msg[key];
    if (!defined(value)) return std::nullopt;
    const double d = value.as<double>();
    if (!std::isfinite(d)) return std::nullopt;
    return static_cast<int>(std::lround(d));
}

double inputNumber(const val& msg, const char* key, double fallback) {
    const val value = msg[key];
    if (!defined(value)) return fallback;
    const double d = value.as<double>();
    return std::isfinite(d) ? d : fallback;
}

bool inputFlag(const val& msg, const char* key) {
    const val value = msg[key];
    return defined(value) && value.as<bool>();
}

val pixelsVal(const chwp::render::PageBitmap* bitmap) {
    val result = val::object();
    if (!bitmap) {
        result.set("width", 0);
        result.set("height", 0);
        result.set("pixels", val(emscripten::typed_memory_view(0, static_cast<const std::uint8_t*>(nullptr))));
        return result;
    }
    result.set("width", bitmap->width);
    result.set("height", bitmap->height);
    result.set("pixels", val(emscripten::typed_memory_view(bitmap->rgba.size(), bitmap->rgba.data())));
    return result;
}

val packCaretStatus(const chwp::render::CaretStatus& st) {
    val out = val::object();
    out.set("active", st.active);
    out.set("section", st.section);
    out.set("sectionCount", st.sectionCount);
    out.set("column", st.column);
    out.set("line", st.line);
    out.set("char", st.character);
    out.set("band", st.band);
    out.set("overwrite", st.overwrite);
    return out;
}

val packCursorStyle(const app::StyleReport& report) {
    const chwp::render::CursorStyle& st = report.style;
    val out = val::object();
    out.set("active", st.active);
    out.set("styleId", st.styleId);
    out.set("styleName", st.styleName);
    out.set("styleEngName", st.styleEngName);
    out.set("styleType", st.styleType);
    out.set("paraShapeId", st.paraShapeId);
    out.set("align", st.align);
    out.set("lineSpacingType", st.lineSpacingType);
    out.set("lineSpacing", st.lineSpacing);
    out.set("lineSpacingValue", st.lineSpacingValue());
    out.set("lineSpacingUnit", std::string(st.lineSpacingUnit()));
    out.set("charShapeId", st.charShapeId);
    out.set("font", st.font);
    out.set("fontSwapTo", st.fontSwapTo);
    out.set("fontSize", st.fontSize);
    out.set("fontSizePt", st.fontSizePt());
    val sizes = val::object();
    for (const char* unit : {"pt", "mm", "cm", "inch", "px"}) sizes.set(unit, st.fontSizeIn(unit));
    out.set("fontSizeIn", sizes);
    out.set("bold", st.bold);
    out.set("italic", st.italic);
    out.set("underline", st.underline);
    out.set("underlineType", st.underlineType);
    out.set("strikeout", st.strikeout);
    out.set("color", st.color);
    val mixed = val::object();
    mixed.set("style", st.mixed.style);
    mixed.set("font", st.mixed.font);
    mixed.set("fontSize", st.mixed.fontSize);
    mixed.set("bold", st.mixed.bold);
    mixed.set("italic", st.mixed.italic);
    mixed.set("underline", st.mixed.underline);
    mixed.set("strikeout", st.mixed.strikeout);
    mixed.set("color", st.mixed.color);
    mixed.set("align", st.mixed.align);
    mixed.set("lineSpacing", st.mixed.lineSpacing);
    out.set("mixed", mixed);
    out.set("caret", packCaretStatus(report.status));
    return out;
}

val packFields(const chwp::render::FormatFields& fields) {
    val out = val::object();
    for (const auto& [key, value] : fields) {
        std::visit([&](const auto& v) { out.set(key, v); }, value);
    }
    return out;
}

val packImages(const std::vector<chwp::render::SelectionImage>& images) {
    val out = val::array();
    for (const chwp::render::SelectionImage& image : images) {
        val item = val::object();
        item.set("page", image.page);
        item.set("x", image.x);
        item.set("y", image.y);
        item.set("w", image.width);
        item.set("h", image.height);
        item.set("pageW", image.pageW);
        item.set("pageH", image.pageH);
        item.set("caret", image.caret);
        item.set("pixels", val(emscripten::typed_memory_view(image.rgba.size(), image.rgba.data())));
        out.call<void>("push", item);
    }
    return out;
}

val packBar(const chwp::render::Scrollbar& bar) {
    val entry = val::object();
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
}

val packEditReport(const app::EditReport& report) {
    const chwp::render::EditResult& res = report.result;
    val out = val::object();
    out.set("handled", res.handled);
    out.set("documentChanged", res.documentChanged);
    out.set("fullInvalidation", res.fullInvalidation);
    out.set("composing", res.composing);
    out.set("canUndo", report.canUndo);
    out.set("canRedo", report.canRedo);
    out.set("docState", report.docState);
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
    out.set("caretTurned", res.caret.turned);
    out.set("scrollX", report.scrollX);
    out.set("scrollY", report.scrollY);
    out.set("cursorStyle", packCursorStyle(report.style));
    out.set("text", report.text);
    val underlines = val::array();
    for (const chwp::render::SelectionMark& mark : res.compositionUnderlines) {
        val u = val::object();
        u.set("page", mark.page);
        u.set("x", mark.x);
        u.set("y", mark.y);
        u.set("w", mark.width);
        u.set("h", mark.height);
        underlines.call<void>("push", u);
    }
    out.set("compositionUnderlines", underlines);
    val pages = val::array();
    for (const app::PageSize& p : report.pages) {
        val item = val::object();
        item.set("width", p.width);
        item.set("height", p.height);
        item.set("marginLeft", p.marginLeft);
        item.set("marginRight", p.marginRight);
        item.set("marginTop", p.marginTop);
        item.set("marginBottom", p.marginBottom);
        pages.call<void>("push", item);
    }
    out.set("pages", pages);
    out.set("images", packImages(app::fallbackImages()));
    return out;
}

val packOpen(const app::OpenResult& result) {
    val out = val::object();
    out.set("ok", result.ok);
    out.set("error", result.error);
    val faces = val::array();
    for (const std::string& face : result.faces) faces.call<void>("push", face);
    out.set("faces", faces);
    out.set("docState", result.docState);
    out.set("name", result.name);
    return out;
}

app::PageSpecPatch parsePageSpec(const val& item) {
    app::PageSpecPatch spec;
    spec.index = item["index"].as<int>();
    spec.widthHu = item["widthHu"].as<int>();
    spec.heightHu = item["heightHu"].as<int>();
    if (defined(item["marginLeftHu"])) spec.marginLeftHu = item["marginLeftHu"].as<int>();
    if (defined(item["marginRightHu"])) spec.marginRightHu = item["marginRightHu"].as<int>();
    if (defined(item["marginTopHu"])) spec.marginTopHu = item["marginTopHu"].as<int>();
    if (defined(item["marginBottomHu"])) spec.marginBottomHu = item["marginBottomHu"].as<int>();
    return spec;
}

app::ViewPatch parseViewPatch(const val& msg) {
    app::ViewPatch patch;
    patch.reset = defined(msg["reset"]) && msg["reset"].as<bool>();
    const val viewport = msg["viewport"];
    if (defined(viewport)) {
        patch.viewport =
            app::ViewPatch::Viewport{viewport["cssW"].as<int>(), viewport["cssH"].as<int>(), viewport["dpr"].as<double>()};
    }
    const val pages = msg["pages"];
    const val page = msg["page"];
    if (defined(pages)) {
        std::vector<app::PageSpecPatch> specs;
        const int count = pages["length"].as<int>();
        for (int i = 0; i < count; ++i) specs.push_back(parsePageSpec(pages[i]));
        patch.pages = std::move(specs);
        patch.gap = defined(msg["gap"]) ? msg["gap"].as<int>() : 0;
    } else if (defined(page)) {
        patch.page = parsePageSpec(page);
    }
    const val grid = msg["grid"];
    if (defined(grid)) {
        app::ViewPatch::Grid g;
        g.mode = defined(grid["mode"]) ? grid["mode"].as<std::string>() : std::string("auto");
        g.cols = defined(grid["cols"]) ? grid["cols"].as<int>() : 1;
        g.rows = defined(grid["rows"]) ? grid["rows"].as<int>() : 1;
        if (defined(grid["fit"])) g.fit = grid["fit"].as<bool>();
        patch.grid = g;
    }
    if (defined(msg["fit"])) patch.fit = msg["fit"].as<std::string>();
    if (defined(msg["zoom"])) patch.zoom = msg["zoom"].as<int>();
    const val scroll = msg["scroll"];
    if (defined(scroll)) patch.scroll = std::make_pair(scroll["x"].as<int>(), scroll["y"].as<int>());
    if (defined(msg["pageStep"])) patch.pageStep = msg["pageStep"].as<int>();
    if (defined(msg["commit"])) patch.commit = msg["commit"].as<int>();
    return patch;
}

val packView(const app::ViewApplied& applied) {
    const chwp::render::ViewPlan& plan = applied.plan;
    val out = val::object();
    out.set("generation", plan.generation);
    out.set("dpr", plan.dpr);
    out.set("viewW", plan.viewW);
    out.set("viewH", plan.viewH);
    out.set("percent", plan.percent);
    out.set("fitPage", plan.fitPage);
    out.set("fitWidth", plan.fitWidth);
    out.set("zoomMode", std::string(app::zoomModeName(plan.fitPage, plan.fitWidth)));
    out.set("zoomLabel", app::zoomLabel(plan.fitPage, plan.fitWidth, plan.percent));
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
    out.set("gridMode", std::string(app::gridModeName(plan.gridMode)));
    out.set("gridCols", plan.gridCols);
    out.set("gridRows", plan.gridRows);
    out.set("effectiveCols", plan.effectiveCols);
    out.set("rowStepY", plan.rowStepY);
    out.set("pageStepY", plan.pageStepY);
    out.set("ruler", plan.ruler);
    out.set("barX", packBar(plan.barX));
    out.set("barY", packBar(plan.barY));
    val pageBoxes = val::array();
    for (const chwp::render::ViewPage& item : plan.pages) {
        val entry = val::object();
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
    out.set("committed", applied.committed);
    val drop = val::array();
    val draw = val::array();
    for (const int id : plan.drop) drop.call<void>("push", id);
    for (const chwp::render::ViewDraw& item : plan.draw) {
        val entry = val::object();
        entry.set("id", item.id);
        entry.set("x", item.x);
        entry.set("y", item.y);
        entry.set("w", item.w);
        entry.set("h", item.h);
        entry.set("nearest", item.nearest);
        entry.set("layer", item.layer);
        draw.call<void>("push", entry);
    }
    out.set("drop", drop);
    out.set("draw", draw);
    if (plan.job.id == 0) {
        out.set("job", val::null());
    } else {
        val job = val::object();
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
        if (applied.frameJob) job.set("frame", plan.job.id);
        out.set("job", job);
    }
    return out;
}

app::InputPointer parsePointer(const val& item) {
    app::InputPointer p;
    p.device = inputInt(item, "device", 0);
    p.phase = inputOptInt(item, "phase");
    p.id = inputInt(item, "id", 0);
    p.x = inputNumber(item, "x", 0);
    p.y = inputNumber(item, "y", 0);
    p.t = inputNumber(item, "t", 0);
    p.button = inputInt(item, "button", 0);
    return p;
}

app::InputMessage parseInput(const val& msg) {
    app::InputMessage m;
    const val typeValue = msg["type"];
    m.type = defined(typeValue) ? typeValue.as<std::string>() : std::string();
    if (m.type == "pointer") {
        m.pointer = parsePointer(msg);
    } else if (m.type == "pointers") {
        const val items = msg["items"];
        const int len = defined(items) ? items["length"].as<int>() : 0;
        for (int i = 0; i < len; ++i) m.items.push_back(parsePointer(items[i]));
    }
    m.phase = inputOptInt(msg, "phase");
    m.kind = inputInt(msg, "kind", 0);
    m.x = inputNumber(msg, "x", 0);
    m.y = inputNumber(msg, "y", 0);
    m.dx = inputNumber(msg, "dx", 0);
    m.dy = inputNumber(msg, "dy", 0);
    m.vx = inputNumber(msg, "vx", 0);
    m.vy = inputNumber(msg, "vy", 0);
    m.scale = inputNumber(msg, "scale", 1);
    m.rotation = inputNumber(msg, "rotation", 0);
    m.zoom = inputFlag(msg, "zoom");
    m.discrete = inputFlag(msg, "discrete");
    m.dt = inputNumber(msg, "dt", 0);
    m.scrollX = inputInt(msg, "x", 0);
    m.scrollY = inputInt(msg, "y", 0);
    m.cssW = inputInt(msg, "cssW", 1);
    m.cssH = inputInt(msg, "cssH", 1);
    m.dpr = inputNumber(msg, "dpr", 1);
    m.deltaMode = inputInt(msg, "deltaMode", 0);
    m.pageW = inputNumber(msg, "pageW", 0);
    m.pageH = inputNumber(msg, "pageH", 0);
    m.page = inputInt(msg, "page", 0);
    m.percent = inputOptInt(msg, "percent");
    m.dir = inputOptInt(msg, "dir");
    const val modeVal = msg["mode"];
    if (defined(modeVal)) m.mode = modeVal.as<std::string>();
    if (defined(msg["choice"])) m.choice = msg["choice"].as<std::string>();
    m.cols = inputInt(msg, "cols", 1);
    m.rows = inputInt(msg, "rows", 1);
    if (defined(msg["fit"])) m.fit = msg["fit"].as<bool>();
    m.on = inputFlag(msg, "on");
    return m;
}

val packInput(const app::InputReport& report) {
    const chwp::input::Camera& camera = report.camera;
    val out = val::object();
    out.set("scrollX", camera.scrollX);
    out.set("scrollY", camera.scrollY);
    out.set("maxScrollX", camera.maxScrollX);
    out.set("maxScrollY", camera.maxScrollY);
    out.set("percent", camera.percent);
    out.set("fitPage", camera.fitPage);
    out.set("fitWidth", camera.fitWidth);
    out.set("zoomMode", std::string(app::zoomModeName(camera.fitPage, camera.fitWidth)));
    out.set("zoomLabel", app::zoomLabel(camera.fitPage, camera.fitWidth, camera.percent));
    out.set("gridMode", std::string(app::gridModeName(camera.gridMode)));
    out.set("gridCols", camera.gridCols);
    out.set("gridRows", camera.gridRows);
    out.set("effectiveCols", camera.effectiveCols);
    out.set("rowStepY", camera.rowStepY);
    out.set("pageStepY", camera.pageStepY);
    out.set("page", report.page);
    out.set("pageCount", report.pageCount);
    out.set("ownsScroll", report.ownsScroll);
    out.set("settled", report.settled);
    out.set("needsTick", report.needsTick);
    out.set("tapSlop", report.tapSlop);
    out.set("rulersWanted", report.rulersWanted);
    out.set("scrollChanged", camera.scrollChanged);
    out.set("zoomChanged", camera.zoomChanged);
    out.set("coasting", camera.coasting);
    out.set("handled", camera.handled);
    out.set("pinching", camera.pinching);
    out.set("draggingBar", camera.draggingBar);
    out.set("wheeling", camera.wheeling);
    out.set("panning", camera.panning);
    out.set("rotation", camera.rotation);
    out.set("barX", packBar(report.barX));
    out.set("barY", packBar(report.barY));
    if (report.pointer) {
        const app::SelectionReport& sel = report.selection;
        if (sel.needsFontSwap) out.set("needsFontSwap", true);
        if (sel.edited) {
            out.set("edited", true);
            out.set("docState", sel.docState);
        }
        if (sel.attached) {
            out.set("select", true);
            out.set("selectPhase", sel.selectPhase);
            if (sel.pinTap) out.set("pinTap", true);
            out.set("active", sel.active);
            out.set("caret", sel.caret);
            if (sel.styled) {
                out.set("cursorStyle", packCursorStyle(sel.style));
                out.set("text", sel.text);
            }
            if (!sel.event.empty()) out.set("event", sel.event);
            out.set("images", packImages(app::fallbackImages()));
        }
    }
    return out;
}

// ---- 플랫폼 일

EM_BOOL screenFrameTrampoline(double time, void*) { return app::screenFrame(time); }

int logicalCores() {
#if defined(__EMSCRIPTEN_PTHREADS__)
    int cores = emscripten_num_logical_cores();
    if (cores <= 0) cores = static_cast<int>(std::thread::hardware_concurrency());
    return cores > 0 ? cores : 1;
#else
    return 0;
#endif
}

// 원격 화면: 2D 그림 목록을 복사해 셸 훅(Module.onScreenOps)에 넘긴다. 셸이 메인 스레드 캔버스로 옮겨 그린다.
bool presentScreenOps(const chwp::render::ScreenFrame2D& frame) {
    if (EM_ASM_INT({ return typeof Module["onScreenOps"] === "function" ? 1 : 0; }) == 0) return false;
    EM_ASM({ Module["_screenUploads"] = []; });
    for (const chwp::render::ScreenUpload& upload : frame.uploads) {
        // 매크로 인자는 쉼표로 갈리니 JS 객체는 한 칸씩 채운다.
        EM_ASM(
            {
                const start = $3 >>> 0;
                const item = {};
                item.key = $0;
                item.width = $1;
                item.height = $2;
                item.pixels = HEAPU8.slice(start, start + ($1 * $2 * 4 >>> 0)).buffer;
                Module["_screenUploads"].push(item);
            },
            upload.key, upload.width, upload.height, upload.rgba.data());
    }
    return EM_ASM_INT(
               {
                   const opsStart = $2 >>> 0;
                   const pathStart = $4 >>> 2;
                   const dropStart = $6 >>> 2;
                   const frame = {};
                   frame.width = $0;
                   frame.height = $1;
                   frame.ops = HEAPU8.slice(opsStart, opsStart + ($3 >>> 0)).buffer;
                   frame.path = HEAPF32.slice(pathStart, pathStart + $5).buffer;
                   frame.drops = HEAP32.slice(dropStart, dropStart + $7).buffer;
                   frame.uploads = Module["_screenUploads"];
                   Module["_screenUploads"] = [];
                   return Module["onScreenOps"](frame) === false ? 0 : 1;
               },
               frame.width, frame.height, frame.ops.data(), static_cast<int>(frame.ops.size() * sizeof(chwp::render::ScreenOp)),
               frame.path.data(), static_cast<int>(frame.path.size()), frame.drops.data(),
               static_cast<int>(frame.drops.size())) != 0;
}

// 워커에 requestAnimationFrame 이 없는 브라우저는 타이머로 프레임을 돈다.
void startFrameLoop() {
    const bool raf = EM_ASM_INT({ return typeof requestAnimationFrame === "function" ? 1 : 0; }) != 0;
    if (raf) emscripten_request_animation_frame_loop(&screenFrameTrampoline, nullptr);
    else emscripten_set_timeout_loop(&screenFrameTrampoline, 1000.0 / 60.0, nullptr);
}

bool attachScreen(bool remote) { return app::attachScreen(remote); }

// 일꾼 스레드가 끝낸 일의 뒷일을 코어를 부르는 스레드(워커)에서 한다.
#if defined(__EMSCRIPTEN_PTHREADS__)
void runPosted(void* arg) {
    std::unique_ptr<std::function<void()>> work(static_cast<std::function<void()>*>(arg));
    (*work)();
}
#endif

void postToShell(std::function<void()> work) {
#if defined(__EMSCRIPTEN_PTHREADS__)
    emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), &runPosted,
                           new std::function<void()>(std::move(work)));
#else
    work();
#endif
}

void notifyFontsSettled(int batch) {
    val hook = val::module_property("onFontsSettled");
    if (hook.typeOf().as<std::string>() == "function") hook(batch);
}

void resizeCanvas(int width, int height) {
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

void notifyCamera(const app::CameraNote& n) {
    val hook = val::module_property("onCamera");
    if (hook.typeOf().as<std::string>() != "function") return;
    val note = val::object();
    note.set("scrollX", n.scrollX);
    note.set("scrollY", n.scrollY);
    note.set("percent", n.percent);
    note.set("fitPage", n.fitPage);
    note.set("fitWidth", n.fitWidth);
    note.set("zoomMode", std::string(app::zoomModeName(n.fitPage, n.fitWidth)));
    note.set("zoomLabel", app::zoomLabel(n.fitPage, n.fitWidth, n.percent));
    note.set("coasting", n.coasting);
    note.set("pinching", n.pinching);
    note.set("page", n.page);
    note.set("pageCount", n.pageCount);
    note.set("viewW", n.viewW);
    note.set("viewH", n.viewH);
    note.set("maxScrollX", n.maxScrollX);
    note.set("maxScrollY", n.maxScrollY);
    note.set("dpr", n.dpr);
    note.set("ownsScroll", n.ownsScroll);
    note.set("settled", n.settled);
    hook(note);
}

void publishFontFaces(void*, const char* const* faces, int count) {
    val list = val::array();
    for (int i = 0; i < count; ++i) {
        if (!faces || !faces[i] || faces[i][0] == '\0') continue;
        list.call<void>("push", std::string(faces[i]));
    }
    val hook = val::module_property("onFontFaces");
    if (hook.typeOf().as<std::string>() == "function") hook(list);
}

struct InstallHost {
    InstallHost() {
        app::Host host;
        // requestAnimationFrame 이 넘기는 시각과 같은 기준이다. 스레드 빌드의 emscripten_get_now() 는
        // 스레드끼리 맞추려고 기준이 달라서, 프레임 시각과 빼면 음수가 되어 "방금 움직임"이 풀리지 않는다.
        host.nowMs = [] { return emscripten_performance_now(); };
        host.logicalCores = &logicalCores;
        // 웹 스레드 풀은 정해진 수만 미리 띄운다. 한 번에 3개까지만 쓴다.
        host.minWorkers = 1;
        host.maxWorkers = 3;
        // 브라우저 메모리가 좁아 확대한 개체 칸은 2048 변, 2048×1536 넓이까지만 굽는다.
        // 휴대폰·태블릿인지(메모리가 좁은 기기). 예산은 core 가 정한다.
        host.smallMemory = EM_ASM_INT({
            const nav = typeof navigator !== "undefined" ? navigator : null;
            return !!nav && (nav.maxTouchPoints > 1 || /Mobi|Android|iPhone|iPad/.test(nav.userAgent || "")) ? 1 : 0;
        }) != 0;
        host.zoomedFrameSide = 2048;
        host.zoomedFramePixels = 2048LL * 1536LL;
        // 웹 화면은 WebGPU 로 글자·그림·테두리를 직접 그린다. GPU 를 못 쓰면 core 가 끈다.
        host.screenVectors = true;
        host.startFrameLoop = &startFrameLoop;
        host.presentScreenOps = &presentScreenOps;
        host.resizeSurface = &resizeCanvas;
        host.cameraChanged = &notifyCamera;
        host.post = &postToShell;
        host.fontsSettled = &notifyFontsSettled;
        app::setHost(std::move(host));
        chwp::layout::setFontFaceRequestHandler(&publishFontFaces, nullptr);
    }
};

InstallHost gInstallHost;

// ---- 내보내는 함수(값 변환만)

std::string detectFormat(const val& input) { return app::detectFormat(toBytes(input)); }
std::string parse(const val& input, bool pretty, bool lineSegs) { return app::parse(toBytes(input), pretty, lineSegs); }
std::string extractText(const val& input) { return app::extractText(toBytes(input)); }
std::string extractPages(const val& input) { return app::extractPages(toBytes(input)); }
std::string extractAnchorPages(const val& input) { return app::extractAnchorPages(toBytes(input)); }
std::string extractInnerLines(const val& input) { return app::extractInnerLines(toBytes(input)); }
std::string extractLayout(const val& input) { return app::extractLayout(toBytes(input)); }
std::string layoutScene(const val& input) { return app::layoutScene(toBytes(input)); }

val saveDocument(const std::string& name) {
    const app::SavedDocument saved = app::saveDocument(name);
    val out = val::object();
    out.set("bytes", saved.bytes ? toUint8ArrayVal(*saved.bytes) : val::null());
    out.set("name", saved.name);
    out.set("format", saved.format);
    out.set("error", saved.error);
    out.set("docState", saved.docState);
    return out;
}

val convertDocument(const val& input, const std::string& format) {
    const std::optional<chwp::Bytes> bytes = app::convertDocument(toBytes(input), format);
    return bytes ? toUint8ArrayVal(*bytes) : val::null();
}

void loadFont(const std::string& face, const val& input) { chwp::layout::registerFontFace(face, toBytes(input)); }

void setFontCatalog(const std::string& json) { chwp::layout::setFontCatalog(json); }
void setFontLicenseSwaps(const std::string& json) { chwp::layout::setFontLicenseSwaps(json); }
void setFontReplacements(const std::string& json) { chwp::layout::setFontReplacements(json); }

std::vector<std::string> faceList(const val& faces) {
    std::vector<std::string> names;
    if (!defined(faces)) return names;
    const int count = faces["length"].as<int>();
    for (int i = 0; i < count; ++i) {
        const val face = faces[i];
        if (face.isString()) names.push_back(face.as<std::string>());
    }
    return names;
}

val fontFetchPlan(const val& faces) {
    val out = val::array();
    for (const app::FontFetchJob& job : app::fontFetchPlan(faceList(faces))) {
        val item = val::object();
        item.set("id", job.id);
        item.set("batch", job.batch);
        item.set("folder", job.folder);
        item.set("file", job.file);
        item.set("face", job.face);
        out.call<void>("push", item);
    }
    return out;
}

void fontFetched(int id, const val& input) { app::fontFetched(id, toBytes(input)); }

val fontGapFaces() {
    val out = val::array();
    for (const std::string& face : app::fontGapFaces()) out.call<void>("push", face);
    return out;
}


int installFont(const val& faces, const val& input) { return app::installFont(faceList(faces), toBytes(input)); }

val pickableFonts() {
    val out = val::array();
    for (const std::string& face : app::pickableFonts()) out.call<void>("push", face);
    return out;
}

val licenseFontSwaps() {
    val out = val::array();
    for (const app::FontSwapRow& swap : app::licenseFontSwaps()) {
        val row = val::object();
        row.set("from", swap.from);
        row.set("to", swap.to);
        out.call<void>("push", row);
    }
    return out;
}

val openDocument(const val& input) { return packOpen(app::openDocument(toBytes(input))); }
val newDocument() { return packOpen(app::newDocument()); }

val viewApply(const val& msg) { return packView(app::applyView(parseViewPatch(msg))); }

int screenAttachState() { return static_cast<int>(app::screenAttachState()); }

val packColorMemory(const app::ColorMemory& memory) {
    val out = val::object();
    val recent = val::array();
    for (const std::string& hex : memory.recent) recent.call<void>("push", hex);
    out.set("recent", recent);
    out.set("pen", memory.pen);
    out.set("saved", memory.saved);
    return out;
}

val setRecentColors(const std::string& saved) { return packColorMemory(app::setRecentColors(saved)); }
val rememberColor(const std::string& hex) { return packColorMemory(app::rememberColor(hex)); }

val pointerEvent(int kind, int page, int x, int y, int percent) {
    const app::PointerReport report = app::pointerEvent(kind, page, x, y, percent);
    val out = val::object();
    out.set("active", report.view.active);
    out.set("story", report.view.story);
    out.set("caret", report.view.caret);
    out.set("para", report.cursor.paragraph);
    out.set("offset", report.cursor.offset);
    out.set("hasSelection", report.cursor.hasSelection);
    if (report.styled) {
        out.set("cursorStyle", packCursorStyle(report.style));
        out.set("text", report.text);
    }
    if (!report.event.empty()) out.set("event", report.event);
    out.set("images", packImages(app::fallbackImages()));
    return out;
}

val cursorStyleNow() { return packCursorStyle(app::cursorStyleNow()); }
val formatFieldsNow() { return packFields(app::formatFieldsNow()); }
val pageSetupFieldsNow() { return packFields(app::pageSetupFieldsNow()); }

val editText(const std::string& text, bool composing) { return packEditReport(app::editText(text, composing)); }
val editKey(int key, bool shift, bool ctrl, bool alt) { return packEditReport(app::editKey(key, shift, ctrl, alt)); }

// 입력칸 사건 이름과 DOM inputType 이름. 사건을 바꿔 넘길 때와 규칙 표를 셸 모양으로 옮길 때 같이 쓴다.
struct FieldKindName {
    const char* name;
    chwp::input::FieldEventKind kind;
};
constexpr FieldKindName kFieldKindNames[] = {
    {"compositionstart", chwp::input::FieldEventKind::CompositionStart},
    {"compositionupdate", chwp::input::FieldEventKind::CompositionUpdate},
    {"compositionend", chwp::input::FieldEventKind::CompositionEnd},
    {"input", chwp::input::FieldEventKind::Input},
    {"keydown", chwp::input::FieldEventKind::KeyDown},
    {"turnend", chwp::input::FieldEventKind::TurnEnd},
    {"wake", chwp::input::FieldEventKind::Wake},
    {"reset", chwp::input::FieldEventKind::Reset},
    {"paste", chwp::input::FieldEventKind::Paste},
};
struct FieldInputName {
    const char* name;
    chwp::input::FieldInputType type;
};
constexpr FieldInputName kFieldInputNames[] = {
    {"insertText", chwp::input::FieldInputType::InsertText},
    {"insertReplacementText", chwp::input::FieldInputType::InsertText},
    {"insertFromPaste", chwp::input::FieldInputType::InsertText},
    {"deleteContentBackward", chwp::input::FieldInputType::DeleteBackward},
    {"insertLineBreak", chwp::input::FieldInputType::LineBreak},
    {"insertParagraph", chwp::input::FieldInputType::LineBreak},
    {"insertCompositionText", chwp::input::FieldInputType::Composition},
    {"deleteCompositionText", chwp::input::FieldInputType::Composition},
};

int fieldMatchValue(chwp::input::FieldMatch match) {
    return match == chwp::input::FieldMatch::Any ? -1 : match == chwp::input::FieldMatch::Yes ? 1 : 0;
}

// 입력칸 규칙 표(코어 input::fieldRules)를 JS 배열로 옮긴다. 조건 -1 은 보지 않음, 1·0 은 참·거짓이다.
val fieldRules() {
    val out = val::array();
    for (const chwp::input::FieldRule& rule : chwp::input::fieldRules()) {
        val item = val::object();
        for (const FieldKindName& entry : kFieldKindNames) {
            if (entry.kind == rule.on) item.set("on", std::string(entry.name));
        }
        if (!rule.anyInputType) {
            val types = val::array();
            for (const FieldInputName& entry : kFieldInputNames) {
                if (entry.type == rule.inputType) types.call<void>("push", std::string(entry.name));
            }
            item.set("inputTypes", types);
        }
        item.set("key", rule.key);
        item.set("ime", fieldMatchValue(rule.ime));
        item.set("repeat", fieldMatchValue(rule.repeat));
        item.set("endComposition", fieldMatchValue(rule.endComposition));
        item.set("hasText", fieldMatchValue(rule.hasText));
        item.set("send", rule.send);
        item.set("preventDefault", rule.preventDefault);
        item.set("clearField", rule.clearField);
        item.set("blur", rule.blur);
        item.set("composing", rule.composing == chwp::input::FieldSet::Keep ? -1 : rule.composing == chwp::input::FieldSet::On ? 1 : 0);
        item.set("turnEnd", rule.turnEnd);
        item.set("toField", rule.toField);
        out.call<void>("push", item);
    }
    return out;
}

// 입력칸 사건을 코어 구조로 바꿔 넘기고, 편집 결과와 입력칸에 할 일을 JS 객체로 옮긴다.
val editField(const val& event) {
    chwp::input::FieldEvent in;
    const std::string kind = event["kind"].as<std::string>();
    in.kind = chwp::input::FieldEventKind::Reset;
    for (const FieldKindName& entry : kFieldKindNames) {
        if (kind == entry.name) in.kind = entry.kind;
    }
    const std::string inputType = event["inputType"].isString() ? event["inputType"].as<std::string>() : "";
    in.inputType = chwp::input::FieldInputType::Other;
    for (const FieldInputName& entry : kFieldInputNames) {
        if (inputType == entry.name) in.inputType = entry.type;
    }
    if (event["data"].isString()) in.data = event["data"].as<std::string>();
    if (event["value"].isString()) in.value = event["value"].as<std::string>();
    in.isComposing = event["isComposing"].as<bool>();
    in.imeKey = event["imeKey"].as<bool>();
    in.repeat = event["repeat"].as<bool>();
    in.hasText = event["hasText"].as<bool>();
    in.shift = event["shift"].as<bool>();
    in.endComposition = event["endComposition"].as<bool>();
    in.wakeId = event["wakeId"].isNumber() ? event["wakeId"].as<int>() : 0;
    const app::FieldResult result = app::editField(in);
    val out = result.edited ? packEditReport(result.report) : val::object();
    val field = val::object();
    field.set("insert", result.field.insertIntoField);
    if (result.field.wake == chwp::input::FieldWake::After) {
        const char* slot = result.field.wakeSlot == chwp::input::FieldWakeSlot::Repeat ? "repeat" : "sync";
        field.set("wakeSlot", std::string(slot));
        field.set("wakeAfterMs", result.field.wakeAfterMs);
        field.set("wakeId", result.field.wakeId);
    }
    out.set("field", field);
    out.set("edited", result.edited);
    return out;
}
val editUndo() { return packEditReport(app::editUndo()); }
val editRedo() { return packEditReport(app::editRedo()); }
val editInsertTable(int rows, int cols) { return packEditReport(app::editInsertTable(rows, cols)); }
val editInsertPicture(const val& input) { return packEditReport(app::editInsertPicture(toBytes(input))); }

// chosenVal 은 셸에서 고른 {막힌 글꼴: 바꿀 글꼴}이다.
val editSwapFonts(const val& chosenVal) {
    std::vector<std::pair<std::string, std::string>> chosen;
    if (defined(chosenVal)) {
        const val keys = val::global("Object").call<val>("keys", chosenVal);
        const int count = keys["length"].as<int>();
        for (int i = 0; i < count; ++i) {
            const std::string from = keys[i].as<std::string>();
            const val to = chosenVal[from];
            if (to.isString()) chosen.emplace_back(from, to.as<std::string>());
        }
    }
    return packEditReport(app::editSwapFonts(chosen));
}

// 셸이 준 객체의 이름·값을 그대로 코어에 넘긴다. 이름과 단위는 코어(setFormatField)가 정한다.
val editStyle(const val& patchVal) {
    chwp::render::FormatPatch patch;
    if (defined(patchVal)) {
        const val keys = val::global("Object").call<val>("keys", patchVal);
        const int count = keys["length"].as<int>();
        for (int i = 0; i < count; ++i) {
            const std::string key = keys[i].as<std::string>();
            const val value = patchVal[key];
            const std::string type = value.typeOf().as<std::string>();
            if (type == "boolean") {
                chwp::render::setFormatField(patch, key, value.as<bool>());
            } else if (type == "number") {
                chwp::render::setFormatField(patch, key, value.as<double>());
            } else if (type == "string") {
                chwp::render::setFormatField(patch, key, value.as<std::string>());
            }
        }
    }
    return packEditReport(app::editStyle(patch));
}

// 셸이 준 편집 용지 값을 그대로 코어에 넘긴다. 이름과 단위는 코어(editApplyPageSetup)가 정한다.
val editPageSetup(const val& patchVal, bool wholeDocument) {
    chwp::render::FormatFields fields;
    if (defined(patchVal)) {
        const val keys = val::global("Object").call<val>("keys", patchVal);
        const int count = keys["length"].as<int>();
        for (int i = 0; i < count; ++i) {
            const std::string key = keys[i].as<std::string>();
            const val value = patchVal[key];
            const std::string type = value.typeOf().as<std::string>();
            if (type == "boolean") fields.emplace_back(key, value.as<bool>());
            else if (type == "number") fields.emplace_back(key, value.as<double>());
        }
    }
    return packEditReport(app::editPageSetup(fields, wholeDocument));
}

// 셸이 준 객체의 이름·값을 그대로 옮긴다. 이름 차례(고친 차례)를 지킨다.
chwp::render::FormatFields fieldsOf(const val& object) {
    chwp::render::FormatFields fields;
    if (!defined(object)) return fields;
    const val keys = val::global("Object").call<val>("keys", object);
    const int count = keys["length"].as<int>();
    for (int i = 0; i < count; ++i) {
        const std::string key = keys[i].as<std::string>();
        const val value = object[key];
        const std::string type = value.typeOf().as<std::string>();
        if (type == "boolean") fields.emplace_back(key, value.as<bool>());
        else if (type == "number") fields.emplace_back(key, value.as<double>());
        else if (type == "string") fields.emplace_back(key, value.as<std::string>());
    }
    return fields;
}

val packFieldValue(const chwp::render::FormatValue& value) {
    if (const bool* b = std::get_if<bool>(&value)) return val(*b);
    if (const double* n = std::get_if<double>(&value)) return val(*n);
    return val(std::get<std::string>(value));
}

val formatDialogView(int kind, const val& patch) {
    const chwp::render::FormatView view = app::formatDialogView(kind, fieldsOf(patch));
    val values = val::object();
    for (const auto& [key, value] : view.values) values.set(key, packFieldValue(value));
    val specs = val::object();
    for (const chwp::render::FieldSpec& spec : view.specs) {
        val item = val::object();
        item.set("unit", spec.unit);
        item.set("min", spec.min);
        item.set("max", spec.max);
        item.set("step", spec.step);
        item.set("digits", spec.digits);
        val options = val::array();
        for (const auto& [id, label] : spec.options) {
            val option = val::array();
            option.call<void>("push", id);
            option.call<void>("push", label);
            options.call<void>("push", option);
        }
        item.set("options", options);
        specs.set(spec.key, item);
    }
    val out = val::object();
    out.set("values", values);
    out.set("specs", specs);
    return out;
}

val editFormatDialog(int kind, const val& patch, bool wholeDocument) {
    return packEditReport(app::editFormatDialog(kind, fieldsOf(patch), wholeDocument));
}

val formatDialogPreview(int kind, const val& patch, int width, int height) {
    return pixelsVal(app::formatDialogPreview(kind, fieldsOf(patch), width, height));
}

val styleNames() {
    val out = val::array();
    for (const std::string& name : app::styleNames()) out.call<void>("push", name);
    return out;
}

val keyBindings() {
    val out = val::array();
    for (const app::KeyBinding& binding : app::keyBindings()) {
        val item = val::object();
        item.set("key", binding.key);
        item.set("shift", binding.shift);
        item.set("ctrl", binding.ctrl);
        item.set("alt", binding.alt);
        item.set("command", binding.command);
        item.set("editKey", binding.editKey);
        item.set("editorOnly", binding.editorOnly);
        out.call<void>("push", item);
    }
    return out;
}

val inputEvent(const val& msg) { return packInput(app::input(parseInput(msg))); }

}  // namespace

EMSCRIPTEN_BINDINGS(chwp) {
    emscripten::function("detectFormat", &detectFormat);
    emscripten::function("parse", &parse);
    emscripten::function("saveDocument", &saveDocument);
    emscripten::function("editDeleteSelection", +[]() { return packEditReport(app::editDeleteSelection()); });
    emscripten::function("convertDocument", &convertDocument);
    emscripten::function("extractText", &extractText);
    emscripten::function("extractPages", &extractPages);
    emscripten::function("extractAnchorPages", &extractAnchorPages);
    emscripten::function("extractInnerLines", &extractInnerLines);
    emscripten::function("extractLayout", &extractLayout);
    emscripten::function("layoutScene", &layoutScene);
    emscripten::function("loadFont", &loadFont);
    emscripten::function("trimCaches", &app::trimCaches);
    emscripten::function("setCacheBudget", &app::setCacheBudget);
    emscripten::function("cacheUsage", &app::cacheUsage);
    emscripten::function("frameStats", &app::frameStats);
    emscripten::function("memoryReport", &app::memoryReport);
    // 지금 WASM 메모리 크기(바이트). 기기 메모리 한도와 견주려고 원격 로그에 싣는다.
    emscripten::function("memoryBytes", +[]() { return static_cast<double>(emscripten_get_heap_size()); });
    // malloc 이 지금 내준 바이트. WASM 메모리는 줄지 않으니 순간 최대치와 지금 쓰는 양을 나눠 보려고 둔다.
    emscripten::function("memoryInUse", +[]() { return static_cast<double>(mallinfo().uordblks); });
    emscripten::function("openDocument", &openDocument);
    emscripten::function("newDocument", &newDocument);
    emscripten::function("setFontCatalog", &setFontCatalog);
    emscripten::function("setFontLicenseSwaps", &setFontLicenseSwaps);
    emscripten::function("setFontReplacements", &setFontReplacements);
    emscripten::function("fontFetchPlan", &fontFetchPlan);
    emscripten::function("fontFetched", &fontFetched);
    emscripten::function("fontGapFaces", &fontGapFaces);
    emscripten::function("fontFetchFailed", &app::fontFetchFailed);
    emscripten::function("installFont", &installFont);
    emscripten::function("pickableFonts", &pickableFonts);
    emscripten::function("licenseFontSwaps", &licenseFontSwaps);
    emscripten::function("editSwapFonts", &editSwapFonts);
    emscripten::function("dropPendingInsert", &chwp::render::dropPendingInsert);
    // 저장된 줄 높이를 쓸지는 core 가 정한다.
    emscripten::function("planDocument", static_cast<std::string (*)()>(&app::planDocument));
    emscripten::function("setMeasuredLineHeight", &chwp::render::setMeasuredLineHeight);
    emscripten::function("beginGrind", &app::beginGrind);
    emscripten::function("viewApply", &viewApply);
    emscripten::function("editField", &editField);
    emscripten::function("fieldRules", &fieldRules);
    emscripten::function("keyBindings", &keyBindings);
    emscripten::function("zoomDialogSpec", &app::zoomDialogSpec);
    emscripten::function("styleNames", &styleNames);
    emscripten::function("setFontSwapPreferences", &app::setFontSwapPreferences);
    emscripten::function("fontSwapPreferences", &app::fontSwapPreferences);
    emscripten::function("setRecentColors", &setRecentColors);
    emscripten::function("rememberColor", &rememberColor);
    emscripten::function("formatDialogView", &formatDialogView);
    emscripten::function("editFormatDialog", &editFormatDialog);
    emscripten::function("formatDialogPreview", &formatDialogPreview);
    emscripten::function("screenAttachState", &screenAttachState);
    emscripten::function("pointerEvent", &pointerEvent);
    emscripten::function("cursorStyle", &cursorStyleNow);
    emscripten::function("hoverCursor", &app::hoverCursor);
    emscripten::function("editText", &editText);
    emscripten::function("editKey", &editKey);
    emscripten::function("editUndo", &editUndo);
    emscripten::function("editInsertTable", &editInsertTable);
    emscripten::function("setDrawTool", &chwp::render::setDrawTool);
    emscripten::function("editInsertPicture", &editInsertPicture);
    emscripten::function("editRedo", &editRedo);
    emscripten::function("editStyle", &editStyle);
    emscripten::function("formatFields", &formatFieldsNow);
    emscripten::function("pageSetupFields", &pageSetupFieldsNow);
    emscripten::function("editPageSetup", &editPageSetup);
    emscripten::function("inputEvent", &inputEvent);
    emscripten::function("attachScreen", &attachScreen);
}
