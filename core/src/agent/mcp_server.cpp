#include "chwp/agent_mcp.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "agent/json_read.hpp"
#include "chwp/common/crypto.hpp"
#include "chwp/export/json.hpp"

// MCP 서버. 메시지 모양은 MCP 명세(JSON-RPC 2.0, stdio 는 한 줄에 메시지 하나)를 따른다.
// 도구 하나는 agentCall 호출 하나다. 도구 인자를 그대로 요청 필드로 옮기고 call 이름만 붙인다.
// 요청·응답의 뜻은 skills/chwp-agent/reference.md 에 있고, 그 글을 hwp_reference 도구로 돌려준다.

namespace chwp::agent {

namespace {

using json_detail::Json;
using json_detail::JsonReader;

// 빌드할 때 넣은 skills/chwp-agent/reference.md. 없으면 빈 글이다.
const unsigned char kReference[] = {
#include "agent_reference.inc"
    0};

std::string_view referenceText() {
    return {reinterpret_cast<const char*>(kReference), sizeof(kReference) - 1};
}

// ---------------------------------------------------------------------------
// JSON 쓰기
// ---------------------------------------------------------------------------

std::string quote(std::string_view text) {
    JsonWriter w(false);
    w.value(text);
    return w.take();
}

std::string numberText(double number) {
    char buffer[40];
    if (std::isfinite(number) && number == std::floor(number) && std::fabs(number) < 9.0e15) {
        std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(number));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.17g", number);
    }
    return buffer;
}

std::string serialize(const Json& value) {
    switch (value.type) {
        case Json::Type::Null: return "null";
        case Json::Type::Bool: return value.boolean ? "true" : "false";
        case Json::Type::Number: return numberText(value.number);
        case Json::Type::String: return quote(value.text);
        case Json::Type::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < value.items.size(); ++i) out += (i ? "," : "") + serialize(value.items[i]);
            return out + "]";
        }
        case Json::Type::Object: {
            std::string out = "{";
            for (std::size_t i = 0; i < value.fields.size(); ++i) {
                out += (i ? "," : "") + quote(value.fields[i].first) + ":" + serialize(value.fields[i].second);
            }
            return out + "}";
        }
    }
    return "null";
}

// ---------------------------------------------------------------------------
// 도구
// ---------------------------------------------------------------------------

enum class Kind { Call, Open, Reference };

struct Tool {
    const char* name;
    const char* call;  // agentCall 의 call 이름
    Kind kind;
    bool readOnly;
    const char* description;
    std::string schema;  // inputSchema JSON
};

// 쪽·위치·구간처럼 모양이 여럿인 값은 type 을 적지 않고 설명으로 모양을 준다(agentCall 이 판단한다).
constexpr const char* kPage =
    R"("page":{"description":"쪽. 쪽 번호 3, {\"page\":3,\"section\":2,\"nth\":1}, 또는 문서 안 물리 순서 {\"order\":5}(hwp_pages 의 order). order 가 가장 확실하다"})";

const Tool kTools[] = {
    {"hwp_open", "", Kind::Open, false,
     "한글 문서(.hwp, .hwpx)를 연다. 다른 도구보다 먼저 부른다. 한 번에 한 문서만 열려 있고, 새로 열면 앞 문서는 닫힌다. "
     "고친 내용을 남기려면 닫기 전에 hwp_save 로 저장한다. 응답은 hwp_info 와 같다.",
     R"({"type":"object","properties":{"path":{"type":"string","description":"문서 파일 경로"}},"required":["path"]})"},
    {"hwp_info", "info", Kind::Call, true,
     "열린 문서의 형식, 쪽·구역·글 묶음 수, 읽기 경고, 받지 못해 대체 글꼴로 그린 글꼴(missingFonts), 그리지 못하는 글자(missingGlyphs).",
     R"({"type":"object","properties":{}})"},
    {"hwp_pages", "pages", Kind::Call, true,
     "쪽 목록. 쪽 번호(page), 구역(section), 쪽 번호 글(label), 문서 안 물리 순서(order), 크기(HWPUNIT, 1mm ≈ 283).",
     R"({"type":"object","properties":{}})"},
    {"hwp_stories", "stories", Kind::Call, true,
     "글 묶음(story) 목록. 구역 본문, 표 칸, 글상자, 머리말·꼬리말, 각주·미주, 캡션, 바탕쪽이 저마다 글 묶음이다. "
     "위치는 {story, paragraph, char} 로 가리킨다. 구조는 hwp_reference(topic \"문서 구조\").",
     R"({"type":"object","properties":{}})"},
    {"hwp_text", "text", Kind::Call, true,
     "문단 글을 읽는다(여러 글 묶음을 한 번에). 글이 maxChars 를 넘으면 멈추고 next 를 준다. 이어 읽으려면 next 의 값을 그대로 넣는다. "
     "글 안의 U+FFFC 는 글자처럼 놓인 표·그림 같은 개체다.",
     R"({"type":"object","properties":{"story":{"type":"integer","description":"이 글 묶음만. 없으면 모두"},"from":{"type":"integer","description":"첫 문단 번호"},"count":{"type":"integer","description":"문단 수"},"maxChars":{"type":"integer","description":"이만큼 넘으면 멈춘다. 기본 30000"},"startStory":{"type":"integer","description":"이어 읽기. next.startStory 값"},"empty":{"type":"boolean","description":"빈 문단도 낸다"}}})"},
    {"hwp_paragraph", "paragraph", Kind::Call, true,
     "문단 하나의 글, 스타일, 정렬, 줄 간격, 글자 모양 구간(runs), 붙은 개체(objects: 표·그림·도형·각주 …). detail 이면 모양 값을 모두 준다.",
     R"({"type":"object","properties":{"story":{"type":"integer"},"paragraph":{"type":"integer"},"detail":{"type":"boolean"}},"required":["story","paragraph"]})"},
    {"hwp_table", "table", Kind::Call, true,
     "표 칸 글 묶음(story) 하나가 든 표 전체: 줄·칸 수, 칸 너비·높이, 칸마다 story·row·col·합친 범위·글·서식.",
     R"({"type":"object","properties":{"story":{"type":"integer","description":"표 칸 하나의 story"}},"required":["story"]})"},
    {"hwp_section", "section", Kind::Call, true,
     "구역(1부터) 하나의 편집 용지: 용지 크기, 여백, 제본, 격자, 단, 쪽 번호 시작. 길이는 HWPUNIT(1mm ≈ 283).",
     R"({"type":"object","properties":{"section":{"type":"integer","description":"1부터"}},"required":["section"]})"},
    {"hwp_layout", "layout", Kind::Call, true,
     "쪽 하나에 그려진 배치: 본문 틀, 줄(문단·글자 범위·상자·넘침), 표·칸·그림·도형 상자. 그림 없이 줄 나눔과 넘침을 확인할 때 쓴다. "
     "좌표는 쪽 왼쪽 위에서 잰 HWPUNIT.",
     (R"({"type":"object","properties":{)" + std::string(kPage) +
      R"(,"text":{"type":"boolean","description":"false 면 줄 글을 뺀다"}},"required":["page"]})")},
    {"hwp_find", "find", Kind::Call, true, "글을 찾는다. 찾은 자리(from, to), 쪽, 앞뒤 글을 준다. 문단 경계는 넘지 않는다.",
     R"({"type":"object","properties":{"text":{"type":"string"},"story":{"type":"integer","description":"이 글 묶음에서만"},"matchCase":{"type":"boolean"},"limit":{"type":"integer"}},"required":["text"]})"},
    {"hwp_render", "render", Kind::Call, true,
     "쪽 하나를 PNG 그림으로 그려 돌려준다. 고친 결과를 눈으로 확인할 때 쓴다. dpi 기본 96.",
     (R"({"type":"object","properties":{)" + std::string(kPage) +
      R"(,"dpi":{"type":"number"},"pixelHeight":{"type":"integer","description":"주면 dpi 대신 이 높이. 32~8000"}},"required":["page"]})")},
    {"hwp_apply", "apply", Kind::Call, false,
     "문서를 고친다. ops 는 차례로 적용하는 op 목록이다: insertText, delete, replace, format, insertTable, insertPicture, setText, "
     "insertRows, insertColumns, deleteRows, deleteColumns, mergeCells, splitCell, cellBorder, cellFill, insertBreak, pageSetup, "
     "replaceAll, 편집기 명령(command) 등. op 마다 모양이 달라 처음 쓰기 전에 hwp_reference(topic \"apply\") 를 읽는다. "
     "위치는 {\"story\":1,\"paragraph\":0,\"char\":4} 또는 \"1:0:4\", 구간은 {\"from\":위치,\"to\":위치} 또는 \"1:0:4-1:0:9\".",
     R"({"type":"object","properties":{"ops":{"type":"array","items":{"type":"object"},"description":"op 목록. 예 [{\"op\":\"insertText\",\"at\":\"1:0:0\",\"text\":\"안녕\"}]"}},"required":["ops"]})"},
    {"hwp_view", "view", Kind::Call, true,
     "편집기 명령의 대화 상자 값(지금 모양)을 읽는다. 인자는 apply 의 command op 와 같다. hwp_reference(topic \"view\").",
     R"({"type":"object","properties":{},"additionalProperties":true})"},
    {"hwp_undo", "undo", Kind::Call, false, "마지막 고치기를 되돌린다.", R"({"type":"object","properties":{}})"},
    {"hwp_redo", "redo", Kind::Call, false, "되돌린 고치기를 다시 한다.", R"({"type":"object","properties":{}})"},
    {"hwp_save", "save", Kind::Call, false,
     "열린 문서를 저장한다. 형식은 path 확장자(.hwp, .hwpx, .pdf) 또는 format 으로 정한다. 원본을 덮어쓰지 말고 새 경로에 저장한다.",
     R"({"type":"object","properties":{"path":{"type":"string"},"format":{"type":"string","enum":["hwp","hwpx","pdf"]},"dpi":{"type":"number","description":"pdf 그림 해상도 72~600, 기본 200"}},"required":["path"]})"},
    {"hwp_reference", "", Kind::Reference, true,
     "요청·op 모양 설명서. topic 없이 부르면 목차와 공통 규칙을 준다. 예: \"apply\", \"편집기 명령 op\", \"format 의 set 이름\", "
     "\"개체 놓기\", \"문서 구조\", \"text\", \"layout\".",
     R"({"type":"object","properties":{"topic":{"type":"string","description":"절 제목"}}})"},
};

constexpr const char* kInstructions =
    "한글 문서(HWP·HWPX)를 읽고, 찾고, 고치고, 쪽 그림으로 확인하고, HWP·HWPX·PDF 로 저장한다. "
    "먼저 hwp_open 으로 문서를 연다. 글은 hwp_text, 구조는 hwp_stories·hwp_paragraph·hwp_table 로 읽는다. "
    "고치기는 hwp_apply 이고 op 모양은 hwp_reference 에 있다. 고친 뒤 hwp_layout 이나 hwp_render 로 확인하고, "
    "hwp_save 로 새 경로에 저장한다. 길이 단위는 HWPUNIT(1pt = 100, 1mm ≈ 283).";

const Tool* findTool(std::string_view name) {
    for (const Tool& tool : kTools) {
        if (name == tool.name) return &tool;
    }
    return nullptr;
}

// reference.md 의 절 하나. 제목이 topic 과 같거나 topic 을 담은 첫 절이다.
std::string referenceSection(std::string_view topic) {
    const std::string_view text = referenceText();
    if (text.empty()) return "설명서가 이 빌드에 들어 있지 않습니다.";
    struct Heading {
        std::size_t at;
        int level;
        std::string title;
    };
    std::vector<Heading> headings;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        int level = 0;
        while (level < static_cast<int>(line.size()) && line[level] == '#') ++level;
        if (level >= 2 && level <= 3 && level < static_cast<int>(line.size()) && line[level] == ' ') {
            headings.push_back({pos, level, std::string(line.substr(level + 1))});
        }
        pos = end + 1;
    }
    auto sectionOf = [&](std::size_t i) {
        std::size_t end = text.size();
        for (std::size_t j = i + 1; j < headings.size(); ++j) {
            if (headings[j].level <= headings[i].level) {
                end = headings[j].at;
                break;
            }
        }
        return std::string(text.substr(headings[i].at, end - headings[i].at));
    };
    if (topic.empty()) {
        std::string out = "목차(hwp_reference 의 topic 으로 준다):\n";
        for (const Heading& h : headings) out += std::string(h.level == 3 ? "  - " : "- ") + h.title + "\n";
        for (std::size_t i = 0; i < headings.size(); ++i) {
            if (headings[i].title == "공통") out += "\n" + sectionOf(i);
        }
        return out;
    }
    for (std::size_t i = 0; i < headings.size(); ++i) {
        if (headings[i].title == topic) return sectionOf(i);
    }
    for (std::size_t i = 0; i < headings.size(); ++i) {
        if (headings[i].title.find(topic) != std::string::npos) return sectionOf(i);
    }
    return "그런 절이 없습니다: " + std::string(topic) + ". topic 없이 부르면 목차를 줍니다.";
}

std::string textContent(std::string_view text) { return R"({"type":"text","text":)" + quote(text) + "}"; }

std::string toolResult(const std::vector<std::string>& content, bool error) {
    std::string out = R"({"content":[)";
    for (std::size_t i = 0; i < content.size(); ++i) out += (i ? "," : "") + content[i];
    return out + R"(],"isError":)" + (error ? "true" : "false") + "}";
}

const char* const kVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

}  // namespace

std::string mcpToolsJson() {
    std::string out = "[";
    bool first = true;
    for (const Tool& tool : kTools) {
        out += first ? "" : ",";
        first = false;
        out += R"({"name":)" + quote(tool.name) + R"(,"description":)" + quote(tool.description) +
               R"(,"inputSchema":)" + tool.schema + R"(,"annotations":{"readOnlyHint":)" +
               (tool.readOnly ? "true" : "false") + "}}";
    }
    return out + "]";
}

struct McpServer::Impl {
    McpHost host;
    std::unique_ptr<AgentDocument> doc;
    int fontRequests = 0;
    std::string pendingFontId;

    // core 가 달라는 글꼴을 캐시에서 넣고, 없는 것은 셸에 받아 달라고 한 뒤 다시 넣는다. 빈 목록이 나올 때까지 되묻는다.
    void loadFonts() {
        if (!doc) return;
        for (int round = 0; round < 50; ++round) {
            const std::vector<FontFile> plan = doc->fontPlan();
            if (plan.empty()) return;
            std::vector<const FontFile*> absent;
            for (const FontFile& file : plan) {
                Bytes bytes;
                if (host.readFont && host.readFont(file.path, bytes)) doc->fontArrived(file.id, std::move(bytes));
                else absent.push_back(&file);
            }
            if (!absent.empty() && host.fetchFonts) {
                std::vector<std::string> paths;
                for (const FontFile* file : absent) paths.push_back(file->path);
                host.fetchFonts(paths);
            }
            for (const FontFile* file : absent) {
                Bytes bytes;
                if (host.fetchFonts && host.readFont && host.readFont(file->path, bytes)) {
                    doc->fontArrived(file->id, std::move(bytes));
                } else {
                    doc->fontMissing(file->id);
                }
            }
            doc->fontsDone();
        }
    }

    // 받지 못한 글꼴이 있으면 에이전트가 사용자에게 알리고(필요하면 허락을 받고) 다시 열 수 있게 안내한다.
    std::string fontNotice() const {
        if (!doc) return {};
        const std::vector<std::string> missing = doc->missingFonts();
        if (missing.empty()) return {};
        std::string names;
        for (std::size_t i = 0; i < missing.size() && i < 8; ++i) names += (i ? ", " : "") + missing[i];
        if (missing.size() > 8) names += " 외 " + std::to_string(missing.size() - 8) + "개";
        std::string text = "글꼴 " + std::to_string(missing.size()) + "개(" + names +
                           ")를 넣지 못해 대체 글꼴로 재고 그렸습니다. 줄 나눔과 쪽 그림이 한글과 다를 수 있어 사용자에게 알립니다. ";
        if (host.fetchFonts) {
            text += "글꼴 서버에는 글꼴 이름만 요청하고 문서 내용은 보내지 않습니다. 네트워크나 바깥 접속이 막혀 받지 못했다면 "
                    "사용자에게 그 사실을 그대로 설명하고 글꼴 받기를 허락받은 뒤 hwp_open 으로 문서를 다시 여세요.";
        } else {
            text += "이 서버(chwp-agent --mcp 를 바로 띄운 것)는 글꼴을 받지 않고 캐시에 있는 것만 씁니다. "
                    "글꼴까지 받으려면 chwp_agent.py --mcp 로 띄웁니다.";
        }
        return text;
    }

    std::string callAgent(std::string_view request, const Tool& tool, bool withFontNotice = false) {
        CallResult result = agentCall(*doc, request, host.outDir, host.readFile);
        std::vector<std::string> content;
        content.push_back(textContent(result.json));
        bool ok = result.ok;
        for (const CallFile& file : result.files) {
            const bool written = host.writeFile && host.writeFile(file.path, file.bytes);
            if (std::string_view(tool.call) == "render") {
                // 그림은 MCP 이미지로 준다. 파일을 못 썼어도 그림은 돌려준다.
                content.push_back(R"({"type":"image","mimeType":"image/png","data":)" +
                                  quote(crypto::base64Encode(ByteView(file.bytes.data(), file.bytes.size()))) + "}");
                continue;
            }
            if (!written) {
                Status status;
                status.code = Code::SaveFailed;
                status.message = "파일을 쓰지 못했습니다: " + file.path;
                content = {textContent(statusJson(status))};
                ok = false;
                break;
            }
        }
        const std::string_view call = tool.call;
        if (ok && (call == "apply" || call == "undo" || call == "redo")) loadFonts();
        if (ok && (withFontNotice || call == "apply")) {
            const std::string notice = fontNotice();
            if (!notice.empty()) content.push_back(textContent(notice));
        }
        return toolResult(content, !ok);
    }

    std::string open(const Json* args) {
        const Json* path = args ? args->get("path") : nullptr;
        Status status;
        status.code = Code::BadArgument;
        if (!path || path->type != Json::Type::String || path->text.empty()) {
            status.message = "path 를 주세요.";
            return toolResult({textContent(statusJson(status))}, true);
        }
        Bytes bytes;
        if (!host.readFile || !host.readFile(path->text, bytes)) {
            status.code = Code::ParseFailed;
            status.message = "문서 파일을 열지 못했습니다: " + path->text;
            return toolResult({textContent(statusJson(status))}, true);
        }
        doc.reset();  // 한 프로세스 한 문서: 앞 문서를 먼저 닫는다
        if (host.readFontCatalog) {
            std::string index, licenseSwap, replace;
            host.readFontCatalog(index, licenseSwap, replace);
            if (!index.empty()) setFontCatalog(index, licenseSwap, replace);
        }
        auto opened = AgentDocument::open(ByteView(bytes.data(), bytes.size()));
        if (!opened.ok()) return toolResult({textContent(statusJson(opened.status))}, true);
        doc = std::move(opened.value);
        loadFonts();
        static const Tool info = {"hwp_info", "info", Kind::Call, true, "", ""};
        return callAgent(R"({"call":"info"})", info, true);
    }

    std::string callTool(const Json& params) {
        const Json* name = params.get("name");
        const Json* args = params.get("arguments");
        const Tool* tool = name && name->type == Json::Type::String ? findTool(name->text) : nullptr;
        if (!tool) return {};
        if (args && args->type != Json::Type::Object) args = nullptr;
        if (tool->kind == Kind::Open) return open(args);
        if (tool->kind == Kind::Reference) {
            const Json* topic = args ? args->get("topic") : nullptr;
            return toolResult({textContent(referenceSection(topic && topic->type == Json::Type::String ? topic->text : ""))},
                              false);
        }
        if (!doc) {
            Status status;
            status.code = Code::BadArgument;
            status.message = "열린 문서가 없습니다. 먼저 hwp_open 으로 문서를 여세요.";
            return toolResult({textContent(statusJson(status))}, true);
        }
        std::string request = R"({"call":)" + quote(tool->call);
        if (args) {
            for (const auto& [key, value] : args->fields) {
                if (key == "call" || key == "id") continue;
                request += "," + quote(key) + ":" + serialize(value);
            }
        }
        request += "}";
        return callAgent(request, *tool);
    }
};

McpServer::McpServer(McpHost host) : impl_(std::make_unique<Impl>()) {
    impl_->host = std::move(host);
}

McpServer::~McpServer() = default;

std::string McpServer::handle(std::string_view message) {
    Json json;
    std::string error;
    auto rpcError = [](const std::string& id, int code, std::string_view text) {
        return R"({"jsonrpc":"2.0","id":)" + id + R"(,"error":{"code":)" + std::to_string(code) +
               R"(,"message":)" + quote(text) + "}}";
    };
    if (!JsonReader(message).read(json, error)) return rpcError("null", -32700, error);
    if (json.type != Json::Type::Object) return rpcError("null", -32600, "메시지는 JSON 객체여야 합니다.");
    const Json* id = json.get("id");
    const Json* method = json.get("method");
    // 알림(id 없음)과 클라이언트가 보낸 응답(method 없음)에는 답하지 않는다.
    if (!id || !method) return {};
    const std::string idText = serialize(*id);
    if (method->type != Json::Type::String) return rpcError(idText, -32600, "method 가 글이 아닙니다.");
    auto ok = [&](const std::string& result) { return R"({"jsonrpc":"2.0","id":)" + idText + R"(,"result":)" + result + "}"; };
    const std::string& name = method->text;
    const Json* params = json.get("params");
    if (name == "initialize") {
        std::string version = kVersions[1];
        if (const Json* asked = params ? params->get("protocolVersion") : nullptr) {
            for (const char* known : kVersions) {
                if (asked->type == Json::Type::String && asked->text == known) version = known;
            }
        }
        return ok(R"({"protocolVersion":)" + quote(version) +
                  R"(,"capabilities":{"tools":{"listChanged":false}},"serverInfo":{"name":"chwp-agent","version":)" +
                  quote(impl_->host.version.empty() ? "0" : impl_->host.version) + R"(},"instructions":)" +
                  quote(kInstructions) + "}");
    }
    if (name == "ping") return ok("{}");
    if (name == "tools/list") return ok(R"({"tools":)" + mcpToolsJson() + "}");
    if (name == "tools/call") {
        if (!params || params->type != Json::Type::Object) return rpcError(idText, -32602, "params 가 없습니다.");
        std::string result = impl_->callTool(*params);
        if (result.empty()) return rpcError(idText, -32602, "모르는 도구입니다.");
        return ok(result);
    }
    return rpcError(idText, -32601, "모르는 메서드입니다: " + name);
}

std::string McpServer::fontRequest(const std::vector<std::string>& paths) {
    impl_->pendingFontId = "chwp-fonts-" + std::to_string(++impl_->fontRequests);
    std::string list = "[";
    for (std::size_t i = 0; i < paths.size(); ++i) list += (i ? "," : "") + quote(paths[i]);
    return R"({"jsonrpc":"2.0","id":)" + quote(impl_->pendingFontId) + R"(,"method":"chwp/fetchFonts","params":{"paths":)" +
           list + "]}}";
}

bool McpServer::answersFontRequest(std::string_view line) const {
    Json json;
    std::string error;
    if (impl_->pendingFontId.empty() || !JsonReader(line).read(json, error) || json.type != Json::Type::Object) {
        return false;
    }
    const Json* id = json.get("id");
    return id && id->type == Json::Type::String && id->text == impl_->pendingFontId && !json.get("method");
}

}  // namespace chwp::agent
