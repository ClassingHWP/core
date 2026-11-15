# chwp-agent MCP 서버

`chwp-agent` 는 MCP(Model Context Protocol) 서버로도 돈다. MCP 를 받는 에이전트(Claude Code·Claude Desktop, Gemini CLI,
Antigravity, Codex 등)가 한글 문서(HWP·HWPX)를 도구로 읽고, 찾고, 고치고, 쪽 그림으로 확인하고, HWP·HWPX·PDF 로 저장한다.
지금은 사용자 PC 에서 띄우는 stdio 서버다.

## 띄우는 법

글꼴까지 받아 주는 감싼 스크립트로 띄운다(Python 3 표준 라이브러리만 쓴다).

```
python3 <스킬 폴더>/scripts/chwp_agent.py --mcp
```

- 실행 파일은 `chwp_agent.py` 가 찾는다(`CHWP_AGENT_BIN` → 스킬의 `bin/` → 저장소 빌드 → 글만 든 스킬이면 GitHub 릴리스에서 받기).
- 문서에 필요한 글꼴은 서버가 달라고 할 때 스크립트가 글꼴 서버에서 받아 캐시(`~/.cache/chwp-agent/fonts`)에 넣는다.
- 실행 파일을 바로 띄울 수도 있다: `chwp-agent --mcp [--font-dir 글꼴캐시]`. 이때는 글꼴을 받지 않고 캐시에 있는 것만 쓴다.
- 글꼴 서버에는 글꼴 이름만 요청하고 문서 내용은 보내지 않는다. 글꼴을 받지 못하면 `hwp_open`·`hwp_apply` 결과에
  받지 못한 글꼴과 그 뜻을 적은 안내가 붙는다. 바깥 접속이 막힌 환경이면 사용자에게 허락받은 뒤 `hwp_open` 으로 다시 열면 받는다.
- 실행 파일 옆 `fonts/` 의 글꼴(스킬 ZIP 의 `bin/fonts/HANBatang.woff2`, 함초롬바탕)을 시작할 때 넣는다. 글꼴을 하나도 못 받아도
  구할 수 없는 글꼴을 함초롬바탕으로 그려 글자가 보인다.
- 쪽 그림은 MCP 이미지로 돌려주고, 같은 PNG 를 임시 폴더(`chwp-mcp`, 바꾸려면 `--out-dir`)에도 쓴다.

## 클라이언트 설정 예

경로는 스킬을 둔 자리로 바꾼다. Windows 에서는 `python3` 대신 `python` 이다.

Claude Code:

```
claude mcp add chwp -- python3 /path/to/chwp-agent/scripts/chwp_agent.py --mcp
```

Claude Desktop, Gemini CLI(`~/.gemini/settings.json`) 처럼 `mcpServers` 를 쓰는 설정:

```json
{
  "mcpServers": {
    "chwp": {
      "command": "python3",
      "args": ["/path/to/chwp-agent/scripts/chwp_agent.py", "--mcp"]
    }
  }
}
```

Codex(`~/.codex/config.toml`):

```toml
[mcp_servers.chwp]
command = "python3"
args = ["/path/to/chwp-agent/scripts/chwp_agent.py", "--mcp"]
```

## 도구

| 도구 | 하는 일 |
|---|---|
| `hwp_open` | 문서를 연다. 먼저 부른다. 한 번에 한 문서이고, 새로 열면 앞 문서는 닫힌다 |
| `hwp_info`, `hwp_pages`, `hwp_stories`, `hwp_section` | 문서 정보, 쪽 목록, 글 묶음 목록, 구역 편집 용지 |
| `hwp_text`, `hwp_paragraph`, `hwp_table`, `hwp_find` | 글 읽기, 문단 하나, 표 하나, 찾기 |
| `hwp_layout`, `hwp_render` | 쪽 배치(줄·상자 좌표), 쪽 그림(PNG 이미지) |
| `hwp_apply`, `hwp_view`, `hwp_undo`, `hwp_redo` | 고치기(op 목록), 편집기 명령 값 읽기, 되돌리기·다시 하기 |
| `hwp_save` | 저장(.hwp, .hwpx, .pdf) |
| `hwp_reference` | op·요청 모양 설명서(`skills/chwp-agent/reference.md` 의 절) |

도구 인자는 [reference.md](../../skills/chwp-agent/reference.md) 의 요청 필드와 같다(`call` 만 도구 이름이 대신한다).
판단은 모두 core(`chwp::agent::McpServer`, `core/src/agent/mcp_server.cpp`)가 하고, 이 실행 파일과 스크립트는 줄을 잇고
파일·글꼴을 읽고 쓰기만 한다.

## 확인

```
python tools/chwp-agent/mcp_smoke.py <문서.hwp|.hwpx>
python tools/chwp-agent/mcp_smoke.py <문서> python skills/chwp-agent/scripts/chwp_agent.py --mcp
```

초기화·도구 목록·열기·읽기·그림·고치기·되돌리기·저장·설명서·오류 처리를 차례로 부르고 결과를 한 줄씩 쓴다.
