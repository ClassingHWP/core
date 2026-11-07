// chwp-agent: 에이전트 API(chwp::agent::agentCall)를 감싸는 CLI. Agent Skill 이 부른다.
//
//   chwp-agent <문서.hwp|.hwpx> [요청.jsonl | -] [-c JSON]... [--save 경로] [--out-dir 폴더]
//              [--font-dir 글꼴캐시] [--font-plan] [--verbose]
//
// 요청은 한 줄에 JSON 하나(JSON Lines)다. 파일, 표준 입력(-), -c 순서로 같은 세션에서 차례로 부른다.
// --save 는 마지막에 {"call":"save","path":경로} 를 하나 더 부른다. 응답도 한 줄에 하나씩 표준 출력에 쓴다.
// --font-dir 는 글꼴 서버와 같은 모양의 캐시 폴더다(index.json, license-swap.json, replace.json, 폴더/파일).
// --font-plan 은 요청을 부르지 않고, 캐시에 아직 없어 받아야 할 파일을 {"ok":true,"need":[경로...]} 한 줄로 쓴다.
// --verbose 는 core 진단 기록(글꼴 받기·대체 등)을 stderr 로 낸다. 주지 않으면 버린다.
// 이 프로그램은 파일을 읽고 쓰기만 한다. 무엇을 받을지, 무엇을 할지는 모두 core 가 정한다.
//
// 끝 코드: 0 모두 성공, 1 실패한 요청이 있음, 2 사용법이 틀렸거나 문서를 열지 못함.

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "chwp/agent.hpp"

namespace {

std::filesystem::path pathOf(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

bool readFile(const std::string& path, chwp::Bytes& out) {
    std::ifstream file(pathOf(path), std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), {});
    return true;
}

bool writeFile(const std::string& path, const chwp::Bytes& bytes) {
    // 경로를 바꾸지 못하면(바르지 않은 글) 실행을 멈추지 않고 쓰기 실패로 답한다.
    try {
        const std::filesystem::path target = pathOf(path);
        std::error_code error;
        if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), error);
        std::ofstream file(target, std::ios::binary | std::ios::trunc);
        if (!file) return false;
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(file);
    } catch (const std::exception&) {
        return false;
    }
}

void readLines(std::istream& in, std::vector<std::string>& out) {
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        out.push_back(line);
    }
}

// core 의 진단 기록(글꼴 시간 등)은 stdout 에 찍힌다. 응답 줄만 stdout 에 남도록 기록은 stderr(verbose)나
// 버리는 곳으로 돌리고, 응답은 원래 stdout 을 복제한 흐름에 쓴다.
FILE* takeStdoutForReplies(bool verbose) {
    std::fflush(stdout);
#ifdef _WIN32
    const int reply = _dup(_fileno(stdout));
    const int sink = verbose ? _dup(_fileno(stderr)) : _open("NUL", _O_WRONLY);
    if (sink >= 0) {
        _dup2(sink, _fileno(stdout));
        _close(sink);
    }
    _setmode(reply, _O_BINARY);
    return _fdopen(reply, "wb");
#else
    const int reply = dup(fileno(stdout));
    const int sink = verbose ? dup(fileno(stderr)) : open("/dev/null", O_WRONLY);
    if (sink >= 0) {
        dup2(sink, fileno(stdout));
        close(sink);
    }
    return fdopen(reply, "w");
#endif
}

void reply(FILE* out, const std::string& line) {
    std::fputs(line.c_str(), out);
    std::fputc('\n', out);
    std::fflush(out);
}

std::string readText(const std::string& path) {
    chwp::Bytes bytes;
    return readFile(path, bytes) ? std::string(bytes.begin(), bytes.end()) : std::string();
}

std::string jsonText(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

// 캐시에 있는 글꼴은 넘기고, 없는 글꼴은 못 받은 것으로 돌려준다. 무엇이 필요한지는 core(fontPlan)가 정한다.
// 받지 못했다는 표시(파일.missing)도 없이 캐시에 없는 파일 경로를 돌려준다. 셸 스크립트가 받아 올 목록이다.
std::vector<std::string> loadFonts(chwp::agent::AgentDocument& doc, const std::string& dir) {
    std::vector<std::string> absent;
    // 받은 글꼴에 없는 글자의 대체 글꼴을 core 가 더 달라고 할 수 있다. 몇 번 되물을지는 core 가 세고,
    // 한 번 낸 파일은 다시 내지 않으므로 빈 목록이 나오면 끝이다.
    for (;;) {
        const std::vector<chwp::agent::FontFile> plan = doc.fontPlan();
        if (plan.empty()) break;
        for (const chwp::agent::FontFile& file : plan) {
            chwp::Bytes bytes;
            const std::string path = dir + "/" + file.path;
            if (readFile(path, bytes)) {
                doc.fontArrived(file.id, std::move(bytes));
                continue;
            }
            std::error_code error;
            if (!std::filesystem::exists(pathOf(path + ".missing"), error)) absent.push_back(file.path);
            doc.fontMissing(file.id);
        }
        doc.fontsDone();
    }
    return absent;
}

int usage() {
    std::cerr << "사용법: chwp-agent <문서.hwp|.hwpx> [요청.jsonl | -] [-c JSON]... [--save 경로] [--out-dir 폴더]\n"
                 "                  [--font-dir 글꼴캐시] [--font-plan] [--verbose]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const bool verbose =
        std::any_of(argv + 1, argv + argc, [](const char* arg) { return std::string(arg) == "--verbose"; });
    FILE* const out = takeStdoutForReplies(verbose);
    if (!out) return 2;
    std::string document;
    std::string outDir = ".";
    std::vector<std::string> requests;
    std::vector<std::string> inline_;
    std::string save;
    std::string fontDir;
    bool fontPlanOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "-c" || arg == "--call") && i + 1 < argc) {
            inline_.push_back(argv[++i]);
        } else if (arg == "--font-dir" && i + 1 < argc) {
            fontDir = argv[++i];
        } else if (arg == "--font-plan") {
            fontPlanOnly = true;
        } else if (arg == "--tex-res" && i + 1 < argc) {
            chwp::agent::setEquationResources(argv[++i]);
        } else if (arg == "--save" && i + 1 < argc) {
            save = argv[++i];
        } else if (arg == "--out-dir" && i + 1 < argc) {
            outDir = argv[++i];
        } else if (document.empty() && arg != "-") {
            document = arg;
        } else if (arg == "-") {
            readLines(std::cin, requests);
        } else {
            std::ifstream file(pathOf(arg), std::ios::binary);
            if (!file) {
                std::cerr << "요청 파일을 열지 못했습니다: " << arg << "\n";
                return 2;
            }
            readLines(file, requests);
        }
    }
    if (document.empty()) return usage();
    requests.insert(requests.end(), inline_.begin(), inline_.end());

    if (!fontDir.empty()) {
        const std::string index = readText(fontDir + "/index.json");
        if (!index.empty()) {
            chwp::agent::setFontCatalog(index, readText(fontDir + "/license-swap.json"),
                                        readText(fontDir + "/replace.json"));
        }
    }

    chwp::Bytes bytes;
    if (!readFile(document, bytes)) {
        chwp::agent::Status status;
        status.code = chwp::agent::Code::ParseFailed;
        status.message = "문서 파일을 열지 못했습니다: " + document;
        reply(out, chwp::agent::statusJson(status));
        return 2;
    }
    auto opened = chwp::agent::AgentDocument::open(bytes);
    if (!opened.ok()) {
        reply(out, chwp::agent::statusJson(opened.status));
        return 2;
    }
    chwp::agent::AgentDocument& doc = *opened.value;

    std::vector<std::string> need;
    if (!fontDir.empty()) {
        need = loadFonts(doc, fontDir);
    } else if (fontPlanOnly) {
        return usage();
    }

    if (!save.empty()) {
        // 경로의 따옴표·역빗금을 JSON 글로 옮긴다.
        std::string escaped;
        for (const char c : save) {
            if (c == '"' || c == '\\') escaped += '\\';
            escaped += c;
        }
        requests.push_back("{\"call\":\"save\",\"path\":\"" + escaped + "\"}");
    }

    int failed = 0;
    for (const std::string& request : requests) {
        chwp::agent::CallResult result = chwp::agent::agentCall(doc, request, outDir);
        // 편집이 새 글꼴을 쓰게 했을 수 있다. 무엇이 필요한지는 core(fontPlan)가 정한다.
        if (!fontDir.empty()) {
            for (std::string& path : loadFonts(doc, fontDir)) need.push_back(std::move(path));
        }
        // 글꼴만 묻는 실행(--font-plan)은 요청을 끝까지 해 보고 필요한 글꼴만 답한다. 파일은 쓰지 않는다.
        if (fontPlanOnly) continue;
        for (const chwp::agent::CallFile& file : result.files) {
            if (!writeFile(file.path, file.bytes)) {
                chwp::agent::Status status;
                status.code = chwp::agent::Code::SaveFailed;
                status.message = "파일을 쓰지 못했습니다: " + file.path;
                result.ok = false;
                result.json = chwp::agent::statusJson(status);
                break;
            }
        }
        if (!result.ok) ++failed;
        reply(out, result.json);
    }
    if (fontPlanOnly) {
        std::sort(need.begin(), need.end());
        need.erase(std::unique(need.begin(), need.end()), need.end());
        std::string line = "{\"ok\":true,\"need\":[";
        for (std::size_t i = 0; i < need.size(); ++i) line += (i ? "," : "") + jsonText(need[i]);
        reply(out, line + "]}");
        return 0;
    }
    return failed > 0 ? 1 : 0;
}
