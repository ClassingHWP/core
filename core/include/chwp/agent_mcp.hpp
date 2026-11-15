#pragma once

// MCP(Model Context Protocol) 서버. 에이전트 API(agentCall)를 MCP 도구(hwp_open, hwp_text, hwp_render, hwp_apply …)로 내놓는다.
//
// 판단은 모두 여기서 한다: JSON-RPC 메시지 해석, 도구 목록과 입력 스키마, 도구 호출을 agentCall 요청으로 바꾸기,
// 결과 포장(쪽 그림은 MCP 이미지), 글꼴 넣기 되풀이. 셸은 메시지 한 줄을 넘기고 돌려받은 줄을 쓰기만 한다.
// 셸이 대신하는 플랫폼 일은 McpHost 다(파일 읽기·쓰기, 글꼴 캐시 읽기, 글꼴 받기).
// stdio(한 줄에 메시지 하나)와 HTTP 셸이 같은 서버를 쓴다.
//
// 한 프로세스 한 문서: AgentDocument 제약을 따른다. hwp_open 으로 새 문서를 열면 앞 문서는 닫는다.

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "chwp/agent.hpp"

namespace chwp::agent {

struct McpHost {
    // 문서·그림 파일 읽기. 없으면 hwp_open 과 그림 넣기(file)를 할 수 없다.
    FileReader readFile;
    // 저장한 문서와 쪽 그림 쓰기. 쓰지 못하면 거짓.
    std::function<bool(const std::string& path, const Bytes& bytes)> writeFile;
    // 글꼴 캐시에서 파일 하나 읽기(글꼴 서버와 같은 "폴더/파일" 경로). 없으면 거짓.
    std::function<bool(const std::string& path, Bytes& out)> readFont;
    // 캐시에 없는 글꼴을 받아 캐시에 넣는다. 다 받을 때까지 기다린다. 없으면 캐시에 있는 글꼴만 쓴다.
    std::function<void(const std::vector<std::string>& paths)> fetchFonts;
    // 글꼴 목록(글꼴 서버의 index.json, license-swap.json, replace.json) 읽기. 문서를 열 때마다 부른다.
    // 목록이 뒤에 들어올 수 있어서다(바깥 접속을 허락받은 뒤 다시 열 때). index 가 비면 목록 없이 쓴다.
    std::function<void(std::string& index, std::string& licenseSwap, std::string& replace)> readFontCatalog;
    // 쪽 그림을 둘 폴더. 그림은 MCP 이미지로도 돌려준다.
    std::string outDir = ".";
    std::string version;
};

class McpServer {
public:
    explicit McpServer(McpHost host);
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    // JSON-RPC 메시지 하나를 처리해 응답 한 줄을 돌려준다. 알림과 클라이언트의 응답에는 빈 글을 돌려준다.
    std::string handle(std::string_view message);

    // 글꼴을 받아 달라고 셸 바깥(감싼 스크립트)에 보내는 요청 줄. 셸은 이 줄을 쓰고, answers 가 참인 줄이 올 때까지
    // 들어오는 다른 줄을 모아 두었다가 뒤에 handle 로 넘긴다. MCP 표준 메서드가 아니므로 감싼 스크립트가 있을 때만 쓴다.
    std::string fontRequest(const std::vector<std::string>& paths);
    bool answersFontRequest(std::string_view line) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// MCP 도구 목록(tools/list 의 tools 배열 JSON). 시험과 문서용.
std::string mcpToolsJson();

}  // namespace chwp::agent
